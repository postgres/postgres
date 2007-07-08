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
 *	Copyright (c) 2001-2006, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/backend/postmaster/pgstat.c,v 1.140.2.5 2007/07/08 22:23:25 tgl Exp $
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
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif

#include "pgstat.h"

#include "access/heapam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "libpq/ip.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "storage/backendid.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


/* ----------
 * Paths for the statistics files (relative to installation's $PGDATA).
 * ----------
 */
#define PGSTAT_STAT_FILENAME	"global/pgstat.stat"
#define PGSTAT_STAT_TMPFILE		"global/pgstat.tmp"

/* ----------
 * Timer definitions.
 * ----------
 */
#define PGSTAT_STAT_INTERVAL	500		/* How often to write the status file;
										 * in milliseconds. */

#define PGSTAT_RESTART_INTERVAL 60		/* How often to attempt to restart a
										 * failed statistics collector; in
										 * seconds. */

#define PGSTAT_SELECT_TIMEOUT	2		/* How often to check for postmaster
										 * death; in seconds. */


/* ----------
 * The initial size hints for the hash tables used in the collector.
 * ----------
 */
#define PGSTAT_DB_HASH_SIZE		16
#define PGSTAT_TAB_HASH_SIZE	512


/* ----------
 * GUC parameters
 * ----------
 */
bool		pgstat_collect_startcollector = true;
bool		pgstat_collect_resetonpmstart = false;
bool		pgstat_collect_tuplelevel = false;
bool		pgstat_collect_blocklevel = false;
bool		pgstat_collect_querystring = false;

/* ----------
 * Local data
 * ----------
 */
NON_EXEC_STATIC int pgStatSock = -1;

static struct sockaddr_storage pgStatAddr;

static time_t last_pgstat_start_time;

static bool pgStatRunningInCollector = false;

/*
 * Place where backends store per-table info to be sent to the collector.
 * We store shared relations separately from non-shared ones, to be able to
 * send them in separate messages.
 */
typedef struct TabStatArray
{
	int			tsa_alloc;		/* num allocated */
	int			tsa_used;		/* num actually used */
	PgStat_MsgTabstat **tsa_messages;	/* the array itself */
} TabStatArray;

#define TABSTAT_QUANTUM		4	/* we alloc this many at a time */

static TabStatArray RegularTabStat = {0, 0, NULL};
static TabStatArray SharedTabStat = {0, 0, NULL};

static int	pgStatXactCommit = 0;
static int	pgStatXactRollback = 0;

static TransactionId pgStatDBHashXact = InvalidTransactionId;
static HTAB *pgStatDBHash = NULL;
static TransactionId pgStatLocalStatusXact = InvalidTransactionId;
static PgBackendStatus *localBackendStatusTable = NULL;
static int	localNumBackends = 0;

static volatile bool need_exit = false;
static volatile bool need_statwrite = false;


/* ----------
 * Local function forward declarations
 * ----------
 */
#ifdef EXEC_BACKEND
static pid_t pgstat_forkexec(void);
#endif

NON_EXEC_STATIC void PgstatCollectorMain(int argc, char *argv[]);
static void pgstat_exit(SIGNAL_ARGS);
static void force_statwrite(SIGNAL_ARGS);
static void pgstat_beshutdown_hook(int code, Datum arg);

static PgStat_StatDBEntry *pgstat_get_db_entry(Oid databaseid, bool create);
static void pgstat_drop_database(Oid databaseid);
static void pgstat_write_statsfile(void);
static void pgstat_read_statsfile(HTAB **dbhash, Oid onlydb);
static void backend_read_statsfile(void);
static void pgstat_read_current_status(void);
static HTAB *pgstat_collect_oids(Oid catalogid);

static void pgstat_setheader(PgStat_MsgHdr *hdr, StatMsgType mtype);
static void pgstat_send(void *msg, int len);

static void pgstat_recv_tabstat(PgStat_MsgTabstat *msg, int len);
static void pgstat_recv_tabpurge(PgStat_MsgTabpurge *msg, int len);
static void pgstat_recv_dropdb(PgStat_MsgDropdb *msg, int len);
static void pgstat_recv_resetcounter(PgStat_MsgResetcounter *msg, int len);
static void pgstat_recv_autovac(PgStat_MsgAutovacStart *msg, int len);
static void pgstat_recv_vacuum(PgStat_MsgVacuum *msg, int len);
static void pgstat_recv_analyze(PgStat_MsgAnalyze *msg, int len);


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
	fd_set		rset;
	struct timeval tv;
	char		test_byte;
	int			sel_res;
	int			tries = 0;

#define TESTBYTEVAL ((char) 199)

	/*
	 * Force start of collector daemon if something to collect.  Note that
	 * pgstat_collect_querystring is now an independent facility that does not
	 * require the collector daemon.
	 */
	if (pgstat_collect_tuplelevel ||
		pgstat_collect_blocklevel)
		pgstat_collect_startcollector = true;

	/*
	 * If we don't have to start a collector or should reset the collected
	 * statistics on postmaster start, simply remove the stats file.
	 */
	if (!pgstat_collect_startcollector || pgstat_collect_resetonpmstart)
		pgstat_reset_all();

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
	ret = pg_getaddrinfo_all("localhost", NULL, &hints, &addrs);
	if (ret || !addrs)
	{
		ereport(LOG,
				(errmsg("could not resolve \"localhost\": %s",
						gai_strerror(ret))));
		goto startup_failed;
	}

	/*
	 * On some platforms, pg_getaddrinfo_all() may return multiple addresses
	 * only one of which will actually work (eg, both IPv6 and IPv4 addresses
	 * when kernel will reject IPv6).  Worse, the failure may occur at the
	 * bind() or perhaps even connect() stage.	So we must loop through the
	 * results till we find a working combination. We will generate LOG
	 * messages, but no error, for bogus combinations.
	 */
	for (addr = addrs; addr; addr = addr->ai_next)
	{
#ifdef HAVE_UNIX_SOCKETS
		/* Ignore AF_UNIX sockets, if any are returned. */
		if (addr->ai_family == AF_UNIX)
			continue;
#endif

		if (++tries > 1)
			ereport(LOG,
			(errmsg("trying another address for the statistics collector")));

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
		if (getsockname(pgStatSock, (struct sockaddr *) & pgStatAddr, &alen) < 0)
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
		if (connect(pgStatSock, (struct sockaddr *) & pgStatAddr, alen) < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
			errmsg("could not connect socket for statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		/*
		 * Try to send and receive a one-byte test message on the socket. This
		 * is to catch situations where the socket can be created but will not
		 * actually pass data (for instance, because kernel packet filtering
		 * rules prevent it).
		 */
		test_byte = TESTBYTEVAL;

retry1:
		if (send(pgStatSock, &test_byte, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry1;	/* if interrupted, just retry */
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
			sel_res = select(pgStatSock + 1, &rset, NULL, NULL, &tv);
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
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("test message did not get through on socket for statistics collector")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		test_byte++;			/* just make sure variable is changed */

retry2:
		if (recv(pgStatSock, &test_byte, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry2;	/* if interrupted, just retry */
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not receive test message on socket for statistics collector: %m")));
			closesocket(pgStatSock);
			pgStatSock = -1;
			continue;
		}

		if (test_byte != TESTBYTEVAL)	/* strictly paranoia ... */
		{
			ereport(LOG,
					(errcode(ERRCODE_INTERNAL_ERROR),
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
		goto startup_failed;

	/*
	 * Set the socket to non-blocking IO.  This ensures that if the collector
	 * falls behind, statistics messages will be discarded; backends won't
	 * block waiting to send messages to the collector.
	 */
	if (!pg_set_noblock(pgStatSock))
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not set statistics collector socket to nonblocking mode: %m")));
		goto startup_failed;
	}

	pg_freeaddrinfo_all(hints.ai_family, addrs);

	return;

startup_failed:
	ereport(LOG,
	  (errmsg("disabling statistics collector for lack of working socket")));

	if (addrs)
		pg_freeaddrinfo_all(hints.ai_family, addrs);

	if (pgStatSock >= 0)
		closesocket(pgStatSock);
	pgStatSock = -1;

	/* Adjust GUC variables to suppress useless activity */
	pgstat_collect_startcollector = false;
	pgstat_collect_tuplelevel = false;
	pgstat_collect_blocklevel = false;
}

/*
 * pgstat_reset_all() -
 *
 * Remove the stats file.  This is used on server start if the
 * stats_reset_on_server_start feature is enabled, or if WAL
 * recovery is needed after a crash.
 */
void
pgstat_reset_all(void)
{
	unlink(PGSTAT_STAT_FILENAME);
}

#ifdef EXEC_BACKEND

/*
 * pgstat_forkexec() -
 *
 * Format up the arglist for, then fork and exec, statistics collector process
 */
static pid_t
pgstat_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkcol";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */


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
	 * Do nothing if too soon since last collector start.  This is a safety
	 * valve to protect against continuous respawn attempts if the collector
	 * is dying immediately at launch.	Note that since we will be re-called
	 * from the postmaster main loop, we will get another chance later.
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
#ifdef EXEC_BACKEND
	switch ((pgStatPid = pgstat_forkexec()))
#else
	switch ((pgStatPid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork statistics collector: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			/* Drop our connection to postmaster's shared memory, as well */
			PGSharedMemoryDetach();

			PgstatCollectorMain(0, NULL);
			break;
#endif

		default:
			return (int) pgStatPid;
	}

	/* shouldn't get here */
	return 0;
}


/* ------------------------------------------------------------
 * Public functions used by backends follow
 *------------------------------------------------------------
 */


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
		(!pgstat_collect_tuplelevel &&
		 !pgstat_collect_blocklevel))
	{
		/* Not reporting stats, so just flush whatever we have */
		RegularTabStat.tsa_used = 0;
		SharedTabStat.tsa_used = 0;
		return;
	}

	/*
	 * For each message buffer used during the last query set the header
	 * fields and send it out.
	 */
	for (i = 0; i < RegularTabStat.tsa_used; i++)
	{
		PgStat_MsgTabstat *tsmsg = RegularTabStat.tsa_messages[i];
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
		tsmsg->m_databaseid = MyDatabaseId;
		pgstat_send(tsmsg, len);
	}
	RegularTabStat.tsa_used = 0;

	/* Ditto, for shared relations */
	for (i = 0; i < SharedTabStat.tsa_used; i++)
	{
		PgStat_MsgTabstat *tsmsg = SharedTabStat.tsa_messages[i];
		int			n;
		int			len;

		n = tsmsg->m_nentries;
		len = offsetof(PgStat_MsgTabstat, m_entry[0]) +
			n * sizeof(PgStat_TableEntry);

		/* We don't report transaction commit/abort here */
		tsmsg->m_xact_commit = 0;
		tsmsg->m_xact_rollback = 0;

		pgstat_setheader(&tsmsg->m_hdr, PGSTAT_MTYPE_TABSTAT);
		tsmsg->m_databaseid = InvalidOid;
		pgstat_send(tsmsg, len);
	}
	SharedTabStat.tsa_used = 0;
}


/* ----------
 * pgstat_vacuum_tabstat() -
 *
 *	Will tell the collector about objects he can get rid of.
 * ----------
 */
void
pgstat_vacuum_tabstat(void)
{
	HTAB	   *htab;
	PgStat_MsgTabpurge msg;
	HASH_SEQ_STATUS hstat;
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;
	int			len;

	if (pgStatSock < 0)
		return;

	/*
	 * If not done for this transaction, read the statistics collector stats
	 * file into some hash tables.
	 */
	backend_read_statsfile();

	/*
	 * Read pg_database and make a list of OIDs of all existing databases
	 */
	htab = pgstat_collect_oids(DatabaseRelationId);

	/*
	 * Search the database hash table for dead databases and tell the
	 * collector to drop them.
	 */
	hash_seq_init(&hstat, pgStatDBHash);
	while ((dbentry = (PgStat_StatDBEntry *) hash_seq_search(&hstat)) != NULL)
	{
		Oid			dbid = dbentry->databaseid;

		CHECK_FOR_INTERRUPTS();

		/* the DB entry for shared tables (with InvalidOid) is never dropped */
		if (OidIsValid(dbid) &&
			hash_search(htab, (void *) &dbid, HASH_FIND, NULL) == NULL)
			pgstat_drop_database(dbid);
	}

	/* Clean up */
	hash_destroy(htab);

	/*
	 * Lookup our own database entry; if not found, nothing more to do.
	 */
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
												 (void *) &MyDatabaseId,
												 HASH_FIND, NULL);
	if (dbentry == NULL || dbentry->tables == NULL)
		return;

	/*
	 * Similarly to above, make a list of all known relations in this DB.
	 */
	htab = pgstat_collect_oids(RelationRelationId);

	/*
	 * Initialize our messages table counter to zero
	 */
	msg.m_nentries = 0;

	/*
	 * Check for all tables listed in stats hashtable if they still exist.
	 */
	hash_seq_init(&hstat, dbentry->tables);
	while ((tabentry = (PgStat_StatTabEntry *) hash_seq_search(&hstat)) != NULL)
	{
		Oid			tabid = tabentry->tableid;

		CHECK_FOR_INTERRUPTS();

		if (hash_search(htab, (void *) &tabid, HASH_FIND, NULL) != NULL)
			continue;

		/*
		 * Not there, so add this table's Oid to the message
		 */
		msg.m_tableid[msg.m_nentries++] = tabid;

		/*
		 * If the message is full, send it out and reinitialize to empty
		 */
		if (msg.m_nentries >= PGSTAT_NUM_TABPURGE)
		{
			len = offsetof(PgStat_MsgTabpurge, m_tableid[0])
				+msg.m_nentries * sizeof(Oid);

			pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_TABPURGE);
			msg.m_databaseid = MyDatabaseId;
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
		msg.m_databaseid = MyDatabaseId;
		pgstat_send(&msg, len);
	}

	/* Clean up */
	hash_destroy(htab);
}


/* ----------
 * pgstat_collect_oids() -
 *
 *	Collect the OIDs of either all databases or all tables, according to
 *	the parameter, into a temporary hash table.  Caller should hash_destroy
 *	the result when done with it.
 * ----------
 */
static HTAB *
pgstat_collect_oids(Oid catalogid)
{
	HTAB	   *htab;
	HASHCTL		hash_ctl;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(Oid);
	hash_ctl.hash = oid_hash;
	htab = hash_create("Temporary table of OIDs",
					   PGSTAT_TAB_HASH_SIZE,
					   &hash_ctl,
					   HASH_ELEM | HASH_FUNCTION);

	rel = heap_open(catalogid, AccessShareLock);
	scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid		thisoid = HeapTupleGetOid(tup);

		CHECK_FOR_INTERRUPTS();

		(void) hash_search(htab, (void *) &thisoid, HASH_ENTER, NULL);
	}
	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	return htab;
}


/* ----------
 * pgstat_drop_database() -
 *
 *	Tell the collector that we just dropped a database.
 *	(If the message gets lost, we will still clean the dead DB eventually
 *	via future invocations of pgstat_vacuum_tabstat().)
 * ----------
 */
static void
pgstat_drop_database(Oid databaseid)
{
	PgStat_MsgDropdb msg;

	if (pgStatSock < 0)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_DROPDB);
	msg.m_databaseid = databaseid;
	pgstat_send(&msg, sizeof(msg));
}


/* ----------
 * pgstat_drop_relation() -
 *
 *	Tell the collector that we just dropped a relation.
 *	(If the message gets lost, we will still clean the dead entry eventually
 *	via future invocations of pgstat_vacuum_tabstat().)
 *
 *	Currently not used for lack of any good place to call it; we rely
 *	entirely on pgstat_vacuum_tabstat() to clean out stats for dead rels.
 * ----------
 */
#ifdef NOT_USED
void
pgstat_drop_relation(Oid relid)
{
	PgStat_MsgTabpurge msg;
	int			len;

	if (pgStatSock < 0)
		return;

	msg.m_tableid[0] = relid;
	msg.m_nentries = 1;

	len = offsetof(PgStat_MsgTabpurge, m_tableid[0]) +sizeof(Oid);

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_TABPURGE);
	msg.m_databaseid = MyDatabaseId;
	pgstat_send(&msg, len);
}
#endif /* NOT_USED */


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
	msg.m_databaseid = MyDatabaseId;
	pgstat_send(&msg, sizeof(msg));
}


/* ----------
 * pgstat_report_autovac() -
 *
 *	Called from autovacuum.c to report startup of an autovacuum process.
 *	We are called before InitPostgres is done, so can't rely on MyDatabaseId;
 *	the db OID must be passed in, instead.
 * ----------
 */
void
pgstat_report_autovac(Oid dboid)
{
	PgStat_MsgAutovacStart msg;

	if (pgStatSock < 0)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_AUTOVAC_START);
	msg.m_databaseid = dboid;
	msg.m_start_time = GetCurrentTimestamp();

	pgstat_send(&msg, sizeof(msg));
}


/* ---------
 * pgstat_report_vacuum() -
 *
 *	Tell the collector about the table we just vacuumed.
 * ---------
 */
void
pgstat_report_vacuum(Oid tableoid, bool shared,
					 bool analyze, PgStat_Counter tuples)
{
	PgStat_MsgVacuum msg;

	if (pgStatSock < 0 ||
		!pgstat_collect_tuplelevel)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_VACUUM);
	msg.m_databaseid = shared ? InvalidOid : MyDatabaseId;
	msg.m_tableoid = tableoid;
	msg.m_analyze = analyze;
	msg.m_autovacuum = IsAutoVacuumProcess();	/* is this autovacuum? */
	msg.m_vacuumtime = GetCurrentTimestamp();
	msg.m_tuples = tuples;
	pgstat_send(&msg, sizeof(msg));
}

/* --------
 * pgstat_report_analyze() -
 *
 *	Tell the collector about the table we just analyzed.
 * --------
 */
void
pgstat_report_analyze(Oid tableoid, bool shared, PgStat_Counter livetuples,
					  PgStat_Counter deadtuples)
{
	PgStat_MsgAnalyze msg;

	if (pgStatSock < 0 ||
		!pgstat_collect_tuplelevel)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_ANALYZE);
	msg.m_databaseid = shared ? InvalidOid : MyDatabaseId;
	msg.m_tableoid = tableoid;
	msg.m_autovacuum = IsAutoVacuumProcess();	/* is this autovacuum? */
	msg.m_analyzetime = GetCurrentTimestamp();
	msg.m_live_tuples = livetuples;
	msg.m_dead_tuples = deadtuples;
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
 * Enlarge a TabStatArray
 */
static void
more_tabstat_space(TabStatArray *tsarr)
{
	PgStat_MsgTabstat *newMessages;
	PgStat_MsgTabstat **msgArray;
	int			newAlloc;
	int			i;

	AssertArg(PointerIsValid(tsarr));

	newAlloc = tsarr->tsa_alloc + TABSTAT_QUANTUM;

	/* Create (another) quantum of message buffers */
	newMessages = (PgStat_MsgTabstat *)
		MemoryContextAllocZero(TopMemoryContext,
							   sizeof(PgStat_MsgTabstat) * TABSTAT_QUANTUM);

	/* Create or enlarge the pointer array */
	if (tsarr->tsa_messages == NULL)
		msgArray = (PgStat_MsgTabstat **)
			MemoryContextAlloc(TopMemoryContext,
							   sizeof(PgStat_MsgTabstat *) * newAlloc);
	else
		msgArray = (PgStat_MsgTabstat **)
			repalloc(tsarr->tsa_messages,
					 sizeof(PgStat_MsgTabstat *) * newAlloc);

	for (i = 0; i < TABSTAT_QUANTUM; i++)
		msgArray[tsarr->tsa_alloc + i] = newMessages++;
	tsarr->tsa_messages = msgArray;
	tsarr->tsa_alloc = newAlloc;

	Assert(tsarr->tsa_used < tsarr->tsa_alloc);
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
	TabStatArray *tsarr;
	PgStat_MsgTabstat *tsmsg;
	int			mb;
	int			i;

	/*
	 * Initialize data not to count at all.
	 */
	stats->tabentry = NULL;

	if (pgStatSock < 0 ||
		!(pgstat_collect_tuplelevel ||
		  pgstat_collect_blocklevel))
		return;

	tsarr = rel->rd_rel->relisshared ? &SharedTabStat : &RegularTabStat;

	/*
	 * Search the already-used message slots for this relation.
	 */
	for (mb = 0; mb < tsarr->tsa_used; mb++)
	{
		tsmsg = tsarr->tsa_messages[mb];

		for (i = tsmsg->m_nentries; --i >= 0;)
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
		 * Not found, but found a message buffer with an empty slot instead.
		 * Fine, let's use this one.
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
	if (tsarr->tsa_used >= tsarr->tsa_alloc)
		more_tabstat_space(tsarr);

	/*
	 * Use the first entry of the next message buffer.
	 */
	mb = tsarr->tsa_used++;
	tsmsg = tsarr->tsa_messages[mb];
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
	if (!pgstat_collect_tuplelevel &&
		!pgstat_collect_blocklevel)
		return;

	pgStatXactCommit++;

	/*
	 * If there was no relation activity yet, just make one existing message
	 * buffer used without slots, causing the next report to tell new
	 * xact-counters.
	 */
	if (RegularTabStat.tsa_alloc == 0)
		more_tabstat_space(&RegularTabStat);

	if (RegularTabStat.tsa_used == 0)
	{
		RegularTabStat.tsa_used++;
		RegularTabStat.tsa_messages[0]->m_nentries = 0;
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
	if (!pgstat_collect_tuplelevel &&
		!pgstat_collect_blocklevel)
		return;

	pgStatXactRollback++;

	/*
	 * If there was no relation activity yet, just make one existing message
	 * buffer used without slots, causing the next report to tell new
	 * xact-counters.
	 */
	if (RegularTabStat.tsa_alloc == 0)
		more_tabstat_space(&RegularTabStat);

	if (RegularTabStat.tsa_used == 0)
	{
		RegularTabStat.tsa_used++;
		RegularTabStat.tsa_messages[0]->m_nentries = 0;
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
	/*
	 * If not done for this transaction, read the statistics collector stats
	 * file into some hash tables.
	 */
	backend_read_statsfile();

	/*
	 * Lookup the requested database; return NULL if not found
	 */
	return (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
											  (void *) &dbid,
											  HASH_FIND, NULL);
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
	Oid			dbid;
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;

	/*
	 * If not done for this transaction, read the statistics collector stats
	 * file into some hash tables.
	 */
	backend_read_statsfile();

	/*
	 * Lookup our database, then look in its table hash table.
	 */
	dbid = MyDatabaseId;
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
												 (void *) &dbid,
												 HASH_FIND, NULL);
	if (dbentry != NULL && dbentry->tables != NULL)
	{
		tabentry = (PgStat_StatTabEntry *) hash_search(dbentry->tables,
													   (void *) &relid,
													   HASH_FIND, NULL);
		if (tabentry)
			return tabentry;
	}

	/*
	 * If we didn't find it, maybe it's a shared table.
	 */
	dbid = InvalidOid;
	dbentry = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
												 (void *) &dbid,
												 HASH_FIND, NULL);
	if (dbentry != NULL && dbentry->tables != NULL)
	{
		tabentry = (PgStat_StatTabEntry *) hash_search(dbentry->tables,
													   (void *) &relid,
													   HASH_FIND, NULL);
		if (tabentry)
			return tabentry;
	}

	return NULL;
}


/* ----------
 * pgstat_fetch_stat_beentry() -
 *
 *	Support function for the SQL-callable pgstat* functions. Returns
 *	our local copy of the current-activity entry for one backend.
 *
 *	NB: caller is responsible for a check if the user is permitted to see
 *	this info (especially the querystring).
 * ----------
 */
PgBackendStatus *
pgstat_fetch_stat_beentry(int beid)
{
	pgstat_read_current_status();

	if (beid < 1 || beid > localNumBackends)
		return NULL;

	return &localBackendStatusTable[beid - 1];
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
	pgstat_read_current_status();

	return localNumBackends;
}


/* ------------------------------------------------------------
 * Functions for management of the shared-memory PgBackendStatus array
 * ------------------------------------------------------------
 */

static PgBackendStatus *BackendStatusArray = NULL;
static PgBackendStatus *MyBEEntry = NULL;


/*
 * Report shared-memory space needed by CreateSharedBackendStatus.
 */
Size
BackendStatusShmemSize(void)
{
	Size		size;

	size = mul_size(sizeof(PgBackendStatus), MaxBackends);
	return size;
}

/*
 * Initialize the shared status array during postmaster startup.
 */
void
CreateSharedBackendStatus(void)
{
	Size		size = BackendStatusShmemSize();
	bool		found;

	/* Create or attach to the shared array */
	BackendStatusArray = (PgBackendStatus *)
		ShmemInitStruct("Backend Status Array", size, &found);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		MemSet(BackendStatusArray, 0, size);
	}
}


/* ----------
 * pgstat_bestart() -
 *
 *	Initialize this backend's entry in the PgBackendStatus array,
 *	and set up an on-proc-exit hook that will clear it again.
 *	Called from InitPostgres.  MyBackendId and MyDatabaseId must be set.
 * ----------
 */
void
pgstat_bestart(void)
{
	volatile PgBackendStatus *beentry;
	TimestampTz proc_start_timestamp;
	Oid			userid;
	SockAddr	clientaddr;

	Assert(MyBackendId >= 1 && MyBackendId <= MaxBackends);
	MyBEEntry = &BackendStatusArray[MyBackendId - 1];

	/*
	 * To minimize the time spent modifying the entry, fetch all the needed
	 * data first.
	 *
	 * If we have a MyProcPort, use its session start time (for consistency,
	 * and to save a kernel call).
	 */
	if (MyProcPort)
		proc_start_timestamp = MyProcPort->SessionStartTime;
	else
		proc_start_timestamp = GetCurrentTimestamp();
	userid = GetSessionUserId();

	/*
	 * We may not have a MyProcPort (eg, if this is the autovacuum process).
	 * If so, use all-zeroes client address, which is dealt with specially in
	 * pg_stat_get_backend_client_addr and pg_stat_get_backend_client_port.
	 */
	if (MyProcPort)
		memcpy(&clientaddr, &MyProcPort->raddr, sizeof(clientaddr));
	else
		MemSet(&clientaddr, 0, sizeof(clientaddr));

	/*
	 * Initialize my status entry, following the protocol of bumping
	 * st_changecount before and after; and make sure it's even afterwards. We
	 * use a volatile pointer here to ensure the compiler doesn't try to get
	 * cute.
	 */
	beentry = MyBEEntry;
	do
	{
		beentry->st_changecount++;
	} while ((beentry->st_changecount & 1) == 0);

	beentry->st_procpid = MyProcPid;
	beentry->st_proc_start_timestamp = proc_start_timestamp;
	beentry->st_activity_start_timestamp = 0;
	beentry->st_databaseid = MyDatabaseId;
	beentry->st_userid = userid;
	beentry->st_clientaddr = clientaddr;
	beentry->st_waiting = false;
	beentry->st_activity[0] = '\0';
	/* Also make sure the last byte in the string area is always 0 */
	beentry->st_activity[PGBE_ACTIVITY_SIZE - 1] = '\0';

	beentry->st_changecount++;
	Assert((beentry->st_changecount & 1) == 0);

	/*
	 * Set up a process-exit hook to clean up.
	 */
	on_shmem_exit(pgstat_beshutdown_hook, 0);
}

/*
 * Shut down a single backend's statistics reporting at process exit.
 *
 * Flush any remaining statistics counts out to the collector.
 * Without this, operations triggered during backend exit (such as
 * temp table deletions) won't be counted.
 *
 * Lastly, clear out our entry in the PgBackendStatus array.
 */
static void
pgstat_beshutdown_hook(int code, Datum arg)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	pgstat_report_tabstat();

	/*
	 * Clear my status entry, following the protocol of bumping st_changecount
	 * before and after.  We use a volatile pointer here to ensure the
	 * compiler doesn't try to get cute.
	 */
	beentry->st_changecount++;

	beentry->st_procpid = 0;	/* mark invalid */

	beentry->st_changecount++;
	Assert((beentry->st_changecount & 1) == 0);
}


/* ----------
 * pgstat_report_activity() -
 *
 *	Called from tcop/postgres.c to report what the backend is actually doing
 *	(usually "<IDLE>" or the start of the query to be executed).
 * ----------
 */
void
pgstat_report_activity(const char *cmd_str)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	TimestampTz start_timestamp;
	int			len;

	if (!pgstat_collect_querystring || !beentry)
		return;

	/*
	 * To minimize the time spent modifying the entry, fetch all the needed
	 * data first.
	 */
	start_timestamp = GetCurrentStatementStartTimestamp();

	len = strlen(cmd_str);
	len = pg_mbcliplen(cmd_str, len, PGBE_ACTIVITY_SIZE - 1);

	/*
	 * Update my status entry, following the protocol of bumping
	 * st_changecount before and after.  We use a volatile pointer here to
	 * ensure the compiler doesn't try to get cute.
	 */
	beentry->st_changecount++;

	beentry->st_activity_start_timestamp = start_timestamp;
	memcpy((char *) beentry->st_activity, cmd_str, len);
	beentry->st_activity[len] = '\0';

	beentry->st_changecount++;
	Assert((beentry->st_changecount & 1) == 0);
}


/* ----------
 * pgstat_report_waiting() -
 *
 *	Called from lock manager to report beginning or end of a lock wait.
 *
 * NB: this *must* be able to survive being called before MyBEEntry has been
 * initialized.
 * ----------
 */
void
pgstat_report_waiting(bool waiting)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!pgstat_collect_querystring || !beentry)
		return;

	/*
	 * Since this is a single-byte field in a struct that only this process
	 * may modify, there seems no need to bother with the st_changecount
	 * protocol.  The update must appear atomic in any case.
	 */
	beentry->st_waiting = waiting;
}


/* ----------
 * pgstat_read_current_status() -
 *
 *	Copy the current contents of the PgBackendStatus array to local memory,
 *	if not already done in this transaction.
 * ----------
 */
static void
pgstat_read_current_status(void)
{
	TransactionId topXid = GetTopTransactionId();
	volatile PgBackendStatus *beentry;
	PgBackendStatus *localentry;
	int			i;

	Assert(!pgStatRunningInCollector);
	if (TransactionIdEquals(pgStatLocalStatusXact, topXid))
		return;					/* already done */

	localBackendStatusTable = (PgBackendStatus *)
		MemoryContextAlloc(TopTransactionContext,
						   sizeof(PgBackendStatus) * MaxBackends);
	localNumBackends = 0;

	beentry = BackendStatusArray;
	localentry = localBackendStatusTable;
	for (i = 1; i <= MaxBackends; i++)
	{
		/*
		 * Follow the protocol of retrying if st_changecount changes while we
		 * copy the entry, or if it's odd.  (The check for odd is needed to
		 * cover the case where we are able to completely copy the entry while
		 * the source backend is between increment steps.)	We use a volatile
		 * pointer here to ensure the compiler doesn't try to get cute.
		 */
		for (;;)
		{
			int			save_changecount = beentry->st_changecount;

			/*
			 * XXX if PGBE_ACTIVITY_SIZE is really large, it might be best to
			 * use strcpy not memcpy for copying the activity string?
			 */
			memcpy(localentry, (char *) beentry, sizeof(PgBackendStatus));

			if (save_changecount == beentry->st_changecount &&
				(save_changecount & 1) == 0)
				break;

			/* Make sure we can break out of loop if stuck... */
			CHECK_FOR_INTERRUPTS();
		}

		beentry++;
		/* Only valid entries get included into the local array */
		if (localentry->st_procpid > 0)
		{
			localentry++;
			localNumBackends++;
		}
	}

	pgStatLocalStatusXact = topXid;
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
pgstat_setheader(PgStat_MsgHdr *hdr, StatMsgType mtype)
{
	hdr->m_type = mtype;
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
	int			rc;

	if (pgStatSock < 0)
		return;

	((PgStat_MsgHdr *) msg)->m_size = len;

	/* We'll retry after EINTR, but ignore all other failures */
	do
	{
		rc = send(pgStatSock, msg, len, 0);
	} while (rc < 0 && errno == EINTR);

#ifdef USE_ASSERT_CHECKING
	/* In debug builds, log send failures ... */
	if (rc < 0)
		elog(LOG, "could not send to statistics collector: %m");
#endif
}


/* ----------
 * PgstatCollectorMain() -
 *
 *	Start up the statistics collector process.	This is the body of the
 *	postmaster child process.
 *
 *	The argc/argv parameters are valid only in EXEC_BACKEND case.
 * ----------
 */
NON_EXEC_STATIC void
PgstatCollectorMain(int argc, char *argv[])
{
	struct itimerval write_timeout;
	bool		need_timer = false;
	int			len;
	PgStat_Msg	msg;

#ifndef WIN32
#ifdef HAVE_POLL
	struct pollfd input_fd;
#else
	struct timeval sel_timeout;
	fd_set		rfds;
#endif
#endif

	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	MyProcPid = getpid();		/* reset MyProcPid */

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.  (pgstat probably never has
	 * any child processes, but for consistency we make all postmaster
	 * child processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Ignore all signals usually bound to some action in the postmaster,
	 * except SIGQUIT and SIGALRM.
	 */
	pqsignal(SIGHUP, SIG_IGN);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, pgstat_exit);
	pqsignal(SIGALRM, force_statwrite);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);
	PG_SETMASK(&UnBlockSig);

	/*
	 * Identify myself via ps
	 */
	init_ps_display("stats collector process", "", "", "");

	/*
	 * Arrange to write the initial status file right away
	 */
	need_statwrite = true;

	/* Preset the delay between status file writes */
	MemSet(&write_timeout, 0, sizeof(struct itimerval));
	write_timeout.it_value.tv_sec = PGSTAT_STAT_INTERVAL / 1000;
	write_timeout.it_value.tv_usec = (PGSTAT_STAT_INTERVAL % 1000) * 1000;

	/*
	 * Read in an existing statistics stats file or initialize the stats to
	 * zero.
	 */
	pgStatRunningInCollector = true;
	pgstat_read_statsfile(&pgStatDBHash, InvalidOid);

	/*
	 * Setup the descriptor set for select(2).	Since only one bit in the set
	 * ever changes, we need not repeat FD_ZERO each time.
	 */
#if !defined(HAVE_POLL) && !defined(WIN32)
	FD_ZERO(&rfds);
#endif

	/*
	 * Loop to process messages until we get SIGQUIT or detect ungraceful
	 * death of our parent postmaster.
	 *
	 * For performance reasons, we don't want to do a PostmasterIsAlive() test
	 * after every message; instead, do it at statwrite time and if
	 * select()/poll() is interrupted by timeout.
	 */
	for (;;)
	{
		int			got_data;

		/*
		 * Quit if we get SIGQUIT from the postmaster.
		 */
		if (need_exit)
			break;

		/*
		 * If time to write the stats file, do so.	Note that the alarm
		 * interrupt isn't re-enabled immediately, but only after we next
		 * receive a stats message; so no cycles are wasted when there is
		 * nothing going on.
		 */
		if (need_statwrite)
		{
			/* Check for postmaster death; if so we'll write file below */
			if (!PostmasterIsAlive(true))
				break;

			pgstat_write_statsfile();
			need_statwrite = false;
			need_timer = true;
		}

		/*
		 * Wait for a message to arrive; but not for more than
		 * PGSTAT_SELECT_TIMEOUT seconds. (This determines how quickly we will
		 * shut down after an ungraceful postmaster termination; so it needn't
		 * be very fast.  However, on some systems SIGQUIT won't interrupt the
		 * poll/select call, so this also limits speed of response to SIGQUIT,
		 * which is more important.)
		 *
		 * We use poll(2) if available, otherwise select(2).
		 * Win32 has its own implementation.
		 */
#ifndef WIN32
#ifdef HAVE_POLL
		input_fd.fd = pgStatSock;
		input_fd.events = POLLIN | POLLERR;
		input_fd.revents = 0;

		if (poll(&input_fd, 1, PGSTAT_SELECT_TIMEOUT * 1000) < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("poll() failed in statistics collector: %m")));
		}

		got_data = (input_fd.revents != 0);
#else							/* !HAVE_POLL */

		FD_SET(pgStatSock, &rfds);

		/*
		 * timeout struct is modified by select() on some operating systems,
		 * so re-fill it each time.
		 */
		sel_timeout.tv_sec = PGSTAT_SELECT_TIMEOUT;
		sel_timeout.tv_usec = 0;

		if (select(pgStatSock + 1, &rfds, NULL, NULL, &sel_timeout) < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("select() failed in statistics collector: %m")));
		}

		got_data = FD_ISSET(pgStatSock, &rfds);
#endif   /* HAVE_POLL */
#else /* WIN32 */
		got_data = pgwin32_waitforsinglesocket(pgStatSock, FD_READ,
											   PGSTAT_SELECT_TIMEOUT*1000);
#endif

		/*
		 * If there is a message on the socket, read it and check for
		 * validity.
		 */
		if (got_data)
		{
			len = recv(pgStatSock, (char *) &msg,
					   sizeof(PgStat_Msg), 0);
			if (len < 0)
			{
				if (errno == EINTR)
					continue;
				ereport(ERROR,
						(errcode_for_socket_access(),
						 errmsg("could not read statistics message: %m")));
			}

			/*
			 * We ignore messages that are smaller than our common header
			 */
			if (len < sizeof(PgStat_MsgHdr))
				continue;

			/*
			 * The received length must match the length in the header
			 */
			if (msg.msg_hdr.m_size != len)
				continue;

			/*
			 * O.K. - we accept this message.  Process it.
			 */
			switch (msg.msg_hdr.m_type)
			{
				case PGSTAT_MTYPE_DUMMY:
					break;

				case PGSTAT_MTYPE_TABSTAT:
					pgstat_recv_tabstat((PgStat_MsgTabstat *) &msg, len);
					break;

				case PGSTAT_MTYPE_TABPURGE:
					pgstat_recv_tabpurge((PgStat_MsgTabpurge *) &msg, len);
					break;

				case PGSTAT_MTYPE_DROPDB:
					pgstat_recv_dropdb((PgStat_MsgDropdb *) &msg, len);
					break;

				case PGSTAT_MTYPE_RESETCOUNTER:
					pgstat_recv_resetcounter((PgStat_MsgResetcounter *) &msg,
											 len);
					break;

				case PGSTAT_MTYPE_AUTOVAC_START:
					pgstat_recv_autovac((PgStat_MsgAutovacStart *) &msg, len);
					break;

				case PGSTAT_MTYPE_VACUUM:
					pgstat_recv_vacuum((PgStat_MsgVacuum *) &msg, len);
					break;

				case PGSTAT_MTYPE_ANALYZE:
					pgstat_recv_analyze((PgStat_MsgAnalyze *) &msg, len);
					break;

				default:
					break;
			}

			/*
			 * If this is the first message after we wrote the stats file the
			 * last time, enable the alarm interrupt to make it be written
			 * again later.
			 */
			if (need_timer)
			{
				if (setitimer(ITIMER_REAL, &write_timeout, NULL))
					ereport(ERROR,
					(errmsg("could not set statistics collector timer: %m")));
				need_timer = false;
			}
		}
		else
		{
			/*
			 * We can only get here if the select/poll timeout elapsed. Check
			 * for postmaster death.
			 */
			if (!PostmasterIsAlive(true))
				break;
		}
	}							/* end of message-processing loop */

	/*
	 * Save the final stats to reuse at next startup.
	 */
	pgstat_write_statsfile();

	exit(0);
}


/* SIGQUIT signal handler for collector process */
static void
pgstat_exit(SIGNAL_ARGS)
{
	need_exit = true;
}

/* SIGALRM signal handler for collector process */
static void
force_statwrite(SIGNAL_ARGS)
{
	need_statwrite = true;
}


/*
 * Lookup the hash table entry for the specified database. If no hash
 * table entry exists, initialize it, if the create parameter is true.
 * Else, return NULL.
 */
static PgStat_StatDBEntry *
pgstat_get_db_entry(Oid databaseid, bool create)
{
	PgStat_StatDBEntry *result;
	bool		found;
	HASHACTION	action = (create ? HASH_ENTER : HASH_FIND);

	/* Lookup or create the hash table entry for this database */
	result = (PgStat_StatDBEntry *) hash_search(pgStatDBHash,
												&databaseid,
												action, &found);

	if (!create && !found)
		return NULL;

	/* If not found, initialize the new one. */
	if (!found)
	{
		HASHCTL		hash_ctl;

		result->tables = NULL;
		result->n_xact_commit = 0;
		result->n_xact_rollback = 0;
		result->n_blocks_fetched = 0;
		result->n_blocks_hit = 0;
		result->last_autovac_time = 0;

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(PgStat_StatTabEntry);
		hash_ctl.hash = oid_hash;
		result->tables = hash_create("Per-database table",
									 PGSTAT_TAB_HASH_SIZE,
									 &hash_ctl,
									 HASH_ELEM | HASH_FUNCTION);
	}

	return result;
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
	FILE	   *fpout;
	int32		format_id;

	/*
	 * Open the statistics temp file to write out the current values.
	 */
	fpout = fopen(PGSTAT_STAT_TMPFILE, PG_BINARY_W);
	if (fpout == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open temporary statistics file \"%s\": %m",
						PGSTAT_STAT_TMPFILE)));
		return;
	}

	/*
	 * Write the file header --- currently just a format ID.
	 */
	format_id = PGSTAT_FILE_FORMAT_ID;
	fwrite(&format_id, sizeof(format_id), 1, fpout);

	/*
	 * Walk through the database table.
	 */
	hash_seq_init(&hstat, pgStatDBHash);
	while ((dbentry = (PgStat_StatDBEntry *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * Write out the DB entry including the number of live backends. We
		 * don't write the tables pointer since it's of no use to any other
		 * process.
		 */
		fputc('D', fpout);
		fwrite(dbentry, offsetof(PgStat_StatDBEntry, tables), 1, fpout);

		/*
		 * Walk through the database's access stats per table.
		 */
		hash_seq_init(&tstat, dbentry->tables);
		while ((tabentry = (PgStat_StatTabEntry *) hash_seq_search(&tstat)) != NULL)
		{
			fputc('T', fpout);
			fwrite(tabentry, sizeof(PgStat_StatTabEntry), 1, fpout);
		}

		/*
		 * Mark the end of this DB
		 */
		fputc('d', fpout);
	}

	/*
	 * No more output to be done. Close the temp file and replace the old
	 * pgstat.stat with it.  The ferror() check replaces testing for error
	 * after each individual fputc or fwrite above.
	 */
	fputc('E', fpout);

	if (ferror(fpout))
	{
		ereport(LOG,
				(errcode_for_file_access(),
			   errmsg("could not write temporary statistics file \"%s\": %m",
					  PGSTAT_STAT_TMPFILE)));
		fclose(fpout);
		unlink(PGSTAT_STAT_TMPFILE);
	}
	else if (fclose(fpout) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
			   errmsg("could not close temporary statistics file \"%s\": %m",
					  PGSTAT_STAT_TMPFILE)));
		unlink(PGSTAT_STAT_TMPFILE);
	}
	else if (rename(PGSTAT_STAT_TMPFILE, PGSTAT_STAT_FILENAME) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename temporary statistics file \"%s\" to \"%s\": %m",
						PGSTAT_STAT_TMPFILE, PGSTAT_STAT_FILENAME)));
		unlink(PGSTAT_STAT_TMPFILE);
	}
}


/* ----------
 * pgstat_read_statsfile() -
 *
 *	Reads in an existing statistics collector file and initializes the
 *	databases' hash table (whose entries point to the tables' hash tables).
 * ----------
 */
static void
pgstat_read_statsfile(HTAB **dbhash, Oid onlydb)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatDBEntry dbbuf;
	PgStat_StatTabEntry *tabentry;
	PgStat_StatTabEntry tabbuf;
	HASHCTL		hash_ctl;
	HTAB	   *tabhash = NULL;
	FILE	   *fpin;
	int32		format_id;
	bool		found;
	MemoryContext use_mcxt;
	int			mcxt_flags;

	/*
	 * If running in the collector or the autovacuum process, we use the
	 * DynaHashCxt memory context.	If running in a backend, we use the
	 * TopTransactionContext instead, so the caller must only know the last
	 * XactId when this call happened to know if his tables are still valid or
	 * already gone!
	 */
	if (pgStatRunningInCollector || IsAutoVacuumProcess())
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
	hash_ctl.hash = oid_hash;
	hash_ctl.hcxt = use_mcxt;
	*dbhash = hash_create("Databases hash", PGSTAT_DB_HASH_SIZE, &hash_ctl,
						  HASH_ELEM | HASH_FUNCTION | mcxt_flags);

	/*
	 * Try to open the status file. If it doesn't exist, the backends simply
	 * return zero for anything and the collector simply starts from scratch
	 * with empty counters.
	 */
	if ((fpin = AllocateFile(PGSTAT_STAT_FILENAME, PG_BINARY_R)) == NULL)
		return;

	/*
	 * Verify it's of the expected format.
	 */
	if (fread(&format_id, 1, sizeof(format_id), fpin) != sizeof(format_id)
		|| format_id != PGSTAT_FILE_FORMAT_ID)
	{
		ereport(pgStatRunningInCollector ? LOG : WARNING,
				(errmsg("corrupted pgstat.stat file")));
		goto done;
	}

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
				 * follows. Subsequently, zero to many 'T' entries will follow
				 * until a 'd' is encountered.
				 */
			case 'D':
				if (fread(&dbbuf, 1, offsetof(PgStat_StatDBEntry, tables),
						  fpin) != offsetof(PgStat_StatDBEntry, tables))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}

				/*
				 * Add to the DB hash
				 */
				dbentry = (PgStat_StatDBEntry *) hash_search(*dbhash,
												  (void *) &dbbuf.databaseid,
															 HASH_ENTER,
															 &found);
				if (found)
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}

				memcpy(dbentry, &dbbuf, sizeof(PgStat_StatDBEntry));
				dbentry->tables = NULL;

				/*
				 * Don't collect tables if not the requested DB (or the
				 * shared-table info)
				 */
				if (onlydb != InvalidOid)
				{
					if (dbbuf.databaseid != onlydb &&
						dbbuf.databaseid != InvalidOid)
						break;
				}

				memset(&hash_ctl, 0, sizeof(hash_ctl));
				hash_ctl.keysize = sizeof(Oid);
				hash_ctl.entrysize = sizeof(PgStat_StatTabEntry);
				hash_ctl.hash = oid_hash;
				hash_ctl.hcxt = use_mcxt;
				dbentry->tables = hash_create("Per-database table",
											  PGSTAT_TAB_HASH_SIZE,
											  &hash_ctl,
									 HASH_ELEM | HASH_FUNCTION | mcxt_flags);

				/*
				 * Arrange that following 'T's add entries to this database's
				 * tables hash table.
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
				if (fread(&tabbuf, 1, sizeof(PgStat_StatTabEntry),
						  fpin) != sizeof(PgStat_StatTabEntry))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}

				/*
				 * Skip if table belongs to a not requested database.
				 */
				if (tabhash == NULL)
					break;

				tabentry = (PgStat_StatTabEntry *) hash_search(tabhash,
													(void *) &tabbuf.tableid,
														 HASH_ENTER, &found);

				if (found)
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}

				memcpy(tabentry, &tabbuf, sizeof(tabbuf));
				break;

				/*
				 * 'E'	The EOF marker of a complete stats file.
				 */
			case 'E':
				goto done;

			default:
				ereport(pgStatRunningInCollector ? LOG : WARNING,
						(errmsg("corrupted pgstat.stat file")));
				goto done;
		}
	}

done:
	FreeFile(fpin);
}

/*
 * If not done for this transaction, read the statistics collector
 * stats file into some hash tables.
 *
 * Because we store the tables in TopTransactionContext, the result
 * is good for the entire current main transaction.
 *
 * Inside the autovacuum process, the statfile is assumed to be valid
 * "forever", that is one iteration, within one database.  This means
 * we only consider the statistics as they were when the autovacuum
 * iteration started.
 */
static void
backend_read_statsfile(void)
{
	if (IsAutoVacuumProcess())
	{
		/* already read it? */
		if (pgStatDBHash)
			return;
		Assert(!pgStatRunningInCollector);
		pgstat_read_statsfile(&pgStatDBHash, InvalidOid);
	}
	else
	{
		TransactionId topXid = GetTopTransactionId();

		if (!TransactionIdEquals(pgStatDBHashXact, topXid))
		{
			Assert(!pgStatRunningInCollector);
			pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId);
			pgStatDBHashXact = topXid;
		}
	}
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

	dbentry = pgstat_get_db_entry(msg->m_databaseid, true);

	/*
	 * Update database-wide stats.
	 */
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

		if (!found)
		{
			/*
			 * If it's a new table entry, initialize counters to the values we
			 * just got.
			 */
			tabentry->numscans = tabmsg[i].t_numscans;
			tabentry->tuples_returned = tabmsg[i].t_tuples_returned;
			tabentry->tuples_fetched = tabmsg[i].t_tuples_fetched;
			tabentry->tuples_inserted = tabmsg[i].t_tuples_inserted;
			tabentry->tuples_updated = tabmsg[i].t_tuples_updated;
			tabentry->tuples_deleted = tabmsg[i].t_tuples_deleted;

			tabentry->n_live_tuples = tabmsg[i].t_tuples_inserted;
			tabentry->n_dead_tuples = tabmsg[i].t_tuples_updated +
				tabmsg[i].t_tuples_deleted;
			tabentry->last_anl_tuples = 0;
			tabentry->vacuum_timestamp = 0;
			tabentry->autovac_vacuum_timestamp = 0;
			tabentry->analyze_timestamp = 0;
			tabentry->autovac_analyze_timestamp = 0;

			tabentry->blocks_fetched = tabmsg[i].t_blocks_fetched;
			tabentry->blocks_hit = tabmsg[i].t_blocks_hit;
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

			tabentry->n_live_tuples += tabmsg[i].t_tuples_inserted -
				tabmsg[i].t_tuples_deleted;
			tabentry->n_dead_tuples += tabmsg[i].t_tuples_updated +
				tabmsg[i].t_tuples_deleted;

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
	int			i;

	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);

	/*
	 * No need to purge if we don't even know the database.
	 */
	if (!dbentry || !dbentry->tables)
		return;

	/*
	 * Process all table entries in the message.
	 */
	for (i = 0; i < msg->m_nentries; i++)
	{
		/* Remove from hashtable if present; we don't care if it's not. */
		(void) hash_search(dbentry->tables,
						   (void *) &(msg->m_tableid[i]),
						   HASH_REMOVE, NULL);
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
	 * Lookup the database in the hashtable.
	 */
	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);

	/*
	 * If found, remove it.
	 */
	if (dbentry)
	{
		if (dbentry->tables != NULL)
			hash_destroy(dbentry->tables);

		if (hash_search(pgStatDBHash,
						(void *) &(dbentry->databaseid),
						HASH_REMOVE, NULL) == NULL)
			ereport(ERROR,
					(errmsg("database hash table corrupted "
							"during cleanup --- abort")));
	}
}


/* ----------
 * pgstat_recv_resetcounter() -
 *
 *	Reset the statistics for the specified database.
 * ----------
 */
static void
pgstat_recv_resetcounter(PgStat_MsgResetcounter *msg, int len)
{
	HASHCTL		hash_ctl;
	PgStat_StatDBEntry *dbentry;

	/*
	 * Lookup the database in the hashtable.  Nothing to do if not there.
	 */
	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);

	if (!dbentry)
		return;

	/*
	 * We simply throw away all the database's table entries by recreating a
	 * new hash table for them.
	 */
	if (dbentry->tables != NULL)
		hash_destroy(dbentry->tables);

	dbentry->tables = NULL;
	dbentry->n_xact_commit = 0;
	dbentry->n_xact_rollback = 0;
	dbentry->n_blocks_fetched = 0;
	dbentry->n_blocks_hit = 0;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(PgStat_StatTabEntry);
	hash_ctl.hash = oid_hash;
	dbentry->tables = hash_create("Per-database table",
								  PGSTAT_TAB_HASH_SIZE,
								  &hash_ctl,
								  HASH_ELEM | HASH_FUNCTION);
}

/* ----------
 * pgstat_recv_autovac() -
 *
 *	Process an autovacuum signalling message.
 * ----------
 */
static void
pgstat_recv_autovac(PgStat_MsgAutovacStart *msg, int len)
{
	PgStat_StatDBEntry *dbentry;

	/*
	 * Lookup the database in the hashtable.  Don't create the entry if it
	 * doesn't exist, because autovacuum may be processing a template
	 * database.  If this isn't the case, the database is most likely to have
	 * an entry already.  (If it doesn't, not much harm is done anyway --
	 * it'll get created as soon as somebody actually uses the database.)
	 */
	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);
	if (dbentry == NULL)
		return;

	/*
	 * Store the last autovacuum time in the database entry.
	 */
	dbentry->last_autovac_time = msg->m_start_time;
}

/* ----------
 * pgstat_recv_vacuum() -
 *
 *	Process a VACUUM message.
 * ----------
 */
static void
pgstat_recv_vacuum(PgStat_MsgVacuum *msg, int len)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;

	/*
	 * Don't create either the database or table entry if it doesn't already
	 * exist.  This avoids bloating the stats with entries for stuff that is
	 * only touched by vacuum and not by live operations.
	 */
	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);
	if (dbentry == NULL)
		return;

	tabentry = hash_search(dbentry->tables, &(msg->m_tableoid),
						   HASH_FIND, NULL);
	if (tabentry == NULL)
		return;

	if (msg->m_autovacuum)
		tabentry->autovac_vacuum_timestamp = msg->m_vacuumtime;
	else
		tabentry->vacuum_timestamp = msg->m_vacuumtime;
	tabentry->n_live_tuples = msg->m_tuples;
	tabentry->n_dead_tuples = 0;
	if (msg->m_analyze)
	{
		tabentry->last_anl_tuples = msg->m_tuples;
		if (msg->m_autovacuum)
			tabentry->autovac_analyze_timestamp = msg->m_vacuumtime;
		else
			tabentry->analyze_timestamp = msg->m_vacuumtime;
	}
	else
	{
		/* last_anl_tuples must never exceed n_live_tuples */
		tabentry->last_anl_tuples = Min(tabentry->last_anl_tuples,
										msg->m_tuples);
	}
}

/* ----------
 * pgstat_recv_analyze() -
 *
 *	Process an ANALYZE message.
 * ----------
 */
static void
pgstat_recv_analyze(PgStat_MsgAnalyze *msg, int len)
{
	PgStat_StatDBEntry *dbentry;
	PgStat_StatTabEntry *tabentry;

	/*
	 * Don't create either the database or table entry if it doesn't already
	 * exist.  This avoids bloating the stats with entries for stuff that is
	 * only touched by analyze and not by live operations.
	 */
	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);
	if (dbentry == NULL)
		return;

	tabentry = hash_search(dbentry->tables, &(msg->m_tableoid),
						   HASH_FIND, NULL);
	if (tabentry == NULL)
		return;

	if (msg->m_autovacuum)
		tabentry->autovac_analyze_timestamp = msg->m_analyzetime;
	else
		tabentry->analyze_timestamp = msg->m_analyzetime;
	tabentry->n_live_tuples = msg->m_live_tuples;
	tabentry->n_dead_tuples = msg->m_dead_tuples;
	tabentry->last_anl_tuples = msg->m_live_tuples + msg->m_dead_tuples;
}
