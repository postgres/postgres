/* ----------
 * pgstat.c
 *
 *	All the statistics collector stuff hacked up in one big, ugly file.
 *
 *	TODO:	- Separate collector, postmaster and backend stuff
 *			  into different files.
 *
 *			- Add some automatic call for pgstat vacuuming.
 *
 *			- Add a pgstat config column to pg_database, so this
 *			  entire thing can be enabled/disabled on a per db basis.
 *
 *	Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/backend/postmaster/pgstat.c,v 1.77 2004/07/01 00:50:36 tgl Exp $
 * ----------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>

#include "pgstat.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "tcop/tcopprot.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* ----------
 * Paths for the statistics files. The %s is replaced with the
 * installation's $PGDATA.
 * ----------
 */
#define PGSTAT_STAT_FILENAME	"%s/global/pgstat.stat"
#define PGSTAT_STAT_TMPFILE		"%s/global/pgstat.tmp.%d"

/* ----------
 * Timer definitions.
 * ----------
 */
#define PGSTAT_STAT_INTERVAL	500		/* How often to write the status
										 * file; in milliseconds. */

#define PGSTAT_DESTROY_DELAY	10000	/* How long to keep destroyed
										 * objects known, to give delayed
										 * UDP packets time to arrive;
										 * in milliseconds. */

#define PGSTAT_DESTROY_COUNT	(PGSTAT_DESTROY_DELAY / PGSTAT_STAT_INTERVAL)

#define PGSTAT_RESTART_INTERVAL 60		/* How often to attempt to restart
										 * a failed statistics collector;
										 * in seconds. */

/* ----------
 * Amount of space reserved in pgstat_recvbuffer().
 * ----------
 */
#define PGSTAT_RECVBUFFERSZ		((int) (1024 * sizeof(PgStat_Msg)))

/* ----------
 * The initial size hints for the hash tables used in the collector.
 * ----------
 */
#define PGSTAT_DB_HASH_SIZE		16
#define PGSTAT_BE_HASH_SIZE		512
#define PGSTAT_TAB_HASH_SIZE	512


/* ----------
 * GUC parameters
 * ----------
 */
bool		pgstat_collect_startcollector = true;
bool		pgstat_collect_resetonpmstart = true;
bool		pgstat_collect_querystring = false;
bool		pgstat_collect_tuplelevel = false;
bool		pgstat_collect_blocklevel = false;

/* ----------
 * Local data
 * ----------
 */
NON_EXEC_STATIC int	pgStatSock = -1;
static int	pgStatPipe[2];
static struct sockaddr_storage pgStatAddr;

static time_t last_pgstat_start_time;

static long pgStatNumMessages = 0;

static bool pgStatRunningInCollector = FALSE;

static int	pgStatTabstatAlloc = 0;
static int	pgStatTabstatUsed = 0;
static PgStat_MsgTabstat **pgStatTabstatMessages = NULL;

#define TABSTAT_QUANTUM		4	/* we alloc this many at a time */

static int	pgStatXactCommit = 0;
static int	pgStatXactRollback = 0;

static TransactionId pgStatDBHashXact = InvalidTransactionId;
static HTAB *pgStatDBHash = NULL;
static HTAB *pgStatBeDead = NULL;
static PgStat_StatBeEntry *pgStatBeTable = NULL;
static int	pgStatNumBackends = 0;

static char pgStat_fname[MAXPGPATH];
static char pgStat_tmpfname[MAXPGPATH];


/* ----------
 * Local function forward declarations
 * ----------
 */
#ifdef EXEC_BACKEND

typedef enum STATS_PROCESS_TYPE
{
	STAT_PROC_BUFFER,
	STAT_PROC_COLLECTOR
} STATS_PROCESS_TYPE;

static pid_t pgstat_forkexec(STATS_PROCESS_TYPE procType);
static void pgstat_parseArgs(int argc, char *argv[]);

#endif

NON_EXEC_STATIC void PgstatBufferMain(int argc, char *argv[]);
NON_EXEC_STATIC void PgstatCollectorMain(int argc, char *argv[]);
static void pgstat_recvbuffer(void);
static void pgstat_exit(SIGNAL_ARGS);
static void pgstat_die(SIGNAL_ARGS);

static int	pgstat_add_backend(PgStat_MsgHdr *msg);
static void pgstat_sub_backend(int procpid);
static void pgstat_drop_database(Oid databaseid);
static void pgstat_write_statsfile(void);
static void pgstat_read_statsfile(HTAB **dbhash, Oid onlydb,
					  PgStat_StatBeEntry **betab,
					  int *numbackends);
static void backend_read_statsfile(void);

static void pgstat_setheader(PgStat_MsgHdr *hdr, int mtype);
static void pgstat_send(void *msg, int len);

static void pgstat_recv_bestart(PgStat_MsgBestart *msg, int len);
static void pgstat_recv_beterm(PgStat_MsgBeterm *msg, int len);
static void pgstat_recv_activity(PgStat_MsgActivity *msg, int len);
static void pgstat_recv_tabstat(PgStat_MsgTabstat *msg, int len);
static void pgstat_recv_tabpurge(PgStat_MsgTabpurge *msg, int len);
static void pgstat_recv_dropdb(PgStat_MsgDropdb *msg, int len);
static void pgstat_recv_resetcounter(PgStat_MsgResetcounter *msg, int len);


/* ------------------------------------------------------------
 * Public functions called from postmaster follow
 * ------------------------------------------------------------
 */

/* ----------
 * pgstat_init() -
 *
 *	Called from postmaster at startup. Create the resources required
 *	by the statistics collector process.  If unable to do so, do not
 *	fail --- better to let the postmaster start with stats collection
 *	disabled.
 * ----------
 */
void
pgstat_init(void)
{
	ACCEPT_TYPE_ARG3 alen;
	struct addrinfo *addrs = NULL,
			   *addr,
				hints;
	int			ret;
	fd_set      rset;
	struct timeval tv;
	char        test_byte;
	int         sel_res;

#define TESTBYTEVAL ((char) 199)

	/*
	 * Force start of collector daemon if something to collect
	 */
	if (pgstat_collect_querystring ||
		pgstat_collect_tuplelevel ||
		pgstat_collect_blocklevel)
		pgstat_collect_startcollector = true;

	/*
	 * Initialize the filename for the status reports.  (In the EXEC_BACKEND
	 * case, this only sets the value in the postmaster.  The collector
	 * subprocess will recompute the value for itself, and individual
	 * backends must do so also if they want to access the file.)
	 */
	snprintf(pgStat_fname, MAXPGPATH, PGSTAT_STAT_FILENAME, DataDir);

	/*
	 * If we don't have to start a collector or should reset the collected
	 * statistics on postmaster start, simply remove the file.
	 */
	if (!pgstat_collect_startcollector || pgstat_collect_resetonpmstart)
		unlink(pgStat_fname);

	/*
	 * Nothing else required if collector will not get started
	 */
	if (!pgstat_collect_startcollector)
		return;

	/*
	 * Create the UDP socket for sending and receiving statistic messages
	 */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_addr = NULL;
	hints.ai_canonname = NULL;
	hints.ai_next = NULL;
	ret = getaddrinfo_all("localhost", NULL, &hints, &addrs);
	if (ret || !addrs)
	{
		ereport(LOG,
				(errmsg("could not resolve \"localhost\": %s",
						gai_strerror(ret))));
		goto startup_failed;
	}

	/*
	 * On some platforms, getaddrinfo_all() may return multiple addresses
	 * only one of which will actually work (eg, both IPv6 and IPv4 addresses
	 * when kernel will reject IPv6).  Worse, the failure may occur at the
	 * bind() or perhaps even connect() stage.  So we must loop through the
	 * results till we find a working combination.  We will generate LOG
	 * messages, but no error, for bogus combinations.
	 */
	for (addr = addrs; addr; addr = addr->ai_next)
	{
#ifdef HAVE_UNIX_SOCKETS
		/* Ignore AF_UNIX sockets, if any are returned. */
		if (addr->ai_family == AF_UNIX)
			continue;
#endif
		/*
		 * Create the socket.
		 */
		if ((pgStatSock = socket(addr->ai_family, SOCK_DGRAM, 0)) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not create socket for statistics collector: %m")));
			continue;
		}

		/*
		 * Bind it to a kernel assigned port on localhost and get the assigned
		 * port via getsockname().
		 */
		if (bind(pgStatSock, addr->ai_addr, addr->ai_addrlen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not bind socket for statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		alen = sizeof(pgStatAddr);
		if (getsockname(pgStatSock, (struct sockaddr *) &pgStatAddr, &alen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not get address of socket for statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		/*
		 * Connect the socket to its own address.  This saves a few cycles by
		 * not having to respecify the target address on every send. This also
		 * provides a kernel-level check that only packets from this same
		 * address will be received.
		 */
		if (connect(pgStatSock, (struct sockaddr *) &pgStatAddr, alen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not connect socket for statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		/*
		 * Try to send and receive a one-byte test message on the socket.
		 * This is to catch situations where the socket can be created but
		 * will not actually pass data (for instance, because kernel packet
		 * filtering rules prevent it).
		 */
		test_byte = TESTBYTEVAL;
		if (send(pgStatSock, &test_byte, 1, 0) != 1)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not send test message on socket for statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		/*
		 * There could possibly be a little delay before the message can be
		 * received.  We arbitrarily allow up to half a second before deciding
		 * it's broken.
		 */
		for (;;)				/* need a loop to handle EINTR */
		{
			FD_ZERO(&rset);
			FD_SET(pgStatSock, &rset);
			tv.tv_sec = 0;
			tv.tv_usec = 500000;
			sel_res = select(pgStatSock+1, &rset, NULL, NULL, &tv);
			if (sel_res >= 0 || errno != EINTR)
				break;
		}
		if (sel_res < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("select() failed in statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}
		if (sel_res == 0 || !FD_ISSET(pgStatSock, &rset))
		{
			/*
			 * This is the case we actually think is likely, so take pains to
			 * give a specific message for it.
			 *
			 * errno will not be set meaningfully here, so don't use it.
			 */
			ereport(LOG,
					(ERRCODE_CONNECTION_FAILURE,
					 errmsg("test message did not get through on socket for statistics collector")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		test_byte++;			/* just make sure variable is changed */

		if (recv(pgStatSock, &test_byte, 1, 0) != 1)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not receive test message on socket for statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		if (test_byte != TESTBYTEVAL) /* strictly paranoia ... */
		{
			ereport(LOG,
					(ERRCODE_INTERNAL_ERROR,
					 errmsg("incorrect test message transmission on socket for statistics collector")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		/* If we get here, we have a working socket */
		break;
	}

	/* Did we find a working address? */
	if (!addr || pgStatSock < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("disabling statistics collector for lack of working socket")));
		goto startup_failed;
	}

	/*
	 * Set the socket to non-blocking IO.  This ensures that if the
	 * collector falls behind (despite the buffering process), statistics
	 * messages will be discarded; backends won't block waiting to send
	 * messages to the collector.
	 */
	if (!set_noblock(pgStatSock))
	{
		ereport(LOG,
				(errcode_for_socket_access(),
		errmsg("could not set statistics collector socket to nonblocking mode: %m")));
		goto startup_failed;
	}

	freeaddrinfo_all(hints.ai_family, addrs);

	return;

startup_failed:
	if (addrs)
		freeaddrinfo_all(hints.ai_family, addrs);

	if (pgStatSock >= 0)
		closesocket(pgStatSock);
	pgStatSock = -1;

	/* Adjust GUC variables to suppress useless activity */
	pgstat_collect_startcollector = false;
	pgstat_collect_querystring = false;
	pgstat_collect_tuplelevel = false;
	pgstat_collect_blocklevel = false;
}


#ifdef EXEC_BACKEND

/*
 * pgstat_forkexec() -
 *
 * Format up the arglist for, then fork and exec, statistics
 * (buffer and collector) processes
 */
static pid_t
pgstat_forkexec(STATS_PROCESS_TYPE procType)
{
	char *av[10];
	int ac = 0, bufc = 0, i;
	char pgstatBuf[2][32];

	av[ac++] = "postgres";

	switch (procType)
	{
		case STAT_PROC_BUFFER:
			av[ac++] = "-forkbuf";
			break;

		case STAT_PROC_COLLECTOR:
			av[ac++] = "-forkcol";
			break;

		default:
			Assert(false);
	}

	av[ac++] = NULL;			/* filled in by postmaster_forkexec */

	/* postgres_exec_path is not passed by write_backend_variables */
	av[ac++] = postgres_exec_path;

	/* Pipe file ids (those not passed by write_backend_variables) */
	snprintf(pgstatBuf[bufc++],32,"%d",pgStatPipe[0]);
	snprintf(pgstatBuf[bufc++],32,"%d",pgStatPipe[1]);

	/* Add to the arg list */
	Assert(bufc <= lengthof(pgstatBuf));
	for (i = 0; i < bufc; i++)
		av[ac++] = pgstatBuf[i];

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}


/*
 * pgstat_parseArgs() -
 *
 * Extract data from the arglist for exec'ed statistics
 * (buffer and collector) processes
 */
static void
pgstat_parseArgs(int argc, char *argv[])
{
	Assert(argc == 6);

	argc = 3;
	StrNCpy(postgres_exec_path,	argv[argc++], MAXPGPATH);
	pgStatPipe[0]	= atoi(argv[argc++]);
	pgStatPipe[1]	= atoi(argv[argc++]);
}

#endif /* EXEC_BACKEND */


/* ----------
 * pgstat_start() -
 *
 *	Called from postmaster at startup or after an existing collector
 *	died.  Attempt to fire up a fresh statistics collector.
 *
 *	Returns PID of child process, or 0 if fail.
 *
 *	Note: if fail, we will be called again from the postmaster main loop.
 * ----------
 */
int
pgstat_start(void)
{
	time_t		curtime;
	pid_t		pgStatPid;

	/*
	 * Do nothing if no collector needed
	 */
	if (!pgstat_collect_startcollector)
		return 0;

	/*
	 * Do nothing if too soon since last collector start.  This is a
	 * safety valve to protect against continuous respawn attempts if the
	 * collector is dying immediately at launch.  Note that since we will
	 * be re-called from the postmaster main loop, we will get another
	 * chance later.
	 */
	curtime = time(NULL);
	if ((unsigned int) (curtime - last_pgstat_start_time) <
		(unsigned int) PGSTAT_RESTART_INTERVAL)
		return 0;
	last_pgstat_start_time = curtime;

	/*
	 * Check that the socket is there, else pgstat_init failed.
	 */
	if (pgStatSock < 0)
	{
		ereport(LOG,
				(errmsg("statistics collector startup skipped")));

		/*
		 * We can only get here if someone tries to manually turn
		 * pgstat_collect_startcollector on after it had been off.
		 */
		pgstat_collect_startcollector = false;
		return 0;
	}

	/*
	 * Okay, fork off the collector.
	 */

	fflush(stdout);
	fflush(stderr);

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

#ifdef EXEC_BACKEND
	switch ((pgStatPid = pgstat_forkexec(STAT_PROC_BUFFER)))
#else
	switch ((pgStatPid = fork()))
#endif
	{
		case -1:
#ifdef __BEOS__
			/* Specific beos actions */
			beos_backend_startup_failed();
#endif
			ereport(LOG,
					(errmsg("could not fork statistics buffer: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
#ifdef __BEOS__
			/* Specific beos actions after backend startup */
			beos_backend_startup();
#endif
			/* Close the postmaster's sockets */
			ClosePostmasterPorts();

			/* Drop our connection to postmaster's shared memory, as well */
			PGSharedMemoryDetach();

			PgstatBufferMain(0, NULL);
			break;
#endif

		default:
			return (int) pgStatPid;
	}

	/* shouldn't get here */
	return 0;
}


/* ----------
 * pgstat_beterm() -
 *
 *	Called from postmaster to tell collector a backend terminated.
 * ----------
 */
void
pgstat_beterm(int pid)
{
	PgStat_MsgBeterm msg;

	if (pgStatSock < 0)
		return;

	MemSet(&(msg.m_hdr), 0, sizeof(msg.m_hdr));
	msg.m_hdr.m_type = PGSTAT_MTYPE_BETERM;
	msg.m_hdr.m_procpid = pid;

	pgstat_send(&msg, sizeof(msg));
}


/* ------------------------------------------------------------
 * Public functions used by backends follow
 *------------------------------------------------------------
 */


/* ----------
 * pgstat_bestart() -
 *
 *	Tell the collector that this new backend is soon ready to process
 *	queries. Called from tcop/postgres.c before entering the mainloop.
 * ----------
 */
void
pgstat_bestart(void)
{
	PgStat_MsgBestart msg;

	if (pgStatSock < 0)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_BESTART);
	pgstat_send(&msg, sizeof(msg));
}


/* ----------
 * pgstat_report_activity() -
 *
 *	Called in tcop/postgres.c to tell the collector what the backend
 *	is actually doing (usually "<IDLE>" or the start of the query to
 *	be executed).
 * ----------
 */
void
pgstat_report_activity(const char *what)
{
	PgStat_MsgActivity msg;
	int			len;

	if (!pgstat_collect_querystring || pgStatSock < 0)
		return;

	len = strlen(what);
	len = pg_mbcliplen((const unsigned char *) what, len,
					   PGSTAT_ACTIVITY_SIZE - 1);

	memcpy(msg.m_what, what, len);
	msg.m_what[len] = '\0';
	len += offsetof(PgStat_MsgActivity, m_what) +1;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_ACTIVITY);
	pgstat_send(&msg, len);
}


/* ----------
 * pgstat_report_tabstat() -
 *
 *	Called from tcop/postgres.c to send the so far collected
 *	per table access statistics to the collector.
 * ----------
 */
void
pgstat_report_tabstat(void)
{
	int			i;

	if (pgStatSock < 0 ||
		!(pgstat_collect_querystring ||
		  pgstat_collect_tuplelevel ||
		  pgstat_collect_blocklevel))
	{
		/* Not reporting stats, so just flush whatever we have */
		pgStatTabstatUsed = 0;
		return;
	}

	/*
	 * For each message buffer used during the last query set the header
	 * fields and send it out.
	 */
	for (i = 0; i < pgStatTabstatUsed; i++)
	{
		PgStat_MsgTabstat *tsmsg = pgStatTabstatMessages[i];
		int			n;
		int			len;

		n = tsmsg->m_nentries;
		len = offsetof(PgStat_MsgTabstat, m_entry[0]) +
			n * sizeof(PgStat_TableEntry);

		tsmsg->m_xact_commit = pgStatXactCommit;
		tsmsg->m_xact_rollback = pgStatXactRollback;
		pgStatXactCommit = 0;
		pgStatXactRollback = 0;

		pgstat_setheader(&tsmsg->m_hdr, PGSTAT_MTYPE_TABSTAT);
		pgstat_send(tsmsg, len);
	}

	pgStatTabstatUsed = 0;
}


/* ----------
 * pgstat_vacuum_tabstat() -
 *
 *	Will tell the collector about objects he can get rid of.
 * ----------
 */
int
pgstat_vacuum_tabstat(void)
{
	Relation	dbrel;
	HeapScanDesc dbscan;
	HeapTuple	dbtup;
	Oid		   *dbidlist;
	int			dbidalloc;
	int			dbidused;
	HASH_SEQ_STATUS hstat;
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	HeapTuple	reltup;
	int			nobjects = 0;
	PgStat_MsgTabpurge msg;
	int			len;
	int			i;

	if (pgStatSock < 0)
		return 0;

	/*
	 * If not done for this transaction, read the statistics collector
	 * stats file into some hash tables.
	 */
	backend_read_statsfile();

	/*
	 * Lookup our own database entry
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
												 (void *) &MyDatabaseId,
												 HASH_FIND, NULL);
	if (dbentry == NULL)
		return -1;

	if (dbentry->tables == NULL)
		return 0;

	/*
	 * Initialize our messages table counter to zero
	 */
	msg.m_nentries = 0;

	/*
	 * Check for all tables if they still exist.
	 */
	hash_seq_init(&hstat, dbentry->tables);
	while ((tabentry = (PgStat_StatTabEntry *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * Check if this relation is still alive by looking up it's
		 * pg_class tuple in the system catalog cache.
		 */
		reltup = SearchSysCache(RELOID,
								ObjectIdGetDatum(tabentry->tableid),
								0, 0, 0);
		if (HeapTupleIsValid(reltup))
		{
			ReleaseSysCache(reltup);
			continue;
		}

		/*
		 * Add this tables Oid to the message
		 */
		msg.m_tableid[msg.m_nentries++] = tabentry->tableid;
		nobjects++;

		/*
		 * If the message is full, send it out and reinitialize ot zero
		 */
		if (msg.m_nentries >= PGSTAT_NUM_TABPURGE)
		{
			len = offsetof(PgStat_MsgTabpurge, m_tableid[0])
				+msg.m_nentries * sizeof(Oid);

			pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_TABPURGE);
			pgstat_send(&msg, len);

			msg.m_nentries = 0;
		}
	}

	/*
	 * Send the rest
	 */
	if (msg.m_nentries > 0)
	{
		len = offsetof(PgStat_MsgTabpurge, m_tableid[0])
			+msg.m_nentries * sizeof(Oid);

		pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_TABPURGE);
		pgstat_send(&msg, len);
	}

	/*
	 * Read pg_database and remember the Oid's of all existing databases
	 */
	dbidalloc = 256;
	dbidused = 0;
	dbidlist = (Oid *) palloc(sizeof(Oid) * dbidalloc);

	dbrel = heap_openr(DatabaseRelationName, AccessShareLock);
	dbscan = heap_beginscan(dbrel, SnapshotNow, 0, NULL);
	while ((dbtup = heap_getnext(dbscan, ForwardScanDirection)) != NULL)
	{
		if (dbidused >= dbidalloc)
		{
			dbidalloc *= 2;
			dbidlist = (Oid *) repalloc((char *) dbidlist,
										sizeof(Oid) * dbidalloc);
		}
		dbidlist[dbidused++] = HeapTupleGetOid(dbtup);
	}
	heap_endscan(dbscan);
	heap_close(dbrel, AccessShareLock);

	/*
	 * Search the database hash table for dead databases and tell the
	 * collector to drop them as well.
	 */
	hash_seq_init(&hstat, pgStatDBHash);
	while ((dbentry = (PgStat_StatDBEntry *) hash_seq_search(&hstat)) != NULL)
	{
		Oid			dbid = dbentry->databaseid;

		for (i = 0; i < dbidused; i++)
		{
			if (dbidlist[i] == dbid)
			{
				dbid = InvalidOid;
				break;
			}
		}

		if (dbid != InvalidOid)
		{
			nobjects++;
			pgstat_drop_database(dbid);
		}
	}

	/*
	 * Free the dbid list.
	 */
	pfree((char *) dbidlist);

	/*
	 * Tell the caller how many removeable objects we found
	 */
	return nobjects;
}


/* ----------
 * pgstat_drop_database() -
 *
 *	Tell the collector that we just dropped a database.
 *	This is the only message that shouldn't get lost in space. Otherwise
 *	the collector will keep the statistics for the dead DB until his
 *	stats file got removed while the postmaster is down.
 * ----------
 */
static void
pgstat_drop_database(Oid databaseid)
{
	PgStat_MsgDropdb msg;

	if (pgStatSock < 0)
		return;

	msg.m_databaseid = databaseid;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_DROPDB);
	pgstat_send(&msg, sizeof(msg));
}


/* ----------
 * pgstat_reset_counters() -
 *
 *	Tell the statistics collector to reset counters for our database.
 * ----------
 */
void
pgstat_reset_counters(void)
{
	PgStat_MsgResetcounter msg;

	if (pgStatSock < 0)
		return;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			  errmsg("must be superuser to reset statistics counters")));

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RESETCOUNTER);
	pgstat_send(&msg, sizeof(msg));
}


/* ----------
 * pgstat_ping() -
 *
 *	Send some junk data to the collector to increase traffic.
 * ----------
 */
void
pgstat_ping(void)
{
	PgStat_MsgDummy msg;

	if (pgStatSock < 0)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_DUMMY);
	pgstat_send(&msg, sizeof(msg));
}

/*
 * Create or enlarge the pgStatTabstatMessages array
 */
static bool
more_tabstat_space(void)
{
	PgStat_MsgTabstat *newMessages;
	PgStat_MsgTabstat **msgArray;
	int			newAlloc = pgStatTabstatAlloc + TABSTAT_QUANTUM;
	int			i;

	/* Create (another) quantum of message buffers */
	newMessages = (PgStat_MsgTabstat *)
		malloc(sizeof(PgStat_MsgTabstat) * TABSTAT_QUANTUM);
	if (newMessages == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return false;
	}

	/* Create or enlarge the pointer array */
	if (pgStatTabstatMessages == NULL)
		msgArray = (PgStat_MsgTabstat **)
			malloc(sizeof(PgStat_MsgTabstat *) * newAlloc);
	else
		msgArray = (PgStat_MsgTabstat **)
			realloc(pgStatTabstatMessages,
					sizeof(PgStat_MsgTabstat *) * newAlloc);
	if (msgArray == NULL)
	{
		free(newMessages);
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return false;
	}

	MemSet(newMessages, 0, sizeof(PgStat_MsgTabstat) * TABSTAT_QUANTUM);
	for (i = 0; i < TABSTAT_QUANTUM; i++)
		msgArray[pgStatTabstatAlloc + i] = newMessages++;
	pgStatTabstatMessages = msgArray;
	pgStatTabstatAlloc = newAlloc;

	return true;
}

/* ----------
 * pgstat_initstats() -
 *
 *	Called from various places usually dealing with initialization
 *	of Relation or Scan structures. The data placed into these
 *	structures from here tell where later to count for buffer reads,
 *	scans and tuples fetched.
 * ----------
 */
void
pgstat_initstats(PgStat_Info *stats, Relation rel)
{
	Oid			rel_id = rel->rd_id;
	PgStat_TableEntry *useent;
	PgStat_MsgTabstat *tsmsg;
	int			mb;
	int			i;

	/*
	 * Initialize data not to count at all.
	 */
	stats->tabentry = NULL;
	stats->no_stats = FALSE;
	stats->heap_scan_counted = FALSE;
	stats->index_scan_counted = FALSE;

	if (pgStatSock < 0 ||
		!(pgstat_collect_tuplelevel ||
		  pgstat_collect_blocklevel))
	{
		stats->no_stats = TRUE;
		return;
	}

	/*
	 * Search the already-used message slots for this relation.
	 */
	for (mb = 0; mb < pgStatTabstatUsed; mb++)
	{
		tsmsg = pgStatTabstatMessages[mb];

		for (i = tsmsg->m_nentries; --i >= 0; )
		{
			if (tsmsg->m_entry[i].t_id == rel_id)
			{
				stats->tabentry = (void *) &(tsmsg->m_entry[i]);
				return;
			}
		}

		if (tsmsg->m_nentries >= PGSTAT_NUM_TABENTRIES)
			continue;

		/*
		 * Not found, but found a message buffer with an empty slot
		 * instead. Fine, let's use this one.
		 */
		i = tsmsg->m_nentries++;
		useent = &tsmsg->m_entry[i];
		MemSet(useent, 0, sizeof(PgStat_TableEntry));
		useent->t_id = rel_id;
		stats->tabentry = (void *) useent;
		return;
	}

	/*
	 * If we ran out of message buffers, we just allocate more.
	 */
	if (pgStatTabstatUsed >= pgStatTabstatAlloc)
	{
		if (!more_tabstat_space())
		{
			stats->no_stats = TRUE;
			return;
		}
		Assert(pgStatTabstatUsed < pgStatTabstatAlloc);
	}

	/*
	 * Use the first entry of the next message buffer.
	 */
	mb = pgStatTabstatUsed++;
	tsmsg = pgStatTabstatMessages[mb];
	tsmsg->m_nentries = 1;
	useent = &tsmsg->m_entry[0];
	MemSet(useent, 0, sizeof(PgStat_TableEntry));
	useent->t_id = rel_id;
	stats->tabentry = (void *) useent;
}


/* ----------
 * pgstat_count_xact_commit() -
 *
 *	Called from access/transam/xact.c to count transaction commits.
 * ----------
 */
void
pgstat_count_xact_commit(void)
{
	if (!(pgstat_collect_querystring ||
		  pgstat_collect_tuplelevel ||
		  pgstat_collect_blocklevel))
		return;

	pgStatXactCommit++;

	/*
	 * If there was no relation activity yet, just make one existing
	 * message buffer used without slots, causing the next report to tell
	 * new xact-counters.
	 */
	if (pgStatTabstatAlloc == 0)
	{
		if (!more_tabstat_space())
			return;
	}
	if (pgStatTabstatUsed == 0)
	{
		pgStatTabstatUsed++;
		pgStatTabstatMessages[0]->m_nentries = 0;
	}
}


/* ----------
 * pgstat_count_xact_rollback() -
 *
 *	Called from access/transam/xact.c to count transaction rollbacks.
 * ----------
 */
void
pgstat_count_xact_rollback(void)
{
	if (!(pgstat_collect_querystring ||
		  pgstat_collect_tuplelevel ||
		  pgstat_collect_blocklevel))
		return;

	pgStatXactRollback++;

	/*
	 * If there was no relation activity yet, just make one existing
	 * message buffer used without slots, causing the next report to tell
	 * new xact-counters.
	 */
	if (pgStatTabstatAlloc == 0)
	{
		if (!more_tabstat_space())
			return;
	}
	if (pgStatTabstatUsed == 0)
	{
		pgStatTabstatUsed++;
		pgStatTabstatMessages[0]->m_nentries = 0;
	}
}


/* ----------
 * pgstat_fetch_stat_dbentry() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	the collected statistics for one database or NULL. NULL doesn't mean
 *	that the database doesn't exist, it is just not yet known by the
 *	collector, so the caller is better off to report ZERO instead.
 * ----------
 */
PgStat_StatDBEntry *
pgstat_fetch_stat_dbentry(Oid dbid)
{
	PgStat_StatDBEntry *dbentry;

	/*
	 * If not done for this transaction, read the statistics collector
	 * stats file into some hash tables.
	 */
	backend_read_statsfile();

	/*
	 * Lookup the requested database
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
												 (void *) &dbid,
												 HASH_FIND, NULL);
	if (dbentry == NULL)
		return NULL;

	return dbentry;
}


/* ----------
 * pgstat_fetch_stat_tabentry() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	the collected statistics for one table or NULL. NULL doesn't mean
 *	that the table doesn't exist, it is just not yet known by the
 *	collector, so the caller is better off to report ZERO instead.
 * ----------
 */
PgStat_StatTabEntry *
pgstat_fetch_stat_tabentry(Oid relid)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;

	/*
	 * If not done for this transaction, read the statistics collector
	 * stats file into some hash tables.
	 */
	backend_read_statsfile();

	/*
	 * Lookup our database.
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
												 (void *) &MyDatabaseId,
												 HASH_FIND, NULL);
	if (dbentry == NULL)
		return NULL;

	/*
	 * Now inside the DB's table hash table lookup the requested one.
	 */
	if (dbentry->tables == NULL)
		return NULL;
	tabentry = (PgStat_StatTabEntry *) hash_search(dbentry->tables,
												   (void *) &relid,
												   HASH_FIND, NULL);
	if (tabentry == NULL)
		return NULL;

	return tabentry;
}


/* ----------
 * pgstat_fetch_stat_beentry() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	the actual activity slot of one active backend. The caller is
 *	responsible for a check if the actual user is permitted to see
 *	that info (especially the querystring).
 * ----------
 */
PgStat_StatBeEntry *
pgstat_fetch_stat_beentry(int beid)
{
	backend_read_statsfile();

	if (beid < 1 || beid > pgStatNumBackends)
		return NULL;

	return &pgStatBeTable[beid - 1];
}


/* ----------
 * pgstat_fetch_stat_numbackends() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	the maximum current backend id.
 * ----------
 */
int
pgstat_fetch_stat_numbackends(void)
{
	backend_read_statsfile();

	return pgStatNumBackends;
}



/* ------------------------------------------------------------
 * Local support functions follow
 * ------------------------------------------------------------
 */


/* ----------
 * pgstat_setheader() -
 *
 *		Set common header fields in a statistics message
 * ----------
 */
static void
pgstat_setheader(PgStat_MsgHdr *hdr, int mtype)
{
	hdr->m_type = mtype;
	hdr->m_backendid = MyBackendId;
	hdr->m_procpid = MyProcPid;
	hdr->m_databaseid = MyDatabaseId;
	hdr->m_userid = GetSessionUserId();
}


/* ----------
 * pgstat_send() -
 *
 *		Send out one statistics message to the collector
 * ----------
 */
static void
pgstat_send(void *msg, int len)
{
	if (pgStatSock < 0)
		return;

	((PgStat_MsgHdr *) msg)->m_size = len;

	send(pgStatSock, msg, len, 0);
	/* We deliberately ignore any error from send() */
}


/* ----------
 * PgstatBufferMain() -
 *
 *	Start up the statistics buffer process.  This is the body of the
 *	postmaster child process.
 *
 *	The argc/argv parameters are valid only in EXEC_BACKEND case.
 * ----------
 */
NON_EXEC_STATIC void
PgstatBufferMain(int argc, char *argv[])
{
	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	MyProcPid = getpid();		/* reset MyProcPid */

	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

	/*
	 * Ignore all signals usually bound to some action in the postmaster,
	 * except for SIGCHLD and SIGQUIT --- see pgstat_recvbuffer.
	 */
	pqsignal(SIGHUP, SIG_IGN);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, pgstat_exit);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, pgstat_die);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);
	/* unblock will happen in pgstat_recvbuffer */

#ifdef EXEC_BACKEND
	pgstat_parseArgs(argc,argv);
#endif

	/*
	 * Start a buffering process to read from the socket, so we have a
	 * little more time to process incoming messages.
	 *
	 * NOTE: the process structure is: postmaster is parent of buffer process
	 * is parent of collector process.	This way, the buffer can detect
	 * collector failure via SIGCHLD, whereas otherwise it wouldn't notice
	 * collector failure until it tried to write on the pipe.  That would
	 * mean that after the postmaster started a new collector, we'd have
	 * two buffer processes competing to read from the UDP socket --- not
	 * good.
	 */
	if (pgpipe(pgStatPipe) < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
			 errmsg("could not create pipe for statistics buffer: %m")));
		exit(1);
	}

#ifdef EXEC_BACKEND
	/* child becomes collector process */
	switch (pgstat_forkexec(STAT_PROC_COLLECTOR))
#else
	switch (fork())
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork statistics collector: %m")));
			exit(1);

#ifndef EXEC_BACKEND
		case 0:
			/* child becomes collector process */
			PgstatCollectorMain(0, NULL);
			break;
#endif

		default:
			/* parent becomes buffer process */
			closesocket(pgStatPipe[0]);
			pgstat_recvbuffer();
	}
	exit(0);
}


/* ----------
 * PgstatCollectorMain() -
 *
 *	Start up the statistics collector itself.  This is the body of the
 *	postmaster grandchild process.
 *
 *	The argc/argv parameters are valid only in EXEC_BACKEND case.
 * ----------
 */
NON_EXEC_STATIC void
PgstatCollectorMain(int argc, char *argv[])
{
	PgStat_Msg	msg;
	fd_set		rfds;
	int			readPipe;
	int			nready;
	int			len = 0;
	struct timeval timeout;
	struct timeval next_statwrite;
	bool		need_statwrite;
	HASHCTL		hash_ctl;

	MyProcPid = getpid();		/* reset MyProcPid */

	/*
	 * Reset signal handling.  With the exception of restoring default
	 * SIGCHLD and SIGQUIT handling, this is a no-op in the non-EXEC_BACKEND
	 * case because we'll have inherited these settings from the buffer
	 * process; but it's not a no-op for EXEC_BACKEND.
	 */
	pqsignal(SIGHUP, SIG_IGN);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);
	PG_SETMASK(&UnBlockSig);

#ifdef EXEC_BACKEND
	pgstat_parseArgs(argc,argv);
#endif

	/* Close unwanted files */
	closesocket(pgStatPipe[1]);
	closesocket(pgStatSock);

	/*
	 * Identify myself via ps
	 */
	init_ps_display("stats collector process", "", "");
	set_ps_display("");

	/*
	 * Initialize filenames needed for status reports.
	 */
	snprintf(pgStat_fname, MAXPGPATH, PGSTAT_STAT_FILENAME, DataDir);
	/* tmpfname need only be set correctly in this process */
	snprintf(pgStat_tmpfname, MAXPGPATH, PGSTAT_STAT_TMPFILE,
			 DataDir, getpid());

	/*
	 * Arrange to write the initial status file right away
	 */
	gettimeofday(&next_statwrite, NULL);
	need_statwrite = TRUE;

	/*
	 * Read in an existing statistics stats file or initialize the stats
	 * to zero.
	 */
	pgStatRunningInCollector = TRUE;
	pgstat_read_statsfile(&pgStatDBHash, InvalidOid, NULL, NULL);

	/*
	 * Create the dead backend hashtable
	 */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(int);
	hash_ctl.entrysize = sizeof(PgStat_StatBeDead);
	hash_ctl.hash = tag_hash;
	pgStatBeDead = hash_create("Dead Backends", PGSTAT_BE_HASH_SIZE,
							   &hash_ctl, HASH_ELEM | HASH_FUNCTION);
	if (pgStatBeDead == NULL)
	{
		/* assume the problem is out-of-memory */
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory in statistics collector --- abort")));
		exit(1);
	}

	/*
	 * Create the known backends table
	 */
	pgStatBeTable = (PgStat_StatBeEntry *) malloc(
							   sizeof(PgStat_StatBeEntry) * MaxBackends);
	if (pgStatBeTable == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory in statistics collector --- abort")));
		exit(1);
	}
	memset(pgStatBeTable, 0, sizeof(PgStat_StatBeEntry) * MaxBackends);

	readPipe = pgStatPipe[0];

	/*
	 * Process incoming messages and handle all the reporting stuff until
	 * there are no more messages.
	 */
	for (;;)
	{
		/*
		 * If we need to write the status file again (there have been
		 * changes in the statistics since we wrote it last) calculate the
		 * timeout until we have to do so.
		 */
		if (need_statwrite)
		{
			struct timeval now;

			gettimeofday(&now, NULL);
			/* avoid assuming that tv_sec is signed */
			if (now.tv_sec > next_statwrite.tv_sec ||
				(now.tv_sec == next_statwrite.tv_sec &&
				 now.tv_usec >= next_statwrite.tv_usec))
			{
				timeout.tv_sec = 0;
				timeout.tv_usec = 0;
			}
			else
			{
				timeout.tv_sec = next_statwrite.tv_sec - now.tv_sec;
				timeout.tv_usec = next_statwrite.tv_usec - now.tv_usec;
				if (timeout.tv_usec < 0)
				{
					timeout.tv_sec--;
					timeout.tv_usec += 1000000;
				}
			}
		}

		/*
		 * Setup the descriptor set for select(2)
		 */
		FD_ZERO(&rfds);
		FD_SET(readPipe, &rfds);

		/*
		 * Now wait for something to do.
		 */
		nready = select(readPipe+1, &rfds, NULL, NULL,
						(need_statwrite) ? &timeout : NULL);
		if (nready < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(LOG,
					(errcode_for_socket_access(),
				   errmsg("select() failed in statistics collector: %m")));
			exit(1);
		}

		/*
		 * If there are no descriptors ready, our timeout for writing the
		 * stats file happened.
		 */
		if (nready == 0)
		{
			pgstat_write_statsfile();
			need_statwrite = FALSE;

			continue;
		}

		/*
		 * Check if there is a new statistics message to collect.
		 */
		if (FD_ISSET(readPipe, &rfds))
		{
			/*
			 * We may need to issue multiple read calls in case the buffer
			 * process didn't write the message in a single write, which
			 * is possible since it dumps its buffer bytewise. In any
			 * case, we'd need two reads since we don't know the message
			 * length initially.
			 */
			int			nread = 0;
			int			targetlen = sizeof(PgStat_MsgHdr);		/* initial */
			bool		pipeEOF = false;

			while (nread < targetlen)
			{
				len = piperead(readPipe, ((char *) &msg) + nread,
								targetlen - nread);
				if (len < 0)
				{
					if (errno == EINTR)
						continue;
					ereport(LOG,
							(errcode_for_socket_access(),
							 errmsg("could not read from statistics collector pipe: %m")));
					exit(1);
				}
				if (len == 0)	/* EOF on the pipe! */
				{
					pipeEOF = true;
					break;
				}
				nread += len;
				if (nread == sizeof(PgStat_MsgHdr))
				{
					/* we have the header, compute actual msg length */
					targetlen = msg.msg_hdr.m_size;
					if (targetlen < (int) sizeof(PgStat_MsgHdr) ||
						targetlen > (int) sizeof(msg))
					{
						/*
						 * Bogus message length implies that we got out of
						 * sync with the buffer process somehow. Abort so
						 * that we can restart both processes.
						 */
						ereport(LOG,
						  (errmsg("invalid statistics message length")));
						exit(1);
					}
				}
			}

			/*
			 * EOF on the pipe implies that the buffer process exited.
			 * Fall out of outer loop.
			 */
			if (pipeEOF)
				break;

			/*
			 * Distribute the message to the specific function handling
			 * it.
			 */
			switch (msg.msg_hdr.m_type)
			{
				case PGSTAT_MTYPE_DUMMY:
					break;

				case PGSTAT_MTYPE_BESTART:
					pgstat_recv_bestart((PgStat_MsgBestart *) &msg, nread);
					break;

				case PGSTAT_MTYPE_BETERM:
					pgstat_recv_beterm((PgStat_MsgBeterm *) &msg, nread);
					break;

				case PGSTAT_MTYPE_TABSTAT:
					pgstat_recv_tabstat((PgStat_MsgTabstat *) &msg, nread);
					break;

				case PGSTAT_MTYPE_TABPURGE:
					pgstat_recv_tabpurge((PgStat_MsgTabpurge *) &msg, nread);
					break;

				case PGSTAT_MTYPE_ACTIVITY:
					pgstat_recv_activity((PgStat_MsgActivity *) &msg, nread);
					break;

				case PGSTAT_MTYPE_DROPDB:
					pgstat_recv_dropdb((PgStat_MsgDropdb *) &msg, nread);
					break;

				case PGSTAT_MTYPE_RESETCOUNTER:
					pgstat_recv_resetcounter((PgStat_MsgResetcounter *) &msg,
											 nread);
					break;

				default:
					break;
			}

			/*
			 * Globally count messages.
			 */
			pgStatNumMessages++;

			/*
			 * If this is the first message after we wrote the stats file
			 * the last time, setup the timeout that it'd be written.
			 */
			if (!need_statwrite)
			{
				gettimeofday(&next_statwrite, NULL);
				next_statwrite.tv_usec += ((PGSTAT_STAT_INTERVAL) * 1000);
				next_statwrite.tv_sec += (next_statwrite.tv_usec / 1000000);
				next_statwrite.tv_usec %= 1000000;
				need_statwrite = TRUE;
			}
		}

		/*
		 * Note that we do NOT check for postmaster exit inside the loop;
		 * only EOF on the buffer pipe causes us to fall out.  This
		 * ensures we don't exit prematurely if there are still a few
		 * messages in the buffer or pipe at postmaster shutdown.
		 */
	}

	/*
	 * Okay, we saw EOF on the buffer pipe, so there are no more messages
	 * to process.	If the buffer process quit because of postmaster
	 * shutdown, we want to save the final stats to reuse at next startup.
	 * But if the buffer process failed, it seems best not to (there may
	 * even now be a new collector firing up, and we don't want it to read
	 * a partially-rewritten stats file).
	 */
	if (!PostmasterIsAlive(false))
		pgstat_write_statsfile();
}


/* ----------
 * pgstat_recvbuffer() -
 *
 *	This is the body of the separate buffering process. Its only
 *	purpose is to receive messages from the UDP socket as fast as
 *	possible and forward them over a pipe into the collector itself.
 *	If the collector is slow to absorb messages, they are buffered here.
 * ----------
 */
static void
pgstat_recvbuffer(void)
{
	fd_set		rfds;
	fd_set		wfds;
	struct timeval timeout;
	int			writePipe = pgStatPipe[1];
	int			maxfd;
	int			nready;
	int			len;
	int			xfr;
	int			frm;
	PgStat_Msg	input_buffer;
	char	   *msgbuffer;
	int			msg_send = 0;	/* next send index in buffer */
	int			msg_recv = 0;	/* next receive index */
	int			msg_have = 0;	/* number of bytes stored */
	bool		overflow = false;

	/*
	 * Identify myself via ps
	 */
	init_ps_display("stats buffer process", "", "");
	set_ps_display("");

	/*
	 * We want to die if our child collector process does.	There are two
	 * ways we might notice that it has died: receive SIGCHLD, or get a
	 * write failure on the pipe leading to the child.	We can set SIGPIPE
	 * to kill us here.  Our SIGCHLD handler was already set up before we
	 * forked (must do it that way, else it's a race condition).
	 */
	pqsignal(SIGPIPE, SIG_DFL);
	PG_SETMASK(&UnBlockSig);

	/*
	 * Set the write pipe to nonblock mode, so that we cannot block when
	 * the collector falls behind.
	 */
	if (!set_noblock(writePipe))
	{
		ereport(LOG,
				(errcode_for_socket_access(),
		  errmsg("could not set statistics collector pipe to nonblocking mode: %m")));
		exit(1);
	}

	/*
	 * Allocate the message buffer
	 */
	msgbuffer = (char *) malloc(PGSTAT_RECVBUFFERSZ);
	if (msgbuffer == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory in statistics collector --- abort")));
		exit(1);
	}

	/*
	 * Loop forever
	 */
	for (;;)
	{
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		maxfd = -1;

		/*
		 * As long as we have buffer space we add the socket to the read
		 * descriptor set.
		 */
		if (msg_have <= (int) (PGSTAT_RECVBUFFERSZ - sizeof(PgStat_Msg)))
		{
			FD_SET(pgStatSock, &rfds);
			maxfd = pgStatSock;
			overflow = false;
		}
		else
		{
			if (!overflow)
			{
				ereport(LOG,
						(errmsg("statistics buffer is full")));
				overflow = true;
			}
		}

		/*
		 * If we have messages to write out, we add the pipe to the write
		 * descriptor set.
		 */
		if (msg_have > 0)
		{
			FD_SET(writePipe, &wfds);
			if (writePipe > maxfd)
				maxfd = writePipe;
		}

		/*
		 * Wait for some work to do; but not for more than 10 seconds.
		 * (This determines how quickly we will shut down after an
		 * ungraceful postmaster termination; so it needn't be very fast.)
		 */
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		nready = select(maxfd + 1, &rfds, &wfds, NULL, &timeout);
		if (nready < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("select() failed in statistics buffer: %m")));
			exit(1);
		}

		/*
		 * If there is a message on the socket, read it and check for
		 * validity.
		 */
		if (FD_ISSET(pgStatSock, &rfds))
		{
			len = recv(pgStatSock, (char *) &input_buffer,
					   sizeof(PgStat_Msg), 0);
			if (len < 0)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
					   errmsg("could not read statistics message: %m")));
				exit(1);
			}

			/*
			 * We ignore messages that are smaller than our common header
			 */
			if (len < sizeof(PgStat_MsgHdr))
				continue;

			/*
			 * The received length must match the length in the header
			 */
			if (input_buffer.msg_hdr.m_size != len)
				continue;

			/*
			 * O.K. - we accept this message.  Copy it to the circular
			 * msgbuffer.
			 */
			frm = 0;
			while (len > 0)
			{
				xfr = PGSTAT_RECVBUFFERSZ - msg_recv;
				if (xfr > len)
					xfr = len;
				Assert(xfr > 0);
				memcpy(msgbuffer + msg_recv,
					   ((char *) &input_buffer) + frm,
					   xfr);
				msg_recv += xfr;
				if (msg_recv == PGSTAT_RECVBUFFERSZ)
					msg_recv = 0;
				msg_have += xfr;
				frm += xfr;
				len -= xfr;
			}
		}

		/*
		 * If the collector is ready to receive, write some data into his
		 * pipe.  We may or may not be able to write all that we have.
		 *
		 * NOTE: if what we have is less than PIPE_BUF bytes but more than
		 * the space available in the pipe buffer, most kernels will
		 * refuse to write any of it, and will return EAGAIN.  This means
		 * we will busy-loop until the situation changes (either because
		 * the collector caught up, or because more data arrives so that
		 * we have more than PIPE_BUF bytes buffered).	This is not good,
		 * but is there any way around it?	We have no way to tell when
		 * the collector has caught up...
		 */
		if (FD_ISSET(writePipe, &wfds))
		{
			xfr = PGSTAT_RECVBUFFERSZ - msg_send;
			if (xfr > msg_have)
				xfr = msg_have;
			Assert(xfr > 0);
			len = pipewrite(writePipe, msgbuffer + msg_send, xfr);
			if (len < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
					continue;	/* not enough space in pipe */
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("could not write to statistics collector pipe: %m")));
				exit(1);
			}
			/* NB: len < xfr is okay */
			msg_send += len;
			if (msg_send == PGSTAT_RECVBUFFERSZ)
				msg_send = 0;
			msg_have -= len;
		}

		/*
		 * Make sure we forwarded all messages before we check for
		 * postmaster termination.
		 */
		if (msg_have != 0 || FD_ISSET(pgStatSock, &rfds))
			continue;

		/*
		 * If the postmaster has terminated, we die too.  (This is no longer
		 * the normal exit path, however.)
		 */
		if (!PostmasterIsAlive(true))
			exit(0);
	}
}

/* SIGQUIT signal handler for buffer process */
static void
pgstat_exit(SIGNAL_ARGS)
{
	/*
	 * For now, we just nail the doors shut and get out of town.  It might
	 * be cleaner to allow any pending messages to be sent, but that creates
	 * a tradeoff against speed of exit.
	 */
	exit(0);
}

/* SIGCHLD signal handler for buffer process */
static void
pgstat_die(SIGNAL_ARGS)
{
	exit(1);
}


/* ----------
 * pgstat_add_backend() -
 *
 *	Support function to keep our backend list up to date.
 * ----------
 */
static int
pgstat_add_backend(PgStat_MsgHdr *msg)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatBeEntry *beentry;
	PgStat_StatBeDead *deadbe;
	bool		found;

	/*
	 * Check that the backend ID is valid
	 */
	if (msg->m_backendid < 1 || msg->m_backendid > MaxBackends)
	{
		ereport(LOG,
				(errmsg("invalid server process ID %d", msg->m_backendid)));
		return -1;
	}

	/*
	 * Get the slot for this backendid.
	 */
	beentry = &pgStatBeTable[msg->m_backendid - 1];
	if (beentry->databaseid != InvalidOid)
	{
		/*
		 * If the slot contains the PID of this backend, everything is
		 * fine and we got nothing to do.
		 */
		if (beentry->procpid == msg->m_procpid)
			return 0;
	}

	/*
	 * Lookup if this backend is known to be dead. This can be caused due
	 * to messages arriving in the wrong order - i.e. Postmaster's BETERM
	 * message might have arrived before we received all the backends
	 * stats messages, or even a new backend with the same backendid was
	 * faster in sending his BESTART.
	 *
	 * If the backend is known to be dead, we ignore this add.
	 */
	deadbe = (PgStat_StatBeDead *) hash_search(pgStatBeDead,
											   (void *) &(msg->m_procpid),
											   HASH_FIND, NULL);
	if (deadbe)
		return 1;

	/*
	 * Backend isn't known to be dead. If it's slot is currently used, we
	 * have to kick out the old backend.
	 */
	if (beentry->databaseid != InvalidOid)
		pgstat_sub_backend(beentry->procpid);

	/*
	 * Put this new backend into the slot.
	 */
	beentry->databaseid = msg->m_databaseid;
	beentry->procpid = msg->m_procpid;
	beentry->userid = msg->m_userid;
	beentry->activity_start_sec = 0;
	beentry->activity_start_usec = 0;
	MemSet(beentry->activity, 0, PGSTAT_ACTIVITY_SIZE);

	/*
	 * Lookup or create the database entry for this backends DB.
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
										   (void *) &(msg->m_databaseid),
												 HASH_ENTER, &found);
	if (dbentry == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory in statistics collector --- abort")));
		exit(1);
	}

	/*
	 * If not found, initialize the new one.
	 */
	if (!found)
	{
		HASHCTL		hash_ctl;

		dbentry->tables = NULL;
		dbentry->n_xact_commit = 0;
		dbentry->n_xact_rollback = 0;
		dbentry->n_blocks_fetched = 0;
		dbentry->n_blocks_hit = 0;
		dbentry->n_connects = 0;
		dbentry->destroy = 0;

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(PgStat_StatTabEntry);
		hash_ctl.hash = tag_hash;
		dbentry->tables = hash_create("Per-database table",
									  PGSTAT_TAB_HASH_SIZE,
									  &hash_ctl,
									  HASH_ELEM | HASH_FUNCTION);
		if (dbentry->tables == NULL)
		{
			/* assume the problem is out-of-memory */
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory in statistics collector --- abort")));
			exit(1);
		}
	}

	/*
	 * Count number of connects to the database
	 */
	dbentry->n_connects++;

	return 0;
}


/* ----------
 * pgstat_sub_backend() -
 *
 *	Remove a backend from the actual backends list.
 * ----------
 */
static void
pgstat_sub_backend(int procpid)
{
	int			i;
	PgStat_StatBeDead *deadbe;
	bool		found;

	/*
	 * Search in the known-backends table for the slot containing this
	 * PID.
	 */
	for (i = 0; i < MaxBackends; i++)
	{
		if (pgStatBeTable[i].databaseid != InvalidOid &&
			pgStatBeTable[i].procpid == procpid)
		{
			/*
			 * That's him. Add an entry to the known to be dead backends.
			 * Due to possible misorder in the arrival of UDP packets it's
			 * possible that even if we know the backend is dead, there
			 * could still be messages queued that arrive later. Those
			 * messages must not cause our number of backends statistics
			 * to get screwed up, so we remember for a couple of seconds
			 * that this PID is dead and ignore them (only the counting of
			 * backends, not the table access stats they sent).
			 */
			deadbe = (PgStat_StatBeDead *) hash_search(pgStatBeDead,
													   (void *) &procpid,
													   HASH_ENTER,
													   &found);
			if (deadbe == NULL)
			{
				ereport(LOG,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory in statistics collector --- abort")));
				exit(1);
			}
			if (!found)
			{
				deadbe->backendid = i + 1;
				deadbe->destroy = PGSTAT_DESTROY_COUNT;
			}

			/*
			 * Declare the backend slot empty.
			 */
			pgStatBeTable[i].databaseid = InvalidOid;
			return;
		}
	}

	/*
	 * No big problem if not found. This can happen if UDP messages arrive
	 * out of order here.
	 */
}


/* ----------
 * pgstat_write_statsfile() -
 *
 *	Tell the news.
 * ----------
 */
static void
pgstat_write_statsfile(void)
{
	HASH_SEQ_STATUS hstat;
	HASH_SEQ_STATUS tstat;
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	PgStat_StatBeDead *deadbe;
	FILE	   *fpout;
	int			i;

	/*
	 * Open the statistics temp file to write out the current values.
	 */
	fpout = fopen(pgStat_tmpfname, PG_BINARY_W);
	if (fpout == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open temporary statistics file \"%s\": %m",
						pgStat_tmpfname)));
		return;
	}

	/*
	 * Walk through the database table.
	 */
	hash_seq_init(&hstat, pgStatDBHash);
	while ((dbentry = (PgStat_StatDBEntry *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * If this database is marked destroyed, count down and do so if
		 * it reaches 0.
		 */
		if (dbentry->destroy > 0)
		{
			if (--(dbentry->destroy) == 0)
			{
				if (dbentry->tables != NULL)
					hash_destroy(dbentry->tables);

				if (hash_search(pgStatDBHash,
								(void *) &(dbentry->databaseid),
								HASH_REMOVE, NULL) == NULL)
				{
					ereport(LOG,
							(errmsg("database hash table corrupted "
									"during cleanup --- abort")));
					exit(1);
				}
			}

			/*
			 * Don't include statistics for it.
			 */
			continue;
		}

		/*
		 * Write out the DB line including the number of life backends.
		 */
		fputc('D', fpout);
		fwrite(dbentry, sizeof(PgStat_StatDBEntry), 1, fpout);

		/*
		 * Walk through the databases access stats per table.
		 */
		hash_seq_init(&tstat, dbentry->tables);
		while ((tabentry = (PgStat_StatTabEntry *) hash_seq_search(&tstat)) != NULL)
		{
			/*
			 * If table entry marked for destruction, same as above for
			 * the database entry.
			 */
			if (tabentry->destroy > 0)
			{
				if (--(tabentry->destroy) == 0)
				{
					if (hash_search(dbentry->tables,
									(void *) &(tabentry->tableid),
									HASH_REMOVE, NULL) == NULL)
					{
						ereport(LOG,
								(errmsg("tables hash table for "
										"database %u corrupted during "
										"cleanup --- abort",
										dbentry->databaseid)));
						exit(1);
					}
				}
				continue;
			}

			/*
			 * At least we think this is still a life table. Print it's
			 * access stats.
			 */
			fputc('T', fpout);
			fwrite(tabentry, sizeof(PgStat_StatTabEntry), 1, fpout);
		}

		/*
		 * Mark the end of this DB
		 */
		fputc('d', fpout);
	}

	/*
	 * Write out the known running backends to the stats file.
	 */
	i = MaxBackends;
	fputc('M', fpout);
	fwrite(&i, sizeof(i), 1, fpout);

	for (i = 0; i < MaxBackends; i++)
	{
		if (pgStatBeTable[i].databaseid != InvalidOid)
		{
			fputc('B', fpout);
			fwrite(&pgStatBeTable[i], sizeof(PgStat_StatBeEntry), 1, fpout);
		}
	}

	/*
	 * No more output to be done. Close the temp file and replace the old
	 * pgstat.stat with it.
	 */
	fputc('E', fpout);
	if (fclose(fpout) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close temporary statistics file \"%s\": %m",
						pgStat_tmpfname)));
	}
	else
	{
		if (rename(pgStat_tmpfname, pgStat_fname) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not rename temporary statistics file \"%s\" to \"%s\": %m",
							pgStat_tmpfname, pgStat_fname)));
		}
	}

	/*
	 * Clear out the dead backends table
	 */
	hash_seq_init(&hstat, pgStatBeDead);
	while ((deadbe = (PgStat_StatBeDead *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * Count down the destroy delay and remove entries where it
		 * reaches 0.
		 */
		if (--(deadbe->destroy) <= 0)
		{
			if (hash_search(pgStatBeDead,
							(void *) &(deadbe->procpid),
							HASH_REMOVE, NULL) == NULL)
			{
				ereport(LOG,
						(errmsg("dead-server-process hash table corrupted "
								"during cleanup --- abort")));
				exit(1);
			}
		}
	}
}


/* ----------
 * pgstat_read_statsfile() -
 *
 *	Reads in an existing statistics collector and initializes the
 *	databases hash table (who's entries point to the tables hash tables)
 *	and the current backend table.
 * ----------
 */
static void
pgstat_read_statsfile(HTAB **dbhash, Oid onlydb,
					  PgStat_StatBeEntry **betab, int *numbackends)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatDBEntry dbbuf;
	PgStat_StatTabEntry *tabentry;
	PgStat_StatTabEntry tabbuf;
	HASHCTL		hash_ctl;
	HTAB	   *tabhash = NULL;
	FILE	   *fpin;
	int			maxbackends = 0;
	int			havebackends = 0;
	bool		found;
	MemoryContext use_mcxt;
	int			mcxt_flags;

	/*
	 * If running in the collector we use the DynaHashCxt memory context.
	 * If running in a backend, we use the TopTransactionContext instead,
	 * so the caller must only know the last XactId when this call
	 * happened to know if his tables are still valid or already gone!
	 */
	if (pgStatRunningInCollector)
	{
		use_mcxt = NULL;
		mcxt_flags = 0;
	}
	else
	{
		use_mcxt = TopTransactionContext;
		mcxt_flags = HASH_CONTEXT;
	}

	/*
	 * Create the DB hashtable
	 */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(PgStat_StatDBEntry);
	hash_ctl.hash = tag_hash;
	hash_ctl.hcxt = use_mcxt;
	*dbhash = hash_create("Databases hash", PGSTAT_DB_HASH_SIZE, &hash_ctl,
						  HASH_ELEM | HASH_FUNCTION | mcxt_flags);
	if (*dbhash == NULL)
	{
		/* assume the problem is out-of-memory */
		if (pgStatRunningInCollector)
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory in statistics collector --- abort")));
			exit(1);
		}
		/* in backend, can do normal error */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/*
	 * Initialize the number of known backends to zero, just in case we do
	 * a silent error return below.
	 */
	if (numbackends != NULL)
		*numbackends = 0;
	if (betab != NULL)
		*betab = NULL;

	/*
	 * In EXEC_BACKEND case, we won't have inherited pgStat_fname from
	 * postmaster, so compute it first time through.
	 */
#ifdef EXEC_BACKEND
	if (pgStat_fname[0] == '\0')
	{
		Assert(DataDir != NULL);
		snprintf(pgStat_fname, MAXPGPATH, PGSTAT_STAT_FILENAME, DataDir);
	}
#endif

	/*
	 * Try to open the status file. If it doesn't exist, the backends
	 * simply return zero for anything and the collector simply starts
	 * from scratch with empty counters.
	 */
	if ((fpin = fopen(pgStat_fname, PG_BINARY_R)) == NULL)
		return;

	/*
	 * We found an existing collector stats file. Read it and put all the
	 * hashtable entries into place.
	 */
	for (;;)
	{
		switch (fgetc(fpin))
		{
				/*
				 * 'D'	A PgStat_StatDBEntry struct describing a database
				 * follows. Subsequently, zero to many 'T' entries will
				 * follow until a 'd' is encountered.
				 */
			case 'D':
				if (fread(&dbbuf, 1, sizeof(dbbuf), fpin) != sizeof(dbbuf))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					fclose(fpin);
					return;
				}

				/*
				 * Add to the DB hash
				 */
				dbentry = (PgStat_StatDBEntry *) hash_search(*dbhash,
											  (void *) &dbbuf.databaseid,
															 HASH_ENTER,
															 &found);
				if (dbentry == NULL)
				{
					if (pgStatRunningInCollector)
					{
						ereport(LOG,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory in statistics collector --- abort")));
						exit(1);
					}
					else
					{
						fclose(fpin);
						ereport(ERROR,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory")));
					}
				}
				if (found)
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					fclose(fpin);
					return;
				}

				memcpy(dbentry, &dbbuf, sizeof(PgStat_StatDBEntry));
				dbentry->tables = NULL;
				dbentry->destroy = 0;
				dbentry->n_backends = 0;

				/*
				 * Don't collect tables if not the requested DB
				 */
				if (onlydb != InvalidOid && onlydb != dbbuf.databaseid)
					break;

				memset(&hash_ctl, 0, sizeof(hash_ctl));
				hash_ctl.keysize = sizeof(Oid);
				hash_ctl.entrysize = sizeof(PgStat_StatTabEntry);
				hash_ctl.hash = tag_hash;
				hash_ctl.hcxt = use_mcxt;
				dbentry->tables = hash_create("Per-database table",
											  PGSTAT_TAB_HASH_SIZE,
											  &hash_ctl,
								 HASH_ELEM | HASH_FUNCTION | mcxt_flags);
				if (dbentry->tables == NULL)
				{
					/* assume the problem is out-of-memory */
					if (pgStatRunningInCollector)
					{
						ereport(LOG,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory in statistics collector --- abort")));
						exit(1);
					}
					/* in backend, can do normal error */
					fclose(fpin);
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));
				}

				/*
				 * Arrange that following 'T's add entries to this
				 * databases tables hash table.
				 */
				tabhash = dbentry->tables;
				break;

				/*
				 * 'd'	End of this database.
				 */
			case 'd':
				tabhash = NULL;
				break;

				/*
				 * 'T'	A PgStat_StatTabEntry follows.
				 */
			case 'T':
				if (fread(&tabbuf, 1, sizeof(tabbuf), fpin) != sizeof(tabbuf))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					fclose(fpin);
					return;
				}

				/*
				 * Skip if table belongs to a not requested database.
				 */
				if (tabhash == NULL)
					break;

				tabentry = (PgStat_StatTabEntry *) hash_search(tabhash,
												(void *) &tabbuf.tableid,
													 HASH_ENTER, &found);
				if (tabentry == NULL)
				{
					if (pgStatRunningInCollector)
					{
						ereport(LOG,
								(errcode(ERRCODE_OUT_OF_MEMORY),
								 errmsg("out of memory in statistics collector --- abort")));
						exit(1);
					}
					/* in backend, can do normal error */
					fclose(fpin);
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));
				}

				if (found)
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					fclose(fpin);
					return;
				}

				memcpy(tabentry, &tabbuf, sizeof(tabbuf));
				break;

				/*
				 * 'M'	The maximum number of backends to expect follows.
				 */
			case 'M':
				if (betab == NULL || numbackends == NULL)
				{
					fclose(fpin);
					return;
				}
				if (fread(&maxbackends, 1, sizeof(maxbackends), fpin) !=
					sizeof(maxbackends))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					fclose(fpin);
					return;
				}
				if (maxbackends == 0)
				{
					fclose(fpin);
					return;
				}

				/*
				 * Allocate space (in TopTransactionContext too) for the
				 * backend table.
				 */
				if (use_mcxt == NULL)
					*betab = (PgStat_StatBeEntry *) malloc(
							   sizeof(PgStat_StatBeEntry) * maxbackends);
				else
					*betab = (PgStat_StatBeEntry *) MemoryContextAlloc(
																use_mcxt,
							   sizeof(PgStat_StatBeEntry) * maxbackends);
				break;

				/*
				 * 'B'	A PgStat_StatBeEntry follows.
				 */
			case 'B':
				if (betab == NULL || numbackends == NULL)
				{
					fclose(fpin);
					return;
				}
				if (*betab == NULL)
				{
					fclose(fpin);
					return;
				}

				/*
				 * Read it directly into the table.
				 */
				if (fread(&(*betab)[havebackends], 1,
						  sizeof(PgStat_StatBeEntry), fpin) !=
					sizeof(PgStat_StatBeEntry))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					fclose(fpin);
					return;
				}

				/*
				 * Count backends per database here.
				 */
				dbentry = (PgStat_StatDBEntry *) hash_search(*dbhash,
						   (void *) &((*betab)[havebackends].databaseid),
														HASH_FIND, NULL);
				if (dbentry)
					dbentry->n_backends++;

				havebackends++;
				if (numbackends != 0)
					*numbackends = havebackends;
				if (havebackends >= maxbackends)
				{
					fclose(fpin);
					return;
				}
				break;

				/*
				 * 'E'	The EOF marker of a complete stats file.
				 */
			case 'E':
				fclose(fpin);
				return;

			default:
				ereport(pgStatRunningInCollector ? LOG : WARNING,
						(errmsg("corrupted pgstat.stat file")));
				fclose(fpin);
				return;
		}
	}

	fclose(fpin);
}

/*
 * If not done for this transaction, read the statistics collector
 * stats file into some hash tables.
 *
 * Because we store the hash tables in TopTransactionContext, the result
 * is good for the entire current main transaction.
 */
static void
backend_read_statsfile(void)
{
	TransactionId	topXid = GetTopTransactionId();

	if (!TransactionIdEquals(pgStatDBHashXact, topXid))
	{
		Assert(!pgStatRunningInCollector);
		pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId,
							  &pgStatBeTable, &pgStatNumBackends);
		pgStatDBHashXact = topXid;
	}
}


/* ----------
 * pgstat_recv_bestart() -
 *
 *	Process a backend startup message.
 * ----------
 */
static void
pgstat_recv_bestart(PgStat_MsgBestart *msg, int len)
{
	pgstat_add_backend(&msg->m_hdr);
}


/* ----------
 * pgstat_recv_beterm() -
 *
 *	Process a backend termination message.
 * ----------
 */
static void
pgstat_recv_beterm(PgStat_MsgBeterm *msg, int len)
{
	pgstat_sub_backend(msg->m_hdr.m_procpid);
}


/* ----------
 * pgstat_recv_activity() -
 *
 *	Remember what the backend is doing.
 * ----------
 */
static void
pgstat_recv_activity(PgStat_MsgActivity *msg, int len)
{
	PgStat_StatBeEntry *entry;

	/*
	 * Here we check explicitly for 0 return, since we don't want to
	 * mangle the activity of an active backend by a delayed packet from a
	 * dead one.
	 */
	if (pgstat_add_backend(&msg->m_hdr) != 0)
		return;

	entry = &(pgStatBeTable[msg->m_hdr.m_backendid - 1]);

	StrNCpy(entry->activity, msg->m_what, PGSTAT_ACTIVITY_SIZE);

	entry->activity_start_sec =
		GetCurrentAbsoluteTimeUsec(&entry->activity_start_usec);
}


/* ----------
 * pgstat_recv_tabstat() -
 *
 *	Count what the backend has done.
 * ----------
 */
static void
pgstat_recv_tabstat(PgStat_MsgTabstat *msg, int len)
{
	PgStat_TableEntry *tabmsg = &(msg->m_entry[0]);
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	int			i;
	bool		found;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
									 (void *) &(msg->m_hdr.m_databaseid),
												 HASH_FIND, NULL);
	if (!dbentry)
		return;

	/*
	 * If the database is marked for destroy, this is a delayed UDP packet
	 * and not worth being counted.
	 */
	if (dbentry->destroy > 0)
		return;

	dbentry->n_xact_commit += (PgStat_Counter) (msg->m_xact_commit);
	dbentry->n_xact_rollback += (PgStat_Counter) (msg->m_xact_rollback);

	/*
	 * Process all table entries in the message.
	 */
	for (i = 0; i < msg->m_nentries; i++)
	{
		tabentry = (PgStat_StatTabEntry *) hash_search(dbentry->tables,
											  (void *) &(tabmsg[i].t_id),
													 HASH_ENTER, &found);
		if (tabentry == NULL)
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory in statistics collector --- abort")));
			exit(1);
		}

		if (!found)
		{
			/*
			 * If it's a new table entry, initialize counters to the
			 * values we just got.
			 */
			tabentry->numscans = tabmsg[i].t_numscans;
			tabentry->tuples_returned = tabmsg[i].t_tuples_returned;
			tabentry->tuples_fetched = tabmsg[i].t_tuples_fetched;
			tabentry->tuples_inserted = tabmsg[i].t_tuples_inserted;
			tabentry->tuples_updated = tabmsg[i].t_tuples_updated;
			tabentry->tuples_deleted = tabmsg[i].t_tuples_deleted;
			tabentry->blocks_fetched = tabmsg[i].t_blocks_fetched;
			tabentry->blocks_hit = tabmsg[i].t_blocks_hit;

			tabentry->destroy = 0;
		}
		else
		{
			/*
			 * Otherwise add the values to the existing entry.
			 */
			tabentry->numscans += tabmsg[i].t_numscans;
			tabentry->tuples_returned += tabmsg[i].t_tuples_returned;
			tabentry->tuples_fetched += tabmsg[i].t_tuples_fetched;
			tabentry->tuples_inserted += tabmsg[i].t_tuples_inserted;
			tabentry->tuples_updated += tabmsg[i].t_tuples_updated;
			tabentry->tuples_deleted += tabmsg[i].t_tuples_deleted;
			tabentry->blocks_fetched += tabmsg[i].t_blocks_fetched;
			tabentry->blocks_hit += tabmsg[i].t_blocks_hit;
		}

		/*
		 * And add the block IO to the database entry.
		 */
		dbentry->n_blocks_fetched += tabmsg[i].t_blocks_fetched;
		dbentry->n_blocks_hit += tabmsg[i].t_blocks_hit;
	}
}


/* ----------
 * pgstat_recv_tabpurge() -
 *
 *	Arrange for dead table removal.
 * ----------
 */
static void
pgstat_recv_tabpurge(PgStat_MsgTabpurge *msg, int len)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	int			i;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
									 (void *) &(msg->m_hdr.m_databaseid),
												 HASH_FIND, NULL);
	if (!dbentry)
		return;

	/*
	 * If the database is marked for destroy, this is a delayed UDP packet
	 * and the tables will go away at DB destruction.
	 */
	if (dbentry->destroy > 0)
		return;

	/*
	 * Process all table entries in the message.
	 */
	for (i = 0; i < msg->m_nentries; i++)
	{
		tabentry = (PgStat_StatTabEntry *) hash_search(dbentry->tables,
										   (void *) &(msg->m_tableid[i]),
													   HASH_FIND, NULL);
		if (tabentry)
			tabentry->destroy = PGSTAT_DESTROY_COUNT;
	}
}


/* ----------
 * pgstat_recv_dropdb() -
 *
 *	Arrange for dead database removal
 * ----------
 */
static void
pgstat_recv_dropdb(PgStat_MsgDropdb *msg, int len)
{
	PgStat_StatDBEntry *dbentry;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
										   (void *) &(msg->m_databaseid),
												 HASH_FIND, NULL);
	if (!dbentry)
		return;

	/*
	 * Mark the database for destruction.
	 */
	dbentry->destroy = PGSTAT_DESTROY_COUNT;
}


/* ----------
 * pgstat_recv_dropdb() -
 *
 *	Arrange for dead database removal
 * ----------
 */
static void
pgstat_recv_resetcounter(PgStat_MsgResetcounter *msg, int len)
{
	HASHCTL		hash_ctl;
	PgStat_StatDBEntry *dbentry;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
									 (void *) &(msg->m_hdr.m_databaseid),
												 HASH_FIND, NULL);
	if (!dbentry)
		return;

	/*
	 * We simply throw away all the databases table entries by recreating
	 * a new hash table for them.
	 */
	if (dbentry->tables != NULL)
		hash_destroy(dbentry->tables);

	dbentry->tables = NULL;
	dbentry->n_xact_commit = 0;
	dbentry->n_xact_rollback = 0;
	dbentry->n_blocks_fetched = 0;
	dbentry->n_blocks_hit = 0;
	dbentry->n_connects = 0;
	dbentry->destroy = 0;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(PgStat_StatTabEntry);
	hash_ctl.hash = tag_hash;
	dbentry->tables = hash_create("Per-database table",
								  PGSTAT_TAB_HASH_SIZE,
								  &hash_ctl,
								  HASH_ELEM | HASH_FUNCTION);
	if (dbentry->tables == NULL)
	{
		/* assume the problem is out-of-memory */
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory in statistics collector --- abort")));
		exit(1);
	}
}
