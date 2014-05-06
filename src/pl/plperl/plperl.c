/**********************************************************************
 * plperl.c - perl as a procedural language for PostgreSQL
 *
 *	  $PostgreSQL: pgsql/src/pl/plperl/plperl.c,v 1.150.2.8 2010/05/17 20:46:53 adunstan Exp $
 *
 **********************************************************************/

#include "postgres.h"
/* Defined by Perl */
#undef _

/* system stuff */
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>

/* postgreSQL stuff */
#include "access/xact.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plperl")

/* perl stuff */
#include "plperl.h"
/* defines PLPERL_SET_OPMASK */
#include "plperl_opmask.h"

EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
EXTERN_C void boot_SPI(pTHX_ CV *cv);

PG_MODULE_MAGIC;


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
	Oid			user_id;				/* Hash key (must be first!) */
	PerlInterpreter *interp;			/* The interpreter */
	HTAB	   *query_hash;				/* plperl_query_entry structs */
} plperl_interp_desc;


/**********************************************************************
 * The information we cache about loaded procedures
 *
 * The refcount field counts the struct's reference from the hash table shown
 * below, plus one reference for each function call level that is using the
 * struct.  We can release the struct, and the associated Perl sub, when the
 * refcount goes to zero.
 **********************************************************************/
typedef struct plperl_proc_desc
{
	char	   *proname;		/* user name of procedure */
	TransactionId fn_xmin;		/* xmin/TID of procedure's pg_proc tuple */
	ItemPointerData fn_tid;
	int			refcount;		/* reference count of this struct */
	SV		   *reference;		/* CODE reference for Perl sub */
	plperl_interp_desc *interp;	/* interpreter it's created in */
	bool		fn_readonly;	/* is function readonly (not volatile)? */
	bool		lanpltrusted;	/* is it plperl, rather than plperlu? */
	bool		fn_retistuple;	/* true, if function returns tuple */
	bool		fn_retisset;	/* true, if function returns set */
	bool		fn_retisarray;	/* true if function returns array */
	/* Conversion info for function's result type: */
	Oid			result_oid;		/* Oid of result type */
	FmgrInfo	result_in_func; /* I/O function and arg for result type */
	Oid			result_typioparam;
	/* Conversion info for function's argument types: */
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	bool		arg_is_rowtype[FUNC_MAX_ARGS];
} plperl_proc_desc;

#define increment_prodesc_refcount(prodesc)  \
	((prodesc)->refcount++)
#define decrement_prodesc_refcount(prodesc)  \
	do { \
		if (--((prodesc)->refcount) <= 0) \
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
	Oid			proc_id;				/* Function OID */
	/*
	 * is_trigger is really a bool, but declare as Oid to ensure this struct
	 * contains no padding
	 */
	Oid			is_trigger;				/* is it a trigger function? */
	Oid			user_id;				/* User calling the function, or 0 */
} plperl_proc_key;

typedef struct plperl_proc_ptr
{
	plperl_proc_key proc_key;			/* Hash key (must be first!) */
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
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	AttInMetadata *attinmeta;
	MemoryContext tmp_cxt;
} plperl_call_data;

/**********************************************************************
 * The information we cache about prepared and saved plans
 **********************************************************************/
typedef struct plperl_query_desc
{
	char		qname[24];
	MemoryContext plan_cxt;		/* context holding this struct */
	void	   *plan;
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
 * Global data
 **********************************************************************/

static HTAB *plperl_interp_hash = NULL;
static HTAB *plperl_proc_hash = NULL;
static plperl_interp_desc *plperl_active_interp = NULL;
/* If we have an unassigned "held" interpreter, it's stored here */
static PerlInterpreter *plperl_held_interp = NULL;

/* GUC variables */
static bool plperl_use_strict = false;

static OP  *(*pp_require_orig) (pTHX) = NULL;
static char plperl_opmask[MAXO];

/* this is saved and restored by plperl_call_handler */
static plperl_call_data *current_call_data = NULL;

/**********************************************************************
 * Forward declarations
 **********************************************************************/
Datum		plperl_call_handler(PG_FUNCTION_ARGS);
Datum		plperl_validator(PG_FUNCTION_ARGS);
void		_PG_init(void);

static PerlInterpreter *plperl_init_interp(void);
static void set_interp_require(bool trusted);

static Datum plperl_func_handler(PG_FUNCTION_ARGS);
static Datum plperl_trigger_handler(PG_FUNCTION_ARGS);

static void free_plperl_function(plperl_proc_desc *prodesc);

static plperl_proc_desc *compile_plperl_function(Oid fn_oid, bool is_trigger);

static SV  *plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc);
static void plperl_init_shared_libs(pTHX);
static void plperl_trusted_init(void);
static void plperl_untrusted_init(void);
static HV  *plperl_spi_execute_fetch_result(SPITupleTable *, int, int);
static SV  *newSVstring(const char *str);
static SV **hv_store_string(HV *hv, const char *key, SV *val);
static SV **hv_fetch_string(HV *hv, const char *key);
static void plperl_create_sub(plperl_proc_desc *desc, char *s, Oid fn_oid);
static SV  *plperl_call_perl_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo);
static char *strip_trailing_ws(const char *msg);
static OP  *pp_require_safe(pTHX);
static void activate_interpreter(plperl_interp_desc *interp_desc);

#ifdef WIN32
static char *setlocale_perl(int category, char *locale);
#endif

/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the cached form of plperl functions/queries is allocated permanently
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
	static bool inited = false;
	HASHCTL		hash_ctl;

	if (inited)
		return;

	pg_bindtextdomain(TEXTDOMAIN);

	DefineCustomBoolVariable("plperl.use_strict",
							 gettext_noop("If true, trusted and untrusted Perl code will be compiled in strict mode."),
							 NULL,
							 &plperl_use_strict,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL);

	EmitWarningsOnPlaceholders("plperl");

	/*
	 * Create hash tables.
	 */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(plperl_interp_desc);
	hash_ctl.hash = oid_hash;
	plperl_interp_hash = hash_create("PL/Perl interpreters",
									 8,
									 &hash_ctl,
									 HASH_ELEM | HASH_FUNCTION);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(plperl_proc_key);
	hash_ctl.entrysize = sizeof(plperl_proc_ptr);
	hash_ctl.hash = tag_hash;
	plperl_proc_hash = hash_create("PL/Perl procedures",
								   32,
								   &hash_ctl,
								   HASH_ELEM | HASH_FUNCTION);

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

/* Each of these macros must represent a single string literal */

#define PERLBOOT \
	"SPI::bootstrap(); use vars qw(%_SHARED);" \
	"sub ::plperl_warn { my $msg = shift; " \
	"       $msg =~ s/\\(eval \\d+\\) //g; &elog(&NOTICE, $msg); } " \
	"$SIG{__WARN__} = \\&::plperl_warn; " \
	"sub ::plperl_die { my $msg = shift; " \
	"       $msg =~ s/\\(eval \\d+\\) //g; die $msg; } " \
	"$SIG{__DIE__} = \\&::plperl_die; " \
	"sub ::mkfunc {" \
	"      my $ret = eval(qq[ sub { $_[0] $_[1] } ]); " \
	"      $@ =~ s/\\(eval \\d+\\) //g if $@; return $ret; }" \
	"use strict; " \
	"sub ::mk_strict_func {" \
	"      my $ret = eval(qq[ sub { use strict; $_[0] $_[1] } ]); " \
	"      $@ =~ s/\\(eval \\d+\\) //g if $@; return $ret; } " \
	"sub ::_plperl_to_pg_array {" \
	"  my $arg = shift; ref $arg eq 'ARRAY' || return $arg; " \
	"  my $res = ''; my $first = 1; " \
	"  foreach my $elem (@$arg) " \
	"  { " \
	"    $res .= ', ' unless $first; $first = undef; " \
	"    if (ref $elem) " \
	"    { " \
	"      $res .= _plperl_to_pg_array($elem); " \
	"    } " \
	"    elsif (defined($elem)) " \
	"    { " \
	"      my $str = qq($elem); " \
	"      $str =~ s/([\"\\\\])/\\\\$1/g; " \
	"      $res .= qq(\"$str\"); " \
	"    } " \
	"    else " \
	"    { "\
	"      $res .= 'NULL' ; " \
	"    } "\
	"  } " \
	"  return qq({$res}); " \
	"} "

#define PLC_TRUSTED \
	"require strict; "


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

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = NAMEDATALEN;
		hash_ctl.entrysize = sizeof(plperl_query_entry);
		interp_desc->query_hash = hash_create("PL/Perl queries",
											  32,
											  &hash_ctl,
											  HASH_ELEM);
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
		elog(ERROR,
			 "cannot allocate multiple Perl interpreters on this platform");
#endif
	}

	set_interp_require(trusted);

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
	static int	perl_sys_init_done;

	static char *embedding[3] = {
		"", "-e", PERLBOOT
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
	 * We restore them using Perl's perl_setlocale() function so that Perl
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
#endif

	/****
	 * The perl API docs state that PERL_SYS_INIT3 should be called before
	 * allocating interprters. Unfortunately, on some platforms this fails
	 * in the Perl_do_taint() routine, which is called when the platform is
	 * using the system's malloc() instead of perl's own. Other platforms,
	 * notably Windows, fail if PERL_SYS_INIT3 is not called. So we call it
	 * if it's available, unless perl is using the system malloc(), which is
	 * true when MYMALLOC is set.
	 */
#if defined(PERL_SYS_INIT3) && !defined(MYMALLOC)
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
#endif

	plperl = perl_alloc();
	if (!plperl)
		elog(ERROR, "could not allocate Perl interpreter");

	PERL_SET_CONTEXT(plperl);
	perl_construct(plperl);

	/*
	 * Record the original function for the 'require' and 'dofile' opcodes.
	 * (They share the same implementation.) Ensure it's used for new
	 * interpreters.
	 */
	if (!pp_require_orig)
		pp_require_orig = PL_ppaddr[OP_REQUIRE];
	else
	{
		PL_ppaddr[OP_REQUIRE] = pp_require_orig;
		PL_ppaddr[OP_DOFILE] = pp_require_orig;
	}

	if (perl_parse(plperl, plperl_init_shared_libs,
				   nargs, embedding, NULL) != 0)
		ereport(ERROR,
				(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
				 errcontext("while parsing Perl initialization")));

	if (perl_run(plperl) != 0)
		ereport(ERROR,
				(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
				 errcontext("while running Perl initialization")));

#ifdef PLPERL_RESTORE_LOCALE
	PLPERL_RESTORE_LOCALE(LC_COLLATE, save_collate);
	PLPERL_RESTORE_LOCALE(LC_CTYPE, save_ctype);
	PLPERL_RESTORE_LOCALE(LC_MONETARY, save_monetary);
	PLPERL_RESTORE_LOCALE(LC_NUMERIC, save_numeric);
	PLPERL_RESTORE_LOCALE(LC_TIME, save_time);
#endif

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
}

/*
 * Initialize the current Perl interpreter as a trusted interp
 */
static void
plperl_trusted_init(void)
{
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
				(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
				 errcontext("while executing PLC_TRUSTED")));

	if (GetDatabaseEncoding() == PG_UTF8)
	{
		/*
		 * Force loading of utf8 module now to prevent errors that can arise
		 * from the regex code later trying to load utf8 modules. See
		 * http://rt.perl.org/rt3/Ticket/Display.html?id=47576
		 */
		eval_pv("my $a=chr(0x100); return $a =~ /\\xa9/i", FALSE);
		if (SvTRUE(ERRSV))
			ereport(ERROR,
					(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
					 errcontext("while executing utf8fix")));
	}

	/*
	 * Lock down the interpreter
	 */

	/* switch to the safe require/dofile opcode for future code */
	PL_ppaddr[OP_REQUIRE] = pp_require_safe;
	PL_ppaddr[OP_DOFILE] = pp_require_safe;

	/*
	 * prevent (any more) unsafe opcodes being compiled
	 * PL_op_mask is per interpreter, so this only needs to be set once
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
#ifdef PL_stashcache
	hv_clear(PL_stashcache);
#endif
}


/*
 * Initialize the current Perl interpreter as an untrusted interp
 */
static void
plperl_untrusted_init(void)
{
	/*
	 * Nothing to do here
	 */
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
plperl_build_tuple_result(HV *perlhash, AttInMetadata *attinmeta)
{
	TupleDesc	td = attinmeta->tupdesc;
	char	  **values;
	SV		   *val;
	char	   *key;
	I32			klen;
	HeapTuple	tup;

	values = (char **) palloc0(td->natts * sizeof(char *));

	hv_iterinit(perlhash);
	while ((val = hv_iternextsv(perlhash, &key, &klen)))
	{
		int			attn = SPI_fnumber(td, key);

		if (attn <= 0 || td->attrs[attn - 1]->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("Perl hash contains nonexistent column \"%s\"",
							key)));
		if (SvOK(val))
			values[attn - 1] = SvPV(val, PL_na);
	}
	hv_iterinit(perlhash);

	tup = BuildTupleFromCStrings(attinmeta, values);
	pfree(values);
	return tup;
}

/*
 * convert perl array to postgres string representation
 */
static SV  *
plperl_convert_to_pg_array(SV *src)
{
	SV		   *rv;
	int			count;

	dSP;

	PUSHMARK(SP);
	XPUSHs(src);
	PUTBACK;

	count = call_pv("::_plperl_to_pg_array", G_SCALAR);

	SPAGAIN;

	if (count != 1)
		elog(ERROR, "unexpected _plperl_to_pg_array failure");

	rv = POPs;

	PUTBACK;

	return rv;
}


/* Set up the arguments for a trigger call. */

static SV  *
plperl_trigger_build_args(FunctionCallInfo fcinfo)
{
	TriggerData *tdata;
	TupleDesc	tupdesc;
	int			i;
	char	   *level;
	char	   *event;
	char	   *relid;
	char	   *when;
	HV		   *hv;

	hv = newHV();

	tdata = (TriggerData *) fcinfo->context;
	tupdesc = tdata->tg_relation->rd_att;

	relid = DatumGetCString(
							DirectFunctionCall1(oidout,
								  ObjectIdGetDatum(tdata->tg_relation->rd_id)
												)
		);

	hv_store_string(hv, "name", newSVstring(tdata->tg_trigger->tgname));
	hv_store_string(hv, "relid", newSVstring(relid));

	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
	{
		event = "INSERT";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
			hv_store_string(hv, "new",
							plperl_hash_from_tuple(tdata->tg_trigtuple,
												   tupdesc));
	}
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
	{
		event = "DELETE";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
			hv_store_string(hv, "old",
							plperl_hash_from_tuple(tdata->tg_trigtuple,
												   tupdesc));
	}
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
	{
		event = "UPDATE";
		if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		{
			hv_store_string(hv, "old",
							plperl_hash_from_tuple(tdata->tg_trigtuple,
												   tupdesc));
			hv_store_string(hv, "new",
							plperl_hash_from_tuple(tdata->tg_newtuple,
												   tupdesc));
		}
	}
	else if (TRIGGER_FIRED_BY_TRUNCATE(tdata->tg_event))
		event = "TRUNCATE";
	else
		event = "UNKNOWN";

	hv_store_string(hv, "event", newSVstring(event));
	hv_store_string(hv, "argc", newSViv(tdata->tg_trigger->tgnargs));

	if (tdata->tg_trigger->tgnargs > 0)
	{
		AV		   *av = newAV();

		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
			av_push(av, newSVstring(tdata->tg_trigger->tgargs[i]));
		hv_store_string(hv, "args", newRV_noinc((SV *) av));
	}

	hv_store_string(hv, "relname",
					newSVstring(SPI_getrelname(tdata->tg_relation)));

	hv_store_string(hv, "table_name",
					newSVstring(SPI_getrelname(tdata->tg_relation)));

	hv_store_string(hv, "table_schema",
					newSVstring(SPI_getnspname(tdata->tg_relation)));

	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		when = "BEFORE";
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		when = "AFTER";
	else
		when = "UNKNOWN";
	hv_store_string(hv, "when", newSVstring(when));

	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		level = "ROW";
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		level = "STATEMENT";
	else
		level = "UNKNOWN";
	hv_store_string(hv, "level", newSVstring(level));

	return newRV_noinc((SV *) hv);
}


/* Set up the new tuple returned from a trigger. */

static HeapTuple
plperl_modify_tuple(HV *hvTD, TriggerData *tdata, HeapTuple otup)
{
	SV		  **svp;
	HV		   *hvNew;
	HeapTuple	rtup;
	SV		   *val;
	char	   *key;
	I32			klen;
	int			slotsused;
	int		   *modattrs;
	Datum	   *modvalues;
	char	   *modnulls;

	TupleDesc	tupdesc;

	tupdesc = tdata->tg_relation->rd_att;

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

	modattrs = palloc(tupdesc->natts * sizeof(int));
	modvalues = palloc(tupdesc->natts * sizeof(Datum));
	modnulls = palloc(tupdesc->natts * sizeof(char));
	slotsused = 0;

	hv_iterinit(hvNew);
	while ((val = hv_iternextsv(hvNew, &key, &klen)))
	{
		int			attn = SPI_fnumber(tupdesc, key);
		Oid			typinput;
		Oid			typioparam;
		int32		atttypmod;
		FmgrInfo	finfo;

		if (attn <= 0 || tupdesc->attrs[attn - 1]->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("Perl hash contains nonexistent column \"%s\"",
							key)));
		/* XXX would be better to cache these lookups */
		getTypeInputInfo(tupdesc->attrs[attn - 1]->atttypid,
						 &typinput, &typioparam);
		fmgr_info(typinput, &finfo);
		atttypmod = tupdesc->attrs[attn - 1]->atttypmod;
		if (SvOK(val))
		{
			modvalues[slotsused] = InputFunctionCall(&finfo,
													 SvPV(val, PL_na),
													 typioparam,
													 atttypmod);
			modnulls[slotsused] = ' ';
		}
		else
		{
			modvalues[slotsused] = InputFunctionCall(&finfo,
													 NULL,
													 typioparam,
													 atttypmod);
			modnulls[slotsused] = 'n';
		}
		modattrs[slotsused] = attn;
		slotsused++;
	}
	hv_iterinit(hvNew);

	rtup = SPI_modifytuple(tdata->tg_relation, otup, slotsused,
						   modattrs, modvalues, modnulls);

	pfree(modattrs);
	pfree(modvalues);
	pfree(modnulls);

	if (rtup == NULL)
		elog(ERROR, "SPI_modifytuple failed: %s",
			 SPI_result_code_string(SPI_result));

	return rtup;
}


/*
 * This is the only externally-visible part of the plperl call interface.
 * The Postgres function and trigger managers call it to execute a
 * perl function.
 */
PG_FUNCTION_INFO_V1(plperl_call_handler);

Datum
plperl_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	plperl_call_data *save_call_data = current_call_data;
	plperl_interp_desc *oldinterp = plperl_active_interp;
	plperl_call_data this_call_data;

	/* Initialize current-call status record */
	MemSet(&this_call_data, 0, sizeof(this_call_data));
	this_call_data.fcinfo = fcinfo;

	PG_TRY();
	{
		current_call_data = &this_call_data;
		if (CALLED_AS_TRIGGER(fcinfo))
			retval = PointerGetDatum(plperl_trigger_handler(fcinfo));
		else
			retval = plperl_func_handler(fcinfo);
	}
	PG_CATCH();
	{
		if (this_call_data.prodesc)
			decrement_prodesc_refcount(this_call_data.prodesc);
		current_call_data = save_call_data;
		activate_interpreter(oldinterp);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (this_call_data.prodesc)
		decrement_prodesc_refcount(this_call_data.prodesc);
	current_call_data = save_call_data;
	activate_interpreter(oldinterp);
	return retval;
}

/*
 * This is the other externally visible function - it is called when CREATE
 * FUNCTION is issued to validate the function being created/replaced.
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
	bool		istrigger = false;
	int			i;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(funcoid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, or VOID */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			istrigger = true;
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
		if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/Perl functions cannot accept type %s",
							format_type_be(argtypes[i]))));
	}

	ReleaseSysCache(tuple);

	/* Postpone body checks if !check_function_bodies */
	if (check_function_bodies)
	{
		(void) compile_plperl_function(funcoid, istrigger);
	}

	/* the result of a validator is ignored */
	PG_RETURN_VOID();
}


/*
 * Uses mkfunc to create an anonymous sub whose text is
 * supplied in s, and returns a reference to the closure.
 */
static void
plperl_create_sub(plperl_proc_desc *prodesc, char *s, Oid fn_oid)
{
	dSP;
	char		subname[NAMEDATALEN + 40];
	SV		   *subref;
	int			count;
	char	   *compile_sub;

	sprintf(subname, "%s__%u", prodesc->proname, fn_oid);

	ENTER;
	SAVETMPS;
	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newSVstring("our $_TD; local $_TD=$_[0]; shift;")));
	XPUSHs(sv_2mortal(newSVstring(s)));
	PUTBACK;

	/*
	 * G_KEEPERR seems to be needed here, else we don't recognize compile
	 * errors properly.  Perhaps it's because there's another level of eval
	 * inside mksafefunc?
	 */

	if (plperl_use_strict)
		compile_sub = "::mk_strict_func";
	else
		compile_sub = "::mkfunc";

	count = perl_call_pv(compile_sub, G_SCALAR | G_EVAL | G_KEEPERR);
	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "didn't get a return item from mksafefunc");
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("creation of Perl function \"%s\" failed: %s",
						prodesc->proname,
						strip_trailing_ws(SvPV(ERRSV, PL_na)))));
	}

	/*
	 * need to make a deep copy of the return. it comes off the stack as a
	 * temporary.
	 */
	subref = newSVsv(POPs);

	if (!SvROK(subref) || SvTYPE(SvRV(subref)) != SVt_PVCV)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;

		/*
		 * subref is our responsibility because it is not mortal
		 */
		SvREFCNT_dec(subref);
		elog(ERROR, "didn't get a code ref");
	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	prodesc->reference = subref;
}


/**********************************************************************
 * plperl_init_shared_libs()		-
 *
 * We cannot use the DynaLoader directly to get at the Opcode
 * module. So, we link Opcode into ourselves
 * and do the initialization behind perl's back.
 *
 **********************************************************************/
static void
plperl_init_shared_libs(pTHX)
{
	char	   *file = __FILE__;

	newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
	newXS("SPI::bootstrap", boot_SPI, file);
}


static SV  *
plperl_call_perl_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo)
{
	dSP;
	SV		   *retval;
	int			i;
	int			count;
	SV		   *sv;

	ENTER;
	SAVETMPS;

	PUSHMARK(SP);

	XPUSHs(&PL_sv_undef);		/* no trigger data */

	for (i = 0; i < desc->nargs; i++)
	{
		if (fcinfo->argnull[i])
			XPUSHs(&PL_sv_undef);
		else if (desc->arg_is_rowtype[i])
		{
			HeapTupleHeader td;
			Oid			tupType;
			int32		tupTypmod;
			TupleDesc	tupdesc;
			HeapTupleData tmptup;
			SV		   *hashref;

			td = DatumGetHeapTupleHeader(fcinfo->arg[i]);
			/* Extract rowtype info and find a tupdesc */
			tupType = HeapTupleHeaderGetTypeId(td);
			tupTypmod = HeapTupleHeaderGetTypMod(td);
			tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
			/* Build a temporary HeapTuple control structure */
			tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
			tmptup.t_data = td;

			hashref = plperl_hash_from_tuple(&tmptup, tupdesc);
			XPUSHs(sv_2mortal(hashref));
			ReleaseTupleDesc(tupdesc);
		}
		else
		{
			char	   *tmp;

			tmp = OutputFunctionCall(&(desc->arg_out_func[i]),
									 fcinfo->arg[i]);
			sv = newSVstring(tmp);
			XPUSHs(sv_2mortal(sv));
			pfree(tmp);
		}
	}
	PUTBACK;

	/* Do NOT use G_KEEPERR here */
	count = perl_call_sv(desc->reference, G_SCALAR | G_EVAL);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "didn't get a return item from function");
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		/* XXX need to find a way to assign an errcode here */
		ereport(ERROR,
				(errmsg("error from Perl function \"%s\": %s",
						desc->proname,
						strip_trailing_ws(SvPV(ERRSV, PL_na)))));
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
	dSP;
	SV		   *retval;
	Trigger    *tg_trigger;
	int			i;
	int			count;

	ENTER;
	SAVETMPS;

	PUSHMARK(sp);

	XPUSHs(td);

	tg_trigger = ((TriggerData *) fcinfo->context)->tg_trigger;
	for (i = 0; i < tg_trigger->tgnargs; i++)
		XPUSHs(sv_2mortal(newSVstring(tg_trigger->tgargs[i])));
	PUTBACK;

	/* Do NOT use G_KEEPERR here */
	count = perl_call_sv(desc->reference, G_SCALAR | G_EVAL);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "didn't get a return item from trigger function");
	}

	if (SvTRUE(ERRSV))
	{
		(void) POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		/* XXX need to find a way to assign an errcode here */
		ereport(ERROR,
				(errmsg("error from Perl function \"%s\": %s",
						desc->proname,
						strip_trailing_ws(SvPV(ERRSV, PL_na)))));
	}

	retval = newSVsv(POPs);

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retval;
}


static Datum
plperl_func_handler(PG_FUNCTION_ARGS)
{
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval;
	ReturnSetInfo *rsi;
	SV		   *array_ret = NULL;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, false);
	current_call_data->prodesc = prodesc;
	increment_prodesc_refcount(prodesc);

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (prodesc->fn_retisset)
	{
		/* Check context before allowing the call to go through */
		if (!rsi || !IsA(rsi, ReturnSetInfo) ||
			(rsi->allowedModes & SFRM_Materialize) == 0 ||
			rsi->expectedDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that "
							"cannot accept a set")));
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
		/*
		 * If the Perl function returned an arrayref, we pretend that it
		 * called return_next() for each element of the array, to handle old
		 * SRFs that didn't know about return_next(). Any other sort of return
		 * value is an error, except undef which means return an empty set.
		 */
		if (SvOK(perlret) &&
			SvROK(perlret) &&
			SvTYPE(SvRV(perlret)) == SVt_PVAV)
		{
			int			i = 0;
			SV		  **svp = 0;
			AV		   *rav = (AV *) SvRV(perlret);

			while ((svp = av_fetch(rav, i, FALSE)) != NULL)
			{
				plperl_return_next(*svp);
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
	else if (!SvOK(perlret))
	{
		/* Return NULL if Perl code returned undef */
		if (rsi && IsA(rsi, ReturnSetInfo))
			rsi->isDone = ExprEndResult;
		retval = InputFunctionCall(&prodesc->result_in_func, NULL,
								   prodesc->result_typioparam, -1);
		fcinfo->isnull = true;
	}
	else if (prodesc->fn_retistuple)
	{
		/* Return a perl hash converted to a Datum */
		TupleDesc	td;
		AttInMetadata *attinmeta;
		HeapTuple	tup;

		if (!SvOK(perlret) || !SvROK(perlret) ||
			SvTYPE(SvRV(perlret)) != SVt_PVHV)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("composite-returning PL/Perl function "
							"must return reference to hash")));
		}

		/* XXX should cache the attinmeta data instead of recomputing */
		if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		}

		attinmeta = TupleDescGetAttInMetadata(td);
		tup = plperl_build_tuple_result((HV *) SvRV(perlret), attinmeta);
		retval = HeapTupleGetDatum(tup);
	}
	else
	{
		/* Return a perl string converted to a Datum */
		char	   *val;

		if (prodesc->fn_retisarray && SvROK(perlret) &&
			SvTYPE(SvRV(perlret)) == SVt_PVAV)
		{
			array_ret = plperl_convert_to_pg_array(perlret);
			SvREFCNT_dec(perlret);
			perlret = array_ret;
		}

		val = SvPV(perlret, PL_na);

		retval = InputFunctionCall(&prodesc->result_in_func, val,
								   prodesc->result_typioparam, -1);
	}

	if (array_ret == NULL)
		SvREFCNT_dec(perlret);

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

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, true);
	current_call_data->prodesc = prodesc;
	increment_prodesc_refcount(prodesc);

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

		tmp = SvPV(perlret, PL_na);

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
	}

	SvREFCNT_dec(svTD);
	if (perlret)
		SvREFCNT_dec(perlret);

	return retval;
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
		uptodate = (prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
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
	Assert(prodesc->refcount <= 0);
	/* Release CODE reference, if we have one, from the appropriate interp */
	if (prodesc->reference)
	{
		plperl_interp_desc *oldinterp = plperl_active_interp;

		activate_interpreter(prodesc->interp);
		SvREFCNT_dec(prodesc->reference);
		activate_interpreter(oldinterp);
	}
	/* Get rid of what we conveniently can of our own structs */
	/* (FmgrInfo subsidiary info will get leaked ...) */
	if (prodesc->proname)
		free(prodesc->proname);
	free(prodesc);
}


static plperl_proc_desc *
compile_plperl_function(Oid fn_oid, bool is_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	plperl_proc_key proc_key;
	plperl_proc_ptr *proc_ptr;
	plperl_proc_desc *prodesc = NULL;
	int			i;
	plperl_interp_desc *oldinterp = plperl_active_interp;

	/* We'll need the pg_proc tuple in any case... */
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(fn_oid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/* Try to find function in plperl_proc_hash */
	proc_key.proc_id = fn_oid;
	proc_key.is_trigger = is_trigger;
	proc_key.user_id = GetUserId();

	proc_ptr = hash_search(plperl_proc_hash, &proc_key,
						   HASH_FIND, NULL);

	if (validate_plperl_function(proc_ptr, procTup))
		prodesc = proc_ptr->proc_ptr;
	else
	{
		/* If not found or obsolete, maybe it's plperlu */
		proc_key.user_id = InvalidOid;
		proc_ptr = hash_search(plperl_proc_hash, &proc_key,
							   HASH_FIND, NULL);
		if (validate_plperl_function(proc_ptr, procTup))
			prodesc = proc_ptr->proc_ptr;
	}

	/************************************************************
	 * If we haven't found it in the hashtable, we analyze
	 * the function's arguments and return type and store
	 * the in-/out-functions in the prodesc block and create
	 * a new hashtable entry for it.
	 *
	 * Then we load the procedure into the Perl interpreter.
	 ************************************************************/
	if (prodesc == NULL)
	{
		HeapTuple	langTup;
		HeapTuple	typeTup;
		Form_pg_language langStruct;
		Form_pg_type typeStruct;
		Datum		prosrcdatum;
		bool		isnull;
		char	   *proc_source;

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (plperl_proc_desc *) malloc(sizeof(plperl_proc_desc));
		if (prodesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		/* Initialize all fields to 0 so free_plperl_function is safe */
		MemSet(prodesc, 0, sizeof(plperl_proc_desc));

		prodesc->proname = strdup(NameStr(procStruct->proname));
		if (prodesc->proname == NULL)
		{
			free_plperl_function(prodesc);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		prodesc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		prodesc->fn_tid = procTup->t_self;

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
			free_plperl_function(prodesc);
			elog(ERROR, "cache lookup failed for language %u",
				 procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		prodesc->lanpltrusted = langStruct->lanpltrusted;
		ReleaseSysCache(langTup);

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
				free_plperl_function(prodesc);
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			}
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID or RECORD */
			if (typeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (procStruct->prorettype == VOIDOID ||
					procStruct->prorettype == RECORDOID)
					 /* okay */ ;
				else if (procStruct->prorettype == TRIGGEROID)
				{
					free_plperl_function(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called "
									"as triggers")));
				}
				else
				{
					free_plperl_function(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Perl functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}

			prodesc->result_oid = procStruct->prorettype;
			prodesc->fn_retisset = procStruct->proretset;
			prodesc->fn_retistuple = (procStruct->prorettype == RECORDOID ||
								   typeStruct->typtype == TYPTYPE_COMPOSITE);

			prodesc->fn_retisarray =
				(typeStruct->typlen == -1 && typeStruct->typelem);

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
			for (i = 0; i < prodesc->nargs; i++)
			{
				typeTup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(procStruct->proargtypes.values[i]),
										 0, 0, 0);
				if (!HeapTupleIsValid(typeTup))
				{
					free_plperl_function(prodesc);
					elog(ERROR, "cache lookup failed for type %u",
						 procStruct->proargtypes.values[i]);
				}
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* Disallow pseudotype argument */
				if (typeStruct->typtype == TYPTYPE_PSEUDO)
				{
					free_plperl_function(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/Perl functions cannot accept type %s",
						format_type_be(procStruct->proargtypes.values[i]))));
				}

				if (typeStruct->typtype == TYPTYPE_COMPOSITE)
					prodesc->arg_is_rowtype[i] = true;
				else
				{
					prodesc->arg_is_rowtype[i] = false;
					perm_fmgr_info(typeStruct->typoutput,
								   &(prodesc->arg_out_func[i]));
				}

				ReleaseSysCache(typeTup);
			}
		}

		/************************************************************
		 * create the text of the anonymous subroutine.
		 * we do not use a named subroutine so that we can call directly
		 * through the reference.
		 ************************************************************/
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
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
		{
			free_plperl_function(prodesc);
			elog(ERROR, "could not create PL/Perl internal procedure");
		}

		/************************************************************
		 * OK, link the procedure into the correct hashtable entry
		 ************************************************************/
		proc_key.user_id = prodesc->lanpltrusted ? GetUserId() : InvalidOid;

		proc_ptr = hash_search(plperl_proc_hash, &proc_key,
							   HASH_ENTER, NULL);
		proc_ptr->proc_ptr = prodesc;
		increment_prodesc_refcount(prodesc);
	}

	ReleaseSysCache(procTup);

	return prodesc;
}


/* Build a hash from all attributes of a given tuple. */

static SV  *
plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
{
	HV		   *hv;
	int			i;

	hv = newHV();

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		attr;
		bool		isnull;
		char	   *attname;
		char	   *outputstr;
		Oid			typoutput;
		bool		typisvarlena;

		if (tupdesc->attrs[i]->attisdropped)
			continue;

		attname = NameStr(tupdesc->attrs[i]->attname);
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			/* Store (attname => undef) and move on. */
			hv_store_string(hv, attname, newSV(0));
			continue;
		}

		/* XXX should have a way to cache these lookups */
		getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
						  &typoutput, &typisvarlena);

		outputstr = OidOutputFunctionCall(typoutput, attr);

		hv_store_string(hv, attname, newSVstring(outputstr));

		pfree(outputstr);
	}

	return newRV_noinc((SV *) hv);
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

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		int			spi_rv;

		spi_rv = SPI_execute(query, current_call_data->prodesc->fn_readonly,
							 limit);
		ret_hv = plperl_spi_execute_fetch_result(SPI_tuptable, SPI_processed,
												 spi_rv);

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

		/* Punt the error to Perl */
		croak("%s", edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return ret_hv;
}


static HV  *
plperl_spi_execute_fetch_result(SPITupleTable *tuptable, int processed,
								int status)
{
	HV		   *result;

	result = newHV();

	hv_store_string(result, "status",
					newSVstring(SPI_result_code_string(status)));
	hv_store_string(result, "processed",
					newSViv(processed));

	if (status > 0 && tuptable)
	{
		AV		   *rows;
		SV		   *row;
		int			i;

		rows = newAV();
		for (i = 0; i < processed; i++)
		{
			row = plperl_hash_from_tuple(tuptable->vals[i], tuptable->tupdesc);
			av_push(rows, row);
		}
		hv_store_string(result, "rows",
						newRV_noinc((SV *) rows));
	}

	SPI_freetuptable(tuptable);

	return result;
}


/*
 * Note: plperl_return_next is called both in Postgres and Perl contexts.
 * We report any errors in Postgres fashion (via ereport).  If called in
 * Perl context, it is SPI.xs's responsibility to catch the error and
 * convert to a Perl error.  We assume (perhaps without adequate justification)
 * that we need not abort the current transaction if the Perl code traps the
 * error.
 */
void
plperl_return_next(SV *sv)
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

	if (prodesc->fn_retistuple &&
		!(SvOK(sv) && SvROK(sv) && SvTYPE(SvRV(sv)) == SVt_PVHV))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("SETOF-composite-returning PL/Perl function "
						"must call return_next with reference to hash")));

	if (!current_call_data->ret_tdesc)
	{
		TupleDesc	tupdesc;

		Assert(!current_call_data->tuple_store);
		Assert(!current_call_data->attinmeta);

		/*
		 * This is the first call to return_next in the current PL/Perl
		 * function call, so memoize some lookups
		 */
		if (prodesc->fn_retistuple)
			(void) get_call_result_type(fcinfo, NULL, &tupdesc);
		else
			tupdesc = rsi->expectedDesc;

		/*
		 * Make sure the tuple_store and ret_tdesc are sufficiently
		 * long-lived.
		 */
		old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

		current_call_data->ret_tdesc = CreateTupleDescCopy(tupdesc);
		current_call_data->tuple_store =
			tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
								  false, work_mem);
		if (prodesc->fn_retistuple)
		{
			current_call_data->attinmeta =
				TupleDescGetAttInMetadata(current_call_data->ret_tdesc);
		}

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
			AllocSetContextCreate(rsi->econtext->ecxt_per_tuple_memory,
								  "PL/Perl return_next temporary cxt",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
	}

	old_cxt = MemoryContextSwitchTo(current_call_data->tmp_cxt);

	if (prodesc->fn_retistuple)
	{
		HeapTuple	tuple;

		tuple = plperl_build_tuple_result((HV *) SvRV(sv),
										  current_call_data->attinmeta);
		tuplestore_puttuple(current_call_data->tuple_store, tuple);
	}
	else
	{
		Datum		ret;
		bool		isNull;

		if (SvOK(sv))
		{
			char	   *val;

			if (prodesc->fn_retisarray && SvROK(sv) &&
				SvTYPE(SvRV(sv)) == SVt_PVAV)
			{
				sv = plperl_convert_to_pg_array(sv);
			}

			val = SvPV(sv, PL_na);

			ret = InputFunctionCall(&prodesc->result_in_func, val,
									prodesc->result_typioparam, -1);
			isNull = false;
		}
		else
		{
			ret = InputFunctionCall(&prodesc->result_in_func, NULL,
									prodesc->result_typioparam, -1);
			isNull = true;
		}

		tuplestore_putvalues(current_call_data->tuple_store,
							 current_call_data->ret_tdesc,
							 &ret, &isNull);
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

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		void	   *plan;
		Portal		portal;

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
		cursor = newSVstring(portal->name);

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

		/* Punt the error to Perl */
		croak("%s", edata->message);

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

	BeginInternalSubTransaction(NULL);
	/* Want to run inside function's memory context */
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
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
				SPI_cursor_close(p);
				row = &PL_sv_undef;
			}
			else
			{
				row = plperl_hash_from_tuple(SPI_tuptable->vals[0],
											 SPI_tuptable->tupdesc);
			}
			SPI_freetuptable(SPI_tuptable);
		}

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

		/* Punt the error to Perl */
		croak("%s", edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return row;
}

void
plperl_spi_cursor_close(char *cursor)
{
	Portal		p = SPI_cursor_find(cursor);

	if (p)
		SPI_cursor_close(p);
}

SV *
plperl_spi_prepare(char *query, int argc, SV **argv)
{
	void	   *volatile plan = NULL;
	volatile MemoryContext plan_cxt = NULL;
	plperl_query_desc *volatile qdesc = NULL;
	plperl_query_entry *volatile hash_entry = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	MemoryContext work_cxt;
	bool		found;
	int			i;

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
										 ALLOCSET_SMALL_MINSIZE,
										 ALLOCSET_SMALL_INITSIZE,
										 ALLOCSET_SMALL_MAXSIZE);
		MemoryContextSwitchTo(plan_cxt);
		qdesc = (plperl_query_desc *) palloc0(sizeof(plperl_query_desc));
		snprintf(qdesc->qname, sizeof(qdesc->qname), "%lx", (long) qdesc);
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
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);
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

			parseTypeString(SvPV(argv[i], PL_na), &typId, &typmod);

			getTypeInputInfo(typId, &typInput, &typIOParam);

			qdesc->argtypes[i] = typId;
			fmgr_info_cxt(typInput, &(qdesc->arginfuncs[i]), plan_cxt);
			qdesc->argtypioparams[i] = typIOParam;
		}

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
		qdesc->plan = SPI_saveplan(plan);
		if (qdesc->plan == NULL)
			elog(ERROR, "SPI_saveplan() failed: %s",
				 SPI_result_code_string(SPI_result));

		/* Release the procCxt copy to avoid within-function memory leak */
		SPI_freeplan(plan);

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

		/*
		 * AtEOSubXact_SPI() should not have popped any SPI context, but just
		 * in case it did, make sure we remain connected.
		 */
		SPI_restore_connection();
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

		/*
		 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it will
		 * have left us in a disconnected state.  We need this hack to return
		 * to connected state.
		 */
		SPI_restore_connection();

		/* Punt the error to Perl */
		croak("%s", edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	/************************************************************
	 * Return the query's hash key to the caller.
	 ************************************************************/
	return newSVstring(qdesc->qname);
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
			if (*sv && SvIOK(*sv))
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
			if (SvOK(argv[i]))
			{
				argvalues[i] = InputFunctionCall(&qdesc->arginfuncs[i],
												 SvPV(argv[i], PL_na),
												 qdesc->argtypioparams[i],
												 -1);
				nulls[i] = ' ';
			}
			else
			{
				argvalues[i] = InputFunctionCall(&qdesc->arginfuncs[i],
												 NULL,
												 qdesc->argtypioparams[i],
												 -1);
				nulls[i] = 'n';
			}
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

		/*
		 * AtEOSubXact_SPI() should not have popped any SPI context, but just
		 * in case it did, make sure we remain connected.
		 */
		SPI_restore_connection();
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

		/*
		 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it will
		 * have left us in a disconnected state.  We need this hack to return
		 * to connected state.
		 */
		SPI_restore_connection();

		/* Punt the error to Perl */
		croak("%s", edata->message);

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
			if (SvOK(argv[i]))
			{
				argvalues[i] = InputFunctionCall(&qdesc->arginfuncs[i],
												 SvPV(argv[i], PL_na),
												 qdesc->argtypioparams[i],
												 -1);
				nulls[i] = ' ';
			}
			else
			{
				argvalues[i] = InputFunctionCall(&qdesc->arginfuncs[i],
												 NULL,
												 qdesc->argtypioparams[i],
												 -1);
				nulls[i] = 'n';
			}
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

		cursor = newSVstring(portal->name);

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

		/* Punt the error to Perl */
		croak("%s", edata->message);

		/* Can't get here, but keep compiler quiet */
		return NULL;
	}
	PG_END_TRY();

	return cursor;
}

void
plperl_spi_freeplan(char *query)
{
	void	   *plan;
	plperl_query_desc *qdesc;
	plperl_query_entry *hash_entry;

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

/*
 * Create a new SV from a string assumed to be in the current database's
 * encoding.
 */
static SV  *
newSVstring(const char *str)
{
	SV		   *sv;

	sv = newSVpv(str, 0);
#if PERL_BCDVERSION >= 0x5006000L
	if (GetDatabaseEncoding() == PG_UTF8)
		SvUTF8_on(sv);
#endif
	return sv;
}

/*
 * Store an SV into a hash table under a key that is a string assumed to be
 * in the current database's encoding.
 */
static SV **
hv_store_string(HV *hv, const char *key, SV *val)
{
	int32		klen = strlen(key);

	/*
	 * This seems nowhere documented, but under Perl 5.8.0 and up, hv_store()
	 * recognizes a negative klen parameter as meaning a UTF-8 encoded key. It
	 * does not appear that hashes track UTF-8-ness of keys at all in Perl
	 * 5.6.
	 */
#if PERL_BCDVERSION >= 0x5008000L
	if (GetDatabaseEncoding() == PG_UTF8)
		klen = -klen;
#endif
	return hv_store(hv, key, klen, val, 0);
}

/*
 * Fetch an SV from a hash table under a key that is a string assumed to be
 * in the current database's encoding.
 */
static SV **
hv_fetch_string(HV *hv, const char *key)
{
	int32		klen = strlen(key);

	/* See notes in hv_store_string */
#if PERL_BCDVERSION >= 0x5008000L
	if (GetDatabaseEncoding() == PG_UTF8)
		klen = -klen;
#endif
	return hv_fetch(hv, key, klen, 0);
}


/*
 * Perl's own setlocal() copied from POSIX.xs
 * (needed because of the calls to new_*())
 */
#ifdef WIN32
static char *
setlocale_perl(int category, char *locale)
{
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
#endif   /* USE_LOCALE_CTYPE */
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
#endif   /* USE_LOCALE_COLLATE */


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
#endif   /* USE_LOCALE_NUMERIC */
	}

	return RETVAL;
}

#endif
