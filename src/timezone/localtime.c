#include "pgtz.h"
#undef open
#define timezone pg_timezone
#define USG_COMPAT
extern time_t pg_timezone;
/*
** This file is in the public domain, so clarified as of
** 1996-06-05 by Arthur David Olson (arthur_david_olson@nih.gov).
*/

#ifndef lint
#ifndef NOID
static char	elsieid[] = "@(#)localtime.c	7.78";
#endif /* !defined NOID */
#endif /* !defined lint */

/*
** Leap second handling from Bradley White (bww@k.gp.cs.cmu.edu).
** POSIX-style TZ environment variable handling from Guy Harris
** (guy@auspex.com).
*/

/*LINTLIBRARY*/

#include "private.h"
#include "tzfile.h"
#include "fcntl.h"

/*
** SunOS 4.1.1 headers lack O_BINARY.
*/

#ifdef O_BINARY
#define OPEN_MODE	(O_RDONLY | O_BINARY)
#endif /* defined O_BINARY */
#ifndef O_BINARY
#define OPEN_MODE	O_RDONLY
#endif /* !defined O_BINARY */

#ifndef WILDABBR
/*
** Someone might make incorrect use of a time zone abbreviation:
**	1.	They might reference tzname[0] before calling tzset (explicitly
**		or implicitly).
**	2.	They might reference tzname[1] before calling tzset (explicitly
**		or implicitly).
**	3.	They might reference tzname[1] after setting to a time zone
**		in which Daylight Saving Time is never observed.
**	4.	They might reference tzname[0] after setting to a time zone
**		in which Standard Time is never observed.
**	5.	They might reference tm.TM_ZONE after calling offtime.
** What's best to do in the above cases is open to debate;
** for now, we just set things up so that in any of the five cases
** WILDABBR is used.  Another possibility:  initialize tzname[0] to the
** string "tzname[0] used before set", and similarly for the other cases.
** And another:  initialize tzname[0] to "ERA", with an explanation in the
** manual page of what this "time zone abbreviation" means (doing this so
** that tzname[0] has the "normal" length of three characters).
*/
#define WILDABBR	"   "
#endif /* !defined WILDABBR */

static char		wildabbr[] = "WILDABBR";

static const char	gmt[] = "GMT";

/*
** The DST rules to use if TZ has no rules and we can't load TZDEFRULES.
** We default to US rules as of 1999-08-17.
** POSIX 1003.1 section 8.1.1 says that the default DST rules are
** implementation dependent; for historical reasons, US rules are a
** common default.
*/
#ifndef TZDEFRULESTRING
#define TZDEFRULESTRING ",M4.1.0,M10.5.0"
#endif /* !defined TZDEFDST */

struct ttinfo {				/* time type information */
	long		tt_gmtoff;	/* UTC offset in seconds */
	int		tt_isdst;	/* used to set tm_isdst */
	int		tt_abbrind;	/* abbreviation list index */
	int		tt_ttisstd;	/* TRUE if transition is std time */
	int		tt_ttisgmt;	/* TRUE if transition is UTC */
};

struct lsinfo {				/* leap second information */
	time_t		ls_trans;	/* transition time */
	long		ls_corr;	/* correction to apply */
};

#define BIGGEST(a, b)	(((a) > (b)) ? (a) : (b))

#ifdef TZNAME_MAX
#define MY_TZNAME_MAX	TZNAME_MAX
#endif /* defined TZNAME_MAX */
#ifndef TZNAME_MAX
#define MY_TZNAME_MAX	255
#endif /* !defined TZNAME_MAX */

struct state {
	int		leapcnt;
	int		timecnt;
	int		typecnt;
	int		charcnt;
	time_t		ats[TZ_MAX_TIMES];
	unsigned char	types[TZ_MAX_TIMES];
	struct ttinfo	ttis[TZ_MAX_TYPES];
	char		chars[BIGGEST(BIGGEST(TZ_MAX_CHARS + 1, sizeof gmt),
				(2 * (MY_TZNAME_MAX + 1)))];
	struct lsinfo	lsis[TZ_MAX_LEAPS];
};

struct rule {
	int		r_type;		/* type of rule--see below */
	int		r_day;		/* day number of rule */
	int		r_week;		/* week number of rule */
	int		r_mon;		/* month number of rule */
	long		r_time;		/* transition time of rule */
};

#define JULIAN_DAY		0	/* Jn - Julian day */
#define DAY_OF_YEAR		1	/* n - day of year */
#define MONTH_NTH_DAY_OF_WEEK	2	/* Mm.n.d - month, week, day of week */

/*
** Prototypes for static functions.
*/

static long		detzcode P((const char * codep));
static const char *	getzname P((const char * strp));
static const char *	getnum P((const char * strp, int * nump, int min,
				int max));
static const char *	getsecs P((const char * strp, long * secsp));
static const char *	getoffset P((const char * strp, long * offsetp));
static const char *	getrule P((const char * strp, struct rule * rulep));
static void		gmtload P((struct state * sp));
static void		gmtsub P((const time_t * timep, long offset,
				struct tm * tmp));
static void		localsub P((const time_t * timep, long offset,
				struct tm * tmp));
static int		increment_overflow P((int * number, int delta));
static int		normalize_overflow P((int * tensptr, int * unitsptr,
				int base));
static void		settzname P((void));
static time_t		time1 P((struct tm * tmp,
				void(*funcp) P((const time_t *,
				long, struct tm *)),
				long offset));
static time_t		time2 P((struct tm *tmp,
				void(*funcp) P((const time_t *,
				long, struct tm*)),
				long offset, int * okayp));
static time_t		time2sub P((struct tm *tmp,
				void(*funcp) P((const time_t *,
				long, struct tm*)),
				long offset, int * okayp, int do_norm_secs));
static void		timesub P((const time_t * timep, long offset,
				const struct state * sp, struct tm * tmp));
static int		tmcomp P((const struct tm * atmp,
				const struct tm * btmp));
static time_t		transtime P((time_t janfirst, int year,
				const struct rule * rulep, long offset));
static int		tzload P((const char * name, struct state * sp));
static int		tzparse P((const char * name, struct state * sp,
				int lastditch));

#ifdef ALL_STATE
static struct state *	lclptr;
static struct state *	gmtptr;
#endif /* defined ALL_STATE */

#ifndef ALL_STATE
static struct state	lclmem;
static struct state	gmtmem;
#define lclptr		(&lclmem)
#define gmtptr		(&gmtmem)
#endif /* State Farm */

#ifndef TZ_STRLEN_MAX
#define TZ_STRLEN_MAX 255
#endif /* !defined TZ_STRLEN_MAX */

static char		lcl_TZname[TZ_STRLEN_MAX + 1];
static int		lcl_is_set;
static int		gmt_is_set;

char *			tzname[2] = {
	wildabbr,
	wildabbr
};

/*
** Section 4.12.3 of X3.159-1989 requires that
**	Except for the strftime function, these functions [asctime,
**	ctime, gmtime, localtime] return values in one of two static
**	objects: a broken-down time structure and an array of char.
** Thanks to Paul Eggert (eggert@twinsun.com) for noting this.
*/

static struct tm	tm;

#ifdef USG_COMPAT
time_t			timezone = 0;
int			daylight = 0;
#endif /* defined USG_COMPAT */

#ifdef ALTZONE
time_t			altzone = 0;
#endif /* defined ALTZONE */

static long
detzcode(codep)
const char * const	codep;
{
	register long	result;
	register int	i;

	result = (codep[0] & 0x80) ? ~0L : 0L;
	for (i = 0; i < 4; ++i)
		result = (result << 8) | (codep[i] & 0xff);
	return result;
}

static void
settzname P((void))
{
	register struct state * const	sp = lclptr;
	register int			i;

	tzname[0] = wildabbr;
	tzname[1] = wildabbr;
#ifdef USG_COMPAT
	daylight = 0;
	timezone = 0;
#endif /* defined USG_COMPAT */
#ifdef ALTZONE
	altzone = 0;
#endif /* defined ALTZONE */
#ifdef ALL_STATE
	if (sp == NULL) {
		tzname[0] = tzname[1] = gmt;
		return;
	}
#endif /* defined ALL_STATE */
	for (i = 0; i < sp->typecnt; ++i) {
		register const struct ttinfo * const	ttisp = &sp->ttis[i];

		tzname[ttisp->tt_isdst] =
			&sp->chars[ttisp->tt_abbrind];
#ifdef USG_COMPAT
		if (ttisp->tt_isdst)
			daylight = 1;
		if (i == 0 || !ttisp->tt_isdst)
			timezone = -(ttisp->tt_gmtoff);
#endif /* defined USG_COMPAT */
#ifdef ALTZONE
		if (i == 0 || ttisp->tt_isdst)
			altzone = -(ttisp->tt_gmtoff);
#endif /* defined ALTZONE */
	}
	/*
	** And to get the latest zone names into tzname. . .
	*/
	for (i = 0; i < sp->timecnt; ++i) {
		register const struct ttinfo * const	ttisp =
							&sp->ttis[
								sp->types[i]];

		tzname[ttisp->tt_isdst] =
			&sp->chars[ttisp->tt_abbrind];
	}
}

static int
tzload(name, sp)
register const char *		name;
register struct state * const	sp;
{
	register const char *	p;
	register int		i;
	register int		fid;

	if (name == NULL && (name = TZDEFAULT) == NULL)
		return -1;
	{
		register int	doaccess;
		/*
		** Section 4.9.1 of the C standard says that
		** "FILENAME_MAX expands to an integral constant expression
		** that is the size needed for an array of char large enough
		** to hold the longest file name string that the implementation
		** guarantees can be opened."
		*/
		char		fullname[FILENAME_MAX + 1];

		if (name[0] == ':')
			++name;
		doaccess = name[0] == '/';
		if (!doaccess) {
			if ((p = TZDIR) == NULL)
				return -1;
			if ((strlen(p) + strlen(name) + 1) >= sizeof fullname)
				return -1;
			(void) strcpy(fullname, p);
			(void) strcat(fullname, "/");
			(void) strcat(fullname, name);
			/*
			** Set doaccess if '.' (as in "../") shows up in name.
			*/
			if (strchr(name, '.') != NULL)
				doaccess = TRUE;
			name = fullname;
		}
		if (doaccess && access(name, R_OK) != 0)
			return -1;
		if ((fid = open(name, OPEN_MODE)) == -1)
			return -1;
	}
	{
		struct tzhead *	tzhp;
		union {
			struct tzhead	tzhead;
			char		buf[sizeof *sp + sizeof *tzhp];
		} u;
		int		ttisstdcnt;
		int		ttisgmtcnt;

		i = read(fid, u.buf, sizeof u.buf);
		if (close(fid) != 0)
			return -1;
		ttisstdcnt = (int) detzcode(u.tzhead.tzh_ttisstdcnt);
		ttisgmtcnt = (int) detzcode(u.tzhead.tzh_ttisgmtcnt);
		sp->leapcnt = (int) detzcode(u.tzhead.tzh_leapcnt);
		sp->timecnt = (int) detzcode(u.tzhead.tzh_timecnt);
		sp->typecnt = (int) detzcode(u.tzhead.tzh_typecnt);
		sp->charcnt = (int) detzcode(u.tzhead.tzh_charcnt);
		p = u.tzhead.tzh_charcnt + sizeof u.tzhead.tzh_charcnt;
		if (sp->leapcnt < 0 || sp->leapcnt > TZ_MAX_LEAPS ||
			sp->typecnt <= 0 || sp->typecnt > TZ_MAX_TYPES ||
			sp->timecnt < 0 || sp->timecnt > TZ_MAX_TIMES ||
			sp->charcnt < 0 || sp->charcnt > TZ_MAX_CHARS ||
			(ttisstdcnt != sp->typecnt && ttisstdcnt != 0) ||
			(ttisgmtcnt != sp->typecnt && ttisgmtcnt != 0))
				return -1;
		if (i - (p - u.buf) < sp->timecnt * 4 +	/* ats */
			sp->timecnt +			/* types */
			sp->typecnt * (4 + 2) +		/* ttinfos */
			sp->charcnt +			/* chars */
			sp->leapcnt * (4 + 4) +		/* lsinfos */
			ttisstdcnt +			/* ttisstds */
			ttisgmtcnt)			/* ttisgmts */
				return -1;
		for (i = 0; i < sp->timecnt; ++i) {
			sp->ats[i] = detzcode(p);
			p += 4;
		}
		for (i = 0; i < sp->timecnt; ++i) {
			sp->types[i] = (unsigned char) *p++;
			if (sp->types[i] >= sp->typecnt)
				return -1;
		}
		for (i = 0; i < sp->typecnt; ++i) {
			register struct ttinfo *	ttisp;

			ttisp = &sp->ttis[i];
			ttisp->tt_gmtoff = detzcode(p);
			p += 4;
			ttisp->tt_isdst = (unsigned char) *p++;
			if (ttisp->tt_isdst != 0 && ttisp->tt_isdst != 1)
				return -1;
			ttisp->tt_abbrind = (unsigned char) *p++;
			if (ttisp->tt_abbrind < 0 ||
				ttisp->tt_abbrind > sp->charcnt)
					return -1;
		}
		for (i = 0; i < sp->charcnt; ++i)
			sp->chars[i] = *p++;
		sp->chars[i] = '\0';	/* ensure '\0' at end */
		for (i = 0; i < sp->leapcnt; ++i) {
			register struct lsinfo *	lsisp;

			lsisp = &sp->lsis[i];
			lsisp->ls_trans = detzcode(p);
			p += 4;
			lsisp->ls_corr = detzcode(p);
			p += 4;
		}
		for (i = 0; i < sp->typecnt; ++i) {
			register struct ttinfo *	ttisp;

			ttisp = &sp->ttis[i];
			if (ttisstdcnt == 0)
				ttisp->tt_ttisstd = FALSE;
			else {
				ttisp->tt_ttisstd = *p++;
				if (ttisp->tt_ttisstd != TRUE &&
					ttisp->tt_ttisstd != FALSE)
						return -1;
			}
		}
		for (i = 0; i < sp->typecnt; ++i) {
			register struct ttinfo *	ttisp;

			ttisp = &sp->ttis[i];
			if (ttisgmtcnt == 0)
				ttisp->tt_ttisgmt = FALSE;
			else {
				ttisp->tt_ttisgmt = *p++;
				if (ttisp->tt_ttisgmt != TRUE &&
					ttisp->tt_ttisgmt != FALSE)
						return -1;
			}
		}
	}
	return 0;
}

static const int	mon_lengths[2][MONSPERYEAR] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

static const int	year_lengths[2] = {
	DAYSPERNYEAR, DAYSPERLYEAR
};

/*
** Given a pointer into a time zone string, scan until a character that is not
** a valid character in a zone name is found.  Return a pointer to that
** character.
*/

static const char *
getzname(strp)
register const char *	strp;
{
	register char	c;

	while ((c = *strp) != '\0' && !is_digit(c) && c != ',' && c != '-' &&
		c != '+')
			++strp;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a number from that string.
** Check that the number is within a specified range; if it is not, return
** NULL.
** Otherwise, return a pointer to the first character not part of the number.
*/

static const char *
getnum(strp, nump, min, max)
register const char *	strp;
int * const		nump;
const int		min;
const int		max;
{
	register char	c;
	register int	num;

	if (strp == NULL || !is_digit(c = *strp))
		return NULL;
	num = 0;
	do {
		num = num * 10 + (c - '0');
		if (num > max)
			return NULL;	/* illegal value */
		c = *++strp;
	} while (is_digit(c));
	if (num < min)
		return NULL;		/* illegal value */
	*nump = num;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a number of seconds,
** in hh[:mm[:ss]] form, from the string.
** If any error occurs, return NULL.
** Otherwise, return a pointer to the first character not part of the number
** of seconds.
*/

static const char *
getsecs(strp, secsp)
register const char *	strp;
long * const		secsp;
{
	int	num;

	/*
	** `HOURSPERDAY * DAYSPERWEEK - 1' allows quasi-Posix rules like
	** "M10.4.6/26", which does not conform to Posix,
	** but which specifies the equivalent of
	** ``02:00 on the first Sunday on or after 23 Oct''.
	*/
	strp = getnum(strp, &num, 0, HOURSPERDAY * DAYSPERWEEK - 1);
	if (strp == NULL)
		return NULL;
	*secsp = num * (long) SECSPERHOUR;
	if (*strp == ':') {
		++strp;
		strp = getnum(strp, &num, 0, MINSPERHOUR - 1);
		if (strp == NULL)
			return NULL;
		*secsp += num * SECSPERMIN;
		if (*strp == ':') {
			++strp;
			/* `SECSPERMIN' allows for leap seconds.  */
			strp = getnum(strp, &num, 0, SECSPERMIN);
			if (strp == NULL)
				return NULL;
			*secsp += num;
		}
	}
	return strp;
}

/*
** Given a pointer into a time zone string, extract an offset, in
** [+-]hh[:mm[:ss]] form, from the string.
** If any error occurs, return NULL.
** Otherwise, return a pointer to the first character not part of the time.
*/

static const char *
getoffset(strp, offsetp)
register const char *	strp;
long * const		offsetp;
{
	register int	neg = 0;

	if (*strp == '-') {
		neg = 1;
		++strp;
	} else if (*strp == '+')
		++strp;
	strp = getsecs(strp, offsetp);
	if (strp == NULL)
		return NULL;		/* illegal time */
	if (neg)
		*offsetp = -*offsetp;
	return strp;
}

/*
** Given a pointer into a time zone string, extract a rule in the form
** date[/time].  See POSIX section 8 for the format of "date" and "time".
** If a valid rule is not found, return NULL.
** Otherwise, return a pointer to the first character not part of the rule.
*/

static const char *
getrule(strp, rulep)
const char *			strp;
register struct rule * const	rulep;
{
	if (*strp == 'J') {
		/*
		** Julian day.
		*/
		rulep->r_type = JULIAN_DAY;
		++strp;
		strp = getnum(strp, &rulep->r_day, 1, DAYSPERNYEAR);
	} else if (*strp == 'M') {
		/*
		** Month, week, day.
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
	} else if (is_digit(*strp)) {
		/*
		** Day of year.
		*/
		rulep->r_type = DAY_OF_YEAR;
		strp = getnum(strp, &rulep->r_day, 0, DAYSPERLYEAR - 1);
	} else	return NULL;		/* invalid format */
	if (strp == NULL)
		return NULL;
	if (*strp == '/') {
		/*
		** Time specified.
		*/
		++strp;
		strp = getsecs(strp, &rulep->r_time);
	} else	rulep->r_time = 2 * SECSPERHOUR;	/* default = 2:00:00 */
	return strp;
}

/*
** Given the Epoch-relative time of January 1, 00:00:00 UTC, in a year, the
** year, a rule, and the offset from UTC at the time that rule takes effect,
** calculate the Epoch-relative time that rule takes effect.
*/

static time_t
transtime(janfirst, year, rulep, offset)
const time_t				janfirst;
const int				year;
register const struct rule * const	rulep;
const long				offset;
{
	register int	leapyear;
	register time_t	value;
	register int	i;
	int		d, m1, yy0, yy1, yy2, dow;

	INITIALIZE(value);
	leapyear = isleap(year);
	switch (rulep->r_type) {

	case JULIAN_DAY:
		/*
		** Jn - Julian day, 1 == January 1, 60 == March 1 even in leap
		** years.
		** In non-leap years, or if the day number is 59 or less, just
		** add SECSPERDAY times the day number-1 to the time of
		** January 1, midnight, to get the day.
		*/
		value = janfirst + (rulep->r_day - 1) * SECSPERDAY;
		if (leapyear && rulep->r_day >= 60)
			value += SECSPERDAY;
		break;

	case DAY_OF_YEAR:
		/*
		** n - day of year.
		** Just add SECSPERDAY times the day number to the time of
		** January 1, midnight, to get the day.
		*/
		value = janfirst + rulep->r_day * SECSPERDAY;
		break;

	case MONTH_NTH_DAY_OF_WEEK:
		/*
		** Mm.n.d - nth "dth day" of month m.
		*/
		value = janfirst;
		for (i = 0; i < rulep->r_mon - 1; ++i)
			value += mon_lengths[leapyear][i] * SECSPERDAY;

		/*
		** Use Zeller's Congruence to get day-of-week of first day of
		** month.
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
		** "dow" is the day-of-week of the first day of the month.  Get
		** the day-of-month (zero-origin) of the first "dow" day of the
		** month.
		*/
		d = rulep->r_day - dow;
		if (d < 0)
			d += DAYSPERWEEK;
		for (i = 1; i < rulep->r_week; ++i) {
			if (d + DAYSPERWEEK >=
				mon_lengths[leapyear][rulep->r_mon - 1])
					break;
			d += DAYSPERWEEK;
		}

		/*
		** "d" is the day-of-month (zero-origin) of the day we want.
		*/
		value += d * SECSPERDAY;
		break;
	}

	/*
	** "value" is the Epoch-relative time of 00:00:00 UTC on the day in
	** question.  To get the Epoch-relative time of the specified local
	** time on that day, add the transition time and the current offset
	** from UTC.
	*/
	return value + rulep->r_time + offset;
}

/*
** Given a POSIX section 8-style TZ string, fill in the rule tables as
** appropriate.
*/

static int
tzparse(name, sp, lastditch)
const char *			name;
register struct state * const	sp;
const int			lastditch;
{
	const char *			stdname;
	const char *			dstname;
	size_t				stdlen;
	size_t				dstlen;
	long				stdoffset;
	long				dstoffset;
	register time_t *		atp;
	register unsigned char *	typep;
	register char *			cp;
	register int			load_result;

	INITIALIZE(dstname);
	stdname = name;
	if (lastditch) {
		stdlen = strlen(name);	/* length of standard zone name */
		name += stdlen;
		if (stdlen >= sizeof sp->chars)
			stdlen = (sizeof sp->chars) - 1;
		stdoffset = 0;
	} else {
		name = getzname(name);
		stdlen = name - stdname;
		if (stdlen < 3)
			return -1;
		if (*name == '\0')
			return -1;
		name = getoffset(name, &stdoffset);
		if (name == NULL)
			return -1;
	}
	load_result = tzload(TZDEFRULES, sp);
	if (load_result != 0)
		sp->leapcnt = 0;		/* so, we're off a little */
	if (*name != '\0') {
		dstname = name;
		name = getzname(name);
		dstlen = name - dstname;	/* length of DST zone name */
		if (dstlen < 3)
			return -1;
		if (*name != '\0' && *name != ',' && *name != ';') {
			name = getoffset(name, &dstoffset);
			if (name == NULL)
				return -1;
		} else	dstoffset = stdoffset - SECSPERHOUR;
		if (*name == '\0' && load_result != 0)
			name = TZDEFRULESTRING;
		if (*name == ',' || *name == ';') {
			struct rule	start;
			struct rule	end;
			register int	year;
			register time_t	janfirst;
			time_t		starttime;
			time_t		endtime;

			++name;
			if ((name = getrule(name, &start)) == NULL)
				return -1;
			if (*name++ != ',')
				return -1;
			if ((name = getrule(name, &end)) == NULL)
				return -1;
			if (*name != '\0')
				return -1;
			sp->typecnt = 2;	/* standard time and DST */
			/*
			** Two transitions per year, from EPOCH_YEAR to 2037.
			*/
			sp->timecnt = 2 * (2037 - EPOCH_YEAR + 1);
			if (sp->timecnt > TZ_MAX_TIMES)
				return -1;
			sp->ttis[0].tt_gmtoff = -dstoffset;
			sp->ttis[0].tt_isdst = 1;
			sp->ttis[0].tt_abbrind = stdlen + 1;
			sp->ttis[1].tt_gmtoff = -stdoffset;
			sp->ttis[1].tt_isdst = 0;
			sp->ttis[1].tt_abbrind = 0;
			atp = sp->ats;
			typep = sp->types;
			janfirst = 0;
			for (year = EPOCH_YEAR; year <= 2037; ++year) {
				starttime = transtime(janfirst, year, &start,
					stdoffset);
				endtime = transtime(janfirst, year, &end,
					dstoffset);
				if (starttime > endtime) {
					*atp++ = endtime;
					*typep++ = 1;	/* DST ends */
					*atp++ = starttime;
					*typep++ = 0;	/* DST begins */
				} else {
					*atp++ = starttime;
					*typep++ = 0;	/* DST begins */
					*atp++ = endtime;
					*typep++ = 1;	/* DST ends */
				}
				janfirst += year_lengths[isleap(year)] *
					SECSPERDAY;
			}
		} else {
			register long	theirstdoffset;
			register long	theirdstoffset;
			register long	theiroffset;
			register int	isdst;
			register int	i;
			register int	j;

			if (*name != '\0')
				return -1;
			/*
			** Initial values of theirstdoffset and theirdstoffset.
			*/
			theirstdoffset = 0;
			for (i = 0; i < sp->timecnt; ++i) {
				j = sp->types[i];
				if (!sp->ttis[j].tt_isdst) {
					theirstdoffset =
						-sp->ttis[j].tt_gmtoff;
					break;
				}
			}
			theirdstoffset = 0;
			for (i = 0; i < sp->timecnt; ++i) {
				j = sp->types[i];
				if (sp->ttis[j].tt_isdst) {
					theirdstoffset =
						-sp->ttis[j].tt_gmtoff;
					break;
				}
			}
			/*
			** Initially we're assumed to be in standard time.
			*/
			isdst = FALSE;
			theiroffset = theirstdoffset;
			/*
			** Now juggle transition times and types
			** tracking offsets as you do.
			*/
			for (i = 0; i < sp->timecnt; ++i) {
				j = sp->types[i];
				sp->types[i] = sp->ttis[j].tt_isdst;
				if (sp->ttis[j].tt_ttisgmt) {
					/* No adjustment to transition time */
				} else {
					/*
					** If summer time is in effect, and the
					** transition time was not specified as
					** standard time, add the summer time
					** offset to the transition time;
					** otherwise, add the standard time
					** offset to the transition time.
					*/
					/*
					** Transitions from DST to DDST
					** will effectively disappear since
					** POSIX provides for only one DST
					** offset.
					*/
					if (isdst && !sp->ttis[j].tt_ttisstd) {
						sp->ats[i] += dstoffset -
							theirdstoffset;
					} else {
						sp->ats[i] += stdoffset -
							theirstdoffset;
					}
				}
				theiroffset = -sp->ttis[j].tt_gmtoff;
				if (sp->ttis[j].tt_isdst)
					theirdstoffset = theiroffset;
				else	theirstdoffset = theiroffset;
			}
			/*
			** Finally, fill in ttis.
			** ttisstd and ttisgmt need not be handled.
			*/
			sp->ttis[0].tt_gmtoff = -stdoffset;
			sp->ttis[0].tt_isdst = FALSE;
			sp->ttis[0].tt_abbrind = 0;
			sp->ttis[1].tt_gmtoff = -dstoffset;
			sp->ttis[1].tt_isdst = TRUE;
			sp->ttis[1].tt_abbrind = stdlen + 1;
			sp->typecnt = 2;
		}
	} else {
		dstlen = 0;
		sp->typecnt = 1;		/* only standard time */
		sp->timecnt = 0;
		sp->ttis[0].tt_gmtoff = -stdoffset;
		sp->ttis[0].tt_isdst = 0;
		sp->ttis[0].tt_abbrind = 0;
	}
	sp->charcnt = stdlen + 1;
	if (dstlen != 0)
		sp->charcnt += dstlen + 1;
	if ((size_t) sp->charcnt > sizeof sp->chars)
		return -1;
	cp = sp->chars;
	(void) strncpy(cp, stdname, stdlen);
	cp += stdlen;
	*cp++ = '\0';
	if (dstlen != 0) {
		(void) strncpy(cp, dstname, dstlen);
		*(cp + dstlen) = '\0';
	}
	return 0;
}

static void
gmtload(sp)
struct state * const	sp;
{
	if (tzload(gmt, sp) != 0)
		(void) tzparse(gmt, sp, TRUE);
}

#ifndef STD_INSPIRED
/*
** A non-static declaration of tzsetwall in a system header file
** may cause a warning about this upcoming static declaration...
*/
static
#endif /* !defined STD_INSPIRED */
void
tzsetwall P((void))
{
	if (lcl_is_set < 0)
		return;
	lcl_is_set = -1;

#ifdef ALL_STATE
	if (lclptr == NULL) {
		lclptr = (struct state *) malloc(sizeof *lclptr);
		if (lclptr == NULL) {
			settzname();	/* all we can do */
			return;
		}
	}
#endif /* defined ALL_STATE */
	if (tzload((char *) NULL, lclptr) != 0)
		gmtload(lclptr);
	settzname();
}

void
tzset P((void))
{
	register const char *	name;

	name = getenv("TZ");
	if (name == NULL) {
		tzsetwall();
		return;
	}

	if (lcl_is_set > 0 && strcmp(lcl_TZname, name) == 0)
		return;
	lcl_is_set = strlen(name) < sizeof lcl_TZname;
	if (lcl_is_set)
		(void) strcpy(lcl_TZname, name);

#ifdef ALL_STATE
	if (lclptr == NULL) {
		lclptr = (struct state *) malloc(sizeof *lclptr);
		if (lclptr == NULL) {
			settzname();	/* all we can do */
			return;
		}
	}
#endif /* defined ALL_STATE */
	if (*name == '\0') {
		/*
		** User wants it fast rather than right.
		*/
		lclptr->leapcnt = 0;		/* so, we're off a little */
		lclptr->timecnt = 0;
		lclptr->typecnt = 0;
		lclptr->ttis[0].tt_isdst = 0;
		lclptr->ttis[0].tt_gmtoff = 0;
		lclptr->ttis[0].tt_abbrind = 0;
		(void) strcpy(lclptr->chars, gmt);
	} else if (tzload(name, lclptr) != 0)
		if (name[0] == ':' || tzparse(name, lclptr, FALSE) != 0)
			(void) gmtload(lclptr);
	settzname();
}

/*
** The easy way to behave "as if no library function calls" localtime
** is to not call it--so we drop its guts into "localsub", which can be
** freely called.  (And no, the PANS doesn't require the above behavior--
** but it *is* desirable.)
**
** The unused offset argument is for the benefit of mktime variants.
*/

/*ARGSUSED*/
static void
localsub(timep, offset, tmp)
const time_t * const	timep;
const long		offset;
struct tm * const	tmp;
{
	register struct state *		sp;
	register const struct ttinfo *	ttisp;
	register int			i;
	const time_t			t = *timep;

	sp = lclptr;
#ifdef ALL_STATE
	if (sp == NULL) {
		gmtsub(timep, offset, tmp);
		return;
	}
#endif /* defined ALL_STATE */
	if (sp->timecnt == 0 || t < sp->ats[0]) {
		i = 0;
		while (sp->ttis[i].tt_isdst)
			if (++i >= sp->typecnt) {
				i = 0;
				break;
			}
	} else {
		for (i = 1; i < sp->timecnt; ++i)
			if (t < sp->ats[i])
				break;
		i = sp->types[i - 1];
	}
	ttisp = &sp->ttis[i];
	/*
	** To get (wrong) behavior that's compatible with System V Release 2.0
	** you'd replace the statement below with
	**	t += ttisp->tt_gmtoff;
	**	timesub(&t, 0L, sp, tmp);
	*/
	timesub(&t, ttisp->tt_gmtoff, sp, tmp);
	tmp->tm_isdst = ttisp->tt_isdst;
	tzname[tmp->tm_isdst] = &sp->chars[ttisp->tt_abbrind];
#ifdef TM_ZONE
	tmp->TM_ZONE = &sp->chars[ttisp->tt_abbrind];
#endif /* defined TM_ZONE */
}

struct tm *
localtime(timep)
const time_t * const	timep;
{
	tzset();
	localsub(timep, 0L, &tm);
	return &tm;
}

/*
** Re-entrant version of localtime.
*/

struct tm *
localtime_r(timep, tm)
const time_t * const	timep;
struct tm *		tm;
{
	localsub(timep, 0L, tm);
	return tm;
}

/*
** gmtsub is to gmtime as localsub is to localtime.
*/

static void
gmtsub(timep, offset, tmp)
const time_t * const	timep;
const long		offset;
struct tm * const	tmp;
{
	if (!gmt_is_set) {
		gmt_is_set = TRUE;
#ifdef ALL_STATE
		gmtptr = (struct state *) malloc(sizeof *gmtptr);
		if (gmtptr != NULL)
#endif /* defined ALL_STATE */
			gmtload(gmtptr);
	}
	timesub(timep, offset, gmtptr, tmp);
#ifdef TM_ZONE
	/*
	** Could get fancy here and deliver something such as
	** "UTC+xxxx" or "UTC-xxxx" if offset is non-zero,
	** but this is no time for a treasure hunt.
	*/
	if (offset != 0)
		tmp->TM_ZONE = wildabbr;
	else {
#ifdef ALL_STATE
		if (gmtptr == NULL)
			tmp->TM_ZONE = gmt;
		else	tmp->TM_ZONE = gmtptr->chars;
#endif /* defined ALL_STATE */
#ifndef ALL_STATE
		tmp->TM_ZONE = gmtptr->chars;
#endif /* State Farm */
	}
#endif /* defined TM_ZONE */
}

struct tm *
gmtime(timep)
const time_t * const	timep;
{
	gmtsub(timep, 0L, &tm);
	return &tm;
}

/*
* Re-entrant version of gmtime.
*/

struct tm *
gmtime_r(timep, tm)
const time_t * const	timep;
struct tm *		tm;
{
	gmtsub(timep, 0L, tm);
	return tm;
}

#ifdef STD_INSPIRED

struct tm *
offtime(timep, offset)
const time_t * const	timep;
const long		offset;
{
	gmtsub(timep, offset, &tm);
	return &tm;
}

#endif /* defined STD_INSPIRED */

static void
timesub(timep, offset, sp, tmp)
const time_t * const			timep;
const long				offset;
register const struct state * const	sp;
register struct tm * const		tmp;
{
	register const struct lsinfo *	lp;
	register long			days;
	register long			rem;
	register int			y;
	register int			yleap;
	register const int *		ip;
	register long			corr;
	register int			hit;
	register int			i;

	corr = 0;
	hit = 0;
#ifdef ALL_STATE
	i = (sp == NULL) ? 0 : sp->leapcnt;
#endif /* defined ALL_STATE */
#ifndef ALL_STATE
	i = sp->leapcnt;
#endif /* State Farm */
	while (--i >= 0) {
		lp = &sp->lsis[i];
		if (*timep >= lp->ls_trans) {
			if (*timep == lp->ls_trans) {
				hit = ((i == 0 && lp->ls_corr > 0) ||
					lp->ls_corr > sp->lsis[i - 1].ls_corr);
				if (hit)
					while (i > 0 &&
						sp->lsis[i].ls_trans ==
						sp->lsis[i - 1].ls_trans + 1 &&
						sp->lsis[i].ls_corr ==
						sp->lsis[i - 1].ls_corr + 1) {
							++hit;
							--i;
					}
			}
			corr = lp->ls_corr;
			break;
		}
	}
	days = *timep / SECSPERDAY;
	rem = *timep % SECSPERDAY;
#ifdef mc68k
	if (*timep == 0x80000000) {
		/*
		** A 3B1 muffs the division on the most negative number.
		*/
		days = -24855;
		rem = -11648;
	}
#endif /* defined mc68k */
	rem += (offset - corr);
	while (rem < 0) {
		rem += SECSPERDAY;
		--days;
	}
	while (rem >= SECSPERDAY) {
		rem -= SECSPERDAY;
		++days;
	}
	tmp->tm_hour = (int) (rem / SECSPERHOUR);
	rem = rem % SECSPERHOUR;
	tmp->tm_min = (int) (rem / SECSPERMIN);
	/*
	** A positive leap second requires a special
	** representation.  This uses "... ??:59:60" et seq.
	*/
	tmp->tm_sec = (int) (rem % SECSPERMIN) + hit;
	tmp->tm_wday = (int) ((EPOCH_WDAY + days) % DAYSPERWEEK);
	if (tmp->tm_wday < 0)
		tmp->tm_wday += DAYSPERWEEK;
	y = EPOCH_YEAR;
#define LEAPS_THRU_END_OF(y)	((y) / 4 - (y) / 100 + (y) / 400)
	while (days < 0 || days >= (long) year_lengths[yleap = isleap(y)]) {
		register int	newy;

		newy = y + days / DAYSPERNYEAR;
		if (days < 0)
			--newy;
		days -= (newy - y) * DAYSPERNYEAR +
			LEAPS_THRU_END_OF(newy - 1) -
			LEAPS_THRU_END_OF(y - 1);
		y = newy;
	}
	tmp->tm_year = y - TM_YEAR_BASE;
	tmp->tm_yday = (int) days;
	ip = mon_lengths[yleap];
	for (tmp->tm_mon = 0; days >= (long) ip[tmp->tm_mon]; ++(tmp->tm_mon))
		days = days - (long) ip[tmp->tm_mon];
	tmp->tm_mday = (int) (days + 1);
	tmp->tm_isdst = 0;
#ifdef TM_GMTOFF
	tmp->TM_GMTOFF = offset;
#endif /* defined TM_GMTOFF */
}

char *
ctime(timep)
const time_t * const	timep;
{
/*
** Section 4.12.3.2 of X3.159-1989 requires that
**	The ctime function converts the calendar time pointed to by timer
**	to local time in the form of a string.  It is equivalent to
**		asctime(localtime(timer))
*/
	return asctime(localtime(timep));
}

char *
ctime_r(timep, buf)
const time_t * const	timep;
char *			buf;
{
	struct tm	tm;

	return asctime_r(localtime_r(timep, &tm), buf);
}

/*
** Adapted from code provided by Robert Elz, who writes:
**	The "best" way to do mktime I think is based on an idea of Bob
**	Kridle's (so its said...) from a long time ago.
**	[kridle@xinet.com as of 1996-01-16.]
**	It does a binary search of the time_t space.  Since time_t's are
**	just 32 bits, its a max of 32 iterations (even at 64 bits it
**	would still be very reasonable).
*/

#ifndef WRONG
#define WRONG	(-1)
#endif /* !defined WRONG */

/*
** Simplified normalize logic courtesy Paul Eggert (eggert@twinsun.com).
*/

static int
increment_overflow(number, delta)
int *	number;
int	delta;
{
	int	number0;

	number0 = *number;
	*number += delta;
	return (*number < number0) != (delta < 0);
}

static int
normalize_overflow(tensptr, unitsptr, base)
int * const	tensptr;
int * const	unitsptr;
const int	base;
{
	register int	tensdelta;

	tensdelta = (*unitsptr >= 0) ?
		(*unitsptr / base) :
		(-1 - (-1 - *unitsptr) / base);
	*unitsptr -= tensdelta * base;
	return increment_overflow(tensptr, tensdelta);
}

static int
tmcomp(atmp, btmp)
register const struct tm * const atmp;
register const struct tm * const btmp;
{
	register int	result;

	if ((result = (atmp->tm_year - btmp->tm_year)) == 0 &&
		(result = (atmp->tm_mon - btmp->tm_mon)) == 0 &&
		(result = (atmp->tm_mday - btmp->tm_mday)) == 0 &&
		(result = (atmp->tm_hour - btmp->tm_hour)) == 0 &&
		(result = (atmp->tm_min - btmp->tm_min)) == 0)
			result = atmp->tm_sec - btmp->tm_sec;
	return result;
}

static time_t
time2sub(tmp, funcp, offset, okayp, do_norm_secs)
struct tm * const	tmp;
void (* const		funcp) P((const time_t*, long, struct tm*));
const long		offset;
int * const		okayp;
const int		do_norm_secs;
{
	register const struct state *	sp;
	register int			dir;
	register int			bits;
	register int			i, j ;
	register int			saved_seconds;
	time_t				newt;
	time_t				t;
	struct tm			yourtm, mytm;

	*okayp = FALSE;
	yourtm = *tmp;
	if (do_norm_secs) {
		if (normalize_overflow(&yourtm.tm_min, &yourtm.tm_sec,
			SECSPERMIN))
				return WRONG;
	}
	if (normalize_overflow(&yourtm.tm_hour, &yourtm.tm_min, MINSPERHOUR))
		return WRONG;
	if (normalize_overflow(&yourtm.tm_mday, &yourtm.tm_hour, HOURSPERDAY))
		return WRONG;
	if (normalize_overflow(&yourtm.tm_year, &yourtm.tm_mon, MONSPERYEAR))
		return WRONG;
	/*
	** Turn yourtm.tm_year into an actual year number for now.
	** It is converted back to an offset from TM_YEAR_BASE later.
	*/
	if (increment_overflow(&yourtm.tm_year, TM_YEAR_BASE))
		return WRONG;
	while (yourtm.tm_mday <= 0) {
		if (increment_overflow(&yourtm.tm_year, -1))
			return WRONG;
		i = yourtm.tm_year + (1 < yourtm.tm_mon);
		yourtm.tm_mday += year_lengths[isleap(i)];
	}
	while (yourtm.tm_mday > DAYSPERLYEAR) {
		i = yourtm.tm_year + (1 < yourtm.tm_mon);
		yourtm.tm_mday -= year_lengths[isleap(i)];
		if (increment_overflow(&yourtm.tm_year, 1))
			return WRONG;
	}
	for ( ; ; ) {
		i = mon_lengths[isleap(yourtm.tm_year)][yourtm.tm_mon];
		if (yourtm.tm_mday <= i)
			break;
		yourtm.tm_mday -= i;
		if (++yourtm.tm_mon >= MONSPERYEAR) {
			yourtm.tm_mon = 0;
			if (increment_overflow(&yourtm.tm_year, 1))
				return WRONG;
		}
	}
	if (increment_overflow(&yourtm.tm_year, -TM_YEAR_BASE))
		return WRONG;
	if (yourtm.tm_sec >= 0 && yourtm.tm_sec < SECSPERMIN)
		saved_seconds = 0;
	else if (yourtm.tm_year + TM_YEAR_BASE < EPOCH_YEAR) {
		/*
		** We can't set tm_sec to 0, because that might push the
		** time below the minimum representable time.
		** Set tm_sec to 59 instead.
		** This assumes that the minimum representable time is
		** not in the same minute that a leap second was deleted from,
		** which is a safer assumption than using 58 would be.
		*/
		if (increment_overflow(&yourtm.tm_sec, 1 - SECSPERMIN))
			return WRONG;
		saved_seconds = yourtm.tm_sec;
		yourtm.tm_sec = SECSPERMIN - 1;
	} else {
		saved_seconds = yourtm.tm_sec;
		yourtm.tm_sec = 0;
	}
	/*
	** Divide the search space in half
	** (this works whether time_t is signed or unsigned).
	*/
	bits = TYPE_BIT(time_t) - 1;
	/*
	** If time_t is signed, then 0 is just above the median,
	** assuming two's complement arithmetic.
	** If time_t is unsigned, then (1 << bits) is just above the median.
	*/
	t = TYPE_SIGNED(time_t) ? 0 : (((time_t) 1) << bits);
	for ( ; ; ) {
		(*funcp)(&t, offset, &mytm);
		dir = tmcomp(&mytm, &yourtm);
		if (dir != 0) {
			if (bits-- < 0)
				return WRONG;
			if (bits < 0)
				--t; /* may be needed if new t is minimal */
			else if (dir > 0)
				t -= ((time_t) 1) << bits;
			else	t += ((time_t) 1) << bits;
			continue;
		}
		if (yourtm.tm_isdst < 0 || mytm.tm_isdst == yourtm.tm_isdst)
			break;
		/*
		** Right time, wrong type.
		** Hunt for right time, right type.
		** It's okay to guess wrong since the guess
		** gets checked.
		*/
		/*
		** The (void *) casts are the benefit of SunOS 3.3 on Sun 2's.
		*/
		sp = (const struct state *)
			(((void *) funcp == (void *) localsub) ?
			lclptr : gmtptr);
#ifdef ALL_STATE
		if (sp == NULL)
			return WRONG;
#endif /* defined ALL_STATE */
		for (i = sp->typecnt - 1; i >= 0; --i) {
			if (sp->ttis[i].tt_isdst != yourtm.tm_isdst)
				continue;
			for (j = sp->typecnt - 1; j >= 0; --j) {
				if (sp->ttis[j].tt_isdst == yourtm.tm_isdst)
					continue;
				newt = t + sp->ttis[j].tt_gmtoff -
					sp->ttis[i].tt_gmtoff;
				(*funcp)(&newt, offset, &mytm);
				if (tmcomp(&mytm, &yourtm) != 0)
					continue;
				if (mytm.tm_isdst != yourtm.tm_isdst)
					continue;
				/*
				** We have a match.
				*/
				t = newt;
				goto label;
			}
		}
		return WRONG;
	}
label:
	newt = t + saved_seconds;
	if ((newt < t) != (saved_seconds < 0))
		return WRONG;
	t = newt;
	(*funcp)(&t, offset, tmp);
	*okayp = TRUE;
	return t;
}

static time_t
time2(tmp, funcp, offset, okayp)
struct tm * const	tmp;
void (* const		funcp) P((const time_t*, long, struct tm*));
const long		offset;
int * const		okayp;
{
	time_t	t;

	/*
	** First try without normalization of seconds
	** (in case tm_sec contains a value associated with a leap second).
	** If that fails, try with normalization of seconds.
	*/
	t = time2sub(tmp, funcp, offset, okayp, FALSE);
	return *okayp ? t : time2sub(tmp, funcp, offset, okayp, TRUE);
}

static time_t
time1(tmp, funcp, offset)
struct tm * const	tmp;
void (* const		funcp) P((const time_t *, long, struct tm *));
const long		offset;
{
	register time_t			t;
	register const struct state *	sp;
	register int			samei, otheri;
	register int			sameind, otherind;
	register int			i;
	register int			nseen;
	int				seen[TZ_MAX_TYPES];
	int				types[TZ_MAX_TYPES];
	int				okay;

	if (tmp->tm_isdst > 1)
		tmp->tm_isdst = 1;
	t = time2(tmp, funcp, offset, &okay);
#ifdef PCTS
	/*
	** PCTS code courtesy Grant Sullivan (grant@osf.org).
	*/
	if (okay)
		return t;
	if (tmp->tm_isdst < 0)
		tmp->tm_isdst = 0;	/* reset to std and try again */
#endif /* defined PCTS */
#ifndef PCTS
	if (okay || tmp->tm_isdst < 0)
		return t;
#endif /* !defined PCTS */
	/*
	** We're supposed to assume that somebody took a time of one type
	** and did some math on it that yielded a "struct tm" that's bad.
	** We try to divine the type they started from and adjust to the
	** type they need.
	*/
	/*
	** The (void *) casts are the benefit of SunOS 3.3 on Sun 2's.
	*/
	sp = (const struct state *) (((void *) funcp == (void *) localsub) ?
		lclptr : gmtptr);
#ifdef ALL_STATE
	if (sp == NULL)
		return WRONG;
#endif /* defined ALL_STATE */
	for (i = 0; i < sp->typecnt; ++i)
		seen[i] = FALSE;
	nseen = 0;
	for (i = sp->timecnt - 1; i >= 0; --i)
		if (!seen[sp->types[i]]) {
			seen[sp->types[i]] = TRUE;
			types[nseen++] = sp->types[i];
		}
	for (sameind = 0; sameind < nseen; ++sameind) {
		samei = types[sameind];
		if (sp->ttis[samei].tt_isdst != tmp->tm_isdst)
			continue;
		for (otherind = 0; otherind < nseen; ++otherind) {
			otheri = types[otherind];
			if (sp->ttis[otheri].tt_isdst == tmp->tm_isdst)
				continue;
			tmp->tm_sec += sp->ttis[otheri].tt_gmtoff -
					sp->ttis[samei].tt_gmtoff;
			tmp->tm_isdst = !tmp->tm_isdst;
			t = time2(tmp, funcp, offset, &okay);
			if (okay)
				return t;
			tmp->tm_sec -= sp->ttis[otheri].tt_gmtoff -
					sp->ttis[samei].tt_gmtoff;
			tmp->tm_isdst = !tmp->tm_isdst;
		}
	}
	return WRONG;
}

time_t
mktime(tmp)
struct tm * const	tmp;
{
	tzset();
	return time1(tmp, localsub, 0L);
}

#ifdef STD_INSPIRED

time_t
timelocal(tmp)
struct tm * const	tmp;
{
	tmp->tm_isdst = -1;	/* in case it wasn't initialized */
	return mktime(tmp);
}

time_t
timegm(tmp)
struct tm * const	tmp;
{
	tmp->tm_isdst = 0;
	return time1(tmp, gmtsub, 0L);
}

time_t
timeoff(tmp, offset)
struct tm * const	tmp;
const long		offset;
{
	tmp->tm_isdst = 0;
	return time1(tmp, gmtsub, offset);
}

#endif /* defined STD_INSPIRED */

#ifdef CMUCS

/*
** The following is supplied for compatibility with
** previous versions of the CMUCS runtime library.
*/

long
gtime(tmp)
struct tm * const	tmp;
{
	const time_t	t = mktime(tmp);

	if (t == WRONG)
		return -1;
	return t;
}

#endif /* defined CMUCS */

/*
** XXX--is the below the right way to conditionalize??
*/

#ifdef STD_INSPIRED

/*
** IEEE Std 1003.1-1988 (POSIX) legislates that 536457599
** shall correspond to "Wed Dec 31 23:59:59 UTC 1986", which
** is not the case if we are accounting for leap seconds.
** So, we provide the following conversion routines for use
** when exchanging timestamps with POSIX conforming systems.
*/

static long
leapcorr(timep)
time_t *	timep;
{
	register struct state *		sp;
	register struct lsinfo *	lp;
	register int			i;

	sp = lclptr;
	i = sp->leapcnt;
	while (--i >= 0) {
		lp = &sp->lsis[i];
		if (*timep >= lp->ls_trans)
			return lp->ls_corr;
	}
	return 0;
}

time_t
time2posix(t)
time_t	t;
{
	tzset();
	return t - leapcorr(&t);
}

time_t
posix2time(t)
time_t	t;
{
	time_t	x;
	time_t	y;

	tzset();
	/*
	** For a positive leap second hit, the result
	** is not unique.  For a negative leap second
	** hit, the corresponding time doesn't exist,
	** so we return an adjacent second.
	*/
	x = t + leapcorr(&t);
	y = x - leapcorr(&x);
	if (y < t) {
		do {
			x++;
			y = x - leapcorr(&x);
		} while (y < t);
		if (t != y)
			return x - 1;
	} else if (y > t) {
		do {
			--x;
			y = x - leapcorr(&x);
		} while (y > t);
		if (t != y)
			return x + 1;
	}
	return x;
}

#endif /* defined STD_INSPIRED */
