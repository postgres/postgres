/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and possibly using ident).
 *
 *	$Id: hba.c,v 1.56 2001/07/30 14:50:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <errno.h>
#include <pwd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "postgres.h"

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/fd.h"


#define MAX_TOKEN 80
/* Maximum size of one token in the configuration file	*/

#define IDENT_USERNAME_MAX 512
 /* Max size of username ident server can return */

static List *hba_lines = NULL;	/* A list of lists: entry for every line,
								 * list of tokens on each line.
								 */

static List *ident_lines = NULL;/* A list of lists: entry for every line,
								 * list of tokens on each line.
								 */

/* Some standard C libraries, including GNU, have an isblank() function.
   Others, including Solaris, do not.  So we have our own.
*/
static bool
isblank(const char c)
{
	return c == ' ' || c == 0x09;/* tab */
}


/*
 *  Grab one token out of fp.  Tokens are strings of non-blank
 *  characters bounded by blank characters, beginning of line, and end
 *  of line.	Blank means space or tab.  Return the token as *buf.
 *  Leave file positioned to character immediately after the token or
 *  EOF, whichever comes first.  If no more tokens on line, return null
 *  string as *buf and position file to beginning of next line or EOF,
 *  whichever comes first.
 */
static void
next_token(FILE *fp, char *buf, const int bufsz)
{
	int			c;
	char	   *eb = buf + (bufsz - 1);

	/* Move over inital token-delimiting blanks */
	while (isblank(c = getc(fp)))
		;

	if (c != '\n')
	{
		/*
		 * build a token in buf of next characters up to EOF, eol, or
		 * blank.
		 */
		while (c != EOF && c != '\n' && !isblank(c))
		{
			if (buf < eb)
				*buf++ = c;
			c = getc(fp);
		}
		/*
		 * Put back the char right after the token (putting back EOF
		 * is ok)
		 */
		ungetc(c, fp);
	}
	*buf = '\0';
}


static void
read_to_eol(FILE *file)
{
	int			c;

	while ((c = getc(file)) != '\n' && c != EOF)
		;
}


/*
 *  Process the file line by line and create a list of list of tokens.
 */
static void
tokenize_file(FILE *file, List **lines)
{
	char		buf[MAX_TOKEN];
	List		*next_line = NIL;
	bool		comment_found = false;

	while (1)
	{
		next_token(file, buf, sizeof(buf));
		if (feof(file))
			break;

		/* trim off comment, even if inside a token */
		if (strstr(buf,"#") != NULL)
		{
			*strstr(buf,"#") = '\0';
			comment_found = true;
		}

		/* add token to list */
		if (buf[0] != '\0')
		{
			if (next_line == NIL)
			{
				/* make a new line List */
				next_line = lcons(pstrdup(buf), NIL);
				*lines = lappend(*lines, next_line);
			}
			else
				/* append token to line */
				next_line = lappend(next_line, pstrdup(buf));
		}
		else
			/* force a new List line */
			next_line = NIL;

		if (comment_found)
		{
			/* Skip the rest of the line */
			read_to_eol(file);
			next_line = NIL;
			comment_found = false;
		}
	}
}


/*
 * Free memory used by lines/tokens
 */
static void free_lines(List **lines)
{
	if (*lines)
	{
		List *line, *token;

		foreach(line, *lines)
		{
			foreach(token,lfirst(line))
				pfree(lfirst(token));
			freeList(lfirst(line));
		}
		freeList(*lines);
		*lines = NULL;
	}
}


/*
 *  Read from file FILE the rest of a host record, after the mask field,
 *  and return the interpretation of it as *userauth_p, auth_arg, and
 *  *error_p.
 */
static void
parse_hba_auth(List *line, UserAuth *userauth_p, char *auth_arg,
				bool *error_p)
{
	char		*token = NULL;

	if (!line)
		*error_p = true;
	else
	{
		/* Get authentication type token. */
		token = lfirst(line);
		if (strcmp(token, "trust") == 0)
			*userauth_p = uaTrust;
		else if (strcmp(token, "ident") == 0)
			*userauth_p = uaIdent;
		else if (strcmp(token, "password") == 0)
			*userauth_p = uaPassword;
		else if (strcmp(token, "krb4") == 0)
			*userauth_p = uaKrb4;
		else if (strcmp(token, "krb5") == 0)
			*userauth_p = uaKrb5;
		else if (strcmp(token, "reject") == 0)
			*userauth_p = uaReject;
		else if (strcmp(token, "crypt") == 0)
			*userauth_p = uaCrypt;
		else
			*error_p = true;
	}

	if (!*error_p)
	{
		/* Get the authentication argument token, if any */
		line = lnext(line);
		if (!line)
			auth_arg[0] = '\0';
		else
		{
			StrNCpy(auth_arg, token, MAX_AUTH_ARG - 1);
			/* If there is more on the line, it is an error */
			if (lnext(line))
				*error_p = true;
		}
	}
}


/*
 *  Process the non-comment lines in the config file.
 *
 *  See if it applies to a connection to a host with IP address "*raddr"
 *  to a database named "*database".	If so, return *found_p true
 *  and *userauth_p and *auth_arg as the values from the entry.
 *  If not, leave *found_p as it was.  If the record has a syntax error,
 *  return *error_p true, after issuing a message to stderr.	If no error,
 *  leave *error_p as it was.
 */
static void
parse_hba(List *line, hbaPort *port, bool *found_p, bool *error_p)
{
	char		*db;
	char		*token;

	Assert(line != NIL);
	token = lfirst(line);
	/* Check the record type. */
	if (strcmp(token, "local") == 0)
	{
		/* Get the database. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		db = lfirst(line);

		line = lnext(line);
		if (!line)
			goto hba_syntax;
		/* Read the rest of the line. */
		parse_hba_auth(line, &port->auth_method, port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;

		/*
		 * For now, disallow methods that need AF_INET sockets to work.
		 */
		if (!*error_p &&
			(port->auth_method == uaIdent ||
			 port->auth_method == uaKrb4 ||
			 port->auth_method == uaKrb5))
			goto hba_syntax;

		/*
		 * If this record isn't for our database, or this is the wrong
		 * sort of connection, ignore it.
		 */
		if ((strcmp(db, port->database) != 0 && strcmp(db, "all") != 0 &&
			 (strcmp(db, "sameuser") != 0 || strcmp(port->database, port->user) != 0)) ||
			port->raddr.sa.sa_family != AF_UNIX)
			return;
	}
	else if (strcmp(token, "host") == 0 || strcmp(token, "hostssl") == 0)
	{
		struct in_addr file_ip_addr, mask;

#ifdef USE_SSL
		/* If SSL, then check that we are on SSL */
		if (strcmp(token, "hostssl") == 0)
		{
			if (!port->ssl)
				return;

			/* Placeholder to require specific SSL level, perhaps? */
			/* Or a client certificate */

			/* Since we were on SSL, proceed as with normal 'host' mode */
		}
#else
		/* If not SSL, we don't support this */
		if (strcmp(token, "hostssl") == 0)
			goto hba_syntax;
#endif

		/* Get the database. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		db = lfirst(line);

		/* Read the IP address field. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		token = lfirst(line);

		/* Remember the IP address field and go get mask field. */
		if (!inet_aton(token, &file_ip_addr))
			goto hba_syntax;

		/* Read the mask field. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		token = lfirst(line);

		if (!inet_aton(token, &mask))
			goto hba_syntax;

		/*
		 * This is the record we're looking for.  Read the rest of the
		 * info from it.
		 */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		parse_hba_auth(line, &port->auth_method, port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;

		/*
		 * If this record isn't for our database, or this is the wrong
		 * sort of connection, ignore it.
		 */
		if ((strcmp(db, port->database) != 0 && strcmp(db, "all") != 0 &&
			 (strcmp(db, "sameuser") != 0 || strcmp(port->database, port->user) != 0)) ||
			port->raddr.sa.sa_family != AF_INET ||
			((file_ip_addr.s_addr ^ port->raddr.in.sin_addr.s_addr) & mask.s_addr) != 0x0000)
			return;
	}
	else
		goto hba_syntax;

	/* Success */
	*found_p = true;
	return;

hba_syntax:
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			 "parse_hba: invalid syntax in pg_hba.conf file\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	*error_p = true;
	return;
}


/*
 *  Process the hba file line by line.
 */
static bool
check_hba(hbaPort *port)
{
	List 	*line;
	bool	found_entry = false;
	bool	error = false;

	foreach (line, hba_lines)
	{
		parse_hba(lfirst(line), port, &found_entry, &error);
		if (found_entry || error)
			break;
	}

	if (!error)
	{
		/* If no matching entry was found, synthesize 'reject' entry. */
		if (!found_entry)
			port->auth_method = uaReject;
		return true;
	}
	else
		return false;
}


/*
 * Read the config file and create a List of Lists of tokens in the file.
 * If we find a file by the old name of the config file (pg_hba), we issue
 * an error message because it probably needs to be converted.	He didn't
 * follow directions and just installed his old hba file in the new database
 * system.
 */
static void
load_hba()
{

	int			fd,
				bufsize;
	FILE	   *file;			/* The config file we have to read */
	char	   *old_conf_file;

	if (hba_lines)
		free_lines(&hba_lines);
	/*
	 *	The name of old config file that better not exist.
	 *	Fail if config file by old name exists.
	 *	Put together the full pathname to the old config file.
	 */
	bufsize = (strlen(DataDir) + strlen(OLD_CONF_FILE) + 2) * sizeof(char);
	old_conf_file = (char *) palloc(bufsize);
	snprintf(old_conf_file, bufsize, "%s/%s", DataDir, OLD_CONF_FILE);

	if ((fd = open(old_conf_file, O_RDONLY | PG_BINARY, 0)) != -1)
	{
		/* Old config file exists.	Tell this guy he needs to upgrade. */
		close(fd);
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			"A file exists by the name used for host-based authentication "
			"in prior releases of Postgres (%s).  The name and format of "
			"the configuration file have changed, so this file should be "
			"converted.\n", old_conf_file);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
	else
	{
		char	   *conf_file;	/* The name of the config file we have to
								 * read */

		/* put together the full pathname to the config file */
		bufsize = (strlen(DataDir) + strlen(CONF_FILE) + 2) * sizeof(char);
		conf_file = (char *) palloc(bufsize);
		snprintf(conf_file, bufsize, "%s/%s", DataDir, CONF_FILE);

		file = AllocateFile(conf_file, "r");
		if (file == NULL)
		{
			/* The open of the config file failed.	*/
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					 "load_hba: Unable to open authentication config file \"%s\": %s\n",
					 conf_file, strerror(errno));
			fputs(PQerrormsg, stderr);
			pqdebug("%s", PQerrormsg);
		}
		else
		{
			tokenize_file(file, &hba_lines);
			FreeFile(file);
		}
		pfree(conf_file);
	}
	pfree(old_conf_file);
}


/*
 *  Take the line and compare it to the needed map, pg_user and ident_user.
 */
static void
parse_ident_usermap(List *line, const char *usermap_name, const char *pg_user,
				 const char *ident_user, bool *found_p, bool *error_p)
{
	char		*token;
	char		*file_map;
	char		*file_pguser;
	char		*file_ident_user;

	*error_p = false;
	*found_p = false;

	/* A token read from the file */
	Assert(line != NIL);
	token = lfirst(line);
	file_map = token;

	line = lnext(line);
	if (!line)
		goto ident_syntax;
	token = lfirst(line);
	if (token[0] != '\0')
	{
		file_ident_user = token;
		line = lnext(line);
		if (!line)
			goto ident_syntax;
		token = lfirst(line);
		if (token[0] != '\0')
		{
			file_pguser = token;
			if (strcmp(file_map, usermap_name) == 0 &&
				strcmp(file_pguser, pg_user) == 0 &&
				strcmp(file_ident_user, ident_user) == 0)
				*found_p = true;
		}
	}

	return;

ident_syntax:
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			 "parse_ident_usermap: invalid syntax in pg_ident.conf file\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	*error_p = true;
	return;
}


/*
 *  Process the ident usermap file line by line.
 */
static bool
check_ident_usermap(const char *usermap_name,
					const char *pg_user,
					const char *ident_user)
{
	List 	*line;
	bool	found_entry = false, error = false;

	if (usermap_name[0] == '\0')
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			"load_ident_usermap: hba configuration file does not "
			"have the usermap field filled in in the entry that pertains "
			"to this connection.  That field is essential for Ident-based "
			"authentication.\n");
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		found_entry = false;
	}
	else if (strcmp(usermap_name, "sameuser") == 0)
	{
		if (strcmp(pg_user, ident_user) == 0)
			found_entry = true;
		else
			found_entry = false;
	}
	else
	{
		foreach(line, ident_lines)
		{
			parse_ident_usermap(lfirst(line), usermap_name, pg_user,
								ident_user, &found_entry, &error);
			if (found_entry || error)
				break;
		}
	}
	return found_entry;
}


/*
 *  See if the user with ident username "ident_user" is allowed to act
 *  as Postgres user "pguser" according to usermap "usermap_name".   Look
 *  it up in the usermap file.
 *
 *  Special case: For usermap "sameuser", don't look in the usermap
 *  file.  That's an implied map where "pguser" must be identical to
 *  "ident_user" in order to be authorized.
 *
 *  Iff authorized, return *checks_out_p == true.
 */
static void
load_ident()
{
	FILE	   *file;		/* The map file we have to read */
	char	   *map_file;	/* The name of the map file we have to
							 * read */
	int			bufsize;

	if (ident_lines)
		free_lines(&ident_lines);

	/* put together the full pathname to the map file */
	bufsize = (strlen(DataDir) + strlen(USERMAP_FILE) + 2) * sizeof(char);
	map_file = (char *) palloc(bufsize);
	snprintf(map_file, bufsize, "%s/%s", DataDir, USERMAP_FILE);

	file = AllocateFile(map_file, PG_BINARY_R);
	if (file == NULL)
	{
		/* The open of the map file failed.  */
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			"load_ident_usermap: Unable to open usermap file \"%s\": %s\n",
			map_file, strerror(errno));
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
	else
	{
		tokenize_file(file, &ident_lines);
		FreeFile(file);
	}
	pfree(map_file);
}


/*
 *  Parse the string "*ident_response" as a response from a query to an Ident
 *  server.  If it's a normal response indicating a username, return
 *  *error_p == false and the username as *ident_user.  If it's anything
 *  else, return *error_p == true and *ident_user undefined.
 */
static bool
interpret_ident_response(char *ident_response,
						 char *ident_user)
{
	char	   *cursor = ident_response;/* Cursor into *ident_response */

	/*
	 * Ident's response, in the telnet tradition, should end in crlf
	 * (\r\n).
	 */
	if (strlen(ident_response) < 2)
		return false;
	else if (ident_response[strlen(ident_response) - 2] != '\r')
		return false;
	else
	{
		while (*cursor != ':' && *cursor != '\r')
			cursor++;			/* skip port field */

		if (*cursor != ':')
			return false;
		else
		{
			/* We're positioned to colon before response type field */
			char		response_type[80];
			int			i;		/* Index into *response_type */

			cursor++;			/* Go over colon */
			while (isblank(*cursor))
				cursor++;		/* skip blanks */
			i = 0;
			while (*cursor != ':' && *cursor != '\r' && !isblank(*cursor) &&
				   i < (int) (sizeof(response_type) - 1))
				response_type[i++] = *cursor++;
			response_type[i] = '\0';
			while (isblank(*cursor))
				cursor++;		/* skip blanks */
			if (strcmp(response_type, "USERID") != 0)
				return false;
			else
			{
				/*
				 * It's a USERID response.  Good.  "cursor" should be
				 * pointing to the colon that precedes the operating
				 * system type.
				 */
				if (*cursor != ':')
					return false;
				else
				{
					cursor++;	/* Go over colon */
					/* Skip over operating system field. */
					while (*cursor != ':' && *cursor != '\r')
						cursor++;
					if (*cursor != ':')
						return false;
					else
					{
						int			i;	/* Index into *ident_user */

						cursor++;		/* Go over colon */
						while (isblank(*cursor))
							cursor++;	/* skip blanks */
						/* Rest of line is username.  Copy it over. */
						i = 0;
						while (*cursor != '\r' && i < IDENT_USERNAME_MAX)
							ident_user[i++] = *cursor++;
						ident_user[i] = '\0';
						return true;
					}
				}
			}
		}
	}
}


/*
 *  Talk to the ident server on host "remote_ip_addr" and find out who
 *  owns the tcp connection from his port "remote_port" to port
 *  "local_port_addr" on host "local_ip_addr".  Return the username the
 *  ident server gives as "*ident_user".

 *  IP addresses and port numbers are in network byte order.

 *  But iff we're unable to get the information from ident, return
 *  false.
 */
static int
ident(const struct in_addr remote_ip_addr,
	  const struct in_addr local_ip_addr,
	  const ushort remote_port,
	  const ushort local_port,
	  char *ident_user)
{
	int			sock_fd,		/* File descriptor for socket on which we
								 * talk to Ident */
				rc;				/* Return code from a locally called
								 * function */
	bool ident_return;

	sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock_fd == -1)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			"Failed to create socket on which to talk to Ident server. "
			"socket() returned errno = %s (%d)\n", strerror(errno), errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		ident_return = false;
	}
	else
	{
		struct sockaddr_in ident_server;
		struct sockaddr_in la;

		/*
		 * Socket address of Ident server on the system from which client
		 * is attempting to connect to us.
		 */
		ident_server.sin_family = AF_INET;
		ident_server.sin_port = htons(IDENT_PORT);
		ident_server.sin_addr = remote_ip_addr;

		/*
		 * Bind to the address which the client originally contacted,
		 * otherwise the ident server won't be able to match up the right
		 * connection. This is necessary if the PostgreSQL server is
		 * running on an IP alias.
		 */
		memset(&la, 0, sizeof(la));
		la.sin_family = AF_INET;
		la.sin_addr = local_ip_addr;
		rc = bind(sock_fd, (struct sockaddr *) & la, sizeof(la));
		if (rc == 0)
		{
			rc = connect(sock_fd,
			   (struct sockaddr *) & ident_server, sizeof(ident_server));
		}
		if (rc != 0)
		{
			snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				"Unable to connect to Ident server on the host which is "
				"trying to connect to Postgres "
				"(IP address %s, Port %d). "
				"errno = %s (%d)\n",
				inet_ntoa(remote_ip_addr), IDENT_PORT, strerror(errno), errno);
			fputs(PQerrormsg, stderr);
			pqdebug("%s", PQerrormsg);
			ident_return = false;
		}
		else
		{
			char		ident_query[80];

			/* The query we send to the Ident server */
			snprintf(ident_query, 80, "%d,%d\n",
					 ntohs(remote_port), ntohs(local_port));
			rc = send(sock_fd, ident_query, strlen(ident_query), 0);
			if (rc < 0)
			{
				snprintf(PQerrormsg, PQERRORMSG_LENGTH,
					"Unable to send query to Ident server on the host which is "
					"trying to connect to Postgres (Host %s, Port %d),"
					"even though we successfully connected to it.  "
					"errno = %s (%d)\n",
					inet_ntoa(remote_ip_addr), IDENT_PORT, strerror(errno), errno);
				fputs(PQerrormsg, stderr);
				pqdebug("%s", PQerrormsg);
				ident_return = false;
			}
			else
			{
				char		ident_response[80 + IDENT_USERNAME_MAX];

				rc = recv(sock_fd, ident_response,
						  sizeof(ident_response) - 1, 0);
				if (rc < 0)
				{
					snprintf(PQerrormsg, PQERRORMSG_LENGTH,
						"Unable to receive response from Ident server "
						"on the host which is "
						"trying to connect to Postgres (Host %s, Port %d),"
						"even though we successfully sent our query to it.  "
						"errno = %s (%d)\n",
						inet_ntoa(remote_ip_addr), IDENT_PORT,
						strerror(errno), errno);
					fputs(PQerrormsg, stderr);
					pqdebug("%s", PQerrormsg);
					ident_return = false;
				}
				else
				{
					ident_response[rc] = '\0';
					ident_return = interpret_ident_response(ident_response,
															ident_user);
				}
			}
			close(sock_fd);
		}
	}
	return ident_return;
}


/*
 *  Talk to the ident server on the remote host and find out who owns the
 *  connection described by "port".  Then look in the usermap file under
 *  the usermap *auth_arg and see if that user is equivalent to
 *  Postgres user *user.
 *
 *  Return STATUS_OK if yes.
 */
int
authident(struct sockaddr_in *raddr, struct sockaddr_in *laddr,
		  const char *pg_user, const char *auth_arg)
{
	/* We were unable to get ident to give us a username */
	char		ident_user[IDENT_USERNAME_MAX + 1];

	/* The username returned by ident */
	if (!ident(raddr->sin_addr, laddr->sin_addr,
		  raddr->sin_port, laddr->sin_port, ident_user))
		return STATUS_ERROR;

	if (check_ident_usermap(auth_arg, pg_user, ident_user))
		return STATUS_OK;
	else
		return STATUS_ERROR;
}


/*
 *  Determine what authentication method should be used when accessing database
 *  "database" from frontend "raddr", user "user".  Return the method,
 *  an optional argument, and STATUS_OK.
 *  Note that STATUS_ERROR indicates a problem with the hba config file.
 *  If the file is OK but does not contain any entry matching the request,
 *  we return STATUS_OK and method = uaReject.
 */
int
hba_getauthmethod(hbaPort *port)
{

	if (check_hba(port))
		return STATUS_OK;
	else
		return STATUS_ERROR;
}

/*
 * Clear tokenized file contents and force reload on next use.
 */
void load_hba_and_ident(void)
{
	load_hba();
	load_ident();
}


/* Character set stuff.  Not sure it really belongs in this file. */

#ifdef CYR_RECODE
#define CHARSET_FILE "charset.conf"
#define MAX_CHARSETS   10
#define KEY_HOST	   1
#define KEY_BASE	   2
#define KEY_TABLE	   3

struct CharsetItem
{
	char		Orig[MAX_TOKEN];
	char		Dest[MAX_TOKEN];
	char		Table[MAX_TOKEN];
};


static bool
CharSetInRange(char *buf, int host)
{
	int			valid,
				i,
				FromAddr,
				ToAddr,
				tmp;
	struct in_addr file_ip_addr;
	char	   *p;
	unsigned int one = 0x80000000,
				NetMask = 0;
	unsigned char mask;

	p = strchr(buf, '/');
	if (p)
	{
		*p++ = '\0';
		valid = inet_aton(buf, &file_ip_addr);
		if (valid)
		{
			mask = strtoul(p, 0, 0);
			FromAddr = ntohl(file_ip_addr.s_addr);
			ToAddr = ntohl(file_ip_addr.s_addr);
			for (i = 0; i < mask; i++)
			{
				NetMask |= one;
				one >>= 1;
			}
			FromAddr &= NetMask;
			ToAddr = ToAddr | ~NetMask;
			tmp = ntohl(host);
			return ((unsigned) tmp >= (unsigned) FromAddr &&
					(unsigned) tmp <= (unsigned) ToAddr);
		}
	}
	else
	{
		p = strchr(buf, '-');
		if (p)
		{
			*p++ = '\0';
			valid = inet_aton(buf, &file_ip_addr);
			if (valid)
			{
				FromAddr = ntohl(file_ip_addr.s_addr);
				valid = inet_aton(p, &file_ip_addr);
				if (valid)
				{
					ToAddr = ntohl(file_ip_addr.s_addr);
					tmp = ntohl(host);
					return ((unsigned) tmp >= (unsigned) FromAddr &&
							(unsigned) tmp <= (unsigned) ToAddr);
				}
			}
		}
		else
		{
			valid = inet_aton(buf, &file_ip_addr);
			if (valid)
			{
				FromAddr = file_ip_addr.s_addr;
				return (unsigned) FromAddr == (unsigned) host;
			}
		}
	}
	return false;
}

void
GetCharSetByHost(char *TableName, int host, const char *DataDir)
{
	FILE	   *file;
	char		buf[MAX_TOKEN],
				BaseCharset[MAX_TOKEN],
				OrigCharset[MAX_TOKEN],
				DestCharset[MAX_TOKEN],
				HostCharset[MAX_TOKEN],
				c,
				eof = false,
			   *map_file;
	int			key = 0,
				ChIndex = 0,
				i,
				bufsize;

	struct CharsetItem *ChArray[MAX_CHARSETS];

	*TableName = '\0';
	bufsize = (strlen(DataDir) + strlen(CHARSET_FILE) + 2) * sizeof(char);
	map_file = (char *) palloc(bufsize);
	snprintf(map_file, bufsize, "%s/%s", DataDir, CHARSET_FILE);
	file = AllocateFile(map_file, PG_BINARY_R);
	if (file == NULL)
	{
		/* XXX should we log a complaint? */
		return;
	}
	while (!eof)
	{
		c = getc(file);
		ungetc(c, file);
		if (c == EOF)
			eof = true;
		else
		{
			if (c == '#')
				read_to_eol(file);
			else
			{
				/* Read the key */
				next_token(file, buf, sizeof(buf));
				if (buf[0] != '\0')
				{
					if (strcasecmp(buf, "HostCharset") == 0)
						key = KEY_HOST;
					if (strcasecmp(buf, "BaseCharset") == 0)
						key = KEY_BASE;
					if (strcasecmp(buf, "RecodeTable") == 0)
						key = KEY_TABLE;
					switch (key)
					{
						case KEY_HOST:
							/* Read the host */
							next_token(file, buf, sizeof(buf));
							if (buf[0] != '\0')
							{
								if (CharSetInRange(buf, host))
								{
									/* Read the charset */
									next_token(file, buf, sizeof(buf));
									if (buf[0] != '\0')
										strcpy(HostCharset, buf);
								}
							}
							break;
						case KEY_BASE:
							/* Read the base charset */
							next_token(file, buf, sizeof(buf));
							if (buf[0] != '\0')
								strcpy(BaseCharset, buf);
							break;
						case KEY_TABLE:
							/* Read the original charset */
							next_token(file, buf, sizeof(buf));
							if (buf[0] != '\0')
							{
								strcpy(OrigCharset, buf);
								/* Read the destination charset */
								next_token(file, buf, sizeof(buf));
								if (buf[0] != '\0')
								{
									strcpy(DestCharset, buf);
									/* Read the table filename */
									next_token(file, buf, sizeof(buf));
									if (buf[0] != '\0')
									{
										ChArray[ChIndex] =
											(struct CharsetItem *) palloc(sizeof(struct CharsetItem));
										strcpy(ChArray[ChIndex]->Orig, OrigCharset);
										strcpy(ChArray[ChIndex]->Dest, DestCharset);
										strcpy(ChArray[ChIndex]->Table, buf);
										ChIndex++;
									}
								}
							}
							break;
					}
					read_to_eol(file);
				}
			}
		}
	}
	FreeFile(file);
	pfree(map_file);

	for (i = 0; i < ChIndex; i++)
	{
		if (!strcasecmp(BaseCharset, ChArray[i]->Orig) &&
			!strcasecmp(HostCharset, ChArray[i]->Dest))
			strncpy(TableName, ChArray[i]->Table, 79);
		pfree((struct CharsetItem *) ChArray[i]);
	}
}
#endif



