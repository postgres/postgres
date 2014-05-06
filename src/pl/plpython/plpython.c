/**********************************************************************
 * plpython.c - python as a procedural language for PostgreSQL
 *
 *	$PostgreSQL: pgsql/src/pl/plpython/plpython.c,v 1.148.2.1 2010/08/25 19:37:52 petere Exp $
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

/*
 * Python 2/3 strings/unicode/bytes handling.  Python 2 has strings
 * and unicode, Python 3 has strings, which are unicode on the C
 * level, and bytes.  The porting convention, which is similarly used
 * in Python 2.6, is that "Unicode" is always unicode, and "Bytes" are
 * bytes in Python 3 and strings in Python 2.  Since we keep
 * supporting Python 2 and its usual strings, we provide a
 * compatibility layer for Python 3 that when asked to convert a C
 * string to a Python string it converts the C string from the
 * PostgreSQL server encoding to a Python Unicode object.
 */

#if PY_VERSION_HEX < 0x02060000
/* This is exactly the compatibility layer that Python 2.6 uses. */
#define PyBytes_AsString PyString_AsString
#define PyBytes_FromStringAndSize PyString_FromStringAndSize
#define PyBytes_Size PyString_Size
#define PyObject_Bytes PyObject_Str
#endif

#if PY_MAJOR_VERSION >= 3
#define PyString_Check(x) 0
#define PyString_AsString(x) PLyUnicode_AsString(x)
#define PyString_FromString(x) PLyUnicode_FromString(x)
#endif

/*
 * Python 3 only has long.
 */
#if PY_MAJOR_VERSION >= 3
#define PyInt_FromLong(x) PyLong_FromLong(x)
#endif

/*
 * PyVarObject_HEAD_INIT was added in Python 2.6.  Its use is
 * necessary to handle both Python 2 and 3.  This replacement
 * definition is for Python <=2.5
 */
#ifndef PyVarObject_HEAD_INIT
#define PyVarObject_HEAD_INIT(type, size)		\
		PyObject_HEAD_INIT(type) size,
#endif

#include "postgres.h"

/* system stuff */
#include <unistd.h>
#include <fcntl.h>

/* postgreSQL stuff */
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpython")

#include <compile.h>
#include <eval.h>

PG_MODULE_MAGIC;

/* convert Postgresql Datum or tuple into a PyObject.
 * input to Python.  Tuples are converted to dictionary
 * objects.
 */

struct PLyDatumToOb;
typedef PyObject *(*PLyDatumToObFunc) (struct PLyDatumToOb *, Datum);

typedef struct PLyDatumToOb
{
	PLyDatumToObFunc func;
	FmgrInfo	typfunc;		/* The type's output function */
	Oid			typoid;			/* The OID of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	struct PLyDatumToOb *elm;
} PLyDatumToOb;

typedef struct PLyTupleToOb
{
	PLyDatumToOb *atts;
	int			natts;
} PLyTupleToOb;

typedef union PLyTypeInput
{
	PLyDatumToOb d;
	PLyTupleToOb r;
} PLyTypeInput;

/* convert PyObject to a Postgresql Datum or tuple.
 * output from Python
 */

struct PLyObToDatum;
typedef Datum (*PLyObToDatumFunc) (struct PLyObToDatum *, int32 typmod,
								   PyObject *);

typedef struct PLyObToDatum
{
	PLyObToDatumFunc func;
	FmgrInfo	typfunc;		/* The type's input function */
	Oid			typoid;			/* The OID of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	struct PLyObToDatum *elm;
} PLyObToDatum;

typedef struct PLyObToTuple
{
	PLyObToDatum *atts;
	int			natts;
} PLyObToTuple;

typedef union PLyTypeOutput
{
	PLyObToDatum d;
	PLyObToTuple r;
} PLyTypeOutput;

/* all we need to move Postgresql data to Python objects,
 * and vis versa
 */
typedef struct PLyTypeInfo
{
	PLyTypeInput in;
	PLyTypeOutput out;

	/*
	 * is_rowtype can be: -1 = not known yet (initial state); 0 = scalar
	 * datatype; 1 = rowtype; 2 = rowtype, but I/O functions not set up yet
	 */
	int			is_rowtype;
} PLyTypeInfo;


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
} PLyProcedure;


/* Python objects */
typedef struct PLyPlanObject
{
	PyObject_HEAD
	void	   *plan;			/* return of an SPI_saveplan */
	int			nargs;
	Oid		   *types;
	Datum	   *values;
	PLyTypeInfo *args;
} PLyPlanObject;

typedef struct PLyResultObject
{
	PyObject_HEAD
	/* HeapTuple *tuples; */
	PyObject   *nrows;			/* number of rows returned by query */
	PyObject   *rows;			/* data rows, or None if no data returned */
	PyObject   *status;			/* query status, SPI_OK_*, or SPI_ERR_* */
} PLyResultObject;


/* function declarations */

#if PY_MAJOR_VERSION >= 3
/* Use separate names to avoid clash in pg_pltemplate */
#define plpython_call_handler plpython3_call_handler
#define plpython_inline_handler plpython3_inline_handler
#endif

/* exported functions */
Datum		plpython_call_handler(PG_FUNCTION_ARGS);
Datum		plpython_inline_handler(PG_FUNCTION_ARGS);
void		_PG_init(void);

PG_FUNCTION_INFO_V1(plpython_call_handler);
PG_FUNCTION_INFO_V1(plpython_inline_handler);

/* most of the remaining of the declarations, all static */

/* these should only be called once at the first call
 * of plpython_call_handler.  initialize the python interpreter
 * and global data.
 */
static void PLy_init_interp(void);
static void PLy_init_plpy(void);

/* call PyErr_SetString with a vprint interface and translation support */
static void
PLy_exception_set(PyObject *, const char *,...)
__attribute__((format(printf, 2, 3)));

/* same, with pluralized message */
static void
PLy_exception_set_plural(PyObject *, const char *, const char *,
						 unsigned long n,...)
__attribute__((format(printf, 2, 5)))
__attribute__((format(printf, 3, 5)));

/* Get the innermost python procedure called from the backend */
static char *PLy_procedure_name(PLyProcedure *);

/* some utility functions */
static void
PLy_elog(int, const char *,...)
__attribute__((format(printf, 2, 3)));
static char *PLy_traceback(int *);

static void *PLy_malloc(size_t);
static void *PLy_malloc0(size_t);
static char *PLy_strdup(const char *);
static void PLy_free(void *);

static PyObject *PLyUnicode_Bytes(PyObject *unicode);
static char *PLyUnicode_AsString(PyObject *unicode);

#if PY_MAJOR_VERSION >= 3
static PyObject *PLyUnicode_FromString(const char *s);
#endif

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
static PyObject *PLyBool_FromBool(PLyDatumToOb *arg, Datum d);
static PyObject *PLyFloat_FromFloat4(PLyDatumToOb *arg, Datum d);
static PyObject *PLyFloat_FromFloat8(PLyDatumToOb *arg, Datum d);
static PyObject *PLyFloat_FromNumeric(PLyDatumToOb *arg, Datum d);
static PyObject *PLyInt_FromInt16(PLyDatumToOb *arg, Datum d);
static PyObject *PLyInt_FromInt32(PLyDatumToOb *arg, Datum d);
static PyObject *PLyLong_FromInt64(PLyDatumToOb *arg, Datum d);
static PyObject *PLyBytes_FromBytea(PLyDatumToOb *arg, Datum d);
static PyObject *PLyString_FromDatum(PLyDatumToOb *arg, Datum d);
static PyObject *PLyList_FromArray(PLyDatumToOb *arg, Datum d);

static PyObject *PLyDict_FromTuple(PLyTypeInfo *, HeapTuple, TupleDesc);

static Datum PLyObject_ToBool(PLyObToDatum *, int32, PyObject *);
static Datum PLyObject_ToBytea(PLyObToDatum *, int32, PyObject *);
static Datum PLyObject_ToDatum(PLyObToDatum *, int32, PyObject *);
static Datum PLySequence_ToArray(PLyObToDatum *, int32, PyObject *);

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

static void
plpython_error_callback(void *arg)
{
	if (PLy_curr_procedure)
		errcontext("PL/Python function \"%s\"",
				   PLy_procedure_name(PLy_curr_procedure));
}

static void
plpython_inline_error_callback(void *arg)
{
	errcontext("PL/Python anonymous code block");
}

static void
plpython_trigger_error_callback(void *arg)
{
	if (PLy_curr_procedure)
		errcontext("while modifying trigger row");
}

static void
plpython_return_error_callback(void *arg)
{
	if (PLy_curr_procedure)
		errcontext("while creating return value");
}

Datum
plpython_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	PLyProcedure *save_curr_proc;
	PLyProcedure *volatile proc = NULL;
	ErrorContextCallback plerrcontext;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	save_curr_proc = PLy_curr_procedure;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpython_error_callback;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

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

	/* Pop the error context stack */
	error_context_stack = plerrcontext.previous;

	PLy_curr_procedure = save_curr_proc;

	Py_DECREF(proc->me);

	return retval;
}

Datum
plpython_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	FunctionCallInfoData fake_fcinfo;
	FmgrInfo	flinfo;
	PLyProcedure *save_curr_proc;
	PLyProcedure *volatile proc = NULL;
	ErrorContextCallback plerrcontext;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	save_curr_proc = PLy_curr_procedure;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpython_inline_error_callback;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	MemSet(&fake_fcinfo, 0, sizeof(fake_fcinfo));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo.flinfo = &flinfo;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	proc = PLy_malloc0(sizeof(PLyProcedure));
	proc->pyname = PLy_strdup("__plpython_inline_block");
	proc->result.out.d.typoid = VOIDOID;

	PG_TRY();
	{
		PLy_procedure_compile(proc, codeblock->source_text);
		PLy_curr_procedure = proc;
		PLy_function_handler(&fake_fcinfo, proc);
	}
	PG_CATCH();
	{
		PLy_procedure_delete(proc);
		PLy_curr_procedure = save_curr_proc;
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	PLy_procedure_delete(proc);

	/* Pop the error context stack */
	error_context_stack = plerrcontext.previous;

	PLy_curr_procedure = save_curr_proc;

	PG_RETURN_VOID();
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
PLy_trigger_handler(FunctionCallInfo fcinfo, PLyProcedure *proc)
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

static HeapTuple
PLy_modify_tuple(PLyProcedure *proc, PyObject *pltd, TriggerData *tdata,
				 HeapTuple otup)
{
	PyObject   *volatile plntup;
	PyObject   *volatile plkeys;
	PyObject   *volatile plval;
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

	plntup = plkeys = plval = NULL;
	modattrs = NULL;
	modvalues = NULL;
	modnulls = NULL;

	PG_TRY();
	{
		if ((plntup = PyDict_GetItemString(pltd, "new")) == NULL)
			ereport(ERROR,
					(errmsg("TD[\"new\"] deleted, cannot modify row")));
		Py_INCREF(plntup);
		if (!PyDict_Check(plntup))
			ereport(ERROR,
					(errmsg("TD[\"new\"] is not a dictionary")));

		plkeys = PyDict_Keys(plntup);
		natts = PyList_Size(plkeys);

		modattrs = (int *) palloc(natts * sizeof(int));
		modvalues = (Datum *) palloc(natts * sizeof(Datum));
		modnulls = (char *) palloc(natts * sizeof(char));

		tupdesc = tdata->tg_relation->rd_att;

		for (i = 0; i < natts; i++)
		{
			PyObject   *platt;
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



/* function handler and friends */
static Datum
PLy_function_handler(FunctionCallInfo fcinfo, PLyProcedure *proc)
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
			 * Simple type returning function or first time for SETOF function:
			 * actually execute the function.
			 */
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
					ereport(ERROR,
							(errcode(ERRCODE_DATA_EXCEPTION),
						  errmsg("error fetching next item from iterator")));

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
			rv = (proc->result.out.d.func) (&proc->result.out.d, -1, plrv);
		}
	}
	PG_CATCH();
	{
		Py_XDECREF(plargs);
		Py_XDECREF(plrv);

		PG_RE_THROW();
	}
	PG_END_TRY();

	error_context_stack = plerrcontext.previous;

	Py_XDECREF(plargs);
	Py_DECREF(plrv);

	return rv;
}

static PyObject *
PLy_procedure_call(PLyProcedure *proc, char *kargs, PyObject *vargs)
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
		PLy_elog(ERROR, NULL);
	}

	return rv;
}

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


/*
 * PLyProcedure functions
 */

/* PLy_procedure_get: returns a cached PLyProcedure, or creates, stores and
 * returns a new PLyProcedure.  fcinfo is the call info, tgreloid is the
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
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
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
		if (!proc)
			PLy_elog(ERROR, "PyCObject_AsVoidPtr() failed");
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
		 * Input/output conversion for trigger tuples.  Use the result
		 * TypeInfo variable to store the tuple conversion info.  We do this
		 * over again on each call to cover the possibility that the
		 * relation's tupdesc changed since the trigger was last called.
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

			rvTypeTup = SearchSysCache1(TYPEOID,
								   ObjectIdGetDatum(procStruct->prorettype));
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
						  errmsg("PL/Python functions cannot return type %s",
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
		 * Now get information required for input conversion of the
		 * procedure's arguments.  Note that we ignore output arguments here
		 * --- since we don't support returning record, and that was already
		 * checked above, there's no need to worry about multiple output
		 * arguments.
		 */
		if (procStruct->pronargs)
		{
			Oid		   *types;
			char	  **names,
					   *modes;
			int			i,
						pos,
						total;

			/* extract argument type info from the pg_proc tuple */
			total = get_func_arg_info(procTup, &types, &names, &modes);

			/* count number of in+inout args into proc->nargs */
			if (modes == NULL)
				proc->nargs = total;
			else
			{
				/* proc->nargs was initialized to 0 above */
				for (i = 0; i < total; i++)
				{
					if (modes[i] != PROARGMODE_OUT &&
						modes[i] != PROARGMODE_TABLE)
						(proc->nargs)++;
				}
			}

			proc->argnames = (char **) PLy_malloc0(sizeof(char *) * proc->nargs);
			for (i = pos = 0; i < total; i++)
			{
				HeapTuple	argTypeTup;
				Form_pg_type argTypeStruct;

				if (modes &&
					(modes[i] == PROARGMODE_OUT ||
					 modes[i] == PROARGMODE_TABLE))
					continue;	/* skip OUT arguments */

				Assert(types[i] == procStruct->proargtypes.values[pos]);

				argTypeTup = SearchSysCache1(TYPEOID,
											 ObjectIdGetDatum(types[i]));
				if (!HeapTupleIsValid(argTypeTup))
					elog(ERROR, "cache lookup failed for type %u", types[i]);
				argTypeStruct = (Form_pg_type) GETSTRUCT(argTypeTup);

				/* check argument type is OK, set up I/O function info */
				switch (argTypeStruct->typtype)
				{
					case TYPTYPE_PSEUDO:
						/* Disallow pseudotype argument */
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						  errmsg("PL/Python functions cannot accept type %s",
								 format_type_be(types[i]))));
						break;
					case TYPTYPE_COMPOSITE:
						/* we'll set IO funcs at first call */
						proc->args[pos].is_rowtype = 2;
						break;
					default:
						PLy_input_datum_func(&(proc->args[pos]),
											 types[i],
											 argTypeTup);
						break;
				}

				/* get argument name */
				proc->argnames[pos] = names ? PLy_strdup(names[i]) : NULL;

				ReleaseSysCache(argTypeTup);

				pos++;
			}
		}

		/*
		 * get the text of the function.
		 */
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
		procSource = TextDatumGetCString(prosrcdatum);

		PLy_procedure_compile(proc, procSource);

		pfree(procSource);
		procSource = NULL;

		proc->me = PyCObject_FromVoidPtr(proc, NULL);
		if (!proc->me)
			PLy_elog(ERROR, "PyCObject_FromVoidPtr() failed");
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
PLy_procedure_compile(PLyProcedure *proc, const char *src)
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

	PLy_elog(ERROR, "could not compile PL/Python function \"%s\"", proc->proname);
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
PLy_procedure_delete(PLyProcedure *proc)
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
	PLy_free(proc);
}

/*
 * Conversion functions.  Remember output from Python is input to
 * PostgreSQL, and vice versa.
 */
static void
PLy_input_tuple_funcs(PLyTypeInfo *arg, TupleDesc desc)
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

		typeTup = SearchSysCache1(TYPEOID,
								  ObjectIdGetDatum(desc->attrs[i]->atttypid));
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
PLy_output_tuple_funcs(PLyTypeInfo *arg, TupleDesc desc)
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

		typeTup = SearchSysCache1(TYPEOID,
								  ObjectIdGetDatum(desc->attrs[i]->atttypid));
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "cache lookup failed for type %u",
				 desc->attrs[i]->atttypid);

		PLy_output_datum_func2(&(arg->out.r.atts[i]), typeTup);

		ReleaseSysCache(typeTup);
	}
}

static void
PLy_output_datum_func(PLyTypeInfo *arg, HeapTuple typeTup)
{
	if (arg->is_rowtype > 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for a Tuple");
	arg->is_rowtype = 0;
	PLy_output_datum_func2(&(arg->out.d), typeTup);
}

static void
PLy_output_datum_func2(PLyObToDatum *arg, HeapTuple typeTup)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	Oid			element_type;

	perm_fmgr_info(typeStruct->typinput, &arg->typfunc);
	arg->typoid = HeapTupleGetOid(typeTup);
	arg->typioparam = getTypeIOParam(typeTup);
	arg->typbyval = typeStruct->typbyval;

	element_type = get_element_type(arg->typoid);

	/*
	 * Select a conversion function to convert Python objects to PostgreSQL
	 * datums.  Most data types can go through the generic function.
	 */
	switch (getBaseType(element_type ? element_type : arg->typoid))
	{
		case BOOLOID:
			arg->func = PLyObject_ToBool;
			break;
		case BYTEAOID:
			arg->func = PLyObject_ToBytea;
			break;
		default:
			arg->func = PLyObject_ToDatum;
			break;
	}

	if (element_type)
	{
		char		dummy_delim;
		Oid			funcid;

		if (type_is_rowtype(element_type))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/Python functions cannot return type %s",
							format_type_be(arg->typoid)),
					 errdetail("PL/Python does not support conversion to arrays of row types.")));

		arg->elm = PLy_malloc0(sizeof(*arg->elm));
		arg->elm->func = arg->func;
		arg->func = PLySequence_ToArray;

		arg->elm->typoid = element_type;
		get_type_io_data(element_type, IOFunc_input,
						 &arg->elm->typlen, &arg->elm->typbyval, &arg->elm->typalign, &dummy_delim,
						 &arg->elm->typioparam, &funcid);
		perm_fmgr_info(funcid, &arg->elm->typfunc);
	}
}

static void
PLy_input_datum_func(PLyTypeInfo *arg, Oid typeOid, HeapTuple typeTup)
{
	if (arg->is_rowtype > 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for Tuple");
	arg->is_rowtype = 0;
	PLy_input_datum_func2(&(arg->in.d), typeOid, typeTup);
}

static void
PLy_input_datum_func2(PLyDatumToOb *arg, Oid typeOid, HeapTuple typeTup)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	Oid			element_type = get_element_type(typeOid);

	/* Get the type's conversion information */
	perm_fmgr_info(typeStruct->typoutput, &arg->typfunc);
	arg->typoid = HeapTupleGetOid(typeTup);
	arg->typioparam = getTypeIOParam(typeTup);
	arg->typbyval = typeStruct->typbyval;
	arg->typlen = typeStruct->typlen;
	arg->typalign = typeStruct->typalign;

	/* Determine which kind of Python object we will convert to */
	switch (getBaseType(element_type ? element_type : typeOid))
	{
		case BOOLOID:
			arg->func = PLyBool_FromBool;
			break;
		case FLOAT4OID:
			arg->func = PLyFloat_FromFloat4;
			break;
		case FLOAT8OID:
			arg->func = PLyFloat_FromFloat8;
			break;
		case NUMERICOID:
			arg->func = PLyFloat_FromNumeric;
			break;
		case INT2OID:
			arg->func = PLyInt_FromInt16;
			break;
		case INT4OID:
			arg->func = PLyInt_FromInt32;
			break;
		case INT8OID:
			arg->func = PLyLong_FromInt64;
			break;
		case BYTEAOID:
			arg->func = PLyBytes_FromBytea;
			break;
		default:
			arg->func = PLyString_FromDatum;
			break;
	}

	if (element_type)
	{
		char		dummy_delim;
		Oid			funcid;

		arg->elm = PLy_malloc0(sizeof(*arg->elm));
		arg->elm->func = arg->func;
		arg->func = PLyList_FromArray;
		arg->elm->typoid = element_type;
		get_type_io_data(element_type, IOFunc_output,
						 &arg->elm->typlen, &arg->elm->typbyval, &arg->elm->typalign, &dummy_delim,
						 &arg->elm->typioparam, &funcid);
		perm_fmgr_info(funcid, &arg->elm->typfunc);
	}
}

static void
PLy_typeinfo_init(PLyTypeInfo *arg)
{
	arg->is_rowtype = -1;
	arg->in.r.natts = arg->out.r.natts = 0;
	arg->in.r.atts = NULL;
	arg->out.r.atts = NULL;
}

static void
PLy_typeinfo_dealloc(PLyTypeInfo *arg)
{
	if (arg->is_rowtype == 1)
	{
		if (arg->in.r.atts)
			PLy_free(arg->in.r.atts);
		if (arg->out.r.atts)
			PLy_free(arg->out.r.atts);
	}
}

static PyObject *
PLyBool_FromBool(PLyDatumToOb *arg, Datum d)
{
	/*
	 * We would like to use Py_RETURN_TRUE and Py_RETURN_FALSE here for
	 * generating SQL from trigger functions, but those are only supported in
	 * Python >= 2.3, and we support older versions.
	 * http://docs.python.org/api/boolObjects.html
	 */
	if (DatumGetBool(d))
		return PyBool_FromLong(1);
	return PyBool_FromLong(0);
}

static PyObject *
PLyFloat_FromFloat4(PLyDatumToOb *arg, Datum d)
{
	return PyFloat_FromDouble(DatumGetFloat4(d));
}

static PyObject *
PLyFloat_FromFloat8(PLyDatumToOb *arg, Datum d)
{
	return PyFloat_FromDouble(DatumGetFloat8(d));
}

static PyObject *
PLyFloat_FromNumeric(PLyDatumToOb *arg, Datum d)
{
	/*
	 * Numeric is cast to a PyFloat: This results in a loss of precision Would
	 * it be better to cast to PyString?
	 */
	Datum		f = DirectFunctionCall1(numeric_float8, d);
	double		x = DatumGetFloat8(f);

	return PyFloat_FromDouble(x);
}

static PyObject *
PLyInt_FromInt16(PLyDatumToOb *arg, Datum d)
{
	return PyInt_FromLong(DatumGetInt16(d));
}

static PyObject *
PLyInt_FromInt32(PLyDatumToOb *arg, Datum d)
{
	return PyInt_FromLong(DatumGetInt32(d));
}

static PyObject *
PLyLong_FromInt64(PLyDatumToOb *arg, Datum d)
{
	/* on 32 bit platforms "long" may be too small */
	if (sizeof(int64) > sizeof(long))
		return PyLong_FromLongLong(DatumGetInt64(d));
	else
		return PyLong_FromLong(DatumGetInt64(d));
}

static PyObject *
PLyBytes_FromBytea(PLyDatumToOb *arg, Datum d)
{
	text	   *txt = DatumGetByteaP(d);
	char	   *str = VARDATA(txt);
	size_t		size = VARSIZE(txt) - VARHDRSZ;

	return PyBytes_FromStringAndSize(str, size);
}

static PyObject *
PLyString_FromDatum(PLyDatumToOb *arg, Datum d)
{
	char	   *x = OutputFunctionCall(&arg->typfunc, d);
	PyObject   *r = PyString_FromString(x);

	pfree(x);
	return r;
}

static PyObject *
PLyList_FromArray(PLyDatumToOb *arg, Datum d)
{
	ArrayType  *array = DatumGetArrayTypeP(d);
	PLyDatumToOb *elm = arg->elm;
	PyObject   *list;
	int			length;
	int			lbound;
	int			i;

	if (ARR_NDIM(array) == 0)
		return PyList_New(0);

	if (ARR_NDIM(array) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("cannot convert multidimensional array to Python list"),
			  errdetail("PL/Python only supports one-dimensional arrays.")));

	length = ARR_DIMS(array)[0];
	lbound = ARR_LBOUND(array)[0];
	list = PyList_New(length);

	for (i = 0; i < length; i++)
	{
		Datum		elem;
		bool		isnull;
		int			offset;

		offset = lbound + i;
		elem = array_ref(array, 1, &offset, arg->typlen,
						 elm->typlen, elm->typbyval, elm->typalign,
						 &isnull);
		if (isnull)
		{
			Py_INCREF(Py_None);
			PyList_SET_ITEM(list, i, Py_None);
		}
		else
			PyList_SET_ITEM(list, i, elm->func(elm, elem));
	}

	return list;
}

static PyObject *
PLyDict_FromTuple(PLyTypeInfo *info, HeapTuple tuple, TupleDesc desc)
{
	PyObject   *volatile dict;
	int			i;

	if (info->is_rowtype != 1)
		elog(ERROR, "PLyTypeInfo structure describes a datum");

	dict = PyDict_New();
	if (dict == NULL)
		PLy_elog(ERROR, "could not create new dictionary");

	PG_TRY();
	{
		for (i = 0; i < info->in.r.natts; i++)
		{
			char	   *key;
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
				value = (info->in.r.atts[i].func) (&info->in.r.atts[i], vattr);
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

/*
 * Convert a Python object to a PostgreSQL bool datum.  This can't go
 * through the generic conversion function, because Python attaches a
 * Boolean value to everything, more things than the PostgreSQL bool
 * type can parse.
 */
static Datum
PLyObject_ToBool(PLyObToDatum *arg, int32 typmod, PyObject *plrv)
{
	Datum		rv;

	Assert(plrv != Py_None);
	rv = BoolGetDatum(PyObject_IsTrue(plrv));

	if (get_typtype(arg->typoid) == TYPTYPE_DOMAIN)
		domain_check(rv, false, arg->typoid, &arg->typfunc.fn_extra, arg->typfunc.fn_mcxt);

	return rv;
}

/*
 * Convert a Python object to a PostgreSQL bytea datum.  This doesn't
 * go through the generic conversion function to circumvent problems
 * with embedded nulls.  And it's faster this way.
 */
static Datum
PLyObject_ToBytea(PLyObToDatum *arg, int32 typmod, PyObject *plrv)
{
	PyObject   *volatile plrv_so = NULL;
	Datum		rv;

	Assert(plrv != Py_None);

	plrv_so = PyObject_Bytes(plrv);
	if (!plrv_so)
		PLy_elog(ERROR, "could not create bytes representation of Python object");

	PG_TRY();
	{
		char	   *plrv_sc = PyBytes_AsString(plrv_so);
		size_t		len = PyBytes_Size(plrv_so);
		size_t		size = len + VARHDRSZ;
		bytea	   *result = palloc(size);

		SET_VARSIZE(result, size);
		memcpy(VARDATA(result), plrv_sc, len);
		rv = PointerGetDatum(result);
	}
	PG_CATCH();
	{
		Py_XDECREF(plrv_so);
		PG_RE_THROW();
	}
	PG_END_TRY();

	Py_XDECREF(plrv_so);

	if (get_typtype(arg->typoid) == TYPTYPE_DOMAIN)
		domain_check(rv, false, arg->typoid, &arg->typfunc.fn_extra, arg->typfunc.fn_mcxt);

	return rv;
}

/*
 * Generic conversion function: Convert PyObject to cstring and
 * cstring into PostgreSQL type.
 */
static Datum
PLyObject_ToDatum(PLyObToDatum *arg, int32 typmod, PyObject *plrv)
{
	PyObject   *volatile plrv_bo = NULL;
	Datum		rv;

	Assert(plrv != Py_None);

	if (PyUnicode_Check(plrv))
		plrv_bo = PLyUnicode_Bytes(plrv);
	else
	{
#if PY_MAJOR_VERSION >= 3
		PyObject   *s = PyObject_Str(plrv);

		plrv_bo = PLyUnicode_Bytes(s);
		Py_XDECREF(s);
#else
		plrv_bo = PyObject_Str(plrv);
#endif
	}
	if (!plrv_bo)
		PLy_elog(ERROR, "could not create string representation of Python object");

	PG_TRY();
	{
		char	   *plrv_sc = PyBytes_AsString(plrv_bo);
		size_t		plen = PyBytes_Size(plrv_bo);
		size_t		slen = strlen(plrv_sc);

		if (slen < plen)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not convert Python object into cstring: Python string representation appears to contain null bytes")));
		else if (slen > plen)
			elog(ERROR, "could not convert Python object into cstring: Python string longer than reported length");
		pg_verifymbstr(plrv_sc, slen, false);
		rv = InputFunctionCall(&arg->typfunc,
							   plrv_sc,
							   arg->typioparam,
							   typmod);
	}
	PG_CATCH();
	{
		Py_XDECREF(plrv_bo);
		PG_RE_THROW();
	}
	PG_END_TRY();

	Py_XDECREF(plrv_bo);

	return rv;
}

static Datum
PLySequence_ToArray(PLyObToDatum *arg, int32 typmod, PyObject *plrv)
{
	ArrayType  *array;
	int			i;
	Datum	   *elems;
	bool	   *nulls;
	int			len;
	int			lbs;

	Assert(plrv != Py_None);

	if (!PySequence_Check(plrv))
		PLy_elog(ERROR, "return value of function with array return type is not a Python sequence");

	len = PySequence_Length(plrv);
	elems = palloc(sizeof(*elems) * len);
	nulls = palloc(sizeof(*nulls) * len);

	for (i = 0; i < len; i++)
	{
		PyObject   *obj = PySequence_GetItem(plrv, i);

		if (obj == Py_None)
			nulls[i] = true;
		else
		{
			nulls[i] = false;

			/*
			 * We don't support arrays of row types yet, so the first argument
			 * can be NULL.
			 */
			elems[i] = arg->elm->func(arg->elm, -1, obj);
		}
		Py_XDECREF(obj);
	}

	lbs = 1;
	array = construct_md_array(elems, nulls, 1, &len, &lbs,
							   get_element_type(arg->typoid), arg->elm->typlen, arg->elm->typbyval, arg->elm->typalign);
	return PointerGetDatum(array);
}

static HeapTuple
PLyMapping_ToTuple(PLyTypeInfo *info, PyObject *mapping)
{
	TupleDesc	desc;
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int i;

	Assert(PyMapping_Check(mapping));

	desc = lookup_rowtype_tupdesc(info->out.d.typoid, -1);
	if (info->is_rowtype == 2)
		PLy_output_tuple_funcs(info, desc);
	Assert(info->is_rowtype == 1);

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(bool) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		char	   *key;
		PyObject   *volatile value;
		PLyObToDatum *att;

		key = NameStr(desc->attrs[i]->attname);
		value = NULL;
		att = &info->out.r.atts[i];
		PG_TRY();
		{
			value = PyMapping_GetItemString(mapping, key);
			if (value == Py_None)
			{
				values[i] = (Datum) NULL;
				nulls[i] = true;
			}
			else if (value)
			{
				values[i] = (att->func) (att, -1, value);
				nulls[i] = false;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("key \"%s\" not found in mapping", key),
						 errhint("To return null in a column, "
								 "add the value None to the mapping with the key named after the column.")));

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_form_tuple(desc, values, nulls);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);

	return tuple;
}


static HeapTuple
PLySequence_ToTuple(PLyTypeInfo *info, PyObject *sequence)
{
	TupleDesc	desc;
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
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
				 errmsg("length of returned sequence did not match number of columns in row")));

	if (info->is_rowtype == 2)
		PLy_output_tuple_funcs(info, desc);
	Assert(info->is_rowtype == 1);

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(bool) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		PyObject   *volatile value;
		PLyObToDatum *att;

		value = NULL;
		att = &info->out.r.atts[i];
		PG_TRY();
		{
			value = PySequence_GetItem(sequence, i);
			Assert(value);
			if (value == Py_None)
			{
				values[i] = (Datum) NULL;
				nulls[i] = true;
			}
			else if (value)
			{
				values[i] = (att->func) (att, -1, value);
				nulls[i] = false;
			}

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_form_tuple(desc, values, nulls);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);

	return tuple;
}


static HeapTuple
PLyObject_ToTuple(PLyTypeInfo *info, PyObject *object)
{
	TupleDesc	desc;
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int i;

	desc = lookup_rowtype_tupdesc(info->out.d.typoid, -1);
	if (info->is_rowtype == 2)
		PLy_output_tuple_funcs(info, desc);
	Assert(info->is_rowtype == 1);

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(bool) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		char	   *key;
		PyObject   *volatile value;
		PLyObToDatum *att;

		key = NameStr(desc->attrs[i]->attname);
		value = NULL;
		att = &info->out.r.atts[i];
		PG_TRY();
		{
			value = PyObject_GetAttrString(object, key);
			if (value == Py_None)
			{
				values[i] = (Datum) NULL;
				nulls[i] = true;
			}
			else if (value)
			{
				values[i] = (att->func) (att, -1, value);
				nulls[i] = false;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("attribute \"%s\" does not exist in Python object", key),
						 errhint("To return null in a column, "
						   "let the returned object have an attribute named "
								 "after column with value None.")));

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_form_tuple(desc, values, nulls);
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
static PyObject *PLy_plan_status(PyObject *, PyObject *);

static PyObject *PLy_result_new(void);
static void PLy_result_dealloc(PyObject *);
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


static PyMethodDef PLy_plan_methods[] = {
	{"status", PLy_plan_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject PLy_PlanType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"PLyPlan",					/* tp_name */
	sizeof(PLyPlanObject),		/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_plan_dealloc,			/* tp_dealloc */
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
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	PLy_plan_doc,				/* tp_doc */
	0,							/* tp_traverse */
	0,							/* tp_clear */
	0,							/* tp_richcompare */
	0,							/* tp_weaklistoffset */
	0,							/* tp_iter */
	0,							/* tp_iternext */
	PLy_plan_methods,			/* tp_tpmethods */
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

static PyMethodDef PLy_result_methods[] = {
	{"nrows", PLy_result_nrows, METH_VARARGS, NULL},
	{"status", PLy_result_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject PLy_ResultType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"PLyResult",				/* tp_name */
	sizeof(PLyResultObject),	/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_result_dealloc,			/* tp_dealloc */
	0,							/* tp_print */
	0,							/* tp_getattr */
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
	0,							/* tp_traverse */
	0,							/* tp_clear */
	0,							/* tp_richcompare */
	0,							/* tp_weaklistoffset */
	0,							/* tp_iter */
	0,							/* tp_iternext */
	PLy_result_methods,			/* tp_tpmethods */
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

#if PY_MAJOR_VERSION >= 3
static PyModuleDef PLy_module = {
	PyModuleDef_HEAD_INIT,		/* m_base */
	"plpy",						/* m_name */
	NULL,						/* m_doc */
	-1,							/* m_size */
	PLy_methods,				/* m_methods */
};
#endif

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
PLy_plan_dealloc(PyObject *arg)
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
PLy_plan_status(PyObject *self, PyObject *args)
{
	if (PyArg_ParseTuple(args, ""))
	{
		Py_INCREF(Py_True);
		return Py_True;
		/* return PyInt_FromLong(self->status); */
	}
	PLy_exception_set(PLy_exc_error, "plan.status takes no arguments");
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
PLy_result_dealloc(PyObject *arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	Py_XDECREF(ob->nrows);
	Py_XDECREF(ob->rows);
	Py_XDECREF(ob->status);

	arg->ob_type->tp_free(arg);
}

static PyObject *
PLy_result_nrows(PyObject *self, PyObject *args)
{
	PLyResultObject *ob = (PLyResultObject *) self;

	Py_INCREF(ob->nrows);
	return ob->nrows;
}

static PyObject *
PLy_result_status(PyObject *self, PyObject *args)
{
	PLyResultObject *ob = (PLyResultObject *) self;

	Py_INCREF(ob->status);
	return ob->status;
}

static Py_ssize_t
PLy_result_length(PyObject *arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	return PyList_Size(ob->rows);
}

static PyObject *
PLy_result_item(PyObject *arg, Py_ssize_t idx)
{
	PyObject   *rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	rv = PyList_GetItem(ob->rows, idx);
	if (rv != NULL)
		Py_INCREF(rv);
	return rv;
}

static int
PLy_result_ass_item(PyObject *arg, Py_ssize_t idx, PyObject *item)
{
	int			rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	Py_INCREF(item);
	rv = PyList_SetItem(ob->rows, idx, item);
	return rv;
}

static PyObject *
PLy_result_slice(PyObject *arg, Py_ssize_t lidx, Py_ssize_t hidx)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	return PyList_GetSlice(ob->rows, lidx, hidx);
}

static int
PLy_result_ass_slice(PyObject *arg, Py_ssize_t lidx, Py_ssize_t hidx, PyObject *slice)
{
	int			rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	rv = PyList_SetSlice(ob->rows, lidx, hidx, slice);
	return rv;
}

/* SPI interface */
static PyObject *
PLy_spi_prepare(PyObject *self, PyObject *args)
{
	PLyPlanObject *plan;
	PyObject   *list = NULL;
	PyObject   *volatile optr = NULL;
	char	   *query;
	void	   *tmpplan;
	volatile MemoryContext oldcontext;

	/* Can't execute more if we have an unhandled error */
	if (PLy_error_in_progress)
	{
		PLy_exception_set(PLy_exc_error, "transaction aborted");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|O", &query, &list))
	{
		PLy_exception_set(PLy_exc_spi_error,
						  "invalid arguments for plpy.prepare");
		return NULL;
	}

	if (list && (!PySequence_Check(list)))
	{
		PLy_exception_set(PLy_exc_spi_error,
					   "second argument of plpy.prepare must be a sequence");
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

					parseTypeString(sptr, &typeId, &typmod);

					typeTup = SearchSysCache1(TYPEOID,
											  ObjectIdGetDatum(typeId));
					if (!HeapTupleIsValid(typeTup))
						elog(ERROR, "cache lookup failed for type %u", typeId);

					Py_DECREF(optr);
					optr = NULL;	/* this is important */

					plan->types[i] = typeId;
					typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
					if (typeStruct->typtype != TYPTYPE_COMPOSITE)
						PLy_output_datum_func(&plan->args[i], typeTup);
					else
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("plpy.prepare does not support composite types")));
					ReleaseSysCache(typeTup);
				}
			}
		}

		pg_verifymbstr(query, strlen(query), false);
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
			PLy_exception_set(PLy_exc_spi_error,
							  "unrecognized error in PLy_spi_prepare");
		PLy_elog(WARNING, NULL);
		return NULL;
	}
	PG_END_TRY();

	return (PyObject *) plan;
}

/* execute(query="select * from foo", limit=5)
 * execute(plan=plan, values=(foo, bar), limit=5)
 */
static PyObject *
PLy_spi_execute(PyObject *self, PyObject *args)
{
	char	   *query;
	PyObject   *plan;
	PyObject   *list = NULL;
	long		limit = 0;

	/* Can't execute more if we have an unhandled error */
	if (PLy_error_in_progress)
	{
		PLy_exception_set(PLy_exc_error, "transaction aborted");
		return NULL;
	}

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

	if (list != NULL)
	{
		if (!PySequence_Check(list) || PyString_Check(list) || PyUnicode_Check(list))
		{
			PLy_exception_set(PLy_exc_spi_error, "plpy.execute takes a sequence as its second argument");
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
		PLy_exception_set_plural(PLy_exc_spi_error,
							  "Expected sequence of %d argument, got %d: %s",
							 "Expected sequence of %d arguments, got %d: %s",
								 plan->nargs,
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
			PLy_exception_set(PLy_exc_error,
							  "unrecognized error in PLy_spi_execute_plan");
		PLy_elog(WARNING, NULL);
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
	volatile MemoryContext oldcontext;

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		pg_verifymbstr(query, strlen(query), false);
		rv = SPI_execute(query, PLy_curr_procedure->fn_readonly, limit);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		PLy_error_in_progress = CopyErrorData();
		FlushErrorState();
		if (!PyErr_Occurred())
			PLy_exception_set(PLy_exc_spi_error,
							  "unrecognized error in PLy_spi_execute_query");
		PLy_elog(WARNING, NULL);
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
	volatile MemoryContext oldcontext;

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
			}
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(oldcontext);
			PLy_error_in_progress = CopyErrorData();
			FlushErrorState();
			if (!PyErr_Occurred())
				PLy_exception_set(PLy_exc_error,
					   "unrecognized error in PLy_spi_execute_fetch_result");
			PLy_typeinfo_dealloc(&args);
			SPI_freetuptable(tuptable);
			Py_DECREF(result);
			return NULL;
		}
		PG_END_TRY();

		PLy_typeinfo_dealloc(&args);
		SPI_freetuptable(tuptable);
	}

	return (PyObject *) result;
}


/*
 * language handler and interpreter initialization
 */

#if PY_MAJOR_VERSION >= 3
/*
 * Must have external linkage, because PyMODINIT_FUNC does dllexport on
 * Windows-like platforms.
 */
PyMODINIT_FUNC PyInit_plpy(void);

PyMODINIT_FUNC
PyInit_plpy(void)
{
	return PyModule_Create(&PLy_module);
}
#endif


static const int plpython_python_version = PY_MAJOR_VERSION;

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
	const int **version_ptr;

	if (inited)
		return;

	/* Be sure we don't run Python 2 and 3 in the same session (might crash) */
	version_ptr = (const int **) find_rendezvous_variable("plpython_python_version");
	if (!(*version_ptr))
		*version_ptr = &plpython_python_version;
	else
	{
		if (**version_ptr != plpython_python_version)
			ereport(FATAL,
					(errmsg("Python major version mismatch in session"),
					 errdetail("This session has previously used Python major version %d, and it is now attempting to use Python major version %d.",
							   **version_ptr, plpython_python_version),
					 errhint("Start a new session to use a different Python major version.")));
	}

	pg_bindtextdomain(TEXTDOMAIN);

#if PY_MAJOR_VERSION >= 3
	PyImport_AppendInittab("plpy", PyInit_plpy);
#endif
	Py_Initialize();
#if PY_MAJOR_VERSION >= 3
	PyImport_ImportModule("plpy");
#endif
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
		PLy_elog(ERROR, "could not import \"__main__\" module");
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
		elog(ERROR, "could not initialize PLy_PlanType");
	if (PyType_Ready(&PLy_ResultType) < 0)
		elog(ERROR, "could not initialize PLy_ResultType");

#if PY_MAJOR_VERSION >= 3
	plpy = PyModule_Create(&PLy_module);
#else
	plpy = Py_InitModule("plpy", PLy_methods);
#endif
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
		elog(ERROR, "could not initialize plpy");
}

/* the python interface to the elog function
 * don't confuse these with PLy_elog
 */
static PyObject *PLy_output(volatile int, PyObject *, PyObject *);

static PyObject *
PLy_debug(PyObject *self, PyObject *args)
{
	return PLy_output(DEBUG2, self, args);
}

static PyObject *
PLy_log(PyObject *self, PyObject *args)
{
	return PLy_output(LOG, self, args);
}

static PyObject *
PLy_info(PyObject *self, PyObject *args)
{
	return PLy_output(INFO, self, args);
}

static PyObject *
PLy_notice(PyObject *self, PyObject *args)
{
	return PLy_output(NOTICE, self, args);
}

static PyObject *
PLy_warning(PyObject *self, PyObject *args)
{
	return PLy_output(WARNING, self, args);
}

static PyObject *
PLy_error(PyObject *self, PyObject *args)
{
	return PLy_output(ERROR, self, args);
}

static PyObject *
PLy_fatal(PyObject *self, PyObject *args)
{
	return PLy_output(FATAL, self, args);
}


static PyObject *
PLy_output(volatile int level, PyObject *self, PyObject *args)
{
	PyObject   *volatile so;
	char	   *volatile sv;
	volatile MemoryContext oldcontext;

	if (PyTuple_Size(args) == 1)
	{
		/*
		 * Treat single argument specially to avoid undesirable ('tuple',)
		 * decoration.
		 */
		PyObject   *o;

		PyArg_UnpackTuple(args, "plpy.elog", 1, 1, &o);
		so = PyObject_Str(o);
	}
	else
		so = PyObject_Str(args);
	if (so == NULL || ((sv = PyString_AsString(so)) == NULL))
	{
		level = ERROR;
		sv = dgettext(TEXTDOMAIN, "could not parse error message in plpy.elog");
	}

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		pg_verifymbstr(sv, strlen(sv), false);
		elog(level, "%s", sv);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		PLy_error_in_progress = CopyErrorData();
		FlushErrorState();

		PyErr_SetString(PLy_exc_error, sv);

		/*
		 * Note: If sv came from PyString_AsString(), it points into storage
		 * owned by so.  So free so after using sv.
		 */
		Py_XDECREF(so);

		/*
		 * returning NULL here causes the python interpreter to bail. when
		 * control passes back to PLy_procedure_call, we check for PG
		 * exceptions and re-throw the error.
		 */
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
PLy_procedure_name(PLyProcedure *proc)
{
	if (proc == NULL)
		return "<unknown procedure>";
	return proc->proname;
}

/*
 * Call PyErr_SetString with a vprint interface and translation support
 */
static void
PLy_exception_set(PyObject *exc, const char *fmt,...)
{
	char		buf[1024];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), dgettext(TEXTDOMAIN, fmt), ap);
	va_end(ap);

	PyErr_SetString(exc, buf);
}

/*
 * The same, pluralized.
 */
static void
PLy_exception_set_plural(PyObject *exc,
						 const char *fmt_singular, const char *fmt_plural,
						 unsigned long n,...)
{
	char		buf[1024];
	va_list		ap;

	va_start(ap, n);
	vsnprintf(buf, sizeof(buf),
			  dngettext(TEXTDOMAIN, fmt_singular, fmt_plural, n),
			  ap);
	va_end(ap);

	PyErr_SetString(exc, buf);
}

/* Emit a PG error or notice, together with any available info about
 * the current Python error, previously set by PLy_exception_set().
 * This should be used to propagate Python errors into PG.  If fmt is
 * NULL, the Python error becomes the primary error message, otherwise
 * it becomes the detail.
 */
static void
PLy_elog(int elevel, const char *fmt,...)
{
	char	   *xmsg;
	int			xlevel;
	StringInfoData emsg;

	xmsg = PLy_traceback(&xlevel);

	if (fmt)
	{
		initStringInfo(&emsg);
		for (;;)
		{
			va_list		ap;
			bool		success;

			va_start(ap, fmt);
			success = appendStringInfoVA(&emsg, dgettext(TEXTDOMAIN, fmt), ap);
			va_end(ap);
			if (success)
				break;
			enlargeStringInfo(&emsg, emsg.maxlen);
		}
	}

	PG_TRY();
	{
		if (fmt)
			ereport(elevel,
					(errmsg("PL/Python: %s", emsg.data),
					 (xmsg) ? errdetail("%s", xmsg) : 0));
		else
			ereport(elevel,
					(errmsg("PL/Python: %s", xmsg)));
	}
	PG_CATCH();
	{
		if (fmt)
			pfree(emsg.data);
		if (xmsg)
			pfree(xmsg);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (fmt)
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
	PyObject   *e_type_o;
	PyObject   *e_module_o;
	char	   *e_type_s = NULL;
	char	   *e_module_s = NULL;
	PyObject   *vob = NULL;
	char	   *vstr;
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

	e_type_o = PyObject_GetAttrString(e, "__name__");
	e_module_o = PyObject_GetAttrString(e, "__module__");
	if (e_type_o)
		e_type_s = PyString_AsString(e_type_o);
	if (e_type_s)
		e_module_s = PyString_AsString(e_module_o);

	if (v && ((vob = PyObject_Str(v)) != NULL))
		vstr = PyString_AsString(vob);
	else
		vstr = "unknown";

	initStringInfo(&xstr);
	if (!e_type_s || !e_module_s)
	{
		if (PyString_Check(e))
			/* deprecated string exceptions */
			appendStringInfoString(&xstr, PyString_AsString(e));
		else
			/* shouldn't happen */
			appendStringInfoString(&xstr, "unrecognized exception");
	}
	/* mimics behavior of traceback.format_exception_only */
	else if (strcmp(e_module_s, "builtins") == 0
			 || strcmp(e_module_s, "__main__") == 0
			 || strcmp(e_module_s, "exceptions") == 0)
		appendStringInfo(&xstr, "%s", e_type_s);
	else
		appendStringInfo(&xstr, "%s.%s", e_module_s, e_type_s);
	appendStringInfo(&xstr, ": %s", vstr);

	Py_XDECREF(e_type_o);
	Py_XDECREF(e_module_o);
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

/*
 * Convert a Python unicode object to a Python string/bytes object in
 * PostgreSQL server encoding.  Reference ownership is passed to the
 * caller.
 */
static PyObject *
PLyUnicode_Bytes(PyObject *unicode)
{
	PyObject	*bytes, *rv;
	char		*utf8string, *encoded;

	/* First encode the Python unicode object with UTF-8. */
	bytes = PyUnicode_AsUTF8String(unicode);
	if (bytes == NULL)
		PLy_elog(ERROR, "could not convert Python Unicode object to bytes");

	utf8string = PyBytes_AsString(bytes);
	if (utf8string == NULL) {
		Py_DECREF(bytes);
		PLy_elog(ERROR, "could not extract bytes from encoded string");
	}

	/*
	 * Then convert to server encoding if necessary.
	 *
	 * PyUnicode_AsEncodedString could be used to encode the object directly
	 * in the server encoding, but Python doesn't support all the encodings
	 * that PostgreSQL does (EUC_TW and MULE_INTERNAL). UTF-8 is used as an
	 * intermediary in PLyUnicode_FromString as well.
	 */
	if (GetDatabaseEncoding() != PG_UTF8)
	{
		PG_TRY();
		{
			encoded = (char *) pg_do_encoding_conversion(
				(unsigned char *) utf8string,
				strlen(utf8string),
				PG_UTF8,
				GetDatabaseEncoding());
		}
		PG_CATCH();
		{
			Py_DECREF(bytes);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
		encoded = utf8string;

	/* finally, build a bytes object in the server encoding */
	rv = PyBytes_FromStringAndSize(encoded, strlen(encoded));

	/* if pg_do_encoding_conversion allocated memory, free it now */
	if (utf8string != encoded)
		pfree(encoded);

	Py_DECREF(bytes);
	return rv;
}

/*
 * Convert a Python unicode object to a C string in PostgreSQL server
 * encoding.  No Python object reference is passed out of this
 * function.  The result is palloc'ed.
 *
 * Note that this function is disguised as PyString_AsString() when
 * using Python 3.  That function retuns a pointer into the internal
 * memory of the argument, which isn't exactly the interface of this
 * function.  But in either case you get a rather short-lived
 * reference that you ought to better leave alone.
 */
static char *
PLyUnicode_AsString(PyObject *unicode)
{
	PyObject   *o = PLyUnicode_Bytes(unicode);
	char	   *rv = pstrdup(PyBytes_AsString(o));

	Py_XDECREF(o);
	return rv;
}

#if PY_MAJOR_VERSION >= 3
/*
 * Convert a C string in the PostgreSQL server encoding to a Python
 * unicode object.  Reference ownership is passed to the caller.
 */
static PyObject *
PLyUnicode_FromString(const char *s)
{
	char	   *utf8string;
	PyObject   *o;

	utf8string = (char *) pg_do_encoding_conversion((unsigned char *) s,
													strlen(s),
													GetDatabaseEncoding(),
													PG_UTF8);

	o = PyUnicode_FromString(utf8string);

	if (utf8string != s)
		pfree(utf8string);

	return o;
}

#endif   /* PY_MAJOR_VERSION >= 3 */
