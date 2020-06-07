/*-------------------------------------------------------------------------
 *
 *	scripts_parallel.c
 *		Parallel support for bin/scripts/
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/scripts_parallel.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef WIN32
#define FD_SETSIZE 1024			/* must set before winsock2.h is included */
#endif

#include "postgres_fe.h"

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "common.h"
#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "scripts_parallel.h"

static void init_slot(ParallelSlot *slot, PGconn *conn);
static int	select_loop(int maxFd, fd_set *workerset);

static void
init_slot(ParallelSlot *slot, PGconn *conn)
{
	slot->connection = conn;
	/* Initially assume connection is idle */
	slot->isFree = true;
}

/*
 * Wait until a file descriptor from the given set becomes readable.
 *
 * Returns the number of ready descriptors, or -1 on failure (including
 * getting a cancel request).
 */
static int
select_loop(int maxFd, fd_set *workerset)
{
	int			i;
	fd_set		saveSet = *workerset;

	if (CancelRequested)
		return -1;

	for (;;)
	{
		/*
		 * On Windows, we need to check once in a while for cancel requests;
		 * on other platforms we rely on select() returning when interrupted.
		 */
		struct timeval *tvp;
#ifdef WIN32
		struct timeval tv = {0, 1000000};

		tvp = &tv;
#else
		tvp = NULL;
#endif

		*workerset = saveSet;
		i = select(maxFd + 1, workerset, NULL, NULL, tvp);

#ifdef WIN32
		if (i == SOCKET_ERROR)
		{
			i = -1;

			if (WSAGetLastError() == WSAEINTR)
				errno = EINTR;
		}
#endif

		if (i < 0 && errno == EINTR)
			continue;			/* ignore this */
		if (i < 0 || CancelRequested)
			return -1;			/* but not this */
		if (i == 0)
			continue;			/* timeout (Win32 only) */
		break;
	}

	return i;
}

/*
 * ParallelSlotsGetIdle
 *		Return a connection slot that is ready to execute a command.
 *
 * This returns the first slot we find that is marked isFree, if one is;
 * otherwise, we loop on select() until one socket becomes available.  When
 * this happens, we read the whole set and mark as free all sockets that
 * become available.  If an error occurs, NULL is returned.
 */
ParallelSlot *
ParallelSlotsGetIdle(ParallelSlot *slots, int numslots)
{
	int			i;
	int			firstFree = -1;

	/*
	 * Look for any connection currently free.  If there is one, mark it as
	 * taken and let the caller know the slot to use.
	 */
	for (i = 0; i < numslots; i++)
	{
		if (slots[i].isFree)
		{
			slots[i].isFree = false;
			return slots + i;
		}
	}

	/*
	 * No free slot found, so wait until one of the connections has finished
	 * its task and return the available slot.
	 */
	while (firstFree < 0)
	{
		fd_set		slotset;
		int			maxFd = 0;

		/* We must reconstruct the fd_set for each call to select_loop */
		FD_ZERO(&slotset);

		for (i = 0; i < numslots; i++)
		{
			int			sock = PQsocket(slots[i].connection);

			/*
			 * We don't really expect any connections to lose their sockets
			 * after startup, but just in case, cope by ignoring them.
			 */
			if (sock < 0)
				continue;

			FD_SET(sock, &slotset);
			if (sock > maxFd)
				maxFd = sock;
		}

		SetCancelConn(slots->connection);
		i = select_loop(maxFd, &slotset);
		ResetCancelConn();

		/* failure? */
		if (i < 0)
			return NULL;

		for (i = 0; i < numslots; i++)
		{
			int			sock = PQsocket(slots[i].connection);

			if (sock >= 0 && FD_ISSET(sock, &slotset))
			{
				/* select() says input is available, so consume it */
				PQconsumeInput(slots[i].connection);
			}

			/* Collect result(s) as long as any are available */
			while (!PQisBusy(slots[i].connection))
			{
				PGresult   *result = PQgetResult(slots[i].connection);

				if (result != NULL)
				{
					/* Check and discard the command result */
					if (!processQueryResult(slots[i].connection, result))
						return NULL;
				}
				else
				{
					/* This connection has become idle */
					slots[i].isFree = true;
					if (firstFree < 0)
						firstFree = i;
					break;
				}
			}
		}
	}

	slots[firstFree].isFree = false;
	return slots + firstFree;
}

/*
 * ParallelSlotsSetup
 *		Prepare a set of parallel slots to use on a given database.
 *
 * This creates and initializes a set of connections to the database
 * using the information given by the caller, marking all parallel slots
 * as free and ready to use.  "conn" is an initial connection set up
 * by the caller and is associated with the first slot in the parallel
 * set.
 */
ParallelSlot *
ParallelSlotsSetup(const char *dbname, const char *host, const char *port,
				   const char *username, bool prompt_password,
				   const char *progname, bool echo,
				   PGconn *conn, int numslots)
{
	ParallelSlot *slots;
	int			i;

	Assert(conn != NULL);

	slots = (ParallelSlot *) pg_malloc(sizeof(ParallelSlot) * numslots);
	init_slot(slots, conn);
	if (numslots > 1)
	{
		for (i = 1; i < numslots; i++)
		{
			conn = connectDatabase(dbname, host, port, username, prompt_password,
								   progname, echo, false, true);

			/*
			 * Fail and exit immediately if trying to use a socket in an
			 * unsupported range.  POSIX requires open(2) to use the lowest
			 * unused file descriptor and the hint given relies on that.
			 */
			if (PQsocket(conn) >= FD_SETSIZE)
			{
				pg_log_fatal("too many jobs for this platform -- try %d", i);
				exit(1);
			}

			init_slot(slots + i, conn);
		}
	}

	return slots;
}

/*
 * ParallelSlotsTerminate
 *		Clean up a set of parallel slots
 *
 * Iterate through all connections in a given set of ParallelSlots and
 * terminate all connections.
 */
void
ParallelSlotsTerminate(ParallelSlot *slots, int numslots)
{
	int			i;

	for (i = 0; i < numslots; i++)
	{
		PGconn	   *conn = slots[i].connection;

		if (conn == NULL)
			continue;

		disconnectDatabase(conn);
	}
}

/*
 * ParallelSlotsWaitCompletion
 *
 * Wait for all connections to finish, returning false if at least one
 * error has been found on the way.
 */
bool
ParallelSlotsWaitCompletion(ParallelSlot *slots, int numslots)
{
	int			i;

	for (i = 0; i < numslots; i++)
	{
		if (!consumeQueryResult((slots + i)->connection))
			return false;
	}

	return true;
}
