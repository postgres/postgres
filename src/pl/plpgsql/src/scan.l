%{
/*-------------------------------------------------------------------------
 *
 * scan.l		- Scanner for the PL/pgSQL procedural language
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/pl/plpgsql/src/scan.l,v 1.76 2009/11/10 02:13:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"

#include "mb/pg_wchar.h"


/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  ereport(ERROR, (errmsg_internal("%s", msg)))

/*
 * Each call to yylex must set yylloc to the location of the found token
 * (expressed as a byte offset from the start of the input text).
 * When we parse a token that requires multiple lexer rules to process,
 * this should be done in the first such rule, else yylloc will point
 * into the middle of the token.
 */
#define SET_YYLLOC()  (yylloc = yytext - scanbuf)

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;

static const char *scanorig;		/* original input string */

static int	pushback_token;
static bool have_pushback_token;
static const char *cur_line_start;
static const char *cur_line_end;
static int	cur_line_num;
static int		xcdepth = 0;	/* depth of nesting in slash-star comments */
static char    *dolqstart;      /* current $foo$ quote start string */

bool plpgsql_LookupIdentifiers = true;

static void location_lineno_init(void);

%}

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
     * Reset the state when entering yylex()
     * ----------
     */
    BEGIN(INITIAL);

    /* ----------
     * The keyword rules
     * ----------
     */
:=				{ SET_YYLLOC(); return K_ASSIGN;		}
=				{ SET_YYLLOC(); return K_ASSIGN;		}
\.\.			{ SET_YYLLOC(); return K_DOTDOT;		}
alias			{ SET_YYLLOC(); return K_ALIAS;			}
all				{ SET_YYLLOC(); return K_ALL;			}
begin			{ SET_YYLLOC(); return K_BEGIN;			}
by				{ SET_YYLLOC(); return K_BY;   			}
case			{ SET_YYLLOC(); return K_CASE;			}
close			{ SET_YYLLOC(); return K_CLOSE;			}
constant		{ SET_YYLLOC(); return K_CONSTANT;		}
continue		{ SET_YYLLOC(); return K_CONTINUE;		}
cursor			{ SET_YYLLOC(); return K_CURSOR;		}
declare			{ SET_YYLLOC(); return K_DECLARE;		}
default			{ SET_YYLLOC(); return K_DEFAULT;		}
diagnostics		{ SET_YYLLOC(); return K_DIAGNOSTICS;	}
else			{ SET_YYLLOC(); return K_ELSE;			}
elseif			{ SET_YYLLOC(); return K_ELSIF;			}
elsif			{ SET_YYLLOC(); return K_ELSIF;			}
end				{ SET_YYLLOC(); return K_END;			}
exception		{ SET_YYLLOC(); return K_EXCEPTION;		}
execute			{ SET_YYLLOC(); return K_EXECUTE;		}
exit			{ SET_YYLLOC(); return K_EXIT;			}
fetch			{ SET_YYLLOC(); return K_FETCH;			}
for				{ SET_YYLLOC(); return K_FOR;			}
from			{ SET_YYLLOC(); return K_FROM;			}
get				{ SET_YYLLOC(); return K_GET;			}
if				{ SET_YYLLOC(); return K_IF;			}
in				{ SET_YYLLOC(); return K_IN;			}
insert			{ SET_YYLLOC(); return K_INSERT;		}
into			{ SET_YYLLOC(); return K_INTO;			}
is				{ SET_YYLLOC(); return K_IS;			}
loop			{ SET_YYLLOC(); return K_LOOP;			}
move			{ SET_YYLLOC(); return K_MOVE;			}
no{space}+scroll { SET_YYLLOC(); return K_NOSCROLL;		}
not				{ SET_YYLLOC(); return K_NOT;			}
null			{ SET_YYLLOC(); return K_NULL;			}
open			{ SET_YYLLOC(); return K_OPEN;			}
or				{ SET_YYLLOC(); return K_OR;			}
perform			{ SET_YYLLOC(); return K_PERFORM;		}
raise			{ SET_YYLLOC(); return K_RAISE;			}
return			{ SET_YYLLOC(); return K_RETURN;		}
scroll			{ SET_YYLLOC(); return K_SCROLL;		}
strict			{ SET_YYLLOC(); return K_STRICT;	    }
then			{ SET_YYLLOC(); return K_THEN;			}
to				{ SET_YYLLOC(); return K_TO;			}
using			{ SET_YYLLOC(); return K_USING;			}
when			{ SET_YYLLOC(); return K_WHEN;			}
while			{ SET_YYLLOC(); return K_WHILE;			}

^#option		{ SET_YYLLOC(); return O_OPTION;		}
dump			{ SET_YYLLOC(); return O_DUMP;			}


    /* ----------
     * Special word rules
     * ----------
     */
{identifier}					{
	SET_YYLLOC();
	return plpgsql_parse_word(yytext); }
{identifier}{space}*\.{space}*{identifier}	{
	SET_YYLLOC();
	return plpgsql_parse_dblword(yytext); }
{identifier}{space}*\.{space}*{identifier}{space}*\.{space}*{identifier}	{
	SET_YYLLOC();
	return plpgsql_parse_tripword(yytext); }
{param}							{
	SET_YYLLOC();
	return plpgsql_parse_word(yytext); }
{param}{space}*\.{space}*{identifier}	{
	SET_YYLLOC();
	return plpgsql_parse_dblword(yytext); }
{param}{space}*\.{space}*{identifier}{space}*\.{space}*{identifier}	{
	SET_YYLLOC();
	return plpgsql_parse_tripword(yytext); }

{digit}+		{ SET_YYLLOC(); return T_NUMBER;		}

\".				{ SET_YYLLOC(); yyerror("unterminated quoted identifier"); }

    /* ----------
     * Comment and literal handling is mostly copied from the core lexer
     * ----------
     */
{whitespace}	{
					/* ignore */
				}

{xcstart}		{
					SET_YYLLOC();
					xcdepth = 0;
					BEGIN(xc);
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
					SET_YYLLOC();
					if (standard_conforming_strings)
						BEGIN(xq);
					else
						BEGIN(xe);
				}
{xestart}		{
					SET_YYLLOC();
					BEGIN(xe);
				}
<xq,xe>{quotestop}	|
<xq,xe>{quotefail} {
					yyless(1);
					BEGIN(INITIAL);
					/* adjust yytext/yyleng to describe whole string token */
					yyleng += (yytext - (scanbuf + yylloc));
					yytext = scanbuf + yylloc;
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
					SET_YYLLOC();
					dolqstart = pstrdup(yytext);
					BEGIN(xdolq);
				}
{dolqfailed}	{
					/* throw back all but the initial "$" */
					yyless(1);
					/* and treat it as {other} */
					SET_YYLLOC(); return yytext[0];
				}
<xdolq>{dolqdelim} {
					if (strcmp(yytext, dolqstart) == 0)
					{
						pfree(dolqstart);
						BEGIN(INITIAL);
						/* adjust yytext/yyleng to describe whole string */
						yyleng += (yytext - (scanbuf + yylloc));
						yytext = scanbuf + yylloc;
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
					SET_YYLLOC(); return yytext[0];
				}

%%


/*
 * This is the yylex routine called from outside. It exists to provide
 * a one-token pushback facility.  Beware of trying to push back more;
 * for the most part, plpgsql's gram.y assumes that yytext and yylloc
 * are in step with the "current token".  In particular it is assumed that
 * those are in step with the result immediately after any yylex() call.
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
 * NOTE: this does not cause yytext or yylloc to "back up".  Also, it
 * is not a good idea to push back a token other than what you read.
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
 * Append the function text starting at startlocation and extending to
 * (not including) endlocation onto the existing contents of "buf".
 */
void
plpgsql_append_source_text(StringInfo buf,
						   int startlocation, int endlocation)
{
	Assert(startlocation <= endlocation);
	appendBinaryStringInfo(buf, scanorig + startlocation,
						   endlocation - startlocation);
}

/*
 * plpgsql_scanner_errposition
 *		Report an error cursor position, if possible.
 *
 * This is expected to be used within an ereport() call.  The return value
 * is a dummy (always 0, in fact).
 *
 * Note that this can only be used for messages emitted during initial
 * parsing of a plpgsql function, since it requires the scanorig string
 * to still be available.
 */
int
plpgsql_scanner_errposition(int location)
{
	int		pos;

	if (location < 0 || scanorig == NULL)
		return 0;				/* no-op if location is unknown */

	/* Convert byte offset to character number */
	pos = pg_mbstrlen_with_len(scanorig, location) + 1;
	/* And pass it to the ereport mechanism */
	(void) internalerrposition(pos);
	/* Also pass the function body string */
	return internalerrquery(scanorig);
}

/*
 * plpgsql_yyerror
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
plpgsql_yyerror(const char *message)
{
	const char *loc = scanbuf + yylloc;

	if (*loc == YY_END_OF_BUFFER_CHAR)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 /* translator: %s is typically the translation of "syntax error" */
				 errmsg("%s at end of input", _(message)),
				 plpgsql_scanner_errposition(yylloc)));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 /* translator: first %s is typically the translation of "syntax error" */
				 errmsg("%s at or near \"%s\"", _(message), loc),
				 plpgsql_scanner_errposition(yylloc)));
	}
}

/*
 * Given a location (a byte offset in the function source text),
 * return a line number.
 *
 * We expect that this is typically called for a sequence of increasing
 * location values, so optimize accordingly by tracking the endpoints
 * of the "current" line.
 */
int
plpgsql_location_to_lineno(int location)
{
	const char *loc;

	if (location < 0 || scanorig == NULL)
		return 0;				/* garbage in, garbage out */
	loc = scanorig + location;

	/* be correct, but not fast, if input location goes backwards */
	if (loc < cur_line_start)
		location_lineno_init();

	while (cur_line_end != NULL && loc > cur_line_end)
	{
		cur_line_start = cur_line_end + 1;
		cur_line_num++;
		cur_line_end = strchr(cur_line_start, '\n');
	}

	return cur_line_num;
}

/* initialize or reset the state for plpgsql_location_to_lineno */
static void
location_lineno_init(void)
{
	cur_line_start = scanorig;
	cur_line_num = 1;

	/*----------
	 * Hack: skip any initial newline, so that in the common coding layout
	 *		CREATE FUNCTION ... AS $$
	 *			code body
	 *		$$ LANGUAGE plpgsql;
	 * we will think "line 1" is what the programmer thinks of as line 1.
	 *----------
	 */
    if (*cur_line_start == '\r')
        cur_line_start++;
    if (*cur_line_start == '\n')
        cur_line_start++;

	cur_line_end = strchr(cur_line_start, '\n');
}

/* return the most recently computed lineno */
int
plpgsql_latest_lineno(void)
{
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
	Size	slen = strlen(str);

	/*
	 * Reset flex internal state.  Whatever data it might think it has
	 * has long since been pfree'd.
	 */
	yy_init_globals();

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	scanbuf = (char *) palloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	/*
	 * scanorig points to the original string, which unlike scanbuf won't
	 * be modified on-the-fly by flex.  Notice that although yytext points
	 * into scanbuf, we rely on being able to apply locations (offsets from
	 * string start) to scanorig as well.
	 */
	scanorig = str;

	/* Other setup */
	have_pushback_token = false;

	location_lineno_init();

	BEGIN(INITIAL);
	plpgsql_LookupIdentifiers = true;
}

/*
 * Called after parsing is done to clean up after plpgsql_scanner_init()
 */
void
plpgsql_scanner_finish(void)
{
	/* release storage */
	yy_delete_buffer(scanbufhandle);
	pfree(scanbuf);
	/* avoid leaving any dangling pointers */
	scanbufhandle = NULL;
	scanbuf = NULL;
	scanorig = NULL;
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 */

void *
plpgsql_base_yyalloc(yy_size_t bytes)
{
	return palloc(bytes);
}

void *
plpgsql_base_yyrealloc(void *ptr, yy_size_t bytes)
{
	if (ptr)
		return repalloc(ptr, bytes);
	else
		return palloc(bytes);
}

void
plpgsql_base_yyfree(void *ptr)
{
	if (ptr)
		pfree(ptr);
}
