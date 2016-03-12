/*-------------------------------------------------------------------------
 *
 * pl_exec.c		- Executor for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_exec.c
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/parse_coerce.h"
#include "parser/scansup.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"


typedef struct
{
	int			nargs;			/* number of arguments */
	Oid		   *types;			/* types of arguments */
	Datum	   *values;			/* evaluated argument values */
	char	   *nulls;			/* null markers (' '/'n' style) */
	bool	   *freevals;		/* which arguments are pfree-able */
} PreparedParamsData;

/*
 * All plpgsql function executions within a single transaction share the same
 * executor EState for evaluating "simple" expressions.  Each function call
 * creates its own "eval_econtext" ExprContext within this estate for
 * per-evaluation workspace.  eval_econtext is freed at normal function exit,
 * and the EState is freed at transaction end (in case of error, we assume
 * that the abort mechanisms clean it all up).  Furthermore, any exception
 * block within a function has to have its own eval_econtext separate from
 * the containing function's, so that we can clean up ExprContext callbacks
 * properly at subtransaction exit.  We maintain a stack that tracks the
 * individual econtexts so that we can clean up correctly at subxact exit.
 *
 * This arrangement is a bit tedious to maintain, but it's worth the trouble
 * so that we don't have to re-prepare simple expressions on each trip through
 * a function.  (We assume the case to optimize is many repetitions of a
 * function within a transaction.)
 *
 * However, there's no value in trying to amortize simple expression setup
 * across multiple executions of a DO block (inline code block), since there
 * can never be any.  If we use the shared EState for a DO block, the expr
 * state trees are effectively leaked till end of transaction, and that can
 * add up if the user keeps on submitting DO blocks.  Therefore, each DO block
 * has its own simple-expression EState, which is cleaned up at exit from
 * plpgsql_inline_handler().  DO blocks still use the simple_econtext_stack,
 * though, so that subxact abort cleanup does the right thing.
 */
typedef struct SimpleEcontextStackEntry
{
	ExprContext *stack_econtext;	/* a stacked econtext */
	SubTransactionId xact_subxid;		/* ID for current subxact */
	struct SimpleEcontextStackEntry *next;		/* next stack entry up */
} SimpleEcontextStackEntry;

static EState *shared_simple_eval_estate = NULL;
static SimpleEcontextStackEntry *simple_econtext_stack = NULL;

/*
 * We use a session-wide hash table for caching cast information.
 *
 * Once built, the compiled expression trees (cast_expr fields) survive for
 * the life of the session.  At some point it might be worth invalidating
 * those after pg_cast changes, but for the moment we don't bother.
 *
 * The evaluation state trees (cast_exprstate) are managed in the same way as
 * simple expressions (i.e., we assume cast expressions are always simple).
 *
 * As with simple expressions, DO blocks don't use the shared hash table but
 * must have their own.  This isn't ideal, but we don't want to deal with
 * multiple simple_eval_estates within a DO block.
 */
typedef struct					/* lookup key for cast info */
{
	/* NB: we assume this struct contains no padding bytes */
	Oid			srctype;		/* source type for cast */
	Oid			dsttype;		/* destination type for cast */
	int32		srctypmod;		/* source typmod for cast */
	int32		dsttypmod;		/* destination typmod for cast */
} plpgsql_CastHashKey;

typedef struct					/* cast_hash table entry */
{
	plpgsql_CastHashKey key;	/* hash key --- MUST BE FIRST */
	Expr	   *cast_expr;		/* cast expression, or NULL if no-op cast */
	/* The ExprState tree is valid only when cast_lxid matches current LXID */
	ExprState  *cast_exprstate; /* expression's eval tree */
	bool		cast_in_use;	/* true while we're executing eval tree */
	LocalTransactionId cast_lxid;
} plpgsql_CastHashEntry;

static MemoryContext shared_cast_context = NULL;
static HTAB *shared_cast_hash = NULL;

/************************************************************
 * Local function forward declarations
 ************************************************************/
static void plpgsql_exec_error_callback(void *arg);
static PLpgSQL_datum *copy_plpgsql_datum(PLpgSQL_datum *datum);

static int exec_stmt_block(PLpgSQL_execstate *estate,
				PLpgSQL_stmt_block *block);
static int exec_stmts(PLpgSQL_execstate *estate,
		   List *stmts);
static int exec_stmt(PLpgSQL_execstate *estate,
		  PLpgSQL_stmt *stmt);
static int exec_stmt_assign(PLpgSQL_execstate *estate,
				 PLpgSQL_stmt_assign *stmt);
static int exec_stmt_perform(PLpgSQL_execstate *estate,
				  PLpgSQL_stmt_perform *stmt);
static int exec_stmt_getdiag(PLpgSQL_execstate *estate,
				  PLpgSQL_stmt_getdiag *stmt);
static int exec_stmt_if(PLpgSQL_execstate *estate,
			 PLpgSQL_stmt_if *stmt);
static int exec_stmt_case(PLpgSQL_execstate *estate,
			   PLpgSQL_stmt_case *stmt);
static int exec_stmt_loop(PLpgSQL_execstate *estate,
			   PLpgSQL_stmt_loop *stmt);
static int exec_stmt_while(PLpgSQL_execstate *estate,
				PLpgSQL_stmt_while *stmt);
static int exec_stmt_fori(PLpgSQL_execstate *estate,
			   PLpgSQL_stmt_fori *stmt);
static int exec_stmt_fors(PLpgSQL_execstate *estate,
			   PLpgSQL_stmt_fors *stmt);
static int exec_stmt_forc(PLpgSQL_execstate *estate,
			   PLpgSQL_stmt_forc *stmt);
static int exec_stmt_foreach_a(PLpgSQL_execstate *estate,
					PLpgSQL_stmt_foreach_a *stmt);
static int exec_stmt_open(PLpgSQL_execstate *estate,
			   PLpgSQL_stmt_open *stmt);
static int exec_stmt_fetch(PLpgSQL_execstate *estate,
				PLpgSQL_stmt_fetch *stmt);
static int exec_stmt_close(PLpgSQL_execstate *estate,
				PLpgSQL_stmt_close *stmt);
static int exec_stmt_exit(PLpgSQL_execstate *estate,
			   PLpgSQL_stmt_exit *stmt);
static int exec_stmt_return(PLpgSQL_execstate *estate,
				 PLpgSQL_stmt_return *stmt);
static int exec_stmt_return_next(PLpgSQL_execstate *estate,
					  PLpgSQL_stmt_return_next *stmt);
static int exec_stmt_return_query(PLpgSQL_execstate *estate,
					   PLpgSQL_stmt_return_query *stmt);
static int exec_stmt_raise(PLpgSQL_execstate *estate,
				PLpgSQL_stmt_raise *stmt);
static int exec_stmt_assert(PLpgSQL_execstate *estate,
				 PLpgSQL_stmt_assert *stmt);
static int exec_stmt_execsql(PLpgSQL_execstate *estate,
				  PLpgSQL_stmt_execsql *stmt);
static int exec_stmt_dynexecute(PLpgSQL_execstate *estate,
					 PLpgSQL_stmt_dynexecute *stmt);
static int exec_stmt_dynfors(PLpgSQL_execstate *estate,
				  PLpgSQL_stmt_dynfors *stmt);

static void plpgsql_estate_setup(PLpgSQL_execstate *estate,
					 PLpgSQL_function *func,
					 ReturnSetInfo *rsi,
					 EState *simple_eval_estate);
static void exec_eval_cleanup(PLpgSQL_execstate *estate);

static void exec_prepare_plan(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr, int cursorOptions);
static bool exec_simple_check_node(Node *node);
static void exec_simple_check_plan(PLpgSQL_expr *expr);
static void exec_simple_recheck_plan(PLpgSQL_expr *expr, CachedPlan *cplan);
static void exec_check_rw_parameter(PLpgSQL_expr *expr, int target_dno);
static bool contains_target_param(Node *node, int *target_dno);
static bool exec_eval_simple_expr(PLpgSQL_execstate *estate,
					  PLpgSQL_expr *expr,
					  Datum *result,
					  bool *isNull,
					  Oid *rettype,
					  int32 *rettypmod);

static void exec_assign_expr(PLpgSQL_execstate *estate,
				 PLpgSQL_datum *target,
				 PLpgSQL_expr *expr);
static void exec_assign_c_string(PLpgSQL_execstate *estate,
					 PLpgSQL_datum *target,
					 const char *str);
static void exec_assign_value(PLpgSQL_execstate *estate,
				  PLpgSQL_datum *target,
				  Datum value, bool isNull,
				  Oid valtype, int32 valtypmod);
static void exec_eval_datum(PLpgSQL_execstate *estate,
				PLpgSQL_datum *datum,
				Oid *typeid,
				int32 *typetypmod,
				Datum *value,
				bool *isnull);
static int exec_eval_integer(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr,
				  bool *isNull);
static bool exec_eval_boolean(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr,
				  bool *isNull);
static Datum exec_eval_expr(PLpgSQL_execstate *estate,
			   PLpgSQL_expr *expr,
			   bool *isNull,
			   Oid *rettype,
			   int32 *rettypmod);
static int exec_run_select(PLpgSQL_execstate *estate,
				PLpgSQL_expr *expr, long maxtuples, Portal *portalP,
				bool parallelOK);
static int exec_for_query(PLpgSQL_execstate *estate, PLpgSQL_stmt_forq *stmt,
			   Portal portal, bool prefetch_ok);
static ParamListInfo setup_param_list(PLpgSQL_execstate *estate,
				 PLpgSQL_expr *expr);
static ParamListInfo setup_unshared_param_list(PLpgSQL_execstate *estate,
						  PLpgSQL_expr *expr);
static void plpgsql_param_fetch(ParamListInfo params, int paramid);
static void exec_move_row(PLpgSQL_execstate *estate,
			  PLpgSQL_rec *rec,
			  PLpgSQL_row *row,
			  HeapTuple tup, TupleDesc tupdesc);
static HeapTuple make_tuple_from_row(PLpgSQL_execstate *estate,
					PLpgSQL_row *row,
					TupleDesc tupdesc);
static HeapTuple get_tuple_from_datum(Datum value);
static TupleDesc get_tupdesc_from_datum(Datum value);
static void exec_move_row_from_datum(PLpgSQL_execstate *estate,
						 PLpgSQL_rec *rec,
						 PLpgSQL_row *row,
						 Datum value);
static char *convert_value_to_string(PLpgSQL_execstate *estate,
						Datum value, Oid valtype);
static Datum exec_cast_value(PLpgSQL_execstate *estate,
				Datum value, bool *isnull,
				Oid valtype, int32 valtypmod,
				Oid reqtype, int32 reqtypmod);
static plpgsql_CastHashEntry *get_cast_hashentry(PLpgSQL_execstate *estate,
				   Oid srctype, int32 srctypmod,
				   Oid dsttype, int32 dsttypmod);
static void exec_init_tuple_store(PLpgSQL_execstate *estate);
static void exec_set_found(PLpgSQL_execstate *estate, bool state);
static void plpgsql_create_econtext(PLpgSQL_execstate *estate);
static void plpgsql_destroy_econtext(PLpgSQL_execstate *estate);
static void assign_simple_var(PLpgSQL_execstate *estate, PLpgSQL_var *var,
				  Datum newvalue, bool isnull, bool freeable);
static void assign_text_var(PLpgSQL_execstate *estate, PLpgSQL_var *var,
				const char *str);
static PreparedParamsData *exec_eval_using_params(PLpgSQL_execstate *estate,
					   List *params);
static void free_params_data(PreparedParamsData *ppd);
static Portal exec_dynquery_with_params(PLpgSQL_execstate *estate,
						  PLpgSQL_expr *dynquery, List *params,
						  const char *portalname, int cursorOptions);

static char *format_expr_params(PLpgSQL_execstate *estate,
				   const PLpgSQL_expr *expr);
static char *format_preparedparamsdata(PLpgSQL_execstate *estate,
						  const PreparedParamsData *ppd);


/* ----------
 * plpgsql_exec_function	Called by the call handler for
 *				function execution.
 *
 * This is also used to execute inline code blocks (DO blocks).  The only
 * difference that this code is aware of is that for a DO block, we want
 * to use a private simple_eval_estate, which is created and passed in by
 * the caller.  For regular functions, pass NULL, which implies using
 * shared_simple_eval_estate.  (When using a private simple_eval_estate,
 * we must also use a private cast hashtable, but that's taken care of
 * within plpgsql_estate_setup.)
 * ----------
 */
Datum
plpgsql_exec_function(PLpgSQL_function *func, FunctionCallInfo fcinfo,
					  EState *simple_eval_estate)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			i;
	int			rc;

	/*
	 * Setup the execution state
	 */
	plpgsql_estate_setup(&estate, func, (ReturnSetInfo *) fcinfo->resultinfo,
						 simple_eval_estate);

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_exec_error_callback;
	plerrcontext.arg = &estate;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * Make local execution copies of all the datums
	 */
	estate.err_text = gettext_noop("during initialization of execution state");
	for (i = 0; i < estate.ndatums; i++)
		estate.datums[i] = copy_plpgsql_datum(func->datums[i]);

	/*
	 * Store the actual call argument values into the appropriate variables
	 */
	estate.err_text = gettext_noop("while storing call arguments into local variables");
	for (i = 0; i < func->fn_nargs; i++)
	{
		int			n = func->fn_argvarnos[i];

		switch (estate.datums[n]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) estate.datums[n];

					assign_simple_var(&estate, var,
									  fcinfo->arg[i],
									  fcinfo->argnull[i],
									  false);

					/*
					 * Force any array-valued parameter to be stored in
					 * expanded form in our local variable, in hopes of
					 * improving efficiency of uses of the variable.  (This is
					 * a hack, really: why only arrays? Need more thought
					 * about which cases are likely to win.  See also
					 * typisarray-specific heuristic in exec_assign_value.)
					 *
					 * Special cases: If passed a R/W expanded pointer, assume
					 * we can commandeer the object rather than having to copy
					 * it.  If passed a R/O expanded pointer, just keep it as
					 * the value of the variable for the moment.  (We'll force
					 * it to R/W if the variable gets modified, but that may
					 * very well never happen.)
					 */
					if (!var->isnull && var->datatype->typisarray)
					{
						if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(var->value)))
						{
							/* take ownership of R/W object */
							assign_simple_var(&estate, var,
										   TransferExpandedObject(var->value,
													   CurrentMemoryContext),
											  false,
											  true);
						}
						else if (VARATT_IS_EXTERNAL_EXPANDED_RO(DatumGetPointer(var->value)))
						{
							/* R/O pointer, keep it as-is until assigned to */
						}
						else
						{
							/* flat array, so force to expanded form */
							assign_simple_var(&estate, var,
											  expand_array(var->value,
														CurrentMemoryContext,
														   NULL),
											  false,
											  true);
						}
					}
				}
				break;

			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) estate.datums[n];

					if (!fcinfo->argnull[i])
					{
						/* Assign row value from composite datum */
						exec_move_row_from_datum(&estate, NULL, row,
												 fcinfo->arg[i]);
					}
					else
					{
						/* If arg is null, treat it as an empty row */
						exec_move_row(&estate, NULL, row, NULL, NULL);
					}
					/* clean up after exec_move_row() */
					exec_eval_cleanup(&estate);
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", func->datums[i]->dtype);
		}
	}

	estate.err_text = gettext_noop("during function entry");

	/*
	 * Set the magic variable FOUND to false
	 */
	exec_set_found(&estate, false);

	/*
	 * Let the instrumentation plugin peek at this function
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_beg)
		((*plpgsql_plugin_ptr)->func_beg) (&estate, func);

	/*
	 * Now call the toplevel block of statements
	 */
	estate.err_text = NULL;
	estate.err_stmt = (PLpgSQL_stmt *) (func->action);
	rc = exec_stmt_block(&estate, func->action);
	if (rc != PLPGSQL_RC_RETURN)
	{
		estate.err_stmt = NULL;
		estate.err_text = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT),
				 errmsg("control reached end of function without RETURN")));
	}

	/*
	 * We got a return value - process it
	 */
	estate.err_stmt = NULL;
	estate.err_text = gettext_noop("while casting return value to function's return type");

	fcinfo->isnull = estate.retisnull;

	if (estate.retisset)
	{
		ReturnSetInfo *rsi = estate.rsi;

		/* Check caller can handle a set result */
		if (!rsi || !IsA(rsi, ReturnSetInfo) ||
			(rsi->allowedModes & SFRM_Materialize) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));
		rsi->returnMode = SFRM_Materialize;

		/* If we produced any tuples, send back the result */
		if (estate.tuple_store)
		{
			rsi->setResult = estate.tuple_store;
			if (estate.rettupdesc)
			{
				MemoryContext oldcxt;

				oldcxt = MemoryContextSwitchTo(estate.tuple_store_cxt);
				rsi->setDesc = CreateTupleDescCopy(estate.rettupdesc);
				MemoryContextSwitchTo(oldcxt);
			}
		}
		estate.retval = (Datum) 0;
		fcinfo->isnull = true;
	}
	else if (!estate.retisnull)
	{
		if (estate.retistuple)
		{
			/*
			 * We have to check that the returned tuple actually matches the
			 * expected result type.  XXX would be better to cache the tupdesc
			 * instead of repeating get_call_result_type()
			 */
			HeapTuple	rettup = (HeapTuple) DatumGetPointer(estate.retval);
			TupleDesc	tupdesc;
			TupleConversionMap *tupmap;

			switch (get_call_result_type(fcinfo, NULL, &tupdesc))
			{
				case TYPEFUNC_COMPOSITE:
					/* got the expected result rowtype, now check it */
					tupmap = convert_tuples_by_position(estate.rettupdesc,
														tupdesc,
														gettext_noop("returned record type does not match expected record type"));
					/* it might need conversion */
					if (tupmap)
						rettup = do_convert_tuple(rettup, tupmap);
					/* no need to free map, we're about to return anyway */
					break;
				case TYPEFUNC_RECORD:

					/*
					 * Failed to determine actual type of RECORD.  We could
					 * raise an error here, but what this means in practice is
					 * that the caller is expecting any old generic rowtype,
					 * so we don't really need to be restrictive. Pass back
					 * the generated result type, instead.
					 */
					tupdesc = estate.rettupdesc;
					if (tupdesc == NULL)		/* shouldn't happen */
						elog(ERROR, "return type must be a row type");
					break;
				default:
					/* shouldn't get here if retistuple is true ... */
					elog(ERROR, "return type must be a row type");
					break;
			}

			/*
			 * Copy tuple to upper executor memory, as a tuple Datum. Make
			 * sure it is labeled with the caller-supplied tuple type.
			 */
			estate.retval = PointerGetDatum(SPI_returntuple(rettup, tupdesc));
		}
		else
		{
			/* Cast value to proper type */
			estate.retval = exec_cast_value(&estate,
											estate.retval,
											&fcinfo->isnull,
											estate.rettype,
											-1,
											func->fn_rettype,
											-1);

			/*
			 * If the function's return type isn't by value, copy the value
			 * into upper executor memory context.  However, if we have a R/W
			 * expanded datum, we can just transfer its ownership out to the
			 * upper executor context.
			 */
			if (!fcinfo->isnull && !func->fn_retbyval)
				estate.retval = SPI_datumTransfer(estate.retval,
												  false,
												  func->fn_rettyplen);
		}
	}

	estate.err_text = gettext_noop("during function exit");

	/*
	 * Let the instrumentation plugin peek at this function
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_end)
		((*plpgsql_plugin_ptr)->func_end) (&estate, func);

	/* Clean up any leftover temporary memory */
	plpgsql_destroy_econtext(&estate);
	exec_eval_cleanup(&estate);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;

	/*
	 * Return the function's result
	 */
	return estate.retval;
}


/* ----------
 * plpgsql_exec_trigger		Called by the call handler for
 *				trigger execution.
 * ----------
 */
HeapTuple
plpgsql_exec_trigger(PLpgSQL_function *func,
					 TriggerData *trigdata)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			i;
	int			rc;
	PLpgSQL_var *var;
	PLpgSQL_rec *rec_new,
			   *rec_old;
	HeapTuple	rettup;

	/*
	 * Setup the execution state
	 */
	plpgsql_estate_setup(&estate, func, NULL, NULL);

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_exec_error_callback;
	plerrcontext.arg = &estate;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * Make local execution copies of all the datums
	 */
	estate.err_text = gettext_noop("during initialization of execution state");
	for (i = 0; i < estate.ndatums; i++)
		estate.datums[i] = copy_plpgsql_datum(func->datums[i]);

	/*
	 * Put the OLD and NEW tuples into record variables
	 *
	 * We make the tupdescs available in both records even though only one may
	 * have a value.  This allows parsing of record references to succeed in
	 * functions that are used for multiple trigger types.  For example, we
	 * might have a test like "if (TG_OP = 'INSERT' and NEW.foo = 'xyz')",
	 * which should parse regardless of the current trigger type.
	 */
	rec_new = (PLpgSQL_rec *) (estate.datums[func->new_varno]);
	rec_new->freetup = false;
	rec_new->tupdesc = trigdata->tg_relation->rd_att;
	rec_new->freetupdesc = false;
	rec_old = (PLpgSQL_rec *) (estate.datums[func->old_varno]);
	rec_old->freetup = false;
	rec_old->tupdesc = trigdata->tg_relation->rd_att;
	rec_old->freetupdesc = false;

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
	{
		/*
		 * Per-statement triggers don't use OLD/NEW variables
		 */
		rec_new->tup = NULL;
		rec_old->tup = NULL;
	}
	else if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
	{
		rec_new->tup = trigdata->tg_trigtuple;
		rec_old->tup = NULL;
	}
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		rec_new->tup = trigdata->tg_newtuple;
		rec_old->tup = trigdata->tg_trigtuple;
	}
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
	{
		rec_new->tup = NULL;
		rec_old->tup = trigdata->tg_trigtuple;
	}
	else
		elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, or UPDATE");

	/*
	 * Assign the special tg_ variables
	 */

	var = (PLpgSQL_var *) (estate.datums[func->tg_op_varno]);
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		assign_text_var(&estate, var, "INSERT");
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		assign_text_var(&estate, var, "UPDATE");
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		assign_text_var(&estate, var, "DELETE");
	else if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
		assign_text_var(&estate, var, "TRUNCATE");
	else
		elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, UPDATE, or TRUNCATE");

	var = (PLpgSQL_var *) (estate.datums[func->tg_name_varno]);
	assign_simple_var(&estate, var,
					  DirectFunctionCall1(namein,
							  CStringGetDatum(trigdata->tg_trigger->tgname)),
					  false, true);

	var = (PLpgSQL_var *) (estate.datums[func->tg_when_varno]);
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		assign_text_var(&estate, var, "BEFORE");
	else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		assign_text_var(&estate, var, "AFTER");
	else if (TRIGGER_FIRED_INSTEAD(trigdata->tg_event))
		assign_text_var(&estate, var, "INSTEAD OF");
	else
		elog(ERROR, "unrecognized trigger execution time: not BEFORE, AFTER, or INSTEAD OF");

	var = (PLpgSQL_var *) (estate.datums[func->tg_level_varno]);
	if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		assign_text_var(&estate, var, "ROW");
	else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		assign_text_var(&estate, var, "STATEMENT");
	else
		elog(ERROR, "unrecognized trigger event type: not ROW or STATEMENT");

	var = (PLpgSQL_var *) (estate.datums[func->tg_relid_varno]);
	assign_simple_var(&estate, var,
					  ObjectIdGetDatum(trigdata->tg_relation->rd_id),
					  false, false);

	var = (PLpgSQL_var *) (estate.datums[func->tg_relname_varno]);
	assign_simple_var(&estate, var,
					  DirectFunctionCall1(namein,
			CStringGetDatum(RelationGetRelationName(trigdata->tg_relation))),
					  false, true);

	var = (PLpgSQL_var *) (estate.datums[func->tg_table_name_varno]);
	assign_simple_var(&estate, var,
					  DirectFunctionCall1(namein,
			CStringGetDatum(RelationGetRelationName(trigdata->tg_relation))),
					  false, true);

	var = (PLpgSQL_var *) (estate.datums[func->tg_table_schema_varno]);
	assign_simple_var(&estate, var,
					  DirectFunctionCall1(namein,
										  CStringGetDatum(get_namespace_name(
														RelationGetNamespace(
												   trigdata->tg_relation)))),
					  false, true);

	var = (PLpgSQL_var *) (estate.datums[func->tg_nargs_varno]);
	assign_simple_var(&estate, var,
					  Int16GetDatum(trigdata->tg_trigger->tgnargs),
					  false, false);

	var = (PLpgSQL_var *) (estate.datums[func->tg_argv_varno]);
	if (trigdata->tg_trigger->tgnargs > 0)
	{
		/*
		 * For historical reasons, tg_argv[] subscripts start at zero not one.
		 * So we can't use construct_array().
		 */
		int			nelems = trigdata->tg_trigger->tgnargs;
		Datum	   *elems;
		int			dims[1];
		int			lbs[1];

		elems = palloc(sizeof(Datum) * nelems);
		for (i = 0; i < nelems; i++)
			elems[i] = CStringGetTextDatum(trigdata->tg_trigger->tgargs[i]);
		dims[0] = nelems;
		lbs[0] = 0;

		assign_simple_var(&estate, var,
						  PointerGetDatum(construct_md_array(elems, NULL,
															 1, dims, lbs,
															 TEXTOID,
															 -1, false, 'i')),
						  false, true);
	}
	else
	{
		assign_simple_var(&estate, var, (Datum) 0, true, false);
	}

	estate.err_text = gettext_noop("during function entry");

	/*
	 * Set the magic variable FOUND to false
	 */
	exec_set_found(&estate, false);

	/*
	 * Let the instrumentation plugin peek at this function
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_beg)
		((*plpgsql_plugin_ptr)->func_beg) (&estate, func);

	/*
	 * Now call the toplevel block of statements
	 */
	estate.err_text = NULL;
	estate.err_stmt = (PLpgSQL_stmt *) (func->action);
	rc = exec_stmt_block(&estate, func->action);
	if (rc != PLPGSQL_RC_RETURN)
	{
		estate.err_stmt = NULL;
		estate.err_text = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT),
		 errmsg("control reached end of trigger procedure without RETURN")));
	}

	estate.err_stmt = NULL;
	estate.err_text = gettext_noop("during function exit");

	if (estate.retisset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("trigger procedure cannot return a set")));

	/*
	 * Check that the returned tuple structure has the same attributes, the
	 * relation that fired the trigger has. A per-statement trigger always
	 * needs to return NULL, so we ignore any return value the function itself
	 * produces (XXX: is this a good idea?)
	 *
	 * XXX This way it is possible, that the trigger returns a tuple where
	 * attributes don't have the correct atttypmod's length. It's up to the
	 * trigger's programmer to ensure that this doesn't happen. Jan
	 */
	if (estate.retisnull || !TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		rettup = NULL;
	else
	{
		TupleConversionMap *tupmap;

		rettup = (HeapTuple) DatumGetPointer(estate.retval);
		/* check rowtype compatibility */
		tupmap = convert_tuples_by_position(estate.rettupdesc,
											trigdata->tg_relation->rd_att,
											gettext_noop("returned row structure does not match the structure of the triggering table"));
		/* it might need conversion */
		if (tupmap)
			rettup = do_convert_tuple(rettup, tupmap);
		/* no need to free map, we're about to return anyway */

		/* Copy tuple to upper executor memory */
		rettup = SPI_copytuple(rettup);
	}

	/*
	 * Let the instrumentation plugin peek at this function
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_end)
		((*plpgsql_plugin_ptr)->func_end) (&estate, func);

	/* Clean up any leftover temporary memory */
	plpgsql_destroy_econtext(&estate);
	exec_eval_cleanup(&estate);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;

	/*
	 * Return the trigger's result
	 */
	return rettup;
}

void
plpgsql_exec_event_trigger(PLpgSQL_function *func, EventTriggerData *trigdata)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			i;
	int			rc;
	PLpgSQL_var *var;

	/*
	 * Setup the execution state
	 */
	plpgsql_estate_setup(&estate, func, NULL, NULL);

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_exec_error_callback;
	plerrcontext.arg = &estate;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * Make local execution copies of all the datums
	 */
	estate.err_text = gettext_noop("during initialization of execution state");
	for (i = 0; i < estate.ndatums; i++)
		estate.datums[i] = copy_plpgsql_datum(func->datums[i]);

	/*
	 * Assign the special tg_ variables
	 */
	var = (PLpgSQL_var *) (estate.datums[func->tg_event_varno]);
	assign_text_var(&estate, var, trigdata->event);

	var = (PLpgSQL_var *) (estate.datums[func->tg_tag_varno]);
	assign_text_var(&estate, var, trigdata->tag);

	/*
	 * Let the instrumentation plugin peek at this function
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_beg)
		((*plpgsql_plugin_ptr)->func_beg) (&estate, func);

	/*
	 * Now call the toplevel block of statements
	 */
	estate.err_text = NULL;
	estate.err_stmt = (PLpgSQL_stmt *) (func->action);
	rc = exec_stmt_block(&estate, func->action);
	if (rc != PLPGSQL_RC_RETURN)
	{
		estate.err_stmt = NULL;
		estate.err_text = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT),
		 errmsg("control reached end of trigger procedure without RETURN")));
	}

	estate.err_stmt = NULL;
	estate.err_text = gettext_noop("during function exit");

	/*
	 * Let the instrumentation plugin peek at this function
	 */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->func_end)
		((*plpgsql_plugin_ptr)->func_end) (&estate, func);

	/* Clean up any leftover temporary memory */
	plpgsql_destroy_econtext(&estate);
	exec_eval_cleanup(&estate);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;

	return;
}

/*
 * error context callback to let us supply a call-stack traceback
 */
static void
plpgsql_exec_error_callback(void *arg)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) arg;

	if (estate->err_text != NULL)
	{
		/*
		 * We don't expend the cycles to run gettext() on err_text unless we
		 * actually need it.  Therefore, places that set up err_text should
		 * use gettext_noop() to ensure the strings get recorded in the
		 * message dictionary.
		 *
		 * If both err_text and err_stmt are set, use the err_text as
		 * description, but report the err_stmt's line number.  When err_stmt
		 * is not set, we're in function entry/exit, or some such place not
		 * attached to a specific line number.
		 */
		if (estate->err_stmt != NULL)
		{
			/*
			 * translator: last %s is a phrase such as "during statement block
			 * local variable initialization"
			 */
			errcontext("PL/pgSQL function %s line %d %s",
					   estate->func->fn_signature,
					   estate->err_stmt->lineno,
					   _(estate->err_text));
		}
		else
		{
			/*
			 * translator: last %s is a phrase such as "while storing call
			 * arguments into local variables"
			 */
			errcontext("PL/pgSQL function %s %s",
					   estate->func->fn_signature,
					   _(estate->err_text));
		}
	}
	else if (estate->err_stmt != NULL)
	{
		/* translator: last %s is a plpgsql statement type name */
		errcontext("PL/pgSQL function %s line %d at %s",
				   estate->func->fn_signature,
				   estate->err_stmt->lineno,
				   plpgsql_stmt_typename(estate->err_stmt));
	}
	else
		errcontext("PL/pgSQL function %s",
				   estate->func->fn_signature);
}


/* ----------
 * Support function for initializing local execution variables
 * ----------
 */
static PLpgSQL_datum *
copy_plpgsql_datum(PLpgSQL_datum *datum)
{
	PLpgSQL_datum *result;

	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
			{
				PLpgSQL_var *new = palloc(sizeof(PLpgSQL_var));

				memcpy(new, datum, sizeof(PLpgSQL_var));
				/* should be preset to null/non-freeable */
				Assert(new->isnull);
				Assert(!new->freeval);

				result = (PLpgSQL_datum *) new;
			}
			break;

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *new = palloc(sizeof(PLpgSQL_rec));

				memcpy(new, datum, sizeof(PLpgSQL_rec));
				/* should be preset to null/non-freeable */
				Assert(new->tup == NULL);
				Assert(new->tupdesc == NULL);
				Assert(!new->freetup);
				Assert(!new->freetupdesc);

				result = (PLpgSQL_datum *) new;
			}
			break;

		case PLPGSQL_DTYPE_ROW:
		case PLPGSQL_DTYPE_RECFIELD:
		case PLPGSQL_DTYPE_ARRAYELEM:

			/*
			 * These datum records are read-only at runtime, so no need to
			 * copy them (well, ARRAYELEM contains some cached type data, but
			 * we'd just as soon centralize the caching anyway)
			 */
			result = datum;
			break;

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
			result = NULL;		/* keep compiler quiet */
			break;
	}

	return result;
}


static bool
exception_matches_conditions(ErrorData *edata, PLpgSQL_condition *cond)
{
	for (; cond != NULL; cond = cond->next)
	{
		int			sqlerrstate = cond->sqlerrstate;

		/*
		 * OTHERS matches everything *except* query-canceled and
		 * assert-failure.  If you're foolish enough, you can match those
		 * explicitly.
		 */
		if (sqlerrstate == 0)
		{
			if (edata->sqlerrcode != ERRCODE_QUERY_CANCELED &&
				edata->sqlerrcode != ERRCODE_ASSERT_FAILURE)
				return true;
		}
		/* Exact match? */
		else if (edata->sqlerrcode == sqlerrstate)
			return true;
		/* Category match? */
		else if (ERRCODE_IS_CATEGORY(sqlerrstate) &&
				 ERRCODE_TO_CATEGORY(edata->sqlerrcode) == sqlerrstate)
			return true;
	}
	return false;
}


/* ----------
 * exec_stmt_block			Execute a block of statements
 * ----------
 */
static int
exec_stmt_block(PLpgSQL_execstate *estate, PLpgSQL_stmt_block *block)
{
	volatile int rc = -1;
	int			i;
	int			n;

	/*
	 * First initialize all variables declared in this block
	 */
	estate->err_text = gettext_noop("during statement block local variable initialization");

	for (i = 0; i < block->n_initvars; i++)
	{
		n = block->initvarnos[i];

		switch (estate->datums[n]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) (estate->datums[n]);

					/*
					 * Free any old value, in case re-entering block, and
					 * initialize to NULL
					 */
					assign_simple_var(estate, var, (Datum) 0, true, false);

					if (var->default_val == NULL)
					{
						/*
						 * If needed, give the datatype a chance to reject
						 * NULLs, by assigning a NULL to the variable. We
						 * claim the value is of type UNKNOWN, not the var's
						 * datatype, else coercion will be skipped. (Do this
						 * before the notnull check to be consistent with
						 * exec_assign_value.)
						 */
						if (var->datatype->typtype == TYPTYPE_DOMAIN)
							exec_assign_value(estate,
											  (PLpgSQL_datum *) var,
											  (Datum) 0,
											  true,
											  UNKNOWNOID,
											  -1);

						if (var->notnull)
							ereport(ERROR,
									(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
									 errmsg("variable \"%s\" declared NOT NULL cannot default to NULL",
											var->refname)));
					}
					else
					{
						exec_assign_expr(estate, (PLpgSQL_datum *) var,
										 var->default_val);
					}
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) (estate->datums[n]);

					if (rec->freetup)
					{
						heap_freetuple(rec->tup);
						rec->freetup = false;
					}
					if (rec->freetupdesc)
					{
						FreeTupleDesc(rec->tupdesc);
						rec->freetupdesc = false;
					}
					rec->tup = NULL;
					rec->tupdesc = NULL;
				}
				break;

			case PLPGSQL_DTYPE_RECFIELD:
			case PLPGSQL_DTYPE_ARRAYELEM:
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d",
					 estate->datums[n]->dtype);
		}
	}

	if (block->exceptions)
	{
		/*
		 * Execute the statements in the block's body inside a sub-transaction
		 */
		MemoryContext oldcontext = CurrentMemoryContext;
		ResourceOwner oldowner = CurrentResourceOwner;
		ExprContext *old_eval_econtext = estate->eval_econtext;
		ErrorData  *save_cur_error = estate->cur_error;

		estate->err_text = gettext_noop("during statement block entry");

		BeginInternalSubTransaction(NULL);
		/* Want to run statements inside function's memory context */
		MemoryContextSwitchTo(oldcontext);

		PG_TRY();
		{
			/*
			 * We need to run the block's statements with a new eval_econtext
			 * that belongs to the current subtransaction; if we try to use
			 * the outer econtext then ExprContext shutdown callbacks will be
			 * called at the wrong times.
			 */
			plpgsql_create_econtext(estate);

			estate->err_text = NULL;

			/* Run the block's statements */
			rc = exec_stmts(estate, block->body);

			estate->err_text = gettext_noop("during statement block exit");

			/*
			 * If the block ended with RETURN, we may need to copy the return
			 * value out of the subtransaction eval_context.  This is
			 * currently only needed for scalar result types --- rowtype
			 * values will always exist in the function's own memory context.
			 */
			if (rc == PLPGSQL_RC_RETURN &&
				!estate->retisset &&
				!estate->retisnull &&
				estate->rettupdesc == NULL)
			{
				int16		resTypLen;
				bool		resTypByVal;

				get_typlenbyval(estate->rettype, &resTypLen, &resTypByVal);
				estate->retval = datumCopy(estate->retval,
										   resTypByVal, resTypLen);
			}

			/* Commit the inner transaction, return to outer xact context */
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcontext);
			CurrentResourceOwner = oldowner;

			/*
			 * Revert to outer eval_econtext.  (The inner one was
			 * automatically cleaned up during subxact exit.)
			 */
			estate->eval_econtext = old_eval_econtext;

			/*
			 * AtEOSubXact_SPI() should not have popped any SPI context, but
			 * just in case it did, make sure we remain connected.
			 */
			SPI_restore_connection();
		}
		PG_CATCH();
		{
			ErrorData  *edata;
			ListCell   *e;

			estate->err_text = gettext_noop("during exception cleanup");

			/* Save error info */
			MemoryContextSwitchTo(oldcontext);
			edata = CopyErrorData();
			FlushErrorState();

			/* Abort the inner transaction */
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcontext);
			CurrentResourceOwner = oldowner;

			/* Revert to outer eval_econtext */
			estate->eval_econtext = old_eval_econtext;

			/*
			 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it
			 * will have left us in a disconnected state.  We need this hack
			 * to return to connected state.
			 */
			SPI_restore_connection();

			/*
			 * Must clean up the econtext too.  However, any tuple table made
			 * in the subxact will have been thrown away by SPI during subxact
			 * abort, so we don't need to (and mustn't try to) free the
			 * eval_tuptable.
			 */
			estate->eval_tuptable = NULL;
			exec_eval_cleanup(estate);

			/* Look for a matching exception handler */
			foreach(e, block->exceptions->exc_list)
			{
				PLpgSQL_exception *exception = (PLpgSQL_exception *) lfirst(e);

				if (exception_matches_conditions(edata, exception->conditions))
				{
					/*
					 * Initialize the magic SQLSTATE and SQLERRM variables for
					 * the exception block; this also frees values from any
					 * prior use of the same exception. We needn't do this
					 * until we have found a matching exception.
					 */
					PLpgSQL_var *state_var;
					PLpgSQL_var *errm_var;

					state_var = (PLpgSQL_var *)
						estate->datums[block->exceptions->sqlstate_varno];
					errm_var = (PLpgSQL_var *)
						estate->datums[block->exceptions->sqlerrm_varno];

					assign_text_var(estate, state_var,
									unpack_sql_state(edata->sqlerrcode));
					assign_text_var(estate, errm_var, edata->message);

					/*
					 * Also set up cur_error so the error data is accessible
					 * inside the handler.
					 */
					estate->cur_error = edata;

					estate->err_text = NULL;

					rc = exec_stmts(estate, exception->action);

					break;
				}
			}

			/*
			 * Restore previous state of cur_error, whether or not we executed
			 * a handler.  This is needed in case an error got thrown from
			 * some inner block's exception handler.
			 */
			estate->cur_error = save_cur_error;

			/* If no match found, re-throw the error */
			if (e == NULL)
				ReThrowError(edata);
			else
				FreeErrorData(edata);
		}
		PG_END_TRY();

		Assert(save_cur_error == estate->cur_error);
	}
	else
	{
		/*
		 * Just execute the statements in the block's body
		 */
		estate->err_text = NULL;

		rc = exec_stmts(estate, block->body);
	}

	estate->err_text = NULL;

	/*
	 * Handle the return code.
	 */
	switch (rc)
	{
		case PLPGSQL_RC_OK:
		case PLPGSQL_RC_RETURN:
		case PLPGSQL_RC_CONTINUE:
			return rc;

		case PLPGSQL_RC_EXIT:

			/*
			 * This is intentionally different from the handling of RC_EXIT
			 * for loops: to match a block, we require a match by label.
			 */
			if (estate->exitlabel == NULL)
				return PLPGSQL_RC_EXIT;
			if (block->label == NULL)
				return PLPGSQL_RC_EXIT;
			if (strcmp(block->label, estate->exitlabel) != 0)
				return PLPGSQL_RC_EXIT;
			estate->exitlabel = NULL;
			return PLPGSQL_RC_OK;

		default:
			elog(ERROR, "unrecognized rc: %d", rc);
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmts			Iterate over a list of statements
 *				as long as their return code is OK
 * ----------
 */
static int
exec_stmts(PLpgSQL_execstate *estate, List *stmts)
{
	ListCell   *s;

	if (stmts == NIL)
	{
		/*
		 * Ensure we do a CHECK_FOR_INTERRUPTS() even though there is no
		 * statement.  This prevents hangup in a tight loop if, for instance,
		 * there is a LOOP construct with an empty body.
		 */
		CHECK_FOR_INTERRUPTS();
		return PLPGSQL_RC_OK;
	}

	foreach(s, stmts)
	{
		PLpgSQL_stmt *stmt = (PLpgSQL_stmt *) lfirst(s);
		int			rc = exec_stmt(estate, stmt);

		if (rc != PLPGSQL_RC_OK)
			return rc;
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt			Distribute one statement to the statements
 *				type specific execution function.
 * ----------
 */
static int
exec_stmt(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt)
{
	PLpgSQL_stmt *save_estmt;
	int			rc = -1;

	save_estmt = estate->err_stmt;
	estate->err_stmt = stmt;

	/* Let the plugin know that we are about to execute this statement */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->stmt_beg)
		((*plpgsql_plugin_ptr)->stmt_beg) (estate, stmt);

	CHECK_FOR_INTERRUPTS();

	switch ((enum PLpgSQL_stmt_types) stmt->cmd_type)
	{
		case PLPGSQL_STMT_BLOCK:
			rc = exec_stmt_block(estate, (PLpgSQL_stmt_block *) stmt);
			break;

		case PLPGSQL_STMT_ASSIGN:
			rc = exec_stmt_assign(estate, (PLpgSQL_stmt_assign *) stmt);
			break;

		case PLPGSQL_STMT_PERFORM:
			rc = exec_stmt_perform(estate, (PLpgSQL_stmt_perform *) stmt);
			break;

		case PLPGSQL_STMT_GETDIAG:
			rc = exec_stmt_getdiag(estate, (PLpgSQL_stmt_getdiag *) stmt);
			break;

		case PLPGSQL_STMT_IF:
			rc = exec_stmt_if(estate, (PLpgSQL_stmt_if *) stmt);
			break;

		case PLPGSQL_STMT_CASE:
			rc = exec_stmt_case(estate, (PLpgSQL_stmt_case *) stmt);
			break;

		case PLPGSQL_STMT_LOOP:
			rc = exec_stmt_loop(estate, (PLpgSQL_stmt_loop *) stmt);
			break;

		case PLPGSQL_STMT_WHILE:
			rc = exec_stmt_while(estate, (PLpgSQL_stmt_while *) stmt);
			break;

		case PLPGSQL_STMT_FORI:
			rc = exec_stmt_fori(estate, (PLpgSQL_stmt_fori *) stmt);
			break;

		case PLPGSQL_STMT_FORS:
			rc = exec_stmt_fors(estate, (PLpgSQL_stmt_fors *) stmt);
			break;

		case PLPGSQL_STMT_FORC:
			rc = exec_stmt_forc(estate, (PLpgSQL_stmt_forc *) stmt);
			break;

		case PLPGSQL_STMT_FOREACH_A:
			rc = exec_stmt_foreach_a(estate, (PLpgSQL_stmt_foreach_a *) stmt);
			break;

		case PLPGSQL_STMT_EXIT:
			rc = exec_stmt_exit(estate, (PLpgSQL_stmt_exit *) stmt);
			break;

		case PLPGSQL_STMT_RETURN:
			rc = exec_stmt_return(estate, (PLpgSQL_stmt_return *) stmt);
			break;

		case PLPGSQL_STMT_RETURN_NEXT:
			rc = exec_stmt_return_next(estate, (PLpgSQL_stmt_return_next *) stmt);
			break;

		case PLPGSQL_STMT_RETURN_QUERY:
			rc = exec_stmt_return_query(estate, (PLpgSQL_stmt_return_query *) stmt);
			break;

		case PLPGSQL_STMT_RAISE:
			rc = exec_stmt_raise(estate, (PLpgSQL_stmt_raise *) stmt);
			break;

		case PLPGSQL_STMT_ASSERT:
			rc = exec_stmt_assert(estate, (PLpgSQL_stmt_assert *) stmt);
			break;

		case PLPGSQL_STMT_EXECSQL:
			rc = exec_stmt_execsql(estate, (PLpgSQL_stmt_execsql *) stmt);
			break;

		case PLPGSQL_STMT_DYNEXECUTE:
			rc = exec_stmt_dynexecute(estate, (PLpgSQL_stmt_dynexecute *) stmt);
			break;

		case PLPGSQL_STMT_DYNFORS:
			rc = exec_stmt_dynfors(estate, (PLpgSQL_stmt_dynfors *) stmt);
			break;

		case PLPGSQL_STMT_OPEN:
			rc = exec_stmt_open(estate, (PLpgSQL_stmt_open *) stmt);
			break;

		case PLPGSQL_STMT_FETCH:
			rc = exec_stmt_fetch(estate, (PLpgSQL_stmt_fetch *) stmt);
			break;

		case PLPGSQL_STMT_CLOSE:
			rc = exec_stmt_close(estate, (PLpgSQL_stmt_close *) stmt);
			break;

		default:
			estate->err_stmt = save_estmt;
			elog(ERROR, "unrecognized cmdtype: %d", stmt->cmd_type);
	}

	/* Let the plugin know that we have finished executing this statement */
	if (*plpgsql_plugin_ptr && (*plpgsql_plugin_ptr)->stmt_end)
		((*plpgsql_plugin_ptr)->stmt_end) (estate, stmt);

	estate->err_stmt = save_estmt;

	return rc;
}


/* ----------
 * exec_stmt_assign			Evaluate an expression and
 *					put the result into a variable.
 * ----------
 */
static int
exec_stmt_assign(PLpgSQL_execstate *estate, PLpgSQL_stmt_assign *stmt)
{
	Assert(stmt->varno >= 0);

	exec_assign_expr(estate, estate->datums[stmt->varno], stmt->expr);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_perform		Evaluate query and discard result (but set
 *							FOUND depending on whether at least one row
 *							was returned).
 * ----------
 */
static int
exec_stmt_perform(PLpgSQL_execstate *estate, PLpgSQL_stmt_perform *stmt)
{
	PLpgSQL_expr *expr = stmt->expr;

	(void) exec_run_select(estate, expr, 0, NULL, true);
	exec_set_found(estate, (estate->eval_processed != 0));
	exec_eval_cleanup(estate);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_getdiag					Put internal PG information into
 *										specified variables.
 * ----------
 */
static int
exec_stmt_getdiag(PLpgSQL_execstate *estate, PLpgSQL_stmt_getdiag *stmt)
{
	ListCell   *lc;

	/*
	 * GET STACKED DIAGNOSTICS is only valid inside an exception handler.
	 *
	 * Note: we trust the grammar to have disallowed the relevant item kinds
	 * if not is_stacked, otherwise we'd dump core below.
	 */
	if (stmt->is_stacked && estate->cur_error == NULL)
		ereport(ERROR,
		(errcode(ERRCODE_STACKED_DIAGNOSTICS_ACCESSED_WITHOUT_ACTIVE_HANDLER),
		 errmsg("GET STACKED DIAGNOSTICS cannot be used outside an exception handler")));

	foreach(lc, stmt->diag_items)
	{
		PLpgSQL_diag_item *diag_item = (PLpgSQL_diag_item *) lfirst(lc);
		PLpgSQL_datum *var = estate->datums[diag_item->target];

		switch (diag_item->kind)
		{
			case PLPGSQL_GETDIAG_ROW_COUNT:
				exec_assign_value(estate, var,
								  UInt64GetDatum(estate->eval_processed),
								  false, INT8OID, -1);
				break;

			case PLPGSQL_GETDIAG_RESULT_OID:
				exec_assign_value(estate, var,
								  ObjectIdGetDatum(estate->eval_lastoid),
								  false, OIDOID, -1);
				break;

			case PLPGSQL_GETDIAG_ERROR_CONTEXT:
				exec_assign_c_string(estate, var,
									 estate->cur_error->context);
				break;

			case PLPGSQL_GETDIAG_ERROR_DETAIL:
				exec_assign_c_string(estate, var,
									 estate->cur_error->detail);
				break;

			case PLPGSQL_GETDIAG_ERROR_HINT:
				exec_assign_c_string(estate, var,
									 estate->cur_error->hint);
				break;

			case PLPGSQL_GETDIAG_RETURNED_SQLSTATE:
				exec_assign_c_string(estate, var,
							unpack_sql_state(estate->cur_error->sqlerrcode));
				break;

			case PLPGSQL_GETDIAG_COLUMN_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->column_name);
				break;

			case PLPGSQL_GETDIAG_CONSTRAINT_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->constraint_name);
				break;

			case PLPGSQL_GETDIAG_DATATYPE_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->datatype_name);
				break;

			case PLPGSQL_GETDIAG_MESSAGE_TEXT:
				exec_assign_c_string(estate, var,
									 estate->cur_error->message);
				break;

			case PLPGSQL_GETDIAG_TABLE_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->table_name);
				break;

			case PLPGSQL_GETDIAG_SCHEMA_NAME:
				exec_assign_c_string(estate, var,
									 estate->cur_error->schema_name);
				break;

			case PLPGSQL_GETDIAG_CONTEXT:
				{
					char	   *contextstackstr = GetErrorContextStack();

					exec_assign_c_string(estate, var, contextstackstr);

					pfree(contextstackstr);
				}
				break;

			default:
				elog(ERROR, "unrecognized diagnostic item kind: %d",
					 diag_item->kind);
		}
	}

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_if				Evaluate a bool expression and
 *					execute the true or false body
 *					conditionally.
 * ----------
 */
static int
exec_stmt_if(PLpgSQL_execstate *estate, PLpgSQL_stmt_if *stmt)
{
	bool		value;
	bool		isnull;
	ListCell   *lc;

	value = exec_eval_boolean(estate, stmt->cond, &isnull);
	exec_eval_cleanup(estate);
	if (!isnull && value)
		return exec_stmts(estate, stmt->then_body);

	foreach(lc, stmt->elsif_list)
	{
		PLpgSQL_if_elsif *elif = (PLpgSQL_if_elsif *) lfirst(lc);

		value = exec_eval_boolean(estate, elif->cond, &isnull);
		exec_eval_cleanup(estate);
		if (!isnull && value)
			return exec_stmts(estate, elif->stmts);
	}

	return exec_stmts(estate, stmt->else_body);
}


/*-----------
 * exec_stmt_case
 *-----------
 */
static int
exec_stmt_case(PLpgSQL_execstate *estate, PLpgSQL_stmt_case *stmt)
{
	PLpgSQL_var *t_var = NULL;
	bool		isnull;
	ListCell   *l;

	if (stmt->t_expr != NULL)
	{
		/* simple case */
		Datum		t_val;
		Oid			t_typoid;
		int32		t_typmod;

		t_val = exec_eval_expr(estate, stmt->t_expr,
							   &isnull, &t_typoid, &t_typmod);

		t_var = (PLpgSQL_var *) estate->datums[stmt->t_varno];

		/*
		 * When expected datatype is different from real, change it. Note that
		 * what we're modifying here is an execution copy of the datum, so
		 * this doesn't affect the originally stored function parse tree.
		 */
		if (t_var->datatype->typoid != t_typoid ||
			t_var->datatype->atttypmod != t_typmod)
			t_var->datatype = plpgsql_build_datatype(t_typoid,
													 t_typmod,
										   estate->func->fn_input_collation);

		/* now we can assign to the variable */
		exec_assign_value(estate,
						  (PLpgSQL_datum *) t_var,
						  t_val,
						  isnull,
						  t_typoid,
						  t_typmod);

		exec_eval_cleanup(estate);
	}

	/* Now search for a successful WHEN clause */
	foreach(l, stmt->case_when_list)
	{
		PLpgSQL_case_when *cwt = (PLpgSQL_case_when *) lfirst(l);
		bool		value;

		value = exec_eval_boolean(estate, cwt->expr, &isnull);
		exec_eval_cleanup(estate);
		if (!isnull && value)
		{
			/* Found it */

			/* We can now discard any value we had for the temp variable */
			if (t_var != NULL)
				assign_simple_var(estate, t_var, (Datum) 0, true, false);

			/* Evaluate the statement(s), and we're done */
			return exec_stmts(estate, cwt->stmts);
		}
	}

	/* We can now discard any value we had for the temp variable */
	if (t_var != NULL)
		assign_simple_var(estate, t_var, (Datum) 0, true, false);

	/* SQL2003 mandates this error if there was no ELSE clause */
	if (!stmt->have_else)
		ereport(ERROR,
				(errcode(ERRCODE_CASE_NOT_FOUND),
				 errmsg("case not found"),
				 errhint("CASE statement is missing ELSE part.")));

	/* Evaluate the ELSE statements, and we're done */
	return exec_stmts(estate, stmt->else_stmts);
}


/* ----------
 * exec_stmt_loop			Loop over statements until
 *					an exit occurs.
 * ----------
 */
static int
exec_stmt_loop(PLpgSQL_execstate *estate, PLpgSQL_stmt_loop *stmt)
{
	for (;;)
	{
		int			rc = exec_stmts(estate, stmt->body);

		switch (rc)
		{
			case PLPGSQL_RC_OK:
				break;

			case PLPGSQL_RC_EXIT:
				if (estate->exitlabel == NULL)
					return PLPGSQL_RC_OK;
				if (stmt->label == NULL)
					return PLPGSQL_RC_EXIT;
				if (strcmp(stmt->label, estate->exitlabel) != 0)
					return PLPGSQL_RC_EXIT;
				estate->exitlabel = NULL;
				return PLPGSQL_RC_OK;

			case PLPGSQL_RC_CONTINUE:
				if (estate->exitlabel == NULL)
					/* anonymous continue, so re-run the loop */
					break;
				else if (stmt->label != NULL &&
						 strcmp(stmt->label, estate->exitlabel) == 0)
					/* label matches named continue, so re-run loop */
					estate->exitlabel = NULL;
				else
					/* label doesn't match named continue, so propagate upward */
					return PLPGSQL_RC_CONTINUE;
				break;

			case PLPGSQL_RC_RETURN:
				return rc;

			default:
				elog(ERROR, "unrecognized rc: %d", rc);
		}
	}
}


/* ----------
 * exec_stmt_while			Loop over statements as long
 *					as an expression evaluates to
 *					true or an exit occurs.
 * ----------
 */
static int
exec_stmt_while(PLpgSQL_execstate *estate, PLpgSQL_stmt_while *stmt)
{
	for (;;)
	{
		int			rc;
		bool		value;
		bool		isnull;

		value = exec_eval_boolean(estate, stmt->cond, &isnull);
		exec_eval_cleanup(estate);

		if (isnull || !value)
			break;

		rc = exec_stmts(estate, stmt->body);

		switch (rc)
		{
			case PLPGSQL_RC_OK:
				break;

			case PLPGSQL_RC_EXIT:
				if (estate->exitlabel == NULL)
					return PLPGSQL_RC_OK;
				if (stmt->label == NULL)
					return PLPGSQL_RC_EXIT;
				if (strcmp(stmt->label, estate->exitlabel) != 0)
					return PLPGSQL_RC_EXIT;
				estate->exitlabel = NULL;
				return PLPGSQL_RC_OK;

			case PLPGSQL_RC_CONTINUE:
				if (estate->exitlabel == NULL)
					/* anonymous continue, so re-run loop */
					break;
				else if (stmt->label != NULL &&
						 strcmp(stmt->label, estate->exitlabel) == 0)
					/* label matches named continue, so re-run loop */
					estate->exitlabel = NULL;
				else
					/* label doesn't match named continue, propagate upward */
					return PLPGSQL_RC_CONTINUE;
				break;

			case PLPGSQL_RC_RETURN:
				return rc;

			default:
				elog(ERROR, "unrecognized rc: %d", rc);
		}
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_fori			Iterate an integer variable
 *					from a lower to an upper value
 *					incrementing or decrementing by the BY value
 * ----------
 */
static int
exec_stmt_fori(PLpgSQL_execstate *estate, PLpgSQL_stmt_fori *stmt)
{
	PLpgSQL_var *var;
	Datum		value;
	bool		isnull;
	Oid			valtype;
	int32		valtypmod;
	int32		loop_value;
	int32		end_value;
	int32		step_value;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;

	var = (PLpgSQL_var *) (estate->datums[stmt->var->dno]);

	/*
	 * Get the value of the lower bound
	 */
	value = exec_eval_expr(estate, stmt->lower,
						   &isnull, &valtype, &valtypmod);
	value = exec_cast_value(estate, value, &isnull,
							valtype, valtypmod,
							var->datatype->typoid,
							var->datatype->atttypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("lower bound of FOR loop cannot be null")));
	loop_value = DatumGetInt32(value);
	exec_eval_cleanup(estate);

	/*
	 * Get the value of the upper bound
	 */
	value = exec_eval_expr(estate, stmt->upper,
						   &isnull, &valtype, &valtypmod);
	value = exec_cast_value(estate, value, &isnull,
							valtype, valtypmod,
							var->datatype->typoid,
							var->datatype->atttypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("upper bound of FOR loop cannot be null")));
	end_value = DatumGetInt32(value);
	exec_eval_cleanup(estate);

	/*
	 * Get the step value
	 */
	if (stmt->step)
	{
		value = exec_eval_expr(estate, stmt->step,
							   &isnull, &valtype, &valtypmod);
		value = exec_cast_value(estate, value, &isnull,
								valtype, valtypmod,
								var->datatype->typoid,
								var->datatype->atttypmod);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("BY value of FOR loop cannot be null")));
		step_value = DatumGetInt32(value);
		exec_eval_cleanup(estate);
		if (step_value <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				  errmsg("BY value of FOR loop must be greater than zero")));
	}
	else
		step_value = 1;

	/*
	 * Now do the loop
	 */
	for (;;)
	{
		/*
		 * Check against upper bound
		 */
		if (stmt->reverse)
		{
			if (loop_value < end_value)
				break;
		}
		else
		{
			if (loop_value > end_value)
				break;
		}

		found = true;			/* looped at least once */

		/*
		 * Assign current value to loop var
		 */
		assign_simple_var(estate, var, Int32GetDatum(loop_value), false, false);

		/*
		 * Execute the statements
		 */
		rc = exec_stmts(estate, stmt->body);

		if (rc == PLPGSQL_RC_RETURN)
			break;				/* break out of the loop */
		else if (rc == PLPGSQL_RC_EXIT)
		{
			if (estate->exitlabel == NULL)
				/* unlabelled exit, finish the current loop */
				rc = PLPGSQL_RC_OK;
			else if (stmt->label != NULL &&
					 strcmp(stmt->label, estate->exitlabel) == 0)
			{
				/* labelled exit, matches the current stmt's label */
				estate->exitlabel = NULL;
				rc = PLPGSQL_RC_OK;
			}

			/*
			 * otherwise, this is a labelled exit that does not match the
			 * current statement's label, if any: return RC_EXIT so that the
			 * EXIT continues to propagate up the stack.
			 */
			break;
		}
		else if (rc == PLPGSQL_RC_CONTINUE)
		{
			if (estate->exitlabel == NULL)
				/* unlabelled continue, so re-run the current loop */
				rc = PLPGSQL_RC_OK;
			else if (stmt->label != NULL &&
					 strcmp(stmt->label, estate->exitlabel) == 0)
			{
				/* label matches named continue, so re-run loop */
				estate->exitlabel = NULL;
				rc = PLPGSQL_RC_OK;
			}
			else
			{
				/*
				 * otherwise, this is a named continue that does not match the
				 * current statement's label, if any: return RC_CONTINUE so
				 * that the CONTINUE will propagate up the stack.
				 */
				break;
			}
		}

		/*
		 * Increase/decrease loop value, unless it would overflow, in which
		 * case exit the loop.
		 */
		if (stmt->reverse)
		{
			if ((int32) (loop_value - step_value) > loop_value)
				break;
			loop_value -= step_value;
		}
		else
		{
			if ((int32) (loop_value + step_value) < loop_value)
				break;
			loop_value += step_value;
		}
	}

	/*
	 * Set the FOUND variable to indicate the result of executing the loop
	 * (namely, whether we looped one or more times). This must be set here so
	 * that it does not interfere with the value of the FOUND variable inside
	 * the loop processing itself.
	 */
	exec_set_found(estate, found);

	return rc;
}


/* ----------
 * exec_stmt_fors			Execute a query, assign each
 *					tuple to a record or row and
 *					execute a group of statements
 *					for it.
 * ----------
 */
static int
exec_stmt_fors(PLpgSQL_execstate *estate, PLpgSQL_stmt_fors *stmt)
{
	Portal		portal;
	int			rc;

	/*
	 * Open the implicit cursor for the statement using exec_run_select
	 */
	exec_run_select(estate, stmt->query, 0, &portal, false);

	/*
	 * Execute the loop
	 */
	rc = exec_for_query(estate, (PLpgSQL_stmt_forq *) stmt, portal, true);

	/*
	 * Close the implicit cursor
	 */
	SPI_cursor_close(portal);

	return rc;
}


/* ----------
 * exec_stmt_forc			Execute a loop for each row from a cursor.
 * ----------
 */
static int
exec_stmt_forc(PLpgSQL_execstate *estate, PLpgSQL_stmt_forc *stmt)
{
	PLpgSQL_var *curvar;
	char	   *curname = NULL;
	PLpgSQL_expr *query;
	ParamListInfo paramLI;
	Portal		portal;
	int			rc;

	/* ----------
	 * Get the cursor variable and if it has an assigned name, check
	 * that it's not in use currently.
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (!curvar->isnull)
	{
		curname = TextDatumGetCString(curvar->value);
		if (SPI_cursor_find(curname) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("cursor \"%s\" already in use", curname)));
	}

	/* ----------
	 * Open the cursor just like an OPEN command
	 *
	 * Note: parser should already have checked that statement supplies
	 * args iff cursor needs them, but we check again to be safe.
	 * ----------
	 */
	if (stmt->argquery != NULL)
	{
		/* ----------
		 * OPEN CURSOR with args.  We fake a SELECT ... INTO ...
		 * statement to evaluate the args and put 'em into the
		 * internal row.
		 * ----------
		 */
		PLpgSQL_stmt_execsql set_args;

		if (curvar->cursor_explicit_argrow < 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("arguments given for cursor without arguments")));

		memset(&set_args, 0, sizeof(set_args));
		set_args.cmd_type = PLPGSQL_STMT_EXECSQL;
		set_args.lineno = stmt->lineno;
		set_args.sqlstmt = stmt->argquery;
		set_args.into = true;
		/* XXX historically this has not been STRICT */
		set_args.row = (PLpgSQL_row *)
			(estate->datums[curvar->cursor_explicit_argrow]);

		if (exec_stmt_execsql(estate, &set_args) != PLPGSQL_RC_OK)
			elog(ERROR, "open cursor failed during argument processing");
	}
	else
	{
		if (curvar->cursor_explicit_argrow >= 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("arguments required for cursor")));
	}

	query = curvar->cursor_explicit_expr;
	Assert(query);

	if (query->plan == NULL)
		exec_prepare_plan(estate, query, curvar->cursor_options);

	/*
	 * Set up short-lived ParamListInfo
	 */
	paramLI = setup_unshared_param_list(estate, query);

	/*
	 * Open the cursor (the paramlist will get copied into the portal)
	 */
	portal = SPI_cursor_open_with_paramlist(curname, query->plan,
											paramLI,
											estate->readonly_func);
	if (portal == NULL)
		elog(ERROR, "could not open cursor: %s",
			 SPI_result_code_string(SPI_result));

	/* don't need paramlist any more */
	if (paramLI)
		pfree(paramLI);

	/*
	 * If cursor variable was NULL, store the generated portal name in it
	 */
	if (curname == NULL)
		assign_text_var(estate, curvar, portal->name);

	/*
	 * Execute the loop.  We can't prefetch because the cursor is accessible
	 * to the user, for instance via UPDATE WHERE CURRENT OF within the loop.
	 */
	rc = exec_for_query(estate, (PLpgSQL_stmt_forq *) stmt, portal, false);

	/* ----------
	 * Close portal, and restore cursor variable if it was initially NULL.
	 * ----------
	 */
	SPI_cursor_close(portal);

	if (curname == NULL)
		assign_simple_var(estate, curvar, (Datum) 0, true, false);

	if (curname)
		pfree(curname);

	return rc;
}


/* ----------
 * exec_stmt_foreach_a			Loop over elements or slices of an array
 *
 * When looping over elements, the loop variable is the same type that the
 * array stores (eg: integer), when looping through slices, the loop variable
 * is an array of size and dimensions to match the size of the slice.
 * ----------
 */
static int
exec_stmt_foreach_a(PLpgSQL_execstate *estate, PLpgSQL_stmt_foreach_a *stmt)
{
	ArrayType  *arr;
	Oid			arrtype;
	int32		arrtypmod;
	PLpgSQL_datum *loop_var;
	Oid			loop_var_elem_type;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;
	ArrayIterator array_iterator;
	Oid			iterator_result_type;
	int32		iterator_result_typmod;
	Datum		value;
	bool		isnull;

	/* get the value of the array expression */
	value = exec_eval_expr(estate, stmt->expr, &isnull, &arrtype, &arrtypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("FOREACH expression must not be null")));

	/* check the type of the expression - must be an array */
	if (!OidIsValid(get_element_type(arrtype)))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("FOREACH expression must yield an array, not type %s",
						format_type_be(arrtype))));

	/*
	 * We must copy the array, else it will disappear in exec_eval_cleanup.
	 * This is annoying, but cleanup will certainly happen while running the
	 * loop body, so we have little choice.
	 */
	arr = DatumGetArrayTypePCopy(value);

	/* Clean up any leftover temporary memory */
	exec_eval_cleanup(estate);

	/* Slice dimension must be less than or equal to array dimension */
	if (stmt->slice < 0 || stmt->slice > ARR_NDIM(arr))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
			   errmsg("slice dimension (%d) is out of the valid range 0..%d",
					  stmt->slice, ARR_NDIM(arr))));

	/* Set up the loop variable and see if it is of an array type */
	loop_var = estate->datums[stmt->varno];
	if (loop_var->dtype == PLPGSQL_DTYPE_REC ||
		loop_var->dtype == PLPGSQL_DTYPE_ROW)
	{
		/*
		 * Record/row variable is certainly not of array type, and might not
		 * be initialized at all yet, so don't try to get its type
		 */
		loop_var_elem_type = InvalidOid;
	}
	else
		loop_var_elem_type = get_element_type(plpgsql_exec_get_datum_type(estate,
																  loop_var));

	/*
	 * Sanity-check the loop variable type.  We don't try very hard here, and
	 * should not be too picky since it's possible that exec_assign_value can
	 * coerce values of different types.  But it seems worthwhile to complain
	 * if the array-ness of the loop variable is not right.
	 */
	if (stmt->slice > 0 && loop_var_elem_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		errmsg("FOREACH ... SLICE loop variable must be of an array type")));
	if (stmt->slice == 0 && loop_var_elem_type != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
			  errmsg("FOREACH loop variable must not be of an array type")));

	/* Create an iterator to step through the array */
	array_iterator = array_create_iterator(arr, stmt->slice, NULL);

	/* Identify iterator result type */
	if (stmt->slice > 0)
	{
		/* When slicing, nominal type of result is same as array type */
		iterator_result_type = arrtype;
		iterator_result_typmod = arrtypmod;
	}
	else
	{
		/* Without slicing, results are individual array elements */
		iterator_result_type = ARR_ELEMTYPE(arr);
		iterator_result_typmod = arrtypmod;
	}

	/* Iterate over the array elements or slices */
	while (array_iterate(array_iterator, &value, &isnull))
	{
		found = true;			/* looped at least once */

		/* Assign current element/slice to the loop variable */
		exec_assign_value(estate, loop_var, value, isnull,
						  iterator_result_type, iterator_result_typmod);

		/* In slice case, value is temporary; must free it to avoid leakage */
		if (stmt->slice > 0)
			pfree(DatumGetPointer(value));

		/*
		 * Execute the statements
		 */
		rc = exec_stmts(estate, stmt->body);

		/* Handle the return code */
		if (rc == PLPGSQL_RC_RETURN)
			break;				/* break out of the loop */
		else if (rc == PLPGSQL_RC_EXIT)
		{
			if (estate->exitlabel == NULL)
				/* unlabelled exit, finish the current loop */
				rc = PLPGSQL_RC_OK;
			else if (stmt->label != NULL &&
					 strcmp(stmt->label, estate->exitlabel) == 0)
			{
				/* labelled exit, matches the current stmt's label */
				estate->exitlabel = NULL;
				rc = PLPGSQL_RC_OK;
			}

			/*
			 * otherwise, this is a labelled exit that does not match the
			 * current statement's label, if any: return RC_EXIT so that the
			 * EXIT continues to propagate up the stack.
			 */
			break;
		}
		else if (rc == PLPGSQL_RC_CONTINUE)
		{
			if (estate->exitlabel == NULL)
				/* unlabelled continue, so re-run the current loop */
				rc = PLPGSQL_RC_OK;
			else if (stmt->label != NULL &&
					 strcmp(stmt->label, estate->exitlabel) == 0)
			{
				/* label matches named continue, so re-run loop */
				estate->exitlabel = NULL;
				rc = PLPGSQL_RC_OK;
			}
			else
			{
				/*
				 * otherwise, this is a named continue that does not match the
				 * current statement's label, if any: return RC_CONTINUE so
				 * that the CONTINUE will propagate up the stack.
				 */
				break;
			}
		}
	}

	/* Release temporary memory, including the array value */
	array_free_iterator(array_iterator);
	pfree(arr);

	/*
	 * Set the FOUND variable to indicate the result of executing the loop
	 * (namely, whether we looped one or more times). This must be set here so
	 * that it does not interfere with the value of the FOUND variable inside
	 * the loop processing itself.
	 */
	exec_set_found(estate, found);

	return rc;
}


/* ----------
 * exec_stmt_exit			Implements EXIT and CONTINUE
 *
 * This begins the process of exiting / restarting a loop.
 * ----------
 */
static int
exec_stmt_exit(PLpgSQL_execstate *estate, PLpgSQL_stmt_exit *stmt)
{
	/*
	 * If the exit / continue has a condition, evaluate it
	 */
	if (stmt->cond != NULL)
	{
		bool		value;
		bool		isnull;

		value = exec_eval_boolean(estate, stmt->cond, &isnull);
		exec_eval_cleanup(estate);
		if (isnull || value == false)
			return PLPGSQL_RC_OK;
	}

	estate->exitlabel = stmt->label;
	if (stmt->is_exit)
		return PLPGSQL_RC_EXIT;
	else
		return PLPGSQL_RC_CONTINUE;
}


/* ----------
 * exec_stmt_return			Evaluate an expression and start
 *					returning from the function.
 * ----------
 */
static int
exec_stmt_return(PLpgSQL_execstate *estate, PLpgSQL_stmt_return *stmt)
{
	/*
	 * If processing a set-returning PL/pgSQL function, the final RETURN
	 * indicates that the function is finished producing tuples.  The rest of
	 * the work will be done at the top level.
	 */
	if (estate->retisset)
		return PLPGSQL_RC_RETURN;

	/* initialize for null result (possibly a tuple) */
	estate->retval = (Datum) 0;
	estate->rettupdesc = NULL;
	estate->retisnull = true;
	estate->rettype = InvalidOid;

	/*
	 * Special case path when the RETURN expression is a simple variable
	 * reference; in particular, this path is always taken in functions with
	 * one or more OUT parameters.
	 *
	 * This special case is especially efficient for returning variables that
	 * have R/W expanded values: we can put the R/W pointer directly into
	 * estate->retval, leading to transferring the value to the caller's
	 * context cheaply.  If we went through exec_eval_expr we'd end up with a
	 * R/O pointer.  It's okay to skip MakeExpandedObjectReadOnly here since
	 * we know we won't need the variable's value within the function anymore.
	 */
	if (stmt->retvarno >= 0)
	{
		PLpgSQL_datum *retvar = estate->datums[stmt->retvarno];

		switch (retvar->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) retvar;

					estate->retval = var->value;
					estate->retisnull = var->isnull;
					estate->rettype = var->datatype->typoid;

					/*
					 * Cope with retistuple case.  A PLpgSQL_var could not be
					 * of composite type, so we needn't make any effort to
					 * convert.  However, for consistency with the expression
					 * code path, don't throw error if the result is NULL.
					 */
					if (estate->retistuple && !estate->retisnull)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("cannot return non-composite value from function returning composite type")));
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) retvar;
					int32		rettypmod;

					if (HeapTupleIsValid(rec->tup))
					{
						if (estate->retistuple)
						{
							estate->retval = PointerGetDatum(rec->tup);
							estate->rettupdesc = rec->tupdesc;
							estate->retisnull = false;
						}
						else
							exec_eval_datum(estate,
											retvar,
											&estate->rettype,
											&rettypmod,
											&estate->retval,
											&estate->retisnull);
					}
				}
				break;

			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) retvar;
					int32		rettypmod;

					if (estate->retistuple)
					{
						HeapTuple	tup;

						if (!row->rowtupdesc)	/* should not happen */
							elog(ERROR, "row variable has no tupdesc");
						tup = make_tuple_from_row(estate, row, row->rowtupdesc);
						if (tup == NULL)		/* should not happen */
							elog(ERROR, "row not compatible with its own tupdesc");
						estate->retval = PointerGetDatum(tup);
						estate->rettupdesc = row->rowtupdesc;
						estate->retisnull = false;
					}
					else
						exec_eval_datum(estate,
										retvar,
										&estate->rettype,
										&rettypmod,
										&estate->retval,
										&estate->retisnull);
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", retvar->dtype);
		}

		return PLPGSQL_RC_RETURN;
	}

	if (stmt->expr != NULL)
	{
		int32		rettypmod;

		estate->retval = exec_eval_expr(estate, stmt->expr,
										&(estate->retisnull),
										&(estate->rettype),
										&rettypmod);

		if (estate->retistuple && !estate->retisnull)
		{
			/* Convert composite datum to a HeapTuple and TupleDesc */
			HeapTuple	tuple;
			TupleDesc	tupdesc;

			/* Source must be of RECORD or composite type */
			if (!type_is_rowtype(estate->rettype))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot return non-composite value from function returning composite type")));
			tuple = get_tuple_from_datum(estate->retval);
			tupdesc = get_tupdesc_from_datum(estate->retval);
			estate->retval = PointerGetDatum(tuple);
			estate->rettupdesc = CreateTupleDescCopy(tupdesc);
			ReleaseTupleDesc(tupdesc);
		}

		return PLPGSQL_RC_RETURN;
	}

	/*
	 * Special hack for function returning VOID: instead of NULL, return a
	 * non-null VOID value.  This is of dubious importance but is kept for
	 * backwards compatibility.
	 */
	if (estate->fn_rettype == VOIDOID)
	{
		estate->retval = (Datum) 0;
		estate->retisnull = false;
		estate->rettype = VOIDOID;
	}

	return PLPGSQL_RC_RETURN;
}

/* ----------
 * exec_stmt_return_next		Evaluate an expression and add it to the
 *								list of tuples returned by the current
 *								SRF.
 * ----------
 */
static int
exec_stmt_return_next(PLpgSQL_execstate *estate,
					  PLpgSQL_stmt_return_next *stmt)
{
	TupleDesc	tupdesc;
	int			natts;
	HeapTuple	tuple = NULL;
	bool		free_tuple = false;

	if (!estate->retisset)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use RETURN NEXT in a non-SETOF function")));

	if (estate->tuple_store == NULL)
		exec_init_tuple_store(estate);

	/* rettupdesc will be filled by exec_init_tuple_store */
	tupdesc = estate->rettupdesc;
	natts = tupdesc->natts;

	/*
	 * Special case path when the RETURN NEXT expression is a simple variable
	 * reference; in particular, this path is always taken in functions with
	 * one or more OUT parameters.
	 *
	 * Unlike exec_statement_return, there's no special win here for R/W
	 * expanded values, since they'll have to get flattened to go into the
	 * tuplestore.  Indeed, we'd better make them R/O to avoid any risk of the
	 * casting step changing them in-place.
	 */
	if (stmt->retvarno >= 0)
	{
		PLpgSQL_datum *retvar = estate->datums[stmt->retvarno];

		switch (retvar->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) retvar;
					Datum		retval = var->value;
					bool		isNull = var->isnull;

					if (natts != 1)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("wrong result type supplied in RETURN NEXT")));

					/* let's be very paranoid about the cast step */
					retval = MakeExpandedObjectReadOnly(retval,
														isNull,
													  var->datatype->typlen);

					/* coerce type if needed */
					retval = exec_cast_value(estate,
											 retval,
											 &isNull,
											 var->datatype->typoid,
											 var->datatype->atttypmod,
											 tupdesc->attrs[0]->atttypid,
											 tupdesc->attrs[0]->atttypmod);

					tuplestore_putvalues(estate->tuple_store, tupdesc,
										 &retval, &isNull);
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) retvar;
					TupleConversionMap *tupmap;

					if (!HeapTupleIsValid(rec->tup))
						ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						errdetail("The tuple structure of a not-yet-assigned"
								  " record is indeterminate.")));
					tupmap = convert_tuples_by_position(rec->tupdesc,
														tupdesc,
														gettext_noop("wrong record type supplied in RETURN NEXT"));
					tuple = rec->tup;
					/* it might need conversion */
					if (tupmap)
					{
						tuple = do_convert_tuple(tuple, tupmap);
						free_conversion_map(tupmap);
						free_tuple = true;
					}
				}
				break;

			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) retvar;

					tuple = make_tuple_from_row(estate, row, tupdesc);
					if (tuple == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("wrong record type supplied in RETURN NEXT")));
					free_tuple = true;
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", retvar->dtype);
				break;
		}
	}
	else if (stmt->expr)
	{
		Datum		retval;
		bool		isNull;
		Oid			rettype;
		int32		rettypmod;

		retval = exec_eval_expr(estate,
								stmt->expr,
								&isNull,
								&rettype,
								&rettypmod);

		if (estate->retistuple)
		{
			/* Expression should be of RECORD or composite type */
			if (!isNull)
			{
				TupleDesc	retvaldesc;
				TupleConversionMap *tupmap;

				if (!type_is_rowtype(rettype))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("cannot return non-composite value from function returning composite type")));

				tuple = get_tuple_from_datum(retval);
				free_tuple = true;		/* tuple is always freshly palloc'd */

				/* it might need conversion */
				retvaldesc = get_tupdesc_from_datum(retval);
				tupmap = convert_tuples_by_position(retvaldesc, tupdesc,
													gettext_noop("returned record type does not match expected record type"));
				if (tupmap)
				{
					HeapTuple	newtuple;

					newtuple = do_convert_tuple(tuple, tupmap);
					free_conversion_map(tupmap);
					heap_freetuple(tuple);
					tuple = newtuple;
				}
				ReleaseTupleDesc(retvaldesc);
				/* tuple will be stored into tuplestore below */
			}
			else
			{
				/* Composite NULL --- store a row of nulls */
				Datum	   *nulldatums;
				bool	   *nullflags;

				nulldatums = (Datum *) palloc0(natts * sizeof(Datum));
				nullflags = (bool *) palloc(natts * sizeof(bool));
				memset(nullflags, true, natts * sizeof(bool));
				tuplestore_putvalues(estate->tuple_store, tupdesc,
									 nulldatums, nullflags);
				pfree(nulldatums);
				pfree(nullflags);
			}
		}
		else
		{
			/* Simple scalar result */
			if (natts != 1)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
					   errmsg("wrong result type supplied in RETURN NEXT")));

			/* coerce type if needed */
			retval = exec_cast_value(estate,
									 retval,
									 &isNull,
									 rettype,
									 rettypmod,
									 tupdesc->attrs[0]->atttypid,
									 tupdesc->attrs[0]->atttypmod);

			tuplestore_putvalues(estate->tuple_store, tupdesc,
								 &retval, &isNull);
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("RETURN NEXT must have a parameter")));
	}

	if (HeapTupleIsValid(tuple))
	{
		tuplestore_puttuple(estate->tuple_store, tuple);

		if (free_tuple)
			heap_freetuple(tuple);
	}

	exec_eval_cleanup(estate);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_return_query		Evaluate a query and add it to the
 *								list of tuples returned by the current
 *								SRF.
 * ----------
 */
static int
exec_stmt_return_query(PLpgSQL_execstate *estate,
					   PLpgSQL_stmt_return_query *stmt)
{
	Portal		portal;
	uint64		processed = 0;
	TupleConversionMap *tupmap;

	if (!estate->retisset)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use RETURN QUERY in a non-SETOF function")));

	if (estate->tuple_store == NULL)
		exec_init_tuple_store(estate);

	if (stmt->query != NULL)
	{
		/* static query */
		exec_run_select(estate, stmt->query, 0, &portal, true);
	}
	else
	{
		/* RETURN QUERY EXECUTE */
		Assert(stmt->dynquery != NULL);
		portal = exec_dynquery_with_params(estate, stmt->dynquery,
										   stmt->params, NULL,
										   CURSOR_OPT_PARALLEL_OK);
	}

	tupmap = convert_tuples_by_position(portal->tupDesc,
										estate->rettupdesc,
	 gettext_noop("structure of query does not match function result type"));

	while (true)
	{
		uint64			i;

		SPI_cursor_fetch(portal, true, 50);
		if (SPI_processed == 0)
			break;

		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple	tuple = SPI_tuptable->vals[i];

			if (tupmap)
				tuple = do_convert_tuple(tuple, tupmap);
			tuplestore_puttuple(estate->tuple_store, tuple);
			if (tupmap)
				heap_freetuple(tuple);
			processed++;
		}

		SPI_freetuptable(SPI_tuptable);
	}

	if (tupmap)
		free_conversion_map(tupmap);

	SPI_freetuptable(SPI_tuptable);
	SPI_cursor_close(portal);

	estate->eval_processed = processed;
	exec_set_found(estate, processed != 0);

	return PLPGSQL_RC_OK;
}

static void
exec_init_tuple_store(PLpgSQL_execstate *estate)
{
	ReturnSetInfo *rsi = estate->rsi;
	MemoryContext oldcxt;
	ResourceOwner oldowner;

	/*
	 * Check caller can handle a set result in the way we want
	 */
	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	/*
	 * Switch to the right memory context and resource owner for storing the
	 * tuplestore for return set. If we're within a subtransaction opened for
	 * an exception-block, for example, we must still create the tuplestore in
	 * the resource owner that was active when this function was entered, and
	 * not in the subtransaction resource owner.
	 */
	oldcxt = MemoryContextSwitchTo(estate->tuple_store_cxt);
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = estate->tuple_store_owner;

	estate->tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	CurrentResourceOwner = oldowner;
	MemoryContextSwitchTo(oldcxt);

	estate->rettupdesc = rsi->expectedDesc;
}

#define SET_RAISE_OPTION_TEXT(opt, name) \
do { \
	if (opt) \
		ereport(ERROR, \
				(errcode(ERRCODE_SYNTAX_ERROR), \
				 errmsg("RAISE option already specified: %s", \
						name))); \
	opt = pstrdup(extval); \
} while (0)

/* ----------
 * exec_stmt_raise			Build a message and throw it with elog()
 * ----------
 */
static int
exec_stmt_raise(PLpgSQL_execstate *estate, PLpgSQL_stmt_raise *stmt)
{
	int			err_code = 0;
	char	   *condname = NULL;
	char	   *err_message = NULL;
	char	   *err_detail = NULL;
	char	   *err_hint = NULL;
	char	   *err_column = NULL;
	char	   *err_constraint = NULL;
	char	   *err_datatype = NULL;
	char	   *err_table = NULL;
	char	   *err_schema = NULL;
	ListCell   *lc;

	/* RAISE with no parameters: re-throw current exception */
	if (stmt->condname == NULL && stmt->message == NULL &&
		stmt->options == NIL)
	{
		if (estate->cur_error != NULL)
			ReThrowError(estate->cur_error);
		/* oops, we're not inside a handler */
		ereport(ERROR,
		(errcode(ERRCODE_STACKED_DIAGNOSTICS_ACCESSED_WITHOUT_ACTIVE_HANDLER),
		 errmsg("RAISE without parameters cannot be used outside an exception handler")));
	}

	if (stmt->condname)
	{
		err_code = plpgsql_recognize_err_condition(stmt->condname, true);
		condname = pstrdup(stmt->condname);
	}

	if (stmt->message)
	{
		StringInfoData ds;
		ListCell   *current_param;
		char	   *cp;

		initStringInfo(&ds);
		current_param = list_head(stmt->params);

		for (cp = stmt->message; *cp; cp++)
		{
			/*
			 * Occurrences of a single % are replaced by the next parameter's
			 * external representation. Double %'s are converted to one %.
			 */
			if (cp[0] == '%')
			{
				Oid			paramtypeid;
				int32		paramtypmod;
				Datum		paramvalue;
				bool		paramisnull;
				char	   *extval;

				if (cp[1] == '%')
				{
					appendStringInfoChar(&ds, '%');
					cp++;
					continue;
				}

				/* should have been checked at compile time */
				if (current_param == NULL)
					elog(ERROR, "unexpected RAISE parameter list length");

				paramvalue = exec_eval_expr(estate,
									  (PLpgSQL_expr *) lfirst(current_param),
											&paramisnull,
											&paramtypeid,
											&paramtypmod);

				if (paramisnull)
					extval = "<NULL>";
				else
					extval = convert_value_to_string(estate,
													 paramvalue,
													 paramtypeid);
				appendStringInfoString(&ds, extval);
				current_param = lnext(current_param);
				exec_eval_cleanup(estate);
			}
			else
				appendStringInfoChar(&ds, cp[0]);
		}

		/* should have been checked at compile time */
		if (current_param != NULL)
			elog(ERROR, "unexpected RAISE parameter list length");

		err_message = ds.data;
		/* No pfree(ds.data), the pfree(err_message) does it */
	}

	foreach(lc, stmt->options)
	{
		PLpgSQL_raise_option *opt = (PLpgSQL_raise_option *) lfirst(lc);
		Datum		optionvalue;
		bool		optionisnull;
		Oid			optiontypeid;
		int32		optiontypmod;
		char	   *extval;

		optionvalue = exec_eval_expr(estate, opt->expr,
									 &optionisnull,
									 &optiontypeid,
									 &optiontypmod);
		if (optionisnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("RAISE statement option cannot be null")));

		extval = convert_value_to_string(estate, optionvalue, optiontypeid);

		switch (opt->opt_type)
		{
			case PLPGSQL_RAISEOPTION_ERRCODE:
				if (err_code)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("RAISE option already specified: %s",
									"ERRCODE")));
				err_code = plpgsql_recognize_err_condition(extval, true);
				condname = pstrdup(extval);
				break;
			case PLPGSQL_RAISEOPTION_MESSAGE:
				SET_RAISE_OPTION_TEXT(err_message, "MESSAGE");
				break;
			case PLPGSQL_RAISEOPTION_DETAIL:
				SET_RAISE_OPTION_TEXT(err_detail, "DETAIL");
				break;
			case PLPGSQL_RAISEOPTION_HINT:
				SET_RAISE_OPTION_TEXT(err_hint, "HINT");
				break;
			case PLPGSQL_RAISEOPTION_COLUMN:
				SET_RAISE_OPTION_TEXT(err_column, "COLUMN");
				break;
			case PLPGSQL_RAISEOPTION_CONSTRAINT:
				SET_RAISE_OPTION_TEXT(err_constraint, "CONSTRAINT");
				break;
			case PLPGSQL_RAISEOPTION_DATATYPE:
				SET_RAISE_OPTION_TEXT(err_datatype, "DATATYPE");
				break;
			case PLPGSQL_RAISEOPTION_TABLE:
				SET_RAISE_OPTION_TEXT(err_table, "TABLE");
				break;
			case PLPGSQL_RAISEOPTION_SCHEMA:
				SET_RAISE_OPTION_TEXT(err_schema, "SCHEMA");
				break;
			default:
				elog(ERROR, "unrecognized raise option: %d", opt->opt_type);
		}

		exec_eval_cleanup(estate);
	}

	/* Default code if nothing specified */
	if (err_code == 0 && stmt->elog_level >= ERROR)
		err_code = ERRCODE_RAISE_EXCEPTION;

	/* Default error message if nothing specified */
	if (err_message == NULL)
	{
		if (condname)
		{
			err_message = condname;
			condname = NULL;
		}
		else
			err_message = pstrdup(unpack_sql_state(err_code));
	}

	/*
	 * Throw the error (may or may not come back)
	 */
	ereport(stmt->elog_level,
			(err_code ? errcode(err_code) : 0,
			 errmsg_internal("%s", err_message),
			 (err_detail != NULL) ? errdetail_internal("%s", err_detail) : 0,
			 (err_hint != NULL) ? errhint("%s", err_hint) : 0,
			 (err_column != NULL) ?
			 err_generic_string(PG_DIAG_COLUMN_NAME, err_column) : 0,
			 (err_constraint != NULL) ?
			 err_generic_string(PG_DIAG_CONSTRAINT_NAME, err_constraint) : 0,
			 (err_datatype != NULL) ?
			 err_generic_string(PG_DIAG_DATATYPE_NAME, err_datatype) : 0,
			 (err_table != NULL) ?
			 err_generic_string(PG_DIAG_TABLE_NAME, err_table) : 0,
			 (err_schema != NULL) ?
			 err_generic_string(PG_DIAG_SCHEMA_NAME, err_schema) : 0));

	if (condname != NULL)
		pfree(condname);
	if (err_message != NULL)
		pfree(err_message);
	if (err_detail != NULL)
		pfree(err_detail);
	if (err_hint != NULL)
		pfree(err_hint);
	if (err_column != NULL)
		pfree(err_column);
	if (err_constraint != NULL)
		pfree(err_constraint);
	if (err_datatype != NULL)
		pfree(err_datatype);
	if (err_table != NULL)
		pfree(err_table);
	if (err_schema != NULL)
		pfree(err_schema);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_assert			Assert statement
 * ----------
 */
static int
exec_stmt_assert(PLpgSQL_execstate *estate, PLpgSQL_stmt_assert *stmt)
{
	bool		value;
	bool		isnull;

	/* do nothing when asserts are not enabled */
	if (!plpgsql_check_asserts)
		return PLPGSQL_RC_OK;

	value = exec_eval_boolean(estate, stmt->cond, &isnull);
	exec_eval_cleanup(estate);

	if (isnull || !value)
	{
		char	   *message = NULL;

		if (stmt->message != NULL)
		{
			Datum		val;
			Oid			typeid;
			int32		typmod;

			val = exec_eval_expr(estate, stmt->message,
								 &isnull, &typeid, &typmod);
			if (!isnull)
				message = convert_value_to_string(estate, val, typeid);
			/* we mustn't do exec_eval_cleanup here */
		}

		ereport(ERROR,
				(errcode(ERRCODE_ASSERT_FAILURE),
				 message ? errmsg_internal("%s", message) :
				 errmsg("assertion failed")));
	}

	return PLPGSQL_RC_OK;
}

/* ----------
 * Initialize a mostly empty execution state
 * ----------
 */
static void
plpgsql_estate_setup(PLpgSQL_execstate *estate,
					 PLpgSQL_function *func,
					 ReturnSetInfo *rsi,
					 EState *simple_eval_estate)
{
	HASHCTL		ctl;

	/* this link will be restored at exit from plpgsql_call_handler */
	func->cur_estate = estate;

	estate->func = func;

	estate->retval = (Datum) 0;
	estate->retisnull = true;
	estate->rettype = InvalidOid;

	estate->fn_rettype = func->fn_rettype;
	estate->retistuple = func->fn_retistuple;
	estate->retisset = func->fn_retset;

	estate->readonly_func = func->fn_readonly;

	estate->rettupdesc = NULL;
	estate->exitlabel = NULL;
	estate->cur_error = NULL;

	estate->tuple_store = NULL;
	if (rsi)
	{
		estate->tuple_store_cxt = rsi->econtext->ecxt_per_query_memory;
		estate->tuple_store_owner = CurrentResourceOwner;
	}
	else
	{
		estate->tuple_store_cxt = NULL;
		estate->tuple_store_owner = NULL;
	}
	estate->rsi = rsi;

	estate->found_varno = func->found_varno;
	estate->ndatums = func->ndatums;
	estate->datums = palloc(sizeof(PLpgSQL_datum *) * estate->ndatums);
	/* caller is expected to fill the datums array */

	/* initialize ParamListInfo with one entry per datum, all invalid */
	estate->paramLI = (ParamListInfo)
		palloc0(offsetof(ParamListInfoData, params) +
				estate->ndatums * sizeof(ParamExternData));
	estate->paramLI->paramFetch = plpgsql_param_fetch;
	estate->paramLI->paramFetchArg = (void *) estate;
	estate->paramLI->parserSetup = (ParserSetupHook) plpgsql_parser_setup;
	estate->paramLI->parserSetupArg = NULL;		/* filled during use */
	estate->paramLI->numParams = estate->ndatums;
	estate->paramLI->paramMask = NULL;
	estate->params_dirty = false;

	/* set up for use of appropriate simple-expression EState and cast hash */
	if (simple_eval_estate)
	{
		estate->simple_eval_estate = simple_eval_estate;
		/* Private cast hash just lives in function's main context */
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(plpgsql_CastHashKey);
		ctl.entrysize = sizeof(plpgsql_CastHashEntry);
		ctl.hcxt = CurrentMemoryContext;
		estate->cast_hash = hash_create("PLpgSQL private cast cache",
										16,		/* start small and extend */
										&ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		estate->cast_hash_context = CurrentMemoryContext;
	}
	else
	{
		estate->simple_eval_estate = shared_simple_eval_estate;
		/* Create the session-wide cast-info hash table if we didn't already */
		if (shared_cast_hash == NULL)
		{
			shared_cast_context = AllocSetContextCreate(TopMemoryContext,
														"PLpgSQL cast info",
													ALLOCSET_DEFAULT_MINSIZE,
												   ALLOCSET_DEFAULT_INITSIZE,
												   ALLOCSET_DEFAULT_MAXSIZE);
			memset(&ctl, 0, sizeof(ctl));
			ctl.keysize = sizeof(plpgsql_CastHashKey);
			ctl.entrysize = sizeof(plpgsql_CastHashEntry);
			ctl.hcxt = shared_cast_context;
			shared_cast_hash = hash_create("PLpgSQL cast cache",
										   16,	/* start small and extend */
										   &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		}
		estate->cast_hash = shared_cast_hash;
		estate->cast_hash_context = shared_cast_context;
	}

	estate->eval_tuptable = NULL;
	estate->eval_processed = 0;
	estate->eval_lastoid = InvalidOid;
	estate->eval_econtext = NULL;

	estate->err_stmt = NULL;
	estate->err_text = NULL;

	estate->plugin_info = NULL;

	/*
	 * Create an EState and ExprContext for evaluation of simple expressions.
	 */
	plpgsql_create_econtext(estate);

	/*
	 * Let the plugin see this function before we initialize any local
	 * PL/pgSQL variables - note that we also give the plugin a few function
	 * pointers so it can call back into PL/pgSQL for doing things like
	 * variable assignments and stack traces
	 */
	if (*plpgsql_plugin_ptr)
	{
		(*plpgsql_plugin_ptr)->error_callback = plpgsql_exec_error_callback;
		(*plpgsql_plugin_ptr)->assign_expr = exec_assign_expr;

		if ((*plpgsql_plugin_ptr)->func_setup)
			((*plpgsql_plugin_ptr)->func_setup) (estate, func);
	}
}

/* ----------
 * Release temporary memory used by expression/subselect evaluation
 *
 * NB: the result of the evaluation is no longer valid after this is done,
 * unless it is a pass-by-value datatype.
 *
 * NB: if you change this code, see also the hacks in exec_assign_value's
 * PLPGSQL_DTYPE_ARRAYELEM case for partial cleanup after subscript evals.
 * ----------
 */
static void
exec_eval_cleanup(PLpgSQL_execstate *estate)
{
	/* Clear result of a full SPI_execute */
	if (estate->eval_tuptable != NULL)
		SPI_freetuptable(estate->eval_tuptable);
	estate->eval_tuptable = NULL;

	/* Clear result of exec_eval_simple_expr (but keep the econtext) */
	if (estate->eval_econtext != NULL)
		ResetExprContext(estate->eval_econtext);
}


/* ----------
 * Generate a prepared plan
 * ----------
 */
static void
exec_prepare_plan(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr, int cursorOptions)
{
	SPIPlanPtr	plan;

	/*
	 * The grammar can't conveniently set expr->func while building the parse
	 * tree, so make sure it's set before parser hooks need it.
	 */
	expr->func = estate->func;

	/*
	 * Generate and save the plan
	 */
	plan = SPI_prepare_params(expr->query,
							  (ParserSetupHook) plpgsql_parser_setup,
							  (void *) expr,
							  cursorOptions);
	if (plan == NULL)
	{
		/* Some SPI errors deserve specific error messages */
		switch (SPI_result)
		{
			case SPI_ERROR_COPY:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot COPY to/from client in PL/pgSQL")));
			case SPI_ERROR_TRANSACTION:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot begin/end transactions in PL/pgSQL"),
						 errhint("Use a BEGIN block with an EXCEPTION clause instead.")));
			default:
				elog(ERROR, "SPI_prepare_params failed for \"%s\": %s",
					 expr->query, SPI_result_code_string(SPI_result));
		}
	}
	SPI_keepplan(plan);
	expr->plan = plan;

	/* Check to see if it's a simple expression */
	exec_simple_check_plan(expr);

	/*
	 * Mark expression as not using a read-write param.  exec_assign_value has
	 * to take steps to override this if appropriate; that seems cleaner than
	 * adding parameters to all other callers.
	 */
	expr->rwparam = -1;
}


/* ----------
 * exec_stmt_execsql			Execute an SQL statement (possibly with INTO).
 * ----------
 */
static int
exec_stmt_execsql(PLpgSQL_execstate *estate,
				  PLpgSQL_stmt_execsql *stmt)
{
	ParamListInfo paramLI;
	long		tcount;
	int			rc;
	PLpgSQL_expr *expr = stmt->sqlstmt;

	/*
	 * On the first call for this statement generate the plan, and detect
	 * whether the statement is INSERT/UPDATE/DELETE
	 */
	if (expr->plan == NULL)
	{
		ListCell   *l;

		exec_prepare_plan(estate, expr, 0);
		stmt->mod_stmt = false;
		foreach(l, SPI_plan_get_plan_sources(expr->plan))
		{
			CachedPlanSource *plansource = (CachedPlanSource *) lfirst(l);
			ListCell   *l2;

			foreach(l2, plansource->query_list)
			{
				Query	   *q = (Query *) lfirst(l2);

				Assert(IsA(q, Query));
				if (q->canSetTag)
				{
					if (q->commandType == CMD_INSERT ||
						q->commandType == CMD_UPDATE ||
						q->commandType == CMD_DELETE)
						stmt->mod_stmt = true;
				}
			}
		}
	}

	/*
	 * Set up ParamListInfo to pass to executor
	 */
	paramLI = setup_param_list(estate, expr);

	/*
	 * If we have INTO, then we only need one row back ... but if we have INTO
	 * STRICT, ask for two rows, so that we can verify the statement returns
	 * only one.  INSERT/UPDATE/DELETE are always treated strictly. Without
	 * INTO, just run the statement to completion (tcount = 0).
	 *
	 * We could just ask for two rows always when using INTO, but there are
	 * some cases where demanding the extra row costs significant time, eg by
	 * forcing completion of a sequential scan.  So don't do it unless we need
	 * to enforce strictness.
	 */
	if (stmt->into)
	{
		if (stmt->strict || stmt->mod_stmt)
			tcount = 2;
		else
			tcount = 1;
	}
	else
		tcount = 0;

	/*
	 * Execute the plan
	 */
	rc = SPI_execute_plan_with_paramlist(expr->plan, paramLI,
										 estate->readonly_func, tcount);

	/*
	 * Check for error, and set FOUND if appropriate (for historical reasons
	 * we set FOUND only for certain query types).  Also Assert that we
	 * identified the statement type the same as SPI did.
	 */
	switch (rc)
	{
		case SPI_OK_SELECT:
			Assert(!stmt->mod_stmt);
			exec_set_found(estate, (SPI_processed != 0));
			break;

		case SPI_OK_INSERT:
		case SPI_OK_UPDATE:
		case SPI_OK_DELETE:
		case SPI_OK_INSERT_RETURNING:
		case SPI_OK_UPDATE_RETURNING:
		case SPI_OK_DELETE_RETURNING:
			Assert(stmt->mod_stmt);
			exec_set_found(estate, (SPI_processed != 0));
			break;

		case SPI_OK_SELINTO:
		case SPI_OK_UTILITY:
			Assert(!stmt->mod_stmt);
			break;

		case SPI_OK_REWRITTEN:
			Assert(!stmt->mod_stmt);

			/*
			 * The command was rewritten into another kind of command. It's
			 * not clear what FOUND would mean in that case (and SPI doesn't
			 * return the row count either), so just set it to false.
			 */
			exec_set_found(estate, false);
			break;

			/* Some SPI errors deserve specific error messages */
		case SPI_ERROR_COPY:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot COPY to/from client in PL/pgSQL")));
		case SPI_ERROR_TRANSACTION:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot begin/end transactions in PL/pgSQL"),
			errhint("Use a BEGIN block with an EXCEPTION clause instead.")));

		default:
			elog(ERROR, "SPI_execute_plan_with_paramlist failed executing query \"%s\": %s",
				 expr->query, SPI_result_code_string(rc));
	}

	/* All variants should save result info for GET DIAGNOSTICS */
	estate->eval_processed = SPI_processed;
	estate->eval_lastoid = SPI_lastoid;

	/* Process INTO if present */
	if (stmt->into)
	{
		SPITupleTable *tuptab = SPI_tuptable;
		uint64		n = SPI_processed;
		PLpgSQL_rec *rec = NULL;
		PLpgSQL_row *row = NULL;

		/* If the statement did not return a tuple table, complain */
		if (tuptab == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("INTO used with a command that cannot return data")));

		/* Determine if we assign to a record or a row */
		if (stmt->rec != NULL)
			rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->dno]);
		else if (stmt->row != NULL)
			row = (PLpgSQL_row *) (estate->datums[stmt->row->dno]);
		else
			elog(ERROR, "unsupported target");

		/*
		 * If SELECT ... INTO specified STRICT, and the query didn't find
		 * exactly one row, throw an error.  If STRICT was not specified, then
		 * allow the query to find any number of rows.
		 */
		if (n == 0)
		{
			if (stmt->strict)
			{
				char	   *errdetail;

				if (estate->func->print_strict_params)
					errdetail = format_expr_params(estate, expr);
				else
					errdetail = NULL;

				ereport(ERROR,
						(errcode(ERRCODE_NO_DATA_FOUND),
						 errmsg("query returned no rows"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0));
			}
			/* set the target to NULL(s) */
			exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
		}
		else
		{
			if (n > 1 && (stmt->strict || stmt->mod_stmt))
			{
				char	   *errdetail;

				if (estate->func->print_strict_params)
					errdetail = format_expr_params(estate, expr);
				else
					errdetail = NULL;

				ereport(ERROR,
						(errcode(ERRCODE_TOO_MANY_ROWS),
						 errmsg("query returned more than one row"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0));
			}
			/* Put the first result row into the target */
			exec_move_row(estate, rec, row, tuptab->vals[0], tuptab->tupdesc);
		}

		/* Clean up */
		exec_eval_cleanup(estate);
		SPI_freetuptable(SPI_tuptable);
	}
	else
	{
		/* If the statement returned a tuple table, complain */
		if (SPI_tuptable != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("query has no destination for result data"),
					 (rc == SPI_OK_SELECT) ? errhint("If you want to discard the results of a SELECT, use PERFORM instead.") : 0));
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_dynexecute			Execute a dynamic SQL query
 *					(possibly with INTO).
 * ----------
 */
static int
exec_stmt_dynexecute(PLpgSQL_execstate *estate,
					 PLpgSQL_stmt_dynexecute *stmt)
{
	Datum		query;
	bool		isnull;
	Oid			restype;
	int32		restypmod;
	char	   *querystr;
	int			exec_res;
	PreparedParamsData *ppd = NULL;

	/*
	 * First we evaluate the string expression after the EXECUTE keyword. Its
	 * result is the querystring we have to execute.
	 */
	query = exec_eval_expr(estate, stmt->query, &isnull, &restype, &restypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("query string argument of EXECUTE is null")));

	/* Get the C-String representation */
	querystr = convert_value_to_string(estate, query, restype);

	/* copy it out of the temporary context before we clean up */
	querystr = pstrdup(querystr);

	exec_eval_cleanup(estate);

	/*
	 * Execute the query without preparing a saved plan.
	 */
	if (stmt->params)
	{
		ppd = exec_eval_using_params(estate, stmt->params);
		exec_res = SPI_execute_with_args(querystr,
										 ppd->nargs, ppd->types,
										 ppd->values, ppd->nulls,
										 estate->readonly_func, 0);
	}
	else
		exec_res = SPI_execute(querystr, estate->readonly_func, 0);

	switch (exec_res)
	{
		case SPI_OK_SELECT:
		case SPI_OK_INSERT:
		case SPI_OK_UPDATE:
		case SPI_OK_DELETE:
		case SPI_OK_INSERT_RETURNING:
		case SPI_OK_UPDATE_RETURNING:
		case SPI_OK_DELETE_RETURNING:
		case SPI_OK_UTILITY:
		case SPI_OK_REWRITTEN:
			break;

		case 0:

			/*
			 * Also allow a zero return, which implies the querystring
			 * contained no commands.
			 */
			break;

		case SPI_OK_SELINTO:

			/*
			 * We want to disallow SELECT INTO for now, because its behavior
			 * is not consistent with SELECT INTO in a normal plpgsql context.
			 * (We need to reimplement EXECUTE to parse the string as a
			 * plpgsql command, not just feed it to SPI_execute.)  This is not
			 * a functional limitation because CREATE TABLE AS is allowed.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("EXECUTE of SELECT ... INTO is not implemented"),
					 errhint("You might want to use EXECUTE ... INTO or EXECUTE CREATE TABLE ... AS instead.")));
			break;

			/* Some SPI errors deserve specific error messages */
		case SPI_ERROR_COPY:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot COPY to/from client in PL/pgSQL")));
		case SPI_ERROR_TRANSACTION:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot begin/end transactions in PL/pgSQL"),
			errhint("Use a BEGIN block with an EXCEPTION clause instead.")));

		default:
			elog(ERROR, "SPI_execute failed executing query \"%s\": %s",
				 querystr, SPI_result_code_string(exec_res));
			break;
	}

	/* Save result info for GET DIAGNOSTICS */
	estate->eval_processed = SPI_processed;
	estate->eval_lastoid = SPI_lastoid;

	/* Process INTO if present */
	if (stmt->into)
	{
		SPITupleTable *tuptab = SPI_tuptable;
		uint64		n = SPI_processed;
		PLpgSQL_rec *rec = NULL;
		PLpgSQL_row *row = NULL;

		/* If the statement did not return a tuple table, complain */
		if (tuptab == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("INTO used with a command that cannot return data")));

		/* Determine if we assign to a record or a row */
		if (stmt->rec != NULL)
			rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->dno]);
		else if (stmt->row != NULL)
			row = (PLpgSQL_row *) (estate->datums[stmt->row->dno]);
		else
			elog(ERROR, "unsupported target");

		/*
		 * If SELECT ... INTO specified STRICT, and the query didn't find
		 * exactly one row, throw an error.  If STRICT was not specified, then
		 * allow the query to find any number of rows.
		 */
		if (n == 0)
		{
			if (stmt->strict)
			{
				char	   *errdetail;

				if (estate->func->print_strict_params)
					errdetail = format_preparedparamsdata(estate, ppd);
				else
					errdetail = NULL;

				ereport(ERROR,
						(errcode(ERRCODE_NO_DATA_FOUND),
						 errmsg("query returned no rows"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0));
			}
			/* set the target to NULL(s) */
			exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
		}
		else
		{
			if (n > 1 && stmt->strict)
			{
				char	   *errdetail;

				if (estate->func->print_strict_params)
					errdetail = format_preparedparamsdata(estate, ppd);
				else
					errdetail = NULL;

				ereport(ERROR,
						(errcode(ERRCODE_TOO_MANY_ROWS),
						 errmsg("query returned more than one row"),
						 errdetail ? errdetail_internal("parameters: %s", errdetail) : 0));
			}

			/* Put the first result row into the target */
			exec_move_row(estate, rec, row, tuptab->vals[0], tuptab->tupdesc);
		}
		/* clean up after exec_move_row() */
		exec_eval_cleanup(estate);
	}
	else
	{
		/*
		 * It might be a good idea to raise an error if the query returned
		 * tuples that are being ignored, but historically we have not done
		 * that.
		 */
	}

	if (ppd)
		free_params_data(ppd);

	/* Release any result from SPI_execute, as well as the querystring */
	SPI_freetuptable(SPI_tuptable);
	pfree(querystr);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_dynfors			Execute a dynamic query, assign each
 *					tuple to a record or row and
 *					execute a group of statements
 *					for it.
 * ----------
 */
static int
exec_stmt_dynfors(PLpgSQL_execstate *estate, PLpgSQL_stmt_dynfors *stmt)
{
	Portal		portal;
	int			rc;

	portal = exec_dynquery_with_params(estate, stmt->query, stmt->params,
									   NULL, 0);

	/*
	 * Execute the loop
	 */
	rc = exec_for_query(estate, (PLpgSQL_stmt_forq *) stmt, portal, true);

	/*
	 * Close the implicit cursor
	 */
	SPI_cursor_close(portal);

	return rc;
}


/* ----------
 * exec_stmt_open			Execute an OPEN cursor statement
 * ----------
 */
static int
exec_stmt_open(PLpgSQL_execstate *estate, PLpgSQL_stmt_open *stmt)
{
	PLpgSQL_var *curvar;
	char	   *curname = NULL;
	PLpgSQL_expr *query;
	Portal		portal;
	ParamListInfo paramLI;

	/* ----------
	 * Get the cursor variable and if it has an assigned name, check
	 * that it's not in use currently.
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (!curvar->isnull)
	{
		curname = TextDatumGetCString(curvar->value);
		if (SPI_cursor_find(curname) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("cursor \"%s\" already in use", curname)));
	}

	/* ----------
	 * Process the OPEN according to it's type.
	 * ----------
	 */
	if (stmt->query != NULL)
	{
		/* ----------
		 * This is an OPEN refcursor FOR SELECT ...
		 *
		 * We just make sure the query is planned. The real work is
		 * done downstairs.
		 * ----------
		 */
		query = stmt->query;
		if (query->plan == NULL)
			exec_prepare_plan(estate, query, stmt->cursor_options);
	}
	else if (stmt->dynquery != NULL)
	{
		/* ----------
		 * This is an OPEN refcursor FOR EXECUTE ...
		 * ----------
		 */
		portal = exec_dynquery_with_params(estate,
										   stmt->dynquery,
										   stmt->params,
										   curname,
										   stmt->cursor_options);

		/*
		 * If cursor variable was NULL, store the generated portal name in it
		 */
		if (curname == NULL)
			assign_text_var(estate, curvar, portal->name);

		return PLPGSQL_RC_OK;
	}
	else
	{
		/* ----------
		 * This is an OPEN cursor
		 *
		 * Note: parser should already have checked that statement supplies
		 * args iff cursor needs them, but we check again to be safe.
		 * ----------
		 */
		if (stmt->argquery != NULL)
		{
			/* ----------
			 * OPEN CURSOR with args.  We fake a SELECT ... INTO ...
			 * statement to evaluate the args and put 'em into the
			 * internal row.
			 * ----------
			 */
			PLpgSQL_stmt_execsql set_args;

			if (curvar->cursor_explicit_argrow < 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("arguments given for cursor without arguments")));

			memset(&set_args, 0, sizeof(set_args));
			set_args.cmd_type = PLPGSQL_STMT_EXECSQL;
			set_args.lineno = stmt->lineno;
			set_args.sqlstmt = stmt->argquery;
			set_args.into = true;
			/* XXX historically this has not been STRICT */
			set_args.row = (PLpgSQL_row *)
				(estate->datums[curvar->cursor_explicit_argrow]);

			if (exec_stmt_execsql(estate, &set_args) != PLPGSQL_RC_OK)
				elog(ERROR, "open cursor failed during argument processing");
		}
		else
		{
			if (curvar->cursor_explicit_argrow >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("arguments required for cursor")));
		}

		query = curvar->cursor_explicit_expr;
		if (query->plan == NULL)
			exec_prepare_plan(estate, query, curvar->cursor_options);
	}

	/*
	 * Set up short-lived ParamListInfo
	 */
	paramLI = setup_unshared_param_list(estate, query);

	/*
	 * Open the cursor
	 */
	portal = SPI_cursor_open_with_paramlist(curname, query->plan,
											paramLI,
											estate->readonly_func);
	if (portal == NULL)
		elog(ERROR, "could not open cursor: %s",
			 SPI_result_code_string(SPI_result));

	/*
	 * If cursor variable was NULL, store the generated portal name in it
	 */
	if (curname == NULL)
		assign_text_var(estate, curvar, portal->name);

	if (curname)
		pfree(curname);
	if (paramLI)
		pfree(paramLI);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_fetch			Fetch from a cursor into a target, or just
 *							move the current position of the cursor
 * ----------
 */
static int
exec_stmt_fetch(PLpgSQL_execstate *estate, PLpgSQL_stmt_fetch *stmt)
{
	PLpgSQL_var *curvar = NULL;
	PLpgSQL_rec *rec = NULL;
	PLpgSQL_row *row = NULL;
	long		how_many = stmt->how_many;
	SPITupleTable *tuptab;
	Portal		portal;
	char	   *curname;
	uint64		n;

	/* ----------
	 * Get the portal of the cursor by name
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (curvar->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cursor variable \"%s\" is null", curvar->refname)));
	curname = TextDatumGetCString(curvar->value);

	portal = SPI_cursor_find(curname);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", curname)));
	pfree(curname);

	/* Calculate position for FETCH_RELATIVE or FETCH_ABSOLUTE */
	if (stmt->expr)
	{
		bool		isnull;

		/* XXX should be doing this in LONG not INT width */
		how_many = exec_eval_integer(estate, stmt->expr, &isnull);

		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("relative or absolute cursor position is null")));

		exec_eval_cleanup(estate);
	}

	if (!stmt->is_move)
	{
		/* ----------
		 * Determine if we fetch into a record or a row
		 * ----------
		 */
		if (stmt->rec != NULL)
			rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->dno]);
		else if (stmt->row != NULL)
			row = (PLpgSQL_row *) (estate->datums[stmt->row->dno]);
		else
			elog(ERROR, "unsupported target");

		/* ----------
		 * Fetch 1 tuple from the cursor
		 * ----------
		 */
		SPI_scroll_cursor_fetch(portal, stmt->direction, how_many);
		tuptab = SPI_tuptable;
		n = SPI_processed;

		/* ----------
		 * Set the target appropriately.
		 * ----------
		 */
		if (n == 0)
			exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
		else
			exec_move_row(estate, rec, row, tuptab->vals[0], tuptab->tupdesc);

		exec_eval_cleanup(estate);
		SPI_freetuptable(tuptab);
	}
	else
	{
		/* Move the cursor */
		SPI_scroll_cursor_move(portal, stmt->direction, how_many);
		n = SPI_processed;
	}

	/* Set the ROW_COUNT and the global FOUND variable appropriately. */
	estate->eval_processed = n;
	exec_set_found(estate, n != 0);

	return PLPGSQL_RC_OK;
}

/* ----------
 * exec_stmt_close			Close a cursor
 * ----------
 */
static int
exec_stmt_close(PLpgSQL_execstate *estate, PLpgSQL_stmt_close *stmt)
{
	PLpgSQL_var *curvar = NULL;
	Portal		portal;
	char	   *curname;

	/* ----------
	 * Get the portal of the cursor by name
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (curvar->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cursor variable \"%s\" is null", curvar->refname)));
	curname = TextDatumGetCString(curvar->value);

	portal = SPI_cursor_find(curname);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", curname)));
	pfree(curname);

	/* ----------
	 * And close it.
	 * ----------
	 */
	SPI_cursor_close(portal);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_assign_expr			Put an expression's result into a variable.
 * ----------
 */
static void
exec_assign_expr(PLpgSQL_execstate *estate, PLpgSQL_datum *target,
				 PLpgSQL_expr *expr)
{
	Datum		value;
	bool		isnull;
	Oid			valtype;
	int32		valtypmod;

	/*
	 * If first time through, create a plan for this expression, and then see
	 * if we can pass the target variable as a read-write parameter to the
	 * expression.  (This is a bit messy, but it seems cleaner than modifying
	 * the API of exec_eval_expr for the purpose.)
	 */
	if (expr->plan == NULL)
	{
		exec_prepare_plan(estate, expr, 0);
		if (target->dtype == PLPGSQL_DTYPE_VAR)
			exec_check_rw_parameter(expr, target->dno);
	}

	value = exec_eval_expr(estate, expr, &isnull, &valtype, &valtypmod);
	exec_assign_value(estate, target, value, isnull, valtype, valtypmod);
	exec_eval_cleanup(estate);
}


/* ----------
 * exec_assign_c_string		Put a C string into a text variable.
 *
 * We take a NULL pointer as signifying empty string, not SQL null.
 * ----------
 */
static void
exec_assign_c_string(PLpgSQL_execstate *estate, PLpgSQL_datum *target,
					 const char *str)
{
	text	   *value;

	if (str != NULL)
		value = cstring_to_text(str);
	else
		value = cstring_to_text("");
	exec_assign_value(estate, target, PointerGetDatum(value), false,
					  TEXTOID, -1);
	pfree(value);
}


/* ----------
 * exec_assign_value			Put a value into a target datum
 *
 * Note: in some code paths, this will leak memory in the eval_econtext;
 * we assume that will be cleaned up later by exec_eval_cleanup.  We cannot
 * call exec_eval_cleanup here for fear of destroying the input Datum value.
 * ----------
 */
static void
exec_assign_value(PLpgSQL_execstate *estate,
				  PLpgSQL_datum *target,
				  Datum value, bool isNull,
				  Oid valtype, int32 valtypmod)
{
	switch (target->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
			{
				/*
				 * Target is a variable
				 */
				PLpgSQL_var *var = (PLpgSQL_var *) target;
				Datum		newvalue;

				newvalue = exec_cast_value(estate,
										   value,
										   &isNull,
										   valtype,
										   valtypmod,
										   var->datatype->typoid,
										   var->datatype->atttypmod);

				if (isNull && var->notnull)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("null value cannot be assigned to variable \"%s\" declared NOT NULL",
									var->refname)));

				/*
				 * If type is by-reference, copy the new value (which is
				 * probably in the eval_econtext) into the procedure's memory
				 * context.  But if it's a read/write reference to an expanded
				 * object, no physical copy needs to happen; at most we need
				 * to reparent the object's memory context.
				 *
				 * If it's an array, we force the value to be stored in R/W
				 * expanded form.  This wins if the function later does, say,
				 * a lot of array subscripting operations on the variable, and
				 * otherwise might lose.  We might need to use a different
				 * heuristic, but it's too soon to tell.  Also, are there
				 * cases where it'd be useful to force non-array values into
				 * expanded form?
				 */
				if (!var->datatype->typbyval && !isNull)
				{
					if (var->datatype->typisarray &&
						!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(newvalue)))
					{
						/* array and not already R/W, so apply expand_array */
						newvalue = expand_array(newvalue,
												CurrentMemoryContext,
												NULL);
					}
					else
					{
						/* else transfer value if R/W, else just datumCopy */
						newvalue = datumTransfer(newvalue,
												 false,
												 var->datatype->typlen);
					}
				}

				/*
				 * Now free the old value, if any, and assign the new one. But
				 * skip the assignment if old and new values are the same.
				 * Note that for expanded objects, this test is necessary and
				 * cannot reliably be made any earlier; we have to be looking
				 * at the object's standard R/W pointer to be sure pointer
				 * equality is meaningful.
				 */
				if (var->value != newvalue || var->isnull || isNull)
					assign_simple_var(estate, var, newvalue, isNull,
									  (!var->datatype->typbyval && !isNull));
				break;
			}

		case PLPGSQL_DTYPE_ROW:
			{
				/*
				 * Target is a row variable
				 */
				PLpgSQL_row *row = (PLpgSQL_row *) target;

				if (isNull)
				{
					/* If source is null, just assign nulls to the row */
					exec_move_row(estate, NULL, row, NULL, NULL);
				}
				else
				{
					/* Source must be of RECORD or composite type */
					if (!type_is_rowtype(valtype))
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("cannot assign non-composite value to a row variable")));
					exec_move_row_from_datum(estate, NULL, row, value);
				}
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				/*
				 * Target is a record variable
				 */
				PLpgSQL_rec *rec = (PLpgSQL_rec *) target;

				if (isNull)
				{
					/* If source is null, just assign nulls to the record */
					exec_move_row(estate, rec, NULL, NULL, NULL);
				}
				else
				{
					/* Source must be of RECORD or composite type */
					if (!type_is_rowtype(valtype))
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("cannot assign non-composite value to a record variable")));
					exec_move_row_from_datum(estate, rec, NULL, value);
				}
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				/*
				 * Target is a field of a record
				 */
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) target;
				PLpgSQL_rec *rec;
				int			fno;
				HeapTuple	newtup;
				int			natts;
				Datum	   *values;
				bool	   *nulls;
				bool	   *replaces;
				Oid			atttype;
				int32		atttypmod;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);

				/*
				 * Check that there is already a tuple in the record. We need
				 * that because records don't have any predefined field
				 * structure.
				 */
				if (!HeapTupleIsValid(rec->tup))
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						   errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));

				/*
				 * Get the number of the records field to change and the
				 * number of attributes in the tuple.  Note: disallow system
				 * column names because the code below won't cope.
				 */
				fno = SPI_fnumber(rec->tupdesc, recfield->fieldname);
				if (fno <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("record \"%s\" has no field \"%s\"",
									rec->refname, recfield->fieldname)));
				fno--;
				natts = rec->tupdesc->natts;

				/*
				 * Set up values/control arrays for heap_modify_tuple. For all
				 * the attributes except the one we want to replace, use the
				 * value that's in the old tuple.
				 */
				values = palloc(sizeof(Datum) * natts);
				nulls = palloc(sizeof(bool) * natts);
				replaces = palloc(sizeof(bool) * natts);

				memset(replaces, false, sizeof(bool) * natts);
				replaces[fno] = true;

				/*
				 * Now insert the new value, being careful to cast it to the
				 * right type.
				 */
				atttype = rec->tupdesc->attrs[fno]->atttypid;
				atttypmod = rec->tupdesc->attrs[fno]->atttypmod;
				values[fno] = exec_cast_value(estate,
											  value,
											  &isNull,
											  valtype,
											  valtypmod,
											  atttype,
											  atttypmod);
				nulls[fno] = isNull;

				/*
				 * Now call heap_modify_tuple() to create a new tuple that
				 * replaces the old one in the record.
				 */
				newtup = heap_modify_tuple(rec->tup, rec->tupdesc,
										   values, nulls, replaces);

				if (rec->freetup)
					heap_freetuple(rec->tup);

				rec->tup = newtup;
				rec->freetup = true;

				pfree(values);
				pfree(nulls);
				pfree(replaces);

				break;
			}

		case PLPGSQL_DTYPE_ARRAYELEM:
			{
				/*
				 * Target is an element of an array
				 */
				PLpgSQL_arrayelem *arrayelem;
				int			nsubscripts;
				int			i;
				PLpgSQL_expr *subscripts[MAXDIM];
				int			subscriptvals[MAXDIM];
				Datum		oldarraydatum,
							newarraydatum,
							coerced_value;
				bool		oldarrayisnull;
				Oid			parenttypoid;
				int32		parenttypmod;
				SPITupleTable *save_eval_tuptable;
				MemoryContext oldcontext;

				/*
				 * We need to do subscript evaluation, which might require
				 * evaluating general expressions; and the caller might have
				 * done that too in order to prepare the input Datum.  We have
				 * to save and restore the caller's SPI_execute result, if
				 * any.
				 */
				save_eval_tuptable = estate->eval_tuptable;
				estate->eval_tuptable = NULL;

				/*
				 * To handle constructs like x[1][2] := something, we have to
				 * be prepared to deal with a chain of arrayelem datums. Chase
				 * back to find the base array datum, and save the subscript
				 * expressions as we go.  (We are scanning right to left here,
				 * but want to evaluate the subscripts left-to-right to
				 * minimize surprises.)  Note that arrayelem is left pointing
				 * to the leftmost arrayelem datum, where we will cache the
				 * array element type data.
				 */
				nsubscripts = 0;
				do
				{
					arrayelem = (PLpgSQL_arrayelem *) target;
					if (nsubscripts >= MAXDIM)
						ereport(ERROR,
								(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
										nsubscripts + 1, MAXDIM)));
					subscripts[nsubscripts++] = arrayelem->subscript;
					target = estate->datums[arrayelem->arrayparentno];
				} while (target->dtype == PLPGSQL_DTYPE_ARRAYELEM);

				/* Fetch current value of array datum */
				exec_eval_datum(estate, target,
								&parenttypoid, &parenttypmod,
								&oldarraydatum, &oldarrayisnull);

				/* Update cached type data if necessary */
				if (arrayelem->parenttypoid != parenttypoid ||
					arrayelem->parenttypmod != parenttypmod)
				{
					Oid			arraytypoid;
					int32		arraytypmod = parenttypmod;
					int16		arraytyplen;
					Oid			elemtypoid;
					int16		elemtyplen;
					bool		elemtypbyval;
					char		elemtypalign;

					/* If target is domain over array, reduce to base type */
					arraytypoid = getBaseTypeAndTypmod(parenttypoid,
													   &arraytypmod);

					/* ... and identify the element type */
					elemtypoid = get_element_type(arraytypoid);
					if (!OidIsValid(elemtypoid))
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
							  errmsg("subscripted object is not an array")));

					/* Collect needed data about the types */
					arraytyplen = get_typlen(arraytypoid);

					get_typlenbyvalalign(elemtypoid,
										 &elemtyplen,
										 &elemtypbyval,
										 &elemtypalign);

					/* Now safe to update the cached data */
					arrayelem->parenttypoid = parenttypoid;
					arrayelem->parenttypmod = parenttypmod;
					arrayelem->arraytypoid = arraytypoid;
					arrayelem->arraytypmod = arraytypmod;
					arrayelem->arraytyplen = arraytyplen;
					arrayelem->elemtypoid = elemtypoid;
					arrayelem->elemtyplen = elemtyplen;
					arrayelem->elemtypbyval = elemtypbyval;
					arrayelem->elemtypalign = elemtypalign;
				}

				/*
				 * Evaluate the subscripts, switch into left-to-right order.
				 * Like ExecEvalArrayRef(), complain if any subscript is null.
				 */
				for (i = 0; i < nsubscripts; i++)
				{
					bool		subisnull;

					subscriptvals[i] =
						exec_eval_integer(estate,
										  subscripts[nsubscripts - 1 - i],
										  &subisnull);
					if (subisnull)
						ereport(ERROR,
								(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
								 errmsg("array subscript in assignment must not be null")));

					/*
					 * Clean up in case the subscript expression wasn't
					 * simple. We can't do exec_eval_cleanup, but we can do
					 * this much (which is safe because the integer subscript
					 * value is surely pass-by-value), and we must do it in
					 * case the next subscript expression isn't simple either.
					 */
					if (estate->eval_tuptable != NULL)
						SPI_freetuptable(estate->eval_tuptable);
					estate->eval_tuptable = NULL;
				}

				/* Now we can restore caller's SPI_execute result if any. */
				Assert(estate->eval_tuptable == NULL);
				estate->eval_tuptable = save_eval_tuptable;

				/* Coerce source value to match array element type. */
				coerced_value = exec_cast_value(estate,
												value,
												&isNull,
												valtype,
												valtypmod,
												arrayelem->elemtypoid,
												arrayelem->arraytypmod);

				/*
				 * If the original array is null, cons up an empty array so
				 * that the assignment can proceed; we'll end with a
				 * one-element array containing just the assigned-to
				 * subscript.  This only works for varlena arrays, though; for
				 * fixed-length array types we skip the assignment.  We can't
				 * support assignment of a null entry into a fixed-length
				 * array, either, so that's a no-op too.  This is all ugly but
				 * corresponds to the current behavior of ExecEvalArrayRef().
				 */
				if (arrayelem->arraytyplen > 0 &&		/* fixed-length array? */
					(oldarrayisnull || isNull))
					return;

				/* empty array, if any, and newarraydatum are short-lived */
				oldcontext = MemoryContextSwitchTo(estate->eval_econtext->ecxt_per_tuple_memory);

				if (oldarrayisnull)
					oldarraydatum = PointerGetDatum(construct_empty_array(arrayelem->elemtypoid));

				/*
				 * Build the modified array value.
				 */
				newarraydatum = array_set_element(oldarraydatum,
												  nsubscripts,
												  subscriptvals,
												  coerced_value,
												  isNull,
												  arrayelem->arraytyplen,
												  arrayelem->elemtyplen,
												  arrayelem->elemtypbyval,
												  arrayelem->elemtypalign);

				MemoryContextSwitchTo(oldcontext);

				/*
				 * Assign the new array to the base variable.  It's never NULL
				 * at this point.  Note that if the target is a domain,
				 * coercing the base array type back up to the domain will
				 * happen within exec_assign_value.
				 */
				exec_assign_value(estate, target,
								  newarraydatum,
								  false,
								  arrayelem->arraytypoid,
								  arrayelem->arraytypmod);
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", target->dtype);
	}
}

/*
 * exec_eval_datum				Get current value of a PLpgSQL_datum
 *
 * The type oid, typmod, value in Datum format, and null flag are returned.
 *
 * At present this doesn't handle PLpgSQL_expr or PLpgSQL_arrayelem datums;
 * that's not needed because we never pass references to such datums to SPI.
 *
 * NOTE: the returned Datum points right at the stored value in the case of
 * pass-by-reference datatypes.  Generally callers should take care not to
 * modify the stored value.  Some callers intentionally manipulate variables
 * referenced by R/W expanded pointers, though; it is those callers'
 * responsibility that the results are semantically OK.
 *
 * In some cases we have to palloc a return value, and in such cases we put
 * it into the estate's short-term memory context.
 */
static void
exec_eval_datum(PLpgSQL_execstate *estate,
				PLpgSQL_datum *datum,
				Oid *typeid,
				int32 *typetypmod,
				Datum *value,
				bool *isnull)
{
	MemoryContext oldcontext;

	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;

				*typeid = var->datatype->typoid;
				*typetypmod = var->datatype->atttypmod;
				*value = var->value;
				*isnull = var->isnull;
				break;
			}

		case PLPGSQL_DTYPE_ROW:
			{
				PLpgSQL_row *row = (PLpgSQL_row *) datum;
				HeapTuple	tup;

				if (!row->rowtupdesc)	/* should not happen */
					elog(ERROR, "row variable has no tupdesc");
				/* Make sure we have a valid type/typmod setting */
				BlessTupleDesc(row->rowtupdesc);
				oldcontext = MemoryContextSwitchTo(estate->eval_econtext->ecxt_per_tuple_memory);
				tup = make_tuple_from_row(estate, row, row->rowtupdesc);
				if (tup == NULL)	/* should not happen */
					elog(ERROR, "row not compatible with its own tupdesc");
				*typeid = row->rowtupdesc->tdtypeid;
				*typetypmod = row->rowtupdesc->tdtypmod;
				*value = HeapTupleGetDatum(tup);
				*isnull = false;
				MemoryContextSwitchTo(oldcontext);
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

				if (!HeapTupleIsValid(rec->tup))
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						   errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));
				Assert(rec->tupdesc != NULL);
				/* Make sure we have a valid type/typmod setting */
				BlessTupleDesc(rec->tupdesc);

				oldcontext = MemoryContextSwitchTo(estate->eval_econtext->ecxt_per_tuple_memory);
				*typeid = rec->tupdesc->tdtypeid;
				*typetypmod = rec->tupdesc->tdtypmod;
				*value = heap_copy_tuple_as_datum(rec->tup, rec->tupdesc);
				*isnull = false;
				MemoryContextSwitchTo(oldcontext);
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
				PLpgSQL_rec *rec;
				int			fno;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);
				if (!HeapTupleIsValid(rec->tup))
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						   errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));
				fno = SPI_fnumber(rec->tupdesc, recfield->fieldname);
				if (fno == SPI_ERROR_NOATTRIBUTE)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("record \"%s\" has no field \"%s\"",
									rec->refname, recfield->fieldname)));
				*typeid = SPI_gettypeid(rec->tupdesc, fno);
				if (fno > 0)
					*typetypmod = rec->tupdesc->attrs[fno - 1]->atttypmod;
				else
					*typetypmod = -1;
				*value = SPI_getbinval(rec->tup, rec->tupdesc, fno, isnull);
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
	}
}

/*
 * plpgsql_exec_get_datum_type				Get datatype of a PLpgSQL_datum
 *
 * This is the same logic as in exec_eval_datum, except that it can handle
 * some cases where exec_eval_datum has to fail; specifically, we may have
 * a tupdesc but no row value for a record variable.  (This currently can
 * happen only for a trigger's NEW/OLD records.)
 */
Oid
plpgsql_exec_get_datum_type(PLpgSQL_execstate *estate,
					PLpgSQL_datum *datum)
{
	Oid			typeid;

	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;

				typeid = var->datatype->typoid;
				break;
			}

		case PLPGSQL_DTYPE_ROW:
			{
				PLpgSQL_row *row = (PLpgSQL_row *) datum;

				if (!row->rowtupdesc)	/* should not happen */
					elog(ERROR, "row variable has no tupdesc");
				/* Make sure we have a valid type/typmod setting */
				BlessTupleDesc(row->rowtupdesc);
				typeid = row->rowtupdesc->tdtypeid;
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

				if (rec->tupdesc == NULL)
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						   errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));
				/* Make sure we have a valid type/typmod setting */
				BlessTupleDesc(rec->tupdesc);
				typeid = rec->tupdesc->tdtypeid;
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
				PLpgSQL_rec *rec;
				int			fno;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);
				if (rec->tupdesc == NULL)
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						   errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));
				fno = SPI_fnumber(rec->tupdesc, recfield->fieldname);
				if (fno == SPI_ERROR_NOATTRIBUTE)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("record \"%s\" has no field \"%s\"",
									rec->refname, recfield->fieldname)));
				typeid = SPI_gettypeid(rec->tupdesc, fno);
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
			typeid = InvalidOid;	/* keep compiler quiet */
			break;
	}

	return typeid;
}

/*
 * plpgsql_exec_get_datum_type_info			Get datatype etc of a PLpgSQL_datum
 *
 * An extended version of plpgsql_exec_get_datum_type, which also retrieves the
 * typmod and collation of the datum.
 */
void
plpgsql_exec_get_datum_type_info(PLpgSQL_execstate *estate,
						 PLpgSQL_datum *datum,
						 Oid *typeid, int32 *typmod, Oid *collation)
{
	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;

				*typeid = var->datatype->typoid;
				*typmod = var->datatype->atttypmod;
				*collation = var->datatype->collation;
				break;
			}

		case PLPGSQL_DTYPE_ROW:
			{
				PLpgSQL_row *row = (PLpgSQL_row *) datum;

				if (!row->rowtupdesc)	/* should not happen */
					elog(ERROR, "row variable has no tupdesc");
				/* Make sure we have a valid type/typmod setting */
				BlessTupleDesc(row->rowtupdesc);
				*typeid = row->rowtupdesc->tdtypeid;
				/* do NOT return the mutable typmod of a RECORD variable */
				*typmod = -1;
				/* composite types are never collatable */
				*collation = InvalidOid;
				break;
			}

		case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_rec *rec = (PLpgSQL_rec *) datum;

				if (rec->tupdesc == NULL)
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						   errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));
				/* Make sure we have a valid type/typmod setting */
				BlessTupleDesc(rec->tupdesc);
				*typeid = rec->tupdesc->tdtypeid;
				/* do NOT return the mutable typmod of a RECORD variable */
				*typmod = -1;
				/* composite types are never collatable */
				*collation = InvalidOid;
				break;
			}

		case PLPGSQL_DTYPE_RECFIELD:
			{
				PLpgSQL_recfield *recfield = (PLpgSQL_recfield *) datum;
				PLpgSQL_rec *rec;
				int			fno;

				rec = (PLpgSQL_rec *) (estate->datums[recfield->recparentno]);
				if (rec->tupdesc == NULL)
					ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("record \"%s\" is not assigned yet",
								  rec->refname),
						   errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));
				fno = SPI_fnumber(rec->tupdesc, recfield->fieldname);
				if (fno == SPI_ERROR_NOATTRIBUTE)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("record \"%s\" has no field \"%s\"",
									rec->refname, recfield->fieldname)));
				*typeid = SPI_gettypeid(rec->tupdesc, fno);
				if (fno > 0)
					*typmod = rec->tupdesc->attrs[fno - 1]->atttypmod;
				else
					*typmod = -1;
				if (fno > 0)
					*collation = rec->tupdesc->attrs[fno - 1]->attcollation;
				else	/* no system column types have collation */
					*collation = InvalidOid;
				break;
			}

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
			*typeid = InvalidOid;		/* keep compiler quiet */
			*typmod = -1;
			*collation = InvalidOid;
			break;
	}
}

/* ----------
 * exec_eval_integer		Evaluate an expression, coerce result to int4
 *
 * Note we do not do exec_eval_cleanup here; the caller must do it at
 * some later point.  (We do this because the caller may be holding the
 * results of other, pass-by-reference, expression evaluations, such as
 * an array value to be subscripted.)
 * ----------
 */
static int
exec_eval_integer(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr,
				  bool *isNull)
{
	Datum		exprdatum;
	Oid			exprtypeid;
	int32		exprtypmod;

	exprdatum = exec_eval_expr(estate, expr, isNull, &exprtypeid, &exprtypmod);
	exprdatum = exec_cast_value(estate, exprdatum, isNull,
								exprtypeid, exprtypmod,
								INT4OID, -1);
	return DatumGetInt32(exprdatum);
}

/* ----------
 * exec_eval_boolean		Evaluate an expression, coerce result to bool
 *
 * Note we do not do exec_eval_cleanup here; the caller must do it at
 * some later point.
 * ----------
 */
static bool
exec_eval_boolean(PLpgSQL_execstate *estate,
				  PLpgSQL_expr *expr,
				  bool *isNull)
{
	Datum		exprdatum;
	Oid			exprtypeid;
	int32		exprtypmod;

	exprdatum = exec_eval_expr(estate, expr, isNull, &exprtypeid, &exprtypmod);
	exprdatum = exec_cast_value(estate, exprdatum, isNull,
								exprtypeid, exprtypmod,
								BOOLOID, -1);
	return DatumGetBool(exprdatum);
}

/* ----------
 * exec_eval_expr			Evaluate an expression and return
 *					the result Datum, along with data type/typmod.
 *
 * NOTE: caller must do exec_eval_cleanup when done with the Datum.
 * ----------
 */
static Datum
exec_eval_expr(PLpgSQL_execstate *estate,
			   PLpgSQL_expr *expr,
			   bool *isNull,
			   Oid *rettype,
			   int32 *rettypmod)
{
	Datum		result = 0;
	int			rc;

	/*
	 * If first time through, create a plan for this expression.
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr, 0);

	/*
	 * If this is a simple expression, bypass SPI and use the executor
	 * directly
	 */
	if (exec_eval_simple_expr(estate, expr,
							  &result, isNull, rettype, rettypmod))
		return result;

	/*
	 * Else do it the hard way via exec_run_select
	 */
	rc = exec_run_select(estate, expr, 2, NULL, false);
	if (rc != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("query \"%s\" did not return data", expr->query)));

	/*
	 * Check that the expression returns exactly one column...
	 */
	if (estate->eval_tuptable->tupdesc->natts != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_plural("query \"%s\" returned %d column",
							   "query \"%s\" returned %d columns",
							   estate->eval_tuptable->tupdesc->natts,
							   expr->query,
							   estate->eval_tuptable->tupdesc->natts)));

	/*
	 * ... and get the column's datatype.
	 */
	*rettype = estate->eval_tuptable->tupdesc->attrs[0]->atttypid;
	*rettypmod = estate->eval_tuptable->tupdesc->attrs[0]->atttypmod;

	/*
	 * If there are no rows selected, the result is a NULL of that type.
	 */
	if (estate->eval_processed == 0)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * Check that the expression returned no more than one row.
	 */
	if (estate->eval_processed != 1)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("query \"%s\" returned more than one row",
						expr->query)));

	/*
	 * Return the single result Datum.
	 */
	return SPI_getbinval(estate->eval_tuptable->vals[0],
						 estate->eval_tuptable->tupdesc, 1, isNull);
}


/* ----------
 * exec_run_select			Execute a select query
 * ----------
 */
static int
exec_run_select(PLpgSQL_execstate *estate,
				PLpgSQL_expr *expr, long maxtuples, Portal *portalP,
				bool parallelOK)
{
	ParamListInfo paramLI;
	int			rc;

	/*
	 * On the first call for this expression generate the plan
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr, parallelOK ?
			CURSOR_OPT_PARALLEL_OK : 0);

	/*
	 * If a portal was requested, put the query into the portal
	 */
	if (portalP != NULL)
	{
		/*
		 * Set up short-lived ParamListInfo
		 */
		paramLI = setup_unshared_param_list(estate, expr);

		*portalP = SPI_cursor_open_with_paramlist(NULL, expr->plan,
												  paramLI,
												  estate->readonly_func);
		if (*portalP == NULL)
			elog(ERROR, "could not open implicit cursor for query \"%s\": %s",
				 expr->query, SPI_result_code_string(SPI_result));
		if (paramLI)
			pfree(paramLI);
		return SPI_OK_CURSOR;
	}

	/*
	 * Set up ParamListInfo to pass to executor
	 */
	paramLI = setup_param_list(estate, expr);

	/*
	 * Execute the query
	 */
	rc = SPI_execute_plan_with_paramlist(expr->plan, paramLI,
										 estate->readonly_func, maxtuples);
	if (rc != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("query \"%s\" is not a SELECT", expr->query)));

	/* Save query results for eventual cleanup */
	Assert(estate->eval_tuptable == NULL);
	estate->eval_tuptable = SPI_tuptable;
	estate->eval_processed = SPI_processed;
	estate->eval_lastoid = SPI_lastoid;

	return rc;
}


/*
 * exec_for_query --- execute body of FOR loop for each row from a portal
 *
 * Used by exec_stmt_fors, exec_stmt_forc and exec_stmt_dynfors
 */
static int
exec_for_query(PLpgSQL_execstate *estate, PLpgSQL_stmt_forq *stmt,
			   Portal portal, bool prefetch_ok)
{
	PLpgSQL_rec *rec = NULL;
	PLpgSQL_row *row = NULL;
	SPITupleTable *tuptab;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;
	uint64		n;

	/*
	 * Determine if we assign to a record or a row
	 */
	if (stmt->rec != NULL)
		rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->dno]);
	else if (stmt->row != NULL)
		row = (PLpgSQL_row *) (estate->datums[stmt->row->dno]);
	else
		elog(ERROR, "unsupported target");

	/*
	 * Make sure the portal doesn't get closed by the user statements we
	 * execute.
	 */
	PinPortal(portal);

	/*
	 * Fetch the initial tuple(s).  If prefetching is allowed then we grab a
	 * few more rows to avoid multiple trips through executor startup
	 * overhead.
	 */
	SPI_cursor_fetch(portal, true, prefetch_ok ? 10 : 1);
	tuptab = SPI_tuptable;
	n = SPI_processed;

	/*
	 * If the query didn't return any rows, set the target to NULL and fall
	 * through with found = false.
	 */
	if (n == 0)
	{
		exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
		exec_eval_cleanup(estate);
	}
	else
		found = true;			/* processed at least one tuple */

	/*
	 * Now do the loop
	 */
	while (n > 0)
	{
		uint64		i;

		for (i = 0; i < n; i++)
		{
			/*
			 * Assign the tuple to the target
			 */
			exec_move_row(estate, rec, row, tuptab->vals[i], tuptab->tupdesc);
			exec_eval_cleanup(estate);

			/*
			 * Execute the statements
			 */
			rc = exec_stmts(estate, stmt->body);

			if (rc != PLPGSQL_RC_OK)
			{
				if (rc == PLPGSQL_RC_EXIT)
				{
					if (estate->exitlabel == NULL)
					{
						/* unlabelled exit, so exit the current loop */
						rc = PLPGSQL_RC_OK;
					}
					else if (stmt->label != NULL &&
							 strcmp(stmt->label, estate->exitlabel) == 0)
					{
						/* label matches this loop, so exit loop */
						estate->exitlabel = NULL;
						rc = PLPGSQL_RC_OK;
					}

					/*
					 * otherwise, we processed a labelled exit that does not
					 * match the current statement's label, if any; return
					 * RC_EXIT so that the EXIT continues to recurse upward.
					 */
				}
				else if (rc == PLPGSQL_RC_CONTINUE)
				{
					if (estate->exitlabel == NULL)
					{
						/* unlabelled continue, so re-run the current loop */
						rc = PLPGSQL_RC_OK;
						continue;
					}
					else if (stmt->label != NULL &&
							 strcmp(stmt->label, estate->exitlabel) == 0)
					{
						/* label matches this loop, so re-run loop */
						estate->exitlabel = NULL;
						rc = PLPGSQL_RC_OK;
						continue;
					}

					/*
					 * otherwise, we process a labelled continue that does not
					 * match the current statement's label, if any; return
					 * RC_CONTINUE so that the CONTINUE will propagate up the
					 * stack.
					 */
				}

				/*
				 * We're aborting the loop.  Need a goto to get out of two
				 * levels of loop...
				 */
				goto loop_exit;
			}
		}

		SPI_freetuptable(tuptab);

		/*
		 * Fetch more tuples.  If prefetching is allowed, grab 50 at a time.
		 */
		SPI_cursor_fetch(portal, true, prefetch_ok ? 50 : 1);
		tuptab = SPI_tuptable;
		n = SPI_processed;
	}

loop_exit:

	/*
	 * Release last group of tuples (if any)
	 */
	SPI_freetuptable(tuptab);

	UnpinPortal(portal);

	/*
	 * Set the FOUND variable to indicate the result of executing the loop
	 * (namely, whether we looped one or more times). This must be set last so
	 * that it does not interfere with the value of the FOUND variable inside
	 * the loop processing itself.
	 */
	exec_set_found(estate, found);

	return rc;
}


/* ----------
 * exec_eval_simple_expr -		Evaluate a simple expression returning
 *								a Datum by directly calling ExecEvalExpr().
 *
 * If successful, store results into *result, *isNull, *rettype, *rettypmod
 * and return TRUE.  If the expression cannot be handled by simple evaluation,
 * return FALSE.
 *
 * Because we only store one execution tree for a simple expression, we
 * can't handle recursion cases.  So, if we see the tree is already busy
 * with an evaluation in the current xact, we just return FALSE and let the
 * caller run the expression the hard way.  (Other alternatives such as
 * creating a new tree for a recursive call either introduce memory leaks,
 * or add enough bookkeeping to be doubtful wins anyway.)  Another case that
 * is covered by the expr_simple_in_use test is where a previous execution
 * of the tree was aborted by an error: the tree may contain bogus state
 * so we dare not re-use it.
 *
 * It is possible though unlikely for a simple expression to become non-simple
 * (consider for example redefining a trivial view).  We must handle that for
 * correctness; fortunately it's normally inexpensive to call
 * SPI_plan_get_cached_plan for a simple expression.  We do not consider the
 * other direction (non-simple expression becoming simple) because we'll still
 * give correct results if that happens, and it's unlikely to be worth the
 * cycles to check.
 *
 * Note: if pass-by-reference, the result is in the eval_econtext's
 * temporary memory context.  It will be freed when exec_eval_cleanup
 * is done.
 * ----------
 */
static bool
exec_eval_simple_expr(PLpgSQL_execstate *estate,
					  PLpgSQL_expr *expr,
					  Datum *result,
					  bool *isNull,
					  Oid *rettype,
					  int32 *rettypmod)
{
	ExprContext *econtext = estate->eval_econtext;
	LocalTransactionId curlxid = MyProc->lxid;
	CachedPlan *cplan;
	ParamListInfo paramLI;
	void	   *save_setup_arg;
	MemoryContext oldcontext;

	/*
	 * Forget it if expression wasn't simple before.
	 */
	if (expr->expr_simple_expr == NULL)
		return false;

	/*
	 * If expression is in use in current xact, don't touch it.
	 */
	if (expr->expr_simple_in_use && expr->expr_simple_lxid == curlxid)
		return false;

	/*
	 * Revalidate cached plan, so that we will notice if it became stale. (We
	 * need to hold a refcount while using the plan, anyway.)
	 */
	cplan = SPI_plan_get_cached_plan(expr->plan);

	/*
	 * We can't get a failure here, because the number of CachedPlanSources in
	 * the SPI plan can't change from what exec_simple_check_plan saw; it's a
	 * property of the raw parsetree generated from the query text.
	 */
	Assert(cplan != NULL);

	if (cplan->generation != expr->expr_simple_generation)
	{
		/* It got replanned ... is it still simple? */
		exec_simple_recheck_plan(expr, cplan);
		/* better recheck r/w safety, as well */
		if (expr->rwparam >= 0)
			exec_check_rw_parameter(expr, expr->rwparam);
		if (expr->expr_simple_expr == NULL)
		{
			/* Ooops, release refcount and fail */
			ReleaseCachedPlan(cplan, true);
			return false;
		}
	}

	/*
	 * Pass back previously-determined result type.
	 */
	*rettype = expr->expr_simple_type;
	*rettypmod = expr->expr_simple_typmod;

	/*
	 * Prepare the expression for execution, if it's not been done already in
	 * the current transaction.  (This will be forced to happen if we called
	 * exec_simple_recheck_plan above.)
	 */
	if (expr->expr_simple_lxid != curlxid)
	{
		oldcontext = MemoryContextSwitchTo(estate->simple_eval_estate->es_query_cxt);
		expr->expr_simple_state = ExecInitExpr(expr->expr_simple_expr, NULL);
		expr->expr_simple_in_use = false;
		expr->expr_simple_lxid = curlxid;
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * We have to do some of the things SPI_execute_plan would do, in
	 * particular advance the snapshot if we are in a non-read-only function.
	 * Without this, stable functions within the expression would fail to see
	 * updates made so far by our own function.
	 */
	SPI_push();

	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	if (!estate->readonly_func)
	{
		CommandCounterIncrement();
		PushActiveSnapshot(GetTransactionSnapshot());
	}

	/*
	 * Set up ParamListInfo to pass to executor.  We need an unshared list if
	 * it's going to include any R/W expanded-object pointer.  For safety,
	 * save and restore estate->paramLI->parserSetupArg around our use of the
	 * param list.
	 */
	save_setup_arg = estate->paramLI->parserSetupArg;

	if (expr->rwparam >= 0)
		paramLI = setup_unshared_param_list(estate, expr);
	else
		paramLI = setup_param_list(estate, expr);

	econtext->ecxt_param_list_info = paramLI;

	/*
	 * Mark expression as busy for the duration of the ExecEvalExpr call.
	 */
	expr->expr_simple_in_use = true;

	/*
	 * Finally we can call the executor to evaluate the expression
	 */
	*result = ExecEvalExpr(expr->expr_simple_state,
						   econtext,
						   isNull,
						   NULL);

	/* Assorted cleanup */
	expr->expr_simple_in_use = false;

	if (paramLI && paramLI != estate->paramLI)
		pfree(paramLI);

	estate->paramLI->parserSetupArg = save_setup_arg;

	if (!estate->readonly_func)
		PopActiveSnapshot();

	MemoryContextSwitchTo(oldcontext);

	SPI_pop();

	/*
	 * Now we can release our refcount on the cached plan.
	 */
	ReleaseCachedPlan(cplan, true);

	/*
	 * That's it.
	 */
	return true;
}


/*
 * Create a ParamListInfo to pass to SPI
 *
 * We share a single ParamListInfo array across all SPI calls made from this
 * estate, except calls creating cursors, which use setup_unshared_param_list
 * (see its comments for reasons why), and calls that pass a R/W expanded
 * object pointer.  A shared array is generally OK since any given slot in
 * the array would need to contain the same current datum value no matter
 * which query or expression we're evaluating; but of course that doesn't
 * hold when a specific variable is being passed as a R/W pointer, because
 * other expressions in the same function probably don't want to do that.
 *
 * Note that paramLI->parserSetupArg points to the specific PLpgSQL_expr
 * being evaluated.  This is not an issue for statement-level callers, but
 * lower-level callers must save and restore estate->paramLI->parserSetupArg
 * just in case there's an active evaluation at an outer call level.
 *
 * The general plan for passing parameters to SPI is that plain VAR datums
 * always have valid images in the shared param list.  This is ensured by
 * assign_simple_var(), which also marks those params as PARAM_FLAG_CONST,
 * allowing the planner to use those values in custom plans.  However, non-VAR
 * datums cannot conveniently be managed that way.  For one thing, they could
 * throw errors (for example "no such record field") and we do not want that
 * to happen in a part of the expression that might never be evaluated at
 * runtime.  For another thing, exec_eval_datum() may return short-lived
 * values stored in the estate's short-term memory context, which will not
 * necessarily survive to the next SPI operation.  And for a third thing, ROW
 * and RECFIELD datums' values depend on other datums, and we don't have a
 * cheap way to track that.  Therefore, param slots for non-VAR datum types
 * are always reset here and then filled on-demand by plpgsql_param_fetch().
 * We can save a few cycles by not bothering with the reset loop unless at
 * least one such param has actually been filled by plpgsql_param_fetch().
 */
static ParamListInfo
setup_param_list(PLpgSQL_execstate *estate, PLpgSQL_expr *expr)
{
	ParamListInfo paramLI;

	/*
	 * We must have created the SPIPlan already (hence, query text has been
	 * parsed/analyzed at least once); else we cannot rely on expr->paramnos.
	 */
	Assert(expr->plan != NULL);

	/*
	 * Expressions with R/W parameters can't use the shared param list.
	 */
	Assert(expr->rwparam == -1);

	/*
	 * We only need a ParamListInfo if the expression has parameters.  In
	 * principle we should test with bms_is_empty(), but we use a not-null
	 * test because it's faster.  In current usage bits are never removed from
	 * expr->paramnos, only added, so this test is correct anyway.
	 */
	if (expr->paramnos)
	{
		/* Use the common ParamListInfo */
		paramLI = estate->paramLI;

		/*
		 * If any resettable parameters have been passed to the executor since
		 * last time, we need to reset those param slots to "invalid", for the
		 * reasons mentioned in the comment above.
		 */
		if (estate->params_dirty)
		{
			Bitmapset  *resettable_datums = estate->func->resettable_datums;
			int			dno = -1;

			while ((dno = bms_next_member(resettable_datums, dno)) >= 0)
			{
				ParamExternData *prm = &paramLI->params[dno];

				prm->ptype = InvalidOid;
			}
			estate->params_dirty = false;
		}

		/*
		 * Set up link to active expr where the hook functions can find it.
		 * Callers must save and restore parserSetupArg if there is any chance
		 * that they are interrupting an active use of parameters.
		 */
		paramLI->parserSetupArg = (void *) expr;

		/*
		 * Allow parameters that aren't needed by this expression to be
		 * ignored.
		 */
		paramLI->paramMask = expr->paramnos;

		/*
		 * Also make sure this is set before parser hooks need it.  There is
		 * no need to save and restore, since the value is always correct once
		 * set.  (Should be set already, but let's be sure.)
		 */
		expr->func = estate->func;
	}
	else
	{
		/*
		 * Expression requires no parameters.  Be sure we represent this case
		 * as a NULL ParamListInfo, so that plancache.c knows there is no
		 * point in a custom plan.
		 */
		paramLI = NULL;
	}
	return paramLI;
}

/*
 * Create an unshared, short-lived ParamListInfo to pass to SPI
 *
 * When creating a cursor, we do not use the shared ParamListInfo array
 * but create a short-lived one that will contain only params actually
 * referenced by the query.  The reason for this is that copyParamList() will
 * be used to copy the parameters into cursor-lifespan storage, and we don't
 * want it to copy anything that's not used by the specific cursor; that
 * could result in uselessly copying some large values.
 *
 * We also use this for expressions that are passing a R/W object pointer
 * to some trusted function.  We don't want the R/W pointer to get into the
 * shared param list, where it could get passed to some less-trusted function.
 *
 * Caller should pfree the result after use, if it's not NULL.
 *
 * XXX. Could we use ParamListInfo's new paramMask to avoid creating unshared
 * parameter lists?
 */
static ParamListInfo
setup_unshared_param_list(PLpgSQL_execstate *estate, PLpgSQL_expr *expr)
{
	ParamListInfo paramLI;

	/*
	 * We must have created the SPIPlan already (hence, query text has been
	 * parsed/analyzed at least once); else we cannot rely on expr->paramnos.
	 */
	Assert(expr->plan != NULL);

	/*
	 * We only need a ParamListInfo if the expression has parameters.  In
	 * principle we should test with bms_is_empty(), but we use a not-null
	 * test because it's faster.  In current usage bits are never removed from
	 * expr->paramnos, only added, so this test is correct anyway.
	 */
	if (expr->paramnos)
	{
		int			dno;

		/* initialize ParamListInfo with one entry per datum, all invalid */
		paramLI = (ParamListInfo)
			palloc0(offsetof(ParamListInfoData, params) +
					estate->ndatums * sizeof(ParamExternData));
		paramLI->paramFetch = plpgsql_param_fetch;
		paramLI->paramFetchArg = (void *) estate;
		paramLI->parserSetup = (ParserSetupHook) plpgsql_parser_setup;
		paramLI->parserSetupArg = (void *) expr;
		paramLI->numParams = estate->ndatums;
		paramLI->paramMask = NULL;

		/*
		 * Instantiate values for "safe" parameters of the expression.  We
		 * could skip this and leave them to be filled by plpgsql_param_fetch;
		 * but then the values would not be available for query planning,
		 * since the planner doesn't call the paramFetch hook.
		 */
		dno = -1;
		while ((dno = bms_next_member(expr->paramnos, dno)) >= 0)
		{
			PLpgSQL_datum *datum = estate->datums[dno];

			if (datum->dtype == PLPGSQL_DTYPE_VAR)
			{
				PLpgSQL_var *var = (PLpgSQL_var *) datum;
				ParamExternData *prm = &paramLI->params[dno];

				if (dno == expr->rwparam)
					prm->value = var->value;
				else
					prm->value = MakeExpandedObjectReadOnly(var->value,
															var->isnull,
													  var->datatype->typlen);
				prm->isnull = var->isnull;
				prm->pflags = PARAM_FLAG_CONST;
				prm->ptype = var->datatype->typoid;
			}
		}

		/*
		 * Also make sure this is set before parser hooks need it.  There is
		 * no need to save and restore, since the value is always correct once
		 * set.  (Should be set already, but let's be sure.)
		 */
		expr->func = estate->func;
	}
	else
	{
		/*
		 * Expression requires no parameters.  Be sure we represent this case
		 * as a NULL ParamListInfo, so that plancache.c knows there is no
		 * point in a custom plan.
		 */
		paramLI = NULL;
	}
	return paramLI;
}

/*
 * plpgsql_param_fetch		paramFetch callback for dynamic parameter fetch
 */
static void
plpgsql_param_fetch(ParamListInfo params, int paramid)
{
	int			dno;
	PLpgSQL_execstate *estate;
	PLpgSQL_expr *expr;
	PLpgSQL_datum *datum;
	ParamExternData *prm;
	int32		prmtypmod;

	/* paramid's are 1-based, but dnos are 0-based */
	dno = paramid - 1;
	Assert(dno >= 0 && dno < params->numParams);

	/* fetch back the hook data */
	estate = (PLpgSQL_execstate *) params->paramFetchArg;
	expr = (PLpgSQL_expr *) params->parserSetupArg;
	Assert(params->numParams == estate->ndatums);

	/* now we can access the target datum */
	datum = estate->datums[dno];

	/*
	 * Since copyParamList() or SerializeParamList() will try to materialize
	 * every single parameter slot, it's important to do nothing when asked
	 * for a datum that's not supposed to be used by this SQL expression.
	 * Otherwise we risk failures in exec_eval_datum(), or copying a lot more
	 * data than necessary.
	 */
	if (!bms_is_member(dno, expr->paramnos))
		return;

	if (params == estate->paramLI)
	{
		/*
		 * We need to mark the shared params array dirty if we're about to
		 * evaluate a resettable datum.
		 */
		switch (datum->dtype)
		{
			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_REC:
			case PLPGSQL_DTYPE_RECFIELD:
				estate->params_dirty = true;
				break;

			default:
				break;
		}
	}

	/* OK, evaluate the value and store into the appropriate paramlist slot */
	prm = &params->params[dno];
	exec_eval_datum(estate, datum,
					&prm->ptype, &prmtypmod,
					&prm->value, &prm->isnull);
	/* We can always mark params as "const" for executor's purposes */
	prm->pflags = PARAM_FLAG_CONST;

	/*
	 * If it's a read/write expanded datum, convert reference to read-only,
	 * unless it's safe to pass as read-write.
	 */
	if (datum->dtype == PLPGSQL_DTYPE_VAR && dno != expr->rwparam)
		prm->value = MakeExpandedObjectReadOnly(prm->value,
												prm->isnull,
								  ((PLpgSQL_var *) datum)->datatype->typlen);
}


/* ----------
 * exec_move_row			Move one tuple's values into a record or row
 *
 * Since this uses exec_assign_value, caller should eventually call
 * exec_eval_cleanup to prevent long-term memory leaks.
 * ----------
 */
static void
exec_move_row(PLpgSQL_execstate *estate,
			  PLpgSQL_rec *rec,
			  PLpgSQL_row *row,
			  HeapTuple tup, TupleDesc tupdesc)
{
	/*
	 * Record is simple - just copy the tuple and its descriptor into the
	 * record variable
	 */
	if (rec != NULL)
	{
		/*
		 * Copy input first, just in case it is pointing at variable's value
		 */
		if (HeapTupleIsValid(tup))
			tup = heap_copytuple(tup);
		else if (tupdesc)
		{
			/* If we have a tupdesc but no data, form an all-nulls tuple */
			bool	   *nulls;

			nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));
			memset(nulls, true, tupdesc->natts * sizeof(bool));

			tup = heap_form_tuple(tupdesc, NULL, nulls);

			pfree(nulls);
		}

		if (tupdesc)
			tupdesc = CreateTupleDescCopy(tupdesc);

		/* Free the old value ... */
		if (rec->freetup)
		{
			heap_freetuple(rec->tup);
			rec->freetup = false;
		}
		if (rec->freetupdesc)
		{
			FreeTupleDesc(rec->tupdesc);
			rec->freetupdesc = false;
		}

		/* ... and install the new */
		if (HeapTupleIsValid(tup))
		{
			rec->tup = tup;
			rec->freetup = true;
		}
		else
			rec->tup = NULL;

		if (tupdesc)
		{
			rec->tupdesc = tupdesc;
			rec->freetupdesc = true;
		}
		else
			rec->tupdesc = NULL;

		return;
	}

	/*
	 * Row is a bit more complicated in that we assign the individual
	 * attributes of the tuple to the variables the row points to.
	 *
	 * NOTE: this code used to demand row->nfields ==
	 * HeapTupleHeaderGetNatts(tup->t_data), but that's wrong.  The tuple
	 * might have more fields than we expected if it's from an
	 * inheritance-child table of the current table, or it might have fewer if
	 * the table has had columns added by ALTER TABLE. Ignore extra columns
	 * and assume NULL for missing columns, the same as heap_getattr would do.
	 * We also have to skip over dropped columns in either the source or
	 * destination.
	 *
	 * If we have no tuple data at all, we'll assign NULL to all columns of
	 * the row variable.
	 */
	if (row != NULL)
	{
		int			td_natts = tupdesc ? tupdesc->natts : 0;
		int			t_natts;
		int			fnum;
		int			anum;

		if (HeapTupleIsValid(tup))
			t_natts = HeapTupleHeaderGetNatts(tup->t_data);
		else
			t_natts = 0;

		anum = 0;
		for (fnum = 0; fnum < row->nfields; fnum++)
		{
			PLpgSQL_var *var;
			Datum		value;
			bool		isnull;
			Oid			valtype;
			int32		valtypmod;

			if (row->varnos[fnum] < 0)
				continue;		/* skip dropped column in row struct */

			var = (PLpgSQL_var *) (estate->datums[row->varnos[fnum]]);

			while (anum < td_natts && tupdesc->attrs[anum]->attisdropped)
				anum++;			/* skip dropped column in tuple */

			if (anum < td_natts)
			{
				if (anum < t_natts)
					value = SPI_getbinval(tup, tupdesc, anum + 1, &isnull);
				else
				{
					value = (Datum) 0;
					isnull = true;
				}
				valtype = tupdesc->attrs[anum]->atttypid;
				valtypmod = tupdesc->attrs[anum]->atttypmod;
				anum++;
			}
			else
			{
				value = (Datum) 0;
				isnull = true;
				valtype = UNKNOWNOID;
				valtypmod = -1;
			}

			exec_assign_value(estate, (PLpgSQL_datum *) var,
							  value, isnull, valtype, valtypmod);
		}

		return;
	}

	elog(ERROR, "unsupported target");
}

/* ----------
 * make_tuple_from_row		Make a tuple from the values of a row object
 *
 * A NULL return indicates rowtype mismatch; caller must raise suitable error
 * ----------
 */
static HeapTuple
make_tuple_from_row(PLpgSQL_execstate *estate,
					PLpgSQL_row *row,
					TupleDesc tupdesc)
{
	int			natts = tupdesc->natts;
	HeapTuple	tuple;
	Datum	   *dvalues;
	bool	   *nulls;
	int			i;

	if (natts != row->nfields)
		return NULL;

	dvalues = (Datum *) palloc0(natts * sizeof(Datum));
	nulls = (bool *) palloc(natts * sizeof(bool));

	for (i = 0; i < natts; i++)
	{
		Oid			fieldtypeid;
		int32		fieldtypmod;

		if (tupdesc->attrs[i]->attisdropped)
		{
			nulls[i] = true;	/* leave the column as null */
			continue;
		}
		if (row->varnos[i] < 0) /* should not happen */
			elog(ERROR, "dropped rowtype entry for non-dropped column");

		exec_eval_datum(estate, estate->datums[row->varnos[i]],
						&fieldtypeid, &fieldtypmod,
						&dvalues[i], &nulls[i]);
		if (fieldtypeid != tupdesc->attrs[i]->atttypid)
			return NULL;
		/* XXX should we insist on typmod match, too? */
	}

	tuple = heap_form_tuple(tupdesc, dvalues, nulls);

	pfree(dvalues);
	pfree(nulls);

	return tuple;
}

/* ----------
 * get_tuple_from_datum		extract a tuple from a composite Datum
 *
 * Returns a freshly palloc'd HeapTuple.
 *
 * Note: it's caller's responsibility to be sure value is of composite type.
 * ----------
 */
static HeapTuple
get_tuple_from_datum(Datum value)
{
	HeapTupleHeader td = DatumGetHeapTupleHeader(value);
	HeapTupleData tmptup;

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = td;

	/* Build a copy and return it */
	return heap_copytuple(&tmptup);
}

/* ----------
 * get_tupdesc_from_datum	get a tuple descriptor for a composite Datum
 *
 * Returns a pointer to the TupleDesc of the tuple's rowtype.
 * Caller is responsible for calling ReleaseTupleDesc when done with it.
 *
 * Note: it's caller's responsibility to be sure value is of composite type.
 * ----------
 */
static TupleDesc
get_tupdesc_from_datum(Datum value)
{
	HeapTupleHeader td = DatumGetHeapTupleHeader(value);
	Oid			tupType;
	int32		tupTypmod;

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	return lookup_rowtype_tupdesc(tupType, tupTypmod);
}

/* ----------
 * exec_move_row_from_datum		Move a composite Datum into a record or row
 *
 * This is equivalent to get_tuple_from_datum() followed by exec_move_row(),
 * but we avoid constructing an intermediate physical copy of the tuple.
 * ----------
 */
static void
exec_move_row_from_datum(PLpgSQL_execstate *estate,
						 PLpgSQL_rec *rec,
						 PLpgSQL_row *row,
						 Datum value)
{
	HeapTupleHeader td = DatumGetHeapTupleHeader(value);
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup;

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = td;

	/* Do the move */
	exec_move_row(estate, rec, row, &tmptup, tupdesc);

	/* Release tupdesc usage count */
	ReleaseTupleDesc(tupdesc);
}

/* ----------
 * convert_value_to_string			Convert a non-null Datum to C string
 *
 * Note: the result is in the estate's eval_econtext, and will be cleared
 * by the next exec_eval_cleanup() call.  The invoked output function might
 * leave additional cruft there as well, so just pfree'ing the result string
 * would not be enough to avoid memory leaks if we did not do it like this.
 * In most usages the Datum being passed in is also in that context (if
 * pass-by-reference) and so an exec_eval_cleanup() call is needed anyway.
 *
 * Note: not caching the conversion function lookup is bad for performance.
 * However, this function isn't currently used in any places where an extra
 * catalog lookup or two seems like a big deal.
 * ----------
 */
static char *
convert_value_to_string(PLpgSQL_execstate *estate, Datum value, Oid valtype)
{
	char	   *result;
	MemoryContext oldcontext;
	Oid			typoutput;
	bool		typIsVarlena;

	oldcontext = MemoryContextSwitchTo(estate->eval_econtext->ecxt_per_tuple_memory);
	getTypeOutputInfo(valtype, &typoutput, &typIsVarlena);
	result = OidOutputFunctionCall(typoutput, value);
	MemoryContextSwitchTo(oldcontext);

	return result;
}

/* ----------
 * exec_cast_value			Cast a value if required
 *
 * Note that *isnull is an input and also an output parameter.  While it's
 * unlikely that a cast operation would produce null from non-null or vice
 * versa, that could happen in principle.
 *
 * Note: the estate's eval_econtext is used for temporary storage, and may
 * also contain the result Datum if we have to do a conversion to a pass-
 * by-reference data type.  Be sure to do an exec_eval_cleanup() call when
 * done with the result.
 * ----------
 */
static Datum
exec_cast_value(PLpgSQL_execstate *estate,
				Datum value, bool *isnull,
				Oid valtype, int32 valtypmod,
				Oid reqtype, int32 reqtypmod)
{
	/*
	 * If the type of the given value isn't what's requested, convert it.
	 */
	if (valtype != reqtype ||
		(valtypmod != reqtypmod && reqtypmod != -1))
	{
		plpgsql_CastHashEntry *cast_entry;

		cast_entry = get_cast_hashentry(estate,
										valtype, valtypmod,
										reqtype, reqtypmod);
		if (cast_entry)
		{
			ExprContext *econtext = estate->eval_econtext;
			MemoryContext oldcontext;

			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

			econtext->caseValue_datum = value;
			econtext->caseValue_isNull = *isnull;

			cast_entry->cast_in_use = true;

			value = ExecEvalExpr(cast_entry->cast_exprstate, econtext,
								 isnull, NULL);

			cast_entry->cast_in_use = false;

			MemoryContextSwitchTo(oldcontext);
		}
	}

	return value;
}

/* ----------
 * get_cast_hashentry			Look up how to perform a type cast
 *
 * Returns a plpgsql_CastHashEntry if an expression has to be evaluated,
 * or NULL if the cast is a mere no-op relabeling.  If there's work to be
 * done, the cast_exprstate field contains an expression evaluation tree
 * based on a CaseTestExpr input, and the cast_in_use field should be set
 * TRUE while executing it.
 * ----------
 */
static plpgsql_CastHashEntry *
get_cast_hashentry(PLpgSQL_execstate *estate,
				   Oid srctype, int32 srctypmod,
				   Oid dsttype, int32 dsttypmod)
{
	plpgsql_CastHashKey cast_key;
	plpgsql_CastHashEntry *cast_entry;
	bool		found;
	LocalTransactionId curlxid;
	MemoryContext oldcontext;

	/* Look for existing entry */
	cast_key.srctype = srctype;
	cast_key.dsttype = dsttype;
	cast_key.srctypmod = srctypmod;
	cast_key.dsttypmod = dsttypmod;
	cast_entry = (plpgsql_CastHashEntry *) hash_search(estate->cast_hash,
													   (void *) &cast_key,
													   HASH_FIND, NULL);

	if (cast_entry == NULL)
	{
		/* We've not looked up this coercion before */
		Node	   *cast_expr;
		CaseTestExpr *placeholder;

		/*
		 * Since we could easily fail (no such coercion), construct a
		 * temporary coercion expression tree in a short-lived context, then
		 * if successful copy it to cast_hash_context.
		 */
		oldcontext = MemoryContextSwitchTo(estate->eval_econtext->ecxt_per_tuple_memory);

		/*
		 * We use a CaseTestExpr as the base of the coercion tree, since it's
		 * very cheap to insert the source value for that.
		 */
		placeholder = makeNode(CaseTestExpr);
		placeholder->typeId = srctype;
		placeholder->typeMod = srctypmod;
		placeholder->collation = get_typcollation(srctype);

		/*
		 * Apply coercion.  We use ASSIGNMENT coercion because that's the
		 * closest match to plpgsql's historical behavior; in particular,
		 * EXPLICIT coercion would allow silent truncation to a destination
		 * varchar/bpchar's length, which we do not want.
		 *
		 * If source type is UNKNOWN, coerce_to_target_type will fail (it only
		 * expects to see that for Const input nodes), so don't call it; we'll
		 * apply CoerceViaIO instead.  Likewise, it doesn't currently work for
		 * coercing RECORD to some other type, so skip for that too.
		 */
		if (srctype == UNKNOWNOID || srctype == RECORDOID)
			cast_expr = NULL;
		else
			cast_expr = coerce_to_target_type(NULL,
											  (Node *) placeholder, srctype,
											  dsttype, dsttypmod,
											  COERCION_ASSIGNMENT,
											  COERCE_IMPLICIT_CAST,
											  -1);

		/*
		 * If there's no cast path according to the parser, fall back to using
		 * an I/O coercion; this is semantically dubious but matches plpgsql's
		 * historical behavior.  We would need something of the sort for
		 * UNKNOWN literals in any case.
		 */
		if (cast_expr == NULL)
		{
			CoerceViaIO *iocoerce = makeNode(CoerceViaIO);

			iocoerce->arg = (Expr *) placeholder;
			iocoerce->resulttype = dsttype;
			iocoerce->resultcollid = InvalidOid;
			iocoerce->coerceformat = COERCE_IMPLICIT_CAST;
			iocoerce->location = -1;
			cast_expr = (Node *) iocoerce;
			if (dsttypmod != -1)
				cast_expr = coerce_to_target_type(NULL,
												  cast_expr, dsttype,
												  dsttype, dsttypmod,
												  COERCION_ASSIGNMENT,
												  COERCE_IMPLICIT_CAST,
												  -1);
		}

		/* Note: we don't bother labeling the expression tree with collation */

		/* Detect whether we have a no-op (RelabelType) coercion */
		if (IsA(cast_expr, RelabelType) &&
			((RelabelType *) cast_expr)->arg == (Expr *) placeholder)
			cast_expr = NULL;

		if (cast_expr)
		{
			/* ExecInitExpr assumes we've planned the expression */
			cast_expr = (Node *) expression_planner((Expr *) cast_expr);

			/* Now copy the tree into cast_hash_context */
			MemoryContextSwitchTo(estate->cast_hash_context);

			cast_expr = copyObject(cast_expr);
		}

		MemoryContextSwitchTo(oldcontext);

		/* Now we can fill in a hashtable entry. */
		cast_entry = (plpgsql_CastHashEntry *) hash_search(estate->cast_hash,
														   (void *) &cast_key,
														 HASH_ENTER, &found);
		Assert(!found);			/* wasn't there a moment ago */
		cast_entry->cast_expr = (Expr *) cast_expr;
		cast_entry->cast_exprstate = NULL;
		cast_entry->cast_in_use = false;
		cast_entry->cast_lxid = InvalidLocalTransactionId;
	}

	/* Done if we have determined that this is a no-op cast. */
	if (cast_entry->cast_expr == NULL)
		return NULL;

	/*
	 * Prepare the expression for execution, if it's not been done already in
	 * the current transaction; also, if it's marked busy in the current
	 * transaction, abandon that expression tree and build a new one, so as to
	 * avoid potential problems with recursive cast expressions and failed
	 * executions.  (We will leak some memory intra-transaction if that
	 * happens a lot, but we don't expect it to.)  It's okay to update the
	 * hash table with the new tree because all plpgsql functions within a
	 * given transaction share the same simple_eval_estate.  (Well, regular
	 * functions do; DO blocks have private simple_eval_estates, and private
	 * cast hash tables to go with them.)
	 */
	curlxid = MyProc->lxid;
	if (cast_entry->cast_lxid != curlxid || cast_entry->cast_in_use)
	{
		oldcontext = MemoryContextSwitchTo(estate->simple_eval_estate->es_query_cxt);
		cast_entry->cast_exprstate = ExecInitExpr(cast_entry->cast_expr, NULL);
		cast_entry->cast_in_use = false;
		cast_entry->cast_lxid = curlxid;
		MemoryContextSwitchTo(oldcontext);
	}

	return cast_entry;
}

/* ----------
 * exec_simple_check_node -		Recursively check if an expression
 *								is made only of simple things we can
 *								hand out directly to ExecEvalExpr()
 *								instead of calling SPI.
 * ----------
 */
static bool
exec_simple_check_node(Node *node)
{
	if (node == NULL)
		return TRUE;

	switch (nodeTag(node))
	{
		case T_Const:
			return TRUE;

		case T_Param:
			return TRUE;

		case T_ArrayRef:
			{
				ArrayRef   *expr = (ArrayRef *) node;

				if (!exec_simple_check_node((Node *) expr->refupperindexpr))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->reflowerindexpr))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->refexpr))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->refassgnexpr))
					return FALSE;

				return TRUE;
			}

		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (expr->funcretset)
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_OpExpr:
			{
				OpExpr	   *expr = (OpExpr *) node;

				if (expr->opretset)
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;

				if (expr->opretset)
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_NullIfExpr:
			{
				NullIfExpr *expr = (NullIfExpr *) node;

				if (expr->opretset)
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_FieldSelect:
			return exec_simple_check_node((Node *) ((FieldSelect *) node)->arg);

		case T_FieldStore:
			{
				FieldStore *expr = (FieldStore *) node;

				if (!exec_simple_check_node((Node *) expr->arg))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->newvals))
					return FALSE;

				return TRUE;
			}

		case T_RelabelType:
			return exec_simple_check_node((Node *) ((RelabelType *) node)->arg);

		case T_CoerceViaIO:
			return exec_simple_check_node((Node *) ((CoerceViaIO *) node)->arg);

		case T_ArrayCoerceExpr:
			return exec_simple_check_node((Node *) ((ArrayCoerceExpr *) node)->arg);

		case T_ConvertRowtypeExpr:
			return exec_simple_check_node((Node *) ((ConvertRowtypeExpr *) node)->arg);

		case T_CaseExpr:
			{
				CaseExpr   *expr = (CaseExpr *) node;

				if (!exec_simple_check_node((Node *) expr->arg))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->defresult))
					return FALSE;

				return TRUE;
			}

		case T_CaseWhen:
			{
				CaseWhen   *when = (CaseWhen *) node;

				if (!exec_simple_check_node((Node *) when->expr))
					return FALSE;
				if (!exec_simple_check_node((Node *) when->result))
					return FALSE;

				return TRUE;
			}

		case T_CaseTestExpr:
			return TRUE;

		case T_ArrayExpr:
			{
				ArrayExpr  *expr = (ArrayExpr *) node;

				if (!exec_simple_check_node((Node *) expr->elements))
					return FALSE;

				return TRUE;
			}

		case T_RowExpr:
			{
				RowExpr    *expr = (RowExpr *) node;

				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_RowCompareExpr:
			{
				RowCompareExpr *expr = (RowCompareExpr *) node;

				if (!exec_simple_check_node((Node *) expr->largs))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->rargs))
					return FALSE;

				return TRUE;
			}

		case T_CoalesceExpr:
			{
				CoalesceExpr *expr = (CoalesceExpr *) node;

				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_MinMaxExpr:
			{
				MinMaxExpr *expr = (MinMaxExpr *) node;

				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_XmlExpr:
			{
				XmlExpr    *expr = (XmlExpr *) node;

				if (!exec_simple_check_node((Node *) expr->named_args))
					return FALSE;
				if (!exec_simple_check_node((Node *) expr->args))
					return FALSE;

				return TRUE;
			}

		case T_NullTest:
			return exec_simple_check_node((Node *) ((NullTest *) node)->arg);

		case T_BooleanTest:
			return exec_simple_check_node((Node *) ((BooleanTest *) node)->arg);

		case T_CoerceToDomain:
			return exec_simple_check_node((Node *) ((CoerceToDomain *) node)->arg);

		case T_CoerceToDomainValue:
			return TRUE;

		case T_List:
			{
				List	   *expr = (List *) node;
				ListCell   *l;

				foreach(l, expr)
				{
					if (!exec_simple_check_node(lfirst(l)))
						return FALSE;
				}

				return TRUE;
			}

		default:
			return FALSE;
	}
}


/* ----------
 * exec_simple_check_plan -		Check if a plan is simple enough to
 *								be evaluated by ExecEvalExpr() instead
 *								of SPI.
 * ----------
 */
static void
exec_simple_check_plan(PLpgSQL_expr *expr)
{
	List	   *plansources;
	CachedPlanSource *plansource;
	Query	   *query;
	CachedPlan *cplan;

	/*
	 * Initialize to "not simple", and remember the plan generation number we
	 * last checked.  (If we don't get as far as obtaining a plan to check, we
	 * just leave expr_simple_generation set to 0.)
	 */
	expr->expr_simple_expr = NULL;
	expr->expr_simple_generation = 0;

	/*
	 * We can only test queries that resulted in exactly one CachedPlanSource
	 */
	plansources = SPI_plan_get_plan_sources(expr->plan);
	if (list_length(plansources) != 1)
		return;
	plansource = (CachedPlanSource *) linitial(plansources);

	/*
	 * Do some checking on the analyzed-and-rewritten form of the query. These
	 * checks are basically redundant with the tests in
	 * exec_simple_recheck_plan, but the point is to avoid building a plan if
	 * possible.  Since this function is only called immediately after
	 * creating the CachedPlanSource, we need not worry about the query being
	 * stale.
	 */

	/*
	 * 1. There must be one single querytree.
	 */
	if (list_length(plansource->query_list) != 1)
		return;
	query = (Query *) linitial(plansource->query_list);

	/*
	 * 2. It must be a plain SELECT query without any input tables
	 */
	if (!IsA(query, Query))
		return;
	if (query->commandType != CMD_SELECT)
		return;
	if (query->rtable != NIL)
		return;

	/*
	 * 3. Can't have any subplans, aggregates, qual clauses either
	 */
	if (query->hasAggs ||
		query->hasWindowFuncs ||
		query->hasSubLinks ||
		query->hasForUpdate ||
		query->cteList ||
		query->jointree->quals ||
		query->groupClause ||
		query->havingQual ||
		query->windowClause ||
		query->distinctClause ||
		query->sortClause ||
		query->limitOffset ||
		query->limitCount ||
		query->setOperations)
		return;

	/*
	 * 4. The query must have a single attribute as result
	 */
	if (list_length(query->targetList) != 1)
		return;

	/*
	 * OK, it seems worth constructing a plan for more careful checking.
	 */

	/* Get the generic plan for the query */
	cplan = SPI_plan_get_cached_plan(expr->plan);

	/* Can't fail, because we checked for a single CachedPlanSource above */
	Assert(cplan != NULL);

	/* Share the remaining work with recheck code path */
	exec_simple_recheck_plan(expr, cplan);

	/* Release our plan refcount */
	ReleaseCachedPlan(cplan, true);
}

/*
 * exec_simple_recheck_plan --- check for simple plan once we have CachedPlan
 */
static void
exec_simple_recheck_plan(PLpgSQL_expr *expr, CachedPlan *cplan)
{
	PlannedStmt *stmt;
	Plan	   *plan;
	TargetEntry *tle;

	/*
	 * Initialize to "not simple", and remember the plan generation number we
	 * last checked.
	 */
	expr->expr_simple_expr = NULL;
	expr->expr_simple_generation = cplan->generation;

	/*
	 * 1. There must be one single plantree
	 */
	if (list_length(cplan->stmt_list) != 1)
		return;
	stmt = (PlannedStmt *) linitial(cplan->stmt_list);

	/*
	 * 2. It must be a RESULT plan --> no scan's required
	 */
	if (!IsA(stmt, PlannedStmt))
		return;
	if (stmt->commandType != CMD_SELECT)
		return;
	plan = stmt->planTree;
	if (!IsA(plan, Result))
		return;

	/*
	 * 3. Can't have any subplan or qual clause, either
	 */
	if (plan->lefttree != NULL ||
		plan->righttree != NULL ||
		plan->initPlan != NULL ||
		plan->qual != NULL ||
		((Result *) plan)->resconstantqual != NULL)
		return;

	/*
	 * 4. The plan must have a single attribute as result
	 */
	if (list_length(plan->targetlist) != 1)
		return;

	tle = (TargetEntry *) linitial(plan->targetlist);

	/*
	 * 5. Check that all the nodes in the expression are non-scary.
	 */
	if (!exec_simple_check_node((Node *) tle->expr))
		return;

	/*
	 * Yes - this is a simple expression.  Mark it as such, and initialize
	 * state to "not valid in current transaction".
	 */
	expr->expr_simple_expr = tle->expr;
	expr->expr_simple_state = NULL;
	expr->expr_simple_in_use = false;
	expr->expr_simple_lxid = InvalidLocalTransactionId;
	/* Also stash away the expression result type */
	expr->expr_simple_type = exprType((Node *) tle->expr);
	expr->expr_simple_typmod = exprTypmod((Node *) tle->expr);
}

/*
 * exec_check_rw_parameter --- can we pass expanded object as read/write param?
 *
 * If we have an assignment like "x := array_append(x, foo)" in which the
 * top-level function is trusted not to corrupt its argument in case of an
 * error, then when x has an expanded object as value, it is safe to pass the
 * value as a read/write pointer and let the function modify the value
 * in-place.
 *
 * This function checks for a safe expression, and sets expr->rwparam to the
 * dno of the target variable (x) if safe, or -1 if not safe.
 */
static void
exec_check_rw_parameter(PLpgSQL_expr *expr, int target_dno)
{
	Oid			funcid;
	List	   *fargs;
	ListCell   *lc;

	/* Assume unsafe */
	expr->rwparam = -1;

	/*
	 * If the expression isn't simple, there's no point in trying to optimize
	 * (because the exec_run_select code path will flatten any expanded result
	 * anyway).  Even without that, this seems like a good safety restriction.
	 */
	if (expr->expr_simple_expr == NULL)
		return;

	/*
	 * If target variable isn't referenced by expression, no need to look
	 * further.
	 */
	if (!bms_is_member(target_dno, expr->paramnos))
		return;

	/*
	 * Top level of expression must be a simple FuncExpr or OpExpr.
	 */
	if (IsA(expr->expr_simple_expr, FuncExpr))
	{
		FuncExpr   *fexpr = (FuncExpr *) expr->expr_simple_expr;

		funcid = fexpr->funcid;
		fargs = fexpr->args;
	}
	else if (IsA(expr->expr_simple_expr, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) expr->expr_simple_expr;

		funcid = opexpr->opfuncid;
		fargs = opexpr->args;
	}
	else
		return;

	/*
	 * The top-level function must be one that we trust to be "safe".
	 * Currently we hard-wire the list, but it would be very desirable to
	 * allow extensions to mark their functions as safe ...
	 */
	if (!(funcid == F_ARRAY_APPEND ||
		  funcid == F_ARRAY_PREPEND))
		return;

	/*
	 * The target variable (in the form of a Param) must only appear as a
	 * direct argument of the top-level function.
	 */
	foreach(lc, fargs)
	{
		Node	   *arg = (Node *) lfirst(lc);

		/* A Param is OK, whether it's the target variable or not */
		if (arg && IsA(arg, Param))
			continue;
		/* Otherwise, argument expression must not reference target */
		if (contains_target_param(arg, &target_dno))
			return;
	}

	/* OK, we can pass target as a read-write parameter */
	expr->rwparam = target_dno;
}

/*
 * Recursively check for a Param referencing the target variable
 */
static bool
contains_target_param(Node *node, int *target_dno)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN &&
			param->paramid == *target_dno + 1)
			return true;
		return false;
	}
	return expression_tree_walker(node, contains_target_param,
								  (void *) target_dno);
}

/* ----------
 * exec_set_found			Set the global found variable to true/false
 * ----------
 */
static void
exec_set_found(PLpgSQL_execstate *estate, bool state)
{
	PLpgSQL_var *var;

	var = (PLpgSQL_var *) (estate->datums[estate->found_varno]);
	assign_simple_var(estate, var, BoolGetDatum(state), false, false);
}

/*
 * plpgsql_create_econtext --- create an eval_econtext for the current function
 *
 * We may need to create a new shared_simple_eval_estate too, if there's not
 * one already for the current transaction.  The EState will be cleaned up at
 * transaction end.
 */
static void
plpgsql_create_econtext(PLpgSQL_execstate *estate)
{
	SimpleEcontextStackEntry *entry;

	/*
	 * Create an EState for evaluation of simple expressions, if there's not
	 * one already in the current transaction.  The EState is made a child of
	 * TopTransactionContext so it will have the right lifespan.
	 *
	 * Note that this path is never taken when executing a DO block; the
	 * required EState was already made by plpgsql_inline_handler.
	 */
	if (estate->simple_eval_estate == NULL)
	{
		MemoryContext oldcontext;

		Assert(shared_simple_eval_estate == NULL);
		oldcontext = MemoryContextSwitchTo(TopTransactionContext);
		shared_simple_eval_estate = CreateExecutorState();
		estate->simple_eval_estate = shared_simple_eval_estate;
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Create a child econtext for the current function.
	 */
	estate->eval_econtext = CreateExprContext(estate->simple_eval_estate);

	/*
	 * Make a stack entry so we can clean up the econtext at subxact end.
	 * Stack entries are kept in TopTransactionContext for simplicity.
	 */
	entry = (SimpleEcontextStackEntry *)
		MemoryContextAlloc(TopTransactionContext,
						   sizeof(SimpleEcontextStackEntry));

	entry->stack_econtext = estate->eval_econtext;
	entry->xact_subxid = GetCurrentSubTransactionId();

	entry->next = simple_econtext_stack;
	simple_econtext_stack = entry;
}

/*
 * plpgsql_destroy_econtext --- destroy function's econtext
 *
 * We check that it matches the top stack entry, and destroy the stack
 * entry along with the context.
 */
static void
plpgsql_destroy_econtext(PLpgSQL_execstate *estate)
{
	SimpleEcontextStackEntry *next;

	Assert(simple_econtext_stack != NULL);
	Assert(simple_econtext_stack->stack_econtext == estate->eval_econtext);

	next = simple_econtext_stack->next;
	pfree(simple_econtext_stack);
	simple_econtext_stack = next;

	FreeExprContext(estate->eval_econtext, true);
	estate->eval_econtext = NULL;
}

/*
 * plpgsql_xact_cb --- post-transaction-commit-or-abort cleanup
 *
 * If a simple-expression EState was created in the current transaction,
 * it has to be cleaned up.
 */
void
plpgsql_xact_cb(XactEvent event, void *arg)
{
	/*
	 * If we are doing a clean transaction shutdown, free the EState (so that
	 * any remaining resources will be released correctly). In an abort, we
	 * expect the regular abort recovery procedures to release everything of
	 * interest.
	 */
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE)
	{
		/* Shouldn't be any econtext stack entries left at commit */
		Assert(simple_econtext_stack == NULL);

		if (shared_simple_eval_estate)
			FreeExecutorState(shared_simple_eval_estate);
		shared_simple_eval_estate = NULL;
	}
	else if (event == XACT_EVENT_ABORT)
	{
		simple_econtext_stack = NULL;
		shared_simple_eval_estate = NULL;
	}
}

/*
 * plpgsql_subxact_cb --- post-subtransaction-commit-or-abort cleanup
 *
 * Make sure any simple-expression econtexts created in the current
 * subtransaction get cleaned up.  We have to do this explicitly because
 * no other code knows which econtexts belong to which level of subxact.
 */
void
plpgsql_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
				   SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_COMMIT_SUB || event == SUBXACT_EVENT_ABORT_SUB)
	{
		while (simple_econtext_stack != NULL &&
			   simple_econtext_stack->xact_subxid == mySubid)
		{
			SimpleEcontextStackEntry *next;

			FreeExprContext(simple_econtext_stack->stack_econtext,
							(event == SUBXACT_EVENT_COMMIT_SUB));
			next = simple_econtext_stack->next;
			pfree(simple_econtext_stack);
			simple_econtext_stack = next;
		}
	}
}

/*
 * assign_simple_var --- assign a new value to any VAR datum.
 *
 * This should be the only mechanism for assignment to simple variables,
 * lest we forget to update the paramLI image.
 */
static void
assign_simple_var(PLpgSQL_execstate *estate, PLpgSQL_var *var,
				  Datum newvalue, bool isnull, bool freeable)
{
	ParamExternData *prm;

	Assert(var->dtype == PLPGSQL_DTYPE_VAR);
	/* Free the old value if needed */
	if (var->freeval)
	{
		if (DatumIsReadWriteExpandedObject(var->value,
										   var->isnull,
										   var->datatype->typlen))
			DeleteExpandedObject(var->value);
		else
			pfree(DatumGetPointer(var->value));
	}
	/* Assign new value to datum */
	var->value = newvalue;
	var->isnull = isnull;
	var->freeval = freeable;
	/* And update the image in the common parameter list */
	prm = &estate->paramLI->params[var->dno];
	prm->value = MakeExpandedObjectReadOnly(newvalue,
											isnull,
											var->datatype->typlen);
	prm->isnull = isnull;
	/* these might be set already, but let's be sure */
	prm->pflags = PARAM_FLAG_CONST;
	prm->ptype = var->datatype->typoid;
}

/*
 * free old value of a text variable and assign new value from C string
 */
static void
assign_text_var(PLpgSQL_execstate *estate, PLpgSQL_var *var, const char *str)
{
	assign_simple_var(estate, var, CStringGetTextDatum(str), false, true);
}

/*
 * exec_eval_using_params --- evaluate params of USING clause
 */
static PreparedParamsData *
exec_eval_using_params(PLpgSQL_execstate *estate, List *params)
{
	PreparedParamsData *ppd;
	int			nargs;
	int			i;
	ListCell   *lc;

	ppd = (PreparedParamsData *) palloc(sizeof(PreparedParamsData));
	nargs = list_length(params);

	ppd->nargs = nargs;
	ppd->types = (Oid *) palloc(nargs * sizeof(Oid));
	ppd->values = (Datum *) palloc(nargs * sizeof(Datum));
	ppd->nulls = (char *) palloc(nargs * sizeof(char));
	ppd->freevals = (bool *) palloc(nargs * sizeof(bool));

	i = 0;
	foreach(lc, params)
	{
		PLpgSQL_expr *param = (PLpgSQL_expr *) lfirst(lc);
		bool		isnull;
		int32		ppdtypmod;

		ppd->values[i] = exec_eval_expr(estate, param,
										&isnull,
										&ppd->types[i],
										&ppdtypmod);
		ppd->nulls[i] = isnull ? 'n' : ' ';
		ppd->freevals[i] = false;

		if (ppd->types[i] == UNKNOWNOID)
		{
			/*
			 * Treat 'unknown' parameters as text, since that's what most
			 * people would expect. SPI_execute_with_args can coerce unknown
			 * constants in a more intelligent way, but not unknown Params.
			 * This code also takes care of copying into the right context.
			 * Note we assume 'unknown' has the representation of C-string.
			 */
			ppd->types[i] = TEXTOID;
			if (!isnull)
			{
				ppd->values[i] = CStringGetTextDatum(DatumGetCString(ppd->values[i]));
				ppd->freevals[i] = true;
			}
		}
		/* pass-by-ref non null values must be copied into plpgsql context */
		else if (!isnull)
		{
			int16		typLen;
			bool		typByVal;

			get_typlenbyval(ppd->types[i], &typLen, &typByVal);
			if (!typByVal)
			{
				ppd->values[i] = datumCopy(ppd->values[i], typByVal, typLen);
				ppd->freevals[i] = true;
			}
		}

		exec_eval_cleanup(estate);

		i++;
	}

	return ppd;
}

/*
 * free_params_data --- pfree all pass-by-reference values used in USING clause
 */
static void
free_params_data(PreparedParamsData *ppd)
{
	int			i;

	for (i = 0; i < ppd->nargs; i++)
	{
		if (ppd->freevals[i])
			pfree(DatumGetPointer(ppd->values[i]));
	}

	pfree(ppd->types);
	pfree(ppd->values);
	pfree(ppd->nulls);
	pfree(ppd->freevals);

	pfree(ppd);
}

/*
 * Open portal for dynamic query
 */
static Portal
exec_dynquery_with_params(PLpgSQL_execstate *estate,
						  PLpgSQL_expr *dynquery,
						  List *params,
						  const char *portalname,
						  int cursorOptions)
{
	Portal		portal;
	Datum		query;
	bool		isnull;
	Oid			restype;
	int32		restypmod;
	char	   *querystr;

	/*
	 * Evaluate the string expression after the EXECUTE keyword. Its result is
	 * the querystring we have to execute.
	 */
	query = exec_eval_expr(estate, dynquery, &isnull, &restype, &restypmod);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("query string argument of EXECUTE is null")));

	/* Get the C-String representation */
	querystr = convert_value_to_string(estate, query, restype);

	/* copy it out of the temporary context before we clean up */
	querystr = pstrdup(querystr);

	exec_eval_cleanup(estate);

	/*
	 * Open an implicit cursor for the query.  We use
	 * SPI_cursor_open_with_args even when there are no params, because this
	 * avoids making and freeing one copy of the plan.
	 */
	if (params)
	{
		PreparedParamsData *ppd;

		ppd = exec_eval_using_params(estate, params);
		portal = SPI_cursor_open_with_args(portalname,
										   querystr,
										   ppd->nargs, ppd->types,
										   ppd->values, ppd->nulls,
										   estate->readonly_func,
										   cursorOptions);
		free_params_data(ppd);
	}
	else
	{
		portal = SPI_cursor_open_with_args(portalname,
										   querystr,
										   0, NULL,
										   NULL, NULL,
										   estate->readonly_func,
										   cursorOptions);
	}

	if (portal == NULL)
		elog(ERROR, "could not open implicit cursor for query \"%s\": %s",
			 querystr, SPI_result_code_string(SPI_result));
	pfree(querystr);

	return portal;
}

/*
 * Return a formatted string with information about an expression's parameters,
 * or NULL if the expression does not take any parameters.
 */
static char *
format_expr_params(PLpgSQL_execstate *estate,
				   const PLpgSQL_expr *expr)
{
	int			paramno;
	int			dno;
	StringInfoData paramstr;

	if (!expr->paramnos)
		return NULL;

	initStringInfo(&paramstr);
	paramno = 0;
	dno = -1;
	while ((dno = bms_next_member(expr->paramnos, dno)) >= 0)
	{
		Datum		paramdatum;
		Oid			paramtypeid;
		bool		paramisnull;
		int32		paramtypmod;
		PLpgSQL_var *curvar;

		curvar = (PLpgSQL_var *) estate->datums[dno];

		exec_eval_datum(estate, (PLpgSQL_datum *) curvar,
						&paramtypeid, &paramtypmod,
						&paramdatum, &paramisnull);

		appendStringInfo(&paramstr, "%s%s = ",
						 paramno > 0 ? ", " : "",
						 curvar->refname);

		if (paramisnull)
			appendStringInfoString(&paramstr, "NULL");
		else
		{
			char	   *value = convert_value_to_string(estate, paramdatum, paramtypeid);
			char	   *p;

			appendStringInfoCharMacro(&paramstr, '\'');
			for (p = value; *p; p++)
			{
				if (*p == '\'') /* double single quotes */
					appendStringInfoCharMacro(&paramstr, *p);
				appendStringInfoCharMacro(&paramstr, *p);
			}
			appendStringInfoCharMacro(&paramstr, '\'');
		}

		paramno++;
	}

	return paramstr.data;
}

/*
 * Return a formatted string with information about PreparedParamsData, or NULL
 * if there are no parameters.
 */
static char *
format_preparedparamsdata(PLpgSQL_execstate *estate,
						  const PreparedParamsData *ppd)
{
	int			paramno;
	StringInfoData paramstr;

	if (!ppd)
		return NULL;

	initStringInfo(&paramstr);
	for (paramno = 0; paramno < ppd->nargs; paramno++)
	{
		appendStringInfo(&paramstr, "%s$%d = ",
						 paramno > 0 ? ", " : "",
						 paramno + 1);

		if (ppd->nulls[paramno] == 'n')
			appendStringInfoString(&paramstr, "NULL");
		else
		{
			char	   *value = convert_value_to_string(estate, ppd->values[paramno], ppd->types[paramno]);
			char	   *p;

			appendStringInfoCharMacro(&paramstr, '\'');
			for (p = value; *p; p++)
			{
				if (*p == '\'') /* double single quotes */
					appendStringInfoCharMacro(&paramstr, *p);
				appendStringInfoCharMacro(&paramstr, *p);
			}
			appendStringInfoCharMacro(&paramstr, '\'');
		}
	}

	return paramstr.data;
}
