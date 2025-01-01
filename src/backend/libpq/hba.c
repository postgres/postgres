/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and choosing authentication method based on it).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "catalog/pg_collation.h"
#include "common/ip.h"
#include "common/string.h"
#include "libpq/hba.h"
#include "libpq/ifaddr.h"
#include "libpq/libpq-be.h"
#include "postmaster/postmaster.h"
#include "regex/regex.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/conffiles.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

#ifdef USE_LDAP
#ifdef WIN32
#include <winldap.h>
#else
#include <ldap.h>
#endif
#endif


/* callback data for check_network_callback */
typedef struct check_network_data
{
	IPCompareMethod method;		/* test method */
	SockAddr   *raddr;			/* client's actual address */
	bool		result;			/* set to true if match */
} check_network_data;

typedef struct
{
	const char *filename;
	int			linenum;
} tokenize_error_callback_arg;

#define token_has_regexp(t)	(t->regex != NULL)
#define token_is_member_check(t)	(!t->quoted && t->string[0] == '+')
#define token_is_keyword(t, k)	(!t->quoted && strcmp(t->string, k) == 0)
#define token_matches(t, k)  (strcmp(t->string, k) == 0)
#define token_matches_insensitive(t,k) (pg_strcasecmp(t->string, k) == 0)

/*
 * Memory context holding the list of TokenizedAuthLines when parsing
 * HBA or ident configuration files.  This is created when opening the first
 * file (depth of CONF_FILE_START_DEPTH).
 */
static MemoryContext tokenize_context = NULL;

/*
 * pre-parsed content of HBA config file: list of HbaLine structs.
 * parsed_hba_context is the memory context where it lives.
 */
static List *parsed_hba_lines = NIL;
static MemoryContext parsed_hba_context = NULL;

/*
 * pre-parsed content of ident mapping file: list of IdentLine structs.
 * parsed_ident_context is the memory context where it lives.
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

/*
 * Make sure UserAuthName[] tracks additions to the UserAuth enum
 */
StaticAssertDecl(lengthof(UserAuthName) == USER_AUTH_LAST + 1,
				 "UserAuthName[] must match the UserAuth enum");


static List *tokenize_expand_file(List *tokens, const char *outer_filename,
								  const char *inc_filename, int elevel,
								  int depth, char **err_msg);
static bool parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline,
							   int elevel, char **err_msg);
static int	regcomp_auth_token(AuthToken *token, char *filename, int line_num,
							   char **err_msg, int elevel);
static int	regexec_auth_token(const char *match, AuthToken *token,
							   size_t nmatch, regmatch_t pmatch[]);
static void tokenize_error_callback(void *arg);


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
 * commas, blanks, and '#', but not newlines).  As in SQL, write two
 * double-quotes to represent a double quote.
 *
 * Comments (started by an unquoted '#') are skipped, i.e. the remainder
 * of the line is ignored.
 *
 * (Note that line continuation processing happens before tokenization.
 * Thus, if a continuation occurs within quoted text or a comment, the
 * quoted text or comment is considered to continue to the next line.)
 *
 * The token, if any, is returned into buf (replacing any previous
 * contents), and *lineptr is advanced past the token.
 *
 * Also, we set *initial_quote to indicate whether there was quoting before
 * the first character.  (We use that to prevent "@x" from being treated
 * as a file inclusion request.  Note that @"x" should be so treated;
 * we want to allow that to support embedded spaces in file paths.)
 *
 * We set *terminating_comma to indicate whether the token is terminated by a
 * comma (which is not returned, nor advanced over).
 *
 * The only possible error condition is lack of terminating quote, but we
 * currently do not detect that, but just return the rest of the line.
 *
 * If successful: store dequoted token in buf and return true.
 * If no more tokens on line: set buf to empty and return false.
 */
static bool
next_token(char **lineptr, StringInfo buf,
		   bool *initial_quote, bool *terminating_comma)
{
	int			c;
	bool		in_quote = false;
	bool		was_quote = false;
	bool		saw_quote = false;

	/* Initialize output parameters */
	resetStringInfo(buf);
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

		/* we do not pass back a terminating comma in the token */
		if (c == ',' && !in_quote)
		{
			*terminating_comma = true;
			break;
		}

		if (c != '"' || was_quote)
			appendStringInfoChar(buf, c);

		/* Literal double-quote is two double-quotes */
		if (in_quote && c == '"')
			was_quote = !was_quote;
		else
			was_quote = false;

		if (c == '"')
		{
			in_quote = !in_quote;
			saw_quote = true;
			if (buf->len == 0)
				*initial_quote = true;
		}

		c = *(*lineptr)++;
	}

	/*
	 * Un-eat the char right after the token (critical in case it is '\0',
	 * else next call will read past end of string).
	 */
	(*lineptr)--;

	return (saw_quote || buf->len > 0);
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
	authtoken = (AuthToken *) palloc0(sizeof(AuthToken) + toklen + 1);
	authtoken->string = (char *) authtoken + sizeof(AuthToken);
	authtoken->quoted = quoted;
	authtoken->regex = NULL;
	memcpy(authtoken->string, token, toklen + 1);

	return authtoken;
}

/*
 * Free an AuthToken, that may include a regular expression that needs
 * to be cleaned up explicitly.
 */
static void
free_auth_token(AuthToken *token)
{
	if (token_has_regexp(token))
		pg_regfree(token->regex);
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
 * Compile the regular expression and store it in the AuthToken given in
 * input.  Returns the result of pg_regcomp().  On error, the details are
 * stored in "err_msg".
 */
static int
regcomp_auth_token(AuthToken *token, char *filename, int line_num,
				   char **err_msg, int elevel)
{
	pg_wchar   *wstr;
	int			wlen;
	int			rc;

	Assert(token->regex == NULL);

	if (token->string[0] != '/')
		return 0;				/* nothing to compile */

	token->regex = (regex_t *) palloc0(sizeof(regex_t));
	wstr = palloc((strlen(token->string + 1) + 1) * sizeof(pg_wchar));
	wlen = pg_mb2wchar_with_len(token->string + 1,
								wstr, strlen(token->string + 1));

	rc = pg_regcomp(token->regex, wstr, wlen, REG_ADVANCED, C_COLLATION_OID);

	if (rc)
	{
		char		errstr[100];

		pg_regerror(rc, token->regex, errstr, sizeof(errstr));
		ereport(elevel,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression \"%s\": %s",
						token->string + 1, errstr),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, filename)));

		*err_msg = psprintf("invalid regular expression \"%s\": %s",
							token->string + 1, errstr);
	}

	pfree(wstr);
	return rc;
}

/*
 * Execute a regular expression computed in an AuthToken, checking for a match
 * with the string specified in "match".  The caller may optionally give an
 * array to store the matches.  Returns the result of pg_regexec().
 */
static int
regexec_auth_token(const char *match, AuthToken *token, size_t nmatch,
				   regmatch_t pmatch[])
{
	pg_wchar   *wmatchstr;
	int			wmatchlen;
	int			r;

	Assert(token->string[0] == '/' && token->regex);

	wmatchstr = palloc((strlen(match) + 1) * sizeof(pg_wchar));
	wmatchlen = pg_mb2wchar_with_len(match, wmatchstr, strlen(match));

	r = pg_regexec(token->regex, wmatchstr, wmatchlen, 0, NULL, nmatch, pmatch, 0);

	pfree(wmatchstr);
	return r;
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
				  int elevel, int depth, char **err_msg)
{
	StringInfoData buf;
	bool		trailing_comma;
	bool		initial_quote;
	List	   *tokens = NIL;

	initStringInfo(&buf);

	do
	{
		if (!next_token(lineptr, &buf,
						&initial_quote, &trailing_comma))
			break;

		/* Is this referencing a file? */
		if (!initial_quote && buf.len > 1 && buf.data[0] == '@')
			tokens = tokenize_expand_file(tokens, filename, buf.data + 1,
										  elevel, depth + 1, err_msg);
		else
		{
			MemoryContext oldcxt;

			/*
			 * lappend() may do its own allocations, so move to the context
			 * for the list of tokens.
			 */
			oldcxt = MemoryContextSwitchTo(tokenize_context);
			tokens = lappend(tokens, make_auth_token(buf.data, initial_quote));
			MemoryContextSwitchTo(oldcxt);
		}
	} while (trailing_comma && (*err_msg == NULL));

	pfree(buf.data);

	return tokens;
}

/*
 * tokenize_include_file
 *		Include a file from another file into an hba "field".
 *
 * Opens and tokenises a file included from another authentication file
 * with one of the include records ("include", "include_if_exists" or
 * "include_dir"), and assign all values found to an existing list of
 * list of AuthTokens.
 *
 * All new tokens are allocated in the memory context dedicated to the
 * tokenization, aka tokenize_context.
 *
 * If missing_ok is true, ignore a missing file.
 *
 * In event of an error, log a message at ereport level elevel, and also
 * set *err_msg to a string describing the error.  Note that the result
 * may be non-NIL anyway, so *err_msg must be tested to determine whether
 * there was an error.
 */
static void
tokenize_include_file(const char *outer_filename,
					  const char *inc_filename,
					  List **tok_lines,
					  int elevel,
					  int depth,
					  bool missing_ok,
					  char **err_msg)
{
	char	   *inc_fullname;
	FILE	   *inc_file;

	inc_fullname = AbsoluteConfigLocation(inc_filename, outer_filename);
	inc_file = open_auth_file(inc_fullname, elevel, depth, err_msg);

	if (!inc_file)
	{
		if (errno == ENOENT && missing_ok)
		{
			ereport(elevel,
					(errmsg("skipping missing authentication file \"%s\"",
							inc_fullname)));
			*err_msg = NULL;
			pfree(inc_fullname);
			return;
		}

		/* error in err_msg, so leave and report */
		pfree(inc_fullname);
		Assert(err_msg);
		return;
	}

	tokenize_auth_file(inc_fullname, inc_file, tok_lines, elevel,
					   depth);
	free_auth_file(inc_file, depth);
	pfree(inc_fullname);
}

/*
 * tokenize_expand_file
 *		Expand a file included from another file into an hba "field"
 *
 * Opens and tokenises a file included from another HBA config file with @,
 * and returns all values found therein as a flat list of AuthTokens.  If a
 * @-token or include record is found, recursively expand it.  The newly
 * read tokens are appended to "tokens" (so that foo,bar,@baz does what you
 * expect).  All new tokens are allocated in the memory context dedicated
 * to the list of TokenizedAuthLines, aka tokenize_context.
 *
 * In event of an error, log a message at ereport level elevel, and also
 * set *err_msg to a string describing the error.  Note that the result
 * may be non-NIL anyway, so *err_msg must be tested to determine whether
 * there was an error.
 */
static List *
tokenize_expand_file(List *tokens,
					 const char *outer_filename,
					 const char *inc_filename,
					 int elevel,
					 int depth,
					 char **err_msg)
{
	char	   *inc_fullname;
	FILE	   *inc_file;
	List	   *inc_lines = NIL;
	ListCell   *inc_line;

	inc_fullname = AbsoluteConfigLocation(inc_filename, outer_filename);
	inc_file = open_auth_file(inc_fullname, elevel, depth, err_msg);

	if (inc_file == NULL)
	{
		/* error already logged */
		pfree(inc_fullname);
		return tokens;
	}

	/*
	 * There is possible recursion here if the file contains @ or an include
	 * record.
	 */
	tokenize_auth_file(inc_fullname, inc_file, &inc_lines, elevel,
					   depth);

	pfree(inc_fullname);

	/*
	 * Move all the tokens found in the file to the tokens list.  These are
	 * already saved in tokenize_context.
	 */
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
				MemoryContext oldcxt;

				/*
				 * lappend() may do its own allocations, so move to the
				 * context for the list of tokens.
				 */
				oldcxt = MemoryContextSwitchTo(tokenize_context);
				tokens = lappend(tokens, token);
				MemoryContextSwitchTo(oldcxt);
			}
		}
	}

	free_auth_file(inc_file, depth);
	return tokens;
}

/*
 * free_auth_file
 *		Free a file opened by open_auth_file().
 */
void
free_auth_file(FILE *file, int depth)
{
	FreeFile(file);

	/* If this is the last cleanup, remove the tokenization context */
	if (depth == CONF_FILE_START_DEPTH)
	{
		MemoryContextDelete(tokenize_context);
		tokenize_context = NULL;
	}
}

/*
 * open_auth_file
 *		Open the given file.
 *
 * filename: the absolute path to the target file
 * elevel: message logging level
 * depth: recursion level when opening the file
 * err_msg: details about the error
 *
 * Return value is the opened file.  On error, returns NULL with details
 * about the error stored in "err_msg".
 */
FILE *
open_auth_file(const char *filename, int elevel, int depth,
			   char **err_msg)
{
	FILE	   *file;

	/*
	 * Reject too-deep include nesting depth.  This is just a safety check to
	 * avoid dumping core due to stack overflow if an include file loops back
	 * to itself.  The maximum nesting depth is pretty arbitrary.
	 */
	if (depth > CONF_FILE_MAX_DEPTH)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": maximum nesting depth exceeded",
						filename)));
		if (err_msg)
			*err_msg = psprintf("could not open file \"%s\": maximum nesting depth exceeded",
								filename);
		return NULL;
	}

	file = AllocateFile(filename, "r");
	if (file == NULL)
	{
		int			save_errno = errno;

		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						filename)));
		if (err_msg)
		{
			errno = save_errno;
			*err_msg = psprintf("could not open file \"%s\": %m",
								filename);
		}
		/* the caller may care about some specific errno */
		errno = save_errno;
		return NULL;
	}

	/*
	 * When opening the top-level file, create the memory context used for the
	 * tokenization.  This will be closed with this file when coming back to
	 * this level of cleanup.
	 */
	if (depth == CONF_FILE_START_DEPTH)
	{
		/*
		 * A context may be present, but assume that it has been eliminated
		 * already.
		 */
		tokenize_context = AllocSetContextCreate(CurrentMemoryContext,
												 "tokenize_context",
												 ALLOCSET_START_SMALL_SIZES);
	}

	return file;
}

/*
 * error context callback for tokenize_auth_file()
 */
static void
tokenize_error_callback(void *arg)
{
	tokenize_error_callback_arg *callback_arg = (tokenize_error_callback_arg *) arg;

	errcontext("line %d of configuration file \"%s\"",
			   callback_arg->linenum, callback_arg->filename);
}

/*
 * tokenize_auth_file
 *		Tokenize the given file.
 *
 * The output is a list of TokenizedAuthLine structs; see the struct definition
 * in libpq/hba.h.  This is the central piece in charge of parsing the
 * authentication files.  All the operations of this function happen in its own
 * local memory context, easing the cleanup of anything allocated here.  This
 * matters a lot when reloading authentication files in the postmaster.
 *
 * filename: the absolute path to the target file
 * file: the already-opened target file
 * tok_lines: receives output list, saved into tokenize_context
 * elevel: message logging level
 * depth: level of recursion when tokenizing the target file
 *
 * Errors are reported by logging messages at ereport level elevel and by
 * adding TokenizedAuthLine structs containing non-null err_msg fields to the
 * output list.
 */
void
tokenize_auth_file(const char *filename, FILE *file, List **tok_lines,
				   int elevel, int depth)
{
	int			line_number = 1;
	StringInfoData buf;
	MemoryContext linecxt;
	MemoryContext funccxt;		/* context of this function's caller */
	ErrorContextCallback tokenerrcontext;
	tokenize_error_callback_arg callback_arg;

	Assert(tokenize_context);

	callback_arg.filename = filename;
	callback_arg.linenum = line_number;

	tokenerrcontext.callback = tokenize_error_callback;
	tokenerrcontext.arg = &callback_arg;
	tokenerrcontext.previous = error_context_stack;
	error_context_stack = &tokenerrcontext;

	/*
	 * Do all the local tokenization in its own context, to ease the cleanup
	 * of any memory allocated while tokenizing.
	 */
	linecxt = AllocSetContextCreate(CurrentMemoryContext,
									"tokenize_auth_file",
									ALLOCSET_SMALL_SIZES);
	funccxt = MemoryContextSwitchTo(linecxt);

	initStringInfo(&buf);

	if (depth == CONF_FILE_START_DEPTH)
		*tok_lines = NIL;

	while (!feof(file) && !ferror(file))
	{
		TokenizedAuthLine *tok_line;
		MemoryContext oldcxt;
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
			errno = save_errno;
			err_msg = psprintf("could not read file \"%s\": %m",
							   filename);
			break;
		}

		/* Parse fields */
		lineptr = buf.data;
		while (*lineptr && err_msg == NULL)
		{
			List	   *current_field;

			current_field = next_field_expand(filename, &lineptr,
											  elevel, depth, &err_msg);
			/* add field to line, unless we are at EOL or comment start */
			if (current_field != NIL)
			{
				/*
				 * lappend() may do its own allocations, so move to the
				 * context for the list of tokens.
				 */
				oldcxt = MemoryContextSwitchTo(tokenize_context);
				current_line = lappend(current_line, current_field);
				MemoryContextSwitchTo(oldcxt);
			}
		}

		/*
		 * Reached EOL; no need to emit line to TokenizedAuthLine list if it's
		 * boring.
		 */
		if (current_line == NIL && err_msg == NULL)
			goto next_line;

		/* If the line is valid, check if that's an include directive */
		if (err_msg == NULL && list_length(current_line) == 2)
		{
			AuthToken  *first,
					   *second;

			first = linitial(linitial_node(List, current_line));
			second = linitial(lsecond_node(List, current_line));

			if (strcmp(first->string, "include") == 0)
			{
				tokenize_include_file(filename, second->string, tok_lines,
									  elevel, depth + 1, false, &err_msg);

				if (err_msg)
					goto process_line;

				/*
				 * tokenize_auth_file() has taken care of creating the
				 * TokenizedAuthLines.
				 */
				goto next_line;
			}
			else if (strcmp(first->string, "include_dir") == 0)
			{
				char	  **filenames;
				char	   *dir_name = second->string;
				int			num_filenames;
				StringInfoData err_buf;

				filenames = GetConfFilesInDir(dir_name, filename, elevel,
											  &num_filenames, &err_msg);

				if (!filenames)
				{
					/* the error is in err_msg, so create an entry */
					goto process_line;
				}

				initStringInfo(&err_buf);
				for (int i = 0; i < num_filenames; i++)
				{
					tokenize_include_file(filename, filenames[i], tok_lines,
										  elevel, depth + 1, false, &err_msg);
					/* cumulate errors if any */
					if (err_msg)
					{
						if (err_buf.len > 0)
							appendStringInfoChar(&err_buf, '\n');
						appendStringInfoString(&err_buf, err_msg);
					}
				}

				/* clean up things */
				for (int i = 0; i < num_filenames; i++)
					pfree(filenames[i]);
				pfree(filenames);

				/*
				 * If there were no errors, the line is fully processed,
				 * bypass the general TokenizedAuthLine processing.
				 */
				if (err_buf.len == 0)
					goto next_line;

				/* Otherwise, process the cumulated errors, if any. */
				err_msg = err_buf.data;
				goto process_line;
			}
			else if (strcmp(first->string, "include_if_exists") == 0)
			{

				tokenize_include_file(filename, second->string, tok_lines,
									  elevel, depth + 1, true, &err_msg);
				if (err_msg)
					goto process_line;

				/*
				 * tokenize_auth_file() has taken care of creating the
				 * TokenizedAuthLines.
				 */
				goto next_line;
			}
		}

process_line:

		/*
		 * General processing: report the error if any and emit line to the
		 * TokenizedAuthLine.  This is saved in the memory context dedicated
		 * to this list.
		 */
		oldcxt = MemoryContextSwitchTo(tokenize_context);
		tok_line = (TokenizedAuthLine *) palloc0(sizeof(TokenizedAuthLine));
		tok_line->fields = current_line;
		tok_line->file_name = pstrdup(filename);
		tok_line->line_num = line_number;
		tok_line->raw_line = pstrdup(buf.data);
		tok_line->err_msg = err_msg ? pstrdup(err_msg) : NULL;
		*tok_lines = lappend(*tok_lines, tok_line);
		MemoryContextSwitchTo(oldcxt);

next_line:
		line_number += continuations + 1;
		callback_arg.linenum = line_number;
	}

	MemoryContextSwitchTo(funccxt);
	MemoryContextDelete(linecxt);

	error_context_stack = tokenerrcontext.previous;
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
 *
 * Each AuthToken listed is checked one-by-one.  Keywords are processed
 * first (these cannot have regular expressions), followed by regular
 * expressions (if any), the case-insensitive match (if requested) and
 * the exact match.
 */
static bool
check_role(const char *role, Oid roleid, List *tokens, bool case_insensitive)
{
	ListCell   *cell;
	AuthToken  *tok;

	foreach(cell, tokens)
	{
		tok = lfirst(cell);
		if (token_is_member_check(tok))
		{
			if (is_member(roleid, tok->string + 1))
				return true;
		}
		else if (token_is_keyword(tok, "all"))
			return true;
		else if (token_has_regexp(tok))
		{
			if (regexec_auth_token(role, tok, 0, NULL) == REG_OKAY)
				return true;
		}
		else if (case_insensitive)
		{
			if (token_matches_insensitive(tok, role))
				return true;
		}
		else if (token_matches(tok, role))
			return true;
	}
	return false;
}

/*
 * Check to see if db/role combination matches AuthToken list.
 *
 * Each AuthToken listed is checked one-by-one.  Keywords are checked
 * first (these cannot have regular expressions), followed by regular
 * expressions (if any) and the exact match.
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
		else if (token_has_regexp(tok))
		{
			if (regexec_auth_token(dbname, tok, 0, NULL) == REG_OKAY)
				return true;
		}
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

static bool
ipv6eq(struct sockaddr_in6 *a, struct sockaddr_in6 *b)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (a->sin6_addr.s6_addr[i] != b->sin6_addr.s6_addr[i])
			return false;

	return true;
}

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
			else if (gai->ai_addr->sa_family == AF_INET6)
			{
				if (ipv6eq((struct sockaddr_in6 *) gai->ai_addr,
						   (struct sockaddr_in6 *) &port->raddr.addr))
				{
					found = true;
					break;
				}
			}
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
					line_num, file_name))); \
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
						line_num, file_name))); \
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
				 errmsg("missing entry at end of line"), \
				 errcontext("line %d of configuration file \"%s\"", \
							line_num, file_name))); \
		*err_msg = pstrdup("missing entry at end of line"); \
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
							line_num, file_name))); \
		*err_msg = pstrdup("multiple values in ident field"); \
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
	char	   *file_name = tok_line->file_name;
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
	parsedline->sourcefile = pstrdup(file_name);
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
							line_num, file_name)));
		*err_msg = "multiple values specified for connection type";
		return NULL;
	}
	token = linitial(tokens);
	if (strcmp(token->string, "local") == 0)
	{
		parsedline->conntype = ctLocal;
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
						 errhint("Set \"ssl = on\" in postgresql.conf."),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				*err_msg = "hostssl record cannot match because SSL is disabled";
			}
#else
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("hostssl record cannot match because SSL is not supported by this build"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
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
								line_num, file_name)));
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
							line_num, file_name)));
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
							line_num, file_name)));
		*err_msg = "end-of-line before database specification";
		return NULL;
	}
	parsedline->databases = NIL;
	tokens = lfirst(field);
	foreach(tokencell, tokens)
	{
		AuthToken  *tok = copy_auth_token(lfirst(tokencell));

		/* Compile a regexp for the database token, if necessary */
		if (regcomp_auth_token(tok, file_name, line_num, err_msg, elevel))
			return NULL;

		parsedline->databases = lappend(parsedline->databases, tok);
	}

	/* Get the roles. */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before role specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "end-of-line before role specification";
		return NULL;
	}
	parsedline->roles = NIL;
	tokens = lfirst(field);
	foreach(tokencell, tokens)
	{
		AuthToken  *tok = copy_auth_token(lfirst(tokencell));

		/* Compile a regexp from the role token, if necessary */
		if (regcomp_auth_token(tok, file_name, line_num, err_msg, elevel))
			return NULL;

		parsedline->roles = lappend(parsedline->roles, tok);
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
								line_num, file_name)));
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
								line_num, file_name)));
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
									line_num, file_name)));
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
										line_num, file_name)));
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
										line_num, file_name)));
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
										line_num, file_name)));
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
										line_num, file_name)));
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
										line_num, file_name)));
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
										line_num, file_name)));
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
							line_num, file_name)));
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
							line_num, file_name)));
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
		parsedline->auth_method = uaMD5;
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
							line_num, file_name)));
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
							line_num, file_name)));
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
							line_num, file_name)));
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
							line_num, file_name)));
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
							line_num, file_name)));
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
									line_num, file_name)));
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
						 errmsg("cannot mix options for simple bind and search+bind modes"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				*err_msg = "cannot mix options for simple bind and search+bind modes";
				return NULL;
			}
		}
		else if (!parsedline->ldapbasedn)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication method \"ldap\" requires argument \"ldapbasedn\", \"ldapprefix\", or \"ldapsuffix\" to be set"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
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
								line_num, file_name)));
			*err_msg = "cannot use ldapsearchattribute together with ldapsearchfilter";
			return NULL;
		}
	}

	if (parsedline->auth_method == uaRADIUS)
	{
		MANDATORY_AUTH_ARG(parsedline->radiusservers, "radiusservers", "radius");
		MANDATORY_AUTH_ARG(parsedline->radiussecrets, "radiussecrets", "radius");

		if (parsedline->radiusservers == NIL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS servers cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "list of RADIUS servers cannot be empty";
			return NULL;
		}

		if (parsedline->radiussecrets == NIL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS secrets cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
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
								line_num, file_name)));
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
								line_num, file_name)));
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
								line_num, file_name)));
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
	char	   *file_name = hbaline->sourcefile;

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
								line_num, file_name)));
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
									line_num, file_name)));
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
								line_num, file_name)));
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
								line_num, file_name)));
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
								line_num, file_name)));
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
								line_num, file_name)));
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
								line_num, file_name)));
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
								line_num, file_name)));
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
									line_num, file_name)));
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
								line_num, file_name)));
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
									line_num, file_name)));

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
								line_num, file_name)));
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
								line_num, file_name)));
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
							line_num, file_name)));
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

		if (!check_role(port->user_name, roleid, hba->roles, false))
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
	MemoryContext oldcxt;
	MemoryContext hbacxt;

	file = open_auth_file(HbaFileName, LOG, 0, NULL);
	if (file == NULL)
	{
		/* error already logged */
		return false;
	}

	tokenize_auth_file(HbaFileName, file, &hba_lines, LOG, 0);

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
	free_auth_file(file, 0);
	MemoryContextSwitchTo(oldcxt);

	if (!ok)
	{
		/*
		 * File contained one or more errors, so bail out. MemoryContextDelete
		 * is enough to clean up everything, including regexes.
		 */
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
	char	   *file_name = tok_line->file_name;
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

	/* Copy the ident user token */
	parsedline->system_user = copy_auth_token(token);

	/* Get the PG rolename token */
	field = lnext(tok_line->fields, field);
	IDENT_FIELD_ABSENT(field);
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->pg_user = copy_auth_token(token);

	/*
	 * Now that the field validation is done, compile a regex from the user
	 * tokens, if necessary.
	 */
	if (regcomp_auth_token(parsedline->system_user, file_name, line_num,
						   err_msg, elevel))
	{
		/* err_msg includes the error to report */
		return NULL;
	}

	if (regcomp_auth_token(parsedline->pg_user, file_name, line_num,
						   err_msg, elevel))
	{
		/* err_msg includes the error to report */
		return NULL;
	}

	return parsedline;
}

/*
 *	Process one line from the parsed ident config lines.
 *
 *	Compare input parsed ident line to the needed map, pg_user and system_user.
 *	*found_p and *error_p are set according to our results.
 */
static void
check_ident_usermap(IdentLine *identLine, const char *usermap_name,
					const char *pg_user, const char *system_user,
					bool case_insensitive, bool *found_p, bool *error_p)
{
	Oid			roleid;

	*found_p = false;
	*error_p = false;

	if (strcmp(identLine->usermap, usermap_name) != 0)
		/* Line does not match the map name we're looking for, so just abort */
		return;

	/* Get the target role's OID.  Note we do not error out for bad role. */
	roleid = get_role_oid(pg_user, true);

	/* Match? */
	if (token_has_regexp(identLine->system_user))
	{
		/*
		 * Process the system username as a regular expression that returns
		 * exactly one match. This is replaced for \1 in the database username
		 * string, if present.
		 */
		int			r;
		regmatch_t	matches[2];
		char	   *ofs;
		AuthToken  *expanded_pg_user_token;
		bool		created_temporary_token = false;

		r = regexec_auth_token(system_user, identLine->system_user, 2, matches);
		if (r)
		{
			char		errstr[100];

			if (r != REG_NOMATCH)
			{
				/* REG_NOMATCH is not an error, everything else is */
				pg_regerror(r, identLine->system_user->regex, errstr, sizeof(errstr));
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression match for \"%s\" failed: %s",
								identLine->system_user->string + 1, errstr)));
				*error_p = true;
			}
			return;
		}

		/*
		 * Replace \1 with the first captured group unless the field already
		 * has some special meaning, like a group membership or a regexp-based
		 * check.
		 */
		if (!token_is_member_check(identLine->pg_user) &&
			!token_has_regexp(identLine->pg_user) &&
			(ofs = strstr(identLine->pg_user->string, "\\1")) != NULL)
		{
			char	   *expanded_pg_user;
			int			offset;

			/* substitution of the first argument requested */
			if (matches[1].rm_so < 0)
			{
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression \"%s\" has no subexpressions as requested by backreference in \"%s\"",
								identLine->system_user->string + 1, identLine->pg_user->string)));
				*error_p = true;
				return;
			}

			/*
			 * length: original length minus length of \1 plus length of match
			 * plus null terminator
			 */
			expanded_pg_user = palloc0(strlen(identLine->pg_user->string) - 2 + (matches[1].rm_eo - matches[1].rm_so) + 1);
			offset = ofs - identLine->pg_user->string;
			memcpy(expanded_pg_user, identLine->pg_user->string, offset);
			memcpy(expanded_pg_user + offset,
				   system_user + matches[1].rm_so,
				   matches[1].rm_eo - matches[1].rm_so);
			strcat(expanded_pg_user, ofs + 2);

			/*
			 * Mark the token as quoted, so it will only be compared literally
			 * and not for some special meaning, such as "all" or a group
			 * membership check.
			 */
			expanded_pg_user_token = make_auth_token(expanded_pg_user, true);
			created_temporary_token = true;
			pfree(expanded_pg_user);
		}
		else
		{
			expanded_pg_user_token = identLine->pg_user;
		}

		/* check the Postgres user */
		*found_p = check_role(pg_user, roleid,
							  list_make1(expanded_pg_user_token),
							  case_insensitive);

		if (created_temporary_token)
			free_auth_token(expanded_pg_user_token);

		return;
	}
	else
	{
		/*
		 * Not a regular expression, so make a complete match.  If the system
		 * user does not match, just leave.
		 */
		if (case_insensitive)
		{
			if (!token_matches_insensitive(identLine->system_user,
										   system_user))
				return;
		}
		else
		{
			if (!token_matches(identLine->system_user, system_user))
				return;
		}

		/* check the Postgres user */
		*found_p = check_role(pg_user, roleid,
							  list_make1(identLine->pg_user),
							  case_insensitive);
	}
}


/*
 *	Scan the (pre-parsed) ident usermap file line by line, looking for a match
 *
 *	See if the system user with ident username "system_user" is allowed to act as
 *	Postgres user "pg_user" according to usermap "usermap_name".
 *
 *	Special case: Usermap NULL, equivalent to what was previously called
 *	"sameuser" or "samerole", means don't look in the usermap file.
 *	That's an implied map wherein "pg_user" must be identical to
 *	"system_user" in order to be authorized.
 *
 *	Iff authorized, return STATUS_OK, otherwise return STATUS_ERROR.
 */
int
check_usermap(const char *usermap_name,
			  const char *pg_user,
			  const char *system_user,
			  bool case_insensitive)
{
	bool		found_entry = false,
				error = false;

	if (usermap_name == NULL || usermap_name[0] == '\0')
	{
		if (case_insensitive)
		{
			if (pg_strcasecmp(pg_user, system_user) == 0)
				return STATUS_OK;
		}
		else
		{
			if (strcmp(pg_user, system_user) == 0)
				return STATUS_OK;
		}
		ereport(LOG,
				(errmsg("provided user name (%s) and authenticated user name (%s) do not match",
						pg_user, system_user)));
		return STATUS_ERROR;
	}
	else
	{
		ListCell   *line_cell;

		foreach(line_cell, parsed_ident_lines)
		{
			check_ident_usermap(lfirst(line_cell), usermap_name,
								pg_user, system_user, case_insensitive,
								&found_entry, &error);
			if (found_entry || error)
				break;
		}
	}
	if (!found_entry && !error)
	{
		ereport(LOG,
				(errmsg("no match in usermap \"%s\" for user \"%s\" authenticated as \"%s\"",
						usermap_name, pg_user, system_user)));
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
	ListCell   *line_cell;
	List	   *new_parsed_lines = NIL;
	bool		ok = true;
	MemoryContext oldcxt;
	MemoryContext ident_context;
	IdentLine  *newline;

	/* not FATAL ... we just won't do any special ident maps */
	file = open_auth_file(IdentFileName, LOG, 0, NULL);
	if (file == NULL)
	{
		/* error already logged */
		return false;
	}

	tokenize_auth_file(IdentFileName, file, &ident_lines, LOG, 0);

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
	free_auth_file(file, 0);
	MemoryContextSwitchTo(oldcxt);

	if (!ok)
	{
		/*
		 * File contained one or more errors, so bail out. MemoryContextDelete
		 * is enough to clean up everything, including regexes.
		 */
		MemoryContextDelete(ident_context);
		return false;
	}

	/* Loaded new file successfully, replace the one we use */
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
	return UserAuthName[auth_method];
}
