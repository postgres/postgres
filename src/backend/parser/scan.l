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
 *	  $Header: /cvsroot/pgsql/src/backend/parser/scan.l,v 1.19 1997/09/08 03:20:04 momjian Exp $
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

int CurScanPosition(void);
int DefaultStartPosition;
int CheckStartPosition;

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
char literal[MAX_PARSE_BUFFER];

%}
	/* OK, here is a short description of lex/flex rules behavior.
	 * The longest pattern which matches an input string is always chosen.
	 * For equal-length patterns, the first occurring in the rules list is chosen.
	 * INITIAL is the starting condition, to which all non-conditional rules apply.
	 * <xc> is an exclusive condition to allow embedded C-style comments.
	 * When in an exclusive condition, only those rules defined for that condition apply.
	 * So, when in condition <xc>, only strings which would terminate the "extended comment"
	 *	trigger any action other than "ignore".
	 * The "extended comment" syntax closely resembles allowable operator syntax.
	 * Therefore, be sure to match _any_ candidate comment, including those with appended
	 *	operator-like symbols. - thomas 1997-07-14
	 */

	/* define an exclusive condition to allow extended C-style comments - tgl 1997-07-12 */
%x xc
	/* define an exclusive condition for quoted strings - tgl 1997-07-30 */
%x xq

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
letter			[_A-Za-z]
letter_or_digit [_A-Za-z0-9]

sysfunc			SYS_{letter}{letter_or_digit}*

identifier		{letter}{letter_or_digit}*

typecast		"::"

self			[,()\[\].;$\:\+\-\*\/\<\>\=\|]
selfm			{self}[\-][\.0-9]

op_and_self		[\~\!\@\#\%\^\&\|\`\?\$\:\+\-\*\/\<\>\=]

operator		{op_and_self}+
operatorm		{op_and_self}+[\-][\.0-9]

integer			-?{digit}+
real			-?{digit}+\.{digit}+([Ee][-+]?{digit}+)?

param			\${integer}

comment			"--".*\n
comment2		"//".*\n

space			[ \t\n\f]
other			.

%%
{sysfunc}		{
						yylval.str = pstrdup(SystemFunctionHandler((char *)yytext));
						return (SCONST);
				}

{comment}		{ /* ignore */	}
{comment2}		{ /* ignore */	}

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

{typecast}		{ return TYPECAST; }

{selfm}			{
					yyless(yyleng-2);
					return (yytext[0]);
				}
{self}			{ 	return (yytext[0]); }

{operatorm}		{
					yyless(yyleng-2);
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
{integer}		{
					yylval.ival = atoi((char*)yytext);
					return (ICONST);
				}
{real}			{
					char* endptr;

					errno = 0;
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
						if ( keyword->value == DEFAULT )
						{
							DefaultStartPosition = CurScanPosition () + yyleng + 1;
							printf( "default offset is %d\n", DefaultStartPosition);
						}
						else if ( keyword->value == CHECK )
						{
							CheckStartPosition = CurScanPosition () + yyleng + 1;
							printf( "check offset is %d\n", CheckStartPosition);
						}
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
	elog(WARN, "parser: %s at or near \"%s\"\n", message, yytext);
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

int
CurScanPosition(void)
{
	return (parseCh - parseString - yyleng);
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

int
CurScanPosition(void)
{
	printf( "current position is %d\n", yy_c_buf_p - yy_current_buffer->yy_ch_buf - yyleng);
	return (yy_c_buf_p - yy_current_buffer->yy_ch_buf - yyleng);
}

#endif /* FLEX_SCANNER */

