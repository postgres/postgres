/*
 * Routines for handling of SET var TO statements
 *
 * $Id: variable.c,v 1.3 1997/04/17 13:50:30 scrappy Exp $
 *
 * $Log: variable.c,v $
 * Revision 1.3  1997/04/17 13:50:30  scrappy
 * From: "Martin J. Laubach" <mjl@CSlab.tuwien.ac.at>
 * Subject: [HACKERS] Patch: set date to euro/us postgres/iso/sql
 *
 *   Here a patch that implements a SET date for use by the datetime
 * stuff. The syntax is
 *
 *         SET date TO 'val[,val,...]'
 *
 *   where val is us (us dates), euro (european dates), postgres,
 * iso or sql.
 *
 *   Thomas is working on the integration in his datetime module.
 * I just needed to get the patch out before it went stale :)
 *
 * Revision 1.1  1997/04/10 16:52:07  mjl
 * Initial revision
 */
/*-----------------------------------------------------------------------*/

#include <string.h>
#include "postgres.h"
#include "tcop/variable.h"

/*-----------------------------------------------------------------------*/
#if USE_EURODATES
#define DATE_EURO	TRUE
#else
#define DATE_EURO FALSE
#endif

/*-----------------------------------------------------------------------*/
struct PGVariables PGVariables =
	{
		{ DATE_EURO, Date_Postgres }
	};

/*-----------------------------------------------------------------------*/
static const char *get_token(char *buf, int size, const char *str)
	{
	if(!*str)
		return NULL;
		
	/* skip white space */
	while(*str && (*str == ' ' || *str == '\t'))
		str++;
	
	/* copy until we hit white space or comma or end of string */
	while(*str && *str != ' ' && *str != '\t' && *str != ',' && size-- > 1)
		*buf++ = *str++;
	
	*buf = '\0';
	
	/* skip white space and comma*/
	while(*str && (*str == ' ' || *str == '\t' || *str == ','))
		str++;
	
	return str;
	}
	
/*-----------------------------------------------------------------------*/
static bool parse_null(const char *value)
	{
	return TRUE;
	}
	
static bool parse_date(const char *value)
	{
	char tok[32];
	int dcnt = 0, ecnt = 0;
	
	while(value = get_token(tok, sizeof(tok), value))
		{
		/* Ugh. Somebody ought to write a table driven version -- mjl */
		
		if(!strcasecmp(tok, "iso"))
			{
			PGVariables.date.format = Date_ISO;
			dcnt++;
			}
		else if(!strcasecmp(tok, "sql"))
			{
			PGVariables.date.format = Date_SQL;
			dcnt++;
			}
		else if(!strcasecmp(tok, "postgres"))
			{
			PGVariables.date.format = Date_Postgres;
			dcnt++;
			}
		else if(!strcasecmp(tok, "euro"))
			{
			PGVariables.date.euro = TRUE;
			ecnt++;
			}
		else if(!strcasecmp(tok, "us"))
			{
			PGVariables.date.euro = FALSE;
			ecnt++;
			}
		else
			{
			elog(WARN, "Bad value for date (%s)", tok);
			}
		}
	
	if(dcnt > 1 || ecnt > 1)
		elog(NOTICE, "Conflicting settings for date");
		
	return TRUE;
	}
	
/*-----------------------------------------------------------------------*/
struct VariableParsers
	{
	const char *name;
	bool (*parser)(const char *);
	} VariableParsers[] =
	{
		{ "date", 		parse_date },
		{ "timezone", 	parse_null },
		{ NULL }
	};

/*-----------------------------------------------------------------------*/
bool SetPGVariable(const char *name, const char *value)
	{
	struct VariableParsers *vp;
	
	for(vp = VariableParsers; vp->name; vp++)
		{
		if(!strcasecmp(vp->name, name))
			return (vp->parser)(value);
		}
		
	elog(NOTICE, "No such variable %s", name);

	return TRUE;
	}

/*-----------------------------------------------------------------------*/
const char *GetPGVariable(const char *varName)
	{
	return NULL;
	}
/*-----------------------------------------------------------------------*/
