/**********************************************************************
 * pl_comp.c		- Compiler part of the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/pl/plpgsql/src/pl_comp.c,v 1.78 2004/07/31 00:45:46 tgl Exp $
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
 **********************************************************************/

#include "plpgsql.h"

#include <ctype.h>

#include "pl.tab.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "parser/gramparse.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* ----------
 * Variables in the parser that shouldn't go into plpgsql.h
 * ----------
 */
extern PLPGSQL_YYSTYPE plpgsql_yylval;

/* ----------
 * Our own local and global variables
 * ----------
 */
static int	datums_alloc;
int			plpgsql_nDatums;
PLpgSQL_datum **plpgsql_Datums;
static int	datums_last = 0;

int			plpgsql_error_lineno;
char	   *plpgsql_error_funcname;
int			plpgsql_DumpExecTree = 0;

PLpgSQL_function *plpgsql_curr_compile;

/* ----------
 * Hash table for compiled functions
 * ----------
 */
static HTAB *plpgsql_HashTable = NULL;

typedef struct plpgsql_hashent
{
	PLpgSQL_func_hashkey key;
	PLpgSQL_function *function;
}	plpgsql_HashEnt;

#define FUNCS_PER_USER		128 /* initial table size */


/* ----------
 * static prototypes
 * ----------
 */
static PLpgSQL_function *do_compile(FunctionCallInfo fcinfo,
		   HeapTuple procTup,
		   PLpgSQL_func_hashkey *hashkey,
		   bool forValidator);
static void plpgsql_compile_error_callback(void *arg);
static char **fetchArgNames(HeapTuple procTup, int nargs);
static PLpgSQL_row *build_row_var(Oid classOid);
static PLpgSQL_type *build_datatype(HeapTuple typeTup, int32 typmod);
static void compute_function_hashkey(FunctionCallInfo fcinfo,
						 Form_pg_proc procStruct,
						 PLpgSQL_func_hashkey *hashkey,
						 bool forValidator);
static PLpgSQL_function *plpgsql_HashTableLookup(PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableInsert(PLpgSQL_function *function,
						PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableDelete(PLpgSQL_function *function);

/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the compiled form of a plpgsql function is allocated permanently
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


/* ----------
 * plpgsql_compile		Make an execution tree for a PL/pgSQL function.
 *
 * If forValidator is true, we're only compiling for validation purposes,
 * and so some checks are skipped.
 *
 * Note: it's important for this to fall through quickly if the function
 * has already been compiled.
 * ----------
 */
PLpgSQL_function *
plpgsql_compile(FunctionCallInfo fcinfo, bool forValidator)
{
	Oid			funcOid = fcinfo->flinfo->fn_oid;
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	PLpgSQL_function *function;
	PLpgSQL_func_hashkey hashkey;
	bool		hashkey_valid = false;

	/*
	 * Lookup the pg_proc tuple by Oid; we'll need it in any case
	 */
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(funcOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", funcOid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * See if there's already a cache entry for the current FmgrInfo. If
	 * not, try to find one in the hash table.
	 */
	function = (PLpgSQL_function *) fcinfo->flinfo->fn_extra;

	if (!function)
	{
		/* First time through in this backend?	If so, init hashtable */
		if (!plpgsql_HashTable)
			plpgsql_HashTableInit();

		/* Compute hashkey using function signature and actual arg types */
		compute_function_hashkey(fcinfo, procStruct, &hashkey, forValidator);
		hashkey_valid = true;

		/* And do the lookup */
		function = plpgsql_HashTableLookup(&hashkey);
	}

	if (function)
	{
		/* We have a compiled function, but is it still valid? */
		if (!(function->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
		   function->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data)))
		{
			/*
			 * Nope, drop the hashtable entry.	XXX someday, free all the
			 * subsidiary storage as well.
			 */
			plpgsql_HashTableDelete(function);

			function = NULL;
		}
	}

	/*
	 * If the function wasn't found or was out-of-date, we have to compile
	 * it
	 */
	if (!function)
	{
		/*
		 * Calculate hashkey if we didn't already; we'll need it to store
		 * the completed function.
		 */
		if (!hashkey_valid)
			compute_function_hashkey(fcinfo, procStruct, &hashkey,
									 forValidator);

		/*
		 * Do the hard part.
		 */
		function = do_compile(fcinfo, procTup, &hashkey, forValidator);
	}

	ReleaseSysCache(procTup);

	/*
	 * Save pointer in FmgrInfo to avoid search on subsequent calls
	 */
	fcinfo->flinfo->fn_extra = (void *) function;

	/*
	 * Finally return the compiled function
	 */
	return function;
}

/*
 * This is the slow part of plpgsql_compile().
 */
static PLpgSQL_function *
do_compile(FunctionCallInfo fcinfo,
		   HeapTuple procTup,
		   PLpgSQL_func_hashkey *hashkey,
		   bool forValidator)
{
	Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	int			functype = CALLED_AS_TRIGGER(fcinfo) ? T_TRIGGER : T_FUNCTION;
	PLpgSQL_function *function;
	Datum		prosrcdatum;
	bool		isnull;
	char	   *proc_source;
	HeapTuple	typeTup;
	Form_pg_type typeStruct;
	PLpgSQL_variable *var;
	PLpgSQL_rec *rec;
	int			i;
	int			arg_varnos[FUNC_MAX_ARGS];
	ErrorContextCallback plerrcontext;
	int			parse_rc;
	Oid			rettypeid;
	char	  **argnames;

	/*
	 * Setup the scanner input and error info.	We assume that this
	 * function cannot be invoked recursively, so there's no need to save
	 * and restore the static variables used here.
	 */
	prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");
	proc_source = DatumGetCString(DirectFunctionCall1(textout, prosrcdatum));
	plpgsql_scanner_init(proc_source, functype);

	plpgsql_error_funcname = pstrdup(NameStr(procStruct->proname));
	plpgsql_error_lineno = 0;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_compile_error_callback;
	plerrcontext.arg = forValidator ? proc_source : (char *) NULL;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * Initialize the compiler
	 */
	plpgsql_ns_init();
	plpgsql_ns_push(NULL);
	plpgsql_DumpExecTree = 0;

	datums_alloc = 128;
	plpgsql_nDatums = 0;
	plpgsql_Datums = palloc(sizeof(PLpgSQL_datum *) * datums_alloc);
	datums_last = 0;

	/*
	 * Create the new function node
	 */
	function = malloc(sizeof(PLpgSQL_function));
	MemSet(function, 0, sizeof(PLpgSQL_function));
	plpgsql_curr_compile = function;

	function->fn_name = strdup(NameStr(procStruct->proname));
	function->fn_oid = fcinfo->flinfo->fn_oid;
	function->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	function->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);
	function->fn_functype = functype;

	switch (functype)
	{
		case T_FUNCTION:

			/*
			 * Check for a polymorphic returntype. If found, use the
			 * actual returntype type from the caller's FuncExpr node, if
			 * we have one.  (In validation mode we arbitrarily assume we
			 * are dealing with integers.)
			 *
			 * Note: errcode is FEATURE_NOT_SUPPORTED because it should
			 * always work; if it doesn't we're in some context that fails
			 * to make the info available.
			 */
			rettypeid = procStruct->prorettype;
			if (rettypeid == ANYARRAYOID || rettypeid == ANYELEMENTOID)
			{
				if (forValidator)
				{
					if (rettypeid == ANYARRAYOID)
						rettypeid = INT4ARRAYOID;
					else
						rettypeid = INT4OID;
				}
				else
					rettypeid = get_fn_expr_rettype(fcinfo->flinfo);
				if (!OidIsValid(rettypeid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("could not determine actual return type "
								"for polymorphic function \"%s\"",
								plpgsql_error_funcname)));
			}

			/*
			 * Normal function has a defined returntype
			 */
			function->fn_rettype = rettypeid;
			function->fn_retset = procStruct->proretset;

			/*
			 * Lookup the functions return type
			 */
			typeTup = SearchSysCache(TYPEOID,
									 ObjectIdGetDatum(rettypeid),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "cache lookup failed for type %u", rettypeid);
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID or RECORD */
			/* (note we already replaced ANYARRAY/ANYELEMENT) */
			if (typeStruct->typtype == 'p')
			{
				if (rettypeid == VOIDOID ||
					rettypeid == RECORDOID)
					 /* okay */ ;
				else if (rettypeid == TRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions may only be called as triggers")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("plpgsql functions cannot return type %s",
							   format_type_be(rettypeid))));
			}

			if (typeStruct->typrelid != InvalidOid ||
				rettypeid == RECORDOID)
				function->fn_retistuple = true;
			else
			{
				function->fn_retbyval = typeStruct->typbyval;
				function->fn_rettyplen = typeStruct->typlen;
				function->fn_rettypioparam = getTypeIOParam(typeTup);
				perm_fmgr_info(typeStruct->typinput, &(function->fn_retinput));

				/*
				 * install $0 reference, but only for polymorphic return
				 * types
				 */
				if (procStruct->prorettype == ANYARRAYOID ||
					procStruct->prorettype == ANYELEMENTOID)
				{
					(void) plpgsql_build_variable(strdup("$0"), 0,
												  build_datatype(typeTup, -1),
												  true);
				}
			}
			ReleaseSysCache(typeTup);

			/*
			 * Create the variables for the procedure's parameters
			 */
			argnames = fetchArgNames(procTup, procStruct->pronargs);

			for (i = 0; i < procStruct->pronargs; i++)
			{
				char		buf[32];
				Oid			argtypeid;
				PLpgSQL_type *argdtype;
				PLpgSQL_variable *argvariable;
				int			argitemtype;

				/* Create $n name for variable */
				snprintf(buf, sizeof(buf), "$%d", i + 1);

				/*
				 * Since we already did the replacement of polymorphic
				 * argument types by actual argument types while computing
				 * the hashkey, we can just use those results.
				 */
				argtypeid = hashkey->argtypes[i];
				argdtype = plpgsql_build_datatype(argtypeid, -1);

				/* Disallow pseudotype argument */
				/* (note we already replaced ANYARRAY/ANYELEMENT) */
				/* (build_variable would do this, but wrong message) */
				if (argdtype->ttype != PLPGSQL_TTYPE_SCALAR &&
					argdtype->ttype != PLPGSQL_TTYPE_ROW)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("plpgsql functions cannot take type %s",
									format_type_be(argtypeid))));

				/* Build variable and add to datum list */
				argvariable = plpgsql_build_variable(strdup(buf), 0,
													 argdtype, false);

				if (argvariable->dtype == PLPGSQL_DTYPE_VAR)
				{
					/* argument vars are forced to be CONSTANT (why?) */
					((PLpgSQL_var *) argvariable)->isconst = true;
					argitemtype = PLPGSQL_NSTYPE_VAR;
				}
				else
				{
					Assert(argvariable->dtype == PLPGSQL_DTYPE_ROW);
					argitemtype = PLPGSQL_NSTYPE_ROW;
				}

				/* Remember datum number */
				arg_varnos[i] = argvariable->dno;

				/* Add to namespace under the $n name */
				plpgsql_ns_additem(argitemtype, argvariable->dno, buf);

				/* If there's a name for the argument, make an alias */
				if (argnames && argnames[i] && argnames[i][0])
					plpgsql_ns_additem(argitemtype, argvariable->dno,
									   argnames[i]);
			}
			break;

		case T_TRIGGER:

			/*
			 * Trigger procedures return type is unknown yet
			 */
			function->fn_rettype = InvalidOid;
			function->fn_retbyval = false;
			function->fn_retistuple = true;
			function->fn_retset = false;

			/*
			 * Add the record for referencing NEW
			 */
			rec = malloc(sizeof(PLpgSQL_rec));
			memset(rec, 0, sizeof(PLpgSQL_rec));
			rec->dtype = PLPGSQL_DTYPE_REC;
			rec->refname = strdup("new");
			rec->tup = NULL;
			rec->tupdesc = NULL;
			rec->freetup = false;
			plpgsql_adddatum((PLpgSQL_datum *) rec);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->recno, rec->refname);
			function->new_varno = rec->recno;

			/*
			 * Add the record for referencing OLD
			 */
			rec = malloc(sizeof(PLpgSQL_rec));
			memset(rec, 0, sizeof(PLpgSQL_rec));
			rec->dtype = PLPGSQL_DTYPE_REC;
			rec->refname = strdup("old");
			rec->tup = NULL;
			rec->tupdesc = NULL;
			rec->freetup = false;
			plpgsql_adddatum((PLpgSQL_datum *) rec);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->recno, rec->refname);
			function->old_varno = rec->recno;

			/*
			 * Add the variable tg_name
			 */
			var = plpgsql_build_variable(strdup("tg_name"), 0,
										 plpgsql_build_datatype(NAMEOID, -1),
										 true);
			function->tg_name_varno = var->dno;

			/*
			 * Add the variable tg_when
			 */
			var = plpgsql_build_variable(strdup("tg_when"), 0,
										 plpgsql_build_datatype(TEXTOID, -1),
										 true);
			function->tg_when_varno = var->dno;

			/*
			 * Add the variable tg_level
			 */
			var = plpgsql_build_variable(strdup("tg_level"), 0,
										 plpgsql_build_datatype(TEXTOID, -1),
										 true);
			function->tg_level_varno = var->dno;

			/*
			 * Add the variable tg_op
			 */
			var = plpgsql_build_variable(strdup("tg_op"), 0,
										 plpgsql_build_datatype(TEXTOID, -1),
										 true);
			function->tg_op_varno = var->dno;

			/*
			 * Add the variable tg_relid
			 */
			var = plpgsql_build_variable(strdup("tg_relid"), 0,
										 plpgsql_build_datatype(OIDOID, -1),
										 true);
			function->tg_relid_varno = var->dno;

			/*
			 * Add the variable tg_relname
			 */
			var = plpgsql_build_variable(strdup("tg_relname"), 0,
										 plpgsql_build_datatype(NAMEOID, -1),
										 true);
			function->tg_relname_varno = var->dno;

			/*
			 * Add the variable tg_nargs
			 */
			var = plpgsql_build_variable(strdup("tg_nargs"), 0,
										 plpgsql_build_datatype(INT4OID, -1),
										 true);
			function->tg_nargs_varno = var->dno;

			break;

		default:
			elog(ERROR, "unrecognized function typecode: %u", functype);
			break;
	}

	/*
	 * Create the magic FOUND variable.
	 */
	var = plpgsql_build_variable(strdup("found"), 0,
								 plpgsql_build_datatype(BOOLOID, -1),
								 true);
	function->found_varno = var->dno;

	/*
	 * Forget about the above created variables
	 */
	plpgsql_add_initdatums(NULL);

	/*
	 * Now parse the functions text
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql parser returned %d", parse_rc);

	plpgsql_scanner_finish();
	pfree(proc_source);

	/*
	 * If that was successful, complete the functions info.
	 */
	function->fn_nargs = procStruct->pronargs;
	for (i = 0; i < function->fn_nargs; i++)
		function->fn_argvarnos[i] = arg_varnos[i];
	function->ndatums = plpgsql_nDatums;
	function->datums = malloc(sizeof(PLpgSQL_datum *) * plpgsql_nDatums);
	for (i = 0; i < plpgsql_nDatums; i++)
		function->datums[i] = plpgsql_Datums[i];
	function->action = plpgsql_yylval.program;

	/* Debug dump for completed functions */
	if (plpgsql_DumpExecTree)
		plpgsql_dumptree(function);

	/*
	 * add it to the hash table
	 */
	plpgsql_HashTableInsert(function, hashkey);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;
	plpgsql_error_funcname = NULL;
	plpgsql_error_lineno = 0;

	return function;
}


/*
 * error context callback to let us supply a call-stack traceback
 *
 * If we are validating, the function source is passed as argument.
 */
static void
plpgsql_compile_error_callback(void *arg)
{
	if (arg)
	{
		/*
		 * Try to convert syntax error position to reference text of
		 * original CREATE FUNCTION command.
		 */
		if (function_parse_error_transpose((const char *) arg))
			return;
		/*
		 * Done if a syntax error position was reported; otherwise we
		 * have to fall back to a "near line N" report.
		 */
	}

	if (plpgsql_error_funcname)
		errcontext("compile of PL/pgSQL function \"%s\" near line %d",
				   plpgsql_error_funcname, plpgsql_error_lineno);
}


/*
 * Fetch the argument names, if any, from the proargnames field of the
 * pg_proc tuple.  Results are palloc'd.
 */
static char **
fetchArgNames(HeapTuple procTup, int nargs)
{
	Datum		argnamesDatum;
	bool		isNull;
	Datum	   *elems;
	int			nelems;
	char	  **result;
	int			i;

	if (nargs == 0)
		return NULL;

	argnamesDatum = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_proargnames,
									&isNull);
	if (isNull)
		return NULL;

	deconstruct_array(DatumGetArrayTypeP(argnamesDatum),
					  TEXTOID, -1, false, 'i',
					  &elems, &nelems);

	if (nelems != nargs)		/* should not happen */
		elog(ERROR, "proargnames must have the same number of elements as the function has arguments");

	result = (char **) palloc(sizeof(char *) * nargs);

	for (i=0; i < nargs; i++)
		result[i] = DatumGetCString(DirectFunctionCall1(textout, elems[i]));

	return result;
}


/* ----------
 * plpgsql_parse_word		The scanner calls this to postparse
 *				any single word not found by a
 *				keyword rule.
 * ----------
 */
int
plpgsql_parse_word(char *word)
{
	PLpgSQL_nsitem *nse;
	char	   *cp[1];

	/* Do case conversion and word separation */
	plpgsql_convert_ident(word, cp, 1);

	/*
	 * Recognize tg_argv when compiling triggers
	 */
	if (plpgsql_curr_compile->fn_functype == T_TRIGGER)
	{
		if (strcmp(cp[0], "tg_argv") == 0)
		{
			int			save_spacescanned = plpgsql_SpaceScanned;
			PLpgSQL_trigarg *trigarg;

			trigarg = malloc(sizeof(PLpgSQL_trigarg));
			memset(trigarg, 0, sizeof(PLpgSQL_trigarg));
			trigarg->dtype = PLPGSQL_DTYPE_TRIGARG;

			if (plpgsql_yylex() != '[')
				plpgsql_yyerror("expected \"[\"");

			trigarg->argnum = plpgsql_read_expression(']', "]");

			plpgsql_adddatum((PLpgSQL_datum *) trigarg);
			plpgsql_yylval.scalar = (PLpgSQL_datum *) trigarg;

			plpgsql_SpaceScanned = save_spacescanned;
			pfree(cp[0]);
			return T_SCALAR;
		}
	}

	/*
	 * Do a lookup on the compilers namestack
	 */
	nse = plpgsql_ns_lookup(cp[0], NULL);
	if (nse != NULL)
	{
		pfree(cp[0]);
		switch (nse->itemtype)
		{
			case PLPGSQL_NSTYPE_LABEL:
				return T_LABEL;

			case PLPGSQL_NSTYPE_VAR:
				plpgsql_yylval.scalar = plpgsql_Datums[nse->itemno];
				return T_SCALAR;

			case PLPGSQL_NSTYPE_REC:
				plpgsql_yylval.rec = (PLpgSQL_rec *) (plpgsql_Datums[nse->itemno]);
				return T_RECORD;

			case PLPGSQL_NSTYPE_ROW:
				plpgsql_yylval.row = (PLpgSQL_row *) (plpgsql_Datums[nse->itemno]);
				return T_ROW;

			default:
				return T_ERROR;
		}
	}

	/*
	 * Nothing found - up to now it's a word without any special meaning
	 * for us.
	 */
	pfree(cp[0]);
	return T_WORD;
}


/* ----------
 * plpgsql_parse_dblword		Same lookup for two words
 *					separated by a dot.
 * ----------
 */
int
plpgsql_parse_dblword(char *word)
{
	PLpgSQL_nsitem *ns;
	char	   *cp[2];

	/* Do case conversion and word separation */
	plpgsql_convert_ident(word, cp, 2);

	/*
	 * Lookup the first word
	 */
	ns = plpgsql_ns_lookup(cp[0], NULL);
	if (ns == NULL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	switch (ns->itemtype)
	{
		case PLPGSQL_NSTYPE_LABEL:

			/*
			 * First word is a label, so second word could be a variable,
			 * record or row in that bodies namestack. Anything else could
			 * only be something in a query given to the SPI manager and
			 * T_ERROR will get eaten up by the collector routines.
			 */
			ns = plpgsql_ns_lookup(cp[1], cp[0]);
			pfree(cp[0]);
			pfree(cp[1]);
			if (ns == NULL)
				return T_ERROR;
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_VAR:
					plpgsql_yylval.scalar = plpgsql_Datums[ns->itemno];
					return T_SCALAR;

				case PLPGSQL_NSTYPE_REC:
					plpgsql_yylval.rec = (PLpgSQL_rec *) (plpgsql_Datums[ns->itemno]);
					return T_RECORD;

				case PLPGSQL_NSTYPE_ROW:
					plpgsql_yylval.row = (PLpgSQL_row *) (plpgsql_Datums[ns->itemno]);
					return T_ROW;

				default:
					return T_ERROR;
			}
			break;

		case PLPGSQL_NSTYPE_REC:
			{
				/*
				 * First word is a record name, so second word must be a
				 * field in this record.
				 */
				PLpgSQL_recfield *new;

				new = malloc(sizeof(PLpgSQL_recfield));
				new->dtype = PLPGSQL_DTYPE_RECFIELD;
				new->fieldname = strdup(cp[1]);
				new->recparentno = ns->itemno;

				plpgsql_adddatum((PLpgSQL_datum *) new);

				plpgsql_yylval.scalar = (PLpgSQL_datum *) new;

				pfree(cp[0]);
				pfree(cp[1]);
				return T_SCALAR;
			}

		case PLPGSQL_NSTYPE_ROW:
			{
				/*
				 * First word is a row name, so second word must be a
				 * field in this row.
				 */
				PLpgSQL_row *row;
				int			i;

				row = (PLpgSQL_row *) (plpgsql_Datums[ns->itemno]);
				for (i = 0; i < row->nfields; i++)
				{
					if (row->fieldnames[i] &&
						strcmp(row->fieldnames[i], cp[1]) == 0)
					{
						plpgsql_yylval.scalar = plpgsql_Datums[row->varnos[i]];
						pfree(cp[0]);
						pfree(cp[1]);
						return T_SCALAR;
					}
				}
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("row \"%s\" has no field \"%s\"",
								cp[0], cp[1])));
			}

		default:
			break;
	}

	pfree(cp[0]);
	pfree(cp[1]);
	return T_ERROR;
}


/* ----------
 * plpgsql_parse_tripword		Same lookup for three words
 *					separated by dots.
 * ----------
 */
int
plpgsql_parse_tripword(char *word)
{
	PLpgSQL_nsitem *ns;
	char	   *cp[3];

	/* Do case conversion and word separation */
	plpgsql_convert_ident(word, cp, 3);

	/*
	 * Lookup the first word - it must be a label
	 */
	ns = plpgsql_ns_lookup(cp[0], NULL);
	if (ns == NULL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		pfree(cp[2]);
		return T_ERROR;
	}
	if (ns->itemtype != PLPGSQL_NSTYPE_LABEL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		pfree(cp[2]);
		return T_ERROR;
	}

	/*
	 * First word is a label, so second word could be a record or row
	 */
	ns = plpgsql_ns_lookup(cp[1], cp[0]);
	if (ns == NULL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		pfree(cp[2]);
		return T_ERROR;
	}

	switch (ns->itemtype)
	{
		case PLPGSQL_NSTYPE_REC:
			{
				/*
				 * This word is a record name, so third word must be a
				 * field in this record.
				 */
				PLpgSQL_recfield *new;

				new = malloc(sizeof(PLpgSQL_recfield));
				new->dtype = PLPGSQL_DTYPE_RECFIELD;
				new->fieldname = strdup(cp[2]);
				new->recparentno = ns->itemno;

				plpgsql_adddatum((PLpgSQL_datum *) new);

				plpgsql_yylval.scalar = (PLpgSQL_datum *) new;

				pfree(cp[0]);
				pfree(cp[1]);
				pfree(cp[2]);

				return T_SCALAR;
			}

		case PLPGSQL_NSTYPE_ROW:
			{
				/*
				 * This word is a row name, so third word must be a field
				 * in this row.
				 */
				PLpgSQL_row *row;
				int			i;

				row = (PLpgSQL_row *) (plpgsql_Datums[ns->itemno]);
				for (i = 0; i < row->nfields; i++)
				{
					if (row->fieldnames[i] &&
						strcmp(row->fieldnames[i], cp[2]) == 0)
					{
						plpgsql_yylval.scalar = plpgsql_Datums[row->varnos[i]];

						pfree(cp[0]);
						pfree(cp[1]);
						pfree(cp[2]);

						return T_SCALAR;
					}
				}
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("row \"%s.%s\" has no field \"%s\"",
								cp[0], cp[1], cp[2])));
			}

		default:
			break;
	}

	pfree(cp[0]);
	pfree(cp[1]);
	pfree(cp[2]);
	return T_ERROR;
}


/* ----------
 * plpgsql_parse_wordtype	The scanner found word%TYPE. word can be
 *				a variable name or a basetype.
 * ----------
 */
int
plpgsql_parse_wordtype(char *word)
{
	PLpgSQL_nsitem *nse;
	bool		old_nsstate;
	Oid			typeOid;
	char	   *cp[2];
	int			i;

	/* Do case conversion and word separation */
	/* We convert %type to .type momentarily to keep converter happy */
	i = strlen(word) - 5;
	Assert(word[i] == '%');
	word[i] = '.';
	plpgsql_convert_ident(word, cp, 2);
	word[i] = '%';
	pfree(cp[1]);

	/*
	 * Do a lookup on the compilers namestack. But ensure it moves up to
	 * the toplevel.
	 */
	old_nsstate = plpgsql_ns_setlocal(false);
	nse = plpgsql_ns_lookup(cp[0], NULL);
	plpgsql_ns_setlocal(old_nsstate);

	if (nse != NULL)
	{
		pfree(cp[0]);
		switch (nse->itemtype)
		{
			case PLPGSQL_NSTYPE_VAR:
				plpgsql_yylval.dtype = ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;
				return T_DTYPE;

			/* XXX perhaps allow REC here? */

			default:
				return T_ERROR;
		}
	}

	/*
	 * Word wasn't found on the namestack. Try to find a data type with
	 * that name, but ignore pg_type entries that are in fact class types.
	 */
	typeOid = LookupTypeName(makeTypeName(cp[0]));
	if (OidIsValid(typeOid))
	{
		HeapTuple	typeTup;

		typeTup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(typeOid),
								 0, 0, 0);
		if (HeapTupleIsValid(typeTup))
		{
			Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			if (!typeStruct->typisdefined ||
				typeStruct->typrelid != InvalidOid)
			{
				ReleaseSysCache(typeTup);
				pfree(cp[0]);
				return T_ERROR;
			}

			plpgsql_yylval.dtype = build_datatype(typeTup, -1);

			ReleaseSysCache(typeTup);
			pfree(cp[0]);
			return T_DTYPE;
		}
	}

	/*
	 * Nothing found - up to now it's a word without any special meaning
	 * for us.
	 */
	pfree(cp[0]);
	return T_ERROR;
}


/* ----------
 * plpgsql_parse_dblwordtype		Same lookup for word.word%TYPE
 * ----------
 */
int
plpgsql_parse_dblwordtype(char *word)
{
	PLpgSQL_nsitem *nse;
	bool		old_nsstate;
	Oid			classOid;
	HeapTuple	classtup;
	Form_pg_class classStruct;
	HeapTuple	attrtup;
	Form_pg_attribute attrStruct;
	HeapTuple	typetup;
	char	   *cp[3];
	int			i;

	/* Do case conversion and word separation */
	/* We convert %type to .type momentarily to keep converter happy */
	i = strlen(word) - 5;
	Assert(word[i] == '%');
	word[i] = '.';
	plpgsql_convert_ident(word, cp, 3);
	word[i] = '%';
	pfree(cp[2]);

	/*
	 * Lookup the first word
	 */
	nse = plpgsql_ns_lookup(cp[0], NULL);

	/*
	 * If this is a label lookup the second word in that labels namestack
	 * level
	 */
	if (nse != NULL)
	{
		if (nse->itemtype == PLPGSQL_NSTYPE_LABEL)
		{
			old_nsstate = plpgsql_ns_setlocal(false);
			nse = plpgsql_ns_lookup(cp[1], cp[0]);
			plpgsql_ns_setlocal(old_nsstate);

			pfree(cp[0]);
			pfree(cp[1]);

			if (nse != NULL)
			{
				switch (nse->itemtype)
				{
					case PLPGSQL_NSTYPE_VAR:
						plpgsql_yylval.dtype = ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;
						return T_DTYPE;

					default:
						return T_ERROR;
				}
			}
			return T_ERROR;
		}
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * First word could also be a table name
	 */
	classOid = RelnameGetRelid(cp[0]);
	if (!OidIsValid(classOid))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classOid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * It must be a relation, sequence, view, or type
	 */
	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * Fetch the named table field and it's type
	 */
	attrtup = SearchSysCacheAttName(classOid, cp[1]);
	if (!HeapTupleIsValid(attrtup))
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(attrStruct->atttypid),
							 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", attrStruct->atttypid);

	/*
	 * Found that - build a compiler type struct and return it
	 */
	plpgsql_yylval.dtype = build_datatype(typetup, attrStruct->atttypmod);

	ReleaseSysCache(classtup);
	ReleaseSysCache(attrtup);
	ReleaseSysCache(typetup);
	pfree(cp[0]);
	pfree(cp[1]);
	return T_DTYPE;
}

/* ----------
 * plpgsql_parse_tripwordtype		Same lookup for word.word.word%TYPE
 * ----------
 */
#define TYPE_JUNK_LEN	5

int
plpgsql_parse_tripwordtype(char *word)
{
	Oid			classOid;
	HeapTuple	classtup;
	Form_pg_class classStruct;
	HeapTuple	attrtup;
	Form_pg_attribute attrStruct;
	HeapTuple	typetup;
	char	   *cp[2];
	char	   *colname[1];
	int			qualified_att_len;
	int			numdots = 0;
	int			i;
	RangeVar   *relvar;

	/* Do case conversion and word separation */
	qualified_att_len = strlen(word) - TYPE_JUNK_LEN;
	Assert(word[qualified_att_len] == '%');

	for (i = 0; i < qualified_att_len; i++)
	{
		if (word[i] == '.' && ++numdots == 2)
		{
			cp[0] = (char *) palloc((i + 1) * sizeof(char));
			memset(cp[0], 0, (i + 1) * sizeof(char));
			memcpy(cp[0], word, i * sizeof(char));

			/*
			 * qualified_att_len - one based position + 1 (null
			 * terminator)
			 */
			cp[1] = (char *) palloc((qualified_att_len - i) * sizeof(char));
			memset(cp[1], 0, (qualified_att_len - i) * sizeof(char));
			memcpy(cp[1], &word[i + 1], (qualified_att_len - i - 1) * sizeof(char));

			break;
		}
	}

	relvar = makeRangeVarFromNameList(stringToQualifiedNameList(cp[0], "plpgsql_parse_tripwordtype"));
	classOid = RangeVarGetRelid(relvar, true);
	if (!OidIsValid(classOid))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classOid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * It must be a relation, sequence, view, or type
	 */
	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * Fetch the named table field and it's type
	 */
	plpgsql_convert_ident(cp[1], colname, 1);
	attrtup = SearchSysCacheAttName(classOid, colname[0]);
	pfree(colname[0]);

	if (!HeapTupleIsValid(attrtup))
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(attrStruct->atttypid),
							 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", attrStruct->atttypid);

	/*
	 * Found that - build a compiler type struct and return it
	 */
	plpgsql_yylval.dtype = build_datatype(typetup, attrStruct->atttypmod);

	ReleaseSysCache(classtup);
	ReleaseSysCache(attrtup);
	ReleaseSysCache(typetup);
	pfree(cp[0]);
	pfree(cp[1]);

	return T_DTYPE;
}

/* ----------
 * plpgsql_parse_wordrowtype		Scanner found word%ROWTYPE.
 *					So word must be a table name.
 * ----------
 */
int
plpgsql_parse_wordrowtype(char *word)
{
	Oid			classOid;
	char	   *cp[2];
	int			i;

	/* Do case conversion and word separation */
	/* We convert %rowtype to .rowtype momentarily to keep converter happy */
	i = strlen(word) - 8;
	Assert(word[i] == '%');
	word[i] = '.';
	plpgsql_convert_ident(word, cp, 2);
	word[i] = '%';

	/* Lookup the relation */
	classOid = RelnameGetRelid(cp[0]);
	if (!OidIsValid(classOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" does not exist", cp[0])));

	/*
	 * Build and return the row type struct
	 */
	plpgsql_yylval.dtype = plpgsql_build_datatype(get_rel_type_id(classOid),
												  -1);

	pfree(cp[0]);
	pfree(cp[1]);

	return T_DTYPE;
}

/* ----------
 * plpgsql_parse_dblwordrowtype		Scanner found word.word%ROWTYPE.
 *			So word must be a namespace qualified table name.
 * ----------
 */
#define ROWTYPE_JUNK_LEN	8

int
plpgsql_parse_dblwordrowtype(char *word)
{
	Oid			classOid;
	char	   *cp;
	int			i;
	RangeVar   *relvar;

	/* Do case conversion and word separation */
	/* We convert %rowtype to .rowtype momentarily to keep converter happy */
	i = strlen(word) - ROWTYPE_JUNK_LEN;
	Assert(word[i] == '%');

	cp = (char *) palloc((i + 1) * sizeof(char));
	memset(cp, 0, (i + 1) * sizeof(char));
	memcpy(cp, word, i * sizeof(char));

	/* Lookup the relation */
	relvar = makeRangeVarFromNameList(stringToQualifiedNameList(cp, "plpgsql_parse_dblwordrowtype"));
	classOid = RangeVarGetRelid(relvar, true);
	if (!OidIsValid(classOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" does not exist", cp)));

	/*
	 * Build and return the row type struct
	 */
	plpgsql_yylval.dtype = plpgsql_build_datatype(get_rel_type_id(classOid),
												  -1);

	pfree(cp);

	return T_DTYPE;
}

/*
 * plpgsql_build_variable - build a datum-array entry of a given datatype
 *
 * The returned struct may be a PLpgSQL_var, PLpgSQL_row, or PLpgSQL_rec
 * depending on the given datatype.  The struct is automatically added
 * to the current datum array, and optionally to the current namespace.
 */
PLpgSQL_variable *
plpgsql_build_variable(char *refname, int lineno, PLpgSQL_type *dtype,
					bool add2namespace)
{
	PLpgSQL_variable *result;

	switch (dtype->ttype)
	{
		case PLPGSQL_TTYPE_SCALAR:
		{
			/* Ordinary scalar datatype */
			PLpgSQL_var		*var;

			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype		= PLPGSQL_DTYPE_VAR;
			var->refname	= refname;
			var->lineno		= lineno;
			var->datatype	= dtype;
			/* other fields might be filled by caller */

			/* preset to NULL */
			var->value = 0;
			var->isnull = true;
			var->freeval = false;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			if (add2namespace)
				plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR,
								   var->varno,
								   refname);
			result = (PLpgSQL_variable *) var;
			break;
		}
		case PLPGSQL_TTYPE_ROW:
		{
			/* Composite type -- build a row variable */
			PLpgSQL_row	   *row;

			row = build_row_var(dtype->typrelid);

			row->dtype		= PLPGSQL_DTYPE_ROW;
			row->refname	= refname;
			row->lineno		= lineno;

			plpgsql_adddatum((PLpgSQL_datum *) row);
			if (add2namespace)
				plpgsql_ns_additem(PLPGSQL_NSTYPE_ROW,
								   row->rowno,
								   refname);
			result = (PLpgSQL_variable *) row;
			break;
		}
		case PLPGSQL_TTYPE_REC:
		{
			/* "record" type -- build a variable-contents record variable */
			PLpgSQL_rec		*rec;

			rec = malloc(sizeof(PLpgSQL_rec));
			memset(rec, 0, sizeof(PLpgSQL_rec));

			rec->dtype		= PLPGSQL_DTYPE_REC;
			rec->refname	= refname;
			rec->lineno		= lineno;

			plpgsql_adddatum((PLpgSQL_datum *) rec);
			if (add2namespace)
				plpgsql_ns_additem(PLPGSQL_NSTYPE_REC,
								   rec->recno,
								   refname);
			result = (PLpgSQL_variable *) rec;
			break;
		}
		case PLPGSQL_TTYPE_PSEUDO:
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("variable \"%s\" has pseudo-type %s",
							refname, format_type_be(dtype->typoid))));
			result = NULL;		/* keep compiler quiet */
			break;
		}
		default:
			elog(ERROR, "unrecognized ttype: %d", dtype->ttype);
			result = NULL;		/* keep compiler quiet */
			break;
	}

	return result;
}

/*
 * Build a row-variable data structure given the pg_class OID.
 */
static PLpgSQL_row *
build_row_var(Oid classOid)
{
	PLpgSQL_row *row;
	Relation	rel;
	Form_pg_class classStruct;
	const char *relname;
	int			i;
	MemoryContext oldcxt;

	/*
	 * Open the relation to get info.
	 */
	rel = relation_open(classOid, AccessShareLock);
	classStruct = RelationGetForm(rel);
	relname = RelationGetRelationName(rel);

	/* accept relation, sequence, view, or composite type entries */
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a table", relname)));

	/*
	 * Create a row datum entry and all the required variables that it
	 * will point to.
	 */
	row = malloc(sizeof(PLpgSQL_row));
	memset(row, 0, sizeof(PLpgSQL_row));

	row->dtype = PLPGSQL_DTYPE_ROW;

	/*
	 * This is a bit ugly --- need a permanent copy of the rel's tupdesc.
	 * Someday all these mallocs should go away in favor of a per-function
	 * memory context ...
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	row->rowtupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	MemoryContextSwitchTo(oldcxt);

	row->nfields = classStruct->relnatts;
	row->fieldnames = malloc(sizeof(char *) * row->nfields);
	row->varnos = malloc(sizeof(int) * row->nfields);

	for (i = 0; i < row->nfields; i++)
	{
		Form_pg_attribute attrStruct;

		/*
		 * Get the attribute and check for dropped column
		 */
		attrStruct = RelationGetDescr(rel)->attrs[i];

		if (!attrStruct->attisdropped)
		{
			const char *attname;
			char	*refname;
			PLpgSQL_variable *var;

			attname = NameStr(attrStruct->attname);
			refname = malloc(strlen(relname) + strlen(attname) + 2);
			strcpy(refname, relname);
			strcat(refname, ".");
			strcat(refname, attname);

			/*
			 * Create the internal variable for the field
			 *
			 * We know if the table definitions contain a default value or if
			 * the field is declared in the table as NOT NULL. But it's
			 * possible to create a table field as NOT NULL without a default
			 * value and that would lead to problems later when initializing
			 * the variables due to entering a block at execution time. Thus
			 * we ignore this information for now.
			 */
			var = plpgsql_build_variable(refname, 0,
							  plpgsql_build_datatype(attrStruct->atttypid,
													 attrStruct->atttypmod),
										 false);

			/*
			 * Add the variable to the row.
			 */
			row->fieldnames[i] = strdup(attname);
			row->varnos[i] = var->dno;
		}
		else
		{
			/* Leave a hole in the row structure for the dropped col */
			row->fieldnames[i] = NULL;
			row->varnos[i] = -1;
		}
	}

	relation_close(rel, AccessShareLock);

	return row;
}


/* ----------
 * plpgsql_parse_datatype			Scanner found something that should
 *					be a datatype name.
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_datatype(const char *string)
{
	Oid			type_id;
	int32		typmod;

	/* Let the main parser try to parse it under standard SQL rules */
	parseTypeString(string, &type_id, &typmod);

	/* Okay, build a PLpgSQL_type data structure for it */
	return plpgsql_build_datatype(type_id, typmod);
}

/*
 * plpgsql_build_datatype
 *		Build PLpgSQL_type struct given type OID and typmod.
 */
PLpgSQL_type *
plpgsql_build_datatype(Oid typeOid, int32 typmod)
{
	HeapTuple	typeTup;
	PLpgSQL_type *typ;

	typeTup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(typeOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(typeTup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	typ = build_datatype(typeTup, typmod);

	ReleaseSysCache(typeTup);

	return typ;
}

/*
 * Utility subroutine to make a PLpgSQL_type struct given a pg_type entry
 */
static PLpgSQL_type *
build_datatype(HeapTuple typeTup, int32 typmod)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	PLpgSQL_type *typ;

	if (!typeStruct->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is only a shell",
						NameStr(typeStruct->typname))));

	typ = (PLpgSQL_type *) malloc(sizeof(PLpgSQL_type));

	typ->typname = strdup(NameStr(typeStruct->typname));
	typ->typoid = HeapTupleGetOid(typeTup);
	switch (typeStruct->typtype)
	{
		case 'b':				/* base type */
		case 'd':				/* domain */
			typ->ttype = PLPGSQL_TTYPE_SCALAR;
			break;
		case 'c':				/* composite, ie, rowtype */
			Assert(OidIsValid(typeStruct->typrelid));
			typ->ttype = PLPGSQL_TTYPE_ROW;
			break;
		case 'p':				/* pseudo */
			if (typ->typoid == RECORDOID)
				typ->ttype = PLPGSQL_TTYPE_REC;
			else
				typ->ttype = PLPGSQL_TTYPE_PSEUDO;
			break;
		default:
			elog(ERROR, "unrecognized typtype: %d",
				 (int) typeStruct->typtype);
			break;
	}
	typ->typlen = typeStruct->typlen;
	typ->typbyval = typeStruct->typbyval;
	typ->typrelid = typeStruct->typrelid;
	typ->typioparam = getTypeIOParam(typeTup);
	perm_fmgr_info(typeStruct->typinput, &(typ->typinput));
	typ->atttypmod = typmod;

	return typ;
}


/* ----------
 * plpgsql_adddatum			Add a variable, record or row
 *					to the compilers datum list.
 * ----------
 */
void
plpgsql_adddatum(PLpgSQL_datum * new)
{
	if (plpgsql_nDatums == datums_alloc)
	{
		datums_alloc *= 2;
		plpgsql_Datums = repalloc(plpgsql_Datums, sizeof(PLpgSQL_datum *) * datums_alloc);
	}

	new->dno = plpgsql_nDatums;
	plpgsql_Datums[plpgsql_nDatums++] = new;
}


/* ----------
 * plpgsql_add_initdatums		Put all datum entries created
 *					since the last call into the
 *					finishing code block so the
 *					block knows which variables to
 *					reinitialize when entered.
 * ----------
 */
int
plpgsql_add_initdatums(int **varnos)
{
	int			i;
	int			n = 0;

	for (i = datums_last; i < plpgsql_nDatums; i++)
	{
		switch (plpgsql_Datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				n++;
				break;

			default:
				break;
		}
	}

	if (varnos != NULL)
	{
		if (n > 0)
		{
			*varnos = (int *) malloc(sizeof(int) * n);

			n = 0;
			for (i = datums_last; i < plpgsql_nDatums; i++)
			{
				switch (plpgsql_Datums[i]->dtype)
				{
					case PLPGSQL_DTYPE_VAR:
						(*varnos)[n++] = plpgsql_Datums[i]->dno;

					default:
						break;
				}
			}
		}
		else
			*varnos = NULL;
	}

	datums_last = plpgsql_nDatums;
	return n;
}


/*
 * Compute the hashkey for a given function invocation
 *
 * The hashkey is returned into the caller-provided storage at *hashkey.
 */
static void
compute_function_hashkey(FunctionCallInfo fcinfo,
						 Form_pg_proc procStruct,
						 PLpgSQL_func_hashkey *hashkey,
						 bool forValidator)
{
	int			i;

	/* Make sure any unused bytes of the struct are zero */
	MemSet(hashkey, 0, sizeof(PLpgSQL_func_hashkey));

	/* get function OID */
	hashkey->funcOid = fcinfo->flinfo->fn_oid;

	/*
	 * if trigger, get relation OID.  In validation mode we do not know what
	 * relation is intended to be used, so we leave trigrelOid zero; the
	 * hash entry built in this case will never really be used.
	 */
	if (CALLED_AS_TRIGGER(fcinfo) && !forValidator)
	{
		TriggerData *trigdata = (TriggerData *) fcinfo->context;

		hashkey->trigrelOid = RelationGetRelid(trigdata->tg_relation);
	}

	/* get the argument types */
	for (i = 0; i < procStruct->pronargs; i++)
	{
		Oid			argtypeid = procStruct->proargtypes[i];

		/*
		 * Check for polymorphic arguments. If found, use the actual
		 * parameter type from the caller's FuncExpr node, if we have one.
		 * (In validation mode we arbitrarily assume we are dealing with
		 * integers.  This lets us build a valid, if possibly useless,
		 * function hashtable entry.)
		 *
		 * We can support arguments of type ANY the same way as normal
		 * polymorphic arguments.
		 */
		if (argtypeid == ANYARRAYOID || argtypeid == ANYELEMENTOID ||
			argtypeid == ANYOID)
		{
			if (forValidator)
			{
				if (argtypeid == ANYARRAYOID)
					argtypeid = INT4ARRAYOID;
				else
					argtypeid = INT4OID;
			}
			else
				argtypeid = get_fn_expr_argtype(fcinfo->flinfo, i);
			if (!OidIsValid(argtypeid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("could not determine actual argument "
								"type for polymorphic function \"%s\"",
								NameStr(procStruct->proname))));
		}

		hashkey->argtypes[i] = argtypeid;
	}
}

/* exported so we can call it from plpgsql_init() */
void
plpgsql_HashTableInit(void)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(PLpgSQL_func_hashkey);
	ctl.entrysize = sizeof(plpgsql_HashEnt);
	ctl.hash = tag_hash;
	plpgsql_HashTable = hash_create("PLpgSQL function cache",
									FUNCS_PER_USER,
									&ctl,
									HASH_ELEM | HASH_FUNCTION);
}

static PLpgSQL_function *
plpgsql_HashTableLookup(PLpgSQL_func_hashkey *func_key)
{
	plpgsql_HashEnt *hentry;

	hentry = (plpgsql_HashEnt *) hash_search(plpgsql_HashTable,
											 (void *) func_key,
											 HASH_FIND,
											 NULL);
	if (hentry)
		return hentry->function;
	else
		return NULL;
}

static void
plpgsql_HashTableInsert(PLpgSQL_function *function,
						PLpgSQL_func_hashkey *func_key)
{
	plpgsql_HashEnt *hentry;
	bool		found;

	hentry = (plpgsql_HashEnt *) hash_search(plpgsql_HashTable,
											 (void *) func_key,
											 HASH_ENTER,
											 &found);
	if (hentry == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	if (found)
		elog(WARNING, "trying to insert a function that already exists");

	hentry->function = function;
	/* prepare back link from function to hashtable key */
	function->fn_hashkey = &hentry->key;
}

static void
plpgsql_HashTableDelete(PLpgSQL_function *function)
{
	plpgsql_HashEnt *hentry;

	hentry = (plpgsql_HashEnt *) hash_search(plpgsql_HashTable,
										   (void *) function->fn_hashkey,
											 HASH_REMOVE,
											 NULL);
	if (hentry == NULL)
		elog(WARNING, "trying to delete function that does not exist");
}
