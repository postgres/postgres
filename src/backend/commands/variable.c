/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling of 'SET var TO',
 *		'SHOW var' and 'RESET var' statements.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/variable.c,v 1.58 2002/02/23 01:31:35 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <time.h>

#include "access/xact.h"
#include "catalog/pg_shadow.h"
#include "commands/variable.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/guc.h"
#include "utils/tqual.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#else
/* Grand unified hard-coded badness */
#define pg_get_client_encoding_name()  "SQL_ASCII"
#define GetDatabaseEncodingName() "SQL_ASCII"
#endif


static bool show_datestyle(void);
static bool reset_datestyle(void);
static bool parse_datestyle(List *);
static bool show_timezone(void);
static bool reset_timezone(void);
static bool parse_timezone(List *);

static bool show_XactIsoLevel(void);
static bool reset_XactIsoLevel(void);
static bool parse_XactIsoLevel(List *);
static bool show_random_seed(void);
static bool reset_random_seed(void);
static bool parse_random_seed(List *);

static bool show_client_encoding(void);
static bool reset_client_encoding(void);
static bool parse_client_encoding(List *);
static bool show_server_encoding(void);
static bool reset_server_encoding(void);
static bool parse_server_encoding(List *);


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
	while (isspace((unsigned char) *str))
		str++;

	/* end of string? then return NULL */
	if (*str == '\0')
		return NULL;

	if (*str == ',' || *str == '=')
		elog(ERROR, "Syntax error near \"%s\": empty setting", str);

	/* OK, at beginning of non-empty item */
	*tok = str;

	/* Advance to end of word */
	while (*str && !isspace((unsigned char) *str) &&
		   *str != ',' && *str != '=')
		str++;

	/* Terminate word string for caller */
	ch = *str;
	*str = '\0';

	/* Skip any whitespace */
	while (isspace((unsigned char) ch))
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
	while (isspace((unsigned char) *str))
		str++;

	if (*str == ',' || *str == '\0')
		elog(ERROR, "Syntax error near \"=%s\"", str);

	/* OK, at beginning of non-empty value */
	*val = str;

	/* Advance to end of word */
	while (*str && !isspace((unsigned char) *str) && *str != ',')
		str++;

	/* Terminate word string for caller */
	ch = *str;
	*str = '\0';

	/* Skip any whitespace */
	while (isspace((unsigned char) ch))
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
 * DATESTYLE
 *
 * NOTE: set_default_datestyle() is called during backend startup to check
 * if the PGDATESTYLE environment variable is set.	We want the env var
 * to determine the value that "RESET DateStyle" will reset to!
 */

/* These get initialized from the "master" values in init/globals.c */
static int	DefaultDateStyle;
static bool DefaultEuroDates;

static bool
parse_datestyle_internal(char *value)
{
	char	   *tok;
	int			dcnt = 0,
				ecnt = 0;

	if (value == NULL)
		return reset_datestyle();

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
		else if (!strncasecmp(tok, "POSTGRESQL", 8))
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
parse_datestyle(List *args)
{
	char	   *value;

	if (args == NULL)
		return reset_datestyle();

	Assert(IsA(lfirst(args), A_Const));

	value = ((A_Const *) lfirst(args))->val.val.str;

	return parse_datestyle_internal(value);
}

static bool
show_datestyle(void)
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
reset_datestyle(void)
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

	/*
	 * Parse desired setting into DateStyle/EuroDates Use
	 * parse_datestyle_internal() to avoid any palloc() issues per above -
	 * thomas 2001-10-15
	 */
	parse_datestyle_internal(DBDate);

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
 * Accept an explicit interval per SQL9x, though this is less useful than a full time zone.
 * - thomas 2001-10-11
 */
static bool
parse_timezone(List *args)
{
	List	   *arg;
	TypeName   *type;

	if (args == NULL)
		return reset_timezone();

	Assert(IsA(args, List));

	foreach(arg, args)
	{
		A_Const    *p;

		Assert(IsA(arg, List));
		p = lfirst(arg);
		Assert(IsA(p, A_Const));

		type = p->typename;
		if (type != NULL)
		{
			if (strcmp(type->name, "interval") == 0)
			{
				Interval   *interval;

				interval = DatumGetIntervalP(DirectFunctionCall3(interval_in,
																 CStringGetDatum(p->val.val.str),
																 ObjectIdGetDatum(InvalidOid),
																 Int32GetDatum(type->typmod)));
				if (interval->month != 0)
					elog(ERROR, "SET TIME ZONE illegal INTERVAL; month not allowed");
				CTimeZone = interval->time;
			}
			else if (strcmp(type->name, "float8") == 0)
			{
				float8		time;

				time = DatumGetFloat8(DirectFunctionCall1(float8in, CStringGetDatum(p->val.val.str)));
				CTimeZone = time * 3600;
			}

			/*
			 * We do not actually generate an integer constant in gram.y
			 * so this is not used...
			 */
			else if (strcmp(type->name, "int4") == 0)
			{
				int32		time;

				time = p->val.val.ival;
				CTimeZone = time * 3600;
			}
			else
			{
				elog(ERROR, "Unable to process SET TIME ZONE command; internal coding error");
			}

			HasCTZSet = true;
		}
		else
		{
			char	   *tok;
			char	   *value;

			value = p->val.val.str;

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

			HasCTZSet = false;
		}
	}

	return TRUE;
}	/* parse_timezone() */

static bool
show_timezone(void)
{
	char	   *tzn;

	if (HasCTZSet)
	{
		Interval	interval;

		interval.month = 0;
		interval.time = CTimeZone;

		tzn = DatumGetCString(DirectFunctionCall1(interval_out, IntervalPGetDatum(&interval)));
	}
	else
		tzn = getenv("TZ");

	if (tzn != NULL)
		elog(NOTICE, "Time zone is '%s'", tzn);
	else
		elog(NOTICE, "Time zone is unset");

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
reset_timezone(void)
{
	if (HasCTZSet)
		HasCTZSet = false;

	/* no time zone has been set in this session? */
	else if (defaultTZ == NULL)
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



/*
 *
 * SET TRANSACTION
 *
 */

static bool
parse_XactIsoLevel(List *args)
{
	char	   *value;

	if (args == NULL)
		return reset_XactIsoLevel();

	Assert(IsA(lfirst(args), A_Const));

	value = ((A_Const *) lfirst(args))->val.val.str;

	if (SerializableSnapshot != NULL)
	{
		elog(ERROR, "SET TRANSACTION ISOLATION LEVEL must be called before any query");
		return TRUE;
	}

	if (strcmp(value, "serializable") == 0)
		XactIsoLevel = XACT_SERIALIZABLE;
	else if (strcmp(value, "read committed") == 0)
		XactIsoLevel = XACT_READ_COMMITTED;
	else
		elog(ERROR, "invalid transaction isolation level: %s", value);

	return TRUE;
}

static bool
show_XactIsoLevel(void)
{

	if (XactIsoLevel == XACT_SERIALIZABLE)
		elog(NOTICE, "TRANSACTION ISOLATION LEVEL is SERIALIZABLE");
	else
		elog(NOTICE, "TRANSACTION ISOLATION LEVEL is READ COMMITTED");
	return TRUE;
}

static bool
reset_XactIsoLevel(void)
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
parse_random_seed(List *args)
{
	char	   *value;
	double		seed = 0;

	if (args == NULL)
		return reset_random_seed();

	Assert(IsA(lfirst(args), A_Const));

	value = ((A_Const *) lfirst(args))->val.val.str;

	sscanf(value, "%lf", &seed);
	DirectFunctionCall1(setseed, Float8GetDatum(seed));

	return (TRUE);
}

static bool
show_random_seed(void)
{
	elog(NOTICE, "Seed for random number generator is unavailable");
	return (TRUE);
}

static bool
reset_random_seed(void)
{
	double		seed = 0.5;

	DirectFunctionCall1(setseed, Float8GetDatum(seed));
	return (TRUE);
}


/*
 * MULTIBYTE-related functions
 *
 * If MULTIBYTE support was not compiled, we still allow these variables
 * to exist, but you can't set them to anything but "SQL_ASCII".  This
 * minimizes interoperability problems between non-MB servers and MB-enabled
 * clients.
 */

static bool
parse_client_encoding(List *args)
{
	char	   *value;

#ifdef MULTIBYTE
	int			encoding;
#endif

	if (args == NULL)
		return reset_client_encoding();

	Assert(IsA(lfirst(args), A_Const));

	value = ((A_Const *) lfirst(args))->val.val.str;

#ifdef MULTIBYTE
	encoding = pg_valid_client_encoding(value);
	if (encoding < 0)
	{
		if (value)
			elog(ERROR, "Client encoding '%s' is not supported", value);
		else
			elog(ERROR, "No client encoding is specified");
	}
	else
	{
		if (pg_set_client_encoding(encoding) < 0)
		{
			elog(ERROR, "Conversion between %s and %s is not supported",
				 value, GetDatabaseEncodingName());
		}
	}
#else
	if (value &&
		strcasecmp(value, pg_get_client_encoding_name()) != 0)
		elog(ERROR, "Client encoding %s is not supported", value);
#endif
	return TRUE;
}

static bool
show_client_encoding(void)
{
	elog(NOTICE, "Current client encoding is '%s'",
		 pg_get_client_encoding_name());
	return TRUE;
}

static bool
reset_client_encoding(void)
{
#ifdef MULTIBYTE
	int			encoding;
	char	   *env = getenv("PGCLIENTENCODING");

	if (env)
	{
		encoding = pg_char_to_encoding(env);
		if (encoding < 0)
			encoding = GetDatabaseEncoding();
	}
	else
		encoding = GetDatabaseEncoding();

	pg_set_client_encoding(encoding);
#endif
	return TRUE;
}

/* Called during MULTIBYTE backend startup ... */
void
set_default_client_encoding(void)
{
	reset_client_encoding();
}


static bool
parse_server_encoding(List *args)
{
	elog(NOTICE, "SET SERVER_ENCODING is not supported");
	return TRUE;
}

static bool
show_server_encoding(void)
{
	elog(NOTICE, "Current server encoding is '%s'", GetDatabaseEncodingName());
	return TRUE;
}

static bool
reset_server_encoding(void)
{
	elog(NOTICE, "RESET SERVER_ENCODING is not supported");
	return TRUE;
}



/* SetPGVariable()
 * Dispatcher for handling SET commands.
 * Special cases ought to be removed and handled separately by TCOP
 */
void
SetPGVariable(const char *name, List *args)
{
	if (strcasecmp(name, "datestyle") == 0)
		parse_datestyle(args);
	else if (strcasecmp(name, "timezone") == 0)
		parse_timezone(args);
	else if (strcasecmp(name, "XactIsoLevel") == 0)
		parse_XactIsoLevel(args);
	else if (strcasecmp(name, "client_encoding") == 0)
		parse_client_encoding(args);
	else if (strcasecmp(name, "server_encoding") == 0)
		parse_server_encoding(args);
	else if (strcasecmp(name, "seed") == 0)
		parse_random_seed(args);
	else
	{
		/*
		 * For routines defined somewhere else, go ahead and extract the
		 * string argument to match the original interface definition.
		 * Later, we can change this code too...
		 */
		char	   *value;

		value = ((args != NULL) ? ((A_Const *) lfirst(args))->val.val.str : NULL);

		if (strcasecmp(name, "session_authorization") == 0)
			SetSessionAuthorization(value);
		else
			SetConfigOption(name, value, superuser() ? PGC_SUSET : PGC_USERSET, PGC_S_SESSION);
	}
	return;
}

void
GetPGVariable(const char *name)
{
	if (strcasecmp(name, "datestyle") == 0)
		show_datestyle();
	else if (strcasecmp(name, "timezone") == 0)
		show_timezone();
	else if (strcasecmp(name, "XactIsoLevel") == 0)
		show_XactIsoLevel();
	else if (strcasecmp(name, "client_encoding") == 0)
		show_client_encoding();
	else if (strcasecmp(name, "server_encoding") == 0)
		show_server_encoding();
	else if (strcasecmp(name, "seed") == 0)
		show_random_seed();
	else if (strcasecmp(name, "all") == 0)
	{
		ShowAllGUCConfig();
		show_datestyle();
		show_timezone();
		show_XactIsoLevel();
		show_client_encoding();
		show_server_encoding();
		show_random_seed();
	}
	else
	{
		const char *val = GetConfigOption(name);

		elog(NOTICE, "%s is %s", name, val);
	}
}

void
ResetPGVariable(const char *name)
{
	if (strcasecmp(name, "datestyle") == 0)
		reset_datestyle();
	else if (strcasecmp(name, "timezone") == 0)
		reset_timezone();
	else if (strcasecmp(name, "XactIsoLevel") == 0)
		reset_XactIsoLevel();
	else if (strcasecmp(name, "client_encoding") == 0)
		reset_client_encoding();
	else if (strcasecmp(name, "server_encoding") == 0)
		reset_server_encoding();
	else if (strcasecmp(name, "seed") == 0)
		reset_random_seed();
	else if (strcasecmp(name, "all") == 0)
	{
		reset_random_seed();
		/* reset_server_encoding(); */
		reset_client_encoding();
		reset_datestyle();
		reset_timezone();

		ResetAllOptions(false);
	}
	else
		SetConfigOption(name, NULL,
						superuser() ? PGC_SUSET : PGC_USERSET,
						PGC_S_SESSION);
}
