%{
/*-------------------------------------------------------------------------
 *
 * scan.l
 *	  lexical scanner for POSTGRES
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/scan.l,v 1.64 2000/02/19 04:17:25 tgl Exp $
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

#else /* !FLEX_SCANNER */

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
/*
 * OK, here is a short description of lex/flex rules behavior.
 * The longest pattern which matches an input string is always chosen.
 * For equal-length patterns, the first occurring in the rules list is chosen.
 * INITIAL is the starting state, to which all non-conditional rules apply.
 * Exclusive states change parsing rules while the state is active.  When in
 * an exclusive state, only those rules defined for that state apply.
 *
 * We use exclusive states for quoted strings, extended comments,
 * and to eliminate parsing troubles for numeric strings.
 * Exclusive states:
 *  <xb> binary numeric string - thomas 1997-11-16
 *  <xc> extended C-style comments - tgl 1997-07-12
 *  <xd> delimited identifiers (double-quoted identifiers) - tgl 1997-10-27
 *  <xh> hexadecimal numeric string - thomas 1997-11-16
 *  <xq> quoted strings - tgl 1997-07-30
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
xbinside		[^']+
xbcat			{quote}{whitespace_with_newline}{quote}

/* Hexadecimal number
 */
xhstart			[xX]{quote}
xhstop			{quote}
xhinside		[^']+
xhcat			{quote}{whitespace_with_newline}{quote}

/* Extended quote
 * xqdouble implements SQL92 embedded quote
 * xqcat allows strings to cross input lines
 * Note: reduction of '' and \ sequences to output text is done in scanstr(),
 * not by rules here.  But we do get rid of xqcat sequences here.
 */
quote			'
xqstart			{quote}
xqstop			{quote}
xqdouble		{quote}{quote}
xqinside		[^\\']+
xqliteral		[\\](.|\n)
xqcat			{quote}{whitespace_with_newline}{quote}

/* Delimited quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xdinside		[^"]+

/* C-style comments
 * Ignored by the scanner and parser.
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * The tricky part here is to get lex to recognize a string starting with
 * slash-star as a comment, when interpreting it as an operator would produce
 * a longer match --- remember lex will prefer a longer match!  So, we have
 * to provide a special rule for xcline (a complete comment that could
 * otherwise look like an operator), as well as append {op_and_self}* to
 * xcstart so that it matches at least as much as {operator} would.
 * Then the tie-breaker (first matching rule of same length) wins.
 * There is still a problem if someone writes, eg, slash-star-star-slash-plus.
 * It'll be taken as an xcstart, rather than xcline and an operator as one
 * could wish.  I don't see any way around that given lex's behavior;
 * that someone will just have to write a space after the comment.
 */
xcline			\/\*{op_and_self}*\*\/
xcstart			\/\*{op_and_self}*
xcstop			\*+\/
xcinside		([^*]+)|(\*+[^/])

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
real			((({digit}*\.{digit}+)|({digit}+\.{digit}*)|({digit}+))([Ee][-+]?{digit}+))

param			\${integer}

/*
 * In order to make the world safe for Windows and Mac clients as well as
 * Unix ones, we accept either \n or \r as a newline.  A DOS-style \r\n
 * sequence will be seen as two successive newlines, but that doesn't cause
 * any problems.  SQL92-style comments, which start with -- and extend to the
 * next newline, are treated as equivalent to a single whitespace character.
 *
 * NOTE a fine point: if there is no newline following --, we will absorb
 * everything to the end of the input as a comment.  This is correct.  Older
 * versions of Postgres failed to recognize -- as a comment if the input
 * did not end with a newline.
 *
 * XXX perhaps \f (formfeed) should be treated as a newline as well?
 */

space			[ \t\n\r\f]
horiz_space		[ \t\f]
newline			[\n\r]
non_newline		[^\n\r]

comment			(("--"|"//"){non_newline}*)

whitespace		({space}|{comment})

/*
 * SQL92 requires at least one newline in the whitespace separating
 * string literals that are to be concatenated.  Silly, but who are we
 * to argue?  Note that {whitespace_with_newline} should not have * after
 * it, whereas {whitespace} should generally have a * after it...
 */

horiz_whitespace	({horiz_space}|{comment})
whitespace_with_newline	({horiz_whitespace}*{newline}{whitespace}*)

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
 * Note that xcline must appear before xcstart, which must appear before
 *  operator, as explained above!  Also whitespace (comment) must appear
 *  before operator.
 */

%%
{whitespace}	{ /* ignore */ }

{xcline}		{ /* ignore */ }

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
					/* ignore */
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
					/* ignore */
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
					if (strcmp((char*)yytext, "!=") == 0)
						yylval.str = pstrdup("<>"); /* compatibility */
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
