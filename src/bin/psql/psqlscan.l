%{
/*-------------------------------------------------------------------------
 *
 * psqlscan.l
 *	  lexical scanner for psql
 *
 * This code is mainly needed to determine where the end of a SQL statement
 * is: we are looking for semicolons that are not within quotes, comments,
 * or parentheses.  The most reliable way to handle this is to borrow the
 * backend's flex lexer rules, lock, stock, and barrel.  The rules below
 * are (except for a few) the same as the backend's, but their actions are
 * just ECHO whereas the backend's actions generally do other things.
 *
 * XXX The rules in this file must be kept in sync with the backend lexer!!!
 *
 * XXX Avoid creating backtracking cases --- see the backend lexer for info.
 *
 * The most difficult aspect of this code is that we need to work in multibyte
 * encodings that are not ASCII-safe.  A "safe" encoding is one in which each
 * byte of a multibyte character has the high bit set (it's >= 0x80).  Since
 * all our lexing rules treat all high-bit-set characters alike, we don't
 * really need to care whether such a byte is part of a sequence or not.
 * In an "unsafe" encoding, we still expect the first byte of a multibyte
 * sequence to be >= 0x80, but later bytes might not be.  If we scan such
 * a sequence as-is, the lexing rules could easily be fooled into matching
 * such bytes to ordinary ASCII characters.  Our solution for this is to
 * substitute 0xFF for each non-first byte within the data presented to flex.
 * The flex rules will then pass the FF's through unmolested.  The emit()
 * subroutine is responsible for looking back to the original string and
 * replacing FF's with the corresponding original bytes.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/bin/psql/psqlscan.l,v 1.23 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "psqlscan.h"

#include <ctype.h>

#include "common.h"
#include "settings.h"
#include "variables.h"


/*
 * We use a stack of flex buffers to handle substitution of psql variables.
 * Each stacked buffer contains the as-yet-unread text from one psql variable.
 * When we pop the stack all the way, we resume reading from the outer buffer
 * identified by scanbufhandle.
 */
typedef struct StackElem
{
	YY_BUFFER_STATE buf;		/* flex input control structure */
	char	   *bufstring;		/* data actually being scanned by flex */
	char	   *origstring;		/* copy of original data, if needed */
	struct StackElem *next;
} StackElem;

/*
 * All working state of the lexer must be stored in PsqlScanStateData
 * between calls.  This allows us to have multiple open lexer operations,
 * which is needed for nested include files.  The lexer itself is not
 * recursive, but it must be re-entrant.
 */
typedef struct PsqlScanStateData
{
	StackElem  *buffer_stack;	/* stack of variable expansion buffers */
	/*
	 * These variables always refer to the outer buffer, never to any
	 * stacked variable-expansion buffer.
	 */
	YY_BUFFER_STATE scanbufhandle;
	char	   *scanbuf;		/* start of outer-level input buffer */
	const char *scanline;		/* current input line at outer level */

	/* safe_encoding, curline, refline are used by emit() to replace FFs */
	int			encoding;		/* encoding being used now */
	bool		safe_encoding;	/* is current encoding "safe"? */
	const char *curline;		/* actual flex input string for cur buf */
	const char *refline;		/* original data for cur buffer */

	/*
	 * All this state lives across successive input lines, until explicitly
	 * reset by psql_scan_reset.
	 */
	int			start_state;	/* saved YY_START */
	int			paren_depth;	/* depth of nesting in parentheses */
	int			xcdepth;		/* depth of nesting in slash-star comments */
	char	   *dolqstart;		/* current $foo$ quote start string */
} PsqlScanStateData;

static PsqlScanState cur_state;	/* current state while active */

static PQExpBuffer output_buf;	/* current output buffer */

/* these variables do not need to be saved across calls */
static enum slash_option_type option_type;
static char *option_quote;


/* Return values from yylex() */
#define LEXRES_EOL			0	/* end of input */
#define LEXRES_SEMI			1	/* command-terminating semicolon found */
#define LEXRES_BACKSLASH	2	/* backslash command start */
#define LEXRES_OK			3	/* OK completion of backslash argument */


int	yylex(void);

static void push_new_buffer(const char *newstr);
static YY_BUFFER_STATE prepare_buffer(const char *txt, int len,
									  char **txtcopy);
static void emit(const char *txt, int len);

#define ECHO emit(yytext, yyleng)

%}

%option 8bit
%option never-interactive
%option nodefault
%option nounput
%option noyywrap

/*
 * All of the following definitions and rules should exactly match
 * src/backend/parser/scan.l so far as the flex patterns are concerned.
 * The rule bodies are just ECHO as opposed to what the backend does,
 * however.  (But be sure to duplicate code that affects the lexing process,
 * such as BEGIN().)  Also, psqlscan uses a single <<EOF>> rule whereas
 * scan.l has a separate one for each exclusive state.
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
 *  <xh> hexadecimal numeric string
 *  <xq> standard quoted strings
 *  <xe> extended quoted strings (support backslash escape sequences)
 *  <xdolq> $foo$ quoted strings
 */

%x xb
%x xc
%x xd
%x xh
%x xe
%x xq
%x xdolq
/* Additional exclusive states for psql only: lex backslash commands */
%x xslashcmd
%x xslasharg
%x xslashquote
%x xslashbackquote
%x xslashdefaultarg
%x xslashquotedarg
%x xslashwholeline
%x xslashend

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
 * XXX perhaps \f (formfeed) should be treated as a newline as well?
 */

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

special_whitespace		({space}+|{comment}{newline})
horiz_whitespace		({horiz_space}|{comment})
whitespace_with_newline	({horiz_whitespace}*{newline}{special_whitespace}*)

/*
 * To ensure that {quotecontinue} can be scanned without having to back up
 * if the full pattern isn't matched, we include trailing whitespace in
 * {quotestop}.  This matches all cases where {quotecontinue} fails to match,
 * except for {quote} followed by whitespace and just one "-" (not two,
 * which would start a {comment}).  To cover that we have {quotefail}.
 * The actions for {quotestop} and {quotefail} must throw back characters
 * beyond the quote proper.
 */
quote			'
quotestop		{quote}{whitespace}*
quotecontinue	{quote}{whitespace_with_newline}{quote}
quotefail		{quote}{whitespace}*"-"

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

digit			[0-9]
ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

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
					/*
					 * Note that the whitespace rule includes both true
					 * whitespace and single-line ("--" style) comments.
					 * We suppress whitespace at the start of the query
					 * buffer.  We also suppress all single-line comments,
					 * which is pretty dubious but is the historical
					 * behavior.
					 */
					if (!(output_buf->len == 0 || yytext[0] == '-'))
						ECHO;
				}

{xcstart}		{
					cur_state->xcdepth = 0;
					BEGIN(xc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
					ECHO;
				}

<xc>{xcstart}	{
					cur_state->xcdepth++;
					/* Put back any characters past slash-star; see above */
					yyless(2);
					ECHO;
				}

<xc>{xcstop}	{
					if (cur_state->xcdepth <= 0)
					{
						BEGIN(INITIAL);
					}
					else
						cur_state->xcdepth--;
					ECHO;
				}

<xc>{xcinside}	{
					ECHO;
				}

<xc>{op_chars}	{
					ECHO;
				}

<xc>\*+			{
					ECHO;
				}

{xbstart}		{
					BEGIN(xb);
					ECHO;
				}
<xb>{quotestop}	|
<xb>{quotefail} {
					yyless(1);
					BEGIN(INITIAL);
					ECHO;
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					ECHO;
				}
<xh>{quotecontinue}	|
<xb>{quotecontinue}	{
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
<xh>{quotestop}	|
<xh>{quotefail} {
					yyless(1);
					BEGIN(INITIAL);
					ECHO;
				}

{xnstart}		{
					yyless(1);				/* eat only 'n' this time */
					ECHO;
				}

{xqstart}		{
					if (standard_strings())
						BEGIN(xq);
					else
						BEGIN(xe);
					ECHO;
				}
{xestart}		{
					BEGIN(xe);
					ECHO;
				}
<xq,xe>{quotestop}	|
<xq,xe>{quotefail} {
					yyless(1);
					BEGIN(INITIAL);
					ECHO;
				}
<xq,xe>{xqdouble} {
					ECHO;
				}
<xq>{xqinside}  {
					ECHO;
				}
<xe>{xeinside}  {
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
<xq,xe>{quotecontinue} {
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
						yyless(yyleng-1);
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
<xd>{xdstop}	{
					BEGIN(INITIAL);
					ECHO;
				}
<xd>{xddouble}	{
					ECHO;
				}
<xd>{xdinside}	{
					ECHO;
				}

{typecast}		{
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
					if (cur_state->paren_depth == 0)
					{
						/* Terminate lexing temporarily */
						return LEXRES_SEMI;
					}
				}

	/*
	 * psql-specific rules to handle backslash commands and variable
	 * substitution.  We want these before {self}, also.
	 */

"\\"[;:]		{
					/* Force a semicolon or colon into the query buffer */
					emit(yytext + 1, 1);
				}

"\\"			{
					/* Terminate lexing temporarily */
					return LEXRES_BACKSLASH;
				}

:[A-Za-z0-9_]+	{
					/* Possible psql variable substitution */
					const char *value;

					value = GetVariable(pset.vars, yytext + 1);

					if (value)
					{
						/* It is a variable, perform substitution */
						push_new_buffer(value);
						/* yy_scan_string already made buffer active */
					}
					else
					{
						/*
						 * if the variable doesn't exist we'll copy the
						 * string as is
						 */
						ECHO;
					}
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
					}
					ECHO;
				}

{param}			{
					ECHO;
				}

{integer}		{
					ECHO;
				}
{decimal}		{
					ECHO;
				}
{real}			{
					ECHO;
				}
{realfail1}		{
					/*
					 * throw back the [Ee], and treat as {decimal}.  Note
					 * that it is possible the input is actually {integer},
					 * but since this case will almost certainly lead to a
					 * syntax error anyway, we don't bother to distinguish.
					 */
					yyless(yyleng-1);
					ECHO;
				}
{realfail2}		{
					/* throw back the [Ee][+-], and proceed as above */
					yyless(yyleng-2);
					ECHO;
				}


{identifier}	{
					ECHO;
				}

{other}			{
					ECHO;
				}


	/*
	 * Everything from here down is psql-specific.
	 */

<<EOF>>			{
					StackElem  *stackelem = cur_state->buffer_stack;

					if (stackelem == NULL)
						return LEXRES_EOL; /* end of input reached */

					/*
					 * We were expanding a variable, so pop the inclusion
					 * stack and keep lexing
					 */
					cur_state->buffer_stack = stackelem->next;
					yy_delete_buffer(stackelem->buf);
					free(stackelem->bufstring);
					if (stackelem->origstring)
						free(stackelem->origstring);
					free(stackelem);

					stackelem = cur_state->buffer_stack;
					if (stackelem != NULL)
					{
						yy_switch_to_buffer(stackelem->buf);
						cur_state->curline = stackelem->bufstring;
						cur_state->refline = stackelem->origstring ? stackelem->origstring : stackelem->bufstring;
					}
					else
					{
						yy_switch_to_buffer(cur_state->scanbufhandle);
						cur_state->curline = cur_state->scanbuf;
						cur_state->refline = cur_state->scanline;
					}
				}

	/*
	 * Exclusive lexer states to handle backslash command lexing
	 */

<xslashcmd>{
	/* command name ends at whitespace or backslash; eat all else */

{space}|"\\"	{
					yyless(0);
					return LEXRES_OK;
				}

{other}			{ ECHO; }

}

<xslasharg>{
	/* eat any whitespace, then decide what to do at first nonblank */

{space}+		{ }

"\\"			{
					/*
					 * backslash is end of command or next command, do not eat
					 *
					 * XXX this means we can't conveniently accept options
					 * that start with a backslash; therefore, option
					 * processing that encourages use of backslashes is rather
					 * broken.
					 */
					yyless(0);
					return LEXRES_OK;
				}

{quote}			{
					*option_quote = '\'';
					BEGIN(xslashquote);
				}

"`"				{
					if (option_type == OT_VERBATIM)
					{
						/* in verbatim mode, backquote is not special */
						ECHO;
						BEGIN(xslashdefaultarg);
					}
					else
					{
						*option_quote = '`';
						BEGIN(xslashbackquote);
					}
				}

:[A-Za-z0-9_]*	{
					/* Possible psql variable substitution */
					if (option_type == OT_VERBATIM)
						ECHO;
					else
					{
						const char *value;

						value = GetVariable(pset.vars, yytext + 1);

						/*
						 * The variable value is just emitted without any
						 * further examination.  This is consistent with the
						 * pre-8.0 code behavior, if not with the way that
						 * variables are handled outside backslash commands.
						 */
						if (value)
							appendPQExpBufferStr(output_buf, value);
					}

					*option_quote = ':';

					return LEXRES_OK;
				}

"|"				{
					ECHO;
					if (option_type == OT_FILEPIPE)
					{
						/* treat like whole-string case */
						BEGIN(xslashwholeline);
					}
					else
					{
						/* treat like default case */
						BEGIN(xslashdefaultarg);
					}
				}

{dquote}		{
					*option_quote = '"';
					ECHO;
					BEGIN(xslashquotedarg);
				}

{other}			{
					ECHO;
					BEGIN(xslashdefaultarg);
				}

}

<xslashquote>{
	/*
	 * single-quoted text: copy literally except for '' and backslash
	 * sequences
	 */

{quote}			{ return LEXRES_OK; }

{xqdouble}		{ appendPQExpBufferChar(output_buf, '\''); }

"\\n"			{ appendPQExpBufferChar(output_buf, '\n'); }
"\\t"			{ appendPQExpBufferChar(output_buf, '\t'); }
"\\b"			{ appendPQExpBufferChar(output_buf, '\b'); }
"\\r"			{ appendPQExpBufferChar(output_buf, '\r'); }
"\\f"			{ appendPQExpBufferChar(output_buf, '\f'); }

{xeoctesc}		{
					/* octal case */
					appendPQExpBufferChar(output_buf,
										  (char) strtol(yytext + 1, NULL, 8));
				}

{xehexesc}		{
					/* hex case */
					appendPQExpBufferChar(output_buf,
										  (char) strtol(yytext + 2, NULL, 16));
				}

"\\".			{ emit(yytext + 1, 1); }

{other}|\n		{ ECHO; }

}

<xslashbackquote>{
	/*
	 * backticked text: copy everything until next backquote or end of line.
	 * Invocation of the command will happen in psql_scan_slash_option.
	 */

"`"				{ return LEXRES_OK; }

{other}|\n		{ ECHO; }

}

<xslashdefaultarg>{
	/*
	 * Copy everything until unquoted whitespace or end of line.  Quotes
	 * do not get stripped yet.
	 */

{space}			{
					yyless(0);
					return LEXRES_OK;
				}

"\\"			{
					/*
					 * unquoted backslash is end of command or next command,
					 * do not eat
					 *
					 * (this was not the behavior pre-8.0, but it seems
					 * consistent)
					 */
					yyless(0);
					return LEXRES_OK;
				}

{dquote}		{
					*option_quote = '"';
					ECHO;
					BEGIN(xslashquotedarg);
				}

{other}			{ ECHO; }

}

<xslashquotedarg>{
	/* double-quoted text within a default-type argument: copy */

{dquote}		{
					ECHO;
					BEGIN(xslashdefaultarg);
				}

{other}|\n		{ ECHO; }

}

<xslashwholeline>{
	/* copy everything until end of input line */
	/* but suppress leading whitespace */

{space}+		{
					if (output_buf->len > 0)
						ECHO;
				}

{other}			{ ECHO; }

}

<xslashend>{
	/* at end of command, eat a double backslash, but not anything else */

"\\\\"			{ return LEXRES_OK; }

{other}|\n		{
					yyless(0);
					return LEXRES_OK;
				}

}

%%

/*
 * Create a lexer working state struct.
 */
PsqlScanState
psql_scan_create(void)
{
	PsqlScanState state;

	state = (PsqlScanStateData *) pg_malloc_zero(sizeof(PsqlScanStateData));

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

	free(state);
}

/*
 * Set up to perform lexing of the given input line.
 *
 * The text at *line, extending for line_len bytes, will be scanned by
 * subsequent calls to the psql_scan routines.  psql_scan_finish should
 * be called when scanning is complete.  Note that the lexer retains
 * a pointer to the storage at *line --- this string must not be altered
 * or freed until after psql_scan_finish is called.
 */
void
psql_scan_setup(PsqlScanState state,
				const char *line, int line_len)
{
	/* Mustn't be scanning already */
	psql_assert(state->scanbufhandle == NULL);
	psql_assert(state->buffer_stack == NULL);

	/* Do we need to hack the character set encoding? */
	state->encoding = pset.encoding;
	state->safe_encoding = pg_valid_server_encoding_id(state->encoding);

	/* needed for prepare_buffer */
	cur_state = state;

	/* Set up flex input buffer with appropriate translation and padding */
	state->scanbufhandle = prepare_buffer(line, line_len,
										  &state->scanbuf);
	state->scanline = line;

	/* Set lookaside data in case we have to map unsafe encoding */
	state->curline = state->scanbuf;
	state->refline = state->scanline;
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
 * PSCAN_BACKSLASH: found a backslash that starts a psql special command.
 * Any previous data on the line has been transferred to query_buf.
 * The caller will typically next call psql_scan_slash_command(),
 * perhaps psql_scan_slash_option(), and psql_scan_slash_command_end().
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
	psql_assert(state->scanbufhandle);

	/* Set up static variables that will be used by yylex */
	cur_state = state;
	output_buf = query_buf;

	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf);
	else
		yy_switch_to_buffer(state->scanbufhandle);

	BEGIN(state->start_state);

	/* And lex. */
	lexresult = yylex();

	/* Update static vars back to the state struct */
	state->start_state = YY_START;

	/*
	 * Check termination state and return appropriate result info.
	 */
	switch (lexresult)
	{
		case LEXRES_EOL:		/* end of input */
			switch (state->start_state)
			{
				case INITIAL:
					if (state->paren_depth > 0)
					{
						result = PSCAN_INCOMPLETE;
						*prompt = PROMPT_PAREN;
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
				case xq:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_SINGLEQUOTE;
					break;
				case xe:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_SINGLEQUOTE;
					break;
				case xdolq:
					result = PSCAN_INCOMPLETE;
					*prompt = PROMPT_DOLLARQUOTE;
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
	{
		StackElem  *stackelem = state->buffer_stack;

		state->buffer_stack = stackelem->next;
		yy_delete_buffer(stackelem->buf);
		free(stackelem->bufstring);
		if (stackelem->origstring)
			free(stackelem->origstring);
		free(stackelem);
	}

	/* Done with the outer scan buffer, too */
	if (state->scanbufhandle)
		yy_delete_buffer(state->scanbufhandle);
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
	return state->start_state != INITIAL;
}

/*
 * Scan the command name of a psql backslash command.  This should be called
 * after psql_scan() returns PSCAN_BACKSLASH.  It is assumed that the input
 * has been consumed through the leading backslash.
 *
 * The return value is a malloc'd copy of the command name, as parsed off
 * from the input.
 */
char *
psql_scan_slash_command(PsqlScanState state)
{
	PQExpBufferData mybuf;
	int			lexresult;

	/* Must be scanning already */
	psql_assert(state->scanbufhandle);

	/* Build a local buffer that we'll return the data of */
	initPQExpBuffer(&mybuf);

	/* Set up static variables that will be used by yylex */
	cur_state = state;
	output_buf = &mybuf;

	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf);
	else
		yy_switch_to_buffer(state->scanbufhandle);

	BEGIN(xslashcmd);

	/* And lex. */
	lexresult = yylex();

	/* There are no possible errors in this lex state... */

	return mybuf.data;
}

/*
 * Parse off the next argument for a backslash command, and return it as a
 * malloc'd string.  If there are no more arguments, returns NULL.
 *
 * type tells what processing, if any, to perform on the option string;
 * for example, if it's a SQL identifier, we want to downcase any unquoted
 * letters.
 *
 * if quote is not NULL, *quote is set to 0 if no quoting was found, else
 * the quote symbol.
 *
 * if semicolon is true, unquoted trailing semicolon(s) that would otherwise
 * be taken as part of the option string will be stripped.
 *
 * NOTE: the only possible syntax errors for backslash options are unmatched
 * quotes, which are detected when we run out of input.  Therefore, on a
 * syntax error we just throw away the string and return NULL; there is no
 * need to worry about flushing remaining input.
 */
char *
psql_scan_slash_option(PsqlScanState state,
					   enum slash_option_type type,
					   char *quote,
					   bool semicolon)
{
	PQExpBufferData mybuf;
	int			lexresult;
	char		local_quote;
	bool		badarg;

	/* Must be scanning already */
	psql_assert(state->scanbufhandle);

	if (quote == NULL)
		quote = &local_quote;
	*quote = 0;

	/* Build a local buffer that we'll return the data of */
	initPQExpBuffer(&mybuf);

	/* Set up static variables that will be used by yylex */
	cur_state = state;
	output_buf = &mybuf;
	option_type = type;
	option_quote = quote;

	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf);
	else
		yy_switch_to_buffer(state->scanbufhandle);

	if (type == OT_WHOLE_LINE)
		BEGIN(xslashwholeline);
	else
		BEGIN(xslasharg);

	/* And lex. */
	lexresult = yylex();

	/*
	 * Check the lex result: we should have gotten back either LEXRES_OK
	 * or LEXRES_EOL (the latter indicating end of string).  If we were inside
	 * a quoted string, as indicated by YY_START, EOL is an error.
	 */
	psql_assert(lexresult == LEXRES_EOL || lexresult == LEXRES_OK);
	badarg = false;
	switch (YY_START)
	{
		case xslasharg:
			/* empty arg, or possibly a psql variable substitution */
			break;
		case xslashquote:
			if (lexresult != LEXRES_OK)
				badarg = true;		/* hit EOL not ending quote */
			break;
		case xslashbackquote:
			if (lexresult != LEXRES_OK)
				badarg = true;		/* hit EOL not ending quote */
			else
			{
				/* Perform evaluation of backticked command */
				char	   *cmd = mybuf.data;
				FILE	   *fd;
				bool		error = false;
				PQExpBufferData output;
				char		buf[512];
				size_t		result;

				fd = popen(cmd, PG_BINARY_R);
				if (!fd)
				{
					psql_error("%s: %s\n", cmd, strerror(errno));
					error = true;
				}

				initPQExpBuffer(&output);

				if (!error)
				{
					do
					{
						result = fread(buf, 1, sizeof(buf), fd);
						if (ferror(fd))
						{
							psql_error("%s: %s\n", cmd, strerror(errno));
							error = true;
							break;
						}
						appendBinaryPQExpBuffer(&output, buf, result);
					} while (!feof(fd));
				}

				if (fd && pclose(fd) == -1)
				{
					psql_error("%s: %s\n", cmd, strerror(errno));
					error = true;
				}

				/* Now done with cmd, transfer result to mybuf */
				resetPQExpBuffer(&mybuf);

				if (!error)
				{
					/* strip any trailing newline */
					if (output.len > 0 &&
						output.data[output.len - 1] == '\n')
						output.len--;
					appendBinaryPQExpBuffer(&mybuf, output.data, output.len);
				}

				termPQExpBuffer(&output);
			}
			break;
		case xslashdefaultarg:
			/* Strip any trailing semi-colons if requested */
			if (semicolon)
			{
				while (mybuf.len > 0 &&
					   mybuf.data[mybuf.len - 1] == ';')
				{
					mybuf.data[--mybuf.len] = '\0';
				}
			}

			/*
			 * If SQL identifier processing was requested, then we strip out
			 * excess double quotes and downcase unquoted letters.
			 * Doubled double-quotes become output double-quotes, per spec.
			 *
			 * Note that a string like FOO"BAR"BAZ will be converted to
			 * fooBARbaz; this is somewhat inconsistent with the SQL spec,
			 * which would have us parse it as several identifiers.  But
			 * for psql's purposes, we want a string like "foo"."bar" to
			 * be treated as one option, so there's little choice.
			 */
			if (type == OT_SQLID || type == OT_SQLIDHACK)
			{
				bool		inquotes = false;
				char	   *cp = mybuf.data;

				while (*cp)
				{
					if (*cp == '"')
					{
						if (inquotes && cp[1] == '"')
						{
							/* Keep the first quote, remove the second */
							cp++;
						}
						inquotes = !inquotes;
						/* Collapse out quote at *cp */
						memmove(cp, cp + 1, strlen(cp));
						mybuf.len--;
						/* do not advance cp */
					}
					else
					{
						if (!inquotes && type == OT_SQLID)
							*cp = pg_tolower((unsigned char) *cp);
						cp += PQmblen(cp, pset.encoding);
					}
				}
			}
			break;
		case xslashquotedarg:
			/* must have hit EOL inside double quotes */
			badarg = true;
			break;
		case xslashwholeline:
			/* always okay */
			break;
		default:
			/* can't get here */
			fprintf(stderr, "invalid YY_START\n");
			exit(1);
	}

	if (badarg)
	{
		psql_error("unterminated quoted string\n");
		termPQExpBuffer(&mybuf);
		return NULL;
	}

	/*
	 * An unquoted empty argument isn't possible unless we are at end of
	 * command.  Return NULL instead.
	 */
	if (mybuf.len == 0 && *quote == 0)
	{
		termPQExpBuffer(&mybuf);
		return NULL;
	}

	/* Else return the completed string. */
	return mybuf.data;
}

/*
 * Eat up any unused \\ to complete a backslash command.
 */
void
psql_scan_slash_command_end(PsqlScanState state)
{
	int			lexresult;

	/* Must be scanning already */
	psql_assert(state->scanbufhandle);

	/* Set up static variables that will be used by yylex */
	cur_state = state;
	output_buf = NULL;

	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf);
	else
		yy_switch_to_buffer(state->scanbufhandle);

	BEGIN(xslashend);

	/* And lex. */
	lexresult = yylex();

	/* There are no possible errors in this lex state... */
}

/*
 * "Push back" the passed string so that it will be rescanned by subsequent
 * psql_scan_slash_option calls.  This is presently only used in the case
 * where a single-letter command has been concatenated with its argument.
 *
 * We use the same buffer stack mechanism as for variable expansion.
 */
void
psql_scan_slash_pushback(PsqlScanState state, const char *str)
{
	/* needed for push_new_buffer */
	cur_state = state;

	push_new_buffer(str);
}


/*
 * Push the given string onto the stack of stuff to scan.
 *
 * cur_state must point to the active PsqlScanState.
 *
 * NOTE SIDE EFFECT: the new buffer is made the active flex input buffer.
 */
static void
push_new_buffer(const char *newstr)
{
	StackElem  *stackelem;

	stackelem = (StackElem *) pg_malloc(sizeof(StackElem));
	stackelem->buf = prepare_buffer(newstr, strlen(newstr),
									&stackelem->bufstring);
	cur_state->curline = stackelem->bufstring;
	if (cur_state->safe_encoding)
	{
		stackelem->origstring = NULL;
		cur_state->refline = stackelem->bufstring;
	}
	else
	{
		stackelem->origstring = pg_strdup(newstr);
		cur_state->refline = stackelem->origstring;
	}
	stackelem->next = cur_state->buffer_stack;
	cur_state->buffer_stack = stackelem;
}

/*
 * Set up a flex input buffer to scan the given data.  We always make a
 * copy of the data.  If working in an unsafe encoding, the copy has
 * multibyte sequences replaced by FFs to avoid fooling the lexer rules.
 *
 * cur_state must point to the active PsqlScanState.
 *
 * NOTE SIDE EFFECT: the new buffer is made the active flex input buffer.
 */
static YY_BUFFER_STATE
prepare_buffer(const char *txt, int len, char **txtcopy)
{
	char	   *newtxt;

	/* Flex wants two \0 characters after the actual data */
	newtxt = pg_malloc(len + 2);
	*txtcopy = newtxt;
	newtxt[len] = newtxt[len + 1] = YY_END_OF_BUFFER_CHAR;

	if (cur_state->safe_encoding)
		memcpy(newtxt, txt, len);
	else
	{
		/* Gotta do it the hard way */
		int		i = 0;

		while (i < len)
		{
			int		thislen = PQmblen(txt + i, cur_state->encoding);

			/* first byte should always be okay... */
			newtxt[i] = txt[i];
			i++;
			while (--thislen > 0)
				newtxt[i++] = (char) 0xFF;
		}
	}

	return yy_scan_buffer(newtxt, len + 2);
}

/*
 * emit() --- body for ECHO macro
 *
 * NB: this must be used for ALL and ONLY the text copied from the flex
 * input data.  If you pass it something that is not part of the yytext
 * string, you are making a mistake.  Internally generated text can be
 * appended directly to output_buf.
 */
static void
emit(const char *txt, int len)
{
	if (cur_state->safe_encoding)
		appendBinaryPQExpBuffer(output_buf, txt, len);
	else
	{
		/* Gotta do it the hard way */
		const char *reference = cur_state->refline;
		int		i;

		reference += (txt - cur_state->curline);

		for (i = 0; i < len; i++)
		{
			char	ch = txt[i];

			if (ch == (char) 0xFF)
				ch = reference[i];
			appendPQExpBufferChar(output_buf, ch);
		}
	}
}
