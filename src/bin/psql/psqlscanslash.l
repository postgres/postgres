%top{
/*-------------------------------------------------------------------------
 *
 * psqlscanslash.l
 *	  lexical scanner for psql backslash commands
 *
 * XXX Avoid creating backtracking cases --- see the backend lexer for info.
 *
 * See fe_utils/psqlscan_int.h for additional commentary.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/bin/psql/psqlscanslash.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "psqlscanslash.h"
#include "common/logging.h"
#include "fe_utils/conditional.h"

#include "libpq-fe.h"
}

%{
#include "fe_utils/psqlscan_int.h"

/*
 * We must have a typedef YYSTYPE for yylex's first argument, but this lexer
 * doesn't presently make use of that argument, so just declare it as int.
 */
typedef int YYSTYPE;

/*
 * Set the type of yyextra; we use it as a pointer back to the containing
 * PsqlScanState.
 */
#define YY_EXTRA_TYPE PsqlScanState

/*
 * These variables do not need to be saved across calls.  Yeah, it's a bit
 * of a hack, but putting them into PsqlScanStateData would be klugy too.
 */
static enum slash_option_type option_type;
static char *option_quote;
static int	unquoted_option_chars;
static int	backtick_start_offset;


/* Return values from yylex() */
#define LEXRES_EOL			0	/* end of input */
#define LEXRES_OK			1	/* OK completion of backslash argument */


static void evaluate_backtick(PsqlScanState state);

#define ECHO psqlscan_emit(cur_state, yytext, yyleng)

/*
 * Work around a bug in flex 2.5.35: it emits a couple of functions that
 * it forgets to emit declarations for.  Since we use -Wmissing-prototypes,
 * this would cause warnings.  Providing our own declarations should be
 * harmless even when the bug gets fixed.
 */
extern int	slash_yyget_column(yyscan_t yyscanner);
extern void slash_yyset_column(int column_no, yyscan_t yyscanner);

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
%option prefix="slash_yy"

/*
 * OK, here is a short description of lex/flex rules behavior.
 * The longest pattern which matches an input string is always chosen.
 * For equal-length patterns, the first occurring in the rules list is chosen.
 * INITIAL is the starting state, to which all non-conditional rules apply.
 * Exclusive states change parsing rules while the state is active.  When in
 * an exclusive state, only those rules defined for that state apply.
 */

/* Exclusive states for lexing backslash commands */
%x xslashcmd
%x xslashargstart
%x xslasharg
%x xslashquote
%x xslashbackquote
%x xslashdquote
%x xslashwholeline
%x xslashend

/*
 * Assorted character class definitions that should match psqlscan.l.
 */
space			[ \t\n\r\f]
quote			'
xeoctesc		[\\][0-7]{1,3}
xehexesc		[\\]x[0-9A-Fa-f]{1,2}
xqdouble		{quote}{quote}
dquote			\"
variable_char	[A-Za-z\200-\377_0-9]

other			.

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

	/*
	 * We don't really expect to be invoked in the INITIAL state in this
	 * lexer; but if we are, just spit data to the output_buf until EOF.
	 */

{other}|\n		{ ECHO; }

	/*
	 * Exclusive lexer states to handle backslash command lexing
	 */

<xslashcmd>{
	/* command name ends at whitespace or backslash; eat all else */

{space}|"\\"	{
					yyless(0);
					cur_state->start_state = YY_START;
					return LEXRES_OK;
				}

{other}			{ ECHO; }

}

<xslashargstart>{
	/*
	 * Discard any whitespace before argument, then go to xslasharg state.
	 * An exception is that "|" is only special at start of argument, so we
	 * check for it here.
	 */

{space}+		{ }

"|"				{
					if (option_type == OT_FILEPIPE)
					{
						/* treat like whole-string case */
						ECHO;
						BEGIN(xslashwholeline);
					}
					else
					{
						/* vertical bar is not special otherwise */
						yyless(0);
						BEGIN(xslasharg);
					}
				}

{other}			{
					yyless(0);
					BEGIN(xslasharg);
				}

}

<xslasharg>{
	/*
	 * Default processing of text in a slash command's argument.
	 *
	 * Note: unquoted_option_chars counts the number of characters at the
	 * end of the argument that were not subject to any form of quoting.
	 * psql_scan_slash_option needs this to strip trailing semicolons safely.
	 */

{space}|"\\"	{
					/*
					 * Unquoted space is end of arg; do not eat.  Likewise
					 * backslash is end of command or next command, do not eat
					 *
					 * XXX this means we can't conveniently accept options
					 * that include unquoted backslashes; therefore, option
					 * processing that encourages use of backslashes is rather
					 * broken.
					 */
					yyless(0);
					cur_state->start_state = YY_START;
					return LEXRES_OK;
				}

{quote}			{
					*option_quote = '\'';
					unquoted_option_chars = 0;
					BEGIN(xslashquote);
				}

"`"				{
					backtick_start_offset = output_buf->len;
					*option_quote = '`';
					unquoted_option_chars = 0;
					BEGIN(xslashbackquote);
				}

{dquote}		{
					ECHO;
					*option_quote = '"';
					unquoted_option_chars = 0;
					BEGIN(xslashdquote);
				}

:{variable_char}+	{
					/* Possible psql variable substitution */
					if (cur_state->callbacks->get_variable == NULL)
						ECHO;
					else
					{
						char	   *varname;
						char	   *value;

						varname = psqlscan_extract_substring(cur_state,
															 yytext + 1,
															 yyleng - 1);
						value = cur_state->callbacks->get_variable(varname,
																   PQUOTE_PLAIN,
																   cur_state->cb_passthrough);
						free(varname);

						/*
						 * The variable value is just emitted without any
						 * further examination.  This is consistent with the
						 * pre-8.0 code behavior, if not with the way that
						 * variables are handled outside backslash commands.
						 * Note that we needn't guard against recursion here.
						 */
						if (value)
						{
							appendPQExpBufferStr(output_buf, value);
							free(value);
						}
						else
							ECHO;

						*option_quote = ':';
					}
					unquoted_option_chars = 0;
				}

:'{variable_char}+'	{
					psqlscan_escape_variable(cur_state, yytext, yyleng,
											 PQUOTE_SQL_LITERAL);
					*option_quote = ':';
					unquoted_option_chars = 0;
				}


:\"{variable_char}+\"	{
					psqlscan_escape_variable(cur_state, yytext, yyleng,
											 PQUOTE_SQL_IDENT);
					*option_quote = ':';
					unquoted_option_chars = 0;
				}

:\{\?{variable_char}+\}	{
					psqlscan_test_variable(cur_state, yytext, yyleng);
				}

:'{variable_char}*	{
					/* Throw back everything but the colon */
					yyless(1);
					unquoted_option_chars++;
					ECHO;
				}

:\"{variable_char}*	{
					/* Throw back everything but the colon */
					yyless(1);
					unquoted_option_chars++;
					ECHO;
				}

:\{\?{variable_char}*	{
					/* Throw back everything but the colon */
					yyless(1);
					unquoted_option_chars++;
					ECHO;
				}

:\{		{
					/* Throw back everything but the colon */
					yyless(1);
					unquoted_option_chars++;
					ECHO;
				}

{other}			{
					unquoted_option_chars++;
					ECHO;
				}

}

<xslashquote>{
	/*
	 * single-quoted text: copy literally except for '' and backslash
	 * sequences
	 */

{quote}			{ BEGIN(xslasharg); }

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

"\\".			{ psqlscan_emit(cur_state, yytext + 1, 1); }

{other}|\n		{ ECHO; }

}

<xslashbackquote>{
	/*
	 * backticked text: copy everything until next backquote (expanding
	 * variable references, but doing nought else), then evaluate.
	 */

"`"				{
					/* In an inactive \if branch, don't evaluate the command */
					if (cur_state->cb_passthrough == NULL ||
						conditional_active((ConditionalStack) cur_state->cb_passthrough))
						evaluate_backtick(cur_state);
					BEGIN(xslasharg);
				}

:{variable_char}+	{
					/* Possible psql variable substitution */
					if (cur_state->callbacks->get_variable == NULL)
						ECHO;
					else
					{
						char	   *varname;
						char	   *value;

						varname = psqlscan_extract_substring(cur_state,
															 yytext + 1,
															 yyleng - 1);
						value = cur_state->callbacks->get_variable(varname,
																   PQUOTE_PLAIN,
																   cur_state->cb_passthrough);
						free(varname);

						if (value)
						{
							appendPQExpBufferStr(output_buf, value);
							free(value);
						}
						else
							ECHO;
					}
				}

:'{variable_char}+'	{
					psqlscan_escape_variable(cur_state, yytext, yyleng,
											 PQUOTE_SHELL_ARG);
				}

:'{variable_char}*	{
					/* Throw back everything but the colon */
					yyless(1);
					ECHO;
				}

{other}|\n		{ ECHO; }

}

<xslashdquote>{
	/* double-quoted text: copy verbatim, including the double quotes */

{dquote}		{
					ECHO;
					BEGIN(xslasharg);
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

"\\\\"			{
					cur_state->start_state = YY_START;
					return LEXRES_OK;
				}

{other}|\n		{
					yyless(0);
					cur_state->start_state = YY_START;
					return LEXRES_OK;
				}

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

	/* Must be scanning already */
	Assert(state->scanbufhandle != NULL);

	/* Build a local buffer that we'll return the data of */
	initPQExpBuffer(&mybuf);

	/* Set current output target */
	state->output_buf = &mybuf;

	/* Set input source */
	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf, state->scanner);
	else
		yy_switch_to_buffer(state->scanbufhandle, state->scanner);

	/*
	 * Set lexer start state.  Note that this is sufficient to switch
	 * state->scanner over to using the tables in this lexer file.
	 */
	state->start_state = xslashcmd;

	/* And lex. */
	yylex(NULL, state->scanner);

	/* There are no possible errors in this lex state... */

	/*
	 * In case the caller returns to using the regular SQL lexer, reselect the
	 * appropriate initial state.
	 */
	psql_scan_reselect_sql_lexer(state);

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
 * the last quote symbol used in the argument.
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
	int			lexresult PG_USED_FOR_ASSERTS_ONLY;
	int			final_state;
	char		local_quote;

	/* Must be scanning already */
	Assert(state->scanbufhandle != NULL);

	if (quote == NULL)
		quote = &local_quote;
	*quote = 0;

	/* Build a local buffer that we'll return the data of */
	initPQExpBuffer(&mybuf);

	/* Set up static variables that will be used by yylex */
	option_type = type;
	option_quote = quote;
	unquoted_option_chars = 0;

	/* Set current output target */
	state->output_buf = &mybuf;

	/* Set input source */
	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf, state->scanner);
	else
		yy_switch_to_buffer(state->scanbufhandle, state->scanner);

	/* Set lexer start state */
	if (type == OT_WHOLE_LINE)
		state->start_state = xslashwholeline;
	else
		state->start_state = xslashargstart;

	/* And lex. */
	lexresult = yylex(NULL, state->scanner);

	/* Save final state for a moment... */
	final_state = state->start_state;

	/*
	 * In case the caller returns to using the regular SQL lexer, reselect the
	 * appropriate initial state.
	 */
	psql_scan_reselect_sql_lexer(state);

	/*
	 * Check the lex result: we should have gotten back either LEXRES_OK
	 * or LEXRES_EOL (the latter indicating end of string).  If we were inside
	 * a quoted string, as indicated by final_state, EOL is an error.
	 */
	Assert(lexresult == LEXRES_EOL || lexresult == LEXRES_OK);

	switch (final_state)
	{
		case xslashargstart:
			/* empty arg */
			break;
		case xslasharg:
			/* Strip any unquoted trailing semi-colons if requested */
			if (semicolon)
			{
				while (unquoted_option_chars-- > 0 &&
					   mybuf.len > 0 &&
					   mybuf.data[mybuf.len - 1] == ';')
				{
					mybuf.data[--mybuf.len] = '\0';
				}
			}

			/*
			 * If SQL identifier processing was requested, then we strip out
			 * excess double quotes and optionally downcase unquoted letters.
			 */
			if (type == OT_SQLID || type == OT_SQLIDHACK)
			{
				dequote_downcase_identifier(mybuf.data,
											(type != OT_SQLIDHACK),
											state->encoding);
				/* update mybuf.len for possible shortening */
				mybuf.len = strlen(mybuf.data);
			}
			break;
		case xslashquote:
		case xslashbackquote:
		case xslashdquote:
			/* must have hit EOL inside quotes */
			pg_log_error("unterminated quoted string");
			termPQExpBuffer(&mybuf);
			return NULL;
		case xslashwholeline:
			/* always okay */
			break;
		default:
			/* can't get here */
			fprintf(stderr, "invalid YY_START\n");
			exit(1);
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
	/* Must be scanning already */
	Assert(state->scanbufhandle != NULL);

	/* Set current output target */
	state->output_buf = NULL;	/* we won't output anything */

	/* Set input source */
	if (state->buffer_stack != NULL)
		yy_switch_to_buffer(state->buffer_stack->buf, state->scanner);
	else
		yy_switch_to_buffer(state->scanbufhandle, state->scanner);

	/* Set lexer start state */
	state->start_state = xslashend;

	/* And lex. */
	yylex(NULL, state->scanner);

	/* There are no possible errors in this lex state... */

	/*
	 * We expect the caller to return to using the regular SQL lexer, so
	 * reselect the appropriate initial state.
	 */
	psql_scan_reselect_sql_lexer(state);
}

/*
 * Fetch current paren nesting depth
 */
int
psql_scan_get_paren_depth(PsqlScanState state)
{
	return state->paren_depth;
}

/*
 * Set paren nesting depth
 */
void
psql_scan_set_paren_depth(PsqlScanState state, int depth)
{
	Assert(depth >= 0);
	state->paren_depth = depth;
}

/*
 * De-quote and optionally downcase a SQL identifier.
 *
 * The string at *str is modified in-place; it can become shorter,
 * but not longer.
 *
 * If downcase is true then non-quoted letters are folded to lower case.
 * Ideally this behavior will match the backend's downcase_identifier();
 * but note that it could differ if LC_CTYPE is different in the frontend.
 *
 * Note that a string like FOO"BAR"BAZ will be converted to fooBARbaz;
 * this is somewhat inconsistent with the SQL spec, which would have us
 * parse it as several identifiers.  But for psql's purposes, we want a
 * string like "foo"."bar" to be treated as one option, so there's little
 * choice; this routine doesn't get to change the token boundaries.
 */
void
dequote_downcase_identifier(char *str, bool downcase, int encoding)
{
	bool		inquotes = false;
	char	   *cp = str;

	while (*cp)
	{
		if (*cp == '"')
		{
			if (inquotes && cp[1] == '"')
			{
				/* Keep the first quote, remove the second */
				cp++;
			}
			else
				inquotes = !inquotes;
			/* Collapse out quote at *cp */
			memmove(cp, cp + 1, strlen(cp));
			/* do not advance cp */
		}
		else
		{
			if (downcase && !inquotes)
				*cp = pg_tolower((unsigned char) *cp);
			cp += PQmblen(cp, encoding);
		}
	}
}

/*
 * Evaluate a backticked substring of a slash command's argument.
 *
 * The portion of output_buf starting at backtick_start_offset is evaluated
 * as a shell command and then replaced by the command's output.
 */
static void
evaluate_backtick(PsqlScanState state)
{
	PQExpBuffer output_buf = state->output_buf;
	char	   *cmd = output_buf->data + backtick_start_offset;
	PQExpBufferData cmd_output;
	FILE	   *fd;
	bool		error = false;
	char		buf[512];
	size_t		result;

	initPQExpBuffer(&cmd_output);

	fd = popen(cmd, PG_BINARY_R);
	if (!fd)
	{
		pg_log_error("%s: %m", cmd);
		error = true;
	}

	if (!error)
	{
		do
		{
			result = fread(buf, 1, sizeof(buf), fd);
			if (ferror(fd))
			{
				pg_log_error("%s: %m", cmd);
				error = true;
				break;
			}
			appendBinaryPQExpBuffer(&cmd_output, buf, result);
		} while (!feof(fd));
	}

	if (fd && pclose(fd) == -1)
	{
		pg_log_error("%s: %m", cmd);
		error = true;
	}

	if (PQExpBufferDataBroken(cmd_output))
	{
		pg_log_error("%s: out of memory", cmd);
		error = true;
	}

	/* Now done with cmd, delete it from output_buf */
	output_buf->len = backtick_start_offset;
	output_buf->data[output_buf->len] = '\0';

	/* If no error, transfer result to output_buf */
	if (!error)
	{
		/* strip any trailing newline */
		if (cmd_output.len > 0 &&
			cmd_output.data[cmd_output.len - 1] == '\n')
			cmd_output.len--;
		appendBinaryPQExpBuffer(output_buf, cmd_output.data, cmd_output.len);
	}

	termPQExpBuffer(&cmd_output);
}
