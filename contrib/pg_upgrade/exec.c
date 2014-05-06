/*
 *	exec.c
 *
 *	execution functions
 *
 *	Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/exec.c
 */

#include "pg_upgrade.h"

#include <fcntl.h>
#include <unistd.h>


static void check_data_dir(const char *pg_data);
static void check_bin_dir(ClusterInfo *cluster);
static void validate_exec(const char *dir, const char *cmdName);
#ifdef WIN32
static int win32_check_directory_write_permissions(void);
#endif


/*
 * exec_prog()
 *
 *	Formats a command from the given argument list and executes that
 *	command.  If the command executes, exec_prog() returns 1 otherwise
 *	exec_prog() logs an error message and returns 0.
 *
 *	If throw_error is TRUE, this function will throw a PG_FATAL error
 *	instead of returning should an error occur.
 */
int
exec_prog(bool throw_error, const char *fmt,...)
{
	va_list		args;
	int			result;
	char		cmd[MAXPGPATH];

	va_start(args, fmt);
	vsnprintf(cmd, MAXPGPATH, fmt, args);
	va_end(args);

	pg_log(PG_INFO, "%s\n", cmd);

	result = system(cmd);

	if (result != 0)
	{
		pg_log(throw_error ? PG_FATAL : PG_INFO,
			   "There were problems executing %s\n", cmd);
		return 1;
	}

	return 0;
}


/*
 * is_server_running()
 *
 * checks whether postmaster on the given data directory is running or not.
 * The check is performed by looking for the existence of postmaster.pid file.
 */
bool
is_server_running(const char *datadir)
{
	char		path[MAXPGPATH];
	int			fd;

	snprintf(path, sizeof(path), "%s/postmaster.pid", datadir);

	if ((fd = open(path, O_RDONLY, 0)) < 0)
	{
		/* ENOTDIR means we will throw a more useful error later */
		if (errno != ENOENT && errno != ENOTDIR)
			pg_log(PG_FATAL, "could not open file \"%s\" for reading\n",
				   path);

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

	prep_status("Checking current, bin, and data directories");

#ifndef WIN32
	if (access(".", R_OK | W_OK | X_OK) != 0)
#else
	if (win32_check_directory_write_permissions() != 0)
#endif
		pg_log(PG_FATAL,
		  "You must have read and write access in the current directory.\n");

	check_bin_dir(&old_cluster);
	check_data_dir(old_cluster.pgdata);
	check_bin_dir(&new_cluster);
	check_data_dir(new_cluster.pgdata);
	check_ok();
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
	int fd;

	/*
	 *	We open a file we would normally create anyway.  We do this even in
	 *	'check' mode, which isn't ideal, but this is the best we can do.
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
			report_status(PG_FATAL, "check for %s failed:  %s\n",
						  subDirName, getErrorText(errno));
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
		report_status(PG_FATAL, "check for %s failed:  %s\n",
					  cluster->bindir, getErrorText(errno));
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
		pg_log(PG_FATAL, "check for %s failed - %s\n",
			   path, getErrorText(errno));
	else if (!S_ISREG(buf.st_mode))
		pg_log(PG_FATAL, "check for %s failed - not an executable file\n",
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
		pg_log(PG_FATAL, "check for %s failed - cannot read file (permission denied)\n",
			   path);

#ifndef WIN32
	if (access(path, X_OK) != 0)
#else
	if ((buf.st_mode & S_IXUSR) == 0)
#endif
		pg_log(PG_FATAL, "check for %s failed - cannot execute (permission denied)\n",
			   path);
}
