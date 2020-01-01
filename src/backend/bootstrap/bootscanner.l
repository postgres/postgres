%{
/*-------------------------------------------------------------------------
 *
 * bootscanner.l
 *	  a lexical scanner for the bootstrap parser
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootscanner.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/attnum.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/tupdesc.h"
#include "bootstrap/bootstrap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "parser/scansup.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/fd.h"
#include "storage/itemptr.h"
#include "storage/off.h"
#include "utils/rel.h"

/* Not needed now that this file is compiled as part of bootparse. */
/* #include "bootparse.h" */

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
sid		\"([^\"])*\"

/*
 * Keyword tokens return the keyword text (as a constant string) in yylval.kw,
 * just in case that's needed because we want to treat the keyword as an
 * unreserved identifier.  Note that _null_ is not treated as a keyword
 * for this purpose; it's the one "reserved word" in the bootstrap syntax.
 *
 * Notice that all the keywords are case-sensitive, and for historical
 * reasons some must be upper case.
 *
 * String tokens return a palloc'd string in yylval.str.
 */

%%

open			{ yylval.kw = "open"; return OPEN; }

close			{ yylval.kw = "close"; return XCLOSE; }

create			{ yylval.kw = "create"; return XCREATE; }

OID				{ yylval.kw = "OID"; return OBJ_ID; }
bootstrap		{ yylval.kw = "bootstrap"; return XBOOTSTRAP; }
shared_relation	{ yylval.kw = "shared_relation"; return XSHARED_RELATION; }
rowtype_oid		{ yylval.kw = "rowtype_oid"; return XROWTYPE_OID; }

insert			{ yylval.kw = "insert"; return INSERT_TUPLE; }

_null_			{ return NULLVAL; }

","				{ return COMMA; }
"="				{ return EQUALS; }
"("				{ return LPAREN; }
")"				{ return RPAREN; }

[\n]			{ yyline++; }
[\r\t ]			;

^\#[^\n]* 		;		/* drop everything after "#" for comments */

declare			{ yylval.kw = "declare"; return XDECLARE; }
build			{ yylval.kw = "build"; return XBUILD; }
indices			{ yylval.kw = "indices"; return INDICES; }
unique			{ yylval.kw = "unique"; return UNIQUE; }
index			{ yylval.kw = "index"; return INDEX; }
on				{ yylval.kw = "on"; return ON; }
using			{ yylval.kw = "using"; return USING; }
toast			{ yylval.kw = "toast"; return XTOAST; }
FORCE			{ yylval.kw = "FORCE"; return XFORCE; }
NOT				{ yylval.kw = "NOT"; return XNOT; }
NULL			{ yylval.kw = "NULL"; return XNULL; }

{id}			{
					yylval.str = scanstr(yytext);
					return ID;
				}
{sid}			{
					/* leading and trailing quotes are not passed to scanstr */
					yytext[strlen(yytext) - 1] = '\0';
					yylval.str = scanstr(yytext+1);
					yytext[strlen(yytext)] = '"';	/* restore yytext */
					return ID;
				}

.				{
					elog(ERROR, "syntax error at line %d: unexpected character \"%s\"", yyline, yytext);
				}

%%

/* LCOV_EXCL_STOP */

void
yyerror(const char *message)
{
	elog(ERROR, "%s at line %d", message, yyline);
}
