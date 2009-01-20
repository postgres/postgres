/*-------------------------------------------------------------------------
 *
 * autovacuum.c
 *
 * PostgreSQL Integrated Autovacuum Daemon
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/postmaster/autovacuum.c,v 1.29.2.2 2009/01/20 12:17:23 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_autovacuum.h"
#include "catalog/pg_database.h"
#include "commands/vacuum.h"
#include "libpq/hba.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"


/*
 * GUC parameters
 */
bool		autovacuum_start_daemon = false;
int			autovacuum_naptime;
int			autovacuum_vac_thresh;
double		autovacuum_vac_scale;
int			autovacuum_anl_thresh;
double		autovacuum_anl_scale;
int			autovacuum_freeze_max_age;

int			autovacuum_vac_cost_delay;
int			autovacuum_vac_cost_limit;

/* Flag to tell if we are in the autovacuum daemon process */
static bool am_autovacuum = false;

/* Last time autovac daemon started/stopped (only valid in postmaster) */
static time_t last_autovac_start_time = 0;
static time_t last_autovac_stop_time = 0;

/* Comparison point for determining whether freeze_max_age is exceeded */
static TransactionId recentXid;

/* Default freeze_min_age to use for autovacuum (varies by database) */
static int	default_freeze_min_age;

/* Memory context for long-lived data */
static MemoryContext AutovacMemCxt;

/* struct to keep list of candidate databases for vacuum */
typedef struct autovac_dbase
{
	Oid			oid;
	char	   *name;
	TransactionId frozenxid;
	PgStat_StatDBEntry *entry;
} autovac_dbase;

/* struct to keep track of tables to vacuum and/or analyze */
typedef struct autovac_table
{
	Oid			relid;
	Oid			toastrelid;
	bool		dovacuum;
	bool		doanalyze;
	int			freeze_min_age;
	int			vacuum_cost_delay;
	int			vacuum_cost_limit;
} autovac_table;


#ifdef EXEC_BACKEND
static pid_t autovac_forkexec(void);
#endif
NON_EXEC_STATIC void AutoVacMain(int argc, char *argv[]);
static void do_autovacuum(PgStat_StatDBEntry *dbentry);
static List *autovac_get_database_list(void);
static void test_rel_for_autovac(Oid relid, PgStat_StatTabEntry *tabentry,
					 Form_pg_class classForm,
					 Form_pg_autovacuum avForm,
					 List **vacuum_tables,
					 List **toast_table_ids);
static void autovacuum_do_vac_analyze(Oid relid, bool dovacuum,
						  bool doanalyze, int freeze_min_age);
static void autovac_report_activity(VacuumStmt *vacstmt, Oid relid);


/*
 * Main entry point for autovacuum controller process.
 *
 * This code is heavily based on pgarch.c, q.v.
 */
int
autovac_start(void)
{
	time_t		curtime;
	pid_t		AutoVacPID;

	/*
	 * Do nothing if too soon since last autovacuum exit.  This limits how
	 * often the daemon runs.  Since the time per iteration can be quite
	 * variable, it seems more useful to measure/control the time since last
	 * subprocess exit than since last subprocess launch.
	 *
	 * However, we *also* check the time since last subprocess launch; this
	 * prevents thrashing under fork-failure conditions.
	 *
	 * Note that since we will be re-called from the postmaster main loop, we
	 * will get another chance later if we do nothing now.
	 *
	 * XXX todo: implement sleep scale factor that existed in contrib code.
	 */

	curtime = time(NULL);
	if ((unsigned int) (curtime - last_autovac_stop_time) <
		(unsigned int) autovacuum_naptime)
		return 0;

	if ((unsigned int) (curtime - last_autovac_start_time) <
		(unsigned int) autovacuum_naptime)
		return 0;

	last_autovac_start_time = curtime;

#ifdef EXEC_BACKEND
	switch ((AutoVacPID = autovac_forkexec()))
#else
	switch ((AutoVacPID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork autovacuum process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			/* Lose the postmaster's on-exit routines */
			on_exit_reset();

			AutoVacMain(0, NULL);
			break;
#endif
		default:
			return (int) AutoVacPID;
	}

	/* shouldn't get here */
	return 0;
}

/*
 * autovac_stopped --- called by postmaster when subprocess exit is detected
 */
void
autovac_stopped(void)
{
	last_autovac_stop_time = time(NULL);
}

#ifdef EXEC_BACKEND
/*
 * autovac_forkexec()
 *
 * Format up the arglist for the autovacuum process, then fork and exec.
 */
static pid_t
autovac_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkautovac";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */

/*
 * AutoVacMain
 */
NON_EXEC_STATIC void
AutoVacMain(int argc, char *argv[])
{
	ListCell   *cell;
	List	   *dblist;
	autovac_dbase *db;
	TransactionId xidForceLimit;
	bool		for_xid_wrap;
	sigjmp_buf	local_sigjmp_buf;

	/* we are a postmaster subprocess now */
	IsUnderPostmaster = true;
	am_autovacuum = true;

	/* reset MyProcPid */
	MyProcPid = getpid();

	/* Identify myself via ps */
	init_ps_display("autovacuum process", "", "", "");

	SetProcessingMode(InitProcessing);

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.  (autovacuum probably never has
	 * any child processes, but for consistency we make all postmaster
	 * child processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * Set up signal handlers.	We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 *
	 * Currently, we don't pay attention to postgresql.conf changes that
	 * happen during a single daemon iteration, so we can ignore SIGHUP.
	 */
	pqsignal(SIGHUP, SIG_IGN);

	/*
	 * Presently, SIGINT will lead to autovacuum shutdown, because that's how
	 * we handle ereport(ERROR).  It could be improved however.
	 */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	pqsignal(SIGQUIT, quickdie);
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, CatchupInterruptHandler);
	/* We don't listen for async notifies */
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Early initialization */
	BaseInit();

	/*
	 * Create a per-backend PGPROC struct in shared memory, except in the
	 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
	 * this before we can use LWLocks (and in the EXEC_BACKEND case we already
	 * had to do some stuff with LWLocks).
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * We can now go away.	Note that because we'll call InitProcess, a
		 * callback will be registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	/*
	 * Force zero_damaged_pages OFF in the autovac process, even if it is set
	 * in postgresql.conf.	We don't really want such a dangerous option being
	 * applied non-interactively.
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/* Get a list of databases */
	dblist = autovac_get_database_list();

	/*
	 * Determine the oldest datfrozenxid/relfrozenxid that we will allow
	 * to pass without forcing a vacuum.  (This limit can be tightened for
	 * particular tables, but not loosened.)
	 */
	recentXid = ReadNewTransactionId();
	xidForceLimit = recentXid - autovacuum_freeze_max_age;
	/* ensure it's a "normal" XID, else TransactionIdPrecedes misbehaves */
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;

	/*
	 * Choose a database to connect to.  We pick the database that was least
	 * recently auto-vacuumed, or one that needs vacuuming to prevent Xid
	 * wraparound-related data loss.  If any db at risk of wraparound is
	 * found, we pick the one with oldest datfrozenxid,
	 * independently of autovacuum times.
	 *
	 * Note that a database with no stats entry is not considered, except for
	 * Xid wraparound purposes.  The theory is that if no one has ever
	 * connected to it since the stats were last initialized, it doesn't need
	 * vacuuming.
	 *
	 * XXX This could be improved if we had more info about whether it needs
	 * vacuuming before connecting to it.  Perhaps look through the pgstats
	 * data for the database's tables?  One idea is to keep track of the
	 * number of new and dead tuples per database in pgstats.  However it
	 * isn't clear how to construct a metric that measures that and not cause
	 * starvation for less busy databases.
	 */
	db = NULL;
	for_xid_wrap = false;
	foreach(cell, dblist)
	{
		autovac_dbase *tmp = lfirst(cell);

		/* Find pgstat entry if any */
		tmp->entry = pgstat_fetch_stat_dbentry(tmp->oid);

		/* Check to see if this one is at risk of wraparound */
		if (TransactionIdPrecedes(tmp->frozenxid, xidForceLimit))
		{
			if (db == NULL ||
				TransactionIdPrecedes(tmp->frozenxid, db->frozenxid))
				db = tmp;
			for_xid_wrap = true;
			continue;
		}
		else if (for_xid_wrap)
			continue;			/* ignore not-at-risk DBs */

		/*
		 * Otherwise, skip a database with no pgstat entry; it means it
		 * hasn't seen any activity.
		 */
		if (!tmp->entry)
			continue;

		/*
		 * Remember the db with oldest autovac time.  (If we are here,
		 * both tmp->entry and db->entry must be non-null.)
		 */
		if (db == NULL ||
			tmp->entry->last_autovac_time < db->entry->last_autovac_time)
			db = tmp;
	}

	if (db)
	{
		/*
		 * Report autovac startup to the stats collector.  We deliberately do
		 * this before InitPostgres, so that the last_autovac_time will get
		 * updated even if the connection attempt fails.  This is to prevent
		 * autovac from getting "stuck" repeatedly selecting an unopenable
		 * database, rather than making any progress on stuff it can connect
		 * to.
		 */
		pgstat_report_autovac(db->oid);

		/*
		 * Connect to the selected database
		 *
		 * Note: if we have selected a just-deleted database (due to using
		 * stale stats info), we'll fail and exit here.
		 */
		InitPostgres(db->name, NULL);
		SetProcessingMode(NormalProcessing);
		set_ps_display(db->name, false);
		ereport(DEBUG1,
				(errmsg("autovacuum: processing database \"%s\"", db->name)));

		/* Create the memory context where cross-transaction state is stored */
		AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
											  "Autovacuum context",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * And do an appropriate amount of work
		 */
		do_autovacuum(db->entry);
	}

	/* One iteration done, go away */
	proc_exit(0);
}

/*
 * autovac_get_database_list
 *
 *		Return a list of all databases.  Note we cannot use pg_database,
 *		because we aren't connected yet; we use the flat database file.
 */
static List *
autovac_get_database_list(void)
{
	char	   *filename;
	List	   *dblist = NIL;
	char		thisname[NAMEDATALEN];
	FILE	   *db_file;
	Oid			db_id;
	Oid			db_tablespace;
	TransactionId db_frozenxid;

	filename = database_getflatfilename();
	db_file = AllocateFile(filename, "r");
	if (db_file == NULL)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));

	while (read_pg_database_line(db_file, thisname, &db_id,
								 &db_tablespace, &db_frozenxid))
	{
		autovac_dbase *db;

		db = (autovac_dbase *) palloc(sizeof(autovac_dbase));

		db->oid = db_id;
		db->name = pstrdup(thisname);
		db->frozenxid = db_frozenxid;
		/* this gets set later: */
		db->entry = NULL;

		dblist = lappend(dblist, db);
	}

	FreeFile(db_file);
	pfree(filename);

	return dblist;
}

/*
 * Process a database table-by-table
 *
 * dbentry is either a pointer to the database entry in the stats databases
 * hash table, or NULL if we couldn't find any entry (the latter case occurs
 * only if we are forcing a vacuum for anti-wrap purposes).
 *
 * Note that CHECK_FOR_INTERRUPTS is supposed to be used in certain spots in
 * order not to ignore shutdown commands for too long.
 */
static void
do_autovacuum(PgStat_StatDBEntry *dbentry)
{
	Relation	classRel,
				avRel;
	HeapTuple	tuple;
	HeapScanDesc relScan;
	Form_pg_database dbForm;
	List	   *vacuum_tables = NIL;
	List	   *toast_table_ids = NIL;
	ListCell   *cell;
	PgStat_StatDBEntry *shared;

	/* Start a transaction so our commands have one to play into. */
	StartTransactionCommand();

	/* functions in indexes may want a snapshot set */
	ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());

	/*
	 * Clean up any dead statistics collector entries for this DB. We always
	 * want to do this exactly once per DB-processing cycle, even if we find
	 * nothing worth vacuuming in the database.
	 */
	pgstat_vacuum_tabstat();

	/*
	 * Find the pg_database entry and select the default freeze_min_age.
	 * We use zero in template and nonconnectable databases,
	 * else the system-wide default.
	 */
	tuple = SearchSysCache(DATABASEOID,
						   ObjectIdGetDatum(MyDatabaseId),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbForm = (Form_pg_database) GETSTRUCT(tuple);

	if (dbForm->datistemplate || !dbForm->datallowconn)
		default_freeze_min_age = 0;
	else
		default_freeze_min_age = vacuum_freeze_min_age;

	ReleaseSysCache(tuple);

	/*
	 * StartTransactionCommand and CommitTransactionCommand will automatically
	 * switch to other contexts.  We need this one to keep the list of
	 * relations to vacuum/analyze across transactions.
	 */
	MemoryContextSwitchTo(AutovacMemCxt);

	/* The database hash where pgstat keeps shared relations */
	shared = pgstat_fetch_stat_dbentry(InvalidOid);

	classRel = heap_open(RelationRelationId, AccessShareLock);
	avRel = heap_open(AutovacuumRelationId, AccessShareLock);

	/*
	 * Scan pg_class and determine which tables to vacuum.
	 *
	 * The stats subsystem collects stats for toast tables independently of
	 * the stats for their parent tables.  We need to check those stats since
	 * in cases with short, wide tables there might be proportionally much
	 * more activity in the toast table than in its parent.
	 *
	 * Since we can only issue VACUUM against the parent table, we need to
	 * transpose a decision to vacuum a toast table into a decision to vacuum
	 * its parent.	There's no point in considering ANALYZE on a toast table,
	 * either.	To support this, we keep a list of OIDs of toast tables that
	 * need vacuuming alongside the list of regular tables.  Regular tables
	 * will be entered into the table list even if they appear not to need
	 * vacuuming; we go back and re-mark them after finding all the vacuumable
	 * toast tables.
	 */
	relScan = heap_beginscan(classRel, SnapshotNow, 0, NULL);

	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		Form_pg_autovacuum avForm = NULL;
		PgStat_StatTabEntry *tabentry;
		SysScanDesc avScan;
		HeapTuple	avTup;
		ScanKeyData entry[1];
		Oid			relid;

		/* Consider only regular and toast tables. */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_TOASTVALUE)
			continue;

		/*
		 * Skip temp tables (i.e. those in temp namespaces).  We cannot safely
		 * process other backends' temp tables.
		 */
		if (isAnyTempNamespace(classForm->relnamespace))
			continue;

		relid = HeapTupleGetOid(tuple);

		/* See if we have a pg_autovacuum entry for this relation. */
		ScanKeyInit(&entry[0],
					Anum_pg_autovacuum_vacrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));

		avScan = systable_beginscan(avRel, AutovacuumRelidIndexId, true,
									SnapshotNow, 1, entry);

		avTup = systable_getnext(avScan);

		if (HeapTupleIsValid(avTup))
			avForm = (Form_pg_autovacuum) GETSTRUCT(avTup);

		if (classForm->relisshared && PointerIsValid(shared))
			tabentry = hash_search(shared->tables, &relid,
								   HASH_FIND, NULL);
		else if (PointerIsValid(dbentry))
			tabentry = hash_search(dbentry->tables, &relid,
								   HASH_FIND, NULL);
		else
			tabentry = NULL;

		test_rel_for_autovac(relid, tabentry, classForm, avForm,
							 &vacuum_tables, &toast_table_ids);

		systable_endscan(avScan);
	}

	heap_endscan(relScan);
	heap_close(avRel, AccessShareLock);
	heap_close(classRel, AccessShareLock);

	/*
	 * Perform operations on collected tables.
	 */
	foreach(cell, vacuum_tables)
	{
		autovac_table *tab = lfirst(cell);

		CHECK_FOR_INTERRUPTS();

		/*
		 * Check to see if we need to force vacuuming of this table because
		 * its toast table needs it.
		 */
		if (OidIsValid(tab->toastrelid) && !tab->dovacuum &&
			list_member_oid(toast_table_ids, tab->toastrelid))
		{
			tab->dovacuum = true;
			elog(DEBUG2, "autovac: VACUUM %u because of TOAST table",
				 tab->relid);
		}

		/* Otherwise, ignore table if it needs no work */
		if (!tab->dovacuum && !tab->doanalyze)
			continue;

		/* Set the vacuum cost parameters for this table */
		VacuumCostDelay = tab->vacuum_cost_delay;
		VacuumCostLimit = tab->vacuum_cost_limit;

		autovacuum_do_vac_analyze(tab->relid,
								  tab->dovacuum,
								  tab->doanalyze,
								  tab->freeze_min_age);
	}

	/*
	 * Update pg_database.datfrozenxid, and truncate pg_clog if possible.
	 * We only need to do this once, not after each table.
	 */
	vac_update_datfrozenxid();

	/* Finally close out the last transaction. */
	CommitTransactionCommand();
}

/*
 * test_rel_for_autovac
 *
 * Check whether a table needs to be vacuumed or analyzed.	Add it to the
 * appropriate output list if so.
 *
 * A table needs to be vacuumed if the number of dead tuples exceeds a
 * threshold.  This threshold is calculated as
 *
 * threshold = vac_base_thresh + vac_scale_factor * reltuples
 *
 * For analyze, the analysis done is that the number of tuples inserted,
 * deleted and updated since the last analyze exceeds a threshold calculated
 * in the same fashion as above.  Note that the collector actually stores
 * the number of tuples (both live and dead) that there were as of the last
 * analyze.  This is asymmetric to the VACUUM case.
 *
 * We also force vacuum if the table's relfrozenxid is more than freeze_max_age
 * transactions back.
 *
 * A table whose pg_autovacuum.enabled value is false, is automatically
 * skipped (unless we have to vacuum it due to freeze_max_age).  Thus
 * autovacuum can be disabled for specific tables.  Also, when the stats
 * collector does not have data about a table, it will be skipped.
 *
 * A table whose vac_base_thresh value is <0 takes the base value from the
 * autovacuum_vacuum_threshold GUC variable.  Similarly, a vac_scale_factor
 * value <0 is substituted with the value of
 * autovacuum_vacuum_scale_factor GUC variable.  Ditto for analyze.
 */
static void
test_rel_for_autovac(Oid relid, PgStat_StatTabEntry *tabentry,
					 Form_pg_class classForm,
					 Form_pg_autovacuum avForm,
					 List **vacuum_tables,
					 List **toast_table_ids)
{
	bool		force_vacuum;
	bool		dovacuum;
	bool		doanalyze;
	float4		reltuples;		/* pg_class.reltuples */
	/* constants from pg_autovacuum or GUC variables */
	int			vac_base_thresh,
				anl_base_thresh;
	float4		vac_scale_factor,
				anl_scale_factor;
	/* thresholds calculated from above constants */
	float4		vacthresh,
				anlthresh;
	/* number of vacuum (resp. analyze) tuples at this time */
	float4		vactuples,
				anltuples;
	/* freeze parameters */
	int			freeze_min_age;
	int			freeze_max_age;
	TransactionId xidForceLimit;
	/* cost-based vacuum delay parameters */
	int			vac_cost_limit;
	int			vac_cost_delay;

	/*
	 * If there is a tuple in pg_autovacuum, use it; else, use the GUC
	 * defaults.  Note that the fields may contain "-1" (or indeed any
	 * negative value), which means use the GUC defaults for each setting.
	 *
	 * Note: in cost_limit, 0 also means use the value from elsewhere,
	 * because 0 is not a valid value for VacuumCostLimit.
	 */
	if (avForm != NULL)
	{
		vac_scale_factor = (avForm->vac_scale_factor >= 0) ?
			avForm->vac_scale_factor : autovacuum_vac_scale;
		vac_base_thresh = (avForm->vac_base_thresh >= 0) ?
			avForm->vac_base_thresh : autovacuum_vac_thresh;

		anl_scale_factor = (avForm->anl_scale_factor >= 0) ?
			avForm->anl_scale_factor : autovacuum_anl_scale;
		anl_base_thresh = (avForm->anl_base_thresh >= 0) ?
			avForm->anl_base_thresh : autovacuum_anl_thresh;

		freeze_min_age = (avForm->freeze_min_age >= 0) ?
			avForm->freeze_min_age : default_freeze_min_age;
		freeze_max_age = (avForm->freeze_max_age >= 0) ?
			Min(avForm->freeze_max_age, autovacuum_freeze_max_age) :
			autovacuum_freeze_max_age;

		vac_cost_limit = (avForm->vac_cost_limit > 0) ?
			avForm->vac_cost_limit :
			((autovacuum_vac_cost_limit > 0) ?
			 autovacuum_vac_cost_limit : VacuumCostLimit);

		vac_cost_delay = (avForm->vac_cost_delay >= 0) ?
			avForm->vac_cost_delay :
			((autovacuum_vac_cost_delay >= 0) ?
			 autovacuum_vac_cost_delay : VacuumCostDelay);
	}
	else
	{
		vac_scale_factor = autovacuum_vac_scale;
		vac_base_thresh = autovacuum_vac_thresh;

		anl_scale_factor = autovacuum_anl_scale;
		anl_base_thresh = autovacuum_anl_thresh;

		freeze_min_age = default_freeze_min_age;
		freeze_max_age = autovacuum_freeze_max_age;

		vac_cost_limit = (autovacuum_vac_cost_limit > 0) ?
			autovacuum_vac_cost_limit : VacuumCostLimit;

		vac_cost_delay = (autovacuum_vac_cost_delay >= 0) ?
			autovacuum_vac_cost_delay : VacuumCostDelay;
	}

	/* Force vacuum if table is at risk of wraparound */
	xidForceLimit = recentXid - freeze_max_age;
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;
	force_vacuum = (TransactionIdIsNormal(classForm->relfrozenxid) &&
					TransactionIdPrecedes(classForm->relfrozenxid,
										  xidForceLimit));

	/* User disabled it in pg_autovacuum?  (But ignore if at risk) */
	if (avForm && !avForm->enabled && !force_vacuum)
		return;

	if (PointerIsValid(tabentry))
	{
		reltuples = classForm->reltuples;
		vactuples = tabentry->n_dead_tuples;
		anltuples = tabentry->n_live_tuples + tabentry->n_dead_tuples -
			tabentry->last_anl_tuples;

		vacthresh = (float4) vac_base_thresh + vac_scale_factor * reltuples;
		anlthresh = (float4) anl_base_thresh + anl_scale_factor * reltuples;

		/*
		 * Note that we don't need to take special consideration for stat
		 * reset, because if that happens, the last vacuum and analyze counts
		 * will be reset too.
		 */
		elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), anl: %.0f (threshold %.0f)",
			 NameStr(classForm->relname),
			 vactuples, vacthresh, anltuples, anlthresh);

		/* Determine if this table needs vacuum or analyze. */
		dovacuum = force_vacuum || (vactuples > vacthresh);
		doanalyze = (anltuples > anlthresh);
	}
	else
	{
		/*
		 * Skip a table not found in stat hash, unless we have to force
		 * vacuum for anti-wrap purposes.  If it's not acted upon, there's
		 * no need to vacuum it.
		 */
		dovacuum = force_vacuum;
		doanalyze = false;
	}

	/* ANALYZE refuses to work with pg_statistics */
	if (relid == StatisticRelationId)
		doanalyze = false;

	Assert(CurrentMemoryContext == AutovacMemCxt);

	if (classForm->relkind == RELKIND_RELATION)
	{
		if (dovacuum || doanalyze)
			elog(DEBUG2, "autovac: will%s%s %s",
				 (dovacuum ? " VACUUM" : ""),
				 (doanalyze ? " ANALYZE" : ""),
				 NameStr(classForm->relname));

		/*
		 * we must record tables that have a toast table, even if we currently
		 * don't think they need vacuuming.
		 */
		if (dovacuum || doanalyze || OidIsValid(classForm->reltoastrelid))
		{
			autovac_table *tab;

			tab = (autovac_table *) palloc(sizeof(autovac_table));
			tab->relid = relid;
			tab->toastrelid = classForm->reltoastrelid;
			tab->dovacuum = dovacuum;
			tab->doanalyze = doanalyze;
			tab->freeze_min_age = freeze_min_age;
			tab->vacuum_cost_limit = vac_cost_limit;
			tab->vacuum_cost_delay = vac_cost_delay;

			*vacuum_tables = lappend(*vacuum_tables, tab);
		}
	}
	else
	{
		Assert(classForm->relkind == RELKIND_TOASTVALUE);
		if (dovacuum)
			*toast_table_ids = lappend_oid(*toast_table_ids, relid);
	}
}

/*
 * autovacuum_do_vac_analyze
 *		Vacuum and/or analyze the specified table
 */
static void
autovacuum_do_vac_analyze(Oid relid, bool dovacuum, bool doanalyze,
						  int freeze_min_age)
{
	VacuumStmt *vacstmt;

	/*
	 * The node must survive transaction boundaries, so make sure we create it
	 * in a long-lived context
	 */
	MemoryContextSwitchTo(AutovacMemCxt);

	vacstmt = makeNode(VacuumStmt);

	/*
	 * Point QueryContext to the autovac memory context to fake out the
	 * PreventTransactionChain check inside vacuum().  Note that this is also
	 * why we palloc vacstmt instead of just using a local variable.
	 */
	QueryContext = CurrentMemoryContext;

	/* Set up command parameters */
	vacstmt->vacuum = dovacuum;
	vacstmt->full = false;
	vacstmt->analyze = doanalyze;
	vacstmt->freeze_min_age = freeze_min_age;
	vacstmt->verbose = false;
	vacstmt->relation = NULL;	/* not used since we pass relids list */
	vacstmt->va_cols = NIL;

	/* Let pgstat know what we're doing */
	autovac_report_activity(vacstmt, relid);

	vacuum(vacstmt, list_make1_oid(relid));

	pfree(vacstmt);

	/* Make sure we end up pointing to the long-lived context at exit */
	MemoryContextSwitchTo(AutovacMemCxt);
}

/*
 * autovac_report_activity
 *		Report to pgstat what autovacuum is doing
 *
 * We send a SQL string corresponding to what the user would see if the
 * equivalent command was to be issued manually.
 *
 * Note we assume that we are going to report the next command as soon as we're
 * done with the current one, and exiting right after the last one, so we don't
 * bother to report "<IDLE>" or some such.
 */
static void
autovac_report_activity(VacuumStmt *vacstmt, Oid relid)
{
	char	   *relname = get_rel_name(relid);
	char	   *nspname = get_namespace_name(get_rel_namespace(relid));
#define MAX_AUTOVAC_ACTIV_LEN (NAMEDATALEN * 2 + 32)
	char		activity[MAX_AUTOVAC_ACTIV_LEN];

	/* Report the command and possible options */
	if (vacstmt->vacuum)
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "VACUUM%s",
				 vacstmt->analyze ? " ANALYZE" : "");
	else
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "ANALYZE");

	/*
	 * Report the qualified name of the relation.
	 *
	 * Paranoia is appropriate here in case relation was recently dropped
	 * --- the lsyscache routines we just invoked will return NULL rather
	 * than failing.
	 */
	if (relname && nspname)
	{
		int			len = strlen(activity);

		snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
				 " %s.%s", nspname, relname);
	}

	pgstat_report_activity(activity);
}

/*
 * AutoVacuumingActive
 *		Check GUC vars and report whether the autovacuum process should be
 *		running.
 */
bool
AutoVacuumingActive(void)
{
	if (!autovacuum_start_daemon || !pgstat_collect_startcollector ||
		!pgstat_collect_tuplelevel)
		return false;
	return true;
}

/*
 * autovac_init
 *		This is called at postmaster initialization.
 *
 * Annoy the user if he got it wrong.
 */
void
autovac_init(void)
{
	if (!autovacuum_start_daemon)
		return;

	if (!pgstat_collect_startcollector || !pgstat_collect_tuplelevel)
	{
		ereport(WARNING,
				(errmsg("autovacuum not started because of misconfiguration"),
				 errhint("Enable options \"stats_start_collector\" and \"stats_row_level\".")));

		/*
		 * Set the GUC var so we don't fork autovacuum uselessly, and also to
		 * help debugging.
		 */
		autovacuum_start_daemon = false;
	}
}

/*
 * IsAutoVacuumProcess
 *		Return whether this process is an autovacuum process.
 */
bool
IsAutoVacuumProcess(void)
{
	return am_autovacuum;
}
