/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling of 'SET var TO',
 *		'SHOW var' and 'RESET var' statements.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/variable.c,v 1.40 2000/08/01 18:29:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <ctype.h>
#include <time.h>

#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_shadow.h"
#include "commands/variable.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/tqual.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif



static bool show_date(void);
static bool reset_date(void);
static bool parse_date(char *);
static bool show_timezone(void);
static bool reset_timezone(void);
static bool parse_timezone(char *);

static bool show_DefaultXactIsoLevel(void);
static bool reset_DefaultXactIsoLevel(void);
static bool parse_DefaultXactIsoLevel(char *);
static bool show_XactIsoLevel(void);
static bool reset_XactIsoLevel(void);
static bool parse_XactIsoLevel(char *);
static bool parse_random_seed(char *);
static bool show_random_seed(void);
static bool reset_random_seed(void);


/*
 * get_token
 *		Obtain the next item in a comma-separated list of items,
 *		where each item can be either "word" or "word=word".
 *		The "word=word" form is only accepted if 'val' is not NULL.
 *		Words are any sequences not containing whitespace, ',', or '='.
 *		Whitespace can appear between the words and punctuation.
 *
 * 'tok': receives a pointer to first word of item, or NULL if none.
 * 'val': if not NULL, receives a pointer to second word, or NULL if none.
 * 'str': start of input string.
 *
 * Returns NULL if input string contained no more words, else pointer
 * to just past this item, which can be used as 'str' for next call.
 * (If this is the last item, returned pointer will point at a null char,
 * so caller can alternatively check for that instead of calling again.)
 *
 * NB: input string is destructively modified by placing null characters
 * at ends of words!
 *
 * A former version of this code avoided modifying the input string by
 * returning palloc'd copies of the words.  However, we want to use this
 * code early in backend startup to parse the PGDATESTYLE environment var,
 * and palloc/pfree aren't initialized at that point.  Cleanest answer
 * seems to be to palloc in SetPGVariable() so that we can treat the string
 * as modifiable here.
 */
static char *
get_token(char **tok, char **val, char *str)
{
	char		ch;

	*tok = NULL;
	if (val != NULL)
		*val = NULL;

	if (!str || *str == '\0')
		return NULL;

	/* skip leading white space */
	while (isspace((int) *str))
		str++;

	/* end of string? then return NULL */
	if (*str == '\0')
		return NULL;

	if (*str == ',' || *str == '=')
		elog(ERROR, "Syntax error near \"%s\": empty setting", str);

	/* OK, at beginning of non-empty item */
	*tok = str;

	/* Advance to end of word */
	while (*str && !isspace((int) *str) && *str != ',' && *str != '=')
		str++;

	/* Terminate word string for caller */
	ch = *str;
	*str = '\0';

	/* Skip any whitespace */
	while (isspace((int) ch))
		ch = *(++str);

	/* end of string? */
	if (ch == '\0')
		return str;
	/* delimiter? */
	if (ch == ',')
		return ++str;

	/* Had better be '=', and caller must be expecting it */
	if (val == NULL || ch != '=')
		elog(ERROR, "Syntax error near \"%s\"", str);

	/* '=': get the value */
	str++;

	/* skip whitespace after '=' */
	while (isspace((int) *str))
		str++;

	if (*str == ',' || *str == '\0')
		elog(ERROR, "Syntax error near \"=%s\"", str);

	/* OK, at beginning of non-empty value */
	*val = str;

	/* Advance to end of word */
	while (*str && !isspace((int) *str) && *str != ',')
		str++;

	/* Terminate word string for caller */
	ch = *str;
	*str = '\0';

	/* Skip any whitespace */
	while (isspace((int) ch))
		ch = *(++str);

	/* end of string? */
	if (ch == '\0')
		return str;
	/* delimiter? */
	if (ch == ',')
		return ++str;

	elog(ERROR, "Syntax error near \"%s\"", str);

	return str;
}


/*
 * DATE_STYLE
 *
 * NOTE: set_default_datestyle() is called during backend startup to check
 * if the PGDATESTYLE environment variable is set.	We want the env var
 * to determine the value that "RESET DateStyle" will reset to!
 */

/* These get initialized from the "master" values in init/globals.c */
static int	DefaultDateStyle;
static bool DefaultEuroDates;

static bool
parse_date(char *value)
{
	char	   *tok;
	int			dcnt = 0,
				ecnt = 0;

	if (value == NULL)
	{
		reset_date();
		return TRUE;
	}

	while ((value = get_token(&tok, NULL, value)) != 0)
	{
		/* Ugh. Somebody ought to write a table driven version -- mjl */

		if (!strcasecmp(tok, "ISO"))
		{
			DateStyle = USE_ISO_DATES;
			dcnt++;
		}
		else if (!strcasecmp(tok, "SQL"))
		{
			DateStyle = USE_SQL_DATES;
			dcnt++;
		}
		else if (!strcasecmp(tok, "POSTGRES"))
		{
			DateStyle = USE_POSTGRES_DATES;
			dcnt++;
		}
		else if (!strcasecmp(tok, "GERMAN"))
		{
			DateStyle = USE_GERMAN_DATES;
			dcnt++;
			EuroDates = TRUE;
			if ((ecnt > 0) && (!EuroDates))
				ecnt++;
		}
		else if (!strncasecmp(tok, "EURO", 4))
		{
			EuroDates = TRUE;
			if ((dcnt <= 0) || (DateStyle != USE_GERMAN_DATES))
				ecnt++;
		}
		else if ((!strcasecmp(tok, "US"))
				 || (!strncasecmp(tok, "NONEURO", 7)))
		{
			EuroDates = FALSE;
			if ((dcnt <= 0) || (DateStyle == USE_GERMAN_DATES))
				ecnt++;
		}
		else if (!strcasecmp(tok, "DEFAULT"))
		{
			DateStyle = DefaultDateStyle;
			EuroDates = DefaultEuroDates;
			ecnt++;
		}
		else
			elog(ERROR, "Bad value for date style (%s)", tok);
	}

	if (dcnt > 1 || ecnt > 1)
		elog(NOTICE, "Conflicting settings for date");

	return TRUE;
}

static bool
show_date()
{
	char		buf[64];

	strcpy(buf, "DateStyle is ");
	switch (DateStyle)
	{
		case USE_ISO_DATES:
			strcat(buf, "ISO");
			break;
		case USE_SQL_DATES:
			strcat(buf, "SQL");
			break;
		case USE_GERMAN_DATES:
			strcat(buf, "German");
			break;
		default:
			strcat(buf, "Postgres");
			break;
	};
	strcat(buf, " with ");
	strcat(buf, ((EuroDates) ? "European" : "US (NonEuropean)"));
	strcat(buf, " conventions");

	elog(NOTICE, buf, NULL);

	return TRUE;
}

static bool
reset_date()
{
	DateStyle = DefaultDateStyle;
	EuroDates = DefaultEuroDates;

	return TRUE;
}

void
set_default_datestyle(void)
{
	char	   *DBDate;

	/*
	 * Initialize from compile-time defaults in init/globals.c. NB: this
	 * is a necessary step; consider PGDATESTYLE="DEFAULT".
	 */
	DefaultDateStyle = DateStyle;
	DefaultEuroDates = EuroDates;

	/* If the environment var is set, override compiled-in values */
	DBDate = getenv("PGDATESTYLE");
	if (DBDate == NULL)
		return;

	/*
	 * Make a modifiable copy --- overwriting the env var doesn't seem
	 * like a good idea, even though we currently won't look at it again.
	 * Note that we cannot use palloc at this early stage of
	 * initialization.
	 */
	DBDate = strdup(DBDate);

	/* Parse desired setting into DateStyle/EuroDates */
	parse_date(DBDate);

	free(DBDate);

	/* And make it the default for future RESETs */
	DefaultDateStyle = DateStyle;
	DefaultEuroDates = EuroDates;
}


/* Timezone support
 * Working storage for strings is allocated with an arbitrary size of 64 bytes.
 */

static char *defaultTZ = NULL;
static char TZvalue[64];
static char tzbuf[64];

/*
 *
 * TIMEZONE
 *
 */
/* parse_timezone()
 * Handle SET TIME ZONE...
 * Try to save existing TZ environment variable for later use in RESET TIME ZONE.
 * - thomas 1997-11-10
 */
static bool
parse_timezone(char *value)
{
	char	   *tok;

	if (value == NULL)
	{
		reset_timezone();
		return TRUE;
	}

	while ((value = get_token(&tok, NULL, value)) != 0)
	{
		/* Not yet tried to save original value from environment? */
		if (defaultTZ == NULL)
		{
			/* found something? then save it for later */
			if ((defaultTZ = getenv("TZ")) != NULL)
				strcpy(TZvalue, defaultTZ);

			/* found nothing so mark with an invalid pointer */
			else
				defaultTZ = (char *) -1;
		}

		strcpy(tzbuf, "TZ=");
		strcat(tzbuf, tok);
		if (putenv(tzbuf) != 0)
			elog(ERROR, "Unable to set TZ environment variable to %s", tok);

		tzset();
	}

	return TRUE;
}	/* parse_timezone() */

static bool
show_timezone()
{
	char	   *tz;

	tz = getenv("TZ");

	elog(NOTICE, "Time zone is %s", ((tz != NULL) ? tz : "unknown"));

	return TRUE;
}	/* show_timezone() */

/* reset_timezone()
 * Set TZ environment variable to original value.
 * Note that if TZ was originally not set, TZ should be cleared.
 * unsetenv() works fine, but is BSD, not POSIX, and is not available
 * under Solaris, among others. Apparently putenv() called as below
 * clears the process-specific environment variables.
 * Other reasonable arguments to putenv() (e.g. "TZ=", "TZ", "") result
 * in a core dump (under Linux anyway).
 * - thomas 1998-01-26
 */
static bool
reset_timezone()
{
	/* no time zone has been set in this session? */
	if (defaultTZ == NULL)
	{
	}

	/* time zone was set and original explicit time zone available? */
	else if (defaultTZ != (char *) -1)
	{
		strcpy(tzbuf, "TZ=");
		strcat(tzbuf, TZvalue);
		if (putenv(tzbuf) != 0)
			elog(ERROR, "Unable to set TZ environment variable to %s", TZvalue);
		tzset();
	}

	/*
	 * otherwise, time zone was set but no original explicit time zone
	 * available
	 */
	else
	{
		strcpy(tzbuf, "=");
		if (putenv(tzbuf) != 0)
			elog(ERROR, "Unable to clear TZ environment variable");
		tzset();
	}

	return TRUE;
}	/* reset_timezone() */



/* SET TRANSACTION */

static bool
parse_DefaultXactIsoLevel(char *value)
{
#if 0
	TransactionState s = CurrentTransactionState;
#endif

	if (value == NULL)
	{
		reset_DefaultXactIsoLevel();
		return TRUE;
	}

#if 0
	if (s->state != TRANS_DEFAULT)
	{
		elog(ERROR, "ALTER SESSION/SET TRANSACTION ISOLATION LEVEL"
			 " can not be called within a transaction");
		return TRUE;
	}
#endif

	if (strcasecmp(value, "SERIALIZABLE") == 0)
		DefaultXactIsoLevel = XACT_SERIALIZABLE;
	else if (strcasecmp(value, "COMMITTED") == 0)
		DefaultXactIsoLevel = XACT_READ_COMMITTED;
	else
		elog(ERROR, "Bad TRANSACTION ISOLATION LEVEL (%s)", value);

	return TRUE;
}

static bool
show_DefaultXactIsoLevel()
{

	if (DefaultXactIsoLevel == XACT_SERIALIZABLE)
		elog(NOTICE, "Default TRANSACTION ISOLATION LEVEL is SERIALIZABLE");
	else
		elog(NOTICE, "Default TRANSACTION ISOLATION LEVEL is READ COMMITTED");
	return TRUE;
}

static bool
reset_DefaultXactIsoLevel()
{
#if 0
	TransactionState s = CurrentTransactionState;

	if (s->state != TRANS_DEFAULT)
	{
		elog(ERROR, "ALTER SESSION/SET TRANSACTION ISOLATION LEVEL"
			 " can not be called within a transaction");
		return TRUE;
	}
#endif

	DefaultXactIsoLevel = XACT_READ_COMMITTED;

	return TRUE;
}

static bool
parse_XactIsoLevel(char *value)
{

	if (value == NULL)
	{
		reset_XactIsoLevel();
		return TRUE;
	}

	if (SerializableSnapshot != NULL)
	{
		elog(ERROR, "SET TRANSACTION ISOLATION LEVEL must be called before any query");
		return TRUE;
	}


	if (strcasecmp(value, "SERIALIZABLE") == 0)
		XactIsoLevel = XACT_SERIALIZABLE;
	else if (strcasecmp(value, "COMMITTED") == 0)
		XactIsoLevel = XACT_READ_COMMITTED;
	else
		elog(ERROR, "Bad TRANSACTION ISOLATION LEVEL (%s)", value);

	return TRUE;
}

static bool
show_XactIsoLevel()
{

	if (XactIsoLevel == XACT_SERIALIZABLE)
		elog(NOTICE, "TRANSACTION ISOLATION LEVEL is SERIALIZABLE");
	else
		elog(NOTICE, "TRANSACTION ISOLATION LEVEL is READ COMMITTED");
	return TRUE;
}

static bool
reset_XactIsoLevel()
{

	if (SerializableSnapshot != NULL)
	{
		elog(ERROR, "SET TRANSACTION ISOLATION LEVEL must be called before any query");
		return TRUE;
	}

	XactIsoLevel = DefaultXactIsoLevel;

	return TRUE;
}


/*
 * Random number seed
 */
static bool
parse_random_seed(char *value)
{
	double		seed = 0;

	if (value == NULL)
		reset_random_seed();
	else
	{
		sscanf(value, "%lf", &seed);
		DirectFunctionCall1(setseed, Float8GetDatum(seed));
	}
	return (TRUE);
}

static bool
show_random_seed(void)
{
	elog(NOTICE, "Seed for random number generator is not known");
	return (TRUE);
}

static bool
reset_random_seed(void)
{
	double		seed = 0.5;

	DirectFunctionCall1(setseed, Float8GetDatum(seed));
	return (TRUE);
}



void
SetPGVariable(const char *name, const char *value)
{
    /*
     * Special cases ought to be removed and handled separately
     * by TCOP
     */
    if (strcasecmp(name, "datestyle")==0)
        parse_date(pstrdup(value));
    else if (strcasecmp(name, "timezone")==0)
        parse_timezone(pstrdup(value));
    else if (strcasecmp(name, "DefaultXactIsoLevel")==0)
        parse_DefaultXactIsoLevel(pstrdup(value));
    else if (strcasecmp(name, "XactIsoLevel")==0)
        parse_XactIsoLevel(pstrdup(value));
#ifdef MULTIBYTE
    else if (strcasecmp(name, "client_encoding")==0)
        parse_client_encoding(pstrdup(value));
    else if (strcasecmp(name, "server_encoding")==0)
        parse_server_encoding(pstrdup(value));
#endif
    else if (strcasecmp(name, "random_seed")==0)
        parse_random_seed(pstrdup(value));
    else
        SetConfigOption(name, value, superuser() ? PGC_SUSET : PGC_USERSET);
}


void
GetPGVariable(const char *name)
{
    if (strcasecmp(name, "datestyle")==0)
        show_date();
    else if (strcasecmp(name, "timezone")==0)
        show_timezone();
    else if (strcasecmp(name, "DefaultXactIsoLevel")==0)
        show_DefaultXactIsoLevel();
    else if (strcasecmp(name, "XactIsoLevel")==0)
        show_XactIsoLevel();
#ifdef MULTIBYTE
    else if (strcasecmp(name, "client_encoding")==0)
        show_client_encoding();
    else if (strcasecmp(name, "server_encoding")==0)
        show_server_encoding();
#endif
    else if (strcasecmp(name, "random_seed")==0)
        show_random_seed();
    else
    {
        const char * val = GetConfigOption(name);
        elog(NOTICE, "%s is %s", name, val);
    }
} 

void
ResetPGVariable(const char *name)
{
    if (strcasecmp(name, "datestyle")==0)
        reset_date();
    else if (strcasecmp(name, "timezone")==0)
        reset_timezone();
    else if (strcasecmp(name, "DefaultXactIsoLevel")==0)
			reset_DefaultXactIsoLevel();
    else if (strcasecmp(name, "XactIsoLevel")==0)
			reset_XactIsoLevel();
#ifdef MULTIBYTE
    else if (strcasecmp(name, "client_encoding")==0)
        reset_client_encoding();
    else if (strcasecmp(name, "server_encoding")==0)
        reset_server_encoding();
#endif
    else if (strcasecmp(name, "random_seed")==0)
        reset_random_seed();
    else
        SetConfigOption(name, NULL, superuser() ? PGC_SUSET : PGC_USERSET);
}  
