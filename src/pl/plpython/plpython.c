/**********************************************************************
 * plpython.c - python as a procedural language for PostgreSQL
 *
 *	src/pl/plpython/plpython.c
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
#define PyInt_AsLong(x) PyLong_AsLong(x)
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
#include "access/transam.h"
#include "access/xact.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
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
	int32		typmod;			/* The typmod of the type */
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
	int32		typmod;			/* The typmod of the type */
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
 * and vice versa
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
	/* used to check if the type has been modified */
	Oid			typ_relid;
	TransactionId typrel_xmin;
	ItemPointerData typrel_tid;
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
	char	   *src;			/* textual procedure code, after mangling */
	char	  **argnames;		/* Argument names */
	PLyTypeInfo args[FUNC_MAX_ARGS];
	int			nargs;
	PyObject   *code;			/* compiled procedure code */
	PyObject   *statics;		/* data saved across calls, local scope */
	PyObject   *globals;		/* data saved across calls, global scope */
} PLyProcedure;


/* the procedure cache key */
typedef struct PLyProcedureKey
{
	Oid			fn_oid;			/* function OID */
	Oid			fn_rel;			/* triggered-on relation or InvalidOid */
} PLyProcedureKey;

/* the procedure cache entry */
typedef struct PLyProcedureEntry
{
	PLyProcedureKey key;		/* hash key */
	PLyProcedure *proc;
} PLyProcedureEntry;

/* explicit subtransaction data */
typedef struct PLySubtransactionData
{
	MemoryContext oldcontext;
	ResourceOwner oldowner;
} PLySubtransactionData;


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

typedef struct PLySubtransactionObject
{
	PyObject_HEAD
	bool		started;
	bool		exited;
} PLySubtransactionObject;

/* A list of all known exceptions, generated from backend/utils/errcodes.txt */
typedef struct ExceptionMap
{
	char	   *name;
	char	   *classname;
	int			sqlstate;
} ExceptionMap;

static const ExceptionMap exception_map[] = {
#include "spiexceptions.h"
	{NULL, NULL, 0}
};

/* A hash table mapping sqlstates to exceptions, for speedy lookup */
static HTAB *PLy_spi_exceptions;

typedef struct PLyExceptionEntry
{
	int			sqlstate;		/* hash key, must be first */
	PyObject   *exc;			/* corresponding exception */
} PLyExceptionEntry;


/* function declarations */

#if PY_MAJOR_VERSION >= 3
/* Use separate names to avoid clash in pg_pltemplate */
#define plpython_validator plpython3_validator
#define plpython_call_handler plpython3_call_handler
#define plpython_inline_handler plpython3_inline_handler
#endif

/* exported functions */
Datum		plpython_validator(PG_FUNCTION_ARGS);
Datum		plpython_call_handler(PG_FUNCTION_ARGS);
Datum		plpython_inline_handler(PG_FUNCTION_ARGS);
void		_PG_init(void);

PG_FUNCTION_INFO_V1(plpython_validator);
PG_FUNCTION_INFO_V1(plpython_call_handler);
PG_FUNCTION_INFO_V1(plpython_inline_handler);

/* most of the remaining of the declarations, all static */

/*
 * These should only be called once from _PG_init.	Initialize the
 * Python interpreter and global data.
 */
static void PLy_init_interp(void);
static void PLy_init_plpy(void);

/* call PyErr_SetString with a vprint interface and translation support */
static void
PLy_exception_set(PyObject *, const char *,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));

/* same, with pluralized message */
static void
PLy_exception_set_plural(PyObject *, const char *, const char *,
						 unsigned long n,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 5)))
__attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 5)));

/* like PLy_exception_set, but conserve more fields from ErrorData */
static void PLy_spi_exception_set(PyObject *excclass, ErrorData *edata);

/* Get the innermost python procedure called from the backend */
static char *PLy_procedure_name(PLyProcedure *);

/* some utility functions */
static void
PLy_elog(int, const char *,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));
static void PLy_get_spi_error_data(PyObject *exc, int *sqlerrcode, char **detail, char **hint, char **query, int *position);
static void PLy_traceback(char **, char **, int *);

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

static PLyProcedure *PLy_procedure_get(Oid fn_oid, Oid fn_rel, bool is_trigger);

static PLyProcedure *PLy_procedure_create(HeapTuple procTup,
					 Oid fn_oid, bool is_trigger);

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
static void PLy_output_record_funcs(PLyTypeInfo *, TupleDesc);

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
static Datum PLyObject_ToComposite(PLyObToDatum *, int32, PyObject *);
static Datum PLyObject_ToDatum(PLyObToDatum *, int32, PyObject *);
static Datum PLySequence_ToArray(PLyObToDatum *, int32, PyObject *);

static Datum PLyObject_ToCompositeDatum(PLyTypeInfo *, TupleDesc, PyObject *);
static Datum PLyString_ToComposite(PLyTypeInfo *, TupleDesc, PyObject *);
static Datum PLyMapping_ToComposite(PLyTypeInfo *, TupleDesc, PyObject *);
static Datum PLySequence_ToComposite(PLyTypeInfo *, TupleDesc, PyObject *);
static Datum PLyGenericObject_ToComposite(PLyTypeInfo *, TupleDesc, PyObject *);

/*
 * Currently active plpython function
 */
static PLyProcedure *PLy_curr_procedure = NULL;

/* list of explicit subtransaction data */
static List *explicit_subtransactions = NIL;

static PyObject *PLy_interp_globals = NULL;
static PyObject *PLy_interp_safe_globals = NULL;
static HTAB *PLy_procedure_cache = NULL;

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

static char PLy_subtransaction_doc[] = {
	"PostgreSQL subtransaction context manager"
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

static bool
PLy_procedure_is_trigger(Form_pg_proc procStruct)
{
	return (procStruct->prorettype == TRIGGEROID ||
			(procStruct->prorettype == OPAQUEOID &&
			 procStruct->pronargs == 0));
}

Datum
plpython_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc procStruct;
	bool		is_trigger;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	if (!check_function_bodies)
	{
		PG_RETURN_VOID();
	}

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	procStruct = (Form_pg_proc) GETSTRUCT(tuple);

	is_trigger = PLy_procedure_is_trigger(procStruct);

	ReleaseSysCache(tuple);

	/* We can't validate triggers against any particular table ... */
	PLy_procedure_get(funcoid, InvalidOid, is_trigger);

	PG_RETURN_VOID();
}

Datum
plpython_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	PLyProcedure *save_curr_proc;
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
		Oid			funcoid = fcinfo->flinfo->fn_oid;
		PLyProcedure *proc;

		if (CALLED_AS_TRIGGER(fcinfo))
		{
			Relation	tgrel = ((TriggerData *) fcinfo->context)->tg_relation;
			HeapTuple	trv;

			proc = PLy_procedure_get(funcoid, RelationGetRelid(tgrel), true);
			PLy_curr_procedure = proc;
			trv = PLy_trigger_handler(fcinfo, proc);
			retval = PointerGetDatum(trv);
		}
		else
		{
			proc = PLy_procedure_get(funcoid, InvalidOid, false);
			PLy_curr_procedure = proc;
			retval = PLy_function_handler(fcinfo, proc);
		}
	}
	PG_CATCH();
	{
		PLy_curr_procedure = save_curr_proc;
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Pop the error context stack */
	error_context_stack = plerrcontext.previous;

	PLy_curr_procedure = save_curr_proc;

	return retval;
}

Datum
plpython_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	FunctionCallInfoData fake_fcinfo;
	FmgrInfo	flinfo;
	PLyProcedure *save_curr_proc;
	PLyProcedure proc;
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

	MemSet(&proc, 0, sizeof(PLyProcedure));
	proc.pyname = PLy_strdup("__plpython_inline_block");
	proc.result.out.d.typoid = VOIDOID;

	PG_TRY();
	{
		PLy_procedure_compile(&proc, codeblock->source_text);
		PLy_curr_procedure = &proc;
		PLy_function_handler(&fake_fcinfo, &proc);
	}
	PG_CATCH();
	{
		PLy_procedure_delete(&proc);
		PLy_curr_procedure = save_curr_proc;
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	PLy_procedure_delete(&proc);

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

/*
 * Check if our cached information about a datatype is still valid
 */
static bool
PLy_procedure_argument_valid(PLyTypeInfo *arg)
{
	HeapTuple	relTup;
	bool		valid;

	/* Nothing to cache unless type is composite */
	if (arg->is_rowtype != 1)
		return true;

	/*
	 * Zero typ_relid means that we got called on an output argument of a
	 * function returning a unnamed record type; the info for it can't change.
	 */
	if (!OidIsValid(arg->typ_relid))
		return true;

	/* Else we should have some cached data */
	Assert(TransactionIdIsValid(arg->typrel_xmin));
	Assert(ItemPointerIsValid(&arg->typrel_tid));

	/* Get the pg_class tuple for the data type */
	relTup = SearchSysCache1(RELOID, ObjectIdGetDatum(arg->typ_relid));
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", arg->typ_relid);

	/* If it has changed, the cached data is not valid */
	valid = (arg->typrel_xmin == HeapTupleHeaderGetXmin(relTup->t_data) &&
			 ItemPointerEquals(&arg->typrel_tid, &relTup->t_self));

	ReleaseSysCache(relTup);

	return valid;
}

/*
 * Decide whether a cached PLyProcedure struct is still valid
 */
static bool
PLy_procedure_valid(PLyProcedure *proc, HeapTuple procTup)
{
	int			i;
	bool		valid;

	Assert(proc != NULL);

	/* If the pg_proc tuple has changed, it's not valid */
	if (!(proc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
		  ItemPointerEquals(&proc->fn_tid, &procTup->t_self)))
		return false;

	/* Else check the input argument datatypes */
	valid = true;
	for (i = 0; i < proc->nargs; i++)
	{
		valid = PLy_procedure_argument_valid(&proc->args[i]);

		/* Short-circuit on first changed argument */
		if (!valid)
			break;
	}

	/* if the output type is composite, it might have changed */
	if (valid)
		valid = PLy_procedure_argument_valid(&proc->result);

	return valid;
}


/*
 * PLyProcedure functions
 */

/*
 * PLy_procedure_get: returns a cached PLyProcedure, or creates, stores and
 * returns a new PLyProcedure.
 *
 * fn_oid is the OID of the function requested
 * fn_rel is InvalidOid or the relation this function triggers on
 * is_trigger denotes whether the function is a trigger function
 *
 * The reason that both fn_rel and is_trigger need to be passed is that when
 * trigger functions get validated we don't know which relation(s) they'll
 * be used with, so no sensible fn_rel can be passed.
 */
static PLyProcedure *
PLy_procedure_get(Oid fn_oid, Oid fn_rel, bool is_trigger)
{
	bool		use_cache = !(is_trigger && fn_rel == InvalidOid);
	HeapTuple	procTup;
	PLyProcedureKey key;
	PLyProcedureEntry *volatile entry = NULL;
	PLyProcedure *volatile proc = NULL;
	bool		found = false;

	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	/*
	 * Look for the function in the cache, unless we don't have the necessary
	 * information (e.g. during validation). In that case we just don't cache
	 * anything.
	 */
	if (use_cache)
	{
		key.fn_oid = fn_oid;
		key.fn_rel = fn_rel;
		entry = hash_search(PLy_procedure_cache, &key, HASH_ENTER, &found);
		proc = entry->proc;
	}

	PG_TRY();
	{
		if (!found)
		{
			/* Haven't found it, create a new procedure */
			proc = PLy_procedure_create(procTup, fn_oid, is_trigger);
			if (use_cache)
				entry->proc = proc;
		}
		else if (!PLy_procedure_valid(proc, procTup))
		{
			/* Found it, but it's invalid, free and reuse the cache entry */
			PLy_procedure_delete(proc);
			PLy_free(proc);
			proc = PLy_procedure_create(procTup, fn_oid, is_trigger);
			entry->proc = proc;
		}
		/* Found it and it's valid, it's fine to use it */
	}
	PG_CATCH();
	{
		/* Do not leave an uninitialised entry in the cache */
		if (use_cache)
			hash_search(PLy_procedure_cache, &key, HASH_REMOVE, NULL);
		PG_RE_THROW();
	}
	PG_END_TRY();

	ReleaseSysCache(procTup);

	return proc;
}

/*
 * Create a new PLyProcedure structure
 */
static PLyProcedure *
PLy_procedure_create(HeapTuple procTup, Oid fn_oid, bool is_trigger)
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
	rv = snprintf(procName, sizeof(procName),
				  "__plpython_procedure_%s_%u",
				  NameStr(procStruct->proname),
				  fn_oid);
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
	proc->globals = NULL;
	proc->is_setof = procStruct->proretset;
	proc->setof = NULL;
	proc->src = NULL;
	proc->argnames = NULL;

	PG_TRY();
	{
		/*
		 * get information required for output conversion of the return value,
		 * but only if this isn't a trigger.
		 */
		if (!is_trigger)
		{
			HeapTuple	rvTypeTup;
			Form_pg_type rvTypeStruct;

			rvTypeTup = SearchSysCache1(TYPEOID,
								   ObjectIdGetDatum(procStruct->prorettype));
			if (!HeapTupleIsValid(rvTypeTup))
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			rvTypeStruct = (Form_pg_type) GETSTRUCT(rvTypeTup);

			/* Disallow pseudotype result, except for void or record */
			if (rvTypeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (procStruct->prorettype == TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				else if (procStruct->prorettype != VOIDOID &&
						 procStruct->prorettype != RECORDOID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						  errmsg("PL/Python functions cannot return type %s",
								 format_type_be(procStruct->prorettype))));
			}

			if (rvTypeStruct->typtype == TYPTYPE_COMPOSITE ||
				procStruct->prorettype == RECORDOID)
			{
				/*
				 * Tuple: set up later, during first call to
				 * PLy_function_handler
				 */
				proc->result.out.d.typoid = procStruct->prorettype;
				proc->result.out.d.typmod = -1;
				proc->result.is_rowtype = 2;
			}
			else
			{
				/* do the real work */
				PLy_output_datum_func(&proc->result, rvTypeTup);
			}

			ReleaseSysCache(rvTypeTup);
		}

		/*
		 * Now get information required for input conversion of the
		 * procedure's arguments.  Note that we ignore output arguments here.
		 * If the function returns record, those I/O functions will be set up
		 * when the function is first called.
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

/*
 * Insert the procedure into the Python interpreter
 */
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
	/* Save the mangled source for later inclusion in tracebacks */
	proc->src = PLy_strdup(msrc);
	crv = PyRun_String(msrc, Py_file_input, proc->globals, NULL);
	pfree(msrc);

	if (crv != NULL)
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
		if (proc->code != NULL)
			return;
	}

	if (proc->proname)
		PLy_elog(ERROR, "could not compile PL/Python function \"%s\"",
				 proc->proname);
	else
		PLy_elog(ERROR, "could not compile anonymous PL/Python code block");
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

	mrc = palloc(mlen);
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
	if (proc->src)
		PLy_free(proc->src);
	if (proc->argnames)
		PLy_free(proc->argnames);
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

	/* Can this be an unnamed tuple? If not, then an Assert would be enough */
	if (desc->tdtypmod != -1)
		elog(ERROR, "received unnamed record type as input");

	Assert(OidIsValid(desc->tdtypeid));

	/*
	 * RECORDOID means we got called to create input functions for a tuple
	 * fetched by plpy.execute or for an anonymous record type
	 */
	if (desc->tdtypeid != RECORDOID)
	{
		HeapTuple	relTup;

		/* Get the pg_class tuple corresponding to the type of the input */
		arg->typ_relid = typeidTypeRelid(desc->tdtypeid);
		relTup = SearchSysCache1(RELOID, ObjectIdGetDatum(arg->typ_relid));
		if (!HeapTupleIsValid(relTup))
			elog(ERROR, "cache lookup failed for relation %u", arg->typ_relid);

		/* Remember XMIN and TID for later validation if cache is still OK */
		arg->typrel_xmin = HeapTupleHeaderGetXmin(relTup->t_data);
		arg->typrel_tid = relTup->t_self;

		ReleaseSysCache(relTup);
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
PLy_output_record_funcs(PLyTypeInfo *arg, TupleDesc desc)
{
	/*
	 * If the output record functions are already set, we just have to check
	 * if the record descriptor has not changed
	 */
	if ((arg->is_rowtype == 1) &&
		(arg->out.d.typmod != -1) &&
		(arg->out.d.typmod == desc->tdtypmod))
		return;

	/* bless the record to make it known to the typcache lookup code */
	BlessTupleDesc(desc);
	/* save the freshly generated typmod */
	arg->out.d.typmod = desc->tdtypmod;
	/* proceed with normal I/O function caching */
	PLy_output_tuple_funcs(arg, desc);

	/*
	 * it should change is_rowtype to 1, so we won't go through this again
	 * unless the the output record description changes
	 */
	Assert(arg->is_rowtype == 1);
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

	Assert(OidIsValid(desc->tdtypeid));

	/*
	 * RECORDOID means we got called to create output functions for an
	 * anonymous record type
	 */
	if (desc->tdtypeid != RECORDOID)
	{
		HeapTuple	relTup;

		/* Get the pg_class tuple corresponding to the type of the output */
		arg->typ_relid = typeidTypeRelid(desc->tdtypeid);
		relTup = SearchSysCache1(RELOID, ObjectIdGetDatum(arg->typ_relid));
		if (!HeapTupleIsValid(relTup))
			elog(ERROR, "cache lookup failed for relation %u", arg->typ_relid);

		/* Remember XMIN and TID for later validation if cache is still OK */
		arg->typrel_xmin = HeapTupleHeaderGetXmin(relTup->t_data);
		arg->typrel_tid = relTup->t_self;

		ReleaseSysCache(relTup);
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
	arg->typmod = -1;
	arg->typioparam = getTypeIOParam(typeTup);
	arg->typbyval = typeStruct->typbyval;

	element_type = get_element_type(arg->typoid);

	/*
	 * Select a conversion function to convert Python objects to PostgreSQL
	 * datums.	Most data types can go through the generic function.
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

	/* Composite types need their own input routine, though */
	if (typeStruct->typtype == TYPTYPE_COMPOSITE)
	{
		arg->func = PLyObject_ToComposite;
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
		arg->elm->typmod = -1;
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
	arg->typmod = -1;
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
		arg->elm->typmod = -1;
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
	arg->typ_relid = InvalidOid;
	arg->typrel_xmin = InvalidTransactionId;
	ItemPointerSetInvalid(&arg->typrel_tid);
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
	if (list == NULL)
		PLy_elog(ERROR, "could not create new list");

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
 * Convert a Python object to a composite Datum, using all supported
 * conversion methods: composite as a string, as a sequence, as a mapping or
 * as an object that has __getattr__ support.
 */
static Datum
PLyObject_ToCompositeDatum(PLyTypeInfo *info, TupleDesc desc, PyObject *plrv)
{
	Datum	datum;

    if (PyString_Check(plrv) || PyUnicode_Check(plrv))
		datum = PLyString_ToComposite(info, desc, plrv);
	else if (PySequence_Check(plrv))
		/* composite type as sequence (tuple, list etc) */
		datum = PLySequence_ToComposite(info, desc, plrv);
	else if (PyMapping_Check(plrv))
		/* composite type as mapping (currently only dict) */
		datum = PLyMapping_ToComposite(info, desc, plrv);
	else
		/* returned as smth, must provide method __getattr__(name) */
		datum = PLyGenericObject_ToComposite(info, desc, plrv);

	return datum;
}

/*
 * Convert a Python object to a PostgreSQL bool datum.	This can't go
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
 * Convert a Python object to a composite type. First look up the type's
 * description, then route the Python object through the conversion function
 * for obtaining PostgreSQL tuples.
 */
static Datum
PLyObject_ToComposite(PLyObToDatum *arg, int32 typmod, PyObject *plrv)
{
	Datum		rv;
	PLyTypeInfo info;
	TupleDesc	desc;

	if (typmod != -1)
		elog(ERROR, "received unnamed record type as input");

	/* Create a dummy PLyTypeInfo */
	MemSet(&info, 0, sizeof(PLyTypeInfo));
	PLy_typeinfo_init(&info);
	/* Mark it as needing output routines lookup */
	info.is_rowtype = 2;

	desc = lookup_rowtype_tupdesc(arg->typoid, arg->typmod);

	/*
	 * This will set up the dummy PLyTypeInfo's output conversion routines,
	 * since we left is_rowtype as 2. A future optimisation could be caching
	 * that info instead of looking it up every time a tuple is returned from
	 * the function.
	 */
	rv = PLyObject_ToCompositeDatum(&info, desc, plrv);

	PLy_typeinfo_dealloc(&info);

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


static Datum
PLyString_ToComposite(PLyTypeInfo *info, TupleDesc desc, PyObject *string)
{
    HeapTuple   typeTup;

	typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(desc->tdtypeid));
	if (!HeapTupleIsValid(typeTup))
		elog(ERROR, "cache lookup failed for type %u", desc->tdtypeid);

	PLy_output_datum_func2(&info->out.d, typeTup);

	ReleaseSysCache(typeTup);
	ReleaseTupleDesc(desc);

	return PLyObject_ToDatum(&info->out.d, info->out.d.typmod, string);
}



static Datum
PLyMapping_ToComposite(PLyTypeInfo *info, TupleDesc desc, PyObject *mapping)
{
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int i;

	Assert(PyMapping_Check(mapping));

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

		if (desc->attrs[i]->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

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

	return HeapTupleGetDatum(tuple);
}


static Datum
PLySequence_ToComposite(PLyTypeInfo *info, TupleDesc desc, PyObject *sequence)
{
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int idx;
	volatile int i;

	Assert(PySequence_Check(sequence));

	/*
	 * Check that sequence length is exactly same as PG tuple's. We actually
	 * can ignore exceeding items or assume missing ones as null but to avoid
	 * plpython developer's errors we are strict here
	 */
	idx = 0;
	for (i = 0; i < desc->natts; i++)
	{
		if (!desc->attrs[i]->attisdropped)
			idx++;
	}
	if (PySequence_Length(sequence) != idx)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("length of returned sequence did not match number of columns in row")));

	if (info->is_rowtype == 2)
		PLy_output_tuple_funcs(info, desc);
	Assert(info->is_rowtype == 1);

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(bool) * desc->natts);
	idx = 0;
	for (i = 0; i < desc->natts; ++i)
	{
		PyObject   *volatile value;
		PLyObToDatum *att;

		if (desc->attrs[i]->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		value = NULL;
		att = &info->out.r.atts[i];
		PG_TRY();
		{
			value = PySequence_GetItem(sequence, idx);
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

		idx++;
	}

	tuple = heap_form_tuple(desc, values, nulls);
	ReleaseTupleDesc(desc);
	pfree(values);
	pfree(nulls);

	return HeapTupleGetDatum(tuple);
}


static Datum
PLyGenericObject_ToComposite(PLyTypeInfo *info, TupleDesc desc, PyObject *object)
{
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int i;

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

		if (desc->attrs[i]->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

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

	return HeapTupleGetDatum(tuple);
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

static PyObject *PLy_quote_literal(PyObject *self, PyObject *args);
static PyObject *PLy_quote_nullable(PyObject *self, PyObject *args);
static PyObject *PLy_quote_ident(PyObject *self, PyObject *args);

static PyObject *PLy_subtransaction(PyObject *, PyObject *);
static PyObject *PLy_subtransaction_new(void);
static void PLy_subtransaction_dealloc(PyObject *);
static PyObject *PLy_subtransaction_enter(PyObject *, PyObject *);
static PyObject *PLy_subtransaction_exit(PyObject *, PyObject *);


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

static PyMethodDef PLy_subtransaction_methods[] = {
	{"__enter__", PLy_subtransaction_enter, METH_VARARGS, NULL},
	{"__exit__", PLy_subtransaction_exit, METH_VARARGS, NULL},
	/* user-friendly names for Python <2.6 */
	{"enter", PLy_subtransaction_enter, METH_VARARGS, NULL},
	{"exit", PLy_subtransaction_exit, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject PLy_SubtransactionType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"PLySubtransaction",		/* tp_name */
	sizeof(PLySubtransactionObject),	/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_subtransaction_dealloc, /* tp_dealloc */
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
	PLy_subtransaction_doc,		/* tp_doc */
	0,							/* tp_traverse */
	0,							/* tp_clear */
	0,							/* tp_richcompare */
	0,							/* tp_weaklistoffset */
	0,							/* tp_iter */
	0,							/* tp_iternext */
	PLy_subtransaction_methods, /* tp_tpmethods */
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

	/*
	 * escaping strings
	 */
	{"quote_literal", PLy_quote_literal, METH_VARARGS, NULL},
	{"quote_nullable", PLy_quote_nullable, METH_VARARGS, NULL},
	{"quote_ident", PLy_quote_ident, METH_VARARGS, NULL},

	/*
	 * create the subtransaction context manager
	 */
	{"subtransaction", PLy_subtransaction, METH_NOARGS, NULL},

	{NULL, NULL, 0, NULL}
};

static PyMethodDef PLy_exc_methods[] = {
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

static PyModuleDef PLy_exc_module = {
	PyModuleDef_HEAD_INIT,		/* m_base */
	"spiexceptions",			/* m_name */
	NULL,						/* m_doc */
	-1,							/* m_size */
	PLy_exc_methods,			/* m_methods */
	NULL,						/* m_reload */
	NULL,						/* m_traverse */
	NULL,						/* m_clear */
	NULL						/* m_free */
};
#endif

/* plan object methods */
static PyObject *
PLy_plan_new(void)
{
	PLyPlanObject *ob;

	if ((ob = PyObject_New(PLyPlanObject, &PLy_PlanType)) == NULL)
		return NULL;

	ob->plan = NULL;
	ob->nargs = 0;
	ob->types = NULL;
	ob->values = NULL;
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
	if (ob->values)
		PLy_free(ob->values);
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

	if ((ob = PyObject_New(PLyResultObject, &PLy_ResultType)) == NULL)
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

	nargs = list ? PySequence_Length(list) : 0;

	plan->nargs = nargs;
	plan->types = nargs ? PLy_malloc(sizeof(Oid) * nargs) : NULL;
	plan->values = nargs ? PLy_malloc(sizeof(Datum) * nargs) : NULL;
	plan->args = nargs ? PLy_malloc(sizeof(PLyTypeInfo) * nargs) : NULL;

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		int			i;

		/*
		 * the other loop might throw an exception, if PLyTypeInfo member
		 * isn't properly initialized the Py_DECREF(plan) will go boom
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

			/*
			 * set optr to NULL, so we won't try to unref it again in case of
			 * an error
			 */
			optr = NULL;

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

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * AtEOSubXact_SPI() should not have popped any SPI context, but just
		 * in case it did, make sure we remain connected.
		 */
		SPI_restore_connection();
	}
	PG_CATCH();
	{
		ErrorData  *edata;
		PLyExceptionEntry *entry;
		PyObject   *exc;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		Py_DECREF(plan);
		Py_XDECREF(optr);

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it will
		 * have left us in a disconnected state.  We need this hack to return
		 * to connected state.
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
		return NULL;
	}
	PG_END_TRY();

	Assert(plan->plan != NULL);
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

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
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
							  PLy_curr_procedure->fn_readonly, limit);
		ret = PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);

		if (nargs > 0)
			pfree(nulls);

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * AtEOSubXact_SPI() should not have popped any SPI context, but just
		 * in case it did, make sure we remain connected.
		 */
		SPI_restore_connection();
	}
	PG_CATCH();
	{
		int			k;
		ErrorData  *edata;
		PLyExceptionEntry *entry;
		PyObject   *exc;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
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

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it will
		 * have left us in a disconnected state.  We need this hack to return
		 * to connected state.
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

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		pg_verifymbstr(query, strlen(query), false);
		rv = SPI_execute(query, PLy_curr_procedure->fn_readonly, limit);
		ret = PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * AtEOSubXact_SPI() should not have popped any SPI context, but just
		 * in case it did, make sure we remain connected.
		 */
		SPI_restore_connection();
	}
	PG_CATCH();
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
		 * have left us in a disconnected state.  We need this hack to return
		 * to connected state.
		 */
		SPI_restore_connection();

		/* Look up the correct exception */
		entry = hash_search(PLy_spi_exceptions, &edata->sqlerrcode,
							HASH_FIND, NULL);
		/* We really should find it, but just in case have a fallback */
		Assert(entry != NULL);
		exc = entry ? entry->exc : PLy_exc_spi_error;
		/* Make Python raise the exception */
		PLy_spi_exception_set(exc, edata);
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

/* s = plpy.subtransaction() */
static PyObject *
PLy_subtransaction(PyObject *self, PyObject *unused)
{
	return PLy_subtransaction_new();
}

/* Allocate and initialize a PLySubtransactionObject */
static PyObject *
PLy_subtransaction_new(void)
{
	PLySubtransactionObject *ob;

	ob = PyObject_New(PLySubtransactionObject, &PLy_SubtransactionType);

	if (ob == NULL)
		return NULL;

	ob->started = false;
	ob->exited = false;

	return (PyObject *) ob;
}

/* Python requires a dealloc function to be defined */
static void
PLy_subtransaction_dealloc(PyObject *subxact)
{
}

/*
 * subxact.__enter__() or subxact.enter()
 *
 * Start an explicit subtransaction.  SPI calls within an explicit
 * subtransaction will not start another one, so you can atomically
 * execute many SPI calls and still get a controllable exception if
 * one of them fails.
 */
static PyObject *
PLy_subtransaction_enter(PyObject *self, PyObject *unused)
{
	PLySubtransactionData *subxactdata;
	MemoryContext oldcontext;
	PLySubtransactionObject *subxact = (PLySubtransactionObject *) self;

	if (subxact->started)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has already been entered");
		return NULL;
	}

	if (subxact->exited)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has already been exited");
		return NULL;
	}

	subxact->started = true;
	oldcontext = CurrentMemoryContext;

	subxactdata = PLy_malloc(sizeof(*subxactdata));
	subxactdata->oldcontext = oldcontext;
	subxactdata->oldowner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	/* Do not want to leave the previous memory context */
	MemoryContextSwitchTo(oldcontext);

	explicit_subtransactions = lcons(subxactdata, explicit_subtransactions);

	Py_INCREF(self);
	return self;
}

/*
 * subxact.__exit__(exc_type, exc, tb) or subxact.exit(exc_type, exc, tb)
 *
 * Exit an explicit subtransaction. exc_type is an exception type, exc
 * is the exception object, tb is the traceback.  If exc_type is None,
 * commit the subtransactiony, if not abort it.
 *
 * The method signature is chosen to allow subtransaction objects to
 * be used as context managers as described in
 * <http://www.python.org/dev/peps/pep-0343/>.
 */
static PyObject *
PLy_subtransaction_exit(PyObject *self, PyObject *args)
{
	PyObject   *type;
	PyObject   *value;
	PyObject   *traceback;
	PLySubtransactionData *subxactdata;
	PLySubtransactionObject *subxact = (PLySubtransactionObject *) self;

	if (!PyArg_ParseTuple(args, "OOO", &type, &value, &traceback))
		return NULL;

	if (!subxact->started)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has not been entered");
		return NULL;
	}

	if (subxact->exited)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has already been exited");
		return NULL;
	}

	if (explicit_subtransactions == NIL)
	{
		PLy_exception_set(PyExc_ValueError, "there is no subtransaction to exit from");
		return NULL;
	}

	subxact->exited = true;

	if (type != Py_None)
	{
		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
	}
	else
	{
		ReleaseCurrentSubTransaction();
	}

	subxactdata = (PLySubtransactionData *) linitial(explicit_subtransactions);
	explicit_subtransactions = list_delete_first(explicit_subtransactions);

	MemoryContextSwitchTo(subxactdata->oldcontext);
	CurrentResourceOwner = subxactdata->oldowner;
	PLy_free(subxactdata);

	/*
	 * AtEOSubXact_SPI() should not have popped any SPI context, but just in
	 * case it did, make sure we remain connected.
	 */
	SPI_restore_connection();

	Py_INCREF(Py_None);
	return Py_None;
}


/*
 * language handler and interpreter initialization
 */

/*
 * Add exceptions to the plpy module
 */

/*
 * Add all the autogenerated exceptions as subclasses of SPIError
 */
static void
PLy_generate_spi_exceptions(PyObject *mod, PyObject *base)
{
	int			i;

	for (i = 0; exception_map[i].name != NULL; i++)
	{
		bool		found;
		PyObject   *exc;
		PLyExceptionEntry *entry;
		PyObject   *sqlstate;
		PyObject   *dict = PyDict_New();

		if (dict == NULL)
			PLy_elog(ERROR, "could not generate SPI exceptions");

		sqlstate = PyString_FromString(unpack_sql_state(exception_map[i].sqlstate));
		if (sqlstate == NULL)
			PLy_elog(ERROR, "could not generate SPI exceptions");

		PyDict_SetItemString(dict, "sqlstate", sqlstate);
		Py_DECREF(sqlstate);
		exc = PyErr_NewException(exception_map[i].name, base, dict);
		PyModule_AddObject(mod, exception_map[i].classname, exc);
		entry = hash_search(PLy_spi_exceptions, &exception_map[i].sqlstate,
							HASH_ENTER, &found);
		entry->exc = exc;
		Assert(!found);
	}
}

static void
PLy_add_exceptions(PyObject *plpy)
{
	PyObject   *excmod;
	HASHCTL		hash_ctl;

#if PY_MAJOR_VERSION < 3
	excmod = Py_InitModule("spiexceptions", PLy_exc_methods);
#else
	excmod = PyModule_Create(&PLy_exc_module);
#endif
	if (PyModule_AddObject(plpy, "spiexceptions", excmod) < 0)
		PLy_elog(ERROR, "could not add the spiexceptions module");

/*
 * XXX it appears that in some circumstances the reference count of the
 * spiexceptions module drops to zero causing a Python assert failure when
 * the garbage collector visits the module. This has been observed on the
 * buildfarm. To fix this, add an additional ref for the module here.
 *
 * This shouldn't cause a memory leak - we don't want this garbage collected,
 * and this function shouldn't be called more than once per backend.
 */
	Py_INCREF(excmod);

	PLy_exc_error = PyErr_NewException("plpy.Error", NULL, NULL);
	PLy_exc_fatal = PyErr_NewException("plpy.Fatal", NULL, NULL);
	PLy_exc_spi_error = PyErr_NewException("plpy.SPIError", NULL, NULL);

	if (PLy_exc_error == NULL ||
		PLy_exc_fatal == NULL ||
		PLy_exc_spi_error == NULL)
		PLy_elog(ERROR, "could not create the base SPI exceptions");

	Py_INCREF(PLy_exc_error);
	PyModule_AddObject(plpy, "Error", PLy_exc_error);
	Py_INCREF(PLy_exc_fatal);
	PyModule_AddObject(plpy, "Fatal", PLy_exc_fatal);
	Py_INCREF(PLy_exc_spi_error);
	PyModule_AddObject(plpy, "SPIError", PLy_exc_spi_error);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(int);
	hash_ctl.entrysize = sizeof(PLyExceptionEntry);
	hash_ctl.hash = tag_hash;
	PLy_spi_exceptions = hash_create("SPI exceptions", 256,
									 &hash_ctl, HASH_ELEM | HASH_FUNCTION);

	PLy_generate_spi_exceptions(excmod, PLy_exc_spi_error);
}

#if PY_MAJOR_VERSION >= 3
/*
 * Must have external linkage, because PyMODINIT_FUNC does dllexport on
 * Windows-like platforms.
 */
PyMODINIT_FUNC PyInit_plpy(void);

PyMODINIT_FUNC
PyInit_plpy(void)
{
	PyObject   *m;

	m = PyModule_Create(&PLy_module);
	if (m == NULL)
		return NULL;

	PLy_add_exceptions(m);

	return m;
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
	HASHCTL		hash_ctl;

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

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(PLyProcedureKey);
	hash_ctl.entrysize = sizeof(PLyProcedureEntry);
	hash_ctl.hash = tag_hash;
	PLy_procedure_cache = hash_create("PL/Python procedures", 32, &hash_ctl,
									  HASH_ELEM | HASH_FUNCTION);

	explicit_subtransactions = NIL;

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
	if (PLy_interp_safe_globals == NULL)
		PLy_elog(ERROR, "could not create globals");
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
	PyObject   *plpy;

	/*
	 * initialize plpy module
	 */
	if (PyType_Ready(&PLy_PlanType) < 0)
		elog(ERROR, "could not initialize PLy_PlanType");
	if (PyType_Ready(&PLy_ResultType) < 0)
		elog(ERROR, "could not initialize PLy_ResultType");
	if (PyType_Ready(&PLy_SubtransactionType) < 0)
		elog(ERROR, "could not initialize PLy_SubtransactionType");

#if PY_MAJOR_VERSION >= 3
	plpy = PyModule_Create(&PLy_module);
	/* for Python 3 we initialized the exceptions in PyInit_plpy */
#else
	plpy = Py_InitModule("plpy", PLy_methods);
	PLy_add_exceptions(plpy);
#endif

	/* PyDict_SetItemString(plpy, "PlanType", (PyObject *) &PLy_PlanType); */

	/*
	 * initialize main module, and add plpy
	 */
	main_mod = PyImport_AddModule("__main__");
	main_dict = PyModule_GetDict(main_mod);
	plpy_mod = PyImport_AddModule("plpy");
	if (plpy_mod == NULL)
		PLy_elog(ERROR, "could not import \"plpy\" module");
	PyDict_SetItemString(main_dict, "plpy", plpy_mod);
	if (PyErr_Occurred())
		PLy_elog(ERROR, "could not import \"plpy\" module");
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

		if (!PyArg_UnpackTuple(args, "plpy.elog", 1, 1, &o))
			PLy_elog(ERROR, "could not unpack arguments in plpy.elog");
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
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/*
		 * Note: If sv came from PyString_AsString(), it points into storage
		 * owned by so.  So free so after using sv.
		 */
		Py_XDECREF(so);

		/* Make Python raise the exception */
		PLy_exception_set(PLy_exc_error, "%s", edata->message);
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


static PyObject *
PLy_quote_literal(PyObject *self, PyObject *args)
{
	const char *str;
	char	   *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "s", &str))
		return NULL;

	quoted = quote_literal_cstr(str);
	ret = PyString_FromString(quoted);
	pfree(quoted);

	return ret;
}

static PyObject *
PLy_quote_nullable(PyObject *self, PyObject *args)
{
	const char *str;
	char	   *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "z", &str))
		return NULL;

	if (str == NULL)
		return PyString_FromString("NULL");

	quoted = quote_literal_cstr(str);
	ret = PyString_FromString(quoted);
	pfree(quoted);

	return ret;
}

static PyObject *
PLy_quote_ident(PyObject *self, PyObject *args)
{
	const char *str;
	const char *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "s", &str))
		return NULL;

	quoted = quote_identifier(str);
	ret = PyString_FromString(quoted);

	return ret;
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

/* Emit a PG error or notice, together with any available info about
 * the current Python error, previously set by PLy_exception_set().
 * This should be used to propagate Python errors into PG.	If fmt is
 * NULL, the Python error becomes the primary error message, otherwise
 * it becomes the detail.  If there is a Python traceback, it is put
 * in the context.
 */
static void
PLy_elog(int elevel, const char *fmt,...)
{
	char	   *xmsg;
	char	   *tbmsg;
	int			tb_depth;
	StringInfoData emsg;
	PyObject   *exc,
			   *val,
			   *tb;
	const char *primary = NULL;
	int        sqlerrcode = 0;
	char	   *detail = NULL;
	char	   *hint = NULL;
	char	   *query = NULL;
	int			position = 0;

	PyErr_Fetch(&exc, &val, &tb);
	if (exc != NULL)
	{
		if (PyErr_GivenExceptionMatches(val, PLy_exc_spi_error))
			PLy_get_spi_error_data(val, &sqlerrcode, &detail, &hint, &query, &position);
		else if (PyErr_GivenExceptionMatches(val, PLy_exc_fatal))
			elevel = FATAL;
	}
	PyErr_Restore(exc, val, tb);

	PLy_traceback(&xmsg, &tbmsg, &tb_depth);

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
		primary = emsg.data;

		/* Since we have a format string, we cannot have a SPI detail. */
		Assert(detail == NULL);

		/* If there's an exception message, it goes in the detail. */
		if (xmsg)
			detail = xmsg;
	}
	else
	{
		if (xmsg)
			primary = xmsg;
	}

	PG_TRY();
	{
		ereport(elevel,
				(errcode(sqlerrcode ? sqlerrcode : ERRCODE_INTERNAL_ERROR),
				 errmsg_internal("%s", primary ? primary : "no exception data"),
				 (detail) ? errdetail_internal("%s", detail) : 0,
				 (tb_depth > 0 && tbmsg) ? errcontext("%s", tbmsg) : 0,
				 (hint) ? errhint("%s", hint) : 0,
				 (query) ? internalerrquery(query) : 0,
				 (position) ? internalerrposition(position) : 0));
	}
	PG_CATCH();
	{
		if (fmt)
			pfree(emsg.data);
		if (xmsg)
			pfree(xmsg);
		if (tbmsg)
			pfree(tbmsg);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (fmt)
		pfree(emsg.data);
	if (xmsg)
		pfree(xmsg);
	if (tbmsg)
		pfree(tbmsg);
}

/*
 * Extract the error data from a SPIError
 */
static void
PLy_get_spi_error_data(PyObject *exc, int* sqlerrcode, char **detail, char **hint, char **query, int *position)
{
	PyObject   *spidata = NULL;

	spidata = PyObject_GetAttrString(exc, "spidata");
	if (!spidata)
		goto cleanup;

	if (!PyArg_ParseTuple(spidata, "izzzi", sqlerrcode, detail, hint, query, position))
		goto cleanup;

cleanup:
	PyErr_Clear();
	/* no elog here, we simply won't report the errhint, errposition etc */
	Py_XDECREF(spidata);
}

/*
 * Get the given source line as a palloc'd string
 */
static char *
get_source_line(const char *src, int lineno)
{
	const char *s = NULL;
	const char *next = src;
	int			current = 0;

	/* sanity check */
	if (lineno <= 0)
		return NULL;

	while (current < lineno)
	{
		s = next;
		next = strchr(s + 1, '\n');
		current++;
		if (next == NULL)
			break;
	}

	if (current != lineno)
		return NULL;

	while (*s && isspace((unsigned char) *s))
		s++;

	if (next == NULL)
		return pstrdup(s);

	/*
	 * Sanity check, next < s if the line was all-whitespace, which should
	 * never happen if Python reported a frame created on that line, but check
	 * anyway.
	 */
	if (next < s)
		return NULL;

	return pnstrdup(s, next - s);
}

/*
 * Extract a Python traceback from the current exception.
 *
 * The exception error message is returned in xmsg, the traceback in
 * tbmsg (both as palloc'd strings) and the traceback depth in
 * tb_depth.
 */
static void
PLy_traceback(char **xmsg, char **tbmsg, int *tb_depth)
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
	StringInfoData tbstr;

	/*
	 * get the current exception
	 */
	PyErr_Fetch(&e, &v, &tb);

	/*
	 * oops, no exception, return
	 */
	if (e == NULL)
	{
		*xmsg = NULL;
		*tbmsg = NULL;
		*tb_depth = 0;

		return;
	}

	PyErr_NormalizeException(&e, &v, &tb);

	/*
	 * Format the exception and its value and put it in xmsg.
	 */

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

	*xmsg = xstr.data;

	/*
	 * Now format the traceback and put it in tbmsg.
	 */

	*tb_depth = 0;
	initStringInfo(&tbstr);
	/* Mimick Python traceback reporting as close as possible. */
	appendStringInfoString(&tbstr, "Traceback (most recent call last):");
	while (tb != NULL && tb != Py_None)
	{
		PyObject   *volatile tb_prev = NULL;
		PyObject   *volatile frame = NULL;
		PyObject   *volatile code = NULL;
		PyObject   *volatile name = NULL;
		PyObject   *volatile lineno = NULL;
		PyObject   *volatile filename = NULL;

		PG_TRY();
		{
			lineno = PyObject_GetAttrString(tb, "tb_lineno");
			if (lineno == NULL)
				elog(ERROR, "could not get line number from Python traceback");

			frame = PyObject_GetAttrString(tb, "tb_frame");
			if (frame == NULL)
				elog(ERROR, "could not get frame from Python traceback");

			code = PyObject_GetAttrString(frame, "f_code");
			if (code == NULL)
				elog(ERROR, "could not get code object from Python frame");

			name = PyObject_GetAttrString(code, "co_name");
			if (name == NULL)
				elog(ERROR, "could not get function name from Python code object");

			filename = PyObject_GetAttrString(code, "co_filename");
			if (filename == NULL)
				elog(ERROR, "could not get file name from Python code object");
		}
		PG_CATCH();
		{
			Py_XDECREF(frame);
			Py_XDECREF(code);
			Py_XDECREF(name);
			Py_XDECREF(lineno);
			Py_XDECREF(filename);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* The first frame always points at <module>, skip it. */
		if (*tb_depth > 0)
		{
			char	   *proname;
			char	   *fname;
			char	   *line;
			char	   *plain_filename;
			long		plain_lineno;

			/*
			 * The second frame points at the internal function, but to mimick
			 * Python error reporting we want to say <module>.
			 */
			if (*tb_depth == 1)
				fname = "<module>";
			else
				fname = PyString_AsString(name);

			proname = PLy_procedure_name(PLy_curr_procedure);
			plain_filename = PyString_AsString(filename);
			plain_lineno = PyInt_AsLong(lineno);

			if (proname == NULL)
				appendStringInfo(
				&tbstr, "\n  PL/Python anonymous code block, line %ld, in %s",
								 plain_lineno - 1, fname);
			else
				appendStringInfo(
					&tbstr, "\n  PL/Python function \"%s\", line %ld, in %s",
								 proname, plain_lineno - 1, fname);

			/*
			 * function code object was compiled with "<string>" as the
			 * filename
			 */
			if (PLy_curr_procedure && plain_filename != NULL &&
				strcmp(plain_filename, "<string>") == 0)
			{
				/*
				 * If we know the current procedure, append the exact line
				 * from the source, again mimicking Python's traceback.py
				 * module behavior.  We could store the already line-split
				 * source to avoid splitting it every time, but producing a
				 * traceback is not the most important scenario to optimize
				 * for.  But we do not go as far as traceback.py in reading
				 * the source of imported modules.
				 */
				line = get_source_line(PLy_curr_procedure->src, plain_lineno);
				if (line)
				{
					appendStringInfo(&tbstr, "\n    %s", line);
					pfree(line);
				}
			}
		}

		Py_DECREF(frame);
		Py_DECREF(code);
		Py_DECREF(name);
		Py_DECREF(lineno);
		Py_DECREF(filename);

		/* Release the current frame and go to the next one. */
		tb_prev = tb;
		tb = PyObject_GetAttrString(tb, "tb_next");
		Assert(tb_prev != Py_None);
		Py_DECREF(tb_prev);
		if (tb == NULL)
			elog(ERROR, "could not traverse Python traceback");
		(*tb_depth)++;
	}

	/* Return the traceback. */
	*tbmsg = tbstr.data;

	Py_XDECREF(e_type_o);
	Py_XDECREF(e_module_o);
	Py_XDECREF(vob);
	Py_XDECREF(v);
	Py_DECREF(e);
}

/* python module code */

/* some dumb utility functions */
static void *
PLy_malloc(size_t bytes)
{
	/* We need our allocations to be long-lived, so use TopMemoryContext */
	return MemoryContextAlloc(TopMemoryContext, bytes);
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
	pfree(ptr);
}

/*
 * Convert a Python unicode object to a Python string/bytes object in
 * PostgreSQL server encoding.	Reference ownership is passed to the
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
 * using Python 3.	That function retuns a pointer into the internal
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
 * unicode object.	Reference ownership is passed to the caller.
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

#if PY_MAJOR_VERSION < 3

/* Define aliases plpython2_call_handler etc */
Datum		plpython2_call_handler(PG_FUNCTION_ARGS);
Datum		plpython2_inline_handler(PG_FUNCTION_ARGS);
Datum		plpython2_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plpython2_call_handler);

Datum
plpython2_call_handler(PG_FUNCTION_ARGS)
{
	return plpython_call_handler(fcinfo);
}

PG_FUNCTION_INFO_V1(plpython2_inline_handler);

Datum
plpython2_inline_handler(PG_FUNCTION_ARGS)
{
	return plpython_inline_handler(fcinfo);
}

PG_FUNCTION_INFO_V1(plpython2_validator);

Datum
plpython2_validator(PG_FUNCTION_ARGS)
{
	/* call plpython validator with our fcinfo so it gets our oid */
	return plpython_validator(fcinfo);
}

#endif   /* PY_MAJOR_VERSION < 3 */
