/*-------------------------------------------------------------------------
 *
 * unix_latch.c
 *	  Routines for inter-process latches
 *
 * A latch is a boolean variable, with operations that let you to sleep
 * until it is set. A latch can be set from another process, or a signal
 * handler within the same process.
 *
 * The latch interface is a reliable replacement for the common pattern of
 * using pg_usleep() or select() to wait until a signal arrives, where the
 * signal handler sets a global variable. Because on some platforms, an
 * incoming signal doesn't interrupt sleep, and even on platforms where it
 * does there is a race condition if the signal arrives just before
 * entering the sleep, the common pattern must periodically wake up and
 * poll the global variable. pselect() system call was invented to solve
 * the problem, but it is not portable enough. Latches are designed to
 * overcome these limitations, allowing you to sleep without polling and
 * ensuring a quick response to signals from other processes.
 *
 * There are two kinds of latches: local and shared. A local latch is
 * initialized by InitLatch, and can only be set from the same process.
 * A local latch can be used to wait for a signal to arrive, by calling
 * SetLatch in the signal handler. A shared latch resides in shared memory,
 * and must be initialized at postmaster startup by InitSharedLatch. Before
 * a shared latch can be waited on, it must be associated with a process
 * with OwnLatch. Only the process owning the latch can wait on it, but any
 * process can set it.
 *
 * There are three basic operations on a latch:
 *
 * SetLatch		- Sets the latch
 * ResetLatch	- Clears the latch, allowing it to be set again
 * WaitLatch	- Waits for the latch to become set
 *
 * The correct pattern to wait for an event is:
 *
 * for (;;)
 * {
 *	   ResetLatch();
 *	   if (work to do)
 *		   Do Stuff();
 *
 *	   WaitLatch();
 * }
 *
 * It's important to reset the latch *before* checking if there's work to
 * do. Otherwise, if someone sets the latch between the check and the
 * ResetLatch call, you will miss it and Wait will block.
 *
 * To wake up the waiter, you must first set a global flag or something
 * else that the main loop tests in the "if (work to do)" part, and call
 * SetLatch *after* that. SetLatch is designed to return quickly if the
 * latch is already set.
 *
 *
 * Implementation
 * --------------
 *
 * The Unix implementation uses the so-called self-pipe trick to overcome
 * the race condition involved with select() and setting a global flag
 * in the signal handler. When a latch is set and the current process
 * is waiting for it, the signal handler wakes up the select() in
 * WaitLatch by writing a byte to a pipe. A signal by itself doesn't
 * interrupt select() on all platforms, and even on platforms where it
 * does, a signal that arrives just before the select() call does not
 * prevent the select() from entering sleep. An incoming byte on a pipe
 * however reliably interrupts the sleep, and makes select() to return
 * immediately if the signal arrives just before select() begins.
 *
 * When SetLatch is called from the same process that owns the latch,
 * SetLatch writes the byte directly to the pipe. If it's owned by another
 * process, SIGUSR1 is sent and the signal handler in the waiting process
 * writes the byte to the pipe on behalf of the signaling process.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/unix_latch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/shmem.h"

/* Are we currently in WaitLatch? The signal handler would like to know. */
static volatile sig_atomic_t waiting = false;

/* Read and write end of the self-pipe */
static int	selfpipe_readfd = -1;
static int	selfpipe_writefd = -1;

/* private function prototypes */
static void initSelfPipe(void);
static void drainSelfPipe(void);
static void sendSelfPipeByte(void);


/*
 * Initialize a backend-local latch.
 */
void
InitLatch(volatile Latch *latch)
{
	/* Initialize the self pipe if this is our first latch in the process */
	if (selfpipe_readfd == -1)
		initSelfPipe();

	latch->is_set = false;
	latch->owner_pid = MyProcPid;
	latch->is_shared = false;
}

/*
 * Initialize a shared latch that can be set from other processes. The latch
 * is initially owned by no-one, use OwnLatch to associate it with the
 * current process.
 *
 * InitSharedLatch needs to be called in postmaster before forking child
 * processes, usually right after allocating the shared memory block
 * containing the latch with ShmemInitStruct. The Unix implementation
 * doesn't actually require that, but the Windows one does.
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
 * wait on it.
 *
 * Make sure that latch_sigusr1_handler() is called from the SIGUSR1 signal
 * handler, as shared latches use SIGUSR1 to for inter-process communication.
 */
void
OwnLatch(volatile Latch *latch)
{
	Assert(latch->is_shared);

	/* Initialize the self pipe if this is our first latch in the process */
	if (selfpipe_readfd == -1)
		initSelfPipe();

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
 * Wait for given latch to be set or until timeout is exceeded.
 * If the latch is already set, the function returns immediately.
 *
 * The 'timeout' is given in microseconds, and -1 means wait forever.
 * On some platforms, signals cause the timeout to be restarted, so beware
 * that the function can sleep for several times longer than the specified
 * timeout.
 *
 * The latch must be owned by the current process, ie. it must be a
 * backend-local latch initialized with InitLatch, or a shared latch
 * associated with the current process by calling OwnLatch.
 *
 * Returns 'true' if the latch was set, or 'false' if timeout was reached.
 */
bool
WaitLatch(volatile Latch *latch, long timeout)
{
	return WaitLatchOrSocket(latch, PGINVALID_SOCKET, false, false, timeout) > 0;
}

/*
 * Like WaitLatch, but will also return when there's data available in
 * 'sock' for reading or writing. Returns 0 if timeout was reached,
 * 1 if the latch was set, 2 if the socket became readable or writable.
 */
int
WaitLatchOrSocket(volatile Latch *latch, pgsocket sock, bool forRead,
				  bool forWrite, long timeout)
{
	struct timeval tv,
			   *tvp = NULL;
	fd_set		input_mask;
	fd_set		output_mask;
	int			rc;
	int			result = 0;

	if (latch->owner_pid != MyProcPid)
		elog(ERROR, "cannot wait on a latch owned by another process");

	/* Initialize timeout */
	if (timeout >= 0)
	{
		tv.tv_sec = timeout / 1000000L;
		tv.tv_usec = timeout % 1000000L;
		tvp = &tv;
	}

	waiting = true;
	for (;;)
	{
		int			hifd;

		/*
		 * Clear the pipe, and check if the latch is set already. If someone
		 * sets the latch between this and the select() below, the setter will
		 * write a byte to the pipe (or signal us and the signal handler will
		 * do that), and the select() will return immediately.
		 */
		drainSelfPipe();
		if (latch->is_set)
		{
			result = 1;
			break;
		}

		FD_ZERO(&input_mask);
		FD_SET(selfpipe_readfd, &input_mask);
		hifd = selfpipe_readfd;
		if (sock != PGINVALID_SOCKET && forRead)
		{
			FD_SET(sock, &input_mask);
			if (sock > hifd)
				hifd = sock;
		}

		FD_ZERO(&output_mask);
		if (sock != PGINVALID_SOCKET && forWrite)
		{
			FD_SET(sock, &output_mask);
			if (sock > hifd)
				hifd = sock;
		}

		rc = select(hifd + 1, &input_mask, &output_mask, NULL, tvp);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("select() failed: %m")));
		}
		if (rc == 0)
		{
			/* timeout exceeded */
			result = 0;
			break;
		}
		if (sock != PGINVALID_SOCKET &&
			((forRead && FD_ISSET(sock, &input_mask)) ||
			 (forWrite && FD_ISSET(sock, &output_mask))))
		{
			result = 2;
			break;				/* data available in socket */
		}
	}
	waiting = false;

	return result;
}

/*
 * Sets a latch and wakes up anyone waiting on it. Returns quickly if the
 * latch is already set.
 */
void
SetLatch(volatile Latch *latch)
{
	pid_t		owner_pid;

	/* Quick exit if already set */
	if (latch->is_set)
		return;

	latch->is_set = true;

	/*
	 * See if anyone's waiting for the latch. It can be the current process if
	 * we're in a signal handler. We use the self-pipe to wake up the select()
	 * in that case. If it's another process, send a signal.
	 *
	 * Fetch owner_pid only once, in case the owner simultaneously disowns the
	 * latch and clears owner_pid. XXX: This assumes that pid_t is atomic,
	 * which isn't guaranteed to be true! In practice, the effective range of
	 * pid_t fits in a 32 bit integer, and so should be atomic. In the worst
	 * case, we might end up signaling wrong process if the right one disowns
	 * the latch just as we fetch owner_pid. Even then, you're very unlucky if
	 * a process with that bogus pid exists.
	 */
	owner_pid = latch->owner_pid;
	if (owner_pid == 0)
		return;
	else if (owner_pid == MyProcPid)
		sendSelfPipeByte();
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
}

/*
 * SetLatch uses SIGUSR1 to wake up the process waiting on the latch. Wake
 * up WaitLatch.
 */
void
latch_sigusr1_handler(void)
{
	if (waiting)
		sendSelfPipeByte();
}

/* initialize the self-pipe */
static void
initSelfPipe(void)
{
	int			pipefd[2];

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

/* Read all available data from the self-pipe */
static void
drainSelfPipe(void)
{
	/*
	 * There shouldn't normally be more than one byte in the pipe, or maybe a
	 * few more if multiple processes run SetLatch at the same instant.
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
				elog(ERROR, "read() on self-pipe failed: %m");
		}
		else if (rc == 0)
			elog(ERROR, "unexpected EOF on self-pipe");
	}
}
