/* -*-pgsql-c-*- */
/*
 * Scanner for the configuration file
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/backend/utils/misc/guc-file.l,v 1.32.2.2 2006/01/02 19:55:31 tgl Exp $
 */

%{

#include "postgres.h"

#include <unistd.h>
#include <ctype.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/guc.h"

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  ereport(ERROR, (errmsg_internal("%s", msg)))

static unsigned ConfigFileLineno;

enum {
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

/* prototype, so compiler is happy with our high warnings setting */
int GUC_yylex(void);
static char *GUC_scanstr(const char *s);
%}

%option 8bit
%option never-interactive
%option nodefault
%option nounput
%option noyywrap


SIGN            ("-"|"+")
DIGIT           [0-9]
HEXDIGIT        [0-9a-fA-F]

INTEGER         {SIGN}?({DIGIT}+|0x{HEXDIGIT}+)

EXPONENT        [Ee]{SIGN}?{DIGIT}+
REAL            {SIGN}?{DIGIT}*"."{DIGIT}*{EXPONENT}?

LETTER          [A-Za-z_\200-\377]
LETTER_OR_DIGIT [A-Za-z_0-9\200-\377]

ID              {LETTER}{LETTER_OR_DIGIT}*
QUALIFIED_ID    {ID}"."{ID}

UNQUOTED_STRING {LETTER}({LETTER_OR_DIGIT}|[-._:/])*
STRING          \'([^'\\\n]|\\.|\'\')*\'

%%

\n              ConfigFileLineno++; return GUC_EOL;
[ \t\r]+        /* eat whitespace */
#.*             /* eat comment (.* matches anything until newline) */

{ID}            return GUC_ID;
{QUALIFIED_ID}  return GUC_QUALIFIED_ID;
{STRING}        return GUC_STRING;
{UNQUOTED_STRING} return GUC_UNQUOTED_STRING;
{INTEGER}       return GUC_INTEGER;
{REAL}          return GUC_REAL;
=               return GUC_EQUALS;

.               return GUC_ERROR;

%%


struct name_value_pair
{
	char       *name;
	char       *value;
	struct name_value_pair *next;
};


/*
 * Free a list of name/value pairs, including the names and the values
 */
static void
free_name_value_list(struct name_value_pair * list)
{
	struct name_value_pair *item;

	item = list;
	while (item)
	{
		struct name_value_pair *save;

		save = item->next;
		pfree(item->name);
		pfree(item->value);
		pfree(item);
		item = save;
	}
}


/*
 * Official function to read and process the configuration file. The
 * parameter indicates in what context the file is being read --- either
 * postmaster startup (including standalone-backend startup) or SIGHUP.
 * All options mentioned in the configuration file are set to new values.
 * If an error occurs, no values will be changed.
 */
void
ProcessConfigFile(GucContext context)
{
	int			elevel;
	int			token;
	char	   *opt_name, *opt_value;
	struct name_value_pair *item, *head, *tail;
	FILE	   *fp;

	Assert(context == PGC_POSTMASTER || context == PGC_SIGHUP);

	if (context == PGC_SIGHUP)
	{
		/*
		 * To avoid cluttering the log, only the postmaster bleats loudly
		 * about problems with the config file.
		 */
		elevel = IsUnderPostmaster ? DEBUG2 : LOG;
	}
	else
		elevel = ERROR;

	fp = AllocateFile(ConfigFileName, "r");
	if (!fp)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open configuration file \"%s\": %m",
						ConfigFileName)));
		return;
	}

	/*
	 * Parse
	 */
	yyrestart(fp);
	head = tail = NULL;
	opt_name = opt_value = NULL;
	ConfigFileLineno = 1;

	/* This loop iterates once per logical line */
	while ((token = yylex()))
	{
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
		if (token != GUC_EOL && token != 0)
			goto parse_error;

		/* OK, save the option name and value */
		if (strcmp(opt_name, "custom_variable_classes") == 0)
		{
			/*
			 * This variable must be processed first as it controls
			 * the validity of other variables; so apply immediately.
			 */
			if (!set_config_option(opt_name, opt_value, context,
								   PGC_S_FILE, false, true))
			{
				pfree(opt_name);
				pfree(opt_value);
				FreeFile(fp);
				goto cleanup_exit;
			}
			pfree(opt_name);
			pfree(opt_value);
		}
		else
		{
			/* append to list */
			item = palloc(sizeof *item);
			item->name = opt_name;
			item->value = opt_value;
			item->next = NULL;
			if (!head)
				head = item;
			else
				tail->next = item;
			tail = item;
		}

		/* break out of loop if read EOF, else loop for next line */
		if (token == 0)
			break;
	}

	FreeFile(fp);

	/*
	 * Check if all options are valid
	 */
	for(item = head; item; item=item->next)
	{
		if (!set_config_option(item->name, item->value, context,
							   PGC_S_FILE, false, false))
			goto cleanup_exit;
	}

	/* If we got here all the options parsed okay, so apply them. */
	for(item = head; item; item=item->next)
	{
		set_config_option(item->name, item->value, context,
						  PGC_S_FILE, false, true);
	}

 cleanup_exit:
	free_name_value_list(head);
	return;

 parse_error:
	FreeFile(fp);
	free_name_value_list(head);
	if (token == GUC_EOL || token == 0)
		ereport(elevel,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error in file \"%s\" line %u, near end of line",
						ConfigFileName, ConfigFileLineno - 1)));
	else
		ereport(elevel,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error in file \"%s\" line %u, near token \"%s\"", 
						ConfigFileName, ConfigFileLineno, yytext)));
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
	Assert(s[len-1] == '\'');

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
		else if (s[i] == '\'' && s[i+1] == '\'')
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
