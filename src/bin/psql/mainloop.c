#include <config.h>
#include <c.h>
#include "mainloop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pqexpbuffer.h>

#include "settings.h"
#include "prompt.h"
#include "input.h"
#include "common.h"
#include "command.h"



/* MainLoop()
 * Main processing loop for reading lines of input
 *	and sending them to the backend.
 *
 * This loop is re-entrant. May be called by \i command
 *	which reads input from a file.
 */
int
MainLoop(PsqlSettings *pset, FILE *source)
{
	PQExpBuffer query_buf;		/* buffer for query being accumulated */
	char	   *line;			/* current line of input */
	char	   *xcomment;		/* start of extended comment */
	int			len;			/* length of the line */
	int			successResult = EXIT_SUCCESS;
	backslashResult slashCmdStatus;

	bool		eof = false;	/* end of our command input? */
	bool		success;
	char		in_quote;		/* == 0 for no in_quote */
	bool		was_bslash;		/* backslash */
	int			paren_level;
	unsigned int query_start;

	int			i,
				prevlen,
				thislen;

	/* Save the prior command source */
	FILE	   *prev_cmd_source;
	bool		prev_cmd_interactive;

	bool		die_on_error;
	const char *interpol_char;


	/* Save old settings */
	prev_cmd_source = pset->cur_cmd_source;
	prev_cmd_interactive = pset->cur_cmd_interactive;

	/* Establish new source */
	pset->cur_cmd_source = source;
	pset->cur_cmd_interactive = ((source == stdin) && !pset->notty);


	query_buf = createPQExpBuffer();
	if (!query_buf)
	{
		perror("createPQExpBuffer");
		exit(EXIT_FAILURE);
	}

	xcomment = NULL;
	in_quote = 0;
	paren_level = 0;
	slashCmdStatus = CMD_UNKNOWN;		/* set default */


	/* main loop to get queries and execute them */
	while (!eof)
	{
		if (slashCmdStatus == CMD_NEWEDIT)
		{

			/*
			 * just returned from editing the line? then just copy to the
			 * input buffer
			 */
			line = strdup(query_buf->data);
			resetPQExpBuffer(query_buf);
			/* reset parsing state since we are rescanning whole query */
			xcomment = NULL;
			in_quote = 0;
			paren_level = 0;
		}
		else
		{

			/*
			 * otherwise, set interactive prompt if necessary and get
			 * another line
			 */
			if (pset->cur_cmd_interactive)
			{
				int			prompt_status;

				if (in_quote && in_quote == '\'')
					prompt_status = PROMPT_SINGLEQUOTE;
				else if (in_quote && in_quote == '"')
					prompt_status = PROMPT_DOUBLEQUOTE;
				else if (xcomment != NULL)
					prompt_status = PROMPT_COMMENT;
				else if (query_buf->len > 0)
					prompt_status = PROMPT_CONTINUE;
				else
					prompt_status = PROMPT_READY;

				line = gets_interactive(get_prompt(pset, prompt_status));
			}
			else
				line = gets_fromFile(source);
		}


		/* Setting these will not have effect until next line */
		die_on_error = GetVariableBool(pset->vars, "die_on_error");
		interpol_char = GetVariable(pset->vars, "sql_interpol");;


		/*
		 * query_buf holds query already accumulated.  line is the
		 * malloc'd new line of input (note it must be freed before
		 * looping around!) query_start is the next command start location
		 * within the line.
		 */

		/* No more input.  Time to quit, or \i done */
		if (line == NULL)
		{
			if (GetVariableBool(pset->vars, "echo") && !GetVariableBool(pset->vars, "quiet"))
				puts("EOF");
			else if (pset->cur_cmd_interactive)
				puts(""); /* just newline */

			eof = true;
			continue;
		}

		/* not currently inside an extended comment? */
		if (xcomment)
			xcomment = line;


		/* strip trailing backslashes, they don't have a clear meaning */
		while (1)
		{
			char	   *cp = strrchr(line, '\\');

			if (cp && (*(cp + 1) == '\0'))
				*cp = '\0';
			else
				break;
		}


		/* echo back if input is from file and flag is set */
		if (!pset->cur_cmd_interactive && GetVariableBool(pset->vars, "echo"))
			fprintf(stderr, "%s\n", line);


		/* interpolate variables into SQL */
		len = strlen(line);
		thislen = PQmblen(line);

		for (i = 0; line[i]; i += (thislen = PQmblen(&line[i])))
		{
			if (interpol_char && interpol_char[0] != '\0' && interpol_char[0] == line[i])
			{
				size_t		in_length,
							out_length;
				const char *value;
				char	   *new;
				bool		closer;		/* did we have a closing delimiter
										 * or just an end of line? */

				in_length = strcspn(&line[i + thislen], interpol_char);
				closer = line[i + thislen + in_length] == line[i];
				line[i + thislen + in_length] = '\0';
				value = interpolate_var(&line[i + thislen], pset);
				out_length = strlen(value);

				new = malloc(len + out_length - (in_length + (closer ? 2 : 1)) + 1);
				if (!new)
				{
					perror("malloc");
					exit(EXIT_FAILURE);
				}

				new[0] = '\0';
				strncat(new, line, i);
				strcat(new, value);
				if (closer)
					strcat(new, line + i + 2 + in_length);

				free(line);
				line = new;
				i += out_length;
			}
		}

		/* nothing left on line? then ignore */
		if (line[0] == '\0')
		{
			free(line);
			continue;
		}

		slashCmdStatus = CMD_UNKNOWN;

		len = strlen(line);
		query_start = 0;

		/*
		 * Parse line, looking for command separators.
		 *
		 * The current character is at line[i], the prior character at line[i
		 * - prevlen], the next character at line[i + thislen].
		 */
		prevlen = 0;
		thislen = (len > 0) ? PQmblen(line) : 0;

#define ADVANCE_1  (prevlen = thislen, i += thislen, thislen = PQmblen(line+i))

		success = true;
		for (i = 0; i < len; ADVANCE_1)
		{
			if (!success && die_on_error)
				break;


			/* was the previous character a backslash? */
			if (i > 0 && line[i - prevlen] == '\\')
				was_bslash = true;
			else
				was_bslash = false;


			/* in quote? */
			if (in_quote)
			{
				/* end of quote */
				if (line[i] == in_quote && !was_bslash)
					in_quote = '\0';
			}

			/* start of quote */
			else if (line[i] == '\'' || line[i] == '"')
				in_quote = line[i];

			/* in extended comment? */
			else if (xcomment != NULL)
			{
				if (line[i] == '*' && line[i + thislen] == '/')
				{
					xcomment = NULL;
					ADVANCE_1;
				}
			}

			/* start of extended comment? */
			else if (line[i] == '/' && line[i + thislen] == '*')
			{
				xcomment = &line[i];
				ADVANCE_1;
			}

			/* single-line comment? truncate line */
			else if ((line[i] == '-' && line[i + thislen] == '-') ||
					 (line[i] == '/' && line[i + thislen] == '/'))
			{
				line[i] = '\0'; /* remove comment */
				break;
			}

			/* count nested parentheses */
			else if (line[i] == '(')
				paren_level++;

			else if (line[i] == ')' && paren_level > 0)
				paren_level--;

			/* semicolon? then send query */
			else if (line[i] == ';' && !was_bslash && paren_level == 0)
			{
				line[i] = '\0';
				/* is there anything else on the line? */
				if (line[query_start + strspn(line + query_start, " \t")] != '\0')
				{

					/*
					 * insert a cosmetic newline, if this is not the first
					 * line in the buffer
					 */
					if (query_buf->len > 0)
						appendPQExpBufferChar(query_buf, '\n');
					/* append the line to the query buffer */
					appendPQExpBufferStr(query_buf, line + query_start);
				}

				/* execute query */
				success = SendQuery(pset, query_buf->data);

				resetPQExpBuffer(query_buf);
				query_start = i + thislen;
			}

			/* backslash command */
			else if (was_bslash)
			{
				const char *end_of_cmd = NULL;

				line[i - prevlen] = '\0';		/* overwrites backslash */

				/* is there anything else on the line? */
				if (line[query_start + strspn(line + query_start, " \t")] != '\0')
				{

					/*
					 * insert a cosmetic newline, if this is not the first
					 * line in the buffer
					 */
					if (query_buf->len > 0)
						appendPQExpBufferChar(query_buf, '\n');
					/* append the line to the query buffer */
					appendPQExpBufferStr(query_buf, line + query_start);
				}

				/* handle backslash command */

				slashCmdStatus = HandleSlashCmds(pset, &line[i], query_buf, &end_of_cmd);

				success = slashCmdStatus != CMD_ERROR;

				if (slashCmdStatus == CMD_SEND)
				{
					success = SendQuery(pset, query_buf->data);
					resetPQExpBuffer(query_buf);
					query_start = i + thislen;
				}

				/* is there anything left after the backslash command? */
				if (end_of_cmd)
				{
					i += end_of_cmd - &line[i];
					query_start = i;
				}
				else
					break;
			}
		}


		if (!success && die_on_error && !pset->cur_cmd_interactive)
		{
			successResult = EXIT_USER;
			break;
		}


		if (slashCmdStatus == CMD_TERMINATE)
		{
			successResult = EXIT_SUCCESS;
			break;
		}


		/* Put the rest of the line in the query buffer. */
		if (line[query_start + strspn(line + query_start, " \t")] != '\0')
		{
			if (query_buf->len > 0)
				appendPQExpBufferChar(query_buf, '\n');
			appendPQExpBufferStr(query_buf, line + query_start);
		}

		free(line);


		/* In single line mode, send off the query if any */
		if (query_buf->data[0] != '\0' && GetVariableBool(pset->vars, "singleline"))
		{
			success = SendQuery(pset, query_buf->data);
			resetPQExpBuffer(query_buf);
		}


		/* Have we lost the db connection? */
		if (pset->db == NULL && !pset->cur_cmd_interactive)
		{
			successResult = EXIT_BADCONN;
			break;
		}
	}							/* while */

	destroyPQExpBuffer(query_buf);

	pset->cur_cmd_source = prev_cmd_source;
	pset->cur_cmd_interactive = prev_cmd_interactive;

	return successResult;
}	/* MainLoop() */
