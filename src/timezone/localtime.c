/* Convert timestamp from pg_time_t to struct pg_tm.  */

/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson.
 *
 * IDENTIFICATION
 *	  src/timezone/localtime.c
 */

/*
 * Leap second handling from Bradley White.
 * POSIX.1-1988 style TZ environment variable handling from Guy Harris.
 */

/* this file needs to build in both frontend and backend contexts */
#include "c.h"

#include <fcntl.h>

#include "datatype/timestamp.h"
#include "pgtz.h"

#include "private.h"
#include "tzfile.h"


/*
 * Pacify gcc -Wcast-qual on char const * exprs.
 * Use this carefully, as the casts disable type checking.
 * This is a macro so that it can be used in static initializers.
 */
#define UNCONST(a) unconstify(char *, a)

#ifndef WILDABBR
/*
 * Someone might make incorrect use of a time zone abbreviation:
 *	1.	They might reference tzname[0] before calling tzset (explicitly
 *		or implicitly).
 *	2.	They might reference tzname[1] before calling tzset (explicitly
 *		or implicitly).
 *	3.	They might reference tzname[1] after setting to a time zone
 *		in which Daylight Saving Time is never observed.
 *	4.	They might reference tzname[0] after setting to a time zone
 *		in which Standard Time is never observed.
 *	5.	They might reference tm.TM_ZONE after calling offtime.
 * What's best to do in the above cases is open to debate;
 * for now, we just set things up so that in any of the five cases
 * WILDABBR is used. Another possibility: initialize tzname[0] to the
 * string "tzname[0] used before set", and similarly for the other cases.
 * And another: initialize tzname[0] to "ERA", with an explanation in the
 * manual page of what this "time zone abbreviation" means (doing this so
 * that tzname[0] has the "normal" length of three characters).
 */
#define WILDABBR "   "
#endif							/* !defined WILDABBR */

static const char wildabbr[] = WILDABBR;

/*
 * The DST rules to use if TZ has no rules.
 * Default to US rules as of 2017-05-07.
 * POSIX does not specify the default DST rules;
 * for historical reasons, US rules are a common default.
 */
#ifndef TZDEFRULESTRING
#define TZDEFRULESTRING ",M3.2.0,M11.1.0"
#endif

/* TZNAME_MAXIMUM and types ttinfo, lsinfo, state have been moved to pgtz.h */

static int
leapcount(ATTRIBUTE_MAYBE_UNUSED struct state const *sp)
{
#if TZ_RUNTIME_LEAPS
	return sp->leapcnt;
#else
	return 0;
#endif
}
static void
set_leapcount(ATTRIBUTE_MAYBE_UNUSED struct state *sp,
			  ATTRIBUTE_MAYBE_UNUSED int leapcnt)
{
#if TZ_RUNTIME_LEAPS
	sp->leapcnt = leapcnt;
#endif
}
static struct lsinfo
lsinfo(ATTRIBUTE_MAYBE_UNUSED struct state const *sp,
	   ATTRIBUTE_MAYBE_UNUSED int i)
{
#if TZ_RUNTIME_LEAPS
	return sp->lsis[i];
#else
	unreachable();
#endif
}
static void
set_lsinfo(ATTRIBUTE_MAYBE_UNUSED struct state *sp,
		   ATTRIBUTE_MAYBE_UNUSED int i,
		   ATTRIBUTE_MAYBE_UNUSED struct lsinfo lsinfo)
{
#if TZ_RUNTIME_LEAPS
	sp->lsis[i] = lsinfo;
#endif
}

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
	int_fast32_t r_time;		/* transition time of rule */
};

/*
 * Prototypes for static functions.
 */

static struct pg_tm *gmtsub(pg_time_t const *timep, int_fast32_t offset,
							struct pg_tm *tmp);
static bool increment_overflow(int *ip, int j);
static bool increment_overflow_time(pg_time_t *tp, int_fast32_2s j);
static int_fast32_2s leapcorr(struct state const *sp, pg_time_t t);
static struct pg_tm *timesub(pg_time_t const *timep,
							 int_fast32_t offset, struct state const *sp,
							 struct pg_tm *tmp);
static bool tzparse(const char *name, struct state *sp, struct state const *basep);


/*
 * Section 4.12.3 of X3.159-1989 requires that
 *	Except for the strftime function, these functions [asctime,
 *	ctime, gmtime, localtime] return values in one of two static
 *	objects: a broken-down time structure and an array of char.
 * Thanks to Paul Eggert for noting this.
 */

static struct pg_tm tm;

/* Initialize *S to a value based on UTOFF, ISDST, and DESIGIDX.  */
static void
init_ttinfo(struct ttinfo *s, int_fast32_t utoff, bool isdst,
			desigidx_type desigidx)
{
	s->tt_utoff = utoff;
	s->tt_isdst = isdst;
	s->tt_desigidx = desigidx;
	s->tt_ttisstd = false;
	s->tt_ttisut = false;
}

static int_fast32_2s
detzcode(const char *const codep)
{
	int			i;
	int_fast32_2s
				maxval = TWO_31_MINUS_1,
				minval = -1 - maxval,
				result;

	result = codep[0] & 0x7f;
	for (i = 1; i < 4; ++i)
		result = (result << 8) | (codep[i] & 0xff);

	if (codep[0] & 0x80)
	{
		/*
		 * Do two's-complement negation even on non-two's-complement machines.
		 * This cannot overflow, as int_fast32_2s is wide enough.
		 */
		result += minval;
	}
	return result;
}

static int_fast64_t
detzcode64(const char *const codep)
{
	int_fast64_t result;
	int			i;
	int_fast64_t one = 1;
	int_fast64_t halfmaxval = one << (64 - 2);
	int_fast64_t maxval = halfmaxval - 1 + halfmaxval;
	int_fast64_t minval = -TWOS_COMPLEMENT(int_fast64_t) - maxval;

	result = codep[0] & 0x7f;
	for (i = 1; i < 8; ++i)
		result = (result << 8) | (codep[i] & 0xff);

	if (codep[0] & 0x80)
	{
		/*
		 * Do two's-complement negation even on non-two's-complement machines.
		 * If the result would be minval - 1, return minval.
		 */
		result -= !TWOS_COMPLEMENT(int_fast64_t) && result != 0;
		result += minval;
	}
	return result;
}

/* Input buffer for data read from a compiled tz file.  */
union input_buffer
{
	/* The first part of the buffer, interpreted as a header.  */
	struct tzhead tzhead;

	/*
	 * The entire buffer.  Ideally this would have no size limits; the
	 * following should suffice for practical use.
	 */
	char		buf[2 * sizeof(struct tzhead) + 2 * sizeof(struct state)
					+ 4 * TZ_MAX_TIMES];
};

/* Local storage needed for 'tzloadbody'.  */
union local_storage
{
	/* The results of analyzing the file's contents after it is opened.  */
	struct file_analysis
	{
		/* The input buffer.  */
		union input_buffer u;

		/* A temporary state used for parsing a TZ string in the file.  */
		struct state st;
	}			u;

	/* PG: we don't need the "fullname" member */
};

/* These tzload flags can be ORed together, and fit into 'char'.  */
enum
{
TZLOAD_FROMENV = 1};			/* The TZ string came from the environment.  */
enum
{
TZLOAD_TZSTRING = 2};			/* Read any newline-surrounded TZ string.  */
enum
{
TZLOAD_TZDIR_SUB = 4};			/* TZ should be a file under TZDIR.  */

/*
 * Load tz data from the file named NAME into *SP.  Respect TZLOADFLAGS.
 * Use **LSPP for temporary storage.  Return 0 on
 * success, an errno value on failure.
 * PG: If "canonname" is not NULL, then on success the canonical spelling of
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 */
static int
tzloadbody(char const *name, char *canonname,
		   struct state *sp, char tzloadflags,
		   union local_storage **lspp)
{
	int			i;
	int			fid;
	int			stored;
	ssize_t		nread;
	union local_storage *lsp = *lspp;
	union input_buffer *up;
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

	/*
	 * The IANA code goes to a great deal of trouble here to try to prevent
	 * inappropriate file accesses.  That seems unnecessary for PG since we
	 * won't run as root.  pg_open_tzfile() does go to some effort to prevent
	 * accesses outside the designated zoneinfo tree, though.
	 */
	fid = pg_open_tzfile(name, canonname);
	if (fid < 0)
		return ENOENT;			/* pg_open_tzfile may not set errno */

	up = &lsp->u.u;
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
		char		version = up->tzhead.tzh_version[0];
		bool		skip_datablock = stored == 4 && version;
		int_fast32_t datablock_size;
		int_fast32_2s
					ttisstdcnt = detzcode(up->tzhead.tzh_ttisstdcnt),
					ttisutcnt = detzcode(up->tzhead.tzh_ttisutcnt),
					leapcnt = detzcode(up->tzhead.tzh_leapcnt),
					timecnt = detzcode(up->tzhead.tzh_timecnt),
					typecnt = detzcode(up->tzhead.tzh_typecnt),
					charcnt = detzcode(up->tzhead.tzh_charcnt);
		char const *p = up->buf + tzheadsize;

		/*
		 * Although tzfile(5) currently requires typecnt to be nonzero,
		 * support future formats that may allow zero typecnt in files that
		 * have a TZ string and no transitions.
		 */
		if (!(0 <= leapcnt
			  && leapcnt <= (TZ_RUNTIME_LEAPS ? TZ_MAX_LEAPS : 0)
			  && 0 <= typecnt && typecnt <= TZ_MAX_TYPES
			  && 0 <= timecnt && timecnt <= TZ_MAX_TIMES
			  && 0 <= charcnt && charcnt <= TZ_MAX_CHARS
			  && 0 <= ttisstdcnt && ttisstdcnt <= TZ_MAX_TYPES
			  && 0 <= ttisutcnt && ttisutcnt <= TZ_MAX_TYPES))
			return EINVAL;
		datablock_size
			= (timecnt * stored /* ats */
			   + timecnt		/* types */
			   + typecnt * 6	/* ttinfos */
			   + charcnt		/* chars */
			   + leapcnt * (stored + 4) /* lsinfos */
			   + ttisstdcnt		/* ttisstds */
			   + ttisutcnt);	/* ttisuts */
		if (nread < tzheadsize + datablock_size)
			return EINVAL;
		if (skip_datablock)
			p += datablock_size;
		else if (!((ttisstdcnt == typecnt || ttisstdcnt == 0)
				   && (ttisutcnt == typecnt || ttisutcnt == 0)))
			return EINVAL;
		else
		{
			int_fast64_t prevtr = -1;
			int_fast32_2s prevcorr = -1;

			set_leapcount(sp, leapcnt);
			sp->timecnt = timecnt;
			sp->typecnt = typecnt;
			sp->charcnt = charcnt;

			/*
			 * Read transitions, discarding those out of pg_time_t range. But
			 * pretend the last transition before TIME_T_MIN occurred at
			 * TIME_T_MIN.
			 */
			timecnt = 0;
			for (i = 0; i < sp->timecnt; ++i)
			{
				int_fast64_t at
				= stored == 4 ? detzcode(p) : detzcode64(p);

				sp->types[i] = at <= TIME_T_MAX;
				if (sp->types[i])
				{
					pg_time_t	attime
					= ((TYPE_SIGNED(pg_time_t) ? at < TIME_T_MIN : at < 0)
					   ? TIME_T_MIN : at);

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
							desigidx;
				int_fast32_2s utoff = detzcode(p);

				/*
				 * Reject a UT offset equal to -2**31, as it might cause
				 * trouble both in this file and in callers. Also, it violates
				 * RFC 9636 section 3.2.
				 */
				if (utoff < -TWO_31_MINUS_1)
					return EINVAL;

				ttisp = &sp->ttis[i];
				ttisp->tt_utoff = utoff;
				p += 4;
				isdst = *p++;
				if (!(isdst < 2))
					return EINVAL;
				ttisp->tt_isdst = isdst;
				desigidx = *p++;
				if (!(desigidx < sp->charcnt))
					return EINVAL;
				ttisp->tt_desigidx = desigidx;
			}
			for (i = 0; i < sp->charcnt; ++i)
				sp->chars[i] = *p++;

			/*
			 * Ensure '\0'-terminated, and make it safe to call ttunspecified
			 * later.
			 */
			memset(&sp->chars[i], 0, CHARS_EXTRA);

			/* Read leap seconds, discarding those out of pg_time_t range.  */
			leapcnt = 0;
			for (i = 0; i < leapcount(sp); i++)
			{
				int_fast64_t tr = stored == 4 ? detzcode(p) : detzcode64(p);
				int_fast32_2s corr = detzcode(p + stored);

				p += stored + 4;

				/*
				 * Leap seconds cannot occur before the Epoch, or out of
				 * order.
				 */
				if (tr <= prevtr)
					return EINVAL;

				/*
				 * To avoid other botches in this code, each leap second's
				 * correction must differ from the previous one's by 1 second
				 * or less, except that the first correction can be any value;
				 * these requirements are more generous than RFC 9636, to
				 * allow future RFC extensions.
				 */
				if (!(i == 0
					  || (prevcorr < corr
						  ? corr == prevcorr + 1
						  : (corr == prevcorr
							 || corr == prevcorr - 1))))
					return EINVAL;
				prevtr = tr;
				prevcorr = corr;

				if (tr <= TIME_T_MAX)
				{
					struct lsinfo ls;

					ls.ls_trans = tr;
					ls.ls_corr = corr;
					set_lsinfo(sp, leapcnt, ls);
					leapcnt++;
				}
			}
			set_leapcount(sp, leapcnt);

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
				if (ttisutcnt == 0)
					ttisp->tt_ttisut = false;
				else
				{
					if (*p != true && *p != false)
						return EINVAL;
					ttisp->tt_ttisut = *p++;
				}
			}
		}

		nread -= p - up->buf;
		memmove(up->buf, p, nread);

		/* If this is an old file, we're done.  */
		if (!version)
			break;
	}
	if ((tzloadflags & TZLOAD_TZSTRING) && nread > 2 &&
		up->buf[0] == '\n' && up->buf[nread - 1] == '\n' &&
		sp->typecnt + 2 <= TZ_MAX_TYPES)
	{
		struct state *ts = &lsp->u.st;

		up->buf[nread - 1] = '\0';
		if (tzparse(&up->buf[1], ts, sp))
		{

			/*
			 * Attempt to reuse existing abbreviations. Without this,
			 * America/Anchorage would consume 50 bytes for abbreviations, as
			 * sp->charcnt equals 40 (for LMT AST AWT APT AHST AHDT YST AKDT
			 * AKST) and ts->charcnt equals 10 (for AKST AKDT).  Reusing means
			 * sp->charcnt can stay 40 in this example.
			 */
			int			gotabbr = 0;
			int			charcnt = sp->charcnt;

			for (i = 0; i < ts->typecnt; i++)
			{
				char	   *tsabbr = ts->chars + ts->ttis[i].tt_desigidx;
				int			j;

				for (j = 0; j < charcnt; j++)
					if (strcmp(sp->chars + j, tsabbr) == 0)
					{
						ts->ttis[i].tt_desigidx = j;
						gotabbr++;
						break;
					}
				if (!(j < charcnt))
				{
					int			tsabbrlen = strnlen(tsabbr, TZ_MAX_CHARS - j);

					if (j + tsabbrlen < TZ_MAX_CHARS)
					{
						char	   *cp = sp->chars + j;

						memcpy(cp, tsabbr, tsabbrlen);
						cp += tsabbrlen;
						*cp = '\0';
						charcnt = j + tsabbrlen + 1;
						ts->ttis[i].tt_desigidx = j;
						gotabbr++;
					}
				}
			}
			if (gotabbr == ts->typecnt)
			{
				sp->charcnt = charcnt;

				/*
				 * Ignore any trailing, no-op transitions generated by zic as
				 * they don't help here and can run afoul of bugs in zic 2016j
				 * or earlier.
				 */
				while (1 < sp->timecnt
					   && (sp->types[sp->timecnt - 1]
						   == sp->types[sp->timecnt - 2]))
					sp->timecnt--;

				sp->goahead = ts->goahead;

				for (i = 0; i < ts->timecnt; i++)
				{
					pg_time_t	t = ts->ats[i];

					if (increment_overflow_time(&t, leapcorr(sp, t))
						|| (0 < sp->timecnt
							&& t <= sp->ats[sp->timecnt - 1]))
						continue;
					if (TZ_MAX_TIMES <= sp->timecnt)
					{
						sp->goahead = false;
						break;
					}
					sp->ats[sp->timecnt] = t;
					sp->types[sp->timecnt] = (sp->typecnt
											  + ts->types[i]);
					sp->timecnt++;
				}
				for (i = 0; i < ts->typecnt; i++)
					sp->ttis[sp->typecnt++] = ts->ttis[i];
			}
		}
	}
	if (sp->typecnt == 0)
		return EINVAL;

	return 0;
}

/*
 * Load tz data from the file named NAME into *SP.  Respect TZLOADFLAGS.
 * Return 0 on success, an errno value on failure.
 * PG: If "canonname" is not NULL, then on success the canonical spelling of
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 */
static int
tzload(char const *name, char *canonname, struct state *sp, char tzloadflags)
{
	/* PG: our version of tzloadbody never reallocates *lspp */
	union local_storage *lsp;
	union local_storage ls;

	lsp = &ls;
	return tzloadbody(name, canonname, sp, tzloadflags, &lsp);
}

static const int mon_lengths[2][MONSPERYEAR] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
	{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

static const int year_lengths[2] = {
	DAYSPERNYEAR, DAYSPERLYEAR
};

/* Is C an ASCII digit?  */
static bool
is_digit(char c)
{
	return '0' <= c && c <= '9';
}

/*
 * Given a pointer into a timezone string, scan until a character that is not
 * a valid character in a time zone abbreviation is found.
 * Return a pointer to that character.
 */

ATTRIBUTE_PURE_114833 static const char *
getzname(const char *strp)
{
	char		c;

	while ((c = *strp) != '\0' && !is_digit(c) && c != ',' && c != '-' &&
		   c != '+')
		++strp;
	return strp;
}

/*
 * Given a pointer into an extended timezone string, scan until the ending
 * delimiter of the time zone abbreviation is located.
 * Return a pointer to the delimiter.
 *
 * As with getzname above, the legal character set is actually quite
 * restricted, with other characters producing undefined results.
 * We don't do any checking here; checking is done later in common-case code.
 */

ATTRIBUTE_PURE_114833 static const char *
getqzname(const char *strp, const int delim)
{
	int			c;

	while ((c = *strp) != '\0' && c != delim)
		++strp;
	return strp;
}

/*
 * Given a pointer into a timezone string, extract a number from that string.
 * Check that the number is within a specified range; if it is not, return
 * NULL.
 * Otherwise, return a pointer to the first character not part of the number.
 */

static const char *
getnum(const char *strp, int *const nump, const int min, const int max)
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
 * Given a pointer into a timezone string, extract a number of seconds,
 * in hh[:mm[:ss]] form, from the string.
 * If any error occurs, return NULL.
 * Otherwise, return a pointer to the first character not part of the number
 * of seconds.
 */

static const char *
getsecs(const char *strp, int_fast32_t *const secsp)
{
	int			num;
	int_fast32_t secsperhour = SECSPERHOUR;

	/*
	 * 'HOURSPERDAY * DAYSPERWEEK - 1' allows quasi-POSIX rules like
	 * "M10.4.6/26", which does not conform to POSIX, but which specifies the
	 * equivalent of "02:00 on the first Sunday on or after 23 Oct".
	 */
	strp = getnum(strp, &num, 0, HOURSPERDAY * DAYSPERWEEK - 1);
	if (strp == NULL)
		return NULL;
	*secsp = num * secsperhour;
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
 * Given a pointer into a timezone string, extract an offset, in
 * [+-]hh[:mm[:ss]] form, from the string.
 * If any error occurs, return NULL.
 * Otherwise, return a pointer to the first character not part of the time.
 */

static const char *
getoffset(const char *strp, int_fast32_t *const offsetp)
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
 * Given a pointer into a timezone string, extract a rule in the form
 * date[/time]. See POSIX Base Definitions section 8.3 variable TZ
 * for the format of "date" and "time".
 * If a valid rule is not found, return NULL.
 * Otherwise, return a pointer to the first character not part of the rule.
 */

static const char *
getrule(const char *strp, struct rule *const rulep)
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
		rulep->r_time = 2 * SECSPERHOUR;	/* default = 2:00:00 */
	return strp;
}

/*
 * Given a year, a rule, and the offset from UT at the time that rule takes
 * effect, calculate the year-relative time that rule takes effect.
 */

static int_fast32_t
transtime(const int year, const struct rule *const rulep,
		  const int_fast32_t offset)
{
	bool		leapyear;
	int_fast32_t value;
	int			i;
	int			d,
				m1,
				yy0,
				yy1,
				yy2,
				dow;

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
					mon_lengths[leapyear][rulep->r_mon - 1])
					break;
				d += DAYSPERWEEK;
			}

			/*
			 * "d" is the day-of-month (zero-origin) of the day we want.
			 */
			value = d * SECSPERDAY;
			for (i = 0; i < rulep->r_mon - 1; ++i)
				value += mon_lengths[leapyear][i] * SECSPERDAY;
			break;

		default:
			unreachable();
	}

	/*
	 * "value" is the year-relative time of 00:00:00 UT on the day in
	 * question. To get the year-relative time of the specified local time on
	 * that day, add the transition time and the current offset from UT.
	 */
	return value + rulep->r_time + offset;
}

/*
 * Given a POSIX.1 proleptic TZ string, fill in the rule tables as
 * appropriate.
 */

static bool
tzparse(const char *name, struct state *sp, struct state const *basep)
{
	const char *stdname;
	const char *dstname = NULL;
	int_fast32_t stdoffset;
	int_fast32_t dstoffset;
	char	   *cp;
	ptrdiff_t	stdlen,
				dstlen,
				charcnt;
	pg_time_t	atlo = TIME_T_MIN,
				leaplo = TIME_T_MIN;

	stdname = name;
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
	if (stdlen > TZNAME_MAXIMUM)	/* allow empty STD abbrev, unlike IANA */
		return false;
	name = getoffset(name, &stdoffset);
	if (name == NULL)
		return false;
	charcnt = stdlen + 1;
	if (basep)
	{
		if (0 < basep->timecnt)
			atlo = basep->ats[basep->timecnt - 1];
		set_leapcount(sp, leapcount(basep));
		if (0 < leapcount(sp))
		{
			int			i;

			for (i = 0; i < leapcount(sp); i++)
				set_lsinfo(sp, i, lsinfo(basep, i));
			leaplo = lsinfo(sp, leapcount(sp) - 1).ls_trans;
		}
	}
	else
		set_leapcount(sp, 0);	/* So, we're off a little.  */
	sp->goback = sp->goahead = false;
	if (*name != '\0')
	{
		struct rule start,
					end;
		int			year,
					yearbeg,
					yearlim,
					timecnt;
		pg_time_t	janfirst;
		int_fast32_t janoffset = 0;

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
			dstlen = name - dstname;	/* length of DST abbr. */
		}
		if (!(0 < dstlen && dstlen <= TZNAME_MAXIMUM))
			return false;
		charcnt += dstlen + 1;
		if (*name != '\0' && *name != ',' && *name != ';')
		{
			name = getoffset(name, &dstoffset);
			if (name == NULL)
				return false;
		}
		else
			dstoffset = stdoffset - SECSPERHOUR;

		if (*name == '\0')
			name = TZDEFRULESTRING;
		if (!(*name == ',' || *name == ';'))
			return false;

		name = getrule(name + 1, &start);
		if (!name)
			return false;
		if (*name++ != ',')
			return false;
		name = getrule(name, &end);
		if (!name || *name)
			return false;
		sp->typecnt = 2;		/* standard time and DST */

		/*
		 * Two transitions per year, from EPOCH_YEAR forward.
		 */
		init_ttinfo(&sp->ttis[0], -stdoffset, false, 0);
		init_ttinfo(&sp->ttis[1], -dstoffset, true, stdlen + 1);
		timecnt = 0;
		janfirst = 0;
		yearbeg = EPOCH_YEAR;

		do
		{
			int_fast32_t yearsecs
			= year_lengths[isleap(yearbeg - 1)] * SECSPERDAY;
			pg_time_t	janfirst1 = janfirst;

			yearbeg--;
			if (increment_overflow_time(&janfirst1, -yearsecs))
			{
				janoffset = -yearsecs;
				break;
			}
			janfirst = janfirst1;
		} while (atlo < janfirst
				 && EPOCH_YEAR - YEARSPERREPEAT / 2 < yearbeg);

		while (true)
		{
			int_fast32_t yearsecs
			= year_lengths[isleap(yearbeg)] * SECSPERDAY;
			int			yearbeg1 = yearbeg;
			pg_time_t	janfirst1 = janfirst;

			if (increment_overflow_time(&janfirst1, yearsecs)
				|| increment_overflow(&yearbeg1, 1)
				|| atlo <= janfirst1)
				break;
			yearbeg = yearbeg1;
			janfirst = janfirst1;
		}

		yearlim = yearbeg;
		if (increment_overflow(&yearlim, years_of_observations))
			yearlim = INT_MAX;
		for (year = yearbeg; year < yearlim; year++)
		{
			int_fast32_t
						starttime = transtime(year, &start, stdoffset),
						endtime = transtime(year, &end, dstoffset),
						yearsecs = year_lengths[isleap(year)] * SECSPERDAY;
			bool		reversed = endtime < starttime;

			if (reversed)
			{
				int_fast32_t swap = starttime;

				starttime = endtime;
				endtime = swap;
			}
			if (reversed
				|| (starttime < endtime
					&& endtime - starttime < yearsecs))
			{
				if (TZ_MAX_TIMES - 2 < timecnt)
					break;
				sp->ats[timecnt] = janfirst;
				if (!increment_overflow_time(&sp->ats[timecnt],
											 janoffset + starttime)
					&& atlo <= sp->ats[timecnt])
					sp->types[timecnt++] = !reversed;
				sp->ats[timecnt] = janfirst;
				if (!increment_overflow_time(&sp->ats[timecnt],
											 janoffset + endtime)
					&& atlo <= sp->ats[timecnt])
				{
					sp->types[timecnt++] = reversed;
				}
			}
			if (endtime < leaplo)
			{
				yearlim = year;
				if (increment_overflow(&yearlim, years_of_observations))
					yearlim = INT_MAX;
			}
			if (increment_overflow_time(&janfirst, janoffset + yearsecs))
				break;
			janoffset = 0;
		}
		sp->timecnt = timecnt;
		if (!timecnt)
		{
			sp->ttis[0] = sp->ttis[1];
			sp->typecnt = 1;	/* Perpetual DST.  */
		}
		else if (years_of_observations <= year - yearbeg)
			sp->goback = sp->goahead = true;
	}
	else
	{
		dstlen = 0;
		sp->typecnt = 1;		/* only standard time */
		sp->timecnt = 0;
		init_ttinfo(&sp->ttis[0], -stdoffset, false, 0);
	}
	sp->charcnt = charcnt;
	cp = sp->chars;
	memcpy(cp, stdname, stdlen);
	cp += stdlen;
	*cp++ = '\0';
	if (dstlen != 0)
	{
		memcpy(cp, dstname, dstlen);
		cp += dstlen;
		*cp = '\0';
	}
	return true;
}

static void
gmtload(struct state *const sp)
{
	/* PG: for historical compatibility, use "GMT" not "UTC" as TZ abbrev */
	tzparse("GMT0", sp, NULL);
}


/*
 * The easy way to behave "as if no library function calls" localtime
 * is to not call it, so we drop its guts into "localsub", which can be
 * freely called. (And no, the PANS doesn't require the above behavior,
 * but it *is* desirable.)
 */
static struct pg_tm *
localsub(struct state const *sp, pg_time_t const *timep,
		 struct pg_tm *const tmp)
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
		pg_time_t	newt;
		pg_time_t	seconds;
		pg_time_t	years;

		if (t < sp->ats[0])
			seconds = sp->ats[0] - t;
		else
			seconds = t - sp->ats[sp->timecnt - 1];
		--seconds;

		/*
		 * Beware integer overflow, as SECONDS might be close to the maximum
		 * pg_time_t.
		 */
		years = seconds / SECSPERREPEAT * YEARSPERREPEAT;
		seconds = years * AVGSECSPERYEAR;
		years += YEARSPERREPEAT;
		if (t < sp->ats[0])
			newt = t + seconds + SECSPERREPEAT;
		else
			newt = t - seconds - SECSPERREPEAT;

		if (newt < sp->ats[0] ||
			newt > sp->ats[sp->timecnt - 1])
			return NULL;		/* "cannot happen" */
		result = localsub(sp, &newt, tmp);
		if (result)
		{
#if defined ckd_add && defined ckd_sub
			if (t < sp->ats[0]
				? ckd_sub(&result->tm_year,
						  result->tm_year, years)
				: ckd_add(&result->tm_year,
						  result->tm_year, years))
				return NULL;
#else
			int_fast64_t newy;

			newy = result->tm_year;
			if (t < sp->ats[0])
				newy -= years;
			else
				newy += years;
			if (!(INT_MIN <= newy && newy <= INT_MAX))
				return NULL;
			result->tm_year = newy;
#endif
		}
		return result;
	}
	if (sp->timecnt == 0 || t < sp->ats[0])
	{
		i = 0;
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
		i = sp->types[lo - 1];
	}
	ttisp = &sp->ttis[i];

	/*
	 * To get (wrong) behavior that's compatible with System V Release 2.0
	 * you'd replace the statement below with t += ttisp->tt_utoff;
	 * timesub(&t, 0, sp, tmp);
	 */
	result = timesub(&t, ttisp->tt_utoff, sp, tmp);
	if (result)
	{
		result->tm_isdst = ttisp->tt_isdst;
#ifdef TM_ZONE
		result->TM_ZONE = UNCONST(&sp->chars[ttisp->tt_desigidx]);
#endif
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
 * PG: except we have a private "struct state" for GMT, so no sp is passed in.
 */

static struct pg_tm *
gmtsub(pg_time_t const *timep,
	   int_fast32_t offset, struct pg_tm *tmp)
{
	struct pg_tm *result;

	/* GMT timezone state data is kept here */
	static struct state *gmtptr = NULL;

	if (gmtptr == NULL)
	{
		/* Allocate on first use */
		gmtptr = (struct state *) malloc(sizeof(struct state));
		if (gmtptr == NULL)
			return NULL;		/* errno should be set by malloc */
		gmtload(gmtptr);
	}

	result = timesub(timep, offset, gmtptr, tmp);
#ifdef TM_ZONE

	/*
	 * Could get fancy here and deliver something such as "+xx" or "-xx" if
	 * offset is non-zero, but this is no time for a treasure hunt.
	 */
	tmp->TM_ZONE = UNCONST(offset ? wildabbr
						   : gmtptr->chars);
#endif							/* defined TM_ZONE */
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

static pg_time_t
leaps_thru_end_of_nonneg(pg_time_t y)
{
	return y / 4 - y / 100 + y / 400;
}

static pg_time_t
leaps_thru_end_of(pg_time_t y)
{
	return (y < 0
			? -1 - leaps_thru_end_of_nonneg(-1 - y)
			: leaps_thru_end_of_nonneg(y));
}

static struct pg_tm *
timesub(const pg_time_t *timep, int_fast32_t offset,
		const struct state *sp, struct pg_tm *tmp)
{
	pg_time_t	tdays;
	const int  *ip;
	int_fast32_2s corr;
	int			i;
	int_fast32_t idays,
				rem,
				dayoff,
				dayrem;
	pg_time_t	y;

	/*
	 * If less than SECSPERMIN, the number of seconds since the most recent
	 * positive leap second; otherwise, do not add 1 to localtime tm_sec
	 * because of leap seconds.
	 */
	pg_time_t	secs_since_posleap = SECSPERMIN;

	corr = 0;
	i = sp ? leapcount(sp) : 0;
	while (--i >= 0)
	{
		struct lsinfo ls = lsinfo(sp, i);

		if (ls.ls_trans <= *timep)
		{
			corr = ls.ls_corr;
			if ((i == 0 ? 0 : lsinfo(sp, i - 1).ls_corr) < corr)
				secs_since_posleap = *timep - ls.ls_trans;
			break;
		}
	}

	/*
	 * Calculate the year, avoiding integer overflow even if pg_time_t is
	 * unsigned.
	 */
	tdays = *timep / SECSPERDAY;
	rem = *timep % SECSPERDAY;
	rem += offset % SECSPERDAY - corr % SECSPERDAY + 3 * SECSPERDAY;
	dayoff = offset / SECSPERDAY - corr / SECSPERDAY + rem / SECSPERDAY - 3;
	rem %= SECSPERDAY;

	/*
	 * y = (EPOCH_YEAR + floor((tdays + dayoff) / DAYSPERREPEAT) *
	 * YEARSPERREPEAT), sans overflow.  But calculate against 1570 (EPOCH_YEAR
	 * - YEARSPERREPEAT) instead of against 1970 so that things work for
	 * localtime values before 1970 when pg_time_t is unsigned.
	 */
	dayrem = tdays % DAYSPERREPEAT;
	dayrem += dayoff % DAYSPERREPEAT;
	y = (EPOCH_YEAR - YEARSPERREPEAT
		 + ((1 + dayoff / DAYSPERREPEAT + dayrem / DAYSPERREPEAT
			 - ((dayrem % DAYSPERREPEAT) < 0)
			 + tdays / DAYSPERREPEAT)
			* YEARSPERREPEAT));
	/* idays = (tdays + dayoff) mod DAYSPERREPEAT, sans overflow.  */
	idays = tdays % DAYSPERREPEAT;
	idays += dayoff % DAYSPERREPEAT + 2 * DAYSPERREPEAT;
	idays %= DAYSPERREPEAT;
	/* Increase Y and decrease IDAYS until IDAYS is in range for Y.  */
	while (year_lengths[isleap(y)] <= idays)
	{
		int			tdelta = idays / DAYSPERLYEAR;
		int_fast32_t ydelta = tdelta + !tdelta;
		pg_time_t	newy = y + ydelta;
		int			leapdays;

		leapdays = leaps_thru_end_of(newy - 1) -
			leaps_thru_end_of(y - 1);
		idays -= ydelta * DAYSPERNYEAR;
		idays -= leapdays;
		y = newy;
	}

#ifdef ckd_add
	if (ckd_add(&tmp->tm_year, y, -TM_YEAR_BASE))
	{
		errno = EOVERFLOW;
		return NULL;
	}
#else
	if (!TYPE_SIGNED(pg_time_t) && y < TM_YEAR_BASE)
	{
		int			signed_y = y;

		tmp->tm_year = signed_y - TM_YEAR_BASE;
	}
	else if ((!TYPE_SIGNED(pg_time_t) || INT_MIN + TM_YEAR_BASE <= y)
			 && y - TM_YEAR_BASE <= INT_MAX)
		tmp->tm_year = y - TM_YEAR_BASE;
	else
	{
		errno = EOVERFLOW;
		return NULL;
	}
#endif
	tmp->tm_yday = idays;

	/*
	 * The "extra" mods below avoid overflow problems.
	 */
	tmp->tm_wday = (TM_WDAY_BASE
					+ ((tmp->tm_year % DAYSPERWEEK)
					   * (DAYSPERNYEAR % DAYSPERWEEK))
					+ leaps_thru_end_of(y - 1)
					- leaps_thru_end_of(TM_YEAR_BASE - 1)
					+ idays);
	tmp->tm_wday %= DAYSPERWEEK;
	if (tmp->tm_wday < 0)
		tmp->tm_wday += DAYSPERWEEK;
	tmp->tm_hour = rem / SECSPERHOUR;
	rem %= SECSPERHOUR;
	tmp->tm_min = rem / SECSPERMIN;
	tmp->tm_sec = rem % SECSPERMIN;

	/*
	 * Use "... ??:??:60" at the end of the localtime minute containing the
	 * second just before the positive leap second.
	 */
	tmp->tm_sec += secs_since_posleap <= tmp->tm_sec;

	ip = mon_lengths[isleap(y)];
	for (tmp->tm_mon = 0; idays >= ip[tmp->tm_mon]; ++(tmp->tm_mon))
		idays -= ip[tmp->tm_mon];
	tmp->tm_mday = idays + 1;
	tmp->tm_isdst = 0;
#ifdef TM_GMTOFF
	tmp->TM_GMTOFF = offset;
#endif							/* defined TM_GMTOFF */
	return tmp;
}

/*
 * Adapted from code provided by Robert Elz, who writes:
 *	The "best" way to do mktime I think is based on an idea of Bob
 *	Kridle's (so its said...) from a long time ago.
 *	It does a binary search of the pg_time_t space. Since pg_time_t's are
 *	just 32 bits, its a max of 32 iterations (even at 64 bits it
 *	would still be very reasonable).
 */

#ifndef WRONG
#define WRONG (-1)
#endif							/* !defined WRONG */

/*
 * Normalize logic courtesy Paul Eggert.
 */

static bool
increment_overflow(int *ip, int j)
{
#ifdef ckd_add
	return ckd_add(ip, *ip, j);
#else
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
#endif
}

static bool
increment_overflow_time(pg_time_t *tp, int_fast32_2s j)
{
#ifdef ckd_add
	return ckd_add(tp, *tp, j);
#else
	/*----------
	 * This is like
	 * 'if (! (TIME_T_MIN <= *tp + j && *tp + j <= TIME_T_MAX)) ...',
	 * except that it does the right thing even if *tp + j would overflow.
	 *----------
	 */
	if (!(j < 0
		  ? (TYPE_SIGNED(pg_time_t) ? TIME_T_MIN - j <= *tp : -1 - j < *tp)
		  : *tp <= TIME_T_MAX - j))
		return true;
	*tp += j;
	return false;
#endif
}

static int_fast32_2s
leapcorr(struct state const *sp, pg_time_t t)
{
	int			i;

	i = leapcount(sp);
	while (--i >= 0)
	{
		struct lsinfo ls = lsinfo(sp, i);

		if (ls.ls_trans <= t)
			return ls.ls_corr;
	}
	return 0;
}

/*
 * Postgres-specific functions begin here.
 */

/*
 * Load the definition of the given time zone name into *sp.
 * Return true if successful, false if not.
 * If "canonname" is not NULL, then on success the canonical spelling of
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 *
 * "GMT" is always interpreted as the gmtload() definition, without attempting
 * to load a definition from the filesystem.  This has a number of benefits:
 * 1. It's guaranteed to succeed, so we don't have the failure mode wherein
 * the bootstrap default timezone setting doesn't work (as could happen if
 * the OS attempts to supply a leap-second-aware version of "GMT").
 * 2. Because we aren't accessing the filesystem, we can safely initialize
 * the "GMT" zone definition before my_exec_path is known.
 * 3. It's quick enough that we don't waste much time when the bootstrap
 * default timezone setting is later overridden from postgresql.conf.
 */
bool
pg_tzload(const char *name, char *canonname, struct state *sp)
{
	if (strcmp(name, "GMT") == 0)
	{
		gmtload(sp);
		/* Use given name as canonical */
		if (canonname)
			strcpy(canonname, name);
	}
	else if (tzload(name, canonname, sp, TZLOAD_TZSTRING) != 0)
	{
		if (name[0] == ':' || !tzparse(name, sp, NULL))
		{
			/* Unknown timezone. Fail our call instead of loading GMT! */
			return false;
		}
		/* For POSIX timezone specs, use given name as canonical */
		if (canonname)
			strcpy(canonname, name);
	}
	return true;
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
		/* non-DST zone, use the defaulttype (now always 0) */
		ttisp = &sp->ttis[0];
		*before_gmtoff = ttisp->tt_utoff;
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
		*before_gmtoff = ttisp->tt_utoff;
		*before_isdst = ttisp->tt_isdst;
		return 0;
	}
	if (t < sp->ats[0])
	{
		/* For "before", use the defaulttype (now always 0) */
		ttisp = &sp->ttis[0];
		*before_gmtoff = ttisp->tt_utoff;
		*before_isdst = ttisp->tt_isdst;
		*boundary = sp->ats[0];
		/* And for "after", use the first segment's type */
		i = sp->types[0];
		ttisp = &sp->ttis[i];
		*after_gmtoff = ttisp->tt_utoff;
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
	*before_gmtoff = ttisp->tt_utoff;
	*before_isdst = ttisp->tt_isdst;
	*boundary = sp->ats[i];
	j = sp->types[i];
	ttisp = &sp->ttis[j];
	*after_gmtoff = ttisp->tt_utoff;
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
	 * Use binary search to locate the first transition > cutoff time.  (Note
	 * that sp->timecnt could be zero, in which case this loop does nothing
	 * and only the defaulttype entry will be checked.)
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
		if (ttisp->tt_desigidx == abbrind)
		{
			*gmtoff = ttisp->tt_utoff;
			*isdst = ttisp->tt_isdst;
			return true;
		}
	}

	/*
	 * Not found yet; check the defaulttype, which is notionally the era
	 * before any of the entries in sp->types[].
	 */
	ttisp = &sp->ttis[0];
	if (ttisp->tt_desigidx == abbrind)
	{
		*gmtoff = ttisp->tt_utoff;
		*isdst = ttisp->tt_isdst;
		return true;
	}

	/*
	 * Not there, so scan forwards to find the first one after the cutoff.
	 */
	for (i = cutoff; i < sp->timecnt; i++)
	{
		ttisp = &sp->ttis[sp->types[i]];
		if (ttisp->tt_desigidx == abbrind)
		{
			*gmtoff = ttisp->tt_utoff;
			*isdst = ttisp->tt_isdst;
			return true;
		}
	}

	return false;				/* hm, not actually used in any interval? */
}

/*
 * Detect whether a timezone abbreviation is defined within the given zone.
 *
 * This is similar to pg_interpret_timezone_abbrev() but is not concerned
 * with a specific point in time.  We want to know if the abbreviation is
 * known at all, and if so whether it has one meaning or several.
 *
 * Returns true if the abbreviation is known, false if not.
 * If the abbreviation is known and has a single meaning (only one value
 * of gmtoff/isdst), sets *isfixed = true and sets *gmtoff and *isdst.
 * If there are multiple meanings, sets *isfixed = false.
 *
 * Note: abbrev is matched case-sensitively; it should be all-upper-case.
 */
bool
pg_timezone_abbrev_is_known(const char *abbrev,
							bool *isfixed,
							long int *gmtoff,
							int *isdst,
							const pg_tz *tz)
{
	bool		result = false;
	const struct state *sp = &tz->state;
	const char *abbrs;
	int			abbrind;

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
		return false;			/* definitely not there */

	/*
	 * Scan the ttinfo array to find uses of the abbreviation.
	 */
	for (int i = 0; i < sp->typecnt; i++)
	{
		const struct ttinfo *ttisp = &sp->ttis[i];

		if (ttisp->tt_desigidx == abbrind)
		{
			if (!result)
			{
				/* First usage */
				*isfixed = true;	/* for the moment */
				*gmtoff = ttisp->tt_utoff;
				*isdst = ttisp->tt_isdst;
				result = true;
			}
			else
			{
				/* Second or later usage, does it match? */
				if (*gmtoff != ttisp->tt_utoff ||
					*isdst != ttisp->tt_isdst)
				{
					*isfixed = false;
					break;		/* no point in looking further */
				}
			}
		}
	}

	return result;
}

/*
 * Iteratively fetch all the abbreviations used in the given time zone.
 *
 * *indx is a state counter that the caller must initialize to zero
 * before the first call, and not touch between calls.
 *
 * Returns the next known abbreviation, or NULL if there are no more.
 *
 * Note: the caller typically applies pg_interpret_timezone_abbrev()
 * to each result.  While that nominally results in O(N^2) time spent
 * searching the sp->chars[] array, we don't expect any zone to have
 * enough abbreviations to make that meaningful.
 */
const char *
pg_get_next_timezone_abbrev(int *indx,
							const pg_tz *tz)
{
	const char *result;
	const struct state *sp = &tz->state;
	const char *abbrs;
	int			abbrind;

	/* If we're still in range, the result is the current abbrev. */
	abbrs = sp->chars;
	abbrind = *indx;
	if (abbrind < 0 || abbrind >= sp->charcnt)
		return NULL;
	result = abbrs + abbrind;

	/* Advance *indx past this abbrev and its trailing null. */
	while (abbrs[abbrind] != '\0')
		abbrind++;
	abbrind++;
	*indx = abbrind;

	return result;
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
		if (sp->ttis[i].tt_utoff != sp->ttis[0].tt_utoff)
			return false;
	}
	*gmtoff = sp->ttis[0].tt_utoff;
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
