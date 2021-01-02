/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2021, PostgreSQL Global Development Group
 *
 * src/bin/psql/mainloop.c
 */
#include "postgres_fe.h"

#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "input.h"
#include "mainloop.h"
#include "mb/pg_wchar.h"
#include "prompt.h"
#include "settings.h"

/* callback functions for our flex lexer */
const PsqlScanCallbacks psqlscan_callbacks = {
	psql_get_variable,
};


/*
 * Main processing loop for reading lines of input
 *	and sending them to the backend.
 *
 * This loop is re-entrant. May be called by \i command
 *	which reads input from a file.
 */
int
MainLoop(FILE *source)
{
	PsqlScanState scan_state;	/* lexer working state */
	ConditionalStack cond_stack;	/* \if status stack */
	volatile PQExpBuffer query_buf; /* buffer for query being accumulated */
	volatile PQExpBuffer previous_buf;	/* if there isn't anything in the new
										 * buffer yet, use this one for \e,
										 * etc. */
	PQExpBuffer history_buf;	/* earlier lines of a multi-line command, not
								 * yet saved to readline history */
	char	   *line;			/* current line of input */
	int			added_nl_pos;
	bool		success;
	bool		line_saved_in_history;
	volatile int successResult = EXIT_SUCCESS;
	volatile backslashResult slashCmdStatus = PSQL_CMD_UNKNOWN;
	volatile promptStatus_t prompt_status = PROMPT_READY;
	volatile bool need_redisplay = false;
	volatile int count_eof = 0;
	volatile bool die_on_error = false;
	FILE	   *prev_cmd_source;
	bool		prev_cmd_interactive;
	uint64		prev_lineno;

	/* Save the prior command source */
	prev_cmd_source = pset.cur_cmd_source;
	prev_cmd_interactive = pset.cur_cmd_interactive;
	prev_lineno = pset.lineno;
	/* pset.stmt_lineno does not need to be saved and restored */

	/* Establish new source */
	pset.cur_cmd_source = source;
	pset.cur_cmd_interactive = ((source == stdin) && !pset.notty);
	pset.lineno = 0;
	pset.stmt_lineno = 1;

	/* Create working state */
	scan_state = psql_scan_create(&psqlscan_callbacks);
	cond_stack = conditional_stack_create();
	psql_scan_set_passthrough(scan_state, (void *) cond_stack);

	query_buf = createPQExpBuffer();
	previous_buf = createPQExpBuffer();
	history_buf = createPQExpBuffer();
	if (PQExpBufferBroken(query_buf) ||
		PQExpBufferBroken(previous_buf) ||
		PQExpBufferBroken(history_buf))
	{
		pg_log_error("out of memory");
		exit(EXIT_FAILURE);
	}

	/* main loop to get queries and execute them */
	while (successResult == EXIT_SUCCESS)
	{
		/*
		 * Clean up after a previous Control-C
		 */
		if (cancel_pressed)
		{
			if (!pset.cur_cmd_interactive)
			{
				/*
				 * You get here if you stopped a script with Ctrl-C.
				 */
				successResult = EXIT_USER;
				break;
			}

			cancel_pressed = false;
		}

		/*
		 * Establish longjmp destination for exiting from wait-for-input. We
		 * must re-do this each time through the loop for safety, since the
		 * jmpbuf might get changed during command execution.
		 */
		if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
		{
			/* got here with longjmp */

			/* reset parsing state */
			psql_scan_finish(scan_state);
			psql_scan_reset(scan_state);
			resetPQExpBuffer(query_buf);
			resetPQExpBuffer(history_buf);
			count_eof = 0;
			slashCmdStatus = PSQL_CMD_UNKNOWN;
			prompt_status = PROMPT_READY;
			need_redisplay = false;
			pset.stmt_lineno = 1;
			cancel_pressed = false;

			if (pset.cur_cmd_interactive)
			{
				putc('\n', stdout);

				/*
				 * if interactive user is in an \if block, then Ctrl-C will
				 * exit from the innermost \if.
				 */
				if (!conditional_stack_empty(cond_stack))
				{
					pg_log_error("\\if: escaped");
					conditional_stack_pop(cond_stack);
				}
			}
			else
			{
				successResult = EXIT_USER;
				break;
			}
		}

		fflush(stdout);

		/*
		 * get another line
		 */
		if (pset.cur_cmd_interactive)
		{
			/* May need to reset prompt, eg after \r command */
			if (query_buf->len == 0)
				prompt_status = PROMPT_READY;
			/* If query buffer came from \e, redisplay it with a prompt */
			if (need_redisplay)
			{
				if (query_buf->len > 0)
				{
					fputs(get_prompt(PROMPT_READY, cond_stack), stdout);
					fputs(query_buf->data, stdout);
					fflush(stdout);
				}
				need_redisplay = false;
			}
			/* Now we can fetch a line */
			line = gets_interactive(get_prompt(prompt_status, cond_stack),
									query_buf);
		}
		else
		{
			line = gets_fromFile(source);
			if (!line && ferror(source))
				successResult = EXIT_FAILURE;
		}

		/*
		 * query_buf holds query already accumulated.  line is the malloc'd
		 * new line of input (note it must be freed before looping around!)
		 */

		/* No more input.  Time to quit, or \i done */
		if (line == NULL)
		{
			if (pset.cur_cmd_interactive)
			{
				/* This tries to mimic bash's IGNOREEOF feature. */
				count_eof++;

				if (count_eof < pset.ignoreeof)
				{
					if (!pset.quiet)
						printf(_("Use \"\\q\" to leave %s.\n"), pset.progname);
					continue;
				}

				puts(pset.quiet ? "" : "\\q");
			}
			break;
		}

		count_eof = 0;

		pset.lineno++;

		/* ignore UTF-8 Unicode byte-order mark */
		if (pset.lineno == 1 && pset.encoding == PG_UTF8 && strncmp(line, "\xef\xbb\xbf", 3) == 0)
			memmove(line, line + 3, strlen(line + 3) + 1);

		/* Detect attempts to run custom-format dumps as SQL scripts */
		if (pset.lineno == 1 && !pset.cur_cmd_interactive &&
			strncmp(line, "PGDMP", 5) == 0)
		{
			free(line);
			puts(_("The input is a PostgreSQL custom-format dump.\n"
				   "Use the pg_restore command-line client to restore this dump to a database.\n"));
			fflush(stdout);
			successResult = EXIT_FAILURE;
			break;
		}

		/* no further processing of empty lines, unless within a literal */
		if (line[0] == '\0' && !psql_scan_in_quote(scan_state))
		{
			free(line);
			continue;
		}

		/* Recognize "help", "quit", "exit" only in interactive mode */
		if (pset.cur_cmd_interactive)
		{
			char	   *first_word = line;
			char	   *rest_of_line = NULL;
			bool		found_help = false;
			bool		found_exit_or_quit = false;
			bool		found_q = false;

			/*
			 * The assistance words, help/exit/quit, must have no whitespace
			 * before them, and only whitespace after, with an optional
			 * semicolon.  This prevents indented use of these words, perhaps
			 * as identifiers, from invoking the assistance behavior.
			 */
			if (pg_strncasecmp(first_word, "help", 4) == 0)
			{
				rest_of_line = first_word + 4;
				found_help = true;
			}
			else if (pg_strncasecmp(first_word, "exit", 4) == 0 ||
					 pg_strncasecmp(first_word, "quit", 4) == 0)
			{
				rest_of_line = first_word + 4;
				found_exit_or_quit = true;
			}
			else if (strncmp(first_word, "\\q", 2) == 0)
			{
				rest_of_line = first_word + 2;
				found_q = true;
			}

			/*
			 * If we found a command word, check whether the rest of the line
			 * contains only whitespace plus maybe one semicolon.  If not,
			 * ignore the command word after all.  These commands are only for
			 * compatibility with other SQL clients and are not documented.
			 */
			if (rest_of_line != NULL)
			{
				/*
				 * Ignore unless rest of line is whitespace, plus maybe one
				 * semicolon
				 */
				while (isspace((unsigned char) *rest_of_line))
					++rest_of_line;
				if (*rest_of_line == ';')
					++rest_of_line;
				while (isspace((unsigned char) *rest_of_line))
					++rest_of_line;
				if (*rest_of_line != '\0')
				{
					found_help = false;
					found_exit_or_quit = false;
				}
			}

			/*
			 * "help" is only a command when the query buffer is empty, but we
			 * emit a one-line message even when it isn't to help confused
			 * users.  The text is still added to the query buffer in that
			 * case.
			 */
			if (found_help)
			{
				if (query_buf->len != 0)
#ifndef WIN32
					puts(_("Use \\? for help or press control-C to clear the input buffer."));
#else
					puts(_("Use \\? for help."));
#endif
				else
				{
					puts(_("You are using psql, the command-line interface to PostgreSQL."));
					printf(_("Type:  \\copyright for distribution terms\n"
							 "       \\h for help with SQL commands\n"
							 "       \\? for help with psql commands\n"
							 "       \\g or terminate with semicolon to execute query\n"
							 "       \\q to quit\n"));
					free(line);
					fflush(stdout);
					continue;
				}
			}

			/*
			 * "quit" and "exit" are only commands when the query buffer is
			 * empty, but we emit a one-line message even when it isn't to
			 * help confused users.  The text is still added to the query
			 * buffer in that case.
			 */
			if (found_exit_or_quit)
			{
				if (query_buf->len != 0)
				{
					if (prompt_status == PROMPT_READY ||
						prompt_status == PROMPT_CONTINUE ||
						prompt_status == PROMPT_PAREN)
						puts(_("Use \\q to quit."));
					else
#ifndef WIN32
						puts(_("Use control-D to quit."));
#else
						puts(_("Use control-C to quit."));
#endif
				}
				else
				{
					/* exit app */
					free(line);
					fflush(stdout);
					successResult = EXIT_SUCCESS;
					break;
				}
			}

			/*
			 * If they typed "\q" in a place where "\q" is not active, supply
			 * a hint.  The text is still added to the query buffer.
			 */
			if (found_q && query_buf->len != 0 &&
				prompt_status != PROMPT_READY &&
				prompt_status != PROMPT_CONTINUE &&
				prompt_status != PROMPT_PAREN)
#ifndef WIN32
				puts(_("Use control-D to quit."));
#else
				puts(_("Use control-C to quit."));
#endif
		}

		/* echo back if flag is set, unless interactive */
		if (pset.echo == PSQL_ECHO_ALL && !pset.cur_cmd_interactive)
		{
			puts(line);
			fflush(stdout);
		}

		/* insert newlines into query buffer between source lines */
		if (query_buf->len > 0)
		{
			appendPQExpBufferChar(query_buf, '\n');
			added_nl_pos = query_buf->len;
		}
		else
			added_nl_pos = -1;	/* flag we didn't add one */

		/* Setting this will not have effect until next line. */
		die_on_error = pset.on_error_stop;

		/*
		 * Parse line, looking for command separators.
		 */
		psql_scan_setup(scan_state, line, strlen(line),
						pset.encoding, standard_strings());
		success = true;
		line_saved_in_history = false;

		while (success || !die_on_error)
		{
			PsqlScanResult scan_result;
			promptStatus_t prompt_tmp = prompt_status;
			size_t		pos_in_query;
			char	   *tmp_line;

			pos_in_query = query_buf->len;
			scan_result = psql_scan(scan_state, query_buf, &prompt_tmp);
			prompt_status = prompt_tmp;

			if (PQExpBufferBroken(query_buf))
			{
				pg_log_error("out of memory");
				exit(EXIT_FAILURE);
			}

			/*
			 * Increase statement line number counter for each linebreak added
			 * to the query buffer by the last psql_scan() call. There only
			 * will be ones to add when navigating to a statement in
			 * readline's history containing newlines.
			 */
			tmp_line = query_buf->data + pos_in_query;
			while (*tmp_line != '\0')
			{
				if (*(tmp_line++) == '\n')
					pset.stmt_lineno++;
			}

			if (scan_result == PSCAN_EOL)
				pset.stmt_lineno++;

			/*
			 * Send command if semicolon found, or if end of line and we're in
			 * single-line mode.
			 */
			if (scan_result == PSCAN_SEMICOLON ||
				(scan_result == PSCAN_EOL && pset.singleline))
			{
				/*
				 * Save line in history.  We use history_buf to accumulate
				 * multi-line queries into a single history entry.  Note that
				 * history accumulation works on input lines, so it doesn't
				 * matter whether the query will be ignored due to \if.
				 */
				if (pset.cur_cmd_interactive && !line_saved_in_history)
				{
					pg_append_history(line, history_buf);
					pg_send_history(history_buf);
					line_saved_in_history = true;
				}

				/* execute query unless we're in an inactive \if branch */
				if (conditional_active(cond_stack))
				{
					success = SendQuery(query_buf->data);
					slashCmdStatus = success ? PSQL_CMD_SEND : PSQL_CMD_ERROR;
					pset.stmt_lineno = 1;

					/* transfer query to previous_buf by pointer-swapping */
					{
						PQExpBuffer swap_buf = previous_buf;

						previous_buf = query_buf;
						query_buf = swap_buf;
					}
					resetPQExpBuffer(query_buf);

					added_nl_pos = -1;
					/* we need not do psql_scan_reset() here */
				}
				else
				{
					/* if interactive, warn about non-executed query */
					if (pset.cur_cmd_interactive)
						pg_log_error("query ignored; use \\endif or Ctrl-C to exit current \\if block");
					/* fake an OK result for purposes of loop checks */
					success = true;
					slashCmdStatus = PSQL_CMD_SEND;
					pset.stmt_lineno = 1;
					/* note that query_buf doesn't change state */
				}
			}
			else if (scan_result == PSCAN_BACKSLASH)
			{
				/* handle backslash command */

				/*
				 * If we added a newline to query_buf, and nothing else has
				 * been inserted in query_buf by the lexer, then strip off the
				 * newline again.  This avoids any change to query_buf when a
				 * line contains only a backslash command.  Also, in this
				 * situation we force out any previous lines as a separate
				 * history entry; we don't want SQL and backslash commands
				 * intermixed in history if at all possible.
				 */
				if (query_buf->len == added_nl_pos)
				{
					query_buf->data[--query_buf->len] = '\0';
					pg_send_history(history_buf);
				}
				added_nl_pos = -1;

				/* save backslash command in history */
				if (pset.cur_cmd_interactive && !line_saved_in_history)
				{
					pg_append_history(line, history_buf);
					pg_send_history(history_buf);
					line_saved_in_history = true;
				}

				/* execute backslash command */
				slashCmdStatus = HandleSlashCmds(scan_state,
												 cond_stack,
												 query_buf,
												 previous_buf);

				success = slashCmdStatus != PSQL_CMD_ERROR;

				/*
				 * Resetting stmt_lineno after a backslash command isn't
				 * always appropriate, but it's what we've done historically
				 * and there have been few complaints.
				 */
				pset.stmt_lineno = 1;

				if (slashCmdStatus == PSQL_CMD_SEND)
				{
					/* should not see this in inactive branch */
					Assert(conditional_active(cond_stack));

					success = SendQuery(query_buf->data);

					/* transfer query to previous_buf by pointer-swapping */
					{
						PQExpBuffer swap_buf = previous_buf;

						previous_buf = query_buf;
						query_buf = swap_buf;
					}
					resetPQExpBuffer(query_buf);

					/* flush any paren nesting info after forced send */
					psql_scan_reset(scan_state);
				}
				else if (slashCmdStatus == PSQL_CMD_NEWEDIT)
				{
					/* should not see this in inactive branch */
					Assert(conditional_active(cond_stack));
					/* ensure what came back from editing ends in a newline */
					if (query_buf->len > 0 &&
						query_buf->data[query_buf->len - 1] != '\n')
						appendPQExpBufferChar(query_buf, '\n');
					/* rescan query_buf as new input */
					psql_scan_finish(scan_state);
					free(line);
					line = pg_strdup(query_buf->data);
					resetPQExpBuffer(query_buf);
					/* reset parsing state since we are rescanning whole line */
					psql_scan_reset(scan_state);
					psql_scan_setup(scan_state, line, strlen(line),
									pset.encoding, standard_strings());
					line_saved_in_history = false;
					prompt_status = PROMPT_READY;
					/* we'll want to redisplay after parsing what we have */
					need_redisplay = true;
				}
				else if (slashCmdStatus == PSQL_CMD_TERMINATE)
					break;
			}

			/* fall out of loop if lexer reached EOL */
			if (scan_result == PSCAN_INCOMPLETE ||
				scan_result == PSCAN_EOL)
				break;
		}

		/* Add line to pending history if we didn't execute anything yet */
		if (pset.cur_cmd_interactive && !line_saved_in_history)
			pg_append_history(line, history_buf);

		psql_scan_finish(scan_state);
		free(line);

		if (slashCmdStatus == PSQL_CMD_TERMINATE)
		{
			successResult = EXIT_SUCCESS;
			break;
		}

		if (!pset.cur_cmd_interactive)
		{
			if (!success && die_on_error)
				successResult = EXIT_USER;
			/* Have we lost the db connection? */
			else if (!pset.db)
				successResult = EXIT_BADCONN;
		}
	}							/* while !endoffile/session */

	/*
	 * If we have a non-semicolon-terminated query at the end of file, we
	 * process it unless the input source is interactive --- in that case it
	 * seems better to go ahead and quit.  Also skip if this is an error exit.
	 */
	if (query_buf->len > 0 && !pset.cur_cmd_interactive &&
		successResult == EXIT_SUCCESS)
	{
		/* save query in history */
		/* currently unneeded since we don't use this block if interactive */
#ifdef NOT_USED
		if (pset.cur_cmd_interactive)
			pg_send_history(history_buf);
#endif

		/* execute query unless we're in an inactive \if branch */
		if (conditional_active(cond_stack))
		{
			success = SendQuery(query_buf->data);
		}
		else
		{
			if (pset.cur_cmd_interactive)
				pg_log_error("query ignored; use \\endif or Ctrl-C to exit current \\if block");
			success = true;
		}

		if (!success && die_on_error)
			successResult = EXIT_USER;
		else if (pset.db == NULL)
			successResult = EXIT_BADCONN;
	}

	/*
	 * Check for unbalanced \if-\endifs unless user explicitly quit, or the
	 * script is erroring out
	 */
	if (slashCmdStatus != PSQL_CMD_TERMINATE &&
		successResult != EXIT_USER &&
		!conditional_stack_empty(cond_stack))
	{
		pg_log_error("reached EOF without finding closing \\endif(s)");
		if (die_on_error && !pset.cur_cmd_interactive)
			successResult = EXIT_USER;
	}

	/*
	 * Let's just make real sure the SIGINT handler won't try to use
	 * sigint_interrupt_jmp after we exit this routine.  If there is an outer
	 * MainLoop instance, it will reset sigint_interrupt_jmp to point to
	 * itself at the top of its loop, before any further interactive input
	 * happens.
	 */
	sigint_interrupt_enabled = false;

	destroyPQExpBuffer(query_buf);
	destroyPQExpBuffer(previous_buf);
	destroyPQExpBuffer(history_buf);

	psql_scan_destroy(scan_state);
	conditional_stack_destroy(cond_stack);

	pset.cur_cmd_source = prev_cmd_source;
	pset.cur_cmd_interactive = prev_cmd_interactive;
	pset.lineno = prev_lineno;

	return successResult;
}								/* MainLoop() */
