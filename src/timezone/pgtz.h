/*-------------------------------------------------------------------------
 *
 * pgtz.h
 *	  Timezone Library Integration Functions
 *
 * Note: this file contains only definitions that are private to the
 * timezone library.  Public definitions are in pgtime.h.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/timezone/pgtz.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGTZ_H
#define _PGTZ_H

#include "pgtime.h"
#include "tzfile.h"


#define SMALLEST(a, b)	(((a) < (b)) ? (a) : (b))
#define BIGGEST(a, b)	(((a) > (b)) ? (a) : (b))

struct ttinfo
{								/* time type information */
	int32		tt_gmtoff;		/* UT offset in seconds */
	bool		tt_isdst;		/* used to set tm_isdst */
	int			tt_abbrind;		/* abbreviation list index */
	bool		tt_ttisstd;		/* transition is std time */
	bool		tt_ttisgmt;		/* transition is UT */
};

struct lsinfo
{								/* leap second information */
	pg_time_t	ls_trans;		/* transition time */
	int64		ls_corr;		/* correction to apply */
};

struct state
{
	int			leapcnt;
	int			timecnt;
	int			typecnt;
	int			charcnt;
	bool		goback;
	bool		goahead;
	pg_time_t	ats[TZ_MAX_TIMES];
	unsigned char types[TZ_MAX_TIMES];
	struct ttinfo ttis[TZ_MAX_TYPES];
	char		chars[BIGGEST(BIGGEST(TZ_MAX_CHARS + 1, 3 /* sizeof gmt */ ),
										  (2 * (TZ_STRLEN_MAX + 1)))];
	struct lsinfo lsis[TZ_MAX_LEAPS];
	int			defaulttype;	/* for early times or if no transitions */
};


struct pg_tz
{
	/* TZname contains the canonically-cased name of the timezone */
	char		TZname[TZ_STRLEN_MAX + 1];
	struct state state;
};


/* in pgtz.c */
extern int	pg_open_tzfile(const char *name, char *canonname);

/* in localtime.c */
extern int tzload(const char *name, char *canonname, struct state * sp,
	   bool doextend);
extern bool tzparse(const char *name, struct state * sp, bool lastditch);

#endif   /* _PGTZ_H */
