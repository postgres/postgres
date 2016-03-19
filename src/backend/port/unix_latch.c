/*-------------------------------------------------------------------------
 *
 * unix_latch.c
 *	  Routines for inter-process latches
 *
 * The Unix implementation uses the so-called self-pipe trick to overcome
 * the race condition involved with select() and setting a global flag
 * in the signal handler. When a latch is set and the current process
 * is waiting for it, the signal handler wakes up the select() in
 * WaitLatch by writing a byte to a pipe. A signal by itself doesn't
 * interrupt select() on all platforms, and even on platforms where it
 * does, a signal that arrives just before the select() call does not
 * prevent the select() from entering sleep. An incoming byte on a pipe
 * however reliably interrupts the sleep, and causes select() to return
 * immediately even if the signal arrives before select() begins.
 *
 * (Actually, we prefer poll() over select() where available, but the
 * same comments apply to it.)
 *
 * When SetLatch is called from the same process that owns the latch,
 * SetLatch writes the byte directly to the pipe. If it's owned by another
 * process, SIGUSR1 is sent and the signal handler in the waiting process
 * writes the byte to the pipe on behalf of the signaling process.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/unix_latch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "miscadmin.h"
#include "portability/instr_time.h"
#include "postmaster/postmaster.h"
#include "storage/barrier.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"

/*
 * Select the fd readiness primitive to use. Normally the "most modern"
 * primitive supported by the OS will be used, but for testing it can be
 * useful to manually specify the used primitive.  If desired, just add a
 * define somewhere before this block.
 */
#if defined(LATCH_USE_POLL) || defined(LATCH_USE_SELECT)
/* don't overwrite manual choice */
#elif defined(HAVE_POLL)
#define LATCH_USE_POLL
#elif HAVE_SYS_SELECT_H
#define LATCH_USE_SELECT
#else
#error "no latch implementation available"
#endif

/* Are we currently in WaitLatch? The signal handler would like to know. */
static volatile sig_atomic_t waiting = false;

/* Read and write ends of the self-pipe */
static int	selfpipe_readfd = -1;
static int	selfpipe_writefd = -1;

/* Private function prototypes */
static void sendSelfPipeByte(void);
static void drainSelfPipe(void);


/*
 * Initialize the process-local latch infrastructure.
 *
 * This must be called once during startup of any process that can wait on
 * latches, before it issues any InitLatch() or OwnLatch() calls.
 */
void
InitializeLatchSupport(void)
{
	int			pipefd[2];

	Assert(selfpipe_readfd == -1);

	/*
	 * Set up the self-pipe that allows a signal handler to wake up the
	 * select() in WaitLatch. Make the write-end non-blocking, so that
	 * SetLatch won't block if the event has already been set many times
	 * filling the kernel buffer. Make the read-end non-blocking too, so that
	 * we can easily clear the pipe by reading until EAGAIN or EWOULDBLOCK.
	 */
	if (pipe(pipefd) < 0)
		elog(FATAL, "pipe() failed: %m");
	if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0)
		elog(FATAL, "fcntl() failed on read-end of self-pipe: %m");
	if (fcntl(pipefd[1], F_SETFL, O_NONBLOCK) < 0)
		elog(FATAL, "fcntl() failed on write-end of self-pipe: %m");

	selfpipe_readfd = pipefd[0];
	selfpipe_writefd = pipefd[1];
}

/*
 * Initialize a backend-local latch.
 */
void
InitLatch(volatile Latch *latch)
{
	/* Assert InitializeLatchSupport has been called in this process */
	Assert(selfpipe_readfd >= 0);

	latch->is_set = false;
	latch->owner_pid = MyProcPid;
	latch->is_shared = false;
}

/*
 * Initialize a shared latch that can be set from other processes. The latch
 * is initially owned by no-one; use OwnLatch to associate it with the
 * current process.
 *
 * InitSharedLatch needs to be called in postmaster before forking child
 * processes, usually right after allocating the shared memory block
 * containing the latch with ShmemInitStruct. (The Unix implementation
 * doesn't actually require that, but the Windows one does.) Because of
 * this restriction, we have no concurrency issues to worry about here.
 */
void
InitSharedLatch(volatile Latch *latch)
{
	latch->is_set = false;
	latch->owner_pid = 0;
	latch->is_shared = true;
}

/*
 * Associate a shared latch with the current process, allowing it to
 * wait on the latch.
 *
 * Although there is a sanity check for latch-already-owned, we don't do
 * any sort of locking here, meaning that we could fail to detect the error
 * if two processes try to own the same latch at about the same time.  If
 * there is any risk of that, caller must provide an interlock to prevent it.
 *
 * In any process that calls OwnLatch(), make sure that
 * latch_sigusr1_handler() is called from the SIGUSR1 signal handler,
 * as shared latches use SIGUSR1 for inter-process communication.
 */
void
OwnLatch(volatile Latch *latch)
{
	/* Assert InitializeLatchSupport has been called in this process */
	Assert(selfpipe_readfd >= 0);

	Assert(latch->is_shared);

	/* sanity check */
	if (latch->owner_pid != 0)
		elog(ERROR, "latch already owned");

	latch->owner_pid = MyProcPid;
}

/*
 * Disown a shared latch currently owned by the current process.
 */
void
DisownLatch(volatile Latch *latch)
{
	Assert(latch->is_shared);
	Assert(latch->owner_pid == MyProcPid);

	latch->owner_pid = 0;
}

/*
 * Wait for a given latch to be set, or for postmaster death, or until timeout
 * is exceeded. 'wakeEvents' is a bitmask that specifies which of those events
 * to wait for. If the latch is already set (and WL_LATCH_SET is given), the
 * function returns immediately.
 *
 * The "timeout" is given in milliseconds. It must be >= 0 if WL_TIMEOUT flag
 * is given.  Although it is declared as "long", we don't actually support
 * timeouts longer than INT_MAX milliseconds.  Note that some extra overhead
 * is incurred when WL_TIMEOUT is given, so avoid using a timeout if possible.
 *
 * The latch must be owned by the current process, ie. it must be a
 * backend-local latch initialized with InitLatch, or a shared latch
 * associated with the current process by calling OwnLatch.
 *
 * Returns bit mask indicating which condition(s) caused the wake-up. Note
 * that if multiple wake-up conditions are true, there is no guarantee that
 * we return all of them in one call, but we will return at least one.
 */
int
WaitLatch(volatile Latch *latch, int wakeEvents, long timeout)
{
	return WaitLatchOrSocket(latch, wakeEvents, PGINVALID_SOCKET, timeout);
}

/*
 * Like WaitLatch, but with an extra socket argument for WL_SOCKET_*
 * conditions.
 *
 * When waiting on a socket, EOF and error conditions are reported by
 * returning the socket as readable/writable or both, depending on
 * WL_SOCKET_READABLE/WL_SOCKET_WRITEABLE being specified.
 */
int
WaitLatchOrSocket(volatile Latch *latch, int wakeEvents, pgsocket sock,
				  long timeout)
{
	int			result = 0;
	int			rc;
	instr_time	start_time,
				cur_time;
	long		cur_timeout;

#if defined(LATCH_USE_POLL)
	struct pollfd pfds[3];
	int			nfds;
#elif defined(LATCH_USE_SELECT)
	struct timeval tv,
			   *tvp;
	fd_set		input_mask;
	fd_set		output_mask;
	int			hifd;
#endif

	Assert(wakeEvents != 0);	/* must have at least one wake event */

	/* waiting for socket readiness without a socket indicates a bug */
	if (sock == PGINVALID_SOCKET &&
		(wakeEvents & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE)) != 0)
		elog(ERROR, "cannot wait on socket event without a socket");

	if ((wakeEvents & WL_LATCH_SET) && latch->owner_pid != MyProcPid)
		elog(ERROR, "cannot wait on a latch owned by another process");

	/*
	 * Initialize timeout if requested.  We must record the current time so
	 * that we can determine the remaining timeout if the poll() or select()
	 * is interrupted.  (On some platforms, select() will update the contents
	 * of "tv" for us, but unfortunately we can't rely on that.)
	 */
	if (wakeEvents & WL_TIMEOUT)
	{
		INSTR_TIME_SET_CURRENT(start_time);
		Assert(timeout >= 0 && timeout <= INT_MAX);
		cur_timeout = timeout;

#ifdef LATCH_USE_SELECT
		tv.tv_sec = cur_timeout / 1000L;
		tv.tv_usec = (cur_timeout % 1000L) * 1000L;
		tvp = &tv;
#endif
	}
	else
	{
		cur_timeout = -1;

#ifdef LATCH_USE_SELECT
		tvp = NULL;
#endif
	}

	waiting = true;
	do
	{
		/*
		 * Check if the latch is set already. If so, leave loop immediately,
		 * avoid blocking again. We don't attempt to report any other events
		 * that might also be satisfied.
		 *
		 * If someone sets the latch between this and the poll()/select()
		 * below, the setter will write a byte to the pipe (or signal us and
		 * the signal handler will do that), and the poll()/select() will
		 * return immediately.
		 *
		 * If there's a pending byte in the self pipe, we'll notice whenever
		 * blocking. Only clearing the pipe in that case avoids having to
		 * drain it every time WaitLatchOrSocket() is used. Should the
		 * pipe-buffer fill up we're still ok, because the pipe is in
		 * nonblocking mode. It's unlikely for that to happen, because the
		 * self pipe isn't filled unless we're blocking (waiting = true), or
		 * from inside a signal handler in latch_sigusr1_handler().
		 *
		 * Note: we assume that the kernel calls involved in drainSelfPipe()
		 * and SetLatch() will provide adequate synchronization on machines
		 * with weak memory ordering, so that we cannot miss seeing is_set if
		 * the signal byte is already in the pipe when we drain it.
		 */
		if ((wakeEvents & WL_LATCH_SET) && latch->is_set)
		{
			result |= WL_LATCH_SET;
			break;
		}

		/*
		 * Must wait ... we use the polling interface determined at the top of
		 * this file to do so.
		 */
#if defined(LATCH_USE_POLL)
		nfds = 0;

		/* selfpipe is always in pfds[0] */
		pfds[0].fd = selfpipe_readfd;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		nfds++;

		if (wakeEvents & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE))
		{
			/* socket, if used, is always in pfds[1] */
			pfds[1].fd = sock;
			pfds[1].events = 0;
			if (wakeEvents & WL_SOCKET_READABLE)
				pfds[1].events |= POLLIN;
			if (wakeEvents & WL_SOCKET_WRITEABLE)
				pfds[1].events |= POLLOUT;
			pfds[1].revents = 0;
			nfds++;
		}

		if (wakeEvents & WL_POSTMASTER_DEATH)
		{
			/* postmaster fd, if used, is always in pfds[nfds - 1] */
			pfds[nfds].fd = postmaster_alive_fds[POSTMASTER_FD_WATCH];
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		/* Sleep */
		rc = poll(pfds, nfds, (int) cur_timeout);

		/* Check return code */
		if (rc < 0)
		{
			/* EINTR is okay, otherwise complain */
			if (errno != EINTR)
			{
				waiting = false;
				ereport(ERROR,
						(errcode_for_socket_access(),
						 errmsg("poll() failed: %m")));
			}
		}
		else if (rc == 0)
		{
			/* timeout exceeded */
			if (wakeEvents & WL_TIMEOUT)
				result |= WL_TIMEOUT;
		}
		else
		{
			/* at least one event occurred, so check revents values */

			if (pfds[0].revents & POLLIN)
			{
				/* There's data in the self-pipe, clear it. */
				drainSelfPipe();
			}

			if ((wakeEvents & WL_SOCKET_READABLE) &&
				(pfds[1].revents & POLLIN))
			{
				/* data available in socket, or EOF/error condition */
				result |= WL_SOCKET_READABLE;
			}
			if ((wakeEvents & WL_SOCKET_WRITEABLE) &&
				(pfds[1].revents & POLLOUT))
			{
				/* socket is writable */
				result |= WL_SOCKET_WRITEABLE;
			}
			if ((wakeEvents & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE)) &&
				(pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)))
			{
				/* EOF/error condition */
				if (wakeEvents & WL_SOCKET_READABLE)
					result |= WL_SOCKET_READABLE;
				if (wakeEvents & WL_SOCKET_WRITEABLE)
					result |= WL_SOCKET_WRITEABLE;
			}

			/*
			 * We expect a POLLHUP when the remote end is closed, but because
			 * we don't expect the pipe to become readable or to have any
			 * errors either, treat those cases as postmaster death, too.
			 */
			if ((wakeEvents & WL_POSTMASTER_DEATH) &&
				(pfds[nfds - 1].revents & (POLLHUP | POLLIN | POLLERR | POLLNVAL)))
			{
				/*
				 * According to the select(2) man page on Linux, select(2) may
				 * spuriously return and report a file descriptor as readable,
				 * when it's not; and presumably so can poll(2).  It's not
				 * clear that the relevant cases would ever apply to the
				 * postmaster pipe, but since the consequences of falsely
				 * returning WL_POSTMASTER_DEATH could be pretty unpleasant,
				 * we take the trouble to positively verify EOF with
				 * PostmasterIsAlive().
				 */
				if (!PostmasterIsAlive())
					result |= WL_POSTMASTER_DEATH;
			}
		}
#elif defined(LATCH_USE_SELECT)

		/*
		 * On at least older linux kernels select(), in violation of POSIX,
		 * doesn't reliably return a socket as writable if closed - but we
		 * rely on that. So far all the known cases of this problem are on
		 * platforms that also provide a poll() implementation without that
		 * bug.  If we find one where that's not the case, we'll need to add a
		 * workaround.
		 */
		FD_ZERO(&input_mask);
		FD_ZERO(&output_mask);

		FD_SET(selfpipe_readfd, &input_mask);
		hifd = selfpipe_readfd;

		if (wakeEvents & WL_POSTMASTER_DEATH)
		{
			FD_SET(postmaster_alive_fds[POSTMASTER_FD_WATCH], &input_mask);
			if (postmaster_alive_fds[POSTMASTER_FD_WATCH] > hifd)
				hifd = postmaster_alive_fds[POSTMASTER_FD_WATCH];
		}

		if (wakeEvents & WL_SOCKET_READABLE)
		{
			FD_SET(sock, &input_mask);
			if (sock > hifd)
				hifd = sock;
		}

		if (wakeEvents & WL_SOCKET_WRITEABLE)
		{
			FD_SET(sock, &output_mask);
			if (sock > hifd)
				hifd = sock;
		}

		/* Sleep */
		rc = select(hifd + 1, &input_mask, &output_mask, NULL, tvp);

		/* Check return code */
		if (rc < 0)
		{
			/* EINTR is okay, otherwise complain */
			if (errno != EINTR)
			{
				waiting = false;
				ereport(ERROR,
						(errcode_for_socket_access(),
						 errmsg("select() failed: %m")));
			}
		}
		else if (rc == 0)
		{
			/* timeout exceeded */
			if (wakeEvents & WL_TIMEOUT)
				result |= WL_TIMEOUT;
		}
		else
		{
			/* at least one event occurred, so check masks */
			if (FD_ISSET(selfpipe_readfd, &input_mask))
			{
				/* There's data in the self-pipe, clear it. */
				drainSelfPipe();
			}
			if ((wakeEvents & WL_SOCKET_READABLE) && FD_ISSET(sock, &input_mask))
			{
				/* data available in socket, or EOF */
				result |= WL_SOCKET_READABLE;
			}
			if ((wakeEvents & WL_SOCKET_WRITEABLE) && FD_ISSET(sock, &output_mask))
			{
				/* socket is writable, or EOF */
				result |= WL_SOCKET_WRITEABLE;
			}
			if ((wakeEvents & WL_POSTMASTER_DEATH) &&
				FD_ISSET(postmaster_alive_fds[POSTMASTER_FD_WATCH],
						 &input_mask))
			{
				/*
				 * According to the select(2) man page on Linux, select(2) may
				 * spuriously return and report a file descriptor as readable,
				 * when it's not; and presumably so can poll(2).  It's not
				 * clear that the relevant cases would ever apply to the
				 * postmaster pipe, but since the consequences of falsely
				 * returning WL_POSTMASTER_DEATH could be pretty unpleasant,
				 * we take the trouble to positively verify EOF with
				 * PostmasterIsAlive().
				 */
				if (!PostmasterIsAlive())
					result |= WL_POSTMASTER_DEATH;
			}
		}
#endif   /* LATCH_USE_SELECT */

		/*
		 * Check again whether latch is set, the arrival of a signal/self-byte
		 * might be what stopped our sleep. It's not required for correctness
		 * to signal the latch as being set (we'd just loop if there's no
		 * other event), but it seems good to report an arrived latch asap.
		 * This way we also don't have to compute the current timestamp again.
		 */
		if ((wakeEvents & WL_LATCH_SET) && latch->is_set)
			result |= WL_LATCH_SET;

		/* If we're not done, update cur_timeout for next iteration */
		if (result == 0 && (wakeEvents & WL_TIMEOUT))
		{
			INSTR_TIME_SET_CURRENT(cur_time);
			INSTR_TIME_SUBTRACT(cur_time, start_time);
			cur_timeout = timeout - (long) INSTR_TIME_GET_MILLISEC(cur_time);
			if (cur_timeout <= 0)
			{
				/* Timeout has expired, no need to continue looping */
				result |= WL_TIMEOUT;
			}
#ifdef LATCH_USE_SELECT
			else
			{
				tv.tv_sec = cur_timeout / 1000L;
				tv.tv_usec = (cur_timeout % 1000L) * 1000L;
			}
#endif
		}
	} while (result == 0);
	waiting = false;

	return result;
}

/*
 * Sets a latch and wakes up anyone waiting on it.
 *
 * This is cheap if the latch is already set, otherwise not so much.
 *
 * NB: when calling this in a signal handler, be sure to save and restore
 * errno around it.  (That's standard practice in most signal handlers, of
 * course, but we used to omit it in handlers that only set a flag.)
 *
 * NB: this function is called from critical sections and signal handlers so
 * throwing an error is not a good idea.
 */
void
SetLatch(volatile Latch *latch)
{
	pid_t		owner_pid;

	/*
	 * The memory barrier has be to be placed here to ensure that any flag
	 * variables possibly changed by this process have been flushed to main
	 * memory, before we check/set is_set.
	 */
	pg_memory_barrier();

	/* Quick exit if already set */
	if (latch->is_set)
		return;

	latch->is_set = true;

	/*
	 * See if anyone's waiting for the latch. It can be the current process if
	 * we're in a signal handler. We use the self-pipe to wake up the select()
	 * in that case. If it's another process, send a signal.
	 *
	 * Fetch owner_pid only once, in case the latch is concurrently getting
	 * owned or disowned. XXX: This assumes that pid_t is atomic, which isn't
	 * guaranteed to be true! In practice, the effective range of pid_t fits
	 * in a 32 bit integer, and so should be atomic. In the worst case, we
	 * might end up signaling the wrong process. Even then, you're very
	 * unlucky if a process with that bogus pid exists and belongs to
	 * Postgres; and PG database processes should handle excess SIGUSR1
	 * interrupts without a problem anyhow.
	 *
	 * Another sort of race condition that's possible here is for a new
	 * process to own the latch immediately after we look, so we don't signal
	 * it. This is okay so long as all callers of ResetLatch/WaitLatch follow
	 * the standard coding convention of waiting at the bottom of their loops,
	 * not the top, so that they'll correctly process latch-setting events
	 * that happen before they enter the loop.
	 */
	owner_pid = latch->owner_pid;
	if (owner_pid == 0)
		return;
	else if (owner_pid == MyProcPid)
	{
		if (waiting)
			sendSelfPipeByte();
	}
	else
		kill(owner_pid, SIGUSR1);
}

/*
 * Clear the latch. Calling WaitLatch after this will sleep, unless
 * the latch is set again before the WaitLatch call.
 */
void
ResetLatch(volatile Latch *latch)
{
	/* Only the owner should reset the latch */
	Assert(latch->owner_pid == MyProcPid);

	latch->is_set = false;

	/*
	 * Ensure that the write to is_set gets flushed to main memory before we
	 * examine any flag variables.  Otherwise a concurrent SetLatch might
	 * falsely conclude that it needn't signal us, even though we have missed
	 * seeing some flag updates that SetLatch was supposed to inform us of.
	 */
	pg_memory_barrier();
}

/*
 * SetLatch uses SIGUSR1 to wake up the process waiting on the latch.
 *
 * Wake up WaitLatch, if we're waiting.  (We might not be, since SIGUSR1 is
 * overloaded for multiple purposes; or we might not have reached WaitLatch
 * yet, in which case we don't need to fill the pipe either.)
 *
 * NB: when calling this in a signal handler, be sure to save and restore
 * errno around it.
 */
void
latch_sigusr1_handler(void)
{
	if (waiting)
		sendSelfPipeByte();
}

/* Send one byte to the self-pipe, to wake up WaitLatch */
static void
sendSelfPipeByte(void)
{
	int			rc;
	char		dummy = 0;

retry:
	rc = write(selfpipe_writefd, &dummy, 1);
	if (rc < 0)
	{
		/* If interrupted by signal, just retry */
		if (errno == EINTR)
			goto retry;

		/*
		 * If the pipe is full, we don't need to retry, the data that's there
		 * already is enough to wake up WaitLatch.
		 */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;

		/*
		 * Oops, the write() failed for some other reason. We might be in a
		 * signal handler, so it's not safe to elog(). We have no choice but
		 * silently ignore the error.
		 */
		return;
	}
}

/*
 * Read all available data from the self-pipe
 *
 * Note: this is only called when waiting = true.  If it fails and doesn't
 * return, it must reset that flag first (though ideally, this will never
 * happen).
 */
static void
drainSelfPipe(void)
{
	/*
	 * There shouldn't normally be more than one byte in the pipe, or maybe a
	 * few bytes if multiple processes run SetLatch at the same instant.
	 */
	char		buf[16];
	int			rc;

	for (;;)
	{
		rc = read(selfpipe_readfd, buf, sizeof(buf));
		if (rc < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;			/* the pipe is empty */
			else if (errno == EINTR)
				continue;		/* retry */
			else
			{
				waiting = false;
				elog(ERROR, "read() on self-pipe failed: %m");
			}
		}
		else if (rc == 0)
		{
			waiting = false;
			elog(ERROR, "unexpected EOF on self-pipe");
		}
		else if (rc < sizeof(buf))
		{
			/* we successfully drained the pipe; no need to read() again */
			break;
		}
		/* else buffer wasn't big enough, so read again */
	}
}
