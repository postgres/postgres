/**********************************************************************
 * pl_funcs.c		- Misc functins for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plpgsql/src/pl_funcs.c,v 1.3 1999/01/28 11:48:31 wieck Exp $
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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>

#include "plpgsql.h"
#include "pl.tab.h"


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
plpgsql_dstring_init(PLpgSQL_dstring * ds)
{
	ds->value = palloc(ds->alloc = 512);
	ds->used = 0;
}


/* ----------
 * plpgsql_dstring_free			Dynamic string destruction
 * ----------
 */
void
plpgsql_dstring_free(PLpgSQL_dstring * ds)
{
	pfree(ds->value);
}


/* ----------
 * plpgsql_dstring_append		Dynamic string extending
 * ----------
 */
void
plpgsql_dstring_append(PLpgSQL_dstring * ds, char *str)
{
	int			len = strlen(str);

	if (ds->used + len + 1 > ds->alloc)
	{
		ds->alloc *= 2;
		ds->value = repalloc(ds->value, ds->alloc);
	}

	strcpy(&(ds->value[ds->used]), str);
	ds->used += len;
}


/* ----------
 * plpgsql_dstring_get			Dynamic string get value
 * ----------
 */
char *
plpgsql_dstring_get(PLpgSQL_dstring * ds)
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
plpgsql_ns_pop()
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
plpgsql_ns_additem(int itemtype, int itemno, char *name)
{
	PLpgSQL_ns *ns = ns_current;
	PLpgSQL_nsitem *nse;

	if (name == NULL)
		name = "";
	name = plpgsql_tolower(name);

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

	/* ----------
	 * If a label is specified, lookup only in that
	 * ----------
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

	/* ----------
	 * No label given, lookup for visible labels ignoring localmode
	 * ----------
	 */
	for (ns = ns_current; ns != NULL; ns = ns->upper)
	{
		if (!strcmp(ns->items[0]->name, name))
			return ns->items[0];
	}

	/* ----------
	 * Finally lookup name in the namestack
	 * ----------
	 */
	for (ns = ns_current; ns != NULL; ns = ns->upper)
	{
		for (i = 1; i < ns->items_used; i++)
		{
			if (!strcmp(ns->items[i]->name, name))
				return ns->items[i];
		}
		if (ns_localmode)
		{
			return NULL;		/* name not found in current namespace */
		}
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

	/* ----------
	 * Lookup in the current namespace only
	 * ----------
	 */
	/* ----------
	 * Lookup name in the namestack
	 * ----------
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

	elog(ERROR, "there is no variable '%s' in the current block", oldname);
}


/* ----------
 * plpgsql_tolower			Translate a string to lower case
 *					but honor "" escaping.
 * ----------
 */
char *
plpgsql_tolower(char *s)
{
	char   *ret;
	char   *cp;

	ret = palloc(strlen(s) + 1);
	cp = ret;

	while (*s)
	{
		if (*s == '"')
		{
			s++;
			while (*s)
			{
				if (*s == '"')
					break;
				*cp++ = *s++;
			}
			if (*s != '"')
			{
				plpgsql_comperrinfo();
				elog(ERROR, "unterminated \"");
			}
			s++;
		}
		else
		{
			if (isupper(*s))
				*cp++ = tolower(*s++);
			else
				*cp++ = *s++;
		}
	}
	*cp = '\0';

	return ret;
}





/**********************************************************************
 * Debug functions for analyzing the compiled code
 **********************************************************************/
static int	dump_indent;

static void dump_ind();
static void dump_stmt(PLpgSQL_stmt * stmt);
static void dump_block(PLpgSQL_stmt_block * block);
static void dump_assign(PLpgSQL_stmt_assign * stmt);
static void dump_if(PLpgSQL_stmt_if * stmt);
static void dump_loop(PLpgSQL_stmt_loop * stmt);
static void dump_while(PLpgSQL_stmt_while * stmt);
static void dump_fori(PLpgSQL_stmt_fori * stmt);
static void dump_fors(PLpgSQL_stmt_fors * stmt);
static void dump_select(PLpgSQL_stmt_select * stmt);
static void dump_exit(PLpgSQL_stmt_exit * stmt);
static void dump_return(PLpgSQL_stmt_return * stmt);
static void dump_raise(PLpgSQL_stmt_raise * stmt);
static void dump_execsql(PLpgSQL_stmt_execsql * stmt);
static void dump_expr(PLpgSQL_expr * expr);


static void
dump_ind()
{
	int			i;

	for (i = 0; i < dump_indent; i++)
		printf(" ");
}

static void
dump_stmt(PLpgSQL_stmt * stmt)
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
		case PLPGSQL_STMT_SELECT:
			dump_select((PLpgSQL_stmt_select *) stmt);
			break;
		case PLPGSQL_STMT_EXIT:
			dump_exit((PLpgSQL_stmt_exit *) stmt);
			break;
		case PLPGSQL_STMT_RETURN:
			dump_return((PLpgSQL_stmt_return *) stmt);
			break;
		case PLPGSQL_STMT_RAISE:
			dump_raise((PLpgSQL_stmt_raise *) stmt);
			break;
		case PLPGSQL_STMT_EXECSQL:
			dump_execsql((PLpgSQL_stmt_execsql *) stmt);
			break;
		default:
			elog(ERROR, "plpgsql_dump: unknown cmd_type %d\n", stmt->cmd_type);
			break;
	}
}

static void
dump_block(PLpgSQL_stmt_block * block)
{
	int			i;
	char	   *name;

	if (block->label == NULL)
		name = "*unnamed*";
	else
		name = block->label;

	dump_ind();
	printf("BLOCK <<%s>>\n", name);

	dump_indent += 2;
	for (i = 0; i < block->body->stmts_used; i++)
		dump_stmt((PLpgSQL_stmt *) (block->body->stmts[i]));
	dump_indent -= 2;

	dump_ind();
	printf("    END -- %s\n", name);
}

static void
dump_assign(PLpgSQL_stmt_assign * stmt)
{
	dump_ind();
	printf("ASSIGN var %d := ", stmt->varno);
	dump_expr(stmt->expr);
	printf("\n");
}

static void
dump_if(PLpgSQL_stmt_if * stmt)
{
	int			i;

	dump_ind();
	printf("IF ");
	dump_expr(stmt->cond);
	printf(" THEN\n");

	dump_indent += 2;
	for (i = 0; i < stmt->true_body->stmts_used; i++)
		dump_stmt((PLpgSQL_stmt *) (stmt->true_body->stmts[i]));
	dump_indent -= 2;

	dump_ind();
	printf("    ELSE\n");

	dump_indent += 2;
	for (i = 0; i < stmt->false_body->stmts_used; i++)
		dump_stmt((PLpgSQL_stmt *) (stmt->false_body->stmts[i]));
	dump_indent -= 2;

	dump_ind();
	printf("    ENDIF\n");
}

static void
dump_loop(PLpgSQL_stmt_loop * stmt)
{
	int			i;

	dump_ind();
	printf("LOOP\n");

	dump_indent += 2;
	for (i = 0; i < stmt->body->stmts_used; i++)
		dump_stmt((PLpgSQL_stmt *) (stmt->body->stmts[i]));
	dump_indent -= 2;

	dump_ind();
	printf("    ENDLOOP\n");
}

static void
dump_while(PLpgSQL_stmt_while * stmt)
{
	int			i;

	dump_ind();
	printf("WHILE ");
	dump_expr(stmt->cond);
	printf("\n");

	dump_indent += 2;
	for (i = 0; i < stmt->body->stmts_used; i++)
		dump_stmt((PLpgSQL_stmt *) (stmt->body->stmts[i]));
	dump_indent -= 2;

	dump_ind();
	printf("    ENDWHILE\n");
}

static void
dump_fori(PLpgSQL_stmt_fori * stmt)
{
	int			i;

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

	for (i = 0; i < stmt->body->stmts_used; i++)
		dump_stmt((PLpgSQL_stmt *) (stmt->body->stmts[i]));
	dump_indent -= 2;

	dump_ind();
	printf("    ENDFORI\n");
}

static void
dump_fors(PLpgSQL_stmt_fors * stmt)
{
	int			i;

	dump_ind();
	printf("FORS %s ", (stmt->rec != NULL) ? stmt->rec->refname : stmt->row->refname);
	dump_expr(stmt->query);
	printf("\n");

	dump_indent += 2;
	for (i = 0; i < stmt->body->stmts_used; i++)
		dump_stmt((PLpgSQL_stmt *) (stmt->body->stmts[i]));
	dump_indent -= 2;

	dump_ind();
	printf("    ENDFORS\n");
}

static void
dump_select(PLpgSQL_stmt_select * stmt)
{
	dump_ind();
	printf("SELECT ");
	dump_expr(stmt->query);
	printf("\n");

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
dump_exit(PLpgSQL_stmt_exit * stmt)
{
	dump_ind();
	printf("EXIT lbl='%s'", stmt->label);
	if (stmt->cond != NULL)
	{
		printf(" WHEN ");
		dump_expr(stmt->cond);
	}
	printf("\n");
}

static void
dump_return(PLpgSQL_stmt_return * stmt)
{
	dump_ind();
	printf("RETURN ");
	if (stmt->retrecno >= 0)
		printf("record %d", stmt->retrecno);
	else
	{
		if (stmt->expr == NULL)
			printf("NULL");
		else
			dump_expr(stmt->expr);
	}
	printf("\n");
}

static void
dump_raise(PLpgSQL_stmt_raise * stmt)
{
	int			i;

	dump_ind();
	printf("RAISE '%s'", stmt->message);
	for (i = 0; i < stmt->nparams; i++)
		printf(" %d", stmt->params[i]);
	printf("\n");
}

static void
dump_execsql(PLpgSQL_stmt_execsql * stmt)
{
	dump_ind();
	printf("EXECSQL ");
	dump_expr(stmt->sqlstmt);
	printf("\n");
}

static void
dump_expr(PLpgSQL_expr * expr)
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
plpgsql_dumptree(PLpgSQL_function * func)
{
	int			i;
	PLpgSQL_datum *d;

	printf("\nExecution tree of successfully compiled PL/pgSQL function %s:\n",
		   func->fn_name);

	printf("\nFunctions data area:\n");
	for (i = 0; i < func->ndatums; i++)
	{
		d = func->datums[i];

		printf("    entry %d: ", i);
		switch (d->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var *var = (PLpgSQL_var *) d;

					printf("VAR %-16s type %s (typoid %d) atttypmod %d\n",
						   var->refname, var->datatype->typname,
						   var->datatype->typoid,
						   var->datatype->atttypmod);
				}
				break;
			case PLPGSQL_DTYPE_ROW:
				{
					PLpgSQL_row *row = (PLpgSQL_row *) d;
					int			i;

					printf("ROW %-16s fields", row->refname);
					for (i = 0; i < row->nfields; i++)
					{
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
				printf("RECFIELD %-16s of REC %d\n", ((PLpgSQL_recfield *) d)->fieldname, ((PLpgSQL_recfield *) d)->recno);
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
	printf("\nFunctions statements:\n");

	dump_indent = 0;
	printf("%3d:", func->action->lineno);
	dump_block(func->action);
	printf("\nEnd of execution tree of function %s\n\n", func->fn_name);
}
