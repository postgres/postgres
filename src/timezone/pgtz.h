/*-------------------------------------------------------------------------
 *
 * pgtz.h
 *	  Timezone Library Integration Functions
 *
 * Note: this file contains only definitions that are private to the
 * timezone library.  Public definitions are in pgtime.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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

/*
 * Support leap seconds in TZ files.
 * Maybe we should disable this?
 */
#define TZ_RUNTIME_LEAPS 1

/*
 * Limit to time zone abbreviation length in proleptic TZ strings.
 * This is distinct from TZ_MAX_CHARS, which limits TZif file contents.
 * It defaults to 254, not 255, so that desigidx_type can be an unsigned char.
 * unsigned char suffices for TZif files, so the only reason to increase
 * TZNAME_MAXIMUM is to support TZ strings specifying abbreviations
 * longer than 254 bytes.  There is little reason to do that, though,
 * as strings that long are hardly "abbreviations".
 */
#define TZNAME_MAXIMUM 254

typedef unsigned char desigidx_type;

/*
 * A type that can represent any 32-bit two's complement integer,
 * i.e., any integer in the range -2**31 .. 2**31 - 1.
 */
typedef int_fast32_t int_fast32_2s;

struct ttinfo
{								/* time type information */
	int_least32_t tt_utoff;		/* UT offset in seconds; in the range -2**31 +
								 * 1 .. 2**31 - 1  */
	desigidx_type tt_desigidx;	/* abbreviation list index */
	bool		tt_isdst;		/* used to set tm_isdst */
	bool		tt_ttisstd;		/* transition is std time */
	bool		tt_ttisut;		/* transition is UT */
};

struct lsinfo
{								/* leap second information */
	pg_time_t	ls_trans;		/* transition time (positive) */
	int_fast32_2s ls_corr;		/* correction to apply */
};

/* This abbreviation means local time is unspecified.  */
static char const UNSPEC[] = "-00";

/*
 * How many extra bytes are needed at the end of struct state's chars array.
 * This needs to be at least 1 for null termination in case the input
 * data isn't properly terminated, and it also needs to be big enough
 * for ttunspecified to work without crashing.
 */
enum
{
CHARS_EXTRA = Max(sizeof UNSPEC, 2) - 1};

/*
 * A representation of the contents of a TZif file.  Ideally this
 * would have no size limits; the following sizes should suffice for
 * practical use.  This struct should not be too large, as instances
 * are put on the stack and stacks are relatively small on some platforms.
 * See tzfile.h for more about the sizes.
 */
struct state
{
#if TZ_RUNTIME_LEAPS
	int			leapcnt;
#endif
	int			timecnt;
	int			typecnt;
	int			charcnt;
	bool		goback;
	bool		goahead;
	pg_time_t	ats[TZ_MAX_TIMES];
	unsigned char types[TZ_MAX_TIMES];
	struct ttinfo ttis[TZ_MAX_TYPES];
	char		chars[Max(Max(TZ_MAX_CHARS + CHARS_EXTRA, sizeof "UTC"),
						  2 * (TZNAME_MAXIMUM + 1))];
#if TZ_RUNTIME_LEAPS
	struct lsinfo lsis[TZ_MAX_LEAPS];
#endif
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
extern bool pg_tzload(const char *name, char *canonname, struct state *sp);

#endif							/* _PGTZ_H */
