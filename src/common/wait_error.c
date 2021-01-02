/*-------------------------------------------------------------------------
 *
 * wait_error.c
 *		Convert a wait/waitpid(2) result code to a human-readable string
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/wait_error.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <signal.h>
#include <sys/wait.h>

/*
 * Return a human-readable string explaining the reason a child process
 * terminated. The argument is a return code returned by wait(2) or
 * waitpid(2). The result is a translated, palloc'd or malloc'd string.
 */
char *
wait_result_to_str(int exitstatus)
{
	char		str[512];

	if (WIFEXITED(exitstatus))
	{
		/*
		 * Give more specific error message for some common exit codes that
		 * have a special meaning in shells.
		 */
		switch (WEXITSTATUS(exitstatus))
		{
			case 126:
				snprintf(str, sizeof(str), _("command not executable"));
				break;

			case 127:
				snprintf(str, sizeof(str), _("command not found"));
				break;

			default:
				snprintf(str, sizeof(str),
						 _("child process exited with exit code %d"),
						 WEXITSTATUS(exitstatus));
		}
	}
	else if (WIFSIGNALED(exitstatus))
	{
#if defined(WIN32)
		snprintf(str, sizeof(str),
				 _("child process was terminated by exception 0x%X"),
				 WTERMSIG(exitstatus));
#else
		snprintf(str, sizeof(str),
				 _("child process was terminated by signal %d: %s"),
				 WTERMSIG(exitstatus), pg_strsignal(WTERMSIG(exitstatus)));
#endif
	}
	else
		snprintf(str, sizeof(str),
				 _("child process exited with unrecognized status %d"),
				 exitstatus);

	return pstrdup(str);
}

/*
 * Return true if a wait(2) result indicates that the child process
 * died due to the specified signal.
 *
 * The reason this is worth having a wrapper function for is that
 * there are two cases: the signal might have been received by our
 * immediate child process, or there might've been a shell process
 * between us and the child that died.  The shell will, per POSIX,
 * report the child death using exit code 128 + signal number.
 *
 * If there is no possibility of an intermediate shell, this function
 * need not (and probably should not) be used.
 */
bool
wait_result_is_signal(int exit_status, int signum)
{
	if (WIFSIGNALED(exit_status) && WTERMSIG(exit_status) == signum)
		return true;
	if (WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 128 + signum)
		return true;
	return false;
}

/*
 * Return true if a wait(2) result indicates that the child process
 * died due to any signal.  We consider either direct child death
 * or a shell report of child process death as matching the condition.
 *
 * If include_command_not_found is true, also return true for shell
 * exit codes indicating "command not found" and the like
 * (specifically, exit codes 126 and 127; see above).
 */
bool
wait_result_is_any_signal(int exit_status, bool include_command_not_found)
{
	if (WIFSIGNALED(exit_status))
		return true;
	if (WIFEXITED(exit_status) &&
		WEXITSTATUS(exit_status) > (include_command_not_found ? 125 : 128))
		return true;
	return false;
}
