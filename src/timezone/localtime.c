/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson.
 *
 * IDENTIFICATION
 *	  src/timezone/localtime.c
 */

/*
 * Leap second handling from Bradley White.
 * POSIX-style TZ environment variable handling from Guy Harris.
 */

/* this file needs to build in both frontend and backend contexts */
#include "c.h"

#include <fcntl.h>

#include "datatype/timestamp.h"
#include "private.h"
#include "pgtz.h"
#include "tzfile.h"


#ifndef WILDABBR
/*
 * Someone might make incorrect use of a time zone abbreviation:
 *	1.  They might reference tzname[0] before calling tzset (explicitly
 *		or implicitly).
 *	2.  They might reference tzname[1] before calling tzset (explicitly
 *		or implicitly).
 *	3.  They might reference tzname[1] after setting to a time zone
 *		in which Daylight Saving Time is never observed.
 *	4.  They might reference tzname[0] after setting to a time zone
 *		in which Standard Time is never observed.
 *	5.  They might reference tm.TM_ZONE after calling offtime.
 * What's best to do in the above cases is open to debate;
 * for now, we just set things up so that in any of the five cases
 * WILDABBR is used. Another possibility: initialize tzname[0] to the
 * string "tzname[0] used before set", and similarly for the other cases.
 * And another: initialize tzname[0] to "ERA", with an explanation in the
 * manual page of what this "time zone abbreviation" means (doing this so
 * that tzname[0] has the "normal" length of three characters).
 */
#define WILDABBR	"   "
#endif   /* !defined WILDABBR */

static const char wildabbr[] = WILDABBR;

static const char gmt[] = "GMT";

/* The minimum and maximum finite time values.  This assumes no padding.  */
static const pg_time_t time_t_min = MINVAL(pg_time_t, TYPE_BIT(pg_time_t));
static const pg_time_t time_t_max = MAXVAL(pg_time_t, TYPE_BIT(pg_time_t));

/*
 * The DST rules to use if TZ has no rules and we can't load TZDEFRULES.
 * We default to US rules as of 1999-08-17.
 * POSIX 1003.1 section 8.1.1 says that the default DST rules are
 * implementation dependent; for historical reasons, US rules are a
 * common default.
 */
#define TZDEFRULESTRING ",M4.1.0,M10.5.0"

/* structs ttinfo, lsinfo, state have been moved to pgtz.h */

enum r_type
{
	JULIAN_DAY,					/* Jn = Julian day */
	DAY_OF_YEAR,				/* n = day of year */
	MONTH_NTH_DAY_OF_WEEK		/* Mm.n.d = month, week, day of week */
};

struct rule
{
	enum r_type r_type;			/* type of rule */
	int			r_day;			/* day number of rule */
	int			r_week;			/* week number of rule */
	int			r_mon;			/* month number of rule */
	int32		r_time;			/* transition time of rule */
};

/*
 * Prototypes for static functions.
 */

static struct pg_tm *gmtsub(pg_time_t const *, int32, struct pg_tm *);
static bool increment_overflow(int *, int);
static bool increment_overflow_time(pg_time_t *, int32);
static struct pg_tm *timesub(pg_time_t const *, int32, struct state const *,
		struct pg_tm *);
static bool typesequiv(struct state const *, int, int);


/*
 * Section 4.12.3 of X3.159-1989 requires that
 *	Except for the strftime function, these functions [asctime,
 *	ctime, gmtime, localtime] return values in one of two static
 *	objects: a broken-down time structure and an array of char.
 * Thanks to Paul Eggert for noting this.
 */

static struct pg_tm tm;

/* Initialize *S to a value based on GMTOFF, ISDST, and ABBRIND.  */
static void
init_ttinfo(struct ttinfo * s, int32 gmtoff, bool isdst, int abbrind)
{
	s->tt_gmtoff = gmtoff;
	s->tt_isdst = isdst;
	s->tt_abbrind = abbrind;
	s->tt_ttisstd = false;
	s->tt_ttisgmt = false;
}

static int32
detzcode(const char *codep)
{
	int32		result;
	int			i;
	int32		one = 1;
	int32		halfmaxval = one << (32 - 2);
	int32		maxval = halfmaxval - 1 + halfmaxval;
	int32		minval = -1 - maxval;

	result = codep[0] & 0x7f;
	for (i = 1; i < 4; ++i)
		result = (result << 8) | (codep[i] & 0xff);

	if (codep[0] & 0x80)
	{
		/*
		 * Do two's-complement negation even on non-two's-complement machines.
		 * If the result would be minval - 1, return minval.
		 */
		result -= !TWOS_COMPLEMENT(int32) &&result != 0;
		result += minval;
	}
	return result;
}

static int64
detzcode64(const char *codep)
{
	uint64		result;
	int			i;
	int64		one = 1;
	int64		halfmaxval = one << (64 - 2);
	int64		maxval = halfmaxval - 1 + halfmaxval;
	int64		minval = -TWOS_COMPLEMENT(int64) -maxval;

	result = codep[0] & 0x7f;
	for (i = 1; i < 8; ++i)
		result = (result << 8) | (codep[i] & 0xff);

	if (codep[0] & 0x80)
	{
		/*
		 * Do two's-complement negation even on non-two's-complement machines.
		 * If the result would be minval - 1, return minval.
		 */
		result -= !TWOS_COMPLEMENT(int64) &&result != 0;
		result += minval;
	}
	return result;
}

static bool
differ_by_repeat(const pg_time_t t1, const pg_time_t t0)
{
	if (TYPE_BIT(pg_time_t) -TYPE_SIGNED(pg_time_t) <SECSPERREPEAT_BITS)
		return 0;
	return t1 - t0 == SECSPERREPEAT;
}

/* Input buffer for data read from a compiled tz file.  */
union input_buffer
{
	/* The first part of the buffer, interpreted as a header.  */
	struct tzhead tzhead;

	/* The entire buffer.  */
	char		buf[2 * sizeof(struct tzhead) + 2 * sizeof(struct state)
					+			4 * TZ_MAX_TIMES];
};

/* Local storage needed for 'tzloadbody'.  */
union local_storage
{
	/* We don't need the "fullname" member */

	/* The results of analyzing the file's contents after it is opened.  */
	struct
	{
		/* The input buffer.  */
		union input_buffer u;

		/* A temporary state used for parsing a TZ string in the file.  */
		struct state st;
	}			u;
};

/* Load tz data from the file named NAME into *SP.  Read extended
 * format if DOEXTEND.  Use *LSP for temporary storage.  Return 0 on
 * success, an errno value on failure.
 * PG: If "canonname" is not NULL, then on success the canonical spelling of
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 */
static int
tzloadbody(char const * name, char *canonname, struct state * sp, bool doextend,
		   union local_storage * lsp)
{
	int			i;
	int			fid;
	int			stored;
	ssize_t		nread;
	union input_buffer *up = &lsp->u.u;
	int			tzheadsize = sizeof(struct tzhead);

	sp->goback = sp->goahead = false;

	if (!name)
	{
		name = TZDEFAULT;
		if (!name)
			return EINVAL;
	}

	if (name[0] == ':')
		++name;

	fid = pg_open_tzfile(name, canonname);
	if (fid < 0)
		return ENOENT;			/* pg_open_tzfile may not set errno */

	nread = read(fid, up->buf, sizeof up->buf);
	if (nread < tzheadsize)
	{
		int			err = nread < 0 ? errno : EINVAL;

		close(fid);
		return err;
	}
	if (close(fid) < 0)
		return errno;
	for (stored = 4; stored <= 8; stored *= 2)
	{
		int32		ttisstdcnt = detzcode(up->tzhead.tzh_ttisstdcnt);
		int32		ttisgmtcnt = detzcode(up->tzhead.tzh_ttisgmtcnt);
		int32		leapcnt = detzcode(up->tzhead.tzh_leapcnt);
		int32		timecnt = detzcode(up->tzhead.tzh_timecnt);
		int32		typecnt = detzcode(up->tzhead.tzh_typecnt);
		int32		charcnt = detzcode(up->tzhead.tzh_charcnt);
		char const *p = up->buf + tzheadsize;

		if (!(0 <= leapcnt && leapcnt < TZ_MAX_LEAPS
			  && 0 < typecnt && typecnt < TZ_MAX_TYPES
			  && 0 <= timecnt && timecnt < TZ_MAX_TIMES
			  && 0 <= charcnt && charcnt < TZ_MAX_CHARS
			  && (ttisstdcnt == typecnt || ttisstdcnt == 0)
			  && (ttisgmtcnt == typecnt || ttisgmtcnt == 0)))
			return EINVAL;
		if (nread
			< (tzheadsize		/* struct tzhead */
			   + timecnt * stored		/* ats */
			   + timecnt		/* types */
			   + typecnt * 6	/* ttinfos */
			   + charcnt		/* chars */
			   + leapcnt * (stored + 4) /* lsinfos */
			   + ttisstdcnt		/* ttisstds */
			   + ttisgmtcnt))	/* ttisgmts */
			return EINVAL;
		sp->leapcnt = leapcnt;
		sp->timecnt = timecnt;
		sp->typecnt = typecnt;
		sp->charcnt = charcnt;

		/*
		 * Read transitions, discarding those out of pg_time_t range. But
		 * pretend the last transition before time_t_min occurred at
		 * time_t_min.
		 */
		timecnt = 0;
		for (i = 0; i < sp->timecnt; ++i)
		{
			int64		at
			= stored == 4 ? detzcode(p) : detzcode64(p);

			sp->types[i] = at <= time_t_max;
			if (sp->types[i])
			{
				pg_time_t	attime
				= ((TYPE_SIGNED(pg_time_t) ? at < time_t_min : at < 0)
				   ? time_t_min : at);

				if (timecnt && attime <= sp->ats[timecnt - 1])
				{
					if (attime < sp->ats[timecnt - 1])
						return EINVAL;
					sp->types[i - 1] = 0;
					timecnt--;
				}
				sp->ats[timecnt++] = attime;
			}
			p += stored;
		}

		timecnt = 0;
		for (i = 0; i < sp->timecnt; ++i)
		{
			unsigned char typ = *p++;

			if (sp->typecnt <= typ)
				return EINVAL;
			if (sp->types[i])
				sp->types[timecnt++] = typ;
		}
		sp->timecnt = timecnt;
		for (i = 0; i < sp->typecnt; ++i)
		{
			struct ttinfo *ttisp;
			unsigned char isdst,
						abbrind;

			ttisp = &sp->ttis[i];
			ttisp->tt_gmtoff = detzcode(p);
			p += 4;
			isdst = *p++;
			if (!(isdst < 2))
				return EINVAL;
			ttisp->tt_isdst = isdst;
			abbrind = *p++;
			if (!(abbrind < sp->charcnt))
				return EINVAL;
			ttisp->tt_abbrind = abbrind;
		}
		for (i = 0; i < sp->charcnt; ++i)
			sp->chars[i] = *p++;
		sp->chars[i] = '\0';	/* ensure '\0' at end */

		/* Read leap seconds, discarding those out of pg_time_t range.  */
		leapcnt = 0;
		for (i = 0; i < sp->leapcnt; ++i)
		{
			int64		tr = stored == 4 ? detzcode(p) : detzcode64(p);
			int32		corr = detzcode(p + stored);

			p += stored + 4;
			if (tr <= time_t_max)
			{
				pg_time_t	trans
				= ((TYPE_SIGNED(pg_time_t) ? tr < time_t_min : tr < 0)
				   ? time_t_min : tr);

				if (leapcnt && trans <= sp->lsis[leapcnt - 1].ls_trans)
				{
					if (trans < sp->lsis[leapcnt - 1].ls_trans)
						return EINVAL;
					leapcnt--;
				}
				sp->lsis[leapcnt].ls_trans = trans;
				sp->lsis[leapcnt].ls_corr = corr;
				leapcnt++;
			}
		}
		sp->leapcnt = leapcnt;

		for (i = 0; i < sp->typecnt; ++i)
		{
			struct ttinfo *ttisp;

			ttisp = &sp->ttis[i];
			if (ttisstdcnt == 0)
				ttisp->tt_ttisstd = false;
			else
			{
				if (*p != true && *p != false)
					return EINVAL;
				ttisp->tt_ttisstd = *p++;
			}
		}
		for (i = 0; i < sp->typecnt; ++i)
		{
			struct ttinfo *ttisp;

			ttisp = &sp->ttis[i];
			if (ttisgmtcnt == 0)
				ttisp->tt_ttisgmt = false;
			else
			{
				if (*p != true && *p != false)
					return EINVAL;
				ttisp->tt_ttisgmt = *p++;
			}
		}

		/*
		 * If this is an old file, we're done.
		 */
		if (up->tzhead.tzh_version[0] == '\0')
			break;
		nread -= p - up->buf;
		memmove(up->buf, p, nread);
	}
	if (doextend && nread > 2 &&
		up->buf[0] == '\n' && up->buf[nread - 1] == '\n' &&
		sp->typecnt + 2 <= TZ_MAX_TYPES)
	{
		struct state *ts = &lsp->u.st;

		up->buf[nread - 1] = '\0';
		if (tzparse(&up->buf[1], ts, false)
			&& ts->typecnt == 2)
		{
			/*
			 * Attempt to reuse existing abbreviations. Without this,
			 * America/Anchorage would stop working after 2037 when
			 * TZ_MAX_CHARS is 50, as sp->charcnt equals 42 (for LMT CAT CAWT
			 * CAPT AHST AHDT YST AKDT AKST) and ts->charcnt equals 10 (for
			 * AKST AKDT).  Reusing means sp->charcnt can stay 42 in this
			 * example.
			 */
			int			gotabbr = 0;
			int			charcnt = sp->charcnt;

			for (i = 0; i < 2; i++)
			{
				char	   *tsabbr = ts->chars + ts->ttis[i].tt_abbrind;
				int			j;

				for (j = 0; j < charcnt; j++)
					if (strcmp(sp->chars + j, tsabbr) == 0)
					{
						ts->ttis[i].tt_abbrind = j;
						gotabbr++;
						break;
					}
				if (!(j < charcnt))
				{
					int			tsabbrlen = strlen(tsabbr);

					if (j + tsabbrlen < TZ_MAX_CHARS)
					{
						strcpy(sp->chars + j, tsabbr);
						charcnt = j + tsabbrlen + 1;
						ts->ttis[i].tt_abbrind = j;
						gotabbr++;
					}
				}
			}
			if (gotabbr == 2)
			{
				sp->charcnt = charcnt;
				for (i = 0; i < ts->timecnt; i++)
					if (sp->ats[sp->timecnt - 1] < ts->ats[i])
						break;
				while (i < ts->timecnt
					   && sp->timecnt < TZ_MAX_TIMES)
				{
					sp->ats[sp->timecnt] = ts->ats[i];
					sp->types[sp->timecnt] = (sp->typecnt
											  + ts->types[i]);
					sp->timecnt++;
					i++;
				}
				sp->ttis[sp->typecnt++] = ts->ttis[0];
				sp->ttis[sp->typecnt++] = ts->ttis[1];
			}
		}
	}
	if (sp->timecnt > 1)
	{
		for (i = 1; i < sp->timecnt; ++i)
			if (typesequiv(sp, sp->types[i], sp->types[0]) &&
				differ_by_repeat(sp->ats[i], sp->ats[0]))
			{
				sp->goback = true;
				break;
			}
		for (i = sp->timecnt - 2; i >= 0; --i)
			if (typesequiv(sp, sp->types[sp->timecnt - 1],
						   sp->types[i]) &&
				differ_by_repeat(sp->ats[sp->timecnt - 1],
								 sp->ats[i]))
			{
				sp->goahead = true;
				break;
			}
	}

	/*
	 * If type 0 is unused in transitions, it's the type to use for early
	 * times.
	 */
	for (i = 0; i < sp->timecnt; ++i)
		if (sp->types[i] == 0)
			break;
	i = i < sp->timecnt ? -1 : 0;

	/*
	 * Absent the above, if there are transition times and the first
	 * transition is to a daylight time find the standard type less than and
	 * closest to the type of the first transition.
	 */
	if (i < 0 && sp->timecnt > 0 && sp->ttis[sp->types[0]].tt_isdst)
	{
		i = sp->types[0];
		while (--i >= 0)
			if (!sp->ttis[i].tt_isdst)
				break;
	}

	/*
	 * If no result yet, find the first standard type. If there is none, punt
	 * to type zero.
	 */
	if (i < 0)
	{
		i = 0;
		while (sp->ttis[i].tt_isdst)
			if (++i >= sp->typecnt)
			{
				i = 0;
				break;
			}
	}
	sp->defaulttype = i;
	return 0;
}

/* Load tz data from the file named NAME into *SP.  Read extended
 * format if DOEXTEND.  Return 0 on success, an errno value on failure.
 * PG: If "canonname" is not NULL, then on success the canonical spelling of
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 */
int
tzload(const char *name, char *canonname, struct state * sp, bool doextend)
{
	union local_storage *lsp = malloc(sizeof *lsp);

	if (!lsp)
		return errno;
	else
	{
		int			err = tzloadbody(name, canonname, sp, doextend, lsp);

		free(lsp);
		return err;
	}
}

static bool
typesequiv(const struct state * sp, int a, int b)
{
	bool		result;

	if (sp == NULL ||
		a < 0 || a >= sp->typecnt ||
		b < 0 || b >= sp->typecnt)
		result = false;
	else
	{
		const struct ttinfo *ap = &sp->ttis[a];
		const struct ttinfo *bp = &sp->ttis[b];

		result = ap->tt_gmtoff == bp->tt_gmtoff &&
			ap->tt_isdst == bp->tt_isdst &&
			ap->tt_ttisstd == bp->tt_ttisstd &&
			ap->tt_ttisgmt == bp->tt_ttisgmt &&
			strcmp(&sp->chars[ap->tt_abbrind],
				   &sp->chars[bp->tt_abbrind]) == 0;
	}
	return result;
}

static const int mon_lengths[2][MONSPERYEAR] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
	{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

static const int year_lengths[2] = {
	DAYSPERNYEAR, DAYSPERLYEAR
};

/*
 * Given a pointer into a time zone string, scan until a character that is not
 * a valid character in a zone name is found. Return a pointer to that
 * character.
 */
static const char *
getzname(const char *strp)
{
	char		c;

	while ((c = *strp) != '\0' && !is_digit(c) && c != ',' && c != '-' &&
		   c != '+')
		++strp;
	return strp;
}

/*
 * Given a pointer into an extended time zone string, scan until the ending
 * delimiter of the zone name is located. Return a pointer to the delimiter.
 *
 * As with getzname above, the legal character set is actually quite
 * restricted, with other characters producing undefined results.
 * We don't do any checking here; checking is done later in common-case code.
 */
static const char *
getqzname(const char *strp, int delim)
{
	int			c;

	while ((c = *strp) != '\0' && c != delim)
		++strp;
	return strp;
}

/*
 * Given a pointer into a time zone string, extract a number from that string.
 * Check that the number is within a specified range; if it is not, return
 * NULL.
 * Otherwise, return a pointer to the first character not part of the number.
 */
static const char *
getnum(const char *strp, int *nump, int min, int max)
{
	char		c;
	int			num;

	if (strp == NULL || !is_digit(c = *strp))
		return NULL;
	num = 0;
	do
	{
		num = num * 10 + (c - '0');
		if (num > max)
			return NULL;		/* illegal value */
		c = *++strp;
	} while (is_digit(c));
	if (num < min)
		return NULL;			/* illegal value */
	*nump = num;
	return strp;
}

/*
 * Given a pointer into a time zone string, extract a number of seconds,
 * in hh[:mm[:ss]] form, from the string.
 * If any error occurs, return NULL.
 * Otherwise, return a pointer to the first character not part of the number
 * of seconds.
 */
static const char *
getsecs(const char *strp, int32 *secsp)
{
	int			num;

	/*
	 * 'HOURSPERDAY * DAYSPERWEEK - 1' allows quasi-Posix rules like
	 * "M10.4.6/26", which does not conform to Posix, but which specifies the
	 * equivalent of "02:00 on the first Sunday on or after 23 Oct".
	 */
	strp = getnum(strp, &num, 0, HOURSPERDAY * DAYSPERWEEK - 1);
	if (strp == NULL)
		return NULL;
	*secsp = num * (int32) SECSPERHOUR;
	if (*strp == ':')
	{
		++strp;
		strp = getnum(strp, &num, 0, MINSPERHOUR - 1);
		if (strp == NULL)
			return NULL;
		*secsp += num * SECSPERMIN;
		if (*strp == ':')
		{
			++strp;
			/* 'SECSPERMIN' allows for leap seconds.  */
			strp = getnum(strp, &num, 0, SECSPERMIN);
			if (strp == NULL)
				return NULL;
			*secsp += num;
		}
	}
	return strp;
}

/*
 * Given a pointer into a time zone string, extract an offset, in
 * [+-]hh[:mm[:ss]] form, from the string.
 * If any error occurs, return NULL.
 * Otherwise, return a pointer to the first character not part of the time.
 */
static const char *
getoffset(const char *strp, int32 *offsetp)
{
	bool		neg = false;

	if (*strp == '-')
	{
		neg = true;
		++strp;
	}
	else if (*strp == '+')
		++strp;
	strp = getsecs(strp, offsetp);
	if (strp == NULL)
		return NULL;			/* illegal time */
	if (neg)
		*offsetp = -*offsetp;
	return strp;
}

/*
 * Given a pointer into a time zone string, extract a rule in the form
 * date[/time]. See POSIX section 8 for the format of "date" and "time".
 * If a valid rule is not found, return NULL.
 * Otherwise, return a pointer to the first character not part of the rule.
 */
static const char *
getrule(const char *strp, struct rule * rulep)
{
	if (*strp == 'J')
	{
		/*
		 * Julian day.
		 */
		rulep->r_type = JULIAN_DAY;
		++strp;
		strp = getnum(strp, &rulep->r_day, 1, DAYSPERNYEAR);
	}
	else if (*strp == 'M')
	{
		/*
		 * Month, week, day.
		 */
		rulep->r_type = MONTH_NTH_DAY_OF_WEEK;
		++strp;
		strp = getnum(strp, &rulep->r_mon, 1, MONSPERYEAR);
		if (strp == NULL)
			return NULL;
		if (*strp++ != '.')
			return NULL;
		strp = getnum(strp, &rulep->r_week, 1, 5);
		if (strp == NULL)
			return NULL;
		if (*strp++ != '.')
			return NULL;
		strp = getnum(strp, &rulep->r_day, 0, DAYSPERWEEK - 1);
	}
	else if (is_digit(*strp))
	{
		/*
		 * Day of year.
		 */
		rulep->r_type = DAY_OF_YEAR;
		strp = getnum(strp, &rulep->r_day, 0, DAYSPERLYEAR - 1);
	}
	else
		return NULL;			/* invalid format */
	if (strp == NULL)
		return NULL;
	if (*strp == '/')
	{
		/*
		 * Time specified.
		 */
		++strp;
		strp = getoffset(strp, &rulep->r_time);
	}
	else
		rulep->r_time = 2 * SECSPERHOUR;		/* default = 2:00:00 */
	return strp;
}

/*
 * Given a year, a rule, and the offset from UT at the time that rule takes
 * effect, calculate the year-relative time that rule takes effect.
 */
static int32
transtime(int year, const struct rule * rulep,
		  int32 offset)
{
	bool		leapyear;
	int32		value;
	int			i,
				d,
				m1,
				yy0,
				yy1,
				yy2,
				dow;

	INITIALIZE(value);
	leapyear = isleap(year);
	switch (rulep->r_type)
	{

		case JULIAN_DAY:

			/*
			 * Jn - Julian day, 1 == January 1, 60 == March 1 even in leap
			 * years. In non-leap years, or if the day number is 59 or less,
			 * just add SECSPERDAY times the day number-1 to the time of
			 * January 1, midnight, to get the day.
			 */
			value = (rulep->r_day - 1) * SECSPERDAY;
			if (leapyear && rulep->r_day >= 60)
				value += SECSPERDAY;
			break;

		case DAY_OF_YEAR:

			/*
			 * n - day of year. Just add SECSPERDAY times the day number to
			 * the time of January 1, midnight, to get the day.
			 */
			value = rulep->r_day * SECSPERDAY;
			break;

		case MONTH_NTH_DAY_OF_WEEK:

			/*
			 * Mm.n.d - nth "dth day" of month m.
			 */

			/*
			 * Use Zeller's Congruence to get day-of-week of first day of
			 * month.
			 */
			m1 = (rulep->r_mon + 9) % 12 + 1;
			yy0 = (rulep->r_mon <= 2) ? (year - 1) : year;
			yy1 = yy0 / 100;
			yy2 = yy0 % 100;
			dow = ((26 * m1 - 2) / 10 +
				   1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
			if (dow < 0)
				dow += DAYSPERWEEK;

			/*
			 * "dow" is the day-of-week of the first day of the month. Get the
			 * day-of-month (zero-origin) of the first "dow" day of the month.
			 */
			d = rulep->r_day - dow;
			if (d < 0)
				d += DAYSPERWEEK;
			for (i = 1; i < rulep->r_week; ++i)
			{
				if (d + DAYSPERWEEK >=
					mon_lengths[(int) leapyear][rulep->r_mon - 1])
					break;
				d += DAYSPERWEEK;
			}

			/*
			 * "d" is the day-of-month (zero-origin) of the day we want.
			 */
			value = d * SECSPERDAY;
			for (i = 0; i < rulep->r_mon - 1; ++i)
				value += mon_lengths[(int) leapyear][i] * SECSPERDAY;
			break;
	}

	/*
	 * "value" is the year-relative time of 00:00:00 UT on the day in
	 * question. To get the year-relative time of the specified local time on
	 * that day, add the transition time and the current offset from UT.
	 */
	return value + rulep->r_time + offset;
}

/*
 * Given a POSIX section 8-style TZ string, fill in the rule tables as
 * appropriate.
 * Returns true on success, false on failure.
 */
bool
tzparse(const char *name, struct state * sp, bool lastditch)
{
	const char *stdname;
	const char *dstname = NULL;
	size_t		stdlen;
	size_t		dstlen;
	size_t		charcnt;
	int32		stdoffset;
	int32		dstoffset;
	char	   *cp;
	bool		load_ok;

	stdname = name;
	if (lastditch)
	{
		/*
		 * This is intentionally somewhat different from the IANA code.  We do
		 * not want to invoke tzload() in the lastditch case: we can't assume
		 * pg_open_tzfile() is sane yet, and we don't care about leap seconds
		 * anyway.
		 */
		stdlen = strlen(name);	/* length of standard zone name */
		name += stdlen;
		if (stdlen >= sizeof sp->chars)
			stdlen = (sizeof sp->chars) - 1;
		charcnt = stdlen + 1;
		stdoffset = 0;
		sp->goback = sp->goahead = false;		/* simulate failed tzload() */
		load_ok = false;
	}
	else
	{
		if (*name == '<')
		{
			name++;
			stdname = name;
			name = getqzname(name, '>');
			if (*name != '>')
				return false;
			stdlen = name - stdname;
			name++;
		}
		else
		{
			name = getzname(name);
			stdlen = name - stdname;
		}
		if (*name == '\0')		/* we allow empty STD abbrev, unlike IANA */
			return false;
		name = getoffset(name, &stdoffset);
		if (name == NULL)
			return false;
		charcnt = stdlen + 1;
		if (sizeof sp->chars < charcnt)
			return false;
		load_ok = tzload(TZDEFRULES, NULL, sp, false) == 0;
	}
	if (!load_ok)
		sp->leapcnt = 0;		/* so, we're off a little */
	if (*name != '\0')
	{
		if (*name == '<')
		{
			dstname = ++name;
			name = getqzname(name, '>');
			if (*name != '>')
				return false;
			dstlen = name - dstname;
			name++;
		}
		else
		{
			dstname = name;
			name = getzname(name);
			dstlen = name - dstname;	/* length of DST zone name */
		}
		if (!dstlen)
			return false;
		charcnt += dstlen + 1;
		if (sizeof sp->chars < charcnt)
			return false;
		if (*name != '\0' && *name != ',' && *name != ';')
		{
			name = getoffset(name, &dstoffset);
			if (name == NULL)
				return false;
		}
		else
			dstoffset = stdoffset - SECSPERHOUR;
		if (*name == '\0' && !load_ok)
			name = TZDEFRULESTRING;
		if (*name == ',' || *name == ';')
		{
			struct rule start;
			struct rule end;
			int			year;
			int			yearlim;
			int			timecnt;
			pg_time_t	janfirst;

			++name;
			if ((name = getrule(name, &start)) == NULL)
				return false;
			if (*name++ != ',')
				return false;
			if ((name = getrule(name, &end)) == NULL)
				return false;
			if (*name != '\0')
				return false;
			sp->typecnt = 2;	/* standard time and DST */

			/*
			 * Two transitions per year, from EPOCH_YEAR forward.
			 */
			init_ttinfo(&sp->ttis[0], -dstoffset, true, stdlen + 1);
			init_ttinfo(&sp->ttis[1], -stdoffset, false, 0);
			sp->defaulttype = 0;
			timecnt = 0;
			janfirst = 0;
			yearlim = EPOCH_YEAR + YEARSPERREPEAT;
			for (year = EPOCH_YEAR; year < yearlim; year++)
			{
				int32
							starttime = transtime(year, &start, stdoffset),
							endtime = transtime(year, &end, dstoffset);
				int32
							yearsecs = (year_lengths[isleap(year)]
										* SECSPERDAY);
				bool		reversed = endtime < starttime;

				if (reversed)
				{
					int32		swap = starttime;

					starttime = endtime;
					endtime = swap;
				}
				if (reversed
					|| (starttime < endtime
						&& (endtime - starttime
							< (yearsecs
							   + (stdoffset - dstoffset)))))
				{
					if (TZ_MAX_TIMES - 2 < timecnt)
						break;
					yearlim = year + YEARSPERREPEAT + 1;
					sp->ats[timecnt] = janfirst;
					if (increment_overflow_time
						(&sp->ats[timecnt], starttime))
						break;
					sp->types[timecnt++] = reversed;
					sp->ats[timecnt] = janfirst;
					if (increment_overflow_time
						(&sp->ats[timecnt], endtime))
						break;
					sp->types[timecnt++] = !reversed;
				}
				if (increment_overflow_time(&janfirst, yearsecs))
					break;
			}
			sp->timecnt = timecnt;
			if (!timecnt)
				sp->typecnt = 1;	/* Perpetual DST.  */
		}
		else
		{
			int32		theirstdoffset;
			int32		theirdstoffset;
			int32		theiroffset;
			bool		isdst;
			int			i;
			int			j;

			if (*name != '\0')
				return false;

			/*
			 * Initial values of theirstdoffset and theirdstoffset.
			 */
			theirstdoffset = 0;
			for (i = 0; i < sp->timecnt; ++i)
			{
				j = sp->types[i];
				if (!sp->ttis[j].tt_isdst)
				{
					theirstdoffset =
						-sp->ttis[j].tt_gmtoff;
					break;
				}
			}
			theirdstoffset = 0;
			for (i = 0; i < sp->timecnt; ++i)
			{
				j = sp->types[i];
				if (sp->ttis[j].tt_isdst)
				{
					theirdstoffset =
						-sp->ttis[j].tt_gmtoff;
					break;
				}
			}

			/*
			 * Initially we're assumed to be in standard time.
			 */
			isdst = false;
			theiroffset = theirstdoffset;

			/*
			 * Now juggle transition times and types tracking offsets as you
			 * do.
			 */
			for (i = 0; i < sp->timecnt; ++i)
			{
				j = sp->types[i];
				sp->types[i] = sp->ttis[j].tt_isdst;
				if (sp->ttis[j].tt_ttisgmt)
				{
					/* No adjustment to transition time */
				}
				else
				{
					/*
					 * If summer time is in effect, and the transition time
					 * was not specified as standard time, add the summer time
					 * offset to the transition time; otherwise, add the
					 * standard time offset to the transition time.
					 */

					/*
					 * Transitions from DST to DDST will effectively disappear
					 * since POSIX provides for only one DST offset.
					 */
					if (isdst && !sp->ttis[j].tt_ttisstd)
					{
						sp->ats[i] += dstoffset -
							theirdstoffset;
					}
					else
					{
						sp->ats[i] += stdoffset -
							theirstdoffset;
					}
				}
				theiroffset = -sp->ttis[j].tt_gmtoff;
				if (sp->ttis[j].tt_isdst)
					theirdstoffset = theiroffset;
				else
					theirstdoffset = theiroffset;
			}

			/*
			 * Finally, fill in ttis.
			 */
			init_ttinfo(&sp->ttis[0], -stdoffset, false, 0);
			init_ttinfo(&sp->ttis[1], -dstoffset, true, stdlen + 1);
			sp->typecnt = 2;
			sp->defaulttype = 0;
		}
	}
	else
	{
		dstlen = 0;
		sp->typecnt = 1;		/* only standard time */
		sp->timecnt = 0;
		init_ttinfo(&sp->ttis[0], -stdoffset, false, 0);
		sp->defaulttype = 0;
	}
	sp->charcnt = charcnt;
	cp = sp->chars;
	memcpy(cp, stdname, stdlen);
	cp += stdlen;
	*cp++ = '\0';
	if (dstlen != 0)
	{
		memcpy(cp, dstname, dstlen);
		*(cp + dstlen) = '\0';
	}
	return true;
}

static void
gmtload(struct state * sp)
{
	if (tzload(gmt, NULL, sp, true) != 0)
		tzparse(gmt, sp, true);
}


/*
 * The easy way to behave "as if no library function calls" localtime
 * is to not call it, so we drop its guts into "localsub", which can be
 * freely called. (And no, the PANS doesn't require the above behavior,
 * but it *is* desirable.)
 */
static struct pg_tm *
localsub(struct state const * sp, pg_time_t const * timep,
		 struct pg_tm * tmp)
{
	const struct ttinfo *ttisp;
	int			i;
	struct pg_tm *result;
	const pg_time_t t = *timep;

	if (sp == NULL)
		return gmtsub(timep, 0, tmp);
	if ((sp->goback && t < sp->ats[0]) ||
		(sp->goahead && t > sp->ats[sp->timecnt - 1]))
	{
		pg_time_t	newt = t;
		pg_time_t	seconds;
		pg_time_t	years;

		if (t < sp->ats[0])
			seconds = sp->ats[0] - t;
		else
			seconds = t - sp->ats[sp->timecnt - 1];
		--seconds;
		years = (seconds / SECSPERREPEAT + 1) * YEARSPERREPEAT;
		seconds = years * AVGSECSPERYEAR;
		if (t < sp->ats[0])
			newt += seconds;
		else
			newt -= seconds;
		if (newt < sp->ats[0] ||
			newt > sp->ats[sp->timecnt - 1])
			return NULL;		/* "cannot happen" */
		result = localsub(sp, &newt, tmp);
		if (result)
		{
			int64		newy;

			newy = result->tm_year;
			if (t < sp->ats[0])
				newy -= years;
			else
				newy += years;
			if (!(INT_MIN <= newy && newy <= INT_MAX))
				return NULL;
			result->tm_year = newy;
		}
		return result;
	}
	if (sp->timecnt == 0 || t < sp->ats[0])
	{
		i = sp->defaulttype;
	}
	else
	{
		int			lo = 1;
		int			hi = sp->timecnt;

		while (lo < hi)
		{
			int			mid = (lo + hi) >> 1;

			if (t < sp->ats[mid])
				hi = mid;
			else
				lo = mid + 1;
		}
		i = (int) sp->types[lo - 1];
	}
	ttisp = &sp->ttis[i];

	result = timesub(&t, ttisp->tt_gmtoff, sp, tmp);
	if (result)
	{
		result->tm_isdst = ttisp->tt_isdst;
		result->tm_zone = (char *) &sp->chars[ttisp->tt_abbrind];
	}
	return result;
}


struct pg_tm *
pg_localtime(const pg_time_t *timep, const pg_tz *tz)
{
	return localsub(&tz->state, timep, &tm);
}


/*
 * gmtsub is to gmtime as localsub is to localtime.
 *
 * Except we have a private "struct state" for GMT, so no sp is passed in.
 */
static struct pg_tm *
gmtsub(pg_time_t const * timep, int32 offset, struct pg_tm * tmp)
{
	struct pg_tm *result;

	/* GMT timezone state data is kept here */
	static struct state gmtmem;
	static bool gmt_is_set = false;
#define gmtptr		(&gmtmem)

	if (!gmt_is_set)
	{
		gmt_is_set = true;
		gmtload(gmtptr);
	}
	result = timesub(timep, offset, gmtptr, tmp);

	/*
	 * Could get fancy here and deliver something such as "UT+xxxx" or
	 * "UT-xxxx" if offset is non-zero, but this is no time for a treasure
	 * hunt.
	 */
	if (offset != 0)
		tmp->tm_zone = wildabbr;
	else
		tmp->tm_zone = gmtptr->chars;

	return result;
}

struct pg_tm *
pg_gmtime(const pg_time_t *timep)
{
	return gmtsub(timep, 0, &tm);
}

/*
 * Return the number of leap years through the end of the given year
 * where, to make the math easy, the answer for year zero is defined as zero.
 */
static int
leaps_thru_end_of(const int y)
{
	return (y >= 0) ? (y / 4 - y / 100 + y / 400) :
		-(leaps_thru_end_of(-(y + 1)) + 1);
}

static struct pg_tm *
timesub(const pg_time_t *timep, int32 offset,
		const struct state * sp, struct pg_tm * tmp)
{
	const struct lsinfo *lp;
	pg_time_t	tdays;
	int			idays;			/* unsigned would be so 2003 */
	int64		rem;
	int			y;
	const int  *ip;
	int64		corr;
	bool		hit;
	int			i;

	corr = 0;
	hit = false;
	i = (sp == NULL) ? 0 : sp->leapcnt;
	while (--i >= 0)
	{
		lp = &sp->lsis[i];
		if (*timep >= lp->ls_trans)
		{
			if (*timep == lp->ls_trans)
			{
				hit = ((i == 0 && lp->ls_corr > 0) ||
					   lp->ls_corr > sp->lsis[i - 1].ls_corr);
				if (hit)
					while (i > 0 &&
						   sp->lsis[i].ls_trans ==
						   sp->lsis[i - 1].ls_trans + 1 &&
						   sp->lsis[i].ls_corr ==
						   sp->lsis[i - 1].ls_corr + 1)
					{
						++hit;
						--i;
					}
			}
			corr = lp->ls_corr;
			break;
		}
	}
	y = EPOCH_YEAR;
	tdays = *timep / SECSPERDAY;
	rem = *timep % SECSPERDAY;
	while (tdays < 0 || tdays >= year_lengths[isleap(y)])
	{
		int			newy;
		pg_time_t	tdelta;
		int			idelta;
		int			leapdays;

		tdelta = tdays / DAYSPERLYEAR;
		if (!((!TYPE_SIGNED(pg_time_t) ||INT_MIN <= tdelta)
			  && tdelta <= INT_MAX))
			goto out_of_range;
		idelta = tdelta;
		if (idelta == 0)
			idelta = (tdays < 0) ? -1 : 1;
		newy = y;
		if (increment_overflow(&newy, idelta))
			goto out_of_range;
		leapdays = leaps_thru_end_of(newy - 1) -
			leaps_thru_end_of(y - 1);
		tdays -= ((pg_time_t) newy - y) * DAYSPERNYEAR;
		tdays -= leapdays;
		y = newy;
	}

	/*
	 * Given the range, we can now fearlessly cast...
	 */
	idays = tdays;
	rem += offset - corr;
	while (rem < 0)
	{
		rem += SECSPERDAY;
		--idays;
	}
	while (rem >= SECSPERDAY)
	{
		rem -= SECSPERDAY;
		++idays;
	}
	while (idays < 0)
	{
		if (increment_overflow(&y, -1))
			goto out_of_range;
		idays += year_lengths[isleap(y)];
	}
	while (idays >= year_lengths[isleap(y)])
	{
		idays -= year_lengths[isleap(y)];
		if (increment_overflow(&y, 1))
			goto out_of_range;
	}
	tmp->tm_year = y;
	if (increment_overflow(&tmp->tm_year, -TM_YEAR_BASE))
		goto out_of_range;
	tmp->tm_yday = idays;

	/*
	 * The "extra" mods below avoid overflow problems.
	 */
	tmp->tm_wday = EPOCH_WDAY +
		((y - EPOCH_YEAR) % DAYSPERWEEK) *
		(DAYSPERNYEAR % DAYSPERWEEK) +
		leaps_thru_end_of(y - 1) -
		leaps_thru_end_of(EPOCH_YEAR - 1) +
		idays;
	tmp->tm_wday %= DAYSPERWEEK;
	if (tmp->tm_wday < 0)
		tmp->tm_wday += DAYSPERWEEK;
	tmp->tm_hour = (int) (rem / SECSPERHOUR);
	rem %= SECSPERHOUR;
	tmp->tm_min = (int) (rem / SECSPERMIN);

	/*
	 * A positive leap second requires a special representation. This uses
	 * "... ??:59:60" et seq.
	 */
	tmp->tm_sec = (int) (rem % SECSPERMIN) + hit;
	ip = mon_lengths[isleap(y)];
	for (tmp->tm_mon = 0; idays >= ip[tmp->tm_mon]; ++(tmp->tm_mon))
		idays -= ip[tmp->tm_mon];
	tmp->tm_mday = (int) (idays + 1);
	tmp->tm_isdst = 0;
	tmp->tm_gmtoff = offset;
	return tmp;

out_of_range:
	errno = EOVERFLOW;
	return NULL;
}

/*
 * Normalize logic courtesy Paul Eggert.
 */

static bool
increment_overflow(int *ip, int j)
{
	int const	i = *ip;

	/*----------
	 * If i >= 0 there can only be overflow if i + j > INT_MAX
	 * or if j > INT_MAX - i; given i >= 0, INT_MAX - i cannot overflow.
	 * If i < 0 there can only be overflow if i + j < INT_MIN
	 * or if j < INT_MIN - i; given i < 0, INT_MIN - i cannot overflow.
	 *----------
	 */
	if ((i >= 0) ? (j > INT_MAX - i) : (j < INT_MIN - i))
		return true;
	*ip += j;
	return false;
}

static bool
increment_overflow_time(pg_time_t *tp, int32 j)
{
	/*----------
	 * This is like
	 * 'if (! (time_t_min <= *tp + j && *tp + j <= time_t_max)) ...',
	 * except that it does the right thing even if *tp + j would overflow.
	 *----------
	 */
	if (!(j < 0
		  ? (TYPE_SIGNED(pg_time_t) ? time_t_min - j <= *tp : -1 - j < *tp)
		  : *tp <= time_t_max - j))
		return true;
	*tp += j;
	return false;
}

/*
 * Find the next DST transition time in the given zone after the given time
 *
 * *timep and *tz are input arguments, the other parameters are output values.
 *
 * When the function result is 1, *boundary is set to the pg_time_t
 * representation of the next DST transition time after *timep,
 * *before_gmtoff and *before_isdst are set to the GMT offset and isdst
 * state prevailing just before that boundary (in particular, the state
 * prevailing at *timep), and *after_gmtoff and *after_isdst are set to
 * the state prevailing just after that boundary.
 *
 * When the function result is 0, there is no known DST transition
 * after *timep, but *before_gmtoff and *before_isdst indicate the GMT
 * offset and isdst state prevailing at *timep.  (This would occur in
 * DST-less time zones, or if a zone has permanently ceased using DST.)
 *
 * A function result of -1 indicates failure (this case does not actually
 * occur in our current implementation).
 */
int
pg_next_dst_boundary(const pg_time_t *timep,
					 long int *before_gmtoff,
					 int *before_isdst,
					 pg_time_t *boundary,
					 long int *after_gmtoff,
					 int *after_isdst,
					 const pg_tz *tz)
{
	const struct state *sp;
	const struct ttinfo *ttisp;
	int			i;
	int			j;
	const pg_time_t t = *timep;

	sp = &tz->state;
	if (sp->timecnt == 0)
	{
		/* non-DST zone, use lowest-numbered standard type */
		i = 0;
		while (sp->ttis[i].tt_isdst)
			if (++i >= sp->typecnt)
			{
				i = 0;
				break;
			}
		ttisp = &sp->ttis[i];
		*before_gmtoff = ttisp->tt_gmtoff;
		*before_isdst = ttisp->tt_isdst;
		return 0;
	}
	if ((sp->goback && t < sp->ats[0]) ||
		(sp->goahead && t > sp->ats[sp->timecnt - 1]))
	{
		/* For values outside the transition table, extrapolate */
		pg_time_t	newt = t;
		pg_time_t	seconds;
		pg_time_t	tcycles;
		int64		icycles;
		int			result;

		if (t < sp->ats[0])
			seconds = sp->ats[0] - t;
		else
			seconds = t - sp->ats[sp->timecnt - 1];
		--seconds;
		tcycles = seconds / YEARSPERREPEAT / AVGSECSPERYEAR;
		++tcycles;
		icycles = tcycles;
		if (tcycles - icycles >= 1 || icycles - tcycles >= 1)
			return -1;
		seconds = icycles;
		seconds *= YEARSPERREPEAT;
		seconds *= AVGSECSPERYEAR;
		if (t < sp->ats[0])
			newt += seconds;
		else
			newt -= seconds;
		if (newt < sp->ats[0] ||
			newt > sp->ats[sp->timecnt - 1])
			return -1;			/* "cannot happen" */

		result = pg_next_dst_boundary(&newt, before_gmtoff,
									  before_isdst,
									  boundary,
									  after_gmtoff,
									  after_isdst,
									  tz);
		if (t < sp->ats[0])
			*boundary -= seconds;
		else
			*boundary += seconds;
		return result;
	}

	if (t >= sp->ats[sp->timecnt - 1])
	{
		/* No known transition > t, so use last known segment's type */
		i = sp->types[sp->timecnt - 1];
		ttisp = &sp->ttis[i];
		*before_gmtoff = ttisp->tt_gmtoff;
		*before_isdst = ttisp->tt_isdst;
		return 0;
	}
	if (t < sp->ats[0])
	{
		/* For "before", use lowest-numbered standard type */
		i = 0;
		while (sp->ttis[i].tt_isdst)
			if (++i >= sp->typecnt)
			{
				i = 0;
				break;
			}
		ttisp = &sp->ttis[i];
		*before_gmtoff = ttisp->tt_gmtoff;
		*before_isdst = ttisp->tt_isdst;
		*boundary = sp->ats[0];
		/* And for "after", use the first segment's type */
		i = sp->types[0];
		ttisp = &sp->ttis[i];
		*after_gmtoff = ttisp->tt_gmtoff;
		*after_isdst = ttisp->tt_isdst;
		return 1;
	}
	/* Else search to find the boundary following t */
	{
		int			lo = 1;
		int			hi = sp->timecnt - 1;

		while (lo < hi)
		{
			int			mid = (lo + hi) >> 1;

			if (t < sp->ats[mid])
				hi = mid;
			else
				lo = mid + 1;
		}
		i = lo;
	}
	j = sp->types[i - 1];
	ttisp = &sp->ttis[j];
	*before_gmtoff = ttisp->tt_gmtoff;
	*before_isdst = ttisp->tt_isdst;
	*boundary = sp->ats[i];
	j = sp->types[i];
	ttisp = &sp->ttis[j];
	*after_gmtoff = ttisp->tt_gmtoff;
	*after_isdst = ttisp->tt_isdst;
	return 1;
}

/*
 * Identify a timezone abbreviation's meaning in the given zone
 *
 * Determine the GMT offset and DST flag associated with the abbreviation.
 * This is generally used only when the abbreviation has actually changed
 * meaning over time; therefore, we also take a UTC cutoff time, and return
 * the meaning in use at or most recently before that time, or the meaning
 * in first use after that time if the abbrev was never used before that.
 *
 * On success, returns true and sets *gmtoff and *isdst.  If the abbreviation
 * was never used at all in this zone, returns false.
 *
 * Note: abbrev is matched case-sensitively; it should be all-upper-case.
 */
bool
pg_interpret_timezone_abbrev(const char *abbrev,
							 const pg_time_t *timep,
							 long int *gmtoff,
							 int *isdst,
							 const pg_tz *tz)
{
	const struct state *sp;
	const char *abbrs;
	const struct ttinfo *ttisp;
	int			abbrind;
	int			cutoff;
	int			i;
	const pg_time_t t = *timep;

	sp = &tz->state;

	/*
	 * Locate the abbreviation in the zone's abbreviation list.  We assume
	 * there are not duplicates in the list.
	 */
	abbrs = sp->chars;
	abbrind = 0;
	while (abbrind < sp->charcnt)
	{
		if (strcmp(abbrev, abbrs + abbrind) == 0)
			break;
		while (abbrs[abbrind] != '\0')
			abbrind++;
		abbrind++;
	}
	if (abbrind >= sp->charcnt)
		return false;			/* not there! */

	/*
	 * Unlike pg_next_dst_boundary, we needn't sweat about extrapolation
	 * (goback/goahead zones).  Finding the newest or oldest meaning of the
	 * abbreviation should get us what we want, since extrapolation would just
	 * be repeating the newest or oldest meanings.
	 *
	 * Use binary search to locate the first transition > cutoff time.
	 */
	{
		int			lo = 0;
		int			hi = sp->timecnt;

		while (lo < hi)
		{
			int			mid = (lo + hi) >> 1;

			if (t < sp->ats[mid])
				hi = mid;
			else
				lo = mid + 1;
		}
		cutoff = lo;
	}

	/*
	 * Scan backwards to find the latest interval using the given abbrev
	 * before the cutoff time.
	 */
	for (i = cutoff - 1; i >= 0; i--)
	{
		ttisp = &sp->ttis[sp->types[i]];
		if (ttisp->tt_abbrind == abbrind)
		{
			*gmtoff = ttisp->tt_gmtoff;
			*isdst = ttisp->tt_isdst;
			return true;
		}
	}

	/*
	 * Not there, so scan forwards to find the first one after.
	 */
	for (i = cutoff; i < sp->timecnt; i++)
	{
		ttisp = &sp->ttis[sp->types[i]];
		if (ttisp->tt_abbrind == abbrind)
		{
			*gmtoff = ttisp->tt_gmtoff;
			*isdst = ttisp->tt_isdst;
			return true;
		}
	}

	return false;				/* hm, not actually used in any interval? */
}

/*
 * If the given timezone uses only one GMT offset, store that offset
 * into *gmtoff and return true, else return false.
 */
bool
pg_get_timezone_offset(const pg_tz *tz, long int *gmtoff)
{
	/*
	 * The zone could have more than one ttinfo, if it's historically used
	 * more than one abbreviation.  We return true as long as they all have
	 * the same gmtoff.
	 */
	const struct state *sp;
	int			i;

	sp = &tz->state;
	for (i = 1; i < sp->typecnt; i++)
	{
		if (sp->ttis[i].tt_gmtoff != sp->ttis[0].tt_gmtoff)
			return false;
	}
	*gmtoff = sp->ttis[0].tt_gmtoff;
	return true;
}

/*
 * Return the name of the current timezone
 */
const char *
pg_get_timezone_name(pg_tz *tz)
{
	if (tz)
		return tz->TZname;
	return NULL;
}

/*
 * Check whether timezone is acceptable.
 *
 * What we are doing here is checking for leap-second-aware timekeeping.
 * We need to reject such TZ settings because they'll wreak havoc with our
 * date/time arithmetic.
 */
bool
pg_tz_acceptable(pg_tz *tz)
{
	struct pg_tm *tt;
	pg_time_t	time2000;

	/*
	 * To detect leap-second timekeeping, run pg_localtime for what should be
	 * GMT midnight, 2000-01-01.  Insist that the tm_sec value be zero; any
	 * other result has to be due to leap seconds.
	 */
	time2000 = (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
	tt = pg_localtime(&time2000, tz);
	if (!tt || tt->tm_sec != 0)
		return false;

	return true;
}
