/*
 * Routines for handling of 'SET var TO', 'SHOW var' and 'RESET var'
 * statements.
 *
 * $Id: variable.c,v 1.14 1997/09/07 04:49:37 momjian Exp $
 *
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "postgres.h"
#include "miscadmin.h"
#include "tcop/variable.h"
#include "utils/builtins.h"
#include "optimizer/internal.h"

extern Cost		_cpu_page_wight_;
extern Cost		_cpu_index_page_wight_;
extern bool		_use_geqo_;
extern int32	_use_geqo_rels_;
extern bool		_use_right_sided_plans_;

/*-----------------------------------------------------------------------*/
#if USE_EURODATES
#define DATE_EURO		TRUE
#else
#define DATE_EURO FALSE
#endif

/*-----------------------------------------------------------------------*/
struct PGVariables PGVariables =
{
	{DATE_EURO, Date_Postgres}
};

/*-----------------------------------------------------------------------*/
static const char *
get_token(char **tok, char **val, const char *str)
{
	const char	   *start;
	int				len = 0;

	*tok = NULL;
	if (val != NULL)
		*val = NULL;

	if (!(*str))
		return NULL;

	/* skip white spaces */
	while (isspace(*str))
		str++;
	if (*str == ',' || *str == '=')
		elog(WARN, "Syntax error near (%s): empty setting", str);

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

	*tok = (char *) PALLOC(len + 1);
	strNcpy(*tok, start, len);

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
		elog(WARN, "Syntax error near (%s)", str);
	};

	str++;						/* '=': get value */
	len = 0;

	/* skip white spaces */
	while (isspace(*str))
		str++;

	if (*str == ',' || !(*str))
		elog(WARN, "Syntax error near (=%s)", str);

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

	*val = (char *) PALLOC(len + 1);
	strNcpy(*val, start, len);

	/* skip white spaces */
	while (isspace(*str))
		str++;

	if (!(*str))
		return (NULL);
	if (*str == ',')
		return (++str);

	elog(WARN, "Syntax error near (%s)", str);

	return str;
}

/*-----------------------------------------------------------------------*/
static bool
parse_null(const char *value)
{
	return TRUE;
}

static bool
show_null(const char *value)
{
	return TRUE;
}

static bool
reset_null(const char *value)
{
	return TRUE;
}

static bool
parse_geqo(const char *value)
{
	const char	   *rest;
	char		   *tok,
				   *val;

	rest = get_token(&tok, &val, value);
	if (tok == NULL)
		elog(WARN, "Value undefined");

	if ((rest) && (*rest != '\0'))
		elog(WARN, "Unable to parse '%s'", value);

	if (strcasecmp(tok, "on") == 0)
	{
		int32			geqo_rels = GEQO_RELS;

		if (val != NULL)
		{
			geqo_rels = pg_atoi(val, sizeof(int32), '\0');
			if (geqo_rels <= 1)
				elog(WARN, "Bad value for # of relations (%s)", val);
			PFREE(val);
		}
		_use_geqo_ = true;
		_use_geqo_rels_ = geqo_rels;
	}
	else if (strcasecmp(tok, "off") == 0)
	{
		if ((val != NULL) && (*val != '\0'))
			elog(WARN, "%s does not allow a parameter", tok);
		_use_geqo_ = false;
	}
	else
		elog(WARN, "Bad value for GEQO (%s)", value);

	PFREE(tok);
	return TRUE;
}

static bool
show_geqo()
{

	if (_use_geqo_)
		elog(NOTICE, "GEQO is ON beginning with %d relations", _use_geqo_rels_);
	else
		elog(NOTICE, "GEQO is OFF");
	return TRUE;
}

static bool
reset_geqo()
{

#ifdef GEQO
	_use_geqo_ = true;
#else
	_use_geqo_ = false;
#endif
	_use_geqo_rels_ = GEQO_RELS;
	return TRUE;
}

static bool
parse_r_plans(const char *value)
{

	if (strcasecmp(value, "on") == 0)
		_use_right_sided_plans_ = true;
	else if (strcasecmp(value, "off") == 0)
		_use_right_sided_plans_ = false;
	else
		elog(WARN, "Bad value for Right-sided Plans (%s)", value);

	return TRUE;
}

static bool
show_r_plans()
{

	if (_use_right_sided_plans_)
		elog(NOTICE, "Right-sided Plans are ON");
	else
		elog(NOTICE, "Right-sided Plans are OFF");
	return TRUE;
}

static bool
reset_r_plans()
{

#ifdef USE_RIGHT_SIDED_PLANS
	_use_right_sided_plans_ = true;
#else
	_use_right_sided_plans_ = false;
#endif
	return TRUE;
}

static bool
parse_cost_heap(const char *value)
{
	float32			res = float4in((char *) value);

	_cpu_page_wight_ = *res;

	return TRUE;
}

static bool
show_cost_heap()
{

	elog(NOTICE, "COST_HEAP is %f", _cpu_page_wight_);
	return TRUE;
}

static bool
reset_cost_heap()
{
	_cpu_page_wight_ = _CPU_PAGE_WEIGHT_;
	return TRUE;
}

static bool
parse_cost_index(const char *value)
{
	float32			res = float4in((char *) value);

	_cpu_index_page_wight_ = *res;

	return TRUE;
}

static bool
show_cost_index()
{

	elog(NOTICE, "COST_INDEX is %f", _cpu_index_page_wight_);
	return TRUE;
}

static bool
reset_cost_index()
{
	_cpu_index_page_wight_ = _CPU_INDEX_PAGE_WEIGHT_;
	return TRUE;
}

static bool
parse_date(const char *value)
{
	char		   *tok;
	int				dcnt = 0,
					ecnt = 0;

	while ((value = get_token(&tok, NULL, value)) != 0)
	{
		/* Ugh. Somebody ought to write a table driven version -- mjl */

		if (!strcasecmp(tok, "iso"))
		{
			DateStyle = USE_ISO_DATES;
			dcnt++;
		}
		else if (!strcasecmp(tok, "sql"))
		{
			DateStyle = USE_SQL_DATES;
			dcnt++;
		}
		else if (!strcasecmp(tok, "postgres"))
		{
			DateStyle = USE_POSTGRES_DATES;
			dcnt++;
		}
		else if (!strncasecmp(tok, "euro", 4))
		{
			EuroDates = TRUE;
			ecnt++;
		}
		else if ((!strcasecmp(tok, "us"))
				 || (!strncasecmp(tok, "noneuro", 7)))
		{
			EuroDates = FALSE;
			ecnt++;
		}
		else if (!strcasecmp(tok, "default"))
		{
			DateStyle = USE_POSTGRES_DATES;
			EuroDates = FALSE;
			ecnt++;
		}
		else
		{
			elog(WARN, "Bad value for date style (%s)", tok);
		}
		PFREE(tok);
	}

	if (dcnt > 1 || ecnt > 1)
		elog(NOTICE, "Conflicting settings for date");

	return TRUE;
}

static bool
show_date()
{
	char			buf[64];

	strcpy(buf, "DateStyle is ");
	switch (DateStyle)
	{
	case USE_ISO_DATES:
		strcat(buf, "ISO");
		break;
	case USE_SQL_DATES:
		strcat(buf, "SQL");
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
	DateStyle = USE_POSTGRES_DATES;
	EuroDates = FALSE;

	return TRUE;
}

/*-----------------------------------------------------------------------*/
struct VariableParsers
{
	const char	   *name;
					bool(*parser) (const char *);
					bool(*show) ();
					bool(*reset) ();
}				VariableParsers[] =

{
	{
		"datestyle", parse_date, show_date, reset_date
	},
	{
		"timezone", parse_null, show_null, reset_null
	},
	{
		"cost_heap", parse_cost_heap,
		show_cost_heap, reset_cost_heap
	},
	{
		"cost_index", parse_cost_index,
		show_cost_index, reset_cost_index
	},
	{
		"geqo", parse_geqo, show_geqo, reset_geqo
	},
	{
		"r_plans", parse_r_plans, show_r_plans, reset_r_plans
	},
	{
		NULL, NULL, NULL
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
