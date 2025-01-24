%top{
/*-------------------------------------------------------------------------
 *
 * exprscan.l
 *	  lexical scanner for pgbench backslash commands
 *
 * This lexer supports two operating modes:
 *
 * In INITIAL state, just parse off whitespace-separated words (this mode
 * is basically equivalent to strtok(), which is what we used to use).
 *
 * In EXPR state, lex for the simple expression syntax of exprparse.y.
 *
 * In either mode, stop upon hitting newline or end of string.
 *
 * Note that this lexer operates within the framework created by psqlscan.l,
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/exprscan.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

/*
 * NB: include exprparse.h only AFTER including pgbench.h, because pgbench.h
 * contains definitions needed for YYSTYPE. Likewise, pgbench.h must come after
 * psqlscan_int.h for yyscan_t.
 */
#include "fe_utils/psqlscan_int.h"
#include "pgbench.h"
#include "exprparse.h"
}

%{
/* context information for reporting errors in expressions */
static const char *expr_source = NULL;
static int	expr_lineno = 0;
static int	expr_start_offset = 0;
static const char *expr_command = NULL;

/* indicates whether last yylex() call read a newline */
static bool last_was_newline = false;

/* LCOV_EXCL_START */

%}

/* Except for the prefix, these options should match psqlscan.l */
%option reentrant
%option bison-bridge
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="expr_yy"

/* Character classes */
alpha			[a-zA-Z\200-\377_]
digit			[0-9]
alnum			[A-Za-z\200-\377_0-9]
/* {space} + {nonspace} + {newline} should cover all characters */
space			[ \t\r\f\v]
nonspace		[^ \t\r\f\v\n]
newline			[\n]

/* Line continuation marker */
continuation	\\\r?{newline}

/* case insensitive keywords */
and				[Aa][Nn][Dd]
or				[Oo][Rr]
not				[Nn][Oo][Tt]
case			[Cc][Aa][Ss][Ee]
when			[Ww][Hh][Ee][Nn]
then			[Tt][Hh][Ee][Nn]
else			[Ee][Ll][Ss][Ee]
end				[Ee][Nn][Dd]
true			[Tt][Rr][Uu][Ee]
false			[Ff][Aa][Ll][Ss][Ee]
null			[Nn][Uu][Ll][Ll]
is				[Ii][Ss]
isnull			[Ii][Ss][Nn][Uu][Ll][Ll]
notnull			[Nn][Oo][Tt][Nn][Uu][Ll][Ll]

/* Exclusive states */
%x EXPR

%%

%{
		/* Declare some local variables inside yylex(), for convenience */
		PsqlScanState cur_state = yyextra;

		/*
		 * Force flex into the state indicated by start_state.  This has a
		 * couple of purposes: it lets some of the functions below set a new
		 * starting state without ugly direct access to flex variables, and it
		 * allows us to transition from one flex lexer to another so that we
		 * can lex different parts of the source string using separate lexers.
		 */
		BEGIN(cur_state->start_state);

		/* Reset was-newline flag */
		last_was_newline = false;
%}

	/* INITIAL state */

{nonspace}+		{
					/* Found a word, emit and return it */
					psqlscan_emit(cur_state, yytext, yyleng);
					return 1;
				}

	/*
	 * We need this rule to avoid returning "word\" instead of recognizing
	 * a continuation marker just after a word:
	 */
{nonspace}+{continuation}	{
					/* Found "word\\\r?\n", emit and return just "word" */
					int		wordlen = yyleng - 2;
					if (yytext[wordlen] == '\r')
						wordlen--;
					Assert(yytext[wordlen] == '\\');
					psqlscan_emit(cur_state, yytext, wordlen);
					return 1;
				}

{space}+		{ /* ignore */ }

{continuation}	{ /* ignore */ }

{newline}		{
					/* report end of command */
					last_was_newline = true;
					return 0;
				}

	/* EXPR state */

<EXPR>{

"+"				{ return '+'; }
"-"				{ return '-'; }
"*"				{ return '*'; }
"/"				{ return '/'; }
"%"				{ return '%'; } /* C version, also in Pg SQL */
"="				{ return '='; }
"<>"			{ return NE_OP; }
"!="			{ return NE_OP; } /* C version, also in Pg SQL */
"<="			{ return LE_OP; }
">="			{ return GE_OP; }
"<<"			{ return LS_OP; }
">>"			{ return RS_OP; }
"<"				{ return '<'; }
">"				{ return '>'; }
"|"				{ return '|'; }
"&"				{ return '&'; }
"#"				{ return '#'; }
"~"				{ return '~'; }

"("				{ return '('; }
")"				{ return ')'; }
","				{ return ','; }

{and}			{ return AND_OP; }
{or}			{ return OR_OP; }
{not}			{ return NOT_OP; }
{is}			{ return IS_OP; }
{isnull}		{ return ISNULL_OP; }
{notnull}		{ return NOTNULL_OP; }

{case}			{ return CASE_KW; }
{when}			{ return WHEN_KW; }
{then}			{ return THEN_KW; }
{else}			{ return ELSE_KW; }
{end}			{ return END_KW; }

:{alnum}+		{
					yylval->str = pg_strdup(yytext + 1);
					return VARIABLE;
				}

{null}			{ return NULL_CONST; }
{true}			{
					yylval->bval = true;
					return BOOLEAN_CONST;
				}
{false}			{
					yylval->bval = false;
					return BOOLEAN_CONST;
				}
"9223372036854775808" {
					/*
					 * Special handling for PG_INT64_MIN, which can't
					 * accurately be represented here, as the minus sign is
					 * lexed separately and INT64_MIN can't be represented as
					 * a positive integer.
					 */
					return MAXINT_PLUS_ONE_CONST;
				}
{digit}+		{
					if (!strtoint64(yytext, true, &yylval->ival))
						expr_yyerror_more(yyscanner, "bigint constant overflow",
										  strdup(yytext));
					return INTEGER_CONST;
				}
{digit}+(\.{digit}*)?([eE][-+]?{digit}+)?	{
					if (!strtodouble(yytext, true, &yylval->dval))
						expr_yyerror_more(yyscanner, "double constant overflow",
										  strdup(yytext));
					return DOUBLE_CONST;
				}
\.{digit}+([eE][-+]?{digit}+)?	{
					if (!strtodouble(yytext, true, &yylval->dval))
						expr_yyerror_more(yyscanner, "double constant overflow",
										  strdup(yytext));
					return DOUBLE_CONST;
				}
{alpha}{alnum}*	{
					yylval->str = pg_strdup(yytext);
					return FUNCTION;
				}

{space}+		{ /* ignore */ }

{continuation}	{ /* ignore */ }

{newline}		{
					/* report end of command */
					last_was_newline = true;
					return 0;
				}

.				{
					/*
					 * must strdup yytext so that expr_yyerror_more doesn't
					 * change it while finding end of line
					 */
					expr_yyerror_more(yyscanner, "unexpected character",
									  pg_strdup(yytext));
					/* NOTREACHED, syntax_error calls exit() */
					return 0;
				}

}

<<EOF>>			{
					if (cur_state->buffer_stack == NULL)
						return 0;			/* end of input reached */

					/*
					 * We were expanding a variable, so pop the inclusion
					 * stack and keep lexing
					 */
					psqlscan_pop_buffer_stack(cur_state);
					psqlscan_select_top_buffer(cur_state);
				}

%%

/* LCOV_EXCL_STOP */

void
expr_yyerror_more(yyscan_t yyscanner, const char *message, const char *more)
{
	PsqlScanState state = yyget_extra(yyscanner);
	int			error_detection_offset = expr_scanner_offset(state) - 1;
	YYSTYPE		lval;
	char	   *full_line;

	/*
	 * While parsing an expression, we may not have collected the whole line
	 * yet from the input source.  Lex till EOL so we can report whole line.
	 * (If we're at EOF, it's okay to call yylex() an extra time.)
	 */
	if (!last_was_newline)
	{
		while (yylex(&lval, yyscanner))
			 /* skip */ ;
	}

	/* Extract the line, trimming trailing newline if any */
	full_line = expr_scanner_get_substring(state,
										   expr_start_offset,
										   expr_scanner_offset(state),
										   true);

	syntax_error(expr_source, expr_lineno, full_line, expr_command,
				 message, more, error_detection_offset - expr_start_offset);
}

/*
 * (The first argument is enforced by Bison to match the first argument of
 * yyparse(), but it is not used here.)
 */
void
expr_yyerror(PgBenchExpr **expr_parse_result_p, yyscan_t yyscanner, const char *message)
{
	expr_yyerror_more(yyscanner, message, NULL);
}

/*
 * Collect a space-separated word from a backslash command and return it
 * in word_buf, along with its starting string offset in *offset.
 * Returns true if successful, false if at end of command.
 */
bool
expr_lex_one_word(PsqlScanState state, PQExpBuffer word_buf, int *offset)
{
	int			lexresult;
	YYSTYPE		lval;

	/* Must be scanning already */
	Assert(state->scanbufhandle != NULL);

	/* Set current output target */
	state->output_buf = word_buf;
	resetPQExpBuffer(word_buf);

	/* Set input source */
	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf, state->scanner);
	else
		yy_switch_to_buffer(state->scanbufhandle, state->scanner);

	/* Set start state */
	state->start_state = INITIAL;

	/* And lex. */
	lexresult = yylex(&lval, state->scanner);

	/*
	 * Save start offset of word, if any.  We could do this more efficiently,
	 * but for now this seems fine.
	 */
	if (lexresult)
		*offset = expr_scanner_offset(state) - word_buf->len;
	else
		*offset = -1;

	/*
	 * In case the caller returns to using the regular SQL lexer, reselect the
	 * appropriate initial state.
	 */
	psql_scan_reselect_sql_lexer(state);

	return (bool) lexresult;
}

/*
 * Prepare to lex an expression via expr_yyparse().
 *
 * Returns the yyscan_t that is to be passed to expr_yyparse().
 * (This is just state->scanner, but callers don't need to know that.)
 */
yyscan_t
expr_scanner_init(PsqlScanState state,
				  const char *source, int lineno, int start_offset,
				  const char *command)
{
	/* Save error context info */
	expr_source = source;
	expr_lineno = lineno;
	expr_start_offset = start_offset;
	expr_command = command;

	/* Must be scanning already */
	Assert(state->scanbufhandle != NULL);

	/* Set current output target */
	state->output_buf = NULL;

	/* Set input source */
	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf, state->scanner);
	else
		yy_switch_to_buffer(state->scanbufhandle, state->scanner);

	/* Set start state */
	state->start_state = EXPR;

	return state->scanner;
}

/*
 * Finish lexing an expression.
 */
void
expr_scanner_finish(yyscan_t yyscanner)
{
	PsqlScanState state = yyget_extra(yyscanner);

	/*
	 * Reselect appropriate initial state for SQL lexer.
	 */
	psql_scan_reselect_sql_lexer(state);
}

/*
 * Get offset from start of string to end of current lexer token.
 *
 * We rely on the knowledge that flex modifies the scan buffer by storing
 * a NUL at the end of the current token (yytext).  Note that this might
 * not work quite right if we were parsing a sub-buffer, but since pgbench
 * never invokes that functionality, it doesn't matter.
 */
int
expr_scanner_offset(PsqlScanState state)
{
	return strlen(state->scanbuf);
}

/*
 * Get a malloc'd copy of the lexer input string from start_offset
 * to just before end_offset.  If chomp is true, drop any trailing
 * newline(s).
 */
char *
expr_scanner_get_substring(PsqlScanState state,
						   int start_offset, int end_offset,
						   bool chomp)
{
	char	   *result;
	const char *scanptr = state->scanbuf + start_offset;
	int			slen = end_offset - start_offset;

	Assert(slen >= 0);
	Assert(end_offset <= strlen(state->scanbuf));

	if (chomp)
	{
		while (slen > 0 &&
			   (scanptr[slen - 1] == '\n' || scanptr[slen - 1] == '\r'))
			slen--;
	}

	result = (char *) pg_malloc(slen + 1);
	memcpy(result, scanptr, slen);
	result[slen] = '\0';

	return result;
}

/*
 * Get the line number associated with the given string offset
 * (which must not be past the end of where we've lexed to).
 */
int
expr_scanner_get_lineno(PsqlScanState state, int offset)
{
	int			lineno = 1;
	const char *p = state->scanbuf;

	while (*p && offset > 0)
	{
		if (*p == '\n')
			lineno++;
		p++, offset--;
	}
	return lineno;
}
