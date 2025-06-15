/*
 * the PLyCursor class
 *
 * src/pl/plpython/plpy_cursorobject.c
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "plpy_cursorobject.h"
#include "plpy_elog.h"
#include "plpy_main.h"
#include "plpy_planobject.h"
#include "plpy_resultobject.h"
#include "plpy_spi.h"
#include "plpy_util.h"
#include "utils/memutils.h"

static PyObject *PLy_cursor_query(const char *query);
static void PLy_cursor_dealloc(PLyCursorObject *self);
static PyObject *PLy_cursor_iternext(PyObject *self);
static PyObject *PLy_cursor_fetch(PyObject *self, PyObject *args);
static PyObject *PLy_cursor_close(PyObject *self, PyObject *unused);

static const char PLy_cursor_doc[] = "Wrapper around a PostgreSQL cursor";

static PyMethodDef PLy_cursor_methods[] = {
	{"fetch", PLy_cursor_fetch, METH_VARARGS, NULL},
	{"close", PLy_cursor_close, METH_NOARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyType_Slot PLyCursor_slots[] =
{
	{
		Py_tp_dealloc, PLy_cursor_dealloc
	},
	{
		Py_tp_doc, (char *) PLy_cursor_doc
	},
	{
		Py_tp_iter, PyObject_SelfIter
	},
	{
		Py_tp_iternext, PLy_cursor_iternext
	},
	{
		Py_tp_methods, PLy_cursor_methods
	},
	{
		0, NULL
	}
};

static PyType_Spec PLyCursor_spec =
{
	.name = "PLyCursor",
	.basicsize = sizeof(PLyCursorObject),
	.flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.slots = PLyCursor_slots,
};

static PyTypeObject *PLy_CursorType;

void
PLy_cursor_init_type(void)
{
	PLy_CursorType = (PyTypeObject *) PyType_FromSpec(&PLyCursor_spec);
	if (!PLy_CursorType)
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
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;

	if ((cursor = PyObject_New(PLyCursorObject, PLy_CursorType)) == NULL)
		return NULL;
#if PY_VERSION_HEX < 0x03080000
	/* Workaround for Python issue 35810; no longer necessary in Python 3.8 */
	Py_INCREF(PLy_CursorType);
#endif
	cursor->portalname = NULL;
	cursor->closed = false;
	cursor->mcxt = AllocSetContextCreate(TopMemoryContext,
										 "PL/Python cursor context",
										 ALLOCSET_DEFAULT_SIZES);

	/* Initialize for converting result tuples to Python */
	PLy_input_setup_func(&cursor->result, cursor->mcxt,
						 RECORDOID, -1,
						 exec_ctx->curr_proc);

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
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

		PinPortal(portal);

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

PyObject *
PLy_cursor_plan(PyObject *ob, PyObject *args)
{
	PLyCursorObject *cursor;
	volatile int nargs;
	PLyPlanObject *plan;
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;

	if (args)
	{
		if (!PySequence_Check(args) || PyUnicode_Check(args))
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
		sv = PLyUnicode_AsString(so);
		PLy_exception_set_plural(PyExc_TypeError,
								 "Expected sequence of %d argument, got %d: %s",
								 "Expected sequence of %d arguments, got %d: %s",
								 plan->nargs,
								 plan->nargs, nargs, sv);
		Py_DECREF(so);

		return NULL;
	}

	if ((cursor = PyObject_New(PLyCursorObject, PLy_CursorType)) == NULL)
		return NULL;
#if PY_VERSION_HEX < 0x03080000
	/* Workaround for Python issue 35810; no longer necessary in Python 3.8 */
	Py_INCREF(PLy_CursorType);
#endif
	cursor->portalname = NULL;
	cursor->closed = false;
	cursor->mcxt = AllocSetContextCreate(TopMemoryContext,
										 "PL/Python cursor context",
										 ALLOCSET_DEFAULT_SIZES);

	/* Initialize for converting result tuples to Python */
	PLy_input_setup_func(&cursor->result, cursor->mcxt,
						 RECORDOID, -1,
						 exec_ctx->curr_proc);

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	PLy_spi_subtransaction_begin(oldcontext, oldowner);

	PG_TRY();
	{
		Portal		portal;
		MemoryContext tmpcontext;
		Datum	   *volatile values;
		char	   *volatile nulls;
		volatile int j;

		/*
		 * Converted arguments and associated cruft will be in this context,
		 * which is local to our subtransaction.
		 */
		tmpcontext = AllocSetContextCreate(CurTransactionContext,
										   "PL/Python temporary context",
										   ALLOCSET_SMALL_SIZES);
		MemoryContextSwitchTo(tmpcontext);

		if (nargs > 0)
		{
			values = (Datum *) palloc(nargs * sizeof(Datum));
			nulls = (char *) palloc(nargs * sizeof(char));
		}
		else
		{
			values = NULL;
			nulls = NULL;
		}

		for (j = 0; j < nargs; j++)
		{
			PLyObToDatum *arg = &plan->args[j];
			PyObject   *elem;

			elem = PySequence_GetItem(args, j);
			PG_TRY(2);
			{
				bool		isnull;

				values[j] = PLy_output_convert(arg, elem, &isnull);
				nulls[j] = isnull ? 'n' : ' ';
			}
			PG_FINALLY(2);
			{
				Py_DECREF(elem);
			}
			PG_END_TRY(2);
		}

		MemoryContextSwitchTo(oldcontext);

		portal = SPI_cursor_open(NULL, plan->plan, values, nulls,
								 exec_ctx->curr_proc->fn_readonly);
		if (portal == NULL)
			elog(ERROR, "SPI_cursor_open() failed: %s",
				 SPI_result_code_string(SPI_result));

		cursor->portalname = MemoryContextStrdup(cursor->mcxt, portal->name);

		PinPortal(portal);

		MemoryContextDelete(tmpcontext);
		PLy_spi_subtransaction_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		Py_DECREF(cursor);
		/* Subtransaction abort will remove the tmpcontext */
		PLy_spi_subtransaction_abort(oldcontext, oldowner);
		return NULL;
	}
	PG_END_TRY();

	Assert(cursor->portalname != NULL);
	return (PyObject *) cursor;
}

static void
PLy_cursor_dealloc(PLyCursorObject *self)
{
#if PY_VERSION_HEX >= 0x03080000
	PyTypeObject *tp = Py_TYPE(self);
#endif
	Portal		portal;

	if (!self->closed)
	{
		portal = GetPortalByName(self->portalname);

		if (PortalIsValid(portal))
		{
			UnpinPortal(portal);
			SPI_cursor_close(portal);
		}
		self->closed = true;
	}
	if (self->mcxt)
	{
		MemoryContextDelete(self->mcxt);
		self->mcxt = NULL;
	}

	PyObject_Free(self);
#if PY_VERSION_HEX >= 0x03080000
	/* This was not needed before Python 3.8 (Python issue 35810) */
	Py_DECREF(tp);
#endif
}

static PyObject *
PLy_cursor_iternext(PyObject *self)
{
	PLyCursorObject *cursor;
	PyObject   *ret;
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();
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
			PLy_input_setup_tuple(&cursor->result, SPI_tuptable->tupdesc,
								  exec_ctx->curr_proc);

			ret = PLy_input_from_tuple(&cursor->result, SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, true);
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
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	Portal		portal;

	if (!PyArg_ParseTuple(args, "i:fetch", &count))
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

		Py_DECREF(ret->status);
		ret->status = PyLong_FromLong(SPI_OK_FETCH);

		Py_DECREF(ret->nrows);
		ret->nrows = PyLong_FromUnsignedLongLong(SPI_processed);

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
			if (!ret->rows)
			{
				Py_DECREF(ret);
				ret = NULL;
			}
			else
			{
				PLy_input_setup_tuple(&cursor->result, SPI_tuptable->tupdesc,
									  exec_ctx->curr_proc);

				for (i = 0; i < SPI_processed; i++)
				{
					PyObject   *row = PLy_input_from_tuple(&cursor->result,
														   SPI_tuptable->vals[i],
														   SPI_tuptable->tupdesc,
														   true);

					PyList_SetItem(ret->rows, i, row);
				}
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

		UnpinPortal(portal);
		SPI_cursor_close(portal);
		cursor->closed = true;
	}

	Py_RETURN_NONE;
}
