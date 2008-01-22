/**********************************************************************
 * plperl.c - perl as a procedural language for PostgreSQL
 *
 *	  $PostgreSQL: pgsql/src/pl/plperl/plperl.c,v 1.123.2.4 2008/01/22 20:19:53 adunstan Exp $
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
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/hsearch.h"

/* perl stuff */
#include "plperl.h"

PG_MODULE_MAGIC;

/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct plperl_proc_desc
{
	char	   *proname;
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	bool		fn_readonly;
	bool		lanpltrusted;
	bool		fn_retistuple;	/* true, if function returns tuple */
	bool		fn_retisset;	/* true, if function returns set */
	bool		fn_retisarray;	/* true if function returns array */
	Oid			result_oid;		/* Oid of result type */
	FmgrInfo	result_in_func; /* I/O function and arg for result type */
	Oid			result_typioparam;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	bool		arg_is_rowtype[FUNC_MAX_ARGS];
	SV		   *reference;
} plperl_proc_desc;

/* hash table entry for proc desc  */

typedef struct plperl_proc_entry
{
	char proc_name[NAMEDATALEN];
	plperl_proc_desc *proc_data;
} plperl_proc_entry;

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
	char		qname[sizeof(long) * 2 + 1];
	void	   *plan;
	int			nargs;
	Oid		   *argtypes;
	FmgrInfo   *arginfuncs;
	Oid		   *argtypioparams;
} plperl_query_desc;

/* hash table entry for query desc  */

typedef struct plperl_query_entry
{
	char query_name[NAMEDATALEN];
	plperl_query_desc *query_data;
} plperl_query_entry;

/**********************************************************************
 * Global data
 **********************************************************************/

typedef enum
{
	INTERP_NONE,
	INTERP_HELD,
	INTERP_TRUSTED,
	INTERP_UNTRUSTED,
	INTERP_BOTH
} InterpState;

static InterpState interp_state = INTERP_NONE;
static bool can_run_two = false;

static bool plperl_safe_init_done = false;
static PerlInterpreter *plperl_trusted_interp = NULL;
static PerlInterpreter *plperl_untrusted_interp = NULL;
static PerlInterpreter *plperl_held_interp = NULL;
static bool trusted_context;
static HTAB  *plperl_proc_hash = NULL;
static HTAB  *plperl_query_hash = NULL;

static bool plperl_use_strict = false;

/* this is saved and restored by plperl_call_handler */
static plperl_call_data *current_call_data = NULL;

/**********************************************************************
 * Forward declarations
 **********************************************************************/
Datum		plperl_call_handler(PG_FUNCTION_ARGS);
Datum		plperl_validator(PG_FUNCTION_ARGS);
void		_PG_init(void);

static void plperl_init_interp(void);

static Datum plperl_func_handler(PG_FUNCTION_ARGS);

static Datum plperl_trigger_handler(PG_FUNCTION_ARGS);
static plperl_proc_desc *compile_plperl_function(Oid fn_oid, bool is_trigger);

static SV  *plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc);
static void plperl_init_shared_libs(pTHX);
static HV  *plperl_spi_execute_fetch_result(SPITupleTable *, int, int);
static SV  *newSVstring(const char *str);
static SV **hv_store_string(HV *hv, const char *key, SV *val);
static SV **hv_fetch_string(HV *hv, const char *key);
static SV  *plperl_create_sub(char *s, bool trusted);
static SV  *plperl_call_perl_func(plperl_proc_desc *desc, FunctionCallInfo fcinfo);

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
    HASHCTL     hash_ctl;

	if (inited)
		return;

	DefineCustomBoolVariable("plperl.use_strict",
	  "If true, will compile trusted and untrusted perl code in strict mode",
							 NULL,
							 &plperl_use_strict,
							 PGC_USERSET,
							 NULL, NULL);

	EmitWarningsOnPlaceholders("plperl");

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(plperl_proc_entry);

	plperl_proc_hash = hash_create("PLPerl Procedures",
								   32,
								   &hash_ctl,
								   HASH_ELEM);

	hash_ctl.entrysize = sizeof(plperl_query_entry);
	plperl_query_hash = hash_create("PLPerl Queries",
									32,
									&hash_ctl,
									HASH_ELEM);

	plperl_init_interp();

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
	"sub ::mkunsafefunc {" \
	"      my $ret = eval(qq[ sub { $_[0] $_[1] } ]); " \
	"      $@ =~ s/\\(eval \\d+\\) //g if $@; return $ret; }" \
	"use strict; " \
	"sub ::mk_strict_unsafefunc {" \
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

#define SAFE_MODULE \
	"require Safe; $Safe::VERSION"

#define SAFE_OK \
	"use vars qw($PLContainer); $PLContainer = new Safe('PLPerl');" \
	"$PLContainer->permit_only(':default');" \
	"$PLContainer->permit(qw[:base_math !:base_io sort time]);" \
	"$PLContainer->share(qw[&elog &spi_exec_query &return_next " \
	"&spi_query &spi_fetchrow &spi_cursor_close " \
	"&spi_prepare &spi_exec_prepared &spi_query_prepared &spi_freeplan " \
	"&_plperl_to_pg_array " \
	"&DEBUG &LOG &INFO &NOTICE &WARNING &ERROR %_SHARED ]);" \
	"sub ::mksafefunc {" \
	"      my $ret = $PLContainer->reval(qq[sub { $_[0] $_[1] }]); " \
	"      $@ =~ s/\\(eval \\d+\\) //g if $@; return $ret; }" \
	"$PLContainer->permit(qw[require caller]); $PLContainer->reval('use strict;');" \
	"$PLContainer->deny(qw[require caller]); " \
	"sub ::mk_strict_safefunc {" \
	"      my $ret = $PLContainer->reval(qq[sub { BEGIN { strict->import(); } $_[0] $_[1] }]); " \
	"      $@ =~ s/\\(eval \\d+\\) //g if $@; return $ret; }"

#define SAFE_BAD \
	"use vars qw($PLContainer); $PLContainer = new Safe('PLPerl');" \
	"$PLContainer->permit_only(':default');" \
	"$PLContainer->share(qw[&elog &ERROR ]);" \
	"sub ::mksafefunc { return $PLContainer->reval(qq[sub { " \
	"      elog(ERROR,'trusted Perl functions disabled - " \
	"      please upgrade Perl Safe module to version 2.09 or later');}]); }" \
	"sub ::mk_strict_safefunc { return $PLContainer->reval(qq[sub { " \
	"      elog(ERROR,'trusted Perl functions disabled - " \
	"      please upgrade Perl Safe module to version 2.09 or later');}]); }"

#define TEST_FOR_MULTI \
	"use Config; " \
	"$Config{usemultiplicity} eq 'define' or "  \
    "($Config{usethreads} eq 'define' " \
	" and $Config{useithreads} eq 'define')"


/********************************************************************
 *
 * We start out by creating a "held" interpreter that we can use in
 * trusted or untrusted mode (but not both) as the need arises. Later, we
 * assign that interpreter if it is available to either the trusted or 
 * untrusted interpreter. If it has already been assigned, and we need to
 * create the other interpreter, we do that if we can, or error out.
 * We detect if it is safe to run two interpreters during the setup of the
 * dummy interpreter.
 */


static void 
check_interp(bool trusted)
{
	if (interp_state == INTERP_HELD)
	{
		if (trusted)
		{
			plperl_trusted_interp = plperl_held_interp;
			interp_state = INTERP_TRUSTED;
		}
		else
		{
			plperl_untrusted_interp = plperl_held_interp;
			interp_state = INTERP_UNTRUSTED;
		}
		plperl_held_interp = NULL;
		trusted_context = trusted;
	}
	else if (interp_state == INTERP_BOTH || 
			 (trusted && interp_state == INTERP_TRUSTED) ||
			 (!trusted && interp_state == INTERP_UNTRUSTED))
	{
		if (trusted_context != trusted)
		{
			if (trusted)
				PERL_SET_CONTEXT(plperl_trusted_interp);
			else
				PERL_SET_CONTEXT(plperl_untrusted_interp);
			trusted_context = trusted;
		}
	}
	else if (can_run_two)
	{
		PERL_SET_CONTEXT(plperl_held_interp);
		plperl_init_interp();
		if (trusted)
			plperl_trusted_interp = plperl_held_interp;
		else
			plperl_untrusted_interp = plperl_held_interp;
		interp_state = INTERP_BOTH;
		plperl_held_interp = NULL;
		trusted_context = trusted;
	}
	else
	{
		elog(ERROR, 
			 "can not allocate second Perl interpreter on this platform");

	}
	
}


static void
restore_context (bool old_context)
{
	if (trusted_context != old_context)
	{
		if (old_context)
			PERL_SET_CONTEXT(plperl_trusted_interp);
		else
			PERL_SET_CONTEXT(plperl_untrusted_interp);
		trusted_context = old_context;
	}
}

static void
plperl_init_interp(void)
{
	static char *embedding[3] = {
		"", "-e", PERLBOOT
	};

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
	 * We restore them using Perl's POSIX::setlocale() function so that Perl
	 * doesn't have a different idea of the locale from Postgres.
	 *
	 */

	char	   *loc;
	char	   *save_collate,
			   *save_ctype,
			   *save_monetary,
			   *save_numeric,
			   *save_time;
	char		buf[1024];

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
#endif


	plperl_held_interp = perl_alloc();
	if (!plperl_held_interp)
		elog(ERROR, "could not allocate Perl interpreter");

	perl_construct(plperl_held_interp);
	perl_parse(plperl_held_interp, plperl_init_shared_libs, 
			   3, embedding, NULL);
	perl_run(plperl_held_interp);

	if (interp_state == INTERP_NONE)
	{
		SV *res;

		res = eval_pv(TEST_FOR_MULTI,TRUE);
		can_run_two = SvIV(res); 
		interp_state = INTERP_HELD;
	}

#ifdef WIN32

	eval_pv("use POSIX qw(locale_h);", TRUE);	/* croak on failure */

	if (save_collate != NULL)
	{
		snprintf(buf, sizeof(buf), "setlocale(%s,'%s');",
				 "LC_COLLATE", save_collate);
		eval_pv(buf, TRUE);
		pfree(save_collate);
	}
	if (save_ctype != NULL)
	{
		snprintf(buf, sizeof(buf), "setlocale(%s,'%s');",
				 "LC_CTYPE", save_ctype);
		eval_pv(buf, TRUE);
		pfree(save_ctype);
	}
	if (save_monetary != NULL)
	{
		snprintf(buf, sizeof(buf), "setlocale(%s,'%s');",
				 "LC_MONETARY", save_monetary);
		eval_pv(buf, TRUE);
		pfree(save_monetary);
	}
	if (save_numeric != NULL)
	{
		snprintf(buf, sizeof(buf), "setlocale(%s,'%s');",
				 "LC_NUMERIC", save_numeric);
		eval_pv(buf, TRUE);
		pfree(save_numeric);
	}
	if (save_time != NULL)
	{
		snprintf(buf, sizeof(buf), "setlocale(%s,'%s');",
				 "LC_TIME", save_time);
		eval_pv(buf, TRUE);
		pfree(save_time);
	}
#endif

}


static void
plperl_safe_init(void)
{
	SV		   *res;
	double		safe_version;

	res = eval_pv(SAFE_MODULE, FALSE);	/* TRUE = croak if failure */

	safe_version = SvNV(res);

	/*
	 * We actually want to reject safe_version < 2.09, but it's risky to
	 * assume that floating-point comparisons are exact, so use a slightly
	 * smaller comparison value.
	 */
	if (safe_version < 2.0899)
	{
		/* not safe, so disallow all trusted funcs */
		eval_pv(SAFE_BAD, FALSE);
	}
	else
	{
		eval_pv(SAFE_OK, FALSE);
		if (GetDatabaseEncoding() == PG_UTF8)
		{
			/* 
			 * Fill in just enough information to set up this perl
			 * function in the safe container and call it.
			 * For some reason not entirely clear, it prevents errors that
			 * can arise from the regex code later trying to load
			 * utf8 modules.
			 */
			plperl_proc_desc desc;			
			FunctionCallInfoData fcinfo;
			SV *ret;
			SV *func;

			/* make sure we don't call ourselves recursively */
			plperl_safe_init_done = true;

			/* compile the function */
			func = plperl_create_sub(
				"return shift =~ /\\xa9/i ? 'true' : 'false' ;",
				true);

			/* set up to call the function with a single text argument 'a' */
			desc.reference = func;
			desc.nargs = 1;
			desc.arg_is_rowtype[0] = false;
			fmgr_info(F_TEXTOUT, &(desc.arg_out_func[0]));

			fcinfo.arg[0] = DirectFunctionCall1(textin, CStringGetDatum("a"));
			fcinfo.argnull[0] = false;
			
			/* and make the call */
			ret = plperl_call_perl_func(&desc, &fcinfo);
		}
	}

	plperl_safe_init_done = true;
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
	if (!SvOK(*svp) || SvTYPE(*svp) != SVt_RV || SvTYPE(SvRV(*svp)) != SVt_PVHV)
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
	plperl_call_data *save_call_data;

	save_call_data = current_call_data;
	PG_TRY();
	{
		if (CALLED_AS_TRIGGER(fcinfo))
			retval = PointerGetDatum(plperl_trigger_handler(fcinfo));
		else
			retval = plperl_func_handler(fcinfo);
	}
	PG_CATCH();
	{
		current_call_data = save_call_data;
		PG_RE_THROW();
	}
	PG_END_TRY();

	current_call_data = save_call_data;
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
	if (functyptype == 'p')
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			istrigger = true;
		else if (proc->prorettype != RECORDOID &&
				 proc->prorettype != VOIDOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("plperl functions cannot return type %s",
							format_type_be(proc->prorettype))));
	}

	/* Disallow pseudotypes in arguments (either IN or OUT) */
	numargs = get_func_arg_info(tuple,
								&argtypes, &argnames, &argmodes);
	for (i = 0; i < numargs; i++)
	{
		if (get_typtype(argtypes[i]) == 'p')
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("plperl functions cannot take type %s",
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
 * Uses mksafefunc/mkunsafefunc to create an anonymous sub whose text is
 * supplied in s, and returns a reference to the closure.
 */
static SV  *
plperl_create_sub(char *s, bool trusted)
{
	dSP;
	SV		   *subref;
	int			count;
	char	   *compile_sub;

	if (trusted && !plperl_safe_init_done)
	{
		plperl_safe_init();
		SPAGAIN;
	}

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

	if (trusted && plperl_use_strict)
		compile_sub = "::mk_strict_safefunc";
	else if (plperl_use_strict)
		compile_sub = "::mk_strict_unsafefunc";
	else if (trusted)
		compile_sub = "::mksafefunc";
	else
		compile_sub = "::mkunsafefunc";

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
				 errmsg("creation of Perl function failed: %s",
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

	return subref;
}


/**********************************************************************
 * plperl_init_shared_libs()		-
 *
 * We cannot use the DynaLoader directly to get at the Opcode
 * module (used by Safe.pm). So, we link Opcode into ourselves
 * and do the initialization behind perl's back.
 *
 **********************************************************************/

EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
EXTERN_C void boot_SPI(pTHX_ CV *cv);

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
				(errmsg("error from Perl function: %s",
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
				(errmsg("error from Perl trigger function: %s",
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
	bool       oldcontext = trusted_context;

	/*
	 * Create the call_data beforing connecting to SPI, so that it is not
	 * allocated in the SPI memory context
	 */
	current_call_data = (plperl_call_data *) palloc0(sizeof(plperl_call_data));
	current_call_data->fcinfo = fcinfo;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, false);
	current_call_data->prodesc = prodesc;

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

	check_interp(prodesc->lanpltrusted);

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
			SvTYPE(perlret) == SVt_RV &&
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
					 errmsg("set-returning Perl function must return "
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

		if (!SvOK(perlret) || SvTYPE(perlret) != SVt_RV ||
			SvTYPE(SvRV(perlret)) != SVt_PVHV)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("composite-returning Perl function "
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

	current_call_data = NULL;
	restore_context(oldcontext);

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
	bool       oldcontext = trusted_context;

	/*
	 * Create the call_data beforing connecting to SPI, so that it is not
	 * allocated in the SPI memory context
	 */
	current_call_data = (plperl_call_data *) palloc0(sizeof(plperl_call_data));
	current_call_data->fcinfo = fcinfo;

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, true);
	current_call_data->prodesc = prodesc;

	check_interp(prodesc->lanpltrusted);

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
					   errmsg("ignoring modified tuple in DELETE trigger")));
				trv = NULL;
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("result of Perl trigger function must be undef, "
							"\"SKIP\" or \"MODIFY\"")));
			trv = NULL;
		}
		retval = PointerGetDatum(trv);
	}

	SvREFCNT_dec(svTD);
	if (perlret)
		SvREFCNT_dec(perlret);

	current_call_data = NULL;
	restore_context(oldcontext);
	return retval;
}


static plperl_proc_desc *
compile_plperl_function(Oid fn_oid, bool is_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[64];
	plperl_proc_desc *prodesc = NULL;
	int			i;
	plperl_proc_entry *hash_entry;
	bool found;
	bool oldcontext = trusted_context;

	/* We'll need the pg_proc tuple in any case... */
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(fn_oid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/************************************************************
	 * Build our internal proc name from the function's Oid
	 ************************************************************/
	if (!is_trigger)
		sprintf(internal_proname, "__PLPerl_proc_%u", fn_oid);
	else
		sprintf(internal_proname, "__PLPerl_proc_%u_trigger", fn_oid);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	hash_entry = hash_search(plperl_proc_hash, internal_proname, 
							 HASH_FIND, NULL);

	if (hash_entry)
	{
		bool		uptodate;

		prodesc = hash_entry->proc_data;

		/************************************************************
		 * If it's present, must check whether it's still up to date.
		 * This is needed because CREATE OR REPLACE FUNCTION can modify the
		 * function's pg_proc entry without changing its OID.
		 ************************************************************/
		uptodate = (prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
				prodesc->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data));

		if (!uptodate)
		{
			free(prodesc); /* are we leaking memory here? */
			prodesc = NULL;
			hash_search(plperl_proc_hash, internal_proname,
						HASH_REMOVE,NULL);
		}
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
		MemSet(prodesc, 0, sizeof(plperl_proc_desc));
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

			/* Disallow pseudotype result, except VOID or RECORD */
			if (typeStruct->typtype == 'p')
			{
				if (procStruct->prorettype == VOIDOID ||
					procStruct->prorettype == RECORDOID)
					 /* okay */ ;
				else if (procStruct->prorettype == TRIGGEROID)
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions may only be called "
									"as triggers")));
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

			prodesc->result_oid = procStruct->prorettype;
			prodesc->fn_retisset = procStruct->proretset;
			prodesc->fn_retistuple = (typeStruct->typtype == 'c' ||
									  procStruct->prorettype == RECORDOID);

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
							 errmsg("plperl functions cannot take type %s",
						format_type_be(procStruct->proargtypes.values[i]))));
				}

				if (typeStruct->typtype == 'c')
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
		proc_source = DatumGetCString(DirectFunctionCall1(textout,
														  prosrcdatum));

		/************************************************************
		 * Create the procedure in the interpreter
		 ************************************************************/

		check_interp(prodesc->lanpltrusted);

		prodesc->reference = plperl_create_sub(proc_source, prodesc->lanpltrusted);

		restore_context(oldcontext);

		pfree(proc_source);
		if (!prodesc->reference)	/* can this happen? */
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "could not create internal procedure \"%s\"",
				 internal_proname);
		}

		hash_entry = hash_search(plperl_proc_hash, internal_proname,
								 HASH_ENTER, &found);
		hash_entry->proc_data = prodesc;
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
 * We report any errors in Postgres fashion (via ereport).	If called in
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
	HeapTuple	tuple;

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
		!(SvOK(sv) && SvTYPE(sv) == SVt_RV && SvTYPE(SvRV(sv)) == SVt_PVHV))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("setof-composite-returning Perl function "
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
			tuplestore_begin_heap(true, false, work_mem);
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
		tuple = plperl_build_tuple_result((HV *) SvRV(sv),
										  current_call_data->attinmeta);
	}
	else
	{
		Datum		ret;
		bool		isNull;

		if (SvOK(sv))
		{
			char	   *val = SvPV(sv, PL_na);

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

		tuple = heap_form_tuple(current_call_data->ret_tdesc, &ret, &isNull);
	}

	/* Make sure to store the tuple in a long-lived memory context */
	MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	tuplestore_puttuple(current_call_data->tuple_store, tuple);
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
	plperl_query_desc *qdesc;
	plperl_query_entry *hash_entry;
	bool        found;
	void	   *plan;
	int			i;

	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	/************************************************************
	 * Allocate the new querydesc structure
	 ************************************************************/
	qdesc = (plperl_query_desc *) malloc(sizeof(plperl_query_desc));
	MemSet(qdesc, 0, sizeof(plperl_query_desc));
	snprintf(qdesc->qname, sizeof(qdesc->qname), "%lx", (long) qdesc);
	qdesc->nargs = argc;
	qdesc->argtypes = (Oid *) malloc(argc * sizeof(Oid));
	qdesc->arginfuncs = (FmgrInfo *) malloc(argc * sizeof(FmgrInfo));
	qdesc->argtypioparams = (Oid *) malloc(argc * sizeof(Oid));

	PG_TRY();
	{
		/************************************************************
		 * Lookup the argument types by name in the system cache
		 * and remember the required information for input conversion
		 ************************************************************/
		for (i = 0; i < argc; i++)
		{
			List	   *names;
			HeapTuple	typeTup;

			/* Parse possibly-qualified type name and look it up in pg_type */
			names = stringToQualifiedNameList(SvPV(argv[i], PL_na),
											  "plperl_spi_prepare");
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

		free(qdesc->argtypes);
		free(qdesc->arginfuncs);
		free(qdesc->argtypioparams);
		free(qdesc);

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

	/************************************************************
	 * Insert a hashtable entry for the plan and return
	 * the key to the caller.
	 ************************************************************/

	hash_entry = hash_search(plperl_query_hash, qdesc->qname,
							 HASH_ENTER,&found);
	hash_entry->query_data = qdesc;

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

		hash_entry = hash_search(plperl_query_hash, query,
										 HASH_FIND,NULL);
		if (hash_entry == NULL)
			elog(ERROR, "spi_exec_prepared: Invalid prepared query passed");

		qdesc = hash_entry->query_data;

		if (qdesc == NULL)
			elog(ERROR, "spi_exec_prepared: panic - plperl_query_hash value vanished");

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
		hash_entry = hash_search(plperl_query_hash, query,
										 HASH_FIND,NULL);
		if (hash_entry == NULL)
			elog(ERROR, "spi_exec_prepared: Invalid prepared query passed");

		qdesc = hash_entry->query_data;

		if (qdesc == NULL)
			elog(ERROR, "spi_query_prepared: panic - plperl_query_hash value vanished");

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

	hash_entry = hash_search(plperl_query_hash, query,
										 HASH_FIND,NULL);
	if (hash_entry == NULL)
		elog(ERROR, "spi_exec_prepared: Invalid prepared query passed");

	qdesc = hash_entry->query_data;

	if (qdesc == NULL)
		elog(ERROR, "spi_exec_freeplan: panic - plperl_query_hash value vanished");

	/*
	 * free all memory before SPI_freeplan, so if it dies, nothing will be
	 * left over
	 */
	hash_search(plperl_query_hash, query, 
				HASH_REMOVE,NULL);

	plan = qdesc->plan;
	free(qdesc->argtypes);
	free(qdesc->arginfuncs);
	free(qdesc->argtypioparams);
	free(qdesc);

	SPI_freeplan(plan);
}

/*
 * Create a new SV from a string assumed to be in the current database's
 * encoding.
 */
static SV *
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
	int32	klen = strlen(key);

	/*
	 * This seems nowhere documented, but under Perl 5.8.0 and up,
	 * hv_store() recognizes a negative klen parameter as meaning
	 * a UTF-8 encoded key.  It does not appear that hashes track
	 * UTF-8-ness of keys at all in Perl 5.6.
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
	int32	klen = strlen(key);

	/* See notes in hv_store_string */
#if PERL_BCDVERSION >= 0x5008000L
	if (GetDatabaseEncoding() == PG_UTF8)
		klen = -klen;
#endif
	return hv_fetch(hv, key, klen, 0);
}
