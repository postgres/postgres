%top{
/*-------------------------------------------------------------------------
 *
 * pgc.l
 *	  lexical scanner for ecpg
 *
 * This is a modified version of src/backend/parser/scan.l
 *
 * The ecpg scanner is not backup-free, so the fail rules are
 * only here to simplify syncing this file with scan.l.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/ecpg/preproc/pgc.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <limits.h>

#include "common/string.h"

#include "preproc_extern.h"
#include "preproc.h"
}

%{

/* LCOV_EXCL_START */

extern YYSTYPE base_yylval;

static int	xcdepth = 0;		/* depth of nesting in slash-star comments */
static char *dolqstart = NULL;	/* current $foo$ quote start string */

/*
 * literalbuf is used to accumulate literal values when multiple rules
 * are needed to parse a single literal.  Call startlit to reset buffer
 * to empty, addlit to add text.  Note that the buffer is permanently
 * malloc'd to the largest size needed so far in the current run.
 */
static char *literalbuf = NULL; /* expandable buffer */
static int	literallen;			/* actual current length */
static int	literalalloc;		/* current allocated buffer size */

/* Used for detecting global state together with braces_open */
static int	parenths_open;

/* Used to tell parse_include() whether the command was #include or #include_next */
static bool include_next;

#define startlit()	(literalbuf[0] = '\0', literallen = 0)
static void addlit(char *ytext, int yleng);
static void addlitchar(unsigned char ychar);
static int	process_integer_literal(const char *token, YYSTYPE *lval, int base);
static void parse_include(void);
static bool ecpg_isspace(char ch);
static bool isdefine(void);
static bool isinformixdefine(void);

char	   *token_start;

/* vars to keep track of start conditions when scanning literals */
static int	state_before_str_start;
static int	state_before_str_stop;

/*
 * State for handling include files and macro expansion.  We use a new
 * flex input buffer for each level of include or macro, and create a
 * struct _yy_buffer to remember the previous level.  There is not a struct
 * for the currently active input source; that state is kept in the global
 * variables YY_CURRENT_BUFFER, yylineno, and input_filename.
 */
static struct _yy_buffer
{
	YY_BUFFER_STATE buffer;
	long		lineno;
	char	   *filename;
	struct _yy_buffer *next;
} *yy_buffer = NULL;

/*
 * Vars for handling ifdef/elif/endif constructs.  preproc_tos is the current
 * nesting depth of such constructs, and stacked_if_value[preproc_tos] is the
 * state for the innermost level.  (For convenience, stacked_if_value[0] is
 * initialized as though we are in the active branch of some outermost IF.)
 * The active field is true if the current branch is active (being expanded).
 * The saw_active field is true if we have found any successful branch,
 * so that all subsequent branches of this level should be skipped.
 * The else_branch field is true if we've found an 'else' (so that another
 * 'else' or 'elif' at this level is an error.)
 * For IFs nested within an inactive branch, all branches always have active
 * set to false, but saw_active and else_branch are maintained normally.
 * ifcond is valid only while evaluating an if-condition; it's true if we
 * are doing ifdef, false if ifndef.
 */
#define MAX_NESTED_IF 128
static short preproc_tos;
static bool ifcond;
static struct _if_value
{
	bool		active;
	bool		saw_active;
	bool		else_branch;
} stacked_if_value[MAX_NESTED_IF];

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option noyywrap
%option warn
%option yylineno
%option prefix="base_yy"

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
 *  <xb> bit string literal
 *  <xc> extended C-style comments
 *  <xd> delimited identifiers (double-quoted identifiers)
 *  <xdc> double-quoted strings in C
 *  <xh> hexadecimal byte string
 *  <xn> national character quoted strings
 *  <xq> standard quoted strings
 *  <xqs> quote stop (detect continued strings)
 *  <xe> extended quoted strings (support backslash escape sequences)
 *  <xqc> single-quoted strings in C
 *  <xdolq> $foo$ quoted strings
 *  <xui> quoted identifier with Unicode escapes
 *  <xus> quoted string with Unicode escapes
 *  <xcond> condition of an EXEC SQL IFDEF construct
 *  <xskip> skipping the inactive part of an EXEC SQL IFDEF construct
 *
 * Note: we intentionally don't mimic the backend's <xeu> state; we have
 * no need to distinguish it from <xe> state.
 *
 * Remember to add an <<EOF>> case whenever you add a new exclusive state!
 * The default one is probably not the right thing.
 */

%x xb
%x xc
%x xd
%x xdc
%x xh
%x xn
%x xq
%x xqs
%x xe
%x xqc
%x xdolq
%x xui
%x xus
%x xcond
%x xskip

/* Additional exclusive states that are specific to ECPG */
%x C SQL incl def def_ident undef

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
 * non_newline_space tracks all space characters except newlines.
 *
 * XXX if you change the set of whitespace characters, fix ecpg_isspace()
 * to agree.
 */

space				[ \t\n\r\f\v]
non_newline_space	[ \t\f\v]
newline				[\n\r]
non_newline			[^\n\r]

comment			("--"{non_newline}*)

whitespace		({space}+|{comment})

/*
 * SQL requires at least one newline in the whitespace separating
 * string literals that are to be concatenated.  Silly, but who are we
 * to argue?  Note that {whitespace_with_newline} should not have * after
 * it, whereas {whitespace} should generally have a * after it...
 */

non_newline_whitespace	({non_newline_space}|{comment})
whitespace_with_newline	({non_newline_whitespace}*{newline}{whitespace}*)

quote			'
/* If we see {quote} then {quotecontinue}, the quoted string continues */
quotecontinue	{whitespace_with_newline}{quote}

/*
 * {quotecontinuefail} is needed to avoid lexer backup when we fail to match
 * {quotecontinue}.  It might seem that this could just be {whitespace}*,
 * but if there's a dash after {whitespace_with_newline}, it must be consumed
 * to see if there's another dash --- which would start a {comment} and thus
 * allow continuation of the {quotecontinue} token.
 */
quotecontinuefail	{whitespace}*"-"?

/* Bit string
 */
xbstart			[bB]{quote}
xbinside		[^']*

/* Hexadecimal byte string */
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

/* Quoted identifier with Unicode escapes */
xuistart		[uU]&{dquote}

/* Quoted string with Unicode escapes */
xusstart		[uU]&{quote}

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
 * Dash-dash comments have similar interactions with the operator rule.
 */
xcstart			\/\*{op_chars}*
xcstop			\*+\/
xcinside		[^*/]+

ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

array			({ident_cont}|{whitespace}|[\[\]\+\-\*\%\/\(\)\>\.])*

/* Assorted special-case operators and operator-like tokens */
typecast		"::"
dot_dot			\.\.
colon_equals	":="

/*
 * These operator-like tokens (unlike the above ones) also match the {operator}
 * rule, which means that they might be overridden by a longer match if they
 * are followed by a comment start or a + or - character. Accordingly, if you
 * add to this list, you must also add corresponding code to the {operator}
 * block to return the correct token in such cases. (This is not needed in
 * psqlscan.l since the token value is ignored there.)
 */
equals_greater	"=>"
less_equals		"<="
greater_equals	">="
less_greater	"<>"
not_equals		"!="

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
self			[,()\[\].;\:\+\-\*\/\%\^\<\>\=]
op_chars		[\~\!\@\#\^\&\|\`\?\+\-\*\/\%\<\>\=]
operator		{op_chars}+

/*
 * Numbers
 *
 * Unary minus is not part of a number here.  Instead we pass it separately to
 * the parser, and there it gets coerced via doNegate().
 *
 * {numericfail} is used because we would like "1..10" to lex as 1, dot_dot, 10.
 *
 * {realfail} is added to prevent the need for scanner
 * backup when the {real} rule fails to match completely.
 */
decdigit		[0-9]
hexdigit		[0-9A-Fa-f]
octdigit		[0-7]
bindigit		[0-1]

decinteger		{decdigit}(_?{decdigit})*
hexinteger		0[xX](_?{hexdigit})+
octinteger		0[oO](_?{octdigit})+
bininteger		0[bB](_?{bindigit})+

hexfail			0[xX]_?
octfail			0[oO]_?
binfail			0[bB]_?

numeric			(({decinteger}\.{decinteger}?)|(\.{decinteger}))
numericfail		{decinteger}\.\.

real			({decinteger}|{numeric})[Ee][-+]?{decinteger}
realfail		({decinteger}|{numeric})[Ee][-+]

decinteger_junk	{decinteger}{ident_start}
hexinteger_junk	{hexinteger}{ident_start}
octinteger_junk	{octinteger}{ident_start}
bininteger_junk	{bininteger}{ident_start}
numeric_junk	{numeric}{ident_start}
real_junk		{real}{ident_start}

/* Positional parameters don't accept underscores. */
param			\${decdigit}+
param_junk		\${decdigit}+{ident_start}

/* special characters for other dbms */
/* we have to react differently in compat mode */
informix_special	[\$]

other			.

/*
 * Dollar quoted strings are totally opaque, and no escaping is done on them.
 * Other quoted strings must allow some special characters such as single-quote
 *  and newline.
 * Embedded single-quotes are implemented both in the SQL standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string.
 * Note that xcstart must appear before operator, as explained above!
 *  Also whitespace (comment) must appear before operator.
 */

/* some stuff needed for ecpg */
exec			[eE][xX][eE][cC]
sql				[sS][qQ][lL]
define			[dD][eE][fF][iI][nN][eE]
include			[iI][nN][cC][lL][uU][dD][eE]
include_next	[iI][nN][cC][lL][uU][dD][eE]_[nN][eE][xX][tT]
import			[iI][mM][pP][oO][rR][tT]
undef			[uU][nN][dD][eE][fF]

ccomment		"//".*\n

if				[iI][fF]
ifdef			[iI][fF][dD][eE][fF]
ifndef			[iI][fF][nN][dD][eE][fF]
else			[eE][lL][sS][eE]
elif			[eE][lL][iI][fF]
endif			[eE][nN][dD][iI][fF]

struct			[sS][tT][rR][uU][cC][tT]

exec_sql		{exec}{space}*{sql}{space}*
ipdigit			({decdigit}|{decdigit}{decdigit}|{decdigit}{decdigit}{decdigit})
ip				{ipdigit}\.{ipdigit}\.{ipdigit}\.{ipdigit}

/* we might want to parse all cpp include files */
cppinclude		{space}*#{include}{space}*
cppinclude_next		{space}*#{include_next}{space}*

/* take care of cpp lines, they may also be continued */
/* first a general line for all commands not starting with "i" */
/* and then the other commands starting with "i", we have to add these
 * separately because the cppline production would match on "include" too
 */
cppline			{space}*#([^i][A-Za-z]*|{if}|{ifdef}|{ifndef}|{import})((\/\*[^*/]*\*+\/)|.|\\{space}*{newline})*{newline}

%%

%{
		/* code to execute during start of each call of yylex() */
		char *newdefsymbol = NULL;

		token_start = NULL;
%}

<SQL>{
{whitespace}	{
					/* ignore */
				}
} /* <SQL> */

<C,SQL>{
{xcstart}		{
					token_start = yytext;
					state_before_str_start = YYSTATE;
					xcdepth = 0;
					BEGIN(xc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
					fputs("/*", yyout);
				}
} /* <C,SQL> */

<xc>{
{xcstart}		{
					if (state_before_str_start == SQL)
					{
						xcdepth++;
						/* Put back any characters past slash-star; see above */
						yyless(2);
						fputs("/_*", yyout);
					}
					else if (state_before_str_start == C)
					{
						ECHO;
					}
				}

{xcstop}		{
					if (state_before_str_start == SQL)
					{
						if (xcdepth <= 0)
						{
							ECHO;
							BEGIN(SQL);
							token_start = NULL;
						}
						else
						{
							xcdepth--;
							fputs("*_/", yyout);
						}
					}
					else if (state_before_str_start == C)
					{
						ECHO;
						BEGIN(C);
						token_start = NULL;
					}
				}

{xcinside}		{
					ECHO;
				}

{op_chars}		{
					ECHO;
				}

\*+				{
					ECHO;
				}

<<EOF>>			{
					mmfatal(PARSE_ERROR, "unterminated /* comment");
				}
} /* <xc> */

<SQL>{
{xbstart}		{
					token_start = yytext;
					state_before_str_start = YYSTATE;
					BEGIN(xb);
					startlit();
				}
} /* <SQL> */

<xh>{xhinside}	|
<xb>{xbinside}	{
					addlit(yytext, yyleng);
				}
<xb><<EOF>>		{
					mmfatal(PARSE_ERROR, "unterminated bit string literal");
				}

<SQL>{xhstart}	{
					token_start = yytext;
					state_before_str_start = YYSTATE;
					BEGIN(xh);
					startlit();
				}
<xh><<EOF>>		{
					mmfatal(PARSE_ERROR, "unterminated hexadecimal string literal");
				}

<C>{xqstart}	{
					token_start = yytext;
					state_before_str_start = YYSTATE;
					BEGIN(xqc);
					startlit();
				}

<SQL>{
{xnstart}		{
					/* National character. Transfer it as-is to the backend. */
					token_start = yytext;
					state_before_str_start = YYSTATE;
					BEGIN(xn);
					startlit();
				}

{xqstart}		{
					token_start = yytext;
					state_before_str_start = YYSTATE;
					BEGIN(xq);
					startlit();
				}
{xestart}		{
					token_start = yytext;
					state_before_str_start = YYSTATE;
					BEGIN(xe);
					startlit();
				}
{xusstart}		{
					token_start = yytext;
					state_before_str_start = YYSTATE;
					BEGIN(xus);
					startlit();
				}
} /* <SQL> */

<xb,xh,xq,xqc,xe,xn,xus>{quote} {
					/*
					 * When we are scanning a quoted string and see an end
					 * quote, we must look ahead for a possible continuation.
					 * If we don't see one, we know the end quote was in fact
					 * the end of the string.  To reduce the lexer table size,
					 * we use a single "xqs" state to do the lookahead for all
					 * types of strings.
					 */
					state_before_str_stop = YYSTATE;
					BEGIN(xqs);
				}
<xqs>{quotecontinue} {
					/*
					 * Found a quote continuation, so return to the in-quote
					 * state and continue scanning the literal.  Nothing is
					 * added to the literal's contents.
					 */
					BEGIN(state_before_str_stop);
				}
<xqs>{quotecontinuefail} |
<xqs>{other} |
<xqs><<EOF>>	{
					/*
					 * Failed to see a quote continuation.  Throw back
					 * everything after the end quote, and handle the string
					 * according to the state we were in previously.
					 */
					yyless(0);
					BEGIN(state_before_str_start);

					switch (state_before_str_stop)
					{
						case xb:
							if (literalbuf[strspn(literalbuf, "01")] != '\0')
								mmerror(PARSE_ERROR, ET_ERROR, "invalid bit string literal");
							base_yylval.str = psprintf("b'%s'", literalbuf);
							return BCONST;
						case xh:
							if (literalbuf[strspn(literalbuf, "0123456789abcdefABCDEF")] != '\0')
								mmerror(PARSE_ERROR, ET_ERROR, "invalid hexadecimal string literal");
							base_yylval.str = psprintf("x'%s'", literalbuf);
							return XCONST;
						case xq:
							/* fallthrough */
						case xqc:
							base_yylval.str = psprintf("'%s'", literalbuf);
							return SCONST;
						case xe:
							base_yylval.str = psprintf("E'%s'", literalbuf);
							return SCONST;
						case xn:
							base_yylval.str = psprintf("N'%s'", literalbuf);
							return SCONST;
						case xus:
							base_yylval.str = psprintf("U&'%s'", literalbuf);
							return USCONST;
						default:
							mmfatal(PARSE_ERROR, "unhandled previous state in xqs\n");
					}
				}

<xq,xe,xn,xus>{xqdouble} {
					addlit(yytext, yyleng);
				}
<xqc>{xqcquote}	{
					addlit(yytext, yyleng);
				}
<xq,xqc,xn,xus>{xqinside} {
					addlit(yytext, yyleng);
				}
<xe>{xeinside}	{
					addlit(yytext, yyleng);
				}
<xe>{xeunicode}	{
					addlit(yytext, yyleng);
				}
<xe>{xeescape}	{
					addlit(yytext, yyleng);
				}
<xe>{xeoctesc}	{
					addlit(yytext, yyleng);
				}
<xe>{xehexesc}	{
					addlit(yytext, yyleng);
				}
<xe>.			{
					/* This is only needed for \ just before EOF */
					addlitchar(yytext[0]);
				}
<xq,xqc,xe,xn,xus><<EOF>>	{
					mmfatal(PARSE_ERROR, "unterminated quoted string");
				}

<SQL>{
{dolqdelim}		{
					token_start = yytext;
					if (dolqstart)
						free(dolqstart);
					dolqstart = mm_strdup(yytext);
					BEGIN(xdolq);
					startlit();
					addlit(yytext, yyleng);
				}
{dolqfailed}	{
					/* throw back all but the initial "$" */
					yyless(1);
					/* and treat it as {other} */
					return yytext[0];
				}
} /* <SQL> */

<xdolq>{dolqdelim}	{
					if (strcmp(yytext, dolqstart) == 0)
					{
						addlit(yytext, yyleng);
						free(dolqstart);
						dolqstart = NULL;
						BEGIN(SQL);
						base_yylval.str = mm_strdup(literalbuf);
						return SCONST;
					}
					else
					{
						/*
						 * When we fail to match $...$ to dolqstart, transfer
						 * the $... part to the output, but put back the final
						 * $ for rescanning.  Consider $delim$...$junk$delim$
						 */
						addlit(yytext, yyleng - 1);
						yyless(yyleng - 1);
					}
				}
<xdolq>{dolqinside} {
					addlit(yytext, yyleng);
				}
<xdolq>{dolqfailed} {
					addlit(yytext, yyleng);
				}
<xdolq>.		{
					/* single quote or dollar sign */
					addlitchar(yytext[0]);
				}
<xdolq><<EOF>>	{
					mmfatal(PARSE_ERROR, "unterminated dollar-quoted string");
				}

<SQL>{
{xdstart}		{
					state_before_str_start = YYSTATE;
					BEGIN(xd);
					startlit();
				}
{xuistart}		{
					state_before_str_start = YYSTATE;
					BEGIN(xui);
					startlit();
				}
} /* <SQL> */

<xd>{xdstop}	{
					BEGIN(state_before_str_start);
					if (literallen == 0)
						mmerror(PARSE_ERROR, ET_ERROR, "zero-length delimited identifier");

					/*
					 * The server will truncate the identifier here.  We do
					 * not, as (1) it does not change the result; (2) we don't
					 * know what NAMEDATALEN the server might use; (3) this
					 * code path is also taken for literal query strings in
					 * PREPARE and EXECUTE IMMEDIATE, which can certainly be
					 * longer than NAMEDATALEN.
					 */
					base_yylval.str = mm_strdup(literalbuf);
					return CSTRING;
				}
<xdc>{xdstop}	{
					BEGIN(state_before_str_start);
					base_yylval.str = mm_strdup(literalbuf);
					return CSTRING;
				}
<xui>{dquote}	{
					BEGIN(state_before_str_start);
					if (literallen == 0)
						mmerror(PARSE_ERROR, ET_ERROR, "zero-length delimited identifier");

					/*
					 * The backend will truncate the identifier here. We do
					 * not as it does not change the result.
					 */
					base_yylval.str = psprintf("U&\"%s\"", literalbuf);
					return UIDENT;
				}
<xd,xui>{xddouble} {
					addlit(yytext, yyleng);
				}
<xd,xui>{xdinside} {
					addlit(yytext, yyleng);
				}
<xd,xui><<EOF>>	{
					mmfatal(PARSE_ERROR, "unterminated quoted identifier");
				}
<C>{xdstart}	{
					state_before_str_start = YYSTATE;
					BEGIN(xdc);
					startlit();
				}
<xdc>{xdcinside} {
					addlit(yytext, yyleng);
				}
<xdc><<EOF>>	{
					mmfatal(PARSE_ERROR, "unterminated quoted string");
				}

<SQL>{
{typecast}		{
					return TYPECAST;
				}

{dot_dot}		{
					return DOT_DOT;
				}

{colon_equals}	{
					return COLON_EQUALS;
				}

{equals_greater} {
					return EQUALS_GREATER;
				}

{less_equals}	{
					return LESS_EQUALS;
				}

{greater_equals} {
					return GREATER_EQUALS;
				}

{less_greater}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS */
					return NOT_EQUALS;
				}

{not_equals}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS */
					return NOT_EQUALS;
				}

{informix_special} {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						unput(':');
					}
					else
						return yytext[0];
				}

{self}			{
					/*
					 * We may find a ';' inside a structure definition in a
					 * TYPE or VAR statement. This is not an EOL marker.
					 */
					if (yytext[0] == ';' && struct_level == 0)
						BEGIN(C);
					return yytext[0];
				}

{operator}		{
					/*
					 * Check for embedded slash-star or dash-dash; those
					 * are comment starts, so operator must stop there.
					 * Note that slash-star or dash-dash at the first
					 * character will match a prior rule, not this one.
					 */
					int			nchars = yyleng;
					char	   *slashstar = strstr(yytext, "/*");
					char	   *dashdash = strstr(yytext, "--");

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
					if (nchars > 1 &&
						(yytext[nchars - 1] == '+' ||
						 yytext[nchars - 1] == '-'))
					{
						int			ic;

						for (ic = nchars - 2; ic >= 0; ic--)
						{
							char		c = yytext[ic];

							if (c == '~' || c == '!' || c == '@' ||
								c == '#' || c == '^' || c == '&' ||
								c == '|' || c == '`' || c == '?' ||
								c == '%')
								break;
						}
						if (ic < 0)
						{
							/*
							 * didn't find a qualifying character, so remove
							 * all trailing [+-]
							 */
							do
							{
								nchars--;
							} while (nchars > 1 &&
									 (yytext[nchars - 1] == '+' ||
									  yytext[nchars - 1] == '-'));
						}
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

						/*
						 * Likewise, if what we have left is two chars, and
						 * those match the tokens ">=", "<=", "=>", "<>" or
						 * "!=", then we must return the appropriate token
						 * rather than the generic Op.
						 */
						if (nchars == 2)
						{
							if (yytext[0] == '=' && yytext[1] == '>')
								return EQUALS_GREATER;
							if (yytext[0] == '>' && yytext[1] == '=')
								return GREATER_EQUALS;
							if (yytext[0] == '<' && yytext[1] == '=')
								return LESS_EQUALS;
							if (yytext[0] == '<' && yytext[1] == '>')
								return NOT_EQUALS;
							if (yytext[0] == '!' && yytext[1] == '=')
								return NOT_EQUALS;
						}
					}

					base_yylval.str = mm_strdup(yytext);
					return Op;
				}

{param}			{
					int			val;

					errno = 0;
					val = strtoint(yytext + 1, NULL, 10);
					if (errno == ERANGE)
						mmfatal(PARSE_ERROR, "parameter number too large");
					base_yylval.ival = val;
					return PARAM;
				}
{param_junk}	{
					mmfatal(PARSE_ERROR, "trailing junk after parameter");
				}

{ip}			{
					base_yylval.str = mm_strdup(yytext);
					return IP;
				}
}  /* <SQL> */

<C,SQL>{
{decinteger}	{
					return process_integer_literal(yytext, &base_yylval, 10);
				}
{hexinteger}	{
					return process_integer_literal(yytext, &base_yylval, 16);
				}
{numeric}		{
					base_yylval.str = mm_strdup(yytext);
					return FCONST;
				}
{numericfail}	{
					/* throw back the .., and treat as integer */
					yyless(yyleng - 2);
					return process_integer_literal(yytext, &base_yylval, 10);
				}
{real}			{
					base_yylval.str = mm_strdup(yytext);
					return FCONST;
				}
{realfail}		{
					/*
					 * throw back the [Ee][+-], and figure out whether what
					 * remains is an {decinteger} or {numeric}.
					 */
					yyless(yyleng - 2);
					return process_integer_literal(yytext, &base_yylval, 10);
				}
} /* <C,SQL> */

<SQL>{
{octinteger}	{
					return process_integer_literal(yytext, &base_yylval, 8);
				}
{bininteger}	{
					return process_integer_literal(yytext, &base_yylval, 2);
				}

	/*
	 * Note that some trailing junk is valid in C (such as 100LL), so we
	 * contain this to SQL mode.
	 */
{decinteger_junk} {
					mmfatal(PARSE_ERROR, "trailing junk after numeric literal");
				}
{hexinteger_junk} {
					mmfatal(PARSE_ERROR, "trailing junk after numeric literal");
				}
{octinteger_junk} {
					mmfatal(PARSE_ERROR, "trailing junk after numeric literal");
				}
{bininteger_junk} {
					mmfatal(PARSE_ERROR, "trailing junk after numeric literal");
				}
{numeric_junk}	{
					mmfatal(PARSE_ERROR, "trailing junk after numeric literal");
				}
{real_junk}		{
					mmfatal(PARSE_ERROR, "trailing junk after numeric literal");
				}

:{identifier}((("->"|\.){identifier})|(\[{array}\]))*	{
					base_yylval.str = mm_strdup(yytext + 1);
					return CVARIABLE;
				}

{identifier}	{
					/* First check to see if it's a define symbol to expand */
					if (!isdefine())
					{
						int			kwvalue;

						/*
						 * User-defined typedefs override SQL keywords, but
						 * not C keywords.  Currently, a typedef name is just
						 * reported as IDENT, but someday we might need to
						 * return a distinct token type.
						 */
						if (get_typedef(yytext, true) == NULL)
						{
							/* Is it an SQL/ECPG keyword? */
							kwvalue = ScanECPGKeywordLookup(yytext);
							if (kwvalue >= 0)
								return kwvalue;
						}

						/* Is it a C keyword? */
						kwvalue = ScanCKeywordLookup(yytext);
						if (kwvalue >= 0)
							return kwvalue;

						/*
						 * None of the above.  Return it as an identifier.
						 *
						 * The backend will attempt to truncate and case-fold
						 * the identifier, but I see no good reason for ecpg
						 * to do so; that's just another way that ecpg could
						 * get out of step with the backend.
						 */
						base_yylval.str = mm_strdup(yytext);
						return IDENT;
					}
				}

{other}			{
					return yytext[0];
				}
} /* <SQL> */

	/*
	 * Begin ECPG-specific rules
	 */

<C>{exec_sql}	{
					BEGIN(SQL);
					return SQL_START;
				}
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
<C>{ccomment}	{
					ECHO;
				}
<C>{cppinclude}	{
					if (system_includes)
					{
						include_next = false;
						BEGIN(incl);
					}
					else
					{
						base_yylval.str = mm_strdup(yytext);
						return CPP_LINE;
					}
				}
<C>{cppinclude_next} {
					if (system_includes)
					{
						include_next = true;
						BEGIN(incl);
					}
					else
					{
						base_yylval.str = mm_strdup(yytext);
						return CPP_LINE;
					}
				}
<C,SQL>{cppline} {
					base_yylval.str = mm_strdup(yytext);
					return CPP_LINE;
				}
<C>{identifier}	{
					/*
					 * Try to detect a function name:
					 * look for identifiers at the global scope
					 * keep the last identifier before the first '(' and '{'
					 */
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
						int			kwvalue;

						kwvalue = ScanCKeywordLookup(yytext);
						if (kwvalue >= 0)
							return kwvalue;
						else
						{
							base_yylval.str = mm_strdup(yytext);
							return IDENT;
						}
					}
				}
<C>{xcstop}		{
					mmerror(PARSE_ERROR, ET_ERROR, "nested /* ... */ comments");
				}
<C>":"				{ return ':'; }
<C>";"				{ return ';'; }
<C>","				{ return ','; }
<C>"*"				{ return '*'; }
<C>"%"				{ return '%'; }
<C>"/"				{ return '/'; }
<C>"+"				{ return '+'; }
<C>"-"				{ return '-'; }
<C>"("				{ parenths_open++; return '('; }
<C>")"				{ parenths_open--; return ')'; }
<C,xskip>{space}		{ ECHO; }
<C>\{				{ return '{'; }
<C>\}				{ return '}'; }
<C>\[				{ return '['; }
<C>\]				{ return ']'; }
<C>\=				{ return '='; }
<C>"->"				{ return S_MEMBER; }
<C>">>"				{ return S_RSHIFT; }
<C>"<<"				{ return S_LSHIFT; }
<C>"||"				{ return S_OR; }
<C>"&&"				{ return S_AND; }
<C>"++"				{ return S_INC; }
<C>"--"				{ return S_DEC; }
<C>"=="				{ return S_EQUAL; }
<C>"!="				{ return S_NEQUAL; }
<C>"+="				{ return S_ADD; }
<C>"-="				{ return S_SUB; }
<C>"*="				{ return S_MUL; }
<C>"/="				{ return S_DIV; }
<C>"%="				{ return S_MOD; }
<C>"->*"			{ return S_MEMPOINT; }
<C>".*"				{ return S_DOTPOINT; }
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
						return S_ANYTHING;
					}
				}
<C>{exec_sql}{undef}{space}* {
					BEGIN(undef);
				}
<C>{informix_special}{undef}{space}* {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						BEGIN(undef);
					}
					else
					{
						yyless(1);
						return S_ANYTHING;
					}
				}
<undef>{identifier}{space}*";" {
					struct _defines *ptr,
							   *ptr2 = NULL;
					int			i;

					/*
					 * Skip the ";" and trailing whitespace. Note that yytext
					 * contains at least one non-space character plus the ";"
					 */
					for (i = strlen(yytext) - 2;
						 i > 0 && ecpg_isspace(yytext[i]);
						 i--)
						;
					yytext[i + 1] = '\0';

					/* Find and unset any matching define; should be only 1 */
					for (ptr = defines; ptr; ptr2 = ptr, ptr = ptr->next)
					{
						if (strcmp(yytext, ptr->name) == 0)
						{
							free(ptr->value);
							ptr->value = NULL;
							/* We cannot forget it if there's a cmdvalue */
							if (ptr->cmdvalue == NULL)
							{
								if (ptr2 == NULL)
									defines = ptr->next;
								else
									ptr2->next = ptr->next;
								free(ptr->name);
								free(ptr);
							}
							break;
						}
					}

					BEGIN(C);
				}
<undef>{other}|\n {
					mmfatal(PARSE_ERROR, "missing identifier in EXEC SQL UNDEF command");
					yyterminate();
				}
<C>{exec_sql}{include}{space}* {
					BEGIN(incl);
				}
<C>{informix_special}{include}{space}* {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						BEGIN(incl);
					}
					else
					{
						yyless(1);
						return S_ANYTHING;
					}
				}
<C,xskip>{exec_sql}{ifdef}{space}* {
					if (preproc_tos >= MAX_NESTED_IF - 1)
						mmfatal(PARSE_ERROR, "too many nested EXEC SQL IFDEF conditions");
					preproc_tos++;
					stacked_if_value[preproc_tos].active = false;
					stacked_if_value[preproc_tos].saw_active = false;
					stacked_if_value[preproc_tos].else_branch = false;
					ifcond = true;
					BEGIN(xcond);
				}
<C,xskip>{informix_special}{ifdef}{space}* {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (preproc_tos >= MAX_NESTED_IF - 1)
							mmfatal(PARSE_ERROR, "too many nested EXEC SQL IFDEF conditions");
						preproc_tos++;
						stacked_if_value[preproc_tos].active = false;
						stacked_if_value[preproc_tos].saw_active = false;
						stacked_if_value[preproc_tos].else_branch = false;
						ifcond = true;
						BEGIN(xcond);
					}
					else
					{
						yyless(1);
						return S_ANYTHING;
					}
				}
<C,xskip>{exec_sql}{ifndef}{space}* {
					if (preproc_tos >= MAX_NESTED_IF - 1)
						mmfatal(PARSE_ERROR, "too many nested EXEC SQL IFDEF conditions");
					preproc_tos++;
					stacked_if_value[preproc_tos].active = false;
					stacked_if_value[preproc_tos].saw_active = false;
					stacked_if_value[preproc_tos].else_branch = false;
					ifcond = false;
					BEGIN(xcond);
				}
<C,xskip>{informix_special}{ifndef}{space}* {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (preproc_tos >= MAX_NESTED_IF - 1)
							mmfatal(PARSE_ERROR, "too many nested EXEC SQL IFDEF conditions");
						preproc_tos++;
						stacked_if_value[preproc_tos].active = false;
						stacked_if_value[preproc_tos].saw_active = false;
						stacked_if_value[preproc_tos].else_branch = false;
						ifcond = false;
						BEGIN(xcond);
					}
					else
					{
						yyless(1);
						return S_ANYTHING;
					}
				}
<C,xskip>{exec_sql}{elif}{space}* {
					if (preproc_tos == 0)
						mmfatal(PARSE_ERROR, "missing matching \"EXEC SQL IFDEF\" / \"EXEC SQL IFNDEF\"");
					if (stacked_if_value[preproc_tos].else_branch)
						mmfatal(PARSE_ERROR, "missing \"EXEC SQL ENDIF;\"");
					ifcond = true;
					BEGIN(xcond);
				}
<C,xskip>{informix_special}{elif}{space}* {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (preproc_tos == 0)
							mmfatal(PARSE_ERROR, "missing matching \"EXEC SQL IFDEF\" / \"EXEC SQL IFNDEF\"");
						if (stacked_if_value[preproc_tos].else_branch)
							mmfatal(PARSE_ERROR, "missing \"EXEC SQL ENDIF;\"");
						ifcond = true;
						BEGIN(xcond);
					}
					else
					{
						yyless(1);
						return S_ANYTHING;
					}
				}

<C,xskip>{exec_sql}{else}{space}*";" {
	/* only exec sql endif pops the stack, so take care of duplicated 'else' */
					if (preproc_tos == 0)
						mmfatal(PARSE_ERROR, "missing matching \"EXEC SQL IFDEF\" / \"EXEC SQL IFNDEF\"");
					else if (stacked_if_value[preproc_tos].else_branch)
						mmfatal(PARSE_ERROR, "more than one EXEC SQL ELSE");
					else
					{
						stacked_if_value[preproc_tos].else_branch = true;
						stacked_if_value[preproc_tos].active =
							(stacked_if_value[preproc_tos - 1].active &&
							 !stacked_if_value[preproc_tos].saw_active);
						stacked_if_value[preproc_tos].saw_active = true;

						if (stacked_if_value[preproc_tos].active)
							BEGIN(C);
						else
							BEGIN(xskip);
					}
				}
<C,xskip>{informix_special}{else}{space}*";" {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (preproc_tos == 0)
							mmfatal(PARSE_ERROR, "missing matching \"EXEC SQL IFDEF\" / \"EXEC SQL IFNDEF\"");
						else if (stacked_if_value[preproc_tos].else_branch)
							mmfatal(PARSE_ERROR, "more than one EXEC SQL ELSE");
						else
						{
							stacked_if_value[preproc_tos].else_branch = true;
							stacked_if_value[preproc_tos].active =
								(stacked_if_value[preproc_tos - 1].active &&
								 !stacked_if_value[preproc_tos].saw_active);
							stacked_if_value[preproc_tos].saw_active = true;

							if (stacked_if_value[preproc_tos].active)
								BEGIN(C);
							else
								BEGIN(xskip);
						}
					}
					else
					{
						yyless(1);
						return S_ANYTHING;
					}
				}
<C,xskip>{exec_sql}{endif}{space}*";" {
					if (preproc_tos == 0)
						mmfatal(PARSE_ERROR, "unmatched EXEC SQL ENDIF");
					else
						preproc_tos--;

					if (stacked_if_value[preproc_tos].active)
						BEGIN(C);
					else
						BEGIN(xskip);
				}
<C,xskip>{informix_special}{endif}{space}*";" {
					/* are we simulating Informix? */
					if (INFORMIX_MODE)
					{
						if (preproc_tos == 0)
							mmfatal(PARSE_ERROR, "unmatched EXEC SQL ENDIF");
						else
							preproc_tos--;

						if (stacked_if_value[preproc_tos].active)
							BEGIN(C);
						else
							BEGIN(xskip);
					}
					else
					{
						yyless(1);
						return S_ANYTHING;
					}
				}

<xskip>{other}	{ /* ignore */ }

<xcond>{identifier}{space}*";" {
					{
						struct _defines *defptr;
						unsigned int i;
						bool		this_active;

						/*
						 * Skip the ";" and trailing whitespace. Note that
						 * yytext contains at least one non-space character
						 * plus the ";"
						 */
						for (i = strlen(yytext) - 2;
							 i > 0 && ecpg_isspace(yytext[i]);
							 i--)
							/* skip */ ;
						yytext[i + 1] = '\0';

						/* Does a definition exist? */
						for (defptr = defines; defptr; defptr = defptr->next)
						{
							if (strcmp(yytext, defptr->name) == 0)
							{
								/* Found it, but is it currently undefined? */
								if (defptr->value == NULL)
									defptr = NULL; /* pretend it's not found */
								break;
							}
						}

						this_active = (defptr ? ifcond : !ifcond);
						stacked_if_value[preproc_tos].active =
							(stacked_if_value[preproc_tos - 1].active &&
							 !stacked_if_value[preproc_tos].saw_active &&
							 this_active);
						stacked_if_value[preproc_tos].saw_active |= this_active;
					}

					if (stacked_if_value[preproc_tos].active)
						BEGIN(C);
					else
						BEGIN(xskip);
				}

<xcond>{other}|\n {
					mmfatal(PARSE_ERROR, "missing identifier in EXEC SQL IFDEF command");
					yyterminate();
				}
<def_ident>{identifier} {
					newdefsymbol = mm_strdup(yytext);
					BEGIN(def);
					startlit();
				}
<def_ident>{other}|\n {
					mmfatal(PARSE_ERROR, "missing identifier in EXEC SQL DEFINE command");
					yyterminate();
				}
<def>{space}*";" {
					struct _defines *ptr;

					/* Does it already exist? */
					for (ptr = defines; ptr != NULL; ptr = ptr->next)
					{
						if (strcmp(newdefsymbol, ptr->name) == 0)
						{
							free(ptr->value);
							ptr->value = mm_strdup(literalbuf);
							/* Don't leak newdefsymbol */
							free(newdefsymbol);
							break;
						}
					}
					if (ptr == NULL)
					{
						/* Not present, make a new entry */
						ptr = (struct _defines *) mm_alloc(sizeof(struct _defines));

						ptr->name = newdefsymbol;
						ptr->value = mm_strdup(literalbuf);
						ptr->cmdvalue = NULL;
						ptr->used = NULL;
						ptr->next = defines;
						defines = ptr;
					}

					BEGIN(C);
				}
<def>[^;]		{ addlit(yytext, yyleng); }
<incl>\<[^\>]+\>{space}*";"?	{	parse_include(); }
<incl>{dquote}{xdinside}{dquote}{space}*";"?	{	parse_include(); }
<incl>[^;\<\>\"]+";"	{ parse_include(); }
<incl>{other}|\n {
					mmfatal(PARSE_ERROR, "syntax error in EXEC SQL INCLUDE command");
					yyterminate();
				}

<<EOF>>			{
					if (yy_buffer == NULL)
					{
						/* No more input */
						if (preproc_tos > 0)
						{
							preproc_tos = 0;
							mmfatal(PARSE_ERROR, "missing \"EXEC SQL ENDIF;\"");
						}
						yyterminate();
					}
					else
					{
						/* Revert to previous input source */
						struct _yy_buffer *yb = yy_buffer;
						int			i;
						struct _defines *ptr;

						/* Check to see if we are exiting a macro value */
						for (ptr = defines; ptr; ptr = ptr->next)
						{
							if (ptr->used == yy_buffer)
							{
								ptr->used = NULL;
								break; /* there can't be multiple matches */
							}
						}

						if (yyin != NULL)
							fclose(yyin);

						yy_delete_buffer(YY_CURRENT_BUFFER);
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

<INITIAL>{other}|\n {
					mmfatal(PARSE_ERROR, "internal error: unreachable state; please report this to <%s>", PACKAGE_BUGREPORT);
				}

%%

/* LCOV_EXCL_STOP */

void
lex_init(void)
{
	braces_open = 0;
	parenths_open = 0;
	current_function = NULL;

	yylineno = 1;

	/* initialize state for if/else/endif */
	preproc_tos = 0;
	stacked_if_value[preproc_tos].active = true;
	stacked_if_value[preproc_tos].saw_active = true;
	stacked_if_value[preproc_tos].else_branch = false;

	/* initialize literal buffer to a reasonable but expansible size */
	if (literalbuf == NULL)
	{
		literalalloc = 1024;
		literalbuf = (char *) mm_alloc(literalalloc);
	}
	startlit();

	BEGIN(C);
}

static void
addlit(char *ytext, int yleng)
{
	/* enlarge buffer if needed */
	if ((literallen + yleng) >= literalalloc)
	{
		do
			literalalloc *= 2;
		while ((literallen + yleng) >= literalalloc);
		literalbuf = (char *) realloc(literalbuf, literalalloc);
	}
	/* append new data, add trailing null */
	memcpy(literalbuf + literallen, ytext, yleng);
	literallen += yleng;
	literalbuf[literallen] = '\0';
}

static void
addlitchar(unsigned char ychar)
{
	/* enlarge buffer if needed */
	if ((literallen + 1) >= literalalloc)
	{
		literalalloc *= 2;
		literalbuf = (char *) realloc(literalbuf, literalalloc);
	}
	/* append new data, add trailing null */
	literalbuf[literallen] = ychar;
	literallen += 1;
	literalbuf[literallen] = '\0';
}

/*
 * Process {decinteger}, {hexinteger}, etc.  Note this will also do the right
 * thing with {numeric}, ie digits and a decimal point.
 */
static int
process_integer_literal(const char *token, YYSTYPE *lval, int base)
{
	int			val;
	char	   *endptr;

	errno = 0;
	val = strtoint(base == 10 ? token : token + 2, &endptr, base);
	if (*endptr != '\0' || errno == ERANGE)
	{
		/* integer too large (or contains decimal pt), treat it as a float */
		lval->str = mm_strdup(token);
		return FCONST;
	}
	lval->ival = val;
	return ICONST;
}

static void
parse_include(void)
{
	/* got the include file name */
	struct _yy_buffer *yb;
	struct _include_path *ip;
	char		inc_file[MAXPGPATH];
	unsigned int i;

	yb = mm_alloc(sizeof(struct _yy_buffer));

	yb->buffer = YY_CURRENT_BUFFER;
	yb->lineno = yylineno;
	yb->filename = input_filename;
	yb->next = yy_buffer;

	yy_buffer = yb;

	/*
	 * skip the ";" if there is one and trailing whitespace. Note that yytext
	 * contains at least one non-space character plus the ";"
	 */
	for (i = strlen(yytext) - 2;
		 i > 0 && ecpg_isspace(yytext[i]);
		 i--)
		;

	if (yytext[i] == ';')
		i--;

	yytext[i + 1] = '\0';

	yyin = NULL;

	/* If file name is enclosed in '"' remove these and look only in '.' */

	/*
	 * Informix does look into all include paths though, except filename
	 * starts with '/'
	 */
	if (yytext[0] == '"' && yytext[i] == '"' &&
		((compat != ECPG_COMPAT_INFORMIX && compat != ECPG_COMPAT_INFORMIX_SE) || yytext[1] == '/'))
	{
		yytext[i] = '\0';
		memmove(yytext, yytext + 1, strlen(yytext));

		strlcpy(inc_file, yytext, sizeof(inc_file));
		yyin = fopen(inc_file, "r");
		if (!yyin)
		{
			if (strlen(inc_file) <= 2 || strcmp(inc_file + strlen(inc_file) - 2, ".h") != 0)
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
			memmove(yytext, yytext + 1, strlen(yytext));
		}

		for (ip = include_paths; yyin == NULL && ip != NULL; ip = ip->next)
		{
			if (strlen(ip->path) + strlen(yytext) + 4 > MAXPGPATH)
			{
				fprintf(stderr, _("Error: include path \"%s/%s\" is too long on line %d, skipping\n"), ip->path, yytext, yylineno);
				continue;
			}
			snprintf(inc_file, sizeof(inc_file), "%s/%s", ip->path, yytext);
			yyin = fopen(inc_file, "r");
			if (!yyin)
			{
				if (strcmp(inc_file + strlen(inc_file) - 2, ".h") != 0)
				{
					strcat(inc_file, ".h");
					yyin = fopen(inc_file, "r");
				}
			}

			/*
			 * if the command was "include_next" we have to disregard the
			 * first hit
			 */
			if (yyin && include_next)
			{
				fclose(yyin);
				yyin = NULL;
				include_next = false;
			}
		}
	}
	if (!yyin)
		mmfatal(NO_INCLUDE_FILE, "could not open include file \"%s\" on line %d", yytext, yylineno);

	input_filename = mm_strdup(inc_file);
	yy_switch_to_buffer(yy_create_buffer(yyin, YY_BUF_SIZE));
	yylineno = 1;
	output_line_number();

	BEGIN(C);
}

/*
 * ecpg_isspace() --- return true if flex scanner considers char whitespace
 */
static bool
ecpg_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\f' ||
		ch == '\v')
		return true;
	return false;
}

/*
 * If yytext matches a define symbol, begin scanning the symbol's value
 * and return true
 */
static bool
isdefine(void)
{
	struct _defines *ptr;

	/* is it a define? */
	for (ptr = defines; ptr; ptr = ptr->next)
	{
		/* notice we do not match anything being actively expanded */
		if (strcmp(yytext, ptr->name) == 0 &&
			ptr->value != NULL &&
			ptr->used == NULL)
		{
			/* Save state associated with the current buffer */
			struct _yy_buffer *yb;

			yb = mm_alloc(sizeof(struct _yy_buffer));

			yb->buffer = YY_CURRENT_BUFFER;
			yb->lineno = yylineno;
			yb->filename = mm_strdup(input_filename);
			yb->next = yy_buffer;
			yy_buffer = yb;

			/* Mark symbol as being actively expanded */
			ptr->used = yb;

			/*
			 * We use yy_scan_string which will copy the value, so there's
			 * no need to worry about a possible undef happening while we
			 * are still scanning it.
			 */
			yy_scan_string(ptr->value);
			return true;
		}
	}

	return false;
}

/*
 * Handle replacement of INFORMIX built-in defines.  This works just
 * like isdefine() except for the source of the string to scan.
 */
static bool
isinformixdefine(void)
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

		yb->buffer = YY_CURRENT_BUFFER;
		yb->lineno = yylineno;
		yb->filename = mm_strdup(input_filename);
		yb->next = yy_buffer;
		yy_buffer = yb;

		yy_scan_string(new);
		return true;
	}

	return false;
}
