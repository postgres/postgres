/*
 * the PLyCursor class
 *
 * src/pl/plpython/plpy_cursorobject.c
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "mb/pg_wchar.h"
#include "utils/memutils.h"

#include "plpython.h"

#include "plpy_cursorobject.h"

#include "plpy_elog.h"
#include "plpy_main.h"
#include "plpy_planobject.h"
#include "plpy_procedure.h"
#include "plpy_resultobject.h"
#include "plpy_spi.h"


static PyObject *PLy_cursor_query(const char *query);
static PyObject *PLy_cursor_plan(PyObject *ob, PyObject *args);
static void PLy_cursor_dealloc(PyObject *arg);
static PyObject *PLy_cursor_iternext(PyObject *self);
static PyObject *PLy_cursor_fetch(PyObject *self, PyObject *args);
static PyObject *PLy_cursor_close(PyObject *self, PyObject *unused);

static char PLy_cursor_doc[] = {
	"Wrapper around a PostgreSQL cursor"
};

static PyMethodDef PLy_cursor_methods[] = {
	{"fetch", PLy_cursor_fetch, METH_VARARGS, NULL},
	{"close", PLy_cursor_close, METH_NOARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject PLy_CursorType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"PLyCursor",				/* tp_name */
	sizeof(PLyCursorObject),	/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_cursor_dealloc,			/* tp_dealloc */
	0,							/* tp_print */
	0,							/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
	0,							/* tp_call */
	0,							/* tp_str */
	0,							/* tp_getattro */
	0,							/* tp_setattro */
	0,							/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_ITER,	/* tp_flags */
	PLy_cursor_doc,				/* tp_doc */
	0,							/* tp_traverse */
	0,							/* tp_clear */
	0,							/* tp_richcompare */
	0,							/* tp_weaklistoffset */
	PyObject_SelfIter,			/* tp_iter */
	PLy_cursor_iternext,		/* tp_iternext */
	PLy_cursor_methods,			/* tp_tpmethods */
};

void
PLy_cursor_init_type(void)
{
	if (PyType_Ready(&PLy_CursorType) < 0)
		elog(ERROR, "could not initialize PLy_CursorType");
}

PyObject *
PLy_cursor(PyObject *self, PyObject *args)
{
	char	   *query;
	PyObject   *plan;
	PyObject   *planargs = NULL;

	if (PyArg_ParseTuple(args, "s", &query))
		return PLy_cursor_query(query);

	PyErr_Clear();

	if (PyArg_ParseTuple(args, "O|O", &plan, &planargs))
		return PLy_cursor_plan(plan, planargs);

	PLy_exception_set(PLy_exc_error, "plpy.cursor expected a query or a plan");
	return NULL;
}


static PyObject *
PLy_cursor_query(const char *query)
{
	PLyCursorObject *cursor;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;

	if ((cursor = PyObject_New(PLyCursorObject, &PLy_CursorType)) == NULL)
		return NULL;
	cursor->portalname = NULL;
	cursor->closed = false;
	cursor->mcxt = AllocSetContextCreate(TopMemoryContext,
										 "PL/Python cursor context",
										 ALLOCSET_DEFAULT_SIZES);
	PLy_typeinfo_init(&cursor->result, cursor->mcxt);

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		PLyExecutionContext *exec_ctx = PLy_current_execution_context();
		SPIPlanPtr	plan;
		Portal		portal;

		pg_verifymbstr(query, strlen(query), false);

		plan = SPI_prepare(query, 0, NULL);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare failed: %s",
				 SPI_result_code_string(SPI_result));

		portal = SPI_cursor_open(NULL, plan, NULL, NULL,
								 exec_ctx->curr_proc->fn_readonly);
		SPI_freeplan(plan);

		if (portal == NULL)
			elog(ERROR, "SPI_cursor_open() failed: %s",
				 SPI_result_code_string(SPI_result));

		cursor->portalname = MemoryContextStrdup(cursor->mcxt, portal->name);

		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		PLy_spi_subtransaction_abort(oldcontext, oldowner);
		return NULL;
	}
	PG_END_TRY();

	Assert(cursor->portalname != NULL);
	return (PyObject *) cursor;
}

static PyObject *
PLy_cursor_plan(PyObject *ob, PyObject *args)
{
	PLyCursorObject *cursor;
	volatile int nargs;
	int			i;
	PLyPlanObject *plan;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;

	if (args)
	{
		if (!PySequence_Check(args) || PyString_Check(args) || PyUnicode_Check(args))
		{
			PLy_exception_set(PyExc_TypeError, "plpy.cursor takes a sequence as its second argument");
			return NULL;
		}
		nargs = PySequence_Length(args);
	}
	else
		nargs = 0;

	plan = (PLyPlanObject *) ob;

	if (nargs != plan->nargs)
	{
		char	   *sv;
		PyObject   *so = PyObject_Str(args);

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

	if ((cursor = PyObject_New(PLyCursorObject, &PLy_CursorType)) == NULL)
		return NULL;
	cursor->portalname = NULL;
	cursor->closed = false;
	cursor->mcxt = AllocSetContextCreate(TopMemoryContext,
										 "PL/Python cursor context",
										 ALLOCSET_DEFAULT_SIZES);
	PLy_typeinfo_init(&cursor->result, cursor->mcxt);

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		PLyExecutionContext *exec_ctx = PLy_current_execution_context();
		Portal		portal;
		char	   *volatile nulls;
		volatile int j;

		if (nargs > 0)
			nulls = palloc(nargs * sizeof(char));
		else
			nulls = NULL;

		for (j = 0; j < nargs; j++)
		{
			PyObject   *elem;

			elem = PySequence_GetItem(args, j);
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

		portal = SPI_cursor_open(NULL, plan->plan, plan->values, nulls,
								 exec_ctx->curr_proc->fn_readonly);
		if (portal == NULL)
			elog(ERROR, "SPI_cursor_open() failed: %s",
				 SPI_result_code_string(SPI_result));

		cursor->portalname = MemoryContextStrdup(cursor->mcxt, portal->name);

		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		int			k;

		/* cleanup plan->values array */
		for (k = 0; k < nargs; k++)
		{
			if (!plan->args[k].out.d.typbyval &&
				(plan->values[k] != PointerGetDatum(NULL)))
			{
				pfree(DatumGetPointer(plan->values[k]));
				plan->values[k] = PointerGetDatum(NULL);
			}
		}

		Py_DECREF(cursor);

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

	Assert(cursor->portalname != NULL);
	return (PyObject *) cursor;
}

static void
PLy_cursor_dealloc(PyObject *arg)
{
	PLyCursorObject *cursor;
	Portal		portal;

	cursor = (PLyCursorObject *) arg;

	if (!cursor->closed)
	{
		portal = GetPortalByName(cursor->portalname);

		if (PortalIsValid(portal))
			SPI_cursor_close(portal);
		cursor->closed = true;
	}
	if (cursor->mcxt)
	{
		MemoryContextDelete(cursor->mcxt);
		cursor->mcxt = NULL;
	}
	arg->ob_type->tp_free(arg);
}

static PyObject *
PLy_cursor_iternext(PyObject *self)
{
	PLyCursorObject *cursor;
	PyObject   *ret;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	Portal		portal;

	cursor = (PLyCursorObject *) self;

	if (cursor->closed)
	{
		PLy_exception_set(PyExc_ValueError, "iterating a closed cursor");
		return NULL;
	}

	portal = GetPortalByName(cursor->portalname);
	if (!PortalIsValid(portal))
	{
		PLy_exception_set(PyExc_ValueError,
						  "iterating a cursor in an aborted subtransaction");
		return NULL;
	}

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		SPI_cursor_fetch(portal, true, 1);
		if (SPI_processed == 0)
		{
			PyErr_SetNone(PyExc_StopIteration);
			ret = NULL;
		}
		else
		{
			if (cursor->result.is_rowtype != 1)
				PLy_input_tuple_funcs(&cursor->result, SPI_tuptable->tupdesc);

			ret = PLyDict_FromTuple(&cursor->result, SPI_tuptable->vals[0],
									SPI_tuptable->tupdesc);
		}

		SPI_freetuptable(SPI_tuptable);

		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		PLy_spi_subtransaction_abort(oldcontext, oldowner);
		return NULL;
	}
	PG_END_TRY();

	return ret;
}

static PyObject *
PLy_cursor_fetch(PyObject *self, PyObject *args)
{
	PLyCursorObject *cursor;
	int			count;
	PLyResultObject *ret;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	Portal		portal;

	if (!PyArg_ParseTuple(args, "i", &count))
		return NULL;

	cursor = (PLyCursorObject *) self;

	if (cursor->closed)
	{
		PLy_exception_set(PyExc_ValueError, "fetch from a closed cursor");
		return NULL;
	}

	portal = GetPortalByName(cursor->portalname);
	if (!PortalIsValid(portal))
	{
		PLy_exception_set(PyExc_ValueError,
						  "iterating a cursor in an aborted subtransaction");
		return NULL;
	}

	ret = (PLyResultObject *) PLy_result_new();
	if (ret == NULL)
		return NULL;

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		SPI_cursor_fetch(portal, true, count);

		if (cursor->result.is_rowtype != 1)
			PLy_input_tuple_funcs(&cursor->result, SPI_tuptable->tupdesc);

		Py_DECREF(ret->status);
		ret->status = PyInt_FromLong(SPI_OK_FETCH);

		Py_DECREF(ret->nrows);
		ret->nrows = (SPI_processed > (uint64) LONG_MAX) ?
			PyFloat_FromDouble((double) SPI_processed) :
			PyInt_FromLong((long) SPI_processed);

		if (SPI_processed != 0)
		{
			uint64		i;

			/*
			 * PyList_New() and PyList_SetItem() use Py_ssize_t for list size
			 * and list indices; so we cannot support a result larger than
			 * PY_SSIZE_T_MAX.
			 */
			if (SPI_processed > (uint64) PY_SSIZE_T_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("query result has too many rows to fit in a Python list")));

			Py_DECREF(ret->rows);
			ret->rows = PyList_New(SPI_processed);

			for (i = 0; i < SPI_processed; i++)
			{
				PyObject   *row = PLyDict_FromTuple(&cursor->result,
													SPI_tuptable->vals[i],
													SPI_tuptable->tupdesc);

				PyList_SetItem(ret->rows, i, row);
			}
		}

		SPI_freetuptable(SPI_tuptable);

		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		PLy_spi_subtransaction_abort(oldcontext, oldowner);
		return NULL;
	}
	PG_END_TRY();

	return (PyObject *) ret;
}

static PyObject *
PLy_cursor_close(PyObject *self, PyObject *unused)
{
	PLyCursorObject *cursor = (PLyCursorObject *) self;

	if (!cursor->closed)
	{
		Portal		portal = GetPortalByName(cursor->portalname);

		if (!PortalIsValid(portal))
		{
			PLy_exception_set(PyExc_ValueError,
							"closing a cursor in an aborted subtransaction");
			return NULL;
		}

		SPI_cursor_close(portal);
		cursor->closed = true;
	}

	Py_INCREF(Py_None);
	return Py_None;
}
