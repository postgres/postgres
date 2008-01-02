/**********************************************************************
 * plpython.c - python as a procedural language for PostgreSQL
 *
 *	$PostgreSQL: pgsql/src/pl/plpython/plpython.c,v 1.106 2008/01/02 03:10:27 tgl Exp $
 *
 *********************************************************************
 */

#if defined(_MSC_VER) && defined(_DEBUG)
/* Python uses #pragma to bring in a non-default libpython on VC++ if
 * _DEBUG is defined */
#undef _DEBUG
/* Also hide away errcode, since we load Python.h before postgres.h */
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#define _DEBUG
#elif defined (_MSC_VER)
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#else
#include <Python.h>
#endif

/*
 * Py_ssize_t compat for Python <= 2.4
 */
#if PY_VERSION_HEX < 0x02050000 && !defined(PY_SSIZE_T_MIN)
typedef int Py_ssize_t;

#define PY_SSIZE_T_MAX INT_MAX
#define PY_SSIZE_T_MIN INT_MIN
#endif

/*
 * PyBool_FromLong is supported from 2.3.
 */
#if PY_VERSION_HEX < 0x02030000
#define PyBool_FromLong(x) PyInt_FromLong(x)
#endif


#include "postgres.h"

/* system stuff */
#include <unistd.h>
#include <fcntl.h>

/* postgreSQL stuff */
#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include <compile.h>
#include <eval.h>

PG_MODULE_MAGIC;

/* convert Postgresql Datum or tuple into a PyObject.
 * input to Python.  Tuples are converted to dictionary
 * objects.
 */

typedef PyObject *(*PLyDatumToObFunc) (const char *);

typedef struct PLyDatumToOb
{
	PLyDatumToObFunc func;
	FmgrInfo	typfunc;		/* The type's output function */
	Oid			typoid;			/* The OID of the type */
	Oid			typioparam;
	bool		typbyval;
}	PLyDatumToOb;

typedef struct PLyTupleToOb
{
	PLyDatumToOb *atts;
	int			natts;
}	PLyTupleToOb;

typedef union PLyTypeInput
{
	PLyDatumToOb d;
	PLyTupleToOb r;
}	PLyTypeInput;

/* convert PyObject to a Postgresql Datum or tuple.
 * output from Python
 */
typedef struct PLyObToDatum
{
	FmgrInfo	typfunc;		/* The type's input function */
	Oid			typoid;			/* The OID of the type */
	Oid			typioparam;
	bool		typbyval;
}	PLyObToDatum;

typedef struct PLyObToTuple
{
	PLyObToDatum *atts;
	int			natts;
}	PLyObToTuple;

typedef union PLyTypeOutput
{
	PLyObToDatum d;
	PLyObToTuple r;
}	PLyTypeOutput;

/* all we need to move Postgresql data to Python objects,
 * and vis versa
 */
typedef struct PLyTypeInfo
{
	PLyTypeInput in;
	PLyTypeOutput out;
	int			is_rowtype;

	/*
	 * is_rowtype can be: -1  not known yet (initial state) 0  scalar datatype
	 * 1  rowtype 2  rowtype, but I/O functions not set up yet
	 */
}	PLyTypeInfo;


/* cached procedure data */
typedef struct PLyProcedure
{
	char	   *proname;		/* SQL name of procedure */
	char	   *pyname;			/* Python name of procedure */
	TransactionId fn_xmin;
	ItemPointerData fn_tid;
	bool		fn_readonly;
	PLyTypeInfo result;			/* also used to store info for trigger tuple
								 * type */
	bool		is_setof;		/* true, if procedure returns result set */
	PyObject   *setof;			/* contents of result set. */
	char	  **argnames;		/* Argument names */
	PLyTypeInfo args[FUNC_MAX_ARGS];
	int			nargs;
	PyObject   *code;			/* compiled procedure code */
	PyObject   *statics;		/* data saved across calls, local scope */
	PyObject   *globals;		/* data saved across calls, global scope */
	PyObject   *me;				/* PyCObject containing pointer to this
								 * PLyProcedure */
}	PLyProcedure;


/* Python objects */
typedef struct PLyPlanObject
{
	PyObject_HEAD
	void	   *plan;			/* return of an SPI_saveplan */
	int			nargs;
	Oid		   *types;
	Datum	   *values;
	PLyTypeInfo *args;
}	PLyPlanObject;

typedef struct PLyResultObject
{
	PyObject_HEAD
	/* HeapTuple *tuples; */
	PyObject * nrows;			/* number of rows returned by query */
	PyObject   *rows;			/* data rows, or None if no data returned */
	PyObject   *status;			/* query status, SPI_OK_*, or SPI_ERR_* */
}	PLyResultObject;


/* function declarations */

/* Two exported functions: first is the magic telling Postgresql
 * what function call interface it implements. Second is for
 * initialization of the interpreter during library load.
 */
Datum		plpython_call_handler(PG_FUNCTION_ARGS);
void		_PG_init(void);

PG_FUNCTION_INFO_V1(plpython_call_handler);

/* most of the remaining of the declarations, all static */

/* these should only be called once at the first call
 * of plpython_call_handler.  initialize the python interpreter
 * and global data.
 */
static void PLy_init_interp(void);
static void PLy_init_plpy(void);

/* call PyErr_SetString with a vprint interface */
static void
PLy_exception_set(PyObject *, const char *,...)
__attribute__((format(printf, 2, 3)));

/* Get the innermost python procedure called from the backend */
static char *PLy_procedure_name(PLyProcedure *);

/* some utility functions */
static void PLy_elog(int, const char *,...);
static char *PLy_traceback(int *);

static void *PLy_malloc(size_t);
static void *PLy_malloc0(size_t);
static char *PLy_strdup(const char *);
static void PLy_free(void *);

/* sub handlers for functions and triggers */
static Datum PLy_function_handler(FunctionCallInfo fcinfo, PLyProcedure *);
static HeapTuple PLy_trigger_handler(FunctionCallInfo fcinfo, PLyProcedure *);

static PyObject *PLy_function_build_args(FunctionCallInfo fcinfo, PLyProcedure *);
static void PLy_function_delete_args(PLyProcedure *);
static PyObject *PLy_trigger_build_args(FunctionCallInfo fcinfo, PLyProcedure *,
					   HeapTuple *);
static HeapTuple PLy_modify_tuple(PLyProcedure *, PyObject *,
				 TriggerData *, HeapTuple);

static PyObject *PLy_procedure_call(PLyProcedure *, char *, PyObject *);

static PLyProcedure *PLy_procedure_get(FunctionCallInfo fcinfo,
				  Oid tgreloid);

static PLyProcedure *PLy_procedure_create(HeapTuple procTup, Oid tgreloid,
										  char *key);

static void PLy_procedure_compile(PLyProcedure *, const char *);
static char *PLy_procedure_munge_source(const char *, const char *);
static void PLy_procedure_delete(PLyProcedure *);

static void PLy_typeinfo_init(PLyTypeInfo *);
static void PLy_typeinfo_dealloc(PLyTypeInfo *);
static void PLy_output_datum_func(PLyTypeInfo *, HeapTuple);
static void PLy_output_datum_func2(PLyObToDatum *, HeapTuple);
static void PLy_input_datum_func(PLyTypeInfo *, Oid, HeapTuple);
static void PLy_input_datum_func2(PLyDatumToOb *, Oid, HeapTuple);
static void PLy_output_tuple_funcs(PLyTypeInfo *, TupleDesc);
static void PLy_input_tuple_funcs(PLyTypeInfo *, TupleDesc);

/* conversion functions */
static PyObject *PLyDict_FromTuple(PLyTypeInfo *, HeapTuple, TupleDesc);
static PyObject *PLyBool_FromString(const char *);
static PyObject *PLyFloat_FromString(const char *);
static PyObject *PLyInt_FromString(const char *);
static PyObject *PLyLong_FromString(const char *);
static PyObject *PLyString_FromString(const char *);

static HeapTuple PLyMapping_ToTuple(PLyTypeInfo *, PyObject *);
static HeapTuple PLySequence_ToTuple(PLyTypeInfo *, PyObject *);
static HeapTuple PLyObject_ToTuple(PLyTypeInfo *, PyObject *);

/*
 * Currently active plpython function
 */
static PLyProcedure *PLy_curr_procedure = NULL;

/*
 * When a callback from Python into PG incurs an error, we temporarily store
 * the error information here, and return NULL to the Python interpreter.
 * Any further callback attempts immediately fail, and when the Python
 * interpreter returns to the calling function, we re-throw the error (even if
 * Python thinks it trapped the error and doesn't return NULL).  Eventually
 * this ought to be improved to let Python code really truly trap the error,
 * but that's more of a change from the pre-8.0 semantics than I have time for
 * now --- it will only be possible if the callback query is executed inside a
 * subtransaction.
 */
static ErrorData *PLy_error_in_progress = NULL;

static PyObject *PLy_interp_globals = NULL;
static PyObject *PLy_interp_safe_globals = NULL;
static PyObject *PLy_procedure_cache = NULL;

/* Python exceptions */
static PyObject *PLy_exc_error = NULL;
static PyObject *PLy_exc_fatal = NULL;
static PyObject *PLy_exc_spi_error = NULL;

/* some globals for the python module */
static char PLy_plan_doc[] = {
	"Store a PostgreSQL plan"
};

static char PLy_result_doc[] = {
	"Results of a PostgreSQL query"
};


/*
 * the function definitions
 */

/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the cached form of plpython functions/queries is allocated permanently
 * (mostly via malloc()) and never released until backend exit.  Subsidiary
 * data structures such as fmgr info records therefore must live forever
 * as well.  A better implementation would store all this stuff in a per-
 * function memory context that could be reclaimed at need.  In the meantime,
 * fmgr_info_cxt must be called specifying TopMemoryContext so that whatever
 * it might allocate, and whatever the eventual function might allocate using
 * fn_mcxt, will live forever too.
 */
static void
perm_fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt(functionId, finfo, TopMemoryContext);
}

Datum
plpython_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	PLyProcedure *save_curr_proc;
	PLyProcedure *volatile proc = NULL;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	save_curr_proc = PLy_curr_procedure;

	PG_TRY();
	{
		if (CALLED_AS_TRIGGER(fcinfo))
		{
			TriggerData *tdata = (TriggerData *) fcinfo->context;
			HeapTuple	trv;

			proc = PLy_procedure_get(fcinfo,
									 RelationGetRelid(tdata->tg_relation));
			PLy_curr_procedure = proc;
			trv = PLy_trigger_handler(fcinfo, proc);
			retval = PointerGetDatum(trv);
		}
		else
		{
			proc = PLy_procedure_get(fcinfo, InvalidOid);
			PLy_curr_procedure = proc;
			retval = PLy_function_handler(fcinfo, proc);
		}
	}
	PG_CATCH();
	{
		PLy_curr_procedure = save_curr_proc;
		if (proc)
		{
			/* note: Py_DECREF needs braces around it, as of 2003/08 */
			Py_DECREF(proc->me);
		}
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	PLy_curr_procedure = save_curr_proc;

	Py_DECREF(proc->me);

	return retval;
}

/* trigger and function sub handlers
 *
 * the python function is expected to return Py_None if the tuple is
 * acceptable and unmodified.  Otherwise it should return a PyString
 * object who's value is SKIP, or MODIFY.  SKIP means don't perform
 * this action.  MODIFY means the tuple has been modified, so update
 * tuple and perform action.  SKIP and MODIFY assume the trigger fires
 * BEFORE the event and is ROW level.  postgres expects the function
 * to take no arguments and return an argument of type trigger.
 */
static HeapTuple
PLy_trigger_handler(FunctionCallInfo fcinfo, PLyProcedure * proc)
{
	HeapTuple	rv = NULL;
	PyObject   *volatile plargs = NULL;
	PyObject   *volatile plrv = NULL;

	PG_TRY();
	{
		plargs = PLy_trigger_build_args(fcinfo, proc, &rv);
		plrv = PLy_procedure_call(proc, "TD", plargs);

		Assert(plrv != NULL);
		Assert(!PLy_error_in_progress);

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

			if (!PyString_Check(plrv))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
					errmsg("unexpected return value from trigger procedure"),
						 errdetail("Expected None or a String.")));

			srv = PyString_AsString(plrv);
			if (pg_strcasecmp(srv, "SKIP") == 0)
				rv = NULL;
			else if (pg_strcasecmp(srv, "MODIFY") == 0)
			{
				TriggerData *tdata = (TriggerData *) fcinfo->context;

				if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event) ||
					TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
					rv = PLy_modify_tuple(proc, plargs, tdata, rv);
				else
					elog(WARNING, "ignoring modified tuple in DELETE trigger");
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

static HeapTuple
PLy_modify_tuple(PLyProcedure * proc, PyObject * pltd, TriggerData *tdata,
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

	plntup = plkeys = platt = plval = plstr = NULL;
	modattrs = NULL;
	modvalues = NULL;
	modnulls = NULL;

	PG_TRY();
	{
		if ((plntup = PyDict_GetItemString(pltd, "new")) == NULL)
			elog(ERROR, "TD[\"new\"] deleted, cannot modify tuple");
		if (!PyDict_Check(plntup))
			elog(ERROR, "TD[\"new\"] is not a dictionary object");
		Py_INCREF(plntup);

		plkeys = PyDict_Keys(plntup);
		natts = PyList_Size(plkeys);

		modattrs = (int *) palloc(natts * sizeof(int));
		modvalues = (Datum *) palloc(natts * sizeof(Datum));
		modnulls = (char *) palloc(natts * sizeof(char));

		tupdesc = tdata->tg_relation->rd_att;

		for (i = 0; i < natts; i++)
		{
			char	   *src;

			platt = PyList_GetItem(plkeys, i);
			if (!PyString_Check(platt))
				elog(ERROR, "attribute name is not a string");
			attn = SPI_fnumber(tupdesc, PyString_AsString(platt));
			if (attn == SPI_ERROR_NOATTRIBUTE)
				elog(ERROR, "invalid attribute \"%s\" in tuple",
					 PyString_AsString(platt));
			atti = attn - 1;

			plval = PyDict_GetItem(plntup, platt);
			if (plval == NULL)
				elog(FATAL, "python interpreter is probably corrupted");

			Py_INCREF(plval);

			modattrs[i] = attn;

			if (tupdesc->attrs[atti]->attisdropped)
			{
				modvalues[i] = (Datum) 0;
				modnulls[i] = 'n';
			}
			else if (plval != Py_None)
			{
				plstr = PyObject_Str(plval);
				if (!plstr)
					PLy_elog(ERROR, "function \"%s\" could not modify tuple",
							 proc->proname);
				src = PyString_AsString(plstr);

				modvalues[i] =
					InputFunctionCall(&proc->result.out.r.atts[atti].typfunc,
									  src,
									proc->result.out.r.atts[atti].typioparam,
									  tupdesc->attrs[atti]->atttypmod);
				modnulls[i] = ' ';

				Py_DECREF(plstr);
				plstr = NULL;
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
			elog(ERROR, "SPI_modifytuple failed -- error %d", SPI_result);
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

	return rtup;
}

static PyObject *
PLy_trigger_build_args(FunctionCallInfo fcinfo, PLyProcedure * proc, HeapTuple *rv)
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
			PLy_elog(ERROR, "could not build arguments for trigger procedure");

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



/* function handler and friends */
static Datum
PLy_function_handler(FunctionCallInfo fcinfo, PLyProcedure * proc)
{
	Datum		rv;
	PyObject   *volatile plargs = NULL;
	PyObject   *volatile plrv = NULL;
	PyObject   *volatile plrv_so = NULL;
	char	   *plrv_sc;

	PG_TRY();
	{
		if (!proc->is_setof || proc->setof == NULL)
		{
			/* Simple type returning function or first time for SETOF function */
			plargs = PLy_function_build_args(fcinfo, proc);
			plrv = PLy_procedure_call(proc, "args", plargs);
			if (!proc->is_setof)

				/*
				 * SETOF function parameters will be deleted when last row is
				 * returned
				 */
				PLy_function_delete_args(proc);
			Assert(plrv != NULL);
			Assert(!PLy_error_in_progress);
		}

		/*
		 * Disconnect from SPI manager and then create the return values datum
		 * (if the input function does a palloc for it this must not be
		 * allocated in the SPI memory context because SPI_finish would free
		 * it).
		 */
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");

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
							 errmsg("only value per call is allowed")));
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
					errdetail("SETOF must be returned as iterable object")));
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
				Py_XDECREF(plrv_so);

				PLy_function_delete_args(proc);

				if (has_error)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_EXCEPTION),
						  errmsg("error fetching next item from iterator")));

				fcinfo->isnull = true;
				return (Datum) NULL;
			}
		}

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
					   errmsg("invalid return value from plpython function"),
						 errdetail("Functions returning type \"void\" must return None.")));

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
			HeapTuple	tuple = NULL;

			if (PySequence_Check(plrv))
				/* composite type as sequence (tuple, list etc) */
				tuple = PLySequence_ToTuple(&proc->result, plrv);
			else if (PyMapping_Check(plrv))
				/* composite type as mapping (currently only dict) */
				tuple = PLyMapping_ToTuple(&proc->result, plrv);
			else
				/* returned as smth, must provide method __getattr__(name) */
				tuple = PLyObject_ToTuple(&proc->result, plrv);

			if (tuple != NULL)
			{
				fcinfo->isnull = false;
				rv = HeapTupleGetDatum(tuple);
			}
			else
			{
				fcinfo->isnull = true;
				rv = (Datum) NULL;
			}
		}
		else
		{
			fcinfo->isnull = false;
			plrv_so = PyObject_Str(plrv);
			if (!plrv_so)
				PLy_elog(ERROR, "function \"%s\" could not create return value", proc->proname);
			plrv_sc = PyString_AsString(plrv_so);
			rv = InputFunctionCall(&proc->result.out.d.typfunc,
								   plrv_sc,
								   proc->result.out.d.typioparam,
								   -1);
		}
	}
	PG_CATCH();
	{
		Py_XDECREF(plargs);
		Py_XDECREF(plrv);
		Py_XDECREF(plrv_so);

		PG_RE_THROW();
	}
	PG_END_TRY();

	Py_XDECREF(plargs);
	Py_DECREF(plrv);
	Py_XDECREF(plrv_so);

	return rv;
}

static PyObject *
PLy_procedure_call(PLyProcedure * proc, char *kargs, PyObject * vargs)
{
	PyObject   *rv;

	PyDict_SetItemString(proc->globals, kargs, vargs);
	rv = PyEval_EvalCode((PyCodeObject *) proc->code,
						 proc->globals, proc->globals);

	/*
	 * If there was an error in a PG callback, propagate that no matter what
	 * Python claims about its success.
	 */
	if (PLy_error_in_progress)
	{
		ErrorData  *edata = PLy_error_in_progress;

		PLy_error_in_progress = NULL;
		ReThrowError(edata);
	}

	if (rv == NULL || PyErr_Occurred())
	{
		Py_XDECREF(rv);
		PLy_elog(ERROR, "function \"%s\" failed", proc->proname);
	}

	return rv;
}

static PyObject *
PLy_function_build_args(FunctionCallInfo fcinfo, PLyProcedure * proc)
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
					char	   *ct;

					ct = OutputFunctionCall(&(proc->args[i].in.d.typfunc),
											fcinfo->arg[i]);
					arg = (proc->args[i].in.d.func) (ct);
					pfree(ct);
				}
			}

			if (arg == NULL)
			{
				Py_INCREF(Py_None);
				arg = Py_None;
			}

			if (PyList_SetItem(args, i, arg) == -1 ||
				(proc->argnames &&
				 PyDict_SetItemString(proc->globals, proc->argnames[i], arg) == -1))
				PLy_elog(ERROR, "problem setting up arguments for \"%s\"", proc->proname);
			arg = NULL;
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
PLy_function_delete_args(PLyProcedure * proc)
{
	int			i;

	if (!proc->argnames)
		return;

	for (i = 0; i < proc->nargs; i++)
		PyDict_DelItemString(proc->globals, proc->argnames[i]);
}


/*
 * PLyProcedure functions
 */

/* PLy_procedure_get: returns a cached PLyProcedure, or creates, stores and
 * returns a new PLyProcedure.	fcinfo is the call info, tgreloid is the
 * relation OID when calling a trigger, or InvalidOid (zero) for ordinary
 * function calls.
 */
static PLyProcedure *
PLy_procedure_get(FunctionCallInfo fcinfo, Oid tgreloid)
{
	Oid			fn_oid;
	HeapTuple	procTup;
	char		key[128];
	PyObject   *plproc;
	PLyProcedure *proc = NULL;
	int			rv;

	fn_oid = fcinfo->flinfo->fn_oid;
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(fn_oid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	rv = snprintf(key, sizeof(key), "%u_%u", fn_oid, tgreloid);
	if (rv >= sizeof(key) || rv < 0)
		elog(ERROR, "key too long");

	plproc = PyDict_GetItemString(PLy_procedure_cache, key);

	if (plproc != NULL)
	{
		Py_INCREF(plproc);
		if (!PyCObject_Check(plproc))
			elog(FATAL, "expected a PyCObject, didn't get one");

		proc = PyCObject_AsVoidPtr(plproc);
		if (proc->me != plproc)
			elog(FATAL, "proc->me != plproc");
		/* did we find an up-to-date cache entry? */
		if (proc->fn_xmin != HeapTupleHeaderGetXmin(procTup->t_data) ||
			!ItemPointerEquals(&proc->fn_tid, &procTup->t_self))
		{
			Py_DECREF(plproc);
			proc = NULL;
		}
	}

	if (proc == NULL)
		proc = PLy_procedure_create(procTup, tgreloid, key);

	if (OidIsValid(tgreloid))
	{
		/*
		 * Input/output conversion for trigger tuples.	Use the result
		 * TypeInfo variable to store the tuple conversion info.  We
		 * do this over again on each call to cover the possibility that
		 * the relation's tupdesc changed since the trigger was last called.
		 * PLy_input_tuple_funcs and PLy_output_tuple_funcs are responsible
		 * for not doing repetitive work.
		 */
		TriggerData *tdata = (TriggerData *) fcinfo->context;

		Assert(CALLED_AS_TRIGGER(fcinfo));
		PLy_input_tuple_funcs(&(proc->result), tdata->tg_relation->rd_att);
		PLy_output_tuple_funcs(&(proc->result), tdata->tg_relation->rd_att);
	}

	ReleaseSysCache(procTup);

	return proc;
}

static PLyProcedure *
PLy_procedure_create(HeapTuple procTup, Oid tgreloid, char *key)
{
	char		procName[NAMEDATALEN + 256];
	Form_pg_proc procStruct;
	PLyProcedure *volatile proc;
	char	   *volatile procSource = NULL;
	Datum		prosrcdatum;
	bool		isnull;
	int			i,
				rv;
	Datum		argnames;
	Datum	   *elems;
	int			nelems;

	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	if (OidIsValid(tgreloid))
		rv = snprintf(procName, sizeof(procName),
					  "__plpython_procedure_%s_%u_trigger_%u",
					  NameStr(procStruct->proname),
					  HeapTupleGetOid(procTup),
					  tgreloid);
	else
		rv = snprintf(procName, sizeof(procName),
					  "__plpython_procedure_%s_%u",
					  NameStr(procStruct->proname),
					  HeapTupleGetOid(procTup));
	if (rv >= sizeof(procName) || rv < 0)
		elog(ERROR, "procedure name would overrun buffer");

	proc = PLy_malloc(sizeof(PLyProcedure));
	proc->proname = PLy_strdup(NameStr(procStruct->proname));
	proc->pyname = PLy_strdup(procName);
	proc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	proc->fn_tid = procTup->t_self;
	/* Remember if function is STABLE/IMMUTABLE */
	proc->fn_readonly =
		(procStruct->provolatile != PROVOLATILE_VOLATILE);
	PLy_typeinfo_init(&proc->result);
	for (i = 0; i < FUNC_MAX_ARGS; i++)
		PLy_typeinfo_init(&proc->args[i]);
	proc->nargs = 0;
	proc->code = proc->statics = NULL;
	proc->globals = proc->me = NULL;
	proc->is_setof = procStruct->proretset;
	proc->setof = NULL;
	proc->argnames = NULL;

	PG_TRY();
	{
		/*
		 * get information required for output conversion of the return value,
		 * but only if this isn't a trigger.
		 */
		if (!OidIsValid(tgreloid))
		{
			HeapTuple	rvTypeTup;
			Form_pg_type rvTypeStruct;

			rvTypeTup = SearchSysCache(TYPEOID,
									ObjectIdGetDatum(procStruct->prorettype),
									   0, 0, 0);
			if (!HeapTupleIsValid(rvTypeTup))
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			rvTypeStruct = (Form_pg_type) GETSTRUCT(rvTypeTup);

			/* Disallow pseudotype result, except for void */
			if (rvTypeStruct->typtype == TYPTYPE_PSEUDO &&
				procStruct->prorettype != VOIDOID)
			{
				if (procStruct->prorettype == TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						   errmsg("plpython functions cannot return type %s",
								  format_type_be(procStruct->prorettype))));
			}

			if (rvTypeStruct->typtype == TYPTYPE_COMPOSITE)
			{
				/*
				 * Tuple: set up later, during first call to
				 * PLy_function_handler
				 */
				proc->result.out.d.typoid = procStruct->prorettype;
				proc->result.is_rowtype = 2;
			}
			else
				PLy_output_datum_func(&proc->result, rvTypeTup);

			ReleaseSysCache(rvTypeTup);
		}

		/*
		 * now get information required for input conversion of the
		 * procedure's arguments.
		 */
		proc->nargs = procStruct->pronargs;
		if (proc->nargs)
		{
			argnames = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_proargnames, &isnull);
			if (!isnull)
			{
				/* XXX this code is WRONG if there are any output arguments */
				deconstruct_array(DatumGetArrayTypeP(argnames), TEXTOID, -1, false, 'i',
								  &elems, NULL, &nelems);
				if (nelems != proc->nargs)
					elog(ERROR,
						 "proargnames must have the same number of elements "
						 "as the function has arguments");
				proc->argnames = (char **) PLy_malloc(sizeof(char *) * proc->nargs);
				memset(proc->argnames, 0, sizeof(char *) * proc->nargs);
			}
		}
		for (i = 0; i < proc->nargs; i++)
		{
			HeapTuple	argTypeTup;
			Form_pg_type argTypeStruct;

			argTypeTup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(procStruct->proargtypes.values[i]),
										0, 0, 0);
			if (!HeapTupleIsValid(argTypeTup))
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->proargtypes.values[i]);
			argTypeStruct = (Form_pg_type) GETSTRUCT(argTypeTup);

			/* Disallow pseudotype argument */
			if (argTypeStruct->typtype == TYPTYPE_PSEUDO)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("plpython functions cannot take type %s",
						format_type_be(procStruct->proargtypes.values[i]))));

			if (argTypeStruct->typtype != TYPTYPE_COMPOSITE)
				PLy_input_datum_func(&(proc->args[i]),
									 procStruct->proargtypes.values[i],
									 argTypeTup);
			else
				proc->args[i].is_rowtype = 2;	/* still need to set I/O funcs */

			ReleaseSysCache(argTypeTup);

			/* Fetch argument name */
			if (proc->argnames)
				proc->argnames[i] = PLy_strdup(DatumGetCString(DirectFunctionCall1(textout, elems[i])));
		}

		/*
		 * get the text of the function.
		 */
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
		procSource = DatumGetCString(DirectFunctionCall1(textout,
														 prosrcdatum));

		PLy_procedure_compile(proc, procSource);

		pfree(procSource);

		proc->me = PyCObject_FromVoidPtr(proc, NULL);
		PyDict_SetItemString(PLy_procedure_cache, key, proc->me);
	}
	PG_CATCH();
	{
		PLy_procedure_delete(proc);
		if (procSource)
			pfree(procSource);

		PG_RE_THROW();
	}
	PG_END_TRY();

	return proc;
}

static void
PLy_procedure_compile(PLyProcedure * proc, const char *src)
{
	PyObject   *crv = NULL;
	char	   *msrc;

	proc->globals = PyDict_Copy(PLy_interp_globals);

	/*
	 * SD is private preserved data between calls. GD is global data shared by
	 * all functions
	 */
	proc->statics = PyDict_New();
	PyDict_SetItemString(proc->globals, "SD", proc->statics);

	/*
	 * insert the function code into the interpreter
	 */
	msrc = PLy_procedure_munge_source(proc->pyname, src);
	crv = PyRun_String(msrc, Py_file_input, proc->globals, NULL);
	free(msrc);

	if (crv != NULL && (!PyErr_Occurred()))
	{
		int			clen;
		char		call[NAMEDATALEN + 256];

		Py_DECREF(crv);

		/*
		 * compile a call to the function
		 */
		clen = snprintf(call, sizeof(call), "%s()", proc->pyname);
		if (clen < 0 || clen >= sizeof(call))
			elog(ERROR, "string would overflow buffer");
		proc->code = Py_CompileString(call, "<string>", Py_eval_input);
		if (proc->code != NULL && (!PyErr_Occurred()))
			return;
	}
	else
		Py_XDECREF(crv);

	PLy_elog(ERROR, "could not compile function \"%s\"", proc->proname);
}

static char *
PLy_procedure_munge_source(const char *name, const char *src)
{
	char	   *mrc,
			   *mp;
	const char *sp;
	size_t		mlen,
				plen;

	/*
	 * room for function source and the def statement
	 */
	mlen = (strlen(src) * 2) + strlen(name) + 16;

	mrc = PLy_malloc(mlen);
	plen = snprintf(mrc, mlen, "def %s():\n\t", name);
	Assert(plen >= 0 && plen < mlen);

	sp = src;
	mp = mrc + plen;

	while (*sp != '\0')
	{
		if (*sp == '\r' && *(sp + 1) == '\n')
			sp++;

		if (*sp == '\n' || *sp == '\r')
		{
			*mp++ = '\n';
			*mp++ = '\t';
			sp++;
		}
		else
			*mp++ = *sp++;
	}
	*mp++ = '\n';
	*mp++ = '\n';
	*mp = '\0';

	if (mp > (mrc + mlen))
		elog(FATAL, "buffer overrun in PLy_munge_source");

	return mrc;
}

static void
PLy_procedure_delete(PLyProcedure * proc)
{
	int			i;

	Py_XDECREF(proc->code);
	Py_XDECREF(proc->statics);
	Py_XDECREF(proc->globals);
	Py_XDECREF(proc->me);
	if (proc->proname)
		PLy_free(proc->proname);
	if (proc->pyname)
		PLy_free(proc->pyname);
	for (i = 0; i < proc->nargs; i++)
	{
		if (proc->args[i].is_rowtype == 1)
		{
			if (proc->args[i].in.r.atts)
				PLy_free(proc->args[i].in.r.atts);
			if (proc->args[i].out.r.atts)
				PLy_free(proc->args[i].out.r.atts);
		}
		if (proc->argnames && proc->argnames[i])
			PLy_free(proc->argnames[i]);
	}
	if (proc->argnames)
		PLy_free(proc->argnames);
}

/* conversion functions.  remember output from python is
 * input to postgresql, and vis versa.
 */
static void
PLy_input_tuple_funcs(PLyTypeInfo * arg, TupleDesc desc)
{
	int			i;

	if (arg->is_rowtype == 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for a Datum");
	arg->is_rowtype = 1;

	if (arg->in.r.natts != desc->natts)
	{
		if (arg->in.r.atts)
			PLy_free(arg->in.r.atts);
		arg->in.r.natts = desc->natts;
		arg->in.r.atts = PLy_malloc0(desc->natts * sizeof(PLyDatumToOb));
	}

	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typeTup;

		if (desc->attrs[i]->attisdropped)
			continue;

		if (arg->in.r.atts[i].typoid == desc->attrs[i]->atttypid)
			continue;			/* already set up this entry */

		typeTup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(desc->attrs[i]->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "cache lookup failed for type %u",
				 desc->attrs[i]->atttypid);

		PLy_input_datum_func2(&(arg->in.r.atts[i]),
							  desc->attrs[i]->atttypid,
							  typeTup);

		ReleaseSysCache(typeTup);
	}
}

static void
PLy_output_tuple_funcs(PLyTypeInfo * arg, TupleDesc desc)
{
	int			i;

	if (arg->is_rowtype == 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for a Datum");
	arg->is_rowtype = 1;

	if (arg->out.r.natts != desc->natts)
	{
		if (arg->out.r.atts)
			PLy_free(arg->out.r.atts);
		arg->out.r.natts = desc->natts;
		arg->out.r.atts = PLy_malloc0(desc->natts * sizeof(PLyDatumToOb));
	}

	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typeTup;

		if (desc->attrs[i]->attisdropped)
			continue;

		if (arg->out.r.atts[i].typoid == desc->attrs[i]->atttypid)
			continue;			/* already set up this entry */

		typeTup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(desc->attrs[i]->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "cache lookup failed for type %u",
				 desc->attrs[i]->atttypid);

		PLy_output_datum_func2(&(arg->out.r.atts[i]), typeTup);

		ReleaseSysCache(typeTup);
	}
}

static void
PLy_output_datum_func(PLyTypeInfo * arg, HeapTuple typeTup)
{
	if (arg->is_rowtype > 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for a Tuple");
	arg->is_rowtype = 0;
	PLy_output_datum_func2(&(arg->out.d), typeTup);
}

static void
PLy_output_datum_func2(PLyObToDatum * arg, HeapTuple typeTup)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

	perm_fmgr_info(typeStruct->typinput, &arg->typfunc);
	arg->typoid = HeapTupleGetOid(typeTup);
	arg->typioparam = getTypeIOParam(typeTup);
	arg->typbyval = typeStruct->typbyval;
}

static void
PLy_input_datum_func(PLyTypeInfo * arg, Oid typeOid, HeapTuple typeTup)
{
	if (arg->is_rowtype > 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for Tuple");
	arg->is_rowtype = 0;
	PLy_input_datum_func2(&(arg->in.d), typeOid, typeTup);
}

static void
PLy_input_datum_func2(PLyDatumToOb * arg, Oid typeOid, HeapTuple typeTup)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

	/* Get the type's conversion information */
	perm_fmgr_info(typeStruct->typoutput, &arg->typfunc);
	arg->typoid = HeapTupleGetOid(typeTup);
	arg->typioparam = getTypeIOParam(typeTup);
	arg->typbyval = typeStruct->typbyval;

	/* Determine which kind of Python object we will convert to */
	switch (typeOid)
	{
		case BOOLOID:
			arg->func = PLyBool_FromString;
			break;
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			arg->func = PLyFloat_FromString;
			break;
		case INT2OID:
		case INT4OID:
			arg->func = PLyInt_FromString;
			break;
		case INT8OID:
			arg->func = PLyLong_FromString;
			break;
		default:
			arg->func = PLyString_FromString;
			break;
	}
}

static void
PLy_typeinfo_init(PLyTypeInfo * arg)
{
	arg->is_rowtype = -1;
	arg->in.r.natts = arg->out.r.natts = 0;
	arg->in.r.atts = NULL;
	arg->out.r.atts = NULL;
}

static void
PLy_typeinfo_dealloc(PLyTypeInfo * arg)
{
	if (arg->is_rowtype == 1)
	{
		if (arg->in.r.atts)
			PLy_free(arg->in.r.atts);
		if (arg->out.r.atts)
			PLy_free(arg->out.r.atts);
	}
}

/* assumes that a bool is always returned as a 't' or 'f' */
static PyObject *
PLyBool_FromString(const char *src)
{
	/*
	 * We would like to use Py_RETURN_TRUE and Py_RETURN_FALSE here for
	 * generating SQL from trigger functions, but those are only supported in
	 * Python >= 2.3, and we support older versions.
	 * http://docs.python.org/api/boolObjects.html
	 */
	if (src[0] == 't')
		return PyBool_FromLong(1);
	return PyBool_FromLong(0);
}

static PyObject *
PLyFloat_FromString(const char *src)
{
	double		v;
	char	   *eptr;

	errno = 0;
	v = strtod(src, &eptr);
	if (*eptr != '\0' || errno)
		return NULL;
	return PyFloat_FromDouble(v);
}

static PyObject *
PLyInt_FromString(const char *src)
{
	long		v;
	char	   *eptr;

	errno = 0;
	v = strtol(src, &eptr, 0);
	if (*eptr != '\0' || errno)
		return NULL;
	return PyInt_FromLong(v);
}

static PyObject *
PLyLong_FromString(const char *src)
{
	return PyLong_FromString((char *) src, NULL, 0);
}

static PyObject *
PLyString_FromString(const char *src)
{
	return PyString_FromString(src);
}

static PyObject *
PLyDict_FromTuple(PLyTypeInfo * info, HeapTuple tuple, TupleDesc desc)
{
	PyObject   *volatile dict;
	int			i;

	if (info->is_rowtype != 1)
		elog(ERROR, "PLyTypeInfo structure describes a datum");

	dict = PyDict_New();
	if (dict == NULL)
		PLy_elog(ERROR, "could not create tuple dictionary");

	PG_TRY();
	{
		for (i = 0; i < info->in.r.natts; i++)
		{
			char	   *key,
					   *vsrc;
			Datum		vattr;
			bool		is_null;
			PyObject   *value;

			if (desc->attrs[i]->attisdropped)
				continue;

			key = NameStr(desc->attrs[i]->attname);
			vattr = heap_getattr(tuple, (i + 1), desc, &is_null);

			if (is_null || info->in.r.atts[i].func == NULL)
				PyDict_SetItemString(dict, key, Py_None);
			else
			{
				vsrc = OutputFunctionCall(&info->in.r.atts[i].typfunc,
										  vattr);

				/*
				 * no exceptions allowed
				 */
				value = info->in.r.atts[i].func(vsrc);
				pfree(vsrc);
				PyDict_SetItemString(dict, key, value);
				Py_DECREF(value);
			}
		}
	}
	PG_CATCH();
	{
		Py_DECREF(dict);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return dict;
}


static HeapTuple
PLyMapping_ToTuple(PLyTypeInfo * info, PyObject * mapping)
{
	TupleDesc	desc;
	HeapTuple	tuple;
	Datum	   *values;
	char	   *nulls;
	volatile int i;

	Assert(PyMapping_Check(mapping));

	desc = lookup_rowtype_tupdesc(info->out.d.typoid, -1);
	if (info->is_rowtype == 2)
		PLy_output_tuple_funcs(info, desc);
	Assert(info->is_rowtype == 1);

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(char) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		char	   *key;
		PyObject   *volatile value,
				   *volatile so;

		key = NameStr(desc->attrs[i]->attname);
		value = so = NULL;
		PG_TRY();
		{
			value = PyMapping_GetItemString(mapping, key);
			if (value == Py_None)
			{
				values[i] = (Datum) NULL;
				nulls[i] = 'n';
			}
			else if (value)
			{
				char	   *valuestr;

				so = PyObject_Str(value);
				if (so == NULL)
					PLy_elog(ERROR, "cannot convert mapping type");
				valuestr = PyString_AsString(so);

				values[i] = InputFunctionCall(&info->out.r.atts[i].typfunc
											  ,valuestr
											  ,info->out.r.atts[i].typioparam
											  ,-1);
				Py_DECREF(so);
				so = NULL;
				nulls[i] = ' ';
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("no mapping found with key \"%s\"", key),
						 errhint("to return null in specific column, "
					  "add value None to map with key named after column")));

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(so);
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_formtuple(desc, values, nulls);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);

	return tuple;
}


static HeapTuple
PLySequence_ToTuple(PLyTypeInfo * info, PyObject * sequence)
{
	TupleDesc	desc;
	HeapTuple	tuple;
	Datum	   *values;
	char	   *nulls;
	volatile int i;

	Assert(PySequence_Check(sequence));

	/*
	 * Check that sequence length is exactly same as PG tuple's. We actually
	 * can ignore exceeding items or assume missing ones as null but to avoid
	 * plpython developer's errors we are strict here
	 */
	desc = lookup_rowtype_tupdesc(info->out.d.typoid, -1);
	if (PySequence_Length(sequence) != desc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		errmsg("returned sequence's length must be same as tuple's length")));

	if (info->is_rowtype == 2)
		PLy_output_tuple_funcs(info, desc);
	Assert(info->is_rowtype == 1);

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(char) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		PyObject   *volatile value,
				   *volatile so;

		value = so = NULL;
		PG_TRY();
		{
			value = PySequence_GetItem(sequence, i);
			Assert(value);
			if (value == Py_None)
			{
				values[i] = (Datum) NULL;
				nulls[i] = 'n';
			}
			else if (value)
			{
				char	   *valuestr;

				so = PyObject_Str(value);
				if (so == NULL)
					PLy_elog(ERROR, "cannot convert sequence type");
				valuestr = PyString_AsString(so);
				values[i] = InputFunctionCall(&info->out.r.atts[i].typfunc
											  ,valuestr
											  ,info->out.r.atts[i].typioparam
											  ,-1);
				Py_DECREF(so);
				so = NULL;
				nulls[i] = ' ';
			}

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(so);
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_formtuple(desc, values, nulls);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);

	return tuple;
}


static HeapTuple
PLyObject_ToTuple(PLyTypeInfo * info, PyObject * object)
{
	TupleDesc	desc;
	HeapTuple	tuple;
	Datum	   *values;
	char	   *nulls;
	volatile int i;

	desc = lookup_rowtype_tupdesc(info->out.d.typoid, -1);
	if (info->is_rowtype == 2)
		PLy_output_tuple_funcs(info, desc);
	Assert(info->is_rowtype == 1);

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(char) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		char	   *key;
		PyObject   *volatile value,
				   *volatile so;

		key = NameStr(desc->attrs[i]->attname);
		value = so = NULL;
		PG_TRY();
		{
			value = PyObject_GetAttrString(object, key);
			if (value == Py_None)
			{
				values[i] = (Datum) NULL;
				nulls[i] = 'n';
			}
			else if (value)
			{
				char	   *valuestr;

				so = PyObject_Str(value);
				if (so == NULL)
					PLy_elog(ERROR, "cannot convert object type");
				valuestr = PyString_AsString(so);
				values[i] = InputFunctionCall(&info->out.r.atts[i].typfunc
											  ,valuestr
											  ,info->out.r.atts[i].typioparam
											  ,-1);
				Py_DECREF(so);
				so = NULL;
				nulls[i] = ' ';
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("no attribute named \"%s\"", key),
						 errhint("to return null in specific column, "
							   "let returned object to have attribute named "
								 "after column with value None")));

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(so);
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_formtuple(desc, values, nulls);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);

	return tuple;
}


/* initialization, some python variables function declared here */

/* interface to postgresql elog */
static PyObject *PLy_debug(PyObject *, PyObject *);
static PyObject *PLy_log(PyObject *, PyObject *);
static PyObject *PLy_info(PyObject *, PyObject *);
static PyObject *PLy_notice(PyObject *, PyObject *);
static PyObject *PLy_warning(PyObject *, PyObject *);
static PyObject *PLy_error(PyObject *, PyObject *);
static PyObject *PLy_fatal(PyObject *, PyObject *);

/* PLyPlanObject, PLyResultObject and SPI interface */
#define is_PLyPlanObject(x) ((x)->ob_type == &PLy_PlanType)
static PyObject *PLy_plan_new(void);
static void PLy_plan_dealloc(PyObject *);
static PyObject *PLy_plan_getattr(PyObject *, char *);
static PyObject *PLy_plan_status(PyObject *, PyObject *);

static PyObject *PLy_result_new(void);
static void PLy_result_dealloc(PyObject *);
static PyObject *PLy_result_getattr(PyObject *, char *);
static PyObject *PLy_result_nrows(PyObject *, PyObject *);
static PyObject *PLy_result_status(PyObject *, PyObject *);
static Py_ssize_t PLy_result_length(PyObject *);
static PyObject *PLy_result_item(PyObject *, Py_ssize_t);
static PyObject *PLy_result_slice(PyObject *, Py_ssize_t, Py_ssize_t);
static int	PLy_result_ass_item(PyObject *, Py_ssize_t, PyObject *);
static int	PLy_result_ass_slice(PyObject *, Py_ssize_t, Py_ssize_t, PyObject *);


static PyObject *PLy_spi_prepare(PyObject *, PyObject *);
static PyObject *PLy_spi_execute(PyObject *, PyObject *);
static PyObject *PLy_spi_execute_query(char *query, long limit);
static PyObject *PLy_spi_execute_plan(PyObject *, PyObject *, long);
static PyObject *PLy_spi_execute_fetch_result(SPITupleTable *, int, int);


static PyTypeObject PLy_PlanType = {
	PyObject_HEAD_INIT(NULL)
	0,							/* ob_size */
	"PLyPlan",					/* tp_name */
	sizeof(PLyPlanObject),		/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_plan_dealloc,			/* tp_dealloc */
	0,							/* tp_print */
	PLy_plan_getattr,			/* tp_getattr */
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
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	PLy_plan_doc,				/* tp_doc */
};

static PyMethodDef PLy_plan_methods[] = {
	{"status", PLy_plan_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PySequenceMethods PLy_result_as_sequence = {
	PLy_result_length,			/* sq_length */
	NULL,						/* sq_concat */
	NULL,						/* sq_repeat */
	PLy_result_item,			/* sq_item */
	PLy_result_slice,			/* sq_slice */
	PLy_result_ass_item,		/* sq_ass_item */
	PLy_result_ass_slice,		/* sq_ass_slice */
};

static PyTypeObject PLy_ResultType = {
	PyObject_HEAD_INIT(NULL)
	0,							/* ob_size */
	"PLyResult",				/* tp_name */
	sizeof(PLyResultObject),	/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_result_dealloc,			/* tp_dealloc */
	0,							/* tp_print */
	PLy_result_getattr,			/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	&PLy_result_as_sequence,	/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
	0,							/* tp_call */
	0,							/* tp_str */
	0,							/* tp_getattro */
	0,							/* tp_setattro */
	0,							/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	PLy_result_doc,				/* tp_doc */
};

static PyMethodDef PLy_result_methods[] = {
	{"nrows", PLy_result_nrows, METH_VARARGS, NULL},
	{"status", PLy_result_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyMethodDef PLy_methods[] = {
	/*
	 * logging methods
	 */
	{"debug", PLy_debug, METH_VARARGS, NULL},
	{"log", PLy_log, METH_VARARGS, NULL},
	{"info", PLy_info, METH_VARARGS, NULL},
	{"notice", PLy_notice, METH_VARARGS, NULL},
	{"warning", PLy_warning, METH_VARARGS, NULL},
	{"error", PLy_error, METH_VARARGS, NULL},
	{"fatal", PLy_fatal, METH_VARARGS, NULL},

	/*
	 * create a stored plan
	 */
	{"prepare", PLy_spi_prepare, METH_VARARGS, NULL},

	/*
	 * execute a plan or query
	 */
	{"execute", PLy_spi_execute, METH_VARARGS, NULL},

	{NULL, NULL, 0, NULL}
};


/* plan object methods */
static PyObject *
PLy_plan_new(void)
{
	PLyPlanObject *ob;

	if ((ob = PyObject_NEW(PLyPlanObject, &PLy_PlanType)) == NULL)
		return NULL;

	ob->plan = NULL;
	ob->nargs = 0;
	ob->types = NULL;
	ob->args = NULL;

	return (PyObject *) ob;
}


static void
PLy_plan_dealloc(PyObject * arg)
{
	PLyPlanObject *ob = (PLyPlanObject *) arg;

	if (ob->plan)
		SPI_freeplan(ob->plan);
	if (ob->types)
		PLy_free(ob->types);
	if (ob->args)
	{
		int			i;

		for (i = 0; i < ob->nargs; i++)
			PLy_typeinfo_dealloc(&ob->args[i]);
		PLy_free(ob->args);
	}

	arg->ob_type->tp_free(arg);
}


static PyObject *
PLy_plan_getattr(PyObject * self, char *name)
{
	return Py_FindMethod(PLy_plan_methods, self, name);
}

static PyObject *
PLy_plan_status(PyObject * self, PyObject * args)
{
	if (PyArg_ParseTuple(args, ""))
	{
		Py_INCREF(Py_True);
		return Py_True;
		/* return PyInt_FromLong(self->status); */
	}
	PyErr_SetString(PLy_exc_error, "plan.status() takes no arguments");
	return NULL;
}



/* result object methods */

static PyObject *
PLy_result_new(void)
{
	PLyResultObject *ob;

	if ((ob = PyObject_NEW(PLyResultObject, &PLy_ResultType)) == NULL)
		return NULL;

	/* ob->tuples = NULL; */

	Py_INCREF(Py_None);
	ob->status = Py_None;
	ob->nrows = PyInt_FromLong(-1);
	ob->rows = PyList_New(0);

	return (PyObject *) ob;
}

static void
PLy_result_dealloc(PyObject * arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	Py_XDECREF(ob->nrows);
	Py_XDECREF(ob->rows);
	Py_XDECREF(ob->status);

	arg->ob_type->tp_free(arg);
}

static PyObject *
PLy_result_getattr(PyObject * self, char *name)
{
	return Py_FindMethod(PLy_result_methods, self, name);
}

static PyObject *
PLy_result_nrows(PyObject * self, PyObject * args)
{
	PLyResultObject *ob = (PLyResultObject *) self;

	Py_INCREF(ob->nrows);
	return ob->nrows;
}

static PyObject *
PLy_result_status(PyObject * self, PyObject * args)
{
	PLyResultObject *ob = (PLyResultObject *) self;

	Py_INCREF(ob->status);
	return ob->status;
}

static Py_ssize_t
PLy_result_length(PyObject * arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	return PyList_Size(ob->rows);
}

static PyObject *
PLy_result_item(PyObject * arg, Py_ssize_t idx)
{
	PyObject   *rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	rv = PyList_GetItem(ob->rows, idx);
	if (rv != NULL)
		Py_INCREF(rv);
	return rv;
}

static int
PLy_result_ass_item(PyObject * arg, Py_ssize_t idx, PyObject * item)
{
	int			rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	Py_INCREF(item);
	rv = PyList_SetItem(ob->rows, idx, item);
	return rv;
}

static PyObject *
PLy_result_slice(PyObject * arg, Py_ssize_t lidx, Py_ssize_t hidx)
{
	PyObject   *rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	rv = PyList_GetSlice(ob->rows, lidx, hidx);
	if (rv == NULL)
		return NULL;
	Py_INCREF(rv);
	return rv;
}

static int
PLy_result_ass_slice(PyObject * arg, Py_ssize_t lidx, Py_ssize_t hidx, PyObject * slice)
{
	int			rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	rv = PyList_SetSlice(ob->rows, lidx, hidx, slice);
	return rv;
}

/* SPI interface */
static PyObject *
PLy_spi_prepare(PyObject * self, PyObject * args)
{
	PLyPlanObject *plan;
	PyObject   *list = NULL;
	PyObject   *volatile optr = NULL;
	char	   *query;
	void	   *tmpplan;
	MemoryContext oldcontext;

	/* Can't execute more if we have an unhandled error */
	if (PLy_error_in_progress)
	{
		PyErr_SetString(PLy_exc_error, "Transaction aborted.");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|O", &query, &list))
	{
		PyErr_SetString(PLy_exc_spi_error,
						"Invalid arguments for plpy.prepare()");
		return NULL;
	}

	if (list && (!PySequence_Check(list)))
	{
		PyErr_SetString(PLy_exc_spi_error,
					 "Second argument in plpy.prepare() must be a sequence");
		return NULL;
	}

	if ((plan = (PLyPlanObject *) PLy_plan_new()) == NULL)
		return NULL;

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		if (list != NULL)
		{
			int			nargs,
						i;

			nargs = PySequence_Length(list);
			if (nargs > 0)
			{
				plan->nargs = nargs;
				plan->types = PLy_malloc(sizeof(Oid) * nargs);
				plan->values = PLy_malloc(sizeof(Datum) * nargs);
				plan->args = PLy_malloc(sizeof(PLyTypeInfo) * nargs);

				/*
				 * the other loop might throw an exception, if PLyTypeInfo
				 * member isn't properly initialized the Py_DECREF(plan) will
				 * go boom
				 */
				for (i = 0; i < nargs; i++)
				{
					PLy_typeinfo_init(&plan->args[i]);
					plan->values[i] = PointerGetDatum(NULL);
				}

				for (i = 0; i < nargs; i++)
				{
					char	   *sptr;
					HeapTuple	typeTup;
					Oid			typeId;
					int32		typmod;
					Form_pg_type typeStruct;

					optr = PySequence_GetItem(list, i);
					if (!PyString_Check(optr))
						elog(ERROR, "Type names must be strings.");
					sptr = PyString_AsString(optr);

					/********************************************************
					 * Resolve argument type names and then look them up by
					 * oid in the system cache, and remember the required
					 *information for input conversion.
					 ********************************************************/

					parseTypeString(sptr, &typeId, &typmod);

					typeTup = SearchSysCache(TYPEOID,
											 ObjectIdGetDatum(typeId),
											 0, 0, 0);
					if (!HeapTupleIsValid(typeTup))
						elog(ERROR, "cache lookup failed for type %u", typeId);

					Py_DECREF(optr);
					optr = NULL;	/* this is important */

					plan->types[i] = typeId;
					typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
					if (typeStruct->typtype != TYPTYPE_COMPOSITE)
						PLy_output_datum_func(&plan->args[i], typeTup);
					else
						elog(ERROR, "tuples not handled in plpy.prepare, yet.");
					ReleaseSysCache(typeTup);
				}
			}
		}

		plan->plan = SPI_prepare(query, plan->nargs, plan->types);
		if (plan->plan == NULL)
			elog(ERROR, "SPI_prepare failed: %s",
				 SPI_result_code_string(SPI_result));

		/* transfer plan from procCxt to topCxt */
		tmpplan = plan->plan;
		plan->plan = SPI_saveplan(tmpplan);
		SPI_freeplan(tmpplan);
		if (plan->plan == NULL)
			elog(ERROR, "SPI_saveplan failed: %s",
				 SPI_result_code_string(SPI_result));
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		PLy_error_in_progress = CopyErrorData();
		FlushErrorState();
		Py_DECREF(plan);
		Py_XDECREF(optr);
		if (!PyErr_Occurred())
			PyErr_SetString(PLy_exc_spi_error,
							"Unknown error in PLy_spi_prepare");
		/* XXX this oughta be replaced with errcontext mechanism */
		PLy_elog(WARNING, "in function %s:",
				 PLy_procedure_name(PLy_curr_procedure));
		return NULL;
	}
	PG_END_TRY();

	return (PyObject *) plan;
}

/* execute(query="select * from foo", limit=5)
 * execute(plan=plan, values=(foo, bar), limit=5)
 */
static PyObject *
PLy_spi_execute(PyObject * self, PyObject * args)
{
	char	   *query;
	PyObject   *plan;
	PyObject   *list = NULL;
	long		limit = 0;

	/* Can't execute more if we have an unhandled error */
	if (PLy_error_in_progress)
	{
		PyErr_SetString(PLy_exc_error, "Transaction aborted.");
		return NULL;
	}

	if (PyArg_ParseTuple(args, "s|l", &query, &limit))
		return PLy_spi_execute_query(query, limit);

	PyErr_Clear();

	if (PyArg_ParseTuple(args, "O|Ol", &plan, &list, &limit) &&
		is_PLyPlanObject(plan))
		return PLy_spi_execute_plan(plan, list, limit);

	PyErr_SetString(PLy_exc_error, "Expected a query or plan.");
	return NULL;
}

static PyObject *
PLy_spi_execute_plan(PyObject * ob, PyObject * list, long limit)
{
	volatile int nargs;
	int			i,
				rv;
	PLyPlanObject *plan;
	MemoryContext oldcontext;

	if (list != NULL)
	{
		if (!PySequence_Check(list) || PyString_Check(list))
		{
			char	   *msg = "plpy.execute() takes a sequence as its second argument";

			PyErr_SetString(PLy_exc_spi_error, msg);
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
			PLy_elog(ERROR, "function \"%s\" could not execute plan",
					 PLy_procedure_name(PLy_curr_procedure));
		sv = PyString_AsString(so);
		PLy_exception_set(PLy_exc_spi_error,
						  "Expected sequence of %d arguments, got %d. %s",
						  plan->nargs, nargs, sv);
		Py_DECREF(so);

		return NULL;
	}

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		char	   *nulls = palloc(nargs * sizeof(char));
		volatile int j;

		for (j = 0; j < nargs; j++)
		{
			PyObject   *elem,
					   *so;

			elem = PySequence_GetItem(list, j);
			if (elem != Py_None)
			{
				so = PyObject_Str(elem);
				if (!so)
					PLy_elog(ERROR, "function \"%s\" could not execute plan",
							 PLy_procedure_name(PLy_curr_procedure));
				Py_DECREF(elem);

				PG_TRY();
				{
					char	   *sv = PyString_AsString(so);

					plan->values[j] =
						InputFunctionCall(&(plan->args[j].out.d.typfunc),
										  sv,
										  plan->args[j].out.d.typioparam,
										  -1);
				}
				PG_CATCH();
				{
					Py_DECREF(so);
					PG_RE_THROW();
				}
				PG_END_TRY();

				Py_DECREF(so);
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
							  PLy_curr_procedure->fn_readonly, limit);

		pfree(nulls);
	}
	PG_CATCH();
	{
		int			k;

		MemoryContextSwitchTo(oldcontext);
		PLy_error_in_progress = CopyErrorData();
		FlushErrorState();

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

		if (!PyErr_Occurred())
			PyErr_SetString(PLy_exc_error,
							"Unknown error in PLy_spi_execute_plan");
		/* XXX this oughta be replaced with errcontext mechanism */
		PLy_elog(WARNING, "in function %s:",
				 PLy_procedure_name(PLy_curr_procedure));
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

	return PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);
}

static PyObject *
PLy_spi_execute_query(char *query, long limit)
{
	int			rv;
	MemoryContext oldcontext;

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		rv = SPI_execute(query, PLy_curr_procedure->fn_readonly, limit);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		PLy_error_in_progress = CopyErrorData();
		FlushErrorState();
		if (!PyErr_Occurred())
			PyErr_SetString(PLy_exc_spi_error,
							"Unknown error in PLy_spi_execute_query");
		/* XXX this oughta be replaced with errcontext mechanism */
		PLy_elog(WARNING, "in function %s:",
				 PLy_procedure_name(PLy_curr_procedure));
		return NULL;
	}
	PG_END_TRY();

	if (rv < 0)
	{
		PLy_exception_set(PLy_exc_spi_error,
						  "SPI_execute failed: %s",
						  SPI_result_code_string(rv));
		return NULL;
	}

	return PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);
}

static PyObject *
PLy_spi_execute_fetch_result(SPITupleTable *tuptable, int rows, int status)
{
	PLyResultObject *result;
	MemoryContext oldcontext;

	result = (PLyResultObject *) PLy_result_new();
	Py_DECREF(result->status);
	result->status = PyInt_FromLong(status);

	if (status > 0 && tuptable == NULL)
	{
		Py_DECREF(result->nrows);
		result->nrows = PyInt_FromLong(rows);
	}
	else if (status > 0 && tuptable != NULL)
	{
		PLyTypeInfo args;
		int			i;

		Py_DECREF(result->nrows);
		result->nrows = PyInt_FromLong(rows);
		PLy_typeinfo_init(&args);

		oldcontext = CurrentMemoryContext;
		PG_TRY();
		{
			if (rows)
			{
				Py_DECREF(result->rows);
				result->rows = PyList_New(rows);

				PLy_input_tuple_funcs(&args, tuptable->tupdesc);
				for (i = 0; i < rows; i++)
				{
					PyObject   *row = PLyDict_FromTuple(&args, tuptable->vals[i],
														tuptable->tupdesc);

					PyList_SetItem(result->rows, i, row);
				}
				PLy_typeinfo_dealloc(&args);

				SPI_freetuptable(tuptable);
			}
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(oldcontext);
			PLy_error_in_progress = CopyErrorData();
			FlushErrorState();
			if (!PyErr_Occurred())
				PyErr_SetString(PLy_exc_error,
							"Unknown error in PLy_spi_execute_fetch_result");
			Py_DECREF(result);
			PLy_typeinfo_dealloc(&args);
			return NULL;
		}
		PG_END_TRY();
	}

	return (PyObject *) result;
}


/*
 * language handler and interpreter initialization
 */

/*
 * _PG_init()			- library load-time initialization
 *
 * DO NOT make this static nor change its name!
 */
void
_PG_init(void)
{
	/* Be sure we do initialization only once (should be redundant now) */
	static bool inited = false;

	if (inited)
		return;

	Py_Initialize();
	PLy_init_interp();
	PLy_init_plpy();
	if (PyErr_Occurred())
		PLy_elog(FATAL, "untrapped error in initialization");
	PLy_procedure_cache = PyDict_New();
	if (PLy_procedure_cache == NULL)
		PLy_elog(ERROR, "could not create procedure cache");

	inited = true;
}

static void
PLy_init_interp(void)
{
	PyObject   *mainmod;

	mainmod = PyImport_AddModule("__main__");
	if (mainmod == NULL || PyErr_Occurred())
		PLy_elog(ERROR, "could not import \"__main__\" module.");
	Py_INCREF(mainmod);
	PLy_interp_globals = PyModule_GetDict(mainmod);
	PLy_interp_safe_globals = PyDict_New();
	PyDict_SetItemString(PLy_interp_globals, "GD", PLy_interp_safe_globals);
	Py_DECREF(mainmod);
	if (PLy_interp_globals == NULL || PyErr_Occurred())
		PLy_elog(ERROR, "could not initialize globals");
}

static void
PLy_init_plpy(void)
{
	PyObject   *main_mod,
			   *main_dict,
			   *plpy_mod;
	PyObject   *plpy,
			   *plpy_dict;

	/*
	 * initialize plpy module
	 */
	if (PyType_Ready(&PLy_PlanType) < 0)
		elog(ERROR, "could not init PLy_PlanType");
	if (PyType_Ready(&PLy_ResultType) < 0)
		elog(ERROR, "could not init PLy_ResultType");

	plpy = Py_InitModule("plpy", PLy_methods);
	plpy_dict = PyModule_GetDict(plpy);

	/* PyDict_SetItemString(plpy, "PlanType", (PyObject *) &PLy_PlanType); */

	PLy_exc_error = PyErr_NewException("plpy.Error", NULL, NULL);
	PLy_exc_fatal = PyErr_NewException("plpy.Fatal", NULL, NULL);
	PLy_exc_spi_error = PyErr_NewException("plpy.SPIError", NULL, NULL);
	PyDict_SetItemString(plpy_dict, "Error", PLy_exc_error);
	PyDict_SetItemString(plpy_dict, "Fatal", PLy_exc_fatal);
	PyDict_SetItemString(plpy_dict, "SPIError", PLy_exc_spi_error);

	/*
	 * initialize main module, and add plpy
	 */
	main_mod = PyImport_AddModule("__main__");
	main_dict = PyModule_GetDict(main_mod);
	plpy_mod = PyImport_AddModule("plpy");
	PyDict_SetItemString(main_dict, "plpy", plpy_mod);
	if (PyErr_Occurred())
		elog(ERROR, "could not init plpy");
}

/* the python interface to the elog function
 * don't confuse these with PLy_elog
 */
static PyObject *PLy_output(volatile int, PyObject *, PyObject *);

static PyObject *
PLy_debug(PyObject * self, PyObject * args)
{
	return PLy_output(DEBUG2, self, args);
}

static PyObject *
PLy_log(PyObject * self, PyObject * args)
{
	return PLy_output(LOG, self, args);
}

static PyObject *
PLy_info(PyObject * self, PyObject * args)
{
	return PLy_output(INFO, self, args);
}

static PyObject *
PLy_notice(PyObject * self, PyObject * args)
{
	return PLy_output(NOTICE, self, args);
}

static PyObject *
PLy_warning(PyObject * self, PyObject * args)
{
	return PLy_output(WARNING, self, args);
}

static PyObject *
PLy_error(PyObject * self, PyObject * args)
{
	return PLy_output(ERROR, self, args);
}

static PyObject *
PLy_fatal(PyObject * self, PyObject * args)
{
	return PLy_output(FATAL, self, args);
}


static PyObject *
PLy_output(volatile int level, PyObject * self, PyObject * args)
{
	PyObject   *so;
	char	   *volatile sv;
	MemoryContext oldcontext;

	so = PyObject_Str(args);
	if (so == NULL || ((sv = PyString_AsString(so)) == NULL))
	{
		level = ERROR;
		sv = "could not parse error message in `plpy.elog'";
	}

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		elog(level, "%s", sv);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		PLy_error_in_progress = CopyErrorData();
		FlushErrorState();
		Py_XDECREF(so);

		/*
		 * returning NULL here causes the python interpreter to bail. when
		 * control passes back to PLy_procedure_call, we check for PG
		 * exceptions and re-throw the error.
		 */
		PyErr_SetString(PLy_exc_error, sv);
		return NULL;
	}
	PG_END_TRY();

	Py_XDECREF(so);

	/*
	 * return a legal object so the interpreter will continue on its merry way
	 */
	Py_INCREF(Py_None);
	return Py_None;
}


/*
 * Get the name of the last procedure called by the backend (the
 * innermost, if a plpython procedure call calls the backend and the
 * backend calls another plpython procedure).
 *
 * NB: this returns the SQL name, not the internal Python procedure name
 */
static char *
PLy_procedure_name(PLyProcedure * proc)
{
	if (proc == NULL)
		return "<unknown procedure>";
	return proc->proname;
}

/* output a python traceback/exception via the postgresql elog
 * function.  not pretty.
 */
static void
PLy_exception_set(PyObject * exc, const char *fmt,...)
{
	char		buf[1024];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	PyErr_SetString(exc, buf);
}

/* Emit a PG error or notice, together with any available info about the
 * current Python error.  This should be used to propagate Python errors
 * into PG.
 */
static void
PLy_elog(int elevel, const char *fmt,...)
{
	char	   *xmsg;
	int			xlevel;
	StringInfoData emsg;

	xmsg = PLy_traceback(&xlevel);

	initStringInfo(&emsg);
	for (;;)
	{
		va_list		ap;
		bool		success;

		va_start(ap, fmt);
		success = appendStringInfoVA(&emsg, fmt, ap);
		va_end(ap);
		if (success)
			break;
		enlargeStringInfo(&emsg, emsg.maxlen);
	}

	PG_TRY();
	{
		ereport(elevel,
				(errmsg("plpython: %s", emsg.data),
				 (xmsg) ? errdetail("%s", xmsg) : 0));
	}
	PG_CATCH();
	{
		pfree(emsg.data);
		if (xmsg)
			pfree(xmsg);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(emsg.data);
	if (xmsg)
		pfree(xmsg);
}

static char *
PLy_traceback(int *xlevel)
{
	PyObject   *e,
			   *v,
			   *tb;
	PyObject   *eob,
			   *vob = NULL;
	char	   *vstr,
			   *estr;
	StringInfoData xstr;

	/*
	 * get the current exception
	 */
	PyErr_Fetch(&e, &v, &tb);

	/*
	 * oops, no exception, return
	 */
	if (e == NULL)
	{
		*xlevel = WARNING;
		return NULL;
	}

	PyErr_NormalizeException(&e, &v, &tb);
	Py_XDECREF(tb);

	eob = PyObject_Str(e);
	if (v && ((vob = PyObject_Str(v)) != NULL))
		vstr = PyString_AsString(vob);
	else
		vstr = "Unknown";

	/*
	 * I'm not sure what to do if eob is NULL here -- we can't call PLy_elog
	 * because that function calls us, so we could end up with infinite
	 * recursion.  I'm not even sure if eob could be NULL here -- would an
	 * Assert() be more appropriate?
	 */
	estr = eob ? PyString_AsString(eob) : "Unknown Exception";
	initStringInfo(&xstr);
	appendStringInfo(&xstr, "%s: %s", estr, vstr);

	Py_DECREF(eob);
	Py_XDECREF(vob);
	Py_XDECREF(v);

	/*
	 * intuit an appropriate error level based on the exception type
	 */
	if (PLy_exc_error && PyErr_GivenExceptionMatches(e, PLy_exc_error))
		*xlevel = ERROR;
	else if (PLy_exc_fatal && PyErr_GivenExceptionMatches(e, PLy_exc_fatal))
		*xlevel = FATAL;
	else
		*xlevel = ERROR;

	Py_DECREF(e);
	return xstr.data;
}

/* python module code */

/* some dumb utility functions */
static void *
PLy_malloc(size_t bytes)
{
	void	   *ptr = malloc(bytes);

	if (ptr == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return ptr;
}

static void *
PLy_malloc0(size_t bytes)
{
	void	   *ptr = PLy_malloc(bytes);

	MemSet(ptr, 0, bytes);
	return ptr;
}

static char *
PLy_strdup(const char *str)
{
	char	   *result;
	size_t		len;

	len = strlen(str) + 1;
	result = PLy_malloc(len);
	memcpy(result, str, len);

	return result;
}

/* define this away */
static void
PLy_free(void *ptr)
{
	free(ptr);
}
