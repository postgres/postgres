/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and choosing authentication method based on it).
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/libpq/hba.c,v 1.188.2.5 2010/03/24 17:05:51 tgl Exp $
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

#include "libpq/ip.h"
#include "libpq/libpq.h"
#include "postmaster/postmaster.h"
#include "regex/regex.h"
#include "storage/fd.h"
#include "utils/flatfiles.h"
#include "utils/guc.h"



#define atooid(x)  ((Oid) strtoul((x), NULL, 10))
#define atoxid(x)  ((TransactionId) strtoul((x), NULL, 10))

/* This is used to separate values in multi-valued column strings */
#define MULTI_VALUE_SEP "\001"

#define MAX_TOKEN	256

/* pre-parsed content of HBA config file */
static List *parsed_hba_lines = NIL;

/*
 * These variables hold the pre-parsed contents of the ident
 * configuration files, as well as the flat auth file.
 * Each is a list of sublists, one sublist for
 * each (non-empty, non-comment) line of the file.	Each sublist's
 * first item is an integer line number (so we can give somewhat-useful
 * location info in error messages).  Remaining items are palloc'd strings,
 * one string per token on the line.  Note there will always be at least
 * one token, since blank lines are not entered in the data structure.
 */

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
bool
pg_isblank(const char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}


/*
 * Grab one token out of fp. Tokens are strings of non-blank
 * characters bounded by blank characters, commas, beginning of line, and
 * end of line. Blank means space or tab. Tokens can be delimited by
 * double quotes (this allows the inclusion of blanks, but not newlines).
 *
 * The token, if any, is returned at *buf (a buffer of size bufsz).
 * Also, we set *initial_quote to indicate whether there was quoting before
 * the first character.  (We use that to prevent "@x" from being treated
 * as a file inclusion request.  Note that @"x" should be so treated;
 * we want to allow that to support embedded spaces in file paths.)
 *
 * If successful: store null-terminated token at *buf and return TRUE.
 * If no more tokens on line: set *buf = '\0' and return FALSE.
 *
 * Leave file positioned at the character immediately after the token or EOF,
 * whichever comes first. If no more tokens on line, position the file to the
 * beginning of the next line or EOF, whichever comes first.
 *
 * Handle comments. Treat unquoted keywords that might be role names or
 * database names specially, by appending a newline to them.  Also, when
 * a token is terminated by a comma, the comma is included in the returned
 * token.
 */
static bool
next_token(FILE *fp, char *buf, int bufsz, bool *initial_quote)
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
		   (!pg_isblank(c) || in_quote))
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

		if (c != '"' || was_quote)
			*buf++ = c;

		/* We pass back the comma so the caller knows there is more */
		if (c == ',' && !in_quote)
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
			if (buf == start_buf)
				*initial_quote = true;
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
	bool		initial_quote;
	char	   *incbuf;
	int			needed;

	do
	{
		if (!next_token(file, buf, sizeof(buf), &initial_quote))
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
		if (!initial_quote && buf[0] == '@' && buf[1] != '\0')
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

	while (!feof(file) && !ferror(file))
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
	if (parsedline->auth_method != methodval) \
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
		return false; \
	} \
} while (0);


/*
 * Parse one line in the hba config file and store the result in
 * a HbaLine structure.
 */
static bool
parse_hba_line(List *line, int line_num, HbaLine *parsedline)
{
	char	   *token;
	struct addrinfo *gai_result;
	struct addrinfo hints;
	int			ret;
	char	   *cidr_slash;
	char	   *unsupauth;
	ListCell   *line_item;

	line_item = list_head(line);

	parsedline->linenumber = line_num;

	/* Check the record type. */
	token = lfirst(line_item);
	if (strcmp(token, "local") == 0)
	{
		parsedline->conntype = ctLocal;
	}
	else if (strcmp(token, "host") == 0
			 || strcmp(token, "hostssl") == 0
			 || strcmp(token, "hostnossl") == 0)
	{

		if (token[4] == 's')	/* "hostssl" */
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
				return false;
			}
#else
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("hostssl not supported on this platform"),
				 errhint("compile with --enable-ssl to use SSL connections"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
#endif
		}
#ifdef USE_SSL
		else if (token[4] == 'n')		/* "hostnossl" */
		{
			parsedline->conntype = ctHostNoSSL;
		}
#endif
		else
		{
			/* "host", or "hostnossl" and SSL support not built in */
			parsedline->conntype = ctHost;
		}
	}							/* record type */
	else
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid connection type \"%s\"",
						token),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}

	/* Get the database. */
	line_item = lnext(line_item);
	if (!line_item)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before database specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}
	parsedline->database = pstrdup(lfirst(line_item));

	/* Get the role. */
	line_item = lnext(line_item);
	if (!line_item)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before role specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}
	parsedline->role = pstrdup(lfirst(line_item));

	if (parsedline->conntype != ctLocal)
	{
		/* Read the IP address field. (with or without CIDR netmask) */
		line_item = lnext(line_item);
		if (!line_item)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("end-of-line before IP address specification"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}

		/* need a modifiable copy of token */
		token = pstrdup(lfirst(line_item));

		/* Check if it has a CIDR suffix and if so isolate it */
		cidr_slash = strchr(token, '/');
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

		ret = pg_getaddrinfo_all(token, NULL, &hints, &gai_result);
		if (ret || !gai_result)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid IP address \"%s\": %s",
							token, gai_strerror(ret)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			if (gai_result)
				pg_freeaddrinfo_all(hints.ai_family, gai_result);
			pfree(token);
			return false;
		}

		memcpy(&parsedline->addr, gai_result->ai_addr, gai_result->ai_addrlen);
		pg_freeaddrinfo_all(hints.ai_family, gai_result);

		/* Get the netmask */
		if (cidr_slash)
		{
			if (pg_sockaddr_cidr_mask(&parsedline->mask, cidr_slash + 1,
									  parsedline->addr.ss_family) < 0)
			{
				*cidr_slash = '/';			/* restore token for message */
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("invalid CIDR mask in address \"%s\"",
								token),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				pfree(token);
				return false;
			}
			pfree(token);
		}
		else
		{
			/* Read the mask field. */
			pfree(token);
			line_item = lnext(line_item);
			if (!line_item)
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("end-of-line before netmask specification"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				return false;
			}
			token = lfirst(line_item);

			ret = pg_getaddrinfo_all(token, NULL, &hints, &gai_result);
			if (ret || !gai_result)
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("invalid IP mask \"%s\": %s",
								token, gai_strerror(ret)),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				if (gai_result)
					pg_freeaddrinfo_all(hints.ai_family, gai_result);
				return false;
			}

			memcpy(&parsedline->mask, gai_result->ai_addr, gai_result->ai_addrlen);
			pg_freeaddrinfo_all(hints.ai_family, gai_result);

			if (parsedline->addr.ss_family != parsedline->mask.ss_family)
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("IP address and mask do not match in file \"%s\" line %d",
								HbaFileName, line_num)));
				return false;
			}
		}
	}							/* != ctLocal */

	/* Get the authentication method */
	line_item = lnext(line_item);
	if (!line_item)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before authentication method"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}
	token = lfirst(line_item);

	unsupauth = NULL;
	if (strcmp(token, "trust") == 0)
		parsedline->auth_method = uaTrust;
	else if (strcmp(token, "ident") == 0)
		parsedline->auth_method = uaIdent;
	else if (strcmp(token, "password") == 0)
		parsedline->auth_method = uaPassword;
	else if (strcmp(token, "krb5") == 0)
#ifdef KRB5
		parsedline->auth_method = uaKrb5;
#else
		unsupauth = "krb5";
#endif
	else if (strcmp(token, "gss") == 0)
#ifdef ENABLE_GSS
		parsedline->auth_method = uaGSS;
#else
		unsupauth = "gss";
#endif
	else if (strcmp(token, "sspi") == 0)
#ifdef ENABLE_SSPI
		parsedline->auth_method = uaSSPI;
#else
		unsupauth = "sspi";
#endif
	else if (strcmp(token, "reject") == 0)
		parsedline->auth_method = uaReject;
	else if (strcmp(token, "md5") == 0)
	{
		if (Db_user_namespace)
		{
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("MD5 authentication is not supported when \"db_user_namespace\" is enabled")));
			return false;
		}
		parsedline->auth_method = uaMD5;
	}
	else if (strcmp(token, "pam") == 0)
#ifdef USE_PAM
		parsedline->auth_method = uaPAM;
#else
		unsupauth = "pam";
#endif
	else if (strcmp(token, "ldap") == 0)
#ifdef USE_LDAP
		parsedline->auth_method = uaLDAP;
#else
		unsupauth = "ldap";
#endif
	else if (strcmp(token, "cert") == 0)
#ifdef USE_SSL
		parsedline->auth_method = uaCert;
#else
		unsupauth = "cert";
#endif
	else
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\"",
						token),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}

	if (unsupauth)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\": not supported on this platform",
						token),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}

	/* Invalid authentication combinations */
	if (parsedline->conntype == ctLocal &&
		parsedline->auth_method == uaKrb5)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg("krb5 authentication is not supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}

	if (parsedline->conntype == ctLocal &&
		parsedline->auth_method == uaGSS)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 errmsg("gssapi authentication is not supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}
	/*
	 * SSPI authentication can never be enabled on ctLocal connections, because
	 * it's only supported on Windows, where ctLocal isn't supported.
	 */


	if (parsedline->conntype != ctHostSSL &&
		parsedline->auth_method == uaCert)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("cert authentication is only supported on hostssl connections"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, HbaFileName)));
		return false;
	}

	/* Parse remaining arguments */
	while ((line_item = lnext(line_item)) != NULL)
	{
		char	   *c;

		token = lfirst(line_item);

		c = strchr(token, '=');
		if (c == NULL)
		{
			/*
			 * Got something that's not a name=value pair.
			 *
			 * XXX: attempt to do some backwards compatible parsing here?
			 */
			ereport(LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication option not in name=value format: %s", token),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, HbaFileName)));
			return false;
		}
		else
		{
			*c++ = '\0';		/* token now holds "name", c holds "value" */
			if (strcmp(token, "map") == 0)
			{
				if (parsedline->auth_method != uaIdent &&
					parsedline->auth_method != uaKrb5 &&
					parsedline->auth_method != uaGSS &&
					parsedline->auth_method != uaSSPI &&
					parsedline->auth_method != uaCert)
					INVALID_AUTH_OPTION("map", gettext_noop("ident, krb5, gssapi, sspi and cert"));
				parsedline->usermap = pstrdup(c);
			}
			else if (strcmp(token, "clientcert") == 0)
			{
				/*
				 * Since we require ctHostSSL, this really can never happen on
				 * non-SSL-enabled builds, so don't bother checking for
				 * USE_SSL.
				 */
				if (parsedline->conntype != ctHostSSL)
				{
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("clientcert can only be configured for \"hostssl\" rows"),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					return false;
				}
				if (strcmp(c, "1") == 0)
				{
					if (!secure_loaded_verify_locations())
					{
						ereport(LOG,
								(errcode(ERRCODE_CONFIG_FILE_ERROR),
								 errmsg("client certificates can only be checked if a root certificate store is available"),
								 errdetail("make sure the root certificate store is present and readable"),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
						return false;
					}
					parsedline->clientcert = true;
				}
				else
				{
					if (parsedline->auth_method == uaCert)
					{
						ereport(LOG,
								(errcode(ERRCODE_CONFIG_FILE_ERROR),
								 errmsg("clientcert can not be set to 0 when using \"cert\" authentication"),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
						return false;
					}
					parsedline->clientcert = false;
				}
			}
			else if (strcmp(token, "pamservice") == 0)
			{
				REQUIRE_AUTH_OPTION(uaPAM, "pamservice", "pam");
				parsedline->pamservice = pstrdup(c);
			}
			else if (strcmp(token, "ldaptls") == 0)
			{
				REQUIRE_AUTH_OPTION(uaLDAP, "ldaptls", "ldap");
				if (strcmp(c, "1") == 0)
					parsedline->ldaptls = true;
				else
					parsedline->ldaptls = false;
			}
			else if (strcmp(token, "ldapserver") == 0)
			{
				REQUIRE_AUTH_OPTION(uaLDAP, "ldapserver", "ldap");
				parsedline->ldapserver = pstrdup(c);
			}
			else if (strcmp(token, "ldapport") == 0)
			{
				REQUIRE_AUTH_OPTION(uaLDAP, "ldapport", "ldap");
				parsedline->ldapport = atoi(c);
				if (parsedline->ldapport == 0)
				{
					ereport(LOG,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid LDAP port number: \"%s\"", c),
						   errcontext("line %d of configuration file \"%s\"",
									  line_num, HbaFileName)));
					return false;
				}
			}
			else if (strcmp(token, "ldapprefix") == 0)
			{
				REQUIRE_AUTH_OPTION(uaLDAP, "ldapprefix", "ldap");
				parsedline->ldapprefix = pstrdup(c);
			}
			else if (strcmp(token, "ldapsuffix") == 0)
			{
				REQUIRE_AUTH_OPTION(uaLDAP, "ldapsuffix", "ldap");
				parsedline->ldapsuffix = pstrdup(c);
			}
			else if (strcmp(token, "krb_server_hostname") == 0)
			{
				REQUIRE_AUTH_OPTION(uaKrb5, "krb_server_hostname", "krb5");
				parsedline->krb_server_hostname = pstrdup(c);
			}
			else if (strcmp(token, "krb_realm") == 0)
			{
				if (parsedline->auth_method != uaKrb5 &&
					parsedline->auth_method != uaGSS &&
					parsedline->auth_method != uaSSPI)
					INVALID_AUTH_OPTION("krb_realm", gettext_noop("krb5, gssapi and sspi"));
				parsedline->krb_realm = pstrdup(c);
			}
			else if (strcmp(token, "include_realm") == 0)
			{
				if (parsedline->auth_method != uaKrb5 &&
					parsedline->auth_method != uaGSS &&
					parsedline->auth_method != uaSSPI)
					INVALID_AUTH_OPTION("include_realm", gettext_noop("krb5, gssapi and sspi"));
				if (strcmp(c, "1") == 0)
					parsedline->include_realm = true;
				else
					parsedline->include_realm = false;
			}
			else
			{
				ereport(LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("unknown authentication option name: \"%s\"", token),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, HbaFileName)));
				return false;
			}
		}
	}

	/*
	 * Check if the selected authentication method has any mandatory arguments
	 * that are not set.
	 */
	if (parsedline->auth_method == uaLDAP)
	{
		MANDATORY_AUTH_ARG(parsedline->ldapserver, "ldapserver", "ldap");
	}

	/*
	 * Enforce any parameters implied by other settings.
	 */
	if (parsedline->auth_method == uaCert)
	{
		parsedline->clientcert = true;
	}

	return true;
}


/*
 *	Scan the (pre-parsed) hba file line by line, looking for a match
 *	to the port's connection request.
 */
static bool
check_hba(hbaPort *port)
{
	ListCell   *line;
	HbaLine    *hba;

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
#ifdef USE_SSL
			if (port->ssl)
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
#else
			/* No SSL support, so reject "hostssl" lines */
			if (hba->conntype == ctHostSSL)
				continue;
#endif

			/* Check IP address */
			if (port->raddr.addr.ss_family == hba->addr.ss_family)
			{
				if (!pg_range_sockaddr(&port->raddr.addr, &hba->addr, &hba->mask))
					continue;
			}
#ifdef HAVE_IPV6
			else if (hba->addr.ss_family == AF_INET &&
					 port->raddr.addr.ss_family == AF_INET6)
			{
				/*
				 * Wrong address family.  We allow only one case: if the file
				 * has IPv4 and the port is IPv6, promote the file address to
				 * IPv6 and try to match that way.
				 */
				struct sockaddr_storage addrcopy,
							maskcopy;

				memcpy(&addrcopy, &hba->addr, sizeof(addrcopy));
				memcpy(&maskcopy, &hba->mask, sizeof(maskcopy));
				pg_promote_v4_to_v6_addr(&addrcopy);
				pg_promote_v4_to_v6_mask(&maskcopy);

				if (!pg_range_sockaddr(&port->raddr.addr, &addrcopy, &maskcopy))
					continue;
			}
#endif   /* HAVE_IPV6 */
			else
				/* Wrong address family, no IPV6 */
				continue;
		}						/* != ctLocal */

		/* Check database and role */
		if (!check_db(port->database_name, port->user_name, hba->database))
			continue;

		if (!check_role(port->user_name, hba->role))
			continue;

		/* Found a record that matched! */
		port->hba = hba;
		return true;
	}

	/* If no matching entry was found, synthesize 'reject' entry. */
	hba = palloc0(sizeof(HbaLine));
	hba->auth_method = uaReject;
	port->hba = hba;
	return true;

	/*
	 * XXX: Return false only happens if we have a parsing error, which we can
	 * no longer have (parsing now in postmaster). Consider changing API.
	 */
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
 * Free an HbaLine structure
 */
static void
free_hba_record(HbaLine *record)
{
	if (record->database)
		pfree(record->database);
	if (record->role)
		pfree(record->role);
	if (record->usermap)
		pfree(record->usermap);
	if (record->pamservice)
		pfree(record->pamservice);
	if (record->ldapserver)
		pfree(record->ldapserver);
	if (record->ldapprefix)
		pfree(record->ldapprefix);
	if (record->ldapsuffix)
		pfree(record->ldapsuffix);
	if (record->krb_server_hostname)
		pfree(record->krb_server_hostname);
	if (record->krb_realm)
		pfree(record->krb_realm);
	pfree(record);
}

/*
 * Free all records on the parsed HBA list
 */
static void
clean_hba_list(List *lines)
{
	ListCell   *line;

	foreach(line, lines)
	{
		HbaLine    *parsed = (HbaLine *) lfirst(line);

		if (parsed)
			free_hba_record(parsed);
	}
	list_free(lines);
}

/*
 * Read the config file and create a List of HbaLine records for the contents.
 *
 * The configuration is read into a temporary list, and if any parse error occurs
 * the old list is kept in place and false is returned. Only if the whole file
 * parses Ok is the list replaced, and the function returns true.
 */
bool
load_hba(void)
{
	FILE	   *file;
	List	   *hba_lines = NIL;
	List	   *hba_line_nums = NIL;
	ListCell   *line,
			   *line_num;
	List	   *new_parsed_lines = NIL;
	bool		ok = true;

	file = AllocateFile(HbaFileName, "r");
	if (file == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open configuration file \"%s\": %m",
						HbaFileName)));

		/*
		 * Caller will take care of making this a FATAL error in case this is
		 * the initial startup. If it happens on reload, we just keep the old
		 * version around.
		 */
		return false;
	}

	tokenize_file(HbaFileName, file, &hba_lines, &hba_line_nums);
	FreeFile(file);

	/* Now parse all the lines */
	forboth(line, hba_lines, line_num, hba_line_nums)
	{
		HbaLine    *newline;

		newline = palloc0(sizeof(HbaLine));

		if (!parse_hba_line(lfirst(line), lfirst_int(line_num), newline))
		{
			/* Parse error in the file, so indicate there's a problem */
			free_hba_record(newline);
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

	/* Free the temporary lists */
	free_lines(&hba_lines, &hba_line_nums);

	if (!ok)
	{
		/* Parsing failed at one or more rows, so bail out */
		clean_hba_list(new_parsed_lines);
		return false;
	}

	/* Loaded new file successfully, replace the one we use */
	clean_hba_list(parsed_hba_lines);
	parsed_hba_lines = new_parsed_lines;

	return true;
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
	bool		initial_quote;

	if (feof(fp))
		return false;
	if (!next_token(fp, buf, sizeof(buf), &initial_quote))
		return false;
	if (strlen(buf) >= NAMEDATALEN)
		elog(FATAL, "bad data in flat pg_database file");
	strcpy(dbname, buf);
	next_token(fp, buf, sizeof(buf), &initial_quote);
	if (!isdigit((unsigned char) buf[0]))
		elog(FATAL, "bad data in flat pg_database file");
	*dboid = atooid(buf);
	next_token(fp, buf, sizeof(buf), &initial_quote);
	if (!isdigit((unsigned char) buf[0]))
		elog(FATAL, "bad data in flat pg_database file");
	*dbtablespace = atooid(buf);
	next_token(fp, buf, sizeof(buf), &initial_quote);
	if (!isdigit((unsigned char) buf[0]))
		elog(FATAL, "bad data in flat pg_database file");
	*dbfrozenxid = atoxid(buf);
	/* expect EOL next */
	if (next_token(fp, buf, sizeof(buf), &initial_quote))
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
					bool case_insensitive, bool *found_p, bool *error_p)
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

	if (strcmp(file_map, usermap_name) != 0)
		/* Line does not match the map name we're looking for, so just abort */
		return;

	/* Match? */
	if (file_ident_user[0] == '/')
	{
		/*
		 * When system username starts with a slash, treat it as a regular
		 * expression. In this case, we process the system username as a
		 * regular expression that returns exactly one match. This is replaced
		 * for \1 in the database username string, if present.
		 */
		int			r;
		regex_t		re;
		regmatch_t	matches[2];
		pg_wchar   *wstr;
		int			wlen;
		char	   *ofs;
		char	   *regexp_pgrole;

		wstr = palloc((strlen(file_ident_user + 1) + 1) * sizeof(pg_wchar));
		wlen = pg_mb2wchar_with_len(file_ident_user + 1, wstr, strlen(file_ident_user + 1));

		/*
		 * XXX: Major room for optimization: regexps could be compiled when
		 * the file is loaded and then re-used in every connection.
		 */
		r = pg_regcomp(&re, wstr, wlen, REG_ADVANCED);
		if (r)
		{
			char		errstr[100];

			pg_regerror(r, &re, errstr, sizeof(errstr));
			ereport(LOG,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("invalid regular expression \"%s\": %s", file_ident_user + 1, errstr)));

			pfree(wstr);
			*error_p = true;
			return;
		}
		pfree(wstr);

		wstr = palloc((strlen(ident_user) + 1) * sizeof(pg_wchar));
		wlen = pg_mb2wchar_with_len(ident_user, wstr, strlen(ident_user));

		r = pg_regexec(&re, wstr, wlen, 0, NULL, 2, matches, 0);
		if (r)
		{
			char		errstr[100];

			if (r != REG_NOMATCH)
			{
				/* REG_NOMATCH is not an error, everything else is */
				pg_regerror(r, &re, errstr, sizeof(errstr));
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression match for \"%s\" failed: %s", file_ident_user + 1, errstr)));
				*error_p = true;
			}

			pfree(wstr);
			pg_regfree(&re);
			return;
		}
		pfree(wstr);

		if ((ofs = strstr(file_pgrole, "\\1")) != NULL)
		{
			/* substitution of the first argument requested */
			if (matches[1].rm_so < 0)
			{
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression \"%s\" has no subexpressions as requested by backreference in \"%s\"",
								file_ident_user + 1, file_pgrole)));
				pg_regfree(&re);
				*error_p = true;
				return;
			}

			/*
			 * length: original length minus length of \1 plus length of match
			 * plus null terminator
			 */
			regexp_pgrole = palloc0(strlen(file_pgrole) - 2 + (matches[1].rm_eo - matches[1].rm_so) + 1);
			strncpy(regexp_pgrole, file_pgrole, (ofs - file_pgrole));
			memcpy(regexp_pgrole + strlen(regexp_pgrole),
				   ident_user + matches[1].rm_so,
				   matches[1].rm_eo - matches[1].rm_so);
			strcat(regexp_pgrole, ofs + 2);
		}
		else
		{
			/* no substitution, so copy the match */
			regexp_pgrole = pstrdup(file_pgrole);
		}

		pg_regfree(&re);

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
			if (pg_strcasecmp(file_pgrole, pg_role) == 0 &&
				pg_strcasecmp(file_ident_user, ident_user) == 0)
				*found_p = true;
		}
		else
		{
			if (strcmp(file_pgrole, pg_role) == 0 &&
				strcmp(file_ident_user, ident_user) == 0)
				*found_p = true;
		}
	}

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
				(errmsg("provided username (%s) and authenticated username (%s) don't match",
						pg_role, auth_user)));
		return STATUS_ERROR;
	}
	else
	{
		ListCell   *line_cell,
				   *num_cell;

		forboth(line_cell, ident_lines, num_cell, ident_line_nums)
		{
			parse_ident_usermap(lfirst(line_cell), lfirst_int(num_cell),
						  usermap_name, pg_role, auth_user, case_insensitive,
								&found_entry, &error);
			if (found_entry || error)
				break;
		}
	}
	if (!found_entry && !error)
	{
		ereport(LOG,
		(errmsg("no match in usermap for user \"%s\" authenticated as \"%s\"",
				pg_role, auth_user),
		 errcontext("usermap \"%s\"", usermap_name)));
	}
	return found_entry ? STATUS_OK : STATUS_ERROR;
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
