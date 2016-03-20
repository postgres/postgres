%{
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
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/exprscan.l
 *
 *-------------------------------------------------------------------------
 */

#include "psqlscan_int.h"

/* context information for reporting errors in expressions */
static const char *expr_source = NULL;
static int	expr_lineno = 0;
static int	expr_start_offset = 0;
static const char *expr_command = NULL;

/* indicates whether last yylex() call read a newline */
static bool last_was_newline = false;

/*
 * Work around a bug in flex 2.5.35: it emits a couple of functions that
 * it forgets to emit declarations for.  Since we use -Wmissing-prototypes,
 * this would cause warnings.  Providing our own declarations should be
 * harmless even when the bug gets fixed.
 */
extern int	expr_yyget_column(yyscan_t yyscanner);
extern void expr_yyset_column(int column_no, yyscan_t yyscanner);

%}

/* Except for the prefix, these options should match psqlscan.l */
%option reentrant
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="expr_yy"

/* Character classes */
alpha			[a-zA-Z_]
digit			[0-9]
alnum			[a-zA-Z0-9_]
/* {space} + {nonspace} + {newline} should cover all characters */
space			[ \t\r\f\v]
nonspace		[^ \t\r\f\v\n]
newline			[\n]

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

{space}+		{ /* ignore */ }

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
"%"				{ return '%'; }
"("				{ return '('; }
")"				{ return ')'; }
","				{ return ','; }

:{alnum}+		{
					yylval.str = pg_strdup(yytext + 1);
					return VARIABLE;
				}
{digit}+		{
					yylval.ival = strtoint64(yytext);
					return INTEGER;
				}
{alpha}{alnum}*	{
					yylval.str = pg_strdup(yytext);
					return FUNCTION;
				}

{newline}		{
					/* report end of command */
					last_was_newline = true;
					return 0;
				}

{space}+		{ /* ignore */ }

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

void
expr_yyerror_more(yyscan_t yyscanner, const char *message, const char *more)
{
	PsqlScanState state = yyget_extra(yyscanner);
	int			error_detection_offset = expr_scanner_offset(state) - 1;
	char	   *full_line;
	size_t		l;

	/*
	 * While parsing an expression, we may not have collected the whole line
	 * yet from the input source.  Lex till EOL so we can report whole line.
	 * (If we're at EOF, it's okay to call yylex() an extra time.)
	 */
	if (!last_was_newline)
	{
		while (yylex(yyscanner))
			 /* skip */ ;
	}

	full_line = expr_scanner_get_substring(state,
										   expr_start_offset,
										   expr_scanner_offset(state));
	/* Trim trailing newline if any */
	l = strlen(full_line);
	while (l > 0 && full_line[l - 1] == '\n')
		full_line[--l] = '\0';

	syntax_error(expr_source, expr_lineno, full_line, expr_command,
				 message, more, error_detection_offset - expr_start_offset);
}

void
expr_yyerror(yyscan_t yyscanner, const char *message)
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
	lexresult = yylex(state->scanner);

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
 * to just before end_offset.
 */
char *
expr_scanner_get_substring(PsqlScanState state,
						   int start_offset, int end_offset)
{
	char	   *result;
	int			slen = end_offset - start_offset;

	Assert(slen >= 0);
	Assert(end_offset <= strlen(state->scanbuf));
	result = (char *) pg_malloc(slen + 1);
	memcpy(result, state->scanbuf + start_offset, slen);
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
