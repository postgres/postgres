/**********************************************************************
 * pl_funcs.c		- Misc functins for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plpgsql/src/pl_funcs.c,v 1.30.2.1 2004/02/21 00:35:13 tgl Exp $
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
plpgsql_dstring_init(PLpgSQL_dstring * ds)
{
	ds->value = palloc(ds->alloc = 512);
	ds->used = 0;
	ds->value[0] = '\0';
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
	int			needed = ds->used + len + 1;

	if (needed > ds->alloc)
	{
		/* might have to double more than once, if len is large */
		do
		{
			ds->alloc *= 2;
		} while (needed > ds->alloc);
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
	 * Lookup in the current namespace only
	 */

	/*
	 * Lookup name in the namestack
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

			curident = palloc(strlen(s) + 1); /* surely enough room */
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
			truncate_identifier(curident, cp-curident, false);
		}
		else
		{
			/* Normal identifier: extends till dot or whitespace */
			const char *thisstart = s;

			while (*s && *s != '.' && !isspace((unsigned char) *s))
				s++;
			/* Downcase and truncate to NAMEDATALEN */
			curident = downcase_truncate_identifier(thisstart, s-thisstart,
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
			while (*s && isspace((unsigned char) *s))
				s++;
			if (*s++ != '.')
				elog(ERROR, "expected dot between identifiers: %s", sstart);
			while (*s && isspace((unsigned char) *s))
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
plpgsql_stmt_typename(PLpgSQL_stmt * stmt)
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
		case PLPGSQL_STMT_SELECT:
			return "select into variables";
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
static void dump_return_next(PLpgSQL_stmt_return_next * stmt);
static void dump_raise(PLpgSQL_stmt_raise * stmt);
static void dump_execsql(PLpgSQL_stmt_execsql * stmt);
static void dump_dynexecute(PLpgSQL_stmt_dynexecute * stmt);
static void dump_dynfors(PLpgSQL_stmt_dynfors * stmt);
static void dump_getdiag(PLpgSQL_stmt_getdiag * stmt);
static void dump_open(PLpgSQL_stmt_open * stmt);
static void dump_fetch(PLpgSQL_stmt_fetch * stmt);
static void dump_close(PLpgSQL_stmt_close * stmt);
static void dump_perform(PLpgSQL_stmt_perform * stmt);
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
dump_open(PLpgSQL_stmt_open * stmt)
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
dump_fetch(PLpgSQL_stmt_fetch * stmt)
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
dump_close(PLpgSQL_stmt_close * stmt)
{
	dump_ind();
	printf("CLOSE curvar=%d\n", stmt->curvar);
}

static void
dump_perform(PLpgSQL_stmt_perform * stmt)
{
	dump_ind();
	printf("PERFORM expr = ");
	dump_expr(stmt->expr);
	printf("\n");
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
	else if (stmt->retrowno >= 0)
		printf("row %d", stmt->retrowno);
	else if (stmt->expr == NULL)
		printf("NULL");
	else
		dump_expr(stmt->expr);
	printf("\n");
}

static void
dump_return_next(PLpgSQL_stmt_return_next * stmt)
{
	dump_ind();
	printf("RETURN NEXT ");
	if (stmt->rec != NULL)
		printf("target = %d %s\n", stmt->rec->recno, stmt->rec->refname);
	else if (stmt->row != NULL)
		printf("target = %d %s\n", stmt->row->rowno, stmt->row->refname);
	else if (stmt->expr != NULL)
		dump_expr(stmt->expr);
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
dump_dynexecute(PLpgSQL_stmt_dynexecute * stmt)
{
	dump_ind();
	printf("EXECUTE ");
	dump_expr(stmt->query);
	printf("\n");
}

static void
dump_dynfors(PLpgSQL_stmt_dynfors * stmt)
{
	int			i;

	dump_ind();
	printf("FORS %s EXECUTE ", (stmt->rec != NULL) ? stmt->rec->refname : stmt->row->refname);
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
dump_getdiag(PLpgSQL_stmt_getdiag * stmt)
{
	int			i;

	dump_ind();
	printf("GET DIAGNOSTICS ");
	for (i = 0; i < stmt->ndtitems; i++)
	{
		PLpgSQL_diag_item *dtitem = &stmt->dtitems[i];

		if (i != 0)
			printf(", ");

		printf("{var %d} = ", dtitem->target);

		switch (dtitem->item)
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
	printf("\nFunctions statements:\n");

	dump_indent = 0;
	printf("%3d:", func->action->lineno);
	dump_block(func->action);
	printf("\nEnd of execution tree of function %s\n\n", func->fn_name);
}
