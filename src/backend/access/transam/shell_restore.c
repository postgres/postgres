/*-------------------------------------------------------------------------
 *
 * shell_restore.c
 *		Recovery functions for a user-specified shell command.
 *
 * These recovery functions use a user-specified shell command (e.g. based
 * on the GUC restore_command).
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/shell_restore.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>

#include "access/xlogarchive.h"
#include "access/xlogrecovery.h"
#include "common/archive.h"
#include "common/percentrepl.h"
#include "storage/ipc.h"
#include "utils/wait_event.h"

static void ExecuteRecoveryCommand(const char *command,
								   const char *commandName,
								   bool failOnSignal,
								   uint32 wait_event_info,
								   const char *lastRestartPointFileName);

/*
 * Attempt to execute a shell-based restore command.
 *
 * Returns true if the command has succeeded, false otherwise.
 */
bool
shell_restore(const char *file, const char *path,
			  const char *lastRestartPointFileName)
{
	char	   *cmd;
	int			rc;

	/* Build the restore command to execute */
	cmd = BuildRestoreCommand(recoveryRestoreCommand, path, file,
							  lastRestartPointFileName);

	ereport(DEBUG3,
			(errmsg_internal("executing restore command \"%s\"", cmd)));

	/*
	 * Copy xlog from archival storage to XLOGDIR
	 */
	fflush(NULL);
	pgstat_report_wait_start(WAIT_EVENT_RESTORE_COMMAND);
	rc = system(cmd);
	pgstat_report_wait_end();

	pfree(cmd);

	/*
	 * Remember, we rollforward UNTIL the restore fails so failure here is
	 * just part of the process... that makes it difficult to determine
	 * whether the restore failed because there isn't an archive to restore,
	 * or because the administrator has specified the restore program
	 * incorrectly.  We have to assume the former.
	 *
	 * However, if the failure was due to any sort of signal, it's best to
	 * punt and abort recovery.  (If we "return false" here, upper levels will
	 * assume that recovery is complete and start up the database!) It's
	 * essential to abort on child SIGINT and SIGQUIT, because per spec
	 * system() ignores SIGINT and SIGQUIT while waiting; if we see one of
	 * those it's a good bet we should have gotten it too.
	 *
	 * On SIGTERM, assume we have received a fast shutdown request, and exit
	 * cleanly. It's pure chance whether we receive the SIGTERM first, or the
	 * child process. If we receive it first, the signal handler will call
	 * proc_exit, otherwise we do it here. If we or the child process received
	 * SIGTERM for any other reason than a fast shutdown request, postmaster
	 * will perform an immediate shutdown when it sees us exiting
	 * unexpectedly.
	 *
	 * We treat hard shell errors such as "command not found" as fatal, too.
	 */
	if (rc != 0)
	{
		if (wait_result_is_signal(rc, SIGTERM))
			proc_exit(1);

		ereport(wait_result_is_any_signal(rc, true) ? FATAL : DEBUG2,
				(errmsg("could not restore file \"%s\" from archive: %s",
						file, wait_result_to_str(rc))));
	}

	return (rc == 0);
}

/*
 * Attempt to execute a shell-based archive cleanup command.
 */
void
shell_archive_cleanup(const char *lastRestartPointFileName)
{
	ExecuteRecoveryCommand(archiveCleanupCommand, "archive_cleanup_command",
						   false, WAIT_EVENT_ARCHIVE_CLEANUP_COMMAND,
						   lastRestartPointFileName);
}

/*
 * Attempt to execute a shell-based end-of-recovery command.
 */
void
shell_recovery_end(const char *lastRestartPointFileName)
{
	ExecuteRecoveryCommand(recoveryEndCommand, "recovery_end_command", true,
						   WAIT_EVENT_RECOVERY_END_COMMAND,
						   lastRestartPointFileName);
}

/*
 * Attempt to execute an external shell command during recovery.
 *
 * 'command' is the shell command to be executed, 'commandName' is a
 * human-readable name describing the command emitted in the logs. If
 * 'failOnSignal' is true and the command is killed by a signal, a FATAL
 * error is thrown. Otherwise a WARNING is emitted.
 *
 * This is currently used for recovery_end_command and archive_cleanup_command.
 */
static void
ExecuteRecoveryCommand(const char *command, const char *commandName,
					   bool failOnSignal, uint32 wait_event_info,
					   const char *lastRestartPointFileName)
{
	char	   *xlogRecoveryCmd;
	int			rc;

	Assert(command && commandName);

	/*
	 * construct the command to be executed
	 */
	xlogRecoveryCmd = replace_percent_placeholders(command, commandName, "r",
												   lastRestartPointFileName);

	ereport(DEBUG3,
			(errmsg_internal("executing %s \"%s\"", commandName, command)));

	/*
	 * execute the constructed command
	 */
	fflush(NULL);
	pgstat_report_wait_start(wait_event_info);
	rc = system(xlogRecoveryCmd);
	pgstat_report_wait_end();

	pfree(xlogRecoveryCmd);

	if (rc != 0)
	{
		/*
		 * If the failure was due to any sort of signal, it's best to punt and
		 * abort recovery.  See comments in shell_restore().
		 */
		ereport((failOnSignal && wait_result_is_any_signal(rc, true)) ? FATAL : WARNING,
		/*------
		   translator: First %s represents a postgresql.conf parameter name like
		  "recovery_end_command", the 2nd is the value of that parameter, the
		  third an already translated error message. */
				(errmsg("%s \"%s\": %s", commandName,
						command, wait_result_to_str(rc))));
	}
}
