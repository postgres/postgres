%top{
/*-------------------------------------------------------------------------
 *
 * psqlscan.l
 *	  lexical scanner for SQL commands
 *
 * This lexer used to be part of psql, and that heritage is reflected in
 * the file name as well as function and typedef names, though it can now
 * be used by other frontend programs as well.  It's also possible to extend
 * this lexer with a compatible add-on lexer to handle program-specific
 * backslash commands.
 *
 * This code is mainly concerned with determining where the end of a SQL
 * statement is: we are looking for semicolons that are not within quotes,
 * comments, or parentheses.  The most reliable way to handle this is to
 * borrow the backend's flex lexer rules, lock, stock, and barrel.  The rules
 * below are (except for a few) the same as the backend's, but their actions
 * are just ECHO whereas the backend's actions generally do other things.
 *
 * XXX The rules in this file must be kept in sync with the backend lexer!!!
 *
 * XXX Avoid creating backtracking cases --- see the backend lexer for info.
 *
 * See psqlscan_int.h for additional commentary.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/fe_utils/psqlscan.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "fe_utils/psqlscan.h"

#include "libpq-fe.h"
}

%{

/* LCOV_EXCL_START */

#include "fe_utils/psqlscan_int.h"

/*
 * We must have a typedef YYSTYPE for yylex's first argument, but this lexer
 * doesn't presently make use of that argument, so just declare it as int.
 */
typedef int YYSTYPE;


/* Return values from yylex() */
#define LEXRES_EOL			0	/* end of input */
#define LEXRES_SEMI			1	/* command-terminating semicolon found */
#define LEXRES_BACKSLASH	2	/* backslash command start */


#define ECHO psqlscan_emit(cur_state, yytext, yyleng)

%}

%option reentrant
%option bison-bridge
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="psql_yy"

/*
 * Set the type of yyextra; we use it as a pointer back to the containing
 * PsqlScanState.
 */
%option extra-type="PsqlScanState"

/*
 * All of the following definitions and rules should exactly match
 * src/backend/parser/scan.l so far as the flex patterns are concerned.
 * The rule bodies are just ECHO as opposed to what the backend does,
 * however.  (But be sure to duplicate code that affects the lexing process,
 * such as BEGIN() and yyless().)  Also, psqlscan uses a single <<EOF>> rule
 * whereas scan.l has a separate one for each exclusive state.
 */

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
 *
 * Note: we intentionally don't mimic the backend's <xeu> state; we have
 * no need to distinguish it from <xe> state, and no good way to get out
 * of it in error cases.  The backend just throws yyerror() in those
 * cases, but that's not an option here.
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
 * non_newline_space tracks all space characters except newlines.
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

/* psql-specific: characters allowed in variable names */
variable_char	[A-Za-z\200-\377_0-9]

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

%{
		/* Declare some local variables inside yylex(), for convenience */
		PsqlScanState cur_state = yyextra;
		PQExpBuffer output_buf = cur_state->output_buf;

		/*
		 * Force flex into the state indicated by start_state.  This has a
		 * couple of purposes: it lets some of the functions below set a new
		 * starting state without ugly direct access to flex variables, and it
		 * allows us to transition from one flex lexer to another so that we
		 * can lex different parts of the source string using separate lexers.
		 */
		BEGIN(cur_state->start_state);
%}

{whitespace}	{
					/*
					 * Note that the whitespace rule includes both true
					 * whitespace and single-line ("--" style) comments.
					 * We suppress whitespace until we have collected some
					 * non-whitespace data.  (This interacts with some
					 * decisions in MainLoop(); see there for details.)
					 */
					if (output_buf->len > 0)
						ECHO;
				}

{xcstart}		{
					cur_state->xcdepth = 0;
					BEGIN(xc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
					ECHO;
				}

<xc>{
{xcstart}		{
					cur_state->xcdepth++;
					/* Put back any characters past slash-star; see above */
					yyless(2);
					ECHO;
				}

{xcstop}		{
					if (cur_state->xcdepth <= 0)
						BEGIN(INITIAL);
					else
						cur_state->xcdepth--;
					ECHO;
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
} /* <xc> */

{xbstart}		{
					BEGIN(xb);
					ECHO;
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					ECHO;
				}

{xhstart}		{
					/* Hexadecimal bit type.
					 * At some point we should simply pass the string
					 * forward to the parser and label it there.
					 * In the meantime, place a leading "x" on the string
					 * to mark it for the input routine as a hex string.
					 */
					BEGIN(xh);
					ECHO;
				}

{xnstart}		{
					yyless(1);	/* eat only 'n' this time */
					ECHO;
				}

{xqstart}		{
					if (cur_state->std_strings)
						BEGIN(xq);
					else
						BEGIN(xe);
					ECHO;
				}
{xestart}		{
					BEGIN(xe);
					ECHO;
				}
{xusstart}		{
					BEGIN(xus);
					ECHO;
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
					cur_state->state_before_str_stop = YYSTATE;
					BEGIN(xqs);
					ECHO;
				}
<xqs>{quotecontinue} {
					/*
					 * Found a quote continuation, so return to the in-quote
					 * state and continue scanning the literal.  Nothing is
					 * added to the literal's contents.
					 */
					BEGIN(cur_state->state_before_str_stop);
					ECHO;
				}
<xqs>{quotecontinuefail} |
<xqs>{other}	{
					/*
					 * Failed to see a quote continuation.  Throw back
					 * everything after the end quote, and handle the string
					 * according to the state we were in previously.
					 */
					yyless(0);
					BEGIN(INITIAL);
					/* There's nothing to echo ... */
				}

<xq,xe,xus>{xqdouble} {
					ECHO;
				}
<xq,xus>{xqinside}  {
					ECHO;
				}
<xe>{xeinside}  {
					ECHO;
				}
<xe>{xeunicode} {
					ECHO;
				}
<xe>{xeunicodefail}	{
					ECHO;
				}
<xe>{xeescape}  {
					ECHO;
				}
<xe>{xeoctesc}  {
					ECHO;
				}
<xe>{xehexesc}  {
					ECHO;
				}
<xe>.			{
					/* This is only needed for \ just before EOF */
					ECHO;
				}

{dolqdelim}		{
					cur_state->dolqstart = pg_strdup(yytext);
					BEGIN(xdolq);
					ECHO;
				}
{dolqfailed}	{
					/* throw back all but the initial "$" */
					yyless(1);
					ECHO;
				}
<xdolq>{dolqdelim} {
					if (strcmp(yytext, cur_state->dolqstart) == 0)
					{
						free(cur_state->dolqstart);
						cur_state->dolqstart = NULL;
						BEGIN(INITIAL);
					}
					else
					{
						/*
						 * When we fail to match $...$ to dolqstart, transfer
						 * the $... part to the output, but put back the final
						 * $ for rescanning.  Consider $delim$...$junk$delim$
						 */
						yyless(yyleng - 1);
					}
					ECHO;
				}
<xdolq>{dolqinside} {
					ECHO;
				}
<xdolq>{dolqfailed} {
					ECHO;
				}
<xdolq>.		{
					/* This is only needed for $ inside the quoted text */
					ECHO;
				}

{xdstart}		{
					BEGIN(xd);
					ECHO;
				}
{xuistart}		{
					BEGIN(xui);
					ECHO;
				}
<xd>{xdstop}	{
					BEGIN(INITIAL);
					ECHO;
				}
<xui>{dquote}	{
					BEGIN(INITIAL);
					ECHO;
				}
<xd,xui>{xddouble}	{
					ECHO;
				}
<xd,xui>{xdinside}	{
					ECHO;
				}

{xufailed}	{
					/* throw back all but the initial u/U */
					yyless(1);
					ECHO;
				}

{typecast}		{
					ECHO;
				}

{dot_dot}		{
					ECHO;
				}

{colon_equals}	{
					ECHO;
				}

{equals_greater} {
					ECHO;
				}

{less_equals}	{
					ECHO;
				}

{greater_equals} {
					ECHO;
				}

{less_greater}	{
					ECHO;
				}

{not_equals}	{
					ECHO;
				}

	/*
	 * These rules are specific to psql --- they implement parenthesis
	 * counting and detection of command-ending semicolon.  These must
	 * appear before the {self} rule so that they take precedence over it.
	 */

"("				{
					cur_state->paren_depth++;
					ECHO;
				}

")"				{
					if (cur_state->paren_depth > 0)
						cur_state->paren_depth--;
					ECHO;
				}

";"				{
					ECHO;
					if (cur_state->paren_depth == 0 && cur_state->begin_depth == 0)
					{
						/* Terminate lexing temporarily */
						cur_state->start_state = YY_START;
						cur_state->identifier_count = 0;
						return LEXRES_SEMI;
					}
				}

	/*
	 * psql-specific rules to handle backslash commands and variable
	 * substitution.  We want these before {self}, also.
	 */

"\\"[;:]		{
					/* Force a semi-colon or colon into the query buffer */
					psqlscan_emit(cur_state, yytext + 1, 1);
					if (yytext[1] == ';')
						cur_state->identifier_count = 0;
				}

"\\"			{
					/* Terminate lexing temporarily */
					cur_state->start_state = YY_START;
					return LEXRES_BACKSLASH;
				}

:{variable_char}+	{
					/* Possible psql variable substitution */
					char	   *varname;
					char	   *value;

					varname = psqlscan_extract_substring(cur_state,
														 yytext + 1,
														 yyleng - 1);
					if (cur_state->callbacks->get_variable)
						value = cur_state->callbacks->get_variable(varname,
																   PQUOTE_PLAIN,
																   cur_state->cb_passthrough);
					else
						value = NULL;

					if (value)
					{
						/* It is a variable, check for recursion */
						if (psqlscan_var_is_current_source(cur_state, varname))
						{
							/* Recursive expansion --- don't go there */
							pg_log_warning("skipping recursive expansion of variable \"%s\"",
															  varname);
							/* Instead copy the string as is */
							ECHO;
						}
						else
						{
							/* OK, perform substitution */
							psqlscan_push_new_buffer(cur_state, value, varname);
							/* yy_scan_string already made buffer active */
						}
						free(value);
					}
					else
					{
						/*
						 * if the variable doesn't exist we'll copy the string
						 * as is
						 */
						ECHO;
					}

					free(varname);
				}

:'{variable_char}+'	{
					psqlscan_escape_variable(cur_state, yytext, yyleng,
											 PQUOTE_SQL_LITERAL);
				}

:\"{variable_char}+\"	{
					psqlscan_escape_variable(cur_state, yytext, yyleng,
											 PQUOTE_SQL_IDENT);
				}

:\{\?{variable_char}+\}	{
					psqlscan_test_variable(cur_state, yytext, yyleng);
				}

	/*
	 * These rules just avoid the need for scanner backup if one of the
	 * three rules above fails to match completely.
	 */

:'{variable_char}*	{
					/* Throw back everything but the colon */
					yyless(1);
					ECHO;
				}

:\"{variable_char}*	{
					/* Throw back everything but the colon */
					yyless(1);
					ECHO;
				}

:\{\?{variable_char}*	{
					/* Throw back everything but the colon */
					yyless(1);
					ECHO;
				}
:\{	{
					/* Throw back everything but the colon */
					yyless(1);
					ECHO;
				}

	/*
	 * Back to backend-compatible rules.
	 */

{self}			{
					ECHO;
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

					if (nchars < yyleng)
					{
						/* Strip the unwanted chars from the token */
						yyless(nchars);
					}
					ECHO;
				}

{param}			{
					ECHO;
				}
{param_junk}	{
					ECHO;
				}

{decinteger}	{
					ECHO;
				}
{hexinteger}	{
					ECHO;
				}
{octinteger}	{
					ECHO;
				}
{bininteger}	{
					ECHO;
				}
{hexfail}		{
					ECHO;
				}
{octfail}		{
					ECHO;
				}
{binfail}		{
					ECHO;
				}
{numeric}		{
					ECHO;
				}
{numericfail}	{
					/* throw back the .., and treat as integer */
					yyless(yyleng - 2);
					ECHO;
				}
{real}			{
					ECHO;
				}
{realfail}		{
					ECHO;
				}
{integer_junk}	{
					ECHO;
				}
{numeric_junk}	{
					ECHO;
				}
{real_junk}		{
					ECHO;
				}


{identifier}	{
					/*
					 * We need to track if we are inside a BEGIN .. END block
					 * in a function definition, so that semicolons contained
					 * therein don't terminate the whole statement.  Short of
					 * writing a full parser here, the following heuristic
					 * should work.  First, we track whether the beginning of
					 * the statement matches CREATE [OR REPLACE]
					 * {FUNCTION|PROCEDURE}
					 */

					if (cur_state->identifier_count == 0)
						memset(cur_state->identifiers, 0, sizeof(cur_state->identifiers));

					if (pg_strcasecmp(yytext, "create") == 0 ||
						pg_strcasecmp(yytext, "function") == 0 ||
						pg_strcasecmp(yytext, "procedure") == 0 ||
						pg_strcasecmp(yytext, "or") == 0 ||
						pg_strcasecmp(yytext, "replace") == 0)
					{
						if (cur_state->identifier_count < sizeof(cur_state->identifiers))
							cur_state->identifiers[cur_state->identifier_count] = pg_tolower((unsigned char) yytext[0]);
					}

					cur_state->identifier_count++;

					if (cur_state->identifiers[0] == 'c' &&
						(cur_state->identifiers[1] == 'f' || cur_state->identifiers[1] == 'p' ||
						 (cur_state->identifiers[1] == 'o' && cur_state->identifiers[2] == 'r' &&
						  (cur_state->identifiers[3] == 'f' || cur_state->identifiers[3] == 'p'))) &&
						cur_state->paren_depth == 0)
					{
						if (pg_strcasecmp(yytext, "begin") == 0)
							cur_state->begin_depth++;
						else if (pg_strcasecmp(yytext, "case") == 0)
						{
							/*
							 * CASE also ends with END.  We only need to track
							 * this if we are already inside a BEGIN.
							 */
							if (cur_state->begin_depth >= 1)
								cur_state->begin_depth++;
						}
						else if (pg_strcasecmp(yytext, "end") == 0)
						{
							if (cur_state->begin_depth > 0)
								cur_state->begin_depth--;
						}
					}

					ECHO;
				}

{other}			{
					ECHO;
				}

<<EOF>>			{
					if (cur_state->buffer_stack == NULL)
					{
						cur_state->start_state = YY_START;
						return LEXRES_EOL;		/* end of input reached */
					}

					/*
					 * We were expanding a variable, so pop the inclusion
					 * stack and keep lexing
					 */
					psqlscan_pop_buffer_stack(cur_state);
					psqlscan_select_top_buffer(cur_state);
				}

%%

/* LCOV_EXCL_STOP */

/*
 * Create a lexer working state struct.
 *
 * callbacks is a struct of function pointers that encapsulate some
 * behavior we need from the surrounding program.  This struct must
 * remain valid for the lifespan of the PsqlScanState.
 */
PsqlScanState
psql_scan_create(const PsqlScanCallbacks *callbacks)
{
	PsqlScanState state;

	state = (PsqlScanStateData *) pg_malloc0(sizeof(PsqlScanStateData));

	state->callbacks = callbacks;

	yylex_init(&state->scanner);

	yyset_extra(state, state->scanner);

	psql_scan_reset(state);

	return state;
}

/*
 * Destroy a lexer working state struct, releasing all resources.
 */
void
psql_scan_destroy(PsqlScanState state)
{
	psql_scan_finish(state);

	psql_scan_reset(state);

	yylex_destroy(state->scanner);

	free(state);
}

/*
 * Set the callback passthrough pointer for the lexer.
 *
 * This could have been integrated into psql_scan_create, but keeping it
 * separate allows the application to change the pointer later, which might
 * be useful.
 */
void
psql_scan_set_passthrough(PsqlScanState state, void *passthrough)
{
	state->cb_passthrough = passthrough;
}

/*
 * Set up to perform lexing of the given input line.
 *
 * The text at *line, extending for line_len bytes, will be scanned by
 * subsequent calls to the psql_scan routines.  psql_scan_finish should
 * be called when scanning is complete.  Note that the lexer retains
 * a pointer to the storage at *line --- this string must not be altered
 * or freed until after psql_scan_finish is called.
 *
 * encoding is the libpq identifier for the character encoding in use,
 * and std_strings says whether standard_conforming_strings is on.
 */
void
psql_scan_setup(PsqlScanState state,
				const char *line, int line_len,
				int encoding, bool std_strings)
{
	/* Mustn't be scanning already */
	Assert(state->scanbufhandle == NULL);
	Assert(state->buffer_stack == NULL);

	/* Do we need to hack the character set encoding? */
	state->encoding = encoding;
	state->safe_encoding = pg_valid_server_encoding_id(encoding);

	/* Save standard-strings flag as well */
	state->std_strings = std_strings;

	/* Set up flex input buffer with appropriate translation and padding */
	state->scanbufhandle = psqlscan_prepare_buffer(state, line, line_len,
												   &state->scanbuf);
	state->scanline = line;

	/* Set lookaside data in case we have to map unsafe encoding */
	state->curline = state->scanbuf;
	state->refline = state->scanline;

	/* Initialize state for psql_scan_get_location() */
	state->cur_line_no = 0;		/* yylex not called yet */
	state->cur_line_ptr = state->scanbuf;
}

/*
 * Do lexical analysis of SQL command text.
 *
 * The text previously passed to psql_scan_setup is scanned, and appended
 * (possibly with transformation) to query_buf.
 *
 * The return value indicates the condition that stopped scanning:
 *
 * PSCAN_SEMICOLON: found a command-ending semicolon.  (The semicolon is
 * transferred to query_buf.)  The command accumulated in query_buf should
 * be executed, then clear query_buf and call again to scan the remainder
 * of the line.
 *
 * PSCAN_BACKSLASH: found a backslash that starts a special command.
 * Any previous data on the line has been transferred to query_buf.
 * The caller will typically next apply a separate flex lexer to scan
 * the special command.
 *
 * PSCAN_INCOMPLETE: the end of the line was reached, but we have an
 * incomplete SQL command.  *prompt is set to the appropriate prompt type.
 *
 * PSCAN_EOL: the end of the line was reached, and there is no lexical
 * reason to consider the command incomplete.  The caller may or may not
 * choose to send it.  *prompt is set to the appropriate prompt type if
 * the caller chooses to collect more input.
 *
 * In the PSCAN_INCOMPLETE and PSCAN_EOL cases, psql_scan_finish() should
 * be called next, then the cycle may be repeated with a fresh input line.
 *
 * In all cases, *prompt is set to an appropriate prompt type code for the
 * next line-input operation.
 */
PsqlScanResult
psql_scan(PsqlScanState state,
		  PQExpBuffer query_buf,
		  promptStatus_t *prompt)
{
	PsqlScanResult result;
	int			lexresult;

	/* Must be scanning already */
	Assert(state->scanbufhandle != NULL);

	/* Set current output target */
	state->output_buf = query_buf;

	/* Set input source */
	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf, state->scanner);
	else
		yy_switch_to_buffer(state->scanbufhandle, state->scanner);

	/* And lex. */
	lexresult = yylex(NULL, state->scanner);

	/* Notify psql_scan_get_location() that a yylex call has been made. */
	if (state->cur_line_no == 0)
		state->cur_line_no = 1;

	/*
	 * Check termination state and return appropriate result info.
	 */
	switch (lexresult)
	{
		case LEXRES_EOL:		/* end of input */
			switch (state->start_state)
			{
				case INITIAL:
				case xqs:		/* we treat this like INITIAL */
					if (state->paren_depth > 0)
					{
						result = PSCAN_INCOMPLETE;
						*prompt = PROMPT_PAREN;
					}
					else if (state->begin_depth > 0)
					{
						result = PSCAN_INCOMPLETE;
						*prompt = PROMPT_CONTINUE;
					}
					else if (query_buf->len > 0)
					{
						result = PSCAN_EOL;
						*prompt = PROMPT_CONTINUE;
					}
					else
					{
						/* never bother to send an empty buffer */
						result = PSCAN_INCOMPLETE;
						*prompt = PROMPT_READY;
					}
					break;
				case xb:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_SINGLEQUOTE;
					break;
				case xc:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_COMMENT;
					break;
				case xd:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_DOUBLEQUOTE;
					break;
				case xh:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_SINGLEQUOTE;
					break;
				case xe:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_SINGLEQUOTE;
					break;
				case xq:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_SINGLEQUOTE;
					break;
				case xdolq:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_DOLLARQUOTE;
					break;
				case xui:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_DOUBLEQUOTE;
					break;
				case xus:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_SINGLEQUOTE;
					break;
				default:
					/* can't get here */
					fprintf(stderr, "invalid YY_START\n");
					exit(1);
			}
			break;
		case LEXRES_SEMI:		/* semicolon */
			result = PSCAN_SEMICOLON;
			*prompt = PROMPT_READY;
			break;
		case LEXRES_BACKSLASH:	/* backslash */
			result = PSCAN_BACKSLASH;
			*prompt = PROMPT_READY;
			break;
		default:
			/* can't get here */
			fprintf(stderr, "invalid yylex result\n");
			exit(1);
	}

	return result;
}

/*
 * Clean up after scanning a string.  This flushes any unread input and
 * releases resources (but not the PsqlScanState itself).  Note however
 * that this does not reset the lexer scan state; that can be done by
 * psql_scan_reset(), which is an orthogonal operation.
 *
 * It is legal to call this when not scanning anything (makes it easier
 * to deal with error recovery).
 */
void
psql_scan_finish(PsqlScanState state)
{
	/* Drop any incomplete variable expansions. */
	while (state->buffer_stack != NULL)
		psqlscan_pop_buffer_stack(state);

	/* Done with the outer scan buffer, too */
	if (state->scanbufhandle)
		yy_delete_buffer(state->scanbufhandle, state->scanner);
	state->scanbufhandle = NULL;
	if (state->scanbuf)
		free(state->scanbuf);
	state->scanbuf = NULL;
}

/*
 * Reset lexer scanning state to start conditions.  This is appropriate
 * for executing \r psql commands (or any other time that we discard the
 * prior contents of query_buf).  It is not, however, necessary to do this
 * when we execute and clear the buffer after getting a PSCAN_SEMICOLON or
 * PSCAN_EOL scan result, because the scan state must be INITIAL when those
 * conditions are returned.
 *
 * Note that this is unrelated to flushing unread input; that task is
 * done by psql_scan_finish().
 */
void
psql_scan_reset(PsqlScanState state)
{
	state->start_state = INITIAL;
	state->paren_depth = 0;
	state->xcdepth = 0;			/* not really necessary */
	if (state->dolqstart)
		free(state->dolqstart);
	state->dolqstart = NULL;
	state->identifier_count = 0;
	state->begin_depth = 0;
}

/*
 * Reselect this lexer (psqlscan.l) after using another one.
 *
 * Currently and for foreseeable uses, it's sufficient to reset to INITIAL
 * state, because we'd never switch to another lexer in a different state.
 * However, we don't want to reset e.g. paren_depth, so this can't be
 * the same as psql_scan_reset().
 *
 * Note: psql setjmp error recovery just calls psql_scan_reset(), so that
 * must be a superset of this.
 *
 * Note: it seems likely that other lexers could just assign INITIAL for
 * themselves, since that probably has the value zero in every flex-generated
 * lexer.  But let's not assume that.
 */
void
psql_scan_reselect_sql_lexer(PsqlScanState state)
{
	state->start_state = INITIAL;
}

/*
 * Return true if lexer is currently in an "inside quotes" state.
 *
 * This is pretty grotty but is needed to preserve the old behavior
 * that mainloop.c drops blank lines not inside quotes without even
 * echoing them.
 */
bool
psql_scan_in_quote(PsqlScanState state)
{
	return state->start_state != INITIAL &&
		state->start_state != xqs;
}

/*
 * Return the current scanning location (end+1 of last scanned token),
 * as a line number counted from 1 and an offset from string start.
 *
 * This considers only the outermost input string, and therefore is of
 * limited use for programs that use psqlscan_push_new_buffer().
 *
 * It would be a bit easier probably to use "%option yylineno" to count
 * lines, but the flex manual says that has a performance cost, and only
 * a minority of programs using psqlscan have need for this functionality.
 * So we implement it ourselves without adding overhead to the lexer itself.
 */
void
psql_scan_get_location(PsqlScanState state,
					   int *lineno, int *offset)
{
	const char *line_end;

	/*
	 * We rely on flex's having stored a NUL after the current token in
	 * scanbuf.  Therefore we must specially handle the state before yylex()
	 * has been called, when obviously that won't have happened yet.
	 */
	if (state->cur_line_no == 0)
	{
		*lineno = 1;
		*offset = 0;
		return;
	}

	/*
	 * Advance cur_line_no/cur_line_ptr past whatever has been lexed so far.
	 * Doing this prevents repeated calls from being O(N^2) for long inputs.
	 */
	while ((line_end = strchr(state->cur_line_ptr, '\n')) != NULL)
	{
		state->cur_line_no++;
		state->cur_line_ptr = line_end + 1;
	}
	state->cur_line_ptr += strlen(state->cur_line_ptr);

	/* Report current location. */
	*lineno = state->cur_line_no;
	*offset = state->cur_line_ptr - state->scanbuf;
}

/*
 * Push the given string onto the stack of stuff to scan.
 *
 * NOTE SIDE EFFECT: the new buffer is made the active flex input buffer.
 */
void
psqlscan_push_new_buffer(PsqlScanState state, const char *newstr,
						 const char *varname)
{
	StackElem  *stackelem;

	stackelem = (StackElem *) pg_malloc(sizeof(StackElem));

	/*
	 * In current usage, the passed varname points at the current flex input
	 * buffer; we must copy it before calling psqlscan_prepare_buffer()
	 * because that will change the buffer state.
	 */
	stackelem->varname = varname ? pg_strdup(varname) : NULL;

	stackelem->buf = psqlscan_prepare_buffer(state, newstr, strlen(newstr),
											 &stackelem->bufstring);
	state->curline = stackelem->bufstring;
	if (state->safe_encoding)
	{
		stackelem->origstring = NULL;
		state->refline = stackelem->bufstring;
	}
	else
	{
		stackelem->origstring = pg_strdup(newstr);
		state->refline = stackelem->origstring;
	}
	stackelem->next = state->buffer_stack;
	state->buffer_stack = stackelem;
}

/*
 * Pop the topmost buffer stack item (there must be one!)
 *
 * NB: after this, the flex input state is unspecified; caller must
 * switch to an appropriate buffer to continue lexing.
 * See psqlscan_select_top_buffer().
 */
void
psqlscan_pop_buffer_stack(PsqlScanState state)
{
	StackElem  *stackelem = state->buffer_stack;

	state->buffer_stack = stackelem->next;
	yy_delete_buffer(stackelem->buf, state->scanner);
	free(stackelem->bufstring);
	if (stackelem->origstring)
		free(stackelem->origstring);
	if (stackelem->varname)
		free(stackelem->varname);
	free(stackelem);
}

/*
 * Select the topmost surviving buffer as the active input.
 */
void
psqlscan_select_top_buffer(PsqlScanState state)
{
	StackElem  *stackelem = state->buffer_stack;

	if (stackelem != NULL)
	{
		yy_switch_to_buffer(stackelem->buf, state->scanner);
		state->curline = stackelem->bufstring;
		state->refline = stackelem->origstring ? stackelem->origstring : stackelem->bufstring;
	}
	else
	{
		yy_switch_to_buffer(state->scanbufhandle, state->scanner);
		state->curline = state->scanbuf;
		state->refline = state->scanline;
	}
}

/*
 * Check if specified variable name is the source for any string
 * currently being scanned
 */
bool
psqlscan_var_is_current_source(PsqlScanState state, const char *varname)
{
	StackElem  *stackelem;

	for (stackelem = state->buffer_stack;
		 stackelem != NULL;
		 stackelem = stackelem->next)
	{
		if (stackelem->varname && strcmp(stackelem->varname, varname) == 0)
			return true;
	}
	return false;
}

/*
 * Set up a flex input buffer to scan the given data.  We always make a
 * copy of the data.  If working in an unsafe encoding, the copy has
 * multibyte sequences replaced by FFs to avoid fooling the lexer rules.
 *
 * NOTE SIDE EFFECT: the new buffer is made the active flex input buffer.
 */
YY_BUFFER_STATE
psqlscan_prepare_buffer(PsqlScanState state, const char *txt, int len,
						char **txtcopy)
{
	char	   *newtxt;

	/* Flex wants two \0 characters after the actual data */
	newtxt = pg_malloc(len + 2);
	*txtcopy = newtxt;
	newtxt[len] = newtxt[len + 1] = YY_END_OF_BUFFER_CHAR;

	if (state->safe_encoding)
		memcpy(newtxt, txt, len);
	else
	{
		/* Gotta do it the hard way */
		int			i = 0;

		while (i < len)
		{
			int			thislen = PQmblen(txt + i, state->encoding);

			/* first byte should always be okay... */
			newtxt[i] = txt[i];
			i++;
			while (--thislen > 0 && i < len)
				newtxt[i++] = (char) 0xFF;
		}
	}

	return yy_scan_buffer(newtxt, len + 2, state->scanner);
}

/*
 * psqlscan_emit() --- body for ECHO macro
 *
 * NB: this must be used for ALL and ONLY the text copied from the flex
 * input data.  If you pass it something that is not part of the yytext
 * string, you are making a mistake.  Internally generated text can be
 * appended directly to state->output_buf.
 */
void
psqlscan_emit(PsqlScanState state, const char *txt, int len)
{
	PQExpBuffer output_buf = state->output_buf;

	if (state->safe_encoding)
		appendBinaryPQExpBuffer(output_buf, txt, len);
	else
	{
		/* Gotta do it the hard way */
		const char *reference = state->refline;
		int			i;

		reference += (txt - state->curline);

		for (i = 0; i < len; i++)
		{
			char		ch = txt[i];

			if (ch == (char) 0xFF)
				ch = reference[i];
			appendPQExpBufferChar(output_buf, ch);
		}
	}
}

/*
 * psqlscan_extract_substring --- fetch value of (part of) the current token
 *
 * This is like psqlscan_emit(), except that the data is returned as a
 * malloc'd string rather than being pushed directly to state->output_buf.
 */
char *
psqlscan_extract_substring(PsqlScanState state, const char *txt, int len)
{
	char	   *result = (char *) pg_malloc(len + 1);

	if (state->safe_encoding)
		memcpy(result, txt, len);
	else
	{
		/* Gotta do it the hard way */
		const char *reference = state->refline;
		int			i;

		reference += (txt - state->curline);

		for (i = 0; i < len; i++)
		{
			char		ch = txt[i];

			if (ch == (char) 0xFF)
				ch = reference[i];
			result[i] = ch;
		}
	}
	result[len] = '\0';
	return result;
}

/*
 * psqlscan_escape_variable --- process :'VARIABLE' or :"VARIABLE"
 *
 * If the variable name is found, escape its value using the appropriate
 * quoting method and emit the value to output_buf.  (Since the result is
 * surely quoted, there is never any reason to rescan it.)	If we don't
 * find the variable or escaping fails, emit the token as-is.
 */
void
psqlscan_escape_variable(PsqlScanState state, const char *txt, int len,
						 PsqlScanQuoteType quote)
{
	char	   *varname;
	char	   *value;

	/* Variable lookup. */
	varname = psqlscan_extract_substring(state, txt + 2, len - 3);
	if (state->callbacks->get_variable)
		value = state->callbacks->get_variable(varname, quote,
											   state->cb_passthrough);
	else
		value = NULL;
	free(varname);

	if (value)
	{
		/* Emit the suitably-escaped value */
		appendPQExpBufferStr(state->output_buf, value);
		free(value);
	}
	else
	{
		/* Emit original token as-is */
		psqlscan_emit(state, txt, len);
	}
}

void
psqlscan_test_variable(PsqlScanState state, const char *txt, int len)
{
	char	   *varname;
	char	   *value;

	varname = psqlscan_extract_substring(state, txt + 3, len - 4);
	if (state->callbacks->get_variable)
		value = state->callbacks->get_variable(varname, PQUOTE_PLAIN,
											   state->cb_passthrough);
	else
		value = NULL;
	free(varname);

	if (value != NULL)
	{
		appendPQExpBufferStr(state->output_buf, "TRUE");
		free(value);
	}
	else
	{
		appendPQExpBufferStr(state->output_buf, "FALSE");
	}
}
