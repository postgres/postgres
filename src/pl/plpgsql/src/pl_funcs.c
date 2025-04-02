/*-------------------------------------------------------------------------
 *
 * pl_funcs.c		- Misc functions for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_funcs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "plpgsql.h"
#include "utils/memutils.h"

/* ----------
 * Local variables for namespace handling
 *
 * The namespace structure actually forms a tree, of which only one linear
 * list or "chain" (from the youngest item to the root) is accessible from
 * any one plpgsql statement.  During initial parsing of a function, ns_top
 * points to the youngest item accessible from the block currently being
 * parsed.  We store the entire tree, however, since at runtime we will need
 * to access the chain that's relevant to any one statement.
 *
 * Block boundaries in the namespace chain are marked by PLPGSQL_NSTYPE_LABEL
 * items.
 * ----------
 */
static PLpgSQL_nsitem *ns_top = NULL;


/* ----------
 * plpgsql_ns_init			Initialize namespace processing for a new function
 * ----------
 */
void
plpgsql_ns_init(void)
{
	ns_top = NULL;
}


/* ----------
 * plpgsql_ns_push			Create a new namespace level
 * ----------
 */
void
plpgsql_ns_push(const char *label, PLpgSQL_label_type label_type)
{
	if (label == NULL)
		label = "";
	plpgsql_ns_additem(PLPGSQL_NSTYPE_LABEL, (int) label_type, label);
}


/* ----------
 * plpgsql_ns_pop			Pop entries back to (and including) the last label
 * ----------
 */
void
plpgsql_ns_pop(void)
{
	Assert(ns_top != NULL);
	while (ns_top->itemtype != PLPGSQL_NSTYPE_LABEL)
		ns_top = ns_top->prev;
	ns_top = ns_top->prev;
}


/* ----------
 * plpgsql_ns_top			Fetch the current namespace chain end
 * ----------
 */
PLpgSQL_nsitem *
plpgsql_ns_top(void)
{
	return ns_top;
}


/* ----------
 * plpgsql_ns_additem		Add an item to the current namespace chain
 * ----------
 */
void
plpgsql_ns_additem(PLpgSQL_nsitem_type itemtype, int itemno, const char *name)
{
	PLpgSQL_nsitem *nse;

	Assert(name != NULL);
	/* first item added must be a label */
	Assert(ns_top != NULL || itemtype == PLPGSQL_NSTYPE_LABEL);

	nse = palloc(offsetof(PLpgSQL_nsitem, name) + strlen(name) + 1);
	nse->itemtype = itemtype;
	nse->itemno = itemno;
	nse->prev = ns_top;
	strcpy(nse->name, name);
	ns_top = nse;
}


/* ----------
 * plpgsql_ns_lookup		Lookup an identifier in the given namespace chain
 *
 * Note that this only searches for variables, not labels.
 *
 * If localmode is true, only the topmost block level is searched.
 *
 * name1 must be non-NULL.  Pass NULL for name2 and/or name3 if parsing a name
 * with fewer than three components.
 *
 * If names_used isn't NULL, *names_used receives the number of names
 * matched: 0 if no match, 1 if name1 matched an unqualified variable name,
 * 2 if name1 and name2 matched a block label + variable name.
 *
 * Note that name3 is never directly matched to anything.  However, if it
 * isn't NULL, we will disregard qualified matches to scalar variables.
 * Similarly, if name2 isn't NULL, we disregard unqualified matches to
 * scalar variables.
 * ----------
 */
PLpgSQL_nsitem *
plpgsql_ns_lookup(PLpgSQL_nsitem *ns_cur, bool localmode,
				  const char *name1, const char *name2, const char *name3,
				  int *names_used)
{
	/* Outer loop iterates once per block level in the namespace chain */
	while (ns_cur != NULL)
	{
		PLpgSQL_nsitem *nsitem;

		/* Check this level for unqualified match to variable name */
		for (nsitem = ns_cur;
			 nsitem->itemtype != PLPGSQL_NSTYPE_LABEL;
			 nsitem = nsitem->prev)
		{
			if (strcmp(nsitem->name, name1) == 0)
			{
				if (name2 == NULL ||
					nsitem->itemtype != PLPGSQL_NSTYPE_VAR)
				{
					if (names_used)
						*names_used = 1;
					return nsitem;
				}
			}
		}

		/* Check this level for qualified match to variable name */
		if (name2 != NULL &&
			strcmp(nsitem->name, name1) == 0)
		{
			for (nsitem = ns_cur;
				 nsitem->itemtype != PLPGSQL_NSTYPE_LABEL;
				 nsitem = nsitem->prev)
			{
				if (strcmp(nsitem->name, name2) == 0)
				{
					if (name3 == NULL ||
						nsitem->itemtype != PLPGSQL_NSTYPE_VAR)
					{
						if (names_used)
							*names_used = 2;
						return nsitem;
					}
				}
			}
		}

		if (localmode)
			break;				/* do not look into upper levels */

		ns_cur = nsitem->prev;
	}

	/* This is just to suppress possibly-uninitialized-variable warnings */
	if (names_used)
		*names_used = 0;
	return NULL;				/* No match found */
}


/* ----------
 * plpgsql_ns_lookup_label		Lookup a label in the given namespace chain
 * ----------
 */
PLpgSQL_nsitem *
plpgsql_ns_lookup_label(PLpgSQL_nsitem *ns_cur, const char *name)
{
	while (ns_cur != NULL)
	{
		if (ns_cur->itemtype == PLPGSQL_NSTYPE_LABEL &&
			strcmp(ns_cur->name, name) == 0)
			return ns_cur;
		ns_cur = ns_cur->prev;
	}

	return NULL;				/* label not found */
}


/* ----------
 * plpgsql_ns_find_nearest_loop		Find innermost loop label in namespace chain
 * ----------
 */
PLpgSQL_nsitem *
plpgsql_ns_find_nearest_loop(PLpgSQL_nsitem *ns_cur)
{
	while (ns_cur != NULL)
	{
		if (ns_cur->itemtype == PLPGSQL_NSTYPE_LABEL &&
			ns_cur->itemno == PLPGSQL_LABEL_LOOP)
			return ns_cur;
		ns_cur = ns_cur->prev;
	}

	return NULL;				/* no loop found */
}


/*
 * Statement type as a string, for use in error messages etc.
 */
const char *
plpgsql_stmt_typename(PLpgSQL_stmt *stmt)
{
	switch (stmt->cmd_type)
	{
		case PLPGSQL_STMT_BLOCK:
			return _("statement block");
		case PLPGSQL_STMT_ASSIGN:
			return _("assignment");
		case PLPGSQL_STMT_IF:
			return "IF";
		case PLPGSQL_STMT_CASE:
			return "CASE";
		case PLPGSQL_STMT_LOOP:
			return "LOOP";
		case PLPGSQL_STMT_WHILE:
			return "WHILE";
		case PLPGSQL_STMT_FORI:
			return _("FOR with integer loop variable");
		case PLPGSQL_STMT_FORS:
			return _("FOR over SELECT rows");
		case PLPGSQL_STMT_FORC:
			return _("FOR over cursor");
		case PLPGSQL_STMT_FOREACH_A:
			return _("FOREACH over array");
		case PLPGSQL_STMT_EXIT:
			return ((PLpgSQL_stmt_exit *) stmt)->is_exit ? "EXIT" : "CONTINUE";
		case PLPGSQL_STMT_RETURN:
			return "RETURN";
		case PLPGSQL_STMT_RETURN_NEXT:
			return "RETURN NEXT";
		case PLPGSQL_STMT_RETURN_QUERY:
			return "RETURN QUERY";
		case PLPGSQL_STMT_RAISE:
			return "RAISE";
		case PLPGSQL_STMT_ASSERT:
			return "ASSERT";
		case PLPGSQL_STMT_EXECSQL:
			return _("SQL statement");
		case PLPGSQL_STMT_DYNEXECUTE:
			return "EXECUTE";
		case PLPGSQL_STMT_DYNFORS:
			return _("FOR over EXECUTE statement");
		case PLPGSQL_STMT_GETDIAG:
			return ((PLpgSQL_stmt_getdiag *) stmt)->is_stacked ?
				"GET STACKED DIAGNOSTICS" : "GET DIAGNOSTICS";
		case PLPGSQL_STMT_OPEN:
			return "OPEN";
		case PLPGSQL_STMT_FETCH:
			return ((PLpgSQL_stmt_fetch *) stmt)->is_move ? "MOVE" : "FETCH";
		case PLPGSQL_STMT_CLOSE:
			return "CLOSE";
		case PLPGSQL_STMT_PERFORM:
			return "PERFORM";
		case PLPGSQL_STMT_CALL:
			return ((PLpgSQL_stmt_call *) stmt)->is_call ? "CALL" : "DO";
		case PLPGSQL_STMT_COMMIT:
			return "COMMIT";
		case PLPGSQL_STMT_ROLLBACK:
			return "ROLLBACK";
	}

	return "unknown";
}

/*
 * GET DIAGNOSTICS item name as a string, for use in error messages etc.
 */
const char *
plpgsql_getdiag_kindname(PLpgSQL_getdiag_kind kind)
{
	switch (kind)
	{
		case PLPGSQL_GETDIAG_ROW_COUNT:
			return "ROW_COUNT";
		case PLPGSQL_GETDIAG_ROUTINE_OID:
			return "PG_ROUTINE_OID";
		case PLPGSQL_GETDIAG_CONTEXT:
			return "PG_CONTEXT";
		case PLPGSQL_GETDIAG_ERROR_CONTEXT:
			return "PG_EXCEPTION_CONTEXT";
		case PLPGSQL_GETDIAG_ERROR_DETAIL:
			return "PG_EXCEPTION_DETAIL";
		case PLPGSQL_GETDIAG_ERROR_HINT:
			return "PG_EXCEPTION_HINT";
		case PLPGSQL_GETDIAG_RETURNED_SQLSTATE:
			return "RETURNED_SQLSTATE";
		case PLPGSQL_GETDIAG_COLUMN_NAME:
			return "COLUMN_NAME";
		case PLPGSQL_GETDIAG_CONSTRAINT_NAME:
			return "CONSTRAINT_NAME";
		case PLPGSQL_GETDIAG_DATATYPE_NAME:
			return "PG_DATATYPE_NAME";
		case PLPGSQL_GETDIAG_MESSAGE_TEXT:
			return "MESSAGE_TEXT";
		case PLPGSQL_GETDIAG_TABLE_NAME:
			return "TABLE_NAME";
		case PLPGSQL_GETDIAG_SCHEMA_NAME:
			return "SCHEMA_NAME";
	}

	return "unknown";
}


/**********************************************************************
 * Support for recursing through a PL/pgSQL statement tree
 *
 * The point of this code is to encapsulate knowledge of where the
 * sub-statements and expressions are in a statement tree, avoiding
 * duplication of code.  The caller supplies two callbacks, one to
 * be invoked on statements and one to be invoked on expressions.
 * (The recursion should be started by invoking the statement callback
 * on function->action.)  The statement callback should do any
 * statement-type-specific action it needs, then recurse by calling
 * plpgsql_statement_tree_walker().  The expression callback can be a
 * no-op if no per-expression behavior is needed.
 **********************************************************************/
typedef void (*plpgsql_stmt_walker_callback) (PLpgSQL_stmt *stmt,
											  void *context);
typedef void (*plpgsql_expr_walker_callback) (PLpgSQL_expr *expr,
											  void *context);

/*
 * As in nodeFuncs.h, we respectfully decline to support the C standard's
 * position that a pointer to struct is incompatible with "void *".  Instead,
 * silence related compiler warnings using casts in this macro wrapper.
 */
#define plpgsql_statement_tree_walker(s, sw, ew, c) \
	plpgsql_statement_tree_walker_impl(s, (plpgsql_stmt_walker_callback) (sw), \
									   (plpgsql_expr_walker_callback) (ew), c)

static void
plpgsql_statement_tree_walker_impl(PLpgSQL_stmt *stmt,
								   plpgsql_stmt_walker_callback stmt_callback,
								   plpgsql_expr_walker_callback expr_callback,
								   void *context)
{
#define S_WALK(st) stmt_callback(st, context)
#define E_WALK(ex) expr_callback(ex, context)
#define S_LIST_WALK(lst) foreach_ptr(PLpgSQL_stmt, st, lst) S_WALK(st)
#define E_LIST_WALK(lst) foreach_ptr(PLpgSQL_expr, ex, lst) E_WALK(ex)

	switch (stmt->cmd_type)
	{
		case PLPGSQL_STMT_BLOCK:
			{
				PLpgSQL_stmt_block *bstmt = (PLpgSQL_stmt_block *) stmt;

				S_LIST_WALK(bstmt->body);
				if (bstmt->exceptions)
				{
					foreach_ptr(PLpgSQL_exception, exc, bstmt->exceptions->exc_list)
					{
						/* conditions list has no interesting sub-structure */
						S_LIST_WALK(exc->action);
					}
				}
				break;
			}
		case PLPGSQL_STMT_ASSIGN:
			{
				PLpgSQL_stmt_assign *astmt = (PLpgSQL_stmt_assign *) stmt;

				E_WALK(astmt->expr);
				break;
			}
		case PLPGSQL_STMT_IF:
			{
				PLpgSQL_stmt_if *ifstmt = (PLpgSQL_stmt_if *) stmt;

				E_WALK(ifstmt->cond);
				S_LIST_WALK(ifstmt->then_body);
				foreach_ptr(PLpgSQL_if_elsif, elif, ifstmt->elsif_list)
				{
					E_WALK(elif->cond);
					S_LIST_WALK(elif->stmts);
				}
				S_LIST_WALK(ifstmt->else_body);
				break;
			}
		case PLPGSQL_STMT_CASE:
			{
				PLpgSQL_stmt_case *cstmt = (PLpgSQL_stmt_case *) stmt;

				E_WALK(cstmt->t_expr);
				foreach_ptr(PLpgSQL_case_when, cwt, cstmt->case_when_list)
				{
					E_WALK(cwt->expr);
					S_LIST_WALK(cwt->stmts);
				}
				S_LIST_WALK(cstmt->else_stmts);
				break;
			}
		case PLPGSQL_STMT_LOOP:
			{
				PLpgSQL_stmt_loop *lstmt = (PLpgSQL_stmt_loop *) stmt;

				S_LIST_WALK(lstmt->body);
				break;
			}
		case PLPGSQL_STMT_WHILE:
			{
				PLpgSQL_stmt_while *wstmt = (PLpgSQL_stmt_while *) stmt;

				E_WALK(wstmt->cond);
				S_LIST_WALK(wstmt->body);
				break;
			}
		case PLPGSQL_STMT_FORI:
			{
				PLpgSQL_stmt_fori *fori = (PLpgSQL_stmt_fori *) stmt;

				E_WALK(fori->lower);
				E_WALK(fori->upper);
				E_WALK(fori->step);
				S_LIST_WALK(fori->body);
				break;
			}
		case PLPGSQL_STMT_FORS:
			{
				PLpgSQL_stmt_fors *fors = (PLpgSQL_stmt_fors *) stmt;

				S_LIST_WALK(fors->body);
				E_WALK(fors->query);
				break;
			}
		case PLPGSQL_STMT_FORC:
			{
				PLpgSQL_stmt_forc *forc = (PLpgSQL_stmt_forc *) stmt;

				S_LIST_WALK(forc->body);
				E_WALK(forc->argquery);
				break;
			}
		case PLPGSQL_STMT_FOREACH_A:
			{
				PLpgSQL_stmt_foreach_a *fstmt = (PLpgSQL_stmt_foreach_a *) stmt;

				E_WALK(fstmt->expr);
				S_LIST_WALK(fstmt->body);
				break;
			}
		case PLPGSQL_STMT_EXIT:
			{
				PLpgSQL_stmt_exit *estmt = (PLpgSQL_stmt_exit *) stmt;

				E_WALK(estmt->cond);
				break;
			}
		case PLPGSQL_STMT_RETURN:
			{
				PLpgSQL_stmt_return *rstmt = (PLpgSQL_stmt_return *) stmt;

				E_WALK(rstmt->expr);
				break;
			}
		case PLPGSQL_STMT_RETURN_NEXT:
			{
				PLpgSQL_stmt_return_next *rstmt = (PLpgSQL_stmt_return_next *) stmt;

				E_WALK(rstmt->expr);
				break;
			}
		case PLPGSQL_STMT_RETURN_QUERY:
			{
				PLpgSQL_stmt_return_query *rstmt = (PLpgSQL_stmt_return_query *) stmt;

				E_WALK(rstmt->query);
				E_WALK(rstmt->dynquery);
				E_LIST_WALK(rstmt->params);
				break;
			}
		case PLPGSQL_STMT_RAISE:
			{
				PLpgSQL_stmt_raise *rstmt = (PLpgSQL_stmt_raise *) stmt;

				E_LIST_WALK(rstmt->params);
				foreach_ptr(PLpgSQL_raise_option, opt, rstmt->options)
				{
					E_WALK(opt->expr);
				}
				break;
			}
		case PLPGSQL_STMT_ASSERT:
			{
				PLpgSQL_stmt_assert *astmt = (PLpgSQL_stmt_assert *) stmt;

				E_WALK(astmt->cond);
				E_WALK(astmt->message);
				break;
			}
		case PLPGSQL_STMT_EXECSQL:
			{
				PLpgSQL_stmt_execsql *xstmt = (PLpgSQL_stmt_execsql *) stmt;

				E_WALK(xstmt->sqlstmt);
				break;
			}
		case PLPGSQL_STMT_DYNEXECUTE:
			{
				PLpgSQL_stmt_dynexecute *dstmt = (PLpgSQL_stmt_dynexecute *) stmt;

				E_WALK(dstmt->query);
				E_LIST_WALK(dstmt->params);
				break;
			}
		case PLPGSQL_STMT_DYNFORS:
			{
				PLpgSQL_stmt_dynfors *dstmt = (PLpgSQL_stmt_dynfors *) stmt;

				S_LIST_WALK(dstmt->body);
				E_WALK(dstmt->query);
				E_LIST_WALK(dstmt->params);
				break;
			}
		case PLPGSQL_STMT_GETDIAG:
			{
				/* no interesting sub-structure */
				break;
			}
		case PLPGSQL_STMT_OPEN:
			{
				PLpgSQL_stmt_open *ostmt = (PLpgSQL_stmt_open *) stmt;

				E_WALK(ostmt->argquery);
				E_WALK(ostmt->query);
				E_WALK(ostmt->dynquery);
				E_LIST_WALK(ostmt->params);
				break;
			}
		case PLPGSQL_STMT_FETCH:
			{
				PLpgSQL_stmt_fetch *fstmt = (PLpgSQL_stmt_fetch *) stmt;

				E_WALK(fstmt->expr);
				break;
			}
		case PLPGSQL_STMT_CLOSE:
			{
				/* no interesting sub-structure */
				break;
			}
		case PLPGSQL_STMT_PERFORM:
			{
				PLpgSQL_stmt_perform *pstmt = (PLpgSQL_stmt_perform *) stmt;

				E_WALK(pstmt->expr);
				break;
			}
		case PLPGSQL_STMT_CALL:
			{
				PLpgSQL_stmt_call *cstmt = (PLpgSQL_stmt_call *) stmt;

				E_WALK(cstmt->expr);
				break;
			}
		case PLPGSQL_STMT_COMMIT:
		case PLPGSQL_STMT_ROLLBACK:
			{
				/* no interesting sub-structure */
				break;
			}
		default:
			elog(ERROR, "unrecognized cmd_type: %d", stmt->cmd_type);
			break;
	}
}


/**********************************************************************
 * Mark assignment source expressions that have local target variables,
 * that is, the target variable is declared within the exception block
 * most closely containing the assignment itself.  (Such target variables
 * need not be preserved if the assignment's source expression raises an
 * error, since the variable will no longer be accessible afterwards.
 * Detecting this allows better optimization.)
 *
 * This code need not be called if the plpgsql function contains no exception
 * blocks, because mark_expr_as_assignment_source will have set all the flags
 * to true already.  Also, we need not reconsider default-value expressions
 * for variables, because variable declarations are necessarily within the
 * nearest exception block.  (In DECLARE ... BEGIN ... EXCEPTION ... END, the
 * variable initializations are done before entering the exception scope.)
 *
 * Within the recursion, local_dnos is a Bitmapset of dnos of variables
 * known to be declared within the current exception level.
 **********************************************************************/
static void mark_stmt(PLpgSQL_stmt *stmt, Bitmapset *local_dnos);
static void mark_expr(PLpgSQL_expr *expr, Bitmapset *local_dnos);

static void
mark_stmt(PLpgSQL_stmt *stmt, Bitmapset *local_dnos)
{
	if (stmt == NULL)
		return;
	if (stmt->cmd_type == PLPGSQL_STMT_BLOCK)
	{
		PLpgSQL_stmt_block *block = (PLpgSQL_stmt_block *) stmt;

		if (block->exceptions)
		{
			/*
			 * The block creates a new exception scope, so variables declared
			 * at outer levels are nonlocal.  For that matter, so are any
			 * variables declared in the block's DECLARE section.  Hence, we
			 * must pass down empty local_dnos.
			 */
			plpgsql_statement_tree_walker(stmt, mark_stmt, mark_expr, NULL);
		}
		else
		{
			/*
			 * Otherwise, the block does not create a new exception scope, and
			 * any variables it declares can also be considered local within
			 * it.  Note that only initializable datum types (VAR, REC) are
			 * included in initvarnos; but that's sufficient for our purposes.
			 */
			local_dnos = bms_copy(local_dnos);
			for (int i = 0; i < block->n_initvars; i++)
				local_dnos = bms_add_member(local_dnos, block->initvarnos[i]);
			plpgsql_statement_tree_walker(stmt, mark_stmt, mark_expr,
										  local_dnos);
			bms_free(local_dnos);
		}
	}
	else
		plpgsql_statement_tree_walker(stmt, mark_stmt, mark_expr, local_dnos);
}

static void
mark_expr(PLpgSQL_expr *expr, Bitmapset *local_dnos)
{
	/*
	 * If this expression has an assignment target, check whether the target
	 * is local, and mark the expression accordingly.
	 */
	if (expr && expr->target_param >= 0)
		expr->target_is_local = bms_is_member(expr->target_param, local_dnos);
}

void
plpgsql_mark_local_assignment_targets(PLpgSQL_function *func)
{
	Bitmapset  *local_dnos;

	/* Function parameters can be treated as local targets at outer level */
	local_dnos = NULL;
	for (int i = 0; i < func->fn_nargs; i++)
		local_dnos = bms_add_member(local_dnos, func->fn_argvarnos[i]);
	mark_stmt((PLpgSQL_stmt *) func->action, local_dnos);
	bms_free(local_dnos);
}


/**********************************************************************
 * Release memory when a PL/pgSQL function is no longer needed
 *
 * This code only needs to deal with cleaning up PLpgSQL_expr nodes,
 * which may contain references to saved SPI Plans that must be freed.
 * The function tree itself, along with subsidiary data, is freed in
 * one swoop by freeing the function's permanent memory context.
 **********************************************************************/
static void free_stmt(PLpgSQL_stmt *stmt, void *context);
static void free_expr(PLpgSQL_expr *expr, void *context);

static void
free_stmt(PLpgSQL_stmt *stmt, void *context)
{
	if (stmt == NULL)
		return;
	plpgsql_statement_tree_walker(stmt, free_stmt, free_expr, NULL);
}

static void
free_expr(PLpgSQL_expr *expr, void *context)
{
	if (expr && expr->plan)
	{
		SPI_freeplan(expr->plan);
		expr->plan = NULL;
	}
}

void
plpgsql_free_function_memory(PLpgSQL_function *func)
{
	int			i;

	/* Better not call this on an in-use function */
	Assert(func->cfunc.use_count == 0);

	/* Release plans associated with variable declarations */
	for (i = 0; i < func->ndatums; i++)
	{
		PLpgSQL_datum *d = func->datums[i];

		switch (d->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) d;

					free_expr(var->default_val, NULL);
					free_expr(var->cursor_explicit_expr, NULL);
				}
				break;
			case PLPGSQL_DTYPE_ROW:
				break;
			case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec *rec = (PLpgSQL_rec *) d;

					free_expr(rec->default_val, NULL);
				}
				break;
			case PLPGSQL_DTYPE_RECFIELD:
				break;
			default:
				elog(ERROR, "unrecognized data type: %d", d->dtype);
		}
	}
	func->ndatums = 0;

	/* Release plans in statement tree */
	free_stmt((PLpgSQL_stmt *) func->action, NULL);
	func->action = NULL;

	/*
	 * And finally, release all memory except the PLpgSQL_function struct
	 * itself (which has to be kept around because there may be multiple
	 * fn_extra pointers to it).
	 */
	if (func->fn_cxt)
		MemoryContextDelete(func->fn_cxt);
	func->fn_cxt = NULL;
}

/* Deletion callback used by funccache.c */
void
plpgsql_delete_callback(CachedFunction *cfunc)
{
	plpgsql_free_function_memory((PLpgSQL_function *) cfunc);
}


/**********************************************************************
 * Debug functions for analyzing the compiled code
 *
 * Sadly, there doesn't seem to be any way to let plpgsql_statement_tree_walker
 * bear some of the burden for this.
 **********************************************************************/
static int	dump_indent;

static void dump_ind(void);
static void dump_stmt(PLpgSQL_stmt *stmt);
static void dump_block(PLpgSQL_stmt_block *block);
static void dump_assign(PLpgSQL_stmt_assign *stmt);
static void dump_if(PLpgSQL_stmt_if *stmt);
static void dump_case(PLpgSQL_stmt_case *stmt);
static void dump_loop(PLpgSQL_stmt_loop *stmt);
static void dump_while(PLpgSQL_stmt_while *stmt);
static void dump_fori(PLpgSQL_stmt_fori *stmt);
static void dump_fors(PLpgSQL_stmt_fors *stmt);
static void dump_forc(PLpgSQL_stmt_forc *stmt);
static void dump_foreach_a(PLpgSQL_stmt_foreach_a *stmt);
static void dump_exit(PLpgSQL_stmt_exit *stmt);
static void dump_return(PLpgSQL_stmt_return *stmt);
static void dump_return_next(PLpgSQL_stmt_return_next *stmt);
static void dump_return_query(PLpgSQL_stmt_return_query *stmt);
static void dump_raise(PLpgSQL_stmt_raise *stmt);
static void dump_assert(PLpgSQL_stmt_assert *stmt);
static void dump_execsql(PLpgSQL_stmt_execsql *stmt);
static void dump_dynexecute(PLpgSQL_stmt_dynexecute *stmt);
static void dump_dynfors(PLpgSQL_stmt_dynfors *stmt);
static void dump_getdiag(PLpgSQL_stmt_getdiag *stmt);
static void dump_open(PLpgSQL_stmt_open *stmt);
static void dump_fetch(PLpgSQL_stmt_fetch *stmt);
static void dump_cursor_direction(PLpgSQL_stmt_fetch *stmt);
static void dump_close(PLpgSQL_stmt_close *stmt);
static void dump_perform(PLpgSQL_stmt_perform *stmt);
static void dump_call(PLpgSQL_stmt_call *stmt);
static void dump_commit(PLpgSQL_stmt_commit *stmt);
static void dump_rollback(PLpgSQL_stmt_rollback *stmt);
static void dump_expr(PLpgSQL_expr *expr);


static void
dump_ind(void)
{
	int			i;

	for (i = 0; i < dump_indent; i++)
		printf(" ");
}

static void
dump_stmt(PLpgSQL_stmt *stmt)
{
	printf("%3d:", stmt->lineno);
	switch (stmt->cmd_type)
	{
		case PLPGSQL_STMT_BLOCK:
			dump_block((PLpgSQL_stmt_block *) stmt);
			break;
		case PLPGSQL_STMT_ASSIGN:
			dump_assign((PLpgSQL_stmt_assign *) stmt);
			break;
		case PLPGSQL_STMT_IF:
			dump_if((PLpgSQL_stmt_if *) stmt);
			break;
		case PLPGSQL_STMT_CASE:
			dump_case((PLpgSQL_stmt_case *) stmt);
			break;
		case PLPGSQL_STMT_LOOP:
			dump_loop((PLpgSQL_stmt_loop *) stmt);
			break;
		case PLPGSQL_STMT_WHILE:
			dump_while((PLpgSQL_stmt_while *) stmt);
			break;
		case PLPGSQL_STMT_FORI:
			dump_fori((PLpgSQL_stmt_fori *) stmt);
			break;
		case PLPGSQL_STMT_FORS:
			dump_fors((PLpgSQL_stmt_fors *) stmt);
			break;
		case PLPGSQL_STMT_FORC:
			dump_forc((PLpgSQL_stmt_forc *) stmt);
			break;
		case PLPGSQL_STMT_FOREACH_A:
			dump_foreach_a((PLpgSQL_stmt_foreach_a *) stmt);
			break;
		case PLPGSQL_STMT_EXIT:
			dump_exit((PLpgSQL_stmt_exit *) stmt);
			break;
		case PLPGSQL_STMT_RETURN:
			dump_return((PLpgSQL_stmt_return *) stmt);
			break;
		case PLPGSQL_STMT_RETURN_NEXT:
			dump_return_next((PLpgSQL_stmt_return_next *) stmt);
			break;
		case PLPGSQL_STMT_RETURN_QUERY:
			dump_return_query((PLpgSQL_stmt_return_query *) stmt);
			break;
		case PLPGSQL_STMT_RAISE:
			dump_raise((PLpgSQL_stmt_raise *) stmt);
			break;
		case PLPGSQL_STMT_ASSERT:
			dump_assert((PLpgSQL_stmt_assert *) stmt);
			break;
		case PLPGSQL_STMT_EXECSQL:
			dump_execsql((PLpgSQL_stmt_execsql *) stmt);
			break;
		case PLPGSQL_STMT_DYNEXECUTE:
			dump_dynexecute((PLpgSQL_stmt_dynexecute *) stmt);
			break;
		case PLPGSQL_STMT_DYNFORS:
			dump_dynfors((PLpgSQL_stmt_dynfors *) stmt);
			break;
		case PLPGSQL_STMT_GETDIAG:
			dump_getdiag((PLpgSQL_stmt_getdiag *) stmt);
			break;
		case PLPGSQL_STMT_OPEN:
			dump_open((PLpgSQL_stmt_open *) stmt);
			break;
		case PLPGSQL_STMT_FETCH:
			dump_fetch((PLpgSQL_stmt_fetch *) stmt);
			break;
		case PLPGSQL_STMT_CLOSE:
			dump_close((PLpgSQL_stmt_close *) stmt);
			break;
		case PLPGSQL_STMT_PERFORM:
			dump_perform((PLpgSQL_stmt_perform *) stmt);
			break;
		case PLPGSQL_STMT_CALL:
			dump_call((PLpgSQL_stmt_call *) stmt);
			break;
		case PLPGSQL_STMT_COMMIT:
			dump_commit((PLpgSQL_stmt_commit *) stmt);
			break;
		case PLPGSQL_STMT_ROLLBACK:
			dump_rollback((PLpgSQL_stmt_rollback *) stmt);
			break;
		default:
			elog(ERROR, "unrecognized cmd_type: %d", stmt->cmd_type);
			break;
	}
}

static void
dump_stmts(List *stmts)
{
	ListCell   *s;

	dump_indent += 2;
	foreach(s, stmts)
		dump_stmt((PLpgSQL_stmt *) lfirst(s));
	dump_indent -= 2;
}

static void
dump_block(PLpgSQL_stmt_block *block)
{
	char	   *name;

	if (block->label == NULL)
		name = "*unnamed*";
	else
		name = block->label;

	dump_ind();
	printf("BLOCK <<%s>>\n", name);

	dump_stmts(block->body);

	if (block->exceptions)
	{
		ListCell   *e;

		foreach(e, block->exceptions->exc_list)
		{
			PLpgSQL_exception *exc = (PLpgSQL_exception *) lfirst(e);
			PLpgSQL_condition *cond;

			dump_ind();
			printf("    EXCEPTION WHEN ");
			for (cond = exc->conditions; cond; cond = cond->next)
			{
				if (cond != exc->conditions)
					printf(" OR ");
				printf("%s", cond->condname);
			}
			printf(" THEN\n");
			dump_stmts(exc->action);
		}
	}

	dump_ind();
	printf("    END -- %s\n", name);
}

static void
dump_assign(PLpgSQL_stmt_assign *stmt)
{
	dump_ind();
	printf("ASSIGN var %d := ", stmt->varno);
	dump_expr(stmt->expr);
	printf("\n");
}

static void
dump_if(PLpgSQL_stmt_if *stmt)
{
	ListCell   *l;

	dump_ind();
	printf("IF ");
	dump_expr(stmt->cond);
	printf(" THEN\n");
	dump_stmts(stmt->then_body);
	foreach(l, stmt->elsif_list)
	{
		PLpgSQL_if_elsif *elif = (PLpgSQL_if_elsif *) lfirst(l);

		dump_ind();
		printf("    ELSIF ");
		dump_expr(elif->cond);
		printf(" THEN\n");
		dump_stmts(elif->stmts);
	}
	if (stmt->else_body != NIL)
	{
		dump_ind();
		printf("    ELSE\n");
		dump_stmts(stmt->else_body);
	}
	dump_ind();
	printf("    ENDIF\n");
}

static void
dump_case(PLpgSQL_stmt_case *stmt)
{
	ListCell   *l;

	dump_ind();
	printf("CASE %d ", stmt->t_varno);
	if (stmt->t_expr)
		dump_expr(stmt->t_expr);
	printf("\n");
	dump_indent += 6;
	foreach(l, stmt->case_when_list)
	{
		PLpgSQL_case_when *cwt = (PLpgSQL_case_when *) lfirst(l);

		dump_ind();
		printf("WHEN ");
		dump_expr(cwt->expr);
		printf("\n");
		dump_ind();
		printf("THEN\n");
		dump_indent += 2;
		dump_stmts(cwt->stmts);
		dump_indent -= 2;
	}
	if (stmt->have_else)
	{
		dump_ind();
		printf("ELSE\n");
		dump_indent += 2;
		dump_stmts(stmt->else_stmts);
		dump_indent -= 2;
	}
	dump_indent -= 6;
	dump_ind();
	printf("    ENDCASE\n");
}

static void
dump_loop(PLpgSQL_stmt_loop *stmt)
{
	dump_ind();
	printf("LOOP\n");

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDLOOP\n");
}

static void
dump_while(PLpgSQL_stmt_while *stmt)
{
	dump_ind();
	printf("WHILE ");
	dump_expr(stmt->cond);
	printf("\n");

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDWHILE\n");
}

static void
dump_fori(PLpgSQL_stmt_fori *stmt)
{
	dump_ind();
	printf("FORI %s %s\n", stmt->var->refname, (stmt->reverse) ? "REVERSE" : "NORMAL");

	dump_indent += 2;
	dump_ind();
	printf("    lower = ");
	dump_expr(stmt->lower);
	printf("\n");
	dump_ind();
	printf("    upper = ");
	dump_expr(stmt->upper);
	printf("\n");
	if (stmt->step)
	{
		dump_ind();
		printf("    step = ");
		dump_expr(stmt->step);
		printf("\n");
	}
	dump_indent -= 2;

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDFORI\n");
}

static void
dump_fors(PLpgSQL_stmt_fors *stmt)
{
	dump_ind();
	printf("FORS %s ", stmt->var->refname);
	dump_expr(stmt->query);
	printf("\n");

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDFORS\n");
}

static void
dump_forc(PLpgSQL_stmt_forc *stmt)
{
	dump_ind();
	printf("FORC %s ", stmt->var->refname);
	printf("curvar=%d\n", stmt->curvar);

	dump_indent += 2;
	if (stmt->argquery != NULL)
	{
		dump_ind();
		printf("  arguments = ");
		dump_expr(stmt->argquery);
		printf("\n");
	}
	dump_indent -= 2;

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDFORC\n");
}

static void
dump_foreach_a(PLpgSQL_stmt_foreach_a *stmt)
{
	dump_ind();
	printf("FOREACHA var %d ", stmt->varno);
	if (stmt->slice != 0)
		printf("SLICE %d ", stmt->slice);
	printf("IN ");
	dump_expr(stmt->expr);
	printf("\n");

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDFOREACHA");
}

static void
dump_open(PLpgSQL_stmt_open *stmt)
{
	dump_ind();
	printf("OPEN curvar=%d\n", stmt->curvar);

	dump_indent += 2;
	if (stmt->argquery != NULL)
	{
		dump_ind();
		printf("  arguments = '");
		dump_expr(stmt->argquery);
		printf("'\n");
	}
	if (stmt->query != NULL)
	{
		dump_ind();
		printf("  query = '");
		dump_expr(stmt->query);
		printf("'\n");
	}
	if (stmt->dynquery != NULL)
	{
		dump_ind();
		printf("  execute = '");
		dump_expr(stmt->dynquery);
		printf("'\n");

		if (stmt->params != NIL)
		{
			ListCell   *lc;
			int			i;

			dump_indent += 2;
			dump_ind();
			printf("    USING\n");
			dump_indent += 2;
			i = 1;
			foreach(lc, stmt->params)
			{
				dump_ind();
				printf("    parameter $%d: ", i++);
				dump_expr((PLpgSQL_expr *) lfirst(lc));
				printf("\n");
			}
			dump_indent -= 4;
		}
	}
	dump_indent -= 2;
}

static void
dump_fetch(PLpgSQL_stmt_fetch *stmt)
{
	dump_ind();

	if (!stmt->is_move)
	{
		printf("FETCH curvar=%d\n", stmt->curvar);
		dump_cursor_direction(stmt);

		dump_indent += 2;
		if (stmt->target != NULL)
		{
			dump_ind();
			printf("    target = %d %s\n",
				   stmt->target->dno, stmt->target->refname);
		}
		dump_indent -= 2;
	}
	else
	{
		printf("MOVE curvar=%d\n", stmt->curvar);
		dump_cursor_direction(stmt);
	}
}

static void
dump_cursor_direction(PLpgSQL_stmt_fetch *stmt)
{
	dump_indent += 2;
	dump_ind();
	switch (stmt->direction)
	{
		case FETCH_FORWARD:
			printf("    FORWARD ");
			break;
		case FETCH_BACKWARD:
			printf("    BACKWARD ");
			break;
		case FETCH_ABSOLUTE:
			printf("    ABSOLUTE ");
			break;
		case FETCH_RELATIVE:
			printf("    RELATIVE ");
			break;
		default:
			printf("??? unknown cursor direction %d", stmt->direction);
	}

	if (stmt->expr)
	{
		dump_expr(stmt->expr);
		printf("\n");
	}
	else
		printf("%ld\n", stmt->how_many);

	dump_indent -= 2;
}

static void
dump_close(PLpgSQL_stmt_close *stmt)
{
	dump_ind();
	printf("CLOSE curvar=%d\n", stmt->curvar);
}

static void
dump_perform(PLpgSQL_stmt_perform *stmt)
{
	dump_ind();
	printf("PERFORM expr = ");
	dump_expr(stmt->expr);
	printf("\n");
}

static void
dump_call(PLpgSQL_stmt_call *stmt)
{
	dump_ind();
	printf("%s expr = ", stmt->is_call ? "CALL" : "DO");
	dump_expr(stmt->expr);
	printf("\n");
}

static void
dump_commit(PLpgSQL_stmt_commit *stmt)
{
	dump_ind();
	if (stmt->chain)
		printf("COMMIT AND CHAIN\n");
	else
		printf("COMMIT\n");
}

static void
dump_rollback(PLpgSQL_stmt_rollback *stmt)
{
	dump_ind();
	if (stmt->chain)
		printf("ROLLBACK AND CHAIN\n");
	else
		printf("ROLLBACK\n");
}

static void
dump_exit(PLpgSQL_stmt_exit *stmt)
{
	dump_ind();
	printf("%s", stmt->is_exit ? "EXIT" : "CONTINUE");
	if (stmt->label != NULL)
		printf(" label='%s'", stmt->label);
	if (stmt->cond != NULL)
	{
		printf(" WHEN ");
		dump_expr(stmt->cond);
	}
	printf("\n");
}

static void
dump_return(PLpgSQL_stmt_return *stmt)
{
	dump_ind();
	printf("RETURN ");
	if (stmt->retvarno >= 0)
		printf("variable %d", stmt->retvarno);
	else if (stmt->expr != NULL)
		dump_expr(stmt->expr);
	else
		printf("NULL");
	printf("\n");
}

static void
dump_return_next(PLpgSQL_stmt_return_next *stmt)
{
	dump_ind();
	printf("RETURN NEXT ");
	if (stmt->retvarno >= 0)
		printf("variable %d", stmt->retvarno);
	else if (stmt->expr != NULL)
		dump_expr(stmt->expr);
	else
		printf("NULL");
	printf("\n");
}

static void
dump_return_query(PLpgSQL_stmt_return_query *stmt)
{
	dump_ind();
	if (stmt->query)
	{
		printf("RETURN QUERY ");
		dump_expr(stmt->query);
		printf("\n");
	}
	else
	{
		printf("RETURN QUERY EXECUTE ");
		dump_expr(stmt->dynquery);
		printf("\n");
		if (stmt->params != NIL)
		{
			ListCell   *lc;
			int			i;

			dump_indent += 2;
			dump_ind();
			printf("    USING\n");
			dump_indent += 2;
			i = 1;
			foreach(lc, stmt->params)
			{
				dump_ind();
				printf("    parameter $%d: ", i++);
				dump_expr((PLpgSQL_expr *) lfirst(lc));
				printf("\n");
			}
			dump_indent -= 4;
		}
	}
}

static void
dump_raise(PLpgSQL_stmt_raise *stmt)
{
	ListCell   *lc;
	int			i = 0;

	dump_ind();
	printf("RAISE level=%d", stmt->elog_level);
	if (stmt->condname)
		printf(" condname='%s'", stmt->condname);
	if (stmt->message)
		printf(" message='%s'", stmt->message);
	printf("\n");
	dump_indent += 2;
	foreach(lc, stmt->params)
	{
		dump_ind();
		printf("    parameter %d: ", i++);
		dump_expr((PLpgSQL_expr *) lfirst(lc));
		printf("\n");
	}
	if (stmt->options)
	{
		dump_ind();
		printf("    USING\n");
		dump_indent += 2;
		foreach(lc, stmt->options)
		{
			PLpgSQL_raise_option *opt = (PLpgSQL_raise_option *) lfirst(lc);

			dump_ind();
			switch (opt->opt_type)
			{
				case PLPGSQL_RAISEOPTION_ERRCODE:
					printf("    ERRCODE = ");
					break;
				case PLPGSQL_RAISEOPTION_MESSAGE:
					printf("    MESSAGE = ");
					break;
				case PLPGSQL_RAISEOPTION_DETAIL:
					printf("    DETAIL = ");
					break;
				case PLPGSQL_RAISEOPTION_HINT:
					printf("    HINT = ");
					break;
				case PLPGSQL_RAISEOPTION_COLUMN:
					printf("    COLUMN = ");
					break;
				case PLPGSQL_RAISEOPTION_CONSTRAINT:
					printf("    CONSTRAINT = ");
					break;
				case PLPGSQL_RAISEOPTION_DATATYPE:
					printf("    DATATYPE = ");
					break;
				case PLPGSQL_RAISEOPTION_TABLE:
					printf("    TABLE = ");
					break;
				case PLPGSQL_RAISEOPTION_SCHEMA:
					printf("    SCHEMA = ");
					break;
			}
			dump_expr(opt->expr);
			printf("\n");
		}
		dump_indent -= 2;
	}
	dump_indent -= 2;
}

static void
dump_assert(PLpgSQL_stmt_assert *stmt)
{
	dump_ind();
	printf("ASSERT ");
	dump_expr(stmt->cond);
	printf("\n");

	dump_indent += 2;
	if (stmt->message != NULL)
	{
		dump_ind();
		printf("    MESSAGE = ");
		dump_expr(stmt->message);
		printf("\n");
	}
	dump_indent -= 2;
}

static void
dump_execsql(PLpgSQL_stmt_execsql *stmt)
{
	dump_ind();
	printf("EXECSQL ");
	dump_expr(stmt->sqlstmt);
	printf("\n");

	dump_indent += 2;
	if (stmt->target != NULL)
	{
		dump_ind();
		printf("    INTO%s target = %d %s\n",
			   stmt->strict ? " STRICT" : "",
			   stmt->target->dno, stmt->target->refname);
	}
	dump_indent -= 2;
}

static void
dump_dynexecute(PLpgSQL_stmt_dynexecute *stmt)
{
	dump_ind();
	printf("EXECUTE ");
	dump_expr(stmt->query);
	printf("\n");

	dump_indent += 2;
	if (stmt->target != NULL)
	{
		dump_ind();
		printf("    INTO%s target = %d %s\n",
			   stmt->strict ? " STRICT" : "",
			   stmt->target->dno, stmt->target->refname);
	}
	if (stmt->params != NIL)
	{
		ListCell   *lc;
		int			i;

		dump_ind();
		printf("    USING\n");
		dump_indent += 2;
		i = 1;
		foreach(lc, stmt->params)
		{
			dump_ind();
			printf("    parameter %d: ", i++);
			dump_expr((PLpgSQL_expr *) lfirst(lc));
			printf("\n");
		}
		dump_indent -= 2;
	}
	dump_indent -= 2;
}

static void
dump_dynfors(PLpgSQL_stmt_dynfors *stmt)
{
	dump_ind();
	printf("FORS %s EXECUTE ", stmt->var->refname);
	dump_expr(stmt->query);
	printf("\n");
	if (stmt->params != NIL)
	{
		ListCell   *lc;
		int			i;

		dump_indent += 2;
		dump_ind();
		printf("    USING\n");
		dump_indent += 2;
		i = 1;
		foreach(lc, stmt->params)
		{
			dump_ind();
			printf("    parameter $%d: ", i++);
			dump_expr((PLpgSQL_expr *) lfirst(lc));
			printf("\n");
		}
		dump_indent -= 4;
	}
	dump_stmts(stmt->body);
	dump_ind();
	printf("    ENDFORS\n");
}

static void
dump_getdiag(PLpgSQL_stmt_getdiag *stmt)
{
	ListCell   *lc;

	dump_ind();
	printf("GET %s DIAGNOSTICS ", stmt->is_stacked ? "STACKED" : "CURRENT");
	foreach(lc, stmt->diag_items)
	{
		PLpgSQL_diag_item *diag_item = (PLpgSQL_diag_item *) lfirst(lc);

		if (lc != list_head(stmt->diag_items))
			printf(", ");

		printf("{var %d} = %s", diag_item->target,
			   plpgsql_getdiag_kindname(diag_item->kind));
	}
	printf("\n");
}

static void
dump_expr(PLpgSQL_expr *expr)
{
	printf("'%s'", expr->query);
	if (expr->target_param >= 0)
		printf(" target %d%s", expr->target_param,
			   expr->target_is_local ? " (local)" : "");
}

void
plpgsql_dumptree(PLpgSQL_function *func)
{
	int			i;
	PLpgSQL_datum *d;

	printf("\nExecution tree of successfully compiled PL/pgSQL function %s:\n",
		   func->fn_signature);

	printf("\nFunction's data area:\n");
	for (i = 0; i < func->ndatums; i++)
	{
		d = func->datums[i];

		printf("    entry %d: ", i);
		switch (d->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_PROMISE:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) d;

					printf("VAR %-16s type %s (typoid %u) atttypmod %d\n",
						   var->refname, var->datatype->typname,
						   var->datatype->typoid,
						   var->datatype->atttypmod);
					if (var->isconst)
						printf("                                  CONSTANT\n");
					if (var->notnull)
						printf("                                  NOT NULL\n");
					if (var->default_val != NULL)
					{
						printf("                                  DEFAULT ");
						dump_expr(var->default_val);
						printf("\n");
					}
					if (var->cursor_explicit_expr != NULL)
					{
						if (var->cursor_explicit_argrow >= 0)
							printf("                                  CURSOR argument row %d\n", var->cursor_explicit_argrow);

						printf("                                  CURSOR IS ");
						dump_expr(var->cursor_explicit_expr);
						printf("\n");
					}
					if (var->promise != PLPGSQL_PROMISE_NONE)
						printf("                                  PROMISE %d\n",
							   (int) var->promise);
				}
				break;
			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) d;

					printf("ROW %-16s fields", row->refname);
					for (int j = 0; j < row->nfields; j++)
					{
						printf(" %s=var %d", row->fieldnames[j],
							   row->varnos[j]);
					}
					printf("\n");
				}
				break;
			case PLPGSQL_DTYPE_REC:
				printf("REC %-16s typoid %u\n",
					   ((PLpgSQL_rec *) d)->refname,
					   ((PLpgSQL_rec *) d)->rectypeid);
				if (((PLpgSQL_rec *) d)->isconst)
					printf("                                  CONSTANT\n");
				if (((PLpgSQL_rec *) d)->notnull)
					printf("                                  NOT NULL\n");
				if (((PLpgSQL_rec *) d)->default_val != NULL)
				{
					printf("                                  DEFAULT ");
					dump_expr(((PLpgSQL_rec *) d)->default_val);
					printf("\n");
				}
				break;
			case PLPGSQL_DTYPE_RECFIELD:
				printf("RECFIELD %-16s of REC %d\n",
					   ((PLpgSQL_recfield *) d)->fieldname,
					   ((PLpgSQL_recfield *) d)->recparentno);
				break;
			default:
				printf("??? unknown data type %d\n", d->dtype);
		}
	}
	printf("\nFunction's statements:\n");

	dump_indent = 0;
	printf("%3d:", func->action->lineno);
	dump_block(func->action);
	printf("\nEnd of execution tree of function %s\n\n", func->fn_signature);
	fflush(stdout);
}
