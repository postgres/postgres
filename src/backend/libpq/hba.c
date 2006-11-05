/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and possibly using ident).
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/libpq/hba.c,v 1.157 2006/11/05 22:42:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
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

#include "libpq/ip.h"
#include "libpq/libpq.h"
#include "storage/fd.h"
#include "utils/flatfiles.h"
#include "utils/guc.h"



#define atooid(x)  ((Oid) strtoul((x), NULL, 10))
#define atoxid(x)  ((TransactionId) strtoul((x), NULL, 10))

/* Max size of username ident server can return */
#define IDENT_USERNAME_MAX 512

/* Standard TCP port number for Ident service.	Assigned by IANA */
#define IDENT_PORT 113

/* This is used to separate values in multi-valued column strings */
#define MULTI_VALUE_SEP "\001"

#define MAX_TOKEN	256

/*
 * These variables hold the pre-parsed contents of the hba and ident
 * configuration files, as well as the flat auth file.
 * Each is a list of sublists, one sublist for
 * each (non-empty, non-comment) line of the file.	Each sublist's
 * first item is an integer line number (so we can give somewhat-useful
 * location info in error messages).  Remaining items are palloc'd strings,
 * one string per token on the line.  Note there will always be at least
 * one token, since blank lines are not entered in the data structure.
 */

/* pre-parsed content of HBA config file and corresponding line #s */
static List *hba_lines = NIL;
static List *hba_line_nums = NIL;

/* pre-parsed content of ident usermap file and corresponding line #s */
static List *ident_lines = NIL;
static List *ident_line_nums = NIL;

/* pre-parsed content of flat auth file and corresponding line #s */
static List *role_lines = NIL;
static List *role_line_nums = NIL;

/* sorted entries so we can do binary search lookups */
static List **role_sorted = NULL;		/* sorted role list, for bsearch() */
static int	role_length;

static void tokenize_file(const char *filename, FILE *file,
			  List **lines, List **line_nums);
static char *tokenize_inc_file(const char *outer_filename,
				  const char *inc_filename);

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
 * Grab one token out of fp. Tokens are strings of non-blank
 * characters bounded by blank characters, commas, beginning of line, and
 * end of line. Blank means space or tab. Tokens can be delimited by
 * double quotes (and usually are, in current usage).
 *
 * The token, if any, is returned at *buf (a buffer of size bufsz).
 *
 * If successful: store null-terminated token at *buf and return TRUE.
 * If no more tokens on line: set *buf = '\0' and return FALSE.
 *
 * Leave file positioned at the character immediately after the token or EOF,
 * whichever comes first. If no more tokens on line, position the file to the
 * beginning of the next line or EOF, whichever comes first.
 *
 * Handle comments. Treat unquoted keywords that might be role names or
 * database names specially, by appending a newline to them.
 */
static bool
next_token(FILE *fp, char *buf, int bufsz)
{
	int			c;
	char	   *start_buf = buf;
	char	   *end_buf = buf + (bufsz - 2);
	bool		in_quote = false;
	bool		was_quote = false;
	bool		saw_quote = false;

	Assert(end_buf > start_buf);

	/* Move over initial whitespace and commas */
	while ((c = getc(fp)) != EOF && (pg_isblank(c) || c == ','))
		;

	if (c == EOF || c == '\n')
	{
		*buf = '\0';
		return false;
	}

	/*
	 * Build a token in buf of next characters up to EOF, EOL, unquoted comma,
	 * or unquoted whitespace.
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
		{
			in_quote = !in_quote;
			saw_quote = true;
		}

		c = getc(fp);
	}

	/*
	 * Put back the char right after the token (critical in case it is EOL,
	 * since we need to detect end-of-line at next call).
	 */
	if (c != EOF)
		ungetc(c, fp);

	*buf = '\0';

	if (!saw_quote &&
		(strcmp(start_buf, "all") == 0 ||
		 strcmp(start_buf, "sameuser") == 0 ||
		 strcmp(start_buf, "samegroup") == 0 ||
		 strcmp(start_buf, "samerole") == 0))
	{
		/* append newline to a magical keyword */
		*buf++ = '\n';
		*buf = '\0';
	}

	return (saw_quote || buf > start_buf);
}

/*
 *	 Tokenize file and handle file inclusion and comma lists. We have
 *	 to  break	apart  the	commas	to	expand	any  file names then
 *	 reconstruct with commas.
 *
 * The result is a palloc'd string, or NULL if we have reached EOL.
 */
static char *
next_token_expand(const char *filename, FILE *file)
{
	char		buf[MAX_TOKEN];
	char	   *comma_str = pstrdup("");
	bool		got_something = false;
	bool		trailing_comma;
	char	   *incbuf;
	int			needed;

	do
	{
		if (!next_token(file, buf, sizeof(buf)))
			break;

		got_something = true;

		if (strlen(buf) > 0 && buf[strlen(buf) - 1] == ',')
		{
			trailing_comma = true;
			buf[strlen(buf) - 1] = '\0';
		}
		else
			trailing_comma = false;

		/* Is this referencing a file? */
		if (buf[0] == '@')
			incbuf = tokenize_inc_file(filename, buf + 1);
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

	if (!got_something)
	{
		pfree(comma_str);
		return NULL;
	}

	return comma_str;
}


/*
 * Free memory used by lines/tokens (i.e., structure built by tokenize_file)
 */
static void
free_lines(List **lines, List **line_nums)
{
	/*
	 * Either both must be non-NULL, or both must be NULL
	 */
	Assert((*lines != NIL && *line_nums != NIL) ||
		   (*lines == NIL && *line_nums == NIL));

	if (*lines)
	{
		/*
		 * "lines" is a list of lists; each of those sublists consists of
		 * palloc'ed tokens, so we want to free each pointed-to token in a
		 * sublist, followed by the sublist itself, and finally the whole
		 * list.
		 */
		ListCell   *line;

		foreach(line, *lines)
		{
			List	   *ln = lfirst(line);
			ListCell   *token;

			foreach(token, ln)
				pfree(lfirst(token));
			/* free the sublist structure itself */
			list_free(ln);
		}
		/* free the list structure itself */
		list_free(*lines);
		/* clear the static variable */
		*lines = NIL;
	}

	if (*line_nums)
	{
		list_free(*line_nums);
		*line_nums = NIL;
	}
}


static char *
tokenize_inc_file(const char *outer_filename,
				  const char *inc_filename)
{
	char	   *inc_fullname;
	FILE	   *inc_file;
	List	   *inc_lines;
	List	   *inc_line_nums;
	ListCell   *line;
	char	   *comma_str;

	if (is_absolute_path(inc_filename))
	{
		/* absolute path is taken as-is */
		inc_fullname = pstrdup(inc_filename);
	}
	else
	{
		/* relative path is relative to dir of calling file */
		inc_fullname = (char *) palloc(strlen(outer_filename) + 1 +
									   strlen(inc_filename) + 1);
		strcpy(inc_fullname, outer_filename);
		get_parent_directory(inc_fullname);
		join_path_components(inc_fullname, inc_fullname, inc_filename);
		canonicalize_path(inc_fullname);
	}

	inc_file = AllocateFile(inc_fullname, "r");
	if (inc_file == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open secondary authentication file \"@%s\" as \"%s\": %m",
						inc_filename, inc_fullname)));
		pfree(inc_fullname);

		/* return single space, it matches nothing */
		return pstrdup(" ");
	}

	/* There is possible recursion here if the file contains @ */
	tokenize_file(inc_fullname, inc_file, &inc_lines, &inc_line_nums);

	FreeFile(inc_file);
	pfree(inc_fullname);

	/* Create comma-separated string from List */
	comma_str = pstrdup("");
	foreach(line, inc_lines)
	{
		List	   *token_list = (List *) lfirst(line);
		ListCell   *token;

		foreach(token, token_list)
		{
			int			oldlen = strlen(comma_str);
			int			needed;

			needed = oldlen + strlen(lfirst(token)) + 1;
			if (oldlen > 0)
				needed++;
			comma_str = repalloc(comma_str, needed);
			if (oldlen > 0)
				strcat(comma_str, MULTI_VALUE_SEP);
			strcat(comma_str, lfirst(token));
		}
	}

	free_lines(&inc_lines, &inc_line_nums);

	/* if file is empty, return single space rather than empty string */
	if (strlen(comma_str) == 0)
	{
		pfree(comma_str);
		return pstrdup(" ");
	}

	return comma_str;
}


/*
 * Tokenize the given file, storing the resulting data into two lists:
 * a list of sublists, each sublist containing the tokens in a line of
 * the file, and a list of line numbers.
 *
 * filename must be the absolute path to the target file.
 */
static void
tokenize_file(const char *filename, FILE *file,
			  List **lines, List **line_nums)
{
	List	   *current_line = NIL;
	int			line_number = 1;
	char	   *buf;

	*lines = *line_nums = NIL;

	while (!feof(file))
	{
		buf = next_token_expand(filename, file);

		/* add token to list, unless we are at EOL or comment start */
		if (buf)
		{
			if (current_line == NIL)
			{
				/* make a new line List, record its line number */
				current_line = lappend(current_line, buf);
				*lines = lappend(*lines, current_line);
				*line_nums = lappend_int(*line_nums, line_number);
			}
			else
			{
				/* append token to current line's list */
				current_line = lappend(current_line, buf);
			}
		}
		else
		{
			/* we are at real or logical EOL, so force a new line List */
			current_line = NIL;
			/* Advance line number whenever we reach EOL */
			line_number++;
		}
	}
}

/*
 * Compare two lines based on their role/member names.
 *
 * Used for bsearch() lookup.
 */
static int
role_bsearch_cmp(const void *role, const void *list)
{
	char	   *role2 = linitial(*(List **) list);

	return strcmp(role, role2);
}


/*
 * Lookup a role name in the pg_auth file
 */
List	  **
get_role_line(const char *role)
{
	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (role_length == 0)
		return NULL;

	return (List **) bsearch((void *) role,
							 (void *) role_sorted,
							 role_length,
							 sizeof(List *),
							 role_bsearch_cmp);
}


/*
 * Does user belong to role?
 *
 * user is always the name given as the attempted login identifier.
 * We check to see if it is a member of the specified role name.
 */
static bool
is_member(const char *user, const char *role)
{
	List	  **line;
	ListCell   *line_item;

	if ((line = get_role_line(user)) == NULL)
		return false;			/* if user not exist, say "no" */

	/* A user always belongs to its own role */
	if (strcmp(user, role) == 0)
		return true;

	/*
	 * skip over the role name, password, valuntil, examine all the membership
	 * entries
	 */
	if (list_length(*line) < 4)
		return false;
	for_each_cell(line_item, lnext(lnext(lnext(list_head(*line)))))
	{
		if (strcmp((char *) lfirst(line_item), role) == 0)
			return true;
	}

	return false;
}

/*
 * Check comma-separated list for a match to role, allowing group names.
 *
 * NB: param_str is destructively modified!  In current usage, this is
 * okay only because this code is run after forking off from the postmaster,
 * and so it doesn't matter that we clobber the stored hba info.
 */
static bool
check_role(const char *role, char *param_str)
{
	char	   *tok;

	for (tok = strtok(param_str, MULTI_VALUE_SEP);
		 tok != NULL;
		 tok = strtok(NULL, MULTI_VALUE_SEP))
	{
		if (tok[0] == '+')
		{
			if (is_member(role, tok + 1))
				return true;
		}
		else if (strcmp(tok, role) == 0 ||
				 strcmp(tok, "all\n") == 0)
			return true;
	}

	return false;
}

/*
 * Check to see if db/role combination matches param string.
 *
 * NB: param_str is destructively modified!  In current usage, this is
 * okay only because this code is run after forking off from the postmaster,
 * and so it doesn't matter that we clobber the stored hba info.
 */
static bool
check_db(const char *dbname, const char *role, char *param_str)
{
	char	   *tok;

	for (tok = strtok(param_str, MULTI_VALUE_SEP);
		 tok != NULL;
		 tok = strtok(NULL, MULTI_VALUE_SEP))
	{
		if (strcmp(tok, "all\n") == 0)
			return true;
		else if (strcmp(tok, "sameuser\n") == 0)
		{
			if (strcmp(dbname, role) == 0)
				return true;
		}
		else if (strcmp(tok, "samegroup\n") == 0 ||
				 strcmp(tok, "samerole\n") == 0)
		{
			if (is_member(role, dbname))
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
 *	*error_p.  *line_item points to the next token of the line, and is
 *	advanced over successfully-read tokens.
 */
static void
parse_hba_auth(ListCell **line_item, UserAuth *userauth_p,
			   char **auth_arg_p, bool *error_p)
{
	char	   *token;

	*auth_arg_p = NULL;

	if (!*line_item)
	{
		*error_p = true;
		return;
	}

	token = lfirst(*line_item);
	if (strcmp(token, "trust") == 0)
		*userauth_p = uaTrust;
	else if (strcmp(token, "ident") == 0)
		*userauth_p = uaIdent;
	else if (strcmp(token, "password") == 0)
		*userauth_p = uaPassword;
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
#ifdef USE_LDAP
	else if (strcmp(token, "ldap") == 0)
		*userauth_p = uaLDAP;
#endif
	else
	{
		*error_p = true;
		return;
	}
	*line_item = lnext(*line_item);

	/* Get the authentication argument token, if any */
	if (*line_item)
	{
		token = lfirst(*line_item);
		*auth_arg_p = pstrdup(token);
		*line_item = lnext(*line_item);
		/* If there is more on the line, it is an error */
		if (*line_item)
			*error_p = true;
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
parse_hba(List *line, int line_num, hbaPort *port,
		  bool *found_p, bool *error_p)
{
	char	   *token;
	char	   *db;
	char	   *role;
	struct addrinfo *gai_result;
	struct addrinfo hints;
	int			ret;
	struct sockaddr_storage addr;
	struct sockaddr_storage mask;
	char	   *cidr_slash;
	ListCell   *line_item;

	line_item = list_head(line);
	/* Check the record type. */
	token = lfirst(line_item);
	if (strcmp(token, "local") == 0)
	{
		/* Get the database. */
		line_item = lnext(line_item);
		if (!line_item)
			goto hba_syntax;
		db = lfirst(line_item);

		/* Get the role. */
		line_item = lnext(line_item);
		if (!line_item)
			goto hba_syntax;
		role = lfirst(line_item);

		line_item = lnext(line_item);
		if (!line_item)
			goto hba_syntax;

		/* Read the rest of the line. */
		parse_hba_auth(&line_item, &port->auth_method,
					   &port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;

		/* Disallow auth methods that always need TCP/IP sockets to work */
		if (port->auth_method == uaKrb5)
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
		line_item = lnext(line_item);
		if (!line_item)
			goto hba_syntax;
		db = lfirst(line_item);

		/* Get the role. */
		line_item = lnext(line_item);
		if (!line_item)
			goto hba_syntax;
		role = lfirst(line_item);

		/* Read the IP address field. (with or without CIDR netmask) */
		line_item = lnext(line_item);
		if (!line_item)
			goto hba_syntax;
		token = lfirst(line_item);

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

		ret = pg_getaddrinfo_all(token, NULL, &hints, &gai_result);
		if (ret || !gai_result)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
			   errmsg("invalid IP address \"%s\" in file \"%s\" line %d: %s",
					  token, HbaFileName, line_num,
					  gai_strerror(ret))));
			if (cidr_slash)
				*cidr_slash = '/';
			if (gai_result)
				pg_freeaddrinfo_all(hints.ai_family, gai_result);
			goto hba_other_error;
		}

		if (cidr_slash)
			*cidr_slash = '/';

		memcpy(&addr, gai_result->ai_addr, gai_result->ai_addrlen);
		pg_freeaddrinfo_all(hints.ai_family, gai_result);

		/* Get the netmask */
		if (cidr_slash)
		{
			if (pg_sockaddr_cidr_mask(&mask, cidr_slash + 1,
									  addr.ss_family) < 0)
				goto hba_syntax;
		}
		else
		{
			/* Read the mask field. */
			line_item = lnext(line_item);
			if (!line_item)
				goto hba_syntax;
			token = lfirst(line_item);

			ret = pg_getaddrinfo_all(token, NULL, &hints, &gai_result);
			if (ret || !gai_result)
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
				  errmsg("invalid IP mask \"%s\" in file \"%s\" line %d: %s",
						 token, HbaFileName, line_num,
						 gai_strerror(ret))));
				if (gai_result)
					pg_freeaddrinfo_all(hints.ai_family, gai_result);
				goto hba_other_error;
			}

			memcpy(&mask, gai_result->ai_addr, gai_result->ai_addrlen);
			pg_freeaddrinfo_all(hints.ai_family, gai_result);

			if (addr.ss_family != mask.ss_family)
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("IP address and mask do not match in file \"%s\" line %d",
								HbaFileName, line_num)));
				goto hba_other_error;
			}
		}

		if (addr.ss_family != port->raddr.addr.ss_family)
		{
			/*
			 * Wrong address family.  We allow only one case: if the file has
			 * IPv4 and the port is IPv6, promote the file address to IPv6 and
			 * try to match that way.
			 */
#ifdef HAVE_IPV6
			if (addr.ss_family == AF_INET &&
				port->raddr.addr.ss_family == AF_INET6)
			{
				pg_promote_v4_to_v6_addr(&addr);
				pg_promote_v4_to_v6_mask(&mask);
			}
			else
#endif   /* HAVE_IPV6 */
			{
				/* Line doesn't match client port, so ignore it. */
				return;
			}
		}

		/* Ignore line if client port is not in the matching addr range. */
		if (!pg_range_sockaddr(&port->raddr.addr, &addr, &mask))
			return;

		/* Read the rest of the line. */
		line_item = lnext(line_item);
		if (!line_item)
			goto hba_syntax;
		parse_hba_auth(&line_item, &port->auth_method,
					   &port->auth_arg, error_p);
		if (*error_p)
			goto hba_syntax;
	}
	else
		goto hba_syntax;

	/* Does the entry match database and role? */
	if (!check_db(port->database_name, port->user_name, db))
		return;
	if (!check_role(port->user_name, role))
		return;

	/* Success */
	*found_p = true;
	return;

hba_syntax:
	if (line_item)
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
			  errmsg("invalid entry in file \"%s\" at line %d, token \"%s\"",
					 HbaFileName, line_num,
					 (char *) lfirst(line_item))));
	else
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("missing field in file \"%s\" at end of line %d",
						HbaFileName, line_num)));

	/* Come here if suitable message already logged */
hba_other_error:
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
	ListCell   *line;
	ListCell   *line_num;

	forboth(line, hba_lines, line_num, hba_line_nums)
	{
		parse_hba(lfirst(line), lfirst_int(line_num),
				  port, &found_entry, &error);
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
 *	 Load role/password mapping file
 */
void
load_role(void)
{
	char	   *filename;
	FILE	   *role_file;

	/* Discard any old data */
	if (role_lines || role_line_nums)
		free_lines(&role_lines, &role_line_nums);
	if (role_sorted)
		pfree(role_sorted);
	role_sorted = NULL;
	role_length = 0;

	/* Read in the file contents */
	filename = auth_getflatfilename();
	role_file = AllocateFile(filename, "r");

	if (role_file == NULL)
	{
		/* no complaint if not there */
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", filename)));
		pfree(filename);
		return;
	}

	tokenize_file(filename, role_file, &role_lines, &role_line_nums);

	FreeFile(role_file);
	pfree(filename);

	/* create array for binary searching */
	role_length = list_length(role_lines);
	if (role_length)
	{
		int			i = 0;
		ListCell   *line;

		/* We assume the flat file was written already-sorted */
		role_sorted = palloc(role_length * sizeof(List *));
		foreach(line, role_lines)
			role_sorted[i++] = lfirst(line);
	}
}


/*
 * Read the config file and create a List of Lists of tokens in the file.
 */
void
load_hba(void)
{
	FILE	   *file;

	if (hba_lines || hba_line_nums)
		free_lines(&hba_lines, &hba_line_nums);

	file = AllocateFile(HbaFileName, "r");
	/* Failure is fatal since with no HBA entries we can do nothing... */
	if (file == NULL)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open configuration file \"%s\": %m",
						HbaFileName)));

	tokenize_file(HbaFileName, file, &hba_lines, &hba_line_nums);
	FreeFile(file);
}

/*
 * Read and parse one line from the flat pg_database file.
 *
 * Returns TRUE on success, FALSE if EOF; bad data causes elog(FATAL).
 *
 * Output parameters:
 *	dbname: gets database name (must be of size NAMEDATALEN bytes)
 *	dboid: gets database OID
 *	dbtablespace: gets database's default tablespace's OID
 *	dbfrozenxid: gets database's frozen XID
 *
 * This is not much related to the other functions in hba.c, but we put it
 * here because it uses the next_token() infrastructure.
 */
bool
read_pg_database_line(FILE *fp, char *dbname, Oid *dboid,
					  Oid *dbtablespace, TransactionId *dbfrozenxid)
{
	char		buf[MAX_TOKEN];

	if (feof(fp))
		return false;
	if (!next_token(fp, buf, sizeof(buf)))
		return false;
	if (strlen(buf) >= NAMEDATALEN)
		elog(FATAL, "bad data in flat pg_database file");
	strcpy(dbname, buf);
	next_token(fp, buf, sizeof(buf));
	if (!isdigit((unsigned char) buf[0]))
		elog(FATAL, "bad data in flat pg_database file");
	*dboid = atooid(buf);
	next_token(fp, buf, sizeof(buf));
	if (!isdigit((unsigned char) buf[0]))
		elog(FATAL, "bad data in flat pg_database file");
	*dbtablespace = atooid(buf);
	next_token(fp, buf, sizeof(buf));
	if (!isdigit((unsigned char) buf[0]))
		elog(FATAL, "bad data in flat pg_database file");
	*dbfrozenxid = atoxid(buf);
	/* expect EOL next */
	if (next_token(fp, buf, sizeof(buf)))
		elog(FATAL, "bad data in flat pg_database file");
	return true;
}

/*
 *	Process one line from the ident config file.
 *
 *	Take the line and compare it to the needed map, pg_role and ident_user.
 *	*found_p and *error_p are set according to our results.
 */
static void
parse_ident_usermap(List *line, int line_number, const char *usermap_name,
					const char *pg_role, const char *ident_user,
					bool *found_p, bool *error_p)
{
	ListCell   *line_item;
	char	   *token;
	char	   *file_map;
	char	   *file_pgrole;
	char	   *file_ident_user;

	*found_p = false;
	*error_p = false;

	Assert(line != NIL);
	line_item = list_head(line);

	/* Get the map token (must exist) */
	token = lfirst(line_item);
	file_map = token;

	/* Get the ident user token */
	line_item = lnext(line_item);
	if (!line_item)
		goto ident_syntax;
	token = lfirst(line_item);
	file_ident_user = token;

	/* Get the PG rolename token */
	line_item = lnext(line_item);
	if (!line_item)
		goto ident_syntax;
	token = lfirst(line_item);
	file_pgrole = token;

	/* Match? */
	if (strcmp(file_map, usermap_name) == 0 &&
		strcmp(file_pgrole, pg_role) == 0 &&
		strcmp(file_ident_user, ident_user) == 0)
		*found_p = true;

	return;

ident_syntax:
	ereport(LOG,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg("missing entry in file \"%s\" at end of line %d",
					IdentFileName, line_number)));
	*error_p = true;
}


/*
 *	Scan the (pre-parsed) ident usermap file line by line, looking for a match
 *
 *	See if the user with ident username "ident_user" is allowed to act
 *	as Postgres user "pgrole" according to usermap "usermap_name".
 *
 *	Special case: For usermap "samerole", don't look in the usermap
 *	file.  That's an implied map where "pgrole" must be identical to
 *	"ident_user" in order to be authorized.
 *
 *	Iff authorized, return true.
 */
static bool
check_ident_usermap(const char *usermap_name,
					const char *pg_role,
					const char *ident_user)
{
	bool		found_entry = false,
				error = false;

	if (usermap_name == NULL || usermap_name[0] == '\0')
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
		   errmsg("cannot use Ident authentication without usermap field")));
		found_entry = false;
	}
	else if (strcmp(usermap_name, "sameuser\n") == 0 ||
			 strcmp(usermap_name, "samerole\n") == 0)
	{
		if (strcmp(pg_role, ident_user) == 0)
			found_entry = true;
		else
			found_entry = false;
	}
	else
	{
		ListCell   *line_cell,
				   *num_cell;

		forboth(line_cell, ident_lines, num_cell, ident_line_nums)
		{
			parse_ident_usermap(lfirst(line_cell), lfirst_int(num_cell),
								usermap_name, pg_role, ident_user,
								&found_entry, &error);
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
	FILE	   *file;

	if (ident_lines || ident_line_nums)
		free_lines(&ident_lines, &ident_line_nums);

	file = AllocateFile(IdentFileName, "r");
	if (file == NULL)
	{
		/* not fatal ... we just won't do any special ident maps */
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open Ident usermap file \"%s\": %m",
						IdentFileName)));
	}
	else
	{
		tokenize_file(IdentFileName, file, &ident_lines, &ident_line_nums);
		FreeFile(file);
	}
}


/*
 *	Parse the string "*ident_response" as a response from a query to an Ident
 *	server.  If it's a normal response indicating a user name, return true
 *	and store the user name at *ident_user. If it's anything else,
 *	return false.
 */
static bool
interpret_ident_response(const char *ident_response,
						 char *ident_user)
{
	const char *cursor = ident_response;		/* Cursor into *ident_response */

	/*
	 * Ident's response, in the telnet tradition, should end in crlf (\r\n).
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
				 * It's a USERID response.  Good.  "cursor" should be pointing
				 * to the colon that precedes the operating system type.
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
	int			sock_fd,		/* File descriptor for socket on which we talk
								 * to Ident */
				rc;				/* Return code from a locally called function */
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
	 * Might look a little weird to first convert it to text and then back to
	 * sockaddr, but it's protocol independent.
	 */
	pg_getnameinfo_all(&remote_addr.addr, remote_addr.salen,
					   remote_addr_s, sizeof(remote_addr_s),
					   remote_port, sizeof(remote_port),
					   NI_NUMERICHOST | NI_NUMERICSERV);
	pg_getnameinfo_all(&local_addr.addr, local_addr.salen,
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
	rc = pg_getaddrinfo_all(remote_addr_s, ident_port, &hints, &ident_serv);
	if (rc || !ident_serv)
	{
		if (ident_serv)
			pg_freeaddrinfo_all(hints.ai_family, ident_serv);
		return false;			/* we don't expect this to happen */
	}

	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = local_addr.addr.ss_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	rc = pg_getaddrinfo_all(local_addr_s, NULL, &hints, &la);
	if (rc || !la)
	{
		if (la)
			pg_freeaddrinfo_all(hints.ai_family, la);
		return false;			/* we don't expect this to happen */
	}

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
	 * Bind to the address which the client originally contacted, otherwise
	 * the ident server won't be able to match up the right connection. This
	 * is necessary if the PostgreSQL server is running on an IP alias.
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
	if (!ident_return)
		ereport(LOG,
			(errmsg("invalidly formatted response from Ident server: \"%s\"",
					ident_response)));

ident_inet_done:
	if (sock_fd >= 0)
		closesocket(sock_fd);
	pg_freeaddrinfo_all(remote_addr.addr.ss_family, ident_serv);
	pg_freeaddrinfo_all(local_addr.addr.ss_family, la);
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
	 * purposes is only to make sure that recvmsg() blocks long enough for the
	 * other side to send its credentials.
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

	ereport(DEBUG2,
			(errmsg("Ident protocol identifies remote user as \"%s\"",
					ident_user)));

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
