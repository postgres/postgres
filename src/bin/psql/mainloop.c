/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/mainloop.c,v 1.68 2005/10/15 02:49:40 momjian Exp $
 */
#include "postgres_fe.h"
#include "mainloop.h"

#include "pqexpbuffer.h"

#include "command.h"
#include "common.h"
#include "input.h"
#include "prompt.h"
#include "psqlscan.h"
#include "settings.h"

#ifndef WIN32
#include <setjmp.h>
sigjmp_buf	main_loop_jmp;
#endif


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
	PQExpBuffer query_buf;		/* buffer for query being accumulated */
	PQExpBuffer previous_buf;	/* if there isn't anything in the new buffer
								 * yet, use this one for \e, etc. */
	char	   *line;			/* current line of input */
	int			added_nl_pos;
	bool		success;
	volatile int successResult = EXIT_SUCCESS;
	volatile backslashResult slashCmdStatus = CMD_UNKNOWN;
	volatile promptStatus_t prompt_status = PROMPT_READY;
	volatile int count_eof = 0;
	volatile bool die_on_error = false;

	/* Save the prior command source */
	FILE	   *prev_cmd_source;
	bool		prev_cmd_interactive;
	unsigned int prev_lineno;

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
	if (!query_buf || !previous_buf)
	{
		psql_error("out of memory\n");
		exit(EXIT_FAILURE);
	}

	/* main loop to get queries and execute them */
	while (successResult == EXIT_SUCCESS)
	{
		/*
		 * Welcome code for Control-C
		 */
		if (cancel_pressed)
		{
			if (!pset.cur_cmd_interactive)
			{
				/*
				 * You get here if you stopped a script with Ctrl-C and a
				 * query cancel was issued. In that case we don't do the
				 * longjmp, so the query routine can finish nicely.
				 */
				successResult = EXIT_USER;
				break;
			}

			cancel_pressed = false;
		}

#ifndef WIN32
		if (sigsetjmp(main_loop_jmp, 1) != 0)
		{
			/* got here with longjmp */

			/* reset parsing state */
			resetPQExpBuffer(query_buf);
			psql_scan_finish(scan_state);
			psql_scan_reset(scan_state);
			count_eof = 0;
			slashCmdStatus = CMD_UNKNOWN;
			prompt_status = PROMPT_READY;

			if (pset.cur_cmd_interactive)
				putc('\n', stdout);
			else
			{
				successResult = EXIT_USER;
				break;
			}
		}

		/*
		 * establish the control-C handler only after main_loop_jmp is ready
		 */
		pqsignal(SIGINT, handle_sigint);		/* control-C => cancel */
#else							/* WIN32 */
		setup_cancel_handler();
#endif

		fflush(stdout);

		if (slashCmdStatus == CMD_NEWEDIT)
		{
			/*
			 * just returned from editing the line? then just copy to the
			 * input buffer
			 */
			line = pg_strdup(query_buf->data);
			/* reset parsing state since we are rescanning whole line */
			resetPQExpBuffer(query_buf);
			psql_scan_reset(scan_state);
			slashCmdStatus = CMD_UNKNOWN;
			prompt_status = PROMPT_READY;
		}

		/*
		 * otherwise, get another line
		 */
		else if (pset.cur_cmd_interactive)
		{
			/* May need to reset prompt, eg after \r command */
			if (query_buf->len == 0)
				prompt_status = PROMPT_READY;
			line = gets_interactive(get_prompt(prompt_status));
		}
		else
			line = gets_fromFile(source);

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
					if (!QUIET())
						printf(_("Use \"\\q\" to leave %s.\n"), pset.progname);
					continue;
				}

				puts(QUIET() ? "" : "\\q");
			}
			break;
		}

		count_eof = 0;

		pset.lineno++;

		/* nothing left on line? then ignore */
		if (line[0] == '\0' && !psql_scan_in_quote(scan_state))
		{
			free(line);
			continue;
		}

		/* echo back if flag is set */
		if (!pset.cur_cmd_interactive &&
			VariableEquals(pset.vars, "ECHO", "all"))
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
		die_on_error = GetVariableBool(pset.vars, "ON_ERROR_STOP");

		/*
		 * Parse line, looking for command separators.
		 */
		psql_scan_setup(scan_state, line, strlen(line));
		success = true;

		while (success || !die_on_error)
		{
			PsqlScanResult scan_result;
			promptStatus_t prompt_tmp = prompt_status;

			scan_result = psql_scan(scan_state, query_buf, &prompt_tmp);
			prompt_status = prompt_tmp;

			/*
			 * Send command if semicolon found, or if end of line and we're in
			 * single-line mode.
			 */
			if (scan_result == PSCAN_SEMICOLON ||
				(scan_result == PSCAN_EOL &&
				 GetVariableBool(pset.vars, "SINGLELINE")))
			{
				/* execute query */
				success = SendQuery(query_buf->data);
				slashCmdStatus = success ? CMD_SEND : CMD_ERROR;

				resetPQExpBuffer(previous_buf);
				appendPQExpBufferStr(previous_buf, query_buf->data);
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
				 * line contains only a backslash command.
				 */
				if (query_buf->len == added_nl_pos)
					query_buf->data[--query_buf->len] = '\0';
				added_nl_pos = -1;

				slashCmdStatus = HandleSlashCmds(scan_state,
												 query_buf->len > 0 ?
												 query_buf : previous_buf);

				success = slashCmdStatus != CMD_ERROR;

				if ((slashCmdStatus == CMD_SEND || slashCmdStatus == CMD_NEWEDIT) &&
					query_buf->len == 0)
				{
					/* copy previous buffer to current for handling */
					appendPQExpBufferStr(query_buf, previous_buf->data);
				}

				if (slashCmdStatus == CMD_SEND)
				{
					success = SendQuery(query_buf->data);

					resetPQExpBuffer(previous_buf);
					appendPQExpBufferStr(previous_buf, query_buf->data);
					resetPQExpBuffer(query_buf);

					/* flush any paren nesting info after forced send */
					psql_scan_reset(scan_state);
				}

				if (slashCmdStatus == CMD_TERMINATE)
					break;
			}

			/* fall out of loop if lexer reached EOL */
			if (scan_result == PSCAN_INCOMPLETE ||
				scan_result == PSCAN_EOL)
				break;
		}

		psql_scan_finish(scan_state);
		free(line);

		if (slashCmdStatus == CMD_TERMINATE)
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
		success = SendQuery(query_buf->data);

		if (!success && die_on_error)
			successResult = EXIT_USER;
		else if (pset.db == NULL)
			successResult = EXIT_BADCONN;
	}

	/*
	 * Reset SIGINT handler because main_loop_jmp will be invalid as soon as
	 * we exit this routine.  If there is an outer MainLoop instance, it will
	 * re-enable ^C catching as soon as it gets back to the top of its loop
	 * and resets main_loop_jmp to point to itself.
	 */
#ifndef WIN32
	pqsignal(SIGINT, SIG_DFL);
#endif

	destroyPQExpBuffer(query_buf);
	destroyPQExpBuffer(previous_buf);

	psql_scan_destroy(scan_state);

	pset.cur_cmd_source = prev_cmd_source;
	pset.cur_cmd_interactive = prev_cmd_interactive;
	pset.lineno = prev_lineno;

	return successResult;
}	/* MainLoop() */
