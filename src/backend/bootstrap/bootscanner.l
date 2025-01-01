%top{
/*-------------------------------------------------------------------------
 *
 * bootscanner.l
 *	  a lexical scanner for the bootstrap parser
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootscanner.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

/*
 * NB: include bootparse.h only AFTER including bootstrap.h, because bootstrap.h
 * includes node definitions needed for YYSTYPE.
 */
#include "bootstrap/bootstrap.h"
#include "bootparse.h"
#include "utils/guc.h"

}

%{

/* LCOV_EXCL_START */

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

%}

%option reentrant
%option bison-bridge
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option noyyalloc
%option noyyrealloc
%option noyyfree
%option warn
%option prefix="boot_yy"


id		[-A-Za-z0-9_]+
sid		\'([^']|\'\')*\'

/*
 * Keyword tokens return the keyword text (as a constant string) in yylval->kw,
 * just in case that's needed because we want to treat the keyword as an
 * unreserved identifier.  Note that _null_ is not treated as a keyword
 * for this purpose; it's the one "reserved word" in the bootstrap syntax.
 *
 * Notice that all the keywords are case-sensitive, and for historical
 * reasons some must be upper case.
 *
 * String tokens return a palloc'd string in yylval->str.
 */

%%

open			{ yylval->kw = "open"; return OPEN; }

close			{ yylval->kw = "close"; return XCLOSE; }

create			{ yylval->kw = "create"; return XCREATE; }

OID				{ yylval->kw = "OID"; return OBJ_ID; }
bootstrap		{ yylval->kw = "bootstrap"; return XBOOTSTRAP; }
shared_relation	{ yylval->kw = "shared_relation"; return XSHARED_RELATION; }
rowtype_oid		{ yylval->kw = "rowtype_oid"; return XROWTYPE_OID; }

insert			{ yylval->kw = "insert"; return INSERT_TUPLE; }

_null_			{ return NULLVAL; }

","				{ return COMMA; }
"="				{ return EQUALS; }
"("				{ return LPAREN; }
")"				{ return RPAREN; }

[\n]			{ yylineno++; }
[\r\t ]			;

^\#[^\n]*		;		/* drop everything after "#" for comments */

declare			{ yylval->kw = "declare"; return XDECLARE; }
build			{ yylval->kw = "build"; return XBUILD; }
indices			{ yylval->kw = "indices"; return INDICES; }
unique			{ yylval->kw = "unique"; return UNIQUE; }
index			{ yylval->kw = "index"; return INDEX; }
on				{ yylval->kw = "on"; return ON; }
using			{ yylval->kw = "using"; return USING; }
toast			{ yylval->kw = "toast"; return XTOAST; }
FORCE			{ yylval->kw = "FORCE"; return XFORCE; }
NOT				{ yylval->kw = "NOT"; return XNOT; }
NULL			{ yylval->kw = "NULL"; return XNULL; }

{id}			{
					yylval->str = pstrdup(yytext);
					return ID;
				}
{sid}			{
					/* strip quotes and escapes */
					yylval->str = DeescapeQuotedString(yytext);
					return ID;
				}

.				{
					elog(ERROR, "syntax error at line %d: unexpected character \"%s\"", yylineno, yytext);
				}

%%

/* LCOV_EXCL_STOP */

void
boot_yyerror(yyscan_t yyscanner, const char *message)
{
	struct yyguts_t *yyg = (struct yyguts_t *) yyscanner;	/* needed for yylineno
															 * macro */

	elog(ERROR, "%s at line %d", message, yylineno);
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 */

void *
yyalloc(yy_size_t size, yyscan_t yyscanner)
{
	return palloc(size);
}

void *
yyrealloc(void *ptr, yy_size_t size, yyscan_t yyscanner)
{
	if (ptr)
		return repalloc(ptr, size);
	else
		return palloc(size);
}

void
yyfree(void *ptr, yyscan_t yyscanner)
{
	if (ptr)
		pfree(ptr);
}
