%top{
/*-------------------------------------------------------------------------
 *
 * scan.l
 *	  lexical scanner for PostgreSQL
 *
 * NOTE NOTE NOTE:
 *
 * The rules in this file must be kept in sync with src/fe_utils/psqlscan.l
 * and src/interfaces/ecpg/preproc/pgc.l!
 *
 * The rules are designed so that the scanner never has to backtrack,
 * in the sense that there is always a rule that can match the input
 * consumed so far (the rule action may internally throw back some input
 * with yyless(), however).  As explained in the flex manual, this makes
 * for a useful speed increase --- several percent faster when measuring
 * raw parsing (Flex + Bison).  The extra complexity is mostly in the rules
 * for handling float numbers and continued string literals.  If you change
 * the lexical rules, verify that you haven't broken the no-backtrack
 * property by running flex with the "-b" option and checking that the
 * resulting "lex.backup" file says that no backing up is needed.  (As of
 * Postgres 9.2, this check is made automatically by the Makefile.)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/parser/scan.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>

#include "common/string.h"
#include "gramparse.h"
#include "nodes/miscnodes.h"
#include "parser/parser.h"		/* only needed for GUC variables */
#include "parser/scansup.h"
#include "port/pg_bitutils.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
}

%{

/* LCOV_EXCL_START */

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

/*
 * GUC variables.  This is a DIRECT violation of the warning given at the
 * head of gram.y, ie flex/bison code must not depend on any GUC variables;
 * as such, changing their values can induce very unintuitive behavior.
 * But we shall have to live with it until we can remove these variables.
 */
int			backslash_quote = BACKSLASH_QUOTE_SAFE_ENCODING;
bool		escape_string_warning = true;
bool		standard_conforming_strings = true;

/*
 * Constant data exported from this file.  This array maps from the
 * zero-based keyword numbers returned by ScanKeywordLookup to the
 * Bison token numbers needed by gram.y.  This is exported because
 * callers need to pass it to scanner_init, if they are using the
 * standard keyword list ScanKeywords.
 */
#define PG_KEYWORD(kwname, value, category, collabel) value,

const uint16 ScanKeywordTokens[] = {
#include "parser/kwlist.h"
};

#undef PG_KEYWORD

/*
 * Set the type of YYSTYPE.
 */
#define YYSTYPE core_YYSTYPE

/*
 * Each call to yylex must set yylloc to the location of the found token
 * (expressed as a byte offset from the start of the input text).
 * When we parse a token that requires multiple lexer rules to process,
 * this should be done in the first such rule, else yylloc will point
 * into the middle of the token.
 */
#define SET_YYLLOC()  (*(yylloc) = yytext - yyextra->scanbuf)

/*
 * Advance yylloc by the given number of bytes.
 */
#define ADVANCE_YYLLOC(delta)  ( *(yylloc) += (delta) )

/*
 * Sometimes, we do want yylloc to point into the middle of a token; this is
 * useful for instance to throw an error about an escape sequence within a
 * string literal.  But if we find no error there, we want to revert yylloc
 * to the token start, so that that's the location reported to the parser.
 * Use PUSH_YYLLOC/POP_YYLLOC to save/restore yylloc around such code.
 * (Currently the implied "stack" is just one location, but someday we might
 * need to nest these.)
 */
#define PUSH_YYLLOC()	(yyextra->save_yylloc = *(yylloc))
#define POP_YYLLOC()	(*(yylloc) = yyextra->save_yylloc)

#define startlit()	( yyextra->literallen = 0 )
static void addlit(char *ytext, int yleng, core_yyscan_t yyscanner);
static void addlitchar(unsigned char ychar, core_yyscan_t yyscanner);
static char *litbufdup(core_yyscan_t yyscanner);
static unsigned char unescape_single_char(unsigned char c, core_yyscan_t yyscanner);
static int	process_integer_literal(const char *token, YYSTYPE *lval, int base);
static void addunicode(pg_wchar c, yyscan_t yyscanner);

#define yyerror(msg)  scanner_yyerror(msg, yyscanner)

#define lexer_errposition()  scanner_errposition(*(yylloc), yyscanner)

static void check_string_escape_warning(unsigned char ychar, core_yyscan_t yyscanner);
static void check_escape_warning(core_yyscan_t yyscanner);

%}

%option reentrant
%option bison-bridge
%option bison-locations
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option noyyalloc
%option noyyrealloc
%option noyyfree
%option warn
%option prefix="core_yy"
%option extra-type="core_yy_extra_type *"

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
 *  <xh> hexadecimal byte string
 *  <xq> standard quoted strings
 *  <xqs> quote stop (detect continued strings)
 *  <xe> extended quoted strings (support backslash escape sequences)
 *  <xdolq> $foo$ quoted strings
 *  <xui> quoted identifier with Unicode escapes
 *  <xus> quoted string with Unicode escapes
 *  <xeu> Unicode surrogate pair in extended quoted string
 *
 * Remember to add an <<EOF>> case whenever you add a new exclusive state!
 * The default one is probably not the right thing.
 */

%x xb
%x xc
%x xd
%x xh
%x xq
%x xqs
%x xe
%x xdolq
%x xui
%x xus
%x xeu

/*
 * In order to make the world safe for Windows and Mac clients as well as
 * Unix ones, we accept either \n or \r as a newline.  A DOS-style \r\n
 * sequence will be seen as two successive newlines, but that doesn't cause
 * any problems.  Comments that start with -- and extend to the next
 * newline are treated as equivalent to a single whitespace character.
 *
 * NOTE a fine point: if there is no newline following --, we will absorb
 * everything to the end of the input as a comment.  This is correct.  Older
 * versions of Postgres failed to recognize -- as a comment if the input
 * did not end with a newline.
 *
 * non_newline_space tracks all the other space characters except newlines.
 *
 * XXX if you change the set of whitespace characters, fix scanner_isspace()
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

special_whitespace		({space}+|{comment}{newline})
non_newline_whitespace	({non_newline_space}|{comment})
whitespace_with_newline	({non_newline_whitespace}*{newline}{special_whitespace}*)

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
 * It is tempting to scan the string for only those characters
 * which are allowed. However, this leads to silently swallowed
 * characters if illegal characters are included in the string.
 * For example, if xbinside is [01] then B'ABCD' is interpreted
 * as a zero-length string, and the ABCD' is lost!
 * Better to pass the string forward and let the input routines
 * validate the contents.
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
xeunicodefail	[\\](u[0-9A-Fa-f]{0,3}|U[0-9A-Fa-f]{0,7})

/* Extended quote
 * xqdouble implements embedded quote, ''''
 */
xqstart			{quote}
xqdouble		{quote}{quote}
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

/* error rule to avoid backup */
xufailed		[uU]&


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

/* Positional parameters don't accept underscores. */
param			\${decdigit}+

/*
 * An identifier immediately following an integer literal is disallowed because
 * in some cases it's ambiguous what is meant: for example, 0x1234 could be
 * either a hexinteger or a decinteger "0" and an identifier "x1234".  We can
 * detect such problems by seeing if integer_junk matches a longer substring
 * than any of the XXXinteger patterns (decinteger, hexinteger, octinteger,
 * bininteger).  One "junk" pattern is sufficient because
 * {decinteger}{identifier} will match all the same strings we'd match with
 * {hexinteger}{identifier} etc.
 *
 * Note that the rule for integer_junk must appear after the ones for
 * XXXinteger to make this work correctly: 0x1234 will match both hexinteger
 * and integer_junk, and we need hexinteger to be chosen in that case.
 *
 * Also disallow strings matched by numeric_junk, real_junk and param_junk
 * for consistency.
 */
integer_junk	{decinteger}{identifier}
numeric_junk	{numeric}{identifier}
real_junk		{real}{identifier}
param_junk		\${decdigit}+{identifier}

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

%%

{whitespace}	{
					/* ignore */
				}

{xcstart}		{
					/* Set location in case of syntax error in comment */
					SET_YYLLOC();
					yyextra->xcdepth = 0;
					BEGIN(xc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

<xc>{
{xcstart}		{
					(yyextra->xcdepth)++;
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

{xcstop}		{
					if (yyextra->xcdepth <= 0)
						BEGIN(INITIAL);
					else
						(yyextra->xcdepth)--;
				}

{xcinside}		{
					/* ignore */
				}

{op_chars}		{
					/* ignore */
				}

\*+				{
					/* ignore */
				}

<<EOF>>			{
					yyerror("unterminated /* comment");
				}
} /* <xc> */

{xbstart}		{
					/* Binary bit type.
					 * At some point we should simply pass the string
					 * forward to the parser and label it there.
					 * In the meantime, place a leading "b" on the string
					 * to mark it for the input routine as a binary string.
					 */
					SET_YYLLOC();
					BEGIN(xb);
					startlit();
					addlitchar('b', yyscanner);
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					addlit(yytext, yyleng, yyscanner);
				}
<xb><<EOF>>		{ yyerror("unterminated bit string literal"); }

{xhstart}		{
					/* Hexadecimal bit type.
					 * At some point we should simply pass the string
					 * forward to the parser and label it there.
					 * In the meantime, place a leading "x" on the string
					 * to mark it for the input routine as a hex string.
					 */
					SET_YYLLOC();
					BEGIN(xh);
					startlit();
					addlitchar('x', yyscanner);
				}
<xh><<EOF>>		{ yyerror("unterminated hexadecimal string literal"); }

{xnstart}		{
					/* National character.
					 * We will pass this along as a normal character string,
					 * but preceded with an internally-generated "NCHAR".
					 */
					int		kwnum;

					SET_YYLLOC();
					yyless(1);	/* eat only 'n' this time */

					kwnum = ScanKeywordLookup("nchar",
											  yyextra->keywordlist);
					if (kwnum >= 0)
					{
						yylval->keyword = GetScanKeyword(kwnum,
														 yyextra->keywordlist);
						return yyextra->keyword_tokens[kwnum];
					}
					else
					{
						/* If NCHAR isn't a keyword, just return "n" */
						yylval->str = pstrdup("n");
						return IDENT;
					}
				}

{xqstart}		{
					yyextra->warn_on_first_escape = true;
					yyextra->saw_non_ascii = false;
					SET_YYLLOC();
					if (yyextra->standard_conforming_strings)
						BEGIN(xq);
					else
						BEGIN(xe);
					startlit();
				}
{xestart}		{
					yyextra->warn_on_first_escape = false;
					yyextra->saw_non_ascii = false;
					SET_YYLLOC();
					BEGIN(xe);
					startlit();
				}
{xusstart}		{
					SET_YYLLOC();
					if (!yyextra->standard_conforming_strings)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("unsafe use of string constant with Unicode escapes"),
								 errdetail("String constants with Unicode escapes cannot be used when \"standard_conforming_strings\" is off."),
								 lexer_errposition()));
					BEGIN(xus);
					startlit();
				}

<xb,xh,xq,xe,xus>{quote} {
					/*
					 * When we are scanning a quoted string and see an end
					 * quote, we must look ahead for a possible continuation.
					 * If we don't see one, we know the end quote was in fact
					 * the end of the string.  To reduce the lexer table size,
					 * we use a single "xqs" state to do the lookahead for all
					 * types of strings.
					 */
					yyextra->state_before_str_stop = YYSTATE;
					BEGIN(xqs);
				}
<xqs>{quotecontinue} {
					/*
					 * Found a quote continuation, so return to the in-quote
					 * state and continue scanning the literal.  Nothing is
					 * added to the literal's contents.
					 */
					BEGIN(yyextra->state_before_str_stop);
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
					BEGIN(INITIAL);

					switch (yyextra->state_before_str_stop)
					{
						case xb:
							yylval->str = litbufdup(yyscanner);
							return BCONST;
						case xh:
							yylval->str = litbufdup(yyscanner);
							return XCONST;
						case xq:
						case xe:
							/*
							 * Check that the data remains valid, if it might
							 * have been made invalid by unescaping any chars.
							 */
							if (yyextra->saw_non_ascii)
								pg_verifymbstr(yyextra->literalbuf,
											   yyextra->literallen,
											   false);
							yylval->str = litbufdup(yyscanner);
							return SCONST;
						case xus:
							yylval->str = litbufdup(yyscanner);
							return USCONST;
						default:
							yyerror("unhandled previous state in xqs");
					}
				}

<xq,xe,xus>{xqdouble} {
					addlitchar('\'', yyscanner);
				}
<xq,xus>{xqinside}  {
					addlit(yytext, yyleng, yyscanner);
				}
<xe>{xeinside}  {
					addlit(yytext, yyleng, yyscanner);
				}
<xe>{xeunicode} {
					pg_wchar	c = strtoul(yytext + 2, NULL, 16);

					/*
					 * For consistency with other productions, issue any
					 * escape warning with cursor pointing to start of string.
					 * We might want to change that, someday.
					 */
					check_escape_warning(yyscanner);

					/* Remember start of overall string token ... */
					PUSH_YYLLOC();
					/* ... and set the error cursor to point at this esc seq */
					SET_YYLLOC();

					if (is_utf16_surrogate_first(c))
					{
						yyextra->utf16_first_part = c;
						BEGIN(xeu);
					}
					else if (is_utf16_surrogate_second(c))
						yyerror("invalid Unicode surrogate pair");
					else
						addunicode(c, yyscanner);

					/* Restore yylloc to be start of string token */
					POP_YYLLOC();
				}
<xeu>{xeunicode} {
					pg_wchar	c = strtoul(yytext + 2, NULL, 16);

					/* Remember start of overall string token ... */
					PUSH_YYLLOC();
					/* ... and set the error cursor to point at this esc seq */
					SET_YYLLOC();

					if (!is_utf16_surrogate_second(c))
						yyerror("invalid Unicode surrogate pair");

					c = surrogate_pair_to_codepoint(yyextra->utf16_first_part, c);

					addunicode(c, yyscanner);

					/* Restore yylloc to be start of string token */
					POP_YYLLOC();

					BEGIN(xe);
				}
<xeu>. |
<xeu>\n |
<xeu><<EOF>>	{
					/* Set the error cursor to point at missing esc seq */
					SET_YYLLOC();
					yyerror("invalid Unicode surrogate pair");
				}
<xe,xeu>{xeunicodefail}	{
					/* Set the error cursor to point at malformed esc seq */
					SET_YYLLOC();
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
							 errmsg("invalid Unicode escape"),
							 errhint("Unicode escapes must be \\uXXXX or \\UXXXXXXXX."),
							 lexer_errposition()));
				}
<xe>{xeescape}  {
					if (yytext[1] == '\'')
					{
						if (yyextra->backslash_quote == BACKSLASH_QUOTE_OFF ||
							(yyextra->backslash_quote == BACKSLASH_QUOTE_SAFE_ENCODING &&
							 PG_ENCODING_IS_CLIENT_ONLY(pg_get_client_encoding())))
							ereport(ERROR,
									(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
									 errmsg("unsafe use of \\' in a string literal"),
									 errhint("Use '' to write quotes in strings. \\' is insecure in client-only encodings."),
									 lexer_errposition()));
					}
					check_string_escape_warning(yytext[1], yyscanner);
					addlitchar(unescape_single_char(yytext[1], yyscanner),
							   yyscanner);
				}
<xe>{xeoctesc}  {
					unsigned char c = strtoul(yytext + 1, NULL, 8);

					check_escape_warning(yyscanner);
					addlitchar(c, yyscanner);
					if (c == '\0' || IS_HIGHBIT_SET(c))
						yyextra->saw_non_ascii = true;
				}
<xe>{xehexesc}  {
					unsigned char c = strtoul(yytext + 2, NULL, 16);

					check_escape_warning(yyscanner);
					addlitchar(c, yyscanner);
					if (c == '\0' || IS_HIGHBIT_SET(c))
						yyextra->saw_non_ascii = true;
				}
<xe>.			{
					/* This is only needed for \ just before EOF */
					addlitchar(yytext[0], yyscanner);
				}
<xq,xe,xus><<EOF>>		{ yyerror("unterminated quoted string"); }

{dolqdelim}		{
					SET_YYLLOC();
					yyextra->dolqstart = pstrdup(yytext);
					BEGIN(xdolq);
					startlit();
				}
{dolqfailed}	{
					SET_YYLLOC();
					/* throw back all but the initial "$" */
					yyless(1);
					/* and treat it as {other} */
					return yytext[0];
				}
<xdolq>{dolqdelim} {
					if (strcmp(yytext, yyextra->dolqstart) == 0)
					{
						pfree(yyextra->dolqstart);
						yyextra->dolqstart = NULL;
						BEGIN(INITIAL);
						yylval->str = litbufdup(yyscanner);
						return SCONST;
					}
					else
					{
						/*
						 * When we fail to match $...$ to dolqstart, transfer
						 * the $... part to the output, but put back the final
						 * $ for rescanning.  Consider $delim$...$junk$delim$
						 */
						addlit(yytext, yyleng - 1, yyscanner);
						yyless(yyleng - 1);
					}
				}
<xdolq>{dolqinside} {
					addlit(yytext, yyleng, yyscanner);
				}
<xdolq>{dolqfailed} {
					addlit(yytext, yyleng, yyscanner);
				}
<xdolq>.		{
					/* This is only needed for $ inside the quoted text */
					addlitchar(yytext[0], yyscanner);
				}
<xdolq><<EOF>>	{ yyerror("unterminated dollar-quoted string"); }

{xdstart}		{
					SET_YYLLOC();
					BEGIN(xd);
					startlit();
				}
{xuistart}		{
					SET_YYLLOC();
					BEGIN(xui);
					startlit();
				}
<xd>{xdstop}	{
					char	   *ident;

					BEGIN(INITIAL);
					if (yyextra->literallen == 0)
						yyerror("zero-length delimited identifier");
					ident = litbufdup(yyscanner);
					if (yyextra->literallen >= NAMEDATALEN)
						truncate_identifier(ident, yyextra->literallen, true);
					yylval->str = ident;
					return IDENT;
				}
<xui>{dquote}	{
					BEGIN(INITIAL);
					if (yyextra->literallen == 0)
						yyerror("zero-length delimited identifier");
					/* can't truncate till after we de-escape the ident */
					yylval->str = litbufdup(yyscanner);
					return UIDENT;
				}
<xd,xui>{xddouble}	{
					addlitchar('"', yyscanner);
				}
<xd,xui>{xdinside}	{
					addlit(yytext, yyleng, yyscanner);
				}
<xd,xui><<EOF>>		{ yyerror("unterminated quoted identifier"); }

{xufailed}	{
					char	   *ident;

					SET_YYLLOC();
					/* throw back all but the initial u/U */
					yyless(1);
					/* and treat it as {identifier} */
					ident = downcase_truncate_identifier(yytext, yyleng, true);
					yylval->str = ident;
					return IDENT;
				}

{typecast}		{
					SET_YYLLOC();
					return TYPECAST;
				}

{dot_dot}		{
					SET_YYLLOC();
					return DOT_DOT;
				}

{colon_equals}	{
					SET_YYLLOC();
					return COLON_EQUALS;
				}

{equals_greater} {
					SET_YYLLOC();
					return EQUALS_GREATER;
				}

{less_equals}	{
					SET_YYLLOC();
					return LESS_EQUALS;
				}

{greater_equals} {
					SET_YYLLOC();
					return GREATER_EQUALS;
				}

{less_greater}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS */
					SET_YYLLOC();
					return NOT_EQUALS;
				}

{not_equals}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS */
					SET_YYLLOC();
					return NOT_EQUALS;
				}

{self}			{
					SET_YYLLOC();
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
							char c = yytext[ic];
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
							do {
								nchars--;
							} while (nchars > 1 &&
								 (yytext[nchars - 1] == '+' ||
								  yytext[nchars - 1] == '-'));
						}
					}

					SET_YYLLOC();

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

					/*
					 * Complain if operator is too long.  Unlike the case
					 * for identifiers, we make this an error not a notice-
					 * and-truncate, because the odds are we are looking at
					 * a syntactic mistake anyway.
					 */
					if (nchars >= NAMEDATALEN)
						yyerror("operator too long");

					yylval->str = pstrdup(yytext);
					return Op;
				}

{param}			{
					ErrorSaveContext escontext = {T_ErrorSaveContext};
					int32		val;

					SET_YYLLOC();
					val = pg_strtoint32_safe(yytext + 1, (Node *) &escontext);
					if (escontext.error_occurred)
						yyerror("parameter number too large");
					yylval->ival = val;
					return PARAM;
				}
{param_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after parameter");
				}

{decinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 10);
				}
{hexinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 16);
				}
{octinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 8);
				}
{bininteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 2);
				}
{hexfail}		{
					SET_YYLLOC();
					yyerror("invalid hexadecimal integer");
				}
{octfail}		{
					SET_YYLLOC();
					yyerror("invalid octal integer");
				}
{binfail}		{
					SET_YYLLOC();
					yyerror("invalid binary integer");
				}
{numeric}		{
					SET_YYLLOC();
					yylval->str = pstrdup(yytext);
					return FCONST;
				}
{numericfail}	{
					/* throw back the .., and treat as integer */
					yyless(yyleng - 2);
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 10);
				}
{real}			{
					SET_YYLLOC();
					yylval->str = pstrdup(yytext);
					return FCONST;
				}
{realfail}		{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{integer_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{numeric_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{real_junk}		{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}


{identifier}	{
					int			kwnum;
					char	   *ident;

					SET_YYLLOC();

					/* Is it a keyword? */
					kwnum = ScanKeywordLookup(yytext,
											  yyextra->keywordlist);
					if (kwnum >= 0)
					{
						yylval->keyword = GetScanKeyword(kwnum,
														 yyextra->keywordlist);
						return yyextra->keyword_tokens[kwnum];
					}

					/*
					 * No.  Convert the identifier to lower case, and truncate
					 * if necessary.
					 */
					ident = downcase_truncate_identifier(yytext, yyleng, true);
					yylval->str = ident;
					return IDENT;
				}

{other}			{
					SET_YYLLOC();
					return yytext[0];
				}

<<EOF>>			{
					SET_YYLLOC();
					yyterminate();
				}

%%

/* LCOV_EXCL_STOP */

/*
 * Arrange access to yyextra for subroutines of the main yylex() function.
 * We expect each subroutine to have a yyscanner parameter.  Rather than
 * use the yyget_xxx functions, which might or might not get inlined by the
 * compiler, we cheat just a bit and cast yyscanner to the right type.
 */
#undef yyextra
#define yyextra  (((struct yyguts_t *) yyscanner)->yyextra_r)

/* Likewise for a couple of other things we need. */
#undef yylloc
#define yylloc	(((struct yyguts_t *) yyscanner)->yylloc_r)
#undef yyleng
#define yyleng	(((struct yyguts_t *) yyscanner)->yyleng_r)


/*
 * scanner_errposition
 *		Report a lexer or grammar error cursor position, if possible.
 *
 * This is expected to be used within an ereport() call, or via an error
 * callback such as setup_scanner_errposition_callback().  The return value
 * is a dummy (always 0, in fact).
 *
 * Note that this can only be used for messages emitted during raw parsing
 * (essentially, scan.l, parser.c, and gram.y), since it requires the
 * yyscanner struct to still be available.
 */
int
scanner_errposition(int location, core_yyscan_t yyscanner)
{
	int			pos;

	if (location < 0)
		return 0;				/* no-op if location is unknown */

	/* Convert byte offset to character number */
	pos = pg_mbstrlen_with_len(yyextra->scanbuf, location) + 1;
	/* And pass it to the ereport mechanism */
	return errposition(pos);
}

/*
 * Error context callback for inserting scanner error location.
 *
 * Note that this will be called for *any* error occurring while the
 * callback is installed.  We avoid inserting an irrelevant error location
 * if the error is a query cancel --- are there any other important cases?
 */
static void
scb_error_callback(void *arg)
{
	ScannerCallbackState *scbstate = (ScannerCallbackState *) arg;

	if (geterrcode() != ERRCODE_QUERY_CANCELED)
		(void) scanner_errposition(scbstate->location, scbstate->yyscanner);
}

/*
 * setup_scanner_errposition_callback
 *		Arrange for non-scanner errors to report an error position
 *
 * Sometimes the scanner calls functions that aren't part of the scanner
 * subsystem and can't reasonably be passed the yyscanner pointer; yet
 * we would like any errors thrown in those functions to be tagged with an
 * error location.  Use this function to set up an error context stack
 * entry that will accomplish that.  Usage pattern:
 *
 *		declare a local variable "ScannerCallbackState scbstate"
 *		...
 *		setup_scanner_errposition_callback(&scbstate, yyscanner, location);
 *		call function that might throw error;
 *		cancel_scanner_errposition_callback(&scbstate);
 */
void
setup_scanner_errposition_callback(ScannerCallbackState *scbstate,
								   core_yyscan_t yyscanner,
								   int location)
{
	/* Setup error traceback support for ereport() */
	scbstate->yyscanner = yyscanner;
	scbstate->location = location;
	scbstate->errcallback.callback = scb_error_callback;
	scbstate->errcallback.arg = scbstate;
	scbstate->errcallback.previous = error_context_stack;
	error_context_stack = &scbstate->errcallback;
}

/*
 * Cancel a previously-set-up errposition callback.
 */
void
cancel_scanner_errposition_callback(ScannerCallbackState *scbstate)
{
	/* Pop the error context stack */
	error_context_stack = scbstate->errcallback.previous;
}

/*
 * scanner_yyerror
 *		Report a lexer or grammar error.
 *
 * The message's cursor position is whatever YYLLOC was last set to,
 * ie, the start of the current token if called within yylex(), or the
 * most recently lexed token if called from the grammar.
 * This is OK for syntax error messages from the Bison parser, because Bison
 * parsers report error as soon as the first unparsable token is reached.
 * Beware of using yyerror for other purposes, as the cursor position might
 * be misleading!
 */
void
scanner_yyerror(const char *message, core_yyscan_t yyscanner)
{
	const char *loc = yyextra->scanbuf + *yylloc;

	if (*loc == YY_END_OF_BUFFER_CHAR)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: %s is typically the translation of "syntax error" */
				 errmsg("%s at end of input", _(message)),
				 lexer_errposition()));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: first %s is typically the translation of "syntax error" */
				 errmsg("%s at or near \"%s\"", _(message), loc),
				 lexer_errposition()));
	}
}


/*
 * Called before any actual parsing is done
 */
core_yyscan_t
scanner_init(const char *str,
			 core_yy_extra_type *yyext,
			 const ScanKeywordList *keywordlist,
			 const uint16 *keyword_tokens)
{
	Size		slen = strlen(str);
	yyscan_t	scanner;

	if (yylex_init(&scanner) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	core_yyset_extra(yyext, scanner);

	yyext->keywordlist = keywordlist;
	yyext->keyword_tokens = keyword_tokens;

	yyext->backslash_quote = backslash_quote;
	yyext->escape_string_warning = escape_string_warning;
	yyext->standard_conforming_strings = standard_conforming_strings;

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	yyext->scanbuf = (char *) palloc(slen + 2);
	yyext->scanbuflen = slen;
	memcpy(yyext->scanbuf, str, slen);
	yyext->scanbuf[slen] = yyext->scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	yy_scan_buffer(yyext->scanbuf, slen + 2, scanner);

	/* initialize literal buffer to a reasonable but expansible size */
	yyext->literalalloc = 1024;
	yyext->literalbuf = (char *) palloc(yyext->literalalloc);
	yyext->literallen = 0;

	return scanner;
}


/*
 * Called after parsing is done to clean up after scanner_init()
 */
void
scanner_finish(core_yyscan_t yyscanner)
{
	/*
	 * We don't bother to call yylex_destroy(), because all it would do is
	 * pfree a small amount of control storage.  It's cheaper to leak the
	 * storage until the parsing context is destroyed.  The amount of space
	 * involved is usually negligible compared to the output parse tree
	 * anyway.
	 *
	 * We do bother to pfree the scanbuf and literal buffer, but only if they
	 * represent a nontrivial amount of space.  The 8K cutoff is arbitrary.
	 */
	if (yyextra->scanbuflen >= 8192)
		pfree(yyextra->scanbuf);
	if (yyextra->literalalloc >= 8192)
		pfree(yyextra->literalbuf);
}


static void
addlit(char *ytext, int yleng, core_yyscan_t yyscanner)
{
	/* enlarge buffer if needed */
	if ((yyextra->literallen + yleng) >= yyextra->literalalloc)
	{
		yyextra->literalalloc = pg_nextpower2_32(yyextra->literallen + yleng + 1);
		yyextra->literalbuf = (char *) repalloc(yyextra->literalbuf,
												yyextra->literalalloc);
	}
	/* append new data */
	memcpy(yyextra->literalbuf + yyextra->literallen, ytext, yleng);
	yyextra->literallen += yleng;
}


static void
addlitchar(unsigned char ychar, core_yyscan_t yyscanner)
{
	/* enlarge buffer if needed */
	if ((yyextra->literallen + 1) >= yyextra->literalalloc)
	{
		yyextra->literalalloc *= 2;
		yyextra->literalbuf = (char *) repalloc(yyextra->literalbuf,
												yyextra->literalalloc);
	}
	/* append new data */
	yyextra->literalbuf[yyextra->literallen] = ychar;
	yyextra->literallen += 1;
}


/*
 * Create a palloc'd copy of literalbuf, adding a trailing null.
 */
static char *
litbufdup(core_yyscan_t yyscanner)
{
	int			llen = yyextra->literallen;
	char	   *new;

	new = palloc(llen + 1);
	memcpy(new, yyextra->literalbuf, llen);
	new[llen] = '\0';
	return new;
}

/*
 * Process {decinteger}, {hexinteger}, etc.  Note this will also do the right
 * thing with {numeric}, ie digits and a decimal point.
 */
static int
process_integer_literal(const char *token, YYSTYPE *lval, int base)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	int32		val;

	val = pg_strtoint32_safe(token, (Node *) &escontext);
	if (escontext.error_occurred)
	{
		/* integer too large (or contains decimal pt), treat it as a float */
		lval->str = pstrdup(token);
		return FCONST;
	}
	lval->ival = val;
	return ICONST;
}

static void
addunicode(pg_wchar c, core_yyscan_t yyscanner)
{
	ScannerCallbackState scbstate;
	char		buf[MAX_UNICODE_EQUIVALENT_STRING + 1];

	if (!is_valid_unicode_codepoint(c))
		yyerror("invalid Unicode escape value");

	/*
	 * We expect that pg_unicode_to_server() will complain about any
	 * unconvertible code point, so we don't have to set saw_non_ascii.
	 */
	setup_scanner_errposition_callback(&scbstate, yyscanner, *(yylloc));
	pg_unicode_to_server(c, (unsigned char *) buf);
	cancel_scanner_errposition_callback(&scbstate);
	addlit(buf, strlen(buf), yyscanner);
}

static unsigned char
unescape_single_char(unsigned char c, core_yyscan_t yyscanner)
{
	switch (c)
	{
		case 'b':
			return '\b';
		case 'f':
			return '\f';
		case 'n':
			return '\n';
		case 'r':
			return '\r';
		case 't':
			return '\t';
		case 'v':
			return '\v';
		default:
			/* check for backslash followed by non-7-bit-ASCII */
			if (c == '\0' || IS_HIGHBIT_SET(c))
				yyextra->saw_non_ascii = true;

			return c;
	}
}

static void
check_string_escape_warning(unsigned char ychar, core_yyscan_t yyscanner)
{
	if (ychar == '\'')
	{
		if (yyextra->warn_on_first_escape && yyextra->escape_string_warning)
			ereport(WARNING,
					(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
					 errmsg("nonstandard use of \\' in a string literal"),
					 errhint("Use '' to write quotes in strings, or use the escape string syntax (E'...')."),
					 lexer_errposition()));
		yyextra->warn_on_first_escape = false;	/* warn only once per string */
	}
	else if (ychar == '\\')
	{
		if (yyextra->warn_on_first_escape && yyextra->escape_string_warning)
			ereport(WARNING,
					(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
					 errmsg("nonstandard use of \\\\ in a string literal"),
					 errhint("Use the escape string syntax for backslashes, e.g., E'\\\\'."),
					 lexer_errposition()));
		yyextra->warn_on_first_escape = false;	/* warn only once per string */
	}
	else
		check_escape_warning(yyscanner);
}

static void
check_escape_warning(core_yyscan_t yyscanner)
{
	if (yyextra->warn_on_first_escape && yyextra->escape_string_warning)
		ereport(WARNING,
				(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
				 errmsg("nonstandard use of escape in a string literal"),
				 errhint("Use the escape string syntax for escapes, e.g., E'\\r\\n'."),
				 lexer_errposition()));
	yyextra->warn_on_first_escape = false;	/* warn only once per string */
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 */

void *
core_yyalloc(yy_size_t bytes, core_yyscan_t yyscanner)
{
	return palloc(bytes);
}

void *
core_yyrealloc(void *ptr, yy_size_t bytes, core_yyscan_t yyscanner)
{
	if (ptr)
		return repalloc(ptr, bytes);
	else
		return palloc(bytes);
}

void
core_yyfree(void *ptr, core_yyscan_t yyscanner)
{
	if (ptr)
		pfree(ptr);
}
