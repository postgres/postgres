/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and possibly using ident).
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/hba.c,v 1.116.2.4 2004/11/17 19:54:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/socket.h>
#if defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || defined(HAVE_STRUCT_SOCKCRED)
#include <sys/uio.h>
#include <sys/ucred.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "commands/user.h"
#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/fd.h"


#define IDENT_USERNAME_MAX 512
/* Max size of username ident server can return */

/* This is used to separate values in multi-valued column strings */
#define MULTI_VALUE_SEP "\001"

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
static List *group_lines = NIL; /* pre-parsed contents of group file */
static List *user_lines = NIL;	/* pre-parsed contents of user password
								 * file */

/* sorted entries so we can do binary search lookups */
static List **user_sorted = NULL;		/* sorted user list, for bsearch() */
static List **group_sorted = NULL;		/* sorted group list, for
										 * bsearch() */
static int	user_length;
static int	group_length;

static List *tokenize_file(FILE *file);
static char *tokenize_inc_file(const char *inc_filename);

/*
 * isblank() exists in the ISO C99 spec, but it's not very portable yet,
 * so provide our own version.
 */
static bool
pg_isblank(const char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}


/*
 *	 Grab one token out of fp. Tokens are strings of non-blank
 *	 characters bounded by blank characters, beginning of line, and
 *	 end of line. Blank means space or tab. Return the token as
 *	 *buf. Leave file positioned to character immediately after the
 *	 token or EOF, whichever comes first. If no more tokens on line,
 *	 return null string as *buf and position file to beginning of
 *	 next line or EOF, whichever comes first. Allow spaces in quoted
 *	 strings. Terminate on unquoted commas. Handle comments.
 */
void
next_token(FILE *fp, char *buf, const int bufsz)
{
	int			c;
	char	   *start_buf = buf;
	char	   *end_buf = buf + (bufsz - 1);
	bool		in_quote = false;
	bool		was_quote = false;

	/* Move over initial whitespace and commas */
	while ((c = getc(fp)) != EOF && (pg_isblank(c) || c == ','))
		;

	if (c != EOF && c != '\n')
	{
		/*
		 * Build a token in buf of next characters up to EOF, EOL,
		 * unquoted comma, or unquoted whitespace.
		 */
		while (c != EOF && c != '\n' &&
			   (!pg_isblank(c) || in_quote == true))
		{
			/* skip comments to EOL */
			if (c == '#' && !in_quote)
			{
				while ((c = getc(fp)) != EOF && c != '\n')
					;
				/* If only comment, consume EOL too; return EOL */
				if (c != EOF && buf == start_buf)
					c = getc(fp);
				break;
			}

			if (buf >= end_buf)
			{
				*buf = '\0';
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("authentication file token too long, skipping: \"%s\"",
								start_buf)));
				/* Discard remainder of line */
				while ((c = getc(fp)) != EOF && c != '\n')
					;
				break;
			}

			if (c != '"' || (c == '"' && was_quote))
				*buf++ = c;

			/* We pass back the comma so the caller knows there is more */
			if ((pg_isblank(c) || c == ',') && !in_quote)
				break;

			/* Literal double-quote is two double-quotes */
			if (in_quote && c == '"')
				was_quote = !was_quote;
			else
				was_quote = false;

			if (c == '"')
				in_quote = !in_quote;

			c = getc(fp);
		}

		/*
		 * Put back the char right after the token (critical in case it is
		 * EOL, since we need to detect end-of-line at next call).
		 */
		if (c != EOF)
			ungetc(c, fp);
	}
	*buf = '\0';
}

/*
 *	 Tokenize file and handle file inclusion and comma lists. We have
 *	 to  break	apart  the	commas	to	expand	any  file names then
 *	 reconstruct with commas.
 *
 * The result is always a palloc'd string.  If it's zero-length then
 * we have reached EOL.
 */
static char *
next_token_expand(FILE *file)
{
	char		buf[MAX_TOKEN];
	char	   *comma_str = pstrdup("");
	bool		trailing_comma;
	char	   *incbuf;
	int			needed;

	do
	{
		next_token(file, buf, sizeof(buf));
		if (!*buf)
			break;

		if (buf[strlen(buf) - 1] == ',')
		{
			trailing_comma = true;
			buf[strlen(buf) - 1] = '\0';
		}
		else
			trailing_comma = false;

		/* Is this referencing a file? */
		if (buf[0] == '@')
			incbuf = tokenize_inc_file(buf + 1);
		else
			incbuf = pstrdup(buf);

		needed = strlen(comma_str) + strlen(incbuf) + 1;
		if (trailing_comma)
			needed++;
		comma_str = repalloc(comma_str, needed);
		strcat(comma_str, incbuf);
		if (trailing_comma)
			strcat(comma_str, MULTI_VALUE_SEP);
		pfree(incbuf);
	} while (trailing_comma);

	return comma_str;
}


/*
 * Free memory used by lines/tokens (i.e., structure built by tokenize_file)
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


static char *
tokenize_inc_file(const char *inc_filename)
{
	char	   *inc_fullname;
	FILE	   *inc_file;
	List	   *inc_lines;
	List	   *line;
	char	   *comma_str = pstrdup("");

	inc_fullname = (char *) palloc(strlen(DataDir) + 1 +
								   strlen(inc_filename) + 1);
	strcpy(inc_fullname, DataDir);
	strcat(inc_fullname, "/");
	strcat(inc_fullname, inc_filename);

	inc_file = AllocateFile(inc_fullname, "r");
	if (!inc_file)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open secondary authentication file \"@%s\" as \"%s\": %m",
						inc_filename, inc_fullname)));
		pfree(inc_fullname);

		/* return empty string, it matches nothing */
		return comma_str;
	}
	pfree(inc_fullname);

	/* There is possible recursion here if the file contains @ */
	inc_lines = tokenize_file(inc_file);
	FreeFile(inc_file);

	/* Create comma-separated string from List */
	foreach(line, inc_lines)
	{
		List	   *ln = lfirst(line);
		List	   *token;

		/* First entry is line number */
		foreach(token, lnext(ln))
		{
			int		oldlen = strlen(comma_str);
			int		needed;

			needed = oldlen + strlen(lfirst(token)) + 1;
			if (oldlen > 0)
				needed++;
			comma_str = repalloc(comma_str, needed);
			if (oldlen > 0)
				strcat(comma_str, MULTI_VALUE_SEP);
			strcat(comma_str, lfirst(token));
		}
	}

	free_lines(&inc_lines);

	return comma_str;
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
	char	   *buf;

	while (!feof(file))
	{
		buf = next_token_expand(file);

		/* add token to list, unless we are at EOL or comment start */
		if (buf[0] != '\0')
		{
			if (next_line == NIL)
			{
				/* make a new line List */
				next_line = makeListi1(line_number);
				lines = lappend(lines, next_line);
			}
			/* append token to current line's list */
			next_line = lappend(next_line, buf);
		}
		else
		{
			/* we are at real or logical EOL, so force a new line List */
			next_line = NIL;
			/* Don't forget to pfree the next_token_expand result */
			pfree(buf);
		}

		/* Advance line number whenever we reach EOL */
		if (next_line == NIL)
			line_number++;
	}

	return lines;
}


/*
 * Compare two lines based on their user/group names.
 *
 * Used for qsort() sorting.
 */
static int
user_group_qsort_cmp(const void *list1, const void *list2)
{
	/* first node is line number */
	char	   *user1 = lfirst(lnext(*(List **) list1));
	char	   *user2 = lfirst(lnext(*(List **) list2));

	return strcmp(user1, user2);
}


/*
 * Compare two lines based on their user/group names.
 *
 * Used for bsearch() lookup.
 */
static int
user_group_bsearch_cmp(const void *user, const void *list)
{
	/* first node is line number */
	char	   *user2 = lfirst(lnext(*(List **) list));

	return strcmp(user, user2);
}


/*
 * Lookup a group name in the pg_group file
 */
static List **
get_group_line(const char *group)
{
	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (group_length == 0)
		return NULL;

	return (List **) bsearch((void *) group,
							 (void *) group_sorted,
							 group_length,
							 sizeof(List *),
							 user_group_bsearch_cmp);
}


/*
 * Lookup a user name in the pg_shadow file
 */
List **
get_user_line(const char *user)
{
	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (user_length == 0)
		return NULL;

	return (List **) bsearch((void *) user,
							 (void *) user_sorted,
							 user_length,
							 sizeof(List *),
							 user_group_bsearch_cmp);
}


/*
 * Check group for a specific user.
 */
static bool
check_group(char *group, char *user)
{
	List	  **line,
			   *l;

	if ((line = get_group_line(group)) != NULL)
	{
		foreach(l, lnext(lnext(*line)))
			if (strcmp(lfirst(l), user) == 0)
			return true;
	}

	return false;
}

/*
 * Check comma user list for a specific user, handle group names.
 */
static bool
check_user(char *user, char *param_str)
{
	char	   *tok;

	for (tok = strtok(param_str, MULTI_VALUE_SEP); tok != NULL; tok = strtok(NULL, MULTI_VALUE_SEP))
	{
		if (tok[0] == '+')
		{
			if (check_group(tok + 1, user))
				return true;
		}
		else if (strcmp(tok, user) == 0 ||
				 strcmp(tok, "all") == 0)
			return true;
	}

	return false;
}

/*
 * Check to see if db/user combination matches param string.
 */
static bool
check_db(char *dbname, char *user, char *param_str)
{
	char	   *tok;

	for (tok = strtok(param_str, MULTI_VALUE_SEP); tok != NULL; tok = strtok(NULL, MULTI_VALUE_SEP))
	{
		if (strcmp(tok, "all") == 0)
			return true;
		else if (strcmp(tok, "sameuser") == 0)
		{
			if (strcmp(dbname, user) == 0)
				return true;
		}
		else if (strcmp(tok, "samegroup") == 0)
		{
			if (check_group(dbname, user))
				return true;
		}
		else if (strcmp(tok, dbname) == 0)
			return true;
	}
	return false;
}


/*
 *	Scan the rest of a host record (after the mask field)
 *	and return the interpretation of it as *userauth_p, *auth_arg_p, and
 *	*error_p.  line points to the next token of the line.
 */
static void
parse_hba_auth(List *line, UserAuth *userauth_p, char **auth_arg_p,
			   bool *error_p)
{
	char	   *token;

	*auth_arg_p = NULL;

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
		if (line)
		{
			token = lfirst(line);
			*auth_arg_p = pstrdup(token);
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
 *	return *error_p true, after issuing a message to the log.  If no error,
 *	leave *error_p as it was.
 */
static void
parse_hba(List *line, hbaPort *port, bool *found_p, bool *error_p)
{
	int			line_number;
	char	   *token;
	char	   *db;
	char	   *user;
	struct addrinfo *gai_result;
	struct addrinfo hints;
	int			ret;
	struct sockaddr_storage addr;
	struct sockaddr_storage mask;
	char	   *cidr_slash;

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

		/* Get the user. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		user = lfirst(line);

		line = lnext(line);
		if (!line)
			goto hba_syntax;

		/* Read the rest of the line. */
		parse_hba_auth(line, &port->auth_method, &port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;

		/* Disallow auth methods that always need TCP/IP sockets to work */
		if (port->auth_method == uaKrb4 ||
			port->auth_method == uaKrb5)
			goto hba_syntax;

		/* Does not match if connection isn't AF_UNIX */
		if (!IS_AF_UNIX(port->raddr.addr.ss_family))
			return;
	}
	else if (strcmp(token, "host") == 0
			 || strcmp(token, "hostssl") == 0
			 || strcmp(token, "hostnossl") == 0)
	{

		if (token[4] == 's')	/* "hostssl" */
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
#ifdef USE_SSL
		else if (token[4] == 'n')		/* "hostnossl" */
		{
			/* Record does not match if we are on an SSL connection */
			if (port->ssl)
				return;
		}
#endif

		/* Get the database. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		db = lfirst(line);

		/* Get the user. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		user = lfirst(line);

		/* Read the IP address field. (with or without CIDR netmask) */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		token = lfirst(line);

		/* Check if it has a CIDR suffix and if so isolate it */
		cidr_slash = strchr(token, '/');
		if (cidr_slash)
			*cidr_slash = '\0';

		/* Get the IP address either way */
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = PF_UNSPEC;
		hints.ai_socktype = 0;
		hints.ai_protocol = 0;
		hints.ai_addrlen = 0;
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		ret = getaddrinfo_all(token, NULL, &hints, &gai_result);
		if (ret || !gai_result)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid IP address \"%s\" in pg_hba.conf file: %s",
							token, gai_strerror(ret))));
			if (cidr_slash)
				*cidr_slash = '/';
			if (gai_result)
				freeaddrinfo_all(hints.ai_family, gai_result);
			goto hba_syntax;
		}

		if (cidr_slash)
			*cidr_slash = '/';

		memcpy(&addr, gai_result->ai_addr, gai_result->ai_addrlen);
		freeaddrinfo_all(hints.ai_family, gai_result);

		/* Get the netmask */
		if (cidr_slash)
		{
			if (SockAddr_cidr_mask(&mask, cidr_slash + 1, addr.ss_family) < 0)
				goto hba_syntax;
		}
		else
		{
			/* Read the mask field. */
			line = lnext(line);
			if (!line)
				goto hba_syntax;
			token = lfirst(line);

			ret = getaddrinfo_all(token, NULL, &hints, &gai_result);
			if (ret || !gai_result)
			{
				if (gai_result)
					freeaddrinfo_all(hints.ai_family, gai_result);
				goto hba_syntax;
			}

			memcpy(&mask, gai_result->ai_addr, gai_result->ai_addrlen);
			freeaddrinfo_all(hints.ai_family, gai_result);

			if (addr.ss_family != mask.ss_family)
				goto hba_syntax;
		}

		if (addr.ss_family != port->raddr.addr.ss_family)
		{
			/*
			 * Wrong address family.  We allow only one case: if the
			 * file has IPv4 and the port is IPv6, promote the file
			 * address to IPv6 and try to match that way.
			 */
#ifdef HAVE_IPV6
			if (addr.ss_family == AF_INET &&
				port->raddr.addr.ss_family == AF_INET6)
			{
				promote_v4_to_v6_addr(&addr);
				promote_v4_to_v6_mask(&mask);
			}
			else
#endif /* HAVE_IPV6 */
			{
				/* Line doesn't match client port, so ignore it. */
				return;
			}
		}

		/* Ignore line if client port is not in the matching addr range. */
		if (!rangeSockAddr(&port->raddr.addr, &addr, &mask))
			return;

		/* Read the rest of the line. */
		line = lnext(line);
		if (!line)
			goto hba_syntax;
		parse_hba_auth(line, &port->auth_method, &port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;
	}
	else
		goto hba_syntax;

	if (!check_db(port->database_name, port->user_name, db))
		return;
	if (!check_user(port->user_name, user))
		return;

	/* Success */
	*found_p = true;
	return;

hba_syntax:
	if (line)
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid entry in pg_hba.conf file at line %d, token \"%s\"",
						line_number, (const char *) lfirst(line))));
	else
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
			errmsg("missing field in pg_hba.conf file at end of line %d",
				   line_number)));

	*error_p = true;
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
 * Open the group file if possible (return NULL if not)
 */
static FILE *
group_openfile(void)
{
	char	   *filename;
	FILE	   *groupfile;

	filename = group_getfilename();
	groupfile = AllocateFile(filename, "r");

	if (groupfile == NULL && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));

	pfree(filename);

	return groupfile;
}



/*
 * Open the password file if possible (return NULL if not)
 */
static FILE *
user_openfile(void)
{
	char	   *filename;
	FILE	   *pwdfile;

	filename = user_getfilename();
	pwdfile = AllocateFile(filename, "r");

	if (pwdfile == NULL && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));

	pfree(filename);

	return pwdfile;
}



/*
 *	 Load group/user name mapping file
 */
void
load_group(void)
{
	FILE	   *group_file;
	List	   *line;

	/* Discard any old data */
	if (group_lines)
		free_lines(&group_lines);
	if (group_sorted)
		pfree(group_sorted);
	group_sorted = NULL;
	group_length = 0;

	group_file = group_openfile();
	if (!group_file)
		return;
	group_lines = tokenize_file(group_file);
	FreeFile(group_file);

	/* create sorted lines for binary searching */
	group_length = length(group_lines);
	if (group_length)
	{
		int			i = 0;

		group_sorted = palloc(group_length * sizeof(List *));

		foreach(line, group_lines)
			group_sorted[i++] = lfirst(line);

		qsort((void *) group_sorted,
			  group_length,
			  sizeof(List *),
			  user_group_qsort_cmp);
	}
}


/*
 *	 Load user/password mapping file
 */
void
load_user(void)
{
	FILE	   *user_file;
	List	   *line;

	/* Discard any old data */
	if (user_lines)
		free_lines(&user_lines);
	if (user_sorted)
		pfree(user_sorted);
	user_sorted = NULL;
	user_length = 0;

	user_file = user_openfile();
	if (!user_file)
		return;
	user_lines = tokenize_file(user_file);
	FreeFile(user_file);

	/* create sorted lines for binary searching */
	user_length = length(user_lines);
	if (user_length)
	{
		int			i = 0;

		user_sorted = palloc(user_length * sizeof(List *));

		foreach(line, user_lines)
			user_sorted[i++] = lfirst(line);

		qsort((void *) user_sorted,
			  user_length,
			  sizeof(List *),
			  user_group_qsort_cmp);
	}
}


/*
 * Read the config file and create a List of Lists of tokens in the file.
 * If we find a file by the old name of the config file (pg_hba), we issue
 * an error message because it probably needs to be converted.	He didn't
 * follow directions and just installed his old hba file in the new database
 * system.
 */
void
load_hba(void)
{
	int			bufsize;
	FILE	   *file;			/* The config file we have to read */
	char	   *conf_file;		/* The name of the config file */

	if (hba_lines)
		free_lines(&hba_lines);

	/* Put together the full pathname to the config file. */
	bufsize = (strlen(DataDir) + strlen(CONF_FILE) + 2) * sizeof(char);
	conf_file = (char *) palloc(bufsize);
	snprintf(conf_file, bufsize, "%s/%s", DataDir, CONF_FILE);

	file = AllocateFile(conf_file, "r");
	if (file == NULL)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open configuration file \"%s\": %m",
						conf_file)));

	hba_lines = tokenize_file(file);
	FreeFile(file);
	pfree(conf_file);
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
	if (line)
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid entry in pg_ident.conf file at line %d, token \"%s\"",
						line_number, (const char *) lfirst(line))));
	else
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
		  errmsg("missing entry in pg_ident.conf file at end of line %d",
				 line_number)));

	*error_p = true;
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

	if (usermap_name == NULL || usermap_name[0] == '\0')
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
		errmsg("cannot use Ident authentication without usermap field")));
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
void
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

	file = AllocateFile(map_file, "r");
	if (file == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open Ident usermap file \"%s\": %m",
						map_file)));
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
 *	server.  If it's a normal response indicating a user name, return true
 *	and store the user name at *ident_user. If it's anything else,
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
						/* Rest of line is user name.  Copy it over. */
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
 *	"local_port_addr" on host "local_ip_addr".	Return the user name the
 *	ident server gives as "*ident_user".
 *
 *	IP addresses and port numbers are in network byte order.
 *
 *	But iff we're unable to get the information from ident, return false.
 */
static bool
ident_inet(const SockAddr remote_addr,
		   const SockAddr local_addr,
		   char *ident_user)
{
	int			sock_fd,		/* File descriptor for socket on which we
								 * talk to Ident */
				rc;				/* Return code from a locally called
								 * function */
	bool		ident_return;
	char		remote_addr_s[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	char		local_addr_s[NI_MAXHOST];
	char		local_port[NI_MAXSERV];
	char		ident_port[NI_MAXSERV];
	char		ident_query[80];
	char		ident_response[80 + IDENT_USERNAME_MAX];
	struct addrinfo *ident_serv = NULL,
			   *la = NULL,
				hints;

	/*
	 * Might look a little weird to first convert it to text and then back
	 * to sockaddr, but it's protocol independent.
	 */
	getnameinfo_all(&remote_addr.addr, remote_addr.salen,
					remote_addr_s, sizeof(remote_addr_s),
					remote_port, sizeof(remote_port),
					NI_NUMERICHOST | NI_NUMERICSERV);
	getnameinfo_all(&local_addr.addr, local_addr.salen,
					local_addr_s, sizeof(local_addr_s),
					local_port, sizeof(local_port),
					NI_NUMERICHOST | NI_NUMERICSERV);

	snprintf(ident_port, sizeof(ident_port), "%d", IDENT_PORT);
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = remote_addr.addr.ss_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	rc = getaddrinfo_all(remote_addr_s, ident_port, &hints, &ident_serv);
	if (rc || !ident_serv)
		return false;			/* we don't expect this to happen */

	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = local_addr.addr.ss_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	rc = getaddrinfo_all(local_addr_s, NULL, &hints, &la);
	if (rc || !la)
		return false;			/* we don't expect this to happen */

	sock_fd = socket(ident_serv->ai_family, ident_serv->ai_socktype,
					 ident_serv->ai_protocol);
	if (sock_fd < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
			errmsg("could not create socket for Ident connection: %m")));
		ident_return = false;
		goto ident_inet_done;
	}

	/*
	 * Bind to the address which the client originally contacted,
	 * otherwise the ident server won't be able to match up the right
	 * connection. This is necessary if the PostgreSQL server is running
	 * on an IP alias.
	 */
	rc = bind(sock_fd, la->ai_addr, la->ai_addrlen);
	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not bind to local address \"%s\": %m",
						local_addr_s)));
		ident_return = false;
		goto ident_inet_done;
	}

	rc = connect(sock_fd, ident_serv->ai_addr,
				 ident_serv->ai_addrlen);
	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not connect to Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	/* The query we send to the Ident server */
	snprintf(ident_query, sizeof(ident_query), "%s,%s\r\n",
			 remote_port, local_port);

	/* loop in case send is interrupted */
	do
	{
		rc = send(sock_fd, ident_query, strlen(ident_query), 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not send query to Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	do
	{
		rc = recv(sock_fd, ident_response, sizeof(ident_response) - 1, 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not receive response from Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	ident_response[rc] = '\0';
	ident_return = interpret_ident_response(ident_response, ident_user);

ident_inet_done:
	if (sock_fd >= 0)
		closesocket(sock_fd);
	freeaddrinfo_all(remote_addr.addr.ss_family, ident_serv);
	freeaddrinfo_all(local_addr.addr.ss_family, la);
	return ident_return;
}

/*
 *	Ask kernel about the credentials of the connecting process and
 *	determine the symbolic name of the corresponding user.
 *
 *	Returns either true and the username put into "ident_user",
 *	or false if we were unable to determine the username.
 */
#ifdef HAVE_UNIX_SOCKETS

static bool
ident_unix(int sock, char *ident_user)
{
#if defined(HAVE_GETPEEREID)
	/* OpenBSD style:  */
	uid_t		uid;
	gid_t		gid;
	struct passwd *pass;

	errno = 0;
	if (getpeereid(sock, &uid, &gid) != 0)
	{
		/* We didn't get a valid credentials struct. */
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not get peer credentials: %m")));
		return false;
	}

	pass = getpwuid(uid);

	if (pass == NULL)
	{
		ereport(LOG,
				(errmsg("local user with ID %d does not exist",
						(int) uid)));
		return false;
	}

	StrNCpy(ident_user, pass->pw_name, IDENT_USERNAME_MAX + 1);

	return true;

#elif defined(SO_PEERCRED)
	/* Linux style: use getsockopt(SO_PEERCRED) */
	struct ucred peercred;
	ACCEPT_TYPE_ARG3 so_len = sizeof(peercred);
	struct passwd *pass;

	errno = 0;
	if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &peercred, &so_len) != 0 ||
		so_len != sizeof(peercred))
	{
		/* We didn't get a valid credentials struct. */
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not get peer credentials: %m")));
		return false;
	}

	pass = getpwuid(peercred.uid);

	if (pass == NULL)
	{
		ereport(LOG,
				(errmsg("local user with ID %d does not exist",
						(int) peercred.uid)));
		return false;
	}

	StrNCpy(ident_user, pass->pw_name, IDENT_USERNAME_MAX + 1);

	return true;

#elif defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || (defined(HAVE_STRUCT_SOCKCRED) && defined(LOCAL_CREDS))
	struct msghdr msg;

/* Credentials structure */
#if defined(HAVE_STRUCT_CMSGCRED)
	typedef struct cmsgcred Cred;

#define cruid cmcred_uid
#elif defined(HAVE_STRUCT_FCRED)
	typedef struct fcred Cred;

#define cruid fc_uid
#elif defined(HAVE_STRUCT_SOCKCRED)
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
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not get peer credentials: %m")));
		return false;
	}

	cred = (Cred *) CMSG_DATA(cmsg);

	pw = getpwuid(cred->cruid);

	if (pw == NULL)
	{
		ereport(LOG,
				(errmsg("local user with ID %d does not exist",
						(int) cred->cruid)));
		return false;
	}

	StrNCpy(ident_user, pw->pw_name, IDENT_USERNAME_MAX + 1);

	return true;

#else
	ereport(LOG,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Ident authentication is not supported on local connections on this platform")));

	return false;
#endif
}
#endif   /* HAVE_UNIX_SOCKETS */


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

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
#ifdef	HAVE_IPV6
		case AF_INET6:
#endif
			if (!ident_inet(port->raddr, port->laddr, ident_user))
				return STATUS_ERROR;
			break;

#ifdef HAVE_UNIX_SOCKETS
		case AF_UNIX:
			if (!ident_unix(port->sock, ident_user))
				return STATUS_ERROR;
			break;
#endif

		default:
			return STATUS_ERROR;
	}

	if (check_ident_usermap(port->auth_arg, port->user_name, ident_user))
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
