/*-------------------------------------------------------------------------
 *
 * wait_error.c
 *		Convert a wait/waitpid(2) result code to a human-readable string
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
 * waitpid(2), which also applies to pclose(3) and system(3). The result is a
 * translated, palloc'd or malloc'd string.
 */
char *
wait_result_to_str(int exitstatus)
{
	char		str[512];

	/*
	 * To simplify using this after pclose() and system(), handle status -1
	 * first.  In that case, there is no wait result but some error indicated
	 * by errno.
	 */
	if (exitstatus == -1)
	{
		snprintf(str, sizeof(str), "%m");
	}
	else if (WIFEXITED(exitstatus))
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

/*
 * Return the shell exit code (normally 0 to 255) that corresponds to the
 * given wait status.  The argument is a wait status as returned by wait(2)
 * or waitpid(2), which also applies to pclose(3) and system(3).  To support
 * the latter two cases, we pass through "-1" unchanged.
 */
int
wait_result_to_exit_code(int exit_status)
{
	if (exit_status == -1)
		return -1;				/* failure of pclose() or system() */
	if (WIFEXITED(exit_status))
		return WEXITSTATUS(exit_status);
	if (WIFSIGNALED(exit_status))
		return 128 + WTERMSIG(exit_status);
	/* On many systems, this is unreachable */
	return -1;
}
