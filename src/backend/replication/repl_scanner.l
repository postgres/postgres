%top{
/*-------------------------------------------------------------------------
 *
 * repl_scanner.l
 *	  a lexical scanner for the replication commands
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/repl_scanner.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "parser/scansup.h"

/*
 * NB: include repl_gram.h only AFTER including walsender_private.h, because
 * walsender_private includes headers that define XLogRecPtr.
 */
#include "replication/walsender_private.h"
#include "repl_gram.h"
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

struct replication_yy_extra_type
{
	/* Pushed-back token (we only handle one) */
	int			repl_pushed_back_token;

	/* Work area for collecting literals */
	StringInfoData litbuf;
};

static void startlit(yyscan_t yyscanner);
static char *litbufdup(yyscan_t yyscanner);
static void addlit(char *ytext, int yleng, yyscan_t yyscanner);
static void addlitchar(unsigned char ychar, yyscan_t yyscanner);

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
%option prefix="replication_yy"
%option extra-type="struct replication_yy_extra_type *"

/*
 * Exclusive states:
 *  <xd> delimited identifiers (double-quoted identifiers)
 *  <xq> standard single-quoted strings
 */
%x xd
%x xq

space			[ \t\n\r\f\v]

quote			'
quotestop		{quote}

/* Extended quote
 * xqdouble implements embedded quote, ''''
 */
xqstart			{quote}
xqdouble		{quote}{quote}
xqinside		[^']+

/* Double quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

digit			[0-9]
hexdigit		[0-9A-Fa-f]

ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

%%

%{
	/* This code is inserted at the start of replication_yylex() */

	/* If we have a pushed-back token, return that. */
	if (yyextra->repl_pushed_back_token)
	{
		int			result = yyextra->repl_pushed_back_token;

		yyextra->repl_pushed_back_token = 0;
		return result;
	}
%}

BASE_BACKUP			{ return K_BASE_BACKUP; }
IDENTIFY_SYSTEM		{ return K_IDENTIFY_SYSTEM; }
READ_REPLICATION_SLOT	{ return K_READ_REPLICATION_SLOT; }
SHOW		{ return K_SHOW; }
TIMELINE			{ return K_TIMELINE; }
START_REPLICATION	{ return K_START_REPLICATION; }
CREATE_REPLICATION_SLOT		{ return K_CREATE_REPLICATION_SLOT; }
DROP_REPLICATION_SLOT		{ return K_DROP_REPLICATION_SLOT; }
ALTER_REPLICATION_SLOT		{ return K_ALTER_REPLICATION_SLOT; }
TIMELINE_HISTORY	{ return K_TIMELINE_HISTORY; }
PHYSICAL			{ return K_PHYSICAL; }
RESERVE_WAL			{ return K_RESERVE_WAL; }
LOGICAL				{ return K_LOGICAL; }
SLOT				{ return K_SLOT; }
TEMPORARY			{ return K_TEMPORARY; }
TWO_PHASE			{ return K_TWO_PHASE; }
EXPORT_SNAPSHOT		{ return K_EXPORT_SNAPSHOT; }
NOEXPORT_SNAPSHOT	{ return K_NOEXPORT_SNAPSHOT; }
USE_SNAPSHOT		{ return K_USE_SNAPSHOT; }
WAIT				{ return K_WAIT; }
UPLOAD_MANIFEST		{ return K_UPLOAD_MANIFEST; }

{space}+		{ /* do nothing */ }

{digit}+		{
					yylval->uintval = strtoul(yytext, NULL, 10);
					return UCONST;
				}

{hexdigit}+\/{hexdigit}+		{
					uint32	hi,
							lo;
					if (sscanf(yytext, "%X/%08X", &hi, &lo) != 2)
						replication_yyerror(NULL, yyscanner, "invalid streaming start location");
					yylval->recptr = ((uint64) hi) << 32 | lo;
					return RECPTR;
				}

{xqstart}		{
					BEGIN(xq);
					startlit(yyscanner);
				}

<xq>{quotestop}	{
					yyless(1);
					BEGIN(INITIAL);
					yylval->str = litbufdup(yyscanner);
					return SCONST;
				}

<xq>{xqdouble}	{
					addlitchar('\'', yyscanner);
				}

<xq>{xqinside}  {
					addlit(yytext, yyleng, yyscanner);
				}

{xdstart}		{
					BEGIN(xd);
					startlit(yyscanner);
				}

<xd>{xdstop}	{
					int			len;

					yyless(1);
					BEGIN(INITIAL);
					yylval->str = litbufdup(yyscanner);
					len = strlen(yylval->str);
					truncate_identifier(yylval->str, len, true);
					return IDENT;
				}

<xd>{xdinside}  {
					addlit(yytext, yyleng, yyscanner);
				}

{identifier}	{
					int			len = strlen(yytext);

					yylval->str = downcase_truncate_identifier(yytext, len, true);
					return IDENT;
				}

.				{
					/* Any char not recognized above is returned as itself */
					return yytext[0];
				}

<xq,xd><<EOF>>	{ replication_yyerror(NULL, yyscanner, "unterminated quoted string"); }


<<EOF>>			{
					yyterminate();
				}

%%

/* LCOV_EXCL_STOP */

/* see scan.l */
#undef yyextra
#define yyextra (((struct yyguts_t *) yyscanner)->yyextra_r)

static void
startlit(yyscan_t yyscanner)
{
	initStringInfo(&yyextra->litbuf);
}

static char *
litbufdup(yyscan_t yyscanner)
{
	return yyextra->litbuf.data;
}

static void
addlit(char *ytext, int yleng, yyscan_t yyscanner)
{
	appendBinaryStringInfo(&yyextra->litbuf, ytext, yleng);
}

static void
addlitchar(unsigned char ychar, yyscan_t yyscanner)
{
	appendStringInfoChar(&yyextra->litbuf, ychar);
}

/*
  * (The first argument is enforced by Bison to match the first argument of
  * yyparse(), but it is not used here.)
  */
void
replication_yyerror(Node **replication_parse_result_p, yyscan_t yyscanner, const char *message)
{
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg_internal("%s", message)));
}

void
replication_scanner_init(const char *str, yyscan_t *yyscannerp)
{
	yyscan_t	yyscanner;
	struct replication_yy_extra_type *yyext = palloc0_object(struct replication_yy_extra_type);

	if (yylex_init(yyscannerp) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	yyscanner = *yyscannerp;

	yyset_extra(yyext, yyscanner);

	yy_scan_string(str, yyscanner);
}

void
replication_scanner_finish(yyscan_t yyscanner)
{
	pfree(yyextra);
	yylex_destroy(yyscanner);
}

/*
 * Check to see if the first token of a command is a WalSender keyword.
 *
 * To keep repl_scanner.l minimal, we don't ask it to know every construct
 * that the core lexer knows.  Therefore, we daren't lex more than the
 * first token of a general SQL command.  That will usually look like an
 * IDENT token here, although some other cases are possible.
 */
bool
replication_scanner_is_replication_command(yyscan_t yyscanner)
{
	YYSTYPE		dummy;
	int			first_token = replication_yylex(&dummy, yyscanner);

	switch (first_token)
	{
		case K_IDENTIFY_SYSTEM:
		case K_BASE_BACKUP:
		case K_START_REPLICATION:
		case K_CREATE_REPLICATION_SLOT:
		case K_DROP_REPLICATION_SLOT:
		case K_ALTER_REPLICATION_SLOT:
		case K_READ_REPLICATION_SLOT:
		case K_TIMELINE_HISTORY:
		case K_UPLOAD_MANIFEST:
		case K_SHOW:
			/* Yes; push back the first token so we can parse later. */
			yyextra->repl_pushed_back_token = first_token;
			return true;
		default:
			/* Nope; we don't bother to push back the token. */
			return false;
	}
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
