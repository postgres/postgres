/**********************************************************************
 * pl_exec.c		- Executor for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plpgsql/src/pl_exec.c,v 1.93.2.2 2005/06/20 20:44:57 tgl Exp $
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
#include "pl.tab.h"

#include <ctype.h>
#include <setjmp.h>

#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/spi_priv.h"
#include "funcapi.h"
#include "optimizer/clauses.h"
#include "parser/parse_expr.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


static const char *const raise_skip_msg = "RAISE";

/*
 * All plpgsql function executions within a single transaction share
 * the same executor EState for evaluating "simple" expressions.  Each
 * function call creates its own "eval_econtext" ExprContext within this
 * estate.  We destroy the estate at transaction shutdown to ensure there
 * is no permanent leakage of memory (especially for xact abort case).
 *
 * If a simple PLpgSQL_expr has been used in the current xact, it is
 * linked into the active_simple_exprs list.
 */
static EState *simple_eval_estate = NULL;
static PLpgSQL_expr *active_simple_exprs = NULL;

/************************************************************
 * Local function forward declarations
 ************************************************************/
static void plpgsql_exec_error_callback(void *arg);
static PLpgSQL_var *copy_var(PLpgSQL_var * var);
static PLpgSQL_rec *copy_rec(PLpgSQL_rec * rec);

static int exec_stmt_block(PLpgSQL_execstate * estate,
				PLpgSQL_stmt_block * block);
static int exec_stmts(PLpgSQL_execstate * estate,
		   PLpgSQL_stmts * stmts);
static int exec_stmt(PLpgSQL_execstate * estate,
		  PLpgSQL_stmt * stmt);
static int exec_stmt_assign(PLpgSQL_execstate * estate,
				 PLpgSQL_stmt_assign * stmt);
static int exec_stmt_perform(PLpgSQL_execstate * estate,
				  PLpgSQL_stmt_perform * stmt);
static int exec_stmt_getdiag(PLpgSQL_execstate * estate,
				  PLpgSQL_stmt_getdiag * stmt);
static int exec_stmt_if(PLpgSQL_execstate * estate,
			 PLpgSQL_stmt_if * stmt);
static int exec_stmt_loop(PLpgSQL_execstate * estate,
			   PLpgSQL_stmt_loop * stmt);
static int exec_stmt_while(PLpgSQL_execstate * estate,
				PLpgSQL_stmt_while * stmt);
static int exec_stmt_fori(PLpgSQL_execstate * estate,
			   PLpgSQL_stmt_fori * stmt);
static int exec_stmt_fors(PLpgSQL_execstate * estate,
			   PLpgSQL_stmt_fors * stmt);
static int exec_stmt_select(PLpgSQL_execstate * estate,
				 PLpgSQL_stmt_select * stmt);
static int exec_stmt_open(PLpgSQL_execstate * estate,
			   PLpgSQL_stmt_open * stmt);
static int exec_stmt_fetch(PLpgSQL_execstate * estate,
				PLpgSQL_stmt_fetch * stmt);
static int exec_stmt_close(PLpgSQL_execstate * estate,
				PLpgSQL_stmt_close * stmt);
static int exec_stmt_exit(PLpgSQL_execstate * estate,
			   PLpgSQL_stmt_exit * stmt);
static int exec_stmt_return(PLpgSQL_execstate * estate,
				 PLpgSQL_stmt_return * stmt);
static int exec_stmt_return_next(PLpgSQL_execstate * estate,
					  PLpgSQL_stmt_return_next * stmt);
static int exec_stmt_raise(PLpgSQL_execstate * estate,
				PLpgSQL_stmt_raise * stmt);
static int exec_stmt_execsql(PLpgSQL_execstate * estate,
				  PLpgSQL_stmt_execsql * stmt);
static int exec_stmt_dynexecute(PLpgSQL_execstate * estate,
					 PLpgSQL_stmt_dynexecute * stmt);
static int exec_stmt_dynfors(PLpgSQL_execstate * estate,
				  PLpgSQL_stmt_dynfors * stmt);

static void plpgsql_estate_setup(PLpgSQL_execstate * estate,
					 PLpgSQL_function * func,
					 ReturnSetInfo *rsi);
static void exec_eval_cleanup(PLpgSQL_execstate * estate);

static void exec_prepare_plan(PLpgSQL_execstate * estate,
				  PLpgSQL_expr * expr);
static bool exec_simple_check_node(Node *node);
static void exec_simple_check_plan(PLpgSQL_expr * expr);
static Datum exec_eval_simple_expr(PLpgSQL_execstate * estate,
					  PLpgSQL_expr * expr,
					  bool *isNull,
					  Oid *rettype);

static void exec_assign_expr(PLpgSQL_execstate * estate,
				 PLpgSQL_datum * target,
				 PLpgSQL_expr * expr);
static void exec_assign_value(PLpgSQL_execstate * estate,
				  PLpgSQL_datum * target,
				  Datum value, Oid valtype, bool *isNull);
static void exec_eval_datum(PLpgSQL_execstate * estate,
				PLpgSQL_datum * datum,
				Oid expectedtypeid,
				Oid *typeid,
				Datum *value,
				bool *isnull);
static int exec_eval_integer(PLpgSQL_execstate * estate,
					PLpgSQL_expr * expr,
					bool *isNull);
static bool exec_eval_boolean(PLpgSQL_execstate * estate,
							  PLpgSQL_expr * expr,
							  bool *isNull);
static Datum exec_eval_expr(PLpgSQL_execstate * estate,
			   PLpgSQL_expr * expr,
			   bool *isNull,
			   Oid *rettype);
static int exec_run_select(PLpgSQL_execstate * estate,
				PLpgSQL_expr * expr, int maxtuples, Portal *portalP);
static void exec_move_row(PLpgSQL_execstate * estate,
			  PLpgSQL_rec * rec,
			  PLpgSQL_row * row,
			  HeapTuple tup, TupleDesc tupdesc);
static HeapTuple make_tuple_from_row(PLpgSQL_execstate * estate,
									 PLpgSQL_row * row,
									 TupleDesc tupdesc);
static char *convert_value_to_string(Datum value, Oid valtype);
static Datum exec_cast_value(Datum value, Oid valtype,
				Oid reqtype,
				FmgrInfo *reqinput,
				Oid reqtypelem,
				int32 reqtypmod,
				bool *isnull);
static Datum exec_simple_cast_value(Datum value, Oid valtype,
					   Oid reqtype, int32 reqtypmod,
					   bool *isnull);
static void exec_init_tuple_store(PLpgSQL_execstate * estate);
static bool compatible_tupdesc(TupleDesc td1, TupleDesc td2);
static void exec_set_found(PLpgSQL_execstate * estate, bool state);


/* ----------
 * plpgsql_exec_function	Called by the call handler for
 *				function execution.
 * ----------
 */
Datum
plpgsql_exec_function(PLpgSQL_function * func, FunctionCallInfo fcinfo)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			i;

	/*
	 * Setup the execution state
	 */
	plpgsql_estate_setup(&estate, func, (ReturnSetInfo *) fcinfo->resultinfo);

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
	for (i = 0; i < func->ndatums; i++)
	{
		switch (func->datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				estate.datums[i] = (PLpgSQL_datum *)
					copy_var((PLpgSQL_var *) (func->datums[i]));
				break;

			case PLPGSQL_DTYPE_REC:
				estate.datums[i] = (PLpgSQL_datum *)
					copy_rec((PLpgSQL_rec *) (func->datums[i]));
				break;

			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_RECFIELD:
			case PLPGSQL_DTYPE_ARRAYELEM:
				estate.datums[i] = func->datums[i];
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", func->datums[i]->dtype);
		}
	}

	/*
	 * Store the actual call argument values into the variables
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

					var->value = fcinfo->arg[i];
					var->isnull = fcinfo->argnull[i];
					var->freeval = false;
				}
				break;

			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) estate.datums[n];
					TupleTableSlot *slot = (TupleTableSlot *) fcinfo->arg[i];
					HeapTuple	tup;
					TupleDesc	tupdesc;

					if (!fcinfo->argnull[i])
					{
						Assert(slot != NULL);
						tup = slot->val;
						tupdesc = slot->ttc_tupleDescriptor;
						exec_move_row(&estate, NULL, row, tup, tupdesc);
					}
					else
					{
						/* If arg is null, treat it as an empty row */
						exec_move_row(&estate, NULL, row, NULL, NULL);
					}
				}
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", func->datums[i]->dtype);
		}
	}

	/*
	 * Initialize the other variables to NULL values for now. The default
	 * values are set when the blocks are entered.
	 */
	estate.err_text = gettext_noop("while initializing local variables to NULL");
	for (i = estate.found_varno; i < estate.ndatums; i++)
	{
		switch (estate.datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) estate.datums[i];

					var->value = 0;
					var->isnull = true;
					var->freeval = false;
				}
				break;

			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_REC:
			case PLPGSQL_DTYPE_RECFIELD:
			case PLPGSQL_DTYPE_ARRAYELEM:
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", func->datums[i]->dtype);
		}
	}

	/*
	 * Set the magic variable FOUND to false
	 */
	exec_set_found(&estate, false);

	/*
	 * Now call the toplevel block of statements
	 */
	estate.err_text = NULL;
	estate.err_stmt = (PLpgSQL_stmt *) (func->action);
	if (exec_stmt_block(&estate, func->action) != PLPGSQL_RC_RETURN)
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
			/* Copy tuple to upper executor memory */
			/* Here we need to return a TupleTableSlot not just a tuple */
			estate.retval = (Datum)
				SPI_copytupleintoslot((HeapTuple) (estate.retval),
									  estate.rettupdesc);
		}
		else
		{
			/* Cast value to proper type */
			estate.retval = exec_cast_value(estate.retval, estate.rettype,
											func->fn_rettype,
											&(func->fn_retinput),
											func->fn_rettypelem,
											-1,
											&fcinfo->isnull);

			/*
			 * If the functions return type isn't by value, copy the value
			 * into upper executor memory context.
			 */
			if (!fcinfo->isnull && !func->fn_retbyval)
			{
				Size		len;
				void	   *tmp;

				len = datumGetSize(estate.retval, false, func->fn_rettyplen);
				tmp = (void *) SPI_palloc(len);
				memcpy(tmp, DatumGetPointer(estate.retval), len);
				estate.retval = PointerGetDatum(tmp);
			}
		}
	}

	/* Clean up any leftover temporary memory */
	if (estate.eval_econtext != NULL)
		FreeExprContext(estate.eval_econtext);
	estate.eval_econtext = NULL;
	exec_eval_cleanup(&estate);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;

	/*
	 * Return the functions result
	 */
	return estate.retval;
}


/* ----------
 * plpgsql_exec_trigger		Called by the call handler for
 *				trigger execution.
 * ----------
 */
HeapTuple
plpgsql_exec_trigger(PLpgSQL_function * func,
					 TriggerData *trigdata)
{
	PLpgSQL_execstate estate;
	ErrorContextCallback plerrcontext;
	int			i;
	PLpgSQL_var *var;
	PLpgSQL_rec *rec_new,
			   *rec_old;
	HeapTuple	rettup;

	/*
	 * Setup the execution state
	 */
	plpgsql_estate_setup(&estate, func, NULL);

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
	for (i = 0; i < func->ndatums; i++)
	{
		switch (func->datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				estate.datums[i] = (PLpgSQL_datum *)
					copy_var((PLpgSQL_var *) (func->datums[i]));
				break;

			case PLPGSQL_DTYPE_REC:
				estate.datums[i] = (PLpgSQL_datum *)
					copy_rec((PLpgSQL_rec *) (func->datums[i]));
				break;

			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_RECFIELD:
			case PLPGSQL_DTYPE_ARRAYELEM:
			case PLPGSQL_DTYPE_TRIGARG:
				estate.datums[i] = func->datums[i];
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", func->datums[i]->dtype);
		}
	}

	/*
	 * Put the OLD and NEW tuples into record variables
	 */
	rec_new = (PLpgSQL_rec *) (estate.datums[func->new_varno]);
	rec_new->freetup = false;
	rec_new->freetupdesc = false;
	rec_old = (PLpgSQL_rec *) (estate.datums[func->old_varno]);
	rec_old->freetup = false;
	rec_old->freetupdesc = false;

	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
	{
		/*
		 * Per-statement triggers don't use OLD/NEW variables
		 */
		rec_new->tup = NULL;
		rec_new->tupdesc = NULL;
		rec_old->tup = NULL;
		rec_old->tupdesc = NULL;
	}
	else if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
	{
		rec_new->tup = trigdata->tg_trigtuple;
		rec_new->tupdesc = trigdata->tg_relation->rd_att;
		rec_old->tup = NULL;
		rec_old->tupdesc = NULL;
	}
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		rec_new->tup = trigdata->tg_newtuple;
		rec_new->tupdesc = trigdata->tg_relation->rd_att;
		rec_old->tup = trigdata->tg_trigtuple;
		rec_old->tupdesc = trigdata->tg_relation->rd_att;
	}
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
	{
		rec_new->tup = NULL;
		rec_new->tupdesc = NULL;
		rec_old->tup = trigdata->tg_trigtuple;
		rec_old->tupdesc = trigdata->tg_relation->rd_att;
	}
	else
		elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, or UPDATE");

	/*
	 * Assign the special tg_ variables
	 */

	var = (PLpgSQL_var *) (estate.datums[func->tg_op_varno]);
	var->isnull = false;
	var->freeval = false;

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		var->value = DirectFunctionCall1(textin, CStringGetDatum("INSERT"));
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		var->value = DirectFunctionCall1(textin, CStringGetDatum("UPDATE"));
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		var->value = DirectFunctionCall1(textin, CStringGetDatum("DELETE"));
	else
		elog(ERROR, "unrecognized trigger action: not INSERT, DELETE, or UPDATE");

	var = (PLpgSQL_var *) (estate.datums[func->tg_name_varno]);
	var->isnull = false;
	var->freeval = true;
	var->value = DirectFunctionCall1(namein,
						  CStringGetDatum(trigdata->tg_trigger->tgname));

	var = (PLpgSQL_var *) (estate.datums[func->tg_when_varno]);
	var->isnull = false;
	var->freeval = true;
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		var->value = DirectFunctionCall1(textin, CStringGetDatum("BEFORE"));
	else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		var->value = DirectFunctionCall1(textin, CStringGetDatum("AFTER"));
	else
		elog(ERROR, "unrecognized trigger execution time: not BEFORE or AFTER");

	var = (PLpgSQL_var *) (estate.datums[func->tg_level_varno]);
	var->isnull = false;
	var->freeval = true;
	if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		var->value = DirectFunctionCall1(textin, CStringGetDatum("ROW"));
	else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		var->value = DirectFunctionCall1(textin, CStringGetDatum("STATEMENT"));
	else
		elog(ERROR, "unrecognized trigger event type: not ROW or STATEMENT");

	var = (PLpgSQL_var *) (estate.datums[func->tg_relid_varno]);
	var->isnull = false;
	var->freeval = false;
	var->value = ObjectIdGetDatum(trigdata->tg_relation->rd_id);

	var = (PLpgSQL_var *) (estate.datums[func->tg_relname_varno]);
	var->isnull = false;
	var->freeval = true;
	var->value = DirectFunctionCall1(namein,
		CStringGetDatum(RelationGetRelationName(trigdata->tg_relation)));

	var = (PLpgSQL_var *) (estate.datums[func->tg_nargs_varno]);
	var->isnull = false;
	var->freeval = false;
	var->value = Int16GetDatum(trigdata->tg_trigger->tgnargs);

	/*
	 * Store the actual call argument values into the special execution
	 * state variables
	 */
	estate.err_text = gettext_noop("while storing call arguments into local variables");
	estate.trig_nargs = trigdata->tg_trigger->tgnargs;
	if (estate.trig_nargs == 0)
		estate.trig_argv = NULL;
	else
	{
		estate.trig_argv = palloc(sizeof(Datum) * estate.trig_nargs);
		for (i = 0; i < trigdata->tg_trigger->tgnargs; i++)
			estate.trig_argv[i] = DirectFunctionCall1(textin,
					   CStringGetDatum(trigdata->tg_trigger->tgargs[i]));
	}

	/*
	 * Initialize the other variables to NULL values for now. The default
	 * values are set when the blocks are entered.
	 */
	estate.err_text = gettext_noop("while initializing local variables to NULL");
	for (i = estate.found_varno; i < estate.ndatums; i++)
	{
		switch (estate.datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) estate.datums[i];

					var->value = 0;
					var->isnull = true;
					var->freeval = false;
				}
				break;

			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_REC:
			case PLPGSQL_DTYPE_RECFIELD:
			case PLPGSQL_DTYPE_ARRAYELEM:
			case PLPGSQL_DTYPE_TRIGARG:
				break;

			default:
				elog(ERROR, "unrecognized dtype: %d", func->datums[i]->dtype);
		}
	}

	/*
	 * Set the magic variable FOUND to false
	 */
	exec_set_found(&estate, false);

	/*
	 * Now call the toplevel block of statements
	 */
	estate.err_text = NULL;
	estate.err_stmt = (PLpgSQL_stmt *) (func->action);
	if (exec_stmt_block(&estate, func->action) != PLPGSQL_RC_RETURN)
	{
		estate.err_stmt = NULL;
		estate.err_text = NULL;
		ereport(ERROR,
		   (errcode(ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT),
			errmsg("control reached end of trigger procedure without RETURN")));
	}

	if (estate.retisset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("trigger procedure cannot return a set")));

	/*
	 * Check that the returned tuple structure has the same attributes,
	 * the relation that fired the trigger has. A per-statement trigger
	 * always needs to return NULL, so we ignore any return value the
	 * function itself produces (XXX: is this a good idea?)
	 *
	 * XXX This way it is possible, that the trigger returns a tuple where
	 * attributes don't have the correct atttypmod's length. It's up to
	 * the trigger's programmer to ensure that this doesn't happen. Jan
	 */
	if (estate.retisnull || TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		rettup = NULL;
	else
	{
		if (!compatible_tupdesc(estate.rettupdesc,
								trigdata->tg_relation->rd_att))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("returned tuple structure does not match table of trigger event")));
		/* Copy tuple to upper executor memory */
		rettup = SPI_copytuple((HeapTuple) (estate.retval));
	}

	/* Clean up any leftover temporary memory */
	if (estate.eval_econtext != NULL)
		FreeExprContext(estate.eval_econtext);
	estate.eval_econtext = NULL;
	exec_eval_cleanup(&estate);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;

	/*
	 * Return the triggers result
	 */
	return rettup;
}


/*
 * error context callback to let us supply a call-stack traceback
 */
static void
plpgsql_exec_error_callback(void *arg)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) arg;

	/* safety check, shouldn't happen */
	if (estate->err_func == NULL)
		return;

	/* if we are doing RAISE, don't report its location */
	if (estate->err_text == raise_skip_msg)
		return;

	if (estate->err_stmt != NULL)
	{
		/* translator: last %s is a plpgsql statement type name */
		errcontext("PL/pgSQL function \"%s\" line %d at %s",
				   estate->err_func->fn_name,
				   estate->err_stmt->lineno,
				   plpgsql_stmt_typename(estate->err_stmt));
	}
	else if (estate->err_text != NULL)
	{
		/*
		 * We don't expend the cycles to run gettext() on err_text unless
		 * we actually need it.  Therefore, places that set up err_text
		 * should use gettext_noop() to ensure the strings get recorded in
		 * the message dictionary.
		 */

		/*
		 * translator: last %s is a phrase such as "while storing call
		 * arguments into local variables"
		 */
		errcontext("PL/pgSQL function \"%s\" %s",
				   estate->err_func->fn_name,
				   gettext(estate->err_text));
	}
	else
		errcontext("PL/pgSQL function \"%s\"",
				   estate->err_func->fn_name);
}


/* ----------
 * Support functions for copying local execution variables
 * ----------
 */
static PLpgSQL_var *
copy_var(PLpgSQL_var * var)
{
	PLpgSQL_var *new = palloc(sizeof(PLpgSQL_var));

	memcpy(new, var, sizeof(PLpgSQL_var));
	new->freeval = false;

	return new;
}


static PLpgSQL_rec *
copy_rec(PLpgSQL_rec * rec)
{
	PLpgSQL_rec *new = palloc(sizeof(PLpgSQL_rec));

	memcpy(new, rec, sizeof(PLpgSQL_rec));
	new->tup = NULL;
	new->tupdesc = NULL;
	new->freetup = false;
	new->freetupdesc = false;

	return new;
}


/* ----------
 * exec_stmt_block			Execute a block of statements
 * ----------
 */
static int
exec_stmt_block(PLpgSQL_execstate * estate, PLpgSQL_stmt_block * block)
{
	int			rc;
	int			i;
	int			n;

	/*
	 * First initialize all variables declared in this block
	 */
	for (i = 0; i < block->n_initvars; i++)
	{
		n = block->initvarnos[i];

		switch (estate->datums[n]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) (estate->datums[n]);

					if (var->freeval)
					{
						pfree((void *) (var->value));
						var->freeval = false;
					}

					if (!var->isconst || var->isnull)
					{
						if (var->default_val == NULL)
						{
							var->value = (Datum) 0;
							var->isnull = true;
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
				}
				break;

			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) (estate->datums[n]);

					if (rec->freetup)
					{
						heap_freetuple(rec->tup);
						FreeTupleDesc(rec->tupdesc);
						rec->freetup = false;
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

	/*
	 * Execute the statements in the block's body
	 */
	rc = exec_stmts(estate, block->body);

	/*
	 * Handle the return code.
	 */
	switch (rc)
	{
		case PLPGSQL_RC_OK:
			return PLPGSQL_RC_OK;

		case PLPGSQL_RC_EXIT:
			if (estate->exitlabel == NULL)
				return PLPGSQL_RC_OK;
			if (block->label == NULL)
				return PLPGSQL_RC_EXIT;
			if (strcmp(block->label, estate->exitlabel))
				return PLPGSQL_RC_EXIT;
			estate->exitlabel = NULL;
			return PLPGSQL_RC_OK;

		case PLPGSQL_RC_RETURN:
			return PLPGSQL_RC_RETURN;

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
exec_stmts(PLpgSQL_execstate * estate, PLpgSQL_stmts * stmts)
{
	int			rc;
	int			i;

	for (i = 0; i < stmts->stmts_used; i++)
	{
		rc = exec_stmt(estate, (PLpgSQL_stmt *) (stmts->stmts[i]));
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
exec_stmt(PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt)
{
	PLpgSQL_stmt *save_estmt;
	int			rc = -1;

	save_estmt = estate->err_stmt;
	estate->err_stmt = stmt;

	CHECK_FOR_INTERRUPTS();

	switch (stmt->cmd_type)
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

		case PLPGSQL_STMT_SELECT:
			rc = exec_stmt_select(estate, (PLpgSQL_stmt_select *) stmt);
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

		case PLPGSQL_STMT_RAISE:
			rc = exec_stmt_raise(estate, (PLpgSQL_stmt_raise *) stmt);
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

	estate->err_stmt = save_estmt;

	return rc;
}


/* ----------
 * exec_stmt_assign			Evaluate an expression and
 *					put the result into a variable.
 * ----------
 */
static int
exec_stmt_assign(PLpgSQL_execstate * estate, PLpgSQL_stmt_assign * stmt)
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
exec_stmt_perform(PLpgSQL_execstate * estate, PLpgSQL_stmt_perform * stmt)
{
	PLpgSQL_expr *expr = stmt->expr;
	int			rc;

	/*
	 * If not already done create a plan for this expression
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr);

	rc = exec_run_select(estate, expr, 0, NULL);
	if (rc != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			   errmsg("query \"%s\" did not return data", expr->query)));

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
exec_stmt_getdiag(PLpgSQL_execstate * estate, PLpgSQL_stmt_getdiag * stmt)
{
	int			i;
	PLpgSQL_datum *var;
	bool		isnull = false;

	for (i = 0; i < stmt->ndtitems; i++)
	{
		PLpgSQL_diag_item *dtitem = &stmt->dtitems[i];

		if (dtitem->target <= 0)
			continue;

		var = (estate->datums[dtitem->target]);

		if (var == NULL)
			continue;

		switch (dtitem->item)
		{
			case PLPGSQL_GETDIAG_ROW_COUNT:

				exec_assign_value(estate, var,
								  UInt32GetDatum(estate->eval_processed),
								  INT4OID, &isnull);
				break;

			case PLPGSQL_GETDIAG_RESULT_OID:

				exec_assign_value(estate, var,
								  ObjectIdGetDatum(estate->eval_lastoid),
								  OIDOID, &isnull);
				break;

			default:
				elog(ERROR, "unrecognized attribute request: %d",
					 dtitem->item);
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
exec_stmt_if(PLpgSQL_execstate * estate, PLpgSQL_stmt_if * stmt)
{
	bool		value;
	bool		isnull = false;

	value = exec_eval_boolean(estate, stmt->cond, &isnull);
	exec_eval_cleanup(estate);

	if (!isnull && value)
	{
		if (stmt->true_body != NULL)
			return exec_stmts(estate, stmt->true_body);
	}
	else
	{
		if (stmt->false_body != NULL)
			return exec_stmts(estate, stmt->false_body);
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_loop			Loop over statements until
 *					an exit occurs.
 * ----------
 */
static int
exec_stmt_loop(PLpgSQL_execstate * estate, PLpgSQL_stmt_loop * stmt)
{
	int			rc;

	for (;;)
	{
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
				if (strcmp(stmt->label, estate->exitlabel))
					return PLPGSQL_RC_EXIT;
				estate->exitlabel = NULL;
				return PLPGSQL_RC_OK;

			case PLPGSQL_RC_RETURN:
				return PLPGSQL_RC_RETURN;

			default:
				elog(ERROR, "unrecognized rc: %d", rc);
		}
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_while			Loop over statements as long
 *					as an expression evaluates to
 *					true or an exit occurs.
 * ----------
 */
static int
exec_stmt_while(PLpgSQL_execstate * estate, PLpgSQL_stmt_while * stmt)
{
	bool		value;
	bool		isnull = false;
	int			rc;

	for (;;)
	{
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
				if (strcmp(stmt->label, estate->exitlabel))
					return PLPGSQL_RC_EXIT;
				estate->exitlabel = NULL;
				return PLPGSQL_RC_OK;

			case PLPGSQL_RC_RETURN:
				return PLPGSQL_RC_RETURN;

			default:
				elog(ERROR, "unrecognized rc: %d", rc);
		}
	}

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_fori			Iterate an integer variable
 *					from a lower to an upper value.
 *					Loop can be left with exit.
 * ----------
 */
static int
exec_stmt_fori(PLpgSQL_execstate * estate, PLpgSQL_stmt_fori * stmt)
{
	PLpgSQL_var *var;
	Datum		value;
	Oid			valtype;
	bool		isnull = false;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;

	var = (PLpgSQL_var *) (estate->datums[stmt->var->varno]);

	/*
	 * Get the value of the lower bound into the loop var
	 */
	value = exec_eval_expr(estate, stmt->lower, &isnull, &valtype);
	value = exec_cast_value(value, valtype, var->datatype->typoid,
							&(var->datatype->typinput),
							var->datatype->typelem,
							var->datatype->atttypmod, &isnull);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("lower bound of FOR loop cannot be NULL")));
	var->value = value;
	var->isnull = false;
	exec_eval_cleanup(estate);

	/*
	 * Get the value of the upper bound
	 */
	value = exec_eval_expr(estate, stmt->upper, &isnull, &valtype);
	value = exec_cast_value(value, valtype, var->datatype->typoid,
							&(var->datatype->typinput),
							var->datatype->typelem,
							var->datatype->atttypmod, &isnull);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("upper bound of FOR loop cannot be NULL")));
	exec_eval_cleanup(estate);

	/*
	 * Now do the loop
	 */
	for (;;)
	{
		/*
		 * Check bounds
		 */
		if (stmt->reverse)
		{
			if ((int4) (var->value) < (int4) value)
				break;
		}
		else
		{
			if ((int4) (var->value) > (int4) value)
				break;
		}

		found = true;			/* looped at least once */

		/*
		 * Execute the statements
		 */
		rc = exec_stmts(estate, stmt->body);

		if (rc == PLPGSQL_RC_RETURN)
			break;				/* return from function */
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
			 * otherwise, we processed a labelled exit that does not match
			 * the current statement's label, if any: return RC_EXIT so
			 * that the EXIT continues to recurse upward.
			 */

			break;
		}

		/*
		 * Increase/decrease loop var
		 */
		if (stmt->reverse)
			var->value--;
		else
			var->value++;
	}

	/*
	 * Set the FOUND variable to indicate the result of executing the loop
	 * (namely, whether we looped one or more times). This must be set
	 * here so that it does not interfere with the value of the FOUND
	 * variable inside the loop processing itself.
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
exec_stmt_fors(PLpgSQL_execstate * estate, PLpgSQL_stmt_fors * stmt)
{
	PLpgSQL_rec *rec = NULL;
	PLpgSQL_row *row = NULL;
	SPITupleTable *tuptab;
	Portal		portal;
	bool		found = false;
	int			rc = PLPGSQL_RC_OK;
	int			i;
	int			n;

	/*
	 * Determine if we assign to a record or a row
	 */
	if (stmt->rec != NULL)
		rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->recno]);
	else if (stmt->row != NULL)
		row = (PLpgSQL_row *) (estate->datums[stmt->row->rowno]);
	else
		elog(ERROR, "unsupported target");

	/*
	 * Open the implicit cursor for the statement and fetch the initial 10
	 * rows.
	 */
	exec_run_select(estate, stmt->query, 0, &portal);

	SPI_cursor_fetch(portal, true, 10);
	tuptab = SPI_tuptable;
	n = SPI_processed;

	/*
	 * If the query didn't return any rows, set the target to NULL and
	 * return with FOUND = false.
	 */
	if (n == 0)
		exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
	else
		found = true;			/* processed at least one tuple */

	/*
	 * Now do the loop
	 */
	while (n > 0)
	{
		for (i = 0; i < n; i++)
		{
			/*
			 * Assign the tuple to the target
			 */
			exec_move_row(estate, rec, row, tuptab->vals[i], tuptab->tupdesc);

			/*
			 * Execute the statements
			 */
			rc = exec_stmts(estate, stmt->body);

			if (rc != PLPGSQL_RC_OK)
			{
				/*
				 * We're aborting the loop, so cleanup and set FOUND.
				 * (This code should match the code after the loop.)
				 */
				SPI_freetuptable(tuptab);
				SPI_cursor_close(portal);
				exec_set_found(estate, found);

				if (rc == PLPGSQL_RC_EXIT)
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
					 * otherwise, we processed a labelled exit that does
					 * not match the current statement's label, if any:
					 * return RC_EXIT so that the EXIT continues to
					 * recurse upward.
					 */
				}

				return rc;
			}
		}

		SPI_freetuptable(tuptab);

		/*
		 * Fetch the next 50 tuples
		 */
		SPI_cursor_fetch(portal, true, 50);
		n = SPI_processed;
		tuptab = SPI_tuptable;
	}

	/*
	 * Release last group of tuples
	 */
	SPI_freetuptable(tuptab);

	/*
	 * Close the implicit cursor
	 */
	SPI_cursor_close(portal);

	/*
	 * Set the FOUND variable to indicate the result of executing the loop
	 * (namely, whether we looped one or more times). This must be set
	 * here so that it does not interfere with the value of the FOUND
	 * variable inside the loop processing itself.
	 */
	exec_set_found(estate, found);

	return rc;
}


/* ----------
 * exec_stmt_select			Run a query and assign the first
 *					row to a record or rowtype.
 * ----------
 */
static int
exec_stmt_select(PLpgSQL_execstate * estate, PLpgSQL_stmt_select * stmt)
{
	PLpgSQL_rec *rec = NULL;
	PLpgSQL_row *row = NULL;
	SPITupleTable *tuptab;
	uint32		n;

	/*
	 * Initialize the global found variable to false
	 */
	exec_set_found(estate, false);

	/*
	 * Determine if we assign to a record or a row
	 */
	if (stmt->rec != NULL)
		rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->recno]);
	else if (stmt->row != NULL)
		row = (PLpgSQL_row *) (estate->datums[stmt->row->rowno]);
	else
		elog(ERROR, "unsupported target");

	/*
	 * Run the query
	 */
	exec_run_select(estate, stmt->query, 1, NULL);
	tuptab = estate->eval_tuptable;
	n = estate->eval_processed;

	/*
	 * If the query didn't return any row, set the target to NULL and
	 * return.
	 */
	if (n == 0)
	{
		exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
		exec_eval_cleanup(estate);
		return PLPGSQL_RC_OK;
	}

	/*
	 * Put the result into the target and set found to true
	 */
	exec_move_row(estate, rec, row, tuptab->vals[0], tuptab->tupdesc);
	exec_set_found(estate, true);

	exec_eval_cleanup(estate);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_exit			Start exiting loop(s) or blocks
 * ----------
 */
static int
exec_stmt_exit(PLpgSQL_execstate * estate, PLpgSQL_stmt_exit * stmt)
{
	/*
	 * If the exit has a condition, check that it's true
	 */
	if (stmt->cond != NULL)
	{
		bool		value;
		bool		isnull = false;

		value = exec_eval_boolean(estate, stmt->cond, &isnull);
		exec_eval_cleanup(estate);
		if (isnull || !value)
			return PLPGSQL_RC_OK;
	}

	estate->exitlabel = stmt->label;
	return PLPGSQL_RC_EXIT;
}


/* ----------
 * exec_stmt_return			Evaluate an expression and start
 *					returning from the function.
 * ----------
 */
static int
exec_stmt_return(PLpgSQL_execstate * estate, PLpgSQL_stmt_return * stmt)
{
	/*
	 * If processing a set-returning PL/PgSQL function, the final RETURN
	 * indicates that the function is finished producing tuples.  The rest
	 * of the work will be done at the top level.
	 */
	if (estate->retisset)
		return PLPGSQL_RC_RETURN;

	if (estate->retistuple)
	{
		/* initialize for null result tuple */
		estate->retval = (Datum) 0;
		estate->rettupdesc = NULL;
		estate->retisnull = true;

		if (stmt->retrecno >= 0)
		{
			PLpgSQL_rec *rec = (PLpgSQL_rec *) (estate->datums[stmt->retrecno]);

			if (HeapTupleIsValid(rec->tup))
			{
				estate->retval = (Datum) rec->tup;
				estate->rettupdesc = rec->tupdesc;
				estate->retisnull = false;
			}
			return PLPGSQL_RC_RETURN;
		}

		if (stmt->retrowno >= 0)
		{
			PLpgSQL_row *row = (PLpgSQL_row *) (estate->datums[stmt->retrowno]);

			if (row->rowtupdesc) /* should always be true here */
			{
				estate->retval = (Datum) make_tuple_from_row(estate, row,
															 row->rowtupdesc);
				if (estate->retval == (Datum) NULL)	/* should not happen */
					elog(ERROR, "row not compatible with its own tupdesc");
				estate->rettupdesc = row->rowtupdesc;
				estate->retisnull = false;
			}
			return PLPGSQL_RC_RETURN;
		}

		if (stmt->expr != NULL)
		{
			exec_run_select(estate, stmt->expr, 1, NULL);
			if (estate->eval_processed > 0)
			{
				estate->retval = (Datum) estate->eval_tuptable->vals[0];
				estate->rettupdesc = estate->eval_tuptable->tupdesc;
				estate->retisnull = false;
			}
		}
		return PLPGSQL_RC_RETURN;
	}

	if (estate->fn_rettype == VOIDOID)
	{
		/* Special hack for function returning VOID */
		estate->retval = (Datum) 0;
		estate->retisnull = false;
		estate->rettype = VOIDOID;
	}
	else
	{
		/* Normal case for scalar results */
		estate->retval = exec_eval_expr(estate, stmt->expr,
										&(estate->retisnull),
										&(estate->rettype));
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
exec_stmt_return_next(PLpgSQL_execstate * estate,
					  PLpgSQL_stmt_return_next * stmt)
{
	TupleDesc	tupdesc;
	int			natts;
	HeapTuple	tuple;
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

	if (stmt->rec)
	{
		PLpgSQL_rec *rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->recno]);

		if (!HeapTupleIsValid(rec->tup))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("record \"%s\" is not assigned yet",
							rec->refname),
					 errdetail("The tuple structure of a not-yet-assigned record is indeterminate.")));
		if (!compatible_tupdesc(tupdesc, rec->tupdesc))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
				   errmsg("wrong record type supplied in RETURN NEXT")));
		tuple = rec->tup;
	}
	else if (stmt->row)
	{
		tuple = make_tuple_from_row(estate, stmt->row, tupdesc);
		if (tuple == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("wrong record type supplied in RETURN NEXT")));
		free_tuple = true;
	}
	else if (stmt->expr)
	{
		Datum		retval;
		bool		isNull;
		Oid			rettype;
		char		nullflag;

		if (natts != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
				   errmsg("wrong result type supplied in RETURN NEXT")));

		retval = exec_eval_expr(estate,
								stmt->expr,
								&isNull,
								&rettype);

		/* coerce type if needed */
		retval = exec_simple_cast_value(retval,
										rettype,
										tupdesc->attrs[0]->atttypid,
										tupdesc->attrs[0]->atttypmod,
										&isNull);

		nullflag = isNull ? 'n' : ' ';

		tuple = heap_formtuple(tupdesc, &retval, &nullflag);

		free_tuple = true;

		exec_eval_cleanup(estate);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("RETURN NEXT must have a parameter")));
		tuple = NULL;			/* keep compiler quiet */
	}

	if (HeapTupleIsValid(tuple))
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(estate->tuple_store_cxt);
		tuplestore_puttuple(estate->tuple_store, tuple);
		MemoryContextSwitchTo(oldcxt);

		if (free_tuple)
			heap_freetuple(tuple);
	}

	return PLPGSQL_RC_OK;
}

static void
exec_init_tuple_store(PLpgSQL_execstate * estate)
{
	ReturnSetInfo *rsi = estate->rsi;
	MemoryContext oldcxt;

	/*
	 * Check caller can handle a set result in the way we want
	 */
	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	estate->tuple_store_cxt = rsi->econtext->ecxt_per_query_memory;

	oldcxt = MemoryContextSwitchTo(estate->tuple_store_cxt);
	estate->tuple_store = tuplestore_begin_heap(true, false, SortMem);
	MemoryContextSwitchTo(oldcxt);

	estate->rettupdesc = rsi->expectedDesc;
}

/* ----------
 * exec_stmt_raise			Build a message and throw it with elog()
 * ----------
 */
static int
exec_stmt_raise(PLpgSQL_execstate * estate, PLpgSQL_stmt_raise * stmt)
{
	Oid			paramtypeid;
	Datum		paramvalue;
	bool		paramisnull;
	char	   *extval;
	int			pidx = 0;
	char		c[2] = {0, 0};
	char	   *cp;
	PLpgSQL_dstring ds;

	plpgsql_dstring_init(&ds);

	for (cp = stmt->message; *cp; cp++)
	{
		/*
		 * Occurences of a single % are replaced by the next argument's
		 * external representation. Double %'s are converted to one %.
		 */
		if ((c[0] = *cp) == '%')
		{
			cp++;
			if (*cp == '%')
			{
				plpgsql_dstring_append(&ds, c);
				continue;
			}
			cp--;
			if (pidx >= stmt->nparams)
			{
				plpgsql_dstring_append(&ds, c);
				continue;
			}
			exec_eval_datum(estate, estate->datums[stmt->params[pidx]],
							InvalidOid,
							&paramtypeid, &paramvalue, &paramisnull);
			if (paramisnull)
				extval = "<NULL>";
			else
				extval = convert_value_to_string(paramvalue, paramtypeid);
			plpgsql_dstring_append(&ds, extval);
			pidx++;
			continue;
		}

		/*
		 * Occurrences of single ' are removed. double ' are reduced to
		 * single ones.  We must do this because the parameter stored by
		 * the grammar is the raw T_STRING input literal, rather than the
		 * de-lexed string as you might expect ...
		 */
		if (*cp == '\'')
		{
			cp++;
			if (*cp == '\'')
				plpgsql_dstring_append(&ds, c);
			else
				cp--;
			continue;
		}
		plpgsql_dstring_append(&ds, c);
	}

	/*
	 * Throw the error (may or may not come back)
	 */
	estate->err_text = raise_skip_msg;	/* suppress traceback of raise */

	ereport(stmt->elog_level,
			(errmsg_internal("%s", plpgsql_dstring_get(&ds))));

	estate->err_text = NULL;	/* un-suppress... */

	plpgsql_dstring_free(&ds);

	return PLPGSQL_RC_OK;
}


/* ----------
 * Initialize a mostly empty execution state
 * ----------
 */
static void
plpgsql_estate_setup(PLpgSQL_execstate * estate,
					 PLpgSQL_function * func,
					 ReturnSetInfo *rsi)
{
	estate->retval = (Datum) 0;
	estate->retisnull = true;
	estate->rettype = InvalidOid;

	estate->fn_rettype = func->fn_rettype;
	estate->retistuple = func->fn_retistuple;
	estate->retisset = func->fn_retset;

	estate->rettupdesc = NULL;
	estate->exitlabel = NULL;

	estate->tuple_store = NULL;
	estate->tuple_store_cxt = NULL;
	estate->rsi = rsi;

	estate->trig_nargs = 0;
	estate->trig_argv = NULL;

	estate->found_varno = func->found_varno;
	estate->ndatums = func->ndatums;
	estate->datums = palloc(sizeof(PLpgSQL_datum *) * estate->ndatums);
	/* caller is expected to fill the datums array */

	estate->eval_tuptable = NULL;
	estate->eval_processed = 0;
	estate->eval_lastoid = InvalidOid;
	estate->eval_econtext = NULL;

	estate->err_func = func;
	estate->err_stmt = NULL;
	estate->err_text = NULL;
}

/* ----------
 * Release temporary memory used by expression/subselect evaluation
 *
 * NB: the result of the evaluation is no longer valid after this is done,
 * unless it is a pass-by-value datatype.
 * ----------
 */
static void
exec_eval_cleanup(PLpgSQL_execstate * estate)
{
	/* Clear result of a full SPI_exec */
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
exec_prepare_plan(PLpgSQL_execstate * estate,
				  PLpgSQL_expr * expr)
{
	int			i;
	_SPI_plan  *spi_plan;
	void	   *plan;
	Oid		   *argtypes;

	/*
	 * We need a temporary argtypes array to load with data. (The finished
	 * plan structure will contain a copy of it.)
	 *
	 * +1 is just to avoid palloc(0) error.
	 */
	argtypes = (Oid *) palloc(sizeof(Oid) * (expr->nparams + 1));

	for (i = 0; i < expr->nparams; i++)
	{
		Datum		paramval;
		bool		paramisnull;

		exec_eval_datum(estate, estate->datums[expr->params[i]],
						InvalidOid,
						&argtypes[i], &paramval, &paramisnull);
	}

	/*
	 * Generate and save the plan
	 */
	plan = SPI_prepare(expr->query, expr->nparams, argtypes);
	if (plan == NULL)
		elog(ERROR, "SPI_prepare() failed on \"%s\"", expr->query);
	expr->plan = SPI_saveplan(plan);
	spi_plan = (_SPI_plan *) expr->plan;
	expr->plan_argtypes = spi_plan->argtypes;
	expr->expr_simple_expr = NULL;
	exec_simple_check_plan(expr);

	SPI_freeplan(plan);
	pfree(argtypes);
}


/* ----------
 * exec_stmt_execsql			Execute an SQL statement not
 *					returning any data.
 * ----------
 */
static int
exec_stmt_execsql(PLpgSQL_execstate * estate,
				  PLpgSQL_stmt_execsql * stmt)
{
	int			i;
	Datum	   *values;
	char	   *nulls;
	int			rc;
	PLpgSQL_expr *expr = stmt->sqlstmt;

	/*
	 * On the first call for this expression generate the plan
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr);

	/*
	 * Now build up the values and nulls arguments for SPI_execp()
	 */
	values = palloc(sizeof(Datum) * (expr->nparams + 1));
	nulls = palloc(expr->nparams + 1);

	for (i = 0; i < expr->nparams; i++)
	{
		PLpgSQL_datum *datum = estate->datums[expr->params[i]];
		Oid			paramtypeid;
		bool		paramisnull;

		exec_eval_datum(estate, datum, expr->plan_argtypes[i],
						&paramtypeid, &values[i], &paramisnull);
		if (paramisnull)
			nulls[i] = 'n';
		else
			nulls[i] = ' ';
	}

	/*
	 * Execute the plan
	 */
	rc = SPI_execp(expr->plan, values, nulls, 0);
	switch (rc)
	{
		case SPI_OK_UTILITY:
		case SPI_OK_SELINTO:
			break;

		case SPI_OK_INSERT:
		case SPI_OK_DELETE:
		case SPI_OK_UPDATE:

			/*
			 * If the INSERT, DELETE, or UPDATE query affected at least
			 * one tuple, set the magic 'FOUND' variable to true. This
			 * conforms with the behavior of PL/SQL.
			 */
			exec_set_found(estate, (SPI_processed != 0));
			break;

		case SPI_OK_SELECT:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			   errmsg("SELECT query has no destination for result data"),
					 errhint("If you want to discard the results, use PERFORM instead.")));

		default:
			elog(ERROR, "error executing query \"%s\"", expr->query);
	}

	/*
	 * Release any result tuples from SPI_execp (probably shouldn't be
	 * any)
	 */
	SPI_freetuptable(SPI_tuptable);

	/* Save result info for GET DIAGNOSTICS */
	estate->eval_processed = SPI_processed;
	estate->eval_lastoid = SPI_lastoid;

	pfree(values);
	pfree(nulls);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_dynexecute			Execute a dynamic SQL query not
 *					returning any data.
 * ----------
 */
static int
exec_stmt_dynexecute(PLpgSQL_execstate * estate,
					 PLpgSQL_stmt_dynexecute * stmt)
{
	Datum		query;
	bool		isnull = false;
	Oid			restype;
	char	   *querystr;
	int			exec_res;

	/*
	 * First we evaluate the string expression after the EXECUTE keyword.
	 * It's result is the querystring we have to execute.
	 */
	query = exec_eval_expr(estate, stmt->query, &isnull, &restype);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cannot EXECUTE a null querystring")));

	/* Get the C-String representation */
	querystr = convert_value_to_string(query, restype);

	exec_eval_cleanup(estate);

	/*
	 * Call SPI_exec() without preparing a saved plan. The returncode can
	 * be any standard OK.	Note that while a SELECT is allowed, its
	 * results will be discarded.
	 */
	exec_res = SPI_exec(querystr, 0);
	switch (exec_res)
	{
		case SPI_OK_SELECT:
		case SPI_OK_INSERT:
		case SPI_OK_UPDATE:
		case SPI_OK_DELETE:
		case SPI_OK_UTILITY:
			break;

		case 0:

			/*
			 * Also allow a zero return, which implies the querystring
			 * contained no commands.
			 */
			break;

		case SPI_OK_SELINTO:

			/*
			 * We want to disallow SELECT INTO for now, because its
			 * behavior is not consistent with SELECT INTO in a normal
			 * plpgsql context. (We need to reimplement EXECUTE to parse
			 * the string as a plpgsql command, not just feed it to
			 * SPI_exec.) However, CREATE AS should be allowed ... and
			 * since it produces the same parsetree as SELECT INTO,
			 * there's no way to tell the difference except to look at the
			 * source text.  Wotta kluge!
			 */
			{
				char	   *ptr;

				for (ptr = querystr; *ptr; ptr++)
					if (!isspace((unsigned char) *ptr))
						break;
				if (*ptr == 'S' || *ptr == 's')
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("EXECUTE of SELECT ... INTO is not implemented yet")));
				break;
			}

		default:
			elog(ERROR, "unexpected error %d in EXECUTE of query \"%s\"",
				 exec_res, querystr);
			break;
	}

	/* Release any result from SPI_exec, as well as the querystring */
	SPI_freetuptable(SPI_tuptable);
	pfree(querystr);

	/* Save result info for GET DIAGNOSTICS */
	estate->eval_processed = SPI_processed;
	estate->eval_lastoid = SPI_lastoid;

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
exec_stmt_dynfors(PLpgSQL_execstate * estate, PLpgSQL_stmt_dynfors * stmt)
{
	Datum		query;
	bool		isnull = false;
	Oid			restype;
	char	   *querystr;
	PLpgSQL_rec *rec = NULL;
	PLpgSQL_row *row = NULL;
	SPITupleTable *tuptab;
	int			rc = PLPGSQL_RC_OK;
	int			i;
	int			n;
	void	   *plan;
	Portal		portal;
	bool		found = false;

	/*
	 * Determine if we assign to a record or a row
	 */
	if (stmt->rec != NULL)
		rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->recno]);
	else if (stmt->row != NULL)
		row = (PLpgSQL_row *) (estate->datums[stmt->row->rowno]);
	else
		elog(ERROR, "unsupported target");

	/*
	 * Evaluate the string expression after the EXECUTE keyword. It's
	 * result is the querystring we have to execute.
	 */
	query = exec_eval_expr(estate, stmt->query, &isnull, &restype);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cannot EXECUTE a null querystring")));

	/* Get the C-String representation */
	querystr = convert_value_to_string(query, restype);

	exec_eval_cleanup(estate);

	/*
	 * Prepare a plan and open an implicit cursor for the query
	 */
	plan = SPI_prepare(querystr, 0, NULL);
	if (plan == NULL)
		elog(ERROR, "SPI_prepare() failed for dynamic query \"%s\"", querystr);
	portal = SPI_cursor_open(NULL, plan, NULL, NULL);
	if (portal == NULL)
		elog(ERROR, "failed to open implicit cursor for dynamic query \"%s\"",
			 querystr);
	pfree(querystr);
	SPI_freeplan(plan);

	/*
	 * Fetch the initial 10 tuples
	 */
	SPI_cursor_fetch(portal, true, 10);
	tuptab = SPI_tuptable;
	n = SPI_processed;

	/*
	 * If the query didn't return any rows, set the target to NULL and
	 * return with FOUND = false.
	 */
	if (n == 0)
		exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
	else
		found = true;			/* processed at least one tuple */

	/*
	 * Now do the loop
	 */
	while (n > 0)
	{
		for (i = 0; i < n; i++)
		{
			/*
			 * Assign the tuple to the target
			 */
			exec_move_row(estate, rec, row, tuptab->vals[i], tuptab->tupdesc);

			/*
			 * Execute the statements
			 */
			rc = exec_stmts(estate, stmt->body);

			if (rc != PLPGSQL_RC_OK)
			{
				/*
				 * We're aborting the loop, so cleanup and set FOUND.
				 * (This code should match the code after the loop.)
				 */
				SPI_freetuptable(tuptab);
				SPI_cursor_close(portal);
				exec_set_found(estate, found);

				if (rc == PLPGSQL_RC_EXIT)
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
					 * otherwise, we processed a labelled exit that does
					 * not match the current statement's label, if any:
					 * return RC_EXIT so that the EXIT continues to
					 * recurse upward.
					 */
				}

				return rc;
			}
		}

		SPI_freetuptable(tuptab);

		/*
		 * Fetch the next 50 tuples
		 */
		SPI_cursor_fetch(portal, true, 50);
		n = SPI_processed;
		tuptab = SPI_tuptable;
	}

	/*
	 * Release last group of tuples
	 */
	SPI_freetuptable(tuptab);

	/*
	 * Close the implicit cursor
	 */
	SPI_cursor_close(portal);

	/*
	 * Set the FOUND variable to indicate the result of executing the loop
	 * (namely, whether we looped one or more times). This must be set
	 * here so that it does not interfere with the value of the FOUND
	 * variable inside the loop processing itself.
	 */
	exec_set_found(estate, found);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_open			Execute an OPEN cursor statement
 * ----------
 */
static int
exec_stmt_open(PLpgSQL_execstate * estate, PLpgSQL_stmt_open * stmt)
{
	PLpgSQL_var *curvar = NULL;
	char	   *curname = NULL;
	PLpgSQL_expr *query = NULL;
	Portal		portal;
	int			i;
	Datum	   *values;
	char	   *nulls;
	bool		isnull;


	/* ----------
	 * Get the cursor variable and if it has an assigned name, check
	 * that it's not in use currently.
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (!curvar->isnull)
	{
		curname = DatumGetCString(DirectFunctionCall1(textout, curvar->value));
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
			exec_prepare_plan(estate, query);
	}
	else if (stmt->dynquery != NULL)
	{
		/* ----------
		 * This is an OPEN refcursor FOR EXECUTE ...
		 * ----------
		 */
		Datum		queryD;
		Oid			restype;
		char	   *querystr;
		void	   *curplan;

		/* ----------
		 * We evaluate the string expression after the
		 * EXECUTE keyword. It's result is the querystring we have
		 * to execute.
		 * ----------
		 */
		queryD = exec_eval_expr(estate, stmt->dynquery, &isnull, &restype);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("cannot EXECUTE a null querystring")));

		/* Get the C-String representation */
		querystr = convert_value_to_string(queryD, restype);

		exec_eval_cleanup(estate);

		/* ----------
		 * Now we prepare a query plan for it and open a cursor
		 * ----------
		 */
		curplan = SPI_prepare(querystr, 0, NULL);
		if (curplan == NULL)
			elog(ERROR, "SPI_prepare() failed for dynamic query \"%s\"",
				 querystr);
		portal = SPI_cursor_open(curname, curplan, NULL, NULL);
		if (portal == NULL)
			elog(ERROR, "failed to open cursor");
		pfree(querystr);
		SPI_freeplan(curplan);

		/* ----------
		 * Store the eventually assigned cursor name in the cursor variable
		 * ----------
		 */
		if (curvar->freeval)
			pfree((void *) (curvar->value));

		curvar->value = DirectFunctionCall1(textin, CStringGetDatum(portal->name));
		curvar->isnull = false;
		curvar->freeval = true;

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
			 * Er - OPEN CURSOR (args). We fake a SELECT ... INTO ...
			 * statement to evaluate the args and put 'em into the
			 * internal row.
			 * ----------
			 */
			PLpgSQL_stmt_select set_args;

			if (curvar->cursor_explicit_argrow < 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("arguments given for cursor without arguments")));

			memset(&set_args, 0, sizeof(set_args));
			set_args.cmd_type = PLPGSQL_STMT_SELECT;
			set_args.lineno = stmt->lineno;
			set_args.row = (PLpgSQL_row *)
				(estate->datums[curvar->cursor_explicit_argrow]);
			set_args.query = stmt->argquery;

			if (exec_stmt_select(estate, &set_args) != PLPGSQL_RC_OK)
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
			exec_prepare_plan(estate, query);
	}

	/* ----------
	 * Here we go if we have a saved plan where we have to put
	 * values into, either from an explicit cursor or from a
	 * refcursor opened with OPEN ... FOR SELECT ...;
	 * ----------
	 */
	values = palloc(sizeof(Datum) * (query->nparams + 1));
	nulls = palloc(query->nparams + 1);

	for (i = 0; i < query->nparams; i++)
	{
		PLpgSQL_datum *datum = estate->datums[query->params[i]];
		Oid			paramtypeid;
		bool		paramisnull;

		exec_eval_datum(estate, datum, query->plan_argtypes[i],
						&paramtypeid, &values[i], &paramisnull);
		if (paramisnull)
			nulls[i] = 'n';
		else
			nulls[i] = ' ';
	}

	/* ----------
	 * Open the cursor
	 * ----------
	 */
	portal = SPI_cursor_open(curname, query->plan, values, nulls);
	if (portal == NULL)
		elog(ERROR, "failed to open cursor");

	pfree(values);
	pfree(nulls);
	if (curname)
		pfree(curname);

	/* ----------
	 * Store the eventually assigned portal name in the cursor variable
	 * ----------
	 */
	if (curvar->freeval)
		pfree((void *) (curvar->value));

	curvar->value = DirectFunctionCall1(textin, CStringGetDatum(portal->name));
	curvar->isnull = false;
	curvar->freeval = true;

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_fetch			Fetch from a cursor into a target
 * ----------
 */
static int
exec_stmt_fetch(PLpgSQL_execstate * estate, PLpgSQL_stmt_fetch * stmt)
{
	PLpgSQL_var *curvar = NULL;
	PLpgSQL_rec *rec = NULL;
	PLpgSQL_row *row = NULL;
	SPITupleTable *tuptab;
	Portal		portal;
	char	   *curname;
	int			n;

	/* ----------
	 * Get the portal of the cursor by name
	 * ----------
	 */
	curvar = (PLpgSQL_var *) (estate->datums[stmt->curvar]);
	if (curvar->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
			 errmsg("cursor variable \"%s\" is NULL", curvar->refname)));
	curname = DatumGetCString(DirectFunctionCall1(textout, curvar->value));

	portal = SPI_cursor_find(curname);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", curname)));
	pfree(curname);

	/* ----------
	 * Determine if we fetch into a record or a row
	 * ----------
	 */
	if (stmt->rec != NULL)
		rec = (PLpgSQL_rec *) (estate->datums[stmt->rec->recno]);
	else if (stmt->row != NULL)
		row = (PLpgSQL_row *) (estate->datums[stmt->row->rowno]);
	else
		elog(ERROR, "unsupported target");

	/* ----------
	 * Fetch 1 tuple from the cursor
	 * ----------
	 */
	SPI_cursor_fetch(portal, true, 1);
	tuptab = SPI_tuptable;
	n = SPI_processed;

	/* ----------
	 * Set the target and the global FOUND variable appropriately.
	 * ----------
	 */
	if (n == 0)
	{
		exec_move_row(estate, rec, row, NULL, tuptab->tupdesc);
		exec_set_found(estate, false);
	}
	else
	{
		exec_move_row(estate, rec, row, tuptab->vals[0], tuptab->tupdesc);
		exec_set_found(estate, true);
	}

	SPI_freetuptable(tuptab);

	return PLPGSQL_RC_OK;
}


/* ----------
 * exec_stmt_close			Close a cursor
 * ----------
 */
static int
exec_stmt_close(PLpgSQL_execstate * estate, PLpgSQL_stmt_close * stmt)
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
			 errmsg("cursor variable \"%s\" is NULL", curvar->refname)));
	curname = DatumGetCString(DirectFunctionCall1(textout, curvar->value));

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
 * exec_assign_expr			Put an expression's result into
 *					a variable.
 * ----------
 */
static void
exec_assign_expr(PLpgSQL_execstate * estate, PLpgSQL_datum * target,
				 PLpgSQL_expr * expr)
{
	Datum		value;
	Oid			valtype;
	bool		isnull = false;

	value = exec_eval_expr(estate, expr, &isnull, &valtype);
	exec_assign_value(estate, target, value, valtype, &isnull);
	exec_eval_cleanup(estate);
}


/* ----------
 * exec_assign_value			Put a value into a target field
 * ----------
 */
static void
exec_assign_value(PLpgSQL_execstate * estate,
				  PLpgSQL_datum * target,
				  Datum value, Oid valtype, bool *isNull)
{
	PLpgSQL_var *var;
	PLpgSQL_rec *rec;
	PLpgSQL_recfield *recfield;
	int			fno;
	int			i;
	int			natts;
	Datum	   *values;
	char	   *nulls;
	void	   *mustfree;
	Datum		newvalue;
	bool		attisnull;
	Oid			atttype;
	int32		atttypmod;
	int			nsubscripts;
	PLpgSQL_expr *subscripts[MAXDIM];
	int			subscriptvals[MAXDIM];
	bool		havenullsubscript,
				oldarrayisnull;
	Oid			arraytypeid,
				arrayelemtypeid,
				arrayInputFn;
	int16		elemtyplen;
	bool		elemtypbyval;
	char		elemtypalign;
	Datum		oldarrayval,
				coerced_value;
	ArrayType  *newarrayval;
	HeapTuple	newtup;

	switch (target->dtype)
	{
		case PLPGSQL_DTYPE_VAR:

			/*
			 * Target is a variable
			 */
			var = (PLpgSQL_var *) target;

			newvalue = exec_cast_value(value, valtype, var->datatype->typoid,
									   &(var->datatype->typinput),
									   var->datatype->typelem,
									   var->datatype->atttypmod,
									   isNull);

			if (*isNull && var->notnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("NULL cannot be assigned to variable \"%s\" declared NOT NULL",
								var->refname)));

			/*
			 * If type is by-reference, make sure we have a freshly
			 * palloc'd copy; the originally passed value may not live as
			 * long as the variable!  But we don't need to re-copy if
			 * exec_cast_value performed a conversion; its output must
			 * already be palloc'd.
			 */
			if (!var->datatype->typbyval && !*isNull)
			{
				if (newvalue == value)
					newvalue = datumCopy(newvalue,
										 false,
										 var->datatype->typlen);
			}

			/*
			 * Now free the old value.  (We can't do this any earlier
			 * because of the possibility that we are assigning the
			 * var's old value to it, eg "foo := foo".  We could optimize
			 * out the assignment altogether in such cases, but it's too
			 * infrequent to be worth testing for.)
			 */
			if (var->freeval)
			{
				pfree(DatumGetPointer(var->value));
				var->freeval = false;
			}

			var->value = newvalue;
			var->isnull = *isNull;
			if (!var->datatype->typbyval && !*isNull)
				var->freeval = true;
			break;

		case PLPGSQL_DTYPE_RECFIELD:

			/*
			 * Target is a field of a record
			 */
			recfield = (PLpgSQL_recfield *) target;
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
			 * number of attributes in the tuple.
			 */
			fno = SPI_fnumber(rec->tupdesc, recfield->fieldname);
			if (fno == SPI_ERROR_NOATTRIBUTE)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("record \"%s\" has no field \"%s\"",
								rec->refname, recfield->fieldname)));
			fno--;
			natts = rec->tupdesc->natts;

			/*
			 * Set up values/datums arrays for heap_formtuple.	For all
			 * the attributes except the one we want to replace, use the
			 * value that's in the old tuple.
			 */
			values = palloc(sizeof(Datum) * natts);
			nulls = palloc(natts);

			for (i = 0; i < natts; i++)
			{
				if (i == fno)
					continue;
				values[i] = SPI_getbinval(rec->tup, rec->tupdesc,
										  i + 1, &attisnull);
				if (attisnull)
					nulls[i] = 'n';
				else
					nulls[i] = ' ';
			}

			/*
			 * Now insert the new value, being careful to cast it to the
			 * right type.
			 */
			atttype = SPI_gettypeid(rec->tupdesc, fno + 1);
			atttypmod = rec->tupdesc->attrs[fno]->atttypmod;
			attisnull = *isNull;
			values[fno] = exec_simple_cast_value(value,
												 valtype,
												 atttype,
												 atttypmod,
												 &attisnull);
			if (attisnull)
				nulls[fno] = 'n';
			else
				nulls[fno] = ' ';

			/*
			 * Avoid leaking the result of exec_simple_cast_value, if it
			 * performed a conversion to a pass-by-ref type.
			 */
			if (!attisnull && values[fno] != value && !get_typbyval(atttype))
				mustfree = DatumGetPointer(values[fno]);
			else
				mustfree = NULL;

			/*
			 * Now call heap_formtuple() to create a new tuple that
			 * replaces the old one in the record.
			 */
			newtup = heap_formtuple(rec->tupdesc, values, nulls);

			if (rec->freetup)
				heap_freetuple(rec->tup);

			rec->tup = newtup;
			rec->freetup = true;

			pfree(values);
			pfree(nulls);
			if (mustfree)
				pfree(mustfree);

			break;

		case PLPGSQL_DTYPE_ARRAYELEM:

			/*
			 * Target is an element of an array
			 *
			 * To handle constructs like x[1][2] := something, we have to be
			 * prepared to deal with a chain of arrayelem datums. Chase
			 * back to find the base array datum, and save the subscript
			 * expressions as we go.  (We are scanning right to left here,
			 * but want to evaluate the subscripts left-to-right to
			 * minimize surprises.)
			 */
			nsubscripts = 0;
			do
			{
				PLpgSQL_arrayelem *arrayelem = (PLpgSQL_arrayelem *) target;

				if (nsubscripts >= MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("number of array dimensions exceeds the maximum allowed, %d",
									MAXDIM)));
				subscripts[nsubscripts++] = arrayelem->subscript;
				target = estate->datums[arrayelem->arrayparentno];
			} while (target->dtype == PLPGSQL_DTYPE_ARRAYELEM);

			/* Fetch current value of array datum */
			exec_eval_datum(estate, target, InvalidOid,
							&arraytypeid, &oldarrayval, &oldarrayisnull);

			getTypeInputInfo(arraytypeid, &arrayInputFn, &arrayelemtypeid);
			if (!OidIsValid(arrayelemtypeid))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("subscripted object is not an array")));

			/* Evaluate the subscripts, switch into left-to-right order */
			havenullsubscript = false;
			for (i = 0; i < nsubscripts; i++)
			{
				bool		subisnull;

				subscriptvals[i] =
					exec_eval_integer(estate,
									  subscripts[nsubscripts - 1 - i],
									  &subisnull);
				havenullsubscript |= subisnull;
			}

			/*
			 * Skip the assignment if we have any nulls, either in the
			 * original array value, the subscripts, or the righthand
			 * side. This is pretty bogus but it corresponds to the
			 * current behavior of ExecEvalArrayRef().
			 */
			if (oldarrayisnull || havenullsubscript || *isNull)
				return;

			/* Coerce source value to match array element type. */
			coerced_value = exec_simple_cast_value(value,
												   valtype,
												   arrayelemtypeid,
												   -1,
												   isNull);

			/*
			 * Build the modified array value.
			 */
			get_typlenbyvalalign(arrayelemtypeid,
								 &elemtyplen,
								 &elemtypbyval,
								 &elemtypalign);

			newarrayval = array_set((ArrayType *) DatumGetPointer(oldarrayval),
									nsubscripts,
									subscriptvals,
									coerced_value,
									get_typlen(arraytypeid),
									elemtyplen,
									elemtypbyval,
									elemtypalign,
									isNull);

			/*
			 * Assign it to the base variable.
			 */
			exec_assign_value(estate, target,
							  PointerGetDatum(newarrayval),
							  arraytypeid, isNull);

			/*
			 * Avoid leaking the result of exec_simple_cast_value, if it
			 * performed a conversion to a pass-by-ref type.
			 */
			if (!*isNull && coerced_value != value && !elemtypbyval)
				pfree(DatumGetPointer(coerced_value));

			/*
			 * Avoid leaking the modified array value, too.
			 */
			pfree(newarrayval);
			break;

		default:
			elog(ERROR, "unrecognized dtype: %d", target->dtype);
	}
}

/*
 * exec_eval_datum				Get current value of a PLpgSQL_datum
 *
 * The type oid, value in Datum format, and null flag are returned.
 *
 * If expectedtypeid isn't InvalidOid, it is checked against the actual type.
 *
 * This obviously only handles scalar datums (not whole records or rows);
 * at present it doesn't need to handle PLpgSQL_expr datums, either.
 *
 * NOTE: caller must not modify the returned value, since it points right
 * at the stored value in the case of pass-by-reference datatypes.
 */
static void
exec_eval_datum(PLpgSQL_execstate * estate,
				PLpgSQL_datum * datum,
				Oid expectedtypeid,
				Oid *typeid,
				Datum *value,
				bool *isnull)
{
	PLpgSQL_var *var;
	PLpgSQL_rec *rec;
	PLpgSQL_recfield *recfield;
	PLpgSQL_trigarg *trigarg;
	int			tgargno;
	int			fno;

	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
			var = (PLpgSQL_var *) datum;
			*typeid = var->datatype->typoid;
			*value = var->value;
			*isnull = var->isnull;
			if (expectedtypeid != InvalidOid && expectedtypeid != *typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of \"%s\" does not match that when preparing the plan",
								var->refname)));
			break;

		case PLPGSQL_DTYPE_RECFIELD:
			recfield = (PLpgSQL_recfield *) datum;
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
			*value = SPI_getbinval(rec->tup, rec->tupdesc, fno, isnull);
			if (expectedtypeid != InvalidOid && expectedtypeid != *typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of \"%s.%s\" does not match that when preparing the plan",
								rec->refname, recfield->fieldname)));
			break;

		case PLPGSQL_DTYPE_TRIGARG:
			trigarg = (PLpgSQL_trigarg *) datum;
			*typeid = TEXTOID;
			tgargno = exec_eval_integer(estate, trigarg->argnum, isnull);
			if (*isnull || tgargno < 0 || tgargno >= estate->trig_nargs)
			{
				*value = (Datum) 0;
				*isnull = true;
			}
			else
			{
				*value = estate->trig_argv[tgargno];
				*isnull = false;
			}
			if (expectedtypeid != InvalidOid && expectedtypeid != *typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of tgargv[%d] does not match that when preparing the plan",
								tgargno)));
			break;

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
	}
}

/* ----------
 * exec_eval_integer		Evaluate an expression, coerce result to int4
 *
 * Note we do not do exec_eval_cleanup here; the caller must do it at
 * some later point.  (We do this because the caller may be holding the
 * results of other, pass-by-reference, expression evaluations, such as
 * an array value to be subscripted.  Also see notes in exec_eval_simple_expr
 * about allocation of the parameter array.)
 * ----------
 */
static int
exec_eval_integer(PLpgSQL_execstate * estate,
				  PLpgSQL_expr * expr,
				  bool *isNull)
{
	Datum		exprdatum;
	Oid			exprtypeid;

	exprdatum = exec_eval_expr(estate, expr, isNull, &exprtypeid);
	exprdatum = exec_simple_cast_value(exprdatum, exprtypeid,
									   INT4OID, -1,
									   isNull);
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
exec_eval_boolean(PLpgSQL_execstate * estate,
				  PLpgSQL_expr * expr,
				  bool *isNull)
{
	Datum		exprdatum;
	Oid			exprtypeid;

	exprdatum = exec_eval_expr(estate, expr, isNull, &exprtypeid);
	exprdatum = exec_simple_cast_value(exprdatum, exprtypeid,
									   BOOLOID, -1,
									   isNull);
	return DatumGetBool(exprdatum);
}

/* ----------
 * exec_eval_expr			Evaluate an expression and return
 *					the result Datum.
 *
 * NOTE: caller must do exec_eval_cleanup when done with the Datum.
 * ----------
 */
static Datum
exec_eval_expr(PLpgSQL_execstate * estate,
			   PLpgSQL_expr * expr,
			   bool *isNull,
			   Oid *rettype)
{
	int			rc;

	/*
	 * If not already done create a plan for this expression
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr);

	/*
	 * If this is a simple expression, bypass SPI and use the executor
	 * directly
	 */
	if (expr->expr_simple_expr != NULL)
		return exec_eval_simple_expr(estate, expr, isNull, rettype);

	rc = exec_run_select(estate, expr, 2, NULL);
	if (rc != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			   errmsg("query \"%s\" did not return data", expr->query)));

	/*
	 * If there are no rows selected, the result is NULL.
	 */
	if (estate->eval_processed == 0)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * Check that the expression returned one single Datum
	 */
	if (estate->eval_processed > 1)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("query \"%s\" returned more than one row",
						expr->query)));
	if (estate->eval_tuptable->tupdesc->natts != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("query \"%s\" returned %d columns", expr->query,
						estate->eval_tuptable->tupdesc->natts)));

	/*
	 * Return the result and its type
	 */
	*rettype = SPI_gettypeid(estate->eval_tuptable->tupdesc, 1);
	return SPI_getbinval(estate->eval_tuptable->vals[0],
						 estate->eval_tuptable->tupdesc, 1, isNull);
}


/* ----------
 * exec_run_select			Execute a select query
 * ----------
 */
static int
exec_run_select(PLpgSQL_execstate * estate,
				PLpgSQL_expr * expr, int maxtuples, Portal *portalP)
{
	int			i;
	Datum	   *values;
	char	   *nulls;
	int			rc;

	/*
	 * On the first call for this expression generate the plan
	 */
	if (expr->plan == NULL)
		exec_prepare_plan(estate, expr);

	/*
	 * Now build up the values and nulls arguments for SPI_execp()
	 */
	values = palloc(sizeof(Datum) * (expr->nparams + 1));
	nulls = palloc(expr->nparams + 1);

	for (i = 0; i < expr->nparams; i++)
	{
		PLpgSQL_datum *datum = estate->datums[expr->params[i]];
		Oid			paramtypeid;
		bool		paramisnull;

		exec_eval_datum(estate, datum, expr->plan_argtypes[i],
						&paramtypeid, &values[i], &paramisnull);
		if (paramisnull)
			nulls[i] = 'n';
		else
			nulls[i] = ' ';
	}

	/*
	 * If a portal was requested, put the query into the portal
	 */
	if (portalP != NULL)
	{
		*portalP = SPI_cursor_open(NULL, expr->plan, values, nulls);
		if (*portalP == NULL)
			elog(ERROR, "failed to open implicit cursor for \"%s\"",
				 expr->query);
		pfree(values);
		pfree(nulls);
		return SPI_OK_CURSOR;
	}

	/*
	 * Execute the query
	 */
	rc = SPI_execp(expr->plan, values, nulls, maxtuples);
	if (rc != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("query \"%s\" is not a SELECT", expr->query)));

	/* Save query results for eventual cleanup */
	Assert(estate->eval_tuptable == NULL);
	estate->eval_tuptable = SPI_tuptable;
	estate->eval_processed = SPI_processed;
	estate->eval_lastoid = SPI_lastoid;

	pfree(values);
	pfree(nulls);

	return rc;
}


/* ----------
 * exec_eval_simple_expr -		Evaluate a simple expression returning
 *								a Datum by directly calling ExecEvalExpr().
 *
 * Note: if pass-by-reference, the result is in the eval_econtext's
 * temporary memory context.  It will be freed when exec_eval_cleanup
 * is done.
 * ----------
 */
static Datum
exec_eval_simple_expr(PLpgSQL_execstate * estate,
					  PLpgSQL_expr * expr,
					  bool *isNull,
					  Oid *rettype)
{
	Datum		retval;
	ExprContext *econtext;
	ParamListInfo paramLI;
	int			i;

	/*
	 * Pass back previously-determined result type.
	 */
	*rettype = expr->expr_simple_type;

	/*
	 * Create an EState for evaluation of simple expressions, if there's
	 * not one already in the current transaction.  The EState is made a
	 * child of TopTransactionContext so it will have the right lifespan.
	 */
	if (simple_eval_estate == NULL)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(TopTransactionContext);
		simple_eval_estate = CreateExecutorState();
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Prepare the expression for execution, if it's not been done already
	 * in the current transaction.
	 */
	if (expr->expr_simple_state == NULL)
	{
		expr->expr_simple_state = ExecPrepareExpr(expr->expr_simple_expr,
												  simple_eval_estate);
		/* Add it to list for cleanup */
		expr->expr_simple_next = active_simple_exprs;
		active_simple_exprs = expr;
	}

	/*
	 * Create an expression context for simple expressions, if there's
	 * not one already in the current function call.  This must be a
	 * child of simple_eval_estate.
	 */
	econtext = estate->eval_econtext;
	if (econtext == NULL)
	{
		econtext = CreateExprContext(simple_eval_estate);
		estate->eval_econtext = econtext;
	}

	/*
	 * Param list can live in econtext's temporary memory context.
	 *
	 * XXX think about avoiding repeated palloc's for param lists?
	 * Beware however that this routine is re-entrant: exec_eval_datum()
	 * can call it back for subscript evaluation, and so there can be a
	 * need to have more than one active param list.
	 */
	paramLI = (ParamListInfo)
		MemoryContextAlloc(econtext->ecxt_per_tuple_memory,
						(expr->nparams + 1) * sizeof(ParamListInfoData));

	/*
	 * Put the parameter values into the parameter list entries.
	 */
	for (i = 0; i < expr->nparams; i++)
	{
		PLpgSQL_datum *datum = estate->datums[expr->params[i]];
		Oid			paramtypeid;

		paramLI[i].kind = PARAM_NUM;
		paramLI[i].id = i + 1;
		exec_eval_datum(estate, datum, expr->plan_argtypes[i],
						&paramtypeid,
						&paramLI[i].value, &paramLI[i].isnull);
	}
	paramLI[i].kind = PARAM_INVALID;

	/*
	 * Now we can safely make the econtext point to the param list.
	 */
	econtext->ecxt_param_list_info = paramLI;

	/*
	 * Now call the executor to evaluate the expression
	 */
	SPI_push();
	retval = ExecEvalExprSwitchContext(expr->expr_simple_state,
									   econtext,
									   isNull,
									   NULL);
	SPI_pop();

	/*
	 * That's it.
	 */
	return retval;
}


/* ----------
 * exec_move_row			Move one tuple's values into a record or row
 * ----------
 */
static void
exec_move_row(PLpgSQL_execstate * estate,
			  PLpgSQL_rec * rec,
			  PLpgSQL_row * row,
			  HeapTuple tup, TupleDesc tupdesc)
{
	/*
	 * Record is simple - just copy the tuple and its descriptor into the
	 * record variable
	 */
	if (rec != NULL)
	{
		/*
		 * copy input first, just in case it is pointing at variable's value
		 */
		if (HeapTupleIsValid(tup))
			tup = heap_copytuple(tup);
		if (tupdesc)
			tupdesc = CreateTupleDescCopy(tupdesc);

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

		if (HeapTupleIsValid(tup))
		{
			rec->tup = tup;
			rec->freetup = true;
		}
		else if (tupdesc)
		{
			/* If we have a tupdesc but no data, form an all-nulls tuple */
			char	   *nulls;

			/* +1 to avoid possible palloc(0) if no attributes */
			nulls = (char *) palloc(tupdesc->natts * sizeof(char) + 1);
			memset(nulls, 'n', tupdesc->natts * sizeof(char));

			rec->tup = heap_formtuple(tupdesc, NULL, nulls);
			rec->freetup = true;

			pfree(nulls);
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
	 * NOTE: this code used to demand row->nfields == tup->t_data->t_natts,
	 * but that's wrong.  The tuple might have more fields than we
	 * expected if it's from an inheritance-child table of the current
	 * table, or it might have fewer if the table has had columns added by
	 * ALTER TABLE. Ignore extra columns and assume NULL for missing
	 * columns, the same as heap_getattr would do.  We also have to skip
	 * over dropped columns in either the source or destination.
	 *
	 * If we have no tuple data at all, we'll assign NULL to all columns of
	 * the row variable.
	 */
	if (row != NULL)
	{
		int			t_natts;
		int			fnum;
		int			anum;

		if (HeapTupleIsValid(tup))
			t_natts = tup->t_data->t_natts;
		else
			t_natts = 0;

		anum = 0;
		for (fnum = 0; fnum < row->nfields; fnum++)
		{
			PLpgSQL_var *var;
			Datum		value;
			bool		isnull;
			Oid			valtype;

			if (row->varnos[fnum] < 0)
				continue;		/* skip dropped column in row struct */

			var = (PLpgSQL_var *) (estate->datums[row->varnos[fnum]]);

			while (anum < t_natts && tupdesc->attrs[anum]->attisdropped)
				anum++;			/* skip dropped column in tuple */

			if (anum < t_natts)
			{
				value = SPI_getbinval(tup, tupdesc, anum + 1, &isnull);
				valtype = SPI_gettypeid(tupdesc, anum + 1);
				anum++;
			}
			else
			{
				value = (Datum) 0;
				isnull = true;
				valtype = InvalidOid;
			}

			exec_assign_value(estate, (PLpgSQL_datum *) var,
							  value, valtype, &isnull);
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
make_tuple_from_row(PLpgSQL_execstate * estate,
					PLpgSQL_row * row,
					TupleDesc tupdesc)
{
	int			natts = tupdesc->natts;
	HeapTuple	tuple;
	Datum	   *dvalues;
	char	   *nulls;
	int			i;

	if (natts != row->nfields)
		return NULL;

	dvalues = (Datum *) palloc0(natts * sizeof(Datum));
	nulls = (char *) palloc(natts * sizeof(char));
	MemSet(nulls, 'n', natts);

	for (i = 0; i < natts; i++)
	{
		PLpgSQL_var *var;

		if (tupdesc->attrs[i]->attisdropped)
			continue;		/* leave the column as null */
		if (row->varnos[i] < 0) /* should not happen */
			elog(ERROR, "dropped rowtype entry for non-dropped column");

		var = (PLpgSQL_var *) (estate->datums[row->varnos[i]]);
		if (var->datatype->typoid != tupdesc->attrs[i]->atttypid)
			return NULL;
		dvalues[i] = var->value;
		if (!var->isnull)
			nulls[i] = ' ';
	}

	tuple = heap_formtuple(tupdesc, dvalues, nulls);

	pfree(dvalues);
	pfree(nulls);

	return tuple;
}

/* ----------
 * convert_value_to_string			Convert a non-null Datum to C string
 *
 * Note: callers generally assume that the result is a palloc'd string and
 * should be pfree'd.  This is not all that safe an assumption ...
 * ----------
 */
static char *
convert_value_to_string(Datum value, Oid valtype)
{
	Oid			typOutput;
	Oid			typElem;
	bool		typIsVarlena;
	FmgrInfo	finfo_output;

	getTypeOutputInfo(valtype, &typOutput, &typElem, &typIsVarlena);

	fmgr_info(typOutput, &finfo_output);

	return DatumGetCString(FunctionCall3(&finfo_output,
										 value,
										 ObjectIdGetDatum(typElem),
										 Int32GetDatum(-1)));
}

/* ----------
 * exec_cast_value			Cast a value if required
 * ----------
 */
static Datum
exec_cast_value(Datum value, Oid valtype,
				Oid reqtype,
				FmgrInfo *reqinput,
				Oid reqtypelem,
				int32 reqtypmod,
				bool *isnull)
{
	if (!*isnull)
	{
		/*
		 * If the type of the queries return value isn't that of the
		 * variable, convert it.
		 */
		if (valtype != reqtype || reqtypmod != -1)
		{
			char	   *extval;

			extval = convert_value_to_string(value, valtype);
			value = FunctionCall3(reqinput,
								  CStringGetDatum(extval),
								  ObjectIdGetDatum(reqtypelem),
								  Int32GetDatum(reqtypmod));
			pfree(extval);
		}
	}

	return value;
}

/* ----------
 * exec_simple_cast_value			Cast a value if required
 *
 * As above, but need not supply details about target type.  Note that this
 * is slower than exec_cast_value with cached type info, and so should be
 * avoided in heavily used code paths.
 * ----------
 */
static Datum
exec_simple_cast_value(Datum value, Oid valtype,
					   Oid reqtype, int32 reqtypmod,
					   bool *isnull)
{
	if (!*isnull)
	{
		if (valtype != reqtype || reqtypmod != -1)
		{
			Oid			typInput;
			Oid			typElem;
			FmgrInfo	finfo_input;

			getTypeInputInfo(reqtype, &typInput, &typElem);

			fmgr_info(typInput, &finfo_input);

			value = exec_cast_value(value,
									valtype,
									reqtype,
									&finfo_input,
									typElem,
									reqtypmod,
									isnull);
		}
	}

	return value;
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

		case T_RelabelType:
			return exec_simple_check_node((Node *) ((RelabelType *) node)->arg);

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

		case T_ArrayExpr:
			{
				ArrayExpr  *expr = (ArrayExpr *) node;

				if (!exec_simple_check_node((Node *) expr->elements))
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

		case T_NullIfExpr:
			{
				NullIfExpr *expr = (NullIfExpr *) node;

				if (expr->opretset)
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

		case T_List:
			{
				List	   *expr = (List *) node;
				List	   *l;

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
exec_simple_check_plan(PLpgSQL_expr * expr)
{
	_SPI_plan  *spi_plan = (_SPI_plan *) expr->plan;
	Plan	   *plan;
	TargetEntry *tle;

	expr->expr_simple_expr = NULL;

	/*
	 * 1. We can only evaluate queries that resulted in one single
	 * execution plan
	 */
	if (length(spi_plan->ptlist) != 1)
		return;

	plan = (Plan *) lfirst(spi_plan->ptlist);

	/*
	 * 2. It must be a RESULT plan --> no scan's required
	 */
	if (plan == NULL)			/* utility statement produces this */
		return;

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
	if (length(plan->targetlist) != 1)
		return;

	tle = (TargetEntry *) lfirst(plan->targetlist);

	/*
	 * 5. Check that all the nodes in the expression are non-scary.
	 */
	if (!exec_simple_check_node((Node *) tle->expr))
		return;

	/*
	 * Yes - this is a simple expression.  Mark it as such, and initialize
	 * state to "not executing".
	 */
	expr->expr_simple_expr = tle->expr;
	expr->expr_simple_state = NULL;
	expr->expr_simple_next = NULL;
	/* Also stash away the expression result type */
	expr->expr_simple_type = exprType((Node *) tle->expr);
}

/*
 * Check two tupledescs have matching number and types of attributes
 */
static bool
compatible_tupdesc(TupleDesc td1, TupleDesc td2)
{
	int			i;

	if (td1->natts != td2->natts)
		return false;

	for (i = 0; i < td1->natts; i++)
	{
		if (td1->attrs[i]->atttypid != td2->attrs[i]->atttypid)
			return false;
	}

	return true;
}

/* ----------
 * exec_set_found			Set the global found variable
 *					to true/false
 * ----------
 */
static void
exec_set_found(PLpgSQL_execstate * estate, bool state)
{
	PLpgSQL_var *var;

	var = (PLpgSQL_var *) (estate->datums[estate->found_varno]);
	var->value = (Datum) state;
	var->isnull = false;
}

/*
 * plpgsql_eoxact --- post-transaction-commit-or-abort cleanup
 *
 * If a simple_eval_estate was created in the current transaction,
 * it has to be cleaned up, and we have to mark all active PLpgSQL_expr
 * structs that are using it as no longer active.
 */
void
plpgsql_eoxact(bool isCommit, void *arg)
{
	PLpgSQL_expr *expr;
	PLpgSQL_expr *enext;

	/* Mark all active exprs as inactive */
	for (expr = active_simple_exprs; expr; expr = enext)
	{
		enext = expr->expr_simple_next;
		expr->expr_simple_state = NULL;
		expr->expr_simple_next = NULL;
	}
	active_simple_exprs = NULL;
	/*
	 * If we are doing a clean transaction shutdown, free the EState
	 * (so that any remaining resources will be released correctly).
	 * In an abort, we expect the regular abort recovery procedures to
	 * release everything of interest.
	 */
	if (isCommit && simple_eval_estate)
		FreeExecutorState(simple_eval_estate);
	simple_eval_estate = NULL;
}
