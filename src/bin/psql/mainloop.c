/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2010, PostgreSQL Global Development Group
 *
 * src/bin/psql/mainloop.c
 */
#include "postgres_fe.h"
#include "mainloop.h"


#include "command.h"
#include "common.h"
#include "input.h"
#include "settings.h"

#include "mb/pg_wchar.h"


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
	volatile PQExpBuffer query_buf;		/* buffer for query being accumulated */
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
	volatile int count_eof = 0;
	volatile bool die_on_error = false;

	/* Save the prior command source */
	FILE	   *prev_cmd_source;
	bool		prev_cmd_interactive;
	uint64		prev_lineno;

	/* Save old settings */
	prev_cmd_source = pset.cur_cmd_source;
	prev_cmd_interactive = pset.cur_cmd_interactive;
	prev_lineno = pset.lineno;

	/* Establish new source */
	pset.cur_cmd_source = source;
	pset.cur_cmd_interactive = ((source == stdin) && !pset.notty);
	pset.lineno = 0;

	/* Create working state */
	scan_state = psql_scan_create();

	query_buf = createPQExpBuffer();
	previous_buf = createPQExpBuffer();
	history_buf = createPQExpBuffer();
	if (PQExpBufferBroken(query_buf) ||
		PQExpBufferBroken(previous_buf) ||
		PQExpBufferBroken(history_buf))
	{
		psql_error("out of memory\n");
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
			cancel_pressed = false;

			if (pset.cur_cmd_interactive)
				putc('\n', stdout);
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
			line = gets_interactive(get_prompt(prompt_status));
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

				if (count_eof < GetVariableNum(pset.vars, "IGNOREEOF", 0, 10, false))
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

		/* nothing left on line? then ignore */
		if (line[0] == '\0' && !psql_scan_in_quote(scan_state))
		{
			free(line);
			continue;
		}

		/* A request for help? Be friendly and give them some guidance */
		if (pset.cur_cmd_interactive && query_buf->len == 0 &&
			pg_strncasecmp(line, "help", 4) == 0 &&
			(line[4] == '\0' || line[4] == ';' || isspace((unsigned char) line[4])))
		{
			free(line);
			puts(_("You are using psql, the command-line interface to PostgreSQL."));
			printf(_("Type:  \\copyright for distribution terms\n"
					 "       \\h for help with SQL commands\n"
					 "       \\? for help with psql commands\n"
				  "       \\g or terminate with semicolon to execute query\n"
					 "       \\q to quit\n"));

			fflush(stdout);
			continue;
		}

		/* echo back if flag is set */
		if (pset.echo == PSQL_ECHO_ALL && !pset.cur_cmd_interactive)
			puts(line);
		fflush(stdout);

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
		psql_scan_setup(scan_state, line, strlen(line));
		success = true;
		line_saved_in_history = false;

		while (success || !die_on_error)
		{
			PsqlScanResult scan_result;
			promptStatus_t prompt_tmp = prompt_status;

			scan_result = psql_scan(scan_state, query_buf, &prompt_tmp);
			prompt_status = prompt_tmp;

			if (PQExpBufferBroken(query_buf))
			{
				psql_error("out of memory\n");
				exit(EXIT_FAILURE);
			}

			/*
			 * Send command if semicolon found, or if end of line and we're in
			 * single-line mode.
			 */
			if (scan_result == PSCAN_SEMICOLON ||
				(scan_result == PSCAN_EOL && pset.singleline))
			{
				/*
				 * Save query in history.  We use history_buf to accumulate
				 * multi-line queries into a single history entry.
				 */
				if (pset.cur_cmd_interactive && !line_saved_in_history)
				{
					pg_append_history(line, history_buf);
					pg_send_history(history_buf);
					line_saved_in_history = true;
				}

				/* execute query */
				success = SendQuery(query_buf->data);
				slashCmdStatus = success ? PSQL_CMD_SEND : PSQL_CMD_ERROR;

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
			else if (scan_result == PSCAN_BACKSLASH)
			{
				/* handle backslash command */

				/*
				 * If we added a newline to query_buf, and nothing else has
				 * been inserted in query_buf by the lexer, then strip off the
				 * newline again.  This avoids any change to query_buf when a
				 * line contains only a backslash command.	Also, in this
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
												 query_buf->len > 0 ?
												 query_buf : previous_buf);

				success = slashCmdStatus != PSQL_CMD_ERROR;

				if ((slashCmdStatus == PSQL_CMD_SEND || slashCmdStatus == PSQL_CMD_NEWEDIT) &&
					query_buf->len == 0)
				{
					/* copy previous buffer to current for handling */
					appendPQExpBufferStr(query_buf, previous_buf->data);
				}

				if (slashCmdStatus == PSQL_CMD_SEND)
				{
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
					/* rescan query_buf as new input */
					psql_scan_finish(scan_state);
					free(line);
					line = pg_strdup(query_buf->data);
					resetPQExpBuffer(query_buf);
					/* reset parsing state since we are rescanning whole line */
					psql_scan_reset(scan_state);
					psql_scan_setup(scan_state, line, strlen(line));
					line_saved_in_history = false;
					prompt_status = PROMPT_READY;
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
	 * Process query at the end of file without a semicolon
	 */
	if (query_buf->len > 0 && !pset.cur_cmd_interactive &&
		successResult == EXIT_SUCCESS)
	{
		/* save query in history */
		if (pset.cur_cmd_interactive)
			pg_send_history(history_buf);

		/* execute query */
		success = SendQuery(query_buf->data);

		if (!success && die_on_error)
			successResult = EXIT_USER;
		else if (pset.db == NULL)
			successResult = EXIT_BADCONN;
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

	pset.cur_cmd_source = prev_cmd_source;
	pset.cur_cmd_interactive = prev_cmd_interactive;
	pset.lineno = prev_lineno;

	return successResult;
}	/* MainLoop() */


/*
 * psqlscan.c is #include'd here instead of being compiled on its own.
 * This is because we need postgres_fe.h to be read before any system
 * include files, else things tend to break on platforms that have
 * multiple infrastructures for stdio.h and so on.	flex is absolutely
 * uncooperative about that, so we can't compile psqlscan.c on its own.
 */
#include "psqlscan.c"
