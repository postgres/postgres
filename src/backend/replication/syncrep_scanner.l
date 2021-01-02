%{
/*-------------------------------------------------------------------------
 *
 * syncrep_scanner.l
 *	  a lexical scanner for synchronous_standby_names
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;

static StringInfoData xdbuf;

/* LCOV_EXCL_START */

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="syncrep_yy"

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
				initStringInfo(&xdbuf);
				BEGIN(xd);
		}
<xd>{xddouble} {
				appendStringInfoChar(&xdbuf, '"');
		}
<xd>{xdinside} {
				appendStringInfoString(&xdbuf, yytext);
		}
<xd>{xdstop} {
				yylval.str = xdbuf.data;
				xdbuf.data = NULL;
				BEGIN(INITIAL);
				return NAME;
		}
<xd><<EOF>> {
				yyerror("unterminated quoted identifier");
				return JUNK;
		}

{identifier} {
				yylval.str = pstrdup(yytext);
				return NAME;
		}

{digit}+	{
				yylval.str = pstrdup(yytext);
				return NUM;
		}

"*"		{
				yylval.str = "*";
				return NAME;
		}

","			{ return ','; }
"("			{ return '('; }
")"			{ return ')'; }

.			{ return JUNK; }
%%

/* LCOV_EXCL_STOP */

/* Needs to be here for access to yytext */
void
syncrep_yyerror(const char *message)
{
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
syncrep_scanner_init(const char *str)
{
	Size		slen = strlen(str);
	char	   *scanbuf;

	/*
	 * Might be left over after ereport()
	 */
	if (YY_CURRENT_BUFFER)
		yy_delete_buffer(YY_CURRENT_BUFFER);

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	scanbuf = (char *) palloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	/* Make sure we start in proper state */
	BEGIN(INITIAL);
}

void
syncrep_scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	scanbufhandle = NULL;
}
