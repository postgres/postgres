/*-------------------------------------------------------------------------
 *
 * initdb --- initialize a PostgreSQL installation
 *
 * initdb creates (initializes) a PostgreSQL database cluster (site,
 * instance, installation, whatever).  A database cluster is a
 * collection of PostgreSQL databases all managed by the same postmaster.
 *
 * To create the database cluster, we create the directory that contains
 * all its data, create the files that hold the global tables, create
 * a few other control files for it, and create two databases: the
 * template0 and template1 databases.
 *
 * The template databases are ordinary PostgreSQL databases.  template0
 * is never supposed to change after initdb, whereas template1 can be
 * changed to add site-local standard data.  Either one can be copied
 * to produce a new database.
 *
 * To create template1, we run the postgres (backend) program in bootstrap
 * mode and feed it data from the postgres.bki library file.  After this
 * initial bootstrap phase, some additional stuff is created by normal
 * SQL commands fed to a standalone backend.  Some of those commands are
 * just embedded into this program (yeah, it's ugly), but larger chunks
 * are taken from script files.
 *
 * template0 is made just by copying the completed template1.
 *
 * Note:
 *	 The program has some memory leakage - it isn't worth cleaning it up.
 *
 *
 * This is a C implementation of the previous shell script for setting up a
 * PostgreSQL cluster location, and should be highly compatible with it.
 * author of C translation: Andrew Dunstan	   mailto:andrew@dunslane.net
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions taken from FreeBSD.
 *
 * $PostgreSQL: pgsql/src/bin/initdb/initdb.c,v 1.36 2004/06/10 16:35:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <errno.h>

#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "getopt_long.h"

#ifndef HAVE_OPTRESET
int			optreset;
#endif


#define _(x) gettext((x))

/* version string we expect back from postgres */
#define PG_VERSIONSTR "postgres (PostgreSQL) " PG_VERSION "\n"

/*
 * these values are passed in by makefile defines
 */
char		*share_path = NULL;

/* values to be obtained from arguments */
char	   *pg_data = "";
char	   *encoding = "";
char	   *locale = "";
char	   *lc_collate = "";
char	   *lc_ctype = "";
char	   *lc_monetary = "";
char	   *lc_numeric = "";
char	   *lc_time = "";
char	   *lc_messages = "";
char	   *username = "";
bool		pwprompt = false;
bool		debug = false;
bool		noclean = false;
bool		show_setting = false;


/* internal vars */
const char *progname;
char	   *postgres;
char	   *encodingid = "0";
char	   *bki_file;
char	   *desc_file;
char	   *hba_file;
char	   *ident_file;
char	   *conf_file;
char	   *conversion_file;
char	   *info_schema_file;
char	   *features_file;
char	   *system_views_file;
char	   *effective_user;
bool		testpath = true;
bool		made_new_pgdata = false;
bool		found_existing_pgdata = false;
char		infoversion[100];
bool		caught_signal = false;
bool		output_failed = false;

/* defaults */
int			n_connections = 10;
int			n_buffers = 50;

/*
 * Centralized knowledge of switches to pass to backend
 *
 * Note: in the shell-script version, we also passed PGDATA as a -D switch,
 * but here it is more convenient to pass it as an environment variable
 * (no quoting to worry about).
 */
static const char *boot_options = "-F";
static const char *backend_options = "-F -O -c search_path=pg_catalog -c exit_on_error=true";


/* path to 'initdb' binary directory */
char	   bin_path[MAXPGPATH];
char	   backend_exec[MAXPGPATH];

static void *xmalloc(size_t size);
static char *xstrdup(const char *s);
static bool rmtree(char *path, bool rmtopdir);
static char **replace_token(char **lines, char *token, char *replacement);
static char **readfile(char *path);
static void writefile(char *path, char **lines);
static int mkdir_p(char *path, mode_t omode);
static void exit_nicely(void);
static char *get_id(void);
static char *get_encoding_id(char *encoding_name);
static char *get_short_version(void);
static int check_data_dir(void);
static bool mkdatadir(char *subdir);
static void set_input(char **dest, char *filename);
static void check_input(char *path);
static void set_short_version(char *short_version, char *extrapath);
static void set_null_conf(void);
static void test_connections(void);
static void test_buffers(void);
static void setup_config(void);
static void bootstrap_template1(char *short_version);
static void setup_shadow(void);
static void get_set_pwd(void);
static void unlimit_systables(void);
static void setup_depend(void);
static void setup_sysviews(void);
static void setup_description(void);
static void setup_conversion(void);
static void setup_privileges(void);
static void set_info_version(void);
static void setup_schema(void);
static void vacuum_db(void);
static void make_template0(void);
static void trapsig(int signum);
static void check_ok(void);
static bool chklocale(const char *locale);
static void setlocales(void);
static void usage(const char *progname);


/*
 * macros for running pipes to postgres
 */
#define PG_CMD_DECL		char cmd[MAXPGPATH]; char ** line ; FILE * pg
#define PG_CMD_DECL_NOLINE		   char cmd[MAXPGPATH]; FILE * pg

#define PG_CMD_OPEN \
do { \
	fflush(stdout); \
	fflush(stderr); \
	pg = popen(cmd, "w"); \
	if (pg == NULL) \
		exit_nicely(); \
} while (0)

#define PG_CMD_CLOSE \
do { \
	if (pclose_check(pg)) \
		exit_nicely(); \
} while (0)

#define PG_CMD_PUTLINE \
do { \
	if (fputs(*line, pg) < 0 || fflush(pg) < 0) \
		output_failed = true; \
} while (0)

#ifndef WIN32
#define QUOTE_PATH	""
#else
#define QUOTE_PATH	"\""
#endif

/*
 * routines to check mem allocations and fail noisily.
 *
 * Note that we can't call exit_nicely() on a memory failure, as it calls
 * rmtree() which needs memory allocation. So we just exit with a bang.
 */
static void *
xmalloc(size_t size)
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
 * delete a directory tree recursively
 * assumes path points to a valid directory
 * deletes everything under path
 * if rmtopdir is true deletes the directory too
 */
static bool
rmtree(char *path, bool rmtopdir)
{
	char		buf[MAXPGPATH + 64];

#ifndef WIN32
	/* doesn't handle .* files, but we don't make any... */
	snprintf(buf, sizeof(buf), "rm -rf \"%s\"%s", path,
			 rmtopdir ? "" : "/*");
#else
	snprintf(buf, sizeof(buf), "%s /s /q \"%s\"",
			 rmtopdir ? "rmdir" : "del", path);
#endif

	return !system(buf);
}


/*
 * make a copy of the array of lines, with token replaced by replacement
 * the first time it occurs on each line.
 *
 * This does most of what sed was used for in the shell script, but
 * doesn't need any regexp stuff.
 */
static char **
replace_token(char **lines, char *token, char *replacement)
{
	int			numlines = 1;
	int			i;
	char	  **result;
	int			toklen,
				replen,
				diff;

	for (i = 0; lines[i]; i++)
		numlines++;

	result = (char **) xmalloc(numlines * sizeof(char *));

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

		newline = (char *) xmalloc(strlen(lines[i]) + diff + 1);

		pre = where - lines[i];

		strncpy(newline, lines[i], pre);

		strcpy(newline + pre, replacement);

		strcpy(newline + pre + replen, lines[i] + pre + toklen);

		result[i] = newline;

	}

	return result;

}

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

	result = (char **) xmalloc((nlines + 2) * sizeof(char *));
	buffer = (char *) xmalloc(maxlength + 2);

	/* now reprocess the file and store the lines */

	rewind(infile);
	nlines = 0;
	while (fgets(buffer, maxlength + 1, infile) != NULL)
	{
		result[nlines] = xstrdup(buffer);
		nlines++;
	}

	fclose(infile);
	result[nlines] = NULL;

	return result;
}

/*
 * write an array of lines to a file
 */
static void
writefile(char *path, char **lines)
{
	FILE	   *out_file;
	char	  **line;

	if ((out_file = fopen(path, PG_BINARY_W)) == NULL)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
	for (line = lines; *line != NULL; line++)
	{
		if (fputs(*line, out_file) < 0)
			exit_nicely();
		free(*line);
	}
	if (fclose(out_file))
		exit_nicely();
}

/* source stolen from FreeBSD /src/bin/mkdir/mkdir.c and adapted */

/*
 * this tries to build all the elements of a path to a directory a la mkdir -p
 * we assume the path is in canonical form, i.e. uses / as the separator
 * we also assume it isn't null.
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
			 * existing directory, effects equivalent to those cased by
			 * the following command shall occcur:
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
		if (mkdir(path, last ? omode : S_IRWXU | S_IRWXG | S_IRWXO) < 0)
		{
			if (errno == EEXIST || errno == EISDIR)
			{
				if (stat(path, &sb) < 0)
				{
					retval = 1;
					break;
				}
				else if (!S_ISDIR(sb.st_mode))
				{
					if (last)
						errno = EEXIST;
					else
						errno = ENOTDIR;
					retval = 1;
					break;
				}
			}
			else
			{
				retval = 1;
				break;
			}
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
	fprintf(stderr, _("%s: failed\n"), progname);

	if (!noclean)
	{
		if (made_new_pgdata)
		{
			fprintf(stderr, _("%s: removing data directory \"%s\"\n"),
					progname, pg_data);
			if (!rmtree(pg_data, true))
				fprintf(stderr, _("%s: failed\n"), progname);
		}
		else if (found_existing_pgdata)
		{
			fprintf(stderr,
					_("%s: removing contents of data directory \"%s\"\n"),
					progname, pg_data);
			if (!rmtree(pg_data, false))
				fprintf(stderr, _("%s: failed\n"), progname);
		}
		/* otherwise died during startup, do nothing! */
	}
	else
	{
		if (made_new_pgdata || found_existing_pgdata)
			fprintf(stderr,
					_("%s: data directory \"%s\" not removed at user's request\n"),
					progname, pg_data);
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

	pw = getpwuid(getuid());

#ifndef __BEOS__				/* no root check on BEOS */

	if (!geteuid())				/* 0 is root's uid */
	{
		fprintf(stderr,
				_("%s: cannot be run as root\n"
				"Please log in (using, e.g., \"su\") as the "
				"(unprivileged) user that will\n"
				"own the server process.\n"),
				progname);
		exit(1);
	}
#endif

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

/*
 * get the encoding id for a given encoding name
 */
static char *
get_encoding_id(char *encoding_name)
{
	int			enc;
	char		result[20];

	if (encoding_name && *encoding_name)
	{
		if ((enc = pg_char_to_encoding(encoding_name)) >= 0 &&
			pg_valid_server_encoding(encoding_name) >= 0)
		{
			sprintf(result, "%d", enc);
			return xstrdup(result);
		}
	}
	fprintf(stderr, _("%s: \"%s\" is not a valid server encoding name\n"),
			progname, encoding_name ? encoding_name : "(null)");
	exit(1);
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
 * make sure the data directory either doesn't exist or is empty
 *
 * Returns 0 if nonexistent, 1 if exists and empty, 2 if not empty,
 * or -1 if trouble accessing directory
 */
static int
check_data_dir(void)
{
	DIR		   *chkdir;
	struct dirent *file;
	int			result = 1;

	errno = 0;

	chkdir = opendir(pg_data);

	if (!chkdir)
		return (errno == ENOENT) ? 0 : -1;

	while ((file = readdir(chkdir)) != NULL)
	{
		if (strcmp(".", file->d_name) == 0 || strcmp("..", file->d_name) == 0)
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

	closedir(chkdir);

	if (errno != 0)
		result = -1;			/* some kind of I/O error? */

	return result;
}

/*
 * make the data directory (or one of its subdirectories if subdir is not NULL)
 */
static bool
mkdatadir(char *subdir)
{
	char	   *path;

	path = xmalloc(strlen(pg_data) + 2 +
				   (subdir == NULL ? 0 : strlen(subdir)));

	if (subdir != NULL)
		sprintf(path, "%s/%s", pg_data, subdir);
	else
		strcpy(path, pg_data);

	return (mkdir_p(path, 0700) == 0);
}


/*
 * set name of given input file variable under data directory
 */
static void
set_input(char **dest, char *filename)
{
	*dest = xmalloc(strlen(share_path) + strlen(filename) + 2);
	sprintf(*dest, "%s/%s", share_path, filename);
}

/*
 * check that given input file exists
 */
static void
check_input(char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) != 0 || !S_ISREG(statbuf.st_mode))
	{
		fprintf(stderr,
				_("%s: file \"%s\" does not exist\n"
				  "This means you have a corrupted installation or identified\n"
				  "the wrong directory with the invocation option -L.\n"),
				progname, path);
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
		path = xmalloc(strlen(pg_data) + 12);
		sprintf(path, "%s/PG_VERSION", pg_data);
	}
	else
	{
		path = xmalloc(strlen(pg_data) + strlen(extrapath) + 13);
		sprintf(path, "%s/%s/PG_VERSION", pg_data, extrapath);
	}
	version_file = fopen(path, PG_BINARY_W);
	if (version_file == NULL)
		exit_nicely();
	fprintf(version_file, "%s\n", short_version);
	if (fclose(version_file))
		exit_nicely();
}

/*
 * set up an empty config file so we can check buffers and connections
 */
static void
set_null_conf(void)
{
	FILE	   *conf_file;
	char	   *path;

	path = xmalloc(strlen(pg_data) + 17);
	sprintf(path, "%s/postgresql.conf", pg_data);
	conf_file = fopen(path, PG_BINARY_W);
	if (conf_file == NULL || fclose(conf_file))
		exit_nicely();
}

/*
 * check how many connections we can sustain
 */
static void
test_connections(void)
{
	char		cmd[MAXPGPATH];
	static const int conns[] = {100, 50, 40, 30, 20, 10};
	static const int len = sizeof(conns) / sizeof(int);
	int			i,
				status;

	printf(_("selecting default max_connections ... "));
	fflush(stdout);

	for (i = 0; i < len; i++)
	{
		snprintf(cmd, sizeof(cmd),
				 "%s\"%s\" -boot -x0 %s "
				 "-c shared_buffers=%d -c max_connections=%d template1 "
				 "< \"%s\" > \"%s\" 2>&1%s",
				 SYSTEMQUOTE, backend_exec, boot_options,
				 conns[i] * 5, conns[i],
				 DEVNULL, DEVNULL, SYSTEMQUOTE);
		status = system(cmd);
		if (status == 0)
			break;
	}
	if (i >= len)
		i = len - 1;
	n_connections = conns[i];

	printf("%d\n", n_connections);
}

/*
 * check how many buffers we can run with
 */
static void
test_buffers(void)
{
	char		cmd[MAXPGPATH];
	static const int bufs[] = {1000, 900, 800, 700, 600, 500,
							   400, 300, 200, 100, 50};
	static const int len = sizeof(bufs) / sizeof(int);
	int			i,
				status;

	printf(_("selecting default shared_buffers ... "));
	fflush(stdout);

	for (i = 0; i < len; i++)
	{
		snprintf(cmd, sizeof(cmd),
				 "%s\"%s\" -boot -x0 %s "
				 "-c shared_buffers=%d -c max_connections=%d template1 "
				 "< \"%s\" > \"%s\" 2>&1%s",
				 SYSTEMQUOTE, backend_exec, boot_options,
				 bufs[i], n_connections,
				 DEVNULL, DEVNULL, SYSTEMQUOTE);
		status = system(cmd);
		if (status == 0)
			break;
	}
	if (i >= len)
		i = len - 1;
	n_buffers = bufs[i];

	printf("%d\n", n_buffers);
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

	snprintf(repltok, sizeof(repltok), "shared_buffers = %d", n_buffers);
	conflines = replace_token(conflines, "#shared_buffers = 1000", repltok);

	snprintf(repltok, sizeof(repltok), "lc_messages = '%s'", lc_messages);
	conflines = replace_token(conflines, "#lc_messages = 'C'", repltok);

	snprintf(repltok, sizeof(repltok), "lc_monetary = '%s'", lc_monetary);
	conflines = replace_token(conflines, "#lc_monetary = 'C'", repltok);

	snprintf(repltok, sizeof(repltok), "lc_numeric = '%s'", lc_numeric);

	conflines = replace_token(conflines, "#lc_numeric = 'C'", repltok);

	snprintf(repltok, sizeof(repltok), "lc_time = '%s'", lc_time);
	conflines = replace_token(conflines, "#lc_time = 'C'", repltok);

	snprintf(path, sizeof(path), "%s/postgresql.conf", pg_data);

	writefile(path, conflines);
	chmod(path, 0600);

	free(conflines);


	/* pg_hba.conf */

	conflines = readfile(hba_file);

#ifndef HAVE_IPV6
	conflines = replace_token(conflines,
							  "host    all         all         ::1",
							  "#host    all         all         ::1");
#endif

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
	char	   *talkargs = "";
	char	  **bki_lines;
	char		headerline[MAXPGPATH];

	PG_CMD_DECL;

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

	bki_lines = replace_token(bki_lines, "POSTGRES", effective_user);

	bki_lines = replace_token(bki_lines, "ENCODING", encodingid);

	/*
	 * Pass correct LC_xxx environment to bootstrap.
	 *
	 * The shell script arranged to restore the LC settings afterwards,
	 * but there doesn't seem to be any compelling reason to do that.
	 */
	snprintf(cmd, sizeof(cmd), "LC_COLLATE=%s", lc_collate);
	putenv(xstrdup(cmd));

	snprintf(cmd, sizeof(cmd), "LC_CTYPE=%s", lc_ctype);
	putenv(xstrdup(cmd));

	unsetenv("LC_ALL");

	/* Also ensure backend isn't confused by this environment var: */
	unsetenv("PGCLIENTENCODING");

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" -boot -x1 %s %s template1",
			 backend_exec, boot_options, talkargs);

	PG_CMD_OPEN;

	for (line = bki_lines; *line != NULL; line++)
	{
		PG_CMD_PUTLINE;
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
setup_shadow(void)
{
	char	   *pg_shadow_setup[] = {
		/*
		 * Create a trigger so that direct updates to pg_shadow will be
		 * written to the flat password/group files pg_pwd and pg_group
		 */
		"CREATE TRIGGER pg_sync_pg_pwd "
		"  AFTER INSERT OR UPDATE OR DELETE ON pg_shadow "
		"  FOR EACH ROW EXECUTE PROCEDURE update_pg_pwd_and_pg_group();\n",
		"CREATE TRIGGER pg_sync_pg_group "
		"  AFTER INSERT OR UPDATE OR DELETE ON pg_group "
		"  FOR EACH ROW EXECUTE PROCEDURE update_pg_pwd_and_pg_group();\n",

		/*
		 * needs to be done before alter user, because alter user checks
		 * that pg_shadow is secure ...
		 */
		"REVOKE ALL on pg_shadow FROM public;\n",
		NULL
	};

	PG_CMD_DECL;

	fputs(_("initializing pg_shadow ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = pg_shadow_setup; *line != NULL; line++)
		PG_CMD_PUTLINE;

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * get the superuser password if required, and call postgres to set it
 */
static void
get_set_pwd(void)
{
	PG_CMD_DECL_NOLINE;

	char	   *pwd1,
			   *pwd2;
	char		pwdpath[MAXPGPATH];
	struct stat statbuf;

	pwd1 = simple_prompt("Enter new superuser password: ", 100, false);
	pwd2 = simple_prompt("Enter it again: ", 100, false);
	if (strcmp(pwd1, pwd2) != 0)
	{
		fprintf(stderr, _("Passwords didn't match.\n"));
		exit_nicely();
	}
	free(pwd2);

	printf(_("setting password ... "));
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	if (fprintf(pg,
		  "ALTER USER \"%s\" WITH PASSWORD '%s';\n", username, pwd1) < 0)
	{
		/* write failure */
		exit_nicely();
	}
	fflush(pg);

	PG_CMD_CLOSE;

	snprintf(pwdpath, sizeof(pwdpath), "%s/global/pg_pwd", pg_data);
	if (stat(pwdpath, &statbuf) != 0 || !S_ISREG(statbuf.st_mode))
	{
		fprintf(stderr,
				_("%s: The password file was not generated. "
				"Please report this problem.\n"),
				progname);
		exit_nicely();
	}

	check_ok();
}

/*
 * toast sys tables
 */
static void
unlimit_systables(void)
{
	char	   *systables_setup[] = {
		"ALTER TABLE pg_attrdef CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_constraint CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_database CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_description CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_group CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_proc CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_rewrite CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_shadow CREATE TOAST TABLE;\n",
		"ALTER TABLE pg_statistic CREATE TOAST TABLE;\n",
		NULL
	};

	PG_CMD_DECL;

	fputs(_("enabling unlimited row size for system tables ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = systables_setup; *line != NULL; line++)
		PG_CMD_PUTLINE;

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * set up pg_depend
 */
static void
setup_depend(void)
{
	char	   *pg_depend_setup[] = {
		/*
		 * Make PIN entries in pg_depend for all objects made so far in
		 * the tables that the dependency code handles.  This is overkill
		 * (the system doesn't really depend on having every last weird
		 * datatype, for instance) but generating only the minimum
		 * required set of dependencies seems hard.
		 *
		 * Note that we deliberately do not pin the system views, which
		 * haven't been created yet.
		 *
		 * First delete any already-made entries; PINs override all else, and
		 * must be the only entries for their objects.
		 */
		"DELETE FROM pg_depend;\n",
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
		" FROM pg_rewrite;\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_trigger;\n",

		/*
		 * restriction here to avoid pinning the public namespace
		 */
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_namespace "
		"    WHERE nspname LIKE 'pg%';\n",
		NULL
	};

	PG_CMD_DECL;

	fputs(_("initializing pg_depend ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = pg_depend_setup; *line != NULL; line++)
		PG_CMD_PUTLINE;

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

	char	  **sysviews_setup;

	fputs(_("creating system views ... "), stdout);
	fflush(stdout);

	sysviews_setup = readfile(system_views_file);

	/*
	 * We use -N here to avoid backslashing stuff in system_views.sql
	 */
	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s -N template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = sysviews_setup; *line != NULL; line++)
	{
		PG_CMD_PUTLINE;
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
	PG_CMD_DECL_NOLINE;
	int			fres;

	fputs(_("loading pg_description ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	fres = fprintf(pg,
				   "CREATE TEMP TABLE tmp_pg_description ( "
				   "	objoid oid, "
				   "	classname name, "
				   "	objsubid int4, "
				   "	description text) WITHOUT OIDS;\n");
	if (fres < 0)
		exit_nicely();

	fres = fprintf(pg,
				   "COPY tmp_pg_description FROM '%s';\n",
				   desc_file);
	if (fres < 0)
		exit_nicely();

	fres = fprintf(pg,
				   "INSERT INTO pg_description "
				   " SELECT t.objoid, c.oid, t.objsubid, t.description "
				   "  FROM tmp_pg_description t, pg_class c "
				   "    WHERE c.relname = t.classname;\n");
	if (fres < 0)
		exit_nicely();

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
			PG_CMD_PUTLINE;

		free(*line);
	}

	free(conv_lines);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * Set up privileges
 *
 * We set most system catalogs and built-in functions as world-accessible.
 * Some objects may require different permissions by default, so we
 * make sure we don't overwrite privilege sets that have already been
 * set (NOT NULL).
 */
static void
setup_privileges(void)
{
	char	   *privileges_setup[] = {
		"UPDATE pg_class "
		"  SET relacl = '{\"=r/\\\\\"$POSTGRES_SUPERUSERNAME\\\\\"\"}' "
		"  WHERE relkind IN ('r', 'v', 'S') AND relacl IS NULL;\n",
		"UPDATE pg_proc "
		"  SET proacl = '{\"=X/\\\\\"$POSTGRES_SUPERUSERNAME\\\\\"\"}' "
		"  WHERE proacl IS NULL;\n",
		"UPDATE pg_language "
		"  SET lanacl = '{\"=U/\\\\\"$POSTGRES_SUPERUSERNAME\\\\\"\"}' "
		"  WHERE lanpltrusted;\n",
		"GRANT USAGE ON SCHEMA pg_catalog TO PUBLIC;\n",
		"GRANT CREATE, USAGE ON SCHEMA public TO PUBLIC;\n",
		NULL
	};

	PG_CMD_DECL;

	char	  **priv_lines;

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
		PG_CMD_PUTLINE;

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
	char	  **lines;
	int			fres;

	fputs(_("creating information schema ... "), stdout);
	fflush(stdout);

	lines = readfile(info_schema_file);

	/*
	 * We use -N here to avoid backslashing stuff in information_schema.sql
	 */
	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s -N template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = lines; *line != NULL; line++)
	{
		PG_CMD_PUTLINE;
		free(*line);
	}

	free(lines);

	PG_CMD_CLOSE;

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	fres = fprintf(pg,
				   "UPDATE information_schema.sql_implementation_info "
				   "  SET character_value = '%s' "
				   "  WHERE implementation_info_name = 'DBMS VERSION';\n",
				   infoversion);
	if (fres < 0)
		exit_nicely();

	fres = fprintf(pg,
				   "COPY information_schema.sql_features "
				   "  (feature_id, feature_name, sub_feature_id, "
				   "  sub_feature_name, is_supported, comments) "
				   " FROM '%s';\n",
				   features_file);
	if (fres < 0)
		exit_nicely();

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * clean everything up in template1
 */
static void
vacuum_db(void)
{
	PG_CMD_DECL_NOLINE;

	fputs(_("vacuuming database template1 ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	if (fputs("ANALYZE;\nVACUUM FULL FREEZE;\n", pg) < 0)
		exit_nicely();
	fflush(pg);

	PG_CMD_CLOSE;

	check_ok();
}

/*
 * copy template1 to template0
 */
static void
make_template0(void)
{
	char	   *template0_setup[] = {
		"CREATE DATABASE template0;\n",
		"UPDATE pg_database SET "
		"	datistemplate = 't', "
		"	datallowconn = 'f' "
		"    WHERE datname = 'template0';\n",

		/*
		 * We use the OID of template0 to determine lastsysoid
		 */
		"UPDATE pg_database SET datlastsysoid = "
		"    (SELECT oid::int4 - 1 FROM pg_database "
		"    WHERE datname = 'template0');\n",

		/*
		 * Explicitly revoke public create-schema and create-temp-table
		 * privileges in template1 and template0; else the latter would be
		 * on by default
		 */
		"REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;\n",
		"REVOKE CREATE,TEMPORARY ON DATABASE template0 FROM public;\n",

		/*
		 * Finally vacuum to clean up dead rows in pg_database
		 */
		"VACUUM FULL pg_database;\n",
		NULL
	};

	PG_CMD_DECL;

	fputs(_("copying template1 to template0 ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	for (line = template0_setup; *line; line++)
		PG_CMD_PUTLINE;

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
check_ok()
{
	if (caught_signal)
	{
		printf(_("caught signal\n"));
		exit_nicely();
	}
	else if (output_failed)
	{
		printf(_("could not write to child process\n"));
		exit_nicely();
	}
	else
	{
		/* all seems well */
		printf(_("ok\n"));
	}
}


/*
 * check if given string is a valid locale specifier
 * based on some code given to me by Peter Eisentraut
 * (but I take responsibility for it :-)
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
	 * override absent/invalid config settings from initdb's locale
	 * settings
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
#ifdef LC_MESSAGES
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
	printf(_("  -U, --username=NAME       database superuser name\n"));
	printf(_("  -W, --pwprompt            prompt for a password for the new superuser\n"));
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
		{"pwprompt", no_argument, NULL, 'W'},
		{"username", required_argument, NULL, 'U'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"debug", no_argument, NULL, 'd'},
		{"show", no_argument, NULL, 's'},
		{"noclean", no_argument, NULL, 'n'},
		{0, 0, 0, 0}
	};

	int			c,
				i,
				ret;
	int			option_index;
	char	   *short_version;
	char	   *pgdenv;			/* PGDATA value got from sent to
								 * environment */
	char	   *subdirs[] =
	{"global", "pg_xlog", "pg_clog", "base", "base/1"};

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

	while ((c = getopt_long(argc, argv, "dD:E:L:nU:W", long_options, &option_index)) != -1)
	{
		switch (c)
		{
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
			case 's':
				show_setting = true;
				break;
			default:
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

	canonicalize_path(pg_data);

	/*
	 * we have to set PGDATA for postgres rather than pass it on the
	 * commnd line to avoid dumb quoting problems on Windows, and we would
	 * expecially need quotes otherwise on Windows because paths there are
	 * most likely to have embedded spaces.
	 */
	pgdenv = xmalloc(8 + strlen(pg_data));
	sprintf(pgdenv, "PGDATA=%s", pg_data);
	putenv(pgdenv);

	if ((ret = find_other_exec(argv[0], "postgres", PG_VERSIONSTR,
							   backend_exec)) < 0)
	{
		if (ret == -1)
			fprintf(stderr,
						_("The program \"postgres\" is needed by %s "
						"but was not found in the same directory as \"%s\".\n"
						"Check your installation.\n"),
						progname, progname);
		else
			fprintf(stderr,
						_("The program \"postgres\" was found by %s "
						"but was not the same version as \"%s\".\n"
						"Check your installation.\n"),
						progname, progname);
		exit(1);
	}

	/* store binary directory */
	strcpy(bin_path, backend_exec);
	*last_path_separator(bin_path) = '\0';

	if (!share_path)
	{
		share_path = xmalloc(MAXPGPATH);
		get_share_path(backend_exec, share_path);
	}

	canonicalize_path(share_path);

	if ((short_version = get_short_version()) == NULL)
	{
		fprintf(stderr, _("%s: could not determine valid short version string\n"), progname);
		exit(1);
	}

	effective_user = get_id();
	if (!strlen(username))
		username = effective_user;

	if (strlen(encoding))
		encodingid = get_encoding_id(encoding);

	set_input(&bki_file, "postgres.bki");
	set_input(&desc_file, "postgres.description");
	set_input(&hba_file, "pg_hba.conf.sample");
	set_input(&ident_file, "pg_ident.conf.sample");
	set_input(&conf_file, "postgresql.conf.sample");
	set_input(&conversion_file, "conversion_create.sql");
	set_input(&info_schema_file, "information_schema.sql");
	set_input(&features_file, "sql_features.txt");
	set_input(&system_views_file, "system_views.sql");

	set_info_version();

	if (show_setting || debug)
	{
		fprintf(stderr,
				"VERSION=%s\n"
				"PGDATA=%s\nshare_path=%s\nPGPATH=%s\n"
				"ENCODING=%s\nENCODINGID=%s\n"
				"POSTGRES_SUPERUSERNAME=%s\nPOSTGRES_BKI=%s\n"
				"POSTGRES_DESCR=%s\nPOSTGRESQL_CONF_SAMPLE=%s\n"
				"PG_HBA_SAMPLE=%s\nPG_IDENT_SAMPLE=%s\n",
				PG_VERSION,
				pg_data, share_path, bin_path,
				encoding, encodingid,
				username, bki_file,
				desc_file, conf_file,
				hba_file, ident_file);
		if (show_setting)
			exit(0);
	}

	check_input(bki_file);
	check_input(desc_file);
	check_input(hba_file);
	check_input(ident_file);
	check_input(conf_file);
	check_input(conversion_file);
	check_input(info_schema_file);
	check_input(features_file);
	check_input(system_views_file);

	printf(_("The files belonging to this database system will be owned "
		   "by user \"%s\".\n"
		   "This user must also own the server process.\n\n"),
		   effective_user);

	setlocales();

	if (strcmp(lc_ctype, lc_collate) == 0 &&
		strcmp(lc_ctype, lc_time) == 0 &&
		strcmp(lc_ctype, lc_numeric) == 0 &&
		strcmp(lc_ctype, lc_monetary) == 0 &&
		strcmp(lc_ctype, lc_messages) == 0)
	{
		printf(_("The database cluster will be initialized with locale %s.\n\n"),
			   lc_ctype);
	}
	else
	{
		printf(_("The database cluster will be initialized with locales\n"
			   "  COLLATE:  %s\n"
			   "  CTYPE:    %s\n"
			   "  MESSAGES: %s\n"
			   "  MONETARY: %s\n"
			   "  NUMERIC:  %s\n"
			   "  TIME:     %s\n\n"),
			   lc_collate,
			   lc_ctype,
			   lc_messages,
			   lc_monetary,
			   lc_numeric,
			   lc_time);
	}

	umask(077);

	/*
	 * now we are starting to do real work, trap signals so we can clean
	 * up
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

	switch (check_data_dir())
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
				perror(pg_data);
				exit_nicely();
			}
			else
				check_ok();

			found_existing_pgdata = true;
			break;

		case 2:
			/* Present and not empty */
			fprintf(stderr,
					_("%s: directory \"%s\" exists but is not empty\n"
					"If you want to create a new database system, either remove or empty\n"
					"the directory \"%s\" or run %s\n"
					"with an argument other than \"%s\".\n"),
					progname, pg_data, pg_data, progname, pg_data);
			exit(1);			/* no further message needed */

		default:
			/* Trouble accessing directory */
			perror(pg_data);
			exit_nicely();
	}

	/* Create required subdirectories */

	for (i = 0; i < (sizeof(subdirs) / sizeof(char *)); i++)
	{
		printf(_("creating directory %s/%s ... "), pg_data, subdirs[i]);
		fflush(stdout);

		if (!mkdatadir(subdirs[i]))
			exit_nicely();
		else
			check_ok();
	}

	/* Top level PG_VERSION is checked by bootstrapper, so make it first */
	set_short_version(short_version, NULL);

	/*
	 * Determine platform-specific config settings
	 *
	 * Use reasonable values if kernel will let us, else scale back.  Probe for
	 * max_connections first since it is subject to more constraints than
	 * shared_buffers.
	 */

	set_null_conf();

	test_connections();
	test_buffers();

	/* Now create all the text config files */
	setup_config();

	/* Bootstrap template1 */
	bootstrap_template1(short_version);

	/* Make the per-database PGVERSION for template1 only after init'ing it */
	set_short_version(short_version, "base/1");

	/* Create the stuff we don't need to use bootstrap mode for */

	setup_shadow();
	if (pwprompt)
		get_set_pwd();

	unlimit_systables();

	setup_depend();

	setup_sysviews();

	setup_description();

	setup_conversion();

	setup_privileges();

	setup_schema();

	vacuum_db();

	make_template0();

	printf(_("\nSuccess. You can now start the database server using:\n\n"
		   "    %s%s%s/postmaster -D %s%s%s\n"
		   "or\n"
		   "    %s%s%s/pg_ctl -D %s%s%s -l logfile start\n\n"),
		 QUOTE_PATH, bin_path, QUOTE_PATH, QUOTE_PATH, pg_data, QUOTE_PATH,
		QUOTE_PATH, bin_path, QUOTE_PATH, QUOTE_PATH, pg_data, QUOTE_PATH);

	return 0;
}
