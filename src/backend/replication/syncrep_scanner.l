%top{
/*-------------------------------------------------------------------------
 *
 * syncrep_scanner.l
 *	  a lexical scanner for synchronous_standby_names
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep_scanner.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "nodes/pg_list.h"

/*
 * NB: include syncrep_gram.h only AFTER including syncrep.h, because syncrep.h
 * includes node definitions needed for YYSTYPE.
 */
#include "replication/syncrep.h"
#include "syncrep_gram.h"
}

%{
/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

struct syncrep_yy_extra_type
{
	StringInfoData xdbuf;
};

/* LCOV_EXCL_START */

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
%option prefix="syncrep_yy"
%option extra-type="struct syncrep_yy_extra_type *"

/*
 * <xd> delimited identifiers (double-quoted identifiers)
 */
%x xd

space			[ \t\n\r\f\v]

digit			[0-9]
ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]
identifier		{ident_start}{ident_cont}*

dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

%%
{space}+	{ /* ignore */ }

	/* brute-force case insensitivity is safer than relying on flex -i */

[Aa][Nn][Yy]			{ return ANY; }
[Ff][Ii][Rr][Ss][Tt]	{ return FIRST; }

{xdstart}	{
				initStringInfo(&yyextra->xdbuf);
				BEGIN(xd);
		}
<xd>{xddouble} {
				appendStringInfoChar(&yyextra->xdbuf, '"');
		}
<xd>{xdinside} {
				appendStringInfoString(&yyextra->xdbuf, yytext);
		}
<xd>{xdstop} {
				yylval->str = yyextra->xdbuf.data;
				yyextra->xdbuf.data = NULL;
				BEGIN(INITIAL);
				return NAME;
		}
<xd><<EOF>> {
				syncrep_yyerror(yyscanner, "unterminated quoted identifier");
				return JUNK;
		}

{identifier} {
				yylval->str = pstrdup(yytext);
				return NAME;
		}

{digit}+	{
				yylval->str = pstrdup(yytext);
				return NUM;
		}

"*"		{
				yylval->str = "*";
				return NAME;
		}

","			{ return ','; }
"("			{ return '('; }
")"			{ return ')'; }

.			{ return JUNK; }
%%

/* LCOV_EXCL_STOP */

/* see scan.l */
#undef yyextra
#define yyextra (((struct yyguts_t *) yyscanner)->yyextra_r)

/* Needs to be here for access to yytext */
void
syncrep_yyerror(yyscan_t yyscanner, const char *message)
{
	struct yyguts_t *yyg = (struct yyguts_t *) yyscanner;	/* needed for yytext
															 * macro */

	/* report only the first error in a parse operation */
	if (syncrep_parse_error_msg)
		return;
	if (yytext[0])
		syncrep_parse_error_msg = psprintf("%s at or near \"%s\"",
										   message, yytext);
	else
		syncrep_parse_error_msg = psprintf("%s at end of input",
										   message);
}

void
syncrep_scanner_init(const char *str, yyscan_t *yyscannerp)
{
	yyscan_t	yyscanner;
	struct syncrep_yy_extra_type *yyext = palloc0_object(struct syncrep_yy_extra_type);

	if (yylex_init(yyscannerp) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	yyscanner = *yyscannerp;

	yyset_extra(yyext, yyscanner);

	yy_scan_string(str, yyscanner);
}

void
syncrep_scanner_finish(yyscan_t yyscanner)
{
	pfree(yyextra);
	yylex_destroy(yyscanner);
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
