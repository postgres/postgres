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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/variable.c,v 1.32 2000/03/17 05:29:04 tgl Exp $
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
#include "utils/tqual.h"
#include "utils/trace.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif


/* XXX should be in a header file */
extern bool _use_keyset_query_optimizer;


static bool show_date(void);
static bool reset_date(void);
static bool parse_date(char *);
static bool show_timezone(void);
static bool reset_timezone(void);
static bool parse_timezone(char *);
static bool show_effective_cache_size(void);
static bool reset_effective_cache_size(void);
static bool parse_effective_cache_size(char *);
static bool show_random_page_cost(void);
static bool reset_random_page_cost(void);
static bool parse_random_page_cost(char *);
static bool show_cpu_tuple_cost(void);
static bool reset_cpu_tuple_cost(void);
static bool parse_cpu_tuple_cost(char *);
static bool show_cpu_index_tuple_cost(void);
static bool reset_cpu_index_tuple_cost(void);
static bool parse_cpu_index_tuple_cost(char *);
static bool show_cpu_operator_cost(void);
static bool reset_cpu_operator_cost(void);
static bool parse_cpu_operator_cost(char *);
static bool reset_enable_seqscan(void);
static bool show_enable_seqscan(void);
static bool parse_enable_seqscan(char *);
static bool reset_enable_indexscan(void);
static bool show_enable_indexscan(void);
static bool parse_enable_indexscan(char *);
static bool reset_enable_tidscan(void);
static bool show_enable_tidscan(void);
static bool parse_enable_tidscan(char *);
static bool reset_enable_sort(void);
static bool show_enable_sort(void);
static bool parse_enable_sort(char *);
static bool reset_enable_nestloop(void);
static bool show_enable_nestloop(void);
static bool parse_enable_nestloop(char *);
static bool reset_enable_mergejoin(void);
static bool show_enable_mergejoin(void);
static bool parse_enable_mergejoin(char *);
static bool reset_enable_hashjoin(void);
static bool show_enable_hashjoin(void);
static bool parse_enable_hashjoin(char *);
static bool reset_geqo(void);
static bool show_geqo(void);
static bool parse_geqo(char *);
static bool show_ksqo(void);
static bool reset_ksqo(void);
static bool parse_ksqo(char *);
static bool reset_max_expr_depth(void);
static bool show_max_expr_depth(void);
static bool parse_max_expr_depth(char *);
static bool show_XactIsoLevel(void);
static bool reset_XactIsoLevel(void);
static bool parse_XactIsoLevel(char *);

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
	while (isspace(*str))
		str++;

	/* end of string? then return NULL */
	if (*str == '\0')
		return NULL;

	if (*str == ',' || *str == '=')
		elog(ERROR, "Syntax error near \"%s\": empty setting", str);

	/* OK, at beginning of non-empty item */
	*tok = str;

	/* Advance to end of word */
	while (*str && !isspace(*str) && *str != ',' && *str != '=')
		str++;

	/* Terminate word string for caller */
	ch = *str;
	*str = '\0';

	/* Skip any whitespace */
	while (isspace(ch))
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
	while (isspace(*str))
		str++;

	if (*str == ',' || *str == '\0')
		elog(ERROR, "Syntax error near \"=%s\"", str);

	/* OK, at beginning of non-empty value */
	*val = str;

	/* Advance to end of word */
	while (*str && !isspace(*str) && *str != ',')
		str++;

	/* Terminate word string for caller */
	ch = *str;
	*str = '\0';

	/* Skip any whitespace */
	while (isspace(ch))
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
 * Generic parse routine for boolean ON/OFF variables
 */
static bool
parse_boolean_var(char *value,
				  bool *variable, const char *varname, bool defaultval)
{
	if (value == NULL)
	{
		*variable = defaultval;
		return TRUE;
	}

	if (strcasecmp(value, "on") == 0)
		*variable = true;
	else if (strcasecmp(value, "off") == 0)
		*variable = false;
	else
		elog(ERROR, "Bad value for %s (%s)", varname, value);

	return TRUE;
}

/*
 * ENABLE_SEQSCAN
 */
static bool
parse_enable_seqscan(char *value)
{
	return parse_boolean_var(value, &enable_seqscan,
							 "ENABLE_SEQSCAN", true);
}

static bool
show_enable_seqscan()
{
	elog(NOTICE, "ENABLE_SEQSCAN is %s",
		 enable_seqscan ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_enable_seqscan()
{
	enable_seqscan = true;
	return TRUE;
}

/*
 * ENABLE_INDEXSCAN
 */
static bool
parse_enable_indexscan(char *value)
{
	return parse_boolean_var(value, &enable_indexscan,
							 "ENABLE_INDEXSCAN", true);
}

static bool
show_enable_indexscan()
{
	elog(NOTICE, "ENABLE_INDEXSCAN is %s",
		 enable_indexscan ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_enable_indexscan()
{
	enable_indexscan = true;
	return TRUE;
}

/*
 * ENABLE_TIDSCAN
 */
static bool
parse_enable_tidscan(char *value)
{
	return parse_boolean_var(value, &enable_tidscan,
							 "ENABLE_TIDSCAN", true);
}

static bool
show_enable_tidscan()
{
	elog(NOTICE, "ENABLE_TIDSCAN is %s",
		 enable_tidscan ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_enable_tidscan()
{
	enable_tidscan = true;
	return TRUE;
}

/*
 * ENABLE_SORT
 */
static bool
parse_enable_sort(char *value)
{
	return parse_boolean_var(value, &enable_sort,
							 "ENABLE_SORT", true);
}

static bool
show_enable_sort()
{
	elog(NOTICE, "ENABLE_SORT is %s",
		 enable_sort ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_enable_sort()
{
	enable_sort = true;
	return TRUE;
}

/*
 * ENABLE_NESTLOOP
 */
static bool
parse_enable_nestloop(char *value)
{
	return parse_boolean_var(value, &enable_nestloop,
							 "ENABLE_NESTLOOP", true);
}

static bool
show_enable_nestloop()
{
	elog(NOTICE, "ENABLE_NESTLOOP is %s",
		 enable_nestloop ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_enable_nestloop()
{
	enable_nestloop = true;
	return TRUE;
}

/*
 * ENABLE_MERGEJOIN
 */
static bool
parse_enable_mergejoin(char *value)
{
	return parse_boolean_var(value, &enable_mergejoin,
							 "ENABLE_MERGEJOIN", true);
}

static bool
show_enable_mergejoin()
{
	elog(NOTICE, "ENABLE_MERGEJOIN is %s",
		 enable_mergejoin ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_enable_mergejoin()
{
	enable_mergejoin = true;
	return TRUE;
}

/*
 * ENABLE_HASHJOIN
 */
static bool
parse_enable_hashjoin(char *value)
{
	return parse_boolean_var(value, &enable_hashjoin,
							 "ENABLE_HASHJOIN", true);
}

static bool
show_enable_hashjoin()
{
	elog(NOTICE, "ENABLE_HASHJOIN is %s",
		 enable_hashjoin ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_enable_hashjoin()
{
	enable_hashjoin = true;
	return TRUE;
}

/*
 *
 * GEQO
 *
 */
static bool
parse_geqo(char *value)
{
	char	   *tok,
			   *val,
			   *rest;

	if (value == NULL)
	{
		reset_geqo();
		return TRUE;
	}

	rest = get_token(&tok, &val, value);

	/* expect one and only one item */
	if (tok == NULL)
		elog(ERROR, "Value undefined");
	if (rest && *rest != '\0')
		elog(ERROR, "Unable to parse '%s'", rest);

	if (strcasecmp(tok, "on") == 0)
	{
		int		new_geqo_rels = GEQO_RELS;

		if (val != NULL)
		{
			new_geqo_rels = pg_atoi(val, sizeof(int), '\0');
			if (new_geqo_rels <= 1)
				elog(ERROR, "Bad value for # of relations (%s)", val);
		}
		enable_geqo = true;
		geqo_rels = new_geqo_rels;
	}
	else if (strcasecmp(tok, "off") == 0)
	{
		if (val != NULL)
			elog(ERROR, "%s does not allow a parameter", tok);
		enable_geqo = false;
	}
	else
		elog(ERROR, "Bad value for GEQO (%s)", value);

	return TRUE;
}

static bool
show_geqo()
{
	if (enable_geqo)
		elog(NOTICE, "GEQO is ON beginning with %d relations", geqo_rels);
	else
		elog(NOTICE, "GEQO is OFF");
	return TRUE;
}

static bool
reset_geqo(void)
{
#ifdef GEQO
	enable_geqo = true;
#else
	enable_geqo = false;
#endif
	geqo_rels = GEQO_RELS;
	return TRUE;
}

/*
 * EFFECTIVE_CACHE_SIZE
 */
static bool
parse_effective_cache_size(char *value)
{
	float64		res;

	if (value == NULL)
	{
		reset_effective_cache_size();
		return TRUE;
	}

	res = float8in((char *) value);
	effective_cache_size = *res;

	return TRUE;
}

static bool
show_effective_cache_size()
{
	elog(NOTICE, "EFFECTIVE_CACHE_SIZE is %g (%dK pages)",
		 effective_cache_size, BLCKSZ/1024);
	return TRUE;
}

static bool
reset_effective_cache_size()
{
	effective_cache_size = DEFAULT_EFFECTIVE_CACHE_SIZE;
	return TRUE;
}

/*
 * RANDOM_PAGE_COST
 */
static bool
parse_random_page_cost(char *value)
{
	float64		res;

	if (value == NULL)
	{
		reset_random_page_cost();
		return TRUE;
	}

	res = float8in((char *) value);
	random_page_cost = *res;

	return TRUE;
}

static bool
show_random_page_cost()
{
	elog(NOTICE, "RANDOM_PAGE_COST is %g", random_page_cost);
	return TRUE;
}

static bool
reset_random_page_cost()
{
	random_page_cost = DEFAULT_RANDOM_PAGE_COST;
	return TRUE;
}

/*
 * CPU_TUPLE_COST
 */
static bool
parse_cpu_tuple_cost(char *value)
{
	float64		res;

	if (value == NULL)
	{
		reset_cpu_tuple_cost();
		return TRUE;
	}

	res = float8in((char *) value);
	cpu_tuple_cost = *res;

	return TRUE;
}

static bool
show_cpu_tuple_cost()
{
	elog(NOTICE, "CPU_TUPLE_COST is %g", cpu_tuple_cost);
	return TRUE;
}

static bool
reset_cpu_tuple_cost()
{
	cpu_tuple_cost = DEFAULT_CPU_TUPLE_COST;
	return TRUE;
}

/*
 * CPU_INDEX_TUPLE_COST
 */
static bool
parse_cpu_index_tuple_cost(char *value)
{
	float64		res;

	if (value == NULL)
	{
		reset_cpu_index_tuple_cost();
		return TRUE;
	}

	res = float8in((char *) value);
	cpu_index_tuple_cost = *res;

	return TRUE;
}

static bool
show_cpu_index_tuple_cost()
{
	elog(NOTICE, "CPU_INDEX_TUPLE_COST is %g", cpu_index_tuple_cost);
	return TRUE;
}

static bool
reset_cpu_index_tuple_cost()
{
	cpu_index_tuple_cost = DEFAULT_CPU_INDEX_TUPLE_COST;
	return TRUE;
}

/*
 * CPU_OPERATOR_COST
 */
static bool
parse_cpu_operator_cost(char *value)
{
	float64		res;

	if (value == NULL)
	{
		reset_cpu_operator_cost();
		return TRUE;
	}

	res = float8in((char *) value);
	cpu_operator_cost = *res;

	return TRUE;
}

static bool
show_cpu_operator_cost()
{
	elog(NOTICE, "CPU_OPERATOR_COST is %g", cpu_operator_cost);
	return TRUE;
}

static bool
reset_cpu_operator_cost()
{
	cpu_operator_cost = DEFAULT_CPU_OPERATOR_COST;
	return TRUE;
}

/*
 * DATE_STYLE
 *
 * NOTE: set_default_datestyle() is called during backend startup to check
 * if the PGDATESTYLE environment variable is set.  We want the env var
 * to determine the value that "RESET DateStyle" will reset to!
 */

/* These get initialized from the "master" values in init/globals.c */
static int DefaultDateStyle;
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

	/* Initialize from compile-time defaults in init/globals.c.
	 * NB: this is a necessary step; consider PGDATESTYLE="DEFAULT".
	 */
	DefaultDateStyle = DateStyle;
	DefaultEuroDates = EuroDates;

	/* If the environment var is set, override compiled-in values */
	DBDate = getenv("PGDATESTYLE");
	if (DBDate == NULL)
		return;

	/* Make a modifiable copy --- overwriting the env var doesn't seem
	 * like a good idea, even though we currently won't look at it again.
	 * Note that we cannot use palloc at this early stage of initialization.
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

/*-----------------------------------------------------------------------
KSQO code will one day be unnecessary when the optimizer makes use of
indexes when multiple ORs are specified in the where clause.
See optimizer/prep/prepkeyset.c for more on this.
	daveh@insightdist.com	 6/16/98
-----------------------------------------------------------------------*/
static bool
parse_ksqo(char *value)
{
	return parse_boolean_var(value, &_use_keyset_query_optimizer,
							 "KSQO", false);
}

static bool
show_ksqo()
{
	elog(NOTICE, "KSQO is %s",
		 _use_keyset_query_optimizer ? "ON" : "OFF");
	return TRUE;
}

static bool
reset_ksqo()
{
	_use_keyset_query_optimizer = false;
	return TRUE;
}

/*
 * MAX_EXPR_DEPTH
 */
static bool
parse_max_expr_depth(char *value)
{
	int			newval;

	if (value == NULL)
	{
		reset_max_expr_depth();
		return TRUE;
	}

	newval = pg_atoi(value, sizeof(int), '\0');

	if (newval < 10)			/* somewhat arbitrary limit */
		elog(ERROR, "Bad value for MAX_EXPR_DEPTH (%s)", value);

	max_expr_depth = newval;

	return TRUE;
}

static bool
show_max_expr_depth()
{
	elog(NOTICE, "MAX_EXPR_DEPTH is %d", max_expr_depth);
	return TRUE;
}

static bool
reset_max_expr_depth(void)
{
	max_expr_depth = DEFAULT_MAX_EXPR_DEPTH;
	return TRUE;
}

/* SET TRANSACTION */

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
 * Pg_options
 */
static bool
parse_pg_options(char *value)
{
	if (!superuser()) {
		elog(ERROR, "Only users with superuser privilege can set pg_options");
	}
	if (value == NULL)
		read_pg_options(0);
	else
		parse_options((char *) value, TRUE);
	return (TRUE);
}

static bool
show_pg_options(void)
{
	show_options();
	return (TRUE);
}

static bool
reset_pg_options(void)
{
	if (!superuser()) {
		elog(ERROR, "Only users with superuser privilege can set pg_options");
	}
	read_pg_options(0);
	return (TRUE);
}


/*-----------------------------------------------------------------------*/

static struct VariableParsers
{
	const char *name;
	bool		(*parser) (char *);
	bool		(*show) ();
	bool		(*reset) ();
}			VariableParsers[] =

{
	{
		"datestyle", parse_date, show_date, reset_date
	},
	{
		"timezone", parse_timezone, show_timezone, reset_timezone
	},
	{
		"effective_cache_size", parse_effective_cache_size,
		show_effective_cache_size, reset_effective_cache_size
	},
	{
		"random_page_cost", parse_random_page_cost,
		show_random_page_cost, reset_random_page_cost
	},
	{
		"cpu_tuple_cost", parse_cpu_tuple_cost,
		show_cpu_tuple_cost, reset_cpu_tuple_cost
	},
	{
		"cpu_index_tuple_cost", parse_cpu_index_tuple_cost,
		show_cpu_index_tuple_cost, reset_cpu_index_tuple_cost
	},
	{
		"cpu_operator_cost", parse_cpu_operator_cost,
		show_cpu_operator_cost, reset_cpu_operator_cost
	},
	{
		"enable_seqscan", parse_enable_seqscan,
		show_enable_seqscan, reset_enable_seqscan
	},
	{
		"enable_indexscan", parse_enable_indexscan,
		show_enable_indexscan, reset_enable_indexscan
	},
	{
		"enable_tidscan", parse_enable_tidscan,
		show_enable_tidscan, reset_enable_tidscan
	},
	{
		"enable_sort", parse_enable_sort,
		show_enable_sort, reset_enable_sort
	},
	{
		"enable_nestloop", parse_enable_nestloop,
		show_enable_nestloop, reset_enable_nestloop
	},
	{
		"enable_mergejoin", parse_enable_mergejoin,
		show_enable_mergejoin, reset_enable_mergejoin
	},
	{
		"enable_hashjoin", parse_enable_hashjoin,
		show_enable_hashjoin, reset_enable_hashjoin
	},
	{
		"geqo", parse_geqo, show_geqo, reset_geqo
	},
#ifdef MULTIBYTE
	{
		"client_encoding", parse_client_encoding, show_client_encoding, reset_client_encoding
	},
	{
		"server_encoding", parse_server_encoding, show_server_encoding, reset_server_encoding
	},
#endif
	{
		"ksqo", parse_ksqo, show_ksqo, reset_ksqo
	},
	{
		"max_expr_depth", parse_max_expr_depth,
		show_max_expr_depth, reset_max_expr_depth
	},
	{
		"XactIsoLevel", parse_XactIsoLevel, show_XactIsoLevel, reset_XactIsoLevel
	},
	{
		"pg_options", parse_pg_options, show_pg_options, reset_pg_options
	},
	{
		NULL, NULL, NULL, NULL
	}
};

/*-----------------------------------------------------------------------*/
/*
 * Set the named variable, or reset to default value if value is NULL
 */
bool
SetPGVariable(const char *name, const char *value)
{
	struct VariableParsers *vp;
	char	   *val;

	/* Make a modifiable copy for convenience of get_token */
	val = value ? pstrdup(value) : ((char *) NULL);

	for (vp = VariableParsers; vp->name; vp++)
	{
		if (!strcasecmp(vp->name, name))
			return (vp->parser) (val);
	}

	elog(NOTICE, "Unrecognized variable %s", name);

	return TRUE;
}

/*-----------------------------------------------------------------------*/
bool
GetPGVariable(const char *name)
{
	struct VariableParsers *vp;

	for (vp = VariableParsers; vp->name; vp++)
	{
		if (!strcasecmp(vp->name, name))
			return (vp->show) ();
	}

	elog(NOTICE, "Unrecognized variable %s", name);

	return TRUE;
}

/*-----------------------------------------------------------------------*/
bool
ResetPGVariable(const char *name)
{
	struct VariableParsers *vp;

	for (vp = VariableParsers; vp->name; vp++)
	{
		if (!strcasecmp(vp->name, name))
			return (vp->reset) ();
	}

	elog(NOTICE, "Unrecognized variable %s", name);

	return TRUE;
}
