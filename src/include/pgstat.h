/* ----------
 *	pgstat.h
 *
 *	Definitions for the PostgreSQL statistics collector daemon.
 *
 *	Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/include/pgstat.h,v 1.25 2004/06/26 16:32:04 tgl Exp $
 * ----------
 */
#ifndef PGSTAT_H
#define PGSTAT_H

#include "utils/hsearch.h"
#include "utils/nabstime.h"
#include "utils/rel.h"


/* ----------
 * The types of backend/postmaster -> collector messages
 * ----------
 */
#define PGSTAT_MTYPE_DUMMY			0
#define PGSTAT_MTYPE_BESTART		1
#define PGSTAT_MTYPE_BETERM			2
#define PGSTAT_MTYPE_ACTIVITY		3
#define PGSTAT_MTYPE_TABSTAT		4
#define PGSTAT_MTYPE_TABPURGE		5
#define PGSTAT_MTYPE_DROPDB			6
#define PGSTAT_MTYPE_RESETCOUNTER	7

/* ----------
 * The data type used for counters.
 * ----------
 */
typedef int64 PgStat_Counter;


/* ------------------------------------------------------------
 * Message formats follow
 * ------------------------------------------------------------
 */


/* ----------
 * PgStat_MsgHdr				The common message header
 * ----------
 */
typedef struct PgStat_MsgHdr
{
	int			m_type;
	int			m_size;
	int			m_backendid;
	int			m_procpid;
	Oid			m_databaseid;
	AclId		m_userid;
} PgStat_MsgHdr;

/* ----------
 * Space available in a message.  This will keep the UDP packets below 1K,
 * which should fit unfragmented into the MTU of the lo interface on most
 * platforms. Does anybody care for platforms where it doesn't?
 * ----------
 */
#define PGSTAT_MSG_PAYLOAD	(1000 - sizeof(PgStat_MsgHdr))

/* ----------
 * PgStat_TableEntry			Per-table info in a MsgTabstat
 * ----------
 */
typedef struct PgStat_TableEntry
{
	Oid			t_id;

	PgStat_Counter t_numscans;

	PgStat_Counter t_tuples_returned;
	PgStat_Counter t_tuples_fetched;
	PgStat_Counter t_tuples_inserted;
	PgStat_Counter t_tuples_updated;
	PgStat_Counter t_tuples_deleted;

	PgStat_Counter t_blocks_fetched;
	PgStat_Counter t_blocks_hit;
} PgStat_TableEntry;


/* ----------
 * PgStat_MsgDummy				A dummy message, ignored by the collector
 * ----------
 */
typedef struct PgStat_MsgDummy
{
	PgStat_MsgHdr m_hdr;
	char		m_dummy[512];
} PgStat_MsgDummy;

/* ----------
 * PgStat_MsgBestart			Sent by the backend on startup
 * ----------
 */
typedef struct PgStat_MsgBestart
{
	PgStat_MsgHdr m_hdr;
} PgStat_MsgBestart;

/* ----------
 * PgStat_MsgBeterm				Sent by the postmaster after backend exit
 * ----------
 */
typedef struct PgStat_MsgBeterm
{
	PgStat_MsgHdr m_hdr;
} PgStat_MsgBeterm;

/* ----------
 * PgStat_MsgActivity			Sent by the backends when they start
 *								to parse a query.
 * ----------
 */
#define PGSTAT_ACTIVITY_SIZE	PGSTAT_MSG_PAYLOAD

typedef struct PgStat_MsgActivity
{
	PgStat_MsgHdr m_hdr;
	char		m_what[PGSTAT_ACTIVITY_SIZE];
} PgStat_MsgActivity;

/* ----------
 * PgStat_MsgTabstat			Sent by the backend to report table
 *								and buffer access statistics.
 * ----------
 */
#define PGSTAT_NUM_TABENTRIES	((PGSTAT_MSG_PAYLOAD - 3 * sizeof(int))		\
								/ sizeof(PgStat_TableEntry))

typedef struct PgStat_MsgTabstat
{
	PgStat_MsgHdr m_hdr;
	int			m_nentries;
	int			m_xact_commit;
	int			m_xact_rollback;
	PgStat_TableEntry m_entry[PGSTAT_NUM_TABENTRIES];
} PgStat_MsgTabstat;

/* ----------
 * PgStat_MsgTabpurge			Sent by the backend to tell the collector
 *								about dead tables.
 * ----------
 */
#define PGSTAT_NUM_TABPURGE		((PGSTAT_MSG_PAYLOAD - sizeof(int))		\
								/ sizeof(Oid))

typedef struct PgStat_MsgTabpurge
{
	PgStat_MsgHdr m_hdr;
	int			m_nentries;
	Oid			m_tableid[PGSTAT_NUM_TABPURGE];
} PgStat_MsgTabpurge;


/* ----------
 * PgStat_MsgDropdb				Sent by the backend to tell the collector
 *								about dropped database
 * ----------
 */
typedef struct PgStat_MsgDropdb
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgDropdb;


/* ----------
 * PgStat_MsgResetcounter		Sent by the backend to tell the collector
 *								to reset counters
 * ----------
 */
typedef struct PgStat_MsgResetcounter
{
	PgStat_MsgHdr m_hdr;
} PgStat_MsgResetcounter;


/* ----------
 * PgStat_Msg					Union over all possible messages.
 * ----------
 */
typedef union PgStat_Msg
{
	PgStat_MsgHdr msg_hdr;
	PgStat_MsgDummy msg_dummy;
	PgStat_MsgBestart msg_bestart;
	PgStat_MsgActivity msg_activity;
	PgStat_MsgTabstat msg_tabstat;
	PgStat_MsgTabpurge msg_tabpurge;
	PgStat_MsgDropdb msg_dropdb;
	PgStat_MsgResetcounter msg_resetcounter;
} PgStat_Msg;


/* ------------------------------------------------------------
 * Statistic collector data structures follow
 * ------------------------------------------------------------
 */

/* ----------
 * PgStat_StatDBEntry			The collectors data per database
 * ----------
 */
typedef struct PgStat_StatDBEntry
{
	Oid			databaseid;
	HTAB	   *tables;
	int			n_backends;
	PgStat_Counter n_connects;
	PgStat_Counter n_xact_commit;
	PgStat_Counter n_xact_rollback;
	PgStat_Counter n_blocks_fetched;
	PgStat_Counter n_blocks_hit;
	int			destroy;
} PgStat_StatDBEntry;


/* ----------
 * PgStat_StatBeEntry			The collectors data per backend
 * ----------
 */
typedef struct PgStat_StatBeEntry
{
	Oid			databaseid;
	Oid			userid;
	int			procpid;
	AbsoluteTime activity_start_sec;
	int			activity_start_usec;
	char		activity[PGSTAT_ACTIVITY_SIZE];
} PgStat_StatBeEntry;


/* ----------
 * PgStat_StatBeDead			Because UDP packets can arrive out of
 *								order, we need to keep some information
 *								about backends that are known to be
 *								dead for some seconds. This info is held
 *								in a hash table of these structs.
 * ----------
 */
typedef struct PgStat_StatBeDead
{
	int			procpid;
	int			backendid;
	int			destroy;
} PgStat_StatBeDead;


/* ----------
 * PgStat_StatTabEntry			The collectors data table data
 * ----------
 */
typedef struct PgStat_StatTabEntry
{
	Oid			tableid;

	PgStat_Counter numscans;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;
	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;

	int			destroy;
} PgStat_StatTabEntry;


/* ----------
 * GUC parameters
 * ----------
 */
extern bool pgstat_collect_startcollector;
extern bool pgstat_collect_resetonpmstart;
extern bool pgstat_collect_querystring;
extern bool pgstat_collect_tuplelevel;
extern bool pgstat_collect_blocklevel;


/* ----------
 * Functions called from postmaster
 * ----------
 */
extern void pgstat_init(void);
extern int	pgstat_start(void);
extern void pgstat_beterm(int pid);

#ifdef EXEC_BACKEND
extern void PgstatBufferMain(int argc, char *argv[]);
extern void PgstatCollectorMain(int argc, char *argv[]);
#endif


/* ----------
 * Functions called from backends
 * ----------
 */
extern void pgstat_bestart(void);

extern void pgstat_ping(void);
extern void pgstat_report_activity(const char *what);
extern void pgstat_report_tabstat(void);
extern int	pgstat_vacuum_tabstat(void);

extern void pgstat_reset_counters(void);

extern void pgstat_initstats(PgStat_Info *stats, Relation rel);


#define pgstat_reset_heap_scan(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			(s)->heap_scan_counted = FALSE;								\
	} while (0)
#define pgstat_count_heap_scan(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL &&		\
				!(s)->heap_scan_counted) {								\
			((PgStat_TableEntry *)((s)->tabentry))->t_numscans++;		\
			(s)->heap_scan_counted = TRUE;								\
		}																\
	} while (0)
#define pgstat_count_heap_getnext(s)									\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_tuples_returned++; \
	} while (0)
#define pgstat_count_heap_fetch(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_tuples_fetched++; \
	} while (0)
#define pgstat_count_heap_insert(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_tuples_inserted++; \
	} while (0)
#define pgstat_count_heap_update(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_tuples_updated++; \
	} while (0)
#define pgstat_count_heap_delete(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_tuples_deleted++; \
	} while (0)
#define pgstat_reset_index_scan(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			(s)->index_scan_counted = FALSE;							\
	} while (0)
#define pgstat_count_index_scan(s)										\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL &&		\
				!(s)->index_scan_counted) {								\
			((PgStat_TableEntry *)((s)->tabentry))->t_numscans++;		\
			(s)->index_scan_counted = TRUE;								\
		}																\
	} while (0)
#define pgstat_count_index_getnext(s)									\
	do {																\
		if (pgstat_collect_tuplelevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_tuples_returned++; \
	} while (0)
#define pgstat_count_buffer_read(s,r)									\
	do {																\
		if (pgstat_collect_blocklevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_blocks_fetched++; \
		else {															\
			if (pgstat_collect_blocklevel && !(s)->no_stats) {			\
				pgstat_initstats((s), (r));								\
				if ((s)->tabentry != NULL)								\
					((PgStat_TableEntry *)((s)->tabentry))->t_blocks_fetched++; \
			}															\
		}																\
	} while (0)
#define pgstat_count_buffer_hit(s,r)									\
	do {																\
		if (pgstat_collect_blocklevel && (s)->tabentry != NULL)			\
			((PgStat_TableEntry *)((s)->tabentry))->t_blocks_hit++;		\
		else {															\
			if (pgstat_collect_blocklevel && !(s)->no_stats) {			\
				pgstat_initstats((s), (r));								\
				if ((s)->tabentry != NULL)								\
					((PgStat_TableEntry *)((s)->tabentry))->t_blocks_hit++; \
			}															\
		}																\
	} while (0)


extern void pgstat_count_xact_commit(void);
extern void pgstat_count_xact_rollback(void);

/* ----------
 * Support functions for the SQL-callable functions to
 * generate the pgstat* views.
 * ----------
 */
extern PgStat_StatDBEntry *pgstat_fetch_stat_dbentry(Oid dbid);
extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry(Oid relid);
extern PgStat_StatBeEntry *pgstat_fetch_stat_beentry(int beid);
extern int	pgstat_fetch_stat_numbackends(void);

#endif   /* PGSTAT_H */
