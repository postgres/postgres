/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and choosing authentication method based on it).
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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

#include "catalog/pg_collation.h"
#include "libpq/ip.h"
#include "libpq/libpq.h"
#include "postmaster/postmaster.h"
#include "regex/regex.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#ifdef USE_LDAP
#ifdef WIN32
#include <winldap.h>
#else
#include <ldap.h>
#endif
#endif


#define atooid(x)  ((Oid) strtoul((x), NULL, 10))
#define atoxid(x)  ((TransactionId) strtoul((x), NULL, 10))

#define MAX_TOKEN	256
#define MAX_LINE	8192

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
 * A single string token lexed from the HBA config file, together with whether
 * the token had been quoted.
 */
typedef struct HbaToken
{
	char	   *string;
	bool		quoted;
} HbaToken;

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
 * memory context, they need to be expliticly free'd.
 */
static List *parsed_ident_lines = NIL;
static MemoryContext parsed_ident_context = NULL;


static MemoryContext tokenize_file(const char *filename, FILE *file,
			  List **lines, List **line_nums, List **raw_lines);
static List *tokenize_inc_file(List *tokens, const char *outer_filename,
				  const char *inc_filename);
static bool parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline,
				   int line_num);

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
 * Grab one token out of the string pointed to by lineptr.
 * Tokens are strings of non-blank
 * characters bounded by blank characters, commas, beginning of line, and
 * end of line. Blank means space or tab. Tokens can be delimited by
 * double quotes (this allows the inclusion of blanks, but not newlines).
 *
 * The token, if any, is returned at *buf (a buffer of size bufsz).
 * Also, we set *initial_quote to indicate whether there was quoting before
 * the first character.  (We use that to prevent "@x" from being treated
 * as a file inclusion request.  Note that @"x" should be so treated;
 * we want to allow that to support embedded spaces in file paths.)
 * We set *terminating_comma to indicate whether the token is terminated by a
 * comma (which is not returned.)
 *
 * If successful: store null-terminated token at *buf and return TRUE.
 * If no more tokens on line: set *buf = '\0' and return FALSE.
 *
 * Leave file positioned at the character immediately after the token or EOF,
 * whichever comes first. If no more tokens on line, position the file to the
 * beginning of the next line or EOF, whichever comes first.
 *
 * Handle comments.
 */
static bool
next_token(char **lineptr, char *buf, int bufsz, bool *initial_quote,
		   bool *terminating_comma)
{
	int			c;
	char	   *start_buf = buf;
	char	   *end_buf = buf + (bufsz - 2);
	bool		in_quote = false;
	bool		was_quote = false;
	bool		saw_quote = false;

	/* end_buf reserves two bytes to ensure we can append \n and \0 */
	Assert(end_buf > start_buf);

	*initial_quote = false;
	*terminating_comma = false;

	/* Move over initial whitespace and commas */
	while ((c = (*(*lineptr)++)) != '\0' && (pg_isblank(c) || c == ','))
		;

	if (c == '\0' || c == '\n')
	{
		*buf = '\0';
		return false;
	}

	/*
	 * Build a token in buf of next characters up to EOF, EOL, unquoted comma,
	 * or unquoted whitespace.
	 */
	while (c != '\0' && c != '\n' &&
		   (!pg_isblank(c) || in_quote))
	{
		/* skip comments to EOL */
		if (c == '#' && !in_quote)
		{
			while ((c = (*(*lineptr)++)) != '\0' && c != '\n')
				;
			/* If only comment, consume EOL too; return EOL */
			if (c != '\0' && buf == start_buf)
				(*lineptr)++;
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
			while ((c = (*(*lineptr)++)) != '\0' && c != '\n')
				;
			break;
		}

		/* we do not pass back the comma in the token */
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
	 * Put back the char right after the token (critical in case it is EOL,
	 * since we need to detect end-of-line at next call).
	 */
	(*lineptr)--;

	*buf = '\0';

	return (saw_quote || buf > start_buf);
}

static HbaToken *
make_hba_token(char *token, bool quoted)
{
	HbaToken   *hbatoken;
	int			toklen;

	toklen = strlen(token);
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
 * The result is a List of HbaToken structs for each individual token,
 * or NIL if we reached EOL.
 */
static List *
next_field_expand(const char *filename, char **lineptr)
{
	char		buf[MAX_TOKEN];
	bool		trailing_comma;
	bool		initial_quote;
	List	   *tokens = NIL;

	do
	{
		if (!next_token(lineptr, buf, sizeof(buf), &initial_quote, &trailing_comma))
			break;

		/* Is this referencing a file? */
		if (!initial_quote && buf[0] == '@' && buf[1] != '\0')
			tokens = tokenize_inc_file(tokens, filename, buf + 1);
		else
			tokens = lappend(tokens, make_hba_token(buf, initial_quote));
	} while (trailing_comma);

	return tokens;
}

/*
 * tokenize_inc_file
 *		Expand a file included from another file into an hba "field"
 *
 * Opens and tokenises a file included from another HBA config file with @,
 * and returns all values found therein as a flat list of HbaTokens.  If a
 * @-token is found, recursively expand it.  The given token list is used as
 * initial contents of list (so foo,bar,@baz does what you expect).
 */
static List *
tokenize_inc_file(List *tokens,
				  const char *outer_filename,
				  const char *inc_filename)
{
	char	   *inc_fullname;
	FILE	   *inc_file;
	List	   *inc_lines;
	List	   *inc_line_nums;
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
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open secondary authentication file \"@%s\" as \"%s\": %m",
						inc_filename, inc_fullname)));
		pfree(inc_fullname);
		return tokens;
	}

	/* There is possible recursion here if the file contains @ */
	linecxt = tokenize_file(inc_fullname, inc_file, &inc_lines, &inc_line_nums, NULL);

	FreeFile(inc_file);
	pfree(inc_fullname);

	foreach(inc_line, inc_lines)
	{
		List	   *inc_fields = lfirst(inc_line);
		ListCell   *inc_field;

		foreach(inc_field, inc_fields)
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
 * Tokenize the given file, storing the resulting data into three Lists: a
 * List of lines, a List of line numbers, and a List of raw line contents.
 *
 * The list of lines is a triple-nested List structure.  Each line is a List of
 * fields, and each field is a List of HbaTokens.
 *
 * filename must be the absolute path to the target file.
 *
 * Return value is a memory context which contains all memory allocated by
 * this function.
 */
static MemoryContext
tokenize_file(const char *filename, FILE *file,
			  List **lines, List **line_nums, List **raw_lines)
{
	List	   *current_line = NIL;
	List	   *current_field = NIL;
	int			line_number = 1;
	MemoryContext linecxt;
	MemoryContext oldcxt;

	linecxt = AllocSetContextCreate(CurrentMemoryContext,
									"tokenize file cxt",
									ALLOCSET_DEFAULT_MINSIZE,
									ALLOCSET_DEFAULT_INITSIZE,
									ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(linecxt);

	*lines = *line_nums = NIL;

	while (!feof(file) && !ferror(file))
	{
		char		rawline[MAX_LINE];
		char	   *lineptr;

		if (!fgets(rawline, sizeof(rawline), file))
			break;
		if (strlen(rawline) == MAX_LINE - 1)
			/* Line too long! */
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication file line too long"),
					 errcontext("line %d of configuration file \"%s\"",
								line_number, filename)));

		/* Strip trailing linebreak from rawline */
		lineptr = rawline + strlen(rawline) - 1;
		while (lineptr >= rawline && (*lineptr == '\n' || *lineptr == '\r'))
			*lineptr-- = '\0';

		lineptr = rawline;
		while (strlen(lineptr) > 0)
		{
			current_field = next_field_expand(filename, &lineptr);

			/* add tokens to list, unless we are at EOL or comment start */
			if (list_length(current_field) > 0)
			{
				if (current_line == NIL)
				{
					/* make a new line List, record its line number */
					current_line = lappend(current_line, current_field);
					*lines = lappend(*lines, current_line);
					*line_nums = lappend_int(*line_nums, line_number);
					if (raw_lines)
						*raw_lines = lappend(*raw_lines, pstrdup(rawline));
				}
				else
				{
					/* append tokens to current line's list */
					current_line = lappend(current_line, current_field);
				}
			}
		}
		/* we are at real or logical EOL, so force a new line List */
		current_line = NIL;
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
		if (am_walsender)
		{
			/* walsender connections can only match replication keyword */
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
ipv4eq(struct sockaddr_in * a, struct sockaddr_in * b)
{
	return (a->sin_addr.s_addr == b->sin_addr.s_addr);
}

#ifdef HAVE_IPV6

static bool
ipv6eq(struct sockaddr_in6 * a, struct sockaddr_in6 * b)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (a->sin6_addr.s6_addr[i] != b->sin6_addr.s6_addr[i])
			return false;

	return true;
}
#endif   /* HAVE_IPV6 */

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
						   (struct sockaddr_in *) & port->raddr.addr))
				{
					found = true;
					break;
				}
			}
#ifdef HAVE_IPV6
			else if (gai->ai_addr->sa_family == AF_INET6)
			{
				if (ipv6eq((struct sockaddr_in6 *) gai->ai_addr,
						   (struct sockaddr_in6 *) & port->raddr.addr))
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
check_ip(SockAddr *raddr, struct sockaddr * addr, struct sockaddr * mask)
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
check_network_callback(struct sockaddr * addr, struct sockaddr * netmask,
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
		cn->result = check_ip(cn->raddr, addr, (struct sockaddr *) & mask);
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
 * INVALID_AUTH_OPTION = reports when an option is specified for a method where it's
 *						 not supported.
 * REQUIRE_AUTH_OPTION = same as INVALID_AUTH_OPTION, except it also checks if the
 *						 method is actually the one specified. Used as a shortcut when
 *						 the option is only valid for one authentication method.
 * MANDATORY_AUTH_ARG  = check if a required option is set for an authentication method,
 *						 reporting error if it's not.
 */
#define INVALID_AUTH_OPTION(optname, validmethods) do {\
	ereport(LOG, \
			(errcode(ERRCODE_CONFIG_FILE_ERROR), \
			 /* translator: the second %s is a list of auth methods */ \
			 errmsg("authentication option \"%s\" is only valid for authentication methods %s", \
					optname, _(validmethods)), \
			 errcontext("line %d of configuration file \"%s\"", \
					line_num, HbaFileName))); \
	return false; \
} while (0);

#define REQUIRE_AUTH_OPTION(methodval, optname, validmethods) do {\
	if (hbaline->auth_method != methodval) \
		INVALID_AUTH_OPTION(optname, validmethods); \
} while (0);

#define MANDATORY_AUTH_ARG(argvar, argname, authname) do {\
	if (argvar == NULL) {\
		ereport(LOG, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("authentication method \"%s\" requires argument \"%s\" to be set", \
						authname, argname), \
				 errcontext("line %d of configuration file \"%s\"", \
						line_num, HbaFileName))); \
		return NULL; \
	} \
} while (0);

/*
 * IDENT_FIELD_ABSENT:
 * Throw an error and exit the function if the given ident field ListCell is
 * not populated.
 *
 * IDENT_MULTI_VALUE:
 * Throw an error and exit the function if the given ident token List has more
 * than one element.
 */
#define IDENT_FIELD_ABSENT(field) do {\
	if (!field) { \
		ereport(LOG, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("missing entry in file \"%s\" at end of line %d", \
						IdentFileName, line_number))); \
		return NULL; \
	} \
} while (0);

#define IDENT_MULTI_VALUE(tokens) do {\
	if (tokens->length > 1) { \
		ereport(LOG, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("multiple values in ident field"), \
				 errcontext("line %d of configuration file \"%s\"", \
						line_number, IdentFileName))); \
		return NULL; \
	} \
} while (0);


/*
 * Parse one tokenised line from the hba config file and store the result in a
 * HbaLine structure, or NULL if parsing fails.
 *
 * The tokenised line is a List of fields, each field being a List of
 * HbaTokens.
 *
 * Note: this function leaks memory when an error occurs.  Caller is expected
 * to have set a memory context that will be reset if this function returns
 * NULL.
 */
static HbaLine *
parse_hba_line(List *line, int line_num, char *raw_line)
{
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
	parsedline->rawline = pstrdup(raw_line);

	/* Check the record type. */
	field = list_head(line);
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("multiple values specified for connection type"),
				 errhint("Specify exactly one connection type per line."),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return NULL;
	}
	token = linitial(tokens);
	if (strcmp(token->string, "local") == 0)
	{
#ifdef HAVE_UNIX_SOCKETS
		parsedline->conntype = ctLocal;
#else
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("local connections are not supported by this build"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return NULL;
#endif
	}
	else if (strcmp(token->string, "host") == 0 ||
			 strcmp(token->string, "hostssl") == 0 ||
			 strcmp(token->string, "hostnossl") == 0)
	{

		if (token->string[4] == 's')	/* "hostssl" */
		{
			/* SSL support must be actually active, else complain */
#ifdef USE_SSL
			if (EnableSSL)
				parsedline->conntype = ctHostSSL;
			else
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("hostssl requires SSL to be turned on"),
						 errhint("Set ssl = on in postgresql.conf."),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				return NULL;
			}
#else
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("hostssl is not supported by this build"),
			  errhint("Compile with --with-openssl to use SSL connections."),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return NULL;
#endif
		}
		else if (token->string[4] == 'n')		/* "hostnossl" */
		{
			parsedline->conntype = ctHostNoSSL;
		}
		else
		{
			/* "host" */
			parsedline->conntype = ctHost;
		}
	}							/* record type */
	else
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid connection type \"%s\"",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return NULL;
	}

	/* Get the databases. */
	field = lnext(field);
	if (!field)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before database specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
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
	field = lnext(field);
	if (!field)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before role specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
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
		field = lnext(field);
		if (!field)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("end-of-line before IP address specification"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return NULL;
		}
		tokens = lfirst(field);
		if (tokens->length > 1)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("multiple values specified for host address"),
					 errhint("Specify one address range per line."),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
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
				memcpy(&parsedline->addr, gai_result->ai_addr,
					   gai_result->ai_addrlen);
			else if (ret == EAI_NONAME)
				parsedline->hostname = str;
			else
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("invalid IP address \"%s\": %s",
								str, gai_strerror(ret)),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
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
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("specifying both host name and CIDR mask is invalid: \"%s\"",
									token->string),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					return NULL;
				}

				if (pg_sockaddr_cidr_mask(&parsedline->mask, cidr_slash + 1,
										  parsedline->addr.ss_family) < 0)
				{
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid CIDR mask in address \"%s\"",
									token->string),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					return NULL;
				}
				pfree(str);
			}
			else if (!parsedline->hostname)
			{
				/* Read the mask field. */
				pfree(str);
				field = lnext(field);
				if (!field)
				{
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
						  errmsg("end-of-line before netmask specification"),
							 errhint("Specify an address range in CIDR notation, or provide a separate netmask."),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					return NULL;
				}
				tokens = lfirst(field);
				if (tokens->length > 1)
				{
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("multiple values specified for netmask"),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					return NULL;
				}
				token = linitial(tokens);

				ret = pg_getaddrinfo_all(token->string, NULL,
										 &hints, &gai_result);
				if (ret || !gai_result)
				{
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid IP mask \"%s\": %s",
									token->string, gai_strerror(ret)),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					if (gai_result)
						pg_freeaddrinfo_all(hints.ai_family, gai_result);
					return NULL;
				}

				memcpy(&parsedline->mask, gai_result->ai_addr,
					   gai_result->ai_addrlen);
				pg_freeaddrinfo_all(hints.ai_family, gai_result);

				if (parsedline->addr.ss_family != parsedline->mask.ss_family)
				{
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("IP address and mask do not match"),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					return NULL;
				}
			}
		}
	}							/* != ctLocal */

	/* Get the authentication method */
	field = lnext(field);
	if (!field)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before authentication method"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return NULL;
	}
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("multiple values specified for authentication type"),
				 errhint("Specify exactly one authentication type per line."),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
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
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("MD5 authentication is not supported when \"db_user_namespace\" is enabled"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return NULL;
		}
		parsedline->auth_method = uaMD5;
	}
	else if (strcmp(token->string, "pam") == 0)
#ifdef USE_PAM
		parsedline->auth_method = uaPAM;
#else
		unsupauth = "pam";
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
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\"",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return NULL;
	}

	if (unsupauth)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\": not supported by this build",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
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
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
		   errmsg("gssapi authentication is not supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return NULL;
	}

	if (parsedline->conntype != ctLocal &&
		parsedline->auth_method == uaPeer)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
			errmsg("peer authentication is only supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return NULL;
	}

	/*
	 * SSPI authentication can never be enabled on ctLocal connections,
	 * because it's only supported on Windows, where ctLocal isn't supported.
	 */


	if (parsedline->conntype != ctHostSSL &&
		parsedline->auth_method == uaCert)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("cert authentication is only supported on hostssl connections"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
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

	/* Parse remaining arguments */
	while ((field = lnext(field)) != NULL)
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
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("authentication option not in name=value format: %s", token->string),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				return NULL;
			}

			*val++ = '\0';		/* str now holds "name", val holds "value" */
			if (!parse_hba_auth_opt(str, val, parsedline, line_num))
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
		MANDATORY_AUTH_ARG(parsedline->ldapserver, "ldapserver", "ldap");

		/*
		 * LDAP can operate in two modes: either with a direct bind, using
		 * ldapprefix and ldapsuffix, or using a search+bind, using
		 * ldapbasedn, ldapbinddn, ldapbindpasswd and ldapsearchattribute.
		 * Disallow mixing these parameters.
		 */
		if (parsedline->ldapprefix || parsedline->ldapsuffix)
		{
			if (parsedline->ldapbasedn ||
				parsedline->ldapbinddn ||
				parsedline->ldapbindpasswd ||
				parsedline->ldapsearchattribute)
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("cannot use ldapbasedn, ldapbinddn, ldapbindpasswd, ldapsearchattribute, or ldapurl together with ldapprefix"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				return NULL;
			}
		}
		else if (!parsedline->ldapbasedn)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication method \"ldap\" requires argument \"ldapbasedn\", \"ldapprefix\", or \"ldapsuffix\" to be set"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return NULL;
		}
	}

	if (parsedline->auth_method == uaRADIUS)
	{
		MANDATORY_AUTH_ARG(parsedline->radiusserver, "radiusserver", "radius");
		MANDATORY_AUTH_ARG(parsedline->radiussecret, "radiussecret", "radius");
	}

	/*
	 * Enforce any parameters implied by other settings.
	 */
	if (parsedline->auth_method == uaCert)
	{
		parsedline->clientcert = true;
	}

	return parsedline;
}

/*
 * Parse one name-value pair as an authentication option into the given
 * HbaLine.  Return true if we successfully parse the option, false if we
 * encounter an error.
 */
static bool
parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline, int line_num)
{
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
		/*
		 * Since we require ctHostSSL, this really can never happen on
		 * non-SSL-enabled builds, so don't bother checking for USE_SSL.
		 */
		if (hbaline->conntype != ctHostSSL)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
			errmsg("clientcert can only be configured for \"hostssl\" rows"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}
		if (strcmp(val, "1") == 0)
		{
			if (!secure_loaded_verify_locations())
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("client certificates can only be checked if a root certificate store is available"),
						 errhint("Make sure the configuration parameter \"%s\" is set.", "ssl_ca_file"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				return false;
			}
			hbaline->clientcert = true;
		}
		else
		{
			if (hbaline->auth_method == uaCert)
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("clientcert can not be set to 0 when using \"cert\" authentication"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				return false;
			}
			hbaline->clientcert = false;
		}
	}
	else if (strcmp(name, "pamservice") == 0)
	{
		REQUIRE_AUTH_OPTION(uaPAM, "pamservice", "pam");
		hbaline->pamservice = pstrdup(val);
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
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse LDAP URL \"%s\": %s", val, ldap_err2string(rc))));
			return false;
		}

		if (strcmp(urldata->lud_scheme, "ldap") != 0)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
			errmsg("unsupported LDAP URL scheme: %s", urldata->lud_scheme)));
			ldap_free_urldesc(urldata);
			return false;
		}

		hbaline->ldapserver = pstrdup(urldata->lud_host);
		hbaline->ldapport = urldata->lud_port;
		hbaline->ldapbasedn = pstrdup(urldata->lud_dn);

		if (urldata->lud_attrs)
			hbaline->ldapsearchattribute = pstrdup(urldata->lud_attrs[0]);		/* only use first one */
		hbaline->ldapscope = urldata->lud_scope;
		if (urldata->lud_filter)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("filters not supported in LDAP URLs")));
			ldap_free_urldesc(urldata);
			return false;
		}
		ldap_free_urldesc(urldata);
#else							/* not OpenLDAP */
		ereport(LOG,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LDAP URLs not supported on this platform")));
#endif   /* not OpenLDAP */
	}
	else if (strcmp(name, "ldaptls") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldaptls", "ldap");
		if (strcmp(val, "1") == 0)
			hbaline->ldaptls = true;
		else
			hbaline->ldaptls = false;
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
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid LDAP port number: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
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
	else if (strcmp(name, "radiusserver") == 0)
	{
		struct addrinfo *gai_result;
		struct addrinfo hints;
		int			ret;

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusserver", "radius");

		MemSet(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_family = AF_UNSPEC;

		ret = pg_getaddrinfo_all(val, NULL, &hints, &gai_result);
		if (ret || !gai_result)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not translate RADIUS server name \"%s\" to address: %s",
							val, gai_strerror(ret)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			if (gai_result)
				pg_freeaddrinfo_all(hints.ai_family, gai_result);
			return false;
		}
		pg_freeaddrinfo_all(hints.ai_family, gai_result);
		hbaline->radiusserver = pstrdup(val);
	}
	else if (strcmp(name, "radiusport") == 0)
	{
		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusport", "radius");
		hbaline->radiusport = atoi(val);
		if (hbaline->radiusport == 0)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid RADIUS port number: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}
	}
	else if (strcmp(name, "radiussecret") == 0)
	{
		REQUIRE_AUTH_OPTION(uaRADIUS, "radiussecret", "radius");
		hbaline->radiussecret = pstrdup(val);
	}
	else if (strcmp(name, "radiusidentifier") == 0)
	{
		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusidentifier", "radius");
		hbaline->radiusidentifier = pstrdup(val);
	}
	else
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("unrecognized authentication option name: \"%s\"",
						name),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
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
									  (struct sockaddr *) & hba->addr,
									  (struct sockaddr *) & hba->mask))
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
	List	   *hba_line_nums = NIL;
	List	   *hba_raw_lines = NIL;
	ListCell   *line,
			   *line_num,
			   *raw_line;
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

	linecxt = tokenize_file(HbaFileName, file, &hba_lines, &hba_line_nums, &hba_raw_lines);
	FreeFile(file);

	/* Now parse all the lines */
	Assert(PostmasterContext);
	hbacxt = AllocSetContextCreate(PostmasterContext,
								   "hba parser context",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	forthree(line, hba_lines, line_num, hba_line_nums, raw_line, hba_raw_lines)
	{
		HbaLine    *newline;

		if ((newline = parse_hba_line(lfirst(line), lfirst_int(line_num), lfirst(raw_line))) == NULL)
		{
			/*
			 * Parse error in the file, so indicate there's a problem.  NB: a
			 * problem in a line will free the memory for all previous lines
			 * as well!
			 */
			MemoryContextReset(hbacxt);
			new_parsed_lines = NIL;
			ok = false;

			/*
			 * Keep parsing the rest of the file so we can report errors on
			 * more than the first row. Error has already been reported in the
			 * parsing function, so no need to log it here.
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
 * an IdentLine structure, or NULL if parsing fails.
 *
 * The tokenised line is a nested List of fields and tokens.
 *
 * If ident_user is a regular expression (ie. begins with a slash), it is
 * compiled and stored in IdentLine structure.
 *
 * Note: this function leaks memory when an error occurs.  Caller is expected
 * to have set a memory context that will be reset if this function returns
 * NULL.
 */
static IdentLine *
parse_ident_line(List *line, int line_number)
{
	ListCell   *field;
	List	   *tokens;
	HbaToken   *token;
	IdentLine  *parsedline;

	Assert(line != NIL);
	field = list_head(line);

	parsedline = palloc0(sizeof(IdentLine));
	parsedline->linenumber = line_number;

	/* Get the map token (must exist) */
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->usermap = pstrdup(token->string);

	/* Get the ident user token */
	field = lnext(field);
	IDENT_FIELD_ABSENT(field);
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->ident_user = pstrdup(token->string);

	/* Get the PG rolename token */
	field = lnext(field);
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
	return;
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
	List	   *ident_line_nums = NIL;
	ListCell   *line_cell,
			   *num_cell,
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

	linecxt = tokenize_file(IdentFileName, file, &ident_lines, &ident_line_nums, NULL);
	FreeFile(file);

	/* Now parse all the lines */
	Assert(PostmasterContext);
	ident_context = AllocSetContextCreate(PostmasterContext,
										  "ident parser context",
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(ident_context);
	forboth(line_cell, ident_lines, num_cell, ident_line_nums)
	{
		if ((newline = parse_ident_line(lfirst(line_cell), lfirst_int(num_cell))) == NULL)
		{
			/*
			 * Parse error in the file, so indicate there's a problem.  Free
			 * all the memory and regular expressions of lines parsed so far.
			 */
			foreach(parsed_line_cell, new_parsed_lines)
			{
				newline = (IdentLine *) lfirst(parsed_line_cell);
				if (newline->ident_user[0] == '/')
					pg_regfree(&newline->re);
			}
			MemoryContextReset(ident_context);
			new_parsed_lines = NIL;
			ok = false;

			/*
			 * Keep parsing the rest of the file so we can report errors on
			 * more than the first row. Error has already been reported in the
			 * parsing function, so no need to log it here.
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
		/* File contained one or more errors, so bail out */
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
