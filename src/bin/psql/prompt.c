/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/prompt.c,v 1.30 2003/10/04 01:04:46 petere Exp $
 */
#include "postgres_fe.h"
#include "prompt.h"

#include "libpq-fe.h"

#include "settings.h"
#include "common.h"
#include "variables.h"

#ifdef WIN32
#include <io.h>
#include <win32.h>
#endif

#ifdef HAVE_UNIX_SOCKETS
#include <unistd.h>
#include <netdb.h>
#endif

/*--------------------------
 * get_prompt
 *
 * Returns a statically allocated prompt made by interpolating certain
 * tcsh style escape sequences into pset.vars "PROMPT1|2|3".
 * (might not be completely multibyte safe)
 *
 * Defined interpolations are:
 * %M - database server "hostname.domainname", "[local]" for AF_UNIX
 *		sockets, "[local:/dir/name]" if not default
 * %m - like %M, but hostname only (before first dot), or always "[local]"
 * %> - database server port number
 * %n - database user name
 * %/ - current database
 * %~ - like %/ but "~" when database name equals user name
 * %# - "#" if superuser, ">" otherwise
 * %R - in prompt1 normally =, or ^ if single line mode,
 *			or a ! if session is not connected to a database;
 *		in prompt2 -, *, ', or ";
 *		in prompt3 nothing
 * %x - transaction status: empty, *, !, ? (unknown or no connection)
 * %? - the error code of the last query (not yet implemented)
 * %% - a percent sign
 *
 * %[0-9]		   - the character with the given decimal code
 * %0[0-7]		   - the character with the given octal code
 * %0x[0-9A-Fa-f]  - the character with the given hexadecimal code
 *
 * %`command`	   - The result of executing command in /bin/sh with trailing
 *					 newline stripped.
 * %:name:		   - The value of the psql variable 'name'
 * (those will not be rescanned for more escape sequences!)
 *
 * If the application-wide prompts became NULL somehow, the returned string
 * will be empty (not NULL!).
 *--------------------------
 */

char *
get_prompt(promptStatus_t status)
{
#define MAX_PROMPT_SIZE 256
	static char destination[MAX_PROMPT_SIZE + 1];
	char		buf[MAX_PROMPT_SIZE + 1];
	bool		esc = false;
	const char *p;
	const char *prompt_string = "? ";
	const char *prompt_name = NULL;

	switch (status)
	{
		case PROMPT_READY:
			prompt_name = "PROMPT1";
			break;

		case PROMPT_CONTINUE:
		case PROMPT_SINGLEQUOTE:
		case PROMPT_DOUBLEQUOTE:
		case PROMPT_COMMENT:
		case PROMPT_PAREN:
			prompt_name = "PROMPT2";
			break;

		case PROMPT_COPY:
			prompt_name = "PROMPT3";
			break;
	}

	if (prompt_name)
		prompt_string = GetVariable(pset.vars, prompt_name);

	destination[0] = '\0';

	for (p = prompt_string;
		 p && *p && strlen(destination) < MAX_PROMPT_SIZE;
		 p++)
	{
		memset(buf, 0, MAX_PROMPT_SIZE + 1);
		if (esc)
		{
			switch (*p)
			{
					/* Current database */
				case '/':
					if (pset.db)
						strncpy(buf, PQdb(pset.db), MAX_PROMPT_SIZE);
					break;
				case '~':
					if (pset.db)
					{
						const char *var;

						if (strcmp(PQdb(pset.db), PQuser(pset.db)) == 0 ||
							((var = getenv("PGDATABASE")) && strcmp(var, PQdb(pset.db)) == 0))
							strcpy(buf, "~");
						else
							strncpy(buf, PQdb(pset.db), MAX_PROMPT_SIZE);
					}
					break;

					/* DB server hostname (long/short) */
				case 'M':
				case 'm':
					if (pset.db)
					{
						const char *host = PQhost(pset.db);

						/* INET socket */
						if (host && host[0] && !is_absolute_path(host))
						{
							strncpy(buf, host, MAX_PROMPT_SIZE);
							if (*p == 'm')
								buf[strcspn(buf, ".")] = '\0';
						}
#ifdef HAVE_UNIX_SOCKETS
						/* UNIX socket */
						else
						{
							if (!host
								|| strcmp(host, DEFAULT_PGSOCKET_DIR) == 0
								|| *p == 'm')
								strncpy(buf, "[local]", MAX_PROMPT_SIZE);
							else
								snprintf(buf, MAX_PROMPT_SIZE, "[local:%s]", host);
						}
#endif
					}
					break;
					/* DB server port number */
				case '>':
					if (pset.db && PQport(pset.db))
						strncpy(buf, PQport(pset.db), MAX_PROMPT_SIZE);
					break;
					/* DB server user name */
				case 'n':
					if (pset.db)
						strncpy(buf, session_username(), MAX_PROMPT_SIZE);
					break;

				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					*buf = parse_char((char **) &p);
					break;

				case 'R':
					switch (status)
					{
						case PROMPT_READY:
							if (!pset.db)
								buf[0] = '!';
							else if (!GetVariableBool(pset.vars, "SINGLELINE"))
								buf[0] = '=';
							else
								buf[0] = '^';
							break;
						case PROMPT_CONTINUE:
							buf[0] = '-';
							break;
						case PROMPT_SINGLEQUOTE:
							buf[0] = '\'';
							break;
						case PROMPT_DOUBLEQUOTE:
							buf[0] = '"';
							break;
						case PROMPT_COMMENT:
							buf[0] = '*';
							break;
						case PROMPT_PAREN:
							buf[0] = '(';
							break;
						default:
							buf[0] = '\0';
							break;
					}
					break;

				case 'x':
					if (!pset.db)
						buf[0] = '?';
					else
						switch (PQtransactionStatus(pset.db))
						{
							case PQTRANS_IDLE:
								buf[0] = '\0';
								break;
							case PQTRANS_ACTIVE:
							case PQTRANS_INTRANS:
								buf[0] = '*';
								break;
							case PQTRANS_INERROR:
								buf[0] = '!';
								break;
							default:
								buf[0] = '?';
								break;
						}
					break;

				case '?':
					/* not here yet */
					break;

				case '#':
					if (is_superuser())
						buf[0] = '#';
					else
						buf[0] = '>';
					break;

					/* execute command */
				case '`':
					{
						FILE	   *fd = NULL;
						char	   *file = strdup(p + 1);
						int			cmdend;

						cmdend = strcspn(file, "`");
						file[cmdend] = '\0';
						if (file)
							fd = popen(file, "r");
						if (fd)
						{
							fgets(buf, MAX_PROMPT_SIZE - 1, fd);
							pclose(fd);
						}
						if (strlen(buf) > 0 && buf[strlen(buf) - 1] == '\n')
							buf[strlen(buf) - 1] = '\0';
						free(file);
						p += cmdend + 1;
						break;
					}

					/* interpolate variable */
				case ':':
					{
						char	   *name;
						const char *val;
						int			nameend;

						name = strdup(p + 1);
						nameend = strcspn(name, ":");
						name[nameend] = '\0';
						val = GetVariable(pset.vars, name);
						if (val)
							strncpy(buf, val, MAX_PROMPT_SIZE);
						free(name);
						p += nameend + 1;
						break;
					}


				default:
					buf[0] = *p;
					buf[1] = '\0';

			}
			esc = false;
		}
		else if (*p == '%')
			esc = true;
		else
		{
			buf[0] = *p;
			buf[1] = '\0';
			esc = false;
		}

		if (!esc)
			strncat(destination, buf, MAX_PROMPT_SIZE - strlen(destination));
	}

	destination[MAX_PROMPT_SIZE] = '\0';
	return destination;
}
