%top{
/*-------------------------------------------------------------------------
 *
 * bootscanner.l
 *	  a lexical scanner for the bootstrap parser
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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


static int	yyline = 1;			/* line number for error reporting */

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="boot_yy"


id		[-A-Za-z0-9_]+
sid		\'([^']|\'\')*\'

/*
 * Keyword tokens return the keyword text (as a constant string) in boot_yylval.kw,
 * just in case that's needed because we want to treat the keyword as an
 * unreserved identifier.  Note that _null_ is not treated as a keyword
 * for this purpose; it's the one "reserved word" in the bootstrap syntax.
 *
 * Notice that all the keywords are case-sensitive, and for historical
 * reasons some must be upper case.
 *
 * String tokens return a palloc'd string in boot_yylval.str.
 */

%%

open			{ boot_yylval.kw = "open"; return OPEN; }

close			{ boot_yylval.kw = "close"; return XCLOSE; }

create			{ boot_yylval.kw = "create"; return XCREATE; }

OID				{ boot_yylval.kw = "OID"; return OBJ_ID; }
bootstrap		{ boot_yylval.kw = "bootstrap"; return XBOOTSTRAP; }
shared_relation	{ boot_yylval.kw = "shared_relation"; return XSHARED_RELATION; }
rowtype_oid		{ boot_yylval.kw = "rowtype_oid"; return XROWTYPE_OID; }

insert			{ boot_yylval.kw = "insert"; return INSERT_TUPLE; }

_null_			{ return NULLVAL; }

","				{ return COMMA; }
"="				{ return EQUALS; }
"("				{ return LPAREN; }
")"				{ return RPAREN; }

[\n]			{ yyline++; }
[\r\t ]			;

^\#[^\n]*		;		/* drop everything after "#" for comments */

declare			{ boot_yylval.kw = "declare"; return XDECLARE; }
build			{ boot_yylval.kw = "build"; return XBUILD; }
indices			{ boot_yylval.kw = "indices"; return INDICES; }
unique			{ boot_yylval.kw = "unique"; return UNIQUE; }
index			{ boot_yylval.kw = "index"; return INDEX; }
on				{ boot_yylval.kw = "on"; return ON; }
using			{ boot_yylval.kw = "using"; return USING; }
toast			{ boot_yylval.kw = "toast"; return XTOAST; }
FORCE			{ boot_yylval.kw = "FORCE"; return XFORCE; }
NOT				{ boot_yylval.kw = "NOT"; return XNOT; }
NULL			{ boot_yylval.kw = "NULL"; return XNULL; }

{id}			{
					boot_yylval.str = pstrdup(yytext);
					return ID;
				}
{sid}			{
					/* strip quotes and escapes */
					boot_yylval.str = DeescapeQuotedString(yytext);
					return ID;
				}

.				{
					elog(ERROR, "syntax error at line %d: unexpected character \"%s\"", yyline, yytext);
				}

%%

/* LCOV_EXCL_STOP */

void
boot_yyerror(const char *message)
{
	elog(ERROR, "%s at line %d", message, yyline);
}
