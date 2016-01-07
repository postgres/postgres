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
 * by the basic bootstrap process.  After it is complete, template0 and
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
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/initdb/initdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <time.h>

#ifdef HAVE_SHM_OPEN
#include "sys/mman.h"
#endif

#include "catalog/catalog.h"
#include "common/restricted_token.h"
#include "common/username.h"
#include "mb/pg_wchar.h"
#include "getaddrinfo.h"
#include "getopt_long.h"
#include "miscadmin.h"


/* Define PG_FLUSH_DATA_WORKS if we have an implementation for pg_flush_data */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif

/* Ideally this would be in a .h file, but it hardly seems worth the trouble */
extern const char *select_default_timezone(const char *share_path);

static const char *const auth_methods_host[] = {
	"trust", "reject", "md5", "password", "ident", "radius",
#ifdef ENABLE_GSS
	"gss",
#endif
#ifdef ENABLE_SSPI
	"sspi",
#endif
#ifdef USE_PAM
	"pam", "pam ",
#endif
#ifdef USE_LDAP
	"ldap",
#endif
#ifdef USE_SSL
	"cert",
#endif
	NULL
};
static const char *const auth_methods_local[] = {
	"trust", "reject", "md5", "password", "peer", "radius",
#ifdef USE_PAM
	"pam", "pam ",
#endif
#ifdef USE_LDAP
	"ldap",
#endif
	NULL
};

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
static const char *authmethodhost = "";
static const char *authmethodlocal = "";
static bool debug = false;
static bool noclean = false;
static bool do_sync = true;
static bool sync_only = false;
static bool show_setting = false;
static bool data_checksums = false;
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
static char *pgdata_native;

/* defaults */
static int	n_connections = 10;
static int	n_buffers = 50;
static char *dynamic_shared_memory_type = NULL;

/*
 * Warning messages for authentication methods
 */
#define AUTHTRUST_WARNING \
"# CAUTION: Configuring the system for local \"trust\" authentication\n" \
"# allows any local user to connect as any PostgreSQL user, including\n" \
"# the database superuser.  If you do not trust all your local users,\n" \
"# use another authentication method.\n"
static char *authwarning = NULL;

/*
 * Centralized knowledge of switches to pass to backend
 *
 * Note: we run the backend with -F (fsync disabled) and then do a single
 * pass of fsync'ing at the end.  This is faster than fsync'ing each step.
 *
 * Note: in the shell-script version, we also passed PGDATA as a -D switch,
 * but here it is more convenient to pass it as an environment variable
 * (no quoting to worry about).
 */
static const char *boot_options = "-F";
static const char *backend_options = "--single -F -O -j -c search_path=pg_catalog -c exit_on_error=true";

static const char *const subdirs[] = {
	"global",
	"pg_xlog/archive_status",
	"pg_clog",
	"pg_commit_ts",
	"pg_dynshmem",
	"pg_notify",
	"pg_serial",
	"pg_snapshots",
	"pg_subtrans",
	"pg_twophase",
	"pg_multixact",
	"pg_multixact/members",
	"pg_multixact/offsets",
	"base",
	"base/1",
	"pg_replslot",
	"pg_tblspc",
	"pg_stat",
	"pg_stat_tmp",
	"pg_logical",
	"pg_logical/snapshots",
	"pg_logical/mappings"
};


/* path to 'initdb' binary directory */
static char bin_path[MAXPGPATH];
static char backend_exec[MAXPGPATH];

static char **replace_token(char **lines,
			  const char *token, const char *replacement);

#ifndef HAVE_UNIX_SOCKETS
static char **filter_lines_with_token(char **lines, const char *token);
#endif
static char **readfile(const char *path);
static void writefile(char *path, char **lines);
static void walkdir(const char *path,
		void (*action) (const char *fname, bool isdir),
		bool process_symlinks);
#ifdef PG_FLUSH_DATA_WORKS
static void pre_sync_fname(const char *fname, bool isdir);
#endif
static void fsync_fname_ext(const char *fname, bool isdir);
static FILE *popen_check(const char *command, const char *mode);
static void exit_nicely(void);
static char *get_id(void);
static char *get_encoding_id(char *encoding_name);
static void set_input(char **dest, char *filename);
static void check_input(char *path);
static void write_version_file(char *extrapath);
static void set_null_conf(void);
static void test_config_settings(void);
static void setup_config(void);
static void bootstrap_template1(void);
static void setup_auth(FILE *cmdfd);
static void get_set_pwd(FILE *cmdfd);
static void setup_depend(FILE *cmdfd);
static void setup_sysviews(FILE *cmdfd);
static void setup_description(FILE *cmdfd);
static void setup_collation(FILE *cmdfd);
static void setup_conversion(FILE *cmdfd);
static void setup_dictionary(FILE *cmdfd);
static void setup_privileges(FILE *cmdfd);
static void set_info_version(void);
static void setup_schema(FILE *cmdfd);
static void load_plpgsql(FILE *cmdfd);
static void vacuum_db(FILE *cmdfd);
static void make_template0(FILE *cmdfd);
static void make_postgres(FILE *cmdfd);
static void fsync_pgdata(void);
static void trapsig(int signum);
static void check_ok(void);
static char *escape_quotes(const char *src);
static int	locale_date_order(const char *locale);
static void check_locale_name(int category, const char *locale,
				  char **canonname);
static bool check_locale_encoding(const char *locale, int encoding);
static void setlocales(void);
static void usage(const char *progname);
void		setup_pgdata(void);
void		setup_bin_paths(const char *argv0);
void		setup_data_file_paths(void);
void		setup_locale_encoding(void);
void		setup_signals(void);
void		setup_text_search(void);
void		create_data_directory(void);
void		create_xlog_or_symlink(void);
void		warn_on_mount_point(int error);
void		initialize_data_directory(void);

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

#define PG_CMD_PRINTF3(fmt, arg1, arg2, arg3)		\
do { \
	if (fprintf(cmdfd, fmt, arg1, arg2, arg3) < 0 || fflush(cmdfd) < 0) \
		output_failed = true, output_errno = errno; \
} while (0)

#ifndef WIN32
#define QUOTE_PATH	""
#define DIR_SEP "/"
#else
#define QUOTE_PATH	"\""
#define DIR_SEP "\\"
#endif

static char *
escape_quotes(const char *src)
{
	char	   *result = escape_single_quotes_ascii(src);

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

		memcpy(newline, lines[i], pre);

		memcpy(newline + pre, replacement, replen);

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
readfile(const char *path)
{
	FILE	   *infile;
	int			maxlength = 1,
				linelen = 0;
	int			nlines = 0;
	int			n;
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
	result = (char **) pg_malloc((nlines + 1) * sizeof(char *));
	buffer = (char *) pg_malloc(maxlength + 1);

	/* now reprocess the file and store the lines */
	rewind(infile);
	n = 0;
	while (fgets(buffer, maxlength + 1, infile) != NULL && n < nlines)
		result[n++] = pg_strdup(buffer);

	fclose(infile);
	free(buffer);
	result[n] = NULL;

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
 * walkdir: recursively walk a directory, applying the action to each
 * regular file and directory (including the named directory itself).
 *
 * If process_symlinks is true, the action and recursion are also applied
 * to regular files and directories that are pointed to by symlinks in the
 * given directory; otherwise symlinks are ignored.  Symlinks are always
 * ignored in subdirectories, ie we intentionally don't pass down the
 * process_symlinks flag to recursive calls.
 *
 * Errors are reported but not considered fatal.
 *
 * See also walkdir in fd.c, which is a backend version of this logic.
 */
static void
walkdir(const char *path,
		void (*action) (const char *fname, bool isdir),
		bool process_symlinks)
{
	DIR		   *dir;
	struct dirent *de;

	dir = opendir(path);
	if (dir == NULL)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\": %s\n"),
				progname, path, strerror(errno));
		return;
	}

	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		char		subpath[MAXPGPATH];
		struct stat fst;
		int			sret;

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, MAXPGPATH, "%s/%s", path, de->d_name);

		if (process_symlinks)
			sret = stat(subpath, &fst);
		else
			sret = lstat(subpath, &fst);

		if (sret < 0)
		{
			fprintf(stderr, _("%s: could not stat file \"%s\": %s\n"),
					progname, subpath, strerror(errno));
			continue;
		}

		if (S_ISREG(fst.st_mode))
			(*action) (subpath, false);
		else if (S_ISDIR(fst.st_mode))
			walkdir(subpath, action, false);
	}

	if (errno)
		fprintf(stderr, _("%s: could not read directory \"%s\": %s\n"),
				progname, path, strerror(errno));

	(void) closedir(dir);

	/*
	 * It's important to fsync the destination directory itself as individual
	 * file fsyncs don't guarantee that the directory entry for the file is
	 * synced.  Recent versions of ext4 have made the window much wider but
	 * it's been an issue for ext3 and other filesystems in the past.
	 */
	(*action) (path, true);
}

/*
 * Hint to the OS that it should get ready to fsync() this file.
 *
 * Ignores errors trying to open unreadable files, and reports other errors
 * non-fatally.
 */
#ifdef PG_FLUSH_DATA_WORKS

static void
pre_sync_fname(const char *fname, bool isdir)
{
	int			fd;

	fd = open(fname, O_RDONLY | PG_BINARY);

	if (fd < 0)
	{
		if (errno == EACCES || (isdir && errno == EISDIR))
			return;
		fprintf(stderr, _("%s: could not open file \"%s\": %s\n"),
				progname, fname, strerror(errno));
		return;
	}

	/*
	 * We do what pg_flush_data() would do in the backend: prefer to use
	 * sync_file_range, but fall back to posix_fadvise.  We ignore errors
	 * because this is only a hint.
	 */
#if defined(HAVE_SYNC_FILE_RANGE)
	(void) sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	(void) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#else
#error PG_FLUSH_DATA_WORKS should not have been defined
#endif

	(void) close(fd);
}

#endif   /* PG_FLUSH_DATA_WORKS */

/*
 * fsync_fname_ext -- Try to fsync a file or directory
 *
 * Ignores errors trying to open unreadable files, or trying to fsync
 * directories on systems where that isn't allowed/required.  Reports
 * other errors non-fatally.
 */
static void
fsync_fname_ext(const char *fname, bool isdir)
{
	int			fd;
	int			flags;
	int			returncode;

	/*
	 * Some OSs require directories to be opened read-only whereas other
	 * systems don't allow us to fsync files opened read-only; so we need both
	 * cases here.  Using O_RDWR will cause us to fail to fsync files that are
	 * not writable by our userid, but we assume that's OK.
	 */
	flags = PG_BINARY;
	if (!isdir)
		flags |= O_RDWR;
	else
		flags |= O_RDONLY;

	/*
	 * Open the file, silently ignoring errors about unreadable files (or
	 * unsupported operations, e.g. opening a directory under Windows), and
	 * logging others.
	 */
	fd = open(fname, flags);
	if (fd < 0)
	{
		if (errno == EACCES || (isdir && errno == EISDIR))
			return;
		fprintf(stderr, _("%s: could not open file \"%s\": %s\n"),
				progname, fname, strerror(errno));
		return;
	}

	returncode = fsync(fd);

	/*
	 * Some OSes don't allow us to fsync directories at all, so we can ignore
	 * those errors. Anything else needs to be reported.
	 */
	if (returncode != 0 && !(isdir && errno == EBADF))
		fprintf(stderr, _("%s: could not fsync file \"%s\": %s\n"),
				progname, fname, strerror(errno));

	(void) close(fd);
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
 * on unix make sure it isn't root
 */
static char *
get_id(void)
{
	const char *username;

#ifndef WIN32
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
#endif

	username = get_user_name_or_exit(progname);

	return pg_strdup(username);
}

static char *
encodingid_to_string(int enc)
{
	char		result[20];

	sprintf(result, "%d", enc);
	return pg_strdup(result);
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
	 * underscore (usual case) or a hyphen (Windows "locale name"; see
	 * comments at IsoLocaleName()).
	 *
	 * XXX Should ' ' be a stop character?	This would select "norwegian" for
	 * the Windows locale "Norwegian (Nynorsk)_Norway.1252".  If we do so, we
	 * should also accept the "nn" and "nb" Unix locales.
	 *
	 * Just for paranoia, we also stop at '.' or '@'.
	 */
	if (lc_type == NULL)
		langname = pg_strdup("");
	else
	{
		ptr = langname = pg_strdup(lc_type);
		while (*ptr &&
			   *ptr != '_' && *ptr != '-' && *ptr != '.' && *ptr != '@')
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
 * set name of given input file variable under data directory
 */
static void
set_input(char **dest, char *filename)
{
	*dest = psprintf("%s/%s", share_path, filename);
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
write_version_file(char *extrapath)
{
	FILE	   *version_file;
	char	   *path;

	if (extrapath == NULL)
		path = psprintf("%s/PG_VERSION", pg_data);
	else
		path = psprintf("%s/%s/PG_VERSION", pg_data, extrapath);

	if ((version_file = fopen(path, PG_BINARY_W)) == NULL)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}
	if (fprintf(version_file, "%s\n", PG_MAJORVERSION) < 0 ||
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

	path = psprintf("%s/postgresql.conf", pg_data);
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
 * Determine which dynamic shared memory implementation should be used on
 * this platform.  POSIX shared memory is preferable because the default
 * allocation limits are much higher than the limits for System V on most
 * systems that support both, but the fact that a platform has shm_open
 * doesn't guarantee that that call will succeed when attempted.  So, we
 * attempt to reproduce what the postmaster will do when allocating a POSIX
 * segment in dsm_impl.c; if it doesn't work, we assume it won't work for
 * the postmaster either, and configure the cluster for System V shared
 * memory instead.
 */
static char *
choose_dsm_implementation(void)
{
#ifdef HAVE_SHM_OPEN
	int			ntries = 10;

	while (ntries > 0)
	{
		uint32		handle;
		char		name[64];
		int			fd;

		handle = random();
		snprintf(name, 64, "/PostgreSQL.%u", handle);
		if ((fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600)) != -1)
		{
			close(fd);
			shm_unlink(name);
			return "posix";
		}
		if (errno != EEXIST)
			break;
		--ntries;
	}
#endif

#ifdef WIN32
	return "windows";
#else
	return "sysv";
#endif
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
	 * This macro defines the minimum shared_buffers we want for a given
	 * max_connections value. The arrays show the settings to try.
	 */
#define MIN_BUFS_FOR_CONNS(nconns)	((nconns) * 10)

	static const int trial_conns[] = {
		100, 50, 40, 30, 20, 10
	};
	static const int trial_bufs[] = {
		16384, 8192, 4096, 3584, 3072, 2560, 2048, 1536,
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
				ok_buffers = 0;


	printf(_("selecting default max_connections ... "));
	fflush(stdout);

	for (i = 0; i < connslen; i++)
	{
		test_conns = trial_conns[i];
		test_buffs = MIN_BUFS_FOR_CONNS(test_conns);

		snprintf(cmd, sizeof(cmd),
				 "\"%s\" --boot -x0 %s "
				 "-c max_connections=%d "
				 "-c shared_buffers=%d "
				 "-c dynamic_shared_memory_type=none "
				 "< \"%s\" > \"%s\" 2>&1",
				 backend_exec, boot_options,
				 test_conns, test_buffs,
				 DEVNULL, DEVNULL);
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

	printf(_("selecting default shared_buffers ... "));
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

		snprintf(cmd, sizeof(cmd),
				 "\"%s\" --boot -x0 %s "
				 "-c max_connections=%d "
				 "-c shared_buffers=%d "
				 "-c dynamic_shared_memory_type=none "
				 "< \"%s\" > \"%s\" 2>&1",
				 backend_exec, boot_options,
				 n_connections, test_buffs,
				 DEVNULL, DEVNULL);
		status = system(cmd);
		if (status == 0)
			break;
	}
	n_buffers = test_buffs;

	if ((n_buffers * (BLCKSZ / 1024)) % 1024 == 0)
		printf("%dMB\n", (n_buffers * (BLCKSZ / 1024)) / 1024);
	else
		printf("%dkB\n", n_buffers * (BLCKSZ / 1024));

	printf(_("selecting dynamic shared memory implementation ... "));
	fflush(stdout);
	dynamic_shared_memory_type = choose_dsm_implementation();
	printf("%s\n", dynamic_shared_memory_type);
}

/*
 * set up all the config files
 */
static void
setup_config(void)
{
	char	  **conflines;
	char		repltok[MAXPGPATH];
	char		path[MAXPGPATH];
	const char *default_timezone;
	char	   *autoconflines[3];

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

#ifdef HAVE_UNIX_SOCKETS
	snprintf(repltok, sizeof(repltok), "#unix_socket_directories = '%s'",
			 DEFAULT_PGSOCKET_DIR);
#else
	snprintf(repltok, sizeof(repltok), "#unix_socket_directories = ''");
#endif
	conflines = replace_token(conflines, "#unix_socket_directories = '/tmp'",
							  repltok);

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

	default_timezone = select_default_timezone(share_path);
	if (default_timezone)
	{
		snprintf(repltok, sizeof(repltok), "timezone = '%s'",
				 escape_quotes(default_timezone));
		conflines = replace_token(conflines, "#timezone = 'GMT'", repltok);
		snprintf(repltok, sizeof(repltok), "log_timezone = '%s'",
				 escape_quotes(default_timezone));
		conflines = replace_token(conflines, "#log_timezone = 'GMT'", repltok);
	}

	snprintf(repltok, sizeof(repltok), "dynamic_shared_memory_type = %s",
			 dynamic_shared_memory_type);
	conflines = replace_token(conflines, "#dynamic_shared_memory_type = posix",
							  repltok);

#ifndef USE_PREFETCH
	conflines = replace_token(conflines,
							  "#effective_io_concurrency = 1",
							  "#effective_io_concurrency = 0");
#endif

	snprintf(path, sizeof(path), "%s/postgresql.conf", pg_data);

	writefile(path, conflines);
	if (chmod(path, S_IRUSR | S_IWUSR) != 0)
	{
		fprintf(stderr, _("%s: could not change permissions of \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}

	/*
	 * create the automatic configuration file to store the configuration
	 * parameters set by ALTER SYSTEM command. The parameters present in this
	 * file will override the value of parameters that exists before parse of
	 * this file.
	 */
	autoconflines[0] = pg_strdup("# Do not edit this file manually!\n");
	autoconflines[1] = pg_strdup("# It will be overwritten by the ALTER SYSTEM command.\n");
	autoconflines[2] = NULL;

	sprintf(path, "%s/postgresql.auto.conf", pg_data);

	writefile(path, autoconflines);
	if (chmod(path, S_IRUSR | S_IWUSR) != 0)
	{
		fprintf(stderr, _("%s: could not change permissions of \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}

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
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = 0;
		hints.ai_protocol = 0;
		hints.ai_addrlen = 0;
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		if (err != 0 ||
			getaddrinfo("::1", NULL, &hints, &gai_result) != 0)
			conflines = replace_token(conflines,
							   "host    all             all             ::1",
							 "#host    all             all             ::1");
	}
#else							/* !HAVE_IPV6 */
	/* If we didn't compile IPV6 support at all, always comment it out */
	conflines = replace_token(conflines,
							  "host    all             all             ::1",
							  "#host    all             all             ::1");
#endif   /* HAVE_IPV6 */

	/* Replace default authentication methods */
	conflines = replace_token(conflines,
							  "@authmethodhost@",
							  authmethodhost);
	conflines = replace_token(conflines,
							  "@authmethodlocal@",
							  authmethodlocal);

	conflines = replace_token(conflines,
							  "@authcomment@",
							  (strcmp(authmethodlocal, "trust") == 0 || strcmp(authmethodhost, "trust") == 0) ? AUTHTRUST_WARNING : "");

	/* Replace username for replication */
	conflines = replace_token(conflines,
							  "@default_username@",
							  username);

	snprintf(path, sizeof(path), "%s/pg_hba.conf", pg_data);

	writefile(path, conflines);
	if (chmod(path, S_IRUSR | S_IWUSR) != 0)
	{
		fprintf(stderr, _("%s: could not change permissions of \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}

	free(conflines);

	/* pg_ident.conf */

	conflines = readfile(ident_file);

	snprintf(path, sizeof(path), "%s/pg_ident.conf", pg_data);

	writefile(path, conflines);
	if (chmod(path, S_IRUSR | S_IWUSR) != 0)
	{
		fprintf(stderr, _("%s: could not change permissions of \"%s\": %s\n"),
				progname, path, strerror(errno));
		exit_nicely();
	}

	free(conflines);

	check_ok();
}


/*
 * run the BKI script in bootstrap mode to create template1
 */
static void
bootstrap_template1(void)
{
	PG_CMD_DECL;
	char	  **line;
	char	   *talkargs = "";
	char	  **bki_lines;
	char		headerline[MAXPGPATH];
	char		buf[64];

	printf(_("running bootstrap script ... "));
	fflush(stdout);

	if (debug)
		talkargs = "-d 5";

	bki_lines = readfile(bki_file);

	/* Check that bki file appears to be of the right version */

	snprintf(headerline, sizeof(headerline), "# PostgreSQL %s\n",
			 PG_MAJORVERSION);

	if (strcmp(headerline, *bki_lines) != 0)
	{
		fprintf(stderr,
				_("%s: input file \"%s\" does not belong to PostgreSQL %s\n"
				  "Check your installation or specify the correct path "
				  "using the option -L.\n"),
				progname, bki_file, PG_VERSION);
		exit_nicely();
	}

	/* Substitute for various symbols used in the BKI file */

	sprintf(buf, "%d", NAMEDATALEN);
	bki_lines = replace_token(bki_lines, "NAMEDATALEN", buf);

	sprintf(buf, "%d", (int) sizeof(Pointer));
	bki_lines = replace_token(bki_lines, "SIZEOF_POINTER", buf);

	bki_lines = replace_token(bki_lines, "ALIGNOF_POINTER",
							  (sizeof(Pointer) == 4) ? "i" : "d");

	bki_lines = replace_token(bki_lines, "FLOAT4PASSBYVAL",
							  FLOAT4PASSBYVAL ? "true" : "false");

	bki_lines = replace_token(bki_lines, "FLOAT8PASSBYVAL",
							  FLOAT8PASSBYVAL ? "true" : "false");

	bki_lines = replace_token(bki_lines, "POSTGRES", escape_quotes(username));

	bki_lines = replace_token(bki_lines, "ENCODING", encodingid);

	bki_lines = replace_token(bki_lines, "LC_COLLATE", escape_quotes(lc_collate));

	bki_lines = replace_token(bki_lines, "LC_CTYPE", escape_quotes(lc_ctype));

	/*
	 * Pass correct LC_xxx environment to bootstrap.
	 *
	 * The shell script arranged to restore the LC settings afterwards, but
	 * there doesn't seem to be any compelling reason to do that.
	 */
	snprintf(cmd, sizeof(cmd), "LC_COLLATE=%s", lc_collate);
	putenv(pg_strdup(cmd));

	snprintf(cmd, sizeof(cmd), "LC_CTYPE=%s", lc_ctype);
	putenv(pg_strdup(cmd));

	unsetenv("LC_ALL");

	/* Also ensure backend isn't confused by this environment var: */
	unsetenv("PGCLIENTENCODING");

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" --boot -x1 %s %s %s",
			 backend_exec,
			 data_checksums ? "-k" : "",
			 boot_options, talkargs);

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
setup_auth(FILE *cmdfd)
{
	const char *const * line;
	static const char *const pg_authid_setup[] = {
		/*
		 * The authid table shouldn't be readable except through views, to
		 * ensure passwords are not publicly visible.
		 */
		"REVOKE ALL on pg_authid FROM public;\n\n",
		NULL
	};

	for (line = pg_authid_setup; *line != NULL; line++)
		PG_CMD_PUTS(*line);
}

/*
 * get the superuser password if required, and call postgres to set it
 */
static void
get_set_pwd(FILE *cmdfd)
{
	char	   *pwd1,
			   *pwd2;

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
			if (ferror(pwf))
				fprintf(stderr, _("%s: could not read password from file \"%s\": %s\n"),
						progname, pwfilename, strerror(errno));
			else
				fprintf(stderr, _("%s: password file \"%s\" is empty\n"),
						progname, pwfilename);
			exit_nicely();
		}
		fclose(pwf);

		i = strlen(pwdbuf);
		while (i > 0 && (pwdbuf[i - 1] == '\r' || pwdbuf[i - 1] == '\n'))
			pwdbuf[--i] = '\0';

		pwd1 = pg_strdup(pwdbuf);

	}

	PG_CMD_PRINTF2("ALTER USER \"%s\" WITH PASSWORD E'%s';\n\n",
				   username, escape_quotes(pwd1));

	free(pwd1);
}

/*
 * set up pg_depend
 */
static void
setup_depend(FILE *cmdfd)
{
	const char *const * line;
	static const char *const pg_depend_setup[] = {
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
		"DELETE FROM pg_depend;\n\n",
		"VACUUM pg_depend;\n\n",
		"DELETE FROM pg_shdepend;\n\n",
		"VACUUM pg_shdepend;\n\n",

		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_class;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_proc;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_type;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_cast;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_constraint;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_attrdef;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_language;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_operator;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_opclass;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_opfamily;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_amop;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_amproc;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_rewrite;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_trigger;\n\n",

		/*
		 * restriction here to avoid pinning the public namespace
		 */
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_namespace "
		"    WHERE nspname LIKE 'pg%';\n\n",

		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_parser;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_dict;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_template;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_ts_config;\n\n",
		"INSERT INTO pg_depend SELECT 0,0,0, tableoid,oid,0, 'p' "
		" FROM pg_collation;\n\n",
		"INSERT INTO pg_shdepend SELECT 0,0,0,0, tableoid,oid, 'p' "
		" FROM pg_authid;\n\n",
		NULL
	};

	for (line = pg_depend_setup; *line != NULL; line++)
		PG_CMD_PUTS(*line);
}

/*
 * set up system views
 */
static void
setup_sysviews(FILE *cmdfd)
{
	char	  **line;
	char	  **sysviews_setup;

	sysviews_setup = readfile(system_views_file);

	for (line = sysviews_setup; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	free(sysviews_setup);
}

/*
 * load description data
 */
static void
setup_description(FILE *cmdfd)
{
	PG_CMD_PUTS("CREATE TEMP TABLE tmp_pg_description ( "
				"	objoid oid, "
				"	classname name, "
				"	objsubid int4, "
				"	description text) WITHOUT OIDS;\n\n");

	PG_CMD_PRINTF1("COPY tmp_pg_description FROM E'%s';\n\n",
				   escape_quotes(desc_file));

	PG_CMD_PUTS("INSERT INTO pg_description "
				" SELECT t.objoid, c.oid, t.objsubid, t.description "
				"  FROM tmp_pg_description t, pg_class c "
				"    WHERE c.relname = t.classname;\n\n");

	PG_CMD_PUTS("CREATE TEMP TABLE tmp_pg_shdescription ( "
				" objoid oid, "
				" classname name, "
				" description text) WITHOUT OIDS;\n\n");

	PG_CMD_PRINTF1("COPY tmp_pg_shdescription FROM E'%s';\n\n",
				   escape_quotes(shdesc_file));

	PG_CMD_PUTS("INSERT INTO pg_shdescription "
				" SELECT t.objoid, c.oid, t.description "
				"  FROM tmp_pg_shdescription t, pg_class c "
				"   WHERE c.relname = t.classname;\n\n");

	/* Create default descriptions for operator implementation functions */
	PG_CMD_PUTS("WITH funcdescs AS ( "
				"SELECT p.oid as p_oid, oprname, "
			  "coalesce(obj_description(o.oid, 'pg_operator'),'') as opdesc "
				"FROM pg_proc p JOIN pg_operator o ON oprcode = p.oid ) "
				"INSERT INTO pg_description "
				"  SELECT p_oid, 'pg_proc'::regclass, 0, "
				"    'implementation of ' || oprname || ' operator' "
				"  FROM funcdescs "
				"  WHERE opdesc NOT LIKE 'deprecated%' AND "
				"  NOT EXISTS (SELECT 1 FROM pg_description "
		"    WHERE objoid = p_oid AND classoid = 'pg_proc'::regclass);\n\n");

	/*
	 * Even though the tables are temp, drop them explicitly so they don't get
	 * copied into template0/postgres databases.
	 */
	PG_CMD_PUTS("DROP TABLE tmp_pg_description;\n\n");
	PG_CMD_PUTS("DROP TABLE tmp_pg_shdescription;\n\n");
}

#ifdef HAVE_LOCALE_T
/*
 * "Normalize" a locale name, stripping off encoding tags such as
 * ".utf8" (e.g., "en_US.utf8" -> "en_US", but "br_FR.iso885915@euro"
 * -> "br_FR@euro").  Return true if a new, different name was
 * generated.
 */
static bool
normalize_locale_name(char *new, const char *old)
{
	char	   *n = new;
	const char *o = old;
	bool		changed = false;

	while (*o)
	{
		if (*o == '.')
		{
			/* skip over encoding tag such as ".utf8" or ".UTF-8" */
			o++;
			while ((*o >= 'A' && *o <= 'Z')
				   || (*o >= 'a' && *o <= 'z')
				   || (*o >= '0' && *o <= '9')
				   || (*o == '-'))
				o++;
			changed = true;
		}
		else
			*n++ = *o++;
	}
	*n = '\0';

	return changed;
}
#endif   /* HAVE_LOCALE_T */

/*
 * populate pg_collation
 */
static void
setup_collation(FILE *cmdfd)
{
#if defined(HAVE_LOCALE_T) && !defined(WIN32)
	int			i;
	FILE	   *locale_a_handle;
	char		localebuf[NAMEDATALEN]; /* we assume ASCII so this is fine */
	int			count = 0;

	locale_a_handle = popen_check("locale -a", "r");
	if (!locale_a_handle)
		return;					/* complaint already printed */

	PG_CMD_PUTS("CREATE TEMP TABLE tmp_pg_collation ( "
				"	collname name, "
				"	locale name, "
				"	encoding int) WITHOUT OIDS;\n\n");

	while (fgets(localebuf, sizeof(localebuf), locale_a_handle))
	{
		size_t		len;
		int			enc;
		bool		skip;
		char	   *quoted_locale;
		char		alias[NAMEDATALEN];

		len = strlen(localebuf);

		if (len == 0 || localebuf[len - 1] != '\n')
		{
			if (debug)
				fprintf(stderr, _("%s: locale name too long, skipped: \"%s\"\n"),
						progname, localebuf);
			continue;
		}
		localebuf[len - 1] = '\0';

		/*
		 * Some systems have locale names that don't consist entirely of ASCII
		 * letters (such as "bokm&aring;l" or "fran&ccedil;ais").  This is
		 * pretty silly, since we need the locale itself to interpret the
		 * non-ASCII characters. We can't do much with those, so we filter
		 * them out.
		 */
		skip = false;
		for (i = 0; i < len; i++)
		{
			if (IS_HIGHBIT_SET(localebuf[i]))
			{
				skip = true;
				break;
			}
		}
		if (skip)
		{
			if (debug)
				fprintf(stderr, _("%s: locale name has non-ASCII characters, skipped: \"%s\"\n"),
						progname, localebuf);
			continue;
		}

		enc = pg_get_encoding_from_locale(localebuf, debug);
		if (enc < 0)
		{
			/* error message printed by pg_get_encoding_from_locale() */
			continue;
		}
		if (!PG_VALID_BE_ENCODING(enc))
			continue;			/* ignore locales for client-only encodings */
		if (enc == PG_SQL_ASCII)
			continue;			/* C/POSIX are already in the catalog */

		count++;

		quoted_locale = escape_quotes(localebuf);

		PG_CMD_PRINTF3("INSERT INTO tmp_pg_collation VALUES (E'%s', E'%s', %d);\n\n",
					   quoted_locale, quoted_locale, enc);

		/*
		 * Generate aliases such as "en_US" in addition to "en_US.utf8" for
		 * ease of use.  Note that collation names are unique per encoding
		 * only, so this doesn't clash with "en_US" for LATIN1, say.
		 */
		if (normalize_locale_name(alias, localebuf))
		{
			char	   *quoted_alias = escape_quotes(alias);

			PG_CMD_PRINTF3("INSERT INTO tmp_pg_collation VALUES (E'%s', E'%s', %d);\n\n",
						   quoted_alias, quoted_locale, enc);
			free(quoted_alias);
		}
		free(quoted_locale);
	}

	/* Add an SQL-standard name */
	PG_CMD_PRINTF1("INSERT INTO tmp_pg_collation VALUES ('ucs_basic', 'C', %d);\n\n", PG_UTF8);

	/*
	 * When copying collations to the final location, eliminate aliases that
	 * conflict with an existing locale name for the same encoding.  For
	 * example, "br_FR.iso88591" is normalized to "br_FR", both for encoding
	 * LATIN1.  But the unnormalized locale "br_FR" already exists for LATIN1.
	 * Prefer the alias that matches the OS locale name, else the first locale
	 * name by sort order (arbitrary choice to be deterministic).
	 *
	 * Also, eliminate any aliases that conflict with pg_collation's
	 * hard-wired entries for "C" etc.
	 */
	PG_CMD_PUTS("INSERT INTO pg_collation (collname, collnamespace, collowner, collencoding, collcollate, collctype) "
				" SELECT DISTINCT ON (collname, encoding)"
				"   collname, "
				"   (SELECT oid FROM pg_namespace WHERE nspname = 'pg_catalog') AS collnamespace, "
				"   (SELECT relowner FROM pg_class WHERE relname = 'pg_collation') AS collowner, "
				"   encoding, locale, locale "
				"  FROM tmp_pg_collation"
				"  WHERE NOT EXISTS (SELECT 1 FROM pg_collation WHERE collname = tmp_pg_collation.collname)"
	 "  ORDER BY collname, encoding, (collname = locale) DESC, locale;\n\n");

	/*
	 * Even though the table is temp, drop it explicitly so it doesn't get
	 * copied into template0/postgres databases.
	 */
	PG_CMD_PUTS("DROP TABLE tmp_pg_collation;\n\n");

	pclose(locale_a_handle);

	if (count == 0 && !debug)
	{
		printf(_("No usable system locales were found.\n"));
		printf(_("Use the option \"--debug\" to see details.\n"));
	}
#endif   /* not HAVE_LOCALE_T  && not WIN32 */
}

/*
 * load conversion functions
 */
static void
setup_conversion(FILE *cmdfd)
{
	char	  **line;
	char	  **conv_lines;

	conv_lines = readfile(conversion_file);
	for (line = conv_lines; *line != NULL; line++)
	{
		if (strstr(*line, "DROP CONVERSION") != *line)
			PG_CMD_PUTS(*line);
		free(*line);
	}

	free(conv_lines);
}

/*
 * load extra dictionaries (Snowball stemmers)
 */
static void
setup_dictionary(FILE *cmdfd)
{
	char	  **line;
	char	  **conv_lines;

	conv_lines = readfile(dictionary_file);
	for (line = conv_lines; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	free(conv_lines);
}

/*
 * Set up privileges
 *
 * We mark most system catalogs as world-readable.  We don't currently have
 * to touch functions, languages, or databases, because their default
 * permissions are OK.
 *
 * Some objects may require different permissions by default, so we
 * make sure we don't overwrite privilege sets that have already been
 * set (NOT NULL).
 */
static void
setup_privileges(FILE *cmdfd)
{
	char	  **line;
	char	  **priv_lines;
	static char *privileges_setup[] = {
		"UPDATE pg_class "
		"  SET relacl = E'{\"=r/\\\\\"$POSTGRES_SUPERUSERNAME\\\\\"\"}' "
		"  WHERE relkind IN ('r', 'v', 'm', 'S') AND relacl IS NULL;\n\n",
		"GRANT USAGE ON SCHEMA pg_catalog TO PUBLIC;\n\n",
		"GRANT CREATE, USAGE ON SCHEMA public TO PUBLIC;\n\n",
		"REVOKE ALL ON pg_largeobject FROM PUBLIC;\n\n",
		NULL
	};

	priv_lines = replace_token(privileges_setup, "$POSTGRES_SUPERUSERNAME",
							   escape_quotes(username));
	for (line = priv_lines; *line != NULL; line++)
		PG_CMD_PUTS(*line);
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
	char	   *vstr = pg_strdup(PG_VERSION);
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
setup_schema(FILE *cmdfd)
{
	char	  **line;
	char	  **lines;

	lines = readfile(info_schema_file);

	for (line = lines; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	free(lines);

	PG_CMD_PRINTF1("UPDATE information_schema.sql_implementation_info "
				   "  SET character_value = '%s' "
				   "  WHERE implementation_info_name = 'DBMS VERSION';\n\n",
				   infoversion);

	PG_CMD_PRINTF1("COPY information_schema.sql_features "
				   "  (feature_id, feature_name, sub_feature_id, "
				   "  sub_feature_name, is_supported, comments) "
				   " FROM E'%s';\n\n",
				   escape_quotes(features_file));
}

/*
 * load PL/pgsql server-side language
 */
static void
load_plpgsql(FILE *cmdfd)
{
	PG_CMD_PUTS("CREATE EXTENSION plpgsql;\n\n");
}

/*
 * clean everything up in template1
 */
static void
vacuum_db(FILE *cmdfd)
{
	/* Run analyze before VACUUM so the statistics are frozen. */
	PG_CMD_PUTS("ANALYZE;\n\nVACUUM FREEZE;\n\n");
}

/*
 * copy template1 to template0
 */
static void
make_template0(FILE *cmdfd)
{
	const char *const * line;
	static const char *const template0_setup[] = {
		"CREATE DATABASE template0 IS_TEMPLATE = true ALLOW_CONNECTIONS = false;\n\n",

		/*
		 * We use the OID of template0 to determine lastsysoid
		 */
		"UPDATE pg_database SET datlastsysoid = "
		"    (SELECT oid FROM pg_database "
		"    WHERE datname = 'template0');\n\n",

		/*
		 * Explicitly revoke public create-schema and create-temp-table
		 * privileges in template1 and template0; else the latter would be on
		 * by default
		 */
		"REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;\n\n",
		"REVOKE CREATE,TEMPORARY ON DATABASE template0 FROM public;\n\n",

		"COMMENT ON DATABASE template0 IS 'unmodifiable empty database';\n\n",

		/*
		 * Finally vacuum to clean up dead rows in pg_database
		 */
		"VACUUM pg_database;\n\n",
		NULL
	};

	for (line = template0_setup; *line; line++)
		PG_CMD_PUTS(*line);
}

/*
 * copy template1 to postgres
 */
static void
make_postgres(FILE *cmdfd)
{
	const char *const * line;
	static const char *const postgres_setup[] = {
		"CREATE DATABASE postgres;\n\n",
		"COMMENT ON DATABASE postgres IS 'default administrative connection database';\n\n",
		NULL
	};

	for (line = postgres_setup; *line; line++)
		PG_CMD_PUTS(*line);
}

/*
 * Issue fsync recursively on PGDATA and all its contents.
 *
 * We fsync regular files and directories wherever they are, but we
 * follow symlinks only for pg_xlog and immediately under pg_tblspc.
 * Other symlinks are presumed to point at files we're not responsible
 * for fsyncing, and might not have privileges to write at all.
 *
 * Errors are reported but not considered fatal.
 */
static void
fsync_pgdata(void)
{
	bool		xlog_is_symlink;
	char		pg_xlog[MAXPGPATH];
	char		pg_tblspc[MAXPGPATH];

	fputs(_("syncing data to disk ... "), stdout);
	fflush(stdout);

	snprintf(pg_xlog, MAXPGPATH, "%s/pg_xlog", pg_data);
	snprintf(pg_tblspc, MAXPGPATH, "%s/pg_tblspc", pg_data);

	/*
	 * If pg_xlog is a symlink, we'll need to recurse into it separately,
	 * because the first walkdir below will ignore it.
	 */
	xlog_is_symlink = false;

#ifndef WIN32
	{
		struct stat st;

		if (lstat(pg_xlog, &st) < 0)
			fprintf(stderr, _("%s: could not stat file \"%s\": %s\n"),
					progname, pg_xlog, strerror(errno));
		else if (S_ISLNK(st.st_mode))
			xlog_is_symlink = true;
	}
#else
	if (pgwin32_is_junction(pg_xlog))
		xlog_is_symlink = true;
#endif

	/*
	 * If possible, hint to the kernel that we're soon going to fsync the data
	 * directory and its contents.
	 */
#ifdef PG_FLUSH_DATA_WORKS
	walkdir(pg_data, pre_sync_fname, false);
	if (xlog_is_symlink)
		walkdir(pg_xlog, pre_sync_fname, false);
	walkdir(pg_tblspc, pre_sync_fname, true);
#endif

	/*
	 * Now we do the fsync()s in the same order.
	 *
	 * The main call ignores symlinks, so in addition to specially processing
	 * pg_xlog if it's a symlink, pg_tblspc has to be visited separately with
	 * process_symlinks = true.  Note that if there are any plain directories
	 * in pg_tblspc, they'll get fsync'd twice.  That's not an expected case
	 * so we don't worry about optimizing it.
	 */
	walkdir(pg_data, fsync_fname_ext, false);
	if (xlog_is_symlink)
		walkdir(pg_xlog, fsync_fname_ext, false);
	walkdir(pg_tblspc, fsync_fname_ext, true);

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
	save = pg_strdup(save);

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
 * Verify that locale name is valid for the locale category.
 *
 * If successful, and canonname isn't NULL, a malloc'd copy of the locale's
 * canonical name is stored there.  This is especially useful for figuring out
 * what locale name "" means (ie, the environment value).  (Actually,
 * it seems that on most implementations that's the only thing it's good for;
 * we could wish that setlocale gave back a canonically spelled version of
 * the locale name, but typically it doesn't.)
 *
 * this should match the backend's check_locale() function
 */
static void
check_locale_name(int category, const char *locale, char **canonname)
{
	char	   *save;
	char	   *res;

	if (canonname)
		*canonname = NULL;		/* in case of failure */

	save = setlocale(category, NULL);
	if (!save)
	{
		fprintf(stderr, _("%s: setlocale() failed\n"),
				progname);
		exit(1);
	}

	/* save may be pointing at a modifiable scratch variable, so copy it. */
	save = pg_strdup(save);

	/* set the locale with setlocale, to see if it accepts it. */
	res = setlocale(category, locale);

	/* save canonical name if requested. */
	if (res && canonname)
		*canonname = pg_strdup(res);

	/* restore old value. */
	if (!setlocale(category, save))
	{
		fprintf(stderr, _("%s: failed to restore old locale \"%s\"\n"),
				progname, save);
		exit(1);
	}
	free(save);

	/* complain if locale wasn't valid */
	if (res == NULL)
	{
		if (*locale)
			fprintf(stderr, _("%s: invalid locale name \"%s\"\n"),
					progname, locale);
		else
		{
			/*
			 * If no relevant switch was given on command line, locale is an
			 * empty string, which is not too helpful to report.  Presumably
			 * setlocale() found something it did not like in the environment.
			 * Ideally we'd report the bad environment variable, but since
			 * setlocale's behavior is implementation-specific, it's hard to
			 * be sure what it didn't like.  Print a safe generic message.
			 */
			fprintf(stderr, _("%s: invalid locale settings; check LANG and LC_* environment variables\n"),
					progname);
		}
		exit(1);
	}
}

/*
 * check if the chosen encoding matches the encoding required by the locale
 *
 * this should match the similar check in the backend createdb() function
 */
static bool
check_locale_encoding(const char *locale, int user_enc)
{
	int			locale_enc;

	locale_enc = pg_get_encoding_from_locale(locale, true);

	/* See notes in createdb() to understand these tests */
	if (!(locale_enc == user_enc ||
		  locale_enc == PG_SQL_ASCII ||
		  locale_enc == -1 ||
#ifdef WIN32
		  user_enc == PG_UTF8 ||
#endif
		  user_enc == PG_SQL_ASCII))
	{
		fprintf(stderr, _("%s: encoding mismatch\n"), progname);
		fprintf(stderr,
				_("The encoding you selected (%s) and the encoding that the\n"
			  "selected locale uses (%s) do not match.  This would lead to\n"
			"misbehavior in various character string processing functions.\n"
			   "Rerun %s and either do not specify an encoding explicitly,\n"
				  "or choose a matching combination.\n"),
				pg_encoding_to_char(user_enc),
				pg_encoding_to_char(locale_enc),
				progname);
		return false;
	}
	return true;
}

/*
 * set up the locale variables
 *
 * assumes we have called setlocale(LC_ALL, "") -- see set_pglocale_pgservice
 */
static void
setlocales(void)
{
	char	   *canonname;

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
	 * canonicalize locale names, and obtain any missing values from our
	 * current environment
	 */

	check_locale_name(LC_CTYPE, lc_ctype, &canonname);
	lc_ctype = canonname;
	check_locale_name(LC_COLLATE, lc_collate, &canonname);
	lc_collate = canonname;
	check_locale_name(LC_NUMERIC, lc_numeric, &canonname);
	lc_numeric = canonname;
	check_locale_name(LC_TIME, lc_time, &canonname);
	lc_time = canonname;
	check_locale_name(LC_MONETARY, lc_monetary, &canonname);
	lc_monetary = canonname;
#if defined(LC_MESSAGES) && !defined(WIN32)
	check_locale_name(LC_MESSAGES, lc_messages, &canonname);
	lc_messages = canonname;
#else
	/* when LC_MESSAGES is not available, use the LC_CTYPE setting */
	check_locale_name(LC_CTYPE, lc_messages, &canonname);
	lc_messages = canonname;
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
	printf(_("  -A, --auth=METHOD         default authentication method for local connections\n"));
	printf(_("      --auth-host=METHOD    default authentication method for local TCP/IP connections\n"));
	printf(_("      --auth-local=METHOD   default authentication method for local-socket connections\n"));
	printf(_(" [-D, --pgdata=]DATADIR     location for this database cluster\n"));
	printf(_("  -E, --encoding=ENCODING   set default encoding for new databases\n"));
	printf(_("      --locale=LOCALE       set default locale for new databases\n"));
	printf(_("      --lc-collate=, --lc-ctype=, --lc-messages=LOCALE\n"
			 "      --lc-monetary=, --lc-numeric=, --lc-time=LOCALE\n"
			 "                            set default locale in the respective category for\n"
			 "                            new databases (default taken from environment)\n"));
	printf(_("      --no-locale           equivalent to --locale=C\n"));
	printf(_("      --pwfile=FILE         read password for the new superuser from file\n"));
	printf(_("  -T, --text-search-config=CFG\n"
		 "                            default text search configuration\n"));
	printf(_("  -U, --username=NAME       database superuser name\n"));
	printf(_("  -W, --pwprompt            prompt for a password for the new superuser\n"));
	printf(_("  -X, --xlogdir=XLOGDIR     location for the transaction log directory\n"));
	printf(_("\nLess commonly used options:\n"));
	printf(_("  -d, --debug               generate lots of debugging output\n"));
	printf(_("  -k, --data-checksums      use data page checksums\n"));
	printf(_("  -L DIRECTORY              where to find the input files\n"));
	printf(_("  -n, --noclean             do not clean up after errors\n"));
	printf(_("  -N, --nosync              do not wait for changes to be written safely to disk\n"));
	printf(_("  -s, --show                show internal settings\n"));
	printf(_("  -S, --sync-only           only sync data directory\n"));
	printf(_("\nOther options:\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nIf the data directory is not specified, the environment variable PGDATA\n"
			 "is used.\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}

static void
check_authmethod_unspecified(const char **authmethod)
{
	if (*authmethod == NULL || strlen(*authmethod) == 0)
	{
		authwarning = _("\nWARNING: enabling \"trust\" authentication for local connections\n"
						"You can change this by editing pg_hba.conf or using the option -A, or\n"
			"--auth-local and --auth-host, the next time you run initdb.\n");
		*authmethod = "trust";
	}
}

static void
check_authmethod_valid(const char *authmethod, const char *const * valid_methods, const char *conntype)
{
	const char *const * p;

	for (p = valid_methods; *p; p++)
	{
		if (strcmp(authmethod, *p) == 0)
			return;
		/* with space = param */
		if (strchr(authmethod, ' '))
			if (strncmp(authmethod, *p, (authmethod - strchr(authmethod, ' '))) == 0)
				return;
	}

	fprintf(stderr, _("%s: invalid authentication method \"%s\" for \"%s\" connections\n"),
			progname, authmethod, conntype);
	exit(1);
}

static void
check_need_password(const char *authmethodlocal, const char *authmethodhost)
{
	if ((strcmp(authmethodlocal, "md5") == 0 ||
		 strcmp(authmethodlocal, "password") == 0) &&
		(strcmp(authmethodhost, "md5") == 0 ||
		 strcmp(authmethodhost, "password") == 0) &&
		!(pwprompt || pwfilename))
	{
		fprintf(stderr, _("%s: must specify a password for the superuser to enable %s authentication\n"), progname,
				(strcmp(authmethodlocal, "md5") == 0 ||
				 strcmp(authmethodlocal, "password") == 0)
				? authmethodlocal
				: authmethodhost);
		exit(1);
	}
}


void
setup_pgdata(void)
{
	char	   *pgdata_get_env,
			   *pgdata_set_env;

	if (strlen(pg_data) == 0)
	{
		pgdata_get_env = getenv("PGDATA");
		if (pgdata_get_env && strlen(pgdata_get_env))
		{
			/* PGDATA found */
			pg_data = pg_strdup(pgdata_get_env);
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

	pgdata_native = pg_strdup(pg_data);
	canonicalize_path(pg_data);

	/*
	 * we have to set PGDATA for postgres rather than pass it on the command
	 * line to avoid dumb quoting problems on Windows, and we would especially
	 * need quotes otherwise on Windows because paths there are most likely to
	 * have embedded spaces.
	 */
	pgdata_set_env = psprintf("PGDATA=%s", pg_data);
	putenv(pgdata_set_env);
}


void
setup_bin_paths(const char *argv0)
{
	int			ret;

	if ((ret = find_other_exec(argv0, "postgres", PG_BACKEND_VERSIONSTR,
							   backend_exec)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
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
}

void
setup_locale_encoding(void)
{
	int			user_enc;

	setlocales();

	if (strcmp(lc_ctype, lc_collate) == 0 &&
		strcmp(lc_ctype, lc_time) == 0 &&
		strcmp(lc_ctype, lc_numeric) == 0 &&
		strcmp(lc_ctype, lc_monetary) == 0 &&
		strcmp(lc_ctype, lc_messages) == 0)
		printf(_("The database cluster will be initialized with locale \"%s\".\n"), lc_ctype);
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

		ctype_enc = pg_get_encoding_from_locale(lc_ctype, true);

		if (ctype_enc == -1)
		{
			/* Couldn't recognize the locale's codeset */
			fprintf(stderr, _("%s: could not find suitable encoding for locale \"%s\"\n"),
					progname, lc_ctype);
			fprintf(stderr, _("Rerun %s with the -E option.\n"), progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
		else if (!pg_valid_server_encoding_id(ctype_enc))
		{
			/*
			 * We recognized it, but it's not a legal server encoding. On
			 * Windows, UTF-8 works with any locale, so we can fall back to
			 * UTF-8.
			 */
#ifdef WIN32
			printf(_("Encoding \"%s\" implied by locale is not allowed as a server-side encoding.\n"
			"The default database encoding will be set to \"%s\" instead.\n"),
				   pg_encoding_to_char(ctype_enc),
				   pg_encoding_to_char(PG_UTF8));
			ctype_enc = PG_UTF8;
			encodingid = encodingid_to_string(ctype_enc);
#else
			fprintf(stderr,
			   _("%s: locale \"%s\" requires unsupported encoding \"%s\"\n"),
					progname, lc_ctype, pg_encoding_to_char(ctype_enc));
			fprintf(stderr,
			  _("Encoding \"%s\" is not allowed as a server-side encoding.\n"
				"Rerun %s with a different locale selection.\n"),
					pg_encoding_to_char(ctype_enc), progname);
			exit(1);
#endif
		}
		else
		{
			encodingid = encodingid_to_string(ctype_enc);
			printf(_("The default database encoding has accordingly been set to \"%s\".\n"),
				   pg_encoding_to_char(ctype_enc));
		}
	}
	else
		encodingid = get_encoding_id(encoding);

	user_enc = atoi(encodingid);
	if (!check_locale_encoding(lc_ctype, user_enc) ||
		!check_locale_encoding(lc_collate, user_enc))
		exit(1);				/* check_locale_encoding printed the error */

}


void
setup_data_file_paths(void)
{
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
}


void
setup_text_search(void)
{
	if (strlen(default_text_search_config) == 0)
	{
		default_text_search_config = find_matching_ts_config(lc_ctype);
		if (default_text_search_config == NULL)
		{
			printf(_("%s: could not find suitable text search configuration for locale \"%s\"\n"),
				   progname, lc_ctype);
			default_text_search_config = "simple";
		}
	}
	else
	{
		const char *checkmatch = find_matching_ts_config(lc_ctype);

		if (checkmatch == NULL)
		{
			printf(_("%s: warning: suitable text search configuration for locale \"%s\" is unknown\n"),
				   progname, lc_ctype);
		}
		else if (strcmp(checkmatch, default_text_search_config) != 0)
		{
			printf(_("%s: warning: specified text search configuration \"%s\" might not match locale \"%s\"\n"),
				   progname, default_text_search_config, lc_ctype);
		}
	}

	printf(_("The default text search configuration will be set to \"%s\".\n"),
		   default_text_search_config);

}


void
setup_signals(void)
{
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

	/* Prevent SIGSYS so we can probe for kernel calls that might not work */
#ifdef SIGSYS
	pqsignal(SIGSYS, SIG_IGN);
#endif
}


void
create_data_directory(void)
{
	int			ret;

	switch ((ret = pg_check_dir(pg_data)))
	{
		case 0:
			/* PGDATA not there, must create it */
			printf(_("creating directory %s ... "),
				   pg_data);
			fflush(stdout);

			if (pg_mkdir_p(pg_data, S_IRWXU) != 0)
			{
				fprintf(stderr, _("%s: could not create directory \"%s\": %s\n"),
						progname, pg_data, strerror(errno));
				exit_nicely();
			}
			else
				check_ok();

			made_new_pgdata = true;
			break;

		case 1:
			/* Present but empty, fix permissions and use it */
			printf(_("fixing permissions on existing directory %s ... "),
				   pg_data);
			fflush(stdout);

			if (chmod(pg_data, S_IRWXU) != 0)
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
		case 3:
		case 4:
			/* Present and not empty */
			fprintf(stderr,
					_("%s: directory \"%s\" exists but is not empty\n"),
					progname, pg_data);
			if (ret != 4)
				warn_on_mount_point(ret);
			else
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
}


/* Create transaction log directory, and symlink if required */
void
create_xlog_or_symlink(void)
{
	char	   *subdirloc;

	/* form name of the place for the subdirectory or symlink */
	subdirloc = psprintf("%s/pg_xlog", pg_data);

	if (strcmp(xlog_dir, "") != 0)
	{
		int			ret;

		/* clean up xlog directory name, check it's absolute */
		canonicalize_path(xlog_dir);
		if (!is_absolute_path(xlog_dir))
		{
			fprintf(stderr, _("%s: transaction log directory location must be an absolute path\n"), progname);
			exit_nicely();
		}

		/* check if the specified xlog directory exists/is empty */
		switch ((ret = pg_check_dir(xlog_dir)))
		{
			case 0:
				/* xlog directory not there, must create it */
				printf(_("creating directory %s ... "),
					   xlog_dir);
				fflush(stdout);

				if (pg_mkdir_p(xlog_dir, S_IRWXU) != 0)
				{
					fprintf(stderr, _("%s: could not create directory \"%s\": %s\n"),
							progname, xlog_dir, strerror(errno));
					exit_nicely();
				}
				else
					check_ok();

				made_new_xlogdir = true;
				break;

			case 1:
				/* Present but empty, fix permissions and use it */
				printf(_("fixing permissions on existing directory %s ... "),
					   xlog_dir);
				fflush(stdout);

				if (chmod(xlog_dir, S_IRWXU) != 0)
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
			case 3:
			case 4:
				/* Present and not empty */
				fprintf(stderr,
						_("%s: directory \"%s\" exists but is not empty\n"),
						progname, xlog_dir);
				if (ret != 4)
					warn_on_mount_point(ret);
				else
					fprintf(stderr,
							_("If you want to store the transaction log there, either\n"
							  "remove or empty the directory \"%s\".\n"),
							xlog_dir);
				exit_nicely();

			default:
				/* Trouble accessing directory */
				fprintf(stderr, _("%s: could not access directory \"%s\": %s\n"),
						progname, xlog_dir, strerror(errno));
				exit_nicely();
		}

#ifdef HAVE_SYMLINK
		if (symlink(xlog_dir, subdirloc) != 0)
		{
			fprintf(stderr, _("%s: could not create symbolic link \"%s\": %s\n"),
					progname, subdirloc, strerror(errno));
			exit_nicely();
		}
#else
		fprintf(stderr, _("%s: symlinks are not supported on this platform"));
		exit_nicely();
#endif
	}
	else
	{
		/* Without -X option, just make the subdirectory normally */
		if (mkdir(subdirloc, S_IRWXU) < 0)
		{
			fprintf(stderr, _("%s: could not create directory \"%s\": %s\n"),
					progname, subdirloc, strerror(errno));
			exit_nicely();
		}
	}

	free(subdirloc);
}


void
warn_on_mount_point(int error)
{
	if (error == 2)
		fprintf(stderr,
				_("It contains a dot-prefixed/invisible file, perhaps due to it being a mount point.\n"));
	else if (error == 3)
		fprintf(stderr,
				_("It contains a lost+found directory, perhaps due to it being a mount point.\n"));

	fprintf(stderr,
			_("Using a mount point directly as the data directory is not recommended.\n"
			  "Create a subdirectory under the mount point.\n"));
}


void
initialize_data_directory(void)
{
	PG_CMD_DECL;
	int			i;

	setup_signals();

	umask(S_IRWXG | S_IRWXO);

	create_data_directory();

	create_xlog_or_symlink();

	/* Create required subdirectories (other than pg_xlog) */
	printf(_("creating subdirectories ... "));
	fflush(stdout);

	for (i = 0; i < lengthof(subdirs); i++)
	{
		char	   *path;

		path = psprintf("%s/%s", pg_data, subdirs[i]);

		/*
		 * The parent directory already exists, so we only need mkdir() not
		 * pg_mkdir_p() here, which avoids some failure modes; cf bug #13853.
		 */
		if (mkdir(path, S_IRWXU) < 0)
		{
			fprintf(stderr, _("%s: could not create directory \"%s\": %s\n"),
					progname, path, strerror(errno));
			exit_nicely();
		}

		free(path);
	}

	check_ok();

	/* Top level PG_VERSION is checked by bootstrapper, so make it first */
	write_version_file(NULL);

	/* Select suitable configuration settings */
	set_null_conf();
	test_config_settings();

	/* Now create all the text config files */
	setup_config();

	/* Bootstrap template1 */
	bootstrap_template1();

	/*
	 * Make the per-database PG_VERSION for template1 only after init'ing it
	 */
	write_version_file("base/1");

	/*
	 * Create the stuff we don't need to use bootstrap mode for, using a
	 * backend running in simple standalone mode.
	 */
	fputs(_("performing post-bootstrap initialization ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s template1 >%s",
			 backend_exec, backend_options,
			 DEVNULL);

	PG_CMD_OPEN;

	setup_auth(cmdfd);
	if (pwprompt || pwfilename)
		get_set_pwd(cmdfd);

	setup_depend(cmdfd);

	setup_sysviews(cmdfd);

	setup_description(cmdfd);

	setup_collation(cmdfd);

	setup_conversion(cmdfd);

	setup_dictionary(cmdfd);

	setup_privileges(cmdfd);

	setup_schema(cmdfd);

	load_plpgsql(cmdfd);

	vacuum_db(cmdfd);

	make_template0(cmdfd);

	make_postgres(cmdfd);

	PG_CMD_CLOSE;

	check_ok();
}


int
main(int argc, char *argv[])
{
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
		{"auth-local", required_argument, NULL, 10},
		{"auth-host", required_argument, NULL, 11},
		{"pwprompt", no_argument, NULL, 'W'},
		{"pwfile", required_argument, NULL, 9},
		{"username", required_argument, NULL, 'U'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"debug", no_argument, NULL, 'd'},
		{"show", no_argument, NULL, 's'},
		{"noclean", no_argument, NULL, 'n'},
		{"nosync", no_argument, NULL, 'N'},
		{"sync-only", no_argument, NULL, 'S'},
		{"xlogdir", required_argument, NULL, 'X'},
		{"data-checksums", no_argument, NULL, 'k'},
		{NULL, 0, NULL, 0}
	};

	/*
	 * options with no short version return a low integer, the rest return
	 * their short version value
	 */
	int			c;
	int			option_index;
	char	   *effective_user;
	char		bin_dir[MAXPGPATH];

	/*
	 * Ensure that buffering behavior of stdout and stderr matches what it is
	 * in interactive usage (at least on most platforms).  This prevents
	 * unexpected output ordering when, eg, output is redirected to a file.
	 * POSIX says we must do this before any other usage of these files.
	 */
	setvbuf(stdout, NULL, PG_IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("initdb"));

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

	while ((c = getopt_long(argc, argv, "dD:E:kL:nNU:WA:sST:X:", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'A':
				authmethodlocal = authmethodhost = pg_strdup(optarg);

				/*
				 * When ident is specified, use peer for local connections.
				 * Mirrored, when peer is specified, use ident for TCP/IP
				 * connections.
				 */
				if (strcmp(authmethodhost, "ident") == 0)
					authmethodlocal = "peer";
				else if (strcmp(authmethodlocal, "peer") == 0)
					authmethodhost = "ident";
				break;
			case 10:
				authmethodlocal = pg_strdup(optarg);
				break;
			case 11:
				authmethodhost = pg_strdup(optarg);
				break;
			case 'D':
				pg_data = pg_strdup(optarg);
				break;
			case 'E':
				encoding = pg_strdup(optarg);
				break;
			case 'W':
				pwprompt = true;
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'd':
				debug = true;
				printf(_("Running in debug mode.\n"));
				break;
			case 'n':
				noclean = true;
				printf(_("Running in noclean mode.  Mistakes will not be cleaned up.\n"));
				break;
			case 'N':
				do_sync = false;
				break;
			case 'S':
				sync_only = true;
				break;
			case 'k':
				data_checksums = true;
				break;
			case 'L':
				share_path = pg_strdup(optarg);
				break;
			case 1:
				locale = pg_strdup(optarg);
				break;
			case 2:
				lc_collate = pg_strdup(optarg);
				break;
			case 3:
				lc_ctype = pg_strdup(optarg);
				break;
			case 4:
				lc_monetary = pg_strdup(optarg);
				break;
			case 5:
				lc_numeric = pg_strdup(optarg);
				break;
			case 6:
				lc_time = pg_strdup(optarg);
				break;
			case 7:
				lc_messages = pg_strdup(optarg);
				break;
			case 8:
				locale = "C";
				break;
			case 9:
				pwfilename = pg_strdup(optarg);
				break;
			case 's':
				show_setting = true;
				break;
			case 'T':
				default_text_search_config = pg_strdup(optarg);
				break;
			case 'X':
				xlog_dir = pg_strdup(optarg);
				break;
			default:
				/* getopt_long already emitted a complaint */
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}


	/*
	 * Non-option argument specifies data directory as long as it wasn't
	 * already specified with -D / --pgdata
	 */
	if (optind < argc && strlen(pg_data) == 0)
	{
		pg_data = pg_strdup(argv[optind]);
		optind++;
	}

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/* If we only need to fsync, just do it and exit */
	if (sync_only)
	{
		setup_pgdata();

		/* must check that directory is readable */
		if (pg_check_dir(pg_data) <= 0)
		{
			fprintf(stderr, _("%s: could not access directory \"%s\": %s\n"),
					progname, pg_data, strerror(errno));
			exit_nicely();
		}

		fsync_pgdata();
		return 0;
	}

	if (pwprompt && pwfilename)
	{
		fprintf(stderr, _("%s: password prompt and password file cannot be specified together\n"), progname);
		exit(1);
	}

	check_authmethod_unspecified(&authmethodlocal);
	check_authmethod_unspecified(&authmethodhost);

	check_authmethod_valid(authmethodlocal, auth_methods_local, "local");
	check_authmethod_valid(authmethodhost, auth_methods_host, "host");

	check_need_password(authmethodlocal, authmethodhost);

	get_restricted_token(progname);

	setup_pgdata();

	setup_bin_paths(argv[0]);

	effective_user = get_id();
	if (strlen(username) == 0)
		username = effective_user;

	printf(_("The files belonging to this database system will be owned "
			 "by user \"%s\".\n"
			 "This user must also own the server process.\n\n"),
		   effective_user);

	set_info_version();

	setup_data_file_paths();

	setup_locale_encoding();

	setup_text_search();

	printf("\n");

	if (data_checksums)
		printf(_("Data page checksums are enabled.\n"));
	else
		printf(_("Data page checksums are disabled.\n"));

	printf("\n");

	initialize_data_directory();

	if (do_sync)
		fsync_pgdata();
	else
		printf(_("\nSync to disk skipped.\nThe data directory might become corrupt if the operating system crashes.\n"));

	if (authwarning != NULL)
		fprintf(stderr, "%s", authwarning);

	/* Get directory specification used to start this executable */
	strlcpy(bin_dir, argv[0], sizeof(bin_dir));
	get_parent_directory(bin_dir);

	printf(_("\nSuccess. You can now start the database server using:\n\n"
			 "    %s%s%spg_ctl%s -D %s%s%s -l logfile start\n\n"),
	   QUOTE_PATH, bin_dir, (strlen(bin_dir) > 0) ? DIR_SEP : "", QUOTE_PATH,
		   QUOTE_PATH, pgdata_native, QUOTE_PATH);

	return 0;
}
