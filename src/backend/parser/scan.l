%{
/*-------------------------------------------------------------------------
 *
 * scan.l--
 *	  lexical scanner for POSTGRES
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/scan.l,v 1.21 1997/09/13 03:12:55 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <unistd.h>
#ifndef __linux__
#include <math.h>
#else
#include <stdlib.h>
#endif /* __linux__ */
#include <string.h>
#include <errno.h>

#include "postgres.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "parser/gramparse.h"
#include "parser/keywords.h"
#include "parser/scansup.h"
#include "parser/sysfunc.h"
#include "parse.h"
#include "utils/builtins.h"

extern char *parseString;
extern char *parseCh;

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

#if defined(FLEX_SCANNER)
/* MAX_PARSE_BUFFER is defined in miscadmin.h */
#define YYLMAX MAX_PARSE_BUFFER
extern int myinput(char* buf, int max);
#undef YY_INPUT
#define YY_INPUT(buf,result,max) {result = myinput(buf,max);}
#else
#undef input
int input();
#undef unput
void unput(char);
#endif /* FLEX_SCANNER */

extern YYSTYPE yylval;

int llen;
char *ScanString;
char literal[MAX_PARSE_BUFFER];

%}
	/* OK, here is a short description of lex/flex rules behavior.
	 * The longest pattern which matches an input string is always chosen.
	 * For equal-length patterns, the first occurring in the rules list is chosen.
	 * INITIAL is the starting condition, to which all non-conditional rules apply.
	 * When in an exclusive condition, only those rules defined for that condition apply.
	 *
	 * Exclusive states change parsing rules while the state is active.
	 * There are exclusive states for quoted strings, extended comments,
	 *  and to eliminate parsing troubles for numeric strings.
	 * Exclusive states:
	 *  <xc> extended C-style comments - tgl 1997-07-12
	 *  <xq> quoted strings - tgl 1997-07-30
	 *  <xm> numeric strings with embedded minus sign - tgl 1997-09-05
	 *
	 * The "extended comment" syntax closely resembles allowable operator syntax.
	 * So, when in condition <xc>, only strings which would terminate the
	 *  "extended comment" trigger any action other than "ignore".
	 * Be sure to match _any_ candidate comment, including those with appended
	 *	operator-like symbols. - thomas 1997-07-14
	 */

%x xc
%x xq
%x xm

	/* We used to allow double-quoted strings, but SQL doesn't so we won't either */
quote			'
xqstart			{quote}
xqstop			{quote}
xqdouble		{quote}{quote}
xqinside		[^\']*
xqliteral		[\\].

xcline			[\/][\*].*[\*][\/]{space}*\n*
xcstart			[\/][\*]{op_and_self}*
xcstop			{op_and_self}*[\*][\/]({space}*|\n)
xcinside		[^*]*
xcstar			[^/]

digit			[0-9]
number			[-+.0-9Ee]
letter			[_A-Za-z]
letter_or_digit [_A-Za-z0-9]

sysfunc			SYS_{letter}{letter_or_digit}*

identifier		{letter}{letter_or_digit}*

typecast		"::"

self			[,()\[\].;$\:\+\-\*\/\<\>\=\|]
op_and_self		[\~\!\@\#\%\^\&\|\`\?\$\:\+\-\*\/\<\>\=]
operator		{op_and_self}+

xminteger		{integer}/-
xmreal			{real}/{space}*-{digit}
xmstop			-

integer			-?{digit}+
real			-?{digit}+\.{digit}+([Ee][-+]?{digit}+)?

param			\${integer}

comment			("--"|"//").*\n

space			[ \t\n\f]
other			.

/* DO NOT PUT ANY COMMENTS IN THE FOLLOWING SECTION.
 * AT&T lex does not properly handle C-style comments in this second lex block.
 * So, put comments here. tgl - 1997-09-08
 */

%%
{comment}		{ /* ignore */ }

{xcline}		{ /* ignore */ }

<xc>{xcstar}	|
{xcstart}		{ BEGIN(xc); }

<xc>{xcstop}	{ BEGIN(INITIAL); }

<xc>{xcinside}	{ /* ignore */ }

{xqstart}		{
					BEGIN(xq);
					llen = 0;
					*literal = '\0';
				}
<xq>{xqstop}	{
					BEGIN(INITIAL);
					yylval.str = pstrdup(scanstr(literal));
					return (SCONST);
				}
<xq>{xqdouble}	|
<xq>{xqinside}	{
					if ((llen+yyleng) > (MAX_PARSE_BUFFER - 1))
						elog(WARN,"quoted string parse buffer of %d chars exceeded",MAX_PARSE_BUFFER);
					memcpy(literal+llen, yytext, yyleng+1);
					llen += yyleng;
				}
<xq>{xqliteral} {
					if ((llen+yyleng-1) > (MAX_PARSE_BUFFER - 1))
						elog(WARN,"quoted string parse buffer of %d chars exceeded",MAX_PARSE_BUFFER);
					memcpy(literal+llen, yytext+1, yyleng);
					llen += yyleng-1;
				}

<xm>{space}*	{ /* ignore */ }
<xm>{xmstop}	{
					BEGIN(INITIAL);
					return (yytext[0]);
				}

{sysfunc}		{
						yylval.str = pstrdup(SystemFunctionHandler((char *)yytext));
						return (SCONST);
				}

{typecast}		{ return TYPECAST; }

{self}/-[\.0-9]	{
					return (yytext[0]);
				}
{self}			{ 	return (yytext[0]); }
{operator}/-[\.0-9]	{
					yylval.str = pstrdup((char*)yytext);
					return (Op);
				}
{operator}		{
					if (strcmp((char*)yytext,"!=") == 0)
						yylval.str = pstrdup("<>"); /* compatability */
					else
						yylval.str = pstrdup((char*)yytext);
					return (Op);
				}
{param}			{
					yylval.ival = atoi((char*)&yytext[1]);
					return (PARAM);
				}

{integer}/{space}*-{number}	{
					BEGIN(xm);
					ScanString = pstrdup((char*)yytext);
					yylval.ival = atoi((char*)yytext);
					return (ICONST);
				}
{real}/{space}*-{number} {
					char* endptr;
					BEGIN(xm);
					errno = 0;
					ScanString = pstrdup((char*)yytext);
					yylval.dval = strtod(((char *)yytext),&endptr);
					if (*endptr != '\0' || errno == ERANGE)
					elog(WARN,"\tBad float8 input format\n");
					CheckFloat8Val(yylval.dval);
					return (FCONST);
				}
{integer}		{
					ScanString = pstrdup((char*)yytext);
					yylval.ival = atoi((char*)yytext);
					return (ICONST);
				}
{real}			{
					char* endptr;

					errno = 0;
					ScanString = pstrdup((char*)yytext);
					yylval.dval = strtod(((char *)yytext),&endptr);
					if (*endptr != '\0' || errno == ERANGE)
						elog(WARN,"\tBad float8 input format\n");
					CheckFloat8Val(yylval.dval);
					return (FCONST);
				}
{identifier}	{
					int i;
					ScanKeyword		*keyword;

					for(i = strlen(yytext); i >= 0; i--)
						if (isupper(yytext[i]))
							yytext[i] = tolower(yytext[i]);

					keyword = ScanKeywordLookup((char*)yytext);
					if (keyword != NULL) {
						return (keyword->value);
					}
					else
					{
						yylval.str = pstrdup((char*)yytext);
						return (IDENT);
					}
				}
{space}			{ /* ignore */ }

{other}			{ return (yytext[0]); }

%%

void yyerror(char message[])
{
	elog(WARN, "parser: %s at or near \"%s\"", message, yytext);
}

int yywrap()
{
	return(1);
}

/*
 init_io:
	called by postgres before any actual parsing is done
*/
void
init_io()
{
	/* it's important to set this to NULL
	   because input()/myinput() checks the non-nullness of parseCh
	   to know when to pass the string to lex/flex */
	parseCh = NULL;
#if defined(FLEX_SCANNER)
	if (YY_CURRENT_BUFFER)
		yy_flush_buffer(YY_CURRENT_BUFFER);
#endif /* FLEX_SCANNER */
	BEGIN INITIAL;
}

#if !defined(FLEX_SCANNER)
/* get lex input from a string instead of from stdin */
int
input()
{
	if (parseCh == NULL)
	{
		parseCh = parseString;
		return(*parseCh++);
	}
	else if (*parseCh == '\0')
		return(0);
	else
		return(*parseCh++);
}

/* undo lex input from a string instead of from stdin */
void
unput(char c)
{
	if (parseCh == NULL)
		elog(FATAL, "Unput() failed.\n");
	else if (c != 0)
		*--parseCh = c;
}
#endif /* !defined(FLEX_SCANNER) */

#ifdef FLEX_SCANNER
/* input routine for flex to read input from a string instead of a file */
int
myinput(char* buf, int max)
{
	int len, copylen;

	if (parseCh == NULL)
	{
		len = strlen(parseString);
		if (len >= max)
			copylen = max - 1;
		else
			copylen = len;
		if (copylen > 0)
			memcpy(buf, parseString, copylen);
		buf[copylen] = '\0';
		parseCh = parseString;
		return copylen;
	}
	else
		return 0; /* end of string */
}
#endif /* FLEX_SCANNER */

