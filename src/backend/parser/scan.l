%{
/*-------------------------------------------------------------------------
 *
 * scan.l
 *	  lexical scanner for POSTGRES
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/scan.l,v 1.62 2000/01/23 01:27:35 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <unistd.h>
#ifndef __linux__
#include <math.h>
#endif
#include <errno.h>

#include "postgres.h"

#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parse.h"
#include "parser/gramparse.h"
#include "parser/keywords.h"
#include "parser/scansup.h"
#include "utils/builtins.h"

extern char *parseString;
static char *parseCh;

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

/* set up my input handler --- need one flavor for flex, one for lex */
#if defined(FLEX_SCANNER)
#define YY_NO_UNPUT
static int myinput(char* buf, int max);
#undef YY_INPUT
#define YY_INPUT(buf,result,max) {result = myinput(buf,max);}
#else
#undef input
int input();
#undef unput
void unput(char);
#endif /* FLEX_SCANNER */

extern YYSTYPE yylval;

/*
 * literalbuf is used to accumulate literal values when multiple rules
 * are needed to parse a single literal.  Call startlit to reset buffer
 * to empty, addlit to add text.  Note that the buffer is palloc'd and
 * starts life afresh on every parse cycle.
 */
static char	   *literalbuf;		/* expandable buffer */
static int		literallen;		/* actual current length */
static int		literalalloc;	/* current allocated buffer size */

#define startlit()  (literalbuf[0] = '\0', literallen = 0)
static void addlit(char *ytext, int yleng);

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
 * Note: reduction of '' and \ sequences to output text is done in scanstr(),
 * not by rules here.
 */
quote			'
xqstart			{quote}
xqstop			{quote}
xqdouble		{quote}{quote}
xqinside		[^\\']*
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
letter			[\200-\377_A-Za-z]
letter_or_digit	[\200-\377_A-Za-z0-9]

identifier		{letter}{letter_or_digit}*

typecast		"::"

self			[,()\[\].;$\:\+\-\*\/\%\^\<\>\=\|]
op_and_self		[\~\!\@\#\^\&\|\`\?\$\:\+\-\*\/\%\<\>\=]
operator		{op_and_self}+

/* we no longer allow unary minus in numbers. 
 * instead we pass it separately to parser. there it gets
 * coerced via doNegate() -- Leon aug 20 1999 
 */

integer			{digit}+
decimal			(({digit}*\.{digit}+)|({digit}+\.{digit}*))
real				((({digit}*\.{digit}+)|({digit}+\.{digit}*)|({digit}+))([Ee][-+]?{digit}+))

param			\${integer}

comment			("--"|"//").*

space			[ \t\n\r\f]
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
					startlit();
				}
<xb>{xbstop}	{
					char* endptr;

					BEGIN(INITIAL);
					errno = 0;
					yylval.ival = strtol(literalbuf, &endptr, 2);
					if (*endptr != '\0' || errno == ERANGE)
						elog(ERROR, "Bad binary integer input '%s'",
							 literalbuf);
					return ICONST;
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					addlit(yytext, yyleng);
				}
<xh>{xhcat}		|
<xb>{xbcat}		{
				}

{xhstart}		{
					BEGIN(xh);
					startlit();
				}
<xh>{xhstop}	{
					char* endptr;

					BEGIN(INITIAL);
					errno = 0;
					yylval.ival = strtol(literalbuf, &endptr, 16);
					if (*endptr != '\0' || errno == ERANGE)
						elog(ERROR, "Bad hexadecimal integer input '%s'",
							 literalbuf);
					return ICONST;
				}

{xqstart}		{
					BEGIN(xq);
					startlit();
				}
<xq>{xqstop}	{
					BEGIN(INITIAL);
					yylval.str = scanstr(literalbuf);
					return SCONST;
				}
<xq>{xqdouble}	|
<xq>{xqinside}	|
<xq>{xqliteral} {
					addlit(yytext, yyleng);
				}
<xq>{xqcat}		{
				}


{xdstart}		{
					BEGIN(xd);
					startlit();
				}
<xd>{xdstop}	{
					BEGIN(INITIAL);
					yylval.str = pstrdup(literalbuf);
					return IDENT;
				}
<xd>{xdinside}	{
					addlit(yytext, yyleng);
				}

{typecast}		{ return TYPECAST; }

{self}			{ return yytext[0]; }

{operator}		{
					if (strcmp((char*)yytext,"!=") == 0)
						yylval.str = pstrdup("<>"); /* compatability */
					else
						yylval.str = pstrdup((char*)yytext);
					return Op;
				}
{param}			{
					yylval.ival = atoi((char*)&yytext[1]);
					return PARAM;
				}


{integer}		{
					char* endptr;

					errno = 0;
					yylval.ival = strtol((char *)yytext,&endptr,10);
					if (*endptr != '\0' || errno == ERANGE)
					{
						errno = 0;
#if 0
						yylval.dval = strtod(((char *)yytext),&endptr);
						if (*endptr != '\0' || errno == ERANGE)
							elog(ERROR,"Bad integer input '%s'",yytext);
						CheckFloat8Val(yylval.dval);
						elog(NOTICE,"Integer input '%s' is out of range; promoted to float", yytext);
						return FCONST;
#endif
						yylval.str = pstrdup((char*)yytext);
						return SCONST;
					}
					return ICONST;
				}
{decimal}		{
					char* endptr;

					if (strlen((char *)yytext) <= 17)
					{
						errno = 0;
						yylval.dval = strtod((char *)yytext,&endptr);
						if (*endptr != '\0' || errno == ERANGE)
							elog(ERROR,"Bad float input '%s'",yytext);
						CheckFloat8Val(yylval.dval);
						return FCONST;
					}
					yylval.str = pstrdup((char*)yytext);
					return SCONST;
				}
{real}			{
					char* endptr;

					errno = 0;
					yylval.dval = strtod((char *)yytext,&endptr);
					if (*endptr != '\0' || errno == ERANGE)
						elog(ERROR,"Bad float input '%s'",yytext);
					CheckFloat8Val(yylval.dval);
					return FCONST;
				}


{identifier}	{
					int i;
					ScanKeyword		*keyword;

					for(i = 0; yytext[i]; i++)
						if (isascii((unsigned char)yytext[i]) &&
							isupper(yytext[i]))
							yytext[i] = tolower(yytext[i]);
					if (i >= NAMEDATALEN)
                    {
                        elog(NOTICE, "identifier \"%s\" will be truncated to \"%.*s\"",
                             yytext, NAMEDATALEN-1, yytext);
						yytext[NAMEDATALEN-1] = '\0';
                    }
					keyword = ScanKeywordLookup((char*)yytext);
					if (keyword != NULL) {
						return keyword->value;
					}
					else
					{
						yylval.str = pstrdup((char*)yytext);
						return IDENT;
					}
				}
{space}			{ /* ignore */ }

{other}			{ return yytext[0]; }

%%

void yyerror(const char * message)
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

	/* initialize literal buffer to a reasonable but expansible size */
	literalalloc = 128;
	literalbuf = (char *) palloc(literalalloc);
	startlit();

#if defined(FLEX_SCANNER)
	if (YY_CURRENT_BUFFER)
		yy_flush_buffer(YY_CURRENT_BUFFER);
#endif /* FLEX_SCANNER */
	BEGIN INITIAL;
}

static void
addlit(char *ytext, int yleng)
{
	/* enlarge buffer if needed */
	if ((literallen+yleng) >= literalalloc)
	{
		do {
			literalalloc *= 2;
		} while ((literallen+yleng) >= literalalloc);
		literalbuf = (char *) repalloc(literalbuf, literalalloc);
	}
	/* append data --- note we assume ytext is null-terminated */
	memcpy(literalbuf+literallen, ytext, yleng+1);
	literallen += yleng;
}

#if !defined(FLEX_SCANNER)

/* get lex input from a string instead of from stdin */
int
input()
{
	if (parseCh == NULL)
		parseCh = parseString;
	if (*parseCh == '\0')
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
static int
myinput(char* buf, int max)
{
	int len;

	if (parseCh == NULL)
		parseCh = parseString;
	len = strlen(parseCh);		/* remaining data available */
	/* Note: this code used to think that flex wants a null-terminated
	 * string.  It does NOT, and returning 1 less character than it asks
	 * for will cause failure under the right boundary conditions.  So
	 * shut up and fill the buffer to the limit, you hear?
	 */
	if (len > max)
		len = max;
	if (len > 0)
		memcpy(buf, parseCh, len);
	parseCh += len;
	return len;
}

#endif /* FLEX_SCANNER */
