/* -----------------------------------------------------------------------
 * formatting.c
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/formatting.c,v 1.116.2.3 2007/06/29 01:51:49 tgl Exp $
 *
 *
 *	 Portions Copyright (c) 1999-2006, PostgreSQL Global Development Group
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
 *	The cache uses a static buffers and is persistent across transactions.
 *	If format-picture is bigger than cache buffer, parser is called always.
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
 *	- add support for abstime
 *	- add support for roman number to standard number conversion
 *	- add support for number spelling
 *	- add support for string to string formatting (we must be better
 *	  than Oracle :-),
 *		to_char('Hello', 'X X X X X') -> 'H e l l o'
 *
 * -----------------------------------------------------------------------
 */

/* ----------
 * UnComment me for DEBUG
 * ----------
 */
/***
#define DEBUG_TO_FROM_CHAR
#define DEBUG_elog_output	DEBUG3
***/

#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <locale.h>

#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/formatting.h"
#include "utils/int8.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"

#define _(x)	gettext((x))

/* ----------
 * Routines type
 * ----------
 */
#define DCH_TYPE		1		/* DATE-TIME version	*/
#define NUM_TYPE		2		/* NUMBER version	*/

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
#define DCH_MAX_ITEM_SIZ		9		/* max julian day		*/
#define NUM_MAX_ITEM_SIZ		8		/* roman number (RN has 15 chars)	*/

/* ----------
 * More is in float.c
 * ----------
 */
#define MAXFLOATWIDTH	60
#define MAXDOUBLEWIDTH	500


/* ----------
 * External (defined in PgSQL datetime.c (timestamp utils))
 * ----------
 */
extern char *months[],			/* month abbreviation	*/
		   *days[];				/* full days		*/

/* ----------
 * Format parser structs
 * ----------
 */
typedef struct
{
	char	   *name;			/* suffix string		*/
	int			len,			/* suffix length		*/
				id,				/* used in node->suffix */
				type;			/* prefix / postfix			*/
} KeySuffix;

typedef struct FormatNode FormatNode;

typedef struct
{
	const char *name;			/* keyword			*/
	int			len;			/* keyword length		*/
	int			(*action) (int arg, char *inout,		/* action for keyword */
								  int suf, bool is_to_char, bool is_interval,
									   FormatNode *node, void *data);
	int			id;				/* keyword id			*/
	bool		isitdigit;		/* is expected output/input digit */
} KeyWord;

struct FormatNode
{
	int			type;			/* node type			*/
	const KeyWord *key;			/* if node type is KEYWORD	*/
	int			character,		/* if node type is CHAR		*/
				suffix;			/* keyword suffix		*/
};

#define NODE_TYPE_END		1
#define NODE_TYPE_ACTION	2
#define NODE_TYPE_CHAR		3

#define SUFFTYPE_PREFIX		1
#define SUFFTYPE_POSTFIX	2


/* ----------
 * Full months
 * ----------
 */
static char *months_full[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December", NULL
};

static char *days_short[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", NULL
};

/* ----------
 * AC / DC
 * ----------
 */
/*
 *	There is no 0 AD.  Years go from 1 BC to 1 AD, so we make it
 *	positive and map year == -1 to year zero, and shift all negative
 *	years up one.  For interval years, we just return the year.
 */
#define ADJUST_YEAR(year, is_interval)	((is_interval) ? (year) : ((year) <= 0 ? -((year) - 1) : (year)))
#define BC_STR_ORIG " BC"

#define A_D_STR		"A.D."
#define a_d_STR		"a.d."
#define AD_STR		"AD"
#define ad_STR		"ad"

#define B_C_STR		"B.C."
#define b_c_STR		"b.c."
#define BC_STR		"BC"
#define bc_STR		"bc"


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


/* ----------
 * Months in roman-numeral
 * (Must be conversely for seq_search (in FROM_CHAR), because
 *	'VIII' must be over 'V')
 * ----------
 */
static char *rm_months_upper[] =
{"XII", "XI", "X", "IX", "VIII", "VII", "VI", "V", "IV", "III", "II", "I", NULL};

static char *rm_months_lower[] =
{"xii", "xi", "x", "ix", "viii", "vii", "vi", "v", "iv", "iii", "ii", "i", NULL};

/* ----------
 * Roman numbers
 * ----------
 */
static char *rm1[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", NULL};
static char *rm10[] = {"X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC", NULL};
static char *rm100[] = {"C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM", NULL};

/* ----------
 * Ordinal postfixes
 * ----------
 */
static char *numTH[] = {"ST", "ND", "RD", "TH", NULL};
static char *numth[] = {"st", "nd", "rd", "th", NULL};

/* ----------
 * Flags & Options:
 * ----------
 */
#define ONE_UPPER	1			/* Name */
#define ALL_UPPER	2			/* NAME */
#define ALL_LOWER	3			/* name */

#define FULL_SIZ	0

#define MAX_MON_LEN 3
#define MAX_DY_LEN	3

#define TH_UPPER	1
#define TH_LOWER	2

/* ----------
 * Flags for DCH version
 * ----------
 */
static bool DCH_global_fx = false;


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

/* ----------
 * Format picture cache
 *	(cache size:
 *		Number part = NUM_CACHE_SIZE * NUM_CACHE_FIELDS
 *		Date-time part	= DCH_CACHE_SIZE * DCH_CACHE_FIELDS
 *	)
 * ----------
 */
#define NUM_CACHE_SIZE		64
#define NUM_CACHE_FIELDS	16
#define DCH_CACHE_SIZE		128
#define DCH_CACHE_FIELDS	16

typedef struct
{
	FormatNode	format[DCH_CACHE_SIZE + 1];
	char		str[DCH_CACHE_SIZE + 1];
	int			age;
} DCHCacheEntry;

typedef struct
{
	FormatNode	format[NUM_CACHE_SIZE + 1];
	char		str[NUM_CACHE_SIZE + 1];
	int			age;
	NUMDesc		Num;
} NUMCacheEntry;

/* global cache for --- date/time part */
static DCHCacheEntry DCHCache[DCH_CACHE_FIELDS + 1];

static int	n_DCHCache = 0;		/* number of entries */
static int	DCHCounter = 0;

/* global cache for --- number part */
static NUMCacheEntry NUMCache[NUM_CACHE_FIELDS + 1];
static NUMCacheEntry *last_NUMCacheEntry;

static int	n_NUMCache = 0;		/* number of entries */
static int	NUMCounter = 0;

#define MAX_INT32	(2147483600)

/* ----------
 * For char->date/time conversion
 * ----------
 */
typedef struct
{
	int			hh,
				am,
				pm,
				mi,
				ss,
				ssss,
				d,
				dd,
				ddd,
				mm,
				ms,
				year,
				bc,
				iw,
				ww,
				w,
				cc,
				q,
				j,
				us,
				yysz;			/* is it YY or YYYY ? */
} TmFromChar;

#define ZERO_tmfc(_X) memset(_X, 0, sizeof(TmFromChar))

/* ----------
 * Debug
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_TMFC(_X) \
		elog(DEBUG_elog_output, "TMFC:\nhh %d\nam %d\npm %d\nmi %d\nss %d\nssss %d\nd %d\ndd %d\nddd %d\nmm %d\nms: %d\nyear %d\nbc %d\niw %d\nww %d\nw %d\ncc %d\nq %d\nj %d\nus: %d\nyysz: %d", \
			(_X)->hh, (_X)->am, (_X)->pm, (_X)->mi, (_X)->ss, \
			(_X)->ssss, (_X)->d, (_X)->dd, (_X)->ddd, (_X)->mm, (_X)->ms, \
			(_X)->year, (_X)->bc, (_X)->iw, (_X)->ww, (_X)->w, \
			(_X)->cc, (_X)->q, (_X)->j, (_X)->us, (_X)->yysz);
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
 * ----------
 */
typedef struct TmToChar
{
	struct pg_tm tm;			/* classic 'tm' struct */
	fsec_t		fsec;			/* fractional seconds */
	char	   *tzn;			/* timezone */
} TmToChar;

#define tmtcTm(_X)	(&(_X)->tm)
#define tmtcTzn(_X) ((_X)->tzn)
#define tmtcFsec(_X)	((_X)->fsec)

#define ZERO_tm(_X) \
do {	\
	(_X)->tm_sec  = (_X)->tm_year = (_X)->tm_min = (_X)->tm_wday = \
	(_X)->tm_hour = (_X)->tm_yday = (_X)->tm_isdst = 0; \
	(_X)->tm_mday = (_X)->tm_mon  = 1; \
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
 *			KeyWords definition & action
 *****************************************************************************/

static int dch_global(int arg, char *inout, int suf, bool is_to_char,
		   bool is_interval, FormatNode *node, void *data);
static int dch_time(int arg, char *inout, int suf, bool is_to_char,
		 bool is_interval, FormatNode *node, void *data);
static int dch_date(int arg, char *inout, int suf, bool is_to_char,
		 bool is_interval, FormatNode *node, void *data);

/* ----------
 * Suffixes:
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

#define S_FM(_s)	(((_s) & DCH_S_FM) ? 1 : 0)
#define S_SP(_s)	(((_s) & DCH_S_SP) ? 1 : 0)
#define S_TM(_s)	(((_s) & DCH_S_TM) ? 1 : 0)

/* ----------
 * Suffixes definition for DATE-TIME TO/FROM CHAR
 * ----------
 */
static KeySuffix DCH_suff[] = {
	{"FM", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"fm", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"TM", 2, DCH_S_TM, SUFFTYPE_PREFIX},
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
 *	2)	take keywords position (enum DCH_MM) from index
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
	DCH_FX,						/* global suffix */
	DCH_HH24,
	DCH_HH12,
	DCH_HH,
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
	DCH_P_M,
	DCH_PM,
	DCH_Q,
	DCH_RM,
	DCH_SSSS,
	DCH_SS,
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
	DCH_fx,
	DCH_hh24,
	DCH_hh12,
	DCH_hh,
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
	DCH_p_m,
	DCH_pm,
	DCH_q,
	DCH_rm,
	DCH_ssss,
	DCH_ss,
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
} DCH_poz;

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
} NUM_poz;

/* ----------
 * KeyWords for DATE-TIME version
 * ----------
 */
static const KeyWord DCH_keywords[] = {
/*	keyword, len, func, type, isitdigit			 is in Index */
	{"A.D.", 4, dch_date, DCH_A_D, FALSE},		/* A */
	{"A.M.", 4, dch_time, DCH_A_M, FALSE},
	{"AD", 2, dch_date, DCH_AD, FALSE},
	{"AM", 2, dch_time, DCH_AM, FALSE},
	{"B.C.", 4, dch_date, DCH_B_C, FALSE},		/* B */
	{"BC", 2, dch_date, DCH_BC, FALSE},
	{"CC", 2, dch_date, DCH_CC, TRUE},	/* C */
	{"DAY", 3, dch_date, DCH_DAY, FALSE},		/* D */
	{"DDD", 3, dch_date, DCH_DDD, TRUE},
	{"DD", 2, dch_date, DCH_DD, TRUE},
	{"DY", 2, dch_date, DCH_DY, FALSE},
	{"Day", 3, dch_date, DCH_Day, FALSE},
	{"Dy", 2, dch_date, DCH_Dy, FALSE},
	{"D", 1, dch_date, DCH_D, TRUE},
	{"FX", 2, dch_global, DCH_FX, FALSE},		/* F */
	{"HH24", 4, dch_time, DCH_HH24, TRUE},		/* H */
	{"HH12", 4, dch_time, DCH_HH12, TRUE},
	{"HH", 2, dch_time, DCH_HH, TRUE},
	{"IW", 2, dch_date, DCH_IW, TRUE},	/* I */
	{"IYYY", 4, dch_date, DCH_IYYY, TRUE},
	{"IYY", 3, dch_date, DCH_IYY, TRUE},
	{"IY", 2, dch_date, DCH_IY, TRUE},
	{"I", 1, dch_date, DCH_I, TRUE},
	{"J", 1, dch_date, DCH_J, TRUE},	/* J */
	{"MI", 2, dch_time, DCH_MI, TRUE},
	{"MM", 2, dch_date, DCH_MM, TRUE},
	{"MONTH", 5, dch_date, DCH_MONTH, FALSE},
	{"MON", 3, dch_date, DCH_MON, FALSE},
	{"MS", 2, dch_time, DCH_MS, TRUE},
	{"Month", 5, dch_date, DCH_Month, FALSE},
	{"Mon", 3, dch_date, DCH_Mon, FALSE},
	{"P.M.", 4, dch_time, DCH_P_M, FALSE},		/* P */
	{"PM", 2, dch_time, DCH_PM, FALSE},
	{"Q", 1, dch_date, DCH_Q, TRUE},	/* Q */
	{"RM", 2, dch_date, DCH_RM, FALSE}, /* R */
	{"SSSS", 4, dch_time, DCH_SSSS, TRUE},		/* S */
	{"SS", 2, dch_time, DCH_SS, TRUE},
	{"TZ", 2, dch_time, DCH_TZ, FALSE}, /* T */
	{"US", 2, dch_time, DCH_US, TRUE},	/* U */
	{"WW", 2, dch_date, DCH_WW, TRUE},	/* W */
	{"W", 1, dch_date, DCH_W, TRUE},
	{"Y,YYY", 5, dch_date, DCH_Y_YYY, TRUE},	/* Y */
	{"YYYY", 4, dch_date, DCH_YYYY, TRUE},
	{"YYY", 3, dch_date, DCH_YYY, TRUE},
	{"YY", 2, dch_date, DCH_YY, TRUE},
	{"Y", 1, dch_date, DCH_Y, TRUE},
	{"a.d.", 4, dch_date, DCH_a_d, FALSE},		/* a */
	{"a.m.", 4, dch_time, DCH_a_m, FALSE},
	{"ad", 2, dch_date, DCH_ad, FALSE},
	{"am", 2, dch_time, DCH_am, FALSE},
	{"b.c.", 4, dch_date, DCH_b_c, FALSE},		/* b */
	{"bc", 2, dch_date, DCH_bc, FALSE},
	{"cc", 2, dch_date, DCH_CC, TRUE},	/* c */
	{"day", 3, dch_date, DCH_day, FALSE},		/* d */
	{"ddd", 3, dch_date, DCH_DDD, TRUE},
	{"dd", 2, dch_date, DCH_DD, TRUE},
	{"dy", 2, dch_date, DCH_dy, FALSE},
	{"d", 1, dch_date, DCH_D, TRUE},
	{"fx", 2, dch_global, DCH_FX, FALSE},		/* f */
	{"hh24", 4, dch_time, DCH_HH24, TRUE},		/* h */
	{"hh12", 4, dch_time, DCH_HH12, TRUE},
	{"hh", 2, dch_time, DCH_HH, TRUE},
	{"iw", 2, dch_date, DCH_IW, TRUE},	/* i */
	{"iyyy", 4, dch_date, DCH_IYYY, TRUE},
	{"iyy", 3, dch_date, DCH_IYY, TRUE},
	{"iy", 2, dch_date, DCH_IY, TRUE},
	{"i", 1, dch_date, DCH_I, TRUE},
	{"j", 1, dch_time, DCH_J, TRUE},	/* j */
	{"mi", 2, dch_time, DCH_MI, TRUE},	/* m */
	{"mm", 2, dch_date, DCH_MM, TRUE},
	{"month", 5, dch_date, DCH_month, FALSE},
	{"mon", 3, dch_date, DCH_mon, FALSE},
	{"ms", 2, dch_time, DCH_MS, TRUE},
	{"p.m.", 4, dch_time, DCH_p_m, FALSE},		/* p */
	{"pm", 2, dch_time, DCH_pm, FALSE},
	{"q", 1, dch_date, DCH_Q, TRUE},	/* q */
	{"rm", 2, dch_date, DCH_rm, FALSE}, /* r */
	{"ssss", 4, dch_time, DCH_SSSS, TRUE},		/* s */
	{"ss", 2, dch_time, DCH_SS, TRUE},
	{"tz", 2, dch_time, DCH_tz, FALSE}, /* t */
	{"us", 2, dch_time, DCH_US, TRUE},	/* u */
	{"ww", 2, dch_date, DCH_WW, TRUE},	/* w */
	{"w", 1, dch_date, DCH_W, TRUE},
	{"y,yyy", 5, dch_date, DCH_Y_YYY, TRUE},	/* y */
	{"yyyy", 4, dch_date, DCH_YYYY, TRUE},
	{"yyy", 3, dch_date, DCH_YYY, TRUE},
	{"yy", 2, dch_date, DCH_YY, TRUE},
	{"y", 1, dch_date, DCH_Y, TRUE},
/* last */
{NULL, 0, NULL, 0}};

/* ----------
 * KeyWords for NUMBER version (now, isitdigit info is not needful here..)
 * ----------
 */
static const KeyWord NUM_keywords[] = {
/*	keyword,	len, func.	type			   is in Index */
	{",", 1, NULL, NUM_COMMA},	/* , */
	{".", 1, NULL, NUM_DEC},	/* . */
	{"0", 1, NULL, NUM_0},		/* 0 */
	{"9", 1, NULL, NUM_9},		/* 9 */
	{"B", 1, NULL, NUM_B},		/* B */
	{"C", 1, NULL, NUM_C},		/* C */
	{"D", 1, NULL, NUM_D},		/* D */
	{"E", 1, NULL, NUM_E},		/* E */
	{"FM", 2, NULL, NUM_FM},	/* F */
	{"G", 1, NULL, NUM_G},		/* G */
	{"L", 1, NULL, NUM_L},		/* L */
	{"MI", 2, NULL, NUM_MI},	/* M */
	{"PL", 2, NULL, NUM_PL},	/* P */
	{"PR", 2, NULL, NUM_PR},
	{"RN", 2, NULL, NUM_RN},	/* R */
	{"SG", 2, NULL, NUM_SG},	/* S */
	{"SP", 2, NULL, NUM_SP},
	{"S", 1, NULL, NUM_S},
	{"TH", 2, NULL, NUM_TH},	/* T */
	{"V", 1, NULL, NUM_V},		/* V */
	{"b", 1, NULL, NUM_B},		/* b */
	{"c", 1, NULL, NUM_C},		/* c */
	{"d", 1, NULL, NUM_D},		/* d */
	{"e", 1, NULL, NUM_E},		/* e */
	{"fm", 2, NULL, NUM_FM},	/* f */
	{"g", 1, NULL, NUM_G},		/* g */
	{"l", 1, NULL, NUM_L},		/* l */
	{"mi", 2, NULL, NUM_MI},	/* m */
	{"pl", 2, NULL, NUM_PL},	/* p */
	{"pr", 2, NULL, NUM_PR},
	{"rn", 2, NULL, NUM_rn},	/* r */
	{"sg", 2, NULL, NUM_SG},	/* s */
	{"sp", 2, NULL, NUM_SP},
	{"s", 1, NULL, NUM_S},
	{"th", 2, NULL, NUM_th},	/* t */
	{"v", 1, NULL, NUM_V},		/* v */

/* last */
{NULL, 0, NULL, 0}};


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
	DCH_FX, -1, DCH_HH24, DCH_IW, DCH_J, -1, -1, DCH_MI, -1, -1,
	DCH_P_M, DCH_Q, DCH_RM, DCH_SSSS, DCH_TZ, DCH_US, -1, DCH_WW, -1, DCH_Y_YYY,
	-1, -1, -1, -1, -1, -1, -1, DCH_a_d, DCH_b_c, DCH_cc,
	DCH_day, -1, DCH_fx, -1, DCH_hh24, DCH_iw, DCH_j, -1, -1, DCH_mi,
	-1, -1, DCH_p_m, DCH_q, DCH_rm, DCH_ssss, DCH_tz, DCH_us, -1, DCH_ww,
	-1, DCH_y_yyy, -1, -1, -1, -1

	/*---- chars over 126 are skiped ----*/
};

/* ----------
 * KeyWords index for NUMBER version
 * ----------
 */
static const int NUM_index[KeyWord_INDEX_SIZE] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
	/*---- first 0..31 chars are skiped ----*/

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

	/*---- chars over 126 are skiped ----*/
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
				num_pre,		/* space before first number	*/

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


/* ----------
 * Functions
 * ----------
 */
static const KeyWord *index_seq_search(char *str, const KeyWord *kw,
				 const int *index);
static KeySuffix *suff_search(char *str, KeySuffix *suf, int type);
static void NUMDesc_prepare(NUMDesc *num, FormatNode *n);
static void parse_format(FormatNode *node, char *str, const KeyWord *kw,
			 KeySuffix *suf, const int *index, int ver, NUMDesc *Num);
static char *DCH_processor(FormatNode *node, char *inout, bool is_to_char,
			  bool is_interval, void *data);

#ifdef DEBUG_TO_FROM_CHAR
static void dump_index(const KeyWord *k, const int *index);
static void dump_node(FormatNode *node, int max);
#endif

static char *get_th(char *num, int type);
static char *str_numth(char *dest, char *num, int type);
static int	strspace_len(char *str);
static int	strdigits_len(char *str);
static char *str_toupper(char *buff);
static char *str_tolower(char *buff);

/* static int is_acdc(char *str, int *len); */
static int	seq_search(char *name, char **array, int type, int max, int *len);
static void do_to_timestamp(text *date_txt, text *fmt,
				struct pg_tm * tm, fsec_t *fsec);
static char *fill_str(char *str, int c, int max);
static FormatNode *NUM_cache(int len, NUMDesc *Num, char *pars_str, bool *shouldFree);
static char *int_to_roman(int number);
static void NUM_prepare_locale(NUMProc *Np);
static char *get_last_relevant_decnum(char *num);
static void NUM_numpart_from_char(NUMProc *Np, int id, int plen);
static void NUM_numpart_to_char(NUMProc *Np, int id);
static char *NUM_processor(FormatNode *node, NUMDesc *Num, char *inout, char *number,
			  int plen, int sign, bool is_to_char);
static DCHCacheEntry *DCH_cache_search(char *str);
static DCHCacheEntry *DCH_cache_getnew(char *str);

static NUMCacheEntry *NUM_cache_search(char *str);
static NUMCacheEntry *NUM_cache_getnew(char *str);
static void NUM_cache_remove(NUMCacheEntry *ent);

static char *localize_month_full(int index);
static char *localize_month(int index);
static char *localize_day_full(int index);
static char *localize_day(int index);

#if defined(HAVE_WCSTOMBS) && defined(HAVE_TOWLOWER)
#define USE_WIDE_UPPER_LOWER
/* externs are in oracle_compat.c */
extern char *wstring_upper(char *str);
extern char *wstring_lower(char *str);

static char *localized_str_toupper(char *buff);
static char *localized_str_tolower(char *buff);
#else
#define localized_str_toupper str_toupper
#define localized_str_tolower str_tolower
#endif

/* ----------
 * Fast sequential search, use index for data selection which
 * go to seq. cycle (it is very fast for unwanted strings)
 * (can't be used binary search in format parsing)
 * ----------
 */
static const KeyWord *
index_seq_search(char *str, const KeyWord *kw, const int *index)
{
	int			poz;

	if (!KeyWord_INDEX_FILTER(*str))
		return NULL;

	if ((poz = *(index + (*str - ' '))) > -1)
	{
		const KeyWord *k = kw + poz;

		do
		{
			if (!strncmp(str, k->name, k->len))
				return k;
			k++;
			if (!k->name)
				return NULL;
		} while (*str == *k->name);
	}
	return NULL;
}

static KeySuffix *
suff_search(char *str, KeySuffix *suf, int type)
{
	KeySuffix  *s;

	for (s = suf; s->name != NULL; s++)
	{
		if (s->type != type)
			continue;

		if (!strncmp(str, s->name, s->len))
			return s;
	}
	return NULL;
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

	switch (n->key->id)
	{
		case NUM_9:
			if (IS_BRACKET(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"9\" must be ahead of \"PR\"")));
			}
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
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"0\" must be ahead of \"PR\"")));
			}
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
			num->need_locale = TRUE;
		case NUM_DEC:
			if (IS_DECIMAL(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("multiple decimal points")));
			}
			if (IS_MULTI(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot use \"V\" and decimal point together")));
			}
			num->flag |= NUM_F_DECIMAL;
			break;

		case NUM_FM:
			num->flag |= NUM_F_FILLMODE;
			break;

		case NUM_S:
			if (IS_LSIGN(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("not unique \"S\"")));
			}
			if (IS_PLUS(num) || IS_MINUS(num) || IS_BRACKET(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\"/\"MI\"/\"SG\"/\"PR\" together")));
			}
			if (!IS_DECIMAL(num))
			{
				num->lsign = NUM_LSIGN_PRE;
				num->pre_lsign_num = num->pre;
				num->need_locale = TRUE;
				num->flag |= NUM_F_LSIGN;
			}
			else if (num->lsign == NUM_LSIGN_NONE)
			{
				num->lsign = NUM_LSIGN_POST;
				num->need_locale = TRUE;
				num->flag |= NUM_F_LSIGN;
			}
			break;

		case NUM_MI:
			if (IS_LSIGN(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"MI\" together")));
			}
			num->flag |= NUM_F_MINUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_MINUS_POST;
			break;

		case NUM_PL:
			if (IS_LSIGN(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\" together")));
			}
			num->flag |= NUM_F_PLUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_PLUS_POST;
			break;

		case NUM_SG:
			if (IS_LSIGN(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"SG\" together")));
			}
			num->flag |= NUM_F_MINUS;
			num->flag |= NUM_F_PLUS;
			break;

		case NUM_PR:
			if (IS_LSIGN(num) || IS_PLUS(num) || IS_MINUS(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"PR\" and \"S\"/\"PL\"/\"MI\"/\"SG\" together")));
			}
			num->flag |= NUM_F_BRACKET;
			break;

		case NUM_rn:
		case NUM_RN:
			num->flag |= NUM_F_ROMAN;
			break;

		case NUM_L:
		case NUM_G:
			num->need_locale = TRUE;
			break;

		case NUM_V:
			if (IS_DECIMAL(num))
			{
				NUM_cache_remove(last_NUMCacheEntry);
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot use \"V\" and decimal point together")));
			}
			num->flag |= NUM_F_MULTI;
			break;

		case NUM_E:
			NUM_cache_remove(last_NUMCacheEntry);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"E\" is not supported")));
	}

	return;
}

/* ----------
 * Format parser, search small keywords and keyword's suffixes, and make
 * format-node tree.
 *
 * for DATE-TIME & NUMBER version
 * ----------
 */
static void
parse_format(FormatNode *node, char *str, const KeyWord *kw,
			 KeySuffix *suf, const int *index, int ver, NUMDesc *Num)
{
	KeySuffix  *s;
	FormatNode *n;
	int			node_set = 0,
				suffix,
				last = 0;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "to_char/number(): run parser");
#endif

	n = node;

	while (*str)
	{
		suffix = 0;

		/*
		 * Prefix
		 */
		if (ver == DCH_TYPE && (s = suff_search(str, suf, SUFFTYPE_PREFIX)) != NULL)
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
			n->suffix = 0;
			node_set = 1;
			if (n->key->len)
				str += n->key->len;

			/*
			 * NUM version: Prepare global NUMDesc struct
			 */
			if (ver == NUM_TYPE)
				NUMDesc_prepare(Num, n);

			/*
			 * Postfix
			 */
			if (ver == DCH_TYPE && *str && (s = suff_search(str, suf, SUFFTYPE_POSTFIX)) != NULL)
			{
				suffix |= s->id;
				if (s->len)
					str += s->len;
			}
		}
		else if (*str)
		{
			/*
			 * Special characters '\' and '"'
			 */
			if (*str == '"' && last != '\\')
			{
				int			x = 0;

				while (*(++str))
				{
					if (*str == '"' && x != '\\')
					{
						str++;
						break;
					}
					else if (*str == '\\' && x != '\\')
					{
						x = '\\';
						continue;
					}
					n->type = NODE_TYPE_CHAR;
					n->character = *str;
					n->key = NULL;
					n->suffix = 0;
					++n;
					x = *str;
				}
				node_set = 0;
				suffix = 0;
				last = 0;
			}
			else if (*str && *str == '\\' && last != '\\' && *(str + 1) == '"')
			{
				last = *str;
				str++;
			}
			else if (*str)
			{
				n->type = NODE_TYPE_CHAR;
				n->character = *str;
				n->key = NULL;
				node_set = 1;
				last = 0;
				str++;
			}
		}

		/* end */
		if (node_set)
		{
			if (n->type == NODE_TYPE_ACTION)
				n->suffix = suffix;
			++n;

			n->suffix = 0;
			node_set = 0;
		}
	}

	n->type = NODE_TYPE_END;
	n->suffix = 0;
	return;
}

/* ----------
 * Call keyword's function for each of (action) node in format-node tree
 * ----------
 */
static char *
DCH_processor(FormatNode *node, char *inout, bool is_to_char,
			  bool is_interval, void *data)
{
	FormatNode *n;
	char	   *s;

	/*
	 * Zeroing global flags
	 */
	DCH_global_fx = false;

	for (n = node, s = inout; n->type != NODE_TYPE_END; n++)
	{
		if (!is_to_char && *s == '\0')

			/*
			 * The input string is shorter than format picture, so it's good
			 * time to break this loop...
			 *
			 * Note: this isn't relevant for TO_CHAR mode, because it uses
			 * 'inout' allocated by format picture length.
			 */
			break;

		if (n->type == NODE_TYPE_ACTION)
		{
			int			len;

			/*
			 * Call node action function
			 */
			len = n->key->action(n->key->id, s, n->suffix, is_to_char,
								 is_interval, n, data);
			if (len > 0)
				s += len - 1;	/* s++ is at the end of the loop */
			else if (len == -1)
				continue;
		}
		else
		{
			/*
			 * Remove to output char from input in TO_CHAR
			 */
			if (is_to_char)
				*s = n->character;
			else
			{
				/*
				 * Skip blank space in FROM_CHAR's input
				 */
				if (isspace((unsigned char) n->character) && !DCH_global_fx)
					while (*s != '\0' && isspace((unsigned char) *(s + 1)))
						++s;
			}
		}
		++s;
	}

	if (is_to_char)
		*s = '\0';
	return inout;
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
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_CHAR '%c'", a, n->character);
		else if (n->type == NODE_TYPE_END)
		{
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_END", a);
			return;
		}
		else
			elog(DEBUG_elog_output, "%d:\t unknown NODE!", a);
	}
}
#endif   /* DEBUG */

/*****************************************************************************
 *			Private utils
 *****************************************************************************/

/* ----------
 * Return ST/ND/RD/TH for simple (1..9) numbers
 * type --> 0 upper, 1 lower
 * ----------
 */
static char *
get_th(char *num, int type)
{
	int			len = strlen(num),
				last,
				seclast;

	last = *(num + (len - 1));
	if (!isdigit((unsigned char) last))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("\"%s\" is not a number", num)));

	/*
	 * All "teens" (<x>1[0-9]) get 'TH/th', while <x>[02-9][123] still get
	 * 'ST/st', 'ND/nd', 'RD/rd', respectively
	 */
	if ((len > 1) && ((seclast = num[len - 2]) == '1'))
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
	return NULL;
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

/* ----------
 * Convert string to upper case. Input string is modified in place.
 * ----------
 */
static char *
str_toupper(char *buff)
{
	char	   *p_buff = buff;

	if (!buff)
		return NULL;

	while (*p_buff)
	{
		*p_buff = pg_toupper((unsigned char) *p_buff);
		++p_buff;
	}

	return buff;
}

/* ----------
 * Convert string to lower case. Input string is modified in place.
 * ----------
 */
static char *
str_tolower(char *buff)
{
	char	   *p_buff = buff;

	if (!buff)
		return NULL;

	while (*p_buff)
	{
		*p_buff = pg_tolower((unsigned char) *p_buff);
		++p_buff;
	}
	return buff;
}


#ifdef USE_WIDE_UPPER_LOWER
/* ----------
 * Convert localized string to upper case.
 * Input string may be modified in place ... or we might make a copy.
 * ----------
 */
static char *
localized_str_toupper(char *buff)
{
	if (!buff)
		return NULL;

	if (pg_database_encoding_max_length() > 1 && !lc_ctype_is_c())
		return wstring_upper(buff);
	else
	{
		char	   *p_buff = buff;

		while (*p_buff)
		{
			*p_buff = pg_toupper((unsigned char) *p_buff);
			++p_buff;
		}
	}

	return buff;
}

/* ----------
 * Convert localized string to lower case.
 * Input string may be modified in place ... or we might make a copy.
 * ----------
 */
static char *
localized_str_tolower(char *buff)
{
	if (!buff)
		return NULL;

	if (pg_database_encoding_max_length() > 1 && !lc_ctype_is_c())
		return wstring_lower(buff);
	else
	{
		char	   *p_buff = buff;

		while (*p_buff)
		{
			*p_buff = pg_tolower((unsigned char) *p_buff);
			++p_buff;
		}
	}

	return buff;
}
#endif /* USE_WIDE_UPPER_LOWER */

/* ----------
 * Sequential search with to upper/lower conversion
 * ----------
 */
static int
seq_search(char *name, char **array, int type, int max, int *len)
{
	char	   *p,
			   *n,
			  **a;
	int			last,
				i;

	*len = 0;

	if (!*name)
		return -1;

	/* set first char */
	if (type == ONE_UPPER || type == ALL_UPPER)
		*name = pg_toupper((unsigned char) *name);
	else if (type == ALL_LOWER)
		*name = pg_tolower((unsigned char) *name);

	for (last = 0, a = array; *a != NULL; a++)
	{
		/* comperate first chars */
		if (*name != **a)
			continue;

		for (i = 1, p = *a + 1, n = name + 1;; n++, p++, i++)
		{
			/* search fragment (max) only */
			if (max && i == max)
			{
				*len = i;
				return a - array;
			}
			/* full size */
			if (*p == '\0')
			{
				*len = i;
				return a - array;
			}
			/* Not found in array 'a' */
			if (*n == '\0')
				break;

			/*
			 * Convert (but convert new chars only)
			 */
			if (i > last)
			{
				if (type == ONE_UPPER || type == ALL_LOWER)
					*n = pg_tolower((unsigned char) *n);
				else if (type == ALL_UPPER)
					*n = pg_toupper((unsigned char) *n);
				last = i;
			}

#ifdef DEBUG_TO_FROM_CHAR

			/*
			 * elog(DEBUG_elog_output, "N: %c, P: %c, A: %s (%s)", *n, *p, *a,
			 * name);
			 */
#endif
			if (*n != *p)
				break;
		}
	}

	return -1;
}


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
#endif   /* DEBUG */

/* ----------
 * Skip TM / th in FROM_CHAR
 * ----------
 */
#define SKIP_THth(_suf)		(S_THth(_suf) ? 2 : 0)


/* ----------
 * Global format option for DCH version
 * ----------
 */
static int
dch_global(int arg, char *inout, int suf, bool is_to_char, bool is_interval,
		   FormatNode *node, void *data)
{
	if (arg == DCH_FX)
		DCH_global_fx = true;
	return -1;
}

/* ----------
 * Return TRUE if next format picture is not digit value
 * ----------
 */
static bool
is_next_separator(FormatNode *n)
{
	if (n->type == NODE_TYPE_END)
		return FALSE;

	if (n->type == NODE_TYPE_ACTION && S_THth(n->suffix))
		return TRUE;

	/*
	 * Next node
	 */
	n++;

	if (n->type == NODE_TYPE_END)
		return FALSE;

	if (n->type == NODE_TYPE_ACTION)
	{
		if (n->key->isitdigit)
			return FALSE;

		return TRUE;
	}
	else if (isdigit((unsigned char) n->character))
		return FALSE;

	return TRUE;				/* some non-digit input (separator) */
}

static int
strspace_len(char *str)
{
	int			len = 0;

	while (*str && isspace((unsigned char) *str))
	{
		str++;
		len++;
	}
	return len;
}

static int
strdigits_len(char *str)
{
	char	   *p = str;
	int			len;

	len = strspace_len(str);
	p += len;

	while (*p && isdigit((unsigned char) *p) && len <= DCH_MAX_ITEM_SIZ)
	{
		len++;
		p++;
	}
	return len;
}

#define AMPM_ERROR	ereport(ERROR, \
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT), \
							 errmsg("invalid AM/PM string")));

/* ----------
 * Master function of TIME for:
 *			  TO_CHAR	- write (inout) formated string
 *			  FROM_CHAR - scan (inout) string by course of FormatNode
 * ----------
 */
static int
dch_time(int arg, char *inout, int suf, bool is_to_char, bool is_interval,
		 FormatNode *node, void *data)
{
	char	   *p_inout = inout;
	struct pg_tm *tm = NULL;
	TmFromChar *tmfc = NULL;
	TmToChar   *tmtc = NULL;

	if (is_to_char)
	{
		tmtc = (TmToChar *) data;
		tm = tmtcTm(tmtc);
	}
	else
		tmfc = (TmFromChar *) data;

	switch (arg)
	{
		case DCH_A_M:
		case DCH_P_M:
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? P_M_STR : A_M_STR);
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, P_M_STR, 4) == 0)
					tmfc->pm = TRUE;
				else if (strncmp(inout, A_M_STR, 4) == 0)
					tmfc->am = TRUE;
				else
					AMPM_ERROR;
				return strlen(P_M_STR);
			}
			break;
		case DCH_AM:
		case DCH_PM:
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? PM_STR : AM_STR);
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, PM_STR, 2) == 0)
					tmfc->pm = TRUE;
				else if (strncmp(inout, AM_STR, 2) == 0)
					tmfc->am = TRUE;
				else
					AMPM_ERROR;
				return strlen(PM_STR);
			}
			break;
		case DCH_a_m:
		case DCH_p_m:
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? p_m_STR : a_m_STR);
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, p_m_STR, 4) == 0)
					tmfc->pm = TRUE;
				else if (strncmp(inout, a_m_STR, 4) == 0)
					tmfc->am = TRUE;
				else
					AMPM_ERROR;
				return strlen(p_m_STR);
			}
			break;
		case DCH_am:
		case DCH_pm:
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? pm_STR : am_STR);
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, pm_STR, 2) == 0)
					tmfc->pm = TRUE;
				else if (strncmp(inout, am_STR, 2) == 0)
					tmfc->am = TRUE;
				else
					AMPM_ERROR;
				return strlen(pm_STR);
			}
			break;
		case DCH_HH:
		case DCH_HH12:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2,
						tm->tm_hour % (HOURS_PER_DAY / 2) == 0 ? 12 :
						tm->tm_hour % (HOURS_PER_DAY / 2));
				if (S_THth(suf))
					str_numth(p_inout, inout, 0);
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->hh);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->hh);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_HH24:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_hour);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->hh);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->hh);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_MI:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_min);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->mi);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->mi);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_SS:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_sec);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->ss);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->ss);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_MS:			/* millisecond */
			if (is_to_char)
			{
#ifdef HAVE_INT64_TIMESTAMP
				sprintf(inout, "%03d", (int) (tmtc->fsec / INT64CONST(1000)));
#else
				sprintf(inout, "%03d", (int) rint(tmtc->fsec * 1000));
#endif
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				int			len,
							x;

				if (is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->ms);
					len = x = strdigits_len(inout);
				}
				else
				{
					sscanf(inout, "%03d", &tmfc->ms);
					x = strdigits_len(inout);
					len = x = x > 3 ? 3 : x;
				}

				/*
				 * 25 is 0.25 and 250 is 0.25 too; 025 is 0.025 and not 0.25
				 */
				tmfc->ms *= x == 1 ? 100 :
					x == 2 ? 10 : 1;

				/*
				 * elog(DEBUG3, "X: %d, MS: %d, LEN: %d", x, tmfc->ms, len);
				 */
				return len + SKIP_THth(suf);
			}
			break;
		case DCH_US:			/* microsecond */
			if (is_to_char)
			{
#ifdef HAVE_INT64_TIMESTAMP
				sprintf(inout, "%06d", (int) tmtc->fsec);
#else
				sprintf(inout, "%06d", (int) rint(tmtc->fsec * 1000000));
#endif
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				int			len,
							x;

				if (is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->us);
					len = x = strdigits_len(inout);
				}
				else
				{
					sscanf(inout, "%06d", &tmfc->us);
					x = strdigits_len(inout);
					len = x = x > 6 ? 6 : x;
				}

				tmfc->us *= x == 1 ? 100000 :
					x == 2 ? 10000 :
					x == 3 ? 1000 :
					x == 4 ? 100 :
					x == 5 ? 10 : 1;

				/*
				 * elog(DEBUG3, "X: %d, US: %d, LEN: %d", x, tmfc->us, len);
				 */
				return len + SKIP_THth(suf);
			}
			break;
		case DCH_SSSS:
			if (is_to_char)
			{
				sprintf(inout, "%d", tm->tm_hour * SECS_PER_HOUR +
						tm->tm_min * SECS_PER_MINUTE +
						tm->tm_sec);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->ssss);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%05d", &tmfc->ssss);
					return strspace_len(inout) + 5 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_tz:
		case DCH_TZ:
			INVALID_FOR_INTERVAL;
			if (is_to_char && tmtcTzn(tmtc))
			{
				if (arg == DCH_TZ)
					strcpy(inout, tmtcTzn(tmtc));
				else
				{
					char	   *p = pstrdup(tmtcTzn(tmtc));

					strcpy(inout, str_tolower(p));
					pfree(p);
				}
				return strlen(inout);
			}
			else if (!is_to_char)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"TZ\"/\"tz\" not supported")));
	}
	return -1;
}

#define CHECK_SEQ_SEARCH(_l, _s) \
do { \
	if ((_l) <= 0) {							\
		ereport(ERROR,	\
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),	\
				 errmsg("invalid value for %s", (_s))));	\
	}								\
} while (0)


/* ----------
 * Master of DATE for:
 *		  TO_CHAR - write (inout) formated string
 *		  FROM_CHAR - scan (inout) string by course of FormatNode
 * ----------
 */
static int
dch_date(int arg, char *inout, int suf, bool is_to_char, bool is_interval,
		 FormatNode *node, void *data)
{
	char		buff[DCH_CACHE_SIZE],
				workbuff[32],
			   *p_inout = inout;
	int			i,
				len;
	struct pg_tm *tm = NULL;
	TmFromChar *tmfc = NULL;
	TmToChar   *tmtc = NULL;

	if (is_to_char)
	{
		tmtc = (TmToChar *) data;
		tm = tmtcTm(tmtc);
	}
	else
		tmfc = (TmFromChar *) data;

	/*
	 * In the FROM-char there is no difference between "January" or "JANUARY"
	 * or "january", all is before search convert to "first-upper". This
	 * convention is used for MONTH, MON, DAY, DY
	 */
	if (!is_to_char)
	{
		if (arg == DCH_MONTH || arg == DCH_Month || arg == DCH_month)
		{
			tmfc->mm = seq_search(inout, months_full, ONE_UPPER, FULL_SIZ, &len) + 1;
			CHECK_SEQ_SEARCH(len, "MONTH/Month/month");
			if (S_FM(suf))
				return len;
			else
				return 9;
		}
		else if (arg == DCH_MON || arg == DCH_Mon || arg == DCH_mon)
		{
			tmfc->mm = seq_search(inout, months, ONE_UPPER, MAX_MON_LEN, &len) + 1;
			CHECK_SEQ_SEARCH(len, "MON/Mon/mon");
			return 3;
		}
		else if (arg == DCH_DAY || arg == DCH_Day || arg == DCH_day)
		{
			tmfc->d = seq_search(inout, days, ONE_UPPER, FULL_SIZ, &len);
			CHECK_SEQ_SEARCH(len, "DAY/Day/day");
			if (S_FM(suf))
				return len;
			else
				return 9;
		}
		else if (arg == DCH_DY || arg == DCH_Dy || arg == DCH_dy)
		{
			tmfc->d = seq_search(inout, days, ONE_UPPER, MAX_DY_LEN, &len);
			CHECK_SEQ_SEARCH(len, "DY/Dy/dy");
			return 3;
		}
	}

	switch (arg)
	{
		case DCH_A_D:
		case DCH_B_C:
			INVALID_FOR_INTERVAL;
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_year <= 0 ? B_C_STR : A_D_STR));
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, B_C_STR, 4) == 0)
					tmfc->bc = TRUE;
				return 4;
			}
			break;
		case DCH_AD:
		case DCH_BC:
			INVALID_FOR_INTERVAL;
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_year <= 0 ? BC_STR : AD_STR));
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, BC_STR, 2) == 0)
					tmfc->bc = TRUE;
				return 2;
			}
			break;
		case DCH_a_d:
		case DCH_b_c:
			INVALID_FOR_INTERVAL;
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_year <= 0 ? b_c_STR : a_d_STR));
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, b_c_STR, 4) == 0)
					tmfc->bc = TRUE;
				return 4;
			}
			break;
		case DCH_ad:
		case DCH_bc:
			INVALID_FOR_INTERVAL;
			if (is_to_char)
			{
				strcpy(inout, (tm->tm_year <= 0 ? bc_STR : ad_STR));
				return strlen(p_inout);
			}
			else
			{
				if (strncmp(inout, bc_STR, 2) == 0)
					tmfc->bc = TRUE;
				return 2;
			}
			break;
		case DCH_MONTH:
			INVALID_FOR_INTERVAL;
			if (!tm->tm_mon)
				return -1;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_month_full(tm->tm_mon - 1));
				sprintf(inout, "%*s", 0, localized_str_toupper(workbuff));
			}
			else
			{
				strcpy(workbuff, months_full[tm->tm_mon - 1]);
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, str_toupper(workbuff));
			}
			return strlen(p_inout);

		case DCH_Month:
			INVALID_FOR_INTERVAL;
			if (!tm->tm_mon)
				return -1;
			if (S_TM(suf))
				sprintf(inout, "%*s", 0, localize_month_full(tm->tm_mon - 1));
			else
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, months_full[tm->tm_mon - 1]);
			return strlen(p_inout);

		case DCH_month:
			INVALID_FOR_INTERVAL;
			if (!tm->tm_mon)
				return -1;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_month_full(tm->tm_mon - 1));
				sprintf(inout, "%*s", 0, localized_str_tolower(workbuff));
			}
			else
			{
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, months_full[tm->tm_mon - 1]);
				*inout = pg_tolower((unsigned char) *inout);
			}
			return strlen(p_inout);

		case DCH_MON:
			INVALID_FOR_INTERVAL;
			if (!tm->tm_mon)
				return -1;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_month(tm->tm_mon - 1));
				strcpy(inout, localized_str_toupper(workbuff));
			}
			else
			{
				strcpy(inout, months[tm->tm_mon - 1]);
				str_toupper(inout);
			}
			return strlen(p_inout);

		case DCH_Mon:
			INVALID_FOR_INTERVAL;
			if (!tm->tm_mon)
				return -1;
			if (S_TM(suf))
				strcpy(inout, localize_month(tm->tm_mon - 1));
			else
				strcpy(inout, months[tm->tm_mon - 1]);
			return strlen(p_inout);

		case DCH_mon:
			INVALID_FOR_INTERVAL;
			if (!tm->tm_mon)
				return -1;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_month(tm->tm_mon - 1));
				strcpy(inout, localized_str_tolower(workbuff));
			}
			else
			{
				strcpy(inout, months[tm->tm_mon - 1]);
				*inout = pg_tolower((unsigned char) *inout);
			}
			return strlen(p_inout);

		case DCH_MM:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_mon);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->mm);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->mm);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_DAY:
			INVALID_FOR_INTERVAL;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_day_full(tm->tm_wday));
				sprintf(inout, "%*s", 0, localized_str_toupper(workbuff));
			}
			else
			{
				strcpy(workbuff, days[tm->tm_wday]);
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, str_toupper(workbuff));
			}
			return strlen(p_inout);

		case DCH_Day:
			INVALID_FOR_INTERVAL;
			if (S_TM(suf))
				sprintf(inout, "%*s", 0, localize_day_full(tm->tm_wday));		    
			else
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, days[tm->tm_wday]);
			return strlen(p_inout);

		case DCH_day:
			INVALID_FOR_INTERVAL;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_day_full(tm->tm_wday));
				sprintf(inout, "%*s", 0, localized_str_tolower(workbuff));				
			}
			else
			{
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, days[tm->tm_wday]);
				*inout = pg_tolower((unsigned char) *inout);
			}
			return strlen(p_inout);

		case DCH_DY:
			INVALID_FOR_INTERVAL;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_day(tm->tm_wday));
				strcpy(inout, localized_str_toupper(workbuff));
			}
			else
			{
				strcpy(inout, days_short[tm->tm_wday]);
				str_toupper(inout);
			}
			
			return strlen(p_inout);

		case DCH_Dy:
			INVALID_FOR_INTERVAL;
			if (S_TM(suf))
				strcpy(inout, localize_day(tm->tm_wday));
			else
				strcpy(inout, days_short[tm->tm_wday]);
			return strlen(p_inout);

		case DCH_dy:
			INVALID_FOR_INTERVAL;
			if (S_TM(suf))
			{
				strcpy(workbuff, localize_day(tm->tm_wday));
				strcpy(inout, localized_str_tolower(workbuff));
			}
			else
			{
				strcpy(inout, days_short[tm->tm_wday]);
				*inout = pg_tolower((unsigned char) *inout);
			}
			return strlen(p_inout);

		case DCH_DDD:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 3, tm->tm_yday);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->ddd);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%03d", &tmfc->ddd);
					return strspace_len(inout) + 3 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_DD:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_mday);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->dd);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->dd);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_D:
			INVALID_FOR_INTERVAL;
			if (is_to_char)
			{
				sprintf(inout, "%d", tm->tm_wday + 1);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				sscanf(inout, "%1d", &tmfc->d);
				return strspace_len(inout) + 1 + SKIP_THth(suf);
			}
			break;
		case DCH_WW:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2,
						(tm->tm_yday - 1) / 7 + 1);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->ww);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->ww);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_IW:
			if (is_to_char)
			{
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2,
						date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->iw);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->iw);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_Q:
			if (is_to_char)
			{
				if (!tm->tm_mon)
					return -1;
				sprintf(inout, "%d", (tm->tm_mon - 1) / 3 + 1);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				sscanf(inout, "%1d", &tmfc->q);
				return strspace_len(inout) + 1 + SKIP_THth(suf);
			}
			break;
		case DCH_CC:
			if (is_to_char)
			{
				if (is_interval)			/* straight calculation */
					i = tm->tm_year / 100;
				else						/* century 21 starts in 2001 */
					i = (tm->tm_year - 1) / 100 + 1;
				if (i <= 99 && i >= -99)
					sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, i);
				else
					sprintf(inout, "%d", i);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->cc);
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%02d", &tmfc->cc);
					return strspace_len(inout) + 2 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_Y_YYY:
			if (is_to_char)
			{
				i = ADJUST_YEAR(tm->tm_year, is_interval) / 1000;
				sprintf(inout, "%d,%03d", i, ADJUST_YEAR(tm->tm_year, is_interval) - (i * 1000));
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				int			cc;

				sscanf(inout, "%d,%03d", &cc, &tmfc->year);
				tmfc->year += (cc * 1000);
				tmfc->yysz = 4;
				return strdigits_len(inout) + 4 + SKIP_THth(suf);
			}
			break;
		case DCH_YYYY:
		case DCH_IYYY:
			if (is_to_char)
			{
				if (tm->tm_year <= 9999 && tm->tm_year >= -9998)
					sprintf(inout, "%0*d",
							S_FM(suf) ? 0 : 4,
							arg == DCH_YYYY ?
							ADJUST_YEAR(tm->tm_year, is_interval) :
							ADJUST_YEAR(date2isoyear(
													 tm->tm_year,
													 tm->tm_mon,
												 tm->tm_mday), is_interval));
				else
					sprintf(inout, "%d",
							arg == DCH_YYYY ?
							ADJUST_YEAR(tm->tm_year, is_interval) :
							ADJUST_YEAR(date2isoyear(
													 tm->tm_year,
													 tm->tm_mon,
												 tm->tm_mday), is_interval));
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				if (S_FM(suf) || is_next_separator(node))
				{
					sscanf(inout, "%d", &tmfc->year);
					tmfc->yysz = 4;
					return strdigits_len(inout) + SKIP_THth(suf);
				}
				else
				{
					sscanf(inout, "%04d", &tmfc->year);
					tmfc->yysz = 4;
					return strspace_len(inout) + 4 + SKIP_THth(suf);
				}
			}
			break;
		case DCH_YYY:
		case DCH_IYY:
			if (is_to_char)
			{
				snprintf(buff, sizeof(buff), "%03d",
						 arg == DCH_YYY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon, tm->tm_mday),
									 is_interval));
				i = strlen(buff);
				strcpy(inout, buff + (i - 3));
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				sscanf(inout, "%03d", &tmfc->year);

				/*
				 * 3-digit year: '100' ... '999' = 1100 ... 1999 '000' ...
				 * '099' = 2000 ... 2099
				 */
				if (tmfc->year >= 100)
					tmfc->year += 1000;
				else
					tmfc->year += 2000;
				tmfc->yysz = 3;
				return strspace_len(inout) + 3 + SKIP_THth(suf);
			}
			break;
		case DCH_YY:
		case DCH_IY:
			if (is_to_char)
			{
				snprintf(buff, sizeof(buff), "%02d",
						 arg == DCH_YY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon, tm->tm_mday),
									 is_interval));
				i = strlen(buff);
				strcpy(inout, buff + (i - 2));
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				sscanf(inout, "%02d", &tmfc->year);

				/*
				 * 2-digit year: '00' ... '69'	= 2000 ... 2069 '70' ... '99'
				 * = 1970 ... 1999
				 */
				if (tmfc->year < 70)
					tmfc->year += 2000;
				else
					tmfc->year += 1900;
				tmfc->yysz = 2;
				return strspace_len(inout) + 2 + SKIP_THth(suf);
			}
			break;
		case DCH_Y:
		case DCH_I:
			if (is_to_char)
			{
				snprintf(buff, sizeof(buff), "%1d",
						 arg == DCH_Y ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon, tm->tm_mday),
									 is_interval));
				i = strlen(buff);
				strcpy(inout, buff + (i - 1));
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				sscanf(inout, "%1d", &tmfc->year);

				/*
				 * 1-digit year: always +2000
				 */
				tmfc->year += 2000;
				tmfc->yysz = 1;
				return strspace_len(inout) + 1 + SKIP_THth(suf);
			}
			break;
		case DCH_RM:
			if (is_to_char)
			{
				if (!tm->tm_mon)
					return -1;
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -4,
						rm_months_upper[12 - tm->tm_mon]);
				return strlen(p_inout);
			}
			else
			{
				tmfc->mm = 12 - seq_search(inout, rm_months_upper, ALL_UPPER, FULL_SIZ, &len);
				CHECK_SEQ_SEARCH(len, "RM");
				if (S_FM(suf))
					return len;
				else
					return 4;
			}
			break;
		case DCH_rm:
			if (is_to_char)
			{
				if (!tm->tm_mon)
					return -1;
				sprintf(inout, "%*s", S_FM(suf) ? 0 : -4,
						rm_months_lower[12 - tm->tm_mon]);
				return strlen(p_inout);
			}
			else
			{
				tmfc->mm = 12 - seq_search(inout, rm_months_lower, ALL_LOWER, FULL_SIZ, &len);
				CHECK_SEQ_SEARCH(len, "rm");
				if (S_FM(suf))
					return len;
				else
					return 4;
			}
			break;
		case DCH_W:
			if (is_to_char)
			{
				sprintf(inout, "%d", (tm->tm_mday - 1) / 7 + 1);
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				sscanf(inout, "%1d", &tmfc->w);
				return strspace_len(inout) + 1 + SKIP_THth(suf);
			}
			break;
		case DCH_J:
			if (is_to_char)
			{
				sprintf(inout, "%d", date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(suf))
					str_numth(p_inout, inout, S_TH_TYPE(suf));
				return strlen(p_inout);
			}
			else
			{
				sscanf(inout, "%d", &tmfc->j);
				return strdigits_len(inout) + SKIP_THth(suf);
			}
			break;
	}
	return -1;
}

static DCHCacheEntry *
DCH_cache_getnew(char *str)
{
	DCHCacheEntry *ent = NULL;

	/* counter overload check  - paranoia? */
	if (DCHCounter + DCH_CACHE_FIELDS >= MAX_INT32)
	{
		DCHCounter = 0;

		for (ent = DCHCache; ent <= (DCHCache + DCH_CACHE_FIELDS); ent++)
			ent->age = (++DCHCounter);
	}

	/*
	 * Cache is full - needs remove any older entry
	 */
	if (n_DCHCache > DCH_CACHE_FIELDS)
	{
		DCHCacheEntry *old = DCHCache + 0;

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "cache is full (%d)", n_DCHCache);
#endif
		for (ent = DCHCache; ent <= (DCHCache + DCH_CACHE_FIELDS); ent++)
		{
			if (ent->age < old->age)
				old = ent;
		}
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "OLD: '%s' AGE: %d", old->str, old->age);
#endif
		StrNCpy(old->str, str, DCH_CACHE_SIZE + 1);
		/* old->format fill parser */
		old->age = (++DCHCounter);
		return old;
	}
	else
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "NEW (%d)", n_DCHCache);
#endif
		ent = DCHCache + n_DCHCache;
		StrNCpy(ent->str, str, DCH_CACHE_SIZE + 1);
		/* ent->format fill parser */
		ent->age = (++DCHCounter);
		++n_DCHCache;
		return ent;
	}

	return NULL;				/* never */
}

static DCHCacheEntry *
DCH_cache_search(char *str)
{
	int			i = 0;
	DCHCacheEntry *ent;

	/* counter overload check  - paranoia? */
	if (DCHCounter + DCH_CACHE_FIELDS >= MAX_INT32)
	{
		DCHCounter = 0;

		for (ent = DCHCache; ent <= (DCHCache + DCH_CACHE_FIELDS); ent++)
			ent->age = (++DCHCounter);
	}

	for (ent = DCHCache; ent <= (DCHCache + DCH_CACHE_FIELDS); ent++)
	{
		if (i == n_DCHCache)
			break;
		if (strcmp(ent->str, str) == 0)
		{
			ent->age = (++DCHCounter);
			return ent;
		}
		i++;
	}

	return NULL;
}

static text *
datetime_to_char_body(TmToChar *tmtc, text *fmt, bool is_interval)
{
	FormatNode *format;
	char	   *fmt_str,
			   *result;
	bool		incache;
	int			fmt_len = VARSIZE(fmt) - VARHDRSZ;
	int			reslen;
	text	   *res;

	/*
	 * Convert fmt to C string
	 */
	fmt_str = (char *) palloc(fmt_len + 1);
	memcpy(fmt_str, VARDATA(fmt), fmt_len);
	*(fmt_str + fmt_len) = '\0';

	/*
	 * Allocate workspace for result as C string
	 */
	result = palloc((fmt_len * DCH_MAX_ITEM_SIZ) + 1);
	*result = '\0';

	/*
	 * Allocate new memory if format picture is bigger than static cache and
	 * not use cache (call parser always)
	 */
	if (fmt_len > DCH_CACHE_SIZE)
	{
		format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));
		incache = FALSE;

		parse_format(format, fmt_str, DCH_keywords,
					 DCH_suff, DCH_index, DCH_TYPE, NULL);

		(format + fmt_len)->type = NODE_TYPE_END;		/* Paranoia? */
	}
	else
	{
		/*
		 * Use cache buffers
		 */
		DCHCacheEntry *ent;

		incache = TRUE;

		if ((ent = DCH_cache_search(fmt_str)) == NULL)
		{
			ent = DCH_cache_getnew(fmt_str);

			/*
			 * Not in the cache, must run parser and save a new format-picture
			 * to the cache.
			 */
			parse_format(ent->format, fmt_str, DCH_keywords,
						 DCH_suff, DCH_index, DCH_TYPE, NULL);

			(ent->format + fmt_len)->type = NODE_TYPE_END;		/* Paranoia? */

#ifdef DEBUG_TO_FROM_CHAR
			/* dump_node(ent->format, fmt_len); */
			/* dump_index(DCH_keywords, DCH_index);  */
#endif
		}
		format = ent->format;
	}

	/* The real work is here */
	DCH_processor(format, result, true, is_interval, (void *) tmtc);

	if (!incache)
		pfree(format);

	pfree(fmt_str);

	/* convert C-string result to TEXT format */
	reslen = strlen(result);
	res = (text *) palloc(reslen + VARHDRSZ);
	memcpy(VARDATA(res), result, reslen);
	VARATT_SIZEP(res) = reslen + VARHDRSZ;

	pfree(result);
	return res;
}

static char *
localize_month_full(int index)
{
	char	   *m = NULL;

	switch (index)
	{
		case 0:
			m = _("January");
			break;
		case 1:
			m = _("February");
			break;
		case 2:
			m = _("March");
			break;
		case 3:
			m = _("April");
			break;
		case 4:
			m = _("May");
			break;
		case 5:
			m = _("June");
			break;
		case 6:
			m = _("July");
			break;
		case 7:
			m = _("August");
			break;
		case 8:
			m = _("September");
			break;
		case 9:
			m = _("October");
			break;
		case 10:
			m = _("November");
			break;
		case 11:
			m = _("December");
			break;
	}

	return m;
}

static char *
localize_month(int index)
{
	char	   *m = NULL;

	switch (index)
	{
		case 0:
			m = _("Jan");
			break;
		case 1:
			m = _("Feb");
			break;
		case 2:
			m = _("Mar");
			break;
		case 3:
			m = _("Apr");
			break;
		case 4:
			/*------ 
			  translator: Translate this as the abbreviation of "May".
			  In English, it is both the full month name and the
			  abbreviation, so this hack is needed to distinguish
			  them.  The translation also needs to start with S:,
			  which will be stripped at run time. */
			m = _("S:May") + 2;
			break;
		case 5:
			m = _("Jun");
			break;
		case 6:
			m = _("Jul");
			break;
		case 7:
			m = _("Aug");
			break;
		case 8:
			m = _("Sep");
			break;
		case 9:
			m = _("Oct");
			break;
		case 10:
			m = _("Nov");
			break;
		case 11:
			m = _("Dec");
			break;
	}

	return m;
}

static char *
localize_day_full(int index)
{
	char	   *d = NULL;

	switch (index)
	{
		case 0:
			d = _("Sunday");
			break;
		case 1:
			d = _("Monday");
			break;
		case 2:
			d = _("Tuesday");
			break;
		case 3:
			d = _("Wednesday");
			break;
		case 4:
			d = _("Thursday");
			break;
		case 5:
			d = _("Friday");
			break;
		case 6:
			d = _("Saturday");
			break;
	}

	return d;
}

static char *
localize_day(int index)
{
	char	   *d = NULL;

	switch (index)
	{
		case 0:
			d = _("Sun");
			break;
		case 1:
			d = _("Mon");
			break;
		case 2:
			d = _("Tue");
			break;
		case 3:
			d = _("Wed");
			break;
		case 4:
			d = _("Thu");
			break;
		case 5:
			d = _("Fri");
			break;
		case 6:
			d = _("Sat");
			break;
	}

	return d;
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
	text	   *fmt = PG_GETARG_TEXT_P(1),
			   *res;
	TmToChar	tmtc;
	struct pg_tm *tm;
	int			thisdate;

	if ((VARSIZE(fmt) - VARHDRSZ) <= 0 || TIMESTAMP_NOT_FINITE(dt))
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	if (timestamp2tm(dt, NULL, tm, &tmtcFsec(&tmtc), NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	thisdate = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
	tm->tm_wday = (thisdate + 1) % 7;
	tm->tm_yday = thisdate - date2j(tm->tm_year, 1, 1) + 1;

	if (!(res = datetime_to_char_body(&tmtc, fmt, false)))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}

Datum
timestamptz_to_char(PG_FUNCTION_ARGS)
{
	TimestampTz dt = PG_GETARG_TIMESTAMP(0);
	text	   *fmt = PG_GETARG_TEXT_P(1),
			   *res;
	TmToChar	tmtc;
	int			tz;
	struct pg_tm *tm;
	int			thisdate;

	if ((VARSIZE(fmt) - VARHDRSZ) <= 0 || TIMESTAMP_NOT_FINITE(dt))
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	if (timestamp2tm(dt, &tz, tm, &tmtcFsec(&tmtc), &tmtcTzn(&tmtc), NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	thisdate = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
	tm->tm_wday = (thisdate + 1) % 7;
	tm->tm_yday = thisdate - date2j(tm->tm_year, 1, 1) + 1;

	if (!(res = datetime_to_char_body(&tmtc, fmt, false)))
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
	text	   *fmt = PG_GETARG_TEXT_P(1),
			   *res;
	TmToChar	tmtc;
	struct pg_tm *tm;

	if ((VARSIZE(fmt) - VARHDRSZ) <= 0)
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	if (interval2tm(*it, tm, &tmtcFsec(&tmtc)) != 0)
		PG_RETURN_NULL();

	/* wday is meaningless, yday approximates the total span in days */
	tm->tm_yday = (tm->tm_year * MONTHS_PER_YEAR + tm->tm_mon) * DAYS_PER_MONTH + tm->tm_mday;

	if (!(res = datetime_to_char_body(&tmtc, fmt, true)))
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
	text	   *date_txt = PG_GETARG_TEXT_P(0);
	text	   *fmt = PG_GETARG_TEXT_P(1);
	Timestamp	result;
	int			tz;
	struct pg_tm tm;
	fsec_t		fsec;

	do_to_timestamp(date_txt, fmt, &tm, &fsec);

	tz = DetermineTimeZoneOffset(&tm, global_timezone);

	if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMP(result);
}

/* ----------
 * TO_DATE
 *	Make Date from date_str which is formated at argument 'fmt'
 * ----------
 */
Datum
to_date(PG_FUNCTION_ARGS)
{
	text	   *date_txt = PG_GETARG_TEXT_P(0);
	text	   *fmt = PG_GETARG_TEXT_P(1);
	DateADT		result;
	struct pg_tm tm;
	fsec_t		fsec;

	do_to_timestamp(date_txt, fmt, &tm, &fsec);

	result = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

	PG_RETURN_DATEADT(result);
}

/*
 * do_to_timestamp: shared code for to_timestamp and to_date
 *
 * Parse the 'date_txt' according to 'fmt', return results as a struct pg_tm
 * and fractional seconds.
 */
static void
do_to_timestamp(text *date_txt, text *fmt,
				struct pg_tm * tm, fsec_t *fsec)
{
	FormatNode *format;
	TmFromChar	tmfc;
	int			fmt_len;

	ZERO_tm(tm);
	*fsec = 0;

	ZERO_tmfc(&tmfc);

	fmt_len = VARSIZE(fmt) - VARHDRSZ;

	if (fmt_len)
	{
		int			date_len;
		char	   *fmt_str;
		char	   *date_str;
		bool		incache;

		fmt_str = (char *) palloc(fmt_len + 1);
		memcpy(fmt_str, VARDATA(fmt), fmt_len);
		*(fmt_str + fmt_len) = '\0';

		/*
		 * Allocate new memory if format picture is bigger than static cache
		 * and not use cache (call parser always)
		 */
		if (fmt_len > DCH_CACHE_SIZE)
		{
			format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));
			incache = FALSE;

			parse_format(format, fmt_str, DCH_keywords,
						 DCH_suff, DCH_index, DCH_TYPE, NULL);

			(format + fmt_len)->type = NODE_TYPE_END;	/* Paranoia? */
		}
		else
		{
			/*
			 * Use cache buffers
			 */
			DCHCacheEntry *ent;

			incache = TRUE;

			if ((ent = DCH_cache_search(fmt_str)) == NULL)
			{
				ent = DCH_cache_getnew(fmt_str);

				/*
				 * Not in the cache, must run parser and save a new
				 * format-picture to the cache.
				 */
				parse_format(ent->format, fmt_str, DCH_keywords,
							 DCH_suff, DCH_index, DCH_TYPE, NULL);

				(ent->format + fmt_len)->type = NODE_TYPE_END;	/* Paranoia? */
#ifdef DEBUG_TO_FROM_CHAR
				/* dump_node(ent->format, fmt_len); */
				/* dump_index(DCH_keywords, DCH_index); */
#endif
			}
			format = ent->format;
		}

		/*
		 * Call action for each node in FormatNode tree
		 */
#ifdef DEBUG_TO_FROM_CHAR
		/* dump_node(format, fmt_len); */
#endif

		/*
		 * Convert date to C string
		 */
		date_len = VARSIZE(date_txt) - VARHDRSZ;
		date_str = (char *) palloc(date_len + 1);
		memcpy(date_str, VARDATA(date_txt), date_len);
		*(date_str + date_len) = '\0';

		DCH_processor(format, date_str, false, false, (void *) &tmfc);

		pfree(date_str);
		pfree(fmt_str);
		if (!incache)
			pfree(format);
	}

	DEBUG_TMFC(&tmfc);

	/*
	 * Convert values that user define for FROM_CHAR (to_date/to_timestamp) to
	 * standard 'tm'
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

	if (tmfc.ww)
		tmfc.ddd = (tmfc.ww - 1) * 7 + 1;

	if (tmfc.w)
		tmfc.dd = (tmfc.w - 1) * 7 + 1;

	if (tmfc.ss)
		tm->tm_sec = tmfc.ss;
	if (tmfc.mi)
		tm->tm_min = tmfc.mi;
	if (tmfc.hh)
		tm->tm_hour = tmfc.hh;

	if (tmfc.pm || tmfc.am)
	{
		if (tm->tm_hour < 1 || tm->tm_hour > 12)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("AM/PM hour must be between 1 and 12")));

		if (tmfc.pm && tm->tm_hour < 12)
			tm->tm_hour += 12;

		else if (tmfc.am && tm->tm_hour == 12)
			tm->tm_hour = 0;
	}

	switch (tmfc.q)
	{
		case 1:
			tm->tm_mday = 1;
			tm->tm_mon = 1;
			break;
		case 2:
			tm->tm_mday = 1;
			tm->tm_mon = 4;
			break;
		case 3:
			tm->tm_mday = 1;
			tm->tm_mon = 7;
			break;
		case 4:
			tm->tm_mday = 1;
			tm->tm_mon = 10;
			break;
	}

	if (tmfc.year)
	{
		/*
		 * If CC and YY (or Y) are provided, use YY as 2 low-order digits
		 * for the year in the given century.  Keep in mind that the 21st
		 * century runs from 2001-2100, not 2000-2099.
		 *
		 * If a 4-digit year is provided, we use that and ignore CC.
		 */
		if (tmfc.cc && tmfc.yysz <= 2)
		{
			tm->tm_year = tmfc.year % 100;
			if (tm->tm_year)
				tm->tm_year += (tmfc.cc - 1) * 100;
			else
				tm->tm_year = tmfc.cc * 100;
		}
		else
			tm->tm_year = tmfc.year;
	}
	else if (tmfc.cc)			/* use first year of century */
		tm->tm_year = (tmfc.cc - 1) * 100 + 1;

	if (tmfc.bc)
	{
		if (tm->tm_year > 0)
			tm->tm_year = -(tm->tm_year - 1);
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("inconsistent use of year %04d and \"BC\"",
							tm->tm_year)));
	}

	if (tmfc.j)
		j2date(tmfc.j, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

	if (tmfc.iw)
		isoweek2date(tmfc.iw, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

	if (tmfc.d)
		tm->tm_wday = tmfc.d;
	if (tmfc.dd)
		tm->tm_mday = tmfc.dd;
	if (tmfc.ddd)
		tm->tm_yday = tmfc.ddd;
	if (tmfc.mm)
		tm->tm_mon = tmfc.mm;

	/*
	 * we don't ignore DDD
	 */
	if (tmfc.ddd && (tm->tm_mon <= 1 || tm->tm_mday <= 1))
	{
		/* count mday and mon from yday */
		int		   *y,
					i;

		int			ysum[2][13] = {
			{31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365, 0},
		{31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366, 0}};

		if (!tm->tm_year)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
			errmsg("cannot calculate day of year without year information")));

		y = ysum[isleap(tm->tm_year)];

		for (i = 0; i <= 11; i++)
		{
			if (tm->tm_yday < y[i])
				break;
		}
		if (tm->tm_mon <= 1)
			tm->tm_mon = i + 1;

		if (tm->tm_mday <= 1)
			tm->tm_mday = i == 0 ? tm->tm_yday :
				tm->tm_yday - y[i - 1];
	}

#ifdef HAVE_INT64_TIMESTAMP
	if (tmfc.ms)
		*fsec += tmfc.ms * 1000;
	if (tmfc.us)
		*fsec += tmfc.us;
#else
	if (tmfc.ms)
		*fsec += (double) tmfc.ms / 1000;
	if (tmfc.us)
		*fsec += (double) tmfc.us / 1000000;
#endif

	DEBUG_TM(tm);
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

static NUMCacheEntry *
NUM_cache_getnew(char *str)
{
	NUMCacheEntry *ent = NULL;

	/* counter overload check  - paranoia? */
	if (NUMCounter + NUM_CACHE_FIELDS >= MAX_INT32)
	{
		NUMCounter = 0;

		for (ent = NUMCache; ent <= (NUMCache + NUM_CACHE_FIELDS); ent++)
			ent->age = (++NUMCounter);
	}

	/*
	 * Cache is full - needs remove any older entry
	 */
	if (n_NUMCache > NUM_CACHE_FIELDS)
	{
		NUMCacheEntry *old = NUMCache + 0;

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Cache is full (%d)", n_NUMCache);
#endif

		for (ent = NUMCache; ent <= (NUMCache + NUM_CACHE_FIELDS); ent++)
		{
			/*
			 * entry removed via NUM_cache_remove() can be used here
			 */
			if (*ent->str == '\0')
			{
				old = ent;
				break;
			}
			if (ent->age < old->age)
				old = ent;
		}
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "OLD: \"%s\" AGE: %d", old->str, old->age);
#endif
		StrNCpy(old->str, str, NUM_CACHE_SIZE + 1);
		/* old->format fill parser */
		old->age = (++NUMCounter);

		ent = old;
	}
	else
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "NEW (%d)", n_NUMCache);
#endif
		ent = NUMCache + n_NUMCache;
		StrNCpy(ent->str, str, NUM_CACHE_SIZE + 1);
		/* ent->format fill parser */
		ent->age = (++NUMCounter);
		++n_NUMCache;
	}

	zeroize_NUM(&ent->Num);

	last_NUMCacheEntry = ent;
	return ent;					/* never */
}

static NUMCacheEntry *
NUM_cache_search(char *str)
{
	int			i = 0;
	NUMCacheEntry *ent;

	/* counter overload check - paranoia? */
	if (NUMCounter + NUM_CACHE_FIELDS >= MAX_INT32)
	{
		NUMCounter = 0;

		for (ent = NUMCache; ent <= (NUMCache + NUM_CACHE_FIELDS); ent++)
			ent->age = (++NUMCounter);
	}

	for (ent = NUMCache; ent <= (NUMCache + NUM_CACHE_FIELDS); ent++)
	{
		if (i == n_NUMCache)
			break;
		if (strcmp(ent->str, str) == 0)
		{
			ent->age = (++NUMCounter);
			last_NUMCacheEntry = ent;
			return ent;
		}
		i++;
	}

	return NULL;
}

static void
NUM_cache_remove(NUMCacheEntry *ent)
{
#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "REMOVING ENTRY (%s)", ent->str);
#endif
	*ent->str = '\0';
	ent->age = 0;
}

/* ----------
 * Cache routine for NUM to_char version
 * ----------
 */
static FormatNode *
NUM_cache(int len, NUMDesc *Num, char *pars_str, bool *shouldFree)
{
	FormatNode *format = NULL;
	char	   *str;

	/*
	 * Convert VARDATA() to string
	 */
	str = (char *) palloc(len + 1);
	memcpy(str, pars_str, len);
	*(str + len) = '\0';

	/*
	 * Allocate new memory if format picture is bigger than static cache and
	 * not use cache (call parser always). This branches sets shouldFree to
	 * true, accordingly.
	 */
	if (len > NUM_CACHE_SIZE)
	{
		format = (FormatNode *) palloc((len + 1) * sizeof(FormatNode));

		*shouldFree = true;

		zeroize_NUM(Num);

		parse_format(format, str, NUM_keywords,
					 NULL, NUM_index, NUM_TYPE, Num);

		(format + len)->type = NODE_TYPE_END;	/* Paranoia? */
	}
	else
	{
		/*
		 * Use cache buffers
		 */
		NUMCacheEntry *ent;

		*shouldFree = false;

		if ((ent = NUM_cache_search(str)) == NULL)
		{
			ent = NUM_cache_getnew(str);

			/*
			 * Not in the cache, must run parser and save a new format-picture
			 * to the cache.
			 */
			parse_format(ent->format, str, NUM_keywords,
						 NULL, NUM_index, NUM_TYPE, &ent->Num);

			(ent->format + len)->type = NODE_TYPE_END;	/* Paranoia? */
		}

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


static char *
int_to_roman(int number)
{
	int			len = 0,
				num = 0;
	char	   *p = NULL,
			   *result,
				numstr[5];

	result = (char *) palloc(16);
	*result = '\0';

	if (number > 3999 || number < 1)
	{
		fill_str(result, '#', 15);
		return result;
	}
	len = snprintf(numstr, sizeof(numstr), "%d", number);

	for (p = numstr; *p != '\0'; p++, --len)
	{
		num = *p - 49;			/* 48 ascii + 1 */
		if (num < 0)
			continue;

		if (len > 3)
		{
			while (num-- != -1)
				strcat(result, "M");
		}
		else
		{
			if (len == 3)
				strcat(result, rm100[num]);
			else if (len == 2)
				strcat(result, rm10[num]);
			else if (len == 1)
				strcat(result, rm1[num]);
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
		 * Number thousands separator
		 */
		if (lconv->thousands_sep && *lconv->thousands_sep)
			Np->L_thousands_sep = lconv->thousands_sep;
		else
			Np->L_thousands_sep = ",";

		/*
		 * Number decimal point
		 */
		if (lconv->decimal_point && *lconv->decimal_point)
			Np->decimal = lconv->decimal_point;

		else
			Np->decimal = ".";

		/*
		 * Currency symbol
		 */
		if (lconv->currency_symbol && *lconv->currency_symbol)
			Np->L_currency_symbol = lconv->currency_symbol;
		else
			Np->L_currency_symbol = " ";


		if (!IS_LDECIMAL(Np->Num))
			Np->decimal = ".";
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
		p = num;
	result = p;

	while (*(++p))
	{
		if (*p != '0')
			result = p;
	}

	return result;
}

/* ----------
 * Number extraction for TO_NUMBER()
 * ----------
 */
static void
NUM_numpart_from_char(NUMProc *Np, int id, int plen)
{
	bool		isread = FALSE;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, " --- scan start --- id=%s",
		 (id == NUM_0 || id == NUM_9) ? "NUM_0/9" : id == NUM_DEC ? "NUM_DEC" : "???");
#endif

	if (*Np->inout_p == ' ')
		Np->inout_p++;

#define OVERLOAD_TEST	(Np->inout_p >= Np->inout + plen)
#define AMOUNT_TEST(_s) (plen-(Np->inout_p-Np->inout) >= _s)

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
				*Np->number = '-';		/* set - */
				Np->inout_p++;
			}
			else if (*Np->inout_p == '+')
			{
				*Np->number = '+';		/* set + */
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
	 * read digit
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

		isread = TRUE;

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Read digit (%c)", *Np->inout_p);
#endif

		/*
		 * read decimal point
		 */
	}
	else if (IS_DECIMAL(Np->Num) && Np->read_dec == FALSE)
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Try read decimal point (%c)", *Np->inout_p);
#endif
		if (*Np->inout_p == '.')
		{
			*Np->number_p = '.';
			Np->number_p++;
			Np->read_dec = TRUE;
			isread = TRUE;
		}
		else
		{
			int			x = strlen(Np->decimal);

#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read locale point (%c)",
				 *Np->inout_p);
#endif
			if (x && AMOUNT_TEST(x) && strncmp(Np->inout_p, Np->decimal, x) == 0)
			{
				Np->inout_p += x - 1;
				*Np->number_p = '.';
				Np->number_p++;
				Np->read_dec = TRUE;
				isread = TRUE;
			}
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
			(Np->inout_p + 1) <= Np->inout + plen &&
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
		 * if (.... && IS_LSIGN(Np->Num)==FALSE) prevents read wrong formats
		 * like to_number('1 -', '9S') where sign is not anchored to last
		 * number.
		 */
		else if (isread == FALSE && IS_LSIGN(Np->Num) == FALSE &&
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
		(IS_ZERO((_n)->Num)==FALSE && \
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
	Np->num_in = FALSE;

	/*
	 * Write sign if real number will write to output Note: IS_PREDEC_SPACE()
	 * handle "9.9" --> " .1"
	 */
	if (Np->sign_wrote == FALSE &&
		(Np->num_curr >= Np->num_pre || (IS_ZERO(Np->Num) && Np->Num->zero_start == Np->num_curr)) &&
		(IS_PREDEC_SPACE(Np) == FALSE || (Np->last_relevant && *Np->last_relevant == '.')))
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
				Np->sign_wrote = TRUE;
			}
		}
		else if (IS_BRACKET(Np->Num))
		{
			*Np->inout_p = Np->sign == '+' ? ' ' : '<';
			++Np->inout_p;
			Np->sign_wrote = TRUE;
		}
		else if (Np->sign == '+')
		{
			if (!IS_FILLMODE(Np->Num))
			{
				*Np->inout_p = ' ';		/* Write + */
				++Np->inout_p;
			}
			Np->sign_wrote = TRUE;
		}
		else if (Np->sign == '-')
		{						/* Write - */
			*Np->inout_p = '-';
			++Np->inout_p;
			Np->sign_wrote = TRUE;
		}
	}


	/*
	 * digits / FM / Zero / Dec. point
	 */
	if (id == NUM_9 || id == NUM_0 || id == NUM_D || id == NUM_DEC)
	{
		if (Np->num_curr < Np->num_pre &&
			(Np->Num->zero_start > Np->num_curr || !IS_ZERO(Np->Num)))
		{
			/*
			 * Write blank space
			 */
			if (!IS_FILLMODE(Np->Num))
			{
				*Np->inout_p = ' ';		/* Write ' ' */
				++Np->inout_p;
			}
		}
		else if (IS_ZERO(Np->Num) &&
				 Np->num_curr < Np->num_pre &&
				 Np->Num->zero_start <= Np->num_curr)
		{
			/*
			 * Write ZERO
			 */
			*Np->inout_p = '0'; /* Write '0' */
			++Np->inout_p;
			Np->num_in = TRUE;
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
					*Np->inout_p = *Np->number_p;		/* Write DIGIT */
					++Np->inout_p;
					Np->num_in = TRUE;
				}
			}
			++Np->number_p;
		}

		end = Np->num_count + (Np->num_pre ? 1 : 0) + (IS_DECIMAL(Np->Num) ? 1 : 0);

		if (Np->last_relevant && Np->last_relevant == Np->number_p)
			end = Np->num_curr;

		if (Np->num_curr + 1 == end)
		{
			if (Np->sign_wrote == TRUE && IS_BRACKET(Np->Num))
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
 * Note: 'plen' is used in FROM_CHAR conversion and it's length of
 * input (inout). In TO_CHAR conversion it's space before first number.
 */
static char *
NUM_processor(FormatNode *node, NUMDesc *Num, char *inout, char *number,
			  int plen, int sign, bool is_to_char)
{
	FormatNode *n;
	NUMProc		_Np,
			   *Np = &_Np;

	MemSet(Np, 0, sizeof(NUMProc));

	Np->Num = Num;
	Np->is_to_char = is_to_char;
	Np->number = number;
	Np->inout = inout;
	Np->last_relevant = NULL;
	Np->read_post = 0;
	Np->read_pre = 0;
	Np->read_dec = FALSE;

	if (Np->Num->zero_start)
		--Np->Num->zero_start;

	/*
	 * Roman correction
	 */
	if (IS_ROMAN(Np->Num))
	{
		if (!Np->is_to_char)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"RN\" not supported")));

		Np->Num->lsign = Np->Num->pre_lsign_num = Np->Num->post =
			Np->Num->pre = Np->num_pre = Np->sign = 0;

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
			if (IS_PLUS(Np->Num) && IS_MINUS(Np->Num) == FALSE)
				Np->sign_wrote = FALSE; /* need sign */
			else
				Np->sign_wrote = TRUE;	/* needn't sign */
		}
		else
		{
			if (Np->sign != '-')
			{
				if (IS_BRACKET(Np->Num) && IS_FILLMODE(Np->Num))
					Np->Num->flag &= ~NUM_F_BRACKET;
				if (IS_MINUS(Np->Num))
					Np->Num->flag &= ~NUM_F_MINUS;
			}
			else if (Np->sign != '+' && IS_PLUS(Np->Num))
				Np->Num->flag &= ~NUM_F_PLUS;

			if (Np->sign == '+' && IS_FILLMODE(Np->Num) && IS_LSIGN(Np->Num) == FALSE)
				Np->sign_wrote = TRUE;	/* needn't sign */
			else
				Np->sign_wrote = FALSE; /* need sign */

			if (Np->Num->lsign == NUM_LSIGN_PRE && Np->Num->pre == Np->Num->pre_lsign_num)
				Np->Num->lsign = NUM_LSIGN_POST;
		}
	}
	else
		Np->sign = FALSE;

	/*
	 * Count
	 */
	Np->num_count = Np->Num->post + Np->Num->pre - 1;

	if (is_to_char)
	{
		Np->num_pre = plen;

		if (IS_FILLMODE(Np->Num))
		{
			if (IS_DECIMAL(Np->Num))
				Np->last_relevant = get_last_relevant_decnum(
															 Np->number +
									 ((Np->Num->zero_end - Np->num_pre > 0) ?
									  Np->Num->zero_end - Np->num_pre : 0));
		}

		if (Np->sign_wrote == FALSE && Np->num_pre == 0)
			++Np->num_count;
	}
	else
	{
		Np->num_pre = 0;
		*Np->number = ' ';		/* sign space */
		*(Np->number + 1) = '\0';
	}

	Np->num_in = 0;
	Np->num_curr = 0;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output,
		 "\n\tSIGN: '%c'\n\tNUM: '%s'\n\tPRE: %d\n\tPOST: %d\n\tNUM_COUNT: %d\n\tNUM_PRE: %d\n\tSIGN_WROTE: %s\n\tZERO: %s\n\tZERO_START: %d\n\tZERO_END: %d\n\tLAST_RELEVANT: %s\n\tBRACKET: %s\n\tPLUS: %s\n\tMINUS: %s\n\tFILLMODE: %s\n\tROMAN: %s",
		 Np->sign,
		 Np->number,
		 Np->Num->pre,
		 Np->Num->post,
		 Np->num_count,
		 Np->num_pre,
		 Np->sign_wrote ? "Yes" : "No",
		 IS_ZERO(Np->Num) ? "Yes" : "No",
		 Np->Num->zero_start,
		 Np->Num->zero_end,
		 Np->last_relevant ? Np->last_relevant : "<not set>",
		 IS_BRACKET(Np->Num) ? "Yes" : "No",
		 IS_PLUS(Np->Num) ? "Yes" : "No",
		 IS_MINUS(Np->Num) ? "Yes" : "No",
		 IS_FILLMODE(Np->Num) ? "Yes" : "No",
		 IS_ROMAN(Np->Num) ? "Yes" : "No"
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
			 * Check non-string inout end
			 */
			if (Np->inout_p >= Np->inout + plen)
				break;
		}

		/*
		 * Format pictures actions
		 */
		if (n->type == NODE_TYPE_ACTION)
		{
			/*
			 * Create/reading digit/zero/blank/sing
			 *
			 * 'NUM_S' note: The locale sign is anchored to number and we
			 * read/write it when we work with first or last number
			 * (NUM_0/NUM_9). This is reason why NUM_S missing in follow
			 * switch().
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
						continue;		/* for() */
					}
					else
					{
						NUM_numpart_from_char(Np, n->key->id, plen);
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
					}
					break;

				case NUM_G:
					if (Np->is_to_char)
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
							else
							{
								int			x = strlen(Np->L_thousands_sep);

								memset(Np->inout_p, ' ', x);
								Np->inout_p += x - 1;
							}
						}
						else
						{
							strcpy(Np->inout_p, Np->L_thousands_sep);
							Np->inout_p += strlen(Np->inout_p) - 1;
						}
					}
					else
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
						}
						Np->inout_p += strlen(Np->L_thousands_sep) - 1;
					}
					break;

				case NUM_L:
					if (Np->is_to_char)
					{
						strcpy(Np->inout_p, Np->L_currency_symbol);
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					else
						Np->inout_p += strlen(Np->L_currency_symbol) - 1;
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
						strcpy(Np->inout_p, str_tolower(Np->number_p));
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					else
					{
						sprintf(Np->inout_p, "%15s", str_tolower(Np->number_p));
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					break;

				case NUM_th:
					if (IS_ROMAN(Np->Num) || *Np->number == '#' ||
						Np->sign == '-' || IS_DECIMAL(Np->Num))
						continue;

					if (Np->is_to_char)
						strcpy(Np->inout_p, get_th(Np->number, TH_LOWER));
					Np->inout_p += 1;
					break;

				case NUM_TH:
					if (IS_ROMAN(Np->Num) || *Np->number == '#' ||
						Np->sign == '-' || IS_DECIMAL(Np->Num))
						continue;

					if (Np->is_to_char)
						strcpy(Np->inout_p, get_th(Np->number, TH_UPPER));
					Np->inout_p += 1;
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
			 * Remove to output char from input in TO_CHAR
			 */
			if (Np->is_to_char)
				*Np->inout_p = n->character;
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
	len = VARSIZE(fmt) - VARHDRSZ;					\
	if (len <= 0 || len >= (INT_MAX-VARHDRSZ)/NUM_MAX_ITEM_SIZ)		\
		return DirectFunctionCall1(textin, CStringGetDatum(""));	\
	result	= (text *) palloc0((len * NUM_MAX_ITEM_SIZ) + 1 + VARHDRSZ);	\
	format	= NUM_cache(len, &Num, VARDATA(fmt), &shouldFree);		\
} while (0)

/* ----------
 * MACRO: Finish part of NUM
 * ----------
 */
#define NUM_TOCHAR_finish \
do { \
	NUM_processor(format, &Num, VARDATA(result), numstr, plen, sign, true);	\
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
	VARATT_SIZEP(result) = len + VARHDRSZ;	\
} while (0)

/* -------------------
 * NUMERIC to_number() (convert string to numeric)
 * -------------------
 */
Datum
numeric_to_number(PG_FUNCTION_ARGS)
{
	text	   *value = PG_GETARG_TEXT_P(0);
	text	   *fmt = PG_GETARG_TEXT_P(1);
	NUMDesc		Num;
	Datum		result;
	FormatNode *format;
	char	   *numstr;
	bool		shouldFree;
	int			len = 0;
	int			scale,
				precision;

	len = VARSIZE(fmt) - VARHDRSZ;

	if (len <= 0 || len >= INT_MAX/NUM_MAX_ITEM_SIZ)
		PG_RETURN_NULL();

	format = NUM_cache(len, &Num, VARDATA(fmt), &shouldFree);

	numstr = (char *) palloc((len * NUM_MAX_ITEM_SIZ) + 1);

	NUM_processor(format, &Num, VARDATA(value), numstr,
				  VARSIZE(value) - VARHDRSZ, 0, false);

	scale = Num.post;
	precision = Max(0, Num.pre) + scale;

	if (shouldFree)
		pfree(format);

	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(numstr),
								 ObjectIdGetDatum(InvalidOid),
					  Int32GetDatum(((precision << 16) | scale) + VARHDRSZ));
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
	text	   *fmt = PG_GETARG_TEXT_P(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			len = 0,
				plen = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum,
			   *p;
	Numeric		x;

	NUM_TOCHAR_prepare;

	/*
	 * On DateType depend part (numeric)
	 */
	if (IS_ROMAN(&Num))
	{
		x = DatumGetNumeric(DirectFunctionCall2(numeric_round,
												NumericGetDatum(value),
												Int32GetDatum(0)));
		numstr = orgnum =
			int_to_roman(DatumGetInt32(DirectFunctionCall1(numeric_int4,
													   NumericGetDatum(x))));
	}
	else
	{
		Numeric		val = value;

		if (IS_MULTI(&Num))
		{
			Numeric		a = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
														 Int32GetDatum(10)));
			Numeric		b = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
												  Int32GetDatum(Num.multi)));

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
			len = p - numstr;
		else
			len = strlen(numstr);

		if (Num.pre > len)
			plen = Num.pre - len;
		else if (len > Num.pre)
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
	text	   *fmt = PG_GETARG_TEXT_P(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			len = 0,
				plen = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum;

	NUM_TOCHAR_prepare;

	/*
	 * On DateType depend part (int32)
	 */
	if (IS_ROMAN(&Num))
		numstr = orgnum = int_to_roman(value);
	else
	{
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
		len = strlen(orgnum);

		if (Num.post)
		{
			numstr = (char *) palloc(len + Num.post + 2);
			strcpy(numstr, orgnum);
			*(numstr + len) = '.';
			memset(numstr + len + 1, '0', Num.post);
			*(numstr + len + Num.post + 1) = '\0';
		}
		else
			numstr = orgnum;

		if (Num.pre > len)
			plen = Num.pre - len;
		else if (len > Num.pre)
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
	text	   *fmt = PG_GETARG_TEXT_P(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			len = 0,
				plen = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum;

	NUM_TOCHAR_prepare;

	/*
	 * On DateType depend part (int32)
	 */
	if (IS_ROMAN(&Num))
	{
		/* Currently don't support int8 conversion to roman... */
		numstr = orgnum = int_to_roman(DatumGetInt32(
						  DirectFunctionCall1(int84, Int64GetDatum(value))));
	}
	else
	{
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
		len = strlen(orgnum);

		if (Num.post)
		{
			numstr = (char *) palloc(len + Num.post + 2);
			strcpy(numstr, orgnum);
			*(numstr + len) = '.';
			memset(numstr + len + 1, '0', Num.post);
			*(numstr + len + Num.post + 1) = '\0';
		}
		else
			numstr = orgnum;

		if (Num.pre > len)
			plen = Num.pre - len;
		else if (len > Num.pre)
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
	text	   *fmt = PG_GETARG_TEXT_P(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			len = 0,
				plen = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum,
			   *p;

	NUM_TOCHAR_prepare;

	if (IS_ROMAN(&Num))
		numstr = orgnum = int_to_roman((int) rint(value));
	else
	{
		float4		val = value;

		if (IS_MULTI(&Num))
		{
			float		multi = pow((double) 10, (double) Num.multi);

			val = value * multi;
			Num.pre += Num.multi;
		}

		orgnum = (char *) palloc(MAXFLOATWIDTH + 1);
		snprintf(orgnum, MAXFLOATWIDTH + 1, "%.0f", fabs(val));
		len = strlen(orgnum);
		if (Num.pre > len)
			plen = Num.pre - len;
		if (len >= FLT_DIG)
			Num.post = 0;
		else if (Num.post + len > FLT_DIG)
			Num.post = FLT_DIG - len;
		snprintf(orgnum, MAXFLOATWIDTH + 1, "%.*f", Num.post, val);

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
			len = p - numstr;
		else
			len = strlen(numstr);

		if (Num.pre > len)
			plen = Num.pre - len;
		else if (len > Num.pre)
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
	text	   *fmt = PG_GETARG_TEXT_P(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			len = 0,
				plen = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum,
			   *p;

	NUM_TOCHAR_prepare;

	if (IS_ROMAN(&Num))
		numstr = orgnum = int_to_roman((int) rint(value));
	else
	{
		float8		val = value;

		if (IS_MULTI(&Num))
		{
			double		multi = pow((double) 10, (double) Num.multi);

			val = value * multi;
			Num.pre += Num.multi;
		}
		orgnum = (char *) palloc(MAXDOUBLEWIDTH + 1);
		len = snprintf(orgnum, MAXDOUBLEWIDTH + 1, "%.0f", fabs(val));
		if (Num.pre > len)
			plen = Num.pre - len;
		if (len >= DBL_DIG)
			Num.post = 0;
		else if (Num.post + len > DBL_DIG)
			Num.post = DBL_DIG - len;
		snprintf(orgnum, MAXDOUBLEWIDTH + 1, "%.*f", Num.post, val);

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
			len = p - numstr;
		else
			len = strlen(numstr);

		if (Num.pre > len)
			plen = Num.pre - len;
		else if (len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}
