/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"


/* GUC variables */
int			SessionReplicationRole = SESSION_REPLICATION_ROLE_ORIGIN;

/* How many levels deep into trigger execution are we? */
static int	MyTriggerDepth = 0;

/* Local function prototypes */
static void renametrig_internal(Relation tgrel, Relation targetrel,
								HeapTuple trigtup, const char *newname,
								const char *expected_name);
static void renametrig_partition(Relation tgrel, Oid partitionId,
								 Oid parentTriggerOid, const char *newname,
								 const char *expected_name);
static void SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger);
static bool GetTupleForTrigger(EState *estate,
							   EPQState *epqstate,
							   ResultRelInfo *relinfo,
							   ItemPointer tid,
							   LockTupleMode lockmode,
							   TupleTableSlot *oldslot,
							   TupleTableSlot **epqslot,
							   TM_Result *tmresultp,
							   TM_FailureData *tmfdp);
static bool TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
						   Trigger *trigger, TriggerEvent event,
						   Bitmapset *modifiedCols,
						   TupleTableSlot *oldslot, TupleTableSlot *newslot);
static HeapTuple ExecCallTriggerFunc(TriggerData *trigdata,
									 int tgindx,
									 FmgrInfo *finfo,
									 Instrumentation *instr,
									 MemoryContext per_tuple_context);
static void AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
								  ResultRelInfo *src_partinfo,
								  ResultRelInfo *dst_partinfo,
								  int event, bool row_trigger,
								  TupleTableSlot *oldslot, TupleTableSlot *newslot,
								  List *recheckIndexes, Bitmapset *modifiedCols,
								  TransitionCaptureState *transition_capture,
								  bool is_crosspart_update);
static void AfterTriggerEnlargeQueryState(void);
static bool before_stmt_triggers_fired(Oid relid, CmdType cmdType);
static HeapTuple check_modified_virtual_generated(TupleDesc tupdesc, HeapTuple tuple);


/*
 * Create a trigger.  Returns the address of the created trigger.
 *
 * queryString is the source text of the CREATE TRIGGER command.
 * This must be supplied if a whenClause is specified, else it can be NULL.
 *
 * relOid, if nonzero, is the relation on which the trigger should be
 * created.  If zero, the name provided in the statement will be looked up.
 *
 * refRelOid, if nonzero, is the relation to which the constraint trigger
 * refers.  If zero, the constraint relation name provided in the statement
 * will be looked up as needed.
 *
 * constraintOid, if nonzero, says that this trigger is being created
 * internally to implement that constraint.  A suitable pg_depend entry will
 * be made to link the trigger to that constraint.  constraintOid is zero when
 * executing a user-entered CREATE TRIGGER command.  (For CREATE CONSTRAINT
 * TRIGGER, we build a pg_constraint entry internally.)
 *
 * indexOid, if nonzero, is the OID of an index associated with the constraint.
 * We do nothing with this except store it into pg_trigger.tgconstrindid;
 * but when creating a trigger for a deferrable unique constraint on a
 * partitioned table, its children are looked up.  Note we don't cope with
 * invalid indexes in that case.
 *
 * funcoid, if nonzero, is the OID of the function to invoke.  When this is
 * given, stmt->funcname is ignored.
 *
 * parentTriggerOid, if nonzero, is a trigger that begets this one; so that
 * if that trigger is dropped, this one should be too.  There are two cases
 * when a nonzero value is passed for this: 1) when this function recurses to
 * create the trigger on partitions, 2) when creating child foreign key
 * triggers; see CreateFKCheckTrigger() and createForeignKeyActionTriggers().
 *
 * If whenClause is passed, it is an already-transformed expression for
 * WHEN.  In this case, we ignore any that may come in stmt->whenClause.
 *
 * If isInternal is true then this is an internally-generated trigger.
 * This argument sets the tgisinternal field of the pg_trigger entry, and
 * if true causes us to modify the given trigger name to ensure uniqueness.
 *
 * When isInternal is not true we require ACL_TRIGGER permissions on the
 * relation, as well as ACL_EXECUTE on the trigger function.  For internal
 * triggers the caller must apply any required permission checks.
 *
 * When called on partitioned tables, this function recurses to create the
 * trigger on all the partitions, except if isInternal is true, in which
 * case caller is expected to execute recursion on its own.  in_partition
 * indicates such a recursive call; outside callers should pass "false"
 * (but see CloneRowTriggersToPartition).
 */
ObjectAddress
CreateTrigger(CreateTrigStmt *stmt, const char *queryString,
			  Oid relOid, Oid refRelOid, Oid constraintOid, Oid indexOid,
			  Oid funcoid, Oid parentTriggerOid, Node *whenClause,
			  bool isInternal, bool in_partition)
{
	return
		CreateTriggerFiringOn(stmt, queryString, relOid, refRelOid,
							  constraintOid, indexOid, funcoid,
							  parentTriggerOid, whenClause, isInternal,
							  in_partition, TRIGGER_FIRES_ON_ORIGIN);
}

/*
 * Like the above; additionally the firing condition
 * (always/origin/replica/disabled) can be specified.
 */
ObjectAddress
CreateTriggerFiringOn(CreateTrigStmt *stmt, const char *queryString,
					  Oid relOid, Oid refRelOid, Oid constraintOid,
					  Oid indexOid, Oid funcoid, Oid parentTriggerOid,
					  Node *whenClause, bool isInternal, bool in_partition,
					  char trigger_fires_when)
{
	int16		tgtype;
	int			ncolumns;
	int16	   *columns;
	int2vector *tgattr;
	List	   *whenRtable;
	char	   *qual;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	Relation	rel;
	AclResult	aclresult;
	Relation	tgrel;
	Relation	pgrel;
	HeapTuple	tuple = NULL;
	Oid			funcrettype;
	Oid			trigoid = InvalidOid;
	char		internaltrigname[NAMEDATALEN];
	char	   *trigname;
	Oid			constrrelid = InvalidOid;
	ObjectAddress myself,
				referenced;
	char	   *oldtablename = NULL;
	char	   *newtablename = NULL;
	bool		partition_recurse;
	bool		trigger_exists = false;
	Oid			existing_constraint_oid = InvalidOid;
	bool		existing_isInternal = false;
	bool		existing_isClone = false;

	if (OidIsValid(relOid))
		rel = table_open(relOid, ShareRowExclusiveLock);
	else
		rel = table_openrv(stmt->relation, ShareRowExclusiveLock);

	/*
	 * Triggers must be on tables or views, and there are additional
	 * relation-type-specific restrictions.
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		/* Tables can't have INSTEAD OF triggers */
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a table",
							RelationGetRelationName(rel)),
					 errdetail("Tables cannot have INSTEAD OF triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/* Partitioned tables can't have INSTEAD OF triggers */
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a table",
							RelationGetRelationName(rel)),
					 errdetail("Tables cannot have INSTEAD OF triggers.")));

		/*
		 * FOR EACH ROW triggers have further restrictions
		 */
		if (stmt->row)
		{
			/*
			 * Disallow use of transition tables.
			 *
			 * Note that we have another restriction about transition tables
			 * in partitions; search for 'has_superclass' below for an
			 * explanation.  The check here is just to protect from the fact
			 * that if we allowed it here, the creation would succeed for a
			 * partitioned table with no partitions, but would be blocked by
			 * the other restriction when the first partition was created,
			 * which is very unfriendly behavior.
			 */
			if (stmt->transitionRels != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"%s\" is a partitioned table",
								RelationGetRelationName(rel)),
						 errdetail("ROW triggers with transition tables are not supported on partitioned tables.")));
		}
	}
	else if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		/*
		 * Views can have INSTEAD OF triggers (which we check below are
		 * row-level), or statement-level BEFORE/AFTER triggers.
		 */
		if (stmt->timing != TRIGGER_TYPE_INSTEAD && stmt->row)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have row-level BEFORE or AFTER triggers.")));
		/* Disallow TRUNCATE triggers on VIEWs */
		if (TRIGGER_FOR_TRUNCATE(stmt->events))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have TRUNCATE triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
					 errdetail("Foreign tables cannot have INSTEAD OF triggers.")));

		/*
		 * We disallow constraint triggers to protect the assumption that
		 * triggers on FKs can't be deferred.  See notes with AfterTriggers
		 * data structures, below.
		 */
		if (stmt->isconstraint)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
					 errdetail("Foreign tables cannot have constraint triggers.")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	if (stmt->isconstraint)
	{
		/*
		 * We must take a lock on the target relation to protect against
		 * concurrent drop.  It's not clear that AccessShareLock is strong
		 * enough, but we certainly need at least that much... otherwise, we
		 * might end up creating a pg_constraint entry referencing a
		 * nonexistent table.
		 */
		if (OidIsValid(refRelOid))
		{
			LockRelationOid(refRelOid, AccessShareLock);
			constrrelid = refRelOid;
		}
		else if (stmt->constrrel != NULL)
			constrrelid = RangeVarGetRelid(stmt->constrrel, AccessShareLock,
										   false);
	}

	/* permission checks */
	if (!isInternal)
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_TRIGGER);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		if (OidIsValid(constrrelid))
		{
			aclresult = pg_class_aclcheck(constrrelid, GetUserId(),
										  ACL_TRIGGER);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(constrrelid)),
							   get_rel_name(constrrelid));
		}
	}

	/*
	 * When called on a partitioned table to create a FOR EACH ROW trigger
	 * that's not internal, we create one trigger for each partition, too.
	 *
	 * For that, we'd better hold lock on all of them ahead of time.
	 */
	partition_recurse = !isInternal && stmt->row &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
	if (partition_recurse)
		list_free(find_all_inheritors(RelationGetRelid(rel),
									  ShareRowExclusiveLock, NULL));

	/* Compute tgtype */
	TRIGGER_CLEAR_TYPE(tgtype);
	if (stmt->row)
		TRIGGER_SETT_ROW(tgtype);
	tgtype |= stmt->timing;
	tgtype |= stmt->events;

	/* Disallow ROW-level TRUNCATE triggers */
	if (TRIGGER_FOR_ROW(tgtype) && TRIGGER_FOR_TRUNCATE(tgtype))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("TRUNCATE FOR EACH ROW triggers are not supported")));

	/* INSTEAD triggers must be row-level, and can't have WHEN or columns */
	if (TRIGGER_FOR_INSTEAD(tgtype))
	{
		if (!TRIGGER_FOR_ROW(tgtype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers must be FOR EACH ROW")));
		if (stmt->whenClause)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers cannot have WHEN conditions")));
		if (stmt->columns != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers cannot have column lists")));
	}

	/*
	 * We don't yet support naming ROW transition variables, but the parser
	 * recognizes the syntax so we can give a nicer message here.
	 *
	 * Per standard, REFERENCING TABLE names are only allowed on AFTER
	 * triggers.  Per standard, REFERENCING ROW names are not allowed with FOR
	 * EACH STATEMENT.  Per standard, each OLD/NEW, ROW/TABLE permutation is
	 * only allowed once.  Per standard, OLD may not be specified when
	 * creating a trigger only for INSERT, and NEW may not be specified when
	 * creating a trigger only for DELETE.
	 *
	 * Notice that the standard allows an AFTER ... FOR EACH ROW trigger to
	 * reference both ROW and TABLE transition data.
	 */
	if (stmt->transitionRels != NIL)
	{
		List	   *varList = stmt->transitionRels;
		ListCell   *lc;

		foreach(lc, varList)
		{
			TriggerTransition *tt = lfirst_node(TriggerTransition, lc);

			if (!(tt->isTable))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("ROW variable naming in the REFERENCING clause is not supported"),
						 errhint("Use OLD TABLE or NEW TABLE for naming transition tables.")));

			/*
			 * Because of the above test, we omit further ROW-related testing
			 * below.  If we later allow naming OLD and NEW ROW variables,
			 * adjustments will be needed below.
			 */

			if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a foreign table",
								RelationGetRelationName(rel)),
						 errdetail("Triggers on foreign tables cannot have transition tables.")));

			if (rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a view",
								RelationGetRelationName(rel)),
						 errdetail("Triggers on views cannot have transition tables.")));

			/*
			 * We currently don't allow row-level triggers with transition
			 * tables on partition or inheritance children.  Such triggers
			 * would somehow need to see tuples converted to the format of the
			 * table they're attached to, and it's not clear which subset of
			 * tuples each child should see.  See also the prohibitions in
			 * ATExecAttachPartition() and ATExecAddInherit().
			 */
			if (TRIGGER_FOR_ROW(tgtype) && has_superclass(rel->rd_id))
			{
				/* Use appropriate error message. */
				if (rel->rd_rel->relispartition)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("ROW triggers with transition tables are not supported on partitions")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("ROW triggers with transition tables are not supported on inheritance children")));
			}

			if (stmt->timing != TRIGGER_TYPE_AFTER)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("transition table name can only be specified for an AFTER trigger")));

			if (TRIGGER_FOR_TRUNCATE(tgtype))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("TRUNCATE triggers with transition tables are not supported")));

			/*
			 * We currently don't allow multi-event triggers ("INSERT OR
			 * UPDATE") with transition tables, because it's not clear how to
			 * handle INSERT ... ON CONFLICT statements which can fire both
			 * INSERT and UPDATE triggers.  We show the inserted tuples to
			 * INSERT triggers and the updated tuples to UPDATE triggers, but
			 * it's not yet clear what INSERT OR UPDATE trigger should see.
			 * This restriction could be lifted if we can decide on the right
			 * semantics in a later release.
			 */
			if (((TRIGGER_FOR_INSERT(tgtype) ? 1 : 0) +
				 (TRIGGER_FOR_UPDATE(tgtype) ? 1 : 0) +
				 (TRIGGER_FOR_DELETE(tgtype) ? 1 : 0)) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("transition tables cannot be specified for triggers with more than one event")));

			/*
			 * We currently don't allow column-specific triggers with
			 * transition tables.  Per spec, that seems to require
			 * accumulating separate transition tables for each combination of
			 * columns, which is a lot of work for a rather marginal feature.
			 */
			if (stmt->columns != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("transition tables cannot be specified for triggers with column lists")));

			/*
			 * We disallow constraint triggers with transition tables, to
			 * protect the assumption that such triggers can't be deferred.
			 * See notes with AfterTriggers data structures, below.
			 *
			 * Currently this is enforced by the grammar, so just Assert here.
			 */
			Assert(!stmt->isconstraint);

			if (tt->isNew)
			{
				if (!(TRIGGER_FOR_INSERT(tgtype) ||
					  TRIGGER_FOR_UPDATE(tgtype)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("NEW TABLE can only be specified for an INSERT or UPDATE trigger")));

				if (newtablename != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("NEW TABLE cannot be specified multiple times")));

				newtablename = tt->name;
			}
			else
			{
				if (!(TRIGGER_FOR_DELETE(tgtype) ||
					  TRIGGER_FOR_UPDATE(tgtype)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("OLD TABLE can only be specified for a DELETE or UPDATE trigger")));

				if (oldtablename != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("OLD TABLE cannot be specified multiple times")));

				oldtablename = tt->name;
			}
		}

		if (newtablename != NULL && oldtablename != NULL &&
			strcmp(newtablename, oldtablename) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("OLD TABLE name and NEW TABLE name cannot be the same")));
	}

	/*
	 * Parse the WHEN clause, if any and we weren't passed an already
	 * transformed one.
	 *
	 * Note that as a side effect, we fill whenRtable when parsing.  If we got
	 * an already parsed clause, this does not occur, which is what we want --
	 * no point in adding redundant dependencies below.
	 */
	if (!whenClause && stmt->whenClause)
	{
		ParseState *pstate;
		ParseNamespaceItem *nsitem;
		List	   *varList;
		ListCell   *lc;

		/* Set up a pstate to parse with */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * Set up nsitems for OLD and NEW references.
		 *
		 * 'OLD' must always have varno equal to 1 and 'NEW' equal to 2.
		 */
		nsitem = addRangeTableEntryForRelation(pstate, rel,
											   AccessShareLock,
											   makeAlias("old", NIL),
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);
		nsitem = addRangeTableEntryForRelation(pstate, rel,
											   AccessShareLock,
											   makeAlias("new", NIL),
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		/* Transform expression.  Copy to be sure we don't modify original */
		whenClause = transformWhereClause(pstate,
										  copyObject(stmt->whenClause),
										  EXPR_KIND_TRIGGER_WHEN,
										  "WHEN");
		/* we have to fix its collations too */
		assign_expr_collations(pstate, whenClause);

		/*
		 * Check for disallowed references to OLD/NEW.
		 *
		 * NB: pull_var_clause is okay here only because we don't allow
		 * subselects in WHEN clauses; it would fail to examine the contents
		 * of subselects.
		 */
		varList = pull_var_clause(whenClause, 0);
		foreach(lc, varList)
		{
			Var		   *var = (Var *) lfirst(lc);

			switch (var->varno)
			{
				case PRS2_OLD_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_INSERT(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("INSERT trigger's WHEN condition cannot reference OLD values"),
								 parser_errposition(pstate, var->location)));
					/* system columns are okay here */
					break;
				case PRS2_NEW_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_DELETE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("DELETE trigger's WHEN condition cannot reference NEW values"),
								 parser_errposition(pstate, var->location)));
					if (var->varattno < 0 && TRIGGER_FOR_BEFORE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW system columns"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_BEFORE(tgtype) &&
						var->varattno == 0 &&
						RelationGetDescr(rel)->constr &&
						(RelationGetDescr(rel)->constr->has_generated_stored ||
						 RelationGetDescr(rel)->constr->has_generated_virtual))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW generated columns"),
								 errdetail("A whole-row reference is used and the table contains generated columns."),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_BEFORE(tgtype) &&
						var->varattno > 0 &&
						TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attgenerated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW generated columns"),
								 errdetail("Column \"%s\" is a generated column.",
										   NameStr(TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attname)),
								 parser_errposition(pstate, var->location)));
					break;
				default:
					/* can't happen without add_missing_from, so just elog */
					elog(ERROR, "trigger WHEN condition cannot contain references to other relations");
					break;
			}
		}

		/* we'll need the rtable for recordDependencyOnExpr */
		whenRtable = pstate->p_rtable;

		qual = nodeToString(whenClause);

		free_parsestate(pstate);
	}
	else if (!whenClause)
	{
		whenClause = NULL;
		whenRtable = NIL;
		qual = NULL;
	}
	else
	{
		qual = nodeToString(whenClause);
		whenRtable = NIL;
	}

	/*
	 * Find and validate the trigger function.
	 */
	if (!OidIsValid(funcoid))
		funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);
	if (!isInternal)
	{
		aclresult = object_aclcheck(ProcedureRelationId, funcoid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION,
						   NameListToString(stmt->funcname));
	}
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->funcname), "trigger")));

	/*
	 * Scan pg_trigger to see if there is already a trigger of the same name.
	 * Skip this for internally generated triggers, since we'll modify the
	 * name to be unique below.
	 *
	 * NOTE that this is cool only because we have ShareRowExclusiveLock on
	 * the relation, so the trigger set won't be changing underneath us.
	 */
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);
	if (!isInternal)
	{
		ScanKeyData skeys[2];
		SysScanDesc tgscan;

		ScanKeyInit(&skeys[0],
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(rel)));

		ScanKeyInit(&skeys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(stmt->trigname));

		tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
									NULL, 2, skeys);

		/* There should be at most one matching tuple */
		if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		{
			Form_pg_trigger oldtrigger = (Form_pg_trigger) GETSTRUCT(tuple);

			trigoid = oldtrigger->oid;
			existing_constraint_oid = oldtrigger->tgconstraint;
			existing_isInternal = oldtrigger->tgisinternal;
			existing_isClone = OidIsValid(oldtrigger->tgparentid);
			trigger_exists = true;
			/* copy the tuple to use in CatalogTupleUpdate() */
			tuple = heap_copytuple(tuple);
		}
		systable_endscan(tgscan);
	}

	if (!trigger_exists)
	{
		/* Generate the OID for the new trigger. */
		trigoid = GetNewOidWithIndex(tgrel, TriggerOidIndexId,
									 Anum_pg_trigger_oid);
	}
	else
	{
		/*
		 * If OR REPLACE was specified, we'll replace the old trigger;
		 * otherwise complain about the duplicate name.
		 */
		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" already exists",
							stmt->trigname, RelationGetRelationName(rel))));

		/*
		 * An internal trigger or a child trigger (isClone) cannot be replaced
		 * by a user-defined trigger.  However, skip this test when
		 * in_partition, because then we're recursing from a partitioned table
		 * and the check was made at the parent level.
		 */
		if ((existing_isInternal || existing_isClone) &&
			!isInternal && !in_partition)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" is an internal or a child trigger",
							stmt->trigname, RelationGetRelationName(rel))));

		/*
		 * It is not allowed to replace with a constraint trigger; gram.y
		 * should have enforced this already.
		 */
		Assert(!stmt->isconstraint);

		/*
		 * It is not allowed to replace an existing constraint trigger,
		 * either.  (The reason for these restrictions is partly that it seems
		 * difficult to deal with pending trigger events in such cases, and
		 * partly that the command might imply changing the constraint's
		 * properties as well, which doesn't seem nice.)
		 */
		if (OidIsValid(existing_constraint_oid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" is a constraint trigger",
							stmt->trigname, RelationGetRelationName(rel))));
	}

	/*
	 * If it's a user-entered CREATE CONSTRAINT TRIGGER command, make a
	 * corresponding pg_constraint entry.
	 */
	if (stmt->isconstraint && !OidIsValid(constraintOid))
	{
		/* Internal callers should have made their own constraints */
		Assert(!isInternal);
		constraintOid = CreateConstraintEntry(stmt->trigname,
											  RelationGetNamespace(rel),
											  CONSTRAINT_TRIGGER,
											  stmt->deferrable,
											  stmt->initdeferred,
											  true, /* Is Enforced */
											  true,
											  InvalidOid,	/* no parent */
											  RelationGetRelid(rel),
											  NULL, /* no conkey */
											  0,
											  0,
											  InvalidOid,	/* no domain */
											  InvalidOid,	/* no index */
											  InvalidOid,	/* no foreign key */
											  NULL,
											  NULL,
											  NULL,
											  NULL,
											  0,
											  ' ',
											  ' ',
											  NULL,
											  0,
											  ' ',
											  NULL, /* no exclusion */
											  NULL, /* no check constraint */
											  NULL,
											  true, /* islocal */
											  0,	/* inhcount */
											  true, /* noinherit */
											  false,	/* conperiod */
											  isInternal);	/* is_internal */
	}

	/*
	 * If trigger is internally generated, modify the provided trigger name to
	 * ensure uniqueness by appending the trigger OID.  (Callers will usually
	 * supply a simple constant trigger name in these cases.)
	 */
	if (isInternal)
	{
		snprintf(internaltrigname, sizeof(internaltrigname),
				 "%s_%u", stmt->trigname, trigoid);
		trigname = internaltrigname;
	}
	else
	{
		/* user-defined trigger; use the specified trigger name as-is */
		trigname = stmt->trigname;
	}

	/*
	 * Build the new pg_trigger tuple.
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_trigger_oid - 1] = ObjectIdGetDatum(trigoid);
	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_trigger_tgparentid - 1] = ObjectIdGetDatum(parentTriggerOid);
	values[Anum_pg_trigger_tgname - 1] = DirectFunctionCall1(namein,
															 CStringGetDatum(trigname));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum(tgtype);
	values[Anum_pg_trigger_tgenabled - 1] = trigger_fires_when;
	values[Anum_pg_trigger_tgisinternal - 1] = BoolGetDatum(isInternal);
	values[Anum_pg_trigger_tgconstrrelid - 1] = ObjectIdGetDatum(constrrelid);
	values[Anum_pg_trigger_tgconstrindid - 1] = ObjectIdGetDatum(indexOid);
	values[Anum_pg_trigger_tgconstraint - 1] = ObjectIdGetDatum(constraintOid);
	values[Anum_pg_trigger_tgdeferrable - 1] = BoolGetDatum(stmt->deferrable);
	values[Anum_pg_trigger_tginitdeferred - 1] = BoolGetDatum(stmt->initdeferred);

	if (stmt->args)
	{
		ListCell   *le;
		char	   *args;
		int16		nargs = list_length(stmt->args);
		int			len = 0;

		foreach(le, stmt->args)
		{
			char	   *ar = strVal(lfirst(le));

			len += strlen(ar) + 4;
			for (; *ar; ar++)
			{
				if (*ar == '\\')
					len++;
			}
		}
		args = (char *) palloc(len + 1);
		args[0] = '\0';
		foreach(le, stmt->args)
		{
			char	   *s = strVal(lfirst(le));
			char	   *d = args + strlen(args);

			while (*s)
			{
				if (*s == '\\')
					*d++ = '\\';
				*d++ = *s++;
			}
			strcpy(d, "\\000");
		}
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(nargs);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
																 CStringGetDatum(args));
	}
	else
	{
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(0);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
																 CStringGetDatum(""));
	}

	/* build column number array if it's a column-specific trigger */
	ncolumns = list_length(stmt->columns);
	if (ncolumns == 0)
		columns = NULL;
	else
	{
		ListCell   *cell;
		int			i = 0;

		columns = (int16 *) palloc(ncolumns * sizeof(int16));
		foreach(cell, stmt->columns)
		{
			char	   *name = strVal(lfirst(cell));
			int16		attnum;
			int			j;

			/* Lookup column name.  System columns are not allowed */
			attnum = attnameAttNum(rel, name, false);
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								name, RelationGetRelationName(rel))));

			/* Check for duplicates */
			for (j = i - 1; j >= 0; j--)
			{
				if (columns[j] == attnum)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name)));
			}

			columns[i++] = attnum;
		}
	}
	tgattr = buildint2vector(columns, ncolumns);
	values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum(tgattr);

	/* set tgqual if trigger has WHEN clause */
	if (qual)
		values[Anum_pg_trigger_tgqual - 1] = CStringGetTextDatum(qual);
	else
		nulls[Anum_pg_trigger_tgqual - 1] = true;

	if (oldtablename)
		values[Anum_pg_trigger_tgoldtable - 1] = DirectFunctionCall1(namein,
																	 CStringGetDatum(oldtablename));
	else
		nulls[Anum_pg_trigger_tgoldtable - 1] = true;
	if (newtablename)
		values[Anum_pg_trigger_tgnewtable - 1] = DirectFunctionCall1(namein,
																	 CStringGetDatum(newtablename));
	else
		nulls[Anum_pg_trigger_tgnewtable - 1] = true;

	/*
	 * Insert or replace tuple in pg_trigger.
	 */
	if (!trigger_exists)
	{
		tuple = heap_form_tuple(tgrel->rd_att, values, nulls);
		CatalogTupleInsert(tgrel, tuple);
	}
	else
	{
		HeapTuple	newtup;

		newtup = heap_form_tuple(tgrel->rd_att, values, nulls);
		CatalogTupleUpdate(tgrel, &tuple->t_self, newtup);
		heap_freetuple(newtup);
	}

	heap_freetuple(tuple);		/* free either original or new tuple */
	table_close(tgrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgattr - 1]));
	if (oldtablename)
		pfree(DatumGetPointer(values[Anum_pg_trigger_tgoldtable - 1]));
	if (newtablename)
		pfree(DatumGetPointer(values[Anum_pg_trigger_tgnewtable - 1]));

	/*
	 * Update relation's pg_class entry; if necessary; and if not, send an SI
	 * message to make other backends (and this one) rebuild relcache entries.
	 */
	pgrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));
	if (!((Form_pg_class) GETSTRUCT(tuple))->relhastriggers)
	{
		((Form_pg_class) GETSTRUCT(tuple))->relhastriggers = true;

		CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);

		CommandCounterIncrement();
	}
	else
		CacheInvalidateRelcacheByTuple(tuple);

	heap_freetuple(tuple);
	table_close(pgrel, RowExclusiveLock);

	/*
	 * If we're replacing a trigger, flush all the old dependencies before
	 * recording new ones.
	 */
	if (trigger_exists)
		deleteDependencyRecordsFor(TriggerRelationId, trigoid, true);

	/*
	 * Record dependencies for trigger.  Always place a normal dependency on
	 * the function.
	 */
	myself.classId = TriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (isInternal && OidIsValid(constraintOid))
	{
		/*
		 * Internally-generated trigger for a constraint, so make it an
		 * internal dependency of the constraint.  We can skip depending on
		 * the relation(s), as there'll be an indirect dependency via the
		 * constraint.
		 */
		referenced.classId = ConstraintRelationId;
		referenced.objectId = constraintOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
	else
	{
		/*
		 * User CREATE TRIGGER, so place dependencies.  We make trigger be
		 * auto-dropped if its relation is dropped or if the FK relation is
		 * dropped.  (Auto drop is compatible with our pre-7.3 behavior.)
		 */
		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

		if (OidIsValid(constrrelid))
		{
			referenced.classId = RelationRelationId;
			referenced.objectId = constrrelid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		}
		/* Not possible to have an index dependency in this case */
		Assert(!OidIsValid(indexOid));

		/*
		 * If it's a user-specified constraint trigger, make the constraint
		 * internally dependent on the trigger instead of vice versa.
		 */
		if (OidIsValid(constraintOid))
		{
			referenced.classId = ConstraintRelationId;
			referenced.objectId = constraintOid;
			referenced.objectSubId = 0;
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
		}

		/*
		 * If it's a partition trigger, create the partition dependencies.
		 */
		if (OidIsValid(parentTriggerOid))
		{
			ObjectAddressSet(referenced, TriggerRelationId, parentTriggerOid);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_PRI);
			ObjectAddressSet(referenced, RelationRelationId, RelationGetRelid(rel));
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_SEC);
		}
	}

	/* If column-specific trigger, add normal dependencies on columns */
	if (columns != NULL)
	{
		int			i;

		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		for (i = 0; i < ncolumns; i++)
		{
			referenced.objectSubId = columns[i];
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * If it has a WHEN clause, add dependencies on objects mentioned in the
	 * expression (eg, functions, as well as any columns used).
	 */
	if (whenRtable != NIL)
		recordDependencyOnExpr(&myself, whenClause, whenRtable,
							   DEPENDENCY_NORMAL);

	/* Post creation hook for new trigger */
	InvokeObjectPostCreateHookArg(TriggerRelationId, trigoid, 0,
								  isInternal);

	/*
	 * Lastly, create the trigger on child relations, if needed.
	 */
	if (partition_recurse)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);
		int			i;
		MemoryContext oldcxt,
					perChildCxt;

		perChildCxt = AllocSetContextCreate(CurrentMemoryContext,
											"part trig clone",
											ALLOCSET_SMALL_SIZES);

		/*
		 * We don't currently expect to be called with a valid indexOid.  If
		 * that ever changes then we'll need to write code here to find the
		 * corresponding child index.
		 */
		Assert(!OidIsValid(indexOid));

		oldcxt = MemoryContextSwitchTo(perChildCxt);

		/* Iterate to create the trigger on each existing partition */
		for (i = 0; i < partdesc->nparts; i++)
		{
			CreateTrigStmt *childStmt;
			Relation	childTbl;
			Node	   *qual;

			childTbl = table_open(partdesc->oids[i], ShareRowExclusiveLock);

			/*
			 * Initialize our fabricated parse node by copying the original
			 * one, then resetting fields that we pass separately.
			 */
			childStmt = copyObject(stmt);
			childStmt->funcname = NIL;
			childStmt->whenClause = NULL;

			/* If there is a WHEN clause, create a modified copy of it */
			qual = copyObject(whenClause);
			qual = (Node *)
				map_partition_varattnos((List *) qual, PRS2_OLD_VARNO,
										childTbl, rel);
			qual = (Node *)
				map_partition_varattnos((List *) qual, PRS2_NEW_VARNO,
										childTbl, rel);

			CreateTriggerFiringOn(childStmt, queryString,
								  partdesc->oids[i], refRelOid,
								  InvalidOid, InvalidOid,
								  funcoid, trigoid, qual,
								  isInternal, true, trigger_fires_when);

			table_close(childTbl, NoLock);

			MemoryContextReset(perChildCxt);
		}

		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(perChildCxt);
	}

	/* Keep lock on target rel until end of xact */
	table_close(rel, NoLock);

	return myself;
}

/*
 * TriggerSetParentTrigger
 *		Set a partition's trigger as child of its parent trigger,
 *		or remove the linkage if parentTrigId is InvalidOid.
 *
 * This updates the constraint's pg_trigger row to show it as inherited, and
 * adds PARTITION dependencies to prevent the trigger from being deleted
 * on its own.  Alternatively, reverse that.
 */
void
TriggerSetParentTrigger(Relation trigRel,
						Oid childTrigId,
						Oid parentTrigId,
						Oid childTableId)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	Form_pg_trigger trigForm;
	HeapTuple	tuple,
				newtup;
	ObjectAddress depender;
	ObjectAddress referenced;

	/*
	 * Find the trigger to delete.
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(childTrigId));

	tgscan = systable_beginscan(trigRel, TriggerOidIndexId, true,
								NULL, 1, skey);

	tuple = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for trigger %u", childTrigId);
	newtup = heap_copytuple(tuple);
	trigForm = (Form_pg_trigger) GETSTRUCT(newtup);
	if (OidIsValid(parentTrigId))
	{
		/* don't allow setting parent for a constraint that already has one */
		if (OidIsValid(trigForm->tgparentid))
			elog(ERROR, "trigger %u already has a parent trigger",
				 childTrigId);

		trigForm->tgparentid = parentTrigId;

		CatalogTupleUpdate(trigRel, &tuple->t_self, newtup);

		ObjectAddressSet(depender, TriggerRelationId, childTrigId);

		ObjectAddressSet(referenced, TriggerRelationId, parentTrigId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_PRI);

		ObjectAddressSet(referenced, RelationRelationId, childTableId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_SEC);
	}
	else
	{
		trigForm->tgparentid = InvalidOid;

		CatalogTupleUpdate(trigRel, &tuple->t_self, newtup);

		deleteDependencyRecordsForClass(TriggerRelationId, childTrigId,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, childTrigId,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);
	}

	heap_freetuple(newtup);
	systable_endscan(tgscan);
}


/*
 * Guts of trigger deletion.
 */
void
RemoveTriggerById(Oid trigOid)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Oid			relid;
	Relation	rel;

	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Find the trigger to delete.
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								NULL, 1, skey);

	tup = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for trigger %u", trigOid);

	/*
	 * Open and exclusive-lock the relation the trigger belongs to.
	 */
	relid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;

	rel = table_open(relid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_VIEW &&
		rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/*
	 * Delete the pg_trigger tuple.
	 */
	CatalogTupleDelete(tgrel, &tup->t_self);

	systable_endscan(tgscan);
	table_close(tgrel, RowExclusiveLock);

	/*
	 * We do not bother to try to determine whether any other triggers remain,
	 * which would be needed in order to decide whether it's safe to clear the
	 * relation's relhastriggers.  (In any case, there might be a concurrent
	 * process adding new triggers.)  Instead, just force a relcache inval to
	 * make other backends (and this one too!) rebuild their relcache entries.
	 * There's no great harm in leaving relhastriggers true even if there are
	 * no triggers left.
	 */
	CacheInvalidateRelcache(rel);

	/* Keep lock on trigger's rel until end of xact */
	table_close(rel, NoLock);
}

/*
 * get_trigger_oid - Look up a trigger by name to find its OID.
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 */
Oid
get_trigger_oid(Oid relid, const char *trigname, bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[2];
	SysScanDesc tgscan;
	HeapTuple	tup;
	Oid			oid;

	/*
	 * Find the trigger, verify permissions, set up object address
	 */
	tgrel = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("trigger \"%s\" for table \"%s\" does not exist",
							trigname, get_rel_name(relid))));
		oid = InvalidOid;
	}
	else
	{
		oid = ((Form_pg_trigger) GETSTRUCT(tup))->oid;
	}

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);
	return oid;
}

/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 */
static void
RangeVarCallbackForRenameTrigger(const RangeVar *rv, Oid relid, Oid oldrelid,
								 void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
	form = (Form_pg_class) GETSTRUCT(tuple);

	/* only tables and views can have triggers */
	if (form->relkind != RELKIND_RELATION && form->relkind != RELKIND_VIEW &&
		form->relkind != RELKIND_FOREIGN_TABLE &&
		form->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						rv->relname),
				 errdetail_relkind_not_supported(form->relkind)));

	/* you must own the table to rename one of its triggers */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);
	if (!allowSystemTableMods && IsSystemClass(relid, form))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	ReleaseSysCache(tuple);
}

/*
 *		renametrig		- changes the name of a trigger on a relation
 *
 *		trigger name is changed in trigger catalog.
 *		No record of the previous name is kept.
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan trigger catalog
 *				for name conflict (within rel)
 *				for original trigger (if not arg)
 *		modify tgname in trigger tuple
 *		update row in catalog
 */
ObjectAddress
renametrig(RenameStmt *stmt)
{
	Oid			tgoid;
	Relation	targetrel;
	Relation	tgrel;
	HeapTuple	tuple;
	SysScanDesc tgscan;
	ScanKeyData key[2];
	Oid			relid;
	ObjectAddress address;

	/*
	 * Look up name, check permissions, and acquire lock (which we will NOT
	 * release until end of transaction).
	 */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 0,
									 RangeVarCallbackForRenameTrigger,
									 NULL);

	/* Have lock already, so just need to build relcache entry. */
	targetrel = relation_open(relid, NoLock);

	/*
	 * On partitioned tables, this operation recurses to partitions.  Lock all
	 * tables upfront.
	 */
	if (targetrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		(void) find_all_inheritors(relid, AccessExclusiveLock, NULL);

	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Search for the trigger to modify.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(stmt->subname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger trigform;

		trigform = (Form_pg_trigger) GETSTRUCT(tuple);
		tgoid = trigform->oid;

		/*
		 * If the trigger descends from a trigger on a parent partitioned
		 * table, reject the rename.  We don't allow a trigger in a partition
		 * to differ in name from that of its parent: that would lead to an
		 * inconsistency that pg_dump would not reproduce.
		 */
		if (OidIsValid(trigform->tgparentid))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot rename trigger \"%s\" on table \"%s\"",
						   stmt->subname, RelationGetRelationName(targetrel)),
					errhint("Rename the trigger on the partitioned table \"%s\" instead.",
							get_rel_name(get_partition_parent(relid, false))));


		/* Rename the trigger on this relation ... */
		renametrig_internal(tgrel, targetrel, tuple, stmt->newname,
							stmt->subname);

		/* ... and if it is partitioned, recurse to its partitions */
		if (targetrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(targetrel, true);

			for (int i = 0; i < partdesc->nparts; i++)
			{
				Oid			partitionId = partdesc->oids[i];

				renametrig_partition(tgrel, partitionId, trigform->oid,
									 stmt->newname, stmt->subname);
			}
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						stmt->subname, RelationGetRelationName(targetrel))));
	}

	ObjectAddressSet(address, TriggerRelationId, tgoid);

	systable_endscan(tgscan);

	table_close(tgrel, RowExclusiveLock);

	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrel, NoLock);

	return address;
}

/*
 * Subroutine for renametrig -- perform the actual work of renaming one
 * trigger on one table.
 *
 * If the trigger has a name different from the expected one, raise a
 * NOTICE about it.
 */
static void
renametrig_internal(Relation tgrel, Relation targetrel, HeapTuple trigtup,
					const char *newname, const char *expected_name)
{
	HeapTuple	tuple;
	Form_pg_trigger tgform;
	ScanKeyData key[2];
	SysScanDesc tgscan;

	/* If the trigger already has the new name, nothing to do. */
	tgform = (Form_pg_trigger) GETSTRUCT(trigtup);
	if (strcmp(NameStr(tgform->tgname), newname) == 0)
		return;

	/*
	 * Before actually trying the rename, search for triggers with the same
	 * name.  The update would fail with an ugly message in that case, and it
	 * is better to throw a nicer error.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(targetrel)));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(newname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("trigger \"%s\" for relation \"%s\" already exists",
						newname, RelationGetRelationName(targetrel))));
	systable_endscan(tgscan);

	/*
	 * The target name is free; update the existing pg_trigger tuple with it.
	 */
	tuple = heap_copytuple(trigtup);	/* need a modifiable copy */
	tgform = (Form_pg_trigger) GETSTRUCT(tuple);

	/*
	 * If the trigger has a name different from what we expected, let the user
	 * know. (We can proceed anyway, since we must have reached here following
	 * a tgparentid link.)
	 */
	if (strcmp(NameStr(tgform->tgname), expected_name) != 0)
		ereport(NOTICE,
				errmsg("renamed trigger \"%s\" on relation \"%s\"",
					   NameStr(tgform->tgname),
					   RelationGetRelationName(targetrel)));

	namestrcpy(&tgform->tgname, newname);

	CatalogTupleUpdate(tgrel, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(TriggerRelationId, tgform->oid, 0);

	/*
	 * Invalidate relation's relcache entry so that other backends (and this
	 * one too!) are sent SI message to make them rebuild relcache entries.
	 * (Ideally this should happen automatically...)
	 */
	CacheInvalidateRelcache(targetrel);
}

/*
 * Subroutine for renametrig -- Helper for recursing to partitions when
 * renaming triggers on a partitioned table.
 */
static void
renametrig_partition(Relation tgrel, Oid partitionId, Oid parentTriggerOid,
					 const char *newname, const char *expected_name)
{
	SysScanDesc tgscan;
	ScanKeyData key;
	HeapTuple	tuple;

	/*
	 * Given a relation and the OID of a trigger on parent relation, find the
	 * corresponding trigger in the child and rename that trigger to the given
	 * name.
	 */
	ScanKeyInit(&key,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partitionId));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger tgform = (Form_pg_trigger) GETSTRUCT(tuple);
		Relation	partitionRel;

		if (tgform->tgparentid != parentTriggerOid)
			continue;			/* not our trigger */

		partitionRel = table_open(partitionId, NoLock);

		/* Rename the trigger on this partition */
		renametrig_internal(tgrel, partitionRel, tuple, newname, expected_name);

		/* And if this relation is partitioned, recurse to its partitions */
		if (partitionRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(partitionRel,
															  true);

			for (int i = 0; i < partdesc->nparts; i++)
			{
				Oid			partoid = partdesc->oids[i];

				renametrig_partition(tgrel, partoid, tgform->oid, newname,
									 NameStr(tgform->tgname));
			}
		}
		table_close(partitionRel, NoLock);

		/* There should be at most one matching tuple */
		break;
	}
	systable_endscan(tgscan);
}

/*
 * EnableDisableTrigger()
 *
 *	Called by ALTER TABLE ENABLE/DISABLE [ REPLICA | ALWAYS ] TRIGGER
 *	to change 'tgenabled' field for the specified trigger(s)
 *
 * rel: relation to process (caller must hold suitable lock on it)
 * tgname: name of trigger to process, or NULL to scan all triggers
 * tgparent: if not zero, process only triggers with this tgparentid
 * fires_when: new value for tgenabled field. In addition to generic
 *			   enablement/disablement, this also defines when the trigger
 *			   should be fired in session replication roles.
 * skip_system: if true, skip "system" triggers (constraint triggers)
 * recurse: if true, recurse to partitions
 *
 * Caller should have checked permissions for the table; here we also
 * enforce that superuser privilege is required to alter the state of
 * system triggers
 */
void
EnableDisableTrigger(Relation rel, const char *tgname, Oid tgparent,
					 char fires_when, bool skip_system, bool recurse,
					 LOCKMODE lockmode)
{
	Relation	tgrel;
	int			nkeys;
	ScanKeyData keys[2];
	SysScanDesc tgscan;
	HeapTuple	tuple;
	bool		found;
	bool		changed;

	/* Scan the relevant entries in pg_triggers */
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	ScanKeyInit(&keys[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	if (tgname)
	{
		ScanKeyInit(&keys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(tgname));
		nkeys = 2;
	}
	else
		nkeys = 1;

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, nkeys, keys);

	found = changed = false;

	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger oldtrig = (Form_pg_trigger) GETSTRUCT(tuple);

		if (OidIsValid(tgparent) && tgparent != oldtrig->tgparentid)
			continue;

		if (oldtrig->tgisinternal)
		{
			/* system trigger ... ok to process? */
			if (skip_system)
				continue;
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied: \"%s\" is a system trigger",
								NameStr(oldtrig->tgname))));
		}

		found = true;

		if (oldtrig->tgenabled != fires_when)
		{
			/* need to change this one ... make a copy to scribble on */
			HeapTuple	newtup = heap_copytuple(tuple);
			Form_pg_trigger newtrig = (Form_pg_trigger) GETSTRUCT(newtup);

			newtrig->tgenabled = fires_when;

			CatalogTupleUpdate(tgrel, &newtup->t_self, newtup);

			heap_freetuple(newtup);

			changed = true;
		}

		/*
		 * When altering FOR EACH ROW triggers on a partitioned table, do the
		 * same on the partitions as well, unless ONLY is specified.
		 *
		 * Note that we recurse even if we didn't change the trigger above,
		 * because the partitions' copy of the trigger may have a different
		 * value of tgenabled than the parent's trigger and thus might need to
		 * be changed.
		 */
		if (recurse &&
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
			(TRIGGER_FOR_ROW(oldtrig->tgtype)))
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);
			int			i;

			for (i = 0; i < partdesc->nparts; i++)
			{
				Relation	part;

				part = relation_open(partdesc->oids[i], lockmode);
				/* Match on child triggers' tgparentid, not their name */
				EnableDisableTrigger(part, NULL, oldtrig->oid,
									 fires_when, skip_system, recurse,
									 lockmode);
				table_close(part, NoLock);	/* keep lock till commit */
			}
		}

		InvokeObjectPostAlterHook(TriggerRelationId,
								  oldtrig->oid, 0);
	}

	systable_endscan(tgscan);

	table_close(tgrel, RowExclusiveLock);

	if (tgname && !found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						tgname, RelationGetRelationName(rel))));

	/*
	 * If we changed anything, broadcast a SI inval message to force each
	 * backend (including our own!) to rebuild relation's relcache entry.
	 * Otherwise they will fail to apply the change promptly.
	 */
	if (changed)
		CacheInvalidateRelcache(rel);
}


/*
 * Build trigger data to attach to the given relcache entry.
 *
 * Note that trigger data attached to a relcache entry must be stored in
 * CacheMemoryContext to ensure it survives as long as the relcache entry.
 * But we should be running in a less long-lived working context.  To avoid
 * leaking cache memory if this routine fails partway through, we build a
 * temporary TriggerDesc in working memory and then copy the completed
 * structure into cache memory.
 */
void
RelationBuildTriggers(Relation relation)
{
	TriggerDesc *trigdesc;
	int			numtrigs;
	int			maxtrigs;
	Trigger    *triggers;
	Relation	tgrel;
	ScanKeyData skey;
	SysScanDesc tgscan;
	HeapTuple	htup;
	MemoryContext oldContext;
	int			i;

	/*
	 * Allocate a working array to hold the triggers (the array is extended if
	 * necessary)
	 */
	maxtrigs = 16;
	triggers = (Trigger *) palloc(maxtrigs * sizeof(Trigger));
	numtrigs = 0;

	/*
	 * Note: since we scan the triggers using TriggerRelidNameIndexId, we will
	 * be reading the triggers in name order, except possibly during
	 * emergency-recovery operations (ie, IgnoreSystemIndexes). This in turn
	 * ensures that triggers will be fired in name order.
	 */
	ScanKeyInit(&skey,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	tgrel = table_open(TriggerRelationId, AccessShareLock);
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);
		Trigger    *build;
		Datum		datum;
		bool		isnull;

		if (numtrigs >= maxtrigs)
		{
			maxtrigs *= 2;
			triggers = (Trigger *) repalloc(triggers, maxtrigs * sizeof(Trigger));
		}
		build = &(triggers[numtrigs]);

		build->tgoid = pg_trigger->oid;
		build->tgname = DatumGetCString(DirectFunctionCall1(nameout,
															NameGetDatum(&pg_trigger->tgname)));
		build->tgfoid = pg_trigger->tgfoid;
		build->tgtype = pg_trigger->tgtype;
		build->tgenabled = pg_trigger->tgenabled;
		build->tgisinternal = pg_trigger->tgisinternal;
		build->tgisclone = OidIsValid(pg_trigger->tgparentid);
		build->tgconstrrelid = pg_trigger->tgconstrrelid;
		build->tgconstrindid = pg_trigger->tgconstrindid;
		build->tgconstraint = pg_trigger->tgconstraint;
		build->tgdeferrable = pg_trigger->tgdeferrable;
		build->tginitdeferred = pg_trigger->tginitdeferred;
		build->tgnargs = pg_trigger->tgnargs;
		/* tgattr is first var-width field, so OK to access directly */
		build->tgnattr = pg_trigger->tgattr.dim1;
		if (build->tgnattr > 0)
		{
			build->tgattr = (int16 *) palloc(build->tgnattr * sizeof(int16));
			memcpy(build->tgattr, &(pg_trigger->tgattr.values),
				   build->tgnattr * sizeof(int16));
		}
		else
			build->tgattr = NULL;
		if (build->tgnargs > 0)
		{
			bytea	   *val;
			char	   *p;

			val = DatumGetByteaPP(fastgetattr(htup,
											  Anum_pg_trigger_tgargs,
											  tgrel->rd_att, &isnull));
			if (isnull)
				elog(ERROR, "tgargs is null in trigger for relation \"%s\"",
					 RelationGetRelationName(relation));
			p = (char *) VARDATA_ANY(val);
			build->tgargs = (char **) palloc(build->tgnargs * sizeof(char *));
			for (i = 0; i < build->tgnargs; i++)
			{
				build->tgargs[i] = pstrdup(p);
				p += strlen(p) + 1;
			}
		}
		else
			build->tgargs = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgoldtable,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgoldtable =
				DatumGetCString(DirectFunctionCall1(nameout, datum));
		else
			build->tgoldtable = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgnewtable,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgnewtable =
				DatumGetCString(DirectFunctionCall1(nameout, datum));
		else
			build->tgnewtable = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgqual,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgqual = TextDatumGetCString(datum);
		else
			build->tgqual = NULL;

		numtrigs++;
	}

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);

	/* There might not be any triggers */
	if (numtrigs == 0)
	{
		pfree(triggers);
		return;
	}

	/* Build trigdesc */
	trigdesc = (TriggerDesc *) palloc0(sizeof(TriggerDesc));
	trigdesc->triggers = triggers;
	trigdesc->numtriggers = numtrigs;
	for (i = 0; i < numtrigs; i++)
		SetTriggerFlags(trigdesc, &(triggers[i]));

	/* Copy completed trigdesc into cache storage */
	oldContext = MemoryContextSwitchTo(CacheMemoryContext);
	relation->trigdesc = CopyTriggerDesc(trigdesc);
	MemoryContextSwitchTo(oldContext);

	/* Release working memory */
	FreeTriggerDesc(trigdesc);
}

/*
 * Update the TriggerDesc's hint flags to include the specified trigger
 */
static void
SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger)
{
	int16		tgtype = trigger->tgtype;

	trigdesc->trig_insert_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_update_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_delete_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	/* there are no row-level truncate triggers */
	trigdesc->trig_truncate_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_TRUNCATE);
	trigdesc->trig_truncate_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_TRUNCATE);

	trigdesc->trig_insert_new_table |=
		(TRIGGER_FOR_INSERT(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgnewtable));
	trigdesc->trig_update_old_table |=
		(TRIGGER_FOR_UPDATE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgoldtable));
	trigdesc->trig_update_new_table |=
		(TRIGGER_FOR_UPDATE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgnewtable));
	trigdesc->trig_delete_old_table |=
		(TRIGGER_FOR_DELETE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgoldtable));
}

/*
 * Copy a TriggerDesc data structure.
 *
 * The copy is allocated in the current memory context.
 */
TriggerDesc *
CopyTriggerDesc(TriggerDesc *trigdesc)
{
	TriggerDesc *newdesc;
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL || trigdesc->numtriggers <= 0)
		return NULL;

	newdesc = (TriggerDesc *) palloc(sizeof(TriggerDesc));
	memcpy(newdesc, trigdesc, sizeof(TriggerDesc));

	trigger = (Trigger *) palloc(trigdesc->numtriggers * sizeof(Trigger));
	memcpy(trigger, trigdesc->triggers,
		   trigdesc->numtriggers * sizeof(Trigger));
	newdesc->triggers = trigger;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		trigger->tgname = pstrdup(trigger->tgname);
		if (trigger->tgnattr > 0)
		{
			int16	   *newattr;

			newattr = (int16 *) palloc(trigger->tgnattr * sizeof(int16));
			memcpy(newattr, trigger->tgattr,
				   trigger->tgnattr * sizeof(int16));
			trigger->tgattr = newattr;
		}
		if (trigger->tgnargs > 0)
		{
			char	  **newargs;
			int16		j;

			newargs = (char **) palloc(trigger->tgnargs * sizeof(char *));
			for (j = 0; j < trigger->tgnargs; j++)
				newargs[j] = pstrdup(trigger->tgargs[j]);
			trigger->tgargs = newargs;
		}
		if (trigger->tgqual)
			trigger->tgqual = pstrdup(trigger->tgqual);
		if (trigger->tgoldtable)
			trigger->tgoldtable = pstrdup(trigger->tgoldtable);
		if (trigger->tgnewtable)
			trigger->tgnewtable = pstrdup(trigger->tgnewtable);
		trigger++;
	}

	return newdesc;
}

/*
 * Free a TriggerDesc data structure.
 */
void
FreeTriggerDesc(TriggerDesc *trigdesc)
{
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL)
		return;

	trigger = trigdesc->triggers;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		pfree(trigger->tgname);
		if (trigger->tgnattr > 0)
			pfree(trigger->tgattr);
		if (trigger->tgnargs > 0)
		{
			while (--(trigger->tgnargs) >= 0)
				pfree(trigger->tgargs[trigger->tgnargs]);
			pfree(trigger->tgargs);
		}
		if (trigger->tgqual)
			pfree(trigger->tgqual);
		if (trigger->tgoldtable)
			pfree(trigger->tgoldtable);
		if (trigger->tgnewtable)
			pfree(trigger->tgnewtable);
		trigger++;
	}
	pfree(trigdesc->triggers);
	pfree(trigdesc);
}

/*
 * Compare two TriggerDesc structures for logical equality.
 */
#ifdef NOT_USED
bool
equalTriggerDescs(TriggerDesc *trigdesc1, TriggerDesc *trigdesc2)
{
	int			i,
				j;

	/*
	 * We need not examine the hint flags, just the trigger array itself; if
	 * we have the same triggers with the same types, the flags should match.
	 *
	 * As of 7.3 we assume trigger set ordering is significant in the
	 * comparison; so we just compare corresponding slots of the two sets.
	 *
	 * Note: comparing the stringToNode forms of the WHEN clauses means that
	 * parse column locations will affect the result.  This is okay as long as
	 * this function is only used for detecting exact equality, as for example
	 * in checking for staleness of a cache entry.
	 */
	if (trigdesc1 != NULL)
	{
		if (trigdesc2 == NULL)
			return false;
		if (trigdesc1->numtriggers != trigdesc2->numtriggers)
			return false;
		for (i = 0; i < trigdesc1->numtriggers; i++)
		{
			Trigger    *trig1 = trigdesc1->triggers + i;
			Trigger    *trig2 = trigdesc2->triggers + i;

			if (trig1->tgoid != trig2->tgoid)
				return false;
			if (strcmp(trig1->tgname, trig2->tgname) != 0)
				return false;
			if (trig1->tgfoid != trig2->tgfoid)
				return false;
			if (trig1->tgtype != trig2->tgtype)
				return false;
			if (trig1->tgenabled != trig2->tgenabled)
				return false;
			if (trig1->tgisinternal != trig2->tgisinternal)
				return false;
			if (trig1->tgisclone != trig2->tgisclone)
				return false;
			if (trig1->tgconstrrelid != trig2->tgconstrrelid)
				return false;
			if (trig1->tgconstrindid != trig2->tgconstrindid)
				return false;
			if (trig1->tgconstraint != trig2->tgconstraint)
				return false;
			if (trig1->tgdeferrable != trig2->tgdeferrable)
				return false;
			if (trig1->tginitdeferred != trig2->tginitdeferred)
				return false;
			if (trig1->tgnargs != trig2->tgnargs)
				return false;
			if (trig1->tgnattr != trig2->tgnattr)
				return false;
			if (trig1->tgnattr > 0 &&
				memcmp(trig1->tgattr, trig2->tgattr,
					   trig1->tgnattr * sizeof(int16)) != 0)
				return false;
			for (j = 0; j < trig1->tgnargs; j++)
				if (strcmp(trig1->tgargs[j], trig2->tgargs[j]) != 0)
					return false;
			if (trig1->tgqual == NULL && trig2->tgqual == NULL)
				 /* ok */ ;
			else if (trig1->tgqual == NULL || trig2->tgqual == NULL)
				return false;
			else if (strcmp(trig1->tgqual, trig2->tgqual) != 0)
				return false;
			if (trig1->tgoldtable == NULL && trig2->tgoldtable == NULL)
				 /* ok */ ;
			else if (trig1->tgoldtable == NULL || trig2->tgoldtable == NULL)
				return false;
			else if (strcmp(trig1->tgoldtable, trig2->tgoldtable) != 0)
				return false;
			if (trig1->tgnewtable == NULL && trig2->tgnewtable == NULL)
				 /* ok */ ;
			else if (trig1->tgnewtable == NULL || trig2->tgnewtable == NULL)
				return false;
			else if (strcmp(trig1->tgnewtable, trig2->tgnewtable) != 0)
				return false;
		}
	}
	else if (trigdesc2 != NULL)
		return false;
	return true;
}
#endif							/* NOT_USED */

/*
 * Check if there is a row-level trigger with transition tables that prevents
 * a table from becoming an inheritance child or partition.  Return the name
 * of the first such incompatible trigger, or NULL if there is none.
 */
const char *
FindTriggerIncompatibleWithInheritance(TriggerDesc *trigdesc)
{
	if (trigdesc != NULL)
	{
		int			i;

		for (i = 0; i < trigdesc->numtriggers; ++i)
		{
			Trigger    *trigger = &trigdesc->triggers[i];

			if (trigger->tgoldtable != NULL || trigger->tgnewtable != NULL)
				return trigger->tgname;
		}
	}

	return NULL;
}

/*
 * Call a trigger function.
 *
 *		trigdata: trigger descriptor.
 *		tgindx: trigger's index in finfo and instr arrays.
 *		finfo: array of cached trigger function call information.
 *		instr: optional array of EXPLAIN ANALYZE instrumentation state.
 *		per_tuple_context: memory context to execute the function in.
 *
 * Returns the tuple (or NULL) as returned by the function.
 */
static HeapTuple
ExecCallTriggerFunc(TriggerData *trigdata,
					int tgindx,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context)
{
	LOCAL_FCINFO(fcinfo, 0);
	PgStat_FunctionCallUsage fcusage;
	Datum		result;
	MemoryContext oldContext;

	/*
	 * Protect against code paths that may fail to initialize transition table
	 * info.
	 */
	Assert(((TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) ||
			 TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event) ||
			 TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) &&
			TRIGGER_FIRED_AFTER(trigdata->tg_event) &&
			!(trigdata->tg_event & AFTER_TRIGGER_DEFERRABLE) &&
			!(trigdata->tg_event & AFTER_TRIGGER_INITDEFERRED)) ||
		   (trigdata->tg_oldtable == NULL && trigdata->tg_newtable == NULL));

	finfo += tgindx;

	/*
	 * We cache fmgr lookup info, to avoid making the lookup again on each
	 * call.
	 */
	if (finfo->fn_oid == InvalidOid)
		fmgr_info(trigdata->tg_trigger->tgfoid, finfo);

	Assert(finfo->fn_oid == trigdata->tg_trigger->tgfoid);

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger.
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Do the function evaluation in the per-tuple memory context, so that
	 * leaked memory will be reclaimed once per tuple. Note in particular that
	 * any new tuple created by the trigger function will live till the end of
	 * the tuple cycle.
	 */
	oldContext = MemoryContextSwitchTo(per_tuple_context);

	/*
	 * Call the function, passing no arguments but setting a context.
	 */
	InitFunctionCallInfoData(*fcinfo, finfo, 0,
							 InvalidOid, (Node *) trigdata, NULL);

	pgstat_init_function_usage(fcinfo, &fcusage);

	MyTriggerDepth++;
	PG_TRY();
	{
		result = FunctionCallInvoke(fcinfo);
	}
	PG_FINALLY();
	{
		MyTriggerDepth--;
	}
	PG_END_TRY();

	pgstat_end_function_usage(&fcusage, true);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Trigger protocol allows function to return a null pointer, but NOT to
	 * set the isnull result flag.
	 */
	if (fcinfo->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("trigger function %u returned null value",
						fcinfo->flinfo->fn_oid)));

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);

	return (HeapTuple) DatumGetPointer(result);
}

void
ExecBSInsertTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_insert_before_statement)
		return;

	/* no-op if we already fired BS triggers in this context */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_INSERT))
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_insert_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_INSERT,
							  false, NULL, NULL, NIL, NULL, transition_capture,
							  false);
}

bool
ExecBRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, slot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			if (should_free)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			newtuple = check_modified_virtual_generated(RelationGetDescr(relinfo->ri_RelationDesc), newtuple);

			ExecForceStoreHeapTuple(newtuple, slot, false);

			/*
			 * After a tuple in a partition goes through a trigger, the user
			 * could have changed the partition key enough that the tuple no
			 * longer fits the partition.  Verify that.
			 */
			if (trigger->tgisclone &&
				!ExecPartitionCheck(relinfo, slot, estate, false))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("moving row to another partition during a BEFORE FOR EACH ROW trigger is not supported"),
						 errdetail("Before executing trigger \"%s\", the row was to be in partition \"%s.%s\".",
								   trigger->tgname,
								   get_namespace_name(RelationGetNamespace(relinfo->ri_RelationDesc)),
								   RelationGetRelationName(relinfo->ri_RelationDesc))));

			if (should_free)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			newtuple = NULL;
		}
	}

	return true;
}

void
ExecARInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot, List *recheckIndexes,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if ((trigdesc && trigdesc->trig_insert_after_row) ||
		(transition_capture && transition_capture->tcs_insert_new_table))
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_INSERT,
							  true, NULL, slot,
							  recheckIndexes, NULL,
							  transition_capture,
							  false);
}

bool
ExecIRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, slot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			if (should_free)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, slot, false);

			if (should_free)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			newtuple = NULL;
		}
	}

	return true;
}

void
ExecBSDeleteTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_delete_before_statement)
		return;

	/* no-op if we already fired BS triggers in this context */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_DELETE))
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_delete_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_DELETE,
							  false, NULL, NULL, NIL, NULL, transition_capture,
							  false);
}

/*
 * Execute BEFORE ROW DELETE triggers.
 *
 * True indicates caller can proceed with the delete.  False indicates caller
 * need to suppress the delete and additionally if requested, we need to pass
 * back the concurrently updated tuple if any.
 */
bool
ExecBRDeleteTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot **epqslot,
					 TM_Result *tmresult,
					 TM_FailureData *tmfd)
{
	TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	bool		result = true;
	TriggerData LocTriggerData = {0};
	HeapTuple	trigtuple;
	bool		should_free = false;
	int			i;

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		TupleTableSlot *epqslot_candidate = NULL;

		if (!GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
								LockTupleExclusive, slot, &epqslot_candidate,
								tmresult, tmfd))
			return false;

		/*
		 * If the tuple was concurrently updated and the caller of this
		 * function requested for the updated tuple, skip the trigger
		 * execution.
		 */
		if (epqslot_candidate != NULL && epqslot != NULL)
		{
			*epqslot = epqslot_candidate;
			return false;
		}

		trigtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);
	}
	else
	{
		trigtuple = fdw_trigtuple;
		ExecForceStoreHeapTuple(trigtuple, slot, false);
	}

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		HeapTuple	newtuple;
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, slot, NULL))
			continue;

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			result = false;		/* tell caller to suppress delete */
			break;
		}
		if (newtuple != trigtuple)
			heap_freetuple(newtuple);
	}
	if (should_free)
		heap_freetuple(trigtuple);

	return result;
}

/*
 * Note: is_crosspart_update must be true if the DELETE is being performed
 * as part of a cross-partition update.
 */
void
ExecARDeleteTriggers(EState *estate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TransitionCaptureState *transition_capture,
					 bool is_crosspart_update)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if ((trigdesc && trigdesc->trig_delete_after_row) ||
		(transition_capture && transition_capture->tcs_delete_old_table))
	{
		TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);

		Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
		if (fdw_trigtuple == NULL)
			GetTupleForTrigger(estate,
							   NULL,
							   relinfo,
							   tupleid,
							   LockTupleExclusive,
							   slot,
							   NULL,
							   NULL,
							   NULL);
		else
			ExecForceStoreHeapTuple(fdw_trigtuple, slot, false);

		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_DELETE,
							  true, slot, NULL, NIL, NULL,
							  transition_capture,
							  is_crosspart_update);
	}
}

bool
ExecIRDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	ExecForceStoreHeapTuple(trigtuple, slot, false);

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		HeapTuple	rettuple;
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, slot, NULL))
			continue;

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigger = trigger;
		rettuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (rettuple == NULL)
			return false;		/* Delete was suppressed */
		if (rettuple != trigtuple)
			heap_freetuple(rettuple);
	}
	return true;
}

void
ExecBSUpdateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};
	Bitmapset  *updatedCols;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_update_before_statement)
		return;

	/* no-op if we already fired BS triggers in this context */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_UPDATE))
		return;

	/* statement-level triggers operate on the parent table */
	Assert(relinfo->ri_RootResultRelInfo == NULL);

	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_updatedcols = updatedCols;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							updatedCols, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	/* statement-level triggers operate on the parent table */
	Assert(relinfo->ri_RootResultRelInfo == NULL);

	if (trigdesc && trigdesc->trig_update_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_UPDATE,
							  false, NULL, NULL, NIL,
							  ExecGetAllUpdatedCols(relinfo, estate),
							  transition_capture,
							  false);
}

bool
ExecBRUpdateTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot *newslot,
					 TM_Result *tmresult,
					 TM_FailureData *tmfd)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *oldslot = ExecGetTriggerOldSlot(estate, relinfo);
	HeapTuple	newtuple = NULL;
	HeapTuple	trigtuple;
	bool		should_free_trig = false;
	bool		should_free_new = false;
	TriggerData LocTriggerData = {0};
	int			i;
	Bitmapset  *updatedCols;
	LockTupleMode lockmode;

	/* Determine lock mode to use */
	lockmode = ExecUpdateLockMode(estate, relinfo);

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		TupleTableSlot *epqslot_candidate = NULL;

		/* get a copy of the on-disk tuple we are planning to update */
		if (!GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
								lockmode, oldslot, &epqslot_candidate,
								tmresult, tmfd))
			return false;		/* cancel the update action */

		/*
		 * In READ COMMITTED isolation level it's possible that target tuple
		 * was changed due to concurrent update.  In that case we have a raw
		 * subplan output tuple in epqslot_candidate, and need to form a new
		 * insertable tuple using ExecGetUpdateNewTuple to replace the one we
		 * received in newslot.  Neither we nor our callers have any further
		 * interest in the passed-in tuple, so it's okay to overwrite newslot
		 * with the newer data.
		 */
		if (epqslot_candidate != NULL)
		{
			TupleTableSlot *epqslot_clean;

			epqslot_clean = ExecGetUpdateNewTuple(relinfo, epqslot_candidate,
												  oldslot);

			/*
			 * Typically, the caller's newslot was also generated by
			 * ExecGetUpdateNewTuple, so that epqslot_clean will be the same
			 * slot and copying is not needed.  But do the right thing if it
			 * isn't.
			 */
			if (unlikely(newslot != epqslot_clean))
				ExecCopySlot(newslot, epqslot_clean);

			/*
			 * At this point newslot contains a virtual tuple that may
			 * reference some fields of oldslot's tuple in some disk buffer.
			 * If that tuple is in a different page than the original target
			 * tuple, then our only pin on that buffer is oldslot's, and we're
			 * about to release it.  Hence we'd better materialize newslot to
			 * ensure it doesn't contain references into an unpinned buffer.
			 * (We'd materialize it below anyway, but too late for safety.)
			 */
			ExecMaterializeSlot(newslot);
		}

		/*
		 * Here we convert oldslot to a materialized slot holding trigtuple.
		 * Neither slot passed to the triggers will hold any buffer pin.
		 */
		trigtuple = ExecFetchSlotHeapTuple(oldslot, true, &should_free_trig);
	}
	else
	{
		/* Put the FDW-supplied tuple into oldslot to unify the cases */
		ExecForceStoreHeapTuple(fdw_trigtuple, oldslot, false);
		trigtuple = fdw_trigtuple;
	}

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);
	LocTriggerData.tg_updatedcols = updatedCols;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							updatedCols, oldslot, newslot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(newslot, true, &should_free_new);

		LocTriggerData.tg_trigslot = oldslot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;
		LocTriggerData.tg_newslot = newslot;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple == NULL)
		{
			if (should_free_trig)
				heap_freetuple(trigtuple);
			if (should_free_new)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			newtuple = check_modified_virtual_generated(RelationGetDescr(relinfo->ri_RelationDesc), newtuple);

			ExecForceStoreHeapTuple(newtuple, newslot, false);

			/*
			 * If the tuple returned by the trigger / being stored, is the old
			 * row version, and the heap tuple passed to the trigger was
			 * allocated locally, materialize the slot. Otherwise we might
			 * free it while still referenced by the slot.
			 */
			if (should_free_trig && newtuple == trigtuple)
				ExecMaterializeSlot(newslot);

			if (should_free_new)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			newtuple = NULL;
		}
	}
	if (should_free_trig)
		heap_freetuple(trigtuple);

	return true;
}

/*
 * Note: 'src_partinfo' and 'dst_partinfo', when non-NULL, refer to the source
 * and destination partitions, respectively, of a cross-partition update of
 * the root partitioned table mentioned in the query, given by 'relinfo'.
 * 'tupleid' in that case refers to the ctid of the "old" tuple in the source
 * partition, and 'newslot' contains the "new" tuple in the destination
 * partition.  This interface allows to support the requirements of
 * ExecCrossPartitionUpdateForeignKey(); is_crosspart_update must be true in
 * that case.
 */
void
ExecARUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 ResultRelInfo *src_partinfo,
					 ResultRelInfo *dst_partinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot *newslot,
					 List *recheckIndexes,
					 TransitionCaptureState *transition_capture,
					 bool is_crosspart_update)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if ((trigdesc && trigdesc->trig_update_after_row) ||
		(transition_capture &&
		 (transition_capture->tcs_update_old_table ||
		  transition_capture->tcs_update_new_table)))
	{
		/*
		 * Note: if the UPDATE is converted into a DELETE+INSERT as part of
		 * update-partition-key operation, then this function is also called
		 * separately for DELETE and INSERT to capture transition table rows.
		 * In such case, either old tuple or new tuple can be NULL.
		 */
		TupleTableSlot *oldslot;
		ResultRelInfo *tupsrc;

		Assert((src_partinfo != NULL && dst_partinfo != NULL) ||
			   !is_crosspart_update);

		tupsrc = src_partinfo ? src_partinfo : relinfo;
		oldslot = ExecGetTriggerOldSlot(estate, tupsrc);

		if (fdw_trigtuple == NULL && ItemPointerIsValid(tupleid))
			GetTupleForTrigger(estate,
							   NULL,
							   tupsrc,
							   tupleid,
							   LockTupleExclusive,
							   oldslot,
							   NULL,
							   NULL,
							   NULL);
		else if (fdw_trigtuple != NULL)
			ExecForceStoreHeapTuple(fdw_trigtuple, oldslot, false);
		else
			ExecClearTuple(oldslot);

		AfterTriggerSaveEvent(estate, relinfo,
							  src_partinfo, dst_partinfo,
							  TRIGGER_EVENT_UPDATE,
							  true,
							  oldslot, newslot, recheckIndexes,
							  ExecGetAllUpdatedCols(relinfo, estate),
							  transition_capture,
							  is_crosspart_update);
	}
}

bool
ExecIRUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple, TupleTableSlot *newslot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *oldslot = ExecGetTriggerOldSlot(estate, relinfo);
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	ExecForceStoreHeapTuple(trigtuple, oldslot, false);

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, oldslot, newslot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(newslot, true, &should_free);

		LocTriggerData.tg_trigslot = oldslot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newslot = newslot;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, newslot, false);

			if (should_free)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			newtuple = NULL;
		}
	}

	return true;
}

void
ExecBSTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_truncate_before_statement)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_TRUNCATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_TRUNCATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_truncate_after_statement)
		AfterTriggerSaveEvent(estate, relinfo,
							  NULL, NULL,
							  TRIGGER_EVENT_TRUNCATE,
							  false, NULL, NULL, NIL, NULL, NULL,
							  false);
}


/*
 * Fetch tuple into "oldslot", dealing with locking and EPQ if necessary
 */
static bool
GetTupleForTrigger(EState *estate,
				   EPQState *epqstate,
				   ResultRelInfo *relinfo,
				   ItemPointer tid,
				   LockTupleMode lockmode,
				   TupleTableSlot *oldslot,
				   TupleTableSlot **epqslot,
				   TM_Result *tmresultp,
				   TM_FailureData *tmfdp)
{
	Relation	relation = relinfo->ri_RelationDesc;

	if (epqslot != NULL)
	{
		TM_Result	test;
		TM_FailureData tmfd;
		int			lockflags = 0;

		*epqslot = NULL;

		/* caller must pass an epqstate if EvalPlanQual is possible */
		Assert(epqstate != NULL);

		/*
		 * lock tuple for update
		 */
		if (!IsolationUsesXactSnapshot())
			lockflags |= TUPLE_LOCK_FLAG_FIND_LAST_VERSION;
		test = table_tuple_lock(relation, tid, estate->es_snapshot, oldslot,
								estate->es_output_cid,
								lockmode, LockWaitBlock,
								lockflags,
								&tmfd);

		/* Let the caller know about the status of this operation */
		if (tmresultp)
			*tmresultp = test;
		if (tmfdp)
			*tmfdp = tmfd;

		switch (test)
		{
			case TM_SelfModified:

				/*
				 * The target tuple was already updated or deleted by the
				 * current command, or by a later command in the current
				 * transaction.  We ignore the tuple in the former case, and
				 * throw error in the latter case, for the same reasons
				 * enumerated in ExecUpdate and ExecDelete in
				 * nodeModifyTable.c.
				 */
				if (tmfd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/* treat it as deleted; do not process */
				return false;

			case TM_Ok:
				if (tmfd.traversed)
				{
					/*
					 * Recheck the tuple using EPQ. For MERGE, we leave this
					 * to the caller (it must do additional rechecking, and
					 * might end up executing a different action entirely).
					 */
					if (estate->es_plannedstmt->commandType == CMD_MERGE)
					{
						if (tmresultp)
							*tmresultp = TM_Updated;
						return false;
					}

					*epqslot = EvalPlanQual(epqstate,
											relation,
											relinfo->ri_RangeTableIndex,
											oldslot);

					/*
					 * If PlanQual failed for updated tuple - we must not
					 * process this tuple!
					 */
					if (TupIsNull(*epqslot))
					{
						*epqslot = NULL;
						return false;
					}
				}
				break;

			case TM_Updated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				elog(ERROR, "unexpected table_tuple_lock status: %u", test);
				break;

			case TM_Deleted:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent delete")));
				/* tuple was deleted */
				return false;

			case TM_Invisible:
				elog(ERROR, "attempted to lock invisible tuple");
				break;

			default:
				elog(ERROR, "unrecognized table_tuple_lock status: %u", test);
				return false;	/* keep compiler quiet */
		}
	}
	else
	{
		/*
		 * We expect the tuple to be present, thus very simple error handling
		 * suffices.
		 */
		if (!table_tuple_fetch_row_version(relation, tid, SnapshotAny,
										   oldslot))
			elog(ERROR, "failed to fetch tuple for trigger");
	}

	return true;
}

/*
 * Is trigger enabled to fire?
 */
static bool
TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
			   Trigger *trigger, TriggerEvent event,
			   Bitmapset *modifiedCols,
			   TupleTableSlot *oldslot, TupleTableSlot *newslot)
{
	/* Check replication-role-dependent enable state */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_ORIGIN ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}
	else						/* ORIGIN or LOCAL role */
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_REPLICA ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}

	/*
	 * Check for column-specific trigger (only possible for UPDATE, and in
	 * fact we *must* ignore tgattr for other event types)
	 */
	if (trigger->tgnattr > 0 && TRIGGER_FIRED_BY_UPDATE(event))
	{
		int			i;
		bool		modified;

		modified = false;
		for (i = 0; i < trigger->tgnattr; i++)
		{
			if (bms_is_member(trigger->tgattr[i] - FirstLowInvalidHeapAttributeNumber,
							  modifiedCols))
			{
				modified = true;
				break;
			}
		}
		if (!modified)
			return false;
	}

	/* Check for WHEN clause */
	if (trigger->tgqual)
	{
		ExprState **predicate;
		ExprContext *econtext;
		MemoryContext oldContext;
		int			i;

		Assert(estate != NULL);

		/*
		 * trigger is an element of relinfo->ri_TrigDesc->triggers[]; find the
		 * matching element of relinfo->ri_TrigWhenExprs[]
		 */
		i = trigger - relinfo->ri_TrigDesc->triggers;
		predicate = &relinfo->ri_TrigWhenExprs[i];

		/*
		 * If first time through for this WHEN expression, build expression
		 * nodetrees for it.  Keep them in the per-query memory context so
		 * they'll survive throughout the query.
		 */
		if (*predicate == NULL)
		{
			Node	   *tgqual;

			oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
			tgqual = stringToNode(trigger->tgqual);
			tgqual = expand_generated_columns_in_expr(tgqual, relinfo->ri_RelationDesc, PRS2_OLD_VARNO);
			tgqual = expand_generated_columns_in_expr(tgqual, relinfo->ri_RelationDesc, PRS2_NEW_VARNO);
			/* Change references to OLD and NEW to INNER_VAR and OUTER_VAR */
			ChangeVarNodes(tgqual, PRS2_OLD_VARNO, INNER_VAR, 0);
			ChangeVarNodes(tgqual, PRS2_NEW_VARNO, OUTER_VAR, 0);
			/* ExecPrepareQual wants implicit-AND form */
			tgqual = (Node *) make_ands_implicit((Expr *) tgqual);
			*predicate = ExecPrepareQual((List *) tgqual, estate);
			MemoryContextSwitchTo(oldContext);
		}

		/*
		 * We will use the EState's per-tuple context for evaluating WHEN
		 * expressions (creating it if it's not already there).
		 */
		econtext = GetPerTupleExprContext(estate);

		/*
		 * Finally evaluate the expression, making the old and/or new tuples
		 * available as INNER_VAR/OUTER_VAR respectively.
		 */
		econtext->ecxt_innertuple = oldslot;
		econtext->ecxt_outertuple = newslot;
		if (!ExecQual(*predicate, econtext))
			return false;
	}

	return true;
}


/* ----------
 * After-trigger stuff
 *
 * The AfterTriggersData struct holds data about pending AFTER trigger events
 * during the current transaction tree.  (BEFORE triggers are fired
 * immediately so we don't need any persistent state about them.)  The struct
 * and most of its subsidiary data are kept in TopTransactionContext; however
 * some data that can be discarded sooner appears in the CurTransactionContext
 * of the relevant subtransaction.  Also, the individual event records are
 * kept in a separate sub-context of TopTransactionContext.  This is done
 * mainly so that it's easy to tell from a memory context dump how much space
 * is being eaten by trigger events.
 *
 * Because the list of pending events can grow large, we go to some
 * considerable effort to minimize per-event memory consumption.  The event
 * records are grouped into chunks and common data for similar events in the
 * same chunk is only stored once.
 *
 * XXX We need to be able to save the per-event data in a file if it grows too
 * large.
 * ----------
 */

/* Per-trigger SET CONSTRAINT status */
typedef struct SetConstraintTriggerData
{
	Oid			sct_tgoid;
	bool		sct_tgisdeferred;
} SetConstraintTriggerData;

typedef struct SetConstraintTriggerData *SetConstraintTrigger;

/*
 * SET CONSTRAINT intra-transaction status.
 *
 * We make this a single palloc'd object so it can be copied and freed easily.
 *
 * all_isset and all_isdeferred are used to keep track
 * of SET CONSTRAINTS ALL {DEFERRED, IMMEDIATE}.
 *
 * trigstates[] stores per-trigger tgisdeferred settings.
 */
typedef struct SetConstraintStateData
{
	bool		all_isset;
	bool		all_isdeferred;
	int			numstates;		/* number of trigstates[] entries in use */
	int			numalloc;		/* allocated size of trigstates[] */
	SetConstraintTriggerData trigstates[FLEXIBLE_ARRAY_MEMBER];
} SetConstraintStateData;

typedef SetConstraintStateData *SetConstraintState;


/*
 * Per-trigger-event data
 *
 * The actual per-event data, AfterTriggerEventData, includes DONE/IN_PROGRESS
 * status bits, up to two tuple CTIDs, and optionally two OIDs of partitions.
 * Each event record also has an associated AfterTriggerSharedData that is
 * shared across all instances of similar events within a "chunk".
 *
 * For row-level triggers, we arrange not to waste storage on unneeded ctid
 * fields.  Updates of regular tables use two; inserts and deletes of regular
 * tables use one; foreign tables always use zero and save the tuple(s) to a
 * tuplestore.  AFTER_TRIGGER_FDW_FETCH directs AfterTriggerExecute() to
 * retrieve a fresh tuple or pair of tuples from that tuplestore, while
 * AFTER_TRIGGER_FDW_REUSE directs it to use the most-recently-retrieved
 * tuple(s).  This permits storing tuples once regardless of the number of
 * row-level triggers on a foreign table.
 *
 * When updates on partitioned tables cause rows to move between partitions,
 * the OIDs of both partitions are stored too, so that the tuples can be
 * fetched; such entries are marked AFTER_TRIGGER_CP_UPDATE (for "cross-
 * partition update").
 *
 * Note that we need triggers on foreign tables to be fired in exactly the
 * order they were queued, so that the tuples come out of the tuplestore in
 * the right order.  To ensure that, we forbid deferrable (constraint)
 * triggers on foreign tables.  This also ensures that such triggers do not
 * get deferred into outer trigger query levels, meaning that it's okay to
 * destroy the tuplestore at the end of the query level.
 *
 * Statement-level triggers always bear AFTER_TRIGGER_1CTID, though they
 * require no ctid field.  We lack the flag bit space to neatly represent that
 * distinct case, and it seems unlikely to be worth much trouble.
 *
 * Note: ats_firing_id is initially zero and is set to something else when
 * AFTER_TRIGGER_IN_PROGRESS is set.  It indicates which trigger firing
 * cycle the trigger will be fired in (or was fired in, if DONE is set).
 * Although this is mutable state, we can keep it in AfterTriggerSharedData
 * because all instances of the same type of event in a given event list will
 * be fired at the same time, if they were queued between the same firing
 * cycles.  So we need only ensure that ats_firing_id is zero when attaching
 * a new event to an existing AfterTriggerSharedData record.
 */
typedef uint32 TriggerFlags;

#define AFTER_TRIGGER_OFFSET			0x07FFFFFF	/* must be low-order bits */
#define AFTER_TRIGGER_DONE				0x80000000
#define AFTER_TRIGGER_IN_PROGRESS		0x40000000
/* bits describing the size and tuple sources of this event */
#define AFTER_TRIGGER_FDW_REUSE			0x00000000
#define AFTER_TRIGGER_FDW_FETCH			0x20000000
#define AFTER_TRIGGER_1CTID				0x10000000
#define AFTER_TRIGGER_2CTID				0x30000000
#define AFTER_TRIGGER_CP_UPDATE			0x08000000
#define AFTER_TRIGGER_TUP_BITS			0x38000000
typedef struct AfterTriggerSharedData *AfterTriggerShared;

typedef struct AfterTriggerSharedData
{
	TriggerEvent ats_event;		/* event type indicator, see trigger.h */
	Oid			ats_tgoid;		/* the trigger's ID */
	Oid			ats_relid;		/* the relation it's on */
	Oid			ats_rolid;		/* role to execute the trigger */
	CommandId	ats_firing_id;	/* ID for firing cycle */
	struct AfterTriggersTableData *ats_table;	/* transition table access */
	Bitmapset  *ats_modifiedcols;	/* modified columns */
} AfterTriggerSharedData;

typedef struct AfterTriggerEventData *AfterTriggerEvent;

typedef struct AfterTriggerEventData
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
	ItemPointerData ate_ctid1;	/* inserted, deleted, or old updated tuple */
	ItemPointerData ate_ctid2;	/* new updated tuple */

	/*
	 * During a cross-partition update of a partitioned table, we also store
	 * the OIDs of source and destination partitions that are needed to fetch
	 * the old (ctid1) and the new tuple (ctid2) from, respectively.
	 */
	Oid			ate_src_part;
	Oid			ate_dst_part;
} AfterTriggerEventData;

/* AfterTriggerEventData, minus ate_src_part, ate_dst_part */
typedef struct AfterTriggerEventDataNoOids
{
	TriggerFlags ate_flags;
	ItemPointerData ate_ctid1;
	ItemPointerData ate_ctid2;
}			AfterTriggerEventDataNoOids;

/* AfterTriggerEventData, minus ate_*_part and ate_ctid2 */
typedef struct AfterTriggerEventDataOneCtid
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
	ItemPointerData ate_ctid1;	/* inserted, deleted, or old updated tuple */
}			AfterTriggerEventDataOneCtid;

/* AfterTriggerEventData, minus ate_*_part, ate_ctid1 and ate_ctid2 */
typedef struct AfterTriggerEventDataZeroCtids
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
}			AfterTriggerEventDataZeroCtids;

#define SizeofTriggerEvent(evt) \
	(((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_CP_UPDATE ? \
	 sizeof(AfterTriggerEventData) : \
	 (((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_2CTID ? \
	  sizeof(AfterTriggerEventDataNoOids) : \
	  (((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_1CTID ? \
	   sizeof(AfterTriggerEventDataOneCtid) : \
	   sizeof(AfterTriggerEventDataZeroCtids))))

#define GetTriggerSharedData(evt) \
	((AfterTriggerShared) ((char *) (evt) + ((evt)->ate_flags & AFTER_TRIGGER_OFFSET)))

/*
 * To avoid palloc overhead, we keep trigger events in arrays in successively-
 * larger chunks (a slightly more sophisticated version of an expansible
 * array).  The space between CHUNK_DATA_START and freeptr is occupied by
 * AfterTriggerEventData records; the space between endfree and endptr is
 * occupied by AfterTriggerSharedData records.
 */
typedef struct AfterTriggerEventChunk
{
	struct AfterTriggerEventChunk *next;	/* list link */
	char	   *freeptr;		/* start of free space in chunk */
	char	   *endfree;		/* end of free space in chunk */
	char	   *endptr;			/* end of chunk */
	/* event data follows here */
} AfterTriggerEventChunk;

#define CHUNK_DATA_START(cptr) ((char *) (cptr) + MAXALIGN(sizeof(AfterTriggerEventChunk)))

/* A list of events */
typedef struct AfterTriggerEventList
{
	AfterTriggerEventChunk *head;
	AfterTriggerEventChunk *tail;
	char	   *tailfree;		/* freeptr of tail chunk */
} AfterTriggerEventList;

/* Macros to help in iterating over a list of events */
#define for_each_chunk(cptr, evtlist) \
	for (cptr = (evtlist).head; cptr != NULL; cptr = cptr->next)
#define for_each_event(eptr, cptr) \
	for (eptr = (AfterTriggerEvent) CHUNK_DATA_START(cptr); \
		 (char *) eptr < (cptr)->freeptr; \
		 eptr = (AfterTriggerEvent) (((char *) eptr) + SizeofTriggerEvent(eptr)))
/* Use this if no special per-chunk processing is needed */
#define for_each_event_chunk(eptr, cptr, evtlist) \
	for_each_chunk(cptr, evtlist) for_each_event(eptr, cptr)

/* Macros for iterating from a start point that might not be list start */
#define for_each_chunk_from(cptr) \
	for (; cptr != NULL; cptr = cptr->next)
#define for_each_event_from(eptr, cptr) \
	for (; \
		 (char *) eptr < (cptr)->freeptr; \
		 eptr = (AfterTriggerEvent) (((char *) eptr) + SizeofTriggerEvent(eptr)))


/*
 * All per-transaction data for the AFTER TRIGGERS module.
 *
 * AfterTriggersData has the following fields:
 *
 * firing_counter is incremented for each call of afterTriggerInvokeEvents.
 * We mark firable events with the current firing cycle's ID so that we can
 * tell which ones to work on.  This ensures sane behavior if a trigger
 * function chooses to do SET CONSTRAINTS: the inner SET CONSTRAINTS will
 * only fire those events that weren't already scheduled for firing.
 *
 * state keeps track of the transaction-local effects of SET CONSTRAINTS.
 * This is saved and restored across failed subtransactions.
 *
 * events is the current list of deferred events.  This is global across
 * all subtransactions of the current transaction.  In a subtransaction
 * abort, we know that the events added by the subtransaction are at the
 * end of the list, so it is relatively easy to discard them.  The event
 * list chunks themselves are stored in event_cxt.
 *
 * query_depth is the current depth of nested AfterTriggerBeginQuery calls
 * (-1 when the stack is empty).
 *
 * query_stack[query_depth] is the per-query-level data, including these fields:
 *
 * events is a list of AFTER trigger events queued by the current query.
 * None of these are valid until the matching AfterTriggerEndQuery call
 * occurs.  At that point we fire immediate-mode triggers, and append any
 * deferred events to the main events list.
 *
 * fdw_tuplestore is a tuplestore containing the foreign-table tuples
 * needed by events queued by the current query.  (Note: we use just one
 * tuplestore even though more than one foreign table might be involved.
 * This is okay because tuplestores don't really care what's in the tuples
 * they store; but it's possible that someday it'd break.)
 *
 * tables is a List of AfterTriggersTableData structs for target tables
 * of the current query (see below).
 *
 * maxquerydepth is just the allocated length of query_stack.
 *
 * trans_stack holds per-subtransaction data, including these fields:
 *
 * state is NULL or a pointer to a saved copy of the SET CONSTRAINTS
 * state data.  Each subtransaction level that modifies that state first
 * saves a copy, which we use to restore the state if we abort.
 *
 * events is a copy of the events head/tail pointers,
 * which we use to restore those values during subtransaction abort.
 *
 * query_depth is the subtransaction-start-time value of query_depth,
 * which we similarly use to clean up at subtransaction abort.
 *
 * firing_counter is the subtransaction-start-time value of firing_counter.
 * We use this to recognize which deferred triggers were fired (or marked
 * for firing) within an aborted subtransaction.
 *
 * We use GetCurrentTransactionNestLevel() to determine the correct array
 * index in trans_stack.  maxtransdepth is the number of allocated entries in
 * trans_stack.  (By not keeping our own stack pointer, we can avoid trouble
 * in cases where errors during subxact abort cause multiple invocations
 * of AfterTriggerEndSubXact() at the same nesting depth.)
 *
 * We create an AfterTriggersTableData struct for each target table of the
 * current query, and each operation mode (INSERT/UPDATE/DELETE), that has
 * either transition tables or statement-level triggers.  This is used to
 * hold the relevant transition tables, as well as info tracking whether
 * we already queued the statement triggers.  (We use that info to prevent
 * firing the same statement triggers more than once per statement, or really
 * once per transition table set.)  These structs, along with the transition
 * table tuplestores, live in the (sub)transaction's CurTransactionContext.
 * That's sufficient lifespan because we don't allow transition tables to be
 * used by deferrable triggers, so they only need to survive until
 * AfterTriggerEndQuery.
 */
typedef struct AfterTriggersQueryData AfterTriggersQueryData;
typedef struct AfterTriggersTransData AfterTriggersTransData;
typedef struct AfterTriggersTableData AfterTriggersTableData;

typedef struct AfterTriggersData
{
	CommandId	firing_counter; /* next firing ID to assign */
	SetConstraintState state;	/* the active S C state */
	AfterTriggerEventList events;	/* deferred-event list */
	MemoryContext event_cxt;	/* memory context for events, if any */

	/* per-query-level data: */
	AfterTriggersQueryData *query_stack;	/* array of structs shown below */
	int			query_depth;	/* current index in above array */
	int			maxquerydepth;	/* allocated len of above array */

	/* per-subtransaction-level data: */
	AfterTriggersTransData *trans_stack;	/* array of structs shown below */
	int			maxtransdepth;	/* allocated len of above array */
} AfterTriggersData;

struct AfterTriggersQueryData
{
	AfterTriggerEventList events;	/* events pending from this query */
	Tuplestorestate *fdw_tuplestore;	/* foreign tuples for said events */
	List	   *tables;			/* list of AfterTriggersTableData, see below */
};

struct AfterTriggersTransData
{
	/* these fields are just for resetting at subtrans abort: */
	SetConstraintState state;	/* saved S C state, or NULL if not yet saved */
	AfterTriggerEventList events;	/* saved list pointer */
	int			query_depth;	/* saved query_depth */
	CommandId	firing_counter; /* saved firing_counter */
};

struct AfterTriggersTableData
{
	/* relid + cmdType form the lookup key for these structs: */
	Oid			relid;			/* target table's OID */
	CmdType		cmdType;		/* event type, CMD_INSERT/UPDATE/DELETE */
	bool		closed;			/* true when no longer OK to add tuples */
	bool		before_trig_done;	/* did we already queue BS triggers? */
	bool		after_trig_done;	/* did we already queue AS triggers? */
	AfterTriggerEventList after_trig_events;	/* if so, saved list pointer */

	/*
	 * We maintain separate transition tables for UPDATE/INSERT/DELETE since
	 * MERGE can run all three actions in a single statement. Note that UPDATE
	 * needs both old and new transition tables whereas INSERT needs only new,
	 * and DELETE needs only old.
	 */

	/* "old" transition table for UPDATE, if any */
	Tuplestorestate *old_upd_tuplestore;
	/* "new" transition table for UPDATE, if any */
	Tuplestorestate *new_upd_tuplestore;
	/* "old" transition table for DELETE, if any */
	Tuplestorestate *old_del_tuplestore;
	/* "new" transition table for INSERT, if any */
	Tuplestorestate *new_ins_tuplestore;

	TupleTableSlot *storeslot;	/* for converting to tuplestore's format */
};

static AfterTriggersData afterTriggers;

static void AfterTriggerExecute(EState *estate,
								AfterTriggerEvent event,
								ResultRelInfo *relInfo,
								ResultRelInfo *src_relInfo,
								ResultRelInfo *dst_relInfo,
								TriggerDesc *trigdesc,
								FmgrInfo *finfo,
								Instrumentation *instr,
								MemoryContext per_tuple_context,
								TupleTableSlot *trig_tuple_slot1,
								TupleTableSlot *trig_tuple_slot2);
static AfterTriggersTableData *GetAfterTriggersTableData(Oid relid,
														 CmdType cmdType);
static TupleTableSlot *GetAfterTriggersStoreSlot(AfterTriggersTableData *table,
												 TupleDesc tupdesc);
static Tuplestorestate *GetAfterTriggersTransitionTable(int event,
														TupleTableSlot *oldslot,
														TupleTableSlot *newslot,
														TransitionCaptureState *transition_capture);
static void TransitionTableAddTuple(EState *estate,
									TransitionCaptureState *transition_capture,
									ResultRelInfo *relinfo,
									TupleTableSlot *slot,
									TupleTableSlot *original_insert_tuple,
									Tuplestorestate *tuplestore);
static void AfterTriggerFreeQuery(AfterTriggersQueryData *qs);
static SetConstraintState SetConstraintStateCreate(int numalloc);
static SetConstraintState SetConstraintStateCopy(SetConstraintState origstate);
static SetConstraintState SetConstraintStateAddItem(SetConstraintState state,
													Oid tgoid, bool tgisdeferred);
static void cancel_prior_stmt_triggers(Oid relid, CmdType cmdType, int tgevent);


/*
 * Get the FDW tuplestore for the current trigger query level, creating it
 * if necessary.
 */
static Tuplestorestate *
GetCurrentFDWTuplestore(void)
{
	Tuplestorestate *ret;

	ret = afterTriggers.query_stack[afterTriggers.query_depth].fdw_tuplestore;
	if (ret == NULL)
	{
		MemoryContext oldcxt;
		ResourceOwner saveResourceOwner;

		/*
		 * Make the tuplestore valid until end of subtransaction.  We really
		 * only need it until AfterTriggerEndQuery().
		 */
		oldcxt = MemoryContextSwitchTo(CurTransactionContext);
		saveResourceOwner = CurrentResourceOwner;
		CurrentResourceOwner = CurTransactionResourceOwner;

		ret = tuplestore_begin_heap(false, false, work_mem);

		CurrentResourceOwner = saveResourceOwner;
		MemoryContextSwitchTo(oldcxt);

		afterTriggers.query_stack[afterTriggers.query_depth].fdw_tuplestore = ret;
	}

	return ret;
}

/* ----------
 * afterTriggerCheckState()
 *
 *	Returns true if the trigger event is actually in state DEFERRED.
 * ----------
 */
static bool
afterTriggerCheckState(AfterTriggerShared evtshared)
{
	Oid			tgoid = evtshared->ats_tgoid;
	SetConstraintState state = afterTriggers.state;
	int			i;

	/*
	 * For not-deferrable triggers (i.e. normal AFTER ROW triggers and
	 * constraints declared NOT DEFERRABLE), the state is always false.
	 */
	if ((evtshared->ats_event & AFTER_TRIGGER_DEFERRABLE) == 0)
		return false;

	/*
	 * If constraint state exists, SET CONSTRAINTS might have been executed
	 * either for this trigger or for all triggers.
	 */
	if (state != NULL)
	{
		/* Check for SET CONSTRAINTS for this specific trigger. */
		for (i = 0; i < state->numstates; i++)
		{
			if (state->trigstates[i].sct_tgoid == tgoid)
				return state->trigstates[i].sct_tgisdeferred;
		}

		/* Check for SET CONSTRAINTS ALL. */
		if (state->all_isset)
			return state->all_isdeferred;
	}

	/*
	 * Otherwise return the default state for the trigger.
	 */
	return ((evtshared->ats_event & AFTER_TRIGGER_INITDEFERRED) != 0);
}

/* ----------
 * afterTriggerCopyBitmap()
 *
 * Copy bitmap into AfterTriggerEvents memory context, which is where the after
 * trigger events are kept.
 * ----------
 */
static Bitmapset *
afterTriggerCopyBitmap(Bitmapset *src)
{
	Bitmapset  *dst;
	MemoryContext oldcxt;

	if (src == NULL)
		return NULL;

	oldcxt = MemoryContextSwitchTo(afterTriggers.event_cxt);

	dst = bms_copy(src);

	MemoryContextSwitchTo(oldcxt);

	return dst;
}

/* ----------
 * afterTriggerAddEvent()
 *
 *	Add a new trigger event to the specified queue.
 *	The passed-in event data is copied.
 * ----------
 */
static void
afterTriggerAddEvent(AfterTriggerEventList *events,
					 AfterTriggerEvent event, AfterTriggerShared evtshared)
{
	Size		eventsize = SizeofTriggerEvent(event);
	Size		needed = eventsize + sizeof(AfterTriggerSharedData);
	AfterTriggerEventChunk *chunk;
	AfterTriggerShared newshared;
	AfterTriggerEvent newevent;

	/*
	 * If empty list or not enough room in the tail chunk, make a new chunk.
	 * We assume here that a new shared record will always be needed.
	 */
	chunk = events->tail;
	if (chunk == NULL ||
		chunk->endfree - chunk->freeptr < needed)
	{
		Size		chunksize;

		/* Create event context if we didn't already */
		if (afterTriggers.event_cxt == NULL)
			afterTriggers.event_cxt =
				AllocSetContextCreate(TopTransactionContext,
									  "AfterTriggerEvents",
									  ALLOCSET_DEFAULT_SIZES);

		/*
		 * Chunk size starts at 1KB and is allowed to increase up to 1MB.
		 * These numbers are fairly arbitrary, though there is a hard limit at
		 * AFTER_TRIGGER_OFFSET; else we couldn't link event records to their
		 * shared records using the available space in ate_flags.  Another
		 * constraint is that if the chunk size gets too huge, the search loop
		 * below would get slow given a (not too common) usage pattern with
		 * many distinct event types in a chunk.  Therefore, we double the
		 * preceding chunk size only if there weren't too many shared records
		 * in the preceding chunk; otherwise we halve it.  This gives us some
		 * ability to adapt to the actual usage pattern of the current query
		 * while still having large chunk sizes in typical usage.  All chunk
		 * sizes used should be MAXALIGN multiples, to ensure that the shared
		 * records will be aligned safely.
		 */
#define MIN_CHUNK_SIZE 1024
#define MAX_CHUNK_SIZE (1024*1024)

#if MAX_CHUNK_SIZE > (AFTER_TRIGGER_OFFSET+1)
#error MAX_CHUNK_SIZE must not exceed AFTER_TRIGGER_OFFSET
#endif

		if (chunk == NULL)
			chunksize = MIN_CHUNK_SIZE;
		else
		{
			/* preceding chunk size... */
			chunksize = chunk->endptr - (char *) chunk;
			/* check number of shared records in preceding chunk */
			if ((chunk->endptr - chunk->endfree) <=
				(100 * sizeof(AfterTriggerSharedData)))
				chunksize *= 2; /* okay, double it */
			else
				chunksize /= 2; /* too many shared records */
			chunksize = Min(chunksize, MAX_CHUNK_SIZE);
		}
		chunk = MemoryContextAlloc(afterTriggers.event_cxt, chunksize);
		chunk->next = NULL;
		chunk->freeptr = CHUNK_DATA_START(chunk);
		chunk->endptr = chunk->endfree = (char *) chunk + chunksize;
		Assert(chunk->endfree - chunk->freeptr >= needed);

		if (events->tail == NULL)
		{
			Assert(events->head == NULL);
			events->head = chunk;
		}
		else
			events->tail->next = chunk;
		events->tail = chunk;
		/* events->tailfree is now out of sync, but we'll fix it below */
	}

	/*
	 * Try to locate a matching shared-data record already in the chunk. If
	 * none, make a new one. The search begins with the most recently added
	 * record, since newer ones are most likely to match.
	 */
	for (newshared = (AfterTriggerShared) chunk->endfree;
		 (char *) newshared < chunk->endptr;
		 newshared++)
	{
		/* compare fields roughly by probability of them being different */
		if (newshared->ats_tgoid == evtshared->ats_tgoid &&
			newshared->ats_event == evtshared->ats_event &&
			newshared->ats_firing_id == 0 &&
			newshared->ats_table == evtshared->ats_table &&
			newshared->ats_relid == evtshared->ats_relid &&
			newshared->ats_rolid == evtshared->ats_rolid &&
			bms_equal(newshared->ats_modifiedcols,
					  evtshared->ats_modifiedcols))
			break;
	}
	if ((char *) newshared >= chunk->endptr)
	{
		newshared = ((AfterTriggerShared) chunk->endfree) - 1;
		*newshared = *evtshared;
		/* now we must make a suitably-long-lived copy of the bitmap */
		newshared->ats_modifiedcols = afterTriggerCopyBitmap(evtshared->ats_modifiedcols);
		newshared->ats_firing_id = 0;	/* just to be sure */
		chunk->endfree = (char *) newshared;
	}

	/* Insert the data */
	newevent = (AfterTriggerEvent) chunk->freeptr;
	memcpy(newevent, event, eventsize);
	/* ... and link the new event to its shared record */
	newevent->ate_flags &= ~AFTER_TRIGGER_OFFSET;
	newevent->ate_flags |= (char *) newshared - (char *) newevent;

	chunk->freeptr += eventsize;
	events->tailfree = chunk->freeptr;
}

/* ----------
 * afterTriggerFreeEventList()
 *
 *	Free all the event storage in the given list.
 * ----------
 */
static void
afterTriggerFreeEventList(AfterTriggerEventList *events)
{
	AfterTriggerEventChunk *chunk;

	while ((chunk = events->head) != NULL)
	{
		events->head = chunk->next;
		pfree(chunk);
	}
	events->tail = NULL;
	events->tailfree = NULL;
}

/* ----------
 * afterTriggerRestoreEventList()
 *
 *	Restore an event list to its prior length, removing all the events
 *	added since it had the value old_events.
 * ----------
 */
static void
afterTriggerRestoreEventList(AfterTriggerEventList *events,
							 const AfterTriggerEventList *old_events)
{
	AfterTriggerEventChunk *chunk;
	AfterTriggerEventChunk *next_chunk;

	if (old_events->tail == NULL)
	{
		/* restoring to a completely empty state, so free everything */
		afterTriggerFreeEventList(events);
	}
	else
	{
		*events = *old_events;
		/* free any chunks after the last one we want to keep */
		for (chunk = events->tail->next; chunk != NULL; chunk = next_chunk)
		{
			next_chunk = chunk->next;
			pfree(chunk);
		}
		/* and clean up the tail chunk to be the right length */
		events->tail->next = NULL;
		events->tail->freeptr = events->tailfree;

		/*
		 * We don't make any effort to remove now-unused shared data records.
		 * They might still be useful, anyway.
		 */
	}
}

/* ----------
 * afterTriggerDeleteHeadEventChunk()
 *
 *	Remove the first chunk of events from the query level's event list.
 *	Keep any event list pointers elsewhere in the query level's data
 *	structures in sync.
 * ----------
 */
static void
afterTriggerDeleteHeadEventChunk(AfterTriggersQueryData *qs)
{
	AfterTriggerEventChunk *target = qs->events.head;
	ListCell   *lc;

	Assert(target && target->next);

	/*
	 * First, update any pointers in the per-table data, so that they won't be
	 * dangling.  Resetting obsoleted pointers to NULL will make
	 * cancel_prior_stmt_triggers start from the list head, which is fine.
	 */
	foreach(lc, qs->tables)
	{
		AfterTriggersTableData *table = (AfterTriggersTableData *) lfirst(lc);

		if (table->after_trig_done &&
			table->after_trig_events.tail == target)
		{
			table->after_trig_events.head = NULL;
			table->after_trig_events.tail = NULL;
			table->after_trig_events.tailfree = NULL;
		}
	}

	/* Now we can flush the head chunk */
	qs->events.head = target->next;
	pfree(target);
}


/* ----------
 * AfterTriggerExecute()
 *
 *	Fetch the required tuples back from the heap and fire one
 *	single trigger function.
 *
 *	Frequently, this will be fired many times in a row for triggers of
 *	a single relation.  Therefore, we cache the open relation and provide
 *	fmgr lookup cache space at the caller level.  (For triggers fired at
 *	the end of a query, we can even piggyback on the executor's state.)
 *
 *	When fired for a cross-partition update of a partitioned table, the old
 *	tuple is fetched using 'src_relInfo' (the source leaf partition) and
 *	the new tuple using 'dst_relInfo' (the destination leaf partition), though
 *	both are converted into the root partitioned table's format before passing
 *	to the trigger function.
 *
 *	event: event currently being fired.
 *	relInfo: result relation for event.
 *	src_relInfo: source partition of a cross-partition update
 *	dst_relInfo: its destination partition
 *	trigdesc: working copy of rel's trigger info.
 *	finfo: array of fmgr lookup cache entries (one per trigger in trigdesc).
 *	instr: array of EXPLAIN ANALYZE instrumentation nodes (one per trigger),
 *		or NULL if no instrumentation is wanted.
 *	per_tuple_context: memory context to call trigger function in.
 *	trig_tuple_slot1: scratch slot for tg_trigtuple (foreign tables only)
 *	trig_tuple_slot2: scratch slot for tg_newtuple (foreign tables only)
 * ----------
 */
static void
AfterTriggerExecute(EState *estate,
					AfterTriggerEvent event,
					ResultRelInfo *relInfo,
					ResultRelInfo *src_relInfo,
					ResultRelInfo *dst_relInfo,
					TriggerDesc *trigdesc,
					FmgrInfo *finfo, Instrumentation *instr,
					MemoryContext per_tuple_context,
					TupleTableSlot *trig_tuple_slot1,
					TupleTableSlot *trig_tuple_slot2)
{
	Relation	rel = relInfo->ri_RelationDesc;
	Relation	src_rel = src_relInfo->ri_RelationDesc;
	Relation	dst_rel = dst_relInfo->ri_RelationDesc;
	AfterTriggerShared evtshared = GetTriggerSharedData(event);
	Oid			tgoid = evtshared->ats_tgoid;
	TriggerData LocTriggerData = {0};
	Oid			save_rolid;
	int			save_sec_context;
	HeapTuple	rettuple;
	int			tgindx;
	bool		should_free_trig = false;
	bool		should_free_new = false;

	/*
	 * Locate trigger in trigdesc.  It might not be present, and in fact the
	 * trigdesc could be NULL, if the trigger was dropped since the event was
	 * queued.  In that case, silently do nothing.
	 */
	if (trigdesc == NULL)
		return;
	for (tgindx = 0; tgindx < trigdesc->numtriggers; tgindx++)
	{
		if (trigdesc->triggers[tgindx].tgoid == tgoid)
		{
			LocTriggerData.tg_trigger = &(trigdesc->triggers[tgindx]);
			break;
		}
	}
	if (LocTriggerData.tg_trigger == NULL)
		return;

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger. We want
	 * to include time spent re-fetching tuples in the trigger cost.
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Fetch the required tuple(s).
	 */
	switch (event->ate_flags & AFTER_TRIGGER_TUP_BITS)
	{
		case AFTER_TRIGGER_FDW_FETCH:
			{
				Tuplestorestate *fdw_tuplestore = GetCurrentFDWTuplestore();

				if (!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot1))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");

				if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
					TRIGGER_EVENT_UPDATE &&
					!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot2))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");
			}
			/* fall through */
		case AFTER_TRIGGER_FDW_REUSE:

			/*
			 * Store tuple in the slot so that tg_trigtuple does not reference
			 * tuplestore memory.  (It is formally possible for the trigger
			 * function to queue trigger events that add to the same
			 * tuplestore, which can push other tuples out of memory.)  The
			 * distinction is academic, because we start with a minimal tuple
			 * that is stored as a heap tuple, constructed in different memory
			 * context, in the slot anyway.
			 */
			LocTriggerData.tg_trigslot = trig_tuple_slot1;
			LocTriggerData.tg_trigtuple =
				ExecFetchSlotHeapTuple(trig_tuple_slot1, true, &should_free_trig);

			if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
				TRIGGER_EVENT_UPDATE)
			{
				LocTriggerData.tg_newslot = trig_tuple_slot2;
				LocTriggerData.tg_newtuple =
					ExecFetchSlotHeapTuple(trig_tuple_slot2, true, &should_free_new);
			}
			else
			{
				LocTriggerData.tg_newtuple = NULL;
			}
			break;

		default:
			if (ItemPointerIsValid(&(event->ate_ctid1)))
			{
				TupleTableSlot *src_slot = ExecGetTriggerOldSlot(estate,
																 src_relInfo);

				if (!table_tuple_fetch_row_version(src_rel,
												   &(event->ate_ctid1),
												   SnapshotAny,
												   src_slot))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");

				/*
				 * Store the tuple fetched from the source partition into the
				 * target (root partitioned) table slot, converting if needed.
				 */
				if (src_relInfo != relInfo)
				{
					TupleConversionMap *map = ExecGetChildToRootMap(src_relInfo);

					LocTriggerData.tg_trigslot = ExecGetTriggerOldSlot(estate, relInfo);
					if (map)
					{
						execute_attr_map_slot(map->attrMap,
											  src_slot,
											  LocTriggerData.tg_trigslot);
					}
					else
						ExecCopySlot(LocTriggerData.tg_trigslot, src_slot);
				}
				else
					LocTriggerData.tg_trigslot = src_slot;
				LocTriggerData.tg_trigtuple =
					ExecFetchSlotHeapTuple(LocTriggerData.tg_trigslot, false, &should_free_trig);
			}
			else
			{
				LocTriggerData.tg_trigtuple = NULL;
			}

			/* don't touch ctid2 if not there */
			if (((event->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_2CTID ||
				 (event->ate_flags & AFTER_TRIGGER_CP_UPDATE)) &&
				ItemPointerIsValid(&(event->ate_ctid2)))
			{
				TupleTableSlot *dst_slot = ExecGetTriggerNewSlot(estate,
																 dst_relInfo);

				if (!table_tuple_fetch_row_version(dst_rel,
												   &(event->ate_ctid2),
												   SnapshotAny,
												   dst_slot))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");

				/*
				 * Store the tuple fetched from the destination partition into
				 * the target (root partitioned) table slot, converting if
				 * needed.
				 */
				if (dst_relInfo != relInfo)
				{
					TupleConversionMap *map = ExecGetChildToRootMap(dst_relInfo);

					LocTriggerData.tg_newslot = ExecGetTriggerNewSlot(estate, relInfo);
					if (map)
					{
						execute_attr_map_slot(map->attrMap,
											  dst_slot,
											  LocTriggerData.tg_newslot);
					}
					else
						ExecCopySlot(LocTriggerData.tg_newslot, dst_slot);
				}
				else
					LocTriggerData.tg_newslot = dst_slot;
				LocTriggerData.tg_newtuple =
					ExecFetchSlotHeapTuple(LocTriggerData.tg_newslot, false, &should_free_new);
			}
			else
			{
				LocTriggerData.tg_newtuple = NULL;
			}
	}

	/*
	 * Set up the tuplestore information to let the trigger have access to
	 * transition tables.  When we first make a transition table available to
	 * a trigger, mark it "closed" so that it cannot change anymore.  If any
	 * additional events of the same type get queued in the current trigger
	 * query level, they'll go into new transition tables.
	 */
	LocTriggerData.tg_oldtable = LocTriggerData.tg_newtable = NULL;
	if (evtshared->ats_table)
	{
		if (LocTriggerData.tg_trigger->tgoldtable)
		{
			if (TRIGGER_FIRED_BY_UPDATE(evtshared->ats_event))
				LocTriggerData.tg_oldtable = evtshared->ats_table->old_upd_tuplestore;
			else
				LocTriggerData.tg_oldtable = evtshared->ats_table->old_del_tuplestore;
			evtshared->ats_table->closed = true;
		}

		if (LocTriggerData.tg_trigger->tgnewtable)
		{
			if (TRIGGER_FIRED_BY_INSERT(evtshared->ats_event))
				LocTriggerData.tg_newtable = evtshared->ats_table->new_ins_tuplestore;
			else
				LocTriggerData.tg_newtable = evtshared->ats_table->new_upd_tuplestore;
			evtshared->ats_table->closed = true;
		}
	}

	/*
	 * Setup the remaining trigger information
	 */
	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event =
		evtshared->ats_event & (TRIGGER_EVENT_OPMASK | TRIGGER_EVENT_ROW);
	LocTriggerData.tg_relation = rel;
	if (TRIGGER_FOR_UPDATE(LocTriggerData.tg_trigger->tgtype))
		LocTriggerData.tg_updatedcols = evtshared->ats_modifiedcols;

	MemoryContextReset(per_tuple_context);

	/*
	 * If necessary, become the role that was active when the trigger got
	 * queued.  Note that the role might have been dropped since the trigger
	 * was queued, but if that is a problem, we will get an error later.
	 * Checking here would still leave a race condition.
	 */
	GetUserIdAndSecContext(&save_rolid, &save_sec_context);
	if (save_rolid != evtshared->ats_rolid)
		SetUserIdAndSecContext(evtshared->ats_rolid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	/*
	 * Call the trigger and throw away any possibly returned updated tuple.
	 * (Don't let ExecCallTriggerFunc measure EXPLAIN time.)
	 */
	rettuple = ExecCallTriggerFunc(&LocTriggerData,
								   tgindx,
								   finfo,
								   NULL,
								   per_tuple_context);
	if (rettuple != NULL &&
		rettuple != LocTriggerData.tg_trigtuple &&
		rettuple != LocTriggerData.tg_newtuple)
		heap_freetuple(rettuple);

	/* Restore the current role if necessary */
	if (save_rolid != evtshared->ats_rolid)
		SetUserIdAndSecContext(save_rolid, save_sec_context);

	/*
	 * Release resources
	 */
	if (should_free_trig)
		heap_freetuple(LocTriggerData.tg_trigtuple);
	if (should_free_new)
		heap_freetuple(LocTriggerData.tg_newtuple);

	/* don't clear slots' contents if foreign table */
	if (trig_tuple_slot1 == NULL)
	{
		if (LocTriggerData.tg_trigslot)
			ExecClearTuple(LocTriggerData.tg_trigslot);
		if (LocTriggerData.tg_newslot)
			ExecClearTuple(LocTriggerData.tg_newslot);
	}

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);
}


/*
 * afterTriggerMarkEvents()
 *
 *	Scan the given event list for not yet invoked events.  Mark the ones
 *	that can be invoked now with the current firing ID.
 *
 *	If move_list isn't NULL, events that are not to be invoked now are
 *	transferred to move_list.
 *
 *	When immediate_only is true, do not invoke currently-deferred triggers.
 *	(This will be false only at main transaction exit.)
 *
 *	Returns true if any invokable events were found.
 */
static bool
afterTriggerMarkEvents(AfterTriggerEventList *events,
					   AfterTriggerEventList *move_list,
					   bool immediate_only)
{
	bool		found = false;
	bool		deferred_found = false;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;

	for_each_event_chunk(event, chunk, *events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);
		bool		defer_it = false;

		if (!(event->ate_flags &
			  (AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS)))
		{
			/*
			 * This trigger hasn't been called or scheduled yet. Check if we
			 * should call it now.
			 */
			if (immediate_only && afterTriggerCheckState(evtshared))
			{
				defer_it = true;
			}
			else
			{
				/*
				 * Mark it as to be fired in this firing cycle.
				 */
				evtshared->ats_firing_id = afterTriggers.firing_counter;
				event->ate_flags |= AFTER_TRIGGER_IN_PROGRESS;
				found = true;
			}
		}

		/*
		 * If it's deferred, move it to move_list, if requested.
		 */
		if (defer_it && move_list != NULL)
		{
			deferred_found = true;
			/* add it to move_list */
			afterTriggerAddEvent(move_list, event, evtshared);
			/* mark original copy "done" so we don't do it again */
			event->ate_flags |= AFTER_TRIGGER_DONE;
		}
	}

	/*
	 * We could allow deferred triggers if, before the end of the
	 * security-restricted operation, we were to verify that a SET CONSTRAINTS
	 * ... IMMEDIATE has fired all such triggers.  For now, don't bother.
	 */
	if (deferred_found && InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot fire deferred trigger within security-restricted operation")));

	return found;
}

/*
 * afterTriggerInvokeEvents()
 *
 *	Scan the given event list for events that are marked as to be fired
 *	in the current firing cycle, and fire them.
 *
 *	If estate isn't NULL, we use its result relation info to avoid repeated
 *	openings and closing of trigger target relations.  If it is NULL, we
 *	make one locally to cache the info in case there are multiple trigger
 *	events per rel.
 *
 *	When delete_ok is true, it's safe to delete fully-processed events.
 *	(We are not very tense about that: we simply reset a chunk to be empty
 *	if all its events got fired.  The objective here is just to avoid useless
 *	rescanning of events when a trigger queues new events during transaction
 *	end, so it's not necessary to worry much about the case where only
 *	some events are fired.)
 *
 *	Returns true if no unfired events remain in the list (this allows us
 *	to avoid repeating afterTriggerMarkEvents).
 */
static bool
afterTriggerInvokeEvents(AfterTriggerEventList *events,
						 CommandId firing_id,
						 EState *estate,
						 bool delete_ok)
{
	bool		all_fired = true;
	AfterTriggerEventChunk *chunk;
	MemoryContext per_tuple_context;
	bool		local_estate = false;
	ResultRelInfo *rInfo = NULL;
	Relation	rel = NULL;
	TriggerDesc *trigdesc = NULL;
	FmgrInfo   *finfo = NULL;
	Instrumentation *instr = NULL;
	TupleTableSlot *slot1 = NULL,
			   *slot2 = NULL;

	/* Make a local EState if need be */
	if (estate == NULL)
	{
		estate = CreateExecutorState();
		local_estate = true;
	}

	/* Make a per-tuple memory context for trigger function calls */
	per_tuple_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AfterTriggerTupleContext",
							  ALLOCSET_DEFAULT_SIZES);

	for_each_chunk(chunk, *events)
	{
		AfterTriggerEvent event;
		bool		all_fired_in_chunk = true;

		for_each_event(event, chunk)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			/*
			 * Is it one for me to fire?
			 */
			if ((event->ate_flags & AFTER_TRIGGER_IN_PROGRESS) &&
				evtshared->ats_firing_id == firing_id)
			{
				ResultRelInfo *src_rInfo,
						   *dst_rInfo;

				/*
				 * So let's fire it... but first, find the correct relation if
				 * this is not the same relation as before.
				 */
				if (rel == NULL || RelationGetRelid(rel) != evtshared->ats_relid)
				{
					rInfo = ExecGetTriggerResultRel(estate, evtshared->ats_relid,
													NULL);
					rel = rInfo->ri_RelationDesc;
					/* Catch calls with insufficient relcache refcounting */
					Assert(!RelationHasReferenceCountZero(rel));
					trigdesc = rInfo->ri_TrigDesc;
					/* caution: trigdesc could be NULL here */
					finfo = rInfo->ri_TrigFunctions;
					instr = rInfo->ri_TrigInstrument;
					if (slot1 != NULL)
					{
						ExecDropSingleTupleTableSlot(slot1);
						ExecDropSingleTupleTableSlot(slot2);
						slot1 = slot2 = NULL;
					}
					if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
					{
						slot1 = MakeSingleTupleTableSlot(rel->rd_att,
														 &TTSOpsMinimalTuple);
						slot2 = MakeSingleTupleTableSlot(rel->rd_att,
														 &TTSOpsMinimalTuple);
					}
				}

				/*
				 * Look up source and destination partition result rels of a
				 * cross-partition update event.
				 */
				if ((event->ate_flags & AFTER_TRIGGER_TUP_BITS) ==
					AFTER_TRIGGER_CP_UPDATE)
				{
					Assert(OidIsValid(event->ate_src_part) &&
						   OidIsValid(event->ate_dst_part));
					src_rInfo = ExecGetTriggerResultRel(estate,
														event->ate_src_part,
														rInfo);
					dst_rInfo = ExecGetTriggerResultRel(estate,
														event->ate_dst_part,
														rInfo);
				}
				else
					src_rInfo = dst_rInfo = rInfo;

				/*
				 * Fire it.  Note that the AFTER_TRIGGER_IN_PROGRESS flag is
				 * still set, so recursive examinations of the event list
				 * won't try to re-fire it.
				 */
				AfterTriggerExecute(estate, event, rInfo,
									src_rInfo, dst_rInfo,
									trigdesc, finfo, instr,
									per_tuple_context, slot1, slot2);

				/*
				 * Mark the event as done.
				 */
				event->ate_flags &= ~AFTER_TRIGGER_IN_PROGRESS;
				event->ate_flags |= AFTER_TRIGGER_DONE;
			}
			else if (!(event->ate_flags & AFTER_TRIGGER_DONE))
			{
				/* something remains to be done */
				all_fired = all_fired_in_chunk = false;
			}
		}

		/* Clear the chunk if delete_ok and nothing left of interest */
		if (delete_ok && all_fired_in_chunk)
		{
			chunk->freeptr = CHUNK_DATA_START(chunk);
			chunk->endfree = chunk->endptr;

			/*
			 * If it's last chunk, must sync event list's tailfree too.  Note
			 * that delete_ok must NOT be passed as true if there could be
			 * additional AfterTriggerEventList values pointing at this event
			 * list, since we'd fail to fix their copies of tailfree.
			 */
			if (chunk == events->tail)
				events->tailfree = chunk->freeptr;
		}
	}
	if (slot1 != NULL)
	{
		ExecDropSingleTupleTableSlot(slot1);
		ExecDropSingleTupleTableSlot(slot2);
	}

	/* Release working resources */
	MemoryContextDelete(per_tuple_context);

	if (local_estate)
	{
		ExecCloseResultRelations(estate);
		ExecResetTupleTable(estate->es_tupleTable, false);
		FreeExecutorState(estate);
	}

	return all_fired;
}


/*
 * GetAfterTriggersTableData
 *
 * Find or create an AfterTriggersTableData struct for the specified
 * trigger event (relation + operation type).  Ignore existing structs
 * marked "closed"; we don't want to put any additional tuples into them,
 * nor change their stmt-triggers-fired state.
 *
 * Note: the AfterTriggersTableData list is allocated in the current
 * (sub)transaction's CurTransactionContext.  This is OK because
 * we don't need it to live past AfterTriggerEndQuery.
 */
static AfterTriggersTableData *
GetAfterTriggersTableData(Oid relid, CmdType cmdType)
{
	AfterTriggersTableData *table;
	AfterTriggersQueryData *qs;
	MemoryContext oldcxt;
	ListCell   *lc;

	/* Caller should have ensured query_depth is OK. */
	Assert(afterTriggers.query_depth >= 0 &&
		   afterTriggers.query_depth < afterTriggers.maxquerydepth);
	qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	foreach(lc, qs->tables)
	{
		table = (AfterTriggersTableData *) lfirst(lc);
		if (table->relid == relid && table->cmdType == cmdType &&
			!table->closed)
			return table;
	}

	oldcxt = MemoryContextSwitchTo(CurTransactionContext);

	table = (AfterTriggersTableData *) palloc0(sizeof(AfterTriggersTableData));
	table->relid = relid;
	table->cmdType = cmdType;
	qs->tables = lappend(qs->tables, table);

	MemoryContextSwitchTo(oldcxt);

	return table;
}

/*
 * Returns a TupleTableSlot suitable for holding the tuples to be put
 * into AfterTriggersTableData's transition table tuplestores.
 */
static TupleTableSlot *
GetAfterTriggersStoreSlot(AfterTriggersTableData *table,
						  TupleDesc tupdesc)
{
	/* Create it if not already done. */
	if (!table->storeslot)
	{
		MemoryContext oldcxt;

		/*
		 * We need this slot only until AfterTriggerEndQuery, but making it
		 * last till end-of-subxact is good enough.  It'll be freed by
		 * AfterTriggerFreeQuery().  However, the passed-in tupdesc might have
		 * a different lifespan, so we'd better make a copy of that.
		 */
		oldcxt = MemoryContextSwitchTo(CurTransactionContext);
		tupdesc = CreateTupleDescCopy(tupdesc);
		table->storeslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
		MemoryContextSwitchTo(oldcxt);
	}

	return table->storeslot;
}

/*
 * MakeTransitionCaptureState
 *
 * Make a TransitionCaptureState object for the given TriggerDesc, target
 * relation, and operation type.  The TCS object holds all the state needed
 * to decide whether to capture tuples in transition tables.
 *
 * If there are no triggers in 'trigdesc' that request relevant transition
 * tables, then return NULL.
 *
 * The resulting object can be passed to the ExecAR* functions.  When
 * dealing with child tables, the caller can set tcs_original_insert_tuple
 * to avoid having to reconstruct the original tuple in the root table's
 * format.
 *
 * Note that we copy the flags from a parent table into this struct (rather
 * than subsequently using the relation's TriggerDesc directly) so that we can
 * use it to control collection of transition tuples from child tables.
 *
 * Per SQL spec, all operations of the same kind (INSERT/UPDATE/DELETE)
 * on the same table during one query should share one transition table.
 * Therefore, the Tuplestores are owned by an AfterTriggersTableData struct
 * looked up using the table OID + CmdType, and are merely referenced by
 * the TransitionCaptureState objects we hand out to callers.
 */
TransitionCaptureState *
MakeTransitionCaptureState(TriggerDesc *trigdesc, Oid relid, CmdType cmdType)
{
	TransitionCaptureState *state;
	bool		need_old_upd,
				need_new_upd,
				need_old_del,
				need_new_ins;
	AfterTriggersTableData *table;
	MemoryContext oldcxt;
	ResourceOwner saveResourceOwner;

	if (trigdesc == NULL)
		return NULL;

	/* Detect which table(s) we need. */
	switch (cmdType)
	{
		case CMD_INSERT:
			need_old_upd = need_old_del = need_new_upd = false;
			need_new_ins = trigdesc->trig_insert_new_table;
			break;
		case CMD_UPDATE:
			need_old_upd = trigdesc->trig_update_old_table;
			need_new_upd = trigdesc->trig_update_new_table;
			need_old_del = need_new_ins = false;
			break;
		case CMD_DELETE:
			need_old_del = trigdesc->trig_delete_old_table;
			need_old_upd = need_new_upd = need_new_ins = false;
			break;
		case CMD_MERGE:
			need_old_upd = trigdesc->trig_update_old_table;
			need_new_upd = trigdesc->trig_update_new_table;
			need_old_del = trigdesc->trig_delete_old_table;
			need_new_ins = trigdesc->trig_insert_new_table;
			break;
		default:
			elog(ERROR, "unexpected CmdType: %d", (int) cmdType);
			/* keep compiler quiet */
			need_old_upd = need_new_upd = need_old_del = need_new_ins = false;
			break;
	}
	if (!need_old_upd && !need_new_upd && !need_new_ins && !need_old_del)
		return NULL;

	/* Check state, like AfterTriggerSaveEvent. */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "MakeTransitionCaptureState() called outside of query");

	/* Be sure we have enough space to record events at this query depth. */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * Find or create an AfterTriggersTableData struct to hold the
	 * tuplestore(s).  If there's a matching struct but it's marked closed,
	 * ignore it; we need a newer one.
	 *
	 * Note: the AfterTriggersTableData list, as well as the tuplestores, are
	 * allocated in the current (sub)transaction's CurTransactionContext, and
	 * the tuplestores are managed by the (sub)transaction's resource owner.
	 * This is sufficient lifespan because we do not allow triggers using
	 * transition tables to be deferrable; they will be fired during
	 * AfterTriggerEndQuery, after which it's okay to delete the data.
	 */
	table = GetAfterTriggersTableData(relid, cmdType);

	/* Now create required tuplestore(s), if we don't have them already. */
	oldcxt = MemoryContextSwitchTo(CurTransactionContext);
	saveResourceOwner = CurrentResourceOwner;
	CurrentResourceOwner = CurTransactionResourceOwner;

	if (need_old_upd && table->old_upd_tuplestore == NULL)
		table->old_upd_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_new_upd && table->new_upd_tuplestore == NULL)
		table->new_upd_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_old_del && table->old_del_tuplestore == NULL)
		table->old_del_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_new_ins && table->new_ins_tuplestore == NULL)
		table->new_ins_tuplestore = tuplestore_begin_heap(false, false, work_mem);

	CurrentResourceOwner = saveResourceOwner;
	MemoryContextSwitchTo(oldcxt);

	/* Now build the TransitionCaptureState struct, in caller's context */
	state = (TransitionCaptureState *) palloc0(sizeof(TransitionCaptureState));
	state->tcs_delete_old_table = need_old_del;
	state->tcs_update_old_table = need_old_upd;
	state->tcs_update_new_table = need_new_upd;
	state->tcs_insert_new_table = need_new_ins;
	state->tcs_private = table;

	return state;
}


/* ----------
 * AfterTriggerBeginXact()
 *
 *	Called at transaction start (either BEGIN or implicit for single
 *	statement outside of transaction block).
 * ----------
 */
void
AfterTriggerBeginXact(void)
{
	/*
	 * Initialize after-trigger state structure to empty
	 */
	afterTriggers.firing_counter = (CommandId) 1;	/* mustn't be 0 */
	afterTriggers.query_depth = -1;

	/*
	 * Verify that there is no leftover state remaining.  If these assertions
	 * trip, it means that AfterTriggerEndXact wasn't called or didn't clean
	 * up properly.
	 */
	Assert(afterTriggers.state == NULL);
	Assert(afterTriggers.query_stack == NULL);
	Assert(afterTriggers.maxquerydepth == 0);
	Assert(afterTriggers.event_cxt == NULL);
	Assert(afterTriggers.events.head == NULL);
	Assert(afterTriggers.trans_stack == NULL);
	Assert(afterTriggers.maxtransdepth == 0);
}


/* ----------
 * AfterTriggerBeginQuery()
 *
 *	Called just before we start processing a single query within a
 *	transaction (or subtransaction).  Most of the real work gets deferred
 *	until somebody actually tries to queue a trigger event.
 * ----------
 */
void
AfterTriggerBeginQuery(void)
{
	/* Increase the query stack depth */
	afterTriggers.query_depth++;
}


/* ----------
 * AfterTriggerEndQuery()
 *
 *	Called after one query has been completely processed. At this time
 *	we invoke all AFTER IMMEDIATE trigger events queued by the query, and
 *	transfer deferred trigger events to the global deferred-trigger list.
 *
 *	Note that this must be called BEFORE closing down the executor
 *	with ExecutorEnd, because we make use of the EState's info about
 *	target relations.  Normally it is called from ExecutorFinish.
 * ----------
 */
void
AfterTriggerEndQuery(EState *estate)
{
	AfterTriggersQueryData *qs;

	/* Must be inside a query, too */
	Assert(afterTriggers.query_depth >= 0);

	/*
	 * If we never even got as far as initializing the event stack, there
	 * certainly won't be any events, so exit quickly.
	 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
	{
		afterTriggers.query_depth--;
		return;
	}

	/*
	 * Process all immediate-mode triggers queued by the query, and move the
	 * deferred ones to the main list of deferred events.
	 *
	 * Notice that we decide which ones will be fired, and put the deferred
	 * ones on the main list, before anything is actually fired.  This ensures
	 * reasonably sane behavior if a trigger function does SET CONSTRAINTS ...
	 * IMMEDIATE: all events we have decided to defer will be available for it
	 * to fire.
	 *
	 * We loop in case a trigger queues more events at the same query level.
	 * Ordinary trigger functions, including all PL/pgSQL trigger functions,
	 * will instead fire any triggers in a dedicated query level.  Foreign key
	 * enforcement triggers do add to the current query level, thanks to their
	 * passing fire_triggers = false to SPI_execute_snapshot().  Other
	 * C-language triggers might do likewise.
	 *
	 * If we find no firable events, we don't have to increment
	 * firing_counter.
	 */
	qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	for (;;)
	{
		if (afterTriggerMarkEvents(&qs->events, &afterTriggers.events, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;
			AfterTriggerEventChunk *oldtail = qs->events.tail;

			if (afterTriggerInvokeEvents(&qs->events, firing_id, estate, false))
				break;			/* all fired */

			/*
			 * Firing a trigger could result in query_stack being repalloc'd,
			 * so we must recalculate qs after each afterTriggerInvokeEvents
			 * call.  Furthermore, it's unsafe to pass delete_ok = true here,
			 * because that could cause afterTriggerInvokeEvents to try to
			 * access qs->events after the stack has been repalloc'd.
			 */
			qs = &afterTriggers.query_stack[afterTriggers.query_depth];

			/*
			 * We'll need to scan the events list again.  To reduce the cost
			 * of doing so, get rid of completely-fired chunks.  We know that
			 * all events were marked IN_PROGRESS or DONE at the conclusion of
			 * afterTriggerMarkEvents, so any still-interesting events must
			 * have been added after that, and so must be in the chunk that
			 * was then the tail chunk, or in later chunks.  So, zap all
			 * chunks before oldtail.  This is approximately the same set of
			 * events we would have gotten rid of by passing delete_ok = true.
			 */
			Assert(oldtail != NULL);
			while (qs->events.head != oldtail)
				afterTriggerDeleteHeadEventChunk(qs);
		}
		else
			break;
	}

	/* Release query-level-local storage, including tuplestores if any */
	AfterTriggerFreeQuery(&afterTriggers.query_stack[afterTriggers.query_depth]);

	afterTriggers.query_depth--;
}


/*
 * AfterTriggerFreeQuery
 *	Release subsidiary storage for a trigger query level.
 *	This includes closing down tuplestores.
 *	Note: it's important for this to be safe if interrupted by an error
 *	and then called again for the same query level.
 */
static void
AfterTriggerFreeQuery(AfterTriggersQueryData *qs)
{
	Tuplestorestate *ts;
	List	   *tables;
	ListCell   *lc;

	/* Drop the trigger events */
	afterTriggerFreeEventList(&qs->events);

	/* Drop FDW tuplestore if any */
	ts = qs->fdw_tuplestore;
	qs->fdw_tuplestore = NULL;
	if (ts)
		tuplestore_end(ts);

	/* Release per-table subsidiary storage */
	tables = qs->tables;
	foreach(lc, tables)
	{
		AfterTriggersTableData *table = (AfterTriggersTableData *) lfirst(lc);

		ts = table->old_upd_tuplestore;
		table->old_upd_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		ts = table->new_upd_tuplestore;
		table->new_upd_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		ts = table->old_del_tuplestore;
		table->old_del_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		ts = table->new_ins_tuplestore;
		table->new_ins_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		if (table->storeslot)
		{
			TupleTableSlot *slot = table->storeslot;

			table->storeslot = NULL;
			ExecDropSingleTupleTableSlot(slot);
		}
	}

	/*
	 * Now free the AfterTriggersTableData structs and list cells.  Reset list
	 * pointer first; if list_free_deep somehow gets an error, better to leak
	 * that storage than have an infinite loop.
	 */
	qs->tables = NIL;
	list_free_deep(tables);
}


/* ----------
 * AfterTriggerFireDeferred()
 *
 *	Called just before the current transaction is committed. At this
 *	time we invoke all pending DEFERRED triggers.
 *
 *	It is possible for other modules to queue additional deferred triggers
 *	during pre-commit processing; therefore xact.c may have to call this
 *	multiple times.
 * ----------
 */
void
AfterTriggerFireDeferred(void)
{
	AfterTriggerEventList *events;
	bool		snap_pushed = false;

	/* Must not be inside a query */
	Assert(afterTriggers.query_depth == -1);

	/*
	 * If there are any triggers to fire, make sure we have set a snapshot for
	 * them to use.  (Since PortalRunUtility doesn't set a snap for COMMIT, we
	 * can't assume ActiveSnapshot is valid on entry.)
	 */
	events = &afterTriggers.events;
	if (events->head != NULL)
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snap_pushed = true;
	}

	/*
	 * Run all the remaining triggers.  Loop until they are all gone, in case
	 * some trigger queues more for us to do.
	 */
	while (afterTriggerMarkEvents(events, NULL, false))
	{
		CommandId	firing_id = afterTriggers.firing_counter++;

		if (afterTriggerInvokeEvents(events, firing_id, NULL, true))
			break;				/* all fired */
	}

	/*
	 * We don't bother freeing the event list, since it will go away anyway
	 * (and more efficiently than via pfree) in AfterTriggerEndXact.
	 */

	if (snap_pushed)
		PopActiveSnapshot();
}


/* ----------
 * AfterTriggerEndXact()
 *
 *	The current transaction is finishing.
 *
 *	Any unfired triggers are canceled so we simply throw
 *	away anything we know.
 *
 *	Note: it is possible for this to be called repeatedly in case of
 *	error during transaction abort; therefore, do not complain if
 *	already closed down.
 * ----------
 */
void
AfterTriggerEndXact(bool isCommit)
{
	/*
	 * Forget the pending-events list.
	 *
	 * Since all the info is in TopTransactionContext or children thereof, we
	 * don't really need to do anything to reclaim memory.  However, the
	 * pending-events list could be large, and so it's useful to discard it as
	 * soon as possible --- especially if we are aborting because we ran out
	 * of memory for the list!
	 */
	if (afterTriggers.event_cxt)
	{
		MemoryContextDelete(afterTriggers.event_cxt);
		afterTriggers.event_cxt = NULL;
		afterTriggers.events.head = NULL;
		afterTriggers.events.tail = NULL;
		afterTriggers.events.tailfree = NULL;
	}

	/*
	 * Forget any subtransaction state as well.  Since this can't be very
	 * large, we let the eventual reset of TopTransactionContext free the
	 * memory instead of doing it here.
	 */
	afterTriggers.trans_stack = NULL;
	afterTriggers.maxtransdepth = 0;


	/*
	 * Forget the query stack and constraint-related state information.  As
	 * with the subtransaction state information, we don't bother freeing the
	 * memory here.
	 */
	afterTriggers.query_stack = NULL;
	afterTriggers.maxquerydepth = 0;
	afterTriggers.state = NULL;

	/* No more afterTriggers manipulation until next transaction starts. */
	afterTriggers.query_depth = -1;
}

/*
 * AfterTriggerBeginSubXact()
 *
 *	Start a subtransaction.
 */
void
AfterTriggerBeginSubXact(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * Allocate more space in the trans_stack if needed.  (Note: because the
	 * minimum nest level of a subtransaction is 2, we waste the first couple
	 * entries of the array; not worth the notational effort to avoid it.)
	 */
	while (my_level >= afterTriggers.maxtransdepth)
	{
		if (afterTriggers.maxtransdepth == 0)
		{
			/* Arbitrarily initialize for max of 8 subtransaction levels */
			afterTriggers.trans_stack = (AfterTriggersTransData *)
				MemoryContextAlloc(TopTransactionContext,
								   8 * sizeof(AfterTriggersTransData));
			afterTriggers.maxtransdepth = 8;
		}
		else
		{
			/* repalloc will keep the stack in the same context */
			int			new_alloc = afterTriggers.maxtransdepth * 2;

			afterTriggers.trans_stack = (AfterTriggersTransData *)
				repalloc(afterTriggers.trans_stack,
						 new_alloc * sizeof(AfterTriggersTransData));
			afterTriggers.maxtransdepth = new_alloc;
		}
	}

	/*
	 * Push the current information into the stack.  The SET CONSTRAINTS state
	 * is not saved until/unless changed.  Likewise, we don't make a
	 * per-subtransaction event context until needed.
	 */
	afterTriggers.trans_stack[my_level].state = NULL;
	afterTriggers.trans_stack[my_level].events = afterTriggers.events;
	afterTriggers.trans_stack[my_level].query_depth = afterTriggers.query_depth;
	afterTriggers.trans_stack[my_level].firing_counter = afterTriggers.firing_counter;
}

/*
 * AfterTriggerEndSubXact()
 *
 *	The current subtransaction is ending.
 */
void
AfterTriggerEndSubXact(bool isCommit)
{
	int			my_level = GetCurrentTransactionNestLevel();
	SetConstraintState state;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	CommandId	subxact_firing_id;

	/*
	 * Pop the prior state if needed.
	 */
	if (isCommit)
	{
		Assert(my_level < afterTriggers.maxtransdepth);
		/* If we saved a prior state, we don't need it anymore */
		state = afterTriggers.trans_stack[my_level].state;
		if (state != NULL)
			pfree(state);
		/* this avoids double pfree if error later: */
		afterTriggers.trans_stack[my_level].state = NULL;
		Assert(afterTriggers.query_depth ==
			   afterTriggers.trans_stack[my_level].query_depth);
	}
	else
	{
		/*
		 * Aborting.  It is possible subxact start failed before calling
		 * AfterTriggerBeginSubXact, in which case we mustn't risk touching
		 * trans_stack levels that aren't there.
		 */
		if (my_level >= afterTriggers.maxtransdepth)
			return;

		/*
		 * Release query-level storage for queries being aborted, and restore
		 * query_depth to its pre-subxact value.  This assumes that a
		 * subtransaction will not add events to query levels started in a
		 * earlier transaction state.
		 */
		while (afterTriggers.query_depth > afterTriggers.trans_stack[my_level].query_depth)
		{
			if (afterTriggers.query_depth < afterTriggers.maxquerydepth)
				AfterTriggerFreeQuery(&afterTriggers.query_stack[afterTriggers.query_depth]);
			afterTriggers.query_depth--;
		}
		Assert(afterTriggers.query_depth ==
			   afterTriggers.trans_stack[my_level].query_depth);

		/*
		 * Restore the global deferred-event list to its former length,
		 * discarding any events queued by the subxact.
		 */
		afterTriggerRestoreEventList(&afterTriggers.events,
									 &afterTriggers.trans_stack[my_level].events);

		/*
		 * Restore the trigger state.  If the saved state is NULL, then this
		 * subxact didn't save it, so it doesn't need restoring.
		 */
		state = afterTriggers.trans_stack[my_level].state;
		if (state != NULL)
		{
			pfree(afterTriggers.state);
			afterTriggers.state = state;
		}
		/* this avoids double pfree if error later: */
		afterTriggers.trans_stack[my_level].state = NULL;

		/*
		 * Scan for any remaining deferred events that were marked DONE or IN
		 * PROGRESS by this subxact or a child, and un-mark them. We can
		 * recognize such events because they have a firing ID greater than or
		 * equal to the firing_counter value we saved at subtransaction start.
		 * (This essentially assumes that the current subxact includes all
		 * subxacts started after it.)
		 */
		subxact_firing_id = afterTriggers.trans_stack[my_level].firing_counter;
		for_each_event_chunk(event, chunk, afterTriggers.events)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags &
				(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS))
			{
				if (evtshared->ats_firing_id >= subxact_firing_id)
					event->ate_flags &=
						~(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS);
			}
		}
	}
}

/*
 * Get the transition table for the given event and depending on whether we are
 * processing the old or the new tuple.
 */
static Tuplestorestate *
GetAfterTriggersTransitionTable(int event,
								TupleTableSlot *oldslot,
								TupleTableSlot *newslot,
								TransitionCaptureState *transition_capture)
{
	Tuplestorestate *tuplestore = NULL;
	bool		delete_old_table = transition_capture->tcs_delete_old_table;
	bool		update_old_table = transition_capture->tcs_update_old_table;
	bool		update_new_table = transition_capture->tcs_update_new_table;
	bool		insert_new_table = transition_capture->tcs_insert_new_table;

	/*
	 * For INSERT events NEW should be non-NULL, for DELETE events OLD should
	 * be non-NULL, whereas for UPDATE events normally both OLD and NEW are
	 * non-NULL.  But for UPDATE events fired for capturing transition tuples
	 * during UPDATE partition-key row movement, OLD is NULL when the event is
	 * for a row being inserted, whereas NEW is NULL when the event is for a
	 * row being deleted.
	 */
	Assert(!(event == TRIGGER_EVENT_DELETE && delete_old_table &&
			 TupIsNull(oldslot)));
	Assert(!(event == TRIGGER_EVENT_INSERT && insert_new_table &&
			 TupIsNull(newslot)));

	if (!TupIsNull(oldslot))
	{
		Assert(TupIsNull(newslot));
		if (event == TRIGGER_EVENT_DELETE && delete_old_table)
			tuplestore = transition_capture->tcs_private->old_del_tuplestore;
		else if (event == TRIGGER_EVENT_UPDATE && update_old_table)
			tuplestore = transition_capture->tcs_private->old_upd_tuplestore;
	}
	else if (!TupIsNull(newslot))
	{
		Assert(TupIsNull(oldslot));
		if (event == TRIGGER_EVENT_INSERT && insert_new_table)
			tuplestore = transition_capture->tcs_private->new_ins_tuplestore;
		else if (event == TRIGGER_EVENT_UPDATE && update_new_table)
			tuplestore = transition_capture->tcs_private->new_upd_tuplestore;
	}

	return tuplestore;
}

/*
 * Add the given heap tuple to the given tuplestore, applying the conversion
 * map if necessary.
 *
 * If original_insert_tuple is given, we can add that tuple without conversion.
 */
static void
TransitionTableAddTuple(EState *estate,
						TransitionCaptureState *transition_capture,
						ResultRelInfo *relinfo,
						TupleTableSlot *slot,
						TupleTableSlot *original_insert_tuple,
						Tuplestorestate *tuplestore)
{
	TupleConversionMap *map;

	/*
	 * Nothing needs to be done if we don't have a tuplestore.
	 */
	if (tuplestore == NULL)
		return;

	if (original_insert_tuple)
		tuplestore_puttupleslot(tuplestore, original_insert_tuple);
	else if ((map = ExecGetChildToRootMap(relinfo)) != NULL)
	{
		AfterTriggersTableData *table = transition_capture->tcs_private;
		TupleTableSlot *storeslot;

		storeslot = GetAfterTriggersStoreSlot(table, map->outdesc);
		execute_attr_map_slot(map->attrMap, slot, storeslot);
		tuplestore_puttupleslot(tuplestore, storeslot);
	}
	else
		tuplestore_puttupleslot(tuplestore, slot);
}

/* ----------
 * AfterTriggerEnlargeQueryState()
 *
 *	Prepare the necessary state so that we can record AFTER trigger events
 *	queued by a query.  It is allowed to have nested queries within a
 *	(sub)transaction, so we need to have separate state for each query
 *	nesting level.
 * ----------
 */
static void
AfterTriggerEnlargeQueryState(void)
{
	int			init_depth = afterTriggers.maxquerydepth;

	Assert(afterTriggers.query_depth >= afterTriggers.maxquerydepth);

	if (afterTriggers.maxquerydepth == 0)
	{
		int			new_alloc = Max(afterTriggers.query_depth + 1, 8);

		afterTriggers.query_stack = (AfterTriggersQueryData *)
			MemoryContextAlloc(TopTransactionContext,
							   new_alloc * sizeof(AfterTriggersQueryData));
		afterTriggers.maxquerydepth = new_alloc;
	}
	else
	{
		/* repalloc will keep the stack in the same context */
		int			old_alloc = afterTriggers.maxquerydepth;
		int			new_alloc = Max(afterTriggers.query_depth + 1,
									old_alloc * 2);

		afterTriggers.query_stack = (AfterTriggersQueryData *)
			repalloc(afterTriggers.query_stack,
					 new_alloc * sizeof(AfterTriggersQueryData));
		afterTriggers.maxquerydepth = new_alloc;
	}

	/* Initialize new array entries to empty */
	while (init_depth < afterTriggers.maxquerydepth)
	{
		AfterTriggersQueryData *qs = &afterTriggers.query_stack[init_depth];

		qs->events.head = NULL;
		qs->events.tail = NULL;
		qs->events.tailfree = NULL;
		qs->fdw_tuplestore = NULL;
		qs->tables = NIL;

		++init_depth;
	}
}

/*
 * Create an empty SetConstraintState with room for numalloc trigstates
 */
static SetConstraintState
SetConstraintStateCreate(int numalloc)
{
	SetConstraintState state;

	/* Behave sanely with numalloc == 0 */
	if (numalloc <= 0)
		numalloc = 1;

	/*
	 * We assume that zeroing will correctly initialize the state values.
	 */
	state = (SetConstraintState)
		MemoryContextAllocZero(TopTransactionContext,
							   offsetof(SetConstraintStateData, trigstates) +
							   numalloc * sizeof(SetConstraintTriggerData));

	state->numalloc = numalloc;

	return state;
}

/*
 * Copy a SetConstraintState
 */
static SetConstraintState
SetConstraintStateCopy(SetConstraintState origstate)
{
	SetConstraintState state;

	state = SetConstraintStateCreate(origstate->numstates);

	state->all_isset = origstate->all_isset;
	state->all_isdeferred = origstate->all_isdeferred;
	state->numstates = origstate->numstates;
	memcpy(state->trigstates, origstate->trigstates,
		   origstate->numstates * sizeof(SetConstraintTriggerData));

	return state;
}

/*
 * Add a per-trigger item to a SetConstraintState.  Returns possibly-changed
 * pointer to the state object (it will change if we have to repalloc).
 */
static SetConstraintState
SetConstraintStateAddItem(SetConstraintState state,
						  Oid tgoid, bool tgisdeferred)
{
	if (state->numstates >= state->numalloc)
	{
		int			newalloc = state->numalloc * 2;

		newalloc = Max(newalloc, 8);	/* in case original has size 0 */
		state = (SetConstraintState)
			repalloc(state,
					 offsetof(SetConstraintStateData, trigstates) +
					 newalloc * sizeof(SetConstraintTriggerData));
		state->numalloc = newalloc;
		Assert(state->numstates < state->numalloc);
	}

	state->trigstates[state->numstates].sct_tgoid = tgoid;
	state->trigstates[state->numstates].sct_tgisdeferred = tgisdeferred;
	state->numstates++;

	return state;
}

/* ----------
 * AfterTriggerSetState()
 *
 *	Execute the SET CONSTRAINTS ... utility command.
 * ----------
 */
void
AfterTriggerSetState(ConstraintsSetStmt *stmt)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/* If we haven't already done so, initialize our state. */
	if (afterTriggers.state == NULL)
		afterTriggers.state = SetConstraintStateCreate(8);

	/*
	 * If in a subtransaction, and we didn't save the current state already,
	 * save it so it can be restored if the subtransaction aborts.
	 */
	if (my_level > 1 &&
		afterTriggers.trans_stack[my_level].state == NULL)
	{
		afterTriggers.trans_stack[my_level].state =
			SetConstraintStateCopy(afterTriggers.state);
	}

	/*
	 * Handle SET CONSTRAINTS ALL ...
	 */
	if (stmt->constraints == NIL)
	{
		/*
		 * Forget any previous SET CONSTRAINTS commands in this transaction.
		 */
		afterTriggers.state->numstates = 0;

		/*
		 * Set the per-transaction ALL state to known.
		 */
		afterTriggers.state->all_isset = true;
		afterTriggers.state->all_isdeferred = stmt->deferred;
	}
	else
	{
		Relation	conrel;
		Relation	tgrel;
		List	   *conoidlist = NIL;
		List	   *tgoidlist = NIL;
		ListCell   *lc;

		/*
		 * Handle SET CONSTRAINTS constraint-name [, ...]
		 *
		 * First, identify all the named constraints and make a list of their
		 * OIDs.  Since, unlike the SQL spec, we allow multiple constraints of
		 * the same name within a schema, the specifications are not
		 * necessarily unique.  Our strategy is to target all matching
		 * constraints within the first search-path schema that has any
		 * matches, but disregard matches in schemas beyond the first match.
		 * (This is a bit odd but it's the historical behavior.)
		 *
		 * A constraint in a partitioned table may have corresponding
		 * constraints in the partitions.  Grab those too.
		 */
		conrel = table_open(ConstraintRelationId, AccessShareLock);

		foreach(lc, stmt->constraints)
		{
			RangeVar   *constraint = lfirst(lc);
			bool		found;
			List	   *namespacelist;
			ListCell   *nslc;

			if (constraint->catalogname)
			{
				if (strcmp(constraint->catalogname, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
									constraint->catalogname, constraint->schemaname,
									constraint->relname)));
			}

			/*
			 * If we're given the schema name with the constraint, look only
			 * in that schema.  If given a bare constraint name, use the
			 * search path to find the first matching constraint.
			 */
			if (constraint->schemaname)
			{
				Oid			namespaceId = LookupExplicitNamespace(constraint->schemaname,
																  false);

				namespacelist = list_make1_oid(namespaceId);
			}
			else
			{
				namespacelist = fetch_search_path(true);
			}

			found = false;
			foreach(nslc, namespacelist)
			{
				Oid			namespaceId = lfirst_oid(nslc);
				SysScanDesc conscan;
				ScanKeyData skey[2];
				HeapTuple	tup;

				ScanKeyInit(&skey[0],
							Anum_pg_constraint_conname,
							BTEqualStrategyNumber, F_NAMEEQ,
							CStringGetDatum(constraint->relname));
				ScanKeyInit(&skey[1],
							Anum_pg_constraint_connamespace,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(namespaceId));

				conscan = systable_beginscan(conrel, ConstraintNameNspIndexId,
											 true, NULL, 2, skey);

				while (HeapTupleIsValid(tup = systable_getnext(conscan)))
				{
					Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

					if (con->condeferrable)
						conoidlist = lappend_oid(conoidlist, con->oid);
					else if (stmt->deferred)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("constraint \"%s\" is not deferrable",
										constraint->relname)));
					found = true;
				}

				systable_endscan(conscan);

				/*
				 * Once we've found a matching constraint we do not search
				 * later parts of the search path.
				 */
				if (found)
					break;
			}

			list_free(namespacelist);

			/*
			 * Not found ?
			 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("constraint \"%s\" does not exist",
								constraint->relname)));
		}

		/*
		 * Scan for any possible descendants of the constraints.  We append
		 * whatever we find to the same list that we're scanning; this has the
		 * effect that we create new scans for those, too, so if there are
		 * further descendents, we'll also catch them.
		 */
		foreach(lc, conoidlist)
		{
			Oid			parent = lfirst_oid(lc);
			ScanKeyData key;
			SysScanDesc scan;
			HeapTuple	tuple;

			ScanKeyInit(&key,
						Anum_pg_constraint_conparentid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(parent));

			scan = systable_beginscan(conrel, ConstraintParentIndexId, true, NULL, 1, &key);

			while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

				conoidlist = lappend_oid(conoidlist, con->oid);
			}

			systable_endscan(scan);
		}

		table_close(conrel, AccessShareLock);

		/*
		 * Now, locate the trigger(s) implementing each of these constraints,
		 * and make a list of their OIDs.
		 */
		tgrel = table_open(TriggerRelationId, AccessShareLock);

		foreach(lc, conoidlist)
		{
			Oid			conoid = lfirst_oid(lc);
			ScanKeyData skey;
			SysScanDesc tgscan;
			HeapTuple	htup;

			ScanKeyInit(&skey,
						Anum_pg_trigger_tgconstraint,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(conoid));

			tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
										NULL, 1, &skey);

			while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
			{
				Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);

				/*
				 * Silently skip triggers that are marked as non-deferrable in
				 * pg_trigger.  This is not an error condition, since a
				 * deferrable RI constraint may have some non-deferrable
				 * actions.
				 */
				if (pg_trigger->tgdeferrable)
					tgoidlist = lappend_oid(tgoidlist, pg_trigger->oid);
			}

			systable_endscan(tgscan);
		}

		table_close(tgrel, AccessShareLock);

		/*
		 * Now we can set the trigger states of individual triggers for this
		 * xact.
		 */
		foreach(lc, tgoidlist)
		{
			Oid			tgoid = lfirst_oid(lc);
			SetConstraintState state = afterTriggers.state;
			bool		found = false;
			int			i;

			for (i = 0; i < state->numstates; i++)
			{
				if (state->trigstates[i].sct_tgoid == tgoid)
				{
					state->trigstates[i].sct_tgisdeferred = stmt->deferred;
					found = true;
					break;
				}
			}
			if (!found)
			{
				afterTriggers.state =
					SetConstraintStateAddItem(state, tgoid, stmt->deferred);
			}
		}
	}

	/*
	 * SQL99 requires that when a constraint is set to IMMEDIATE, any deferred
	 * checks against that constraint must be made when the SET CONSTRAINTS
	 * command is executed -- i.e. the effects of the SET CONSTRAINTS command
	 * apply retroactively.  We've updated the constraints state, so scan the
	 * list of previously deferred events to fire any that have now become
	 * immediate.
	 *
	 * Obviously, if this was SET ... DEFERRED then it can't have converted
	 * any unfired events to immediate, so we need do nothing in that case.
	 */
	if (!stmt->deferred)
	{
		AfterTriggerEventList *events = &afterTriggers.events;
		bool		snapshot_set = false;

		while (afterTriggerMarkEvents(events, NULL, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;

			/*
			 * Make sure a snapshot has been established in case trigger
			 * functions need one.  Note that we avoid setting a snapshot if
			 * we don't find at least one trigger that has to be fired now.
			 * This is so that BEGIN; SET CONSTRAINTS ...; SET TRANSACTION
			 * ISOLATION LEVEL SERIALIZABLE; ... works properly.  (If we are
			 * at the start of a transaction it's not possible for any trigger
			 * events to be queued yet.)
			 */
			if (!snapshot_set)
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				snapshot_set = true;
			}

			/*
			 * We can delete fired events if we are at top transaction level,
			 * but we'd better not if inside a subtransaction, since the
			 * subtransaction could later get rolled back.
			 */
			if (afterTriggerInvokeEvents(events, firing_id, NULL,
										 !IsSubTransaction()))
				break;			/* all fired */
		}

		if (snapshot_set)
			PopActiveSnapshot();
	}
}

/* ----------
 * AfterTriggerPendingOnRel()
 *		Test to see if there are any pending after-trigger events for rel.
 *
 * This is used by TRUNCATE, CLUSTER, ALTER TABLE, etc to detect whether
 * it is unsafe to perform major surgery on a relation.  Note that only
 * local pending events are examined.  We assume that having exclusive lock
 * on a rel guarantees there are no unserviced events in other backends ---
 * but having a lock does not prevent there being such events in our own.
 *
 * In some scenarios it'd be reasonable to remove pending events (more
 * specifically, mark them DONE by the current subxact) but without a lot
 * of knowledge of the trigger semantics we can't do this in general.
 * ----------
 */
bool
AfterTriggerPendingOnRel(Oid relid)
{
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	int			depth;

	/* Scan queued events */
	for_each_event_chunk(event, chunk, afterTriggers.events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);

		/*
		 * We can ignore completed events.  (Even if a DONE flag is rolled
		 * back by subxact abort, it's OK because the effects of the TRUNCATE
		 * or whatever must get rolled back too.)
		 */
		if (event->ate_flags & AFTER_TRIGGER_DONE)
			continue;

		if (evtshared->ats_relid == relid)
			return true;
	}

	/*
	 * Also scan events queued by incomplete queries.  This could only matter
	 * if TRUNCATE/etc is executed by a function or trigger within an updating
	 * query on the same relation, which is pretty perverse, but let's check.
	 */
	for (depth = 0; depth <= afterTriggers.query_depth && depth < afterTriggers.maxquerydepth; depth++)
	{
		for_each_event_chunk(event, chunk, afterTriggers.query_stack[depth].events)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags & AFTER_TRIGGER_DONE)
				continue;

			if (evtshared->ats_relid == relid)
				return true;
		}
	}

	return false;
}

/* ----------
 * AfterTriggerSaveEvent()
 *
 *	Called by ExecA[RS]...Triggers() to queue up the triggers that should
 *	be fired for an event.
 *
 *	NOTE: this is called whenever there are any triggers associated with
 *	the event (even if they are disabled).  This function decides which
 *	triggers actually need to be queued.  It is also called after each row,
 *	even if there are no triggers for that event, if there are any AFTER
 *	STATEMENT triggers for the statement which use transition tables, so that
 *	the transition tuplestores can be built.  Furthermore, if the transition
 *	capture is happening for UPDATEd rows being moved to another partition due
 *	to the partition-key being changed, then this function is called once when
 *	the row is deleted (to capture OLD row), and once when the row is inserted
 *	into another partition (to capture NEW row).  This is done separately because
 *	DELETE and INSERT happen on different tables.
 *
 *	Transition tuplestores are built now, rather than when events are pulled
 *	off of the queue because AFTER ROW triggers are allowed to select from the
 *	transition tables for the statement.
 *
 *	This contains special support to queue the update events for the case where
 *	a partitioned table undergoing a cross-partition update may have foreign
 *	keys pointing into it.  Normally, a partitioned table's row triggers are
 *	not fired because the leaf partition(s) which are modified as a result of
 *	the operation on the partitioned table contain the same triggers which are
 *	fired instead. But that general scheme can cause problematic behavior with
 *	foreign key triggers during cross-partition updates, which are implemented
 *	as DELETE on the source partition followed by INSERT into the destination
 *	partition.  Specifically, firing DELETE triggers would lead to the wrong
 *	foreign key action to be enforced considering that the original command is
 *	UPDATE; in this case, this function is called with relinfo as the
 *	partitioned table, and src_partinfo and dst_partinfo referring to the
 *	source and target leaf partitions, respectively.
 *
 *	is_crosspart_update is true either when a DELETE event is fired on the
 *	source partition (which is to be ignored) or an UPDATE event is fired on
 *	the root partitioned table.
 * ----------
 */
static void
AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
					  ResultRelInfo *src_partinfo,
					  ResultRelInfo *dst_partinfo,
					  int event, bool row_trigger,
					  TupleTableSlot *oldslot, TupleTableSlot *newslot,
					  List *recheckIndexes, Bitmapset *modifiedCols,
					  TransitionCaptureState *transition_capture,
					  bool is_crosspart_update)
{
	Relation	rel = relinfo->ri_RelationDesc;
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	AfterTriggerEventData new_event;
	AfterTriggerSharedData new_shared;
	char		relkind = rel->rd_rel->relkind;
	int			tgtype_event;
	int			tgtype_level;
	int			i;
	Tuplestorestate *fdw_tuplestore = NULL;

	/*
	 * Check state.  We use a normal test not Assert because it is possible to
	 * reach here in the wrong state given misconfigured RI triggers, in
	 * particular deferring a cascade action trigger.
	 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "AfterTriggerSaveEvent() called outside of query");

	/* Be sure we have enough space to record events at this query depth. */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * If the directly named relation has any triggers with transition tables,
	 * then we need to capture transition tuples.
	 */
	if (row_trigger && transition_capture != NULL)
	{
		TupleTableSlot *original_insert_tuple = transition_capture->tcs_original_insert_tuple;

		/*
		 * Capture the old tuple in the appropriate transition table based on
		 * the event.
		 */
		if (!TupIsNull(oldslot))
		{
			Tuplestorestate *old_tuplestore;

			old_tuplestore = GetAfterTriggersTransitionTable(event,
															 oldslot,
															 NULL,
															 transition_capture);
			TransitionTableAddTuple(estate, transition_capture, relinfo,
									oldslot, NULL, old_tuplestore);
		}

		/*
		 * Capture the new tuple in the appropriate transition table based on
		 * the event.
		 */
		if (!TupIsNull(newslot))
		{
			Tuplestorestate *new_tuplestore;

			new_tuplestore = GetAfterTriggersTransitionTable(event,
															 NULL,
															 newslot,
															 transition_capture);
			TransitionTableAddTuple(estate, transition_capture, relinfo,
									newslot, original_insert_tuple, new_tuplestore);
		}

		/*
		 * If transition tables are the only reason we're here, return. As
		 * mentioned above, we can also be here during update tuple routing in
		 * presence of transition tables, in which case this function is
		 * called separately for OLD and NEW, so we expect exactly one of them
		 * to be NULL.
		 */
		if (trigdesc == NULL ||
			(event == TRIGGER_EVENT_DELETE && !trigdesc->trig_delete_after_row) ||
			(event == TRIGGER_EVENT_INSERT && !trigdesc->trig_insert_after_row) ||
			(event == TRIGGER_EVENT_UPDATE && !trigdesc->trig_update_after_row) ||
			(event == TRIGGER_EVENT_UPDATE && (TupIsNull(oldslot) ^ TupIsNull(newslot))))
			return;
	}

	/*
	 * We normally don't see partitioned tables here for row level triggers
	 * except in the special case of a cross-partition update.  In that case,
	 * nodeModifyTable.c:ExecCrossPartitionUpdateForeignKey() calls here to
	 * queue an update event on the root target partitioned table, also
	 * passing the source and destination partitions and their tuples.
	 */
	Assert(!row_trigger ||
		   rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE ||
		   (is_crosspart_update &&
			TRIGGER_FIRED_BY_UPDATE(event) &&
			src_partinfo != NULL && dst_partinfo != NULL));

	/*
	 * Validate the event code and collect the associated tuple CTIDs.
	 *
	 * The event code will be used both as a bitmask and an array offset, so
	 * validation is important to make sure we don't walk off the edge of our
	 * arrays.
	 *
	 * Also, if we're considering statement-level triggers, check whether we
	 * already queued a set of them for this event, and cancel the prior set
	 * if so.  This preserves the behavior that statement-level triggers fire
	 * just once per statement and fire after row-level triggers.
	 */
	switch (event)
	{
		case TRIGGER_EVENT_INSERT:
			tgtype_event = TRIGGER_TYPE_INSERT;
			if (row_trigger)
			{
				Assert(oldslot == NULL);
				Assert(newslot != NULL);
				ItemPointerCopy(&(newslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_INSERT, event);
			}
			break;
		case TRIGGER_EVENT_DELETE:
			tgtype_event = TRIGGER_TYPE_DELETE;
			if (row_trigger)
			{
				Assert(oldslot != NULL);
				Assert(newslot == NULL);
				ItemPointerCopy(&(oldslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_DELETE, event);
			}
			break;
		case TRIGGER_EVENT_UPDATE:
			tgtype_event = TRIGGER_TYPE_UPDATE;
			if (row_trigger)
			{
				Assert(oldslot != NULL);
				Assert(newslot != NULL);
				ItemPointerCopy(&(oldslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerCopy(&(newslot->tts_tid), &(new_event.ate_ctid2));

				/*
				 * Also remember the OIDs of partitions to fetch these tuples
				 * out of later in AfterTriggerExecute().
				 */
				if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				{
					Assert(src_partinfo != NULL && dst_partinfo != NULL);
					new_event.ate_src_part =
						RelationGetRelid(src_partinfo->ri_RelationDesc);
					new_event.ate_dst_part =
						RelationGetRelid(dst_partinfo->ri_RelationDesc);
				}
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_UPDATE, event);
			}
			break;
		case TRIGGER_EVENT_TRUNCATE:
			tgtype_event = TRIGGER_TYPE_TRUNCATE;
			Assert(oldslot == NULL);
			Assert(newslot == NULL);
			ItemPointerSetInvalid(&(new_event.ate_ctid1));
			ItemPointerSetInvalid(&(new_event.ate_ctid2));
			break;
		default:
			elog(ERROR, "invalid after-trigger event code: %d", event);
			tgtype_event = 0;	/* keep compiler quiet */
			break;
	}

	/* Determine flags */
	if (!(relkind == RELKIND_FOREIGN_TABLE && row_trigger))
	{
		if (row_trigger && event == TRIGGER_EVENT_UPDATE)
		{
			if (relkind == RELKIND_PARTITIONED_TABLE)
				new_event.ate_flags = AFTER_TRIGGER_CP_UPDATE;
			else
				new_event.ate_flags = AFTER_TRIGGER_2CTID;
		}
		else
			new_event.ate_flags = AFTER_TRIGGER_1CTID;
	}

	/* else, we'll initialize ate_flags for each trigger */

	tgtype_level = (row_trigger ? TRIGGER_TYPE_ROW : TRIGGER_TYPE_STATEMENT);

	/*
	 * Must convert/copy the source and destination partition tuples into the
	 * root partitioned table's format/slot, because the processing in the
	 * loop below expects both oldslot and newslot tuples to be in that form.
	 */
	if (row_trigger && rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		TupleTableSlot *rootslot;
		TupleConversionMap *map;

		rootslot = ExecGetTriggerOldSlot(estate, relinfo);
		map = ExecGetChildToRootMap(src_partinfo);
		if (map)
			oldslot = execute_attr_map_slot(map->attrMap,
											oldslot,
											rootslot);
		else
			oldslot = ExecCopySlot(rootslot, oldslot);

		rootslot = ExecGetTriggerNewSlot(estate, relinfo);
		map = ExecGetChildToRootMap(dst_partinfo);
		if (map)
			newslot = execute_attr_map_slot(map->attrMap,
											newslot,
											rootslot);
		else
			newslot = ExecCopySlot(rootslot, newslot);
	}

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  tgtype_level,
								  TRIGGER_TYPE_AFTER,
								  tgtype_event))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, event,
							modifiedCols, oldslot, newslot))
			continue;

		if (relkind == RELKIND_FOREIGN_TABLE && row_trigger)
		{
			if (fdw_tuplestore == NULL)
			{
				fdw_tuplestore = GetCurrentFDWTuplestore();
				new_event.ate_flags = AFTER_TRIGGER_FDW_FETCH;
			}
			else
				/* subsequent event for the same tuple */
				new_event.ate_flags = AFTER_TRIGGER_FDW_REUSE;
		}

		/*
		 * If the trigger is a foreign key enforcement trigger, there are
		 * certain cases where we can skip queueing the event because we can
		 * tell by inspection that the FK constraint will still pass. There
		 * are also some cases during cross-partition updates of a partitioned
		 * table where queuing the event can be skipped.
		 */
		if (TRIGGER_FIRED_BY_UPDATE(event) || TRIGGER_FIRED_BY_DELETE(event))
		{
			switch (RI_FKey_trigger_type(trigger->tgfoid))
			{
				case RI_TRIGGER_PK:

					/*
					 * For cross-partitioned updates of partitioned PK table,
					 * skip the event fired by the component delete on the
					 * source leaf partition unless the constraint originates
					 * in the partition itself (!tgisclone), because the
					 * update event that will be fired on the root
					 * (partitioned) target table will be used to perform the
					 * necessary foreign key enforcement action.
					 */
					if (is_crosspart_update &&
						TRIGGER_FIRED_BY_DELETE(event) &&
						trigger->tgisclone)
						continue;

					/* Update or delete on trigger's PK table */
					if (!RI_FKey_pk_upd_check_required(trigger, rel,
													   oldslot, newslot))
					{
						/* skip queuing this event */
						continue;
					}
					break;

				case RI_TRIGGER_FK:

					/*
					 * Update on trigger's FK table.  We can skip the update
					 * event fired on a partitioned table during a
					 * cross-partition of that table, because the insert event
					 * that is fired on the destination leaf partition would
					 * suffice to perform the necessary foreign key check.
					 * Moreover, RI_FKey_fk_upd_check_required() expects to be
					 * passed a tuple that contains system attributes, most of
					 * which are not present in the virtual slot belonging to
					 * a partitioned table.
					 */
					if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
						!RI_FKey_fk_upd_check_required(trigger, rel,
													   oldslot, newslot))
					{
						/* skip queuing this event */
						continue;
					}
					break;

				case RI_TRIGGER_NONE:

					/*
					 * Not an FK trigger.  No need to queue the update event
					 * fired during a cross-partitioned update of a
					 * partitioned table, because the same row trigger must be
					 * present in the leaf partition(s) that are affected as
					 * part of this update and the events fired on them are
					 * queued instead.
					 */
					if (row_trigger &&
						rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
						continue;
					break;
			}
		}

		/*
		 * If the trigger is a deferred unique constraint check trigger, only
		 * queue it if the unique constraint was potentially violated, which
		 * we know from index insertion time.
		 */
		if (trigger->tgfoid == F_UNIQUE_KEY_RECHECK)
		{
			if (!list_member_oid(recheckIndexes, trigger->tgconstrindid))
				continue;		/* Uniqueness definitely not violated */
		}

		/*
		 * Fill in event structure and add it to the current query's queue.
		 * Note we set ats_table to NULL whenever this trigger doesn't use
		 * transition tables, to improve sharability of the shared event data.
		 */
		new_shared.ats_event =
			(event & TRIGGER_EVENT_OPMASK) |
			(row_trigger ? TRIGGER_EVENT_ROW : 0) |
			(trigger->tgdeferrable ? AFTER_TRIGGER_DEFERRABLE : 0) |
			(trigger->tginitdeferred ? AFTER_TRIGGER_INITDEFERRED : 0);
		new_shared.ats_tgoid = trigger->tgoid;
		new_shared.ats_relid = RelationGetRelid(rel);
		new_shared.ats_rolid = GetUserId();
		new_shared.ats_firing_id = 0;
		if ((trigger->tgoldtable || trigger->tgnewtable) &&
			transition_capture != NULL)
			new_shared.ats_table = transition_capture->tcs_private;
		else
			new_shared.ats_table = NULL;
		new_shared.ats_modifiedcols = modifiedCols;

		afterTriggerAddEvent(&afterTriggers.query_stack[afterTriggers.query_depth].events,
							 &new_event, &new_shared);
	}

	/*
	 * Finally, spool any foreign tuple(s).  The tuplestore squashes them to
	 * minimal tuples, so this loses any system columns.  The executor lost
	 * those columns before us, for an unrelated reason, so this is fine.
	 */
	if (fdw_tuplestore)
	{
		if (oldslot != NULL)
			tuplestore_puttupleslot(fdw_tuplestore, oldslot);
		if (newslot != NULL)
			tuplestore_puttupleslot(fdw_tuplestore, newslot);
	}
}

/*
 * Detect whether we already queued BEFORE STATEMENT triggers for the given
 * relation + operation, and set the flag so the next call will report "true".
 */
static bool
before_stmt_triggers_fired(Oid relid, CmdType cmdType)
{
	bool		result;
	AfterTriggersTableData *table;

	/* Check state, like AfterTriggerSaveEvent. */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "before_stmt_triggers_fired() called outside of query");

	/* Be sure we have enough space to record events at this query depth. */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * We keep this state in the AfterTriggersTableData that also holds
	 * transition tables for the relation + operation.  In this way, if we are
	 * forced to make a new set of transition tables because more tuples get
	 * entered after we've already fired triggers, we will allow a new set of
	 * statement triggers to get queued.
	 */
	table = GetAfterTriggersTableData(relid, cmdType);
	result = table->before_trig_done;
	table->before_trig_done = true;
	return result;
}

/*
 * If we previously queued a set of AFTER STATEMENT triggers for the given
 * relation + operation, and they've not been fired yet, cancel them.  The
 * caller will queue a fresh set that's after any row-level triggers that may
 * have been queued by the current sub-statement, preserving (as much as
 * possible) the property that AFTER ROW triggers fire before AFTER STATEMENT
 * triggers, and that the latter only fire once.  This deals with the
 * situation where several FK enforcement triggers sequentially queue triggers
 * for the same table into the same trigger query level.  We can't fully
 * prevent odd behavior though: if there are AFTER ROW triggers taking
 * transition tables, we don't want to change the transition tables once the
 * first such trigger has seen them.  In such a case, any additional events
 * will result in creating new transition tables and allowing new firings of
 * statement triggers.
 *
 * This also saves the current event list location so that a later invocation
 * of this function can cheaply find the triggers we're about to queue and
 * cancel them.
 */
static void
cancel_prior_stmt_triggers(Oid relid, CmdType cmdType, int tgevent)
{
	AfterTriggersTableData *table;
	AfterTriggersQueryData *qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	/*
	 * We keep this state in the AfterTriggersTableData that also holds
	 * transition tables for the relation + operation.  In this way, if we are
	 * forced to make a new set of transition tables because more tuples get
	 * entered after we've already fired triggers, we will allow a new set of
	 * statement triggers to get queued without canceling the old ones.
	 */
	table = GetAfterTriggersTableData(relid, cmdType);

	if (table->after_trig_done)
	{
		/*
		 * We want to start scanning from the tail location that existed just
		 * before we inserted any statement triggers.  But the events list
		 * might've been entirely empty then, in which case scan from the
		 * current head.
		 */
		AfterTriggerEvent event;
		AfterTriggerEventChunk *chunk;

		if (table->after_trig_events.tail)
		{
			chunk = table->after_trig_events.tail;
			event = (AfterTriggerEvent) table->after_trig_events.tailfree;
		}
		else
		{
			chunk = qs->events.head;
			event = NULL;
		}

		for_each_chunk_from(chunk)
		{
			if (event == NULL)
				event = (AfterTriggerEvent) CHUNK_DATA_START(chunk);
			for_each_event_from(event, chunk)
			{
				AfterTriggerShared evtshared = GetTriggerSharedData(event);

				/*
				 * Exit loop when we reach events that aren't AS triggers for
				 * the target relation.
				 */
				if (evtshared->ats_relid != relid)
					goto done;
				if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) != tgevent)
					goto done;
				if (!TRIGGER_FIRED_FOR_STATEMENT(evtshared->ats_event))
					goto done;
				if (!TRIGGER_FIRED_AFTER(evtshared->ats_event))
					goto done;
				/* OK, mark it DONE */
				event->ate_flags &= ~AFTER_TRIGGER_IN_PROGRESS;
				event->ate_flags |= AFTER_TRIGGER_DONE;
			}
			/* signal we must reinitialize event ptr for next chunk */
			event = NULL;
		}
	}
done:

	/* In any case, save current insertion point for next time */
	table->after_trig_done = true;
	table->after_trig_events = qs->events;
}

/*
 * GUC assign_hook for session_replication_role
 */
void
assign_session_replication_role(int newval, void *extra)
{
	/*
	 * Must flush the plan cache when changing replication role; but don't
	 * flush unnecessarily.
	 */
	if (SessionReplicationRole != newval)
		ResetPlanCache();
}

/*
 * SQL function pg_trigger_depth()
 */
Datum
pg_trigger_depth(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyTriggerDepth);
}

/*
 * Check whether a trigger modified a virtual generated column and replace the
 * value with null if so.
 *
 * We need to check this so that we don't end up storing a non-null value in a
 * virtual generated column.
 *
 * We don't need to check for stored generated columns, since those will be
 * overwritten later anyway.
 */
static HeapTuple
check_modified_virtual_generated(TupleDesc tupdesc, HeapTuple tuple)
{
	if (!(tupdesc->constr && tupdesc->constr->has_generated_virtual))
		return tuple;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
		{
			if (!heap_attisnull(tuple, i + 1, tupdesc))
			{
				int			replCol = i + 1;
				Datum		replValue = 0;
				bool		replIsnull = true;

				tuple = heap_modify_tuple_by_cols(tuple, tupdesc, 1, &replCol, &replValue, &replIsnull);
			}
		}
	}

	return tuple;
}
