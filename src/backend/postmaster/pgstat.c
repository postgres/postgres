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
 *	Copyright (c) 2001-2005, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/backend/postmaster/pgstat.c,v 1.111.2.3 2006/05/19 15:15:38 alvherre Exp $
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
#include "catalog/pg_database.h"
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
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/syscache.h"


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

#define PGSTAT_DESTROY_DELAY	10000	/* How long to keep destroyed objects
										 * known, to give delayed UDP packets
										 * time to arrive; in milliseconds. */

#define PGSTAT_DESTROY_COUNT	(PGSTAT_DESTROY_DELAY / PGSTAT_STAT_INTERVAL)

#define PGSTAT_RESTART_INTERVAL 60		/* How often to attempt to restart a
										 * failed statistics collector; in
										 * seconds. */

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
bool		pgstat_collect_resetonpmstart = false;
bool		pgstat_collect_querystring = false;
bool		pgstat_collect_tuplelevel = false;
bool		pgstat_collect_blocklevel = false;

/* ----------
 * Local data
 * ----------
 */
NON_EXEC_STATIC int pgStatSock = -1;
NON_EXEC_STATIC int pgStatPipe[2] = {-1, -1};
static struct sockaddr_storage pgStatAddr;
static pid_t pgStatCollectorPid = 0;

static time_t last_pgstat_start_time;

static long pgStatNumMessages = 0;

static bool pgStatRunningInCollector = FALSE;

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
static HTAB *pgStatBeDead = NULL;
static PgStat_StatBeEntry *pgStatBeTable = NULL;
static int	pgStatNumBackends = 0;


/* ----------
 * Local function forward declarations
 * ----------
 */
#ifdef EXEC_BACKEND

typedef enum STATS_PROCESS_TYPE
{
	STAT_PROC_BUFFER,
	STAT_PROC_COLLECTOR
}	STATS_PROCESS_TYPE;

static pid_t pgstat_forkexec(STATS_PROCESS_TYPE procType);
static void pgstat_parseArgs(int argc, char *argv[]);
#endif

NON_EXEC_STATIC void PgstatBufferMain(int argc, char *argv[]);
NON_EXEC_STATIC void PgstatCollectorMain(int argc, char *argv[]);
static void pgstat_recvbuffer(void);
static void pgstat_exit(SIGNAL_ARGS);
static void pgstat_die(SIGNAL_ARGS);
static void pgstat_beshutdown_hook(int code, Datum arg);

static PgStat_StatDBEntry *pgstat_get_db_entry(Oid databaseid, bool create);
static int	pgstat_add_backend(PgStat_MsgHdr *msg);
static void pgstat_sub_backend(int procpid);
static void pgstat_drop_database(Oid databaseid);
static void pgstat_write_statsfile(void);
static void pgstat_read_statsfile(HTAB **dbhash, Oid onlydb,
					  PgStat_StatBeEntry **betab,
					  int *numbackends);
static void backend_read_statsfile(void);

static void pgstat_setheader(PgStat_MsgHdr *hdr, StatMsgType mtype);
static void pgstat_send(void *msg, int len);

static void pgstat_recv_bestart(PgStat_MsgBestart *msg, int len);
static void pgstat_recv_beterm(PgStat_MsgBeterm *msg, int len);
static void pgstat_recv_activity(PgStat_MsgActivity *msg, int len);
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

#define TESTBYTEVAL ((char) 199)

	/*
	 * Force start of collector daemon if something to collect
	 */
	if (pgstat_collect_querystring ||
		pgstat_collect_tuplelevel ||
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

		if (recv(pgStatSock, &test_byte, 1, 0) != 1)
		{
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
	 * falls behind (despite the buffering process), statistics messages will
	 * be discarded; backends won't block waiting to send messages to the
	 * collector.
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
	pgstat_collect_querystring = false;
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
 * Format up the arglist for, then fork and exec, statistics
 * (buffer and collector) processes
 */
static pid_t
pgstat_forkexec(STATS_PROCESS_TYPE procType)
{
	char	   *av[10];
	int			ac = 0,
				bufc = 0,
				i;
	char		pgstatBuf[2][32];

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
	Assert(argc == 4);

	argc = 3;
	StrNCpy(postgres_exec_path, argv[argc++], MAXPGPATH);
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
	switch ((pgStatPid = pgstat_forkexec(STAT_PROC_BUFFER)))
#else
	switch ((pgStatPid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork statistics buffer: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

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

	/* can't use pgstat_setheader() because it's not called in a backend */
	MemSet(&(msg.m_hdr), 0, sizeof(msg.m_hdr));
	msg.m_hdr.m_type = PGSTAT_MTYPE_BETERM;
	msg.m_hdr.m_procpid = pid;

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

/* ------------------------------------------------------------
 * Public functions used by backends follow
 *------------------------------------------------------------
 */


/* ----------
 * pgstat_bestart() -
 *
 *	Tell the collector that this new backend is soon ready to process
 *	queries. Called from InitPostgres.
 * ----------
 */
void
pgstat_bestart(void)
{
	PgStat_MsgBestart msg;

	if (pgStatSock < 0)
		return;

	/*
	 * We may not have a MyProcPort (eg, if this is the autovacuum process).
	 * Send an all-zeroes client address, which is dealt with specially in
	 * pg_stat_get_backend_client_addr and pg_stat_get_backend_client_port.
	 */
	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_BESTART);
	msg.m_databaseid = MyDatabaseId;
	msg.m_userid = GetSessionUserId();
	if (MyProcPort)
		memcpy(&msg.m_clientaddr, &MyProcPort->raddr, sizeof(msg.m_clientaddr));
	else
		MemSet(&msg.m_clientaddr, 0, sizeof(msg.m_clientaddr));
	pgstat_send(&msg, sizeof(msg));

	/*
	 * Set up a process-exit hook to ensure we flush the last batch of
	 * statistics to the collector.
	 */
	on_shmem_exit(pgstat_beshutdown_hook, 0);
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
	msg.m_live_tuples = livetuples;
	msg.m_dead_tuples = deadtuples;
	pgstat_send(&msg, sizeof(msg));
}

/*
 * Flush any remaining statistics counts out to the collector at process
 * exit.   Without this, operations triggered during backend exit (such as
 * temp table deletions) won't be counted.
 */
static void
pgstat_beshutdown_hook(int code, Datum arg)
{
	pgstat_report_tabstat();
}


/* ----------
 * pgstat_report_activity() -
 *
 *	Called from tcop/postgres.c to tell the collector what the backend
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
	len = pg_mbcliplen(what, len, PGSTAT_ACTIVITY_SIZE - 1);

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
	List	   *oidlist;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;
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
	oidlist = NIL;
	rel = heap_open(DatabaseRelationId, AccessShareLock);
	scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		oidlist = lappend_oid(oidlist, HeapTupleGetOid(tup));
	}
	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	/*
	 * Search the database hash table for dead databases and tell the
	 * collector to drop them.
	 */
	hash_seq_init(&hstat, pgStatDBHash);
	while ((dbentry = (PgStat_StatDBEntry *) hash_seq_search(&hstat)) != NULL)
	{
		Oid			dbid = dbentry->databaseid;

		if (!list_member_oid(oidlist, dbid))
			pgstat_drop_database(dbid);
	}

	/* Clean up */
	list_free(oidlist);

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
	oidlist = NIL;
	rel = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		oidlist = lappend_oid(oidlist, HeapTupleGetOid(tup));
	}
	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

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
		if (list_member_oid(oidlist, tabentry->tableid))
			continue;

		/*
		 * Not there, so add this table's Oid to the message
		 */
		msg.m_tableid[msg.m_nentries++] = tabentry->tableid;

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
	list_free(oidlist);
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
 * ----------
 */
void
pgstat_drop_relation(Oid relid)
{
	PgStat_MsgTabpurge msg;
	int			len;

	if (pgStatSock < 0)
		return;

	msg.m_tableid[0] = relid;
	msg.m_nentries = 1;

	len = offsetof(PgStat_MsgTabpurge, m_tableid[0]) + sizeof(Oid);

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_TABPURGE);
	msg.m_databaseid = MyDatabaseId;
	pgstat_send(&msg, len);
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
	msg.m_databaseid = MyDatabaseId;
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
	if (!(pgstat_collect_querystring ||
		  pgstat_collect_tuplelevel ||
		  pgstat_collect_blocklevel))
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
	if (!(pgstat_collect_querystring ||
		  pgstat_collect_tuplelevel ||
		  pgstat_collect_blocklevel))
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
pgstat_setheader(PgStat_MsgHdr *hdr, StatMsgType mtype)
{
	hdr->m_type = mtype;
	hdr->m_backendid = MyBackendId;
	hdr->m_procpid = MyProcPid;
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

#ifdef USE_ASSERT_CHECKING
	if (send(pgStatSock, msg, len, 0) < 0)
		elog(LOG, "could not send to statistics collector: %m");
#else
	send(pgStatSock, msg, len, 0);
	/* We deliberately ignore any error from send() */
#endif
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
	pgstat_parseArgs(argc, argv);
#endif

	/*
	 * Start a buffering process to read from the socket, so we have a little
	 * more time to process incoming messages.
	 *
	 * NOTE: the process structure is: postmaster is parent of buffer process
	 * is parent of collector process.	This way, the buffer can detect
	 * collector failure via SIGCHLD, whereas otherwise it wouldn't notice
	 * collector failure until it tried to write on the pipe.  That would mean
	 * that after the postmaster started a new collector, we'd have two buffer
	 * processes competing to read from the UDP socket --- not good.
	 */
	if (pgpipe(pgStatPipe) < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("could not create pipe for statistics buffer: %m")));

	/* child becomes collector process */
#ifdef EXEC_BACKEND
	pgStatCollectorPid = pgstat_forkexec(STAT_PROC_COLLECTOR);
#else
	pgStatCollectorPid = fork();
#endif
	switch (pgStatCollectorPid)
	{
		case -1:
			ereport(ERROR,
					(errmsg("could not fork statistics collector: %m")));

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
	 * Reset signal handling.  With the exception of restoring default SIGCHLD
	 * and SIGQUIT handling, this is a no-op in the non-EXEC_BACKEND case
	 * because we'll have inherited these settings from the buffer process;
	 * but it's not a no-op for EXEC_BACKEND.
	 */
	pqsignal(SIGHUP, SIG_IGN);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
#ifndef WIN32
	pqsignal(SIGQUIT, SIG_IGN);
#else
	/* kluge to allow buffer process to kill collector; FIXME */
	pqsignal(SIGQUIT, pgstat_exit);
#endif
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
	pgstat_parseArgs(argc, argv);
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
	 * Arrange to write the initial status file right away
	 */
	gettimeofday(&next_statwrite, NULL);
	need_statwrite = TRUE;

	/*
	 * Read in an existing statistics stats file or initialize the stats to
	 * zero.
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

	/*
	 * Create the known backends table
	 */
	pgStatBeTable = (PgStat_StatBeEntry *)
		palloc0(sizeof(PgStat_StatBeEntry) * MaxBackends);

	readPipe = pgStatPipe[0];

	/*
	 * Process incoming messages and handle all the reporting stuff until
	 * there are no more messages.
	 */
	for (;;)
	{
		/*
		 * If we need to write the status file again (there have been changes
		 * in the statistics since we wrote it last) calculate the timeout
		 * until we have to do so.
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
		nready = select(readPipe + 1, &rfds, NULL, NULL,
						(need_statwrite) ? &timeout : NULL);
		if (nready < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("select() failed in statistics collector: %m")));
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
			 * process didn't write the message in a single write, which is
			 * possible since it dumps its buffer bytewise. In any case, we'd
			 * need two reads since we don't know the message length
			 * initially.
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
					ereport(ERROR,
							(errcode_for_socket_access(),
							 errmsg("could not read from statistics collector pipe: %m")));
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
						 * sync with the buffer process somehow. Abort so that
						 * we can restart both processes.
						 */
						ereport(ERROR,
							  (errmsg("invalid statistics message length")));
					}
				}
			}

			/*
			 * EOF on the pipe implies that the buffer process exited. Fall
			 * out of outer loop.
			 */
			if (pipeEOF)
				break;

			/*
			 * Distribute the message to the specific function handling it.
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

				case PGSTAT_MTYPE_AUTOVAC_START:
					pgstat_recv_autovac((PgStat_MsgAutovacStart *) &msg, nread);
					break;

				case PGSTAT_MTYPE_VACUUM:
					pgstat_recv_vacuum((PgStat_MsgVacuum *) &msg, nread);
					break;

				case PGSTAT_MTYPE_ANALYZE:
					pgstat_recv_analyze((PgStat_MsgAnalyze *) &msg, nread);
					break;

				default:
					break;
			}

			/*
			 * Globally count messages.
			 */
			pgStatNumMessages++;

			/*
			 * If this is the first message after we wrote the stats file the
			 * last time, setup the timeout that it'd be written.
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
		 * Note that we do NOT check for postmaster exit inside the loop; only
		 * EOF on the buffer pipe causes us to fall out.  This ensures we
		 * don't exit prematurely if there are still a few messages in the
		 * buffer or pipe at postmaster shutdown.
		 */
	}

	/*
	 * Okay, we saw EOF on the buffer pipe, so there are no more messages to
	 * process.  If the buffer process quit because of postmaster shutdown, we
	 * want to save the final stats to reuse at next startup. But if the
	 * buffer process failed, it seems best not to (there may even now be a
	 * new collector firing up, and we don't want it to read a
	 * partially-rewritten stats file).
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
	 * We want to die if our child collector process does.	There are two ways
	 * we might notice that it has died: receive SIGCHLD, or get a write
	 * failure on the pipe leading to the child.  We can set SIGPIPE to kill
	 * us here.  Our SIGCHLD handler was already set up before we forked (must
	 * do it that way, else it's a race condition).
	 */
	pqsignal(SIGPIPE, SIG_DFL);
	PG_SETMASK(&UnBlockSig);

	/*
	 * Set the write pipe to nonblock mode, so that we cannot block when the
	 * collector falls behind.
	 */
	if (!pg_set_noblock(writePipe))
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("could not set statistics collector pipe to nonblocking mode: %m")));

	/*
	 * Allocate the message buffer
	 */
	msgbuffer = (char *) palloc(PGSTAT_RECVBUFFERSZ);

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
		 * Wait for some work to do; but not for more than 10 seconds. (This
		 * determines how quickly we will shut down after an ungraceful
		 * postmaster termination; so it needn't be very fast.)
		 */
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		nready = select(maxfd + 1, &rfds, &wfds, NULL, &timeout);
		if (nready < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("select() failed in statistics buffer: %m")));
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
				ereport(ERROR,
						(errcode_for_socket_access(),
						 errmsg("could not read statistics message: %m")));

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
		 * NOTE: if what we have is less than PIPE_BUF bytes but more than the
		 * space available in the pipe buffer, most kernels will refuse to
		 * write any of it, and will return EAGAIN.  This means we will
		 * busy-loop until the situation changes (either because the collector
		 * caught up, or because more data arrives so that we have more than
		 * PIPE_BUF bytes buffered).  This is not good, but is there any way
		 * around it?  We have no way to tell when the collector has caught
		 * up...
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
				ereport(ERROR,
						(errcode_for_socket_access(),
				errmsg("could not write to statistics collector pipe: %m")));
			}
			/* NB: len < xfr is okay */
			msg_send += len;
			if (msg_send == PGSTAT_RECVBUFFERSZ)
				msg_send = 0;
			msg_have -= len;
		}

		/*
		 * Make sure we forwarded all messages before we check for postmaster
		 * termination.
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
	 * For now, we just nail the doors shut and get out of town.  It might be
	 * cleaner to allow any pending messages to be sent, but that creates a
	 * tradeoff against speed of exit.
	 */

	/*
	 * If running in bufferer, kill our collector as well. On some broken
	 * win32 systems, it does not shut down automatically because of issues
	 * with socket inheritance.  XXX so why not fix the socket inheritance...
	 */
#ifdef WIN32
	if (pgStatCollectorPid > 0)
		kill(pgStatCollectorPid, SIGQUIT);
#endif
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
	PgStat_StatBeEntry *beentry;
	PgStat_StatBeDead *deadbe;

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

	/*
	 * If the slot contains the PID of this backend, everything is fine and we
	 * have nothing to do. Note that all the slots are zero'd out when the
	 * collector is started. We assume that a slot is "empty" iff procpid ==
	 * 0.
	 */
	if (beentry->procpid > 0 && beentry->procpid == msg->m_procpid)
		return 0;

	/*
	 * Lookup if this backend is known to be dead. This can be caused due to
	 * messages arriving in the wrong order - e.g. postmaster's BETERM message
	 * might have arrived before we received all the backends stats messages,
	 * or even a new backend with the same backendid was faster in sending his
	 * BESTART.
	 *
	 * If the backend is known to be dead, we ignore this add.
	 */
	deadbe = (PgStat_StatBeDead *) hash_search(pgStatBeDead,
											   (void *) &(msg->m_procpid),
											   HASH_FIND, NULL);
	if (deadbe)
		return 1;

	/*
	 * Backend isn't known to be dead. If it's slot is currently used, we have
	 * to kick out the old backend.
	 */
	if (beentry->procpid > 0)
		pgstat_sub_backend(beentry->procpid);

	/* Must be able to distinguish between empty and non-empty slots */
	Assert(msg->m_procpid > 0);

	/* Put this new backend into the slot */
	beentry->procpid = msg->m_procpid;
	beentry->start_timestamp = GetCurrentTimestamp();
	beentry->activity_start_timestamp = 0;
	beentry->activity[0] = '\0';

	/*
	 * We can't initialize the rest of the data in this slot until we see the
	 * BESTART message. Therefore, we set the database and user to sentinel
	 * values, to indicate "undefined". There is no easy way to do this for
	 * the client address, so make sure to check that the database or user are
	 * defined before accessing the client address.
	 */
	beentry->userid = InvalidOid;
	beentry->databaseid = InvalidOid;

	return 0;
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
		result->destroy = 0;
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
	 * Search in the known-backends table for the slot containing this PID.
	 */
	for (i = 0; i < MaxBackends; i++)
	{
		if (pgStatBeTable[i].procpid == procpid)
		{
			/*
			 * That's him. Add an entry to the known to be dead backends. Due
			 * to possible misorder in the arrival of UDP packets it's
			 * possible that even if we know the backend is dead, there could
			 * still be messages queued that arrive later. Those messages must
			 * not cause our number of backends statistics to get screwed up,
			 * so we remember for a couple of seconds that this PID is dead
			 * and ignore them (only the counting of backends, not the table
			 * access stats they sent).
			 */
			deadbe = (PgStat_StatBeDead *) hash_search(pgStatBeDead,
													   (void *) &procpid,
													   HASH_ENTER,
													   &found);

			if (!found)
			{
				deadbe->backendid = i + 1;
				deadbe->destroy = PGSTAT_DESTROY_COUNT;
			}

			/*
			 * Declare the backend slot empty.
			 */
			pgStatBeTable[i].procpid = 0;
			return;
		}
	}

	/*
	 * No big problem if not found. This can happen if UDP messages arrive out
	 * of order here.
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
		 * If this database is marked destroyed, count down and do so if it
		 * reaches 0.
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
					ereport(ERROR,
							(errmsg("database hash table corrupted "
									"during cleanup --- abort")));
			}

			/*
			 * Don't include statistics for it.
			 */
			continue;
		}

		/*
		 * Write out the DB entry including the number of live backends.
		 * We don't write the tables pointer since it's of no use to any
		 * other process.
		 */
		fputc('D', fpout);
		fwrite(dbentry, offsetof(PgStat_StatDBEntry, tables), 1, fpout);

		/*
		 * Walk through the database's access stats per table.
		 */
		hash_seq_init(&tstat, dbentry->tables);
		while ((tabentry = (PgStat_StatTabEntry *) hash_seq_search(&tstat)) != NULL)
		{
			/*
			 * If table entry marked for destruction, same as above for the
			 * database entry.
			 */
			if (tabentry->destroy > 0)
			{
				if (--(tabentry->destroy) == 0)
				{
					if (hash_search(dbentry->tables,
									(void *) &(tabentry->tableid),
									HASH_REMOVE, NULL) == NULL)
						ereport(ERROR,
								(errmsg("tables hash table for "
										"database %u corrupted during "
										"cleanup --- abort",
										dbentry->databaseid)));
				}
				continue;
			}

			/*
			 * At least we think this is still a live table.  Emit its access
			 * stats.
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
		PgStat_StatBeEntry *beentry = &pgStatBeTable[i];

		if (beentry->procpid > 0)
		{
			int		len;

			len = offsetof(PgStat_StatBeEntry, activity) +
				strlen(beentry->activity) + 1;
			fputc('B', fpout);
			fwrite(&len, sizeof(len), 1, fpout);
			fwrite(beentry, len, 1, fpout);
		}
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

	/*
	 * Clear out the dead backends table
	 */
	hash_seq_init(&hstat, pgStatBeDead);
	while ((deadbe = (PgStat_StatBeDead *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * Count down the destroy delay and remove entries where it reaches 0.
		 */
		if (--(deadbe->destroy) <= 0)
		{
			if (hash_search(pgStatBeDead,
							(void *) &(deadbe->procpid),
							HASH_REMOVE, NULL) == NULL)
				ereport(ERROR,
						(errmsg("dead-server-process hash table corrupted "
								"during cleanup --- abort")));
		}
	}
}


/* ----------
 * pgstat_read_statsfile() -
 *
 *	Reads in an existing statistics collector and initializes the
 *	databases' hash table (whose entries point to the tables' hash tables)
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
	PgStat_StatBeEntry *beentry;
	HASHCTL		hash_ctl;
	HTAB	   *tabhash = NULL;
	FILE	   *fpin;
	int32		format_id;
	int			len;
	int			maxbackends = 0;
	int			havebackends = 0;
	bool		found;
	bool		check_pids;
	MemoryContext use_mcxt;
	int			mcxt_flags;

	/*
	 * If running in the collector or the autovacuum process, we use the
	 * DynaHashCxt memory context.	If running in a backend, we use the
	 * TopTransactionContext instead, so the caller must only know the last
	 * XactId when this call happened to know if his tables are still valid or
	 * already gone!
	 *
	 * Also, if running in a regular backend, we check backend entries against
	 * the PGPROC array so that we can detect stale entries.  This lets us
	 * discard entries whose BETERM message got lost for some reason.
	 */
	if (pgStatRunningInCollector || IsAutoVacuumProcess())
	{
		use_mcxt = NULL;
		mcxt_flags = 0;
		check_pids = false;
	}
	else
	{
		use_mcxt = TopTransactionContext;
		mcxt_flags = HASH_CONTEXT;
		check_pids = true;
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
	 * Initialize the number of known backends to zero, just in case we do a
	 * silent error return below.
	 */
	if (numbackends != NULL)
		*numbackends = 0;
	if (betab != NULL)
		*betab = NULL;

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
				dbentry->destroy = 0;
				dbentry->n_backends = 0;

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
				 * 'M'	The maximum number of backends to expect follows.
				 */
			case 'M':
				if (betab == NULL || numbackends == NULL)
					goto done;
				if (fread(&maxbackends, 1, sizeof(maxbackends), fpin) !=
					sizeof(maxbackends))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}
				if (maxbackends == 0)
					goto done;

				/*
				 * Allocate space (in TopTransactionContext too) for the
				 * backend table.
				 */
				if (use_mcxt == NULL)
					*betab = (PgStat_StatBeEntry *)
						palloc(sizeof(PgStat_StatBeEntry) * maxbackends);
				else
					*betab = (PgStat_StatBeEntry *)
						MemoryContextAlloc(use_mcxt,
								   sizeof(PgStat_StatBeEntry) * maxbackends);
				break;

				/*
				 * 'B'	A PgStat_StatBeEntry follows.
				 */
			case 'B':
				if (betab == NULL || numbackends == NULL || *betab == NULL)
					goto done;

				if (havebackends >= maxbackends)
					goto done;

				/* Read and validate the entry length */
				if (fread(&len, 1, sizeof(len), fpin) != sizeof(len))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}
				if (len <= offsetof(PgStat_StatBeEntry, activity) ||
					len > sizeof(PgStat_StatBeEntry))
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}

				/*
				 * Read it directly into the table.
				 */
				beentry = &(*betab)[havebackends];

				if (fread(beentry, 1, len, fpin) != len)
				{
					ereport(pgStatRunningInCollector ? LOG : WARNING,
							(errmsg("corrupted pgstat.stat file")));
					goto done;
				}

				/*
				 * If possible, check PID to verify still running
				 */
				if (check_pids && !IsBackendPid(beentry->procpid))
				{
					/*
					 * Note: we could send a BETERM message to tell the
					 * collector to drop the entry, but I'm a bit worried
					 * about race conditions.  For now, just silently ignore
					 * dead entries; they'll get recycled eventually anyway.
					 */

					/* Don't accept the entry */
					memset(beentry, 0, sizeof(PgStat_StatBeEntry));
					break;
				}

				/*
				 * Count backends per database here.
				 */
				dbentry = (PgStat_StatDBEntry *)
					hash_search(*dbhash,
								&(beentry->databaseid),
								HASH_FIND,
								NULL);
				if (dbentry)
					dbentry->n_backends++;

				havebackends++;
				*numbackends = havebackends;

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
 * Because we store the hash tables in TopTransactionContext, the result
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
		pgstat_read_statsfile(&pgStatDBHash, InvalidOid,
							  &pgStatBeTable, &pgStatNumBackends);
	}
	else
	{
		TransactionId topXid = GetTopTransactionId();

		if (!TransactionIdEquals(pgStatDBHashXact, topXid))
		{
			Assert(!pgStatRunningInCollector);
			pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId,
								  &pgStatBeTable, &pgStatNumBackends);
			pgStatDBHashXact = topXid;
		}
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
	PgStat_StatBeEntry *entry;

	/*
	 * If the backend is known dead, we ignore the message -- we don't want to
	 * update the backend entry's state since this BESTART message refers to
	 * an old, dead backend
	 */
	if (pgstat_add_backend(&msg->m_hdr) != 0)
		return;

	entry = &(pgStatBeTable[msg->m_hdr.m_backendid - 1]);
	entry->userid = msg->m_userid;
	memcpy(&entry->clientaddr, &msg->m_clientaddr, sizeof(entry->clientaddr));
	entry->databaseid = msg->m_databaseid;
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

	tabentry->n_live_tuples = msg->m_tuples;
	tabentry->n_dead_tuples = 0;
	if (msg->m_analyze)
		tabentry->last_anl_tuples = msg->m_tuples;
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

	tabentry->n_live_tuples = msg->m_live_tuples;
	tabentry->n_dead_tuples = msg->m_dead_tuples;
	tabentry->last_anl_tuples = msg->m_live_tuples + msg->m_dead_tuples;
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
	 * Here we check explicitly for 0 return, since we don't want to mangle
	 * the activity of an active backend by a delayed packet from a dead one.
	 */
	if (pgstat_add_backend(&msg->m_hdr) != 0)
		return;

	entry = &(pgStatBeTable[msg->m_hdr.m_backendid - 1]);

	StrNCpy(entry->activity, msg->m_what, PGSTAT_ACTIVITY_SIZE);

	entry->activity_start_timestamp = GetCurrentTimestamp();
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

	dbentry = pgstat_get_db_entry(msg->m_databaseid, true);

	/*
	 * If the database is marked for destroy, this is a delayed UDP packet and
	 * not worth being counted.
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

			tabentry->n_live_tuples += tabmsg[i].t_tuples_inserted;
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
	PgStat_StatTabEntry *tabentry;
	int			i;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);

	/*
	 * No need to purge if we don't even know the database.
	 */
	if (!dbentry || !dbentry->tables)
		return;

	/*
	 * If the database is marked for destroy, this is a delayed UDP packet and
	 * the tables will go away at DB destruction.
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
	dbentry = pgstat_get_db_entry(msg->m_databaseid, false);

	/*
	 * Mark the database for destruction.
	 */
	if (dbentry)
		dbentry->destroy = PGSTAT_DESTROY_COUNT;
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
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

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
	dbentry->destroy = 0;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(PgStat_StatTabEntry);
	hash_ctl.hash = oid_hash;
	dbentry->tables = hash_create("Per-database table",
								  PGSTAT_TAB_HASH_SIZE,
								  &hash_ctl,
								  HASH_ELEM | HASH_FUNCTION);
}
