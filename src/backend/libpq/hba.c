/*-------------------------------------------------------------------------
 *
 * hba.c--
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and possibly using ident).
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/hba.c,v 1.30 1998/03/15 08:18:03 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pwd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <postgres.h>
#include <miscadmin.h>
#include <libpq/libpq.h>
#include <libpq/pqcomm.h>
#include <libpq/hba.h>
#include <port/inet_aton.h>		/* For inet_aton() */
#include <storage/fd.h>

/* Some standard C libraries, including GNU, have an isblank() function.
   Others, including Solaris, do not.  So we have our own.
*/
static bool
isblank(const char c)
{
	return (c == ' ' || c == 9 /* tab */ );
}



static void
next_token(FILE *fp, char *buf, const int bufsz)
{
/*--------------------------------------------------------------------------
  Grab one token out of fp.  Tokens are strings of non-blank
  characters bounded by blank characters, beginning of line, and end
  of line.	Blank means space or tab.  Return the token as *buf.
  Leave file positioned to character immediately after the token or
  EOF, whichever comes first.  If no more tokens on line, return null
  string as *buf and position file to beginning of next line or EOF,
  whichever comes first.
--------------------------------------------------------------------------*/
	int			c;
	char	   *eb = buf + (bufsz - 1);

	/* Move over inital token-delimiting blanks */
	while (isblank(c = getc(fp)));

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

			/*
			 * Put back the char right after the token (putting back EOF
			 * is ok)
			 */
		}
		ungetc(c, fp);
	}
	*buf = '\0';
}



static void
read_through_eol(FILE *file)
{
	int			c;

	do
		c = getc(file);
	while (c != '\n' && c != EOF);
}



static void
read_hba_entry2(FILE *file, UserAuth *userauth_p, char auth_arg[],
				bool *error_p)
{
/*--------------------------------------------------------------------------
  Read from file FILE the rest of a host record, after the mask field,
  and return the interpretation of it as *userauth_p, auth_arg, and
  *error_p.
---------------------------------------------------------------------------*/
	char		buf[MAX_TOKEN];

	/* Get authentication type token. */
	next_token(file, buf, sizeof(buf));

	if (strcmp(buf, "trust") == 0)
		*userauth_p = uaTrust;
	else if (strcmp(buf, "ident") == 0)
		*userauth_p = uaIdent;
	else if (strcmp(buf, "password") == 0)
		*userauth_p = uaPassword;
	else if (strcmp(buf, "krb4") == 0)
		*userauth_p = uaKrb4;
	else if (strcmp(buf, "krb5") == 0)
		*userauth_p = uaKrb5;
	else if (strcmp(buf, "reject") == 0)
		*userauth_p = uaReject;
	else if (strcmp(buf, "crypt") == 0)
		*userauth_p = uaCrypt;
	else
	{
		*error_p = true;

		if (buf[0] != '\0')
			read_through_eol(file);
	}

	if (!*error_p)
	{
		/* Get the authentication argument token, if any */
		next_token(file, buf, sizeof(buf));
		if (buf[0] == '\0')
			auth_arg[0] = '\0';
		else
		{
			StrNCpy(auth_arg, buf, MAX_AUTH_ARG - 1);
			next_token(file, buf, sizeof(buf));
			if (buf[0] != '\0')
			{
				*error_p = true;
				read_through_eol(file);
			}
		}
	}
}



static void
process_hba_record(FILE *file, SockAddr *raddr, const char database[],
				   bool *matches_p, bool *error_p,
				   UserAuth *userauth_p, char auth_arg[])
{
/*---------------------------------------------------------------------------
  Process the non-comment record in the config file that is next on the file.
  See if it applies to a connection to a host with IP address "*raddr"
  to a database named "database[]".  If so, return *matches_p true
  and *userauth_p and auth_arg[] as the values from the entry.
  If not, leave *matches_p as it was.  If the record has a syntax error,
  return *error_p true, after issuing a message to stderr.	If no error,
  leave *error_p as it was.
---------------------------------------------------------------------------*/
	char		db[MAX_TOKEN],
				buf[MAX_TOKEN];

	/* Read the record type field. */

	next_token(file, buf, sizeof(buf));

	if (buf[0] == '\0')
		return;

	/* Check the record type. */

	if (strcmp(buf, "local") == 0)
	{
		/* Get the database. */

		next_token(file, db, sizeof(db));

		if (db[0] == '\0')
			goto syntax;

		/* Read the rest of the line. */

		read_hba_entry2(file, userauth_p, auth_arg, error_p);

		/*
		 * For now, disallow methods that need AF_INET sockets to work.
		 */

		if (!*error_p &&
			(*userauth_p == uaIdent ||
			 *userauth_p == uaKrb4 ||
			 *userauth_p == uaKrb5))
			*error_p = true;

		if (*error_p)
			goto syntax;

		/*
		 * If this record isn't for our database, or this is the wrong
		 * sort of connection, ignore it.
		 */

		if ((strcmp(db, database) != 0 && strcmp(db, "all") != 0) ||
			raddr->sa.sa_family != AF_UNIX)
			return;
	}
	else if (strcmp(buf, "host") == 0)
	{
		struct in_addr file_ip_addr,
					mask;

		/* Get the database. */

		next_token(file, db, sizeof(db));

		if (db[0] == '\0')
			goto syntax;

		/* Read the IP address field. */

		next_token(file, buf, sizeof(buf));

		if (buf[0] == '\0')
			goto syntax;

		/* Remember the IP address field and go get mask field. */

		if (!inet_aton(buf, &file_ip_addr))
		{
			read_through_eol(file);
			goto syntax;
		}

		/* Read the mask field. */

		next_token(file, buf, sizeof(buf));

		if (buf[0] == '\0')
			goto syntax;

		if (!inet_aton(buf, &mask))
		{
			read_through_eol(file);
			goto syntax;
		}

		/*
		 * This is the record we're looking for.  Read the rest of the
		 * info from it.
		 */

		read_hba_entry2(file, userauth_p, auth_arg, error_p);

		if (*error_p)
			goto syntax;

		/*
		 * If this record isn't for our database, or this is the wrong
		 * sort of connection, ignore it.
		 */

		if ((strcmp(db, database) != 0 && strcmp(db, "all") != 0) ||
			raddr->sa.sa_family != AF_INET ||
			((file_ip_addr.s_addr ^ raddr->in.sin_addr.s_addr) & mask.s_addr) != 0x0000)
			return;
	}
	else
	{
		read_through_eol(file);
		goto syntax;
	}

	*matches_p = true;

	return;

syntax:
	sprintf(PQerrormsg,
			"process_hba_record: invalid syntax in pg_hba.conf file\n");

	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	*error_p = true;
}



static void
process_open_config_file(FILE *file, SockAddr *raddr, const char database[],
						 bool *host_ok_p, UserAuth *userauth_p,
						 char auth_arg[])
{
/*---------------------------------------------------------------------------
  This function does the same thing as find_hba_entry, only with
  the config file already open on stream descriptor "file".
----------------------------------------------------------------------------*/
	bool		found_entry;

	/* We've processed a record that applies to our connection */
	bool		error;

	/* Said record has invalid syntax. */
	bool		eof;			/* We've reached the end of the file we're
								 * reading */

	found_entry = false;		/* initial value */
	error = false;				/* initial value */
	eof = false;				/* initial value */
	while (!eof && !found_entry && !error)
	{
		/* Process a line from the config file */

		int			c;			/* a character read from the file */

		c = getc(file);
		ungetc(c, file);
		if (c == EOF)
			eof = true;
		else
		{
			if (c == '#')
				read_through_eol(file);
			else
			{
				process_hba_record(file, raddr, database,
							 &found_entry, &error, userauth_p, auth_arg);
			}
		}
	}

	if (!error)
	{
		/* If no entry was found then force a rejection. */

		if (!found_entry)
			*userauth_p = uaReject;

		*host_ok_p = true;
	}
}



static void
find_hba_entry(SockAddr *raddr, const char database[], bool *host_ok_p,
			   UserAuth *userauth_p, char auth_arg[])
{
/*--------------------------------------------------------------------------
  Read the config file and find an entry that allows connection from
  host "*raddr" to database "database".  If found, return *host_ok_p == true
  and *userauth_p and *auth_arg representing the contents of that entry.

  When a record has invalid syntax, we either ignore it or reject the
  connection (depending on where it's invalid).  No message or anything.
  We need to fix that some day.

  If we don't find or can't access the config file, we issue an error
  message and deny the connection.

  If we find a file by the old name of the config file (pg_hba), we issue
  an error message because it probably needs to be converted.  He didn't
  follow directions and just installed his old hba file in the new database
  system.

---------------------------------------------------------------------------*/
	int			fd;

	FILE	   *file;			/* The config file we have to read */

	char	   *old_conf_file;

	/* The name of old config file that better not exist. */

	/* Fail if config file by old name exists. */


	/* put together the full pathname to the old config file */
	old_conf_file = (char *) palloc((strlen(DataDir) +
							  strlen(OLD_CONF_FILE) + 2) * sizeof(char));
	sprintf(old_conf_file, "%s/%s", DataDir, OLD_CONF_FILE);

	if ((fd = open(old_conf_file, O_RDONLY, 0)) != -1)
	{
		/* Old config file exists.	Tell this guy he needs to upgrade. */
		close(fd);
		sprintf(PQerrormsg,
		  "A file exists by the name used for host-based authentication "
		   "in prior releases of Postgres (%s).  The name and format of "
		   "the configuration file have changed, so this file should be "
				"converted.\n",
				old_conf_file);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
	else
	{
		char	   *conf_file;	/* The name of the config file we have to
								 * read */

		/* put together the full pathname to the config file */
		conf_file = (char *) palloc((strlen(DataDir) +
								  strlen(CONF_FILE) + 2) * sizeof(char));
		sprintf(conf_file, "%s/%s", DataDir, CONF_FILE);

		file = AllocateFile(conf_file, "r");
		if (file == NULL)
		{
			/* The open of the config file failed.	*/

			sprintf(PQerrormsg,
				 "find_hba_entry: Host-based authentication config file "
				"does not exist or permissions are not setup correctly! "
					"Unable to open file \"%s\".\n",
					conf_file);
			fputs(PQerrormsg, stderr);
			pqdebug("%s", PQerrormsg);
		}
		else
		{
			process_open_config_file(file, raddr, database, host_ok_p, userauth_p,
									 auth_arg);
			FreeFile(file);
		}
		pfree(conf_file);
	}
	pfree(old_conf_file);
	return;
}


static void
interpret_ident_response(char ident_response[],
						 bool *error_p, char ident_username[])
{
/*----------------------------------------------------------------------------
  Parse the string "ident_response[]" as a response from a query to an Ident
  server.  If it's a normal response indicating a username, return
  *error_p == false and the username as ident_username[].  If it's anything
  else, return *error_p == true and ident_username[] undefined.
----------------------------------------------------------------------------*/
	char	   *cursor;			/* Cursor into ident_response[] */

	cursor = &ident_response[0];

	/*
	 * Ident's response, in the telnet tradition, should end in crlf
	 * (\r\n).
	 */
	if (strlen(ident_response) < 2)
		*error_p = true;
	else if (ident_response[strlen(ident_response) - 2] != '\r')
		*error_p = true;
	else
	{
		while (*cursor != ':' && *cursor != '\r')
			cursor++;			/* skip port field */

		if (*cursor != ':')
			*error_p = true;
		else
		{
			/* We're positioned to colon before response type field */
			char		response_type[80];
			int			i;		/* Index into response_type[] */

			cursor++;			/* Go over colon */
			while (isblank(*cursor))
				cursor++;		/* skip blanks */
			i = 0;
			while (*cursor != ':' && *cursor != '\r' && !isblank(*cursor)
				   && i < sizeof(response_type) - 1)
				response_type[i++] = *cursor++;
			response_type[i] = '\0';
			while (isblank(*cursor))
				cursor++;		/* skip blanks */
			if (strcmp(response_type, "USERID") != 0)
				*error_p = true;
			else
			{

				/*
				 * It's a USERID response.  Good.  "cursor" should be
				 * pointing to the colon that precedes the operating
				 * system type.
				 */
				if (*cursor != ':')
					*error_p = true;
				else
				{
					cursor++;	/* Go over colon */
					/* Skip over operating system field. */
					while (*cursor != ':' && *cursor != '\r')
						cursor++;
					if (*cursor != ':')
						*error_p = true;
					else
					{
						int			i;	/* Index into ident_username[] */

						cursor++;		/* Go over colon */
						while (isblank(*cursor))
							cursor++;	/* skip blanks */
						/* Rest of line is username.  Copy it over. */
						i = 0;
						while (*cursor != '\r' && i < IDENT_USERNAME_MAX)
							ident_username[i++] = *cursor++;
						ident_username[i] = '\0';
						*error_p = false;
					}
				}
			}
		}
	}
}



static void
ident(const struct in_addr remote_ip_addr, const struct in_addr local_ip_addr,
	  const ushort remote_port, const ushort local_port,
	  bool *ident_failed, char ident_username[])
{
/*--------------------------------------------------------------------------
  Talk to the ident server on host "remote_ip_addr" and find out who
  owns the tcp connection from his port "remote_port" to port
  "local_port_addr" on host "local_ip_addr".  Return the username the
  ident server gives as "ident_username[]".

  IP addresses and port numbers are in network byte order.

  But iff we're unable to get the information from ident, return
  *ident_failed == true (and ident_username[] undefined).
----------------------------------------------------------------------------*/

	int			sock_fd;

	/* File descriptor for socket on which we talk to Ident */

	int			rc;				/* Return code from a locally called
								 * function */

	sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock_fd == -1)
	{
		sprintf(PQerrormsg,
			 "Failed to create socket on which to talk to Ident server. "
				"socket() returned errno = %s (%d)\n",
				strerror(errno), errno);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
	else
	{
		struct sockaddr_in ident_server;

		/*
		 * Socket address of Ident server on the system from which client
		 * is attempting to connect to us.
		 */
		ident_server.sin_family = AF_INET;
		ident_server.sin_port = htons(IDENT_PORT);
		ident_server.sin_addr = remote_ip_addr;
		rc = connect(sock_fd,
			   (struct sockaddr *) & ident_server, sizeof(ident_server));
		if (rc != 0)
		{
			sprintf(PQerrormsg,
				"Unable to connect to Ident server on the host which is "
					"trying to connect to Postgres "
					"(IP address %s, Port %d). "
					"errno = %s (%d)\n",
					inet_ntoa(remote_ip_addr), IDENT_PORT, strerror(errno), errno);
			fputs(PQerrormsg, stderr);
			pqdebug("%s", PQerrormsg);
			*ident_failed = true;
		}
		else
		{
			char		ident_query[80];

			/* The query we send to the Ident server */
			sprintf(ident_query, "%d,%d\n",
					ntohs(remote_port), ntohs(local_port));
			rc = send(sock_fd, ident_query, strlen(ident_query), 0);
			if (rc < 0)
			{
				sprintf(PQerrormsg,
						"Unable to send query to Ident server on the host which is "
					  "trying to connect to Postgres (Host %s, Port %d),"
						"even though we successfully connected to it.  "
						"errno = %s (%d)\n",
						inet_ntoa(remote_ip_addr), IDENT_PORT, strerror(errno), errno);
				fputs(PQerrormsg, stderr);
				pqdebug("%s", PQerrormsg);
				*ident_failed = true;
			}
			else
			{
				char		ident_response[80 + IDENT_USERNAME_MAX];

				rc = recv(sock_fd, ident_response, sizeof(ident_response) - 1, 0);
				if (rc < 0)
				{
					sprintf(PQerrormsg,
						  "Unable to receive response from Ident server "
							"on the host which is "
					  "trying to connect to Postgres (Host %s, Port %d),"
					"even though we successfully sent our query to it.  "
							"errno = %s (%d)\n",
							inet_ntoa(remote_ip_addr), IDENT_PORT,
							strerror(errno), errno);
					fputs(PQerrormsg, stderr);
					pqdebug("%s", PQerrormsg);
					*ident_failed = true;
				}
				else
				{
					bool		error;	/* response from Ident is garbage. */

					ident_response[rc] = '\0';
					interpret_ident_response(ident_response, &error, ident_username);
					*ident_failed = error;
				}
			}
			close(sock_fd);
		}
	}
}



static void
parse_map_record(FILE *file,
				 char file_map[], char file_pguser[], char file_iuser[])
{
/*---------------------------------------------------------------------------
  Take the noncomment line which is next on file "file" and interpret
  it as a line in a usermap file.  Specifically, return the first
  3 tokens as file_map, file_iuser, and file_pguser, respectively.	If
  there are fewer than 3 tokens, return null strings for the missing
  ones.

---------------------------------------------------------------------------*/
	char		buf[MAX_TOKEN];

	/* A token read from the file */

	/* Set defaults in case fields not in file */
	file_map[0] = '\0';
	file_pguser[0] = '\0';
	file_iuser[0] = '\0';

	next_token(file, buf, sizeof(buf));
	if (buf[0] != '\0')
	{
		strcpy(file_map, buf);
		next_token(file, buf, sizeof(buf));
		if (buf[0] != '\0')
		{
			strcpy(file_iuser, buf);
			next_token(file, buf, sizeof(buf));
			if (buf[0] != '\0')
			{
				strcpy(file_pguser, buf);
				read_through_eol(file);
				return;
			}
		}
		sprintf(PQerrormsg,"Incomplete line in pg_ident: %s",file_map);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
}



static void
verify_against_open_usermap(FILE *file,
							const char pguser[],
							const char ident_username[],
							const char usermap_name[],
							bool *checks_out_p)
{
/*--------------------------------------------------------------------------
  This function does the same thing as verify_against_usermap,
  only with the config file already open on stream descriptor "file".
---------------------------------------------------------------------------*/
	bool		match;			/* We found a matching entry in the map
								 * file */
	bool		eof;			/* We've reached the end of the file we're
								 * reading */

	match = false;				/* initial value */
	eof = false;				/* initial value */
	while (!eof && !match)
	{
		/* Process a line from the map file */

		int			c;			/* a character read from the file */

		c = getc(file);
		ungetc(c, file);
		if (c == EOF)
			eof = true;
		else
		{
			if (c == '#')
				read_through_eol(file);
			else
			{
				/* The following are fields read from a record of the file */
				char		file_map[MAX_TOKEN + 1];
				char		file_pguser[MAX_TOKEN + 1];
				char		file_iuser[MAX_TOKEN + 1];

				parse_map_record(file, file_map, file_pguser, file_iuser);
				if (strcmp(file_map, usermap_name) == 0 &&
					strcmp(file_pguser, pguser) == 0 &&
					strcmp(file_iuser, ident_username) == 0)
					match = true;
			}
		}
	}
	*checks_out_p = match;
}



static void
verify_against_usermap(const char pguser[],
					   const char ident_username[],
					   const char usermap_name[],
					   bool *checks_out_p)
{
/*--------------------------------------------------------------------------
  See if the user with ident username "ident_username" is allowed to act
  as Postgres user "pguser" according to usermap "usermap_name".   Look
  it up in the usermap file.

  Special case: For usermap "sameuser", don't look in the usermap
  file.  That's an implied map where "pguser" must be identical to
  "ident_username" in order to be authorized.

  Iff authorized, return *checks_out_p == true.

--------------------------------------------------------------------------*/

	if (usermap_name[0] == '\0')
	{
		*checks_out_p = false;
		sprintf(PQerrormsg,
				"verify_against_usermap: hba configuration file does not "
		   "have the usermap field filled in in the entry that pertains "
		  "to this connection.  That field is essential for Ident-based "
				"authentication.\n");
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
	}
	else if (strcmp(usermap_name, "sameuser") == 0)
	{
		if (strcmp(ident_username, pguser) == 0)
			*checks_out_p = true;
		else
			*checks_out_p = false;
	}
	else
	{
		FILE	   *file;		/* The map file we have to read */

		char	   *map_file;	/* The name of the map file we have to
								 * read */

		/* put together the full pathname to the map file */
		map_file = (char *) palloc((strlen(DataDir) +
									strlen(MAP_FILE) + 2) * sizeof(char));
		sprintf(map_file, "%s/%s", DataDir, MAP_FILE);

		file = AllocateFile(map_file, "r");
		if (file == NULL)
		{
			/* The open of the map file failed.  */

			*checks_out_p = false;

			sprintf(PQerrormsg,
				  "verify_against_usermap: usermap file for Ident-based "
					"authentication "
				"does not exist or permissions are not setup correctly! "
					"Unable to open file \"%s\".\n",
					map_file);
			fputs(PQerrormsg, stderr);
			pqdebug("%s", PQerrormsg);
		}
		else
		{
			verify_against_open_usermap(file,
									pguser, ident_username, usermap_name,
										checks_out_p);
			FreeFile(file);
		}
		pfree(map_file);


	}
}



int
authident(struct sockaddr_in * raddr, struct sockaddr_in * laddr,
		  const char postgres_username[],
		  const char auth_arg[])
{
/*---------------------------------------------------------------------------
  Talk to the ident server on the remote host and find out who owns the
  connection described by "port".  Then look in the usermap file under
  the usermap auth_arg[] and see if that user is equivalent to
  Postgres user user[].

  Return STATUS_OK if yes.
---------------------------------------------------------------------------*/
	bool		checks_out;
	bool		ident_failed;

	/* We were unable to get ident to give us a username */
	char		ident_username[IDENT_USERNAME_MAX + 1];

	/* The username returned by ident */

	ident(raddr->sin_addr, laddr->sin_addr,
		  raddr->sin_port, laddr->sin_port,
		  &ident_failed, ident_username);

	if (ident_failed)
		return STATUS_ERROR;

	verify_against_usermap(postgres_username, ident_username, auth_arg,
						   &checks_out);

	return (checks_out ? STATUS_OK : STATUS_ERROR);
}


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

int
InRange(char *buf, int host)
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
				return ((unsigned) FromAddr == (unsigned) host);
			}
		}
	}
	return false;
}

void
GetCharSetByHost(char TableName[], int host, const char DataDir[])
{
	FILE	   *file;
	char		buf[MAX_TOKEN],
				BaseCharset[MAX_TOKEN],
				OrigCharset[MAX_TOKEN],
				DestCharset[MAX_TOKEN],
				HostCharset[MAX_TOKEN];
	char		c,
				eof = false;
	char	   *map_file;
	int			key = 0,
				i;
	struct CharsetItem *ChArray[MAX_CHARSETS];
	int			ChIndex = 0;

	*TableName = '\0';
	map_file = (char *) malloc((strlen(DataDir) +
								strlen(CHARSET_FILE) + 2) * sizeof(char));
	sprintf(map_file, "%s/%s", DataDir, CHARSET_FILE);
	file = fopen(map_file, "r");
	if (file == NULL)
		return;
	while (!eof)
	{
		c = getc(file);
		ungetc(c, file);
		if (c == EOF)
			eof = true;
		else
		{
			if (c == '#')
				read_through_eol(file);
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
								if (InRange(buf, host))
								{
									/* Read the charset */
									next_token(file, buf, sizeof(buf));
									if (buf[0] != '\0')
									{
										strcpy(HostCharset, buf);
									}
								}
							}
							break;
						case KEY_BASE:
							/* Read the base charset */
							next_token(file, buf, sizeof(buf));
							if (buf[0] != '\0')
							{
								strcpy(BaseCharset, buf);
							}
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
										ChArray[ChIndex] = (struct CharsetItem *) malloc(sizeof(struct CharsetItem));
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
	}
	fclose(file);
	free(map_file);

	for (i = 0; i < ChIndex; i++)
	{
		if (!strcasecmp(BaseCharset, ChArray[i]->Orig) &&
			!strcasecmp(HostCharset, ChArray[i]->Dest))
		{
			strncpy(TableName, ChArray[i]->Table, 79);
		}
		free((struct CharsetItem *) ChArray[i]);
	}
}

#endif

extern int
hba_getauthmethod(SockAddr *raddr, char *database, char *auth_arg,
				  UserAuth *auth_method)
{
/*---------------------------------------------------------------------------
  Determine what authentication method should be used when accessing database
  "database" from frontend "raddr".  Return the method, an optional argument,
  and STATUS_OK.
----------------------------------------------------------------------------*/
	bool		host_ok;

	host_ok = false;

	find_hba_entry(raddr, database, &host_ok, auth_method, auth_arg);

	return (host_ok ? STATUS_OK : STATUS_ERROR);
}
