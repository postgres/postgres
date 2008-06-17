/**********************************************************************
 * pltcl.c		- PostgreSQL support for Tcl as
 *				  procedural language (PL)
 *
 *	  $PostgreSQL: pgsql/src/pl/tcl/pltcl.c,v 1.108.2.1 2008/06/17 00:52:56 tgl Exp $
 *
 **********************************************************************/

#include "postgres.h"

#include <tcl.h>

#include <unistd.h>
#include <fcntl.h>

/* Hack to deal with Tcl 8.4 const-ification without losing compatibility */
#ifndef CONST84
#define CONST84
#endif

#include "access/heapam.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#define HAVE_TCL_VERSION(maj,min) \
	((TCL_MAJOR_VERSION > maj) || \
	 (TCL_MAJOR_VERSION == maj && TCL_MINOR_VERSION >= min))

/* In Tcl >= 8.0, really not supposed to touch interp->result directly */
#if !HAVE_TCL_VERSION(8,0)
#define Tcl_GetStringResult(interp)  ((interp)->result)
#endif

#if defined(UNICODE_CONVERSION) && HAVE_TCL_VERSION(8,1)

#include "mb/pg_wchar.h"

static unsigned char *
utf_u2e(unsigned char *src)
{
	return pg_do_encoding_conversion(src, strlen(src), PG_UTF8, GetDatabaseEncoding());
}

static unsigned char *
utf_e2u(unsigned char *src)
{
	return pg_do_encoding_conversion(src, strlen(src), GetDatabaseEncoding(), PG_UTF8);
}

#define PLTCL_UTF
#define UTF_BEGIN	 do { \
					unsigned char *_pltcl_utf_src; \
					unsigned char *_pltcl_utf_dst
#define UTF_END		 if (_pltcl_utf_src!=_pltcl_utf_dst) \
					pfree(_pltcl_utf_dst); } while (0)
#define UTF_U2E(x)	 (_pltcl_utf_dst=utf_u2e(_pltcl_utf_src=(x)))
#define UTF_E2U(x)	 (_pltcl_utf_dst=utf_e2u(_pltcl_utf_src=(x)))
#else							/* !PLTCL_UTF */

#define  UTF_BEGIN
#define  UTF_END
#define  UTF_U2E(x)  (x)
#define  UTF_E2U(x)  (x)
#endif   /* PLTCL_UTF */

PG_MODULE_MAGIC;

/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct pltcl_proc_desc
{
	char	   *proname;
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	bool		fn_readonly;
	bool		lanpltrusted;
	FmgrInfo	result_in_func;
	Oid			result_typioparam;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	bool		arg_is_rowtype[FUNC_MAX_ARGS];
} pltcl_proc_desc;


/**********************************************************************
 * The information we cache about prepared and saved plans
 **********************************************************************/
typedef struct pltcl_query_desc
{
	char		qname[20];
	void	   *plan;
	int			nargs;
	Oid		   *argtypes;
	FmgrInfo   *arginfuncs;
	Oid		   *argtypioparams;
} pltcl_query_desc;


/**********************************************************************
 * Global data
 **********************************************************************/
static bool pltcl_pm_init_done = false;
static bool pltcl_be_init_done = false;
static Tcl_Interp *pltcl_hold_interp = NULL;
static Tcl_Interp *pltcl_norm_interp = NULL;
static Tcl_Interp *pltcl_safe_interp = NULL;
static Tcl_HashTable *pltcl_proc_hash = NULL;
static Tcl_HashTable *pltcl_norm_query_hash = NULL;
static Tcl_HashTable *pltcl_safe_query_hash = NULL;

/* these are saved and restored by pltcl_call_handler */
static FunctionCallInfo pltcl_current_fcinfo = NULL;
static pltcl_proc_desc *pltcl_current_prodesc = NULL;

/**********************************************************************
 * Forward declarations
 **********************************************************************/
Datum		pltcl_call_handler(PG_FUNCTION_ARGS);
Datum		pltclu_call_handler(PG_FUNCTION_ARGS);
void		_PG_init(void);

static void pltcl_init_all(void);
static void pltcl_init_interp(Tcl_Interp *interp);
static void pltcl_init_load_unknown(Tcl_Interp *interp);

static Datum pltcl_func_handler(PG_FUNCTION_ARGS);

static HeapTuple pltcl_trigger_handler(PG_FUNCTION_ARGS);

static void throw_tcl_error(Tcl_Interp *interp);

static pltcl_proc_desc *compile_pltcl_function(Oid fn_oid, Oid tgreloid);

static int pltcl_elog(ClientData cdata, Tcl_Interp *interp,
		   int argc, CONST84 char *argv[]);
static int pltcl_quote(ClientData cdata, Tcl_Interp *interp,
			int argc, CONST84 char *argv[]);
static int pltcl_argisnull(ClientData cdata, Tcl_Interp *interp,
				int argc, CONST84 char *argv[]);
static int pltcl_returnnull(ClientData cdata, Tcl_Interp *interp,
				 int argc, CONST84 char *argv[]);

static int pltcl_SPI_execute(ClientData cdata, Tcl_Interp *interp,
				  int argc, CONST84 char *argv[]);
static int pltcl_process_SPI_result(Tcl_Interp *interp,
						 CONST84 char *arrayname,
						 CONST84 char *loop_body,
						 int spi_rc,
						 SPITupleTable *tuptable,
						 int ntuples);
static int pltcl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				  int argc, CONST84 char *argv[]);
static int pltcl_SPI_execute_plan(ClientData cdata, Tcl_Interp *interp,
					   int argc, CONST84 char *argv[]);
static int pltcl_SPI_lastoid(ClientData cdata, Tcl_Interp *interp,
				  int argc, CONST84 char *argv[]);

static void pltcl_set_tuple_values(Tcl_Interp *interp, CONST84 char *arrayname,
					   int tupno, HeapTuple tuple, TupleDesc tupdesc);
static void pltcl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc,
						   Tcl_DString *retval);


/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the cached form of pltcl functions/queries is allocated permanently
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

/*
 * _PG_init()			- library load-time initialization
 *
 * DO NOT make this static nor change its name!
 */
void
_PG_init(void)
{
	/* Be sure we do initialization only once (should be redundant now) */
	if (pltcl_pm_init_done)
		return;

#ifdef WIN32
	/* Required on win32 to prevent error loading init.tcl */
	Tcl_FindExecutable("");
#endif

	/************************************************************
	 * Create the dummy hold interpreter to prevent close of
	 * stdout and stderr on DeleteInterp
	 ************************************************************/
	if ((pltcl_hold_interp = Tcl_CreateInterp()) == NULL)
		elog(ERROR, "could not create \"hold\" interpreter");

	/************************************************************
	 * Create the two interpreters
	 ************************************************************/
	if ((pltcl_norm_interp =
		 Tcl_CreateSlave(pltcl_hold_interp, "norm", 0)) == NULL)
		elog(ERROR, "could not create \"normal\" interpreter");
	pltcl_init_interp(pltcl_norm_interp);

	if ((pltcl_safe_interp =
		 Tcl_CreateSlave(pltcl_hold_interp, "safe", 1)) == NULL)
		elog(ERROR, "could not create \"safe\" interpreter");
	pltcl_init_interp(pltcl_safe_interp);

	/************************************************************
	 * Initialize the proc and query hash tables
	 ************************************************************/
	pltcl_proc_hash = (Tcl_HashTable *) malloc(sizeof(Tcl_HashTable));
	pltcl_norm_query_hash = (Tcl_HashTable *) malloc(sizeof(Tcl_HashTable));
	pltcl_safe_query_hash = (Tcl_HashTable *) malloc(sizeof(Tcl_HashTable));
	Tcl_InitHashTable(pltcl_proc_hash, TCL_STRING_KEYS);
	Tcl_InitHashTable(pltcl_norm_query_hash, TCL_STRING_KEYS);
	Tcl_InitHashTable(pltcl_safe_query_hash, TCL_STRING_KEYS);

	pltcl_pm_init_done = true;
}

/**********************************************************************
 * pltcl_init_all()		- Initialize all
 *
 * This does initialization that can't be done in the postmaster, and
 * hence is not safe to do at library load time.
 **********************************************************************/
static void
pltcl_init_all(void)
{
	/************************************************************
	 * Try to load the unknown procedure from pltcl_modules
	 ************************************************************/
	if (!pltcl_be_init_done)
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		pltcl_init_load_unknown(pltcl_norm_interp);
		pltcl_init_load_unknown(pltcl_safe_interp);
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");
		pltcl_be_init_done = true;
	}
}


/**********************************************************************
 * pltcl_init_interp() - initialize a Tcl interpreter
 **********************************************************************/
static void
pltcl_init_interp(Tcl_Interp *interp)
{
	/************************************************************
	 * Install the commands for SPI support in the interpreter
	 ************************************************************/
	Tcl_CreateCommand(interp, "elog",
					  pltcl_elog, NULL, NULL);
	Tcl_CreateCommand(interp, "quote",
					  pltcl_quote, NULL, NULL);
	Tcl_CreateCommand(interp, "argisnull",
					  pltcl_argisnull, NULL, NULL);
	Tcl_CreateCommand(interp, "return_null",
					  pltcl_returnnull, NULL, NULL);

	Tcl_CreateCommand(interp, "spi_exec",
					  pltcl_SPI_execute, NULL, NULL);
	Tcl_CreateCommand(interp, "spi_prepare",
					  pltcl_SPI_prepare, NULL, NULL);
	Tcl_CreateCommand(interp, "spi_execp",
					  pltcl_SPI_execute_plan, NULL, NULL);
	Tcl_CreateCommand(interp, "spi_lastoid",
					  pltcl_SPI_lastoid, NULL, NULL);
}


/**********************************************************************
 * pltcl_init_load_unknown()	- Load the unknown procedure from
 *				  table pltcl_modules (if it exists)
 **********************************************************************/
static void
pltcl_init_load_unknown(Tcl_Interp *interp)
{
	int			spi_rc;
	int			tcl_rc;
	Tcl_DString unknown_src;
	char	   *part;
	int			i;
	int			fno;

	/************************************************************
	 * Check if table pltcl_modules exists
	 ************************************************************/
	spi_rc = SPI_execute("select 1 from pg_catalog.pg_class "
						 "where relname = 'pltcl_modules'",
						 false, 1);
	SPI_freetuptable(SPI_tuptable);
	if (spi_rc != SPI_OK_SELECT)
		elog(ERROR, "select from pg_class failed");
	if (SPI_processed == 0)
		return;

	/************************************************************
	 * Read all the row's from it where modname = 'unknown' in
	 * the order of modseq
	 ************************************************************/
	Tcl_DStringInit(&unknown_src);

	spi_rc = SPI_execute("select modseq, modsrc from pltcl_modules "
						 "where modname = 'unknown' "
						 "order by modseq",
						 false, 0);
	if (spi_rc != SPI_OK_SELECT)
		elog(ERROR, "select from pltcl_modules failed");

	/************************************************************
	 * If there's nothing, module unknown doesn't exist
	 ************************************************************/
	if (SPI_processed == 0)
	{
		Tcl_DStringFree(&unknown_src);
		SPI_freetuptable(SPI_tuptable);
		elog(WARNING, "module \"unknown\" not found in pltcl_modules");
		return;
	}

	/************************************************************
	 * There is a module named unknown. Resemble the
	 * source from the modsrc attributes and evaluate
	 * it in the Tcl interpreter
	 ************************************************************/
	fno = SPI_fnumber(SPI_tuptable->tupdesc, "modsrc");

	for (i = 0; i < SPI_processed; i++)
	{
		part = SPI_getvalue(SPI_tuptable->vals[i],
							SPI_tuptable->tupdesc, fno);
		if (part != NULL)
		{
			UTF_BEGIN;
			Tcl_DStringAppend(&unknown_src, UTF_E2U(part), -1);
			UTF_END;
			pfree(part);
		}
	}
	tcl_rc = Tcl_GlobalEval(interp, Tcl_DStringValue(&unknown_src));
	Tcl_DStringFree(&unknown_src);
	SPI_freetuptable(SPI_tuptable);
}


/**********************************************************************
 * pltcl_call_handler		- This is the only visible function
 *				  of the PL interpreter. The PostgreSQL
 *				  function manager and trigger manager
 *				  call this function for execution of
 *				  PL/Tcl procedures.
 **********************************************************************/
PG_FUNCTION_INFO_V1(pltcl_call_handler);

/* keep non-static */
Datum
pltcl_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	FunctionCallInfo save_fcinfo;
	pltcl_proc_desc *save_prodesc;

	/*
	 * Initialize interpreters if first time through
	 */
	pltcl_init_all();

	/*
	 * Ensure that static pointers are saved/restored properly
	 */
	save_fcinfo = pltcl_current_fcinfo;
	save_prodesc = pltcl_current_prodesc;

	PG_TRY();
	{
		/*
		 * Determine if called as function or trigger and call appropriate
		 * subhandler
		 */
		if (CALLED_AS_TRIGGER(fcinfo))
		{
			pltcl_current_fcinfo = NULL;
			retval = PointerGetDatum(pltcl_trigger_handler(fcinfo));
		}
		else
		{
			pltcl_current_fcinfo = fcinfo;
			retval = pltcl_func_handler(fcinfo);
		}
	}
	PG_CATCH();
	{
		pltcl_current_fcinfo = save_fcinfo;
		pltcl_current_prodesc = save_prodesc;
		PG_RE_THROW();
	}
	PG_END_TRY();

	pltcl_current_fcinfo = save_fcinfo;
	pltcl_current_prodesc = save_prodesc;

	return retval;
}


/*
 * Alternate handler for unsafe functions
 */
PG_FUNCTION_INFO_V1(pltclu_call_handler);

/* keep non-static */
Datum
pltclu_call_handler(PG_FUNCTION_ARGS)
{
	return pltcl_call_handler(fcinfo);
}

/**********************************************************************
 * pltcl_func_handler()		- Handler for regular function calls
 **********************************************************************/
static Datum
pltcl_func_handler(PG_FUNCTION_ARGS)
{
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	Tcl_DString tcl_cmd;
	Tcl_DString list_tmp;
	int			i;
	int			tcl_rc;
	Datum		retval;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid, InvalidOid);

	pltcl_current_prodesc = prodesc;

	if (prodesc->lanpltrusted)
		interp = pltcl_safe_interp;
	else
		interp = pltcl_norm_interp;

	/************************************************************
	 * Create the tcl command to call the internal
	 * proc in the Tcl interpreter
	 ************************************************************/
	Tcl_DStringInit(&tcl_cmd);
	Tcl_DStringInit(&list_tmp);
	Tcl_DStringAppendElement(&tcl_cmd, prodesc->proname);

	/************************************************************
	 * Add all call arguments to the command
	 ************************************************************/
	PG_TRY();
	{
		for (i = 0; i < prodesc->nargs; i++)
		{
			if (prodesc->arg_is_rowtype[i])
			{
				/**************************************************
				 * For tuple values, add a list for 'array set ...'
				 **************************************************/
				if (fcinfo->argnull[i])
					Tcl_DStringAppendElement(&tcl_cmd, "");
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
					/* Build a temporary HeapTuple control structure */
					tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
					tmptup.t_data = td;

					Tcl_DStringSetLength(&list_tmp, 0);
					pltcl_build_tuple_argument(&tmptup, tupdesc, &list_tmp);
					Tcl_DStringAppendElement(&tcl_cmd,
											 Tcl_DStringValue(&list_tmp));
					ReleaseTupleDesc(tupdesc);
				}
			}
			else
			{
				/**************************************************
				 * Single values are added as string element
				 * of their external representation
				 **************************************************/
				if (fcinfo->argnull[i])
					Tcl_DStringAppendElement(&tcl_cmd, "");
				else
				{
					char	   *tmp;

					tmp = OutputFunctionCall(&prodesc->arg_out_func[i],
											 fcinfo->arg[i]);
					UTF_BEGIN;
					Tcl_DStringAppendElement(&tcl_cmd, UTF_E2U(tmp));
					UTF_END;
					pfree(tmp);
				}
			}
		}
	}
	PG_CATCH();
	{
		Tcl_DStringFree(&tcl_cmd);
		Tcl_DStringFree(&list_tmp);
		PG_RE_THROW();
	}
	PG_END_TRY();
	Tcl_DStringFree(&list_tmp);

	/************************************************************
	 * Call the Tcl function
	 *
	 * We assume no PG error can be thrown directly from this call.
	 ************************************************************/
	tcl_rc = Tcl_GlobalEval(interp, Tcl_DStringValue(&tcl_cmd));
	Tcl_DStringFree(&tcl_cmd);

	/************************************************************
	 * Check for errors reported by Tcl.
	 ************************************************************/
	if (tcl_rc != TCL_OK)
		throw_tcl_error(interp);

	/************************************************************
	 * Disconnect from SPI manager and then create the return
	 * value datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).  But don't try to call
	 * the result_in_func if we've been told to return a NULL;
	 * the Tcl result may not be a valid value of the result type
	 * in that case.
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	if (fcinfo->isnull)
		retval = InputFunctionCall(&prodesc->result_in_func,
								   NULL,
								   prodesc->result_typioparam,
								   -1);
	else
	{
		UTF_BEGIN;
		retval = InputFunctionCall(&prodesc->result_in_func,
								   UTF_U2E((char *) Tcl_GetStringResult(interp)),
								   prodesc->result_typioparam,
								   -1);
		UTF_END;
	}

	return retval;
}


/**********************************************************************
 * pltcl_trigger_handler()	- Handler for trigger calls
 **********************************************************************/
static HeapTuple
pltcl_trigger_handler(PG_FUNCTION_ARGS)
{
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	char	   *stroid;
	TupleDesc	tupdesc;
	volatile HeapTuple rettup;
	Tcl_DString tcl_cmd;
	Tcl_DString tcl_trigtup;
	Tcl_DString tcl_newtup;
	int			tcl_rc;
	int			i;
	int		   *modattrs;
	Datum	   *modvalues;
	char	   *modnulls;
	int			ret_numvals;
	CONST84 char *result;
	CONST84 char **ret_values;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid,
									 RelationGetRelid(trigdata->tg_relation));

	pltcl_current_prodesc = prodesc;

	if (prodesc->lanpltrusted)
		interp = pltcl_safe_interp;
	else
		interp = pltcl_norm_interp;

	tupdesc = trigdata->tg_relation->rd_att;

	/************************************************************
	 * Create the tcl command to call the internal
	 * proc in the interpreter
	 ************************************************************/
	Tcl_DStringInit(&tcl_cmd);
	Tcl_DStringInit(&tcl_trigtup);
	Tcl_DStringInit(&tcl_newtup);
	PG_TRY();
	{
		/* The procedure name */
		Tcl_DStringAppendElement(&tcl_cmd, prodesc->proname);

		/* The trigger name for argument TG_name */
		Tcl_DStringAppendElement(&tcl_cmd, trigdata->tg_trigger->tgname);

		/* The oid of the trigger relation for argument TG_relid */
		stroid = DatumGetCString(DirectFunctionCall1(oidout,
							ObjectIdGetDatum(trigdata->tg_relation->rd_id)));
		Tcl_DStringAppendElement(&tcl_cmd, stroid);
		pfree(stroid);

		/* The name of the table the trigger is acting on: TG_table_name */
		stroid = SPI_getrelname(trigdata->tg_relation);
		Tcl_DStringAppendElement(&tcl_cmd, stroid);
		pfree(stroid);

		/* The schema of the table the trigger is acting on: TG_table_schema */
		stroid = SPI_getnspname(trigdata->tg_relation);
		Tcl_DStringAppendElement(&tcl_cmd, stroid);
		pfree(stroid);

		/* A list of attribute names for argument TG_relatts */
		Tcl_DStringAppendElement(&tcl_trigtup, "");
		for (i = 0; i < tupdesc->natts; i++)
		{
			if (tupdesc->attrs[i]->attisdropped)
				Tcl_DStringAppendElement(&tcl_trigtup, "");
			else
				Tcl_DStringAppendElement(&tcl_trigtup,
										 NameStr(tupdesc->attrs[i]->attname));
		}
		Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));
		Tcl_DStringFree(&tcl_trigtup);
		Tcl_DStringInit(&tcl_trigtup);

		/* The when part of the event for TG_when */
		if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
			Tcl_DStringAppendElement(&tcl_cmd, "BEFORE");
		else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
			Tcl_DStringAppendElement(&tcl_cmd, "AFTER");
		else
			elog(ERROR, "unrecognized WHEN tg_event: %u", trigdata->tg_event);

		/* The level part of the event for TG_level */
		if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		{
			Tcl_DStringAppendElement(&tcl_cmd, "ROW");

			/* Build the data list for the trigtuple */
			pltcl_build_tuple_argument(trigdata->tg_trigtuple,
									   tupdesc, &tcl_trigtup);

			/*
			 * Now the command part of the event for TG_op and data for NEW
			 * and OLD
			 */
			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			{
				Tcl_DStringAppendElement(&tcl_cmd, "INSERT");

				Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));
				Tcl_DStringAppendElement(&tcl_cmd, "");

				rettup = trigdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			{
				Tcl_DStringAppendElement(&tcl_cmd, "DELETE");

				Tcl_DStringAppendElement(&tcl_cmd, "");
				Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));

				rettup = trigdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			{
				Tcl_DStringAppendElement(&tcl_cmd, "UPDATE");

				pltcl_build_tuple_argument(trigdata->tg_newtuple,
										   tupdesc, &tcl_newtup);

				Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_newtup));
				Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));

				rettup = trigdata->tg_newtuple;
			}
			else
				elog(ERROR, "unrecognized OP tg_event: %u", trigdata->tg_event);
		}
		else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		{
			Tcl_DStringAppendElement(&tcl_cmd, "STATEMENT");

			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
				Tcl_DStringAppendElement(&tcl_cmd, "INSERT");
			else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
				Tcl_DStringAppendElement(&tcl_cmd, "DELETE");
			else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				Tcl_DStringAppendElement(&tcl_cmd, "UPDATE");
			else
				elog(ERROR, "unrecognized OP tg_event: %u", trigdata->tg_event);

			Tcl_DStringAppendElement(&tcl_cmd, "");
			Tcl_DStringAppendElement(&tcl_cmd, "");

			rettup = (HeapTuple) NULL;
		}
		else
			elog(ERROR, "unrecognized LEVEL tg_event: %u", trigdata->tg_event);

		/* Finally append the arguments from CREATE TRIGGER */
		for (i = 0; i < trigdata->tg_trigger->tgnargs; i++)
			Tcl_DStringAppendElement(&tcl_cmd, trigdata->tg_trigger->tgargs[i]);

	}
	PG_CATCH();
	{
		Tcl_DStringFree(&tcl_cmd);
		Tcl_DStringFree(&tcl_trigtup);
		Tcl_DStringFree(&tcl_newtup);
		PG_RE_THROW();
	}
	PG_END_TRY();
	Tcl_DStringFree(&tcl_trigtup);
	Tcl_DStringFree(&tcl_newtup);

	/************************************************************
	 * Call the Tcl function
	 *
	 * We assume no PG error can be thrown directly from this call.
	 ************************************************************/
	tcl_rc = Tcl_GlobalEval(interp, Tcl_DStringValue(&tcl_cmd));
	Tcl_DStringFree(&tcl_cmd);

	/************************************************************
	 * Check for errors reported by Tcl.
	 ************************************************************/
	if (tcl_rc != TCL_OK)
		throw_tcl_error(interp);

	/************************************************************
	 * The return value from the procedure might be one of
	 * the magic strings OK or SKIP or a list from array get.
	 * We can check for OK or SKIP without worrying about encoding.
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	result = Tcl_GetStringResult(interp);

	if (strcmp(result, "OK") == 0)
		return rettup;
	if (strcmp(result, "SKIP") == 0)
		return (HeapTuple) NULL;

	/************************************************************
	 * Convert the result value from the Tcl interpreter
	 * and setup structures for SPI_modifytuple();
	 ************************************************************/
	if (Tcl_SplitList(interp, result,
					  &ret_numvals, &ret_values) != TCL_OK)
	{
		UTF_BEGIN;
		elog(ERROR, "could not split return value from trigger: %s",
			 UTF_U2E(Tcl_GetStringResult(interp)));
		UTF_END;
	}

	/* Use a TRY to ensure ret_values will get freed */
	PG_TRY();
	{
		if (ret_numvals % 2 != 0)
			elog(ERROR, "invalid return list from trigger - must have even # of elements");

		modattrs = (int *) palloc(tupdesc->natts * sizeof(int));
		modvalues = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
		for (i = 0; i < tupdesc->natts; i++)
		{
			modattrs[i] = i + 1;
			modvalues[i] = (Datum) NULL;
		}

		modnulls = palloc(tupdesc->natts);
		memset(modnulls, 'n', tupdesc->natts);

		for (i = 0; i < ret_numvals; i += 2)
		{
			CONST84 char *ret_name = ret_values[i];
			CONST84 char *ret_value = ret_values[i + 1];
			int			attnum;
			HeapTuple	typeTup;
			Oid			typinput;
			Oid			typioparam;
			FmgrInfo	finfo;

			/************************************************************
			 * Ignore ".tupno" pseudo elements (see pltcl_set_tuple_values)
			 ************************************************************/
			if (strcmp(ret_name, ".tupno") == 0)
				continue;

			/************************************************************
			 * Get the attribute number
			 ************************************************************/
			attnum = SPI_fnumber(tupdesc, ret_name);
			if (attnum == SPI_ERROR_NOATTRIBUTE)
				elog(ERROR, "invalid attribute \"%s\"", ret_name);
			if (attnum <= 0)
				elog(ERROR, "cannot set system attribute \"%s\"", ret_name);

			/************************************************************
			 * Ignore dropped columns
			 ************************************************************/
			if (tupdesc->attrs[attnum - 1]->attisdropped)
				continue;

			/************************************************************
			 * Lookup the attribute type in the syscache
			 * for the input function
			 ************************************************************/
			typeTup = SearchSysCache(TYPEOID,
					  ObjectIdGetDatum(tupdesc->attrs[attnum - 1]->atttypid),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "cache lookup failed for type %u",
					 tupdesc->attrs[attnum - 1]->atttypid);
			typinput = ((Form_pg_type) GETSTRUCT(typeTup))->typinput;
			typioparam = getTypeIOParam(typeTup);
			ReleaseSysCache(typeTup);

			/************************************************************
			 * Set the attribute to NOT NULL and convert the contents
			 ************************************************************/
			modnulls[attnum - 1] = ' ';
			fmgr_info(typinput, &finfo);
			UTF_BEGIN;
			modvalues[attnum - 1] = InputFunctionCall(&finfo,
												 (char *) UTF_U2E(ret_value),
													  typioparam,
									  tupdesc->attrs[attnum - 1]->atttypmod);
			UTF_END;
		}

		rettup = SPI_modifytuple(trigdata->tg_relation, rettup, tupdesc->natts,
								 modattrs, modvalues, modnulls);

		pfree(modattrs);
		pfree(modvalues);
		pfree(modnulls);

		if (rettup == NULL)
			elog(ERROR, "SPI_modifytuple() failed - RC = %d", SPI_result);

	}
	PG_CATCH();
	{
		ckfree((char *) ret_values);
		PG_RE_THROW();
	}
	PG_END_TRY();
	ckfree((char *) ret_values);

	return rettup;
}


/**********************************************************************
 * throw_tcl_error	- ereport an error returned from the Tcl interpreter
 **********************************************************************/
static void
throw_tcl_error(Tcl_Interp *interp)
{
	/*
	 * Caution is needed here because Tcl_GetVar could overwrite the
	 * interpreter result (even though it's not really supposed to),
	 * and we can't control the order of evaluation of ereport arguments.
	 * Hence, make real sure we have our own copy of the result string
	 * before invoking Tcl_GetVar.
	 */
	char	   *emsg;
	char	   *econtext;

	UTF_BEGIN;
	emsg = pstrdup(UTF_U2E(Tcl_GetStringResult(interp)));
	UTF_END;
	UTF_BEGIN;
	econtext = UTF_U2E((char *) Tcl_GetVar(interp, "errorInfo",
										   TCL_GLOBAL_ONLY));
	ereport(ERROR,
			(errmsg("%s", emsg),
			 errcontext("%s", econtext)));
	UTF_END;
}


/**********************************************************************
 * compile_pltcl_function	- compile (or hopefully just look up) function
 *
 * tgreloid is the OID of the relation when compiling a trigger, or zero
 * (InvalidOid) when compiling a plain function.
 **********************************************************************/
static pltcl_proc_desc *
compile_pltcl_function(Oid fn_oid, Oid tgreloid)
{
	bool		is_trigger = OidIsValid(tgreloid);
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[128];
	Tcl_HashEntry *hashent;
	pltcl_proc_desc *prodesc = NULL;
	Tcl_Interp *interp;
	int			i;
	int			hashnew;
	int			tcl_rc;

	/* We'll need the pg_proc tuple in any case... */
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(fn_oid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/************************************************************
	 * Build our internal proc name from the functions Oid
	 ************************************************************/
	if (!is_trigger)
		snprintf(internal_proname, sizeof(internal_proname),
				 "__PLTcl_proc_%u", fn_oid);
	else
		snprintf(internal_proname, sizeof(internal_proname),
				 "__PLTcl_proc_%u_trigger_%u", fn_oid, tgreloid);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	hashent = Tcl_FindHashEntry(pltcl_proc_hash, internal_proname);

	/************************************************************
	 * If it's present, must check whether it's still up to date.
	 * This is needed because CREATE OR REPLACE FUNCTION can modify the
	 * function's pg_proc entry without changing its OID.
	 ************************************************************/
	if (hashent != NULL)
	{
		bool		uptodate;

		prodesc = (pltcl_proc_desc *) Tcl_GetHashValue(hashent);

		uptodate = (prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
				prodesc->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data));

		if (!uptodate)
		{
			Tcl_DeleteHashEntry(hashent);
			hashent = NULL;
		}
	}

	/************************************************************
	 * If we haven't found it in the hashtable, we analyze
	 * the functions arguments and returntype and store
	 * the in-/out-functions in the prodesc block and create
	 * a new hashtable entry for it.
	 *
	 * Then we load the procedure into the Tcl interpreter.
	 ************************************************************/
	if (hashent == NULL)
	{
		HeapTuple	langTup;
		HeapTuple	typeTup;
		Form_pg_language langStruct;
		Form_pg_type typeStruct;
		Tcl_DString proc_internal_def;
		Tcl_DString proc_internal_body;
		char		proc_internal_args[33 * FUNC_MAX_ARGS];
		Datum		prosrcdatum;
		bool		isnull;
		char	   *proc_source;
		char		buf[32];

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (pltcl_proc_desc *) malloc(sizeof(pltcl_proc_desc));
		if (prodesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		MemSet(prodesc, 0, sizeof(pltcl_proc_desc));
		prodesc->proname = strdup(internal_proname);
		prodesc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		prodesc->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);

		/* Remember if function is STABLE/IMMUTABLE */
		prodesc->fn_readonly =
			(procStruct->provolatile != PROVOLATILE_VOLATILE);

		/************************************************************
		 * Lookup the pg_language tuple by Oid
		 ************************************************************/
		langTup = SearchSysCache(LANGOID,
								 ObjectIdGetDatum(procStruct->prolang),
								 0, 0, 0);
		if (!HeapTupleIsValid(langTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "cache lookup failed for language %u",
				 procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		prodesc->lanpltrusted = langStruct->lanpltrusted;
		ReleaseSysCache(langTup);

		if (prodesc->lanpltrusted)
			interp = pltcl_safe_interp;
		else
			interp = pltcl_norm_interp;

		/************************************************************
		 * Get the required information for input conversion of the
		 * return value.
		 ************************************************************/
		if (!is_trigger)
		{
			typeTup = SearchSysCache(TYPEOID,
									 ObjectIdGetDatum(procStruct->prorettype),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
			{
				free(prodesc->proname);
				free(prodesc);
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			}
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID */
			if (typeStruct->typtype == 'p')
			{
				if (procStruct->prorettype == VOIDOID)
					 /* okay */ ;
				else if (procStruct->prorettype == TRIGGEROID)
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions may only be called as triggers")));
				}
				else
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("pltcl functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}

			if (typeStruct->typtype == 'c')
			{
				free(prodesc->proname);
				free(prodesc);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("pltcl functions cannot return tuples yet")));
			}

			perm_fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
			prodesc->result_typioparam = getTypeIOParam(typeTup);

			ReleaseSysCache(typeTup);
		}

		/************************************************************
		 * Get the required information for output conversion
		 * of all procedure arguments
		 ************************************************************/
		if (!is_trigger)
		{
			prodesc->nargs = procStruct->pronargs;
			proc_internal_args[0] = '\0';
			for (i = 0; i < prodesc->nargs; i++)
			{
				typeTup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(procStruct->proargtypes.values[i]),
										 0, 0, 0);
				if (!HeapTupleIsValid(typeTup))
				{
					free(prodesc->proname);
					free(prodesc);
					elog(ERROR, "cache lookup failed for type %u",
						 procStruct->proargtypes.values[i]);
				}
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* Disallow pseudotype argument */
				if (typeStruct->typtype == 'p')
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("pltcl functions cannot take type %s",
						format_type_be(procStruct->proargtypes.values[i]))));
				}

				if (typeStruct->typtype == 'c')
				{
					prodesc->arg_is_rowtype[i] = true;
					snprintf(buf, sizeof(buf), "__PLTcl_Tup_%d", i + 1);
				}
				else
				{
					prodesc->arg_is_rowtype[i] = false;
					perm_fmgr_info(typeStruct->typoutput,
								   &(prodesc->arg_out_func[i]));
					snprintf(buf, sizeof(buf), "%d", i + 1);
				}

				if (i > 0)
					strcat(proc_internal_args, " ");
				strcat(proc_internal_args, buf);

				ReleaseSysCache(typeTup);
			}
		}
		else
		{
			/* trigger procedure has fixed args */
			strcpy(proc_internal_args,
				   "TG_name TG_relid TG_table_name TG_table_schema TG_relatts TG_when TG_level TG_op __PLTcl_Tup_NEW __PLTcl_Tup_OLD args");
		}

		/************************************************************
		 * Create the tcl command to define the internal
		 * procedure
		 ************************************************************/
		Tcl_DStringInit(&proc_internal_def);
		Tcl_DStringInit(&proc_internal_body);
		Tcl_DStringAppendElement(&proc_internal_def, "proc");
		Tcl_DStringAppendElement(&proc_internal_def, internal_proname);
		Tcl_DStringAppendElement(&proc_internal_def, proc_internal_args);

		/************************************************************
		 * prefix procedure body with
		 * upvar #0 <internal_procname> GD
		 * and with appropriate setting of arguments
		 ************************************************************/
		Tcl_DStringAppend(&proc_internal_body, "upvar #0 ", -1);
		Tcl_DStringAppend(&proc_internal_body, internal_proname, -1);
		Tcl_DStringAppend(&proc_internal_body, " GD\n", -1);
		if (!is_trigger)
		{
			for (i = 0; i < prodesc->nargs; i++)
			{
				if (prodesc->arg_is_rowtype[i])
				{
					snprintf(buf, sizeof(buf),
							 "array set %d $__PLTcl_Tup_%d\n",
							 i + 1, i + 1);
					Tcl_DStringAppend(&proc_internal_body, buf, -1);
				}
			}
		}
		else
		{
			Tcl_DStringAppend(&proc_internal_body,
							  "array set NEW $__PLTcl_Tup_NEW\n", -1);
			Tcl_DStringAppend(&proc_internal_body,
							  "array set OLD $__PLTcl_Tup_OLD\n", -1);

			Tcl_DStringAppend(&proc_internal_body,
							  "set i 0\n"
							  "set v 0\n"
							  "foreach v $args {\n"
							  "  incr i\n"
							  "  set $i $v\n"
							  "}\n"
							  "unset i v\n\n", -1);
		}

		/************************************************************
		 * Add user's function definition to proc body
		 ************************************************************/
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
		proc_source = DatumGetCString(DirectFunctionCall1(textout,
														  prosrcdatum));
		UTF_BEGIN;
		Tcl_DStringAppend(&proc_internal_body, UTF_E2U(proc_source), -1);
		UTF_END;
		pfree(proc_source);
		Tcl_DStringAppendElement(&proc_internal_def,
								 Tcl_DStringValue(&proc_internal_body));
		Tcl_DStringFree(&proc_internal_body);

		/************************************************************
		 * Create the procedure in the interpreter
		 ************************************************************/
		tcl_rc = Tcl_GlobalEval(interp,
								Tcl_DStringValue(&proc_internal_def));
		Tcl_DStringFree(&proc_internal_def);
		if (tcl_rc != TCL_OK)
		{
			free(prodesc->proname);
			free(prodesc);
			UTF_BEGIN;
			elog(ERROR, "could not create internal procedure \"%s\": %s",
				 internal_proname, UTF_U2E(Tcl_GetStringResult(interp)));
			UTF_END;
		}

		/************************************************************
		 * Add the proc description block to the hashtable
		 ************************************************************/
		hashent = Tcl_CreateHashEntry(pltcl_proc_hash,
									  prodesc->proname, &hashnew);
		Tcl_SetHashValue(hashent, (ClientData) prodesc);
	}

	ReleaseSysCache(procTup);

	return prodesc;
}


/**********************************************************************
 * pltcl_elog()		- elog() support for PLTcl
 **********************************************************************/
static int
pltcl_elog(ClientData cdata, Tcl_Interp *interp,
		   int argc, CONST84 char *argv[])
{
	volatile int level;
	MemoryContext oldcontext;

	if (argc != 3)
	{
		Tcl_SetResult(interp, "syntax error - 'elog level msg'", TCL_STATIC);
		return TCL_ERROR;
	}

	if (strcmp(argv[1], "DEBUG") == 0)
		level = DEBUG2;
	else if (strcmp(argv[1], "LOG") == 0)
		level = LOG;
	else if (strcmp(argv[1], "INFO") == 0)
		level = INFO;
	else if (strcmp(argv[1], "NOTICE") == 0)
		level = NOTICE;
	else if (strcmp(argv[1], "WARNING") == 0)
		level = WARNING;
	else if (strcmp(argv[1], "ERROR") == 0)
		level = ERROR;
	else if (strcmp(argv[1], "FATAL") == 0)
		level = FATAL;
	else
	{
		Tcl_AppendResult(interp, "Unknown elog level '", argv[1],
						 "'", NULL);
		return TCL_ERROR;
	}

	if (level == ERROR)
	{
		/*
		 * We just pass the error back to Tcl.  If it's not caught,
		 * it'll eventually get converted to a PG error when we reach
		 * the call handler.
		 */
		Tcl_SetResult(interp, (char *) argv[2], TCL_VOLATILE);
		return TCL_ERROR;
	}

	/*
	 * For non-error messages, just pass 'em to elog().  We do not expect
	 * that this will fail, but just on the off chance it does, report the
	 * error back to Tcl.  Note we are assuming that elog() can't have any
	 * internal failures that are so bad as to require a transaction abort.
	 *
	 * This path is also used for FATAL errors, which aren't going to come
	 * back to us at all.
	 */
	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		UTF_BEGIN;
		elog(level, "%s", UTF_U2E(argv[2]));
		UTF_END;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Must reset elog.c's state */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Pass the error message to Tcl */
		UTF_BEGIN;
		Tcl_SetResult(interp, UTF_E2U(edata->message), TCL_VOLATILE);
		UTF_END;
		FreeErrorData(edata);

		return TCL_ERROR;
	}
	PG_END_TRY();

	return TCL_OK;
}


/**********************************************************************
 * pltcl_quote()	- quote literal strings that are to
 *			  be used in SPI_execute query strings
 **********************************************************************/
static int
pltcl_quote(ClientData cdata, Tcl_Interp *interp,
			int argc, CONST84 char *argv[])
{
	char	   *tmp;
	const char *cp1;
	char	   *cp2;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (argc != 2)
	{
		Tcl_SetResult(interp, "syntax error - 'quote string'", TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Allocate space for the maximum the string can
	 * grow to and initialize pointers
	 ************************************************************/
	tmp = palloc(strlen(argv[1]) * 2 + 1);
	cp1 = argv[1];
	cp2 = tmp;

	/************************************************************
	 * Walk through string and double every quote and backslash
	 ************************************************************/
	while (*cp1)
	{
		if (*cp1 == '\'')
			*cp2++ = '\'';
		else
		{
			if (*cp1 == '\\')
				*cp2++ = '\\';
		}
		*cp2++ = *cp1++;
	}

	/************************************************************
	 * Terminate the string and set it as result
	 ************************************************************/
	*cp2 = '\0';
	Tcl_SetResult(interp, tmp, TCL_VOLATILE);
	pfree(tmp);
	return TCL_OK;
}


/**********************************************************************
 * pltcl_argisnull()	- determine if a specific argument is NULL
 **********************************************************************/
static int
pltcl_argisnull(ClientData cdata, Tcl_Interp *interp,
				int argc, CONST84 char *argv[])
{
	int			argno;
	FunctionCallInfo fcinfo = pltcl_current_fcinfo;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (argc != 2)
	{
		Tcl_SetResult(interp, "syntax error - 'argisnull argno'",
					  TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Check that we're called as a normal function
	 ************************************************************/
	if (fcinfo == NULL)
	{
		Tcl_SetResult(interp, "argisnull cannot be used in triggers",
					  TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Get the argument number
	 ************************************************************/
	if (Tcl_GetInt(interp, argv[1], &argno) != TCL_OK)
		return TCL_ERROR;

	/************************************************************
	 * Check that the argno is valid
	 ************************************************************/
	argno--;
	if (argno < 0 || argno >= fcinfo->nargs)
	{
		Tcl_SetResult(interp, "argno out of range", TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Get the requested NULL state
	 ************************************************************/
	if (PG_ARGISNULL(argno))
		Tcl_SetResult(interp, "1", TCL_STATIC);
	else
		Tcl_SetResult(interp, "0", TCL_STATIC);

	return TCL_OK;
}


/**********************************************************************
 * pltcl_returnnull()	- Cause a NULL return from a function
 **********************************************************************/
static int
pltcl_returnnull(ClientData cdata, Tcl_Interp *interp,
				 int argc, CONST84 char *argv[])
{
	FunctionCallInfo fcinfo = pltcl_current_fcinfo;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (argc != 1)
	{
		Tcl_SetResult(interp, "syntax error - 'return_null'", TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Check that we're called as a normal function
	 ************************************************************/
	if (fcinfo == NULL)
	{
		Tcl_SetResult(interp, "return_null cannot be used in triggers",
					  TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Set the NULL return flag and cause Tcl to return from the
	 * procedure.
	 ************************************************************/
	fcinfo->isnull = true;

	return TCL_RETURN;
}


/*----------
 * Support for running SPI operations inside subtransactions
 *
 * Intended usage pattern is:
 *
 *	MemoryContext oldcontext = CurrentMemoryContext;
 *	ResourceOwner oldowner = CurrentResourceOwner;
 *
 *	...
 *	pltcl_subtrans_begin(oldcontext, oldowner);
 *	PG_TRY();
 *	{
 *		do something risky;
 *		pltcl_subtrans_commit(oldcontext, oldowner);
 *	}
 *	PG_CATCH();
 *	{
 *		pltcl_subtrans_abort(interp, oldcontext, oldowner);
 *		return TCL_ERROR;
 *	}
 *	PG_END_TRY();
 *	return TCL_OK;
 *----------
 */
static void
pltcl_subtrans_begin(MemoryContext oldcontext, ResourceOwner oldowner)
{
	BeginInternalSubTransaction(NULL);

	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);
}

static void
pltcl_subtrans_commit(MemoryContext oldcontext, ResourceOwner oldowner)
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

static void
pltcl_subtrans_abort(Tcl_Interp *interp,
					 MemoryContext oldcontext, ResourceOwner oldowner)
{
	ErrorData  *edata;

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

	/* Pass the error message to Tcl */
	UTF_BEGIN;
	Tcl_SetResult(interp, UTF_E2U(edata->message), TCL_VOLATILE);
	UTF_END;
	FreeErrorData(edata);
}


/**********************************************************************
 * pltcl_SPI_execute()		- The builtin SPI_execute command
 *				  for the Tcl interpreter
 **********************************************************************/
static int
pltcl_SPI_execute(ClientData cdata, Tcl_Interp *interp,
				  int argc, CONST84 char *argv[])
{
	int			my_rc;
	int			spi_rc;
	int			query_idx;
	int			i;
	int			count = 0;
	CONST84 char *volatile arrayname = NULL;
	CONST84 char *volatile loop_body = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	char	   *usage = "syntax error - 'SPI_exec "
	"?-count n? "
	"?-array name? query ?loop body?";

	/************************************************************
	 * Check the call syntax and get the options
	 ************************************************************/
	if (argc < 2)
	{
		Tcl_SetResult(interp, usage, TCL_STATIC);
		return TCL_ERROR;
	}

	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "-array") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_STATIC);
				return TCL_ERROR;
			}
			arrayname = argv[i++];
			continue;
		}

		if (strcmp(argv[i], "-count") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_STATIC);
				return TCL_ERROR;
			}
			if (Tcl_GetInt(interp, argv[i++], &count) != TCL_OK)
				return TCL_ERROR;
			continue;
		}

		break;
	}

	query_idx = i;
	if (query_idx >= argc || query_idx + 2 < argc)
	{
		Tcl_SetResult(interp, usage, TCL_STATIC);
		return TCL_ERROR;
	}
	if (query_idx + 1 < argc)
		loop_body = argv[query_idx + 1];

	/************************************************************
	 * Execute the query inside a sub-transaction, so we can cope with
	 * errors sanely
	 ************************************************************/

	pltcl_subtrans_begin(oldcontext, oldowner);

	PG_TRY();
	{
		UTF_BEGIN;
		spi_rc = SPI_execute(UTF_U2E(argv[query_idx]),
							 pltcl_current_prodesc->fn_readonly, count);
		UTF_END;

		my_rc = pltcl_process_SPI_result(interp,
										 arrayname,
										 loop_body,
										 spi_rc,
										 SPI_tuptable,
										 SPI_processed);

		pltcl_subtrans_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		pltcl_subtrans_abort(interp, oldcontext, oldowner);
		return TCL_ERROR;
	}
	PG_END_TRY();

	return my_rc;
}

/*
 * Process the result from SPI_execute or SPI_execute_plan
 *
 * Shared code between pltcl_SPI_execute and pltcl_SPI_execute_plan
 */
static int
pltcl_process_SPI_result(Tcl_Interp *interp,
						 CONST84 char *arrayname,
						 CONST84 char *loop_body,
						 int spi_rc,
						 SPITupleTable *tuptable,
						 int ntuples)
{
	int			my_rc = TCL_OK;
	char		buf[64];
	int			i;
	int			loop_rc;
	HeapTuple  *tuples;
	TupleDesc	tupdesc;

	switch (spi_rc)
	{
		case SPI_OK_SELINTO:
		case SPI_OK_INSERT:
		case SPI_OK_DELETE:
		case SPI_OK_UPDATE:
			snprintf(buf, sizeof(buf), "%d", ntuples);
			Tcl_SetResult(interp, buf, TCL_VOLATILE);
			break;

		case SPI_OK_UTILITY:
			if (tuptable == NULL)
			{
				Tcl_SetResult(interp, "0", TCL_STATIC);
				break;
			}
			/* FALL THRU for utility returning tuples */

		case SPI_OK_SELECT:
		case SPI_OK_INSERT_RETURNING:
		case SPI_OK_DELETE_RETURNING:
		case SPI_OK_UPDATE_RETURNING:

			/*
			 * Process the tuples we got
			 */
			tuples = tuptable->vals;
			tupdesc = tuptable->tupdesc;

			if (loop_body == NULL)
			{
				/*
				 * If there is no loop body given, just set the variables from
				 * the first tuple (if any)
				 */
				if (ntuples > 0)
					pltcl_set_tuple_values(interp, arrayname, 0,
										   tuples[0], tupdesc);
			}
			else
			{
				/*
				 * There is a loop body - process all tuples and evaluate the
				 * body on each
				 */
				for (i = 0; i < ntuples; i++)
				{
					pltcl_set_tuple_values(interp, arrayname, i,
										   tuples[i], tupdesc);

					loop_rc = Tcl_Eval(interp, loop_body);

					if (loop_rc == TCL_OK)
						continue;
					if (loop_rc == TCL_CONTINUE)
						continue;
					if (loop_rc == TCL_RETURN)
					{
						my_rc = TCL_RETURN;
						break;
					}
					if (loop_rc == TCL_BREAK)
						break;
					my_rc = TCL_ERROR;
					break;
				}
			}

			if (my_rc == TCL_OK)
			{
				snprintf(buf, sizeof(buf), "%d", ntuples);
				Tcl_SetResult(interp, buf, TCL_VOLATILE);
			}
			break;

		default:
			Tcl_AppendResult(interp, "pltcl: SPI_execute failed: ",
							 SPI_result_code_string(spi_rc), NULL);
			my_rc = TCL_ERROR;
			break;
	}

	SPI_freetuptable(tuptable);

	return my_rc;
}


/**********************************************************************
 * pltcl_SPI_prepare()		- Builtin support for prepared plans
 *				  The Tcl command SPI_prepare
 *				  always saves the plan using
 *				  SPI_saveplan and returns a key for
 *				  access. There is no chance to prepare
 *				  and not save the plan currently.
 **********************************************************************/
static int
pltcl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				  int argc, CONST84 char *argv[])
{
	int			nargs;
	CONST84 char **args;
	pltcl_query_desc *qdesc;
	void	   *plan;
	int			i;
	Tcl_HashEntry *hashent;
	int			hashnew;
	Tcl_HashTable *query_hash;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	/************************************************************
	 * Check the call syntax
	 ************************************************************/
	if (argc != 3)
	{
		Tcl_SetResult(interp, "syntax error - 'SPI_prepare query argtypes'",
					  TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Split the argument type list
	 ************************************************************/
	if (Tcl_SplitList(interp, argv[2], &nargs, &args) != TCL_OK)
		return TCL_ERROR;

	/************************************************************
	 * Allocate the new querydesc structure
	 ************************************************************/
	qdesc = (pltcl_query_desc *) malloc(sizeof(pltcl_query_desc));
	snprintf(qdesc->qname, sizeof(qdesc->qname), "%lx", (long) qdesc);
	qdesc->nargs = nargs;
	qdesc->argtypes = (Oid *) malloc(nargs * sizeof(Oid));
	qdesc->arginfuncs = (FmgrInfo *) malloc(nargs * sizeof(FmgrInfo));
	qdesc->argtypioparams = (Oid *) malloc(nargs * sizeof(Oid));

	/************************************************************
	 * Execute the prepare inside a sub-transaction, so we can cope with
	 * errors sanely
	 ************************************************************/

	pltcl_subtrans_begin(oldcontext, oldowner);

	PG_TRY();
	{
		/************************************************************
		 * Lookup the argument types by name in the system cache
		 * and remember the required information for input conversion
		 ************************************************************/
		for (i = 0; i < nargs; i++)
		{
			List	   *names;
			HeapTuple	typeTup;

			/* Parse possibly-qualified type name and look it up in pg_type */
			names = stringToQualifiedNameList(args[i],
											  "pltcl_SPI_prepare");
			typeTup = typenameType(NULL, makeTypeNameFromNameList(names));
			qdesc->argtypes[i] = HeapTupleGetOid(typeTup);
			perm_fmgr_info(((Form_pg_type) GETSTRUCT(typeTup))->typinput,
						   &(qdesc->arginfuncs[i]));
			qdesc->argtypioparams[i] = getTypeIOParam(typeTup);
			ReleaseSysCache(typeTup);
		}

		/************************************************************
		 * Prepare the plan and check for errors
		 ************************************************************/
		UTF_BEGIN;
		plan = SPI_prepare(UTF_U2E(argv[1]), nargs, qdesc->argtypes);
		UTF_END;

		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed");

		/************************************************************
		 * Save the plan into permanent memory (right now it's in the
		 * SPI procCxt, which will go away at function end).
		 ************************************************************/
		qdesc->plan = SPI_saveplan(plan);
		if (qdesc->plan == NULL)
			elog(ERROR, "SPI_saveplan() failed");

		/* Release the procCxt copy to avoid within-function memory leak */
		SPI_freeplan(plan);

		pltcl_subtrans_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		pltcl_subtrans_abort(interp, oldcontext, oldowner);

		free(qdesc->argtypes);
		free(qdesc->arginfuncs);
		free(qdesc->argtypioparams);
		free(qdesc);
		ckfree((char *) args);

		return TCL_ERROR;
	}
	PG_END_TRY();

	/************************************************************
	 * Insert a hashtable entry for the plan and return
	 * the key to the caller
	 ************************************************************/
	if (interp == pltcl_norm_interp)
		query_hash = pltcl_norm_query_hash;
	else
		query_hash = pltcl_safe_query_hash;

	hashent = Tcl_CreateHashEntry(query_hash, qdesc->qname, &hashnew);
	Tcl_SetHashValue(hashent, (ClientData) qdesc);

	ckfree((char *) args);

	/* qname is ASCII, so no need for encoding conversion */
	Tcl_SetResult(interp, qdesc->qname, TCL_VOLATILE);
	return TCL_OK;
}


/**********************************************************************
 * pltcl_SPI_execute_plan()		- Execute a prepared plan
 **********************************************************************/
static int
pltcl_SPI_execute_plan(ClientData cdata, Tcl_Interp *interp,
					   int argc, CONST84 char *argv[])
{
	int			my_rc;
	int			spi_rc;
	int			i;
	int			j;
	Tcl_HashEntry *hashent;
	pltcl_query_desc *qdesc;
	const char *volatile nulls = NULL;
	CONST84 char *volatile arrayname = NULL;
	CONST84 char *volatile loop_body = NULL;
	int			count = 0;
	int			callnargs;
	CONST84 char **callargs = NULL;
	Datum	   *argvalues;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	Tcl_HashTable *query_hash;

	char	   *usage = "syntax error - 'SPI_execp "
	"?-nulls string? ?-count n? "
	"?-array name? query ?args? ?loop body?";

	/************************************************************
	 * Get the options and check syntax
	 ************************************************************/
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "-array") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_STATIC);
				return TCL_ERROR;
			}
			arrayname = argv[i++];
			continue;
		}
		if (strcmp(argv[i], "-nulls") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_STATIC);
				return TCL_ERROR;
			}
			nulls = argv[i++];
			continue;
		}
		if (strcmp(argv[i], "-count") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_STATIC);
				return TCL_ERROR;
			}
			if (Tcl_GetInt(interp, argv[i++], &count) != TCL_OK)
				return TCL_ERROR;
			continue;
		}

		break;
	}

	/************************************************************
	 * Get the prepared plan descriptor by its key
	 ************************************************************/
	if (i >= argc)
	{
		Tcl_SetResult(interp, usage, TCL_STATIC);
		return TCL_ERROR;
	}

	if (interp == pltcl_norm_interp)
		query_hash = pltcl_norm_query_hash;
	else
		query_hash = pltcl_safe_query_hash;

	hashent = Tcl_FindHashEntry(query_hash, argv[i]);
	if (hashent == NULL)
	{
		Tcl_AppendResult(interp, "invalid queryid '", argv[i], "'", NULL);
		return TCL_ERROR;
	}
	qdesc = (pltcl_query_desc *) Tcl_GetHashValue(hashent);
	i++;

	/************************************************************
	 * If a nulls string is given, check for correct length
	 ************************************************************/
	if (nulls != NULL)
	{
		if (strlen(nulls) != qdesc->nargs)
		{
			Tcl_SetResult(interp,
					   "length of nulls string doesn't match # of arguments",
						  TCL_STATIC);
			return TCL_ERROR;
		}
	}

	/************************************************************
	 * If there was a argtype list on preparation, we need
	 * an argument value list now
	 ************************************************************/
	if (qdesc->nargs > 0)
	{
		if (i >= argc)
		{
			Tcl_SetResult(interp, "missing argument list", TCL_STATIC);
			return TCL_ERROR;
		}

		/************************************************************
		 * Split the argument values
		 ************************************************************/
		if (Tcl_SplitList(interp, argv[i++], &callnargs, &callargs) != TCL_OK)
			return TCL_ERROR;

		/************************************************************
		 * Check that the # of arguments matches
		 ************************************************************/
		if (callnargs != qdesc->nargs)
		{
			Tcl_SetResult(interp,
			   "argument list length doesn't match # of arguments for query",
						  TCL_STATIC);
			ckfree((char *) callargs);
			return TCL_ERROR;
		}
	}
	else
		callnargs = 0;

	/************************************************************
	 * Get loop body if present
	 ************************************************************/
	if (i < argc)
		loop_body = argv[i++];

	if (i != argc)
	{
		Tcl_SetResult(interp, usage, TCL_STATIC);
		return TCL_ERROR;
	}

	/************************************************************
	 * Execute the plan inside a sub-transaction, so we can cope with
	 * errors sanely
	 ************************************************************/

	pltcl_subtrans_begin(oldcontext, oldowner);

	PG_TRY();
	{
		/************************************************************
		 * Setup the value array for SPI_execute_plan() using
		 * the type specific input functions
		 ************************************************************/
		argvalues = (Datum *) palloc(callnargs * sizeof(Datum));

		for (j = 0; j < callnargs; j++)
		{
			if (nulls && nulls[j] == 'n')
			{
				argvalues[j] = InputFunctionCall(&qdesc->arginfuncs[j],
												 NULL,
												 qdesc->argtypioparams[j],
												 -1);
			}
			else
			{
				UTF_BEGIN;
				argvalues[j] = InputFunctionCall(&qdesc->arginfuncs[j],
											   (char *) UTF_U2E(callargs[j]),
												 qdesc->argtypioparams[j],
												 -1);
				UTF_END;
			}
		}

		if (callargs)
			ckfree((char *) callargs);
		callargs = NULL;

		/************************************************************
		 * Execute the plan
		 ************************************************************/
		spi_rc = SPI_execute_plan(qdesc->plan, argvalues, nulls,
								  pltcl_current_prodesc->fn_readonly, count);

		my_rc = pltcl_process_SPI_result(interp,
										 arrayname,
										 loop_body,
										 spi_rc,
										 SPI_tuptable,
										 SPI_processed);

		pltcl_subtrans_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		pltcl_subtrans_abort(interp, oldcontext, oldowner);

		if (callargs)
			ckfree((char *) callargs);

		return TCL_ERROR;
	}
	PG_END_TRY();

	return my_rc;
}


/**********************************************************************
 * pltcl_SPI_lastoid()	- return the last oid. To
 *		  be used after insert queries
 **********************************************************************/
static int
pltcl_SPI_lastoid(ClientData cdata, Tcl_Interp *interp,
				  int argc, CONST84 char *argv[])
{
	char		buf[64];

	snprintf(buf, sizeof(buf), "%u", SPI_lastoid);
	Tcl_SetResult(interp, buf, TCL_VOLATILE);
	return TCL_OK;
}


/**********************************************************************
 * pltcl_set_tuple_values() - Set variables for all attributes
 *				  of a given tuple
 **********************************************************************/
static void
pltcl_set_tuple_values(Tcl_Interp *interp, CONST84 char *arrayname,
					   int tupno, HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	char	   *outputstr;
	char		buf[64];
	Datum		attr;
	bool		isnull;

	CONST84 char *attname;
	HeapTuple	typeTup;
	Oid			typoutput;

	CONST84 char **arrptr;
	CONST84 char **nameptr;
	CONST84 char *nullname = NULL;

	/************************************************************
	 * Prepare pointers for Tcl_SetVar2() below and in array
	 * mode set the .tupno element
	 ************************************************************/
	if (arrayname == NULL)
	{
		arrptr = &attname;
		nameptr = &nullname;
	}
	else
	{
		arrptr = &arrayname;
		nameptr = &attname;
		snprintf(buf, sizeof(buf), "%d", tupno);
		Tcl_SetVar2(interp, arrayname, ".tupno", buf, 0);
	}

	for (i = 0; i < tupdesc->natts; i++)
	{
		/* ignore dropped attributes */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		attname = NameStr(tupdesc->attrs[i]->attname);

		/************************************************************
		 * Get the attributes value
		 ************************************************************/
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/************************************************************
		 * Lookup the attribute type in the syscache
		 * for the output function
		 ************************************************************/
		typeTup = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "cache lookup failed for type %u",
				 tupdesc->attrs[i]->atttypid);

		typoutput = ((Form_pg_type) GETSTRUCT(typeTup))->typoutput;
		ReleaseSysCache(typeTup);

		/************************************************************
		 * If there is a value, set the variable
		 * If not, unset it
		 *
		 * Hmmm - Null attributes will cause functions to
		 *		  crash if they don't expect them - need something
		 *		  smarter here.
		 ************************************************************/
		if (!isnull && OidIsValid(typoutput))
		{
			outputstr = OidOutputFunctionCall(typoutput, attr);
			UTF_BEGIN;
			Tcl_SetVar2(interp, *arrptr, *nameptr, UTF_E2U(outputstr), 0);
			UTF_END;
			pfree(outputstr);
		}
		else
			Tcl_UnsetVar2(interp, *arrptr, *nameptr, 0);
	}
}


/**********************************************************************
 * pltcl_build_tuple_argument() - Build a string usable for 'array set'
 *				  from all attributes of a given tuple
 **********************************************************************/
static void
pltcl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc,
						   Tcl_DString *retval)
{
	int			i;
	char	   *outputstr;
	Datum		attr;
	bool		isnull;

	char	   *attname;
	HeapTuple	typeTup;
	Oid			typoutput;

	for (i = 0; i < tupdesc->natts; i++)
	{
		/* ignore dropped attributes */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		attname = NameStr(tupdesc->attrs[i]->attname);

		/************************************************************
		 * Get the attributes value
		 ************************************************************/
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/************************************************************
		 * Lookup the attribute type in the syscache
		 * for the output function
		 ************************************************************/
		typeTup = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "cache lookup failed for type %u",
				 tupdesc->attrs[i]->atttypid);

		typoutput = ((Form_pg_type) GETSTRUCT(typeTup))->typoutput;
		ReleaseSysCache(typeTup);

		/************************************************************
		 * If there is a value, append the attribute name and the
		 * value to the list
		 *
		 * Hmmm - Null attributes will cause functions to
		 *		  crash if they don't expect them - need something
		 *		  smarter here.
		 ************************************************************/
		if (!isnull && OidIsValid(typoutput))
		{
			outputstr = OidOutputFunctionCall(typoutput, attr);
			Tcl_DStringAppendElement(retval, attname);
			UTF_BEGIN;
			Tcl_DStringAppendElement(retval, UTF_E2U(outputstr));
			UTF_END;
			pfree(outputstr);
		}
	}
}
