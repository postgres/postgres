/*
 *	exec.c
 *
 *	execution functions
 */

#include "pg_upgrade.h"

#include <fcntl.h>
#include <grp.h>


static void checkBinDir(migratorContext *ctx, ClusterInfo *cluster);
static int check_exec(migratorContext *ctx, const char *dir, const char *cmdName);
static const char *validate_exec(const char *path);
static int	check_data_dir(migratorContext *ctx, const char *pg_data);


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
exec_prog(migratorContext *ctx, bool throw_error, const char *fmt,...)
{
	va_list		args;
	int			result;
	char		cmd[MAXPGPATH];

	va_start(args, fmt);
	vsnprintf(cmd, MAXPGPATH, fmt, args);
	va_end(args);

	pg_log(ctx, PG_INFO, "%s\n", cmd);

	result = system(cmd);

	if (result != 0)
	{
		pg_log(ctx, throw_error ? PG_FATAL : PG_INFO,
			   "\nThere were problems executing %s\n", cmd);
		return 1;
	}

	return 0;
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
verify_directories(migratorContext *ctx)
{
	prep_status(ctx, "Checking old data directory (%s)", ctx->old.pgdata);
	if (check_data_dir(ctx, ctx->old.pgdata) != 0)
		pg_log(ctx, PG_FATAL, "Failed\n");
	checkBinDir(ctx, &ctx->old);
	check_ok(ctx);

	prep_status(ctx, "Checking new data directory (%s)", ctx->new.pgdata);
	if (check_data_dir(ctx, ctx->new.pgdata) != 0)
		pg_log(ctx, PG_FATAL, "Failed\n");
	checkBinDir(ctx, &ctx->new);
	check_ok(ctx);
}


/*
 * checkBinDir()
 *
 *	This function searches for the executables that we expect to find
 *	in the binaries directory.	If we find that a required executable
 *	is missing (or secured against us), we display an error message and
 *	exit().
 */
static void
checkBinDir(migratorContext *ctx, ClusterInfo *cluster)
{
	check_exec(ctx, cluster->bindir, "postgres");
	check_exec(ctx, cluster->bindir, "psql");
	check_exec(ctx, cluster->bindir, "pg_ctl");
	check_exec(ctx, cluster->bindir, "pg_dumpall");
}


/*
 * is_server_running()
 *
 * checks whether postmaster on the given data directory is running or not.
 * The check is performed by looking for the existence of postmaster.pid file.
 */
bool
is_server_running(migratorContext *ctx, const char *datadir)
{
	char		path[MAXPGPATH];
	int			fd;

	snprintf(path, sizeof(path), "%s/postmaster.pid", datadir);

	if ((fd = open(path, O_RDONLY, 0)) < 0)
	{
		if (errno != ENOENT)
			pg_log(ctx, PG_FATAL, "\ncould not open file \"%s\" for reading\n",
				   path);

		return false;
	}

	close(fd);
	return true;
}


/*
 * check_exec()
 *
 *	Checks whether either of the two command names (cmdName and alternative)
 *	appears to be an executable (in the given directory).  If dir/cmdName is
 *	an executable, this function returns 1. If dir/alternative is an
 *	executable, this function returns 2.  If neither of the given names is
 *	a valid executable, this function returns 0 to indicated failure.
 */
static int
check_exec(migratorContext *ctx, const char *dir, const char *cmdName)
{
	char		path[MAXPGPATH];
	const char *errMsg;

	snprintf(path, sizeof(path), "%s/%s", dir, cmdName);

	if ((errMsg = validate_exec(path)) == NULL)
		return 1;				/* 1 -> first alternative OK */
	else
		pg_log(ctx, PG_FATAL, "check for %s failed - %s\n", cmdName, errMsg);

	return 0;					/* 0 -> neither alternative is acceptable */
}


/*
 * validate_exec()
 *
 * validate "path" as an executable file
 * returns 0 if the file is found and no error is encountered.
 *		  -1 if the regular file "path" does not exist or cannot be executed.
 *		  -2 if the file is otherwise valid but cannot be read.
 */
static const char *
validate_exec(const char *path)
{
	struct stat buf;

#ifndef WIN32
	uid_t		euid;
	struct group *gp;
	struct passwd *pwp;
	int			in_grp = 0;
#else
	char		path_exe[MAXPGPATH + sizeof(EXE_EXT) - 1];
#endif

#ifdef WIN32
	/* Win32 requires a .exe suffix for stat() */

	if (strlen(path) >= strlen(EXE_EXT) &&
		pg_strcasecmp(path + strlen(path) - strlen(EXE_EXT), EXE_EXT) != 0)
	{
		strcpy(path_exe, path);
		strcat(path_exe, EXE_EXT);
		path = path_exe;
	}
#endif

	/*
	 * Ensure that the file exists and is a regular file.
	 */
	if (stat(path, &buf) < 0)
		return getErrorText(errno);

	if ((buf.st_mode & S_IFMT) != S_IFREG)
		return "not an executable file";

	/*
	 * Ensure that we are using an authorized executable.
	 */

	/*
	 * Ensure that the file is both executable and readable (required for
	 * dynamic loading).
	 */
#ifndef WIN32
	euid = geteuid();

	/* If owned by us, just check owner bits */
	if (euid == buf.st_uid)
	{
		if ((buf.st_mode & S_IRUSR) == 0)
			return "can't read file (permission denied)";
		if ((buf.st_mode & S_IXUSR) == 0)
			return "can't execute (permission denied)";
		return NULL;
	}

	/* OK, check group bits */
	pwp = getpwuid(euid);		/* not thread-safe */

	if (pwp)
	{
		if (pwp->pw_gid == buf.st_gid)	/* my primary group? */
			++in_grp;
		else if (pwp->pw_name &&
				 (gp = getgrgid(buf.st_gid)) != NULL &&
				  /* not thread-safe */ gp->gr_mem != NULL)
		{
			/* try list of member groups */
			int			i;

			for (i = 0; gp->gr_mem[i]; ++i)
			{
				if (!strcmp(gp->gr_mem[i], pwp->pw_name))
				{
					++in_grp;
					break;
				}
			}
		}

		if (in_grp)
		{
			if ((buf.st_mode & S_IRGRP) == 0)
				return "can't read file (permission denied)";
			if ((buf.st_mode & S_IXGRP) == 0)
				return "can't execute (permission denied)";
			return NULL;
		}
	}

	/* Check "other" bits */
	if ((buf.st_mode & S_IROTH) == 0)
		return "can't read file (permission denied)";
	if ((buf.st_mode & S_IXOTH) == 0)
		return "can't execute (permission denied)";
	return NULL;
#else
	if ((buf.st_mode & S_IRUSR) == 0)
		return "can't read file (permission denied)";
	if ((buf.st_mode & S_IXUSR) == 0)
		return "can't execute (permission denied)";
	return NULL;
#endif
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
static int
check_data_dir(migratorContext *ctx, const char *pg_data)
{
	char		subDirName[MAXPGPATH];
	const char *requiredSubdirs[] = {"base", "global", "pg_clog",
		"pg_multixact", "pg_subtrans",
	"pg_tblspc", "pg_twophase", "pg_xlog"};
	bool		fail = false;
	int			subdirnum;

	for (subdirnum = 0; subdirnum < sizeof(requiredSubdirs) / sizeof(requiredSubdirs[0]); ++subdirnum)
	{
		struct stat statBuf;

		snprintf(subDirName, sizeof(subDirName), "%s/%s", pg_data,
				 requiredSubdirs[subdirnum]);

		if ((stat(subDirName, &statBuf)) != 0)
		{
			report_status(ctx, PG_WARNING, "check for %s warning:  %s",
						  requiredSubdirs[subdirnum], getErrorText(errno));
			fail = true;
		}
		else
		{
			if (!S_ISDIR(statBuf.st_mode))
			{
				report_status(ctx, PG_WARNING, "%s is not a directory",
							  requiredSubdirs[subdirnum]);
				fail = true;
			}
		}
	}

	return (fail) ? -1 : 0;
}


