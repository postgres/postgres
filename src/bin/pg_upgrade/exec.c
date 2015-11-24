/*
 *	exec.c
 *
 *	execution functions
 *
 *	Copyright (c) 2010-2015, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/exec.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include <fcntl.h>
#include <sys/types.h>

static void check_data_dir(const char *pg_data);
static void check_bin_dir(ClusterInfo *cluster);
static void validate_exec(const char *dir, const char *cmdName);

#ifdef WIN32
static int	win32_check_directory_write_permissions(void);
#endif


/*
 * exec_prog()
 *		Execute an external program with stdout/stderr redirected, and report
 *		errors
 *
 * Formats a command from the given argument list, logs it to the log file,
 * and attempts to execute that command.  If the command executes
 * successfully, exec_prog() returns true.
 *
 * If the command fails, an error message is saved to the specified log_file.
 * If throw_error is true, this raises a PG_FATAL error and pg_upgrade
 * terminates; otherwise it is just reported as PG_REPORT and exec_prog()
 * returns false.
 *
 * The code requires it be called first from the primary thread on Windows.
 */
bool
exec_prog(const char *log_file, const char *opt_log_file,
		  bool throw_error, const char *fmt,...)
{
	int			result = 0;
	int			written;

#define MAXCMDLEN (2 * MAXPGPATH)
	char		cmd[MAXCMDLEN];
	FILE	   *log;
	va_list		ap;

#ifdef WIN32
	static DWORD mainThreadId = 0;

	/* We assume we are called from the primary thread first */
	if (mainThreadId == 0)
		mainThreadId = GetCurrentThreadId();
#endif

	written = 0;
	va_start(ap, fmt);
	written += vsnprintf(cmd + written, MAXCMDLEN - written, fmt, ap);
	va_end(ap);
	if (written >= MAXCMDLEN)
		pg_fatal("command too long\n");
	written += snprintf(cmd + written, MAXCMDLEN - written,
						" >> \"%s\" 2>&1", log_file);
	if (written >= MAXCMDLEN)
		pg_fatal("command too long\n");

	pg_log(PG_VERBOSE, "%s\n", cmd);

#ifdef WIN32

	/*
	 * For some reason, Windows issues a file-in-use error if we write data to
	 * the log file from a non-primary thread just before we create a
	 * subprocess that also writes to the same log file.  One fix is to sleep
	 * for 100ms.  A cleaner fix is to write to the log file _after_ the
	 * subprocess has completed, so we do this only when writing from a
	 * non-primary thread.  fflush(), running system() twice, and pre-creating
	 * the file do not see to help.
	 */
	if (mainThreadId != GetCurrentThreadId())
		result = system(cmd);
#endif

	log = fopen(log_file, "a");

#ifdef WIN32
	{
		/*
		 * "pg_ctl -w stop" might have reported that the server has stopped
		 * because the postmaster.pid file has been removed, but "pg_ctl -w
		 * start" might still be in the process of closing and might still be
		 * holding its stdout and -l log file descriptors open.  Therefore,
		 * try to open the log file a few more times.
		 */
		int			iter;

		for (iter = 0; iter < 4 && log == NULL; iter++)
		{
			pg_usleep(1000000); /* 1 sec */
			log = fopen(log_file, "a");
		}
	}
#endif

	if (log == NULL)
		pg_fatal("cannot write to log file %s\n", log_file);

#ifdef WIN32
	/* Are we printing "command:" before its output? */
	if (mainThreadId == GetCurrentThreadId())
		fprintf(log, "\n\n");
#endif
	fprintf(log, "command: %s\n", cmd);
#ifdef WIN32
	/* Are we printing "command:" after its output? */
	if (mainThreadId != GetCurrentThreadId())
		fprintf(log, "\n\n");
#endif

	/*
	 * In Windows, we must close the log file at this point so the file is not
	 * open while the command is running, or we get a share violation.
	 */
	fclose(log);

#ifdef WIN32
	/* see comment above */
	if (mainThreadId == GetCurrentThreadId())
#endif
		result = system(cmd);

	if (result != 0)
	{
		/* we might be in on a progress status line, so go to the next line */
		report_status(PG_REPORT, "\n*failure*");
		fflush(stdout);

		pg_log(PG_VERBOSE, "There were problems executing \"%s\"\n", cmd);
		if (opt_log_file)
			pg_log(throw_error ? PG_FATAL : PG_REPORT,
				   "Consult the last few lines of \"%s\" or \"%s\" for\n"
				   "the probable cause of the failure.\n",
				   log_file, opt_log_file);
		else
			pg_log(throw_error ? PG_FATAL : PG_REPORT,
				   "Consult the last few lines of \"%s\" for\n"
				   "the probable cause of the failure.\n",
				   log_file);
	}

#ifndef WIN32

	/*
	 * We can't do this on Windows because it will keep the "pg_ctl start"
	 * output filename open until the server stops, so we do the \n\n above on
	 * that platform.  We use a unique filename for "pg_ctl start" that is
	 * never reused while the server is running, so it works fine.  We could
	 * log these commands to a third file, but that just adds complexity.
	 */
	if ((log = fopen(log_file, "a")) == NULL)
		pg_fatal("cannot write to log file %s\n", log_file);
	fprintf(log, "\n\n");
	fclose(log);
#endif

	return result == 0;
}


/*
 * pid_lock_file_exists()
 *
 * Checks whether the postmaster.pid file exists.
 */
bool
pid_lock_file_exists(const char *datadir)
{
	char		path[MAXPGPATH];
	int			fd;

	snprintf(path, sizeof(path), "%s/postmaster.pid", datadir);

	if ((fd = open(path, O_RDONLY, 0)) < 0)
	{
		/* ENOTDIR means we will throw a more useful error later */
		if (errno != ENOENT && errno != ENOTDIR)
			pg_fatal("could not open file \"%s\" for reading: %s\n",
					 path, getErrorText());

		return false;
	}

	close(fd);
	return true;
}


/*
 * verify_directories()
 *
 * does all the hectic work of verifying directories and executables
 * of old and new server.
 *
 * NOTE: May update the values of all parameters
 */
void
verify_directories(void)
{
#ifndef WIN32
	if (access(".", R_OK | W_OK | X_OK) != 0)
#else
	if (win32_check_directory_write_permissions() != 0)
#endif
		pg_fatal("You must have read and write access in the current directory.\n");

	check_bin_dir(&old_cluster);
	check_data_dir(old_cluster.pgdata);
	check_bin_dir(&new_cluster);
	check_data_dir(new_cluster.pgdata);
}


#ifdef WIN32
/*
 * win32_check_directory_write_permissions()
 *
 *	access() on WIN32 can't check directory permissions, so we have to
 *	optionally create, then delete a file to check.
 *		http://msdn.microsoft.com/en-us/library/1w06ktdy%28v=vs.80%29.aspx
 */
static int
win32_check_directory_write_permissions(void)
{
	int			fd;

	/*
	 * We open a file we would normally create anyway.  We do this even in
	 * 'check' mode, which isn't ideal, but this is the best we can do.
	 */
	if ((fd = open(GLOBALS_DUMP_FILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) < 0)
		return -1;
	close(fd);

	return unlink(GLOBALS_DUMP_FILE);
}
#endif


/*
 * check_data_dir()
 *
 *	This function validates the given cluster directory - we search for a
 *	small set of subdirectories that we expect to find in a valid $PGDATA
 *	directory.  If any of the subdirectories are missing (or secured against
 *	us) we display an error message and exit()
 *
 */
static void
check_data_dir(const char *pg_data)
{
	char		subDirName[MAXPGPATH];
	int			subdirnum;

	/* start check with top-most directory */
	const char *requiredSubdirs[] = {"", "base", "global", "pg_clog",
		"pg_multixact", "pg_subtrans", "pg_tblspc", "pg_twophase",
	"pg_xlog"};

	for (subdirnum = 0;
		 subdirnum < sizeof(requiredSubdirs) / sizeof(requiredSubdirs[0]);
		 ++subdirnum)
	{
		struct stat statBuf;

		snprintf(subDirName, sizeof(subDirName), "%s%s%s", pg_data,
		/* Win32 can't stat() a directory with a trailing slash. */
				 *requiredSubdirs[subdirnum] ? "/" : "",
				 requiredSubdirs[subdirnum]);

		if (stat(subDirName, &statBuf) != 0)
			report_status(PG_FATAL, "check for \"%s\" failed: %s\n",
						  subDirName, getErrorText());
		else if (!S_ISDIR(statBuf.st_mode))
			report_status(PG_FATAL, "%s is not a directory\n",
						  subDirName);
	}
}


/*
 * check_bin_dir()
 *
 *	This function searches for the executables that we expect to find
 *	in the binaries directory.  If we find that a required executable
 *	is missing (or secured against us), we display an error message and
 *	exit().
 */
static void
check_bin_dir(ClusterInfo *cluster)
{
	struct stat statBuf;

	/* check bindir */
	if (stat(cluster->bindir, &statBuf) != 0)
		report_status(PG_FATAL, "check for \"%s\" failed: %s\n",
					  cluster->bindir, getErrorText());
	else if (!S_ISDIR(statBuf.st_mode))
		report_status(PG_FATAL, "%s is not a directory\n",
					  cluster->bindir);

	validate_exec(cluster->bindir, "postgres");
	validate_exec(cluster->bindir, "pg_ctl");
	validate_exec(cluster->bindir, "pg_resetxlog");
	if (cluster == &new_cluster)
	{
		/* these are only needed in the new cluster */
		validate_exec(cluster->bindir, "psql");
		validate_exec(cluster->bindir, "pg_dump");
		validate_exec(cluster->bindir, "pg_dumpall");
	}
}


/*
 * validate_exec()
 *
 * validate "path" as an executable file
 */
static void
validate_exec(const char *dir, const char *cmdName)
{
	char		path[MAXPGPATH];
	struct stat buf;

	snprintf(path, sizeof(path), "%s/%s", dir, cmdName);

#ifdef WIN32
	/* Windows requires a .exe suffix for stat() */
	if (strlen(path) <= strlen(EXE_EXT) ||
		pg_strcasecmp(path + strlen(path) - strlen(EXE_EXT), EXE_EXT) != 0)
		strlcat(path, EXE_EXT, sizeof(path));
#endif

	/*
	 * Ensure that the file exists and is a regular file.
	 */
	if (stat(path, &buf) < 0)
		pg_fatal("check for \"%s\" failed: %s\n",
				 path, getErrorText());
	else if (!S_ISREG(buf.st_mode))
		pg_fatal("check for \"%s\" failed: not an executable file\n",
				 path);

	/*
	 * Ensure that the file is both executable and readable (required for
	 * dynamic loading).
	 */
#ifndef WIN32
	if (access(path, R_OK) != 0)
#else
	if ((buf.st_mode & S_IRUSR) == 0)
#endif
		pg_fatal("check for \"%s\" failed: cannot read file (permission denied)\n",
				 path);

#ifndef WIN32
	if (access(path, X_OK) != 0)
#else
	if ((buf.st_mode & S_IXUSR) == 0)
#endif
		pg_fatal("check for \"%s\" failed: cannot execute (permission denied)\n",
				 path);
}
