/* src/interfaces/ecpg/pgtypeslib/dt_common.c */

#include "postgres_fe.h"

#include <time.h>
#include <ctype.h>
#include <math.h>

#include "common/string.h"
#include "dt.h"
#include "pgtypes_timestamp.h"
#include "pgtypeslib_extern.h"

const int	day_tab[2][13] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0},
{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0}};

typedef long AbsoluteTime;

static const datetkn datetktbl[] = {
/*	text, token, lexval */
	{EARLY, RESERV, DTK_EARLY}, /* "-infinity" reserved for "early time" */
	{"acsst", DTZ, 37800},		/* Cent. Australia */
	{"acst", DTZ, -14400},		/* Atlantic/Porto Acre */
	{"act", TZ, -18000},		/* Atlantic/Porto Acre */
	{DA_D, ADBC, AD},			/* "ad" for years >= 0 */
	{"adt", DTZ, -10800},		/* Atlantic Daylight Time */
	{"aesst", DTZ, 39600},		/* E. Australia */
	{"aest", TZ, 36000},		/* Australia Eastern Std Time */
	{"aft", TZ, 16200},			/* Kabul */
	{"ahst", TZ, -36000},		/* Alaska-Hawaii Std Time */
	{"akdt", DTZ, -28800},		/* Alaska Daylight Time */
	{"akst", DTZ, -32400},		/* Alaska Standard Time */
	{"allballs", RESERV, DTK_ZULU}, /* 00:00:00 */
	{"almst", TZ, 25200},		/* Almaty Savings Time */
	{"almt", TZ, 21600},		/* Almaty Time */
	{"am", AMPM, AM},
	{"amst", DTZ, 18000},		/* Armenia Summer Time (Yerevan) */
#if 0
	{"amst", DTZ, -10800},		/* Porto Velho */
#endif
	{"amt", TZ, 14400},			/* Armenia Time (Yerevan) */
	{"anast", DTZ, 46800},		/* Anadyr Summer Time (Russia) */
	{"anat", TZ, 43200},		/* Anadyr Time (Russia) */
	{"apr", MONTH, 4},
	{"april", MONTH, 4},
#if 0
	aqtst
	aqtt
	arst
#endif
	{"art", TZ, -10800},		/* Argentina Time */
#if 0
	ashst
	ast							/* Atlantic Standard Time, Arabia Standard
								 * Time, Acre Standard Time */
#endif
	{"ast", TZ, -14400},		/* Atlantic Std Time (Canada) */
	{"at", IGNORE_DTF, 0},		/* "at" (throwaway) */
	{"aug", MONTH, 8},
	{"august", MONTH, 8},
	{"awsst", DTZ, 32400},		/* W. Australia */
	{"awst", TZ, 28800},		/* W. Australia */
	{"awt", DTZ, -10800},
	{"azost", DTZ, 0},			/* Azores Summer Time */
	{"azot", TZ, -3600},		/* Azores Time */
	{"azst", DTZ, 18000},		/* Azerbaijan Summer Time */
	{"azt", TZ, 14400},			/* Azerbaijan Time */
	{DB_C, ADBC, BC},			/* "bc" for years < 0 */
	{"bdst", TZ, 7200},			/* British Double Summer Time */
	{"bdt", TZ, 21600},			/* Dacca */
	{"bnt", TZ, 28800},			/* Brunei Darussalam Time */
	{"bort", TZ, 28800},		/* Borneo Time (Indonesia) */
#if 0
	bortst
	bost
#endif
	{"bot", TZ, -14400},		/* Bolivia Time */
	{"bra", TZ, -10800},		/* Brazil Time */
#if 0
	brst
	brt
#endif
	{"bst", DTZ, 3600},			/* British Summer Time */
#if 0
	{"bst", TZ, -10800},		/* Brazil Standard Time */
	{"bst", DTZ, -39600},		/* Bering Summer Time */
#endif
	{"bt", TZ, 10800},			/* Baghdad Time */
	{"btt", TZ, 21600},			/* Bhutan Time */
	{"cadt", DTZ, 37800},		/* Central Australian DST */
	{"cast", TZ, 34200},		/* Central Australian ST */
	{"cat", TZ, -36000},		/* Central Alaska Time */
	{"cct", TZ, 28800},			/* China Coast Time */
#if 0
	{"cct", TZ, 23400},			/* Indian Cocos (Island) Time */
#endif
	{"cdt", DTZ, -18000},		/* Central Daylight Time */
	{"cest", DTZ, 7200},		/* Central European Dayl.Time */
	{"cet", TZ, 3600},			/* Central European Time */
	{"cetdst", DTZ, 7200},		/* Central European Dayl.Time */
	{"chadt", DTZ, 49500},		/* Chatham Island Daylight Time (13:45) */
	{"chast", TZ, 45900},		/* Chatham Island Time (12:45) */
#if 0
	ckhst
#endif
	{"ckt", TZ, 43200},			/* Cook Islands Time */
	{"clst", DTZ, -10800},		/* Chile Summer Time */
	{"clt", TZ, -14400},		/* Chile Time */
#if 0
	cost
#endif
	{"cot", TZ, -18000},		/* Columbia Time */
	{"cst", TZ, -21600},		/* Central Standard Time */
#if 0
	cvst
#endif
	{"cvt", TZ, 25200},			/* Christmas Island Time (Indian Ocean) */
	{"cxt", TZ, 25200},			/* Christmas Island Time (Indian Ocean) */
	{"d", UNITS, DTK_DAY},		/* "day of month" for ISO input */
	{"davt", TZ, 25200},		/* Davis Time (Antarctica) */
	{"ddut", TZ, 36000},		/* Dumont-d'Urville Time (Antarctica) */
	{"dec", MONTH, 12},
	{"december", MONTH, 12},
	{"dnt", TZ, 3600},			/* Dansk Normal Tid */
	{"dow", UNITS, DTK_DOW},	/* day of week */
	{"doy", UNITS, DTK_DOY},	/* day of year */
	{"dst", DTZMOD, SECS_PER_HOUR},
#if 0
	{"dusst", DTZ, 21600},		/* Dushanbe Summer Time */
#endif
	{"easst", DTZ, -18000},		/* Easter Island Summer Time */
	{"east", TZ, -21600},		/* Easter Island Time */
	{"eat", TZ, 10800},			/* East Africa Time */
#if 0
	{"east", DTZ, 14400},		/* Indian Antananarivo Savings Time */
	{"eat", TZ, 10800},			/* Indian Antananarivo Time */
	{"ect", TZ, -14400},		/* Eastern Caribbean Time */
	{"ect", TZ, -18000},		/* Ecuador Time */
#endif
	{"edt", DTZ, -14400},		/* Eastern Daylight Time */
	{"eest", DTZ, 10800},		/* Eastern Europe Summer Time */
	{"eet", TZ, 7200},			/* East. Europe, USSR Zone 1 */
	{"eetdst", DTZ, 10800},		/* Eastern Europe Daylight Time */
	{"egst", DTZ, 0},			/* East Greenland Summer Time */
	{"egt", TZ, -3600},			/* East Greenland Time */
#if 0
	ehdt
#endif
	{EPOCH, RESERV, DTK_EPOCH}, /* "epoch" reserved for system epoch time */
	{"est", TZ, -18000},		/* Eastern Standard Time */
	{"feb", MONTH, 2},
	{"february", MONTH, 2},
	{"fjst", DTZ, -46800},		/* Fiji Summer Time (13 hour offset!) */
	{"fjt", TZ, -43200},		/* Fiji Time */
	{"fkst", DTZ, -10800},		/* Falkland Islands Summer Time */
	{"fkt", TZ, -7200},			/* Falkland Islands Time */
#if 0
	fnst
	fnt
#endif
	{"fri", DOW, 5},
	{"friday", DOW, 5},
	{"fst", TZ, 3600},			/* French Summer Time */
	{"fwt", DTZ, 7200},			/* French Winter Time  */
	{"galt", TZ, -21600},		/* Galapagos Time */
	{"gamt", TZ, -32400},		/* Gambier Time */
	{"gest", DTZ, 18000},		/* Georgia Summer Time */
	{"get", TZ, 14400},			/* Georgia Time */
	{"gft", TZ, -10800},		/* French Guiana Time */
#if 0
	ghst
#endif
	{"gilt", TZ, 43200},		/* Gilbert Islands Time */
	{"gmt", TZ, 0},				/* Greenwich Mean Time */
	{"gst", TZ, 36000},			/* Guam Std Time, USSR Zone 9 */
	{"gyt", TZ, -14400},		/* Guyana Time */
	{"h", UNITS, DTK_HOUR},		/* "hour" */
#if 0
	hadt
	hast
#endif
	{"hdt", DTZ, -32400},		/* Hawaii/Alaska Daylight Time */
#if 0
	hkst
#endif
	{"hkt", TZ, 28800},			/* Hong Kong Time */
#if 0
	{"hmt", TZ, 10800},			/* Hellas ? ? */
	hovst
	hovt
#endif
	{"hst", TZ, -36000},		/* Hawaii Std Time */
#if 0
	hwt
#endif
	{"ict", TZ, 25200},			/* Indochina Time */
	{"idle", TZ, 43200},		/* Intl. Date Line, East */
	{"idlw", TZ, -43200},		/* Intl. Date Line, West */
#if 0
	idt							/* Israeli, Iran, Indian Daylight Time */
#endif
	{LATE, RESERV, DTK_LATE},	/* "infinity" reserved for "late time" */
	{"iot", TZ, 18000},			/* Indian Chagos Time */
	{"irkst", DTZ, 32400},		/* Irkutsk Summer Time */
	{"irkt", TZ, 28800},		/* Irkutsk Time */
	{"irt", TZ, 12600},			/* Iran Time */
	{"isodow", UNITS, DTK_ISODOW},	/* ISO day of week, Sunday == 7 */
#if 0
	isst
#endif
	{"ist", TZ, 7200},			/* Israel */
	{"it", TZ, 12600},			/* Iran Time */
	{"j", UNITS, DTK_JULIAN},
	{"jan", MONTH, 1},
	{"january", MONTH, 1},
	{"javt", TZ, 25200},		/* Java Time (07:00? see JT) */
	{"jayt", TZ, 32400},		/* Jayapura Time (Indonesia) */
	{"jd", UNITS, DTK_JULIAN},
	{"jst", TZ, 32400},			/* Japan Std Time,USSR Zone 8 */
	{"jt", TZ, 27000},			/* Java Time (07:30? see JAVT) */
	{"jul", MONTH, 7},
	{"julian", UNITS, DTK_JULIAN},
	{"july", MONTH, 7},
	{"jun", MONTH, 6},
	{"june", MONTH, 6},
	{"kdt", DTZ, 36000},		/* Korea Daylight Time */
	{"kgst", DTZ, 21600},		/* Kyrgyzstan Summer Time */
	{"kgt", TZ, 18000},			/* Kyrgyzstan Time */
	{"kost", TZ, 43200},		/* Kosrae Time */
	{"krast", DTZ, 25200},		/* Krasnoyarsk Summer Time */
	{"krat", TZ, 28800},		/* Krasnoyarsk Standard Time */
	{"kst", TZ, 32400},			/* Korea Standard Time */
	{"lhdt", DTZ, 39600},		/* Lord Howe Daylight Time, Australia */
	{"lhst", TZ, 37800},		/* Lord Howe Standard Time, Australia */
	{"ligt", TZ, 36000},		/* From Melbourne, Australia */
	{"lint", TZ, 50400},		/* Line Islands Time (Kiribati; +14 hours!) */
	{"lkt", TZ, 21600},			/* Lanka Time */
	{"m", UNITS, DTK_MONTH},	/* "month" for ISO input */
	{"magst", DTZ, 43200},		/* Magadan Summer Time */
	{"magt", TZ, 39600},		/* Magadan Time */
	{"mar", MONTH, 3},
	{"march", MONTH, 3},
	{"mart", TZ, -34200},		/* Marquesas Time */
	{"mawt", TZ, 21600},		/* Mawson, Antarctica */
	{"may", MONTH, 5},
	{"mdt", DTZ, -21600},		/* Mountain Daylight Time */
	{"mest", DTZ, 7200},		/* Middle Europe Summer Time */
	{"met", TZ, 3600},			/* Middle Europe Time */
	{"metdst", DTZ, 7200},		/* Middle Europe Daylight Time */
	{"mewt", TZ, 3600},			/* Middle Europe Winter Time */
	{"mez", TZ, 3600},			/* Middle Europe Zone */
	{"mht", TZ, 43200},			/* Kwajalein */
	{"mm", UNITS, DTK_MINUTE},	/* "minute" for ISO input */
	{"mmt", TZ, 23400},			/* Myannar Time */
	{"mon", DOW, 1},
	{"monday", DOW, 1},
#if 0
	most
#endif
	{"mpt", TZ, 36000},			/* North Mariana Islands Time */
	{"msd", DTZ, 14400},		/* Moscow Summer Time */
	{"msk", TZ, 10800},			/* Moscow Time */
	{"mst", TZ, -25200},		/* Mountain Standard Time */
	{"mt", TZ, 30600},			/* Moluccas Time */
	{"mut", TZ, 14400},			/* Mauritius Island Time */
	{"mvt", TZ, 18000},			/* Maldives Island Time */
	{"myt", TZ, 28800},			/* Malaysia Time */
#if 0
	ncst
#endif
	{"nct", TZ, 39600},			/* New Caledonia Time */
	{"ndt", DTZ, -9000},		/* Nfld. Daylight Time */
	{"nft", TZ, -12600},		/* Newfoundland Standard Time */
	{"nor", TZ, 3600},			/* Norway Standard Time */
	{"nov", MONTH, 11},
	{"november", MONTH, 11},
	{"novst", DTZ, 25200},		/* Novosibirsk Summer Time */
	{"novt", TZ, 21600},		/* Novosibirsk Standard Time */
	{NOW, RESERV, DTK_NOW},		/* current transaction time */
	{"npt", TZ, 20700},			/* Nepal Standard Time (GMT-5:45) */
	{"nst", TZ, -12600},		/* Nfld. Standard Time */
	{"nt", TZ, -39600},			/* Nome Time */
	{"nut", TZ, -39600},		/* Niue Time */
	{"nzdt", DTZ, 46800},		/* New Zealand Daylight Time */
	{"nzst", TZ, 43200},		/* New Zealand Standard Time */
	{"nzt", TZ, 43200},			/* New Zealand Time */
	{"oct", MONTH, 10},
	{"october", MONTH, 10},
	{"omsst", DTZ, 25200},		/* Omsk Summer Time */
	{"omst", TZ, 21600},		/* Omsk Time */
	{"on", IGNORE_DTF, 0},		/* "on" (throwaway) */
	{"pdt", DTZ, -25200},		/* Pacific Daylight Time */
#if 0
	pest
#endif
	{"pet", TZ, -18000},		/* Peru Time */
	{"petst", DTZ, 46800},		/* Petropavlovsk-Kamchatski Summer Time */
	{"pett", TZ, 43200},		/* Petropavlovsk-Kamchatski Time */
	{"pgt", TZ, 36000},			/* Papua New Guinea Time */
	{"phot", TZ, 46800},		/* Phoenix Islands (Kiribati) Time */
#if 0
	phst
#endif
	{"pht", TZ, 28800},			/* Philippine Time */
	{"pkt", TZ, 18000},			/* Pakistan Time */
	{"pm", AMPM, PM},
	{"pmdt", DTZ, -7200},		/* Pierre & Miquelon Daylight Time */
#if 0
	pmst
#endif
	{"pont", TZ, 39600},		/* Ponape Time (Micronesia) */
	{"pst", TZ, -28800},		/* Pacific Standard Time */
	{"pwt", TZ, 32400},			/* Palau Time */
	{"pyst", DTZ, -10800},		/* Paraguay Summer Time */
	{"pyt", TZ, -14400},		/* Paraguay Time */
	{"ret", DTZ, 14400},		/* Reunion Island Time */
	{"s", UNITS, DTK_SECOND},	/* "seconds" for ISO input */
	{"sadt", DTZ, 37800},		/* S. Australian Dayl. Time */
#if 0
	samst
	samt
#endif
	{"sast", TZ, 34200},		/* South Australian Std Time */
	{"sat", DOW, 6},
	{"saturday", DOW, 6},
#if 0
	sbt
#endif
	{"sct", DTZ, 14400},		/* Mahe Island Time */
	{"sep", MONTH, 9},
	{"sept", MONTH, 9},
	{"september", MONTH, 9},
	{"set", TZ, -3600},			/* Seychelles Time ?? */
#if 0
	sgt
#endif
	{"sst", DTZ, 7200},			/* Swedish Summer Time */
	{"sun", DOW, 0},
	{"sunday", DOW, 0},
	{"swt", TZ, 3600},			/* Swedish Winter Time */
#if 0
	syot
#endif
	{"t", ISOTIME, DTK_TIME},	/* Filler for ISO time fields */
	{"tft", TZ, 18000},			/* Kerguelen Time */
	{"that", TZ, -36000},		/* Tahiti Time */
	{"thu", DOW, 4},
	{"thur", DOW, 4},
	{"thurs", DOW, 4},
	{"thursday", DOW, 4},
	{"tjt", TZ, 18000},			/* Tajikistan Time */
	{"tkt", TZ, -36000},		/* Tokelau Time */
	{"tmt", TZ, 18000},			/* Turkmenistan Time */
	{TODAY, RESERV, DTK_TODAY}, /* midnight */
	{TOMORROW, RESERV, DTK_TOMORROW},	/* tomorrow midnight */
#if 0
	tost
#endif
	{"tot", TZ, 46800},			/* Tonga Time */
#if 0
	tpt
#endif
	{"truk", TZ, 36000},		/* Truk Time */
	{"tue", DOW, 2},
	{"tues", DOW, 2},
	{"tuesday", DOW, 2},
	{"tvt", TZ, 43200},			/* Tuvalu Time */
#if 0
	uct
#endif
	{"ulast", DTZ, 32400},		/* Ulan Bator Summer Time */
	{"ulat", TZ, 28800},		/* Ulan Bator Time */
	{"ut", TZ, 0},
	{"utc", TZ, 0},
	{"uyst", DTZ, -7200},		/* Uruguay Summer Time */
	{"uyt", TZ, -10800},		/* Uruguay Time */
	{"uzst", DTZ, 21600},		/* Uzbekistan Summer Time */
	{"uzt", TZ, 18000},			/* Uzbekistan Time */
	{"vet", TZ, -14400},		/* Venezuela Time */
	{"vlast", DTZ, 39600},		/* Vladivostok Summer Time */
	{"vlat", TZ, 36000},		/* Vladivostok Time */
#if 0
	vust
#endif
	{"vut", TZ, 39600},			/* Vanuata Time */
	{"wadt", DTZ, 28800},		/* West Australian DST */
	{"wakt", TZ, 43200},		/* Wake Time */
#if 0
	warst
#endif
	{"wast", TZ, 25200},		/* West Australian Std Time */
	{"wat", TZ, -3600},			/* West Africa Time */
	{"wdt", DTZ, 32400},		/* West Australian DST */
	{"wed", DOW, 3},
	{"wednesday", DOW, 3},
	{"weds", DOW, 3},
	{"west", DTZ, 3600},		/* Western Europe Summer Time */
	{"wet", TZ, 0},				/* Western Europe */
	{"wetdst", DTZ, 3600},		/* Western Europe Daylight Savings Time */
	{"wft", TZ, 43200},			/* Wallis and Futuna Time */
	{"wgst", DTZ, -7200},		/* West Greenland Summer Time */
	{"wgt", TZ, -10800},		/* West Greenland Time */
	{"wst", TZ, 28800},			/* West Australian Standard Time */
	{"y", UNITS, DTK_YEAR},		/* "year" for ISO input */
	{"yakst", DTZ, 36000},		/* Yakutsk Summer Time */
	{"yakt", TZ, 32400},		/* Yakutsk Time */
	{"yapt", TZ, 36000},		/* Yap Time (Micronesia) */
	{"ydt", DTZ, -28800},		/* Yukon Daylight Time */
	{"yekst", DTZ, 21600},		/* Yekaterinburg Summer Time */
	{"yekt", TZ, 18000},		/* Yekaterinburg Time */
	{YESTERDAY, RESERV, DTK_YESTERDAY}, /* yesterday midnight */
	{"yst", TZ, -32400},		/* Yukon Standard Time */
	{"z", TZ, 0},				/* time zone tag per ISO-8601 */
	{"zp4", TZ, -14400},		/* UTC +4  hours. */
	{"zp5", TZ, -18000},		/* UTC +5  hours. */
	{"zp6", TZ, -21600},		/* UTC +6  hours. */
	{ZULU, TZ, 0},				/* UTC */
};

static const datetkn deltatktbl[] = {
	/* text, token, lexval */
	{"@", IGNORE_DTF, 0},		/* postgres relative prefix */
	{DAGO, AGO, 0},				/* "ago" indicates negative time offset */
	{"c", UNITS, DTK_CENTURY},	/* "century" relative */
	{"cent", UNITS, DTK_CENTURY},	/* "century" relative */
	{"centuries", UNITS, DTK_CENTURY},	/* "centuries" relative */
	{DCENTURY, UNITS, DTK_CENTURY}, /* "century" relative */
	{"d", UNITS, DTK_DAY},		/* "day" relative */
	{DDAY, UNITS, DTK_DAY},		/* "day" relative */
	{"days", UNITS, DTK_DAY},	/* "days" relative */
	{"dec", UNITS, DTK_DECADE}, /* "decade" relative */
	{DDECADE, UNITS, DTK_DECADE},	/* "decade" relative */
	{"decades", UNITS, DTK_DECADE}, /* "decades" relative */
	{"decs", UNITS, DTK_DECADE},	/* "decades" relative */
	{"h", UNITS, DTK_HOUR},		/* "hour" relative */
	{DHOUR, UNITS, DTK_HOUR},	/* "hour" relative */
	{"hours", UNITS, DTK_HOUR}, /* "hours" relative */
	{"hr", UNITS, DTK_HOUR},	/* "hour" relative */
	{"hrs", UNITS, DTK_HOUR},	/* "hours" relative */
	{"m", UNITS, DTK_MINUTE},	/* "minute" relative */
	{"microsecon", UNITS, DTK_MICROSEC},	/* "microsecond" relative */
	{"mil", UNITS, DTK_MILLENNIUM}, /* "millennium" relative */
	{"millennia", UNITS, DTK_MILLENNIUM},	/* "millennia" relative */
	{DMILLENNIUM, UNITS, DTK_MILLENNIUM},	/* "millennium" relative */
	{"millisecon", UNITS, DTK_MILLISEC},	/* relative */
	{"mils", UNITS, DTK_MILLENNIUM},	/* "millennia" relative */
	{"min", UNITS, DTK_MINUTE}, /* "minute" relative */
	{"mins", UNITS, DTK_MINUTE},	/* "minutes" relative */
	{DMINUTE, UNITS, DTK_MINUTE},	/* "minute" relative */
	{"minutes", UNITS, DTK_MINUTE}, /* "minutes" relative */
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
	{DQUARTER, UNITS, DTK_QUARTER}, /* "quarter" relative */
	{"s", UNITS, DTK_SECOND},
	{"sec", UNITS, DTK_SECOND},
	{DSECOND, UNITS, DTK_SECOND},
	{"seconds", UNITS, DTK_SECOND},
	{"secs", UNITS, DTK_SECOND},
	{DTIMEZONE, UNITS, DTK_TZ}, /* "timezone" time offset */
	{"timezone_h", UNITS, DTK_TZ_HOUR}, /* timezone hour units */
	{"timezone_m", UNITS, DTK_TZ_MINUTE},	/* timezone minutes units */
	{"us", UNITS, DTK_MICROSEC},	/* "microsecond" relative */
	{"usec", UNITS, DTK_MICROSEC},	/* "microsecond" relative */
	{DMICROSEC, UNITS, DTK_MICROSEC},	/* "microsecond" relative */
	{"useconds", UNITS, DTK_MICROSEC},	/* "microseconds" relative */
	{"usecs", UNITS, DTK_MICROSEC}, /* "microseconds" relative */
	{"w", UNITS, DTK_WEEK},		/* "week" relative */
	{DWEEK, UNITS, DTK_WEEK},	/* "week" relative */
	{"weeks", UNITS, DTK_WEEK}, /* "weeks" relative */
	{"y", UNITS, DTK_YEAR},		/* "year" relative */
	{DYEAR, UNITS, DTK_YEAR},	/* "year" relative */
	{"years", UNITS, DTK_YEAR}, /* "years" relative */
	{"yr", UNITS, DTK_YEAR},	/* "year" relative */
	{"yrs", UNITS, DTK_YEAR},	/* "years" relative */
};

static const unsigned int szdatetktbl = lengthof(datetktbl);
static const unsigned int szdeltatktbl = lengthof(deltatktbl);

static const datetkn *datecache[MAXDATEFIELDS] = {NULL};

static const datetkn *deltacache[MAXDATEFIELDS] = {NULL};

char	   *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL};

char	   *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", NULL};

char	   *pgtypes_date_weekdays_short[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", NULL};

char	   *pgtypes_date_months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December", NULL};

static const datetkn *
datebsearch(const char *key, const datetkn *base, unsigned int nel)
{
	if (nel > 0)
	{
		const datetkn *last = base + nel - 1,
				   *position;
		int			result;

		while (last >= base)
		{
			position = base + ((last - base) >> 1);
			/* precheck the first character for a bit of extra speed */
			result = (int) key[0] - (int) position->token[0];
			if (result == 0)
			{
				/* use strncmp so that we match truncated tokens */
				result = strncmp(key, position->token, TOKMAXLEN);
				if (result == 0)
					return position;
			}
			if (result < 0)
				last = position - 1;
			else
				base = position + 1;
		}
	}
	return NULL;
}

/* DecodeUnits()
 * Decode text string using lookup table.
 * This routine supports time interval decoding.
 */
int
DecodeUnits(int field, char *lowtoken, int *val)
{
	int			type;
	const datetkn *tp;

	/* use strncmp so that we match truncated tokens */
	if (deltacache[field] != NULL &&
		strncmp(lowtoken, deltacache[field]->token, TOKMAXLEN) == 0)
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
		*val = tp->value;
	}

	return type;
}								/* DecodeUnits() */

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
 * 0 to 2147483647	(Nov 24, -4713 to Jun 3, 5874898) assuming
 * a 32-bit integer. Longer types should also work to the limits
 * of their precision.
 */

int
date2j(int y, int m, int d)
{
	int			julian;
	int			century;

	if (m > 2)
	{
		m += 1;
		y += 4800;
	}
	else
	{
		m += 13;
		y += 4799;
	}

	century = y / 100;
	julian = y * 365 - 32167;
	julian += y / 4 - century + century / 4;
	julian += 7834 * m / 256 + d;

	return julian;
}								/* date2j() */

void
j2date(int jd, int *year, int *month, int *day)
{
	unsigned int julian;
	unsigned int quad;
	unsigned int extra;
	int			y;

	julian = jd;
	julian += 32044;
	quad = julian / 146097;
	extra = (julian - quad * 146097) * 4 + 3;
	julian += 60 + quad * 3 + extra / 146097;
	quad = julian / 1461;
	julian -= quad * 1461;
	y = julian * 4 / 1461;
	julian = ((y != 0) ? (julian + 305) % 365 : (julian + 306) % 366) + 123;
	y += quad * 4;
	*year = y - 4800;
	quad = julian * 2141 / 65536;
	*day = julian - 7834 * quad / 256;
	*month = (quad + 10) % 12 + 1;
}								/* j2date() */

/* DecodeSpecial()
 * Decode text string using lookup table.
 * Implement a cache lookup since it is likely that dates
 *	will be related in format.
 */
static int
DecodeSpecial(int field, char *lowtoken, int *val)
{
	int			type;
	const datetkn *tp;

	/* use strncmp so that we match truncated tokens */
	if (datecache[field] != NULL &&
		strncmp(lowtoken, datecache[field]->token, TOKMAXLEN) == 0)
		tp = datecache[field];
	else
	{
		tp = NULL;
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
		*val = tp->value;
	}

	return type;
}								/* DecodeSpecial() */

/* EncodeDateOnly()
 * Encode date as local time.
 */
void
EncodeDateOnly(struct tm *tm, int style, char *str, bool EuroDates)
{
	Assert(tm->tm_mon >= 1 && tm->tm_mon <= MONTHS_PER_YEAR);

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
				sprintf(str + 5, "/%04d", tm->tm_year);
			else
				sprintf(str + 5, "/%04d %s", -(tm->tm_year - 1), "BC");
			break;

		case USE_GERMAN_DATES:
			/* German-style date format */
			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);
			if (tm->tm_year > 0)
				sprintf(str + 5, ".%04d", tm->tm_year);
			else
				sprintf(str + 5, ".%04d %s", -(tm->tm_year - 1), "BC");
			break;

		case USE_POSTGRES_DATES:
		default:
			/* traditional date-only style for Postgres */
			if (EuroDates)
				sprintf(str, "%02d-%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d-%02d", tm->tm_mon, tm->tm_mday);
			if (tm->tm_year > 0)
				sprintf(str + 5, "-%04d", tm->tm_year);
			else
				sprintf(str + 5, "-%04d %s", -(tm->tm_year - 1), "BC");
			break;
	}
}

void
TrimTrailingZeros(char *str)
{
	int			len = strlen(str);

	/* chop off trailing zeros... but leave at least 2 fractional digits */
	while (*(str + len - 1) == '0' && *(str + len - 3) != '.')
	{
		len--;
		*(str + len) = '\0';
	}
}

/* EncodeDateTime()
 * Encode date and time interpreted as local time.
 *
 * tm and fsec are the value to encode, print_tz determines whether to include
 * a time zone (the difference between timestamp and timestamptz types), tz is
 * the numeric time zone offset, tzn is the textual time zone, which if
 * specified will be used instead of tz by some styles, style is the date
 * style, str is where to write the output.
 *
 * Supported date styles:
 *	Postgres - day mon hh:mm:ss yyyy tz
 *	SQL - mm/dd/yyyy hh:mm:ss.ss tz
 *	ISO - yyyy-mm-dd hh:mm:ss+/-tz
 *	German - dd.mm.yyyy hh:mm:ss tz
 * Variants (affects order of month and day for Postgres and SQL styles):
 *	US - mm/dd/yyyy
 *	European - dd/mm/yyyy
 */
void
EncodeDateTime(struct tm *tm, fsec_t fsec, bool print_tz, int tz, const char *tzn, int style, char *str, bool EuroDates)
{
	int			day,
				hour,
				min;

	/*
	 * Negative tm_isdst means we have no valid time zone translation.
	 */
	if (tm->tm_isdst < 0)
		print_tz = false;

	switch (style)
	{
		case USE_ISO_DATES:
			/* Compatible with ISO-8601 date formats */

			sprintf(str, "%04d-%02d-%02d %02d:%02d",
					(tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1),
					tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 */
			if (fsec != 0)
			{
				sprintf(str + strlen(str), ":%02d.%06d", tm->tm_sec, fsec);
				TrimTrailingZeros(str);
			}
			else
				sprintf(str + strlen(str), ":%02d", tm->tm_sec);

			if (tm->tm_year <= 0)
				sprintf(str + strlen(str), " BC");

			if (print_tz)
			{
				hour = -(tz / SECS_PER_HOUR);
				min = (abs(tz) / MINS_PER_HOUR) % MINS_PER_HOUR;
				if (min != 0)
					sprintf(str + strlen(str), "%+03d:%02d", hour, min);
				else
					sprintf(str + strlen(str), "%+03d", hour);
			}
			break;

		case USE_SQL_DATES:
			/* Compatible with Oracle/Ingres date formats */

			if (EuroDates)
				sprintf(str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d/%02d", tm->tm_mon, tm->tm_mday);

			sprintf(str + 5, "/%04d %02d:%02d",
					(tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1),
					tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 */
			if (fsec != 0)
			{
				sprintf(str + strlen(str), ":%02d.%06d", tm->tm_sec, fsec);
				TrimTrailingZeros(str);
			}
			else
				sprintf(str + strlen(str), ":%02d", tm->tm_sec);

			if (tm->tm_year <= 0)
				sprintf(str + strlen(str), " BC");

			/*
			 * Note: the uses of %.*s in this function would be risky if the
			 * timezone names ever contain non-ASCII characters, since we are
			 * not being careful to do encoding-aware clipping.  However, all
			 * TZ abbreviations in the IANA database are plain ASCII.
			 */

			if (print_tz)
			{
				if (tzn)
					sprintf(str + strlen(str), " %.*s", MAXTZLEN, tzn);
				else
				{
					hour = -(tz / SECS_PER_HOUR);
					min = (abs(tz) / MINS_PER_HOUR) % MINS_PER_HOUR;
					if (min != 0)
						sprintf(str + strlen(str), "%+03d:%02d", hour, min);
					else
						sprintf(str + strlen(str), "%+03d", hour);
				}
			}
			break;

		case USE_GERMAN_DATES:
			/* German variant on European style */

			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);

			sprintf(str + 5, ".%04d %02d:%02d",
					(tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1),
					tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 */
			if (fsec != 0)
			{
				sprintf(str + strlen(str), ":%02d.%06d", tm->tm_sec, fsec);
				TrimTrailingZeros(str);
			}
			else
				sprintf(str + strlen(str), ":%02d", tm->tm_sec);

			if (tm->tm_year <= 0)
				sprintf(str + strlen(str), " BC");

			if (print_tz)
			{
				if (tzn)
					sprintf(str + strlen(str), " %.*s", MAXTZLEN, tzn);
				else
				{
					hour = -(tz / SECS_PER_HOUR);
					min = (abs(tz) / MINS_PER_HOUR) % MINS_PER_HOUR;
					if (min != 0)
						sprintf(str + strlen(str), "%+03d:%02d", hour, min);
					else
						sprintf(str + strlen(str), "%+03d", hour);
				}
			}
			break;

		case USE_POSTGRES_DATES:
		default:
			/* Backward-compatible with traditional Postgres abstime dates */

			day = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			tm->tm_wday = (int) ((day + date2j(2000, 1, 1) + 1) % 7);

			memcpy(str, days[tm->tm_wday], 3);
			strcpy(str + 3, " ");

			if (EuroDates)
				sprintf(str + 4, "%02d %3s", tm->tm_mday, months[tm->tm_mon - 1]);
			else
				sprintf(str + 4, "%3s %02d", months[tm->tm_mon - 1], tm->tm_mday);

			sprintf(str + 10, " %02d:%02d", tm->tm_hour, tm->tm_min);

			/*
			 * Print fractional seconds if any.  The field widths here should
			 * be at least equal to MAX_TIMESTAMP_PRECISION.
			 */
			if (fsec != 0)
			{
				sprintf(str + strlen(str), ":%02d.%06d", tm->tm_sec, fsec);
				TrimTrailingZeros(str);
			}
			else
				sprintf(str + strlen(str), ":%02d", tm->tm_sec);

			sprintf(str + strlen(str), " %04d",
					(tm->tm_year > 0) ? tm->tm_year : -(tm->tm_year - 1));
			if (tm->tm_year <= 0)
				sprintf(str + strlen(str), " BC");

			if (print_tz)
			{
				if (tzn)
					sprintf(str + strlen(str), " %.*s", MAXTZLEN, tzn);
				else
				{
					/*
					 * We have a time zone, but no string version. Use the
					 * numeric form, but be sure to include a leading space to
					 * avoid formatting something which would be rejected by
					 * the date/time parser later. - thomas 2001-10-19
					 */
					hour = -(tz / SECS_PER_HOUR);
					min = (abs(tz) / MINS_PER_HOUR) % MINS_PER_HOUR;
					if (min != 0)
						sprintf(str + strlen(str), " %+03d:%02d", hour, min);
					else
						sprintf(str + strlen(str), " %+03d", hour);
				}
			}
			break;
	}
}

int
GetEpochTime(struct tm *tm)
{
	struct tm  *t0;
	struct tm	tmbuf;
	time_t		epoch = 0;

	t0 = gmtime_r(&epoch, &tmbuf);

	if (t0)
	{
		tm->tm_year = t0->tm_year + 1900;
		tm->tm_mon = t0->tm_mon + 1;
		tm->tm_mday = t0->tm_mday;
		tm->tm_hour = t0->tm_hour;
		tm->tm_min = t0->tm_min;
		tm->tm_sec = t0->tm_sec;

		return 0;
	}

	return -1;
}								/* GetEpochTime() */

static void
abstime2tm(AbsoluteTime _time, int *tzp, struct tm *tm, char **tzn)
{
	time_t		time = (time_t) _time;
	struct tm  *tx;
	struct tm	tmbuf;

	errno = 0;
	if (tzp != NULL)
		tx = localtime_r(&time, &tmbuf);
	else
		tx = gmtime_r(&time, &tmbuf);

	if (!tx)
	{
		errno = PGTYPES_TS_BAD_TIMESTAMP;
		return;
	}

	tm->tm_year = tx->tm_year + 1900;
	tm->tm_mon = tx->tm_mon + 1;
	tm->tm_mday = tx->tm_mday;
	tm->tm_hour = tx->tm_hour;
	tm->tm_min = tx->tm_min;
	tm->tm_sec = tx->tm_sec;
	tm->tm_isdst = tx->tm_isdst;

#if defined(HAVE_STRUCT_TM_TM_ZONE)
	tm->tm_gmtoff = tx->tm_gmtoff;
	tm->tm_zone = tx->tm_zone;

	if (tzp != NULL)
	{
		/*
		 * We have a brute force time zone per SQL99? Then use it without
		 * change since we have already rotated to the time zone.
		 */
		*tzp = -tm->tm_gmtoff;	/* tm_gmtoff is Sun/DEC-ism */

		/*
		 * FreeBSD man pages indicate that this should work - tgl 97/04/23
		 */
		if (tzn != NULL)
		{
			/*
			 * Copy no more than MAXTZLEN bytes of timezone to tzn, in case it
			 * contains an error message, which doesn't fit in the buffer
			 */
			strlcpy(*tzn, tm->tm_zone, MAXTZLEN + 1);
			if (strlen(tm->tm_zone) > MAXTZLEN)
				tm->tm_isdst = -1;
		}
	}
	else
		tm->tm_isdst = -1;
#elif defined(HAVE_INT_TIMEZONE)
	if (tzp != NULL)
	{
		*tzp = (tm->tm_isdst > 0) ? TIMEZONE_GLOBAL - SECS_PER_HOUR : TIMEZONE_GLOBAL;

		if (tzn != NULL)
		{
			/*
			 * Copy no more than MAXTZLEN bytes of timezone to tzn, in case it
			 * contains an error message, which doesn't fit in the buffer
			 */
			strlcpy(*tzn, TZNAME_GLOBAL[tm->tm_isdst], MAXTZLEN + 1);
			if (strlen(TZNAME_GLOBAL[tm->tm_isdst]) > MAXTZLEN)
				tm->tm_isdst = -1;
		}
	}
	else
		tm->tm_isdst = -1;
#else							/* not (HAVE_STRUCT_TM_TM_ZONE ||
								 * HAVE_INT_TIMEZONE) */
	if (tzp != NULL)
	{
		/* default to UTC */
		*tzp = 0;
		if (tzn != NULL)
			*tzn = NULL;
	}
	else
		tm->tm_isdst = -1;
#endif
}

void
GetCurrentDateTime(struct tm *tm)
{
	int			tz;

	abstime2tm(time(NULL), &tz, tm, NULL);
}

void
dt2time(double jd, int *hour, int *min, int *sec, fsec_t *fsec)
{
	int64		time;

	time = jd;
	*hour = time / USECS_PER_HOUR;
	time -= (*hour) * USECS_PER_HOUR;
	*min = time / USECS_PER_MINUTE;
	time -= (*min) * USECS_PER_MINUTE;
	*sec = time / USECS_PER_SEC;
	*fsec = time - (*sec * USECS_PER_SEC);
}								/* dt2time() */



/* DecodeNumberField()
 * Interpret numeric string as a concatenated date or time field.
 * Use the context of previously decoded fields to help with
 * the interpretation.
 */
static int
DecodeNumberField(int len, char *str, int fmask,
				  int *tmask, struct tm *tm, fsec_t *fsec, bool *is2digits)
{
	char	   *cp;

	/*
	 * Have a decimal point? Then this is a date or something with a seconds
	 * field...
	 */
	if ((cp = strchr(str, '.')) != NULL)
	{
		char		fstr[7];
		int			i;

		cp++;

		/*
		 * OK, we have at most six digits to care about. Let's construct a
		 * string with those digits, zero-padded on the right, and then do the
		 * conversion to an integer.
		 *
		 * XXX This truncates the seventh digit, unlike rounding it as the
		 * backend does.
		 */
		for (i = 0; i < 6; i++)
			fstr[i] = *cp != '\0' ? *cp++ : '0';
		fstr[i] = '\0';
		*fsec = strtoint(fstr, NULL, 10);
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
			*is2digits = true;

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
			*is2digits = true;

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
}								/* DecodeNumberField() */


/* DecodeNumber()
 * Interpret plain numeric field as a date value in context.
 */
static int
DecodeNumber(int flen, char *str, int fmask,
			 int *tmask, struct tm *tm, fsec_t *fsec, bool *is2digits, bool EuroDates)
{
	int			val;
	char	   *cp;

	*tmask = 0;

	val = strtoint(str, &cp, 10);
	if (cp == str)
		return -1;

	if (*cp == '.')
	{
		/*
		 * More than two digits? Then could be a date or a run-together time:
		 * 2001.360 20011225 040506.789
		 */
		if (cp - str > 2)
			return DecodeNumberField(flen, str, (fmask | DTK_DATE_M),
									 tmask, tm, fsec, is2digits);

		*fsec = strtod(cp, &cp);
		if (*cp != '\0')
			return -1;
	}
	else if (*cp != '\0')
		return -1;

	/* Special case day of year? */
	if (flen == 3 && (fmask & DTK_M(YEAR)) && val >= 1 && val <= 366)
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
		if ((fmask & DTK_M(YEAR)) && !(fmask & DTK_M(DAY)) &&
			tm->tm_year >= 1 && tm->tm_year <= 31)
		{
			tm->tm_mday = tm->tm_year;
			*tmask = DTK_M(DAY);
		}

		tm->tm_year = val;
	}

	/* already have year? then could be month */
	else if ((fmask & DTK_M(YEAR)) && !(fmask & DTK_M(MONTH)) && val >= 1 && val <= MONTHS_PER_YEAR)
	{
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;
	}
	/* no year and EuroDates enabled? then could be day */
	else if ((EuroDates || (fmask & DTK_M(MONTH))) &&
			 !(fmask & DTK_M(YEAR)) && !(fmask & DTK_M(DAY)) &&
			 val >= 1 && val <= 31)
	{
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;
	}
	else if (!(fmask & DTK_M(MONTH)) && val >= 1 && val <= MONTHS_PER_YEAR)
	{
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;
	}
	else if (!(fmask & DTK_M(DAY)) && val >= 1 && val <= 31)
	{
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;
	}

	/*
	 * Check for 2 or 4 or more digits, but currently we reach here only if
	 * two digits. - thomas 2000-03-28
	 */
	else if (!(fmask & DTK_M(YEAR)) && (flen >= 4 || flen == 2))
	{
		*tmask = DTK_M(YEAR);
		tm->tm_year = val;

		/* adjust ONLY if exactly two digits... */
		*is2digits = (flen == 2);
	}
	else
		return -1;

	return 0;
}								/* DecodeNumber() */

/* DecodeDate()
 * Decode date string which includes delimiters.
 * Insist on a complete set of fields.
 */
static int
DecodeDate(char *str, int fmask, int *tmask, struct tm *tm, bool EuroDates)
{
	fsec_t		fsec;

	int			nf = 0;
	int			i,
				len;
	bool		bc = false;
	bool		is2digits = false;
	int			type,
				val,
				dmask = 0;
	char	   *field[MAXDATEFIELDS];

	/* parse this string... */
	while (*str != '\0' && nf < MAXDATEFIELDS)
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

		if (DecodeNumber(len, field[i], fmask, &dmask, tm, &fsec, &is2digits, EuroDates) != 0)
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
			return -1;
	}
	else if (is2digits)
	{
		if (tm->tm_year < 70)
			tm->tm_year += 2000;
		else if (tm->tm_year < 100)
			tm->tm_year += 1900;
	}

	return 0;
}								/* DecodeDate() */


/* DecodeTime()
 * Decode time string which includes delimiters.
 * Only check the lower limit on hours, since this same code
 *	can be used to represent time spans.
 */
int
DecodeTime(char *str, int *tmask, struct tm *tm, fsec_t *fsec)
{
	char	   *cp;

	*tmask = DTK_TIME_M;

	tm->tm_hour = strtoint(str, &cp, 10);
	if (*cp != ':')
		return -1;
	str = cp + 1;
	tm->tm_min = strtoint(str, &cp, 10);
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
		tm->tm_sec = strtoint(str, &cp, 10);
		if (*cp == '\0')
			*fsec = 0;
		else if (*cp == '.')
		{
			char		fstr[7];
			int			i;

			cp++;

			/*
			 * OK, we have at most six digits to care about. Let's construct a
			 * string with those digits, zero-padded on the right, and then do
			 * the conversion to an integer.
			 *
			 * XXX This truncates the seventh digit, unlike rounding it as the
			 * backend does.
			 */
			for (i = 0; i < 6; i++)
				fstr[i] = *cp != '\0' ? *cp++ : '0';
			fstr[i] = '\0';
			*fsec = strtoint(fstr, &cp, 10);
			if (*cp != '\0')
				return -1;
		}
		else
			return -1;
	}

	/* do a sanity check */
	if (tm->tm_hour < 0 || tm->tm_min < 0 || tm->tm_min > 59 ||
		tm->tm_sec < 0 || tm->tm_sec > 59 || *fsec >= USECS_PER_SEC)
		return -1;

	return 0;
}								/* DecodeTime() */

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
	hr = strtoint(str + 1, &cp, 10);

	/* explicit delimiter? */
	if (*cp == ':')
		min = strtoint(cp + 1, &cp, 10);
	/* otherwise, might have run things together... */
	else if (*cp == '\0' && (len = strlen(str)) > 3)
	{
		min = strtoint(str + len - 2, &cp, 10);
		if (min < 0 || min >= 60)
			return -1;

		*(str + len - 2) = '\0';
		hr = strtoint(str + 1, &cp, 10);
		if (hr < 0 || hr > 13)
			return -1;
	}
	else
		min = 0;

	tz = (hr * MINS_PER_HOUR + min) * SECS_PER_MINUTE;
	if (*str == '-')
		tz = -tz;

	*tzp = -tz;
	return *cp != '\0';
}								/* DecodeTimezone() */


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
	while (*cp != '\0' && isalpha((unsigned char) *cp))
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
			*tzp = -(val + tz);
			break;

		default:
			return -1;
	}

	return 0;
}								/* DecodePosixTimezone() */

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
 *
 * The "lowstr" work buffer must have at least strlen(timestr) + MAXDATEFIELDS
 * bytes of space.  On output, field[] entries will point into it.
 * The field[] and ftype[] arrays must have at least MAXDATEFIELDS entries.
 */
int
ParseDateTime(char *timestr, char *lowstr,
			  char **field, int *ftype, int *numfields, char **endstr)
{
	int			nf = 0;
	char	   *lp = lowstr;

	*endstr = timestr;
	/* outer loop through fields */
	while (*(*endstr) != '\0')
	{
		/* Record start of current field */
		if (nf >= MAXDATEFIELDS)
			return -1;
		field[nf] = lp;

		/* leading digit? then date or time */
		if (isdigit((unsigned char) *(*endstr)))
		{
			*lp++ = *(*endstr)++;
			while (isdigit((unsigned char) *(*endstr)))
				*lp++ = *(*endstr)++;

			/* time field? */
			if (*(*endstr) == ':')
			{
				ftype[nf] = DTK_TIME;
				*lp++ = *(*endstr)++;
				while (isdigit((unsigned char) *(*endstr)) ||
					   (*(*endstr) == ':') || (*(*endstr) == '.'))
					*lp++ = *(*endstr)++;
			}
			/* date field? allow embedded text month */
			else if (*(*endstr) == '-' || *(*endstr) == '/' || *(*endstr) == '.')
			{
				/* save delimiting character to use later */
				char	   *dp = (*endstr);

				*lp++ = *(*endstr)++;
				/* second field is all digits? then no embedded text month */
				if (isdigit((unsigned char) *(*endstr)))
				{
					ftype[nf] = (*dp == '.') ? DTK_NUMBER : DTK_DATE;
					while (isdigit((unsigned char) *(*endstr)))
						*lp++ = *(*endstr)++;

					/*
					 * insist that the delimiters match to get a three-field
					 * date.
					 */
					if (*(*endstr) == *dp)
					{
						ftype[nf] = DTK_DATE;
						*lp++ = *(*endstr)++;
						while (isdigit((unsigned char) *(*endstr)) || (*(*endstr) == *dp))
							*lp++ = *(*endstr)++;
					}
				}
				else
				{
					ftype[nf] = DTK_DATE;
					while (isalnum((unsigned char) *(*endstr)) || (*(*endstr) == *dp))
						*lp++ = pg_tolower((unsigned char) *(*endstr)++);
				}
			}

			/*
			 * otherwise, number only and will determine year, month, day, or
			 * concatenated fields later...
			 */
			else
				ftype[nf] = DTK_NUMBER;
		}
		/* Leading decimal point? Then fractional seconds... */
		else if (*(*endstr) == '.')
		{
			*lp++ = *(*endstr)++;
			while (isdigit((unsigned char) *(*endstr)))
				*lp++ = *(*endstr)++;

			ftype[nf] = DTK_NUMBER;
		}

		/*
		 * text? then date string, month, day of week, special, or timezone
		 */
		else if (isalpha((unsigned char) *(*endstr)))
		{
			ftype[nf] = DTK_STRING;
			*lp++ = pg_tolower((unsigned char) *(*endstr)++);
			while (isalpha((unsigned char) *(*endstr)))
				*lp++ = pg_tolower((unsigned char) *(*endstr)++);

			/*
			 * Full date string with leading text month? Could also be a POSIX
			 * time zone...
			 */
			if (*(*endstr) == '-' || *(*endstr) == '/' || *(*endstr) == '.')
			{
				char	   *dp = (*endstr);

				ftype[nf] = DTK_DATE;
				*lp++ = *(*endstr)++;
				while (isdigit((unsigned char) *(*endstr)) || *(*endstr) == *dp)
					*lp++ = *(*endstr)++;
			}
		}
		/* skip leading spaces */
		else if (isspace((unsigned char) *(*endstr)))
		{
			(*endstr)++;
			continue;
		}
		/* sign? then special or numeric timezone */
		else if (*(*endstr) == '+' || *(*endstr) == '-')
		{
			*lp++ = *(*endstr)++;
			/* soak up leading whitespace */
			while (isspace((unsigned char) *(*endstr)))
				(*endstr)++;
			/* numeric timezone? */
			if (isdigit((unsigned char) *(*endstr)))
			{
				ftype[nf] = DTK_TZ;
				*lp++ = *(*endstr)++;
				while (isdigit((unsigned char) *(*endstr)) ||
					   (*(*endstr) == ':') || (*(*endstr) == '.'))
					*lp++ = *(*endstr)++;
			}
			/* special? */
			else if (isalpha((unsigned char) *(*endstr)))
			{
				ftype[nf] = DTK_SPECIAL;
				*lp++ = pg_tolower((unsigned char) *(*endstr)++);
				while (isalpha((unsigned char) *(*endstr)))
					*lp++ = pg_tolower((unsigned char) *(*endstr)++);
			}
			/* otherwise something wrong... */
			else
				return -1;
		}
		/* ignore punctuation but use as delimiter */
		else if (ispunct((unsigned char) *(*endstr)))
		{
			(*endstr)++;
			continue;
		}
		/* otherwise, something is not right... */
		else
			return -1;

		/* force in a delimiter after each field */
		*lp++ = '\0';
		nf++;
	}

	*numfields = nf;

	return 0;
}								/* ParseDateTime() */


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
			   int *dtype, struct tm *tm, fsec_t *fsec, bool EuroDates)
{
	int			fmask = 0,
				tmask,
				type;
	int			ptype = 0;		/* "prefix type" for ISO y2001m02d04 format */
	int			i;
	int			val;
	int			mer = HR24;
	bool		haveTextMonth = false;
	bool		is2digits = false;
	bool		bc = false;
	int			t = 0;
	int		   *tzp = &t;

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
					int			jday;

					if (tzp == NULL)
						return -1;

					jday = strtoint(field[i], &cp, 10);
					if (*cp != '-')
						return -1;

					j2date(jday, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
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
						 * field? Then we are in trouble with a date and time
						 * already...
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
						 * Then read the rest of the field as a concatenated
						 * time
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
				else if (DecodeDate(field[i], fmask, &tmask, tm, EuroDates) != 0)
					return -1;
				break;

			case DTK_TIME:
				if (DecodeTime(field[i], &tmask, tm, fsec) != 0)
					return -1;

				/*
				 * Check upper limit on hours; other limits checked in
				 * DecodeTime()
				 */
				/* test for > 24:00:00 */
				if (tm->tm_hour > 24 ||
					(tm->tm_hour == 24 && (tm->tm_min > 0 || tm->tm_sec > 0)))
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
					 * Already have a time zone? Then maybe this is the second
					 * field of a POSIX time: EST+3 (equivalent to PST)
					 */
					if (i > 0 && (fmask & DTK_M(TZ)) != 0 &&
						ftype[i - 1] == DTK_TZ &&
						isalpha((unsigned char) *field[i - 1]))
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
					int			value;

					value = strtoint(field[i], &cp, 10);

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
							tm->tm_year = value;
							tmask = DTK_M(YEAR);
							break;

						case DTK_MONTH:

							/*
							 * already have a month and hour? then assume
							 * minutes
							 */
							if ((fmask & DTK_M(MONTH)) != 0 &&
								(fmask & DTK_M(HOUR)) != 0)
							{
								tm->tm_min = value;
								tmask = DTK_M(MINUTE);
							}
							else
							{
								tm->tm_mon = value;
								tmask = DTK_M(MONTH);
							}
							break;

						case DTK_DAY:
							tm->tm_mday = value;
							tmask = DTK_M(DAY);
							break;

						case DTK_HOUR:
							tm->tm_hour = value;
							tmask = DTK_M(HOUR);
							break;

						case DTK_MINUTE:
							tm->tm_min = value;
							tmask = DTK_M(MINUTE);
							break;

						case DTK_SECOND:
							tm->tm_sec = value;
							tmask = DTK_M(SECOND);
							if (*cp == '.')
							{
								double		frac;

								frac = strtod(cp, &cp);
								if (*cp != '\0')
									return -1;
								*fsec = frac * 1000000;
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
							j2date(value, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
							/* fractional Julian Day? */
							if (*cp == '.')
							{
								double		time;

								time = strtod(cp, &cp);
								if (*cp != '\0')
									return -1;

								tmask |= DTK_TIME_M;
								dt2time((time * USECS_PER_DAY), &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
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
					if (cp != NULL && !(fmask & DTK_DATE_M))
					{
						if (DecodeDate(field[i], fmask, &tmask, tm, EuroDates) != 0)
							return -1;
					}
					/* embedded decimal and several digits before? */
					else if (cp != NULL && flen - strlen(cp) > 2)
					{
						/*
						 * Interpret as a concatenated date or time Set the
						 * type field to allow decoding other fields later.
						 * Example: 20011223 or 040506
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
										  &tmask, tm, fsec, &is2digits, EuroDates) != 0)
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
							case DTK_NOW:
								tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								GetCurrentDateTime(tm);
								break;

							case DTK_YESTERDAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentDateTime(tm);
								j2date(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - 1,
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
								j2date(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + 1,
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
						 * already have a (numeric) month? then see if we can
						 * substitute...
						 */
						if ((fmask & DTK_M(MONTH)) && !haveTextMonth &&
							!(fmask & DTK_M(DAY)) && tm->tm_mon >= 1 && tm->tm_mon <= 31)
						{
							tm->tm_mday = tm->tm_mon;
							tmask = DTK_M(DAY);
						}
						haveTextMonth = true;
						tm->tm_mon = val;
						break;

					case DTZMOD:

						/*
						 * daylight savings time modifier (solves "MET DST"
						 * syntax)
						 */
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp -= val;
						break;

					case DTZ:

						/*
						 * set mask for TZ here _or_ check for DTZ later when
						 * getting default timezone
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp = -val;
						ftype[i] = DTK_TZ;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return -1;
						*tzp = -val;
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
						 * This is a filler field "t" indicating that the next
						 * field is time. Try to verify that this is sensible.
						 */
						tmask = 0;

						/* No preceding date? Then quit... */
						if ((fmask & DTK_DATE_M) != DTK_DATE_M)
							return -1;

						/***
						 * We will need one of the following fields:
						 *	DTK_NUMBER should be hhmmss.fff
						 *	DTK_TIME should be hh:mm:ss.fff
						 *	DTK_DATE should be hhmmss-zz
						 ***/
						if (i >= nf - 1 ||
							(ftype[i + 1] != DTK_NUMBER &&
							 ftype[i + 1] != DTK_TIME &&
							 ftype[i + 1] != DTK_DATE))
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
			return -1;
	}
	else if (is2digits)
	{
		if (tm->tm_year < 70)
			tm->tm_year += 2000;
		else if (tm->tm_year < 100)
			tm->tm_year += 1900;
	}

	if (mer != HR24 && tm->tm_hour > 12)
		return -1;
	if (mer == AM && tm->tm_hour == 12)
		tm->tm_hour = 0;
	else if (mer == PM && tm->tm_hour != 12)
		tm->tm_hour += 12;

	/* do additional checking for full date specs... */
	if (*dtype == DTK_DATE)
	{
		if ((fmask & DTK_DATE_M) != DTK_DATE_M)
			return ((fmask & DTK_TIME_M) == DTK_TIME_M) ? 1 : -1;

		/*
		 * check for valid day of month, now that we know for sure the month
		 * and year...
		 */
		if (tm->tm_mday < 1 || tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
			return -1;

		/*
		 * backend tried to find local timezone here but we don't use the
		 * result afterwards anyway so we only check for this error: daylight
		 * savings time modifier but no standard timezone?
		 */
		if ((fmask & DTK_DATE_M) == DTK_DATE_M && tzp != NULL && !(fmask & DTK_M(TZ)) && (fmask & DTK_M(DTZMOD)))
			return -1;
	}

	return 0;
}								/* DecodeDateTime() */

/* Function works as follows:
 *
 *
 * */

static char *
find_end_token(char *str, char *fmt)
{
	/*
	 * str: here is28the day12the hour fmt: here is%dthe day%hthe hour
	 *
	 * we extract the 28, we read the percent sign and the type "d" then this
	 * functions gets called as find_end_token("28the day12the hour", "the
	 * day%hthehour")
	 *
	 * fmt points to "the day%hthehour", next_percent points to %hthehour and
	 * we have to find a match for everything between these positions ("the
	 * day"). We look for "the day" in str and know that the pattern we are
	 * about to scan ends where this string starts (right after the "28")
	 *
	 * At the end, *fmt is '\0' and *str isn't. end_position then is
	 * unchanged.
	 */
	char	   *end_position = NULL;
	char	   *next_percent,
			   *subst_location = NULL;
	int			scan_offset = 0;
	char		last_char;

	/* are we at the end? */
	if (!*fmt)
	{
		end_position = fmt;
		return end_position;
	}

	/* not at the end */
	while (fmt[scan_offset] == '%' && fmt[scan_offset + 1])
	{
		/*
		 * there is no delimiter, skip to the next delimiter if we're reading
		 * a number and then something that is not a number "9:15pm", we might
		 * be able to recover with the strtol end pointer. Go for the next
		 * percent sign
		 */
		scan_offset += 2;
	}
	next_percent = strchr(fmt + scan_offset, '%');
	if (next_percent)
	{
		/*
		 * we don't want to allocate extra memory, so we temporarily set the
		 * '%' sign to '\0' and call strstr However since we allow whitespace
		 * to float around everything, we have to shorten the pattern until we
		 * reach a non-whitespace character
		 */

		subst_location = next_percent;
		while (*(subst_location - 1) == ' ' && subst_location - 1 > fmt + scan_offset)
			subst_location--;
		last_char = *subst_location;
		*subst_location = '\0';

		/*
		 * the haystack is the str and the needle is the original fmt but it
		 * ends at the position where the next percent sign would be
		 */

		/*
		 * There is one special case. Imagine: str = " 2", fmt = "%d %...",
		 * since we want to allow blanks as "dynamic" padding we have to
		 * accept this. Now, we are called with a fmt of " %..." and look for
		 * " " in str. We find it at the first position and never read the
		 * 2...
		 */
		while (*str == ' ')
			str++;
		end_position = strstr(str, fmt + scan_offset);
		*subst_location = last_char;
	}
	else
	{
		/*
		 * there is no other percent sign. So everything up to the end has to
		 * match.
		 */
		end_position = str + strlen(str);
	}
	if (!end_position)
	{
		/*
		 * maybe we have the following case:
		 *
		 * str = "4:15am" fmt = "%M:%S %p"
		 *
		 * at this place we could have
		 *
		 * str = "15am" fmt = " %p"
		 *
		 * and have set fmt to " " because overwrote the % sign with a NULL
		 *
		 * In this case where we would have to match a space but can't find
		 * it, set end_position to the end of the string
		 */
		if ((fmt + scan_offset)[0] == ' ' && fmt + scan_offset + 1 == subst_location)
			end_position = str + strlen(str);
	}
	return end_position;
}

static int
pgtypes_defmt_scan(union un_fmt_comb *scan_val, int scan_type, char **pstr, char *pfmt)
{
	/*
	 * scan everything between pstr and pstr_end. This is not including the
	 * last character so we might set it to '\0' for the parsing
	 */

	char		last_char;
	int			err = 0;
	char	   *pstr_end;
	char	   *strtol_end = NULL;

	while (**pstr == ' ')
		pstr++;
	pstr_end = find_end_token(*pstr, pfmt);
	if (!pstr_end)
	{
		/* there was an error, no match */
		return 1;
	}
	last_char = *pstr_end;
	*pstr_end = '\0';

	switch (scan_type)
	{
		case PGTYPES_TYPE_UINT:

			/*
			 * numbers may be blank-padded, this is the only deviation from
			 * the fmt-string we accept
			 */
			while (**pstr == ' ')
				(*pstr)++;
			errno = 0;
			scan_val->uint_val = (unsigned int) strtol(*pstr, &strtol_end, 10);
			if (errno)
				err = 1;
			break;
		case PGTYPES_TYPE_UINT_LONG:
			while (**pstr == ' ')
				(*pstr)++;
			errno = 0;
			scan_val->luint_val = (unsigned long int) strtol(*pstr, &strtol_end, 10);
			if (errno)
				err = 1;
			break;
		case PGTYPES_TYPE_STRING_MALLOCED:
			scan_val->str_val = pgtypes_strdup(*pstr);
			if (scan_val->str_val == NULL)
				err = 1;
			break;
	}
	if (strtol_end && *strtol_end)
		*pstr = strtol_end;
	else
		*pstr = pstr_end;
	*pstr_end = last_char;
	return err;
}

/* XXX range checking */
int
PGTYPEStimestamp_defmt_scan(char **str, char *fmt, timestamp * d,
							int *year, int *month, int *day,
							int *hour, int *minute, int *second,
							int *tz)
{
	union un_fmt_comb scan_val;
	int			scan_type;

	char	   *pstr,
			   *pfmt,
			   *tmp;
	int			err = 1;
	unsigned int j;
	struct tm	tm;

	pfmt = fmt;
	pstr = *str;

	while (*pfmt)
	{
		err = 0;
		while (*pfmt == ' ')
			pfmt++;
		while (*pstr == ' ')
			pstr++;
		if (*pfmt != '%')
		{
			if (*pfmt == *pstr)
			{
				pfmt++;
				pstr++;
			}
			else
			{
				/* Error: no match */
				err = 1;
				return err;
			}
			continue;
		}
		/* here *pfmt equals '%' */
		pfmt++;
		switch (*pfmt)
		{
			case 'a':
				pfmt++;

				/*
				 * we parse the day and see if it is a week day but we do not
				 * check if the week day really matches the date
				 */
				err = 1;
				j = 0;
				while (pgtypes_date_weekdays_short[j])
				{
					if (strncmp(pgtypes_date_weekdays_short[j], pstr,
								strlen(pgtypes_date_weekdays_short[j])) == 0)
					{
						/* found it */
						err = 0;
						pstr += strlen(pgtypes_date_weekdays_short[j]);
						break;
					}
					j++;
				}
				break;
			case 'A':
				/* see note above */
				pfmt++;
				err = 1;
				j = 0;
				while (days[j])
				{
					if (strncmp(days[j], pstr, strlen(days[j])) == 0)
					{
						/* found it */
						err = 0;
						pstr += strlen(days[j]);
						break;
					}
					j++;
				}
				break;
			case 'b':
			case 'h':
				pfmt++;
				err = 1;
				j = 0;
				while (months[j])
				{
					if (strncmp(months[j], pstr, strlen(months[j])) == 0)
					{
						/* found it */
						err = 0;
						pstr += strlen(months[j]);
						*month = j + 1;
						break;
					}
					j++;
				}
				break;
			case 'B':
				/* see note above */
				pfmt++;
				err = 1;
				j = 0;
				while (pgtypes_date_months[j])
				{
					if (strncmp(pgtypes_date_months[j], pstr, strlen(pgtypes_date_months[j])) == 0)
					{
						/* found it */
						err = 0;
						pstr += strlen(pgtypes_date_months[j]);
						*month = j + 1;
						break;
					}
					j++;
				}
				break;
			case 'c':
				/* XXX */
				break;
			case 'C':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*year = scan_val.uint_val * 100;
				break;
			case 'd':
			case 'e':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*day = scan_val.uint_val;
				break;
			case 'D':

				/*
				 * we have to concatenate the strings in order to be able to
				 * find the end of the substitution
				 */
				pfmt++;
				tmp = pgtypes_alloc(strlen("%m/%d/%y") + strlen(pstr) + 1);
				if (!tmp)
					return 1;
				strcpy(tmp, "%m/%d/%y");
				strcat(tmp, pfmt);
				err = PGTYPEStimestamp_defmt_scan(&pstr, tmp, d, year, month, day, hour, minute, second, tz);
				free(tmp);
				return err;
			case 'm':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*month = scan_val.uint_val;
				break;
			case 'y':
			case 'g':			/* XXX difference to y (ISO) */
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (*year < 0)
				{
					/* not yet set */
					*year = scan_val.uint_val;
				}
				else
					*year += scan_val.uint_val;
				if (*year < 100)
					*year += 1900;
				break;
			case 'G':
				/* XXX difference to %V (ISO) */
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*year = scan_val.uint_val;
				break;
			case 'H':
			case 'I':
			case 'k':
			case 'l':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*hour += scan_val.uint_val;
				break;
			case 'j':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);

				/*
				 * XXX what should we do with that? We could say that it's
				 * sufficient if we have the year and the day within the year
				 * to get at least a specific day.
				 */
				break;
			case 'M':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*minute = scan_val.uint_val;
				break;
			case 'n':
				pfmt++;
				if (*pstr == '\n')
					pstr++;
				else
					err = 1;
				break;
			case 'p':
				err = 1;
				pfmt++;
				if (strncmp(pstr, "am", 2) == 0)
				{
					*hour += 0;
					err = 0;
					pstr += 2;
				}
				if (strncmp(pstr, "a.m.", 4) == 0)
				{
					*hour += 0;
					err = 0;
					pstr += 4;
				}
				if (strncmp(pstr, "pm", 2) == 0)
				{
					*hour += 12;
					err = 0;
					pstr += 2;
				}
				if (strncmp(pstr, "p.m.", 4) == 0)
				{
					*hour += 12;
					err = 0;
					pstr += 4;
				}
				break;
			case 'P':
				err = 1;
				pfmt++;
				if (strncmp(pstr, "AM", 2) == 0)
				{
					*hour += 0;
					err = 0;
					pstr += 2;
				}
				if (strncmp(pstr, "A.M.", 4) == 0)
				{
					*hour += 0;
					err = 0;
					pstr += 4;
				}
				if (strncmp(pstr, "PM", 2) == 0)
				{
					*hour += 12;
					err = 0;
					pstr += 2;
				}
				if (strncmp(pstr, "P.M.", 4) == 0)
				{
					*hour += 12;
					err = 0;
					pstr += 4;
				}
				break;
			case 'r':
				pfmt++;
				tmp = pgtypes_alloc(strlen("%I:%M:%S %p") + strlen(pstr) + 1);
				if (!tmp)
					return 1;
				strcpy(tmp, "%I:%M:%S %p");
				strcat(tmp, pfmt);
				err = PGTYPEStimestamp_defmt_scan(&pstr, tmp, d, year, month, day, hour, minute, second, tz);
				free(tmp);
				return err;
			case 'R':
				pfmt++;
				tmp = pgtypes_alloc(strlen("%H:%M") + strlen(pstr) + 1);
				if (!tmp)
					return 1;
				strcpy(tmp, "%H:%M");
				strcat(tmp, pfmt);
				err = PGTYPEStimestamp_defmt_scan(&pstr, tmp, d, year, month, day, hour, minute, second, tz);
				free(tmp);
				return err;
			case 's':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT_LONG;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				/* number of seconds in scan_val.luint_val */
				{
					struct tm  *tms;
					struct tm	tmbuf;
					time_t		et = (time_t) scan_val.luint_val;

					tms = gmtime_r(&et, &tmbuf);

					if (tms)
					{
						*year = tms->tm_year + 1900;
						*month = tms->tm_mon + 1;
						*day = tms->tm_mday;
						*hour = tms->tm_hour;
						*minute = tms->tm_min;
						*second = tms->tm_sec;
					}
					else
						err = 1;
				}
				break;
			case 'S':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*second = scan_val.uint_val;
				break;
			case 't':
				pfmt++;
				if (*pstr == '\t')
					pstr++;
				else
					err = 1;
				break;
			case 'T':
				pfmt++;
				tmp = pgtypes_alloc(strlen("%H:%M:%S") + strlen(pstr) + 1);
				if (!tmp)
					return 1;
				strcpy(tmp, "%H:%M:%S");
				strcat(tmp, pfmt);
				err = PGTYPEStimestamp_defmt_scan(&pstr, tmp, d, year, month, day, hour, minute, second, tz);
				free(tmp);
				return err;
			case 'u':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (scan_val.uint_val < 1 || scan_val.uint_val > 7)
					err = 1;
				break;
			case 'U':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (scan_val.uint_val > 53)
					err = 1;
				break;
			case 'V':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (scan_val.uint_val < 1 || scan_val.uint_val > 53)
					err = 1;
				break;
			case 'w':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (scan_val.uint_val > 6)
					err = 1;
				break;
			case 'W':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (scan_val.uint_val > 53)
					err = 1;
				break;
			case 'x':
			case 'X':
				/* XXX */
				break;
			case 'Y':
				pfmt++;
				scan_type = PGTYPES_TYPE_UINT;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				*year = scan_val.uint_val;
				break;
			case 'z':
				pfmt++;
				scan_type = PGTYPES_TYPE_STRING_MALLOCED;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (!err)
				{
					err = DecodeTimezone(scan_val.str_val, tz);
					free(scan_val.str_val);
				}
				break;
			case 'Z':
				pfmt++;
				scan_type = PGTYPES_TYPE_STRING_MALLOCED;
				err = pgtypes_defmt_scan(&scan_val, scan_type, &pstr, pfmt);
				if (!err)
				{
					/*
					 * XXX use DecodeSpecial instead?  Do we need strcasecmp
					 * here?
					 */
					err = 1;
					for (j = 0; j < szdatetktbl; j++)
					{
						if ((datetktbl[j].type == TZ || datetktbl[j].type == DTZ) &&
							pg_strcasecmp(datetktbl[j].token,
										  scan_val.str_val) == 0)
						{
							*tz = -datetktbl[j].value;
							err = 0;
							break;
						}
					}
					free(scan_val.str_val);
				}
				break;
			case '+':
				/* XXX */
				break;
			case '%':
				pfmt++;
				if (*pstr == '%')
					pstr++;
				else
					err = 1;
				break;
			default:
				err = 1;
		}
	}
	if (!err)
	{
		if (*second < 0)
			*second = 0;
		if (*minute < 0)
			*minute = 0;
		if (*hour < 0)
			*hour = 0;
		if (*day < 0)
		{
			err = 1;
			*day = 1;
		}
		if (*month < 0)
		{
			err = 1;
			*month = 1;
		}
		if (*year < 0)
		{
			err = 1;
			*year = 1970;
		}

		if (*second > 59)
		{
			err = 1;
			*second = 0;
		}
		if (*minute > 59)
		{
			err = 1;
			*minute = 0;
		}
		if (*hour > 24 ||		/* test for > 24:00:00 */
			(*hour == 24 && (*minute > 0 || *second > 0)))
		{
			err = 1;
			*hour = 0;
		}
		if (*month > MONTHS_PER_YEAR)
		{
			err = 1;
			*month = 1;
		}
		if (*day > day_tab[isleap(*year)][*month - 1])
		{
			*day = day_tab[isleap(*year)][*month - 1];
			err = 1;
		}

		tm.tm_sec = *second;
		tm.tm_min = *minute;
		tm.tm_hour = *hour;
		tm.tm_mday = *day;
		tm.tm_mon = *month;
		tm.tm_year = *year;

		tm2timestamp(&tm, 0, tz, d);
	}
	return err;
}

/* XXX: 1900 is compiled in as the base for years */
