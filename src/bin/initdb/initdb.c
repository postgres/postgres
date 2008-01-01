/*-------------------------------------------------------------------------
 *
 * initdb --- initialize a PostgreSQL installation
 *
 * initdb creates (initializes) a PostgreSQL database cluster (site,
 * instance, installation, whatever).  A database cluster is a
 * collection of PostgreSQL databases all managed by the same server.
 *
 * To create the database cluster, we create the directory that contains
 * all its data, create the files that hold the global tables, create
 * a few other control files for it, and create three databases: the
 * template databases "template0" and "template1", and a default user
 * database "postgres".
 *
 * The template databases are ordinary PostgreSQL databases.  template0
 * is never supposed to change after initdb, whereas template1 can be
 * changed to add site-local standard data.  Either one can be copied
 * to produce a new database.
 *
 * For largely-historical reasons, the template1 database is the one built
 * by the basic bootstrap process.	After it is complete, template0 and
 * the default database, postgres, are made just by copying template1.
 *
 * To create template1, we run the postgres (backend) program in bootstrap
 * mode and feed it data from the postgres.bki library file.  After this
 * initial bootstrap phase, some additional stuff is created by normal
 * SQL commands fed to a standalone backend.  Some of those commands are
 * just embedded into this program (yeah, it's ugly), but larger chunks
 * are taken from script files.
 *
 *
 * Note:
 *	 The program has some memory leakage - it isn't worth cleaning it up.
 *
 * This is a C implementation of the previous shell script for setting up a
 * PostgreSQL cluster location, and should be highly compatible with it.
 * author of C translation: Andrew Dunstan	   mailto:andrew@dunslane.net
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions taken from FreeBSD.
 *
 * $PostgreSQL: pgsql/src/bin/initdb/initdb.c,v 1.152 2008/01/01 19:45:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <time.h>

#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "getaddrinfo.h"
#include "getopt_long.h"
#include "miscadmin.h"

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif


/* version string we expect back from postgres */
#define PG_VERSIONSTR "postgres (PostgreSQL) " PG_VERSION "\n"

/*
 * these values are passed in by makefile defines
 */
static char *share_path = NULL;

/* values to be obtained from arguments */
static char *pg_data = "";
static char *encoding = "";
static char *locale = "";
static char *lc_collate = "";
static char *lc_ctype = "";
static char *lc_monetary = "";
static char *lc_numeric = "";
static char *lc_time = "";
static char *lc_messages = "";
static const char *default_text_search_config = "";
static char *username = "";
static bool pwprompt = false;
static char *pwfilename = NULL;
static char *authmethod = "";
static bool debug = false;
static bool noclean = false;
static bool show_setting = false;
static char *xlog_dir = "";


/* internal vars */
static const char *progname;
static char *encodingid = "0";
static char *bki_file;
static char *desc_file;
static char *shdesc_file;
static char *hba_file;
static char *ident_file;
static char *conf_file;
static char *conversion_file;
static char *dictionary_file;
static char *info_schema_file;
static char *features_file;
static char *system_views_file;
static bool made_new_pgdata = false;
static bool found_existing_pgdata = false;
static bool made_new_xlogdir = false;
static bool found_existing_xlogdir = false;
static char infoversion[100];
static bool caught_signal = false;
static bool output_failed = false;
static int	output_errno = 0;

/* defaults */
static int	n_connections = 10;
static int	n_buffers = 50;
static int	n_fsm_pages = 20000;

/*
 * Warning messages for authentication methods
 */
#define AUTHTRUST_WARNING \
"# CAUTION: Configuring the system for local \"trust\" authentication allows\n" \
"# any local user to connect as any PostgreSQL user, including the database\n" \
"# superuser. If you do not trust all your local users, use another\n" \
"# authentication method.\n"
static char *authwarning = NULL;

/*
 * Centralized knowledge of switches to pass to backend
 *
 * Note: in the shell-script version, we also passed PGDATA as a -D switch,
 * but here it is more convenient to pass it as an environment variable
 * (no quoting to worry about).
 */
static const char *boot_options = "-F";
static const char *backend_options = "--single -F -O -c search_path=pg_catalog -c exit_on_error=true";


/* path to 'initdb' binary directory */
static char bin_path[MAXPGPATH];
static char backend_exec[MAXPGPATH];

static void *pg_malloc(size_t size);
static char *xstrdup(const char *s);
static char **replace_token(char **lines,
			  const char *token, const char *replacement);

#ifndef HAVE_UNIX_SOCKETS
static char **filter_lines_with_token(char **lines, const char *token);
#endif
static char **readfile(char *path);
static void writefile(char *path, char **lines);
static FILE *popen_check(const char *command, const char *mode);
static int	mkdir_p(char *path, mode_t omode);
static void exit_nicely(void);
static char *get_id(void);
static char *get_encoding_id(char *encoding_name);
static char *get_short_version(void);
static int	check_data_dir(char *dir);
static bool mkdatadir(const char *subdir);
static void set_input(char **dest, char *filename);
static void check_input(char *path);
static void set_short_version(char *short_version, char *extrapath);
static void set_null_conf(void);
static void test_config_settings(void);
static void setup_config(void);
static void bootstrap_template1(char *short_version);
static void setup_auth(void);
static void get_set_pwd(void);
static void setup_depend(void);
static void setup_sysviews(void);
static void setup_description(void);
static void setup_conversion(void);
static void setup_dictionary(void);
static void setup_privileges(void);
static void set_info_version(void);
static void setup_schema(void);
static void vacuum_db(void);
static void make_template0(void);
static void make_postgres(void);
static void trapsig(int signum);
static void check_ok(void);
static char *escape_quotes(const char *src);
static int	locale_date_order(const char *locale);
static bool chklocale(const char *locale);
static void setlocales(void);
static void usage(const char *progname);

#ifdef WIN32
static int	CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION * processInfo);
#endif


/*
 * macros for running pipes to postgres
 */
#define PG_CMD_DECL		char cmd[MAXPGPATH]; FILE *cmdfd

#define PG_CMD_OPEN \
do { \
	cmdfd = popen_check(cmd, "w"); \
	if (cmdfd == NULL) \
		exit_nicely(); /* message already printed by popen_check */ \
} while (0)

#define PG_CMD_CLOSE \
do { \
	if (pclose_check(cmdfd)) \
		exit_nicely(); /* message already printed by pclose_check */ \
} while (0)

#define PG_CMD_PUTS(line) \
do { \
	if (fputs(line, cmdfd) < 0 || fflush(cmdfd) < 0) \
		output_failed = true, output_errno = errno; \
} while (0)

#define PG_CMD_PRINTF1(fmt, arg1) \
do { \
	if (fprintf(cmdfd, fmt, arg1) < 0 || fflush(cmdfd) < 0) \
		output_failed = true, output_errno = errno; \
} while (0)

#define PG_CMD_PRINTF2(fmt, arg1, arg2) \
do { \
	if (fprintf(cmdfd, fmt, arg1, arg2) < 0 || fflush(cmdfd) < 0) \
		output_failed = true, output_errno = errno; \
} while (0)

#ifndef WIN32
#define QUOTE_PATH	""
#define DIR_SEP "/"
#else
#define QUOTE_PATH	"\""
#define DIR_SEP "\\"
#endif

/*
 * routines to check mem allocations and fail noisily.
 *
 * Note that we can't call exit_nicely() on a memory failure, as it calls
 * rmtree() which needs memory allocation. So we just exit with a bang.
 */
static void *
pg_malloc(size_t size)
{
	void	   *result;

	result = malloc(size);
	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}

static char *
xstrdup(const char *s)
{
	char	   *result;

	result = strdup(s);
	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}

/*
 * make a copy of the array of lines, with token replaced by replacement
 * the first time it occurs on each line.
 *
 * This does most of what sed was used for in the shell script, but
 * doesn't need any regexp stuff.
 */
static char **
replace_token(char **lines, const char *token, const char *replacement)
{
	int			numlines = 1;
	int			i;
	char	  **result;
	int			toklen,
				replen,
				diff;

	for (i = 0; lines[i]; i++)
		numlines++;

	result = (char **) pg_malloc(numlines * sizeof(char *));

	toklen = strlen(token);
	replen = strlen(replacement);
	diff = replen - toklen;

	for (i = 0; i < numlines; i++)
	{
		char	   *where;
		char	   *newline;
		int			pre;

		/* just copy pointer if NULL or no change needed */
		if (lines[i] == NULL || (where = strstr(lines[i], token)) == NULL)
		{
			result[i] = lines[i];
			continue;
		}

		/* if we get here a change is needed - set up new line */

		newline = (char *) pg_malloc(strlen(lines[i]) + diff + 1);

		pre = where - lines[i];

		strncpy(newline, lines[i], pre);

		strcpy(newline + pre, replacement);

		strcpy(newline + pre + replen, lines[i] + pre + toklen);

		result[i] = newline;
	}

	return result;
}

/*
 * make a copy of lines without any that contain the token
 *
 * a sort of poor man's grep -v
 */
#ifndef HAVE_UNIX_SOCKETS
static char **
filter_lines_with_token(char **lines, const char *token)
{
	int			numlines = 1;
	int			i,
				src,
				dst;
	char	  **result;

	for (i = 0; lines[i]; i++)
		numlines++;

	result = (char **) pg_malloc(numlines * sizeof(char *));

	for (src = 0, dst = 0; src < numlines; src++)
	{
		if (lines[src] == NULL || strstr(lines[src], token) == NULL)
			result[dst++] = lines[src];
	}

	return result;
}
#endif

/*
 * get the lines from a text file
 */
static char **
readfile(char *path)
{
	FILE	   *infile;
	int			maxlength = 0,
				linelen = 0;
	int			nlines = 0;
	char	  **result;
	char	   *buffer;
	int			c;

	if ((infile = fopen(path, "r")) == NULL)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}

	/* pass over the file twice - the first time to size the result */

	while ((c = fgetc(infile)) != EOF)
	{
		linelen++;
		if (c == '\n')
		{
			nlines++;
			if (linelen > maxlength)
				maxlength = linelen;
			linelen = 0;
		}
	}

	/* handle last line without a terminating newline (yuck) */

	if (linelen)
		nlines++;
	if (linelen > maxlength)
		maxlength = linelen;

	/* set up the result and the line buffer */

	result = (char **) pg_malloc((nlines + 2) * sizeof(char *));
	buffer = (char *) pg_malloc(maxlength + 2);

	/* now reprocess the file and store the lines */

	rewind(infile);
	nlines = 0;
	while (fgets(buffer, maxlength + 1, infile) != NULL)
	{
		result[nlines] = xstrdup(buffer);
		nlines++;
	}

	fclose(infile);
	free(buffer);
	result[nlines] = NULL;

	return result;
}

/*
 * write an array of lines to a file
 *
 * This is only used to write text files.  Use fopen "w" not PG_BINARY_W
 * so that the resulting configuration files are nicely editable on Windows.
 */
static void
writefile(char *path, char **lines)
{
	FILE	   *out_file;
	char	  **line;

	if ((out_file = fopen(path, "w")) == NULL)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
	for (line = lines; *line != NULL; line++)
	{
		if (fputs(*line, out_file) < 0)
		{
			fprintf(stderr, _("%s: could not write file \"%s\": %s\n"),
					progname, path, strerror(errno));
			exit_nicely();
		}
		free(*line);
	}
	if (fclose(out_file))
	{
		fprintf(stderr, _("%s: could not write file \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
}

/*
 * Open a subcommand with suitable error messaging
 */
static FILE *
popen_check(const char *command, const char *mode)
{
	FILE	   *cmdfd;

	fflush(stdout);
	fflush(stderr);
	errno = 0;
	cmdfd = popen(command, mode);
	if (cmdfd == NULL)
		fprintf(stderr, _("%s: could not execute command \"%s\": %s\n"),
				progname, command, strerror(errno));
	return cmdfd;
}

/* source stolen from FreeBSD /src/bin/mkdir/mkdir.c and adapted */

/*
 * this tries to build all the elements of a path to a directory a la mkdir -p
 * we assume the path is in canonical form, i.e. uses / as the separator
 * we also assume it isn't null.
 *
 * note that on failure, the path arg has been modified to show the particular
 * directory level we had problems with.
 */
static int
mkdir_p(char *path, mode_t omode)
{
	struct stat sb;
	mode_t		numask,
				oumask;
	int			first,
				last,
				retval;
	char	   *p;

	p = path;
	oumask = 0;
	retval = 0;

#ifdef WIN32
	/* skip network and drive specifiers for win32 */
	if (strlen(p) >= 2)
	{
		if (p[0] == '/' && p[1] == '/')
		{
			/* network drive */
			p = strstr(p + 2, "/");
			if (p == NULL)
				return 1;
		}
		else if (p[1] == ':' &&
				 ((p[0] >= 'a' && p[0] <= 'z') ||
				  (p[0] >= 'A' && p[0] <= 'Z')))
		{
			/* local drive */
			p += 2;
		}
	}
#endif

	if (p[0] == '/')			/* Skip leading '/'. */
		++p;
	for (first = 1, last = 0; !last; ++p)
	{
		if (p[0] == '\0')
			last = 1;
		else if (p[0] != '/')
			continue;
		*p = '\0';
		if (!last && p[1] == '\0')
			last = 1;
		if (first)
		{
			/*
			 * POSIX 1003.2: For each dir operand that does not name an
			 * existing directory, effects equivalent to those caused by the
			 * following command shall occcur:
			 *
			 * mkdir -p -m $(umask -S),u+wx $(dirname dir) && mkdir [-m mode]
			 * dir
			 *
			 * We change the user's umask and then restore it, instead of
			 * doing chmod's.
			 */
			oumask = umask(0);
			numask = oumask & ~(S_IWUSR | S_IXUSR);
			(void) umask(numask);
			first = 0;
		}
		if (last)
			(void) umask(oumask);

		/* check for pre-existing directory; ok if it's a parent */
		if (stat(path, &sb) == 0)
		{
			if (!S_ISDIR(sb.st_mode))
			{
				if (last)
					errno = EEXIST;
				else
					errno = ENOTDIR;
				retval = 1;
				break;
			}
		}
		else if (mkdir(path, last ? omode : S_IRWXU | S_IRWXG | S_IRWXO) < 0)
		{
			retval = 1;
			break;
		}
		if (!last)
			*p = '/';
	}
	if (!first && !last)
		(void) umask(oumask);
	return retval;
}

/*
 * clean up any files we created on failure
 * if we created the data directory remove it too
 */
static void
exit_nicely(void)
{
	if (!noclean)
	{
		if (made_new_pgdata)
		{
			fprintf(stderr, _("%s: removing data directory \"%s\"\n"),
					progname, pg_data);
			if (!rmtree(pg_data, true))
				fprintf(stderr, _("%s: failed to remove data directory\n"),
						progname);
		}
		else if (found_existing_pgdata)
		{
			fprintf(stderr,
					_("%s: removing contents of data directory \"%s\"\n"),
					progname, pg_data);
			if (!rmtree(pg_data, false))
				fprintf(stderr, _("%s: failed to remove contents of data directory\n"),
						progname);
		}

		if (made_new_xlogdir)
		{
			fprintf(stderr, _("%s: removing transaction log directory \"%s\"\n"),
					progname, xlog_dir);
			if (!rmtree(xlog_dir, true))
				fprintf(stderr, _("%s: failed to remove transaction log directory\n"),
						progname);
		}
		else if (found_existing_xlogdir)
		{
			fprintf(stderr,
			_("%s: removing contents of transaction log directory \"%s\"\n"),
					progname, xlog_dir);
			if (!rmtree(xlog_dir, false))
				fprintf(stderr, _("%s: failed to remove contents of transaction log directory\n"),
						progname);
		}
		/* otherwise died during startup, do nothing! */
	}
	else
	{
		if (made_new_pgdata || found_existing_pgdata)
			fprintf(stderr,
			  _("%s: data directory \"%s\" not removed at user's request\n"),
					progname, pg_data);

		if (made_new_xlogdir || found_existing_xlogdir)
			fprintf(stderr,
					_("%s: transaction log directory \"%s\" not removed at user's request\n"),
					progname, xlog_dir);
	}

	exit(1);
}

/*
 * find the current user
 *
 * on unix make sure it isn't really root
 */
static char *
get_id(void)
{
#ifndef WIN32

	struct passwd *pw;

	pw = getpwuid(geteuid());

	if (geteuid() == 0)			/* 0 is root's uid */
	{
		fprintf(stderr,
				_("%s: cannot be run as root\n"
				  "Please log in (using, e.g., \"su\") as the "
				  "(unprivileged) user that will\n"
				  "own the server process.\n"),
				progname);
		exit(1);
	}
#else							/* the windows code */

	struct passwd_win32
	{
		int			pw_uid;
		char		pw_name[128];
	}			pass_win32;
	struct passwd_win32 *pw = &pass_win32;
	DWORD		pwname_size = sizeof(pass_win32.pw_name) - 1;

	pw->pw_uid = 1;
	GetUserName(pw->pw_name, &pwname_size);
#endif

	return xstrdup(pw->pw_name);
}

static char *
encodingid_to_string(int enc)
{
	char		result[20];

	sprintf(result, "%d", enc);
	return xstrdup(result);
}

/*
 * get the encoding id for a given encoding name
 */
static char *
get_encoding_id(char *encoding_name)
{
	int			enc;

	if (encoding_name && *encoding_name)
	{
		if ((enc = pg_valid_server_encoding(encoding_name)) >= 0)
			return encodingid_to_string(enc);
	}
	fprintf(stderr, _("%s: \"%s\" is not a valid server encoding name\n"),
			progname, encoding_name ? encoding_name : "(null)");
	exit(1);
}

/*
 * Support for determining the best default text search configuration.
 * We key this off the first part of LC_CTYPE (ie, the language name).
 */
struct tsearch_config_match
{
	const char *tsconfname;
	const char *langname;
};

static const struct tsearch_config_match tsearch_config_languages[] =
{
	{"danish", "da"},
	{"danish", "Danish"},
	{"dutch", "nl"},
	{"dutch", "Dutch"},
	{"english", "C"},
	{"english", "POSIX"},
	{"english", "en"},
	{"english", "English"},
	{"finnish", "fi"},
	{"finnish", "Finnish"},
	{"french", "fr"},
	{"french", "French"},
	{"german", "de"},
	{"german", "German"},
	{"hungarian", "hu"},
	{"hungarian", "Hungarian"},
	{"italian", "it"},
	{"italian", "Italian"},
	{"norwegian", "no"},
	{"norwegian", "Norwegian"},
	{"portuguese", "pt"},
	{"portuguese", "Portuguese"},
	{"romanian", "ro"},
	{"russian", "ru"},
	{"russian", "Russian"},
	{"spanish", "es"},
	{"spanish", "Spanish"},
	{"swedish", "sv"},
	{"swedish", "Swedish"},
	{"turkish", "tr"},
	{"turkish", "Turkish"},
	{NULL, NULL}				/* end marker */
};

/*
 * Look for a text search configuration matching lc_ctype, and return its
 * name; return NULL if no match.
 */
static const char *
find_matching_ts_config(const char *lc_type)
{
	int			i;
	char	   *langname,
			   *ptr;

	/*
	 * Convert lc_ctype to a language name by stripping everything after an
	 * underscore.	Just for paranoia, we also stop at '.' or '@'.
	 */
	if (lc_type == NULL)
		langname = xstrdup("");
	else
	{
		ptr = langname = xstrdup(lc_type);
		while (*ptr && *ptr != '_' && *ptr != '.' && *ptr != '@')
			ptr++;
		*ptr = '\0';
	}

	for (i = 0; tsearch_config_languages[i].tsconfname; i++)
	{
		if (pg_strcasecmp(tsearch_config_languages[i].langname, langname) == 0)
		{
			free(langname);
			return tsearch_config_languages[i].tsconfname;
		}
	}

	free(langname);
	return NULL;
}


/*
 * get short version of VERSION
 */
static char *
get_short_version(void)
{
	bool		gotdot = false;
	int			end;
	char	   *vr;

	vr = xstrdup(PG_VERSION);

	for (end = 0; vr[end] != '\0'; end++)
	{
		if (vr[end] == '.')
		{
			if (end == 0)
				return NULL;
			else if (gotdot)
				break;
			else
				gotdot = true;
		}
		else if (vr[end] < '0' || vr[end] > '9')
		{
			/* gone past digits and dots */
			break;
		}
	}
	if (end == 0 || vr[end - 1] == '.' || !gotdot)
		return NULL;

	vr[end] = '\0';
	return vr;
}

/*
 * make sure the directory either doesn't exist or is empty
 *
 * Returns 0 if nonexistent, 1 if exists and empty, 2 if not empty,
 * or -1 if trouble accessing directory
 */
static int
check_data_dir(char *dir)
{
	DIR		   *chkdir;
	struct dirent *file;
	int			result = 1;

	errno = 0;

	chkdir = opendir(dir);

	if (!chkdir)
		return (errno == ENOENT) ? 0 : -1;

	while ((file = readdir(chkdir)) != NULL)
	{
		if (strcmp(".", file->d_name) == 0 ||
			strcmp("..", file->d_name) == 0)
		{
			/* skip this and parent directory */
			continue;
		}
		else
		{
			result = 2;			/* not empty */
			break;
		}
	}

#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but not in
	 * released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	closedir(chkdir);

	if (errno != 0)
		result = -1;			/* some kind of I/O error? */

	return result;
}

/*
 * make the data directory (or one of its subdirectories if subdir is not NULL)
 */
static bool
mkdatadir(const char *subdir)
{
	char	   *path;

	path = pg_malloc(strlen(pg_data) + 2 +
					 (subdir == NULL ? 0 : strlen(subdir)));

	if (subdir != NULL)
		sprintf(path, "%s/%s", pg_data, subdir);
	else
		strcpy(path, pg_data);

	if (mkdir_p(path, 0700) == 0)
		return true;

	fprintf(stderr, _("%s: could not create directory \"%s\": %s\n"),
			progname, path, strerror(errno));

	return false;
}


/*
 * set name of given input file variable under data directory
 */
static void
set_input(char **dest, char *filename)
{
	*dest = pg_malloc(strlen(share_path) + strlen(filename) + 2);
	sprintf(*dest, "%s/%s", share_path, filename);
}

/*
 * check that given input file exists
 */
static void
check_input(char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) != 0)
	{
		if (errno == ENOENT)
		{
			fprintf(stderr,
					_("%s: file \"%s\" does not exist\n"), progname, path);
			fprintf(stderr,
					_("This might mean you have a corrupted installation or identified\n"
					  "the wrong directory with the invocation option -L.\n"));
		}
		else
		{
			fprintf(stderr,
					_("%s: could not access file \"%s\": %s\n"), progname, path,
					  strerror(errno));
			fprintf(stderr,
					_("This might mean you have a corrupted installation or identified\n"
					  "the wrong directory with the invocation option -L.\n"));
		}
		exit(1);
	}
	if (!S_ISREG(statbuf.st_mode))
	{
		fprintf(stderr,
				_("%s: file \"%s\" is not a regular file\n"), progname, path);
		fprintf(stderr,
				_("This might mean you have a corrupted installation or identified\n"
				  "the wrong directory with the invocation option -L.\n"));
		exit(1);
	}
}

/*
 * write out the PG_VERSION file in the data dir, or its subdirectory
 * if extrapath is not NULL
 */
static void
set_short_version(char *short_version, char *extrapath)
{
	FILE	   *version_file;
	char	   *path;

	if (extrapath == NULL)
	{
		path = pg_malloc(strlen(pg_data) + 12);
		sprintf(path, "%s/PG_VERSION", pg_data);
	}
	else
	{
		path = pg_malloc(strlen(pg_data) + strlen(extrapath) + 13);
		sprintf(path, "%s/%s/PG_VERSION", pg_data, extrapath);
	}
	version_file = fopen(path, PG_BINARY_W);
	if (version_file == NULL)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
	if (fprintf(version_file, "%s\n", short_version) < 0 ||
		fclose(version_file))
	{
		fprintf(stderr, _("%s: could not write file \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
	free(path);
}

/*
 * set up an empty config file so we can check config settings by launching
 * a test backend
 */
static void
set_null_conf(void)
{
	FILE	   *conf_file;
	char	   *path;

	path = pg_malloc(strlen(pg_data) + 17);
	sprintf(path, "%s/postgresql.conf", pg_data);
	conf_file = fopen(path, PG_BINARY_W);
	if (conf_file == NULL)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
	if (fclose(conf_file))
	{
		fprintf(stderr, _("%s: could not write file \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
	free(path);
}

/*
 * Determine platform-specific config settings
 *
 * Use reasonable values if kernel will let us, else scale back.  Probe
 * for max_connections first since it is subject to more constraints than
 * shared_buffers.
 */
static void
test_config_settings(void)
{
	/*
	 * These macros define the minimum shared_buffers we want for a given
	 * max_connections value, and the max_fsm_pages setting to be used for a
	 * given shared_buffers value.	The arrays show the settings to try.
	 */

#define MIN_BUFS_FOR_CONNS(nconns)	((nconns) * 10)
#define FSM_FOR_BUFS(nbuffers)	((nbuffers) > 1000 ? 50 * (nbuffers) : 20000)

	static const int trial_conns[] = {
		100, 50, 40, 30, 20, 10
	};
	static const int trial_bufs[] = {
		4096, 3584, 3072, 2560, 2048, 1536,
		1000, 900, 800, 700, 600, 500,
		400, 300, 200, 100, 50
	};

	char		cmd[MAXPGPATH];
	const int	connslen = sizeof(trial_conns) / sizeof(int);
	const int	bufslen = sizeof(trial_bufs) / sizeof(int);
	int			i,
				status,
				test_conns,
				test_buffs,
				test_max_fsm,
				ok_buffers = 0;


	printf(_("selecting default max_connections ... "));
	fflush(stdout);

	for (i = 0; i < connslen; i++)
	{
		test_conns = trial_conns[i];
		test_buffs = MIN_BUFS_FOR_CONNS(test_conns);
		test_max_fsm = FSM_FOR_BUFS(test_buffs);

		snprintf(cmd, sizeof(cmd),
				 "%s\"%s\" --boot -x0 %s "
				 "-c max_connections=%d "
				 "-c shared_buffers=%d "
				 "-c max_fsm_pages=%d "
				 "< \"%s\" > \"%s\" 2>&1%s",
				 SYSTEMQUOTE, backend_exec, boot_options,
				 test_conns, test_buffs, test_max_fsm,
				 DEVNULL, DEVNULL, SYSTEMQUOTE);
		status = system(cmd);
		if (status == 0)
		{
			ok_buffers = test_buffs;
			break;
		}
	}
	if (i >= connslen)
		i = connslen - 1;
	n_connections = trial_conns[i];

	printf("%d\n", n_connections);

	printf(_("selecting default shared_buffers/max_fsm_pages ... "));
	fflush(stdout);

	for (i = 0; i < bufslen; i++)
	{
		/* Use same amount of memory, independent of BLCKSZ */
		test_buffs = (trial_bufs[i] * 8192) / BLCKSZ;
		if (test_buffs <= ok_buffers)
		{
			test_buffs = ok_buffers;
			break;
		}
		test_max_fsm = FSM_FOR_BUFS(test_buffs);

		snprintf(cmd, sizeof(cmd),
				 "%s\"%s\" --boot -x0 %s "
				 "-c max_connections=%d "
				 "-c shared_buffers=%d "
				 "-c max_fsm_pages=%d "
				 "< \"%s\" > \"%s\" 2>&1%s",
				 SYSTEMQUOTE, backend_exec, boot_options,
				 n_connections, test_buffs, test_max_fsm,
				 DEVNULL, DEVNULL, SYSTEMQUOTE);
		status = system(cmd);
		if (status == 0)
			break;
	}
	n_buffers = test_buffs;
	n_fsm_pages = FSM_FOR_BUFS(n_buffers);

	if ((n_buffers * (BLCKSZ / 1024)) % 1024 == 0)
		printf("%dMB/%d\n", (n_buffers * (BLCKSZ / 1024)) / 1024, n_fsm_pages);
	else
		printf("%dkB/%d\n", n_buffers * (BLCKSZ / 1024), n_fsm_pages);
}

/*
 * set up all the config files
 */
static void
setup_config(void)
{
	char	  **conflines;
	char		repltok[100];
	char		path[MAXPGPATH];

	fputs(_("creating configuration files ... "), stdout);
	fflush(stdout);

	/* postgresql.conf */

	conflines = readfile(conf_file);

	snprintf(repltok, sizeof(repltok), "max_connections = %d", n_connections);
	conflines = replace_token(conflines, "#max_connections = 100", repltok);

	if ((n_buffers * (BLCKSZ / 1024)) % 1024 == 0)
		snprintf(repltok, sizeof(repltok), "shared_buffers = %dMB",
				 (n_buffers * (BLCKSZ / 1024)) / 1024);
	else
		snprintf(repltok, sizeof(repltok), "shared_buffers = %dkB",
				 n_buffers * (BLCKSZ / 1024));
	conflines = replace_token(conflines, "#shared_buffers = 32MB", repltok);

	snprintf(repltok, sizeof(repltok), "max_fsm_pages = %d", n_fsm_pages);
	conflines = replace_token(conflines, "#max_fsm_pages = 204800", repltok);

#if DEF_PGPORT != 5432
	snprintf(repltok, sizeof(repltok), "#port = %d", DEF_PGPORT);
	conflines = replace_token(conflines, "#port = 5432", repltok);
#endif

	snprintf(repltok, sizeof(repltok), "lc_messages = '%s'",
			 escape_quotes(lc_messages));
	conflines = replace_token(conflines, "#lc_messages = 'C'", repltok);

	snprintf(repltok, sizeof(repltok), "lc_monetary = '%s'",
			 escape_quotes(lc_monetary));
	conflines = replace_token(conflines, "#lc_monetary = 'C'", repltok);

	snprintf(repltok, sizeof(repltok), "lc_numeric = '%s'",
			 escape_quotes(lc_numeric));
	conflines = replace_token(conflines, "#lc_numeric = 'C'", repltok);

	snprintf(repltok, sizeof(repltok), "lc_time = '%s'",
			 escape_quotes(lc_time));
	conflines = replace_token(conflines, "#lc_time = 'C'", repltok);

	switch (locale_date_order(lc_time))
	{
		case DATEORDER_YMD:
			strcpy(repltok, "datestyle = 'iso, ymd'");
			break;
		case DATEORDER_DMY:
			strcpy(repltok, "datestyle = 'iso, dmy'");
			break;
		case DATEORDER_MDY:
		default:
			strcpy(repltok, "datestyle = 'iso, mdy'");
			break;
	}
	conflines = replace_token(conflines, "#datestyle = 'iso, mdy'", repltok);

	snprintf(repltok, sizeof(repltok),
			 "default_text_search_config = 'pg_catalog.%s'",
			 escape_quotes(default_text_search_config));
	conflines = replace_token(conflines,
						 "#default_text_search_config = 'pg_catalog.simple'",
							  repltok);

	snprintf(path, sizeof(path), "%s/postgresql.conf", pg_data);

	writefile(path, conflines);
	chmod(path, 0600);

	free(conflines);


	/* pg_hba.conf */

	conflines = readfile(hba_file);

#ifndef HAVE_UNIX_SOCKETS
	conflines = filter_lines_with_token(conflines, "@remove-line-for-nolocal@");
#else
	conflines = replace_token(conflines, "@remove-line-for-nolocal@", "");
#endif

#ifdef HAVE_IPV6

	/*
	 * Probe to see if there is really any platform support for IPv6, and
	 * comment out the relevant pg_hba line if not.  This avoids runtime
	 * warnings if getaddrinfo doesn't actually cope with IPv6.  Particularly
	 * useful on Windows, where executables built on a machine with IPv6 may
	 * have to run on a machine without.
	 */
	{
		struct addrinfo *gai_result;
		struct addrinfo hints;
		int			err = 0;

#ifdef WIN32
		/* need to call WSAStartup before calling getaddrinfo */
		WSADATA		wsaData;

		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		/* for best results, this code should match parse_hba() */
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = PF_UNSPEC;
		hints.ai_socktype = 0;
		hints.ai_protocol = 0;
		hints.ai_addrlen = 0;
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		if (err != 0 ||
			getaddrinfo("::1", NULL, &hints, &gai_result) != 0)
			conflines = replace_token(conflines,
									  "host    all         all         ::1",
									  "#host    all         all         ::1");
	}
#else							/* !HAVE_IPV6 */
	/* If we didn't compile IPV6 support at all, always comment it out */
	conflines = replace_token(conflines,
							  "host    all         all         ::1",
							  "#host    all         all         ::1");
#endif   /* HAVE_IPV6 */

	/* Replace default authentication methods */
	conflines = replace_token(conflines,
							  "@authmethod@",
							  authmethod);

	conflines = replace_token(conflines,
							  "@authcomment@",
					   strcmp(authmethod, "trust") ? "" : AUTHTRUST_WARNING);

	snprintf(path, sizeof(path), "%s/pg_hba.conf", pg_data);

	writefile(path, conflines);
	chmod(path, 0600);

	free(conflines);

	/* pg_ident.conf */

	conflines = readfile(ident_file);

	snprintf(path, sizeof(path), "%s/pg_ident.conf", pg_data);

	writefile(path, conflines);
	chmod(path, 0600);

	free(conflines);

	check_ok();
}


/*
 * run the BKI script in bootstrap mode to create template1
 */
static void
bootstrap_template1(char *short_version)
{
	PG_CMD_DECL;
	char	  **line;
	char	   *talkargs = "";
	char	  **bki_lines;
	char		headerline[MAXPGPATH];

	printf(_("creating template1 database in %s/base/1 ... "), pg_data);
	fflush(stdout);

	if (debug)
		talkargs = "-d 5";

	bki_lines = readfile(bki_file);

	/* Check that bki file appears to be of the right version */

	snprintf(headerline, sizeof(headerline), "# PostgreSQL %s\n",
			 short_version);

	if (strcmp(headerline, *bki_lines) != 0)
	{
		fprintf(stderr,
				_("%s: input file \"%s\" does not belong to PostgreSQL %s\n"
				  "Check your installation or specify the correct path "
				  "using the option -L.\n"),
				progname, bki_file, PG_VERSION);
		exit_nicely();
	}

	bki_lines = replace_token(bki_lines, "POSTGRES", username);

	bki_lines = replace_token(bki_lines, "ENCODING", encodingid);

	/*
	 * Pass correct LC_xxx environment to bootstrap.
	 *
	 * The shell script arranged to restore the LC settings afterwards, but
	 * there doesn't seem to be any compelling reason to do that.
	 */
	snprintf(cmd, sizeof(cmd), "LC_COLLATE=%s", lc_collate);
	putenv(xstrdup(cmd));

	snprintf(cmd, sizeof(cmd), "LC_CTYPE=%s", lc_ctype);
	putenv(xstrdup(cmd));

	unsetenv("LC_ALL");

	/* Also ensure backend isn't confused by this environment var: */
	unsetenv("PGCLIENTENCODING");

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" --boot -x1 %s %s",
			 backend_exec, boot_options, talkargs);

	PG_CMD_OPEN;

	for (line = bki_lines; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	PG_CMD_CLOSE;

	free(bki_lines);

	check_ok();
}

/*
 * set up the shadow password table
 */
static void
setup_auth(void)
{
	PG_CMD_DECL;
	const char **line;
	static const char *pg_authid_setup[] = {
		/*
		 * Create triggers to ensure manual updates to shared catalogs will be
		 * reflected into their "flat file" copies.
		 */
		"CREATE TRIGGER pg_sync_pg_database "
		"  AFTER INSERT OR UPDATE OR DELETE ON pg_database "
		"  FOR EACH STATEMENT EXECUTE PROCEDURE flatfile_update_trigger();\n",
		"CREATE TRIGGER pg_sync_pg_authid "
		"  AFTER INSERT OR UPDATE OR DELETE ON pg_authid "
		"  FOR EACH STATEMENT EXECUTE PROCEDURE flatfile_update_trigger();\n",
		"CREATE TRIGGER pg_sync_pg_auth_members "
		"  AFTER INSERT OR UPDATE OR DELETE ON pg_auth_members "
		"  FOR EACH STATEMENT EXECUTE PROCEDURE flatfile_update_trigger();\n",

		/*
		 * The authid table shouldn't be readable except through views, to
		 * ensure passwords are not publicly visible.
		 */
		"REVOKE ALL on pg_authid FROM public;\n",
		NULL
	};

	fputs(_("initializing pg_authid ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = pg_authid_setup; *line != NULL; line++)
		PG_CMD_PUTS(*line);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * get the superuser password if required, and call postgres to set it
 */
static void
get_set_pwd(void)
{
	PG_CMD_DECL;

	char	   *pwd1,
			   *pwd2;
	char		pwdpath[MAXPGPATH];
	struct stat statbuf;

	if (pwprompt)
	{
		/*
		 * Read password from terminal
		 */
		pwd1 = simple_prompt("Enter new superuser password: ", 100, false);
		pwd2 = simple_prompt("Enter it again: ", 100, false);
		if (strcmp(pwd1, pwd2) != 0)
		{
			fprintf(stderr, _("Passwords didn't match.\n"));
			exit_nicely();
		}
		free(pwd2);
	}
	else
	{
		/*
		 * Read password from file
		 *
		 * Ideally this should insist that the file not be world-readable.
		 * However, this option is mainly intended for use on Windows where
		 * file permissions may not exist at all, so we'll skip the paranoia
		 * for now.
		 */
		FILE	   *pwf = fopen(pwfilename, "r");
		char		pwdbuf[MAXPGPATH];
		int			i;

		if (!pwf)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
					progname, pwfilename, strerror(errno));
			exit_nicely();
		}
		if (!fgets(pwdbuf, sizeof(pwdbuf), pwf))
		{
			fprintf(stderr, _("%s: could not read password from file \"%s\": %s\n"),
					progname, pwfilename, strerror(errno));
			exit_nicely();
		}
		fclose(pwf);

		i = strlen(pwdbuf);
		while (i > 0 && (pwdbuf[i - 1] == '\r' || pwdbuf[i - 1] == '\n'))
			pwdbuf[--i] = '\0';

		pwd1 = xstrdup(pwdbuf);

	}
	printf(_("setting password ... "));
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	PG_CMD_PRINTF2("ALTER USER \"%s\" WITH PASSWORD E'%s';\n",
				   username, escape_quotes(pwd1));

	/* MM: pwd1 is no longer needed, freeing it */
	free(pwd1);

	PG_CMD_CLOSE;

	check_ok();

	snprintf(pwdpath, sizeof(pwdpath), "%s/global/pg_auth", pg_data);
	if (stat(pwdpath, &statbuf) != 0 || !S_ISREG(statbuf.st_mode))
	{
		fprintf(stderr,
				_("%s: The password file was not generated. "
				  "Please report this problem.\n"),
				progname);
		exit_nicely();
	}
}

/*
 * set up pg_depend
 */
static void
setup_depend(void)
{
	PG_CMD_DECL;
	const char **line;
	static const char *pg_depend_setup[] = {
		/*
		 * Make PIN entries in pg_depend for all objects made so far in the
		 * tables that the dependency code handles.  This is overkill (the
		 * system doesn't really depend on having every last weird datatype,
		 * for instance) but generating only the minimum required set of
		 * dependencies seems hard.
		 *
		 * Note that we deliberately do not pin the system views, which
		 * haven't been created yet.  Also, no conversions, databases, or
		 * tablespaces are pinned.
		 *
		 * First delete any already-made entries; PINs override all else, and
		 * must be the only entries for their objects.
		 */
		"DELETE FROM pg_depend;\n",
		"VACUUM pg_depend;\n",
		"DELETE FROM pg_shdepend;\n",
		"VACUUM pg_shdepend;\n",

		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_class;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_proc;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_type;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_cast;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_constraint;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_attrdef;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_language;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_operator;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_opclass;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_opfamily;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_amop;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_amproc;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_rewrite;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_trigger;\n",

		/*
		 * restriction here to avoid pinning the public namespace
		 */
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_namespace "
		"    WHERE nspname LIKE 'pg%';\n",

		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_parser;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_dict;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_template;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_config;\n",
		"INSERT INTO pg_shdepend SELECT 0, 0, 0, tableoid, oid, 'p' "
		" FROM pg_authid;\n",
		NULL
	};

	fputs(_("initializing dependencies ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = pg_depend_setup; *line != NULL; line++)
		PG_CMD_PUTS(*line);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * set up system views
 */
static void
setup_sysviews(void)
{
	PG_CMD_DECL;
	char	  **line;
	char	  **sysviews_setup;

	fputs(_("creating system views ... "), stdout);
	fflush(stdout);

	sysviews_setup = readfile(system_views_file);

	/*
	 * We use -j here to avoid backslashing stuff in system_views.sql
	 */
	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s -j template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = sysviews_setup; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	PG_CMD_CLOSE;

	free(sysviews_setup);

	check_ok();
}

/*
 * load description data
 */
static void
setup_description(void)
{
	PG_CMD_DECL;

	fputs(_("loading system objects' descriptions ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	PG_CMD_PUTS("CREATE TEMP TABLE tmp_pg_description ( "
				"	objoid oid, "
				"	classname name, "
				"	objsubid int4, "
				"	description text) WITHOUT OIDS;\n");

	PG_CMD_PRINTF1("COPY tmp_pg_description FROM E'%s';\n",
				   escape_quotes(desc_file));

	PG_CMD_PUTS("INSERT INTO pg_description "
				" SELECT t.objoid, c.oid, t.objsubid, t.description "
				"  FROM tmp_pg_description t, pg_class c "
				"    WHERE c.relname = t.classname;\n");

	PG_CMD_PUTS("CREATE TEMP TABLE tmp_pg_shdescription ( "
				" objoid oid, "
				" classname name, "
				" description text) WITHOUT OIDS;\n");

	PG_CMD_PRINTF1("COPY tmp_pg_shdescription FROM E'%s';\n",
				   escape_quotes(shdesc_file));

	PG_CMD_PUTS("INSERT INTO pg_shdescription "
				" SELECT t.objoid, c.oid, t.description "
				"  FROM tmp_pg_shdescription t, pg_class c "
				"   WHERE c.relname = t.classname;\n");

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * load conversion functions
 */
static void
setup_conversion(void)
{
	PG_CMD_DECL;
	char	  **line;
	char	  **conv_lines;

	fputs(_("creating conversions ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	conv_lines = readfile(conversion_file);
	for (line = conv_lines; *line != NULL; line++)
	{
		if (strstr(*line, "DROP CONVERSION") != *line)
			PG_CMD_PUTS(*line);
		free(*line);
	}

	free(conv_lines);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * load extra dictionaries (Snowball stemmers)
 */
static void
setup_dictionary(void)
{
	PG_CMD_DECL;
	char	  **line;
	char	  **conv_lines;

	fputs(_("creating dictionaries ... "), stdout);
	fflush(stdout);

	/*
	 * We use -j here to avoid backslashing stuff
	 */
	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s -j template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	conv_lines = readfile(dictionary_file);
	for (line = conv_lines; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	free(conv_lines);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * Set up privileges
 *
 * We mark most system catalogs as world-readable.	We don't currently have
 * to touch functions, languages, or databases, because their default
 * permissions are OK.
 *
 * Some objects may require different permissions by default, so we
 * make sure we don't overwrite privilege sets that have already been
 * set (NOT NULL).
 */
static void
setup_privileges(void)
{
	PG_CMD_DECL;
	char	  **line;
	char	  **priv_lines;
	static char *privileges_setup[] = {
		"UPDATE pg_class "
		"  SET relacl = E'{\"=r/\\\\\"$POSTGRES_SUPERUSERNAME\\\\\"\"}' "
		"  WHERE relkind IN ('r', 'v', 'S') AND relacl IS NULL;\n",
		"GRANT USAGE ON SCHEMA pg_catalog TO PUBLIC;\n",
		"GRANT CREATE, USAGE ON SCHEMA public TO PUBLIC;\n",
		NULL
	};

	fputs(_("setting privileges on built-in objects ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	priv_lines = replace_token(privileges_setup,
							   "$POSTGRES_SUPERUSERNAME", username);
	for (line = priv_lines; *line != NULL; line++)
		PG_CMD_PUTS(*line);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * extract the strange version of version required for information schema
 * (09.08.0007abc)
 */
static void
set_info_version(void)
{
	char	   *letterversion;
	long		major = 0,
				minor = 0,
				micro = 0;
	char	   *endptr;
	char	   *vstr = xstrdup(PG_VERSION);
	char	   *ptr;

	ptr = vstr + (strlen(vstr) - 1);
	while (ptr != vstr && (*ptr < '0' || *ptr > '9'))
		ptr--;
	letterversion = ptr + 1;
	major = strtol(vstr, &endptr, 10);
	if (*endptr)
		minor = strtol(endptr + 1, &endptr, 10);
	if (*endptr)
		micro = strtol(endptr + 1, &endptr, 10);
	snprintf(infoversion, sizeof(infoversion), "%02ld.%02ld.%04ld%s",
			 major, minor, micro, letterversion);
}

/*
 * load info schema and populate from features file
 */
static void
setup_schema(void)
{
	PG_CMD_DECL;
	char	  **line;
	char	  **lines;

	fputs(_("creating information schema ... "), stdout);
	fflush(stdout);

	lines = readfile(info_schema_file);

	/*
	 * We use -j here to avoid backslashing stuff in information_schema.sql
	 */
	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s -j template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = lines; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	free(lines);

	PG_CMD_CLOSE;

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	PG_CMD_PRINTF1("UPDATE information_schema.sql_implementation_info "
				   "  SET character_value = '%s' "
				   "  WHERE implementation_info_name = 'DBMS VERSION';\n",
				   infoversion);

	PG_CMD_PRINTF1("COPY information_schema.sql_features "
				   "  (feature_id, feature_name, sub_feature_id, "
				   "  sub_feature_name, is_supported, comments) "
				   " FROM E'%s';\n",
				   escape_quotes(features_file));

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * clean everything up in template1
 */
static void
vacuum_db(void)
{
	PG_CMD_DECL;

	fputs(_("vacuuming database template1 ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	PG_CMD_PUTS("ANALYZE;\nVACUUM FULL;\nVACUUM FREEZE;\n");

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * copy template1 to template0
 */
static void
make_template0(void)
{
	PG_CMD_DECL;
	const char **line;
	static const char *template0_setup[] = {
		"CREATE DATABASE template0;\n",
		"UPDATE pg_database SET "
		"	datistemplate = 't', "
		"	datallowconn = 'f' "
		"    WHERE datname = 'template0';\n",

		/*
		 * We use the OID of template0 to determine lastsysoid
		 */
		"UPDATE pg_database SET datlastsysoid = "
		"    (SELECT oid FROM pg_database "
		"    WHERE datname = 'template0');\n",

		/*
		 * Explicitly revoke public create-schema and create-temp-table
		 * privileges in template1 and template0; else the latter would be on
		 * by default
		 */
		"REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;\n",
		"REVOKE CREATE,TEMPORARY ON DATABASE template0 FROM public;\n",

		/*
		 * Finally vacuum to clean up dead rows in pg_database
		 */
		"VACUUM FULL pg_database;\n",
		NULL
	};

	fputs(_("copying template1 to template0 ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = template0_setup; *line; line++)
		PG_CMD_PUTS(*line);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * copy template1 to postgres
 */
static void
make_postgres(void)
{
	PG_CMD_DECL;
	const char **line;
	static const char *postgres_setup[] = {
		"CREATE DATABASE postgres;\n",
		NULL
	};

	fputs(_("copying template1 to postgres ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = postgres_setup; *line; line++)
		PG_CMD_PUTS(*line);

	PG_CMD_CLOSE;

	check_ok();
}


/*
 * signal handler in case we are interrupted.
 *
 * The Windows runtime docs at
 * http://msdn.microsoft.com/library/en-us/vclib/html/_crt_signal.asp
 * specifically forbid a number of things being done from a signal handler,
 * including IO, memory allocation and system calls, and only allow jmpbuf
 * if you are handling SIGFPE.
 *
 * I avoided doing the forbidden things by setting a flag instead of calling
 * exit_nicely() directly.
 *
 * Also note the behaviour of Windows with SIGINT, which says this:
 *	 Note	SIGINT is not supported for any Win32 application, including
 *	 Windows 98/Me and Windows NT/2000/XP. When a CTRL+C interrupt occurs,
 *	 Win32 operating systems generate a new thread to specifically handle
 *	 that interrupt. This can cause a single-thread application such as UNIX,
 *	 to become multithreaded, resulting in unexpected behavior.
 *
 * I have no idea how to handle this. (Strange they call UNIX an application!)
 * So this will need some testing on Windows.
 */
static void
trapsig(int signum)
{
	/* handle systems that reset the handler, like Windows (grr) */
	pqsignal(signum, trapsig);
	caught_signal = true;
}

/*
 * call exit_nicely() if we got a signal, or else output "ok".
 */
static void
check_ok(void)
{
	if (caught_signal)
	{
		printf(_("caught signal\n"));
		fflush(stdout);
		exit_nicely();
	}
	else if (output_failed)
	{
		printf(_("could not write to child process: %s\n"),
			   strerror(output_errno));
		fflush(stdout);
		exit_nicely();
	}
	else
	{
		/* all seems well */
		printf(_("ok\n"));
		fflush(stdout);
	}
}

/*
 * Escape (by doubling) any single quotes or backslashes in given string
 *
 * Note: this is used to process both postgresql.conf entries and SQL
 * string literals.  Since postgresql.conf strings are defined to treat
 * backslashes as escapes, we have to double backslashes here.	Hence,
 * when using this for a SQL string literal, use E'' syntax.
 *
 * We do not need to worry about encoding considerations because all
 * valid backend encodings are ASCII-safe.
 */
static char *
escape_quotes(const char *src)
{
	int			len = strlen(src),
				i,
				j;
	char	   *result = pg_malloc(len * 2 + 1);

	for (i = 0, j = 0; i < len; i++)
	{
		if (SQL_STR_DOUBLE(src[i], true))
			result[j++] = src[i];
		result[j++] = src[i];
	}
	result[j] = '\0';
	return result;
}

/* Hack to suppress a warning about %x from some versions of gcc */
static inline size_t
my_strftime(char *s, size_t max, const char *fmt, const struct tm * tm)
{
	return strftime(s, max, fmt, tm);
}

/*
 * Determine likely date order from locale
 */
static int
locale_date_order(const char *locale)
{
	struct tm	testtime;
	char		buf[128];
	char	   *posD;
	char	   *posM;
	char	   *posY;
	char	   *save;
	size_t		res;
	int			result;

	result = DATEORDER_MDY;		/* default */

	save = setlocale(LC_TIME, NULL);
	if (!save)
		return result;
	save = xstrdup(save);

	setlocale(LC_TIME, locale);

	memset(&testtime, 0, sizeof(testtime));
	testtime.tm_mday = 22;
	testtime.tm_mon = 10;		/* November, should come out as "11" */
	testtime.tm_year = 133;		/* 2033 */

	res = my_strftime(buf, sizeof(buf), "%x", &testtime);

	setlocale(LC_TIME, save);
	free(save);

	if (res == 0)
		return result;

	posM = strstr(buf, "11");
	posD = strstr(buf, "22");
	posY = strstr(buf, "33");

	if (!posM || !posD || !posY)
		return result;

	if (posY < posM && posM < posD)
		result = DATEORDER_YMD;
	else if (posD < posM)
		result = DATEORDER_DMY;
	else
		result = DATEORDER_MDY;

	return result;
}

/*
 * check if given string is a valid locale specifier
 */
static bool
chklocale(const char *locale)
{
	bool		ret;
	int			category = LC_CTYPE;
	char	   *save;

	save = setlocale(category, NULL);
	if (!save)
		return false;			/* should not happen; */

	save = xstrdup(save);

	ret = (setlocale(category, locale) != NULL);

	setlocale(category, save);
	free(save);

	/* should we exit here? */
	if (!ret)
		fprintf(stderr, _("%s: invalid locale name \"%s\"\n"), progname, locale);

	return ret;
}

/*
 * set up the locale variables
 *
 * assumes we have called setlocale(LC_ALL,"")
 */
static void
setlocales(void)
{
	/* set empty lc_* values to locale config if set */

	if (strlen(locale) > 0)
	{
		if (strlen(lc_ctype) == 0)
			lc_ctype = locale;
		if (strlen(lc_collate) == 0)
			lc_collate = locale;
		if (strlen(lc_numeric) == 0)
			lc_numeric = locale;
		if (strlen(lc_time) == 0)
			lc_time = locale;
		if (strlen(lc_monetary) == 0)
			lc_monetary = locale;
		if (strlen(lc_messages) == 0)
			lc_messages = locale;
	}

	/*
	 * override absent/invalid config settings from initdb's locale settings
	 */

	if (strlen(lc_ctype) == 0 || !chklocale(lc_ctype))
		lc_ctype = xstrdup(setlocale(LC_CTYPE, NULL));
	if (strlen(lc_collate) == 0 || !chklocale(lc_collate))
		lc_collate = xstrdup(setlocale(LC_COLLATE, NULL));
	if (strlen(lc_numeric) == 0 || !chklocale(lc_numeric))
		lc_numeric = xstrdup(setlocale(LC_NUMERIC, NULL));
	if (strlen(lc_time) == 0 || !chklocale(lc_time))
		lc_time = xstrdup(setlocale(LC_TIME, NULL));
	if (strlen(lc_monetary) == 0 || !chklocale(lc_monetary))
		lc_monetary = xstrdup(setlocale(LC_MONETARY, NULL));
	if (strlen(lc_messages) == 0 || !chklocale(lc_messages))
#if defined(LC_MESSAGES) && !defined(WIN32)
	{
		/* when available get the current locale setting */
		lc_messages = xstrdup(setlocale(LC_MESSAGES, NULL));
	}
#else
	{
		/* when not available, get the CTYPE setting */
		lc_messages = xstrdup(setlocale(LC_CTYPE, NULL));
	}
#endif

}

#ifdef WIN32
typedef		BOOL(WINAPI * __CreateRestrictedToken) (HANDLE, DWORD, DWORD, PSID_AND_ATTRIBUTES, DWORD, PLUID_AND_ATTRIBUTES, DWORD, PSID_AND_ATTRIBUTES, PHANDLE);

#define DISABLE_MAX_PRIVILEGE	0x1

/*
 * Create a restricted token and execute the specified process with it.
 *
 * Returns 0 on failure, non-zero on success, same as CreateProcess().
 *
 * On NT4, or any other system not containing the required functions, will
 * NOT execute anything.
 */
static int
CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION * processInfo)
{
	BOOL		b;
	STARTUPINFO si;
	HANDLE		origToken;
	HANDLE		restrictedToken;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	SID_AND_ATTRIBUTES dropSids[2];
	__CreateRestrictedToken _CreateRestrictedToken = NULL;
	HANDLE		Advapi32Handle;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);

	Advapi32Handle = LoadLibrary("ADVAPI32.DLL");
	if (Advapi32Handle != NULL)
	{
		_CreateRestrictedToken = (__CreateRestrictedToken) GetProcAddress(Advapi32Handle, "CreateRestrictedToken");
	}

	if (_CreateRestrictedToken == NULL)
	{
		fprintf(stderr, "WARNING: cannot create restricted tokens on this platform\n");
		if (Advapi32Handle != NULL)
			FreeLibrary(Advapi32Handle);
		return 0;
	}

	/* Open the current token to use as a base for the restricted one */
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &origToken))
	{
		fprintf(stderr, "Failed to open process token: %lu\n", GetLastError());
		return 0;
	}

	/* Allocate list of SIDs to remove */
	ZeroMemory(&dropSids, sizeof(dropSids));
	if (!AllocateAndInitializeSid(&NtAuthority, 2,
		 SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
								  0, &dropSids[0].Sid) ||
		!AllocateAndInitializeSid(&NtAuthority, 2,
	SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_POWER_USERS, 0, 0, 0, 0, 0,
								  0, &dropSids[1].Sid))
	{
		fprintf(stderr, "Failed to allocate SIDs: %lu\n", GetLastError());
		return 0;
	}

	b = _CreateRestrictedToken(origToken,
							   DISABLE_MAX_PRIVILEGE,
							   sizeof(dropSids) / sizeof(dropSids[0]),
							   dropSids,
							   0, NULL,
							   0, NULL,
							   &restrictedToken);

	FreeSid(dropSids[1].Sid);
	FreeSid(dropSids[0].Sid);
	CloseHandle(origToken);
	FreeLibrary(Advapi32Handle);

	if (!b)
	{
		fprintf(stderr, "Failed to create restricted token: %lu\n", GetLastError());
		return 0;
	}

	return CreateProcessAsUser(restrictedToken, NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, processInfo);
}
#endif

/*
 * print help text
 */
static void
usage(const char *progname)
{
	printf(_("%s initializes a PostgreSQL database cluster.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D, --pgdata=]DATADIR     location for this database cluster\n"));
	printf(_("  -E, --encoding=ENCODING   set default encoding for new databases\n"));
	printf(_("  --locale=LOCALE           initialize database cluster with given locale\n"));
	printf(_("  --lc-collate, --lc-ctype, --lc-messages=LOCALE\n"
			 "  --lc-monetary, --lc-numeric, --lc-time=LOCALE\n"
			 "                            initialize database cluster with given locale\n"
			 "                            in the respective category (default taken from\n"
			 "                            environment)\n"));
	printf(_("  --no-locale               equivalent to --locale=C\n"));
	printf(_("  -T, --text-search-config=CFG\n"
		 "                            default text search configuration\n"));
	printf(_("  -X, --xlogdir=XLOGDIR     location for the transaction log directory\n"));
	printf(_("  -A, --auth=METHOD         default authentication method for local connections\n"));
	printf(_("  -U, --username=NAME       database superuser name\n"));
	printf(_("  -W, --pwprompt            prompt for a password for the new superuser\n"));
	printf(_("  --pwfile=FILE             read password for the new superuser from file\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("\nLess commonly used options:\n"));
	printf(_("  -d, --debug               generate lots of debugging output\n"));
	printf(_("  -s, --show                show internal settings\n"));
	printf(_("  -L DIRECTORY              where to find the input files\n"));
	printf(_("  -n, --noclean             do not clean up after errors\n"));
	printf(_("\nIf the data directory is not specified, the environment variable PGDATA\n"
			 "is used.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}

int
main(int argc, char *argv[])
{
	/*
	 * options with no short version return a low integer, the rest return
	 * their short version value
	 */
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{"encoding", required_argument, NULL, 'E'},
		{"locale", required_argument, NULL, 1},
		{"lc-collate", required_argument, NULL, 2},
		{"lc-ctype", required_argument, NULL, 3},
		{"lc-monetary", required_argument, NULL, 4},
		{"lc-numeric", required_argument, NULL, 5},
		{"lc-time", required_argument, NULL, 6},
		{"lc-messages", required_argument, NULL, 7},
		{"no-locale", no_argument, NULL, 8},
		{"text-search-config", required_argument, NULL, 'T'},
		{"auth", required_argument, NULL, 'A'},
		{"pwprompt", no_argument, NULL, 'W'},
		{"pwfile", required_argument, NULL, 9},
		{"username", required_argument, NULL, 'U'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"debug", no_argument, NULL, 'd'},
		{"show", no_argument, NULL, 's'},
		{"noclean", no_argument, NULL, 'n'},
		{"xlogdir", required_argument, NULL, 'X'},
		{NULL, 0, NULL, 0}
	};

	int			c,
				i,
				ret;
	int			option_index;
	char	   *short_version;
	char	   *effective_user;
	char	   *pgdenv;			/* PGDATA value gotten from and sent to
								 * environment */
	char		bin_dir[MAXPGPATH];
	char	   *pg_data_native;

#ifdef WIN32
	char	   *restrict_env;
#endif
	static const char *subdirs[] = {
		"global",
		"pg_xlog",
		"pg_xlog/archive_status",
		"pg_clog",
		"pg_subtrans",
		"pg_twophase",
		"pg_multixact/members",
		"pg_multixact/offsets",
		"base",
		"base/1",
		"pg_tblspc"
	};

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "initdb");

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("initdb (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/* process command-line options */

	while ((c = getopt_long(argc, argv, "dD:E:L:nU:WA:sT:X:", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'A':
				authmethod = xstrdup(optarg);
				break;
			case 'D':
				pg_data = xstrdup(optarg);
				break;
			case 'E':
				encoding = xstrdup(optarg);
				break;
			case 'W':
				pwprompt = true;
				break;
			case 'U':
				username = xstrdup(optarg);
				break;
			case 'd':
				debug = true;
				printf(_("Running in debug mode.\n"));
				break;
			case 'n':
				noclean = true;
				printf(_("Running in noclean mode.  Mistakes will not be cleaned up.\n"));
				break;
			case 'L':
				share_path = xstrdup(optarg);
				break;
			case 1:
				locale = xstrdup(optarg);
				break;
			case 2:
				lc_collate = xstrdup(optarg);
				break;
			case 3:
				lc_ctype = xstrdup(optarg);
				break;
			case 4:
				lc_monetary = xstrdup(optarg);
				break;
			case 5:
				lc_numeric = xstrdup(optarg);
				break;
			case 6:
				lc_time = xstrdup(optarg);
				break;
			case 7:
				lc_messages = xstrdup(optarg);
				break;
			case 8:
				locale = "C";
				break;
			case 9:
				pwfilename = xstrdup(optarg);
				break;
			case 's':
				show_setting = true;
				break;
			case 'T':
				default_text_search_config = xstrdup(optarg);
				break;
			case 'X':
				xlog_dir = xstrdup(optarg);
				break;
			default:
				/* getopt_long already emitted a complaint */
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}


	/* Non-option argument specifies data directory */
	if (optind < argc)
	{
		pg_data = xstrdup(argv[optind]);
		optind++;
	}

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind + 1]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (pwprompt && pwfilename)
	{
		fprintf(stderr, _("%s: password prompt and password file cannot be specified together\n"), progname);
		exit(1);
	}

	if (authmethod == NULL || !strlen(authmethod))
	{
		authwarning = _("\nWARNING: enabling \"trust\" authentication for local connections\n"
						"You can change this by editing pg_hba.conf or using the -A option the\n"
						"next time you run initdb.\n");
		authmethod = "trust";
	}

	if (strcmp(authmethod, "md5") &&
		strcmp(authmethod, "ident") &&
		strncmp(authmethod, "ident ", 6) &&		/* ident with space = param */
		strcmp(authmethod, "trust") &&
#ifdef USE_PAM
		strcmp(authmethod, "pam") &&
		strncmp(authmethod, "pam ", 4) &&		/* pam with space = param */
#endif
		strcmp(authmethod, "crypt") &&
		strcmp(authmethod, "password")
		)

		/*
		 * Kerberos methods not listed because they are not supported over
		 * local connections and are rejected in hba.c
		 */
	{
		fprintf(stderr, _("%s: unrecognized authentication method \"%s\"\n"),
				progname, authmethod);
		exit(1);
	}

	if ((!strcmp(authmethod, "md5") ||
		 !strcmp(authmethod, "crypt") ||
		 !strcmp(authmethod, "password")) &&
		!(pwprompt || pwfilename))
	{
		fprintf(stderr, _("%s: must specify a password for the superuser to enable %s authentication\n"), progname, authmethod);
		exit(1);
	}

	if (strlen(pg_data) == 0)
	{
		pgdenv = getenv("PGDATA");
		if (pgdenv && strlen(pgdenv))
		{
			/* PGDATA found */
			pg_data = xstrdup(pgdenv);
		}
		else
		{
			fprintf(stderr,
					_("%s: no data directory specified\n"
					  "You must identify the directory where the data for this database system\n"
					  "will reside.  Do this with either the invocation option -D or the\n"
					  "environment variable PGDATA.\n"),
					progname);
			exit(1);
		}
	}

	pg_data_native = pg_data;
	canonicalize_path(pg_data);

#ifdef WIN32

	/*
	 * Before we execute another program, make sure that we are running with a
	 * restricted token. If not, re-execute ourselves with one.
	 */

	if ((restrict_env = getenv("PG_RESTRICT_EXEC")) == NULL
		|| strcmp(restrict_env, "1") != 0)
	{
		PROCESS_INFORMATION pi;
		char	   *cmdline;

		ZeroMemory(&pi, sizeof(pi));

		cmdline = xstrdup(GetCommandLine());

		putenv("PG_RESTRICT_EXEC=1");

		if (!CreateRestrictedProcess(cmdline, &pi))
		{
			fprintf(stderr, "Failed to re-exec with restricted token: %lu.\n", GetLastError());
		}
		else
		{
			/*
			 * Successfully re-execed. Now wait for child process to capture
			 * exitcode.
			 */
			DWORD		x;

			CloseHandle(pi.hThread);
			WaitForSingleObject(pi.hProcess, INFINITE);

			if (!GetExitCodeProcess(pi.hProcess, &x))
			{
				fprintf(stderr, "Failed to get exit code from subprocess: %lu\n", GetLastError());
				exit(1);
			}
			exit(x);
		}
	}
#endif

	/*
	 * we have to set PGDATA for postgres rather than pass it on the command
	 * line to avoid dumb quoting problems on Windows, and we would especially
	 * need quotes otherwise on Windows because paths there are most likely to
	 * have embedded spaces.
	 */
	pgdenv = pg_malloc(8 + strlen(pg_data));
	sprintf(pgdenv, "PGDATA=%s", pg_data);
	putenv(pgdenv);

	if ((ret = find_other_exec(argv[0], "postgres", PG_VERSIONSTR,
							   backend_exec)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv[0], full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			fprintf(stderr,
					_("The program \"postgres\" is needed by %s "
					  "but was not found in the\n"
					  "same directory as \"%s\".\n"
					  "Check your installation.\n"),
					progname, full_path);
		else
			fprintf(stderr,
					_("The program \"postgres\" was found by \"%s\"\n"
					  "but was not the same version as %s.\n"
					  "Check your installation.\n"),
					full_path, progname);
		exit(1);
	}

	/* store binary directory */
	strcpy(bin_path, backend_exec);
	*last_dir_separator(bin_path) = '\0';
	canonicalize_path(bin_path);

	if (!share_path)
	{
		share_path = pg_malloc(MAXPGPATH);
		get_share_path(backend_exec, share_path);
	}
	else if (!is_absolute_path(share_path))
	{
		fprintf(stderr, _("%s: input file location must be an absolute path\n"), progname);
		exit(1);
	}

	canonicalize_path(share_path);

	if ((short_version = get_short_version()) == NULL)
	{
		fprintf(stderr, _("%s: could not determine valid short version string\n"), progname);
		exit(1);
	}

	effective_user = get_id();
	if (strlen(username) == 0)
		username = effective_user;

	set_input(&bki_file, "postgres.bki");
	set_input(&desc_file, "postgres.description");
	set_input(&shdesc_file, "postgres.shdescription");
	set_input(&hba_file, "pg_hba.conf.sample");
	set_input(&ident_file, "pg_ident.conf.sample");
	set_input(&conf_file, "postgresql.conf.sample");
	set_input(&conversion_file, "conversion_create.sql");
	set_input(&dictionary_file, "snowball_create.sql");
	set_input(&info_schema_file, "information_schema.sql");
	set_input(&features_file, "sql_features.txt");
	set_input(&system_views_file, "system_views.sql");

	set_info_version();

	if (show_setting || debug)
	{
		fprintf(stderr,
				"VERSION=%s\n"
				"PGDATA=%s\nshare_path=%s\nPGPATH=%s\n"
				"POSTGRES_SUPERUSERNAME=%s\nPOSTGRES_BKI=%s\n"
				"POSTGRES_DESCR=%s\nPOSTGRES_SHDESCR=%s\n"
				"POSTGRESQL_CONF_SAMPLE=%s\n"
				"PG_HBA_SAMPLE=%s\nPG_IDENT_SAMPLE=%s\n",
				PG_VERSION,
				pg_data, share_path, bin_path,
				username, bki_file,
				desc_file, shdesc_file,
				conf_file,
				hba_file, ident_file);
		if (show_setting)
			exit(0);
	}

	check_input(bki_file);
	check_input(desc_file);
	check_input(shdesc_file);
	check_input(hba_file);
	check_input(ident_file);
	check_input(conf_file);
	check_input(conversion_file);
	check_input(dictionary_file);
	check_input(info_schema_file);
	check_input(features_file);
	check_input(system_views_file);

	setlocales();

	printf(_("The files belonging to this database system will be owned "
			 "by user \"%s\".\n"
			 "This user must also own the server process.\n\n"),
		   effective_user);

	if (strcmp(lc_ctype, lc_collate) == 0 &&
		strcmp(lc_ctype, lc_time) == 0 &&
		strcmp(lc_ctype, lc_numeric) == 0 &&
		strcmp(lc_ctype, lc_monetary) == 0 &&
		strcmp(lc_ctype, lc_messages) == 0)
		printf(_("The database cluster will be initialized with locale %s.\n"), lc_ctype);
	else
	{
		printf(_("The database cluster will be initialized with locales\n"
				 "  COLLATE:  %s\n"
				 "  CTYPE:    %s\n"
				 "  MESSAGES: %s\n"
				 "  MONETARY: %s\n"
				 "  NUMERIC:  %s\n"
				 "  TIME:     %s\n"),
			   lc_collate,
			   lc_ctype,
			   lc_messages,
			   lc_monetary,
			   lc_numeric,
			   lc_time);
	}

	if (strlen(encoding) == 0)
	{
		int			ctype_enc;

		ctype_enc = pg_get_encoding_from_locale(lc_ctype);

		if (ctype_enc == PG_SQL_ASCII &&
			!(pg_strcasecmp(lc_ctype, "C") == 0 ||
			  pg_strcasecmp(lc_ctype, "POSIX") == 0))
		{
			/* Hmm, couldn't recognize the locale's codeset */
			fprintf(stderr, _("%s: could not find suitable encoding for locale %s\n"),
					progname, lc_ctype);
			fprintf(stderr, _("Rerun %s with the -E option.\n"), progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
		else if (!pg_valid_server_encoding_id(ctype_enc))
		{
			/* We recognized it, but it's not a legal server encoding */
			fprintf(stderr,
					_("%s: locale %s requires unsupported encoding %s\n"),
					progname, lc_ctype, pg_encoding_to_char(ctype_enc));
			fprintf(stderr,
				  _("Encoding %s is not allowed as a server-side encoding.\n"
					"Rerun %s with a different locale selection.\n"),
					pg_encoding_to_char(ctype_enc), progname);
			exit(1);
		}
		else
		{
			encodingid = encodingid_to_string(ctype_enc);
			printf(_("The default database encoding has accordingly been set to %s.\n"),
				   pg_encoding_to_char(ctype_enc));
		}
	}
	else
	{
		int			user_enc;
		int			ctype_enc;

		encodingid = get_encoding_id(encoding);
		user_enc = atoi(encodingid);

		ctype_enc = pg_get_encoding_from_locale(lc_ctype);

		/* We allow selection of SQL_ASCII --- see notes in createdb() */
		if (!(ctype_enc == user_enc ||
			  ctype_enc == PG_SQL_ASCII ||
			  user_enc == PG_SQL_ASCII
#ifdef WIN32

		/*
		 * On win32, if the encoding chosen is UTF8, all locales are OK
		 * (assuming the actual locale name passed the checks above). This is
		 * because UTF8 is a pseudo-codepage, that we convert to UTF16 before
		 * doing any operations on, and UTF16 supports all locales.
		 */
			  || user_enc == PG_UTF8
#endif
			  ))
		{
			fprintf(stderr, _("%s: encoding mismatch\n"), progname);
			fprintf(stderr,
			   _("The encoding you selected (%s) and the encoding that the\n"
			  "selected locale uses (%s) do not match.  This would lead to\n"
			"misbehavior in various character string processing functions.\n"
			   "Rerun %s and either do not specify an encoding explicitly,\n"
				 "or choose a matching combination.\n"),
					pg_encoding_to_char(user_enc),
					pg_encoding_to_char(ctype_enc),
					progname);
			exit(1);
		}
	}

	if (strlen(default_text_search_config) == 0)
	{
		default_text_search_config = find_matching_ts_config(lc_ctype);
		if (default_text_search_config == NULL)
		{
			printf(_("%s: could not find suitable text search configuration for locale %s\n"),
				   progname, lc_ctype);
			default_text_search_config = "simple";
		}
	}
	else
	{
		const char *checkmatch = find_matching_ts_config(lc_ctype);

		if (checkmatch == NULL)
		{
			printf(_("%s: warning: suitable text search configuration for locale %s is unknown\n"),
				   progname, lc_ctype);
		}
		else if (strcmp(checkmatch, default_text_search_config) != 0)
		{
			printf(_("%s: warning: specified text search configuration \"%s\" might not match locale %s\n"),
				   progname, default_text_search_config, lc_ctype);
		}
	}

	printf(_("The default text search configuration will be set to \"%s\".\n"),
		   default_text_search_config);

	printf("\n");

	umask(077);

	/*
	 * now we are starting to do real work, trap signals so we can clean up
	 */

	/* some of these are not valid on Windows */
#ifdef SIGHUP
	pqsignal(SIGHUP, trapsig);
#endif
#ifdef SIGINT
	pqsignal(SIGINT, trapsig);
#endif
#ifdef SIGQUIT
	pqsignal(SIGQUIT, trapsig);
#endif
#ifdef SIGTERM
	pqsignal(SIGTERM, trapsig);
#endif

	/* Ignore SIGPIPE when writing to backend, so we can clean up */
#ifdef SIGPIPE
	pqsignal(SIGPIPE, SIG_IGN);
#endif

	switch (check_data_dir(pg_data))
	{
		case 0:
			/* PGDATA not there, must create it */
			printf(_("creating directory %s ... "),
				   pg_data);
			fflush(stdout);

			if (!mkdatadir(NULL))
				exit_nicely();
			else
				check_ok();

			made_new_pgdata = true;
			break;

		case 1:
			/* Present but empty, fix permissions and use it */
			printf(_("fixing permissions on existing directory %s ... "),
				   pg_data);
			fflush(stdout);

			if (chmod(pg_data, 0700) != 0)
			{
				fprintf(stderr, _("%s: could not change permissions of directory \"%s\": %s\n"),
						progname, pg_data, strerror(errno));
				exit_nicely();
			}
			else
				check_ok();

			found_existing_pgdata = true;
			break;

		case 2:
			/* Present and not empty */
			fprintf(stderr,
					_("%s: directory \"%s\" exists but is not empty\n"),
					progname, pg_data);
			fprintf(stderr,
					_("If you want to create a new database system, either remove or empty\n"
					  "the directory \"%s\" or run %s\n"
					  "with an argument other than \"%s\".\n"),
					pg_data, progname, pg_data);
			exit(1);			/* no further message needed */

		default:
			/* Trouble accessing directory */
			fprintf(stderr, _("%s: could not access directory \"%s\": %s\n"),
					progname, pg_data, strerror(errno));
			exit_nicely();
	}

	/* Create transaction log symlink, if required */
	if (strcmp(xlog_dir, "") != 0)
	{
		char	   *linkloc;

		linkloc = (char *) pg_malloc(strlen(pg_data) + 8 + 2);
		sprintf(linkloc, "%s/pg_xlog", pg_data);

		/* check if the specified xlog directory is empty */
		switch (check_data_dir(xlog_dir))
		{
			case 0:
				/* xlog directory not there, must create it */
				printf(_("creating directory %s ... "),
					   xlog_dir);
				fflush(stdout);

				if (mkdir_p(xlog_dir, 0700) != 0)
				{
					fprintf(stderr, _("%s: could not create directory \"%s\": %s\n"),
							progname, xlog_dir, strerror(errno));
					exit_nicely();
				}
				else
				{
					check_ok();
				}

				made_new_xlogdir = true;
				break;
			case 1:
				/* Present but empty, fix permissions and use it */
				printf(_("fixing permissions on existing directory %s ... "),
					   xlog_dir);
				fflush(stdout);

				if (chmod(xlog_dir, 0700) != 0)
				{
					fprintf(stderr, _("%s: could not change permissions of directory \"%s\": %s\n"),
							progname, xlog_dir, strerror(errno));
					exit_nicely();
				}
				else
					check_ok();

				found_existing_xlogdir = true;
				break;
			case 2:
				/* Present and not empty */
				fprintf(stderr,
						_("%s: directory \"%s\" exists but is not empty\n"),
						progname, xlog_dir);
				fprintf(stderr,
						_("If you want to store the transaction log there, either\n"
						  "remove or empty the directory \"%s\".\n"),
						xlog_dir);
				exit(1);		/* no further message needed */

			default:
				/* Trouble accessing directory */
				fprintf(stderr, _("%s: could not access directory \"%s\": %s\n"),
						progname, xlog_dir, strerror(errno));
				exit_nicely();
		}

#ifdef HAVE_SYMLINK
		if (symlink(xlog_dir, linkloc) != 0)
		{
			fprintf(stderr, _("%s: could not create symbolic link \"%s\": %s\n"),
					progname, linkloc, strerror(errno));
			exit_nicely();
		}
#else
		fprintf(stderr, _("%s: symlinks are not supported on this platform"));
		exit_nicely();
#endif
	}

	/* Create required subdirectories */
	printf(_("creating subdirectories ... "));
	fflush(stdout);

	for (i = 0; i < (sizeof(subdirs) / sizeof(char *)); i++)
	{
		if (!mkdatadir(subdirs[i]))
			exit_nicely();
	}

	check_ok();

	/* Top level PG_VERSION is checked by bootstrapper, so make it first */
	set_short_version(short_version, NULL);

	/* Select suitable configuration settings */
	set_null_conf();
	test_config_settings();

	/* Now create all the text config files */
	setup_config();

	/* Bootstrap template1 */
	bootstrap_template1(short_version);

	/*
	 * Make the per-database PG_VERSION for template1 only after init'ing it
	 */
	set_short_version(short_version, "base/1");

	/* Create the stuff we don't need to use bootstrap mode for */

	setup_auth();
	if (pwprompt || pwfilename)
		get_set_pwd();

	setup_depend();

	setup_sysviews();

	setup_description();

	setup_conversion();

	setup_dictionary();

	setup_privileges();

	setup_schema();

	vacuum_db();

	make_template0();

	make_postgres();

	if (authwarning != NULL)
		fprintf(stderr, "%s", authwarning);

	/* Get directory specification used to start this executable */
	strcpy(bin_dir, argv[0]);
	get_parent_directory(bin_dir);

	printf(_("\nSuccess. You can now start the database server using:\n\n"
			 "    %s%s%spostgres%s -D %s%s%s\n"
			 "or\n"
			 "    %s%s%spg_ctl%s -D %s%s%s -l logfile start\n\n"),
	   QUOTE_PATH, bin_dir, (strlen(bin_dir) > 0) ? DIR_SEP : "", QUOTE_PATH,
		   QUOTE_PATH, pg_data_native, QUOTE_PATH,
	   QUOTE_PATH, bin_dir, (strlen(bin_dir) > 0) ? DIR_SEP : "", QUOTE_PATH,
		   QUOTE_PATH, pg_data_native, QUOTE_PATH);

	return 0;
}
