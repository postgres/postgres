/**********************************************************************
 * pltcl.c		- PostgreSQL support for Tcl as
 *				  procedural language (PL)
 *
 *	  src/pl/tcl/pltcl.c
 *
 **********************************************************************/

#include "postgres.h"

#include <tcl.h>

#include <unistd.h>
#include <fcntl.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


PG_MODULE_MAGIC;

#define HAVE_TCL_VERSION(maj,min) \
	((TCL_MAJOR_VERSION > maj) || \
	 (TCL_MAJOR_VERSION == maj && TCL_MINOR_VERSION >= min))

/* Insist on Tcl >= 8.4 */
#if !HAVE_TCL_VERSION(8,4)
#error PostgreSQL only supports Tcl 8.4 or later.
#endif

/* Hack to deal with Tcl 8.6 const-ification without losing compatibility */
#ifndef CONST86
#define CONST86
#endif

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("pltcl")


/*
 * Support for converting between UTF8 (which is what all strings going into
 * or out of Tcl should be) and the database encoding.
 *
 * If you just use utf_u2e() or utf_e2u() directly, they will leak some
 * palloc'd space when doing a conversion.  This is not worth worrying about
 * if it only happens, say, once per PL/Tcl function call.  If it does seem
 * worth worrying about, use the wrapper macros.
 */

static inline char *
utf_u2e(const char *src)
{
	return pg_any_to_server(src, strlen(src), PG_UTF8);
}

static inline char *
utf_e2u(const char *src)
{
	return pg_server_to_any(src, strlen(src), PG_UTF8);
}

#define UTF_BEGIN \
	do { \
		const char *_pltcl_utf_src = NULL; \
		char *_pltcl_utf_dst = NULL

#define UTF_END \
	if (_pltcl_utf_src != (const char *) _pltcl_utf_dst) \
			pfree(_pltcl_utf_dst); \
	} while (0)

#define UTF_U2E(x) \
	(_pltcl_utf_dst = utf_u2e(_pltcl_utf_src = (x)))

#define UTF_E2U(x) \
	(_pltcl_utf_dst = utf_e2u(_pltcl_utf_src = (x)))


/**********************************************************************
 * Information associated with a Tcl interpreter.  We have one interpreter
 * that is used for all pltclu (untrusted) functions.  For pltcl (trusted)
 * functions, there is a separate interpreter for each effective SQL userid.
 * (This is needed to ensure that an unprivileged user can't inject Tcl code
 * that'll be executed with the privileges of some other SQL user.)
 *
 * The pltcl_interp_desc structs are kept in a Postgres hash table indexed
 * by userid OID, with OID 0 used for the single untrusted interpreter.
 **********************************************************************/
typedef struct pltcl_interp_desc
{
	Oid			user_id;		/* Hash key (must be first!) */
	Tcl_Interp *interp;			/* The interpreter */
	Tcl_HashTable query_hash;	/* pltcl_query_desc structs */
} pltcl_interp_desc;


/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct pltcl_proc_desc
{
	char	   *user_proname;
	char	   *internal_proname;
	TransactionId fn_xmin;
	ItemPointerData fn_tid;
	bool		fn_readonly;
	bool		lanpltrusted;
	pltcl_interp_desc *interp_desc;
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
	SPIPlanPtr	plan;
	int			nargs;
	Oid		   *argtypes;
	FmgrInfo   *arginfuncs;
	Oid		   *argtypioparams;
} pltcl_query_desc;


/**********************************************************************
 * For speedy lookup, we maintain a hash table mapping from
 * function OID + trigger flag + user OID to pltcl_proc_desc pointers.
 * The reason the pltcl_proc_desc struct isn't directly part of the hash
 * entry is to simplify recovery from errors during compile_pltcl_function.
 *
 * Note: if the same function is called by multiple userIDs within a session,
 * there will be a separate pltcl_proc_desc entry for each userID in the case
 * of pltcl functions, but only one entry for pltclu functions, because we
 * set user_id = 0 for that case.
 **********************************************************************/
typedef struct pltcl_proc_key
{
	Oid			proc_id;		/* Function OID */

	/*
	 * is_trigger is really a bool, but declare as Oid to ensure this struct
	 * contains no padding
	 */
	Oid			is_trigger;		/* is it a trigger function? */
	Oid			user_id;		/* User calling the function, or 0 */
} pltcl_proc_key;

typedef struct pltcl_proc_ptr
{
	pltcl_proc_key proc_key;	/* Hash key (must be first!) */
	pltcl_proc_desc *proc_ptr;
} pltcl_proc_ptr;


/**********************************************************************
 * Global data
 **********************************************************************/
static bool pltcl_pm_init_done = false;
static Tcl_Interp *pltcl_hold_interp = NULL;
static HTAB *pltcl_interp_htab = NULL;
static HTAB *pltcl_proc_htab = NULL;

/* these are saved and restored by pltcl_handler */
static FunctionCallInfo pltcl_current_fcinfo = NULL;
static pltcl_proc_desc *pltcl_current_prodesc = NULL;

/**********************************************************************
 * Forward declarations
 **********************************************************************/
void		_PG_init(void);

static void pltcl_init_interp(pltcl_interp_desc *interp_desc, bool pltrusted);
static pltcl_interp_desc *pltcl_fetch_interp(bool pltrusted);
static void pltcl_init_load_unknown(Tcl_Interp *interp);

static Datum pltcl_handler(PG_FUNCTION_ARGS, bool pltrusted);

static Datum pltcl_func_handler(PG_FUNCTION_ARGS, bool pltrusted);

static HeapTuple pltcl_trigger_handler(PG_FUNCTION_ARGS, bool pltrusted);
static void pltcl_event_trigger_handler(PG_FUNCTION_ARGS, bool pltrusted);

static void throw_tcl_error(Tcl_Interp *interp, const char *proname);

static pltcl_proc_desc *compile_pltcl_function(Oid fn_oid, Oid tgreloid,
					   bool is_event_trigger,
					   bool pltrusted);

static int pltcl_elog(ClientData cdata, Tcl_Interp *interp,
		   int objc, Tcl_Obj *const objv[]);
static int pltcl_quote(ClientData cdata, Tcl_Interp *interp,
			int objc, Tcl_Obj *const objv[]);
static int pltcl_argisnull(ClientData cdata, Tcl_Interp *interp,
				int objc, Tcl_Obj *const objv[]);
static int pltcl_returnnull(ClientData cdata, Tcl_Interp *interp,
				 int objc, Tcl_Obj *const objv[]);

static int pltcl_SPI_execute(ClientData cdata, Tcl_Interp *interp,
				  int objc, Tcl_Obj *const objv[]);
static int pltcl_process_SPI_result(Tcl_Interp *interp,
						 const char *arrayname,
						 Tcl_Obj *loop_body,
						 int spi_rc,
						 SPITupleTable *tuptable,
						 int ntuples);
static int pltcl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				  int objc, Tcl_Obj *const objv[]);
static int pltcl_SPI_execute_plan(ClientData cdata, Tcl_Interp *interp,
					   int objc, Tcl_Obj *const objv[]);
static int pltcl_SPI_lastoid(ClientData cdata, Tcl_Interp *interp,
				  int objc, Tcl_Obj *const objv[]);

static void pltcl_set_tuple_values(Tcl_Interp *interp, const char *arrayname,
					   int tupno, HeapTuple tuple, TupleDesc tupdesc);
static Tcl_Obj *pltcl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc);


/*
 * Hack to override Tcl's builtin Notifier subsystem.  This prevents the
 * backend from becoming multithreaded, which breaks all sorts of things.
 * That happens in the default version of Tcl_InitNotifier if the TCL library
 * has been compiled with multithreading support (i.e. when TCL_THREADS is
 * defined under Unix, and in all cases under Windows).
 * It's okay to disable the notifier because we never enter the Tcl event loop
 * from Postgres, so the notifier capabilities are initialized, but never
 * used.  Only InitNotifier and DeleteFileHandler ever seem to get called
 * within Postgres, but we implement all the functions for completeness.
 */
static ClientData
pltcl_InitNotifier(void)
{
	static int	fakeThreadKey;	/* To give valid address for ClientData */

	return (ClientData) &(fakeThreadKey);
}

static void
pltcl_FinalizeNotifier(ClientData clientData)
{
}

static void
pltcl_SetTimer(CONST86 Tcl_Time *timePtr)
{
}

static void
pltcl_AlertNotifier(ClientData clientData)
{
}

static void
pltcl_CreateFileHandler(int fd, int mask,
						Tcl_FileProc *proc, ClientData clientData)
{
}

static void
pltcl_DeleteFileHandler(int fd)
{
}

static void
pltcl_ServiceModeHook(int mode)
{
}

static int
pltcl_WaitForEvent(CONST86 Tcl_Time *timePtr)
{
	return 0;
}


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
 *
 * The work done here must be safe to do in the postmaster process,
 * in case the pltcl library is preloaded in the postmaster.
 */
void
_PG_init(void)
{
	Tcl_NotifierProcs notifier;
	HASHCTL		hash_ctl;

	/* Be sure we do initialization only once (should be redundant now) */
	if (pltcl_pm_init_done)
		return;

	pg_bindtextdomain(TEXTDOMAIN);

#ifdef WIN32
	/* Required on win32 to prevent error loading init.tcl */
	Tcl_FindExecutable("");
#endif

	/*
	 * Override the functions in the Notifier subsystem.  See comments above.
	 */
	notifier.setTimerProc = pltcl_SetTimer;
	notifier.waitForEventProc = pltcl_WaitForEvent;
	notifier.createFileHandlerProc = pltcl_CreateFileHandler;
	notifier.deleteFileHandlerProc = pltcl_DeleteFileHandler;
	notifier.initNotifierProc = pltcl_InitNotifier;
	notifier.finalizeNotifierProc = pltcl_FinalizeNotifier;
	notifier.alertNotifierProc = pltcl_AlertNotifier;
	notifier.serviceModeHookProc = pltcl_ServiceModeHook;
	Tcl_SetNotifier(&notifier);

	/************************************************************
	 * Create the dummy hold interpreter to prevent close of
	 * stdout and stderr on DeleteInterp
	 ************************************************************/
	if ((pltcl_hold_interp = Tcl_CreateInterp()) == NULL)
		elog(ERROR, "could not create master Tcl interpreter");
	if (Tcl_Init(pltcl_hold_interp) == TCL_ERROR)
		elog(ERROR, "could not initialize master Tcl interpreter");

	/************************************************************
	 * Create the hash table for working interpreters
	 ************************************************************/
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(pltcl_interp_desc);
	pltcl_interp_htab = hash_create("PL/Tcl interpreters",
									8,
									&hash_ctl,
									HASH_ELEM | HASH_BLOBS);

	/************************************************************
	 * Create the hash table for function lookup
	 ************************************************************/
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(pltcl_proc_key);
	hash_ctl.entrysize = sizeof(pltcl_proc_ptr);
	pltcl_proc_htab = hash_create("PL/Tcl functions",
								  100,
								  &hash_ctl,
								  HASH_ELEM | HASH_BLOBS);

	pltcl_pm_init_done = true;
}

/**********************************************************************
 * pltcl_init_interp() - initialize a new Tcl interpreter
 **********************************************************************/
static void
pltcl_init_interp(pltcl_interp_desc *interp_desc, bool pltrusted)
{
	Tcl_Interp *interp;
	char		interpname[32];

	/************************************************************
	 * Create the Tcl interpreter as a slave of pltcl_hold_interp.
	 * Note: Tcl automatically does Tcl_Init in the untrusted case,
	 * and it's not wanted in the trusted case.
	 ************************************************************/
	snprintf(interpname, sizeof(interpname), "slave_%u", interp_desc->user_id);
	if ((interp = Tcl_CreateSlave(pltcl_hold_interp, interpname,
								  pltrusted ? 1 : 0)) == NULL)
		elog(ERROR, "could not create slave Tcl interpreter");
	interp_desc->interp = interp;

	/************************************************************
	 * Initialize the query hash table associated with interpreter
	 ************************************************************/
	Tcl_InitHashTable(&interp_desc->query_hash, TCL_STRING_KEYS);

	/************************************************************
	 * Install the commands for SPI support in the interpreter
	 ************************************************************/
	Tcl_CreateObjCommand(interp, "elog",
						 pltcl_elog, NULL, NULL);
	Tcl_CreateObjCommand(interp, "quote",
						 pltcl_quote, NULL, NULL);
	Tcl_CreateObjCommand(interp, "argisnull",
						 pltcl_argisnull, NULL, NULL);
	Tcl_CreateObjCommand(interp, "return_null",
						 pltcl_returnnull, NULL, NULL);

	Tcl_CreateObjCommand(interp, "spi_exec",
						 pltcl_SPI_execute, NULL, NULL);
	Tcl_CreateObjCommand(interp, "spi_prepare",
						 pltcl_SPI_prepare, NULL, NULL);
	Tcl_CreateObjCommand(interp, "spi_execp",
						 pltcl_SPI_execute_plan, NULL, NULL);
	Tcl_CreateObjCommand(interp, "spi_lastoid",
						 pltcl_SPI_lastoid, NULL, NULL);

	/************************************************************
	 * Try to load the unknown procedure from pltcl_modules
	 ************************************************************/
	pltcl_init_load_unknown(interp);
}

/**********************************************************************
 * pltcl_fetch_interp() - fetch the Tcl interpreter to use for a function
 *
 * This also takes care of any on-first-use initialization required.
 * Note: we assume caller has already connected to SPI.
 **********************************************************************/
static pltcl_interp_desc *
pltcl_fetch_interp(bool pltrusted)
{
	Oid			user_id;
	pltcl_interp_desc *interp_desc;
	bool		found;

	/* Find or create the interpreter hashtable entry for this userid */
	if (pltrusted)
		user_id = GetUserId();
	else
		user_id = InvalidOid;

	interp_desc = hash_search(pltcl_interp_htab, &user_id,
							  HASH_ENTER,
							  &found);
	if (!found)
		pltcl_init_interp(interp_desc, pltrusted);

	return interp_desc;
}

/**********************************************************************
 * pltcl_init_load_unknown()	- Load the unknown procedure from
 *				  table pltcl_modules (if it exists)
 **********************************************************************/
static void
pltcl_init_load_unknown(Tcl_Interp *interp)
{
	Relation	pmrel;
	char	   *pmrelname,
			   *nspname;
	char	   *buf;
	int			buflen;
	int			spi_rc;
	int			tcl_rc;
	Tcl_DString unknown_src;
	char	   *part;
	int			i;
	int			fno;

	/************************************************************
	 * Check if table pltcl_modules exists
	 *
	 * We allow the table to be found anywhere in the search_path.
	 * This is for backwards compatibility.  To ensure that the table
	 * is trustworthy, we require it to be owned by a superuser.
	 ************************************************************/
	pmrel = relation_openrv_extended(makeRangeVar(NULL, "pltcl_modules", -1),
									 AccessShareLock, true);
	if (pmrel == NULL)
		return;
	/* sanity-check the relation kind */
	if (!(pmrel->rd_rel->relkind == RELKIND_RELATION ||
		  pmrel->rd_rel->relkind == RELKIND_MATVIEW ||
		  pmrel->rd_rel->relkind == RELKIND_VIEW))
	{
		relation_close(pmrel, AccessShareLock);
		return;
	}
	/* must be owned by superuser, else ignore */
	if (!superuser_arg(pmrel->rd_rel->relowner))
	{
		relation_close(pmrel, AccessShareLock);
		return;
	}
	/* get fully qualified table name for use in select command */
	nspname = get_namespace_name(RelationGetNamespace(pmrel));
	if (!nspname)
		elog(ERROR, "cache lookup failed for namespace %u",
			 RelationGetNamespace(pmrel));
	pmrelname = quote_qualified_identifier(nspname,
										   RelationGetRelationName(pmrel));

	/************************************************************
	 * Read all the rows from it where modname = 'unknown',
	 * in the order of modseq
	 ************************************************************/
	buflen = strlen(pmrelname) + 100;
	buf = (char *) palloc(buflen);
	snprintf(buf, buflen,
		   "select modsrc from %s where modname = 'unknown' order by modseq",
			 pmrelname);

	spi_rc = SPI_execute(buf, false, 0);
	if (spi_rc != SPI_OK_SELECT)
		elog(ERROR, "select from pltcl_modules failed");

	pfree(buf);

	/************************************************************
	 * If there's nothing, module unknown doesn't exist
	 ************************************************************/
	if (SPI_processed == 0)
	{
		SPI_freetuptable(SPI_tuptable);
		ereport(WARNING,
				(errmsg("module \"unknown\" not found in pltcl_modules")));
		relation_close(pmrel, AccessShareLock);
		return;
	}

	/************************************************************
	 * There is a module named unknown. Reassemble the
	 * source from the modsrc attributes and evaluate
	 * it in the Tcl interpreter
	 *
	 * leave this code as DString - it's only executed once per session
	 ************************************************************/
	fno = SPI_fnumber(SPI_tuptable->tupdesc, "modsrc");

	Tcl_DStringInit(&unknown_src);

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
	tcl_rc = Tcl_EvalEx(interp, Tcl_DStringValue(&unknown_src),
						Tcl_DStringLength(&unknown_src),
						TCL_EVAL_GLOBAL);

	Tcl_DStringFree(&unknown_src);
	SPI_freetuptable(SPI_tuptable);

	if (tcl_rc != TCL_OK)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not load module \"unknown\": %s",
						utf_u2e(Tcl_GetStringResult(interp)))));

	relation_close(pmrel, AccessShareLock);
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
	return pltcl_handler(fcinfo, true);
}

/*
 * Alternative handler for unsafe functions
 */
PG_FUNCTION_INFO_V1(pltclu_call_handler);

/* keep non-static */
Datum
pltclu_call_handler(PG_FUNCTION_ARGS)
{
	return pltcl_handler(fcinfo, false);
}


static Datum
pltcl_handler(PG_FUNCTION_ARGS, bool pltrusted)
{
	Datum		retval;
	FunctionCallInfo save_fcinfo;
	pltcl_proc_desc *save_prodesc;

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
			retval = PointerGetDatum(pltcl_trigger_handler(fcinfo, pltrusted));
		}
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		{
			pltcl_current_fcinfo = NULL;
			pltcl_event_trigger_handler(fcinfo, pltrusted);
			retval = (Datum) 0;
		}
		else
		{
			pltcl_current_fcinfo = fcinfo;
			retval = pltcl_func_handler(fcinfo, pltrusted);
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


/**********************************************************************
 * pltcl_func_handler()		- Handler for regular function calls
 **********************************************************************/
static Datum
pltcl_func_handler(PG_FUNCTION_ARGS, bool pltrusted)
{
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	Tcl_Obj    *tcl_cmd;
	int			i;
	int			tcl_rc;
	Datum		retval;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid, InvalidOid,
									 false, pltrusted);

	pltcl_current_prodesc = prodesc;

	interp = prodesc->interp_desc->interp;

	/************************************************************
	 * Create the tcl command to call the internal
	 * proc in the Tcl interpreter
	 ************************************************************/
	tcl_cmd = Tcl_NewObj();
	Tcl_ListObjAppendElement(NULL, tcl_cmd,
							 Tcl_NewStringObj(prodesc->internal_proname, -1));

	/* We hold a refcount on tcl_cmd just to be sure it stays around */
	Tcl_IncrRefCount(tcl_cmd);

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
					Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());
				else
				{
					HeapTupleHeader td;
					Oid			tupType;
					int32		tupTypmod;
					TupleDesc	tupdesc;
					HeapTupleData tmptup;
					Tcl_Obj    *list_tmp;

					td = DatumGetHeapTupleHeader(fcinfo->arg[i]);
					/* Extract rowtype info and find a tupdesc */
					tupType = HeapTupleHeaderGetTypeId(td);
					tupTypmod = HeapTupleHeaderGetTypMod(td);
					tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
					/* Build a temporary HeapTuple control structure */
					tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
					tmptup.t_data = td;

					list_tmp = pltcl_build_tuple_argument(&tmptup, tupdesc);
					Tcl_ListObjAppendElement(NULL, tcl_cmd, list_tmp);

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
					Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());
				else
				{
					char	   *tmp;

					tmp = OutputFunctionCall(&prodesc->arg_out_func[i],
											 fcinfo->arg[i]);
					UTF_BEGIN;
					Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj(UTF_E2U(tmp), -1));
					UTF_END;
					pfree(tmp);
				}
			}
		}
	}
	PG_CATCH();
	{
		/* Release refcount to free tcl_cmd */
		Tcl_DecrRefCount(tcl_cmd);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/************************************************************
	 * Call the Tcl function
	 *
	 * We assume no PG error can be thrown directly from this call.
	 ************************************************************/
	tcl_rc = Tcl_EvalObjEx(interp, tcl_cmd, (TCL_EVAL_DIRECT | TCL_EVAL_GLOBAL));

	/* Release refcount to free tcl_cmd (and all subsidiary objects) */
	Tcl_DecrRefCount(tcl_cmd);

	/************************************************************
	 * Check for errors reported by Tcl.
	 ************************************************************/
	if (tcl_rc != TCL_OK)
		throw_tcl_error(interp, prodesc->user_proname);

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
		retval = InputFunctionCall(&prodesc->result_in_func,
								   utf_u2e(Tcl_GetStringResult(interp)),
								   prodesc->result_typioparam,
								   -1);

	return retval;
}


/**********************************************************************
 * pltcl_trigger_handler()	- Handler for trigger calls
 **********************************************************************/
static HeapTuple
pltcl_trigger_handler(PG_FUNCTION_ARGS, bool pltrusted)
{
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	char	   *stroid;
	TupleDesc	tupdesc;
	volatile HeapTuple rettup;
	Tcl_Obj    *tcl_cmd;
	Tcl_Obj    *tcl_trigtup;
	Tcl_Obj    *tcl_newtup;
	int			tcl_rc;
	int			i;
	int		   *modattrs;
	Datum	   *modvalues;
	char	   *modnulls;
	int			ret_numvals;
	const char *result;
	const char **ret_values;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid,
									 RelationGetRelid(trigdata->tg_relation),
									 false,		/* not an event trigger */
									 pltrusted);

	pltcl_current_prodesc = prodesc;

	interp = prodesc->interp_desc->interp;

	tupdesc = trigdata->tg_relation->rd_att;

	/************************************************************
	 * Create the tcl command to call the internal
	 * proc in the interpreter
	 ************************************************************/
	tcl_cmd = Tcl_NewObj();
	Tcl_IncrRefCount(tcl_cmd);

	PG_TRY();
	{
		/* The procedure name (note this is all ASCII, so no utf_e2u) */
		Tcl_ListObjAppendElement(NULL, tcl_cmd,
							Tcl_NewStringObj(prodesc->internal_proname, -1));

		/* The trigger name for argument TG_name */
		Tcl_ListObjAppendElement(NULL, tcl_cmd,
				Tcl_NewStringObj(utf_e2u(trigdata->tg_trigger->tgname), -1));

		/* The oid of the trigger relation for argument TG_relid */
		/* Consider not converting to a string for more performance? */
		stroid = DatumGetCString(DirectFunctionCall1(oidout,
							ObjectIdGetDatum(trigdata->tg_relation->rd_id)));
		Tcl_ListObjAppendElement(NULL, tcl_cmd,
								 Tcl_NewStringObj(stroid, -1));
		pfree(stroid);

		/* The name of the table the trigger is acting on: TG_table_name */
		stroid = SPI_getrelname(trigdata->tg_relation);
		Tcl_ListObjAppendElement(NULL, tcl_cmd,
								 Tcl_NewStringObj(utf_e2u(stroid), -1));
		pfree(stroid);

		/* The schema of the table the trigger is acting on: TG_table_schema */
		stroid = SPI_getnspname(trigdata->tg_relation);
		Tcl_ListObjAppendElement(NULL, tcl_cmd,
								 Tcl_NewStringObj(utf_e2u(stroid), -1));
		pfree(stroid);

		/* A list of attribute names for argument TG_relatts */
		tcl_trigtup = Tcl_NewObj();
		Tcl_ListObjAppendElement(NULL, tcl_trigtup, Tcl_NewObj());
		for (i = 0; i < tupdesc->natts; i++)
		{
			if (tupdesc->attrs[i]->attisdropped)
				Tcl_ListObjAppendElement(NULL, tcl_trigtup, Tcl_NewObj());
			else
				Tcl_ListObjAppendElement(NULL, tcl_trigtup,
										 Tcl_NewStringObj(utf_e2u(NameStr(tupdesc->attrs[i]->attname)), -1));
		}
		Tcl_ListObjAppendElement(NULL, tcl_cmd, tcl_trigtup);

		/* The when part of the event for TG_when */
		if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
			Tcl_ListObjAppendElement(NULL, tcl_cmd,
									 Tcl_NewStringObj("BEFORE", -1));
		else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
			Tcl_ListObjAppendElement(NULL, tcl_cmd,
									 Tcl_NewStringObj("AFTER", -1));
		else if (TRIGGER_FIRED_INSTEAD(trigdata->tg_event))
			Tcl_ListObjAppendElement(NULL, tcl_cmd,
									 Tcl_NewStringObj("INSTEAD OF", -1));
		else
			elog(ERROR, "unrecognized WHEN tg_event: %u", trigdata->tg_event);

		/* The level part of the event for TG_level */
		if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		{
			Tcl_ListObjAppendElement(NULL, tcl_cmd,
									 Tcl_NewStringObj("ROW", -1));

			/* Build the data list for the trigtuple */
			tcl_trigtup = pltcl_build_tuple_argument(trigdata->tg_trigtuple,
													 tupdesc);

			/*
			 * Now the command part of the event for TG_op and data for NEW
			 * and OLD
			 */
			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			{
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("INSERT", -1));

				Tcl_ListObjAppendElement(NULL, tcl_cmd, tcl_trigtup);
				Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());

				rettup = trigdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			{
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("DELETE", -1));

				Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());
				Tcl_ListObjAppendElement(NULL, tcl_cmd, tcl_trigtup);

				rettup = trigdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			{
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("UPDATE", -1));

				tcl_newtup = pltcl_build_tuple_argument(trigdata->tg_newtuple,
														tupdesc);

				Tcl_ListObjAppendElement(NULL, tcl_cmd, tcl_newtup);
				Tcl_ListObjAppendElement(NULL, tcl_cmd, tcl_trigtup);

				rettup = trigdata->tg_newtuple;
			}
			else
				elog(ERROR, "unrecognized OP tg_event: %u", trigdata->tg_event);
		}
		else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		{
			Tcl_ListObjAppendElement(NULL, tcl_cmd,
									 Tcl_NewStringObj("STATEMENT", -1));

			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("INSERT", -1));
			else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("DELETE", -1));
			else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("UPDATE", -1));
			else if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("TRUNCATE", -1));
			else
				elog(ERROR, "unrecognized OP tg_event: %u", trigdata->tg_event);

			Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());
			Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());

			rettup = (HeapTuple) NULL;
		}
		else
			elog(ERROR, "unrecognized LEVEL tg_event: %u", trigdata->tg_event);

		/* Finally append the arguments from CREATE TRIGGER */
		for (i = 0; i < trigdata->tg_trigger->tgnargs; i++)
			Tcl_ListObjAppendElement(NULL, tcl_cmd,
			 Tcl_NewStringObj(utf_e2u(trigdata->tg_trigger->tgargs[i]), -1));

	}
	PG_CATCH();
	{
		Tcl_DecrRefCount(tcl_cmd);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/************************************************************
	 * Call the Tcl function
	 *
	 * We assume no PG error can be thrown directly from this call.
	 ************************************************************/
	tcl_rc = Tcl_EvalObjEx(interp, tcl_cmd, (TCL_EVAL_DIRECT | TCL_EVAL_GLOBAL));

	/* Release refcount to free tcl_cmd (and all subsidiary objects) */
	Tcl_DecrRefCount(tcl_cmd);

	/************************************************************
	 * Check for errors reported by Tcl.
	 ************************************************************/
	if (tcl_rc != TCL_OK)
		throw_tcl_error(interp, prodesc->user_proname);

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
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("could not split return value from trigger: %s",
						utf_u2e(Tcl_GetStringResult(interp)))));

	/* Use a TRY to ensure ret_values will get freed */
	PG_TRY();
	{
		if (ret_numvals % 2 != 0)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("trigger's return list must have even number of elements")));

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
			char	   *ret_name = utf_u2e(ret_values[i]);
			char	   *ret_value = utf_u2e(ret_values[i + 1]);
			int			attnum;
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
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("unrecognized attribute \"%s\"",
								ret_name)));
			if (attnum <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set system attribute \"%s\"",
								ret_name)));

			/************************************************************
			 * Ignore dropped columns
			 ************************************************************/
			if (tupdesc->attrs[attnum - 1]->attisdropped)
				continue;

			/************************************************************
			 * Lookup the attribute type in the syscache
			 * for the input function
			 ************************************************************/
			getTypeInputInfo(tupdesc->attrs[attnum - 1]->atttypid,
							 &typinput, &typioparam);
			fmgr_info(typinput, &finfo);

			/************************************************************
			 * Set the attribute to NOT NULL and convert the contents
			 ************************************************************/
			modvalues[attnum - 1] = InputFunctionCall(&finfo,
													  ret_value,
													  typioparam,
									  tupdesc->attrs[attnum - 1]->atttypmod);
			modnulls[attnum - 1] = ' ';
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
 * pltcl_event_trigger_handler()	- Handler for event trigger calls
 **********************************************************************/
static void
pltcl_event_trigger_handler(PG_FUNCTION_ARGS, bool pltrusted)
{
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	EventTriggerData *tdata = (EventTriggerData *) fcinfo->context;
	Tcl_Obj    *tcl_cmd;
	int			tcl_rc;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid,
									 InvalidOid, true, pltrusted);

	pltcl_current_prodesc = prodesc;

	interp = prodesc->interp_desc->interp;

	/* Create the tcl command and call the internal proc */
	tcl_cmd = Tcl_NewObj();
	Tcl_IncrRefCount(tcl_cmd);
	Tcl_ListObjAppendElement(NULL, tcl_cmd,
							 Tcl_NewStringObj(prodesc->internal_proname, -1));
	Tcl_ListObjAppendElement(NULL, tcl_cmd,
							 Tcl_NewStringObj(utf_e2u(tdata->event), -1));
	Tcl_ListObjAppendElement(NULL, tcl_cmd,
							 Tcl_NewStringObj(utf_e2u(tdata->tag), -1));

	tcl_rc = Tcl_EvalObjEx(interp, tcl_cmd, (TCL_EVAL_DIRECT | TCL_EVAL_GLOBAL));

	/* Release refcount to free tcl_cmd (and all subsidiary objects) */
	Tcl_DecrRefCount(tcl_cmd);

	/* Check for errors reported by Tcl. */
	if (tcl_rc != TCL_OK)
		throw_tcl_error(interp, prodesc->user_proname);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");
}


/**********************************************************************
 * throw_tcl_error	- ereport an error returned from the Tcl interpreter
 **********************************************************************/
static void
throw_tcl_error(Tcl_Interp *interp, const char *proname)
{
	/*
	 * Caution is needed here because Tcl_GetVar could overwrite the
	 * interpreter result (even though it's not really supposed to), and we
	 * can't control the order of evaluation of ereport arguments. Hence, make
	 * real sure we have our own copy of the result string before invoking
	 * Tcl_GetVar.
	 */
	char	   *emsg;
	char	   *econtext;

	emsg = pstrdup(utf_u2e(Tcl_GetStringResult(interp)));
	econtext = utf_u2e(Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY));
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("%s", emsg),
			 errcontext("%s\nin PL/Tcl function \"%s\"",
						econtext, proname)));
}


/**********************************************************************
 * compile_pltcl_function	- compile (or hopefully just look up) function
 *
 * tgreloid is the OID of the relation when compiling a trigger, or zero
 * (InvalidOid) when compiling a plain function.
 **********************************************************************/
static pltcl_proc_desc *
compile_pltcl_function(Oid fn_oid, Oid tgreloid,
					   bool is_event_trigger, bool pltrusted)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	pltcl_proc_key proc_key;
	pltcl_proc_ptr *proc_ptr;
	bool		found;
	pltcl_proc_desc *prodesc;

	/* We'll need the pg_proc tuple in any case... */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/* Try to find function in pltcl_proc_htab */
	proc_key.proc_id = fn_oid;
	proc_key.is_trigger = OidIsValid(tgreloid);
	proc_key.user_id = pltrusted ? GetUserId() : InvalidOid;

	proc_ptr = hash_search(pltcl_proc_htab, &proc_key,
						   HASH_ENTER,
						   &found);
	if (!found)
		proc_ptr->proc_ptr = NULL;

	prodesc = proc_ptr->proc_ptr;

	/************************************************************
	 * If it's present, must check whether it's still up to date.
	 * This is needed because CREATE OR REPLACE FUNCTION can modify the
	 * function's pg_proc entry without changing its OID.
	 ************************************************************/
	if (prodesc != NULL)
	{
		bool		uptodate;

		uptodate = (prodesc->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
					ItemPointerEquals(&prodesc->fn_tid, &procTup->t_self));

		if (!uptodate)
		{
			proc_ptr->proc_ptr = NULL;
			prodesc = NULL;
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
	if (prodesc == NULL)
	{
		bool		is_trigger = OidIsValid(tgreloid);
		char		internal_proname[128];
		HeapTuple	typeTup;
		Form_pg_type typeStruct;
		Tcl_DString proc_internal_def;
		Tcl_DString proc_internal_body;
		char		proc_internal_args[33 * FUNC_MAX_ARGS];
		Datum		prosrcdatum;
		bool		isnull;
		char	   *proc_source;
		char		buf[32];
		Tcl_Interp *interp;
		int			i;
		int			tcl_rc;

		/************************************************************
		 * Build our internal proc name from the function's Oid.  Append
		 * "_trigger" when appropriate to ensure the normal and trigger
		 * cases are kept separate.  Note name must be all-ASCII.
		 ************************************************************/
		if (!is_trigger && !is_event_trigger)
			snprintf(internal_proname, sizeof(internal_proname),
					 "__PLTcl_proc_%u", fn_oid);
		else if (is_event_trigger)
			snprintf(internal_proname, sizeof(internal_proname),
					 "__PLTcl_proc_%u_evttrigger", fn_oid);
		else if (is_trigger)
			snprintf(internal_proname, sizeof(internal_proname),
					 "__PLTcl_proc_%u_trigger", fn_oid);

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (pltcl_proc_desc *) malloc(sizeof(pltcl_proc_desc));
		if (prodesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		MemSet(prodesc, 0, sizeof(pltcl_proc_desc));
		prodesc->user_proname = strdup(NameStr(procStruct->proname));
		prodesc->internal_proname = strdup(internal_proname);
		if (prodesc->user_proname == NULL || prodesc->internal_proname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		prodesc->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
		prodesc->fn_tid = procTup->t_self;

		/* Remember if function is STABLE/IMMUTABLE */
		prodesc->fn_readonly =
			(procStruct->provolatile != PROVOLATILE_VOLATILE);
		/* And whether it is trusted */
		prodesc->lanpltrusted = pltrusted;

		/************************************************************
		 * Identify the interpreter to use for the function
		 ************************************************************/
		prodesc->interp_desc = pltcl_fetch_interp(prodesc->lanpltrusted);
		interp = prodesc->interp_desc->interp;

		/************************************************************
		 * Get the required information for input conversion of the
		 * return value.
		 ************************************************************/
		if (!is_trigger && !is_event_trigger)
		{
			typeTup =
				SearchSysCache1(TYPEOID,
								ObjectIdGetDatum(procStruct->prorettype));
			if (!HeapTupleIsValid(typeTup))
			{
				free(prodesc->user_proname);
				free(prodesc->internal_proname);
				free(prodesc);
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			}
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID */
			if (typeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (procStruct->prorettype == VOIDOID)
					 /* okay */ ;
				else if (procStruct->prorettype == TRIGGEROID ||
						 procStruct->prorettype == EVTTRIGGEROID)
				{
					free(prodesc->user_proname);
					free(prodesc->internal_proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				}
				else
				{
					free(prodesc->user_proname);
					free(prodesc->internal_proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Tcl functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}

			if (typeStruct->typtype == TYPTYPE_COMPOSITE)
			{
				free(prodesc->user_proname);
				free(prodesc->internal_proname);
				free(prodesc);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				  errmsg("PL/Tcl functions cannot return composite types")));
			}

			perm_fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
			prodesc->result_typioparam = getTypeIOParam(typeTup);

			ReleaseSysCache(typeTup);
		}

		/************************************************************
		 * Get the required information for output conversion
		 * of all procedure arguments
		 ************************************************************/
		if (!is_trigger && !is_event_trigger)
		{
			prodesc->nargs = procStruct->pronargs;
			proc_internal_args[0] = '\0';
			for (i = 0; i < prodesc->nargs; i++)
			{
				typeTup = SearchSysCache1(TYPEOID,
						ObjectIdGetDatum(procStruct->proargtypes.values[i]));
				if (!HeapTupleIsValid(typeTup))
				{
					free(prodesc->user_proname);
					free(prodesc->internal_proname);
					free(prodesc);
					elog(ERROR, "cache lookup failed for type %u",
						 procStruct->proargtypes.values[i]);
				}
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* Disallow pseudotype argument */
				if (typeStruct->typtype == TYPTYPE_PSEUDO)
				{
					free(prodesc->user_proname);
					free(prodesc->internal_proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Tcl functions cannot accept type %s",
						format_type_be(procStruct->proargtypes.values[i]))));
				}

				if (typeStruct->typtype == TYPTYPE_COMPOSITE)
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
		else if (is_trigger)
		{
			/* trigger procedure has fixed args */
			strcpy(proc_internal_args,
				   "TG_name TG_relid TG_table_name TG_table_schema TG_relatts TG_when TG_level TG_op __PLTcl_Tup_NEW __PLTcl_Tup_OLD args");
		}
		else if (is_event_trigger)
		{
			/* event trigger procedure has fixed args */
			strcpy(proc_internal_args, "TG_event TG_tag");
		}

		/************************************************************
		 * Create the tcl command to define the internal
		 * procedure
		 *
		 * leave this code as DString - it's a text processing function
		 * that only gets invoked when the tcl function is invoked
		 * for the first time
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
		if (is_trigger)
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
		else if (is_event_trigger)
		{
			/* no argument support for event triggers */
		}
		else
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

		/************************************************************
		 * Add user's function definition to proc body
		 ************************************************************/
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
		proc_source = TextDatumGetCString(prosrcdatum);
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
		tcl_rc = Tcl_EvalEx(interp,
							Tcl_DStringValue(&proc_internal_def),
							Tcl_DStringLength(&proc_internal_def),
							TCL_EVAL_GLOBAL);
		Tcl_DStringFree(&proc_internal_def);
		if (tcl_rc != TCL_OK)
		{
			free(prodesc->user_proname);
			free(prodesc->internal_proname);
			free(prodesc);
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("could not create internal procedure \"%s\": %s",
							internal_proname,
							utf_u2e(Tcl_GetStringResult(interp)))));
		}

		/************************************************************
		 * Add the proc description block to the hashtable.  Note we do not
		 * attempt to free any previously existing prodesc block.  This is
		 * annoying, but necessary since there could be active calls using
		 * the old prodesc.
		 ************************************************************/
		proc_ptr->proc_ptr = prodesc;
	}

	ReleaseSysCache(procTup);

	return prodesc;
}


/**********************************************************************
 * pltcl_elog()		- elog() support for PLTcl
 **********************************************************************/
static int
pltcl_elog(ClientData cdata, Tcl_Interp *interp,
		   int objc, Tcl_Obj *const objv[])
{
	volatile int level;
	MemoryContext oldcontext;
	int			priIndex;

	static const char *logpriorities[] = {
		"DEBUG", "LOG", "INFO", "NOTICE",
		"WARNING", "ERROR", "FATAL", (const char *) NULL
	};

	static const int loglevels[] = {
		DEBUG2, LOG, INFO, NOTICE,
		WARNING, ERROR, FATAL
	};

	if (objc != 3)
	{
		Tcl_WrongNumArgs(interp, 1, objv, "level msg");
		return TCL_ERROR;
	}

	if (Tcl_GetIndexFromObj(interp, objv[1], logpriorities, "priority",
							TCL_EXACT, &priIndex) != TCL_OK)
		return TCL_ERROR;

	level = loglevels[priIndex];

	if (level == ERROR)
	{
		/*
		 * We just pass the error back to Tcl.  If it's not caught, it'll
		 * eventually get converted to a PG error when we reach the call
		 * handler.
		 */
		Tcl_SetObjResult(interp, objv[2]);
		return TCL_ERROR;
	}

	/*
	 * For non-error messages, just pass 'em to ereport().  We do not expect
	 * that this will fail, but just on the off chance it does, report the
	 * error back to Tcl.  Note we are assuming that ereport() can't have any
	 * internal failures that are so bad as to require a transaction abort.
	 *
	 * This path is also used for FATAL errors, which aren't going to come
	 * back to us at all.
	 */
	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		UTF_BEGIN;
		ereport(level,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", UTF_U2E(Tcl_GetString(objv[2])))));
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
		Tcl_SetObjResult(interp, Tcl_NewStringObj(UTF_E2U(edata->message), -1));
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
			int objc, Tcl_Obj *const objv[])
{
	char	   *tmp;
	const char *cp1;
	char	   *cp2;
	int			length;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (objc != 2)
	{
		Tcl_WrongNumArgs(interp, 1, objv, "string");
		return TCL_ERROR;
	}

	/************************************************************
	 * Allocate space for the maximum the string can
	 * grow to and initialize pointers
	 ************************************************************/
	cp1 = Tcl_GetStringFromObj(objv[1], &length);
	tmp = palloc(length * 2 + 1);
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
	Tcl_SetObjResult(interp, Tcl_NewStringObj(tmp, -1));
	pfree(tmp);
	return TCL_OK;
}


/**********************************************************************
 * pltcl_argisnull()	- determine if a specific argument is NULL
 **********************************************************************/
static int
pltcl_argisnull(ClientData cdata, Tcl_Interp *interp,
				int objc, Tcl_Obj *const objv[])
{
	int			argno;
	FunctionCallInfo fcinfo = pltcl_current_fcinfo;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (objc != 2)
	{
		Tcl_WrongNumArgs(interp, 1, objv, "argno");
		return TCL_ERROR;
	}

	/************************************************************
	 * Check that we're called as a normal function
	 ************************************************************/
	if (fcinfo == NULL)
	{
		Tcl_SetObjResult(interp,
			   Tcl_NewStringObj("argisnull cannot be used in triggers", -1));
		return TCL_ERROR;
	}

	/************************************************************
	 * Get the argument number
	 ************************************************************/
	if (Tcl_GetIntFromObj(interp, objv[1], &argno) != TCL_OK)
		return TCL_ERROR;

	/************************************************************
	 * Check that the argno is valid
	 ************************************************************/
	argno--;
	if (argno < 0 || argno >= fcinfo->nargs)
	{
		Tcl_SetObjResult(interp,
						 Tcl_NewStringObj("argno out of range", -1));
		return TCL_ERROR;
	}

	/************************************************************
	 * Get the requested NULL state
	 ************************************************************/
	Tcl_SetObjResult(interp, Tcl_NewBooleanObj(PG_ARGISNULL(argno)));
	return TCL_OK;
}


/**********************************************************************
 * pltcl_returnnull()	- Cause a NULL return from the current function
 **********************************************************************/
static int
pltcl_returnnull(ClientData cdata, Tcl_Interp *interp,
				 int objc, Tcl_Obj *const objv[])
{
	FunctionCallInfo fcinfo = pltcl_current_fcinfo;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (objc != 1)
	{
		Tcl_WrongNumArgs(interp, 1, objv, "");
		return TCL_ERROR;
	}

	/************************************************************
	 * Check that we're called as a normal function
	 ************************************************************/
	if (fcinfo == NULL)
	{
		Tcl_SetObjResult(interp,
			 Tcl_NewStringObj("return_null cannot be used in triggers", -1));
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
				  int objc, Tcl_Obj *const objv[])
{
	int			my_rc;
	int			spi_rc;
	int			query_idx;
	int			i;
	int			optIndex;
	int			count = 0;
	const char *volatile arrayname = NULL;
	Tcl_Obj    *volatile loop_body = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	enum options
	{
		OPT_ARRAY, OPT_COUNT
	};

	static const char *options[] = {
		"-array", "-count", (const char *) NULL
	};

	/************************************************************
	 * Check the call syntax and get the options
	 ************************************************************/
	if (objc < 2)
	{
		Tcl_WrongNumArgs(interp, 1, objv,
						 "?-count n? ?-array name? query ?loop body?");
		return TCL_ERROR;
	}

	i = 1;
	while (i < objc)
	{
		if (Tcl_GetIndexFromObj(interp, objv[i], options, "option",
								TCL_EXACT, &optIndex) != TCL_OK)
			break;

		if (++i >= objc)
		{
			Tcl_SetObjResult(interp,
			   Tcl_NewStringObj("missing argument to -count or -array", -1));
			return TCL_ERROR;
		}

		switch ((enum options) optIndex)
		{
			case OPT_ARRAY:
				arrayname = Tcl_GetString(objv[i++]);
				break;

			case OPT_COUNT:
				if (Tcl_GetIntFromObj(interp, objv[i++], &count) != TCL_OK)
					return TCL_ERROR;
				break;
		}
	}

	query_idx = i;
	if (query_idx >= objc || query_idx + 2 < objc)
	{
		Tcl_WrongNumArgs(interp, query_idx - 1, objv, "query ?loop body?");
		return TCL_ERROR;
	}

	if (query_idx + 1 < objc)
		loop_body = objv[query_idx + 1];

	/************************************************************
	 * Execute the query inside a sub-transaction, so we can cope with
	 * errors sanely
	 ************************************************************/

	pltcl_subtrans_begin(oldcontext, oldowner);

	PG_TRY();
	{
		UTF_BEGIN;
		spi_rc = SPI_execute(UTF_U2E(Tcl_GetString(objv[query_idx])),
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
						 const char *arrayname,
						 Tcl_Obj *loop_body,
						 int spi_rc,
						 SPITupleTable *tuptable,
						 int ntuples)
{
	int			my_rc = TCL_OK;
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
			Tcl_SetObjResult(interp, Tcl_NewIntObj(ntuples));
			break;

		case SPI_OK_UTILITY:
		case SPI_OK_REWRITTEN:
			if (tuptable == NULL)
			{
				Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
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

					loop_rc = Tcl_EvalObjEx(interp, loop_body, 0);

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
				Tcl_SetObjResult(interp, Tcl_NewIntObj(ntuples));
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
 *				  SPI_keepplan and returns a key for
 *				  access. There is no chance to prepare
 *				  and not save the plan currently.
 **********************************************************************/
static int
pltcl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				  int objc, Tcl_Obj *const objv[])
{
	volatile MemoryContext plan_cxt = NULL;
	int			nargs;
	Tcl_Obj   **argsObj;
	pltcl_query_desc *qdesc;
	int			i;
	Tcl_HashEntry *hashent;
	int			hashnew;
	Tcl_HashTable *query_hash;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	/************************************************************
	 * Check the call syntax
	 ************************************************************/
	if (objc != 3)
	{
		Tcl_WrongNumArgs(interp, 1, objv, "query argtypes");
		return TCL_ERROR;
	}

	/************************************************************
	 * Split the argument type list
	 ************************************************************/
	if (Tcl_ListObjGetElements(interp, objv[2], &nargs, &argsObj) != TCL_OK)
		return TCL_ERROR;

	/************************************************************
	 * Allocate the new querydesc structure
	 *
	 * struct qdesc and subsidiary data all live in plan_cxt.  Note that if the
	 * function is recompiled for whatever reason, permanent memory leaks
	 * occur.  FIXME someday.
	 ************************************************************/
	plan_cxt = AllocSetContextCreate(TopMemoryContext,
									 "PL/TCL spi_prepare query",
									 ALLOCSET_SMALL_MINSIZE,
									 ALLOCSET_SMALL_INITSIZE,
									 ALLOCSET_SMALL_MAXSIZE);
	MemoryContextSwitchTo(plan_cxt);
	qdesc = (pltcl_query_desc *) palloc0(sizeof(pltcl_query_desc));
	snprintf(qdesc->qname, sizeof(qdesc->qname), "%p", qdesc);
	qdesc->nargs = nargs;
	qdesc->argtypes = (Oid *) palloc(nargs * sizeof(Oid));
	qdesc->arginfuncs = (FmgrInfo *) palloc(nargs * sizeof(FmgrInfo));
	qdesc->argtypioparams = (Oid *) palloc(nargs * sizeof(Oid));
	MemoryContextSwitchTo(oldcontext);

	/************************************************************
	 * Execute the prepare inside a sub-transaction, so we can cope with
	 * errors sanely
	 ************************************************************/

	pltcl_subtrans_begin(oldcontext, oldowner);

	PG_TRY();
	{
		/************************************************************
		 * Resolve argument type names and then look them up by oid
		 * in the system cache, and remember the required information
		 * for input conversion.
		 ************************************************************/
		for (i = 0; i < nargs; i++)
		{
			Oid			typId,
						typInput,
						typIOParam;
			int32		typmod;

			parseTypeString(Tcl_GetString(argsObj[i]), &typId, &typmod, false);

			getTypeInputInfo(typId, &typInput, &typIOParam);

			qdesc->argtypes[i] = typId;
			fmgr_info_cxt(typInput, &(qdesc->arginfuncs[i]), plan_cxt);
			qdesc->argtypioparams[i] = typIOParam;
		}

		/************************************************************
		 * Prepare the plan and check for errors
		 ************************************************************/
		UTF_BEGIN;
		qdesc->plan = SPI_prepare(UTF_U2E(Tcl_GetString(objv[1])),
								  nargs, qdesc->argtypes);
		UTF_END;

		if (qdesc->plan == NULL)
			elog(ERROR, "SPI_prepare() failed");

		/************************************************************
		 * Save the plan into permanent memory (right now it's in the
		 * SPI procCxt, which will go away at function end).
		 ************************************************************/
		if (SPI_keepplan(qdesc->plan))
			elog(ERROR, "SPI_keepplan() failed");

		pltcl_subtrans_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		pltcl_subtrans_abort(interp, oldcontext, oldowner);

		MemoryContextDelete(plan_cxt);

		return TCL_ERROR;
	}
	PG_END_TRY();

	/************************************************************
	 * Insert a hashtable entry for the plan and return
	 * the key to the caller
	 ************************************************************/
	query_hash = &pltcl_current_prodesc->interp_desc->query_hash;

	hashent = Tcl_CreateHashEntry(query_hash, qdesc->qname, &hashnew);
	Tcl_SetHashValue(hashent, (ClientData) qdesc);

	/* qname is ASCII, so no need for encoding conversion */
	Tcl_SetObjResult(interp, Tcl_NewStringObj(qdesc->qname, -1));
	return TCL_OK;
}


/**********************************************************************
 * pltcl_SPI_execute_plan()		- Execute a prepared plan
 **********************************************************************/
static int
pltcl_SPI_execute_plan(ClientData cdata, Tcl_Interp *interp,
					   int objc, Tcl_Obj *const objv[])
{
	int			my_rc;
	int			spi_rc;
	int			i;
	int			j;
	int			optIndex;
	Tcl_HashEntry *hashent;
	pltcl_query_desc *qdesc;
	const char *nulls = NULL;
	const char *arrayname = NULL;
	Tcl_Obj    *loop_body = NULL;
	int			count = 0;
	int			callObjc;
	Tcl_Obj   **callObjv = NULL;
	Datum	   *argvalues;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	Tcl_HashTable *query_hash;

	enum options
	{
		OPT_ARRAY, OPT_COUNT, OPT_NULLS
	};

	static const char *options[] = {
		"-array", "-count", "-nulls", (const char *) NULL
	};

	/************************************************************
	 * Get the options and check syntax
	 ************************************************************/
	i = 1;
	while (i < objc)
	{
		if (Tcl_GetIndexFromObj(interp, objv[i], options, "option",
								TCL_EXACT, &optIndex) != TCL_OK)
			break;

		if (++i >= objc)
		{
			Tcl_SetObjResult(interp,
							 Tcl_NewStringObj("missing argument to -array, -count or -nulls", -1));
			return TCL_ERROR;
		}

		switch ((enum options) optIndex)
		{
			case OPT_ARRAY:
				arrayname = Tcl_GetString(objv[i++]);
				break;

			case OPT_COUNT:
				if (Tcl_GetIntFromObj(interp, objv[i++], &count) != TCL_OK)
					return TCL_ERROR;
				break;

			case OPT_NULLS:
				nulls = Tcl_GetString(objv[i++]);
				break;
		}
	}

	/************************************************************
	 * Get the prepared plan descriptor by its key
	 ************************************************************/
	if (i >= objc)
	{
		Tcl_SetObjResult(interp,
			   Tcl_NewStringObj("missing argument to -count or -array", -1));
		return TCL_ERROR;
	}

	query_hash = &pltcl_current_prodesc->interp_desc->query_hash;

	hashent = Tcl_FindHashEntry(query_hash, Tcl_GetString(objv[i]));
	if (hashent == NULL)
	{
		Tcl_AppendResult(interp, "invalid queryid '", Tcl_GetString(objv[i]), "'", NULL);
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
			Tcl_SetObjResult(interp,
							 Tcl_NewStringObj(
				  "length of nulls string doesn't match number of arguments",
											  -1));
			return TCL_ERROR;
		}
	}

	/************************************************************
	 * If there was a argtype list on preparation, we need
	 * an argument value list now
	 ************************************************************/
	if (qdesc->nargs > 0)
	{
		if (i >= objc)
		{
			Tcl_SetObjResult(interp,
							 Tcl_NewStringObj(
			"argument list length doesn't match number of arguments for query"
											  ,-1));
			return TCL_ERROR;
		}

		/************************************************************
		 * Split the argument values
		 ************************************************************/
		if (Tcl_ListObjGetElements(interp, objv[i++], &callObjc, &callObjv) != TCL_OK)
			return TCL_ERROR;

		/************************************************************
		 * Check that the number of arguments matches
		 ************************************************************/
		if (callObjc != qdesc->nargs)
		{
			Tcl_SetObjResult(interp,
							 Tcl_NewStringObj(
			"argument list length doesn't match number of arguments for query"
											  ,-1));
			return TCL_ERROR;
		}
	}
	else
		callObjc = 0;

	/************************************************************
	 * Get loop body if present
	 ************************************************************/
	if (i < objc)
		loop_body = objv[i++];

	if (i != objc)
	{
		Tcl_WrongNumArgs(interp, 1, objv,
						 "?-count n? ?-array name? ?-nulls string? "
						 "query ?args? ?loop body?");
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
		argvalues = (Datum *) palloc(callObjc * sizeof(Datum));

		for (j = 0; j < callObjc; j++)
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
										 UTF_U2E(Tcl_GetString(callObjv[j])),
												 qdesc->argtypioparams[j],
												 -1);
				UTF_END;
			}
		}

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
				  int objc, Tcl_Obj *const objv[])
{
	Tcl_SetObjResult(interp, Tcl_NewWideIntObj(SPI_lastoid));
	return TCL_OK;
}


/**********************************************************************
 * pltcl_set_tuple_values() - Set variables for all attributes
 *				  of a given tuple
 *
 * Note: arrayname is presumed to be UTF8; it usually came from Tcl
 **********************************************************************/
static void
pltcl_set_tuple_values(Tcl_Interp *interp, const char *arrayname,
					   int tupno, HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	char	   *outputstr;
	Datum		attr;
	bool		isnull;
	const char *attname;
	Oid			typoutput;
	bool		typisvarlena;
	const char **arrptr;
	const char **nameptr;
	const char *nullname = NULL;

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
		Tcl_SetVar2Ex(interp, arrayname, ".tupno", Tcl_NewIntObj(tupno), 0);
	}

	for (i = 0; i < tupdesc->natts; i++)
	{
		/* ignore dropped attributes */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		UTF_BEGIN;
		attname = pstrdup(UTF_E2U(NameStr(tupdesc->attrs[i]->attname)));
		UTF_END;

		/************************************************************
		 * Get the attributes value
		 ************************************************************/
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/************************************************************
		 * If there is a value, set the variable
		 * If not, unset it
		 *
		 * Hmmm - Null attributes will cause functions to
		 *		  crash if they don't expect them - need something
		 *		  smarter here.
		 ************************************************************/
		if (!isnull)
		{
			getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
							  &typoutput, &typisvarlena);
			outputstr = OidOutputFunctionCall(typoutput, attr);
			UTF_BEGIN;
			Tcl_SetVar2Ex(interp, *arrptr, *nameptr,
						  Tcl_NewStringObj(UTF_E2U(outputstr), -1), 0);
			UTF_END;
			pfree(outputstr);
		}
		else
			Tcl_UnsetVar2(interp, *arrptr, *nameptr, 0);

		pfree((char *) attname);
	}
}


/**********************************************************************
 * pltcl_build_tuple_argument() - Build a list object usable for 'array set'
 *				  from all attributes of a given tuple
 **********************************************************************/
static Tcl_Obj *
pltcl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc)
{
	Tcl_Obj    *retobj = Tcl_NewObj();
	int			i;
	char	   *outputstr;
	Datum		attr;
	bool		isnull;
	char	   *attname;
	Oid			typoutput;
	bool		typisvarlena;

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
		 * If there is a value, append the attribute name and the
		 * value to the list
		 *
		 * Hmmm - Null attributes will cause functions to
		 *		  crash if they don't expect them - need something
		 *		  smarter here.
		 ************************************************************/
		if (!isnull)
		{
			getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
							  &typoutput, &typisvarlena);
			outputstr = OidOutputFunctionCall(typoutput, attr);
			UTF_BEGIN;
			Tcl_ListObjAppendElement(NULL, retobj,
									 Tcl_NewStringObj(UTF_E2U(attname), -1));
			UTF_END;
			UTF_BEGIN;
			Tcl_ListObjAppendElement(NULL, retobj,
								   Tcl_NewStringObj(UTF_E2U(outputstr), -1));
			UTF_END;
			pfree(outputstr);
		}
	}

	return retobj;
}
