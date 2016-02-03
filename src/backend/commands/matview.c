/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  materialized view support
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "commands/cluster.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Oid			transientoid;	/* OID of new heap into which to store */
	/* These fields are filled by transientrel_startup: */
	Relation	transientrel;	/* relation to write to */
	CommandId	output_cid;		/* cmin to insert in output tuples */
	int			hi_options;		/* heap_insert performance options */
	BulkInsertState bistate;	/* bulk insert state */
} DR_transientrel;

static int	matview_maintenance_depth = 0;

static void transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static void transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);
static void refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString);

static char *make_temptable_name_n(char *tempname, int n);
static void mv_GenerateOper(StringInfo buf, Oid opoid);

static void refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
					   int save_sec_context);
static void refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence);

static void OpenMatViewIncrementalMaintenance(void);
static void CloseMatViewIncrementalMaintenance(void);

/*
 * SetMatViewPopulatedState
 *		Mark a materialized view as populated, or not.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 */
void
SetMatViewPopulatedState(Relation relation, bool newstate)
{
	Relation	pgrel;
	HeapTuple	tuple;

	Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 */
	pgrel = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(relation));

	((Form_pg_class) GETSTRUCT(tuple))->relispopulated = newstate;

	simple_heap_update(pgrel, &tuple->t_self, tuple);

	CatalogUpdateIndexes(pgrel, tuple);

	heap_freetuple(tuple);
	heap_close(pgrel, RowExclusiveLock);

	/*
	 * Advance command counter to make the updated pg_class row locally
	 * visible.
	 */
	CommandCounterIncrement();
}

/*
 * ExecRefreshMatView -- execute a REFRESH MATERIALIZED VIEW command
 *
 * This refreshes the materialized view by creating a new table and swapping
 * the relfilenodes of the new table and the old materialized view, so the OID
 * of the original materialized view is preserved. Thus we do not lose GRANT
 * nor references to this materialized view.
 *
 * If WITH NO DATA was specified, this is effectively like a TRUNCATE;
 * otherwise it is like a TRUNCATE followed by an INSERT using the SELECT
 * statement associated with the materialized view.  The statement node's
 * skipData field shows whether the clause was used.
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new heap, it's better to create the indexes afterwards than to fill them
 * incrementally while we load.
 *
 * The matview's "populated" state is changed based on whether the contents
 * reflect the result set of the materialized view's query.
 */
ObjectAddress
ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				   ParamListInfo params, char *completionTag)
{
	Oid			matviewOid;
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;
	Oid			tableSpace;
	Oid			relowner;
	Oid			OIDNewHeap;
	DestReceiver *dest;
	bool		concurrent;
	LOCKMODE	lockmode;
	char		relpersistence;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	ObjectAddress address;

	/* Determine strength of lock needed. */
	concurrent = stmt->concurrent;
	lockmode = concurrent ? ExclusiveLock : AccessExclusiveLock;

	/*
	 * Get a lock until end of transaction.
	 */
	matviewOid = RangeVarGetRelidExtended(stmt->relation,
										  lockmode, false, false,
										  RangeVarCallbackOwnsTable, NULL);
	matviewRel = heap_open(matviewOid, NoLock);

	/* Make sure it is a materialized view. */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/* Check that CONCURRENTLY is not specified if not populated. */
	if (concurrent && !RelationIsPopulated(matviewRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CONCURRENTLY cannot be used when the materialized view is not populated")));

	/* Check that conflicting options have not been specified. */
	if (concurrent && stmt->skipData)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("CONCURRENTLY and WITH NO DATA options cannot be used together")));

	/* We don't allow an oid column for a materialized view. */
	Assert(!matviewRel->rd_rel->relhasoids);

	/*
	 * Check that everything is correct for a refresh. Problems at this point
	 * are internal errors, so elog is sufficient.
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	/*
	 * The stored query was rewritten at the time of the MV definition, but
	 * has not been scribbled on by the planner.
	 */
	dataQuery = (Query *) linitial(actions);
	Assert(IsA(dataQuery, Query));

	/*
	 * Check for active uses of the relation in the current transaction, such
	 * as open scans.
	 *
	 * NB: We count on this to protect us against problems with refreshing the
	 * data using HEAP_INSERT_FROZEN.
	 */
	CheckTableNotInUse(matviewRel, "REFRESH MATERIALIZED VIEW");

	/*
	 * Tentatively mark the matview as populated or not (this will roll back
	 * if we fail later).
	 */
	SetMatViewPopulatedState(matviewRel, !stmt->skipData);

	relowner = matviewRel->rd_rel->relowner;

	/*
	 * Switch to the owner's userid, so that any functions are run as that
	 * user.  Also arrange to make GUC variable changes local to this command.
	 * Don't lock it down too tight to create a temporary table just yet.  We
	 * will switch modes when we are about to execute user code.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	save_nestlevel = NewGUCNestLevel();

	/* Concurrent refresh builds new data in temp tablespace, and does diff. */
	if (concurrent)
	{
		tableSpace = GetDefaultTablespace(RELPERSISTENCE_TEMP);
		relpersistence = RELPERSISTENCE_TEMP;
	}
	else
	{
		tableSpace = matviewRel->rd_rel->reltablespace;
		relpersistence = matviewRel->rd_rel->relpersistence;
	}

	/*
	 * Create the transient table that will receive the regenerated data. Lock
	 * it against access by any other process until commit (by which time it
	 * will be gone).
	 */
	OIDNewHeap = make_new_heap(matviewOid, tableSpace, relpersistence,
							   ExclusiveLock);
	LockRelationOid(OIDNewHeap, AccessExclusiveLock);
	dest = CreateTransientRelDestReceiver(OIDNewHeap);

	/*
	 * Now lock down security-restricted operations.
	 */
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);

	/* Generate the data, if wanted. */
	if (!stmt->skipData)
		refresh_matview_datafill(dest, dataQuery, queryString);

	heap_close(matviewRel, NoLock);

	/* Make the matview match the newly generated data. */
	if (concurrent)
	{
		int			old_depth = matview_maintenance_depth;

		PG_TRY();
		{
			refresh_by_match_merge(matviewOid, OIDNewHeap, relowner,
								   save_sec_context);
		}
		PG_CATCH();
		{
			matview_maintenance_depth = old_depth;
			PG_RE_THROW();
		}
		PG_END_TRY();
		Assert(matview_maintenance_depth == old_depth);
	}
	else
		refresh_by_heap_swap(matviewOid, OIDNewHeap, relpersistence);

	/* Roll back any GUC changes */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	ObjectAddressSet(address, RelationRelationId, matviewOid);

	return address;
}

/*
 * refresh_matview_datafill
 */
static void
refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString)
{
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	Query	   *copied_query;

	/* Lock and rewrite, using a copy to preserve the original query. */
	copied_query = copyObject(query);
	AcquireRewriteLocks(copied_query, true, false);
	rewritten = QueryRewrite(copied_query);

	/* SELECT should never rewrite to more or less than one SELECT query */
	if (list_length(rewritten) != 1)
		elog(ERROR, "unexpected rewrite result for REFRESH MATERIALIZED VIEW");
	query = (Query *) linitial(rewritten);

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/* Plan the query which will generate data for the refresh. */
	plan = pg_plan_query(query, 0, NULL);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.  (This could only matter if
	 * the planner executed an allegedly-stable function that changed the
	 * database contents, but let's do it anyway to be safe.)
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* Create a QueryDesc, redirecting output to our tuple receiver */
	queryDesc = CreateQueryDesc(plan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, NULL, 0);

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, EXEC_FLAG_WITHOUT_OIDS);

	/* run the plan */
	ExecutorRun(queryDesc, ForwardScanDirection, 0L);

	/* and clean up */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();
}

DestReceiver *
CreateTransientRelDestReceiver(Oid transientoid)
{
	DR_transientrel *self = (DR_transientrel *) palloc0(sizeof(DR_transientrel));

	self->pub.receiveSlot = transientrel_receive;
	self->pub.rStartup = transientrel_startup;
	self->pub.rShutdown = transientrel_shutdown;
	self->pub.rDestroy = transientrel_destroy;
	self->pub.mydest = DestTransientRel;
	self->transientoid = transientoid;

	return (DestReceiver *) self;
}

/*
 * transientrel_startup --- executor startup
 */
static void
transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Relation	transientrel;

	transientrel = heap_open(myState->transientoid, NoLock);

	/*
	 * Fill private fields of myState for use by later routines
	 */
	myState->transientrel = transientrel;
	myState->output_cid = GetCurrentCommandId(true);

	/*
	 * We can skip WAL-logging the insertions, unless PITR or streaming
	 * replication is in use. We can skip the FSM in any case.
	 */
	myState->hi_options = HEAP_INSERT_SKIP_FSM | HEAP_INSERT_FROZEN;
	if (!XLogIsNeeded())
		myState->hi_options |= HEAP_INSERT_SKIP_WAL;
	myState->bistate = GetBulkInsertState();

	/* Not using WAL requires smgr_targblock be initially invalid */
	Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- receive one tuple
 */
static void
transientrel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	HeapTuple	tuple;

	/*
	 * get the heap tuple out of the tuple table slot, making sure we have a
	 * writable copy
	 */
	tuple = ExecMaterializeSlot(slot);

	heap_insert(myState->transientrel,
				tuple,
				myState->output_cid,
				myState->hi_options,
				myState->bistate);

	/* We know this is a newly created relation, so there are no indexes */
}

/*
 * transientrel_shutdown --- executor end
 */
static void
transientrel_shutdown(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	FreeBulkInsertState(myState->bistate);

	/* If we skipped using WAL, must heap_sync before commit */
	if (myState->hi_options & HEAP_INSERT_SKIP_WAL)
		heap_sync(myState->transientrel);

	/* close transientrel, but keep lock until commit */
	heap_close(myState->transientrel, NoLock);
	myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- release DestReceiver object
 */
static void
transientrel_destroy(DestReceiver *self)
{
	pfree(self);
}


/*
 * Given a qualified temporary table name, append an underscore followed by
 * the given integer, to make a new table name based on the old one.
 *
 * This leaks memory through palloc(), which won't be cleaned up until the
 * current memory context is freed.
 */
static char *
make_temptable_name_n(char *tempname, int n)
{
	StringInfoData namebuf;

	initStringInfo(&namebuf);
	appendStringInfoString(&namebuf, tempname);
	appendStringInfo(&namebuf, "_%i", n);
	return namebuf.data;
}

static void
mv_GenerateOper(StringInfo buf, Oid opoid)
{
	HeapTuple	opertup;
	Form_pg_operator operform;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opoid));
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", opoid);
	operform = (Form_pg_operator) GETSTRUCT(opertup);
	Assert(operform->oprkind == 'b');

	appendStringInfo(buf, "OPERATOR(%s.%s)",
				quote_identifier(get_namespace_name(operform->oprnamespace)),
					 NameStr(operform->oprname));

	ReleaseSysCache(opertup);
}

/*
 * refresh_by_match_merge
 *
 * Refresh a materialized view with transactional semantics, while allowing
 * concurrent reads.
 *
 * This is called after a new version of the data has been created in a
 * temporary table.  It performs a full outer join against the old version of
 * the data, producing "diff" results.  This join cannot work if there are any
 * duplicated rows in either the old or new versions, in the sense that every
 * column would compare as equal between the two rows.  It does work correctly
 * in the face of rows which have at least one NULL value, with all non-NULL
 * columns equal.  The behavior of NULLs on equality tests and on UNIQUE
 * indexes turns out to be quite convenient here; the tests we need to make
 * are consistent with default behavior.  If there is at least one UNIQUE
 * index on the materialized view, we have exactly the guarantee we need.
 *
 * The temporary table used to hold the diff results contains just the TID of
 * the old record (if matched) and the ROW from the new table as a single
 * column of complex record type (if matched).
 *
 * Once we have the diff table, we perform set-based DELETE and INSERT
 * operations against the materialized view, and discard both temporary
 * tables.
 *
 * Everything from the generation of the new data to applying the differences
 * takes place under cover of an ExclusiveLock, since it seems as though we
 * would want to prohibit not only concurrent REFRESH operations, but also
 * incremental maintenance.  It also doesn't seem reasonable or safe to allow
 * SELECT FOR UPDATE or SELECT FOR SHARE on rows being updated or deleted by
 * this command.
 */
static void
refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
					   int save_sec_context)
{
	StringInfoData querybuf;
	Relation	matviewRel;
	Relation	tempRel;
	char	   *matviewname;
	char	   *tempname;
	char	   *diffname;
	TupleDesc	tupdesc;
	bool		foundUniqueIndex;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int16		relnatts;
	bool	   *usedForQual;

	initStringInfo(&querybuf);
	matviewRel = heap_open(matviewOid, NoLock);
	matviewname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
										RelationGetRelationName(matviewRel));
	tempRel = heap_open(tempOid, NoLock);
	tempname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(tempRel)),
										  RelationGetRelationName(tempRel));
	diffname = make_temptable_name_n(tempname, 2);

	relnatts = matviewRel->rd_rel->relnatts;
	usedForQual = (bool *) palloc0(sizeof(bool) * relnatts);

	/* Open SPI context. */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/* Analyze the temp table with the new contents. */
	appendStringInfo(&querybuf, "ANALYZE %s", tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * We need to ensure that there are not duplicate rows without NULLs in
	 * the new data set before we can count on the "diff" results.  Check for
	 * that in a way that allows showing the first duplicated row found.  Even
	 * after we pass this test, a unique index on the materialized view may
	 * find a duplicate key problem.
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "SELECT newdata FROM %s newdata "
					 "WHERE newdata IS NOT NULL AND EXISTS "
					 "(SELECT * FROM %s newdata2 WHERE newdata2 IS NOT NULL "
					 "AND newdata2 OPERATOR(pg_catalog.*=) newdata "
					 "AND newdata2.ctid OPERATOR(pg_catalog.<>) "
					 "newdata.ctid) LIMIT 1",
					 tempname, tempname);
	if (SPI_execute(querybuf.data, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	if (SPI_processed > 0)
	{
		/*
		 * Note that this ereport() is returning data to the user.  Generally,
		 * we would want to make sure that the user has been granted access to
		 * this data.  However, REFRESH MAT VIEW is only able to be run by the
		 * owner of the mat view (or a superuser) and therefore there is no
		 * need to check for access to data in the mat view.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("new data for materialized view \"%s\" contains duplicate rows without any null columns",
						RelationGetRelationName(matviewRel)),
				 errdetail("Row: %s",
			SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1))));
	}

	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	/* Start building the query for creating the diff table. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "CREATE TEMP TABLE %s AS "
					 "SELECT mv.ctid AS tid, newdata "
					 "FROM %s mv FULL JOIN %s newdata ON (",
					 diffname, matviewname, tempname);

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache.  We will test for equality on all
	 * columns present in all unique indexes which only reference columns and
	 * include all rows.
	 */
	tupdesc = matviewRel->rd_att;
	foundUniqueIndex = false;
	indexoidlist = RelationGetIndexList(matviewRel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;
		Form_pg_index indexStruct;

		indexRel = index_open(indexoid, RowExclusiveLock);
		indexStruct = indexRel->rd_index;

		/*
		 * We're only interested if it is unique, valid, contains no
		 * expressions, and is not partial.
		 */
		if (indexStruct->indisunique &&
			IndexIsValid(indexStruct) &&
			RelationGetIndexExpressions(indexRel) == NIL &&
			RelationGetIndexPredicate(indexRel) == NIL)
		{
			int			numatts = indexStruct->indnatts;
			int			i;

			/* Add quals for all columns from this index. */
			for (i = 0; i < numatts; i++)
			{
				int			attnum = indexStruct->indkey.values[i];
				Oid			type;
				Oid			op;
				const char *colname;

				/*
				 * Only include the column once regardless of how many times
				 * it shows up in how many indexes.
				 */
				if (usedForQual[attnum - 1])
					continue;
				usedForQual[attnum - 1] = true;

				/*
				 * Actually add the qual, ANDed with any others.
				 */
				if (foundUniqueIndex)
					appendStringInfoString(&querybuf, " AND ");

				colname = quote_identifier(NameStr((tupdesc->attrs[attnum - 1])->attname));
				appendStringInfo(&querybuf, "newdata.%s ", colname);
				type = attnumTypeId(matviewRel, attnum);
				op = lookup_type_cache(type, TYPECACHE_EQ_OPR)->eq_opr;
				mv_GenerateOper(&querybuf, op);
				appendStringInfo(&querybuf, " mv.%s", colname);

				foundUniqueIndex = true;
			}
		}

		/* Keep the locks, since we're about to run DML which needs them. */
		index_close(indexRel, NoLock);
	}

	list_free(indexoidlist);

	if (!foundUniqueIndex)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			   errmsg("cannot refresh materialized view \"%s\" concurrently",
					  matviewname),
				 errhint("Create a unique index with no WHERE clause on one or more columns of the materialized view.")));

	appendStringInfoString(&querybuf,
						   " AND newdata OPERATOR(pg_catalog.*=) mv) "
						   "WHERE newdata IS NULL OR mv IS NULL "
						   "ORDER BY tid");

	/* Create the temporary "diff" table. */
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);

	/*
	 * We have no further use for data from the "full-data" temp table, but we
	 * must keep it around because its type is referenced from the diff table.
	 */

	/* Analyze the diff table. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "ANALYZE %s", diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	OpenMatViewIncrementalMaintenance();

	/* Deletes must come before inserts; do them first. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
				   "DELETE FROM %s mv WHERE ctid OPERATOR(pg_catalog.=) ANY "
					 "(SELECT diff.tid FROM %s diff "
					 "WHERE diff.tid IS NOT NULL "
					 "AND diff.newdata IS NULL)",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_DELETE)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Inserts go last. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s SELECT (diff.newdata).* "
					 "FROM %s diff WHERE tid IS NULL",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* We're done maintaining the materialized view. */
	CloseMatViewIncrementalMaintenance();
	heap_close(tempRel, NoLock);
	heap_close(matviewRel, NoLock);

	/* Clean up temp tables. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "DROP TABLE %s, %s", diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Close SPI context. */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}

/*
 * Swap the physical files of the target and transient tables, then rebuild
 * the target's indexes and throw away the transient table.  Security context
 * swapping is handled by the called function, so it is not needed here.
 */
static void
refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence)
{
	finish_heap_swap(matviewOid, OIDNewHeap, false, false, true, true,
					 RecentXmin, ReadNextMultiXactId(), relpersistence);
}


/*
 * This should be used to test whether the backend is in a context where it is
 * OK to allow DML statements to modify materialized views.  We only want to
 * allow that for internal code driven by the materialized view definition,
 * not for arbitrary user-supplied code.
 *
 * While the function names reflect the fact that their main intended use is
 * incremental maintenance of materialized views (in response to changes to
 * the data in referenced relations), they are initially used to allow REFRESH
 * without blocking concurrent reads.
 */
bool
MatViewIncrementalMaintenanceIsEnabled(void)
{
	return matview_maintenance_depth > 0;
}

static void
OpenMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth++;
}

static void
CloseMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth--;
	Assert(matview_maintenance_depth >= 0);
}
