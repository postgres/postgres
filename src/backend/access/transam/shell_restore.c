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
#include "common/percentrepl.h"
#include "storage/ipc.h"
#include "utils/wait_event.h"

static bool ExecuteRecoveryCommand(const char *command,
								   const char *commandName,
								   bool failOnSignal,
								   bool exitOnSigterm,
								   uint32 wait_event_info,
								   int fail_elevel);

/*
 * Attempt to execute a shell-based restore command.
 *
 * Returns true if the command has succeeded, false otherwise.
 */
bool
shell_restore(const char *file, const char *path,
			  const char *lastRestartPointFileName)
{
	char	   *nativePath = pstrdup(path);
	char	   *cmd;
	bool		ret;

	/* Build the restore command to execute */
	make_native_path(nativePath);
	cmd = replace_percent_placeholders(recoveryRestoreCommand,
									   "restore_command", "frp", file,
									   lastRestartPointFileName,
									   nativePath);
	pfree(nativePath);

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
	ret = ExecuteRecoveryCommand(cmd, "restore_command",
								 true,	/* failOnSignal */
								 true,	/* exitOnSigterm */
								 WAIT_EVENT_RESTORE_COMMAND, DEBUG2);
	pfree(cmd);

	return ret;
}

/*
 * Attempt to execute a shell-based archive cleanup command.
 */
void
shell_archive_cleanup(const char *lastRestartPointFileName)
{
	char	   *cmd;

	cmd = replace_percent_placeholders(archiveCleanupCommand,
									   "archive_cleanup_command",
									   "r", lastRestartPointFileName);
	(void) ExecuteRecoveryCommand(cmd, "archive_cleanup_command", false, false,
								  WAIT_EVENT_ARCHIVE_CLEANUP_COMMAND, WARNING);
	pfree(cmd);
}

/*
 * Attempt to execute a shell-based end-of-recovery command.
 */
void
shell_recovery_end(const char *lastRestartPointFileName)
{
	char	   *cmd;

	cmd = replace_percent_placeholders(recoveryEndCommand,
									   "recovery_end_command",
									   "r", lastRestartPointFileName);
	(void) ExecuteRecoveryCommand(cmd, "recovery_end_command", true, false,
								  WAIT_EVENT_RECOVERY_END_COMMAND, WARNING);
	pfree(cmd);
}

/*
 * Attempt to execute an external shell command during recovery.
 *
 * 'command' is the shell command to be executed, 'commandName' is a
 * human-readable name describing the command emitted in the logs. If
 * 'failOnSignal' is true and the command is killed by a signal, a FATAL
 * error is thrown. Otherwise, 'fail_elevel' is used for the log message.
 * If 'exitOnSigterm' is true and the command is killed by SIGTERM, we exit
 * immediately.
 *
 * Returns whether the command succeeded.
 */
static bool
ExecuteRecoveryCommand(const char *command, const char *commandName,
					   bool failOnSignal, bool exitOnSigterm,
					   uint32 wait_event_info, int fail_elevel)
{
	int			rc;

	Assert(command && commandName);

	ereport(DEBUG3,
			(errmsg_internal("executing %s \"%s\"", commandName, command)));

	/*
	 * execute the constructed command
	 */
	fflush(NULL);
	pgstat_report_wait_start(wait_event_info);
	rc = system(command);
	pgstat_report_wait_end();

	if (rc != 0)
	{
		if (exitOnSigterm && wait_result_is_signal(rc, SIGTERM))
			proc_exit(1);

		/*
		 * If the failure was due to any sort of signal, it's best to punt and
		 * abort recovery.  See comments in shell_restore().
		 */
		ereport((failOnSignal && wait_result_is_any_signal(rc, true)) ? FATAL : fail_elevel,
		/*------
		   translator: First %s represents a postgresql.conf parameter name like
		  "recovery_end_command", the 2nd is the value of that parameter, the
		  third an already translated error message. */
				(errmsg("%s \"%s\": %s", commandName,
						command, wait_result_to_str(rc))));
	}

	return (rc == 0);
}
