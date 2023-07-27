/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and choosing authentication method based on it).
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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


#define MAX_TOKEN	10240
#define MAX_LINE	20480

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
 * A single string token lexed from a config file, together with whether
 * the token had been quoted.
 */
typedef struct HbaToken
{
	char	   *string;
	bool		quoted;
} HbaToken;

/*
 * TokenizedLine represents one line lexed from a config file.
 * Each item in the "fields" list is a sub-list of HbaTokens.
 * We don't emit a TokenizedLine for empty or all-comment lines,
 * so "fields" is never NIL (nor are any of its sub-lists).
 * Exception: if an error occurs during tokenization, we might
 * have fields == NIL, in which case err_msg != NULL.
 */
typedef struct TokenizedLine
{
	List	   *fields;			/* List of lists of HbaTokens */
	int			line_num;		/* Line number */
	char	   *raw_line;		/* Raw line text */
	char	   *err_msg;		/* Error message if any */
} TokenizedLine;

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


static MemoryContext tokenize_file(const char *filename, FILE *file,
								   List **tok_lines, int elevel);
static List *tokenize_inc_file(List *tokens, const char *outer_filename,
							   const char *inc_filename, int elevel, char **err_msg);
static bool parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline,
							   int elevel, char **err_msg);
static bool verify_option_list_length(List *options, const char *optionname,
									  List *masters, const char *mastername, int line_num);
static ArrayType *gethba_options(HbaLine *hba);
static void fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
						  int lineno, HbaLine *hba, const char *err_msg);
static void fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);


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
 * Tokens are strings of non-blank
 * characters bounded by blank characters, commas, beginning of line, and
 * end of line. Blank means space or tab. Tokens can be delimited by
 * double quotes (this allows the inclusion of blanks, but not newlines).
 * Comments (started by an unquoted '#') are skipped.
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
 * Construct a palloc'd HbaToken struct, copying the given string.
 */
static HbaToken *
make_hba_token(const char *token, bool quoted)
{
	HbaToken   *hbatoken;
	int			toklen;

	toklen = strlen(token);
	/* we copy string into same palloc block as the struct */
	hbatoken = (HbaToken *) palloc(sizeof(HbaToken) + toklen + 1);
	hbatoken->string = (char *) hbatoken + sizeof(HbaToken);
	hbatoken->quoted = quoted;
	memcpy(hbatoken->string, token, toklen + 1);

	return hbatoken;
}

/*
 * Copy a HbaToken struct into freshly palloc'd memory.
 */
static HbaToken *
copy_hba_token(HbaToken *in)
{
	HbaToken   *out = make_hba_token(in->string, in->quoted);

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
 * The result is a List of HbaToken structs, one for each token in the field,
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
			tokens = lappend(tokens, make_hba_token(buf, initial_quote));
	} while (trailing_comma && (*err_msg == NULL));

	return tokens;
}

/*
 * tokenize_inc_file
 *		Expand a file included from another file into an hba "field"
 *
 * Opens and tokenises a file included from another HBA config file with @,
 * and returns all values found therein as a flat list of HbaTokens.  If a
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
	linecxt = tokenize_file(inc_fullname, inc_file, &inc_lines, elevel);

	FreeFile(inc_file);
	pfree(inc_fullname);

	/* Copy all tokens found in the file and append to the tokens list */
	foreach(inc_line, inc_lines)
	{
		TokenizedLine *tok_line = (TokenizedLine *) lfirst(inc_line);
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
				HbaToken   *token = lfirst(inc_token);

				tokens = lappend(tokens, copy_hba_token(token));
			}
		}
	}

	MemoryContextDelete(linecxt);
	return tokens;
}

/*
 * Tokenize the given file.
 *
 * The output is a list of TokenizedLine structs; see struct definition above.
 *
 * filename: the absolute path to the target file
 * file: the already-opened target file
 * tok_lines: receives output list
 * elevel: message logging level
 *
 * Errors are reported by logging messages at ereport level elevel and by
 * adding TokenizedLine structs containing non-null err_msg fields to the
 * output list.
 *
 * Return value is a memory context which contains all memory allocated by
 * this function (it's a child of caller's context).
 */
static MemoryContext
tokenize_file(const char *filename, FILE *file, List **tok_lines, int elevel)
{
	int			line_number = 1;
	MemoryContext linecxt;
	MemoryContext oldcxt;

	linecxt = AllocSetContextCreate(CurrentMemoryContext,
									"tokenize_file",
									ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(linecxt);

	*tok_lines = NIL;

	while (!feof(file) && !ferror(file))
	{
		char		rawline[MAX_LINE];
		char	   *lineptr;
		List	   *current_line = NIL;
		char	   *err_msg = NULL;

		if (!fgets(rawline, sizeof(rawline), file))
		{
			int			save_errno = errno;

			if (!ferror(file))
				break;			/* normal EOF */
			/* I/O error! */
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", filename)));
			err_msg = psprintf("could not read file \"%s\": %s",
							   filename, strerror(save_errno));
			rawline[0] = '\0';
		}
		if (strlen(rawline) == MAX_LINE - 1)
		{
			/* Line too long! */
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication file line too long"),
					 errcontext("line %d of configuration file \"%s\"",
								line_number, filename)));
			err_msg = "authentication file line too long";
		}

		/* Strip trailing linebreak from rawline */
		lineptr = rawline + strlen(rawline) - 1;
		while (lineptr >= rawline && (*lineptr == '\n' || *lineptr == '\r'))
			*lineptr-- = '\0';

		/* Parse fields */
		lineptr = rawline;
		while (*lineptr && err_msg == NULL)
		{
			List	   *current_field;

			current_field = next_field_expand(filename, &lineptr,
											  elevel, &err_msg);
			/* add field to line, unless we are at EOL or comment start */
			if (current_field != NIL)
				current_line = lappend(current_line, current_field);
		}

		/* Reached EOL; emit line to TokenizedLine list unless it's boring */
		if (current_line != NIL || err_msg != NULL)
		{
			TokenizedLine *tok_line;

			tok_line = (TokenizedLine *) palloc(sizeof(TokenizedLine));
			tok_line->fields = current_line;
			tok_line->line_num = line_number;
			tok_line->raw_line = pstrdup(rawline);
			tok_line->err_msg = err_msg;
			*tok_lines = lappend(*tok_lines, tok_line);
		}

		line_number++;
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
 * Check HbaToken list for a match to role, allowing group names.
 */
static bool
check_role(const char *role, Oid roleid, List *tokens)
{
	ListCell   *cell;
	HbaToken   *tok;

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
 * Check to see if db/role combination matches HbaToken list.
 */
static bool
check_db(const char *dbname, const char *role, Oid roleid, List *tokens)
{
	ListCell   *cell;
	HbaToken   *tok;

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
		elog(LOG, "error enumerating network interfaces: %m");
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
 * Macros for handling pg_ident problems.
 * Much as above, but currently the message level is hardwired as LOG
 * and there is no provision for an err_msg string.
 *
 * IDENT_FIELD_ABSENT:
 * Log a message and exit the function if the given ident field ListCell is
 * not populated.
 *
 * IDENT_MULTI_VALUE:
 * Log a message and exit the function if the given ident token List has more
 * than one element.
 */
#define IDENT_FIELD_ABSENT(field) \
do { \
	if (!field) { \
		ereport(LOG, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("missing entry in file \"%s\" at end of line %d", \
						IdentFileName, line_num))); \
		return NULL; \
	} \
} while (0)

#define IDENT_MULTI_VALUE(tokens) \
do { \
	if (tokens->length > 1) { \
		ereport(LOG, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("multiple values in ident field"), \
				 errcontext("line %d of configuration file \"%s\"", \
							line_num, IdentFileName))); \
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
static HbaLine *
parse_hba_line(TokenizedLine *tok_line, int elevel)
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
	HbaToken   *token;
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
					 errhint("Compile with --with-openssl to use SSL connections."),
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
					 errhint("Compile with --with-gssapi to use GSSAPI connections."),
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
										copy_hba_token(lfirst(tokencell)));
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
									copy_hba_token(lfirst(tokencell)));
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
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS servers cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return NULL;
		}

		if (list_length(parsedline->radiussecrets) < 1)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS secrets cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return NULL;
		}

		/*
		 * Verify length of option lists - each can be 0 (except for secrets,
		 * but that's already checked above), 1 (use the same value
		 * everywhere) or the same as the number of servers.
		 */
		if (!verify_option_list_length(parsedline->radiussecrets,
									   "RADIUS secrets",
									   parsedline->radiusservers,
									   "RADIUS servers",
									   line_num))
			return NULL;
		if (!verify_option_list_length(parsedline->radiusports,
									   "RADIUS ports",
									   parsedline->radiusservers,
									   "RADIUS servers",
									   line_num))
			return NULL;
		if (!verify_option_list_length(parsedline->radiusidentifiers,
									   "RADIUS identifiers",
									   parsedline->radiusservers,
									   "RADIUS servers",
									   line_num))
			return NULL;
	}

	/*
	 * Enforce any parameters implied by other settings.
	 */
	if (parsedline->auth_method == uaCert)
	{
		/*
		 * For auth method cert, client certificate validation is mandatory, and it implies
		 * the level of verify-full.
		 */
		parsedline->clientcert = clientCertFull;
	}

	return parsedline;
}


static bool
verify_option_list_length(List *options, const char *optionname, List *masters, const char *mastername, int line_num)
{
	if (list_length(options) == 0 ||
		list_length(options) == 1 ||
		list_length(options) == list_length(masters))
		return true;

	ereport(LOG,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg("the number of %s (%d) must be 1 or the same as the number of %s (%d)",
					optionname,
					list_length(options),
					mastername,
					list_length(masters)
					),
			 errcontext("line %d of configuration file \"%s\"",
						line_num, HbaFileName)));
	return false;
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
		if (strcmp(val, "1") == 0
			|| strcmp(val, "verify-ca") == 0)
		{
			hbaline->clientcert = clientCertCA;
		}
		else if (strcmp(val, "verify-full") == 0)
		{
			hbaline->clientcert = clientCertFull;
		}
		else if (strcmp(val, "0") == 0
				 || strcmp(val, "no-verify") == 0)
		{
			if (hbaline->auth_method == uaCert)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("clientcert cannot be set to \"no-verify\" when using \"cert\" authentication"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				*err_msg = "clientcert cannot be set to \"no-verify\" when using \"cert\" authentication";
				return false;
			}
			hbaline->clientcert = clientCertOff;
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
			if (!IS_AF_UNIX(port->raddr.addr.ss_family))
				continue;
		}
		else
		{
			if (IS_AF_UNIX(port->raddr.addr.ss_family))
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

	linecxt = tokenize_file(HbaFileName, file, &hba_lines, LOG);
	FreeFile(file);

	/* Now parse all the lines */
	Assert(PostmasterContext);
	hbacxt = AllocSetContextCreate(PostmasterContext,
								   "hba parser context",
								   ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	foreach(line, hba_lines)
	{
		TokenizedLine *tok_line = (TokenizedLine *) lfirst(line);
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
 * This macro specifies the maximum number of authentication options
 * that are possible with any given authentication method that is supported.
 * Currently LDAP supports 11, and there are 3 that are not dependent on
 * the auth method here.  It may not actually be possible to set all of them
 * at the same time, but we'll set the macro value high enough to be
 * conservative and avoid warnings from static analysis tools.
 */
#define MAX_HBA_OPTIONS 14

/*
 * Create a text array listing the options specified in the HBA line.
 * Return NULL if no options are specified.
 */
static ArrayType *
gethba_options(HbaLine *hba)
{
	int			noptions;
	Datum		options[MAX_HBA_OPTIONS];

	noptions = 0;

	if (hba->auth_method == uaGSS || hba->auth_method == uaSSPI)
	{
		if (hba->include_realm)
			options[noptions++] =
				CStringGetTextDatum("include_realm=true");

		if (hba->krb_realm)
			options[noptions++] =
				CStringGetTextDatum(psprintf("krb_realm=%s", hba->krb_realm));
	}

	if (hba->usermap)
		options[noptions++] =
			CStringGetTextDatum(psprintf("map=%s", hba->usermap));

	if (hba->clientcert != clientCertOff)
		options[noptions++] =
			CStringGetTextDatum(psprintf("clientcert=%s", (hba->clientcert == clientCertCA) ? "verify-ca" : "verify-full"));

	if (hba->pamservice)
		options[noptions++] =
			CStringGetTextDatum(psprintf("pamservice=%s", hba->pamservice));

	if (hba->auth_method == uaLDAP)
	{
		if (hba->ldapserver)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapserver=%s", hba->ldapserver));

		if (hba->ldapport)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapport=%d", hba->ldapport));

		if (hba->ldaptls)
			options[noptions++] =
				CStringGetTextDatum("ldaptls=true");

		if (hba->ldapprefix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapprefix=%s", hba->ldapprefix));

		if (hba->ldapsuffix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsuffix=%s", hba->ldapsuffix));

		if (hba->ldapbasedn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbasedn=%s", hba->ldapbasedn));

		if (hba->ldapbinddn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbinddn=%s", hba->ldapbinddn));

		if (hba->ldapbindpasswd)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbindpasswd=%s",
											 hba->ldapbindpasswd));

		if (hba->ldapsearchattribute)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchattribute=%s",
											 hba->ldapsearchattribute));

		if (hba->ldapsearchfilter)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchfilter=%s",
											 hba->ldapsearchfilter));

		if (hba->ldapscope)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapscope=%d", hba->ldapscope));
	}

	if (hba->auth_method == uaRADIUS)
	{
		if (hba->radiusservers_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusservers=%s", hba->radiusservers_s));

		if (hba->radiussecrets_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiussecrets=%s", hba->radiussecrets_s));

		if (hba->radiusidentifiers_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusidentifiers=%s", hba->radiusidentifiers_s));

		if (hba->radiusports_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusports=%s", hba->radiusports_s));
	}

	/* If you add more options, consider increasing MAX_HBA_OPTIONS. */
	Assert(noptions <= MAX_HBA_OPTIONS);

	if (noptions > 0)
		return construct_array(options, noptions, TEXTOID, -1, false, TYPALIGN_INT);
	else
		return NULL;
}

/* Number of columns in pg_hba_file_rules view */
#define NUM_PG_HBA_FILE_RULES_ATTS	 9

/*
 * fill_hba_line: build one row of pg_hba_file_rules view, add it to tuplestore
 *
 * tuple_store: where to store data
 * tupdesc: tuple descriptor for the view
 * lineno: pg_hba.conf line number (must always be valid)
 * hba: parsed line data (can be NULL, in which case err_msg should be set)
 * err_msg: error message (NULL if none)
 *
 * Note: leaks memory, but we don't care since this is run in a short-lived
 * memory context.
 */
static void
fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
			  int lineno, HbaLine *hba, const char *err_msg)
{
	Datum		values[NUM_PG_HBA_FILE_RULES_ATTS];
	bool		nulls[NUM_PG_HBA_FILE_RULES_ATTS];
	char		buffer[NI_MAXHOST];
	HeapTuple	tuple;
	int			index;
	ListCell   *lc;
	const char *typestr;
	const char *addrstr;
	const char *maskstr;
	ArrayType  *options;

	Assert(tupdesc->natts == NUM_PG_HBA_FILE_RULES_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	if (hba != NULL)
	{
		/* type */
		/* Avoid a default: case so compiler will warn about missing cases */
		typestr = NULL;
		switch (hba->conntype)
		{
			case ctLocal:
				typestr = "local";
				break;
			case ctHost:
				typestr = "host";
				break;
			case ctHostSSL:
				typestr = "hostssl";
				break;
			case ctHostNoSSL:
				typestr = "hostnossl";
				break;
			case ctHostGSS:
				typestr = "hostgssenc";
				break;
			case ctHostNoGSS:
				typestr = "hostnogssenc";
				break;
		}
		if (typestr)
			values[index++] = CStringGetTextDatum(typestr);
		else
			nulls[index++] = true;

		/* database */
		if (hba->databases)
		{
			/*
			 * Flatten HbaToken list to string list.  It might seem that we
			 * should re-quote any quoted tokens, but that has been rejected
			 * on the grounds that it makes it harder to compare the array
			 * elements to other system catalogs.  That makes entries like
			 * "all" or "samerole" formally ambiguous ... but users who name
			 * databases/roles that way are inflicting their own pain.
			 */
			List	   *names = NIL;

			foreach(lc, hba->databases)
			{
				HbaToken   *tok = lfirst(lc);

				names = lappend(names, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(names));
		}
		else
			nulls[index++] = true;

		/* user */
		if (hba->roles)
		{
			/* Flatten HbaToken list to string list; see comment above */
			List	   *roles = NIL;

			foreach(lc, hba->roles)
			{
				HbaToken   *tok = lfirst(lc);

				roles = lappend(roles, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(roles));
		}
		else
			nulls[index++] = true;

		/* address and netmask */
		/* Avoid a default: case so compiler will warn about missing cases */
		addrstr = maskstr = NULL;
		switch (hba->ip_cmp_method)
		{
			case ipCmpMask:
				if (hba->hostname)
				{
					addrstr = hba->hostname;
				}
				else
				{
					/*
					 * Note: if pg_getnameinfo_all fails, it'll set buffer to
					 * "???", which we want to return.
					 */
					if (hba->addrlen > 0)
					{
						if (pg_getnameinfo_all(&hba->addr, hba->addrlen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->addr.ss_family, buffer);
						addrstr = pstrdup(buffer);
					}
					if (hba->masklen > 0)
					{
						if (pg_getnameinfo_all(&hba->mask, hba->masklen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->mask.ss_family, buffer);
						maskstr = pstrdup(buffer);
					}
				}
				break;
			case ipCmpAll:
				addrstr = "all";
				break;
			case ipCmpSameHost:
				addrstr = "samehost";
				break;
			case ipCmpSameNet:
				addrstr = "samenet";
				break;
		}
		if (addrstr)
			values[index++] = CStringGetTextDatum(addrstr);
		else
			nulls[index++] = true;
		if (maskstr)
			values[index++] = CStringGetTextDatum(maskstr);
		else
			nulls[index++] = true;

		/*
		 * Make sure UserAuthName[] tracks additions to the UserAuth enum
		 */
		StaticAssertStmt(lengthof(UserAuthName) == USER_AUTH_LAST + 1,
						 "UserAuthName[] must match the UserAuth enum");

		/* auth_method */
		values[index++] = CStringGetTextDatum(UserAuthName[hba->auth_method]);

		/* options */
		options = gethba_options(hba);
		if (options)
			values[index++] = PointerGetDatum(options);
		else
			nulls[index++] = true;
	}
	else
	{
		/* no parsing result, so set relevant fields to nulls */
		memset(&nulls[1], true, (NUM_PG_HBA_FILE_RULES_ATTS - 2) * sizeof(bool));
	}

	/* error */
	if (err_msg)
		values[NUM_PG_HBA_FILE_RULES_ATTS - 1] = CStringGetTextDatum(err_msg);
	else
		nulls[NUM_PG_HBA_FILE_RULES_ATTS - 1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * Read the pg_hba.conf file and fill the tuplestore with view records.
 */
static void
fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	FILE	   *file;
	List	   *hba_lines = NIL;
	ListCell   *line;
	MemoryContext linecxt;
	MemoryContext hbacxt;
	MemoryContext oldcxt;

	/*
	 * In the unlikely event that we can't open pg_hba.conf, we throw an
	 * error, rather than trying to report it via some sort of view entry.
	 * (Most other error conditions should result in a message in a view
	 * entry.)
	 */
	file = AllocateFile(HbaFileName, "r");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open configuration file \"%s\": %m",
						HbaFileName)));

	linecxt = tokenize_file(HbaFileName, file, &hba_lines, DEBUG3);
	FreeFile(file);

	/* Now parse all the lines */
	hbacxt = AllocSetContextCreate(CurrentMemoryContext,
								   "hba parser context",
								   ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	foreach(line, hba_lines)
	{
		TokenizedLine *tok_line = (TokenizedLine *) lfirst(line);
		HbaLine    *hbaline = NULL;

		/* don't parse lines that already have errors */
		if (tok_line->err_msg == NULL)
			hbaline = parse_hba_line(tok_line, DEBUG3);

		fill_hba_line(tuple_store, tupdesc, tok_line->line_num,
					  hbaline, tok_line->err_msg);
	}

	/* Free tokenizer memory */
	MemoryContextDelete(linecxt);
	/* Free parse_hba_line memory */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(hbacxt);
}

/*
 * SQL-accessible SRF to return all the entries in the pg_hba.conf file.
 */
Datum
pg_hba_file_rules(PG_FUNCTION_ARGS)
{
	Tuplestorestate *tuple_store;
	TupleDesc	tupdesc;
	MemoryContext old_cxt;
	ReturnSetInfo *rsi;

	/*
	 * We must use the Materialize mode to be safe against HBA file changes
	 * while the cursor is open. It's also more efficient than having to look
	 * up our current position in the parsed list every time.
	 */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	/* Check to see if caller supports us returning a tuplestore */
	if (rsi == NULL || !IsA(rsi, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsi->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	rsi->returnMode = SFRM_Materialize;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Build tuplestore to hold the result rows */
	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);
	rsi->setDesc = tupdesc;
	rsi->setResult = tuple_store;

	MemoryContextSwitchTo(old_cxt);

	/* Fill the tuplestore */
	fill_hba_view(tuple_store, tupdesc);

	PG_RETURN_NULL();
}


/*
 * Parse one tokenised line from the ident config file and store the result in
 * an IdentLine structure.
 *
 * If parsing fails, log a message and return NULL.
 *
 * If ident_user is a regular expression (ie. begins with a slash), it is
 * compiled and stored in IdentLine structure.
 *
 * Note: this function leaks memory when an error occurs.  Caller is expected
 * to have set a memory context that will be reset if this function returns
 * NULL.
 */
static IdentLine *
parse_ident_line(TokenizedLine *tok_line)
{
	int			line_num = tok_line->line_num;
	ListCell   *field;
	List	   *tokens;
	HbaToken   *token;
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
			ereport(LOG,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("invalid regular expression \"%s\": %s",
							parsedline->ident_user + 1, errstr)));

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

	linecxt = tokenize_file(IdentFileName, file, &ident_lines, LOG);
	FreeFile(file);

	/* Now parse all the lines */
	Assert(PostmasterContext);
	ident_context = AllocSetContextCreate(PostmasterContext,
										  "ident parser context",
										  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(ident_context);
	foreach(line_cell, ident_lines)
	{
		TokenizedLine *tok_line = (TokenizedLine *) lfirst(line_cell);

		/* don't parse lines that already have errors */
		if (tok_line->err_msg != NULL)
		{
			ok = false;
			continue;
		}

		if ((newline = parse_ident_line(tok_line)) == NULL)
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
