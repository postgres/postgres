%top{
/*-------------------------------------------------------------------------
 *
 * pgc.l
 *	  lexical scanner for ecpg
 *
 * This is a modified version of src/backend/parser/scan.l
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/ecpg/preproc/pgc.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <limits.h>

#include "extern.h"
#include "preproc.h"
}

%{
extern YYSTYPE base_yylval;

static int		xcdepth = 0;	/* depth of nesting in slash-star comments */
static char	   *dolqstart = NULL;	/* current $foo$ quote start string */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;

/*
 * literalbuf is used to accumulate literal values when multiple rules
 * are needed to parse a single literal.  Call startlit to reset buffer
 * to empty, addlit to add text.  Note that the buffer is permanently
 * malloc'd to the largest size needed so far in the current run.
 */
static char	   *literalbuf = NULL;		/* expandable buffer */
static int		literallen;				/* actual current length */
static int		literalalloc;			/* current allocated buffer size */

/* Used for detecting global state together with braces_open */
static int		parenths_open;

/* Used to tell parse_include() whether the command was #include or #include_next */
static bool		include_next;

#define startlit()	(literalbuf[0] = '\0', literallen = 0)
static void addlit(char *ytext, int yleng);
static void addlitchar (unsigned char);
static void parse_include (void);
static bool ecpg_isspace(char ch);
static bool isdefine(void);
static bool isinformixdefine(void);

char *token_start;
static int state_before;

struct _yy_buffer
{
	YY_BUFFER_STATE		buffer;
	long				lineno;
	char			   *filename;
	struct _yy_buffer  *next;
} *yy_buffer = NULL;

static char *old;

#define MAX_NESTED_IF 128
static short preproc_tos;
static short ifcond;
static struct _if_value
{
	short condition;
	short else_branch;
} stacked_if_value[MAX_NESTED_IF];

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option noyywrap
%option warn
%option prefix="base_yy"

%option yylineno

%x C SQL incl def def_ident undef

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
 *	<xb> bit string literal
 *	<xcc> extended C-style comments in C
 *	<xcsql> extended C-style comments in SQL
 *	<xd> delimited identifiers (double-quoted identifiers) - thomas 1997-10-27
 *	<xh> hexadecimal numeric string - thomas 1997-11-16
 *	<xq> standard quoted strings - thomas 1997-07-30
 *	<xqc> standard quoted strings in C - michael
 *	<xe> extended quoted strings (support backslash escape sequences)
 *	<xn> national character quoted strings
 *  <xdolq> $foo$ quoted strings
 *  <xui> quoted identifier with Unicode escapes
 *  <xus> quoted string with Unicode escapes
 */

%x xb
%x xcc
%x xcsql
%x xd
%x xdc
%x xh
%x xe
%x xn
%x xq
%x xqc
%x xdolq
%x xcond
%x xskip
%x xui
%x xus

/* Bit string
 */
xbstart			[bB]{quote}
xbinside		[^']*

/* Hexadecimal number */
xhstart			[xX]{quote}
xhinside		[^']*

/* National character */
xnstart			[nN]{quote}

/* Quoted string that allows backslash escapes */
xestart			[eE]{quote}
xeinside		[^\\']+
xeescape		[\\][^0-7]
xeoctesc		[\\][0-7]{1,3}
xehexesc		[\\]x[0-9A-Fa-f]{1,2}
xeunicode		[\\](u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8})

/* C version of hex number */
xch				0[xX][0-9A-Fa-f]*

/* Extended quote
 * xqdouble implements embedded quote, ''''
 */
xqstart			{quote}
xqdouble		{quote}{quote}
xqcquote		[\\]{quote}
xqinside		[^']+

/* $foo$ style quotes ("dollar quoting")
 * The quoted string starts with $foo$ where "foo" is an optional string
 * in the form of an identifier, except that it may not contain "$",
 * and extends to the first occurrence of an identical string.
 * There is *no* processing of the quoted text.
 *
 * {dolqfailed} is an error rule to avoid scanner backup when {dolqdelim}
 * fails to match its trailing "$".
 */
dolq_start		[A-Za-z\200-\377_]
dolq_cont		[A-Za-z\200-\377_0-9]
dolqdelim		\$({dolq_start}{dolq_cont}*)?\$
dolqfailed		\${dolq_start}{dolq_cont}*
dolqinside		[^$]+

/* Double quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

/* Unicode escapes */
/* (The ecpg scanner is not backup-free, so the fail rules in scan.l are not needed here, but could be added if desired.) */
uescape			[uU][eE][sS][cC][aA][pP][eE]{whitespace}*{quote}[^']{quote}

/* Quoted identifier with Unicode escapes */
xuistart		[uU]&{dquote}
xuistop			{dquote}({whitespace}*{uescape})?

/* Quoted string with Unicode escapes */
xusstart		[uU]&{quote}
xusstop			{quote}({whitespace}*{uescape})?

/* special stuff for C strings */
xdcqq			\\\\
xdcqdq			\\\"
xdcother		[^"]
xdcinside		({xdcqq}|{xdcqdq}|{xdcother})

/* C-style comments
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * The tricky part here is to get lex to recognize a string starting with
 * slash-star as a comment, when interpreting it as an operator would produce
 * a longer match --- remember lex will prefer a longer match!	Also, if we
 * have something like plus-slash-star, lex will think this is a 3-character
 * operator whereas we want to see it as a + operator and a comment start.
 * The solution is two-fold:
 * 1. append {op_chars}* to xcstart so that it matches as much text as
 *	  {operator} would. Then the tie-breaker (first matching rule of same
 *	  length) ensures xcstart wins.  We put back the extra stuff with yyless()
 *	  in case it contains a star-slash that should terminate the comment.
 * 2. In the operator rule, check for slash-star within the operator, and
 *	  if found throw it back with yyless().  This handles the plus-slash-star
 *	  problem.
 * Dash-dash comments have similar interactions with the operator rule.
 */
xcstart			\/\*{op_chars}*
xcstop			\*+\/
xcinside		[^*/]+

digit			[0-9]
ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

array			({ident_cont}|{whitespace}|[\[\]\+\-\*\%\/\(\)\>\.])*

/* Assorted special-case operators and operator-like tokens */
typecast		"::"
dot_dot			\.\.
colon_equals	":="
equals_greater	"=>"
less_equals		"<="
greater_equals	">="
less_greater	"<>"
not_equals		"!="

/*
 * "self" is the set of chars that should be returned as single-character
 * tokens.	"op_chars" is the set of chars that can make up "Op" tokens,
 * which can be one or more characters long (but if a single-char token
 * appears in the "self" set, it is not to be returned as an Op).  Note
 * that the sets overlap, but each has some chars that are not in the other.
 *
 * If you change either set, adjust the character lists appearing in the
 * rule for "operator"!
 */
self			[,()\[\].;\:\+\-\*\/\%\^\<\>\=]
op_chars		[\~\!\@\#\^\&\|\`\?\+\-\*\/\%\<\>\=]
operator		{op_chars}+

/* we no longer allow unary minus in numbers.
 * instead we pass it separately to parser. there it gets
 * coerced via doNegate() -- Leon aug 20 1999
 *
 * {realfail1} and {realfail2} are added to prevent the need for scanner
 * backup when the {real} rule fails to match completely.
 */

integer			{digit}+
decimal			(({digit}*\.{digit}+)|({digit}+\.{digit}*))
real			({integer}|{decimal})[Ee][-+]?{digit}+
realfail1		({integer}|{decimal})[Ee]
realfail2		({integer}|{decimal})[Ee][-+]

param			\${integer}

/*
 * In order to make the world safe for Windows and Mac clients as well as
 * Unix ones, we accept either \n or \r as a newline.  A DOS-style \r\n
 * sequence will be seen as two successive newlines, but that doesn't cause
 * any problems.  SQL-style comments, which start with -- and extend to the
 * next newline, are treated as equivalent to a single whitespace character.
 *
 * NOTE a fine point: if there is no newline following --, we will absorb
 * everything to the end of the input as a comment.  This is correct.  Older
 * versions of Postgres failed to recognize -- as a comment if the input
 * did not end with a newline.
 *
 * XXX perhaps \f (formfeed) should be treated as a newline as well?
 *
 * XXX if you change the set of whitespace characters, fix ecpg_isspace()
 * to agree.
 */

ccomment		"//".*\n

space			[ \t\n\r\f]
horiz_space		[ \t\f]
newline			[\n\r]
non_newline		[^\n\r]

comment			("--"{non_newline}*)

whitespace		({space}+|{comment})

/*
 * SQL requires at least one newline in the whitespace separating
 * string literals that are to be concatenated.  Silly, but who are we
 * to argue?  Note that {whitespace_with_newline} should not have * after
 * it, whereas {whitespace} should generally have a * after it...
 */

horiz_whitespace	({horiz_space}|{comment})
whitespace_with_newline ({horiz_whitespace}*{newline}{whitespace}*)

quote			'
quotestop		{quote}{whitespace}*
quotecontinue	{quote}{whitespace_with_newline}{quote}
quotefail		{quote}{whitespace}*"-"

/* special characters for other dbms */
/* we have to react differently in compat mode */
informix_special	[\$]

other			.

/* some stuff needed for ecpg */
exec			[eE][xX][eE][cC]
sql				[sS][qQ][lL]
define			[dD][eE][fF][iI][nN][eE]
include			[iI][nN][cC][lL][uU][dD][eE]
include_next	[iI][nN][cC][lL][uU][dD][eE]_[nN][eE][xX][tT]
import			[iI][mM][pP][oO][rR][tT]
undef			[uU][nN][dD][eE][fF]

if				[iI][fF]
ifdef			[iI][fF][dD][eE][fF]
ifndef			[iI][fF][nN][dD][eE][fF]
else			[eE][lL][sS][eE]
elif			[eE][lL][iI][fF]
endif			[eE][nN][dD][iI][fF]

struct			[sS][tT][rR][uU][cC][tT]

exec_sql		{exec}{space}*{sql}{space}*
ipdigit			({digit}|{digit}{digit}|{digit}{digit}{digit})
ip				{ipdigit}\.{ipdigit}\.{ipdigit}\.{ipdigit}

/* we might want to parse all cpp include files */
cppinclude		{space}*#{include}{space}*
cppinclude_next		{space}*#{include_next}{space}*

/* take care of cpp lines, they may also be continuated */
/* first a general line for all commands not starting with "i" */
/* and then the other commands starting with "i", we have to add these
 * separately because the cppline production would match on "include" too */
cppline			{space}*#([^i][A-Za-z]*|{if}|{ifdef}|{ifndef}|{import})((\/\*[^*/]*\*+\/)|.)*{newline}

/*
 * Dollar quoted strings are totally opaque, and no escaping is done on them.
 * Other quoted strings must allow some special characters such as single-quote
 *	and newline.
 * Embedded single-quotes are implemented both in the SQL standard
 *	style of two adjacent single quotes "''" and in the Postgres/Java style
 *	of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *	backslash is dropped from the string. - thomas 1997-09-24
 * Note that xcstart must appear before operator, as explained above!
 *	Also whitespace (comment) must appear before operator.
 */

%%

%{
		/* code to execute during start of each call of yylex() */
		token_start = NULL;
%}

<SQL>{whitespace}	{ /* ignore */ }

<C>{xcstart}		{
					token_start = yytext;
					state_before = YYSTATE;
					xcdepth = 0;
					BEGIN(xcc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
					fputs("/*", yyout);
				}
<SQL>{xcstart}		{
					token_start = yytext;
					state_before = YYSTATE;
					xcdepth = 0;
					BEGIN(xcsql);
					/* Put back any characters past slash-star; see above */
					yyless(2);
					fputs("/*", yyout);
				}
<xcc>{xcstart}	{ ECHO; }
<xcsql>{xcstart}	{
					xcdepth++;
					/* Put back any characters past slash-star; see above */
					yyless(2);
					fputs("/_*", yyout);
				}
<xcsql>{xcstop}	{
					if (xcdepth <= 0)
					{
						ECHO;
						BEGIN(state_before);
						token_start = NULL;
					}
					else
					{
						xcdepth--;
						fputs("*_/", yyout);
					}
				}
<xcc>{xcstop}	{
					ECHO;
					BEGIN(state_before);
					token_start = NULL;
				}
<xcc,xcsql>{xcinside}	{ ECHO; }
<xcc,xcsql>{op_chars}	{ ECHO; }
<xcc,xcsql>\*+		{ ECHO; }

<xcc,xcsql><<EOF>>		{ mmfatal(PARSE_ERROR, "unterminated /* comment"); }

<SQL>{xbstart}	{
					token_start = yytext;
					BEGIN(xb);
					startlit();
					addlitchar('b');
				}
<xb>{quotestop} |
<xb>{quotefail}	{
					yyless(1);
					BEGIN(SQL);
					if (literalbuf[strspn(literalbuf, "01") + 1] != '\0')
						mmerror(PARSE_ERROR, ET_ERROR, "invalid bit string literal");
					base_yylval.str = mm_strdup(literalbuf);
					return BCONST;
				}

<xh>{xhinside}	|
<xb>{xbinside}	{ addlit(yytext, yyleng); }
<xh>{quotecontinue}	|
<xb>{quotecontinue}	{ /* ignore */ }
<xb><<EOF>>		{ mmfatal(PARSE_ERROR, "unterminated bit string literal"); }

<SQL>{xhstart}	{
					token_start = yytext;
					BEGIN(xh);
					startlit();
					addlitchar('x');
				}
<xh>{quotestop}	|
<xh>{quotefail}	{
				yyless(1);
				BEGIN(SQL);
				base_yylval.str = mm_strdup(literalbuf);
				return XCONST;
			}

<xh><<EOF>>		{ mmfatal(PARSE_ERROR, "unterminated hexadecimal string literal"); }
<SQL>{xnstart} {
				/* National character.
				 * Transfer it as-is to the backend.
				 */
				token_start = yytext;
				state_before = YYSTATE;
				BEGIN(xn);
				startlit();
			}
<C>{xqstart}	{
				token_start = yytext;
				state_before = YYSTATE;
				BEGIN(xqc);
				startlit();
			}
<SQL>{xqstart}	{
				token_start = yytext;
				state_before = YYSTATE;
				BEGIN(xq);
				startlit();
			}
<SQL>{xestart}	{
				token_start = yytext;
				state_before = YYSTATE;
				BEGIN(xe);
				startlit();
			}
<SQL>{xusstart}	{
				token_start = yytext;
				state_before = YYSTATE;
				BEGIN(xus);
				startlit();
				addlit(yytext, yyleng);
			}
<xq,xqc>{quotestop} |
<xq,xqc>{quotefail} {
				yyless(1);
				BEGIN(state_before);
				base_yylval.str = mm_strdup(literalbuf);
				return SCONST;
			}
<xe>{quotestop} |
<xe>{quotefail} {
				yyless(1);
				BEGIN(state_before);
				base_yylval.str = mm_strdup(literalbuf);
				return ECONST;
			}
<xn>{quotestop} |
<xn>{quotefail} {
				yyless(1);
				BEGIN(state_before);
				base_yylval.str = mm_strdup(literalbuf);
				return NCONST;
			}
<xus>{xusstop} {
				addlit(yytext, yyleng);
				BEGIN(state_before);
				base_yylval.str = mm_strdup(literalbuf);
				return UCONST;
			}
<xq,xe,xn,xus>{xqdouble}	{ addlitchar('\''); }
<xqc>{xqcquote}		{
				addlitchar('\\');
				addlitchar('\'');
			}
<xq,xqc,xn,xus>{xqinside}	{ addlit(yytext, yyleng); }
<xe>{xeinside}		{ addlit(yytext, yyleng); }
<xe>{xeunicode}		{ addlit(yytext, yyleng); }
<xe>{xeescape}		{ addlit(yytext, yyleng); }
<xe>{xeoctesc}		{ addlit(yytext, yyleng); }
<xe>{xehexesc}		{ addlit(yytext, yyleng); }
<xq,xqc,xe,xn,xus>{quotecontinue}	{ /* ignore */ }
<xe>.		{
			   /* This is only needed for \ just before EOF */
			   addlitchar(yytext[0]);
			}
<xq,xqc,xe,xn,xus><<EOF>>	{ mmfatal(PARSE_ERROR, "unterminated quoted string"); }
<SQL>{dolqfailed}	{
				/* throw back all but the initial "$" */
				yyless(1);
				/* and treat it as {other} */
				return yytext[0];
			}
<SQL>{dolqdelim} {
				token_start = yytext;
				if (dolqstart)
					free(dolqstart);
				dolqstart = mm_strdup(yytext);
				BEGIN(xdolq);
				startlit();
				addlit(yytext, yyleng);
			}
<xdolq>{dolqdelim} {
				if (strcmp(yytext, dolqstart) == 0)
				{
					addlit(yytext, yyleng);
					free(dolqstart);
					dolqstart = NULL;
					BEGIN(SQL);
					base_yylval.str = mm_strdup(literalbuf);
					return DOLCONST;
				}
				else
				{
					/*
					 * When we fail to match $...$ to dolqstart, transfer
					 * the $... part to the output, but put back the final
					 * $ for rescanning.  Consider $delim$...$junk$delim$
					 */
					addlit(yytext, yyleng-1);
					yyless(yyleng-1);
				}
			}
<xdolq>{dolqinside}	{ addlit(yytext, yyleng); }
<xdolq>{dolqfailed}	{ addlit(yytext, yyleng); }
<xdolq>{other}		{
				/* single quote or dollar sign */
				addlitchar(yytext[0]);
			}
<xdolq><<EOF>>		{ base_yyerror("unterminated dollar-quoted string"); }
<SQL>{xdstart}		{
						state_before = YYSTATE;
						BEGIN(xd);
						startlit();
					}
<SQL>{xuistart}		{
						state_before = YYSTATE;
						BEGIN(xui);
						startlit();
						addlit(yytext, yyleng);
					}
<xd>{xdstop}		{
						BEGIN(state_before);
						if (literallen == 0)
							mmerror(PARSE_ERROR, ET_ERROR, "zero-length delimited identifier");
						/* The backend will truncate the identifier here. We do not as it does not change the result. */
						base_yylval.str = mm_strdup(literalbuf);
						return CSTRING;
					}
<xdc>{xdstop}		{
						BEGIN(state_before);
						base_yylval.str = mm_strdup(literalbuf);
						return CSTRING;
					}
<xui>{xuistop}		{
						BEGIN(state_before);
						if (literallen == 2) /* "U&" */
							mmerror(PARSE_ERROR, ET_ERROR, "zero-length delimited identifier");
						/* The backend will truncate the identifier here. We do not as it does not change the result. */
						addlit(yytext, yyleng);
						base_yylval.str = mm_strdup(literalbuf);
						return UIDENT;
					}
<xd,xui>{xddouble}		{ addlitchar('"'); }
<xd,xui>{xdinside}		{ addlit(yytext, yyleng); }
<xd,xdc,xui><<EOF>>		{ mmfatal(PARSE_ERROR, "unterminated quoted identifier"); }
<C,SQL>{xdstart}	{
						state_before = YYSTATE;
						BEGIN(xdc);
						startlit();
					}
<xdc>{xdcinside}	{ addlit(yytext, yyleng); }
<SQL>{typecast}		{ return TYPECAST; }
<SQL>{dot_dot}		{ return DOT_DOT; }
<SQL>{colon_equals}	{ return COLON_EQUALS; }
<SQL>{equals_greater} { return EQUALS_GREATER; }
<SQL>{less_equals}	{ return LESS_EQUALS; }
<SQL>{greater_equals} { return GREATER_EQUALS; }
<SQL>{less_greater}	{ return NOT_EQUALS; }
<SQL>{not_equals}	{ return NOT_EQUALS; }
<SQL>{informix_special}	{
			  /* are we simulating Informix? */
				if (INFORMIX_MODE)
				{
					unput(':');
				}
				else
					return yytext[0];
				}
<SQL>{self}			{ /*
					   * We may find a ';' inside a structure
					   * definition in a TYPE or VAR statement.
					   * This is not an EOL marker.
					   */
					  if (yytext[0] == ';' && struct_level == 0)
						 BEGIN(C);
					  return yytext[0];
					}
<SQL>{operator}		{
						/*
						 * Check for embedded slash-star or dash-dash; those
						 * are comment starts, so operator must stop there.
						 * Note that slash-star or dash-dash at the first
						 * character will match a prior rule, not this one.
						 */
						int		nchars = yyleng;
						char   *slashstar = strstr(yytext, "/*");
						char   *dashdash = strstr(yytext, "--");

						if (slashstar && dashdash)
						{
							/* if both appear, take the first one */
							if (slashstar > dashdash)
								slashstar = dashdash;
						}
						else if (!slashstar)
							slashstar = dashdash;
						if (slashstar)
							nchars = slashstar - yytext;

						/*
						 * For SQL compatibility, '+' and '-' cannot be the
						 * last char of a multi-char operator unless the operator
						 * contains chars that are not in SQL operators.
						 * The idea is to lex '=-' as two operators, but not
						 * to forbid operator names like '?-' that could not be
						 * sequences of SQL operators.
						 */
						while (nchars > 1 &&
							   (yytext[nchars-1] == '+' ||
								yytext[nchars-1] == '-'))
						{
							int		ic;

							for (ic = nchars-2; ic >= 0; ic--)
							{
								if (strchr("~!@#^&|`?%", yytext[ic]))
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
								strchr(",()[].;:+-*/%^<>=", yytext[0]))
								return yytext[0];
						}

						base_yylval.str = mm_strdup(yytext);
						return Op;
					}
<SQL>{param}		{
						base_yylval.ival = atol(yytext+1);
						return PARAM;
					}
<C,SQL>{integer}	{
						long val;
						char* endptr;

						errno = 0;
						val = strtol((char *)yytext, &endptr,10);
						if (*endptr != '\0' || errno == ERANGE
#ifdef HAVE_LONG_INT_64
							/* if long > 32 bits, check for overflow of int4 */
							|| val != (long) ((int32) val)
#endif
							)
						{
							errno = 0;
							base_yylval.str = mm_strdup(yytext);
							return FCONST;
						}
						base_yylval.ival = val;
						return ICONST;
					}
<SQL>{ip}			{
						base_yylval.str = mm_strdup(yytext);
						return IP;
				}
<C,SQL>{decimal}	{
						base_yylval.str = mm_strdup(yytext);
						return FCONST;
			}
<C,SQL>{real}		{
						base_yylval.str = mm_strdup(yytext);
						return FCONST;
			}
<SQL>{realfail1}	{
						yyless(yyleng-1);
						base_yylval.str = mm_strdup(yytext);
						return FCONST;
					}
<SQL>{realfail2}	{
						yyless(yyleng-2);
						base_yylval.str = mm_strdup(yytext);
						return FCONST;
					}
<SQL>:{identifier}((("->"|\.){identifier})|(\[{array}\]))*	{
						base_yylval.str = mm_strdup(yytext+1);
						return(CVARIABLE);
					}
<SQL>{identifier}	{
						const ScanKeyword  *keyword;

						if (!isdefine())
						{
							/* Is it an SQL/ECPG keyword? */
							keyword = ScanECPGKeywordLookup(yytext);
							if (keyword != NULL)
								return keyword->value;

							/* Is it a C keyword? */
							keyword = ScanCKeywordLookup(yytext);
							if (keyword != NULL)
								return keyword->value;

							/*
							 * None of the above.  Return it as an identifier.
							 *
							 * The backend will attempt to truncate and case-fold
							 * the identifier, but I see no good reason for ecpg
							 * to do so; that's just another way that ecpg could get
							 * out of step with the backend.
							 */
							base_yylval.str = mm_strdup(yytext);
							return IDENT;
						}
					}
<SQL>{other}		{ return yytext[0]; }
<C>{exec_sql}		{ BEGIN(SQL); return SQL_START; }
<C>{informix_special}	{
						/* are we simulating Informix? */
						if (INFORMIX_MODE)
						{
							BEGIN(SQL);
							return SQL_START;
						}
						else
							return S_ANYTHING;
					 }
<C>{ccomment}		{ ECHO; }
<C>{xch}			{
						char* endptr;

						errno = 0;
						base_yylval.ival = strtoul((char *)yytext,&endptr,16);
						if (*endptr != '\0' || errno == ERANGE)
						{
							errno = 0;
							base_yylval.str = mm_strdup(yytext);
							return SCONST;
						}
						return ICONST;
					}
<C>{cppinclude}		{
						if (system_includes)
						{
							include_next = false;
							BEGIN(incl);
						}
						else
						{
							base_yylval.str = mm_strdup(yytext);
							return(CPP_LINE);
						}
					}
<C>{cppinclude_next}		{
						if (system_includes)
						{
							include_next = true;
							BEGIN(incl);
						}
						else
						{
							base_yylval.str = mm_strdup(yytext);
							return(CPP_LINE);
						}
					}
<C,SQL>{cppline}	{
						base_yylval.str = mm_strdup(yytext);
						return(CPP_LINE);
					}
<C>{identifier}		{
						const ScanKeyword		*keyword;

						/*
						 * Try to detect a function name:
						 * look for identifiers at the global scope
						 * keep the last identifier before the first '(' and '{' */
						if (braces_open == 0 && parenths_open == 0)
						{
							if (current_function)
								free(current_function);
							current_function = mm_strdup(yytext);
						}
						/* Informix uses SQL defines only in SQL space */
						/* however, some defines have to be taken care of for compatibility */
						if ((!INFORMIX_MODE || !isinformixdefine()) && !isdefine())
						{
							keyword = ScanCKeywordLookup(yytext);
							if (keyword != NULL)
								return keyword->value;
							else
							{
								base_yylval.str = mm_strdup(yytext);
								return IDENT;
							}
						}
					}
<C>{xcstop}			{ mmerror(PARSE_ERROR, ET_ERROR, "nested /* ... */ comments"); }
<C>":"				{ return(':'); }
<C>";"				{ return(';'); }
<C>","				{ return(','); }
<C>"*"				{ return('*'); }
<C>"%"				{ return('%'); }
<C>"/"				{ return('/'); }
<C>"+"				{ return('+'); }
<C>"-"				{ return('-'); }
<C>"("				{ parenths_open++; return('('); }
<C>")"				{ parenths_open--; return(')'); }
<C,xskip>{space}		{ ECHO; }
<C>\{				{ return('{'); }
<C>\}				{ return('}'); }
<C>\[				{ return('['); }
<C>\]				{ return(']'); }
<C>\=				{ return('='); }
<C>"->"				{ return(S_MEMBER); }
<C>">>"				{ return(S_RSHIFT); }
<C>"<<"				{ return(S_LSHIFT); }
<C>"||"				{ return(S_OR); }
<C>"&&"				{ return(S_AND); }
<C>"++"				{ return(S_INC); }
<C>"--"				{ return(S_DEC); }
<C>"=="				{ return(S_EQUAL); }
<C>"!="				{ return(S_NEQUAL); }
<C>"+="				{ return(S_ADD); }
<C>"-="				{ return(S_SUB); }
<C>"*="				{ return(S_MUL); }
<C>"/="				{ return(S_DIV); }
<C>"%="				{ return(S_MOD); }
<C>"->*"			{ return(S_MEMPOINT); }
<C>".*"				{ return(S_DOTPOINT); }
<C>{other}			{ return S_ANYTHING; }
<C>{exec_sql}{define}{space}*	{ BEGIN(def_ident); }
<C>{informix_special}{define}{space}*	{
						/* are we simulating Informix? */
						if (INFORMIX_MODE)
						{
							BEGIN(def_ident);
						}
						else
						{
							yyless(1);
							return (S_ANYTHING);
						}
					}
<C>{exec_sql}{undef}{space}*		{ BEGIN(undef); }
<C>{informix_special}{undef}{space}* {
						/* are we simulating Informix? */
						if (INFORMIX_MODE)
						{
							BEGIN(undef);
						}
						else
						{
							yyless(1);
							return (S_ANYTHING);
						}
					}
<undef>{identifier}{space}*";" {
					struct _defines *ptr, *ptr2 = NULL;
					int i;

					/*
					 *	Skip the ";" and trailing whitespace. Note that yytext
					 *	contains at least one non-space character plus the ";"
					 */
					for (i = strlen(yytext)-2;
						 i > 0 && ecpg_isspace(yytext[i]);
						 i-- )
						;
					yytext[i+1] = '\0';


					for (ptr = defines; ptr != NULL; ptr2 = ptr, ptr = ptr->next)
					{
						if (strcmp(yytext, ptr->old) == 0)
						{
							if (ptr2 == NULL)
								defines = ptr->next;
							else
								ptr2->next = ptr->next;
							free(ptr->new);
							free(ptr->old);
							free(ptr);
							break;
						}
					}

					BEGIN(C);
				}
<undef>{other}|\n {
						mmfatal(PARSE_ERROR, "missing identifier in EXEC SQL UNDEF command");
						yyterminate();
				}
<C>{exec_sql}{include}{space}*	{ BEGIN(incl); }
<C>{informix_special}{include}{space}* {
					  /* are we simulating Informix? */
					  if (INFORMIX_MODE)
					  {
						  BEGIN(incl);
					  }
					  else
					  {
						  yyless(1);
						  return (S_ANYTHING);
					  }
					}
<C,xskip>{exec_sql}{ifdef}{space}*	{ ifcond = TRUE; BEGIN(xcond); }
<C,xskip>{informix_special}{ifdef}{space}* {
					  /* are we simulating Informix? */
					  if (INFORMIX_MODE)
					  {
						  ifcond = TRUE;
						  BEGIN(xcond);
					  }
					  else
					  {
						  yyless(1);
						  return (S_ANYTHING);
					  }
					}
<C,xskip>{exec_sql}{ifndef}{space}* { ifcond = FALSE; BEGIN(xcond); }
<C,xskip>{informix_special}{ifndef}{space}* {
					  /* are we simulating Informix? */
					  if (INFORMIX_MODE)
					  {
						  ifcond = FALSE;
						  BEGIN(xcond);
					  }
					  else
					  {
						  yyless(1);
						  return (S_ANYTHING);
					  }
					}
<C,xskip>{exec_sql}{elif}{space}*	{	/* pop stack */
						if ( preproc_tos == 0 ) {
							mmfatal(PARSE_ERROR, "missing matching \"EXEC SQL IFDEF\" / \"EXEC SQL IFNDEF\"");
						}
						else if ( stacked_if_value[preproc_tos].else_branch )
							mmfatal(PARSE_ERROR, "missing \"EXEC SQL ENDIF;\"");
						else
							preproc_tos--;

						ifcond = TRUE; BEGIN(xcond);
					}
<C,xskip>{informix_special}{elif}{space}* {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (preproc_tos == 0)
							mmfatal(PARSE_ERROR, "missing matching \"EXEC SQL IFDEF\" / \"EXEC SQL IFNDEF\"");
						else if (stacked_if_value[preproc_tos].else_branch)
							mmfatal(PARSE_ERROR, "missing \"EXEC SQL ENDIF;\"");
						else
							preproc_tos--;

						ifcond = TRUE;
						BEGIN(xcond);
					}
					else
					{
						yyless(1);
						return (S_ANYTHING);
					}
				}

<C,xskip>{exec_sql}{else}{space}*";" {	/* only exec sql endif pops the stack, so take care of duplicated 'else' */
					if (stacked_if_value[preproc_tos].else_branch)
						mmfatal(PARSE_ERROR, "more than one EXEC SQL ELSE");
					else
					{
						stacked_if_value[preproc_tos].else_branch = TRUE;
						stacked_if_value[preproc_tos].condition =
							(stacked_if_value[preproc_tos-1].condition &&
							 !stacked_if_value[preproc_tos].condition);

						if (stacked_if_value[preproc_tos].condition)
							BEGIN(C);
						else
							BEGIN(xskip);
					}
				}
<C,xskip>{informix_special}{else}{space}*";"	{
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (stacked_if_value[preproc_tos].else_branch)
							mmfatal(PARSE_ERROR, "more than one EXEC SQL ELSE");
						else
						{
							stacked_if_value[preproc_tos].else_branch = TRUE;
							stacked_if_value[preproc_tos].condition =
							(stacked_if_value[preproc_tos-1].condition &&
							 !stacked_if_value[preproc_tos].condition);

							if (stacked_if_value[preproc_tos].condition)
								BEGIN(C);
							else
								BEGIN(xskip);
						}
					}
					else
					{
						yyless(1);
						return (S_ANYTHING);
					}
				}
<C,xskip>{exec_sql}{endif}{space}*";" {
					if (preproc_tos == 0)
						mmfatal(PARSE_ERROR, "unmatched EXEC SQL ENDIF");
					else
						preproc_tos--;

					if (stacked_if_value[preproc_tos].condition)
					   BEGIN(C);
					else
					   BEGIN(xskip);
				}
<C,xskip>{informix_special}{endif}{space}*";"	{
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (preproc_tos == 0)
							mmfatal(PARSE_ERROR, "unmatched EXEC SQL ENDIF");
						else
							preproc_tos--;

						if (stacked_if_value[preproc_tos].condition)
							BEGIN(C);
						else
							BEGIN(xskip);
					}
					else
					{
						yyless(1);
						return (S_ANYTHING);
					}
				}

<xskip>{other}		{ /* ignore */ }

<xcond>{identifier}{space}*";" {
					if (preproc_tos >= MAX_NESTED_IF-1)
						mmfatal(PARSE_ERROR, "too many nested EXEC SQL IFDEF conditions");
					else
					{
						struct _defines *defptr;
						unsigned int i;

						/*
						 *	Skip the ";" and trailing whitespace. Note that yytext
						 *	contains at least one non-space character plus the ";"
						 */
						for (i = strlen(yytext)-2;
							 i > 0 && ecpg_isspace(yytext[i]);
							 i-- )
							;
						yytext[i+1] = '\0';

						for (defptr = defines;
							 defptr != NULL && strcmp(yytext, defptr->old) != 0;
							 defptr = defptr->next);

						preproc_tos++;
						stacked_if_value[preproc_tos].else_branch = FALSE;
						stacked_if_value[preproc_tos].condition =
						(defptr ? ifcond : !ifcond) && stacked_if_value[preproc_tos-1].condition;
					}

					if (stacked_if_value[preproc_tos].condition)
						BEGIN(C);
					else
						BEGIN(xskip);
				}

<xcond>{other}|\n	{
				mmfatal(PARSE_ERROR, "missing identifier in EXEC SQL IFDEF command");
				yyterminate();
			}
<def_ident>{identifier} {
				old = mm_strdup(yytext);
				BEGIN(def);
				startlit();
			}
<def_ident>{other}|\n	{
				mmfatal(PARSE_ERROR, "missing identifier in EXEC SQL DEFINE command");
				yyterminate();
			}
<def>{space}*";"	{
						struct _defines *ptr, *this;

						for (ptr = defines; ptr != NULL; ptr = ptr->next)
						{
							 if (strcmp(old, ptr->old) == 0)
							 {
								free(ptr->new);
								ptr->new = mm_strdup(literalbuf);
							 }
						}
						if (ptr == NULL)
						{
							this = (struct _defines *) mm_alloc(sizeof(struct _defines));

							/* initial definition */
							this->old = old;
							this->new = mm_strdup(literalbuf);
							this->next = defines;
							this->used = NULL;
							defines = this;
						}

						BEGIN(C);
					}
<def>[^;]			{ addlit(yytext, yyleng); }
<incl>\<[^\>]+\>{space}*";"?		{	parse_include(); }
<incl>{dquote}{xdinside}{dquote}{space}*";"?	{	parse_include(); }
<incl>[^;\<\>\"]+";"		{ parse_include(); }
<incl>{other}|\n		{
					mmfatal(PARSE_ERROR, "syntax error in EXEC SQL INCLUDE command");
					yyterminate();
				}

<<EOF>>				{
					if (yy_buffer == NULL)
					{
						if ( preproc_tos > 0 )
						{
							preproc_tos = 0;
							mmfatal(PARSE_ERROR, "missing \"EXEC SQL ENDIF;\"");
						}
						yyterminate();
					}
					else
					{
						struct _yy_buffer *yb = yy_buffer;
						int i;
						struct _defines *ptr;

						for (ptr = defines; ptr; ptr = ptr->next)
							if (ptr->used == yy_buffer)
							{
								ptr->used = NULL;
								break;
							}

						if (yyin != NULL)
							fclose(yyin);

						yy_delete_buffer( YY_CURRENT_BUFFER );
						yy_switch_to_buffer(yy_buffer->buffer);

						yylineno = yy_buffer->lineno;

						/* We have to output the filename only if we change files here */
						i = strcmp(input_filename, yy_buffer->filename);

						free(input_filename);
						input_filename = yy_buffer->filename;

						yy_buffer = yy_buffer->next;
						free(yb);

						if (i != 0)
							output_line_number();

					}
				}
<INITIAL>{other}|\n	{ mmfatal(PARSE_ERROR, "internal error: unreachable state; please report this to <pgsql-bugs@postgresql.org>"); }
%%
void
lex_init(void)
{
	braces_open = 0;
	parenths_open = 0;
	current_function = NULL;

	preproc_tos = 0;
	yylineno = 1;
	ifcond = TRUE;
	stacked_if_value[preproc_tos].condition = ifcond;
	stacked_if_value[preproc_tos].else_branch = FALSE;

	/* initialize literal buffer to a reasonable but expansible size */
	if (literalbuf == NULL)
	{
		literalalloc = 1024;
		literalbuf = (char *) malloc(literalalloc);
	}
	startlit();

	BEGIN(C);
}

static void
addlit(char *ytext, int yleng)
{
	/* enlarge buffer if needed */
	if ((literallen+yleng) >= literalalloc)
	{
		do
			literalalloc *= 2;
		while ((literallen+yleng) >= literalalloc);
		literalbuf = (char *) realloc(literalbuf, literalalloc);
	}
	/* append new data, add trailing null */
	memcpy(literalbuf+literallen, ytext, yleng);
	literallen += yleng;
	literalbuf[literallen] = '\0';
}

static void
addlitchar(unsigned char ychar)
{
	/* enlarge buffer if needed */
	if ((literallen+1) >= literalalloc)
	{
		literalalloc *= 2;
		literalbuf = (char *) realloc(literalbuf, literalalloc);
	}
	/* append new data, add trailing null */
	literalbuf[literallen] = ychar;
	literallen += 1;
	literalbuf[literallen] = '\0';
}

static void
parse_include(void)
{
	/* got the include file name */
	struct _yy_buffer *yb;
	struct _include_path *ip;
	char inc_file[MAXPGPATH];
	unsigned int i;

	yb = mm_alloc(sizeof(struct _yy_buffer));

	yb->buffer =	YY_CURRENT_BUFFER;
	yb->lineno = yylineno;
	yb->filename = input_filename;
	yb->next = yy_buffer;

	yy_buffer = yb;

	/*
	 * skip the ";" if there is one and trailing whitespace. Note that
	 * yytext contains at least one non-space character plus the ";"
	 */
	for (i = strlen(yytext)-2;
		 i > 0 && ecpg_isspace(yytext[i]);
		 i--)
		;

	if (yytext[i] == ';')
		i--;

	yytext[i+1] = '\0';

	yyin = NULL;

	/* If file name is enclosed in '"' remove these and look only in '.' */
	/* Informix does look into all include paths though, except filename starts with '/' */
	if (yytext[0] == '"' && yytext[i] == '"' &&
		((compat != ECPG_COMPAT_INFORMIX && compat != ECPG_COMPAT_INFORMIX_SE) || yytext[1] == '/'))
	{
		yytext[i] = '\0';
		memmove(yytext, yytext+1, strlen(yytext));

		strlcpy(inc_file, yytext, sizeof(inc_file));
		yyin = fopen(inc_file, "r");
		if (!yyin)
		{
			if (strcmp(inc_file + strlen(inc_file) - 2, ".h") != 0)
			{
				strcat(inc_file, ".h");
				yyin = fopen(inc_file, "r");
			}
		}

	}
	else
	{
		if ((yytext[0] == '"' && yytext[i] == '"') || (yytext[0] == '<' && yytext[i] == '>'))
		{
			yytext[i] = '\0';
			memmove(yytext, yytext+1, strlen(yytext));
		}

		for (ip = include_paths; yyin == NULL && ip != NULL; ip = ip->next)
		{
			if (strlen(ip->path) + strlen(yytext) + 3 > MAXPGPATH)
			{
				fprintf(stderr, _("Error: include path \"%s/%s\" is too long on line %d, skipping\n"), ip->path, yytext, yylineno);
				continue;
			}
			snprintf (inc_file, sizeof(inc_file), "%s/%s", ip->path, yytext);
			yyin = fopen(inc_file, "r");
			if (!yyin)
			{
				if (strcmp(inc_file + strlen(inc_file) - 2, ".h") != 0)
				{
					strcat(inc_file, ".h");
					yyin = fopen( inc_file, "r" );
				}
			}
			/* if the command was "include_next" we have to disregard the first hit */
			if (yyin && include_next)
			{
				fclose (yyin);
				yyin = NULL;
				include_next = false;
			}
		}
	}
	if (!yyin)
		mmfatal(NO_INCLUDE_FILE, "could not open include file \"%s\" on line %d", yytext, yylineno);

	input_filename = mm_strdup(inc_file);
	yy_switch_to_buffer(yy_create_buffer(yyin,YY_BUF_SIZE ));
	yylineno = 1;
	output_line_number();

	BEGIN(C);
}

/*
 * ecpg_isspace() --- return TRUE if flex scanner considers char whitespace
 */
static bool
ecpg_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\f')
		return true;
	return false;
}

static bool isdefine(void)
{
	struct _defines *ptr;

	/* is it a define? */
	for (ptr = defines; ptr; ptr = ptr->next)
	{
		if (strcmp(yytext, ptr->old) == 0 && ptr->used == NULL)
		{
			struct _yy_buffer *yb;

			yb = mm_alloc(sizeof(struct _yy_buffer));

			yb->buffer =  YY_CURRENT_BUFFER;
			yb->lineno = yylineno;
			yb->filename = mm_strdup(input_filename);
			yb->next = yy_buffer;

			ptr->used = yy_buffer = yb;

			yy_scan_string(ptr->new);
			return true;
		}
	}

	return false;
}

static bool isinformixdefine(void)
{
	const char *new = NULL;

	if (strcmp(yytext, "dec_t") == 0)
		new = "decimal";
	else if (strcmp(yytext, "intrvl_t") == 0)
		new = "interval";
	else if (strcmp(yytext, "dtime_t") == 0)
		new = "timestamp";

	if (new)
	{
		struct _yy_buffer *yb;

		yb = mm_alloc(sizeof(struct _yy_buffer));

		yb->buffer =  YY_CURRENT_BUFFER;
		yb->lineno = yylineno;
		yb->filename = mm_strdup(input_filename);
		yb->next = yy_buffer;
		yy_buffer = yb;

		yy_scan_string(new);
		return true;
	}

	return false;
}

/*
 * Called before any actual parsing is done
 */
void
scanner_init(const char *str)
{
	Size	slen = strlen(str);

	/*
	 * Might be left over after ereport()
	 */
	if (YY_CURRENT_BUFFER)
		yy_delete_buffer(YY_CURRENT_BUFFER);

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	scanbuf = mm_alloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	/* initialize literal buffer to a reasonable but expansible size */
	literalalloc = 128;
	literalbuf = (char *) mm_alloc(literalalloc);
	startlit();

	BEGIN(INITIAL);
}


/*
 * Called after parsing is done to clean up after scanner_init()
 */
void
scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	free(scanbuf);
}
