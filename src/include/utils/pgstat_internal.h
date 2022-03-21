/* ----------
 * pgstat_internal.h
 *
 * Definitions for the PostgreSQL activity statistics facility that should
 * only be needed by files implementing statistics support (rather than ones
 * reporting / querying stats).
 *
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * src/include/utils/pgstat_internal.h
 * ----------
 */
#ifndef PGSTAT_INTERNAL_H
#define PGSTAT_INTERNAL_H


#include "pgstat.h"


#define PGSTAT_STAT_INTERVAL	500 /* Minimum time between stats file
									 * updates; in milliseconds. */

/* ----------
 * The initial size hints for the hash tables used in the collector.
 * ----------
 */
#define PGSTAT_DB_HASH_SIZE		16
#define PGSTAT_TAB_HASH_SIZE	512
#define PGSTAT_FUNCTION_HASH_SIZE	512
#define PGSTAT_SUBSCRIPTION_HASH_SIZE	32
#define PGSTAT_REPLSLOT_HASH_SIZE	32


/*
 * Some stats changes are transactional. To maintain those, a stack of
 * PgStat_SubXactStatus entries is maintained, which contain data pertaining
 * to the current transaction and its active subtransactions.
 */
typedef struct PgStat_SubXactStatus
{
	int			nest_level;		/* subtransaction nest level */

	struct PgStat_SubXactStatus *prev;	/* higher-level subxact if any */

	/*
	 * Tuple insertion/deletion counts for an open transaction can't be
	 * propagated into PgStat_TableStatus counters until we know if it is
	 * going to commit or abort.  Hence, we keep these counts in per-subxact
	 * structs that live in TopTransactionContext.  This data structure is
	 * designed on the assumption that subxacts won't usually modify very many
	 * tables.
	 */
	PgStat_TableXactStatus *first;	/* head of list for this subxact */
} PgStat_SubXactStatus;


/*
 * List of SLRU names that we keep stats for.  There is no central registry of
 * SLRUs, so we use this fixed list instead.  The "other" entry is used for
 * all SLRUs without an explicit entry (e.g. SLRUs in extensions).
 */
static const char *const slru_names[] = {
	"CommitTs",
	"MultiXactMember",
	"MultiXactOffset",
	"Notify",
	"Serial",
	"Subtrans",
	"Xact",
	"other"						/* has to be last */
};

#define SLRU_NUM_ELEMENTS	lengthof(slru_names)


/*
 * Functions in pgstat.c
 */

extern PgStat_SubXactStatus *pgstat_xact_stack_level_get(int nest_level);
extern void pgstat_setheader(PgStat_MsgHdr *hdr, StatMsgType mtype);
extern void pgstat_send(void *msg, int len);
#ifdef USE_ASSERT_CHECKING
extern void pgstat_assert_is_up(void);
#else
#define pgstat_assert_is_up() ((void)true)
#endif


/*
 * Functions in pgstat_database.c
 */

extern void AtEOXact_PgStat_Database(bool isCommit, bool parallel);
extern void pgstat_report_disconnect(Oid dboid);
extern void pgstat_update_dbstats(PgStat_MsgTabstat *tsmsg, TimestampTz now);


/*
 * Functions in pgstat_function.c
 */

extern void pgstat_send_funcstats(void);


/*
 * Functions in pgstat_relation.c
 */

extern void AtEOXact_PgStat_Relations(PgStat_SubXactStatus *xact_state, bool isCommit);
extern void AtEOSubXact_PgStat_Relations(PgStat_SubXactStatus *xact_state, bool isCommit, int nestDepth);
extern void AtPrepare_PgStat_Relations(PgStat_SubXactStatus *xact_state);
extern void PostPrepare_PgStat_Relations(PgStat_SubXactStatus *xact_state);
extern void pgstat_send_tabstats(TimestampTz now, bool disconnect);


/*
 * Functions in pgstat_slru.c
 */

extern void pgstat_send_slru(void);


/*
 * Functions in pgstat_wal.c
 */

extern void pgstat_wal_initialize(void);
extern bool pgstat_wal_pending(void);


/*
 * Variables in pgstat.c
 */

extern pgsocket pgStatSock;


/*
 * Variables in pgstat_database.c
 */

extern int	pgStatXactCommit;
extern int	pgStatXactRollback;


/*
 * Variables in pgstat_functions.c
 */

extern bool have_function_stats;


/*
 * Variables in pgstat_relation.c
 */

extern bool have_relation_stats;


#endif							/* PGSTAT_INTERNAL_H */
