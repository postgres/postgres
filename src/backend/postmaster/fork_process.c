/*
 * fork_process.c
 *	 A simple wrapper on top of fork(). This does not handle the
 *	 EXEC_BACKEND case; it might be extended to do so, but it would be
 *	 considerably more complex.
 *
 * Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/fork_process.c
 */
#include "postgres.h"
#include "postmaster/fork_process.h"

#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef USE_SSL
#include <openssl/rand.h>
#endif

#ifndef WIN32
/*
 * Wrapper for fork(). Return values are the same as those for fork():
 * -1 if the fork failed, 0 in the child process, and the PID of the
 * child in the parent process.
 */
pid_t
fork_process(void)
{
	pid_t		result;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	/*
	 * Flush stdio channels just before fork, to avoid double-output problems.
	 * Ideally we'd use fflush(NULL) here, but there are still a few non-ANSI
	 * stdio libraries out there (like SunOS 4.1.x) that coredump if we do.
	 * Presently stdout and stderr are the only stdio output channels used by
	 * the postmaster, so fflush'ing them should be sufficient.
	 */
	fflush(stdout);
	fflush(stderr);

#ifdef LINUX_PROFILE

	/*
	 * Linux's fork() resets the profiling timer in the child process. If we
	 * want to profile child processes then we need to save and restore the
	 * timer setting.  This is a waste of time if not profiling, however, so
	 * only do it if commanded by specific -DLINUX_PROFILE switch.
	 */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

	result = fork();
	if (result == 0)
	{
		/* fork succeeded, in child */
#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

		/*
		 * By default, Linux tends to kill the postmaster in out-of-memory
		 * situations, because it blames the postmaster for the sum of child
		 * process sizes *including shared memory*.  (This is unbelievably
		 * stupid, but the kernel hackers seem uninterested in improving it.)
		 * Therefore it's often a good idea to protect the postmaster by
		 * setting its oom_score_adj value negative (which has to be done in a
		 * root-owned startup script). If you just do that much, all child
		 * processes will also be protected against OOM kill, which might not
		 * be desirable.  You can then choose to build with
		 * LINUX_OOM_SCORE_ADJ #defined to 0, or to some other value that you
		 * want child processes to adopt here.
		 */
#ifdef LINUX_OOM_SCORE_ADJ
		{
			/*
			 * Use open() not stdio, to ensure we control the open flags. Some
			 * Linux security environments reject anything but O_WRONLY.
			 */
			int			fd = open("/proc/self/oom_score_adj", O_WRONLY, 0);

			/* We ignore all errors */
			if (fd >= 0)
			{
				char		buf[16];
				int			rc;

				snprintf(buf, sizeof(buf), "%d\n", LINUX_OOM_SCORE_ADJ);
				rc = write(fd, buf, strlen(buf));
				(void) rc;
				close(fd);
			}
		}
#endif   /* LINUX_OOM_SCORE_ADJ */

		/*
		 * Older Linux kernels have oom_adj not oom_score_adj.  This works
		 * similarly except with a different scale of adjustment values. If
		 * it's necessary to build Postgres to work with either API, you can
		 * define both LINUX_OOM_SCORE_ADJ and LINUX_OOM_ADJ.
		 */
#ifdef LINUX_OOM_ADJ
		{
			/*
			 * Use open() not stdio, to ensure we control the open flags. Some
			 * Linux security environments reject anything but O_WRONLY.
			 */
			int			fd = open("/proc/self/oom_adj", O_WRONLY, 0);

			/* We ignore all errors */
			if (fd >= 0)
			{
				char		buf[16];
				int			rc;

				snprintf(buf, sizeof(buf), "%d\n", LINUX_OOM_ADJ);
				rc = write(fd, buf, strlen(buf));
				(void) rc;
				close(fd);
			}
		}
#endif   /* LINUX_OOM_ADJ */

		/*
		 * Make sure processes do not share OpenSSL randomness state.
		 */
#ifdef USE_SSL
		RAND_cleanup();
#endif
	}

	return result;
}

#endif   /* ! WIN32 */
