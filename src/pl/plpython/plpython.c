/*                                               -*- C -*-
 *
 * plpython.c - python as a procedural language for PostgreSQL
 *
 * IDENTIFICATION
 *
 * This software is copyright by Andrew Bosma
 * but is really shameless cribbed from pltcl.c by Jan Weick, and
 * plperl.c by Mark Hollomon.
 *
 * The author hereby grants permission to use, copy, modify,
 * distribute, and license this software and its documentation for any
 * purpose, provided that existing copyright notices are retained in
 * all copies and that this notice is included verbatim in any
 * distributions. No written agreement, license, or royalty fee is
 * required for any of the authorized uses.  Modifications to this
 * software may be copyrighted by their author and need not follow the
 * licensing terms described here, provided that the new terms are
 * clearly indicated on the first page of each file where they apply.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY
 * FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE, ITS DOCUMENTATION, OR ANY
 * DERIVATIVES THEREOF, EVEN IF THE AUTHOR HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
 * NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS,
 * AND THE AUTHOR AND DISTRIBUTORS HAVE NO OBLIGATION TO PROVIDE
 * MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 **********************************************************************/

/* system stuff
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <setjmp.h>

/* postgreSQL stuff
 */
#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/elog.h"
#include "fmgr.h"
#include "access/heapam.h"

#include "tcop/tcopprot.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"

#include <Python.h>
#include "plpython.h"

/* convert Postgresql Datum or tuple into a PyObject.
 * input to Python.  Tuples are converted to dictionary
 * objects.
 */

typedef PyObject *(*PLyDatumToObFunc) (const char *);

typedef struct PLyDatumToOb {
  PLyDatumToObFunc func;
  FmgrInfo typfunc;
  Oid typoutput;
  Oid typelem;
  int2 typlen;
} PLyDatumToOb;

typedef struct PLyTupleToOb {
  PLyDatumToOb *atts;
  int natts;
} PLyTupleToOb;

typedef union PLyTypeInput {
  PLyDatumToOb d;
  PLyTupleToOb r;
} PLyTypeInput;

/* convert PyObject to a Postgresql Datum or tuple.
 * output from Python
 */
typedef struct PLyObToDatum {
  FmgrInfo typfunc;
  Oid typelem;
  int2 typlen;
} PLyObToDatum;

typedef struct PLyObToTuple {
  PLyObToDatum *atts;
  int natts;
} PLyObToTuple;

typedef union PLyTypeOutput {
  PLyObToDatum d;
  PLyObToTuple r;
} PLyTypeOutput;

/* all we need to move Postgresql data to Python objects,
 * and vis versa
 */
typedef struct PLyTypeInfo {
  PLyTypeInput in;
  PLyTypeOutput out;
  int is_rel;
} PLyTypeInfo;


/* cached procedure data
 */
typedef struct PLyProcedure {
  char *proname;
  PLyTypeInfo result; /* also used to store info for trigger tuple type */
  PLyTypeInfo args[FUNC_MAX_ARGS];
  int nargs;
  PyObject *interp;  /* restricted interpreter instance */
  PyObject *reval;   /* interpreter return */
  PyObject *code;    /* compiled procedure code */
  PyObject *statics; /* data saved across calls, local scope */
  PyObject *globals; /* data saved across calls, global score */
  PyObject *me;      /* PyCObject containing pointer to this PLyProcedure */
} PLyProcedure;


/* Python objects.  
 */
typedef struct PLyPlanObject {
  PyObject_HEAD;
  void *plan;        /* return of an SPI_saveplan */
  int nargs;
  Oid *types;
  Datum *values;
  PLyTypeInfo *args;
} PLyPlanObject;

typedef struct PLyResultObject {
  PyObject_HEAD;
  /* HeapTuple *tuples; */
  PyObject *nrows;  /* number of rows returned by query */
  PyObject *rows;   /* data rows, or None if no data returned */
  PyObject *status; /* query status, SPI_OK_*, or SPI_ERR_* */
} PLyResultObject;


/* function declarations
 */

/* the only exported function, with the magic telling Postgresql
 * what function call interface it implements.
 */
Datum plpython_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(plpython_call_handler);

/* most of the remaining of the declarations, all static
 */

/* these should only be called once at the first call
 * of plpython_call_handler.  initialize the python interpreter
 * and global data.
 */
static void PLy_init_all(void);
static void PLy_init_interp(void);
static void PLy_init_safe_interp(void);
static void PLy_init_plpy(void);

/* error handler.  collects the current Python exception, if any,
 * and appends it to the error and sends it to elog
 */
static void PLy_elog(int, const char *, ...);

/* call PyErr_SetString with a vprint interface
 */
static void PLy_exception_set(PyObject *, const char *, ...)
     __attribute__ ((format (printf, 2, 3)));

/* some utility functions
 */
static void *PLy_malloc(size_t);
static void *PLy_realloc(void *, size_t);
static void PLy_free(void *);

/* sub handlers for functions and triggers
 */
static Datum PLy_function_handler(PG_FUNCTION_ARGS, PLyProcedure *);
static HeapTuple PLy_trigger_handler(PG_FUNCTION_ARGS, PLyProcedure *);

static PyObject *PLy_function_build_args(PG_FUNCTION_ARGS, PLyProcedure *);
static PyObject *PLy_trigger_build_args(PG_FUNCTION_ARGS, PLyProcedure *,
					HeapTuple *);
static HeapTuple PLy_modify_tuple(PLyProcedure *, PyObject *,
				  TriggerData *, HeapTuple);

static PyObject *PLy_procedure_call(PLyProcedure *, char *, PyObject *);

/* returns a cached PLyProcedure, or creates, stores and returns
 * a new PLyProcedure.
 */
static PLyProcedure *PLy_procedure_get(PG_FUNCTION_ARGS, bool);

static PLyProcedure *PLy_procedure_create(PG_FUNCTION_ARGS, bool, char *);
static void PLy_procedure_compile(PLyProcedure *, const char *);
static char *PLy_procedure_munge_source(const char *, const char *);
static PLyProcedure *PLy_procedure_new(const char *name);
static void PLy_procedure_delete(PLyProcedure *);

static void PLy_typeinfo_init(PLyTypeInfo *);
static void PLy_typeinfo_dealloc(PLyTypeInfo *);
static void PLy_output_datum_func(PLyTypeInfo *, Form_pg_type);
static void PLy_output_datum_func2(PLyObToDatum *, Form_pg_type);
static void PLy_input_datum_func(PLyTypeInfo *, Form_pg_type);
static void PLy_input_datum_func2(PLyDatumToOb *, Form_pg_type);
static void PLy_output_tuple_funcs(PLyTypeInfo *, TupleDesc);
static void PLy_input_tuple_funcs(PLyTypeInfo *, TupleDesc);

/* conversion functions
 */
static PyObject *PLyDict_FromTuple(PLyTypeInfo *, HeapTuple, TupleDesc);
static PyObject *PLyBool_FromString(const char *);
static PyObject *PLyFloat_FromString(const char *);
static PyObject *PLyInt_FromString(const char *);
static PyObject *PLyString_FromString(const char *);


/* global data
 */
static int PLy_first_call = 1;
static volatile int PLy_call_level = 0;

/* this gets modified in plpython_call_handler and PLy_elog.
 * test it any old where, but do NOT modify it anywhere except
 * those two functions
 */
static volatile int PLy_restart_in_progress = 0;

static PyObject *PLy_interp_globals = NULL;
static PyObject *PLy_interp_safe = NULL;
static PyObject *PLy_interp_safe_globals = NULL;
static PyObject *PLy_importable_modules = NULL;
static PyObject *PLy_procedure_cache = NULL;
static char *PLy_procedure_fmt = "__plpython_procedure_%s_%u";

char *PLy_importable_modules_list[] = {
  "array",
  "bisect",
  "calendar",
  "cmath",
  "errno",
  "marshal",
  "math",
  "md5",
  "mpz",
  "operator",
  "pickle",
  "random",
  "re",
  "sha",
  "string",
  "StringIO",
  "time",
  "whrandom",
  "zlib"
};

/* Python exceptions
 */
PyObject *PLy_exc_error = NULL;
PyObject *PLy_exc_fatal = NULL;
PyObject *PLy_exc_spi_error = NULL;

/* some globals for the python module
 */
static char PLy_plan_doc[] = {
  "Store a PostgreSQL plan"
};

static char PLy_result_doc[] = {
  "Results of a PostgreSQL query"
};


#if DEBUG_EXC
volatile int exc_save_calls = 0;
volatile int exc_restore_calls = 0;
volatile int func_enter_calls = 0;
volatile int func_leave_calls = 0;
#endif

/* the function definitions
 */
Datum
plpython_call_handler(PG_FUNCTION_ARGS)
{
  DECLARE_EXC();
  Datum retval;
  bool is_trigger;
  PLyProcedure *volatile proc = NULL;

  enter();

  if (PLy_first_call)
    PLy_init_all();

  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "plpython: Unable to connect to SPI manager");

  CALL_LEVEL_INC();
  is_trigger = CALLED_AS_TRIGGER(fcinfo);

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();
      CALL_LEVEL_DEC();
      if (PLy_call_level == 0)
	{
	  PLy_restart_in_progress = 0;
	  PyErr_Clear();
	}
      else
	PLy_restart_in_progress += 1;
      if (proc)
	{ Py_DECREF(proc->me); }
      RERAISE_EXC();
    }

  /*elog(NOTICE, "PLy_restart_in_progress is %d", PLy_restart_in_progress);*/

  proc = PLy_procedure_get(fcinfo, is_trigger);

  if (is_trigger)
    {
      HeapTuple trv = PLy_trigger_handler(fcinfo, proc);
      retval = PointerGetDatum(trv);
    }
  else
    retval = PLy_function_handler(fcinfo, proc);

  CALL_LEVEL_DEC();
  RESTORE_EXC();
  
  Py_DECREF(proc->me);
  refc(proc->me);

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
 * to take no arguments and return an argument of type opaque.
 */
HeapTuple
PLy_trigger_handler(PG_FUNCTION_ARGS, PLyProcedure *proc)
{
  DECLARE_EXC();
  HeapTuple rv = NULL;
  PyObject *plargs = NULL;
  PyObject *plrv = NULL;

  enter();

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();

      Py_XDECREF(plargs);
      Py_XDECREF(plrv);

      RERAISE_EXC();
    }

  plargs = PLy_trigger_build_args(fcinfo, proc, &rv);
  plrv = PLy_procedure_call(proc, "TD", plargs);

  /* Disconnect from SPI manager
   */
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "plpython: SPI_finish failed");

  if (plrv == NULL)
    elog(FATAL, "Aiieee, PLy_procedure_call returned NULL");

  if (PLy_restart_in_progress)
    elog(FATAL, "Aiieee, restart in progress not expected");

  /* return of None means we're happy with the tuple
   */
  if (plrv != Py_None)
    {
      char *srv;

      if (!PyString_Check(plrv))
	elog(ERROR, "plpython: Expected trigger to return None or a String");

      srv = PyString_AsString(plrv);
      if (strcasecmp(srv, "SKIP") == 0)
	rv = NULL;
      else if (strcasecmp(srv, "MODIFY") == 0)
	{
	  TriggerData *tdata = (TriggerData *) fcinfo->context;

	  if ((TRIGGER_FIRED_BY_INSERT(tdata->tg_event)) ||
	      (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event)))
	    {
	      rv = PLy_modify_tuple(proc, plargs, tdata, rv);
	    }
	  else
	    elog(NOTICE,"plpython: Ignoring modified tuple in DELETE trigger");
	}
      else if (strcasecmp(srv, "OK"))
	{
	  /* hmmm, perhaps they only read the pltcl page, not a surprising
	   * thing since i've written no documentation, so accept a
	   * belated OK
	   */
	  elog(ERROR, "plpython: Expected return to be 'SKIP' or 'MODIFY'");
	}
    }

  Py_DECREF(plargs);
  Py_DECREF(plrv);

  RESTORE_EXC();

  return rv;
}

HeapTuple
PLy_modify_tuple(PLyProcedure *proc, PyObject *pltd, TriggerData *tdata,
		 HeapTuple otup)
{
  DECLARE_EXC();
  PyObject *plntup, *plkeys, *platt, *plval, *plstr;
  HeapTuple rtup;
  int natts, i, j, attn, atti;
  int *modattrs;
  Datum *modvalues;
  char *modnulls;
  TupleDesc tupdesc;

  plntup = plkeys = platt = plval = plstr = NULL;
  modattrs = NULL;
  modvalues = NULL;
  modnulls = NULL;

  enter();

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();

      Py_XDECREF(plntup);
      Py_XDECREF(plkeys);
      Py_XDECREF(platt);
      Py_XDECREF(plval);
      Py_XDECREF(plstr);

      if (modnulls)
	pfree(modnulls);
      if (modvalues)
	pfree(modvalues);
      if (modattrs)
	pfree(modattrs);

      RERAISE_EXC();
    }

  if ((plntup = PyDict_GetItemString(pltd, "new")) == NULL)
    elog(ERROR, "plpython: TD[\"new\"] deleted, unable to modify tuple");
  if (!PyDict_Check(plntup))
    elog(ERROR, "plpython: TD[\"new\"] is not a dictionary object");
  Py_INCREF(plntup);

  plkeys = PyDict_Keys(plntup);
  natts = PyList_Size(plkeys);

  if (natts != proc->result.out.r.natts)
    elog(ERROR, "plpython: TD[\"new\"] has an incorrect number of keys.");

  modattrs = palloc(natts * sizeof(int));
  modvalues = palloc(natts * sizeof(Datum));
  for (i = 0; i < natts; i++)
    {
      modattrs[i] = i + 1;
      modvalues[i] = (Datum) NULL;
    }
  modnulls = palloc(natts + 1);
  memset(modnulls, 'n', natts);
  modnulls[natts] = '\0';

  tupdesc = tdata->tg_relation->rd_att;
      
  for (j = 0; j < natts; j++)
    {
      char *src;

      platt = PyList_GetItem(plkeys, j);
      if (!PyString_Check(platt))
	elog(ERROR, "plpython: attribute is not a string");
      attn = modattrs[j] = SPI_fnumber(tupdesc, PyString_AsString(platt));
	  
      if (attn == SPI_ERROR_NOATTRIBUTE)
	elog(ERROR, "plpython: invalid attribute `%s' in tuple.",
	     PyString_AsString(platt));
      atti = attn - 1;

      plval = PyDict_GetItem(plntup, platt);
      if (plval == NULL)
	elog(FATAL, "plpython: interpreter is probably corrupted");

      Py_INCREF(plval);

      if (plval != Py_None)
	{
	  plstr = PyObject_Str(plval);
	  src = PyString_AsString(plstr);

	  modvalues[j] = FunctionCall3(&proc->result.out.r.atts[atti].typfunc,
				       CStringGetDatum(src),
				       proc->result.out.r.atts[atti].typelem,
				       proc->result.out.r.atts[atti].typlen);
	  modnulls[j] = ' ';
	  
	  Py_DECREF(plstr);
	  plstr = NULL;
	}
      Py_DECREF(plval);
      plval = NULL;

    }
  rtup = SPI_modifytuple(tdata->tg_relation, otup, natts, modattrs,
			 modvalues, modnulls);

  /* FIXME -- these leak if not explicity pfree'd by other elog calls, no?
   */
  pfree(modattrs);
  pfree(modvalues);
  pfree(modnulls);
  
  if (rtup == NULL)
    elog(ERROR, "plpython: SPI_modifytuple failed -- error %d", SPI_result);
  
  Py_DECREF(plntup);
  Py_DECREF(plkeys);

  RESTORE_EXC();

  return rtup;
}

PyObject *
PLy_trigger_build_args(PG_FUNCTION_ARGS, PLyProcedure *proc, HeapTuple *rv)
{
  DECLARE_EXC();
  TriggerData *tdata;
  PyObject *pltname, *pltevent, *pltwhen, *pltlevel;
  PyObject *pltargs, *pytnew, *pytold;
  PyObject *pltdata = NULL;  

  enter();

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();

      Py_XDECREF(pltdata);

      RERAISE_EXC();
    }

  tdata = (TriggerData *) fcinfo->context;

  pltdata = PyDict_New();
  if (!pltdata)
    PLy_elog(ERROR, "Unable to build arguments for trigger procedure");
  
  pltname = PyString_FromString(tdata->tg_trigger->tgname);
  PyDict_SetItemString(pltdata, "name", pltname);
  Py_DECREF(pltname);

  if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
    pltwhen = PyString_FromString("BEFORE");
  else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
    pltwhen = PyString_FromString("AFTER");
  else
    pltwhen = PyString_FromString("UNKNOWN");
  PyDict_SetItemString(pltdata, "when", pltwhen);
  Py_DECREF(pltwhen);

  if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
    pltlevel = PyString_FromString("ROW");
  else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
    pltlevel = PyString_FromString("STATEMENT");
  else
    pltlevel = PyString_FromString("UNKNOWN");
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
      pltevent = PyString_FromString("UNKNOWN");
      PyDict_SetItemString(pltdata, "old", Py_None);
      PyDict_SetItemString(pltdata, "new", Py_None);
      *rv = tdata->tg_trigtuple;
    }
  PyDict_SetItemString(pltdata, "event", pltevent);
  Py_DECREF(pltevent);

  if (tdata->tg_trigger->tgnargs)
    {
      /* all strings...
       */
      int i;
      PyObject *pltarg;

      pltargs = PyList_New(tdata->tg_trigger->tgnargs);
      for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
	{
	  pltarg = PyString_FromString(tdata->tg_trigger->tgargs[i]);
	  /* stolen, don't Py_DECREF
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

  RESTORE_EXC();

  return pltdata;
}



/* function handler and friends
 */
Datum
PLy_function_handler(PG_FUNCTION_ARGS, PLyProcedure *proc)
{
  DECLARE_EXC();
  Datum rv;
  PyObject *plargs = NULL;
  PyObject *plrv = NULL;
  PyObject *plrv_so = NULL;
  char *plrv_sc;

  enter();

  /*
   * setup to catch elog in while building function arguments,
   * and DECREF the plargs if the function call fails
   */
  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();

      Py_XDECREF(plargs);
      Py_XDECREF(plrv);
      Py_XDECREF(plrv_so);

      RERAISE_EXC();
    }

  plargs = PLy_function_build_args(fcinfo, proc);
  plrv = PLy_procedure_call(proc, "args", plargs);

  /* Disconnect from SPI manager and then create the return
   * values datum (if the input function does a palloc for it
   * this must not be allocated in the SPI memory context
   * because SPI_finish would free it).
   */
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "plpython: SPI_finish failed");

  if (plrv == NULL)
    { 
      elog(FATAL, "Aiieee, PLy_procedure_call returned NULL");
#if 0
      if (!PLy_restart_in_progress)
	PLy_elog(ERROR, "plpython: Function \"%s\" failed.", proc->proname);

      /* FIXME is this dead code?  i'm pretty sure it is for unnested
       * calls, but not for nested calls
       */
      RAISE_EXC(1);
#endif
    }

  /* convert the python PyObject to a postgresql Datum
   * FIXME returning a NULL, ie PG_RETURN_NULL() blows the backend
   * to small messy bits...   it this a bug or expected?  so just
   * call with the string value of None for now
   */

  if (plrv == Py_None)
    {
      fcinfo->isnull = true;
      rv = (Datum) NULL;
    }
  else
    {
      fcinfo->isnull = false;
      plrv_so = PyObject_Str(plrv);
      plrv_sc = PyString_AsString(plrv_so);
      rv = FunctionCall3(&proc->result.out.d.typfunc,
			 PointerGetDatum(plrv_sc),
			 proc->result.out.d.typelem,
			 proc->result.out.d.typlen);
    }

  RESTORE_EXC();

  Py_XDECREF(plargs);
  Py_DECREF(plrv);
  Py_XDECREF(plrv_so);

  return rv;
}

PyObject *
PLy_procedure_call(PLyProcedure *proc, char *kargs, PyObject *vargs)
{
  PyObject *rv;
  
  enter();

  PyDict_SetItemString(proc->globals, kargs, vargs);
  rv = PyObject_CallFunction(proc->reval, "O", proc->code);

  if ((rv == NULL) || (PyErr_Occurred()))
    {
      Py_XDECREF(rv);
      if (!PLy_restart_in_progress)
	PLy_elog(ERROR, "Call of function `%s' failed.", proc->proname);
      RAISE_EXC(1);
    }

  return rv;
}

PyObject *
PLy_function_build_args(PG_FUNCTION_ARGS, PLyProcedure *proc)
{
  DECLARE_EXC();
  PyObject *arg = NULL;
  PyObject *args = NULL;
  int i;

  enter();

  /* FIXME -- if the setjmp setup is expensive, add the arg and
   * args field to the procedure struct and cleanup at the
   * start of the next call
   */
  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();
      Py_XDECREF(arg);
      Py_XDECREF(args);

      RERAISE_EXC();
    }
  
  args = PyList_New(proc->nargs);
  for (i = 0; i < proc->nargs; i++)
    {
      if (proc->args[i].is_rel == 1)
	{
	  TupleTableSlot *slot = (TupleTableSlot *) fcinfo->arg[i];
	  arg = PLyDict_FromTuple(&(proc->args[i]), slot->val,
				  slot->ttc_tupleDescriptor);
	}
      else
	{
	  if (!fcinfo->argnull[i])
	    {
	      char *ct;
	      Datum dt;

	      dt = FunctionCall3(&(proc->args[i].in.d.typfunc),
				 fcinfo->arg[i],
				 proc->args[i].in.d.typelem,
				 proc->args[i].in.d.typlen);
	      ct = DatumGetCString(dt);
	      arg = (proc->args[i].in.d.func)(ct);
	      pfree(ct);
	    }
	  else
	    arg = NULL;
	}

      if (arg == NULL)
	{
	  Py_INCREF(Py_None);
	  arg = Py_None;
	}

      /* FIXME -- error check this
       */
      PyList_SetItem(args, i, arg);
    }

  RESTORE_EXC();

  return args;
}


/* PLyProcedure functions
 */
PLyProcedure *
PLy_procedure_get(PG_FUNCTION_ARGS, bool is_trigger)
{
  char key[128];
  PyObject *plproc;
  PLyProcedure *proc;
  int rv;

  enter();

  rv = snprintf(key, sizeof(key), "%u", fcinfo->flinfo->fn_oid);
  if ((rv >= sizeof(key)) || (rv < 0))
    elog(FATAL, "plpython: Buffer overrun in %s:%d", __FILE__, __LINE__);
  
  plproc = PyDict_GetItemString(PLy_procedure_cache, key);
  if (plproc == NULL)
    return PLy_procedure_create(fcinfo, is_trigger, key);

  Py_INCREF(plproc);
  if (!PyCObject_Check(plproc))
    elog(FATAL, "plpython: Expected a PyCObject, didn't get one");

  mark();

  proc = PyCObject_AsVoidPtr(plproc);
  if (proc->me != plproc)
    elog(FATAL, "plpython: Aiieee, proc->me != plproc");

  return proc;
}

PLyProcedure *
PLy_procedure_create(PG_FUNCTION_ARGS, bool is_trigger, char *key)
{
  char procName[256];
  DECLARE_EXC();
  HeapTuple procTup;
  Form_pg_proc procStruct;
  Oid fn_oid;
  PLyProcedure *volatile proc;
  char *volatile procSource = NULL;
  Datum procDatum;
  int i, rv;

  enter();

  fn_oid = fcinfo->flinfo->fn_oid;
  procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
  if (!HeapTupleIsValid(procTup))
    elog(ERROR, "plpython: cache lookup for procedure \"%u\" failed", fn_oid);
  procStruct = (Form_pg_proc) GETSTRUCT(procTup);

  rv = snprintf(procName, sizeof(procName), PLy_procedure_fmt,
		NameStr(procStruct->proname), fn_oid);
  if ((rv >= sizeof(procName)) || (rv < 0))
    elog(FATAL, "plpython: Procedure name would overrun buffer");

  proc = PLy_procedure_new(procName);
  
  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();
      PLy_procedure_delete(proc);
      if (procSource)
	pfree(procSource);
      RERAISE_EXC();
    }

  /* get information required for output conversion of the return
   * value, but only if this isn't a trigger.
   */
  if (!is_trigger)
    {
      HeapTuple rvTypeTup;
      Form_pg_type rvTypeStruct;
      Datum rvDatum;

      rvDatum = ObjectIdGetDatum(procStruct->prorettype);
      rvTypeTup = SearchSysCache(TYPEOID, rvDatum, 0, 0, 0);
      if (!HeapTupleIsValid(rvTypeTup))
	elog(ERROR, "plpython: cache lookup for type \"%u\" failed",
	     procStruct->prorettype);
      
      rvTypeStruct = (Form_pg_type) GETSTRUCT(rvTypeTup);
      if (rvTypeStruct->typrelid == InvalidOid)
	PLy_output_datum_func(&proc->result, rvTypeStruct);
      else
	elog(ERROR, "plpython: tuple return types not supported, yet");

      ReleaseSysCache(rvTypeTup);
    }
  else
    {
      /* input/output conversion for trigger tuples.  use the
       * result TypeInfo variable to store the tuple conversion
       * info.
       */
      TriggerData *tdata = (TriggerData *) fcinfo->context;
      PLy_input_tuple_funcs(&(proc->result), tdata->tg_relation->rd_att);
      PLy_output_tuple_funcs(&(proc->result), tdata->tg_relation->rd_att);
    }

  /* now get information required for input conversion of the
   * procedures arguments.
   */
  proc->nargs = fcinfo->nargs;
  for (i = 0; i < fcinfo->nargs; i++)
    {
      HeapTuple argTypeTup;
      Form_pg_type argTypeStruct;
      Datum argDatum;
      
      argDatum = ObjectIdGetDatum(procStruct->proargtypes[i]);
      argTypeTup = SearchSysCache(TYPEOID, argDatum, 0, 0, 0);
      if (!HeapTupleIsValid(argTypeTup))
	elog(ERROR, "plpython: cache lookup for type \"%u\" failed",
	     procStruct->proargtypes[i]);
      argTypeStruct = (Form_pg_type) GETSTRUCT(argTypeTup);
      
      if (argTypeStruct->typrelid == InvalidOid)
	PLy_input_datum_func(&(proc->args[i]), argTypeStruct);
      else
	{
	  TupleTableSlot *slot = (TupleTableSlot *) fcinfo->arg[i];
	  PLy_input_tuple_funcs(&(proc->args[i]),
				slot->ttc_tupleDescriptor);
	}
      
      ReleaseSysCache(argTypeTup);
    }


  /* get the text of the function.
   */
  procDatum = DirectFunctionCall1(textout,
				  PointerGetDatum(&procStruct->prosrc));
  procSource = DatumGetCString(procDatum);

  ReleaseSysCache(procTup);

  PLy_procedure_compile(proc, procSource);

  pfree(procSource);

  proc->me = PyCObject_FromVoidPtr(proc, NULL);
  PyDict_SetItemString(PLy_procedure_cache, key, proc->me);

  RESTORE_EXC();

  return proc;
}

void
PLy_procedure_compile(PLyProcedure *proc, const char *src)
{
  PyObject *module, *crv = NULL;
  char *msrc;

  enter();

  /* get an instance of rexec.RExec for the function
   */
  proc->interp = PyObject_CallMethod(PLy_interp_safe, "RExec", NULL);
  if ((proc->interp == NULL) || (PyErr_Occurred ()))
    PLy_elog(ERROR, "Unable to create rexec.RExec instance");

  /* tweak the list of permitted modules
   */
  PyObject_SetAttrString(proc->interp, "ok_builtin_modules",
			 PLy_importable_modules);

  proc->reval = PyObject_GetAttrString(proc->interp, "r_eval");
  if ((proc->reval == NULL) || (PyErr_Occurred ()))
    PLy_elog(ERROR, "Unable to get method `r_eval' from rexec.RExec");

  /* add a __main__ module to the function's interpreter
   */
  module = PyObject_CallMethod (proc->interp, "add_module", "s", "__main__");
  if ((module == NULL) || (PyErr_Occurred ()))
    PLy_elog(ERROR, "Unable to get module `__main__' from rexec.RExec");

  /* add plpy module to the interpreters main dictionary
   */
  proc->globals = PyModule_GetDict (module);
  if ((proc->globals == NULL) || (PyErr_Occurred ()))
    PLy_elog(ERROR, "Unable to get `__main__.__dict__' from rexec.RExec");

  /* why the hell won't r_import or r_exec('import plpy') work?
   */
  module = PyDict_GetItemString(PLy_interp_globals, "plpy");
  if ((module == NULL) || (PyErr_Occurred()))
    PLy_elog(ERROR, "Unable to get `plpy'");
  Py_INCREF(module);
  PyDict_SetItemString(proc->globals, "plpy", module);

  /* SD is private preserved data between calls
   * GD is global data shared by all functions
   */
  proc->statics = PyDict_New();
  PyDict_SetItemString(proc->globals, "SD", proc->statics);
  PyDict_SetItemString(proc->globals, "GD", PLy_interp_safe_globals);

  /* insert the function code into the interpreter
   */
  msrc = PLy_procedure_munge_source(proc->proname, src);
  crv = PyObject_CallMethod(proc->interp, "r_exec", "s", msrc);
  free(msrc);

  if ((crv != NULL) && (!PyErr_Occurred ()))
    {
      int clen;
      char call[256];

      Py_DECREF(crv);

      /* compile a call to the function
       */
      clen = snprintf(call, sizeof(call), "%s()", proc->proname);
      if ((clen < 0) || (clen >= sizeof(call)))
	elog(ERROR, "plpython: string would overflow buffer.");
      proc->code = Py_CompileString(call, "<string>", Py_eval_input);
      if ((proc->code != NULL) && (!PyErr_Occurred ()))
	return;
    }
  else
    Py_XDECREF(crv);

  PLy_elog(ERROR, "Unable to compile function %s", proc->proname);
}

char *
PLy_procedure_munge_source(const char *name, const char *src)
{
  char *mrc, *mp;
  const char *sp;
  size_t mlen, plen;

  enter();

  /* room for function source and the def statement
   */
  mlen = (strlen (src) * 2) + strlen(name) + 16;

  mrc = PLy_malloc(mlen);
  plen = snprintf(mrc, mlen, "def %s():\n\t", name);
  if ((plen < 0) || (plen >= mlen))
    elog(FATAL, "Aiieee, impossible buffer overrun (or snprintf failure)");

  sp = src;
  mp = mrc + plen;

  while (*sp != '\0')
    {
      if (*sp == '\n')
	{
	  *mp++ = *sp++;
	  *mp++ = '\t';
	}
      else
	*mp++ = *sp++;
    }
  *mp++ = '\n';
  *mp++ = '\n';
  *mp = '\0';

  if (mp > (mrc + mlen))
    elog(FATAL, "plpython: Buffer overrun in PLy_munge_source");

  return mrc;
}

PLyProcedure *
PLy_procedure_new(const char *name)
{
  int i;
  PLyProcedure *proc;

  enter();
  
  proc = PLy_malloc(sizeof(PLyProcedure));
  proc->proname = PLy_malloc(strlen(name) + 1);
  strcpy(proc->proname, name);
  PLy_typeinfo_init(&proc->result);
  for (i = 0; i < FUNC_MAX_ARGS; i++)
    PLy_typeinfo_init(&proc->args[i]);
  proc->nargs = 0;
  proc->code = proc->interp = proc->reval = proc->statics = NULL;
  proc->globals = proc->me = NULL;

  leave();

  return proc;
}

void
PLy_procedure_delete(PLyProcedure *proc)
{
  int i;

  enter();

  Py_XDECREF(proc->code);
  Py_XDECREF(proc->interp);
  Py_XDECREF(proc->reval);
  Py_XDECREF(proc->statics);
  Py_XDECREF(proc->globals);
  Py_XDECREF(proc->me);
  if (proc->proname)
    PLy_free(proc->proname);
  for (i = 0; i < proc->nargs; i++)
    if (proc->args[i].is_rel == 1)
      {
	if (proc->args[i].in.r.atts)
	  PLy_free(proc->args[i].in.r.atts);
	if (proc->args[i].out.r.atts)
	  PLy_free(proc->args[i].out.r.atts);
      }

  leave();
}

/* conversion functions.  remember output from python is
 * input to postgresql, and vis versa.
 */
void
PLy_input_tuple_funcs(PLyTypeInfo *arg, TupleDesc desc)
{
  int i;
  Datum datum;

  enter ();

  if (arg->is_rel == 0)
    elog(FATAL, "plpython: PLyTypeInfo struct is initialized for a Datum");

  arg->is_rel = 1;
  arg->in.r.natts = desc->natts;
  arg->in.r.atts = malloc(desc->natts * sizeof(PLyDatumToOb));

  for (i = 0; i < desc->natts; i++)
    {
      HeapTuple typeTup;
      Form_pg_type typeStruct;

      datum = ObjectIdGetDatum(desc->attrs[i]->atttypid);
      typeTup = SearchSysCache(TYPEOID, datum, 0, 0, 0);
      if (!HeapTupleIsValid(typeTup))
	{
	  char *attname = NameStr(desc->attrs[i]->attname);
	  elog(ERROR, "plpython: Cache lookup for attribute `%s' type `%u' failed",
	       attname, desc->attrs[i]->atttypid);
	}

      typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

      PLy_input_datum_func2(&(arg->in.r.atts[i]), typeStruct);

      ReleaseSysCache(typeTup);
    }
}

void
PLy_output_tuple_funcs(PLyTypeInfo *arg, TupleDesc desc)
{
  int i;
  Datum datum;

  enter ();

  if (arg->is_rel == 0)
    elog(FATAL, "plpython: PLyTypeInfo struct is initialized for a Datum");

  arg->is_rel = 1;
  arg->out.r.natts = desc->natts;
  arg->out.r.atts = malloc(desc->natts * sizeof(PLyDatumToOb));

  for (i = 0; i < desc->natts; i++)
    {
      HeapTuple typeTup;
      Form_pg_type typeStruct;

      datum = ObjectIdGetDatum(desc->attrs[i]->atttypid);
      typeTup = SearchSysCache(TYPEOID, datum, 0, 0, 0);
      if (!HeapTupleIsValid(typeTup))
	{
	  char *attname = NameStr(desc->attrs[i]->attname);
	  elog(ERROR, "plpython: Cache lookup for attribute `%s' type `%u' failed",
	       attname, desc->attrs[i]->atttypid);
	}

      typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

      PLy_output_datum_func2(&(arg->out.r.atts[i]), typeStruct);

      ReleaseSysCache(typeTup);
    }
}

void
PLy_output_datum_func(PLyTypeInfo *arg, Form_pg_type typeStruct)
{
  enter();

  if (arg->is_rel == 1)
    elog(FATAL, "plpython: PLyTypeInfo struct is initialized for a Tuple");
  arg->is_rel = 0;
  PLy_output_datum_func2(&(arg->out.d), typeStruct);
}

void
PLy_output_datum_func2(PLyObToDatum *arg, Form_pg_type typeStruct)
{
  enter();

  fmgr_info(typeStruct->typinput, &arg->typfunc);
  arg->typelem = (Oid) typeStruct->typelem;
  arg->typlen = typeStruct->typlen;
}

void
PLy_input_datum_func(PLyTypeInfo *arg, Form_pg_type typeStruct)
{
  enter();

  if (arg->is_rel == 1)
    elog(FATAL, "plpython: PLyTypeInfo struct is initialized for Tuple");
  arg->is_rel = 0;
  PLy_input_datum_func2(&(arg->in.d), typeStruct);
}

void
PLy_input_datum_func2(PLyDatumToOb *arg, Form_pg_type typeStruct)
{
  char *type;

  arg->typoutput = typeStruct->typoutput;
  fmgr_info(typeStruct->typoutput, &arg->typfunc);
  arg->typlen = typeStruct->typlen;
  arg->typelem = typeStruct->typelem;

  /* hmmm, wierd.  means this arg will always be converted
   * to a python None
   */
  if (!OidIsValid(typeStruct->typoutput))
    {
      elog(ERROR, "plpython: (FIXME) typeStruct->typoutput is invalid");
      
      arg->func = NULL;
      return;
    }

  type = NameStr(typeStruct->typname);
  switch (type[0])
    {
    case 'b':
      {
	if (strcasecmp("bool", type))
	  {
	    arg->func = PLyBool_FromString;
	    return;
	  }
	break;
      }
    case 'f':
      {
	if ((strncasecmp("float", type, 5) == 0) && 
	    ((type[5] == '8') || (type[5] == '4')))
	  {
	    arg->func = PLyFloat_FromString;
	    return;
	  }
	break;
      }
    case 'i':
      {
	if ((strncasecmp("int", type, 3) == 0) && 
	    ((type[3] == '4') || (type[3] == '2') || (type[3] == '8')) &&
	    (type[4] == '\0'))
	  {
	    arg->func = PLyInt_FromString;
	    return;
	  }
	break;
      }
    case 'n':
      {
	if (strcasecmp("numeric", type) == 0)
	  {
	    arg->func = PLyFloat_FromString;
	    return;
	  }
	break;
      }
    default:
      break;
    }
  arg->func = PLyString_FromString;
}

void
PLy_typeinfo_init(PLyTypeInfo *arg)
{
  arg->is_rel = -1;
  arg->in.r.natts = arg->out.r.natts = 0;
  arg->in.r.atts = NULL;
  arg->out.r.atts = NULL;
}

void
PLy_typeinfo_dealloc(PLyTypeInfo *arg)
{
  if (arg->is_rel == 1)
    {
      if (arg->in.r.atts)
	PLy_free(arg->in.r.atts);
      if (arg->out.r.atts)
	PLy_free(arg->out.r.atts);
    }
}

/* assumes that a bool is always returned as a 't' or 'f'
 */
PyObject *
PLyBool_FromString(const char *src)
{
  enter();

  if (src[0] == 't')
    return PyInt_FromLong(1);
  return PyInt_FromLong(0);
}

PyObject *
PLyFloat_FromString(const char *src)
{
  double v;
  char *eptr;

  enter();
  
  errno = 0;
  v = strtod(src, &eptr);
  if ((*eptr != '\0') || (errno))
    return NULL;
  return PyFloat_FromDouble(v);
}

PyObject *
PLyInt_FromString(const char *src)
{
  long v;
  char *eptr;

  enter();

  errno = 0;
  v = strtol(src, &eptr, 0);
  if ((*eptr != '\0') || (errno))
    return NULL;
  return PyInt_FromLong(v);
}

PyObject *
PLyString_FromString(const char *src)
{
  return PyString_FromString(src);
}

PyObject *
PLyDict_FromTuple(PLyTypeInfo *info, HeapTuple tuple, TupleDesc desc)
{
  DECLARE_EXC();
  PyObject *volatile dict;
  int i;

  enter();

  if (info->is_rel != 1)
    elog(FATAL, "plpython: PLyTypeInfo structure describes a datum.");

  dict = PyDict_New();
  if (dict == NULL)
    PLy_elog(ERROR, "Unable to create tuple dictionary.");

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();
      Py_DECREF(dict);

      RERAISE_EXC();
    }
  
  for (i = 0; i < info->in.r.natts; i++)
    {
      char *key, *vsrc;
      Datum vattr, vdat;
      bool is_null;
      PyObject *value;

      key = NameStr(desc->attrs[i]->attname);
      vattr = heap_getattr(tuple, (i + 1), desc, &is_null);

      if ((is_null) || (info->in.r.atts[i].func == NULL))
	PyDict_SetItemString(dict, key, Py_None);
      else
	{
	  vdat = OidFunctionCall3(info->in.r.atts[i].typoutput, vattr,
				  ObjectIdGetDatum(info->in.r.atts[i].typelem),
				  Int32GetDatum(info->in.r.atts[i].typlen));
	  vsrc = DatumGetCString(vdat);

	  /* no exceptions allowed
	   */
	  value = info->in.r.atts[i].func (vsrc);
	  pfree(vsrc);
	  PyDict_SetItemString(dict, key, value);
	  Py_DECREF(value);
	}
    }
  
  RESTORE_EXC();

  return dict;
}

/* initialization, some python variables function declared here
 */

/* interface to postgresql elog
 */
static PyObject *PLy_debug(PyObject *, PyObject *);
static PyObject *PLy_error(PyObject *, PyObject *);
static PyObject *PLy_fatal(PyObject *, PyObject *);
static PyObject *PLy_notice(PyObject *, PyObject *);

/* PLyPlanObject, PLyResultObject and SPI interface
 */
#define is_PLyPlanObject(x) ((x)->ob_type == &PLy_PlanType)
static PyObject *PLy_plan_new(void);
static void PLy_plan_dealloc(PyObject *);
static PyObject *PLy_plan_getattr(PyObject *, char *);
static PyObject *PLy_plan_status(PyObject *, PyObject *);

static PyObject *PLy_result_new(void);
static void PLy_result_dealloc(PyObject *);
static PyObject *PLy_result_getattr(PyObject *, char *);
static PyObject *PLy_result_fetch(PyObject *, PyObject *);
static PyObject *PLy_result_nrows(PyObject *, PyObject *);
static PyObject *PLy_result_status(PyObject *, PyObject *);
static int PLy_result_length(PyObject *);
static PyObject *PLy_result_item(PyObject *, int);
static PyObject *PLy_result_slice(PyObject *, int, int);
static int PLy_result_ass_item(PyObject *, int, PyObject *);
static int PLy_result_ass_slice(PyObject *, int, int, PyObject *);


static PyObject *PLy_spi_prepare(PyObject *, PyObject *);
static PyObject *PLy_spi_execute(PyObject *, PyObject *);
static const char *PLy_spi_error_string(int);
static PyObject *PLy_spi_execute_query(char *query, int limit);
static PyObject *PLy_spi_execute_plan(PyObject *, PyObject *, int);
static PyObject *PLy_spi_execute_fetch_result(SPITupleTable *, int, int);


PyTypeObject PLy_PlanType = {
  PyObject_HEAD_INIT(&PyType_Type)
  0,                               /*ob_size*/
  "PLyPlan",                       /*tp_name*/
  sizeof(PLyPlanObject),           /*tp_size*/
  0,                               /*tp_itemsize*/
  /* methods 
   */
  (destructor) PLy_plan_dealloc,   /*tp_dealloc*/
  0,                               /*tp_print*/
  (getattrfunc)PLy_plan_getattr,   /*tp_getattr*/
  0,                               /*tp_setattr*/
  0,                               /*tp_compare*/
  0,                               /*tp_repr*/
  0,                               /*tp_as_number*/
  0,                               /*tp_as_sequence*/
  0,                               /*tp_as_mapping*/
  0,                               /*tp_hash*/
  0,                               /*tp_call*/
  0,                               /*tp_str*/
  0,                               /*tp_getattro*/
  0,                               /*tp_setattro*/
  0,                               /*tp_as_buffer*/
  0,                               /*tp_xxx4*/
  PLy_plan_doc,                    /*tp_doc*/
};

PyMethodDef PLy_plan_methods[] = {
  { "status",  (PyCFunction) PLy_plan_status,  METH_VARARGS, NULL },
  { NULL, NULL, 0, NULL }
};


PySequenceMethods PLy_result_as_sequence = {
  (inquiry) PLy_result_length,                /* sq_length */
  (binaryfunc) 0,                             /* sq_concat */
  (intargfunc) 0,                             /* sq_repeat */
  (intargfunc) PLy_result_item,               /* sq_item */
  (intintargfunc) PLy_result_slice,           /* sq_slice */
  (intobjargproc) PLy_result_ass_item,        /* sq_ass_item */
  (intintobjargproc) PLy_result_ass_slice,    /* sq_ass_slice */
};

PyTypeObject PLy_ResultType = {
  PyObject_HEAD_INIT(&PyType_Type)
  0,                                 /*ob_size*/
  "PLyResult",                       /*tp_name*/
  sizeof(PLyResultObject),           /*tp_size*/
  0,                                 /*tp_itemsize*/
  /* methods 
   */
  (destructor) PLy_result_dealloc,   /*tp_dealloc*/
  0,                                 /*tp_print*/
  (getattrfunc) PLy_result_getattr,  /*tp_getattr*/
  0,                                 /*tp_setattr*/
  0,                                 /*tp_compare*/
  0,                                 /*tp_repr*/
  0,                                 /*tp_as_number*/
  &PLy_result_as_sequence,           /*tp_as_sequence*/
  0,                                 /*tp_as_mapping*/
  0,                                 /*tp_hash*/
  0,                                 /*tp_call*/
  0,                                 /*tp_str*/
  0,                                 /*tp_getattro*/
  0,                                 /*tp_setattro*/
  0,                                 /*tp_as_buffer*/
  0,                                 /*tp_xxx4*/
  PLy_result_doc,                    /*tp_doc*/
};

PyMethodDef PLy_result_methods[] = {
  { "fetch",  (PyCFunction) PLy_result_fetch,  METH_VARARGS, NULL,},
  { "nrows",  (PyCFunction) PLy_result_nrows,  METH_VARARGS, NULL },
  { "status", (PyCFunction) PLy_result_status, METH_VARARGS, NULL },
  { NULL, NULL, 0, NULL }
};


static PyMethodDef PLy_methods[] = {
  /* logging methods
   */
  { "debug", PLy_debug, METH_VARARGS, NULL },
  { "error", PLy_error, METH_VARARGS, NULL },
  { "fatal", PLy_fatal, METH_VARARGS, NULL },
  { "notice", PLy_notice, METH_VARARGS, NULL },

  /* create a stored plan
   */
  { "prepare", PLy_spi_prepare, METH_VARARGS, NULL },
  
  /* execute a plan or query
   */
  { "execute", PLy_spi_execute, METH_VARARGS, NULL },

  { NULL, NULL, 0, NULL }
};


/* plan object methods
 */
PyObject *
PLy_plan_new(void)
{
  PLyPlanObject *ob;
  
  enter();

  if ((ob = PyObject_NEW(PLyPlanObject, &PLy_PlanType)) == NULL)
    return NULL;

  ob->plan = NULL;
  ob->nargs = 0;
  ob->types = NULL;
  ob->args = NULL;

  return (PyObject *) ob;
}


void
PLy_plan_dealloc(PyObject *arg)
{
  PLyPlanObject *ob = (PLyPlanObject *) arg;

  enter();

  if (ob->plan)
    {
      /* free the plan...
       * pfree(ob->plan); 
       *
       * FIXME -- leaks saved plan on object destruction.  can
       *          this be avoided?
       */
    }
  if (ob->types)
    PLy_free(ob->types);
  if (ob->args)
    {
      int i;

      for (i = 0; i < ob->nargs; i++)
	PLy_typeinfo_dealloc(&ob->args[i]);
      PLy_free(ob->args);
    }

  PyMem_DEL(arg);

  leave();
}


PyObject *
PLy_plan_getattr(PyObject *self, char *name)
{
  return Py_FindMethod(PLy_plan_methods, self, name);
}

PyObject *
PLy_plan_status(PyObject *self, PyObject *args)
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



/* result object methods
 */

static PyObject *
PLy_result_new(void)
{
  PLyResultObject *ob;
  
  enter();

  if ((ob = PyObject_NEW(PLyResultObject, &PLy_ResultType)) == NULL)
    return NULL;

  /*   ob->tuples = NULL; */

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

  enter();

  Py_XDECREF(ob->nrows);
  Py_XDECREF(ob->rows);
  Py_XDECREF(ob->status);

  PyMem_DEL(ob);
}

static PyObject *
PLy_result_getattr(PyObject *self, char *attr)
{
  return NULL;
}

static PyObject *
PLy_result_fetch(PyObject *self, PyObject *args)
{
  return NULL;
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

int
PLy_result_length(PyObject *arg)
{
  PLyResultObject *ob = (PLyResultObject *) arg;
  return PyList_Size(ob->rows);
}

PyObject *
PLy_result_item(PyObject *arg, int idx)
{
  PyObject *rv;
  PLyResultObject *ob = (PLyResultObject *) arg;

  rv = PyList_GetItem(ob->rows, idx);
  if (rv != NULL)
    Py_INCREF(rv);
  return rv;
}

int
PLy_result_ass_item(PyObject *arg, int idx, PyObject *item)
{
  int rv;
  PLyResultObject *ob = (PLyResultObject *) arg;

  Py_INCREF(item);
  rv = PyList_SetItem(ob->rows, idx, item);
  return rv;
}

PyObject *
PLy_result_slice(PyObject *arg, int lidx, int hidx)
{
  PyObject *rv;
  PLyResultObject *ob = (PLyResultObject *) arg;
  
  rv = PyList_GetSlice(ob->rows, lidx, hidx);
  if (rv == NULL)
    return NULL;
  Py_INCREF(rv);
  return rv;
}

int
PLy_result_ass_slice(PyObject *arg, int lidx, int hidx, PyObject *slice)
{
  int rv;
  PLyResultObject *ob = (PLyResultObject *) arg;
  
  rv = PyList_SetSlice(ob->rows, lidx, hidx, slice);
  return rv;
}

/* SPI interface
 */
PyObject *
PLy_spi_prepare(PyObject *self, PyObject *args)
{
  DECLARE_EXC();
  PLyPlanObject *plan;
  PyObject *list = NULL;
  PyObject *optr = NULL;
  char *query;

  enter();

  if (!PyArg_ParseTuple(args, "s|O", &query, &list))
    {
      PyErr_SetString(PLy_exc_spi_error,
		      "Invalid arguments for plpy.prepare()");
      return NULL;
    }

  if ((list) && (!PySequence_Check(list)))
    {
      PyErr_SetString(PLy_exc_spi_error,
		      "Second argument in plpy.prepare() must be a sequence");
      return NULL;
    }


  if ((plan = (PLyPlanObject *) PLy_plan_new()) == NULL)
    return NULL;

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();
      Py_DECREF(plan);
      Py_XDECREF(optr);
      if (!PyErr_Occurred ())
	PyErr_SetString(PLy_exc_spi_error,
			"Unknown error in PLy_spi_prepare.");
      return NULL;
    }

  if (list != NULL)
    {
      int nargs, i;

     
      nargs = PySequence_Length(list);
      if (nargs > 0)
	{
	  plan->nargs = nargs;
	  plan->types = PLy_malloc(sizeof(Oid) * nargs);
	  plan->values = PLy_malloc(sizeof(Datum) * nargs);
	  plan->args = PLy_malloc(sizeof(PLyTypeInfo) * nargs);

	  /* the other loop might throw an exception, if PLyTypeInfo
	   * member isn't properly initialized the Py_DECREF(plan)
	   * will go boom
	   */
	  for (i = 0; i < nargs; i++)
	    {
	      PLy_typeinfo_init(&plan->args[i]);
	      plan->values[i] = (Datum) NULL;
	    }

	  for (i = 0; i < nargs; i++)
	    {
	      char *sptr;
	      HeapTuple typeTup;
	      Form_pg_type typeStruct;

	      optr = PySequence_GetItem(list, i);
	      if (!PyString_Check(optr))
		{
		  PyErr_SetString(PLy_exc_spi_error,
				  "Type names must be strings.");
		  RAISE_EXC(1);
		}
	      sptr = PyString_AsString(optr);
	      typeTup = SearchSysCache(TYPENAME, PointerGetDatum(sptr),
				       0, 0, 0);
	      if (!HeapTupleIsValid(typeTup))
		{
		  PLy_exception_set(PLy_exc_spi_error,
				    "Cache lookup for type `%s' failed.",
				    sptr);
		  RAISE_EXC(1);
		}

	      Py_DECREF(optr);
	      optr = NULL;     /* this is important */

	      plan->types[i] = typeTup->t_data->t_oid;
	      typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	      if (typeStruct->typrelid == InvalidOid)
		PLy_output_datum_func(&plan->args[i], typeStruct);
	      else
		{
		  PyErr_SetString(PLy_exc_spi_error, 
				  "tuples not handled in plpy.prepare, yet.");
		  RAISE_EXC(1);
		}
	      ReleaseSysCache(typeTup);
	    }
	}
    }

  plan->plan = SPI_prepare(query, plan->nargs, plan->types);
  if (plan->plan == NULL)
    {
      PLy_exception_set(PLy_exc_spi_error,
			"Unable to prepare plan. SPI_prepare failed -- %s.",
			PLy_spi_error_string(SPI_result));
      RAISE_EXC(1);
    }

  plan->plan = SPI_saveplan(plan->plan);
  if (plan->plan == NULL)
    {
      PLy_exception_set(PLy_exc_spi_error,
			"Unable to save plan. SPI_saveplan failed -- %s.",
			PLy_spi_error_string(SPI_result));
      RAISE_EXC(1);
    }

  RESTORE_EXC();

  return (PyObject *) plan;
}

/* execute(query="select * from foo", limit=5)
 * execute(plan=plan, values=(foo, bar), limit=5)
 */
PyObject *
PLy_spi_execute(PyObject *self, PyObject *args)
{
  char *query;
  PyObject *plan;
  PyObject *list = NULL;
  int limit = 0;

  enter();

#if 0
  /* there should - hahaha - be an python exception set so just
   * return NULL.  FIXME -- is this needed?
   */
  if (PLy_restart_in_progress)
    return NULL;
#endif

  if (PyArg_ParseTuple(args, "s|i", &query, &limit))
    return PLy_spi_execute_query(query, limit);

  PyErr_Clear();

  if ((PyArg_ParseTuple(args, "O|Oi", &plan, &list, &limit)) &&
      (is_PLyPlanObject(plan)))
    {
      PyObject *rv = PLy_spi_execute_plan(plan, list, limit);
      return rv;
    }

  PyErr_SetString(PLy_exc_error, "Expected a query or plan.");
  return NULL;
}

PyObject *
PLy_spi_execute_plan(PyObject *ob, PyObject *list, int limit)
{
  DECLARE_EXC();
  int nargs, i, rv;
  PLyPlanObject *plan;

  enter();

  if (list != NULL)
    {
      if ((!PySequence_Check(list)) || (PyString_Check(list)))
	{
	  char *msg = "plpy.execute() takes a sequence as its second argument";
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
      char *sv;

      PyObject *so = PyObject_Str(list);
      sv = PyString_AsString(so);
      PLy_exception_set(PLy_exc_spi_error,
			"Expected sequence of %d arguments, got %d. %s",
			plan->nargs, nargs, sv);
      Py_DECREF(so);

      return NULL;
    }

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();

      /* cleanup plan->values array
       */
      for (i = 0; i < nargs; i++)
	{
	  /* FIXME -- typbyval the proper check?
	   */
	  if ((plan->values[i] != (Datum) NULL) &&
	      (plan->args[i].out.d.typlen < 0))
	    {
	      pfree((void *) plan->values[i]);
	      plan->values[i] = (Datum) NULL;
	    }
	}

      if (!PyErr_Occurred())
	PyErr_SetString(PLy_exc_error,
			"Unknown error in PLy_spi_execute_plan");
      return NULL;
    }

  if (nargs)
    {
      for (i = 0; i < nargs; i++)
	{
	  Datum typelem, typlen, dv;
	  PyObject *elem, *so;
	  char *sv;

	  typelem = ObjectIdGetDatum(plan->args[i].out.d.typelem);
	  typlen = Int32GetDatum(plan->args[i].out.d.typlen);
	  elem = PySequence_GetItem(list, i);
	  so = PyObject_Str(elem);
	  sv = PyString_AsString(so);
	  dv = CStringGetDatum(sv);

	  /* FIXME -- if this can elog, we have leak
	   */
	  plan->values[i] = FunctionCall3(&(plan->args[i].out.d.typfunc),
					  dv, typelem, typlen);

	  Py_DECREF(so);
	  Py_DECREF(elem);
	}
    }

  rv = SPI_execp(plan->plan, plan->values, NULL, limit);
  RESTORE_EXC();

  for (i = 0; i < nargs; i++)
    {
      /* FIXME -- typbyval the proper check?
       */
      if ((plan->values[i] != (Datum) NULL) &&
	  (plan->args[i].out.d.typlen < 0))
	{
	  pfree((void *) plan->values[i]);
	  plan->values[i] = (Datum) NULL;
	}
    }

  if (rv < 0)
    {
      PLy_exception_set(PLy_exc_spi_error,
			"Unable to execute plan.  SPI_execp failed -- %s",
			PLy_spi_error_string(rv));
      return NULL;
    }

  return PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);  
}

PyObject *
PLy_spi_execute_query(char *query, int limit)
{
  DECLARE_EXC();
  int rv;

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();

      if ((!PLy_restart_in_progress) && (!PyErr_Occurred()))
	PyErr_SetString(PLy_exc_spi_error,
			"Unknown error in PLy_spi_execute_query.");
      return NULL;
    }
  
  rv = SPI_exec(query, limit);
  RESTORE_EXC();

  if (rv < 0)
    {
      PLy_exception_set(PLy_exc_spi_error,
			"Unable to execute query.  SPI_exec failed -- %s",
			PLy_spi_error_string(rv));
      return NULL;
    }

  return PLy_spi_execute_fetch_result(SPI_tuptable, SPI_processed, rv);
}

PyObject *
PLy_spi_execute_fetch_result(SPITupleTable *tuptable, int rows, int status)
{
  PLyResultObject *result;

  enter();

  result = (PLyResultObject *) PLy_result_new();
  Py_DECREF(result->status);
  result->status = PyInt_FromLong(status);

  if (status == SPI_OK_UTILITY)
    {
      Py_DECREF(result->nrows);
      result->nrows = PyInt_FromLong(0);
    }
  else if (status != SPI_OK_SELECT)
    {
      Py_DECREF(result->nrows);
      result->nrows = PyInt_FromLong(rows);
    }
  else
    {
      DECLARE_EXC();
      PLyTypeInfo args;
      int i;

      PLy_typeinfo_init(&args);
      Py_DECREF(result->nrows);
      result->nrows = PyInt_FromLong(rows);

      SAVE_EXC();
      if (TRAP_EXC())
	{
	  RESTORE_EXC();

	  if (!PyErr_Occurred())
	    PyErr_SetString(PLy_exc_error,
			    "Unknown error in PLy_spi_execute_fetch_result");
	  Py_DECREF(result);
	  PLy_typeinfo_dealloc(&args);
	  return NULL;
	}
      
      if (rows)
	{
	  Py_DECREF(result->rows);
	  result->rows = PyList_New(rows);

	  PLy_input_tuple_funcs(&args, tuptable->tupdesc);
	  for (i = 0; i < rows; i++)
	    {
	      PyObject *row = PLyDict_FromTuple(&args, tuptable->vals[i], 
						tuptable->tupdesc);
	      PyList_SetItem(result->rows, i, row);
	    }
	  PLy_typeinfo_dealloc(&args);
	}
      RESTORE_EXC();
    }

  return (PyObject *) result;
}

const char *
PLy_spi_error_string(int code)
{
  switch (code)
    {
    case SPI_ERROR_TYPUNKNOWN:
      return "SPI_ERROR_TYPUNKNOWN";
    case SPI_ERROR_NOOUTFUNC:
      return "SPI_ERROR_NOOUTFUNC";
    case SPI_ERROR_NOATTRIBUTE:
      return "SPI_ERROR_NOATTRIBUTE";
    case SPI_ERROR_TRANSACTION:
      return "SPI_ERROR_TRANSACTION";
    case SPI_ERROR_PARAM:
      return "SPI_ERROR_PARAM";
    case SPI_ERROR_ARGUMENT:
      return "SPI_ERROR_ARGUMENT";
    case SPI_ERROR_CURSOR:
      return "SPI_ERROR_CURSOR";
    case SPI_ERROR_UNCONNECTED:
      return "SPI_ERROR_UNCONNECTED";
    case SPI_ERROR_OPUNKNOWN:
      return "SPI_ERROR_OPUNKNOWN";
    case SPI_ERROR_COPY:
      return "SPI_ERROR_COPY";
    case SPI_ERROR_CONNECT:
      return "SPI_ERROR_CONNECT";
    }
  return "Unknown or Invalid code";
}

/* language handler and interpreter initialization
 */

void PLy_init_all(void)
{
  static volatile int init_active = 0;

  enter();

  if (init_active)
    elog(FATAL, "plpython: Initialization of language module failed.");
  init_active = 1;

  Py_Initialize();
  PLy_init_interp();
  PLy_init_plpy();
  PLy_init_safe_interp();
  if (PyErr_Occurred())
    PLy_elog(FATAL, "Untrapped error in initialization.");
  PLy_procedure_cache = PyDict_New();
  if (PLy_procedure_cache == NULL)
    PLy_elog(ERROR, "Unable to create procedure cache.");

  PLy_first_call = 0;

  leave();
}

void
PLy_init_interp(void)
{
  PyObject *mainmod;

  enter();

  mainmod = PyImport_AddModule("__main__");
  if ((mainmod == NULL) || (PyErr_Occurred()))
    PLy_elog(ERROR, "Unable to import '__main__' module.");
  Py_INCREF(mainmod);
  PLy_interp_globals = PyModule_GetDict(mainmod);
  Py_DECREF(mainmod);
  if ((PLy_interp_globals == NULL) || (PyErr_Occurred()))
    PLy_elog(ERROR, "Unable to initialize globals.");
}

void
PLy_init_plpy(void)
{
  PyObject *main_mod, *main_dict, *plpy_mod;
  PyObject *plpy, *plpy_dict;

  enter();

  /* initialize plpy module
   */
  plpy = Py_InitModule("plpy", PLy_methods);
  plpy_dict = PyModule_GetDict(plpy);

  //PyDict_SetItemString(plpy, "PlanType", (PyObject *) &PLy_PlanType);

  PLy_exc_error = PyErr_NewException("plpy.Error", NULL, NULL);
  PLy_exc_fatal = PyErr_NewException("plpy.Fatal", NULL, NULL);
  PLy_exc_spi_error = PyErr_NewException("plpy.SPIError", NULL, NULL);
  PyDict_SetItemString(plpy_dict, "Error", PLy_exc_error);
  PyDict_SetItemString(plpy_dict, "Fatal", PLy_exc_fatal);
  PyDict_SetItemString(plpy_dict, "SPIError", PLy_exc_spi_error);

  /* initialize main module, and add plpy
   */
  main_mod = PyImport_AddModule("__main__");
  main_dict = PyModule_GetDict(main_mod);
  plpy_mod = PyImport_AddModule("plpy");
  PyDict_SetItemString(main_dict, "plpy", plpy_mod);
  if (PyErr_Occurred ())
    elog(ERROR, "Unable to init plpy.");
}

void
PLy_init_safe_interp(void)
{
  PyObject *rmod;
  char *rname = "rexec";
  int i, imax;

  enter();

  rmod = PyImport_ImportModuleEx(rname, PLy_interp_globals,
				 PLy_interp_globals, Py_None);
  if ((rmod == NULL) || (PyErr_Occurred ()))
    PLy_elog(ERROR, "Unable to import %s.", rname);
  PyDict_SetItemString(PLy_interp_globals, rname, rmod);
  PLy_interp_safe = rmod;

  imax = sizeof(PLy_importable_modules_list) / sizeof(char *);
  PLy_importable_modules = PyTuple_New(imax);
  for (i = 0; i < imax; i++)
    {
      PyObject *m = PyString_FromString(PLy_importable_modules_list[i]);
      PyTuple_SetItem(PLy_importable_modules, i, m);
    }

  PLy_interp_safe_globals = PyDict_New();
  if (PLy_interp_safe_globals == NULL)
    PLy_elog(ERROR, "Unable to create shared global dictionary.");

}


/* the python interface to the elog function
 * don't confuse these with PLy_elog
 */
static PyObject *PLy_log(int, PyObject *, PyObject *);

PyObject *
PLy_debug(PyObject *self, PyObject *args)
{
  return PLy_log(DEBUG, self, args);
}

PyObject *
PLy_error(PyObject *self, PyObject *args)
{
  return PLy_log(ERROR, self, args);
}

PyObject *
PLy_fatal(PyObject *self, PyObject *args)
{
  return PLy_log(FATAL, self, args);
}

PyObject *
PLy_notice(PyObject *self, PyObject *args)
{
  return PLy_log(NOTICE, self, args);
}


PyObject *
PLy_log(int level, PyObject *self, PyObject *args)
{
  DECLARE_EXC();
  PyObject *so;
  char *sv;

  enter();

  if (args == NULL)
    elog(NOTICE, "plpython, args is NULL in %s", __FUNCTION__);

  so = PyObject_Str(args);
  if ((so == NULL) || ((sv = PyString_AsString(so)) == NULL))
    {
      level = ERROR;
      sv = "Unable to parse error message in `plpy.elog'";
    }

  /* returning NULL here causes the python interpreter to bail.
   * when control passes back into plpython_*_handler, we
   * check for python exceptions and do the actual elog
   * call.  actually PLy_elog.
   */
  if (level == ERROR)
    {
      PyErr_SetString(PLy_exc_error, sv);
      return NULL;
    }
  else if (level >= FATAL)
    {
      PyErr_SetString(PLy_exc_fatal, sv);
      return NULL;
    }

  /* ok, this is a NOTICE, or DEBUG message
   *
   * but just in case DON'T long jump out of the interpreter!
   */
  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();

      Py_XDECREF(so);

      /* the real error message should already be written into
       * the postgresql log, no?  whatever, this shouldn't happen
       * so die hideously.
       */
      elog(FATAL, "plpython: Aiieee, elog threw an unknown exception!");
      return NULL;
    }

  elog(level, sv);

  RESTORE_EXC();
  
  Py_XDECREF(so);
  Py_INCREF(Py_None);

  /* return a legal object so the interpreter will continue on its
   * merry way
   */
  return Py_None;
}


/* output a python traceback/exception via the postgresql elog
 * function.  not pretty.
 */

static char *PLy_traceback(int *);
static char *PLy_vprintf(const char *fmt, va_list ap);
static char *PLy_printf(const char *fmt, ...);

void
PLy_exception_set(PyObject *exc, const char *fmt, ...)
{
  char buf[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  PyErr_SetString(exc, buf);
}

void
PLy_elog(int elevel, const char *fmt,...)
{
  DECLARE_EXC();
  va_list ap;
  char *xmsg, *emsg;
  int xlevel;

  enter();

  xmsg = PLy_traceback(&xlevel);

  va_start(ap, fmt);
  emsg = PLy_vprintf(fmt, ap);
  va_end(ap);

  SAVE_EXC();
  if (TRAP_EXC())
    {
      RESTORE_EXC();
      mark();
      /* elog called siglongjmp. cleanup, restore and reraise
       */
      PLy_restart_in_progress += 1;
      PLy_free(emsg);
      PLy_free(xmsg);
      RERAISE_EXC();
    }

  if (xmsg)
    {
      elog(elevel, "plpython: %s\n%s", emsg, xmsg);
      PLy_free(xmsg);
    }
  else
    elog(elevel, "plpython: %s", emsg);
  PLy_free(emsg);

  leave();

  RESTORE_EXC();
}

char *
PLy_traceback(int *xlevel)
{
  PyObject *e, *v, *tb;
  PyObject *eob, *vob = NULL;
  char *vstr, *estr, *xstr = NULL;

  enter();
  
  /* get the current exception
   */
  PyErr_Fetch(&e, &v, &tb);

  /* oops, no exception, return
   */
  if (e == NULL)
    {
      *xlevel = NOTICE;
      return NULL;
    }

  PyErr_NormalizeException(&e, &v, &tb);

  eob = PyObject_Str(e);
  if ((v) && ((vob = PyObject_Str(v)) != NULL))
    vstr = PyString_AsString(vob);
  else
    vstr = "Unknown";

  estr = PyString_AsString(eob);
  xstr = PLy_printf("%s: %s", estr, vstr);

  Py_DECREF(eob);
  Py_XDECREF(vob);

  /* intuit an appropriate error level for based on the exception type
   */
  if ((PLy_exc_error) && (PyErr_GivenExceptionMatches(e, PLy_exc_error)))
    *xlevel = ERROR;
  else if ((PLy_exc_fatal) && (PyErr_GivenExceptionMatches(e, PLy_exc_fatal)))
    *xlevel = FATAL;
  else
    *xlevel = ERROR;

  leave();

  return xstr;
}

char *
PLy_printf(const char *fmt, ...)
{
  va_list ap;
  char *emsg;

  va_start(ap, fmt);
  emsg = PLy_vprintf(fmt, ap);
  va_end(ap);
  return emsg;
}

char *
PLy_vprintf(const char *fmt, va_list ap)
{
  size_t blen;
  int bchar, tries = 2;
  char *buf;

  blen = strlen(fmt) * 2;
  if (blen < 256)
    blen = 256;
  buf = PLy_malloc(blen * sizeof(char));

  while (1)
    {
      bchar = vsnprintf(buf, blen, fmt, ap);
      if ((bchar > 0) && (bchar < blen))
	return buf;
      if (tries-- <= 0)
	break;
      if (blen > 0)
	blen = bchar + 1;
      else
	blen *= 2;
      buf = PLy_realloc(buf, blen);
    }
  PLy_free(buf);
  return NULL;
}

/* python module code
 */


/* some dumb utility functions
 */

void *
PLy_malloc(size_t bytes)
{
  void *ptr = malloc(bytes);
  if (ptr == NULL)
    elog(FATAL, "plpython: Memory exhausted.");
  return ptr;
}

void *
PLy_realloc(void *optr, size_t bytes)
{
  void *nptr = realloc(optr, bytes);
  if (nptr == NULL)
    elog(FATAL, "plpython: Memory exhausted.");
  return nptr;
}

/* define this away
 */
void
PLy_free(void *ptr)
{
  free(ptr);
}
