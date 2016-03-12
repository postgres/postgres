/*
 * interface to SPI functions
 *
 * src/pl/plpython/plpy_spi.c
 */

#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "parser/parse_type.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "plpython.h"

#include "plpy_spi.h"

#include "plpy_elog.h"
#include "plpy_main.h"
#include "plpy_planobject.h"
#include "plpy_plpymodule.h"
#include "plpy_procedure.h"
#include "plpy_resultobject.h"


static PyObject *PLy_spi_execute_query(char *query, long limit);
static PyObject *PLy_spi_execute_plan(PyObject *ob, PyObject *list, long limit);
static PyObject *PLy_spi_execute_fetch_result(SPITupleTable *tuptable,
							 uint64 rows, int status);
static void PLy_spi_exception_set(PyObject *excclass, ErrorData *edata);


/* prepare(query="select * from foo")
 * prepare(query="select * from foo where bar = $1", params=["text"])
 * prepare(query="select * from foo where bar = $1", params=["text"], limit=5)
 */
PyObject *
PLy_spi_prepare(PyObject *self, PyObject *args)
{
	PLyPlanObject *plan;
	PyObject   *list = NULL;
	PyObject   *volatile optr = NULL;
	char	   *query;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	volatile int nargs;

	if (!PyArg_ParseTuple(args, "s|O", &query, &list))
		return NULL;

	if (list && (!PySequence_Check(list)))
	{
		PLy_exception_set(PyExc_TypeError,
					   "second argument of plpy.prepare must be a sequence");
		return NULL;
	}

	if ((plan = (PLyPlanObject *) PLy_plan_new()) == NULL)
		return NULL;

	plan->mcxt = AllocSetContextCreate(TopMemoryContext,
									   "PL/Python plan context",
									   ALLOCSET_DEFAULT_MINSIZE,
									   ALLOCSET_DEFAULT_INITSIZE,
									   ALLOCSET_DEFAULT_MAXSIZE);
	oldcontext = MemoryContextSwitchTo(plan->mcxt);

	nargs = list ? PySequence_Length(list) : 0;

	plan->nargs = nargs;
	plan->types = nargs ? palloc(sizeof(Oid) * nargs) : NULL;
	plan->values = nargs ? palloc(sizeof(Datum) * nargs) : NULL;
	plan->args = nargs ? palloc(sizeof(PLyTypeInfo) * nargs) : NULL;

	MemoryContextSwitchTo(oldcontext);

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		int			i;
		PLyExecutionContext *exec_ctx = PLy_current_execution_context();

		/*
		 * the other loop might throw an exception, if PLyTypeInfo member
		 * isn't properly initialized the Py_DECREF(plan) will go boom
		 */
		for (i = 0; i < nargs; i++)
		{
			PLy_typeinfo_init(&plan->args[i], plan->mcxt);
			plan->values[i] = PointerGetDatum(NULL);
		}

		for (i = 0; i < nargs; i++)
		{
			char	   *sptr;
			HeapTuple	typeTup;
			Oid			typeId;
			int32		typmod;

			optr = PySequence_GetItem(list, i);
			if (PyString_Check(optr))
				sptr = PyString_AsString(optr);
			else if (PyUnicode_Check(optr))
				sptr = PLyUnicode_AsString(optr);
			else
			{
				ereport(ERROR,
						(errmsg("plpy.prepare: type name at ordinal position %d is not a string", i)));
				sptr = NULL;	/* keep compiler quiet */
			}

			/********************************************************
			 * Resolve argument type names and then look them up by
			 * oid in the system cache, and remember the required
			 *information for input conversion.
			 ********************************************************/

			parseTypeString(sptr, &typeId, &typmod, false);

			typeTup = SearchSysCache1(TYPEOID,
									  ObjectIdGetDatum(typeId));
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "cache lookup failed for type %u", typeId);

			Py_DECREF(optr);

			/*
			 * set optr to NULL, so we won't try to unref it again in case of
			 * an error
			 */
			optr = NULL;

			plan->types[i] = typeId;
			PLy_output_datum_func(&plan->args[i], typeTup, exec_ctx->curr_proc->langid, exec_ctx->curr_proc->trftypes);
			ReleaseSysCache(typeTup);
		}

		pg_verifymbstr(query, strlen(query), false);
		plan->plan = SPI_prepare(query, plan->nargs, plan->types);
		if (plan->plan == NULL)
			elog(ERROR, "SPI_prepare failed: %s",
				 SPI_result_code_string(SPI_result));

		/* transfer plan from procCxt to topCxt */
		if (SPI_keepplan(plan->plan))
			elog(ERROR, "SPI_keepplan failed");

		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		Py_DECREF(plan);
		Py_XDECREF(optr);

		PLy_spi_subtransaction_abort(oldcontext, oldowner);
		return NULL;
	}
	PG_END_TRY();

	Assert(plan->plan != NULL);
	return (PyObject *) plan;
}

/* execute(query="select * from foo", limit=5)
 * execute(plan=plan, values=(foo, bar), limit=5)
 */
PyObject *
PLy_spi_execute(PyObject *self, PyObject *args)
{
	char	   *query;
	PyObject   *plan;
	PyObject   *list = NULL;
	long		limit = 0;

	if (PyArg_ParseTuple(args, "s|l", &query, &limit))
		return PLy_spi_execute_query(query, limit);

	PyErr_Clear();

	if (PyArg_ParseTuple(args, "O|Ol", &plan, &list, &limit) &&
		is_PLyPlanObject(plan))
		return PLy_spi_execute_plan(plan, list, limit);

	PLy_exception_set(PLy_exc_error, "plpy.execute expected a query or a plan");
	return NULL;
}

static PyObject *
PLy_spi_execute_plan(PyObject *ob, PyObject *list, long limit)
{
	volatile int nargs;
	int			i,
				rv;
	PLyPlanObject *plan;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	PyObject   *ret;

	if (list != NULL)
	{
		if (!PySequence_Check(list) || PyString_Check(list) || PyUnicode_Check(list))
		{
			PLy_exception_set(PyExc_TypeError, "plpy.execute takes a sequence as its second argument");
			return NULL;
		}
		nargs = PySequence_Length(list);
	}
	else
		nargs = 0;

	plan = (PLyPlanObject *) ob;

	if (nargs != plan->nargs)
	{
		char	   *sv;
		PyObject   *so = PyObject_Str(list);

		if (!so)
			PLy_elog(ERROR, "could not execute plan");
		sv = PyString_AsString(so);
		PLy_exception_set_plural(PyExc_TypeError,
							  "Expected sequence of %d argument, got %d: %s",
							 "Expected sequence of %d arguments, got %d: %s",
								 plan->nargs,
								 plan->nargs, nargs, sv);
		Py_DECREF(so);

		return NULL;
	}

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		PLyExecutionContext *exec_ctx = PLy_current_execution_context();
		char	   *volatile nulls;
		volatile int j;

		if (nargs > 0)
			nulls = palloc(nargs * sizeof(char));
		else
			nulls = NULL;

		for (j = 0; j < nargs; j++)
		{
			PyObject   *elem;

			elem = PySequence_GetItem(list, j);
			if (elem != Py_None)
			{
				PG_TRY();
				{
					plan->values[j] =
						plan->args[j].out.d.func(&(plan->args[j].out.d),
												 -1,
												 elem);
				}
				PG_CATCH();
				{
					Py_DECREF(elem);
					PG_RE_THROW();
				}
				PG_END_TRY();

				Py_DECREF(elem);
				nulls[j] = ' ';
			}
			else
			{
				Py_DECREF(elem);
				plan->values[j] =
					InputFunctionCall(&(plan->args[j].out.d.typfunc),
									  NULL,
									  plan->args[j].out.d.typioparam,
									  -1);
				nulls[j] = 'n';
			}
		}

		rv = SPI_execute_plan(plan->plan, plan->values, nulls,
							  exec_ctx->curr_proc->fn_readonly, limit);
		ret = PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);

		if (nargs > 0)
			pfree(nulls);

		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		int			k;

		/*
		 * cleanup plan->values array
		 */
		for (k = 0; k < nargs; k++)
		{
			if (!plan->args[k].out.d.typbyval &&
				(plan->values[k] != PointerGetDatum(NULL)))
			{
				pfree(DatumGetPointer(plan->values[k]));
				plan->values[k] = PointerGetDatum(NULL);
			}
		}

		PLy_spi_subtransaction_abort(oldcontext, oldowner);
		return NULL;
	}
	PG_END_TRY();

	for (i = 0; i < nargs; i++)
	{
		if (!plan->args[i].out.d.typbyval &&
			(plan->values[i] != PointerGetDatum(NULL)))
		{
			pfree(DatumGetPointer(plan->values[i]));
			plan->values[i] = PointerGetDatum(NULL);
		}
	}

	if (rv < 0)
	{
		PLy_exception_set(PLy_exc_spi_error,
						  "SPI_execute_plan failed: %s",
						  SPI_result_code_string(rv));
		return NULL;
	}

	return ret;
}

static PyObject *
PLy_spi_execute_query(char *query, long limit)
{
	int			rv;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	PyObject   *ret = NULL;

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		PLyExecutionContext *exec_ctx = PLy_current_execution_context();

		pg_verifymbstr(query, strlen(query), false);
		rv = SPI_execute(query, exec_ctx->curr_proc->fn_readonly, limit);
		ret = PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);

		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		PLy_spi_subtransaction_abort(oldcontext, oldowner);
		return NULL;
	}
	PG_END_TRY();

	if (rv < 0)
	{
		Py_XDECREF(ret);
		PLy_exception_set(PLy_exc_spi_error,
						  "SPI_execute failed: %s",
						  SPI_result_code_string(rv));
		return NULL;
	}

	return ret;
}

static PyObject *
PLy_spi_execute_fetch_result(SPITupleTable *tuptable, uint64 rows, int status)
{
	PLyResultObject *result;
	volatile MemoryContext oldcontext;

	result = (PLyResultObject *) PLy_result_new();
	Py_DECREF(result->status);
	result->status = PyInt_FromLong(status);

	if (status > 0 && tuptable == NULL)
	{
		Py_DECREF(result->nrows);
		result->nrows = (rows > (uint64) LONG_MAX) ?
			PyFloat_FromDouble((double) rows) :
			PyInt_FromLong((long) rows);
	}
	else if (status > 0 && tuptable != NULL)
	{
		PLyTypeInfo args;
		MemoryContext cxt;

		Py_DECREF(result->nrows);
		result->nrows = (rows > (uint64) LONG_MAX) ?
			PyFloat_FromDouble((double) rows) :
			PyInt_FromLong((long) rows);

		cxt = AllocSetContextCreate(CurrentMemoryContext,
									"PL/Python temp context",
									ALLOCSET_DEFAULT_MINSIZE,
									ALLOCSET_DEFAULT_INITSIZE,
									ALLOCSET_DEFAULT_MAXSIZE);
		PLy_typeinfo_init(&args, cxt);

		oldcontext = CurrentMemoryContext;
		PG_TRY();
		{
			MemoryContext oldcontext2;

			if (rows)
			{
				uint64		i;

				/*
				 * PyList_New() and PyList_SetItem() use Py_ssize_t for list
				 * size and list indices; so we cannot support a result larger
				 * than PY_SSIZE_T_MAX.
				 */
				if (rows > (uint64) PY_SSIZE_T_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("query result has too many rows to fit in a Python list")));

				Py_DECREF(result->rows);
				result->rows = PyList_New(rows);

				PLy_input_tuple_funcs(&args, tuptable->tupdesc);
				for (i = 0; i < rows; i++)
				{
					PyObject   *row = PLyDict_FromTuple(&args,
														tuptable->vals[i],
														tuptable->tupdesc);

					PyList_SetItem(result->rows, i, row);
				}
			}

			/*
			 * Save tuple descriptor for later use by result set metadata
			 * functions.  Save it in TopMemoryContext so that it survives
			 * outside of an SPI context.  We trust that PLy_result_dealloc()
			 * will clean it up when the time is right.  (Do this as late as
			 * possible, to minimize the number of ways the tupdesc could get
			 * leaked due to errors.)
			 */
			oldcontext2 = MemoryContextSwitchTo(TopMemoryContext);
			result->tupdesc = CreateTupleDescCopy(tuptable->tupdesc);
			MemoryContextSwitchTo(oldcontext2);
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(oldcontext);
			MemoryContextDelete(cxt);
			Py_DECREF(result);
			PG_RE_THROW();
		}
		PG_END_TRY();

		MemoryContextDelete(cxt);
		SPI_freetuptable(tuptable);
	}

	return (PyObject *) result;
}

/*
 * Utilities for running SPI functions in subtransactions.
 *
 * Usage:
 *
 *	MemoryContext oldcontext = CurrentMemoryContext;
 *	ResourceOwner oldowner = CurrentResourceOwner;
 *
 *	PLy_spi_subtransaction_begin(oldcontext, oldowner);
 *	PG_TRY();
 *	{
 *		<call SPI functions>
 *		PLy_spi_subtransaction_commit(oldcontext, oldowner);
 *	}
 *	PG_CATCH();
 *	{
 *		<do cleanup>
 *		PLy_spi_subtransaction_abort(oldcontext, oldowner);
 *		return NULL;
 *	}
 *	PG_END_TRY();
 *
 * These utilities take care of restoring connection to the SPI manager and
 * setting a Python exception in case of an abort.
 */
void
PLy_spi_subtransaction_begin(MemoryContext oldcontext, ResourceOwner oldowner)
{
	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);
}

void
PLy_spi_subtransaction_commit(MemoryContext oldcontext, ResourceOwner oldowner)
{
	/* Commit the inner transaction, return to outer xact context */
	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = oldowner;

	/*
	 * AtEOSubXact_SPI() should not have popped any SPI context, but just in
	 * case it did, make sure we remain connected.
	 */
	SPI_restore_connection();
}

void
PLy_spi_subtransaction_abort(MemoryContext oldcontext, ResourceOwner oldowner)
{
	ErrorData  *edata;
	PLyExceptionEntry *entry;
	PyObject   *exc;

	/* Save error info */
	MemoryContextSwitchTo(oldcontext);
	edata = CopyErrorData();
	FlushErrorState();

	/* Abort the inner transaction */
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = oldowner;

	/*
	 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it will
	 * have left us in a disconnected state.  We need this hack to return to
	 * connected state.
	 */
	SPI_restore_connection();

	/* Look up the correct exception */
	entry = hash_search(PLy_spi_exceptions, &(edata->sqlerrcode),
						HASH_FIND, NULL);
	/* We really should find it, but just in case have a fallback */
	Assert(entry != NULL);
	exc = entry ? entry->exc : PLy_exc_spi_error;
	/* Make Python raise the exception */
	PLy_spi_exception_set(exc, edata);
	FreeErrorData(edata);
}

/*
 * Raise a SPIError, passing in it more error details, like the
 * internal query and error position.
 */
static void
PLy_spi_exception_set(PyObject *excclass, ErrorData *edata)
{
	PyObject   *args = NULL;
	PyObject   *spierror = NULL;
	PyObject   *spidata = NULL;

	args = Py_BuildValue("(s)", edata->message);
	if (!args)
		goto failure;

	/* create a new SPI exception with the error message as the parameter */
	spierror = PyObject_CallObject(excclass, args);
	if (!spierror)
		goto failure;

	spidata = Py_BuildValue("(izzzi)", edata->sqlerrcode, edata->detail, edata->hint,
							edata->internalquery, edata->internalpos);
	if (!spidata)
		goto failure;

	if (PyObject_SetAttrString(spierror, "spidata", spidata) == -1)
		goto failure;

	PyErr_SetObject(excclass, spierror);

	Py_DECREF(args);
	Py_DECREF(spierror);
	Py_DECREF(spidata);
	return;

failure:
	Py_XDECREF(args);
	Py_XDECREF(spierror);
	Py_XDECREF(spidata);
	elog(ERROR, "could not convert SPI error to Python exception");
}
