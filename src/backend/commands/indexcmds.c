/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/indexcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_am.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/pg_rusage.h"
#include "utils/regproc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static bool CompareOpclassOptions(Datum *opts1, Datum *opts2, int natts);
static void CheckPredicate(Expr *predicate);
static void ComputeIndexAttrs(IndexInfo *indexInfo,
							  Oid *typeOidP,
							  Oid *collationOidP,
							  Oid *classOidP,
							  int16 *colOptionP,
							  List *attList,
							  List *exclusionOpNames,
							  Oid relId,
							  const char *accessMethodName, Oid accessMethodId,
							  bool amcanorder,
							  bool isconstraint,
							  Oid ddl_userid,
							  int ddl_sec_context,
							  int *ddl_save_nestlevel);
static char *ChooseIndexName(const char *tabname, Oid namespaceId,
							 List *colnames, List *exclusionOpNames,
							 bool primary, bool isconstraint);
static char *ChooseIndexNameAddition(List *colnames);
static List *ChooseIndexColumnNames(List *indexElems);
static void ReindexIndex(RangeVar *indexRelation, ReindexParams *params,
						 bool isTopLevel);
static void RangeVarCallbackForReindexIndex(const RangeVar *relation,
											Oid relId, Oid oldRelId, void *arg);
static Oid	ReindexTable(RangeVar *relation, ReindexParams *params,
						 bool isTopLevel);
static void ReindexMultipleTables(const char *objectName,
								  ReindexObjectType objectKind, ReindexParams *params);
static void reindex_error_callback(void *args);
static void ReindexPartitions(Oid relid, ReindexParams *params,
							  bool isTopLevel);
static void ReindexMultipleInternal(List *relids,
									ReindexParams *params);
static bool ReindexRelationConcurrently(Oid relationOid,
										ReindexParams *params);
static void update_relispartition(Oid relationId, bool newval);
static inline void set_indexsafe_procflags(void);

/*
 * callback argument type for RangeVarCallbackForReindexIndex()
 */
struct ReindexIndexCallbackState
{
	ReindexParams params;		/* options from statement */
	Oid			locked_table_oid;	/* tracks previously locked table */
};

/*
 * callback arguments for reindex_error_callback()
 */
typedef struct ReindexErrorInfo
{
	char	   *relname;
	char	   *relnamespace;
	char		relkind;
} ReindexErrorInfo;

/*
 * CheckIndexCompatible
 *		Determine whether an existing index definition is compatible with a
 *		prospective index definition, such that the existing index storage
 *		could become the storage of the new index, avoiding a rebuild.
 *
 * 'oldId': the OID of the existing index
 * 'accessMethodName': name of the AM to use.
 * 'attributeList': a list of IndexElem specifying columns and expressions
 *		to index on.
 * 'exclusionOpNames': list of names of exclusion-constraint operators,
 *		or NIL if not an exclusion constraint.
 *
 * This is tailored to the needs of ALTER TABLE ALTER TYPE, which recreates
 * any indexes that depended on a changing column from their pg_get_indexdef
 * or pg_get_constraintdef definitions.  We omit some of the sanity checks of
 * DefineIndex.  We assume that the old and new indexes have the same number
 * of columns and that if one has an expression column or predicate, both do.
 * Errors arising from the attribute list still apply.
 *
 * Most column type changes that can skip a table rewrite do not invalidate
 * indexes.  We acknowledge this when all operator classes, collations and
 * exclusion operators match.  Though we could further permit intra-opfamily
 * changes for btree and hash indexes, that adds subtle complexity with no
 * concrete benefit for core types. Note, that INCLUDE columns aren't
 * checked by this function, for them it's enough that table rewrite is
 * skipped.
 *
 * When a comparison or exclusion operator has a polymorphic input type, the
 * actual input types must also match.  This defends against the possibility
 * that operators could vary behavior in response to get_fn_expr_argtype().
 * At present, this hazard is theoretical: check_exclusion_constraint() and
 * all core index access methods decline to set fn_expr for such calls.
 *
 * We do not yet implement a test to verify compatibility of expression
 * columns or predicates, so assume any such index is incompatible.
 */
bool
CheckIndexCompatible(Oid oldId,
					 const char *accessMethodName,
					 List *attributeList,
					 List *exclusionOpNames)
{
	bool		isconstraint;
	Oid		   *typeObjectId;
	Oid		   *collationObjectId;
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
	HeapTuple	tuple;
	Form_pg_index indexForm;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	int			numberOfAttributes;
	int			old_natts;
	bool		isnull;
	bool		ret = true;
	oidvector  *old_indclass;
	oidvector  *old_indcollation;
	Relation	irel;
	int			i;
	Datum		d;

	/* Caller should already have the relation locked in some way. */
	relationId = IndexGetRelation(oldId, false);

	/*
	 * We can pretend isconstraint = false unconditionally.  It only serves to
	 * decide the text of an error message that should never happen for us.
	 */
	isconstraint = false;

	numberOfAttributes = list_length(attributeList);
	Assert(numberOfAttributes > 0);
	Assert(numberOfAttributes <= INDEX_MAX_KEYS);

	/* look up the access method */
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						accessMethodName)));
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);
	ReleaseSysCache(tuple);

	amcanorder = amRoutine->amcanorder;

	/*
	 * Compute the operator classes, collations, and exclusion operators for
	 * the new index, so we can test whether it's compatible with the existing
	 * one.  Note that ComputeIndexAttrs might fail here, but that's OK:
	 * DefineIndex would have failed later.  Our attributeList contains only
	 * key attributes, thus we're filling ii_NumIndexAttrs and
	 * ii_NumIndexKeyAttrs with same value.
	 */
	indexInfo = makeIndexInfo(numberOfAttributes, numberOfAttributes,
							  accessMethodId, NIL, NIL, false, false, false, false);
	typeObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	collationObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	coloptions = (int16 *) palloc(numberOfAttributes * sizeof(int16));
	ComputeIndexAttrs(indexInfo,
					  typeObjectId, collationObjectId, classObjectId,
					  coloptions, attributeList,
					  exclusionOpNames, relationId,
					  accessMethodName, accessMethodId,
					  amcanorder, isconstraint, InvalidOid, 0, NULL);


	/* Get the soon-obsolete pg_index tuple. */
	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", oldId);
	indexForm = (Form_pg_index) GETSTRUCT(tuple);

	/*
	 * We don't assess expressions or predicates; assume incompatibility.
	 * Also, if the index is invalid for any reason, treat it as incompatible.
	 */
	if (!(heap_attisnull(tuple, Anum_pg_index_indpred, NULL) &&
		  heap_attisnull(tuple, Anum_pg_index_indexprs, NULL) &&
		  indexForm->indisvalid))
	{
		ReleaseSysCache(tuple);
		return false;
	}

	/* Any change in operator class or collation breaks compatibility. */
	old_natts = indexForm->indnkeyatts;
	Assert(old_natts == numberOfAttributes);

	d = SysCacheGetAttr(INDEXRELID, tuple, Anum_pg_index_indcollation, &isnull);
	Assert(!isnull);
	old_indcollation = (oidvector *) DatumGetPointer(d);

	d = SysCacheGetAttr(INDEXRELID, tuple, Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	old_indclass = (oidvector *) DatumGetPointer(d);

	ret = (memcmp(old_indclass->values, classObjectId,
				  old_natts * sizeof(Oid)) == 0 &&
		   memcmp(old_indcollation->values, collationObjectId,
				  old_natts * sizeof(Oid)) == 0);

	ReleaseSysCache(tuple);

	if (!ret)
		return false;

	/* For polymorphic opcintype, column type changes break compatibility. */
	irel = index_open(oldId, AccessShareLock);	/* caller probably has a lock */
	for (i = 0; i < old_natts; i++)
	{
		if (IsPolymorphicType(get_opclass_input_type(classObjectId[i])) &&
			TupleDescAttr(irel->rd_att, i)->atttypid != typeObjectId[i])
		{
			ret = false;
			break;
		}
	}

	/* Any change in opclass options break compatibility. */
	if (ret)
	{
		Datum	   *opclassOptions = RelationGetIndexRawAttOptions(irel);

		ret = CompareOpclassOptions(opclassOptions,
									indexInfo->ii_OpclassOptions, old_natts);

		if (opclassOptions)
			pfree(opclassOptions);
	}

	/* Any change in exclusion operator selections breaks compatibility. */
	if (ret && indexInfo->ii_ExclusionOps != NULL)
	{
		Oid		   *old_operators,
				   *old_procs;
		uint16	   *old_strats;

		RelationGetExclusionInfo(irel, &old_operators, &old_procs, &old_strats);
		ret = memcmp(old_operators, indexInfo->ii_ExclusionOps,
					 old_natts * sizeof(Oid)) == 0;

		/* Require an exact input type match for polymorphic operators. */
		if (ret)
		{
			for (i = 0; i < old_natts && ret; i++)
			{
				Oid			left,
							right;

				op_input_types(indexInfo->ii_ExclusionOps[i], &left, &right);
				if ((IsPolymorphicType(left) || IsPolymorphicType(right)) &&
					TupleDescAttr(irel->rd_att, i)->atttypid != typeObjectId[i])
				{
					ret = false;
					break;
				}
			}
		}
	}

	index_close(irel, NoLock);
	return ret;
}

/*
 * CompareOpclassOptions
 *
 * Compare per-column opclass options which are represented by arrays of text[]
 * datums.  Both elements of arrays and array themselves can be NULL.
 */
static bool
CompareOpclassOptions(Datum *opts1, Datum *opts2, int natts)
{
	int			i;

	if (!opts1 && !opts2)
		return true;

	for (i = 0; i < natts; i++)
	{
		Datum		opt1 = opts1 ? opts1[i] : (Datum) 0;
		Datum		opt2 = opts2 ? opts2[i] : (Datum) 0;

		if (opt1 == (Datum) 0)
		{
			if (opt2 == (Datum) 0)
				continue;
			else
				return false;
		}
		else if (opt2 == (Datum) 0)
			return false;

		/* Compare non-NULL text[] datums. */
		if (!DatumGetBool(DirectFunctionCall2(array_eq, opt1, opt2)))
			return false;
	}

	return true;
}

/*
 * WaitForOlderSnapshots
 *
 * Wait for transactions that might have an older snapshot than the given xmin
 * limit, because it might not contain tuples deleted just before it has
 * been taken. Obtain a list of VXIDs of such transactions, and wait for them
 * individually. This is used when building an index concurrently.
 *
 * We can exclude any running transactions that have xmin > the xmin given;
 * their oldest snapshot must be newer than our xmin limit.
 * We can also exclude any transactions that have xmin = zero, since they
 * evidently have no live snapshot at all (and any one they might be in
 * process of taking is certainly newer than ours).  Transactions in other
 * DBs can be ignored too, since they'll never even be able to see the
 * index being worked on.
 *
 * We can also exclude autovacuum processes and processes running manual
 * lazy VACUUMs, because they won't be fazed by missing index entries
 * either.  (Manual ANALYZEs, however, can't be excluded because they
 * might be within transactions that are going to do arbitrary operations
 * later.)  Processes running CREATE INDEX CONCURRENTLY or REINDEX CONCURRENTLY
 * on indexes that are neither expressional nor partial are also safe to
 * ignore, since we know that those processes won't examine any data
 * outside the table they're indexing.
 *
 * Also, GetCurrentVirtualXIDs never reports our own vxid, so we need not
 * check for that.
 *
 * If a process goes idle-in-transaction with xmin zero, we do not need to
 * wait for it anymore, per the above argument.  We do not have the
 * infrastructure right now to stop waiting if that happens, but we can at
 * least avoid the folly of waiting when it is idle at the time we would
 * begin to wait.  We do this by repeatedly rechecking the output of
 * GetCurrentVirtualXIDs.  If, during any iteration, a particular vxid
 * doesn't show up in the output, we know we can forget about it.
 */
void
WaitForOlderSnapshots(TransactionId limitXmin, bool progress)
{
	int			n_old_snapshots;
	int			i;
	VirtualTransactionId *old_snapshots;

	old_snapshots = GetCurrentVirtualXIDs(limitXmin, true, false,
										  PROC_IS_AUTOVACUUM | PROC_IN_VACUUM
										  | PROC_IN_SAFE_IC,
										  &n_old_snapshots);
	if (progress)
		pgstat_progress_update_param(PROGRESS_WAITFOR_TOTAL, n_old_snapshots);

	for (i = 0; i < n_old_snapshots; i++)
	{
		if (!VirtualTransactionIdIsValid(old_snapshots[i]))
			continue;			/* found uninteresting in previous cycle */

		if (i > 0)
		{
			/* see if anything's changed ... */
			VirtualTransactionId *newer_snapshots;
			int			n_newer_snapshots;
			int			j;
			int			k;

			newer_snapshots = GetCurrentVirtualXIDs(limitXmin,
													true, false,
													PROC_IS_AUTOVACUUM | PROC_IN_VACUUM
													| PROC_IN_SAFE_IC,
													&n_newer_snapshots);
			for (j = i; j < n_old_snapshots; j++)
			{
				if (!VirtualTransactionIdIsValid(old_snapshots[j]))
					continue;	/* found uninteresting in previous cycle */
				for (k = 0; k < n_newer_snapshots; k++)
				{
					if (VirtualTransactionIdEquals(old_snapshots[j],
												   newer_snapshots[k]))
						break;
				}
				if (k >= n_newer_snapshots) /* not there anymore */
					SetInvalidVirtualTransactionId(old_snapshots[j]);
			}
			pfree(newer_snapshots);
		}

		if (VirtualTransactionIdIsValid(old_snapshots[i]))
		{
			/* If requested, publish who we're going to wait for. */
			if (progress)
			{
				PGPROC	   *holder = BackendIdGetProc(old_snapshots[i].backendId);

				if (holder)
					pgstat_progress_update_param(PROGRESS_WAITFOR_CURRENT_PID,
												 holder->pid);
			}
			VirtualXactLock(old_snapshots[i], true);
		}

		if (progress)
			pgstat_progress_update_param(PROGRESS_WAITFOR_DONE, i + 1);
	}
}


/*
 * DefineIndex
 *		Creates a new index.
 *
 * This function manages the current userid according to the needs of pg_dump.
 * Recreating old-database catalog entries in new-database is fine, regardless
 * of which users would have permission to recreate those entries now.  That's
 * just preservation of state.  Running opaque expressions, like calling a
 * function named in a catalog entry or evaluating a pg_node_tree in a catalog
 * entry, as anyone other than the object owner, is not fine.  To adhere to
 * those principles and to remain fail-safe, use the table owner userid for
 * most ACL checks.  Use the original userid for ACL checks reached without
 * traversing opaque expressions.  (pg_dump can predict such ACL checks from
 * catalogs.)  Overall, this is a mess.  Future DDL development should
 * consider offering one DDL command for catalog setup and a separate DDL
 * command for steps that run opaque expressions.
 *
 * 'relationId': the OID of the heap relation on which the index is to be
 *		created
 * 'stmt': IndexStmt describing the properties of the new index.
 * 'indexRelationId': normally InvalidOid, but during bootstrap can be
 *		nonzero to specify a preselected OID for the index.
 * 'parentIndexId': the OID of the parent index; InvalidOid if not the child
 *		of a partitioned index.
 * 'parentConstraintId': the OID of the parent constraint; InvalidOid if not
 *		the child of a constraint (only used when recursing)
 * 'is_alter_table': this is due to an ALTER rather than a CREATE operation.
 * 'check_rights': check for CREATE rights in namespace and tablespace.  (This
 *		should be true except when ALTER is deleting/recreating an index.)
 * 'check_not_in_use': check for table not already in use in current session.
 *		This should be true unless caller is holding the table open, in which
 *		case the caller had better have checked it earlier.
 * 'skip_build': make the catalog entries but don't create the index files
 * 'quiet': suppress the NOTICE chatter ordinarily provided for constraints.
 *
 * Returns the object address of the created index.
 */
ObjectAddress
DefineIndex(Oid relationId,
			IndexStmt *stmt,
			Oid indexRelationId,
			Oid parentIndexId,
			Oid parentConstraintId,
			bool is_alter_table,
			bool check_rights,
			bool check_not_in_use,
			bool skip_build,
			bool quiet)
{
	bool		concurrent;
	char	   *indexRelationName;
	char	   *accessMethodName;
	Oid		   *typeObjectId;
	Oid		   *collationObjectId;
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			namespaceId;
	Oid			tablespaceId;
	Oid			createdConstraintId = InvalidOid;
	List	   *indexColNames;
	List	   *allIndexParams;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	amoptions_function amoptions;
	bool		partitioned;
	bool		safe_index;
	Datum		reloptions;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	bits16		flags;
	bits16		constr_flags;
	int			numberOfAttributes;
	int			numberOfKeyAttributes;
	TransactionId limitXmin;
	ObjectAddress address;
	LockRelId	heaprelid;
	LOCKTAG		heaplocktag;
	LOCKMODE	lockmode;
	Snapshot	snapshot;
	Oid			root_save_userid;
	int			root_save_sec_context;
	int			root_save_nestlevel;
	int			i;

	root_save_nestlevel = NewGUCNestLevel();

	/*
	 * Some callers need us to run with an empty default_tablespace; this is a
	 * necessary hack to be able to reproduce catalog state accurately when
	 * recreating indexes after table-rewriting ALTER TABLE.
	 */
	if (stmt->reset_default_tblspc)
		(void) set_config_option("default_tablespace", "",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Force non-concurrent build on temporary relations, even if CONCURRENTLY
	 * was requested.  Other backends can't access a temporary relation, so
	 * there's no harm in grabbing a stronger lock, and a non-concurrent DROP
	 * is more efficient.  Do this before any use of the concurrent option is
	 * done.
	 */
	if (stmt->concurrent && get_rel_persistence(relationId) != RELPERSISTENCE_TEMP)
		concurrent = true;
	else
		concurrent = false;

	/*
	 * Start progress report.  If we're building a partition, this was already
	 * done.
	 */
	if (!OidIsValid(parentIndexId))
	{
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  relationId);
		pgstat_progress_update_param(PROGRESS_CREATEIDX_COMMAND,
									 concurrent ?
									 PROGRESS_CREATEIDX_COMMAND_CREATE_CONCURRENTLY :
									 PROGRESS_CREATEIDX_COMMAND_CREATE);
	}

	/*
	 * No index OID to report yet
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_INDEX_OID,
								 InvalidOid);

	/*
	 * count key attributes in index
	 */
	numberOfKeyAttributes = list_length(stmt->indexParams);

	/*
	 * Calculate the new list of index columns including both key columns and
	 * INCLUDE columns.  Later we can determine which of these are key
	 * columns, and which are just part of the INCLUDE list by checking the
	 * list position.  A list item in a position less than ii_NumIndexKeyAttrs
	 * is part of the key columns, and anything equal to and over is part of
	 * the INCLUDE columns.
	 */
	allIndexParams = list_concat_copy(stmt->indexParams,
									  stmt->indexIncludingParams);
	numberOfAttributes = list_length(allIndexParams);

	if (numberOfKeyAttributes <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("must specify at least one column")));
	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d columns in an index",
						INDEX_MAX_KEYS)));

	/*
	 * Only SELECT ... FOR UPDATE/SHARE are allowed while doing a standard
	 * index build; but for concurrent builds we allow INSERT/UPDATE/DELETE
	 * (but not VACUUM).
	 *
	 * NB: Caller is responsible for making sure that relationId refers to the
	 * relation on which the index should be built; except in bootstrap mode,
	 * this will typically require the caller to have already locked the
	 * relation.  To avoid lock upgrade hazards, that lock should be at least
	 * as strong as the one we take here.
	 *
	 * NB: If the lock strength here ever changes, code that is run by
	 * parallel workers under the control of certain particular ambuild
	 * functions will need to be updated, too.
	 */
	lockmode = concurrent ? ShareUpdateExclusiveLock : ShareLock;
	rel = table_open(relationId, lockmode);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations.  We
	 * already arranged to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&root_save_userid, &root_save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   root_save_sec_context | SECURITY_RESTRICTED_OPERATION);

	namespaceId = RelationGetNamespace(rel);

	/* Ensure that it makes sense to index this kind of relation */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_PARTITIONED_TABLE:
			/* OK */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot create index on relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
			break;
	}

	/*
	 * Establish behavior for partitioned tables, and verify sanity of
	 * parameters.
	 *
	 * We do not build an actual index in this case; we only create a few
	 * catalog entries.  The actual indexes are built by recursing for each
	 * partition.
	 */
	partitioned = rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
	if (partitioned)
	{
		/*
		 * Note: we check 'stmt->concurrent' rather than 'concurrent', so that
		 * the error is thrown also for temporary tables.  Seems better to be
		 * consistent, even though we could do it on temporary table because
		 * we're not actually doing it concurrently.
		 */
		if (stmt->concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create index on partitioned table \"%s\" concurrently",
							RelationGetRelationName(rel))));
		if (stmt->excludeOpNames)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create exclusion constraints on partitioned table \"%s\"",
							RelationGetRelationName(rel))));
	}

	/*
	 * Don't try to CREATE INDEX on temp tables of other backends.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot create indexes on temporary tables of other sessions")));

	/*
	 * Unless our caller vouches for having checked this already, insist that
	 * the table not be in use by our own session, either.  Otherwise we might
	 * fail to make entries in the new index (for instance, if an INSERT or
	 * UPDATE is in progress and has already made its list of target indexes).
	 */
	if (check_not_in_use)
		CheckTableNotInUse(rel, "CREATE INDEX");

	/*
	 * Verify we (still) have CREATE rights in the rel's namespace.
	 * (Presumably we did when the rel was created, but maybe not anymore.)
	 * Skip check if caller doesn't want it.  Also skip check if
	 * bootstrapping, since permissions machinery may not be working yet.
	 */
	if (check_rights && !IsBootstrapProcessingMode())
	{
		AclResult	aclresult;

		aclresult = pg_namespace_aclcheck(namespaceId, root_save_userid,
										  ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(namespaceId));
	}

	/*
	 * Select tablespace to use.  If not specified, use default tablespace
	 * (which may in turn default to database's default).
	 */
	if (stmt->tableSpace)
	{
		tablespaceId = get_tablespace_oid(stmt->tableSpace, false);
		if (partitioned && tablespaceId == MyDatabaseTableSpace)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
	}
	else
	{
		tablespaceId = GetDefaultTablespace(rel->rd_rel->relpersistence,
											partitioned);
		/* note InvalidOid is OK in this case */
	}

	/* Check tablespace permissions */
	if (check_rights &&
		OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(tablespaceId, root_save_userid,
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/*
	 * Force shared indexes into the pg_global tablespace.  This is a bit of a
	 * hack but seems simpler than marking them in the BKI commands.  On the
	 * other hand, if it's not shared, don't allow it to be placed there.
	 */
	if (rel->rd_rel->relisshared)
		tablespaceId = GLOBALTABLESPACE_OID;
	else if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * Choose the index column names.
	 */
	indexColNames = ChooseIndexColumnNames(allIndexParams);

	/*
	 * Select name for index if caller didn't specify
	 */
	indexRelationName = stmt->idxname;
	if (indexRelationName == NULL)
		indexRelationName = ChooseIndexName(RelationGetRelationName(rel),
											namespaceId,
											indexColNames,
											stmt->excludeOpNames,
											stmt->primary,
											stmt->isconstraint);

	/*
	 * look up the access method, verify it can handle the requested features
	 */
	accessMethodName = stmt->accessMethod;
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
	{
		/*
		 * Hack to provide more-or-less-transparent updating of old RTREE
		 * indexes to GiST: if RTREE is requested and not found, use GIST.
		 */
		if (strcmp(accessMethodName, "rtree") == 0)
		{
			ereport(NOTICE,
					(errmsg("substituting access method \"gist\" for obsolete method \"rtree\"")));
			accessMethodName = "gist";
			tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
		}

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("access method \"%s\" does not exist",
							accessMethodName)));
	}
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_ACCESS_METHOD_OID,
								 accessMethodId);

	if (stmt->unique && !amRoutine->amcanunique)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support unique indexes",
						accessMethodName)));
	if (stmt->indexIncludingParams != NIL && !amRoutine->amcaninclude)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support included columns",
						accessMethodName)));
	if (numberOfKeyAttributes > 1 && !amRoutine->amcanmulticol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support multicolumn indexes",
						accessMethodName)));
	if (stmt->excludeOpNames && amRoutine->amgettuple == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support exclusion constraints",
						accessMethodName)));

	amcanorder = amRoutine->amcanorder;
	amoptions = amRoutine->amoptions;

	pfree(amRoutine);
	ReleaseSysCache(tuple);

	/*
	 * Validate predicate, if given
	 */
	if (stmt->whereClause)
		CheckPredicate((Expr *) stmt->whereClause);

	/*
	 * Parse AM-specific options, convert to text array form, validate.
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options,
									 NULL, NULL, false, false);

	(void) index_reloptions(amoptions, reloptions, true);

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo structure.
	 * Note that predicates must be in implicit-AND format.  In a concurrent
	 * build, mark it not-ready-for-inserts.
	 */
	indexInfo = makeIndexInfo(numberOfAttributes,
							  numberOfKeyAttributes,
							  accessMethodId,
							  NIL,	/* expressions, NIL for now */
							  make_ands_implicit((Expr *) stmt->whereClause),
							  stmt->unique,
							  stmt->nulls_not_distinct,
							  !concurrent,
							  concurrent);

	typeObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	collationObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	coloptions = (int16 *) palloc(numberOfAttributes * sizeof(int16));
	ComputeIndexAttrs(indexInfo,
					  typeObjectId, collationObjectId, classObjectId,
					  coloptions, allIndexParams,
					  stmt->excludeOpNames, relationId,
					  accessMethodName, accessMethodId,
					  amcanorder, stmt->isconstraint, root_save_userid,
					  root_save_sec_context, &root_save_nestlevel);

	/*
	 * Extra checks when creating a PRIMARY KEY index.
	 */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, is_alter_table, stmt);

	/*
	 * If this table is partitioned and we're creating a unique index or a
	 * primary key, make sure that the partition key is a subset of the
	 * index's columns.  Otherwise it would be possible to violate uniqueness
	 * by putting values that ought to be unique in different partitions.
	 *
	 * We could lift this limitation if we had global indexes, but those have
	 * their own problems, so this is a useful feature combination.
	 */
	if (partitioned && (stmt->unique || stmt->primary))
	{
		PartitionKey key = RelationGetPartitionKey(rel);
		const char *constraint_type;
		int			i;

		if (stmt->primary)
			constraint_type = "PRIMARY KEY";
		else if (stmt->unique)
			constraint_type = "UNIQUE";
		else if (stmt->excludeOpNames != NIL)
			constraint_type = "EXCLUDE";
		else
		{
			elog(ERROR, "unknown constraint type");
			constraint_type = NULL; /* keep compiler quiet */
		}

		/*
		 * Verify that all the columns in the partition key appear in the
		 * unique key definition, with the same notion of equality.
		 */
		for (i = 0; i < key->partnatts; i++)
		{
			bool		found = false;
			int			eq_strategy;
			Oid			ptkey_eqop;
			int			j;

			/*
			 * Identify the equality operator associated with this partkey
			 * column.  For list and range partitioning, partkeys use btree
			 * operator classes; hash partitioning uses hash operator classes.
			 * (Keep this in sync with ComputePartitionAttrs!)
			 */
			if (key->strategy == PARTITION_STRATEGY_HASH)
				eq_strategy = HTEqualStrategyNumber;
			else
				eq_strategy = BTEqualStrategyNumber;

			ptkey_eqop = get_opfamily_member(key->partopfamily[i],
											 key->partopcintype[i],
											 key->partopcintype[i],
											 eq_strategy);
			if (!OidIsValid(ptkey_eqop))
				elog(ERROR, "missing operator %d(%u,%u) in partition opfamily %u",
					 eq_strategy, key->partopcintype[i], key->partopcintype[i],
					 key->partopfamily[i]);

			/*
			 * We'll need to be able to identify the equality operators
			 * associated with index columns, too.  We know what to do with
			 * btree opclasses; if there are ever any other index types that
			 * support unique indexes, this logic will need extension.
			 */
			if (accessMethodId == BTREE_AM_OID)
				eq_strategy = BTEqualStrategyNumber;
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot match partition key to an index using access method \"%s\"",
								accessMethodName)));

			/*
			 * It may be possible to support UNIQUE constraints when partition
			 * keys are expressions, but is it worth it?  Give up for now.
			 */
			if (key->partattrs[i] == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unsupported %s constraint with partition key definition",
								constraint_type),
						 errdetail("%s constraints cannot be used when partition keys include expressions.",
								   constraint_type)));

			/* Search the index column(s) for a match */
			for (j = 0; j < indexInfo->ii_NumIndexKeyAttrs; j++)
			{
				if (key->partattrs[i] == indexInfo->ii_IndexAttrNumbers[j])
				{
					/* Matched the column, now what about the equality op? */
					Oid			idx_opfamily;
					Oid			idx_opcintype;

					if (get_opclass_opfamily_and_input_type(classObjectId[j],
															&idx_opfamily,
															&idx_opcintype))
					{
						Oid			idx_eqop;

						idx_eqop = get_opfamily_member(idx_opfamily,
													   idx_opcintype,
													   idx_opcintype,
													   eq_strategy);
						if (ptkey_eqop == idx_eqop)
						{
							found = true;
							break;
						}
					}
				}
			}

			if (!found)
			{
				Form_pg_attribute att;

				att = TupleDescAttr(RelationGetDescr(rel),
									key->partattrs[i] - 1);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unique constraint on partitioned table must include all partitioning columns"),
						 errdetail("%s constraint on table \"%s\" lacks column \"%s\" which is part of the partition key.",
								   constraint_type, RelationGetRelationName(rel),
								   NameStr(att->attname))));
			}
		}
	}


	/*
	 * We disallow indexes on system columns.  They would not necessarily get
	 * updated correctly, and they don't seem useful anyway.
	 */
	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		AttrNumber	attno = indexInfo->ii_IndexAttrNumbers[i];

		if (attno < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("index creation on system columns is not supported")));
	}

	/*
	 * Also check for system columns used in expressions or predicates.
	 */
	if (indexInfo->ii_Expressions || indexInfo->ii_Predicate)
	{
		Bitmapset  *indexattrs = NULL;

		pull_varattnos((Node *) indexInfo->ii_Expressions, 1, &indexattrs);
		pull_varattnos((Node *) indexInfo->ii_Predicate, 1, &indexattrs);

		for (i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
							  indexattrs))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("index creation on system columns is not supported")));
		}
	}

	/* Is index safe for others to ignore?  See set_indexsafe_procflags() */
	safe_index = indexInfo->ii_Expressions == NIL &&
		indexInfo->ii_Predicate == NIL;

	/*
	 * Report index creation if appropriate (delay this till after most of the
	 * error checks)
	 */
	if (stmt->isconstraint && !quiet)
	{
		const char *constraint_type;

		if (stmt->primary)
			constraint_type = "PRIMARY KEY";
		else if (stmt->unique)
			constraint_type = "UNIQUE";
		else if (stmt->excludeOpNames != NIL)
			constraint_type = "EXCLUDE";
		else
		{
			elog(ERROR, "unknown constraint type");
			constraint_type = NULL; /* keep compiler quiet */
		}

		ereport(DEBUG1,
				(errmsg_internal("%s %s will create implicit index \"%s\" for table \"%s\"",
								 is_alter_table ? "ALTER TABLE / ADD" : "CREATE TABLE /",
								 constraint_type,
								 indexRelationName, RelationGetRelationName(rel))));
	}

	/*
	 * A valid stmt->oldNode implies that we already have a built form of the
	 * index.  The caller should also decline any index build.
	 */
	Assert(!OidIsValid(stmt->oldNode) || (skip_build && !concurrent));

	/*
	 * Make the catalog entries for the index, including constraints. This
	 * step also actually builds the index, except if caller requested not to
	 * or in concurrent mode, in which case it'll be done later, or doing a
	 * partitioned index (because those don't have storage).
	 */
	flags = constr_flags = 0;
	if (stmt->isconstraint)
		flags |= INDEX_CREATE_ADD_CONSTRAINT;
	if (skip_build || concurrent || partitioned)
		flags |= INDEX_CREATE_SKIP_BUILD;
	if (stmt->if_not_exists)
		flags |= INDEX_CREATE_IF_NOT_EXISTS;
	if (concurrent)
		flags |= INDEX_CREATE_CONCURRENT;
	if (partitioned)
		flags |= INDEX_CREATE_PARTITIONED;
	if (stmt->primary)
		flags |= INDEX_CREATE_IS_PRIMARY;

	/*
	 * If the table is partitioned, and recursion was declined but partitions
	 * exist, mark the index as invalid.
	 */
	if (partitioned && stmt->relation && !stmt->relation->inh)
	{
		PartitionDesc pd = RelationGetPartitionDesc(rel, true);

		if (pd->nparts != 0)
			flags |= INDEX_CREATE_INVALID;
	}

	if (stmt->deferrable)
		constr_flags |= INDEX_CONSTR_CREATE_DEFERRABLE;
	if (stmt->initdeferred)
		constr_flags |= INDEX_CONSTR_CREATE_INIT_DEFERRED;

	indexRelationId =
		index_create(rel, indexRelationName, indexRelationId, parentIndexId,
					 parentConstraintId,
					 stmt->oldNode, indexInfo, indexColNames,
					 accessMethodId, tablespaceId,
					 collationObjectId, classObjectId,
					 coloptions, reloptions,
					 flags, constr_flags,
					 allowSystemTableMods, !check_rights,
					 &createdConstraintId);

	ObjectAddressSet(address, RelationRelationId, indexRelationId);

	if (!OidIsValid(indexRelationId))
	{
		/*
		 * Roll back any GUC changes executed by index functions.  Also revert
		 * to original default_tablespace if we changed it above.
		 */
		AtEOXact_GUC(false, root_save_nestlevel);

		/* Restore userid and security context */
		SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

		table_close(rel, NoLock);

		/* If this is the top-level index, we're done */
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();

		return address;
	}

	/*
	 * Roll back any GUC changes executed by index functions, and keep
	 * subsequent changes local to this command.  This is essential if some
	 * index function changed a behavior-affecting GUC, e.g. search_path.
	 */
	AtEOXact_GUC(false, root_save_nestlevel);
	root_save_nestlevel = NewGUCNestLevel();

	/* Add any requested comment */
	if (stmt->idxcomment != NULL)
		CreateComments(indexRelationId, RelationRelationId, 0,
					   stmt->idxcomment);

	if (partitioned)
	{
		PartitionDesc partdesc;

		/*
		 * Unless caller specified to skip this step (via ONLY), process each
		 * partition to make sure they all contain a corresponding index.
		 *
		 * If we're called internally (no stmt->relation), recurse always.
		 */
		partdesc = RelationGetPartitionDesc(rel, true);
		if ((!stmt->relation || stmt->relation->inh) && partdesc->nparts > 0)
		{
			int			nparts = partdesc->nparts;
			Oid		   *part_oids = palloc(sizeof(Oid) * nparts);
			bool		invalidate_parent = false;
			Relation	parentIndex;
			TupleDesc	parentDesc;

			pgstat_progress_update_param(PROGRESS_CREATEIDX_PARTITIONS_TOTAL,
										 nparts);

			/* Make a local copy of partdesc->oids[], just for safety */
			memcpy(part_oids, partdesc->oids, sizeof(Oid) * nparts);

			/*
			 * We'll need an IndexInfo describing the parent index.  The one
			 * built above is almost good enough, but not quite, because (for
			 * example) its predicate expression if any hasn't been through
			 * expression preprocessing.  The most reliable way to get an
			 * IndexInfo that will match those for child indexes is to build
			 * it the same way, using BuildIndexInfo().
			 */
			parentIndex = index_open(indexRelationId, lockmode);
			indexInfo = BuildIndexInfo(parentIndex);

			parentDesc = RelationGetDescr(rel);

			/*
			 * For each partition, scan all existing indexes; if one matches
			 * our index definition and is not already attached to some other
			 * parent index, attach it to the one we just created.
			 *
			 * If none matches, build a new index by calling ourselves
			 * recursively with the same options (except for the index name).
			 */
			for (i = 0; i < nparts; i++)
			{
				Oid			childRelid = part_oids[i];
				Relation	childrel;
				Oid			child_save_userid;
				int			child_save_sec_context;
				int			child_save_nestlevel;
				List	   *childidxs;
				ListCell   *cell;
				AttrMap    *attmap;
				bool		found = false;

				childrel = table_open(childRelid, lockmode);

				GetUserIdAndSecContext(&child_save_userid,
									   &child_save_sec_context);
				SetUserIdAndSecContext(childrel->rd_rel->relowner,
									   child_save_sec_context | SECURITY_RESTRICTED_OPERATION);
				child_save_nestlevel = NewGUCNestLevel();

				/*
				 * Don't try to create indexes on foreign tables, though. Skip
				 * those if a regular index, or fail if trying to create a
				 * constraint index.
				 */
				if (childrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				{
					if (stmt->unique || stmt->primary)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("cannot create unique index on partitioned table \"%s\"",
										RelationGetRelationName(rel)),
								 errdetail("Table \"%s\" contains partitions that are foreign tables.",
										   RelationGetRelationName(rel))));

					AtEOXact_GUC(false, child_save_nestlevel);
					SetUserIdAndSecContext(child_save_userid,
										   child_save_sec_context);
					table_close(childrel, lockmode);
					continue;
				}

				childidxs = RelationGetIndexList(childrel);
				attmap =
					build_attrmap_by_name(RelationGetDescr(childrel),
										  parentDesc);

				foreach(cell, childidxs)
				{
					Oid			cldidxid = lfirst_oid(cell);
					Relation	cldidx;
					IndexInfo  *cldIdxInfo;

					/* this index is already partition of another one */
					if (has_superclass(cldidxid))
						continue;

					cldidx = index_open(cldidxid, lockmode);
					cldIdxInfo = BuildIndexInfo(cldidx);
					if (CompareIndexInfo(cldIdxInfo, indexInfo,
										 cldidx->rd_indcollation,
										 parentIndex->rd_indcollation,
										 cldidx->rd_opfamily,
										 parentIndex->rd_opfamily,
										 attmap))
					{
						Oid			cldConstrOid = InvalidOid;

						/*
						 * Found a match.
						 *
						 * If this index is being created in the parent
						 * because of a constraint, then the child needs to
						 * have a constraint also, so look for one.  If there
						 * is no such constraint, this index is no good, so
						 * keep looking.
						 */
						if (createdConstraintId != InvalidOid)
						{
							cldConstrOid =
								get_relation_idx_constraint_oid(childRelid,
																cldidxid);
							if (cldConstrOid == InvalidOid)
							{
								index_close(cldidx, lockmode);
								continue;
							}
						}

						/* Attach index to parent and we're done. */
						IndexSetParentIndex(cldidx, indexRelationId);
						if (createdConstraintId != InvalidOid)
							ConstraintSetParentConstraint(cldConstrOid,
														  createdConstraintId,
														  childRelid);

						if (!cldidx->rd_index->indisvalid)
							invalidate_parent = true;

						found = true;
						/* keep lock till commit */
						index_close(cldidx, NoLock);
						break;
					}

					index_close(cldidx, lockmode);
				}

				list_free(childidxs);
				AtEOXact_GUC(false, child_save_nestlevel);
				SetUserIdAndSecContext(child_save_userid,
									   child_save_sec_context);
				table_close(childrel, NoLock);

				/*
				 * If no matching index was found, create our own.
				 */
				if (!found)
				{
					IndexStmt  *childStmt = copyObject(stmt);
					bool		found_whole_row;
					ListCell   *lc;
					ObjectAddress childAddr;

					/*
					 * We can't use the same index name for the child index,
					 * so clear idxname to let the recursive invocation choose
					 * a new name.  Likewise, the existing target relation
					 * field is wrong, and if indexOid or oldNode are set,
					 * they mustn't be applied to the child either.
					 */
					childStmt->idxname = NULL;
					childStmt->relation = NULL;
					childStmt->indexOid = InvalidOid;
					childStmt->oldNode = InvalidOid;
					childStmt->oldCreateSubid = InvalidSubTransactionId;
					childStmt->oldFirstRelfilenodeSubid = InvalidSubTransactionId;

					/*
					 * Adjust any Vars (both in expressions and in the index's
					 * WHERE clause) to match the partition's column numbering
					 * in case it's different from the parent's.
					 */
					foreach(lc, childStmt->indexParams)
					{
						IndexElem  *ielem = lfirst(lc);

						/*
						 * If the index parameter is an expression, we must
						 * translate it to contain child Vars.
						 */
						if (ielem->expr)
						{
							ielem->expr =
								map_variable_attnos((Node *) ielem->expr,
													1, 0, attmap,
													InvalidOid,
													&found_whole_row);
							if (found_whole_row)
								elog(ERROR, "cannot convert whole-row table reference");
						}
					}
					childStmt->whereClause =
						map_variable_attnos(stmt->whereClause, 1, 0,
											attmap,
											InvalidOid, &found_whole_row);
					if (found_whole_row)
						elog(ERROR, "cannot convert whole-row table reference");

					/*
					 * Recurse as the starting user ID.  Callee will use that
					 * for permission checks, then switch again.
					 */
					Assert(GetUserId() == child_save_userid);
					SetUserIdAndSecContext(root_save_userid,
										   root_save_sec_context);
					childAddr =
						DefineIndex(childRelid, childStmt,
									InvalidOid, /* no predefined OID */
									indexRelationId,	/* this is our child */
									createdConstraintId,
									is_alter_table, check_rights,
									check_not_in_use,
									skip_build, quiet);
					SetUserIdAndSecContext(child_save_userid,
										   child_save_sec_context);

					/*
					 * Check if the index just created is valid or not, as it
					 * could be possible that it has been switched as invalid
					 * when recursing across multiple partition levels.
					 */
					if (!get_index_isvalid(childAddr.objectId))
						invalidate_parent = true;
				}

				pgstat_progress_update_param(PROGRESS_CREATEIDX_PARTITIONS_DONE,
											 i + 1);
				free_attrmap(attmap);
			}

			index_close(parentIndex, lockmode);

			/*
			 * The pg_index row we inserted for this index was marked
			 * indisvalid=true.  But if we attached an existing index that is
			 * invalid, this is incorrect, so update our row to invalid too.
			 */
			if (invalidate_parent)
			{
				Relation	pg_index = table_open(IndexRelationId, RowExclusiveLock);
				HeapTuple	tup,
							newtup;

				tup = SearchSysCache1(INDEXRELID,
									  ObjectIdGetDatum(indexRelationId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for index %u",
						 indexRelationId);
				newtup = heap_copytuple(tup);
				((Form_pg_index) GETSTRUCT(newtup))->indisvalid = false;
				CatalogTupleUpdate(pg_index, &tup->t_self, newtup);
				ReleaseSysCache(tup);
				table_close(pg_index, RowExclusiveLock);
				heap_freetuple(newtup);

				/*
				 * CCI here to make this update visible, in case this recurses
				 * across multiple partition levels.
				 */
				CommandCounterIncrement();
			}
		}

		/*
		 * Indexes on partitioned tables are not themselves built, so we're
		 * done here.
		 */
		AtEOXact_GUC(false, root_save_nestlevel);
		SetUserIdAndSecContext(root_save_userid, root_save_sec_context);
		table_close(rel, NoLock);
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();
		return address;
	}

	AtEOXact_GUC(false, root_save_nestlevel);
	SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

	if (!concurrent)
	{
		/* Close the heap and we're done, in the non-concurrent case */
		table_close(rel, NoLock);

		/* If this is the top-level index, we're done. */
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();

		return address;
	}

	/* save lockrelid and locktag for below, then close rel */
	heaprelid = rel->rd_lockInfo.lockRelId;
	SET_LOCKTAG_RELATION(heaplocktag, heaprelid.dbId, heaprelid.relId);
	table_close(rel, NoLock);

	/*
	 * For a concurrent build, it's important to make the catalog entries
	 * visible to other transactions before we start to build the index. That
	 * will prevent them from making incompatible HOT updates.  The new index
	 * will be marked not indisready and not indisvalid, so that no one else
	 * tries to either insert into it or use it for queries.
	 *
	 * We must commit our current transaction so that the index becomes
	 * visible; then start another.  Note that all the data structures we just
	 * built are lost in the commit.  The only data we keep past here are the
	 * relation IDs.
	 *
	 * Before committing, get a session-level lock on the table, to ensure
	 * that neither it nor the index can be dropped before we finish. This
	 * cannot block, even if someone else is waiting for access, because we
	 * already have the same lock within our transaction.
	 *
	 * Note: we don't currently bother with a session lock on the index,
	 * because there are no operations that could change its state while we
	 * hold lock on the parent table.  This might need to change later.
	 */
	LockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);

	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * The index is now visible, so we can report the OID.  While on it,
	 * include the report for the beginning of phase 2.
	 */
	{
		const int	progress_cols[] = {
			PROGRESS_CREATEIDX_INDEX_OID,
			PROGRESS_CREATEIDX_PHASE
		};
		const int64 progress_vals[] = {
			indexRelationId,
			PROGRESS_CREATEIDX_PHASE_WAIT_1
		};

		pgstat_progress_update_multi_param(2, progress_cols, progress_vals);
	}

	/*
	 * Phase 2 of concurrent index build (see comments for validate_index()
	 * for an overview of how this works)
	 *
	 * Now we must wait until no running transaction could have the table open
	 * with the old list of indexes.  Use ShareLock to consider running
	 * transactions that hold locks that permit writing to the table.  Note we
	 * do not need to worry about xacts that open the table for writing after
	 * this point; they will see the new index when they open it.
	 *
	 * Note: the reason we use actual lock acquisition here, rather than just
	 * checking the ProcArray and sleeping, is that deadlock is possible if
	 * one of the transactions in question is blocked trying to acquire an
	 * exclusive lock on our table.  The lock code will detect deadlock and
	 * error out properly.
	 */
	WaitForLockers(heaplocktag, ShareLock, true);

	/*
	 * At this moment we are sure that there are no transactions with the
	 * table open for write that don't have this new index in their list of
	 * indexes.  We have waited out all the existing transactions and any new
	 * transaction will have the new index in its list, but the index is still
	 * marked as "not-ready-for-inserts".  The index is consulted while
	 * deciding HOT-safety though.  This arrangement ensures that no new HOT
	 * chains can be created where the new tuple and the old tuple in the
	 * chain have different index keys.
	 *
	 * We now take a new snapshot, and build the index using all tuples that
	 * are visible in this snapshot.  We can be sure that any HOT updates to
	 * these tuples will be compatible with the index, since any updates made
	 * by transactions that didn't know about the index are now committed or
	 * rolled back.  Thus, each visible tuple is either the end of its
	 * HOT-chain or the extension of the chain is HOT-safe for this index.
	 */

	/* Set ActiveSnapshot since functions in the indexes may need it */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Perform concurrent build of index */
	index_concurrently_build(relationId, indexRelationId);

	/* we can do away with our snapshot */
	PopActiveSnapshot();

	/*
	 * Commit this transaction to make the indisready update visible.
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * Phase 3 of concurrent index build
	 *
	 * We once again wait until no transaction can have the table open with
	 * the index marked as read-only for updates.
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_2);
	WaitForLockers(heaplocktag, ShareLock, true);

	/*
	 * Now take the "reference snapshot" that will be used by validate_index()
	 * to filter candidate tuples.  Beware!  There might still be snapshots in
	 * use that treat some transaction as in-progress that our reference
	 * snapshot treats as committed.  If such a recently-committed transaction
	 * deleted tuples in the table, we will not include them in the index; yet
	 * those transactions which see the deleting one as still-in-progress will
	 * expect such tuples to be there once we mark the index as valid.
	 *
	 * We solve this by waiting for all endangered transactions to exit before
	 * we mark the index as valid.
	 *
	 * We also set ActiveSnapshot to this snap, since functions in indexes may
	 * need a snapshot.
	 */
	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	PushActiveSnapshot(snapshot);

	/*
	 * Scan the index and the heap, insert any missing index entries.
	 */
	validate_index(relationId, indexRelationId, snapshot);

	/*
	 * Drop the reference snapshot.  We must do this before waiting out other
	 * snapshot holders, else we will deadlock against other processes also
	 * doing CREATE INDEX CONCURRENTLY, which would see our snapshot as one
	 * they must wait for.  But first, save the snapshot's xmin to use as
	 * limitXmin for GetCurrentVirtualXIDs().
	 */
	limitXmin = snapshot->xmin;

	PopActiveSnapshot();
	UnregisterSnapshot(snapshot);

	/*
	 * The snapshot subsystem could still contain registered snapshots that
	 * are holding back our process's advertised xmin; in particular, if
	 * default_transaction_isolation = serializable, there is a transaction
	 * snapshot that is still active.  The CatalogSnapshot is likewise a
	 * hazard.  To ensure no deadlocks, we must commit and start yet another
	 * transaction, and do our wait before any snapshot has been taken in it.
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* Tell concurrent index builds to ignore us, if index qualifies */
	if (safe_index)
		set_indexsafe_procflags();

	/* We should now definitely not be advertising any xmin. */
	Assert(MyProc->xmin == InvalidTransactionId);

	/*
	 * The index is now valid in the sense that it contains all currently
	 * interesting tuples.  But since it might not contain tuples deleted just
	 * before the reference snap was taken, we have to wait out any
	 * transactions that might have older snapshots.
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_3);
	WaitForOlderSnapshots(limitXmin, true);

	/*
	 * Index can now be marked valid -- update its pg_index entry
	 */
	index_set_state_flags(indexRelationId, INDEX_CREATE_SET_VALID);

	/*
	 * The pg_index update will cause backends (including this one) to update
	 * relcache entries for the index itself, but we should also send a
	 * relcache inval on the parent table to force replanning of cached plans.
	 * Otherwise existing sessions might fail to use the new index where it
	 * would be useful.  (Note that our earlier commits did not create reasons
	 * to replan; so relcache flush on the index itself was sufficient.)
	 */
	CacheInvalidateRelcacheByRelid(heaprelid.relId);

	/*
	 * Last thing to do is release the session-level lock on the parent table.
	 */
	UnlockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);

	pgstat_progress_end_command();

	return address;
}


/*
 * CheckMutability
 *		Test whether given expression is mutable
 */
static bool
CheckMutability(Expr *expr)
{
	/*
	 * First run the expression through the planner.  This has a couple of
	 * important consequences.  First, function default arguments will get
	 * inserted, which may affect volatility (consider "default now()").
	 * Second, inline-able functions will get inlined, which may allow us to
	 * conclude that the function is really less volatile than it's marked. As
	 * an example, polymorphic functions must be marked with the most volatile
	 * behavior that they have for any input type, but once we inline the
	 * function we may be able to conclude that it's not so volatile for the
	 * particular input type we're dealing with.
	 *
	 * We assume here that expression_planner() won't scribble on its input.
	 */
	expr = expression_planner(expr);

	/* Now we can search for non-immutable functions */
	return contain_mutable_functions((Node *) expr);
}


/*
 * CheckPredicate
 *		Checks that the given partial-index predicate is valid.
 *
 * This used to also constrain the form of the predicate to forms that
 * indxpath.c could do something with.  However, that seems overly
 * restrictive.  One useful application of partial indexes is to apply
 * a UNIQUE constraint across a subset of a table, and in that scenario
 * any evaluable predicate will work.  So accept any predicate here
 * (except ones requiring a plan), and let indxpath.c fend for itself.
 */
static void
CheckPredicate(Expr *predicate)
{
	/*
	 * transformExpr() should have already rejected subqueries, aggregates,
	 * and window functions, based on the EXPR_KIND_ for a predicate.
	 */

	/*
	 * A predicate using mutable functions is probably wrong, for the same
	 * reasons that we don't allow an index expression to use one.
	 */
	if (CheckMutability(predicate))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("functions in index predicate must be marked IMMUTABLE")));
}

/*
 * Compute per-index-column information, including indexed column numbers
 * or index expressions, opclasses and their options. Note, all output vectors
 * should be allocated for all columns, including "including" ones.
 *
 * If the caller switched to the table owner, ddl_userid is the role for ACL
 * checks reached without traversing opaque expressions.  Otherwise, it's
 * InvalidOid, and other ddl_* arguments are undefined.
 */
static void
ComputeIndexAttrs(IndexInfo *indexInfo,
				  Oid *typeOidP,
				  Oid *collationOidP,
				  Oid *classOidP,
				  int16 *colOptionP,
				  List *attList,	/* list of IndexElem's */
				  List *exclusionOpNames,
				  Oid relId,
				  const char *accessMethodName,
				  Oid accessMethodId,
				  bool amcanorder,
				  bool isconstraint,
				  Oid ddl_userid,
				  int ddl_sec_context,
				  int *ddl_save_nestlevel)
{
	ListCell   *nextExclOp;
	ListCell   *lc;
	int			attn;
	int			nkeycols = indexInfo->ii_NumIndexKeyAttrs;
	Oid			save_userid;
	int			save_sec_context;

	/* Allocate space for exclusion operator info, if needed */
	if (exclusionOpNames)
	{
		Assert(list_length(exclusionOpNames) == nkeycols);
		indexInfo->ii_ExclusionOps = (Oid *) palloc(sizeof(Oid) * nkeycols);
		indexInfo->ii_ExclusionProcs = (Oid *) palloc(sizeof(Oid) * nkeycols);
		indexInfo->ii_ExclusionStrats = (uint16 *) palloc(sizeof(uint16) * nkeycols);
		nextExclOp = list_head(exclusionOpNames);
	}
	else
		nextExclOp = NULL;

	if (OidIsValid(ddl_userid))
		GetUserIdAndSecContext(&save_userid, &save_sec_context);

	/*
	 * process attributeList
	 */
	attn = 0;
	foreach(lc, attList)
	{
		IndexElem  *attribute = (IndexElem *) lfirst(lc);
		Oid			atttype;
		Oid			attcollation;

		/*
		 * Process the column-or-expression to be indexed.
		 */
		if (attribute->name != NULL)
		{
			/* Simple index attribute */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			Assert(attribute->expr == NULL);
			atttuple = SearchSysCacheAttName(relId, attribute->name);
			if (!HeapTupleIsValid(atttuple))
			{
				/* difference in error message spellings is historical */
				if (isconstraint)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" named in key does not exist",
									attribute->name)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									attribute->name)));
			}
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);
			indexInfo->ii_IndexAttrNumbers[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = attform->attcollation;
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* Index expression */
			Node	   *expr = attribute->expr;

			Assert(expr != NULL);

			if (attn >= nkeycols)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("expressions are not supported in included columns")));
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * Strip any top-level COLLATE clause.  This ensures that we treat
			 * "x COLLATE y" and "(x COLLATE y)" alike.
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno != InvalidAttrNumber)
			{
				/*
				 * User wrote "(column)" or "(column COLLATE something)".
				 * Treat it like simple attribute anyway.
				 */
				indexInfo->ii_IndexAttrNumbers[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				indexInfo->ii_IndexAttrNumbers[attn] = 0;	/* marks expression */
				indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions,
													expr);

				/*
				 * transformExpr() should have already rejected subqueries,
				 * aggregates, and window functions, based on the EXPR_KIND_
				 * for an index expression.
				 */

				/*
				 * An expression using mutable functions is probably wrong,
				 * since if you aren't going to get the same result for the
				 * same data every time, it's not clear what the index entries
				 * mean at all.
				 */
				if (CheckMutability((Expr *) expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in index expression must be marked IMMUTABLE")));
			}
		}

		typeOidP[attn] = atttype;

		/*
		 * Included columns have no collation, no opclass and no ordering
		 * options.
		 */
		if (attn >= nkeycols)
		{
			if (attribute->collation)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support a collation")));
			if (attribute->opclass)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support an operator class")));
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support ASC/DESC options")));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support NULLS FIRST/LAST options")));

			classOidP[attn] = InvalidOid;
			colOptionP[attn] = 0;
			collationOidP[attn] = InvalidOid;
			attn++;

			continue;
		}

		/*
		 * Apply collation override if any.  Use of ddl_userid is necessary
		 * due to ACL checks therein, and it's safe because collations don't
		 * contain opaque expressions (or non-opaque expressions).
		 */
		if (attribute->collation)
		{
			if (OidIsValid(ddl_userid))
			{
				AtEOXact_GUC(false, *ddl_save_nestlevel);
				SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
			}
			attcollation = get_collation_oid(attribute->collation, false);
			if (OidIsValid(ddl_userid))
			{
				SetUserIdAndSecContext(save_userid, save_sec_context);
				*ddl_save_nestlevel = NewGUCNestLevel();
			}
		}

		/*
		 * Check we have a collation iff it's a collatable type.  The only
		 * expected failures here are (1) COLLATE applied to a noncollatable
		 * type, or (2) index expression had an unresolved collation.  But we
		 * might as well code this to be a complete consistency check.
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for index expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		collationOidP[attn] = attcollation;

		/*
		 * Identify the opclass to use.  Use of ddl_userid is necessary due to
		 * ACL checks therein.  This is safe despite opclasses containing
		 * opaque expressions (specifically, functions), because only
		 * superusers can define opclasses.
		 */
		if (OidIsValid(ddl_userid))
		{
			AtEOXact_GUC(false, *ddl_save_nestlevel);
			SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
		}
		classOidP[attn] = ResolveOpClass(attribute->opclass,
										 atttype,
										 accessMethodName,
										 accessMethodId);
		if (OidIsValid(ddl_userid))
		{
			SetUserIdAndSecContext(save_userid, save_sec_context);
			*ddl_save_nestlevel = NewGUCNestLevel();
		}

		/*
		 * Identify the exclusion operator, if any.
		 */
		if (nextExclOp)
		{
			List	   *opname = (List *) lfirst(nextExclOp);
			Oid			opid;
			Oid			opfamily;
			int			strat;

			/*
			 * Find the operator --- it must accept the column datatype
			 * without runtime coercion (but binary compatibility is OK).
			 * Operators contain opaque expressions (specifically, functions).
			 * compatible_oper_opid() boils down to oper() and
			 * IsBinaryCoercible().  PostgreSQL would have security problems
			 * elsewhere if oper() started calling opaque expressions.
			 */
			if (OidIsValid(ddl_userid))
			{
				AtEOXact_GUC(false, *ddl_save_nestlevel);
				SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
			}
			opid = compatible_oper_opid(opname, atttype, atttype, false);
			if (OidIsValid(ddl_userid))
			{
				SetUserIdAndSecContext(save_userid, save_sec_context);
				*ddl_save_nestlevel = NewGUCNestLevel();
			}

			/*
			 * Only allow commutative operators to be used in exclusion
			 * constraints. If X conflicts with Y, but Y does not conflict
			 * with X, bad things will happen.
			 */
			if (get_commutator(opid) != opid)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("operator %s is not commutative",
								format_operator(opid)),
						 errdetail("Only commutative operators can be used in exclusion constraints.")));

			/*
			 * Operator must be a member of the right opfamily, too
			 */
			opfamily = get_opclass_family(classOidP[attn]);
			strat = get_op_opfamily_strategy(opid, opfamily);
			if (strat == 0)
			{
				HeapTuple	opftuple;
				Form_pg_opfamily opfform;

				/*
				 * attribute->opclass might not explicitly name the opfamily,
				 * so fetch the name of the selected opfamily for use in the
				 * error message.
				 */
				opftuple = SearchSysCache1(OPFAMILYOID,
										   ObjectIdGetDatum(opfamily));
				if (!HeapTupleIsValid(opftuple))
					elog(ERROR, "cache lookup failed for opfamily %u",
						 opfamily);
				opfform = (Form_pg_opfamily) GETSTRUCT(opftuple);

				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("operator %s is not a member of operator family \"%s\"",
								format_operator(opid),
								NameStr(opfform->opfname)),
						 errdetail("The exclusion operator must be related to the index operator class for the constraint.")));
			}

			indexInfo->ii_ExclusionOps[attn] = opid;
			indexInfo->ii_ExclusionProcs[attn] = get_opcode(opid);
			indexInfo->ii_ExclusionStrats[attn] = strat;
			nextExclOp = lnext(exclusionOpNames, nextExclOp);
		}

		/*
		 * Set up the per-column options (indoption field).  For now, this is
		 * zero for any un-ordered index, while ordered indexes have DESC and
		 * NULLS FIRST/LAST options.
		 */
		colOptionP[attn] = 0;
		if (amcanorder)
		{
			/* default ordering is ASC */
			if (attribute->ordering == SORTBY_DESC)
				colOptionP[attn] |= INDOPTION_DESC;
			/* default null ordering is LAST for ASC, FIRST for DESC */
			if (attribute->nulls_ordering == SORTBY_NULLS_DEFAULT)
			{
				if (attribute->ordering == SORTBY_DESC)
					colOptionP[attn] |= INDOPTION_NULLS_FIRST;
			}
			else if (attribute->nulls_ordering == SORTBY_NULLS_FIRST)
				colOptionP[attn] |= INDOPTION_NULLS_FIRST;
		}
		else
		{
			/* index AM does not support ordering */
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support ASC/DESC options",
								accessMethodName)));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support NULLS FIRST/LAST options",
								accessMethodName)));
		}

		/* Set up the per-column opclass options (attoptions field). */
		if (attribute->opclassopts)
		{
			Assert(attn < nkeycols);

			if (!indexInfo->ii_OpclassOptions)
				indexInfo->ii_OpclassOptions =
					palloc0(sizeof(Datum) * indexInfo->ii_NumIndexAttrs);

			indexInfo->ii_OpclassOptions[attn] =
				transformRelOptions((Datum) 0, attribute->opclassopts,
									NULL, NULL, false, false);
		}

		attn++;
	}
}

/*
 * Resolve possibly-defaulted operator class specification
 *
 * Note: This is used to resolve operator class specifications in index and
 * partition key definitions.
 */
Oid
ResolveOpClass(List *opclass, Oid attrType,
			   const char *accessMethodName, Oid accessMethodId)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	Form_pg_opclass opform;
	Oid			opClassId,
				opInputType;

	if (opclass == NIL)
	{
		/* no operator class specified, so find the default */
		opClassId = GetDefaultOpClass(attrType, accessMethodId);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(attrType), accessMethodName),
					 errhint("You must specify an operator class for the index or define a default operator class for the data type.")));
		return opClassId;
	}

	/*
	 * Specific opclass name given, so look up the opclass.
	 */

	/* deconstruct the name list */
	DeconstructQualifiedName(opclass, &schemaname, &opcname);

	if (schemaname)
	{
		/* Look in specific schema only */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, false);
		tuple = SearchSysCache3(CLAAMNAMENSP,
								ObjectIdGetDatum(accessMethodId),
								PointerGetDatum(opcname),
								ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* Unqualified opclass name, so search the search path */
		opClassId = OpclassnameGetOpcid(accessMethodId, opcname);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, accessMethodName)));
		tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opClassId));
	}

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclass), accessMethodName)));

	/*
	 * Verify that the index operator class accepts this datatype.  Note we
	 * will accept binary compatibility.
	 */
	opform = (Form_pg_opclass) GETSTRUCT(tuple);
	opClassId = opform->oid;
	opInputType = opform->opcintype;

	if (!IsBinaryCoercible(attrType, opInputType))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("operator class \"%s\" does not accept data type %s",
						NameListToString(opclass), format_type_be(attrType))));

	ReleaseSysCache(tuple);

	return opClassId;
}

/*
 * GetDefaultOpClass
 *
 * Given the OIDs of a datatype and an access method, find the default
 * operator class, if any.  Returns InvalidOid if there is none.
 */
Oid
GetDefaultOpClass(Oid type_id, Oid am_id)
{
	Oid			result = InvalidOid;
	int			nexact = 0;
	int			ncompatible = 0;
	int			ncompatiblepreferred = 0;
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;
	TYPCATEGORY tcategory;

	/* If it's a domain, look at the base type instead */
	type_id = getBaseType(type_id);

	tcategory = TypeCategory(type_id);

	/*
	 * We scan through all the opclasses available for the access method,
	 * looking for one that is marked default and matches the target type
	 * (either exactly or binary-compatibly, but prefer an exact match).
	 *
	 * We could find more than one binary-compatible match.  If just one is
	 * for a preferred type, use that one; otherwise we fail, forcing the user
	 * to specify which one he wants.  (The preferred-type special case is a
	 * kluge for varchar: it's binary-compatible to both text and bpchar, so
	 * we need a tiebreaker.)  If we find more than one exact match, then
	 * someone put bogus entries in pg_opclass.
	 */
	rel = table_open(OperatorClassRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_opclass_opcmethod,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(am_id));

	scan = systable_beginscan(rel, OpclassAmNameNspIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

		/* ignore altogether if not a default opclass */
		if (!opclass->opcdefault)
			continue;
		if (opclass->opcintype == type_id)
		{
			nexact++;
			result = opclass->oid;
		}
		else if (nexact == 0 &&
				 IsBinaryCoercible(type_id, opclass->opcintype))
		{
			if (IsPreferredType(tcategory, opclass->opcintype))
			{
				ncompatiblepreferred++;
				result = opclass->oid;
			}
			else if (ncompatiblepreferred == 0)
			{
				ncompatible++;
				result = opclass->oid;
			}
		}
	}

	systable_endscan(scan);

	table_close(rel, AccessShareLock);

	/* raise error if pg_opclass contains inconsistent data */
	if (nexact > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("there are multiple default operator classes for data type %s",
						format_type_be(type_id))));

	if (nexact == 1 ||
		ncompatiblepreferred == 1 ||
		(ncompatiblepreferred == 0 && ncompatible == 1))
		return result;

	return InvalidOid;
}

/*
 *	makeObjectName()
 *
 *	Create a name for an implicitly created index, sequence, constraint,
 *	extended statistics, etc.
 *
 *	The parameters are typically: the original table name, the original field
 *	name, and a "type" string (such as "seq" or "pkey").    The field name
 *	and/or type can be NULL if not relevant.
 *
 *	The result is a palloc'd string.
 *
 *	The basic result we want is "name1_name2_label", omitting "_name2" or
 *	"_label" when those parameters are NULL.  However, we must generate
 *	a name with less than NAMEDATALEN characters!  So, we truncate one or
 *	both names if necessary to make a short-enough string.  The label part
 *	is never truncated (so it had better be reasonably short).
 *
 *	The caller is responsible for checking uniqueness of the generated
 *	name and retrying as needed; retrying will be done by altering the
 *	"label" string (which is why we never truncate that part).
 */
char *
makeObjectName(const char *name1, const char *name2, const char *label)
{
	char	   *name;
	int			overhead = 0;	/* chars needed for label and underscores */
	int			availchars;		/* chars available for name(s) */
	int			name1chars;		/* chars allocated to name1 */
	int			name2chars;		/* chars allocated to name2 */
	int			ndx;

	name1chars = strlen(name1);
	if (name2)
	{
		name2chars = strlen(name2);
		overhead++;				/* allow for separating underscore */
	}
	else
		name2chars = 0;
	if (label)
		overhead += strlen(label) + 1;

	availchars = NAMEDATALEN - 1 - overhead;
	Assert(availchars > 0);		/* else caller chose a bad label */

	/*
	 * If we must truncate, preferentially truncate the longer name. This
	 * logic could be expressed without a loop, but it's simple and obvious as
	 * a loop.
	 */
	while (name1chars + name2chars > availchars)
	{
		if (name1chars > name2chars)
			name1chars--;
		else
			name2chars--;
	}

	name1chars = pg_mbcliplen(name1, name1chars, name1chars);
	if (name2)
		name2chars = pg_mbcliplen(name2, name2chars, name2chars);

	/* Now construct the string using the chosen lengths */
	name = palloc(name1chars + name2chars + overhead + 1);
	memcpy(name, name1, name1chars);
	ndx = name1chars;
	if (name2)
	{
		name[ndx++] = '_';
		memcpy(name + ndx, name2, name2chars);
		ndx += name2chars;
	}
	if (label)
	{
		name[ndx++] = '_';
		strcpy(name + ndx, label);
	}
	else
		name[ndx] = '\0';

	return name;
}

/*
 * Select a nonconflicting name for a new relation.  This is ordinarily
 * used to choose index names (which is why it's here) but it can also
 * be used for sequences, or any autogenerated relation kind.
 *
 * name1, name2, and label are used the same way as for makeObjectName(),
 * except that the label can't be NULL; digits will be appended to the label
 * if needed to create a name that is unique within the specified namespace.
 *
 * If isconstraint is true, we also avoid choosing a name matching any
 * existing constraint in the same namespace.  (This is stricter than what
 * Postgres itself requires, but the SQL standard says that constraint names
 * should be unique within schemas, so we follow that for autogenerated
 * constraint names.)
 *
 * Note: it is theoretically possible to get a collision anyway, if someone
 * else chooses the same name concurrently.  This is fairly unlikely to be
 * a problem in practice, especially if one is holding an exclusive lock on
 * the relation identified by name1.  However, if choosing multiple names
 * within a single command, you'd better create the new object and do
 * CommandCounterIncrement before choosing the next one!
 *
 * Returns a palloc'd string.
 */
char *
ChooseRelationName(const char *name1, const char *name2,
				   const char *label, Oid namespaceid,
				   bool isconstraint)
{
	int			pass = 0;
	char	   *relname = NULL;
	char		modlabel[NAMEDATALEN];

	/* try the unmodified label first */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		relname = makeObjectName(name1, name2, modlabel);

		if (!OidIsValid(get_relname_relid(relname, namespaceid)))
		{
			if (!isconstraint ||
				!ConstraintNameExists(relname, namespaceid))
				break;
		}

		/* found a conflict, so try a new name component */
		pfree(relname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	return relname;
}

/*
 * Select the name to be used for an index.
 *
 * The argument list is pretty ad-hoc :-(
 */
static char *
ChooseIndexName(const char *tabname, Oid namespaceId,
				List *colnames, List *exclusionOpNames,
				bool primary, bool isconstraint)
{
	char	   *indexname;

	if (primary)
	{
		/* the primary key's name does not depend on the specific column(s) */
		indexname = ChooseRelationName(tabname,
									   NULL,
									   "pkey",
									   namespaceId,
									   true);
	}
	else if (exclusionOpNames != NIL)
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "excl",
									   namespaceId,
									   true);
	}
	else if (isconstraint)
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "key",
									   namespaceId,
									   true);
	}
	else
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "idx",
									   namespaceId,
									   false);
	}

	return indexname;
}

/*
 * Generate "name2" for a new index given the list of column names for it
 * (as produced by ChooseIndexColumnNames).  This will be passed to
 * ChooseRelationName along with the parent table name and a suitable label.
 *
 * We know that less than NAMEDATALEN characters will actually be used,
 * so we can truncate the result once we've generated that many.
 *
 * XXX See also ChooseForeignKeyConstraintNameAddition and
 * ChooseExtendedStatisticNameAddition.
 */
static char *
ChooseIndexNameAddition(List *colnames)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, colnames)
	{
		const char *name = (const char *) lfirst(lc);

		if (buflen > 0)
			buf[buflen++] = '_';	/* insert _ between names */

		/*
		 * At this point we have buflen <= NAMEDATALEN.  name should be less
		 * than NAMEDATALEN already, but use strlcpy for paranoia.
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * Select the actual names to be used for the columns of an index, given the
 * list of IndexElems for the columns.  This is mostly about ensuring the
 * names are unique so we don't get a conflicting-attribute-names error.
 *
 * Returns a List of plain strings (char *, not String nodes).
 */
static List *
ChooseIndexColumnNames(List *indexElems)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, indexElems)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(lc);
		const char *origname;
		const char *curname;
		int			i;
		char		buf[NAMEDATALEN];

		/* Get the preliminary name from the IndexElem */
		if (ielem->indexcolname)
			origname = ielem->indexcolname; /* caller-specified name */
		else if (ielem->name)
			origname = ielem->name; /* simple column reference */
		else
			origname = "expr";	/* default name for expression */

		/* If it conflicts with any previous column, tweak it */
		curname = origname;
		for (i = 1;; i++)
		{
			ListCell   *lc2;
			char		nbuf[32];
			int			nlen;

			foreach(lc2, result)
			{
				if (strcmp(curname, (char *) lfirst(lc2)) == 0)
					break;
			}
			if (lc2 == NULL)
				break;			/* found nonconflicting name */

			sprintf(nbuf, "%d", i);

			/* Ensure generated names are shorter than NAMEDATALEN */
			nlen = pg_mbcliplen(origname, strlen(origname),
								NAMEDATALEN - 1 - strlen(nbuf));
			memcpy(buf, origname, nlen);
			strcpy(buf + nlen, nbuf);
			curname = buf;
		}

		/* And attach to the result list */
		result = lappend(result, pstrdup(curname));
	}
	return result;
}

/*
 * ExecReindex
 *
 * Primary entry point for manual REINDEX commands.  This is mainly a
 * preparation wrapper for the real operations that will happen in
 * each subroutine of REINDEX.
 */
void
ExecReindex(ParseState *pstate, ReindexStmt *stmt, bool isTopLevel)
{
	ReindexParams params = {0};
	ListCell   *lc;
	bool		concurrently = false;
	bool		verbose = false;
	char	   *tablespacename = NULL;

	/* Parse option list */
	foreach(lc, stmt->params)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "concurrently") == 0)
			concurrently = defGetBoolean(opt);
		else if (strcmp(opt->defname, "tablespace") == 0)
			tablespacename = defGetString(opt);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized REINDEX option \"%s\"",
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	if (concurrently)
		PreventInTransactionBlock(isTopLevel,
								  "REINDEX CONCURRENTLY");

	params.options =
		(verbose ? REINDEXOPT_VERBOSE : 0) |
		(concurrently ? REINDEXOPT_CONCURRENTLY : 0);

	/*
	 * Assign the tablespace OID to move indexes to, with InvalidOid to do
	 * nothing.
	 */
	if (tablespacename != NULL)
	{
		params.tablespaceOid = get_tablespace_oid(tablespacename, false);

		/* Check permissions except when moving to database's default */
		if (OidIsValid(params.tablespaceOid) &&
			params.tablespaceOid != MyDatabaseTableSpace)
		{
			AclResult	aclresult;

			aclresult = pg_tablespace_aclcheck(params.tablespaceOid,
											   GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLESPACE,
							   get_tablespace_name(params.tablespaceOid));
		}
	}
	else
		params.tablespaceOid = InvalidOid;

	switch (stmt->kind)
	{
		case REINDEX_OBJECT_INDEX:
			ReindexIndex(stmt->relation, &params, isTopLevel);
			break;
		case REINDEX_OBJECT_TABLE:
			ReindexTable(stmt->relation, &params, isTopLevel);
			break;
		case REINDEX_OBJECT_SCHEMA:
		case REINDEX_OBJECT_SYSTEM:
		case REINDEX_OBJECT_DATABASE:

			/*
			 * This cannot run inside a user transaction block; if we were
			 * inside a transaction, then its commit- and
			 * start-transaction-command calls would not have the intended
			 * effect!
			 */
			PreventInTransactionBlock(isTopLevel,
									  (stmt->kind == REINDEX_OBJECT_SCHEMA) ? "REINDEX SCHEMA" :
									  (stmt->kind == REINDEX_OBJECT_SYSTEM) ? "REINDEX SYSTEM" :
									  "REINDEX DATABASE");
			ReindexMultipleTables(stmt->name, stmt->kind, &params);
			break;
		default:
			elog(ERROR, "unrecognized object type: %d",
				 (int) stmt->kind);
			break;
	}
}

/*
 * ReindexIndex
 *		Recreate a specific index.
 */
static void
ReindexIndex(RangeVar *indexRelation, ReindexParams *params, bool isTopLevel)
{
	struct ReindexIndexCallbackState state;
	Oid			indOid;
	char		persistence;
	char		relkind;

	/*
	 * Find and lock index, and check permissions on table; use callback to
	 * obtain lock on table first, to avoid deadlock hazard.  The lock level
	 * used here must match the index lock obtained in reindex_index().
	 *
	 * If it's a temporary index, we will perform a non-concurrent reindex,
	 * even if CONCURRENTLY was requested.  In that case, reindex_index() will
	 * upgrade the lock, but that's OK, because other sessions can't hold
	 * locks on our temporary table.
	 */
	state.params = *params;
	state.locked_table_oid = InvalidOid;
	indOid = RangeVarGetRelidExtended(indexRelation,
									  (params->options & REINDEXOPT_CONCURRENTLY) != 0 ?
									  ShareUpdateExclusiveLock : AccessExclusiveLock,
									  0,
									  RangeVarCallbackForReindexIndex,
									  &state);

	/*
	 * Obtain the current persistence and kind of the existing index.  We
	 * already hold a lock on the index.
	 */
	persistence = get_rel_persistence(indOid);
	relkind = get_rel_relkind(indOid);

	if (relkind == RELKIND_PARTITIONED_INDEX)
		ReindexPartitions(indOid, params, isTopLevel);
	else if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			 persistence != RELPERSISTENCE_TEMP)
		ReindexRelationConcurrently(indOid, params);
	else
	{
		ReindexParams newparams = *params;

		newparams.options |= REINDEXOPT_REPORT_PROGRESS;
		reindex_index(indOid, false, persistence, &newparams);
	}
}

/*
 * Check permissions on table before acquiring relation lock; also lock
 * the heap before the RangeVarGetRelidExtended takes the index lock, to avoid
 * deadlocks.
 */
static void
RangeVarCallbackForReindexIndex(const RangeVar *relation,
								Oid relId, Oid oldRelId, void *arg)
{
	char		relkind;
	struct ReindexIndexCallbackState *state = arg;
	LOCKMODE	table_lockmode;

	/*
	 * Lock level here should match table lock in reindex_index() for
	 * non-concurrent case and table locks used by index_concurrently_*() for
	 * concurrent case.
	 */
	table_lockmode = (state->params.options & REINDEXOPT_CONCURRENTLY) != 0 ?
		ShareUpdateExclusiveLock : ShareLock;

	/*
	 * If we previously locked some other index's heap, and the name we're
	 * looking up no longer refers to that relation, release the now-useless
	 * lock.
	 */
	if (relId != oldRelId && OidIsValid(oldRelId))
	{
		UnlockRelationOid(state->locked_table_oid, table_lockmode);
		state->locked_table_oid = InvalidOid;
	}

	/* If the relation does not exist, there's nothing more to do. */
	if (!OidIsValid(relId))
		return;

	/*
	 * If the relation does exist, check whether it's an index.  But note that
	 * the relation might have been dropped between the time we did the name
	 * lookup and now.  In that case, there's nothing to do.
	 */
	relkind = get_rel_relkind(relId);
	if (!relkind)
		return;
	if (relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", relation->relname)));

	/* Check permissions */
	if (!pg_class_ownercheck(relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, relation->relname);

	/* Lock heap before index to avoid deadlock. */
	if (relId != oldRelId)
	{
		Oid			table_oid = IndexGetRelation(relId, true);

		/*
		 * If the OID isn't valid, it means the index was concurrently
		 * dropped, which is not a problem for us; just return normally.
		 */
		if (OidIsValid(table_oid))
		{
			LockRelationOid(table_oid, table_lockmode);
			state->locked_table_oid = table_oid;
		}
	}
}

/*
 * ReindexTable
 *		Recreate all indexes of a table (and of its toast table, if any)
 */
static Oid
ReindexTable(RangeVar *relation, ReindexParams *params, bool isTopLevel)
{
	Oid			heapOid;
	bool		result;

	/*
	 * The lock level used here should match reindex_relation().
	 *
	 * If it's a temporary table, we will perform a non-concurrent reindex,
	 * even if CONCURRENTLY was requested.  In that case, reindex_relation()
	 * will upgrade the lock, but that's OK, because other sessions can't hold
	 * locks on our temporary table.
	 */
	heapOid = RangeVarGetRelidExtended(relation,
									   (params->options & REINDEXOPT_CONCURRENTLY) != 0 ?
									   ShareUpdateExclusiveLock : ShareLock,
									   0,
									   RangeVarCallbackOwnsTable, NULL);

	if (get_rel_relkind(heapOid) == RELKIND_PARTITIONED_TABLE)
		ReindexPartitions(heapOid, params, isTopLevel);
	else if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			 get_rel_persistence(heapOid) != RELPERSISTENCE_TEMP)
	{
		result = ReindexRelationConcurrently(heapOid, params);

		if (!result)
			ereport(NOTICE,
					(errmsg("table \"%s\" has no indexes that can be reindexed concurrently",
							relation->relname)));
	}
	else
	{
		ReindexParams newparams = *params;

		newparams.options |= REINDEXOPT_REPORT_PROGRESS;
		result = reindex_relation(heapOid,
								  REINDEX_REL_PROCESS_TOAST |
								  REINDEX_REL_CHECK_CONSTRAINTS,
								  &newparams);
		if (!result)
			ereport(NOTICE,
					(errmsg("table \"%s\" has no indexes to reindex",
							relation->relname)));
	}

	return heapOid;
}

/*
 * ReindexMultipleTables
 *		Recreate indexes of tables selected by objectName/objectKind.
 *
 * To reduce the probability of deadlocks, each table is reindexed in a
 * separate transaction, so we can release the lock on it right away.
 * That means this must not be called within a user transaction block!
 */
static void
ReindexMultipleTables(const char *objectName, ReindexObjectType objectKind,
					  ReindexParams *params)
{
	Oid			objectOid;
	Relation	relationRelation;
	TableScanDesc scan;
	ScanKeyData scan_keys[1];
	HeapTuple	tuple;
	MemoryContext private_context;
	MemoryContext old;
	List	   *relids = NIL;
	int			num_keys;
	bool		concurrent_warning = false;
	bool		tablespace_warning = false;

	AssertArg(objectName);
	Assert(objectKind == REINDEX_OBJECT_SCHEMA ||
		   objectKind == REINDEX_OBJECT_SYSTEM ||
		   objectKind == REINDEX_OBJECT_DATABASE);

	if (objectKind == REINDEX_OBJECT_SYSTEM &&
		(params->options & REINDEXOPT_CONCURRENTLY) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex system catalogs concurrently")));

	/*
	 * Get OID of object to reindex, being the database currently being used
	 * by session for a database or for system catalogs, or the schema defined
	 * by caller. At the same time do permission checks that need different
	 * processing depending on the object type.
	 */
	if (objectKind == REINDEX_OBJECT_SCHEMA)
	{
		objectOid = get_namespace_oid(objectName, false);

		if (!pg_namespace_ownercheck(objectOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
						   objectName);
	}
	else
	{
		objectOid = MyDatabaseId;

		if (strcmp(objectName, get_database_name(objectOid)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("can only reindex the currently open database")));
		if (!pg_database_ownercheck(objectOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
						   objectName);
	}

	/*
	 * Create a memory context that will survive forced transaction commits we
	 * do below.  Since it is a child of PortalContext, it will go away
	 * eventually even if we suffer an error; there's no need for special
	 * abort cleanup logic.
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexMultipleTables",
											ALLOCSET_SMALL_SIZES);

	/*
	 * Define the search keys to find the objects to reindex. For a schema, we
	 * select target relations using relnamespace, something not necessary for
	 * a database-wide operation.
	 */
	if (objectKind == REINDEX_OBJECT_SCHEMA)
	{
		num_keys = 1;
		ScanKeyInit(&scan_keys[0],
					Anum_pg_class_relnamespace,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectOid));
	}
	else
		num_keys = 0;

	/*
	 * Scan pg_class to build a list of the relations we need to reindex.
	 *
	 * We only consider plain relations and materialized views here (toast
	 * rels will be processed indirectly by reindex_relation).
	 */
	relationRelation = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(relationRelation, num_keys, scan_keys);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classtuple = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relid = classtuple->oid;

		/*
		 * Only regular tables and matviews can have indexes, so ignore any
		 * other kind of relation.
		 *
		 * Partitioned tables/indexes are skipped but matching leaf partitions
		 * are processed.
		 */
		if (classtuple->relkind != RELKIND_RELATION &&
			classtuple->relkind != RELKIND_MATVIEW)
			continue;

		/* Skip temp tables of other backends; we can't reindex them at all */
		if (classtuple->relpersistence == RELPERSISTENCE_TEMP &&
			!isTempNamespace(classtuple->relnamespace))
			continue;

		/* Check user/system classification, and optionally skip */
		if (objectKind == REINDEX_OBJECT_SYSTEM &&
			!IsSystemClass(relid, classtuple))
			continue;

		/*
		 * The table can be reindexed if the user is superuser, the table
		 * owner, or the database/schema owner (but in the latter case, only
		 * if it's not a shared relation).  pg_class_ownercheck includes the
		 * superuser case, and depending on objectKind we already know that
		 * the user has permission to run REINDEX on this database or schema
		 * per the permission checks at the beginning of this routine.
		 */
		if (classtuple->relisshared &&
			!pg_class_ownercheck(relid, GetUserId()))
			continue;

		/*
		 * Skip system tables, since index_create() would reject indexing them
		 * concurrently (and it would likely fail if we tried).
		 */
		if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			IsCatalogRelationOid(relid))
		{
			if (!concurrent_warning)
				ereport(WARNING,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot reindex system catalogs concurrently, skipping all")));
			concurrent_warning = true;
			continue;
		}

		/*
		 * If a new tablespace is set, check if this relation has to be
		 * skipped.
		 */
		if (OidIsValid(params->tablespaceOid))
		{
			bool		skip_rel = false;

			/*
			 * Mapped relations cannot be moved to different tablespaces (in
			 * particular this eliminates all shared catalogs.).
			 */
			if (RELKIND_HAS_STORAGE(classtuple->relkind) &&
				!OidIsValid(classtuple->relfilenode))
				skip_rel = true;

			/*
			 * A system relation is always skipped, even with
			 * allow_system_table_mods enabled.
			 */
			if (IsSystemClass(relid, classtuple))
				skip_rel = true;

			if (skip_rel)
			{
				if (!tablespace_warning)
					ereport(WARNING,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("cannot move system relations, skipping all")));
				tablespace_warning = true;
				continue;
			}
		}

		/* Save the list of relation OIDs in private context */
		old = MemoryContextSwitchTo(private_context);

		/*
		 * We always want to reindex pg_class first if it's selected to be
		 * reindexed.  This ensures that if there is any corruption in
		 * pg_class' indexes, they will be fixed before we process any other
		 * tables.  This is critical because reindexing itself will try to
		 * update pg_class.
		 */
		if (relid == RelationRelationId)
			relids = lcons_oid(relid, relids);
		else
			relids = lappend_oid(relids, relid);

		MemoryContextSwitchTo(old);
	}
	table_endscan(scan);
	table_close(relationRelation, AccessShareLock);

	/*
	 * Process each relation listed in a separate transaction.  Note that this
	 * commits and then starts a new transaction immediately.
	 */
	ReindexMultipleInternal(relids, params);

	MemoryContextDelete(private_context);
}

/*
 * Error callback specific to ReindexPartitions().
 */
static void
reindex_error_callback(void *arg)
{
	ReindexErrorInfo *errinfo = (ReindexErrorInfo *) arg;

	Assert(RELKIND_HAS_PARTITIONS(errinfo->relkind));

	if (errinfo->relkind == RELKIND_PARTITIONED_TABLE)
		errcontext("while reindexing partitioned table \"%s.%s\"",
				   errinfo->relnamespace, errinfo->relname);
	else if (errinfo->relkind == RELKIND_PARTITIONED_INDEX)
		errcontext("while reindexing partitioned index \"%s.%s\"",
				   errinfo->relnamespace, errinfo->relname);
}

/*
 * ReindexPartitions
 *
 * Reindex a set of partitions, per the partitioned index or table given
 * by the caller.
 */
static void
ReindexPartitions(Oid relid, ReindexParams *params, bool isTopLevel)
{
	List	   *partitions = NIL;
	char		relkind = get_rel_relkind(relid);
	char	   *relname = get_rel_name(relid);
	char	   *relnamespace = get_namespace_name(get_rel_namespace(relid));
	MemoryContext reindex_context;
	List	   *inhoids;
	ListCell   *lc;
	ErrorContextCallback errcallback;
	ReindexErrorInfo errinfo;

	Assert(RELKIND_HAS_PARTITIONS(relkind));

	/*
	 * Check if this runs in a transaction block, with an error callback to
	 * provide more context under which a problem happens.
	 */
	errinfo.relname = pstrdup(relname);
	errinfo.relnamespace = pstrdup(relnamespace);
	errinfo.relkind = relkind;
	errcallback.callback = reindex_error_callback;
	errcallback.arg = (void *) &errinfo;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	PreventInTransactionBlock(isTopLevel,
							  relkind == RELKIND_PARTITIONED_TABLE ?
							  "REINDEX TABLE" : "REINDEX INDEX");

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/*
	 * Create special memory context for cross-transaction storage.
	 *
	 * Since it is a child of PortalContext, it will go away eventually even
	 * if we suffer an error so there is no need for special abort cleanup
	 * logic.
	 */
	reindex_context = AllocSetContextCreate(PortalContext, "Reindex",
											ALLOCSET_DEFAULT_SIZES);

	/* ShareLock is enough to prevent schema modifications */
	inhoids = find_all_inheritors(relid, ShareLock, NULL);

	/*
	 * The list of relations to reindex are the physical partitions of the
	 * tree so discard any partitioned table or index.
	 */
	foreach(lc, inhoids)
	{
		Oid			partoid = lfirst_oid(lc);
		char		partkind = get_rel_relkind(partoid);
		MemoryContext old_context;

		/*
		 * This discards partitioned tables, partitioned indexes and foreign
		 * tables.
		 */
		if (!RELKIND_HAS_STORAGE(partkind))
			continue;

		Assert(partkind == RELKIND_INDEX ||
			   partkind == RELKIND_RELATION);

		/* Save partition OID */
		old_context = MemoryContextSwitchTo(reindex_context);
		partitions = lappend_oid(partitions, partoid);
		MemoryContextSwitchTo(old_context);
	}

	/*
	 * Process each partition listed in a separate transaction.  Note that
	 * this commits and then starts a new transaction immediately.
	 */
	ReindexMultipleInternal(partitions, params);

	/*
	 * Clean up working storage --- note we must do this after
	 * StartTransactionCommand, else we might be trying to delete the active
	 * context!
	 */
	MemoryContextDelete(reindex_context);
}

/*
 * ReindexMultipleInternal
 *
 * Reindex a list of relations, each one being processed in its own
 * transaction.  This commits the existing transaction immediately,
 * and starts a new transaction when finished.
 */
static void
ReindexMultipleInternal(List *relids, ReindexParams *params)
{
	ListCell   *l;

	PopActiveSnapshot();
	CommitTransactionCommand();

	foreach(l, relids)
	{
		Oid			relid = lfirst_oid(l);
		char		relkind;
		char		relpersistence;

		StartTransactionCommand();

		/* functions in indexes may want a snapshot set */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* check if the relation still exists */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		{
			PopActiveSnapshot();
			CommitTransactionCommand();
			continue;
		}

		/*
		 * Check permissions except when moving to database's default if a new
		 * tablespace is chosen.  Note that this check also happens in
		 * ExecReindex(), but we do an extra check here as this runs across
		 * multiple transactions.
		 */
		if (OidIsValid(params->tablespaceOid) &&
			params->tablespaceOid != MyDatabaseTableSpace)
		{
			AclResult	aclresult;

			aclresult = pg_tablespace_aclcheck(params->tablespaceOid,
											   GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLESPACE,
							   get_tablespace_name(params->tablespaceOid));
		}

		relkind = get_rel_relkind(relid);
		relpersistence = get_rel_persistence(relid);

		/*
		 * Partitioned tables and indexes can never be processed directly, and
		 * a list of their leaves should be built first.
		 */
		Assert(!RELKIND_HAS_PARTITIONS(relkind));

		if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			relpersistence != RELPERSISTENCE_TEMP)
		{
			ReindexParams newparams = *params;

			newparams.options |= REINDEXOPT_MISSING_OK;
			(void) ReindexRelationConcurrently(relid, &newparams);
			/* ReindexRelationConcurrently() does the verbose output */
		}
		else if (relkind == RELKIND_INDEX)
		{
			ReindexParams newparams = *params;

			newparams.options |=
				REINDEXOPT_REPORT_PROGRESS | REINDEXOPT_MISSING_OK;
			reindex_index(relid, false, relpersistence, &newparams);
			PopActiveSnapshot();
			/* reindex_index() does the verbose output */
		}
		else
		{
			bool		result;
			ReindexParams newparams = *params;

			newparams.options |=
				REINDEXOPT_REPORT_PROGRESS | REINDEXOPT_MISSING_OK;
			result = reindex_relation(relid,
									  REINDEX_REL_PROCESS_TOAST |
									  REINDEX_REL_CHECK_CONSTRAINTS,
									  &newparams);

			if (result && (params->options & REINDEXOPT_VERBOSE) != 0)
				ereport(INFO,
						(errmsg("table \"%s.%s\" was reindexed",
								get_namespace_name(get_rel_namespace(relid)),
								get_rel_name(relid))));

			PopActiveSnapshot();
		}

		CommitTransactionCommand();
	}

	StartTransactionCommand();
}


/*
 * ReindexRelationConcurrently - process REINDEX CONCURRENTLY for given
 * relation OID
 *
 * 'relationOid' can either belong to an index, a table or a materialized
 * view.  For tables and materialized views, all its indexes will be rebuilt,
 * excluding invalid indexes and any indexes used in exclusion constraints,
 * but including its associated toast table indexes.  For indexes, the index
 * itself will be rebuilt.
 *
 * The locks taken on parent tables and involved indexes are kept until the
 * transaction is committed, at which point a session lock is taken on each
 * relation.  Both of these protect against concurrent schema changes.
 *
 * Returns true if any indexes have been rebuilt (including toast table's
 * indexes, when relevant), otherwise returns false.
 *
 * NOTE: This cannot be used on temporary relations.  A concurrent build would
 * cause issues with ON COMMIT actions triggered by the transactions of the
 * concurrent build.  Temporary relations are not subject to concurrent
 * concerns, so there's no need for the more complicated concurrent build,
 * anyway, and a non-concurrent reindex is more efficient.
 */
static bool
ReindexRelationConcurrently(Oid relationOid, ReindexParams *params)
{
	typedef struct ReindexIndexInfo
	{
		Oid			indexId;
		Oid			tableId;
		Oid			amId;
		bool		safe;		/* for set_indexsafe_procflags */
	} ReindexIndexInfo;
	List	   *heapRelationIds = NIL;
	List	   *indexIds = NIL;
	List	   *newIndexIds = NIL;
	List	   *relationLocks = NIL;
	List	   *lockTags = NIL;
	ListCell   *lc,
			   *lc2;
	MemoryContext private_context;
	MemoryContext oldcontext;
	char		relkind;
	char	   *relationName = NULL;
	char	   *relationNamespace = NULL;
	PGRUsage	ru0;
	const int	progress_index[] = {
		PROGRESS_CREATEIDX_COMMAND,
		PROGRESS_CREATEIDX_PHASE,
		PROGRESS_CREATEIDX_INDEX_OID,
		PROGRESS_CREATEIDX_ACCESS_METHOD_OID
	};
	int64		progress_vals[4];

	/*
	 * Create a memory context that will survive forced transaction commits we
	 * do below.  Since it is a child of PortalContext, it will go away
	 * eventually even if we suffer an error; there's no need for special
	 * abort cleanup logic.
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexConcurrent",
											ALLOCSET_SMALL_SIZES);

	if ((params->options & REINDEXOPT_VERBOSE) != 0)
	{
		/* Save data needed by REINDEX VERBOSE in private context */
		oldcontext = MemoryContextSwitchTo(private_context);

		relationName = get_rel_name(relationOid);
		relationNamespace = get_namespace_name(get_rel_namespace(relationOid));

		pg_rusage_init(&ru0);

		MemoryContextSwitchTo(oldcontext);
	}

	relkind = get_rel_relkind(relationOid);

	/*
	 * Extract the list of indexes that are going to be rebuilt based on the
	 * relation Oid given by caller.
	 */
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_TOASTVALUE:
			{
				/*
				 * In the case of a relation, find all its indexes including
				 * toast indexes.
				 */
				Relation	heapRelation;

				/* Save the list of relation OIDs in private context */
				oldcontext = MemoryContextSwitchTo(private_context);

				/* Track this relation for session locks */
				heapRelationIds = lappend_oid(heapRelationIds, relationOid);

				MemoryContextSwitchTo(oldcontext);

				if (IsCatalogRelationOid(relationOid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex system catalogs concurrently")));

				/* Open relation to get its indexes */
				if ((params->options & REINDEXOPT_MISSING_OK) != 0)
				{
					heapRelation = try_table_open(relationOid,
												  ShareUpdateExclusiveLock);
					/* leave if relation does not exist */
					if (!heapRelation)
						break;
				}
				else
					heapRelation = table_open(relationOid,
											  ShareUpdateExclusiveLock);

				if (OidIsValid(params->tablespaceOid) &&
					IsSystemRelation(heapRelation))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot move system relation \"%s\"",
									RelationGetRelationName(heapRelation))));

				/* Add all the valid indexes of relation to list */
				foreach(lc, RelationGetIndexList(heapRelation))
				{
					Oid			cellOid = lfirst_oid(lc);
					Relation	indexRelation = index_open(cellOid,
														   ShareUpdateExclusiveLock);

					if (!indexRelation->rd_index->indisvalid)
						ereport(WARNING,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot reindex invalid index \"%s.%s\" concurrently, skipping",
										get_namespace_name(get_rel_namespace(cellOid)),
										get_rel_name(cellOid))));
					else if (indexRelation->rd_index->indisexclusion)
						ereport(WARNING,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot reindex exclusion constraint index \"%s.%s\" concurrently, skipping",
										get_namespace_name(get_rel_namespace(cellOid)),
										get_rel_name(cellOid))));
					else
					{
						ReindexIndexInfo *idx;

						/* Save the list of relation OIDs in private context */
						oldcontext = MemoryContextSwitchTo(private_context);

						idx = palloc(sizeof(ReindexIndexInfo));
						idx->indexId = cellOid;
						/* other fields set later */

						indexIds = lappend(indexIds, idx);

						MemoryContextSwitchTo(oldcontext);
					}

					index_close(indexRelation, NoLock);
				}

				/* Also add the toast indexes */
				if (OidIsValid(heapRelation->rd_rel->reltoastrelid))
				{
					Oid			toastOid = heapRelation->rd_rel->reltoastrelid;
					Relation	toastRelation = table_open(toastOid,
														   ShareUpdateExclusiveLock);

					/* Save the list of relation OIDs in private context */
					oldcontext = MemoryContextSwitchTo(private_context);

					/* Track this relation for session locks */
					heapRelationIds = lappend_oid(heapRelationIds, toastOid);

					MemoryContextSwitchTo(oldcontext);

					foreach(lc2, RelationGetIndexList(toastRelation))
					{
						Oid			cellOid = lfirst_oid(lc2);
						Relation	indexRelation = index_open(cellOid,
															   ShareUpdateExclusiveLock);

						if (!indexRelation->rd_index->indisvalid)
							ereport(WARNING,
									(errcode(ERRCODE_INDEX_CORRUPTED),
									 errmsg("cannot reindex invalid index \"%s.%s\" concurrently, skipping",
											get_namespace_name(get_rel_namespace(cellOid)),
											get_rel_name(cellOid))));
						else
						{
							ReindexIndexInfo *idx;

							/*
							 * Save the list of relation OIDs in private
							 * context
							 */
							oldcontext = MemoryContextSwitchTo(private_context);

							idx = palloc(sizeof(ReindexIndexInfo));
							idx->indexId = cellOid;
							indexIds = lappend(indexIds, idx);
							/* other fields set later */

							MemoryContextSwitchTo(oldcontext);
						}

						index_close(indexRelation, NoLock);
					}

					table_close(toastRelation, NoLock);
				}

				table_close(heapRelation, NoLock);
				break;
			}
		case RELKIND_INDEX:
			{
				Oid			heapId = IndexGetRelation(relationOid,
													  (params->options & REINDEXOPT_MISSING_OK) != 0);
				Relation	heapRelation;
				ReindexIndexInfo *idx;

				/* if relation is missing, leave */
				if (!OidIsValid(heapId))
					break;

				if (IsCatalogRelationOid(heapId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex system catalogs concurrently")));

				/*
				 * Don't allow reindex for an invalid index on TOAST table, as
				 * if rebuilt it would not be possible to drop it.  Match
				 * error message in reindex_index().
				 */
				if (IsToastNamespace(get_rel_namespace(relationOid)) &&
					!get_index_isvalid(relationOid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex invalid index on TOAST table")));

				/*
				 * Check if parent relation can be locked and if it exists,
				 * this needs to be done at this stage as the list of indexes
				 * to rebuild is not complete yet, and REINDEXOPT_MISSING_OK
				 * should not be used once all the session locks are taken.
				 */
				if ((params->options & REINDEXOPT_MISSING_OK) != 0)
				{
					heapRelation = try_table_open(heapId,
												  ShareUpdateExclusiveLock);
					/* leave if relation does not exist */
					if (!heapRelation)
						break;
				}
				else
					heapRelation = table_open(heapId,
											  ShareUpdateExclusiveLock);

				if (OidIsValid(params->tablespaceOid) &&
					IsSystemRelation(heapRelation))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot move system relation \"%s\"",
									get_rel_name(relationOid))));

				table_close(heapRelation, NoLock);

				/* Save the list of relation OIDs in private context */
				oldcontext = MemoryContextSwitchTo(private_context);

				/* Track the heap relation of this index for session locks */
				heapRelationIds = list_make1_oid(heapId);

				/*
				 * Save the list of relation OIDs in private context.  Note
				 * that invalid indexes are allowed here.
				 */
				idx = palloc(sizeof(ReindexIndexInfo));
				idx->indexId = relationOid;
				indexIds = lappend(indexIds, idx);
				/* other fields set later */

				MemoryContextSwitchTo(oldcontext);
				break;
			}

		case RELKIND_PARTITIONED_TABLE:
		case RELKIND_PARTITIONED_INDEX:
		default:
			/* Return error if type of relation is not supported */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot reindex this type of relation concurrently")));
			break;
	}

	/*
	 * Definitely no indexes, so leave.  Any checks based on
	 * REINDEXOPT_MISSING_OK should be done only while the list of indexes to
	 * work on is built as the session locks taken before this transaction
	 * commits will make sure that they cannot be dropped by a concurrent
	 * session until this operation completes.
	 */
	if (indexIds == NIL)
	{
		PopActiveSnapshot();
		return false;
	}

	/* It's not a shared catalog, so refuse to move it to shared tablespace */
	if (params->tablespaceOid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move non-shared relation to tablespace \"%s\"",
						get_tablespace_name(params->tablespaceOid))));

	Assert(heapRelationIds != NIL);

	/*-----
	 * Now we have all the indexes we want to process in indexIds.
	 *
	 * The phases now are:
	 *
	 * 1. create new indexes in the catalog
	 * 2. build new indexes
	 * 3. let new indexes catch up with tuples inserted in the meantime
	 * 4. swap index names
	 * 5. mark old indexes as dead
	 * 6. drop old indexes
	 *
	 * We process each phase for all indexes before moving to the next phase,
	 * for efficiency.
	 */

	/*
	 * Phase 1 of REINDEX CONCURRENTLY
	 *
	 * Create a new index with the same properties as the old one, but it is
	 * only registered in catalogs and will be built later.  Then get session
	 * locks on all involved tables.  See analogous code in DefineIndex() for
	 * more detailed comments.
	 */

	foreach(lc, indexIds)
	{
		char	   *concurrentName;
		ReindexIndexInfo *idx = lfirst(lc);
		ReindexIndexInfo *newidx;
		Oid			newIndexId;
		Relation	indexRel;
		Relation	heapRel;
		Oid			save_userid;
		int			save_sec_context;
		int			save_nestlevel;
		Relation	newIndexRel;
		LockRelId  *lockrelid;
		Oid			tablespaceid;

		indexRel = index_open(idx->indexId, ShareUpdateExclusiveLock);
		heapRel = table_open(indexRel->rd_index->indrelid,
							 ShareUpdateExclusiveLock);

		/*
		 * Switch to the table owner's userid, so that any index functions are
		 * run as that user.  Also lock down security-restricted operations
		 * and arrange to make GUC variable changes local to this command.
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heapRel->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();

		/* determine safety of this index for set_indexsafe_procflags */
		idx->safe = (indexRel->rd_indexprs == NIL &&
					 indexRel->rd_indpred == NIL);
		idx->tableId = RelationGetRelid(heapRel);
		idx->amId = indexRel->rd_rel->relam;

		/* This function shouldn't be called for temporary relations. */
		if (indexRel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			elog(ERROR, "cannot reindex a temporary table concurrently");

		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  idx->tableId);

		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = 0;	/* initializing */
		progress_vals[2] = idx->indexId;
		progress_vals[3] = idx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		/* Choose a temporary relation name for the new index */
		concurrentName = ChooseRelationName(get_rel_name(idx->indexId),
											NULL,
											"ccnew",
											get_rel_namespace(indexRel->rd_index->indrelid),
											false);

		/* Choose the new tablespace, indexes of toast tables are not moved */
		if (OidIsValid(params->tablespaceOid) &&
			heapRel->rd_rel->relkind != RELKIND_TOASTVALUE)
			tablespaceid = params->tablespaceOid;
		else
			tablespaceid = indexRel->rd_rel->reltablespace;

		/* Create new index definition based on given index */
		newIndexId = index_concurrently_create_copy(heapRel,
													idx->indexId,
													tablespaceid,
													concurrentName);

		/*
		 * Now open the relation of the new index, a session-level lock is
		 * also needed on it.
		 */
		newIndexRel = index_open(newIndexId, ShareUpdateExclusiveLock);

		/*
		 * Save the list of OIDs and locks in private context
		 */
		oldcontext = MemoryContextSwitchTo(private_context);

		newidx = palloc(sizeof(ReindexIndexInfo));
		newidx->indexId = newIndexId;
		newidx->safe = idx->safe;
		newidx->tableId = idx->tableId;
		newidx->amId = idx->amId;

		newIndexIds = lappend(newIndexIds, newidx);

		/*
		 * Save lockrelid to protect each relation from drop then close
		 * relations. The lockrelid on parent relation is not taken here to
		 * avoid multiple locks taken on the same relation, instead we rely on
		 * parentRelationIds built earlier.
		 */
		lockrelid = palloc(sizeof(*lockrelid));
		*lockrelid = indexRel->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);
		lockrelid = palloc(sizeof(*lockrelid));
		*lockrelid = newIndexRel->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);

		MemoryContextSwitchTo(oldcontext);

		index_close(indexRel, NoLock);
		index_close(newIndexRel, NoLock);

		/* Roll back any GUC changes executed by index functions */
		AtEOXact_GUC(false, save_nestlevel);

		/* Restore userid and security context */
		SetUserIdAndSecContext(save_userid, save_sec_context);

		table_close(heapRel, NoLock);
	}

	/*
	 * Save the heap lock for following visibility checks with other backends
	 * might conflict with this session.
	 */
	foreach(lc, heapRelationIds)
	{
		Relation	heapRelation = table_open(lfirst_oid(lc), ShareUpdateExclusiveLock);
		LockRelId  *lockrelid;
		LOCKTAG    *heaplocktag;

		/* Save the list of locks in private context */
		oldcontext = MemoryContextSwitchTo(private_context);

		/* Add lockrelid of heap relation to the list of locked relations */
		lockrelid = palloc(sizeof(*lockrelid));
		*lockrelid = heapRelation->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);

		heaplocktag = (LOCKTAG *) palloc(sizeof(LOCKTAG));

		/* Save the LOCKTAG for this parent relation for the wait phase */
		SET_LOCKTAG_RELATION(*heaplocktag, lockrelid->dbId, lockrelid->relId);
		lockTags = lappend(lockTags, heaplocktag);

		MemoryContextSwitchTo(oldcontext);

		/* Close heap relation */
		table_close(heapRelation, NoLock);
	}

	/* Get a session-level lock on each table. */
	foreach(lc, relationLocks)
	{
		LockRelId  *lockrelid = (LockRelId *) lfirst(lc);

		LockRelationIdForSession(lockrelid, ShareUpdateExclusiveLock);
	}

	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * Because we don't take a snapshot in this transaction, there's no need
	 * to set the PROC_IN_SAFE_IC flag here.
	 */

	/*
	 * Phase 2 of REINDEX CONCURRENTLY
	 *
	 * Build the new indexes in a separate transaction for each index to avoid
	 * having open transactions for an unnecessary long time.  But before
	 * doing that, wait until no running transactions could have the table of
	 * the index open with the old list of indexes.  See "phase 2" in
	 * DefineIndex() for more details.
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_1);
	WaitForLockersMultiple(lockTags, ShareLock, true);
	CommitTransactionCommand();

	foreach(lc, newIndexIds)
	{
		ReindexIndexInfo *newidx = lfirst(lc);

		/* Start new transaction for this index's concurrent build */
		StartTransactionCommand();

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 */
		CHECK_FOR_INTERRUPTS();

		/* Tell concurrent indexing to ignore us, if index qualifies */
		if (newidx->safe)
			set_indexsafe_procflags();

		/* Set ActiveSnapshot since functions in the indexes may need it */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Update progress for the index to build, with the correct parent
		 * table involved.
		 */
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX, newidx->tableId);
		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = PROGRESS_CREATEIDX_PHASE_BUILD;
		progress_vals[2] = newidx->indexId;
		progress_vals[3] = newidx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		/* Perform concurrent build of new index */
		index_concurrently_build(newidx->tableId, newidx->indexId);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	StartTransactionCommand();

	/*
	 * Because we don't take a snapshot or Xid in this transaction, there's no
	 * need to set the PROC_IN_SAFE_IC flag here.
	 */

	/*
	 * Phase 3 of REINDEX CONCURRENTLY
	 *
	 * During this phase the old indexes catch up with any new tuples that
	 * were created during the previous phase.  See "phase 3" in DefineIndex()
	 * for more details.
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_2);
	WaitForLockersMultiple(lockTags, ShareLock, true);
	CommitTransactionCommand();

	foreach(lc, newIndexIds)
	{
		ReindexIndexInfo *newidx = lfirst(lc);
		TransactionId limitXmin;
		Snapshot	snapshot;

		StartTransactionCommand();

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 */
		CHECK_FOR_INTERRUPTS();

		/* Tell concurrent indexing to ignore us, if index qualifies */
		if (newidx->safe)
			set_indexsafe_procflags();

		/*
		 * Take the "reference snapshot" that will be used by validate_index()
		 * to filter candidate tuples.
		 */
		snapshot = RegisterSnapshot(GetTransactionSnapshot());
		PushActiveSnapshot(snapshot);

		/*
		 * Update progress for the index to build, with the correct parent
		 * table involved.
		 */
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  newidx->tableId);
		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = PROGRESS_CREATEIDX_PHASE_VALIDATE_IDXSCAN;
		progress_vals[2] = newidx->indexId;
		progress_vals[3] = newidx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		validate_index(newidx->tableId, newidx->indexId, snapshot);

		/*
		 * We can now do away with our active snapshot, we still need to save
		 * the xmin limit to wait for older snapshots.
		 */
		limitXmin = snapshot->xmin;

		PopActiveSnapshot();
		UnregisterSnapshot(snapshot);

		/*
		 * To ensure no deadlocks, we must commit and start yet another
		 * transaction, and do our wait before any snapshot has been taken in
		 * it.
		 */
		CommitTransactionCommand();
		StartTransactionCommand();

		/*
		 * The index is now valid in the sense that it contains all currently
		 * interesting tuples.  But since it might not contain tuples deleted
		 * just before the reference snap was taken, we have to wait out any
		 * transactions that might have older snapshots.
		 *
		 * Because we don't take a snapshot or Xid in this transaction,
		 * there's no need to set the PROC_IN_SAFE_IC flag here.
		 */
		pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
									 PROGRESS_CREATEIDX_PHASE_WAIT_3);
		WaitForOlderSnapshots(limitXmin, true);

		CommitTransactionCommand();
	}

	/*
	 * Phase 4 of REINDEX CONCURRENTLY
	 *
	 * Now that the new indexes have been validated, swap each new index with
	 * its corresponding old index.
	 *
	 * We mark the new indexes as valid and the old indexes as not valid at
	 * the same time to make sure we only get constraint violations from the
	 * indexes with the correct names.
	 */

	StartTransactionCommand();

	/*
	 * Because this transaction only does catalog manipulations and doesn't do
	 * any index operations, we can set the PROC_IN_SAFE_IC flag here
	 * unconditionally.
	 */
	set_indexsafe_procflags();

	forboth(lc, indexIds, lc2, newIndexIds)
	{
		ReindexIndexInfo *oldidx = lfirst(lc);
		ReindexIndexInfo *newidx = lfirst(lc2);
		char	   *oldName;

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 */
		CHECK_FOR_INTERRUPTS();

		/* Choose a relation name for old index */
		oldName = ChooseRelationName(get_rel_name(oldidx->indexId),
									 NULL,
									 "ccold",
									 get_rel_namespace(oldidx->tableId),
									 false);

		/*
		 * Swap old index with the new one.  This also marks the new one as
		 * valid and the old one as not valid.
		 */
		index_concurrently_swap(newidx->indexId, oldidx->indexId, oldName);

		/*
		 * Invalidate the relcache for the table, so that after this commit
		 * all sessions will refresh any cached plans that might reference the
		 * index.
		 */
		CacheInvalidateRelcacheByRelid(oldidx->tableId);

		/*
		 * CCI here so that subsequent iterations see the oldName in the
		 * catalog and can choose a nonconflicting name for their oldName.
		 * Otherwise, this could lead to conflicts if a table has two indexes
		 * whose names are equal for the first NAMEDATALEN-minus-a-few
		 * characters.
		 */
		CommandCounterIncrement();
	}

	/* Commit this transaction and make index swaps visible */
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * While we could set PROC_IN_SAFE_IC if all indexes qualified, there's no
	 * real need for that, because we only acquire an Xid after the wait is
	 * done, and that lasts for a very short period.
	 */

	/*
	 * Phase 5 of REINDEX CONCURRENTLY
	 *
	 * Mark the old indexes as dead.  First we must wait until no running
	 * transaction could be using the index for a query.  See also
	 * index_drop() for more details.
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_4);
	WaitForLockersMultiple(lockTags, AccessExclusiveLock, true);

	foreach(lc, indexIds)
	{
		ReindexIndexInfo *oldidx = lfirst(lc);

		/*
		 * Check for user-requested abort.  This is inside a transaction so as
		 * xact.c does not issue a useless WARNING, and ensures that
		 * session-level locks are cleaned up on abort.
		 */
		CHECK_FOR_INTERRUPTS();

		index_concurrently_set_dead(oldidx->tableId, oldidx->indexId);
	}

	/* Commit this transaction to make the updates visible. */
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * While we could set PROC_IN_SAFE_IC if all indexes qualified, there's no
	 * real need for that, because we only acquire an Xid after the wait is
	 * done, and that lasts for a very short period.
	 */

	/*
	 * Phase 6 of REINDEX CONCURRENTLY
	 *
	 * Drop the old indexes.
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_5);
	WaitForLockersMultiple(lockTags, AccessExclusiveLock, true);

	PushActiveSnapshot(GetTransactionSnapshot());

	{
		ObjectAddresses *objects = new_object_addresses();

		foreach(lc, indexIds)
		{
			ReindexIndexInfo *idx = lfirst(lc);
			ObjectAddress object;

			object.classId = RelationRelationId;
			object.objectId = idx->indexId;
			object.objectSubId = 0;

			add_exact_object_address(&object, objects);
		}

		/*
		 * Use PERFORM_DELETION_CONCURRENT_LOCK so that index_drop() uses the
		 * right lock level.
		 */
		performMultipleDeletions(objects, DROP_RESTRICT,
								 PERFORM_DELETION_CONCURRENT_LOCK | PERFORM_DELETION_INTERNAL);
	}

	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * Finally, release the session-level lock on the table.
	 */
	foreach(lc, relationLocks)
	{
		LockRelId  *lockrelid = (LockRelId *) lfirst(lc);

		UnlockRelationIdForSession(lockrelid, ShareUpdateExclusiveLock);
	}

	/* Start a new transaction to finish process properly */
	StartTransactionCommand();

	/* Log what we did */
	if ((params->options & REINDEXOPT_VERBOSE) != 0)
	{
		if (relkind == RELKIND_INDEX)
			ereport(INFO,
					(errmsg("index \"%s.%s\" was reindexed",
							relationNamespace, relationName),
					 errdetail("%s.",
							   pg_rusage_show(&ru0))));
		else
		{
			foreach(lc, newIndexIds)
			{
				ReindexIndexInfo *idx = lfirst(lc);
				Oid			indOid = idx->indexId;

				ereport(INFO,
						(errmsg("index \"%s.%s\" was reindexed",
								get_namespace_name(get_rel_namespace(indOid)),
								get_rel_name(indOid))));
				/* Don't show rusage here, since it's not per index. */
			}

			ereport(INFO,
					(errmsg("table \"%s.%s\" was reindexed",
							relationNamespace, relationName),
					 errdetail("%s.",
							   pg_rusage_show(&ru0))));
		}
	}

	MemoryContextDelete(private_context);

	pgstat_progress_end_command();

	return true;
}

/*
 * Insert or delete an appropriate pg_inherits tuple to make the given index
 * be a partition of the indicated parent index.
 *
 * This also corrects the pg_depend information for the affected index.
 */
void
IndexSetParentIndex(Relation partitionIdx, Oid parentOid)
{
	Relation	pg_inherits;
	ScanKeyData key[2];
	SysScanDesc scan;
	Oid			partRelid = RelationGetRelid(partitionIdx);
	HeapTuple	tuple;
	bool		fix_dependencies;

	/* Make sure this is an index */
	Assert(partitionIdx->rd_rel->relkind == RELKIND_INDEX ||
		   partitionIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

	/*
	 * Scan pg_inherits for rows linking our index to some parent.
	 */
	pg_inherits = relation_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partRelid));
	ScanKeyInit(&key[1],
				Anum_pg_inherits_inhseqno,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(1));
	scan = systable_beginscan(pg_inherits, InheritsRelidSeqnoIndexId, true,
							  NULL, 2, key);
	tuple = systable_getnext(scan);

	if (!HeapTupleIsValid(tuple))
	{
		if (parentOid == InvalidOid)
		{
			/*
			 * No pg_inherits row, and no parent wanted: nothing to do in this
			 * case.
			 */
			fix_dependencies = false;
		}
		else
		{
			StoreSingleInheritance(partRelid, parentOid, 1);
			fix_dependencies = true;
		}
	}
	else
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(tuple);

		if (parentOid == InvalidOid)
		{
			/*
			 * There exists a pg_inherits row, which we want to clear; do so.
			 */
			CatalogTupleDelete(pg_inherits, &tuple->t_self);
			fix_dependencies = true;
		}
		else
		{
			/*
			 * A pg_inherits row exists.  If it's the same we want, then we're
			 * good; if it differs, that amounts to a corrupt catalog and
			 * should not happen.
			 */
			if (inhForm->inhparent != parentOid)
			{
				/* unexpected: we should not get called in this case */
				elog(ERROR, "bogus pg_inherit row: inhrelid %u inhparent %u",
					 inhForm->inhrelid, inhForm->inhparent);
			}

			/* already in the right state */
			fix_dependencies = false;
		}
	}

	/* done with pg_inherits */
	systable_endscan(scan);
	relation_close(pg_inherits, RowExclusiveLock);

	/* set relhassubclass if an index partition has been added to the parent */
	if (OidIsValid(parentOid))
		SetRelationHasSubclass(parentOid, true);

	/* set relispartition correctly on the partition */
	update_relispartition(partRelid, OidIsValid(parentOid));

	if (fix_dependencies)
	{
		/*
		 * Insert/delete pg_depend rows.  If setting a parent, add PARTITION
		 * dependencies on the parent index and the table; if removing a
		 * parent, delete PARTITION dependencies.
		 */
		if (OidIsValid(parentOid))
		{
			ObjectAddress partIdx;
			ObjectAddress parentIdx;
			ObjectAddress partitionTbl;

			ObjectAddressSet(partIdx, RelationRelationId, partRelid);
			ObjectAddressSet(parentIdx, RelationRelationId, parentOid);
			ObjectAddressSet(partitionTbl, RelationRelationId,
							 partitionIdx->rd_index->indrelid);
			recordDependencyOn(&partIdx, &parentIdx,
							   DEPENDENCY_PARTITION_PRI);
			recordDependencyOn(&partIdx, &partitionTbl,
							   DEPENDENCY_PARTITION_SEC);
		}
		else
		{
			deleteDependencyRecordsForClass(RelationRelationId, partRelid,
											RelationRelationId,
											DEPENDENCY_PARTITION_PRI);
			deleteDependencyRecordsForClass(RelationRelationId, partRelid,
											RelationRelationId,
											DEPENDENCY_PARTITION_SEC);
		}

		/* make our updates visible */
		CommandCounterIncrement();
	}
}

/*
 * Subroutine of IndexSetParentIndex to update the relispartition flag of the
 * given index to the given value.
 */
static void
update_relispartition(Oid relationId, bool newval)
{
	HeapTuple	tup;
	Relation	classRel;

	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	Assert(((Form_pg_class) GETSTRUCT(tup))->relispartition != newval);
	((Form_pg_class) GETSTRUCT(tup))->relispartition = newval;
	CatalogTupleUpdate(classRel, &tup->t_self, tup);
	heap_freetuple(tup);
	table_close(classRel, RowExclusiveLock);
}

/*
 * Set the PROC_IN_SAFE_IC flag in MyProc->statusFlags.
 *
 * When doing concurrent index builds, we can set this flag
 * to tell other processes concurrently running CREATE
 * INDEX CONCURRENTLY or REINDEX CONCURRENTLY to ignore us when
 * doing their waits for concurrent snapshots.  On one hand it
 * avoids pointlessly waiting for a process that's not interesting
 * anyway; but more importantly it avoids deadlocks in some cases.
 *
 * This can be done safely only for indexes that don't execute any
 * expressions that could access other tables, so index must not be
 * expressional nor partial.  Caller is responsible for only calling
 * this routine when that assumption holds true.
 *
 * (The flag is reset automatically at transaction end, so it must be
 * set for each transaction.)
 */
static inline void
set_indexsafe_procflags(void)
{
	/*
	 * This should only be called before installing xid or xmin in MyProc;
	 * otherwise, concurrent processes could see an Xmin that moves backwards.
	 */
	Assert(MyProc->xid == InvalidTransactionId &&
		   MyProc->xmin == InvalidTransactionId);

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags |= PROC_IN_SAFE_IC;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}
