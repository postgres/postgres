/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2013, PostgreSQL Global Development Group
 *
 * src/bin/psql/prompt.c
 */
#include "postgres_fe.h"

#ifdef WIN32
#include <io.h>
#include <win32.h>
#endif

#ifdef HAVE_UNIX_SOCKETS
#include <unistd.h>
#include <netdb.h>
#endif

#include "common.h"
#include "input.h"
#include "prompt.h"
#include "settings.h"


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
 * %[ ... %]	   - tell readline that the contained text is invisible
 *
 * If the application-wide prompts become NULL somehow, the returned string
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

	switch (status)
	{
		case PROMPT_READY:
			prompt_string = pset.prompt1;
			break;

		case PROMPT_CONTINUE:
		case PROMPT_SINGLEQUOTE:
		case PROMPT_DOUBLEQUOTE:
		case PROMPT_DOLLARQUOTE:
		case PROMPT_COMMENT:
		case PROMPT_PAREN:
			prompt_string = pset.prompt2;
			break;

		case PROMPT_COPY:
			prompt_string = pset.prompt3;
			break;
	}

	destination[0] = '\0';

	for (p = prompt_string;
		 *p && strlen(destination) < sizeof(destination) - 1;
		 p++)
	{
		memset(buf, 0, sizeof(buf));
		if (esc)
		{
			switch (*p)
			{
					/* Current database */
				case '/':
					if (pset.db)
						strlcpy(buf, PQdb(pset.db), sizeof(buf));
					break;
				case '~':
					if (pset.db)
					{
						const char *var;

						if (strcmp(PQdb(pset.db), PQuser(pset.db)) == 0 ||
							((var = getenv("PGDATABASE")) && strcmp(var, PQdb(pset.db)) == 0))
							strlcpy(buf, "~", sizeof(buf));
						else
							strlcpy(buf, PQdb(pset.db), sizeof(buf));
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
							strlcpy(buf, host, sizeof(buf));
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
								strlcpy(buf, "[local]", sizeof(buf));
							else
								snprintf(buf, sizeof(buf), "[local:%s]", host);
						}
#endif
					}
					break;
					/* DB server port number */
				case '>':
					if (pset.db && PQport(pset.db))
						strlcpy(buf, PQport(pset.db), sizeof(buf));
					break;
					/* DB server user name */
				case 'n':
					if (pset.db)
						strlcpy(buf, session_username(), sizeof(buf));
					break;

				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					*buf = (char) strtol(p, (char **) &p, 8);
					--p;
					break;
				case 'R':
					switch (status)
					{
						case PROMPT_READY:
							if (!pset.db)
								buf[0] = '!';
							else if (!pset.singleline)
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
						case PROMPT_DOLLARQUOTE:
							buf[0] = '$';
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
						FILE	   *fd;
						char	   *file = pg_strdup(p + 1);
						int			cmdend;

						cmdend = strcspn(file, "`");
						file[cmdend] = '\0';
						fd = popen(file, "r");
						if (fd)
						{
							if (fgets(buf, sizeof(buf), fd) == NULL)
								buf[0] = '\0';
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

						name = pg_strdup(p + 1);
						nameend = strcspn(name, ":");
						name[nameend] = '\0';
						val = GetVariable(pset.vars, name);
						if (val)
							strlcpy(buf, val, sizeof(buf));
						free(name);
						p += nameend + 1;
						break;
					}

				case '[':
				case ']':
#if defined(USE_READLINE) && defined(RL_PROMPT_START_IGNORE)

					/*
					 * readline >=4.0 undocumented feature: non-printing
					 * characters in prompt strings must be marked as such, in
					 * order to properly display the line during editing.
					 */
					buf[0] = (*p == '[') ? RL_PROMPT_START_IGNORE : RL_PROMPT_END_IGNORE;
					buf[1] = '\0';
#endif   /* USE_READLINE */
					break;

				default:
					buf[0] = *p;
					buf[1] = '\0';
					break;

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
			strlcat(destination, buf, sizeof(destination));
	}

	return destination;
}
