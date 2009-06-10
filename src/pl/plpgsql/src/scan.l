%{
/*-------------------------------------------------------------------------
 *
 * scan.l		- Scanner for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/pl/plpgsql/src/scan.l,v 1.70 2009/06/10 22:34:11 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"

#include "mb/pg_wchar.h"


/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  ereport(ERROR, (errmsg_internal("%s", msg)))

/*
 * When we parse a token that requires multiple lexer rules to process,
 * remember the token's starting position this way.
 */
#define SAVE_TOKEN_START()  \
	( start_lineno = plpgsql_scanner_lineno(), start_charpos = yytext )

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;

static const char *scanstr;		/* original input string */

static int	pushback_token;
static bool have_pushback_token;
static const char *cur_line_start;
static int	cur_line_num;
static int		xcdepth = 0;	/* depth of nesting in slash-star comments */
static char    *dolqstart;      /* current $foo$ quote start string */

extern PGDLLIMPORT bool standard_conforming_strings;

bool plpgsql_SpaceScanned = false;
%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option prefix="plpgsql_base_yy"

%option case-insensitive

/*
 * Exclusive states are a subset of the core lexer's:
 *  <xc> extended C-style comments
 *  <xq> standard quoted strings
 *  <xe> extended quoted strings (support backslash escape sequences)
 *  <xdolq> $foo$ quoted strings
 */

%x xc
%x xe
%x xq
%x xdolq

/*
 * Definitions --- these generally must match the core lexer, but in some
 * cases we can simplify, since we only care about identifying the token
 * boundaries and not about deriving the represented value.  Also, we
 * aren't trying to lex multicharacter operators so their interactions
 * with comments go away.
 */

space			[ \t\n\r\f]
horiz_space		[ \t\f]
newline			[\n\r]
non_newline		[^\n\r]

comment			("--"{non_newline}*)

whitespace		({space}+|{comment})
special_whitespace		({space}+|{comment}{newline})
horiz_whitespace		({horiz_space}|{comment})
whitespace_with_newline	({horiz_whitespace}*{newline}{special_whitespace}*)

quote			'
quotestop		{quote}{whitespace}*
quotecontinue	{quote}{whitespace_with_newline}{quote}
quotefail		{quote}{whitespace}*"-"

xestart			[eE]{quote}
xeinside		[^\\']+
xeescape		[\\].

xqstart			{quote}
xqdouble		{quote}{quote}
xqinside		[^']+

dolq_start		[A-Za-z\200-\377_]
dolq_cont		[A-Za-z\200-\377_0-9]
dolqdelim		\$({dolq_start}{dolq_cont}*)?\$
dolqfailed		\${dolq_start}{dolq_cont}*
dolqinside		[^$]+

xcstart			\/\*
xcstop			\*+\/
xcinside		[^*/]+

digit			[0-9]
ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

/* This is a simpler treatment of quoted identifiers than the core uses */
quoted_ident	(\"[^\"]*\")+

identifier		({ident_start}{ident_cont}*|{quoted_ident})

param			\${digit}+

%%
    /* ----------
     * Local variables in scanner to remember where
     * a string or comment started
     * ----------
     */
    int	start_lineno = 0;
	char *start_charpos = NULL;

    /* ----------
     * Reset the state when entering the scanner
     * ----------
     */
    BEGIN(INITIAL);
    plpgsql_SpaceScanned = false;

    /* ----------
     * The keyword rules
     * ----------
     */
:=				{ return K_ASSIGN;			}
=				{ return K_ASSIGN;			}
\.\.			{ return K_DOTDOT;			}
alias			{ return K_ALIAS;			}
begin			{ return K_BEGIN;			}
by				{ return K_BY;   			}
case			{ return K_CASE;			}
close			{ return K_CLOSE;			}
constant		{ return K_CONSTANT;		}
continue		{ return K_CONTINUE;		}
cursor			{ return K_CURSOR;			}
declare			{ return K_DECLARE;			}
default			{ return K_DEFAULT;			}
diagnostics		{ return K_DIAGNOSTICS;		}
else			{ return K_ELSE;			}
elseif			{ return K_ELSIF;			}
elsif			{ return K_ELSIF;			}
end				{ return K_END;				}
exception		{ return K_EXCEPTION;		}
execute			{ return K_EXECUTE;			}
exit			{ return K_EXIT;			}
fetch			{ return K_FETCH;			}
for				{ return K_FOR;				}
from			{ return K_FROM;			}
get				{ return K_GET;				}
if				{ return K_IF;				}
in				{ return K_IN;				}
insert			{ return K_INSERT;			}
into			{ return K_INTO;			}
is				{ return K_IS;				}
loop			{ return K_LOOP;			}
move			{ return K_MOVE;			}
no{space}+scroll { return K_NOSCROLL;		}
not				{ return K_NOT;				}
null			{ return K_NULL;			}
open			{ return K_OPEN;			}
or				{ return K_OR;				}
perform			{ return K_PERFORM;			}
raise			{ return K_RAISE;			}
rename			{ return K_RENAME;			}
result_oid		{ return K_RESULT_OID;		}
return			{ return K_RETURN;			}
reverse			{ return K_REVERSE;			}
row_count		{ return K_ROW_COUNT;		}
scroll			{ return K_SCROLL;			}
strict			{ return K_STRICT;	    	}
then			{ return K_THEN;			}
to				{ return K_TO;				}
type			{ return K_TYPE;			}
using			{ return K_USING;			}
when			{ return K_WHEN;			}
while			{ return K_WHILE;			}

^#option		{ return O_OPTION;			}
dump			{ return O_DUMP;			}


    /* ----------
     * Special word rules
	 *
	 * We set plpgsql_error_lineno in each rule so that errors reported
	 * in the pl_comp.c subroutines will point to the right place.
     * ----------
     */
{identifier}					{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_word(yytext); }
{identifier}{space}*\.{space}*{identifier}	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_dblword(yytext); }
{identifier}{space}*\.{space}*{identifier}{space}*\.{space}*{identifier}	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_tripword(yytext); }
{identifier}{space}*%TYPE		{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_wordtype(yytext); }
{identifier}{space}*\.{space}*{identifier}{space}*%TYPE	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_dblwordtype(yytext); }
{identifier}{space}*\.{space}*{identifier}{space}*\.{space}*{identifier}{space}*%TYPE	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_tripwordtype(yytext); }
{identifier}{space}*%ROWTYPE	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_wordrowtype(yytext); }
{identifier}{space}*\.{space}*{identifier}{space}*%ROWTYPE	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_dblwordrowtype(yytext); }
{param}							{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_word(yytext); }
{param}{space}*\.{space}*{identifier}	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_dblword(yytext); }
{param}{space}*\.{space}*{identifier}{space}*\.{space}*{identifier}	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_tripword(yytext); }
{param}{space}*%TYPE			{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_wordtype(yytext); }
{param}{space}*\.{space}*{identifier}{space}*%TYPE	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_dblwordtype(yytext); }
{param}{space}*\.{space}*{identifier}{space}*\.{space}*{identifier}{space}*%TYPE	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_tripwordtype(yytext); }
{param}{space}*%ROWTYPE		{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_wordrowtype(yytext); }
{param}{space}*\.{space}*{identifier}{space}*%ROWTYPE	{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	return plpgsql_parse_dblwordrowtype(yytext); }

{digit}+		{ return T_NUMBER;			}

\".				{ yyerror("unterminated quoted identifier"); }

    /* ----------
     * Ignore whitespace (including comments) but remember this happened
     * ----------
     */
{whitespace}	{ plpgsql_SpaceScanned = true; }

    /* ----------
     * Comment and literal handling is mostly copied from the core lexer
     * ----------
     */
{xcstart}		{
					/* Set location in case of syntax error in comment */
					SAVE_TOKEN_START();
					xcdepth = 0;
					BEGIN(xc);
					plpgsql_SpaceScanned = true;
				}

<xc>{xcstart}	{
					xcdepth++;
				}

<xc>{xcstop}	{
					if (xcdepth <= 0)
						BEGIN(INITIAL);
					else
						xcdepth--;
				}

<xc>{xcinside}	{
					/* ignore */
				}

<xc>\/+			{
					/* ignore */
				}

<xc>\*+			{
					/* ignore */
				}

<xc><<EOF>>		{ yyerror("unterminated /* comment"); }

{xqstart}		{
					SAVE_TOKEN_START();
					if (standard_conforming_strings)
						BEGIN(xq);
					else
						BEGIN(xe);
				}
{xestart}		{
					SAVE_TOKEN_START();
					BEGIN(xe);
				}
<xq,xe>{quotestop}	|
<xq,xe>{quotefail} {
					yyless(1);
					BEGIN(INITIAL);
					/* adjust yytext/yyleng to describe whole string token */
					yyleng += (yytext - start_charpos);
					yytext = start_charpos;
					return T_STRING;
				}
<xq,xe>{xqdouble} {
				}
<xq>{xqinside}  {
				}
<xe>{xeinside}  {
				}
<xe>{xeescape}  {
				}
<xq,xe>{quotecontinue} {
					/* ignore */
				}
<xe>.			{
					/* This is only needed for \ just before EOF */
				}
<xq,xe><<EOF>>		{ yyerror("unterminated quoted string"); }

{dolqdelim}		{
					SAVE_TOKEN_START();
					dolqstart = pstrdup(yytext);
					BEGIN(xdolq);
				}
{dolqfailed}	{
					/* throw back all but the initial "$" */
					yyless(1);
					/* and treat it as {other} */
					return yytext[0];
				}
<xdolq>{dolqdelim} {
					if (strcmp(yytext, dolqstart) == 0)
					{
						pfree(dolqstart);
						BEGIN(INITIAL);
						/* adjust yytext/yyleng to describe whole string */
						yyleng += (yytext - start_charpos);
						yytext = start_charpos;
						return T_STRING;
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
				}
<xdolq>{dolqinside} {
				}
<xdolq>{dolqfailed} {
				}
<xdolq>.		{
					/* This is only needed for $ inside the quoted text */
				}
<xdolq><<EOF>>	{ yyerror("unterminated dollar-quoted string"); }

    /* ----------
     * Any unmatched character is returned as is
     * ----------
     */
.				{
					return yytext[0];
				}

%%


/*
 * This is the yylex routine called from outside. It exists to provide
 * a one-token pushback facility.  Beware of trying to make it do more:
 * for the most part, plpgsql's gram.y assumes that yytext is in step
 * with the "current token".
 */
int
plpgsql_yylex(void)
{
	if (have_pushback_token)
	{
		have_pushback_token = false;
		return pushback_token;
	}
	return yylex();
}

/*
 * Push back a single token to be re-read by next plpgsql_yylex() call.
 *
 * NOTE: this does not cause yytext to "back up".
 */
void
plpgsql_push_back_token(int token)
{
	if (have_pushback_token)
		elog(ERROR, "cannot push back multiple tokens");
	pushback_token = token;
	have_pushback_token = true;
}

/*
 * Report a syntax error.
 */
void
plpgsql_yyerror(const char *message)
{
	const char *loc = yytext;
	int			cursorpos;

	plpgsql_error_lineno = plpgsql_scanner_lineno();

	/* in multibyte encodings, return index in characters not bytes */
	cursorpos = pg_mbstrlen_with_len(scanbuf, loc - scanbuf) + 1;

	if (*loc == YY_END_OF_BUFFER_CHAR)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 /* translator: %s is typically the translation of "syntax error" */
				 errmsg("%s at end of input", _(message)),
				 internalerrposition(cursorpos),
				 internalerrquery(scanstr)));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 /* translator: first %s is typically the translation of "syntax error" */
				 errmsg("%s at or near \"%s\"", _(message), loc),
				 internalerrposition(cursorpos),
				 internalerrquery(scanstr)));
	}
}

/*
 * Get the line number at which the current token ends.  This substitutes
 * for flex's very poorly implemented yylineno facility.
 *
 * We assume that flex has written a '\0' over the character following the
 * current token in scanbuf.  So, we just have to count the '\n' characters
 * before that.  We optimize this a little by keeping track of the last
 * '\n' seen so far.
 */
int
plpgsql_scanner_lineno(void)
{
	const char *c;

	while ((c = strchr(cur_line_start, '\n')) != NULL)
	{
		cur_line_start = c + 1;
		cur_line_num++;
	}
	return cur_line_num;
}

/*
 * Called before any actual parsing is done
 *
 * Note: the passed "str" must remain valid until plpgsql_scanner_finish().
 * Although it is not fed directly to flex, we need the original string
 * to cite in error messages.
 */
void
plpgsql_scanner_init(const char *str)
{
	Size	slen;

	slen = strlen(str);

	/*
	 * Might be left over after ereport()
	 */
	if (YY_CURRENT_BUFFER)
		yy_delete_buffer(YY_CURRENT_BUFFER);

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	scanbuf = palloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	/* Other setup */
	scanstr = str;

	have_pushback_token = false;

	cur_line_start = scanbuf;
	cur_line_num = 1;

	/*----------
	 * Hack: skip any initial newline, so that in the common coding layout
	 *		CREATE FUNCTION ... AS '
	 *			code body
	 *		' LANGUAGE plpgsql;
	 * we will think "line 1" is what the programmer thinks of as line 1.
	 *----------
	 */
    if (*cur_line_start == '\r')
        cur_line_start++;
    if (*cur_line_start == '\n')
        cur_line_start++;

	BEGIN(INITIAL);
}

/*
 * Called after parsing is done to clean up after plpgsql_scanner_init()
 */
void
plpgsql_scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	pfree(scanbuf);
}
