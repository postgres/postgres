/*
 * executing Python code
 *
 * src/pl/plpython/plpy_exec.c
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/typcache.h"

#include "plpython.h"

#include "plpy_exec.h"

#include "plpy_elog.h"
#include "plpy_main.h"
#include "plpy_procedure.h"
#include "plpy_subxactobject.h"


static PyObject *PLy_function_build_args(FunctionCallInfo fcinfo, PLyProcedure *proc);
static void PLy_function_delete_args(PLyProcedure *proc);
static void plpython_return_error_callback(void *arg);

static PyObject *PLy_trigger_build_args(FunctionCallInfo fcinfo, PLyProcedure *proc,
					   HeapTuple *rv);
static HeapTuple PLy_modify_tuple(PLyProcedure *proc, PyObject *pltd,
				 TriggerData *tdata, HeapTuple otup);
static void plpython_trigger_error_callback(void *arg);

static PyObject *PLy_procedure_call(PLyProcedure *proc, char *kargs, PyObject *vargs);
static void PLy_abort_open_subtransactions(int save_subxact_level);


/* function subhandler */
Datum
PLy_exec_function(FunctionCallInfo fcinfo, PLyProcedure *proc)
{
	Datum		rv;
	PyObject   *volatile plargs = NULL;
	PyObject   *volatile plrv = NULL;
	ErrorContextCallback plerrcontext;

	PG_TRY();
	{
		if (!proc->is_setof || proc->setof == NULL)
		{
			/*
			 * Simple type returning function or first time for SETOF
			 * function: actually execute the function.
			 */
			plargs = PLy_function_build_args(fcinfo, proc);
			plrv = PLy_procedure_call(proc, "args", plargs);
			if (!proc->is_setof)
			{
				/*
				 * SETOF function parameters will be deleted when last row is
				 * returned
				 */
				PLy_function_delete_args(proc);
			}
			Assert(plrv != NULL);
		}

		/*
		 * If it returns a set, call the iterator to get the next return item.
		 * We stay in the SPI context while doing this, because PyIter_Next()
		 * calls back into Python code which might contain SPI calls.
		 */
		if (proc->is_setof)
		{
			bool		has_error = false;
			ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

			if (proc->setof == NULL)
			{
				/* first time -- do checks and setup */
				if (!rsi || !IsA(rsi, ReturnSetInfo) ||
					(rsi->allowedModes & SFRM_ValuePerCall) == 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unsupported set function return mode"),
							 errdetail("PL/Python set-returning functions only support returning only value per call.")));
				}
				rsi->returnMode = SFRM_ValuePerCall;

				/* Make iterator out of returned object */
				proc->setof = PyObject_GetIter(plrv);
				Py_DECREF(plrv);
				plrv = NULL;

				if (proc->setof == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("returned object cannot be iterated"),
							 errdetail("PL/Python set-returning functions must return an iterable object.")));
			}

			/* Fetch next from iterator */
			plrv = PyIter_Next(proc->setof);
			if (plrv)
				rsi->isDone = ExprMultipleResult;
			else
			{
				rsi->isDone = ExprEndResult;
				has_error = PyErr_Occurred() != NULL;
			}

			if (rsi->isDone == ExprEndResult)
			{
				/* Iterator is exhausted or error happened */
				Py_DECREF(proc->setof);
				proc->setof = NULL;

				Py_XDECREF(plargs);
				Py_XDECREF(plrv);

				PLy_function_delete_args(proc);

				if (has_error)
					PLy_elog(ERROR, "error fetching next item from iterator");

				/* Disconnect from the SPI manager before returning */
				if (SPI_finish() != SPI_OK_FINISH)
					elog(ERROR, "SPI_finish failed");

				fcinfo->isnull = true;
				return (Datum) NULL;
			}
		}

		/*
		 * Disconnect from SPI manager and then create the return values datum
		 * (if the input function does a palloc for it this must not be
		 * allocated in the SPI memory context because SPI_finish would free
		 * it).
		 */
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");

		plerrcontext.callback = plpython_return_error_callback;
		plerrcontext.previous = error_context_stack;
		error_context_stack = &plerrcontext;

		/*
		 * If the function is declared to return void, the Python return value
		 * must be None. For void-returning functions, we also treat a None
		 * return value as a special "void datum" rather than NULL (as is the
		 * case for non-void-returning functions).
		 */
		if (proc->result.out.d.typoid == VOIDOID)
		{
			if (plrv != Py_None)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("PL/Python function with return type \"void\" did not return None")));

			fcinfo->isnull = false;
			rv = (Datum) 0;
		}
		else if (plrv == Py_None)
		{
			fcinfo->isnull = true;
			if (proc->result.is_rowtype < 1)
				rv = InputFunctionCall(&proc->result.out.d.typfunc,
									   NULL,
									   proc->result.out.d.typioparam,
									   -1);
			else
				/* Tuple as None */
				rv = (Datum) NULL;
		}
		else if (proc->result.is_rowtype >= 1)
		{
			TupleDesc	desc;

			/* make sure it's not an unnamed record */
			Assert((proc->result.out.d.typoid == RECORDOID &&
					proc->result.out.d.typmod != -1) ||
				   (proc->result.out.d.typoid != RECORDOID &&
					proc->result.out.d.typmod == -1));

			desc = lookup_rowtype_tupdesc(proc->result.out.d.typoid,
										  proc->result.out.d.typmod);

			rv = PLyObject_ToCompositeDatum(&proc->result, desc, plrv);
			fcinfo->isnull = (rv == (Datum) NULL);
		}
		else
		{
			fcinfo->isnull = false;
			rv = (proc->result.out.d.func) (&proc->result.out.d, -1, plrv);
		}
	}
	PG_CATCH();
	{
		Py_XDECREF(plargs);
		Py_XDECREF(plrv);

		/*
		 * If there was an error the iterator might have not been exhausted
		 * yet. Set it to NULL so the next invocation of the function will
		 * start the iteration again.
		 */
		Py_XDECREF(proc->setof);
		proc->setof = NULL;

		PG_RE_THROW();
	}
	PG_END_TRY();

	error_context_stack = plerrcontext.previous;

	Py_XDECREF(plargs);
	Py_DECREF(plrv);

	return rv;
}

/* trigger subhandler
 *
 * the python function is expected to return Py_None if the tuple is
 * acceptable and unmodified.  Otherwise it should return a PyString
 * object who's value is SKIP, or MODIFY.  SKIP means don't perform
 * this action.  MODIFY means the tuple has been modified, so update
 * tuple and perform action.  SKIP and MODIFY assume the trigger fires
 * BEFORE the event and is ROW level.  postgres expects the function
 * to take no arguments and return an argument of type trigger.
 */
HeapTuple
PLy_exec_trigger(FunctionCallInfo fcinfo, PLyProcedure *proc)
{
	HeapTuple	rv = NULL;
	PyObject   *volatile plargs = NULL;
	PyObject   *volatile plrv = NULL;
	TriggerData *tdata;

	Assert(CALLED_AS_TRIGGER(fcinfo));

	/*
	 * Input/output conversion for trigger tuples.	Use the result TypeInfo
	 * variable to store the tuple conversion info.  We do this over again on
	 * each call to cover the possibility that the relation's tupdesc changed
	 * since the trigger was last called. PLy_input_tuple_funcs and
	 * PLy_output_tuple_funcs are responsible for not doing repetitive work.
	 */
	tdata = (TriggerData *) fcinfo->context;

	PLy_input_tuple_funcs(&(proc->result), tdata->tg_relation->rd_att);
	PLy_output_tuple_funcs(&(proc->result), tdata->tg_relation->rd_att);

	PG_TRY();
	{
		plargs = PLy_trigger_build_args(fcinfo, proc, &rv);
		plrv = PLy_procedure_call(proc, "TD", plargs);

		Assert(plrv != NULL);

		/*
		 * Disconnect from SPI manager
		 */
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");

		/*
		 * return of None means we're happy with the tuple
		 */
		if (plrv != Py_None)
		{
			char	   *srv;

			if (PyString_Check(plrv))
				srv = PyString_AsString(plrv);
			else if (PyUnicode_Check(plrv))
				srv = PLyUnicode_AsString(plrv);
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
					errmsg("unexpected return value from trigger procedure"),
						 errdetail("Expected None or a string.")));
				srv = NULL;		/* keep compiler quiet */
			}

			if (pg_strcasecmp(srv, "SKIP") == 0)
				rv = NULL;
			else if (pg_strcasecmp(srv, "MODIFY") == 0)
			{
				TriggerData *tdata = (TriggerData *) fcinfo->context;

				if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event) ||
					TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
					rv = PLy_modify_tuple(proc, plargs, tdata, rv);
				else
					ereport(WARNING,
							(errmsg("PL/Python trigger function returned \"MODIFY\" in a DELETE trigger -- ignored")));
			}
			else if (pg_strcasecmp(srv, "OK") != 0)
			{
				/*
				 * accept "OK" as an alternative to None; otherwise, raise an
				 * error
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
					errmsg("unexpected return value from trigger procedure"),
						 errdetail("Expected None, \"OK\", \"SKIP\", or \"MODIFY\".")));
			}
		}
	}
	PG_CATCH();
	{
		Py_XDECREF(plargs);
		Py_XDECREF(plrv);

		PG_RE_THROW();
	}
	PG_END_TRY();

	Py_DECREF(plargs);
	Py_DECREF(plrv);

	return rv;
}

/* helper functions for Python code execution */

static PyObject *
PLy_function_build_args(FunctionCallInfo fcinfo, PLyProcedure *proc)
{
	PyObject   *volatile arg = NULL;
	PyObject   *volatile args = NULL;
	int			i;

	PG_TRY();
	{
		args = PyList_New(proc->nargs);
		for (i = 0; i < proc->nargs; i++)
		{
			if (proc->args[i].is_rowtype > 0)
			{
				if (fcinfo->argnull[i])
					arg = NULL;
				else
				{
					HeapTupleHeader td;
					Oid			tupType;
					int32		tupTypmod;
					TupleDesc	tupdesc;
					HeapTupleData tmptup;

					td = DatumGetHeapTupleHeader(fcinfo->arg[i]);
					/* Extract rowtype info and find a tupdesc */
					tupType = HeapTupleHeaderGetTypeId(td);
					tupTypmod = HeapTupleHeaderGetTypMod(td);
					tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

					/* Set up I/O funcs if not done yet */
					if (proc->args[i].is_rowtype != 1)
						PLy_input_tuple_funcs(&(proc->args[i]), tupdesc);

					/* Build a temporary HeapTuple control structure */
					tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
					tmptup.t_data = td;

					arg = PLyDict_FromTuple(&(proc->args[i]), &tmptup, tupdesc);
					ReleaseTupleDesc(tupdesc);
				}
			}
			else
			{
				if (fcinfo->argnull[i])
					arg = NULL;
				else
				{
					arg = (proc->args[i].in.d.func) (&(proc->args[i].in.d),
													 fcinfo->arg[i]);
				}
			}

			if (arg == NULL)
			{
				Py_INCREF(Py_None);
				arg = Py_None;
			}

			if (PyList_SetItem(args, i, arg) == -1)
				PLy_elog(ERROR, "PyList_SetItem() failed, while setting up arguments");

			if (proc->argnames && proc->argnames[i] &&
			PyDict_SetItemString(proc->globals, proc->argnames[i], arg) == -1)
				PLy_elog(ERROR, "PyDict_SetItemString() failed, while setting up arguments");
			arg = NULL;
		}

		/* Set up output conversion for functions returning RECORD */
		if (proc->result.out.d.typoid == RECORDOID)
		{
			TupleDesc	desc;

			if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));

			/* cache the output conversion functions */
			PLy_output_record_funcs(&(proc->result), desc);
		}
	}
	PG_CATCH();
	{
		Py_XDECREF(arg);
		Py_XDECREF(args);

		PG_RE_THROW();
	}
	PG_END_TRY();

	return args;
}

static void
PLy_function_delete_args(PLyProcedure *proc)
{
	int			i;

	if (!proc->argnames)
		return;

	for (i = 0; i < proc->nargs; i++)
		if (proc->argnames[i])
			PyDict_DelItemString(proc->globals, proc->argnames[i]);
}

static void
plpython_return_error_callback(void *arg)
{
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();

	if (exec_ctx->curr_proc)
		errcontext("while creating return value");
}

static PyObject *
PLy_trigger_build_args(FunctionCallInfo fcinfo, PLyProcedure *proc, HeapTuple *rv)
{
	TriggerData *tdata = (TriggerData *) fcinfo->context;
	PyObject   *pltname,
			   *pltevent,
			   *pltwhen,
			   *pltlevel,
			   *pltrelid,
			   *plttablename,
			   *plttableschema;
	PyObject   *pltargs,
			   *pytnew,
			   *pytold;
	PyObject   *volatile pltdata = NULL;
	char	   *stroid;

	PG_TRY();
	{
		pltdata = PyDict_New();
		if (!pltdata)
			PLy_elog(ERROR, "could not create new dictionary while building trigger arguments");

		pltname = PyString_FromString(tdata->tg_trigger->tgname);
		PyDict_SetItemString(pltdata, "name", pltname);
		Py_DECREF(pltname);

		stroid = DatumGetCString(DirectFunctionCall1(oidout,
							   ObjectIdGetDatum(tdata->tg_relation->rd_id)));
		pltrelid = PyString_FromString(stroid);
		PyDict_SetItemString(pltdata, "relid", pltrelid);
		Py_DECREF(pltrelid);
		pfree(stroid);

		stroid = SPI_getrelname(tdata->tg_relation);
		plttablename = PyString_FromString(stroid);
		PyDict_SetItemString(pltdata, "table_name", plttablename);
		Py_DECREF(plttablename);
		pfree(stroid);

		stroid = SPI_getnspname(tdata->tg_relation);
		plttableschema = PyString_FromString(stroid);
		PyDict_SetItemString(pltdata, "table_schema", plttableschema);
		Py_DECREF(plttableschema);
		pfree(stroid);

		if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
			pltwhen = PyString_FromString("BEFORE");
		else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
			pltwhen = PyString_FromString("AFTER");
		else if (TRIGGER_FIRED_INSTEAD(tdata->tg_event))
			pltwhen = PyString_FromString("INSTEAD OF");
		else
		{
			elog(ERROR, "unrecognized WHEN tg_event: %u", tdata->tg_event);
			pltwhen = NULL;		/* keep compiler quiet */
		}
		PyDict_SetItemString(pltdata, "when", pltwhen);
		Py_DECREF(pltwhen);

		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		{
			pltlevel = PyString_FromString("ROW");
			PyDict_SetItemString(pltdata, "level", pltlevel);
			Py_DECREF(pltlevel);

			if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
			{
				pltevent = PyString_FromString("INSERT");

				PyDict_SetItemString(pltdata, "old", Py_None);
				pytnew = PLyDict_FromTuple(&(proc->result), tdata->tg_trigtuple,
										   tdata->tg_relation->rd_att);
				PyDict_SetItemString(pltdata, "new", pytnew);
				Py_DECREF(pytnew);
				*rv = tdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
			{
				pltevent = PyString_FromString("DELETE");

				PyDict_SetItemString(pltdata, "new", Py_None);
				pytold = PLyDict_FromTuple(&(proc->result), tdata->tg_trigtuple,
										   tdata->tg_relation->rd_att);
				PyDict_SetItemString(pltdata, "old", pytold);
				Py_DECREF(pytold);
				*rv = tdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
			{
				pltevent = PyString_FromString("UPDATE");

				pytnew = PLyDict_FromTuple(&(proc->result), tdata->tg_newtuple,
										   tdata->tg_relation->rd_att);
				PyDict_SetItemString(pltdata, "new", pytnew);
				Py_DECREF(pytnew);
				pytold = PLyDict_FromTuple(&(proc->result), tdata->tg_trigtuple,
										   tdata->tg_relation->rd_att);
				PyDict_SetItemString(pltdata, "old", pytold);
				Py_DECREF(pytold);
				*rv = tdata->tg_newtuple;
			}
			else
			{
				elog(ERROR, "unrecognized OP tg_event: %u", tdata->tg_event);
				pltevent = NULL;	/* keep compiler quiet */
			}

			PyDict_SetItemString(pltdata, "event", pltevent);
			Py_DECREF(pltevent);
		}
		else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		{
			pltlevel = PyString_FromString("STATEMENT");
			PyDict_SetItemString(pltdata, "level", pltlevel);
			Py_DECREF(pltlevel);

			PyDict_SetItemString(pltdata, "old", Py_None);
			PyDict_SetItemString(pltdata, "new", Py_None);
			*rv = NULL;

			if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
				pltevent = PyString_FromString("INSERT");
			else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
				pltevent = PyString_FromString("DELETE");
			else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
				pltevent = PyString_FromString("UPDATE");
			else if (TRIGGER_FIRED_BY_TRUNCATE(tdata->tg_event))
				pltevent = PyString_FromString("TRUNCATE");
			else
			{
				elog(ERROR, "unrecognized OP tg_event: %u", tdata->tg_event);
				pltevent = NULL;	/* keep compiler quiet */
			}

			PyDict_SetItemString(pltdata, "event", pltevent);
			Py_DECREF(pltevent);
		}
		else
			elog(ERROR, "unrecognized LEVEL tg_event: %u", tdata->tg_event);

		if (tdata->tg_trigger->tgnargs)
		{
			/*
			 * all strings...
			 */
			int			i;
			PyObject   *pltarg;

			pltargs = PyList_New(tdata->tg_trigger->tgnargs);
			for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
			{
				pltarg = PyString_FromString(tdata->tg_trigger->tgargs[i]);

				/*
				 * stolen, don't Py_DECREF
				 */
				PyList_SetItem(pltargs, i, pltarg);
			}
		}
		else
		{
			Py_INCREF(Py_None);
			pltargs = Py_None;
		}
		PyDict_SetItemString(pltdata, "args", pltargs);
		Py_DECREF(pltargs);
	}
	PG_CATCH();
	{
		Py_XDECREF(pltdata);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return pltdata;
}

static HeapTuple
PLy_modify_tuple(PLyProcedure *proc, PyObject *pltd, TriggerData *tdata,
				 HeapTuple otup)
{
	PyObject   *volatile plntup;
	PyObject   *volatile plkeys;
	PyObject   *volatile platt;
	PyObject   *volatile plval;
	PyObject   *volatile plstr;
	HeapTuple	rtup;
	int			natts,
				i,
				attn,
				atti;
	int		   *volatile modattrs;
	Datum	   *volatile modvalues;
	char	   *volatile modnulls;
	TupleDesc	tupdesc;
	ErrorContextCallback plerrcontext;

	plerrcontext.callback = plpython_trigger_error_callback;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	plntup = plkeys = platt = plval = plstr = NULL;
	modattrs = NULL;
	modvalues = NULL;
	modnulls = NULL;

	PG_TRY();
	{
		if ((plntup = PyDict_GetItemString(pltd, "new")) == NULL)
			ereport(ERROR,
					(errmsg("TD[\"new\"] deleted, cannot modify row")));
		if (!PyDict_Check(plntup))
			ereport(ERROR,
					(errmsg("TD[\"new\"] is not a dictionary")));
		Py_INCREF(plntup);

		plkeys = PyDict_Keys(plntup);
		natts = PyList_Size(plkeys);

		modattrs = (int *) palloc(natts * sizeof(int));
		modvalues = (Datum *) palloc(natts * sizeof(Datum));
		modnulls = (char *) palloc(natts * sizeof(char));

		tupdesc = tdata->tg_relation->rd_att;

		for (i = 0; i < natts; i++)
		{
			char	   *plattstr;

			platt = PyList_GetItem(plkeys, i);
			if (PyString_Check(platt))
				plattstr = PyString_AsString(platt);
			else if (PyUnicode_Check(platt))
				plattstr = PLyUnicode_AsString(platt);
			else
			{
				ereport(ERROR,
						(errmsg("TD[\"new\"] dictionary key at ordinal position %d is not a string", i)));
				plattstr = NULL;	/* keep compiler quiet */
			}
			attn = SPI_fnumber(tupdesc, plattstr);
			if (attn == SPI_ERROR_NOATTRIBUTE)
				ereport(ERROR,
						(errmsg("key \"%s\" found in TD[\"new\"] does not exist as a column in the triggering row",
								plattstr)));
			atti = attn - 1;

			plval = PyDict_GetItem(plntup, platt);
			if (plval == NULL)
				elog(FATAL, "Python interpreter is probably corrupted");

			Py_INCREF(plval);

			modattrs[i] = attn;

			if (tupdesc->attrs[atti]->attisdropped)
			{
				modvalues[i] = (Datum) 0;
				modnulls[i] = 'n';
			}
			else if (plval != Py_None)
			{
				PLyObToDatum *att = &proc->result.out.r.atts[atti];

				modvalues[i] = (att->func) (att,
											tupdesc->attrs[atti]->atttypmod,
											plval);
				modnulls[i] = ' ';
			}
			else
			{
				modvalues[i] =
					InputFunctionCall(&proc->result.out.r.atts[atti].typfunc,
									  NULL,
									proc->result.out.r.atts[atti].typioparam,
									  tupdesc->attrs[atti]->atttypmod);
				modnulls[i] = 'n';
			}

			Py_DECREF(plval);
			plval = NULL;
		}

		rtup = SPI_modifytuple(tdata->tg_relation, otup, natts,
							   modattrs, modvalues, modnulls);
		if (rtup == NULL)
			elog(ERROR, "SPI_modifytuple failed: error %d", SPI_result);
	}
	PG_CATCH();
	{
		Py_XDECREF(plntup);
		Py_XDECREF(plkeys);
		Py_XDECREF(plval);
		Py_XDECREF(plstr);

		if (modnulls)
			pfree(modnulls);
		if (modvalues)
			pfree(modvalues);
		if (modattrs)
			pfree(modattrs);

		PG_RE_THROW();
	}
	PG_END_TRY();

	Py_DECREF(plntup);
	Py_DECREF(plkeys);

	pfree(modattrs);
	pfree(modvalues);
	pfree(modnulls);

	error_context_stack = plerrcontext.previous;

	return rtup;
}

static void
plpython_trigger_error_callback(void *arg)
{
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();

	if (exec_ctx->curr_proc)
		errcontext("while modifying trigger row");
}

/* execute Python code, propagate Python errors to the backend */
static PyObject *
PLy_procedure_call(PLyProcedure *proc, char *kargs, PyObject *vargs)
{
	PyObject   *rv;
	int volatile save_subxact_level = list_length(explicit_subtransactions);

	PyDict_SetItemString(proc->globals, kargs, vargs);

	PG_TRY();
	{
#if PY_VERSION_HEX >= 0x03020000
		rv = PyEval_EvalCode(proc->code,
							 proc->globals, proc->globals);
#else
		rv = PyEval_EvalCode((PyCodeObject *) proc->code,
							 proc->globals, proc->globals);
#endif

		/*
		 * Since plpy will only let you close subtransactions that you
		 * started, you cannot *unnest* subtransactions, only *nest* them
		 * without closing.
		 */
		Assert(list_length(explicit_subtransactions) >= save_subxact_level);
	}
	PG_CATCH();
	{
		PLy_abort_open_subtransactions(save_subxact_level);
		PG_RE_THROW();
	}
	PG_END_TRY();

	PLy_abort_open_subtransactions(save_subxact_level);

	/* If the Python code returned an error, propagate it */
	if (rv == NULL)
		PLy_elog(ERROR, NULL);

	return rv;
}

/*
 * Abort lingering subtransactions that have been explicitly started
 * by plpy.subtransaction().start() and not properly closed.
 */
static void
PLy_abort_open_subtransactions(int save_subxact_level)
{
	Assert(save_subxact_level >= 0);

	while (list_length(explicit_subtransactions) > save_subxact_level)
	{
		PLySubtransactionData *subtransactiondata;

		Assert(explicit_subtransactions != NIL);

		ereport(WARNING,
				(errmsg("forcibly aborting a subtransaction that has not been exited")));

		RollbackAndReleaseCurrentSubTransaction();

		SPI_restore_connection();

		subtransactiondata = (PLySubtransactionData *) linitial(explicit_subtransactions);
		explicit_subtransactions = list_delete_first(explicit_subtransactions);

		MemoryContextSwitchTo(subtransactiondata->oldcontext);
		CurrentResourceOwner = subtransactiondata->oldowner;
		PLy_free(subtransactiondata);
	}
}
