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
 **********************************************************************/


/* system stuff */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <setjmp.h>

/* postgreSQL stuff */
#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/elog.h"
#include "fmgr.h"
#include "access/heapam.h"

#include "tcop/tcopprot.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"

/* perl stuff */
/*
 * Evil Code Alert
 *
 * both posgreSQL and perl try to do 'the right thing'
 * and provide union semun if the platform doesn't define
 * it in a system header.
 * psql uses HAVE_UNION_SEMUN
 * perl uses HAS_UNION_SEMUN
 * together, they cause compile errors.
 * If we need it, the psql headers above will provide it.
 * So we tell perl that we have it.
 */
#ifndef HAS_UNION_SEMUN
#define HAS_UNION_SEMUN
#endif
#include <EXTERN.h>
#include <perl.h>


/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct plperl_proc_desc
{
	char	   *proname;
	FmgrInfo	result_in_func;
	Oid			result_in_elem;
	int			result_in_len;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	Oid			arg_out_elem[FUNC_MAX_ARGS];
	int			arg_out_len[FUNC_MAX_ARGS];
	int			arg_is_rel[FUNC_MAX_ARGS];
	SV		   *reference;
}			plperl_proc_desc;


/**********************************************************************
 * The information we cache about prepared and saved plans
 **********************************************************************/
typedef struct plperl_query_desc
{
	char		qname[20];
	void	   *plan;
	int			nargs;
	Oid		   *argtypes;
	FmgrInfo   *arginfuncs;
	Oid		   *argtypelems;
	Datum	   *argvalues;
	int		   *arglen;
}			plperl_query_desc;


/**********************************************************************
 * Global data
 **********************************************************************/
static int	plperl_firstcall = 1;
static int	plperl_call_level = 0;
static int	plperl_restart_in_progress = 0;
static PerlInterpreter *plperl_safe_interp = NULL;
static HV  *plperl_proc_hash = NULL;

#if REALLYHAVEITONTHEBALL
static Tcl_HashTable *plperl_query_hash = NULL;

#endif

/**********************************************************************
 * Forward declarations
 **********************************************************************/
static void plperl_init_all(void);
static void plperl_init_safe_interp(void);

Datum plperl_call_handler(FmgrInfo *proinfo,
					FmgrValues *proargs, bool *isNull);

static Datum plperl_func_handler(FmgrInfo *proinfo,
					FmgrValues *proargs, bool *isNull);

static SV  *plperl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc);
static void plperl_init_shared_libs(void);

#ifdef REALLYHAVEITONTHEBALL
static HeapTuple plperl_trigger_handler(FmgrInfo *proinfo);

static int plperl_elog(ClientData cdata, Tcl_Interp *interp,
			int argc, char *argv[]);
static int plperl_quote(ClientData cdata, Tcl_Interp *interp,
			 int argc, char *argv[]);

static int plperl_SPI_exec(ClientData cdata, Tcl_Interp *interp,
				int argc, char *argv[]);
static int plperl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				   int argc, char *argv[]);
static int plperl_SPI_execp(ClientData cdata, Tcl_Interp *interp,
				 int argc, char *argv[]);

static void plperl_set_tuple_values(Tcl_Interp *interp, char *arrayname,
						int tupno, HeapTuple tuple, TupleDesc tupdesc);

#endif


/**********************************************************************
 * plperl_init_all()		- Initialize all
 **********************************************************************/
static void
plperl_init_all(void)
{

	/************************************************************
	 * Do initialization only once
	 ************************************************************/
	if (!plperl_firstcall)
		return;


	/************************************************************
	 * Destroy the existing safe interpreter
	 ************************************************************/
	if (plperl_safe_interp != NULL)
	{
		perl_destruct(plperl_safe_interp);
		perl_free(plperl_safe_interp);
		plperl_safe_interp = NULL;
	}

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
	 * Free the prepared query hash table
	 ************************************************************/

	/*
	 * if (plperl_query_hash != NULL) { }
	 */

	/************************************************************
	 * Now recreate a new safe interpreter
	 ************************************************************/
	plperl_init_safe_interp();

	plperl_firstcall = 0;
	return;
}


/**********************************************************************
 * plperl_init_safe_interp() - Create the safe Perl interpreter
 **********************************************************************/
static void
plperl_init_safe_interp(void)
{

	char	   *embedding[] = {"", "-e", "use DynaLoader; require Safe; SPI::bootstrap()", "0"};

	plperl_safe_interp = perl_alloc();
	if (!plperl_safe_interp)
		elog(ERROR, "plperl_init_safe_interp(): could not allocate perl interpreter");

	perl_construct(plperl_safe_interp);
	perl_parse(plperl_safe_interp, plperl_init_shared_libs, 3, embedding, NULL);
	perl_run(plperl_safe_interp);



	/************************************************************
	 * Initialize the proc and query hash tables
	 ************************* ***********************************/
	plperl_proc_hash = newHV();

}



/**********************************************************************
 * plperl_call_handler		- This is the only visible function
 *				  of the PL interpreter. The PostgreSQL
 *				  function manager and trigger manager
 *				  call this function for execution of
 *				  perl procedures.
 **********************************************************************/

/* keep non-static */
Datum
plperl_call_handler(FmgrInfo *proinfo,
					FmgrValues *proargs,
					bool *isNull)
{
	Datum		retval;

	/************************************************************
	 * Initialize interpreters on first call
	 ************************************************************/
	if (plperl_firstcall)
		plperl_init_all();

	/************************************************************
	 * Connect to SPI manager
	 ************************************************************/
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "plperl: cannot connect to SPI manager");
	/************************************************************
	 * Keep track about the nesting of Tcl-SPI-Tcl-... calls
	 ************************************************************/
	plperl_call_level++;

	/************************************************************
	 * Determine if called as function or trigger and
	 * call appropriate subhandler
	 ************************************************************/
	if (CurrentTriggerData == NULL)
		retval = plperl_func_handler(proinfo, proargs, isNull);
	else
	{
		elog(ERROR, "plperl: can't use perl in triggers yet.");

		/*
		 * retval = (Datum) plperl_trigger_handler(proinfo);
		 */
		/* make the compiler happy */
		retval = (Datum) 0;
	}

	plperl_call_level--;

	return retval;
}


/**********************************************************************
 * plperl_create_sub()		- calls the perl interpreter to
 *		create the anonymous subroutine whose text is in the SV.
 *		Returns the SV containing the RV to the closure.
 **********************************************************************/
static
SV *
plperl_create_sub(SV * s)
{
	dSP;

	SV		   *subref = NULL;

	ENTER;
	SAVETMPS;
	PUSHMARK(SP);
	perl_eval_sv(s, G_SCALAR | G_EVAL | G_KEEPERR);
	SPAGAIN;

	if (SvTRUE(GvSV(errgv)))
	{
		POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "creation of function failed : %s", SvPV(GvSV(errgv), na));
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
		elog(ERROR, "plperl_create_sub: didn't get a code ref");
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

extern void boot_DynaLoader _((CV * cv));
extern void boot_Opcode _((CV * cv));
extern void boot_SPI _((CV * cv));

static void
plperl_init_shared_libs(void)
{
	char	   *file = __FILE__;

	newXS("DynaLoader::bootstrap", boot_DynaLoader, file);
	newXS("Opcode::bootstrap", boot_Opcode, file);
	newXS("SPI::bootstrap", boot_SPI, file);
}

/**********************************************************************
 * plperl_call_perl_func()		- calls a perl function through the RV
 *			stored in the prodesc structure. massages the input parms properly
 **********************************************************************/
static
SV *
plperl_call_perl_func(plperl_proc_desc * desc, FmgrValues *pargs)
{
	dSP;

	SV		   *retval;
	int			i;
	int			count;


	ENTER;
	SAVETMPS;

	PUSHMARK(sp);
	for (i = 0; i < desc->nargs; i++)
	{
		if (desc->arg_is_rel[i])
		{

			/*
			 * plperl_build_tuple_argument better return a mortal SV.
			 */
			SV		   *hashref = plperl_build_tuple_argument(
							  ((TupleTableSlot *) (pargs->data[i]))->val,
			 ((TupleTableSlot *) (pargs->data[i]))->ttc_tupleDescriptor);

			XPUSHs(hashref);
		}
		else
		{
			char	   *tmp = (*fmgr_faddr(&(desc->arg_out_func[i])))
			(pargs->data[i],
			 desc->arg_out_elem[i],
			 desc->arg_out_len[i]);

			XPUSHs(sv_2mortal(newSVpv(tmp, 0)));
			pfree(tmp);
		}
	}
	PUTBACK;
	count = perl_call_sv(desc->reference, G_SCALAR | G_EVAL | G_KEEPERR);

	SPAGAIN;

	if (count != 1)
	{
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "plperl : didn't get a return item from function");
	}

	if (SvTRUE(GvSV(errgv)))
	{
		POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		elog(ERROR, "plperl : error from function : %s", SvPV(GvSV(errgv), na));
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
plperl_func_handler(FmgrInfo *proinfo,
					FmgrValues *proargs,
					bool *isNull)
{
	int			i;
	char		internal_proname[512];
	int			proname_len;
	char	   *stroid;
	plperl_proc_desc *prodesc;
	SV		   *perlret;
	Datum		retval;
	sigjmp_buf	save_restart;

	/************************************************************
	 * Build our internal proc name from the functions Oid
	 ************************************************************/
	stroid = oidout(proinfo->fn_oid);
	strcpy(internal_proname, "__PLperl_proc_");
	strcat(internal_proname, stroid);
	pfree(stroid);
	proname_len = strlen(internal_proname);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	if (!hv_exists(plperl_proc_hash, internal_proname, proname_len))
	{
		/************************************************************
		 * If we haven't found it in the hashtable, we analyze
		 * the functions arguments and returntype and store
		 * the in-/out-functions in the prodesc block and create
		 * a new hashtable entry for it.
		 *
		 * Then we load the procedure into the safe interpreter.
		 ************************************************************/
		HeapTuple	procTup;
		HeapTuple	typeTup;
		Form_pg_proc procStruct;
		Form_pg_type typeStruct;
		SV		   *proc_internal_def;
		char		proc_internal_args[4096];
		char	   *proc_source;

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (plperl_proc_desc *) malloc(sizeof(plperl_proc_desc));
		prodesc->proname = malloc(strlen(internal_proname) + 1);
		strcpy(prodesc->proname, internal_proname);

		/************************************************************
		 * Lookup the pg_proc tuple by Oid
		 ************************************************************/
		procTup = SearchSysCacheTuple(PROCOID,
									  ObjectIdGetDatum(proinfo->fn_oid),
									  0, 0, 0);
		if (!HeapTupleIsValid(procTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "plperl: cache lookup from pg_proc failed");
		}
		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

		/************************************************************
		 * Get the required information for input conversion of the
		 * return value.
		 ************************************************************/
		typeTup = SearchSysCacheTuple(TYPEOID,
								ObjectIdGetDatum(procStruct->prorettype),
									  0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "plperl: cache lookup for return type failed");
		}
		typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

		if (typeStruct->typrelid != InvalidOid)
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "plperl: return types of tuples not supported yet");
		}

		fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
		prodesc->result_in_elem = (Oid) (typeStruct->typelem);
		prodesc->result_in_len = typeStruct->typlen;

		/************************************************************
		 * Get the required information for output conversion
		 * of all procedure arguments
		 ************************************************************/
		prodesc->nargs = proinfo->fn_nargs;
		proc_internal_args[0] = '\0';
		for (i = 0; i < proinfo->fn_nargs; i++)
		{
			typeTup = SearchSysCacheTuple(TYPEOID,
							ObjectIdGetDatum(procStruct->proargtypes[i]),
										  0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
			{
				free(prodesc->proname);
				free(prodesc);
				elog(ERROR, "plperl: cache lookup for argument type failed");
			}
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			if (typeStruct->typrelid != InvalidOid)
				prodesc->arg_is_rel[i] = 1;
			else
				prodesc->arg_is_rel[i] = 0;

			fmgr_info(typeStruct->typoutput, &(prodesc->arg_out_func[i]));
			prodesc->arg_out_elem[i] = (Oid) (typeStruct->typelem);
			prodesc->arg_out_len[i] = typeStruct->typlen;

		}

		/************************************************************
		 * create the text of the anonymous subroutine.
		 * we do not use a named subroutine so that we can call directly
		 * through the reference.
		 *
		 ************************************************************/
		proc_source = textout(&(procStruct->prosrc));

		/*
		 * the string has been split for readbility. please don't put
		 * commas between them. Hope everyone is ANSI
		 */
		proc_internal_def = newSVpvf(
									 "$::x = new Safe;"
									 "$::x->permit_only(':default');"
				   "$::x->share(qw[&elog &DEBUG &NOTICE &NOIND &ERROR]);"
									 "use strict;"
				   "return $::x->reval( q[ sub { %s } ]);", proc_source);

		pfree(proc_source);

		/************************************************************
		 * Create the procedure in the interpreter
		 ************************************************************/
		prodesc->reference = plperl_create_sub(proc_internal_def);
		if (!prodesc->reference)
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "plperl: cannot create internal procedure %s",
				 internal_proname);
		}

		/************************************************************
		 * Add the proc description block to the hashtable
		 ************************************************************/
		hv_store(plperl_proc_hash, internal_proname, proname_len,
				 newSViv((IV) prodesc), 0);
	}
	else
	{
		/************************************************************
		 * Found the proc description block in the hashtable
		 ************************************************************/
		prodesc = (plperl_proc_desc *) SvIV(*hv_fetch(plperl_proc_hash,
									  internal_proname, proname_len, 0));
	}


	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));

	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		plperl_restart_in_progress = 1;
		if (--plperl_call_level == 0)
			plperl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}


	/************************************************************
	 * Call the Perl function
	 ************************************************************/
	perlret = plperl_call_perl_func(prodesc, proargs);

	/************************************************************
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "plperl: SPI_finish() failed");

	retval = (Datum) (*fmgr_faddr(&prodesc->result_in_func))
		(SvPV(perlret, na),
		 prodesc->result_in_elem,
		 prodesc->result_in_len);

	SvREFCNT_dec(perlret);

	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	if (plperl_restart_in_progress)
	{
		if (--plperl_call_level == 0)
			plperl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	return retval;
}


#ifdef REALLYHAVEITONTHEBALL
/**********************************************************************
 * plperl_trigger_handler() - Handler for trigger calls
 **********************************************************************/
static HeapTuple
plperl_trigger_handler(FmgrInfo *proinfo)
{
	TriggerData *trigdata;
	char		internal_proname[512];
	char	   *stroid;
	Tcl_HashEntry *hashent;
	int			hashnew;
	plperl_proc_desc *prodesc;
	TupleDesc	tupdesc;
	HeapTuple	rettup;
	Tcl_DString tcl_cmd;
	Tcl_DString tcl_trigtup;
	Tcl_DString tcl_newtup;
	int			tcl_rc;
	int			i;

	int		   *modattrs;
	Datum	   *modvalues;
	char	   *modnulls;

	int			ret_numvals;
	char	  **ret_values;

	sigjmp_buf	save_restart;

	/************************************************************
	 * Save the current trigger data local
	 ************************************************************/
	trigdata = CurrentTriggerData;
	CurrentTriggerData = NULL;

	/************************************************************
	 * Build our internal proc name from the functions Oid
	 ************************************************************/
	stroid = oidout(proinfo->fn_oid);
	strcpy(internal_proname, "__PLTcl_proc_");
	strcat(internal_proname, stroid);
	pfree(stroid);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	hashent = Tcl_FindHashEntry(plperl_proc_hash, internal_proname);
	if (hashent == NULL)
	{
		/************************************************************
		 * If we haven't found it in the hashtable,
		 * we load the procedure into the safe interpreter.
		 ************************************************************/
		Tcl_DString proc_internal_def;
		Tcl_DString proc_internal_body;
		HeapTuple	procTup;
		Form_pg_proc procStruct;
		char	   *proc_source;

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (plperl_proc_desc *) malloc(sizeof(plperl_proc_desc));
		memset(prodesc, 0, sizeof(plperl_proc_desc));
		prodesc->proname = malloc(strlen(internal_proname) + 1);
		strcpy(prodesc->proname, internal_proname);

		/************************************************************
		 * Lookup the pg_proc tuple by Oid
		 ************************************************************/
		procTup = SearchSysCacheTuple(PROCOID,
									  ObjectIdGetDatum(proinfo->fn_oid),
									  0, 0, 0);
		if (!HeapTupleIsValid(procTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "plperl: cache lookup from pg_proc failed");
		}
		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

		/************************************************************
		 * Create the tcl command to define the internal
		 * procedure
		 ************************************************************/
		Tcl_DStringInit(&proc_internal_def);
		Tcl_DStringInit(&proc_internal_body);
		Tcl_DStringAppendElement(&proc_internal_def, "proc");
		Tcl_DStringAppendElement(&proc_internal_def, internal_proname);
		Tcl_DStringAppendElement(&proc_internal_def,
								 "TG_name TG_relid TG_relatts TG_when TG_level TG_op __PLTcl_Tup_NEW __PLTcl_Tup_OLD args");

		/************************************************************
		 * prefix procedure body with
		 * upvar #0 <internal_procname> GD
		 * and with appropriate setting of NEW, OLD,
		 * and the arguments as numerical variables.
		 ************************************************************/
		Tcl_DStringAppend(&proc_internal_body, "upvar #0 ", -1);
		Tcl_DStringAppend(&proc_internal_body, internal_proname, -1);
		Tcl_DStringAppend(&proc_internal_body, " GD\n", -1);

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

		proc_source = textout(&(procStruct->prosrc));
		Tcl_DStringAppend(&proc_internal_body, proc_source, -1);
		pfree(proc_source);
		Tcl_DStringAppendElement(&proc_internal_def,
								 Tcl_DStringValue(&proc_internal_body));
		Tcl_DStringFree(&proc_internal_body);

		/************************************************************
		 * Create the procedure in the safe interpreter
		 ************************************************************/
		tcl_rc = Tcl_GlobalEval(plperl_safe_interp,
								Tcl_DStringValue(&proc_internal_def));
		Tcl_DStringFree(&proc_internal_def);
		if (tcl_rc != TCL_OK)
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "plperl: cannot create internal procedure %s - %s",
				 internal_proname, plperl_safe_interp->result);
		}

		/************************************************************
		 * Add the proc description block to the hashtable
		 ************************************************************/
		hashent = Tcl_CreateHashEntry(plperl_proc_hash,
									  prodesc->proname, &hashnew);
		Tcl_SetHashValue(hashent, (ClientData) prodesc);
	}
	else
	{
		/************************************************************
		 * Found the proc description block in the hashtable
		 ************************************************************/
		prodesc = (plperl_proc_desc *) Tcl_GetHashValue(hashent);
	}

	tupdesc = trigdata->tg_relation->rd_att;

	/************************************************************
	 * Create the tcl command to call the internal
	 * proc in the safe interpreter
	 ************************************************************/
	Tcl_DStringInit(&tcl_cmd);
	Tcl_DStringInit(&tcl_trigtup);
	Tcl_DStringInit(&tcl_newtup);

	/************************************************************
	 * We call external functions below - care for elog(ERROR)
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		Tcl_DStringFree(&tcl_cmd);
		Tcl_DStringFree(&tcl_trigtup);
		Tcl_DStringFree(&tcl_newtup);
		plperl_restart_in_progress = 1;
		if (--plperl_call_level == 0)
			plperl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	/* The procedure name */
	Tcl_DStringAppendElement(&tcl_cmd, internal_proname);

	/* The trigger name for argument TG_name */
	Tcl_DStringAppendElement(&tcl_cmd, trigdata->tg_trigger->tgname);

	/* The oid of the trigger relation for argument TG_relid */
	stroid = oidout(trigdata->tg_relation->rd_id);
	Tcl_DStringAppendElement(&tcl_cmd, stroid);
	pfree(stroid);

	/* A list of attribute names for argument TG_relatts */
	Tcl_DStringAppendElement(&tcl_trigtup, "");
	for (i = 0; i < tupdesc->natts; i++)
		Tcl_DStringAppendElement(&tcl_trigtup, tupdesc->attrs[i]->attname.data);
	Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));
	Tcl_DStringFree(&tcl_trigtup);
	Tcl_DStringInit(&tcl_trigtup);

	/* The when part of the event for TG_when */
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		Tcl_DStringAppendElement(&tcl_cmd, "BEFORE");
	else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		Tcl_DStringAppendElement(&tcl_cmd, "AFTER");
	else
		Tcl_DStringAppendElement(&tcl_cmd, "UNKNOWN");

	/* The level part of the event for TG_level */
	if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		Tcl_DStringAppendElement(&tcl_cmd, "ROW");
	else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		Tcl_DStringAppendElement(&tcl_cmd, "STATEMENT");
	else
		Tcl_DStringAppendElement(&tcl_cmd, "UNKNOWN");

	/* Build the data list for the trigtuple */
	plperl_build_tuple_argument(trigdata->tg_trigtuple,
								tupdesc, &tcl_trigtup);

	/*
	 * Now the command part of the event for TG_op and data for NEW and
	 * OLD
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

		plperl_build_tuple_argument(trigdata->tg_newtuple,
									tupdesc, &tcl_newtup);

		Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_newtup));
		Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));

		rettup = trigdata->tg_newtuple;
	}
	else
	{
		Tcl_DStringAppendElement(&tcl_cmd, "UNKNOWN");

		Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));
		Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&tcl_trigtup));

		rettup = trigdata->tg_trigtuple;
	}

	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	Tcl_DStringFree(&tcl_trigtup);
	Tcl_DStringFree(&tcl_newtup);

	/************************************************************
	 * Finally append the arguments from CREATE TRIGGER
	 ************************************************************/
	for (i = 0; i < trigdata->tg_trigger->tgnargs; i++)
		Tcl_DStringAppendElement(&tcl_cmd, trigdata->tg_trigger->tgargs[i]);

	/************************************************************
	 * Call the Tcl function
	 ************************************************************/
	tcl_rc = Tcl_GlobalEval(plperl_safe_interp, Tcl_DStringValue(&tcl_cmd));
	Tcl_DStringFree(&tcl_cmd);

	/************************************************************
	 * Check the return code from Tcl and handle
	 * our special restart mechanism to get rid
	 * of all nested call levels on transaction
	 * abort.
	 ************************************************************/
	if (tcl_rc == TCL_ERROR || plperl_restart_in_progress)
	{
		if (!plperl_restart_in_progress)
		{
			plperl_restart_in_progress = 1;
			if (--plperl_call_level == 0)
				plperl_restart_in_progress = 0;
			elog(ERROR, "plperl: %s", plperl_safe_interp->result);
		}
		if (--plperl_call_level == 0)
			plperl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	switch (tcl_rc)
	{
		case TCL_OK:
			break;

		default:
			elog(ERROR, "plperl: unsupported TCL return code %d", tcl_rc);
	}

	/************************************************************
	 * The return value from the procedure might be one of
	 * the magic strings OK or SKIP or a list from array get
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "plperl: SPI_finish() failed");

	if (strcmp(plperl_safe_interp->result, "OK") == 0)
		return rettup;
	if (strcmp(plperl_safe_interp->result, "SKIP") == 0)
	{
		return (HeapTuple) NULL;;
	}

	/************************************************************
	 * Convert the result value from the safe interpreter
	 * and setup structures for SPI_modifytuple();
	 ************************************************************/
	if (Tcl_SplitList(plperl_safe_interp, plperl_safe_interp->result,
					  &ret_numvals, &ret_values) != TCL_OK)
	{
		elog(NOTICE, "plperl: cannot split return value from trigger");
		elog(ERROR, "plperl: %s", plperl_safe_interp->result);
	}

	if (ret_numvals % 2 != 0)
	{
		ckfree(ret_values);
		elog(ERROR, "plperl: invalid return list from trigger - must have even # of elements");
	}

	modattrs = (int *) palloc(tupdesc->natts * sizeof(int));
	modvalues = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
	for (i = 0; i < tupdesc->natts; i++)
	{
		modattrs[i] = i + 1;
		modvalues[i] = (Datum) NULL;
	}

	modnulls = palloc(tupdesc->natts + 1);
	memset(modnulls, 'n', tupdesc->natts);
	modnulls[tupdesc->natts] = '\0';

	/************************************************************
	 * Care for possible elog(ERROR)'s below
	 ************************************************************/
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		ckfree(ret_values);
		plperl_restart_in_progress = 1;
		if (--plperl_call_level == 0)
			plperl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	i = 0;
	while (i < ret_numvals)
	{
		int			attnum;
		HeapTuple	typeTup;
		Oid			typinput;
		Oid			typelem;
		FmgrInfo	finfo;

		/************************************************************
		 * Ignore pseudo elements with a dot name
		 ************************************************************/
		if (*(ret_values[i]) == '.')
		{
			i += 2;
			continue;
		}

		/************************************************************
		 * Get the attribute number
		 ************************************************************/
		attnum = SPI_fnumber(tupdesc, ret_values[i++]);
		if (attnum == SPI_ERROR_NOATTRIBUTE)
			elog(ERROR, "plperl: invalid attribute '%s'", ret_values[--i]);

		/************************************************************
		 * Lookup the attribute type in the syscache
		 * for the input function
		 ************************************************************/
		typeTup = SearchSysCacheTuple(TYPEOID,
				  ObjectIdGetDatum(tupdesc->attrs[attnum - 1]->atttypid),
									  0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			elog(ERROR, "plperl: Cache lookup for attribute '%s' type %ld failed",
				 ret_values[--i],
				 ObjectIdGetDatum(tupdesc->attrs[attnum - 1]->atttypid));
		}
		typinput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typinput);
		typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);

		/************************************************************
		 * Set the attribute to NOT NULL and convert the contents
		 ************************************************************/
		modnulls[attnum - 1] = ' ';
		fmgr_info(typinput, &finfo);
		modvalues[attnum - 1] = (Datum) (*fmgr_faddr(&finfo))
			(ret_values[i++],
			 typelem,
			 (!VARLENA_FIXED_SIZE(tupdesc->attrs[attnum - 1]))
			 ? tupdesc->attrs[attnum - 1]->attlen
			 : tupdesc->attrs[attnum - 1]->atttypmod
			);
	}


	rettup = SPI_modifytuple(trigdata->tg_relation, rettup, tupdesc->natts,
							 modattrs, modvalues, modnulls);

	pfree(modattrs);
	pfree(modvalues);
	pfree(modnulls);

	if (rettup == NULL)
		elog(ERROR, "plperl: SPI_modifytuple() failed - RC = %d\n", SPI_result);

	ckfree(ret_values);
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

	return rettup;
}


/**********************************************************************
 * plperl_elog()		- elog() support for PLTcl
 **********************************************************************/
static int
plperl_elog(ClientData cdata, Tcl_Interp *interp,
			int argc, char *argv[])
{
	int			level;
	sigjmp_buf	save_restart;

	/************************************************************
	 * Suppress messages during the restart process
	 ************************************************************/
	if (plperl_restart_in_progress)
		return TCL_ERROR;

	/************************************************************
	 * Catch the restart longjmp and begin a controlled
	 * return though all interpreter levels if it happens
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		plperl_restart_in_progress = 1;
		return TCL_ERROR;
	}

	if (argc != 3)
	{
		Tcl_SetResult(interp, "syntax error - 'elog level msg'",
					  TCL_VOLATILE);
		return TCL_ERROR;
	}

	if (strcmp(argv[1], "NOTICE") == 0)
		level = NOTICE;
	else if (strcmp(argv[1], "WARN") == 0)
		level = ERROR;
	else if (strcmp(argv[1], "ERROR") == 0)
		level = ERROR;
	else if (strcmp(argv[1], "FATAL") == 0)
		level = FATAL;
	else if (strcmp(argv[1], "DEBUG") == 0)
		level = DEBUG;
	else if (strcmp(argv[1], "NOIND") == 0)
		level = NOIND;
	else
	{
		Tcl_AppendResult(interp, "Unknown elog level '", argv[1],
						 "'", NULL);
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		return TCL_ERROR;
	}

	/************************************************************
	 * Call elog(), restore the original restart address
	 * and return to the caller (if not catched)
	 ************************************************************/
	elog(level, argv[2]);
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	return TCL_OK;
}


/**********************************************************************
 * plperl_quote()	- quote literal strings that are to
 *			  be used in SPI_exec query strings
 **********************************************************************/
static int
plperl_quote(ClientData cdata, Tcl_Interp *interp,
			 int argc, char *argv[])
{
	char	   *tmp;
	char	   *cp1;
	char	   *cp2;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (argc != 2)
	{
		Tcl_SetResult(interp, "syntax error - 'quote string'", TCL_VOLATILE);
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
 * plperl_SPI_exec()		- The builtin SPI_exec command
 *				  for the safe interpreter
 **********************************************************************/
static int
plperl_SPI_exec(ClientData cdata, Tcl_Interp *interp,
				int argc, char *argv[])
{
	int			spi_rc;
	char		buf[64];
	int			count = 0;
	char	   *arrayname = NULL;
	int			query_idx;
	int			i;
	int			loop_rc;
	int			ntuples;
	HeapTuple  *tuples;
	TupleDesc	tupdesc = NULL;
	sigjmp_buf	save_restart;

	char	   *usage = "syntax error - 'SPI_exec "
	"?-count n? "
	"?-array name? query ?loop body?";

	/************************************************************
	 * Don't do anything if we are already in restart mode
	 ************************************************************/
	if (plperl_restart_in_progress)
		return TCL_ERROR;

	/************************************************************
	 * Check the call syntax and get the count option
	 ************************************************************/
	if (argc < 2)
	{
		Tcl_SetResult(interp, usage, TCL_VOLATILE);
		return TCL_ERROR;
	}

	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "-array") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_VOLATILE);
				return TCL_ERROR;
			}
			arrayname = argv[i++];
			continue;
		}

		if (strcmp(argv[i], "-count") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_VOLATILE);
				return TCL_ERROR;
			}
			if (Tcl_GetInt(interp, argv[i++], &count) != TCL_OK)
				return TCL_ERROR;
			continue;
		}

		break;
	}

	query_idx = i;
	if (query_idx >= argc)
	{
		Tcl_SetResult(interp, usage, TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * Prepare to start a controlled return through all
	 * interpreter levels on transaction abort
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		plperl_restart_in_progress = 1;
		Tcl_SetResult(interp, "Transaction abort", TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * Execute the query and handle return codes
	 ************************************************************/
	spi_rc = SPI_exec(argv[query_idx], count);
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

	switch (spi_rc)
	{
		case SPI_OK_UTILITY:
			Tcl_SetResult(interp, "0", TCL_VOLATILE);
			return TCL_OK;

		case SPI_OK_SELINTO:
		case SPI_OK_INSERT:
		case SPI_OK_DELETE:
		case SPI_OK_UPDATE:
			sprintf(buf, "%d", SPI_processed);
			Tcl_SetResult(interp, buf, TCL_VOLATILE);
			return TCL_OK;

		case SPI_OK_SELECT:
			break;

		case SPI_ERROR_ARGUMENT:
			Tcl_SetResult(interp,
						"plperl: SPI_exec() failed - SPI_ERROR_ARGUMENT",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_UNCONNECTED:
			Tcl_SetResult(interp,
					 "plperl: SPI_exec() failed - SPI_ERROR_UNCONNECTED",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_COPY:
			Tcl_SetResult(interp,
						  "plperl: SPI_exec() failed - SPI_ERROR_COPY",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_CURSOR:
			Tcl_SetResult(interp,
						  "plperl: SPI_exec() failed - SPI_ERROR_CURSOR",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_TRANSACTION:
			Tcl_SetResult(interp,
					 "plperl: SPI_exec() failed - SPI_ERROR_TRANSACTION",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_OPUNKNOWN:
			Tcl_SetResult(interp,
					   "plperl: SPI_exec() failed - SPI_ERROR_OPUNKNOWN",
						  TCL_VOLATILE);
			return TCL_ERROR;

		default:
			sprintf(buf, "%d", spi_rc);
			Tcl_AppendResult(interp, "plperl: SPI_exec() failed - ",
							 "unknown RC ", buf, NULL);
			return TCL_ERROR;
	}

	/************************************************************
	 * Only SELECT queries fall through to here - remember the
	 * tuples we got
	 ************************************************************/

	ntuples = SPI_processed;
	if (ntuples > 0)
	{
		tuples = SPI_tuptable->vals;
		tupdesc = SPI_tuptable->tupdesc;
	}

	/************************************************************
	 * Again prepare for elog(ERROR)
	 ************************************************************/
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		plperl_restart_in_progress = 1;
		Tcl_SetResult(interp, "Transaction abort", TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * If there is no loop body given, just set the variables
	 * from the first tuple (if any) and return the number of
	 * tuples selected
	 ************************************************************/
	if (argc == query_idx + 1)
	{
		if (ntuples > 0)
			plperl_set_tuple_values(interp, arrayname, 0, tuples[0], tupdesc);
		sprintf(buf, "%d", ntuples);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		return TCL_OK;
	}

	/************************************************************
	 * There is a loop body - process all tuples and evaluate
	 * the body on each
	 ************************************************************/
	query_idx++;
	for (i = 0; i < ntuples; i++)
	{
		plperl_set_tuple_values(interp, arrayname, i, tuples[i], tupdesc);

		loop_rc = Tcl_Eval(interp, argv[query_idx]);

		if (loop_rc == TCL_OK)
			continue;
		if (loop_rc == TCL_CONTINUE)
			continue;
		if (loop_rc == TCL_RETURN)
		{
			memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
			return TCL_RETURN;
		}
		if (loop_rc == TCL_BREAK)
			break;
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		return TCL_ERROR;
	}

	/************************************************************
	 * Finally return the number of tuples
	 ************************************************************/
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	sprintf(buf, "%d", ntuples);
	Tcl_SetResult(interp, buf, TCL_VOLATILE);
	return TCL_OK;
}


/**********************************************************************
 * plperl_SPI_prepare()		- Builtin support for prepared plans
 *				  The Tcl command SPI_prepare
 *				  allways saves the plan using
 *				  SPI_saveplan and returns a key for
 *				  access. There is no chance to prepare
 *				  and not save the plan currently.
 **********************************************************************/
static int
plperl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				   int argc, char *argv[])
{
	int			nargs;
	char	  **args;
	plperl_query_desc *qdesc;
	void	   *plan;
	int			i;
	HeapTuple	typeTup;
	Tcl_HashEntry *hashent;
	int			hashnew;
	sigjmp_buf	save_restart;

	/************************************************************
	 * Don't do anything if we are already in restart mode
	 ************************************************************/
	if (plperl_restart_in_progress)
		return TCL_ERROR;

	/************************************************************
	 * Check the call syntax
	 ************************************************************/
	if (argc != 3)
	{
		Tcl_SetResult(interp, "syntax error - 'SPI_prepare query argtypes'",
					  TCL_VOLATILE);
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
	qdesc = (plperl_query_desc *) malloc(sizeof(plperl_query_desc));
	sprintf(qdesc->qname, "%lx", (long) qdesc);
	qdesc->nargs = nargs;
	qdesc->argtypes = (Oid *) malloc(nargs * sizeof(Oid));
	qdesc->arginfuncs = (FmgrInfo *) malloc(nargs * sizeof(FmgrInfo));
	qdesc->argtypelems = (Oid *) malloc(nargs * sizeof(Oid));
	qdesc->argvalues = (Datum *) malloc(nargs * sizeof(Datum));
	qdesc->arglen = (int *) malloc(nargs * sizeof(int));

	/************************************************************
	 * Prepare to start a controlled return through all
	 * interpreter levels on transaction abort
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		plperl_restart_in_progress = 1;
		free(qdesc->argtypes);
		free(qdesc->arginfuncs);
		free(qdesc->argtypelems);
		free(qdesc->argvalues);
		free(qdesc->arglen);
		free(qdesc);
		ckfree(args);
		return TCL_ERROR;
	}

	/************************************************************
	 * Lookup the argument types by name in the system cache
	 * and remember the required information for input conversion
	 ************************************************************/
	for (i = 0; i < nargs; i++)
	{
		typeTup = SearchSysCacheTuple(TYPNAME,
									  PointerGetDatum(args[i]),
									  0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "plperl: Cache lookup of type %s failed", args[i]);
		qdesc->argtypes[i] = typeTup->t_data->t_oid;
		fmgr_info(((Form_pg_type) GETSTRUCT(typeTup))->typinput,
				  &(qdesc->arginfuncs[i]));
		qdesc->argtypelems[i] = ((Form_pg_type) GETSTRUCT(typeTup))->typelem;
		qdesc->argvalues[i] = (Datum) NULL;
		qdesc->arglen[i] = (int) (((Form_pg_type) GETSTRUCT(typeTup))->typlen);
	}

	/************************************************************
	 * Prepare the plan and check for errors
	 ************************************************************/
	plan = SPI_prepare(argv[1], nargs, qdesc->argtypes);

	if (plan == NULL)
	{
		char		buf[128];
		char	   *reason;

		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

		switch (SPI_result)
		{
			case SPI_ERROR_ARGUMENT:
				reason = "SPI_ERROR_ARGUMENT";
				break;

			case SPI_ERROR_UNCONNECTED:
				reason = "SPI_ERROR_UNCONNECTED";
				break;

			case SPI_ERROR_COPY:
				reason = "SPI_ERROR_COPY";
				break;

			case SPI_ERROR_CURSOR:
				reason = "SPI_ERROR_CURSOR";
				break;

			case SPI_ERROR_TRANSACTION:
				reason = "SPI_ERROR_TRANSACTION";
				break;

			case SPI_ERROR_OPUNKNOWN:
				reason = "SPI_ERROR_OPUNKNOWN";
				break;

			default:
				sprintf(buf, "unknown RC %d", SPI_result);
				reason = buf;
				break;

		}

		elog(ERROR, "plperl: SPI_prepare() failed - %s", reason);
	}

	/************************************************************
	 * Save the plan
	 ************************************************************/
	qdesc->plan = SPI_saveplan(plan);
	if (qdesc->plan == NULL)
	{
		char		buf[128];
		char	   *reason;

		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

		switch (SPI_result)
		{
			case SPI_ERROR_ARGUMENT:
				reason = "SPI_ERROR_ARGUMENT";
				break;

			case SPI_ERROR_UNCONNECTED:
				reason = "SPI_ERROR_UNCONNECTED";
				break;

			default:
				sprintf(buf, "unknown RC %d", SPI_result);
				reason = buf;
				break;

		}

		elog(ERROR, "plperl: SPI_saveplan() failed - %s", reason);
	}

	/************************************************************
	 * Insert a hashtable entry for the plan and return
	 * the key to the caller
	 ************************************************************/
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	hashent = Tcl_CreateHashEntry(plperl_query_hash, qdesc->qname, &hashnew);
	Tcl_SetHashValue(hashent, (ClientData) qdesc);

	Tcl_SetResult(interp, qdesc->qname, TCL_VOLATILE);
	return TCL_OK;
}


/**********************************************************************
 * plperl_SPI_execp()		- Execute a prepared plan
 **********************************************************************/
static int
plperl_SPI_execp(ClientData cdata, Tcl_Interp *interp,
				 int argc, char *argv[])
{
	int			spi_rc;
	char		buf[64];
	int			i,
				j;
	int			loop_body;
	Tcl_HashEntry *hashent;
	plperl_query_desc *qdesc;
	char	   *nulls = NULL;
	char	   *arrayname = NULL;
	int			count = 0;
	int			callnargs;
	static char **callargs = NULL;
	int			loop_rc;
	int			ntuples;
	HeapTuple  *tuples = NULL;
	TupleDesc	tupdesc = NULL;
	sigjmp_buf	save_restart;

	char	   *usage = "syntax error - 'SPI_execp "
	"?-nulls string? ?-count n? "
	"?-array name? query ?args? ?loop body?";

	/************************************************************
	 * Tidy up from an earlier abort
	 ************************************************************/
	if (callargs != NULL)
	{
		ckfree(callargs);
		callargs = NULL;
	}

	/************************************************************
	 * Don't do anything if we are already in restart mode
	 ************************************************************/
	if (plperl_restart_in_progress)
		return TCL_ERROR;

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
				Tcl_SetResult(interp, usage, TCL_VOLATILE);
				return TCL_ERROR;
			}
			arrayname = argv[i++];
			continue;
		}
		if (strcmp(argv[i], "-nulls") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_VOLATILE);
				return TCL_ERROR;
			}
			nulls = argv[i++];
			continue;
		}
		if (strcmp(argv[i], "-count") == 0)
		{
			if (++i >= argc)
			{
				Tcl_SetResult(interp, usage, TCL_VOLATILE);
				return TCL_ERROR;
			}
			if (Tcl_GetInt(interp, argv[i++], &count) != TCL_OK)
				return TCL_ERROR;
			continue;
		}

		break;
	}

	/************************************************************
	 * Check minimum call arguments
	 ************************************************************/
	if (i >= argc)
	{
		Tcl_SetResult(interp, usage, TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * Get the prepared plan descriptor by it's key
	 ************************************************************/
	hashent = Tcl_FindHashEntry(plperl_query_hash, argv[i++]);
	if (hashent == NULL)
	{
		Tcl_AppendResult(interp, "invalid queryid '", argv[--i], "'", NULL);
		return TCL_ERROR;
	}
	qdesc = (plperl_query_desc *) Tcl_GetHashValue(hashent);

	/************************************************************
	 * If a nulls string is given, check for correct length
	 ************************************************************/
	if (nulls != NULL)
	{
		if (strlen(nulls) != qdesc->nargs)
		{
			Tcl_SetResult(interp,
				   "length of nulls string doesn't match # of arguments",
						  TCL_VOLATILE);
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
			Tcl_SetResult(interp, "missing argument list", TCL_VOLATILE);
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
						  TCL_VOLATILE);
			if (callargs != NULL)
			{
				ckfree(callargs);
				callargs = NULL;
			}
			return TCL_ERROR;
		}

		/************************************************************
		 * Prepare to start a controlled return through all
		 * interpreter levels on transaction abort during the
		 * parse of the arguments
		 ************************************************************/
		memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
		if (sigsetjmp(Warn_restart, 1) != 0)
		{
			memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
			for (j = 0; j < callnargs; j++)
			{
				if (qdesc->arglen[j] < 0 &&
					qdesc->argvalues[j] != (Datum) NULL)
				{
					pfree((char *) (qdesc->argvalues[j]));
					qdesc->argvalues[j] = (Datum) NULL;
				}
			}
			ckfree(callargs);
			callargs = NULL;
			plperl_restart_in_progress = 1;
			Tcl_SetResult(interp, "Transaction abort", TCL_VOLATILE);
			return TCL_ERROR;
		}

		/************************************************************
		 * Setup the value array for the SPI_execp() using
		 * the type specific input functions
		 ************************************************************/
		for (j = 0; j < callnargs; j++)
		{
			qdesc->argvalues[j] = (Datum) (*fmgr_faddr(&qdesc->arginfuncs[j]))
				(callargs[j],
				 qdesc->argtypelems[j],
				 qdesc->arglen[j]);
		}

		/************************************************************
		 * Free the splitted argument value list
		 ************************************************************/
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		ckfree(callargs);
		callargs = NULL;
	}
	else
		callnargs = 0;

	/************************************************************
	 * Remember the index of the last processed call
	 * argument - a loop body for SELECT might follow
	 ************************************************************/
	loop_body = i;

	/************************************************************
	 * Prepare to start a controlled return through all
	 * interpreter levels on transaction abort
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		for (j = 0; j < callnargs; j++)
		{
			if (qdesc->arglen[j] < 0 && qdesc->argvalues[j] != (Datum) NULL)
			{
				pfree((char *) (qdesc->argvalues[j]));
				qdesc->argvalues[j] = (Datum) NULL;
			}
		}
		plperl_restart_in_progress = 1;
		Tcl_SetResult(interp, "Transaction abort", TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * Execute the plan
	 ************************************************************/
	spi_rc = SPI_execp(qdesc->plan, qdesc->argvalues, nulls, count);
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

	/************************************************************
	 * For varlena data types, free the argument values
	 ************************************************************/
	for (j = 0; j < callnargs; j++)
	{
		if (qdesc->arglen[j] < 0 && qdesc->argvalues[j] != (Datum) NULL)
		{
			pfree((char *) (qdesc->argvalues[j]));
			qdesc->argvalues[j] = (Datum) NULL;
		}
	}

	/************************************************************
	 * Check the return code from SPI_execp()
	 ************************************************************/
	switch (spi_rc)
	{
		case SPI_OK_UTILITY:
			Tcl_SetResult(interp, "0", TCL_VOLATILE);
			return TCL_OK;

		case SPI_OK_SELINTO:
		case SPI_OK_INSERT:
		case SPI_OK_DELETE:
		case SPI_OK_UPDATE:
			sprintf(buf, "%d", SPI_processed);
			Tcl_SetResult(interp, buf, TCL_VOLATILE);
			return TCL_OK;

		case SPI_OK_SELECT:
			break;

		case SPI_ERROR_ARGUMENT:
			Tcl_SetResult(interp,
						"plperl: SPI_exec() failed - SPI_ERROR_ARGUMENT",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_UNCONNECTED:
			Tcl_SetResult(interp,
					 "plperl: SPI_exec() failed - SPI_ERROR_UNCONNECTED",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_COPY:
			Tcl_SetResult(interp,
						  "plperl: SPI_exec() failed - SPI_ERROR_COPY",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_CURSOR:
			Tcl_SetResult(interp,
						  "plperl: SPI_exec() failed - SPI_ERROR_CURSOR",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_TRANSACTION:
			Tcl_SetResult(interp,
					 "plperl: SPI_exec() failed - SPI_ERROR_TRANSACTION",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_OPUNKNOWN:
			Tcl_SetResult(interp,
					   "plperl: SPI_exec() failed - SPI_ERROR_OPUNKNOWN",
						  TCL_VOLATILE);
			return TCL_ERROR;

		default:
			sprintf(buf, "%d", spi_rc);
			Tcl_AppendResult(interp, "plperl: SPI_exec() failed - ",
							 "unknown RC ", buf, NULL);
			return TCL_ERROR;
	}

	/************************************************************
	 * Only SELECT queries fall through to here - remember the
	 * tuples we got
	 ************************************************************/

	ntuples = SPI_processed;
	if (ntuples > 0)
	{
		tuples = SPI_tuptable->vals;
		tupdesc = SPI_tuptable->tupdesc;
	}

	/************************************************************
	 * Prepare to start a controlled return through all
	 * interpreter levels on transaction abort during
	 * the ouput conversions of the results
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		plperl_restart_in_progress = 1;
		Tcl_SetResult(interp, "Transaction abort", TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * If there is no loop body given, just set the variables
	 * from the first tuple (if any) and return the number of
	 * tuples selected
	 ************************************************************/
	if (loop_body >= argc)
	{
		if (ntuples > 0)
			plperl_set_tuple_values(interp, arrayname, 0, tuples[0], tupdesc);
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		sprintf(buf, "%d", ntuples);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
		return TCL_OK;
	}

	/************************************************************
	 * There is a loop body - process all tuples and evaluate
	 * the body on each
	 ************************************************************/
	for (i = 0; i < ntuples; i++)
	{
		plperl_set_tuple_values(interp, arrayname, i, tuples[i], tupdesc);

		loop_rc = Tcl_Eval(interp, argv[loop_body]);

		if (loop_rc == TCL_OK)
			continue;
		if (loop_rc == TCL_CONTINUE)
			continue;
		if (loop_rc == TCL_RETURN)
		{
			memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
			return TCL_RETURN;
		}
		if (loop_rc == TCL_BREAK)
			break;
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		return TCL_ERROR;
	}

	/************************************************************
	 * Finally return the number of tuples
	 ************************************************************/
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	sprintf(buf, "%d", ntuples);
	Tcl_SetResult(interp, buf, TCL_VOLATILE);
	return TCL_OK;
}


/**********************************************************************
 * plperl_set_tuple_values() - Set variables for all attributes
 *				  of a given tuple
 **********************************************************************/
static void
plperl_set_tuple_values(Tcl_Interp *interp, char *arrayname,
						int tupno, HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	char	   *outputstr;
	char		buf[64];
	Datum		attr;
	bool		isnull;

	char	   *attname;
	HeapTuple	typeTup;
	Oid			typoutput;
	Oid			typelem;

	char	  **arrptr;
	char	  **nameptr;
	char	   *nullname = NULL;

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
		sprintf(buf, "%d", tupno);
		Tcl_SetVar2(interp, arrayname, ".tupno", buf, 0);
	}

	for (i = 0; i < tupdesc->natts; i++)
	{
		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		attname = tupdesc->attrs[i]->attname.data;

		/************************************************************
		 * Get the attributes value
		 ************************************************************/
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/************************************************************
		 * Lookup the attribute type in the syscache
		 * for the output function
		 ************************************************************/
		typeTup = SearchSysCacheTuple(TYPEOID,
						   ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
									  0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			elog(ERROR, "plperl: Cache lookup for attribute '%s' type %ld failed",
				 attname, ObjectIdGetDatum(tupdesc->attrs[i]->atttypid));
		}

		typoutput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typoutput);
		typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);

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
			FmgrInfo	finfo;

			fmgr_info(typoutput, &finfo);

			outputstr = (*fmgr_faddr(&finfo))
				(attr, typelem,
				 tupdesc->attrs[i]->attlen);

			Tcl_SetVar2(interp, *arrptr, *nameptr, outputstr, 0);
			pfree(outputstr);
		}
		else
			Tcl_UnsetVar2(interp, *arrptr, *nameptr, 0);
	}
}


#endif
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
	Oid			typelem;

	output = sv_2mortal(newSVpv("{", 0));

	for (i = 0; i < tupdesc->natts; i++)
	{
		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		attname = tupdesc->attrs[i]->attname.data;

		/************************************************************
		 * Get the attributes value
		 ************************************************************/
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/************************************************************
		 * Lookup the attribute type in the syscache
		 * for the output function
		 ************************************************************/
		typeTup = SearchSysCacheTuple(TYPEOID,
						   ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
									  0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			elog(ERROR, "plperl: Cache lookup for attribute '%s' type %ld failed",
				 attname, ObjectIdGetDatum(tupdesc->attrs[i]->atttypid));
		}

		typoutput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typoutput);
		typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);

		/************************************************************
		 * If there is a value, append the attribute name and the
		 * value to the list.
		 *	If it is null it will be set to undef.
		 ************************************************************/
		if (!isnull && OidIsValid(typoutput))
		{
			FmgrInfo	finfo;

			fmgr_info(typoutput, &finfo);

			outputstr = (*fmgr_faddr(&finfo))
				(attr, typelem,
				 tupdesc->attrs[i]->attlen);

			sv_catpvf(output, "'%s' => '%s',", attname, outputstr);
			pfree(outputstr);
		}
		else
			sv_catpvf(output, "'%s' => undef,", attname);
	}
	sv_catpv(output, "}");
	output = perl_eval_pv(SvPV(output, na), TRUE);
	return output;
}
