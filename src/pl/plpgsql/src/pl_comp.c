/*-------------------------------------------------------------------------
 *
 * pl_comp.c		- Compiler part of the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_comp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "plpgsql.h"


/* ----------
 * Our own local and global variables
 * ----------
 */
PLpgSQL_stmt_block *plpgsql_parse_result;

static int	datums_alloc;
int			plpgsql_nDatums;
PLpgSQL_datum **plpgsql_Datums;
static int	datums_last;

char	   *plpgsql_error_funcname;
bool		plpgsql_DumpExecTree = false;
bool		plpgsql_check_syntax = false;

PLpgSQL_function *plpgsql_curr_compile;

/* A context appropriate for short-term allocs during compilation */
MemoryContext plpgsql_compile_tmp_cxt;

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
#include "plerrcodes.h"			/* pgrminclude ignore */
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
static void plpgsql_compile_error_callback(void *arg);
static void add_parameter_name(PLpgSQL_nsitem_type itemtype, int itemno, const char *name);
static void add_dummy_return(PLpgSQL_function *function);
static Node *plpgsql_pre_column_ref(ParseState *pstate, ColumnRef *cref);
static Node *plpgsql_post_column_ref(ParseState *pstate, ColumnRef *cref, Node *var);
static Node *plpgsql_param_ref(ParseState *pstate, ParamRef *pref);
static Node *resolve_column_ref(ParseState *pstate, PLpgSQL_expr *expr,
								ColumnRef *cref, bool error_if_no_field);
static Node *make_datum_param(PLpgSQL_expr *expr, int dno, int location);
static PLpgSQL_row *build_row_from_vars(PLpgSQL_variable **vars, int numvars);
static PLpgSQL_type *build_datatype(HeapTuple typeTup, int32 typmod,
									Oid collation, TypeName *origtypname);
static void plpgsql_start_datums(void);
static void plpgsql_finish_datums(PLpgSQL_function *function);
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
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
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
		if (function->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
			ItemPointerEquals(&function->fn_tid, &procTup->t_self))
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
			 * a replacement has already been made, so go back and recheck the
			 * hashtable.
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
 * allocations is the plpgsql_compile_tmp_cxt.
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
	bool		is_dml_trigger = CALLED_AS_TRIGGER(fcinfo);
	bool		is_event_trigger = CALLED_AS_EVENT_TRIGGER(fcinfo);
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
	 * Setup the scanner input and error info.  We assume that this function
	 * cannot be invoked recursively, so there's no need to save and restore
	 * the static variables used here.
	 */
	prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");
	proc_source = TextDatumGetCString(prosrcdatum);
	plpgsql_scanner_init(proc_source);

	plpgsql_error_funcname = pstrdup(NameStr(procStruct->proname));

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_compile_error_callback;
	plerrcontext.arg = forValidator ? proc_source : NULL;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

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
	 * All the permanent output of compilation (e.g. parse tree) is kept in a
	 * per-function memory context, so it can be reclaimed easily.
	 */
	func_cxt = AllocSetContextCreate(TopMemoryContext,
									 "PL/pgSQL function",
									 ALLOCSET_DEFAULT_SIZES);
	plpgsql_compile_tmp_cxt = MemoryContextSwitchTo(func_cxt);

	function->fn_signature = format_procedure(fcinfo->flinfo->fn_oid);
	MemoryContextSetIdentifier(func_cxt, function->fn_signature);
	function->fn_oid = fcinfo->flinfo->fn_oid;
	function->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
	function->fn_tid = procTup->t_self;
	function->fn_input_collation = fcinfo->fncollation;
	function->fn_cxt = func_cxt;
	function->out_param_varno = -1; /* set up for no OUT param */
	function->resolve_option = plpgsql_variable_conflict;
	function->print_strict_params = plpgsql_print_strict_params;
	/* only promote extra warnings and errors at CREATE FUNCTION time */
	function->extra_warnings = forValidator ? plpgsql_extra_warnings : 0;
	function->extra_errors = forValidator ? plpgsql_extra_errors : 0;

	if (is_dml_trigger)
		function->fn_is_trigger = PLPGSQL_DML_TRIGGER;
	else if (is_event_trigger)
		function->fn_is_trigger = PLPGSQL_EVENT_TRIGGER;
	else
		function->fn_is_trigger = PLPGSQL_NOT_TRIGGER;

	function->fn_prokind = procStruct->prokind;

	function->nstatements = 0;

	/*
	 * Initialize the compiler, particularly the namespace stack.  The
	 * outermost namespace contains function parameters and other special
	 * variables (such as FOUND), and is named after the function itself.
	 */
	plpgsql_ns_init();
	plpgsql_ns_push(NameStr(procStruct->proname), PLPGSQL_LABEL_BLOCK);
	plpgsql_DumpExecTree = false;
	plpgsql_start_datums();

	switch (function->fn_is_trigger)
	{
		case PLPGSQL_NOT_TRIGGER:

			/*
			 * Fetch info about the procedure's parameters. Allocations aren't
			 * needed permanently, so make them in tmp cxt.
			 *
			 * We also need to resolve any polymorphic input or output
			 * argument types.  In validation mode we won't be able to, so we
			 * arbitrarily assume we are dealing with integers.
			 */
			MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

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
				PLpgSQL_nsitem_type argitemtype;

				/* Create $n name for variable */
				snprintf(buf, sizeof(buf), "$%d", i + 1);

				/* Create datatype info */
				argdtype = plpgsql_build_datatype(argtypeid,
												  -1,
												  function->fn_input_collation,
												  NULL);

				/* Disallow pseudotype argument */
				/* (note we already replaced polymorphic types) */
				/* (build_variable would do this, but wrong message) */
				if (argdtype->ttype == PLPGSQL_TTYPE_PSEUDO)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/pgSQL functions cannot accept type %s",
									format_type_be(argtypeid))));

				/*
				 * Build variable and add to datum list.  If there's a name
				 * for the argument, use that as refname, else use $n name.
				 */
				argvariable = plpgsql_build_variable((argnames &&
													  argnames[i][0] != '\0') ?
													 argnames[i] : buf,
													 0, argdtype, false);

				if (argvariable->dtype == PLPGSQL_DTYPE_VAR)
				{
					argitemtype = PLPGSQL_NSTYPE_VAR;
				}
				else
				{
					Assert(argvariable->dtype == PLPGSQL_DTYPE_REC);
					argitemtype = PLPGSQL_NSTYPE_REC;
				}

				/* Remember arguments in appropriate arrays */
				if (argmode == PROARGMODE_IN ||
					argmode == PROARGMODE_INOUT ||
					argmode == PROARGMODE_VARIADIC)
					in_arg_varnos[num_in_args++] = argvariable->dno;
				if (argmode == PROARGMODE_OUT ||
					argmode == PROARGMODE_INOUT ||
					argmode == PROARGMODE_TABLE)
					out_arg_variables[num_out_args++] = argvariable;

				/* Add to namespace under the $n name */
				add_parameter_name(argitemtype, argvariable->dno, buf);

				/* If there's a name for the argument, make an alias */
				if (argnames && argnames[i][0] != '\0')
					add_parameter_name(argitemtype, argvariable->dno,
									   argnames[i]);
			}

			/*
			 * If there's just one OUT parameter, out_param_varno points
			 * directly to it.  If there's more than one, build a row that
			 * holds all of them.  Procedures return a row even for one OUT
			 * parameter.
			 */
			if (num_out_args > 1 ||
				(num_out_args == 1 && function->fn_prokind == PROKIND_PROCEDURE))
			{
				PLpgSQL_row *row = build_row_from_vars(out_arg_variables,
													   num_out_args);

				plpgsql_adddatum((PLpgSQL_datum *) row);
				function->out_param_varno = row->dno;
			}
			else if (num_out_args == 1)
				function->out_param_varno = out_arg_variables[0]->dno;

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
			if (IsPolymorphicType(rettypeid))
			{
				if (forValidator)
				{
					if (rettypeid == ANYARRAYOID)
						rettypeid = INT4ARRAYOID;
					else if (rettypeid == ANYRANGEOID)
						rettypeid = INT4RANGEOID;
					else		/* ANYELEMENT or ANYNONARRAY */
						rettypeid = INT4OID;
					/* XXX what could we use for ANYENUM? */
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
			typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(rettypeid));
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "cache lookup failed for type %u", rettypeid);
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID or RECORD */
			/* (note we already replaced polymorphic types) */
			if (typeStruct->typtype == TYPTYPE_PSEUDO)
			{
				if (rettypeid == VOIDOID ||
					rettypeid == RECORDOID)
					 /* okay */ ;
				else if (rettypeid == TRIGGEROID || rettypeid == EVTTRIGGEROID)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions can only be called as triggers")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("PL/pgSQL functions cannot return type %s",
									format_type_be(rettypeid))));
			}

			function->fn_retistuple = type_is_rowtype(rettypeid);
			function->fn_retisdomain = (typeStruct->typtype == TYPTYPE_DOMAIN);
			function->fn_retbyval = typeStruct->typbyval;
			function->fn_rettyplen = typeStruct->typlen;

			/*
			 * install $0 reference, but only for polymorphic return types,
			 * and not when the return is specified through an output
			 * parameter.
			 */
			if (IsPolymorphicType(procStruct->prorettype) &&
				num_out_args == 0)
			{
				(void) plpgsql_build_variable("$0", 0,
											  build_datatype(typeTup,
															 -1,
															 function->fn_input_collation,
															 NULL),
											  true);
			}

			ReleaseSysCache(typeTup);
			break;

		case PLPGSQL_DML_TRIGGER:
			/* Trigger procedure's return type is unknown yet */
			function->fn_rettype = InvalidOid;
			function->fn_retbyval = false;
			function->fn_retistuple = true;
			function->fn_retisdomain = false;
			function->fn_retset = false;

			/* shouldn't be any declared arguments */
			if (procStruct->pronargs != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("trigger functions cannot have declared arguments"),
						 errhint("The arguments of the trigger can be accessed through TG_NARGS and TG_ARGV instead.")));

			/* Add the record for referencing NEW ROW */
			rec = plpgsql_build_record("new", 0, NULL, RECORDOID, true);
			function->new_varno = rec->dno;

			/* Add the record for referencing OLD ROW */
			rec = plpgsql_build_record("old", 0, NULL, RECORDOID, true);
			function->old_varno = rec->dno;

			/* Add the variable tg_name */
			var = plpgsql_build_variable("tg_name", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_NAME;

			/* Add the variable tg_when */
			var = plpgsql_build_variable("tg_when", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_WHEN;

			/* Add the variable tg_level */
			var = plpgsql_build_variable("tg_level", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_LEVEL;

			/* Add the variable tg_op */
			var = plpgsql_build_variable("tg_op", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_OP;

			/* Add the variable tg_relid */
			var = plpgsql_build_variable("tg_relid", 0,
										 plpgsql_build_datatype(OIDOID,
																-1,
																InvalidOid,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_RELID;

			/* Add the variable tg_relname */
			var = plpgsql_build_variable("tg_relname", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TABLE_NAME;

			/* tg_table_name is now preferred to tg_relname */
			var = plpgsql_build_variable("tg_table_name", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TABLE_NAME;

			/* add the variable tg_table_schema */
			var = plpgsql_build_variable("tg_table_schema", 0,
										 plpgsql_build_datatype(NAMEOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TABLE_SCHEMA;

			/* Add the variable tg_nargs */
			var = plpgsql_build_variable("tg_nargs", 0,
										 plpgsql_build_datatype(INT4OID,
																-1,
																InvalidOid,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_NARGS;

			/* Add the variable tg_argv */
			var = plpgsql_build_variable("tg_argv", 0,
										 plpgsql_build_datatype(TEXTARRAYOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_ARGV;

			break;

		case PLPGSQL_EVENT_TRIGGER:
			function->fn_rettype = VOIDOID;
			function->fn_retbyval = false;
			function->fn_retistuple = true;
			function->fn_retisdomain = false;
			function->fn_retset = false;

			/* shouldn't be any declared arguments */
			if (procStruct->pronargs != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("event trigger functions cannot have declared arguments")));

			/* Add the variable tg_event */
			var = plpgsql_build_variable("tg_event", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_EVENT;

			/* Add the variable tg_tag */
			var = plpgsql_build_variable("tg_tag", 0,
										 plpgsql_build_datatype(TEXTOID,
																-1,
																function->fn_input_collation,
																NULL),
										 true);
			Assert(var->dtype == PLPGSQL_DTYPE_VAR);
			var->dtype = PLPGSQL_DTYPE_PROMISE;
			((PLpgSQL_var *) var)->promise = PLPGSQL_PROMISE_TG_TAG;

			break;

		default:
			elog(ERROR, "unrecognized function typecode: %d",
				 (int) function->fn_is_trigger);
			break;
	}

	/* Remember if function is STABLE/IMMUTABLE */
	function->fn_readonly = (procStruct->provolatile != PROVOLATILE_VOLATILE);

	/*
	 * Create the magic FOUND variable.
	 */
	var = plpgsql_build_variable("found", 0,
								 plpgsql_build_datatype(BOOLOID,
														-1,
														InvalidOid,
														NULL),
								 true);
	function->found_varno = var->dno;

	/*
	 * Now parse the function's text
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql parser returned %d", parse_rc);
	function->action = plpgsql_parse_result;

	plpgsql_scanner_finish();
	pfree(proc_source);

	/*
	 * If it has OUT parameters or returns VOID or returns a set, we allow
	 * control to fall off the end without an explicit RETURN statement. The
	 * easiest way to implement this is to add a RETURN statement to the end
	 * of the statement list during parsing.
	 */
	if (num_out_args > 0 || function->fn_rettype == VOIDOID ||
		function->fn_retset)
		add_dummy_return(function);

	/*
	 * Complete the function's info
	 */
	function->fn_nargs = procStruct->pronargs;
	for (i = 0; i < function->fn_nargs; i++)
		function->fn_argvarnos[i] = in_arg_varnos[i];

	plpgsql_finish_datums(function);

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

	plpgsql_check_syntax = false;

	MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);
	plpgsql_compile_tmp_cxt = NULL;
	return function;
}

/* ----------
 * plpgsql_compile_inline	Make an execution tree for an anonymous code block.
 *
 * Note: this is generally parallel to do_compile(); is it worth trying to
 * merge the two?
 *
 * Note: we assume the block will be thrown away so there is no need to build
 * persistent data structures.
 * ----------
 */
PLpgSQL_function *
plpgsql_compile_inline(char *proc_source)
{
	char	   *func_name = "inline_code_block";
	PLpgSQL_function *function;
	ErrorContextCallback plerrcontext;
	PLpgSQL_variable *var;
	int			parse_rc;
	MemoryContext func_cxt;

	/*
	 * Setup the scanner input and error info.  We assume that this function
	 * cannot be invoked recursively, so there's no need to save and restore
	 * the static variables used here.
	 */
	plpgsql_scanner_init(proc_source);

	plpgsql_error_funcname = func_name;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_compile_error_callback;
	plerrcontext.arg = proc_source;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/* Do extra syntax checking if check_function_bodies is on */
	plpgsql_check_syntax = check_function_bodies;

	/* Function struct does not live past current statement */
	function = (PLpgSQL_function *) palloc0(sizeof(PLpgSQL_function));

	plpgsql_curr_compile = function;

	/*
	 * All the rest of the compile-time storage (e.g. parse tree) is kept in
	 * its own memory context, so it can be reclaimed easily.
	 */
	func_cxt = AllocSetContextCreate(CurrentMemoryContext,
									 "PL/pgSQL inline code context",
									 ALLOCSET_DEFAULT_SIZES);
	plpgsql_compile_tmp_cxt = MemoryContextSwitchTo(func_cxt);

	function->fn_signature = pstrdup(func_name);
	function->fn_is_trigger = PLPGSQL_NOT_TRIGGER;
	function->fn_input_collation = InvalidOid;
	function->fn_cxt = func_cxt;
	function->out_param_varno = -1; /* set up for no OUT param */
	function->resolve_option = plpgsql_variable_conflict;
	function->print_strict_params = plpgsql_print_strict_params;

	/*
	 * don't do extra validation for inline code as we don't want to add spam
	 * at runtime
	 */
	function->extra_warnings = 0;
	function->extra_errors = 0;

	function->nstatements = 0;

	plpgsql_ns_init();
	plpgsql_ns_push(func_name, PLPGSQL_LABEL_BLOCK);
	plpgsql_DumpExecTree = false;
	plpgsql_start_datums();

	/* Set up as though in a function returning VOID */
	function->fn_rettype = VOIDOID;
	function->fn_retset = false;
	function->fn_retistuple = false;
	function->fn_retisdomain = false;
	function->fn_prokind = PROKIND_FUNCTION;
	/* a bit of hardwired knowledge about type VOID here */
	function->fn_retbyval = true;
	function->fn_rettyplen = sizeof(int32);

	/*
	 * Remember if function is STABLE/IMMUTABLE.  XXX would it be better to
	 * set this true inside a read-only transaction?  Not clear.
	 */
	function->fn_readonly = false;

	/*
	 * Create the magic FOUND variable.
	 */
	var = plpgsql_build_variable("found", 0,
								 plpgsql_build_datatype(BOOLOID,
														-1,
														InvalidOid,
														NULL),
								 true);
	function->found_varno = var->dno;

	/*
	 * Now parse the function's text
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql parser returned %d", parse_rc);
	function->action = plpgsql_parse_result;

	plpgsql_scanner_finish();

	/*
	 * If it returns VOID (always true at the moment), we allow control to
	 * fall off the end without an explicit RETURN statement.
	 */
	if (function->fn_rettype == VOIDOID)
		add_dummy_return(function);

	/*
	 * Complete the function's info
	 */
	function->fn_nargs = 0;

	plpgsql_finish_datums(function);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;
	plpgsql_error_funcname = NULL;

	plpgsql_check_syntax = false;

	MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);
	plpgsql_compile_tmp_cxt = NULL;
	return function;
}


/*
 * error context callback to let us supply a call-stack traceback.
 * If we are validating or executing an anonymous code block, the function
 * source text is passed as an argument.
 */
static void
plpgsql_compile_error_callback(void *arg)
{
	if (arg)
	{
		/*
		 * Try to convert syntax error position to reference text of original
		 * CREATE FUNCTION or DO command.
		 */
		if (function_parse_error_transpose((const char *) arg))
			return;

		/*
		 * Done if a syntax error position was reported; otherwise we have to
		 * fall back to a "near line N" report.
		 */
	}

	if (plpgsql_error_funcname)
		errcontext("compilation of PL/pgSQL function \"%s\" near line %d",
				   plpgsql_error_funcname, plpgsql_latest_lineno());
}


/*
 * Add a name for a function parameter to the function's namespace
 */
static void
add_parameter_name(PLpgSQL_nsitem_type itemtype, int itemno, const char *name)
{
	/*
	 * Before adding the name, check for duplicates.  We need this even though
	 * functioncmds.c has a similar check, because that code explicitly
	 * doesn't complain about conflicting IN and OUT parameter names.  In
	 * plpgsql, such names are in the same namespace, so there is no way to
	 * disambiguate.
	 */
	if (plpgsql_ns_lookup(plpgsql_ns_top(), true,
						  name, NULL, NULL,
						  NULL) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("parameter name \"%s\" used more than once",
						name)));

	/* OK, add the name */
	plpgsql_ns_additem(itemtype, itemno, name);
}

/*
 * Add a dummy RETURN statement to the given function's body
 */
static void
add_dummy_return(PLpgSQL_function *function)
{
	/*
	 * If the outer block has an EXCEPTION clause, we need to make a new outer
	 * block, since the added RETURN shouldn't act like it is inside the
	 * EXCEPTION clause.  Likewise, if it has a label, wrap it in a new outer
	 * block so that EXIT doesn't skip the RETURN.
	 */
	if (function->action->exceptions != NULL ||
		function->action->label != NULL)
	{
		PLpgSQL_stmt_block *new;

		new = palloc0(sizeof(PLpgSQL_stmt_block));
		new->cmd_type = PLPGSQL_STMT_BLOCK;
		new->stmtid = ++function->nstatements;
		new->body = list_make1(function->action);

		function->action = new;
	}
	if (function->action->body == NIL ||
		((PLpgSQL_stmt *) llast(function->action->body))->cmd_type != PLPGSQL_STMT_RETURN)
	{
		PLpgSQL_stmt_return *new;

		new = palloc0(sizeof(PLpgSQL_stmt_return));
		new->cmd_type = PLPGSQL_STMT_RETURN;
		new->stmtid = ++function->nstatements;
		new->expr = NULL;
		new->retvarno = function->out_param_varno;

		function->action->body = lappend(function->action->body, new);
	}
}


/*
 * plpgsql_parser_setup		set up parser hooks for dynamic parameters
 *
 * Note: this routine, and the hook functions it prepares for, are logically
 * part of plpgsql parsing.  But they actually run during function execution,
 * when we are ready to evaluate a SQL query or expression that has not
 * previously been parsed and planned.
 */
void
plpgsql_parser_setup(struct ParseState *pstate, PLpgSQL_expr *expr)
{
	pstate->p_pre_columnref_hook = plpgsql_pre_column_ref;
	pstate->p_post_columnref_hook = plpgsql_post_column_ref;
	pstate->p_paramref_hook = plpgsql_param_ref;
	/* no need to use p_coerce_param_hook */
	pstate->p_ref_hook_state = (void *) expr;
}

/*
 * plpgsql_pre_column_ref		parser callback before parsing a ColumnRef
 */
static Node *
plpgsql_pre_column_ref(ParseState *pstate, ColumnRef *cref)
{
	PLpgSQL_expr *expr = (PLpgSQL_expr *) pstate->p_ref_hook_state;

	if (expr->func->resolve_option == PLPGSQL_RESOLVE_VARIABLE)
		return resolve_column_ref(pstate, expr, cref, false);
	else
		return NULL;
}

/*
 * plpgsql_post_column_ref		parser callback after parsing a ColumnRef
 */
static Node *
plpgsql_post_column_ref(ParseState *pstate, ColumnRef *cref, Node *var)
{
	PLpgSQL_expr *expr = (PLpgSQL_expr *) pstate->p_ref_hook_state;
	Node	   *myvar;

	if (expr->func->resolve_option == PLPGSQL_RESOLVE_VARIABLE)
		return NULL;			/* we already found there's no match */

	if (expr->func->resolve_option == PLPGSQL_RESOLVE_COLUMN && var != NULL)
		return NULL;			/* there's a table column, prefer that */

	/*
	 * If we find a record/row variable but can't match a field name, throw
	 * error if there was no core resolution for the ColumnRef either.  In
	 * that situation, the reference is inevitably going to fail, and
	 * complaining about the record/row variable is likely to be more on-point
	 * than the core parser's error message.  (It's too bad we don't have
	 * access to transformColumnRef's internal crerr state here, as in case of
	 * a conflict with a table name this could still be less than the most
	 * helpful error message possible.)
	 */
	myvar = resolve_column_ref(pstate, expr, cref, (var == NULL));

	if (myvar != NULL && var != NULL)
	{
		/*
		 * We could leave it to the core parser to throw this error, but we
		 * can add a more useful detail message than the core could.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_COLUMN),
				 errmsg("column reference \"%s\" is ambiguous",
						NameListToString(cref->fields)),
				 errdetail("It could refer to either a PL/pgSQL variable or a table column."),
				 parser_errposition(pstate, cref->location)));
	}

	return myvar;
}

/*
 * plpgsql_param_ref		parser callback for ParamRefs ($n symbols)
 */
static Node *
plpgsql_param_ref(ParseState *pstate, ParamRef *pref)
{
	PLpgSQL_expr *expr = (PLpgSQL_expr *) pstate->p_ref_hook_state;
	char		pname[32];
	PLpgSQL_nsitem *nse;

	snprintf(pname, sizeof(pname), "$%d", pref->number);

	nse = plpgsql_ns_lookup(expr->ns, false,
							pname, NULL, NULL,
							NULL);

	if (nse == NULL)
		return NULL;			/* name not known to plpgsql */

	return make_datum_param(expr, nse->itemno, pref->location);
}

/*
 * resolve_column_ref		attempt to resolve a ColumnRef as a plpgsql var
 *
 * Returns the translated node structure, or NULL if name not found
 *
 * error_if_no_field tells whether to throw error or quietly return NULL if
 * we are able to match a record/row name but don't find a field name match.
 */
static Node *
resolve_column_ref(ParseState *pstate, PLpgSQL_expr *expr,
				   ColumnRef *cref, bool error_if_no_field)
{
	PLpgSQL_execstate *estate;
	PLpgSQL_nsitem *nse;
	const char *name1;
	const char *name2 = NULL;
	const char *name3 = NULL;
	const char *colname = NULL;
	int			nnames;
	int			nnames_scalar = 0;
	int			nnames_wholerow = 0;
	int			nnames_field = 0;

	/*
	 * We use the function's current estate to resolve parameter data types.
	 * This is really pretty bogus because there is no provision for updating
	 * plans when those types change ...
	 */
	estate = expr->func->cur_estate;

	/*----------
	 * The allowed syntaxes are:
	 *
	 * A		Scalar variable reference, or whole-row record reference.
	 * A.B		Qualified scalar or whole-row reference, or field reference.
	 * A.B.C	Qualified record field reference.
	 * A.*		Whole-row record reference.
	 * A.B.*	Qualified whole-row record reference.
	 *----------
	 */
	switch (list_length(cref->fields))
	{
		case 1:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);

				Assert(IsA(field1, String));
				name1 = strVal(field1);
				nnames_scalar = 1;
				nnames_wholerow = 1;
				break;
			}
		case 2:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);

				Assert(IsA(field1, String));
				name1 = strVal(field1);

				/* Whole-row reference? */
				if (IsA(field2, A_Star))
				{
					/* Set name2 to prevent matches to scalar variables */
					name2 = "*";
					nnames_wholerow = 1;
					break;
				}

				Assert(IsA(field2, String));
				name2 = strVal(field2);
				colname = name2;
				nnames_scalar = 2;
				nnames_wholerow = 2;
				nnames_field = 1;
				break;
			}
		case 3:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);
				Node	   *field3 = (Node *) lthird(cref->fields);

				Assert(IsA(field1, String));
				name1 = strVal(field1);
				Assert(IsA(field2, String));
				name2 = strVal(field2);

				/* Whole-row reference? */
				if (IsA(field3, A_Star))
				{
					/* Set name3 to prevent matches to scalar variables */
					name3 = "*";
					nnames_wholerow = 2;
					break;
				}

				Assert(IsA(field3, String));
				name3 = strVal(field3);
				colname = name3;
				nnames_field = 2;
				break;
			}
		default:
			/* too many names, ignore */
			return NULL;
	}

	nse = plpgsql_ns_lookup(expr->ns, false,
							name1, name2, name3,
							&nnames);

	if (nse == NULL)
		return NULL;			/* name not known to plpgsql */

	switch (nse->itemtype)
	{
		case PLPGSQL_NSTYPE_VAR:
			if (nnames == nnames_scalar)
				return make_datum_param(expr, nse->itemno, cref->location);
			break;
		case PLPGSQL_NSTYPE_REC:
			if (nnames == nnames_wholerow)
				return make_datum_param(expr, nse->itemno, cref->location);
			if (nnames == nnames_field)
			{
				/* colname could be a field in this record */
				PLpgSQL_rec *rec = (PLpgSQL_rec *) estate->datums[nse->itemno];
				int			i;

				/* search for a datum referencing this field */
				i = rec->firstfield;
				while (i >= 0)
				{
					PLpgSQL_recfield *fld = (PLpgSQL_recfield *) estate->datums[i];

					Assert(fld->dtype == PLPGSQL_DTYPE_RECFIELD &&
						   fld->recparentno == nse->itemno);
					if (strcmp(fld->fieldname, colname) == 0)
					{
						return make_datum_param(expr, i, cref->location);
					}
					i = fld->nextfield;
				}

				/*
				 * We should not get here, because a RECFIELD datum should
				 * have been built at parse time for every possible qualified
				 * reference to fields of this record.  But if we do, handle
				 * it like field-not-found: throw error or return NULL.
				 */
				if (error_if_no_field)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("record \"%s\" has no field \"%s\"",
									(nnames_field == 1) ? name1 : name2,
									colname),
							 parser_errposition(pstate, cref->location)));
			}
			break;
		default:
			elog(ERROR, "unrecognized plpgsql itemtype: %d", nse->itemtype);
	}

	/* Name format doesn't match the plpgsql variable type */
	return NULL;
}

/*
 * Helper for columnref parsing: build a Param referencing a plpgsql datum,
 * and make sure that that datum is listed in the expression's paramnos.
 */
static Node *
make_datum_param(PLpgSQL_expr *expr, int dno, int location)
{
	PLpgSQL_execstate *estate;
	PLpgSQL_datum *datum;
	Param	   *param;
	MemoryContext oldcontext;

	/* see comment in resolve_column_ref */
	estate = expr->func->cur_estate;
	Assert(dno >= 0 && dno < estate->ndatums);
	datum = estate->datums[dno];

	/*
	 * Bitmapset must be allocated in function's permanent memory context
	 */
	oldcontext = MemoryContextSwitchTo(expr->func->fn_cxt);
	expr->paramnos = bms_add_member(expr->paramnos, dno);
	MemoryContextSwitchTo(oldcontext);

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = dno + 1;
	plpgsql_exec_get_datum_type_info(estate,
									 datum,
									 &param->paramtype,
									 &param->paramtypmod,
									 &param->paramcollid);
	param->location = location;

	return (Node *) param;
}


/* ----------
 * plpgsql_parse_word		The scanner calls this to postparse
 *				any single word that is not a reserved keyword.
 *
 * word1 is the downcased/dequoted identifier; it must be palloc'd in the
 * function's long-term memory context.
 *
 * yytxt is the original token text; we need this to check for quoting,
 * so that later checks for unreserved keywords work properly.
 *
 * We attempt to recognize the token as a variable only if lookup is true
 * and the plpgsql_IdentifierLookup context permits it.
 *
 * If recognized as a variable, fill in *wdatum and return true;
 * if not recognized, fill in *word and return false.
 * (Note: those two pointers actually point to members of the same union,
 * but for notational reasons we pass them separately.)
 * ----------
 */
bool
plpgsql_parse_word(char *word1, const char *yytxt, bool lookup,
				   PLwdatum *wdatum, PLword *word)
{
	PLpgSQL_nsitem *ns;

	/*
	 * We should not lookup variables in DECLARE sections.  In SQL
	 * expressions, there's no need to do so either --- lookup will happen
	 * when the expression is compiled.
	 */
	if (lookup && plpgsql_IdentifierLookup == IDENTIFIER_LOOKUP_NORMAL)
	{
		/*
		 * Do a lookup in the current namespace stack
		 */
		ns = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							   word1, NULL, NULL,
							   NULL);

		if (ns != NULL)
		{
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_VAR:
				case PLPGSQL_NSTYPE_REC:
					wdatum->datum = plpgsql_Datums[ns->itemno];
					wdatum->ident = word1;
					wdatum->quoted = (yytxt[0] == '"');
					wdatum->idents = NIL;
					return true;

				default:
					/* plpgsql_ns_lookup should never return anything else */
					elog(ERROR, "unrecognized plpgsql itemtype: %d",
						 ns->itemtype);
			}
		}
	}

	/*
	 * Nothing found - up to now it's a word without any special meaning for
	 * us.
	 */
	word->ident = word1;
	word->quoted = (yytxt[0] == '"');
	return false;
}


/* ----------
 * plpgsql_parse_dblword		Same lookup for two words
 *					separated by a dot.
 * ----------
 */
bool
plpgsql_parse_dblword(char *word1, char *word2,
					  PLwdatum *wdatum, PLcword *cword)
{
	PLpgSQL_nsitem *ns;
	List	   *idents;
	int			nnames;

	idents = list_make2(makeString(word1),
						makeString(word2));

	/*
	 * We should do nothing in DECLARE sections.  In SQL expressions, we
	 * really only need to make sure that RECFIELD datums are created when
	 * needed.
	 */
	if (plpgsql_IdentifierLookup != IDENTIFIER_LOOKUP_DECLARE)
	{
		/*
		 * Do a lookup in the current namespace stack
		 */
		ns = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							   word1, word2, NULL,
							   &nnames);
		if (ns != NULL)
		{
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_VAR:
					/* Block-qualified reference to scalar variable. */
					wdatum->datum = plpgsql_Datums[ns->itemno];
					wdatum->ident = NULL;
					wdatum->quoted = false; /* not used */
					wdatum->idents = idents;
					return true;

				case PLPGSQL_NSTYPE_REC:
					if (nnames == 1)
					{
						/*
						 * First word is a record name, so second word could
						 * be a field in this record.  We build a RECFIELD
						 * datum whether it is or not --- any error will be
						 * detected later.
						 */
						PLpgSQL_rec *rec;
						PLpgSQL_recfield *new;

						rec = (PLpgSQL_rec *) (plpgsql_Datums[ns->itemno]);
						new = plpgsql_build_recfield(rec, word2);

						wdatum->datum = (PLpgSQL_datum *) new;
					}
					else
					{
						/* Block-qualified reference to record variable. */
						wdatum->datum = plpgsql_Datums[ns->itemno];
					}
					wdatum->ident = NULL;
					wdatum->quoted = false; /* not used */
					wdatum->idents = idents;
					return true;

				default:
					break;
			}
		}
	}

	/* Nothing found */
	cword->idents = idents;
	return false;
}


/* ----------
 * plpgsql_parse_tripword		Same lookup for three words
 *					separated by dots.
 * ----------
 */
bool
plpgsql_parse_tripword(char *word1, char *word2, char *word3,
					   PLwdatum *wdatum, PLcword *cword)
{
	PLpgSQL_nsitem *ns;
	List	   *idents;
	int			nnames;

	idents = list_make3(makeString(word1),
						makeString(word2),
						makeString(word3));

	/*
	 * We should do nothing in DECLARE sections.  In SQL expressions, we
	 * really only need to make sure that RECFIELD datums are created when
	 * needed.
	 */
	if (plpgsql_IdentifierLookup != IDENTIFIER_LOOKUP_DECLARE)
	{
		/*
		 * Do a lookup in the current namespace stack. Must find a qualified
		 * reference, else ignore.
		 */
		ns = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							   word1, word2, word3,
							   &nnames);
		if (ns != NULL && nnames == 2)
		{
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_REC:
					{
						/*
						 * words 1/2 are a record name, so third word could be
						 * a field in this record.
						 */
						PLpgSQL_rec *rec;
						PLpgSQL_recfield *new;

						rec = (PLpgSQL_rec *) (plpgsql_Datums[ns->itemno]);
						new = plpgsql_build_recfield(rec, word3);

						wdatum->datum = (PLpgSQL_datum *) new;
						wdatum->ident = NULL;
						wdatum->quoted = false; /* not used */
						wdatum->idents = idents;
						return true;
					}

				default:
					break;
			}
		}
	}

	/* Nothing found */
	cword->idents = idents;
	return false;
}


/* ----------
 * plpgsql_parse_wordtype	The scanner found word%TYPE. word can be
 *				a variable name or a basetype.
 *
 * Returns datatype struct, or NULL if no match found for word.
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_wordtype(char *ident)
{
	PLpgSQL_type *dtype;
	PLpgSQL_nsitem *nse;
	TypeName   *typeName;
	HeapTuple	typeTup;

	/*
	 * Do a lookup in the current namespace stack
	 */
	nse = plpgsql_ns_lookup(plpgsql_ns_top(), false,
							ident, NULL, NULL,
							NULL);

	if (nse != NULL)
	{
		switch (nse->itemtype)
		{
			case PLPGSQL_NSTYPE_VAR:
				return ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;

				/* XXX perhaps allow REC/ROW here? */

			default:
				return NULL;
		}
	}

	/*
	 * Word wasn't found in the namespace stack. Try to find a data type with
	 * that name, but ignore shell types and complex types.
	 */
	typeName = makeTypeName(ident);
	typeTup = LookupTypeName(NULL, typeName, NULL, false);
	if (typeTup)
	{
		Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

		if (!typeStruct->typisdefined ||
			typeStruct->typrelid != InvalidOid)
		{
			ReleaseSysCache(typeTup);
			return NULL;
		}

		dtype = build_datatype(typeTup, -1,
							   plpgsql_curr_compile->fn_input_collation,
							   typeName);

		ReleaseSysCache(typeTup);
		return dtype;
	}

	/*
	 * Nothing found - up to now it's a word without any special meaning for
	 * us.
	 */
	return NULL;
}


/* ----------
 * plpgsql_parse_cwordtype		Same lookup for compositeword%TYPE
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_cwordtype(List *idents)
{
	PLpgSQL_type *dtype = NULL;
	PLpgSQL_nsitem *nse;
	const char *fldname;
	Oid			classOid;
	HeapTuple	classtup = NULL;
	HeapTuple	attrtup = NULL;
	HeapTuple	typetup = NULL;
	Form_pg_class classStruct;
	Form_pg_attribute attrStruct;
	MemoryContext oldCxt;

	/* Avoid memory leaks in the long-term function context */
	oldCxt = MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

	if (list_length(idents) == 2)
	{
		/*
		 * Do a lookup in the current namespace stack. We don't need to check
		 * number of names matched, because we will only consider scalar
		 * variables.
		 */
		nse = plpgsql_ns_lookup(plpgsql_ns_top(), false,
								strVal(linitial(idents)),
								strVal(lsecond(idents)),
								NULL,
								NULL);

		if (nse != NULL && nse->itemtype == PLPGSQL_NSTYPE_VAR)
		{
			dtype = ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;
			goto done;
		}

		/*
		 * First word could also be a table name
		 */
		classOid = RelnameGetRelid(strVal(linitial(idents)));
		if (!OidIsValid(classOid))
			goto done;
		fldname = strVal(lsecond(idents));
	}
	else if (list_length(idents) == 3)
	{
		RangeVar   *relvar;

		relvar = makeRangeVar(strVal(linitial(idents)),
							  strVal(lsecond(idents)),
							  -1);
		/* Can't lock relation - we might not have privileges. */
		classOid = RangeVarGetRelid(relvar, NoLock, true);
		if (!OidIsValid(classOid))
			goto done;
		fldname = strVal(lthird(idents));
	}
	else
		goto done;

	classtup = SearchSysCache1(RELOID, ObjectIdGetDatum(classOid));
	if (!HeapTupleIsValid(classtup))
		goto done;
	classStruct = (Form_pg_class) GETSTRUCT(classtup);

	/*
	 * It must be a relation, sequence, view, materialized view, composite
	 * type, or foreign table
	 */
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_MATVIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE &&
		classStruct->relkind != RELKIND_FOREIGN_TABLE &&
		classStruct->relkind != RELKIND_PARTITIONED_TABLE)
		goto done;

	/*
	 * Fetch the named table field and its type
	 */
	attrtup = SearchSysCacheAttName(classOid, fldname);
	if (!HeapTupleIsValid(attrtup))
		goto done;
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache1(TYPEOID,
							  ObjectIdGetDatum(attrStruct->atttypid));
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", attrStruct->atttypid);

	/*
	 * Found that - build a compiler type struct in the caller's cxt and
	 * return it.  Note that we treat the type as being found-by-OID; no
	 * attempt to re-look-up the type name will happen during invalidations.
	 */
	MemoryContextSwitchTo(oldCxt);
	dtype = build_datatype(typetup,
						   attrStruct->atttypmod,
						   attrStruct->attcollation,
						   NULL);
	MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

done:
	if (HeapTupleIsValid(classtup))
		ReleaseSysCache(classtup);
	if (HeapTupleIsValid(attrtup))
		ReleaseSysCache(attrtup);
	if (HeapTupleIsValid(typetup))
		ReleaseSysCache(typetup);

	MemoryContextSwitchTo(oldCxt);
	return dtype;
}

/* ----------
 * plpgsql_parse_wordrowtype		Scanner found word%ROWTYPE.
 *					So word must be a table name.
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_wordrowtype(char *ident)
{
	Oid			classOid;

	/*
	 * Look up the relation.  Note that because relation rowtypes have the
	 * same names as their relations, this could be handled as a type lookup
	 * equally well; we use the relation lookup code path only because the
	 * errors thrown here have traditionally referred to relations not types.
	 * But we'll make a TypeName in case we have to do re-look-up of the type.
	 */
	classOid = RelnameGetRelid(ident);
	if (!OidIsValid(classOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" does not exist", ident)));

	/* Build and return the row type struct */
	return plpgsql_build_datatype(get_rel_type_id(classOid), -1, InvalidOid,
								  makeTypeName(ident));
}

/* ----------
 * plpgsql_parse_cwordrowtype		Scanner found compositeword%ROWTYPE.
 *			So word must be a namespace qualified table name.
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_cwordrowtype(List *idents)
{
	Oid			classOid;
	RangeVar   *relvar;
	MemoryContext oldCxt;

	/*
	 * As above, this is a relation lookup but could be a type lookup if we
	 * weren't being backwards-compatible about error wording.
	 */
	if (list_length(idents) != 2)
		return NULL;

	/* Avoid memory leaks in long-term function context */
	oldCxt = MemoryContextSwitchTo(plpgsql_compile_tmp_cxt);

	/* Look up relation name.  Can't lock it - we might not have privileges. */
	relvar = makeRangeVar(strVal(linitial(idents)),
						  strVal(lsecond(idents)),
						  -1);
	classOid = RangeVarGetRelid(relvar, NoLock, false);

	MemoryContextSwitchTo(oldCxt);

	/* Build and return the row type struct */
	return plpgsql_build_datatype(get_rel_type_id(classOid), -1, InvalidOid,
								  makeTypeNameFromNameList(idents));
}

/*
 * plpgsql_build_variable - build a datum-array entry of a given
 * datatype
 *
 * The returned struct may be a PLpgSQL_var or PLpgSQL_rec
 * depending on the given datatype, and is allocated via
 * palloc.  The struct is automatically added to the current datum
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
				/* other fields are left as 0, might be changed by caller */

				/* preset to NULL */
				var->value = 0;
				var->isnull = true;
				var->freeval = false;

				plpgsql_adddatum((PLpgSQL_datum *) var);
				if (add2namespace)
					plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR,
									   var->dno,
									   refname);
				result = (PLpgSQL_variable *) var;
				break;
			}
		case PLPGSQL_TTYPE_REC:
			{
				/* Composite type -- build a record variable */
				PLpgSQL_rec *rec;

				rec = plpgsql_build_record(refname, lineno,
										   dtype, dtype->typoid,
										   add2namespace);
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
 * Build empty named record variable, and optionally add it to namespace
 */
PLpgSQL_rec *
plpgsql_build_record(const char *refname, int lineno,
					 PLpgSQL_type *dtype, Oid rectypeid,
					 bool add2namespace)
{
	PLpgSQL_rec *rec;

	rec = palloc0(sizeof(PLpgSQL_rec));
	rec->dtype = PLPGSQL_DTYPE_REC;
	rec->refname = pstrdup(refname);
	rec->lineno = lineno;
	/* other fields are left as 0, might be changed by caller */
	rec->datatype = dtype;
	rec->rectypeid = rectypeid;
	rec->firstfield = -1;
	rec->erh = NULL;
	plpgsql_adddatum((PLpgSQL_datum *) rec);
	if (add2namespace)
		plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->dno, rec->refname);

	return rec;
}

/*
 * Build a row-variable data structure given the component variables.
 * Include a rowtupdesc, since we will need to materialize the row result.
 */
static PLpgSQL_row *
build_row_from_vars(PLpgSQL_variable **vars, int numvars)
{
	PLpgSQL_row *row;
	int			i;

	row = palloc0(sizeof(PLpgSQL_row));
	row->dtype = PLPGSQL_DTYPE_ROW;
	row->refname = "(unnamed row)";
	row->lineno = -1;
	row->rowtupdesc = CreateTemplateTupleDesc(numvars);
	row->nfields = numvars;
	row->fieldnames = palloc(numvars * sizeof(char *));
	row->varnos = palloc(numvars * sizeof(int));

	for (i = 0; i < numvars; i++)
	{
		PLpgSQL_variable *var = vars[i];
		Oid			typoid;
		int32		typmod;
		Oid			typcoll;

		/* Member vars of a row should never be const */
		Assert(!var->isconst);

		switch (var->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				typoid = ((PLpgSQL_var *) var)->datatype->typoid;
				typmod = ((PLpgSQL_var *) var)->datatype->atttypmod;
				typcoll = ((PLpgSQL_var *) var)->datatype->collation;
				break;

			case PLPGSQL_DTYPE_REC:
				/* shouldn't need to revalidate rectypeid already... */
				typoid = ((PLpgSQL_rec *) var)->rectypeid;
				typmod = -1;	/* don't know typmod, if it's used at all */
				typcoll = InvalidOid;	/* composite types have no collation */
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", var->dtype);
				typoid = InvalidOid;	/* keep compiler quiet */
				typmod = 0;
				typcoll = InvalidOid;
				break;
		}

		row->fieldnames[i] = var->refname;
		row->varnos[i] = var->dno;

		TupleDescInitEntry(row->rowtupdesc, i + 1,
						   var->refname,
						   typoid, typmod,
						   0);
		TupleDescInitEntryCollation(row->rowtupdesc, i + 1, typcoll);
	}

	return row;
}

/*
 * Build a RECFIELD datum for the named field of the specified record variable
 *
 * If there's already such a datum, just return it; we don't need duplicates.
 */
PLpgSQL_recfield *
plpgsql_build_recfield(PLpgSQL_rec *rec, const char *fldname)
{
	PLpgSQL_recfield *recfield;
	int			i;

	/* search for an existing datum referencing this field */
	i = rec->firstfield;
	while (i >= 0)
	{
		PLpgSQL_recfield *fld = (PLpgSQL_recfield *) plpgsql_Datums[i];

		Assert(fld->dtype == PLPGSQL_DTYPE_RECFIELD &&
			   fld->recparentno == rec->dno);
		if (strcmp(fld->fieldname, fldname) == 0)
			return fld;
		i = fld->nextfield;
	}

	/* nope, so make a new one */
	recfield = palloc0(sizeof(PLpgSQL_recfield));
	recfield->dtype = PLPGSQL_DTYPE_RECFIELD;
	recfield->fieldname = pstrdup(fldname);
	recfield->recparentno = rec->dno;
	recfield->rectupledescid = INVALID_TUPLEDESC_IDENTIFIER;

	plpgsql_adddatum((PLpgSQL_datum *) recfield);

	/* now we can link it into the parent's chain */
	recfield->nextfield = rec->firstfield;
	rec->firstfield = recfield->dno;

	return recfield;
}

/*
 * plpgsql_build_datatype
 *		Build PLpgSQL_type struct given type OID, typmod, collation,
 *		and type's parsed name.
 *
 * If collation is not InvalidOid then it overrides the type's default
 * collation.  But collation is ignored if the datatype is non-collatable.
 *
 * origtypname is the parsed form of what the user wrote as the type name.
 * It can be NULL if the type could not be a composite type, or if it was
 * identified by OID to begin with (e.g., it's a function argument type).
 */
PLpgSQL_type *
plpgsql_build_datatype(Oid typeOid, int32 typmod,
					   Oid collation, TypeName *origtypname)
{
	HeapTuple	typeTup;
	PLpgSQL_type *typ;

	typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(typeTup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	typ = build_datatype(typeTup, typmod, collation, origtypname);

	ReleaseSysCache(typeTup);

	return typ;
}

/*
 * Utility subroutine to make a PLpgSQL_type struct given a pg_type entry
 * and additional details (see comments for plpgsql_build_datatype).
 */
static PLpgSQL_type *
build_datatype(HeapTuple typeTup, int32 typmod,
			   Oid collation, TypeName *origtypname)
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
	typ->typoid = typeStruct->oid;
	switch (typeStruct->typtype)
	{
		case TYPTYPE_BASE:
		case TYPTYPE_ENUM:
		case TYPTYPE_RANGE:
			typ->ttype = PLPGSQL_TTYPE_SCALAR;
			break;
		case TYPTYPE_COMPOSITE:
			typ->ttype = PLPGSQL_TTYPE_REC;
			break;
		case TYPTYPE_DOMAIN:
			if (type_is_rowtype(typeStruct->typbasetype))
				typ->ttype = PLPGSQL_TTYPE_REC;
			else
				typ->ttype = PLPGSQL_TTYPE_SCALAR;
			break;
		case TYPTYPE_PSEUDO:
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
	typ->typtype = typeStruct->typtype;
	typ->collation = typeStruct->typcollation;
	if (OidIsValid(collation) && OidIsValid(typ->collation))
		typ->collation = collation;
	/* Detect if type is true array, or domain thereof */
	/* NB: this is only used to decide whether to apply expand_array */
	if (typeStruct->typtype == TYPTYPE_BASE)
	{
		/*
		 * This test should include what get_element_type() checks.  We also
		 * disallow non-toastable array types (i.e. oidvector and int2vector).
		 */
		typ->typisarray = (typeStruct->typlen == -1 &&
						   OidIsValid(typeStruct->typelem) &&
						   typeStruct->typstorage != 'p');
	}
	else if (typeStruct->typtype == TYPTYPE_DOMAIN)
	{
		/* we can short-circuit looking up base types if it's not varlena */
		typ->typisarray = (typeStruct->typlen == -1 &&
						   typeStruct->typstorage != 'p' &&
						   OidIsValid(get_base_element_type(typeStruct->typbasetype)));
	}
	else
		typ->typisarray = false;
	typ->atttypmod = typmod;

	/*
	 * If it's a named composite type (or domain over one), find the typcache
	 * entry and record the current tupdesc ID, so we can detect changes
	 * (including drops).  We don't currently support on-the-fly replacement
	 * of non-composite types, else we might want to do this for them too.
	 */
	if (typ->ttype == PLPGSQL_TTYPE_REC && typ->typoid != RECORDOID)
	{
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(typ->typoid,
									 TYPECACHE_TUPDESC |
									 TYPECACHE_DOMAIN_BASE_INFO);
		if (typentry->typtype == TYPTYPE_DOMAIN)
			typentry = lookup_type_cache(typentry->domainBaseType,
										 TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(typ->typoid))));

		typ->origtypname = origtypname;
		typ->tcache = typentry;
		typ->tupdesc_id = typentry->tupDesc_identifier;
	}
	else
	{
		typ->origtypname = NULL;
		typ->tcache = NULL;
		typ->tupdesc_id = 0;
	}

	return typ;
}

/*
 *	plpgsql_recognize_err_condition
 *		Check condition name and translate it to SQLSTATE.
 *
 * Note: there are some cases where the same condition name has multiple
 * entries in the table.  We arbitrarily return the first match.
 */
int
plpgsql_recognize_err_condition(const char *condname, bool allow_sqlstate)
{
	int			i;

	if (allow_sqlstate)
	{
		if (strlen(condname) == 5 &&
			strspn(condname, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") == 5)
			return MAKE_SQLSTATE(condname[0],
								 condname[1],
								 condname[2],
								 condname[3],
								 condname[4]);
	}

	for (i = 0; exception_label_map[i].label != NULL; i++)
	{
		if (strcmp(condname, exception_label_map[i].label) == 0)
			return exception_label_map[i].sqlerrstate;
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("unrecognized exception condition \"%s\"",
					condname)));
	return 0;					/* keep compiler quiet */
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
 * plpgsql_start_datums			Initialize datum list at compile startup.
 * ----------
 */
static void
plpgsql_start_datums(void)
{
	datums_alloc = 128;
	plpgsql_nDatums = 0;
	/* This is short-lived, so needn't allocate in function's cxt */
	plpgsql_Datums = MemoryContextAlloc(plpgsql_compile_tmp_cxt,
										sizeof(PLpgSQL_datum *) * datums_alloc);
	/* datums_last tracks what's been seen by plpgsql_add_initdatums() */
	datums_last = 0;
}

/* ----------
 * plpgsql_adddatum			Add a variable, record or row
 *					to the compiler's datum list.
 * ----------
 */
void
plpgsql_adddatum(PLpgSQL_datum *newdatum)
{
	if (plpgsql_nDatums == datums_alloc)
	{
		datums_alloc *= 2;
		plpgsql_Datums = repalloc(plpgsql_Datums, sizeof(PLpgSQL_datum *) * datums_alloc);
	}

	newdatum->dno = plpgsql_nDatums;
	plpgsql_Datums[plpgsql_nDatums++] = newdatum;
}

/* ----------
 * plpgsql_finish_datums	Copy completed datum info into function struct.
 * ----------
 */
static void
plpgsql_finish_datums(PLpgSQL_function *function)
{
	Size		copiable_size = 0;
	int			i;

	function->ndatums = plpgsql_nDatums;
	function->datums = palloc(sizeof(PLpgSQL_datum *) * plpgsql_nDatums);
	for (i = 0; i < plpgsql_nDatums; i++)
	{
		function->datums[i] = plpgsql_Datums[i];

		/* This must agree with copy_plpgsql_datums on what is copiable */
		switch (function->datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				copiable_size += MAXALIGN(sizeof(PLpgSQL_var));
				break;
			case PLPGSQL_DTYPE_REC:
				copiable_size += MAXALIGN(sizeof(PLpgSQL_rec));
				break;
			default:
				break;
		}
	}
	function->copiable_size = copiable_size;
}


/* ----------
 * plpgsql_add_initdatums		Make an array of the datum numbers of
 *					all the initializable datums created since the last call
 *					to this function.
 *
 * If varnos is NULL, we just forget any datum entries created since the
 * last call.
 *
 * This is used around a DECLARE section to create a list of the datums
 * that have to be initialized at block entry.  Note that datums can also
 * be created elsewhere than DECLARE, eg by a FOR-loop, but it is then
 * the responsibility of special-purpose code to initialize them.
 * ----------
 */
int
plpgsql_add_initdatums(int **varnos)
{
	int			i;
	int			n = 0;

	/*
	 * The set of dtypes recognized here must match what exec_stmt_block()
	 * cares about (re)initializing at block entry.
	 */
	for (i = datums_last; i < plpgsql_nDatums; i++)
	{
		switch (plpgsql_Datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_REC:
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
					case PLPGSQL_DTYPE_REC:
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
	hashkey->isEventTrigger = CALLED_AS_EVENT_TRIGGER(fcinfo);

	/*
	 * If DML trigger, include trigger's OID in the hash, so that each trigger
	 * usage gets a different hash entry, allowing for e.g. different relation
	 * rowtypes or transition table names.  In validation mode we do not know
	 * what relation or transition table names are intended to be used, so we
	 * leave trigOid zero; the hash entry built in this case will never be
	 * used for any actual calls.
	 *
	 * We don't currently need to distinguish different event trigger usages
	 * in the same way, since the special parameter variables don't vary in
	 * type in that case.
	 */
	if (hashkey->isTrigger && !forValidator)
	{
		TriggerData *trigdata = (TriggerData *) fcinfo->context;

		hashkey->trigOid = trigdata->tg_trigger->tgoid;
	}

	/* get input collation, if known */
	hashkey->inputCollation = fcinfo->fncollation;

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
 * except that:
 * 1. We go ahead and report the error if we can't resolve the types.
 * 2. We treat RECORD-type input arguments (not output arguments) as if
 *    they were polymorphic, replacing their types with the actual input
 *    types if we can determine those.  This allows us to create a separate
 *    function cache entry for each named composite type passed to such an
 *    argument.
 * 3. In validation mode, we have no inputs to look at, so assume that
 *    polymorphic arguments are integer, integer-array or integer-range.
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
		int			inargno;

		/* normal case, pass to standard routine */
		if (!resolve_polymorphic_argtypes(numargs, argtypes, argmodes,
										  call_expr))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine actual argument "
							"type for polymorphic function \"%s\"",
							proname)));
		/* also, treat RECORD inputs (but not outputs) as polymorphic */
		inargno = 0;
		for (i = 0; i < numargs; i++)
		{
			char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

			if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				continue;
			if (argtypes[i] == RECORDOID || argtypes[i] == RECORDARRAYOID)
			{
				Oid			resolvedtype = get_call_expr_argtype(call_expr,
																 inargno);

				if (OidIsValid(resolvedtype))
					argtypes[i] = resolvedtype;
			}
			inargno++;
		}
	}
	else
	{
		/* special validation case (no need to do anything for RECORD) */
		for (i = 0; i < numargs; i++)
		{
			switch (argtypes[i])
			{
				case ANYELEMENTOID:
				case ANYNONARRAYOID:
				case ANYENUMOID:	/* XXX dubious */
					argtypes[i] = INT4OID;
					break;
				case ANYARRAYOID:
					argtypes[i] = INT4ARRAYOID;
					break;
				case ANYRANGEOID:
					argtypes[i] = INT4RANGEOID;
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
	if (func->use_count == 0)
		plpgsql_free_function_memory(func);
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
	plpgsql_HashTable = hash_create("PLpgSQL function hash",
									FUNCS_PER_USER,
									&ctl,
									HASH_ELEM | HASH_BLOBS);
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
