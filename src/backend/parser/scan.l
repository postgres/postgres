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
 *	  $Header: /cvsroot/pgsql/src/backend/parser/scan.l,v 1.40 1998/06/16 07:29:27 momjian Exp $
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
#include "parse.h"
#include "utils/builtins.h"

extern char *parseString;
static char *parseCh;

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

#if defined(FLEX_SCANNER)
/* MAX_PARSE_BUFFER is defined in miscadmin.h */
#define YYLMAX MAX_PARSE_BUFFER
#define YY_NO_UNPUT
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
 * When in an exclusive condition, only those rules defined for that condition apply.
 *
 * Exclusive states change parsing rules while the state is active.
 * There are exclusive states for quoted strings, extended comments,
 *  and to eliminate parsing troubles for numeric strings.
 * Exclusive states:
 *  <xb> binary numeric string - thomas 1997-11-16
 *  <xc> extended C-style comments - tgl 1997-07-12
 *  <xd> delimited identifiers (double-quoted identifiers) - tgl 1997-10-27
 *  <xh> hexadecimal numeric string - thomas 1997-11-16
 *  <xm> numeric strings with embedded minus sign - tgl 1997-09-05
 *  <xq> quoted strings - tgl 1997-07-30
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * So, when in condition <xc>, only strings which would terminate the
 *  "extended comment" trigger any action other than "ignore".
 * Be sure to match _any_ candidate comment, including those with appended
 *	operator-like symbols. - thomas 1997-07-14
 */

%x xb
%x xc
%x xd
%x xh
%x xm
%x xq

/* Binary number
 */
xbstart			[bB]{quote}
xbstop			{quote}
xbinside		[^']*
xbcat			{quote}{space}*\n{space}*{quote}

/* Hexadecimal number
 */
xhstart			[xX]{quote}
xhstop			{quote}
xhinside		[^']*
xhcat			{quote}{space}*\n{space}*{quote}

/* Extended quote
 * xqdouble implements SQL92 embedded quote
 * xqcat allows strings to cross input lines
 */
quote			'
xqstart			{quote}
xqstop			{quote}
xqdouble		{quote}{quote}
xqinside		[^\\']*
xqembedded		"\\'"
xqliteral		[\\](.|\n)
xqcat			{quote}{space}*\n{space}*{quote}

/* Delimited quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xdinside		[^"]*

/* Comments
 * Ignored by the scanner and parser.
 */
xcline			[\/][\*].*[\*][\/]{space}*\n*
xcstart			[\/][\*]{op_and_self}*
xcstop			{op_and_self}*[\*][\/]({space}*|\n)
xcinside		[^*]*
xcstar			[^/]

digit			[0-9]
number			[-+.0-9Ee]
letter			[\200-\377_A-Za-z]
letter_or_digit	[\200-\377_A-Za-z0-9]

identifier		{letter}{letter_or_digit}*

typecast		"::"

self			[,()\[\].;$\:\+\-\*\/\<\>\=\|]
op_and_self		[\~\!\@\#\%\^\&\|\`\?\$\:\+\-\*\/\<\>\=]
operator		{op_and_self}+

xmstop			-

integer			[\-]?{digit}+
/*
real			[\-]?{digit}+\.{digit}+([Ee][-+]?{digit}+)?
*/
real			[\-]?(((({digit}*\.{digit}+)|({digit}+\.{digit}*))([Ee][-+]?{digit}+)?)|({digit}+[Ee][-+]?{digit}+))

param			\${integer}

comment			("--"|"//").*\n

space			[ \t\n\f]
other			.

/* DO NOT PUT ANY COMMENTS IN THE FOLLOWING SECTION.
 * AT&T lex does not properly handle C-style comments in this second lex block.
 * So, put comments here. tgl - 1997-09-08
 *
 * Quoted strings must allow some special characters such as single-quote
 *  and newline.
 * Embedded single-quotes are implemented both in the SQL/92-standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string. - thomas 1997-09-24
 */

%%
{comment}		{ /* ignore */ }

{xcline}		{ /* ignore */ }

<xc>{xcstar}	|
{xcstart}		{ BEGIN(xc); }

<xc>{xcstop}	{ BEGIN(INITIAL); }

<xc>{xcinside}	{ /* ignore */ }

{xbstart}		{
					BEGIN(xb);
					llen = 0;
					*literal = '\0';
				}
<xb>{xbstop}	{
					char* endptr;

					BEGIN(INITIAL);
					errno = 0;
					yylval.ival = strtol((char *)literal,&endptr,2);
					if (*endptr != '\0' || errno == ERANGE)
						elog(ERROR,"Bad binary integer input '%s'",literal);
					return (ICONST);
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					if ((llen+yyleng) > (MAX_PARSE_BUFFER - 1))
						elog(ERROR,"quoted string parse buffer of %d chars exceeded",MAX_PARSE_BUFFER);
					memcpy(literal+llen, yytext, yyleng+1);
					llen += yyleng;
				}
<xh>{xhcat}		|
<xb>{xbcat}		{
				}

{xhstart}		{
					BEGIN(xh);
					llen = 0;
					*literal = '\0';
				}
<xh>{xhstop}	{
					char* endptr;

					BEGIN(INITIAL);
					errno = 0;
					yylval.ival = strtol((char *)literal,&endptr,16);
					if (*endptr != '\0' || errno == ERANGE)
						elog(ERROR,"Bad hexadecimal integer input '%s'",literal);
					return (ICONST);
				}

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
						elog(ERROR,"quoted string parse buffer of %d chars exceeded",MAX_PARSE_BUFFER);
					memcpy(literal+llen, yytext, yyleng+1);
					llen += yyleng;
				}
<xq>{xqembedded} {
					if ((llen+yyleng-1) > (MAX_PARSE_BUFFER - 1))
						elog(ERROR,"quoted string parse buffer of %d chars exceeded",MAX_PARSE_BUFFER);
					memcpy(literal+llen, yytext, yyleng+1);
					*(literal+llen) = '\'';
					llen += yyleng;
				}

<xq>{xqliteral} {
					if ((llen+yyleng-1) > (MAX_PARSE_BUFFER - 1))
						elog(ERROR,"quoted string parse buffer of %d chars exceeded",MAX_PARSE_BUFFER);
					memcpy(literal+llen, yytext, yyleng+1);
					llen += yyleng;
				}
<xq>{xqcat}		{
				}


{xdstart}		{
					BEGIN(xd);
					llen = 0;
					*literal = '\0';
				}
<xd>{xdstop}	{
					BEGIN(INITIAL);
					yylval.str = pstrdup(literal);
					return (IDENT);
				}
<xd>{xdinside}	{
					if ((llen+yyleng) > (MAX_PARSE_BUFFER - 1))
						elog(ERROR,"quoted string parse buffer of %d chars exceeded",MAX_PARSE_BUFFER);
					memcpy(literal+llen, yytext, yyleng+1);
					llen += yyleng;
				}


<xm>{space}*	{ /* ignore */ }
<xm>{xmstop}	{
					BEGIN(INITIAL);
					return (yytext[0]);
				}


{typecast}		{ return TYPECAST; }

{self}/{space}*-[\.0-9]	{
					BEGIN(xm);
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


{identifier}/{space}*-{number}	{
					int i;
					ScanKeyword		*keyword;

					BEGIN(xm);
					for(i = 0; yytext[i]; i++)
						if (isascii((unsigned char)yytext[i]) &&
							isupper(yytext[i]))
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
{integer}/{space}*-{number}	{
					char* endptr;

					BEGIN(xm);
					errno = 0;
					yylval.ival = strtol((char *)yytext,&endptr,10);
					if (*endptr != '\0' || errno == ERANGE)
					{
						errno = 0;
						yylval.dval = strtod(((char *)yytext),&endptr);
						if (*endptr != '\0' || errno == ERANGE)
							elog(ERROR,"Bad integer input '%s'",yytext);
						CheckFloat8Val(yylval.dval);
						elog(NOTICE,"Integer input '%s' is out of range; promoted to float", yytext);
						return (FCONST);
					}
					return (ICONST);
				}
{real}/{space}*-{number} {
					char* endptr;

					BEGIN(xm);
					errno = 0;
					yylval.dval = strtod(((char *)yytext),&endptr);
					if (*endptr != '\0' || errno == ERANGE)
						elog(ERROR,"Bad float8 input '%s'",yytext);
					CheckFloat8Val(yylval.dval);
					return (FCONST);
				}
{integer}		{
					char* endptr;

					errno = 0;
					yylval.ival = strtol((char *)yytext,&endptr,10);
					if (*endptr != '\0' || errno == ERANGE)
					{
						errno = 0;
						yylval.dval = strtod(((char *)yytext),&endptr);
						if (*endptr != '\0' || errno == ERANGE)
							elog(ERROR,"Bad integer input '%s'",yytext);
						CheckFloat8Val(yylval.dval);
						elog(NOTICE,"Integer input '%s' is out of range; promoted to float", yytext);
						return (FCONST);
					}
					return (ICONST);
				}
{real}			{
					char* endptr;

					errno = 0;
					yylval.dval = strtod((char *)yytext,&endptr);
					if (*endptr != '\0' || errno == ERANGE)
						elog(ERROR,"Bad float input '%s'",yytext);
					CheckFloat8Val(yylval.dval);
					return (FCONST);
				}


{identifier}	{
					int i;
					ScanKeyword		*keyword;

					for(i = 0; yytext[i]; i++)
						if (isascii((unsigned char)yytext[i]) &&
							isupper(yytext[i]))
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
	elog(ERROR, "parser: %s at or near \"%s\"", message, yytext);
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

