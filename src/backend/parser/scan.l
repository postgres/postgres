%{
/*-------------------------------------------------------------------------
 *
 * scan.l
 *	  lexical scanner for POSTGRES
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/scan.l,v 1.86 2001/02/03 20:13:05 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#ifndef __linux__
#include <math.h>
#endif
#include <errno.h>

#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/gramparse.h"
#include "parser/keywords.h"
#include "parser/parse.h"
#include "parser/scansup.h"
#include "utils/builtins.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

extern char *parseString;
static char *parseCh;

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

/* set up my input handler --- need one flavor for flex, one for lex */
#if defined(FLEX_SCANNER)

#define YY_NEVER_INTERACTIVE 1
#define YY_NO_UNPUT
static int myinput(char* buf, int max);
#undef YY_INPUT
#define YY_INPUT(buf,result,max) {result = myinput(buf,max);}

/* No reason to constrain amount of data slurped per myinput() call. */
#define YY_READ_BUF_SIZE 16777216

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#define fprintf(file, fmt, msg)  elog(FATAL, "%s", (msg))

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

static int		xcdepth = 0;	/* depth of nesting in slash-star comments */

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
 *  <xbit> bit string literal
 *  <xc> extended C-style comments - thomas 1997-07-12
 *  <xd> delimited identifiers (double-quoted identifiers) - thomas 1997-10-27
 *  <xh> hexadecimal numeric string - thomas 1997-11-16
 *  <xq> quoted strings - thomas 1997-07-30
 */

%x xbit
%x xc
%x xd
%x xh
%x xq

/* Bit string
 */
xbitstart		[bB]{quote}
xbitstop		{quote}
xbitinside		[^']*
xbitcat			{quote}{whitespace_with_newline}{quote}

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
xddouble		{dquote}{dquote}
xdinside		[^"]+

/* C-style comments
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * The tricky part here is to get lex to recognize a string starting with
 * slash-star as a comment, when interpreting it as an operator would produce
 * a longer match --- remember lex will prefer a longer match!  Also, if we
 * have something like plus-slash-star, lex will think this is a 3-character
 * operator whereas we want to see it as a + operator and a comment start.
 * The solution is two-fold:
 * 1. append {op_chars}* to xcstart so that it matches as much text as
 *    {operator} would. Then the tie-breaker (first matching rule of same
 *    length) ensures xcstart wins.  We put back the extra stuff with yyless()
 *    in case it contains a star-slash that should terminate the comment.
 * 2. In the operator rule, check for slash-star within the operator, and
 *    if found throw it back with yyless().  This handles the plus-slash-star
 *    problem.
 * SQL92-style comments, which start with dash-dash, have similar interactions
 * with the operator rule.
 */
xcstart			\/\*{op_chars}*
xcstop			\*+\/
xcinside		[^*/]+

digit			[0-9]
letter			[\200-\377_A-Za-z]
letter_or_digit	[\200-\377_A-Za-z0-9]

identifier		{letter}{letter_or_digit}*

typecast		"::"

/*
 * "self" is the set of chars that should be returned as single-character
 * tokens.  "op_chars" is the set of chars that can make up "Op" tokens,
 * which can be one or more characters long (but if a single-char token
 * appears in the "self" set, it is not to be returned as an Op).  Note
 * that the sets overlap, but each has some chars that are not in the other.
 *
 * If you change either set, adjust the character lists appearing in the
 * rule for "operator"!
 */
self			[,()\[\].;$\:\+\-\*\/\%\^\<\>\=]
op_chars		[\~\!\@\#\^\&\|\`\?\$\+\-\*\/\%\<\>\=]
operator		{op_chars}+

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

comment			("--"{non_newline}*)

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
 * So, put comments here. thomas - 1997-09-08
 *
 * Quoted strings must allow some special characters such as single-quote
 *  and newline.
 * Embedded single-quotes are implemented both in the SQL92-standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string. - thomas 1997-09-24
 * Note that xcstart must appear before operator, as explained above!
 *  Also whitespace (comment) must appear before operator.
 */

%%
{whitespace}	{ /* ignore */ }

{xcstart}		{
					xcdepth = 0;
					BEGIN(xc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

<xc>{xcstart}	{
					xcdepth++;
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

<xc>{xcstop}	{
					if (xcdepth <= 0)
						BEGIN(INITIAL);
					else
						xcdepth--;
				}

<xc>{xcinside}	{ /* ignore */ }

<xc>{op_chars}	{ /* ignore */ }

<xc><<EOF>>		{ elog(ERROR, "Unterminated /* comment"); }

{xbitstart}		{
					BEGIN(xbit);
					startlit();
					addlit("b", 1);
				}
<xbit>{xbitstop}	{
					BEGIN(INITIAL);
					if (literalbuf[strspn(literalbuf + 1, "01") + 1] != '\0')
						elog(ERROR, "invalid bit string input: '%s'",
							 literalbuf);
					yylval.str = pstrdup(literalbuf);
					return BITCONST;
				}
<xh>{xhinside}	|
<xbit>{xbitinside}	{
					addlit(yytext, yyleng);
				}
<xh>{xhcat}		|
<xbit>{xbitcat}		{
					/* ignore */
				}
<xbit><<EOF>>		{ elog(ERROR, "unterminated bit string literal"); }

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
<xh><<EOF>>		{ elog(ERROR, "Unterminated hexadecimal integer"); }

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
<xq><<EOF>>		{ elog(ERROR, "Unterminated quoted string"); }


{xdstart}		{
					BEGIN(xd);
					startlit();
				}
<xd>{xdstop}	{
					BEGIN(INITIAL);
					if (strlen(literalbuf) == 0)
						elog(ERROR, "zero-length delimited identifier");
					if (strlen(literalbuf) >= NAMEDATALEN)
					{
#ifdef MULTIBYTE
						int len;
						len = pg_mbcliplen(literalbuf,strlen(literalbuf),NAMEDATALEN-1);
						elog(NOTICE, "identifier \"%s\" will be truncated to \"%.*s\"",
							 literalbuf, len, literalbuf);
						literalbuf[len] = '\0';
#else
						elog(NOTICE, "identifier \"%s\" will be truncated to \"%.*s\"",
							 literalbuf, NAMEDATALEN-1, literalbuf);
						literalbuf[NAMEDATALEN-1] = '\0';
#endif
					}
					yylval.str = pstrdup(literalbuf);
					return IDENT;
				}
<xd>{xddouble} {
					addlit(yytext, yyleng-1);
				}
<xd>{xdinside}	{
					addlit(yytext, yyleng);
				}
<xd><<EOF>>		{ elog(ERROR, "Unterminated quoted identifier"); }

{typecast}		{ return TYPECAST; }

{self}			{ return yytext[0]; }

{operator}		{
					/*
					 * Check for embedded slash-star or dash-dash; those
					 * are comment starts, so operator must stop there.
					 * Note that slash-star or dash-dash at the first
					 * character will match a prior rule, not this one.
					 */
					int		nchars = yyleng;
					char   *slashstar = strstr((char*)yytext, "/*");
					char   *dashdash = strstr((char*)yytext, "--");

					if (slashstar && dashdash)
					{
						/* if both appear, take the first one */
						if (slashstar > dashdash)
							slashstar = dashdash;
					}
					else if (!slashstar)
						slashstar = dashdash;
					if (slashstar)
						nchars = slashstar - ((char*)yytext);

					/*
					 * For SQL92 compatibility, '+' and '-' cannot be the
					 * last char of a multi-char operator unless the operator
					 * contains chars that are not in SQL92 operators.
					 * The idea is to lex '=-' as two operators, but not
					 * to forbid operator names like '?-' that could not be
					 * sequences of SQL92 operators.
					 */
					while (nchars > 1 &&
						   (yytext[nchars-1] == '+' ||
							yytext[nchars-1] == '-'))
					{
						int		ic;

						for (ic = nchars-2; ic >= 0; ic--)
						{
							if (strchr("~!@#^&|`?$%", yytext[ic]))
								break;
						}
						if (ic >= 0)
							break; /* found a char that makes it OK */
						nchars--; /* else remove the +/-, and check again */
					}

					if (nchars < yyleng)
					{
						/* Strip the unwanted chars from the token */
						yyless(nchars);
						/*
						 * If what we have left is only one char, and it's
						 * one of the characters matching "self", then
						 * return it as a character token the same way
						 * that the "self" rule would have.
						 */
						if (nchars == 1 &&
							strchr(",()[].;$:+-*/%^<>=", yytext[0]))
							return yytext[0];
					}

					/* Convert "!=" operator to "<>" for compatibility */
					if (strcmp((char*)yytext, "!=") == 0)
						yylval.str = pstrdup("<>");
					else
						yylval.str = pstrdup((char*)yytext);
					return Op;
				}

{param}			{
					yylval.ival = atol((char*)&yytext[1]);
					return PARAM;
				}

{integer}		{
					char* endptr;

					errno = 0;
					yylval.ival = strtol((char *)yytext, &endptr, 10);
					if (*endptr != '\0' || errno == ERANGE)
					{
						/* integer too large, treat it as a float */
						yylval.str = pstrdup((char*)yytext);
						return FCONST;
					}
					return ICONST;
				}
{decimal}		{
					yylval.str = pstrdup((char*)yytext);
					return FCONST;
				}
{real}			{
					yylval.str = pstrdup((char*)yytext);
					return FCONST;
				}


{identifier}	{
					int i;
					ScanKeyword		*keyword;

					for(i = 0; yytext[i]; i++)
						if (isupper((unsigned char) yytext[i]))
							yytext[i] = tolower((unsigned char) yytext[i]);
					if (i >= NAMEDATALEN)
                    {
#ifdef MULTIBYTE
						int len;
						len = pg_mbcliplen(yytext,i,NAMEDATALEN-1);
                        elog(NOTICE, "identifier \"%s\" will be truncated to \"%.*s\"",
                             yytext, len, yytext);
						yytext[len] = '\0';
#else
                        elog(NOTICE, "identifier \"%s\" will be truncated to \"%.*s\"",
                             yytext, NAMEDATALEN-1, yytext);
						yytext[NAMEDATALEN-1] = '\0';
#endif
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

void
yyerror(const char *message)
{
	elog(ERROR, "parser: %s at or near \"%s\"", message, yytext);
}

int
yywrap(void)
{
	return(1);
}

/*
 scanner_init:
	called by postgres before any actual parsing is done
*/
void
scanner_init(void)
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
