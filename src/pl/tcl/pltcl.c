/**********************************************************************
 * pltcl.c		- PostgreSQL support for Tcl as
 *				  procedural language (PL)
 *
 *	  This software is copyrighted by Jan Wieck - Hamburg.
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
 *	  $Header: /cvsroot/pgsql/src/pl/tcl/pltcl.c,v 1.34 2001/03/22 04:01:42 momjian Exp $
 *
 **********************************************************************/

#include <tcl.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <setjmp.h>

#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "access/heapam.h"

#include "tcop/tcopprot.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_type.h"


/**********************************************************************
 * The information we cache about loaded procedures
 **********************************************************************/
typedef struct pltcl_proc_desc
{
	char	   *proname;
	bool		lanpltrusted;
	FmgrInfo	result_in_func;
	Oid			result_in_elem;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	Oid			arg_out_elem[FUNC_MAX_ARGS];
	int			arg_out_len[FUNC_MAX_ARGS];
	int			arg_is_rel[FUNC_MAX_ARGS];
}			pltcl_proc_desc;


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
	Oid		   *argtypelems;
	Datum	   *argvalues;
	int		   *arglen;
}			pltcl_query_desc;


/**********************************************************************
 * Global data
 **********************************************************************/
static int	pltcl_firstcall = 1;
static int	pltcl_call_level = 0;
static int	pltcl_restart_in_progress = 0;
static Tcl_Interp *pltcl_hold_interp = NULL;
static Tcl_Interp *pltcl_norm_interp = NULL;
static Tcl_Interp *pltcl_safe_interp = NULL;
static Tcl_HashTable *pltcl_proc_hash = NULL;
static Tcl_HashTable *pltcl_norm_query_hash = NULL;
static Tcl_HashTable *pltcl_safe_query_hash = NULL;
static FunctionCallInfo pltcl_current_fcinfo = NULL;

/**********************************************************************
 * Forward declarations
 **********************************************************************/
static void pltcl_init_all(void);
static void pltcl_init_interp(Tcl_Interp *interp);

#ifdef PLTCL_UNKNOWN_SUPPORT
static void pltcl_init_load_unknown(Tcl_Interp *interp);

#endif	 /* PLTCL_UNKNOWN_SUPPORT */

Datum		pltcl_call_handler(PG_FUNCTION_ARGS);
Datum		pltclu_call_handler(PG_FUNCTION_ARGS);

static Datum pltcl_func_handler(PG_FUNCTION_ARGS);

static HeapTuple pltcl_trigger_handler(PG_FUNCTION_ARGS);

static int pltcl_elog(ClientData cdata, Tcl_Interp *interp,
		   int argc, char *argv[]);
static int pltcl_quote(ClientData cdata, Tcl_Interp *interp,
			int argc, char *argv[]);
static int pltcl_argisnull(ClientData cdata, Tcl_Interp *interp,
				int argc, char *argv[]);
static int pltcl_returnnull(ClientData cdata, Tcl_Interp *interp,
				 int argc, char *argv[]);

static int pltcl_SPI_exec(ClientData cdata, Tcl_Interp *interp,
			   int argc, char *argv[]);
static int pltcl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				  int argc, char *argv[]);
static int pltcl_SPI_execp(ClientData cdata, Tcl_Interp *interp,
				int argc, char *argv[]);

static void pltcl_set_tuple_values(Tcl_Interp *interp, char *arrayname,
					   int tupno, HeapTuple tuple, TupleDesc tupdesc);
static void pltcl_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc,
						   Tcl_DString *retval);

/**********************************************************************
 * pltcl_init_all()		- Initialize all
 **********************************************************************/
static void
pltcl_init_all(void)
{
	Tcl_HashEntry *hashent;
	Tcl_HashSearch hashsearch;
	pltcl_proc_desc *prodesc;
	pltcl_query_desc *querydesc;

	/************************************************************
	 * Do initialization only once
	 ************************************************************/
	if (!pltcl_firstcall)
		return;

	/************************************************************
	 * Create the dummy hold interpreter to prevent close of
	 * stdout and stderr on DeleteInterp
	 ************************************************************/
	if ((pltcl_hold_interp = Tcl_CreateInterp()) == NULL)
	{
		elog(ERROR, "pltcl: internal error - cannot create 'hold' "
			 "interpreter");
	}

	/************************************************************
	 * Create the two interpreters
	 ************************************************************/
	if ((pltcl_norm_interp =
		 Tcl_CreateSlave(pltcl_hold_interp, "norm", 0)) == NULL)
	{
		elog(ERROR,
		   "pltcl: internal error - cannot create 'normal' interpreter");
	}
	pltcl_init_interp(pltcl_norm_interp);

	if ((pltcl_safe_interp =
		 Tcl_CreateSlave(pltcl_hold_interp, "safe", 1)) == NULL)
	{
		elog(ERROR,
			 "pltcl: internal error - cannot create 'safe' interpreter");
	}
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

	pltcl_firstcall = 0;
	return;
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
					  pltcl_SPI_exec, NULL, NULL);
	Tcl_CreateCommand(interp, "spi_prepare",
					  pltcl_SPI_prepare, NULL, NULL);
	Tcl_CreateCommand(interp, "spi_execp",
					  pltcl_SPI_execp, NULL, NULL);

#ifdef PLTCL_UNKNOWN_SUPPORT
	/************************************************************
	 * Try to load the unknown procedure from pltcl_modules
	 ************************************************************/
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pltcl_init_interp(): SPI_connect failed");
	pltcl_init_load_unknown(interp);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "pltcl_init_interp(): SPI_finish failed");
#endif	 /* PLTCL_UNKNOWN_SUPPORT */
}


#ifdef PLTCL_UNKNOWN_SUPPORT

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
	spi_rc = SPI_exec("select 1 from pg_class "
					  "where relname = 'pltcl_modules'", 1);
	if (spi_rc != SPI_OK_SELECT)
		elog(ERROR, "pltcl_init_load_unknown(): select from pg_class failed");
	if (SPI_processed == 0)
		return;

	/************************************************************
	 * Read all the row's from it where modname = 'unknown' in
	 * the order of modseq
	 ************************************************************/
	Tcl_DStringInit(&unknown_src);

	spi_rc = SPI_exec("select modseq, modsrc from pltcl_modules "
					  "where modname = 'unknown' "
					  "order by modseq", 0);
	if (spi_rc != SPI_OK_SELECT)
	{
		elog(ERROR, "pltcl_init_load_unknown(): select from pltcl_modules "
			 "failed");
	}

	/************************************************************
	 * If there's nothing, module unknown doesn't exist
	 ************************************************************/
	if (SPI_processed == 0)
	{
		Tcl_DStringFree(&unknown_src);
		elog(NOTICE, "pltcl: Module unknown not found in pltcl_modules");
		return;
	}

	/************************************************************
	 * There is a module named unknown. Resemble the
	 * source from the modsrc attributes and evaluate
	 * it in the safe interpreter
	 ************************************************************/
	fno = SPI_fnumber(SPI_tuptable->tupdesc, "modsrc");

	for (i = 0; i < SPI_processed; i++)
	{
		part = SPI_getvalue(SPI_tuptable->vals[i],
							SPI_tuptable->tupdesc, fno);
		if (part != NULL)
		{
			Tcl_DStringAppend(&unknown_src, part, -1);
			pfree(part);
		}
	}
	tcl_rc = Tcl_GlobalEval(interp, Tcl_DStringValue(&unknown_src));
	Tcl_DStringFree(&unknown_src);
}

#endif	 /* PLTCL_UNKNOWN_SUPPORT */


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

	/************************************************************
	 * Initialize interpreters on first call
	 ************************************************************/
	if (pltcl_firstcall)
		pltcl_init_all();

	/************************************************************
	 * Connect to SPI manager
	 ************************************************************/
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pltcl: cannot connect to SPI manager");
	/************************************************************
	 * Keep track about the nesting of Tcl-SPI-Tcl-... calls
	 ************************************************************/
	pltcl_call_level++;

	/************************************************************
	 * Determine if called as function or trigger and
	 * call appropriate subhandler
	 ************************************************************/
	save_fcinfo = pltcl_current_fcinfo;

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

	pltcl_current_fcinfo = save_fcinfo;

	pltcl_call_level--;

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
	Tcl_Interp *interp;
	int			i;
	char		internal_proname[512];
	Tcl_HashEntry *hashent;
	int			hashnew;
	pltcl_proc_desc *volatile prodesc;
	Tcl_DString tcl_cmd;
	Tcl_DString list_tmp;
	int			tcl_rc;
	Datum		retval;
	sigjmp_buf	save_restart;

	/************************************************************
	 * Build our internal proc name from the functions Oid
	 ************************************************************/
	sprintf(internal_proname, "__PLTcl_proc_%u", fcinfo->flinfo->fn_oid);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	hashent = Tcl_FindHashEntry(pltcl_proc_hash, internal_proname);
	if (hashent == NULL)
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
		HeapTuple	langTup;
		HeapTuple	typeTup;
		Form_pg_proc procStruct;
		Form_pg_language langStruct;
		Form_pg_type typeStruct;
		Tcl_DString proc_internal_def;
		Tcl_DString proc_internal_body;
		char		proc_internal_args[4096];
		char	   *proc_source;
		char		buf[512];

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (pltcl_proc_desc *) malloc(sizeof(pltcl_proc_desc));
		prodesc->proname = malloc(strlen(internal_proname) + 1);
		strcpy(prodesc->proname, internal_proname);

		/************************************************************
		 * Lookup the pg_proc tuple by Oid
		 ************************************************************/
		procTup = SearchSysCache(PROCOID,
								 ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
								 0, 0, 0);
		if (!HeapTupleIsValid(procTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "pltcl: cache lookup for proc %u failed",
				 fcinfo->flinfo->fn_oid);
		}
		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

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
			elog(ERROR, "pltcl: cache lookup for language %u failed",
				 procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);

		prodesc->lanpltrusted = langStruct->lanpltrusted;
		if (prodesc->lanpltrusted)
			interp = pltcl_safe_interp;
		else
			interp = pltcl_norm_interp;
		ReleaseSysCache(langTup);

		/************************************************************
		 * Get the required information for input conversion of the
		 * return value.
		 ************************************************************/
		typeTup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(procStruct->prorettype),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			free(prodesc->proname);
			free(prodesc);
			if (!OidIsValid(procStruct->prorettype))
				elog(ERROR, "pltcl functions cannot return type \"opaque\""
					 "\n\texcept when used as triggers");
			else
				elog(ERROR, "pltcl: cache lookup for return type %u failed",
					 procStruct->prorettype);
		}
		typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

		if (typeStruct->typrelid != InvalidOid)
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "pltcl: return types of tuples not supported yet");
		}

		fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
		prodesc->result_in_elem = typeStruct->typelem;

		ReleaseSysCache(typeTup);

		/************************************************************
		 * Get the required information for output conversion
		 * of all procedure arguments
		 ************************************************************/
		prodesc->nargs = procStruct->pronargs;
		proc_internal_args[0] = '\0';
		for (i = 0; i < prodesc->nargs; i++)
		{
			typeTup = SearchSysCache(TYPEOID,
							ObjectIdGetDatum(procStruct->proargtypes[i]),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
			{
				free(prodesc->proname);
				free(prodesc);
				if (!OidIsValid(procStruct->proargtypes[i]))
					elog(ERROR, "pltcl functions cannot take type \"opaque\"");
				else
					elog(ERROR, "pltcl: cache lookup for argument type %u failed",
						 procStruct->proargtypes[i]);
			}
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			if (typeStruct->typrelid != InvalidOid)
			{
				prodesc->arg_is_rel[i] = 1;
				if (i > 0)
					strcat(proc_internal_args, " ");
				sprintf(buf, "__PLTcl_Tup_%d", i + 1);
				strcat(proc_internal_args, buf);
				ReleaseSysCache(typeTup);
				continue;
			}
			else
				prodesc->arg_is_rel[i] = 0;

			fmgr_info(typeStruct->typoutput, &(prodesc->arg_out_func[i]));
			prodesc->arg_out_elem[i] = (Oid) (typeStruct->typelem);
			prodesc->arg_out_len[i] = typeStruct->typlen;

			if (i > 0)
				strcat(proc_internal_args, " ");
			sprintf(buf, "%d", i + 1);
			strcat(proc_internal_args, buf);

			ReleaseSysCache(typeTup);
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
		 * and with appropriate upvars for tuple arguments
		 ************************************************************/
		Tcl_DStringAppend(&proc_internal_body, "upvar #0 ", -1);
		Tcl_DStringAppend(&proc_internal_body, internal_proname, -1);
		Tcl_DStringAppend(&proc_internal_body, " GD\n", -1);
		for (i = 0; i < fcinfo->nargs; i++)
		{
			if (!prodesc->arg_is_rel[i])
				continue;
			sprintf(buf, "array set %d $__PLTcl_Tup_%d\n", i + 1, i + 1);
			Tcl_DStringAppend(&proc_internal_body, buf, -1);
		}
		proc_source = DatumGetCString(DirectFunctionCall1(textout,
								  PointerGetDatum(&procStruct->prosrc)));
		Tcl_DStringAppend(&proc_internal_body, proc_source, -1);
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
			elog(ERROR, "pltcl: cannot create internal procedure %s - %s",
				 internal_proname, interp->result);
		}

		/************************************************************
		 * Add the proc description block to the hashtable
		 ************************************************************/
		hashent = Tcl_CreateHashEntry(pltcl_proc_hash,
									  prodesc->proname, &hashnew);
		Tcl_SetHashValue(hashent, (ClientData) prodesc);

		ReleaseSysCache(procTup);
	}
	else
	{
		/************************************************************
		 * Found the proc description block in the hashtable
		 ************************************************************/
		prodesc = (pltcl_proc_desc *) Tcl_GetHashValue(hashent);

		if (prodesc->lanpltrusted)
			interp = pltcl_safe_interp;
		else
			interp = pltcl_norm_interp;
	}

	/************************************************************
	 * Create the tcl command to call the internal
	 * proc in the safe interpreter
	 ************************************************************/
	Tcl_DStringInit(&tcl_cmd);
	Tcl_DStringInit(&list_tmp);
	Tcl_DStringAppendElement(&tcl_cmd, internal_proname);

	/************************************************************
	 * Catch elog(ERROR) during build of the Tcl command
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		Tcl_DStringFree(&tcl_cmd);
		Tcl_DStringFree(&list_tmp);
		pltcl_restart_in_progress = 1;
		if (--pltcl_call_level == 0)
			pltcl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	/************************************************************
	 * Add all call arguments to the command
	 ************************************************************/
	for (i = 0; i < prodesc->nargs; i++)
	{
		if (prodesc->arg_is_rel[i])
		{
			/**************************************************
			 * For tuple values, add a list for 'array set ...'
			 **************************************************/
			TupleTableSlot *slot = (TupleTableSlot *) fcinfo->arg[i];

			Assert(slot != NULL && !fcinfo->argnull[i]);
			Tcl_DStringInit(&list_tmp);
			pltcl_build_tuple_argument(slot->val,
									   slot->ttc_tupleDescriptor,
									   &list_tmp);
			Tcl_DStringAppendElement(&tcl_cmd, Tcl_DStringValue(&list_tmp));
			Tcl_DStringFree(&list_tmp);
			Tcl_DStringInit(&list_tmp);
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

				tmp = DatumGetCString(FunctionCall3(&prodesc->arg_out_func[i],
													fcinfo->arg[i],
							  ObjectIdGetDatum(prodesc->arg_out_elem[i]),
								Int32GetDatum(prodesc->arg_out_len[i])));
				Tcl_DStringAppendElement(&tcl_cmd, tmp);
				pfree(tmp);
			}
		}
	}
	Tcl_DStringFree(&list_tmp);
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

	/************************************************************
	 * Call the Tcl function
	 ************************************************************/
	tcl_rc = Tcl_GlobalEval(interp, Tcl_DStringValue(&tcl_cmd));
	Tcl_DStringFree(&tcl_cmd);

	/************************************************************
	 * Check the return code from Tcl and handle
	 * our special restart mechanism to get rid
	 * of all nested call levels on transaction
	 * abort.
	 ************************************************************/
	if (tcl_rc != TCL_OK || pltcl_restart_in_progress)
	{
		if (!pltcl_restart_in_progress)
		{
			pltcl_restart_in_progress = 1;
			if (--pltcl_call_level == 0)
				pltcl_restart_in_progress = 0;
			elog(ERROR, "pltcl: %s", interp->result);
		}
		if (--pltcl_call_level == 0)
			pltcl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	/************************************************************
	 * Convert the result value from the safe interpreter
	 * into its PostgreSQL data format and return it.
	 * Again, the function call could fire an elog and we
	 * have to count for the current interpreter level we are
	 * on. The save_restart from above is still good.
	 ************************************************************/
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		pltcl_restart_in_progress = 1;
		if (--pltcl_call_level == 0)
			pltcl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	/************************************************************
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).  But don't try to call
	 * the result_in_func if we've been told to return a NULL;
	 * the contents of interp->result may not be a valid value of
	 * the result type in that case.
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "pltcl: SPI_finish() failed");

	if (fcinfo->isnull)
		retval = (Datum) 0;
	else
		retval = FunctionCall3(&prodesc->result_in_func,
							   PointerGetDatum(interp->result),
							   ObjectIdGetDatum(prodesc->result_in_elem),
							   Int32GetDatum(-1));

	/************************************************************
	 * Finally we may restore normal error handling.
	 ************************************************************/
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

	return retval;
}


/**********************************************************************
 * pltcl_trigger_handler()	- Handler for trigger calls
 **********************************************************************/
static HeapTuple
pltcl_trigger_handler(PG_FUNCTION_ARGS)
{
	Tcl_Interp *interp;
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	char		internal_proname[512];
	char	   *stroid;
	Tcl_HashEntry *hashent;
	int			hashnew;
	pltcl_proc_desc *prodesc;
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
	char	  **ret_values;

	sigjmp_buf	save_restart;

	/************************************************************
	 * Build our internal proc name from the functions Oid
	 ************************************************************/
	sprintf(internal_proname, "__PLTcl_proc_%u", fcinfo->flinfo->fn_oid);

	/************************************************************
	 * Lookup the internal proc name in the hashtable
	 ************************************************************/
	hashent = Tcl_FindHashEntry(pltcl_proc_hash, internal_proname);
	if (hashent == NULL)
	{
		/************************************************************
		 * If we haven't found it in the hashtable,
		 * we load the procedure into the safe interpreter.
		 ************************************************************/
		Tcl_DString proc_internal_def;
		Tcl_DString proc_internal_body;
		HeapTuple	procTup;
		HeapTuple	langTup;
		Form_pg_proc procStruct;
		Form_pg_language langStruct;
		char	   *proc_source;

		/************************************************************
		 * Allocate a new procedure description block
		 ************************************************************/
		prodesc = (pltcl_proc_desc *) malloc(sizeof(pltcl_proc_desc));
		memset(prodesc, 0, sizeof(pltcl_proc_desc));
		prodesc->proname = malloc(strlen(internal_proname) + 1);
		strcpy(prodesc->proname, internal_proname);

		/************************************************************
		 * Lookup the pg_proc tuple by Oid
		 ************************************************************/
		procTup = SearchSysCache(PROCOID,
								 ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
								 0, 0, 0);
		if (!HeapTupleIsValid(procTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "pltcl: cache lookup for proc %u failed",
				 fcinfo->flinfo->fn_oid);
		}
		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

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
			elog(ERROR, "pltcl: cache lookup for language %u failed",
				 procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);

		prodesc->lanpltrusted = langStruct->lanpltrusted;
		if (prodesc->lanpltrusted)
			interp = pltcl_safe_interp;
		else
			interp = pltcl_norm_interp;
		ReleaseSysCache(langTup);

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

		proc_source = DatumGetCString(DirectFunctionCall1(textout,
								  PointerGetDatum(&procStruct->prosrc)));
		Tcl_DStringAppend(&proc_internal_body, proc_source, -1);
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
			elog(ERROR, "pltcl: cannot create internal procedure %s - %s",
				 internal_proname, interp->result);
		}

		/************************************************************
		 * Add the proc description block to the hashtable
		 ************************************************************/
		hashent = Tcl_CreateHashEntry(pltcl_proc_hash,
									  prodesc->proname, &hashnew);
		Tcl_SetHashValue(hashent, (ClientData) prodesc);

		ReleaseSysCache(procTup);
	}
	else
	{
		/************************************************************
		 * Found the proc description block in the hashtable
		 ************************************************************/
		prodesc = (pltcl_proc_desc *) Tcl_GetHashValue(hashent);

		if (prodesc->lanpltrusted)
			interp = pltcl_safe_interp;
		else
			interp = pltcl_norm_interp;
	}

	tupdesc = trigdata->tg_relation->rd_att;

	/************************************************************
	 * Create the tcl command to call the internal
	 * proc in the interpreter
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
		pltcl_restart_in_progress = 1;
		if (--pltcl_call_level == 0)
			pltcl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	/* The procedure name */
	Tcl_DStringAppendElement(&tcl_cmd, internal_proname);

	/* The trigger name for argument TG_name */
	Tcl_DStringAppendElement(&tcl_cmd, trigdata->tg_trigger->tgname);

	/* The oid of the trigger relation for argument TG_relid */
	stroid = DatumGetCString(DirectFunctionCall1(oidout,
						ObjectIdGetDatum(trigdata->tg_relation->rd_id)));
	Tcl_DStringAppendElement(&tcl_cmd, stroid);
	pfree(stroid);

	/* A list of attribute names for argument TG_relatts */
	Tcl_DStringAppendElement(&tcl_trigtup, "");
	for (i = 0; i < tupdesc->natts; i++)
		Tcl_DStringAppendElement(&tcl_trigtup,
								 NameStr(tupdesc->attrs[i]->attname));
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
	pltcl_build_tuple_argument(trigdata->tg_trigtuple,
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

		pltcl_build_tuple_argument(trigdata->tg_newtuple,
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
	tcl_rc = Tcl_GlobalEval(interp, Tcl_DStringValue(&tcl_cmd));
	Tcl_DStringFree(&tcl_cmd);

	/************************************************************
	 * Check the return code from Tcl and handle
	 * our special restart mechanism to get rid
	 * of all nested call levels on transaction
	 * abort.
	 ************************************************************/
	if (tcl_rc == TCL_ERROR || pltcl_restart_in_progress)
	{
		if (!pltcl_restart_in_progress)
		{
			pltcl_restart_in_progress = 1;
			if (--pltcl_call_level == 0)
				pltcl_restart_in_progress = 0;
			elog(ERROR, "pltcl: %s", interp->result);
		}
		if (--pltcl_call_level == 0)
			pltcl_restart_in_progress = 0;
		siglongjmp(Warn_restart, 1);
	}

	switch (tcl_rc)
	{
		case TCL_OK:
			break;

		default:
			elog(ERROR, "pltcl: unsupported TCL return code %d", tcl_rc);
	}

	/************************************************************
	 * The return value from the procedure might be one of
	 * the magic strings OK or SKIP or a list from array get
	 ************************************************************/
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "pltcl: SPI_finish() failed");

	if (strcmp(interp->result, "OK") == 0)
		return rettup;
	if (strcmp(interp->result, "SKIP") == 0)
	{
		return (HeapTuple) NULL;;
	}

	/************************************************************
	 * Convert the result value from the safe interpreter
	 * and setup structures for SPI_modifytuple();
	 ************************************************************/
	if (Tcl_SplitList(interp, interp->result,
					  &ret_numvals, &ret_values) != TCL_OK)
	{
		elog(NOTICE, "pltcl: cannot split return value from trigger");
		elog(ERROR, "pltcl: %s", interp->result);
	}

	if (ret_numvals % 2 != 0)
	{
		ckfree((char *) ret_values);
		elog(ERROR, "pltcl: invalid return list from trigger - must have even # of elements");
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
		ckfree((char *) ret_values);
		pltcl_restart_in_progress = 1;
		if (--pltcl_call_level == 0)
			pltcl_restart_in_progress = 0;
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
			elog(ERROR, "pltcl: invalid attribute '%s'", ret_values[--i]);

		/************************************************************
		 * Lookup the attribute type in the syscache
		 * for the input function
		 ************************************************************/
		typeTup = SearchSysCache(TYPEOID,
				  ObjectIdGetDatum(tupdesc->attrs[attnum - 1]->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			elog(ERROR, "pltcl: Cache lookup for attribute '%s' type %u failed",
				 ret_values[--i],
				 tupdesc->attrs[attnum - 1]->atttypid);
		}
		typinput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typinput);
		typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);
		ReleaseSysCache(typeTup);

		/************************************************************
		 * Set the attribute to NOT NULL and convert the contents
		 ************************************************************/
		modnulls[attnum - 1] = ' ';
		fmgr_info(typinput, &finfo);
		modvalues[attnum - 1] =
			FunctionCall3(&finfo,
						  CStringGetDatum(ret_values[i++]),
						  ObjectIdGetDatum(typelem),
				   Int32GetDatum(tupdesc->attrs[attnum - 1]->atttypmod));
	}

	rettup = SPI_modifytuple(trigdata->tg_relation, rettup, tupdesc->natts,
							 modattrs, modvalues, modnulls);

	pfree(modattrs);
	pfree(modvalues);
	pfree(modnulls);

	if (rettup == NULL)
		elog(ERROR, "pltcl: SPI_modifytuple() failed - RC = %d\n", SPI_result);

	ckfree((char *) ret_values);
	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));

	return rettup;
}


/**********************************************************************
 * pltcl_elog()		- elog() support for PLTcl
 **********************************************************************/
static int
pltcl_elog(ClientData cdata, Tcl_Interp *interp,
		   int argc, char *argv[])
{
	int			level;
	sigjmp_buf	save_restart;

	/************************************************************
	 * Suppress messages during the restart process
	 ************************************************************/
	if (pltcl_restart_in_progress)
		return TCL_ERROR;

	/************************************************************
	 * Catch the restart longjmp and begin a controlled
	 * return though all interpreter levels if it happens
	 ************************************************************/
	memcpy(&save_restart, &Warn_restart, sizeof(save_restart));
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		pltcl_restart_in_progress = 1;
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
 * pltcl_quote()	- quote literal strings that are to
 *			  be used in SPI_exec query strings
 **********************************************************************/
static int
pltcl_quote(ClientData cdata, Tcl_Interp *interp,
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
 * pltcl_argisnull()	- determine if a specific argument is NULL
 **********************************************************************/
static int
pltcl_argisnull(ClientData cdata, Tcl_Interp *interp,
				int argc, char *argv[])
{
	int			argno;
	FunctionCallInfo fcinfo = pltcl_current_fcinfo;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (argc != 2)
	{
		Tcl_SetResult(interp, "syntax error - 'argisnull argno'", TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * Check that we're called as a normal function
	 ************************************************************/
	if (fcinfo == NULL)
	{
		Tcl_SetResult(interp, "argisnull cannot be used in triggers",
					  TCL_VOLATILE);
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
		Tcl_SetResult(interp, "argno out of range", TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * Get the requested NULL state
	 ************************************************************/
	if (PG_ARGISNULL(argno))
		Tcl_SetResult(interp, "1", TCL_VOLATILE);
	else
		Tcl_SetResult(interp, "0", TCL_VOLATILE);

	return TCL_OK;
}


/**********************************************************************
 * pltcl_returnnull()	- Cause a NULL return from a function
 **********************************************************************/
static int
pltcl_returnnull(ClientData cdata, Tcl_Interp *interp,
				 int argc, char *argv[])
{
	int			argno;
	FunctionCallInfo fcinfo = pltcl_current_fcinfo;

	/************************************************************
	 * Check call syntax
	 ************************************************************/
	if (argc != 1)
	{
		Tcl_SetResult(interp, "syntax error - 'return_null'", TCL_VOLATILE);
		return TCL_ERROR;
	}

	/************************************************************
	 * Check that we're called as a normal function
	 ************************************************************/
	if (fcinfo == NULL)
	{
		Tcl_SetResult(interp, "return_null cannot be used in triggers",
					  TCL_VOLATILE);
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
 * pltcl_SPI_exec()		- The builtin SPI_exec command
 *				  for the safe interpreter
 **********************************************************************/
static int
pltcl_SPI_exec(ClientData cdata, Tcl_Interp *interp,
			   int argc, char *argv[])
{
	int			spi_rc;
	char		buf[64];
	int			count = 0;
	char	   *volatile arrayname = NULL;
	volatile int query_idx;
	int			i;
	int			loop_rc;
	int			ntuples;
	HeapTuple  *volatile tuples;
	volatile TupleDesc tupdesc = NULL;
	sigjmp_buf	save_restart;

	char	   *usage = "syntax error - 'SPI_exec "
	"?-count n? "
	"?-array name? query ?loop body?";

	/************************************************************
	 * Don't do anything if we are already in restart mode
	 ************************************************************/
	if (pltcl_restart_in_progress)
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
		pltcl_restart_in_progress = 1;
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
						  "pltcl: SPI_exec() failed - SPI_ERROR_ARGUMENT",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_UNCONNECTED:
			Tcl_SetResult(interp,
					  "pltcl: SPI_exec() failed - SPI_ERROR_UNCONNECTED",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_COPY:
			Tcl_SetResult(interp,
						  "pltcl: SPI_exec() failed - SPI_ERROR_COPY",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_CURSOR:
			Tcl_SetResult(interp,
						  "pltcl: SPI_exec() failed - SPI_ERROR_CURSOR",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_TRANSACTION:
			Tcl_SetResult(interp,
					  "pltcl: SPI_exec() failed - SPI_ERROR_TRANSACTION",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_OPUNKNOWN:
			Tcl_SetResult(interp,
						"pltcl: SPI_exec() failed - SPI_ERROR_OPUNKNOWN",
						  TCL_VOLATILE);
			return TCL_ERROR;

		default:
			sprintf(buf, "%d", spi_rc);
			Tcl_AppendResult(interp, "pltcl: SPI_exec() failed - ",
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
		pltcl_restart_in_progress = 1;
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
			pltcl_set_tuple_values(interp, arrayname, 0, tuples[0], tupdesc);
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
		pltcl_set_tuple_values(interp, arrayname, i, tuples[i], tupdesc);

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
 * pltcl_SPI_prepare()		- Builtin support for prepared plans
 *				  The Tcl command SPI_prepare
 *				  allways saves the plan using
 *				  SPI_saveplan and returns a key for
 *				  access. There is no chance to prepare
 *				  and not save the plan currently.
 **********************************************************************/
static int
pltcl_SPI_prepare(ClientData cdata, Tcl_Interp *interp,
				  int argc, char *argv[])
{
	int			nargs;
	char	  **args;
	pltcl_query_desc *qdesc;
	void	   *plan;
	int			i;
	HeapTuple	typeTup;
	Tcl_HashEntry *hashent;
	int			hashnew;
	sigjmp_buf	save_restart;
	Tcl_HashTable *query_hash;

	/************************************************************
	 * Don't do anything if we are already in restart mode
	 ************************************************************/
	if (pltcl_restart_in_progress)
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
	qdesc = (pltcl_query_desc *) malloc(sizeof(pltcl_query_desc));
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
		pltcl_restart_in_progress = 1;
		free(qdesc->argtypes);
		free(qdesc->arginfuncs);
		free(qdesc->argtypelems);
		free(qdesc->argvalues);
		free(qdesc->arglen);
		free(qdesc);
		ckfree((char *) args);
		return TCL_ERROR;
	}

	/************************************************************
	 * Lookup the argument types by name in the system cache
	 * and remember the required information for input conversion
	 ************************************************************/
	for (i = 0; i < nargs; i++)
	{
		typeTup = SearchSysCache(TYPENAME,
								 PointerGetDatum(args[i]),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "pltcl: Cache lookup of type %s failed", args[i]);
		qdesc->argtypes[i] = typeTup->t_data->t_oid;
		fmgr_info(((Form_pg_type) GETSTRUCT(typeTup))->typinput,
				  &(qdesc->arginfuncs[i]));
		qdesc->argtypelems[i] = ((Form_pg_type) GETSTRUCT(typeTup))->typelem;
		qdesc->argvalues[i] = (Datum) NULL;
		qdesc->arglen[i] = (int) (((Form_pg_type) GETSTRUCT(typeTup))->typlen);
		ReleaseSysCache(typeTup);
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

		elog(ERROR, "pltcl: SPI_prepare() failed - %s", reason);
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

		elog(ERROR, "pltcl: SPI_saveplan() failed - %s", reason);
	}

	/************************************************************
	 * Insert a hashtable entry for the plan and return
	 * the key to the caller
	 ************************************************************/
	if (interp == pltcl_norm_interp)
		query_hash = pltcl_norm_query_hash;
	else
		query_hash = pltcl_safe_query_hash;

	memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
	hashent = Tcl_CreateHashEntry(query_hash, qdesc->qname, &hashnew);
	Tcl_SetHashValue(hashent, (ClientData) qdesc);

	Tcl_SetResult(interp, qdesc->qname, TCL_VOLATILE);
	return TCL_OK;
}


/**********************************************************************
 * pltcl_SPI_execp()		- Execute a prepared plan
 **********************************************************************/
static int
pltcl_SPI_execp(ClientData cdata, Tcl_Interp *interp,
				int argc, char *argv[])
{
	int			spi_rc;
	char		buf[64];
	volatile int i;
	int			j;
	int			loop_body;
	Tcl_HashEntry *hashent;
	pltcl_query_desc *qdesc;
	char	   *volatile nulls = NULL;
	char	   *volatile arrayname = NULL;
	int			count = 0;
	int			callnargs;
	static char **callargs = NULL;
	int			loop_rc;
	int			ntuples;
	HeapTuple  *volatile tuples = NULL;
	volatile TupleDesc tupdesc = NULL;
	sigjmp_buf	save_restart;
	Tcl_HashTable *query_hash;

	char	   *usage = "syntax error - 'SPI_execp "
	"?-nulls string? ?-count n? "
	"?-array name? query ?args? ?loop body?";

	/************************************************************
	 * Tidy up from an earlier abort
	 ************************************************************/
	if (callargs != NULL)
	{
		ckfree((char *) callargs);
		callargs = NULL;
	}

	/************************************************************
	 * Don't do anything if we are already in restart mode
	 ************************************************************/
	if (pltcl_restart_in_progress)
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
	 * Get the prepared plan descriptor by its key
	 ************************************************************/
	if (interp == pltcl_norm_interp)
		query_hash = pltcl_norm_query_hash;
	else
		query_hash = pltcl_safe_query_hash;

	hashent = Tcl_FindHashEntry(query_hash, argv[i++]);
	if (hashent == NULL)
	{
		Tcl_AppendResult(interp, "invalid queryid '", argv[--i], "'", NULL);
		return TCL_ERROR;
	}
	qdesc = (pltcl_query_desc *) Tcl_GetHashValue(hashent);

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
				ckfree((char *) callargs);
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
			ckfree((char *) callargs);
			callargs = NULL;
			pltcl_restart_in_progress = 1;
			Tcl_SetResult(interp, "Transaction abort", TCL_VOLATILE);
			return TCL_ERROR;
		}

		/************************************************************
		 * Setup the value array for the SPI_execp() using
		 * the type specific input functions
		 ************************************************************/
		for (j = 0; j < callnargs; j++)
		{
			qdesc->argvalues[j] =
				FunctionCall3(&qdesc->arginfuncs[j],
							  CStringGetDatum(callargs[j]),
							  ObjectIdGetDatum(qdesc->argtypelems[j]),
							  Int32GetDatum(qdesc->arglen[j]));
		}

		/************************************************************
		 * Free the splitted argument value list
		 ************************************************************/
		memcpy(&Warn_restart, &save_restart, sizeof(Warn_restart));
		ckfree((char *) callargs);
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
		pltcl_restart_in_progress = 1;
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
						  "pltcl: SPI_exec() failed - SPI_ERROR_ARGUMENT",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_UNCONNECTED:
			Tcl_SetResult(interp,
					  "pltcl: SPI_exec() failed - SPI_ERROR_UNCONNECTED",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_COPY:
			Tcl_SetResult(interp,
						  "pltcl: SPI_exec() failed - SPI_ERROR_COPY",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_CURSOR:
			Tcl_SetResult(interp,
						  "pltcl: SPI_exec() failed - SPI_ERROR_CURSOR",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_TRANSACTION:
			Tcl_SetResult(interp,
					  "pltcl: SPI_exec() failed - SPI_ERROR_TRANSACTION",
						  TCL_VOLATILE);
			return TCL_ERROR;

		case SPI_ERROR_OPUNKNOWN:
			Tcl_SetResult(interp,
						"pltcl: SPI_exec() failed - SPI_ERROR_OPUNKNOWN",
						  TCL_VOLATILE);
			return TCL_ERROR;

		default:
			sprintf(buf, "%d", spi_rc);
			Tcl_AppendResult(interp, "pltcl: SPI_exec() failed - ",
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
		pltcl_restart_in_progress = 1;
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
			pltcl_set_tuple_values(interp, arrayname, 0, tuples[0], tupdesc);
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
		pltcl_set_tuple_values(interp, arrayname, i, tuples[i], tupdesc);

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
 * pltcl_set_tuple_values() - Set variables for all attributes
 *				  of a given tuple
 **********************************************************************/
static void
pltcl_set_tuple_values(Tcl_Interp *interp, char *arrayname,
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
		{
			elog(ERROR, "pltcl: Cache lookup for attribute '%s' type %u failed",
				 attname, tupdesc->attrs[i]->atttypid);
		}

		typoutput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typoutput);
		typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);
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
			outputstr = DatumGetCString(OidFunctionCall3(typoutput,
														 attr,
											   ObjectIdGetDatum(typelem),
							  Int32GetDatum(tupdesc->attrs[i]->attlen)));
			Tcl_SetVar2(interp, *arrptr, *nameptr, outputstr, 0);
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
	Oid			typelem;

	for (i = 0; i < tupdesc->natts; i++)
	{
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
		{
			elog(ERROR, "pltcl: Cache lookup for attribute '%s' type %u failed",
				 attname, tupdesc->attrs[i]->atttypid);
		}

		typoutput = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typoutput);
		typelem = (Oid) (((Form_pg_type) GETSTRUCT(typeTup))->typelem);
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
			outputstr = DatumGetCString(OidFunctionCall3(typoutput,
														 attr,
											   ObjectIdGetDatum(typelem),
							  Int32GetDatum(tupdesc->attrs[i]->attlen)));
			Tcl_DStringAppendElement(retval, attname);
			Tcl_DStringAppendElement(retval, outputstr);
			pfree(outputstr);
		}
	}
}
