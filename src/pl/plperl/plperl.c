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
 *	  $PostgreSQL: pgsql/src/pl/plperl/plperl.c,v 1.45 2004/07/01 20:50:22 joe Exp $
 *
 **********************************************************************/

#include "postgres.h"

/* system stuff */
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>

/* postgreSQL stuff */
#include "access/heapam.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"			/* need for SRF support */
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "tcop/tcopprot.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

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


/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct plperl_proc_desc
{
	char	   *proname;
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	bool		lanpltrusted;
	bool		fn_retistuple;	/* true, if function returns tuple */
	Oid			ret_oid;		/* Oid of returning type */
	FmgrInfo	result_in_func;
	Oid			result_typioparam;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	Oid			arg_typioparam[FUNC_MAX_ARGS];
	bool		arg_is_rowtype[FUNC_MAX_ARGS];
	SV		   *reference;
}	plperl_proc_desc;


/**********************************************************************
 * Global data
 **********************************************************************/
static int	plperl_firstcall = 1;
static PerlInterpreter *plperl_interp = NULL;
static HV  *plperl_proc_hash = NULL;
AV		   *g_row_keys = NULL;
AV		   *g_column_keys = NULL;
int			g_attr_num = 0;

/**********************************************************************
 * Forward declarations
 **********************************************************************/
static void plperl_init_all(void);
static void plperl_init_interp(void);

Datum		plperl_call_handler(PG_FUNCTION_ARGS);
void		plperl_init(void);

static Datum plperl_func_handler(PG_FUNCTION_ARGS);

static Datum plperl_trigger_handler(PG_FUNCTION_ARGS);
static plperl_proc_desc *compile_plperl_function(Oid fn_oid, bool is_trigger);

static SV  *plperl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc);
static void plperl_init_shared_libs(pTHX);


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
	/************************************************************
	 * Do initialization only once
	 ************************************************************/
	if (!plperl_firstcall)
		return;

	/************************************************************
	 * Free the proc hash table
	 ************************************************************/
	if (plperl_proc_hash != NULL)
	{
		hv_undef(plperl_proc_hash);
		SvREFCNT_dec((SV *) plperl_proc_hash);
		plperl_proc_hash = NULL;
	}

	/************************************************************
	 * Destroy the existing Perl interpreter
	 ************************************************************/
	if (plperl_interp != NULL)
	{
		perl_destruct(plperl_interp);
		perl_free(plperl_interp);
		plperl_interp = NULL;
	}

	/************************************************************
	 * Now recreate a new Perl interpreter
	 ************************************************************/
	plperl_init_interp();

	plperl_firstcall = 0;
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


/**********************************************************************
 * plperl_init_interp() - Create the Perl interpreter
 **********************************************************************/
static void
plperl_init_interp(void)
{

	char	   *embedding[3] = {
		"", "-e",

		/*
		 * no commas between the next lines please. They are supposed to be
		 * one string
		 */
		"require Safe; SPI::bootstrap(); use vars qw(%_SHARED);"
		"use vars qw($PLContainer); $PLContainer = new Safe('PLPerl');"
		"$PLContainer->permit_only(':default');$PLContainer->permit(':base_math');"
		"$PLContainer->share(qw[&elog &spi_exec_query &DEBUG &LOG &INFO &NOTICE &WARNING &ERROR %SHARED ]);"
		"sub ::mksafefunc { return $PLContainer->reval(qq[sub { $_[0] $_[1]}]); }"
		"sub ::mkunsafefunc {return eval(qq[ sub { $_[0] $_[1] } ]); }"
	};

	plperl_interp = perl_alloc();
	if (!plperl_interp)
		elog(ERROR, "could not allocate perl interpreter");

	perl_construct(plperl_interp);
	perl_parse(plperl_interp, plperl_init_shared_libs, 3, embedding, NULL);
	perl_run(plperl_interp);

	/************************************************************
	 * Initialize the proc and query hash tables
	 ************************************************************/
	plperl_proc_hash = newHV();

}

/**********************************************************************
 * turn a tuple into a hash expression and add it to a list
 **********************************************************************/
static void
plperl_sv_add_tuple_value(SV * rv, HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	char	   *value;
	char	   *key;

	sv_catpvf(rv, "{ ");

	for (i = 0; i < tupdesc->natts; i++)
	{
		key = SPI_fname(tupdesc, i + 1);
		value = SPI_getvalue(tuple, tupdesc, i + 1);
		if (value)
			sv_catpvf(rv, "%s => '%s'", key, value);
		else
			sv_catpvf(rv, "%s => undef", key);
		if (i != tupdesc->natts - 1)
			sv_catpvf(rv, ", ");
	}

	sv_catpvf(rv, " }");
}

/**********************************************************************
 * set up arguments for a trigger call
 **********************************************************************/
static SV  *
plperl_trigger_build_args(FunctionCallInfo fcinfo)
{
	TriggerData *tdata;
	TupleDesc	tupdesc;
	int			i = 0;
	SV		   *rv;

	rv = newSVpv("{ ", 0);

	tdata = (TriggerData *) fcinfo->context;

	tupdesc = tdata->tg_relation->rd_att;

	sv_catpvf(rv, "name => '%s'", tdata->tg_trigger->tgname);
	sv_catpvf(rv, ", relid => '%s'", DatumGetCString(DirectFunctionCall1(oidout, ObjectIdGetDatum(tdata->tg_relation->rd_id))));

	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
	{
		sv_catpvf(rv, ", event => 'INSERT'");
		sv_catpvf(rv, ", new =>");
		plperl_sv_add_tuple_value(rv, tdata->tg_trigtuple, tupdesc);
	}
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
	{
		sv_catpvf(rv, ", event => 'DELETE'");
		sv_catpvf(rv, ", old => ");
		plperl_sv_add_tuple_value(rv, tdata->tg_trigtuple, tupdesc);
	}
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
	{
		sv_catpvf(rv, ", event => 'UPDATE'");

		sv_catpvf(rv, ", new =>");
		plperl_sv_add_tuple_value(rv, tdata->tg_newtuple, tupdesc);

		sv_catpvf(rv, ", old => ");
		plperl_sv_add_tuple_value(rv, tdata->tg_trigtuple, tupdesc);
	}
	else
		sv_catpvf(rv, ", event => 'UNKNOWN'");

	sv_catpvf(rv, ", argc => %d", tdata->tg_trigger->tgnargs);

	if (tdata->tg_trigger->tgnargs != 0)
	{
		sv_catpvf(rv, ", args => [ ");
		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
		{
			sv_catpvf(rv, "%s", tdata->tg_trigger->tgargs[i]);
			if (i != tdata->tg_trigger->tgnargs - 1)
				sv_catpvf(rv, ", ");
		}
		sv_catpvf(rv, " ]");
	}
	sv_catpvf(rv, ", relname => '%s'", SPI_getrelname(tdata->tg_relation));

	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		sv_catpvf(rv, ", when => 'BEFORE'");
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		sv_catpvf(rv, ", when => 'AFTER'");
	else
		sv_catpvf(rv, ", when => 'UNKNOWN'");

	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		sv_catpvf(rv, ", level => 'ROW'");
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		sv_catpvf(rv, ", level => 'STATEMENT'");
	else
		sv_catpvf(rv, ", level => 'UNKNOWN'");

	sv_catpvf(rv, " }");

	rv = perl_eval_pv(SvPV(rv, PL_na), TRUE);

	return rv;
}


/**********************************************************************
 * check return value from plperl function
 **********************************************************************/
static int
plperl_is_set(SV * sv)
{
	int			i = 0;
	int			len = 0;
	int			set = 0;
	int			other = 0;
	AV		   *input_av;
	SV		  **val;

	if (SvTYPE(sv) != SVt_RV)
		return 0;

	if (SvTYPE(SvRV(sv)) == SVt_PVHV)
		return 0;

	if (SvTYPE(SvRV(sv)) == SVt_PVAV)
	{
		input_av = (AV *) SvRV(sv);
		len = av_len(input_av) + 1;

		for (i = 0; i < len; i++)
		{
			val = av_fetch(input_av, i, FALSE);
			if (SvTYPE(*val) == SVt_RV)
				set = 1;
			else
				other = 1;
		}
	}

	if (len == 0)
		return 1;
	if (set && !other)
		return 1;
	if (!set && other)
		return 0;
	if (set && other)
		elog(ERROR, "plperl: check your return value structure");
	if (!set && !other)
		elog(ERROR, "plperl: check your return value structure");

	return 0;					/* for compiler */
}

/**********************************************************************
 * extract a list of keys from a hash
 **********************************************************************/
static AV *
plperl_get_keys(HV * hv)
{
	AV		   *ret;
	SV		  **svp;
	int			key_count;
	SV		   *val;
	char	   *key;
	I32			klen;

	key_count = 0;
	ret = newAV();

	hv_iterinit(hv);
	while (val = hv_iternextsv(hv, (char **) &key, &klen))
	{
		av_store(ret, key_count, eval_pv(key, TRUE));
		key_count++;
	}
	hv_iterinit(hv);
	return ret;
}

/**********************************************************************
 * extract a given key (by index) from a list of keys
 **********************************************************************/
static char *
plperl_get_key(AV * keys, int index)
{
	SV		  **svp;
	int			len;

	len = av_len(keys) + 1;
	if (index < len)
		svp = av_fetch(keys, index, FALSE);
	else
		return NULL;
	return SvPV(*svp, PL_na);
}

/**********************************************************************
 * extract a value for a given key from a hash
 *
 * return NULL on error or if we got an undef
 *
 **********************************************************************/
static char *
plperl_get_elem(HV * hash, char *key)
{
	SV		  **svp;

	if (hv_exists_ent(hash, eval_pv(key, TRUE), FALSE))
		svp = hv_fetch(hash, key, strlen(key), FALSE);
	else
	{
		elog(ERROR, "plperl: key '%s' not found", key);
		return NULL;
	}
	return SvTYPE(*svp) == SVt_NULL ? NULL : SvPV(*svp, PL_na);
}

/**********************************************************************
 * set up the new tuple returned from a trigger
 **********************************************************************/
static HeapTuple
plperl_modify_tuple(HV * hvTD, TriggerData *tdata, HeapTuple otup, Oid fn_oid)
{
	SV		  **svp;
	HV		   *hvNew;
	AV		   *plkeys;
	char	   *platt;
	char	   *plval;
	HeapTuple	rtup;
	int			natts,
				i,
				attn,
				atti;
	int		   *volatile modattrs = NULL;
	Datum	   *volatile modvalues = NULL;
	char	   *volatile modnulls = NULL;
	TupleDesc	tupdesc;
	HeapTuple	typetup;

	tupdesc = tdata->tg_relation->rd_att;

	svp = hv_fetch(hvTD, "new", 3, FALSE);
	hvNew = (HV *) SvRV(*svp);

	if (SvTYPE(hvNew) != SVt_PVHV)
		elog(ERROR, "plperl: $_TD->{new} is not a hash");

	plkeys = plperl_get_keys(hvNew);
    natts = av_len(plkeys)+1;
    if (natts != tupdesc->natts)
        elog(ERROR, "plperl: $_TD->{new} has an incorrect number of keys.");

	modattrs = palloc0(natts * sizeof(int));
	modvalues = palloc0(natts * sizeof(Datum));
	modnulls = palloc0(natts * sizeof(char));

	for (i = 0; i < natts; i++)
	{
		FmgrInfo	finfo;
		Oid			typinput;
		Oid			typelem;

		platt = plperl_get_key(plkeys, i);

		attn = modattrs[i] = SPI_fnumber(tupdesc, platt);

		if (attn == SPI_ERROR_NOATTRIBUTE)
			elog(ERROR, "plperl: invalid attribute `%s' in tuple.", platt);
		atti = attn - 1;

		plval = plperl_get_elem(hvNew, platt);

		typetup = SearchSysCache(TYPEOID, ObjectIdGetDatum(tupdesc->attrs[atti]->atttypid), 0, 0, 0);
		typinput = ((Form_pg_type) GETSTRUCT(typetup))->typinput;
		typelem = ((Form_pg_type) GETSTRUCT(typetup))->typelem;
		ReleaseSysCache(typetup);
		fmgr_info(typinput, &finfo);

		if (plval)
		{
			modvalues[i] = FunctionCall3(&finfo,
										 CStringGetDatum(plval),
										 ObjectIdGetDatum(typelem),
					 Int32GetDatum(tupdesc->attrs[atti]->atttypmod));
			modnulls[i] = ' ';
		}
		else
		{
			modvalues[i] = (Datum) 0;
			modnulls[i] = 'n';
		}
	}
	rtup = SPI_modifytuple(tdata->tg_relation, otup, natts, modattrs, modvalues, modnulls);

	pfree(modattrs);
	pfree(modvalues);
	pfree(modnulls);
	if (rtup == NULL)
		elog(ERROR, "plperl: SPI_modifytuple failed -- error:  %d", SPI_result);

	return rtup;
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
	if (CALLED_AS_TRIGGER(fcinfo))
		retval = PointerGetDatum(plperl_trigger_handler(fcinfo));
	else
		retval = plperl_func_handler(fcinfo);

	return retval;
}


/**********************************************************************
 * plperl_create_sub()		- calls the perl interpreter to
 *		create the anonymous subroutine whose text is in the SV.
 *		Returns the SV containing the RV to the closure.
 **********************************************************************/
static SV  *
plperl_create_sub(char *s, bool trusted)
{
	dSP;
	SV		   *subref;
	int			count;

	ENTER;
	SAVETMPS;
	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newSVpv("my $_TD=$_[0]; shift;", 0)));
	XPUSHs(sv_2mortal(newSVpv(s, 0)));
	PUTBACK;

	/*
	 * G_KEEPERR seems to be needed here, else we don't recognize compile
	 * errors properly.  Perhaps it's because there's another level of
	 * eval inside mksafefunc?
	 */
	count = perl_call_pv((trusted ? "mksafefunc" : "mkunsafefunc"),
						 G_SCALAR | G_EVAL | G_KEEPERR);
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

EXTERN_C void boot_DynaLoader(pTHX_ CV * cv);
EXTERN_C void boot_SPI(pTHX_ CV * cv);

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
plperl_call_perl_func(plperl_proc_desc * desc, FunctionCallInfo fcinfo)
{
	dSP;
	SV		   *retval;
	int			i;
	int			count;

	ENTER;
	SAVETMPS;

	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newSVpv("undef", 0)));
	for (i = 0; i < desc->nargs; i++)
	{
		if (desc->arg_is_rowtype[i])
		{
			if (fcinfo->argnull[i])
				XPUSHs(&PL_sv_undef);
			else
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

				/*
				 * plperl_build_tuple_argument better return a mortal SV.
				 */
				hashref = plperl_build_tuple_argument(&tmptup, tupdesc);
				XPUSHs(hashref);
			}
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
								 ObjectIdGetDatum(desc->arg_typioparam[i]),
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
 * plperl_call_perl_trigger_func()	- calls a perl function affected by trigger
 * through the RV stored in the prodesc structure. massages the input parms properly
 **********************************************************************/
static SV  *
plperl_call_perl_trigger_func(plperl_proc_desc * desc, FunctionCallInfo fcinfo, SV * td)
{
	dSP;
	SV		   *retval;
	int			i;
	int			count;
	char	   *ret_test;

	ENTER;
	SAVETMPS;

	PUSHMARK(sp);
	XPUSHs(td);
	for (i = 0; i < ((TriggerData *) fcinfo->context)->tg_trigger->tgnargs; i++)
		XPUSHs(sv_2mortal(newSVpv(((TriggerData *) fcinfo->context)->tg_trigger->tgargs[i], 0)));
	PUTBACK;

	count = perl_call_sv(desc->reference, G_SCALAR | G_EVAL | G_KEEPERR);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "plperl: didn't get a return item from function");
	}

	if (SvTRUE(ERRSV))
	{
		POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "plperl: error from function: %s", SvPV(ERRSV, PL_na));
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
	/************************************************************
	 * Call the Perl function
	 ************************************************************/
	perlret = plperl_call_perl_func(prodesc, fcinfo);
	if (prodesc->fn_retistuple && SRF_IS_FIRSTCALL())
	{

		if (SvTYPE(perlret) != SVt_RV)
			elog(ERROR, "plperl: this function must return a reference");
		g_column_keys = newAV();
	}

	/************************************************************
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	if (!(perlret && SvOK(perlret) && SvTYPE(perlret)!=SVt_NULL ))
	{
		/* return NULL if Perl code returned undef */
		retval = (Datum) 0;
		fcinfo->isnull = true;
	}

	if (prodesc->fn_retistuple)
	{
		/* SRF support */
		HV		   *ret_hv;
		AV		   *ret_av;

		FuncCallContext *funcctx;
		int			call_cntr;
		int			max_calls;
		TupleDesc	tupdesc;
		TupleTableSlot *slot;
		AttInMetadata *attinmeta;
		bool		isset = 0;
		char	  **values = NULL;
		ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

		if (!rsinfo)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("returning a composite type is not allowed in this context"),
					errhint("This function is intended for use in the FROM clause.")));

		if (SvTYPE(perlret) != SVt_RV)
			elog(ERROR, "plperl: this function must return a reference");

		isset = plperl_is_set(perlret);

		if (SvTYPE(SvRV(perlret)) == SVt_PVHV)
			ret_hv = (HV *) SvRV(perlret);
		else
			ret_av = (AV *) SvRV(perlret);

		if (SRF_IS_FIRSTCALL())
		{
			MemoryContext oldcontext;
			int			i;

			funcctx = SRF_FIRSTCALL_INIT();

			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

			if (SvTYPE(SvRV(perlret)) == SVt_PVHV)
			{
				if (isset)
					funcctx->max_calls = hv_iterinit(ret_hv);
				else
					funcctx->max_calls = 1;
			}
			else
			{
				if (isset)
					funcctx->max_calls = av_len(ret_av) + 1;
				else
					funcctx->max_calls = 1;
			}

			tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

			g_attr_num = tupdesc->natts;

			for (i = 0; i < tupdesc->natts; i++)
				av_store(g_column_keys, i + 1, eval_pv(SPI_fname(tupdesc, i + 1), TRUE));

			slot = TupleDescGetSlot(tupdesc);
			funcctx->slot = slot;
			attinmeta = TupleDescGetAttInMetadata(tupdesc);
			funcctx->attinmeta = attinmeta;
			MemoryContextSwitchTo(oldcontext);
		}

		funcctx = SRF_PERCALL_SETUP();
		call_cntr = funcctx->call_cntr;
		max_calls = funcctx->max_calls;
		slot = funcctx->slot;
		attinmeta = funcctx->attinmeta;

		if (call_cntr < max_calls)
		{
			HeapTuple	tuple;
			Datum		result;
			int			i;
			char	   *column_key;
			char	   *elem;

			if (isset)
			{
				HV		   *row_hv;
				SV		  **svp;
				char	   *row_key;

				svp = av_fetch(ret_av, call_cntr, FALSE);

				row_hv = (HV *) SvRV(*svp);

				values = (char **) palloc(g_attr_num * sizeof(char *));

				for (i = 0; i < g_attr_num; i++)
				{
					column_key = plperl_get_key(g_column_keys, i + 1);
					elem = plperl_get_elem(row_hv, column_key);
					if (elem)
						values[i] = elem;
					else
						values[i] = NULL;
				}
			}
	else
	{
				int			i;

				values = (char **) palloc(g_attr_num * sizeof(char *));
				for (i = 0; i < g_attr_num; i++)
				{
					column_key = SPI_fname(tupdesc, i + 1);
					elem = plperl_get_elem(ret_hv, column_key);
					if (elem)
						values[i] = elem;
					else
						values[i] = NULL;
				}
			}
			tuple = BuildTupleFromCStrings(attinmeta, values);
			result = TupleGetDatum(slot, tuple);
			SRF_RETURN_NEXT(funcctx, result);
		}
		else
		{
			SvREFCNT_dec(perlret);
			SRF_RETURN_DONE(funcctx);
		}
	}
	else if (! fcinfo->isnull)
	{
		retval = FunctionCall3(&prodesc->result_in_func,
							   PointerGetDatum(SvPV(perlret, PL_na)),
							   ObjectIdGetDatum(prodesc->result_typioparam),
							   Int32GetDatum(-1));
	}

	SvREFCNT_dec(perlret);
	return retval;
}

/**********************************************************************
 * plperl_trigger_handler()		- Handler for trigger function calls
 **********************************************************************/
static Datum
plperl_trigger_handler(PG_FUNCTION_ARGS)
{
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval;
	char	   *tmp;
	SV		   *svTD;
	HV		   *hvTD;

	/* Find or compile the function */
	prodesc = compile_plperl_function(fcinfo->flinfo->fn_oid, true);

	/************************************************************
	* Call the Perl function
	************************************************************/
	/*
	* call perl trigger function and build TD hash
	*/
	svTD = plperl_trigger_build_args(fcinfo);
	perlret = plperl_call_perl_trigger_func(prodesc, fcinfo, svTD);

	hvTD = (HV *) SvRV(svTD);	/* convert SV TD structure to Perl Hash
								 * structure */

	tmp = SvPV(perlret, PL_na);

	/************************************************************
	* Disconnect from SPI manager and then create the return
	* values datum (if the input function does a palloc for it
	* this must not be allocated in the SPI memory context
	* because SPI_finish would free it).
	************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "plperl: SPI_finish() failed");

	if (!(perlret && SvOK(perlret)))
	{
		TriggerData *trigdata = ((TriggerData *) fcinfo->context);

		if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			retval = (Datum) trigdata->tg_trigtuple;
		else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			retval = (Datum) trigdata->tg_newtuple;
		else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			retval = (Datum) trigdata->tg_trigtuple;
	}
	else
	{
		if (!fcinfo->isnull)
		{

			HeapTuple	trv;

			if (strcasecmp(tmp, "SKIP") == 0)
				trv = NULL;
			else if (strcasecmp(tmp, "MODIFY") == 0)
			{
				TriggerData *trigdata = (TriggerData *) fcinfo->context;

				if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
					trv = plperl_modify_tuple(hvTD, trigdata, trigdata->tg_trigtuple, fcinfo->flinfo->fn_oid);
				else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
					trv = plperl_modify_tuple(hvTD, trigdata, trigdata->tg_newtuple, fcinfo->flinfo->fn_oid);
				else
				{
					trv = NULL;
					elog(WARNING, "plperl: Ignoring modified tuple in DELETE trigger");
				}
			}
			else if (strcasecmp(tmp, "OK"))
			{
				trv = NULL;
				elog(ERROR, "plperl: Expected return to be undef, 'SKIP' or 'MODIFY'");
			}
			else
			{
				trv = NULL;
				elog(ERROR, "plperl: Expected return to be undef, 'SKIP' or 'MODIFY'");
			}
			retval = PointerGetDatum(trv);
		}
	}

	SvREFCNT_dec(perlret);

	fcinfo->isnull = false;
	return retval;
}

/**********************************************************************
 * compile_plperl_function	- compile (or hopefully just look up) function
 **********************************************************************/
static plperl_proc_desc *
compile_plperl_function(Oid fn_oid, bool is_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[64];
	int			proname_len;
	plperl_proc_desc *prodesc = NULL;
	int			i;

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
		sprintf(internal_proname, "__PLPerl_proc_%u", fn_oid);
	else
		sprintf(internal_proname, "__PLPerl_proc_%u_trigger", fn_oid);

	proname_len = strlen(internal_proname);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	if (hv_exists(plperl_proc_hash, internal_proname, proname_len))
	{
		bool		uptodate;

		prodesc = (plperl_proc_desc *) SvIV(*hv_fetch(plperl_proc_hash,
									  internal_proname, proname_len, 0));

		/************************************************************
		 * If it's present, must check whether it's still up to date.
		 * This is needed because CREATE OR REPLACE FUNCTION can modify the
		 * function's pg_proc entry without changing its OID.
		 ************************************************************/
		uptodate = (prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			prodesc->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data));

		if (!uptodate)
		{
			/* need we delete old entry? */
			prodesc = NULL;
		}
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

			if (typeStruct->typtype == 'c' || procStruct->prorettype == RECORDOID)
			{
				prodesc->fn_retistuple = true;
				prodesc->ret_oid = typeStruct->typrelid;
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

				if (typeStruct->typtype == 'c')
					prodesc->arg_is_rowtype[i] = true;
				else
				{
					prodesc->arg_is_rowtype[i] = false;
					perm_fmgr_info(typeStruct->typoutput,
								   &(prodesc->arg_out_func[i]));
					prodesc->arg_typioparam[i] = getTypeIOParam(typeTup);
				}

				ReleaseSysCache(typeTup);
			}
		}

		/************************************************************
		 * create the text of the anonymous subroutine.
		 * we do not use a named subroutine so that we can call directly
		 * through the reference.
		 *
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
		prodesc->reference = plperl_create_sub(proc_source, prodesc->lanpltrusted);
		pfree(proc_source);
		if (!prodesc->reference)
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "could not create internal procedure \"%s\"",
				 internal_proname);
		}

		/************************************************************
		 * Add the proc description block to the hashtable
		 ************************************************************/
		hv_store(plperl_proc_hash, internal_proname, proname_len,
				 newSViv((IV) prodesc), 0);
	}

	ReleaseSysCache(procTup);

	return prodesc;
}


/**********************************************************************
 * plperl_build_tuple_argument() - Build a string for a ref to a hash
 *				  from all attributes of a given tuple
 **********************************************************************/
static SV  *
plperl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	SV		   *output;
	Datum		attr;
	bool		isnull;
	char	   *attname;
	char	   *outputstr;
	HeapTuple	typeTup;
	Oid			typoutput;
	Oid			typioparam;

	output = sv_2mortal(newSVpv("{", 0));

	for (i = 0; i < tupdesc->natts; i++)
	{
		/* ignore dropped attributes */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		attname = tupdesc->attrs[i]->attname.data;

		/************************************************************
		 * Get the attributes value
		 ************************************************************/
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/************************************************************
		 *	If it is null it will be set to undef in the hash.
		 ************************************************************/
		if (isnull)
		{
			sv_catpvf(output, "'%s' => undef,", attname);
			continue;
		}

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
		typioparam = getTypeIOParam(typeTup);
		ReleaseSysCache(typeTup);

		/************************************************************
		 * Append the attribute name and the value to the list.
		 ************************************************************/
		outputstr = DatumGetCString(OidFunctionCall3(typoutput,
													 attr,
											   ObjectIdGetDatum(typioparam),
						   Int32GetDatum(tupdesc->attrs[i]->atttypmod)));
		sv_catpvf(output, "'%s' => '%s',", attname, outputstr);
		pfree(outputstr);
	}

	sv_catpv(output, "}");
	output = perl_eval_pv(SvPV(output, PL_na), TRUE);
	return output;
}
