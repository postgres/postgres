/*
 * Routines for handling of SET var TO statements
 *
 * $Id: variable.c,v 1.4 1997/04/23 03:17:16 scrappy Exp $
 *
 * $Log: variable.c,v $
 * Revision 1.4  1997/04/23 03:17:16  scrappy
 * To: Thomas Lockhart <Thomas.G.Lockhart@jpl.nasa.gov>
 * Subject: Re: [PATCHES] SET DateStyle patches
 *
 * On Tue, 22 Apr 1997, Thomas Lockhart wrote:
 *
 * > Some more patches! These (try to) finish implementing SET variable TO value
 * > for "DateStyle" (changed the name from simply "date" to be more descriptive).
 * > This is based on code from Martin and Bruce (?), which was easy to modify.
 * > The syntax is
 * >
 * > SET DateStyle TO 'iso'
 * > SET DateStyle TO 'postgres'
 * > SET DateStyle TO 'sql'
 * > SET DateStyle TO 'european'
 * > SET DateStyle TO 'noneuropean'
 * > SET DateStyle TO 'us'         (same as "noneuropean")
 * > SET DateStyle TO 'default'    (current same as "postgres,us")
 * >
 * > ("european" is just compared for the first 4 characters, and "noneuropean"
 * > is compared for the first 7 to allow less typing).
 * >
 * > Multiple arguments are allowed, so SET datestyle TO 'sql,euro' is valid.
 * >
 * > My mods also try to implement "SHOW variable" and "RESET variable", but
 * > that part just core dumps at the moment. I would guess that my errors
 * > are obvious to someone who knows what they are doing with the parser stuff,
 * > so if someone (Bruce and/or Martin??) could have it do the right thing
 * > we will have a more complete set of what we need.
 * >
 * > Also, I would like to have a floating point precision global variable to
 * > implement "SET precision TO 10" and perhaps "SET precision TO 10,2" for
 * > float8 and float4, but I don't know how to do that for integer types rather
 * > than strings. If someone is fixing the SHOW and RESET code, perhaps they can
 * > add some hooks for me to do the floats while they are at it.
 * >
 * > I've left some remnants of variable structures in the source code which
 * > I did not use in the interests of getting something working for v6.1.
 * > We'll have time to clean things up for the next release...
 *
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

#include <stdio.h>
#include <string.h>
#include "postgres.h"
#include "miscadmin.h"
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
	
static bool show_null(const char *value)
	{
	return TRUE;
	}
	
static bool reset_null(const char *value)
	{
	return TRUE;
	}
	
static bool parse_date(const char *value)
	{
	char tok[32];
	int dcnt = 0, ecnt = 0;
	
	while((value = get_token(tok, sizeof(tok), value)) != 0)
		{
		/* Ugh. Somebody ought to write a table driven version -- mjl */
		
		if(!strcasecmp(tok, "iso"))
			{
			DateStyle = USE_ISO_DATES;
			dcnt++;
			}
		else if(!strcasecmp(tok, "sql"))
			{
			DateStyle = USE_SQL_DATES;
			dcnt++;
			}
		else if(!strcasecmp(tok, "postgres"))
			{
			DateStyle = USE_POSTGRES_DATES;
			dcnt++;
			}
		else if(!strncasecmp(tok, "euro", 4))
			{
			EuroDates = TRUE;
			ecnt++;
			}
		else if((!strcasecmp(tok, "us"))
		     || (!strncasecmp(tok, "noneuro", 7)))
			{
			EuroDates = FALSE;
			ecnt++;
			}
		else if(!strcasecmp(tok, "default"))
			{
			DateStyle = USE_POSTGRES_DATES;
			EuroDates = FALSE;
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
	
static bool show_date()
	{
	char buf[64];

	sprintf( buf, "Date style is %s with%s European conventions",
	  ((DateStyle == USE_ISO_DATES)? "iso": ((DateStyle == USE_ISO_DATES)? "sql": "postgres")),
	  ((EuroDates)? "": "out"));

	elog(NOTICE, buf, NULL);

	return TRUE;
	}
	
static bool reset_date()
	{
	DateStyle = USE_POSTGRES_DATES;
	EuroDates = FALSE;

	return TRUE;
	}
	
/*-----------------------------------------------------------------------*/
struct VariableParsers
	{
	const char *name;
	bool (*parser)(const char *);
	bool (*show)();
	bool (*reset)();
	} VariableParsers[] =
	{
		{ "datestyle",	parse_date,	show_date,	reset_date },
		{ "timezone", 	parse_null,	show_null,	reset_null },
		{ NULL, NULL, NULL }
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
		
	elog(NOTICE, "Unrecognized variable %s", name);

	return TRUE;
	}

/*-----------------------------------------------------------------------*/
bool GetPGVariable(const char *name)
	{
	struct VariableParsers *vp;
	
	for(vp = VariableParsers; vp->name; vp++)
		{
		if(!strcasecmp(vp->name, name))
			return (vp->show)();
		}
		
	elog(NOTICE, "Unrecognized variable %s", name);

	return TRUE;
	}

/*-----------------------------------------------------------------------*/
bool ResetPGVariable(const char *name)
	{
	struct VariableParsers *vp;
	
	for(vp = VariableParsers; vp->name; vp++)
		{
		if(!strcasecmp(vp->name, name))
			return (vp->reset)();
		}
		
	elog(NOTICE, "Unrecognized variable %s", name);

	return TRUE;
	}
