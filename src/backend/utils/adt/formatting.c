/* -----------------------------------------------------------------------
 * formatting.c
 *
 * src/backend/utils/adt/formatting.c
 *
 *
 *	 Portions Copyright (c) 1999-2025, PostgreSQL Global Development Group
 *
 *
 *	 TO_CHAR(); TO_TIMESTAMP(); TO_DATE(); TO_NUMBER();
 *
 *	 The PostgreSQL routines for a timestamp/int/float/numeric formatting,
 *	 inspired by the Oracle TO_CHAR() / TO_DATE() / TO_NUMBER() routines.
 *
 *
 *	 Cache & Memory:
 *	Routines use (itself) internal cache for format pictures.
 *
 *	The cache uses a static buffer and is persistent across transactions.  If
 *	the format-picture is bigger than the cache buffer, the parser is called
 *	always.
 *
 *	 NOTE for Number version:
 *	All in this version is implemented as keywords ( => not used
 *	suffixes), because a format picture is for *one* item (number)
 *	only. It not is as a timestamp version, where each keyword (can)
 *	has suffix.
 *
 *	 NOTE for Timestamp routines:
 *	In this module the POSIX 'struct tm' type is *not* used, but rather
 *	PgSQL type, which has tm_mon based on one (*non* zero) and
 *	year *not* based on 1900, but is used full year number.
 *	Module supports AD / BC / AM / PM.
 *
 *	Supported types for to_char():
 *
 *		Timestamp, Numeric, int4, int8, float4, float8
 *
 *	Supported types for reverse conversion:
 *
 *		Timestamp	- to_timestamp()
 *		Date		- to_date()
 *		Numeric		- to_number()
 *
 *
 *	Karel Zak
 *
 * TODO
 *	- better number building (formatting) / parsing, now it isn't
 *		  ideal code
 *	- use Assert()
 *	- add support for roman number to standard number conversion
 *	- add support for number spelling
 *	- add support for string to string formatting (we must be better
 *	  than Oracle :-),
 *		to_char('Hello', 'X X X X X') -> 'H e l l o'
 *
 * -----------------------------------------------------------------------
 */

#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_elog_output	DEBUG3
#endif

#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <wctype.h>

#ifdef USE_ICU
#include <unicode/ustring.h>
#endif

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "varatt.h"


/* ----------
 * Routines flags
 * ----------
 */
#define DCH_FLAG		0x1		/* DATE-TIME flag	*/
#define NUM_FLAG		0x2		/* NUMBER flag	*/
#define STD_FLAG		0x4		/* STANDARD flag	*/

/* ----------
 * KeyWord Index (ascii from position 32 (' ') to 126 (~))
 * ----------
 */
#define KeyWord_INDEX_SIZE		('~' - ' ')
#define KeyWord_INDEX_FILTER(_c)	((_c) <= ' ' || (_c) >= '~' ? 0 : 1)

/* ----------
 * Maximal length of one node
 * ----------
 */
#define DCH_MAX_ITEM_SIZ	   12	/* max localized day name		*/
#define NUM_MAX_ITEM_SIZ		8	/* roman number (RN has 15 chars)	*/


/* ----------
 * Format parser structs
 * ----------
 */
typedef struct
{
	const char *name;			/* suffix string		*/
	int			len,			/* suffix length		*/
				id,				/* used in node->suffix */
				type;			/* prefix / postfix		*/
} KeySuffix;

/* ----------
 * FromCharDateMode
 * ----------
 *
 * This value is used to nominate one of several distinct (and mutually
 * exclusive) date conventions that a keyword can belong to.
 */
typedef enum
{
	FROM_CHAR_DATE_NONE = 0,	/* Value does not affect date mode. */
	FROM_CHAR_DATE_GREGORIAN,	/* Gregorian (day, month, year) style date */
	FROM_CHAR_DATE_ISOWEEK,		/* ISO 8601 week date */
} FromCharDateMode;

typedef struct
{
	const char *name;
	int			len;
	int			id;
	bool		is_digit;
	FromCharDateMode date_mode;
} KeyWord;

typedef struct
{
	uint8		type;			/* NODE_TYPE_XXX, see below */
	char		character[MAX_MULTIBYTE_CHAR_LEN + 1];	/* if type is CHAR */
	uint8		suffix;			/* keyword prefix/suffix code, if any */
	const KeyWord *key;			/* if type is ACTION */
} FormatNode;

#define NODE_TYPE_END		1
#define NODE_TYPE_ACTION	2
#define NODE_TYPE_CHAR		3
#define NODE_TYPE_SEPARATOR	4
#define NODE_TYPE_SPACE		5

#define SUFFTYPE_PREFIX		1
#define SUFFTYPE_POSTFIX	2

#define CLOCK_24_HOUR		0
#define CLOCK_12_HOUR		1


/* ----------
 * Full months
 * ----------
 */
static const char *const months_full[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December", NULL
};

static const char *const days_short[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", NULL
};

/* ----------
 * AD / BC
 * ----------
 *	There is no 0 AD.  Years go from 1 BC to 1 AD, so we make it
 *	positive and map year == -1 to year zero, and shift all negative
 *	years up one.  For interval years, we just return the year.
 */
#define ADJUST_YEAR(year, is_interval)	((is_interval) ? (year) : ((year) <= 0 ? -((year) - 1) : (year)))

#define A_D_STR		"A.D."
#define a_d_STR		"a.d."
#define AD_STR		"AD"
#define ad_STR		"ad"

#define B_C_STR		"B.C."
#define b_c_STR		"b.c."
#define BC_STR		"BC"
#define bc_STR		"bc"

/*
 * AD / BC strings for seq_search.
 *
 * These are given in two variants, a long form with periods and a standard
 * form without.
 *
 * The array is laid out such that matches for AD have an even index, and
 * matches for BC have an odd index.  So the boolean value for BC is given by
 * taking the array index of the match, modulo 2.
 */
static const char *const adbc_strings[] = {ad_STR, bc_STR, AD_STR, BC_STR, NULL};
static const char *const adbc_strings_long[] = {a_d_STR, b_c_STR, A_D_STR, B_C_STR, NULL};

/* ----------
 * AM / PM
 * ----------
 */
#define A_M_STR		"A.M."
#define a_m_STR		"a.m."
#define AM_STR		"AM"
#define am_STR		"am"

#define P_M_STR		"P.M."
#define p_m_STR		"p.m."
#define PM_STR		"PM"
#define pm_STR		"pm"

/*
 * AM / PM strings for seq_search.
 *
 * These are given in two variants, a long form with periods and a standard
 * form without.
 *
 * The array is laid out such that matches for AM have an even index, and
 * matches for PM have an odd index.  So the boolean value for PM is given by
 * taking the array index of the match, modulo 2.
 */
static const char *const ampm_strings[] = {am_STR, pm_STR, AM_STR, PM_STR, NULL};
static const char *const ampm_strings_long[] = {a_m_STR, p_m_STR, A_M_STR, P_M_STR, NULL};

/* ----------
 * Months in roman-numeral
 * (Must be in reverse order for seq_search (in FROM_CHAR), because
 *	'VIII' must have higher precedence than 'V')
 * ----------
 */
static const char *const rm_months_upper[] =
{"XII", "XI", "X", "IX", "VIII", "VII", "VI", "V", "IV", "III", "II", "I", NULL};

static const char *const rm_months_lower[] =
{"xii", "xi", "x", "ix", "viii", "vii", "vi", "v", "iv", "iii", "ii", "i", NULL};

/* ----------
 * Roman numbers
 * ----------
 */
static const char *const rm1[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", NULL};
static const char *const rm10[] = {"X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC", NULL};
static const char *const rm100[] = {"C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM", NULL};

/* ----------
 * Ordinal postfixes
 * ----------
 */
static const char *const numTH[] = {"ST", "ND", "RD", "TH", NULL};
static const char *const numth[] = {"st", "nd", "rd", "th", NULL};

/* ----------
 * Flags & Options:
 * ----------
 */
#define TH_UPPER		1
#define TH_LOWER		2

/* ----------
 * Number description struct
 * ----------
 */
typedef struct
{
	int			pre,			/* (count) numbers before decimal */
				post,			/* (count) numbers after decimal  */
				lsign,			/* want locales sign		  */
				flag,			/* number parameters		  */
				pre_lsign_num,	/* tmp value for lsign		  */
				multi,			/* multiplier for 'V'		  */
				zero_start,		/* position of first zero	  */
				zero_end,		/* position of last zero	  */
				need_locale;	/* needs it locale		  */
} NUMDesc;

/* ----------
 * Flags for NUMBER version
 * ----------
 */
#define NUM_F_DECIMAL		(1 << 1)
#define NUM_F_LDECIMAL		(1 << 2)
#define NUM_F_ZERO			(1 << 3)
#define NUM_F_BLANK			(1 << 4)
#define NUM_F_FILLMODE		(1 << 5)
#define NUM_F_LSIGN			(1 << 6)
#define NUM_F_BRACKET		(1 << 7)
#define NUM_F_MINUS			(1 << 8)
#define NUM_F_PLUS			(1 << 9)
#define NUM_F_ROMAN			(1 << 10)
#define NUM_F_MULTI			(1 << 11)
#define NUM_F_PLUS_POST		(1 << 12)
#define NUM_F_MINUS_POST	(1 << 13)
#define NUM_F_EEEE			(1 << 14)

#define NUM_LSIGN_PRE	(-1)
#define NUM_LSIGN_POST	1
#define NUM_LSIGN_NONE	0

/* ----------
 * Tests
 * ----------
 */
#define IS_DECIMAL(_f)	((_f)->flag & NUM_F_DECIMAL)
#define IS_LDECIMAL(_f) ((_f)->flag & NUM_F_LDECIMAL)
#define IS_ZERO(_f) ((_f)->flag & NUM_F_ZERO)
#define IS_BLANK(_f)	((_f)->flag & NUM_F_BLANK)
#define IS_FILLMODE(_f) ((_f)->flag & NUM_F_FILLMODE)
#define IS_BRACKET(_f)	((_f)->flag & NUM_F_BRACKET)
#define IS_MINUS(_f)	((_f)->flag & NUM_F_MINUS)
#define IS_LSIGN(_f)	((_f)->flag & NUM_F_LSIGN)
#define IS_PLUS(_f) ((_f)->flag & NUM_F_PLUS)
#define IS_ROMAN(_f)	((_f)->flag & NUM_F_ROMAN)
#define IS_MULTI(_f)	((_f)->flag & NUM_F_MULTI)
#define IS_EEEE(_f)		((_f)->flag & NUM_F_EEEE)

/* ----------
 * Format picture cache
 *
 * We will cache datetime format pictures up to DCH_CACHE_SIZE bytes long;
 * likewise number format pictures up to NUM_CACHE_SIZE bytes long.
 *
 * For simplicity, the cache entries are fixed-size, so they allow for the
 * worst case of a FormatNode for each byte in the picture string.
 *
 * The CACHE_SIZE constants are computed to make sizeof(DCHCacheEntry) and
 * sizeof(NUMCacheEntry) be powers of 2, or just less than that, so that
 * we don't waste too much space by palloc'ing them individually.  Be sure
 * to adjust those macros if you add fields to those structs.
 *
 * The max number of entries in each cache is DCH_CACHE_ENTRIES
 * resp. NUM_CACHE_ENTRIES.
 * ----------
 */
#define DCH_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int))
#define NUM_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int) + sizeof(NUMDesc))

#define DCH_CACHE_SIZE \
	((2048 - DCH_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)
#define NUM_CACHE_SIZE \
	((1024 - NUM_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)

#define DCH_CACHE_ENTRIES	20
#define NUM_CACHE_ENTRIES	20

typedef struct
{
	FormatNode	format[DCH_CACHE_SIZE + 1];
	char		str[DCH_CACHE_SIZE + 1];
	bool		std;
	bool		valid;
	int			age;
} DCHCacheEntry;

typedef struct
{
	FormatNode	format[NUM_CACHE_SIZE + 1];
	char		str[NUM_CACHE_SIZE + 1];
	bool		valid;
	int			age;
	NUMDesc		Num;
} NUMCacheEntry;

/* global cache for date/time format pictures */
static DCHCacheEntry *DCHCache[DCH_CACHE_ENTRIES];
static int	n_DCHCache = 0;		/* current number of entries */
static int	DCHCounter = 0;		/* aging-event counter */

/* global cache for number format pictures */
static NUMCacheEntry *NUMCache[NUM_CACHE_ENTRIES];
static int	n_NUMCache = 0;		/* current number of entries */
static int	NUMCounter = 0;		/* aging-event counter */

/* ----------
 * For char->date/time conversion
 * ----------
 */
typedef struct
{
	FromCharDateMode mode;
	int			hh,
				pm,
				mi,
				ss,
				ssss,
				d,				/* stored as 1-7, Sunday = 1, 0 means missing */
				dd,
				ddd,
				mm,
				ms,
				year,
				bc,
				ww,
				w,
				cc,
				j,
				us,
				yysz,			/* is it YY or YYYY ? */
				clock,			/* 12 or 24 hour clock? */
				tzsign,			/* +1, -1, or 0 if no TZH/TZM fields */
				tzh,
				tzm,
				ff;				/* fractional precision */
	bool		has_tz;			/* was there a TZ field? */
	int			gmtoffset;		/* GMT offset of fixed-offset zone abbrev */
	pg_tz	   *tzp;			/* pg_tz for dynamic abbrev */
	char	   *abbrev;			/* dynamic abbrev */
} TmFromChar;

#define ZERO_tmfc(_X) memset(_X, 0, sizeof(TmFromChar))

struct fmt_tz					/* do_to_timestamp's timezone info output */
{
	bool		has_tz;			/* was there any TZ/TZH/TZM field? */
	int			gmtoffset;		/* GMT offset in seconds */
};

/* ----------
 * Debug
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_TMFC(_X) \
		elog(DEBUG_elog_output, "TMFC:\nmode %d\nhh %d\npm %d\nmi %d\nss %d\nssss %d\nd %d\ndd %d\nddd %d\nmm %d\nms: %d\nyear %d\nbc %d\nww %d\nw %d\ncc %d\nj %d\nus: %d\nyysz: %d\nclock: %d", \
			(_X)->mode, (_X)->hh, (_X)->pm, (_X)->mi, (_X)->ss, (_X)->ssss, \
			(_X)->d, (_X)->dd, (_X)->ddd, (_X)->mm, (_X)->ms, (_X)->year, \
			(_X)->bc, (_X)->ww, (_X)->w, (_X)->cc, (_X)->j, (_X)->us, \
			(_X)->yysz, (_X)->clock)
#define DEBUG_TM(_X) \
		elog(DEBUG_elog_output, "TM:\nsec %d\nyear %d\nmin %d\nwday %d\nhour %d\nyday %d\nmday %d\nnisdst %d\nmon %d\n",\
			(_X)->tm_sec, (_X)->tm_year,\
			(_X)->tm_min, (_X)->tm_wday, (_X)->tm_hour, (_X)->tm_yday,\
			(_X)->tm_mday, (_X)->tm_isdst, (_X)->tm_mon)
#else
#define DEBUG_TMFC(_X)
#define DEBUG_TM(_X)
#endif

/* ----------
 * Datetime to char conversion
 *
 * To support intervals as well as timestamps, we use a custom "tm" struct
 * that is almost like struct pg_tm, but has a 64-bit tm_hour field.
 * We omit the tm_isdst and tm_zone fields, which are not used here.
 * ----------
 */
struct fmt_tm
{
	int			tm_sec;
	int			tm_min;
	int64		tm_hour;
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
	int			tm_wday;
	int			tm_yday;
	long int	tm_gmtoff;
};

typedef struct TmToChar
{
	struct fmt_tm tm;			/* almost the classic 'tm' struct */
	fsec_t		fsec;			/* fractional seconds */
	const char *tzn;			/* timezone */
} TmToChar;

#define tmtcTm(_X)	(&(_X)->tm)
#define tmtcTzn(_X) ((_X)->tzn)
#define tmtcFsec(_X)	((_X)->fsec)

/* Note: this is used to copy pg_tm to fmt_tm, so not quite a bitwise copy */
#define COPY_tm(_DST, _SRC) \
do {	\
	(_DST)->tm_sec = (_SRC)->tm_sec; \
	(_DST)->tm_min = (_SRC)->tm_min; \
	(_DST)->tm_hour = (_SRC)->tm_hour; \
	(_DST)->tm_mday = (_SRC)->tm_mday; \
	(_DST)->tm_mon = (_SRC)->tm_mon; \
	(_DST)->tm_year = (_SRC)->tm_year; \
	(_DST)->tm_wday = (_SRC)->tm_wday; \
	(_DST)->tm_yday = (_SRC)->tm_yday; \
	(_DST)->tm_gmtoff = (_SRC)->tm_gmtoff; \
} while(0)

/* Caution: this is used to zero both pg_tm and fmt_tm structs */
#define ZERO_tm(_X) \
do {	\
	memset(_X, 0, sizeof(*(_X))); \
	(_X)->tm_mday = (_X)->tm_mon = 1; \
} while(0)

#define ZERO_tmtc(_X) \
do { \
	ZERO_tm( tmtcTm(_X) ); \
	tmtcFsec(_X) = 0; \
	tmtcTzn(_X) = NULL; \
} while(0)

/*
 *	to_char(time) appears to to_char() as an interval, so this check
 *	is really for interval and time data types.
 */
#define INVALID_FOR_INTERVAL  \
do { \
	if (is_interval) \
		ereport(ERROR, \
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT), \
				 errmsg("invalid format specification for an interval value"), \
				 errhint("Intervals are not tied to specific calendar dates."))); \
} while(0)

/*****************************************************************************
 *			KeyWord definitions
 *****************************************************************************/

/* ----------
 * Suffixes (FormatNode.suffix is an OR of these codes)
 * ----------
 */
#define DCH_S_FM	0x01
#define DCH_S_TH	0x02
#define DCH_S_th	0x04
#define DCH_S_SP	0x08
#define DCH_S_TM	0x10

/* ----------
 * Suffix tests
 * ----------
 */
#define S_THth(_s)	((((_s) & DCH_S_TH) || ((_s) & DCH_S_th)) ? 1 : 0)
#define S_TH(_s)	(((_s) & DCH_S_TH) ? 1 : 0)
#define S_th(_s)	(((_s) & DCH_S_th) ? 1 : 0)
#define S_TH_TYPE(_s)	(((_s) & DCH_S_TH) ? TH_UPPER : TH_LOWER)

/* Oracle toggles FM behavior, we don't; see docs. */
#define S_FM(_s)	(((_s) & DCH_S_FM) ? 1 : 0)
#define S_SP(_s)	(((_s) & DCH_S_SP) ? 1 : 0)
#define S_TM(_s)	(((_s) & DCH_S_TM) ? 1 : 0)

/* ----------
 * Suffixes definition for DATE-TIME TO/FROM CHAR
 * ----------
 */
#define TM_SUFFIX_LEN	2

static const KeySuffix DCH_suff[] = {
	{"FM", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"fm", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"TM", TM_SUFFIX_LEN, DCH_S_TM, SUFFTYPE_PREFIX},
	{"tm", 2, DCH_S_TM, SUFFTYPE_PREFIX},
	{"TH", 2, DCH_S_TH, SUFFTYPE_POSTFIX},
	{"th", 2, DCH_S_th, SUFFTYPE_POSTFIX},
	{"SP", 2, DCH_S_SP, SUFFTYPE_POSTFIX},
	/* last */
	{NULL, 0, 0, 0}
};


/* ----------
 * Format-pictures (KeyWord).
 *
 * The KeyWord field; alphabetic sorted, *BUT* strings alike is sorted
 *		  complicated -to-> easy:
 *
 *	(example: "DDD","DD","Day","D" )
 *
 * (this specific sort needs the algorithm for sequential search for strings,
 * which not has exact end; -> How keyword is in "HH12blabla" ? - "HH"
 * or "HH12"? You must first try "HH12", because "HH" is in string, but
 * it is not good.
 *
 * (!)
 *	 - Position for the keyword is similar as position in the enum DCH/NUM_poz.
 * (!)
 *
 * For fast search is used the 'int index[]', index is ascii table from position
 * 32 (' ') to 126 (~), in this index is DCH_ / NUM_ enums for each ASCII
 * position or -1 if char is not used in the KeyWord. Search example for
 * string "MM":
 *	1)	see in index to index['M' - 32],
 *	2)	take keywords position (enum DCH_MI) from index
 *	3)	run sequential search in keywords[] from this position
 *
 * ----------
 */

typedef enum
{
	DCH_A_D,
	DCH_A_M,
	DCH_AD,
	DCH_AM,
	DCH_B_C,
	DCH_BC,
	DCH_CC,
	DCH_DAY,
	DCH_DDD,
	DCH_DD,
	DCH_DY,
	DCH_Day,
	DCH_Dy,
	DCH_D,
	DCH_FF1,					/* FFn codes must be consecutive */
	DCH_FF2,
	DCH_FF3,
	DCH_FF4,
	DCH_FF5,
	DCH_FF6,
	DCH_FX,						/* global suffix */
	DCH_HH24,
	DCH_HH12,
	DCH_HH,
	DCH_IDDD,
	DCH_ID,
	DCH_IW,
	DCH_IYYY,
	DCH_IYY,
	DCH_IY,
	DCH_I,
	DCH_J,
	DCH_MI,
	DCH_MM,
	DCH_MONTH,
	DCH_MON,
	DCH_MS,
	DCH_Month,
	DCH_Mon,
	DCH_OF,
	DCH_P_M,
	DCH_PM,
	DCH_Q,
	DCH_RM,
	DCH_SSSSS,
	DCH_SSSS,
	DCH_SS,
	DCH_TZH,
	DCH_TZM,
	DCH_TZ,
	DCH_US,
	DCH_WW,
	DCH_W,
	DCH_Y_YYY,
	DCH_YYYY,
	DCH_YYY,
	DCH_YY,
	DCH_Y,
	DCH_a_d,
	DCH_a_m,
	DCH_ad,
	DCH_am,
	DCH_b_c,
	DCH_bc,
	DCH_cc,
	DCH_day,
	DCH_ddd,
	DCH_dd,
	DCH_dy,
	DCH_d,
	DCH_ff1,
	DCH_ff2,
	DCH_ff3,
	DCH_ff4,
	DCH_ff5,
	DCH_ff6,
	DCH_fx,
	DCH_hh24,
	DCH_hh12,
	DCH_hh,
	DCH_iddd,
	DCH_id,
	DCH_iw,
	DCH_iyyy,
	DCH_iyy,
	DCH_iy,
	DCH_i,
	DCH_j,
	DCH_mi,
	DCH_mm,
	DCH_month,
	DCH_mon,
	DCH_ms,
	DCH_of,
	DCH_p_m,
	DCH_pm,
	DCH_q,
	DCH_rm,
	DCH_sssss,
	DCH_ssss,
	DCH_ss,
	DCH_tzh,
	DCH_tzm,
	DCH_tz,
	DCH_us,
	DCH_ww,
	DCH_w,
	DCH_y_yyy,
	DCH_yyyy,
	DCH_yyy,
	DCH_yy,
	DCH_y,

	/* last */
	_DCH_last_
}			DCH_poz;

typedef enum
{
	NUM_COMMA,
	NUM_DEC,
	NUM_0,
	NUM_9,
	NUM_B,
	NUM_C,
	NUM_D,
	NUM_E,
	NUM_FM,
	NUM_G,
	NUM_L,
	NUM_MI,
	NUM_PL,
	NUM_PR,
	NUM_RN,
	NUM_SG,
	NUM_SP,
	NUM_S,
	NUM_TH,
	NUM_V,
	NUM_b,
	NUM_c,
	NUM_d,
	NUM_e,
	NUM_fm,
	NUM_g,
	NUM_l,
	NUM_mi,
	NUM_pl,
	NUM_pr,
	NUM_rn,
	NUM_sg,
	NUM_sp,
	NUM_s,
	NUM_th,
	NUM_v,

	/* last */
	_NUM_last_
}			NUM_poz;

/* ----------
 * KeyWords for DATE-TIME version
 * ----------
 */
static const KeyWord DCH_keywords[] = {
/*	name, len, id, is_digit, date_mode */
	{"A.D.", 4, DCH_A_D, false, FROM_CHAR_DATE_NONE},	/* A */
	{"A.M.", 4, DCH_A_M, false, FROM_CHAR_DATE_NONE},
	{"AD", 2, DCH_AD, false, FROM_CHAR_DATE_NONE},
	{"AM", 2, DCH_AM, false, FROM_CHAR_DATE_NONE},
	{"B.C.", 4, DCH_B_C, false, FROM_CHAR_DATE_NONE},	/* B */
	{"BC", 2, DCH_BC, false, FROM_CHAR_DATE_NONE},
	{"CC", 2, DCH_CC, true, FROM_CHAR_DATE_NONE},	/* C */
	{"DAY", 3, DCH_DAY, false, FROM_CHAR_DATE_NONE},	/* D */
	{"DDD", 3, DCH_DDD, true, FROM_CHAR_DATE_GREGORIAN},
	{"DD", 2, DCH_DD, true, FROM_CHAR_DATE_GREGORIAN},
	{"DY", 2, DCH_DY, false, FROM_CHAR_DATE_NONE},
	{"Day", 3, DCH_Day, false, FROM_CHAR_DATE_NONE},
	{"Dy", 2, DCH_Dy, false, FROM_CHAR_DATE_NONE},
	{"D", 1, DCH_D, true, FROM_CHAR_DATE_GREGORIAN},
	{"FF1", 3, DCH_FF1, true, FROM_CHAR_DATE_NONE}, /* F */
	{"FF2", 3, DCH_FF2, true, FROM_CHAR_DATE_NONE},
	{"FF3", 3, DCH_FF3, true, FROM_CHAR_DATE_NONE},
	{"FF4", 3, DCH_FF4, true, FROM_CHAR_DATE_NONE},
	{"FF5", 3, DCH_FF5, true, FROM_CHAR_DATE_NONE},
	{"FF6", 3, DCH_FF6, true, FROM_CHAR_DATE_NONE},
	{"FX", 2, DCH_FX, false, FROM_CHAR_DATE_NONE},
	{"HH24", 4, DCH_HH24, true, FROM_CHAR_DATE_NONE},	/* H */
	{"HH12", 4, DCH_HH12, true, FROM_CHAR_DATE_NONE},
	{"HH", 2, DCH_HH, true, FROM_CHAR_DATE_NONE},
	{"IDDD", 4, DCH_IDDD, true, FROM_CHAR_DATE_ISOWEEK},	/* I */
	{"ID", 2, DCH_ID, true, FROM_CHAR_DATE_ISOWEEK},
	{"IW", 2, DCH_IW, true, FROM_CHAR_DATE_ISOWEEK},
	{"IYYY", 4, DCH_IYYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"IYY", 3, DCH_IYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"IY", 2, DCH_IY, true, FROM_CHAR_DATE_ISOWEEK},
	{"I", 1, DCH_I, true, FROM_CHAR_DATE_ISOWEEK},
	{"J", 1, DCH_J, true, FROM_CHAR_DATE_NONE}, /* J */
	{"MI", 2, DCH_MI, true, FROM_CHAR_DATE_NONE},	/* M */
	{"MM", 2, DCH_MM, true, FROM_CHAR_DATE_GREGORIAN},
	{"MONTH", 5, DCH_MONTH, false, FROM_CHAR_DATE_GREGORIAN},
	{"MON", 3, DCH_MON, false, FROM_CHAR_DATE_GREGORIAN},
	{"MS", 2, DCH_MS, true, FROM_CHAR_DATE_NONE},
	{"Month", 5, DCH_Month, false, FROM_CHAR_DATE_GREGORIAN},
	{"Mon", 3, DCH_Mon, false, FROM_CHAR_DATE_GREGORIAN},
	{"OF", 2, DCH_OF, false, FROM_CHAR_DATE_NONE},	/* O */
	{"P.M.", 4, DCH_P_M, false, FROM_CHAR_DATE_NONE},	/* P */
	{"PM", 2, DCH_PM, false, FROM_CHAR_DATE_NONE},
	{"Q", 1, DCH_Q, true, FROM_CHAR_DATE_NONE}, /* Q */
	{"RM", 2, DCH_RM, false, FROM_CHAR_DATE_GREGORIAN}, /* R */
	{"SSSSS", 5, DCH_SSSS, true, FROM_CHAR_DATE_NONE},	/* S */
	{"SSSS", 4, DCH_SSSS, true, FROM_CHAR_DATE_NONE},
	{"SS", 2, DCH_SS, true, FROM_CHAR_DATE_NONE},
	{"TZH", 3, DCH_TZH, false, FROM_CHAR_DATE_NONE},	/* T */
	{"TZM", 3, DCH_TZM, true, FROM_CHAR_DATE_NONE},
	{"TZ", 2, DCH_TZ, false, FROM_CHAR_DATE_NONE},
	{"US", 2, DCH_US, true, FROM_CHAR_DATE_NONE},	/* U */
	{"WW", 2, DCH_WW, true, FROM_CHAR_DATE_GREGORIAN},	/* W */
	{"W", 1, DCH_W, true, FROM_CHAR_DATE_GREGORIAN},
	{"Y,YYY", 5, DCH_Y_YYY, true, FROM_CHAR_DATE_GREGORIAN},	/* Y */
	{"YYYY", 4, DCH_YYYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"YYY", 3, DCH_YYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"YY", 2, DCH_YY, true, FROM_CHAR_DATE_GREGORIAN},
	{"Y", 1, DCH_Y, true, FROM_CHAR_DATE_GREGORIAN},
	{"a.d.", 4, DCH_a_d, false, FROM_CHAR_DATE_NONE},	/* a */
	{"a.m.", 4, DCH_a_m, false, FROM_CHAR_DATE_NONE},
	{"ad", 2, DCH_ad, false, FROM_CHAR_DATE_NONE},
	{"am", 2, DCH_am, false, FROM_CHAR_DATE_NONE},
	{"b.c.", 4, DCH_b_c, false, FROM_CHAR_DATE_NONE},	/* b */
	{"bc", 2, DCH_bc, false, FROM_CHAR_DATE_NONE},
	{"cc", 2, DCH_CC, true, FROM_CHAR_DATE_NONE},	/* c */
	{"day", 3, DCH_day, false, FROM_CHAR_DATE_NONE},	/* d */
	{"ddd", 3, DCH_DDD, true, FROM_CHAR_DATE_GREGORIAN},
	{"dd", 2, DCH_DD, true, FROM_CHAR_DATE_GREGORIAN},
	{"dy", 2, DCH_dy, false, FROM_CHAR_DATE_NONE},
	{"d", 1, DCH_D, true, FROM_CHAR_DATE_GREGORIAN},
	{"ff1", 3, DCH_FF1, true, FROM_CHAR_DATE_NONE}, /* f */
	{"ff2", 3, DCH_FF2, true, FROM_CHAR_DATE_NONE},
	{"ff3", 3, DCH_FF3, true, FROM_CHAR_DATE_NONE},
	{"ff4", 3, DCH_FF4, true, FROM_CHAR_DATE_NONE},
	{"ff5", 3, DCH_FF5, true, FROM_CHAR_DATE_NONE},
	{"ff6", 3, DCH_FF6, true, FROM_CHAR_DATE_NONE},
	{"fx", 2, DCH_FX, false, FROM_CHAR_DATE_NONE},
	{"hh24", 4, DCH_HH24, true, FROM_CHAR_DATE_NONE},	/* h */
	{"hh12", 4, DCH_HH12, true, FROM_CHAR_DATE_NONE},
	{"hh", 2, DCH_HH, true, FROM_CHAR_DATE_NONE},
	{"iddd", 4, DCH_IDDD, true, FROM_CHAR_DATE_ISOWEEK},	/* i */
	{"id", 2, DCH_ID, true, FROM_CHAR_DATE_ISOWEEK},
	{"iw", 2, DCH_IW, true, FROM_CHAR_DATE_ISOWEEK},
	{"iyyy", 4, DCH_IYYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"iyy", 3, DCH_IYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"iy", 2, DCH_IY, true, FROM_CHAR_DATE_ISOWEEK},
	{"i", 1, DCH_I, true, FROM_CHAR_DATE_ISOWEEK},
	{"j", 1, DCH_J, true, FROM_CHAR_DATE_NONE}, /* j */
	{"mi", 2, DCH_MI, true, FROM_CHAR_DATE_NONE},	/* m */
	{"mm", 2, DCH_MM, true, FROM_CHAR_DATE_GREGORIAN},
	{"month", 5, DCH_month, false, FROM_CHAR_DATE_GREGORIAN},
	{"mon", 3, DCH_mon, false, FROM_CHAR_DATE_GREGORIAN},
	{"ms", 2, DCH_MS, true, FROM_CHAR_DATE_NONE},
	{"of", 2, DCH_OF, false, FROM_CHAR_DATE_NONE},	/* o */
	{"p.m.", 4, DCH_p_m, false, FROM_CHAR_DATE_NONE},	/* p */
	{"pm", 2, DCH_pm, false, FROM_CHAR_DATE_NONE},
	{"q", 1, DCH_Q, true, FROM_CHAR_DATE_NONE}, /* q */
	{"rm", 2, DCH_rm, false, FROM_CHAR_DATE_GREGORIAN}, /* r */
	{"sssss", 5, DCH_SSSS, true, FROM_CHAR_DATE_NONE},	/* s */
	{"ssss", 4, DCH_SSSS, true, FROM_CHAR_DATE_NONE},
	{"ss", 2, DCH_SS, true, FROM_CHAR_DATE_NONE},
	{"tzh", 3, DCH_TZH, false, FROM_CHAR_DATE_NONE},	/* t */
	{"tzm", 3, DCH_TZM, true, FROM_CHAR_DATE_NONE},
	{"tz", 2, DCH_tz, false, FROM_CHAR_DATE_NONE},
	{"us", 2, DCH_US, true, FROM_CHAR_DATE_NONE},	/* u */
	{"ww", 2, DCH_WW, true, FROM_CHAR_DATE_GREGORIAN},	/* w */
	{"w", 1, DCH_W, true, FROM_CHAR_DATE_GREGORIAN},
	{"y,yyy", 5, DCH_Y_YYY, true, FROM_CHAR_DATE_GREGORIAN},	/* y */
	{"yyyy", 4, DCH_YYYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"yyy", 3, DCH_YYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"yy", 2, DCH_YY, true, FROM_CHAR_DATE_GREGORIAN},
	{"y", 1, DCH_Y, true, FROM_CHAR_DATE_GREGORIAN},

	/* last */
	{NULL, 0, 0, 0, 0}
};

/* ----------
 * KeyWords for NUMBER version
 *
 * The is_digit and date_mode fields are not relevant here.
 * ----------
 */
static const KeyWord NUM_keywords[] = {
/*	name, len, id			is in Index */
	{",", 1, NUM_COMMA},		/* , */
	{".", 1, NUM_DEC},			/* . */
	{"0", 1, NUM_0},			/* 0 */
	{"9", 1, NUM_9},			/* 9 */
	{"B", 1, NUM_B},			/* B */
	{"C", 1, NUM_C},			/* C */
	{"D", 1, NUM_D},			/* D */
	{"EEEE", 4, NUM_E},			/* E */
	{"FM", 2, NUM_FM},			/* F */
	{"G", 1, NUM_G},			/* G */
	{"L", 1, NUM_L},			/* L */
	{"MI", 2, NUM_MI},			/* M */
	{"PL", 2, NUM_PL},			/* P */
	{"PR", 2, NUM_PR},
	{"RN", 2, NUM_RN},			/* R */
	{"SG", 2, NUM_SG},			/* S */
	{"SP", 2, NUM_SP},
	{"S", 1, NUM_S},
	{"TH", 2, NUM_TH},			/* T */
	{"V", 1, NUM_V},			/* V */
	{"b", 1, NUM_B},			/* b */
	{"c", 1, NUM_C},			/* c */
	{"d", 1, NUM_D},			/* d */
	{"eeee", 4, NUM_E},			/* e */
	{"fm", 2, NUM_FM},			/* f */
	{"g", 1, NUM_G},			/* g */
	{"l", 1, NUM_L},			/* l */
	{"mi", 2, NUM_MI},			/* m */
	{"pl", 2, NUM_PL},			/* p */
	{"pr", 2, NUM_PR},
	{"rn", 2, NUM_rn},			/* r */
	{"sg", 2, NUM_SG},			/* s */
	{"sp", 2, NUM_SP},
	{"s", 1, NUM_S},
	{"th", 2, NUM_th},			/* t */
	{"v", 1, NUM_V},			/* v */

	/* last */
	{NULL, 0, 0}
};


/* ----------
 * KeyWords index for DATE-TIME version
 * ----------
 */
static const int DCH_index[KeyWord_INDEX_SIZE] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
	/*---- first 0..31 chars are skipped ----*/

	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, DCH_A_D, DCH_B_C, DCH_CC, DCH_DAY, -1,
	DCH_FF1, -1, DCH_HH24, DCH_IDDD, DCH_J, -1, -1, DCH_MI, -1, DCH_OF,
	DCH_P_M, DCH_Q, DCH_RM, DCH_SSSSS, DCH_TZH, DCH_US, -1, DCH_WW, -1, DCH_Y_YYY,
	-1, -1, -1, -1, -1, -1, -1, DCH_a_d, DCH_b_c, DCH_cc,
	DCH_day, -1, DCH_ff1, -1, DCH_hh24, DCH_iddd, DCH_j, -1, -1, DCH_mi,
	-1, DCH_of, DCH_p_m, DCH_q, DCH_rm, DCH_sssss, DCH_tzh, DCH_us, -1, DCH_ww,
	-1, DCH_y_yyy, -1, -1, -1, -1

	/*---- chars over 126 are skipped ----*/
};

/* ----------
 * KeyWords index for NUMBER version
 * ----------
 */
static const int NUM_index[KeyWord_INDEX_SIZE] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
	/*---- first 0..31 chars are skipped ----*/

	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, NUM_COMMA, -1, NUM_DEC, -1, NUM_0, -1,
	-1, -1, -1, -1, -1, -1, -1, NUM_9, -1, -1,
	-1, -1, -1, -1, -1, -1, NUM_B, NUM_C, NUM_D, NUM_E,
	NUM_FM, NUM_G, -1, -1, -1, -1, NUM_L, NUM_MI, -1, -1,
	NUM_PL, -1, NUM_RN, NUM_SG, NUM_TH, -1, NUM_V, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, NUM_b, NUM_c,
	NUM_d, NUM_e, NUM_fm, NUM_g, -1, -1, -1, -1, NUM_l, NUM_mi,
	-1, -1, NUM_pl, -1, NUM_rn, NUM_sg, NUM_th, -1, NUM_v, -1,
	-1, -1, -1, -1, -1, -1

	/*---- chars over 126 are skipped ----*/
};

/* ----------
 * Number processor struct
 * ----------
 */
typedef struct NUMProc
{
	bool		is_to_char;
	NUMDesc    *Num;			/* number description		*/

	int			sign,			/* '-' or '+'			*/
				sign_wrote,		/* was sign write		*/
				num_count,		/* number of write digits	*/
				num_in,			/* is inside number		*/
				num_curr,		/* current position in number	*/
				out_pre_spaces, /* spaces before first digit	*/

				read_dec,		/* to_number - was read dec. point	*/
				read_post,		/* to_number - number of dec. digit */
				read_pre;		/* to_number - number non-dec. digit */

	char	   *number,			/* string with number	*/
			   *number_p,		/* pointer to current number position */
			   *inout,			/* in / out buffer	*/
			   *inout_p,		/* pointer to current inout position */
			   *last_relevant,	/* last relevant number after decimal point */

			   *L_negative_sign,	/* Locale */
			   *L_positive_sign,
			   *decimal,
			   *L_thousands_sep,
			   *L_currency_symbol;
} NUMProc;

/* Return flags for DCH_from_char() */
#define DCH_DATED	0x01
#define DCH_TIMED	0x02
#define DCH_ZONED	0x04

/* ----------
 * Functions
 * ----------
 */
static const KeyWord *index_seq_search(const char *str, const KeyWord *kw,
									   const int *index);
static const KeySuffix *suff_search(const char *str, const KeySuffix *suf, int type);
static bool is_separator_char(const char *str);
static void NUMDesc_prepare(NUMDesc *num, FormatNode *n);
static void parse_format(FormatNode *node, const char *str, const KeyWord *kw,
						 const KeySuffix *suf, const int *index, uint32 flags, NUMDesc *Num);

static void DCH_to_char(FormatNode *node, bool is_interval,
						TmToChar *in, char *out, Oid collid);
static void DCH_from_char(FormatNode *node, const char *in, TmFromChar *out,
						  Oid collid, bool std, Node *escontext);

#ifdef DEBUG_TO_FROM_CHAR
static void dump_index(const KeyWord *k, const int *index);
static void dump_node(FormatNode *node, int max);
#endif

static const char *get_th(char *num, int type);
static char *str_numth(char *dest, char *num, int type);
static int	adjust_partial_year_to_2020(int year);
static int	strspace_len(const char *str);
static bool from_char_set_mode(TmFromChar *tmfc, const FromCharDateMode mode,
							   Node *escontext);
static bool from_char_set_int(int *dest, const int value, const FormatNode *node,
							  Node *escontext);
static int	from_char_parse_int_len(int *dest, const char **src, const int len,
									FormatNode *node, Node *escontext);
static int	from_char_parse_int(int *dest, const char **src, FormatNode *node,
								Node *escontext);
static int	seq_search_ascii(const char *name, const char *const *array, int *len);
static int	seq_search_localized(const char *name, char **array, int *len,
								 Oid collid);
static bool from_char_seq_search(int *dest, const char **src,
								 const char *const *array,
								 char **localized_array, Oid collid,
								 FormatNode *node, Node *escontext);
static bool do_to_timestamp(text *date_txt, text *fmt, Oid collid, bool std,
							struct pg_tm *tm, fsec_t *fsec, struct fmt_tz *tz,
							int *fprec, uint32 *flags, Node *escontext);
static char *fill_str(char *str, int c, int max);
static FormatNode *NUM_cache(int len, NUMDesc *Num, text *pars_str, bool *shouldFree);
static char *int_to_roman(int number);
static void NUM_prepare_locale(NUMProc *Np);
static char *get_last_relevant_decnum(char *num);
static void NUM_numpart_from_char(NUMProc *Np, int id, int input_len);
static void NUM_numpart_to_char(NUMProc *Np, int id);
static char *NUM_processor(FormatNode *node, NUMDesc *Num, char *inout,
						   char *number, int input_len, int to_char_out_pre_spaces,
						   int sign, bool is_to_char, Oid collid);
static DCHCacheEntry *DCH_cache_getnew(const char *str, bool std);
static DCHCacheEntry *DCH_cache_search(const char *str, bool std);
static DCHCacheEntry *DCH_cache_fetch(const char *str, bool std);
static NUMCacheEntry *NUM_cache_getnew(const char *str);
static NUMCacheEntry *NUM_cache_search(const char *str);
static NUMCacheEntry *NUM_cache_fetch(const char *str);


/* ----------
 * Fast sequential search, use index for data selection which
 * go to seq. cycle (it is very fast for unwanted strings)
 * (can't be used binary search in format parsing)
 * ----------
 */
static const KeyWord *
index_seq_search(const char *str, const KeyWord *kw, const int *index)
{
	int			poz;

	if (!KeyWord_INDEX_FILTER(*str))
		return NULL;

	if ((poz = *(index + (*str - ' '))) > -1)
	{
		const KeyWord *k = kw + poz;

		do
		{
			if (strncmp(str, k->name, k->len) == 0)
				return k;
			k++;
			if (!k->name)
				return NULL;
		} while (*str == *k->name);
	}
	return NULL;
}

static const KeySuffix *
suff_search(const char *str, const KeySuffix *suf, int type)
{
	const KeySuffix *s;

	for (s = suf; s->name != NULL; s++)
	{
		if (s->type != type)
			continue;

		if (strncmp(str, s->name, s->len) == 0)
			return s;
	}
	return NULL;
}

static bool
is_separator_char(const char *str)
{
	/* ASCII printable character, but not letter or digit */
	return (*str > 0x20 && *str < 0x7F &&
			!(*str >= 'A' && *str <= 'Z') &&
			!(*str >= 'a' && *str <= 'z') &&
			!(*str >= '0' && *str <= '9'));
}

/* ----------
 * Prepare NUMDesc (number description struct) via FormatNode struct
 * ----------
 */
static void
NUMDesc_prepare(NUMDesc *num, FormatNode *n)
{
	if (n->type != NODE_TYPE_ACTION)
		return;

	if (IS_EEEE(num) && n->key->id != NUM_E)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("\"EEEE\" must be the last pattern used")));

	switch (n->key->id)
	{
		case NUM_9:
			if (IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"9\" must be ahead of \"PR\"")));
			if (IS_MULTI(num))
			{
				++num->multi;
				break;
			}
			if (IS_DECIMAL(num))
				++num->post;
			else
				++num->pre;
			break;

		case NUM_0:
			if (IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"0\" must be ahead of \"PR\"")));
			if (!IS_ZERO(num) && !IS_DECIMAL(num))
			{
				num->flag |= NUM_F_ZERO;
				num->zero_start = num->pre + 1;
			}
			if (!IS_DECIMAL(num))
				++num->pre;
			else
				++num->post;

			num->zero_end = num->pre + num->post;
			break;

		case NUM_B:
			if (num->pre == 0 && num->post == 0 && (!IS_ZERO(num)))
				num->flag |= NUM_F_BLANK;
			break;

		case NUM_D:
			num->flag |= NUM_F_LDECIMAL;
			num->need_locale = true;
			/* FALLTHROUGH */
		case NUM_DEC:
			if (IS_DECIMAL(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("multiple decimal points")));
			if (IS_MULTI(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"V\" and decimal point together")));
			num->flag |= NUM_F_DECIMAL;
			break;

		case NUM_FM:
			num->flag |= NUM_F_FILLMODE;
			break;

		case NUM_S:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" twice")));
			if (IS_PLUS(num) || IS_MINUS(num) || IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\"/\"MI\"/\"SG\"/\"PR\" together")));
			if (!IS_DECIMAL(num))
			{
				num->lsign = NUM_LSIGN_PRE;
				num->pre_lsign_num = num->pre;
				num->need_locale = true;
				num->flag |= NUM_F_LSIGN;
			}
			else if (num->lsign == NUM_LSIGN_NONE)
			{
				num->lsign = NUM_LSIGN_POST;
				num->need_locale = true;
				num->flag |= NUM_F_LSIGN;
			}
			break;

		case NUM_MI:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"MI\" together")));
			num->flag |= NUM_F_MINUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_MINUS_POST;
			break;

		case NUM_PL:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\" together")));
			num->flag |= NUM_F_PLUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_PLUS_POST;
			break;

		case NUM_SG:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"SG\" together")));
			num->flag |= NUM_F_MINUS;
			num->flag |= NUM_F_PLUS;
			break;

		case NUM_PR:
			if (IS_LSIGN(num) || IS_PLUS(num) || IS_MINUS(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"PR\" and \"S\"/\"PL\"/\"MI\"/\"SG\" together")));
			num->flag |= NUM_F_BRACKET;
			break;

		case NUM_rn:
		case NUM_RN:
			num->flag |= NUM_F_ROMAN;
			break;

		case NUM_L:
		case NUM_G:
			num->need_locale = true;
			break;

		case NUM_V:
			if (IS_DECIMAL(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"V\" and decimal point together")));
			num->flag |= NUM_F_MULTI;
			break;

		case NUM_E:
			if (IS_EEEE(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"EEEE\" twice")));
			if (IS_BLANK(num) || IS_FILLMODE(num) || IS_LSIGN(num) ||
				IS_BRACKET(num) || IS_MINUS(num) || IS_PLUS(num) ||
				IS_ROMAN(num) || IS_MULTI(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"EEEE\" is incompatible with other formats"),
						 errdetail("\"EEEE\" may only be used together with digit and decimal point patterns.")));
			num->flag |= NUM_F_EEEE;
			break;
	}
}

/* ----------
 * Format parser, search small keywords and keyword's suffixes, and make
 * format-node tree.
 *
 * for DATE-TIME & NUMBER version
 * ----------
 */
static void
parse_format(FormatNode *node, const char *str, const KeyWord *kw,
			 const KeySuffix *suf, const int *index, uint32 flags, NUMDesc *Num)
{
	FormatNode *n;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "to_char/number(): run parser");
#endif

	n = node;

	while (*str)
	{
		int			suffix = 0;
		const KeySuffix *s;

		/*
		 * Prefix
		 */
		if ((flags & DCH_FLAG) &&
			(s = suff_search(str, suf, SUFFTYPE_PREFIX)) != NULL)
		{
			suffix |= s->id;
			if (s->len)
				str += s->len;
		}

		/*
		 * Keyword
		 */
		if (*str && (n->key = index_seq_search(str, kw, index)) != NULL)
		{
			n->type = NODE_TYPE_ACTION;
			n->suffix = suffix;
			if (n->key->len)
				str += n->key->len;

			/*
			 * NUM version: Prepare global NUMDesc struct
			 */
			if (flags & NUM_FLAG)
				NUMDesc_prepare(Num, n);

			/*
			 * Postfix
			 */
			if ((flags & DCH_FLAG) && *str &&
				(s = suff_search(str, suf, SUFFTYPE_POSTFIX)) != NULL)
			{
				n->suffix |= s->id;
				if (s->len)
					str += s->len;
			}

			n++;
		}
		else if (*str)
		{
			int			chlen;

			if ((flags & STD_FLAG) && *str != '"')
			{
				/*
				 * Standard mode, allow only following separators: "-./,':; ".
				 * However, we support double quotes even in standard mode
				 * (see below).  This is our extension of standard mode.
				 */
				if (strchr("-./,':; ", *str) == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("invalid datetime format separator: \"%s\"",
									pnstrdup(str, pg_mblen(str)))));

				if (*str == ' ')
					n->type = NODE_TYPE_SPACE;
				else
					n->type = NODE_TYPE_SEPARATOR;

				n->character[0] = *str;
				n->character[1] = '\0';
				n->key = NULL;
				n->suffix = 0;
				n++;
				str++;
			}
			else if (*str == '"')
			{
				/*
				 * Process double-quoted literal string, if any
				 */
				str++;
				while (*str)
				{
					if (*str == '"')
					{
						str++;
						break;
					}
					/* backslash quotes the next character, if any */
					if (*str == '\\' && *(str + 1))
						str++;
					chlen = pg_mblen(str);
					n->type = NODE_TYPE_CHAR;
					memcpy(n->character, str, chlen);
					n->character[chlen] = '\0';
					n->key = NULL;
					n->suffix = 0;
					n++;
					str += chlen;
				}
			}
			else
			{
				/*
				 * Outside double-quoted strings, backslash is only special if
				 * it immediately precedes a double quote.
				 */
				if (*str == '\\' && *(str + 1) == '"')
					str++;
				chlen = pg_mblen(str);

				if ((flags & DCH_FLAG) && is_separator_char(str))
					n->type = NODE_TYPE_SEPARATOR;
				else if (isspace((unsigned char) *str))
					n->type = NODE_TYPE_SPACE;
				else
					n->type = NODE_TYPE_CHAR;

				memcpy(n->character, str, chlen);
				n->character[chlen] = '\0';
				n->key = NULL;
				n->suffix = 0;
				n++;
				str += chlen;
			}
		}
	}

	n->type = NODE_TYPE_END;
	n->suffix = 0;
}

/* ----------
 * DEBUG: Dump the FormatNode Tree (debug)
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR

#define DUMP_THth(_suf) (S_TH(_suf) ? "TH" : (S_th(_suf) ? "th" : " "))
#define DUMP_FM(_suf)	(S_FM(_suf) ? "FM" : " ")

static void
dump_node(FormatNode *node, int max)
{
	FormatNode *n;
	int			a;

	elog(DEBUG_elog_output, "to_from-char(): DUMP FORMAT");

	for (a = 0, n = node; a <= max; n++, a++)
	{
		if (n->type == NODE_TYPE_ACTION)
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_ACTION '%s'\t(%s,%s)",
				 a, n->key->name, DUMP_THth(n->suffix), DUMP_FM(n->suffix));
		else if (n->type == NODE_TYPE_CHAR)
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_CHAR '%s'",
				 a, n->character);
		else if (n->type == NODE_TYPE_END)
		{
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_END", a);
			return;
		}
		else
			elog(DEBUG_elog_output, "%d:\t unknown NODE!", a);
	}
}
#endif							/* DEBUG */

/*****************************************************************************
 *			Private utils
 *****************************************************************************/

/* ----------
 * Return ST/ND/RD/TH for simple (1..9) numbers
 * type --> 0 upper, 1 lower
 * ----------
 */
static const char *
get_th(char *num, int type)
{
	int			len = strlen(num),
				last;

	last = *(num + (len - 1));
	if (!isdigit((unsigned char) last))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("\"%s\" is not a number", num)));

	/*
	 * All "teens" (<x>1[0-9]) get 'TH/th', while <x>[02-9][123] still get
	 * 'ST/st', 'ND/nd', 'RD/rd', respectively
	 */
	if ((len > 1) && (num[len - 2] == '1'))
		last = 0;

	switch (last)
	{
		case '1':
			if (type == TH_UPPER)
				return numTH[0];
			return numth[0];
		case '2':
			if (type == TH_UPPER)
				return numTH[1];
			return numth[1];
		case '3':
			if (type == TH_UPPER)
				return numTH[2];
			return numth[2];
		default:
			if (type == TH_UPPER)
				return numTH[3];
			return numth[3];
	}
}

/* ----------
 * Convert string-number to ordinal string-number
 * type --> 0 upper, 1 lower
 * ----------
 */
static char *
str_numth(char *dest, char *num, int type)
{
	if (dest != num)
		strcpy(dest, num);
	strcat(dest, get_th(num, type));
	return dest;
}

/*****************************************************************************
 *			upper/lower/initcap functions
 *****************************************************************************/

/*
 * If the system provides the needed functions for wide-character manipulation
 * (which are all standardized by C99), then we implement upper/lower/initcap
 * using wide-character functions, if necessary.  Otherwise we use the
 * traditional <ctype.h> functions, which of course will not work as desired
 * in multibyte character sets.  Note that in either case we are effectively
 * assuming that the database character encoding matches the encoding implied
 * by LC_CTYPE.
 */

/*
 * collation-aware, wide-character-aware lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
str_tolower(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	pg_locale_t mylocale;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"lower()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	mylocale = pg_newlocale_from_collation(collid);

	/* C/POSIX collations use this path regardless of database encoding */
	if (mylocale->ctype_is_c)
	{
		result = asc_tolower(buff, nbytes);
	}
	else
	{
		const char *src = buff;
		size_t		srclen = nbytes;
		size_t		dstsize;
		char	   *dst;
		size_t		needed;

		/* first try buffer of equal size plus terminating NUL */
		dstsize = srclen + 1;
		dst = palloc(dstsize);

		needed = pg_strlower(dst, dstsize, src, srclen, mylocale);
		if (needed + 1 > dstsize)
		{
			/* grow buffer if needed and retry */
			dstsize = needed + 1;
			dst = repalloc(dst, dstsize);
			needed = pg_strlower(dst, dstsize, src, srclen, mylocale);
			Assert(needed + 1 <= dstsize);
		}

		Assert(dst[needed] == '\0');
		result = dst;
	}

	return result;
}

/*
 * collation-aware, wide-character-aware upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
str_toupper(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	pg_locale_t mylocale;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"upper()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	mylocale = pg_newlocale_from_collation(collid);

	/* C/POSIX collations use this path regardless of database encoding */
	if (mylocale->ctype_is_c)
	{
		result = asc_toupper(buff, nbytes);
	}
	else
	{
		const char *src = buff;
		size_t		srclen = nbytes;
		size_t		dstsize;
		char	   *dst;
		size_t		needed;

		/* first try buffer of equal size plus terminating NUL */
		dstsize = srclen + 1;
		dst = palloc(dstsize);

		needed = pg_strupper(dst, dstsize, src, srclen, mylocale);
		if (needed + 1 > dstsize)
		{
			/* grow buffer if needed and retry */
			dstsize = needed + 1;
			dst = repalloc(dst, dstsize);
			needed = pg_strupper(dst, dstsize, src, srclen, mylocale);
			Assert(needed + 1 <= dstsize);
		}

		Assert(dst[needed] == '\0');
		result = dst;
	}

	return result;
}

/*
 * collation-aware, wide-character-aware initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
str_initcap(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	pg_locale_t mylocale;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"initcap()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	mylocale = pg_newlocale_from_collation(collid);

	/* C/POSIX collations use this path regardless of database encoding */
	if (mylocale->ctype_is_c)
	{
		result = asc_initcap(buff, nbytes);
	}
	else
	{
		const char *src = buff;
		size_t		srclen = nbytes;
		size_t		dstsize;
		char	   *dst;
		size_t		needed;

		/* first try buffer of equal size plus terminating NUL */
		dstsize = srclen + 1;
		dst = palloc(dstsize);

		needed = pg_strtitle(dst, dstsize, src, srclen, mylocale);
		if (needed + 1 > dstsize)
		{
			/* grow buffer if needed and retry */
			dstsize = needed + 1;
			dst = repalloc(dst, dstsize);
			needed = pg_strtitle(dst, dstsize, src, srclen, mylocale);
			Assert(needed + 1 <= dstsize);
		}

		Assert(dst[needed] == '\0');
		result = dst;
	}

	return result;
}

/*
 * ASCII-only lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
asc_tolower(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_tolower((unsigned char) *p);

	return result;
}

/*
 * ASCII-only upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
asc_toupper(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_toupper((unsigned char) *p);

	return result;
}

/*
 * ASCII-only initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *
asc_initcap(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;
	int			wasalnum = false;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
	{
		char		c;

		if (wasalnum)
			*p = c = pg_ascii_tolower((unsigned char) *p);
		else
			*p = c = pg_ascii_toupper((unsigned char) *p);
		/* we don't trust isalnum() here */
		wasalnum = ((c >= 'A' && c <= 'Z') ||
					(c >= 'a' && c <= 'z') ||
					(c >= '0' && c <= '9'));
	}

	return result;
}

/* convenience routines for when the input is null-terminated */

static char *
str_tolower_z(const char *buff, Oid collid)
{
	return str_tolower(buff, strlen(buff), collid);
}

static char *
str_toupper_z(const char *buff, Oid collid)
{
	return str_toupper(buff, strlen(buff), collid);
}

static char *
str_initcap_z(const char *buff, Oid collid)
{
	return str_initcap(buff, strlen(buff), collid);
}

static char *
asc_tolower_z(const char *buff)
{
	return asc_tolower(buff, strlen(buff));
}

static char *
asc_toupper_z(const char *buff)
{
	return asc_toupper(buff, strlen(buff));
}

/* asc_initcap_z is not currently needed */


/* ----------
 * Skip TM / th in FROM_CHAR
 *
 * If S_THth is on, skip two chars, assuming there are two available
 * ----------
 */
#define SKIP_THth(ptr, _suf) \
	do { \
		if (S_THth(_suf)) \
		{ \
			if (*(ptr)) (ptr) += pg_mblen(ptr); \
			if (*(ptr)) (ptr) += pg_mblen(ptr); \
		} \
	} while (0)


#ifdef DEBUG_TO_FROM_CHAR
/* -----------
 * DEBUG: Call for debug and for index checking; (Show ASCII char
 * and defined keyword for each used position
 * ----------
 */
static void
dump_index(const KeyWord *k, const int *index)
{
	int			i,
				count = 0,
				free_i = 0;

	elog(DEBUG_elog_output, "TO-FROM_CHAR: Dump KeyWord Index:");

	for (i = 0; i < KeyWord_INDEX_SIZE; i++)
	{
		if (index[i] != -1)
		{
			elog(DEBUG_elog_output, "\t%c: %s, ", i + 32, k[index[i]].name);
			count++;
		}
		else
		{
			free_i++;
			elog(DEBUG_elog_output, "\t(%d) %c %d", i, i + 32, index[i]);
		}
	}
	elog(DEBUG_elog_output, "\n\t\tUsed positions: %d,\n\t\tFree positions: %d",
		 count, free_i);
}
#endif							/* DEBUG */

/* ----------
 * Return true if next format picture is not digit value
 * ----------
 */
static bool
is_next_separator(FormatNode *n)
{
	if (n->type == NODE_TYPE_END)
		return false;

	if (n->type == NODE_TYPE_ACTION && S_THth(n->suffix))
		return true;

	/*
	 * Next node
	 */
	n++;

	/* end of format string is treated like a non-digit separator */
	if (n->type == NODE_TYPE_END)
		return true;

	if (n->type == NODE_TYPE_ACTION)
	{
		if (n->key->is_digit)
			return false;

		return true;
	}
	else if (n->character[1] == '\0' &&
			 isdigit((unsigned char) n->character[0]))
		return false;

	return true;				/* some non-digit input (separator) */
}


static int
adjust_partial_year_to_2020(int year)
{
	/*
	 * Adjust all dates toward 2020; this is effectively what happens when we
	 * assume '70' is 1970 and '69' is 2069.
	 */
	/* Force 0-69 into the 2000's */
	if (year < 70)
		return year + 2000;
	/* Force 70-99 into the 1900's */
	else if (year < 100)
		return year + 1900;
	/* Force 100-519 into the 2000's */
	else if (year < 520)
		return year + 2000;
	/* Force 520-999 into the 1000's */
	else if (year < 1000)
		return year + 1000;
	else
		return year;
}


static int
strspace_len(const char *str)
{
	int			len = 0;

	while (*str && isspace((unsigned char) *str))
	{
		str++;
		len++;
	}
	return len;
}

/*
 * Set the date mode of a from-char conversion.
 *
 * Puke if the date mode has already been set, and the caller attempts to set
 * it to a conflicting mode.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
static bool
from_char_set_mode(TmFromChar *tmfc, const FromCharDateMode mode,
				   Node *escontext)
{
	if (mode != FROM_CHAR_DATE_NONE)
	{
		if (tmfc->mode == FROM_CHAR_DATE_NONE)
			tmfc->mode = mode;
		else if (tmfc->mode != mode)
			ereturn(escontext, false,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("invalid combination of date conventions"),
					 errhint("Do not mix Gregorian and ISO week date "
							 "conventions in a formatting template.")));
	}
	return true;
}

/*
 * Set the integer pointed to by 'dest' to the given value.
 *
 * Puke if the destination integer has previously been set to some other
 * non-zero value.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
static bool
from_char_set_int(int *dest, const int value, const FormatNode *node,
				  Node *escontext)
{
	if (*dest != 0 && *dest != value)
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("conflicting values for \"%s\" field in formatting string",
						node->key->name),
				 errdetail("This value contradicts a previous setting "
						   "for the same field type.")));
	*dest = value;
	return true;
}

/*
 * Read a single integer from the source string, into the int pointed to by
 * 'dest'. If 'dest' is NULL, the result is discarded.
 *
 * In fixed-width mode (the node does not have the FM suffix), consume at most
 * 'len' characters.  However, any leading whitespace isn't counted in 'len'.
 *
 * We use strtol() to recover the integer value from the source string, in
 * accordance with the given FormatNode.
 *
 * If the conversion completes successfully, src will have been advanced to
 * point at the character immediately following the last character used in the
 * conversion.
 *
 * Returns the number of characters consumed, or -1 on error (if escontext
 * points to an ErrorSaveContext; otherwise errors are thrown).
 *
 * Note that from_char_parse_int() provides a more convenient wrapper where
 * the length of the field is the same as the length of the format keyword (as
 * with DD and MI).
 */
static int
from_char_parse_int_len(int *dest, const char **src, const int len, FormatNode *node,
						Node *escontext)
{
	long		result;
	char		copy[DCH_MAX_ITEM_SIZ + 1];
	const char *init = *src;
	int			used;

	/*
	 * Skip any whitespace before parsing the integer.
	 */
	*src += strspace_len(*src);

	Assert(len <= DCH_MAX_ITEM_SIZ);
	used = (int) strlcpy(copy, *src, len + 1);

	if (S_FM(node->suffix) || is_next_separator(node))
	{
		/*
		 * This node is in Fill Mode, or the next node is known to be a
		 * non-digit value, so we just slurp as many characters as we can get.
		 */
		char	   *endptr;

		errno = 0;
		result = strtol(init, &endptr, 10);
		*src = endptr;
	}
	else
	{
		/*
		 * We need to pull exactly the number of characters given in 'len' out
		 * of the string, and convert those.
		 */
		char	   *last;

		if (used < len)
			ereturn(escontext, -1,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("source string too short for \"%s\" formatting field",
							node->key->name),
					 errdetail("Field requires %d characters, but only %d remain.",
							   len, used),
					 errhint("If your source string is not fixed-width, "
							 "try using the \"FM\" modifier.")));

		errno = 0;
		result = strtol(copy, &last, 10);
		used = last - copy;

		if (used > 0 && used < len)
			ereturn(escontext, -1,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("invalid value \"%s\" for \"%s\"",
							copy, node->key->name),
					 errdetail("Field requires %d characters, but only %d could be parsed.",
							   len, used),
					 errhint("If your source string is not fixed-width, "
							 "try using the \"FM\" modifier.")));

		*src += used;
	}

	if (*src == init)
		ereturn(escontext, -1,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid value \"%s\" for \"%s\"",
						copy, node->key->name),
				 errdetail("Value must be an integer.")));

	if (errno == ERANGE || result < INT_MIN || result > INT_MAX)
		ereturn(escontext, -1,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("value for \"%s\" in source string is out of range",
						node->key->name),
				 errdetail("Value must be in the range %d to %d.",
						   INT_MIN, INT_MAX)));

	if (dest != NULL)
	{
		if (!from_char_set_int(dest, (int) result, node, escontext))
			return -1;
	}

	return *src - init;
}

/*
 * Call from_char_parse_int_len(), using the length of the format keyword as
 * the expected length of the field.
 *
 * Don't call this function if the field differs in length from the format
 * keyword (as with HH24; the keyword length is 4, but the field length is 2).
 * In such cases, call from_char_parse_int_len() instead to specify the
 * required length explicitly.
 */
static int
from_char_parse_int(int *dest, const char **src, FormatNode *node,
					Node *escontext)
{
	return from_char_parse_int_len(dest, src, node->key->len, node, escontext);
}

/*
 * Sequentially search null-terminated "array" for a case-insensitive match
 * to the initial character(s) of "name".
 *
 * Returns array index of match, or -1 for no match.
 *
 * *len is set to the length of the match, or 0 for no match.
 *
 * Case-insensitivity is defined per pg_ascii_tolower, so this is only
 * suitable for comparisons to ASCII strings.
 */
static int
seq_search_ascii(const char *name, const char *const *array, int *len)
{
	unsigned char firstc;
	const char *const *a;

	*len = 0;

	/* empty string can't match anything */
	if (!*name)
		return -1;

	/* we handle first char specially to gain some speed */
	firstc = pg_ascii_tolower((unsigned char) *name);

	for (a = array; *a != NULL; a++)
	{
		const char *p;
		const char *n;

		/* compare first chars */
		if (pg_ascii_tolower((unsigned char) **a) != firstc)
			continue;

		/* compare rest of string */
		for (p = *a + 1, n = name + 1;; p++, n++)
		{
			/* return success if we matched whole array entry */
			if (*p == '\0')
			{
				*len = n - name;
				return a - array;
			}
			/* else, must have another character in "name" ... */
			if (*n == '\0')
				break;
			/* ... and it must match */
			if (pg_ascii_tolower((unsigned char) *p) !=
				pg_ascii_tolower((unsigned char) *n))
				break;
		}
	}

	return -1;
}

/*
 * Sequentially search an array of possibly non-English words for
 * a case-insensitive match to the initial character(s) of "name".
 *
 * This has the same API as seq_search_ascii(), but we use a more general
 * case-folding transformation to achieve case-insensitivity.  Case folding
 * is done per the rules of the collation identified by "collid".
 *
 * The array is treated as const, but we don't declare it that way because
 * the arrays exported by pg_locale.c aren't const.
 */
static int
seq_search_localized(const char *name, char **array, int *len, Oid collid)
{
	char	  **a;
	char	   *upper_name;
	char	   *lower_name;

	*len = 0;

	/* empty string can't match anything */
	if (!*name)
		return -1;

	/*
	 * The case-folding processing done below is fairly expensive, so before
	 * doing that, make a quick pass to see if there is an exact match.
	 */
	for (a = array; *a != NULL; a++)
	{
		int			element_len = strlen(*a);

		if (strncmp(name, *a, element_len) == 0)
		{
			*len = element_len;
			return a - array;
		}
	}

	/*
	 * Fold to upper case, then to lower case, so that we can match reliably
	 * even in languages in which case conversions are not injective.
	 */
	upper_name = str_toupper(name, strlen(name), collid);
	lower_name = str_tolower(upper_name, strlen(upper_name), collid);
	pfree(upper_name);

	for (a = array; *a != NULL; a++)
	{
		char	   *upper_element;
		char	   *lower_element;
		int			element_len;

		/* Likewise upper/lower-case array element */
		upper_element = str_toupper(*a, strlen(*a), collid);
		lower_element = str_tolower(upper_element, strlen(upper_element),
									collid);
		pfree(upper_element);
		element_len = strlen(lower_element);

		/* Match? */
		if (strncmp(lower_name, lower_element, element_len) == 0)
		{
			*len = element_len;
			pfree(lower_element);
			pfree(lower_name);
			return a - array;
		}
		pfree(lower_element);
	}

	pfree(lower_name);
	return -1;
}

/*
 * Perform a sequential search in 'array' (or 'localized_array', if that's
 * not NULL) for an entry matching the first character(s) of the 'src'
 * string case-insensitively.
 *
 * The 'array' is presumed to be English words (all-ASCII), but
 * if 'localized_array' is supplied, that might be non-English
 * so we need a more expensive case-folding transformation
 * (which will follow the rules of the collation 'collid').
 *
 * If a match is found, copy the array index of the match into the integer
 * pointed to by 'dest' and advance 'src' to the end of the part of the string
 * which matched.
 *
 * Returns true on match, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 *
 * 'node' is used only for error reports: node->key->name identifies the
 * field type we were searching for.
 */
static bool
from_char_seq_search(int *dest, const char **src, const char *const *array,
					 char **localized_array, Oid collid,
					 FormatNode *node, Node *escontext)
{
	int			len;

	if (localized_array == NULL)
		*dest = seq_search_ascii(*src, array, &len);
	else
		*dest = seq_search_localized(*src, localized_array, &len, collid);

	if (len <= 0)
	{
		/*
		 * In the error report, truncate the string at the next whitespace (if
		 * any) to avoid including irrelevant data.
		 */
		char	   *copy = pstrdup(*src);
		char	   *c;

		for (c = copy; *c; c++)
		{
			if (scanner_isspace(*c))
			{
				*c = '\0';
				break;
			}
		}

		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid value \"%s\" for \"%s\"",
						copy, node->key->name),
				 errdetail("The given value did not match any of "
						   "the allowed values for this field.")));
	}
	*src += len;
	return true;
}

/* ----------
 * Process a TmToChar struct as denoted by a list of FormatNodes.
 * The formatted data is written to the string pointed to by 'out'.
 * ----------
 */
static void
DCH_to_char(FormatNode *node, bool is_interval, TmToChar *in, char *out, Oid collid)
{
	FormatNode *n;
	char	   *s;
	struct fmt_tm *tm = &in->tm;
	int			i;

	/* cache localized days and months */
	cache_locale_time();

	s = out;
	for (n = node; n->type != NODE_TYPE_END; n++)
	{
		if (n->type != NODE_TYPE_ACTION)
		{
			strcpy(s, n->character);
			s += strlen(s);
			continue;
		}

		switch (n->key->id)
		{
			case DCH_A_M:
			case DCH_P_M:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? P_M_STR : A_M_STR);
				s += strlen(s);
				break;
			case DCH_AM:
			case DCH_PM:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? PM_STR : AM_STR);
				s += strlen(s);
				break;
			case DCH_a_m:
			case DCH_p_m:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? p_m_STR : a_m_STR);
				s += strlen(s);
				break;
			case DCH_am:
			case DCH_pm:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? pm_STR : am_STR);
				s += strlen(s);
				break;
			case DCH_HH:
			case DCH_HH12:

				/*
				 * display time as shown on a 12-hour clock, even for
				 * intervals
				 */
				sprintf(s, "%0*lld", S_FM(n->suffix) ? 0 : (tm->tm_hour >= 0) ? 2 : 3,
						tm->tm_hour % (HOURS_PER_DAY / 2) == 0 ?
						(long long) (HOURS_PER_DAY / 2) :
						(long long) (tm->tm_hour % (HOURS_PER_DAY / 2)));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_HH24:
				sprintf(s, "%0*lld", S_FM(n->suffix) ? 0 : (tm->tm_hour >= 0) ? 2 : 3,
						(long long) tm->tm_hour);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_MI:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_min >= 0) ? 2 : 3,
						tm->tm_min);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_SS:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_sec >= 0) ? 2 : 3,
						tm->tm_sec);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;

#define DCH_to_char_fsec(frac_fmt, frac_val) \
				sprintf(s, frac_fmt, (int) (frac_val)); \
				if (S_THth(n->suffix)) \
					str_numth(s, s, S_TH_TYPE(n->suffix)); \
				s += strlen(s)

			case DCH_FF1:		/* tenth of second */
				DCH_to_char_fsec("%01d", in->fsec / 100000);
				break;
			case DCH_FF2:		/* hundredth of second */
				DCH_to_char_fsec("%02d", in->fsec / 10000);
				break;
			case DCH_FF3:
			case DCH_MS:		/* millisecond */
				DCH_to_char_fsec("%03d", in->fsec / 1000);
				break;
			case DCH_FF4:		/* tenth of a millisecond */
				DCH_to_char_fsec("%04d", in->fsec / 100);
				break;
			case DCH_FF5:		/* hundredth of a millisecond */
				DCH_to_char_fsec("%05d", in->fsec / 10);
				break;
			case DCH_FF6:
			case DCH_US:		/* microsecond */
				DCH_to_char_fsec("%06d", in->fsec);
				break;
#undef DCH_to_char_fsec
			case DCH_SSSS:
				sprintf(s, "%lld",
						(long long) (tm->tm_hour * SECS_PER_HOUR +
									 tm->tm_min * SECS_PER_MINUTE +
									 tm->tm_sec));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_tz:
				INVALID_FOR_INTERVAL;
				if (tmtcTzn(in))
				{
					/* We assume here that timezone names aren't localized */
					char	   *p = asc_tolower_z(tmtcTzn(in));

					strcpy(s, p);
					pfree(p);
					s += strlen(s);
				}
				break;
			case DCH_TZ:
				INVALID_FOR_INTERVAL;
				if (tmtcTzn(in))
				{
					strcpy(s, tmtcTzn(in));
					s += strlen(s);
				}
				break;
			case DCH_TZH:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%c%02d",
						(tm->tm_gmtoff >= 0) ? '+' : '-',
						abs((int) tm->tm_gmtoff) / SECS_PER_HOUR);
				s += strlen(s);
				break;
			case DCH_TZM:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%02d",
						(abs((int) tm->tm_gmtoff) % SECS_PER_HOUR) / SECS_PER_MINUTE);
				s += strlen(s);
				break;
			case DCH_OF:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%c%0*d",
						(tm->tm_gmtoff >= 0) ? '+' : '-',
						S_FM(n->suffix) ? 0 : 2,
						abs((int) tm->tm_gmtoff) / SECS_PER_HOUR);
				s += strlen(s);
				if (abs((int) tm->tm_gmtoff) % SECS_PER_HOUR != 0)
				{
					sprintf(s, ":%02d",
							(abs((int) tm->tm_gmtoff) % SECS_PER_HOUR) / SECS_PER_MINUTE);
					s += strlen(s);
				}
				break;
			case DCH_A_D:
			case DCH_B_C:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? B_C_STR : A_D_STR));
				s += strlen(s);
				break;
			case DCH_AD:
			case DCH_BC:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? BC_STR : AD_STR));
				s += strlen(s);
				break;
			case DCH_a_d:
			case DCH_b_c:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? b_c_STR : a_d_STR));
				s += strlen(s);
				break;
			case DCH_ad:
			case DCH_bc:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? bc_STR : ad_STR));
				s += strlen(s);
				break;
			case DCH_MONTH:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_toupper_z(months_full[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_Month:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							months_full[tm->tm_mon - 1]);
				s += strlen(s);
				break;
			case DCH_month:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_tolower_z(months_full[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_MON:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_toupper_z(months[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_Mon:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, months[tm->tm_mon - 1]);
				s += strlen(s);
				break;
			case DCH_mon:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_tolower_z(months[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_MM:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_mon >= 0) ? 2 : 3,
						tm->tm_mon);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_DAY:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_toupper_z(days[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_Day:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							days[tm->tm_wday]);
				s += strlen(s);
				break;
			case DCH_day:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_tolower_z(days[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_DY:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_toupper_z(days_short[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_Dy:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, days_short[tm->tm_wday]);
				s += strlen(s);
				break;
			case DCH_dy:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_tolower_z(days_short[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_DDD:
			case DCH_IDDD:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 3,
						(n->key->id == DCH_DDD) ?
						tm->tm_yday :
						date2isoyearday(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_DD:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2, tm->tm_mday);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_D:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%d", tm->tm_wday + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_ID:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%d", (tm->tm_wday == 0) ? 7 : tm->tm_wday);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_WW:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2,
						(tm->tm_yday - 1) / 7 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_IW:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2,
						date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Q:
				if (!tm->tm_mon)
					break;
				sprintf(s, "%d", (tm->tm_mon - 1) / 3 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_CC:
				if (is_interval)	/* straight calculation */
					i = tm->tm_year / 100;
				else
				{
					if (tm->tm_year > 0)
						/* Century 20 == 1901 - 2000 */
						i = (tm->tm_year - 1) / 100 + 1;
					else
						/* Century 6BC == 600BC - 501BC */
						i = tm->tm_year / 100 - 1;
				}
				if (i <= 99 && i >= -99)
					sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (i >= 0) ? 2 : 3, i);
				else
					sprintf(s, "%d", i);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Y_YYY:
				i = ADJUST_YEAR(tm->tm_year, is_interval) / 1000;
				sprintf(s, "%d,%03d", i,
						ADJUST_YEAR(tm->tm_year, is_interval) - (i * 1000));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YYYY:
			case DCH_IYYY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 4 : 5,
						(n->key->id == DCH_YYYY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YYY:
			case DCH_IYY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 3 : 4,
						(n->key->id == DCH_YYY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 1000);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YY:
			case DCH_IY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 2 : 3,
						(n->key->id == DCH_YY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 100);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Y:
			case DCH_I:
				sprintf(s, "%1d",
						(n->key->id == DCH_Y ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 10);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_RM:
				/* FALLTHROUGH */
			case DCH_rm:

				/*
				 * For intervals, values like '12 month' will be reduced to 0
				 * month and some years.  These should be processed.
				 */
				if (!tm->tm_mon && !tm->tm_year)
					break;
				else
				{
					int			mon = 0;
					const char *const *months;

					if (n->key->id == DCH_RM)
						months = rm_months_upper;
					else
						months = rm_months_lower;

					/*
					 * Compute the position in the roman-numeral array.  Note
					 * that the contents of the array are reversed, December
					 * being first and January last.
					 */
					if (tm->tm_mon == 0)
					{
						/*
						 * This case is special, and tracks the case of full
						 * interval years.
						 */
						mon = tm->tm_year >= 0 ? 0 : MONTHS_PER_YEAR - 1;
					}
					else if (tm->tm_mon < 0)
					{
						/*
						 * Negative case.  In this case, the calculation is
						 * reversed, where -1 means December, -2 November,
						 * etc.
						 */
						mon = -1 * (tm->tm_mon + 1);
					}
					else
					{
						/*
						 * Common case, with a strictly positive value.  The
						 * position in the array matches with the value of
						 * tm_mon.
						 */
						mon = MONTHS_PER_YEAR - tm->tm_mon;
					}

					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -4,
							months[mon]);
					s += strlen(s);
				}
				break;
			case DCH_W:
				sprintf(s, "%d", (tm->tm_mday - 1) / 7 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_J:
				sprintf(s, "%d", date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
		}
	}

	*s = '\0';
}

/*
 * Process the string 'in' as denoted by the array of FormatNodes 'node[]'.
 * The TmFromChar struct pointed to by 'out' is populated with the results.
 *
 * 'collid' identifies the collation to use, if needed.
 * 'std' specifies standard parsing mode.
 *
 * If escontext points to an ErrorSaveContext, data errors will be reported
 * by filling that struct; the caller must test SOFT_ERROR_OCCURRED() to see
 * whether an error occurred.  Otherwise, errors are thrown.
 *
 * Note: we currently don't have any to_interval() function, so there
 * is no need here for INVALID_FOR_INTERVAL checks.
 */
static void
DCH_from_char(FormatNode *node, const char *in, TmFromChar *out,
			  Oid collid, bool std, Node *escontext)
{
	FormatNode *n;
	const char *s;
	int			len,
				value;
	bool		fx_mode = std;

	/* number of extra skipped characters (more than given in format string) */
	int			extra_skip = 0;

	/* cache localized days and months */
	cache_locale_time();

	for (n = node, s = in; n->type != NODE_TYPE_END && *s != '\0'; n++)
	{
		/*
		 * Ignore spaces at the beginning of the string and before fields when
		 * not in FX (fixed width) mode.
		 */
		if (!fx_mode && (n->type != NODE_TYPE_ACTION || n->key->id != DCH_FX) &&
			(n->type == NODE_TYPE_ACTION || n == node))
		{
			while (*s != '\0' && isspace((unsigned char) *s))
			{
				s++;
				extra_skip++;
			}
		}

		if (n->type == NODE_TYPE_SPACE || n->type == NODE_TYPE_SEPARATOR)
		{
			if (std)
			{
				/*
				 * Standard mode requires strict matching between format
				 * string separators/spaces and input string.
				 */
				Assert(n->character[0] && !n->character[1]);

				if (*s == n->character[0])
					s++;
				else
					ereturn(escontext,,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("unmatched format separator \"%c\"",
									n->character[0])));
			}
			else if (!fx_mode)
			{
				/*
				 * In non FX (fixed format) mode one format string space or
				 * separator match to one space or separator in input string.
				 * Or match nothing if there is no space or separator in the
				 * current position of input string.
				 */
				extra_skip--;
				if (isspace((unsigned char) *s) || is_separator_char(s))
				{
					s++;
					extra_skip++;
				}
			}
			else
			{
				/*
				 * In FX mode, on format string space or separator we consume
				 * exactly one character from input string.  Notice we don't
				 * insist that the consumed character match the format's
				 * character.
				 */
				s += pg_mblen(s);
			}
			continue;
		}
		else if (n->type != NODE_TYPE_ACTION)
		{
			/*
			 * Text character, so consume one character from input string.
			 * Notice we don't insist that the consumed character match the
			 * format's character.
			 */
			if (!fx_mode)
			{
				/*
				 * In non FX mode we might have skipped some extra characters
				 * (more than specified in format string) before.  In this
				 * case we don't skip input string character, because it might
				 * be part of field.
				 */
				if (extra_skip > 0)
					extra_skip--;
				else
					s += pg_mblen(s);
			}
			else
			{
				int			chlen = pg_mblen(s);

				/*
				 * Standard mode requires strict match of format characters.
				 */
				if (std && n->type == NODE_TYPE_CHAR &&
					strncmp(s, n->character, chlen) != 0)
					ereturn(escontext,,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("unmatched format character \"%s\"",
									n->character)));

				s += chlen;
			}
			continue;
		}

		if (!from_char_set_mode(out, n->key->date_mode, escontext))
			return;

		switch (n->key->id)
		{
			case DCH_FX:
				fx_mode = true;
				break;
			case DCH_A_M:
			case DCH_P_M:
			case DCH_a_m:
			case DCH_p_m:
				if (!from_char_seq_search(&value, &s, ampm_strings_long,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->pm, value % 2, n, escontext))
					return;
				out->clock = CLOCK_12_HOUR;
				break;
			case DCH_AM:
			case DCH_PM:
			case DCH_am:
			case DCH_pm:
				if (!from_char_seq_search(&value, &s, ampm_strings,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->pm, value % 2, n, escontext))
					return;
				out->clock = CLOCK_12_HOUR;
				break;
			case DCH_HH:
			case DCH_HH12:
				if (from_char_parse_int_len(&out->hh, &s, 2, n, escontext) < 0)
					return;
				out->clock = CLOCK_12_HOUR;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_HH24:
				if (from_char_parse_int_len(&out->hh, &s, 2, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_MI:
				if (from_char_parse_int(&out->mi, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_SS:
				if (from_char_parse_int(&out->ss, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_MS:		/* millisecond */
				len = from_char_parse_int_len(&out->ms, &s, 3, n, escontext);
				if (len < 0)
					return;

				/*
				 * 25 is 0.25 and 250 is 0.25 too; 025 is 0.025 and not 0.25
				 */
				out->ms *= len == 1 ? 100 :
					len == 2 ? 10 : 1;

				SKIP_THth(s, n->suffix);
				break;
			case DCH_FF1:
			case DCH_FF2:
			case DCH_FF3:
			case DCH_FF4:
			case DCH_FF5:
			case DCH_FF6:
				out->ff = n->key->id - DCH_FF1 + 1;
				/* FALLTHROUGH */
			case DCH_US:		/* microsecond */
				len = from_char_parse_int_len(&out->us, &s,
											  n->key->id == DCH_US ? 6 :
											  out->ff, n, escontext);
				if (len < 0)
					return;

				out->us *= len == 1 ? 100000 :
					len == 2 ? 10000 :
					len == 3 ? 1000 :
					len == 4 ? 100 :
					len == 5 ? 10 : 1;

				SKIP_THth(s, n->suffix);
				break;
			case DCH_SSSS:
				if (from_char_parse_int(&out->ssss, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_tz:
			case DCH_TZ:
				{
					int			tzlen;

					tzlen = DecodeTimezoneAbbrevPrefix(s,
													   &out->gmtoffset,
													   &out->tzp);
					if (tzlen > 0)
					{
						out->has_tz = true;
						/* we only need the zone abbrev for DYNTZ case */
						if (out->tzp)
							out->abbrev = pnstrdup(s, tzlen);
						out->tzsign = 0;	/* drop any earlier TZH/TZM info */
						s += tzlen;
						break;
					}
					else if (isalpha((unsigned char) *s))
					{
						/*
						 * It doesn't match any abbreviation, but it starts
						 * with a letter.  OF format certainly won't succeed;
						 * assume it's a misspelled abbreviation and complain
						 * accordingly.
						 */
						ereturn(escontext,,
								(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
								 errmsg("invalid value \"%s\" for \"%s\"",
										s, n->key->name),
								 errdetail("Time zone abbreviation is not recognized.")));
					}
					/* otherwise parse it like OF */
				}
				/* FALLTHROUGH */
			case DCH_OF:
				/* OF is equivalent to TZH or TZH:TZM */
				/* see TZH comments below */
				if (*s == '+' || *s == '-' || *s == ' ')
				{
					out->tzsign = *s == '-' ? -1 : +1;
					s++;
				}
				else
				{
					if (extra_skip > 0 && *(s - 1) == '-')
						out->tzsign = -1;
					else
						out->tzsign = +1;
				}
				if (from_char_parse_int_len(&out->tzh, &s, 2, n, escontext) < 0)
					return;
				if (*s == ':')
				{
					s++;
					if (from_char_parse_int_len(&out->tzm, &s, 2, n,
												escontext) < 0)
						return;
				}
				break;
			case DCH_TZH:

				/*
				 * Value of TZH might be negative.  And the issue is that we
				 * might swallow minus sign as the separator.  So, if we have
				 * skipped more characters than specified in the format
				 * string, then we consider prepending last skipped minus to
				 * TZH.
				 */
				if (*s == '+' || *s == '-' || *s == ' ')
				{
					out->tzsign = *s == '-' ? -1 : +1;
					s++;
				}
				else
				{
					if (extra_skip > 0 && *(s - 1) == '-')
						out->tzsign = -1;
					else
						out->tzsign = +1;
				}

				if (from_char_parse_int_len(&out->tzh, &s, 2, n, escontext) < 0)
					return;
				break;
			case DCH_TZM:
				/* assign positive timezone sign if TZH was not seen before */
				if (!out->tzsign)
					out->tzsign = +1;
				if (from_char_parse_int_len(&out->tzm, &s, 2, n, escontext) < 0)
					return;
				break;
			case DCH_A_D:
			case DCH_B_C:
			case DCH_a_d:
			case DCH_b_c:
				if (!from_char_seq_search(&value, &s, adbc_strings_long,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->bc, value % 2, n, escontext))
					return;
				break;
			case DCH_AD:
			case DCH_BC:
			case DCH_ad:
			case DCH_bc:
				if (!from_char_seq_search(&value, &s, adbc_strings,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->bc, value % 2, n, escontext))
					return;
				break;
			case DCH_MONTH:
			case DCH_Month:
			case DCH_month:
				if (!from_char_seq_search(&value, &s, months_full,
										  S_TM(n->suffix) ? localized_full_months : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->mm, value + 1, n, escontext))
					return;
				break;
			case DCH_MON:
			case DCH_Mon:
			case DCH_mon:
				if (!from_char_seq_search(&value, &s, months,
										  S_TM(n->suffix) ? localized_abbrev_months : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->mm, value + 1, n, escontext))
					return;
				break;
			case DCH_MM:
				if (from_char_parse_int(&out->mm, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_DAY:
			case DCH_Day:
			case DCH_day:
				if (!from_char_seq_search(&value, &s, days,
										  S_TM(n->suffix) ? localized_full_days : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->d, value, n, escontext))
					return;
				out->d++;
				break;
			case DCH_DY:
			case DCH_Dy:
			case DCH_dy:
				if (!from_char_seq_search(&value, &s, days_short,
										  S_TM(n->suffix) ? localized_abbrev_days : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->d, value, n, escontext))
					return;
				out->d++;
				break;
			case DCH_DDD:
				if (from_char_parse_int(&out->ddd, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_IDDD:
				if (from_char_parse_int_len(&out->ddd, &s, 3, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_DD:
				if (from_char_parse_int(&out->dd, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_D:
				if (from_char_parse_int(&out->d, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_ID:
				if (from_char_parse_int_len(&out->d, &s, 1, n, escontext) < 0)
					return;
				/* Shift numbering to match Gregorian where Sunday = 1 */
				if (++out->d > 7)
					out->d = 1;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_WW:
			case DCH_IW:
				if (from_char_parse_int(&out->ww, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_Q:

				/*
				 * We ignore 'Q' when converting to date because it is unclear
				 * which date in the quarter to use, and some people specify
				 * both quarter and month, so if it was honored it might
				 * conflict with the supplied month. That is also why we don't
				 * throw an error.
				 *
				 * We still parse the source string for an integer, but it
				 * isn't stored anywhere in 'out'.
				 */
				if (from_char_parse_int((int *) NULL, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_CC:
				if (from_char_parse_int(&out->cc, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_Y_YYY:
				{
					int			matched,
								years,
								millennia,
								nch;

					matched = sscanf(s, "%d,%03d%n", &millennia, &years, &nch);
					if (matched < 2)
						ereturn(escontext,,
								(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
								 errmsg("invalid input string for \"Y,YYY\"")));

					/* years += (millennia * 1000); */
					if (pg_mul_s32_overflow(millennia, 1000, &millennia) ||
						pg_add_s32_overflow(years, millennia, &years))
						ereturn(escontext,,
								(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
								 errmsg("value for \"Y,YYY\" in source string is out of range")));

					if (!from_char_set_int(&out->year, years, n, escontext))
						return;
					out->yysz = 4;
					s += nch;
					SKIP_THth(s, n->suffix);
				}
				break;
			case DCH_YYYY:
			case DCH_IYYY:
				if (from_char_parse_int(&out->year, &s, n, escontext) < 0)
					return;
				out->yysz = 4;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_YYY:
			case DCH_IYY:
				len = from_char_parse_int(&out->year, &s, n, escontext);
				if (len < 0)
					return;
				if (len < 4)
					out->year = adjust_partial_year_to_2020(out->year);
				out->yysz = 3;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_YY:
			case DCH_IY:
				len = from_char_parse_int(&out->year, &s, n, escontext);
				if (len < 0)
					return;
				if (len < 4)
					out->year = adjust_partial_year_to_2020(out->year);
				out->yysz = 2;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_Y:
			case DCH_I:
				len = from_char_parse_int(&out->year, &s, n, escontext);
				if (len < 0)
					return;
				if (len < 4)
					out->year = adjust_partial_year_to_2020(out->year);
				out->yysz = 1;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_RM:
			case DCH_rm:
				if (!from_char_seq_search(&value, &s, rm_months_lower,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->mm, MONTHS_PER_YEAR - value, n,
									   escontext))
					return;
				break;
			case DCH_W:
				if (from_char_parse_int(&out->w, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_J:
				if (from_char_parse_int(&out->j, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
		}

		/* Ignore all spaces after fields */
		if (!fx_mode)
		{
			extra_skip = 0;
			while (*s != '\0' && isspace((unsigned char) *s))
			{
				s++;
				extra_skip++;
			}
		}
	}

	/*
	 * Standard parsing mode doesn't allow unmatched format patterns or
	 * trailing characters in the input string.
	 */
	if (std)
	{
		if (n->type != NODE_TYPE_END)
			ereturn(escontext,,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("input string is too short for datetime format")));

		while (*s != '\0' && isspace((unsigned char) *s))
			s++;

		if (*s != '\0')
			ereturn(escontext,,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("trailing characters remain in input string after datetime format")));
	}
}

/*
 * The invariant for DCH cache entry management is that DCHCounter is equal
 * to the maximum age value among the existing entries, and we increment it
 * whenever an access occurs.  If we approach overflow, deal with that by
 * halving all the age values, so that we retain a fairly accurate idea of
 * which entries are oldest.
 */
static inline void
DCH_prevent_counter_overflow(void)
{
	if (DCHCounter >= (INT_MAX - 1))
	{
		for (int i = 0; i < n_DCHCache; i++)
			DCHCache[i]->age >>= 1;
		DCHCounter >>= 1;
	}
}

/*
 * Get mask of date/time/zone components present in format nodes.
 */
static int
DCH_datetime_type(FormatNode *node)
{
	FormatNode *n;
	int			flags = 0;

	for (n = node; n->type != NODE_TYPE_END; n++)
	{
		if (n->type != NODE_TYPE_ACTION)
			continue;

		switch (n->key->id)
		{
			case DCH_FX:
				break;
			case DCH_A_M:
			case DCH_P_M:
			case DCH_a_m:
			case DCH_p_m:
			case DCH_AM:
			case DCH_PM:
			case DCH_am:
			case DCH_pm:
			case DCH_HH:
			case DCH_HH12:
			case DCH_HH24:
			case DCH_MI:
			case DCH_SS:
			case DCH_MS:		/* millisecond */
			case DCH_US:		/* microsecond */
			case DCH_FF1:
			case DCH_FF2:
			case DCH_FF3:
			case DCH_FF4:
			case DCH_FF5:
			case DCH_FF6:
			case DCH_SSSS:
				flags |= DCH_TIMED;
				break;
			case DCH_tz:
			case DCH_TZ:
			case DCH_OF:
			case DCH_TZH:
			case DCH_TZM:
				flags |= DCH_ZONED;
				break;
			case DCH_A_D:
			case DCH_B_C:
			case DCH_a_d:
			case DCH_b_c:
			case DCH_AD:
			case DCH_BC:
			case DCH_ad:
			case DCH_bc:
			case DCH_MONTH:
			case DCH_Month:
			case DCH_month:
			case DCH_MON:
			case DCH_Mon:
			case DCH_mon:
			case DCH_MM:
			case DCH_DAY:
			case DCH_Day:
			case DCH_day:
			case DCH_DY:
			case DCH_Dy:
			case DCH_dy:
			case DCH_DDD:
			case DCH_IDDD:
			case DCH_DD:
			case DCH_D:
			case DCH_ID:
			case DCH_WW:
			case DCH_Q:
			case DCH_CC:
			case DCH_Y_YYY:
			case DCH_YYYY:
			case DCH_IYYY:
			case DCH_YYY:
			case DCH_IYY:
			case DCH_YY:
			case DCH_IY:
			case DCH_Y:
			case DCH_I:
			case DCH_RM:
			case DCH_rm:
			case DCH_W:
			case DCH_J:
				flags |= DCH_DATED;
				break;
		}
	}

	return flags;
}

/* select a DCHCacheEntry to hold the given format picture */
static DCHCacheEntry *
DCH_cache_getnew(const char *str, bool std)
{
	DCHCacheEntry *ent;

	/* Ensure we can advance DCHCounter below */
	DCH_prevent_counter_overflow();

	/*
	 * If cache is full, remove oldest entry (or recycle first not-valid one)
	 */
	if (n_DCHCache >= DCH_CACHE_ENTRIES)
	{
		DCHCacheEntry *old = DCHCache[0];

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "cache is full (%d)", n_DCHCache);
#endif
		if (old->valid)
		{
			for (int i = 1; i < DCH_CACHE_ENTRIES; i++)
			{
				ent = DCHCache[i];
				if (!ent->valid)
				{
					old = ent;
					break;
				}
				if (ent->age < old->age)
					old = ent;
			}
		}
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "OLD: '%s' AGE: %d", old->str, old->age);
#endif
		old->valid = false;
		strlcpy(old->str, str, DCH_CACHE_SIZE + 1);
		old->age = (++DCHCounter);
		/* caller is expected to fill format, then set valid */
		return old;
	}
	else
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "NEW (%d)", n_DCHCache);
#endif
		Assert(DCHCache[n_DCHCache] == NULL);
		DCHCache[n_DCHCache] = ent = (DCHCacheEntry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(DCHCacheEntry));
		ent->valid = false;
		strlcpy(ent->str, str, DCH_CACHE_SIZE + 1);
		ent->std = std;
		ent->age = (++DCHCounter);
		/* caller is expected to fill format, then set valid */
		++n_DCHCache;
		return ent;
	}
}

/* look for an existing DCHCacheEntry matching the given format picture */
static DCHCacheEntry *
DCH_cache_search(const char *str, bool std)
{
	/* Ensure we can advance DCHCounter below */
	DCH_prevent_counter_overflow();

	for (int i = 0; i < n_DCHCache; i++)
	{
		DCHCacheEntry *ent = DCHCache[i];

		if (ent->valid && strcmp(ent->str, str) == 0 && ent->std == std)
		{
			ent->age = (++DCHCounter);
			return ent;
		}
	}

	return NULL;
}

/* Find or create a DCHCacheEntry for the given format picture */
static DCHCacheEntry *
DCH_cache_fetch(const char *str, bool std)
{
	DCHCacheEntry *ent;

	if ((ent = DCH_cache_search(str, std)) == NULL)
	{
		/*
		 * Not in the cache, must run parser and save a new format-picture to
		 * the cache.  Do not mark the cache entry valid until parsing
		 * succeeds.
		 */
		ent = DCH_cache_getnew(str, std);

		parse_format(ent->format, str, DCH_keywords, DCH_suff, DCH_index,
					 DCH_FLAG | (std ? STD_FLAG : 0), NULL);

		ent->valid = true;
	}
	return ent;
}

/*
 * Format a date/time or interval into a string according to fmt.
 * We parse fmt into a list of FormatNodes.  This is then passed to DCH_to_char
 * for formatting.
 */
static text *
datetime_to_char_body(TmToChar *tmtc, text *fmt, bool is_interval, Oid collid)
{
	FormatNode *format;
	char	   *fmt_str,
			   *result;
	bool		incache;
	int			fmt_len;
	text	   *res;

	/*
	 * Convert fmt to C string
	 */
	fmt_str = text_to_cstring(fmt);
	fmt_len = strlen(fmt_str);

	/*
	 * Allocate workspace for result as C string
	 */
	result = palloc((fmt_len * DCH_MAX_ITEM_SIZ) + 1);
	*result = '\0';

	if (fmt_len > DCH_CACHE_SIZE)
	{
		/*
		 * Allocate new memory if format picture is bigger than static cache
		 * and do not use cache (call parser always)
		 */
		incache = false;

		format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));

		parse_format(format, fmt_str, DCH_keywords,
					 DCH_suff, DCH_index, DCH_FLAG, NULL);
	}
	else
	{
		/*
		 * Use cache buffers
		 */
		DCHCacheEntry *ent = DCH_cache_fetch(fmt_str, false);

		incache = true;
		format = ent->format;
	}

	/* The real work is here */
	DCH_to_char(format, is_interval, tmtc, result, collid);

	if (!incache)
		pfree(format);

	pfree(fmt_str);

	/* convert C-string result to TEXT format */
	res = cstring_to_text(result);

	pfree(result);
	return res;
}

/****************************************************************************
 *				Public routines
 ***************************************************************************/

/* -------------------
 * TIMESTAMP to_char()
 * -------------------
 */
Datum
timestamp_to_char(PG_FUNCTION_ARGS)
{
	Timestamp	dt = PG_GETARG_TIMESTAMP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1),
			   *res;
	TmToChar	tmtc;
	struct pg_tm tt;
	struct fmt_tm *tm;
	int			thisdate;

	if (VARSIZE_ANY_EXHDR(fmt) <= 0 || TIMESTAMP_NOT_FINITE(dt))
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	if (timestamp2tm(dt, NULL, &tt, &tmtcFsec(&tmtc), NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/* calculate wday and yday, because timestamp2tm doesn't */
	thisdate = date2j(tt.tm_year, tt.tm_mon, tt.tm_mday);
	tt.tm_wday = (thisdate + 1) % 7;
	tt.tm_yday = thisdate - date2j(tt.tm_year, 1, 1) + 1;

	COPY_tm(tm, &tt);

	if (!(res = datetime_to_char_body(&tmtc, fmt, false, PG_GET_COLLATION())))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}

Datum
timestamptz_to_char(PG_FUNCTION_ARGS)
{
	TimestampTz dt = PG_GETARG_TIMESTAMP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1),
			   *res;
	TmToChar	tmtc;
	int			tz;
	struct pg_tm tt;
	struct fmt_tm *tm;
	int			thisdate;

	if (VARSIZE_ANY_EXHDR(fmt) <= 0 || TIMESTAMP_NOT_FINITE(dt))
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	if (timestamp2tm(dt, &tz, &tt, &tmtcFsec(&tmtc), &tmtcTzn(&tmtc), NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/* calculate wday and yday, because timestamp2tm doesn't */
	thisdate = date2j(tt.tm_year, tt.tm_mon, tt.tm_mday);
	tt.tm_wday = (thisdate + 1) % 7;
	tt.tm_yday = thisdate - date2j(tt.tm_year, 1, 1) + 1;

	COPY_tm(tm, &tt);

	if (!(res = datetime_to_char_body(&tmtc, fmt, false, PG_GET_COLLATION())))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}


/* -------------------
 * INTERVAL to_char()
 * -------------------
 */
Datum
interval_to_char(PG_FUNCTION_ARGS)
{
	Interval   *it = PG_GETARG_INTERVAL_P(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1),
			   *res;
	TmToChar	tmtc;
	struct fmt_tm *tm;
	struct pg_itm tt,
			   *itm = &tt;

	if (VARSIZE_ANY_EXHDR(fmt) <= 0 || INTERVAL_NOT_FINITE(it))
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	interval2itm(*it, itm);
	tmtc.fsec = itm->tm_usec;
	tm->tm_sec = itm->tm_sec;
	tm->tm_min = itm->tm_min;
	tm->tm_hour = itm->tm_hour;
	tm->tm_mday = itm->tm_mday;
	tm->tm_mon = itm->tm_mon;
	tm->tm_year = itm->tm_year;

	/* wday is meaningless, yday approximates the total span in days */
	tm->tm_yday = (tm->tm_year * MONTHS_PER_YEAR + tm->tm_mon) * DAYS_PER_MONTH + tm->tm_mday;

	if (!(res = datetime_to_char_body(&tmtc, fmt, true, PG_GET_COLLATION())))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}

/* ---------------------
 * TO_TIMESTAMP()
 *
 * Make Timestamp from date_str which is formatted at argument 'fmt'
 * ( to_timestamp is reverse to_char() )
 * ---------------------
 */
Datum
to_timestamp(PG_FUNCTION_ARGS)
{
	text	   *date_txt = PG_GETARG_TEXT_PP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	Oid			collid = PG_GET_COLLATION();
	Timestamp	result;
	int			tz;
	struct pg_tm tm;
	struct fmt_tz ftz;
	fsec_t		fsec;
	int			fprec;

	do_to_timestamp(date_txt, fmt, collid, false,
					&tm, &fsec, &ftz, &fprec, NULL, NULL);

	/* Use the specified time zone, if any. */
	if (ftz.has_tz)
		tz = ftz.gmtoffset;
	else
		tz = DetermineTimeZoneOffset(&tm, session_timezone);

	if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/* Use the specified fractional precision, if any. */
	if (fprec)
		AdjustTimestampForTypmod(&result, fprec, NULL);

	PG_RETURN_TIMESTAMP(result);
}

/* ----------
 * TO_DATE
 *	Make Date from date_str which is formatted at argument 'fmt'
 * ----------
 */
Datum
to_date(PG_FUNCTION_ARGS)
{
	text	   *date_txt = PG_GETARG_TEXT_PP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	Oid			collid = PG_GET_COLLATION();
	DateADT		result;
	struct pg_tm tm;
	struct fmt_tz ftz;
	fsec_t		fsec;

	do_to_timestamp(date_txt, fmt, collid, false,
					&tm, &fsec, &ftz, NULL, NULL, NULL);

	/* Prevent overflow in Julian-day routines */
	if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: \"%s\"",
						text_to_cstring(date_txt))));

	result = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

	/* Now check for just-out-of-range dates */
	if (!IS_VALID_DATE(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: \"%s\"",
						text_to_cstring(date_txt))));

	PG_RETURN_DATEADT(result);
}

/*
 * Convert the 'date_txt' input to a datetime type using argument 'fmt'
 * as a format string.  The collation 'collid' may be used for case-folding
 * rules in some cases.  'strict' specifies standard parsing mode.
 *
 * The actual data type (returned in 'typid', 'typmod') is determined by
 * the presence of date/time/zone components in the format string.
 *
 * When a timezone component is present, the corresponding offset is
 * returned in '*tz'.
 *
 * If escontext points to an ErrorSaveContext, data errors will be reported
 * by filling that struct; the caller must test SOFT_ERROR_OCCURRED() to see
 * whether an error occurred.  Otherwise, errors are thrown.
 */
Datum
parse_datetime(text *date_txt, text *fmt, Oid collid, bool strict,
			   Oid *typid, int32 *typmod, int *tz,
			   Node *escontext)
{
	struct pg_tm tm;
	struct fmt_tz ftz;
	fsec_t		fsec;
	int			fprec;
	uint32		flags;

	if (!do_to_timestamp(date_txt, fmt, collid, strict,
						 &tm, &fsec, &ftz, &fprec, &flags, escontext))
		return (Datum) 0;

	*typmod = fprec ? fprec : -1;	/* fractional part precision */

	if (flags & DCH_DATED)
	{
		if (flags & DCH_TIMED)
		{
			if (flags & DCH_ZONED)
			{
				TimestampTz result;

				if (ftz.has_tz)
				{
					*tz = ftz.gmtoffset;
				}
				else
				{
					/*
					 * Time zone is present in format string, but not in input
					 * string.  Assuming do_to_timestamp() triggers no error
					 * this should be possible only in non-strict case.
					 */
					Assert(!strict);

					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("missing time zone in input string for type timestamptz")));
				}

				if (tm2timestamp(&tm, fsec, tz, &result) != 0)
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamptz out of range")));

				AdjustTimestampForTypmod(&result, *typmod, escontext);

				*typid = TIMESTAMPTZOID;
				return TimestampTzGetDatum(result);
			}
			else
			{
				Timestamp	result;

				if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));

				AdjustTimestampForTypmod(&result, *typmod, escontext);

				*typid = TIMESTAMPOID;
				return TimestampGetDatum(result);
			}
		}
		else
		{
			if (flags & DCH_ZONED)
			{
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
						 errmsg("datetime format is zoned but not timed")));
			}
			else
			{
				DateADT		result;

				/* Prevent overflow in Julian-day routines */
				if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("date out of range: \"%s\"",
									text_to_cstring(date_txt))));

				result = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) -
					POSTGRES_EPOCH_JDATE;

				/* Now check for just-out-of-range dates */
				if (!IS_VALID_DATE(result))
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("date out of range: \"%s\"",
									text_to_cstring(date_txt))));

				*typid = DATEOID;
				return DateADTGetDatum(result);
			}
		}
	}
	else if (flags & DCH_TIMED)
	{
		if (flags & DCH_ZONED)
		{
			TimeTzADT  *result = palloc(sizeof(TimeTzADT));

			if (ftz.has_tz)
			{
				*tz = ftz.gmtoffset;
			}
			else
			{
				/*
				 * Time zone is present in format string, but not in input
				 * string.  Assuming do_to_timestamp() triggers no error this
				 * should be possible only in non-strict case.
				 */
				Assert(!strict);

				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
						 errmsg("missing time zone in input string for type timetz")));
			}

			if (tm2timetz(&tm, fsec, *tz, result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timetz out of range")));

			AdjustTimeForTypmod(&result->time, *typmod);

			*typid = TIMETZOID;
			return TimeTzADTPGetDatum(result);
		}
		else
		{
			TimeADT		result;

			if (tm2time(&tm, fsec, &result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("time out of range")));

			AdjustTimeForTypmod(&result, *typmod);

			*typid = TIMEOID;
			return TimeADTGetDatum(result);
		}
	}
	else
	{
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("datetime format is not dated and not timed")));
	}
}

/*
 * Parses the datetime format string in 'fmt_str' and returns true if it
 * contains a timezone specifier, false if not.
 */
bool
datetime_format_has_tz(const char *fmt_str)
{
	bool		incache;
	int			fmt_len = strlen(fmt_str);
	int			result;
	FormatNode *format;

	if (fmt_len > DCH_CACHE_SIZE)
	{
		/*
		 * Allocate new memory if format picture is bigger than static cache
		 * and do not use cache (call parser always)
		 */
		incache = false;

		format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));

		parse_format(format, fmt_str, DCH_keywords,
					 DCH_suff, DCH_index, DCH_FLAG, NULL);
	}
	else
	{
		/*
		 * Use cache buffers
		 */
		DCHCacheEntry *ent = DCH_cache_fetch(fmt_str, false);

		incache = true;
		format = ent->format;
	}

	result = DCH_datetime_type(format);

	if (!incache)
		pfree(format);

	return result & DCH_ZONED;
}

/*
 * do_to_timestamp: shared code for to_timestamp and to_date
 *
 * Parse the 'date_txt' according to 'fmt', return results as a struct pg_tm,
 * fractional seconds, struct fmt_tz, and fractional precision.
 *
 * 'collid' identifies the collation to use, if needed.
 * 'std' specifies standard parsing mode.
 *
 * Bit mask of date/time/zone components found in 'fmt' is returned in 'flags',
 * if that is not NULL.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).  Note that currently,
 * soft-error behavior is provided for bad data but not bad format.
 *
 * We parse 'fmt' into a list of FormatNodes, which is then passed to
 * DCH_from_char to populate a TmFromChar with the parsed contents of
 * 'date_txt'.
 *
 * The TmFromChar is then analysed and converted into the final results in
 * struct 'tm', 'fsec', struct 'tz', and 'fprec'.
 */
static bool
do_to_timestamp(text *date_txt, text *fmt, Oid collid, bool std,
				struct pg_tm *tm, fsec_t *fsec, struct fmt_tz *tz,
				int *fprec, uint32 *flags, Node *escontext)
{
	FormatNode *format = NULL;
	TmFromChar	tmfc;
	int			fmt_len;
	char	   *date_str;
	int			fmask;
	bool		incache = false;

	Assert(tm != NULL);
	Assert(fsec != NULL);

	date_str = text_to_cstring(date_txt);

	ZERO_tmfc(&tmfc);
	ZERO_tm(tm);
	*fsec = 0;
	tz->has_tz = false;
	if (fprec)
		*fprec = 0;
	if (flags)
		*flags = 0;
	fmask = 0;					/* bit mask for ValidateDate() */

	fmt_len = VARSIZE_ANY_EXHDR(fmt);

	if (fmt_len)
	{
		char	   *fmt_str;

		fmt_str = text_to_cstring(fmt);

		if (fmt_len > DCH_CACHE_SIZE)
		{
			/*
			 * Allocate new memory if format picture is bigger than static
			 * cache and do not use cache (call parser always)
			 */
			format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));

			parse_format(format, fmt_str, DCH_keywords, DCH_suff, DCH_index,
						 DCH_FLAG | (std ? STD_FLAG : 0), NULL);
		}
		else
		{
			/*
			 * Use cache buffers
			 */
			DCHCacheEntry *ent = DCH_cache_fetch(fmt_str, std);

			incache = true;
			format = ent->format;
		}

#ifdef DEBUG_TO_FROM_CHAR
		/* dump_node(format, fmt_len); */
		/* dump_index(DCH_keywords, DCH_index); */
#endif

		DCH_from_char(format, date_str, &tmfc, collid, std, escontext);
		pfree(fmt_str);
		if (SOFT_ERROR_OCCURRED(escontext))
			goto fail;

		if (flags)
			*flags = DCH_datetime_type(format);

		if (!incache)
		{
			pfree(format);
			format = NULL;
		}
	}

	DEBUG_TMFC(&tmfc);

	/*
	 * Convert to_date/to_timestamp input fields to standard 'tm'
	 */
	if (tmfc.ssss)
	{
		int			x = tmfc.ssss;

		tm->tm_hour = x / SECS_PER_HOUR;
		x %= SECS_PER_HOUR;
		tm->tm_min = x / SECS_PER_MINUTE;
		x %= SECS_PER_MINUTE;
		tm->tm_sec = x;
	}

	if (tmfc.ss)
		tm->tm_sec = tmfc.ss;
	if (tmfc.mi)
		tm->tm_min = tmfc.mi;
	if (tmfc.hh)
		tm->tm_hour = tmfc.hh;

	if (tmfc.clock == CLOCK_12_HOUR)
	{
		if (tm->tm_hour < 1 || tm->tm_hour > HOURS_PER_DAY / 2)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("hour \"%d\" is invalid for the 12-hour clock",
							tm->tm_hour),
					 errhint("Use the 24-hour clock, or give an hour between 1 and 12.")));
			goto fail;
		}

		if (tmfc.pm && tm->tm_hour < HOURS_PER_DAY / 2)
			tm->tm_hour += HOURS_PER_DAY / 2;
		else if (!tmfc.pm && tm->tm_hour == HOURS_PER_DAY / 2)
			tm->tm_hour = 0;
	}

	if (tmfc.year)
	{
		/*
		 * If CC and YY (or Y) are provided, use YY as 2 low-order digits for
		 * the year in the given century.  Keep in mind that the 21st century
		 * AD runs from 2001-2100, not 2000-2099; 6th century BC runs from
		 * 600BC to 501BC.
		 */
		if (tmfc.cc && tmfc.yysz <= 2)
		{
			if (tmfc.bc)
				tmfc.cc = -tmfc.cc;
			tm->tm_year = tmfc.year % 100;
			if (tm->tm_year)
			{
				int			tmp;

				if (tmfc.cc >= 0)
				{
					/* tm->tm_year += (tmfc.cc - 1) * 100; */
					tmp = tmfc.cc - 1;
					if (pg_mul_s32_overflow(tmp, 100, &tmp) ||
						pg_add_s32_overflow(tm->tm_year, tmp, &tm->tm_year))
					{
						DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
										   text_to_cstring(date_txt), "timestamp",
										   escontext);
						goto fail;
					}
				}
				else
				{
					/* tm->tm_year = (tmfc.cc + 1) * 100 - tm->tm_year + 1; */
					tmp = tmfc.cc + 1;
					if (pg_mul_s32_overflow(tmp, 100, &tmp) ||
						pg_sub_s32_overflow(tmp, tm->tm_year, &tmp) ||
						pg_add_s32_overflow(tmp, 1, &tm->tm_year))
					{
						DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
										   text_to_cstring(date_txt), "timestamp",
										   escontext);
						goto fail;
					}
				}
			}
			else
			{
				/* find century year for dates ending in "00" */
				tm->tm_year = tmfc.cc * 100 + ((tmfc.cc >= 0) ? 0 : 1);
			}
		}
		else
		{
			/* If a 4-digit year is provided, we use that and ignore CC. */
			tm->tm_year = tmfc.year;
			if (tmfc.bc)
				tm->tm_year = -tm->tm_year;
			/* correct for our representation of BC years */
			if (tm->tm_year < 0)
				tm->tm_year++;
		}
		fmask |= DTK_M(YEAR);
	}
	else if (tmfc.cc)
	{
		/* use first year of century */
		if (tmfc.bc)
			tmfc.cc = -tmfc.cc;
		if (tmfc.cc >= 0)
		{
			/* +1 because 21st century started in 2001 */
			/* tm->tm_year = (tmfc.cc - 1) * 100 + 1; */
			if (pg_mul_s32_overflow(tmfc.cc - 1, 100, &tm->tm_year) ||
				pg_add_s32_overflow(tm->tm_year, 1, &tm->tm_year))
			{
				DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
								   text_to_cstring(date_txt), "timestamp",
								   escontext);
				goto fail;
			}
		}
		else
		{
			/* +1 because year == 599 is 600 BC */
			/* tm->tm_year = tmfc.cc * 100 + 1; */
			if (pg_mul_s32_overflow(tmfc.cc, 100, &tm->tm_year) ||
				pg_add_s32_overflow(tm->tm_year, 1, &tm->tm_year))
			{
				DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
								   text_to_cstring(date_txt), "timestamp",
								   escontext);
				goto fail;
			}
		}
		fmask |= DTK_M(YEAR);
	}

	if (tmfc.j)
	{
		j2date(tmfc.j, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
		fmask |= DTK_DATE_M;
	}

	if (tmfc.ww)
	{
		if (tmfc.mode == FROM_CHAR_DATE_ISOWEEK)
		{
			/*
			 * If tmfc.d is not set, then the date is left at the beginning of
			 * the ISO week (Monday).
			 */
			if (tmfc.d)
				isoweekdate2date(tmfc.ww, tmfc.d, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
			else
				isoweek2date(tmfc.ww, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
			fmask |= DTK_DATE_M;
		}
		else
		{
			/* tmfc.ddd = (tmfc.ww - 1) * 7 + 1; */
			if (pg_sub_s32_overflow(tmfc.ww, 1, &tmfc.ddd) ||
				pg_mul_s32_overflow(tmfc.ddd, 7, &tmfc.ddd) ||
				pg_add_s32_overflow(tmfc.ddd, 1, &tmfc.ddd))
			{
				DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
								   date_str, "timestamp", escontext);
				goto fail;
			}
		}
	}

	if (tmfc.w)
	{
		/* tmfc.dd = (tmfc.w - 1) * 7 + 1; */
		if (pg_sub_s32_overflow(tmfc.w, 1, &tmfc.dd) ||
			pg_mul_s32_overflow(tmfc.dd, 7, &tmfc.dd) ||
			pg_add_s32_overflow(tmfc.dd, 1, &tmfc.dd))
		{
			DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
							   date_str, "timestamp", escontext);
			goto fail;
		}
	}
	if (tmfc.dd)
	{
		tm->tm_mday = tmfc.dd;
		fmask |= DTK_M(DAY);
	}
	if (tmfc.mm)
	{
		tm->tm_mon = tmfc.mm;
		fmask |= DTK_M(MONTH);
	}

	if (tmfc.ddd && (tm->tm_mon <= 1 || tm->tm_mday <= 1))
	{
		/*
		 * The month and day field have not been set, so we use the
		 * day-of-year field to populate them.  Depending on the date mode,
		 * this field may be interpreted as a Gregorian day-of-year, or an ISO
		 * week date day-of-year.
		 */

		if (!tm->tm_year && !tmfc.bc)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("cannot calculate day of year without year information")));
			goto fail;
		}

		if (tmfc.mode == FROM_CHAR_DATE_ISOWEEK)
		{
			int			j0;		/* zeroth day of the ISO year, in Julian */

			j0 = isoweek2j(tm->tm_year, 1) - 1;

			j2date(j0 + tmfc.ddd, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
			fmask |= DTK_DATE_M;
		}
		else
		{
			const int  *y;
			int			i;

			static const int ysum[2][13] = {
				{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
			{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

			y = ysum[isleap(tm->tm_year)];

			for (i = 1; i <= MONTHS_PER_YEAR; i++)
			{
				if (tmfc.ddd <= y[i])
					break;
			}
			if (tm->tm_mon <= 1)
				tm->tm_mon = i;

			if (tm->tm_mday <= 1)
				tm->tm_mday = tmfc.ddd - y[i - 1];

			fmask |= DTK_M(MONTH) | DTK_M(DAY);
		}
	}

	if (tmfc.ms)
	{
		int			tmp = 0;

		/* *fsec += tmfc.ms * 1000; */
		if (pg_mul_s32_overflow(tmfc.ms, 1000, &tmp) ||
			pg_add_s32_overflow(*fsec, tmp, fsec))
		{
			DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
							   date_str, "timestamp", escontext);
			goto fail;
		}
	}
	if (tmfc.us)
		*fsec += tmfc.us;
	if (fprec)
		*fprec = tmfc.ff;		/* fractional precision, if specified */

	/* Range-check date fields according to bit mask computed above */
	if (fmask != 0)
	{
		/* We already dealt with AD/BC, so pass isjulian = true */
		int			dterr = ValidateDate(fmask, true, false, false, tm);

		if (dterr != 0)
		{
			/*
			 * Force the error to be DTERR_FIELD_OVERFLOW even if ValidateDate
			 * said DTERR_MD_FIELD_OVERFLOW, because we don't want to print an
			 * irrelevant hint about datestyle.
			 */
			DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
							   date_str, "timestamp", escontext);
			goto fail;
		}
	}

	/* Range-check time fields too */
	if (tm->tm_hour < 0 || tm->tm_hour >= HOURS_PER_DAY ||
		tm->tm_min < 0 || tm->tm_min >= MINS_PER_HOUR ||
		tm->tm_sec < 0 || tm->tm_sec >= SECS_PER_MINUTE ||
		*fsec < INT64CONST(0) || *fsec >= USECS_PER_SEC)
	{
		DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
						   date_str, "timestamp", escontext);
		goto fail;
	}

	/*
	 * If timezone info was present, reduce it to a GMT offset.  (We cannot do
	 * this until we've filled all of the tm struct, since the zone's offset
	 * might be time-varying.)
	 */
	if (tmfc.tzsign)
	{
		/* TZH and/or TZM fields */
		if (tmfc.tzh < 0 || tmfc.tzh > MAX_TZDISP_HOUR ||
			tmfc.tzm < 0 || tmfc.tzm >= MINS_PER_HOUR)
		{
			DateTimeParseError(DTERR_TZDISP_OVERFLOW, NULL,
							   date_str, "timestamp", escontext);
			goto fail;
		}

		tz->has_tz = true;
		tz->gmtoffset = (tmfc.tzh * MINS_PER_HOUR + tmfc.tzm) * SECS_PER_MINUTE;
		/* note we are flipping the sign convention here */
		if (tmfc.tzsign > 0)
			tz->gmtoffset = -tz->gmtoffset;
	}
	else if (tmfc.has_tz)
	{
		/* TZ field */
		tz->has_tz = true;
		if (tmfc.tzp == NULL)
		{
			/* fixed-offset abbreviation; flip the sign convention */
			tz->gmtoffset = -tmfc.gmtoffset;
		}
		else
		{
			/* dynamic-offset abbreviation, resolve using specified time */
			tz->gmtoffset = DetermineTimeZoneAbbrevOffset(tm, tmfc.abbrev,
														  tmfc.tzp);
		}
	}

	DEBUG_TM(tm);

	if (format && !incache)
		pfree(format);
	pfree(date_str);

	return true;

fail:
	if (format && !incache)
		pfree(format);
	pfree(date_str);

	return false;
}


/**********************************************************************
 *	the NUMBER version part
 *********************************************************************/


static char *
fill_str(char *str, int c, int max)
{
	memset(str, c, max);
	*(str + max) = '\0';
	return str;
}

#define zeroize_NUM(_n) \
do { \
	(_n)->flag		= 0;	\
	(_n)->lsign		= 0;	\
	(_n)->pre		= 0;	\
	(_n)->post		= 0;	\
	(_n)->pre_lsign_num = 0;	\
	(_n)->need_locale	= 0;	\
	(_n)->multi		= 0;	\
	(_n)->zero_start	= 0;	\
	(_n)->zero_end		= 0;	\
} while(0)

/* This works the same as DCH_prevent_counter_overflow */
static inline void
NUM_prevent_counter_overflow(void)
{
	if (NUMCounter >= (INT_MAX - 1))
	{
		for (int i = 0; i < n_NUMCache; i++)
			NUMCache[i]->age >>= 1;
		NUMCounter >>= 1;
	}
}

/* select a NUMCacheEntry to hold the given format picture */
static NUMCacheEntry *
NUM_cache_getnew(const char *str)
{
	NUMCacheEntry *ent;

	/* Ensure we can advance NUMCounter below */
	NUM_prevent_counter_overflow();

	/*
	 * If cache is full, remove oldest entry (or recycle first not-valid one)
	 */
	if (n_NUMCache >= NUM_CACHE_ENTRIES)
	{
		NUMCacheEntry *old = NUMCache[0];

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Cache is full (%d)", n_NUMCache);
#endif
		if (old->valid)
		{
			for (int i = 1; i < NUM_CACHE_ENTRIES; i++)
			{
				ent = NUMCache[i];
				if (!ent->valid)
				{
					old = ent;
					break;
				}
				if (ent->age < old->age)
					old = ent;
			}
		}
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "OLD: \"%s\" AGE: %d", old->str, old->age);
#endif
		old->valid = false;
		strlcpy(old->str, str, NUM_CACHE_SIZE + 1);
		old->age = (++NUMCounter);
		/* caller is expected to fill format and Num, then set valid */
		return old;
	}
	else
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "NEW (%d)", n_NUMCache);
#endif
		Assert(NUMCache[n_NUMCache] == NULL);
		NUMCache[n_NUMCache] = ent = (NUMCacheEntry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(NUMCacheEntry));
		ent->valid = false;
		strlcpy(ent->str, str, NUM_CACHE_SIZE + 1);
		ent->age = (++NUMCounter);
		/* caller is expected to fill format and Num, then set valid */
		++n_NUMCache;
		return ent;
	}
}

/* look for an existing NUMCacheEntry matching the given format picture */
static NUMCacheEntry *
NUM_cache_search(const char *str)
{
	/* Ensure we can advance NUMCounter below */
	NUM_prevent_counter_overflow();

	for (int i = 0; i < n_NUMCache; i++)
	{
		NUMCacheEntry *ent = NUMCache[i];

		if (ent->valid && strcmp(ent->str, str) == 0)
		{
			ent->age = (++NUMCounter);
			return ent;
		}
	}

	return NULL;
}

/* Find or create a NUMCacheEntry for the given format picture */
static NUMCacheEntry *
NUM_cache_fetch(const char *str)
{
	NUMCacheEntry *ent;

	if ((ent = NUM_cache_search(str)) == NULL)
	{
		/*
		 * Not in the cache, must run parser and save a new format-picture to
		 * the cache.  Do not mark the cache entry valid until parsing
		 * succeeds.
		 */
		ent = NUM_cache_getnew(str);

		zeroize_NUM(&ent->Num);

		parse_format(ent->format, str, NUM_keywords,
					 NULL, NUM_index, NUM_FLAG, &ent->Num);

		ent->valid = true;
	}
	return ent;
}

/* ----------
 * Cache routine for NUM to_char version
 * ----------
 */
static FormatNode *
NUM_cache(int len, NUMDesc *Num, text *pars_str, bool *shouldFree)
{
	FormatNode *format = NULL;
	char	   *str;

	str = text_to_cstring(pars_str);

	if (len > NUM_CACHE_SIZE)
	{
		/*
		 * Allocate new memory if format picture is bigger than static cache
		 * and do not use cache (call parser always)
		 */
		format = (FormatNode *) palloc((len + 1) * sizeof(FormatNode));

		*shouldFree = true;

		zeroize_NUM(Num);

		parse_format(format, str, NUM_keywords,
					 NULL, NUM_index, NUM_FLAG, Num);
	}
	else
	{
		/*
		 * Use cache buffers
		 */
		NUMCacheEntry *ent = NUM_cache_fetch(str);

		*shouldFree = false;

		format = ent->format;

		/*
		 * Copy cache to used struct
		 */
		Num->flag = ent->Num.flag;
		Num->lsign = ent->Num.lsign;
		Num->pre = ent->Num.pre;
		Num->post = ent->Num.post;
		Num->pre_lsign_num = ent->Num.pre_lsign_num;
		Num->need_locale = ent->Num.need_locale;
		Num->multi = ent->Num.multi;
		Num->zero_start = ent->Num.zero_start;
		Num->zero_end = ent->Num.zero_end;
	}

#ifdef DEBUG_TO_FROM_CHAR
	/* dump_node(format, len); */
	dump_index(NUM_keywords, NUM_index);
#endif

	pfree(str);
	return format;
}


/*
 * Convert integer to Roman numerals
 * Result is upper-case and not blank-padded (NUM_processor converts as needed)
 * If input is out-of-range, produce '###############'
 */
static char *
int_to_roman(int number)
{
	int			len,
				num;
	char	   *p,
			   *result,
				numstr[12];

	result = (char *) palloc(16);
	*result = '\0';

	/*
	 * This range limit is the same as in Oracle(TM).  The difficulty with
	 * handling 4000 or more is that we'd need to use more than 3 "M"'s, and
	 * more than 3 of the same digit isn't considered a valid Roman string.
	 */
	if (number > 3999 || number < 1)
	{
		fill_str(result, '#', 15);
		return result;
	}

	/* Convert to decimal, then examine each digit */
	len = snprintf(numstr, sizeof(numstr), "%d", number);
	Assert(len > 0 && len <= 4);

	for (p = numstr; *p != '\0'; p++, --len)
	{
		num = *p - ('0' + 1);
		if (num < 0)
			continue;			/* ignore zeroes */
		/* switch on current column position */
		switch (len)
		{
			case 4:
				while (num-- >= 0)
					strcat(result, "M");
				break;
			case 3:
				strcat(result, rm100[num]);
				break;
			case 2:
				strcat(result, rm10[num]);
				break;
			case 1:
				strcat(result, rm1[num]);
				break;
		}
	}
	return result;
}



/* ----------
 * Locale
 * ----------
 */
static void
NUM_prepare_locale(NUMProc *Np)
{
	if (Np->Num->need_locale)
	{
		struct lconv *lconv;

		/*
		 * Get locales
		 */
		lconv = PGLC_localeconv();

		/*
		 * Positive / Negative number sign
		 */
		if (lconv->negative_sign && *lconv->negative_sign)
			Np->L_negative_sign = lconv->negative_sign;
		else
			Np->L_negative_sign = "-";

		if (lconv->positive_sign && *lconv->positive_sign)
			Np->L_positive_sign = lconv->positive_sign;
		else
			Np->L_positive_sign = "+";

		/*
		 * Number decimal point
		 */
		if (lconv->decimal_point && *lconv->decimal_point)
			Np->decimal = lconv->decimal_point;

		else
			Np->decimal = ".";

		if (!IS_LDECIMAL(Np->Num))
			Np->decimal = ".";

		/*
		 * Number thousands separator
		 *
		 * Some locales (e.g. broken glibc pt_BR), have a comma for decimal,
		 * but "" for thousands_sep, so we set the thousands_sep too.
		 * http://archives.postgresql.org/pgsql-hackers/2007-11/msg00772.php
		 */
		if (lconv->thousands_sep && *lconv->thousands_sep)
			Np->L_thousands_sep = lconv->thousands_sep;
		/* Make sure thousands separator doesn't match decimal point symbol. */
		else if (strcmp(Np->decimal, ",") != 0)
			Np->L_thousands_sep = ",";
		else
			Np->L_thousands_sep = ".";

		/*
		 * Currency symbol
		 */
		if (lconv->currency_symbol && *lconv->currency_symbol)
			Np->L_currency_symbol = lconv->currency_symbol;
		else
			Np->L_currency_symbol = " ";
	}
	else
	{
		/*
		 * Default values
		 */
		Np->L_negative_sign = "-";
		Np->L_positive_sign = "+";
		Np->decimal = ".";

		Np->L_thousands_sep = ",";
		Np->L_currency_symbol = " ";
	}
}

/* ----------
 * Return pointer of last relevant number after decimal point
 *	12.0500 --> last relevant is '5'
 *	12.0000 --> last relevant is '.'
 * If there is no decimal point, return NULL (which will result in same
 * behavior as if FM hadn't been specified).
 * ----------
 */
static char *
get_last_relevant_decnum(char *num)
{
	char	   *result,
			   *p = strchr(num, '.');

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "get_last_relevant_decnum()");
#endif

	if (!p)
		return NULL;

	result = p;

	while (*(++p))
	{
		if (*p != '0')
			result = p;
	}

	return result;
}

/*
 * These macros are used in NUM_processor() and its subsidiary routines.
 * OVERLOAD_TEST: true if we've reached end of input string
 * AMOUNT_TEST(s): true if at least s bytes remain in string
 */
#define OVERLOAD_TEST	(Np->inout_p >= Np->inout + input_len)
#define AMOUNT_TEST(s)	(Np->inout_p <= Np->inout + (input_len - (s)))

/* ----------
 * Number extraction for TO_NUMBER()
 * ----------
 */
static void
NUM_numpart_from_char(NUMProc *Np, int id, int input_len)
{
	bool		isread = false;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, " --- scan start --- id=%s",
		 (id == NUM_0 || id == NUM_9) ? "NUM_0/9" : id == NUM_DEC ? "NUM_DEC" : "???");
#endif

	if (OVERLOAD_TEST)
		return;

	if (*Np->inout_p == ' ')
		Np->inout_p++;

	if (OVERLOAD_TEST)
		return;

	/*
	 * read sign before number
	 */
	if (*Np->number == ' ' && (id == NUM_0 || id == NUM_9) &&
		(Np->read_pre + Np->read_post) == 0)
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Try read sign (%c), locale positive: %s, negative: %s",
			 *Np->inout_p, Np->L_positive_sign, Np->L_negative_sign);
#endif

		/*
		 * locale sign
		 */
		if (IS_LSIGN(Np->Num) && Np->Num->lsign == NUM_LSIGN_PRE)
		{
			int			x = 0;

#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read locale pre-sign (%c)", *Np->inout_p);
#endif
			if ((x = strlen(Np->L_negative_sign)) &&
				AMOUNT_TEST(x) &&
				strncmp(Np->inout_p, Np->L_negative_sign, x) == 0)
			{
				Np->inout_p += x;
				*Np->number = '-';
			}
			else if ((x = strlen(Np->L_positive_sign)) &&
					 AMOUNT_TEST(x) &&
					 strncmp(Np->inout_p, Np->L_positive_sign, x) == 0)
			{
				Np->inout_p += x;
				*Np->number = '+';
			}
		}
		else
		{
#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read simple sign (%c)", *Np->inout_p);
#endif

			/*
			 * simple + - < >
			 */
			if (*Np->inout_p == '-' || (IS_BRACKET(Np->Num) &&
										*Np->inout_p == '<'))
			{
				*Np->number = '-';	/* set - */
				Np->inout_p++;
			}
			else if (*Np->inout_p == '+')
			{
				*Np->number = '+';	/* set + */
				Np->inout_p++;
			}
		}
	}

	if (OVERLOAD_TEST)
		return;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "Scan for numbers (%c), current number: '%s'", *Np->inout_p, Np->number);
#endif

	/*
	 * read digit or decimal point
	 */
	if (isdigit((unsigned char) *Np->inout_p))
	{
		if (Np->read_dec && Np->read_post == Np->Num->post)
			return;

		*Np->number_p = *Np->inout_p;
		Np->number_p++;

		if (Np->read_dec)
			Np->read_post++;
		else
			Np->read_pre++;

		isread = true;

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Read digit (%c)", *Np->inout_p);
#endif
	}
	else if (IS_DECIMAL(Np->Num) && Np->read_dec == false)
	{
		/*
		 * We need not test IS_LDECIMAL(Np->Num) explicitly here, because
		 * Np->decimal is always just "." if we don't have a D format token.
		 * So we just unconditionally match to Np->decimal.
		 */
		int			x = strlen(Np->decimal);

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Try read decimal point (%c)",
			 *Np->inout_p);
#endif
		if (x && AMOUNT_TEST(x) && strncmp(Np->inout_p, Np->decimal, x) == 0)
		{
			Np->inout_p += x - 1;
			*Np->number_p = '.';
			Np->number_p++;
			Np->read_dec = true;
			isread = true;
		}
	}

	if (OVERLOAD_TEST)
		return;

	/*
	 * Read sign behind "last" number
	 *
	 * We need sign detection because determine exact position of post-sign is
	 * difficult:
	 *
	 * FM9999.9999999S	   -> 123.001- 9.9S			   -> .5- FM9.999999MI ->
	 * 5.01-
	 */
	if (*Np->number == ' ' && Np->read_pre + Np->read_post > 0)
	{
		/*
		 * locale sign (NUM_S) is always anchored behind a last number, if: -
		 * locale sign expected - last read char was NUM_0/9 or NUM_DEC - and
		 * next char is not digit
		 */
		if (IS_LSIGN(Np->Num) && isread &&
			(Np->inout_p + 1) < Np->inout + input_len &&
			!isdigit((unsigned char) *(Np->inout_p + 1)))
		{
			int			x;
			char	   *tmp = Np->inout_p++;

#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read locale post-sign (%c)", *Np->inout_p);
#endif
			if ((x = strlen(Np->L_negative_sign)) &&
				AMOUNT_TEST(x) &&
				strncmp(Np->inout_p, Np->L_negative_sign, x) == 0)
			{
				Np->inout_p += x - 1;	/* -1 .. NUM_processor() do inout_p++ */
				*Np->number = '-';
			}
			else if ((x = strlen(Np->L_positive_sign)) &&
					 AMOUNT_TEST(x) &&
					 strncmp(Np->inout_p, Np->L_positive_sign, x) == 0)
			{
				Np->inout_p += x - 1;	/* -1 .. NUM_processor() do inout_p++ */
				*Np->number = '+';
			}
			if (*Np->number == ' ')
				/* no sign read */
				Np->inout_p = tmp;
		}

		/*
		 * try read non-locale sign, it's happen only if format is not exact
		 * and we cannot determine sign position of MI/PL/SG, an example:
		 *
		 * FM9.999999MI			   -> 5.01-
		 *
		 * if (.... && IS_LSIGN(Np->Num)==false) prevents read wrong formats
		 * like to_number('1 -', '9S') where sign is not anchored to last
		 * number.
		 */
		else if (isread == false && IS_LSIGN(Np->Num) == false &&
				 (IS_PLUS(Np->Num) || IS_MINUS(Np->Num)))
		{
#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read simple post-sign (%c)", *Np->inout_p);
#endif

			/*
			 * simple + -
			 */
			if (*Np->inout_p == '-' || *Np->inout_p == '+')
				/* NUM_processor() do inout_p++ */
				*Np->number = *Np->inout_p;
		}
	}
}

#define IS_PREDEC_SPACE(_n) \
		(IS_ZERO((_n)->Num)==false && \
		 (_n)->number == (_n)->number_p && \
		 *(_n)->number == '0' && \
				 (_n)->Num->post != 0)

/* ----------
 * Add digit or sign to number-string
 * ----------
 */
static void
NUM_numpart_to_char(NUMProc *Np, int id)
{
	int			end;

	if (IS_ROMAN(Np->Num))
		return;

	/* Note: in this elog() output not set '\0' in 'inout' */

#ifdef DEBUG_TO_FROM_CHAR

	/*
	 * Np->num_curr is number of current item in format-picture, it is not
	 * current position in inout!
	 */
	elog(DEBUG_elog_output,
		 "SIGN_WROTE: %d, CURRENT: %d, NUMBER_P: \"%s\", INOUT: \"%s\"",
		 Np->sign_wrote,
		 Np->num_curr,
		 Np->number_p,
		 Np->inout);
#endif
	Np->num_in = false;

	/*
	 * Write sign if real number will write to output Note: IS_PREDEC_SPACE()
	 * handle "9.9" --> " .1"
	 */
	if (Np->sign_wrote == false &&
		(Np->num_curr >= Np->out_pre_spaces || (IS_ZERO(Np->Num) && Np->Num->zero_start == Np->num_curr)) &&
		(IS_PREDEC_SPACE(Np) == false || (Np->last_relevant && *Np->last_relevant == '.')))
	{
		if (IS_LSIGN(Np->Num))
		{
			if (Np->Num->lsign == NUM_LSIGN_PRE)
			{
				if (Np->sign == '-')
					strcpy(Np->inout_p, Np->L_negative_sign);
				else
					strcpy(Np->inout_p, Np->L_positive_sign);
				Np->inout_p += strlen(Np->inout_p);
				Np->sign_wrote = true;
			}
		}
		else if (IS_BRACKET(Np->Num))
		{
			*Np->inout_p = Np->sign == '+' ? ' ' : '<';
			++Np->inout_p;
			Np->sign_wrote = true;
		}
		else if (Np->sign == '+')
		{
			if (!IS_FILLMODE(Np->Num))
			{
				*Np->inout_p = ' '; /* Write + */
				++Np->inout_p;
			}
			Np->sign_wrote = true;
		}
		else if (Np->sign == '-')
		{						/* Write - */
			*Np->inout_p = '-';
			++Np->inout_p;
			Np->sign_wrote = true;
		}
	}


	/*
	 * digits / FM / Zero / Dec. point
	 */
	if (id == NUM_9 || id == NUM_0 || id == NUM_D || id == NUM_DEC)
	{
		if (Np->num_curr < Np->out_pre_spaces &&
			(Np->Num->zero_start > Np->num_curr || !IS_ZERO(Np->Num)))
		{
			/*
			 * Write blank space
			 */
			if (!IS_FILLMODE(Np->Num))
			{
				*Np->inout_p = ' '; /* Write ' ' */
				++Np->inout_p;
			}
		}
		else if (IS_ZERO(Np->Num) &&
				 Np->num_curr < Np->out_pre_spaces &&
				 Np->Num->zero_start <= Np->num_curr)
		{
			/*
			 * Write ZERO
			 */
			*Np->inout_p = '0'; /* Write '0' */
			++Np->inout_p;
			Np->num_in = true;
		}
		else
		{
			/*
			 * Write Decimal point
			 */
			if (*Np->number_p == '.')
			{
				if (!Np->last_relevant || *Np->last_relevant != '.')
				{
					strcpy(Np->inout_p, Np->decimal);	/* Write DEC/D */
					Np->inout_p += strlen(Np->inout_p);
				}

				/*
				 * Ora 'n' -- FM9.9 --> 'n.'
				 */
				else if (IS_FILLMODE(Np->Num) &&
						 Np->last_relevant && *Np->last_relevant == '.')
				{
					strcpy(Np->inout_p, Np->decimal);	/* Write DEC/D */
					Np->inout_p += strlen(Np->inout_p);
				}
			}
			else
			{
				/*
				 * Write Digits
				 */
				if (Np->last_relevant && Np->number_p > Np->last_relevant &&
					id != NUM_0)
					;

				/*
				 * '0.1' -- 9.9 --> '  .1'
				 */
				else if (IS_PREDEC_SPACE(Np))
				{
					if (!IS_FILLMODE(Np->Num))
					{
						*Np->inout_p = ' ';
						++Np->inout_p;
					}

					/*
					 * '0' -- FM9.9 --> '0.'
					 */
					else if (Np->last_relevant && *Np->last_relevant == '.')
					{
						*Np->inout_p = '0';
						++Np->inout_p;
					}
				}
				else
				{
					*Np->inout_p = *Np->number_p;	/* Write DIGIT */
					++Np->inout_p;
					Np->num_in = true;
				}
			}
			/* do no exceed string length */
			if (*Np->number_p)
				++Np->number_p;
		}

		end = Np->num_count + (Np->out_pre_spaces ? 1 : 0) + (IS_DECIMAL(Np->Num) ? 1 : 0);

		if (Np->last_relevant && Np->last_relevant == Np->number_p)
			end = Np->num_curr;

		if (Np->num_curr + 1 == end)
		{
			if (Np->sign_wrote == true && IS_BRACKET(Np->Num))
			{
				*Np->inout_p = Np->sign == '+' ? ' ' : '>';
				++Np->inout_p;
			}
			else if (IS_LSIGN(Np->Num) && Np->Num->lsign == NUM_LSIGN_POST)
			{
				if (Np->sign == '-')
					strcpy(Np->inout_p, Np->L_negative_sign);
				else
					strcpy(Np->inout_p, Np->L_positive_sign);
				Np->inout_p += strlen(Np->inout_p);
			}
		}
	}

	++Np->num_curr;
}

/*
 * Skip over "n" input characters, but only if they aren't numeric data
 */
static void
NUM_eat_non_data_chars(NUMProc *Np, int n, int input_len)
{
	while (n-- > 0)
	{
		if (OVERLOAD_TEST)
			break;				/* end of input */
		if (strchr("0123456789.,+-", *Np->inout_p) != NULL)
			break;				/* it's a data character */
		Np->inout_p += pg_mblen(Np->inout_p);
	}
}

static char *
NUM_processor(FormatNode *node, NUMDesc *Num, char *inout,
			  char *number, int input_len, int to_char_out_pre_spaces,
			  int sign, bool is_to_char, Oid collid)
{
	FormatNode *n;
	NUMProc		_Np,
			   *Np = &_Np;
	const char *pattern;
	int			pattern_len;

	MemSet(Np, 0, sizeof(NUMProc));

	Np->Num = Num;
	Np->is_to_char = is_to_char;
	Np->number = number;
	Np->inout = inout;
	Np->last_relevant = NULL;
	Np->read_post = 0;
	Np->read_pre = 0;
	Np->read_dec = false;

	if (Np->Num->zero_start)
		--Np->Num->zero_start;

	if (IS_EEEE(Np->Num))
	{
		if (!Np->is_to_char)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"EEEE\" not supported for input")));
		return strcpy(inout, number);
	}

	/*
	 * Roman correction
	 */
	if (IS_ROMAN(Np->Num))
	{
		if (!Np->is_to_char)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"RN\" not supported for input")));

		Np->Num->lsign = Np->Num->pre_lsign_num = Np->Num->post =
			Np->Num->pre = Np->out_pre_spaces = Np->sign = 0;

		if (IS_FILLMODE(Np->Num))
		{
			Np->Num->flag = 0;
			Np->Num->flag |= NUM_F_FILLMODE;
		}
		else
			Np->Num->flag = 0;
		Np->Num->flag |= NUM_F_ROMAN;
	}

	/*
	 * Sign
	 */
	if (is_to_char)
	{
		Np->sign = sign;

		/* MI/PL/SG - write sign itself and not in number */
		if (IS_PLUS(Np->Num) || IS_MINUS(Np->Num))
		{
			if (IS_PLUS(Np->Num) && IS_MINUS(Np->Num) == false)
				Np->sign_wrote = false; /* need sign */
			else
				Np->sign_wrote = true;	/* needn't sign */
		}
		else
		{
			if (Np->sign != '-')
			{
				if (IS_FILLMODE(Np->Num))
					Np->Num->flag &= ~NUM_F_BRACKET;
			}

			if (Np->sign == '+' && IS_FILLMODE(Np->Num) && IS_LSIGN(Np->Num) == false)
				Np->sign_wrote = true;	/* needn't sign */
			else
				Np->sign_wrote = false; /* need sign */

			if (Np->Num->lsign == NUM_LSIGN_PRE && Np->Num->pre == Np->Num->pre_lsign_num)
				Np->Num->lsign = NUM_LSIGN_POST;
		}
	}
	else
		Np->sign = false;

	/*
	 * Count
	 */
	Np->num_count = Np->Num->post + Np->Num->pre - 1;

	if (is_to_char)
	{
		Np->out_pre_spaces = to_char_out_pre_spaces;

		if (IS_FILLMODE(Np->Num) && IS_DECIMAL(Np->Num))
		{
			Np->last_relevant = get_last_relevant_decnum(Np->number);

			/*
			 * If any '0' specifiers are present, make sure we don't strip
			 * those digits.  But don't advance last_relevant beyond the last
			 * character of the Np->number string, which is a hazard if the
			 * number got shortened due to precision limitations.
			 */
			if (Np->last_relevant && Np->Num->zero_end > Np->out_pre_spaces)
			{
				int			last_zero_pos;
				char	   *last_zero;

				/* note that Np->number cannot be zero-length here */
				last_zero_pos = strlen(Np->number) - 1;
				last_zero_pos = Min(last_zero_pos,
									Np->Num->zero_end - Np->out_pre_spaces);
				last_zero = Np->number + last_zero_pos;
				if (Np->last_relevant < last_zero)
					Np->last_relevant = last_zero;
			}
		}

		if (Np->sign_wrote == false && Np->out_pre_spaces == 0)
			++Np->num_count;
	}
	else
	{
		Np->out_pre_spaces = 0;
		*Np->number = ' ';		/* sign space */
		*(Np->number + 1) = '\0';
	}

	Np->num_in = 0;
	Np->num_curr = 0;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output,
		 "\n\tSIGN: '%c'\n\tNUM: '%s'\n\tPRE: %d\n\tPOST: %d\n\tNUM_COUNT: %d\n\tNUM_PRE: %d\n\tSIGN_WROTE: %s\n\tZERO: %s\n\tZERO_START: %d\n\tZERO_END: %d\n\tLAST_RELEVANT: %s\n\tBRACKET: %s\n\tPLUS: %s\n\tMINUS: %s\n\tFILLMODE: %s\n\tROMAN: %s\n\tEEEE: %s",
		 Np->sign,
		 Np->number,
		 Np->Num->pre,
		 Np->Num->post,
		 Np->num_count,
		 Np->out_pre_spaces,
		 Np->sign_wrote ? "Yes" : "No",
		 IS_ZERO(Np->Num) ? "Yes" : "No",
		 Np->Num->zero_start,
		 Np->Num->zero_end,
		 Np->last_relevant ? Np->last_relevant : "<not set>",
		 IS_BRACKET(Np->Num) ? "Yes" : "No",
		 IS_PLUS(Np->Num) ? "Yes" : "No",
		 IS_MINUS(Np->Num) ? "Yes" : "No",
		 IS_FILLMODE(Np->Num) ? "Yes" : "No",
		 IS_ROMAN(Np->Num) ? "Yes" : "No",
		 IS_EEEE(Np->Num) ? "Yes" : "No"
		);
#endif

	/*
	 * Locale
	 */
	NUM_prepare_locale(Np);

	/*
	 * Processor direct cycle
	 */
	if (Np->is_to_char)
		Np->number_p = Np->number;
	else
		Np->number_p = Np->number + 1;	/* first char is space for sign */

	for (n = node, Np->inout_p = Np->inout; n->type != NODE_TYPE_END; n++)
	{
		if (!Np->is_to_char)
		{
			/*
			 * Check at least one byte remains to be scanned.  (In actions
			 * below, must use AMOUNT_TEST if we want to read more bytes than
			 * that.)
			 */
			if (OVERLOAD_TEST)
				break;
		}

		/*
		 * Format pictures actions
		 */
		if (n->type == NODE_TYPE_ACTION)
		{
			/*
			 * Create/read digit/zero/blank/sign/special-case
			 *
			 * 'NUM_S' note: The locale sign is anchored to number and we
			 * read/write it when we work with first or last number
			 * (NUM_0/NUM_9).  This is why NUM_S is missing in switch().
			 *
			 * Notice the "Np->inout_p++" at the bottom of the loop.  This is
			 * why most of the actions advance inout_p one less than you might
			 * expect.  In cases where we don't want that increment to happen,
			 * a switch case ends with "continue" not "break".
			 */
			switch (n->key->id)
			{
				case NUM_9:
				case NUM_0:
				case NUM_DEC:
				case NUM_D:
					if (Np->is_to_char)
					{
						NUM_numpart_to_char(Np, n->key->id);
						continue;	/* for() */
					}
					else
					{
						NUM_numpart_from_char(Np, n->key->id, input_len);
						break;	/* switch() case: */
					}

				case NUM_COMMA:
					if (Np->is_to_char)
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
							else
								*Np->inout_p = ' ';
						}
						else
							*Np->inout_p = ',';
					}
					else
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
						}
						if (*Np->inout_p != ',')
							continue;
					}
					break;

				case NUM_G:
					pattern = Np->L_thousands_sep;
					pattern_len = strlen(pattern);
					if (Np->is_to_char)
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
							else
							{
								/* just in case there are MB chars */
								pattern_len = pg_mbstrlen(pattern);
								memset(Np->inout_p, ' ', pattern_len);
								Np->inout_p += pattern_len - 1;
							}
						}
						else
						{
							strcpy(Np->inout_p, pattern);
							Np->inout_p += pattern_len - 1;
						}
					}
					else
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
						}

						/*
						 * Because L_thousands_sep typically contains data
						 * characters (either '.' or ','), we can't use
						 * NUM_eat_non_data_chars here.  Instead skip only if
						 * the input matches L_thousands_sep.
						 */
						if (AMOUNT_TEST(pattern_len) &&
							strncmp(Np->inout_p, pattern, pattern_len) == 0)
							Np->inout_p += pattern_len - 1;
						else
							continue;
					}
					break;

				case NUM_L:
					pattern = Np->L_currency_symbol;
					if (Np->is_to_char)
					{
						strcpy(Np->inout_p, pattern);
						Np->inout_p += strlen(pattern) - 1;
					}
					else
					{
						NUM_eat_non_data_chars(Np, pg_mbstrlen(pattern), input_len);
						continue;
					}
					break;

				case NUM_RN:
					if (IS_FILLMODE(Np->Num))
					{
						strcpy(Np->inout_p, Np->number_p);
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					else
					{
						sprintf(Np->inout_p, "%15s", Np->number_p);
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					break;

				case NUM_rn:
					if (IS_FILLMODE(Np->Num))
					{
						strcpy(Np->inout_p, asc_tolower_z(Np->number_p));
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					else
					{
						sprintf(Np->inout_p, "%15s", asc_tolower_z(Np->number_p));
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					break;

				case NUM_th:
					if (IS_ROMAN(Np->Num) || *Np->number == '#' ||
						Np->sign == '-' || IS_DECIMAL(Np->Num))
						continue;

					if (Np->is_to_char)
					{
						strcpy(Np->inout_p, get_th(Np->number, TH_LOWER));
						Np->inout_p += 1;
					}
					else
					{
						/* All variants of 'th' occupy 2 characters */
						NUM_eat_non_data_chars(Np, 2, input_len);
						continue;
					}
					break;

				case NUM_TH:
					if (IS_ROMAN(Np->Num) || *Np->number == '#' ||
						Np->sign == '-' || IS_DECIMAL(Np->Num))
						continue;

					if (Np->is_to_char)
					{
						strcpy(Np->inout_p, get_th(Np->number, TH_UPPER));
						Np->inout_p += 1;
					}
					else
					{
						/* All variants of 'TH' occupy 2 characters */
						NUM_eat_non_data_chars(Np, 2, input_len);
						continue;
					}
					break;

				case NUM_MI:
					if (Np->is_to_char)
					{
						if (Np->sign == '-')
							*Np->inout_p = '-';
						else if (IS_FILLMODE(Np->Num))
							continue;
						else
							*Np->inout_p = ' ';
					}
					else
					{
						if (*Np->inout_p == '-')
							*Np->number = '-';
						else
						{
							NUM_eat_non_data_chars(Np, 1, input_len);
							continue;
						}
					}
					break;

				case NUM_PL:
					if (Np->is_to_char)
					{
						if (Np->sign == '+')
							*Np->inout_p = '+';
						else if (IS_FILLMODE(Np->Num))
							continue;
						else
							*Np->inout_p = ' ';
					}
					else
					{
						if (*Np->inout_p == '+')
							*Np->number = '+';
						else
						{
							NUM_eat_non_data_chars(Np, 1, input_len);
							continue;
						}
					}
					break;

				case NUM_SG:
					if (Np->is_to_char)
						*Np->inout_p = Np->sign;
					else
					{
						if (*Np->inout_p == '-')
							*Np->number = '-';
						else if (*Np->inout_p == '+')
							*Np->number = '+';
						else
						{
							NUM_eat_non_data_chars(Np, 1, input_len);
							continue;
						}
					}
					break;

				default:
					continue;
					break;
			}
		}
		else
		{
			/*
			 * In TO_CHAR, non-pattern characters in the format are copied to
			 * the output.  In TO_NUMBER, we skip one input character for each
			 * non-pattern format character, whether or not it matches the
			 * format character.
			 */
			if (Np->is_to_char)
			{
				strcpy(Np->inout_p, n->character);
				Np->inout_p += strlen(Np->inout_p);
			}
			else
			{
				Np->inout_p += pg_mblen(Np->inout_p);
			}
			continue;
		}
		Np->inout_p++;
	}

	if (Np->is_to_char)
	{
		*Np->inout_p = '\0';
		return Np->inout;
	}
	else
	{
		if (*(Np->number_p - 1) == '.')
			*(Np->number_p - 1) = '\0';
		else
			*Np->number_p = '\0';

		/*
		 * Correction - precision of dec. number
		 */
		Np->Num->post = Np->read_post;

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "TO_NUMBER (number): '%s'", Np->number);
#endif
		return Np->number;
	}
}

/* ----------
 * MACRO: Start part of NUM - for all NUM's to_char variants
 *	(sorry, but I hate copy same code - macro is better..)
 * ----------
 */
#define NUM_TOCHAR_prepare \
do { \
	int len = VARSIZE_ANY_EXHDR(fmt); \
	if (len <= 0 || len >= (INT_MAX-VARHDRSZ)/NUM_MAX_ITEM_SIZ)		\
		PG_RETURN_TEXT_P(cstring_to_text("")); \
	result	= (text *) palloc0((len * NUM_MAX_ITEM_SIZ) + 1 + VARHDRSZ);	\
	format	= NUM_cache(len, &Num, fmt, &shouldFree);		\
} while (0)

/* ----------
 * MACRO: Finish part of NUM
 * ----------
 */
#define NUM_TOCHAR_finish \
do { \
	int		len; \
									\
	NUM_processor(format, &Num, VARDATA(result), numstr, 0, out_pre_spaces, sign, true, PG_GET_COLLATION()); \
									\
	if (shouldFree)					\
		pfree(format);				\
									\
	/*								\
	 * Convert null-terminated representation of result to standard text. \
	 * The result is usually much bigger than it needs to be, but there \
	 * seems little point in realloc'ing it smaller. \
	 */								\
	len = strlen(VARDATA(result));	\
	SET_VARSIZE(result, len + VARHDRSZ); \
} while (0)

/* -------------------
 * NUMERIC to_number() (convert string to numeric)
 * -------------------
 */
Datum
numeric_to_number(PG_FUNCTION_ARGS)
{
	text	   *value = PG_GETARG_TEXT_PP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	Datum		result;
	FormatNode *format;
	char	   *numstr;
	bool		shouldFree;
	int			len = 0;
	int			scale,
				precision;

	len = VARSIZE_ANY_EXHDR(fmt);

	if (len <= 0 || len >= INT_MAX / NUM_MAX_ITEM_SIZ)
		PG_RETURN_NULL();

	format = NUM_cache(len, &Num, fmt, &shouldFree);

	numstr = (char *) palloc((len * NUM_MAX_ITEM_SIZ) + 1);

	NUM_processor(format, &Num, VARDATA_ANY(value), numstr,
				  VARSIZE_ANY_EXHDR(value), 0, 0, false, PG_GET_COLLATION());

	scale = Num.post;
	precision = Num.pre + Num.multi + scale;

	if (shouldFree)
		pfree(format);

	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(numstr),
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(((precision << 16) | scale) + VARHDRSZ));

	if (IS_MULTI(&Num))
	{
		Numeric		x;
		Numeric		a = int64_to_numeric(10);
		Numeric		b = int64_to_numeric(-Num.multi);

		x = DatumGetNumeric(DirectFunctionCall2(numeric_power,
												NumericGetDatum(a),
												NumericGetDatum(b)));
		result = DirectFunctionCall2(numeric_mul,
									 result,
									 NumericGetDatum(x));
	}

	pfree(numstr);
	return result;
}

/* ------------------
 * NUMERIC to_char()
 * ------------------
 */
Datum
numeric_to_char(PG_FUNCTION_ARGS)
{
	Numeric		value = PG_GETARG_NUMERIC(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum,
			   *p;

	NUM_TOCHAR_prepare;

	/*
	 * On DateType depend part (numeric)
	 */
	if (IS_ROMAN(&Num))
	{
		int32		intvalue;
		bool		err;

		/* Round and convert to int */
		intvalue = numeric_int4_opt_error(value, &err);
		/* On overflow, just use PG_INT32_MAX; int_to_roman will cope */
		if (err)
			intvalue = PG_INT32_MAX;
		numstr = int_to_roman(intvalue);
	}
	else if (IS_EEEE(&Num))
	{
		orgnum = numeric_out_sci(value, Num.post);

		/*
		 * numeric_out_sci() does not emit a sign for positive numbers.  We
		 * need to add a space in this case so that positive and negative
		 * numbers are aligned.  Also must check for NaN/infinity cases, which
		 * we handle the same way as in float8_to_char.
		 */
		if (strcmp(orgnum, "NaN") == 0 ||
			strcmp(orgnum, "Infinity") == 0 ||
			strcmp(orgnum, "-Infinity") == 0)
		{
			/*
			 * Allow 6 characters for the leading sign, the decimal point,
			 * "e", the exponent's sign and two exponent digits.
			 */
			numstr = (char *) palloc(Num.pre + Num.post + 7);
			fill_str(numstr, '#', Num.pre + Num.post + 6);
			*numstr = ' ';
			*(numstr + Num.pre + 1) = '.';
		}
		else if (*orgnum != '-')
		{
			numstr = (char *) palloc(strlen(orgnum) + 2);
			*numstr = ' ';
			strcpy(numstr + 1, orgnum);
		}
		else
		{
			numstr = orgnum;
		}
	}
	else
	{
		int			numstr_pre_len;
		Numeric		val = value;
		Numeric		x;

		if (IS_MULTI(&Num))
		{
			Numeric		a = int64_to_numeric(10);
			Numeric		b = int64_to_numeric(Num.multi);

			x = DatumGetNumeric(DirectFunctionCall2(numeric_power,
													NumericGetDatum(a),
													NumericGetDatum(b)));
			val = DatumGetNumeric(DirectFunctionCall2(numeric_mul,
													  NumericGetDatum(value),
													  NumericGetDatum(x)));
			Num.pre += Num.multi;
		}

		x = DatumGetNumeric(DirectFunctionCall2(numeric_round,
												NumericGetDatum(val),
												Int32GetDatum(Num.post)));
		orgnum = DatumGetCString(DirectFunctionCall1(numeric_out,
													 NumericGetDatum(x)));

		if (*orgnum == '-')
		{
			sign = '-';
			numstr = orgnum + 1;
		}
		else
		{
			sign = '+';
			numstr = orgnum;
		}

		if ((p = strchr(numstr, '.')))
			numstr_pre_len = p - numstr;
		else
			numstr_pre_len = strlen(numstr);

		/* needs padding? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* overflowed prefix digit format? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* ---------------
 * INT4 to_char()
 * ---------------
 */
Datum
int4_to_char(PG_FUNCTION_ARGS)
{
	int32		value = PG_GETARG_INT32(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum;

	NUM_TOCHAR_prepare;

	/*
	 * On DateType depend part (int32)
	 */
	if (IS_ROMAN(&Num))
		numstr = int_to_roman(value);
	else if (IS_EEEE(&Num))
	{
		/* we can do it easily because float8 won't lose any precision */
		float8		val = (float8) value;

		orgnum = (char *) psprintf("%+.*e", Num.post, val);

		/*
		 * Swap a leading positive sign for a space.
		 */
		if (*orgnum == '+')
			*orgnum = ' ';

		numstr = orgnum;
	}
	else
	{
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			orgnum = DatumGetCString(DirectFunctionCall1(int4out,
														 Int32GetDatum(value * ((int32) pow((double) 10, (double) Num.multi)))));
			Num.pre += Num.multi;
		}
		else
		{
			orgnum = DatumGetCString(DirectFunctionCall1(int4out,
														 Int32GetDatum(value)));
		}

		if (*orgnum == '-')
		{
			sign = '-';
			orgnum++;
		}
		else
			sign = '+';

		numstr_pre_len = strlen(orgnum);

		/* post-decimal digits?  Pad out with zeros. */
		if (Num.post)
		{
			numstr = (char *) palloc(numstr_pre_len + Num.post + 2);
			strcpy(numstr, orgnum);
			*(numstr + numstr_pre_len) = '.';
			memset(numstr + numstr_pre_len + 1, '0', Num.post);
			*(numstr + numstr_pre_len + Num.post + 1) = '\0';
		}
		else
			numstr = orgnum;

		/* needs padding? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* overflowed prefix digit format? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* ---------------
 * INT8 to_char()
 * ---------------
 */
Datum
int8_to_char(PG_FUNCTION_ARGS)
{
	int64		value = PG_GETARG_INT64(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum;

	NUM_TOCHAR_prepare;

	/*
	 * On DateType depend part (int64)
	 */
	if (IS_ROMAN(&Num))
	{
		int32		intvalue;

		/* On overflow, just use PG_INT32_MAX; int_to_roman will cope */
		if (value <= PG_INT32_MAX && value >= PG_INT32_MIN)
			intvalue = (int32) value;
		else
			intvalue = PG_INT32_MAX;
		numstr = int_to_roman(intvalue);
	}
	else if (IS_EEEE(&Num))
	{
		/* to avoid loss of precision, must go via numeric not float8 */
		orgnum = numeric_out_sci(int64_to_numeric(value),
								 Num.post);

		/*
		 * numeric_out_sci() does not emit a sign for positive numbers.  We
		 * need to add a space in this case so that positive and negative
		 * numbers are aligned.  We don't have to worry about NaN/inf here.
		 */
		if (*orgnum != '-')
		{
			numstr = (char *) palloc(strlen(orgnum) + 2);
			*numstr = ' ';
			strcpy(numstr + 1, orgnum);
		}
		else
		{
			numstr = orgnum;
		}
	}
	else
	{
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			double		multi = pow((double) 10, (double) Num.multi);

			value = DatumGetInt64(DirectFunctionCall2(int8mul,
													  Int64GetDatum(value),
													  DirectFunctionCall1(dtoi8,
																		  Float8GetDatum(multi))));
			Num.pre += Num.multi;
		}

		orgnum = DatumGetCString(DirectFunctionCall1(int8out,
													 Int64GetDatum(value)));

		if (*orgnum == '-')
		{
			sign = '-';
			orgnum++;
		}
		else
			sign = '+';

		numstr_pre_len = strlen(orgnum);

		/* post-decimal digits?  Pad out with zeros. */
		if (Num.post)
		{
			numstr = (char *) palloc(numstr_pre_len + Num.post + 2);
			strcpy(numstr, orgnum);
			*(numstr + numstr_pre_len) = '.';
			memset(numstr + numstr_pre_len + 1, '0', Num.post);
			*(numstr + numstr_pre_len + Num.post + 1) = '\0';
		}
		else
			numstr = orgnum;

		/* needs padding? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* overflowed prefix digit format? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* -----------------
 * FLOAT4 to_char()
 * -----------------
 */
Datum
float4_to_char(PG_FUNCTION_ARGS)
{
	float4		value = PG_GETARG_FLOAT4(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *p;

	NUM_TOCHAR_prepare;

	if (IS_ROMAN(&Num))
	{
		int32		intvalue;

		/* See notes in ftoi4() */
		value = rint(value);
		/* On overflow, just use PG_INT32_MAX; int_to_roman will cope */
		if (!isnan(value) && FLOAT4_FITS_IN_INT32(value))
			intvalue = (int32) value;
		else
			intvalue = PG_INT32_MAX;
		numstr = int_to_roman(intvalue);
	}
	else if (IS_EEEE(&Num))
	{
		if (isnan(value) || isinf(value))
		{
			/*
			 * Allow 6 characters for the leading sign, the decimal point,
			 * "e", the exponent's sign and two exponent digits.
			 */
			numstr = (char *) palloc(Num.pre + Num.post + 7);
			fill_str(numstr, '#', Num.pre + Num.post + 6);
			*numstr = ' ';
			*(numstr + Num.pre + 1) = '.';
		}
		else
		{
			numstr = psprintf("%+.*e", Num.post, value);

			/*
			 * Swap a leading positive sign for a space.
			 */
			if (*numstr == '+')
				*numstr = ' ';
		}
	}
	else
	{
		float4		val = value;
		char	   *orgnum;
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			float		multi = pow((double) 10, (double) Num.multi);

			val = value * multi;
			Num.pre += Num.multi;
		}

		orgnum = psprintf("%.0f", fabs(val));
		numstr_pre_len = strlen(orgnum);

		/* adjust post digits to fit max float digits */
		if (numstr_pre_len >= FLT_DIG)
			Num.post = 0;
		else if (numstr_pre_len + Num.post > FLT_DIG)
			Num.post = FLT_DIG - numstr_pre_len;
		orgnum = psprintf("%.*f", Num.post, val);

		if (*orgnum == '-')
		{						/* < 0 */
			sign = '-';
			numstr = orgnum + 1;
		}
		else
		{
			sign = '+';
			numstr = orgnum;
		}

		if ((p = strchr(numstr, '.')))
			numstr_pre_len = p - numstr;
		else
			numstr_pre_len = strlen(numstr);

		/* needs padding? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* overflowed prefix digit format? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* -----------------
 * FLOAT8 to_char()
 * -----------------
 */
Datum
float8_to_char(PG_FUNCTION_ARGS)
{
	float8		value = PG_GETARG_FLOAT8(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *p;

	NUM_TOCHAR_prepare;

	if (IS_ROMAN(&Num))
	{
		int32		intvalue;

		/* See notes in dtoi4() */
		value = rint(value);
		/* On overflow, just use PG_INT32_MAX; int_to_roman will cope */
		if (!isnan(value) && FLOAT8_FITS_IN_INT32(value))
			intvalue = (int32) value;
		else
			intvalue = PG_INT32_MAX;
		numstr = int_to_roman(intvalue);
	}
	else if (IS_EEEE(&Num))
	{
		if (isnan(value) || isinf(value))
		{
			/*
			 * Allow 6 characters for the leading sign, the decimal point,
			 * "e", the exponent's sign and two exponent digits.
			 */
			numstr = (char *) palloc(Num.pre + Num.post + 7);
			fill_str(numstr, '#', Num.pre + Num.post + 6);
			*numstr = ' ';
			*(numstr + Num.pre + 1) = '.';
		}
		else
		{
			numstr = psprintf("%+.*e", Num.post, value);

			/*
			 * Swap a leading positive sign for a space.
			 */
			if (*numstr == '+')
				*numstr = ' ';
		}
	}
	else
	{
		float8		val = value;
		char	   *orgnum;
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			double		multi = pow((double) 10, (double) Num.multi);

			val = value * multi;
			Num.pre += Num.multi;
		}

		orgnum = psprintf("%.0f", fabs(val));
		numstr_pre_len = strlen(orgnum);

		/* adjust post digits to fit max double digits */
		if (numstr_pre_len >= DBL_DIG)
			Num.post = 0;
		else if (numstr_pre_len + Num.post > DBL_DIG)
			Num.post = DBL_DIG - numstr_pre_len;
		orgnum = psprintf("%.*f", Num.post, val);

		if (*orgnum == '-')
		{						/* < 0 */
			sign = '-';
			numstr = orgnum + 1;
		}
		else
		{
			sign = '+';
			numstr = orgnum;
		}

		if ((p = strchr(numstr, '.')))
			numstr_pre_len = p - numstr;
		else
			numstr_pre_len = strlen(numstr);

		/* needs padding? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* overflowed prefix digit format? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}
