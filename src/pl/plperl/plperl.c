/**********************************************************************
 * plperl.c - perl as a procedural language for PostgreSQL
 *
 * IDENTIFICATION
 *
 *	  This software is copyrighted by Mark Hollomon
 *	 but is shameless cribbed from pltcl.c by Jan Weick.
 *
 *	  The author hereby grants permission  to  use,  copy,	modify,
 *	  distribute,  and	license this software and its documentation
 *	  for any purpose, provided that existing copyright notices are
 *	  retained	in	all  copies  and  that	this notice is included
 *	  verbatim in any distributions. No written agreement, license,
 *	  or  royalty  fee	is required for any of the authorized uses.
 *	  Modifications to this software may be  copyrighted  by  their
 *	  author  and  need  not  follow  the licensing terms described
 *	  here, provided that the new terms are  clearly  indicated  on
 *	  the first page of each file where they apply.
 *
 *	  IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *	  PARTY  FOR  DIRECT,	INDIRECT,	SPECIAL,   INCIDENTAL,	 OR
 *	  CONSEQUENTIAL   DAMAGES  ARISING	OUT  OF  THE  USE  OF  THIS
 *	  SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *	  IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *	  DAMAGE.
 *
 *	  THE  AUTHOR  AND	DISTRIBUTORS  SPECIFICALLY	 DISCLAIM	ANY
 *	  WARRANTIES,  INCLUDING,  BUT	NOT  LIMITED  TO,  THE	IMPLIED
 *	  WARRANTIES  OF  MERCHANTABILITY,	FITNESS  FOR  A  PARTICULAR
 *	  PURPOSE,	AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *	  AN "AS IS" BASIS, AND THE AUTHOR	AND  DISTRIBUTORS  HAVE  NO
 *	  OBLIGATION   TO	PROVIDE   MAINTENANCE,	 SUPPORT,  UPDATES,
 *	  ENHANCEMENTS, OR MODIFICATIONS.
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plperl/plperl.c,v 1.40.2.5 2010/05/17 20:46:03 adunstan Exp $
 *
 **********************************************************************/

#include "postgres.h"

/* system stuff */
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>

/* postgreSQL stuff */
#include "executor/spi.h"
#include "commands/trigger.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "access/heapam.h"
#include "tcop/tcopprot.h"
#include "utils/syscache.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"

/* perl stuff */
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "ppport.h"

/* just in case these symbols aren't provided */
#ifndef pTHX_
#define pTHX_
#define pTHX void
#endif

/* defines PLPERL_SET_OPMASK */
#include "plperl_opmask.h"

EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
EXTERN_C void boot_SPI(pTHX_ CV *cv);


/**********************************************************************
 * Information associated with a Perl interpreter.  We have one interpreter
 * that is used for all plperlu (untrusted) functions.  For plperl (trusted)
 * functions, there is a separate interpreter for each effective SQL userid.
 * (This is needed to ensure that an unprivileged user can't inject Perl code
 * that'll be executed with the privileges of some other SQL user.)
 *
 * The plperl_interp_desc structs are kept in a Postgres hash table indexed
 * by userid OID, with OID 0 used for the single untrusted interpreter.
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
} plperl_interp_desc;


/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct plperl_proc_desc
{
	char	   *proname;
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	plperl_interp_desc *interp;	/* interpreter it's created in */
	bool		lanpltrusted;
	FmgrInfo	result_in_func;
	Oid			result_in_elem;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	Oid			arg_out_elem[FUNC_MAX_ARGS];
	int			arg_is_rel[FUNC_MAX_ARGS];
	SV		   *reference;
} plperl_proc_desc;

/**********************************************************************
 * Global data
 **********************************************************************/

static bool plperl_firstcall = true;
static HTAB *plperl_interp_hash = NULL;
static HTAB *plperl_proc_hash = NULL;
static plperl_interp_desc *plperl_active_interp = NULL;
/* If we have an unassigned "held" interpreter, it's stored here */
static PerlInterpreter *plperl_held_interp = NULL;

static OP  *(*pp_require_orig) (pTHX) = NULL;
static char plperl_opmask[MAXO];

/**********************************************************************
 * Forward declarations
 **********************************************************************/
Datum		plperl_call_handler(PG_FUNCTION_ARGS);
void		plperl_init(void);

static PerlInterpreter *plperl_init_interp(void);
static void set_interp_require(bool trusted);

static Datum plperl_func_handler(PG_FUNCTION_ARGS);

static plperl_proc_desc *compile_plperl_function(Oid fn_oid, bool is_trigger);

static SV  *plperl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc);
static void plperl_init_shared_libs(pTHX);
static void plperl_trusted_init(void);
static void plperl_untrusted_init(void);
static void plperl_create_sub(plperl_proc_desc *desc, char *s, Oid fn_oid);
static char *strip_trailing_ws(const char *msg);
static OP  *pp_require_safe(pTHX);
static void activate_interpreter(plperl_interp_desc *interp_desc);


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

/**********************************************************************
 * plperl_init()			- Initialize everything that can be
 *							  safely initialized during postmaster
 *							  startup.
 *
 * DO NOT make this static --- it has to be callable by preload
 **********************************************************************/
void
plperl_init(void)
{
	HASHCTL		hash_ctl;

	/************************************************************
	 * Do initialization only once
	 ************************************************************/
	if (!plperl_firstcall)
		return;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(plperl_interp_desc);
	hash_ctl.hash = tag_hash;
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

	/************************************************************
	 * Create the Perl interpreter
	 ************************************************************/
	PLPERL_SET_OPMASK(plperl_opmask);

	plperl_held_interp = plperl_init_interp();

	plperl_firstcall = false;
}

/**********************************************************************
 * plperl_init_all()		- Initialize all
 **********************************************************************/
static void
plperl_init_all(void)
{

	/************************************************************
	 * Execute postmaster-startup safe initialization
	 ************************************************************/
	if (plperl_firstcall)
		plperl_init();

	/************************************************************
	 * Any other initialization that must be done each time a new
	 * backend starts -- currently none
	 ************************************************************/

}

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
		"", "-e",

		/*
		 * no commas between the next lines please. They are supposed to be
		 * one string
		 */
		"SPI::bootstrap();"
		"sub ::mkfunc {return eval(qq[ sub { $_[0] } ]); }"
	};

	/****
	 * The perl API docs state that PERL_SYS_INIT3 should be called before
	 * allocating interprters. Unfortunately, on some platforms this fails
	 * in the Perl_do_taint() routine, which is called when the platform is
	 * using the system's malloc() instead of perl's own. Other platforms,
	 * fail if PERL_SYS_INIT3 is not called. So we call it
	 * if it's available, unless perl is using the system malloc(), which is
	 * true when MYMALLOC is set.
	 */
#if defined(PERL_SYS_INIT3) && !defined(MYMALLOC)
	if (!perl_sys_init_done)
	{
		int			nargs;
		char	   *dummy_perl_env[1];

		/* initialize this way to silence silly compiler warnings */
		nargs = 3;
		dummy_perl_env[0] = NULL;
		PERL_SYS_INIT3(&nargs, (char ***) &embedding, (char ***) &dummy_perl_env);
		perl_sys_init_done = 1;
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
	{
		pp_require_orig = PL_ppaddr[OP_REQUIRE];
	}
	else
	{
		PL_ppaddr[OP_REQUIRE] = pp_require_orig;
		PL_ppaddr[OP_DOFILE] = pp_require_orig;
	}

	if (perl_parse(plperl, plperl_init_shared_libs,
				   3, embedding, NULL) != 0)
		ereport(ERROR,
				(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
				 errcontext("while parsing Perl initialization")));

	if (perl_run(plperl) != 0)
		ereport(ERROR,
				(errmsg("%s", strip_trailing_ws(SvPV_nolen(ERRSV))),
				 errcontext("while running Perl initialization")));

	return plperl;
}


/**********************************************************************
 * plperl_call_handler		- This is the only visible function
 *				  of the PL interpreter. The PostgreSQL
 *				  function manager and trigger manager
 *				  call this function for execution of
 *				  perl procedures.
 **********************************************************************/
PG_FUNCTION_INFO_V1(plperl_call_handler);

/* keep non-static */
Datum
plperl_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	plperl_interp_desc *oldinterp;
	sigjmp_buf	save_restart;

	/************************************************************
	 * Initialize interpreter
	 ************************************************************/
	plperl_init_all();

	/************************************************************
	 * Connect to SPI manager
	 ************************************************************/
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/************************************************************
	 * Determine if called as function or trigger and
	 * call appropriate subhandler
	 ************************************************************/
	oldinterp = plperl_active_interp;

	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		activate_interpreter(oldinterp);
		siglongjmp(Warn_restart, 1);
	}

	if (CALLED_AS_TRIGGER(fcinfo))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use perl in triggers yet")));

		/*
		 * retval = PointerGetDatum(plperl_trigger_handler(fcinfo));
		 */
		/* make the compiler happy */
		retval = (Datum) 0;
	}
	else
	{
		/* non-trigger functions are ok */
		retval = plperl_func_handler(fcinfo);
	}

	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	activate_interpreter(oldinterp);
	return retval;
}


/**********************************************************************
 * plperl_create_sub()		- calls the perl interpreter to
 *		create the anonymous subroutine whose text is in the SV.
 *		Returns the SV containing the RV to the closure.
 **********************************************************************/
static void
plperl_create_sub(plperl_proc_desc *prodesc, char *s, Oid fn_oid)
{
	dSP;
	SV		   *subref;
	int			count;

	ENTER;
	SAVETMPS;
	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newSVpv(s, 0)));
	PUTBACK;

	/*
	 * G_KEEPERR seems to be needed here, else we don't recognize compile
	 * errors properly.  Perhaps it's because there's another level of eval
	 * inside mkfunc?
	 */
	count = perl_call_pv("::mkfunc",
						 G_SCALAR | G_EVAL | G_KEEPERR);
	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "didn't get a return item from mkfunc");
	}

	if (SvTRUE(ERRSV))
	{
		POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "creation of function failed: %s", SvPV(ERRSV, PL_na));
	}

	/*
	 * need to make a deep copy of the return. it comes off the stack as a
	 * temporary.
	 */
	subref = newSVsv(POPs);

	if (!SvROK(subref))
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
		GvCV(sv) = NULL;		/* prevent call via GV */
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

/**********************************************************************
 * plperl_call_perl_func()		- calls a perl function through the RV
 *			stored in the prodesc structure. massages the input parms properly
 **********************************************************************/
static SV  *
plperl_call_perl_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo)
{
	dSP;
	SV		   *retval;
	int			i;
	int			count;

	ENTER;
	SAVETMPS;

	PUSHMARK(SP);
	for (i = 0; i < desc->nargs; i++)
	{
		if (desc->arg_is_rel[i])
		{
			TupleTableSlot *slot = (TupleTableSlot *) fcinfo->arg[i];
			SV		   *hashref;

			Assert(slot != NULL && !fcinfo->argnull[i]);

			/*
			 * plperl_build_tuple_argument better return a mortal SV.
			 */
			hashref = plperl_build_tuple_argument(slot->val,
												  slot->ttc_tupleDescriptor);
			XPUSHs(hashref);
		}
		else
		{
			if (fcinfo->argnull[i])
				XPUSHs(&PL_sv_undef);
			else
			{
				char	   *tmp;

				tmp = DatumGetCString(FunctionCall3(&(desc->arg_out_func[i]),
													fcinfo->arg[i],
									 ObjectIdGetDatum(desc->arg_out_elem[i]),
													Int32GetDatum(-1)));
				XPUSHs(sv_2mortal(newSVpv(tmp, 0)));
				pfree(tmp);
			}
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
		POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "error from function: %s", SvPV(ERRSV, PL_na));
	}

	retval = newSVsv(POPs);

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retval;
}


/**********************************************************************
 * plperl_func_handler()		- Handler for regular function calls
 **********************************************************************/
static Datum
plperl_func_handler(PG_FUNCTION_ARGS)
{
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval;

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, false);

	activate_interpreter(prodesc->interp);

	/************************************************************
	 * Call the Perl function
	 ************************************************************/
	perlret = plperl_call_perl_func(prodesc, fcinfo);

	/************************************************************
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	if (!(perlret && SvOK(perlret)))
	{
		/* return NULL if Perl code returned undef */
		retval = (Datum) 0;
		fcinfo->isnull = true;
	}
	else
	{
		retval = FunctionCall3(&prodesc->result_in_func,
							   PointerGetDatum(SvPV(perlret, PL_na)),
							   ObjectIdGetDatum(prodesc->result_in_elem),
							   Int32GetDatum(-1));
	}

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
				prodesc->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data));

		if (uptodate)
			return true;

		/* Otherwise, unlink the obsoleted entry from the hashtable ... */
		proc_ptr->proc_ptr = NULL;
		/* ... and throw it away */
		if (prodesc->reference)
		{
			plperl_interp_desc *oldinterp = plperl_active_interp;

			activate_interpreter(prodesc->interp);
			SvREFCNT_dec(prodesc->reference);
			activate_interpreter(oldinterp);
		}
		free(prodesc->proname);
		free(prodesc);
	}

	return false;
}


/**********************************************************************
 * compile_plperl_function	- compile (or hopefully just look up) function
 **********************************************************************/
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
	 * the functions arguments and returntype and store
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
		char	   *proc_source;

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (plperl_proc_desc *) malloc(sizeof(plperl_proc_desc));
		if (prodesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		MemSet(prodesc, 0, sizeof(plperl_proc_desc));
		prodesc->proname = strdup(NameStr(procStruct->proname));
		if (prodesc->proname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		prodesc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		prodesc->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);

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
							 errmsg("plperl functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}

			if (typeStruct->typrelid != InvalidOid)
			{
				free(prodesc->proname);
				free(prodesc);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					   errmsg("plperl functions cannot return tuples yet")));
			}

			if (procStruct->proretset)
			{
				free(prodesc->proname);
				free(prodesc);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("plperl functions cannot return sets yet")));
			}

			perm_fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
			prodesc->result_in_elem = typeStruct->typelem;


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
								ObjectIdGetDatum(procStruct->proargtypes[i]),
										 0, 0, 0);
				if (!HeapTupleIsValid(typeTup))
				{
					free(prodesc->proname);
					free(prodesc);
					elog(ERROR, "cache lookup failed for type %u",
						 procStruct->proargtypes[i]);
				}
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* Disallow pseudotype argument */
				if (typeStruct->typtype == 'p')
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("plperl functions cannot take type %s",
							   format_type_be(procStruct->proargtypes[i]))));
				}

				if (typeStruct->typrelid != InvalidOid)
					prodesc->arg_is_rel[i] = 1;
				else
					prodesc->arg_is_rel[i] = 0;

				perm_fmgr_info(typeStruct->typoutput, &(prodesc->arg_out_func[i]));
				prodesc->arg_out_elem[i] = typeStruct->typelem;
				ReleaseSysCache(typeTup);
			}
		}

		/************************************************************
		 * create the text of the anonymous subroutine.
		 * we do not use a named subroutine so that we can call directly
		 * through the reference.
		 *
		 ************************************************************/
		proc_source = DatumGetCString(DirectFunctionCall1(textout,
									  PointerGetDatum(&procStruct->prosrc)));

		/************************************************************
		 * Create the procedure in the appropriate interpreter
		 ************************************************************/

		select_perl_context(prodesc->lanpltrusted);

		prodesc->interp = plperl_active_interp;

		plperl_create_sub(prodesc, proc_source, fn_oid);

		activate_interpreter(oldinterp);

		pfree(proc_source);
		if (!prodesc->reference)
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "could not create PL/Perl internal procedure");
		}

		/************************************************************
		 * OK, link the procedure into the correct hashtable entry
		 ************************************************************/
		proc_key.user_id = prodesc->lanpltrusted ? GetUserId() : InvalidOid;

		proc_ptr = hash_search(plperl_proc_hash, &proc_key,
							   HASH_ENTER, NULL);
		proc_ptr->proc_ptr = prodesc;
	}

	ReleaseSysCache(procTup);

	return prodesc;
}


/**********************************************************************
 * plperl_build_tuple_argument() - Build a ref to a hash
 *				  from all attributes of a given tuple
 **********************************************************************/
static SV  *
plperl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc)
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
		Oid			typioparam;
		bool		typisvarlena;
		int			namelen;

		if (tupdesc->attrs[i]->attisdropped)
			continue;

		attname = NameStr(tupdesc->attrs[i]->attname);
		namelen = strlen(attname);
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			/* Store (attname => undef) and move on. */
			hv_store(hv, attname, namelen, newSV(0), 0);
			continue;
		}

		/* XXX should have a way to cache these lookups */

		getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
						  &typoutput, &typioparam, &typisvarlena);

		outputstr = DatumGetCString(OidFunctionCall3(typoutput,
													 attr,
												ObjectIdGetDatum(typioparam),
							   Int32GetDatum(tupdesc->attrs[i]->atttypmod)));

		hv_store(hv, attname, namelen, newSVpv(outputstr, 0), 0);
	}

	return newRV_noinc((SV *) hv);
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
