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



/*
 * Main processing loop for reading lines of input
 *	and sending them to the backend.
 *
 * This loop is re-entrant. May be called by \i command
 *	which reads input from a file.
 *
 * FIXME: rewrite this whole thing with flex
 */
int
MainLoop(FILE *source, int encoding)
{
	PQExpBuffer query_buf;		/* buffer for query being accumulated */
    PQExpBuffer previous_buf;   /* if there isn't anything in the new buffer
                                   yet, use this one for \e, etc. */
	char	   *line;			/* current line of input */
	int			len;			/* length of the line */
	int			successResult = EXIT_SUCCESS;
	backslashResult slashCmdStatus;

	bool		success;
	char		in_quote;		/* == 0 for no in_quote */
	bool		was_bslash;		/* backslash */
	bool        xcomment;		/* in extended comment */
	int			paren_level;
	unsigned int query_start;
    int         count_eof;
    const char *var;

	int			i,
				prevlen,
				thislen;

	/* Save the prior command source */
	FILE	   *prev_cmd_source;
	bool		prev_cmd_interactive;

	bool		die_on_error;


	/* Save old settings */
	prev_cmd_source = pset.cur_cmd_source;
	prev_cmd_interactive = pset.cur_cmd_interactive;

	/* Establish new source */
	pset.cur_cmd_source = source;
	pset.cur_cmd_interactive = ((source == stdin) && !pset.notty);


	query_buf = createPQExpBuffer();
    previous_buf = createPQExpBuffer();
	if (!query_buf || !previous_buf)
	{
		perror("createPQExpBuffer");
		exit(EXIT_FAILURE);
	}

	xcomment = false;
	in_quote = 0;
	paren_level = 0;
	slashCmdStatus = CMD_UNKNOWN;		/* set default */


	/* main loop to get queries and execute them */
	while (1)
	{
		if (slashCmdStatus == CMD_NEWEDIT)
		{
			/*
			 * just returned from editing the line? then just copy to the
			 * input buffer
			 */
			line = xstrdup(query_buf->data);
			resetPQExpBuffer(query_buf);
			/* reset parsing state since we are rescanning whole line */
			xcomment = false;
			in_quote = 0;
			paren_level = 0;
            slashCmdStatus = CMD_UNKNOWN;
		}
		else
		{
            fflush(stdout);
			/*
			 * otherwise, set interactive prompt if necessary and get
			 * another line
			 */
			if (pset.cur_cmd_interactive)
			{
				int			prompt_status;

				if (in_quote && in_quote == '\'')
					prompt_status = PROMPT_SINGLEQUOTE;
				else if (in_quote && in_quote == '"')
					prompt_status = PROMPT_DOUBLEQUOTE;
				else if (xcomment)
					prompt_status = PROMPT_COMMENT;
				else if (query_buf->len > 0)
					prompt_status = PROMPT_CONTINUE;
				else
					prompt_status = PROMPT_READY;

				line = gets_interactive(get_prompt(prompt_status));
			}
			else
				line = gets_fromFile(source);
		}


		/* Setting this will not have effect until next line. */
		die_on_error = GetVariableBool(pset.vars, "EXIT_ON_ERROR");

		/*
		 * query_buf holds query already accumulated.  line is the
		 * malloc'd new line of input (note it must be freed before
		 * looping around!) query_start is the next command start location
		 * within the line.
		 */

		/* No more input.  Time to quit, or \i done */
		if (line == NULL)
		{
			if (pset.cur_cmd_interactive)
            {
                bool getout = true;

                /* This tries to mimic bash's IGNOREEOF feature. */
                const char * val = GetVariable(pset.vars, "IGNOREEOF");
                if (val)
                {
                    long int maxeof;
                    char * endptr;

                    if (*val == '\0')
                        maxeof = 10;
                    else
                    {
                        maxeof = strtol(val, &endptr, 0);
                        if (*endptr != '\0') /* string not valid as a number */
                            maxeof = 10;
                    }

                    if (count_eof++ != maxeof)
                        getout = false; /* not quite there yet */
                }

                if (getout)
                {
                    putc('\n', stdout); /* just newline */
                    break;
                }
                else
                {
                    if (!QUIET())
                        printf("Use \"\\q\" to leave %s.\n", pset.progname);
                    continue;
                }
            }
            else /* not interactive */
                break;
		}
        else
            count_eof = 0;

		/* strip trailing backslashes, they don't have a clear meaning */
		while (1)
		{
			char	   *cp = strrchr(line, '\\');

			if (cp && (*(cp + 1) == '\0'))
				*cp = '\0';
			else
				break;
		}

		/* nothing left on line? then ignore */
		if (line[0] == '\0')
		{
			free(line);
			continue;
		}

		/* echo back if flag is set */
        var = GetVariable(pset.vars, "ECHO");
        if (var && strcmp(var, "full")==0)
            puts(line);
	fflush(stdout);

		len = strlen(line);
		query_start = 0;

		/*
		 * Parse line, looking for command separators.
		 *
		 * The current character is at line[i], the prior character at line[i
		 * - prevlen], the next character at line[i + thislen].
		 */
#define ADVANCE_1 (prevlen = thislen, i += thislen, thislen = PQmblen(line+i, encoding))

		success = true;
		for (i = 0, prevlen = 0, thislen = (len > 0) ? PQmblen(line, encoding) : 0;
             i < len;
             ADVANCE_1)
		{
			/* was the previous character a backslash? */
			was_bslash = (i > 0 && line[i - prevlen] == '\\');

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
			else if (xcomment)
			{
				if (line[i] == '*' && line[i + thislen] == '/')
				{
					xcomment = false;
					ADVANCE_1;
				}
			}

			/* start of extended comment? */
			else if (line[i] == '/' && line[i + thislen] == '*')
			{
				xcomment = true;
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

            /* colon -> substitute variable */
            /* we need to be on the watch for the '::' operator */
            else if (line[i] == ':' && !was_bslash &&
                     strspn(line+i+thislen, VALID_VARIABLE_CHARS)>0
                )
            {
				size_t		in_length,
							out_length;
				const char *value;
				char	   *new;
                char       after;		/* the character after the variable name
                                           will be temporarily overwritten */

				in_length = strspn(&line[i + thislen], VALID_VARIABLE_CHARS);
				after = line[i + thislen + in_length];
				line[i + thislen + in_length] = '\0';

                /* if the variable doesn't exist we'll leave the string as is */
				value = GetVariable(pset.vars, &line[i + thislen]);
                if (value)
                {
                    out_length = strlen(value);

                    new = malloc(len + out_length - (1 + in_length) + 1);
                    if (!new)
                    {
                        perror("malloc");
                        exit(EXIT_FAILURE);
                    }

                    sprintf(new, "%.*s%s%c", i, line, value, after);
                    if (after)
                        strcat(new, line + i + 1 + in_length + 1);

                    free(line);
                    line = new;
                    len = strlen(new);
                    continue; /* reparse the just substituted */
                }
                else
                {
                    /* restore overwritten character */
                    line[i + thislen + in_length] = after;
                    /* move on ... */
                }
            }

			/* semicolon? then send query */
			else if (line[i] == ';' && !was_bslash && !paren_level)
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
				success = SendQuery(query_buf->data);
                slashCmdStatus = success ? CMD_SEND : CMD_ERROR;

                resetPQExpBuffer(previous_buf);
                appendPQExpBufferStr(previous_buf, query_buf->data);
                resetPQExpBuffer(query_buf);
				query_start = i + thislen;
			}

            /* if you have a burning need to send a semicolon or colon to
               the backend ... */
            else if (was_bslash && (line[i] == ';' || line[i] == ':'))
            {
                /* remove the backslash */
                memmove(line + i - prevlen, line + i, len - i + 1);
                len--;
            }

			/* backslash command */
			else if (was_bslash)
			{
				const char *end_of_cmd = NULL;

				paren_level = 0;
				line[i - prevlen] = '\0';		/* overwrites backslash */

				/* is there anything else on the line for the command? */
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
                slashCmdStatus = HandleSlashCmds(&line[i], 
                                                 query_buf->len>0 ? query_buf : previous_buf,
                                                 &end_of_cmd, encoding);

				success = slashCmdStatus != CMD_ERROR;

                if ((slashCmdStatus == CMD_SEND || slashCmdStatus == CMD_NEWEDIT) &&
                    query_buf->len == 0) {
                    /* copy previous buffer to current for for handling */
                    appendPQExpBufferStr(query_buf, previous_buf->data);
                }

				if (slashCmdStatus == CMD_SEND)
				{
					success = SendQuery(query_buf->data);
					query_start = i + thislen;

                    resetPQExpBuffer(previous_buf);
                    appendPQExpBufferStr(previous_buf, query_buf->data);
                    resetPQExpBuffer(query_buf);
				}

				/* process anything left after the backslash command */
                i += end_of_cmd - &line[i];
                query_start = i;
			}


            /* stop the script after error */
			if (!success && die_on_error)
				break;

		} /* for (line) */


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
		if (query_buf->data[0] != '\0' && GetVariableBool(pset.vars, "SINGLELINE"))
		{
			success = SendQuery(query_buf->data);
            slashCmdStatus = success ? CMD_SEND : CMD_ERROR;
            resetPQExpBuffer(previous_buf);
            appendPQExpBufferStr(previous_buf, query_buf->data);
            resetPQExpBuffer(query_buf);
		}


		if (!success && die_on_error && !pset.cur_cmd_interactive)
		{
			successResult = EXIT_USER;
			break;
		}


		/* Have we lost the db connection? */
		if (pset.db == NULL && !pset.cur_cmd_interactive)
		{
			successResult = EXIT_BADCONN;
			break;
		}
	} /* while !endofprogram */

	destroyPQExpBuffer(query_buf);
	destroyPQExpBuffer(previous_buf);

	pset.cur_cmd_source = prev_cmd_source;
	pset.cur_cmd_interactive = prev_cmd_interactive;

	return successResult;
}	/* MainLoop() */
