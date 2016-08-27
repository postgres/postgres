/* -*-pgsql-c-*- */
/*
 * Scanner for the configuration file
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/backend/utils/misc/guc-file.l
 */

%{

#include "postgres.h"

#include <ctype.h>
#include <unistd.h>

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/guc.h"


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

static void record_config_file_error(const char *errmsg,
						 const char *config_file,
						 int lineno,
						 ConfigVariable **head_p,
						 ConfigVariable **tail_p);

static int	GUC_flex_fatal(const char *msg);
static char *GUC_scanstr(const char *s);

%}

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
 * This function handles both actual config file (re)loads and execution of
 * show_all_file_settings() (i.e., the pg_file_settings view).  In the latter
 * case we don't apply any of the settings, but we make all the usual validity
 * checks, and we return the ConfigVariable list so that it can be printed out
 * by show_all_file_settings().
 */
static ConfigVariable *
ProcessConfigFileInternal(GucContext context, bool applySettings, int elevel)
{
	bool		error = false;
	bool		applying = false;
	const char *ConfFileWithError;
	ConfigVariable *item,
			   *head,
			   *tail;
	int			i;

	/* Parse the main config file into a list of option names and values */
	ConfFileWithError = ConfigFileName;
	head = tail = NULL;

	if (!ParseConfigFile(ConfigFileName, true,
						 NULL, 0, 0, elevel,
						 &head, &tail))
	{
		/* Syntax error(s) detected in the file, so bail out */
		error = true;
		goto bail_out;
	}

	/*
	 * Parse the PG_AUTOCONF_FILENAME file, if present, after the main file to
	 * replace any parameters set by ALTER SYSTEM command.  Because this file
	 * is in the data directory, we can't read it until the DataDir has been
	 * set.
	 */
	if (DataDir)
	{
		if (!ParseConfigFile(PG_AUTOCONF_FILENAME, false,
							 NULL, 0, 0, elevel,
							 &head, &tail))
		{
			/* Syntax error(s) detected in the file, so bail out */
			error = true;
			ConfFileWithError = PG_AUTOCONF_FILENAME;
			goto bail_out;
		}
	}
	else
	{
		/*
		 * If DataDir is not set, the PG_AUTOCONF_FILENAME file cannot be
		 * read.  In this case, we don't want to accept any settings but
		 * data_directory from postgresql.conf, because they might be
		 * overwritten with settings in the PG_AUTOCONF_FILENAME file which
		 * will be read later. OTOH, since data_directory isn't allowed in the
		 * PG_AUTOCONF_FILENAME file, it will never be overwritten later.
		 */
		ConfigVariable *newlist = NULL;

		/*
		 * Prune all items except the last "data_directory" from the list.
		 */
		for (item = head; item; item = item->next)
		{
			if (!item->ignore &&
				strcmp(item->name, "data_directory") == 0)
				newlist = item;
		}

		if (newlist)
			newlist->next = NULL;
		head = tail = newlist;

		/*
		 * Quick exit if data_directory is not present in file.
		 *
		 * We need not do any further processing, in particular we don't set
		 * PgReloadTime; that will be set soon by subsequent full loading of
		 * the config file.
		 */
		if (head == NULL)
			goto bail_out;
	}

	/*
	 * Mark all extant GUC variables as not present in the config file. We
	 * need this so that we can tell below which ones have been removed from
	 * the file since we last processed it.
	 */
	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];

		gconf->status &= ~GUC_IS_IN_FILE;
	}

	/*
	 * Check if all the supplied option names are valid, as an additional
	 * quasi-syntactic check on the validity of the config file.  It is
	 * important that the postmaster and all backends agree on the results of
	 * this phase, else we will have strange inconsistencies about which
	 * processes accept a config file update and which don't.  Hence, unknown
	 * custom variable names have to be accepted without complaint.  For the
	 * same reason, we don't attempt to validate the options' values here.
	 *
	 * In addition, the GUC_IS_IN_FILE flag is set on each existing GUC
	 * variable mentioned in the file; and we detect duplicate entries in the
	 * file and mark the earlier occurrences as ignorable.
	 */
	for (item = head; item; item = item->next)
	{
		struct config_generic *record;

		/* Ignore anything already marked as ignorable */
		if (item->ignore)
			continue;

		/*
		 * Try to find the variable; but do not create a custom placeholder if
		 * it's not there already.
		 */
		record = find_option(item->name, false, elevel);

		if (record)
		{
			/* If it's already marked, then this is a duplicate entry */
			if (record->status & GUC_IS_IN_FILE)
			{
				/*
				 * Mark the earlier occurrence(s) as dead/ignorable.  We could
				 * avoid the O(N^2) behavior here with some additional state,
				 * but it seems unlikely to be worth the trouble.
				 */
				ConfigVariable *pitem;

				for (pitem = head; pitem != item; pitem = pitem->next)
				{
					if (!pitem->ignore &&
						strcmp(pitem->name, item->name) == 0)
						pitem->ignore = true;
				}
			}
			/* Now mark it as present in file */
			record->status |= GUC_IS_IN_FILE;
		}
		else if (strchr(item->name, GUC_QUALIFIER_SEPARATOR) == NULL)
		{
			/* Invalid non-custom variable, so complain */
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("unrecognized configuration parameter \"%s\" in file \"%s\" line %u",
							item->name,
							item->filename, item->sourceline)));
			item->errmsg = pstrdup("unrecognized configuration parameter");
			error = true;
			ConfFileWithError = item->filename;
		}
	}

	/*
	 * If we've detected any errors so far, we don't want to risk applying any
	 * changes.
	 */
	if (error)
		goto bail_out;

	/* Otherwise, set flag that we're beginning to apply changes */
	applying = true;

	/*
	 * Check for variables having been removed from the config file, and
	 * revert their reset values (and perhaps also effective values) to the
	 * boot-time defaults.  If such a variable can't be changed after startup,
	 * report that and continue.
	 */
	for (i = 0; i < num_guc_variables; i++)
	{
		struct config_generic *gconf = guc_variables[i];
		GucStack   *stack;

		if (gconf->reset_source != PGC_S_FILE ||
			(gconf->status & GUC_IS_IN_FILE))
			continue;
		if (gconf->context < PGC_SIGHUP)
		{
			ereport(elevel,
					(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
					 errmsg("parameter \"%s\" cannot be changed without restarting the server",
							gconf->name)));
			record_config_file_error(psprintf("parameter \"%s\" cannot be changed without restarting the server",
											  gconf->name),
									 NULL, 0,
									 &head, &tail);
			error = true;
			continue;
		}

		/* No more to do if we're just doing show_all_file_settings() */
		if (!applySettings)
			continue;

		/*
		 * Reset any "file" sources to "default", else set_config_option will
		 * not override those settings.
		 */
		if (gconf->reset_source == PGC_S_FILE)
			gconf->reset_source = PGC_S_DEFAULT;
		if (gconf->source == PGC_S_FILE)
			gconf->source = PGC_S_DEFAULT;
		for (stack = gconf->stack; stack; stack = stack->prev)
		{
			if (stack->source == PGC_S_FILE)
				stack->source = PGC_S_DEFAULT;
		}

		/* Now we can re-apply the wired-in default (i.e., the boot_val) */
		if (set_config_option(gconf->name, NULL,
							  context, PGC_S_DEFAULT,
							  GUC_ACTION_SET, true, 0, false) > 0)
		{
			/* Log the change if appropriate */
			if (context == PGC_SIGHUP)
				ereport(elevel,
						(errmsg("parameter \"%s\" removed from configuration file, reset to default",
								gconf->name)));
		}
	}

	/*
	 * Restore any variables determined by environment variables or
	 * dynamically-computed defaults.  This is a no-op except in the case
	 * where one of these had been in the config file and is now removed.
	 *
	 * In particular, we *must not* do this during the postmaster's initial
	 * loading of the file, since the timezone functions in particular should
	 * be run only after initialization is complete.
	 *
	 * XXX this is an unmaintainable crock, because we have to know how to set
	 * (or at least what to call to set) every variable that could potentially
	 * have PGC_S_DYNAMIC_DEFAULT or PGC_S_ENV_VAR source. However, there's no
	 * time to redesign it for 9.1.
	 */
	if (context == PGC_SIGHUP && applySettings)
	{
		InitializeGUCOptionsFromEnvironment();
		pg_timezone_abbrev_initialize();
		/* this selects SQL_ASCII in processes not connected to a database */
		SetConfigOption("client_encoding", GetDatabaseEncodingName(),
						PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);
	}

	/*
	 * Now apply the values from the config file.
	 */
	for (item = head; item; item = item->next)
	{
		char	   *pre_value = NULL;
		int			scres;

		/* Ignore anything marked as ignorable */
		if (item->ignore)
			continue;

		/* In SIGHUP cases in the postmaster, we want to report changes */
		if (context == PGC_SIGHUP && applySettings && !IsUnderPostmaster)
		{
			const char *preval = GetConfigOption(item->name, true, false);

			/* If option doesn't exist yet or is NULL, treat as empty string */
			if (!preval)
				preval = "";
			/* must dup, else might have dangling pointer below */
			pre_value = pstrdup(preval);
		}

		scres = set_config_option(item->name, item->value,
								  context, PGC_S_FILE,
								  GUC_ACTION_SET, applySettings, 0, false);
		if (scres > 0)
		{
			/* variable was updated, so log the change if appropriate */
			if (pre_value)
			{
				const char *post_value = GetConfigOption(item->name, true, false);

				if (!post_value)
					post_value = "";
				if (strcmp(pre_value, post_value) != 0)
					ereport(elevel,
							(errmsg("parameter \"%s\" changed to \"%s\"",
									item->name, item->value)));
			}
			item->applied = true;
		}
		else if (scres == 0)
		{
			error = true;
			item->errmsg = pstrdup("setting could not be applied");
			ConfFileWithError = item->filename;
		}
		else
		{
			/* no error, but variable's active value was not changed */
			item->applied = true;
		}

		/*
		 * We should update source location unless there was an error, since
		 * even if the active value didn't change, the reset value might have.
		 * (In the postmaster, there won't be a difference, but it does matter
		 * in backends.)
		 */
		if (scres != 0 && applySettings)
			set_config_sourcefile(item->name, item->filename,
								  item->sourceline);

		if (pre_value)
			pfree(pre_value);
	}

	/* Remember when we last successfully loaded the config file. */
	if (applySettings)
		PgReloadTime = GetCurrentTimestamp();

bail_out:
	if (error && applySettings)
	{
		/* During postmaster startup, any error is fatal */
		if (context == PGC_POSTMASTER)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors",
							ConfFileWithError)));
		else if (applying)
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors; unaffected changes were applied",
							ConfFileWithError)));
		else
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors; no changes were applied",
							ConfFileWithError)));
	}

	/* Successful or otherwise, return the collected data list */
	return head;
}

/*
 * Given a configuration file or directory location that may be a relative
 * path, return an absolute one.  We consider the location to be relative to
 * the directory holding the calling file, or to DataDir if no calling file.
 */
static char *
AbsoluteConfigLocation(const char *location, const char *calling_file)
{
	char		abs_path[MAXPGPATH];

	if (is_absolute_path(location))
		return pstrdup(location);
	else
	{
		if (calling_file != NULL)
		{
			strlcpy(abs_path, calling_file, sizeof(abs_path));
			get_parent_directory(abs_path);
			join_path_components(abs_path, abs_path, location);
			canonicalize_path(abs_path);
		}
		else
		{
			AssertState(DataDir);
			join_path_components(abs_path, DataDir, location);
			canonicalize_path(abs_path);
		}
		return pstrdup(abs_path);
	}
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
	 * Reject too-deep include nesting depth.  This is just a safety check to
	 * avoid dumping core due to stack overflow if an include file loops back
	 * to itself.  The maximum nesting depth is pretty arbitrary.
	 */
	if (depth > 10)
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
static void
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
 * holds for flex 2.5.31 (earliest we support) and flex 2.5.35 (latest as of
 * this writing).  Otherwise, we would need to copy the message.
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
 *	depth: recursion depth (should be 0 in the outermost call)
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

	lex_buffer = yy_create_buffer(fp, YY_BUF_SIZE);
	yy_switch_to_buffer(lex_buffer);

	/* This loop iterates once per logical line */
	while ((token = yylex()))
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
		token = yylex();
		if (token == GUC_EQUALS)
			token = yylex();

		/* now we must have the option value */
		if (token != GUC_ID &&
			token != GUC_STRING &&
			token != GUC_INTEGER &&
			token != GUC_REAL &&
			token != GUC_UNQUOTED_STRING)
			goto parse_error;
		if (token == GUC_STRING)	/* strip quotes and escapes */
			opt_value = GUC_scanstr(yytext);
		else
			opt_value = pstrdup(yytext);

		/* now we'd like an end of line, or possibly EOF */
		token = yylex();
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
			yy_switch_to_buffer(lex_buffer);
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
			yy_switch_to_buffer(lex_buffer);
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
			yy_switch_to_buffer(lex_buffer);
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
			token = yylex();
		/* break out of loop on EOF */
		if (token == 0)
			break;
	}

cleanup:
	yy_delete_buffer(lex_buffer);
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
	char	   *directory;
	DIR		   *d;
	struct dirent *de;
	char	  **filenames;
	int			num_filenames;
	int			size_filenames;
	bool		status;

	directory = AbsoluteConfigLocation(includedir, calling_file);
	d = AllocateDir(directory);
	if (d == NULL)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open configuration directory \"%s\": %m",
						directory)));
		record_config_file_error(psprintf("could not open directory \"%s\"",
										  directory),
								 calling_file, calling_lineno,
								 head_p, tail_p);
		status = false;
		goto cleanup;
	}

	/*
	 * Read the directory and put the filenames in an array, so we can sort
	 * them prior to processing the contents.
	 */
	size_filenames = 32;
	filenames = (char **) palloc(size_filenames * sizeof(char *));
	num_filenames = 0;

	while ((de = ReadDir(d, directory)) != NULL)
	{
		struct stat st;
		char		filename[MAXPGPATH];

		/*
		 * Only parse files with names ending in ".conf".  Explicitly reject
		 * files starting with ".".  This excludes things like "." and "..",
		 * as well as typical hidden files, backup files, and editor debris.
		 */
		if (strlen(de->d_name) < 6)
			continue;
		if (de->d_name[0] == '.')
			continue;
		if (strcmp(de->d_name + strlen(de->d_name) - 5, ".conf") != 0)
			continue;

		join_path_components(filename, directory, de->d_name);
		canonicalize_path(filename);
		if (stat(filename, &st) == 0)
		{
			if (!S_ISDIR(st.st_mode))
			{
				/* Add file to array, increasing its size in blocks of 32 */
				if (num_filenames >= size_filenames)
				{
					size_filenames += 32;
					filenames = (char **) repalloc(filenames,
											size_filenames * sizeof(char *));
				}
				filenames[num_filenames] = pstrdup(filename);
				num_filenames++;
			}
		}
		else
		{
			/*
			 * stat does not care about permissions, so the most likely reason
			 * a file can't be accessed now is if it was removed between the
			 * directory listing and now.
			 */
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m",
							filename)));
			record_config_file_error(psprintf("could not stat file \"%s\"",
											  filename),
									 calling_file, calling_lineno,
									 head_p, tail_p);
			status = false;
			goto cleanup;
		}
	}

	if (num_filenames > 0)
	{
		int			i;

		qsort(filenames, num_filenames, sizeof(char *), pg_qsort_strcmp);
		for (i = 0; i < num_filenames; i++)
		{
			if (!ParseConfigFile(filenames[i], true,
								 calling_file, calling_lineno,
								 depth, elevel,
								 head_p, tail_p))
			{
				status = false;
				goto cleanup;
			}
		}
	}
	status = true;

cleanup:
	if (d)
		FreeDir(d);
	pfree(directory);
	return status;
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
 *		scanstr
 *
 * Strip the quotes surrounding the given string, and collapse any embedded
 * '' sequences and backslash escapes.
 *
 * the string returned is palloc'd and should eventually be pfree'd by the
 * caller.
 */
static char *
GUC_scanstr(const char *s)
{
	char	   *newStr;
	int			len,
				i,
				j;

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
