/* -*-pgsql-c-*- */
/*
 * Scanner for the configuration file
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/misc/guc-file.l,v 1.19 2003/09/25 06:58:05 petere Exp $
 */

%{

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/guc.h"

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#define fprintf(file, fmt, msg)  ereport(ERROR, (errmsg_internal("%s", msg)))

#define CONFIG_FILENAME "postgresql.conf"

static unsigned ConfigFileLineno;

enum {
	GUC_ID = 1,
	GUC_STRING = 2,
	GUC_INTEGER = 3,
	GUC_REAL = 4,
	GUC_EQUALS = 5,
	GUC_UNQUOTED_STRING = 6,
	GUC_EOL = 99,
	GUC_ERROR = 100
};

#define YY_USER_INIT (ConfigFileLineno = 1)

/* prototype, so compiler is happy with our high warnings setting */
int GUC_yylex(void);
char *GUC_scanstr(char *);
%}

%option 8bit
%option never-interactive
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

UNQUOTED_STRING {LETTER}({LETTER_OR_DIGIT}|[-._:/])*
STRING          \'([^'\n]|\\.)*\'

%%

\n              ConfigFileLineno++; return GUC_EOL;
[ \t\r]+        /* eat whitespace */
#.*$            /* eat comment */

{ID}            return GUC_ID;
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
		free(item->name);
		free(item->value);
		free(item);
		item = save;
	}
}


/*
 * Official function to read and process the configuration file. The
 * parameter indicates in what context the file is being read
 * (postmaster startup, backend startup, or SIGHUP). All options
 * mentioned in the configuration file are set to new values. This
 * function does not return if an error occurs. If an error occurs, no
 * values will be changed.
 */
void
ProcessConfigFile(GucContext context)
{
	int token, parse_state;
	char *opt_name, *opt_value;
	char *filename;
	struct name_value_pair *item, *head, *tail;
	int elevel;
	FILE * fp;

	Assert(context == PGC_POSTMASTER || context == PGC_BACKEND 
		|| context == PGC_SIGHUP);
	Assert(DataDir);
	elevel = (context == PGC_SIGHUP) ? DEBUG4 : ERROR;

	/*
	 * Open file
	 */
	filename = malloc(strlen(DataDir) + strlen(CONFIG_FILENAME) + 2);
	if (filename == NULL)
	{
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}
	sprintf(filename, "%s/" CONFIG_FILENAME, DataDir);

    fp = AllocateFile(filename, "r");
    if (!fp)
    {
		free(filename);
        /* File not found is fine */
        if (errno != ENOENT)
            ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open configuration file \"%s\": %m", CONFIG_FILENAME)));
		return;
    }

	/*
	 * Parse
	 */
	yyin = fp;
    parse_state = 0;
	head = tail = NULL;
	opt_name = opt_value = NULL;

    while ((token = yylex()))
        switch(parse_state)
        {
            case 0: /* no previous input */
                if (token == GUC_EOL) /* empty line */
                    continue;
                if (token != GUC_ID)
                    goto parse_error;
                opt_name = strdup(yytext);
				if (opt_name == NULL)
					goto out_of_memory;
                parse_state = 1;
                break;

            case 1: /* found name */
                /* ignore equals sign */
                if (token == GUC_EQUALS)
                    token = yylex();

                if (token != GUC_ID && token != GUC_STRING && 
					token != GUC_INTEGER && token != GUC_REAL && 
					token != GUC_UNQUOTED_STRING)
                    goto parse_error;
                opt_value = strdup(yytext);
				if (opt_value == NULL)
					goto out_of_memory;
				if (token == GUC_STRING)
				{
					/* remove the beginning and ending quote/apostrophe */
					/* first: shift the whole thing down one character */
					memmove(opt_value,opt_value+1,strlen(opt_value)-1);
					/* second: null out the 2 characters we shifted */
					opt_value[strlen(opt_value)-2]='\0';
					/* do the escape thing.  free()'s the strdup above */
					opt_value=GUC_scanstr(opt_value);
				}
                parse_state = 2;
                break;

            case 2: /* now we'd like an end of line */
				if (token != GUC_EOL)
					goto parse_error;

				/* append to list */
				item = malloc(sizeof *item);
				if (item == NULL)
					goto out_of_memory;
				item->name = opt_name;
				item->value = opt_value;
				item->next = NULL;

				if (!head)
					tail = head = item;
				else
				{
					tail->next = item;
					tail = item;
				}

                parse_state = 0;
                break;
        }

	FreeFile(fp);
	free(filename);

	/*
	 * Check if all options are valid
	 */
    for(item = head; item; item=item->next)
	{
		if (!set_config_option(item->name, item->value, context,
							   PGC_S_FILE, false, false))
			goto cleanup_exit;
	}

    /* If we got here all the options parsed okay. */
	for(item = head; item; item=item->next)
		set_config_option(item->name, item->value, context,
						  PGC_S_FILE, false, true);

 cleanup_exit:
	free_name_value_list(head);
	return;

 parse_error:
	FreeFile(fp);
	free(filename);
	free_name_value_list(head);
	ereport(elevel,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("syntax error in file \"%s\" line %u, near token \"%s\"", 
					CONFIG_FILENAME, ConfigFileLineno, yytext)));
	return;

 out_of_memory:
	FreeFile(fp);
	free(filename);
	free_name_value_list(head);
	ereport(elevel,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory")));
	return;
}



/* ----------------
 *		scanstr
 *
 * if the string passed in has escaped codes, map the escape codes to actual
 * chars
 *
 * the string returned is malloc'd and should eventually be free'd by the
 * caller!
 * ----------------
 */

char *
GUC_scanstr(char *s)
{
	char	   *newStr;
	int			len,
				i,
				j;

	if (s == NULL || s[0] == '\0')
	{
		if (s != NULL)
			free(s);
		return strdup("");
	}
	len = strlen(s);

	newStr = malloc(len + 1);	/* string cannot get longer */
	if (newStr == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

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
				}
			}					/* switch */
		else
			newStr[j] = s[i];
		j++;
	}
	newStr[j] = '\0';
	free(s);
	return newStr;
}
