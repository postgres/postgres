/*-------------------------------------------------------------------------
 *
 * wait_error.c
 *		Convert a wait/waitpid(2) result code to a human-readable string
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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
#include <stdio.h>
#include <string.h>
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
#if defined(WIN32)
		snprintf(str, sizeof(str),
				 _("child process was terminated by exception 0x%X"),
				 WTERMSIG(exitstatus));
#elif defined(HAVE_DECL_SYS_SIGLIST) && HAVE_DECL_SYS_SIGLIST
	{
		char		str2[256];

		snprintf(str2, sizeof(str2), "%d: %s", WTERMSIG(exitstatus),
				 WTERMSIG(exitstatus) < NSIG ?
				 sys_siglist[WTERMSIG(exitstatus)] : "(unknown)");
		snprintf(str, sizeof(str),
				 _("child process was terminated by signal %s"), str2);
	}
#else
		snprintf(str, sizeof(str),
				 _("child process was terminated by signal %d"),
				 WTERMSIG(exitstatus));
#endif
	else
		snprintf(str, sizeof(str),
				 _("child process exited with unrecognized status %d"),
				 exitstatus);

	return pstrdup(str);
}
