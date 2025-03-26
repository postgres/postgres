/**********************************************************************
 * plperl.c - perl as a procedural language for PostgreSQL
 *
 *	  src/pl/plperl/plperl.c
 *
 **********************************************************************/

#include "postgres.h"

/* system stuff */
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

/* postgreSQL stuff */
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plperl")

/* perl stuff */
/* string literal macros defining chunks of perl code */
#include "perlchunks.h"
#include "plperl.h"
/* defines PLPERL_SET_OPMASK */
#include "plperl_opmask.h"

EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
EXTERN_C void boot_PostgreSQL__InServer__Util(pTHX_ CV *cv);
EXTERN_C void boot_PostgreSQL__InServer__SPI(pTHX_ CV *cv);

PG_MODULE_MAGIC_EXT(
					.name = "plperl",
					.version = PG_VERSION
);

/**********************************************************************
 * Information associated with a Perl interpreter.  We have one interpreter
 * that is used for all plperlu (untrusted) functions.  For plperl (trusted)
 * functions, there is a separate interpreter for each effective SQL userid.
 * (This is needed to ensure that an unprivileged user can't inject Perl code
 * that'll be executed with the privileges of some other SQL user.)
 *
 * The plperl_interp_desc structs are kept in a Postgres hash table indexed
 * by userid OID, with OID 0 used for the single untrusted interpreter.
 * Once created, an interpreter is kept for the life of the process.
 *
 * We start out by creating a "held" interpreter, which we initialize
 * only as far as we can do without deciding if it will be trusted or
 * untrusted.  Later, when we first need to run a plperl or plperlu
 * function, we complete the initialization appropriately and move the
 * PerlInterpreter pointer into the plperl_interp_hash hashtable.  If after
 * that we need more interpreters, we create them as needed if we can, or
 * fail if the Perl build doesn't support multiple interpreters.
 *
 * The reason for all the dancing about with a held interpreter is to make
 * it possible for people to preload a lot of Perl code at postmaster startup
 * (using plperl.on_init) and then use that code in backends.  Of course this
 * will only work for the first interpreter created in any backend, but it's
 * still useful with that restriction.
 **********************************************************************/
typedef struct plperl_interp_desc
{
	Oid			user_id;		/* Hash key (must be first!) */
	PerlInterpreter *interp;	/* The interpreter */
	HTAB	   *query_hash;		/* plperl_query_entry structs */
} plperl_interp_desc;


/**********************************************************************
 * The information we cache about loaded procedures
 *
 * The fn_refcount field counts the struct's reference from the hash table
 * shown below, plus one reference for each function call level that is using
 * the struct.  We can release the struct, and the associated Perl sub, when
 * the fn_refcount goes to zero.  Releasing the struct itself is done by
 * deleting the fn_cxt, which also gets rid of all subsidiary data.
 **********************************************************************/
typedef struct plperl_proc_desc
{
	char	   *proname;		/* user name of procedure */
	MemoryContext fn_cxt;		/* memory context for this procedure */
	unsigned long fn_refcount;	/* number of active references */
	TransactionId fn_xmin;		/* xmin/TID of procedure's pg_proc tuple */
	ItemPointerData fn_tid;
	SV		   *reference;		/* CODE reference for Perl sub */
	plperl_interp_desc *interp; /* interpreter it's created in */
	bool		fn_readonly;	/* is function readonly (not volatile)? */
	Oid			lang_oid;
	List	   *trftypes;
	bool		lanpltrusted;	/* is it plperl, rather than plperlu? */
	bool		fn_retistuple;	/* true, if function returns tuple */
	bool		fn_retisset;	/* true, if function returns set */
	bool		fn_retisarray;	/* true if function returns array */
	/* Conversion info for function's result type: */
	Oid			result_oid;		/* Oid of result type */
	FmgrInfo	result_in_func; /* I/O function and arg for result type */
	Oid			result_typioparam;
	/* Per-argument info for function's argument types: */
	int			nargs;
	FmgrInfo   *arg_out_func;	/* output fns for arg types */
	bool	   *arg_is_rowtype; /* is each arg composite? */
	Oid		   *arg_arraytype;	/* InvalidOid if not an array */
} plperl_proc_desc;

#define increment_prodesc_refcount(prodesc)  \
	((prodesc)->fn_refcount++)
#define decrement_prodesc_refcount(prodesc)  \
	do { \
		Assert((prodesc)->fn_refcount > 0); \
		if (--((prodesc)->fn_refcount) == 0) \
			free_plperl_function(prodesc); \
	} while(0)

/**********************************************************************
 * For speedy lookup, we maintain a hash table mapping from
 * function OID + trigger flag + user OID to plperl_proc_desc pointers.
 * The reason the plperl_proc_desc struct isn't directly part of the hash
 * entry is to simplify recovery from errors during compile_plperl_function.
 *
 * Note: if the same function is called by multiple userIDs within a session,
 * there will be a separate plperl_proc_desc entry for each userID in the case
 * of plperl functions, but only one entry for plperlu functions, because we
 * set user_id = 0 for that case.  If the user redeclares the same function
 * from plperl to plperlu or vice versa, there might be multiple
 * plperl_proc_ptr entries in the hashtable, but only one is valid.
 **********************************************************************/
typedef struct plperl_proc_key
{
	Oid			proc_id;		/* Function OID */

	/*
	 * is_trigger is really a bool, but declare as Oid to ensure this struct
	 * contains no padding
	 */
	Oid			is_trigger;		/* is it a trigger function? */
	Oid			user_id;		/* User calling the function, or 0 */
} plperl_proc_key;

typedef struct plperl_proc_ptr
{
	plperl_proc_key proc_key;	/* Hash key (must be first!) */
	plperl_proc_desc *proc_ptr;
} plperl_proc_ptr;

/*
 * The information we cache for the duration of a single call to a
 * function.
 */
typedef struct plperl_call_data
{
	plperl_proc_desc *prodesc;
	FunctionCallInfo fcinfo;
	/* remaining fields are used only in a function returning set: */
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	Oid			cdomain_oid;	/* 0 unless returning domain-over-composite */
	void	   *cdomain_info;
	MemoryContext tmp_cxt;
} plperl_call_data;

/**********************************************************************
 * The information we cache about prepared and saved plans
 **********************************************************************/
typedef struct plperl_query_desc
{
	char		qname[24];
	MemoryContext plan_cxt;		/* context holding this struct */
	SPIPlanPtr	plan;
	int			nargs;
	Oid		   *argtypes;
	FmgrInfo   *arginfuncs;
	Oid		   *argtypioparams;
} plperl_query_desc;

/* hash table entry for query desc	*/

typedef struct plperl_query_entry
{
	char		query_name[NAMEDATALEN];
	plperl_query_desc *query_data;
} plperl_query_entry;

/**********************************************************************
 * Information for PostgreSQL - Perl array conversion.
 **********************************************************************/
typedef struct plperl_array_info
{
	int			ndims;
	bool		elem_is_rowtype;	/* 't' if element type is a rowtype */
	Datum	   *elements;
	bool	   *nulls;
	int		   *nelems;
	FmgrInfo	proc;
	FmgrInfo	transform_proc;
} plperl_array_info;

/**********************************************************************
 * Global data
 **********************************************************************/

static HTAB *plperl_interp_hash = NULL;
static HTAB *plperl_proc_hash = NULL;
static plperl_interp_desc *plperl_active_interp = NULL;

/* If we have an unassigned "held" interpreter, it's stored here */
static PerlInterpreter *plperl_held_interp = NULL;

/* GUC variables */
static bool plperl_use_strict = false;
static char *plperl_on_init = NULL;
static char *plperl_on_plperl_init = NULL;
static char *plperl_on_plperlu_init = NULL;

static bool plperl_ending = false;
static OP  *(*pp_require_orig) (pTHX) = NULL;
static char plperl_opmask[MAXO];

/* this is saved and restored by plperl_call_handler */
static plperl_call_data *current_call_data = NULL;

/**********************************************************************
 * Forward declarations
 **********************************************************************/

static PerlInterpreter *plperl_init_interp(void);
static void plperl_destroy_interp(PerlInterpreter **);
static void plperl_fini(int code, Datum arg);
static void set_interp_require(bool trusted);

static Datum plperl_func_handler(PG_FUNCTION_ARGS);
static Datum plperl_trigger_handler(PG_FUNCTION_ARGS);
static void plperl_event_trigger_handler(PG_FUNCTION_ARGS);

static void free_plperl_function(plperl_proc_desc *prodesc);

static plperl_proc_desc *compile_plperl_function(Oid fn_oid,
												 bool is_trigger,
												 bool is_event_trigger);

static SV  *plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc, bool include_generated);
static SV  *plperl_hash_from_datum(Datum attr);
static void check_spi_usage_allowed(void);
static SV  *plperl_ref_from_pg_array(Datum arg, Oid typid);
static SV  *split_array(plperl_array_info *info, int first, int last, int nest);
static SV  *make_array_ref(plperl_array_info *info, int first, int last);
static SV  *get_perl_array_ref(SV *sv);
static Datum plperl_sv_to_datum(SV *sv, Oid typid, int32 typmod,
								FunctionCallInfo fcinfo,
								FmgrInfo *finfo, Oid typioparam,
								bool *isnull);
static void _sv_to_datum_finfo(Oid typid, FmgrInfo *finfo, Oid *typioparam);
static Datum plperl_array_to_datum(SV *src, Oid typid, int32 typmod);
static void array_to_datum_internal(AV *av, ArrayBuildState **astatep,
									int *ndims, int *dims, int cur_depth,
									Oid elemtypid, int32 typmod,
									FmgrInfo *finfo, Oid typioparam);
static Datum plperl_hash_to_datum(SV *src, TupleDesc td);

static void plperl_init_shared_libs(pTHX);
static void plperl_trusted_init(void);
static void plperl_untrusted_init(void);
static HV  *plperl_spi_execute_fetch_result(SPITupleTable *, uint64, int);
static void plperl_return_next_internal(SV *sv);
static char *hek2cstr(HE *he);
static SV **hv_store_string(HV *hv, const char *key, SV *val);
static SV **hv_fetch_string(HV *hv, const char *key);
static void plperl_create_sub(plperl_proc_desc *desc, const char *s, Oid fn_oid);
static SV  *plperl_call_perl_func(plperl_proc_desc *desc,
								  FunctionCallInfo fcinfo);
static void plperl_compile_callback(void *arg);
static void plperl_exec_callback(void *arg);
static void plperl_inline_callback(void *arg);
static char *strip_trailing_ws(const char *msg);
static OP  *pp_require_safe(pTHX);
static void activate_interpreter(plperl_interp_desc *interp_desc);

#if defined(WIN32) && PERL_VERSION_LT(5, 28, 0)
static char *setlocale_perl(int category, char *locale);
#else
#define setlocale_perl(a,b)  Perl_setlocale(a,b)
#endif							/* defined(WIN32) && PERL_VERSION_LT(5, 28, 0) */

/*
 * Decrement the refcount of the given SV within the active Perl interpreter
 *
 * This is handy because it reloads the active-interpreter pointer, saving
 * some notation in callers that switch the active interpreter.
 */
static inline void
SvREFCNT_dec_current(SV *sv)
{
	dTHX;

	SvREFCNT_dec(sv);
}

/*
 * convert a HE (hash entry) key to a cstr in the current database encoding
 */
static char *
hek2cstr(HE *he)
{
	dTHX;
	char	   *ret;
	SV		   *sv;

	/*
	 * HeSVKEY_force will return a temporary mortal SV*, so we need to make
	 * sure to free it with ENTER/SAVE/FREE/LEAVE
	 */
	ENTER;
	SAVETMPS;

	/*-------------------------
	 * Unfortunately, while HeUTF8 is true for most things > 256, for values
	 * 128..255 it's not, but perl will treat them as unicode code points if
	 * the utf8 flag is not set ( see The "Unicode Bug" in perldoc perlunicode
	 * for more)
	 *
	 * So if we did the expected:
	 *	  if (HeUTF8(he))
	 *		  utf_u2e(key...);
	 *	  else // must be ascii
	 *		  return HePV(he);
	 * we won't match columns with codepoints from 128..255
	 *
	 * For a more concrete example given a column with the name of the unicode
	 * codepoint U+00ae (registered sign) and a UTF8 database and the perl
	 * return_next { "\N{U+00ae}=>'text } would always fail as heUTF8 returns
	 * 0 and HePV() would give us a char * with 1 byte contains the decimal
	 * value 174
	 *
	 * Perl has the brains to know when it should utf8 encode 174 properly, so
	 * here we force it into an SV so that perl will figure it out and do the
	 * right thing
	 *-------------------------
	 */

	sv = HeSVKEY_force(he);
	if (HeUTF8(he))
		SvUTF8_on(sv);
	ret = sv2cstr(sv);

	/* free sv */
	FREETMPS;
	LEAVE;

	return ret;
}


/*
 * _PG_init()			- library load-time initialization
 *
 * DO NOT make this static nor change its name!
 */
void
_PG_init(void)
{
	/*
	 * Be sure we do initialization only once.
	 *
	 * If initialization fails due to, e.g., plperl_init_interp() throwing an
	 * exception, then we'll return here on the next usage and the user will
	 * get a rather cryptic: ERROR:  attempt to redefine parameter
	 * "plperl.use_strict"
	 */
	static bool inited = false;
	HASHCTL		hash_ctl;

	if (inited)
		return;

	/*
	 * Support localized messages.
	 */
	pg_bindtextdomain(TEXTDOMAIN);

	/*
	 * Initialize plperl's GUCs.
	 */
	DefineCustomBoolVariable("plperl.use_strict",
							 gettext_noop("If true, trusted and untrusted Perl code will be compiled in strict mode."),
							 NULL,
							 &plperl_use_strict,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	/*
	 * plperl.on_init is marked PGC_SIGHUP to support the idea that it might
	 * be executed in the postmaster (if plperl is loaded into the postmaster
	 * via shared_preload_libraries).  This isn't really right either way,
	 * though.
	 */
	DefineCustomStringVariable("plperl.on_init",
							   gettext_noop("Perl initialization code to execute when a Perl interpreter is initialized."),
							   NULL,
							   &plperl_on_init,
							   NULL,
							   PGC_SIGHUP, 0,
							   NULL, NULL, NULL);

	/*
	 * plperl.on_plperl_init is marked PGC_SUSET to avoid issues whereby a
	 * user who might not even have USAGE privilege on the plperl language
	 * could nonetheless use SET plperl.on_plperl_init='...' to influence the
	 * behaviour of any existing plperl function that they can execute (which
	 * might be SECURITY DEFINER, leading to a privilege escalation).  See
	 * http://archives.postgresql.org/pgsql-hackers/2010-02/msg00281.php and
	 * the overall thread.
	 *
	 * Note that because plperl.use_strict is USERSET, a nefarious user could
	 * set it to be applied against other people's functions.  This is judged
	 * OK since the worst result would be an error.  Your code oughta pass
	 * use_strict anyway ;-)
	 */
	DefineCustomStringVariable("plperl.on_plperl_init",
							   gettext_noop("Perl initialization code to execute once when plperl is first used."),
							   NULL,
							   &plperl_on_plperl_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("plperl.on_plperlu_init",
							   gettext_noop("Perl initialization code to execute once when plperlu is first used."),
							   NULL,
							   &plperl_on_plperlu_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("plperl");

	/*
	 * Create hash tables.
	 */
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(plperl_interp_desc);
	plperl_interp_hash = hash_create("PL/Perl interpreters",
									 8,
									 &hash_ctl,
									 HASH_ELEM | HASH_BLOBS);

	hash_ctl.keysize = sizeof(plperl_proc_key);
	hash_ctl.entrysize = sizeof(plperl_proc_ptr);
	plperl_proc_hash = hash_create("PL/Perl procedures",
								   32,
								   &hash_ctl,
								   HASH_ELEM | HASH_BLOBS);

	/*
	 * Save the default opmask.
	 */
	PLPERL_SET_OPMASK(plperl_opmask);

	/*
	 * Create the first Perl interpreter, but only partially initialize it.
	 */
	plperl_held_interp = plperl_init_interp();

	inited = true;
}


static void
set_interp_require(bool trusted)
{
	if (trusted)
	{
		PL_ppaddr[OP_REQUIRE] = pp_require_safe;
		PL_ppaddr[OP_DOFILE] = pp_require_safe;
	}
	else
	{
		PL_ppaddr[OP_REQUIRE] = pp_require_orig;
		PL_ppaddr[OP_DOFILE] = pp_require_orig;
	}
}

/*
 * Cleanup perl interpreters, including running END blocks.
 * Does not fully undo the actions of _PG_init() nor make it callable again.
 */
static void
plperl_fini(int code, Datum arg)
{
	HASH_SEQ_STATUS hash_seq;
	plperl_interp_desc *interp_desc;

	elog(DEBUG3, "plperl_fini");

	/*
	 * Indicate that perl is terminating. Disables use of spi_* functions when
	 * running END/DESTROY code. See check_spi_usage_allowed(). Could be
	 * enabled in future, with care, using a transaction
	 * http://archives.postgresql.org/pgsql-hackers/2010-01/msg02743.php
	 */
	plperl_ending = true;

	/* Only perform perl cleanup if we're exiting cleanly */
	if (code)
	{
		elog(DEBUG3, "plperl_fini: skipped");
		return;
	}

	/* Zap the "held" interpreter, if we still have it */
	plperl_destroy_interp(&plperl_held_interp);

	/* Zap any fully-initialized interpreters */
	hash_seq_init(&hash_seq, plperl_interp_hash);
	while ((interp_desc = hash_seq_search(&hash_seq)) != NULL)
	{
		if (interp_desc->interp)
		{
			activate_interpreter(interp_desc);
			plperl_destroy_interp(&interp_desc->interp);
		}
	}

	elog(DEBUG3, "plperl_fini: done");
}


/*
 * Select and activate an appropriate Perl interpreter.
 */
static void
select_perl_context(bool trusted)
{
	Oid			user_id;
	plperl_interp_desc *interp_desc;
	bool		found;
	PerlInterpreter *interp = NULL;

	/* Find or create the interpreter hashtable entry for this userid */
	if (trusted)
		user_id = GetUserId();
	else
		user_id = InvalidOid;

	interp_desc = hash_search(plperl_interp_hash, &user_id,
							  HASH_ENTER,
							  &found);
	if (!found)
	{
		/* Initialize newly-created hashtable entry */
		interp_desc->interp = NULL;
		interp_desc->query_hash = NULL;
	}

	/* Make sure we have a query_hash for this interpreter */
	if (interp_desc->query_hash == NULL)
	{
		HASHCTL		hash_ctl;

		hash_ctl.keysize = NAMEDATALEN;
		hash_ctl.entrysize = sizeof(plperl_query_entry);
		interp_desc->query_hash = hash_create("PL/Perl queries",
											  32,
											  &hash_ctl,
											  HASH_ELEM | HASH_STRINGS);
	}

	/*
	 * Quick exit if already have an interpreter
	 */
	if (interp_desc->interp)
	{
		activate_interpreter(interp_desc);
		return;
	}

	/*
	 * adopt held interp if free, else create new one if possible
	 */
	if (plperl_held_interp != NULL)
	{
		/* first actual use of a perl interpreter */
		interp = plperl_held_interp;

		/*
		 * Reset the plperl_held_interp pointer first; if we fail during init
		 * we don't want to try again with the partially-initialized interp.
		 */
		plperl_held_interp = NULL;

		if (trusted)
			plperl_trusted_init();
		else
			plperl_untrusted_init();

		/* successfully initialized, so arrange for cleanup */
		on_proc_exit(plperl_fini, 0);
	}
	else
	{
#ifdef MULTIPLICITY

		/*
		 * plperl_init_interp will change Perl's idea of the active
		 * interpreter.  Reset plperl_active_interp temporarily, so that if we
		 * hit an error partway through here, we'll make sure to switch back
		 * to a non-broken interpreter before running any other Perl
		 * functions.
		 */
		plperl_active_interp = NULL;

		/* Now build the new interpreter */
		interp = plperl_init_interp();

		if (trusted)
			plperl_trusted_init();
		else
			plperl_untrusted_init();
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot allocate multiple Perl interpreters on this platform")));
#endif
	}

	set_interp_require(trusted);

	/*
	 * Since the timing of first use of PL/Perl can't be predicted, any
	 * database interaction during initialization is problematic. Including,
	 * but not limited to, security definer issues. So we only enable access
	 * to the database AFTER on_*_init code has run. See
	 * http://archives.postgresql.org/pgsql-hackers/2010-01/msg02669.php
	 */
	{
		dTHX;

		newXS("PostgreSQL::InServer::SPI::bootstrap",
			  boot_PostgreSQL__InServer__SPI, __FILE__);

		eval_pv("PostgreSQL::InServer::SPI::bootstrap()", FALSE);
		if (SvTRUE(ERRSV))
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV))),
					 errcontext("while executing PostgreSQL::InServer::SPI::bootstrap")));
	}

	/* Fully initialized, so mark the hashtable entry valid */
	interp_desc->interp = interp;

	/* And mark this as the active interpreter */
	plperl_active_interp = interp_desc;
}

/*
 * Make the specified interpreter the active one
 *
 * A call with NULL does nothing.  This is so that "restoring" to a previously
 * null state of plperl_active_interp doesn't result in useless thrashing.
 */
static void
activate_interpreter(plperl_interp_desc *interp_desc)
{
	if (interp_desc && plperl_active_interp != interp_desc)
	{
		Assert(interp_desc->interp);
		PERL_SET_CONTEXT(interp_desc->interp);
		/* trusted iff user_id isn't InvalidOid */
		set_interp_require(OidIsValid(interp_desc->user_id));
		plperl_active_interp = interp_desc;
	}
}

/*
 * Create a new Perl interpreter.
 *
 * We initialize the interpreter as far as we can without knowing whether
 * it will become a trusted or untrusted interpreter; in particular, the
 * plperl.on_init code will get executed.  Later, either plperl_trusted_init
 * or plperl_untrusted_init must be called to complete the initialization.
 */
static PerlInterpreter *
plperl_init_interp(void)
{
	PerlInterpreter *plperl;

	static char *embedding[3 + 2] = {
		"", "-e", PLC_PERLBOOT
	};
	int			nargs = 3;

#ifdef WIN32

	/*
	 * The perl library on startup does horrible things like call
	 * setlocale(LC_ALL,""). We have protected against that on most platforms
	 * by setting the environment appropriately. However, on Windows,
	 * setlocale() does not consult the environment, so we need to save the
	 * existing locale settings before perl has a chance to mangle them and
	 * restore them after its dirty deeds are done.
	 *
	 * MSDN ref:
	 * http://msdn.microsoft.com/library/en-us/vclib/html/_crt_locale.asp
	 *
	 * It appears that we only need to do this on interpreter startup, and
	 * subsequent calls to the interpreter don't mess with the locale
	 * settings.
	 *
	 * We restore them using setlocale_perl(), defined below, so that Perl
	 * doesn't have a different idea of the locale from Postgres.
	 *
	 */

	char	   *loc;
	char	   *save_collate,
			   *save_ctype,
			   *save_monetary,
			   *save_numeric,
			   *save_time;

	loc = setlocale(LC_COLLATE, NULL);
	save_collate = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_CTYPE, NULL);
	save_ctype = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_MONETARY, NULL);
	save_monetary = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_NUMERIC, NULL);
	save_numeric = loc ? pstrdup(loc) : NULL;
	loc = setlocale(LC_TIME, NULL);
	save_time = loc ? pstrdup(loc) : NULL;

#define PLPERL_RESTORE_LOCALE(name, saved) \
	STMT_START { \
		if (saved != NULL) { setlocale_perl(name, saved); pfree(saved); } \
	} STMT_END
#endif							/* WIN32 */

	if (plperl_on_init && *plperl_on_init)
	{
		embedding[nargs++] = "-e";
		embedding[nargs++] = plperl_on_init;
	}

	/*
	 * The perl API docs state that PERL_SYS_INIT3 should be called before
	 * allocating interpreters. Unfortunately, on some platforms this fails in
	 * the Perl_do_taint() routine, which is called when the platform is using
	 * the system's malloc() instead of perl's own. Other platforms, notably
	 * Windows, fail if PERL_SYS_INIT3 is not called. So we call it if it's
	 * available, unless perl is using the system malloc(), which is true when
	 * MYMALLOC is set.
	 */
#if defined(PERL_SYS_INIT3) && !defined(MYMALLOC)
	{
		static int	perl_sys_init_done;

		/* only call this the first time through, as per perlembed man page */
		if (!perl_sys_init_done)
		{
			char	   *dummy_env[1] = {NULL};

			PERL_SYS_INIT3(&nargs, (char ***) &embedding, (char ***) &dummy_env);

			/*
			 * For unclear reasons, PERL_SYS_INIT3 sets the SIGFPE handler to
			 * SIG_IGN.  Aside from being extremely unfriendly behavior for a
			 * library, this is dumb on the grounds that the results of a
			 * SIGFPE in this state are undefined according to POSIX, and in
			 * fact you get a forced process kill at least on Linux.  Hence,
			 * restore the SIGFPE handler to the backend's standard setting.
			 * (See Perl bug 114574 for more information.)
			 */
			pqsignal(SIGFPE, FloatExceptionHandler);

			perl_sys_init_done = 1;
			/* quiet warning if PERL_SYS_INIT3 doesn't use the third argument */
			dummy_env[0] = NULL;
		}
	}
#endif

	plperl = perl_alloc();
	if (!plperl)
		elog(ERROR, "could not allocate Perl interpreter");

	PERL_SET_CONTEXT(plperl);
	perl_construct(plperl);

	/*
	 * Run END blocks in perl_destruct instead of perl_run.  Note that dTHX
	 * loads up a pointer to the current interpreter, so we have to postpone
	 * it to here rather than put it at the function head.
	 */
	{
		dTHX;

		PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

		/*
		 * Record the original function for the 'require' and 'dofile'
		 * opcodes.  (They share the same implementation.)  Ensure it's used
		 * for new interpreters.
		 */
		if (!pp_require_orig)
			pp_require_orig = PL_ppaddr[OP_REQUIRE];
		else
		{
			PL_ppaddr[OP_REQUIRE] = pp_require_orig;
			PL_ppaddr[OP_DOFILE] = pp_require_orig;
		}

#ifdef PLPERL_ENABLE_OPMASK_EARLY

		/*
		 * For regression testing to prove that the PLC_PERLBOOT and
		 * PLC_TRUSTED code doesn't even compile any unsafe ops.  In future
		 * there may be a valid need for them to do so, in which case this
		 * could be softened (perhaps moved to plperl_trusted_init()) or
		 * removed.
		 */
		PL_op_mask = plperl_opmask;
#endif

		if (perl_parse(plperl, plperl_init_shared_libs,
					   nargs, embedding, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV))),
					 errcontext("while parsing Perl initialization")));

		if (perl_run(plperl) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV))),
					 errcontext("while running Perl initialization")));

#ifdef PLPERL_RESTORE_LOCALE
		PLPERL_RESTORE_LOCALE(LC_COLLATE, save_collate);
		PLPERL_RESTORE_LOCALE(LC_CTYPE, save_ctype);
		PLPERL_RESTORE_LOCALE(LC_MONETARY, save_monetary);
		PLPERL_RESTORE_LOCALE(LC_NUMERIC, save_numeric);
		PLPERL_RESTORE_LOCALE(LC_TIME, save_time);
#endif
	}

	return plperl;
}


/*
 * Our safe implementation of the require opcode.
 * This is safe because it's completely unable to load any code.
 * If the requested file/module has already been loaded it'll return true.
 * If not, it'll die.
 * So now "use Foo;" will work iff Foo has already been loaded.
 */
static OP  *
pp_require_safe(pTHX)
{
	dVAR;
	dSP;
	SV		   *sv,
			  **svp;
	char	   *name;
	STRLEN		len;

	sv = POPs;
	name = SvPV(sv, len);
	if (!(name && len > 0 && *name))
		RETPUSHNO;

	svp = hv_fetch(GvHVn(PL_incgv), name, len, 0);
	if (svp && *svp != &PL_sv_undef)
		RETPUSHYES;

	DIE(aTHX_ "Unable to load %s into plperl", name);

	/*
	 * In most Perl versions, DIE() expands to a return statement, so the next
	 * line is not necessary.  But in versions between but not including
	 * 5.11.1 and 5.13.3 it does not, so the next line is necessary to avoid a
	 * "control reaches end of non-void function" warning from gcc.  Other
	 * compilers such as Solaris Studio will, however, issue a "statement not
	 * reached" warning instead.
	 */
	return NULL;
}


/*
 * Destroy one Perl interpreter ... actually we just run END blocks.
 *
 * Caller must have ensured this interpreter is the active one.
 */
static void
plperl_destroy_interp(PerlInterpreter **interp)
{
	if (interp && *interp)
	{
		/*
		 * Only a very minimal destruction is performed: - just call END
		 * blocks.
		 *
		 * We could call perl_destruct() but we'd need to audit its actions
		 * very carefully and work-around any that impact us. (Calling
		 * sv_clean_objs() isn't an option because it's not part of perl's
		 * public API so isn't portably available.) Meanwhile END blocks can
		 * be used to perform manual cleanup.
		 */
		dTHX;

		/* Run END blocks - based on perl's perl_destruct() */
		if (PL_exit_flags & PERL_EXIT_DESTRUCT_END)
		{
			dJMPENV;
			int			x = 0;

			JMPENV_PUSH(x);
			PERL_UNUSED_VAR(x);
			if (PL_endav && !PL_minus_c)
				call_list(PL_scopestack_ix, PL_endav);
			JMPENV_POP;
		}
		LEAVE;
		FREETMPS;

		*interp = NULL;
	}
}

/*
 * Initialize the current Perl interpreter as a trusted interp
 */
static void
plperl_trusted_init(void)
{
	dTHX;
	HV		   *stash;
	SV		   *sv;
	char	   *key;
	I32			klen;

	/* use original require while we set up */
	PL_ppaddr[OP_REQUIRE] = pp_require_orig;
	PL_ppaddr[OP_DOFILE] = pp_require_orig;

	eval_pv(PLC_TRUSTED, FALSE);
	if (SvTRUE(ERRSV))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV))),
				 errcontext("while executing PLC_TRUSTED")));

	/*
	 * Force loading of utf8 module now to prevent errors that can arise from
	 * the regex code later trying to load utf8 modules. See
	 * http://rt.perl.org/rt3/Ticket/Display.html?id=47576
	 */
	eval_pv("my $a=chr(0x100); return $a =~ /\\xa9/i", FALSE);
	if (SvTRUE(ERRSV))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV))),
				 errcontext("while executing utf8fix")));

	/*
	 * Lock down the interpreter
	 */

	/* switch to the safe require/dofile opcode for future code */
	PL_ppaddr[OP_REQUIRE] = pp_require_safe;
	PL_ppaddr[OP_DOFILE] = pp_require_safe;

	/*
	 * prevent (any more) unsafe opcodes being compiled PL_op_mask is per
	 * interpreter, so this only needs to be set once
	 */
	PL_op_mask = plperl_opmask;

	/* delete the DynaLoader:: namespace so extensions can't be loaded */
	stash = gv_stashpv("DynaLoader", GV_ADDWARN);
	hv_iterinit(stash);
	while ((sv = hv_iternextsv(stash, &key, &klen)))
	{
		if (!isGV_with_GP(sv) || !GvCV(sv))
			continue;
		SvREFCNT_dec(GvCV(sv)); /* free the CV */
		GvCV_set(sv, NULL);		/* prevent call via GV */
	}
	hv_clear(stash);

	/* invalidate assorted caches */
	++PL_sub_generation;
	hv_clear(PL_stashcache);

	/*
	 * Execute plperl.on_plperl_init in the locked-down interpreter
	 */
	if (plperl_on_plperl_init && *plperl_on_plperl_init)
	{
		eval_pv(plperl_on_plperl_init, FALSE);
		/* XXX need to find a way to determine a better errcode here */
		if (SvTRUE(ERRSV))
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV))),
					 errcontext("while executing plperl.on_plperl_init")));
	}
}


/*
 * Initialize the current Perl interpreter as an untrusted interp
 */
static void
plperl_untrusted_init(void)
{
	dTHX;

	/*
	 * Nothing to do except execute plperl.on_plperlu_init
	 */
	if (plperl_on_plperlu_init && *plperl_on_plperlu_init)
	{
		eval_pv(plperl_on_plperlu_init, FALSE);
		if (SvTRUE(ERRSV))
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV))),
					 errcontext("while executing plperl.on_plperlu_init")));
	}
}


/*
 * Perl likes to put a newline after its error messages; clean up such
 */
static char *
strip_trailing_ws(const char *msg)
{
	char	   *res = pstrdup(msg);
	int			len = strlen(res);

	while (len > 0 && isspace((unsigned char) res[len - 1]))
		res[--len] = '\0';
	return res;
}


/* Build a tuple from a hash. */

static HeapTuple
plperl_build_tuple_result(HV *perlhash, TupleDesc td)
{
	dTHX;
	Datum	   *values;
	bool	   *nulls;
	HE		   *he;
	HeapTuple	tup;

	values = palloc0(sizeof(Datum) * td->natts);
	nulls = palloc(sizeof(bool) * td->natts);
	memset(nulls, true, sizeof(bool) * td->natts);

	hv_iterinit(perlhash);
	while ((he = hv_iternext(perlhash)))
	{
		SV		   *val = HeVAL(he);
		char	   *key = hek2cstr(he);
		int			attn = SPI_fnumber(td, key);
		Form_pg_attribute attr = TupleDescAttr(td, attn - 1);

		if (attn == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("Perl hash contains nonexistent column \"%s\"",
							key)));
		if (attn <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot set system attribute \"%s\"",
							key)));

		values[attn - 1] = plperl_sv_to_datum(val,
											  attr->atttypid,
											  attr->atttypmod,
											  NULL,
											  NULL,
											  InvalidOid,
											  &nulls[attn - 1]);

		pfree(key);
	}
	hv_iterinit(perlhash);

	tup = heap_form_tuple(td, values, nulls);
	pfree(values);
	pfree(nulls);
	return tup;
}

/* convert a hash reference to a datum */
static Datum
plperl_hash_to_datum(SV *src, TupleDesc td)
{
	HeapTuple	tup = plperl_build_tuple_result((HV *) SvRV(src), td);

	return HeapTupleGetDatum(tup);
}

/*
 * if we are an array ref return the reference. this is special in that if we
 * are a PostgreSQL::InServer::ARRAY object we will return the 'magic' array.
 */
static SV  *
get_perl_array_ref(SV *sv)
{
	dTHX;

	if (SvOK(sv) && SvROK(sv))
	{
		if (SvTYPE(SvRV(sv)) == SVt_PVAV)
			return sv;
		else if (sv_isa(sv, "PostgreSQL::InServer::ARRAY"))
		{
			HV		   *hv = (HV *) SvRV(sv);
			SV		  **sav = hv_fetch_string(hv, "array");

			if (*sav && SvOK(*sav) && SvROK(*sav) &&
				SvTYPE(SvRV(*sav)) == SVt_PVAV)
				return *sav;

			elog(ERROR, "could not get array reference from PostgreSQL::InServer::ARRAY object");
		}
	}
	return NULL;
}

/*
 * helper function for plperl_array_to_datum, recurses for multi-D arrays
 *
 * The ArrayBuildState is created only when we first find a scalar element;
 * if we didn't do it like that, we'd need some other convention for knowing
 * whether we'd already found any scalars (and thus the number of dimensions
 * is frozen).
 */
static void
array_to_datum_internal(AV *av, ArrayBuildState **astatep,
						int *ndims, int *dims, int cur_depth,
						Oid elemtypid, int32 typmod,
						FmgrInfo *finfo, Oid typioparam)
{
	dTHX;
	int			i;
	int			len = av_len(av) + 1;

	for (i = 0; i < len; i++)
	{
		/* fetch the array element */
		SV		  **svp = av_fetch(av, i, FALSE);

		/* see if this element is an array, if so get that */
		SV		   *sav = svp ? get_perl_array_ref(*svp) : NULL;

		/* multi-dimensional array? */
		if (sav)
		{
			AV		   *nav = (AV *) SvRV(sav);

			/* set size when at first element in this level, else compare */
			if (i == 0 && *ndims == cur_depth)
			{
				/* array after some scalars at same level? */
				if (*astatep != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("multidimensional arrays must have array expressions with matching dimensions")));
				/* too many dimensions? */
				if (cur_depth + 1 > MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("number of array dimensions exceeds the maximum allowed (%d)",
									MAXDIM)));
				/* OK, add a dimension */
				dims[*ndims] = av_len(nav) + 1;
				(*ndims)++;
			}
			else if (cur_depth >= *ndims ||
					 av_len(nav) + 1 != dims[cur_depth])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("multidimensional arrays must have array expressions with matching dimensions")));

			/* recurse to fetch elements of this sub-array */
			array_to_datum_internal(nav, astatep,
									ndims, dims, cur_depth + 1,
									elemtypid, typmod,
									finfo, typioparam);
		}
		else
		{
			Datum		dat;
			bool		isnull;

			/* scalar after some sub-arrays at same level? */
			if (*ndims != cur_depth)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("multidimensional arrays must have array expressions with matching dimensions")));

			dat = plperl_sv_to_datum(svp ? *svp : NULL,
									 elemtypid,
									 typmod,
									 NULL,
									 finfo,
									 typioparam,
									 &isnull);

			/* Create ArrayBuildState if we didn't already */
			if (*astatep == NULL)
				*astatep = initArrayResult(elemtypid,
										   CurrentMemoryContext, true);

			/* ... and save the element value in it */
			(void) accumArrayResult(*astatep, dat, isnull,
									elemtypid, CurrentMemoryContext);
		}
	}
}

/*
 * convert perl array ref to a datum
 */
static Datum
plperl_array_to_datum(SV *src, Oid typid, int32 typmod)
{
	dTHX;
	AV		   *nav = (AV *) SvRV(src);
	ArrayBuildState *astate = NULL;
	Oid			elemtypid;
	FmgrInfo	finfo;
	Oid			typioparam;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];
	int			ndims = 1;
	int			i;

	elemtypid = get_element_type(typid);
	if (!elemtypid)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot convert Perl array to non-array type %s",
						format_type_be(typid))));

	_sv_to_datum_finfo(elemtypid, &finfo, &typioparam);

	memset(dims, 0, sizeof(dims));
	dims[0] = av_len(nav) + 1;

	array_to_datum_internal(nav, &astate,
							&ndims, dims, 1,
							elemtypid, typmod,
							&finfo, typioparam);

	/* ensure we get zero-D array for no inputs, as per PG convention */
	if (astate == NULL)
		return PointerGetDatum(construct_empty_array(elemtypid));

	for (i = 0; i < ndims; i++)
		lbs[i] = 1;

	return makeMdArrayResult(astate, ndims, dims, lbs,
							 CurrentMemoryContext, true);
}

/* Get the information needed to convert data to the specified PG type */
static void
_sv_to_datum_finfo(Oid typid, FmgrInfo *finfo, Oid *typioparam)
{
	Oid			typinput;

	/* XXX would be better to cache these lookups */
	getTypeInputInfo(typid,
					 &typinput, typioparam);
	fmgr_info(typinput, finfo);
}

/*
 * convert Perl SV to PG datum of type typid, typmod typmod
 *
 * Pass the PL/Perl function's fcinfo when attempting to convert to the
 * function's result type; otherwise pass NULL.  This is used when we need to
 * resolve the actual result type of a function returning RECORD.
 *
 * finfo and typioparam should be the results of _sv_to_datum_finfo for the
 * given typid, or NULL/InvalidOid to let this function do the lookups.
 *
 * *isnull is an output parameter.
 */
static Datum
plperl_sv_to_datum(SV *sv, Oid typid, int32 typmod,
				   FunctionCallInfo fcinfo,
				   FmgrInfo *finfo, Oid typioparam,
				   bool *isnull)
{
	FmgrInfo	tmp;
	Oid			funcid;

	/* we might recurse */
	check_stack_depth();

	*isnull = false;

	/*
	 * Return NULL if result is undef, or if we're in a function returning
	 * VOID.  In the latter case, we should pay no attention to the last Perl
	 * statement's result, and this is a convenient means to ensure that.
	 */
	if (!sv || !SvOK(sv) || typid == VOIDOID)
	{
		/* look up type info if they did not pass it */
		if (!finfo)
		{
			_sv_to_datum_finfo(typid, &tmp, &typioparam);
			finfo = &tmp;
		}
		*isnull = true;
		/* must call typinput in case it wants to reject NULL */
		return InputFunctionCall(finfo, NULL, typioparam, typmod);
	}
	else if ((funcid = get_transform_tosql(typid, current_call_data->prodesc->lang_oid, current_call_data->prodesc->trftypes)))
		return OidFunctionCall1(funcid, PointerGetDatum(sv));
	else if (SvROK(sv))
	{
		/* handle references */
		SV		   *sav = get_perl_array_ref(sv);

		if (sav)
		{
			/* handle an arrayref */
			return plperl_array_to_datum(sav, typid, typmod);
		}
		else if (SvTYPE(SvRV(sv)) == SVt_PVHV)
		{
			/* handle a hashref */
			Datum		ret;
			TupleDesc	td;
			bool		isdomain;

			if (!type_is_rowtype(typid))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot convert Perl hash to non-composite type %s",
								format_type_be(typid))));

			td = lookup_rowtype_tupdesc_domain(typid, typmod, true);
			if (td != NULL)
			{
				/* Did we look through a domain? */
				isdomain = (typid != td->tdtypeid);
			}
			else
			{
				/* Must be RECORD, try to resolve based on call info */
				TypeFuncClass funcclass;

				if (fcinfo)
					funcclass = get_call_result_type(fcinfo, &typid, &td);
				else
					funcclass = TYPEFUNC_OTHER;
				if (funcclass != TYPEFUNC_COMPOSITE &&
					funcclass != TYPEFUNC_COMPOSITE_DOMAIN)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("function returning record called in context "
									"that cannot accept type record")));
				Assert(td);
				isdomain = (funcclass == TYPEFUNC_COMPOSITE_DOMAIN);
			}

			ret = plperl_hash_to_datum(sv, td);

			if (isdomain)
				domain_check(ret, false, typid, NULL, NULL);

			/* Release on the result of get_call_result_type is harmless */
			ReleaseTupleDesc(td);

			return ret;
		}

		/*
		 * If it's a reference to something else, such as a scalar, just
		 * recursively look through the reference.
		 */
		return plperl_sv_to_datum(SvRV(sv), typid, typmod,
								  fcinfo, finfo, typioparam,
								  isnull);
	}
	else
	{
		/* handle a string/number */
		Datum		ret;
		char	   *str = sv2cstr(sv);

		/* did not pass in any typeinfo? look it up */
		if (!finfo)
		{
			_sv_to_datum_finfo(typid, &tmp, &typioparam);
			finfo = &tmp;
		}

		ret = InputFunctionCall(finfo, str, typioparam, typmod);
		pfree(str);

		return ret;
	}
}

/* Convert the perl SV to a string returned by the type output function */
char *
plperl_sv_to_literal(SV *sv, char *fqtypename)
{
	Oid			typid;
	Oid			typoutput;
	Datum		datum;
	bool		typisvarlena,
				isnull;

	check_spi_usage_allowed();

	typid = DirectFunctionCall1(regtypein, CStringGetDatum(fqtypename));
	if (!OidIsValid(typid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("lookup failed for type %s", fqtypename)));

	datum = plperl_sv_to_datum(sv,
							   typid, -1,
							   NULL, NULL, InvalidOid,
							   &isnull);

	if (isnull)
		return NULL;

	getTypeOutputInfo(typid,
					  &typoutput, &typisvarlena);

	return OidOutputFunctionCall(typoutput, datum);
}

/*
 * Convert PostgreSQL array datum to a perl array reference.
 *
 * typid is arg's OID, which must be an array type.
 */
static SV  *
plperl_ref_from_pg_array(Datum arg, Oid typid)
{
	dTHX;
	ArrayType  *ar = DatumGetArrayTypeP(arg);
	Oid			elementtype = ARR_ELEMTYPE(ar);
	int16		typlen;
	bool		typbyval;
	char		typalign,
				typdelim;
	Oid			typioparam;
	Oid			typoutputfunc;
	Oid			transform_funcid;
	int			i,
				nitems,
			   *dims;
	plperl_array_info *info;
	SV		   *av;
	HV		   *hv;

	/*
	 * Currently we make no effort to cache any of the stuff we look up here,
	 * which is bad.
	 */
	info = palloc0(sizeof(plperl_array_info));

	/* get element type information, including output conversion function */
	get_type_io_data(elementtype, IOFunc_output,
					 &typlen, &typbyval, &typalign,
					 &typdelim, &typioparam, &typoutputfunc);

	/* Check for a transform function */
	transform_funcid = get_transform_fromsql(elementtype,
											 current_call_data->prodesc->lang_oid,
											 current_call_data->prodesc->trftypes);

	/* Look up transform or output function as appropriate */
	if (OidIsValid(transform_funcid))
		fmgr_info(transform_funcid, &info->transform_proc);
	else
		fmgr_info(typoutputfunc, &info->proc);

	info->elem_is_rowtype = type_is_rowtype(elementtype);

	/* Get the number and bounds of array dimensions */
	info->ndims = ARR_NDIM(ar);
	dims = ARR_DIMS(ar);

	/* No dimensions? Return an empty array */
	if (info->ndims == 0)
	{
		av = newRV_noinc((SV *) newAV());
	}
	else
	{
		deconstruct_array(ar, elementtype, typlen, typbyval,
						  typalign, &info->elements, &info->nulls,
						  &nitems);

		/* Get total number of elements in each dimension */
		info->nelems = palloc(sizeof(int) * info->ndims);
		info->nelems[0] = nitems;
		for (i = 1; i < info->ndims; i++)
			info->nelems[i] = info->nelems[i - 1] / dims[i - 1];

		av = split_array(info, 0, nitems, 0);
	}

	hv = newHV();
	(void) hv_store(hv, "array", 5, av, 0);
	(void) hv_store(hv, "typeoid", 7, newSVuv(typid), 0);

	return sv_bless(newRV_noinc((SV *) hv),
					gv_stashpv("PostgreSQL::InServer::ARRAY", 0));
}

/*
 * Recursively form array references from splices of the initial array
 */
static SV  *
split_array(plperl_array_info *info, int first, int last, int nest)
{
	dTHX;
	int			i;
	AV		   *result;

	/* we should only be called when we have something to split */
	Assert(info->ndims > 0);

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	/*
	 * Base case, return a reference to a single-dimensional array
	 */
	if (nest >= info->ndims - 1)
		return make_array_ref(info, first, last);

	result = newAV();
	for (i = first; i < last; i += info->nelems[nest + 1])
	{
		/* Recursively form references to arrays of lower dimensions */
		SV		   *ref = split_array(info, i, i + info->nelems[nest + 1], nest + 1);

		av_push(result, ref);
	}
	return newRV_noinc((SV *) result);
}

/*
 * Create a Perl reference from a one-dimensional C array, converting
 * composite type elements to hash references.
 */
static SV  *
make_array_ref(plperl_array_info *info, int first, int last)
{
	dTHX;
	int			i;
	AV		   *result = newAV();

	for (i = first; i < last; i++)
	{
		if (info->nulls[i])
		{
			/*
			 * We can't use &PL_sv_undef here.  See "AVs, HVs and undefined
			 * values" in perlguts.
			 */
			av_push(result, newSV(0));
		}
		else
		{
			Datum		itemvalue = info->elements[i];

			if (info->transform_proc.fn_oid)
				av_push(result, (SV *) DatumGetPointer(FunctionCall1(&info->transform_proc, itemvalue)));
			else if (info->elem_is_rowtype)
				/* Handle composite type elements */
				av_push(result, plperl_hash_from_datum(itemvalue));
			else
			{
				char	   *val = OutputFunctionCall(&info->proc, itemvalue);

				av_push(result, cstr2sv(val));
			}
		}
	}
	return newRV_noinc((SV *) result);
}

/* Set up the arguments for a trigger call. */
static SV  *
plperl_trigger_build_args(FunctionCallInfo fcinfo)
{
	dTHX;
	TriggerData *tdata;
	TupleDesc	tupdesc;
	int			i;
	char	   *level;
	char	   *event;
	char	   *relid;
	char	   *when;
	HV		   *hv;

	hv = newHV();
	hv_ksplit(hv, 12);			/* pre-grow the hash */

	tdata = (TriggerData *) fcinfo->context;
	tupdesc = tdata->tg_relation->rd_att;

	relid = DatumGetCString(DirectFunctionCall1(oidout,
												ObjectIdGetDatum(tdata->tg_relation->rd_id)));

	hv_store_string(hv, "name", cstr2sv(tdata->tg_trigger->tgname));
	hv_store_string(hv, "relid", cstr2sv(relid));

	/*
	 * Note: In BEFORE trigger, stored generated columns are not computed yet,
	 * so don't make them accessible in NEW row.
	 */

	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
	{
		event = "INSERT";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
			hv_store_string(hv, "new",
							plperl_hash_from_tuple(tdata->tg_trigtuple,
												   tupdesc,
												   !TRIGGER_FIRED_BEFORE(tdata->tg_event)));
	}
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
	{
		event = "DELETE";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
			hv_store_string(hv, "old",
							plperl_hash_from_tuple(tdata->tg_trigtuple,
												   tupdesc,
												   true));
	}
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
	{
		event = "UPDATE";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		{
			hv_store_string(hv, "old",
							plperl_hash_from_tuple(tdata->tg_trigtuple,
												   tupdesc,
												   true));
			hv_store_string(hv, "new",
							plperl_hash_from_tuple(tdata->tg_newtuple,
												   tupdesc,
												   !TRIGGER_FIRED_BEFORE(tdata->tg_event)));
		}
	}
	else if (TRIGGER_FIRED_BY_TRUNCATE(tdata->tg_event))
		event = "TRUNCATE";
	else
		event = "UNKNOWN";

	hv_store_string(hv, "event", cstr2sv(event));
	hv_store_string(hv, "argc", newSViv(tdata->tg_trigger->tgnargs));

	if (tdata->tg_trigger->tgnargs > 0)
	{
		AV		   *av = newAV();

		av_extend(av, tdata->tg_trigger->tgnargs);
		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
			av_push(av, cstr2sv(tdata->tg_trigger->tgargs[i]));
		hv_store_string(hv, "args", newRV_noinc((SV *) av));
	}

	hv_store_string(hv, "relname",
					cstr2sv(SPI_getrelname(tdata->tg_relation)));

	hv_store_string(hv, "table_name",
					cstr2sv(SPI_getrelname(tdata->tg_relation)));

	hv_store_string(hv, "table_schema",
					cstr2sv(SPI_getnspname(tdata->tg_relation)));

	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		when = "BEFORE";
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		when = "AFTER";
	else if (TRIGGER_FIRED_INSTEAD(tdata->tg_event))
		when = "INSTEAD OF";
	else
		when = "UNKNOWN";
	hv_store_string(hv, "when", cstr2sv(when));

	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		level = "ROW";
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		level = "STATEMENT";
	else
		level = "UNKNOWN";
	hv_store_string(hv, "level", cstr2sv(level));

	return newRV_noinc((SV *) hv);
}


/* Set up the arguments for an event trigger call. */
static SV  *
plperl_event_trigger_build_args(FunctionCallInfo fcinfo)
{
	dTHX;
	EventTriggerData *tdata;
	HV		   *hv;

	hv = newHV();

	tdata = (EventTriggerData *) fcinfo->context;

	hv_store_string(hv, "event", cstr2sv(tdata->event));
	hv_store_string(hv, "tag", cstr2sv(GetCommandTagName(tdata->tag)));

	return newRV_noinc((SV *) hv);
}

/* Construct the modified new tuple to be returned from a trigger. */
static HeapTuple
plperl_modify_tuple(HV *hvTD, TriggerData *tdata, HeapTuple otup)
{
	dTHX;
	SV		  **svp;
	HV		   *hvNew;
	HE		   *he;
	HeapTuple	rtup;
	TupleDesc	tupdesc;
	int			natts;
	Datum	   *modvalues;
	bool	   *modnulls;
	bool	   *modrepls;

	svp = hv_fetch_string(hvTD, "new");
	if (!svp)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("$_TD->{new} does not exist")));
	if (!SvOK(*svp) || !SvROK(*svp) || SvTYPE(SvRV(*svp)) != SVt_PVHV)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("$_TD->{new} is not a hash reference")));
	hvNew = (HV *) SvRV(*svp);

	tupdesc = tdata->tg_relation->rd_att;
	natts = tupdesc->natts;

	modvalues = (Datum *) palloc0(natts * sizeof(Datum));
	modnulls = (bool *) palloc0(natts * sizeof(bool));
	modrepls = (bool *) palloc0(natts * sizeof(bool));

	hv_iterinit(hvNew);
	while ((he = hv_iternext(hvNew)))
	{
		char	   *key = hek2cstr(he);
		SV		   *val = HeVAL(he);
		int			attn = SPI_fnumber(tupdesc, key);
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attn - 1);

		if (attn == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("Perl hash contains nonexistent column \"%s\"",
							key)));
		if (attn <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot set system attribute \"%s\"",
							key)));
		if (attr->attgenerated)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("cannot set generated column \"%s\"",
							key)));

		modvalues[attn - 1] = plperl_sv_to_datum(val,
												 attr->atttypid,
												 attr->atttypmod,
												 NULL,
												 NULL,
												 InvalidOid,
												 &modnulls[attn - 1]);
		modrepls[attn - 1] = true;

		pfree(key);
	}
	hv_iterinit(hvNew);

	rtup = heap_modify_tuple(otup, tupdesc, modvalues, modnulls, modrepls);

	pfree(modvalues);
	pfree(modnulls);
	pfree(modrepls);

	return rtup;
}


/*
 * There are three externally visible pieces to plperl: plperl_call_handler,
 * plperl_inline_handler, and plperl_validator.
 */

/*
 * The call handler is called to run normal functions (including trigger
 * functions) that are defined in pg_proc.
 */
PG_FUNCTION_INFO_V1(plperl_call_handler);

Datum
plperl_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval = (Datum) 0;
	plperl_call_data *volatile save_call_data = current_call_data;
	plperl_interp_desc *volatile oldinterp = plperl_active_interp;
	plperl_call_data this_call_data;

	/* Initialize current-call status record */
	MemSet(&this_call_data, 0, sizeof(this_call_data));
	this_call_data.fcinfo = fcinfo;

	PG_TRY();
	{
		current_call_data = &this_call_data;
		if (CALLED_AS_TRIGGER(fcinfo))
			retval = plperl_trigger_handler(fcinfo);
		else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		{
			plperl_event_trigger_handler(fcinfo);
			retval = (Datum) 0;
		}
		else
			retval = plperl_func_handler(fcinfo);
	}
	PG_FINALLY();
	{
		current_call_data = save_call_data;
		activate_interpreter(oldinterp);
		if (this_call_data.prodesc)
			decrement_prodesc_refcount(this_call_data.prodesc);
	}
	PG_END_TRY();

	return retval;
}

/*
 * The inline handler runs anonymous code blocks (DO blocks).
 */
PG_FUNCTION_INFO_V1(plperl_inline_handler);

Datum
plperl_inline_handler(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(fake_fcinfo, 0);
	InlineCodeBlock *codeblock = (InlineCodeBlock *) PG_GETARG_POINTER(0);
	FmgrInfo	flinfo;
	plperl_proc_desc desc;
	plperl_call_data *volatile save_call_data = current_call_data;
	plperl_interp_desc *volatile oldinterp = plperl_active_interp;
	plperl_call_data this_call_data;
	ErrorContextCallback pl_error_context;

	/* Initialize current-call status record */
	MemSet(&this_call_data, 0, sizeof(this_call_data));

	/* Set up a callback for error reporting */
	pl_error_context.callback = plperl_inline_callback;
	pl_error_context.previous = error_context_stack;
	pl_error_context.arg = NULL;
	error_context_stack = &pl_error_context;

	/*
	 * Set up a fake fcinfo and descriptor with just enough info to satisfy
	 * plperl_call_perl_func().  In particular note that this sets things up
	 * with no arguments passed, and a result type of VOID.
	 */
	MemSet(fake_fcinfo, 0, SizeForFunctionCallInfo(0));
	MemSet(&flinfo, 0, sizeof(flinfo));
	MemSet(&desc, 0, sizeof(desc));
	fake_fcinfo->flinfo = &flinfo;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	desc.proname = "inline_code_block";
	desc.fn_readonly = false;

	desc.lang_oid = codeblock->langOid;
	desc.trftypes = NIL;
	desc.lanpltrusted = codeblock->langIsTrusted;

	desc.fn_retistuple = false;
	desc.fn_retisset = false;
	desc.fn_retisarray = false;
	desc.result_oid = InvalidOid;
	desc.nargs = 0;
	desc.reference = NULL;

	this_call_data.fcinfo = fake_fcinfo;
	this_call_data.prodesc = &desc;
	/* we do not bother with refcounting the fake prodesc */

	PG_TRY();
	{
		SV		   *perlret;

		current_call_data = &this_call_data;

		SPI_connect_ext(codeblock->atomic ? 0 : SPI_OPT_NONATOMIC);

		select_perl_context(desc.lanpltrusted);

		plperl_create_sub(&desc, codeblock->source_text, 0);

		if (!desc.reference)	/* can this happen? */
			elog(ERROR, "could not create internal procedure for anonymous code block");

		perlret = plperl_call_perl_func(&desc, fake_fcinfo);

		SvREFCNT_dec_current(perlret);

		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish() failed");
	}
	PG_FINALLY();
	{
		if (desc.reference)
			SvREFCNT_dec_current(desc.reference);
		current_call_data = save_call_data;
		activate_interpreter(oldinterp);
	}
	PG_END_TRY();

	error_context_stack = pl_error_context.previous;

	PG_RETURN_VOID();
}

/*
 * The validator is called during CREATE FUNCTION to validate the function
 * being created/replaced. The precise behavior of the validator may be
 * modified by the check_function_bodies GUC.
 */
PG_FUNCTION_INFO_V1(plperl_validator);

Datum
plperl_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	char		functyptype;
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	bool		is_trigger = false;
	bool		is_event_trigger = false;
	int			i;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, EVTTRIGGER, RECORD, or VOID */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		if (proc->prorettype == TRIGGEROID)
			is_trigger = true;
		else if (proc->prorettype == EVENT_TRIGGEROID)
			is_event_trigger = true;
		else if (proc->prorettype != RECORDOID &&
				 proc->prorettype != VOIDOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/Perl functions cannot return type %s",
							format_type_be(proc->prorettype))));
	}

	/* Disallow pseudotypes in arguments (either IN or OUT) */
	numargs = get_func_arg_info(tuple,
								&argtypes, &argnames, &argmodes);
	for (i = 0; i < numargs; i++)
	{
		if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO &&
			argtypes[i] != RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/Perl functions cannot accept type %s",
							format_type_be(argtypes[i]))));
	}

	ReleaseSysCache(tuple);

	/* Postpone body checks if !check_function_bodies */
	if (check_function_bodies)
	{
		(void) compile_plperl_function(funcoid, is_trigger, is_event_trigger);
	}

	/* the result of a validator is ignored */
	PG_RETURN_VOID();
}


/*
 * plperlu likewise requires three externally visible functions:
 * plperlu_call_handler, plperlu_inline_handler, and plperlu_validator.
 * These are currently just aliases that send control to the plperl
 * handler functions, and we decide whether a particular function is
 * trusted or not by inspecting the actual pg_language tuple.
 */

PG_FUNCTION_INFO_V1(plperlu_call_handler);

Datum
plperlu_call_handler(PG_FUNCTION_ARGS)
{
	return plperl_call_handler(fcinfo);
}

PG_FUNCTION_INFO_V1(plperlu_inline_handler);

Datum
plperlu_inline_handler(PG_FUNCTION_ARGS)
{
	return plperl_inline_handler(fcinfo);
}

PG_FUNCTION_INFO_V1(plperlu_validator);

Datum
plperlu_validator(PG_FUNCTION_ARGS)
{
	/* call plperl validator with our fcinfo so it gets our oid */
	return plperl_validator(fcinfo);
}


/*
 * Uses mkfunc to create a subroutine whose text is
 * supplied in s, and returns a reference to it
 */
static void
plperl_create_sub(plperl_proc_desc *prodesc, const char *s, Oid fn_oid)
{
	dTHX;
	dSP;
	char		subname[NAMEDATALEN + 40];
	HV		   *pragma_hv = newHV();
	SV		   *subref = NULL;
	int			count;

	sprintf(subname, "%s__%u", prodesc->proname, fn_oid);

	if (plperl_use_strict)
		hv_store_string(pragma_hv, "strict", (SV *) newAV());

	ENTER;
	SAVETMPS;
	PUSHMARK(SP);
	EXTEND(SP, 4);
	PUSHs(sv_2mortal(cstr2sv(subname)));
	PUSHs(sv_2mortal(newRV_noinc((SV *) pragma_hv)));

	/*
	 * Use 'false' for $prolog in mkfunc, which is kept for compatibility in
	 * case a module such as PostgreSQL::PLPerl::NYTprof replaces the function
	 * compiler.
	 */
	PUSHs(&PL_sv_no);
	PUSHs(sv_2mortal(cstr2sv(s)));
	PUTBACK;

	/*
	 * G_KEEPERR seems to be needed here, else we don't recognize compile
	 * errors properly.  Perhaps it's because there's another level of eval
	 * inside mkfunc?
	 */
	count = call_pv("PostgreSQL::InServer::mkfunc",
					G_SCALAR | G_EVAL | G_KEEPERR);
	SPAGAIN;

	if (count == 1)
	{
		SV		   *sub_rv = (SV *) POPs;

		if (sub_rv && SvROK(sub_rv) && SvTYPE(SvRV(sub_rv)) == SVt_PVCV)
		{
			subref = newRV_inc(SvRV(sub_rv));
		}
	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	if (SvTRUE(ERRSV))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV)))));

	if (!subref)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("didn't get a CODE reference from compiling function \"%s\"",
						prodesc->proname)));

	prodesc->reference = subref;
}


/**********************************************************************
 * plperl_init_shared_libs()		-
 **********************************************************************/

static void
plperl_init_shared_libs(pTHX)
{
	char	   *file = __FILE__;

	newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
	newXS("PostgreSQL::InServer::Util::bootstrap",
		  boot_PostgreSQL__InServer__Util, file);
	/* newXS for...::SPI::bootstrap is in select_perl_context() */
}


static SV  *
plperl_call_perl_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo)
{
	dTHX;
	dSP;
	SV		   *retval;
	int			i;
	int			count;
	Oid		   *argtypes = NULL;
	int			nargs = 0;

	ENTER;
	SAVETMPS;

	PUSHMARK(SP);
	EXTEND(sp, desc->nargs);

	/* Get signature for true functions; inline blocks have no args. */
	if (fcinfo->flinfo->fn_oid)
		get_func_signature(fcinfo->flinfo->fn_oid, &argtypes, &nargs);
	Assert(nargs == desc->nargs);

	for (i = 0; i < desc->nargs; i++)
	{
		if (fcinfo->args[i].isnull)
			PUSHs(&PL_sv_undef);
		else if (desc->arg_is_rowtype[i])
		{
			SV		   *sv = plperl_hash_from_datum(fcinfo->args[i].value);

			PUSHs(sv_2mortal(sv));
		}
		else
		{
			SV		   *sv;
			Oid			funcid;

			if (OidIsValid(desc->arg_arraytype[i]))
				sv = plperl_ref_from_pg_array(fcinfo->args[i].value, desc->arg_arraytype[i]);
			else if ((funcid = get_transform_fromsql(argtypes[i], current_call_data->prodesc->lang_oid, current_call_data->prodesc->trftypes)))
				sv = (SV *) DatumGetPointer(OidFunctionCall1(funcid, fcinfo->args[i].value));
			else
			{
				char	   *tmp;

				tmp = OutputFunctionCall(&(desc->arg_out_func[i]),
										 fcinfo->args[i].value);
				sv = cstr2sv(tmp);
				pfree(tmp);
			}

			PUSHs(sv_2mortal(sv));
		}
	}
	PUTBACK;

	/* Do NOT use G_KEEPERR here */
	count = call_sv(desc->reference, G_SCALAR | G_EVAL);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("didn't get a return item from function")));
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		/* XXX need to find a way to determine a better errcode here */
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV)))));
	}

	retval = newSVsv(POPs);

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retval;
}


static SV  *
plperl_call_perl_trigger_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo,
							  SV *td)
{
	dTHX;
	dSP;
	SV		   *retval,
			   *TDsv;
	int			i,
				count;
	Trigger    *tg_trigger = ((TriggerData *) fcinfo->context)->tg_trigger;

	ENTER;
	SAVETMPS;

	TDsv = get_sv("main::_TD", 0);
	if (!TDsv)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("couldn't fetch $_TD")));

	save_item(TDsv);			/* local $_TD */
	sv_setsv(TDsv, td);

	PUSHMARK(sp);
	EXTEND(sp, tg_trigger->tgnargs);

	for (i = 0; i < tg_trigger->tgnargs; i++)
		PUSHs(sv_2mortal(cstr2sv(tg_trigger->tgargs[i])));
	PUTBACK;

	/* Do NOT use G_KEEPERR here */
	count = call_sv(desc->reference, G_SCALAR | G_EVAL);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("didn't get a return item from trigger function")));
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		/* XXX need to find a way to determine a better errcode here */
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV)))));
	}

	retval = newSVsv(POPs);

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retval;
}


static void
plperl_call_perl_event_trigger_func(plperl_proc_desc *desc,
									FunctionCallInfo fcinfo,
									SV *td)
{
	dTHX;
	dSP;
	SV		   *retval,
			   *TDsv;
	int			count;

	ENTER;
	SAVETMPS;

	TDsv = get_sv("main::_TD", 0);
	if (!TDsv)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("couldn't fetch $_TD")));

	save_item(TDsv);			/* local $_TD */
	sv_setsv(TDsv, td);

	PUSHMARK(sp);
	PUTBACK;

	/* Do NOT use G_KEEPERR here */
	count = call_sv(desc->reference, G_SCALAR | G_EVAL);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("didn't get a return item from trigger function")));
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		/* XXX need to find a way to determine a better errcode here */
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s", strip_trailing_ws(sv2cstr(ERRSV)))));
	}

	retval = newSVsv(POPs);
	(void) retval;				/* silence compiler warning */

	PUTBACK;
	FREETMPS;
	LEAVE;
}

static Datum
plperl_func_handler(PG_FUNCTION_ARGS)
{
	bool		nonatomic;
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval = 0;
	ReturnSetInfo *rsi;
	ErrorContextCallback pl_error_context;

	nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0);

	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, false, false);
	current_call_data->prodesc = prodesc;
	increment_prodesc_refcount(prodesc);

	/* Set a callback for error reporting */
	pl_error_context.callback = plperl_exec_callback;
	pl_error_context.previous = error_context_stack;
	pl_error_context.arg = prodesc->proname;
	error_context_stack = &pl_error_context;

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (prodesc->fn_retisset)
	{
		/* Check context before allowing the call to go through */
		if (!rsi || !IsA(rsi, ReturnSetInfo))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));

		if (!(rsi->allowedModes & SFRM_Materialize))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialize mode required, but it is not allowed in this context")));
	}

	activate_interpreter(prodesc->interp);

	perlret = plperl_call_perl_func(prodesc, fcinfo);

	/************************************************************
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	if (prodesc->fn_retisset)
	{
		SV		   *sav;

		/*
		 * If the Perl function returned an arrayref, we pretend that it
		 * called return_next() for each element of the array, to handle old
		 * SRFs that didn't know about return_next(). Any other sort of return
		 * value is an error, except undef which means return an empty set.
		 */
		sav = get_perl_array_ref(perlret);
		if (sav)
		{
			dTHX;
			int			i = 0;
			SV		  **svp = 0;
			AV		   *rav = (AV *) SvRV(sav);

			while ((svp = av_fetch(rav, i, FALSE)) != NULL)
			{
				plperl_return_next_internal(*svp);
				i++;
			}
		}
		else if (SvOK(perlret))
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("set-returning PL/Perl function must return "
							"reference to array or use return_next")));
		}

		rsi->returnMode = SFRM_Materialize;
		if (current_call_data->tuple_store)
		{
			rsi->setResult = current_call_data->tuple_store;
			rsi->setDesc = current_call_data->ret_tdesc;
		}
		retval = (Datum) 0;
	}
	else if (prodesc->result_oid)
	{
		retval = plperl_sv_to_datum(perlret,
									prodesc->result_oid,
									-1,
									fcinfo,
									&prodesc->result_in_func,
									prodesc->result_typioparam,
									&fcinfo->isnull);

		if (fcinfo->isnull && rsi && IsA(rsi, ReturnSetInfo))
			rsi->isDone = ExprEndResult;
	}

	/* Restore the previous error callback */
	error_context_stack = pl_error_context.previous;

	SvREFCNT_dec_current(perlret);

	return retval;
}


static Datum
plperl_trigger_handler(PG_FUNCTION_ARGS)
{
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval;
	SV		   *svTD;
	HV		   *hvTD;
	ErrorContextCallback pl_error_context;
	TriggerData *tdata;
	int			rc PG_USED_FOR_ASSERTS_ONLY;

	/* Connect to SPI manager */
	SPI_connect();

	/* Make transition tables visible to this SPI connection */
	tdata = (TriggerData *) fcinfo->context;
	rc = SPI_register_trigger_data(tdata);
	Assert(rc >= 0);

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, true, false);
	current_call_data->prodesc = prodesc;
	increment_prodesc_refcount(prodesc);

	/* Set a callback for error reporting */
	pl_error_context.callback = plperl_exec_callback;
	pl_error_context.previous = error_context_stack;
	pl_error_context.arg = prodesc->proname;
	error_context_stack = &pl_error_context;

	activate_interpreter(prodesc->interp);

	svTD = plperl_trigger_build_args(fcinfo);
	perlret = plperl_call_perl_trigger_func(prodesc, fcinfo, svTD);
	hvTD = (HV *) SvRV(svTD);

	/************************************************************
	* Disconnect from SPI manager and then create the return
	* values datum (if the input function does a palloc for it
	* this must not be allocated in the SPI memory context
	* because SPI_finish would free it).
	************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	if (perlret == NULL || !SvOK(perlret))
	{
		/* undef result means go ahead with original tuple */
		TriggerData *trigdata = ((TriggerData *) fcinfo->context);

		if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			retval = (Datum) trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			retval = (Datum) trigdata->tg_newtuple;
		else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			retval = (Datum) trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
			retval = (Datum) trigdata->tg_trigtuple;
		else
			retval = (Datum) 0; /* can this happen? */
	}
	else
	{
		HeapTuple	trv;
		char	   *tmp;

		tmp = sv2cstr(perlret);

		if (pg_strcasecmp(tmp, "SKIP") == 0)
			trv = NULL;
		else if (pg_strcasecmp(tmp, "MODIFY") == 0)
		{
			TriggerData *trigdata = (TriggerData *) fcinfo->context;

			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
				trv = plperl_modify_tuple(hvTD, trigdata,
										  trigdata->tg_trigtuple);
			else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				trv = plperl_modify_tuple(hvTD, trigdata,
										  trigdata->tg_newtuple);
			else
			{
				ereport(WARNING,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("ignoring modified row in DELETE trigger")));
				trv = NULL;
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("result of PL/Perl trigger function must be undef, "
							"\"SKIP\", or \"MODIFY\"")));
			trv = NULL;
		}
		retval = PointerGetDatum(trv);
		pfree(tmp);
	}

	/* Restore the previous error callback */
	error_context_stack = pl_error_context.previous;

	SvREFCNT_dec_current(svTD);
	if (perlret)
		SvREFCNT_dec_current(perlret);

	return retval;
}


static void
plperl_event_trigger_handler(PG_FUNCTION_ARGS)
{
	plperl_proc_desc *prodesc;
	SV		   *svTD;
	ErrorContextCallback pl_error_context;

	/* Connect to SPI manager */
	SPI_connect();

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, false, true);
	current_call_data->prodesc = prodesc;
	increment_prodesc_refcount(prodesc);

	/* Set a callback for error reporting */
	pl_error_context.callback = plperl_exec_callback;
	pl_error_context.previous = error_context_stack;
	pl_error_context.arg = prodesc->proname;
	error_context_stack = &pl_error_context;

	activate_interpreter(prodesc->interp);

	svTD = plperl_event_trigger_build_args(fcinfo);
	plperl_call_perl_event_trigger_func(prodesc, fcinfo, svTD);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	/* Restore the previous error callback */
	error_context_stack = pl_error_context.previous;

	SvREFCNT_dec_current(svTD);
}


static bool
validate_plperl_function(plperl_proc_ptr *proc_ptr, HeapTuple procTup)
{
	if (proc_ptr && proc_ptr->proc_ptr)
	{
		plperl_proc_desc *prodesc = proc_ptr->proc_ptr;
		bool		uptodate;

		/************************************************************
		 * If it's present, must check whether it's still up to date.
		 * This is needed because CREATE OR REPLACE FUNCTION can modify the
		 * function's pg_proc entry without changing its OID.
		 ************************************************************/
		uptodate = (prodesc->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
					ItemPointerEquals(&prodesc->fn_tid, &procTup->t_self));

		if (uptodate)
			return true;

		/* Otherwise, unlink the obsoleted entry from the hashtable ... */
		proc_ptr->proc_ptr = NULL;
		/* ... and release the corresponding refcount, probably deleting it */
		decrement_prodesc_refcount(prodesc);
	}

	return false;
}


static void
free_plperl_function(plperl_proc_desc *prodesc)
{
	Assert(prodesc->fn_refcount == 0);
	/* Release CODE reference, if we have one, from the appropriate interp */
	if (prodesc->reference)
	{
		plperl_interp_desc *oldinterp = plperl_active_interp;

		activate_interpreter(prodesc->interp);
		SvREFCNT_dec_current(prodesc->reference);
		activate_interpreter(oldinterp);
	}
	/* Release all PG-owned data for this proc */
	MemoryContextDelete(prodesc->fn_cxt);
}


static plperl_proc_desc *
compile_plperl_function(Oid fn_oid, bool is_trigger, bool is_event_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	plperl_proc_key proc_key;
	plperl_proc_ptr *proc_ptr;
	plperl_proc_desc *volatile prodesc = NULL;
	volatile MemoryContext proc_cxt = NULL;
	plperl_interp_desc *oldinterp = plperl_active_interp;
	ErrorContextCallback plperl_error_context;

	/* We'll need the pg_proc tuple in any case... */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Try to find function in plperl_proc_hash.  The reason for this
	 * overcomplicated-seeming lookup procedure is that we don't know whether
	 * it's plperl or plperlu, and don't want to spend a lookup in pg_language
	 * to find out.
	 */
	proc_key.proc_id = fn_oid;
	proc_key.is_trigger = is_trigger;
	proc_key.user_id = GetUserId();
	proc_ptr = hash_search(plperl_proc_hash, &proc_key,
						   HASH_FIND, NULL);
	if (validate_plperl_function(proc_ptr, procTup))
	{
		/* Found valid plperl entry */
		ReleaseSysCache(procTup);
		return proc_ptr->proc_ptr;
	}

	/* If not found or obsolete, maybe it's plperlu */
	proc_key.user_id = InvalidOid;
	proc_ptr = hash_search(plperl_proc_hash, &proc_key,
						   HASH_FIND, NULL);
	if (validate_plperl_function(proc_ptr, procTup))
	{
		/* Found valid plperlu entry */
		ReleaseSysCache(procTup);
		return proc_ptr->proc_ptr;
	}

	/************************************************************
	 * If we haven't found it in the hashtable, we analyze
	 * the function's arguments and return type and store
	 * the in-/out-functions in the prodesc block,
	 * then we load the procedure into the Perl interpreter,
	 * and last we create a new hashtable entry for it.
	 ************************************************************/

	/* Set a callback for reporting compilation errors */
	plperl_error_context.callback = plperl_compile_callback;
	plperl_error_context.previous = error_context_stack;
	plperl_error_context.arg = NameStr(procStruct->proname);
	error_context_stack = &plperl_error_context;

	PG_TRY();
	{
		HeapTuple	langTup;
		HeapTuple	typeTup;
		Form_pg_language langStruct;
		Form_pg_type typeStruct;
		Datum		protrftypes_datum;
		Datum		prosrcdatum;
		bool		isnull;
		char	   *proc_source;
		MemoryContext oldcontext;

		/************************************************************
		 * Allocate a context that will hold all PG data for the procedure.
		 ************************************************************/
		proc_cxt = AllocSetContextCreate(TopMemoryContext,
										 "PL/Perl function",
										 ALLOCSET_SMALL_SIZES);

		/************************************************************
		 * Allocate and fill a new procedure description block.
		 * struct prodesc and subsidiary data must all live in proc_cxt.
		 ************************************************************/
		oldcontext = MemoryContextSwitchTo(proc_cxt);
		prodesc = (plperl_proc_desc *) palloc0(sizeof(plperl_proc_desc));
		prodesc->proname = pstrdup(NameStr(procStruct->proname));
		MemoryContextSetIdentifier(proc_cxt, prodesc->proname);
		prodesc->fn_cxt = proc_cxt;
		prodesc->fn_refcount = 0;
		prodesc->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
		prodesc->fn_tid = procTup->t_self;
		prodesc->nargs = procStruct->pronargs;
		prodesc->arg_out_func = (FmgrInfo *) palloc0(prodesc->nargs * sizeof(FmgrInfo));
		prodesc->arg_is_rowtype = (bool *) palloc0(prodesc->nargs * sizeof(bool));
		prodesc->arg_arraytype = (Oid *) palloc0(prodesc->nargs * sizeof(Oid));
		MemoryContextSwitchTo(oldcontext);

		/* Remember if function is STABLE/IMMUTABLE */
		prodesc->fn_readonly =
			(procStruct->provolatile != PROVOLATILE_VOLATILE);

		/* Fetch protrftypes */
		protrftypes_datum = SysCacheGetAttr(PROCOID, procTup,
											Anum_pg_proc_protrftypes, &isnull);
		MemoryContextSwitchTo(proc_cxt);
		prodesc->trftypes = isnull ? NIL : oid_array_to_list(protrftypes_datum);
		MemoryContextSwitchTo(oldcontext);

		/************************************************************
		 * Lookup the pg_language tuple by Oid
		 ************************************************************/
		langTup = SearchSysCache1(LANGOID,
								  ObjectIdGetDatum(procStruct->prolang));
		if (!HeapTupleIsValid(langTup))
			elog(ERROR, "cache lookup failed for language %u",
				 procStruct->prolang);
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		prodesc->lang_oid = langStruct->oid;
		prodesc->lanpltrusted = langStruct->lanpltrusted;
		ReleaseSysCache(langTup);

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

			/* Disallow pseudotype result, except VOID or RECORD */
			if (typeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (rettype == VOIDOID ||
					rettype == RECORDOID)
					 /* okay */ ;
				else if (rettype == TRIGGEROID ||
						 rettype == EVENT_TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called "
									"as triggers")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Perl functions cannot return type %s",
									format_type_be(rettype))));
			}

			prodesc->result_oid = rettype;
			prodesc->fn_retisset = procStruct->proretset;
			prodesc->fn_retistuple = type_is_rowtype(rettype);
			prodesc->fn_retisarray = IsTrueArrayType(typeStruct);

			fmgr_info_cxt(typeStruct->typinput,
						  &(prodesc->result_in_func),
						  proc_cxt);
			prodesc->result_typioparam = getTypeIOParam(typeTup);

			ReleaseSysCache(typeTup);
		}

		/************************************************************
		 * Get the required information for output conversion
		 * of all procedure arguments
		 ************************************************************/
		if (!is_trigger && !is_event_trigger)
		{
			int			i;

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
							 errmsg("PL/Perl functions cannot accept type %s",
									format_type_be(argtype))));

				if (type_is_rowtype(argtype))
					prodesc->arg_is_rowtype[i] = true;
				else
				{
					prodesc->arg_is_rowtype[i] = false;
					fmgr_info_cxt(typeStruct->typoutput,
								  &(prodesc->arg_out_func[i]),
								  proc_cxt);
				}

				/* Identify array-type arguments */
				if (IsTrueArrayType(typeStruct))
					prodesc->arg_arraytype[i] = argtype;
				else
					prodesc->arg_arraytype[i] = InvalidOid;

				ReleaseSysCache(typeTup);
			}
		}

		/************************************************************
		 * create the text of the anonymous subroutine.
		 * we do not use a named subroutine so that we can call directly
		 * through the reference.
		 ************************************************************/
		prosrcdatum = SysCacheGetAttrNotNull(PROCOID, procTup,
											 Anum_pg_proc_prosrc);
		proc_source = TextDatumGetCString(prosrcdatum);

		/************************************************************
		 * Create the procedure in the appropriate interpreter
		 ************************************************************/

		select_perl_context(prodesc->lanpltrusted);

		prodesc->interp = plperl_active_interp;

		plperl_create_sub(prodesc, proc_source, fn_oid);

		activate_interpreter(oldinterp);

		pfree(proc_source);

		if (!prodesc->reference)	/* can this happen? */
			elog(ERROR, "could not create PL/Perl internal procedure");

		/************************************************************
		 * OK, link the procedure into the correct hashtable entry.
		 * Note we assume that the hashtable entry either doesn't exist yet,
		 * or we already cleared its proc_ptr during the validation attempts
		 * above.  So no need to decrement an old refcount here.
		 ************************************************************/
		proc_key.user_id = prodesc->lanpltrusted ? GetUserId() : InvalidOid;

		proc_ptr = hash_search(plperl_proc_hash, &proc_key,
							   HASH_ENTER, NULL);
		/* We assume these two steps can't throw an error: */
		proc_ptr->proc_ptr = prodesc;
		increment_prodesc_refcount(prodesc);
	}
	PG_CATCH();
	{
		/*
		 * If we got as far as creating a reference, we should be able to use
		 * free_plperl_function() to clean up.  If not, then at most we have
		 * some PG memory resources in proc_cxt, which we can just delete.
		 */
		if (prodesc && prodesc->reference)
			free_plperl_function(prodesc);
		else if (proc_cxt)
			MemoryContextDelete(proc_cxt);

		/* Be sure to restore the previous interpreter, too, for luck */
		activate_interpreter(oldinterp);

		PG_RE_THROW();
	}
	PG_END_TRY();

	/* restore previous error callback */
	error_context_stack = plperl_error_context.previous;

	ReleaseSysCache(procTup);

	return prodesc;
}

/* Build a hash from a given composite/row datum */
static SV  *
plperl_hash_from_datum(Datum attr)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup;
	SV		   *sv;

	td = DatumGetHeapTupleHeader(attr);

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;

	sv = plperl_hash_from_tuple(&tmptup, tupdesc, true);
	ReleaseTupleDesc(tupdesc);

	return sv;
}

/* Build a hash from all attributes of a given tuple. */
static SV  *
plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc, bool include_generated)
{
	dTHX;
	HV		   *hv;
	int			i;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	hv = newHV();
	hv_ksplit(hv, tupdesc->natts);	/* pre-grow the hash */

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		attr;
		bool		isnull,
					typisvarlena;
		char	   *attname;
		Oid			typoutput;
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		if (att->attgenerated)
		{
			/* don't include unless requested */
			if (!include_generated)
				continue;
			/* never include virtual columns */
			if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
				continue;
		}

		attname = NameStr(att->attname);
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			/*
			 * Store (attname => undef) and move on.  Note we can't use
			 * &PL_sv_undef here; see "AVs, HVs and undefined values" in
			 * perlguts for an explanation.
			 */
			hv_store_string(hv, attname, newSV(0));
			continue;
		}

		if (type_is_rowtype(att->atttypid))
		{
			SV		   *sv = plperl_hash_from_datum(attr);

			hv_store_string(hv, attname, sv);
		}
		else
		{
			SV		   *sv;
			Oid			funcid;

			if (OidIsValid(get_base_element_type(att->atttypid)))
				sv = plperl_ref_from_pg_array(attr, att->atttypid);
			else if ((funcid = get_transform_fromsql(att->atttypid, current_call_data->prodesc->lang_oid, current_call_data->prodesc->trftypes)))
				sv = (SV *) DatumGetPointer(OidFunctionCall1(funcid, attr));
			else
			{
				char	   *outputstr;

				/* XXX should have a way to cache these lookups */
				getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);

				outputstr = OidOutputFunctionCall(typoutput, attr);
				sv = cstr2sv(outputstr);
				pfree(outputstr);
			}

			hv_store_string(hv, attname, sv);
		}
	}
	return newRV_noinc((SV *) hv);
}


static void
check_spi_usage_allowed(void)
{
	/* see comment in plperl_fini() */
	if (plperl_ending)
	{
		/* simple croak as we don't want to involve PostgreSQL code */
		croak("SPI functions can not be used in END blocks");
	}

	/*
	 * Disallow SPI usage if we're not executing a fully-compiled plperl
	 * function.  It might seem impossible to get here in that case, but there
	 * are cases where Perl will try to execute code during compilation.  If
	 * we proceed we are likely to crash trying to dereference the prodesc
	 * pointer.  Working around that might be possible, but it seems unwise
	 * because it'd allow code execution to happen while validating a
	 * function, which is undesirable.
	 */
	if (current_call_data == NULL || current_call_data->prodesc == NULL)
	{
		/* simple croak as we don't want to involve PostgreSQL code */
		croak("SPI functions can not be used during function compilation");
	}
}


HV *
plperl_spi_exec(char *query, int limit)
{
	HV		   *ret_hv;

	/*
	 * Execute the query inside a sub-transaction, so we can cope with errors
	 * sanely
	 */
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	check_spi_usage_allowed();

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		int			spi_rv;

		pg_verifymbstr(query, strlen(query), false);

		spi_rv = SPI_execute(query, current_call_data->prodesc->fn_readonly,
							 limit);
		ret_hv = plperl_spi_execute_fetch_result(SPI_tuptable, SPI_processed,
												 spi_rv);

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
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

		/* Punt the error to Perl */
		croak_cstr(edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return ret_hv;
}


static HV  *
plperl_spi_execute_fetch_result(SPITupleTable *tuptable, uint64 processed,
								int status)
{
	dTHX;
	HV		   *result;

	check_spi_usage_allowed();

	result = newHV();

	hv_store_string(result, "status",
					cstr2sv(SPI_result_code_string(status)));
	hv_store_string(result, "processed",
					(processed > (uint64) UV_MAX) ?
					newSVnv((NV) processed) :
					newSVuv((UV) processed));

	if (status > 0 && tuptable)
	{
		AV		   *rows;
		SV		   *row;
		uint64		i;

		/* Prevent overflow in call to av_extend() */
		if (processed > (uint64) AV_SIZE_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("query result has too many rows to fit in a Perl array")));

		rows = newAV();
		av_extend(rows, processed);
		for (i = 0; i < processed; i++)
		{
			row = plperl_hash_from_tuple(tuptable->vals[i], tuptable->tupdesc, true);
			av_push(rows, row);
		}
		hv_store_string(result, "rows",
						newRV_noinc((SV *) rows));
	}

	SPI_freetuptable(tuptable);

	return result;
}


/*
 * plperl_return_next catches any error and converts it to a Perl error.
 * We assume (perhaps without adequate justification) that we need not abort
 * the current transaction if the Perl code traps the error.
 */
void
plperl_return_next(SV *sv)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	check_spi_usage_allowed();

	PG_TRY();
	{
		plperl_return_next_internal(sv);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Must reset elog.c's state */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Punt the error to Perl */
		croak_cstr(edata->message);
	}
	PG_END_TRY();
}

/*
 * plperl_return_next_internal reports any errors in Postgres fashion
 * (via ereport).
 */
static void
plperl_return_next_internal(SV *sv)
{
	plperl_proc_desc *prodesc;
	FunctionCallInfo fcinfo;
	ReturnSetInfo *rsi;
	MemoryContext old_cxt;

	if (!sv)
		return;

	prodesc = current_call_data->prodesc;
	fcinfo = current_call_data->fcinfo;
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!prodesc->fn_retisset)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use return_next in a non-SETOF function")));

	if (!current_call_data->ret_tdesc)
	{
		TupleDesc	tupdesc;

		Assert(!current_call_data->tuple_store);

		/*
		 * This is the first call to return_next in the current PL/Perl
		 * function call, so identify the output tuple type and create a
		 * tuplestore to hold the result rows.
		 */
		if (prodesc->fn_retistuple)
		{
			TypeFuncClass funcclass;
			Oid			typid;

			funcclass = get_call_result_type(fcinfo, &typid, &tupdesc);
			if (funcclass != TYPEFUNC_COMPOSITE &&
				funcclass != TYPEFUNC_COMPOSITE_DOMAIN)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
			/* if domain-over-composite, remember the domain's type OID */
			if (funcclass == TYPEFUNC_COMPOSITE_DOMAIN)
				current_call_data->cdomain_oid = typid;
		}
		else
		{
			tupdesc = rsi->expectedDesc;
			/* Protect assumption below that we return exactly one column */
			if (tupdesc == NULL || tupdesc->natts != 1)
				elog(ERROR, "expected single-column result descriptor for non-composite SETOF result");
		}

		/*
		 * Make sure the tuple_store and ret_tdesc are sufficiently
		 * long-lived.
		 */
		old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

		current_call_data->ret_tdesc = CreateTupleDescCopy(tupdesc);
		current_call_data->tuple_store =
			tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
								  false, work_mem);

		MemoryContextSwitchTo(old_cxt);
	}

	/*
	 * Producing the tuple we want to return requires making plenty of
	 * palloc() allocations that are not cleaned up. Since this function can
	 * be called many times before the current memory context is reset, we
	 * need to do those allocations in a temporary context.
	 */
	if (!current_call_data->tmp_cxt)
	{
		current_call_data->tmp_cxt =
			AllocSetContextCreate(CurrentMemoryContext,
								  "PL/Perl return_next temporary cxt",
								  ALLOCSET_DEFAULT_SIZES);
	}

	old_cxt = MemoryContextSwitchTo(current_call_data->tmp_cxt);

	if (prodesc->fn_retistuple)
	{
		HeapTuple	tuple;

		if (!(SvOK(sv) && SvROK(sv) && SvTYPE(SvRV(sv)) == SVt_PVHV))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("SETOF-composite-returning PL/Perl function "
							"must call return_next with reference to hash")));

		tuple = plperl_build_tuple_result((HV *) SvRV(sv),
										  current_call_data->ret_tdesc);

		if (OidIsValid(current_call_data->cdomain_oid))
			domain_check(HeapTupleGetDatum(tuple), false,
						 current_call_data->cdomain_oid,
						 &current_call_data->cdomain_info,
						 rsi->econtext->ecxt_per_query_memory);

		tuplestore_puttuple(current_call_data->tuple_store, tuple);
	}
	else if (prodesc->result_oid)
	{
		Datum		ret[1];
		bool		isNull[1];

		ret[0] = plperl_sv_to_datum(sv,
									prodesc->result_oid,
									-1,
									fcinfo,
									&prodesc->result_in_func,
									prodesc->result_typioparam,
									&isNull[0]);

		tuplestore_putvalues(current_call_data->tuple_store,
							 current_call_data->ret_tdesc,
							 ret, isNull);
	}

	MemoryContextSwitchTo(old_cxt);
	MemoryContextReset(current_call_data->tmp_cxt);
}


SV *
plperl_spi_query(char *query)
{
	SV		   *cursor;

	/*
	 * Execute the query inside a sub-transaction, so we can cope with errors
	 * sanely
	 */
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	check_spi_usage_allowed();

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		SPIPlanPtr	plan;
		Portal		portal;

		/* Make sure the query is validly encoded */
		pg_verifymbstr(query, strlen(query), false);

		/* Create a cursor for the query */
		plan = SPI_prepare(query, 0, NULL);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed:%s",
				 SPI_result_code_string(SPI_result));

		portal = SPI_cursor_open(NULL, plan, NULL, NULL, false);
		SPI_freeplan(plan);
		if (portal == NULL)
			elog(ERROR, "SPI_cursor_open() failed:%s",
				 SPI_result_code_string(SPI_result));
		cursor = cstr2sv(portal->name);

		PinPortal(portal);

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
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

		/* Punt the error to Perl */
		croak_cstr(edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return cursor;
}


SV *
plperl_spi_fetchrow(char *cursor)
{
	SV		   *row;

	/*
	 * Execute the FETCH inside a sub-transaction, so we can cope with errors
	 * sanely
	 */
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	check_spi_usage_allowed();

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		dTHX;
		Portal		p = SPI_cursor_find(cursor);

		if (!p)
		{
			row = &PL_sv_undef;
		}
		else
		{
			SPI_cursor_fetch(p, true, 1);
			if (SPI_processed == 0)
			{
				UnpinPortal(p);
				SPI_cursor_close(p);
				row = &PL_sv_undef;
			}
			else
			{
				row = plperl_hash_from_tuple(SPI_tuptable->vals[0],
											 SPI_tuptable->tupdesc,
											 true);
			}
			SPI_freetuptable(SPI_tuptable);
		}

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
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

		/* Punt the error to Perl */
		croak_cstr(edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return row;
}

void
plperl_spi_cursor_close(char *cursor)
{
	Portal		p;

	check_spi_usage_allowed();

	p = SPI_cursor_find(cursor);

	if (p)
	{
		UnpinPortal(p);
		SPI_cursor_close(p);
	}
}

SV *
plperl_spi_prepare(char *query, int argc, SV **argv)
{
	volatile SPIPlanPtr plan = NULL;
	volatile MemoryContext plan_cxt = NULL;
	plperl_query_desc *volatile qdesc = NULL;
	plperl_query_entry *volatile hash_entry = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	MemoryContext work_cxt;
	bool		found;
	int			i;

	check_spi_usage_allowed();

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();

		/************************************************************
		 * Allocate the new querydesc structure
		 *
		 * The qdesc struct, as well as all its subsidiary data, lives in its
		 * plan_cxt.  But note that the SPIPlan does not.
		 ************************************************************/
		plan_cxt = AllocSetContextCreate(TopMemoryContext,
										 "PL/Perl spi_prepare query",
										 ALLOCSET_SMALL_SIZES);
		MemoryContextSwitchTo(plan_cxt);
		qdesc = (plperl_query_desc *) palloc0(sizeof(plperl_query_desc));
		snprintf(qdesc->qname, sizeof(qdesc->qname), "%p", qdesc);
		qdesc->plan_cxt = plan_cxt;
		qdesc->nargs = argc;
		qdesc->argtypes = (Oid *) palloc(argc * sizeof(Oid));
		qdesc->arginfuncs = (FmgrInfo *) palloc(argc * sizeof(FmgrInfo));
		qdesc->argtypioparams = (Oid *) palloc(argc * sizeof(Oid));
		MemoryContextSwitchTo(oldcontext);

		/************************************************************
		 * Do the following work in a short-lived context so that we don't
		 * leak a lot of memory in the PL/Perl function's SPI Proc context.
		 ************************************************************/
		work_cxt = AllocSetContextCreate(CurrentMemoryContext,
										 "PL/Perl spi_prepare workspace",
										 ALLOCSET_DEFAULT_SIZES);
		MemoryContextSwitchTo(work_cxt);

		/************************************************************
		 * Resolve argument type names and then look them up by oid
		 * in the system cache, and remember the required information
		 * for input conversion.
		 ************************************************************/
		for (i = 0; i < argc; i++)
		{
			Oid			typId,
						typInput,
						typIOParam;
			int32		typmod;
			char	   *typstr;

			typstr = sv2cstr(argv[i]);
			(void) parseTypeString(typstr, &typId, &typmod, NULL);
			pfree(typstr);

			getTypeInputInfo(typId, &typInput, &typIOParam);

			qdesc->argtypes[i] = typId;
			fmgr_info_cxt(typInput, &(qdesc->arginfuncs[i]), plan_cxt);
			qdesc->argtypioparams[i] = typIOParam;
		}

		/* Make sure the query is validly encoded */
		pg_verifymbstr(query, strlen(query), false);

		/************************************************************
		 * Prepare the plan and check for errors
		 ************************************************************/
		plan = SPI_prepare(query, argc, qdesc->argtypes);

		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed:%s",
				 SPI_result_code_string(SPI_result));

		/************************************************************
		 * Save the plan into permanent memory (right now it's in the
		 * SPI procCxt, which will go away at function end).
		 ************************************************************/
		if (SPI_keepplan(plan))
			elog(ERROR, "SPI_keepplan() failed");
		qdesc->plan = plan;

		/************************************************************
		 * Insert a hashtable entry for the plan.
		 ************************************************************/
		hash_entry = hash_search(plperl_active_interp->query_hash,
								 qdesc->qname,
								 HASH_ENTER, &found);
		hash_entry->query_data = qdesc;

		/* Get rid of workspace */
		MemoryContextDelete(work_cxt);

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Drop anything we managed to allocate */
		if (hash_entry)
			hash_search(plperl_active_interp->query_hash,
						qdesc->qname,
						HASH_REMOVE, NULL);
		if (plan_cxt)
			MemoryContextDelete(plan_cxt);
		if (plan)
			SPI_freeplan(plan);

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/* Punt the error to Perl */
		croak_cstr(edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	/************************************************************
	 * Return the query's hash key to the caller.
	 ************************************************************/
	return cstr2sv(qdesc->qname);
}

HV *
plperl_spi_exec_prepared(char *query, HV *attr, int argc, SV **argv)
{
	HV		   *ret_hv;
	SV		  **sv;
	int			i,
				limit,
				spi_rv;
	char	   *nulls;
	Datum	   *argvalues;
	plperl_query_desc *qdesc;
	plperl_query_entry *hash_entry;

	/*
	 * Execute the query inside a sub-transaction, so we can cope with errors
	 * sanely
	 */
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	check_spi_usage_allowed();

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		dTHX;

		/************************************************************
		 * Fetch the saved plan descriptor, see if it's o.k.
		 ************************************************************/
		hash_entry = hash_search(plperl_active_interp->query_hash, query,
								 HASH_FIND, NULL);
		if (hash_entry == NULL)
			elog(ERROR, "spi_exec_prepared: Invalid prepared query passed");

		qdesc = hash_entry->query_data;
		if (qdesc == NULL)
			elog(ERROR, "spi_exec_prepared: plperl query_hash value vanished");

		if (qdesc->nargs != argc)
			elog(ERROR, "spi_exec_prepared: expected %d argument(s), %d passed",
				 qdesc->nargs, argc);

		/************************************************************
		 * Parse eventual attributes
		 ************************************************************/
		limit = 0;
		if (attr != NULL)
		{
			sv = hv_fetch_string(attr, "limit");
			if (sv && *sv && SvIOK(*sv))
				limit = SvIV(*sv);
		}
		/************************************************************
		 * Set up arguments
		 ************************************************************/
		if (argc > 0)
		{
			nulls = (char *) palloc(argc);
			argvalues = (Datum *) palloc(argc * sizeof(Datum));
		}
		else
		{
			nulls = NULL;
			argvalues = NULL;
		}

		for (i = 0; i < argc; i++)
		{
			bool		isnull;

			argvalues[i] = plperl_sv_to_datum(argv[i],
											  qdesc->argtypes[i],
											  -1,
											  NULL,
											  &qdesc->arginfuncs[i],
											  qdesc->argtypioparams[i],
											  &isnull);
			nulls[i] = isnull ? 'n' : ' ';
		}

		/************************************************************
		 * go
		 ************************************************************/
		spi_rv = SPI_execute_plan(qdesc->plan, argvalues, nulls,
								  current_call_data->prodesc->fn_readonly, limit);
		ret_hv = plperl_spi_execute_fetch_result(SPI_tuptable, SPI_processed,
												 spi_rv);
		if (argc > 0)
		{
			pfree(argvalues);
			pfree(nulls);
		}

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
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

		/* Punt the error to Perl */
		croak_cstr(edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return ret_hv;
}

SV *
plperl_spi_query_prepared(char *query, int argc, SV **argv)
{
	int			i;
	char	   *nulls;
	Datum	   *argvalues;
	plperl_query_desc *qdesc;
	plperl_query_entry *hash_entry;
	SV		   *cursor;
	Portal		portal = NULL;

	/*
	 * Execute the query inside a sub-transaction, so we can cope with errors
	 * sanely
	 */
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	check_spi_usage_allowed();

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		/************************************************************
		 * Fetch the saved plan descriptor, see if it's o.k.
		 ************************************************************/
		hash_entry = hash_search(plperl_active_interp->query_hash, query,
								 HASH_FIND, NULL);
		if (hash_entry == NULL)
			elog(ERROR, "spi_query_prepared: Invalid prepared query passed");

		qdesc = hash_entry->query_data;
		if (qdesc == NULL)
			elog(ERROR, "spi_query_prepared: plperl query_hash value vanished");

		if (qdesc->nargs != argc)
			elog(ERROR, "spi_query_prepared: expected %d argument(s), %d passed",
				 qdesc->nargs, argc);

		/************************************************************
		 * Set up arguments
		 ************************************************************/
		if (argc > 0)
		{
			nulls = (char *) palloc(argc);
			argvalues = (Datum *) palloc(argc * sizeof(Datum));
		}
		else
		{
			nulls = NULL;
			argvalues = NULL;
		}

		for (i = 0; i < argc; i++)
		{
			bool		isnull;

			argvalues[i] = plperl_sv_to_datum(argv[i],
											  qdesc->argtypes[i],
											  -1,
											  NULL,
											  &qdesc->arginfuncs[i],
											  qdesc->argtypioparams[i],
											  &isnull);
			nulls[i] = isnull ? 'n' : ' ';
		}

		/************************************************************
		 * go
		 ************************************************************/
		portal = SPI_cursor_open(NULL, qdesc->plan, argvalues, nulls,
								 current_call_data->prodesc->fn_readonly);
		if (argc > 0)
		{
			pfree(argvalues);
			pfree(nulls);
		}
		if (portal == NULL)
			elog(ERROR, "SPI_cursor_open() failed:%s",
				 SPI_result_code_string(SPI_result));

		cursor = cstr2sv(portal->name);

		PinPortal(portal);

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
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

		/* Punt the error to Perl */
		croak_cstr(edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return cursor;
}

void
plperl_spi_freeplan(char *query)
{
	SPIPlanPtr	plan;
	plperl_query_desc *qdesc;
	plperl_query_entry *hash_entry;

	check_spi_usage_allowed();

	hash_entry = hash_search(plperl_active_interp->query_hash, query,
							 HASH_FIND, NULL);
	if (hash_entry == NULL)
		elog(ERROR, "spi_freeplan: Invalid prepared query passed");

	qdesc = hash_entry->query_data;
	if (qdesc == NULL)
		elog(ERROR, "spi_freeplan: plperl query_hash value vanished");
	plan = qdesc->plan;

	/*
	 * free all memory before SPI_freeplan, so if it dies, nothing will be
	 * left over
	 */
	hash_search(plperl_active_interp->query_hash, query,
				HASH_REMOVE, NULL);

	MemoryContextDelete(qdesc->plan_cxt);

	SPI_freeplan(plan);
}

void
plperl_spi_commit(void)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	check_spi_usage_allowed();

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

		/* Punt the error to Perl */
		croak_cstr(edata->message);
	}
	PG_END_TRY();
}

void
plperl_spi_rollback(void)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	check_spi_usage_allowed();

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

		/* Punt the error to Perl */
		croak_cstr(edata->message);
	}
	PG_END_TRY();
}

/*
 * Implementation of plperl's elog() function
 *
 * If the error level is less than ERROR, we'll just emit the message and
 * return.  When it is ERROR, elog() will longjmp, which we catch and
 * turn into a Perl croak().  Note we are assuming that elog() can't have
 * any internal failures that are so bad as to require a transaction abort.
 *
 * The main reason this is out-of-line is to avoid conflicts between XSUB.h
 * and the PG_TRY macros.
 */
void
plperl_util_elog(int level, SV *msg)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	char	   *volatile cmsg = NULL;

	/*
	 * We intentionally omit check_spi_usage_allowed() here, as this seems
	 * safe to allow even in the contexts that that function rejects.
	 */

	PG_TRY();
	{
		cmsg = sv2cstr(msg);
		elog(level, "%s", cmsg);
		pfree(cmsg);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Must reset elog.c's state */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		if (cmsg)
			pfree(cmsg);

		/* Punt the error to Perl */
		croak_cstr(edata->message);
	}
	PG_END_TRY();
}

/*
 * Store an SV into a hash table under a key that is a string assumed to be
 * in the current database's encoding.
 */
static SV **
hv_store_string(HV *hv, const char *key, SV *val)
{
	dTHX;
	int32		hlen;
	char	   *hkey;
	SV		  **ret;

	hkey = pg_server_to_any(key, strlen(key), PG_UTF8);

	/*
	 * hv_store() recognizes a negative klen parameter as meaning a UTF-8
	 * encoded key.
	 */
	hlen = -(int) strlen(hkey);
	ret = hv_store(hv, hkey, hlen, val, 0);

	if (hkey != key)
		pfree(hkey);

	return ret;
}

/*
 * Fetch an SV from a hash table under a key that is a string assumed to be
 * in the current database's encoding.
 */
static SV **
hv_fetch_string(HV *hv, const char *key)
{
	dTHX;
	int32		hlen;
	char	   *hkey;
	SV		  **ret;

	hkey = pg_server_to_any(key, strlen(key), PG_UTF8);

	/* See notes in hv_store_string */
	hlen = -(int) strlen(hkey);
	ret = hv_fetch(hv, hkey, hlen, 0);

	if (hkey != key)
		pfree(hkey);

	return ret;
}

/*
 * Provide function name for PL/Perl execution errors
 */
static void
plperl_exec_callback(void *arg)
{
	char	   *procname = (char *) arg;

	if (procname)
		errcontext("PL/Perl function \"%s\"", procname);
}

/*
 * Provide function name for PL/Perl compilation errors
 */
static void
plperl_compile_callback(void *arg)
{
	char	   *procname = (char *) arg;

	if (procname)
		errcontext("compilation of PL/Perl function \"%s\"", procname);
}

/*
 * Provide error context for the inline handler
 */
static void
plperl_inline_callback(void *arg)
{
	errcontext("PL/Perl anonymous code block");
}


/*
 * Perl's own setlocale(), copied from POSIX.xs
 * (needed because of the calls to new_*())
 *
 * Starting in 5.28, perl exposes Perl_setlocale to do so.
 */
#if defined(WIN32) && PERL_VERSION_LT(5, 28, 0)
static char *
setlocale_perl(int category, char *locale)
{
	dTHX;
	char	   *RETVAL = setlocale(category, locale);

	if (RETVAL)
	{
#ifdef USE_LOCALE_CTYPE
		if (category == LC_CTYPE
#ifdef LC_ALL
			|| category == LC_ALL
#endif
			)
		{
			char	   *newctype;

#ifdef LC_ALL
			if (category == LC_ALL)
				newctype = setlocale(LC_CTYPE, NULL);
			else
#endif
				newctype = RETVAL;
			new_ctype(newctype);
		}
#endif							/* USE_LOCALE_CTYPE */
#ifdef USE_LOCALE_COLLATE
		if (category == LC_COLLATE
#ifdef LC_ALL
			|| category == LC_ALL
#endif
			)
		{
			char	   *newcoll;

#ifdef LC_ALL
			if (category == LC_ALL)
				newcoll = setlocale(LC_COLLATE, NULL);
			else
#endif
				newcoll = RETVAL;
			new_collate(newcoll);
		}
#endif							/* USE_LOCALE_COLLATE */

#ifdef USE_LOCALE_NUMERIC
		if (category == LC_NUMERIC
#ifdef LC_ALL
			|| category == LC_ALL
#endif
			)
		{
			char	   *newnum;

#ifdef LC_ALL
			if (category == LC_ALL)
				newnum = setlocale(LC_NUMERIC, NULL);
			else
#endif
				newnum = RETVAL;
			new_numeric(newnum);
		}
#endif							/* USE_LOCALE_NUMERIC */
	}

	return RETVAL;
}
#endif							/* defined(WIN32) && PERL_VERSION_LT(5, 28, 0) */
