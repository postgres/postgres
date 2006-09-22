/*-------------------------------------------------------------------------
 *
 * pl_funcs.c		- Misc functions for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/pl/plpgsql/src/pl_funcs.c,v 1.55 2006/09/22 21:39:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"
#include "pl.tab.h"

#include <ctype.h>

#include "parser/scansup.h"


/* ----------
 * Local variables for the namestack handling
 * ----------
 */
static PLpgSQL_ns *ns_current = NULL;
static bool ns_localmode = false;


/* ----------
 * plpgsql_dstring_init			Dynamic string initialization
 * ----------
 */
void
plpgsql_dstring_init(PLpgSQL_dstring *ds)
{
	ds->value = palloc(ds->alloc = 512);
	ds->used = 1;
	ds->value[0] = '\0';
}


/* ----------
 * plpgsql_dstring_free			Dynamic string destruction
 * ----------
 */
void
plpgsql_dstring_free(PLpgSQL_dstring *ds)
{
	pfree(ds->value);
}

static void
plpgsql_dstring_expand(PLpgSQL_dstring *ds, int needed)
{
	/* Don't allow truncating the string */
	Assert(needed > ds->alloc);
	Assert(ds->used <= ds->alloc);

	/* Might have to double more than once, if needed is large */
	do
	{
		ds->alloc *= 2;
	} while (needed > ds->alloc);
	ds->value = repalloc(ds->value, ds->alloc);
}

/* ----------
 * plpgsql_dstring_append		Dynamic string extending
 * ----------
 */
void
plpgsql_dstring_append(PLpgSQL_dstring *ds, const char *str)
{
	int			len = strlen(str);
	int			needed = ds->used + len;

	if (needed > ds->alloc)
		plpgsql_dstring_expand(ds, needed);

	memcpy(&(ds->value[ds->used - 1]), str, len);
	ds->used += len;
	ds->value[ds->used - 1] = '\0';
}

/* ----------
 * plpgsql_dstring_append_char	Append a single character
 *								to a dynamic string
 * ----------
 */
void
plpgsql_dstring_append_char(PLpgSQL_dstring *ds, char c)
{
	if (ds->used == ds->alloc)
		plpgsql_dstring_expand(ds, ds->used + 1);

	ds->value[ds->used - 1] = c;
	ds->value[ds->used] = '\0';
	ds->used++;
}


/* ----------
 * plpgsql_dstring_get			Dynamic string get value
 * ----------
 */
char *
plpgsql_dstring_get(PLpgSQL_dstring *ds)
{
	return ds->value;
}


/* ----------
 * plpgsql_ns_init			Initialize the namestack
 * ----------
 */
void
plpgsql_ns_init(void)
{
	ns_current = NULL;
	ns_localmode = false;
}


/* ----------
 * plpgsql_ns_setlocal			Tell plpgsql_ns_lookup to or to
 *					not look into the current level
 *					only.
 * ----------
 */
bool
plpgsql_ns_setlocal(bool flag)
{
	bool		oldstate;

	oldstate = ns_localmode;
	ns_localmode = flag;
	return oldstate;
}


/* ----------
 * plpgsql_ns_push			Enter a new namestack level
 * ----------
 */
void
plpgsql_ns_push(char *label)
{
	PLpgSQL_ns *new;

	if (label == NULL)
		label = "";

	new = palloc(sizeof(PLpgSQL_ns));
	memset(new, 0, sizeof(PLpgSQL_ns));
	new->upper = ns_current;
	ns_current = new;

	plpgsql_ns_additem(PLPGSQL_NSTYPE_LABEL, 0, label);
}


/* ----------
 * plpgsql_ns_pop			Return to the previous level
 * ----------
 */
void
plpgsql_ns_pop(void)
{
	int			i;
	PLpgSQL_ns *old;

	old = ns_current;
	ns_current = old->upper;

	for (i = 0; i < old->items_used; i++)
		pfree(old->items[i]);
	pfree(old->items);
	pfree(old);
}


/* ----------
 * plpgsql_ns_additem			Add an item to the current
 *					namestack level
 * ----------
 */
void
plpgsql_ns_additem(int itemtype, int itemno, const char *name)
{
	PLpgSQL_ns *ns = ns_current;
	PLpgSQL_nsitem *nse;

	Assert(name != NULL);

	if (ns->items_used == ns->items_alloc)
	{
		if (ns->items_alloc == 0)
		{
			ns->items_alloc = 32;
			ns->items = palloc(sizeof(PLpgSQL_nsitem *) * ns->items_alloc);
		}
		else
		{
			ns->items_alloc *= 2;
			ns->items = repalloc(ns->items,
								 sizeof(PLpgSQL_nsitem *) * ns->items_alloc);
		}
	}

	nse = palloc(sizeof(PLpgSQL_nsitem) + strlen(name));
	nse->itemtype = itemtype;
	nse->itemno = itemno;
	strcpy(nse->name, name);
	ns->items[ns->items_used++] = nse;
}


/* ----------
 * plpgsql_ns_lookup			Lookup for a word in the namestack
 * ----------
 */
PLpgSQL_nsitem *
plpgsql_ns_lookup(char *name, char *label)
{
	PLpgSQL_ns *ns;
	int			i;

	/*
	 * If a label is specified, lookup only in that
	 */
	if (label != NULL)
	{
		for (ns = ns_current; ns != NULL; ns = ns->upper)
		{
			if (!strcmp(ns->items[0]->name, label))
			{
				for (i = 1; i < ns->items_used; i++)
				{
					if (!strcmp(ns->items[i]->name, name))
						return ns->items[i];
				}
				return NULL;	/* name not found in specified label */
			}
		}
		return NULL;			/* label not found */
	}

	/*
	 * No label given, lookup for visible labels ignoring localmode
	 */
	for (ns = ns_current; ns != NULL; ns = ns->upper)
	{
		if (!strcmp(ns->items[0]->name, name))
			return ns->items[0];
	}

	/*
	 * Finally lookup name in the namestack
	 */
	for (ns = ns_current; ns != NULL; ns = ns->upper)
	{
		for (i = 1; i < ns->items_used; i++)
		{
			if (!strcmp(ns->items[i]->name, name))
				return ns->items[i];
		}
		if (ns_localmode)
			return NULL;		/* name not found in current namespace */
	}

	return NULL;
}


/* ----------
 * plpgsql_ns_rename			Rename a namespace entry
 * ----------
 */
void
plpgsql_ns_rename(char *oldname, char *newname)
{
	PLpgSQL_ns *ns;
	PLpgSQL_nsitem *newitem;
	int			i;

	/*
	 * Lookup name in the namestack; do the lookup in the current namespace
	 * only.
	 */
	for (ns = ns_current; ns != NULL; ns = ns->upper)
	{
		for (i = 1; i < ns->items_used; i++)
		{
			if (!strcmp(ns->items[i]->name, oldname))
			{
				newitem = palloc(sizeof(PLpgSQL_nsitem) + strlen(newname));
				newitem->itemtype = ns->items[i]->itemtype;
				newitem->itemno = ns->items[i]->itemno;
				strcpy(newitem->name, newname);

				pfree(oldname);
				pfree(newname);

				pfree(ns->items[i]);
				ns->items[i] = newitem;
				return;
			}
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("there is no variable \"%s\" in the current block",
					oldname)));
}


/* ----------
 * plpgsql_convert_ident
 *
 * Convert a possibly-qualified identifier to internal form: handle
 * double quotes, translate to lower case where not inside quotes,
 * truncate to NAMEDATALEN.
 *
 * There may be several identifiers separated by dots and optional
 * whitespace.	Each one is converted to a separate palloc'd string.
 * The caller passes the expected number of identifiers, as well as
 * a char* array to hold them.	It is an error if we find the wrong
 * number of identifiers (cf grammar processing of fori_varname).
 *
 * NOTE: the input string has already been accepted by the flex lexer,
 * so we don't need a heckuva lot of error checking here.
 * ----------
 */
void
plpgsql_convert_ident(const char *s, char **output, int numidents)
{
	const char *sstart = s;
	int			identctr = 0;

	/* Outer loop over identifiers */
	while (*s)
	{
		char	   *curident;
		char	   *cp;

		/* Process current identifier */

		if (*s == '"')
		{
			/* Quoted identifier: copy, collapsing out doubled quotes */

			curident = palloc(strlen(s) + 1);	/* surely enough room */
			cp = curident;
			s++;
			while (*s)
			{
				if (*s == '"')
				{
					if (s[1] != '"')
						break;
					s++;
				}
				*cp++ = *s++;
			}
			if (*s != '"')		/* should not happen if lexer checked */
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unterminated \" in name: %s", sstart)));
			s++;
			*cp = '\0';
			/* Truncate to NAMEDATALEN */
			truncate_identifier(curident, cp - curident, false);
		}
		else
		{
			/* Normal identifier: extends till dot or whitespace */
			const char *thisstart = s;

			while (*s && *s != '.' && !scanner_isspace(*s))
				s++;
			/* Downcase and truncate to NAMEDATALEN */
			curident = downcase_truncate_identifier(thisstart, s - thisstart,
													false);
		}

		/* Pass ident to caller */
		if (identctr < numidents)
			output[identctr++] = curident;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("qualified identifier cannot be used here: %s",
							sstart)));

		/* If not done, skip whitespace, dot, whitespace */
		if (*s)
		{
			while (*s && scanner_isspace(*s))
				s++;
			if (*s++ != '.')
				elog(ERROR, "expected dot between identifiers: %s", sstart);
			while (*s && scanner_isspace(*s))
				s++;
			if (*s == '\0')
				elog(ERROR, "expected another identifier: %s", sstart);
		}
	}

	if (identctr != numidents)
		elog(ERROR, "improperly qualified identifier: %s",
			 sstart);
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
			return "block variables initialization";
		case PLPGSQL_STMT_ASSIGN:
			return "assignment";
		case PLPGSQL_STMT_IF:
			return "if";
		case PLPGSQL_STMT_LOOP:
			return "loop";
		case PLPGSQL_STMT_WHILE:
			return "while";
		case PLPGSQL_STMT_FORI:
			return "for with integer loopvar";
		case PLPGSQL_STMT_FORS:
			return "for over select rows";
		case PLPGSQL_STMT_EXIT:
			return "exit";
		case PLPGSQL_STMT_RETURN:
			return "return";
		case PLPGSQL_STMT_RETURN_NEXT:
			return "return next";
		case PLPGSQL_STMT_RAISE:
			return "raise";
		case PLPGSQL_STMT_EXECSQL:
			return "SQL statement";
		case PLPGSQL_STMT_DYNEXECUTE:
			return "execute statement";
		case PLPGSQL_STMT_DYNFORS:
			return "for over execute statement";
		case PLPGSQL_STMT_GETDIAG:
			return "get diagnostics";
		case PLPGSQL_STMT_OPEN:
			return "open";
		case PLPGSQL_STMT_FETCH:
			return "fetch";
		case PLPGSQL_STMT_CLOSE:
			return "close";
		case PLPGSQL_STMT_PERFORM:
			return "perform";
	}

	return "unknown";
}


/**********************************************************************
 * Debug functions for analyzing the compiled code
 **********************************************************************/
static int	dump_indent;

static void dump_ind();
static void dump_stmt(PLpgSQL_stmt *stmt);
static void dump_block(PLpgSQL_stmt_block *block);
static void dump_assign(PLpgSQL_stmt_assign *stmt);
static void dump_if(PLpgSQL_stmt_if *stmt);
static void dump_loop(PLpgSQL_stmt_loop *stmt);
static void dump_while(PLpgSQL_stmt_while *stmt);
static void dump_fori(PLpgSQL_stmt_fori *stmt);
static void dump_fors(PLpgSQL_stmt_fors *stmt);
static void dump_exit(PLpgSQL_stmt_exit *stmt);
static void dump_return(PLpgSQL_stmt_return *stmt);
static void dump_return_next(PLpgSQL_stmt_return_next *stmt);
static void dump_raise(PLpgSQL_stmt_raise *stmt);
static void dump_execsql(PLpgSQL_stmt_execsql *stmt);
static void dump_dynexecute(PLpgSQL_stmt_dynexecute *stmt);
static void dump_dynfors(PLpgSQL_stmt_dynfors *stmt);
static void dump_getdiag(PLpgSQL_stmt_getdiag *stmt);
static void dump_open(PLpgSQL_stmt_open *stmt);
static void dump_fetch(PLpgSQL_stmt_fetch *stmt);
static void dump_close(PLpgSQL_stmt_close *stmt);
static void dump_perform(PLpgSQL_stmt_perform *stmt);
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
		case PLPGSQL_STMT_EXIT:
			dump_exit((PLpgSQL_stmt_exit *) stmt);
			break;
		case PLPGSQL_STMT_RETURN:
			dump_return((PLpgSQL_stmt_return *) stmt);
			break;
		case PLPGSQL_STMT_RETURN_NEXT:
			dump_return_next((PLpgSQL_stmt_return_next *) stmt);
			break;
		case PLPGSQL_STMT_RAISE:
			dump_raise((PLpgSQL_stmt_raise *) stmt);
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
	dump_ind();
	printf("IF ");
	dump_expr(stmt->cond);
	printf(" THEN\n");

	dump_stmts(stmt->true_body);

	if (stmt->false_body != NIL)
	{
		dump_ind();
		printf("    ELSE\n");
		dump_stmts(stmt->false_body);
	}

	dump_ind();
	printf("    ENDIF\n");
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
	dump_ind();
	printf("    by = ");
	dump_expr(stmt->by);
	printf("\n");
	dump_indent -= 2;

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDFORI\n");
}

static void
dump_fors(PLpgSQL_stmt_fors *stmt)
{
	dump_ind();
	printf("FORS %s ", (stmt->rec != NULL) ? stmt->rec->refname : stmt->row->refname);
	dump_expr(stmt->query);
	printf("\n");

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDFORS\n");
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
	}
	dump_indent -= 2;

}

static void
dump_fetch(PLpgSQL_stmt_fetch *stmt)
{
	dump_ind();
	printf("FETCH curvar=%d\n", stmt->curvar);

	dump_indent += 2;
	if (stmt->rec != NULL)
	{
		dump_ind();
		printf("    target = %d %s\n", stmt->rec->recno, stmt->rec->refname);
	}
	if (stmt->row != NULL)
	{
		dump_ind();
		printf("    target = %d %s\n", stmt->row->rowno, stmt->row->refname);
	}
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
dump_exit(PLpgSQL_stmt_exit *stmt)
{
	dump_ind();
	printf("%s label='%s'",
		   stmt->is_exit ? "EXIT" : "CONTINUE", stmt->label);
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
dump_raise(PLpgSQL_stmt_raise *stmt)
{
	ListCell   *lc;
	int			i = 0;

	dump_ind();
	printf("RAISE '%s'\n", stmt->message);
	dump_indent += 2;
	foreach(lc, stmt->params)
	{
		dump_ind();
		printf("    parameter %d: ", i++);
		dump_expr((PLpgSQL_expr *) lfirst(lc));
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
	if (stmt->rec != NULL)
	{
		dump_ind();
		printf("    INTO%s target = %d %s\n",
			   stmt->strict ? " STRICT" : "",
			   stmt->rec->recno, stmt->rec->refname);
	}
	if (stmt->row != NULL)
	{
		dump_ind();
		printf("    INTO%s target = %d %s\n",
			   stmt->strict ? " STRICT" : "",
			   stmt->row->rowno, stmt->row->refname);
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
	if (stmt->rec != NULL)
	{
		dump_ind();
		printf("    INTO%s target = %d %s\n",
			   stmt->strict ? " STRICT" : "",
			   stmt->rec->recno, stmt->rec->refname);
	}
	if (stmt->row != NULL)
	{
		dump_ind();
		printf("    INTO%s target = %d %s\n",
			   stmt->strict ? " STRICT" : "",
			   stmt->row->rowno, stmt->row->refname);
	}
	dump_indent -= 2;
}

static void
dump_dynfors(PLpgSQL_stmt_dynfors *stmt)
{
	dump_ind();
	printf("FORS %s EXECUTE ", (stmt->rec != NULL) ? stmt->rec->refname : stmt->row->refname);
	dump_expr(stmt->query);
	printf("\n");

	dump_stmts(stmt->body);

	dump_ind();
	printf("    ENDFORS\n");
}

static void
dump_getdiag(PLpgSQL_stmt_getdiag *stmt)
{
	ListCell   *lc;

	dump_ind();
	printf("GET DIAGNOSTICS ");
	foreach(lc, stmt->diag_items)
	{
		PLpgSQL_diag_item *diag_item = (PLpgSQL_diag_item *) lfirst(lc);

		if (lc != list_head(stmt->diag_items))
			printf(", ");

		printf("{var %d} = ", diag_item->target);

		switch (diag_item->kind)
		{
			case PLPGSQL_GETDIAG_ROW_COUNT:
				printf("ROW_COUNT");
				break;

			case PLPGSQL_GETDIAG_RESULT_OID:
				printf("RESULT_OID");
				break;

			default:
				printf("???");
				break;
		}
	}
	printf("\n");
}

static void
dump_expr(PLpgSQL_expr *expr)
{
	int			i;

	printf("'%s", expr->query);
	if (expr->nparams > 0)
	{
		printf(" {");
		for (i = 0; i < expr->nparams; i++)
		{
			if (i > 0)
				printf(", ");
			printf("$%d=%d", i + 1, expr->params[i]);
		}
		printf("}");
	}
	printf("'");
}

void
plpgsql_dumptree(PLpgSQL_function *func)
{
	int			i;
	PLpgSQL_datum *d;

	printf("\nExecution tree of successfully compiled PL/pgSQL function %s:\n",
		   func->fn_name);

	printf("\nFunction's data area:\n");
	for (i = 0; i < func->ndatums; i++)
	{
		d = func->datums[i];

		printf("    entry %d: ", i);
		switch (d->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
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
				}
				break;
			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) d;
					int			i;

					printf("ROW %-16s fields", row->refname);
					for (i = 0; i < row->nfields; i++)
					{
						if (row->fieldnames[i])
							printf(" %s=var %d", row->fieldnames[i],
								   row->varnos[i]);
					}
					printf("\n");
				}
				break;
			case PLPGSQL_DTYPE_REC:
				printf("REC %s\n", ((PLpgSQL_rec *) d)->refname);
				break;
			case PLPGSQL_DTYPE_RECFIELD:
				printf("RECFIELD %-16s of REC %d\n",
					   ((PLpgSQL_recfield *) d)->fieldname,
					   ((PLpgSQL_recfield *) d)->recparentno);
				break;
			case PLPGSQL_DTYPE_ARRAYELEM:
				printf("ARRAYELEM of VAR %d subscript ",
					   ((PLpgSQL_arrayelem *) d)->arrayparentno);
				dump_expr(((PLpgSQL_arrayelem *) d)->subscript);
				printf("\n");
				break;
			case PLPGSQL_DTYPE_TRIGARG:
				printf("TRIGARG ");
				dump_expr(((PLpgSQL_trigarg *) d)->argnum);
				printf("\n");
				break;
			default:
				printf("??? unknown data type %d\n", d->dtype);
		}
	}
	printf("\nFunction's statements:\n");

	dump_indent = 0;
	printf("%3d:", func->action->lineno);
	dump_block(func->action);
	printf("\nEnd of execution tree of function %s\n\n", func->fn_name);
	fflush(stdout);
}
