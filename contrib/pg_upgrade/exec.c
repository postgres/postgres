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
			   "\nThere were problems executing %s\n", cmd);
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
		if (errno != ENOENT)
			pg_log(PG_FATAL, "\ncould not open file \"%s\" for reading\n",
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
	prep_status("Checking old data directory (%s)", old_cluster.pgdata);
	check_data_dir(old_cluster.pgdata);
	check_ok();

	prep_status("Checking old bin directory (%s)", old_cluster.bindir);
	check_bin_dir(&old_cluster);
	check_ok();

	prep_status("Checking new data directory (%s)", new_cluster.pgdata);
	check_data_dir(new_cluster.pgdata);
	check_ok();

	prep_status("Checking new bin directory (%s)", new_cluster.bindir);
	check_bin_dir(&new_cluster);
	check_ok();
}


/*
 * check_data_dir()
 *
 *	This function validates the given cluster directory - we search for a
 *	small set of subdirectories that we expect to find in a valid $PGDATA
 *	directory.	If any of the subdirectories are missing (or secured against
 *	us) we display an error message and exit()
 *
 */
static void
check_data_dir(const char *pg_data)
{
	char		subDirName[MAXPGPATH];
	int			subdirnum;
	const char *requiredSubdirs[] = {"base", "global", "pg_clog",
		"pg_multixact", "pg_subtrans", "pg_tblspc", "pg_twophase",
	"pg_xlog"};

	for (subdirnum = 0;
		 subdirnum < sizeof(requiredSubdirs) / sizeof(requiredSubdirs[0]);
		 ++subdirnum)
	{
		struct stat statBuf;

		snprintf(subDirName, sizeof(subDirName), "%s/%s", pg_data,
				 requiredSubdirs[subdirnum]);

		if (stat(subDirName, &statBuf) != 0)
			report_status(PG_FATAL, "check for %s failed:  %s",
						  requiredSubdirs[subdirnum], getErrorText(errno));
		else if (!S_ISDIR(statBuf.st_mode))
			report_status(PG_FATAL, "%s is not a directory",
						  requiredSubdirs[subdirnum]);
	}
}


/*
 * check_bin_dir()
 *
 *	This function searches for the executables that we expect to find
 *	in the binaries directory.	If we find that a required executable
 *	is missing (or secured against us), we display an error message and
 *	exit().
 */
static void
check_bin_dir(ClusterInfo *cluster)
{
	validate_exec(cluster->bindir, "postgres");
	validate_exec(cluster->bindir, "pg_ctl");
	validate_exec(cluster->bindir, "pg_resetxlog");
	if (cluster == &new_cluster)
	{
		/* these are only needed in the new cluster */
		validate_exec(cluster->bindir, "pg_config");
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
			   cmdName, getErrorText(errno));

	if (!S_ISREG(buf.st_mode))
		pg_log(PG_FATAL, "check for %s failed - not an executable file\n",
			   cmdName);

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
			   cmdName);

#ifndef WIN32
	if (access(path, X_OK) != 0)
#else
	if ((buf.st_mode & S_IXUSR) == 0)
#endif
		pg_log(PG_FATAL, "check for %s failed - cannot execute (permission denied)\n",
			   cmdName);
}
