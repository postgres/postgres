/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and choosing authentication method based on it).
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/hba.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "common/string.h"
#include "funcapi.h"
#include "libpq/ifaddr.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "regex/regex.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

#ifdef USE_LDAP
#ifdef WIN32
#include <winldap.h>
#else
#include <ldap.h>
#endif
#endif


#define MAX_TOKEN	256

/* callback data for check_network_callback */
typedef struct check_network_data
{
	IPCompareMethod method;		/* test method */
	SockAddr   *raddr;			/* client's actual address */
	bool		result;			/* set to true if match */
} check_network_data;


#define token_is_keyword(t, k)	(!t->quoted && strcmp(t->string, k) == 0)
#define token_matches(t, k)  (strcmp(t->string, k) == 0)

/*
 * pre-parsed content of HBA config file: list of HbaLine structs.
 * parsed_hba_context is the memory context where it lives.
 */
static List *parsed_hba_lines = NIL;
static MemoryContext parsed_hba_context = NULL;

/*
 * pre-parsed content of ident mapping file: list of IdentLine structs.
 * parsed_ident_context is the memory context where it lives.
 *
 * NOTE: the IdentLine structs can contain pre-compiled regular expressions
 * that live outside the memory context. Before destroying or resetting the
 * memory context, they need to be explicitly free'd.
 */
static List *parsed_ident_lines = NIL;
static MemoryContext parsed_ident_context = NULL;

/*
 * The following character array represents the names of the authentication
 * methods that are supported by PostgreSQL.
 *
 * Note: keep this in sync with the UserAuth enum in hba.h.
 */
static const char *const UserAuthName[] =
{
	"reject",
	"implicit reject",			/* Not a user-visible option */
	"trust",
	"ident",
	"password",
	"md5",
	"scram-sha-256",
	"gss",
	"sspi",
	"pam",
	"bsd",
	"ldap",
	"cert",
	"radius",
	"peer"
};


static List *tokenize_inc_file(List *tokens, const char *outer_filename,
							   const char *inc_filename, int elevel, char **err_msg);
static bool parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline,
							   int elevel, char **err_msg);


/*
 * isblank() exists in the ISO C99 spec, but it's not very portable yet,
 * so provide our own version.
 */
bool
pg_isblank(const char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}


/*
 * Grab one token out of the string pointed to by *lineptr.
 *
 * Tokens are strings of non-blank characters bounded by blank characters,
 * commas, beginning of line, and end of line.  Blank means space or tab.
 *
 * Tokens can be delimited by double quotes (this allows the inclusion of
 * blanks or '#', but not newlines).  As in SQL, write two double-quotes
 * to represent a double quote.
 *
 * Comments (started by an unquoted '#') are skipped, i.e. the remainder
 * of the line is ignored.
 *
 * (Note that line continuation processing happens before tokenization.
 * Thus, if a continuation occurs within quoted text or a comment, the
 * quoted text or comment is considered to continue to the next line.)
 *
 * The token, if any, is returned at *buf (a buffer of size bufsz), and
 * *lineptr is advanced past the token.
 *
 * Also, we set *initial_quote to indicate whether there was quoting before
 * the first character.  (We use that to prevent "@x" from being treated
 * as a file inclusion request.  Note that @"x" should be so treated;
 * we want to allow that to support embedded spaces in file paths.)
 *
 * We set *terminating_comma to indicate whether the token is terminated by a
 * comma (which is not returned).
 *
 * In event of an error, log a message at ereport level elevel, and also
 * set *err_msg to a string describing the error.  Currently the only
 * possible error is token too long for buf.
 *
 * If successful: store null-terminated token at *buf and return true.
 * If no more tokens on line: set *buf = '\0' and return false.
 * If error: fill buf with truncated or misformatted token and return false.
 */
static bool
next_token(char **lineptr, char *buf, int bufsz,
		   bool *initial_quote, bool *terminating_comma,
		   int elevel, char **err_msg)
{
	int			c;
	char	   *start_buf = buf;
	char	   *end_buf = buf + (bufsz - 1);
	bool		in_quote = false;
	bool		was_quote = false;
	bool		saw_quote = false;

	Assert(end_buf > start_buf);

	*initial_quote = false;
	*terminating_comma = false;

	/* Move over any whitespace and commas preceding the next token */
	while ((c = (*(*lineptr)++)) != '\0' && (pg_isblank(c) || c == ','))
		;

	/*
	 * Build a token in buf of next characters up to EOL, unquoted comma, or
	 * unquoted whitespace.
	 */
	while (c != '\0' &&
		   (!pg_isblank(c) || in_quote))
	{
		/* skip comments to EOL */
		if (c == '#' && !in_quote)
		{
			while ((c = (*(*lineptr)++)) != '\0')
				;
			break;
		}

		if (buf >= end_buf)
		{
			*buf = '\0';
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication file token too long, skipping: \"%s\"",
							start_buf)));
			*err_msg = "authentication file token too long";
			/* Discard remainder of line */
			while ((c = (*(*lineptr)++)) != '\0')
				;
			/* Un-eat the '\0', in case we're called again */
			(*lineptr)--;
			return false;
		}

		/* we do not pass back a terminating comma in the token */
		if (c == ',' && !in_quote)
		{
			*terminating_comma = true;
			break;
		}

		if (c != '"' || was_quote)
			*buf++ = c;

		/* Literal double-quote is two double-quotes */
		if (in_quote && c == '"')
			was_quote = !was_quote;
		else
			was_quote = false;

		if (c == '"')
		{
			in_quote = !in_quote;
			saw_quote = true;
			if (buf == start_buf)
				*initial_quote = true;
		}

		c = *(*lineptr)++;
	}

	/*
	 * Un-eat the char right after the token (critical in case it is '\0',
	 * else next call will read past end of string).
	 */
	(*lineptr)--;

	*buf = '\0';

	return (saw_quote || buf > start_buf);
}

/*
 * Construct a palloc'd AuthToken struct, copying the given string.
 */
static AuthToken *
make_auth_token(const char *token, bool quoted)
{
	AuthToken  *authtoken;
	int			toklen;

	toklen = strlen(token);
	/* we copy string into same palloc block as the struct */
	authtoken = (AuthToken *) palloc(sizeof(AuthToken) + toklen + 1);
	authtoken->string = (char *) authtoken + sizeof(AuthToken);
	authtoken->quoted = quoted;
	memcpy(authtoken->string, token, toklen + 1);

	return authtoken;
}

/*
 * Copy a AuthToken struct into freshly palloc'd memory.
 */
static AuthToken *
copy_auth_token(AuthToken *in)
{
	AuthToken  *out = make_auth_token(in->string, in->quoted);

	return out;
}


/*
 * Tokenize one HBA field from a line, handling file inclusion and comma lists.
 *
 * filename: current file's pathname (needed to resolve relative pathnames)
 * *lineptr: current line pointer, which will be advanced past field
 *
 * In event of an error, log a message at ereport level elevel, and also
 * set *err_msg to a string describing the error.  Note that the result
 * may be non-NIL anyway, so *err_msg must be tested to determine whether
 * there was an error.
 *
 * The result is a List of AuthToken structs, one for each token in the field,
 * or NIL if we reached EOL.
 */
static List *
next_field_expand(const char *filename, char **lineptr,
				  int elevel, char **err_msg)
{
	char		buf[MAX_TOKEN];
	bool		trailing_comma;
	bool		initial_quote;
	List	   *tokens = NIL;

	do
	{
		if (!next_token(lineptr, buf, sizeof(buf),
						&initial_quote, &trailing_comma,
						elevel, err_msg))
			break;

		/* Is this referencing a file? */
		if (!initial_quote && buf[0] == '@' && buf[1] != '\0')
			tokens = tokenize_inc_file(tokens, filename, buf + 1,
									   elevel, err_msg);
		else
			tokens = lappend(tokens, make_auth_token(buf, initial_quote));
	} while (trailing_comma && (*err_msg == NULL));

	return tokens;
}

/*
 * tokenize_inc_file
 *		Expand a file included from another file into an hba "field"
 *
 * Opens and tokenises a file included from another HBA config file with @,
 * and returns all values found therein as a flat list of AuthTokens.  If a
 * @-token is found, recursively expand it.  The newly read tokens are
 * appended to "tokens" (so that foo,bar,@baz does what you expect).
 * All new tokens are allocated in caller's memory context.
 *
 * In event of an error, log a message at ereport level elevel, and also
 * set *err_msg to a string describing the error.  Note that the result
 * may be non-NIL anyway, so *err_msg must be tested to determine whether
 * there was an error.
 */
static List *
tokenize_inc_file(List *tokens,
				  const char *outer_filename,
				  const char *inc_filename,
				  int elevel,
				  char **err_msg)
{
	char	   *inc_fullname;
	FILE	   *inc_file;
	List	   *inc_lines;
	ListCell   *inc_line;
	MemoryContext linecxt;

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
		int			save_errno = errno;

		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open secondary authentication file \"@%s\" as \"%s\": %m",
						inc_filename, inc_fullname)));
		*err_msg = psprintf("could not open secondary authentication file \"@%s\" as \"%s\": %s",
							inc_filename, inc_fullname, strerror(save_errno));
		pfree(inc_fullname);
		return tokens;
	}

	/* There is possible recursion here if the file contains @ */
	linecxt = tokenize_auth_file(inc_fullname, inc_file, &inc_lines, elevel);

	FreeFile(inc_file);
	pfree(inc_fullname);

	/* Copy all tokens found in the file and append to the tokens list */
	foreach(inc_line, inc_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(inc_line);
		ListCell   *inc_field;

		/* If any line has an error, propagate that up to caller */
		if (tok_line->err_msg)
		{
			*err_msg = pstrdup(tok_line->err_msg);
			break;
		}

		foreach(inc_field, tok_line->fields)
		{
			List	   *inc_tokens = lfirst(inc_field);
			ListCell   *inc_token;

			foreach(inc_token, inc_tokens)
			{
				AuthToken  *token = lfirst(inc_token);

				tokens = lappend(tokens, copy_auth_token(token));
			}
		}
	}

	MemoryContextDelete(linecxt);
	return tokens;
}

/*
 * tokenize_auth_file
 *		Tokenize the given file.
 *
 * The output is a list of TokenizedAuthLine structs; see the struct definition
 * in libpq/hba.h.
 *
 * filename: the absolute path to the target file
 * file: the already-opened target file
 * tok_lines: receives output list
 * elevel: message logging level
 *
 * Errors are reported by logging messages at ereport level elevel and by
 * adding TokenizedAuthLine structs containing non-null err_msg fields to the
 * output list.
 *
 * Return value is a memory context which contains all memory allocated by
 * this function (it's a child of caller's context).
 */
MemoryContext
tokenize_auth_file(const char *filename, FILE *file, List **tok_lines,
				   int elevel)
{
	int			line_number = 1;
	StringInfoData buf;
	MemoryContext linecxt;
	MemoryContext oldcxt;

	linecxt = AllocSetContextCreate(CurrentMemoryContext,
									"tokenize_auth_file",
									ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(linecxt);

	initStringInfo(&buf);

	*tok_lines = NIL;

	while (!feof(file) && !ferror(file))
	{
		char	   *lineptr;
		List	   *current_line = NIL;
		char	   *err_msg = NULL;
		int			last_backslash_buflen = 0;
		int			continuations = 0;

		/* Collect the next input line, handling backslash continuations */
		resetStringInfo(&buf);

		while (pg_get_line_append(file, &buf, NULL))
		{
			/* Strip trailing newline, including \r in case we're on Windows */
			buf.len = pg_strip_crlf(buf.data);

			/*
			 * Check for backslash continuation.  The backslash must be after
			 * the last place we found a continuation, else two backslashes
			 * followed by two \n's would behave surprisingly.
			 */
			if (buf.len > last_backslash_buflen &&
				buf.data[buf.len - 1] == '\\')
			{
				/* Continuation, so strip it and keep reading */
				buf.data[--buf.len] = '\0';
				last_backslash_buflen = buf.len;
				continuations++;
				continue;
			}

			/* Nope, so we have the whole line */
			break;
		}

		if (ferror(file))
		{
			/* I/O error! */
			int			save_errno = errno;

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", filename)));
			err_msg = psprintf("could not read file \"%s\": %s",
							   filename, strerror(save_errno));
			break;
		}

		/* Parse fields */
		lineptr = buf.data;
		while (*lineptr && err_msg == NULL)
		{
			List	   *current_field;

			current_field = next_field_expand(filename, &lineptr,
											  elevel, &err_msg);
			/* add field to line, unless we are at EOL or comment start */
			if (current_field != NIL)
				current_line = lappend(current_line, current_field);
		}

		/*
		 * Reached EOL; emit line to TokenizedAuthLine list unless it's boring
		 */
		if (current_line != NIL || err_msg != NULL)
		{
			TokenizedAuthLine *tok_line;

			tok_line = (TokenizedAuthLine *) palloc(sizeof(TokenizedAuthLine));
			tok_line->fields = current_line;
			tok_line->line_num = line_number;
			tok_line->raw_line = pstrdup(buf.data);
			tok_line->err_msg = err_msg;
			*tok_lines = lappend(*tok_lines, tok_line);
		}

		line_number += continuations + 1;
	}

	MemoryContextSwitchTo(oldcxt);

	return linecxt;
}


/*
 * Does user belong to role?
 *
 * userid is the OID of the role given as the attempted login identifier.
 * We check to see if it is a member of the specified role name.
 */
static bool
is_member(Oid userid, const char *role)
{
	Oid			roleid;

	if (!OidIsValid(userid))
		return false;			/* if user not exist, say "no" */

	roleid = get_role_oid(role, true);

	if (!OidIsValid(roleid))
		return false;			/* if target role not exist, say "no" */

	/*
	 * See if user is directly or indirectly a member of role. For this
	 * purpose, a superuser is not considered to be automatically a member of
	 * the role, so group auth only applies to explicit membership.
	 */
	return is_member_of_role_nosuper(userid, roleid);
}

/*
 * Check AuthToken list for a match to role, allowing group names.
 */
static bool
check_role(const char *role, Oid roleid, List *tokens)
{
	ListCell   *cell;
	AuthToken  *tok;

	foreach(cell, tokens)
	{
		tok = lfirst(cell);
		if (!tok->quoted && tok->string[0] == '+')
		{
			if (is_member(roleid, tok->string + 1))
				return true;
		}
		else if (token_matches(tok, role) ||
				 token_is_keyword(tok, "all"))
			return true;
	}
	return false;
}

/*
 * Check to see if db/role combination matches AuthToken list.
 */
static bool
check_db(const char *dbname, const char *role, Oid roleid, List *tokens)
{
	ListCell   *cell;
	AuthToken  *tok;

	foreach(cell, tokens)
	{
		tok = lfirst(cell);
		if (am_walsender && !am_db_walsender)
		{
			/*
			 * physical replication walsender connections can only match
			 * replication keyword
			 */
			if (token_is_keyword(tok, "replication"))
				return true;
		}
		else if (token_is_keyword(tok, "all"))
			return true;
		else if (token_is_keyword(tok, "sameuser"))
		{
			if (strcmp(dbname, role) == 0)
				return true;
		}
		else if (token_is_keyword(tok, "samegroup") ||
				 token_is_keyword(tok, "samerole"))
		{
			if (is_member(roleid, dbname))
				return true;
		}
		else if (token_is_keyword(tok, "replication"))
			continue;			/* never match this if not walsender */
		else if (token_matches(tok, dbname))
			return true;
	}
	return false;
}

static bool
ipv4eq(struct sockaddr_in *a, struct sockaddr_in *b)
{
	return (a->sin_addr.s_addr == b->sin_addr.s_addr);
}

#ifdef HAVE_IPV6

static bool
ipv6eq(struct sockaddr_in6 *a, struct sockaddr_in6 *b)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (a->sin6_addr.s6_addr[i] != b->sin6_addr.s6_addr[i])
			return false;

	return true;
}
#endif							/* HAVE_IPV6 */

/*
 * Check whether host name matches pattern.
 */
static bool
hostname_match(const char *pattern, const char *actual_hostname)
{
	if (pattern[0] == '.')		/* suffix match */
	{
		size_t		plen = strlen(pattern);
		size_t		hlen = strlen(actual_hostname);

		if (hlen < plen)
			return false;

		return (pg_strcasecmp(pattern, actual_hostname + (hlen - plen)) == 0);
	}
	else
		return (pg_strcasecmp(pattern, actual_hostname) == 0);
}

/*
 * Check to see if a connecting IP matches a given host name.
 */
static bool
check_hostname(hbaPort *port, const char *hostname)
{
	struct addrinfo *gai_result,
			   *gai;
	int			ret;
	bool		found;

	/* Quick out if remote host name already known bad */
	if (port->remote_hostname_resolv < 0)
		return false;

	/* Lookup remote host name if not already done */
	if (!port->remote_hostname)
	{
		char		remote_hostname[NI_MAXHOST];

		ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								 remote_hostname, sizeof(remote_hostname),
								 NULL, 0,
								 NI_NAMEREQD);
		if (ret != 0)
		{
			/* remember failure; don't complain in the postmaster log yet */
			port->remote_hostname_resolv = -2;
			port->remote_hostname_errcode = ret;
			return false;
		}

		port->remote_hostname = pstrdup(remote_hostname);
	}

	/* Now see if remote host name matches this pg_hba line */
	if (!hostname_match(hostname, port->remote_hostname))
		return false;

	/* If we already verified the forward lookup, we're done */
	if (port->remote_hostname_resolv == +1)
		return true;

	/* Lookup IP from host name and check against original IP */
	ret = getaddrinfo(port->remote_hostname, NULL, NULL, &gai_result);
	if (ret != 0)
	{
		/* remember failure; don't complain in the postmaster log yet */
		port->remote_hostname_resolv = -2;
		port->remote_hostname_errcode = ret;
		return false;
	}

	found = false;
	for (gai = gai_result; gai; gai = gai->ai_next)
	{
		if (gai->ai_addr->sa_family == port->raddr.addr.ss_family)
		{
			if (gai->ai_addr->sa_family == AF_INET)
			{
				if (ipv4eq((struct sockaddr_in *) gai->ai_addr,
						   (struct sockaddr_in *) &port->raddr.addr))
				{
					found = true;
					break;
				}
			}
#ifdef HAVE_IPV6
			else if (gai->ai_addr->sa_family == AF_INET6)
			{
				if (ipv6eq((struct sockaddr_in6 *) gai->ai_addr,
						   (struct sockaddr_in6 *) &port->raddr.addr))
				{
					found = true;
					break;
				}
			}
#endif
		}
	}

	if (gai_result)
		freeaddrinfo(gai_result);

	if (!found)
		elog(DEBUG2, "pg_hba.conf host name \"%s\" rejected because address resolution did not return a match with IP address of client",
			 hostname);

	port->remote_hostname_resolv = found ? +1 : -1;

	return found;
}

/*
 * Check to see if a connecting IP matches the given address and netmask.
 */
static bool
check_ip(SockAddr *raddr, struct sockaddr *addr, struct sockaddr *mask)
{
	if (raddr->addr.ss_family == addr->sa_family &&
		pg_range_sockaddr(&raddr->addr,
						  (struct sockaddr_storage *) addr,
						  (struct sockaddr_storage *) mask))
		return true;
	return false;
}

/*
 * pg_foreach_ifaddr callback: does client addr match this machine interface?
 */
static void
check_network_callback(struct sockaddr *addr, struct sockaddr *netmask,
					   void *cb_data)
{
	check_network_data *cn = (check_network_data *) cb_data;
	struct sockaddr_storage mask;

	/* Already found a match? */
	if (cn->result)
		return;

	if (cn->method == ipCmpSameHost)
	{
		/* Make an all-ones netmask of appropriate length for family */
		pg_sockaddr_cidr_mask(&mask, NULL, addr->sa_family);
		cn->result = check_ip(cn->raddr, addr, (struct sockaddr *) &mask);
	}
	else
	{
		/* Use the netmask of the interface itself */
		cn->result = check_ip(cn->raddr, addr, netmask);
	}
}

/*
 * Use pg_foreach_ifaddr to check a samehost or samenet match
 */
static bool
check_same_host_or_net(SockAddr *raddr, IPCompareMethod method)
{
	check_network_data cn;

	cn.method = method;
	cn.raddr = raddr;
	cn.result = false;

	errno = 0;
	if (pg_foreach_ifaddr(check_network_callback, &cn) < 0)
	{
		ereport(LOG,
				(errmsg("error enumerating network interfaces: %m")));
		return false;
	}

	return cn.result;
}


/*
 * Macros used to check and report on invalid configuration options.
 * On error: log a message at level elevel, set *err_msg, and exit the function.
 * These macros are not as general-purpose as they look, because they know
 * what the calling function's error-exit value is.
 *
 * INVALID_AUTH_OPTION = reports when an option is specified for a method where it's
 *						 not supported.
 * REQUIRE_AUTH_OPTION = same as INVALID_AUTH_OPTION, except it also checks if the
 *						 method is actually the one specified. Used as a shortcut when
 *						 the option is only valid for one authentication method.
 * MANDATORY_AUTH_ARG  = check if a required option is set for an authentication method,
 *						 reporting error if it's not.
 */
#define INVALID_AUTH_OPTION(optname, validmethods) \
do { \
	ereport(elevel, \
			(errcode(ERRCODE_CONFIG_FILE_ERROR), \
			 /* translator: the second %s is a list of auth methods */ \
			 errmsg("authentication option \"%s\" is only valid for authentication methods %s", \
					optname, _(validmethods)), \
			 errcontext("line %d of configuration file \"%s\"", \
					line_num, HbaFileName))); \
	*err_msg = psprintf("authentication option \"%s\" is only valid for authentication methods %s", \
						optname, validmethods); \
	return false; \
} while (0)

#define REQUIRE_AUTH_OPTION(methodval, optname, validmethods) \
do { \
	if (hbaline->auth_method != methodval) \
		INVALID_AUTH_OPTION(optname, validmethods); \
} while (0)

#define MANDATORY_AUTH_ARG(argvar, argname, authname) \
do { \
	if (argvar == NULL) { \
		ereport(elevel, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("authentication method \"%s\" requires argument \"%s\" to be set", \
						authname, argname), \
				 errcontext("line %d of configuration file \"%s\"", \
						line_num, HbaFileName))); \
		*err_msg = psprintf("authentication method \"%s\" requires argument \"%s\" to be set", \
							authname, argname); \
		return NULL; \
	} \
} while (0)

/*
 * Macros for handling pg_ident problems, similar as above.
 *
 * IDENT_FIELD_ABSENT:
 * Reports when the given ident field ListCell is not populated.
 *
 * IDENT_MULTI_VALUE:
 * Reports when the given ident token List has more than one element.
 */
#define IDENT_FIELD_ABSENT(field) \
do { \
	if (!field) { \
		ereport(elevel, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("missing entry in file \"%s\" at end of line %d", \
						IdentFileName, line_num))); \
		*err_msg = psprintf("missing entry at end of line"); \
		return NULL; \
	} \
} while (0)

#define IDENT_MULTI_VALUE(tokens) \
do { \
	if (tokens->length > 1) { \
		ereport(elevel, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("multiple values in ident field"), \
				 errcontext("line %d of configuration file \"%s\"", \
							line_num, IdentFileName))); \
		*err_msg = psprintf("multiple values in ident field"); \
		return NULL; \
	} \
} while (0)


/*
 * Parse one tokenised line from the hba config file and store the result in a
 * HbaLine structure.
 *
 * If parsing fails, log a message at ereport level elevel, store an error
 * string in tok_line->err_msg, and return NULL.  (Some non-error conditions
 * can also result in such messages.)
 *
 * Note: this function leaks memory when an error occurs.  Caller is expected
 * to have set a memory context that will be reset if this function returns
 * NULL.
 */
HbaLine *
parse_hba_line(TokenizedAuthLine *tok_line, int elevel)
{
	int			line_num = tok_line->line_num;
	char	  **err_msg = &tok_line->err_msg;
	char	   *str;
	struct addrinfo *gai_result;
	struct addrinfo hints;
	int			ret;
	char	   *cidr_slash;
	char	   *unsupauth;
	ListCell   *field;
	List	   *tokens;
	ListCell   *tokencell;
	AuthToken  *token;
	HbaLine    *parsedline;

	parsedline = palloc0(sizeof(HbaLine));
	parsedline->linenumber = line_num;
	parsedline->rawline = pstrdup(tok_line->raw_line);

	/* Check the record type. */
	Assert(tok_line->fields != NIL);
	field = list_head(tok_line->fields);
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("multiple values specified for connection type"),
				 errhint("Specify exactly one connection type per line."),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "multiple values specified for connection type";
		return NULL;
	}
	token = linitial(tokens);
	if (strcmp(token->string, "local") == 0)
	{
#ifdef HAVE_UNIX_SOCKETS
		parsedline->conntype = ctLocal;
#else
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("local connections are not supported by this build"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "local connections are not supported by this build";
		return NULL;
#endif
	}
	else if (strcmp(token->string, "host") == 0 ||
			 strcmp(token->string, "hostssl") == 0 ||
			 strcmp(token->string, "hostnossl") == 0 ||
			 strcmp(token->string, "hostgssenc") == 0 ||
			 strcmp(token->string, "hostnogssenc") == 0)
	{

		if (token->string[4] == 's')	/* "hostssl" */
		{
			parsedline->conntype = ctHostSSL;
			/* Log a warning if SSL support is not active */
#ifdef USE_SSL
			if (!EnableSSL)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("hostssl record cannot match because SSL is disabled"),
						 errhint("Set ssl = on in postgresql.conf."),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				*err_msg = "hostssl record cannot match because SSL is disabled";
			}
#else
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("hostssl record cannot match because SSL is not supported by this build"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "hostssl record cannot match because SSL is not supported by this build";
#endif
		}
		else if (token->string[4] == 'g')	/* "hostgssenc" */
		{
			parsedline->conntype = ctHostGSS;
#ifndef ENABLE_GSS
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("hostgssenc record cannot match because GSSAPI is not supported by this build"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "hostgssenc record cannot match because GSSAPI is not supported by this build";
#endif
		}
		else if (token->string[4] == 'n' && token->string[6] == 's')
			parsedline->conntype = ctHostNoSSL;
		else if (token->string[4] == 'n' && token->string[6] == 'g')
			parsedline->conntype = ctHostNoGSS;
		else
		{
			/* "host" */
			parsedline->conntype = ctHost;
		}
	}							/* record type */
	else
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid connection type \"%s\"",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = psprintf("invalid connection type \"%s\"", token->string);
		return NULL;
	}

	/* Get the databases. */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before database specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "end-of-line before database specification";
		return NULL;
	}
	parsedline->databases = NIL;
	tokens = lfirst(field);
	foreach(tokencell, tokens)
	{
		parsedline->databases = lappend(parsedline->databases,
										copy_auth_token(lfirst(tokencell)));
	}

	/* Get the roles. */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before role specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "end-of-line before role specification";
		return NULL;
	}
	parsedline->roles = NIL;
	tokens = lfirst(field);
	foreach(tokencell, tokens)
	{
		parsedline->roles = lappend(parsedline->roles,
									copy_auth_token(lfirst(tokencell)));
	}

	if (parsedline->conntype != ctLocal)
	{
		/* Read the IP address field. (with or without CIDR netmask) */
		field = lnext(tok_line->fields, field);
		if (!field)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("end-of-line before IP address specification"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "end-of-line before IP address specification";
			return NULL;
		}
		tokens = lfirst(field);
		if (tokens->length > 1)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("multiple values specified for host address"),
					 errhint("Specify one address range per line."),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "multiple values specified for host address";
			return NULL;
		}
		token = linitial(tokens);

		if (token_is_keyword(token, "all"))
		{
			parsedline->ip_cmp_method = ipCmpAll;
		}
		else if (token_is_keyword(token, "samehost"))
		{
			/* Any IP on this host is allowed to connect */
			parsedline->ip_cmp_method = ipCmpSameHost;
		}
		else if (token_is_keyword(token, "samenet"))
		{
			/* Any IP on the host's subnets is allowed to connect */
			parsedline->ip_cmp_method = ipCmpSameNet;
		}
		else
		{
			/* IP and netmask are specified */
			parsedline->ip_cmp_method = ipCmpMask;

			/* need a modifiable copy of token */
			str = pstrdup(token->string);

			/* Check if it has a CIDR suffix and if so isolate it */
			cidr_slash = strchr(str, '/');
			if (cidr_slash)
				*cidr_slash = '\0';

			/* Get the IP address either way */
			hints.ai_flags = AI_NUMERICHOST;
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = 0;
			hints.ai_protocol = 0;
			hints.ai_addrlen = 0;
			hints.ai_canonname = NULL;
			hints.ai_addr = NULL;
			hints.ai_next = NULL;

			ret = pg_getaddrinfo_all(str, NULL, &hints, &gai_result);
			if (ret == 0 && gai_result)
			{
				memcpy(&parsedline->addr, gai_result->ai_addr,
					   gai_result->ai_addrlen);
				parsedline->addrlen = gai_result->ai_addrlen;
			}
			else if (ret == EAI_NONAME)
				parsedline->hostname = str;
			else
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("invalid IP address \"%s\": %s",
								str, gai_strerror(ret)),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				*err_msg = psprintf("invalid IP address \"%s\": %s",
									str, gai_strerror(ret));
				if (gai_result)
					pg_freeaddrinfo_all(hints.ai_family, gai_result);
				return NULL;
			}

			pg_freeaddrinfo_all(hints.ai_family, gai_result);

			/* Get the netmask */
			if (cidr_slash)
			{
				if (parsedline->hostname)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("specifying both host name and CIDR mask is invalid: \"%s\"",
									token->string),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, HbaFileName)));
					*err_msg = psprintf("specifying both host name and CIDR mask is invalid: \"%s\"",
										token->string);
					return NULL;
				}

				if (pg_sockaddr_cidr_mask(&parsedline->mask, cidr_slash + 1,
										  parsedline->addr.ss_family) < 0)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid CIDR mask in address \"%s\"",
									token->string),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, HbaFileName)));
					*err_msg = psprintf("invalid CIDR mask in address \"%s\"",
										token->string);
					return NULL;
				}
				parsedline->masklen = parsedline->addrlen;
				pfree(str);
			}
			else if (!parsedline->hostname)
			{
				/* Read the mask field. */
				pfree(str);
				field = lnext(tok_line->fields, field);
				if (!field)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("end-of-line before netmask specification"),
							 errhint("Specify an address range in CIDR notation, or provide a separate netmask."),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, HbaFileName)));
					*err_msg = "end-of-line before netmask specification";
					return NULL;
				}
				tokens = lfirst(field);
				if (tokens->length > 1)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("multiple values specified for netmask"),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, HbaFileName)));
					*err_msg = "multiple values specified for netmask";
					return NULL;
				}
				token = linitial(tokens);

				ret = pg_getaddrinfo_all(token->string, NULL,
										 &hints, &gai_result);
				if (ret || !gai_result)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid IP mask \"%s\": %s",
									token->string, gai_strerror(ret)),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, HbaFileName)));
					*err_msg = psprintf("invalid IP mask \"%s\": %s",
										token->string, gai_strerror(ret));
					if (gai_result)
						pg_freeaddrinfo_all(hints.ai_family, gai_result);
					return NULL;
				}

				memcpy(&parsedline->mask, gai_result->ai_addr,
					   gai_result->ai_addrlen);
				parsedline->masklen = gai_result->ai_addrlen;
				pg_freeaddrinfo_all(hints.ai_family, gai_result);

				if (parsedline->addr.ss_family != parsedline->mask.ss_family)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("IP address and mask do not match"),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, HbaFileName)));
					*err_msg = "IP address and mask do not match";
					return NULL;
				}
			}
		}
	}							/* != ctLocal */

	/* Get the authentication method */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before authentication method"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "end-of-line before authentication method";
		return NULL;
	}
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("multiple values specified for authentication type"),
				 errhint("Specify exactly one authentication type per line."),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "multiple values specified for authentication type";
		return NULL;
	}
	token = linitial(tokens);

	unsupauth = NULL;
	if (strcmp(token->string, "trust") == 0)
		parsedline->auth_method = uaTrust;
	else if (strcmp(token->string, "ident") == 0)
		parsedline->auth_method = uaIdent;
	else if (strcmp(token->string, "peer") == 0)
		parsedline->auth_method = uaPeer;
	else if (strcmp(token->string, "password") == 0)
		parsedline->auth_method = uaPassword;
	else if (strcmp(token->string, "gss") == 0)
#ifdef ENABLE_GSS
		parsedline->auth_method = uaGSS;
#else
		unsupauth = "gss";
#endif
	else if (strcmp(token->string, "sspi") == 0)
#ifdef ENABLE_SSPI
		parsedline->auth_method = uaSSPI;
#else
		unsupauth = "sspi";
#endif
	else if (strcmp(token->string, "reject") == 0)
		parsedline->auth_method = uaReject;
	else if (strcmp(token->string, "md5") == 0)
	{
		if (Db_user_namespace)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("MD5 authentication is not supported when \"db_user_namespace\" is enabled"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "MD5 authentication is not supported when \"db_user_namespace\" is enabled";
			return NULL;
		}
		parsedline->auth_method = uaMD5;
	}
	else if (strcmp(token->string, "scram-sha-256") == 0)
		parsedline->auth_method = uaSCRAM;
	else if (strcmp(token->string, "pam") == 0)
#ifdef USE_PAM
		parsedline->auth_method = uaPAM;
#else
		unsupauth = "pam";
#endif
	else if (strcmp(token->string, "bsd") == 0)
#ifdef USE_BSD_AUTH
		parsedline->auth_method = uaBSD;
#else
		unsupauth = "bsd";
#endif
	else if (strcmp(token->string, "ldap") == 0)
#ifdef USE_LDAP
		parsedline->auth_method = uaLDAP;
#else
		unsupauth = "ldap";
#endif
	else if (strcmp(token->string, "cert") == 0)
#ifdef USE_SSL
		parsedline->auth_method = uaCert;
#else
		unsupauth = "cert";
#endif
	else if (strcmp(token->string, "radius") == 0)
		parsedline->auth_method = uaRADIUS;
	else
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\"",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = psprintf("invalid authentication method \"%s\"",
							token->string);
		return NULL;
	}

	if (unsupauth)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\": not supported by this build",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = psprintf("invalid authentication method \"%s\": not supported by this build",
							token->string);
		return NULL;
	}

	/*
	 * XXX: When using ident on local connections, change it to peer, for
	 * backwards compatibility.
	 */
	if (parsedline->conntype == ctLocal &&
		parsedline->auth_method == uaIdent)
		parsedline->auth_method = uaPeer;

	/* Invalid authentication combinations */
	if (parsedline->conntype == ctLocal &&
		parsedline->auth_method == uaGSS)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("gssapi authentication is not supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "gssapi authentication is not supported on local sockets";
		return NULL;
	}

	if (parsedline->conntype != ctLocal &&
		parsedline->auth_method == uaPeer)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("peer authentication is only supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "peer authentication is only supported on local sockets";
		return NULL;
	}

	/*
	 * SSPI authentication can never be enabled on ctLocal connections,
	 * because it's only supported on Windows, where ctLocal isn't supported.
	 */


	if (parsedline->conntype != ctHostSSL &&
		parsedline->auth_method == uaCert)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("cert authentication is only supported on hostssl connections"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = "cert authentication is only supported on hostssl connections";
		return NULL;
	}

	/*
	 * For GSS and SSPI, set the default value of include_realm to true.
	 * Having include_realm set to false is dangerous in multi-realm
	 * situations and is generally considered bad practice.  We keep the
	 * capability around for backwards compatibility, but we might want to
	 * remove it at some point in the future.  Users who still need to strip
	 * the realm off would be better served by using an appropriate regex in a
	 * pg_ident.conf mapping.
	 */
	if (parsedline->auth_method == uaGSS ||
		parsedline->auth_method == uaSSPI)
		parsedline->include_realm = true;

	/*
	 * For SSPI, include_realm defaults to the SAM-compatible domain (aka
	 * NetBIOS name) and user names instead of the Kerberos principal name for
	 * compatibility.
	 */
	if (parsedline->auth_method == uaSSPI)
	{
		parsedline->compat_realm = true;
		parsedline->upn_username = false;
	}

	/* Parse remaining arguments */
	while ((field = lnext(tok_line->fields, field)) != NULL)
	{
		tokens = lfirst(field);
		foreach(tokencell, tokens)
		{
			char	   *val;

			token = lfirst(tokencell);

			str = pstrdup(token->string);
			val = strchr(str, '=');
			if (val == NULL)
			{
				/*
				 * Got something that's not a name=value pair.
				 */
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("authentication option not in name=value format: %s", token->string),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				*err_msg = psprintf("authentication option not in name=value format: %s",
									token->string);
				return NULL;
			}

			*val++ = '\0';		/* str now holds "name", val holds "value" */
			if (!parse_hba_auth_opt(str, val, parsedline, elevel, err_msg))
				/* parse_hba_auth_opt already logged the error message */
				return NULL;
			pfree(str);
		}
	}

	/*
	 * Check if the selected authentication method has any mandatory arguments
	 * that are not set.
	 */
	if (parsedline->auth_method == uaLDAP)
	{
#ifndef HAVE_LDAP_INITIALIZE
		/* Not mandatory for OpenLDAP, because it can use DNS SRV records */
		MANDATORY_AUTH_ARG(parsedline->ldapserver, "ldapserver", "ldap");
#endif

		/*
		 * LDAP can operate in two modes: either with a direct bind, using
		 * ldapprefix and ldapsuffix, or using a search+bind, using
		 * ldapbasedn, ldapbinddn, ldapbindpasswd and one of
		 * ldapsearchattribute or ldapsearchfilter.  Disallow mixing these
		 * parameters.
		 */
		if (parsedline->ldapprefix || parsedline->ldapsuffix)
		{
			if (parsedline->ldapbasedn ||
				parsedline->ldapbinddn ||
				parsedline->ldapbindpasswd ||
				parsedline->ldapsearchattribute ||
				parsedline->ldapsearchfilter)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("cannot use ldapbasedn, ldapbinddn, ldapbindpasswd, ldapsearchattribute, ldapsearchfilter, or ldapurl together with ldapprefix"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				*err_msg = "cannot use ldapbasedn, ldapbinddn, ldapbindpasswd, ldapsearchattribute, ldapsearchfilter, or ldapurl together with ldapprefix";
				return NULL;
			}
		}
		else if (!parsedline->ldapbasedn)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication method \"ldap\" requires argument \"ldapbasedn\", \"ldapprefix\", or \"ldapsuffix\" to be set"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "authentication method \"ldap\" requires argument \"ldapbasedn\", \"ldapprefix\", or \"ldapsuffix\" to be set";
			return NULL;
		}

		/*
		 * When using search+bind, you can either use a simple attribute
		 * (defaulting to "uid") or a fully custom search filter.  You can't
		 * do both.
		 */
		if (parsedline->ldapsearchattribute && parsedline->ldapsearchfilter)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("cannot use ldapsearchattribute together with ldapsearchfilter"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "cannot use ldapsearchattribute together with ldapsearchfilter";
			return NULL;
		}
	}

	if (parsedline->auth_method == uaRADIUS)
	{
		MANDATORY_AUTH_ARG(parsedline->radiusservers, "radiusservers", "radius");
		MANDATORY_AUTH_ARG(parsedline->radiussecrets, "radiussecrets", "radius");

		if (list_length(parsedline->radiusservers) < 1)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS servers cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "list of RADIUS servers cannot be empty";
			return NULL;
		}

		if (list_length(parsedline->radiussecrets) < 1)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS secrets cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "list of RADIUS secrets cannot be empty";
			return NULL;
		}

		/*
		 * Verify length of option lists - each can be 0 (except for secrets,
		 * but that's already checked above), 1 (use the same value
		 * everywhere) or the same as the number of servers.
		 */
		if (!(list_length(parsedline->radiussecrets) == 1 ||
			  list_length(parsedline->radiussecrets) == list_length(parsedline->radiusservers)))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("the number of RADIUS secrets (%d) must be 1 or the same as the number of RADIUS servers (%d)",
							list_length(parsedline->radiussecrets),
							list_length(parsedline->radiusservers)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = psprintf("the number of RADIUS secrets (%d) must be 1 or the same as the number of RADIUS servers (%d)",
								list_length(parsedline->radiussecrets),
								list_length(parsedline->radiusservers));
			return NULL;
		}
		if (!(list_length(parsedline->radiusports) == 0 ||
			  list_length(parsedline->radiusports) == 1 ||
			  list_length(parsedline->radiusports) == list_length(parsedline->radiusservers)))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("the number of RADIUS ports (%d) must be 1 or the same as the number of RADIUS servers (%d)",
							list_length(parsedline->radiusports),
							list_length(parsedline->radiusservers)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = psprintf("the number of RADIUS ports (%d) must be 1 or the same as the number of RADIUS servers (%d)",
								list_length(parsedline->radiusports),
								list_length(parsedline->radiusservers));
			return NULL;
		}
		if (!(list_length(parsedline->radiusidentifiers) == 0 ||
			  list_length(parsedline->radiusidentifiers) == 1 ||
			  list_length(parsedline->radiusidentifiers) == list_length(parsedline->radiusservers)))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("the number of RADIUS identifiers (%d) must be 1 or the same as the number of RADIUS servers (%d)",
							list_length(parsedline->radiusidentifiers),
							list_length(parsedline->radiusservers)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = psprintf("the number of RADIUS identifiers (%d) must be 1 or the same as the number of RADIUS servers (%d)",
								list_length(parsedline->radiusidentifiers),
								list_length(parsedline->radiusservers));
			return NULL;
		}
	}

	/*
	 * Enforce any parameters implied by other settings.
	 */
	if (parsedline->auth_method == uaCert)
	{
		/*
		 * For auth method cert, client certificate validation is mandatory,
		 * and it implies the level of verify-full.
		 */
		parsedline->clientcert = clientCertFull;
	}

	return parsedline;
}


/*
 * Parse one name-value pair as an authentication option into the given
 * HbaLine.  Return true if we successfully parse the option, false if we
 * encounter an error.  In the event of an error, also log a message at
 * ereport level elevel, and store a message string into *err_msg.
 */
static bool
parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline,
				   int elevel, char **err_msg)
{
	int			line_num = hbaline->linenumber;

#ifdef USE_LDAP
	hbaline->ldapscope = LDAP_SCOPE_SUBTREE;
#endif

	if (strcmp(name, "map") == 0)
	{
		if (hbaline->auth_method != uaIdent &&
			hbaline->auth_method != uaPeer &&
			hbaline->auth_method != uaGSS &&
			hbaline->auth_method != uaSSPI &&
			hbaline->auth_method != uaCert)
			INVALID_AUTH_OPTION("map", gettext_noop("ident, peer, gssapi, sspi, and cert"));
		hbaline->usermap = pstrdup(val);
	}
	else if (strcmp(name, "clientcert") == 0)
	{
		if (hbaline->conntype != ctHostSSL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("clientcert can only be configured for \"hostssl\" rows"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "clientcert can only be configured for \"hostssl\" rows";
			return false;
		}

		if (strcmp(val, "verify-full") == 0)
		{
			hbaline->clientcert = clientCertFull;
		}
		else if (strcmp(val, "verify-ca") == 0)
		{
			if (hbaline->auth_method == uaCert)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("clientcert only accepts \"verify-full\" when using \"cert\" authentication"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				*err_msg = "clientcert can only be set to \"verify-full\" when using \"cert\" authentication";
				return false;
			}

			hbaline->clientcert = clientCertCA;
		}
		else
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid value for clientcert: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}
	}
	else if (strcmp(name, "clientname") == 0)
	{
		if (hbaline->conntype != ctHostSSL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("clientname can only be configured for \"hostssl\" rows"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = "clientname can only be configured for \"hostssl\" rows";
			return false;
		}

		if (strcmp(val, "CN") == 0)
		{
			hbaline->clientcertname = clientCertCN;
		}
		else if (strcmp(val, "DN") == 0)
		{
			hbaline->clientcertname = clientCertDN;
		}
		else
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid value for clientname: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}
	}
	else if (strcmp(name, "pamservice") == 0)
	{
		REQUIRE_AUTH_OPTION(uaPAM, "pamservice", "pam");
		hbaline->pamservice = pstrdup(val);
	}
	else if (strcmp(name, "pam_use_hostname") == 0)
	{
		REQUIRE_AUTH_OPTION(uaPAM, "pam_use_hostname", "pam");
		if (strcmp(val, "1") == 0)
			hbaline->pam_use_hostname = true;
		else
			hbaline->pam_use_hostname = false;
	}
	else if (strcmp(name, "ldapurl") == 0)
	{
#ifdef LDAP_API_FEATURE_X_OPENLDAP
		LDAPURLDesc *urldata;
		int			rc;
#endif

		REQUIRE_AUTH_OPTION(uaLDAP, "ldapurl", "ldap");
#ifdef LDAP_API_FEATURE_X_OPENLDAP
		rc = ldap_url_parse(val, &urldata);
		if (rc != LDAP_SUCCESS)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse LDAP URL \"%s\": %s", val, ldap_err2string(rc))));
			*err_msg = psprintf("could not parse LDAP URL \"%s\": %s",
								val, ldap_err2string(rc));
			return false;
		}

		if (strcmp(urldata->lud_scheme, "ldap") != 0 &&
			strcmp(urldata->lud_scheme, "ldaps") != 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("unsupported LDAP URL scheme: %s", urldata->lud_scheme)));
			*err_msg = psprintf("unsupported LDAP URL scheme: %s",
								urldata->lud_scheme);
			ldap_free_urldesc(urldata);
			return false;
		}

		if (urldata->lud_scheme)
			hbaline->ldapscheme = pstrdup(urldata->lud_scheme);
		if (urldata->lud_host)
			hbaline->ldapserver = pstrdup(urldata->lud_host);
		hbaline->ldapport = urldata->lud_port;
		if (urldata->lud_dn)
			hbaline->ldapbasedn = pstrdup(urldata->lud_dn);

		if (urldata->lud_attrs)
			hbaline->ldapsearchattribute = pstrdup(urldata->lud_attrs[0]);	/* only use first one */
		hbaline->ldapscope = urldata->lud_scope;
		if (urldata->lud_filter)
			hbaline->ldapsearchfilter = pstrdup(urldata->lud_filter);
		ldap_free_urldesc(urldata);
#else							/* not OpenLDAP */
		ereport(elevel,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LDAP URLs not supported on this platform")));
		*err_msg = "LDAP URLs not supported on this platform";
#endif							/* not OpenLDAP */
	}
	else if (strcmp(name, "ldaptls") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldaptls", "ldap");
		if (strcmp(val, "1") == 0)
			hbaline->ldaptls = true;
		else
			hbaline->ldaptls = false;
	}
	else if (strcmp(name, "ldapscheme") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapscheme", "ldap");
		if (strcmp(val, "ldap") != 0 && strcmp(val, "ldaps") != 0)
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid ldapscheme value: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
		hbaline->ldapscheme = pstrdup(val);
	}
	else if (strcmp(name, "ldapserver") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapserver", "ldap");
		hbaline->ldapserver = pstrdup(val);
	}
	else if (strcmp(name, "ldapport") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapport", "ldap");
		hbaline->ldapport = atoi(val);
		if (hbaline->ldapport == 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid LDAP port number: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = psprintf("invalid LDAP port number: \"%s\"", val);
			return false;
		}
	}
	else if (strcmp(name, "ldapbinddn") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapbinddn", "ldap");
		hbaline->ldapbinddn = pstrdup(val);
	}
	else if (strcmp(name, "ldapbindpasswd") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapbindpasswd", "ldap");
		hbaline->ldapbindpasswd = pstrdup(val);
	}
	else if (strcmp(name, "ldapsearchattribute") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapsearchattribute", "ldap");
		hbaline->ldapsearchattribute = pstrdup(val);
	}
	else if (strcmp(name, "ldapsearchfilter") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapsearchfilter", "ldap");
		hbaline->ldapsearchfilter = pstrdup(val);
	}
	else if (strcmp(name, "ldapbasedn") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapbasedn", "ldap");
		hbaline->ldapbasedn = pstrdup(val);
	}
	else if (strcmp(name, "ldapprefix") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapprefix", "ldap");
		hbaline->ldapprefix = pstrdup(val);
	}
	else if (strcmp(name, "ldapsuffix") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapsuffix", "ldap");
		hbaline->ldapsuffix = pstrdup(val);
	}
	else if (strcmp(name, "krb_realm") == 0)
	{
		if (hbaline->auth_method != uaGSS &&
			hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("krb_realm", gettext_noop("gssapi and sspi"));
		hbaline->krb_realm = pstrdup(val);
	}
	else if (strcmp(name, "include_realm") == 0)
	{
		if (hbaline->auth_method != uaGSS &&
			hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("include_realm", gettext_noop("gssapi and sspi"));
		if (strcmp(val, "1") == 0)
			hbaline->include_realm = true;
		else
			hbaline->include_realm = false;
	}
	else if (strcmp(name, "compat_realm") == 0)
	{
		if (hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("compat_realm", gettext_noop("sspi"));
		if (strcmp(val, "1") == 0)
			hbaline->compat_realm = true;
		else
			hbaline->compat_realm = false;
	}
	else if (strcmp(name, "upn_username") == 0)
	{
		if (hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("upn_username", gettext_noop("sspi"));
		if (strcmp(val, "1") == 0)
			hbaline->upn_username = true;
		else
			hbaline->upn_username = false;
	}
	else if (strcmp(name, "radiusservers") == 0)
	{
		struct addrinfo *gai_result;
		struct addrinfo hints;
		int			ret;
		List	   *parsed_servers;
		ListCell   *l;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusservers", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_servers))
		{
			/* syntax error in list */
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS server list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}

		/* For each entry in the list, translate it */
		foreach(l, parsed_servers)
		{
			MemSet(&hints, 0, sizeof(hints));
			hints.ai_socktype = SOCK_DGRAM;
			hints.ai_family = AF_UNSPEC;

			ret = pg_getaddrinfo_all((char *) lfirst(l), NULL, &hints, &gai_result);
			if (ret || !gai_result)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not translate RADIUS server name \"%s\" to address: %s",
								(char *) lfirst(l), gai_strerror(ret)),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				if (gai_result)
					pg_freeaddrinfo_all(hints.ai_family, gai_result);

				list_free(parsed_servers);
				return false;
			}
			pg_freeaddrinfo_all(hints.ai_family, gai_result);
		}

		/* All entries are OK, so store them */
		hbaline->radiusservers = parsed_servers;
		hbaline->radiusservers_s = pstrdup(val);
	}
	else if (strcmp(name, "radiusports") == 0)
	{
		List	   *parsed_ports;
		ListCell   *l;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusports", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_ports))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS port list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			*err_msg = psprintf("invalid RADIUS port number: \"%s\"", val);
			return false;
		}

		foreach(l, parsed_ports)
		{
			if (atoi(lfirst(l)) == 0)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("invalid RADIUS port number: \"%s\"", val),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));

				return false;
			}
		}
		hbaline->radiusports = parsed_ports;
		hbaline->radiusports_s = pstrdup(val);
	}
	else if (strcmp(name, "radiussecrets") == 0)
	{
		List	   *parsed_secrets;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiussecrets", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_secrets))
		{
			/* syntax error in list */
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS secret list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}

		hbaline->radiussecrets = parsed_secrets;
		hbaline->radiussecrets_s = pstrdup(val);
	}
	else if (strcmp(name, "radiusidentifiers") == 0)
	{
		List	   *parsed_identifiers;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusidentifiers", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_identifiers))
		{
			/* syntax error in list */
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS identifiers list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}

		hbaline->radiusidentifiers = parsed_identifiers;
		hbaline->radiusidentifiers_s = pstrdup(val);
	}
	else
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("unrecognized authentication option name: \"%s\"",
						name),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		*err_msg = psprintf("unrecognized authentication option name: \"%s\"",
							name);
		return false;
	}
	return true;
}

/*
 *	Scan the pre-parsed hba file, looking for a match to the port's connection
 *	request.
 */
static void
check_hba(hbaPort *port)
{
	Oid			roleid;
	ListCell   *line;
	HbaLine    *hba;

	/* Get the target role's OID.  Note we do not error out for bad role. */
	roleid = get_role_oid(port->user_name, true);

	foreach(line, parsed_hba_lines)
	{
		hba = (HbaLine *) lfirst(line);

		/* Check connection type */
		if (hba->conntype == ctLocal)
		{
			if (port->raddr.addr.ss_family != AF_UNIX)
				continue;
		}
		else
		{
			if (port->raddr.addr.ss_family == AF_UNIX)
				continue;

			/* Check SSL state */
			if (port->ssl_in_use)
			{
				/* Connection is SSL, match both "host" and "hostssl" */
				if (hba->conntype == ctHostNoSSL)
					continue;
			}
			else
			{
				/* Connection is not SSL, match both "host" and "hostnossl" */
				if (hba->conntype == ctHostSSL)
					continue;
			}

			/* Check GSSAPI state */
#ifdef ENABLE_GSS
			if (port->gss && port->gss->enc &&
				hba->conntype == ctHostNoGSS)
				continue;
			else if (!(port->gss && port->gss->enc) &&
					 hba->conntype == ctHostGSS)
				continue;
#else
			if (hba->conntype == ctHostGSS)
				continue;
#endif

			/* Check IP address */
			switch (hba->ip_cmp_method)
			{
				case ipCmpMask:
					if (hba->hostname)
					{
						if (!check_hostname(port,
											hba->hostname))
							continue;
					}
					else
					{
						if (!check_ip(&port->raddr,
									  (struct sockaddr *) &hba->addr,
									  (struct sockaddr *) &hba->mask))
							continue;
					}
					break;
				case ipCmpAll:
					break;
				case ipCmpSameHost:
				case ipCmpSameNet:
					if (!check_same_host_or_net(&port->raddr,
												hba->ip_cmp_method))
						continue;
					break;
				default:
					/* shouldn't get here, but deem it no-match if so */
					continue;
			}
		}						/* != ctLocal */

		/* Check database and role */
		if (!check_db(port->database_name, port->user_name, roleid,
					  hba->databases))
			continue;

		if (!check_role(port->user_name, roleid, hba->roles))
			continue;

		/* Found a record that matched! */
		port->hba = hba;
		return;
	}

	/* If no matching entry was found, then implicitly reject. */
	hba = palloc0(sizeof(HbaLine));
	hba->auth_method = uaImplicitReject;
	port->hba = hba;
}

/*
 * Read the config file and create a List of HbaLine records for the contents.
 *
 * The configuration is read into a temporary list, and if any parse error
 * occurs the old list is kept in place and false is returned.  Only if the
 * whole file parses OK is the list replaced, and the function returns true.
 *
 * On a false result, caller will take care of reporting a FATAL error in case
 * this is the initial startup.  If it happens on reload, we just keep running
 * with the old data.
 */
bool
load_hba(void)
{
	FILE	   *file;
	List	   *hba_lines = NIL;
	ListCell   *line;
	List	   *new_parsed_lines = NIL;
	bool		ok = true;
	MemoryContext linecxt;
	MemoryContext oldcxt;
	MemoryContext hbacxt;

	file = AllocateFile(HbaFileName, "r");
	if (file == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open configuration file \"%s\": %m",
						HbaFileName)));
		return false;
	}

	linecxt = tokenize_auth_file(HbaFileName, file, &hba_lines, LOG);
	FreeFile(file);

	/* Now parse all the lines */
	Assert(PostmasterContext);
	hbacxt = AllocSetContextCreate(PostmasterContext,
								   "hba parser context",
								   ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	foreach(line, hba_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		HbaLine    *newline;

		/* don't parse lines that already have errors */
		if (tok_line->err_msg != NULL)
		{
			ok = false;
			continue;
		}

		if ((newline = parse_hba_line(tok_line, LOG)) == NULL)
		{
			/* Parse error; remember there's trouble */
			ok = false;

			/*
			 * Keep parsing the rest of the file so we can report errors on
			 * more than the first line.  Error has already been logged, no
			 * need for more chatter here.
			 */
			continue;
		}

		new_parsed_lines = lappend(new_parsed_lines, newline);
	}

	/*
	 * A valid HBA file must have at least one entry; else there's no way to
	 * connect to the postmaster.  But only complain about this if we didn't
	 * already have parsing errors.
	 */
	if (ok && new_parsed_lines == NIL)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("configuration file \"%s\" contains no entries",
						HbaFileName)));
		ok = false;
	}

	/* Free tokenizer memory */
	MemoryContextDelete(linecxt);
	MemoryContextSwitchTo(oldcxt);

	if (!ok)
	{
		/* File contained one or more errors, so bail out */
		MemoryContextDelete(hbacxt);
		return false;
	}

	/* Loaded new file successfully, replace the one we use */
	if (parsed_hba_context != NULL)
		MemoryContextDelete(parsed_hba_context);
	parsed_hba_context = hbacxt;
	parsed_hba_lines = new_parsed_lines;

	return true;
}


/*
 * Parse one tokenised line from the ident config file and store the result in
 * an IdentLine structure.
 *
 * If parsing fails, log a message at ereport level elevel, store an error
 * string in tok_line->err_msg and return NULL.
 *
 * If ident_user is a regular expression (ie. begins with a slash), it is
 * compiled and stored in IdentLine structure.
 *
 * Note: this function leaks memory when an error occurs.  Caller is expected
 * to have set a memory context that will be reset if this function returns
 * NULL.
 */
IdentLine *
parse_ident_line(TokenizedAuthLine *tok_line, int elevel)
{
	int			line_num = tok_line->line_num;
	char	  **err_msg = &tok_line->err_msg;
	ListCell   *field;
	List	   *tokens;
	AuthToken  *token;
	IdentLine  *parsedline;

	Assert(tok_line->fields != NIL);
	field = list_head(tok_line->fields);

	parsedline = palloc0(sizeof(IdentLine));
	parsedline->linenumber = line_num;

	/* Get the map token (must exist) */
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->usermap = pstrdup(token->string);

	/* Get the ident user token */
	field = lnext(tok_line->fields, field);
	IDENT_FIELD_ABSENT(field);
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->ident_user = pstrdup(token->string);

	/* Get the PG rolename token */
	field = lnext(tok_line->fields, field);
	IDENT_FIELD_ABSENT(field);
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->pg_role = pstrdup(token->string);

	if (parsedline->ident_user[0] == '/')
	{
		/*
		 * When system username starts with a slash, treat it as a regular
		 * expression. Pre-compile it.
		 */
		int			r;
		pg_wchar   *wstr;
		int			wlen;

		wstr = palloc((strlen(parsedline->ident_user + 1) + 1) * sizeof(pg_wchar));
		wlen = pg_mb2wchar_with_len(parsedline->ident_user + 1,
									wstr, strlen(parsedline->ident_user + 1));

		r = pg_regcomp(&parsedline->re, wstr, wlen, REG_ADVANCED, C_COLLATION_OID);
		if (r)
		{
			char		errstr[100];

			pg_regerror(r, &parsedline->re, errstr, sizeof(errstr));
			ereport(elevel,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("invalid regular expression \"%s\": %s",
							parsedline->ident_user + 1, errstr)));

			*err_msg = psprintf("invalid regular expression \"%s\": %s",
								parsedline->ident_user + 1, errstr);

			pfree(wstr);
			return NULL;
		}
		pfree(wstr);
	}

	return parsedline;
}

/*
 *	Process one line from the parsed ident config lines.
 *
 *	Compare input parsed ident line to the needed map, pg_role and ident_user.
 *	*found_p and *error_p are set according to our results.
 */
static void
check_ident_usermap(IdentLine *identLine, const char *usermap_name,
					const char *pg_role, const char *ident_user,
					bool case_insensitive, bool *found_p, bool *error_p)
{
	*found_p = false;
	*error_p = false;

	if (strcmp(identLine->usermap, usermap_name) != 0)
		/* Line does not match the map name we're looking for, so just abort */
		return;

	/* Match? */
	if (identLine->ident_user[0] == '/')
	{
		/*
		 * When system username starts with a slash, treat it as a regular
		 * expression. In this case, we process the system username as a
		 * regular expression that returns exactly one match. This is replaced
		 * for \1 in the database username string, if present.
		 */
		int			r;
		regmatch_t	matches[2];
		pg_wchar   *wstr;
		int			wlen;
		char	   *ofs;
		char	   *regexp_pgrole;

		wstr = palloc((strlen(ident_user) + 1) * sizeof(pg_wchar));
		wlen = pg_mb2wchar_with_len(ident_user, wstr, strlen(ident_user));

		r = pg_regexec(&identLine->re, wstr, wlen, 0, NULL, 2, matches, 0);
		if (r)
		{
			char		errstr[100];

			if (r != REG_NOMATCH)
			{
				/* REG_NOMATCH is not an error, everything else is */
				pg_regerror(r, &identLine->re, errstr, sizeof(errstr));
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression match for \"%s\" failed: %s",
								identLine->ident_user + 1, errstr)));
				*error_p = true;
			}

			pfree(wstr);
			return;
		}
		pfree(wstr);

		if ((ofs = strstr(identLine->pg_role, "\\1")) != NULL)
		{
			int			offset;

			/* substitution of the first argument requested */
			if (matches[1].rm_so < 0)
			{
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression \"%s\" has no subexpressions as requested by backreference in \"%s\"",
								identLine->ident_user + 1, identLine->pg_role)));
				*error_p = true;
				return;
			}

			/*
			 * length: original length minus length of \1 plus length of match
			 * plus null terminator
			 */
			regexp_pgrole = palloc0(strlen(identLine->pg_role) - 2 + (matches[1].rm_eo - matches[1].rm_so) + 1);
			offset = ofs - identLine->pg_role;
			memcpy(regexp_pgrole, identLine->pg_role, offset);
			memcpy(regexp_pgrole + offset,
				   ident_user + matches[1].rm_so,
				   matches[1].rm_eo - matches[1].rm_so);
			strcat(regexp_pgrole, ofs + 2);
		}
		else
		{
			/* no substitution, so copy the match */
			regexp_pgrole = pstrdup(identLine->pg_role);
		}

		/*
		 * now check if the username actually matched what the user is trying
		 * to connect as
		 */
		if (case_insensitive)
		{
			if (pg_strcasecmp(regexp_pgrole, pg_role) == 0)
				*found_p = true;
		}
		else
		{
			if (strcmp(regexp_pgrole, pg_role) == 0)
				*found_p = true;
		}
		pfree(regexp_pgrole);

		return;
	}
	else
	{
		/* Not regular expression, so make complete match */
		if (case_insensitive)
		{
			if (pg_strcasecmp(identLine->pg_role, pg_role) == 0 &&
				pg_strcasecmp(identLine->ident_user, ident_user) == 0)
				*found_p = true;
		}
		else
		{
			if (strcmp(identLine->pg_role, pg_role) == 0 &&
				strcmp(identLine->ident_user, ident_user) == 0)
				*found_p = true;
		}
	}
}


/*
 *	Scan the (pre-parsed) ident usermap file line by line, looking for a match
 *
 *	See if the user with ident username "auth_user" is allowed to act
 *	as Postgres user "pg_role" according to usermap "usermap_name".
 *
 *	Special case: Usermap NULL, equivalent to what was previously called
 *	"sameuser" or "samerole", means don't look in the usermap file.
 *	That's an implied map wherein "pg_role" must be identical to
 *	"auth_user" in order to be authorized.
 *
 *	Iff authorized, return STATUS_OK, otherwise return STATUS_ERROR.
 */
int
check_usermap(const char *usermap_name,
			  const char *pg_role,
			  const char *auth_user,
			  bool case_insensitive)
{
	bool		found_entry = false,
				error = false;

	if (usermap_name == NULL || usermap_name[0] == '\0')
	{
		if (case_insensitive)
		{
			if (pg_strcasecmp(pg_role, auth_user) == 0)
				return STATUS_OK;
		}
		else
		{
			if (strcmp(pg_role, auth_user) == 0)
				return STATUS_OK;
		}
		ereport(LOG,
				(errmsg("provided user name (%s) and authenticated user name (%s) do not match",
						pg_role, auth_user)));
		return STATUS_ERROR;
	}
	else
	{
		ListCell   *line_cell;

		foreach(line_cell, parsed_ident_lines)
		{
			check_ident_usermap(lfirst(line_cell), usermap_name,
								pg_role, auth_user, case_insensitive,
								&found_entry, &error);
			if (found_entry || error)
				break;
		}
	}
	if (!found_entry && !error)
	{
		ereport(LOG,
				(errmsg("no match in usermap \"%s\" for user \"%s\" authenticated as \"%s\"",
						usermap_name, pg_role, auth_user)));
	}
	return found_entry ? STATUS_OK : STATUS_ERROR;
}


/*
 * Read the ident config file and create a List of IdentLine records for
 * the contents.
 *
 * This works the same as load_hba(), but for the user config file.
 */
bool
load_ident(void)
{
	FILE	   *file;
	List	   *ident_lines = NIL;
	ListCell   *line_cell,
			   *parsed_line_cell;
	List	   *new_parsed_lines = NIL;
	bool		ok = true;
	MemoryContext linecxt;
	MemoryContext oldcxt;
	MemoryContext ident_context;
	IdentLine  *newline;

	file = AllocateFile(IdentFileName, "r");
	if (file == NULL)
	{
		/* not fatal ... we just won't do any special ident maps */
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open usermap file \"%s\": %m",
						IdentFileName)));
		return false;
	}

	linecxt = tokenize_auth_file(IdentFileName, file, &ident_lines, LOG);
	FreeFile(file);

	/* Now parse all the lines */
	Assert(PostmasterContext);
	ident_context = AllocSetContextCreate(PostmasterContext,
										  "ident parser context",
										  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(ident_context);
	foreach(line_cell, ident_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line_cell);

		/* don't parse lines that already have errors */
		if (tok_line->err_msg != NULL)
		{
			ok = false;
			continue;
		}

		if ((newline = parse_ident_line(tok_line, LOG)) == NULL)
		{
			/* Parse error; remember there's trouble */
			ok = false;

			/*
			 * Keep parsing the rest of the file so we can report errors on
			 * more than the first line.  Error has already been logged, no
			 * need for more chatter here.
			 */
			continue;
		}

		new_parsed_lines = lappend(new_parsed_lines, newline);
	}

	/* Free tokenizer memory */
	MemoryContextDelete(linecxt);
	MemoryContextSwitchTo(oldcxt);

	if (!ok)
	{
		/*
		 * File contained one or more errors, so bail out, first being careful
		 * to clean up whatever we allocated.  Most stuff will go away via
		 * MemoryContextDelete, but we have to clean up regexes explicitly.
		 */
		foreach(parsed_line_cell, new_parsed_lines)
		{
			newline = (IdentLine *) lfirst(parsed_line_cell);
			if (newline->ident_user[0] == '/')
				pg_regfree(&newline->re);
		}
		MemoryContextDelete(ident_context);
		return false;
	}

	/* Loaded new file successfully, replace the one we use */
	if (parsed_ident_lines != NIL)
	{
		foreach(parsed_line_cell, parsed_ident_lines)
		{
			newline = (IdentLine *) lfirst(parsed_line_cell);
			if (newline->ident_user[0] == '/')
				pg_regfree(&newline->re);
		}
	}
	if (parsed_ident_context != NULL)
		MemoryContextDelete(parsed_ident_context);

	parsed_ident_context = ident_context;
	parsed_ident_lines = new_parsed_lines;

	return true;
}



/*
 *	Determine what authentication method should be used when accessing database
 *	"database" from frontend "raddr", user "user".  Return the method and
 *	an optional argument (stored in fields of *port), and STATUS_OK.
 *
 *	If the file does not contain any entry matching the request, we return
 *	method = uaImplicitReject.
 */
void
hba_getauthmethod(hbaPort *port)
{
	check_hba(port);
}


/*
 * Return the name of the auth method in use ("gss", "md5", "trust", etc.).
 *
 * The return value is statically allocated (see the UserAuthName array) and
 * should not be freed.
 */
const char *
hba_authname(UserAuth auth_method)
{
	/*
	 * Make sure UserAuthName[] tracks additions to the UserAuth enum
	 */
	StaticAssertStmt(lengthof(UserAuthName) == USER_AUTH_LAST + 1,
					 "UserAuthName[] must match the UserAuth enum");

	return UserAuthName[auth_method];
}
