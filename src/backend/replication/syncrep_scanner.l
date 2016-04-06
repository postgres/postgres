%{
/*-------------------------------------------------------------------------
 *
 * syncrep_scanner.l
 *	  a lexical scanner for synchronous_standby_names
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep_scanner.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "lib/stringinfo.h"

/*
  * flex emits a yy_fatal_error() function that it calls in response to
  * critical errors like malloc failure, file I/O errors, and detection of
  * internal inconsistency.  That function prints a message and calls exit().
  * Mutate it to instead call ereport(FATAL), which terminates this process.
  *
  * The process that causes this fatal error should be terminated.
  * Otherwise it has to abandon the new setting value of
  * synchronous_standby_names and keep running with the previous one
  * while the other processes switch to the new one.
  * This inconsistency of the setting that each process is based on
  * can cause a serious problem. Though it's basically not good idea to
  * use FATAL here because it can take down the postmaster,
  * we should do that in order to avoid such an inconsistency.
  */
#undef fprintf
#define fprintf(file, fmt, msg) syncrep_flex_fatal(fmt, msg)

static void
syncrep_flex_fatal(const char *fmt, const char *msg)
{
	ereport(FATAL, (errmsg_internal("%s", msg)));
}

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;

static StringInfoData xdbuf;

%}

%option 8bit
%option never-interactive
%option nounput
%option noinput
%option noyywrap
%option warn
%option prefix="syncrep_yy"

/*
 * <xd> delimited identifiers (double-quoted identifiers)
 */
%x xd

space		[ \t\n\r\f\v]

undquoted_start	[^ ,\(\)\"]
undquoted_cont		[^ ,\(\)]
undquoted_name    {undquoted_start}{undquoted_cont}*
dquoted_name		[^\"]+

/* Double-quoted string */
dquote		\"
xdstart		{dquote}
xddouble		{dquote}{dquote}
xdstop		{dquote}
xdinside		{dquoted_name}

%%
{space}+		{ /* ignore */ }
{xdstart}	{
				initStringInfo(&xdbuf);
				BEGIN(xd);
		}
<xd>{xddouble} {
				appendStringInfoChar(&xdbuf, '\"');
		}
<xd>{xdinside} {
				appendStringInfoString(&xdbuf, yytext);
		}
<xd>{xdstop} {
				yylval.str = pstrdup(xdbuf.data);
				pfree(xdbuf.data);
				BEGIN(INITIAL);
				return NAME;
		}
","			{ return ','; }
"("			{ return '('; }
")"			{ return ')'; }
[1-9][0-9]*	{
				yylval.str = pstrdup(yytext);
				return NUM;
		}
{undquoted_name} {
				yylval.str = pstrdup(yytext);
				return NAME;
		}
%%

void
yyerror(const char *message)
{
	ereport(IsUnderPostmaster ? DEBUG2 : LOG,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("%s at or near \"%s\"", message, yytext)));
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
}

void
syncrep_scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	scanbufhandle = NULL;
}
