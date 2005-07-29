/*-------------------------------------------------------------------------
 *
 * autovacuum.c
 *
 * PostgreSQL Integrated Autovacuum Daemon
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/postmaster/autovacuum.c,v 1.2 2005/07/29 19:30:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xlog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_autovacuum.h"
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
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/relcache.h"


/*
 * GUC parameters
 */
bool		autovacuum_start_daemon = false;
int			autovacuum_naptime;
int			autovacuum_vac_thresh;
double		autovacuum_vac_scale;
int			autovacuum_anl_thresh;
double		autovacuum_anl_scale;

/* Flag to tell if we are in the autovacuum daemon process */
static bool am_autovacuum = false;

/* Last time autovac daemon started/stopped (only valid in postmaster) */
static time_t last_autovac_start_time = 0;
static time_t last_autovac_stop_time = 0;

/* struct to keep list of candidate databases for vacuum */
typedef struct autovac_dbase
{
	Oid				oid;
	char		   *name;
	TransactionId	frozenxid;
	PgStat_StatDBEntry *entry;
	int32			age;
} autovac_dbase;


#ifdef EXEC_BACKEND
static pid_t autovac_forkexec(void);
#endif
NON_EXEC_STATIC void AutoVacMain(int argc, char *argv[]);
static void do_autovacuum(bool whole_db, PgStat_StatDBEntry *dbentry);
static List *autovac_get_database_list(void);
static void test_rel_for_autovac(Oid relid, PgStat_StatTabEntry *tabentry,
			 Form_pg_class classForm, Form_pg_autovacuum avForm,
			 List **vacuum_tables, List **analyze_tables);
static void autovacuum_do_vac_analyze(List *relids, bool dovacuum);


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

	/* Do nothing if no autovacuum process needed */
	if (!AutoVacuumingActive())
		return 0;

	/*
	 * Do nothing if too soon since last autovacuum exit.  This limits
	 * how often the daemon runs.  Since the time per iteration can be
	 * quite variable, it seems more useful to measure/control the time
	 * since last subprocess exit than since last subprocess launch.
	 *
	 * However, we *also* check the time since last subprocess launch;
	 * this prevents thrashing under fork-failure conditions.
	 *
	 * Note that since we will be re-called from the postmaster main loop,
	 * we will get another chance later if we do nothing now.
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
	switch((AutoVacPID = autovac_forkexec()))
#else
	switch((AutoVacPID = fork_process()))
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
	av[ac++] = "-forkautovac";
	av[ac++] = NULL;		/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif	/* EXEC_BACKEND */

/*
 * AutoVacMain
 */
NON_EXEC_STATIC void
AutoVacMain(int argc, char *argv[])
{
	ListCell	   *cell;
	List		   *dblist;
	TransactionId	nextXid;
	autovac_dbase  *db;
	bool			whole_db;
	sigjmp_buf		local_sigjmp_buf;

	/* we are a postmaster subprocess now */
	IsUnderPostmaster = true;
	am_autovacuum = true;

	/* reset MyProcPid */
	MyProcPid = getpid();

	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

	/*
	 * Set up signal handlers.  We operate on databases much like a
	 * regular backend, so we use the same signal handling.  See
	 * equivalent code in tcop/postgres.c.
	 *
	 * Currently, we don't pay attention to postgresql.conf changes
	 * that happen during a single daemon iteration, so we can ignore
	 * SIGHUP.
	 */
	pqsignal(SIGHUP, SIG_IGN);
	/*
	 * Presently, SIGINT will lead to autovacuum shutdown, because that's
	 * how we handle ereport(ERROR).  It could be improved however.
	 */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	pqsignal(SIGQUIT, quickdie);
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, CatchupInterruptHandler);
	/* We don't listen for async notifies */
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Identify myself via ps */
	init_ps_display("autovacuum process", "", "");
	set_ps_display("");

	/* Early initialization */
	BaseInit();

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
		 * We can now go away.  Note that because we'll call InitProcess,
		 * a callback will be registered to do ProcKill, which will clean
		 * up necessary state.
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	/* Get a list of databases */
	dblist = autovac_get_database_list();

	/*
	 * Get the next Xid that was current as of the last checkpoint.
	 * We need it to determine whether databases are about to need
	 * database-wide vacuums.
	 */
	nextXid = GetRecentNextXid();

	/*
	 * Choose a database to connect to.  We pick the database that was least
	 * recently auto-vacuumed, or one that needs database-wide vacuum (to
	 * prevent Xid wraparound-related data loss).
	 *
	 * Note that a database with no stats entry is not considered, except
	 * for Xid wraparound purposes.  The theory is that if no one has ever
	 * connected to it since the stats were last initialized, it doesn't
	 * need vacuuming.
	 *
	 * XXX This could be improved if we had more info about whether it needs
	 * vacuuming before connecting to it.  Perhaps look through the pgstats
	 * data for the database's tables?  One idea is to keep track of the
	 * number of new and dead tuples per database in pgstats.  However it
	 * isn't clear how to construct a metric that measures that and not
	 * cause starvation for less busy databases.
	 */
	db = NULL;
	whole_db = false;

	foreach(cell, dblist)
	{
		autovac_dbase  *tmp = lfirst(cell);
		bool			this_whole_db;

		/*
		 * We look for the database that most urgently needs a database-wide
		 * vacuum.  We decide that a database-wide vacuum is needed 100000
		 * transactions sooner than vacuum.c's vac_truncate_clog() would
		 * decide to start giving warnings.  If any such db is found, we
		 * ignore all other dbs.
		 */
		tmp->age = (int32) (nextXid - tmp->frozenxid);
		this_whole_db = (tmp->age > (int32) ((MaxTransactionId >> 3) * 3 - 100000));
		if (whole_db || this_whole_db)
		{
			if (!this_whole_db)
				continue;
			if (db == NULL || tmp->age > db->age)
			{
				db = tmp;
				whole_db = true;
			}
			continue;
		}

		/*
		 * Otherwise, skip a database with no pgstat entry; it means it hasn't
		 * seen any activity.
		 */
		tmp->entry = pgstat_fetch_stat_dbentry(tmp->oid);
		if (!tmp->entry)
			continue;

		/*
		 * Don't try to access a database that was dropped.  This could only
		 * happen if we read the pg_database flat file right before it was
		 * modified, after the database was dropped from the pg_database
		 * table.  (This is of course a not-very-bulletproof test, but it's
		 * cheap to make.  If we do mistakenly choose a recently dropped
		 * database, InitPostgres will fail and we'll drop out until the
		 * next autovac run.)
		 */
		if (tmp->entry->destroy != 0)
			continue;

		/*
		 * Else remember the db with oldest autovac time.
		 */
		if (db == NULL ||
			tmp->entry->last_autovac_time < db->entry->last_autovac_time)
			db = tmp;
	}

	if (db)
	{
		/*
		 * Connect to the selected database
		 */
		InitPostgres(db->name, NULL);
		SetProcessingMode(NormalProcessing);
		pgstat_report_autovac();
		set_ps_display(db->name);
		ereport(LOG,
				(errmsg("autovacuum: processing database \"%s\"", db->name)));
		/*
		 * And do an appropriate amount of work on it
		 */
		do_autovacuum(whole_db, db->entry);
	}

	/* One iteration done, go away */
	proc_exit(0);
}

/*
 * autovac_get_database_list
 *
 * 		Return a list of all databases.  Note we cannot use pg_database,
 *		because we aren't connected yet; we use the flat database file.
 */
static List *
autovac_get_database_list(void)
{
	char   *filename;
	List   *dblist = NIL;
	char	thisname[NAMEDATALEN];
	FILE   *db_file;
	Oid		db_id;
	Oid		db_tablespace;
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
		autovac_dbase	*db;

		db = (autovac_dbase *) palloc(sizeof(autovac_dbase));

		db->oid = db_id;
		db->name = pstrdup(thisname);
		db->frozenxid = db_frozenxid;
		/* these get set later: */
		db->entry = NULL;
		db->age = 0;

		dblist = lappend(dblist, db);
	}

	FreeFile(db_file);
	pfree(filename);

	return dblist;
}

/*
 * Process a database.
 *
 * If whole_db is true, the database is processed as a whole, and the
 * dbentry parameter is ignored.  If it's false, dbentry must be a valid
 * pointer to the database entry in the stats databases' hash table, and
 * it will be used to determine whether vacuum or analyze is needed on a
 * per-table basis.
 *
 * Note that test_rel_for_autovac generates two separate lists, one for
 * vacuum and other for analyze.  This is to facilitate processing all
 * analyzes first, and then all vacuums.
 *
 * Note that CHECK_FOR_INTERRUPTS is supposed to be used in certain spots in
 * order not to ignore shutdown commands for too long.
 */
static void
do_autovacuum(bool whole_db, PgStat_StatDBEntry *dbentry)
{
	Relation		classRel,
					avRel;
	HeapTuple		tuple;
	HeapScanDesc	relScan;
	List		   *vacuum_tables = NIL,
				   *analyze_tables = NIL;
	MemoryContext	AutovacMemCxt;

	Assert(whole_db || PointerIsValid(dbentry));

	/* Memory context where cross-transaction state is stored */
	AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Autovacuum context",
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_INITSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);

	/* Start a transaction so our commands have one to play into. */
	StartTransactionCommand();

	/*
	 * StartTransactionCommand and CommitTransactionCommand will
	 * automatically switch to other contexts.  We need this one
	 * to keep the list of relations to vacuum/analyze across
	 * transactions.
	 */
	MemoryContextSwitchTo(AutovacMemCxt);

	if (whole_db)
	{
		elog(DEBUG2, "autovacuum: VACUUM ANALYZE whole database");
		autovacuum_do_vac_analyze(NIL, true);
	}
	else
	{
		/* the hash entry where pgstat stores shared relations */
		PgStat_StatDBEntry *shared = pgstat_fetch_stat_dbentry(InvalidOid);

		classRel = heap_open(RelationRelationId, AccessShareLock);
		avRel = heap_open(AutovacuumRelationId, AccessShareLock);

		relScan = heap_beginscan(classRel, SnapshotNow, 0, NULL);

		/* Scan pg_class looking for tables to vacuum */
		while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
			Form_pg_autovacuum avForm = NULL;
			PgStat_StatTabEntry *tabentry;
			SysScanDesc	avScan;
			HeapTuple	avTup;
			ScanKeyData	entry[1];
			Oid			relid;

			/* Skip non-table entries. */
			/* XXX possibly allow RELKIND_TOASTVALUE entries here too? */
			if (classForm->relkind != RELKIND_RELATION)
				continue;

			/*
			 * Skip temp tables (i.e. those in temp namespaces).  We cannot
			 * safely process other backends' temp tables.
			 */
			if (isTempNamespace(classForm->relnamespace))
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
			else
				tabentry = hash_search(dbentry->tables, &relid,
									   HASH_FIND, NULL);

			test_rel_for_autovac(relid, tabentry, classForm, avForm,
								 &vacuum_tables, &analyze_tables);

			systable_endscan(avScan);
		}

		heap_endscan(relScan);
		heap_close(avRel, AccessShareLock);
		heap_close(classRel, AccessShareLock);

		CHECK_FOR_INTERRUPTS();

		/*
		 * Perform operations on collected tables.
		 */

		if (analyze_tables)
			autovacuum_do_vac_analyze(analyze_tables, false);

		CHECK_FOR_INTERRUPTS();

		/* get back to proper context */
		MemoryContextSwitchTo(AutovacMemCxt);

		if (vacuum_tables)
			autovacuum_do_vac_analyze(vacuum_tables, true);
	}

	/* Finally close out the last transaction. */
	CommitTransactionCommand();
}

/*
 * test_rel_for_autovac
 *
 * Check whether a table needs to be vacuumed or analyzed.  Add it to the
 * respective list if so.
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
 * A table whose pg_autovacuum.enabled value is false, is automatically
 * skipped.  Thus autovacuum can be disabled for specific tables.  Also,
 * when the stats collector does not have data about a table, it will be
 * skipped.
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
					 List **vacuum_tables, List **analyze_tables)
{
	Relation		rel;
	float4			reltuples;	/* pg_class.reltuples */
	/* constants from pg_autovacuum or GUC variables */
	int				vac_base_thresh,
					anl_base_thresh;
	float4			vac_scale_factor,
					anl_scale_factor;
	/* thresholds calculated from above constants */
	float4			vacthresh,
					anlthresh;
	/* number of vacuum (resp. analyze) tuples at this time */
	float4			vactuples,
					anltuples;

	/* User disabled it in pg_autovacuum? */
	if (avForm && !avForm->enabled)
		return;

	/*
	 * Skip a table not found in stat hash.  If it's not acted upon,
	 * there's no need to vacuum it.  (Note that database-level check
	 * will take care of Xid wraparound.)
	 */
	if (!PointerIsValid(tabentry))
		return;

	rel = RelationIdGetRelation(relid);
	/* The table was recently dropped? */
	if (!PointerIsValid(rel))
		return;

	reltuples = rel->rd_rel->reltuples;
	vactuples = tabentry->n_dead_tuples;
	anltuples = tabentry->n_live_tuples + tabentry->n_dead_tuples -
		tabentry->last_anl_tuples;

	/*
	 * If there is a tuple in pg_autovacuum, use it; else, use the GUC
	 * defaults.  Note that the fields may contain "-1" (or indeed any
	 * negative value), which means use the GUC defaults for each setting.
	 */
	if (avForm != NULL)
	{
		vac_scale_factor = (avForm->vac_scale_factor < 0) ?
			autovacuum_vac_scale : avForm->vac_scale_factor;
		vac_base_thresh = (avForm->vac_base_thresh < 0) ?
			autovacuum_vac_thresh : avForm->vac_base_thresh;

		anl_scale_factor = (avForm->anl_scale_factor < 0) ?
			autovacuum_anl_scale : avForm->anl_scale_factor;
		anl_base_thresh = (avForm->anl_base_thresh < 0) ?
			autovacuum_anl_thresh : avForm->anl_base_thresh;
	}
	else
	{
		vac_scale_factor = autovacuum_vac_scale;
		vac_base_thresh = autovacuum_vac_thresh;

		anl_scale_factor = autovacuum_anl_scale;
		anl_base_thresh = autovacuum_anl_thresh;
	}

	vacthresh = (float4) vac_base_thresh + vac_scale_factor * reltuples;
	anlthresh = (float4) anl_base_thresh + anl_scale_factor * reltuples;

	/*
	 * Note that we don't need to take special consideration for stat
	 * reset, because if that happens, the last vacuum and analyze counts
	 * will be reset too.
	 */

	elog(DEBUG2, "%s: vac: %.0f (threshold %.0f), anl: %.0f (threshold %.0f)",
		 RelationGetRelationName(rel),
		 vactuples, vacthresh, anltuples, anlthresh);

	/* Determine if this table needs vacuum or analyze. */
	if (vactuples > vacthresh)
	{
		elog(DEBUG2, "will VACUUM ANALYZE %s",
			 RelationGetRelationName(rel));
		*vacuum_tables = lappend_oid(*vacuum_tables, relid);
	}
	else if (anltuples > anlthresh)
	{
		/* ANALYZE refuses to work with pg_statistics */
		if (relid != StatisticRelationId)
		{
			elog(DEBUG2, "will ANALYZE %s",
					RelationGetRelationName(rel));
			*analyze_tables = lappend_oid(*analyze_tables, relid);
		}
	}

	RelationClose(rel);
}

/*
 * autovacuum_do_vac_analyze
 * 		Vacuum or analyze a list of tables; or all tables if relids = NIL
 *
 * We must be in AutovacMemCxt when this routine is called.
 */
static void
autovacuum_do_vac_analyze(List *relids, bool dovacuum)
{
	VacuumStmt		*vacstmt = makeNode(VacuumStmt);

	/*
	 * Point QueryContext to the autovac memory context to fake out the
	 * PreventTransactionChain check inside vacuum().  Note that this
	 * is also why we palloc vacstmt instead of just using a local variable.
	 */
	QueryContext = CurrentMemoryContext;

	/* Set up command parameters */
	vacstmt->vacuum = dovacuum;
	vacstmt->full = false;
	vacstmt->analyze = true;
	vacstmt->freeze = false;
	vacstmt->verbose = false;
	vacstmt->relation = NULL;	/* all tables, or not used if relids != NIL */
	vacstmt->va_cols = NIL;

	vacuum(vacstmt, relids);
}

/*
 * AutoVacuumingActive
 * 		Check GUC vars and report whether the autovacuum process should be
 * 		running.
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
 * 		This is called at postmaster initialization.
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
 * 		Return whether this process is an autovacuum process.
 */
bool
IsAutoVacuumProcess(void)
{
	return am_autovacuum;
}
