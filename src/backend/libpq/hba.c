/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and possibly using ident).
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/hba.c,v 1.79.2.2 2003/04/13 04:07:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#if defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || defined(HAVE_STRUCT_SOCKCRED)
#include <sys/uio.h>
#include <sys/ucred.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/fd.h"


#define MAX_TOKEN 80
/* Maximum size of one token in the configuration file	*/

#define IDENT_USERNAME_MAX 512
/* Max size of username ident server can return */

/*
 * These variables hold the pre-parsed contents of the hba and ident
 * configuration files.  Each is a list of sublists, one sublist for
 * each (non-empty, non-comment) line of the file.	Each sublist's
 * first item is an integer line number (so we can give somewhat-useful
 * location info in error messages).  Remaining items are palloc'd strings,
 * one string per token on the line.  Note there will always be at least
 * one token, since blank lines are not entered in the data structure.
 */
static List *hba_lines = NIL;	/* pre-parsed contents of hba file */
static List *ident_lines = NIL; /* pre-parsed contents of ident file */


/*
 * isblank() exists in the ISO C99 spec, but it's not very portable yet,
 * so provide our own version.
 */
static bool
pg_isblank(const char c)
{
	return c == ' ' || c == '\t';
}


/*
 *	Grab one token out of fp.  Tokens are strings of non-blank
 *	characters bounded by blank characters, beginning of line, and end
 *	of line.	Blank means space or tab.  Return the token as *buf.
 *	Leave file positioned to character immediately after the token or
 *	EOF, whichever comes first.  If no more tokens on line, return null
 *	string as *buf and position file to beginning of next line or EOF,
 *	whichever comes first.
 */
static void
next_token(FILE *fp, char *buf, const int bufsz)
{
	int			c;
	char	   *eb = buf + (bufsz - 1);

	/* Move over initial token-delimiting blanks */
	while ((c = getc(fp)) != EOF && pg_isblank(c))
		;

	if (c != EOF && c != '\n')
	{
		/*
		 * build a token in buf of next characters up to EOF, eol, or
		 * blank.  If the token gets too long, we still parse it
		 * correctly, but the excess characters are not stored into *buf.
		 */
		while (c != EOF && c != '\n' && !pg_isblank(c))
		{
			if (buf < eb)
				*buf++ = c;
			c = getc(fp);
		}

		/*
		 * Put back the char right after the token (critical in case it is
		 * eol, since we need to detect end-of-line at next call).
		 */
		if (c != EOF)
			ungetc(c, fp);
	}
	*buf = '\0';
}


static void
read_through_eol(FILE *file)
{
	int			c;

	while ((c = getc(file)) != EOF && c != '\n')
		;
}


/*
 *	Read the given file and create a list of line sublists.
 */
static List *
tokenize_file(FILE *file)
{
	List	   *lines = NIL;
	List	   *next_line = NIL;
	int			line_number = 1;
	char		buf[MAX_TOKEN];
	char	   *comment_ptr;

	while (!feof(file))
	{
		next_token(file, buf, sizeof(buf));

		/* trim off comment, even if inside a token */
		comment_ptr = strchr(buf, '#');
		if (comment_ptr != NULL)
			*comment_ptr = '\0';

		/* add token to list, unless we are at eol or comment start */
		if (buf[0] != '\0')
		{
			if (next_line == NIL)
			{
				/* make a new line List */
				next_line = makeListi1(line_number);
				lines = lappend(lines, next_line);
			}
			/* append token to current line's list */
			next_line = lappend(next_line, pstrdup(buf));
		}
		else
		{
			/* we are at real or logical eol, so force a new line List */
			next_line = NIL;
		}

		if (comment_ptr != NULL)
		{
			/* Found a comment, so skip the rest of the line */
			read_through_eol(file);
			next_line = NIL;
		}

		/* Advance line number whenever we reach eol */
		if (next_line == NIL)
			line_number++;
	}

	return lines;
}


/*
 * Free memory used by lines/tokens (ie, structure built by tokenize_file)
 */
static void
free_lines(List **lines)
{
	if (*lines)
	{
		List	   *line,
				   *token;

		foreach(line, *lines)
		{
			List	   *ln = lfirst(line);

			/* free the pstrdup'd tokens (don't try it on the line number) */
			foreach(token, lnext(ln))
				pfree(lfirst(token));
			/* free the sublist structure itself */
			freeList(ln);
		}
		/* free the list structure itself */
		freeList(*lines);
		/* clear the static variable */
		*lines = NIL;
	}
}


/*
 *	Scan the rest of a host record (after the mask field)
 *	and return the interpretation of it as *userauth_p, auth_arg, and
 *	*error_p.  line points to the next token of the line.
 */
static void
parse_hba_auth(List *line, UserAuth *userauth_p, char *auth_arg,
			   bool *error_p)
{
	char	   *token;

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
		else if (strcmp(token, "md5") == 0)
			*userauth_p = uaMD5;
		else if (strcmp(token, "crypt") == 0)
			*userauth_p = uaCrypt;
#ifdef USE_PAM
		else if (strcmp(token, "pam") == 0)
			*userauth_p = uaPAM;
#endif
		else
			*error_p = true;
		line = lnext(line);
	}

	if (!*error_p)
	{
		/* Get the authentication argument token, if any */
		if (!line)
			auth_arg[0] = '\0';
		else
		{
			StrNCpy(auth_arg, lfirst(line), MAX_AUTH_ARG - 1);
			/* If there is more on the line, it is an error */
			if (lnext(line))
				*error_p = true;
		}
	}
}


/*
 *	Process one line from the hba config file.
 *
 *	See if it applies to a connection from a host with IP address port->raddr
 *	to a database named port->database.  If so, return *found_p true
 *	and fill in the auth arguments into the appropriate port fields.
 *	If not, leave *found_p as it was.  If the record has a syntax error,
 *	return *error_p true, after issuing a message to stderr.  If no error,
 *	leave *error_p as it was.
 */
static void
parse_hba(List *line, hbaPort *port, bool *found_p, bool *error_p)
{
	int			line_number;
	char	   *token;
	char	   *db;

	Assert(line != NIL);
	line_number = lfirsti(line);
	line = lnext(line);
	Assert(line != NIL);
	/* Check the record type. */
	token = lfirst(line);
	if (strcmp(token, "local") == 0)
	{
		/* Get the database. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		db = lfirst(line);

		/* Read the rest of the line. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		parse_hba_auth(line, &port->auth_method, port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;

		/*
		 * Disallow auth methods that always need AF_INET sockets to work.
		 */
		if (port->auth_method == uaKrb4 ||
			port->auth_method == uaKrb5)
			goto hba_syntax;

		/*
		 * If this record doesn't match the parameters of the connection
		 * attempt, ignore it.
		 */
		if ((strcmp(db, port->database) != 0 &&
			 strcmp(db, "all") != 0 &&
			 (strcmp(db, "sameuser") != 0 ||
			  strcmp(port->database, port->user) != 0)) ||
			port->raddr.sa.sa_family != AF_UNIX)
			return;
	}
	else if (strcmp(token, "host") == 0 || strcmp(token, "hostssl") == 0)
	{
		struct in_addr file_ip_addr,
					mask;

		if (strcmp(token, "hostssl") == 0)
		{
#ifdef USE_SSL
			/* Record does not match if we are not on an SSL connection */
			if (!port->ssl)
				return;

			/* Placeholder to require specific SSL level, perhaps? */
			/* Or a client certificate */

			/* Since we were on SSL, proceed as with normal 'host' mode */
#else
			/* We don't accept this keyword at all if no SSL support */
			goto hba_syntax;
#endif
		}

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
		if (!inet_aton(token, &file_ip_addr))
			goto hba_syntax;

		/* Read the mask field. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		token = lfirst(line);
		if (!inet_aton(token, &mask))
			goto hba_syntax;

		/* Read the rest of the line. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		parse_hba_auth(line, &port->auth_method, port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;

		/*
		 * If this record doesn't match the parameters of the connection
		 * attempt, ignore it.
		 */
		if ((strcmp(db, port->database) != 0 &&
			 strcmp(db, "all") != 0 &&
			 (strcmp(db, "sameuser") != 0 ||
			  strcmp(port->database, port->user) != 0)) ||
			port->raddr.sa.sa_family != AF_INET ||
			((file_ip_addr.s_addr ^ port->raddr.in.sin_addr.s_addr) & mask.s_addr) != 0)
			return;
	}
	else
		goto hba_syntax;

	/* Success */
	*found_p = true;
	return;

hba_syntax:
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			 "parse_hba: invalid syntax in pg_hba.conf file at line %d, token \"%s\"\n",
			 line_number,
			 line ? (const char *) lfirst(line) : "(end of line)");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	*error_p = true;
	return;
}


/*
 *	Scan the (pre-parsed) hba file line by line, looking for a match
 *	to the port's connection request.
 */
static bool
check_hba(hbaPort *port)
{
	bool		found_entry = false;
	bool		error = false;
	List	   *line;

	foreach(line, hba_lines)
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
load_hba(void)
{
	int			fd,
				bufsize;
	FILE	   *file;			/* The config file we have to read */
	char	   *old_conf_file;

	if (hba_lines)
		free_lines(&hba_lines);

	/*
	 * The name of old config file that better not exist. Fail if config
	 * file by old name exists. Put together the full pathname to the old
	 * config file.
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
			hba_lines = tokenize_file(file);
			FreeFile(file);
		}
		pfree(conf_file);
	}
	pfree(old_conf_file);
}


/*
 *	Process one line from the ident config file.
 *
 *	Take the line and compare it to the needed map, pg_user and ident_user.
 *	*found_p and *error_p are set according to our results.
 */
static void
parse_ident_usermap(List *line, const char *usermap_name, const char *pg_user,
					const char *ident_user, bool *found_p, bool *error_p)
{
	int			line_number;
	char	   *token;
	char	   *file_map;
	char	   *file_pguser;
	char	   *file_ident_user;

	*found_p = false;
	*error_p = false;

	Assert(line != NIL);
	line_number = lfirsti(line);
	line = lnext(line);
	Assert(line != NIL);

	/* Get the map token (must exist) */
	token = lfirst(line);
	file_map = token;

	/* Get the ident user token (must be provided) */
	line = lnext(line);
	if (!line)
		goto ident_syntax;
	token = lfirst(line);
	file_ident_user = token;

	/* Get the PG username token */
	line = lnext(line);
	if (!line)
		goto ident_syntax;
	token = lfirst(line);
	file_pguser = token;

	/* Match? */
	if (strcmp(file_map, usermap_name) == 0 &&
		strcmp(file_pguser, pg_user) == 0 &&
		strcmp(file_ident_user, ident_user) == 0)
		*found_p = true;
	return;

ident_syntax:
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			 "parse_ident_usermap: invalid syntax in pg_ident.conf file at line %d, token \"%s\"\n",
			 line_number,
			 line ? (const char *) lfirst(line) : "(end of line)");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	*error_p = true;
	return;
}


/*
 *	Scan the (pre-parsed) ident usermap file line by line, looking for a match
 *
 *	See if the user with ident username "ident_user" is allowed to act
 *	as Postgres user "pguser" according to usermap "usermap_name".
 *
 *	Special case: For usermap "sameuser", don't look in the usermap
 *	file.  That's an implied map where "pguser" must be identical to
 *	"ident_user" in order to be authorized.
 *
 *	Iff authorized, return true.
 */
static bool
check_ident_usermap(const char *usermap_name,
					const char *pg_user,
					const char *ident_user)
{
	List	   *line;
	bool		found_entry = false,
				error = false;

	if (usermap_name[0] == '\0')
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "check_ident_usermap: hba configuration file does not "
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
 * Read the ident config file and create a List of Lists of tokens in the file.
 */
static void
load_ident(void)
{
	FILE	   *file;			/* The map file we have to read */
	char	   *map_file;		/* The name of the map file we have to
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
				 "load_ident: Unable to open usermap file \"%s\": %s\n",
				 map_file, strerror(errno));
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
	else
	{
		ident_lines = tokenize_file(file);
		FreeFile(file);
	}
	pfree(map_file);
}


/*
 *	Parse the string "*ident_response" as a response from a query to an Ident
 *	server.  If it's a normal response indicating a username, return true
 *	and store the username at *ident_user.	If it's anything else,
 *	return false.
 */
static bool
interpret_ident_response(char *ident_response,
						 char *ident_user)
{
	char	   *cursor = ident_response;		/* Cursor into
												 * *ident_response */

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
			while (pg_isblank(*cursor))
				cursor++;		/* skip blanks */
			i = 0;
			while (*cursor != ':' && *cursor != '\r' && !pg_isblank(*cursor) &&
				   i < (int) (sizeof(response_type) - 1))
				response_type[i++] = *cursor++;
			response_type[i] = '\0';
			while (pg_isblank(*cursor))
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
						while (pg_isblank(*cursor))
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
 *	Talk to the ident server on host "remote_ip_addr" and find out who
 *	owns the tcp connection from his port "remote_port" to port
 *	"local_port_addr" on host "local_ip_addr".	Return the username the
 *	ident server gives as "*ident_user".
 *
 *	IP addresses and port numbers are in network byte order.
 *
 *	But iff we're unable to get the information from ident, return false.
 */
static bool
ident_inet(const struct in_addr remote_ip_addr,
		   const struct in_addr local_ip_addr,
		   const ushort remote_port,
		   const ushort local_port,
		   char *ident_user)
{
	int			sock_fd,		/* File descriptor for socket on which we
								 * talk to Ident */
				rc;				/* Return code from a locally called
								 * function */
	bool		ident_return;

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
			/* loop in case send is interrupted */
			do {
				rc = send(sock_fd, ident_query, strlen(ident_query), 0);
			} while (rc < 0 && errno == EINTR);
			if (rc < 0)
			{
				snprintf(PQerrormsg, PQERRORMSG_LENGTH,
						 "Unable to send query to Ident server on the host which is "
					  "trying to connect to Postgres (Host %s, Port %d),"
						 "even though we successfully connected to it.  "
						 "errno = %s (%d)\n",
						 inet_ntoa(remote_ip_addr), IDENT_PORT,
						 strerror(errno), errno);
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
 *	Ask kernel about the credentials of the connecting process and
 *	determine the symbolic name of the corresponding user.
 *
 *	Returns either true and the username put into "ident_user",
 *	or false if we were unable to determine the username.
 */
static bool
ident_unix(int sock, char *ident_user)
{
#if defined(SO_PEERCRED)
	/* Linux style: use getsockopt(SO_PEERCRED) */
	struct ucred peercred;
	ACCEPT_TYPE_ARG3 so_len = sizeof(peercred);
	struct passwd *pass;

	errno = 0;
	if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &peercred, &so_len) != 0 ||
		so_len != sizeof(peercred))
	{
		/* We didn't get a valid credentials struct. */
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "ident_unix: error receiving credentials: %s\n",
				 strerror(errno));
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return false;
	}

	pass = getpwuid(peercred.uid);

	if (pass == NULL)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
		   "ident_unix: unknown local user with uid %d\n", peercred.uid);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return false;
	}

	StrNCpy(ident_user, pass->pw_name, IDENT_USERNAME_MAX + 1);

	return true;

#elif defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || (defined(HAVE_STRUCT_SOCKCRED) && defined(LOCAL_CREDS))
	struct msghdr msg;

/* Credentials structure */
#ifdef HAVE_STRUCT_CMSGCRED
	typedef struct cmsgcred Cred;

#define cruid cmcred_uid
#elif HAVE_STRUCT_FCRED
	typedef struct fcred Cred;

#define cruid fc_uid
#elif HAVE_STRUCT_SOCKCRED
	typedef struct sockcred Cred;

#define cruid sc_uid
#endif
	Cred	   *cred;

	/* Compute size without padding */
	char		cmsgmem[ALIGN(sizeof(struct cmsghdr)) + ALIGN(sizeof(Cred))];	/* for NetBSD */

	/* Point to start of first structure */
	struct cmsghdr *cmsg = (struct cmsghdr *) cmsgmem;

	struct iovec iov;
	char		buf;
	struct passwd *pw;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = (char *) cmsg;
	msg.msg_controllen = sizeof(cmsgmem);
	memset(cmsg, 0, sizeof(cmsgmem));

	/*
	 * The one character which is received here is not meaningful; its
	 * purposes is only to make sure that recvmsg() blocks long enough for
	 * the other side to send its credentials.
	 */
	iov.iov_base = &buf;
	iov.iov_len = 1;

	if (recvmsg(sock, &msg, 0) < 0 ||
		cmsg->cmsg_len < sizeof(cmsgmem) ||
		cmsg->cmsg_type != SCM_CREDS)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "ident_unix: error receiving credentials: %s\n",
				 strerror(errno));
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return false;
	}

	cred = (Cred *) CMSG_DATA(cmsg);

	pw = getpwuid(cred->cruid);
	if (pw == NULL)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "ident_unix: unknown local user with uid %d\n",
				 cred->cruid);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return false;
	}

	StrNCpy(ident_user, pw->pw_name, IDENT_USERNAME_MAX + 1);

	return true;

#else
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			 "'ident' auth is not supported on local connections on this platform\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	return false;
#endif
}

/*
 *	Determine the username of the initiator of the connection described
 *	by "port".	Then look in the usermap file under the usermap
 *	port->auth_arg and see if that user is equivalent to Postgres user
 *	port->user.
 *
 *	Return STATUS_OK if yes, STATUS_ERROR if no match (or couldn't get info).
 */
int
authident(hbaPort *port)
{
	char		ident_user[IDENT_USERNAME_MAX + 1];

	switch (port->raddr.sa.sa_family)
	{
		case AF_INET:
			if (!ident_inet(port->raddr.in.sin_addr,
							port->laddr.in.sin_addr,
							port->raddr.in.sin_port,
							port->laddr.in.sin_port, ident_user))
				return STATUS_ERROR;
			break;
		case AF_UNIX:
			if (!ident_unix(port->sock, ident_user))
				return STATUS_ERROR;
			break;
		default:
			return STATUS_ERROR;
	}

	if (check_ident_usermap(port->auth_arg, port->user, ident_user))
		return STATUS_OK;
	else
		return STATUS_ERROR;
}


/*
 *	Determine what authentication method should be used when accessing database
 *	"database" from frontend "raddr", user "user".	Return the method and
 *	an optional argument (stored in fields of *port), and STATUS_OK.
 *
 *	Note that STATUS_ERROR indicates a problem with the hba config file.
 *	If the file is OK but does not contain any entry matching the request,
 *	we return STATUS_OK and method = uaReject.
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
 * Clear and reload tokenized file contents.
 */
void
load_hba_and_ident(void)
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
			   *map_file;
	int			key,
				ChIndex = 0,
				c,
				i,
				bufsize;
	struct CharsetItem *ChArray[MAX_CHARSETS];

	*TableName = '\0';
	bufsize = (strlen(DataDir) + strlen(CHARSET_FILE) + 2) * sizeof(char);
	map_file = (char *) palloc(bufsize);
	snprintf(map_file, bufsize, "%s/%s", DataDir, CHARSET_FILE);
	file = AllocateFile(map_file, PG_BINARY_R);
	pfree(map_file);
	if (file == NULL)
	{
		/* XXX should we log a complaint? */
		return;
	}
	while ((c = getc(file)) != EOF)
	{
		if (c == '#')
			read_through_eol(file);
		else
		{
			/* Read the key */
			ungetc(c, file);
			next_token(file, buf, sizeof(buf));
			if (buf[0] != '\0')
			{
				key = 0;
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
				read_through_eol(file);
			}
		}
	}
	FreeFile(file);

	for (i = 0; i < ChIndex; i++)
	{
		if (strcasecmp(BaseCharset, ChArray[i]->Orig) == 0 &&
			strcasecmp(HostCharset, ChArray[i]->Dest) == 0)
			strncpy(TableName, ChArray[i]->Table, 79);
		pfree(ChArray[i]);
	}
}

#endif   /* CYR_RECODE */
