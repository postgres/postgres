/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson (arthur_david_olson@nih.gov).
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/zic.c,v 1.22 2007/10/26 13:30:10 tgl Exp $
 */

#include "postgres_fe.h"

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <limits.h>
#include <locale.h>

extern int	optind;
extern char *optarg;

#include "private.h"
#include "pgtz.h"
#include "tzfile.h"

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifndef WIN32
#ifdef S_IRUSR
#define MKDIR_UMASK (S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)
#else
#define MKDIR_UMASK 0755
#endif
#endif

static char elsieid[] = "@(#)zic.c	7.115";

/*
 * On some ancient hosts, predicates like `isspace(C)' are defined
 * only if isascii(C) || C == EOF.	Modern hosts obey the C Standard,
 * which says they are defined only if C == ((unsigned char) C) || C == EOF.
 * Neither the C Standard nor Posix require that `isascii' exist.
 * For portability, we check both ancient and modern requirements.
 * If isascii is not defined, the isascii check succeeds trivially.
 */
#include <ctype.h>
#ifndef isascii
#define isascii(x) 1
#endif

struct rule
{
	const char *r_filename;
	int			r_linenum;
	const char *r_name;

	int			r_loyear;		/* for example, 1986 */
	int			r_hiyear;		/* for example, 1986 */
	const char *r_yrtype;

	int			r_month;		/* 0..11 */

	int			r_dycode;		/* see below */
	int			r_dayofmonth;
	int			r_wday;

	long		r_tod;			/* time from midnight */
	int			r_todisstd;		/* above is standard time if TRUE */
	/* or wall clock time if FALSE */
	int			r_todisgmt;		/* above is GMT if TRUE */
	/* or local time if FALSE */
	long		r_stdoff;		/* offset from standard time */
	const char *r_abbrvar;		/* variable part of abbreviation */

	int			r_todo;			/* a rule to do (used in outzone) */
	pg_time_t	r_temp;			/* used in outzone */
};

/*
 *	r_dycode		r_dayofmonth	r_wday
 */

#define DC_DOM		0	/* 1..31 */		/* unused */
#define DC_DOWGEQ	1	/* 1..31 */		/* 0..6 (Sun..Sat) */
#define DC_DOWLEQ	2	/* 1..31 */		/* 0..6 (Sun..Sat) */

struct zone
{
	const char *z_filename;
	int			z_linenum;

	const char *z_name;
	long		z_gmtoff;
	const char *z_rule;
	const char *z_format;

	long		z_stdoff;

	struct rule *z_rules;
	int			z_nrules;

	struct rule z_untilrule;
	pg_time_t	z_untiltime;
};

extern int	link(const char *fromname, const char *toname);
static void addtt(const pg_time_t starttime, int type);
static int addtype(long gmtoff, const char *abbr, int isdst,
		int ttisstd, int ttisgmt);
static void leapadd(const pg_time_t t, int positive, int rolling, int count);
static void adjleap(void);
static void associate(void);
static int	ciequal(const char *ap, const char *bp);
static void convert(long val, char *buf);
static void dolink(const char *fromfile, const char *tofile);
static void doabbr(char *abbr, const char *format,
	   const char *letters, int isdst);
static void eat(const char *name, int num);
static void eats(const char *name, int num,
	 const char *rname, int rnum);
static long eitol(int i);
static void error(const char *message);
static char **getfields(char *buf);
static long gethms(const char *string, const char *errstrng,
	   int signable);
static void infile(const char *filename);
static void inleap(char **fields, int nfields);
static void inlink(char **fields, int nfields);
static void inrule(char **fields, int nfields);
static int	inzcont(char **fields, int nfields);
static int	inzone(char **fields, int nfields);
static int	inzsub(char **fields, int nfields, int iscont);
static int	itsabbr(const char *abbr, const char *word);
static int	itsdir(const char *name);
static int	lowerit(int c);
static char *memcheck(char *tocheck);
static int	mkdirs(char *filename);
static void newabbr(const char *abbr);
static long oadd(long t1, long t2);
static void outzone(const struct zone * zp, int ntzones);
static void puttzcode(long code, FILE *fp);
static int	rcomp(const void *leftp, const void *rightp);
static pg_time_t rpytime(const struct rule * rp, int wantedy);
static void rulesub(struct rule * rp,
		const char *loyearp, const char *hiyearp,
		const char *typep, const char *monthp,
		const char *dayp, const char *timep);
static void setboundaries(void);
static pg_time_t tadd(const pg_time_t t1, long t2);
static void usage(void);
static void writezone(const char *name);
static int	yearistype(int year, const char *type);

static int	charcnt;
static int	errors;
static const char *filename;
static int	leapcnt;
static int	linenum;
static pg_time_t max_time;
static int	max_year;
static int	max_year_representable;
static pg_time_t min_time;
static int	min_year;
static int	min_year_representable;
static int	noise;
static const char *rfilename;
static int	rlinenum;
static const char *progname;
static int	timecnt;
static int	typecnt;

/*
 * Line codes.
 */

#define LC_RULE		0
#define LC_ZONE		1
#define LC_LINK		2
#define LC_LEAP		3

/*
 * Which fields are which on a Zone line.
 */

#define ZF_NAME		1
#define ZF_GMTOFF	2
#define ZF_RULE		3
#define ZF_FORMAT	4
#define ZF_TILYEAR	5
#define ZF_TILMONTH 6
#define ZF_TILDAY	7
#define ZF_TILTIME	8
#define ZONE_MINFIELDS	5
#define ZONE_MAXFIELDS	9

/*
 * Which fields are which on a Zone continuation line.
 */

#define ZFC_GMTOFF	0
#define ZFC_RULE	1
#define ZFC_FORMAT	2
#define ZFC_TILYEAR 3
#define ZFC_TILMONTH	4
#define ZFC_TILDAY	5
#define ZFC_TILTIME 6
#define ZONEC_MINFIELDS 3
#define ZONEC_MAXFIELDS 7

/*
 * Which files are which on a Rule line.
 */

#define RF_NAME		1
#define RF_LOYEAR	2
#define RF_HIYEAR	3
#define RF_COMMAND	4
#define RF_MONTH	5
#define RF_DAY		6
#define RF_TOD		7
#define RF_STDOFF	8
#define RF_ABBRVAR	9
#define RULE_FIELDS 10

/*
 * Which fields are which on a Link line.
 */

#define LF_FROM		1
#define LF_TO		2
#define LINK_FIELDS 3

/*
 * Which fields are which on a Leap line.
 */

#define LP_YEAR		1
#define LP_MONTH	2
#define LP_DAY		3
#define LP_TIME		4
#define LP_CORR		5
#define LP_ROLL		6
#define LEAP_FIELDS 7

/*
 * Year synonyms.
 */

#define YR_MINIMUM	0
#define YR_MAXIMUM	1
#define YR_ONLY		2

static struct rule *rules;
static int	nrules;				/* number of rules */

static struct zone *zones;
static int	nzones;				/* number of zones */

struct link
{
	const char *l_filename;
	int			l_linenum;
	const char *l_from;
	const char *l_to;
};

static struct link *links;
static int	nlinks;

struct lookup
{
	const char *l_word;
	const int	l_value;
};

static struct lookup const *byword(const char *string,
	   const struct lookup * lp);

static struct lookup const line_codes[] = {
	{"Rule", LC_RULE},
	{"Zone", LC_ZONE},
	{"Link", LC_LINK},
	{"Leap", LC_LEAP},
	{NULL, 0}
};

static struct lookup const mon_names[] = {
	{"January", TM_JANUARY},
	{"February", TM_FEBRUARY},
	{"March", TM_MARCH},
	{"April", TM_APRIL},
	{"May", TM_MAY},
	{"June", TM_JUNE},
	{"July", TM_JULY},
	{"August", TM_AUGUST},
	{"September", TM_SEPTEMBER},
	{"October", TM_OCTOBER},
	{"November", TM_NOVEMBER},
	{"December", TM_DECEMBER},
	{NULL, 0}
};

static struct lookup const wday_names[] = {
	{"Sunday", TM_SUNDAY},
	{"Monday", TM_MONDAY},
	{"Tuesday", TM_TUESDAY},
	{"Wednesday", TM_WEDNESDAY},
	{"Thursday", TM_THURSDAY},
	{"Friday", TM_FRIDAY},
	{"Saturday", TM_SATURDAY},
	{NULL, 0}
};

static struct lookup const lasts[] = {
	{"last-Sunday", TM_SUNDAY},
	{"last-Monday", TM_MONDAY},
	{"last-Tuesday", TM_TUESDAY},
	{"last-Wednesday", TM_WEDNESDAY},
	{"last-Thursday", TM_THURSDAY},
	{"last-Friday", TM_FRIDAY},
	{"last-Saturday", TM_SATURDAY},
	{NULL, 0}
};

static struct lookup const begin_years[] = {
	{"minimum", YR_MINIMUM},
	{"maximum", YR_MAXIMUM},
	{NULL, 0}
};

static struct lookup const end_years[] = {
	{"minimum", YR_MINIMUM},
	{"maximum", YR_MAXIMUM},
	{"only", YR_ONLY},
	{NULL, 0}
};

static struct lookup const leap_types[] = {
	{"Rolling", TRUE},
	{"Stationary", FALSE},
	{NULL, 0}
};

static const int len_months[2][MONSPERYEAR] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
	{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

static const int len_years[2] = {
	DAYSPERNYEAR, DAYSPERLYEAR
};

static struct attype
{
	pg_time_t	at;
	unsigned char type;
}	attypes[TZ_MAX_TIMES];
static long gmtoffs[TZ_MAX_TYPES];
static char isdsts[TZ_MAX_TYPES];
static unsigned char abbrinds[TZ_MAX_TYPES];
static char ttisstds[TZ_MAX_TYPES];
static char ttisgmts[TZ_MAX_TYPES];
static char chars[TZ_MAX_CHARS];
static pg_time_t trans[TZ_MAX_LEAPS];
static long corr[TZ_MAX_LEAPS];
static char roll[TZ_MAX_LEAPS];

/*
 * Memory allocation.
 */

static char *
memcheck(char *ptr)
{
	if (ptr == NULL)
	{
		const char *e = strerror(errno);

		(void) fprintf(stderr, _("%s: Memory exhausted: %s\n"),
					   progname, e);
		(void) exit(EXIT_FAILURE);
	}
	return ptr;
}

#define emalloc(size)		memcheck(imalloc(size))
#define erealloc(ptr, size) memcheck(irealloc((ptr), (size)))
#define ecpyalloc(ptr)		memcheck(icpyalloc(ptr))
#define ecatalloc(oldp, newp)	memcheck(icatalloc((oldp), (newp)))

/*
 * Error handling.
 */

static void
eats(const char *name, int num, const char *rname, int rnum)
{
	filename = name;
	linenum = num;
	rfilename = rname;
	rlinenum = rnum;
}

static void
eat(const char *name, int num)
{
	eats(name, num, (char *) NULL, -1);
}

static void
error(const char *string)
{
	/*
	 * Match the format of "cc" to allow sh users to  zic ... 2>&1 | error -t
	 * "*" -v on BSD systems.
	 */
	(void) fprintf(stderr, _("\"%s\", line %d: %s"),
				   filename, linenum, string);
	if (rfilename != NULL)
		(void) fprintf(stderr, _(" (rule from \"%s\", line %d)"),
					   rfilename, rlinenum);
	(void) fprintf(stderr, "\n");
	++errors;
}

static void
warning(const char *string)
{
	char	   *cp;

	cp = ecpyalloc(_("warning: "));
	cp = ecatalloc(cp, string);
	error(cp);
	ifree(cp);
	--errors;
}

static void
usage(void)
{
	(void) fprintf(stderr, _("%s: usage is %s [ --version ] [ -s ] [ -v ] [ -l localtime ] [ -p posixrules ] \\\n\t[ -d directory ] [ -L leapseconds ] [ -y yearistype ] [ filename ... ]\n"),
				   progname, progname);
	(void) exit(EXIT_FAILURE);
}

static const char *psxrules;
static const char *lcltime;
static const char *directory;
static const char *leapsec;
static const char *yitcommand;
static int	sflag = FALSE;

int
main(int argc, char *argv[])
{
	int			i;
	int			j;
	int			c;

#ifndef WIN32
	(void) umask(umask(S_IWGRP | S_IWOTH) | (S_IWGRP | S_IWOTH));
#endif   /* !WIN32 */
	progname = argv[0];
	for (i = 1; i < argc; ++i)
		if (strcmp(argv[i], "--version") == 0)
		{
			(void) printf("%s\n", elsieid);
			(void) exit(EXIT_SUCCESS);
		}
	while ((c = getopt(argc, argv, "d:l:p:L:vsy:")) != EOF && c != -1)
		switch (c)
		{
			default:
				usage();
			case 'd':
				if (directory == NULL)
					directory = optarg;
				else
				{
					(void) fprintf(stderr,
								_("%s: More than one -d option specified\n"),
								   progname);
					(void) exit(EXIT_FAILURE);
				}
				break;
			case 'l':
				if (lcltime == NULL)
					lcltime = optarg;
				else
				{
					(void) fprintf(stderr,
								_("%s: More than one -l option specified\n"),
								   progname);
					(void) exit(EXIT_FAILURE);
				}
				break;
			case 'p':
				if (psxrules == NULL)
					psxrules = optarg;
				else
				{
					(void) fprintf(stderr,
								_("%s: More than one -p option specified\n"),
								   progname);
					(void) exit(EXIT_FAILURE);
				}
				break;
			case 'y':
				if (yitcommand == NULL)
					yitcommand = optarg;
				else
				{
					(void) fprintf(stderr,
								_("%s: More than one -y option specified\n"),
								   progname);
					(void) exit(EXIT_FAILURE);
				}
				break;
			case 'L':
				if (leapsec == NULL)
					leapsec = optarg;
				else
				{
					(void) fprintf(stderr,
								_("%s: More than one -L option specified\n"),
								   progname);
					(void) exit(EXIT_FAILURE);
				}
				break;
			case 'v':
				noise = TRUE;
				break;
			case 's':
				sflag = TRUE;
				break;
		}
	if (optind == argc - 1 && strcmp(argv[optind], "=") == 0)
		usage();				/* usage message by request */
	if (directory == NULL)
		directory = "data";
	if (yitcommand == NULL)
		yitcommand = "yearistype";

	setboundaries();

	if (optind < argc && leapsec != NULL)
	{
		infile(leapsec);
		adjleap();
	}

	for (i = optind; i < argc; ++i)
		infile(argv[i]);
	if (errors)
		(void) exit(EXIT_FAILURE);
	associate();
	for (i = 0; i < nzones; i = j)
	{
		/*
		 * Find the next non-continuation zone entry.
		 */
		for (j = i + 1; j < nzones && zones[j].z_name == NULL; ++j)
			continue;
		outzone(&zones[i], j - i);
	}

	/*
	 * Make links.
	 */
	for (i = 0; i < nlinks; ++i)
	{
		eat(links[i].l_filename, links[i].l_linenum);
		dolink(links[i].l_from, links[i].l_to);
	}
	if (lcltime != NULL)
	{
		eat("command line", 1);
		dolink(lcltime, TZDEFAULT);
	}
	if (psxrules != NULL)
	{
		eat("command line", 1);
		dolink(psxrules, TZDEFRULES);
	}
	return (errors == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void
dolink(const char *fromfile, const char *tofile)
{
	char	   *fromname;
	char	   *toname;

	if (fromfile[0] == '/')
		fromname = ecpyalloc(fromfile);
	else
	{
		fromname = ecpyalloc(directory);
		fromname = ecatalloc(fromname, "/");
		fromname = ecatalloc(fromname, fromfile);
	}
	if (tofile[0] == '/')
		toname = ecpyalloc(tofile);
	else
	{
		toname = ecpyalloc(directory);
		toname = ecatalloc(toname, "/");
		toname = ecatalloc(toname, tofile);
	}

	/*
	 * We get to be careful here since there's a fair chance of root running
	 * us.
	 */
	if (!itsdir(toname))
		(void) remove(toname);
	if (link(fromname, toname) != 0)
	{
		int			result;

		if (mkdirs(toname) != 0)
			(void) exit(EXIT_FAILURE);

		result = link(fromname, toname);
#ifdef HAVE_SYMLINK
		if (result != 0 &&
			access(fromname, F_OK) == 0 &&
			!itsdir(fromname))
		{
			const char *s = tofile;
			char	   *symlinkcontents = NULL;

			while ((s = strchr(s + 1, '/')) != NULL)
				symlinkcontents = ecatalloc(symlinkcontents, "../");
			symlinkcontents = ecatalloc(symlinkcontents, fromfile);

			result = symlink(symlinkcontents, toname);
			if (result == 0)
				warning(_("hard link failed, symbolic link used"));
			ifree(symlinkcontents);
		}
#endif
		if (result != 0)
		{
			const char *e = strerror(errno);

			(void) fprintf(stderr,
						   _("%s: Cannot link from %s to %s: %s\n"),
						   progname, fromname, toname, e);
			(void) exit(EXIT_FAILURE);
		}
	}
	ifree(fromname);
	ifree(toname);
}

#ifndef INT_MAX
#define INT_MAX ((int) (((unsigned)~0)>>1))
#endif   /* !defined INT_MAX */

#ifndef INT_MIN
#define INT_MIN ((int) ~(((unsigned)~0)>>1))
#endif   /* !defined INT_MIN */

/*
 * The tz file format currently allows at most 32-bit quantities.
 * This restriction should be removed before signed 32-bit values
 * wrap around in 2038, but unfortunately this will require a
 * change to the tz file format.
 */

#define TIME_T_BITS_IN_FILE 32

static void
setboundaries(void)
{
	/*
	 * pg_time_t is always signed, but might be only 32 bits ...
	 */
	min_time = ~(pg_time_t) 0;
	min_time <<= TYPE_BIT(pg_time_t) -1;
	max_time = ~(pg_time_t) 0 - min_time;

	/*
	 * For the moment, hard-wire the range as 1901 to 2038.  We cannot go
	 * wider without adopting an incompatible zone file format, which is a
	 * step I'd just as soon not take just yet.
	 */
	min_time = Max(min_time, (pg_time_t) INT_MIN);
	max_time = Min(max_time, (pg_time_t) INT_MAX);

	min_year = TM_YEAR_BASE + pg_gmtime(&min_time)->tm_year;
	max_year = TM_YEAR_BASE + pg_gmtime(&max_time)->tm_year;
	min_year_representable = min_year;
	max_year_representable = max_year;
}

static int
itsdir(const char *name)
{
	char	   *myname;
	int			accres;

	myname = ecpyalloc(name);
	myname = ecatalloc(myname, "/.");
	accres = access(myname, F_OK);
	ifree(myname);
	return accres == 0;
}

/*
 * Associate sets of rules with zones.
 */

/*
 * Sort by rule name.
 */

static int
rcomp(const void *cp1, const void *cp2)
{
	return strcmp(((const struct rule *) cp1)->r_name,
				  ((const struct rule *) cp2)->r_name);
}

static void
associate(void)
{
	struct zone *zp;
	struct rule *rp;
	int			base,
				out;
	int			i,
				j;

	if (nrules != 0)
	{
		(void) qsort((void *) rules, (size_t) nrules,
					 (size_t) sizeof *rules, rcomp);
		for (i = 0; i < nrules - 1; ++i)
		{
			if (strcmp(rules[i].r_name,
					   rules[i + 1].r_name) != 0)
				continue;
			if (strcmp(rules[i].r_filename,
					   rules[i + 1].r_filename) == 0)
				continue;
			eat(rules[i].r_filename, rules[i].r_linenum);
			warning(_("same rule name in multiple files"));
			eat(rules[i + 1].r_filename, rules[i + 1].r_linenum);
			warning(_("same rule name in multiple files"));
			for (j = i + 2; j < nrules; ++j)
			{
				if (strcmp(rules[i].r_name,
						   rules[j].r_name) != 0)
					break;
				if (strcmp(rules[i].r_filename,
						   rules[j].r_filename) == 0)
					continue;
				if (strcmp(rules[i + 1].r_filename,
						   rules[j].r_filename) == 0)
					continue;
				break;
			}
			i = j - 1;
		}
	}
	for (i = 0; i < nzones; ++i)
	{
		zp = &zones[i];
		zp->z_rules = NULL;
		zp->z_nrules = 0;
	}
	for (base = 0; base < nrules; base = out)
	{
		rp = &rules[base];
		for (out = base + 1; out < nrules; ++out)
			if (strcmp(rp->r_name, rules[out].r_name) != 0)
				break;
		for (i = 0; i < nzones; ++i)
		{
			zp = &zones[i];
			if (strcmp(zp->z_rule, rp->r_name) != 0)
				continue;
			zp->z_rules = rp;
			zp->z_nrules = out - base;
		}
	}
	for (i = 0; i < nzones; ++i)
	{
		zp = &zones[i];
		if (zp->z_nrules == 0)
		{
			/*
			 * Maybe we have a local standard time offset.
			 */
			eat(zp->z_filename, zp->z_linenum);
			zp->z_stdoff = gethms(zp->z_rule, _("unruly zone"),
								  TRUE);

			/*
			 * Note, though, that if there's no rule, a '%s' in the format is
			 * a bad thing.
			 */
			if (strchr(zp->z_format, '%') != 0)
				error(_("%s in ruleless zone"));
		}
	}
	if (errors)
		(void) exit(EXIT_FAILURE);
}

static void
infile(const char *name)
{
	FILE	   *fp;
	char	  **fields;
	char	   *cp;
	const struct lookup *lp;
	int			nfields;
	int			wantcont;
	int			num;
	char		buf[BUFSIZ];

	if (strcmp(name, "-") == 0)
	{
		name = _("standard input");
		fp = stdin;
	}
	else if ((fp = fopen(name, "r")) == NULL)
	{
		const char *e = strerror(errno);

		(void) fprintf(stderr, _("%s: Cannot open %s: %s\n"),
					   progname, name, e);
		(void) exit(EXIT_FAILURE);
	}
	wantcont = FALSE;
	for (num = 1;; ++num)
	{
		eat(name, num);
		if (fgets(buf, (int) sizeof buf, fp) != buf)
			break;
		cp = strchr(buf, '\n');
		if (cp == NULL)
		{
			error(_("line too long"));
			(void) exit(EXIT_FAILURE);
		}
		*cp = '\0';
		fields = getfields(buf);
		nfields = 0;
		while (fields[nfields] != NULL)
		{
			static char nada;

			if (strcmp(fields[nfields], "-") == 0)
				fields[nfields] = &nada;
			++nfields;
		}
		if (nfields == 0)
		{
			/* nothing to do */
		}
		else if (wantcont)
			wantcont = inzcont(fields, nfields);
		else
		{
			lp = byword(fields[0], line_codes);
			if (lp == NULL)
				error(_("input line of unknown type"));
			else
				switch ((int) (lp->l_value))
				{
					case LC_RULE:
						inrule(fields, nfields);
						wantcont = FALSE;
						break;
					case LC_ZONE:
						wantcont = inzone(fields, nfields);
						break;
					case LC_LINK:
						inlink(fields, nfields);
						wantcont = FALSE;
						break;
					case LC_LEAP:
						if (name != leapsec)
							(void) fprintf(stderr,
							_("%s: Leap line in non leap seconds file %s\n"),
										   progname, name);
						else
							inleap(fields, nfields);
						wantcont = FALSE;
						break;
					default:	/* "cannot happen" */
						(void) fprintf(stderr,
									   _("%s: panic: Invalid l_value %d\n"),
									   progname, lp->l_value);
						(void) exit(EXIT_FAILURE);
				}
		}
		ifree((char *) fields);
	}
	if (ferror(fp))
	{
		(void) fprintf(stderr, _("%s: Error reading %s\n"),
					   progname, filename);
		(void) exit(EXIT_FAILURE);
	}
	if (fp != stdin && fclose(fp))
	{
		const char *e = strerror(errno);

		(void) fprintf(stderr, _("%s: Error closing %s: %s\n"),
					   progname, filename, e);
		(void) exit(EXIT_FAILURE);
	}
	if (wantcont)
		error(_("expected continuation line not found"));
}

/*----------
 * Convert a string of one of the forms
 *	h	-h	hh:mm	-hh:mm	hh:mm:ss	-hh:mm:ss
 * into a number of seconds.
 * A null string maps to zero.
 * Call error with errstring and return zero on errors.
 *----------
 */
static long
gethms(const char *string, const char *errstring, int signable)
{
	int			hh,
				mm,
				ss,
				sign;

	if (string == NULL || *string == '\0')
		return 0;
	if (!signable)
		sign = 1;
	else if (*string == '-')
	{
		sign = -1;
		++string;
	}
	else
		sign = 1;
	if (sscanf(string, scheck(string, "%d"), &hh) == 1)
		mm = ss = 0;
	else if (sscanf(string, scheck(string, "%d:%d"), &hh, &mm) == 2)
		ss = 0;
	else if (sscanf(string, scheck(string, "%d:%d:%d"),
					&hh, &mm, &ss) != 3)
	{
		error(errstring);
		return 0;
	}
	if ((hh < 0 || hh >= HOURSPERDAY ||
		 mm < 0 || mm >= MINSPERHOUR ||
		 ss < 0 || ss > SECSPERMIN) &&
		!(hh == HOURSPERDAY && mm == 0 && ss == 0))
	{
		error(errstring);
		return 0;
	}
	if (noise && hh == HOURSPERDAY)
		warning(_("24:00 not handled by pre-1998 versions of zic"));
	return eitol(sign) *
		(eitol(hh * MINSPERHOUR + mm) *
		 eitol(SECSPERMIN) + eitol(ss));
}

static void
inrule(char **fields, int nfields)
{
	static struct rule r;

	if (nfields != RULE_FIELDS)
	{
		error(_("wrong number of fields on Rule line"));
		return;
	}
	if (*fields[RF_NAME] == '\0')
	{
		error(_("nameless rule"));
		return;
	}
	r.r_filename = filename;
	r.r_linenum = linenum;
	r.r_stdoff = gethms(fields[RF_STDOFF], _("invalid saved time"), TRUE);
	rulesub(&r, fields[RF_LOYEAR], fields[RF_HIYEAR], fields[RF_COMMAND],
			fields[RF_MONTH], fields[RF_DAY], fields[RF_TOD]);
	r.r_name = ecpyalloc(fields[RF_NAME]);
	r.r_abbrvar = ecpyalloc(fields[RF_ABBRVAR]);
	rules = (struct rule *) (void *) erealloc((char *) rules,
									   (int) ((nrules + 1) * sizeof *rules));
	rules[nrules++] = r;
}

static int
inzone(char **fields, int nfields)
{
	int			i;
	static char *buf;

	if (nfields < ZONE_MINFIELDS || nfields > ZONE_MAXFIELDS)
	{
		error(_("wrong number of fields on Zone line"));
		return FALSE;
	}
	if (strcmp(fields[ZF_NAME], TZDEFAULT) == 0 && lcltime != NULL)
	{
		buf = erealloc(buf, (int) (132 + strlen(TZDEFAULT)));
		(void) sprintf(buf,
				  _("\"Zone %s\" line and -l option are mutually exclusive"),
					   TZDEFAULT);
		error(buf);
		return FALSE;
	}
	if (strcmp(fields[ZF_NAME], TZDEFRULES) == 0 && psxrules != NULL)
	{
		buf = erealloc(buf, (int) (132 + strlen(TZDEFRULES)));
		(void) sprintf(buf,
				  _("\"Zone %s\" line and -p option are mutually exclusive"),
					   TZDEFRULES);
		error(buf);
		return FALSE;
	}
	for (i = 0; i < nzones; ++i)
		if (zones[i].z_name != NULL &&
			strcmp(zones[i].z_name, fields[ZF_NAME]) == 0)
		{
			buf = erealloc(buf, (int) (132 +
									   strlen(fields[ZF_NAME]) +
									   strlen(zones[i].z_filename)));
			(void) sprintf(buf,
						   _("duplicate zone name %s (file \"%s\", line %d)"),
						   fields[ZF_NAME],
						   zones[i].z_filename,
						   zones[i].z_linenum);
			error(buf);
			return FALSE;
		}
	return inzsub(fields, nfields, FALSE);
}

static int
inzcont(char **fields, int nfields)
{
	if (nfields < ZONEC_MINFIELDS || nfields > ZONEC_MAXFIELDS)
	{
		error(_("wrong number of fields on Zone continuation line"));
		return FALSE;
	}
	return inzsub(fields, nfields, TRUE);
}

static int
inzsub(char **fields, int nfields, int iscont)
{
	char	   *cp;
	static struct zone z;
	int			i_gmtoff,
				i_rule,
				i_format;
	int			i_untilyear,
				i_untilmonth;
	int			i_untilday,
				i_untiltime;
	int			hasuntil;

	if (iscont)
	{
		i_gmtoff = ZFC_GMTOFF;
		i_rule = ZFC_RULE;
		i_format = ZFC_FORMAT;
		i_untilyear = ZFC_TILYEAR;
		i_untilmonth = ZFC_TILMONTH;
		i_untilday = ZFC_TILDAY;
		i_untiltime = ZFC_TILTIME;
		z.z_name = NULL;
	}
	else
	{
		i_gmtoff = ZF_GMTOFF;
		i_rule = ZF_RULE;
		i_format = ZF_FORMAT;
		i_untilyear = ZF_TILYEAR;
		i_untilmonth = ZF_TILMONTH;
		i_untilday = ZF_TILDAY;
		i_untiltime = ZF_TILTIME;
		z.z_name = ecpyalloc(fields[ZF_NAME]);
	}
	z.z_filename = filename;
	z.z_linenum = linenum;
	z.z_gmtoff = gethms(fields[i_gmtoff], _("invalid UTC offset"), TRUE);
	if ((cp = strchr(fields[i_format], '%')) != 0)
	{
		if (*++cp != 's' || strchr(cp, '%') != 0)
		{
			error(_("invalid abbreviation format"));
			return FALSE;
		}
	}
	z.z_rule = ecpyalloc(fields[i_rule]);
	z.z_format = ecpyalloc(fields[i_format]);
	hasuntil = nfields > i_untilyear;
	if (hasuntil)
	{
		z.z_untilrule.r_filename = filename;
		z.z_untilrule.r_linenum = linenum;
		rulesub(&z.z_untilrule,
				fields[i_untilyear],
				"only",
				"",
				(nfields > i_untilmonth) ?
				fields[i_untilmonth] : "Jan",
				(nfields > i_untilday) ? fields[i_untilday] : "1",
				(nfields > i_untiltime) ? fields[i_untiltime] : "0");
		z.z_untiltime = rpytime(&z.z_untilrule,
								z.z_untilrule.r_loyear);
		if (iscont && nzones > 0 &&
			z.z_untiltime > min_time &&
			z.z_untiltime < max_time &&
			zones[nzones - 1].z_untiltime > min_time &&
			zones[nzones - 1].z_untiltime < max_time &&
			zones[nzones - 1].z_untiltime >= z.z_untiltime)
		{
			error(_("Zone continuation line end time is not after end time of previous line"));
			return FALSE;
		}
	}
	zones = (struct zone *) (void *) erealloc((char *) zones,
									   (int) ((nzones + 1) * sizeof *zones));
	zones[nzones++] = z;

	/*
	 * If there was an UNTIL field on this line, there's more information
	 * about the zone on the next line.
	 */
	return hasuntil;
}

static void
inleap(char **fields, int nfields)
{
	const char *cp;
	const struct lookup *lp;
	int			i,
				j;
	int			year,
				month,
				day;
	long		dayoff,
				tod;
	pg_time_t	t;

	if (nfields != LEAP_FIELDS)
	{
		error(_("wrong number of fields on Leap line"));
		return;
	}
	dayoff = 0;
	cp = fields[LP_YEAR];
	if (sscanf(cp, scheck(cp, "%d"), &year) != 1)
	{
		/*
		 * Leapin' Lizards!
		 */
		error(_("invalid leaping year"));
		return;
	}
	j = EPOCH_YEAR;
	while (j != year)
	{
		if (year > j)
		{
			i = len_years[isleap(j)];
			++j;
		}
		else
		{
			--j;
			i = -len_years[isleap(j)];
		}
		dayoff = oadd(dayoff, eitol(i));
	}
	if ((lp = byword(fields[LP_MONTH], mon_names)) == NULL)
	{
		error(_("invalid month name"));
		return;
	}
	month = lp->l_value;
	j = TM_JANUARY;
	while (j != month)
	{
		i = len_months[isleap(year)][j];
		dayoff = oadd(dayoff, eitol(i));
		++j;
	}
	cp = fields[LP_DAY];
	if (sscanf(cp, scheck(cp, "%d"), &day) != 1 ||
		day <= 0 || day > len_months[isleap(year)][month])
	{
		error(_("invalid day of month"));
		return;
	}
	dayoff = oadd(dayoff, eitol(day - 1));
	if (dayoff < min_time / SECSPERDAY)
	{
		error(_("time too small"));
		return;
	}
	if (dayoff > max_time / SECSPERDAY)
	{
		error(_("time too large"));
		return;
	}
	t = (pg_time_t) dayoff *SECSPERDAY;

	tod = gethms(fields[LP_TIME], _("invalid time of day"), FALSE);
	cp = fields[LP_CORR];
	{
		int			positive;
		int			count;

		if (strcmp(cp, "") == 0)
		{						/* infile() turns "-" into "" */
			positive = FALSE;
			count = 1;
		}
		else if (strcmp(cp, "--") == 0)
		{
			positive = FALSE;
			count = 2;
		}
		else if (strcmp(cp, "+") == 0)
		{
			positive = TRUE;
			count = 1;
		}
		else if (strcmp(cp, "++") == 0)
		{
			positive = TRUE;
			count = 2;
		}
		else
		{
			error(_("illegal CORRECTION field on Leap line"));
			return;
		}
		if ((lp = byword(fields[LP_ROLL], leap_types)) == NULL)
		{
			error(_("illegal Rolling/Stationary field on Leap line"));
			return;
		}
		leapadd(tadd(t, tod), positive, lp->l_value, count);
	}
}

static void
inlink(char **fields, int nfields)
{
	struct link l;

	if (nfields != LINK_FIELDS)
	{
		error(_("wrong number of fields on Link line"));
		return;
	}
	if (*fields[LF_FROM] == '\0')
	{
		error(_("blank FROM field on Link line"));
		return;
	}
	if (*fields[LF_TO] == '\0')
	{
		error(_("blank TO field on Link line"));
		return;
	}
	l.l_filename = filename;
	l.l_linenum = linenum;
	l.l_from = ecpyalloc(fields[LF_FROM]);
	l.l_to = ecpyalloc(fields[LF_TO]);
	links = (struct link *) (void *) erealloc((char *) links,
									   (int) ((nlinks + 1) * sizeof *links));
	links[nlinks++] = l;
}

static void
rulesub(struct rule * rp, const char *loyearp, const char *hiyearp,
		const char *typep, const char *monthp, const char *dayp,
		const char *timep)
{
	const struct lookup *lp;
	const char *cp;
	char	   *dp;
	char	   *ep;

	if ((lp = byword(monthp, mon_names)) == NULL)
	{
		error(_("invalid month name"));
		return;
	}
	rp->r_month = lp->l_value;
	rp->r_todisstd = FALSE;
	rp->r_todisgmt = FALSE;
	dp = ecpyalloc(timep);
	if (*dp != '\0')
	{
		ep = dp + strlen(dp) - 1;
		switch (lowerit(*ep))
		{
			case 's':			/* Standard */
				rp->r_todisstd = TRUE;
				rp->r_todisgmt = FALSE;
				*ep = '\0';
				break;
			case 'w':			/* Wall */
				rp->r_todisstd = FALSE;
				rp->r_todisgmt = FALSE;
				*ep = '\0';
				break;
			case 'g':			/* Greenwich */
			case 'u':			/* Universal */
			case 'z':			/* Zulu */
				rp->r_todisstd = TRUE;
				rp->r_todisgmt = TRUE;
				*ep = '\0';
				break;
		}
	}
	rp->r_tod = gethms(dp, _("invalid time of day"), FALSE);
	ifree(dp);

	/*
	 * Year work.
	 */
	cp = loyearp;
	lp = byword(cp, begin_years);
	if (lp != NULL)
		switch ((int) lp->l_value)
		{
			case YR_MINIMUM:
				rp->r_loyear = INT_MIN;
				break;
			case YR_MAXIMUM:
				rp->r_loyear = INT_MAX;
				break;
			default:			/* "cannot happen" */
				(void) fprintf(stderr,
							   _("%s: panic: Invalid l_value %d\n"),
							   progname, lp->l_value);
				(void) exit(EXIT_FAILURE);
		}
	else if (sscanf(cp, scheck(cp, "%d"), &rp->r_loyear) != 1)
	{
		error(_("invalid starting year"));
		return;
	}
	else if (noise)
	{
		if (rp->r_loyear < min_year_representable)
			warning(_("starting year too low to be represented"));
		else if (rp->r_loyear > max_year_representable)
			warning(_("starting year too high to be represented"));
	}
	cp = hiyearp;
	if ((lp = byword(cp, end_years)) != NULL)
		switch ((int) lp->l_value)
		{
			case YR_MINIMUM:
				rp->r_hiyear = INT_MIN;
				break;
			case YR_MAXIMUM:
				rp->r_hiyear = INT_MAX;
				break;
			case YR_ONLY:
				rp->r_hiyear = rp->r_loyear;
				break;
			default:			/* "cannot happen" */
				(void) fprintf(stderr,
							   _("%s: panic: Invalid l_value %d\n"),
							   progname, lp->l_value);
				(void) exit(EXIT_FAILURE);
		}
	else if (sscanf(cp, scheck(cp, "%d"), &rp->r_hiyear) != 1)
	{
		error(_("invalid ending year"));
		return;
	}
	else if (noise)
	{
		if (rp->r_loyear < min_year_representable)
			warning(_("ending year too low to be represented"));
		else if (rp->r_loyear > max_year_representable)
			warning(_("ending year too high to be represented"));
	}
	if (rp->r_loyear > rp->r_hiyear)
	{
		error(_("starting year greater than ending year"));
		return;
	}
	if (*typep == '\0')
		rp->r_yrtype = NULL;
	else
	{
		if (rp->r_loyear == rp->r_hiyear)
		{
			error(_("typed single year"));
			return;
		}
		rp->r_yrtype = ecpyalloc(typep);
	}
	if (rp->r_loyear < min_year && rp->r_loyear > 0)
		min_year = rp->r_loyear;

	/*
	 * Day work. Accept things such as:  1	last-Sunday  Sun<=20  Sun>=7
	 */
	dp = ecpyalloc(dayp);
	if ((lp = byword(dp, lasts)) != NULL)
	{
		rp->r_dycode = DC_DOWLEQ;
		rp->r_wday = lp->l_value;
		rp->r_dayofmonth = len_months[1][rp->r_month];
	}
	else
	{
		if ((ep = strchr(dp, '<')) != 0)
			rp->r_dycode = DC_DOWLEQ;
		else if ((ep = strchr(dp, '>')) != 0)
			rp->r_dycode = DC_DOWGEQ;
		else
		{
			ep = dp;
			rp->r_dycode = DC_DOM;
		}
		if (rp->r_dycode != DC_DOM)
		{
			*ep++ = 0;
			if (*ep++ != '=')
			{
				error(_("invalid day of month"));
				ifree(dp);
				return;
			}
			if ((lp = byword(dp, wday_names)) == NULL)
			{
				error(_("invalid weekday name"));
				ifree(dp);
				return;
			}
			rp->r_wday = lp->l_value;
		}
		if (sscanf(ep, scheck(ep, "%d"), &rp->r_dayofmonth) != 1 ||
			rp->r_dayofmonth <= 0 ||
			(rp->r_dayofmonth > len_months[1][rp->r_month]))
		{
			error(_("invalid day of month"));
			ifree(dp);
			return;
		}
	}
	ifree(dp);
}

static void
convert(long val, char *buf)
{
	int			i;
	long		shift;

	for (i = 0, shift = 24; i < 4; ++i, shift -= 8)
		buf[i] = val >> shift;
}

static void
puttzcode(long val, FILE *fp)
{
	char		buf[4];

	convert(val, buf);
	(void) fwrite((void *) buf, (size_t) sizeof buf, (size_t) 1, fp);
}

static int
atcomp(const void *avp, const void *bvp)
{
	if (((struct attype *) avp)->at < ((struct attype *) bvp)->at)
		return -1;
	else if (((struct attype *) avp)->at > ((struct attype *) bvp)->at)
		return 1;
	else
		return 0;
}

static void
writezone(const char *name)
{
	FILE	   *fp;
	int			i,
				j;
	static char *fullname;
	static struct tzhead tzh;
	pg_time_t	ats[TZ_MAX_TIMES];
	unsigned char types[TZ_MAX_TIMES];

	/*
	 * Sort.
	 */
	if (timecnt > 1)
		(void) qsort((void *) attypes, (size_t) timecnt,
					 (size_t) sizeof *attypes, atcomp);

	/*
	 * Optimize.
	 */
	{
		int			fromi;
		int			toi;

		toi = 0;
		fromi = 0;
		while (fromi < timecnt && attypes[fromi].at < min_time)
			++fromi;
		if (isdsts[0] == 0)
			while (fromi < timecnt && attypes[fromi].type == 0)
				++fromi;		/* handled by default rule */
		for (; fromi < timecnt; ++fromi)
		{
			if (toi != 0
				&& ((attypes[fromi].at
					 + gmtoffs[attypes[toi - 1].type])
					<= (attypes[toi - 1].at
						+ gmtoffs[toi == 1 ? 0
								  : attypes[toi - 2].type])))
			{
				attypes[toi - 1].type = attypes[fromi].type;
				continue;
			}
			if (toi == 0 ||
				attypes[toi - 1].type != attypes[fromi].type)
				attypes[toi++] = attypes[fromi];
		}
		timecnt = toi;
	}

	/*
	 * Transfer.
	 */
	for (i = 0; i < timecnt; ++i)
	{
		ats[i] = attypes[i].at;
		types[i] = attypes[i].type;
	}
	fullname = erealloc(fullname,
						(int) (strlen(directory) + 1 + strlen(name) + 1));
	(void) sprintf(fullname, "%s/%s", directory, name);

	/*
	 * Remove old file, if any, to snap links.
	 */
	if (!itsdir(fullname) && remove(fullname) != 0 && errno != ENOENT)
	{
		const char *e = strerror(errno);

		(void) fprintf(stderr, _("%s: Cannot remove %s: %s\n"),
					   progname, fullname, e);
		(void) exit(EXIT_FAILURE);
	}
	if ((fp = fopen(fullname, "wb")) == NULL)
	{
		if (mkdirs(fullname) != 0)
			(void) exit(EXIT_FAILURE);
		if ((fp = fopen(fullname, "wb")) == NULL)
		{
			const char *e = strerror(errno);

			(void) fprintf(stderr, _("%s: Cannot create %s: %s\n"),
						   progname, fullname, e);
			(void) exit(EXIT_FAILURE);
		}
	}
	convert(eitol(typecnt), tzh.tzh_ttisgmtcnt);
	convert(eitol(typecnt), tzh.tzh_ttisstdcnt);
	convert(eitol(leapcnt), tzh.tzh_leapcnt);
	convert(eitol(timecnt), tzh.tzh_timecnt);
	convert(eitol(typecnt), tzh.tzh_typecnt);
	convert(eitol(charcnt), tzh.tzh_charcnt);
	(void) strncpy(tzh.tzh_magic, TZ_MAGIC, sizeof tzh.tzh_magic);
#define DO(field)	(void) fwrite((void *) tzh.field, (size_t) sizeof tzh.field, (size_t) 1, fp)
	DO(tzh_magic);
	DO(tzh_reserved);
	DO(tzh_ttisgmtcnt);
	DO(tzh_ttisstdcnt);
	DO(tzh_leapcnt);
	DO(tzh_timecnt);
	DO(tzh_typecnt);
	DO(tzh_charcnt);
#undef DO
	for (i = 0; i < timecnt; ++i)
	{
		j = leapcnt;
		while (--j >= 0)
			if (ats[i] >= trans[j])
			{
				ats[i] = tadd(ats[i], corr[j]);
				break;
			}
		puttzcode((long) ats[i], fp);
	}
	if (timecnt > 0)
		(void) fwrite((void *) types, (size_t) sizeof types[0],
					  (size_t) timecnt, fp);
	for (i = 0; i < typecnt; ++i)
	{
		puttzcode((long) gmtoffs[i], fp);
		(void) putc(isdsts[i], fp);
		(void) putc(abbrinds[i], fp);
	}
	if (charcnt != 0)
		(void) fwrite((void *) chars, (size_t) sizeof chars[0],
					  (size_t) charcnt, fp);
	for (i = 0; i < leapcnt; ++i)
	{
		if (roll[i])
		{
			if (timecnt == 0 || trans[i] < ats[0])
			{
				j = 0;
				while (isdsts[j])
					if (++j >= typecnt)
					{
						j = 0;
						break;
					}
			}
			else
			{
				j = 1;
				while (j < timecnt && trans[i] >= ats[j])
					++j;
				j = types[j - 1];
			}
			puttzcode((long) tadd(trans[i], -gmtoffs[j]), fp);
		}
		else
			puttzcode((long) trans[i], fp);
		puttzcode((long) corr[i], fp);
	}
	for (i = 0; i < typecnt; ++i)
		(void) putc(ttisstds[i], fp);
	for (i = 0; i < typecnt; ++i)
		(void) putc(ttisgmts[i], fp);
	if (ferror(fp) || fclose(fp))
	{
		(void) fprintf(stderr, _("%s: Error writing %s\n"),
					   progname, fullname);
		(void) exit(EXIT_FAILURE);
	}
}

static void
doabbr(char *abbr, const char *format, const char *letters, int isdst)
{
	if (strchr(format, '/') == NULL)
	{
		if (letters == NULL)
			(void) strcpy(abbr, format);
		else
			(void) sprintf(abbr, format, letters);
	}
	else if (isdst)
		(void) strcpy(abbr, strchr(format, '/') + 1);
	else
	{
		(void) strcpy(abbr, format);
		*strchr(abbr, '/') = '\0';
	}
}

static void
outzone(const struct zone * zpfirst, int zonecount)
{
	const struct zone *zp;
	struct rule *rp;
	int			i,
				j;
	int			usestart,
				useuntil;
	pg_time_t	starttime = 0;
	pg_time_t	untiltime = 0;
	long		gmtoff;
	long		stdoff;
	int			year;
	long		startoff;
	int			startttisstd;
	int			startttisgmt;
	int			type;
	char		startbuf[BUFSIZ];

	/*
	 * Now. . .finally. . .generate some useful data!
	 */
	timecnt = 0;
	typecnt = 0;
	charcnt = 0;

	/*
	 * Thanks to Earl Chew (earl@dnd.icp.nec.com.au) for noting the need to
	 * unconditionally initialize startttisstd.
	 */
	startttisstd = FALSE;
	startttisgmt = FALSE;
	for (i = 0; i < zonecount; ++i)
	{
		/*
		 * A guess that may well be corrected later.
		 */
		stdoff = 0;
		zp = &zpfirst[i];
		usestart = i > 0 && (zp - 1)->z_untiltime > min_time;
		useuntil = i < (zonecount - 1);
		if (useuntil && zp->z_untiltime <= min_time)
			continue;
		gmtoff = zp->z_gmtoff;
		eat(zp->z_filename, zp->z_linenum);
		*startbuf = '\0';
		startoff = zp->z_gmtoff;
		if (zp->z_nrules == 0)
		{
			stdoff = zp->z_stdoff;
			doabbr(startbuf, zp->z_format,
				   (char *) NULL, stdoff != 0);
			type = addtype(oadd(zp->z_gmtoff, stdoff),
						   startbuf, stdoff != 0, startttisstd,
						   startttisgmt);
			if (usestart)
			{
				addtt(starttime, type);
				usestart = FALSE;
			}
			else if (stdoff != 0)
				addtt(min_time, type);
		}
		else
			for (year = min_year; year <= max_year; ++year)
			{
				if (useuntil && year > zp->z_untilrule.r_hiyear)
					break;

				/*
				 * Mark which rules to do in the current year. For those to
				 * do, calculate rpytime(rp, year);
				 */
				for (j = 0; j < zp->z_nrules; ++j)
				{
					rp = &zp->z_rules[j];
					eats(zp->z_filename, zp->z_linenum,
						 rp->r_filename, rp->r_linenum);
					rp->r_todo = year >= rp->r_loyear &&
						year <= rp->r_hiyear &&
						yearistype(year, rp->r_yrtype);
					if (rp->r_todo)
						rp->r_temp = rpytime(rp, year);
				}
				for (;;)
				{
					int			k;
					pg_time_t	jtime,
								ktime = 0;
					long		offset;
					char		buf[BUFSIZ];

					if (useuntil)
					{
						/*
						 * Turn untiltime into UTC assuming the current gmtoff
						 * and stdoff values.
						 */
						untiltime = zp->z_untiltime;
						if (!zp->z_untilrule.r_todisgmt)
							untiltime = tadd(untiltime,
											 -gmtoff);
						if (!zp->z_untilrule.r_todisstd)
							untiltime = tadd(untiltime,
											 -stdoff);
					}

					/*
					 * Find the rule (of those to do, if any) that takes
					 * effect earliest in the year.
					 */
					k = -1;
					for (j = 0; j < zp->z_nrules; ++j)
					{
						rp = &zp->z_rules[j];
						if (!rp->r_todo)
							continue;
						eats(zp->z_filename, zp->z_linenum,
							 rp->r_filename, rp->r_linenum);
						offset = rp->r_todisgmt ? 0 : gmtoff;
						if (!rp->r_todisstd)
							offset = oadd(offset, stdoff);
						jtime = rp->r_temp;
						if (jtime == min_time ||
							jtime == max_time)
							continue;
						jtime = tadd(jtime, -offset);
						if (k < 0 || jtime < ktime)
						{
							k = j;
							ktime = jtime;
						}
					}
					if (k < 0)
						break;	/* go on to next year */
					rp = &zp->z_rules[k];
					rp->r_todo = FALSE;
					if (useuntil && ktime >= untiltime)
						break;
					stdoff = rp->r_stdoff;
					if (usestart && ktime == starttime)
						usestart = FALSE;
					if (usestart)
					{
						if (ktime < starttime)
						{
							startoff = oadd(zp->z_gmtoff,
											stdoff);
							doabbr(startbuf, zp->z_format,
								   rp->r_abbrvar,
								   rp->r_stdoff != 0);
							continue;
						}
						if (*startbuf == '\0' &&
							startoff == oadd(zp->z_gmtoff,
											 stdoff))
						{
							doabbr(startbuf, zp->z_format,
								   rp->r_abbrvar,
								   rp->r_stdoff != 0);
						}
					}
					eats(zp->z_filename, zp->z_linenum,
						 rp->r_filename, rp->r_linenum);
					doabbr(buf, zp->z_format, rp->r_abbrvar,
						   rp->r_stdoff != 0);
					offset = oadd(zp->z_gmtoff, rp->r_stdoff);
					type = addtype(offset, buf, rp->r_stdoff != 0,
								   rp->r_todisstd, rp->r_todisgmt);
					addtt(ktime, type);
				}
			}
		if (usestart)
		{
			if (*startbuf == '\0' &&
				zp->z_format != NULL &&
				strchr(zp->z_format, '%') == NULL &&
				strchr(zp->z_format, '/') == NULL)
				(void) strcpy(startbuf, zp->z_format);
			eat(zp->z_filename, zp->z_linenum);
			if (*startbuf == '\0')
				error(_("cannot determine time zone abbreviation to use just after until time"));
			else
				addtt(starttime,
					  addtype(startoff, startbuf,
							  startoff != zp->z_gmtoff,
							  startttisstd,
							  startttisgmt));
		}

		/*
		 * Now we may get to set starttime for the next zone line.
		 */
		if (useuntil)
		{
			startttisstd = zp->z_untilrule.r_todisstd;
			startttisgmt = zp->z_untilrule.r_todisgmt;
			starttime = zp->z_untiltime;
			if (!startttisstd)
				starttime = tadd(starttime, -stdoff);
			if (!startttisgmt)
				starttime = tadd(starttime, -gmtoff);
		}
	}
	writezone(zpfirst->z_name);
}

static void
addtt(const pg_time_t starttime, int type)
{
	if (starttime <= min_time ||
		(timecnt == 1 && attypes[0].at < min_time))
	{
		gmtoffs[0] = gmtoffs[type];
		isdsts[0] = isdsts[type];
		ttisstds[0] = ttisstds[type];
		ttisgmts[0] = ttisgmts[type];
		if (abbrinds[type] != 0)
			(void) strcpy(chars, &chars[abbrinds[type]]);
		abbrinds[0] = 0;
		charcnt = strlen(chars) + 1;
		typecnt = 1;
		timecnt = 0;
		type = 0;
	}
	if (timecnt >= TZ_MAX_TIMES)
	{
		error(_("too many transitions?!"));
		(void) exit(EXIT_FAILURE);
	}
	attypes[timecnt].at = starttime;
	attypes[timecnt].type = type;
	++timecnt;
}

static int
addtype(long gmtoff, const char *abbr, int isdst,
		int ttisstd, int ttisgmt)
{
	int			i;
	int			j;

	if (isdst != TRUE && isdst != FALSE)
	{
		error(_("internal error - addtype called with bad isdst"));
		(void) exit(EXIT_FAILURE);
	}
	if (ttisstd != TRUE && ttisstd != FALSE)
	{
		error(_("internal error - addtype called with bad ttisstd"));
		(void) exit(EXIT_FAILURE);
	}
	if (ttisgmt != TRUE && ttisgmt != FALSE)
	{
		error(_("internal error - addtype called with bad ttisgmt"));
		(void) exit(EXIT_FAILURE);
	}

	/*
	 * See if there's already an entry for this zone type. If so, just return
	 * its index.
	 */
	for (i = 0; i < typecnt; ++i)
	{
		if (gmtoff == gmtoffs[i] && isdst == isdsts[i] &&
			strcmp(abbr, &chars[abbrinds[i]]) == 0 &&
			ttisstd == ttisstds[i] &&
			ttisgmt == ttisgmts[i])
			return i;
	}

	/*
	 * There isn't one; add a new one, unless there are already too many.
	 */
	if (typecnt >= TZ_MAX_TYPES)
	{
		error(_("too many local time types"));
		(void) exit(EXIT_FAILURE);
	}
	gmtoffs[i] = gmtoff;
	isdsts[i] = isdst;
	ttisstds[i] = ttisstd;
	ttisgmts[i] = ttisgmt;

	for (j = 0; j < charcnt; ++j)
		if (strcmp(&chars[j], abbr) == 0)
			break;
	if (j == charcnt)
		newabbr(abbr);
	abbrinds[i] = j;
	++typecnt;
	return i;
}

static void
leapadd(const pg_time_t t, int positive, int rolling, int count)
{
	int			i;
	int			j;

	if (leapcnt + (positive ? count : 1) > TZ_MAX_LEAPS)
	{
		error(_("too many leap seconds"));
		(void) exit(EXIT_FAILURE);
	}
	for (i = 0; i < leapcnt; ++i)
		if (t <= trans[i])
		{
			if (t == trans[i])
			{
				error(_("repeated leap second moment"));
				(void) exit(EXIT_FAILURE);
			}
			break;
		}
	do
	{
		for (j = leapcnt; j > i; --j)
		{
			trans[j] = trans[j - 1];
			corr[j] = corr[j - 1];
			roll[j] = roll[j - 1];
		}
		trans[i] = t;
		corr[i] = positive ? 1L : eitol(-count);
		roll[i] = rolling;
		++leapcnt;
	} while (positive && --count != 0);
}

static void
adjleap(void)
{
	int			i;
	long		last = 0;

	/*
	 * propagate leap seconds forward
	 */
	for (i = 0; i < leapcnt; ++i)
	{
		trans[i] = tadd(trans[i], last);
		last = corr[i] += last;
	}
}

static int
yearistype(int year, const char *type)
{
	static char *buf;
	int			result;

	if (type == NULL || *type == '\0')
		return TRUE;
	buf = erealloc(buf, (int) (132 + strlen(yitcommand) + strlen(type)));
	(void) sprintf(buf, "%s %d %s", yitcommand, year, type);
	result = system(buf);
	if (WIFEXITED(result))
		switch (WEXITSTATUS(result))
		{
			case 0:
				return TRUE;
			case 1:
				return FALSE;
		}
	error(_("Wild result from command execution"));
	(void) fprintf(stderr, _("%s: command was '%s', result was %d\n"),
				   progname, buf, result);
	for (;;)
		(void) exit(EXIT_FAILURE);
}

static int
lowerit(int a)
{
	a = (unsigned char) a;
	return (isascii(a) && isupper(a)) ? tolower(a) : a;
}

static int
ciequal(const char *ap, const char *bp)
{
	while (lowerit(*ap) == lowerit(*bp++))
		if (*ap++ == '\0')
			return TRUE;
	return FALSE;
}

static int
itsabbr(const char *abbr, const char *word)
{
	if (lowerit(*abbr) != lowerit(*word))
		return FALSE;
	++word;
	while (*++abbr != '\0')
		do
		{
			if (*word == '\0')
				return FALSE;
		} while (lowerit(*word++) != lowerit(*abbr));
	return TRUE;
}

static const struct lookup *
byword(const char *word, const struct lookup * table)
{
	const struct lookup *foundlp;
	const struct lookup *lp;

	if (word == NULL || table == NULL)
		return NULL;

	/*
	 * Look for exact match.
	 */
	for (lp = table; lp->l_word != NULL; ++lp)
		if (ciequal(word, lp->l_word))
			return lp;

	/*
	 * Look for inexact match.
	 */
	foundlp = NULL;
	for (lp = table; lp->l_word != NULL; ++lp)
		if (itsabbr(word, lp->l_word))
		{
			if (foundlp == NULL)
				foundlp = lp;
			else
				return NULL;	/* multiple inexact matches */
		}
	return foundlp;
}

static char **
getfields(char *cp)
{
	char	   *dp;
	char	  **array;
	int			nsubs;

	if (cp == NULL)
		return NULL;
	array = (char **) (void *)
		emalloc((int) ((strlen(cp) + 1) * sizeof *array));
	nsubs = 0;
	for (;;)
	{
		while (isascii(*cp) && isspace((unsigned char) *cp))
			++cp;
		if (*cp == '\0' || *cp == '#')
			break;
		array[nsubs++] = dp = cp;
		do
		{
			if ((*dp = *cp++) != '"')
				++dp;
			else
				while ((*dp = *cp++) != '"')
					if (*dp != '\0')
						++dp;
					else
						error(_("Odd number of quotation marks"));
		} while (*cp != '\0' && *cp != '#' &&
				 (!isascii(*cp) || !isspace((unsigned char) *cp)));
		if (isascii(*cp) && isspace((unsigned char) *cp))
			++cp;
		*dp = '\0';
	}
	array[nsubs] = NULL;
	return array;
}

static long
oadd(long t1, long t2)
{
	long		t;

	t = t1 + t2;
	if ((t2 > 0 && t <= t1) || (t2 < 0 && t >= t1))
	{
		error(_("time overflow"));
		(void) exit(EXIT_FAILURE);
	}
	return t;
}

static pg_time_t
tadd(const pg_time_t t1, long t2)
{
	pg_time_t	t;

	if (t1 == max_time && t2 > 0)
		return max_time;
	if (t1 == min_time && t2 < 0)
		return min_time;
	t = t1 + t2;
	if ((t2 > 0 && t <= t1) || (t2 < 0 && t >= t1))
	{
		error(_("time overflow"));
		(void) exit(EXIT_FAILURE);
	}
	return t;
}

/*
 * Given a rule, and a year, compute the date - in seconds since January 1,
 * 1970, 00:00 LOCAL time - in that year that the rule refers to.
 */

static pg_time_t
rpytime(const struct rule * rp, int wantedy)
{
	int			y,
				m,
				i;
	long		dayoff;			/* with a nod to Margaret O. */
	pg_time_t	t;

	if (wantedy == INT_MIN)
		return min_time;
	if (wantedy == INT_MAX)
		return max_time;
	dayoff = 0;
	m = TM_JANUARY;
	y = EPOCH_YEAR;
	while (wantedy != y)
	{
		if (wantedy > y)
		{
			i = len_years[isleap(y)];
			++y;
		}
		else
		{
			--y;
			i = -len_years[isleap(y)];
		}
		dayoff = oadd(dayoff, eitol(i));
	}
	while (m != rp->r_month)
	{
		i = len_months[isleap(y)][m];
		dayoff = oadd(dayoff, eitol(i));
		++m;
	}
	i = rp->r_dayofmonth;
	if (m == TM_FEBRUARY && i == 29 && !isleap(y))
	{
		if (rp->r_dycode == DC_DOWLEQ)
			--i;
		else
		{
			error(_("use of 2/29 in non leap-year"));
			(void) exit(EXIT_FAILURE);
		}
	}
	--i;
	dayoff = oadd(dayoff, eitol(i));
	if (rp->r_dycode == DC_DOWGEQ || rp->r_dycode == DC_DOWLEQ)
	{
		long		wday;

#define LDAYSPERWEEK	((long) DAYSPERWEEK)
		wday = eitol(EPOCH_WDAY);

		/*
		 * Don't trust mod of negative numbers.
		 */
		if (dayoff >= 0)
			wday = (wday + dayoff) % LDAYSPERWEEK;
		else
		{
			wday -= ((-dayoff) % LDAYSPERWEEK);
			if (wday < 0)
				wday += LDAYSPERWEEK;
		}
		while (wday != eitol(rp->r_wday))
			if (rp->r_dycode == DC_DOWGEQ)
			{
				dayoff = oadd(dayoff, (long) 1);
				if (++wday >= LDAYSPERWEEK)
					wday = 0;
				++i;
			}
			else
			{
				dayoff = oadd(dayoff, (long) -1);
				if (--wday < 0)
					wday = LDAYSPERWEEK - 1;
				--i;
			}
		if (i < 0 || i >= len_months[isleap(y)][m])
		{
			error(_("no day in month matches rule"));
			(void) exit(EXIT_FAILURE);
		}
	}
	if (dayoff < min_time / SECSPERDAY)
		return min_time;
	if (dayoff > max_time / SECSPERDAY)
		return max_time;
	t = (pg_time_t) dayoff *SECSPERDAY;

	return tadd(t, rp->r_tod);
}

static void
newabbr(const char *string)
{
	int			i;

	i = strlen(string) + 1;
	if (charcnt + i > TZ_MAX_CHARS)
	{
		error(_("too many, or too long, time zone abbreviations"));
		(void) exit(EXIT_FAILURE);
	}
	(void) strcpy(&chars[charcnt], string);
	charcnt += eitol(i);
}

static int
mkdirs(char *argname)
{
	char	   *name;
	char	   *cp;

	if (argname == NULL || *argname == '\0')
		return 0;
	cp = name = ecpyalloc(argname);
	while ((cp = strchr(cp + 1, '/')) != 0)
	{
		*cp = '\0';
#ifdef WIN32

		/*
		 * DOS drive specifier?
		 */
		if (isalpha((unsigned char) name[0]) &&
			name[1] == ':' && name[2] == '\0')
		{
			*cp = '/';
			continue;
		}
#endif   /* WIN32 */
		if (!itsdir(name))
		{
			/*
			 * It doesn't seem to exist, so we try to create it. Creation may
			 * fail because of the directory being created by some other
			 * multiprocessor, so we get to do extra checking.
			 */
			if (mkdir(name, MKDIR_UMASK) != 0)
			{
				const char *e = strerror(errno);

				if (errno != EEXIST || !itsdir(name))
				{
					(void) fprintf(stderr,
								   _("%s: Cannot create directory %s: %s\n"),
								   progname, name, e);
					ifree(name);
					return -1;
				}
			}
		}
		*cp = '/';
	}
	ifree(name);
	return 0;
}

static long
eitol(int i)
{
	long		l;

	l = i;
	if ((i < 0 && l >= 0) || (i == 0 && l != 0) || (i > 0 && l <= 0))
	{
		(void) fprintf(stderr,
					   _("%s: %d did not sign extend correctly\n"),
					   progname, i);
		(void) exit(EXIT_FAILURE);
	}
	return l;
}

/*
 * UNIX was a registered trademark of The Open Group in 2003.
 */


#ifdef WIN32
/*
 * To run on win32
 */
int
link(const char *oldpath, const char *newpath)
{
	if (!CopyFile(oldpath, newpath, FALSE))
		return -1;
	return 0;
}
#endif

/*
 *	This allows zic to compile by just returning a dummy value.
 *	localtime.c references it, but no one uses it from zic.
 */
int
pg_open_tzfile(const char *name, char *canonname)
{
	return -1;
}
