/*-------------------------------------------------------------------------
 *
 * datetime.c
 *	  Support functions for date/time types.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/datetime.c,v 1.104 2003/05/04 04:30:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#include "miscadmin.h"
#include "utils/datetime.h"
#include "utils/guc.h"


static int DecodeNumber(int flen, char *field,
			 int fmask, int *tmask,
			 struct tm * tm, fsec_t *fsec, int *is2digits);
static int DecodeNumberField(int len, char *str,
				  int fmask, int *tmask,
				  struct tm * tm, fsec_t *fsec, int *is2digits);
static int DecodeTime(char *str, int fmask, int *tmask,
		   struct tm * tm, fsec_t *fsec);
static int	DecodeTimezone(char *str, int *tzp);
static datetkn *datebsearch(char *key, datetkn *base, unsigned int nel);
static int	DecodeDate(char *str, int fmask, int *tmask, struct tm * tm);
static int	DecodePosixTimezone(char *str, int *val);
static void TrimTrailingZeros(char *str);


int			day_tab[2][13] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0},
{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0}};

char	   *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL};

char	   *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
"Thursday", "Friday", "Saturday", NULL};


/*****************************************************************************
 *	 PRIVATE ROUTINES														 *
 *****************************************************************************/

/*
 * Definitions for squeezing values into "value"
 * We set aside a high bit for a sign, and scale the timezone offsets
 * in minutes by a factor of 15 (so can represent quarter-hour increments).
 */
#define ABS_SIGNBIT		((char) 0200)
#define VALMASK			((char) 0177)
#define POS(n)			(n)
#define NEG(n)			((n)|ABS_SIGNBIT)
#define SIGNEDCHAR(c)	((c)&ABS_SIGNBIT? -((c)&VALMASK): (c))
#define FROMVAL(tp)		(-SIGNEDCHAR((tp)->value) * 15) /* uncompress */
#define TOVAL(tp, v)	((tp)->value = ((v) < 0? NEG((-(v))/15): POS(v)/15))

/*
 * datetktbl holds date/time keywords.
 *
 * Note that this table must be strictly alphabetically ordered to allow an
 * O(ln(N)) search algorithm to be used.
 *
 * The text field is NOT guaranteed to be NULL-terminated.
 *
 * To keep this table reasonably small, we divide the lexval for TZ and DTZ
 * entries by 15 (so they are on 15 minute boundaries) and truncate the text
 * field at TOKMAXLEN characters.
 * Formerly, we divided by 10 rather than 15 but there are a few time zones
 * which are 30 or 45 minutes away from an even hour, most are on an hour
 * boundary, and none on other boundaries.
 *
 * Let's include all strings from my current zinc time zone database.
 * Not all of them are unique, or even very understandable, so we will
 * leave some commented out for now.
 */
static datetkn datetktbl[] = {
/*	text, token, lexval */
	{EARLY, RESERV, DTK_EARLY}, /* "-infinity" reserved for "early time" */
	{"abstime", IGNORE_DTF, 0}, /* for pre-v6.1 "Invalid Abstime" */
	{"acsst", DTZ, POS(42)},	/* Cent. Australia */
	{"acst", DTZ, NEG(16)},		/* Atlantic/Porto Acre */
	{"act", TZ, NEG(20)},		/* Atlantic/Porto Acre */
	{DA_D, ADBC, AD},			/* "ad" for years >= 0 */
	{"adt", DTZ, NEG(12)},		/* Atlantic Daylight Time */
	{"aesst", DTZ, POS(44)},	/* E. Australia */
	{"aest", TZ, POS(40)},		/* Australia Eastern Std Time */
	{"aft", TZ, POS(18)},		/* Kabul */
	{"ahst", TZ, NEG(40)},		/* Alaska-Hawaii Std Time */
	{"akdt", DTZ, NEG(32)},		/* Alaska Daylight Time */
	{"akst", DTZ, NEG(36)},		/* Alaska Standard Time */
	{"allballs", RESERV, DTK_ZULU},		/* 00:00:00 */
	{"almst", TZ, POS(28)},		/* Almaty Savings Time */
	{"almt", TZ, POS(24)},		/* Almaty Time */
	{"am", AMPM, AM},
	{"amst", DTZ, POS(20)},		/* Armenia Summer Time (Yerevan) */
#if 0
	{"amst", DTZ, NEG(12)},		/* Porto Velho */
#endif
	{"amt", TZ, POS(16)},		/* Armenia Time (Yerevan) */
	{"anast", DTZ, POS(52)},	/* Anadyr Summer Time (Russia) */
	{"anat", TZ, POS(48)},		/* Anadyr Time (Russia) */
	{"apr", MONTH, 4},
	{"april", MONTH, 4},
#if 0
	aqtst
	aqtt
	arst
#endif
	{"art", TZ, NEG(12)},		/* Argentina Time */
#if 0
	ashst
	ast							/* Atlantic Standard Time, Arabia Standard
								 * Time, Acre Standard Time */
#endif
	{"ast", TZ, NEG(16)},		/* Atlantic Std Time (Canada) */
	{"at", IGNORE_DTF, 0},		/* "at" (throwaway) */
	{"aug", MONTH, 8},
	{"august", MONTH, 8},
	{"awsst", DTZ, POS(36)},	/* W. Australia */
	{"awst", TZ, POS(32)},		/* W. Australia */
	{"awt", DTZ, NEG(12)},
	{"azost", DTZ, POS(0)},		/* Azores Summer Time */
	{"azot", TZ, NEG(4)},		/* Azores Time */
	{"azst", DTZ, POS(20)},		/* Azerbaijan Summer Time */
	{"azt", TZ, POS(16)},		/* Azerbaijan Time */
	{DB_C, ADBC, BC},			/* "bc" for years < 0 */
	{"bdst", TZ, POS(8)},		/* British Double Summer Time */
	{"bdt", TZ, POS(24)},		/* Dacca */
	{"bnt", TZ, POS(32)},		/* Brunei Darussalam Time */
	{"bort", TZ, POS(32)},		/* Borneo Time (Indonesia) */
#if 0
	bortst
	bost
#endif
	{"bot", TZ, NEG(16)},		/* Bolivia Time */
	{"bra", TZ, NEG(12)},		/* Brazil Time */
#if 0
	brst
	brt
#endif
	{"bst", DTZ, POS(4)},		/* British Summer Time */
#if 0
	{"bst", TZ, NEG(12)},		/* Brazil Standard Time */
	{"bst", DTZ, NEG(44)},		/* Bering Summer Time */
#endif
	{"bt", TZ, POS(12)},		/* Baghdad Time */
	{"btt", TZ, POS(24)},		/* Bhutan Time */
	{"cadt", DTZ, POS(42)},		/* Central Australian DST */
	{"cast", TZ, POS(38)},		/* Central Australian ST */
	{"cat", TZ, NEG(40)},		/* Central Alaska Time */
	{"cct", TZ, POS(32)},		/* China Coast Time */
#if 0
	{"cct", TZ, POS(26)},		/* Indian Cocos (Island) Time */
#endif
	{"cdt", DTZ, NEG(20)},		/* Central Daylight Time */
	{"cest", DTZ, POS(8)},		/* Central European Dayl.Time */
	{"cet", TZ, POS(4)},		/* Central European Time */
	{"cetdst", DTZ, POS(8)},	/* Central European Dayl.Time */
	{"chadt", DTZ, POS(55)},	/* Chatham Island Daylight Time (13:45) */
	{"chast", TZ, POS(51)},		/* Chatham Island Time (12:45) */
#if 0
	ckhst
#endif
	{"ckt", TZ, POS(48)},		/* Cook Islands Time */
	{"clst", DTZ, NEG(12)},		/* Chile Summer Time */
	{"clt", TZ, NEG(16)},		/* Chile Time */
#if 0
	cost
#endif
	{"cot", TZ, NEG(20)},		/* Columbia Time */
	{"cst", TZ, NEG(24)},		/* Central Standard Time */
	{DCURRENT, RESERV, DTK_CURRENT},	/* "current" is always now */
#if 0
	cvst
#endif
	{"cvt", TZ, POS(28)},		/* Christmas Island Time (Indian Ocean) */
	{"cxt", TZ, POS(28)},		/* Christmas Island Time (Indian Ocean) */
	{"d", UNITS, DTK_DAY},		/* "day of month" for ISO input */
	{"davt", TZ, POS(28)},		/* Davis Time (Antarctica) */
	{"ddut", TZ, POS(40)},		/* Dumont-d'Urville Time (Antarctica) */
	{"dec", MONTH, 12},
	{"december", MONTH, 12},
	{"dnt", TZ, POS(4)},		/* Dansk Normal Tid */
	{"dow", RESERV, DTK_DOW},	/* day of week */
	{"doy", RESERV, DTK_DOY},	/* day of year */
	{"dst", DTZMOD, 6},
#if 0
	{"dusst", DTZ, POS(24)},	/* Dushanbe Summer Time */
#endif
	{"easst", DTZ, NEG(20)},	/* Easter Island Summer Time */
	{"east", TZ, NEG(24)},		/* Easter Island Time */
	{"eat", TZ, POS(12)},		/* East Africa Time */
#if 0
	{"east", DTZ, POS(16)},		/* Indian Antananarivo Savings Time */
	{"eat", TZ, POS(12)},		/* Indian Antananarivo Time */
	{"ect", TZ, NEG(16)},		/* Eastern Caribbean Time */
	{"ect", TZ, NEG(20)},		/* Ecuador Time */
#endif
	{"edt", DTZ, NEG(16)},		/* Eastern Daylight Time */
	{"eest", DTZ, POS(12)},		/* Eastern Europe Summer Time */
	{"eet", TZ, POS(8)},		/* East. Europe, USSR Zone 1 */
	{"eetdst", DTZ, POS(12)},	/* Eastern Europe Daylight Time */
	{"egst", DTZ, POS(0)},		/* East Greenland Summer Time */
	{"egt", TZ, NEG(4)},		/* East Greenland Time */
#if 0
	ehdt
#endif
	{EPOCH, RESERV, DTK_EPOCH}, /* "epoch" reserved for system epoch time */
	{"est", TZ, NEG(20)},		/* Eastern Standard Time */
	{"feb", MONTH, 2},
	{"february", MONTH, 2},
	{"fjst", DTZ, NEG(52)},		/* Fiji Summer Time (13 hour offset!) */
	{"fjt", TZ, NEG(48)},		/* Fiji Time */
	{"fkst", DTZ, NEG(12)},		/* Falkland Islands Summer Time */
	{"fkt", TZ, NEG(8)},		/* Falkland Islands Time */
#if 0
	fnst
	fnt
#endif
	{"fri", DOW, 5},
	{"friday", DOW, 5},
	{"fst", TZ, POS(4)},		/* French Summer Time */
	{"fwt", DTZ, POS(8)},		/* French Winter Time  */
	{"galt", TZ, NEG(24)},		/* Galapagos Time */
	{"gamt", TZ, NEG(36)},		/* Gambier Time */
	{"gest", DTZ, POS(20)},		/* Georgia Summer Time */
	{"get", TZ, POS(16)},		/* Georgia Time */
	{"gft", TZ, NEG(12)},		/* French Guiana Time */
#if 0
	ghst
#endif
	{"gilt", TZ, POS(48)},		/* Gilbert Islands Time */
	{"gmt", TZ, POS(0)},		/* Greenwish Mean Time */
	{"gst", TZ, POS(40)},		/* Guam Std Time, USSR Zone 9 */
	{"gyt", TZ, NEG(16)},		/* Guyana Time */
	{"h", UNITS, DTK_HOUR},		/* "hour" */
#if 0
	hadt
	hast
#endif
	{"hdt", DTZ, NEG(36)},		/* Hawaii/Alaska Daylight Time */
#if 0
	hkst
#endif
	{"hkt", TZ, POS(32)},		/* Hong Kong Time */
#if 0
	{"hmt", TZ, POS(12)},		/* Hellas ? ? */
	hovst
	hovt
#endif
	{"hst", TZ, NEG(40)},		/* Hawaii Std Time */
#if 0
	hwt
#endif
	{"ict", TZ, POS(28)},		/* Indochina Time */
	{"idle", TZ, POS(48)},		/* Intl. Date Line, East */
	{"idlw", TZ, NEG(48)},		/* Intl. Date Line, West */
#if 0
	idt							/* Israeli, Iran, Indian Daylight Time */
#endif
	{LATE, RESERV, DTK_LATE},	/* "infinity" reserved for "late time" */
	{INVALID, RESERV, DTK_INVALID},		/* "invalid" reserved for bad time */
	{"iot", TZ, POS(20)},		/* Indian Chagos Time */
	{"irkst", DTZ, POS(36)},	/* Irkutsk Summer Time */
	{"irkt", TZ, POS(32)},		/* Irkutsk Time */
	{"irt", TZ, POS(14)},		/* Iran Time */
#if 0
	isst
#endif
	{"ist", TZ, POS(8)},		/* Israel */
	{"it", TZ, POS(14)},		/* Iran Time */
	{"j", UNITS, DTK_JULIAN},
	{"jan", MONTH, 1},
	{"january", MONTH, 1},
	{"javt", TZ, POS(28)},		/* Java Time (07:00? see JT) */
	{"jayt", TZ, POS(36)},		/* Jayapura Time (Indonesia) */
	{"jd", UNITS, DTK_JULIAN},
	{"jst", TZ, POS(36)},		/* Japan Std Time,USSR Zone 8 */
	{"jt", TZ, POS(30)},		/* Java Time (07:30? see JAVT) */
	{"jul", MONTH, 7},
	{"julian", UNITS, DTK_JULIAN},
	{"july", MONTH, 7},
	{"jun", MONTH, 6},
	{"june", MONTH, 6},
	{"kdt", DTZ, POS(40)},		/* Korea Daylight Time */
	{"kgst", DTZ, POS(24)},		/* Kyrgyzstan Summer Time */
	{"kgt", TZ, POS(20)},		/* Kyrgyzstan Time */
	{"kost", TZ, POS(48)},		/* Kosrae Time */
	{"krast", DTZ, POS(28)},	/* Krasnoyarsk Summer Time */
	{"krat", TZ, POS(32)},		/* Krasnoyarsk Standard Time */
	{"kst", TZ, POS(36)},		/* Korea Standard Time */
	{"lhdt", DTZ, POS(44)},		/* Lord Howe Daylight Time, Australia */
	{"lhst", TZ, POS(42)},		/* Lord Howe Standard Time, Australia */
	{"ligt", TZ, POS(40)},		/* From Melbourne, Australia */
	{"lint", TZ, POS(56)},		/* Line Islands Time (Kiribati; +14
								 * hours!) */
	{"lkt", TZ, POS(24)},		/* Lanka Time */
	{"m", UNITS, DTK_MONTH},	/* "month" for ISO input */
	{"magst", DTZ, POS(48)},	/* Magadan Summer Time */
	{"magt", TZ, POS(44)},		/* Magadan Time */
	{"mar", MONTH, 3},
	{"march", MONTH, 3},
	{"mart", TZ, NEG(38)},		/* Marquesas Time */
	{"mawt", TZ, POS(24)},		/* Mawson, Antarctica */
	{"may", MONTH, 5},
	{"mdt", DTZ, NEG(24)},		/* Mountain Daylight Time */
	{"mest", DTZ, POS(8)},		/* Middle Europe Summer Time */
	{"met", TZ, POS(4)},		/* Middle Europe Time */
	{"metdst", DTZ, POS(8)},	/* Middle Europe Daylight Time */
	{"mewt", TZ, POS(4)},		/* Middle Europe Winter Time */
	{"mez", TZ, POS(4)},		/* Middle Europe Zone */
	{"mht", TZ, POS(48)},		/* Kwajalein */
	{"mm", UNITS, DTK_MINUTE},	/* "minute" for ISO input */
	{"mmt", TZ, POS(26)},		/* Myannar Time */
	{"mon", DOW, 1},
	{"monday", DOW, 1},
#if 0
	most
#endif
	{"mpt", TZ, POS(40)},		/* North Mariana Islands Time */
	{"msd", DTZ, POS(16)},		/* Moscow Summer Time */
	{"msk", TZ, POS(12)},		/* Moscow Time */
	{"mst", TZ, NEG(28)},		/* Mountain Standard Time */
	{"mt", TZ, POS(34)},		/* Moluccas Time */
	{"mut", TZ, POS(16)},		/* Mauritius Island Time */
	{"mvt", TZ, POS(20)},		/* Maldives Island Time */
	{"myt", TZ, POS(32)},		/* Malaysia Time */
#if 0
	ncst
#endif
	{"nct", TZ, POS(44)},		/* New Caledonia Time */
	{"ndt", DTZ, NEG(10)},		/* Nfld. Daylight Time */
	{"nft", TZ, NEG(14)},		/* Newfoundland Standard Time */
	{"nor", TZ, POS(4)},		/* Norway Standard Time */
	{"nov", MONTH, 11},
	{"november", MONTH, 11},
	{"novst", DTZ, POS(28)},	/* Novosibirsk Summer Time */
	{"novt", TZ, POS(24)},		/* Novosibirsk Standard Time */
	{NOW, RESERV, DTK_NOW},		/* current transaction time */
	{"npt", TZ, POS(23)},		/* Nepal Standard Time (GMT-5:45) */
	{"nst", TZ, NEG(14)},		/* Nfld. Standard Time */
	{"nt", TZ, NEG(44)},		/* Nome Time */
	{"nut", TZ, NEG(44)},		/* Niue Time */
	{"nzdt", DTZ, POS(52)},		/* New Zealand Daylight Time */
	{"nzst", TZ, POS(48)},		/* New Zealand Standard Time */
	{"nzt", TZ, POS(48)},		/* New Zealand Time */
	{"oct", MONTH, 10},
	{"october", MONTH, 10},
	{"omsst", DTZ, POS(28)},	/* Omsk Summer Time */
	{"omst", TZ, POS(24)},		/* Omsk Time */
	{"on", IGNORE_DTF, 0},		/* "on" (throwaway) */
	{"pdt", DTZ, NEG(28)},		/* Pacific Daylight Time */
#if 0
	pest
#endif
	{"pet", TZ, NEG(20)},		/* Peru Time */
	{"petst", DTZ, POS(52)},	/* Petropavlovsk-Kamchatski Summer Time */
	{"pett", TZ, POS(48)},		/* Petropavlovsk-Kamchatski Time */
	{"pgt", TZ, POS(40)},		/* Papua New Guinea Time */
	{"phot", TZ, POS(52)},		/* Phoenix Islands (Kiribati) Time */
#if 0
	phst
#endif
	{"pht", TZ, POS(32)},		/* Phillipine Time */
	{"pkt", TZ, POS(20)},		/* Pakistan Time */
	{"pm", AMPM, PM},
	{"pmdt", DTZ, NEG(8)},		/* Pierre & Miquelon Daylight Time */
#if 0
	pmst
#endif
	{"pont", TZ, POS(44)},		/* Ponape Time (Micronesia) */
	{"pst", TZ, NEG(32)},		/* Pacific Standard Time */
	{"pwt", TZ, POS(36)},		/* Palau Time */
	{"pyst", DTZ, NEG(12)},		/* Paraguay Summer Time */
	{"pyt", TZ, NEG(16)},		/* Paraguay Time */
	{"ret", DTZ, POS(16)},		/* Reunion Island Time */
	{"s", UNITS, DTK_SECOND},	/* "seconds" for ISO input */
	{"sadt", DTZ, POS(42)},		/* S. Australian Dayl. Time */
#if 0
	samst
	samt
#endif
	{"sast", TZ, POS(38)},		/* South Australian Std Time */
	{"sat", DOW, 6},
	{"saturday", DOW, 6},
#if 0
	sbt
#endif
	{"sct", DTZ, POS(16)},		/* Mahe Island Time */
	{"sep", MONTH, 9},
	{"sept", MONTH, 9},
	{"september", MONTH, 9},
	{"set", TZ, NEG(4)},		/* Seychelles Time ?? */
#if 0
	sgt
#endif
	{"sst", DTZ, POS(8)},		/* Swedish Summer Time */
	{"sun", DOW, 0},
	{"sunday", DOW, 0},
	{"swt", TZ, POS(4)},		/* Swedish Winter Time */
#if 0
	syot
#endif
	{"t", ISOTIME, DTK_TIME},	/* Filler for ISO time fields */
	{"tft", TZ, POS(20)},		/* Kerguelen Time */
	{"that", TZ, NEG(40)},		/* Tahiti Time */
	{"thu", DOW, 4},
	{"thur", DOW, 4},
	{"thurs", DOW, 4},
	{"thursday", DOW, 4},
	{"tjt", TZ, POS(20)},		/* Tajikistan Time */
	{"tkt", TZ, NEG(40)},		/* Tokelau Time */
	{"tmt", TZ, POS(20)},		/* Turkmenistan Time */
	{TODAY, RESERV, DTK_TODAY}, /* midnight */
	{TOMORROW, RESERV, DTK_TOMORROW},	/* tomorrow midnight */
#if 0
	tost
#endif
	{"tot", TZ, POS(52)},		/* Tonga Time */
#if 0
	tpt
#endif
	{"truk", TZ, POS(40)},		/* Truk Time */
	{"tue", DOW, 2},
	{"tues", DOW, 2},
	{"tuesday", DOW, 2},
	{"tvt", TZ, POS(48)},		/* Tuvalu Time */
#if 0
	uct
#endif
	{"ulast", DTZ, POS(36)},	/* Ulan Bator Summer Time */
	{"ulat", TZ, POS(32)},		/* Ulan Bator Time */
	{"undefined", RESERV, DTK_INVALID}, /* pre-v6.1 invalid time */
	{"ut", TZ, POS(0)},
	{"utc", TZ, POS(0)},
	{"uyst", DTZ, NEG(8)},		/* Uruguay Summer Time */
	{"uyt", TZ, NEG(12)},		/* Uruguay Time */
	{"uzst", DTZ, POS(24)},		/* Uzbekistan Summer Time */
	{"uzt", TZ, POS(20)},		/* Uzbekistan Time */
	{"vet", TZ, NEG(16)},		/* Venezuela Time */
	{"vlast", DTZ, POS(44)},	/* Vladivostok Summer Time */
	{"vlat", TZ, POS(40)},		/* Vladivostok Time */
#if 0
	vust
#endif
	{"vut", TZ, POS(44)},		/* Vanuata Time */
	{"wadt", DTZ, POS(32)},		/* West Australian DST */
	{"wakt", TZ, POS(48)},		/* Wake Time */
#if 0
	warst
#endif
	{"wast", TZ, POS(28)},		/* West Australian Std Time */
	{"wat", TZ, NEG(4)},		/* West Africa Time */
	{"wdt", DTZ, POS(36)},		/* West Australian DST */
	{"wed", DOW, 3},
	{"wednesday", DOW, 3},
	{"weds", DOW, 3},
	{"west", DTZ, POS(4)},		/* Western Europe Summer Time */
	{"wet", TZ, POS(0)},		/* Western Europe */
	{"wetdst", DTZ, POS(4)},	/* Western Europe Daylight Savings Time */
	{"wft", TZ, POS(48)},		/* Wallis and Futuna Time */
	{"wgst", DTZ, NEG(8)},		/* West Greenland Summer Time */
	{"wgt", TZ, NEG(12)},		/* West Greenland Time */
	{"wst", TZ, POS(32)},		/* West Australian Standard Time */
	{"y", UNITS, DTK_YEAR},		/* "year" for ISO input */
	{"yakst", DTZ, POS(40)},	/* Yakutsk Summer Time */
	{"yakt", TZ, POS(36)},		/* Yakutsk Time */
	{"yapt", TZ, POS(40)},		/* Yap Time (Micronesia) */
	{"ydt", DTZ, NEG(32)},		/* Yukon Daylight Time */
	{"yekst", DTZ, POS(24)},	/* Yekaterinburg Summer Time */
	{"yekt", TZ, POS(20)},		/* Yekaterinburg Time */
	{YESTERDAY, RESERV, DTK_YESTERDAY}, /* yesterday midnight */
	{"yst", TZ, NEG(36)},		/* Yukon Standard Time */
	{"z", TZ, POS(0)},			/* time zone tag per ISO-8601 */
	{"zp4", TZ, NEG(16)},		/* UTC +4  hours. */
	{"zp5", TZ, NEG(20)},		/* UTC +5  hours. */
	{"zp6", TZ, NEG(24)},		/* UTC +6  hours. */
	{ZULU, TZ, POS(0)},			/* UTC */
};

static unsigned int szdatetktbl = sizeof datetktbl / sizeof datetktbl[0];

/* Used for SET australian_timezones to override North American ones */
static datetkn australian_datetktbl[] = {
	{"acst", TZ, POS(38)},		/* Cent. Australia */
	{"cst", TZ, POS(42)},		/* Australia Central Std Time */
	{"east", TZ, POS(40)},		/* East Australian Std Time */
	{"est", TZ, POS(40)},		/* Australia Eastern Std Time */
	{"sat", TZ, POS(38)},
};

static unsigned int australian_szdatetktbl = sizeof australian_datetktbl /
sizeof australian_datetktbl[0];

static datetkn deltatktbl[] = {
	/* text, token, lexval */
	{"@", IGNORE_DTF, 0},		/* postgres relative prefix */
	{DAGO, AGO, 0},				/* "ago" indicates negative time offset */
	{"c", UNITS, DTK_CENTURY},	/* "century" relative */
	{"cent", UNITS, DTK_CENTURY},		/* "century" relative */
	{"centuries", UNITS, DTK_CENTURY},	/* "centuries" relative */
	{DCENTURY, UNITS, DTK_CENTURY},		/* "century" relative */
	{"d", UNITS, DTK_DAY},		/* "day" relative */
	{DDAY, UNITS, DTK_DAY},		/* "day" relative */
	{"days", UNITS, DTK_DAY},	/* "days" relative */
	{"dec", UNITS, DTK_DECADE}, /* "decade" relative */
	{DDECADE, UNITS, DTK_DECADE},		/* "decade" relative */
	{"decades", UNITS, DTK_DECADE},		/* "decades" relative */
	{"decs", UNITS, DTK_DECADE},	/* "decades" relative */
	{"h", UNITS, DTK_HOUR},		/* "hour" relative */
	{DHOUR, UNITS, DTK_HOUR},	/* "hour" relative */
	{"hours", UNITS, DTK_HOUR}, /* "hours" relative */
	{"hr", UNITS, DTK_HOUR},	/* "hour" relative */
	{"hrs", UNITS, DTK_HOUR},	/* "hours" relative */
	{INVALID, RESERV, DTK_INVALID},		/* reserved for invalid time */
	{"m", UNITS, DTK_MINUTE},	/* "minute" relative */
	{"microsecon", UNITS, DTK_MICROSEC},		/* "microsecond" relative */
	{"mil", UNITS, DTK_MILLENNIUM},		/* "millennium" relative */
	{"millennia", UNITS, DTK_MILLENNIUM},		/* "millennia" relative */
	{DMILLENNIUM, UNITS, DTK_MILLENNIUM},		/* "millennium" relative */
	{"millisecon", UNITS, DTK_MILLISEC},		/* relative */
	{"mils", UNITS, DTK_MILLENNIUM},	/* "millennia" relative */
	{"min", UNITS, DTK_MINUTE}, /* "minute" relative */
	{"mins", UNITS, DTK_MINUTE},	/* "minutes" relative */
	{DMINUTE, UNITS, DTK_MINUTE},		/* "minute" relative */
	{"minutes", UNITS, DTK_MINUTE},		/* "minutes" relative */
	{"mon", UNITS, DTK_MONTH},	/* "months" relative */
	{"mons", UNITS, DTK_MONTH}, /* "months" relative */
	{DMONTH, UNITS, DTK_MONTH}, /* "month" relative */
	{"months", UNITS, DTK_MONTH},
	{"ms", UNITS, DTK_MILLISEC},
	{"msec", UNITS, DTK_MILLISEC},
	{DMILLISEC, UNITS, DTK_MILLISEC},
	{"mseconds", UNITS, DTK_MILLISEC},
	{"msecs", UNITS, DTK_MILLISEC},
	{"qtr", UNITS, DTK_QUARTER},	/* "quarter" relative */
	{DQUARTER, UNITS, DTK_QUARTER},		/* "quarter" relative */
	{"reltime", IGNORE_DTF, 0}, /* pre-v6.1 "Undefined Reltime" */
	{"s", UNITS, DTK_SECOND},
	{"sec", UNITS, DTK_SECOND},
	{DSECOND, UNITS, DTK_SECOND},
	{"seconds", UNITS, DTK_SECOND},
	{"secs", UNITS, DTK_SECOND},
	{DTIMEZONE, UNITS, DTK_TZ}, /* "timezone" time offset */
	{"timezone_h", UNITS, DTK_TZ_HOUR}, /* timezone hour units */
	{"timezone_m", UNITS, DTK_TZ_MINUTE},		/* timezone minutes units */
	{"undefined", RESERV, DTK_INVALID}, /* pre-v6.1 invalid time */
	{"us", UNITS, DTK_MICROSEC},	/* "microsecond" relative */
	{"usec", UNITS, DTK_MICROSEC},		/* "microsecond" relative */
	{DMICROSEC, UNITS, DTK_MICROSEC},	/* "microsecond" relative */
	{"useconds", UNITS, DTK_MICROSEC},	/* "microseconds" relative */
	{"usecs", UNITS, DTK_MICROSEC},		/* "microseconds" relative */
	{"w", UNITS, DTK_WEEK},		/* "week" relative */
	{DWEEK, UNITS, DTK_WEEK},	/* "week" relative */
	{"weeks", UNITS, DTK_WEEK}, /* "weeks" relative */
	{"y", UNITS, DTK_YEAR},		/* "year" relative */
	{DYEAR, UNITS, DTK_YEAR},	/* "year" relative */
	{"years", UNITS, DTK_YEAR}, /* "years" relative */
	{"yr", UNITS, DTK_YEAR},	/* "year" relative */
	{"yrs", UNITS, DTK_YEAR},	/* "years" relative */
};

static unsigned int szdeltatktbl = sizeof deltatktbl / sizeof deltatktbl[0];

static datetkn    *datecache[MAXDATEFIELDS] = {NULL};

static datetkn    *deltacache[MAXDATEFIELDS] = {NULL};


/*
 * Calendar time to Julian date conversions.
 * Julian date is commonly used in astronomical applications,
 *	since it is numerically accurate and computationally simple.
 * The algorithms here will accurately convert between Julian day
 *	and calendar date for all non-negative Julian days
 *	(i.e. from Nov 24, -4713 on).
 *
 * These routines will be used by other date/time packages
 * - thomas 97/02/25
 *
 * Rewritten to eliminate overflow problems. This now allows the
 * routines to work correctly for all Julian day counts from
 * 0 to 2147483647  (Nov 24, -4713 to Jun 3, 5874898) assuming
 * a 32-bit integer. Longer types should also work to the limits
 * of their precision.
 */

int
date2j(int y, int m, int d)
{
	int			julian;
	int			century;

	if (m > 2) {
		m += 1;
		y += 4800;
	} else {
		m += 13;
		y += 4799;
	}

	century = y/100;
	julian  = y*365 - 32167;
	julian += y/4 - century + century/4;
	julian += 7834*m/256 + d;

	return julian;
}	/* date2j() */

void
j2date(int jd, int *year, int *month, int *day)
{
	unsigned int		julian;
	unsigned int		quad;
	unsigned int		extra;
	int			y;

	julian = jd;
	julian += 32044;
	quad = julian/146097;
	extra = (julian - quad*146097)*4 + 3;
	julian += 60 + quad*3 + extra/146097;
	quad = julian/1461;
	julian -= quad*1461;
	y = julian * 4 / 1461;
	julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366))
		+ 123;
	y += quad*4;
	*year = y - 4800;
	quad = julian * 2141 / 65536;
	*day = julian - 7834*quad/256;
	*month = (quad + 10) % 12 + 1;

	return;
}	/* j2date() */


/*
 * j2day - convert Julian date to day-of-week (0..6 == Sun..Sat)
 *
 * Note: various places use the locution j2day(date - 1) to produce a
 * result according to the convention 0..6 = Mon..Sun.  This is a bit of
 * a crock, but will work as long as the computation here is just a modulo.
 */
int
j2day(int date)
{
	unsigned int day;

	day = date;
	day += 1;
	day %= 7;

	return (int) day;
}	/* j2day() */


/* TrimTrailingZeros()
 * ... resulting from printing numbers with full precision.
 */
static void
TrimTrailingZeros(char *str)
{
	int			len = strlen(str);

#if 0
	/* chop off trailing one to cope with interval rounding */
	if (strcmp((str + len - 4), "0001") == 0)
	{
		len -= 4;
		*(str + len) = '\0';
	}
#endif

	/* chop off trailing zeros... but leave at least 2 fractional digits */
	while ((*(str + len - 1) == '0')
		   && (*(str + len - 3) != '.'))
	{
		len--;
		*(str + len) = '\0';
	}
}


/* ParseDateTime()
 * Break string into tokens based on a date/time context.
 * Several field types are assigned:
 *	DTK_NUMBER - digits and (possibly) a decimal point
 *	DTK_DATE - digits and two delimiters, or digits and text
 *	DTK_TIME - digits, colon delimiters, and possibly a decimal point
 *	DTK_STRING - text (no digits)
 *	DTK_SPECIAL - leading "+" or "-" followed by text
 *	DTK_TZ - leading "+" or "-" followed by digits
 * Note that some field types can hold unexpected items:
 *	DTK_NUMBER can hold date fields (yy.ddd)
 *	DTK_STRING can hold months (January) and time zones (PST)
 *	DTK_DATE can hold Posix time zones (GMT-8)
 */
int
ParseDateTime(char *timestr, char *lowstr,
			  char **field, int *ftype, int maxfields, int *numfields)
{
	int			nf = 0;
	char	   *cp = timestr;
	char	   *lp = lowstr;

	/* outer loop through fields */
	while (*cp != '\0')
	{
		field[nf] = lp;

		/* leading digit? then date or time */
		if (isdigit((unsigned char) *cp))
		{
			*lp++ = *cp++;
			while (isdigit((unsigned char) *cp))
				*lp++ = *cp++;

			/* time field? */
			if (*cp == ':')
			{
				ftype[nf] = DTK_TIME;
				*lp++ = *cp++;
				while (isdigit((unsigned char) *cp) ||
					   (*cp == ':') || (*cp == '.'))
					*lp++ = *cp++;
			}
			/* date field? allow embedded text month */
			else if ((*cp == '-') || (*cp == '/') || (*cp == '.'))
			{
				/* save delimiting character to use later */
				char	   *dp = cp;

				*lp++ = *cp++;
				/* second field is all digits? then no embedded text month */
				if (isdigit((unsigned char) *cp))
				{
					ftype[nf] = ((*dp == '.') ? DTK_NUMBER : DTK_DATE);
					while (isdigit((unsigned char) *cp))
						*lp++ = *cp++;

					/*
					 * insist that the delimiters match to get a
					 * three-field date.
					 */
					if (*cp == *dp)
					{
						ftype[nf] = DTK_DATE;
						*lp++ = *cp++;
						while (isdigit((unsigned char) *cp) || (*cp == *dp))
							*lp++ = *cp++;
					}
				}
				else
				{
					ftype[nf] = DTK_DATE;
					while (isalnum((unsigned char) *cp) || (*cp == *dp))
						*lp++ = tolower((unsigned char) *cp++);
				}
			}

			/*
			 * otherwise, number only and will determine year, month, day,
			 * or concatenated fields later...
			 */
			else
				ftype[nf] = DTK_NUMBER;
		}
		/* Leading decimal point? Then fractional seconds... */
		else if (*cp == '.')
		{
			*lp++ = *cp++;
			while (isdigit((unsigned char) *cp))
				*lp++ = *cp++;

			ftype[nf] = DTK_NUMBER;
		}

		/*
		 * text? then date string, month, day of week, special, or
		 * timezone
		 */
		else if (isalpha((unsigned char) *cp))
		{
			ftype[nf] = DTK_STRING;
			*lp++ = tolower((unsigned char) *cp++);
			while (isalpha((unsigned char) *cp))
				*lp++ = tolower((unsigned char) *cp++);

			/*
			 * Full date string with leading text month? Could also be a
			 * POSIX time zone...
			 */
			if ((*cp == '-') || (*cp == '/') || (*cp == '.'))
			{
				char	   *dp = cp;

				ftype[nf] = DTK_DATE;
				*lp++ = *cp++;
				while (isdigit((unsigned char) *cp) || (*cp == *dp))
					*lp++ = *cp++;
			}
		}
		/* skip leading spaces */
		else if (isspace((unsigned char) *cp))
		{
			cp++;
			continue;
		}
		/* sign? then special or numeric timezone */
		else if ((*cp == '+') || (*cp == '-'))
		{
			*lp++ = *cp++;
			/* soak up leading whitespace */
			while (isspace((unsigned char) *cp))
				cp++;
			/* numeric timezone? */
			if (isdigit((unsigned char) *cp))
			{
				ftype[nf] = DTK_TZ;
				*lp++ = *cp++;
				while (isdigit((unsigned char) *cp) ||
					   (*cp == ':') || (*cp == '.'))
					*lp++ = *cp++;
			}
			/* special? */
			else if (isalpha((unsigned char) *cp))
			{
				ftype[nf] = DTK_SPECIAL;
				*lp++ = tolower((unsigned char) *cp++);
				while (isalpha((unsigned char) *cp))
					*lp++ = tolower((unsigned char) *cp++);
			}
			/* otherwise something wrong... */
			else
				return -1;
		}
		/* ignore punctuation but use as delimiter */
		else if (ispunct((unsigned char) *cp))
		{
			cp++;
			continue;

		}
		/* otherwise, something is not right... */
		else
			return -1;

		/* force in a delimiter after each field */
		*lp++ = '\0';
		nf++;
		if (nf > MAXDATEFIELDS)
			return -1;
	}

	*numfields = nf;

	return 0;
}	/* ParseDateTime() */


/* DecodeDateTime()
 * Interpret previously parsed fields for general date and time.
 * Return 0 if full date, 1 if only time, and -1 if problems.
 *		External format(s):
 *				"<weekday> <month>-<day>-<year> <hour>:<minute>:<second>"
 *				"Fri Feb-7-1997 15:23:27"
 *				"Feb-7-1997 15:23:27"
 *				"2-7-1997 15:23:27"
 *				"1997-2-7 15:23:27"
 *				"1997.038 15:23:27"		(day of year 1-366)
 *		Also supports input in compact time:
 *				"970207 152327"
 *				"97038 152327"
 *				"20011225T040506.789-07"
 *
 * Use the system-provided functions to get the current time zone
 *	if not specified in the input string.
 * If the date is outside the time_t system-supported time range,
 *	then assume UTC time zone. - thomas 1997-05-27
 */
int
DecodeDateTime(char **field, int *ftype, int nf,
			   int *dtype, struct tm * tm, fsec_t *fsec, int *tzp)
{
	int			fmask = 0,
				tmask,
				type;
	int			ptype = 0;		/* "prefix type" for ISO y2001m02d04
								 * format */
	int			i;
	int			val;
	int			mer = HR24;
	int			haveTextMonth = FALSE;
	int			is2digits = FALSE;
	int			bc = FALSE;

	/***
	 * We'll insist on at least all of the date fields, but initialize the
	 * remaining fields in case they are not set later...
	 ***/
	*dtype = DTK_DATE;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;
	/* don't know daylight savings time status apriori */
	tm->tm_isdst = -1;
	if (tzp != NULL)
		*tzp = 0;

	for (i = 0; i < nf; i++)
	{
		switch (ftype[i])
		{
			case DTK_DATE:
				/***
				 * Integral julian day with attached time zone?
				 * All other forms with JD will be separated into
				 * distinct fields, so we handle just this case here.
				 ***/
				if (ptype == DTK_JULIAN)
				{
					char	   *cp;
					int			val;

					if (tzp == NULL)
						return -1;

					val = strtol(field[i], &cp, 10);
					if (*cp != '-')
						return -1;

					j2date(val, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
					/* Get the time zone from the end of the string */
					if (DecodeTimezone(cp, tzp) != 0)
						return -1;

					tmask = DTK_DATE_M | DTK_TIME_M | DTK_M(TZ);
					ptype = 0;
					break;
				}
				/***
				 * Already have a date? Then this might be a POSIX time
				 * zone with an embedded dash (e.g. "PST-3" == "EST") or
				 * a run-together time with trailing time zone (e.g. hhmmss-zz).
				 * - thomas 2001-12-25
				 ***/
				else if (((fmask & DTK_DATE_M) == DTK_DATE_M)
						 || (ptype != 0))
				{
					/* No time zone accepted? Then quit... */
					if (tzp == NULL)
						return -1;

					if (isdigit((unsigned char) *field[i]) || ptype != 0)
					{
						char	   *cp;

						if (ptype != 0)
						{
							/* Sanity check; should not fail this test */
							if (ptype != DTK_TIME)
								return -1;
							ptype = 0;
						}

						/*
						 * Starts with a digit but we already have a time
						 * field? Then we are in trouble with a date and
						 * time already...
						 */
						if ((fmask & DTK_TIME_M) == DTK_TIME_M)
							return -1;

						if ((cp = strchr(field[i], '-')) == NULL)
							return -1;

						/* Get the time zone from the end of the string */
						if (DecodeTimezone(cp, tzp) != 0)
							return -1;
						*cp = '\0';

						/*
						 * Then read the rest of the field as a
						 * concatenated time
						 */
						if ((ftype[i] = DecodeNumberField(strlen(field[i]), field[i], fmask,
									  &tmask, tm, fsec, &is2digits)) < 0)
							return -1;

						/*
						 * modify tmask after returning from
						 * DecodeNumberField()
						 */
						tmask |= DTK_M(TZ);
					}
					else
					{
						if (DecodePosixTimezone(field[i], tzp) != 0)
							return -1;

						ftype[i] = DTK_TZ;
						tmask = DTK_M(TZ);
					}
				}
				else if (DecodeDate(field[i], fmask, &tmask, tm) != 0)
					return -1;
				break;

			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;

				/*
				 * Check upper limit on hours; other limits checked in
				 * DecodeTime()
				 */
				if (tm->tm_hour > 23)
					return -1;
				break;

			case DTK_TZ:
				{
					int			tz;

					if (tzp == NULL)
						return -1;

					if (DecodeTimezone(field[i], &tz) != 0)
						return -1;

					/*
					 * Already have a time zone? Then maybe this is the
					 * second field of a POSIX time: EST+3 (equivalent to
					 * PST)
					 */
					if ((i > 0) && ((fmask & DTK_M(TZ)) != 0)
						&& (ftype[i - 1] == DTK_TZ)
						&& (isalpha((unsigned char) *field[i - 1])))
					{
						*tzp -= tz;
						tmask = 0;
					}
					else
					{
						*tzp = tz;
						tmask = DTK_M(TZ);
					}
				}
				break;

			case DTK_NUMBER:

				/*
				 * Was this an "ISO date" with embedded field labels? An
				 * example is "y2001m02d04" - thomas 2001-02-04
				 */
				if (ptype != 0)
				{
					char	   *cp;
					int			val;

					val = strtol(field[i], &cp, 10);

					/*
					 * only a few kinds are allowed to have an embedded
					 * decimal
					 */
					if (*cp == '.')
						switch (ptype)
						{
							case DTK_JULIAN:
							case DTK_TIME:
							case DTK_SECOND:
								break;
							default:
								return 1;
								break;
						}
					else if (*cp != '\0')
						return -1;

					switch (ptype)
					{
						case DTK_YEAR:
							tm->tm_year = val;
							tmask = DTK_M(YEAR);
							break;

						case DTK_MONTH:

							/*
							 * already have a month and hour? then assume
							 * minutes
							 */
							if (((fmask & DTK_M(MONTH)) != 0)
								&& ((fmask & DTK_M(HOUR)) != 0))
							{
								tm->tm_min = val;
								tmask = DTK_M(MINUTE);
							}
							else
							{
								tm->tm_mon = val;
								tmask = DTK_M(MONTH);
							}
							break;

						case DTK_DAY:
							tm->tm_mday = val;
							tmask = DTK_M(DAY);
							break;

						case DTK_HOUR:
							tm->tm_hour = val;
							tmask = DTK_M(HOUR);
							break;

						case DTK_MINUTE:
							tm->tm_min = val;
							tmask = DTK_M(MINUTE);
							break;

						case DTK_SECOND:
							tm->tm_sec = val;
							tmask = DTK_M(SECOND);
							if (*cp == '.')
							{
								double		frac;

								frac = strtod(cp, &cp);
								if (*cp != '\0')
									return -1;
#ifdef HAVE_INT64_TIMESTAMP
								*fsec = frac * 1000000;
#else
								*fsec = frac;
#endif
							}
							break;

						case DTK_TZ:
							tmask = DTK_M(TZ);
							if (DecodeTimezone(field[i], tzp) != 0)
								return -1;
							break;

						case DTK_JULIAN:
							/***
							 * previous field was a label for "julian date"?
							 ***/
							tmask = DTK_DATE_M;
							j2date(val, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
							/* fractional Julian Day? */
							if (*cp == '.')
							{
								double		time;

								time = strtod(cp, &cp);
								if (*cp != '\0')
									return -1;

								tmask |= DTK_TIME_M;
#ifdef HAVE_INT64_TIMESTAMP
								dt2time((time * 86400000000), &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
#else
								dt2time((time * 86400), &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
#endif
							}
							break;

						case DTK_TIME:
							/* previous field was "t" for ISO time */
							if ((ftype[i] = DecodeNumberField(strlen(field[i]), field[i], (fmask | DTK_DATE_M),
									  &tmask, tm, fsec, &is2digits)) < 0)
								return -1;

							if (tmask != DTK_TIME_M)
								return -1;
							break;

						default:
							return -1;
							break;
					}

					ptype = 0;
					*dtype = DTK_DATE;
				}
				else
				{
					char	   *cp;
					int			flen;

					flen = strlen(field[i]);
					cp = strchr(field[i], '.');

					/* Embedded decimal and no date yet? */
					if ((cp != NULL) && !(fmask & DTK_DATE_M))
					{
						if (DecodeDate(field[i], fmask, &tmask, tm) != 0)
							return -1;
					}
					/* embedded decimal and several digits before? */
					else if ((cp != NULL) && ((flen - strlen(cp)) > 2))
					{
						/*
						 * Interpret as a concatenated date or time Set
						 * the type field to allow decoding other fields
						 * later. Example: 20011223 or 040506
						 */
						if ((ftype[i] = DecodeNumberField(flen, field[i], fmask,
									  &tmask, tm, fsec, &is2digits)) < 0)
							return -1;
					}
					else if (flen > 4)
					{
						if ((ftype[i] = DecodeNumberField(flen, field[i], fmask,
									  &tmask, tm, fsec, &is2digits)) < 0)
							return -1;
					}
					/* otherwise it is a single date/time field... */
					else if (DecodeNumber(flen, field[i], fmask,
									  &tmask, tm, fsec, &is2digits) != 0)
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeSpecial(i, field[i], &val);
				if (type == IGNORE_DTF)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
						switch (val)
						{
							case DTK_CURRENT:
								elog(ERROR, "'CURRENT' is no longer supported");
								return -1;
								break;

							case DTK_NOW:
								tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								GetCurrentTimeUsec(tm, fsec, tzp);
								break;

							case DTK_YESTERDAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentDateTime(tm);
								j2date((date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - 1),
									   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_TODAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentDateTime(tm);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_TOMORROW:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentDateTime(tm);
								j2date((date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + 1),
									   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								if (tzp != NULL)
									*tzp = 0;
								break;

							default:
								*dtype = val;
						}

						break;

					case MONTH:

						/*
						 * already have a (numeric) month? then see if we
						 * can substitute...
						 */
						if ((fmask & DTK_M(MONTH)) && (!haveTextMonth)
							&& (!(fmask & DTK_M(DAY)))
							&& ((tm->tm_mon >= 1) && (tm->tm_mon <= 31)))
						{
							tm->tm_mday = tm->tm_mon;
							tmask = DTK_M(DAY);
						}
						haveTextMonth = TRUE;
						tm->tm_mon = val;
						break;

					case DTZMOD:

						/*
						 * daylight savings time modifier (solves "MET
						 * DST" syntax)
						 */
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp += val * 60;
						break;

					case DTZ:

						/*
						 * set mask for TZ here _or_ check for DTZ later
						 * when getting default timezone
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case IGNORE_DTF:
						break;

					case AMPM:
						mer = val;
						break;

					case ADBC:
						bc = (val == BC);
						break;

					case DOW:
						tm->tm_wday = val;
						break;

					case UNITS:
						tmask = 0;
						ptype = val;
						break;

					case ISOTIME:

						/*
						 * This is a filler field "t" indicating that the
						 * next field is time. Try to verify that this is
						 * sensible.
						 */
						tmask = 0;

						/* No preceeding date? Then quit... */
						if ((fmask & DTK_DATE_M) != DTK_DATE_M)
							return -1;

						/***
						 * We will need one of the following fields:
						 *	DTK_NUMBER should be hhmmss.fff
						 *	DTK_TIME should be hh:mm:ss.fff
						 *	DTK_DATE should be hhmmss-zz
						 ***/
						if ((i >= (nf - 1))
							|| ((ftype[i + 1] != DTK_NUMBER)
								&& (ftype[i + 1] != DTK_TIME)
								&& (ftype[i + 1] != DTK_DATE)))
							return -1;

						ptype = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	/* there is no year zero in AD/BC notation; i.e. "1 BC" == year 0 */
	if (bc)
	{
		if (tm->tm_year > 0)
			tm->tm_year = -(tm->tm_year - 1);
		else
			elog(ERROR, "Inconsistent use of year %04d and 'BC'", tm->tm_year);
	}
	else if (is2digits)
	{
		if (tm->tm_year < 70)
			tm->tm_year += 2000;
		else if (tm->tm_year < 100)
			tm->tm_year += 1900;
	}

	if ((mer != HR24) && (tm->tm_hour > 12))
		return -1;
	if ((mer == AM) && (tm->tm_hour == 12))
		tm->tm_hour = 0;
	else if ((mer == PM) && (tm->tm_hour != 12))
		tm->tm_hour += 12;

	/* do additional checking for full date specs... */
	if (*dtype == DTK_DATE)
	{
		if ((fmask & DTK_DATE_M) != DTK_DATE_M)
			return ((fmask & DTK_TIME_M) == DTK_TIME_M) ? 1 : -1;

		/*
		 * check for valid day of month, now that we know for sure the
		 * month and year...
		 */
		if ((tm->tm_mday < 1)
		 || (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]))
			return -1;

		/* timezone not specified? then find local timezone if possible */
		if (((fmask & DTK_DATE_M) == DTK_DATE_M)
			&& (tzp != NULL) && (!(fmask & DTK_M(TZ))))
		{
			/*
			 * daylight savings time modifier but no standard timezone?
			 * then error
			 */
			if (fmask & DTK_M(DTZMOD))
				return -1;

			*tzp = DetermineLocalTimeZone(tm);
		}
	}

	return 0;
}	/* DecodeDateTime() */


/* DetermineLocalTimeZone()
 *
 * Given a struct tm in which tm_year, tm_mon, tm_mday, tm_hour, tm_min, and
 * tm_sec fields are set, attempt to determine the applicable local zone
 * (ie, regular or daylight-savings time) at that time.  Set the struct tm's
 * tm_isdst field accordingly, and return the actual timezone offset.
 *
 * This subroutine exists to centralize uses of mktime() and defend against
 * mktime() bugs/restrictions on various platforms.  This should be
 * the *only* call of mktime() in the backend.
 */
int
DetermineLocalTimeZone(struct tm * tm)
{
	int			tz;

	if (HasCTZSet)
	{
		tm->tm_isdst = 0;		/* for lack of a better idea */
		tz = CTimeZone;
	}
	else if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
	{
#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)

		/*
		 * Some buggy mktime() implementations may change the
		 * year/month/day when given a time right at a DST boundary.  To
		 * prevent corruption of the caller's data, give mktime() a
		 * copy...
		 */
		struct tm	tt,
				   *tmp = &tt;

		*tmp = *tm;
		/* change to Unix conventions for year/month */
		tmp->tm_year -= 1900;
		tmp->tm_mon -= 1;

		/* indicate timezone unknown */
		tmp->tm_isdst = -1;

		if (mktime(tmp) != ((time_t) -1) &&
			tmp->tm_isdst >= 0)
		{
			/* mktime() succeeded, trust its result */
			tm->tm_isdst = tmp->tm_isdst;

#if defined(HAVE_TM_ZONE)
			/* tm_gmtoff is Sun/DEC-ism */
			tz = -(tmp->tm_gmtoff);
#elif defined(HAVE_INT_TIMEZONE)
			tz = ((tmp->tm_isdst > 0) ? (TIMEZONE_GLOBAL - 3600) : TIMEZONE_GLOBAL);
#endif   /* HAVE_INT_TIMEZONE */
		}
		else
		{
			/*
			 * We have a buggy (not to say deliberately brain damaged)
			 * mktime().  Work around it by using localtime() instead.
			 *
			 * First, generate the time_t value corresponding to the given
			 * y/m/d/h/m/s taken as GMT time.  This will not overflow (at
			 * least not for time_t taken as signed) because of the range
			 * check we did above.
			 */
			long		day,
						mysec,
						locsec,
						delta1,
						delta2;
			time_t		mytime;

			day = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - UNIX_EPOCH_JDATE;
			mysec = tm->tm_sec + (tm->tm_min + (day * 24 + tm->tm_hour) * 60) * 60;
			mytime = (time_t) mysec;

			/*
			 * Use localtime to convert that time_t to broken-down time,
			 * and reassemble to get a representation of local time.
			 */
			tmp = localtime(&mytime);
			day = date2j(tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday) -
				UNIX_EPOCH_JDATE;
			locsec = tmp->tm_sec + (tmp->tm_min + (day * 24 + tmp->tm_hour) * 60) * 60;

			/*
			 * The local time offset corresponding to that GMT time is now
			 * computable as mysec - locsec.
			 */
			delta1 = mysec - locsec;

			/*
			 * However, if that GMT time and the local time we are
			 * actually interested in are on opposite sides of a
			 * daylight-savings-time transition, then this is not the time
			 * offset we want.	So, adjust the time_t to be what we think
			 * the GMT time corresponding to our target local time is, and
			 * repeat the localtime() call and delta calculation.  We may
			 * have to do it twice before we have a trustworthy delta.
			 *
			 * Note: think not to put a loop here, since if we've been given
			 * an "impossible" local time (in the gap during a
			 * spring-forward transition) we'd never get out of the loop.
			 * Twice is enough to give the behavior we want, which is that
			 * "impossible" times are taken as standard time, while at a
			 * fall-back boundary ambiguous times are also taken as
			 * standard.
			 */
			mysec += delta1;
			mytime = (time_t) mysec;
			tmp = localtime(&mytime);
			day = date2j(tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday) -
				UNIX_EPOCH_JDATE;
			locsec = tmp->tm_sec + (tmp->tm_min + (day * 24 + tmp->tm_hour) * 60) * 60;
			delta2 = mysec - locsec;
			if (delta2 != delta1)
			{
				mysec += (delta2 - delta1);
				mytime = (time_t) mysec;
				tmp = localtime(&mytime);
				day = date2j(tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday) -
					UNIX_EPOCH_JDATE;
				locsec = tmp->tm_sec + (tmp->tm_min + (day * 24 + tmp->tm_hour) * 60) * 60;
				delta2 = mysec - locsec;
			}
			tm->tm_isdst = tmp->tm_isdst;
			tz = (int) delta2;
		}
#else							/* not (HAVE_TM_ZONE || HAVE_INT_TIMEZONE) */
		/* Assume UTC if no system timezone info available */
		tm->tm_isdst = 0;
		tz = 0;
#endif
	}
	else
	{
		/* Given date is out of range, so assume UTC */
		tm->tm_isdst = 0;
		tz = 0;
	}

	return tz;
}


/* DecodeTimeOnly()
 * Interpret parsed string as time fields only.
 * Note that support for time zone is here for
 * SQL92 TIME WITH TIME ZONE, but it reveals
 * bogosity with SQL92 date/time standards, since
 * we must infer a time zone from current time.
 * - thomas 2000-03-10
 * Allow specifying date to get a better time zone,
 * if time zones are allowed. - thomas 2001-12-26
 */
int
DecodeTimeOnly(char **field, int *ftype, int nf,
			   int *dtype, struct tm * tm, fsec_t *fsec, int *tzp)
{
	int			fmask = 0,
				tmask,
				type;
	int			ptype = 0;		/* "prefix type" for ISO h04mm05s06 format */
	int			i;
	int			val;
	int			is2digits = FALSE;
	int			mer = HR24;

	*dtype = DTK_TIME;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;
	/* don't know daylight savings time status apriori */
	tm->tm_isdst = -1;

	if (tzp != NULL)
		*tzp = 0;

	for (i = 0; i < nf; i++)
	{
		switch (ftype[i])
		{
			case DTK_DATE:

				/*
				 * Time zone not allowed? Then should not accept dates or
				 * time zones no matter what else!
				 */
				if (tzp == NULL)
					return -1;

				/* Under limited circumstances, we will accept a date... */
				if ((i == 0) && (nf >= 2)
					&& ((ftype[nf - 1] == DTK_DATE)
						|| (ftype[1] == DTK_TIME)))
				{
					if (DecodeDate(field[i], fmask, &tmask, tm) != 0)
						return -1;
				}
				/* otherwise, this is a time and/or time zone */
				else
				{
					if (isdigit((unsigned char) *field[i]))
					{
						char	   *cp;

						/*
						 * Starts with a digit but we already have a time
						 * field? Then we are in trouble with time
						 * already...
						 */
						if ((fmask & DTK_TIME_M) == DTK_TIME_M)
							return -1;

						/*
						 * Should not get here and fail. Sanity check
						 * only...
						 */
						if ((cp = strchr(field[i], '-')) == NULL)
							return -1;

						/* Get the time zone from the end of the string */
						if (DecodeTimezone(cp, tzp) != 0)
							return -1;
						*cp = '\0';

						/*
						 * Then read the rest of the field as a
						 * concatenated time
						 */
						if ((ftype[i] = DecodeNumberField(strlen(field[i]), field[i], (fmask | DTK_DATE_M),
									  &tmask, tm, fsec, &is2digits)) < 0)
							return -1;

						tmask |= DTK_M(TZ);
					}
					else
					{
						if (DecodePosixTimezone(field[i], tzp) != 0)
							return -1;

						ftype[i] = DTK_TZ;
						tmask = DTK_M(TZ);
					}
				}
				break;

			case DTK_TIME:
				if (DecodeTime(field[i], (fmask | DTK_DATE_M), &tmask, tm, fsec) != 0)
					return -1;
				break;

			case DTK_TZ:
				if (tzp == NULL)
					return -1;

				{
					int			tz;

					if (DecodeTimezone(field[i], &tz) != 0)
						return -1;

					/*
					 * Already have a time zone? Then maybe this is the
					 * second field of a POSIX time: EST+3 (equivalent to
					 * PST)
					 */
					if ((i > 0) && ((fmask & DTK_M(TZ)) != 0)
						&& (ftype[i - 1] == DTK_TZ) && (isalpha((unsigned char) *field[i - 1])))
					{
						*tzp -= tz;
						tmask = 0;
					}
					else
					{
						*tzp = tz;
						tmask = DTK_M(TZ);
					}
				}
				break;

			case DTK_NUMBER:

				/*
				 * Was this an "ISO time" with embedded field labels? An
				 * example is "h04m05s06" - thomas 2001-02-04
				 */
				if (ptype != 0)
				{
					char	   *cp;
					int			val;

					/* Only accept a date under limited circumstances */
					switch (ptype)
					{
						case DTK_JULIAN:
						case DTK_YEAR:
						case DTK_MONTH:
						case DTK_DAY:
							if (tzp == NULL)
								return -1;
						default:
							break;
					}

					val = strtol(field[i], &cp, 10);

					/*
					 * only a few kinds are allowed to have an embedded
					 * decimal
					 */
					if (*cp == '.')
						switch (ptype)
						{
							case DTK_JULIAN:
							case DTK_TIME:
							case DTK_SECOND:
								break;
							default:
								return 1;
								break;
						}
					else if (*cp != '\0')
						return -1;

					switch (ptype)
					{
						case DTK_YEAR:
							tm->tm_year = val;
							tmask = DTK_M(YEAR);
							break;

						case DTK_MONTH:

							/*
							 * already have a month and hour? then assume
							 * minutes
							 */
							if (((fmask & DTK_M(MONTH)) != 0)
								&& ((fmask & DTK_M(HOUR)) != 0))
							{
								tm->tm_min = val;
								tmask = DTK_M(MINUTE);
							}
							else
							{
								tm->tm_mon = val;
								tmask = DTK_M(MONTH);
							}
							break;

						case DTK_DAY:
							tm->tm_mday = val;
							tmask = DTK_M(DAY);
							break;

						case DTK_HOUR:
							tm->tm_hour = val;
							tmask = DTK_M(HOUR);
							break;

						case DTK_MINUTE:
							tm->tm_min = val;
							tmask = DTK_M(MINUTE);
							break;

						case DTK_SECOND:
							tm->tm_sec = val;
							tmask = DTK_M(SECOND);
							if (*cp == '.')
							{
								*fsec = strtod(cp, &cp);
								if (*cp != '\0')
									return -1;
							}
							break;

						case DTK_TZ:
							tmask = DTK_M(TZ);
							if (DecodeTimezone(field[i], tzp) != 0)
								return -1;
							break;

						case DTK_JULIAN:
							/***
							 * previous field was a label for "julian date"?
							 ***/
							tmask = DTK_DATE_M;
							j2date(val, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
							if (*cp == '.')
							{
								double		time;

								time = strtod(cp, &cp);
								if (*cp != '\0')
									return -1;

								tmask |= DTK_TIME_M;
#ifdef HAVE_INT64_TIMESTAMP
								dt2time((time * 86400000000), &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
#else
								dt2time((time * 86400), &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
#endif
							}
							break;

						case DTK_TIME:
							/* previous field was "t" for ISO time */
							if ((ftype[i] = DecodeNumberField(strlen(field[i]), field[i], (fmask | DTK_DATE_M),
									  &tmask, tm, fsec, &is2digits)) < 0)
								return -1;

							if (tmask != DTK_TIME_M)
								return -1;
							break;

						default:
							return -1;
							break;
					}

					ptype = 0;
					*dtype = DTK_DATE;
				}
				else
				{
					char	   *cp;
					int			flen;

					flen = strlen(field[i]);
					cp = strchr(field[i], '.');

					/* Embedded decimal? */
					if (cp != NULL)
					{
						/*
						 * Under limited circumstances, we will accept a
						 * date...
						 */
						if ((i == 0) && ((nf >= 2) && (ftype[nf - 1] == DTK_DATE)))
						{
							if (DecodeDate(field[i], fmask, &tmask, tm) != 0)
								return -1;
						}
						/* embedded decimal and several digits before? */
						else if ((flen - strlen(cp)) > 2)
						{
							/*
							 * Interpret as a concatenated date or time
							 * Set the type field to allow decoding other
							 * fields later. Example: 20011223 or 040506
							 */
							if ((ftype[i] = DecodeNumberField(flen, field[i], fmask,
									  &tmask, tm, fsec, &is2digits)) < 0)
								return -1;
						}
						else
							return -1;
					}
					else if (flen > 4)
					{
						if ((ftype[i] = DecodeNumberField(flen, field[i], fmask,
									  &tmask, tm, fsec, &is2digits)) < 0)
							return -1;
					}
					/* otherwise it is a single date/time field... */
					else if (DecodeNumber(flen, field[i], fmask,
									  &tmask, tm, fsec, &is2digits) != 0)
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeSpecial(i, field[i], &val);
				if (type == IGNORE_DTF)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
						switch (val)
						{
							case DTK_CURRENT:
								elog(ERROR, "'CURRENT' is no longer supported");
								return -1;
								break;

							case DTK_NOW:
								tmask = DTK_TIME_M;
								*dtype = DTK_TIME;
								GetCurrentTimeUsec(tm, fsec, NULL);
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_TIME;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								tm->tm_isdst = 0;
								break;

							default:
								return -1;
						}

						break;

					case DTZMOD:

						/*
						 * daylight savings time modifier (solves "MET
						 * DST" syntax)
						 */
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp += val * 60;
						break;

					case DTZ:

						/*
						 * set mask for TZ here _or_ check for DTZ later
						 * when getting default timezone
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case IGNORE_DTF:
						break;

					case AMPM:
						mer = val;
						break;

					case UNITS:
						tmask = 0;
						ptype = val;
						break;

					case ISOTIME:
						tmask = 0;

						/***
						 * We will need one of the following fields:
						 *	DTK_NUMBER should be hhmmss.fff
						 *	DTK_TIME should be hh:mm:ss.fff
						 *	DTK_DATE should be hhmmss-zz
						 ***/
						if ((i >= (nf - 1))
							|| ((ftype[i + 1] != DTK_NUMBER)
								&& (ftype[i + 1] != DTK_TIME)
								&& (ftype[i + 1] != DTK_DATE)))
							return -1;

						ptype = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	if ((mer != HR24) && (tm->tm_hour > 12))
		return -1;
	if ((mer == AM) && (tm->tm_hour == 12))
		tm->tm_hour = 0;
	else if ((mer == PM) && (tm->tm_hour != 12))
		tm->tm_hour += 12;

#ifdef HAVE_INT64_TIMESTAMP
	if ((tm->tm_hour < 0) || (tm->tm_hour > 23)
		|| (tm->tm_min < 0) || (tm->tm_min > 59)
		|| (tm->tm_sec < 0) || (tm->tm_sec > 60)
		|| (*fsec < INT64CONST(0)) || (*fsec >= INT64CONST(1000000)))
		return -1;
#else
	if ((tm->tm_hour < 0) || (tm->tm_hour > 23)
		|| (tm->tm_min < 0) || (tm->tm_min > 59)
		|| (tm->tm_sec < 0) || (tm->tm_sec > 60)
		|| (*fsec < 0) || (*fsec >= 1))
		return -1;
#endif

	if ((fmask & DTK_TIME_M) != DTK_TIME_M)
		return -1;

	/* timezone not specified? then find local timezone if possible */
	if ((tzp != NULL) && (!(fmask & DTK_M(TZ))))
	{
		struct tm	tt,
				   *tmp = &tt;

		/*
		 * daylight savings time modifier but no standard timezone? then
		 * error
		 */
		if (fmask & DTK_M(DTZMOD))
			return -1;

		if ((fmask & DTK_DATE_M) == 0)
			GetCurrentDateTime(tmp);
		else
		{
			tmp->tm_year = tm->tm_year;
			tmp->tm_mon = tm->tm_mon;
			tmp->tm_mday = tm->tm_mday;
		}
		tmp->tm_hour = tm->tm_hour;
		tmp->tm_min = tm->tm_min;
		tmp->tm_sec = tm->tm_sec;
		*tzp = DetermineLocalTimeZone(tmp);
		tm->tm_isdst = tmp->tm_isdst;
	}

	return 0;
}	/* DecodeTimeOnly() */

/* DecodeDate()
 * Decode date string which includes delimiters.
 * Insist on a complete set of fields.
 */
static int
DecodeDate(char *str, int fmask, int *tmask, struct tm * tm)
{
	fsec_t		fsec;

	int			nf = 0;
	int			i,
				len;
	int			bc = FALSE;
	int			is2digits = FALSE;
	int			type,
				val,
				dmask = 0;
	char	   *field[MAXDATEFIELDS];

	/* parse this string... */
	while ((*str != '\0') && (nf < MAXDATEFIELDS))
	{
		/* skip field separators */
		while (!isalnum((unsigned char) *str))
			str++;

		field[nf] = str;
		if (isdigit((unsigned char) *str))
		{
			while (isdigit((unsigned char) *str))
				str++;
		}
		else if (isalpha((unsigned char) *str))
		{
			while (isalpha((unsigned char) *str))
				str++;
		}

		/* Just get rid of any non-digit, non-alpha characters... */
		if (*str != '\0')
			*str++ = '\0';
		nf++;
	}

#if 0
	/* don't allow too many fields */
	if (nf > 3)
		return -1;
#endif

	*tmask = 0;

	/* look first for text fields, since that will be unambiguous month */
	for (i = 0; i < nf; i++)
	{
		if (isalpha((unsigned char) *field[i]))
		{
			type = DecodeSpecial(i, field[i], &val);
			if (type == IGNORE_DTF)
				continue;

			dmask = DTK_M(type);
			switch (type)
			{
				case MONTH:
					tm->tm_mon = val;
					break;

				case ADBC:
					bc = (val == BC);
					break;

				default:
					return -1;
			}
			if (fmask & dmask)
				return -1;

			fmask |= dmask;
			*tmask |= dmask;

			/* mark this field as being completed */
			field[i] = NULL;
		}
	}

	/* now pick up remaining numeric fields */
	for (i = 0; i < nf; i++)
	{
		if (field[i] == NULL)
			continue;

		if ((len = strlen(field[i])) <= 0)
			return -1;

		if (DecodeNumber(len, field[i], fmask, &dmask, tm, &fsec, &is2digits) != 0)
			return -1;

		if (fmask & dmask)
			return -1;

		fmask |= dmask;
		*tmask |= dmask;
	}

	if ((fmask & ~(DTK_M(DOY) | DTK_M(TZ))) != DTK_DATE_M)
		return -1;

	/* there is no year zero in AD/BC notation; i.e. "1 BC" == year 0 */
	if (bc)
	{
		if (tm->tm_year > 0)
			tm->tm_year = -(tm->tm_year - 1);
		else
			elog(ERROR, "Inconsistent use of year %04d and 'BC'", tm->tm_year);
	}
	else if (is2digits)
	{
		if (tm->tm_year < 70)
			tm->tm_year += 2000;
		else if (tm->tm_year < 100)
			tm->tm_year += 1900;
	}

	return 0;
}	/* DecodeDate() */


/* DecodeTime()
 * Decode time string which includes delimiters.
 * Only check the lower limit on hours, since this same code
 *	can be used to represent time spans.
 */
static int
DecodeTime(char *str, int fmask, int *tmask, struct tm * tm, fsec_t *fsec)
{
	char	   *cp;

	*tmask = DTK_TIME_M;

	tm->tm_hour = strtol(str, &cp, 10);
	if (*cp != ':')
		return -1;
	str = cp + 1;
	tm->tm_min = strtol(str, &cp, 10);
	if (*cp == '\0')
	{
		tm->tm_sec = 0;
		*fsec = 0;
	}
	else if (*cp != ':')
		return -1;
	else
	{
		str = cp + 1;
		tm->tm_sec = strtol(str, &cp, 10);
		if (*cp == '\0')
			*fsec = 0;
		else if (*cp == '.')
		{
#ifdef HAVE_INT64_TIMESTAMP
			char		fstr[MAXDATELEN + 1];

			/*
			 * OK, we have at most six digits to work with. Let's
			 * construct a string and then do the conversion to an
			 * integer.
			 */
			strncpy(fstr, (cp + 1), 7);
			strcpy((fstr + strlen(fstr)), "000000");
			*(fstr + 6) = '\0';
			*fsec = strtol(fstr, &cp, 10);
#else
			str = cp;
			*fsec = strtod(str, &cp);
#endif
			if (*cp != '\0')
				return -1;
		}
		else
			return -1;
	}

	/* do a sanity check */
#ifdef HAVE_INT64_TIMESTAMP
	if ((tm->tm_hour < 0)
		|| (tm->tm_min < 0) || (tm->tm_min > 59)
		|| (tm->tm_sec < 0) || (tm->tm_sec > 60)
		|| (*fsec < INT64CONST(0)) || (*fsec >= INT64CONST(1000000)))
		return -1;
#else
	if ((tm->tm_hour < 0)
		|| (tm->tm_min < 0) || (tm->tm_min > 59)
		|| (tm->tm_sec < 0) || (tm->tm_sec > 60)
		|| (*fsec < 0) || (*fsec >= 1))
		return -1;
#endif

	return 0;
}	/* DecodeTime() */


/* DecodeNumber()
 * Interpret plain numeric field as a date value in context.
 */
static int
DecodeNumber(int flen, char *str, int fmask,
			 int *tmask, struct tm * tm, fsec_t *fsec, int *is2digits)
{
	int			val;
	char	   *cp;

	*tmask = 0;

	val = strtol(str, &cp, 10);
	if (cp == str)
		return -1;

	if (*cp == '.')
	{
		/*
		 * More than two digits? Then could be a date or a run-together
		 * time: 2001.360 20011225 040506.789
		 */
		if ((cp - str) > 2)
			return DecodeNumberField(flen, str, (fmask | DTK_DATE_M),
									 tmask, tm, fsec, is2digits);

		*fsec = strtod(cp, &cp);
		if (*cp != '\0')
			return -1;
	}
	else if (*cp != '\0')
		return -1;

	/* Special case day of year? */
	if ((flen == 3) && (fmask & DTK_M(YEAR))
		&& ((val >= 1) && (val <= 366)))
	{
		*tmask = (DTK_M(DOY) | DTK_M(MONTH) | DTK_M(DAY));
		tm->tm_yday = val;
		j2date(date2j(tm->tm_year, 1, 1) + tm->tm_yday - 1,
			   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	}

	/***
	 * Enough digits to be unequivocal year? Used to test for 4 digits or
	 * more, but we now test first for a three-digit doy so anything
	 * bigger than two digits had better be an explicit year.
	 * - thomas 1999-01-09
	 * Back to requiring a 4 digit year. We accept a two digit
	 * year farther down. - thomas 2000-03-28
	 ***/
	else if (flen >= 4)
	{
		*tmask = DTK_M(YEAR);

		/* already have a year? then see if we can substitute... */
		if ((fmask & DTK_M(YEAR)) && (!(fmask & DTK_M(DAY)))
			&& ((tm->tm_year >= 1) && (tm->tm_year <= 31)))
		{
			tm->tm_mday = tm->tm_year;
			*tmask = DTK_M(DAY);
		}

		tm->tm_year = val;
	}

	/* already have year? then could be month */
	else if ((fmask & DTK_M(YEAR)) && (!(fmask & DTK_M(MONTH)))
			 && ((val >= 1) && (val <= 12)))
	{
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;
	}
	/* no year and EuroDates enabled? then could be day */
	else if ((EuroDates || (fmask & DTK_M(MONTH)))
			 && (!(fmask & DTK_M(YEAR)) && !(fmask & DTK_M(DAY)))
			 && ((val >= 1) && (val <= 31)))
	{
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;
	}
	else if ((!(fmask & DTK_M(MONTH)))
			 && ((val >= 1) && (val <= 12)))
	{
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;
	}
	else if ((!(fmask & DTK_M(DAY)))
			 && ((val >= 1) && (val <= 31)))
	{
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;
	}

	/*
	 * Check for 2 or 4 or more digits, but currently we reach here only
	 * if two digits. - thomas 2000-03-28
	 */
	else if (!(fmask & DTK_M(YEAR))
			 && ((flen >= 4) || (flen == 2)))
	{
		*tmask = DTK_M(YEAR);
		tm->tm_year = val;

		/* adjust ONLY if exactly two digits... */
		*is2digits = (flen == 2);
	}
	else
		return -1;

	return 0;
}	/* DecodeNumber() */


/* DecodeNumberField()
 * Interpret numeric string as a concatenated date or time field.
 * Use the context of previously decoded fields to help with
 * the interpretation.
 */
static int
DecodeNumberField(int len, char *str, int fmask,
				int *tmask, struct tm * tm, fsec_t *fsec, int *is2digits)
{
	char	   *cp;

	/*
	 * Have a decimal point? Then this is a date or something with a
	 * seconds field...
	 */
	if ((cp = strchr(str, '.')) != NULL)
	{
#ifdef HAVE_INT64_TIMESTAMP
		char		fstr[MAXDATELEN + 1];

		/*
		 * OK, we have at most six digits to care about. Let's construct a
		 * string and then do the conversion to an integer.
		 */
		strcpy(fstr, (cp + 1));
		strcpy((fstr + strlen(fstr)), "000000");
		*(fstr + 6) = '\0';
		*fsec = strtol(fstr, NULL, 10);
#else
		*fsec = strtod(cp, NULL);
#endif
		*cp = '\0';
		len = strlen(str);
	}
	/* No decimal point and no complete date yet? */
	else if ((fmask & DTK_DATE_M) != DTK_DATE_M)
	{
		/* yyyymmdd? */
		if (len == 8)
		{
			*tmask = DTK_DATE_M;

			tm->tm_mday = atoi(str + 6);
			*(str + 6) = '\0';
			tm->tm_mon = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_year = atoi(str + 0);

			return DTK_DATE;
		}
		/* yymmdd? */
		else if (len == 6)
		{
			*tmask = DTK_DATE_M;
			tm->tm_mday = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_mon = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_year = atoi(str + 0);
			*is2digits = TRUE;

			return DTK_DATE;
		}
		/* yyddd? */
		else if (len == 5)
		{
			*tmask = DTK_DATE_M;
			tm->tm_mday = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_mon = 1;
			tm->tm_year = atoi(str + 0);
			*is2digits = TRUE;

			return DTK_DATE;
		}
	}

	/* not all time fields are specified? */
	if ((fmask & DTK_TIME_M) != DTK_TIME_M)
	{
		/* hhmmss */
		if (len == 6)
		{
			*tmask = DTK_TIME_M;
			tm->tm_sec = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_min = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_hour = atoi(str + 0);

			return DTK_TIME;
		}
		/* hhmm? */
		else if (len == 4)
		{
			*tmask = DTK_TIME_M;
			tm->tm_sec = 0;
			tm->tm_min = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_hour = atoi(str + 0);

			return DTK_TIME;
		}
	}

	return -1;
}	/* DecodeNumberField() */


/* DecodeTimezone()
 * Interpret string as a numeric timezone.
 *
 * Note: we allow timezone offsets up to 13:59.  There are places that
 * use +1300 summer time.
 */
static int
DecodeTimezone(char *str, int *tzp)
{
	int			tz;
	int			hr,
				min;
	char	   *cp;
	int			len;

	/* assume leading character is "+" or "-" */
	hr = strtol((str + 1), &cp, 10);

	/* explicit delimiter? */
	if (*cp == ':')
		min = strtol((cp + 1), &cp, 10);
	/* otherwise, might have run things together... */
	else if ((*cp == '\0') && ((len = strlen(str)) > 3))
	{
		min = strtol((str + len - 2), &cp, 10);
		if ((min < 0) || (min >= 60))
			return -1;

		*(str + len - 2) = '\0';
		hr = strtol((str + 1), &cp, 10);
		if ((hr < 0) || (hr > 13))
			return -1;
	}
	else
		min = 0;

	tz = (hr * 60 + min) * 60;
	if (*str == '-')
		tz = -tz;

	*tzp = -tz;
	return *cp != '\0';
}	/* DecodeTimezone() */


/* DecodePosixTimezone()
 * Interpret string as a POSIX-compatible timezone:
 *	PST-hh:mm
 *	PST+h
 * - thomas 2000-03-15
 */
static int
DecodePosixTimezone(char *str, int *tzp)
{
	int			val,
				tz;
	int			type;
	char	   *cp;
	char		delim;

	cp = str;
	while ((*cp != '\0') && isalpha((unsigned char) *cp))
		cp++;

	if (DecodeTimezone(cp, &tz) != 0)
		return -1;

	delim = *cp;
	*cp = '\0';
	type = DecodeSpecial(MAXDATEFIELDS - 1, str, &val);
	*cp = delim;

	switch (type)
	{
		case DTZ:
		case TZ:
			*tzp = (val * 60) - tz;
			break;

		default:
			return -1;
	}

	return 0;
}	/* DecodePosixTimezone() */


/* DecodeSpecial()
 * Decode text string using lookup table.
 * Implement a cache lookup since it is likely that dates
 *	will be related in format.
 */
int
DecodeSpecial(int field, char *lowtoken, int *val)
{
	int			type;
	datetkn    *tp;

	if ((datecache[field] != NULL)
		&& (strncmp(lowtoken, datecache[field]->token, TOKMAXLEN) == 0))
		tp = datecache[field];
	else
	{
		tp = NULL;
		if (Australian_timezones)
			tp = datebsearch(lowtoken, australian_datetktbl,
							 australian_szdatetktbl);
		if (!tp)
			tp = datebsearch(lowtoken, datetktbl, szdatetktbl);
	}
	datecache[field] = tp;
	if (tp == NULL)
	{
		type = UNKNOWN_FIELD;
		*val = 0;
	}
	else
	{
		type = tp->type;
		switch (type)
		{
			case TZ:
			case DTZ:
			case DTZMOD:
				*val = FROMVAL(tp);
				break;

			default:
				*val = tp->value;
				break;
		}
	}

	return type;
}	/* DecodeSpecial() */


/* DecodeInterval()
 * Interpret previously parsed fields for general time interval.
 * Return 0 if decoded and -1 if problems.
 *
 * Allow "date" field DTK_DATE since this could be just
 *	an unsigned floating point number. - thomas 1997-11-16
 *
 * Allow ISO-style time span, with implicit units on number of days
 *	preceding an hh:mm:ss field. - thomas 1998-04-30
 */
int
DecodeInterval(char **field, int *ftype, int nf, int *dtype, struct tm * tm, fsec_t *fsec)
{
	int			is_before = FALSE;

	char	   *cp;
	int			fmask = 0,
				tmask,
				type;
	int			i;
	int			val;
	double		fval;

	*dtype = DTK_DELTA;

	type = IGNORE_DTF;
	tm->tm_year = 0;
	tm->tm_mon = 0;
	tm->tm_mday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;

	/* read through list backwards to pick up units before values */
	for (i = nf - 1; i >= 0; i--)
	{
		switch (ftype[i])
		{
			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;
				type = DTK_DAY;
				break;

			case DTK_TZ:

				/*
				 * Timezone is a token with a leading sign character and
				 * otherwise the same as a non-signed time field
				 */
				Assert((*field[i] == '-') || (*field[i] == '+'));

				/*
				 * A single signed number ends up here, but will be
				 * rejected by DecodeTime(). So, work this out to drop
				 * through to DTK_NUMBER, which *can* tolerate this.
				 */
				cp = field[i] + 1;
				while ((*cp != '\0') && (*cp != ':') && (*cp != '.'))
					cp++;
				if ((*cp == ':')
					&& (DecodeTime((field[i] + 1), fmask, &tmask, tm, fsec) == 0))
				{
					if (*field[i] == '-')
					{
						/* flip the sign on all fields */
						tm->tm_hour = -tm->tm_hour;
						tm->tm_min = -tm->tm_min;
						tm->tm_sec = -tm->tm_sec;
						*fsec = -(*fsec);
					}

					/*
					 * Set the next type to be a day, if units are not
					 * specified. This handles the case of '1 +02:03'
					 * since we are reading right to left.
					 */
					type = DTK_DAY;
					tmask = DTK_M(TZ);
					break;
				}
				else if (type == IGNORE_DTF)
				{
					if (*cp == '.')
					{
						/*
						 * Got a decimal point? Then assume some sort of
						 * seconds specification
						 */
						type = DTK_SECOND;
					}
					else if (*cp == '\0')
					{
						/*
						 * Only a signed integer? Then must assume a
						 * timezone-like usage
						 */
						type = DTK_HOUR;
					}
				}
				/* DROP THROUGH */

			case DTK_DATE:
			case DTK_NUMBER:
				val = strtol(field[i], &cp, 10);

				if (type == IGNORE_DTF)
					type = DTK_SECOND;

				if (*cp == '.')
				{
					fval = strtod(cp, &cp);
					if (*cp != '\0')
						return -1;

					if (val < 0)
						fval = -(fval);
				}
				else if (*cp == '\0')
					fval = 0;
				else
					return -1;

				tmask = 0;		/* DTK_M(type); */

				switch (type)
				{
					case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
						*fsec += (val + fval);
#else
						*fsec += ((val + fval) * 1e-6);
#endif
						break;

					case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
						*fsec += ((val + fval) * 1000);
#else
						*fsec += ((val + fval) * 1e-3);
#endif
						break;

					case DTK_SECOND:
						tm->tm_sec += val;
#ifdef HAVE_INT64_TIMESTAMP
						*fsec += (fval * 1000000);
#else
						*fsec += fval;
#endif
						tmask = DTK_M(SECOND);
						break;

					case DTK_MINUTE:
						tm->tm_min += val;
						if (fval != 0)
						{
							int			sec;

							fval *= 60;
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += ((fval - sec) * 1000000);
#else
							*fsec += (fval - sec);
#endif
						}
						tmask = DTK_M(MINUTE);
						break;

					case DTK_HOUR:
						tm->tm_hour += val;
						if (fval != 0)
						{
							int			sec;

							fval *= 3600;
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += ((fval - sec) * 1000000);
#else
							*fsec += (fval - sec);
#endif
						}
						tmask = DTK_M(HOUR);
						break;

					case DTK_DAY:
						tm->tm_mday += val;
						if (fval != 0)
						{
							int			sec;

							fval *= 86400;
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += ((fval - sec) * 1000000);
#else
							*fsec += (fval - sec);
#endif
						}
						tmask = ((fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY));
						break;

					case DTK_WEEK:
						tm->tm_mday += val * 7;
						if (fval != 0)
						{
							int			sec;

							fval *= (7 * 86400);
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += ((fval - sec) * 1000000);
#else
							*fsec += (fval - sec);
#endif
						}
						tmask = ((fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY));
						break;

					case DTK_MONTH:
						tm->tm_mon += val;
						if (fval != 0)
						{
							int			sec;

							fval *= (30 * 86400);
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += ((fval - sec) * 1000000);
#else
							*fsec += (fval - sec);
#endif
						}
						tmask = DTK_M(MONTH);
						break;

					case DTK_YEAR:
						tm->tm_year += val;
						if (fval != 0)
							tm->tm_mon += (fval * 12);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_DECADE:
						tm->tm_year += val * 10;
						if (fval != 0)
							tm->tm_mon += (fval * 120);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_CENTURY:
						tm->tm_year += val * 100;
						if (fval != 0)
							tm->tm_mon += (fval * 1200);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_MILLENNIUM:
						tm->tm_year += val * 1000;
						if (fval != 0)
							tm->tm_mon += (fval * 12000);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					default:
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeUnits(i, field[i], &val);
				if (type == IGNORE_DTF)
					continue;

				tmask = 0;		/* DTK_M(type); */
				switch (type)
				{
					case UNITS:
						type = val;
						break;

					case AGO:
						is_before = TRUE;
						type = val;
						break;

					case RESERV:
						tmask = (DTK_DATE_M || DTK_TIME_M);
						*dtype = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	if (*fsec != 0)
	{
		int			sec;

#ifdef HAVE_INT64_TIMESTAMP
		sec = (*fsec / INT64CONST(1000000));
		*fsec -= (sec * INT64CONST(1000000));
#else
		TMODULO(*fsec, sec, 1e0);
#endif
		tm->tm_sec += sec;
	}

	if (is_before)
	{
		*fsec = -(*fsec);
		tm->tm_sec = -(tm->tm_sec);
		tm->tm_min = -(tm->tm_min);
		tm->tm_hour = -(tm->tm_hour);
		tm->tm_mday = -(tm->tm_mday);
		tm->tm_mon = -(tm->tm_mon);
		tm->tm_year = -(tm->tm_year);
	}

	/* ensure that at least one time field has been found */
	return (fmask != 0) ? 0 : -1;
}	/* DecodeInterval() */


/* DecodeUnits()
 * Decode text string using lookup table.
 * This routine supports time interval decoding.
 */
int
DecodeUnits(int field, char *lowtoken, int *val)
{
	int			type;
	datetkn    *tp;

	if ((deltacache[field] != NULL)
		&& (strncmp(lowtoken, deltacache[field]->token, TOKMAXLEN) == 0))
		tp = deltacache[field];
	else
		tp = datebsearch(lowtoken, deltatktbl, szdeltatktbl);
	deltacache[field] = tp;
	if (tp == NULL)
	{
		type = UNKNOWN_FIELD;
		*val = 0;
	}
	else
	{
		type = tp->type;
		if ((type == TZ) || (type == DTZ))
			*val = FROMVAL(tp);
		else
			*val = tp->value;
	}

	return type;
}	/* DecodeUnits() */


/* datebsearch()
 * Binary search -- from Knuth (6.2.1) Algorithm B.  Special case like this
 * is WAY faster than the generic bsearch().
 */
static datetkn *
datebsearch(char *key, datetkn *base, unsigned int nel)
{
	datetkn    *last = base + nel - 1,
			   *position;
	int			result;

	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = key[0] - position->token[0];
		if (result == 0)
		{
			result = strncmp(key, position->token, TOKMAXLEN);
			if (result == 0)
				return position;
		}
		if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return NULL;
}


/* EncodeDateOnly()
 * Encode date as local time.
 */
int
EncodeDateOnly(struct tm * tm, int style, char *str)
{
	if ((tm->tm_mon < 1) || (tm->tm_mon > 12))
		return -1;

	switch (style)
	{
		case USE_ISO_DATES:
			/* compatible with ISO date formats */
			if (tm->tm_year > 0)
				sprintf(str, "%04d-%02d-%02d",
						tm->tm_year, tm->tm_mon, tm->tm_mday);
			else
				sprintf(str, "%04d-%02d-%02d %s",
					  -(tm->tm_year - 1), tm->tm_mon, tm->tm_mday, "BC");
			break;

		case USE_SQL_DATES:
			/* compatible with Oracle/Ingres date formats */
			if (EuroDates)
				sprintf(str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d/%02d", tm->tm_mon, tm->tm_mday);
			if (tm->tm_year > 0)
				sprintf((str + 5), "/%04d", tm->tm_year);
			else
				sprintf((str + 5), "/%04d %s", -(tm->tm_year - 1), "BC");
			break;

		case USE_GERMAN_DATES:
			/* German-style date format */
			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);
			if (tm->tm_year > 0)
				sprintf((str + 5), ".%04d", tm->tm_year);
			else
				sprintf((str + 5), ".%04d %s", -(tm->tm_year - 1), "BC");
			break;

		case USE_POSTGRES_DATES:
		default:
			/* traditional date-only style for Postgres */
			if (EuroDates)
				sprintf(str, "%02d-%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d-%02d", tm->tm_mon, tm->tm_mday);
			if (tm->tm_year > 0)
				sprintf((str + 5), "-%04d", tm->tm_year);
			else
				sprintf((str + 5), "-%04d %s", -(tm->tm_year - 1), "BC");
			break;
	}

	return TRUE;
}	/* EncodeDateOnly() */


/* EncodeTimeOnly()
 * Encode time fields only.
 */
int
EncodeTimeOnly(struct tm * tm, fsec_t fsec, int *tzp, int style, char *str)
{
	if ((tm->tm_hour < 0) || (tm->tm_hour > 24))
		return -1;

	sprintf(str, "%02d:%02d", tm->tm_hour, tm->tm_min);

	/*
	 * Print fractional seconds if any.  The field widths here should be
	 * at least equal to the larger of MAX_TIME_PRECISION and
	 * MAX_TIMESTAMP_PRECISION.
	 */
	if (fsec != 0)
	{
#ifdef HAVE_INT64_TIMESTAMP
		sprintf((str + strlen(str)), ":%02d.%06d", tm->tm_sec, fsec);
#else
		sprintf((str + strlen(str)), ":%013.10f", tm->tm_sec + fsec);
#endif
		/* chop off trailing pairs of zeros... */
		while ((strcmp((str + strlen(str) - 2), "00") == 0)
			   && (*(str + strlen(str) - 3) != '.'))
			*(str + strlen(str) - 2) = '\0';
	}
	else
		sprintf((str + strlen(str)), ":%02d", tm->tm_sec);

	if (tzp != NULL)
	{
		int			hour,
					min;

		hour = -(*tzp / 3600);
		min = ((abs(*tzp) / 60) % 60);
		sprintf((str + strlen(str)), ((min != 0) ? "%+03d:%02d" : "%+03d"), hour, min);
	}

	return TRUE;
}	/* EncodeTimeOnly() */


/* EncodeDateTime()
 * Encode date and time interpreted as local time.
 * Support several date styles:
 *	Postgres - day mon hh:mm:ss yyyy tz
 *	SQL - mm/dd/yyyy hh:mm:ss.ss tz
 *	ISO - yyyy-mm-dd hh:mm:ss+/-tz
 *	German - dd.mm.yyyy hh:mm:ss tz
 * Variants (affects order of month and day for Postgres and SQL styles):
 *	US - mm/dd/yyyy
 *	European - dd/mm/yyyy
 */
int
EncodeDateTime(struct tm * tm, fsec_t fsec, int *tzp, char **tzn, int style, char *str)
{
	int			day,
				hour,
				min;

	/*
	 * Why are we checking only the month field? Change this to an
	 * assert... if ((tm->tm_mon < 1) || (tm->tm_mon > 12)) return -1;
	 */
	Assert((tm->tm_mon >= 1) && (tm->tm_mon <= 12));

	switch (style)
	{
		case USE_ISO_DATES:
			/* Compatible with ISO-8601 date formats */

			sprintf(str, "%04d-%02d-%02d %02d:%02d",
				  ((tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1)),
					tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 *
			 * In float mode, don't print fractional seconds before 1 AD,
			 * since it's unlikely there's any precision left ...
			 */
#ifdef HAVE_INT64_TIMESTAMP
			if (fsec != 0)
			{
				sprintf((str + strlen(str)), ":%02d.%06d", tm->tm_sec, fsec);
#else
			if ((fsec != 0) && (tm->tm_year > 0))
			{
				sprintf((str + strlen(str)), ":%09.6f", tm->tm_sec + fsec);
#endif
				TrimTrailingZeros(str);
			}
			else
				sprintf((str + strlen(str)), ":%02d", tm->tm_sec);

			if (tm->tm_year <= 0)
				sprintf((str + strlen(str)), " BC");

			/*
			 * tzp == NULL indicates that we don't want *any* time zone
			 * info in the output string. *tzn != NULL indicates that we
			 * have alpha time zone info available. tm_isdst != -1
			 * indicates that we have a valid time zone translation.
			 */
			if ((tzp != NULL) && (tm->tm_isdst >= 0))
			{
				hour = -(*tzp / 3600);
				min = ((abs(*tzp) / 60) % 60);
				sprintf((str + strlen(str)), ((min != 0) ? "%+03d:%02d" : "%+03d"), hour, min);
			}
			break;

		case USE_SQL_DATES:
			/* Compatible with Oracle/Ingres date formats */

			if (EuroDates)
				sprintf(str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d/%02d", tm->tm_mon, tm->tm_mday);

			sprintf((str + 5), "/%04d %02d:%02d",
				  ((tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1)),
					tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 *
			 * In float mode, don't print fractional seconds before 1 AD,
			 * since it's unlikely there's any precision left ...
			 */
#ifdef HAVE_INT64_TIMESTAMP
			if (fsec != 0)
			{
				sprintf((str + strlen(str)), ":%02d.%06d", tm->tm_sec, fsec);
#else
			if ((fsec != 0) && (tm->tm_year > 0))
			{
				sprintf((str + strlen(str)), ":%09.6f", tm->tm_sec + fsec);
#endif
				TrimTrailingZeros(str);
			}
			else
				sprintf((str + strlen(str)), ":%02d", tm->tm_sec);

			if (tm->tm_year <= 0)
				sprintf((str + strlen(str)), " BC");

			if ((tzp != NULL) && (tm->tm_isdst >= 0))
			{
				if (*tzn != NULL)
					sprintf((str + strlen(str)), " %.*s", MAXTZLEN, *tzn);
				else
				{
					hour = -(*tzp / 3600);
					min = ((abs(*tzp) / 60) % 60);
					sprintf((str + strlen(str)), ((min != 0) ? "%+03d:%02d" : "%+03d"), hour, min);
				}
			}
			break;

		case USE_GERMAN_DATES:
			/* German variant on European style */

			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);

			sprintf((str + 5), ".%04d %02d:%02d",
				  ((tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1)),
					tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 *
			 * In float mode, don't print fractional seconds before 1 AD,
			 * since it's unlikely there's any precision left ...
			 */
#ifdef HAVE_INT64_TIMESTAMP
			if (fsec != 0)
			{
				sprintf((str + strlen(str)), ":%02d.%06d", tm->tm_sec, fsec);
#else
			if ((fsec != 0) && (tm->tm_year > 0))
			{
				sprintf((str + strlen(str)), ":%09.6f", tm->tm_sec + fsec);
#endif
				TrimTrailingZeros(str);
			}
			else
				sprintf((str + strlen(str)), ":%02d", tm->tm_sec);

			if (tm->tm_year <= 0)
				sprintf((str + strlen(str)), " BC");

			if ((tzp != NULL) && (tm->tm_isdst >= 0))
			{
				if (*tzn != NULL)
					sprintf((str + strlen(str)), " %.*s", MAXTZLEN, *tzn);
				else
				{
					hour = -(*tzp / 3600);
					min = ((abs(*tzp) / 60) % 60);
					sprintf((str + strlen(str)), ((min != 0) ? "%+03d:%02d" : "%+03d"), hour, min);
				}
			}
			break;

		case USE_POSTGRES_DATES:
		default:
			/* Backward-compatible with traditional Postgres abstime dates */

			day = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			tm->tm_wday = j2day(day);

			strncpy(str, days[tm->tm_wday], 3);
			strcpy((str + 3), " ");

			if (EuroDates)
				sprintf((str + 4), "%02d %3s", tm->tm_mday, months[tm->tm_mon - 1]);
			else
				sprintf((str + 4), "%3s %02d", months[tm->tm_mon - 1], tm->tm_mday);

			sprintf((str + 10), " %02d:%02d", tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 *
			 * In float mode, don't print fractional seconds before 1 AD,
			 * since it's unlikely there's any precision left ...
			 */
#ifdef HAVE_INT64_TIMESTAMP
			if (fsec != 0)
			{
				sprintf((str + strlen(str)), ":%02d.%06d", tm->tm_sec, fsec);
#else
			if ((fsec != 0) && (tm->tm_year > 0))
			{
				sprintf((str + strlen(str)), ":%09.6f", tm->tm_sec + fsec);
#endif
				TrimTrailingZeros(str);
			}
			else
				sprintf((str + strlen(str)), ":%02d", tm->tm_sec);

			sprintf((str + strlen(str)), " %04d",
				 ((tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1)));
			if (tm->tm_year <= 0)
				sprintf((str + strlen(str)), " BC");

			if ((tzp != NULL) && (tm->tm_isdst >= 0))
			{
				if (*tzn != NULL)
					sprintf((str + strlen(str)), " %.*s", MAXTZLEN, *tzn);
				else
				{
					/*
					 * We have a time zone, but no string version. Use the
					 * numeric form, but be sure to include a leading
					 * space to avoid formatting something which would be
					 * rejected by the date/time parser later. - thomas
					 * 2001-10-19
					 */
					hour = -(*tzp / 3600);
					min = ((abs(*tzp) / 60) % 60);
					sprintf((str + strlen(str)), ((min != 0) ? " %+03d:%02d" : " %+03d"), hour, min);
				}
			}
			break;
	}

	return TRUE;
}	/* EncodeDateTime() */


/* EncodeInterval()
 * Interpret time structure as a delta time and convert to string.
 *
 * Support "traditional Postgres" and ISO-8601 styles.
 * Actually, afaik ISO does not address time interval formatting,
 *	but this looks similar to the spec for absolute date/time.
 * - thomas 1998-04-30
 */
int
EncodeInterval(struct tm * tm, fsec_t fsec, int style, char *str)
{
	int			is_before = FALSE;
	int			is_nonzero = FALSE;
	char	   *cp = str;

	/*
	 * The sign of year and month are guaranteed to match, since they are
	 * stored internally as "month". But we'll need to check for is_before
	 * and is_nonzero when determining the signs of hour/minute/seconds
	 * fields.
	 */
	switch (style)
	{
			/* compatible with ISO date formats */
		case USE_ISO_DATES:
			if (tm->tm_year != 0)
			{
				sprintf(cp, "%d year%s",
						tm->tm_year, ((tm->tm_year != 1) ? "s" : ""));
				cp += strlen(cp);
				is_before = (tm->tm_year < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mon != 0)
			{
				sprintf(cp, "%s%s%d mon%s", (is_nonzero ? " " : ""),
						((is_before && (tm->tm_mon > 0)) ? "+" : ""),
						tm->tm_mon, ((tm->tm_mon != 1) ? "s" : ""));
				cp += strlen(cp);
				is_before = (tm->tm_mon < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mday != 0)
			{
				sprintf(cp, "%s%s%d day%s", (is_nonzero ? " " : ""),
						((is_before && (tm->tm_mday > 0)) ? "+" : ""),
						tm->tm_mday, ((tm->tm_mday != 1) ? "s" : ""));
				cp += strlen(cp);
				is_before = (tm->tm_mday < 0);
				is_nonzero = TRUE;
			}
			if ((!is_nonzero) || (tm->tm_hour != 0) || (tm->tm_min != 0)
				|| (tm->tm_sec != 0) || (fsec != 0))
			{
				int			minus = ((tm->tm_hour < 0) || (tm->tm_min < 0)
									 || (tm->tm_sec < 0) || (fsec < 0));

				sprintf(cp, "%s%s%02d:%02d", (is_nonzero ? " " : ""),
						(minus ? "-" : (is_before ? "+" : "")),
						abs(tm->tm_hour), abs(tm->tm_min));
				cp += strlen(cp);
				/* Mark as "non-zero" since the fields are now filled in */
				is_nonzero = TRUE;

				/* fractional seconds? */
				if (fsec != 0)
				{
#ifdef HAVE_INT64_TIMESTAMP
					sprintf(cp, ":%02d", abs(tm->tm_sec));
					cp += strlen(cp);
					sprintf(cp, ".%06d", ((fsec >= 0) ? fsec : -(fsec)));
#else
					fsec += tm->tm_sec;
					sprintf(cp, ":%013.10f", fabs(fsec));
#endif
					TrimTrailingZeros(cp);
					cp += strlen(cp);
					is_nonzero = TRUE;
				}
				/* otherwise, integer seconds only? */
				else if (tm->tm_sec != 0)
				{
					sprintf(cp, ":%02d", abs(tm->tm_sec));
					cp += strlen(cp);
					is_nonzero = TRUE;
				}
			}
			break;

		case USE_POSTGRES_DATES:
		default:
			strcpy(cp, "@ ");
			cp += strlen(cp);

			if (tm->tm_year != 0)
			{
				int			year = tm->tm_year;

				if (tm->tm_year < 0)
					year = -year;

				sprintf(cp, "%d year%s", year,
						((year != 1) ? "s" : ""));
				cp += strlen(cp);
				is_before = (tm->tm_year < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mon != 0)
			{
				int			mon = tm->tm_mon;

				if (is_before || ((!is_nonzero) && (tm->tm_mon < 0)))
					mon = -mon;

				sprintf(cp, "%s%d mon%s", (is_nonzero ? " " : ""), mon,
						((mon != 1) ? "s" : ""));
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_mon < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mday != 0)
			{
				int			day = tm->tm_mday;

				if (is_before || ((!is_nonzero) && (tm->tm_mday < 0)))
					day = -day;

				sprintf(cp, "%s%d day%s", (is_nonzero ? " " : ""), day,
						((day != 1) ? "s" : ""));
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_mday < 0);
				is_nonzero = TRUE;
			}
			if (tm->tm_hour != 0)
			{
				int			hour = tm->tm_hour;

				if (is_before || ((!is_nonzero) && (tm->tm_hour < 0)))
					hour = -hour;

				sprintf(cp, "%s%d hour%s", (is_nonzero ? " " : ""), hour,
						((hour != 1) ? "s" : ""));
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_hour < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_min != 0)
			{
				int			min = tm->tm_min;

				if (is_before || ((!is_nonzero) && (tm->tm_min < 0)))
					min = -min;

				sprintf(cp, "%s%d min%s", (is_nonzero ? " " : ""), min,
						((min != 1) ? "s" : ""));
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_min < 0);
				is_nonzero = TRUE;
			}

			/* fractional seconds? */
			if (fsec != 0)
			{
#ifdef HAVE_INT64_TIMESTAMP
				if (is_before || ((!is_nonzero) && (tm->tm_sec < 0)))
					tm->tm_sec = -tm->tm_sec;
				sprintf(cp, "%s%d.%02d secs", (is_nonzero ? " " : ""),
						tm->tm_sec, (((int) fsec) / 10000));
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (fsec < 0);
#else
				fsec_t		sec;

				fsec += tm->tm_sec;
				sec = fsec;
				if (is_before || ((!is_nonzero) && (fsec < 0)))
					sec = -sec;

				sprintf(cp, "%s%.2f secs", (is_nonzero ? " " : ""), sec);
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (fsec < 0);
#endif
				is_nonzero = TRUE;

				/* otherwise, integer seconds only? */
			}
			else if (tm->tm_sec != 0)
			{
				int			sec = tm->tm_sec;

				if (is_before || ((!is_nonzero) && (tm->tm_sec < 0)))
					sec = -sec;

				sprintf(cp, "%s%d sec%s", (is_nonzero ? " " : ""), sec,
						((sec != 1) ? "s" : ""));
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_sec < 0);
				is_nonzero = TRUE;
			}
			break;
	}

	/* identically zero? then put in a unitless zero... */
	if (!is_nonzero)
	{
		strcat(cp, "0");
		cp += strlen(cp);
	}

	if (is_before && (style == USE_POSTGRES_DATES))
	{
		strcat(cp, " ago");
		cp += strlen(cp);
	}

	return 0;
}	/* EncodeInterval() */


/* GUC assign_hook for australian_timezones */
bool
ClearDateCache(bool newval, bool doit, bool interactive)
{
	int			i;

	if (doit)
	{
		for (i = 0; i < MAXDATEFIELDS; i++)
			datecache[i] = NULL;
	}

	return true;
}

/*
 * We've been burnt by stupid errors in the ordering of the datetkn tables
 * once too often.  Arrange to check them during postmaster start.
 */
static bool
CheckDateTokenTable(const char *tablename, datetkn *base, unsigned int nel)
{
	bool		ok = true;
	unsigned int i;

	for (i = 1; i < nel; i++)
	{
		if (strncmp(base[i-1].token, base[i].token, TOKMAXLEN) >= 0)
		{
			elog(LOG, "Ordering error in %s table: \"%.*s\" >= \"%.*s\"",
				 tablename,
				 TOKMAXLEN, base[i-1].token,
				 TOKMAXLEN, base[i].token);
			ok = false;
		}
	}
	return ok;
}

bool
CheckDateTokenTables(void)
{
	bool		ok = true;

	Assert(UNIX_EPOCH_JDATE == date2j(1970, 1, 1));
	Assert(POSTGRES_EPOCH_JDATE == date2j(2000, 1, 1));

	ok &= CheckDateTokenTable("datetktbl", datetktbl, szdatetktbl);
	ok &= CheckDateTokenTable("deltatktbl", deltatktbl, szdeltatktbl);
	ok &= CheckDateTokenTable("australian_datetktbl",
							  australian_datetktbl,
							  australian_szdatetktbl);
	return ok;
}
