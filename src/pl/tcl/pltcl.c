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
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
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
 *
 * The pltcl_proc_desc struct itself, as well as all subsidiary data,
 * is stored in the memory context identified by the fn_cxt field.
 * We can reclaim all the data by deleting that context, and should do so
 * when the fn_refcount goes to zero.  That will happen if we build a new
 * pltcl_proc_desc following an update of the pg_proc row.  If that happens
 * while the old proc is being executed, we mustn't remove the struct until
 * execution finishes.  When building a new pltcl_proc_desc, we unlink
 * Tcl's copy of the old procedure definition, similarly relying on Tcl's
 * internal reference counting to prevent that structure from disappearing
 * while it's in use.
 *
 * Note that the data in this struct is shared across all active calls;
 * nothing except the fn_refcount should be changed by a call instance.
 **********************************************************************/
typedef struct pltcl_proc_desc
{
	char	   *user_proname;	/* user's name (from format_procedure) */
	char	   *internal_proname;	/* Tcl proc name (NULL if deleted) */
	MemoryContext fn_cxt;		/* memory context for this procedure */
	unsigned long fn_refcount;	/* number of active references */
	TransactionId fn_xmin;		/* xmin of pg_proc row */
	ItemPointerData fn_tid;		/* TID of pg_proc row */
	bool		fn_readonly;	/* is function readonly? */
	bool		lanpltrusted;	/* is it pltcl (vs. pltclu)? */
	pltcl_interp_desc *interp_desc; /* interpreter to use */
	Oid			result_typid;	/* OID of fn's result type */
	FmgrInfo	result_in_func; /* input function for fn's result type */
	Oid			result_typioparam;	/* param to pass to same */
	bool		fn_retisset;	/* true if function returns a set */
	bool		fn_retistuple;	/* true if function returns composite */
	bool		fn_retisdomain; /* true if function returns domain */
	void	   *domain_info;	/* opaque cache for domain checks */
	int			nargs;			/* number of arguments */
	/* these arrays have nargs entries: */
	FmgrInfo   *arg_out_func;	/* output fns for arg types */
	bool	   *arg_is_rowtype; /* is each arg composite? */
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
 * Per-call state
 **********************************************************************/
typedef struct pltcl_call_state
{
	/* Call info struct, or NULL in a trigger */
	FunctionCallInfo fcinfo;

	/* Trigger data, if we're in a normal (not event) trigger; else NULL */
	TriggerData *trigdata;

	/* Function we're executing (NULL if not yet identified) */
	pltcl_proc_desc *prodesc;

	/*
	 * Information for SRFs and functions returning composite types.
	 * ret_tupdesc and attinmeta are set up if either fn_retistuple or
	 * fn_retisset, since even a scalar-returning SRF needs a tuplestore.
	 */
	TupleDesc	ret_tupdesc;	/* return rowtype, if retistuple or retisset */
	AttInMetadata *attinmeta;	/* metadata for building tuples of that type */

	ReturnSetInfo *rsi;			/* passed-in ReturnSetInfo, if any */
	Tuplestorestate *tuple_store;	/* SRFs accumulate result here */
	MemoryContext tuple_store_cxt;	/* context and resowner for tuplestore */
	ResourceOwner tuple_store_owner;
} pltcl_call_state;


/**********************************************************************
 * Global data
 **********************************************************************/
static char *pltcl_start_proc = NULL;
static char *pltclu_start_proc = NULL;
static bool pltcl_pm_init_done = false;
static Tcl_Interp *pltcl_hold_interp = NULL;
static HTAB *pltcl_interp_htab = NULL;
static HTAB *pltcl_proc_htab = NULL;

/* this is saved and restored by pltcl_handler */
static pltcl_call_state *pltcl_current_call_state = NULL;

/**********************************************************************
 * Lookup table for SQLSTATE condition names
 **********************************************************************/
typedef struct
{
	const char *label;
	int			sqlerrstate;
} TclExceptionNameMap;

static const TclExceptionNameMap exception_name_map[] = {
#include "pltclerrcodes.h"		/* pgrminclude ignore */
	{NULL, 0}
};

/**********************************************************************
 * Forward declarations
 **********************************************************************/

static void pltcl_init_interp(pltcl_interp_desc *interp_desc,
							  Oid prolang, bool pltrusted);
static pltcl_interp_desc *pltcl_fetch_interp(Oid prolang, bool pltrusted);
static void call_pltcl_start_proc(Oid prolang, bool pltrusted);
static void start_proc_error_callback(void *arg);

static Datum pltcl_handler(PG_FUNCTION_ARGS, bool pltrusted);

static Datum pltcl_func_handler(PG_FUNCTION_ARGS, pltcl_call_state *call_state,
								bool pltrusted);
static HeapTuple pltcl_trigger_handler(PG_FUNCTION_ARGS, pltcl_call_state *call_state,
									   bool pltrusted);
static void pltcl_event_trigger_handler(PG_FUNCTION_ARGS, pltcl_call_state *call_state,
										bool pltrusted);

static void throw_tcl_error(Tcl_Interp *interp, const char *proname);

static pltcl_proc_desc *compile_pltcl_function(Oid fn_oid, Oid tgreloid,
											   bool is_event_trigger,
											   bool pltrusted);

static int	pltcl_elog(ClientData cdata, Tcl_Interp *interp,
					   int objc, Tcl_Obj *const objv[]);
static void pltcl_construct_errorCode(Tcl_Interp *interp, ErrorData *edata);
static const char *pltcl_get_condition_name(int sqlstate);
static int	pltcl_quote(ClientData cdata, Tcl_Interp *interp,
						int objc, Tcl_Obj *const objv[]);
static int	pltcl_argisnull(ClientData cdata, Tcl_Interp *interp,
							int objc, Tcl_Obj *const objv[]);
static int	pltcl_returnnull(ClientData cdata, Tcl_Interp *interp,
							 int objc, Tcl_Obj *const objv[]);
static int	pltcl_returnnext(ClientData cdata, Tcl_Interp *interp,
							 int objc, Tcl_Obj *const objv[]);
static int	pltcl_SPI_execute(ClientData cdata, Tcl_Interp *interp,
							  int objc, Tcl_Obj *const objv[]);
static int	pltcl_process_SPI_result(Tcl_Interp *interp,
									 const char *arrayname,
									 Tcl_Obj *loop_body,
									 int spi_rc,
									 SPITupleTable *tuptable,
									 uint64 ntuples);
static int	pltcl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
							  int objc, Tcl_Obj *const objv[]);
static int	pltcl_SPI_execute_plan(ClientData cdata, Tcl_Interp *interp,
								   int objc, Tcl_Obj *const objv[]);
static int	pltcl_subtransaction(ClientData cdata, Tcl_Interp *interp,
								 int objc, Tcl_Obj *const objv[]);
static int	pltcl_commit(ClientData cdata, Tcl_Interp *interp,
						 int objc, Tcl_Obj *const objv[]);
static int	pltcl_rollback(ClientData cdata, Tcl_Interp *interp,
						   int objc, Tcl_Obj *const objv[]);

static void pltcl_subtrans_begin(MemoryContext oldcontext,
								 ResourceOwner oldowner);
static void pltcl_subtrans_commit(MemoryContext oldcontext,
								  ResourceOwner oldowner);
static void pltcl_subtrans_abort(Tcl_Interp *interp,
								 MemoryContext oldcontext,
								 ResourceOwner oldowner);

static void pltcl_set_tuple_values(Tcl_Interp *interp, const char *arrayname,
								   uint64 tupno, HeapTuple tuple, TupleDesc tupdesc);
static Tcl_Obj *pltcl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc, bool include_generated);
static HeapTuple pltcl_build_tuple_result(Tcl_Interp *interp,
										  Tcl_Obj **kvObjv, int kvObjc,
										  pltcl_call_state *call_state);
static void pltcl_init_tuple_store(pltcl_call_state *call_state);


/*
 * Hack to override Tcl's builtin Notifier subsystem.  This prevents the
 * backend from becoming multithreaded, which breaks all sorts of things.
 * That happens in the default version of Tcl_InitNotifier if the Tcl library
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
		elog(ERROR, "could not create dummy Tcl interpreter");
	if (Tcl_Init(pltcl_hold_interp) == TCL_ERROR)
		elog(ERROR, "could not initialize dummy Tcl interpreter");

	/************************************************************
	 * Create the hash table for working interpreters
	 ************************************************************/
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(pltcl_interp_desc);
	pltcl_interp_htab = hash_create("PL/Tcl interpreters",
									8,
									&hash_ctl,
									HASH_ELEM | HASH_BLOBS);

	/************************************************************
	 * Create the hash table for function lookup
	 ************************************************************/
	hash_ctl.keysize = sizeof(pltcl_proc_key);
	hash_ctl.entrysize = sizeof(pltcl_proc_ptr);
	pltcl_proc_htab = hash_create("PL/Tcl functions",
								  100,
								  &hash_ctl,
								  HASH_ELEM | HASH_BLOBS);

	/************************************************************
	 * Define PL/Tcl's custom GUCs
	 ************************************************************/
	DefineCustomStringVariable("pltcl.start_proc",
							   gettext_noop("PL/Tcl function to call once when pltcl is first used."),
							   NULL,
							   &pltcl_start_proc,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("pltclu.start_proc",
							   gettext_noop("PL/TclU function to call once when pltclu is first used."),
							   NULL,
							   &pltclu_start_proc,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("pltcl");
	MarkGUCPrefixReserved("pltclu");

	pltcl_pm_init_done = true;
}

/**********************************************************************
 * pltcl_init_interp() - initialize a new Tcl interpreter
 **********************************************************************/
static void
pltcl_init_interp(pltcl_interp_desc *interp_desc, Oid prolang, bool pltrusted)
{
	Tcl_Interp *interp;
	char		interpname[32];

	/************************************************************
	 * Create the Tcl interpreter subsidiary to pltcl_hold_interp.
	 * Note: Tcl automatically does Tcl_Init in the untrusted case,
	 * and it's not wanted in the trusted case.
	 ************************************************************/
	snprintf(interpname, sizeof(interpname), "subsidiary_%u", interp_desc->user_id);
	if ((interp = Tcl_CreateSlave(pltcl_hold_interp, interpname,
								  pltrusted ? 1 : 0)) == NULL)
		elog(ERROR, "could not create subsidiary Tcl interpreter");

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
	Tcl_CreateObjCommand(interp, "return_next",
						 pltcl_returnnext, NULL, NULL);
	Tcl_CreateObjCommand(interp, "spi_exec",
						 pltcl_SPI_execute, NULL, NULL);
	Tcl_CreateObjCommand(interp, "spi_prepare",
						 pltcl_SPI_prepare, NULL, NULL);
	Tcl_CreateObjCommand(interp, "spi_execp",
						 pltcl_SPI_execute_plan, NULL, NULL);
	Tcl_CreateObjCommand(interp, "subtransaction",
						 pltcl_subtransaction, NULL, NULL);
	Tcl_CreateObjCommand(interp, "commit",
						 pltcl_commit, NULL, NULL);
	Tcl_CreateObjCommand(interp, "rollback",
						 pltcl_rollback, NULL, NULL);

	/************************************************************
	 * Call the appropriate start_proc, if there is one.
	 *
	 * We must set interp_desc->interp before the call, else the start_proc
	 * won't find the interpreter it's supposed to use.  But, if the
	 * start_proc fails, we want to abandon use of the interpreter.
	 ************************************************************/
	PG_TRY();
	{
		interp_desc->interp = interp;
		call_pltcl_start_proc(prolang, pltrusted);
	}
	PG_CATCH();
	{
		interp_desc->interp = NULL;
		Tcl_DeleteInterp(interp);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/**********************************************************************
 * pltcl_fetch_interp() - fetch the Tcl interpreter to use for a function
 *
 * This also takes care of any on-first-use initialization required.
 **********************************************************************/
static pltcl_interp_desc *
pltcl_fetch_interp(Oid prolang, bool pltrusted)
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
		interp_desc->interp = NULL;

	/* If we haven't yet successfully made an interpreter, try to do that */
	if (!interp_desc->interp)
		pltcl_init_interp(interp_desc, prolang, pltrusted);

	return interp_desc;
}


/**********************************************************************
 * call_pltcl_start_proc()	 - Call user-defined initialization proc, if any
 **********************************************************************/
static void
call_pltcl_start_proc(Oid prolang, bool pltrusted)
{
	LOCAL_FCINFO(fcinfo, 0);
	char	   *start_proc;
	const char *gucname;
	ErrorContextCallback errcallback;
	List	   *namelist;
	Oid			procOid;
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	AclResult	aclresult;
	FmgrInfo	finfo;
	PgStat_FunctionCallUsage fcusage;

	/* select appropriate GUC */
	start_proc = pltrusted ? pltcl_start_proc : pltclu_start_proc;
	gucname = pltrusted ? "pltcl.start_proc" : "pltclu.start_proc";

	/* Nothing to do if it's empty or unset */
	if (start_proc == NULL || start_proc[0] == '\0')
		return;

	/* Set up errcontext callback to make errors more helpful */
	errcallback.callback = start_proc_error_callback;
	errcallback.arg = unconstify(char *, gucname);
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Parse possibly-qualified identifier and look up the function */
	namelist = stringToQualifiedNameList(start_proc, NULL);
	procOid = LookupFuncName(namelist, 0, NULL, false);

	/* Current user must have permission to call function */
	aclresult = object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, start_proc);

	/* Get the function's pg_proc entry */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procOid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", procOid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/* It must be same language as the function we're currently calling */
	if (procStruct->prolang != prolang)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("function \"%s\" is in the wrong language",
						start_proc)));

	/*
	 * It must not be SECURITY DEFINER, either.  This together with the
	 * language match check ensures that the function will execute in the same
	 * Tcl interpreter we just finished initializing.
	 */
	if (procStruct->prosecdef)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("function \"%s\" must not be SECURITY DEFINER",
						start_proc)));

	/* A-OK */
	ReleaseSysCache(procTup);

	/*
	 * Call the function using the normal SQL function call mechanism.  We
	 * could perhaps cheat and jump directly to pltcl_handler(), but it seems
	 * better to do it this way so that the call is exposed to, eg, call
	 * statistics collection.
	 */
	InvokeFunctionExecuteHook(procOid);
	fmgr_info(procOid, &finfo);
	InitFunctionCallInfoData(*fcinfo, &finfo,
							 0,
							 InvalidOid, NULL, NULL);
	pgstat_init_function_usage(fcinfo, &fcusage);
	(void) FunctionCallInvoke(fcinfo);
	pgstat_end_function_usage(&fcusage, true);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

/*
 * Error context callback for errors occurring during start_proc processing.
 */
static void
start_proc_error_callback(void *arg)
{
	const char *gucname = (const char *) arg;

	/* translator: %s is "pltcl.start_proc" or "pltclu.start_proc" */
	errcontext("processing %s parameter", gucname);
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


/**********************************************************************
 * pltcl_handler()		- Handler for function and trigger calls, for
 *						  both trusted and untrusted interpreters.
 **********************************************************************/
static Datum
pltcl_handler(PG_FUNCTION_ARGS, bool pltrusted)
{
	Datum		retval = (Datum) 0;
	pltcl_call_state current_call_state;
	pltcl_call_state *save_call_state;

	/*
	 * Initialize current_call_state to nulls/zeroes; in particular, set its
	 * prodesc pointer to null.  Anything that sets it non-null should
	 * increase the prodesc's fn_refcount at the same time.  We'll decrease
	 * the refcount, and then delete the prodesc if it's no longer referenced,
	 * on the way out of this function.  This ensures that prodescs live as
	 * long as needed even if somebody replaces the originating pg_proc row
	 * while they're executing.
	 */
	memset(&current_call_state, 0, sizeof(current_call_state));

	/*
	 * Ensure that static pointer is saved/restored properly
	 */
	save_call_state = pltcl_current_call_state;
	pltcl_current_call_state = &current_call_state;

	PG_TRY();
	{
		/*
		 * Determine if called as function or trigger and call appropriate
		 * subhandler
		 */
		if (CALLED_AS_TRIGGER(fcinfo))
		{
			/* invoke the trigger handler */
			retval = PointerGetDatum(pltcl_trigger_handler(fcinfo,
														   &current_call_state,
														   pltrusted));
		}
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		{
			/* invoke the event trigger handler */
			pltcl_event_trigger_handler(fcinfo, &current_call_state, pltrusted);
			retval = (Datum) 0;
		}
		else
		{
			/* invoke the regular function handler */
			current_call_state.fcinfo = fcinfo;
			retval = pltcl_func_handler(fcinfo, &current_call_state, pltrusted);
		}
	}
	PG_FINALLY();
	{
		/* Restore static pointer, then clean up the prodesc refcount if any */
		/*
		 * (We're being paranoid in case an error is thrown in context
		 * deletion)
		 */
		pltcl_current_call_state = save_call_state;
		if (current_call_state.prodesc != NULL)
		{
			Assert(current_call_state.prodesc->fn_refcount > 0);
			if (--current_call_state.prodesc->fn_refcount == 0)
				MemoryContextDelete(current_call_state.prodesc->fn_cxt);
		}
	}
	PG_END_TRY();

	return retval;
}


/**********************************************************************
 * pltcl_func_handler()		- Handler for regular function calls
 **********************************************************************/
static Datum
pltcl_func_handler(PG_FUNCTION_ARGS, pltcl_call_state *call_state,
				   bool pltrusted)
{
	bool		nonatomic;
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	Tcl_Obj    *tcl_cmd;
	int			i;
	int			tcl_rc;
	Datum		retval;

	nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	/* Connect to SPI manager */
	SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0);

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid, InvalidOid,
									 false, pltrusted);

	call_state->prodesc = prodesc;
	prodesc->fn_refcount++;

	interp = prodesc->interp_desc->interp;

	/*
	 * If we're a SRF, check caller can handle materialize mode, and save
	 * relevant info into call_state.  We must ensure that the returned
	 * tuplestore is owned by the caller's context, even if we first create it
	 * inside a subtransaction.
	 */
	if (prodesc->fn_retisset)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (!rsi || !IsA(rsi, ReturnSetInfo))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));

		if (!(rsi->allowedModes & SFRM_Materialize))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialize mode required, but it is not allowed in this context")));

		call_state->rsi = rsi;
		call_state->tuple_store_cxt = rsi->econtext->ecxt_per_query_memory;
		call_state->tuple_store_owner = CurrentResourceOwner;
	}

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
				if (fcinfo->args[i].isnull)
					Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());
				else
				{
					HeapTupleHeader td;
					Oid			tupType;
					int32		tupTypmod;
					TupleDesc	tupdesc;
					HeapTupleData tmptup;
					Tcl_Obj    *list_tmp;

					td = DatumGetHeapTupleHeader(fcinfo->args[i].value);
					/* Extract rowtype info and find a tupdesc */
					tupType = HeapTupleHeaderGetTypeId(td);
					tupTypmod = HeapTupleHeaderGetTypMod(td);
					tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
					/* Build a temporary HeapTuple control structure */
					tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
					tmptup.t_data = td;

					list_tmp = pltcl_build_tuple_argument(&tmptup, tupdesc, true);
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
				if (fcinfo->args[i].isnull)
					Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());
				else
				{
					char	   *tmp;

					tmp = OutputFunctionCall(&prodesc->arg_out_func[i],
											 fcinfo->args[i].value);
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

	if (prodesc->fn_retisset)
	{
		ReturnSetInfo *rsi = call_state->rsi;

		/* We already checked this is OK */
		rsi->returnMode = SFRM_Materialize;

		/* If we produced any tuples, send back the result */
		if (call_state->tuple_store)
		{
			rsi->setResult = call_state->tuple_store;
			if (call_state->ret_tupdesc)
			{
				MemoryContext oldcxt;

				oldcxt = MemoryContextSwitchTo(call_state->tuple_store_cxt);
				rsi->setDesc = CreateTupleDescCopy(call_state->ret_tupdesc);
				MemoryContextSwitchTo(oldcxt);
			}
		}
		retval = (Datum) 0;
		fcinfo->isnull = true;
	}
	else if (fcinfo->isnull)
	{
		retval = InputFunctionCall(&prodesc->result_in_func,
								   NULL,
								   prodesc->result_typioparam,
								   -1);
	}
	else if (prodesc->fn_retistuple)
	{
		TupleDesc	td;
		HeapTuple	tup;
		Tcl_Obj    *resultObj;
		Tcl_Obj   **resultObjv;
		int			resultObjc;

		/*
		 * Set up data about result type.  XXX it's tempting to consider
		 * caching this in the prodesc, in the common case where the rowtype
		 * is determined by the function not the calling query.  But we'd have
		 * to be able to deal with ADD/DROP/ALTER COLUMN events when the
		 * result type is a named composite type, so it's not exactly trivial.
		 * Maybe worth improving someday.
		 */
		switch (get_call_result_type(fcinfo, NULL, &td))
		{
			case TYPEFUNC_COMPOSITE:
				/* success */
				break;
			case TYPEFUNC_COMPOSITE_DOMAIN:
				Assert(prodesc->fn_retisdomain);
				break;
			case TYPEFUNC_RECORD:
				/* failed to determine actual type of RECORD */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
				break;
			default:
				/* result type isn't composite? */
				elog(ERROR, "return type must be a row type");
				break;
		}

		Assert(!call_state->ret_tupdesc);
		Assert(!call_state->attinmeta);
		call_state->ret_tupdesc = td;
		call_state->attinmeta = TupleDescGetAttInMetadata(td);

		/* Convert function result to tuple */
		resultObj = Tcl_GetObjResult(interp);
		if (Tcl_ListObjGetElements(interp, resultObj, &resultObjc, &resultObjv) == TCL_ERROR)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("could not parse function return value: %s",
							utf_u2e(Tcl_GetStringResult(interp)))));

		tup = pltcl_build_tuple_result(interp, resultObjv, resultObjc,
									   call_state);
		retval = HeapTupleGetDatum(tup);
	}
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
pltcl_trigger_handler(PG_FUNCTION_ARGS, pltcl_call_state *call_state,
					  bool pltrusted)
{
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	char	   *stroid;
	TupleDesc	tupdesc;
	volatile HeapTuple rettup;
	Tcl_Obj    *tcl_cmd;
	Tcl_Obj    *tcl_trigtup;
	int			tcl_rc;
	int			i;
	const char *result;
	int			result_Objc;
	Tcl_Obj   **result_Objv;
	int			rc PG_USED_FOR_ASSERTS_ONLY;

	call_state->trigdata = trigdata;

	/* Connect to SPI manager */
	SPI_connect();

	/* Make transition tables visible to this SPI connection */
	rc = SPI_register_trigger_data(trigdata);
	Assert(rc >= 0);

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid,
									 RelationGetRelid(trigdata->tg_relation),
									 false, /* not an event trigger */
									 pltrusted);

	call_state->prodesc = prodesc;
	prodesc->fn_refcount++;

	interp = prodesc->interp_desc->interp;

	tupdesc = RelationGetDescr(trigdata->tg_relation);

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
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (att->attisdropped)
				Tcl_ListObjAppendElement(NULL, tcl_trigtup, Tcl_NewObj());
			else
				Tcl_ListObjAppendElement(NULL, tcl_trigtup,
										 Tcl_NewStringObj(utf_e2u(NameStr(att->attname)), -1));
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

			/*
			 * Now the command part of the event for TG_op and data for NEW
			 * and OLD
			 *
			 * Note: In BEFORE trigger, stored generated columns are not
			 * computed yet, so don't make them accessible in NEW row.
			 */
			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			{
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("INSERT", -1));

				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 pltcl_build_tuple_argument(trigdata->tg_trigtuple,
																	tupdesc,
																	!TRIGGER_FIRED_BEFORE(trigdata->tg_event)));
				Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());

				rettup = trigdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			{
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("DELETE", -1));

				Tcl_ListObjAppendElement(NULL, tcl_cmd, Tcl_NewObj());
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 pltcl_build_tuple_argument(trigdata->tg_trigtuple,
																	tupdesc,
																	true));

				rettup = trigdata->tg_trigtuple;
			}
			else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			{
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 Tcl_NewStringObj("UPDATE", -1));

				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 pltcl_build_tuple_argument(trigdata->tg_newtuple,
																	tupdesc,
																	!TRIGGER_FIRED_BEFORE(trigdata->tg_event)));
				Tcl_ListObjAppendElement(NULL, tcl_cmd,
										 pltcl_build_tuple_argument(trigdata->tg_trigtuple,
																	tupdesc,
																	true));

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
	 * Exit SPI environment.
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	/************************************************************
	 * The return value from the procedure might be one of
	 * the magic strings OK or SKIP, or a list from array get.
	 * We can check for OK or SKIP without worrying about encoding.
	 ************************************************************/
	result = Tcl_GetStringResult(interp);

	if (strcmp(result, "OK") == 0)
		return rettup;
	if (strcmp(result, "SKIP") == 0)
		return (HeapTuple) NULL;

	/************************************************************
	 * Otherwise, the return value should be a column name/value list
	 * specifying the modified tuple to return.
	 ************************************************************/
	if (Tcl_ListObjGetElements(interp, Tcl_GetObjResult(interp),
							   &result_Objc, &result_Objv) != TCL_OK)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("could not parse trigger return value: %s",
						utf_u2e(Tcl_GetStringResult(interp)))));

	/* Convert function result to tuple */
	rettup = pltcl_build_tuple_result(interp, result_Objv, result_Objc,
									  call_state);

	return rettup;
}

/**********************************************************************
 * pltcl_event_trigger_handler()	- Handler for event trigger calls
 **********************************************************************/
static void
pltcl_event_trigger_handler(PG_FUNCTION_ARGS, pltcl_call_state *call_state,
							bool pltrusted)
{
	pltcl_proc_desc *prodesc;
	Tcl_Interp *volatile interp;
	EventTriggerData *tdata = (EventTriggerData *) fcinfo->context;
	Tcl_Obj    *tcl_cmd;
	int			tcl_rc;

	/* Connect to SPI manager */
	SPI_connect();

	/* Find or compile the function */
	prodesc = compile_pltcl_function(fcinfo->flinfo->fn_oid,
									 InvalidOid, true, pltrusted);

	call_state->prodesc = prodesc;
	prodesc->fn_refcount++;

	interp = prodesc->interp_desc->interp;

	/* Create the tcl command and call the internal proc */
	tcl_cmd = Tcl_NewObj();
	Tcl_IncrRefCount(tcl_cmd);
	Tcl_ListObjAppendElement(NULL, tcl_cmd,
							 Tcl_NewStringObj(prodesc->internal_proname, -1));
	Tcl_ListObjAppendElement(NULL, tcl_cmd,
							 Tcl_NewStringObj(utf_e2u(tdata->event), -1));
	Tcl_ListObjAppendElement(NULL, tcl_cmd,
							 Tcl_NewStringObj(utf_e2u(GetCommandTagName(tdata->tag)),
											  -1));

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
 *
 * Caution: use this only to report errors returned by Tcl_EvalObjEx() or
 * other variants of Tcl_Eval().  Other functions may not fill "errorInfo",
 * so it could be unset or even contain details from some previous error.
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
	int			emsglen;

	emsg = pstrdup(utf_u2e(Tcl_GetStringResult(interp)));
	econtext = utf_u2e(Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY));

	/*
	 * Typically, the first line of errorInfo matches the primary error
	 * message (the interpreter result); don't print that twice if so.
	 */
	emsglen = strlen(emsg);
	if (strncmp(emsg, econtext, emsglen) == 0 &&
		econtext[emsglen] == '\n')
		econtext += emsglen + 1;

	/* Tcl likes to prefix the next line with some spaces, too */
	while (*econtext == ' ')
		econtext++;

	/* Note: proname will already contain quoting if any is needed */
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("%s", emsg),
			 errcontext("%s\nin PL/Tcl function %s",
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
	pltcl_proc_desc *old_prodesc;
	volatile MemoryContext proc_cxt = NULL;
	Tcl_DString proc_internal_def;
	Tcl_DString proc_internal_name;
	Tcl_DString proc_internal_body;

	/* We'll need the pg_proc tuple in any case... */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Look up function in pltcl_proc_htab; if it's not there, create an entry
	 * and set the entry's proc_ptr to NULL.
	 */
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
	if (prodesc != NULL &&
		prodesc->internal_proname != NULL &&
		prodesc->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
		ItemPointerEquals(&prodesc->fn_tid, &procTup->t_self))
	{
		/* It's still up-to-date, so we can use it */
		ReleaseSysCache(procTup);
		return prodesc;
	}

	/************************************************************
	 * If we haven't found it in the hashtable, we analyze
	 * the functions arguments and returntype and store
	 * the in-/out-functions in the prodesc block and create
	 * a new hashtable entry for it.
	 *
	 * Then we load the procedure into the Tcl interpreter.
	 ************************************************************/
	Tcl_DStringInit(&proc_internal_def);
	Tcl_DStringInit(&proc_internal_name);
	Tcl_DStringInit(&proc_internal_body);
	PG_TRY();
	{
		bool		is_trigger = OidIsValid(tgreloid);
		Tcl_CmdInfo cmdinfo;
		const char *user_proname;
		const char *internal_proname;
		bool		need_underscore;
		HeapTuple	typeTup;
		Form_pg_type typeStruct;
		char		proc_internal_args[33 * FUNC_MAX_ARGS];
		Datum		prosrcdatum;
		char	   *proc_source;
		char		buf[48];
		pltcl_interp_desc *interp_desc;
		Tcl_Interp *interp;
		int			i;
		int			tcl_rc;
		MemoryContext oldcontext;

		/************************************************************
		 * Identify the interpreter to use for the function
		 ************************************************************/
		interp_desc = pltcl_fetch_interp(procStruct->prolang, pltrusted);
		interp = interp_desc->interp;

		/************************************************************
		 * If redefining the function, try to remove the old internal
		 * procedure from Tcl's namespace.  The point of this is partly to
		 * allow re-use of the same internal proc name, and partly to avoid
		 * leaking the Tcl procedure object if we end up not choosing the same
		 * name.  We assume that Tcl is smart enough to not physically delete
		 * the procedure object if it's currently being executed.
		 ************************************************************/
		if (prodesc != NULL &&
			prodesc->internal_proname != NULL)
		{
			/* We simply ignore any error */
			(void) Tcl_DeleteCommand(interp, prodesc->internal_proname);
			/* Don't do this more than once */
			prodesc->internal_proname = NULL;
		}

		/************************************************************
		 * Build the proc name we'll use in error messages.
		 ************************************************************/
		user_proname = format_procedure(fn_oid);

		/************************************************************
		 * Build the internal proc name from the user_proname and/or OID.
		 * The internal name must be all-ASCII since we don't want to deal
		 * with encoding conversions.  We don't want to worry about Tcl
		 * quoting rules either, so use only the characters of the function
		 * name that are ASCII alphanumerics, plus underscores to separate
		 * function name and arguments.  If what we end up with isn't
		 * unique (that is, it matches some existing Tcl command name),
		 * append the function OID (perhaps repeatedly) so that it is unique.
		 ************************************************************/

		/* For historical reasons, use a function-type-specific prefix */
		if (is_event_trigger)
			Tcl_DStringAppend(&proc_internal_name,
							  "__PLTcl_evttrigger_", -1);
		else if (is_trigger)
			Tcl_DStringAppend(&proc_internal_name,
							  "__PLTcl_trigger_", -1);
		else
			Tcl_DStringAppend(&proc_internal_name,
							  "__PLTcl_proc_", -1);
		/* Now add what we can from the user_proname */
		need_underscore = false;
		for (const char *ptr = user_proname; *ptr; ptr++)
		{
			if (strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
					   "abcdefghijklmnopqrstuvwxyz"
					   "0123456789_", *ptr) != NULL)
			{
				/* Done this way to avoid adding a trailing underscore */
				if (need_underscore)
				{
					Tcl_DStringAppend(&proc_internal_name, "_", 1);
					need_underscore = false;
				}
				Tcl_DStringAppend(&proc_internal_name, ptr, 1);
			}
			else if (strchr("(, ", *ptr) != NULL)
				need_underscore = true;
		}
		/* If this name already exists, append fn_oid; repeat as needed */
		while (Tcl_GetCommandInfo(interp,
								  Tcl_DStringValue(&proc_internal_name),
								  &cmdinfo))
		{
			snprintf(buf, sizeof(buf), "_%u", fn_oid);
			Tcl_DStringAppend(&proc_internal_name, buf, -1);
		}
		internal_proname = Tcl_DStringValue(&proc_internal_name);

		/************************************************************
		 * Allocate a context that will hold all PG data for the procedure.
		 ************************************************************/
		proc_cxt = AllocSetContextCreate(TopMemoryContext,
										 "PL/Tcl function",
										 ALLOCSET_SMALL_SIZES);

		/************************************************************
		 * Allocate and fill a new procedure description block.
		 * struct prodesc and subsidiary data must all live in proc_cxt.
		 ************************************************************/
		oldcontext = MemoryContextSwitchTo(proc_cxt);
		prodesc = (pltcl_proc_desc *) palloc0(sizeof(pltcl_proc_desc));
		prodesc->user_proname = pstrdup(user_proname);
		MemoryContextSetIdentifier(proc_cxt, prodesc->user_proname);
		prodesc->internal_proname = pstrdup(internal_proname);
		prodesc->fn_cxt = proc_cxt;
		prodesc->fn_refcount = 0;
		prodesc->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
		prodesc->fn_tid = procTup->t_self;
		prodesc->nargs = procStruct->pronargs;
		prodesc->arg_out_func = (FmgrInfo *) palloc0(prodesc->nargs * sizeof(FmgrInfo));
		prodesc->arg_is_rowtype = (bool *) palloc0(prodesc->nargs * sizeof(bool));
		MemoryContextSwitchTo(oldcontext);

		/* Remember if function is STABLE/IMMUTABLE */
		prodesc->fn_readonly =
			(procStruct->provolatile != PROVOLATILE_VOLATILE);
		/* And whether it is trusted */
		prodesc->lanpltrusted = pltrusted;
		/* Save the associated interpreter, too */
		prodesc->interp_desc = interp_desc;

		/************************************************************
		 * Get the required information for input conversion of the
		 * return value.
		 ************************************************************/
		if (!is_trigger && !is_event_trigger)
		{
			Oid			rettype = procStruct->prorettype;

			typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(rettype));
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "cache lookup failed for type %u", rettype);
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID and RECORD */
			if (typeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (rettype == VOIDOID ||
					rettype == RECORDOID)
					 /* okay */ ;
				else if (rettype == TRIGGEROID ||
						 rettype == EVENT_TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Tcl functions cannot return type %s",
									format_type_be(rettype))));
			}

			prodesc->result_typid = rettype;
			fmgr_info_cxt(typeStruct->typinput,
						  &(prodesc->result_in_func),
						  proc_cxt);
			prodesc->result_typioparam = getTypeIOParam(typeTup);

			prodesc->fn_retisset = procStruct->proretset;
			prodesc->fn_retistuple = type_is_rowtype(rettype);
			prodesc->fn_retisdomain = (typeStruct->typtype == TYPTYPE_DOMAIN);
			prodesc->domain_info = NULL;

			ReleaseSysCache(typeTup);
		}

		/************************************************************
		 * Get the required information for output conversion
		 * of all procedure arguments, and set up argument naming info.
		 ************************************************************/
		if (!is_trigger && !is_event_trigger)
		{
			proc_internal_args[0] = '\0';
			for (i = 0; i < prodesc->nargs; i++)
			{
				Oid			argtype = procStruct->proargtypes.values[i];

				typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(argtype));
				if (!HeapTupleIsValid(typeTup))
					elog(ERROR, "cache lookup failed for type %u", argtype);
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* Disallow pseudotype argument, except RECORD */
				if (typeStruct->typtype == TYPTYPE_PSEUDO &&
					argtype != RECORDOID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Tcl functions cannot accept type %s",
									format_type_be(argtype))));

				if (type_is_rowtype(argtype))
				{
					prodesc->arg_is_rowtype[i] = true;
					snprintf(buf, sizeof(buf), "__PLTcl_Tup_%d", i + 1);
				}
				else
				{
					prodesc->arg_is_rowtype[i] = false;
					fmgr_info_cxt(typeStruct->typoutput,
								  &(prodesc->arg_out_func[i]),
								  proc_cxt);
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
		 * Leave this code as DString - performance is not critical here,
		 * and we don't want to duplicate the knowledge of the Tcl quoting
		 * rules that's embedded in Tcl_DStringAppendElement.
		 ************************************************************/
		Tcl_DStringAppendElement(&proc_internal_def, "proc");
		Tcl_DStringAppendElement(&proc_internal_def, internal_proname);
		Tcl_DStringAppendElement(&proc_internal_def, proc_internal_args);

		/************************************************************
		 * prefix procedure body with
		 * upvar #0 <internal_proname> GD
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
		prosrcdatum = SysCacheGetAttrNotNull(PROCOID, procTup,
											 Anum_pg_proc_prosrc);
		proc_source = TextDatumGetCString(prosrcdatum);
		UTF_BEGIN;
		Tcl_DStringAppend(&proc_internal_body, UTF_E2U(proc_source), -1);
		UTF_END;
		pfree(proc_source);
		Tcl_DStringAppendElement(&proc_internal_def,
								 Tcl_DStringValue(&proc_internal_body));

		/************************************************************
		 * Create the procedure in the interpreter
		 ************************************************************/
		tcl_rc = Tcl_EvalEx(interp,
							Tcl_DStringValue(&proc_internal_def),
							Tcl_DStringLength(&proc_internal_def),
							TCL_EVAL_GLOBAL);
		if (tcl_rc != TCL_OK)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("could not create internal procedure \"%s\": %s",
							internal_proname,
							utf_u2e(Tcl_GetStringResult(interp)))));
	}
	PG_CATCH();
	{
		/*
		 * If we failed anywhere above, clean up whatever got allocated.  It
		 * should all be in the proc_cxt, except for the DStrings.
		 */
		if (proc_cxt)
			MemoryContextDelete(proc_cxt);
		Tcl_DStringFree(&proc_internal_def);
		Tcl_DStringFree(&proc_internal_name);
		Tcl_DStringFree(&proc_internal_body);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * Install the new proc description block in the hashtable, incrementing
	 * its refcount (the hashtable link counts as a reference).  Then, if
	 * there was a previous definition of the function, decrement that one's
	 * refcount, and delete it if no longer referenced.  The order of
	 * operations here is important: if something goes wrong during the
	 * MemoryContextDelete, leaking some memory for the old definition is OK,
	 * but we don't want to corrupt the live hashtable entry.  (Likewise,
	 * freeing the DStrings is pretty low priority if that happens.)
	 */
	old_prodesc = proc_ptr->proc_ptr;

	proc_ptr->proc_ptr = prodesc;
	prodesc->fn_refcount++;

	if (old_prodesc != NULL)
	{
		Assert(old_prodesc->fn_refcount > 0);
		if (--old_prodesc->fn_refcount == 0)
			MemoryContextDelete(old_prodesc->fn_cxt);
	}

	Tcl_DStringFree(&proc_internal_def);
	Tcl_DStringFree(&proc_internal_name);
	Tcl_DStringFree(&proc_internal_body);

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

		/* Pass the error data to Tcl */
		pltcl_construct_errorCode(interp, edata);
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
 * pltcl_construct_errorCode()		- construct a Tcl errorCode
 *		list with detailed information from the PostgreSQL server
 **********************************************************************/
static void
pltcl_construct_errorCode(Tcl_Interp *interp, ErrorData *edata)
{
	Tcl_Obj    *obj = Tcl_NewObj();

	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj("POSTGRES", -1));
	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj(PG_VERSION, -1));
	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj("SQLSTATE", -1));
	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj(unpack_sql_state(edata->sqlerrcode), -1));
	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj("condition", -1));
	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj(pltcl_get_condition_name(edata->sqlerrcode), -1));
	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj("message", -1));
	UTF_BEGIN;
	Tcl_ListObjAppendElement(interp, obj,
							 Tcl_NewStringObj(UTF_E2U(edata->message), -1));
	UTF_END;
	if (edata->detail)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("detail", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->detail), -1));
		UTF_END;
	}
	if (edata->hint)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("hint", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->hint), -1));
		UTF_END;
	}
	if (edata->context)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("context", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->context), -1));
		UTF_END;
	}
	if (edata->schema_name)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("schema", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->schema_name), -1));
		UTF_END;
	}
	if (edata->table_name)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("table", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->table_name), -1));
		UTF_END;
	}
	if (edata->column_name)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("column", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->column_name), -1));
		UTF_END;
	}
	if (edata->datatype_name)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("datatype", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->datatype_name), -1));
		UTF_END;
	}
	if (edata->constraint_name)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("constraint", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->constraint_name), -1));
		UTF_END;
	}
	/* cursorpos is never interesting here; report internal query/pos */
	if (edata->internalquery)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("statement", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->internalquery), -1));
		UTF_END;
	}
	if (edata->internalpos > 0)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("cursor_position", -1));
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewIntObj(edata->internalpos));
	}
	if (edata->filename)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("filename", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->filename), -1));
		UTF_END;
	}
	if (edata->lineno > 0)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("lineno", -1));
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewIntObj(edata->lineno));
	}
	if (edata->funcname)
	{
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj("funcname", -1));
		UTF_BEGIN;
		Tcl_ListObjAppendElement(interp, obj,
								 Tcl_NewStringObj(UTF_E2U(edata->funcname), -1));
		UTF_END;
	}

	Tcl_SetObjErrorCode(interp, obj);
}


/**********************************************************************
 * pltcl_get_condition_name()	- find name for SQLSTATE
 **********************************************************************/
static const char *
pltcl_get_condition_name(int sqlstate)
{
	int			i;

	for (i = 0; exception_name_map[i].label != NULL; i++)
	{
		if (exception_name_map[i].sqlerrstate == sqlstate)
			return exception_name_map[i].label;
	}
	return "unrecognized_sqlstate";
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
	FunctionCallInfo fcinfo = pltcl_current_call_state->fcinfo;

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
	FunctionCallInfo fcinfo = pltcl_current_call_state->fcinfo;

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


/**********************************************************************
 * pltcl_returnnext()	- Add a row to the result tuplestore in a SRF.
 **********************************************************************/
static int
pltcl_returnnext(ClientData cdata, Tcl_Interp *interp,
				 int objc, Tcl_Obj *const objv[])
{
	pltcl_call_state *call_state = pltcl_current_call_state;
	FunctionCallInfo fcinfo = call_state->fcinfo;
	pltcl_proc_desc *prodesc = call_state->prodesc;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	volatile int result = TCL_OK;

	/*
	 * Check that we're called as a set-returning function
	 */
	if (fcinfo == NULL)
	{
		Tcl_SetObjResult(interp,
						 Tcl_NewStringObj("return_next cannot be used in triggers", -1));
		return TCL_ERROR;
	}

	if (!prodesc->fn_retisset)
	{
		Tcl_SetObjResult(interp,
						 Tcl_NewStringObj("return_next cannot be used in non-set-returning functions", -1));
		return TCL_ERROR;
	}

	/*
	 * Check call syntax
	 */
	if (objc != 2)
	{
		Tcl_WrongNumArgs(interp, 1, objv, "result");
		return TCL_ERROR;
	}

	/*
	 * The rest might throw elog(ERROR), so must run in a subtransaction.
	 *
	 * A small advantage of using a subtransaction is that it provides a
	 * short-lived memory context for free, so we needn't worry about leaking
	 * memory here.  To use that context, call BeginInternalSubTransaction
	 * directly instead of going through pltcl_subtrans_begin.
	 */
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		/* Set up tuple store if first output row */
		if (call_state->tuple_store == NULL)
			pltcl_init_tuple_store(call_state);

		if (prodesc->fn_retistuple)
		{
			Tcl_Obj   **rowObjv;
			int			rowObjc;

			/* result should be a list, so break it down */
			if (Tcl_ListObjGetElements(interp, objv[1], &rowObjc, &rowObjv) == TCL_ERROR)
				result = TCL_ERROR;
			else
			{
				HeapTuple	tuple;

				tuple = pltcl_build_tuple_result(interp, rowObjv, rowObjc,
												 call_state);
				tuplestore_puttuple(call_state->tuple_store, tuple);
			}
		}
		else
		{
			Datum		retval;
			bool		isNull = false;

			/* for paranoia's sake, check that tupdesc has exactly one column */
			if (call_state->ret_tupdesc->natts != 1)
				elog(ERROR, "wrong result type supplied in return_next");

			retval = InputFunctionCall(&prodesc->result_in_func,
									   utf_u2e((char *) Tcl_GetString(objv[1])),
									   prodesc->result_typioparam,
									   -1);
			tuplestore_putvalues(call_state->tuple_store, call_state->ret_tupdesc,
								 &retval, &isNull);
		}

		pltcl_subtrans_commit(oldcontext, oldowner);
	}
	PG_CATCH();
	{
		pltcl_subtrans_abort(interp, oldcontext, oldowner);
		return TCL_ERROR;
	}
	PG_END_TRY();

	return result;
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

	/* Pass the error data to Tcl */
	pltcl_construct_errorCode(interp, edata);
	UTF_BEGIN;
	Tcl_SetObjResult(interp, Tcl_NewStringObj(UTF_E2U(edata->message), -1));
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
		if (Tcl_GetIndexFromObj(NULL, objv[i], options, NULL,
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
							 pltcl_current_call_state->prodesc->fn_readonly, count);
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
						 uint64 ntuples)
{
	int			my_rc = TCL_OK;
	int			loop_rc;
	HeapTuple  *tuples;
	TupleDesc	tupdesc;

	switch (spi_rc)
	{
		case SPI_OK_SELINTO:
		case SPI_OK_INSERT:
		case SPI_OK_DELETE:
		case SPI_OK_UPDATE:
		case SPI_OK_MERGE:
			Tcl_SetObjResult(interp, Tcl_NewWideIntObj(ntuples));
			break;

		case SPI_OK_UTILITY:
		case SPI_OK_REWRITTEN:
			if (tuptable == NULL)
			{
				Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
				break;
			}
			/* fall through for utility returning tuples */
			/* FALLTHROUGH */

		case SPI_OK_SELECT:
		case SPI_OK_INSERT_RETURNING:
		case SPI_OK_DELETE_RETURNING:
		case SPI_OK_UPDATE_RETURNING:
		case SPI_OK_MERGE_RETURNING:

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
				uint64		i;

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
				Tcl_SetObjResult(interp, Tcl_NewWideIntObj(ntuples));
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
									 "PL/Tcl spi_prepare query",
									 ALLOCSET_SMALL_SIZES);
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

			(void) parseTypeString(Tcl_GetString(argsObj[i]),
								   &typId, &typmod, NULL);

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
	query_hash = &pltcl_current_call_state->prodesc->interp_desc->query_hash;

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
		if (Tcl_GetIndexFromObj(NULL, objv[i], options, NULL,
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

	query_hash = &pltcl_current_call_state->prodesc->interp_desc->query_hash;

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
							 Tcl_NewStringObj("length of nulls string doesn't match number of arguments",
											  -1));
			return TCL_ERROR;
		}
	}

	/************************************************************
	 * If there was an argtype list on preparation, we need
	 * an argument value list now
	 ************************************************************/
	if (qdesc->nargs > 0)
	{
		if (i >= objc)
		{
			Tcl_SetObjResult(interp,
							 Tcl_NewStringObj("argument list length doesn't match number of arguments for query",
											  -1));
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
							 Tcl_NewStringObj("argument list length doesn't match number of arguments for query",
											  -1));
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
								  pltcl_current_call_state->prodesc->fn_readonly,
								  count);

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
 * pltcl_subtransaction()	- Execute some Tcl code in a subtransaction
 *
 * The subtransaction is aborted if the Tcl code fragment returns TCL_ERROR,
 * otherwise it's subcommitted.
 **********************************************************************/
static int
pltcl_subtransaction(ClientData cdata, Tcl_Interp *interp,
					 int objc, Tcl_Obj *const objv[])
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	int			retcode;

	if (objc != 2)
	{
		Tcl_WrongNumArgs(interp, 1, objv, "command");
		return TCL_ERROR;
	}

	/*
	 * Note: we don't use pltcl_subtrans_begin and friends here because we
	 * don't want the error handling in pltcl_subtrans_abort.  But otherwise
	 * the processing should be about the same as in those functions.
	 */
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	retcode = Tcl_EvalObjEx(interp, objv[1], 0);

	if (retcode == TCL_ERROR)
	{
		/* Rollback the subtransaction */
		RollbackAndReleaseCurrentSubTransaction();
	}
	else
	{
		/* Commit the subtransaction */
		ReleaseCurrentSubTransaction();
	}

	/* In either case, restore previous memory context and resource owner */
	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = oldowner;

	return retcode;
}


/**********************************************************************
 * pltcl_commit()
 *
 * Commit the transaction and start a new one.
 **********************************************************************/
static int
pltcl_commit(ClientData cdata, Tcl_Interp *interp,
			 int objc, Tcl_Obj *const objv[])
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		SPI_commit();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Pass the error data to Tcl */
		pltcl_construct_errorCode(interp, edata);
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
 * pltcl_rollback()
 *
 * Abort the transaction and start a new one.
 **********************************************************************/
static int
pltcl_rollback(ClientData cdata, Tcl_Interp *interp,
			   int objc, Tcl_Obj *const objv[])
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		SPI_rollback();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Pass the error data to Tcl */
		pltcl_construct_errorCode(interp, edata);
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
 * pltcl_set_tuple_values() - Set variables for all attributes
 *				  of a given tuple
 *
 * Note: arrayname is presumed to be UTF8; it usually came from Tcl
 **********************************************************************/
static void
pltcl_set_tuple_values(Tcl_Interp *interp, const char *arrayname,
					   uint64 tupno, HeapTuple tuple, TupleDesc tupdesc)
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
	 * Prepare pointers for Tcl_SetVar2Ex() below
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

		/*
		 * When outputting to an array, fill the ".tupno" element with the
		 * current tuple number.  This will be overridden below if ".tupno" is
		 * in use as an actual field name in the rowtype.
		 */
		Tcl_SetVar2Ex(interp, arrayname, ".tupno", Tcl_NewWideIntObj(tupno), 0);
	}

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/* ignore dropped attributes */
		if (att->attisdropped)
			continue;

		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		UTF_BEGIN;
		attname = pstrdup(UTF_E2U(NameStr(att->attname)));
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
			getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
			outputstr = OidOutputFunctionCall(typoutput, attr);
			UTF_BEGIN;
			Tcl_SetVar2Ex(interp, *arrptr, *nameptr,
						  Tcl_NewStringObj(UTF_E2U(outputstr), -1), 0);
			UTF_END;
			pfree(outputstr);
		}
		else
			Tcl_UnsetVar2(interp, *arrptr, *nameptr, 0);

		pfree(unconstify(char *, attname));
	}
}


/**********************************************************************
 * pltcl_build_tuple_argument() - Build a list object usable for 'array set'
 *				  from all attributes of a given tuple
 **********************************************************************/
static Tcl_Obj *
pltcl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc, bool include_generated)
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
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/* ignore dropped attributes */
		if (att->attisdropped)
			continue;

		if (att->attgenerated)
		{
			/* don't include unless requested */
			if (!include_generated)
				continue;
		}

		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		attname = NameStr(att->attname);

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
			getTypeOutputInfo(att->atttypid,
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

/**********************************************************************
 * pltcl_build_tuple_result() - Build a tuple of function's result rowtype
 *				  from a Tcl list of column names and values
 *
 * In a trigger function, we build a tuple of the trigger table's rowtype.
 *
 * Note: this function leaks memory.  Even if we made it clean up its own
 * mess, there's no way to prevent the datatype input functions it calls
 * from leaking.  Run it in a short-lived context, unless we're about to
 * exit the procedure anyway.
 **********************************************************************/
static HeapTuple
pltcl_build_tuple_result(Tcl_Interp *interp, Tcl_Obj **kvObjv, int kvObjc,
						 pltcl_call_state *call_state)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;
	char	  **values;
	int			i;

	if (call_state->ret_tupdesc)
	{
		tupdesc = call_state->ret_tupdesc;
		attinmeta = call_state->attinmeta;
	}
	else if (call_state->trigdata)
	{
		tupdesc = RelationGetDescr(call_state->trigdata->tg_relation);
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
	}
	else
	{
		elog(ERROR, "PL/Tcl function does not return a tuple");
		tupdesc = NULL;			/* keep compiler quiet */
		attinmeta = NULL;
	}

	values = (char **) palloc0(tupdesc->natts * sizeof(char *));

	if (kvObjc % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column name/value list must have even number of elements")));

	for (i = 0; i < kvObjc; i += 2)
	{
		char	   *fieldName = utf_u2e(Tcl_GetString(kvObjv[i]));
		int			attn = SPI_fnumber(tupdesc, fieldName);

		/*
		 * We silently ignore ".tupno", if it's present but doesn't match any
		 * actual output column.  This allows direct use of a row returned by
		 * pltcl_set_tuple_values().
		 */
		if (attn == SPI_ERROR_NOATTRIBUTE)
		{
			if (strcmp(fieldName, ".tupno") == 0)
				continue;
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column name/value list contains nonexistent column name \"%s\"",
							fieldName)));
		}

		if (attn <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot set system attribute \"%s\"",
							fieldName)));

		if (TupleDescAttr(tupdesc, attn - 1)->attgenerated)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("cannot set generated column \"%s\"",
							fieldName)));

		values[attn - 1] = utf_u2e(Tcl_GetString(kvObjv[i + 1]));
	}

	tuple = BuildTupleFromCStrings(attinmeta, values);

	/* if result type is domain-over-composite, check domain constraints */
	if (call_state->prodesc->fn_retisdomain)
		domain_check(HeapTupleGetDatum(tuple), false,
					 call_state->prodesc->result_typid,
					 &call_state->prodesc->domain_info,
					 call_state->prodesc->fn_cxt);

	return tuple;
}

/**********************************************************************
 * pltcl_init_tuple_store() - Initialize the result tuplestore for a SRF
 **********************************************************************/
static void
pltcl_init_tuple_store(pltcl_call_state *call_state)
{
	ReturnSetInfo *rsi = call_state->rsi;
	MemoryContext oldcxt;
	ResourceOwner oldowner;

	/* Should be in a SRF */
	Assert(rsi);
	/* Should be first time through */
	Assert(!call_state->tuple_store);
	Assert(!call_state->attinmeta);

	/* We expect caller to provide an appropriate result tupdesc */
	Assert(rsi->expectedDesc);
	call_state->ret_tupdesc = rsi->expectedDesc;

	/*
	 * Switch to the right memory context and resource owner for storing the
	 * tuplestore. If we're within a subtransaction opened for an exception
	 * block, for example, we must still create the tuplestore in the resource
	 * owner that was active when this function was entered, and not in the
	 * subtransaction's resource owner.
	 */
	oldcxt = MemoryContextSwitchTo(call_state->tuple_store_cxt);
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = call_state->tuple_store_owner;

	call_state->tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	/* Build attinmeta in this context, too */
	call_state->attinmeta = TupleDescGetAttInMetadata(call_state->ret_tupdesc);

	CurrentResourceOwner = oldowner;
	MemoryContextSwitchTo(oldcxt);
}
