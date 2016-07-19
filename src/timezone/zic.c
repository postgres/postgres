/*
 * This file is in the public domain, so clarified as of
 * 2006-07-17 by Arthur David Olson.
 *
 * IDENTIFICATION
 *	  src/timezone/zic.c
 */

#include "postgres_fe.h"

#include <locale.h>
#include <sys/stat.h>
#include <time.h>

#include "pg_getopt.h"

#include "private.h"
#include "tzfile.h"

#define ZIC_VERSION_PRE_2013 '2'
#define ZIC_VERSION '3'

typedef int64 zic_t;
#define ZIC_MIN PG_INT64_MIN
#define ZIC_MAX PG_INT64_MAX

#ifndef ZIC_MAX_ABBR_LEN_WO_WARN
#define ZIC_MAX_ABBR_LEN_WO_WARN	  6
#endif   /* !defined ZIC_MAX_ABBR_LEN_WO_WARN */

#ifndef WIN32
#ifdef S_IRUSR
#define MKDIR_UMASK (S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)
#else
#define MKDIR_UMASK 0755
#endif
#endif

struct rule
{
	const char *r_filename;
	int			r_linenum;
	const char *r_name;

	zic_t		r_loyear;		/* for example, 1986 */
	zic_t		r_hiyear;		/* for example, 1986 */
	const char *r_yrtype;
	bool		r_lowasnum;
	bool		r_hiwasnum;

	int			r_month;		/* 0..11 */

	int			r_dycode;		/* see below */
	int			r_dayofmonth;
	int			r_wday;

	zic_t		r_tod;			/* time from midnight */
	bool		r_todisstd;		/* above is standard time if 1 or wall clock
								 * time if 0 */
	bool		r_todisgmt;		/* above is GMT if 1 or local time if 0 */
	zic_t		r_stdoff;		/* offset from standard time */
	const char *r_abbrvar;		/* variable part of abbreviation */

	int			r_todo;			/* a rule to do (used in outzone) */
	zic_t		r_temp;			/* used in outzone */
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
	zic_t		z_gmtoff;
	const char *z_rule;
	const char *z_format;
	char		z_format_specifier;

	zic_t		z_stdoff;

	struct rule *z_rules;
	int			z_nrules;

	struct rule z_untilrule;
	zic_t		z_untiltime;
};

extern int	link(const char *fromname, const char *toname);

static void memory_exhausted(const char *msg) pg_attribute_noreturn();
static void verror(const char *string, va_list args) pg_attribute_printf(1, 0);
static void error(const char *string,...) pg_attribute_printf(1, 2);
static void warning(const char *string,...) pg_attribute_printf(1, 2);
static void usage(FILE *stream, int status) pg_attribute_noreturn();
static void addtt(zic_t starttime, int type);
static int	addtype(zic_t, char const *, bool, bool, bool);
static void leapadd(zic_t, bool, int, int);
static void adjleap(void);
static void associate(void);
static void dolink(const char *fromfield, const char *tofield);
static char **getfields(char *buf);
static zic_t gethms(const char *string, const char *errstring,
	   bool);
static void infile(const char *filename);
static void inleap(char **fields, int nfields);
static void inlink(char **fields, int nfields);
static void inrule(char **fields, int nfields);
static bool inzcont(char **fields, int nfields);
static bool inzone(char **fields, int nfields);
static bool inzsub(char **, int, bool);
static int	itsdir(const char *name);
static bool is_alpha(char a);
static char lowerit(char);
static bool mkdirs(char *);
static void newabbr(const char *abbr);
static zic_t oadd(zic_t t1, zic_t t2);
static void outzone(const struct zone * zp, int ntzones);
static zic_t rpytime(const struct rule * rp, zic_t wantedy);
static void rulesub(struct rule * rp,
		const char *loyearp, const char *hiyearp,
		const char *typep, const char *monthp,
		const char *dayp, const char *timep);
static zic_t tadd(zic_t t1, zic_t t2);
static bool yearistype(int year, const char *type);

/* Bound on length of what %z can expand to.  */
enum
{
PERCENT_Z_LEN_BOUND = sizeof "+995959" - 1};

static int	charcnt;
static bool errors;
static bool warnings;
static const char *filename;
static int	leapcnt;
static bool leapseen;
static zic_t leapminyear;
static zic_t leapmaxyear;
static int	linenum;
static int	max_abbrvar_len = PERCENT_Z_LEN_BOUND;
static int	max_format_len;
static zic_t max_year;
static zic_t min_year;
static bool noise;
static bool print_abbrevs;
static zic_t print_cutoff;
static const char *rfilename;
static int	rlinenum;
static const char *progname;
static int	timecnt;
static int	timecnt_alloc;
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
static int	nrules_alloc;

static struct zone *zones;
static int	nzones;				/* number of zones */
static int	nzones_alloc;

struct link
{
	const char *l_filename;
	int			l_linenum;
	const char *l_from;
	const char *l_to;
};

static struct link *links;
static int	nlinks;
static int	nlinks_alloc;

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
	{"Rolling", true},
	{"Stationary", false},
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
	zic_t		at;
	unsigned char type;
}	*attypes;
static zic_t gmtoffs[TZ_MAX_TYPES];
static char isdsts[TZ_MAX_TYPES];
static unsigned char abbrinds[TZ_MAX_TYPES];
static bool ttisstds[TZ_MAX_TYPES];
static bool ttisgmts[TZ_MAX_TYPES];
static char chars[TZ_MAX_CHARS];
static zic_t trans[TZ_MAX_LEAPS];
static zic_t corr[TZ_MAX_LEAPS];
static char roll[TZ_MAX_LEAPS];

/*
 * Memory allocation.
 */

static void
memory_exhausted(const char *msg)
{
	fprintf(stderr, _("%s: Memory exhausted: %s\n"), progname, msg);
	exit(EXIT_FAILURE);
}

static size_t
size_product(size_t nitems, size_t itemsize)
{
	if (SIZE_MAX / itemsize < nitems)
		memory_exhausted(_("size overflow"));
	return nitems * itemsize;
}

static void *
memcheck(void *ptr)
{
	if (ptr == NULL)
		memory_exhausted(strerror(errno));
	return ptr;
}

static void *
emalloc(size_t size)
{
	return memcheck(malloc(size));
}

static void *
erealloc(void *ptr, size_t size)
{
	return memcheck(realloc(ptr, size));
}

static char *
ecpyalloc(char const * str)
{
	return memcheck(strdup(str));
}

static void *
growalloc(void *ptr, size_t itemsize, int nitems, int *nitems_alloc)
{
	if (nitems < *nitems_alloc)
		return ptr;
	else
	{
		int			amax = INT_MAX < SIZE_MAX ? INT_MAX : SIZE_MAX;

		if ((amax - 1) / 3 * 2 < *nitems_alloc)
			memory_exhausted(_("int overflow"));
		*nitems_alloc = *nitems_alloc + (*nitems_alloc >> 1) + 1;
		return erealloc(ptr, size_product(*nitems_alloc, itemsize));
	}
}

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
	eats(name, num, NULL, -1);
}

static void
verror(const char *string, va_list args)
{
	/*
	 * Match the format of "cc" to allow sh users to  zic ... 2>&1 | error -t
	 * "*" -v on BSD systems.
	 */
	if (filename)
		fprintf(stderr, _("\"%s\", line %d: "), filename, linenum);
	vfprintf(stderr, string, args);
	if (rfilename != NULL)
		fprintf(stderr, _(" (rule from \"%s\", line %d)"),
				rfilename, rlinenum);
	fprintf(stderr, "\n");
}

static void
error(const char *string,...)
{
	va_list		args;

	va_start(args, string);
	verror(string, args);
	va_end(args);
	errors = true;
}

static void
warning(const char *string,...)
{
	va_list		args;

	fprintf(stderr, _("warning: "));
	va_start(args, string);
	verror(string, args);
	va_end(args);
	warnings = true;
}

static void
close_file(FILE *stream, char const * name)
{
	char const *e = (ferror(stream) ? _("I/O error")
					 : fclose(stream) != 0 ? strerror(errno) : NULL);

	if (e)
	{
		fprintf(stderr, "%s: ", progname);
		if (name)
			fprintf(stderr, "%s: ", name);
		fprintf(stderr, "%s\n", e);
		exit(EXIT_FAILURE);
	}
}

static void
usage(FILE *stream, int status)
{
	fprintf(stream,
			_("%s: usage is %s [ --version ] [ --help ] [ -v ] [ -P ] \\\n"
			  "\t[ -l localtime ] [ -p posixrules ] [ -d directory ] \\\n"
			  "\t[ -L leapseconds ] [ filename ... ]\n\n"
			  "Report bugs to %s.\n"),
			progname, progname, PACKAGE_BUGREPORT);
	if (status == EXIT_SUCCESS)
		close_file(stream, NULL);
	exit(status);
}

static const char *psxrules;
static const char *lcltime;
static const char *directory;
static const char *leapsec;
static const char *yitcommand;

int
main(int argc, char *argv[])
{
	int			i;
	int			j;
	int			c;

#ifndef WIN32
	umask(umask(S_IWGRP | S_IWOTH) | (S_IWGRP | S_IWOTH));
#endif   /* !WIN32 */
	progname = argv[0];
	if (TYPE_BIT(zic_t) <64)
	{
		fprintf(stderr, "%s: %s\n", progname,
				_("wild compilation-time specification of zic_t"));
		return EXIT_FAILURE;
	}
	for (i = 1; i < argc; ++i)
		if (strcmp(argv[i], "--version") == 0)
		{
			printf("zic %s\n", PG_VERSION);
			close_file(stdout, NULL);
			return EXIT_SUCCESS;
		}
		else if (strcmp(argv[i], "--help") == 0)
		{
			usage(stdout, EXIT_SUCCESS);
		}
	while ((c = getopt(argc, argv, "d:l:p:L:vPsy:")) != EOF && c != -1)
		switch (c)
		{
			default:
				usage(stderr, EXIT_FAILURE);
			case 'd':
				if (directory == NULL)
					directory = strdup(optarg);
				else
				{
					fprintf(stderr,
							_("%s: More than one -d option specified\n"),
							progname);
					return EXIT_FAILURE;
				}
				break;
			case 'l':
				if (lcltime == NULL)
					lcltime = strdup(optarg);
				else
				{
					fprintf(stderr,
							_("%s: More than one -l option specified\n"),
							progname);
					return EXIT_FAILURE;
				}
				break;
			case 'p':
				if (psxrules == NULL)
					psxrules = strdup(optarg);
				else
				{
					fprintf(stderr,
							_("%s: More than one -p option specified\n"),
							progname);
					return EXIT_FAILURE;
				}
				break;
			case 'y':
				if (yitcommand == NULL)
					yitcommand = strdup(optarg);
				else
				{
					fprintf(stderr,
							_("%s: More than one -y option specified\n"),
							progname);
					return EXIT_FAILURE;
				}
				break;
			case 'L':
				if (leapsec == NULL)
					leapsec = strdup(optarg);
				else
				{
					fprintf(stderr,
							_("%s: More than one -L option specified\n"),
							progname);
					return EXIT_FAILURE;
				}
				break;
			case 'v':
				noise = true;
				break;
			case 'P':
				print_abbrevs = true;
				print_cutoff = time(NULL);
				break;
			case 's':
				warning(_("-s ignored"));
				break;
		}
	if (optind == argc - 1 && strcmp(argv[optind], "=") == 0)
		usage(stderr, EXIT_FAILURE);	/* usage message by request */
	if (directory == NULL)
		directory = "data";
	if (yitcommand == NULL)
		yitcommand = "yearistype";

	if (optind < argc && leapsec != NULL)
	{
		infile(leapsec);
		adjleap();
	}

	for (i = optind; i < argc; ++i)
		infile(argv[i]);
	if (errors)
		return EXIT_FAILURE;
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
		if (noise)
			for (j = 0; j < nlinks; ++j)
				if (strcmp(links[i].l_to,
						   links[j].l_from) == 0)
					warning(_("link to link"));
	}
	if (lcltime != NULL)
	{
		eat(_("command line"), 1);
		dolink(lcltime, TZDEFAULT);
	}
	if (psxrules != NULL)
	{
		eat(_("command line"), 1);
		dolink(psxrules, TZDEFRULES);
	}
	if (warnings && (ferror(stderr) || fclose(stderr) != 0))
		return EXIT_FAILURE;
	return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}

static bool
componentcheck(char const * name, char const * component,
			   char const * component_end)
{
	enum
	{
	component_len_max = 14};
	size_t		component_len = component_end - component;

	if (component_len == 0)
	{
		if (!*name)
			error(_("empty file name"));
		else
			error(_(component == name
					? "file name '%s' begins with '/'"
					: *component_end
					? "file name '%s' contains '//'"
					: "file name '%s' ends with '/'"),
				  name);
		return false;
	}
	if (0 < component_len && component_len <= 2
		&& component[0] == '.' && component_end[-1] == '.')
	{
		error(_("file name '%s' contains '%.*s' component"),
			  name, (int) component_len, component);
		return false;
	}
	if (noise)
	{
		if (0 < component_len && component[0] == '-')
			warning(_("file name '%s' component contains leading '-'"),
					name);
		if (component_len_max < component_len)
			warning(_("file name '%s' contains overlength component"
					  " '%.*s...'"),
					name, component_len_max, component);
	}
	return true;
}

static bool
namecheck(const char *name)
{
	char const *cp;

	/* Benign characters in a portable file name.  */
	static char const benign[] =
	"-/_"
	"abcdefghijklmnopqrstuvwxyz"
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	/*
	 * Non-control chars in the POSIX portable character set, excluding the
	 * benign characters.
	 */
	static char const printable_and_not_benign[] =
	" !\"#$%&'()*+,.0123456789:;<=>?@[\\]^`{|}~";

	char const *component = name;

	for (cp = name; *cp; cp++)
	{
		unsigned char c = *cp;

		if (noise && !strchr(benign, c))
		{
			warning((strchr(printable_and_not_benign, c)
					 ? _("file name '%s' contains byte '%c'")
					 : _("file name '%s' contains byte '\\%o'")),
					name, c);
		}
		if (c == '/')
		{
			if (!componentcheck(name, component, cp))
				return false;
			component = cp + 1;
		}
	}
	return componentcheck(name, component, cp);
}

static char *
relname(char const * dir, char const * base)
{
	if (*base == '/')
		return ecpyalloc(base);
	else
	{
		size_t		dir_len = strlen(dir);
		bool		needs_slash = dir_len && dir[dir_len - 1] != '/';
		char	   *result = emalloc(dir_len + needs_slash + strlen(base) + 1);

		result[dir_len] = '/';
		strcpy(result + dir_len + needs_slash, base);
		return memcpy(result, dir, dir_len);
	}
}

static void
dolink(char const * fromfield, char const * tofield)
{
	char	   *fromname;
	char	   *toname;
	int			fromisdir;

	fromname = relname(directory, fromfield);
	toname = relname(directory, tofield);

	/*
	 * We get to be careful here since there's a fair chance of root running
	 * us.
	 */
	fromisdir = itsdir(fromname);
	if (fromisdir)
	{
		char const *e = strerror(fromisdir < 0 ? errno : EPERM);

		fprintf(stderr, _("%s: link from %s failed: %s"),
				progname, fromname, e);
		exit(EXIT_FAILURE);
	}
	if (link(fromname, toname) != 0)
	{
		int			link_errno = errno;
		bool		retry_if_link_supported = false;

		if (link_errno == ENOENT || link_errno == ENOTSUP)
		{
			if (!mkdirs(toname))
				exit(EXIT_FAILURE);
			retry_if_link_supported = true;
		}
		if ((link_errno == EEXIST || link_errno == ENOTSUP)
			&& itsdir(toname) == 0
			&& (remove(toname) == 0 || errno == ENOENT))
			retry_if_link_supported = true;
		if (retry_if_link_supported && link_errno != ENOTSUP)
			link_errno = link(fromname, toname) == 0 ? 0 : errno;
		if (link_errno != 0)
		{
#ifdef HAVE_SYMLINK
			const char *s = fromfield;
			const char *t;
			char	   *p;
			size_t		dotdots = 0;
			char	   *symlinkcontents;
			int			symlink_result;

			do
				t = s;
			while ((s = strchr(s, '/'))
				   && strncmp(fromfield, tofield, ++s - fromfield) == 0);

			for (s = tofield + (t - fromfield); *s; s++)
				dotdots += *s == '/';
			symlinkcontents = emalloc(3 * dotdots + strlen(t) + 1);
			for (p = symlinkcontents; dotdots-- != 0; p += 3)
				memcpy(p, "../", 3);
			strcpy(p, t);
			symlink_result = symlink(symlinkcontents, toname);
			free(symlinkcontents);
			if (symlink_result == 0)
			{
				if (link_errno != ENOTSUP)
					warning(_("symbolic link used because hard link failed: %s"),
							strerror(link_errno));
			}
			else
#endif   /* HAVE_SYMLINK */
			{
				FILE	   *fp,
						   *tp;
				int			c;

				fp = fopen(fromname, "rb");
				if (!fp)
				{
					const char *e = strerror(errno);

					fprintf(stderr,
							_("%s: Can't read %s: %s\n"),
							progname, fromname, e);
					exit(EXIT_FAILURE);
				}
				tp = fopen(toname, "wb");
				if (!tp)
				{
					const char *e = strerror(errno);

					fprintf(stderr,
							_("%s: Can't create %s: %s\n"),
							progname, toname, e);
					exit(EXIT_FAILURE);
				}
				while ((c = getc(fp)) != EOF)
					putc(c, tp);
				close_file(fp, fromname);
				close_file(tp, toname);
				if (link_errno != ENOTSUP)
					warning(_("copy used because hard link failed: %s"),
							strerror(link_errno));
			}
		}
	}
	free(fromname);
	free(toname);
}

#define TIME_T_BITS_IN_FILE 64

static zic_t const min_time = MINVAL(zic_t, TIME_T_BITS_IN_FILE);
static zic_t const max_time = MAXVAL(zic_t, TIME_T_BITS_IN_FILE);

/*
 * Estimated time of the Big Bang, in seconds since the POSIX epoch.
 * rounded downward to the negation of a power of two that is
 * comfortably outside the error bounds.
 *
 * zic does not output time stamps before this, partly because they
 * are physically suspect, and partly because GNOME mishandles them; see
 * GNOME bug 730332 <https://bugzilla.gnome.org/show_bug.cgi?id=730332>.
 *
 * For the time of the Big Bang, see:
 *
 * Ade PAR, Aghanim N, Armitage-Caplan C et al.  Planck 2013 results.
 * I. Overview of products and scientific results.
 * arXiv:1303.5062 2013-03-20 20:10:01 UTC
 * <http://arxiv.org/pdf/1303.5062v1> [PDF]
 *
 * Page 36, Table 9, row Age/Gyr, column Planck+WP+highL+BAO 68% limits
 * gives the value 13.798 plus-or-minus 0.037 billion years.
 * Multiplying this by 1000000000 and then by 31557600 (the number of
 * seconds in an astronomical year) gives a value that is comfortably
 * less than 2**59, so BIG_BANG is - 2**59.
 *
 * BIG_BANG is approximate, and may change in future versions.
 * Please do not rely on its exact value.
 */

#ifndef BIG_BANG
#define BIG_BANG (- (((zic_t) 1) << 59))
#endif

static const zic_t big_bang_time = BIG_BANG;

/* Return 1 if NAME is a directory, 0 if it's something else, -1 if trouble.  */
static int
itsdir(char const * name)
{
	struct stat st;
	int			res = stat(name, &st);

#ifdef S_ISDIR
	if (res == 0)
		return S_ISDIR(st.st_mode) != 0;
#endif
	if (res == 0 || errno == EOVERFLOW)
	{
		char	   *nameslashdot = relname(name, ".");
		bool		dir = stat(nameslashdot, &st) == 0 || errno == EOVERFLOW;

		free(nameslashdot);
		return dir;
	}
	return -1;
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
		qsort(rules, nrules, sizeof *rules, rcomp);
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
								  true);

			/*
			 * Note, though, that if there's no rule, a '%s' in the format is
			 * a bad thing.
			 */
			if (zp->z_format_specifier == 's')
				error("%s", _("%s in ruleless zone"));
		}
	}
	if (errors)
		exit(EXIT_FAILURE);
}

static void
infile(const char *name)
{
	FILE	   *fp;
	char	  **fields;
	char	   *cp;
	const struct lookup *lp;
	int			nfields;
	bool		wantcont;
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

		fprintf(stderr, _("%s: Cannot open %s: %s\n"),
				progname, name, e);
		exit(EXIT_FAILURE);
	}
	wantcont = false;
	for (num = 1;; ++num)
	{
		eat(name, num);
		if (fgets(buf, sizeof buf, fp) != buf)
			break;
		cp = strchr(buf, '\n');
		if (cp == NULL)
		{
			error(_("line too long"));
			exit(EXIT_FAILURE);
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
						wantcont = false;
						break;
					case LC_ZONE:
						wantcont = inzone(fields, nfields);
						break;
					case LC_LINK:
						inlink(fields, nfields);
						wantcont = false;
						break;
					case LC_LEAP:
						if (name != leapsec)
							warning(_("%s: Leap line in non leap"
									  " seconds file %s"),
									progname, name);
						else
							inleap(fields, nfields);
						wantcont = false;
						break;
					default:	/* "cannot happen" */
						fprintf(stderr,
								_("%s: panic: Invalid l_value %d\n"),
								progname, lp->l_value);
						exit(EXIT_FAILURE);
				}
		}
		free(fields);
	}
	close_file(fp, filename);
	if (wantcont)
		error(_("expected continuation line not found"));
}

/*
 * Convert a string of one of the forms
 *	h	-h	hh:mm	-hh:mm	hh:mm:ss	-hh:mm:ss
 * into a number of seconds.
 * A null string maps to zero.
 * Call error with errstring and return zero on errors.
 */
static zic_t
gethms(char const * string, char const * errstring, bool signable)
{
	/* PG: make hh be int not zic_t to avoid sscanf portability issues */
	int			hh;
	int			mm,
				ss,
				sign;
	char		xs;

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
	if (sscanf(string, "%d%c", &hh, &xs) == 1)
		mm = ss = 0;
	else if (sscanf(string, "%d:%d%c", &hh, &mm, &xs) == 2)
		ss = 0;
	else if (sscanf(string, "%d:%d:%d%c", &hh, &mm, &ss, &xs)
			 != 3)
	{
		error("%s", errstring);
		return 0;
	}
	if (hh < 0 ||
		mm < 0 || mm >= MINSPERHOUR ||
		ss < 0 || ss > SECSPERMIN)
	{
		error("%s", errstring);
		return 0;
	}
	/* Some compilers warn that this test is unsatisfiable for 32-bit ints */
#if INT_MAX > PG_INT32_MAX
	if (ZIC_MAX / SECSPERHOUR < hh)
	{
		error(_("time overflow"));
		return 0;
	}
#endif
	if (noise && (hh > HOURSPERDAY ||
				  (hh == HOURSPERDAY && (mm != 0 || ss != 0))))
		warning(_("values over 24 hours not handled by pre-2007 versions of zic"));
	return oadd(sign * (zic_t) hh * SECSPERHOUR,
				sign * (mm * SECSPERMIN + ss));
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
	r.r_stdoff = gethms(fields[RF_STDOFF], _("invalid saved time"), true);
	rulesub(&r, fields[RF_LOYEAR], fields[RF_HIYEAR], fields[RF_COMMAND],
			fields[RF_MONTH], fields[RF_DAY], fields[RF_TOD]);
	r.r_name = ecpyalloc(fields[RF_NAME]);
	r.r_abbrvar = ecpyalloc(fields[RF_ABBRVAR]);
	if (max_abbrvar_len < strlen(r.r_abbrvar))
		max_abbrvar_len = strlen(r.r_abbrvar);
	rules = growalloc(rules, sizeof *rules, nrules, &nrules_alloc);
	rules[nrules++] = r;
}

static bool
inzone(char **fields, int nfields)
{
	int			i;

	if (nfields < ZONE_MINFIELDS || nfields > ZONE_MAXFIELDS)
	{
		error(_("wrong number of fields on Zone line"));
		return false;
	}
	if (strcmp(fields[ZF_NAME], TZDEFAULT) == 0 && lcltime != NULL)
	{
		error(
			  _("\"Zone %s\" line and -l option are mutually exclusive"),
			  TZDEFAULT);
		return false;
	}
	if (strcmp(fields[ZF_NAME], TZDEFRULES) == 0 && psxrules != NULL)
	{
		error(
			  _("\"Zone %s\" line and -p option are mutually exclusive"),
			  TZDEFRULES);
		return false;
	}
	for (i = 0; i < nzones; ++i)
		if (zones[i].z_name != NULL &&
			strcmp(zones[i].z_name, fields[ZF_NAME]) == 0)
		{
			error(
				  _("duplicate zone name %s (file \"%s\", line %d)"),
				  fields[ZF_NAME],
				  zones[i].z_filename,
				  zones[i].z_linenum);
			return false;
		}
	return inzsub(fields, nfields, false);
}

static bool
inzcont(char **fields, int nfields)
{
	if (nfields < ZONEC_MINFIELDS || nfields > ZONEC_MAXFIELDS)
	{
		error(_("wrong number of fields on Zone continuation line"));
		return false;
	}
	return inzsub(fields, nfields, true);
}

static bool
inzsub(char **fields, int nfields, bool iscont)
{
	char	   *cp;
	char	   *cp1;
	static struct zone z;
	int			i_gmtoff,
				i_rule,
				i_format;
	int			i_untilyear,
				i_untilmonth;
	int			i_untilday,
				i_untiltime;
	bool		hasuntil;

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
	else if (!namecheck(fields[ZF_NAME]))
		return false;
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
	z.z_gmtoff = gethms(fields[i_gmtoff], _("invalid UT offset"), true);
	if ((cp = strchr(fields[i_format], '%')) != 0)
	{
		if ((*++cp != 's' && *cp != 'z') || strchr(cp, '%')
			|| strchr(fields[i_format], '/'))
		{
			error(_("invalid abbreviation format"));
			return false;
		}
	}
	z.z_rule = ecpyalloc(fields[i_rule]);
	z.z_format = cp1 = ecpyalloc(fields[i_format]);
	z.z_format_specifier = cp ? *cp : '\0';
	if (z.z_format_specifier == 'z')
	{
		if (noise)
			warning(_("format '%s' not handled by pre-2015 versions of zic"),
					z.z_format);
		cp1[cp - fields[i_format]] = 's';
	}
	if (max_format_len < strlen(z.z_format))
		max_format_len = strlen(z.z_format);
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
			return false;
		}
	}
	zones = growalloc(zones, sizeof *zones, nzones, &nzones_alloc);
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

	/* PG: make year be int not zic_t to avoid sscanf portability issues */
	int			year;
	int			month,
				day;
	zic_t		dayoff,
				tod;
	zic_t		t;
	char		xs;

	if (nfields != LEAP_FIELDS)
	{
		error(_("wrong number of fields on Leap line"));
		return;
	}
	dayoff = 0;
	cp = fields[LP_YEAR];
	if (sscanf(cp, "%d%c", &year, &xs) != 1)
	{
		/*
		 * Leapin' Lizards!
		 */
		error(_("invalid leaping year"));
		return;
	}
	if (!leapseen || leapmaxyear < year)
		leapmaxyear = year;
	if (!leapseen || leapminyear > year)
		leapminyear = year;
	leapseen = true;
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
		dayoff = oadd(dayoff, i);
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
		dayoff = oadd(dayoff, i);
		++j;
	}
	cp = fields[LP_DAY];
	if (sscanf(cp, "%d%c", &day, &xs) != 1 ||
		day <= 0 || day > len_months[isleap(year)][month])
	{
		error(_("invalid day of month"));
		return;
	}
	dayoff = oadd(dayoff, day - 1);
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
	t = dayoff * SECSPERDAY;
	tod = gethms(fields[LP_TIME], _("invalid time of day"), false);
	cp = fields[LP_CORR];
	{
		bool		positive;
		int			count;

		if (strcmp(cp, "") == 0)
		{						/* infile() turns "-" into "" */
			positive = false;
			count = 1;
		}
		else if (strcmp(cp, "--") == 0)
		{
			positive = false;
			count = 2;
		}
		else if (strcmp(cp, "+") == 0)
		{
			positive = true;
			count = 1;
		}
		else if (strcmp(cp, "++") == 0)
		{
			positive = true;
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
		t = tadd(t, tod);
		if (t < big_bang_time)
		{
			error(_("leap second precedes Big Bang"));
			return;
		}
		leapadd(t, positive, lp->l_value, count);
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
	if (!namecheck(fields[LF_TO]))
		return;
	l.l_filename = filename;
	l.l_linenum = linenum;
	l.l_from = ecpyalloc(fields[LF_FROM]);
	l.l_to = ecpyalloc(fields[LF_TO]);
	links = growalloc(links, sizeof *links, nlinks, &nlinks_alloc);
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
	char		xs;

	/* PG: year_tmp is to avoid sscanf portability issues */
	int			year_tmp;

	if ((lp = byword(monthp, mon_names)) == NULL)
	{
		error(_("invalid month name"));
		return;
	}
	rp->r_month = lp->l_value;
	rp->r_todisstd = false;
	rp->r_todisgmt = false;
	dp = ecpyalloc(timep);
	if (*dp != '\0')
	{
		ep = dp + strlen(dp) - 1;
		switch (lowerit(*ep))
		{
			case 's':			/* Standard */
				rp->r_todisstd = true;
				rp->r_todisgmt = false;
				*ep = '\0';
				break;
			case 'w':			/* Wall */
				rp->r_todisstd = false;
				rp->r_todisgmt = false;
				*ep = '\0';
				break;
			case 'g':			/* Greenwich */
			case 'u':			/* Universal */
			case 'z':			/* Zulu */
				rp->r_todisstd = true;
				rp->r_todisgmt = true;
				*ep = '\0';
				break;
		}
	}
	rp->r_tod = gethms(dp, _("invalid time of day"), false);
	free(dp);

	/*
	 * Year work.
	 */
	cp = loyearp;
	lp = byword(cp, begin_years);
	rp->r_lowasnum = lp == NULL;
	if (!rp->r_lowasnum)
		switch ((int) lp->l_value)
		{
			case YR_MINIMUM:
				rp->r_loyear = ZIC_MIN;
				break;
			case YR_MAXIMUM:
				rp->r_loyear = ZIC_MAX;
				break;
			default:			/* "cannot happen" */
				fprintf(stderr,
						_("%s: panic: Invalid l_value %d\n"),
						progname, lp->l_value);
				exit(EXIT_FAILURE);
		}
	else if (sscanf(cp, "%d%c", &year_tmp, &xs) == 1)
		rp->r_loyear = year_tmp;
	else
	{
		error(_("invalid starting year"));
		return;
	}
	cp = hiyearp;
	lp = byword(cp, end_years);
	rp->r_hiwasnum = lp == NULL;
	if (!rp->r_hiwasnum)
		switch ((int) lp->l_value)
		{
			case YR_MINIMUM:
				rp->r_hiyear = ZIC_MIN;
				break;
			case YR_MAXIMUM:
				rp->r_hiyear = ZIC_MAX;
				break;
			case YR_ONLY:
				rp->r_hiyear = rp->r_loyear;
				break;
			default:			/* "cannot happen" */
				fprintf(stderr,
						_("%s: panic: Invalid l_value %d\n"),
						progname, lp->l_value);
				exit(EXIT_FAILURE);
		}
	else if (sscanf(cp, "%d%c", &year_tmp, &xs) == 1)
		rp->r_hiyear = year_tmp;
	else
	{
		error(_("invalid ending year"));
		return;
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
		if ((ep = strchr(dp, '<')) != NULL)
			rp->r_dycode = DC_DOWLEQ;
		else if ((ep = strchr(dp, '>')) != NULL)
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
				free(dp);
				return;
			}
			if ((lp = byword(dp, wday_names)) == NULL)
			{
				error(_("invalid weekday name"));
				free(dp);
				return;
			}
			rp->r_wday = lp->l_value;
		}
		if (sscanf(ep, "%d%c", &rp->r_dayofmonth, &xs) != 1 ||
			rp->r_dayofmonth <= 0 ||
			(rp->r_dayofmonth > len_months[1][rp->r_month]))
		{
			error(_("invalid day of month"));
			free(dp);
			return;
		}
	}
	free(dp);
}

static void
convert(const int32 val, char *const buf)
{
	int			i;
	int			shift;
	unsigned char *const b = (unsigned char *) buf;

	for (i = 0, shift = 24; i < 4; ++i, shift -= 8)
		b[i] = val >> shift;
}

static void
convert64(const zic_t val, char *const buf)
{
	int			i;
	int			shift;
	unsigned char *const b = (unsigned char *) buf;

	for (i = 0, shift = 56; i < 8; ++i, shift -= 8)
		b[i] = val >> shift;
}

static void
puttzcode(const int32 val, FILE *const fp)
{
	char		buf[4];

	convert(val, buf);
	fwrite(buf, sizeof buf, 1, fp);
}

static void
puttzcode64(const zic_t val, FILE *const fp)
{
	char		buf[8];

	convert64(val, buf);
	fwrite(buf, sizeof buf, 1, fp);
}

static int
atcomp(const void *avp, const void *bvp)
{
	const zic_t a = ((const struct attype *) avp)->at;
	const zic_t b = ((const struct attype *) bvp)->at;

	return (a < b) ? -1 : (a > b);
}

static bool
is32(const zic_t x)
{
	return x == ((zic_t) ((int32) x));
}

static void
writezone(const char *const name, const char *const string, char version)
{
	FILE	   *fp;
	int			i,
				j;
	int			leapcnt32,
				leapi32;
	int			timecnt32,
				timei32;
	int			pass;
	char	   *fullname;
	static const struct tzhead tzh0;
	static struct tzhead tzh;
	zic_t	   *ats = emalloc(size_product(timecnt, sizeof *ats + 1));
	void	   *typesptr = ats + timecnt;
	unsigned char *types = typesptr;

	/*
	 * Sort.
	 */
	if (timecnt > 1)
		qsort(attypes, timecnt, sizeof *attypes, atcomp);

	/*
	 * Optimize.
	 */
	{
		int			fromi;
		int			toi;

		toi = 0;
		fromi = 0;
		while (fromi < timecnt && attypes[fromi].at < big_bang_time)
			++fromi;
		for (; fromi < timecnt; ++fromi)
		{
			if (toi > 1 && ((attypes[fromi].at +
							 gmtoffs[attypes[toi - 1].type]) <=
							(attypes[toi - 1].at +
							 gmtoffs[attypes[toi - 2].type])))
			{
				attypes[toi - 1].type =
					attypes[fromi].type;
				continue;
			}
			if (toi == 0 ||
				attypes[toi - 1].type != attypes[fromi].type)
				attypes[toi++] = attypes[fromi];
		}
		timecnt = toi;
	}
	if (noise && timecnt > 1200)
		warning(_("pre-2014 clients may mishandle"
				  " more than 1200 transition times"));

	/*
	 * Transfer.
	 */
	for (i = 0; i < timecnt; ++i)
	{
		ats[i] = attypes[i].at;
		types[i] = attypes[i].type;
	}

	/*
	 * Correct for leap seconds.
	 */
	for (i = 0; i < timecnt; ++i)
	{
		j = leapcnt;
		while (--j >= 0)
			if (ats[i] > trans[j] - corr[j])
			{
				ats[i] = tadd(ats[i], corr[j]);
				break;
			}
	}

	/*
	 * Figure out 32-bit-limited starts and counts.
	 */
	timecnt32 = timecnt;
	timei32 = 0;
	leapcnt32 = leapcnt;
	leapi32 = 0;
	while (timecnt32 > 0 && !is32(ats[timecnt32 - 1]))
		--timecnt32;
	while (timecnt32 > 0 && !is32(ats[timei32]))
	{
		--timecnt32;
		++timei32;
	}

	/*
	 * Output an INT32_MIN "transition" if appropriate; see below.
	 */
	if (timei32 > 0 && ats[timei32] > PG_INT32_MIN)
	{
		--timei32;
		++timecnt32;
	}
	while (leapcnt32 > 0 && !is32(trans[leapcnt32 - 1]))
		--leapcnt32;
	while (leapcnt32 > 0 && !is32(trans[leapi32]))
	{
		--leapcnt32;
		++leapi32;
	}
	fullname = relname(directory, name);

	/*
	 * Remove old file, if any, to snap links.
	 */
	if (itsdir(fullname) == 0 && remove(fullname) != 0 && errno != ENOENT)
	{
		const char *e = strerror(errno);

		fprintf(stderr, _("%s: Cannot remove %s: %s\n"),
				progname, fullname, e);
		exit(EXIT_FAILURE);
	}
	if ((fp = fopen(fullname, "wb")) == NULL)
	{
		if (!mkdirs(fullname))
			exit(EXIT_FAILURE);
		if ((fp = fopen(fullname, "wb")) == NULL)
		{
			const char *e = strerror(errno);

			fprintf(stderr, _("%s: Cannot create %s: %s\n"),
					progname, fullname, e);
			exit(EXIT_FAILURE);
		}
	}
	for (pass = 1; pass <= 2; ++pass)
	{
		int			thistimei,
					thistimecnt;
		int			thisleapi,
					thisleapcnt;
		int			thistimelim,
					thisleaplim;
		int			writetype[TZ_MAX_TYPES];
		int			typemap[TZ_MAX_TYPES];
		int			thistypecnt;
		char		thischars[TZ_MAX_CHARS];
		char		thischarcnt;
		int			indmap[TZ_MAX_CHARS];

		if (pass == 1)
		{
			thistimei = timei32;
			thistimecnt = timecnt32;
			thisleapi = leapi32;
			thisleapcnt = leapcnt32;
		}
		else
		{
			thistimei = 0;
			thistimecnt = timecnt;
			thisleapi = 0;
			thisleapcnt = leapcnt;
		}
		thistimelim = thistimei + thistimecnt;
		thisleaplim = thisleapi + thisleapcnt;
		for (i = 0; i < typecnt; ++i)
			writetype[i] = thistimecnt == timecnt;
		if (thistimecnt == 0)
		{
			/*
			 * No transition times fall in the current (32- or 64-bit) window.
			 */
			if (typecnt != 0)
				writetype[typecnt - 1] = true;
		}
		else
		{
			for (i = thistimei - 1; i < thistimelim; ++i)
				if (i >= 0)
					writetype[types[i]] = true;

			/*
			 * For America/Godthab and Antarctica/Palmer
			 */
			if (thistimei == 0)
				writetype[0] = true;
		}
#ifndef LEAVE_SOME_PRE_2011_SYSTEMS_IN_THE_LURCH

		/*
		 * For some pre-2011 systems: if the last-to-be-written standard (or
		 * daylight) type has an offset different from the most recently used
		 * offset, append an (unused) copy of the most recently used type (to
		 * help get global "altzone" and "timezone" variables set correctly).
		 */
		{
			int			mrudst,
						mrustd,
						hidst,
						histd,
						type;

			hidst = histd = mrudst = mrustd = -1;
			for (i = thistimei; i < thistimelim; ++i)
				if (isdsts[types[i]])
					mrudst = types[i];
				else
					mrustd = types[i];
			for (i = 0; i < typecnt; ++i)
				if (writetype[i])
				{
					if (isdsts[i])
						hidst = i;
					else
						histd = i;
				}
			if (hidst >= 0 && mrudst >= 0 && hidst != mrudst &&
				gmtoffs[hidst] != gmtoffs[mrudst])
			{
				isdsts[mrudst] = -1;
				type = addtype(gmtoffs[mrudst],
							   &chars[abbrinds[mrudst]],
							   true,
							   ttisstds[mrudst],
							   ttisgmts[mrudst]);
				isdsts[mrudst] = 1;
				writetype[type] = true;
			}
			if (histd >= 0 && mrustd >= 0 && histd != mrustd &&
				gmtoffs[histd] != gmtoffs[mrustd])
			{
				isdsts[mrustd] = -1;
				type = addtype(gmtoffs[mrustd],
							   &chars[abbrinds[mrustd]],
							   false,
							   ttisstds[mrustd],
							   ttisgmts[mrustd]);
				isdsts[mrustd] = 0;
				writetype[type] = true;
			}
		}
#endif   /* !defined
								 * LEAVE_SOME_PRE_2011_SYSTEMS_IN_THE_LURCH */
		thistypecnt = 0;
		for (i = 0; i < typecnt; ++i)
			typemap[i] = writetype[i] ? thistypecnt++ : -1;
		for (i = 0; i < sizeof indmap / sizeof indmap[0]; ++i)
			indmap[i] = -1;
		thischarcnt = 0;
		for (i = 0; i < typecnt; ++i)
		{
			char	   *thisabbr;

			if (!writetype[i])
				continue;
			if (indmap[abbrinds[i]] >= 0)
				continue;
			thisabbr = &chars[abbrinds[i]];
			for (j = 0; j < thischarcnt; ++j)
				if (strcmp(&thischars[j], thisabbr) == 0)
					break;
			if (j == thischarcnt)
			{
				strcpy(&thischars[(int) thischarcnt],
					   thisabbr);
				thischarcnt += strlen(thisabbr) + 1;
			}
			indmap[abbrinds[i]] = j;
		}
#define DO(field)	fwrite(tzh.field, sizeof tzh.field, 1, fp)
		tzh = tzh0;
		strncpy(tzh.tzh_magic, TZ_MAGIC, sizeof tzh.tzh_magic);
		tzh.tzh_version[0] = version;
		convert(thistypecnt, tzh.tzh_ttisgmtcnt);
		convert(thistypecnt, tzh.tzh_ttisstdcnt);
		convert(thisleapcnt, tzh.tzh_leapcnt);
		convert(thistimecnt, tzh.tzh_timecnt);
		convert(thistypecnt, tzh.tzh_typecnt);
		convert(thischarcnt, tzh.tzh_charcnt);
		DO(tzh_magic);
		DO(tzh_version);
		DO(tzh_reserved);
		DO(tzh_ttisgmtcnt);
		DO(tzh_ttisstdcnt);
		DO(tzh_leapcnt);
		DO(tzh_timecnt);
		DO(tzh_typecnt);
		DO(tzh_charcnt);
#undef DO
		for (i = thistimei; i < thistimelim; ++i)
			if (pass == 1)

				/*
				 * Output an INT32_MIN "transition" if appropriate; see above.
				 */
				puttzcode(((ats[i] < PG_INT32_MIN) ?
						   PG_INT32_MIN : ats[i]), fp);
			else
			{
				puttzcode64(ats[i], fp);

				/* Print current timezone abbreviations if requested */
				if (print_abbrevs &&
					(i == thistimelim - 1 || ats[i + 1] > print_cutoff))
				{
					unsigned char tm = typemap[types[i]];
					char	   *thisabbrev = &thischars[indmap[abbrinds[tm]]];

					/* filter out assorted junk entries */
					if (strcmp(thisabbrev, GRANDPARENTED) != 0 &&
						strcmp(thisabbrev, "zzz") != 0)
						fprintf(stdout, "%s\t" INT64_FORMAT "%s\n",
								thisabbrev,
								gmtoffs[tm],
								isdsts[tm] ? "\tD" : "");
				}
			}
		for (i = thistimei; i < thistimelim; ++i)
		{
			unsigned char uc;

			uc = typemap[types[i]];
			fwrite(&uc, sizeof uc, 1, fp);
		}
		for (i = 0; i < typecnt; ++i)
			if (writetype[i])
			{
				puttzcode(gmtoffs[i], fp);
				putc(isdsts[i], fp);
				putc((unsigned char) indmap[abbrinds[i]], fp);
			}
		if (thischarcnt != 0)
			fwrite(thischars, sizeof thischars[0],
				   thischarcnt, fp);
		for (i = thisleapi; i < thisleaplim; ++i)
		{
			zic_t		todo;

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
					while (j < timecnt &&
						   trans[i] >= ats[j])
						++j;
					j = types[j - 1];
				}
				todo = tadd(trans[i], -gmtoffs[j]);
			}
			else
				todo = trans[i];
			if (pass == 1)
				puttzcode(todo, fp);
			else
				puttzcode64(todo, fp);
			puttzcode(corr[i], fp);
		}
		for (i = 0; i < typecnt; ++i)
			if (writetype[i])
				putc(ttisstds[i], fp);
		for (i = 0; i < typecnt; ++i)
			if (writetype[i])
				putc(ttisgmts[i], fp);
	}
	fprintf(fp, "\n%s\n", string);
	close_file(fp, fullname);
	free(ats);
	free(fullname);
}

static char const *
abbroffset(char *buf, zic_t offset)
{
	char		sign = '+';
	int			seconds,
				minutes;

	if (offset < 0)
	{
		offset = -offset;
		sign = '-';
	}

	seconds = offset % SECSPERMIN;
	offset /= SECSPERMIN;
	minutes = offset % MINSPERHOUR;
	offset /= MINSPERHOUR;
	if (100 <= offset)
	{
		error(_("%%z UTC offset magnitude exceeds 99:59:59"));
		return "%z";
	}
	else
	{
		char	   *p = buf;

		*p++ = sign;
		*p++ = '0' + offset / 10;
		*p++ = '0' + offset % 10;
		if (minutes | seconds)
		{
			*p++ = '0' + minutes / 10;
			*p++ = '0' + minutes % 10;
			if (seconds)
			{
				*p++ = '0' + seconds / 10;
				*p++ = '0' + seconds % 10;
			}
		}
		*p = '\0';
		return buf;
	}
}

static size_t
doabbr(char *abbr, struct zone const * zp, char const * letters,
	   zic_t stdoff, bool doquotes)
{
	char	   *cp;
	char	   *slashp;
	size_t		len;
	char const *format = zp->z_format;

	slashp = strchr(format, '/');
	if (slashp == NULL)
	{
		char		letterbuf[PERCENT_Z_LEN_BOUND + 1];

		if (zp->z_format_specifier == 'z')
			letters = abbroffset(letterbuf, zp->z_gmtoff + stdoff);
		else if (!letters)
			letters = "%s";
		sprintf(abbr, format, letters);
	}
	else if (stdoff != 0)
	{
		strcpy(abbr, slashp + 1);
	}
	else
	{
		memcpy(abbr, format, slashp - format);
		abbr[slashp - format] = '\0';
	}
	len = strlen(abbr);
	if (!doquotes)
		return len;
	for (cp = abbr; is_alpha(*cp); cp++)
		continue;
	if (len > 0 && *cp == '\0')
		return len;
	abbr[len + 2] = '\0';
	abbr[len + 1] = '>';
	memmove(abbr + 1, abbr, len);
	abbr[0] = '<';
	return len + 2;
}

static void
updateminmax(const zic_t x)
{
	if (min_year > x)
		min_year = x;
	if (max_year < x)
		max_year = x;
}

static int
stringoffset(char *result, zic_t offset)
{
	int			hours;
	int			minutes;
	int			seconds;
	bool		negative = offset < 0;
	int			len = negative;

	if (negative)
	{
		offset = -offset;
		result[0] = '-';
	}
	seconds = offset % SECSPERMIN;
	offset /= SECSPERMIN;
	minutes = offset % MINSPERHOUR;
	offset /= MINSPERHOUR;
	hours = offset;
	if (hours >= HOURSPERDAY * DAYSPERWEEK)
	{
		result[0] = '\0';
		return 0;
	}
	len += sprintf(result + len, "%d", hours);
	if (minutes != 0 || seconds != 0)
	{
		len += sprintf(result + len, ":%02d", minutes);
		if (seconds != 0)
			len += sprintf(result + len, ":%02d", seconds);
	}
	return len;
}

static int
stringrule(char *result, const struct rule * const rp, const zic_t dstoff,
		   const zic_t gmtoff)
{
	zic_t		tod = rp->r_tod;
	int			compat = 0;

	if (rp->r_dycode == DC_DOM)
	{
		int			month,
					total;

		if (rp->r_dayofmonth == 29 && rp->r_month == TM_FEBRUARY)
			return -1;
		total = 0;
		for (month = 0; month < rp->r_month; ++month)
			total += len_months[0][month];
		/* Omit the "J" in Jan and Feb, as that's shorter.  */
		if (rp->r_month <= 1)
			result += sprintf(result, "%d", total + rp->r_dayofmonth - 1);
		else
			result += sprintf(result, "J%d", total + rp->r_dayofmonth);
	}
	else
	{
		int			week;
		int			wday = rp->r_wday;
		int			wdayoff;

		if (rp->r_dycode == DC_DOWGEQ)
		{
			wdayoff = (rp->r_dayofmonth - 1) % DAYSPERWEEK;
			if (wdayoff)
				compat = 2013;
			wday -= wdayoff;
			tod += wdayoff * SECSPERDAY;
			week = 1 + (rp->r_dayofmonth - 1) / DAYSPERWEEK;
		}
		else if (rp->r_dycode == DC_DOWLEQ)
		{
			if (rp->r_dayofmonth == len_months[1][rp->r_month])
				week = 5;
			else
			{
				wdayoff = rp->r_dayofmonth % DAYSPERWEEK;
				if (wdayoff)
					compat = 2013;
				wday -= wdayoff;
				tod += wdayoff * SECSPERDAY;
				week = rp->r_dayofmonth / DAYSPERWEEK;
			}
		}
		else
			return -1;			/* "cannot happen" */
		if (wday < 0)
			wday += DAYSPERWEEK;
		result += sprintf(result, "M%d.%d.%d",
						  rp->r_month + 1, week, wday);
	}
	if (rp->r_todisgmt)
		tod += gmtoff;
	if (rp->r_todisstd && rp->r_stdoff == 0)
		tod += dstoff;
	if (tod != 2 * SECSPERMIN * MINSPERHOUR)
	{
		*result++ = '/';
		if (!stringoffset(result, tod))
			return -1;
		if (tod < 0)
		{
			if (compat < 2013)
				compat = 2013;
		}
		else if (SECSPERDAY <= tod)
		{
			if (compat < 1994)
				compat = 1994;
		}
	}
	return compat;
}

static int
rule_cmp(struct rule const * a, struct rule const * b)
{
	if (!a)
		return -!!b;
	if (!b)
		return 1;
	if (a->r_hiyear != b->r_hiyear)
		return a->r_hiyear < b->r_hiyear ? -1 : 1;
	if (a->r_month - b->r_month != 0)
		return a->r_month - b->r_month;
	return a->r_dayofmonth - b->r_dayofmonth;
}

enum
{
YEAR_BY_YEAR_ZONE = 1};

static int
stringzone(char *result, const struct zone * const zpfirst, const int zonecount)
{
	const struct zone *zp;
	struct rule *rp;
	struct rule *stdrp;
	struct rule *dstrp;
	int			i;
	const char *abbrvar;
	int			compat = 0;
	int			c;
	size_t		len;
	int			offsetlen;
	struct rule stdr,
				dstr;

	result[0] = '\0';
	zp = zpfirst + zonecount - 1;
	stdrp = dstrp = NULL;
	for (i = 0; i < zp->z_nrules; ++i)
	{
		rp = &zp->z_rules[i];
		if (rp->r_hiwasnum || rp->r_hiyear != ZIC_MAX)
			continue;
		if (rp->r_yrtype != NULL)
			continue;
		if (rp->r_stdoff == 0)
		{
			if (stdrp == NULL)
				stdrp = rp;
			else
				return -1;
		}
		else
		{
			if (dstrp == NULL)
				dstrp = rp;
			else
				return -1;
		}
	}
	if (stdrp == NULL && dstrp == NULL)
	{
		/*
		 * There are no rules running through "max". Find the latest std rule
		 * in stdabbrrp and latest rule of any type in stdrp.
		 */
		struct rule *stdabbrrp = NULL;

		for (i = 0; i < zp->z_nrules; ++i)
		{
			rp = &zp->z_rules[i];
			if (rp->r_stdoff == 0 && rule_cmp(stdabbrrp, rp) < 0)
				stdabbrrp = rp;
			if (rule_cmp(stdrp, rp) < 0)
				stdrp = rp;
		}

		/*
		 * Horrid special case: if year is 2037, presume this is a zone
		 * handled on a year-by-year basis; do not try to apply a rule to the
		 * zone.
		 */
		if (stdrp != NULL && stdrp->r_hiyear == 2037)
			return YEAR_BY_YEAR_ZONE;

		if (stdrp != NULL && stdrp->r_stdoff != 0)
		{
			/* Perpetual DST.  */
			dstr.r_month = TM_JANUARY;
			dstr.r_dycode = DC_DOM;
			dstr.r_dayofmonth = 1;
			dstr.r_tod = 0;
			dstr.r_todisstd = dstr.r_todisgmt = false;
			dstr.r_stdoff = stdrp->r_stdoff;
			dstr.r_abbrvar = stdrp->r_abbrvar;
			stdr.r_month = TM_DECEMBER;
			stdr.r_dycode = DC_DOM;
			stdr.r_dayofmonth = 31;
			stdr.r_tod = SECSPERDAY + stdrp->r_stdoff;
			stdr.r_todisstd = stdr.r_todisgmt = false;
			stdr.r_stdoff = 0;
			stdr.r_abbrvar
				= (stdabbrrp ? stdabbrrp->r_abbrvar : "");
			dstrp = &dstr;
			stdrp = &stdr;
		}
	}
	if (stdrp == NULL && (zp->z_nrules != 0 || zp->z_stdoff != 0))
		return -1;
	abbrvar = (stdrp == NULL) ? "" : stdrp->r_abbrvar;
	len = doabbr(result, zp, abbrvar, 0, true);
	offsetlen = stringoffset(result + len, -zp->z_gmtoff);
	if (!offsetlen)
	{
		result[0] = '\0';
		return -1;
	}
	len += offsetlen;
	if (dstrp == NULL)
		return compat;
	len += doabbr(result + len, zp, dstrp->r_abbrvar, dstrp->r_stdoff, true);
	if (dstrp->r_stdoff != SECSPERMIN * MINSPERHOUR)
	{
		offsetlen = stringoffset(result + len,
								 -(zp->z_gmtoff + dstrp->r_stdoff));
		if (!offsetlen)
		{
			result[0] = '\0';
			return -1;
		}
		len += offsetlen;
	}
	result[len++] = ',';
	c = stringrule(result + len, dstrp, dstrp->r_stdoff, zp->z_gmtoff);
	if (c < 0)
	{
		result[0] = '\0';
		return -1;
	}
	if (compat < c)
		compat = c;
	len += strlen(result + len);
	result[len++] = ',';
	c = stringrule(result + len, stdrp, dstrp->r_stdoff, zp->z_gmtoff);
	if (c < 0)
	{
		result[0] = '\0';
		return -1;
	}
	if (compat < c)
		compat = c;
	return compat;
}

static void
outzone(const struct zone * zpfirst, int zonecount)
{
	const struct zone *zp;
	struct rule *rp;
	int			i,
				j;
	bool		usestart,
				useuntil;
	zic_t		starttime,
				untiltime;
	zic_t		gmtoff;
	zic_t		stdoff;
	zic_t		year;
	zic_t		startoff;
	bool		startttisstd;
	bool		startttisgmt;
	int			type;
	char	   *startbuf;
	char	   *ab;
	char	   *envvar;
	int			max_abbr_len;
	int			max_envvar_len;
	bool		prodstic;		/* all rules are min to max */
	int			compat;
	bool		do_extend;
	char		version;

	max_abbr_len = 2 + max_format_len + max_abbrvar_len;
	max_envvar_len = 2 * max_abbr_len + 5 * 9;
	startbuf = emalloc(max_abbr_len + 1);
	ab = emalloc(max_abbr_len + 1);
	envvar = emalloc(max_envvar_len + 1);
	INITIALIZE(untiltime);
	INITIALIZE(starttime);

	/*
	 * Now. . .finally. . .generate some useful data!
	 */
	timecnt = 0;
	typecnt = 0;
	charcnt = 0;
	prodstic = zonecount == 1;

	/*
	 * Thanks to Earl Chew for noting the need to unconditionally initialize
	 * startttisstd.
	 */
	startttisstd = false;
	startttisgmt = false;
	min_year = max_year = EPOCH_YEAR;
	if (leapseen)
	{
		updateminmax(leapminyear);
		updateminmax(leapmaxyear + (leapmaxyear < ZIC_MAX));
	}
	for (i = 0; i < zonecount; ++i)
	{
		zp = &zpfirst[i];
		if (i < zonecount - 1)
			updateminmax(zp->z_untilrule.r_loyear);
		for (j = 0; j < zp->z_nrules; ++j)
		{
			rp = &zp->z_rules[j];
			if (rp->r_lowasnum)
				updateminmax(rp->r_loyear);
			if (rp->r_hiwasnum)
				updateminmax(rp->r_hiyear);
			if (rp->r_lowasnum || rp->r_hiwasnum)
				prodstic = false;
		}
	}

	/*
	 * Generate lots of data if a rule can't cover all future times.
	 */
	compat = stringzone(envvar, zpfirst, zonecount);
	version = compat < 2013 ? ZIC_VERSION_PRE_2013 : ZIC_VERSION;
	do_extend = compat < 0 || compat == YEAR_BY_YEAR_ZONE;
	if (noise)
	{
		if (!*envvar)
			warning("%s %s",
					_("no POSIX environment variable for zone"),
					zpfirst->z_name);
		else if (compat != 0 && compat != YEAR_BY_YEAR_ZONE)
		{
			/*
			 * Circa-COMPAT clients, and earlier clients, might not work for
			 * this zone when given dates before 1970 or after 2038.
			 */
			warning(_("%s: pre-%d clients may mishandle"
					  " distant timestamps"),
					zpfirst->z_name, compat);
		}
	}
	if (do_extend)
	{
		/*
		 * Search through a couple of extra years past the obvious 400, to
		 * avoid edge cases.  For example, suppose a non-POSIX rule applies
		 * from 2012 onwards and has transitions in March and September, plus
		 * some one-off transitions in November 2013.  If zic looked only at
		 * the last 400 years, it would set max_year=2413, with the intent
		 * that the 400 years 2014 through 2413 will be repeated.  The last
		 * transition listed in the tzfile would be in 2413-09, less than 400
		 * years after the last one-off transition in 2013-11.  Two years
		 * might be overkill, but with the kind of edge cases available we're
		 * not sure that one year would suffice.
		 */
		enum
		{
		years_of_observations = YEARSPERREPEAT + 2};

		if (min_year >= ZIC_MIN + years_of_observations)
			min_year -= years_of_observations;
		else
			min_year = ZIC_MIN;
		if (max_year <= ZIC_MAX - years_of_observations)
			max_year += years_of_observations;
		else
			max_year = ZIC_MAX;

		/*
		 * Regardless of any of the above, for a "proDSTic" zone which
		 * specifies that its rules always have and always will be in effect,
		 * we only need one cycle to define the zone.
		 */
		if (prodstic)
		{
			min_year = 1900;
			max_year = min_year + years_of_observations;
		}
	}

	/*
	 * For the benefit of older systems, generate data from 1900 through 2037.
	 */
	if (min_year > 1900)
		min_year = 1900;
	if (max_year < 2037)
		max_year = 2037;
	for (i = 0; i < zonecount; ++i)
	{
		/*
		 * A guess that may well be corrected later.
		 */
		stdoff = 0;
		zp = &zpfirst[i];
		usestart = i > 0 && (zp - 1)->z_untiltime > big_bang_time;
		useuntil = i < (zonecount - 1);
		if (useuntil && zp->z_untiltime <= big_bang_time)
			continue;
		gmtoff = zp->z_gmtoff;
		eat(zp->z_filename, zp->z_linenum);
		*startbuf = '\0';
		startoff = zp->z_gmtoff;
		if (zp->z_nrules == 0)
		{
			stdoff = zp->z_stdoff;
			doabbr(startbuf, zp, NULL, stdoff, false);
			type = addtype(oadd(zp->z_gmtoff, stdoff),
						   startbuf, stdoff != 0, startttisstd,
						   startttisgmt);
			if (usestart)
			{
				addtt(starttime, type);
				usestart = false;
			}
			else
				addtt(big_bang_time, type);
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
					zic_t		jtime,
								ktime = 0;
					zic_t		offset;

					if (useuntil)
					{
						/*
						 * Turn untiltime into UT assuming the current gmtoff
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
						else if (jtime == ktime)
						{
							char const *dup_rules_msg =
							_("two rules for same instant");

							eats(zp->z_filename, zp->z_linenum,
								 rp->r_filename, rp->r_linenum);
							warning("%s", dup_rules_msg);
							rp = &zp->z_rules[k];
							eats(zp->z_filename, zp->z_linenum,
								 rp->r_filename, rp->r_linenum);
							error("%s", dup_rules_msg);
						}
					}
					if (k < 0)
						break;	/* go on to next year */
					rp = &zp->z_rules[k];
					rp->r_todo = false;
					if (useuntil && ktime >= untiltime)
						break;
					stdoff = rp->r_stdoff;
					if (usestart && ktime == starttime)
						usestart = false;
					if (usestart)
					{
						if (ktime < starttime)
						{
							startoff = oadd(zp->z_gmtoff,
											stdoff);
							doabbr(startbuf, zp,
								   rp->r_abbrvar,
								   rp->r_stdoff,
								   false);
							continue;
						}
						if (*startbuf == '\0' &&
							startoff == oadd(zp->z_gmtoff, stdoff))
						{
							doabbr(startbuf,
								   zp,
								   rp->r_abbrvar,
								   rp->r_stdoff,
								   false);
						}
					}
					eats(zp->z_filename, zp->z_linenum,
						 rp->r_filename, rp->r_linenum);
					doabbr(ab, zp, rp->r_abbrvar,
						   rp->r_stdoff, false);
					offset = oadd(zp->z_gmtoff, rp->r_stdoff);
					type = addtype(offset, ab, rp->r_stdoff != 0,
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
				strcpy(startbuf, zp->z_format);
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
	if (do_extend)
	{
		/*
		 * If we're extending the explicitly listed observations for 400 years
		 * because we can't fill the POSIX-TZ field, check whether we actually
		 * ended up explicitly listing observations through that period.  If
		 * there aren't any near the end of the 400-year period, add a
		 * redundant one at the end of the final year, to make it clear that
		 * we are claiming to have definite knowledge of the lack of
		 * transitions up to that point.
		 */
		struct rule xr;
		struct attype *lastat;

		xr.r_month = TM_JANUARY;
		xr.r_dycode = DC_DOM;
		xr.r_dayofmonth = 1;
		xr.r_tod = 0;
		for (lastat = &attypes[0], i = 1; i < timecnt; i++)
			if (attypes[i].at > lastat->at)
				lastat = &attypes[i];
		if (lastat->at < rpytime(&xr, max_year - 1))
		{
			/*
			 * Create new type code for the redundant entry, to prevent it
			 * being optimized away.
			 */
			if (typecnt >= TZ_MAX_TYPES)
			{
				error(_("too many local time types"));
				exit(EXIT_FAILURE);
			}
			gmtoffs[typecnt] = gmtoffs[lastat->type];
			isdsts[typecnt] = isdsts[lastat->type];
			ttisstds[typecnt] = ttisstds[lastat->type];
			ttisgmts[typecnt] = ttisgmts[lastat->type];
			abbrinds[typecnt] = abbrinds[lastat->type];
			++typecnt;
			addtt(rpytime(&xr, max_year + 1), typecnt - 1);
		}
	}
	writezone(zpfirst->z_name, envvar, version);
	free(startbuf);
	free(ab);
	free(envvar);
}

static void
addtt(zic_t starttime, int type)
{
	if (starttime <= big_bang_time ||
		(timecnt == 1 && attypes[0].at < big_bang_time))
	{
		gmtoffs[0] = gmtoffs[type];
		isdsts[0] = isdsts[type];
		ttisstds[0] = ttisstds[type];
		ttisgmts[0] = ttisgmts[type];
		if (abbrinds[type] != 0)
			strcpy(chars, &chars[abbrinds[type]]);
		abbrinds[0] = 0;
		charcnt = strlen(chars) + 1;
		typecnt = 1;
		timecnt = 0;
		type = 0;
	}
	attypes = growalloc(attypes, sizeof *attypes, timecnt, &timecnt_alloc);
	attypes[timecnt].at = starttime;
	attypes[timecnt].type = type;
	++timecnt;
}

static int
addtype(zic_t gmtoff, char const * abbr, bool isdst, bool ttisstd, bool ttisgmt)
{
	int			i,
				j;

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
		exit(EXIT_FAILURE);
	}
	if (!(-1L - 2147483647L <= gmtoff && gmtoff <= 2147483647L))
	{
		error(_("UT offset out of range"));
		exit(EXIT_FAILURE);
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
leapadd(zic_t t, bool positive, int rolling, int count)
{
	int			i,
				j;

	if (leapcnt + (positive ? count : 1) > TZ_MAX_LEAPS)
	{
		error(_("too many leap seconds"));
		exit(EXIT_FAILURE);
	}
	for (i = 0; i < leapcnt; ++i)
		if (t <= trans[i])
		{
			if (t == trans[i])
			{
				error(_("repeated leap second moment"));
				exit(EXIT_FAILURE);
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
		corr[i] = positive ? 1 : -count;
		roll[i] = rolling;
		++leapcnt;
	} while (positive && --count != 0);
}

static void
adjleap(void)
{
	int			i;
	zic_t		last = 0;

	/*
	 * propagate leap seconds forward
	 */
	for (i = 0; i < leapcnt; ++i)
	{
		trans[i] = tadd(trans[i], last);
		last = corr[i] += last;
	}
}

static bool
yearistype(int year, const char *type)
{
	static char *buf;
	int			result;

	if (type == NULL || *type == '\0')
		return true;
	buf = erealloc(buf, 132 + strlen(yitcommand) + strlen(type));
	sprintf(buf, "%s %d %s", yitcommand, year, type);
	result = system(buf);
	if (WIFEXITED(result))
		switch (WEXITSTATUS(result))
		{
			case 0:
				return true;
			case 1:
				return false;
		}
	error(_("Wild result from command execution"));
	fprintf(stderr, _("%s: command was '%s', result was %d\n"),
			progname, buf, result);
	for (;;)
		exit(EXIT_FAILURE);
}

/* Is A a space character in the C locale?	*/
static bool
is_space(char a)
{
	switch (a)
	{
		default:
			return false;
		case ' ':
		case '\f':
		case '\n':
		case '\r':
		case '\t':
		case '\v':
			return true;
	}
}

/* Is A an alphabetic character in the C locale?  */
static bool
is_alpha(char a)
{
	switch (a)
	{
		default:
			return false;
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'G':
		case 'H':
		case 'I':
		case 'J':
		case 'K':
		case 'L':
		case 'M':
		case 'N':
		case 'O':
		case 'P':
		case 'Q':
		case 'R':
		case 'S':
		case 'T':
		case 'U':
		case 'V':
		case 'W':
		case 'X':
		case 'Y':
		case 'Z':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
		case 'g':
		case 'h':
		case 'i':
		case 'j':
		case 'k':
		case 'l':
		case 'm':
		case 'n':
		case 'o':
		case 'p':
		case 'q':
		case 'r':
		case 's':
		case 't':
		case 'u':
		case 'v':
		case 'w':
		case 'x':
		case 'y':
		case 'z':
			return true;
	}
}

/* If A is an uppercase character in the C locale, return its lowercase
 * counterpart.  Otherwise, return A.  */
static char
lowerit(char a)
{
	switch (a)
	{
		default:
			return a;
		case 'A':
			return 'a';
		case 'B':
			return 'b';
		case 'C':
			return 'c';
		case 'D':
			return 'd';
		case 'E':
			return 'e';
		case 'F':
			return 'f';
		case 'G':
			return 'g';
		case 'H':
			return 'h';
		case 'I':
			return 'i';
		case 'J':
			return 'j';
		case 'K':
			return 'k';
		case 'L':
			return 'l';
		case 'M':
			return 'm';
		case 'N':
			return 'n';
		case 'O':
			return 'o';
		case 'P':
			return 'p';
		case 'Q':
			return 'q';
		case 'R':
			return 'r';
		case 'S':
			return 's';
		case 'T':
			return 't';
		case 'U':
			return 'u';
		case 'V':
			return 'v';
		case 'W':
			return 'w';
		case 'X':
			return 'x';
		case 'Y':
			return 'y';
		case 'Z':
			return 'z';
	}
}

/* case-insensitive equality */
static bool
ciequal(const char *ap, const char *bp)
{
	while (lowerit(*ap) == lowerit(*bp++))
		if (*ap++ == '\0')
			return true;
	return false;
}

static bool
itsabbr(const char *abbr, const char *word)
{
	if (lowerit(*abbr) != lowerit(*word))
		return false;
	++word;
	while (*++abbr != '\0')
		do
		{
			if (*word == '\0')
				return false;
		} while (lowerit(*word++) != lowerit(*abbr));
	return true;
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
	array = emalloc(size_product(strlen(cp) + 1, sizeof *array));
	nsubs = 0;
	for (;;)
	{
		while (is_space(*cp))
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
					{
						error(_("Odd number of quotation marks"));
						exit(1);
					}
		} while (*cp && *cp != '#' && !is_space(*cp));
		if (is_space(*cp))
			++cp;
		*dp = '\0';
	}
	array[nsubs] = NULL;
	return array;
}

static void
time_overflow(void)
{
	error(_("time overflow"));
	exit(EXIT_FAILURE);
}

static zic_t
oadd(zic_t t1, zic_t t2)
{
	if (t1 < 0 ? t2 < ZIC_MIN - t1 : ZIC_MAX - t1 < t2)
		time_overflow();
	return t1 + t2;
}

static zic_t
tadd(zic_t t1, zic_t t2)
{
	if (t1 < 0)
	{
		if (t2 < min_time - t1)
		{
			if (t1 != min_time)
				time_overflow();
			return min_time;
		}
	}
	else
	{
		if (max_time - t1 < t2)
		{
			if (t1 != max_time)
				time_overflow();
			return max_time;
		}
	}
	return t1 + t2;
}

/*
 * Given a rule, and a year, compute the date (in seconds since January 1,
 * 1970, 00:00 LOCAL time) in that year that the rule refers to.
 */

static zic_t
rpytime(const struct rule * rp, zic_t wantedy)
{
	int			m,
				i;
	zic_t		dayoff;			/* with a nod to Margaret O. */
	zic_t		t,
				y;

	if (wantedy == ZIC_MIN)
		return min_time;
	if (wantedy == ZIC_MAX)
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
		dayoff = oadd(dayoff, i);
	}
	while (m != rp->r_month)
	{
		i = len_months[isleap(y)][m];
		dayoff = oadd(dayoff, i);
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
			exit(EXIT_FAILURE);
		}
	}
	--i;
	dayoff = oadd(dayoff, i);
	if (rp->r_dycode == DC_DOWGEQ || rp->r_dycode == DC_DOWLEQ)
	{
		zic_t		wday;

#define LDAYSPERWEEK	((zic_t) DAYSPERWEEK)
		wday = EPOCH_WDAY;

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
		while (wday != rp->r_wday)
			if (rp->r_dycode == DC_DOWGEQ)
			{
				dayoff = oadd(dayoff, 1);
				if (++wday >= LDAYSPERWEEK)
					wday = 0;
				++i;
			}
			else
			{
				dayoff = oadd(dayoff, -1);
				if (--wday < 0)
					wday = LDAYSPERWEEK - 1;
				--i;
			}
		if (i < 0 || i >= len_months[isleap(y)][m])
		{
			if (noise)
				warning(_("rule goes past start/end of month; \
will not work with pre-2004 versions of zic"));
		}
	}
	if (dayoff < min_time / SECSPERDAY)
		return min_time;
	if (dayoff > max_time / SECSPERDAY)
		return max_time;
	t = (zic_t) dayoff *SECSPERDAY;

	return tadd(t, rp->r_tod);
}

static void
newabbr(const char *string)
{
	int			i;

	if (strcmp(string, GRANDPARENTED) != 0)
	{
		const char *cp;
		const char *mp;

		cp = string;
		mp = NULL;
		while (is_alpha(*cp) || ('0' <= *cp && *cp <= '9')
			   || *cp == '-' || *cp == '+')
			++cp;
		if (noise && cp - string < 3)
			mp = _("time zone abbreviation has fewer than 3 characters");
		if (cp - string > ZIC_MAX_ABBR_LEN_WO_WARN)
			mp = _("time zone abbreviation has too many characters");
		if (*cp != '\0')
			mp = _("time zone abbreviation differs from POSIX standard");
		if (mp != NULL)
			warning("%s (%s)", mp, string);
	}
	i = strlen(string) + 1;
	if (charcnt + i > TZ_MAX_CHARS)
	{
		error(_("too many, or too long, time zone abbreviations"));
		exit(EXIT_FAILURE);
	}
	strcpy(&chars[charcnt], string);
	charcnt += i;
}

static bool
mkdirs(char *argname)
{
	char	   *name;
	char	   *cp;

	if (argname == NULL || *argname == '\0')
		return true;
	cp = name = ecpyalloc(argname);
	while ((cp = strchr(cp + 1, '/')) != NULL)
	{
		*cp = '\0';
#ifdef WIN32

		/*
		 * DOS drive specifier?
		 */
		if (is_alpha(name[0]) && name[1] == ':' && name[2] == '\0')
		{
			*cp = '/';
			continue;
		}
#endif   /* WIN32 */

		/*
		 * Try to create it.  It's OK if creation fails because the directory
		 * already exists, perhaps because some other process just created it.
		 */
		if (mkdir(name, MKDIR_UMASK) != 0)
		{
			int			err = errno;

			if (itsdir(name) <= 0)
			{
				char const *e = strerror(err);

				warning(_("%s: Can't create directory"
						  " %s: %s"),
						progname, name, e);
				free(name);
				return false;
			}
		}
		*cp = '/';
	}
	free(name);
	return true;
}


#ifdef WIN32
/*
 * To run on win32
 */
int
link(const char *oldpath, const char *newpath)
{
	if (!CopyFile(oldpath, newpath, false))
	{
		_dosmaperr(GetLastError());
		return -1;
	}
	return 0;
}
#endif
