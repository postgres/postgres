/*
 * Routines for handling of 'SET var TO',
 *	'SHOW var' and 'RESET var' statements.
 *
 * $Id: variable.c,v 1.6 1998/06/15 19:28:17 momjian Exp $
 *
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "postgres.h"
#include "miscadmin.h"
#include "commands/variable.h"
#include "utils/builtins.h"
#include "optimizer/internal.h"

extern Cost _cpu_page_wight_;
extern Cost _cpu_index_page_wight_;
extern bool _use_geqo_;
extern int32 _use_geqo_rels_;
extern bool _use_right_sided_plans_;

/*-----------------------------------------------------------------------*/
static const char *
get_token(char **tok, char **val, const char *str)
{
	const char *start;
	int			len = 0;

	*tok = NULL;
	if (val != NULL)
		*val = NULL;

	if (!(*str))
		return NULL;

	/* skip white spaces */
	while (isspace(*str))
		str++;
	if (*str == ',' || *str == '=')
		elog(ERROR, "Syntax error near (%s): empty setting", str);

	/* end of string? then return NULL */
	if (!(*str))
		return NULL;

	/* OK, at beginning of non-NULL string... */
	start = str;

	/*
	 * count chars in token until we hit white space or comma or '=' or
	 * end of string
	 */
	while (*str && (!isspace(*str))
		   && *str != ',' && *str != '=')
	{
		str++;
		len++;
	}

	*tok = (char *) palloc(len + 1);
	StrNCpy(*tok, start, len + 1);

	/* skip white spaces */
	while (isspace(*str))
		str++;

	/* end of string? */
	if (!(*str))
	{
		return (str);

		/* delimiter? */
	}
	else if (*str == ',')
	{
		return (++str);

	}
	else if ((val == NULL) || (*str != '='))
	{
		elog(ERROR, "Syntax error near (%s)", str);
	};

	str++;						/* '=': get value */
	len = 0;

	/* skip white spaces */
	while (isspace(*str))
		str++;

	if (*str == ',' || !(*str))
		elog(ERROR, "Syntax error near (=%s)", str);

	start = str;

	/*
	 * count chars in token's value until we hit white space or comma or
	 * end of string
	 */
	while (*str && (!isspace(*str)) && *str != ',')
	{
		str++;
		len++;
	}

	*val = (char *) palloc(len + 1);
	StrNCpy(*val, start, len + 1);

	/* skip white spaces */
	while (isspace(*str))
		str++;

	if (!(*str))
		return (NULL);
	if (*str == ',')
		return (++str);

	elog(ERROR, "Syntax error near (%s)", str);

	return str;
}

/*-----------------------------------------------------------------------*/
bool
parse_geqo(const char *value)
{
	const char *rest;
	char	   *tok,
			   *val;

	if (value == NULL)
	{
		reset_geqo();
		return TRUE;
	}

	rest = get_token(&tok, &val, value);
	if (tok == NULL)
		elog(ERROR, "Value undefined");

	if ((rest) && (*rest != '\0'))
		elog(ERROR, "Unable to parse '%s'", value);

	if (strcasecmp(tok, "on") == 0)
	{
		int32		geqo_rels = GEQO_RELS;

		if (val != NULL)
		{
			geqo_rels = pg_atoi(val, sizeof(int32), '\0');
			if (geqo_rels <= 1)
				elog(ERROR, "Bad value for # of relations (%s)", val);
			pfree(val);
		}
		_use_geqo_ = true;
		_use_geqo_rels_ = geqo_rels;
	}
	else if (strcasecmp(tok, "off") == 0)
	{
		if ((val != NULL) && (*val != '\0'))
			elog(ERROR, "%s does not allow a parameter", tok);
		_use_geqo_ = false;
	}
	else
		elog(ERROR, "Bad value for GEQO (%s)", value);

	pfree(tok);
	return TRUE;
}

bool
show_geqo()
{

	if (_use_geqo_)
		elog(NOTICE, "GEQO is ON beginning with %d relations", _use_geqo_rels_);
	else
		elog(NOTICE, "GEQO is OFF");
	return TRUE;
}

bool
reset_geqo(void)
{

#ifdef GEQO
	_use_geqo_ = true;
#else
	_use_geqo_ = false;
#endif
	_use_geqo_rels_ = GEQO_RELS;
	return TRUE;
}

bool
parse_r_plans(const char *value)
{
	if (value == NULL)
	{
		reset_r_plans();
		return TRUE;
	}

	if (strcasecmp(value, "on") == 0)
		_use_right_sided_plans_ = true;
	else if (strcasecmp(value, "off") == 0)
		_use_right_sided_plans_ = false;
	else
		elog(ERROR, "Bad value for Right-sided Plans (%s)", value);

	return TRUE;
}

/*-----------------------------------------------------------------------*/
bool
show_r_plans()
{

	if (_use_right_sided_plans_)
		elog(NOTICE, "Right-sided Plans are ON");
	else
		elog(NOTICE, "Right-sided Plans are OFF");
	return TRUE;
}

bool
reset_r_plans()
{

#ifdef USE_RIGHT_SIDED_PLANS
	_use_right_sided_plans_ = true;
#else
	_use_right_sided_plans_ = false;
#endif
	return TRUE;
}

/*-----------------------------------------------------------------------*/
bool
parse_cost_heap(const char *value)
{
	float32		res;

	if (value == NULL)
	{
		reset_cost_heap();
		return TRUE;
	}

	res = float4in((char *) value);
	_cpu_page_wight_ = *res;

	return TRUE;
}

bool
show_cost_heap()
{

	elog(NOTICE, "COST_HEAP is %f", _cpu_page_wight_);
	return TRUE;
}

bool
reset_cost_heap()
{
	_cpu_page_wight_ = _CPU_PAGE_WEIGHT_;
	return TRUE;
}

/*-----------------------------------------------------------------------*/
bool
parse_cost_index(const char *value)
{
	float32		res;

	if (value == NULL)
	{
		reset_cost_index();
		return TRUE;
	}

	res = float4in((char *) value);
	_cpu_index_page_wight_ = *res;

	return TRUE;
}

bool
show_cost_index()
{

	elog(NOTICE, "COST_INDEX is %f", _cpu_index_page_wight_);
	return TRUE;
}

bool
reset_cost_index()
{
	_cpu_index_page_wight_ = _CPU_INDEX_PAGE_WEIGHT_;
	return TRUE;
}

/*-----------------------------------------------------------------------*/
bool
parse_date(const char *value)
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
			DateStyle = USE_POSTGRES_DATES;
			EuroDates = FALSE;
			ecnt++;
		}
		else
			elog(ERROR, "Bad value for date style (%s)", tok);
		pfree(tok);
	}

	if (dcnt > 1 || ecnt > 1)
		elog(NOTICE, "Conflicting settings for date");

	return TRUE;
}

bool
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

bool
reset_date()
{
	DateStyle = USE_POSTGRES_DATES;
	EuroDates = FALSE;

	return TRUE;
}

/* Timezone support
 * Working storage for strings is allocated with an arbitrary size of 64 bytes.
 */

static char *defaultTZ = NULL;
static char TZvalue[64];
static char tzbuf[64];

/* parse_timezone()
 * Handle SET TIME ZONE...
 * Try to save existing TZ environment variable for later use in RESET TIME ZONE.
 * - thomas 1997-11-10
 */
bool
parse_timezone(const char *value)
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
		pfree(tok);
	}

	return TRUE;
}	/* parse_timezone() */

bool
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
bool
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
			elog(ERROR, "Unable to clear TZ environment variable", NULL);
		tzset();
	}

	return TRUE;
}	/* reset_timezone() */

/*-----------------------------------------------------------------------*/
struct VariableParsers
{
	const char *name;
	bool		(*parser) (const char *);
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
		"cost_heap", parse_cost_heap, show_cost_heap, reset_cost_heap
	},
	{
		"cost_index", parse_cost_index, show_cost_index, reset_cost_index
	},
	{
		"geqo", parse_geqo, show_geqo, reset_geqo
	},
	{
		"r_plans", parse_r_plans, show_r_plans, reset_r_plans
	},
	{
		NULL, NULL, NULL, NULL
	}
};

/*-----------------------------------------------------------------------*/
bool
SetPGVariable(const char *name, const char *value)
{
	struct VariableParsers *vp;

	for (vp = VariableParsers; vp->name; vp++)
	{
		if (!strcasecmp(vp->name, name))
			return (vp->parser) (value);
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
