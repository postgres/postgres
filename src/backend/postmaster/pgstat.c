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
 *			  entire thing can be enabled/disabled on a per db base.
 *			  Not to be done before 7.2 - requires catalog change and
 *			  thus an initdb and we might want to provide this as a
 *			  patch for 7.1.
 *
 *			- Make the functions from contrib/pgstat_tmp builtin
 *			  and create the views on initdb.
 *
 *	Copyright (c) 2001, PostgreSQL Global Development Group
 *
 *	$Id: pgstat.c,v 1.2 2001/06/29 16:29:37 wieck Exp $
 * ----------
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <signal.h>

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"
#include "storage/backendid.h"
#include "utils/rel.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"
#include "access/xact.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_database.h"

#include "pgstat.h"


/* ----------
 * Global data
 * ----------
 */


/* ----------
 * Local data
 * ----------
 */
static int					pgStatSock = -1;
static int					pgStatPipe[2];
static struct sockaddr_in	pgStatAddr;
static int					pgStatPmPipe[2];

static int					pgStatRunning = 0;
static int					pgStatPid;

static long					pgStatNumMessages = 0;

static bool					pgStatRunningInCollector = FALSE;
static int					pgStatTabstatAlloc = 0;
static int					pgStatTabstatUsed = 0;
static PgStat_MsgTabstat  **pgStatTabstatMessages = NULL;
static int					pgStatXactCommit = 0;
static int					pgStatXactRollback = 0;


static TransactionId		pgStatDBHashXact = InvalidTransactionId;
static HTAB				   *pgStatDBHash = NULL;
static HTAB				   *pgStatBeDead = NULL;
static PgStat_StatBeEntry  *pgStatBeTable = NULL;
static int					pgStatNumBackends = 0;

static char					pgStat_tmpfname[PATH_MAX];
static char					pgStat_fname[PATH_MAX];


/* ----------
 * Local function forward declarations
 * ----------
 */
static void		pgstat_main(void);
static void		pgstat_recvbuffer(void);

static int		pgstat_add_backend(PgStat_MsgHdr *msg);
static void		pgstat_sub_backend(int procpid);
static void		pgstat_drop_database(Oid databaseid);
static void		pgstat_write_statsfile(void);
static void		pgstat_read_statsfile(HTAB **dbhash, Oid onlydb, 
									PgStat_StatBeEntry **betab, 
									int *numbackends);

static void		pgstat_setheader(PgStat_MsgHdr *hdr, int mtype);
static void		pgstat_send(void *msg, int len);

static void		pgstat_recv_bestart(PgStat_MsgBestart *msg, int len);
static void		pgstat_recv_beterm(PgStat_MsgBeterm *msg, int len);
static void		pgstat_recv_activity(PgStat_MsgActivity *msg, int len);
static void		pgstat_recv_tabstat(PgStat_MsgTabstat *msg, int len);
static void		pgstat_recv_tabpurge(PgStat_MsgTabpurge *msg, int len);
static void		pgstat_recv_dropdb(PgStat_MsgDropdb *msg, int len);
static void		pgstat_recv_resetcounter(PgStat_MsgResetcounter *msg, int len);


/* ------------------------------------------------------------
 * Public functions called from postmaster follow
 * ------------------------------------------------------------
 */


/* ----------
 * pgstat_init() -
 *
 *	Called from postmaster at startup. Create the resources required
 *	by the statistics collector process.
 * ----------
 */
int
pgstat_init(void)
{
	int			alen;

	/*
	 * Initialize the filenames for the status reports.
	 */
	snprintf(pgStat_tmpfname,  PATH_MAX - 1, 
				PGSTAT_STAT_TMPFILE,  DataDir, getpid());
	snprintf(pgStat_fname, PATH_MAX - 1, 
				PGSTAT_STAT_FILENAME, DataDir);

	/*
	 * Create the UDP socket for receiving statistic messages
	 */
	if ((pgStatSock = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
	{
		perror("PGSTAT: socket(2)");
		return -1;
	}

	/*
	 * Bind it to a kernel assigned port on localhost and
	 * get the assigned port via getsockname().
	 */
	pgStatAddr.sin_family	= AF_INET;
	pgStatAddr.sin_port	= htons(0);
	inet_aton("127.0.0.1", &(pgStatAddr.sin_addr));
	alen = sizeof(pgStatAddr);
	if (bind(pgStatSock, (struct sockaddr *)&pgStatAddr, alen) < 0)
	{
		perror("PGSTAT: bind(2)");
		close(pgStatSock);
		pgStatSock = -1;
		return -1;
	}
	if (getsockname(pgStatSock, (struct sockaddr *)&pgStatAddr, &alen) < 0)
	{
		perror("PGSTAT: getsockname(2)");
		close(pgStatSock);
		pgStatSock = -1;
		return -1;
	}

	/*
	 * Set the socket to non-blocking IO
	 */
	if (fcntl(pgStatSock, F_SETFL, O_NONBLOCK) < 0)
	{
		perror("PGSTAT: fcntl(2)");
		close(pgStatSock);
		pgStatSock = -1;
		return -1;
	}

	/*
	 * Create the pipe that controls the statistics collector shutdown
	 */
	if (pipe(pgStatPmPipe) < 0)
	{
		perror("PGSTAT: pipe(2)");
		close(pgStatSock);
		pgStatSock = -1;
		return -1;
	}

	return 0;
}


/* ----------
 * pgstat_start() -
 *
 *	Called from postmaster at startup or after an existing collector
 *	died. Fire up a fresh statistics collector.
 * ----------
 */
int
pgstat_start(void)
{
	/*
	 * Check that the socket at least is there
	 */
	if (pgStatSock < 0)
	{
		fprintf(stderr, 
			"PGSTAT: suppress collector startup due to missing socket\n");
		return 0;
	}

	/*
	 * Then fork off the collector.
	 */
	switch(pgStatPid = (int)fork())
	{
		case -1:
			perror("PGSTAT: fork(2)");
			pgStatRunning = 0;
			return -1;

		case 0:
			break;

		default:
			pgStatRunning = 1;
			return 0;
	}

	pgstat_main();
	exit(0);
}


/* ----------
 * pgstat_ispgstat() -
 *
 *	Called from postmaster to check if a terminated child process
 *	was the statistics collector.
 * ----------
 */
int
pgstat_ispgstat(int pid)
{
	if (pgStatRunning == 0)
		return 0;

	if (pgStatPid != pid)
		return 0;

	return 1;
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
	PgStat_MsgBeterm		msg;

	msg.m_hdr.m_type		= PGSTAT_MTYPE_BETERM;
	msg.m_hdr.m_backendid	= 0;
	msg.m_hdr.m_procpid		= pid;
	msg.m_hdr.m_databaseid	= 0;

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
	PgStat_MsgBestart		msg;

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
pgstat_report_activity(char *what)
{
	PgStat_MsgActivity	msg;
	int						len;

	if (pgStatSock < 0)
		return;

	len = strlen(what);
	if (len >= PGSTAT_ACTIVITY_SIZE)
		len = PGSTAT_ACTIVITY_SIZE - 1;
	
	memcpy(msg.m_what, what, len);
	msg.m_what[len] = '\0';
	len += offsetof(PgStat_MsgActivity, m_what) + 1;

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
	int				i;
	int				n;
	int				len;

	if (pgStatSock < 0)
		return;

	/*
	 * For each message buffer used during the last query
	 * set the header fields and send it out.
	 */
	for (i = 0; i < pgStatTabstatUsed; i++)
	{
		n = pgStatTabstatMessages[i]->m_nentries;
		len = offsetof(PgStat_MsgTabstat, m_entry[n]);

		pgStatTabstatMessages[i]->m_xact_commit   = pgStatXactCommit;
		pgStatTabstatMessages[i]->m_xact_rollback = pgStatXactRollback;
		pgStatXactCommit   = 0;
		pgStatXactRollback = 0;

		pgstat_setheader(&pgStatTabstatMessages[i]->m_hdr, 
					PGSTAT_MTYPE_TABSTAT);
		pgstat_send(pgStatTabstatMessages[i], len);
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
	Relation				dbrel;
	HeapScanDesc			dbscan;
	HeapTuple				dbtup;
	Oid					   *dbidlist;
	int						dbidalloc;
	int						dbidused;
	HASH_SEQ_STATUS			hstat;
	long				   *hentry;
	PgStat_StatDBEntry	   *dbentry;
	PgStat_StatTabEntry	   *tabentry;
	HeapTuple				reltup;
	bool					found;
	int						nobjects = 0;
	PgStat_MsgTabpurge		msg;
	int						len;
	int						i;

	if (pgStatSock < 0)
		return 0;

	/*
	 * We don't vacuum inside of transaction blocks, because 
	 * a possible later rollback might reactivate objects we
	 * didn't find because of earlier destruction in the xact.
	 */
	if (IsTransactionBlock())
		return 0;

	/*
	 * If not done for this transaction, read the statistics collector
	 * stats file into some hash tables.
	 */
	if (pgStatDBHashXact != GetCurrentTransactionId())
	{
		pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId, 
						&pgStatBeTable, &pgStatNumBackends);
		pgStatDBHashXact = GetCurrentTransactionId();
	}

	/*
	 * Lookup our own database entry
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
					(char *)&MyDatabaseId, HASH_FIND, &found);
	if (!found || dbentry == NULL)
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
	while((hentry = hash_seq_search(&hstat)) != (long *)TRUE)
	{
		if (hentry == NULL)
			return -1;

		/*
		 * Check if this relation is still alive by
		 * looking up it's pg_class tuple in the 
		 * system catalog cache.
		 */
		tabentry = (PgStat_StatTabEntry *)hentry;
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
			len = offsetof(PgStat_MsgTabpurge, m_tableid[msg.m_nentries]);

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
		len = offsetof(PgStat_MsgTabpurge, m_tableid[msg.m_nentries]);

		pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_TABPURGE);
		pgstat_send(&msg, len);
	}

	/*
	 * Read pg_database and remember the Oid's of all existing databases
	 */
	dbidalloc = 256;
	dbidused = 0;
	dbidlist = (Oid *)palloc(sizeof(Oid) * dbidalloc);

	dbrel = heap_openr(DatabaseRelationName, AccessShareLock);
	dbscan = heap_beginscan(dbrel, 0, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(dbtup = heap_getnext(dbscan, FALSE)))
	{
		if (dbidused >= dbidalloc)
		{
			dbidalloc *= 2;
			dbidlist = (Oid *)repalloc((char *)dbidlist,
							sizeof(Oid) * dbidalloc);
		}
		dbidlist[dbidused++] = dbtup->t_data->t_oid;
	}
	heap_endscan(dbscan);
	heap_close(dbrel, AccessShareLock);

	/*
	 * Search the database hash table for dead databases and
	 * tell the collector to drop them as well.
	 */
	hash_seq_init(&hstat, pgStatDBHash);
	while((hentry = hash_seq_search(&hstat)) != (long *)TRUE)
	{
		Oid		dbid;

		if (hentry == NULL)
			break;
		
		dbentry = (PgStat_StatDBEntry *)hentry;
		dbid = dbentry->databaseid;

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
	pfree((char *)dbidlist);

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
	PgStat_MsgDropdb		msg;

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
	PgStat_MsgResetcounter		msg;

	if (pgStatSock < 0)
		return;

	if (!superuser())
		elog(ERROR, "Only database superusers can reset statistic counters");

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
	PgStat_MsgDummy		msg;

	if (pgStatSock < 0)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_DUMMY);
	pgstat_send(&msg, sizeof(msg));
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
	PgStat_TableEntry	   *useent = NULL;
	Oid						rel_id = rel->rd_id;
	int						mb;
	int						i;

	/*
	 * Initialize data not to count at all.
	 */
	stats->tabentry				= NULL;
	stats->no_stats				= FALSE;
	stats->heap_scan_counted	= FALSE;
	stats->index_scan_counted	= FALSE;

	if (pgStatSock < 0)
	{
		stats->no_stats = TRUE;
		return;
	}

	/*
	 * On the first of all calls initialize the message buffers.
	 */
	if (pgStatTabstatAlloc == 0)
	{
		pgStatTabstatAlloc = 4;
		pgStatTabstatMessages = (PgStat_MsgTabstat **)
					malloc(sizeof(PgStat_MsgTabstat *) * pgStatTabstatAlloc);
		if (pgStatTabstatMessages == NULL)
		{
			perror("PGSTATBE: malloc(2)");
			return;
		}
		for (i = 0; i < pgStatTabstatAlloc; i++)
		{
			pgStatTabstatMessages[i] = (PgStat_MsgTabstat *)
					malloc(sizeof(PgStat_MsgTabstat));
			if (pgStatTabstatMessages[i] == NULL)
			{
				perror("PGSTATBE: malloc(2)");
				return;
			}
		}
	}

	/*
	 * Lookup the so far used table slots for this relation.
	 */
	for (mb = 0; mb < pgStatTabstatUsed; mb++)
	{
		for (i = 0; i < pgStatTabstatMessages[mb]->m_nentries; i++)
		{
			if (pgStatTabstatMessages[mb]->m_entry[i].t_id == rel_id)
			{
				stats->tabentry = (void *)&(pgStatTabstatMessages[mb]->m_entry[i]);
				return;
			}
		}

		if (pgStatTabstatMessages[mb]->m_nentries >= PGSTAT_NUM_TABENTRIES)
			continue;
		
		/*
		 * Not found, but found a message buffer with an empty slot
		 * instead. Fine, let's use this one.
		 */
		i = pgStatTabstatMessages[mb]->m_nentries++;
		useent = &pgStatTabstatMessages[mb]->m_entry[i];
		memset(useent, 0, sizeof(PgStat_TableEntry));
		useent->t_id = rel_id;
		stats->tabentry = (void *)useent;
		return;
	}

	/*
	 * If we ran out of message buffers, we just allocate more.
	 */
	if (pgStatTabstatUsed >= pgStatTabstatAlloc)
	{
		pgStatTabstatAlloc += 4;
		pgStatTabstatMessages = (PgStat_MsgTabstat **)
					realloc(pgStatTabstatMessages,
					sizeof(PgStat_MsgTabstat *) * pgStatTabstatAlloc);
		if (pgStatTabstatMessages == NULL)
		{
			pgStatTabstatAlloc -= 4;
			perror("PGSTATBE: malloc(2)");
			return;
		}
		for (i = pgStatTabstatUsed; i < pgStatTabstatAlloc; i++)
		{
			pgStatTabstatMessages[i] = (PgStat_MsgTabstat *)
					malloc(sizeof(PgStat_MsgTabstat));
			if (pgStatTabstatMessages[i] == NULL)
			{
				pgStatTabstatAlloc -= 4;
				perror("PGSTATBE: malloc(2)");
				return;
			}
		}
	}

	/*
	 * Use the first entry of the next message buffer.
	 */
	mb = pgStatTabstatUsed++;
	pgStatTabstatMessages[mb]->m_nentries = 1;
	useent = &pgStatTabstatMessages[mb]->m_entry[0];
	memset(useent, 0, sizeof(PgStat_TableEntry));
	useent->t_id = rel_id;
	stats->tabentry = (void *)useent;
	return;
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
	pgStatXactCommit++;

	/*
	 * If there was no relation activity yet, just make
	 * one existing message buffer used without slots,
	 * causing the next report to tell new xact-counters.
	 */
	if (pgStatTabstatAlloc > 0)
	{
		if (pgStatTabstatUsed == 0)
		{
			pgStatTabstatUsed++;
			pgStatTabstatMessages[0]->m_nentries = 0;
		}
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
	pgStatXactRollback++;

	/*
	 * If there was no relation activity yet, just make
	 * one existing message buffer used without slots,
	 * causing the next report to tell new xact-counters.
	 */
	if (pgStatTabstatAlloc > 0)
	{
		if (pgStatTabstatUsed == 0)
		{
			pgStatTabstatUsed++;
			pgStatTabstatMessages[0]->m_nentries = 0;
		}
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
	PgStat_StatDBEntry	   *dbentry;
	bool					found;

	/*
	 * If not done for this transaction, read the statistics collector
	 * stats file into some hash tables. Be careful with the read_statsfile()
	 * call below!
	 */
	if (pgStatDBHashXact != GetCurrentTransactionId())
	{
		pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId, 
						&pgStatBeTable, &pgStatNumBackends);
		pgStatDBHashXact = GetCurrentTransactionId();
	}

	/*
	 * Lookup the requested database
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
					(char *)&dbid, HASH_FIND, &found);
	if (!found || dbentry == NULL)
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
	PgStat_StatDBEntry	   *dbentry;
	PgStat_StatTabEntry	   *tabentry;
	bool					found;

	/*
	 * If not done for this transaction, read the statistics collector
	 * stats file into some hash tables. Be careful with the read_statsfile()
	 * call below!
	 */
	if (pgStatDBHashXact != GetCurrentTransactionId())
	{
		pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId, 
						&pgStatBeTable, &pgStatNumBackends);
		pgStatDBHashXact = GetCurrentTransactionId();
	}

	/*
	 * Lookup our database.
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
					(char *)&MyDatabaseId, HASH_FIND, &found);
	if (!found || dbentry == NULL)
		return NULL;

	/*
	 * Now inside the DB's table hash table lookup the requested one.
	 */
	if (dbentry->tables == NULL)
		return NULL;
	tabentry = (PgStat_StatTabEntry *)hash_search(dbentry->tables,
					(char *)&relid, HASH_FIND, &found);
	if (!found || tabentry == NULL)
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
	if (pgStatDBHashXact != GetCurrentTransactionId())
	{
		pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId, 
						&pgStatBeTable, &pgStatNumBackends);
		pgStatDBHashXact = GetCurrentTransactionId();
	}

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
	if (pgStatDBHashXact != GetCurrentTransactionId())
	{
		pgstat_read_statsfile(&pgStatDBHash, MyDatabaseId, 
						&pgStatBeTable, &pgStatNumBackends);
		pgStatDBHashXact = GetCurrentTransactionId();
	}

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
	hdr->m_type			= mtype;
	hdr->m_backendid	= MyBackendId;
	hdr->m_procpid		= MyProcPid;
	hdr->m_databaseid	= MyDatabaseId;
	hdr->m_userid		= GetSessionUserId();
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

	((PgStat_MsgHdr *)msg)->m_size = len;

	sendto(pgStatSock, msg, len, 0, 
			(struct sockaddr *)&pgStatAddr, sizeof(pgStatAddr));
}


/* ------------------------------------------------------------
 * Local functions implementing the statistics collector itself follow
 *------------------------------------------------------------ 
 */


/* ----------
 * pgstat_main() -
 *
 *	The statistics collector itself.
 * ----------
 */
static void
pgstat_main(void)
{
	PgStat_Msg	msg;
	fd_set			rfds;
	int				maxfd;
	int				nready;
	int				len;
	int				dlen;
	struct timeval	timeout;
	struct timeval	next_statwrite;
	bool			need_statwrite;
	HASHCTL			hash_ctl;

	/*
	 * Close the writing end of the postmaster pipe, so we'll see
	 * it closing when the postmaster terminates and can terminate
	 * as well.
	 */
	close(pgStatPmPipe[1]);

	/*
	 * Ignore all signals usually bound to some action in the postmaster
	 */
	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGALRM, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGWINCH, SIG_IGN);

	/*
	 * Write the initial status file right at startup
	 */
	gettimeofday(&next_statwrite, NULL);
	need_statwrite = TRUE;

	/*
	 * Now we start the buffer process to read from the socket, so
	 * we have a little more time to process incoming messages.
	 */
	if (pipe(pgStatPipe) < 0)
	{
		perror("PGSTAT: pipe(2)");
		exit(1);
	}

	switch(fork())
	{
		case -1:	perror("PGSTAT: fork(2)");
					exit(1);

		case 0:		close(pgStatPipe[0]);
					signal(SIGPIPE, SIG_DFL);
					pgstat_recvbuffer();
					exit(2);

		default:	close(pgStatPipe[1]);
					close(pgStatSock);
					break;
	}

	/*
	 * Read in an existing statistics stats file or initialize the
	 * stats to zero.
	 */
	pgStatRunningInCollector = TRUE;
	pgstat_read_statsfile(&pgStatDBHash, InvalidOid, NULL, NULL);

	/*
	 * Create the dead backend hashtable
	 */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize  = sizeof(int);
	hash_ctl.datasize = sizeof(PgStat_StatBeDead);
	hash_ctl.hash     = tag_hash;
	pgStatBeDead = hash_create(PGSTAT_BE_HASH_SIZE, &hash_ctl, 
							HASH_ELEM | HASH_FUNCTION);
	if (pgStatBeDead == NULL)
	{
		fprintf(stderr, 
				"PGSTAT: Creation of dead backend hash table failed\n");
		exit(1);
	}

	/*
	 * Create the known backends table
	 */
	pgStatBeTable = (PgStat_StatBeEntry *)malloc(
					sizeof(PgStat_StatBeEntry) * MaxBackends);
	if (pgStatBeTable == NULL)
	{
		perror("PGSTAT: Allocation of backend table failed");
		exit(1);
	}
	memset(pgStatBeTable, 0, sizeof(PgStat_StatBeEntry) * MaxBackends);

	/*
	 * Process incoming messages and handle all the reporting stuff
	 * until the postmaster waves us good bye.
	 */
	for (;;)
	{
		/*
		 * If we need to write the status file again (there have
		 * been changes in the statistics since we wrote it last)
		 * calculate the timeout until we have to do so.
		 */
		if (need_statwrite)
		{
			gettimeofday(&timeout, NULL);
			timeout.tv_usec = next_statwrite.tv_usec - timeout.tv_usec;
			timeout.tv_sec  = next_statwrite.tv_sec  - timeout.tv_sec;
			if (timeout.tv_usec < 0)
			{
				timeout.tv_sec  -= 1;
				timeout.tv_usec += 1000000;
			}
			if (timeout.tv_sec < 0)
			{
				timeout.tv_sec  = 0;
				timeout.tv_usec = 0;
			}
		}

		/*
		 * Setup the descriptor set for select(2)
		 */
		FD_ZERO(&rfds);
		FD_SET(pgStatPipe[0], &rfds);
		FD_SET(pgStatPmPipe[0], &rfds);

		if (pgStatPipe[0] > pgStatPmPipe[0])
			maxfd = pgStatPipe[0];
		else
			maxfd = pgStatPmPipe[0];

		/*
		 * Now wait for something to do.
		 */
		nready = select(maxfd + 1, &rfds, NULL, NULL, 
						(need_statwrite) ? &timeout : NULL);
		if (nready < 0)
		{
			perror("PGSTAT: select(2)");
			exit(1);
		}

		/*
		 * If there are no descriptors ready, our timeout for writing
		 * the stats file happened.
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
		if (FD_ISSET(pgStatPipe[0], &rfds))
		{
			/*
			 * If this is the first message after we wrote the stats
			 * file the last time, setup the timeout that it'd be
			 * written.
			 */
			if (!need_statwrite)
			{
				gettimeofday(&next_statwrite, NULL);
				next_statwrite.tv_usec += ((PGSTAT_STAT_INTERVAL) * 1000);
				next_statwrite.tv_sec  += (next_statwrite.tv_usec / 1000000);
				next_statwrite.tv_usec %= 1000000;
				need_statwrite = TRUE;
			}

			/*
			 * Read the header.
			 */
			len = read(pgStatPipe[0], &msg, sizeof(PgStat_MsgHdr));
			if (len < 0)
			{
				perror("PGSTAT: read(2)");
				exit(1);
			}
			if (len == 0)
			{
				return;
			}
			if (len != sizeof(PgStat_MsgHdr))
			{
				fprintf(stderr, "PGSTAT: short read(2)");
				exit(1);
			}

			/*
			 * And the body. We need to do it in two steps because
			 * we don't know the length.
			 */
			dlen = msg.msg_hdr.m_size - sizeof(PgStat_MsgHdr);
			if (dlen > 0)
			{
				len = read(pgStatPipe[0], 
						((char *)&msg) + sizeof(PgStat_MsgHdr), dlen);
				if (len < 0)
				{
					perror("PGSTAT: read(2)");
					exit(1);
				}
				if (len == 0)
				{
					return;
				}
				if (len != dlen)
				{
					fprintf(stderr, "PGSTAT: short read(2)");
					exit(1);
				}
			}

			/*
			 * Distribute the message to the specific function
			 * handling it.
			 */
			len += sizeof(PgStat_MsgHdr);
			switch (msg.msg_hdr.m_type)
			{
				case PGSTAT_MTYPE_DUMMY:
					break;

				case PGSTAT_MTYPE_BESTART:
					pgstat_recv_bestart((PgStat_MsgBestart *)&msg, len);
					break;

				case PGSTAT_MTYPE_BETERM:
					pgstat_recv_beterm((PgStat_MsgBeterm *)&msg, len);
					break;

				case PGSTAT_MTYPE_TABSTAT:
					pgstat_recv_tabstat((PgStat_MsgTabstat *)&msg, len);
					break;

				case PGSTAT_MTYPE_TABPURGE:
					pgstat_recv_tabpurge((PgStat_MsgTabpurge *)&msg, len);
					break;

				case PGSTAT_MTYPE_ACTIVITY:
					pgstat_recv_activity((PgStat_MsgActivity *)&msg, len);
					break;

				case PGSTAT_MTYPE_DROPDB:
					pgstat_recv_dropdb((PgStat_MsgDropdb *)&msg, len);
					break;

				case PGSTAT_MTYPE_RESETCOUNTER:
					pgstat_recv_resetcounter((PgStat_MsgResetcounter *)&msg,
												len);
					break;

				default:
					break;
			}

			/*
			 * Globally count messages and start over.
			 */
			pgStatNumMessages++;
			continue;
		}

		/*
		 * If the postmaster pipe is ready for reading this means that
		 * the kernel must have closed it because of the termination
		 * of the postmaster (he never really writes to it). Give up
		 * then, but save the final stats in case we want to reuse
		 * them at startup in the future.
		 */
		if (FD_ISSET(pgStatPmPipe[0], &rfds))
		{
			pgstat_write_statsfile();
			return;
		}
	}
}


/* ----------
 * pgstat_recvbuffer() -
 *
 *	This is a special receive buffer started by the statistics
 *	collector itself and running in a separate process. It's only
 *	purpose is to receive messages from the UDP socket as fast as
 *	possible and forward them over a pipe into the collector
 *	itself.
 * ----------
 */
static void
pgstat_recvbuffer(void)
{
	fd_set				rfds;
	fd_set				wfds;
	int					maxfd;
	int					nready;
	int					len;
	PgStat_Msg		   *msgbuffer = NULL;
	int					msg_recv = 0;
	int					msg_send = 0;
	int					msg_have = 0;
	struct sockaddr_in	fromaddr;
	int					fromlen;
	int					overflow = 0;

	/*
	 * Allocate the message buffer
	 */
	msgbuffer = (PgStat_Msg *)malloc(sizeof(PgStat_Msg) *
				PGSTAT_RECVBUFFERSZ);
	if (msgbuffer == NULL)
	{
		perror("PGSTATBUFF: malloc()");
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
		 * As long as we have buffer space we add the socket
		 * to the read descriptor set.
		 */
		if (msg_have < PGSTAT_RECVBUFFERSZ)
		{
			FD_SET(pgStatSock, &rfds);
			maxfd = pgStatSock;
			overflow = 0;
		}
		else
		{
			if (overflow == 0)
			{
				fprintf(stderr, "PGSTAT: Warning - receive buffer full\n");
				overflow = 1;
			}
		}


		/*
		 * If we have messages to write out, we add the pipe
		 * to the write descriptor set. Otherwise, we check if
		 * the postmaster might have terminated.
		 */
		if (msg_have > 0)
		{
			FD_SET(pgStatPipe[1], &wfds);
			if (pgStatPipe[1] > maxfd)
				maxfd = pgStatPipe[1];
		}
		else
		{
			FD_SET(pgStatPmPipe[0], &rfds);
			if (pgStatPmPipe[0] > maxfd)
				maxfd = pgStatPmPipe[0];
		}


		/*
		 * Wait for some work to do.
		 */
		nready = select(maxfd + 1, &rfds, &wfds, NULL, NULL);
		if (nready < 0)
		{
			perror("PGSTATBUFF: select(2)");
			exit(1);
		}

		/*
		 * If there is a message on the socket, read it and check for
		 * validity.
		 */
		if (FD_ISSET(pgStatSock, &rfds))
		{
			fromlen = sizeof(fromaddr);
			len = recvfrom(pgStatSock, 
						&msgbuffer[msg_recv], sizeof(PgStat_Msg), 0,
						(struct sockaddr *)&fromaddr, &fromlen);
			if (len < 0)
			{
				perror("PGSTATBUFF: recvfrom(2)");
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
			if (msgbuffer[msg_recv].msg_hdr.m_size != len)
				continue;

			/*
			 * The source address of the packet must be our own socket.
			 * This ensures that only real hackers or our own backends
			 * tell us something.
			 */
			if (fromaddr.sin_addr.s_addr != pgStatAddr.sin_addr.s_addr)
				continue;
			if (fromaddr.sin_port != pgStatAddr.sin_port)
				continue;
			
			/*
			 * O.K. - we accept this message.
			 */
			msg_have++;
			msg_recv++;
			if (msg_recv == PGSTAT_RECVBUFFERSZ)
				msg_recv = 0;
		}

		/*
		 * If the collector is ready to receive, write a buffered
		 * message into his pipe.
		 */
		if (FD_ISSET(pgStatPipe[1], &wfds))
		{
			len = write(pgStatPipe[1], &msgbuffer[msg_send], 
						msgbuffer[msg_send].msg_hdr.m_size);
			if (len < 0)
			{
				perror("PGSTATBUFF: write(2)");
				exit(1);
			}
			if (len != msgbuffer[msg_send].msg_hdr.m_size)
			{
				fprintf(stderr, "PGSTATBUFF: short write(2)");
				exit(1);
			}

			msg_have--;
			msg_send++;
			if (msg_send == PGSTAT_RECVBUFFERSZ)
				msg_send = 0;
		}

		/*
		 * Make sure we forwarded all messages before we check for
		 * Postmaster termination.
		 */
		if (FD_ISSET(pgStatSock, &rfds) || FD_ISSET(pgStatPipe[1], &wfds))
			continue;

		/*
		 * If the pipe from the postmaster is ready for reading,
		 * the kernel must have closed it on exit() (the postmaster
		 * never really writes to it). So we've done our job.
		 */
		if (FD_ISSET(pgStatPmPipe[0], &rfds))
			exit(0);
	}
}


/* ----------
 * pgstat_add_backend() -
 *
 *	Support function to keep our backen list up to date.
 * ----------
 */
static int
pgstat_add_backend(PgStat_MsgHdr *msg)
{
	PgStat_StatDBEntry	   *dbentry;
	PgStat_StatBeEntry	   *beentry;
	PgStat_StatBeDead		   *deadbe;
	bool						found;

	/*
	 * Check that the backend ID is valid
	 */
	if (msg->m_backendid < 1 || msg->m_backendid > MaxBackends)
	{
		fprintf(stderr, "PGSTAT: Invalid backend ID %d\n", msg->m_backendid);
		return -1;
	}

	/*
	 * Get the slot for this backendid.
	 */
	beentry = &pgStatBeTable[msg->m_backendid - 1];
	if (beentry->databaseid != InvalidOid)
	{
		/*
		 * If the slot contains the PID of this backend, 
		 * everything is fine and we got nothing to do.
		 */
		if (beentry->procpid == msg->m_procpid)
			return 0;
	}

	/*
	 * Lookup if this backend is known to be dead. This can be caused
	 * due to messages arriving in the wrong order - i.e. Postmaster's
	 * BETERM message might have arrived before we received all the
	 * backends stats messages, or even a new backend with the same
	 * backendid was faster in sending his BESTART.
	 *
	 * If the backend is known to be dead, we ignore this add.
	 */
	deadbe = (PgStat_StatBeDead *)hash_search(pgStatBeDead,
						(char *)&(msg->m_procpid), HASH_FIND, &found);
	if (deadbe == NULL)
	{
		fprintf(stderr, "PGSTAT: Dead backend table corrupted - abort\n");
		exit(1);
	}
	if (found)
		return 1;

	/*
	 * Backend isn't known to be dead. If it's slot is currently used,
	 * we have to kick out the old backend.
	 */
	if (beentry->databaseid != InvalidOid)
		pgstat_sub_backend(beentry->procpid);

	/*
	 * Put this new backend into the slot.
	 */
	beentry->databaseid = msg->m_databaseid;
	beentry->procpid    = msg->m_procpid;
	beentry->userid		= msg->m_userid;

	/*
	 * Lookup or create the database entry for this backends DB.
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
					(char *)&(msg->m_databaseid), HASH_ENTER,
					&found);
    if (dbentry == NULL)
	{
		fprintf(stderr, "PGSTAT: DB hash table corrupted - abort\n");
		exit(1);
	}

	/*
	 * If not found, initialize the new one.
	 */
	if (!found)
	{
		HASHCTL			hash_ctl;

		dbentry->tables				= NULL;
		dbentry->n_xact_commit		= 0;
		dbentry->n_xact_rollback	= 0;
		dbentry->n_blocks_fetched	= 0;
		dbentry->n_blocks_hit		= 0;
		dbentry->n_connects			= 0;
		dbentry->destroy			= 0;

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize  = sizeof(Oid);
		hash_ctl.datasize = sizeof(PgStat_StatTabEntry);
		hash_ctl.hash     = tag_hash;
		dbentry->tables = hash_create(PGSTAT_TAB_HASH_SIZE, &hash_ctl,
							HASH_ELEM | HASH_FUNCTION);
		if (dbentry->tables == NULL)
		{
			fprintf(stderr, "PGSTAT: failed to initialize hash table for "
							"new database entry\n");
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
	int						i;
	PgStat_StatBeDead	   *deadbe;
	bool					found;

	/*
	 * Search in the known-backends table for the slot containing
	 * this PID.
	 */
	for (i = 0; i < MaxBackends; i++)
	{
		if (pgStatBeTable[i].databaseid != InvalidOid &&
			pgStatBeTable[i].procpid == procpid)
		{
			/*
			 * That's him. Add an entry to the known to be dead backends.
			 * Due to possible misorder in the arrival of UDP packets
			 * it's possible that even if we know the backend is dead,
			 * there could still be messages queued that arrive later.
			 * Those messages must not cause our number of backends
			 * statistics to get screwed up, so we remember for a couple
			 * of seconds that this PID is dead and ignore them (only
			 * the counting of backends, not the table access stats they
			 * sent).
			 */
			deadbe = (PgStat_StatBeDead *)hash_search(pgStatBeDead,
							(char *)&procpid, HASH_ENTER, &found);
			if (deadbe == NULL)
			{
				fprintf(stderr, "PGSTAT: dead backend hash table corrupted "
								"- abort\n");
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
	 * No big problem if not found. This can happen if UDP messages
	 * arrive out of order here.
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
	HASH_SEQ_STATUS				hstat;
	HASH_SEQ_STATUS				tstat;
	PgStat_StatDBEntry		   *dbentry;
	PgStat_StatTabEntry		   *tabentry;
	PgStat_StatBeDead		   *deadbe;
	FILE					   *fpout;
	long					   *hentry;
	bool						found;
	int							i;

	/*
	 * Open the statistics temp file to write out
	 * the current values.
	 */
	fpout = fopen(pgStat_tmpfname, "w");
	if (fpout == NULL)
	{
		fprintf(stderr, "PGSTAT: cannot open temp stats file\nPGSTAT: ");
		perror(pgStat_tmpfname);
		fflush(stderr);
		return;
	}

	/*
	 * Walk through the database table.
	 */
	hash_seq_init(&hstat, pgStatDBHash);
	while ((hentry = hash_seq_search(&hstat)) != (long *)TRUE)
	{
		if (hentry == NULL)
		{
			fprintf(stderr, "PGSTAT: database hash table corrupted "
							"- abort\n");
			exit(1);
		}
		dbentry = (PgStat_StatDBEntry *)hentry;

		/*
		 * If this database is marked destroyed, count down and do
		 * so if it reaches 0.
		 */
		if (dbentry->destroy > 0)
		{
			if (--(dbentry->destroy) == 0)
			{
				if (dbentry->tables != NULL)
					hash_destroy(dbentry->tables);

				hentry = hash_search(pgStatDBHash, 
								(char *)&(dbentry->databaseid),
								HASH_REMOVE, &found);
				if (hentry == NULL)
				{
					fprintf(stderr, "PGSTAT: database hash table corrupted "
									"during cleanup - abort\n");
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
		while((hentry = hash_seq_search(&tstat)) != (long *)TRUE)
		{
			if (hentry == NULL)
			{
				fprintf(stderr, "PGSTAT: tables hash table for database "
								"%d corrupted - abort\n",
								dbentry->databaseid);
				exit(1);
			}
			tabentry = (PgStat_StatTabEntry *)hentry;

			/*
			 * If table entry marked for destruction, same as above
			 * for the database entry.
			 */
			if (tabentry->destroy > 0)
			{
				if (--(tabentry->destroy) == 0)
				{
					hentry = hash_search(dbentry->tables,
									(char *)&(tabentry->tableid),
									HASH_REMOVE, &found);
					if (hentry == NULL)
					{
						fprintf(stderr, "PGSTAT: tables hash table for "
										"database %d corrupted during "
										"cleanup - abort\n",
										dbentry->databaseid);
						exit(1);
					}
				}
				continue;
			}

			/*
			 * At least we think this is still a life table. Print
			 * it's access stats.
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
	 * No more output to be done. Close the temp file and
	 * replace the old pgstat.stat with it's content.
	 */
	fputc('E', fpout);
	if (fclose(fpout) < 0)
	{
		fprintf(stderr, "PGSTAT: Error closing temp stats file\nPGSTAT: ");
		perror(pgStat_tmpfname);
		fprintf(stderr, "PGSTAT: Abort\n");
		fflush(stderr);
		exit(1);
	}
	else
	{
		if (rename(pgStat_tmpfname, pgStat_fname) < 0)
		{
			fprintf(stderr, "PGSTAT: Cannot rename temp stats file\n"
							"PGSTAT: ");
			perror(pgStat_fname);
			fflush(stderr);
		}
	}

	/*
	 * Clear out the dead backends table
	 */
	hash_seq_init(&hstat, pgStatBeDead);
	while ((hentry = hash_seq_search(&hstat)) != (long *)TRUE)
	{
		if (hentry == NULL)
		{
			fprintf(stderr, "PGSTAT: dead backend hash table corrupted "
							"during cleanup - abort\n");
			exit(1);
		}
		deadbe = (PgStat_StatBeDead *)hentry;

		/*
		 * Count down the destroy delay and remove entries where
		 * it reaches 0.
		 */
		if (--(deadbe->destroy) <= 0)
		{
			hentry = hash_search(pgStatBeDead, (char *)&(deadbe->procpid),
							HASH_REMOVE, &found);
			if (hentry == NULL)
			{
				fprintf(stderr, "PGSTAT: dead backend hash table corrupted "
								"during cleanup - abort\n");
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
	PgStat_StatDBEntry		   *dbentry;
	PgStat_StatDBEntry			dbbuf;
	PgStat_StatTabEntry		   *tabentry;
	PgStat_StatTabEntry			tabbuf;
	HASHCTL						hash_ctl;
	HTAB					   *tabhash = NULL;
	FILE					   *fpin;
	int							maxbackends = 0;
	int							havebackends = 0;
	bool						found;
	MemoryContext				use_mcxt;
	int							mcxt_flags;

	/*
	 * If running in the collector we use the DynaHashCxt
	 * memory context. If running in a backend, we use the
	 * TopTransactionContext instead, so the caller must only know the
	 * last XactId when this call happened to know if his tables are
	 * still valid or already gone!
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
	hash_ctl.keysize  = sizeof(Oid);
	hash_ctl.datasize = sizeof(PgStat_StatDBEntry);
	hash_ctl.hash     = tag_hash;
	hash_ctl.hcxt     = use_mcxt;
	*dbhash = hash_create(PGSTAT_DB_HASH_SIZE, &hash_ctl, 
							HASH_ELEM | HASH_FUNCTION | mcxt_flags);
	if (pgStatDBHash == NULL)
	{
		if (pgStatRunningInCollector)
		{
			fprintf(stderr, "PGSTAT: Creation of DB hash table failed\n");
			exit(1);
		}
			elog(ERROR, "PGSTAT: Creation of DB hash table failed");
	}

	/*
	 * Initialize the number of known backends to zero, just in case we
	 * do a silent error return below.
	 */
	if (numbackends != NULL)
		*numbackends = 0;
	if (betab != NULL)
		*betab = NULL;

	/*
	 * Try to open the status file. If it doesn't exist, the backends
	 * simply return zero for anything and the collector simply
	 * starts from scratch with empty counters.
	 */
	if ((fpin = fopen(pgStat_fname, "r")) == NULL)
		return;

	/*
	 * We found an existing collector stats file. Read it and put
	 * all the hashtable entries into place.
	 */
	for (;;)
	{
		switch (fgetc(fpin))
		{
			/*
			 * 'D'	A PgStat_StatDBEntry struct describing a database
			 *		follows. Subsequently, zero to many 'T' entries will
			 *		follow until a 'd' is encountered.
			 */
			case 'D':
				if (fread(&dbbuf, 1, sizeof(dbbuf), fpin) != sizeof(dbbuf))
				{
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, 
								"PGSTAT: corrupted pgstat.stat file\n");
						fclose(fpin);
						return;
					}
					else
					{
						elog(NOTICE, 
								"PGSTAT: corrupted pgstat.stat file");
						fclose(fpin);
						return;
					}
				}

				/*
				 * Add to the DB hash
				 */
				dbentry = (PgStat_StatDBEntry *)hash_search(*dbhash,
								(char *)&dbbuf.databaseid,
								HASH_ENTER, &found);
				if (dbentry == NULL)
				{
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, "PGSTAT: DB hash table corrupted\n");
						exit(1);
					}
					else
					{
						fclose(fpin);
						elog(ERROR, "PGSTAT: DB hash table corrupted");
					}
				}
				if (found)
				{
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, 
								"PGSTAT: corrupted pgstat.stat file\n");
						fclose(fpin);
						return;
					}
					else
					{
						elog(NOTICE, 
								"PGSTAT: corrupted pgstat.stat file");
						fclose(fpin);
						return;
					}
				}

				memcpy(dbentry, &dbbuf, sizeof(PgStat_StatDBEntry));
				dbentry->tables			= NULL;
				dbentry->destroy		= 0;
				dbentry->n_backends		= 0;

				/*
				 * Don't collect tables if not the requested DB
				 */
				if (onlydb != InvalidOid && onlydb != dbbuf.databaseid)
					break;


				memset(&hash_ctl, 0, sizeof(hash_ctl));
				hash_ctl.keysize  = sizeof(Oid);
				hash_ctl.datasize = sizeof(PgStat_StatTabEntry);
				hash_ctl.hash     = tag_hash;
				hash_ctl.hcxt     = use_mcxt;
				dbentry->tables = hash_create(PGSTAT_TAB_HASH_SIZE, &hash_ctl,
									HASH_ELEM | HASH_FUNCTION | mcxt_flags);
				if (dbentry->tables == NULL)
				{
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, "PGSTAT: failed to initialize "
										"hash table for new database entry\n");
						exit(1);
					}
					else
					{
						fclose(fpin);
						elog(ERROR, "PGSTAT: failed to initialize "
									"hash table for new database entry\n");
					}
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
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, 
								"PGSTAT: corrupted pgstat.stat file\n");
						fclose(fpin);
						return;
					}
					else
					{
						elog(NOTICE, 
								"PGSTAT: corrupted pgstat.stat file");
						fclose(fpin);
						return;
					}
				}

				/*
				 * Skip if table belongs to a not requested database.
				 */
				if (tabhash == NULL)
					break;

				tabentry = (PgStat_StatTabEntry *)hash_search(tabhash,
								(char *)&tabbuf.tableid,
								HASH_ENTER, &found);
				if (tabentry == NULL)
				{
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, "PGSTAT: Tab hash table corrupted\n");
						exit(1);
					}
					else
					{
						fclose(fpin);
						elog(ERROR, "PGSTAT: Tab hash table corrupted");
					}
				}

				if (found)
				{
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, 
								"PGSTAT: corrupted pgstat.stat file\n");
						fclose(fpin);
						return;
					}
					else
					{
						elog(NOTICE, 
								"PGSTAT: corrupted pgstat.stat file");
						fclose(fpin);
						return;
					}
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
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, 
								"PGSTAT: corrupted pgstat.stat file\n");
						fclose(fpin);
						return;
					}
					else
					{
						elog(NOTICE, 
								"PGSTAT: corrupted pgstat.stat file");
						fclose(fpin);
						return;
					}
				}
				if (maxbackends == 0)
				{
					fclose(fpin);
					return;
				}

				/*
				 * Allocate space (in TopTransactionContext too) for
				 * the backend table.
				 */
				if (use_mcxt == NULL)
					*betab = (PgStat_StatBeEntry *)malloc(
								sizeof(PgStat_StatBeEntry) * maxbackends);
				else
					*betab = (PgStat_StatBeEntry *)MemoryContextAlloc(
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
					if (pgStatRunningInCollector)
					{
						fprintf(stderr, 
								"PGSTAT: corrupted pgstat.stat file\n");
						fclose(fpin);
						return;
					}
					else
					{
						elog(NOTICE, 
								"PGSTAT: corrupted pgstat.stat file");
						fclose(fpin);
						return;
					}
				}

				/*
				 * Count backends per database here.
				 */
				dbentry = (PgStat_StatDBEntry *)hash_search(*dbhash,
								(char *)&((*betab)[havebackends].databaseid),
								HASH_FIND, &found);
				if (found)
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
				if (pgStatRunningInCollector)
				{
					fprintf(stderr, "PGSTAT: corrupted pgstat.stat file\n");
					fclose(fpin);
					return;
				}
				else
				{
					elog(NOTICE, "PGSTAT: corrupted pgstat.stat file");
					fclose(fpin);
					return;
				}
		}
	}

	fclose(fpin);
}


/* ----------
 * pgstat_recv_bestart() -
 *
 *	Process a backend starup message.
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
	/*
	 * Here we check explicitly for 0 return, since we don't
	 * want to mangle the activity of an active backend by
	 * a delayed packed from a dead one.
	 */
	if (pgstat_add_backend(&msg->m_hdr) != 0)
		return;
		
	strncpy(pgStatBeTable[msg->m_hdr.m_backendid - 1].activity,
			msg->m_what, PGSTAT_ACTIVITY_SIZE);
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
	PgStat_TableEntry	   *tabmsg = &(msg->m_entry[0]);
	PgStat_StatDBEntry     *dbentry;
	PgStat_StatTabEntry    *tabentry;
	int						i;
	bool					found;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
							(char *)&(msg->m_hdr.m_databaseid),
							HASH_FIND, &found);
	if (dbentry == NULL)
	{
		fprintf(stderr, "PGSTAT: database hash table corrupted - abort\n");
		exit(1);
	}
	if (!found)
		return;

	/*
	 * If the database is marked for destroy, this is a delayed
	 * UDP packet and not worth beeing counted.
	 */
	if (dbentry->destroy > 0)
		return;

	dbentry->n_xact_commit   += (PgStat_Counter)(msg->m_xact_commit);
	dbentry->n_xact_rollback += (PgStat_Counter)(msg->m_xact_rollback);

	/*
	 * Process all table entries in the message.
	 */
	for (i = 0; i < msg->m_nentries; i++)
	{
		tabentry = (PgStat_StatTabEntry *)hash_search(dbentry->tables,
						(char *)&(tabmsg[i].t_id), 
						HASH_ENTER, &found);
		if (tabentry == NULL)
		{
			fprintf(stderr, "PGSTAT: tables hash table corrupted for "
							"database %d - abort\n", dbentry->databaseid);
			exit(1);
		}

		if (!found)
		{
			/*
			 * If it's a new table entry, initialize counters
			 * to the values we just got.
			 */
			tabentry->numscans			= tabmsg[i].t_numscans;
			tabentry->tuples_returned	= tabmsg[i].t_tuples_returned;
			tabentry->tuples_fetched	= tabmsg[i].t_tuples_fetched;
			tabentry->tuples_inserted	= tabmsg[i].t_tuples_inserted;
			tabentry->tuples_updated	= tabmsg[i].t_tuples_updated;
			tabentry->tuples_deleted	= tabmsg[i].t_tuples_deleted;
			tabentry->blocks_fetched	= tabmsg[i].t_blocks_fetched;
			tabentry->blocks_hit		= tabmsg[i].t_blocks_hit;

			tabentry->destroy = 0;
		}
		else
		{
			/*
			 * Otherwise add the values to the existing entry.
			 */
			tabentry->numscans			+= tabmsg[i].t_numscans;
			tabentry->tuples_returned	+= tabmsg[i].t_tuples_returned;
			tabentry->tuples_fetched	+= tabmsg[i].t_tuples_fetched;
			tabentry->tuples_inserted	+= tabmsg[i].t_tuples_inserted;
			tabentry->tuples_updated	+= tabmsg[i].t_tuples_updated;
			tabentry->tuples_deleted	+= tabmsg[i].t_tuples_deleted;
			tabentry->blocks_fetched	+= tabmsg[i].t_blocks_fetched;
			tabentry->blocks_hit		+= tabmsg[i].t_blocks_hit;
		}

		/*
		 * And add the block IO to the database entry.
		 */
		dbentry->n_blocks_fetched	+= tabmsg[i].t_blocks_fetched;
		dbentry->n_blocks_hit		+= tabmsg[i].t_blocks_hit;
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
	PgStat_StatDBEntry     *dbentry;
	PgStat_StatTabEntry    *tabentry;
	int						i;
	bool					found;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
							(char *)&(msg->m_hdr.m_databaseid),
							HASH_FIND, &found);
	if (dbentry == NULL)
	{
		fprintf(stderr, "PGSTAT: database hash table corrupted - abort\n");
		exit(1);
	}
	if (!found)
		return;

	/*
	 * If the database is marked for destroy, this is a delayed
	 * UDP packet and the tables will go away at DB destruction.
	 */
	if (dbentry->destroy > 0)
		return;

	/*
	 * Process all table entries in the message.
	 */
	for (i = 0; i < msg->m_nentries; i++)
	{
		tabentry = (PgStat_StatTabEntry *)hash_search(dbentry->tables,
						(char *)&(msg->m_tableid[i]), 
						HASH_FIND, &found);
		if (tabentry == NULL)
		{
			fprintf(stderr, "PGSTAT: tables hash table corrupted for "
							"database %d - abort\n", dbentry->databaseid);
			exit(1);
		}

		if (found)
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
	PgStat_StatDBEntry     *dbentry;
	bool					found;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
							(char *)&(msg->m_databaseid),
							HASH_FIND, &found);
	if (dbentry == NULL)
	{
		fprintf(stderr, "PGSTAT: database hash table corrupted - abort\n");
		exit(1);
	}
	if (!found)
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
	HASHCTL					hash_ctl;
	PgStat_StatDBEntry     *dbentry;
	bool					found;

	/*
	 * Make sure the backend is counted for.
	 */
	if (pgstat_add_backend(&msg->m_hdr) < 0)
		return;

	/*
	 * Lookup the database in the hashtable.
	 */
	dbentry = (PgStat_StatDBEntry *)hash_search(pgStatDBHash,
							(char *)&(msg->m_hdr.m_databaseid),
							HASH_FIND, &found);
	if (dbentry == NULL)
	{
		fprintf(stderr, "PGSTAT: database hash table corrupted - abort\n");
		exit(1);
	}
	if (!found)
		return;

	/*
	 * We simply throw away all the databases table entries by recreating
	 * a new hash table for them.
	 */
	if (dbentry->tables != NULL)
		hash_destroy(dbentry->tables);

	dbentry->tables				= NULL;
	dbentry->n_xact_commit		= 0;
	dbentry->n_xact_rollback	= 0;
	dbentry->n_blocks_fetched	= 0;
	dbentry->n_blocks_hit		= 0;
	dbentry->n_connects			= 0;
	dbentry->destroy			= 0;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize  = sizeof(Oid);
	hash_ctl.datasize = sizeof(PgStat_StatTabEntry);
	hash_ctl.hash     = tag_hash;
	dbentry->tables = hash_create(PGSTAT_TAB_HASH_SIZE, &hash_ctl,
						HASH_ELEM | HASH_FUNCTION);
	if (dbentry->tables == NULL)
	{
		fprintf(stderr, "PGSTAT: failed to reinitialize hash table for "
						"database entry\n");
		exit(1);
	}
}


