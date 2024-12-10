/*
 * fork_process.c
 *	 A simple wrapper on top of fork(). This does not handle the
 *	 EXEC_BACKEND case; it might be extended to do so, but it would be
 *	 considerably more complex.
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/fork_process.c
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/fork_process.h"

#ifndef WIN32
/*
 * Wrapper for fork(). Return values are the same as those for fork():
 * -1 if the fork failed, 0 in the child process, and the PID of the
 * child in the parent process.  Signals are blocked while forking, so
 * the child must unblock.
 */
pid_t
fork_process(void)
{
	pid_t		result;
	const char *oomfilename;
	sigset_t	save_mask;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	/*
	 * Flush stdio channels just before fork, to avoid double-output problems.
	 */
	fflush(NULL);

#ifdef LINUX_PROFILE

	/*
	 * Linux's fork() resets the profiling timer in the child process. If we
	 * want to profile child processes then we need to save and restore the
	 * timer setting.  This is a waste of time if not profiling, however, so
	 * only do it if commanded by specific -DLINUX_PROFILE switch.
	 */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

	/*
	 * We start postmaster children with signals blocked.  This allows them to
	 * install their own handlers before unblocking, to avoid races where they
	 * might run the postmaster's handler and miss an important control
	 * signal. With more analysis this could potentially be relaxed.
	 */
	sigprocmask(SIG_SETMASK, &BlockSig, &save_mask);
	result = fork();
	if (result == 0)
	{
		/* fork succeeded, in child */
		MyProcPid = getpid();
#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

		/*
		 * By default, Linux tends to kill the postmaster in out-of-memory
		 * situations, because it blames the postmaster for the sum of child
		 * process sizes *including shared memory*.  (This is unbelievably
		 * stupid, but the kernel hackers seem uninterested in improving it.)
		 * Therefore it's often a good idea to protect the postmaster by
		 * setting its OOM score adjustment negative (which has to be done in
		 * a root-owned startup script).  Since the adjustment is inherited by
		 * child processes, this would ordinarily mean that all the
		 * postmaster's children are equally protected against OOM kill, which
		 * is not such a good idea.  So we provide this code to allow the
		 * children to change their OOM score adjustments again.  Both the
		 * file name to write to and the value to write are controlled by
		 * environment variables, which can be set by the same startup script
		 * that did the original adjustment.
		 */
		oomfilename = getenv("PG_OOM_ADJUST_FILE");

		if (oomfilename != NULL)
		{
			/*
			 * Use open() not stdio, to ensure we control the open flags. Some
			 * Linux security environments reject anything but O_WRONLY.
			 */
			int			fd = open(oomfilename, O_WRONLY, 0);

			/* We ignore all errors */
			if (fd >= 0)
			{
				const char *oomvalue = getenv("PG_OOM_ADJUST_VALUE");
				int			rc;

				if (oomvalue == NULL)	/* supply a useful default */
					oomvalue = "0";

				rc = write(fd, oomvalue, strlen(oomvalue));
				(void) rc;
				close(fd);
			}
		}

		/* do post-fork initialization for random number generation */
		pg_strong_random_init();
	}
	else
	{
		/* in parent, restore signal mask */
		sigprocmask(SIG_SETMASK, &save_mask, NULL);
	}

	return result;
}

#endif							/* ! WIN32 */
