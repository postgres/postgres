/*-------------------------------------------------------------------------
 *
 * pl_comp.c		- Compiler part of the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/pl/plpgsql/src/pl_comp.c,v 1.108.2.3 2008/10/09 16:35:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"

#include <ctype.h>

#include "pl.tab.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "parser/gramparse.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


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
bool		plpgsql_DumpExecTree = false;
bool		plpgsql_check_syntax = false;

PLpgSQL_function *plpgsql_curr_compile;

/* A context appropriate for short-term allocs during compilation */
MemoryContext compile_tmp_cxt;

/* ----------
 * Hash table for compiled functions
 * ----------
 */
static HTAB *plpgsql_HashTable = NULL;

typedef struct plpgsql_hashent
{
	PLpgSQL_func_hashkey key;
	PLpgSQL_function *function;
} plpgsql_HashEnt;

#define FUNCS_PER_USER		128 /* initial table size */

/* ----------
 * Lookup table for EXCEPTION condition names
 * ----------
 */
typedef struct
{
	const char *label;
	int			sqlerrstate;
} ExceptionLabelMap;

static const ExceptionLabelMap exception_label_map[] = {
#include "plerrcodes.h"
	{NULL, 0}
};


/* ----------
 * static prototypes
 * ----------
 */
static PLpgSQL_function *do_compile(FunctionCallInfo fcinfo,
		   HeapTuple procTup,
		   PLpgSQL_function *function,
		   PLpgSQL_func_hashkey *hashkey,
		   bool forValidator);
static PLpgSQL_row *build_row_from_class(Oid classOid);
static PLpgSQL_row *build_row_from_vars(PLpgSQL_variable **vars, int numvars);
static PLpgSQL_type *build_datatype(HeapTuple typeTup, int32 typmod);
static void compute_function_hashkey(FunctionCallInfo fcinfo,
						 Form_pg_proc procStruct,
						 PLpgSQL_func_hashkey *hashkey,
						 bool forValidator);
static void plpgsql_resolve_polymorphic_argtypes(int numargs,
									 Oid *argtypes, char *argmodes,
									 Node *call_expr, bool forValidator,
									 const char *proname);
static PLpgSQL_function *plpgsql_HashTableLookup(PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableInsert(PLpgSQL_function *function,
						PLpgSQL_func_hashkey *func_key);
static void plpgsql_HashTableDelete(PLpgSQL_function *function);
static void delete_function(PLpgSQL_function *func);

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
	bool		function_valid = false;
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
	 * See if there's already a cache entry for the current FmgrInfo. If not,
	 * try to find one in the hash table.
	 */
	function = (PLpgSQL_function *) fcinfo->flinfo->fn_extra;

recheck:
	if (!function)
	{
		/* Compute hashkey using function signature and actual arg types */
		compute_function_hashkey(fcinfo, procStruct, &hashkey, forValidator);
		hashkey_valid = true;

		/* And do the lookup */
		function = plpgsql_HashTableLookup(&hashkey);
	}

	if (function)
	{
		/* We have a compiled function, but is it still valid? */
		if (function->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			function->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data))
			function_valid = true;
		else
		{
			/*
			 * Nope, so remove it from hashtable and try to drop associated
			 * storage (if not done already).
			 */
			delete_function(function);
			/*
			 * If the function isn't in active use then we can overwrite the
			 * func struct with new data, allowing any other existing fn_extra
			 * pointers to make use of the new definition on their next use.
			 * If it is in use then just leave it alone and make a new one.
			 * (The active invocations will run to completion using the
			 * previous definition, and then the cache entry will just be
			 * leaked; doesn't seem worth adding code to clean it up, given
			 * what a corner case this is.)
			 *
			 * If we found the function struct via fn_extra then it's possible
			 * a replacement has already been made, so go back and recheck
			 * the hashtable.
			 */
			if (function->use_count != 0)
			{
				function = NULL;
				if (!hashkey_valid)
					goto recheck;
			}
		}
	}

	/*
	 * If the function wasn't found or was out-of-date, we have to compile it
	 */
	if (!function_valid)
	{
		/*
		 * Calculate hashkey if we didn't already; we'll need it to store the
		 * completed function.
		 */
		if (!hashkey_valid)
			compute_function_hashkey(fcinfo, procStruct, &hashkey,
									 forValidator);

		/*
		 * Do the hard part.
		 */
		function = do_compile(fcinfo, procTup, function,
							  &hashkey, forValidator);
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
 *
 * The passed-in "function" pointer is either NULL or an already-allocated
 * function struct to overwrite.
 *
 * While compiling a function, the CurrentMemoryContext is the
 * per-function memory context of the function we are compiling. That
 * means a palloc() will allocate storage with the same lifetime as
 * the function itself.
 *
 * Because palloc()'d storage will not be immediately freed, temporary
 * allocations should either be performed in a short-lived memory
 * context or explicitly pfree'd. Since not all backend functions are
 * careful about pfree'ing their allocations, it is also wise to
 * switch into a short-term context before calling into the
 * backend. An appropriate context for performing short-term
 * allocations is the compile_tmp_cxt.
 *
 * NB: this code is not re-entrant.  We assume that nothing we do here could
 * result in the invocation of another plpgsql function.
 */
static PLpgSQL_function *
do_compile(FunctionCallInfo fcinfo,
		   HeapTuple procTup,
		   PLpgSQL_function *function,
		   PLpgSQL_func_hashkey *hashkey,
		   bool forValidator)
{
	Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	int			functype = CALLED_AS_TRIGGER(fcinfo) ? T_TRIGGER : T_FUNCTION;
	Datum		prosrcdatum;
	bool		isnull;
	char	   *proc_source;
	HeapTuple	typeTup;
	Form_pg_type typeStruct;
	PLpgSQL_variable *var;
	PLpgSQL_rec *rec;
	int			i;
	ErrorContextCallback plerrcontext;
	int			parse_rc;
	Oid			rettypeid;
	int			numargs;
	int			num_in_args = 0;
	int			num_out_args = 0;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int		   *in_arg_varnos = NULL;
	PLpgSQL_variable **out_arg_variables;
	MemoryContext func_cxt;

	/*
	 * Setup the scanner input and error info.	We assume that this function
	 * cannot be invoked recursively, so there's no need to save and restore
	 * the static variables used here.
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
	plerrcontext.arg = forValidator ? proc_source : NULL;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * Initialize the compiler
	 */
	plpgsql_ns_init();
	plpgsql_ns_push(NULL);
	plpgsql_DumpExecTree = false;

	datums_alloc = 128;
	plpgsql_nDatums = 0;
	/* This is short-lived, so needn't allocate in function's cxt */
	plpgsql_Datums = palloc(sizeof(PLpgSQL_datum *) * datums_alloc);
	datums_last = 0;

	/*
	 * Do extra syntax checks when validating the function definition. We skip
	 * this when actually compiling functions for execution, for performance
	 * reasons.
	 */
	plpgsql_check_syntax = forValidator;

	/*
	 * Create the new function struct, if not done already.  The function
	 * structs are never thrown away, so keep them in TopMemoryContext.
	 */
	if (function == NULL)
	{
		function = (PLpgSQL_function *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(PLpgSQL_function));
	}
	else
	{
		/* re-using a previously existing struct, so clear it out */
		memset(function, 0, sizeof(PLpgSQL_function));
	}
	plpgsql_curr_compile = function;

	/*
	 * All the rest of the compile-time storage (e.g. parse tree) is kept in
	 * its own memory context, so it can be reclaimed easily.
	 */
	func_cxt = AllocSetContextCreate(TopMemoryContext,
									 "PL/PgSQL function context",
									 ALLOCSET_DEFAULT_MINSIZE,
									 ALLOCSET_DEFAULT_INITSIZE,
									 ALLOCSET_DEFAULT_MAXSIZE);
	compile_tmp_cxt = MemoryContextSwitchTo(func_cxt);

	function->fn_name = pstrdup(NameStr(procStruct->proname));
	function->fn_oid = fcinfo->flinfo->fn_oid;
	function->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	function->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);
	function->fn_functype = functype;
	function->fn_cxt = func_cxt;
	function->out_param_varno = -1;		/* set up for no OUT param */

	switch (functype)
	{
		case T_FUNCTION:

			/*
			 * Fetch info about the procedure's parameters. Allocations aren't
			 * needed permanently, so make them in tmp cxt.
			 *
			 * We also need to resolve any polymorphic input or output
			 * argument types.	In validation mode we won't be able to, so we
			 * arbitrarily assume we are dealing with integers.
			 */
			MemoryContextSwitchTo(compile_tmp_cxt);

			numargs = get_func_arg_info(procTup,
										&argtypes, &argnames, &argmodes);

			plpgsql_resolve_polymorphic_argtypes(numargs, argtypes, argmodes,
												 fcinfo->flinfo->fn_expr,
												 forValidator,
												 plpgsql_error_funcname);

			in_arg_varnos = (int *) palloc(numargs * sizeof(int));
			out_arg_variables = (PLpgSQL_variable **) palloc(numargs * sizeof(PLpgSQL_variable *));

			MemoryContextSwitchTo(func_cxt);

			/*
			 * Create the variables for the procedure's parameters.
			 */
			for (i = 0; i < numargs; i++)
			{
				char		buf[32];
				Oid			argtypeid = argtypes[i];
				char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;
				PLpgSQL_type *argdtype;
				PLpgSQL_variable *argvariable;
				int			argitemtype;

				/* Create $n name for variable */
				snprintf(buf, sizeof(buf), "$%d", i + 1);

				/* Create datatype info */
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
				argvariable = plpgsql_build_variable(buf, 0,
													 argdtype, false);

				if (argvariable->dtype == PLPGSQL_DTYPE_VAR)
				{
					argitemtype = PLPGSQL_NSTYPE_VAR;
					/* input argument vars are forced to be CONSTANT */
					if (argmode == PROARGMODE_IN)
						((PLpgSQL_var *) argvariable)->isconst = true;
				}
				else
				{
					Assert(argvariable->dtype == PLPGSQL_DTYPE_ROW);
					argitemtype = PLPGSQL_NSTYPE_ROW;
				}

				/* Remember arguments in appropriate arrays */
				if (argmode == PROARGMODE_IN || argmode == PROARGMODE_INOUT)
					in_arg_varnos[num_in_args++] = argvariable->dno;
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_INOUT)
					out_arg_variables[num_out_args++] = argvariable;

				/* Add to namespace under the $n name */
				plpgsql_ns_additem(argitemtype, argvariable->dno, buf);

				/* If there's a name for the argument, make an alias */
				if (argnames && argnames[i][0] != '\0')
					plpgsql_ns_additem(argitemtype, argvariable->dno,
									   argnames[i]);
			}

			/*
			 * If there's just one OUT parameter, out_param_varno points
			 * directly to it.	If there's more than one, build a row that
			 * holds all of them.
			 */
			if (num_out_args == 1)
				function->out_param_varno = out_arg_variables[0]->dno;
			else if (num_out_args > 1)
			{
				PLpgSQL_row *row = build_row_from_vars(out_arg_variables,
													   num_out_args);

				plpgsql_adddatum((PLpgSQL_datum *) row);
				function->out_param_varno = row->rowno;
			}

			/*
			 * Check for a polymorphic returntype. If found, use the actual
			 * returntype type from the caller's FuncExpr node, if we have
			 * one.  (In validation mode we arbitrarily assume we are dealing
			 * with integers.)
			 *
			 * Note: errcode is FEATURE_NOT_SUPPORTED because it should always
			 * work; if it doesn't we're in some context that fails to make
			 * the info available.
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
				{
					rettypeid = get_fn_expr_rettype(fcinfo->flinfo);
					if (!OidIsValid(rettypeid))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("could not determine actual return type "
									"for polymorphic function \"%s\"",
									plpgsql_error_funcname)));
				}
			}

			/*
			 * Normal function has a defined returntype
			 */
			function->fn_rettype = rettypeid;
			function->fn_retset = procStruct->proretset;

			/*
			 * Lookup the function's return type
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
				fmgr_info(typeStruct->typinput, &(function->fn_retinput));

				/*
				 * install $0 reference, but only for polymorphic return
				 * types, and not when the return is specified through an
				 * output parameter.
				 */
				if ((procStruct->prorettype == ANYARRAYOID ||
					 procStruct->prorettype == ANYELEMENTOID) &&
					num_out_args == 0)
				{
					(void) plpgsql_build_variable("$0", 0,
												  build_datatype(typeTup, -1),
												  true);
				}
			}
			ReleaseSysCache(typeTup);
			break;

		case T_TRIGGER:
			/* Trigger procedure's return type is unknown yet */
			function->fn_rettype = InvalidOid;
			function->fn_retbyval = false;
			function->fn_retistuple = true;
			function->fn_retset = false;

			/* shouldn't be any declared arguments */
			if (procStruct->pronargs != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				  errmsg("trigger functions cannot have declared arguments"),
						 errhint("You probably want to use TG_NARGS and TG_ARGV instead.")));

			/* Add the record for referencing NEW */
			rec = palloc0(sizeof(PLpgSQL_rec));
			rec->dtype = PLPGSQL_DTYPE_REC;
			rec->refname = pstrdup("new");
			rec->tup = NULL;
			rec->tupdesc = NULL;
			rec->freetup = false;
			plpgsql_adddatum((PLpgSQL_datum *) rec);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->recno, rec->refname);
			function->new_varno = rec->recno;

			/* Add the record for referencing OLD */
			rec = palloc0(sizeof(PLpgSQL_rec));
			rec->dtype = PLPGSQL_DTYPE_REC;
			rec->refname = pstrdup("old");
			rec->tup = NULL;
			rec->tupdesc = NULL;
			rec->freetup = false;
			plpgsql_adddatum((PLpgSQL_datum *) rec);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->recno, rec->refname);
			function->old_varno = rec->recno;

			/* Add the variable tg_name */
			var = plpgsql_build_variable("tg_name", 0,
										 plpgsql_build_datatype(NAMEOID, -1),
										 true);
			function->tg_name_varno = var->dno;

			/* Add the variable tg_when */
			var = plpgsql_build_variable("tg_when", 0,
										 plpgsql_build_datatype(TEXTOID, -1),
										 true);
			function->tg_when_varno = var->dno;

			/* Add the variable tg_level */
			var = plpgsql_build_variable("tg_level", 0,
										 plpgsql_build_datatype(TEXTOID, -1),
										 true);
			function->tg_level_varno = var->dno;

			/* Add the variable tg_op */
			var = plpgsql_build_variable("tg_op", 0,
										 plpgsql_build_datatype(TEXTOID, -1),
										 true);
			function->tg_op_varno = var->dno;

			/* Add the variable tg_relid */
			var = plpgsql_build_variable("tg_relid", 0,
										 plpgsql_build_datatype(OIDOID, -1),
										 true);
			function->tg_relid_varno = var->dno;

			/* Add the variable tg_relname */
			var = plpgsql_build_variable("tg_relname", 0,
										 plpgsql_build_datatype(NAMEOID, -1),
										 true);
			function->tg_relname_varno = var->dno;

			/* tg_table_name is now preferred to tg_relname */
			var = plpgsql_build_variable("tg_table_name", 0,
										 plpgsql_build_datatype(NAMEOID, -1),
										 true);
			function->tg_table_name_varno = var->dno;


			/* add variable tg_table_schema */
			var = plpgsql_build_variable("tg_table_schema", 0,
										 plpgsql_build_datatype(NAMEOID, -1),
										 true);
			function->tg_table_schema_varno = var->dno;


			/* Add the variable tg_nargs */
			var = plpgsql_build_variable("tg_nargs", 0,
										 plpgsql_build_datatype(INT4OID, -1),
										 true);
			function->tg_nargs_varno = var->dno;

			break;

		default:
			elog(ERROR, "unrecognized function typecode: %u", functype);
			break;
	}

	/* Remember if function is STABLE/IMMUTABLE */
	function->fn_readonly = (procStruct->provolatile != PROVOLATILE_VOLATILE);

	/*
	 * Create the magic FOUND variable.
	 */
	var = plpgsql_build_variable("found", 0,
								 plpgsql_build_datatype(BOOLOID, -1),
								 true);
	function->found_varno = var->dno;

	/*
	 * Now parse the function's text
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql parser returned %d", parse_rc);
	function->action = plpgsql_yylval.program;

	plpgsql_scanner_finish();
	pfree(proc_source);

	/*
	 * If it has OUT parameters or returns VOID or returns a set, we allow
	 * control to fall off the end without an explicit RETURN statement. The
	 * easiest way to implement this is to add a RETURN statement to the end
	 * of the statement list during parsing.  However, if the outer block has
	 * an EXCEPTION clause, we need to make a new outer block, since the added
	 * RETURN shouldn't act like it is inside the EXCEPTION clause.
	 */
	if (num_out_args > 0 || function->fn_rettype == VOIDOID ||
		function->fn_retset)
	{
		if (function->action->exceptions != NULL)
		{
			PLpgSQL_stmt_block *new;

			new = palloc0(sizeof(PLpgSQL_stmt_block));
			new->cmd_type = PLPGSQL_STMT_BLOCK;
			new->body = list_make1(function->action);

			function->action = new;
		}
		if (function->action->body == NIL ||
			((PLpgSQL_stmt *) llast(function->action->body))->cmd_type != PLPGSQL_STMT_RETURN)
		{
			PLpgSQL_stmt_return *new;

			new = palloc0(sizeof(PLpgSQL_stmt_return));
			new->cmd_type = PLPGSQL_STMT_RETURN;
			new->expr = NULL;
			new->retvarno = function->out_param_varno;

			function->action->body = lappend(function->action->body, new);
		}
	}

	/*
	 * Complete the function's info
	 */
	function->fn_nargs = procStruct->pronargs;
	for (i = 0; i < function->fn_nargs; i++)
		function->fn_argvarnos[i] = in_arg_varnos[i];
	function->ndatums = plpgsql_nDatums;
	function->datums = palloc(sizeof(PLpgSQL_datum *) * plpgsql_nDatums);
	for (i = 0; i < plpgsql_nDatums; i++)
		function->datums[i] = plpgsql_Datums[i];

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

	plpgsql_check_syntax = false;

	MemoryContextSwitchTo(compile_tmp_cxt);
	compile_tmp_cxt = NULL;
	return function;
}


/*
 * error context callback to let us supply a call-stack traceback. If
 * we are validating, the function source is passed as an
 * argument. This function is public only for the sake of an assertion
 * in gram.y
 */
void
plpgsql_compile_error_callback(void *arg)
{
	if (arg)
	{
		/*
		 * Try to convert syntax error position to reference text of original
		 * CREATE FUNCTION command.
		 */
		if (function_parse_error_transpose((const char *) arg))
			return;

		/*
		 * Done if a syntax error position was reported; otherwise we have to
		 * fall back to a "near line N" report.
		 */
	}

	if (plpgsql_error_funcname)
		errcontext("compile of PL/pgSQL function \"%s\" near line %d",
				   plpgsql_error_funcname, plpgsql_error_lineno);
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
			bool		save_spacescanned = plpgsql_SpaceScanned;
			PLpgSQL_trigarg *trigarg;

			trigarg = palloc0(sizeof(PLpgSQL_trigarg));
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
	 * Do a lookup on the compiler's namestack
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
	 * Nothing found - up to now it's a word without any special meaning for
	 * us.
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
				 * First word is a record name, so second word must be a field
				 * in this record.
				 */
				PLpgSQL_recfield *new;

				new = palloc(sizeof(PLpgSQL_recfield));
				new->dtype = PLPGSQL_DTYPE_RECFIELD;
				new->fieldname = pstrdup(cp[1]);
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
				 * First word is a row name, so second word must be a field in
				 * this row.
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
				 * This word is a record name, so third word must be a field
				 * in this record.
				 */
				PLpgSQL_recfield *new;

				new = palloc(sizeof(PLpgSQL_recfield));
				new->dtype = PLPGSQL_DTYPE_RECFIELD;
				new->fieldname = pstrdup(cp[2]);
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
				 * This word is a row name, so third word must be a field in
				 * this row.
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
	 * Do a lookup on the compiler's namestack. But ensure it moves up to the
	 * toplevel.
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
	 * Word wasn't found on the namestack. Try to find a data type with that
	 * name, but ignore pg_type entries that are in fact class types.
	 */
	typeOid = LookupTypeName(NULL, makeTypeName(cp[0]));
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
	 * Nothing found - up to now it's a word without any special meaning for
	 * us.
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
	HeapTuple	classtup = NULL;
	HeapTuple	attrtup = NULL;
	HeapTuple	typetup = NULL;
	Form_pg_class classStruct;
	Form_pg_attribute attrStruct;
	char	   *cp[3];
	int			i;
	MemoryContext oldCxt;
	int			result = T_ERROR;

	/* Avoid memory leaks in the long-term function context */
	oldCxt = MemoryContextSwitchTo(compile_tmp_cxt);

	/* Do case conversion and word separation */
	/* We convert %type to .type momentarily to keep converter happy */
	i = strlen(word) - 5;
	Assert(word[i] == '%');
	word[i] = '.';
	plpgsql_convert_ident(word, cp, 3);
	word[i] = '%';

	/*
	 * Lookup the first word
	 */
	nse = plpgsql_ns_lookup(cp[0], NULL);

	/*
	 * If this is a label lookup the second word in that label's namestack
	 * level
	 */
	if (nse != NULL)
	{
		if (nse->itemtype == PLPGSQL_NSTYPE_LABEL)
		{
			old_nsstate = plpgsql_ns_setlocal(false);
			nse = plpgsql_ns_lookup(cp[1], cp[0]);
			plpgsql_ns_setlocal(old_nsstate);

			if (nse != NULL && nse->itemtype == PLPGSQL_NSTYPE_VAR)
			{
				plpgsql_yylval.dtype = ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;
				result = T_DTYPE;
			}
		}

		/* Return T_ERROR if not found, otherwise T_DTYPE */
		goto done;
	}

	/*
	 * First word could also be a table name
	 */
	classOid = RelnameGetRelid(cp[0]);
	if (!OidIsValid(classOid))
		goto done;

	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classOid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
		goto done;
	classStruct = (Form_pg_class) GETSTRUCT(classtup);

	/*
	 * It must be a relation, sequence, view, or type
	 */
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
		goto done;

	/*
	 * Fetch the named table field and its type
	 */
	attrtup = SearchSysCacheAttName(classOid, cp[1]);
	if (!HeapTupleIsValid(attrtup))
		goto done;
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(attrStruct->atttypid),
							 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", attrStruct->atttypid);

	/*
	 * Found that - build a compiler type struct in the caller's cxt and
	 * return it
	 */
	MemoryContextSwitchTo(oldCxt);
	plpgsql_yylval.dtype = build_datatype(typetup, attrStruct->atttypmod);
	MemoryContextSwitchTo(compile_tmp_cxt);
	result = T_DTYPE;

done:
	if (HeapTupleIsValid(classtup))
		ReleaseSysCache(classtup);
	if (HeapTupleIsValid(attrtup))
		ReleaseSysCache(attrtup);
	if (HeapTupleIsValid(typetup))
		ReleaseSysCache(typetup);

	MemoryContextSwitchTo(oldCxt);
	return result;
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
	HeapTuple	classtup = NULL;
	HeapTuple	attrtup = NULL;
	HeapTuple	typetup = NULL;
	Form_pg_class classStruct;
	Form_pg_attribute attrStruct;
	char	   *cp[2];
	char	   *colname[1];
	int			qualified_att_len;
	int			numdots = 0;
	int			i;
	RangeVar   *relvar;
	MemoryContext oldCxt;
	int			result = T_ERROR;

	/* Avoid memory leaks in the long-term function context */
	oldCxt = MemoryContextSwitchTo(compile_tmp_cxt);

	/* Do case conversion and word separation */
	qualified_att_len = strlen(word) - TYPE_JUNK_LEN;
	Assert(word[qualified_att_len] == '%');

	for (i = 0; i < qualified_att_len; i++)
	{
		if (word[i] == '.' && ++numdots == 2)
			break;
	}

	cp[0] = (char *) palloc((i + 1) * sizeof(char));
	memcpy(cp[0], word, i * sizeof(char));
	cp[0][i] = '\0';

	/*
	 * qualified_att_len - one based position + 1 (null terminator)
	 */
	cp[1] = (char *) palloc((qualified_att_len - i) * sizeof(char));
	memcpy(cp[1], &word[i + 1], (qualified_att_len - i - 1) * sizeof(char));
	cp[1][qualified_att_len - i - 1] = '\0';

	relvar = makeRangeVarFromNameList(stringToQualifiedNameList(cp[0],
											  "plpgsql_parse_tripwordtype"));
	classOid = RangeVarGetRelid(relvar, true);
	if (!OidIsValid(classOid))
		goto done;

	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classOid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
		goto done;
	classStruct = (Form_pg_class) GETSTRUCT(classtup);

	/*
	 * It must be a relation, sequence, view, or type
	 */
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
		goto done;

	/*
	 * Fetch the named table field and its type
	 */
	plpgsql_convert_ident(cp[1], colname, 1);
	attrtup = SearchSysCacheAttName(classOid, colname[0]);
	if (!HeapTupleIsValid(attrtup))
		goto done;
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(attrStruct->atttypid),
							 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", attrStruct->atttypid);

	/*
	 * Found that - build a compiler type struct in the caller's cxt and
	 * return it
	 */
	MemoryContextSwitchTo(oldCxt);
	plpgsql_yylval.dtype = build_datatype(typetup, attrStruct->atttypmod);
	MemoryContextSwitchTo(compile_tmp_cxt);
	result = T_DTYPE;

done:
	if (HeapTupleIsValid(classtup))
		ReleaseSysCache(classtup);
	if (HeapTupleIsValid(attrtup))
		ReleaseSysCache(attrtup);
	if (HeapTupleIsValid(typetup))
		ReleaseSysCache(typetup);

	MemoryContextSwitchTo(oldCxt);
	return result;
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
	MemoryContext oldCxt;

	/* Avoid memory leaks in long-term function context */
	oldCxt = MemoryContextSwitchTo(compile_tmp_cxt);

	/* Do case conversion and word separation */
	/* We convert %rowtype to .rowtype momentarily to keep converter happy */
	i = strlen(word) - ROWTYPE_JUNK_LEN;
	Assert(word[i] == '%');
	word[i] = '\0';
	cp = pstrdup(word);
	word[i] = '%';

	/* Lookup the relation */
	relvar = makeRangeVarFromNameList(stringToQualifiedNameList(cp, "plpgsql_parse_dblwordrowtype"));
	classOid = RangeVarGetRelid(relvar, true);
	if (!OidIsValid(classOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" does not exist", cp)));

	/* Build and return the row type struct */
	plpgsql_yylval.dtype = plpgsql_build_datatype(get_rel_type_id(classOid),
												  -1);

	MemoryContextSwitchTo(oldCxt);
	return T_DTYPE;
}

/*
 * plpgsql_build_variable - build a datum-array entry of a given
 * datatype
 *
 * The returned struct may be a PLpgSQL_var, PLpgSQL_row, or
 * PLpgSQL_rec depending on the given datatype, and is allocated via
 * palloc.	The struct is automatically added to the current datum
 * array, and optionally to the current namespace.
 */
PLpgSQL_variable *
plpgsql_build_variable(const char *refname, int lineno, PLpgSQL_type *dtype,
					   bool add2namespace)
{
	PLpgSQL_variable *result;

	switch (dtype->ttype)
	{
		case PLPGSQL_TTYPE_SCALAR:
			{
				/* Ordinary scalar datatype */
				PLpgSQL_var *var;

				var = palloc0(sizeof(PLpgSQL_var));
				var->dtype = PLPGSQL_DTYPE_VAR;
				var->refname = pstrdup(refname);
				var->lineno = lineno;
				var->datatype = dtype;
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
				PLpgSQL_row *row;

				row = build_row_from_class(dtype->typrelid);

				row->dtype = PLPGSQL_DTYPE_ROW;
				row->refname = pstrdup(refname);
				row->lineno = lineno;

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
				/*
				 * "record" type -- build a variable-contents record variable
				 */
				PLpgSQL_rec *rec;

				rec = palloc0(sizeof(PLpgSQL_rec));
				rec->dtype = PLPGSQL_DTYPE_REC;
				rec->refname = pstrdup(refname);
				rec->lineno = lineno;

				plpgsql_adddatum((PLpgSQL_datum *) rec);
				if (add2namespace)
					plpgsql_ns_additem(PLPGSQL_NSTYPE_REC,
									   rec->recno,
									   refname);
				result = (PLpgSQL_variable *) rec;
				break;
			}
		case PLPGSQL_TTYPE_PSEUDO:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("variable \"%s\" has pseudo-type %s",
							refname, format_type_be(dtype->typoid))));
			result = NULL;		/* keep compiler quiet */
			break;
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
build_row_from_class(Oid classOid)
{
	PLpgSQL_row *row;
	Relation	rel;
	Form_pg_class classStruct;
	const char *relname;
	int			i;

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
	 * Create a row datum entry and all the required variables that it will
	 * point to.
	 */
	row = palloc0(sizeof(PLpgSQL_row));
	row->dtype = PLPGSQL_DTYPE_ROW;
	row->rowtupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	row->nfields = classStruct->relnatts;
	row->fieldnames = palloc(sizeof(char *) * row->nfields);
	row->varnos = palloc(sizeof(int) * row->nfields);

	for (i = 0; i < row->nfields; i++)
	{
		Form_pg_attribute attrStruct;

		/*
		 * Get the attribute and check for dropped column
		 */
		attrStruct = row->rowtupdesc->attrs[i];

		if (!attrStruct->attisdropped)
		{
			char	   *attname;
			char		refname[(NAMEDATALEN * 2) + 100];
			PLpgSQL_variable *var;

			attname = NameStr(attrStruct->attname);
			snprintf(refname, sizeof(refname), "%s.%s", relname, attname);

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

			/* Add the variable to the row */
			row->fieldnames[i] = attname;
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

/*
 * Build a row-variable data structure given the component variables.
 */
static PLpgSQL_row *
build_row_from_vars(PLpgSQL_variable **vars, int numvars)
{
	PLpgSQL_row *row;
	int			i;

	row = palloc0(sizeof(PLpgSQL_row));
	row->dtype = PLPGSQL_DTYPE_ROW;
	row->rowtupdesc = CreateTemplateTupleDesc(numvars, false);
	row->nfields = numvars;
	row->fieldnames = palloc(numvars * sizeof(char *));
	row->varnos = palloc(numvars * sizeof(int));

	for (i = 0; i < numvars; i++)
	{
		PLpgSQL_variable *var = vars[i];
		Oid			typoid = RECORDOID;
		int32		typmod = -1;

		switch (var->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				typoid = ((PLpgSQL_var *) var)->datatype->typoid;
				typmod = ((PLpgSQL_var *) var)->datatype->atttypmod;
				break;

			case PLPGSQL_DTYPE_REC:
				break;

			case PLPGSQL_DTYPE_ROW:
				if (((PLpgSQL_row *) var)->rowtupdesc)
				{
					typoid = ((PLpgSQL_row *) var)->rowtupdesc->tdtypeid;
					typmod = ((PLpgSQL_row *) var)->rowtupdesc->tdtypmod;
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", var->dtype);
		}

		row->fieldnames[i] = var->refname;
		row->varnos[i] = var->dno;

		TupleDescInitEntry(row->rowtupdesc, i + 1,
						   var->refname,
						   typoid, typmod,
						   0);
	}

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

	typ = (PLpgSQL_type *) palloc(sizeof(PLpgSQL_type));

	typ->typname = pstrdup(NameStr(typeStruct->typname));
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
	fmgr_info(typeStruct->typinput, &(typ->typinput));
	typ->atttypmod = typmod;

	return typ;
}

/*
 * plpgsql_parse_err_condition
 *		Generate PLpgSQL_condition entry(s) for an exception condition name
 *
 * This has to be able to return a list because there are some duplicate
 * names in the table of error code names.
 */
PLpgSQL_condition *
plpgsql_parse_err_condition(char *condname)
{
	int			i;
	PLpgSQL_condition *new;
	PLpgSQL_condition *prev;

	/*
	 * XXX Eventually we will want to look for user-defined exception names
	 * here.
	 */

	/*
	 * OTHERS is represented as code 0 (which would map to '00000', but we
	 * have no need to represent that as an exception condition).
	 */
	if (strcmp(condname, "others") == 0)
	{
		new = palloc(sizeof(PLpgSQL_condition));
		new->sqlerrstate = 0;
		new->condname = condname;
		new->next = NULL;
		return new;
	}

	prev = NULL;
	for (i = 0; exception_label_map[i].label != NULL; i++)
	{
		if (strcmp(condname, exception_label_map[i].label) == 0)
		{
			new = palloc(sizeof(PLpgSQL_condition));
			new->sqlerrstate = exception_label_map[i].sqlerrstate;
			new->condname = condname;
			new->next = prev;
			prev = new;
		}
	}

	if (!prev)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized exception condition \"%s\"",
						condname)));

	return prev;
}

/* ----------
 * plpgsql_adddatum			Add a variable, record or row
 *					to the compiler's datum list.
 * ----------
 */
void
plpgsql_adddatum(PLpgSQL_datum *new)
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
 * plpgsql_add_initdatums		Make an array of the datum numbers of
 *					all the simple VAR datums created since the last call
 *					to this function.
 *
 * If varnos is NULL, we just forget any datum entries created since the
 * last call.
 *
 * This is used around a DECLARE section to create a list of the VARs
 * that have to be initialized at block entry.  Note that VARs can also
 * be created elsewhere than DECLARE, eg by a FOR-loop, but it is then
 * the responsibility of special-purpose code to initialize them.
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
			*varnos = (int *) palloc(sizeof(int) * n);

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
	/* Make sure any unused bytes of the struct are zero */
	MemSet(hashkey, 0, sizeof(PLpgSQL_func_hashkey));

	/* get function OID */
	hashkey->funcOid = fcinfo->flinfo->fn_oid;

	/* get call context */
	hashkey->isTrigger = CALLED_AS_TRIGGER(fcinfo);

	/*
	 * if trigger, get relation OID.  In validation mode we do not know what
	 * relation is intended to be used, so we leave trigrelOid zero; the hash
	 * entry built in this case will never really be used.
	 */
	if (hashkey->isTrigger && !forValidator)
	{
		TriggerData *trigdata = (TriggerData *) fcinfo->context;

		hashkey->trigrelOid = RelationGetRelid(trigdata->tg_relation);
	}

	if (procStruct->pronargs > 0)
	{
		/* get the argument types */
		memcpy(hashkey->argtypes, procStruct->proargtypes.values,
			   procStruct->pronargs * sizeof(Oid));

		/* resolve any polymorphic argument types */
		plpgsql_resolve_polymorphic_argtypes(procStruct->pronargs,
											 hashkey->argtypes,
											 NULL,
											 fcinfo->flinfo->fn_expr,
											 forValidator,
											 NameStr(procStruct->proname));
	}
}

/*
 * This is the same as the standard resolve_polymorphic_argtypes() function,
 * but with a special case for validation: assume that polymorphic arguments
 * are integer or integer-array.  Also, we go ahead and report the error
 * if we can't resolve the types.
 */
static void
plpgsql_resolve_polymorphic_argtypes(int numargs,
									 Oid *argtypes, char *argmodes,
									 Node *call_expr, bool forValidator,
									 const char *proname)
{
	int			i;

	if (!forValidator)
	{
		/* normal case, pass to standard routine */
		if (!resolve_polymorphic_argtypes(numargs, argtypes, argmodes,
										  call_expr))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine actual argument "
							"type for polymorphic function \"%s\"",
							proname)));
	}
	else
	{
		/* special validation case */
		for (i = 0; i < numargs; i++)
		{
			switch (argtypes[i])
			{
				case ANYELEMENTOID:
					argtypes[i] = INT4OID;
					break;
				case ANYARRAYOID:
					argtypes[i] = INT4ARRAYOID;
					break;
				default:
					break;
			}
		}
	}
}

/*
 * delete_function - clean up as much as possible of a stale function cache
 *
 * We can't release the PLpgSQL_function struct itself, because of the
 * possibility that there are fn_extra pointers to it.  We can release
 * the subsidiary storage, but only if there are no active evaluations
 * in progress.  Otherwise we'll just leak that storage.  Since the
 * case would only occur if a pg_proc update is detected during a nested
 * recursive call on the function, a leak seems acceptable.
 *
 * Note that this can be called more than once if there are multiple fn_extra
 * pointers to the same function cache.  Hence be careful not to do things
 * twice.
 */
static void
delete_function(PLpgSQL_function *func)
{
	/* remove function from hash table (might be done already) */
	plpgsql_HashTableDelete(func);

	/* release the function's storage if safe and not done already */
	if (func->use_count == 0 && func->fn_cxt)
	{
		MemoryContextDelete(func->fn_cxt);
		func->fn_cxt = NULL;
	}
}

/* exported so we can call it from plpgsql_init() */
void
plpgsql_HashTableInit(void)
{
	HASHCTL		ctl;

	/* don't allow double-initialization */
	Assert(plpgsql_HashTable == NULL);

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

	/* do nothing if not in table */
	if (function->fn_hashkey == NULL)
		return;

	hentry = (plpgsql_HashEnt *) hash_search(plpgsql_HashTable,
											 (void *) function->fn_hashkey,
											 HASH_REMOVE,
											 NULL);
	if (hentry == NULL)
		elog(WARNING, "trying to delete function that does not exist");

	/* remove back link, which no longer points to allocated storage */
	function->fn_hashkey = NULL;
}
