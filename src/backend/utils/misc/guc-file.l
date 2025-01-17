%top{
/*
 * Scanner for the configuration file
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/backend/utils/misc/guc-file.l
 */

#include "postgres.h"

#include <ctype.h>
#include <unistd.h>

#include "common/file_utils.h"
#include "guc_internal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/conffiles.h"
#include "utils/memutils.h"
}


%{
/*
 * flex emits a yy_fatal_error() function that it calls in response to
 * critical errors like malloc failure, file I/O errors, and detection of
 * internal inconsistency.  That function prints a message and calls exit().
 * Mutate it to instead call our handler, which jumps out of the parser.
 */
#undef fprintf
#define fprintf(file, fmt, msg) GUC_flex_fatal(msg)

enum
{
	GUC_ID = 1,
	GUC_STRING = 2,
	GUC_INTEGER = 3,
	GUC_REAL = 4,
	GUC_EQUALS = 5,
	GUC_UNQUOTED_STRING = 6,
	GUC_QUALIFIED_ID = 7,
	GUC_EOL = 99,
	GUC_ERROR = 100
};

static unsigned int ConfigFileLineno;
static const char *GUC_flex_fatal_errmsg;
static sigjmp_buf *GUC_flex_fatal_jmp;

static void FreeConfigVariable(ConfigVariable *item);

static int	GUC_flex_fatal(const char *msg);

/* LCOV_EXCL_START */

%}

%option reentrant
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="GUC_yy"


SIGN			("-"|"+")
DIGIT			[0-9]
HEXDIGIT		[0-9a-fA-F]

UNIT_LETTER		[a-zA-Z]

INTEGER			{SIGN}?({DIGIT}+|0x{HEXDIGIT}+){UNIT_LETTER}*

EXPONENT		[Ee]{SIGN}?{DIGIT}+
REAL			{SIGN}?{DIGIT}*"."{DIGIT}*{EXPONENT}?

LETTER			[A-Za-z_\200-\377]
LETTER_OR_DIGIT [A-Za-z_0-9\200-\377]

ID				{LETTER}{LETTER_OR_DIGIT}*
QUALIFIED_ID	{ID}"."{ID}

UNQUOTED_STRING {LETTER}({LETTER_OR_DIGIT}|[-._:/])*
STRING			\'([^'\\\n]|\\.|\'\')*\'

%%

\n				ConfigFileLineno++; return GUC_EOL;
[ \t\r]+		/* eat whitespace */
#.*				/* eat comment (.* matches anything until newline) */

{ID}			return GUC_ID;
{QUALIFIED_ID}	return GUC_QUALIFIED_ID;
{STRING}		return GUC_STRING;
{UNQUOTED_STRING} return GUC_UNQUOTED_STRING;
{INTEGER}		return GUC_INTEGER;
{REAL}			return GUC_REAL;
=				return GUC_EQUALS;

.				return GUC_ERROR;

%%

/* LCOV_EXCL_STOP */

/*
 * Exported function to read and process the configuration file. The
 * parameter indicates in what context the file is being read --- either
 * postmaster startup (including standalone-backend startup) or SIGHUP.
 * All options mentioned in the configuration file are set to new values.
 * If a hard error occurs, no values will be changed.  (There can also be
 * errors that prevent just one value from being changed.)
 */
void
ProcessConfigFile(GucContext context)
{
	int			elevel;
	MemoryContext config_cxt;
	MemoryContext caller_cxt;

	/*
	 * Config files are processed on startup (by the postmaster only) and on
	 * SIGHUP (by the postmaster and its children)
	 */
	Assert((context == PGC_POSTMASTER && !IsUnderPostmaster) ||
		   context == PGC_SIGHUP);

	/*
	 * To avoid cluttering the log, only the postmaster bleats loudly about
	 * problems with the config file.
	 */
	elevel = IsUnderPostmaster ? DEBUG2 : LOG;

	/*
	 * This function is usually called within a process-lifespan memory
	 * context.  To ensure that any memory leaked during GUC processing does
	 * not accumulate across repeated SIGHUP cycles, do the work in a private
	 * context that we can free at exit.
	 */
	config_cxt = AllocSetContextCreate(CurrentMemoryContext,
									   "config file processing",
									   ALLOCSET_DEFAULT_SIZES);
	caller_cxt = MemoryContextSwitchTo(config_cxt);

	/*
	 * Read and apply the config file.  We don't need to examine the result.
	 */
	(void) ProcessConfigFileInternal(context, true, elevel);

	/* Clean up */
	MemoryContextSwitchTo(caller_cxt);
	MemoryContextDelete(config_cxt);
}

/*
 * Read and parse a single configuration file.  This function recurses
 * to handle "include" directives.
 *
 * If "strict" is true, treat failure to open the config file as an error,
 * otherwise just skip the file.
 *
 * calling_file/calling_lineno identify the source of the request.
 * Pass NULL/0 if not recursing from an inclusion request.
 *
 * See ParseConfigFp for further details.  This one merely adds opening the
 * config file rather than working from a caller-supplied file descriptor,
 * and absolute-ifying the path name if necessary.
 */
bool
ParseConfigFile(const char *config_file, bool strict,
				const char *calling_file, int calling_lineno,
				int depth, int elevel,
				ConfigVariable **head_p,
				ConfigVariable **tail_p)
{
	char	   *abs_path;
	bool		OK = true;
	FILE	   *fp;

	/*
	 * Reject file name that is all-blank (including empty), as that leads to
	 * confusion --- we'd try to read the containing directory as a file.
	 */
	if (strspn(config_file, " \t\r\n") == strlen(config_file))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty configuration file name: \"%s\"",
						config_file)));
		record_config_file_error("empty configuration file name",
								 calling_file, calling_lineno,
								 head_p, tail_p);
		return false;
	}

	/*
	 * Reject too-deep include nesting depth.  This is just a safety check to
	 * avoid dumping core due to stack overflow if an include file loops back
	 * to itself.  The maximum nesting depth is pretty arbitrary.
	 */
	if (depth > CONF_FILE_MAX_DEPTH)
	{
		ereport(elevel,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("could not open configuration file \"%s\": maximum nesting depth exceeded",
						config_file)));
		record_config_file_error("nesting depth exceeded",
								 calling_file, calling_lineno,
								 head_p, tail_p);
		return false;
	}

	abs_path = AbsoluteConfigLocation(config_file, calling_file);

	/*
	 * Reject direct recursion.  Indirect recursion is also possible, but it's
	 * harder to detect and so doesn't seem worth the trouble.  (We test at
	 * this step because the canonicalization done by AbsoluteConfigLocation
	 * makes it more likely that a simple strcmp comparison will match.)
	 */
	if (calling_file && strcmp(abs_path, calling_file) == 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("configuration file recursion in \"%s\"",
						calling_file)));
		record_config_file_error("configuration file recursion",
								 calling_file, calling_lineno,
								 head_p, tail_p);
		pfree(abs_path);
		return false;
	}

	fp = AllocateFile(abs_path, "r");
	if (!fp)
	{
		if (strict)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open configuration file \"%s\": %m",
							abs_path)));
			record_config_file_error(psprintf("could not open file \"%s\"",
											  abs_path),
									 calling_file, calling_lineno,
									 head_p, tail_p);
			OK = false;
		}
		else
		{
			ereport(LOG,
					(errmsg("skipping missing configuration file \"%s\"",
							abs_path)));
		}
		goto cleanup;
	}

	OK = ParseConfigFp(fp, abs_path, depth, elevel, head_p, tail_p);

cleanup:
	if (fp)
		FreeFile(fp);
	pfree(abs_path);

	return OK;
}

/*
 * Capture an error message in the ConfigVariable list returned by
 * config file parsing.
 */
void
record_config_file_error(const char *errmsg,
						 const char *config_file,
						 int lineno,
						 ConfigVariable **head_p,
						 ConfigVariable **tail_p)
{
	ConfigVariable *item;

	item = palloc(sizeof *item);
	item->name = NULL;
	item->value = NULL;
	item->errmsg = pstrdup(errmsg);
	item->filename = config_file ? pstrdup(config_file) : NULL;
	item->sourceline = lineno;
	item->ignore = true;
	item->applied = false;
	item->next = NULL;
	if (*head_p == NULL)
		*head_p = item;
	else
		(*tail_p)->next = item;
	*tail_p = item;
}

/*
 * Flex fatal errors bring us here.  Stash the error message and jump back to
 * ParseConfigFp().  Assume all msg arguments point to string constants; this
 * holds for all currently known flex versions. Otherwise, we would need to
 * copy the message.
 *
 * We return "int" since this takes the place of calls to fprintf().
*/
static int
GUC_flex_fatal(const char *msg)
{
	GUC_flex_fatal_errmsg = msg;
	siglongjmp(*GUC_flex_fatal_jmp, 1);
	return 0;					/* keep compiler quiet */
}

/*
 * Read and parse a single configuration file.  This function recurses
 * to handle "include" directives.
 *
 * Input parameters:
 *	fp: file pointer from AllocateFile for the configuration file to parse
 *	config_file: absolute or relative path name of the configuration file
 *	depth: recursion depth (should be CONF_FILE_START_DEPTH in the outermost
 *	call)
 *	elevel: error logging level to use
 * Input/Output parameters:
 *	head_p, tail_p: head and tail of linked list of name/value pairs
 *
 * *head_p and *tail_p must be initialized, either to NULL or valid pointers
 * to a ConfigVariable list, before calling the outer recursion level.  Any
 * name-value pairs read from the input file(s) will be appended to the list.
 * Error reports will also be appended to the list, if elevel < ERROR.
 *
 * Returns TRUE if successful, FALSE if an error occurred.  The error has
 * already been ereport'd, it is only necessary for the caller to clean up
 * its own state and release the ConfigVariable list.
 *
 * Note: if elevel >= ERROR then an error will not return control to the
 * caller, so there is no need to check the return value in that case.
 *
 * Note: this function is used to parse not only postgresql.conf, but
 * various other configuration files that use the same "name = value"
 * syntax.  Hence, do not do anything here or in the subsidiary routines
 * ParseConfigFile/ParseConfigDirectory that assumes we are processing
 * GUCs specifically.
 */
bool
ParseConfigFp(FILE *fp, const char *config_file, int depth, int elevel,
			  ConfigVariable **head_p, ConfigVariable **tail_p)
{
	volatile bool OK = true;
	unsigned int save_ConfigFileLineno = ConfigFileLineno;
	sigjmp_buf *save_GUC_flex_fatal_jmp = GUC_flex_fatal_jmp;
	sigjmp_buf	flex_fatal_jmp;
	yyscan_t	scanner;
	struct yyguts_t *yyg;		/* needed for yytext macro */
	volatile YY_BUFFER_STATE lex_buffer = NULL;
	int			errorcount;
	int			token;

	if (sigsetjmp(flex_fatal_jmp, 1) == 0)
		GUC_flex_fatal_jmp = &flex_fatal_jmp;
	else
	{
		/*
		 * Regain control after a fatal, internal flex error.  It may have
		 * corrupted parser state.  Consequently, abandon the file, but trust
		 * that the state remains sane enough for yy_delete_buffer().
		 */
		elog(elevel, "%s at file \"%s\" line %u",
			 GUC_flex_fatal_errmsg, config_file, ConfigFileLineno);
		record_config_file_error(GUC_flex_fatal_errmsg,
								 config_file, ConfigFileLineno,
								 head_p, tail_p);
		OK = false;
		goto cleanup;
	}

	/*
	 * Parse
	 */
	ConfigFileLineno = 1;
	errorcount = 0;

	if (yylex_init(&scanner) != 0)
		elog(elevel, "yylex_init() failed: %m");
	yyg = (struct yyguts_t *) scanner;

	lex_buffer = yy_create_buffer(fp, YY_BUF_SIZE, scanner);
	yy_switch_to_buffer(lex_buffer, scanner);

	/* This loop iterates once per logical line */
	while ((token = yylex(scanner)))
	{
		char	   *opt_name = NULL;
		char	   *opt_value = NULL;
		ConfigVariable *item;

		if (token == GUC_EOL)	/* empty or comment line */
			continue;

		/* first token on line is option name */
		if (token != GUC_ID && token != GUC_QUALIFIED_ID)
			goto parse_error;
		opt_name = pstrdup(yytext);

		/* next we have an optional equal sign; discard if present */
		token = yylex(scanner);
		if (token == GUC_EQUALS)
			token = yylex(scanner);

		/* now we must have the option value */
		if (token != GUC_ID &&
			token != GUC_STRING &&
			token != GUC_INTEGER &&
			token != GUC_REAL &&
			token != GUC_UNQUOTED_STRING)
			goto parse_error;
		if (token == GUC_STRING)	/* strip quotes and escapes */
			opt_value = DeescapeQuotedString(yytext);
		else
			opt_value = pstrdup(yytext);

		/* now we'd like an end of line, or possibly EOF */
		token = yylex(scanner);
		if (token != GUC_EOL)
		{
			if (token != 0)
				goto parse_error;
			/* treat EOF like \n for line numbering purposes, cf bug 4752 */
			ConfigFileLineno++;
		}

		/* OK, process the option name and value */
		if (guc_name_compare(opt_name, "include_dir") == 0)
		{
			/*
			 * An include_dir directive isn't a variable and should be
			 * processed immediately.
			 */
			if (!ParseConfigDirectory(opt_value,
									  config_file, ConfigFileLineno - 1,
									  depth + 1, elevel,
									  head_p, tail_p))
				OK = false;
			yy_switch_to_buffer(lex_buffer, scanner);
			pfree(opt_name);
			pfree(opt_value);
		}
		else if (guc_name_compare(opt_name, "include_if_exists") == 0)
		{
			/*
			 * An include_if_exists directive isn't a variable and should be
			 * processed immediately.
			 */
			if (!ParseConfigFile(opt_value, false,
								 config_file, ConfigFileLineno - 1,
								 depth + 1, elevel,
								 head_p, tail_p))
				OK = false;
			yy_switch_to_buffer(lex_buffer, scanner);
			pfree(opt_name);
			pfree(opt_value);
		}
		else if (guc_name_compare(opt_name, "include") == 0)
		{
			/*
			 * An include directive isn't a variable and should be processed
			 * immediately.
			 */
			if (!ParseConfigFile(opt_value, true,
								 config_file, ConfigFileLineno - 1,
								 depth + 1, elevel,
								 head_p, tail_p))
				OK = false;
			yy_switch_to_buffer(lex_buffer, scanner);
			pfree(opt_name);
			pfree(opt_value);
		}
		else
		{
			/* ordinary variable, append to list */
			item = palloc(sizeof *item);
			item->name = opt_name;
			item->value = opt_value;
			item->errmsg = NULL;
			item->filename = pstrdup(config_file);
			item->sourceline = ConfigFileLineno - 1;
			item->ignore = false;
			item->applied = false;
			item->next = NULL;
			if (*head_p == NULL)
				*head_p = item;
			else
				(*tail_p)->next = item;
			*tail_p = item;
		}

		/* break out of loop if read EOF, else loop for next line */
		if (token == 0)
			break;
		continue;

parse_error:
		/* release storage if we allocated any on this line */
		if (opt_name)
			pfree(opt_name);
		if (opt_value)
			pfree(opt_value);

		/* report the error */
		if (token == GUC_EOL || token == 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("syntax error in file \"%s\" line %u, near end of line",
							config_file, ConfigFileLineno - 1)));
			record_config_file_error("syntax error",
									 config_file, ConfigFileLineno - 1,
									 head_p, tail_p);
		}
		else
		{
			ereport(elevel,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("syntax error in file \"%s\" line %u, near token \"%s\"",
							config_file, ConfigFileLineno, yytext)));
			record_config_file_error("syntax error",
									 config_file, ConfigFileLineno,
									 head_p, tail_p);
		}
		OK = false;
		errorcount++;

		/*
		 * To avoid producing too much noise when fed a totally bogus file,
		 * give up after 100 syntax errors per file (an arbitrary number).
		 * Also, if we're only logging the errors at DEBUG level anyway, might
		 * as well give up immediately.  (This prevents postmaster children
		 * from bloating the logs with duplicate complaints.)
		 */
		if (errorcount >= 100 || elevel <= DEBUG1)
		{
			ereport(elevel,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many syntax errors found, abandoning file \"%s\"",
							config_file)));
			break;
		}

		/* resync to next end-of-line or EOF */
		while (token != GUC_EOL && token != 0)
			token = yylex(scanner);
		/* break out of loop on EOF */
		if (token == 0)
			break;
	}

cleanup:
	yy_delete_buffer(lex_buffer, scanner);
	yylex_destroy(scanner);
	/* Each recursion level must save and restore these static variables. */
	ConfigFileLineno = save_ConfigFileLineno;
	GUC_flex_fatal_jmp = save_GUC_flex_fatal_jmp;
	return OK;
}

/*
 * Read and parse all config files in a subdirectory in alphabetical order
 *
 * includedir is the absolute or relative path to the subdirectory to scan.
 *
 * calling_file/calling_lineno identify the source of the request.
 * Pass NULL/0 if not recursing from an inclusion request.
 *
 * See ParseConfigFp for further details.
 */
bool
ParseConfigDirectory(const char *includedir,
					 const char *calling_file, int calling_lineno,
					 int depth, int elevel,
					 ConfigVariable **head_p,
					 ConfigVariable **tail_p)
{
	char	   *err_msg;
	char	  **filenames;
	int			num_filenames;

	filenames = GetConfFilesInDir(includedir, calling_file, elevel,
								  &num_filenames, &err_msg);

	if (!filenames)
	{
		record_config_file_error(err_msg, calling_file, calling_lineno, head_p,
								 tail_p);
		return false;
	}

	for (int i = 0; i < num_filenames; i++)
	{
		if (!ParseConfigFile(filenames[i], true,
							 calling_file, calling_lineno,
							 depth, elevel,
							 head_p, tail_p))
			return false;
	}

	return true;
}

/*
 * Free a list of ConfigVariables, including the names and the values
 */
void
FreeConfigVariables(ConfigVariable *list)
{
	ConfigVariable *item;

	item = list;
	while (item)
	{
		ConfigVariable *next = item->next;

		FreeConfigVariable(item);
		item = next;
	}
}

/*
 * Free a single ConfigVariable
 */
static void
FreeConfigVariable(ConfigVariable *item)
{
	if (item->name)
		pfree(item->name);
	if (item->value)
		pfree(item->value);
	if (item->errmsg)
		pfree(item->errmsg);
	if (item->filename)
		pfree(item->filename);
	pfree(item);
}


/*
 *		DeescapeQuotedString
 *
 * Strip the quotes surrounding the given string, and collapse any embedded
 * '' sequences and backslash escapes.
 *
 * The string returned is palloc'd and should eventually be pfree'd by the
 * caller.
 *
 * This is exported because it is also used by the bootstrap scanner.
 */
char *
DeescapeQuotedString(const char *s)
{
	char	   *newStr;
	int			len,
				i,
				j;

	/* We just Assert that there are leading and trailing quotes */
	Assert(s != NULL && s[0] == '\'');
	len = strlen(s);
	Assert(len >= 2);
	Assert(s[len - 1] == '\'');

	/* Skip the leading quote; we'll handle the trailing quote below */
	s++, len--;

	/* Since len still includes trailing quote, this is enough space */
	newStr = palloc(len);

	for (i = 0, j = 0; i < len; i++)
	{
		if (s[i] == '\\')
		{
			i++;
			switch (s[i])
			{
				case 'b':
					newStr[j] = '\b';
					break;
				case 'f':
					newStr[j] = '\f';
					break;
				case 'n':
					newStr[j] = '\n';
					break;
				case 'r':
					newStr[j] = '\r';
					break;
				case 't':
					newStr[j] = '\t';
					break;
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					{
						int			k;
						long		octVal = 0;

						for (k = 0;
							 s[i + k] >= '0' && s[i + k] <= '7' && k < 3;
							 k++)
							octVal = (octVal << 3) + (s[i + k] - '0');
						i += k - 1;
						newStr[j] = ((char) octVal);
					}
					break;
				default:
					newStr[j] = s[i];
					break;
			}					/* switch */
		}
		else if (s[i] == '\'' && s[i + 1] == '\'')
		{
			/* doubled quote becomes just one quote */
			newStr[j] = s[++i];
		}
		else
			newStr[j] = s[i];
		j++;
	}

	/* We copied the ending quote to newStr, so replace with \0 */
	Assert(j > 0 && j <= len);
	newStr[--j] = '\0';

	return newStr;
}
