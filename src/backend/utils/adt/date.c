/*-------------------------------------------------------------------------
 *
 * date.c--
 *	  Utilities for the built-in type "AbsoluteTime" (defined in nabstime).
 *	  Functions for the built-in type "RelativeTime".
 *	  Functions for the built-in type "TimeInterval".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/date.c,v 1.17 1997/09/08 20:57:26 momjian Exp $
 *
 * NOTES
 *	 This code is actually (almost) unused.
 *	 It needs to be integrated with Time and struct trange.
 *
 * XXX	This code needs to be rewritten to work with the "new" definitions
 * XXX	in h/tim.h.  Look for int32's, int, long, etc. in the code.  The
 * XXX	definitions in h/tim.h may need to be rethought also.
 *
 * XXX	This code has been cleaned up some - avi 07/07/93
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <string.h>

#include "postgres.h"
#include "miscadmin.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include "access/xact.h"
#include "utils/builtins.h"		/* where function declarations go */
#include "utils/palloc.h"
#include "utils/dt.h"

#define INVALID_RELTIME_STR		"Undefined RelTime"
#define INVALID_RELTIME_STR_LEN (sizeof(INVALID_RELTIME_STR)-1)
#define RELTIME_LABEL			'@'
#define RELTIME_PAST			"ago"
#define DIRMAXLEN				(sizeof(RELTIME_PAST)-1)

/*
 *	Unix epoch is Jan  1 00:00:00 1970.  Postgres knows about times
 *	sixty-eight years on either side of that.
 */

#define IsSpace(C)				((C) == ' ')

#define T_INTERVAL_INVAL   0	/* data represents no valid interval */
#define T_INTERVAL_VALID   1	/* data represents a valid interval */
/*
 * ['Mon May 10 23:59:12 1943 PST' 'Sun Jan 14 03:14:21 1973 PST']
 * 0		1		  2			3		  4			5		  6
 * 1234567890123456789012345678901234567890123456789012345678901234
 *
 * we allocate some extra -- timezones are usually 3 characters but
 * this is not in the POSIX standard...
 */
#define T_INTERVAL_LEN					80
#define INVALID_INTERVAL_STR			"Undefined Range"
#define INVALID_INTERVAL_STR_LEN		(sizeof(INVALID_INTERVAL_STR)-1)

#define ABSTIMEMIN(t1, t2) abstimele((t1),(t2)) ? (t1) : (t2)
#define ABSTIMEMAX(t1, t2) abstimelt((t1),(t2)) ? (t2) : (t1)

#if FALSE
static char *unit_tab[] = {
	"second", "seconds", "minute", "minutes",
	"hour", "hours", "day", "days", "week", "weeks",
"month", "months", "year", "years"};

#define UNITMAXLEN 7			/* max length of a unit name */
#define NUNITS 14				/* number of different units */

/* table of seconds per unit (month = 30 days, year = 365 days)  */
static int	sec_tab[] = {
	1, 1, 60, 60,
	3600, 3600, 86400, 86400, 604800, 604800,
2592000, 2592000, 31536000, 31536000};

#endif

/*
 * Function prototypes -- internal to this file only
 */

static void reltime2tm(int32 time, struct tm * tm);

#if FALSE
static int	correct_unit(char unit[], int *unptr);
static int	correct_dir(char direction[], int *signptr);

#endif

static int
istinterval(char *i_string,
			AbsoluteTime *i_start,
			AbsoluteTime *i_end);

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		reltimein		- converts a reltime string in an internal format
 */
RelativeTime
reltimein(char *str)
{
	RelativeTime result;

	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	int			dtype;
	char	   *field[MAXDATEFIELDS];
	int			nf,
				ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	if (!PointerIsValid(str))
		elog(WARN, "Bad (null) date external representation", NULL);

	if (strlen(str) > MAXDATELEN)
		elog(WARN, "Bad (length) reltime external representation '%s'", str);

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateDelta(field, ftype, nf, &dtype, tm, &fsec) != 0))
		elog(WARN, "Bad reltime external representation '%s'", str);

#ifdef DATEDEBUG
	printf("reltimein- %d fields are type %d (DTK_DATE=%d)\n", nf, dtype, DTK_DATE);
#endif

	switch (dtype)
	{
		case DTK_DELTA:
			result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec);
			result += (((tm->tm_year * 365) + (tm->tm_mon * 30) + tm->tm_mday) * (24 * 60 * 60));
			return (result);

		default:
			return (INVALID_RELTIME);
	}

	elog(WARN, "Bad reltime (internal coding error) '%s'", str);
	return (INVALID_RELTIME);
}								/* reltimein() */


/*
 *		reltimeout		- converts the internal format to a reltime string
 */
char	   *
reltimeout(int32 time)
{
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	if (time == INVALID_RELTIME)
	{
		strcpy(buf, INVALID_RELTIME_STR);

	}
	else
	{
		reltime2tm(time, tm);
		EncodeTimeSpan(tm, 0, DateStyle, buf);
	}

	result = PALLOC(strlen(buf) + 1);
	strcpy(result, buf);

	return (result);
}								/* reltimeout() */


#define TMODULO(t,q,u) {q = (t / u); \
						if (q != 0) t -= (q * u);}

static void
reltime2tm(int32 time, struct tm * tm)
{
	TMODULO(time, tm->tm_year, 31536000);
	TMODULO(time, tm->tm_mon, 2592000);
	TMODULO(time, tm->tm_mday, 86400);
	TMODULO(time, tm->tm_hour, 3600);
	TMODULO(time, tm->tm_min, 60);
	TMODULO(time, tm->tm_sec, 1);

	return;
}								/* reltime2tm() */

#if FALSE
char	   *timestring;
long		quantity;
register int i;
int			unitnr;

timestring = (char *) palloc(Max(strlen(INVALID_RELTIME_STR),
								 UNITMAXLEN) + 1);
if (timevalue == INVALID_RELTIME)
{
	strcpy(timestring, INVALID_RELTIME_STR);
	return (timestring);
}

if (timevalue == 0)
	i = 1;						/* unit = 'seconds' */
else
	for (i = 12; i >= 0; i = i - 2)
		if ((timevalue % sec_tab[i]) == 0)
			break;				/* appropriate unit found */
unitnr = i;
quantity = (timevalue / sec_tab[unitnr]);
if (quantity > 1 || quantity < -1)
	unitnr++;					/* adjust index for PLURAL of unit */
if (quantity >= 0)
	sprintf(timestring, "%c %lu %s", RELTIME_LABEL,
			quantity, unit_tab[unitnr]);
else
	sprintf(timestring, "%c %lu %s %s", RELTIME_LABEL,
			(quantity * -1), unit_tab[unitnr], RELTIME_PAST);
return (timestring);
}

#endif


/*
 *		tintervalin		- converts an interval string to an internal format
 */
TimeInterval
tintervalin(char *intervalstr)
{
	int			error;
	AbsoluteTime i_start,
				i_end,
				t1,
				t2;
	TimeInterval interval;

	interval = (TimeInterval) palloc(sizeof(TimeIntervalData));
	error = istinterval(intervalstr, &t1, &t2);
	if (error == 0)
		interval->status = T_INTERVAL_INVAL;
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		interval->status = T_INTERVAL_INVAL;	/* undefined  */
	else
	{
		i_start = ABSTIMEMIN(t1, t2);
		i_end = ABSTIMEMAX(t1, t2);
		interval->data[0] = i_start;
		interval->data[1] = i_end;
		interval->status = T_INTERVAL_VALID;
	}
	return (interval);
}


/*
 *		tintervalout	- converts an internal interval format to a string
 *
 */
char	   *
tintervalout(TimeInterval interval)
{
	char	   *i_str,
			   *p;

	i_str = (char *) palloc(T_INTERVAL_LEN);	/* ['...' '...'] */
	strcpy(i_str, "[\"");
	if (interval->status == T_INTERVAL_INVAL)
		strcat(i_str, INVALID_INTERVAL_STR);
	else
	{
		p = nabstimeout(interval->data[0]);
		strcat(i_str, p);
		pfree(p);
		strcat(i_str, "\" \"");
		p = nabstimeout(interval->data[1]);
		strcat(i_str, p);
		pfree(p);
	}
	strcat(i_str, "\"]\0");
	return (i_str);
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

RelativeTime
timespan_reltime(TimeSpan * timespan)
{
	RelativeTime time;
	int			year,
				month;
	double		span;

	if (!PointerIsValid(timespan))
		time = INVALID_RELTIME;

	if (TIMESPAN_IS_INVALID(*timespan))
	{
		time = INVALID_RELTIME;

	}
	else
	{
		if (timespan->month == 0)
		{
			year = 0;
			month = 0;

		}
		else if (abs(timespan->month) >= 12)
		{
			year = (timespan->month / 12);
			month = (timespan->month % 12);

		}
		else
		{
			year = 0;
			month = timespan->month;
		}

		span = (((((double) 365 * year) + ((double) 30 * month)) * 86400) + timespan->time);

#ifdef DATEDEBUG
		printf("timespan_reltime- convert m%d s%f to %f [%d %d]\n",
			   timespan->month, timespan->time, span, INT_MIN, INT_MAX);
#endif

		time = (((span > INT_MIN) && (span < INT_MAX)) ? span : INVALID_RELTIME);
	}

	return (time);
}								/* timespan_reltime() */


TimeSpan   *
reltime_timespan(RelativeTime reltime)
{
	TimeSpan   *result;
	int			year,
				month;

	if (!PointerIsValid(result = PALLOCTYPE(TimeSpan)))
		elog(WARN, "Memory allocation failed, can't convert reltime to timespan", NULL);

	switch (reltime)
	{
		case INVALID_RELTIME:
			TIMESPAN_INVALID(*result);
			break;

		default:
			TMODULO(reltime, year, 31536000);
			TMODULO(reltime, month, 2592000);

			result->time = reltime;
			result->month = ((12 * year) + month);
	}

	return (result);
}								/* reltime_timespan() */


/*
 *		mktinterval		- creates a time interval with endpoints t1 and t2
 */
TimeInterval
mktinterval(AbsoluteTime t1, AbsoluteTime t2)
{
	AbsoluteTime tstart = ABSTIMEMIN(t1, t2),
				tend = ABSTIMEMAX(t1, t2);
	TimeInterval interval;

	interval = (TimeInterval) palloc(sizeof(TimeIntervalData));
	if (t1 == INVALID_ABSTIME || t2 == INVALID_ABSTIME)
		interval->status = T_INTERVAL_INVAL;
	else
	{
		interval->status = T_INTERVAL_VALID;
		interval->data[0] = tstart;
		interval->data[1] = tend;
	}

	return interval;
}

/*
 *		timepl, timemi and abstimemi use the formula
 *				abstime + reltime = abstime
 *		so		abstime - reltime = abstime
 *		and		abstime - abstime = reltime
 */

/*
 *		timepl			- returns the value of (abstime t1 + relime t2)
 */
AbsoluteTime
timepl(AbsoluteTime t1, RelativeTime t2)
{
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		RelativeTimeIsValid(t2) &&
		((t2 > 0) ? (t1 < NOEND_ABSTIME - t2)
		 : (t1 > NOSTART_ABSTIME - t2)))		/* prevent overflow */
		return (t1 + t2);

	return (INVALID_ABSTIME);
}


/*
 *		timemi			- returns the value of (abstime t1 - reltime t2)
 */
AbsoluteTime
timemi(AbsoluteTime t1, RelativeTime t2)
{
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		RelativeTimeIsValid(t2) &&
		((t2 > 0) ? (t1 > NOSTART_ABSTIME + t2)
		 : (t1 < NOEND_ABSTIME + t2)))	/* prevent overflow */
		return (t1 - t2);

	return (INVALID_ABSTIME);
}


/*
 *		abstimemi		- returns the value of (abstime t1 - abstime t2)
 */
static RelativeTime
abstimemi(AbsoluteTime t1, AbsoluteTime t2)
{
	if (t1 == CURRENT_ABSTIME)
		t1 = GetCurrentTransactionStartTime();
	if (t2 == CURRENT_ABSTIME)
		t2 = GetCurrentTransactionStartTime();

	if (AbsoluteTimeIsReal(t1) &&
		AbsoluteTimeIsReal(t2))
		return (t1 - t2);

	return (INVALID_RELTIME);
}


/*
 *		ininterval		- returns 1, iff absolute date is in the interval
 */
int
ininterval(AbsoluteTime t, TimeInterval interval)
{
	if (interval->status == T_INTERVAL_VALID && t != INVALID_ABSTIME)
		return (abstimege(t, interval->data[0]) &&
				abstimele(t, interval->data[1]));
	return (0);
}

/*
 *		intervalrel		- returns  relative time corresponding to interval
 */
RelativeTime
intervalrel(TimeInterval interval)
{
	if (interval->status == T_INTERVAL_VALID)
		return (abstimemi(interval->data[1], interval->data[0]));
	else
		return (INVALID_RELTIME);
}

/*
 *		timenow			- returns  time "now", internal format
 *
 *		Now AbsoluteTime is time since Jan 1 1970 -mer 7 Feb 1992
 */
AbsoluteTime
timenow()
{
	time_t		sec;

	if (time(&sec) < 0)
		return (INVALID_ABSTIME);
	return ((AbsoluteTime) sec);
}

/*
 *		reltimeeq		- returns 1, iff arguments are equal
 *		reltimene		- returns 1, iff arguments are not equal
 *		reltimelt		- returns 1, iff t1 less than t2
 *		reltimegt		- returns 1, iff t1 greater than t2
 *		reltimele		- returns 1, iff t1 less than or equal to t2
 *		reltimege		- returns 1, iff t1 greater than or equal to t2
 */
bool
reltimeeq(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return (t1 == t2);
}

bool
reltimene(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return (t1 != t2);
}

bool
reltimelt(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return (t1 < t2);
}

bool
reltimegt(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return (t1 > t2);
}

bool
reltimele(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return (t1 <= t2);
}

bool
reltimege(RelativeTime t1, RelativeTime t2)
{
	if (t1 == INVALID_RELTIME || t2 == INVALID_RELTIME)
		return 0;
	return (t1 >= t2);
}


/*
 *		intervaleq		- returns 1, iff interval i1 is equal to interval i2
 */
bool
intervaleq(TimeInterval i1, TimeInterval i2)
{
	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return (0);				/* invalid interval */
	return (abstimeeq(i1->data[0], i2->data[0]) &&
			abstimeeq(i1->data[1], i2->data[1]));
}

/*
 *		intervalleneq	- returns 1, iff length of interval i is equal to
 *								reltime t
 */
bool
intervalleneq(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return (0);
	rt = intervalrel(i);
	return (rt != INVALID_RELTIME && rt == t);
}

/*
 *		intervallenne	- returns 1, iff length of interval i is not equal
 *								to reltime t
 */
bool
intervallenne(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return (0);
	rt = intervalrel(i);
	return (rt != INVALID_RELTIME && rt != t);
}

/*
 *		intervallenlt	- returns 1, iff length of interval i is less than
 *								reltime t
 */
bool
intervallenlt(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return (0);
	rt = intervalrel(i);
	return (rt != INVALID_RELTIME && rt < t);
}

/*
 *		intervallengt	- returns 1, iff length of interval i is greater than
 *								reltime t
 */
bool
intervallengt(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return (0);
	rt = intervalrel(i);
	return (rt != INVALID_RELTIME && rt > t);
}

/*
 *		intervallenle	- returns 1, iff length of interval i is less or equal
 *									than reltime t
 */
bool
intervallenle(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return (0);
	rt = intervalrel(i);
	return (rt != INVALID_RELTIME && rt <= t);
}

/*
 *		intervallenge	- returns 1, iff length of interval i is greater or
 *								equal than reltime t
 */
bool
intervallenge(TimeInterval i, RelativeTime t)
{
	RelativeTime rt;

	if ((i->status == T_INTERVAL_INVAL) || (t == INVALID_RELTIME))
		return (0);
	rt = intervalrel(i);
	return (rt != INVALID_RELTIME && rt >= t);
}

/*
 *		intervalct		- returns 1, iff interval i1 contains interval i2
 */
bool
intervalct(TimeInterval i1, TimeInterval i2)
{
	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return (0);
	return (abstimele(i1->data[0], i2->data[0]) &&
			abstimege(i1->data[1], i2->data[1]));
}

/*
 *		intervalov		- returns 1, iff interval i1 (partially) overlaps i2
 */
bool
intervalov(TimeInterval i1, TimeInterval i2)
{
	if (i1->status == T_INTERVAL_INVAL || i2->status == T_INTERVAL_INVAL)
		return (0);
	return (!(abstimelt(i1->data[1], i2->data[0]) ||
			  abstimegt(i1->data[0], i2->data[1])));
}

/*
 *		intervalstart	- returns  the start of interval i
 */
AbsoluteTime
intervalstart(TimeInterval i)
{
	if (i->status == T_INTERVAL_INVAL)
		return INVALID_ABSTIME;
	return (i->data[0]);
}

/*
 *		intervalend		- returns  the end of interval i
 */
AbsoluteTime
intervalend(TimeInterval i)
{
	if (i->status == T_INTERVAL_INVAL)
		return INVALID_ABSTIME;
	return (i->data[1]);
}


/*****************************************************************************
 *	 PRIVATE ROUTINES														 *
 *****************************************************************************/

/*
 *		isreltime		- returns 1, iff datestring is of type reltime
 *								  2, iff datestring is 'invalid time' identifier
 *								  0, iff datestring contains a syntax error
 *		VALID time	less or equal +/-  `@ 68 years'
 *
 */
int
isreltime(char *str)
{
	struct tm	tt,
			   *tm = &tt;
	double		fsec;
	int			dtype;
	char	   *field[MAXDATEFIELDS];
	int			nf,
				ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];

	if (!PointerIsValid(str))
		return 0;

	if (strlen(str) > MAXDATELEN)
		return 0;

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf) != 0)
		|| (DecodeDateDelta(field, ftype, nf, &dtype, tm, &fsec) != 0))
		return 0;

	switch (dtype)
	{
		case (DTK_DELTA):
			return ((abs(tm->tm_year) <= 68) ? 1 : 0);
			break;

		case (DTK_INVALID):
			return 2;
			break;

		default:
			return 0;
			break;
	}

	return 0;
}								/* isreltime() */

#if FALSE
register char *p;
register char c;
int			i;
char		unit[UNITMAXLEN];
char		direction[DIRMAXLEN];
int			localSign;
int			localUnitNumber;
long		localQuantity;

if (!PointerIsValid(sign))
{
	sign = &localSign;
}

if (!PointerIsValid(unitnr))
{
	unitnr = &localUnitNumber;
}

if (!PointerIsValid(quantity))
{
	quantity = &localQuantity;
}

unit[0] = '\0';
direction[0] = '\0';
p = timestring;
 /* skip leading blanks */
while ((c = *p) != '\0')
{
	if (c != ' ')
		break;
	p++;
}

 /* Test whether 'invalid time' identifier or not */
if (!strncmp(INVALID_RELTIME_STR, p, strlen(INVALID_RELTIME_STR) + 1))
	return (2);					/* correct 'invalid time' identifier found */

 /* handle label of relative time */
if (c != RELTIME_LABEL)
	return (0);					/* syntax error */
c = *++p;
if (c != ' ')
	return (0);					/* syntax error */
p++;
 /* handle the quantity */
*quantity = 0;
for (;;)
{
	c = *p;
	if (isdigit(c))
	{
		*quantity = *quantity * 10 + (c - '0');
		p++;
	}
	else
	{
		if (c == ' ')
			break;				/* correct quantity found */
		else
			return (0);			/* syntax error */
	}
}

 /* handle unit */
p++;
i = 0;
for (;;)
{
	c = *p;
	if (c >= 'a' && c <= 'z' && i <= (UNITMAXLEN - 1))
	{
		unit[i] = c;
		p++;
		i++;
	}
	else
	{
		if ((c == ' ' || c == '\0')
			&& correct_unit(unit, unitnr))
			break;				/* correct unit found */
		else
			return (0);			/* syntax error */
	}
}

 /* handle optional direction */
if (c == ' ')
	p++;
i = 0;
*sign = 1;
for (;;)
{
	c = *p;
	if (c >= 'a' && c <= 'z' && i <= (DIRMAXLEN - 1))
	{
		direction[i] = c;
		p++;
		i++;
	}
	else
	{
		if ((c == ' ' || c == '\0') && i == 0)
		{
			*sign = 1;
			break;				/* no direction specified */
		}
		if ((c == ' ' || c == '\0') && i != 0)
		{
			direction[i] = '\0';
			correct_dir(direction, sign);
			break;				/* correct direction found */
		}
		else
			return (0);			/* syntax error */
	}
}

return (1);
}

/*
 *		correct_unit	- returns 1, iff unit is a correct unit description
 *
 *		output parameter:
 *				unptr: points to an integer which is the appropriate unit number
 *					   (see function isreltime())
 */
static int
correct_unit(char unit[], int *unptr)
{
	int			j = 0;

	while (j < NUNITS)
	{
		if (strncmp(unit, unit_tab[j], strlen(unit_tab[j])) == 0)
		{
			*unptr = j;
			return (1);
		}
		j++;
	}
	return (0);					/* invalid unit descriptor */
}

/*
 *		correct_dir		- returns 1, iff direction is a correct identifier
 *
 *		output parameter:
 *				signptr: points to -1 if dir corresponds to past tense
 *						 else  to 1
 */
static int
correct_dir(char direction[], int *signptr)
{
	*signptr = 1;
	if (strncmp(RELTIME_PAST, direction, strlen(RELTIME_PAST) + 1) == 0)
	{
		*signptr = -1;
		return (1);
	}
	else
		return (0);				/* invalid direction descriptor */
}

#endif

/*
 *		istinterval		- returns 1, iff i_string is a valid interval descr.
 *								  0, iff i_string is NOT a valid interval desc.
 *								  2, iff any time is INVALID_ABSTIME
 *
 *		output parameter:
 *				i_start, i_end: interval margins
 *
 *		Time interval:
 *		`[' {` '} `'' <AbsTime> `'' {` '} `'' <AbsTime> `'' {` '} `]'
 *
 *		OR	`Undefined Range'	(see also INVALID_INTERVAL_STR)
 *
 *		where <AbsTime> satisfies the syntax of absolute time.
 *
 *		e.g.  [  '  Jan 18 1902'   'Jan 1 00:00:00 1970']
 */
static int
istinterval(char *i_string,
			AbsoluteTime *i_start,
			AbsoluteTime *i_end)
{
	register char *p,
			   *p1;
	register char c;

	p = i_string;
	/* skip leading blanks up to '[' */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != '[')
			return (0);			/* syntax error */
		else
			break;
	}
	p++;
	/* skip leading blanks up to "'" */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != '"')
			return (0);			/* syntax error */
		else
			break;
	}
	p++;
	if (strncmp(INVALID_INTERVAL_STR, p, strlen(INVALID_INTERVAL_STR)) == 0)
		return (0);				/* undefined range, handled like a syntax
								 * err. */
	/* search for the end of the first date and change it to a NULL */
	p1 = p;
	while ((c = *p1) != '\0')
	{
		if (c == '"')
		{
			*p1 = '\0';
			break;
		}
		p1++;
	}
	/* get the first date */
	*i_start = nabstimein(p);	/* first absolute date */
	/* rechange NULL at the end of the first date to a "'" */
	*p1 = '"';
	p = ++p1;
	/* skip blanks up to "'", beginning of second date */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != '"')
			return (0);			/* syntax error */
		else
			break;
	}
	p++;
	/* search for the end of the second date and change it to a NULL */
	p1 = p;
	while ((c = *p1) != '\0')
	{
		if (c == '"')
		{
			*p1 = '\0';
			break;
		}
		p1++;
	}
	/* get the second date */
	*i_end = nabstimein(p);		/* second absolute date */
	/* rechange NULL at the end of the first date to a ''' */
	*p1 = '"';
	p = ++p1;
	/* skip blanks up to ']' */
	while ((c = *p) != '\0')
	{
		if (IsSpace(c))
			p++;
		else if (c != ']')
			return (0);			/* syntax error */
		else
			break;
	}
	p++;
	c = *p;
	if (c != '\0')
		return (0);				/* syntax error */
	/* it seems to be a valid interval */
	return (1);
}


/*****************************************************************************
 *
 *****************************************************************************/

/*
 * timeofday -
 *	   returns the current time as a text. similar to timenow() but returns
 *	   seconds with more precision (up to microsecs). (I need this to compare
 *	   the Wisconsin benchmark with Illustra whose TimeNow() shows current
 *	   time with precision up to microsecs.)			  - ay 3/95
 */
text	   *
timeofday(void)
{

	struct timeval tp;
	struct timezone tpz;
	char		templ[500];
	char		buf[500];
	text	   *tm;
	int			len = 0;

	gettimeofday(&tp, &tpz);
	strftime(templ, sizeof(templ), "%a %b %d %H:%M:%S.%%d %Y %Z",
			 localtime((time_t *) & tp.tv_sec));
	sprintf(buf, templ, tp.tv_usec);

	len = VARHDRSZ + strlen(buf);
	tm = (text *) palloc(len);
	VARSIZE(tm) = len;
	strncpy(VARDATA(tm), buf, strlen(buf));
	return tm;
}
