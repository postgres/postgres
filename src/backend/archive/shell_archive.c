/*-------------------------------------------------------------------------
 *
 * shell_archive.c
 *
 * This archiving function uses a user-specified shell command (the
 * archive_command GUC) to copy write-ahead log files.  It is used as the
 * default, but other modules may define their own custom archiving logic.
 *
 * Copyright (c) 2022-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/archive/shell_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/wait.h>

#include "access/xlog.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "common/percentrepl.h"
#include "pgstat.h"

static bool shell_archive_configured(ArchiveModuleState *state);
static bool shell_archive_file(ArchiveModuleState *state,
							   const char *file,
							   const char *path);
static void shell_archive_shutdown(ArchiveModuleState *state);

static const ArchiveModuleCallbacks shell_archive_callbacks = {
	.startup_cb = NULL,
	.check_configured_cb = shell_archive_configured,
	.archive_file_cb = shell_archive_file,
	.shutdown_cb = shell_archive_shutdown
};

const ArchiveModuleCallbacks *
shell_archive_init(void)
{
	return &shell_archive_callbacks;
}

static bool
shell_archive_configured(ArchiveModuleState *state)
{
	if (XLogArchiveCommand[0] != '\0')
		return true;

	arch_module_check_errdetail("\"%s\" is not set.",
								"archive_command");
	return false;
}

static bool
shell_archive_file(ArchiveModuleState *state, const char *file,
				   const char *path)
{
	char	   *xlogarchcmd;
	char	   *nativePath = NULL;
	int			rc;

	if (path)
	{
		nativePath = pstrdup(path);
		make_native_path(nativePath);
	}

	xlogarchcmd = replace_percent_placeholders(XLogArchiveCommand,
											   "archive_command", "fp",
											   file, nativePath);

	ereport(DEBUG3,
			(errmsg_internal("executing archive command \"%s\"",
							 xlogarchcmd)));

	fflush(NULL);
	pgstat_report_wait_start(WAIT_EVENT_ARCHIVE_COMMAND);
	rc = system(xlogarchcmd);
	pgstat_report_wait_end();

	if (rc != 0)
	{
		/*
		 * If either the shell itself, or a called command, died on a signal,
		 * abort the archiver.  We do this because system() ignores SIGINT and
		 * SIGQUIT while waiting; so a signal is very likely something that
		 * should have interrupted us too.  Also die if the shell got a hard
		 * "command not found" type of error.  If we overreact it's no big
		 * deal, the postmaster will just start the archiver again.
		 */
		int			lev = wait_result_is_any_signal(rc, true) ? FATAL : LOG;

		if (WIFEXITED(rc))
		{
			ereport(lev,
					(errmsg("archive command failed with exit code %d",
							WEXITSTATUS(rc)),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}
		else if (WIFSIGNALED(rc))
		{
#if defined(WIN32)
			ereport(lev,
					(errmsg("archive command was terminated by exception 0x%X",
							WTERMSIG(rc)),
					 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#else
			ereport(lev,
					(errmsg("archive command was terminated by signal %d: %s",
							WTERMSIG(rc), pg_strsignal(WTERMSIG(rc))),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
#endif
		}
		else
		{
			ereport(lev,
					(errmsg("archive command exited with unrecognized status %d",
							rc),
					 errdetail("The failed archive command was: %s",
							   xlogarchcmd)));
		}
		pfree(xlogarchcmd);

		return false;
	}
	pfree(xlogarchcmd);

	elog(DEBUG1, "archived write-ahead log file \"%s\"", file);
	return true;
}

static void
shell_archive_shutdown(ArchiveModuleState *state)
{
	elog(DEBUG1, "archiver process shutting down");
}
