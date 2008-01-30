/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  Commands for creating and altering table structures and settings
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/tablecmds.c,v 1.241 2008/01/30 19:46:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "parser/gramparse.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/*
 * ON COMMIT action list
 */
typedef struct OnCommitItem
{
	Oid			relid;			/* relid of relation */
	OnCommitAction oncommit;	/* what to do at end of xact */

	/*
	 * If this entry was created during the current transaction,
	 * creating_subid is the ID of the creating subxact; if created in a prior
	 * transaction, creating_subid is zero.  If deleted during the current
	 * transaction, deleting_subid is the ID of the deleting subxact; if no
	 * deletion request is pending, deleting_subid is zero.
	 */
	SubTransactionId creating_subid;
	SubTransactionId deleting_subid;
} OnCommitItem;

static List *on_commits = NIL;


/*
 * State information for ALTER TABLE
 *
 * The pending-work queue for an ALTER TABLE is a List of AlteredTableInfo
 * structs, one for each table modified by the operation (the named table
 * plus any child tables that are affected).  We save lists of subcommands
 * to apply to this table (possibly modified by parse transformation steps);
 * these lists will be executed in Phase 2.  If a Phase 3 step is needed,
 * necessary information is stored in the constraints and newvals lists.
 *
 * Phase 2 is divided into multiple passes; subcommands are executed in
 * a pass determined by subcommand type.
 */

#define AT_PASS_DROP			0		/* DROP (all flavors) */
#define AT_PASS_ALTER_TYPE		1		/* ALTER COLUMN TYPE */
#define AT_PASS_OLD_INDEX		2		/* re-add existing indexes */
#define AT_PASS_OLD_CONSTR		3		/* re-add existing constraints */
#define AT_PASS_COL_ATTRS		4		/* set other column attributes */
/* We could support a RENAME COLUMN pass here, but not currently used */
#define AT_PASS_ADD_COL			5		/* ADD COLUMN */
#define AT_PASS_ADD_INDEX		6		/* ADD indexes */
#define AT_PASS_ADD_CONSTR		7		/* ADD constraints, defaults */
#define AT_PASS_MISC			8		/* other stuff */
#define AT_NUM_PASSES			9

typedef struct AlteredTableInfo
{
	/* Information saved before any work commences: */
	Oid			relid;			/* Relation to work on */
	char		relkind;		/* Its relkind */
	TupleDesc	oldDesc;		/* Pre-modification tuple descriptor */
	/* Information saved by Phase 1 for Phase 2: */
	List	   *subcmds[AT_NUM_PASSES]; /* Lists of AlterTableCmd */
	/* Information saved by Phases 1/2 for Phase 3: */
	List	   *constraints;	/* List of NewConstraint */
	List	   *newvals;		/* List of NewColumnValue */
	bool		new_notnull;	/* T if we added new NOT NULL constraints */
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
	/* Objects to rebuild after completing ALTER TYPE operations */
	List	   *changedConstraintOids;	/* OIDs of constraints to rebuild */
	List	   *changedConstraintDefs;	/* string definitions of same */
	List	   *changedIndexOids;		/* OIDs of indexes to rebuild */
	List	   *changedIndexDefs;		/* string definitions of same */
} AlteredTableInfo;

/* Struct describing one new constraint to check in Phase 3 scan */
/* Note: new NOT NULL constraints are handled elsewhere */
typedef struct NewConstraint
{
	char	   *name;			/* Constraint name, or NULL if none */
	ConstrType	contype;		/* CHECK or FOREIGN */
	Oid			refrelid;		/* PK rel, if FOREIGN */
	Oid			conid;			/* OID of pg_constraint entry, if FOREIGN */
	Node	   *qual;			/* Check expr or FkConstraint struct */
	List	   *qualstate;		/* Execution state for CHECK */
} NewConstraint;

/*
 * Struct describing one new column value that needs to be computed during
 * Phase 3 copy (this could be either a new column with a non-null default, or
 * a column that we're changing the type of).  Columns without such an entry
 * are just copied from the old table during ATRewriteTable.  Note that the
 * expr is an expression over *old* table values.
 */
typedef struct NewColumnValue
{
	AttrNumber	attnum;			/* which column */
	Expr	   *expr;			/* expression to compute */
	ExprState  *exprstate;		/* execution state */
} NewColumnValue;


static void truncate_check_rel(Relation rel);
static List *MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr, int *supOidCount);
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel);
static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel);
static void add_nonduplicate_constraint(Constraint *cdef,
							ConstrCheck *check, int *ncheck);
static bool change_varattnos_walker(Node *node, const AttrNumber *newattno);
static void StoreCatalogInheritance(Oid relationId, List *supers);
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int16 seqNumber, Relation inhRelation);
static int	findAttrByName(const char *attributeName, List *schema);
static void setRelhassubclassInRelation(Oid relationId, bool relhassubclass);
static void AlterIndexNamespaces(Relation classRel, Relation rel,
					 Oid oldNspOid, Oid newNspOid);
static void AlterSeqNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid,
				   const char *newNspName);
static int transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids);
static int transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids,
						   Oid *opclasses);
static Oid transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						Oid *opclasses);
static void validateForeignKeyConstraint(FkConstraint *fkconstraint,
							 Relation rel, Relation pkrel, Oid constraintOid);
static void createForeignKeyTriggers(Relation rel, FkConstraint *fkconstraint,
						 Oid constraintOid);
static void ATController(Relation rel, List *cmds, bool recurse);
static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing);
static void ATRewriteCatalogs(List **wqueue);
static void ATExecCmd(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd);
static void ATRewriteTables(List **wqueue);
static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap);
static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
static void ATSimplePermissions(Relation rel, bool allowView);
static void ATSimplePermissionsRelationOrIndex(Relation rel);
static void ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse);
static void ATOneLevelRecursion(List **wqueue, Relation rel,
					AlterTableCmd *cmd);
static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse,
				AlterTableCmd *cmd);
static void ATExecAddColumn(AlteredTableInfo *tab, Relation rel,
				ColumnDef *colDef);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void ATExecDropNotNull(Relation rel, const char *colName);
static void ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName);
static void ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault);
static void ATPrepSetStatistics(Relation rel, const char *colName,
					Node *flagValue);
static void ATExecSetStatistics(Relation rel, const char *colName,
					Node *newValue);
static void ATExecSetStorage(Relation rel, const char *colName,
				 Node *newValue);
static void ATExecDropColumn(Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing);
static void ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild);
static void ATExecAddConstraint(AlteredTableInfo *tab, Relation rel,
					Node *newConstraint);
static void ATAddForeignKeyConstraint(AlteredTableInfo *tab, Relation rel,
						  FkConstraint *fkconstraint);
static void ATPrepDropConstraint(List **wqueue, Relation rel,
					 bool recurse, AlterTableCmd *cmd);
static void ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior, bool quiet);
static void ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd);
static void ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  const char *colName, TypeName *typename);
static void ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab);
static void ATPostAlterTypeParse(char *cmd, List **wqueue);
static void change_owner_recurse_to_sequences(Oid relationOid,
								  Oid newOwnerId);
static void ATExecClusterOn(Relation rel, const char *indexName);
static void ATExecDropCluster(Relation rel);
static void ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel,
					char *tablespacename);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace);
static void ATExecSetRelOptions(Relation rel, List *defList, bool isReset);
static void ATExecEnableDisableTrigger(Relation rel, char *trigname,
						   char fires_when, bool skip_system);
static void ATExecEnableDisableRule(Relation rel, char *rulename,
						char fires_when);
static void ATExecAddInherit(Relation rel, RangeVar *parent);
static void ATExecDropInherit(Relation rel, RangeVar *parent);
static void copy_relation_data(Relation rel, SMgrRelation dst);


/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 *
 * If successful, returns the OID of the new relation.
 * ----------------------------------------------------------------
 */
Oid
DefineRelation(CreateStmt *stmt, char relkind)
{
	char		relname[NAMEDATALEN];
	Oid			namespaceId;
	List	   *schema = stmt->tableElts;
	Oid			relationId;
	Oid			tablespaceId;
	Relation	rel;
	TupleDesc	descriptor;
	List	   *inheritOids;
	List	   *old_constraints;
	bool		localHasOids;
	int			parentOidCount;
	List	   *rawDefaults;
	Datum		reloptions;
	ListCell   *listptr;
	AttrNumber	attnum;

	/*
	 * Truncate relname to appropriate length (probably a waste of time, as
	 * parser should have done this already).
	 */
	StrNCpy(relname, stmt->relation->relname, NAMEDATALEN);

	/*
	 * Check consistency of arguments
	 */
	if (stmt->oncommit != ONCOMMIT_NOOP && !stmt->relation->istemp)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("ON COMMIT can only be used on temporary tables")));

	/*
	 * Look up the namespace in which we are supposed to create the relation.
	 * Check we have permission to create there. Skip check if bootstrapping,
	 * since permissions machinery may not be working yet.
	 */
	namespaceId = RangeVarGetCreationNamespace(stmt->relation);

	if (!IsBootstrapProcessingMode())
	{
		AclResult	aclresult;

		aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(),
										  ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(namespaceId));
	}

	/*
	 * Select tablespace to use.  If not specified, use default tablespace
	 * (which may in turn default to database's default).
	 */
	if (stmt->tablespacename)
	{
		tablespaceId = get_tablespace_oid(stmt->tablespacename);
		if (!OidIsValid(tablespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							stmt->tablespacename)));
	}
	else
	{
		tablespaceId = GetDefaultTablespace(stmt->relation->istemp);
		/* note InvalidOid is OK in this case */
	}

	/* Check permissions except when using database's default */
	if (OidIsValid(tablespaceId))
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/*
	 * Parse and validate reloptions, if any.
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options, true, false);

	(void) heap_reloptions(relkind, reloptions, true);

	/*
	 * Look up inheritance ancestors and generate relation schema, including
	 * inherited attributes.
	 */
	schema = MergeAttributes(schema, stmt->inhRelations,
							 stmt->relation->istemp,
							 &inheritOids, &old_constraints, &parentOidCount);

	/*
	 * Create a relation descriptor from the relation schema and create the
	 * relation.  Note that in this stage only inherited (pre-cooked) defaults
	 * and constraints will be included into the new relation.
	 * (BuildDescForRelation takes care of the inherited defaults, but we have
	 * to copy inherited constraints here.)
	 */
	descriptor = BuildDescForRelation(schema);

	localHasOids = interpretOidsOption(stmt->options);
	descriptor->tdhasoid = (localHasOids || parentOidCount > 0);

	if (old_constraints || stmt->constraints)
	{
		ConstrCheck *check;
		int			ncheck = 0;

		/* make array that's certainly big enough */
		check = (ConstrCheck *)
			palloc((list_length(old_constraints) +
					list_length(stmt->constraints)) * sizeof(ConstrCheck));
		/* deal with constraints from MergeAttributes */
		foreach(listptr, old_constraints)
		{
			Constraint *cdef = (Constraint *) lfirst(listptr);

			if (cdef->contype == CONSTR_CHECK)
				add_nonduplicate_constraint(cdef, check, &ncheck);
		}

		/*
		 * parse_utilcmd.c might have passed some precooked constraints too,
		 * due to LIKE tab INCLUDING CONSTRAINTS
		 */
		foreach(listptr, stmt->constraints)
		{
			Constraint *cdef = (Constraint *) lfirst(listptr);

			if (cdef->contype == CONSTR_CHECK && cdef->cooked_expr != NULL)
				add_nonduplicate_constraint(cdef, check, &ncheck);
		}
		/* if we found any, insert 'em into the descriptor */
		if (ncheck > 0)
		{
			if (descriptor->constr == NULL)
			{
				descriptor->constr = (TupleConstr *) palloc(sizeof(TupleConstr));
				descriptor->constr->defval = NULL;
				descriptor->constr->num_defval = 0;
				descriptor->constr->has_not_null = false;
			}
			descriptor->constr->num_check = ncheck;
			descriptor->constr->check = check;
		}
	}

	relationId = heap_create_with_catalog(relname,
										  namespaceId,
										  tablespaceId,
										  InvalidOid,
										  GetUserId(),
										  descriptor,
										  relkind,
										  false,
										  localHasOids,
										  parentOidCount,
										  stmt->oncommit,
										  reloptions,
										  allowSystemTableMods);

	StoreCatalogInheritance(relationId, inheritOids);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire exclusive lock on it.	This isn't
	 * really necessary for locking out other backends (since they can't see
	 * the new rel anyway until we commit), but it keeps the lock manager from
	 * complaining about deadlock risks.
	 */
	rel = relation_open(relationId, AccessExclusiveLock);

	/*
	 * Now add any newly specified column default values and CHECK constraints
	 * to the new relation.  These are passed to us in the form of raw
	 * parsetrees; we need to transform them to executable expression trees
	 * before they can be added. The most convenient way to do that is to
	 * apply the parser's transformExpr routine, but transformExpr doesn't
	 * work unless we have a pre-existing relation. So, the transformation has
	 * to be postponed to this final step of CREATE TABLE.
	 *
	 * First, scan schema to find new column defaults.
	 */
	rawDefaults = NIL;
	attnum = 0;

	foreach(listptr, schema)
	{
		ColumnDef  *colDef = lfirst(listptr);

		attnum++;

		if (colDef->raw_default != NULL)
		{
			RawColumnDefault *rawEnt;

			Assert(colDef->cooked_default == NULL);

			rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
			rawEnt->attnum = attnum;
			rawEnt->raw_default = colDef->raw_default;
			rawDefaults = lappend(rawDefaults, rawEnt);
		}
	}

	/*
	 * Parse and add the defaults/constraints, if any.
	 */
	if (rawDefaults || stmt->constraints)
		AddRelationRawConstraints(rel, rawDefaults, stmt->constraints);

	/*
	 * Clean up.  We keep lock on new relation (although it shouldn't be
	 * visible to anyone else anyway, until commit).
	 */
	relation_close(rel, NoLock);

	return relationId;
}

/*
 * RemoveRelation
 *		Deletes a relation.
 */
void
RemoveRelation(const RangeVar *relation, DropBehavior behavior)
{
	Oid			relOid;
	ObjectAddress object;

	relOid = RangeVarGetRelid(relation, false);

	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * ExecuteTruncate
 *		Executes a TRUNCATE command.
 *
 * This is a multi-relation truncate.  We first open and grab exclusive
 * lock on all relations involved, checking permissions and otherwise
 * verifying that the relation is OK for truncation.  In CASCADE mode,
 * relations having FK references to the targeted relations are automatically
 * added to the group; in RESTRICT mode, we check that all FK references are
 * internal to the group that's being truncated.  Finally all the relations
 * are truncated and reindexed.
 */
void
ExecuteTruncate(TruncateStmt *stmt)
{
	List	   *rels = NIL;
	List	   *relids = NIL;
	ListCell   *cell;

	/*
	 * Open, exclusive-lock, and check all the explicitly-specified relations
	 */
	foreach(cell, stmt->relations)
	{
		RangeVar   *rv = lfirst(cell);
		Relation	rel;

		rel = heap_openrv(rv, AccessExclusiveLock);
		truncate_check_rel(rel);
		rels = lappend(rels, rel);
		relids = lappend_oid(relids, RelationGetRelid(rel));
	}

	/*
	 * In CASCADE mode, suck in all referencing relations as well.	This
	 * requires multiple iterations to find indirectly-dependent relations. At
	 * each phase, we need to exclusive-lock new rels before looking for their
	 * dependencies, else we might miss something.	Also, we check each rel as
	 * soon as we open it, to avoid a faux pas such as holding lock for a long
	 * time on a rel we have no permissions for.
	 */
	if (stmt->behavior == DROP_CASCADE)
	{
		for (;;)
		{
			List	   *newrelids;

			newrelids = heap_truncate_find_FKs(relids);
			if (newrelids == NIL)
				break;			/* nothing else to add */

			foreach(cell, newrelids)
			{
				Oid			relid = lfirst_oid(cell);
				Relation	rel;

				rel = heap_open(relid, AccessExclusiveLock);
				ereport(NOTICE,
						(errmsg("truncate cascades to table \"%s\"",
								RelationGetRelationName(rel))));
				truncate_check_rel(rel);
				rels = lappend(rels, rel);
				relids = lappend_oid(relids, relid);
			}
		}
	}

	/*
	 * Check foreign key references.  In CASCADE mode, this should be
	 * unnecessary since we just pulled in all the references; but as a
	 * cross-check, do it anyway if in an Assert-enabled build.
	 */
#ifdef USE_ASSERT_CHECKING
	heap_truncate_check_FKs(rels, false);
#else
	if (stmt->behavior == DROP_RESTRICT)
		heap_truncate_check_FKs(rels, false);
#endif

	/*
	 * OK, truncate each table.
	 */
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);
		Oid			heap_relid;
		Oid			toast_relid;

		/*
		 * Create a new empty storage file for the relation, and assign it as
		 * the relfilenode value.	The old storage file is scheduled for
		 * deletion at commit.
		 */
		setNewRelfilenode(rel, RecentXmin);

		heap_relid = RelationGetRelid(rel);
		toast_relid = rel->rd_rel->reltoastrelid;

		heap_close(rel, NoLock);

		/*
		 * The same for the toast table, if any.
		 */
		if (OidIsValid(toast_relid))
		{
			rel = relation_open(toast_relid, AccessExclusiveLock);
			setNewRelfilenode(rel, RecentXmin);
			heap_close(rel, NoLock);
		}

		/*
		 * Reconstruct the indexes to match, and we're done.
		 */
		reindex_relation(heap_relid, true);
	}
}

/*
 * Check that a given rel is safe to truncate.	Subroutine for ExecuteTruncate
 */
static void
truncate_check_rel(Relation rel)
{
	/* Only allow truncate on regular tables */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/*
	 * We can never allow truncation of shared or nailed-in-cache relations,
	 * because we can't support changing their relfilenode values.
	 */
	if (rel->rd_rel->relisshared || rel->rd_isnailed)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot truncate system relation \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * Don't allow truncate on temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("cannot truncate temporary tables of other sessions")));

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 */
	CheckTableNotInUse(rel, "TRUNCATE");
}

/*----------
 * MergeAttributes
 *		Returns new schema given initial schema and superclasses.
 *
 * Input arguments:
 * 'schema' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'supers' is a list of names (as RangeVar nodes) of parent relations.
 * 'istemp' is TRUE if we are creating a temp relation.
 *
 * Output arguments:
 * 'supOids' receives a list of the OIDs of the parent relations.
 * 'supconstr' receives a list of constraints belonging to the parents,
 *		updated as necessary to be valid for the child.
 * 'supOidCount' is set to the number of parents that have OID columns.
 *
 * Return value:
 * Completed schema list.
 *
 * Notes:
 *	  The order in which the attributes are inherited is very important.
 *	  Intuitively, the inherited attributes should come first. If a table
 *	  inherits from multiple parents, the order of those attributes are
 *	  according to the order of the parents specified in CREATE TABLE.
 *
 *	  Here's an example:
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  The order of the attributes of stud_emp is:
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   If the same attribute name appears multiple times, then it appears
 *	   in the result table in the proper location for its first appearance.
 *
 *	   Constraints (including NOT NULL constraints) for the child table
 *	   are the union of all relevant constraints, from both the child schema
 *	   and parent tables.
 *
 *	   The default value for a child column is defined as:
 *		(1) If the child schema specifies a default, that value is used.
 *		(2) If neither the child nor any parent specifies a default, then
 *			the column will not have a default.
 *		(3) If conflicting defaults are inherited from different parents
 *			(and not overridden by the child), an error is raised.
 *		(4) Otherwise the inherited default is used.
 *		Rule (3) is new in Postgres 7.1; in earlier releases you got a
 *		rather arbitrary choice of which parent default to use.
 *----------
 */
static List *
MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr, int *supOidCount)
{
	ListCell   *entry;
	List	   *inhSchema = NIL;
	List	   *parentOids = NIL;
	List	   *constraints = NIL;
	int			parentsWithOids = 0;
	bool		have_bogus_defaults = false;
	char	   *bogus_marker = "Bogus!";		/* marks conflicting defaults */
	int			child_attno;

	/*
	 * Check for and reject tables with too many columns. We perform this
	 * check relatively early for two reasons: (a) we don't run the risk of
	 * overflowing an AttrNumber in subsequent code (b) an O(n^2) algorithm is
	 * okay if we're processing <= 1600 columns, but could take minutes to
	 * execute if the user attempts to create a table with hundreds of
	 * thousands of columns.
	 *
	 * Note that we also need to check that any we do not exceed this figure
	 * after including columns from inherited relations.
	 */
	if (list_length(schema) > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * Check for duplicate names in the explicit list of attributes.
	 *
	 * Although we might consider merging such entries in the same way that we
	 * handle name conflicts for inherited attributes, it seems to make more
	 * sense to assume such conflicts are errors.
	 */
	foreach(entry, schema)
	{
		ColumnDef  *coldef = lfirst(entry);
		ListCell   *rest;

		for_each_cell(rest, lnext(entry))
		{
			ColumnDef  *restdef = lfirst(rest);

			if (strcmp(coldef->colname, restdef->colname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" specified more than once",
								coldef->colname)));
		}
	}

	/*
	 * Scan the parents left-to-right, and merge their attributes to form a
	 * list of inherited attributes (inhSchema).  Also check to see if we need
	 * to inherit an OID column.
	 */
	child_attno = 0;
	foreach(entry, supers)
	{
		RangeVar   *parent = (RangeVar *) lfirst(entry);
		Relation	relation;
		TupleDesc	tupleDesc;
		TupleConstr *constr;
		AttrNumber *newattno;
		AttrNumber	parent_attno;

		relation = heap_openrv(parent, AccessShareLock);

		if (relation->rd_rel->relkind != RELKIND_RELATION)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("inherited relation \"%s\" is not a table",
							parent->relname)));
		/* Permanent rels cannot inherit from temporary ones */
		if (!istemp && isTempNamespace(RelationGetNamespace(relation)))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from temporary relation \"%s\"",
							parent->relname)));

		/*
		 * We should have an UNDER permission flag for this, but for now,
		 * demand that creator of a child table own the parent.
		 */
		if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
						   RelationGetRelationName(relation));

		/*
		 * Reject duplications in the list of parents.
		 */
		if (list_member_oid(parentOids, RelationGetRelid(relation)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
			 errmsg("relation \"%s\" would be inherited from more than once",
					parent->relname)));

		parentOids = lappend_oid(parentOids, RelationGetRelid(relation));

		if (relation->rd_rel->relhasoids)
			parentsWithOids++;

		tupleDesc = RelationGetDescr(relation);
		constr = tupleDesc->constr;

		/*
		 * newattno[] will contain the child-table attribute numbers for the
		 * attributes of this parent table.  (They are not the same for
		 * parents after the first one, nor if we have dropped columns.)
		 */
		newattno = (AttrNumber *)
			palloc(tupleDesc->natts * sizeof(AttrNumber));

		for (parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
			char	   *attributeName = NameStr(attribute->attname);
			int			exist_attno;
			ColumnDef  *def;

			/*
			 * Ignore dropped columns in the parent.
			 */
			if (attribute->attisdropped)
			{
				/*
				 * change_varattnos_of_a_node asserts that this is greater
				 * than zero, so if anything tries to use it, we should find
				 * out.
				 */
				newattno[parent_attno - 1] = 0;
				continue;
			}

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				Oid			defTypeId;
				int32		deftypmod;

				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				ereport(NOTICE,
						(errmsg("merging multiple inherited definitions of column \"%s\"",
								attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				defTypeId = typenameTypeId(NULL, def->typename, &deftypmod);
				if (defTypeId != attribute->atttypid ||
					deftypmod != attribute->atttypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("inherited column \"%s\" has a type conflict",
							   attributeName),
							 errdetail("%s versus %s",
									   TypeNameToString(def->typename),
									   format_type_be(attribute->atttypid))));
				def->inhcount++;
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= attribute->attnotnull;
				/* Default and other constraints are handled below */
				newattno[parent_attno - 1] = exist_attno;
			}
			else
			{
				/*
				 * No, create a new inherited column
				 */
				def = makeNode(ColumnDef);
				def->colname = pstrdup(attributeName);
				def->typename = makeTypeNameFromOid(attribute->atttypid,
													attribute->atttypmod);
				def->inhcount = 1;
				def->is_local = false;
				def->is_not_null = attribute->attnotnull;
				def->raw_default = NULL;
				def->cooked_default = NULL;
				def->constraints = NIL;
				inhSchema = lappend(inhSchema, def);
				newattno[parent_attno - 1] = ++child_attno;
			}

			/*
			 * Copy default if any
			 */
			if (attribute->atthasdef)
			{
				char	   *this_default = NULL;
				AttrDefault *attrdef;
				int			i;

				/* Find default in constraint structure */
				Assert(constr != NULL);
				attrdef = constr->defval;
				for (i = 0; i < constr->num_defval; i++)
				{
					if (attrdef[i].adnum == parent_attno)
					{
						this_default = attrdef[i].adbin;
						break;
					}
				}
				Assert(this_default != NULL);

				/*
				 * If default expr could contain any vars, we'd need to fix
				 * 'em, but it can't; so default is ready to apply to child.
				 *
				 * If we already had a default from some prior parent, check
				 * to see if they are the same.  If so, no problem; if not,
				 * mark the column as having a bogus default. Below, we will
				 * complain if the bogus default isn't overridden by the child
				 * schema.
				 */
				Assert(def->raw_default == NULL);
				if (def->cooked_default == NULL)
					def->cooked_default = pstrdup(this_default);
				else if (strcmp(def->cooked_default, this_default) != 0)
				{
					def->cooked_default = bogus_marker;
					have_bogus_defaults = true;
				}
			}
		}

		/*
		 * Now copy the constraints of this parent, adjusting attnos using the
		 * completed newattno[] map
		 */
		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;
			int			i;

			for (i = 0; i < constr->num_check; i++)
			{
				Constraint *cdef = makeNode(Constraint);
				Node	   *expr;

				cdef->contype = CONSTR_CHECK;
				cdef->name = pstrdup(check[i].ccname);
				cdef->raw_expr = NULL;
				/* adjust varattnos of ccbin here */
				expr = stringToNode(check[i].ccbin);
				change_varattnos_of_a_node(expr, newattno);
				cdef->cooked_expr = nodeToString(expr);
				constraints = lappend(constraints, cdef);
			}
		}

		pfree(newattno);

		/*
		 * Close the parent rel, but keep our AccessShareLock on it until xact
		 * commit.	That will prevent someone else from deleting or ALTERing
		 * the parent before the child is committed.
		 */
		heap_close(relation, NoLock);
	}

	/*
	 * If we had no inherited attributes, the result schema is just the
	 * explicitly declared columns.  Otherwise, we need to merge the declared
	 * columns into the inherited schema list.
	 */
	if (inhSchema != NIL)
	{
		foreach(entry, schema)
		{
			ColumnDef  *newdef = lfirst(entry);
			char	   *attributeName = newdef->colname;
			int			exist_attno;

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				ColumnDef  *def;
				Oid			defTypeId,
							newTypeId;
				int32		deftypmod,
							newtypmod;

				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				ereport(NOTICE,
				   (errmsg("merging column \"%s\" with inherited definition",
						   attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				defTypeId = typenameTypeId(NULL, def->typename, &deftypmod);
				newTypeId = typenameTypeId(NULL, newdef->typename, &newtypmod);
				if (defTypeId != newTypeId || deftypmod != newtypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("column \"%s\" has a type conflict",
									attributeName),
							 errdetail("%s versus %s",
									   TypeNameToString(def->typename),
									   TypeNameToString(newdef->typename))));
				/* Mark the column as locally defined */
				def->is_local = true;
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= newdef->is_not_null;
				/* If new def has a default, override previous default */
				if (newdef->raw_default != NULL)
				{
					def->raw_default = newdef->raw_default;
					def->cooked_default = newdef->cooked_default;
				}
			}
			else
			{
				/*
				 * No, attach new column to result schema
				 */
				inhSchema = lappend(inhSchema, newdef);
			}
		}

		schema = inhSchema;

		/*
		 * Check that we haven't exceeded the legal # of columns after merging
		 * in inherited columns.
		 */
		if (list_length(schema) > MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("tables can have at most %d columns",
							MaxHeapAttributeNumber)));
	}

	/*
	 * If we found any conflicting parent default values, check to make sure
	 * they were overridden by the child.
	 */
	if (have_bogus_defaults)
	{
		foreach(entry, schema)
		{
			ColumnDef  *def = lfirst(entry);

			if (def->cooked_default == bogus_marker)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				  errmsg("column \"%s\" inherits conflicting default values",
						 def->colname),
						 errhint("To resolve the conflict, specify a default explicitly.")));
		}
	}

	*supOids = parentOids;
	*supconstr = constraints;
	*supOidCount = parentsWithOids;
	return schema;
}


/*
 * In multiple-inheritance situations, it's possible to inherit
 * the same grandparent constraint through multiple parents.
 * Hence, we want to discard inherited constraints that match as to
 * both name and expression.  Otherwise, gripe if there are conflicting
 * names.  Nonconflicting constraints are added to the array check[]
 * of length *ncheck ... caller must ensure there is room!
 */
static void
add_nonduplicate_constraint(Constraint *cdef, ConstrCheck *check, int *ncheck)
{
	int			i;

	/* Should only see precooked constraints here */
	Assert(cdef->contype == CONSTR_CHECK);
	Assert(cdef->name != NULL);
	Assert(cdef->raw_expr == NULL && cdef->cooked_expr != NULL);

	for (i = 0; i < *ncheck; i++)
	{
		if (strcmp(check[i].ccname, cdef->name) != 0)
			continue;
		if (strcmp(check[i].ccbin, cdef->cooked_expr) == 0)
			return;				/* duplicate constraint, so ignore it */
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("check constraint name \"%s\" appears multiple times but with different expressions",
						cdef->name)));
	}
	/* No match on name, so add it to array */
	check[*ncheck].ccname = cdef->name;
	check[*ncheck].ccbin = pstrdup(cdef->cooked_expr);
	(*ncheck)++;
}


/*
 * Replace varattno values in an expression tree according to the given
 * map array, that is, varattno N is replaced by newattno[N-1].  It is
 * caller's responsibility to ensure that the array is long enough to
 * define values for all user varattnos present in the tree.  System column
 * attnos remain unchanged.
 *
 * Note that the passed node tree is modified in-place!
 */
void
change_varattnos_of_a_node(Node *node, const AttrNumber *newattno)
{
	/* no setup needed, so away we go */
	(void) change_varattnos_walker(node, newattno);
}

static bool
change_varattnos_walker(Node *node, const AttrNumber *newattno)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0 && var->varno == 1 &&
			var->varattno > 0)
		{
			/*
			 * ??? the following may be a problem when the node is multiply
			 * referenced though stringToNode() doesn't create such a node
			 * currently.
			 */
			Assert(newattno[var->varattno - 1] > 0);
			var->varattno = newattno[var->varattno - 1];
		}
		return false;
	}
	return expression_tree_walker(node, change_varattnos_walker,
								  (void *) newattno);
}

/*
 * Generate a map for change_varattnos_of_a_node from old and new TupleDesc's,
 * matching according to column name.
 */
AttrNumber *
varattnos_map(TupleDesc old, TupleDesc new)
{
	AttrNumber *attmap;
	int			i,
				j;

	attmap = (AttrNumber *) palloc0(sizeof(AttrNumber) * old->natts);
	for (i = 1; i <= old->natts; i++)
	{
		if (old->attrs[i - 1]->attisdropped)
			continue;			/* leave the entry as zero */

		for (j = 1; j <= new->natts; j++)
		{
			if (strcmp(NameStr(old->attrs[i - 1]->attname),
					   NameStr(new->attrs[j - 1]->attname)) == 0)
			{
				attmap[i - 1] = j;
				break;
			}
		}
	}
	return attmap;
}

/*
 * Generate a map for change_varattnos_of_a_node from a TupleDesc and a list
 * of ColumnDefs
 */
AttrNumber *
varattnos_map_schema(TupleDesc old, List *schema)
{
	AttrNumber *attmap;
	int			i;

	attmap = (AttrNumber *) palloc0(sizeof(AttrNumber) * old->natts);
	for (i = 1; i <= old->natts; i++)
	{
		if (old->attrs[i - 1]->attisdropped)
			continue;			/* leave the entry as zero */

		attmap[i - 1] = findAttrByName(NameStr(old->attrs[i - 1]->attname),
									   schema);
	}
	return attmap;
}


/*
 * StoreCatalogInheritance
 *		Updates the system catalogs with proper inheritance information.
 *
 * supers is a list of the OIDs of the new relation's direct ancestors.
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers)
{
	Relation	relation;
	int16		seqNumber;
	ListCell   *entry;

	/*
	 * sanity checks
	 */
	AssertArg(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/*
	 * Store INHERITS information in pg_inherits using direct ancestors only.
	 * Also enter dependencies on the direct ancestors, and make sure they are
	 * marked with relhassubclass = true.
	 *
	 * (Once upon a time, both direct and indirect ancestors were found here
	 * and then entered into pg_ipl.  Since that catalog doesn't exist
	 * anymore, there's no need to look for indirect ancestors.)
	 */
	relation = heap_open(InheritsRelationId, RowExclusiveLock);

	seqNumber = 1;
	foreach(entry, supers)
	{
		Oid			parentOid = lfirst_oid(entry);

		StoreCatalogInheritance1(relationId, parentOid, seqNumber, relation);
		seqNumber++;
	}

	heap_close(relation, RowExclusiveLock);
}

/*
 * Make catalog entries showing relationId as being an inheritance child
 * of parentOid.  inhRelation is the already-opened pg_inherits catalog.
 */
static void
StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int16 seqNumber, Relation inhRelation)
{
	TupleDesc	desc = RelationGetDescr(inhRelation);
	Datum		datum[Natts_pg_inherits];
	char		nullarr[Natts_pg_inherits];
	ObjectAddress childobject,
				parentobject;
	HeapTuple	tuple;

	/*
	 * Make the pg_inherits entry
	 */
	datum[0] = ObjectIdGetDatum(relationId);	/* inhrelid */
	datum[1] = ObjectIdGetDatum(parentOid);		/* inhparent */
	datum[2] = Int16GetDatum(seqNumber);		/* inhseqno */

	nullarr[0] = ' ';
	nullarr[1] = ' ';
	nullarr[2] = ' ';

	tuple = heap_formtuple(desc, datum, nullarr);

	simple_heap_insert(inhRelation, tuple);

	CatalogUpdateIndexes(inhRelation, tuple);

	heap_freetuple(tuple);

	/*
	 * Store a dependency too
	 */
	parentobject.classId = RelationRelationId;
	parentobject.objectId = parentOid;
	parentobject.objectSubId = 0;
	childobject.classId = RelationRelationId;
	childobject.objectId = relationId;
	childobject.objectSubId = 0;

	recordDependencyOn(&childobject, &parentobject, DEPENDENCY_NORMAL);

	/*
	 * Mark the parent as having subclasses.
	 */
	setRelhassubclassInRelation(parentOid, true);
}

/*
 * Look for an existing schema entry with the given name.
 *
 * Returns the index (starting with 1) if attribute already exists in schema,
 * 0 if it doesn't.
 */
static int
findAttrByName(const char *attributeName, List *schema)
{
	ListCell   *s;
	int			i = 1;

	foreach(s, schema)
	{
		ColumnDef  *def = lfirst(s);

		if (strcmp(attributeName, def->colname) == 0)
			return i;

		i++;
	}
	return 0;
}

/*
 * Update a relation's pg_class.relhassubclass entry to the given value
 */
static void
setRelhassubclassInRelation(Oid relationId, bool relhassubclass)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Form_pg_class classtuple;

	/*
	 * Fetch a modifiable copy of the tuple, modify it, update pg_class.
	 *
	 * If the tuple already has the right relhassubclass setting, we don't
	 * need to update it, but we still need to issue an SI inval message.
	 */
	relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(relationId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	classtuple = (Form_pg_class) GETSTRUCT(tuple);

	if (classtuple->relhassubclass != relhassubclass)
	{
		classtuple->relhassubclass = relhassubclass;
		simple_heap_update(relationRelation, &tuple->t_self, tuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relationRelation, tuple);
	}
	else
	{
		/* no need to change tuple, but force relcache rebuild anyway */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);
	heap_close(relationRelation, RowExclusiveLock);
}


/*
 *		renameatt		- changes the name of a attribute in a relation
 *
 *		Attname attribute is changed in attribute catalog.
 *		No record of the previous attname is kept (correct?).
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan attribute catalog
 *				for name conflict (within rel)
 *				for original attribute (if not arg)
 *		modify attname in attribute tuple
 *		insert modified attribute in attribute catalog
 *		delete original attribute from attribute catalog
 */
void
renameatt(Oid myrelid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse,
		  bool recursing)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	atttup;
	Form_pg_attribute attform;
	int			attnum;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);

	/*
	 * permissions checking.  this would normally be done in utility.c, but
	 * this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!pg_class_ownercheck(myrelid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(targetrelation));
	if (!allowSystemTableMods && IsSystemRelation(targetrelation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(targetrelation))));

	/*
	 * if the 'recurse' flag is set then we are supposed to rename this
	 * attribute in all classes that inherit from 'relname' (as well as in
	 * 'relname').
	 *
	 * any permissions or problems with duplicate attributes will cause the
	 * whole transaction to abort, which is what we want -- all or nothing.
	 */
	if (recurse)
	{
		ListCell   *child;
		List	   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(myrelid);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);

			if (childrelid == myrelid)
				continue;
			/* note we need not recurse again */
			renameatt(childrelid, oldattname, newattname, false, true);
		}
	}
	else
	{
		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables; else the rename would put them out of step.
		 */
		if (!recursing &&
			find_inheritance_children(myrelid) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("inherited column \"%s\" must be renamed in child tables too",
							oldattname)));
	}

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	atttup = SearchSysCacheCopyAttName(myrelid, oldattname);
	if (!HeapTupleIsValid(atttup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist",
						oldattname)));
	attform = (Form_pg_attribute) GETSTRUCT(atttup);

	attnum = attform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot rename system column \"%s\"",
						oldattname)));

	/*
	 * if the attribute is inherited, forbid the renaming, unless we are
	 * already inside a recursive rename.
	 */
	if (attform->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot rename inherited column \"%s\"",
						oldattname)));

	/* should not already exist */
	/* this test is deliberately not attisdropped-aware */
	if (SearchSysCacheExists(ATTNAME,
							 ObjectIdGetDatum(myrelid),
							 PointerGetDatum(newattname),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
					  newattname, RelationGetRelationName(targetrelation))));

	namestrcpy(&(attform->attname), newattname);

	simple_heap_update(attrelation, &atttup->t_self, atttup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, atttup);

	heap_freetuple(atttup);

	/*
	 * Update column names of indexes that refer to the column being renamed.
	 */
	indexoidlist = RelationGetIndexList(targetrelation);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indextup;
		Form_pg_index indexform;
		int			i;

		/*
		 * Scan through index columns to see if there's any simple index
		 * entries for this attribute.	We ignore expressional entries.
		 */
		indextup = SearchSysCache(INDEXRELID,
								  ObjectIdGetDatum(indexoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(indextup))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexform = (Form_pg_index) GETSTRUCT(indextup);

		for (i = 0; i < indexform->indnatts; i++)
		{
			if (attnum != indexform->indkey.values[i])
				continue;

			/*
			 * Found one, rename it.
			 */
			atttup = SearchSysCacheCopy(ATTNUM,
										ObjectIdGetDatum(indexoid),
										Int16GetDatum(i + 1),
										0, 0);
			if (!HeapTupleIsValid(atttup))
				continue;		/* should we raise an error? */

			/*
			 * Update the (copied) attribute tuple.
			 */
			namestrcpy(&(((Form_pg_attribute) GETSTRUCT(atttup))->attname),
					   newattname);

			simple_heap_update(attrelation, &atttup->t_self, atttup);

			/* keep system catalog indexes current */
			CatalogUpdateIndexes(attrelation, atttup);

			heap_freetuple(atttup);
		}

		ReleaseSysCache(indextup);
	}

	list_free(indexoidlist);

	heap_close(attrelation, RowExclusiveLock);

	relation_close(targetrelation, NoLock);		/* close rel but keep lock */
}

/*
 *		renamerel		- change the name of a relation
 *
 *		XXX - When renaming sequences, we don't bother to modify the
 *			  sequence name that is stored within the sequence itself
 *			  (this would cause problems with MVCC). In the future,
 *			  the sequence name should probably be removed from the
 *			  sequence, AFAIK there's no need for it to be there.
 */
void
renamerel(Oid myrelid, const char *newrelname, ObjectType reltype)
{
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			namespaceId;
	char	   *oldrelname;
	char		relkind;
	bool		relhastriggers;

	/*
	 * Grab an exclusive lock on the target table, index, sequence or view,
	 * which we will NOT release until end of transaction.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);

	oldrelname = pstrdup(RelationGetRelationName(targetrelation));
	namespaceId = RelationGetNamespace(targetrelation);

	if (!allowSystemTableMods && IsSystemRelation(targetrelation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(targetrelation))));

	/*
	 * For compatibility with prior releases, we don't complain if ALTER TABLE
	 * or ALTER INDEX is used to rename a sequence or view.
	 */
	relkind = targetrelation->rd_rel->relkind;
	if (reltype == OBJECT_SEQUENCE && relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						RelationGetRelationName(targetrelation))));

	if (reltype == OBJECT_VIEW && relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a view",
						RelationGetRelationName(targetrelation))));

	relhastriggers = (targetrelation->rd_rel->reltriggers > 0);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in use.
	 */
	relrelation = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(myrelid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						newrelname)));

	/*
	 * Update pg_class tuple with new relname.	(Scribbling on reltup is OK
	 * because it's a copy...)
	 */
	namestrcpy(&(relform->relname), newrelname);

	simple_heap_update(relrelation, &reltup->t_self, reltup);

	/* keep the system catalog indexes current */
	CatalogUpdateIndexes(relrelation, reltup);

	heap_freetuple(reltup);
	heap_close(relrelation, RowExclusiveLock);

	/*
	 * Also rename the associated type, if any.
	 */
	if (OidIsValid(targetrelation->rd_rel->reltype))
		TypeRename(targetrelation->rd_rel->reltype, newrelname, namespaceId);

	/*
	 * Also rename the associated constraint, if any.
	 */
	if (relkind == RELKIND_INDEX)
	{
		Oid			constraintId = get_index_constraint(myrelid);

		if (OidIsValid(constraintId))
			RenameConstraintById(constraintId, newrelname);
	}

	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrelation, NoLock);
}

/*
 * Disallow ALTER TABLE (and similar commands) when the current backend has
 * any open reference to the target table besides the one just acquired by
 * the calling command; this implies there's an open cursor or active plan.
 * We need this check because our AccessExclusiveLock doesn't protect us
 * against stomping on our own foot, only other people's feet!
 *
 * For ALTER TABLE, the only case known to cause serious trouble is ALTER
 * COLUMN TYPE, and some changes are obviously pretty benign, so this could
 * possibly be relaxed to only error out for certain types of alterations.
 * But the use-case for allowing any of these things is not obvious, so we
 * won't work hard at it for now.
 *
 * We also reject these commands if there are any pending AFTER trigger events
 * for the rel.  This is certainly necessary for the rewriting variants of
 * ALTER TABLE, because they don't preserve tuple TIDs and so the pending
 * events would try to fetch the wrong tuples.  It might be overly cautious
 * in other cases, but again it seems better to err on the side of paranoia.
 *
 * REINDEX calls this with "rel" referencing the index to be rebuilt; here
 * we are worried about active indexscans on the index.  The trigger-event
 * check can be skipped, since we are doing no damage to the parent table.
 *
 * The statement name (eg, "ALTER TABLE") is passed for use in error messages.
 */
void
CheckTableNotInUse(Relation rel, const char *stmt)
{
	int			expected_refcnt;

	expected_refcnt = rel->rd_isnailed ? 2 : 1;
	if (rel->rd_refcnt != expected_refcnt)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 /* translator: first %s is a SQL command, eg ALTER TABLE */
				 errmsg("cannot %s \"%s\" because "
						"it is being used by active queries in this session",
						stmt, RelationGetRelationName(rel))));

	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		AfterTriggerPendingOnRel(RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 /* translator: first %s is a SQL command, eg ALTER TABLE */
				 errmsg("cannot %s \"%s\" because "
						"it has pending trigger events",
						stmt, RelationGetRelationName(rel))));
}

/*
 * AlterTable
 *		Execute ALTER TABLE, which can be a list of subcommands
 *
 * ALTER TABLE is performed in three phases:
 *		1. Examine subcommands and perform pre-transformation checking.
 *		2. Update system catalogs.
 *		3. Scan table(s) to check new constraints, and optionally recopy
 *		   the data into new table(s).
 * Phase 3 is not performed unless one or more of the subcommands requires
 * it.	The intention of this design is to allow multiple independent
 * updates of the table schema to be performed with only one pass over the
 * data.
 *
 * ATPrepCmd performs phase 1.	A "work queue" entry is created for
 * each table to be affected (there may be multiple affected tables if the
 * commands traverse a table inheritance hierarchy).  Also we do preliminary
 * validation of the subcommands, including parse transformation of those
 * expressions that need to be evaluated with respect to the old table
 * schema.
 *
 * ATRewriteCatalogs performs phase 2 for each affected table (note that
 * phases 2 and 3 do no explicit recursion, since phase 1 already did it).
 * Certain subcommands need to be performed before others to avoid
 * unnecessary conflicts; for example, DROP COLUMN should come before
 * ADD COLUMN.	Therefore phase 1 divides the subcommands into multiple
 * lists, one for each logical "pass" of phase 2.
 *
 * ATRewriteTables performs phase 3 for those tables that need it.
 *
 * Thanks to the magic of MVCC, an error anywhere along the way rolls back
 * the whole operation; we don't have to do anything special to clean up.
 */
void
AlterTable(AlterTableStmt *stmt)
{
	Relation	rel = relation_openrv(stmt->relation, AccessExclusiveLock);

	CheckTableNotInUse(rel, "ALTER TABLE");

	ATController(rel, stmt->cmds, interpretInhOption(stmt->relation->inhOpt));
}

/*
 * AlterTableInternal
 *
 * ALTER TABLE with target specified by OID
 *
 * We do not reject if the relation is already open, because it's quite
 * likely that one or more layers of caller have it open.  That means it
 * is unsafe to use this entry point for alterations that could break
 * existing query plans.  On the assumption it's not used for such, we
 * don't have to reject pending AFTER triggers, either.
 */
void
AlterTableInternal(Oid relid, List *cmds, bool recurse)
{
	Relation	rel = relation_open(relid, AccessExclusiveLock);

	ATController(rel, cmds, recurse);
}

static void
ATController(Relation rel, List *cmds, bool recurse)
{
	List	   *wqueue = NIL;
	ListCell   *lcmd;

	/* Phase 1: preliminary examination of commands, create work queue */
	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		ATPrepCmd(&wqueue, rel, cmd, recurse, false);
	}

	/* Close the relation, but keep lock until commit */
	relation_close(rel, NoLock);

	/* Phase 2: update system catalogs */
	ATRewriteCatalogs(&wqueue);

	/* Phase 3: scan/rewrite tables as needed */
	ATRewriteTables(&wqueue);
}

/*
 * ATPrepCmd
 *
 * Traffic cop for ALTER TABLE Phase 1 operations, including simple
 * recursion and permission checks.
 *
 * Caller must have acquired AccessExclusiveLock on relation already.
 * This lock should be held until commit.
 */
static void
ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing)
{
	AlteredTableInfo *tab;
	int			pass;

	/* Find or create work queue entry for this table */
	tab = ATGetQueueEntry(wqueue, rel);

	/*
	 * Copy the original subcommand for each table.  This avoids conflicts
	 * when different child tables need to make different parse
	 * transformations (for example, the same column may have different column
	 * numbers in different children).
	 */
	cmd = copyObject(cmd);

	/*
	 * Do permissions checking, recursion to child tables if needed, and any
	 * additional phase-1 processing needed.
	 */
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
			ATSimplePermissions(rel, false);
			/* Performs own recursion */
			ATPrepAddColumn(wqueue, rel, recurse, cmd);
			pass = AT_PASS_ADD_COL;
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */

			/*
			 * We allow defaults on views so that INSERT into a view can have
			 * default-ish behavior.  This works because the rewriter
			 * substitutes default values into INSERTs before it expands
			 * rules.
			 */
			ATSimplePermissions(rel, true);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = cmd->def ? AT_PASS_ADD_CONSTR : AT_PASS_DROP;
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_DROP;
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_SetStatistics:	/* ALTER COLUMN STATISTICS */
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* Performs own permission checks */
			ATPrepSetStatistics(rel, cmd->name, cmd->def);
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_SetStorage:		/* ALTER COLUMN STORAGE */
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			ATSimplePermissions(rel, false);
			/* Recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_DropColumnRecurse;
			pass = AT_PASS_DROP;
			break;
		case AT_AddIndex:		/* ADD INDEX */
			ATSimplePermissions(rel, false);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_INDEX;
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			ATSimplePermissions(rel, false);

			/*
			 * Currently we recurse only for CHECK constraints, never for
			 * foreign-key constraints.  UNIQUE/PKEY constraints won't be seen
			 * here.
			 */
			if (IsA(cmd->def, Constraint))
				ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_DropConstraint:	/* DROP CONSTRAINT */
			ATSimplePermissions(rel, false);
			/* Performs own recursion */
			ATPrepDropConstraint(wqueue, rel, recurse, cmd);
			pass = AT_PASS_DROP;
			break;
		case AT_DropConstraintQuietly:	/* DROP CONSTRAINT for child */
			ATSimplePermissions(rel, false);
			ATSimpleRecursion(wqueue, rel, cmd, recurse);
			/* No command-specific prep needed */
			pass = AT_PASS_DROP;
			break;
		case AT_AlterColumnType:		/* ALTER COLUMN TYPE */
			ATSimplePermissions(rel, false);
			/* Performs own recursion */
			ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd);
			pass = AT_PASS_ALTER_TYPE;
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATSimplePermissions(rel, false);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
			ATSimplePermissions(rel, false);
			/* Performs own recursion */
			if (rel->rd_rel->relhasoids)
			{
				AlterTableCmd *dropCmd = makeNode(AlterTableCmd);

				dropCmd->subtype = AT_DropColumn;
				dropCmd->name = pstrdup("oid");
				dropCmd->behavior = cmd->behavior;
				ATPrepCmd(wqueue, rel, dropCmd, recurse, false);
			}
			pass = AT_PASS_DROP;
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */
			ATSimplePermissionsRelationOrIndex(rel);
			/* This command never recurses */
			ATPrepSetTableSpace(tab, rel, cmd->name);
			pass = AT_PASS_MISC;	/* doesn't actually matter */
			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:		/* RESET (...) */
			ATSimplePermissionsRelationOrIndex(rel);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER variants */
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* DISABLE TRIGGER variants */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
		case AT_EnableRule:		/* ENABLE/DISABLE RULE variants */
		case AT_EnableAlwaysRule:
		case AT_EnableReplicaRule:
		case AT_DisableRule:
		case AT_AddInherit:		/* INHERIT / NO INHERIT */
		case AT_DropInherit:
			ATSimplePermissions(rel, false);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			pass = 0;			/* keep compiler quiet */
			break;
	}

	/* Add the subcommand to the appropriate list for phase 2 */
	tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}

/*
 * ATRewriteCatalogs
 *
 * Traffic cop for ALTER TABLE Phase 2 operations.	Subcommands are
 * dispatched in a "safe" execution order (designed to avoid unnecessary
 * conflicts).
 */
static void
ATRewriteCatalogs(List **wqueue)
{
	int			pass;
	ListCell   *ltab;

	/*
	 * We process all the tables "in parallel", one pass at a time.  This is
	 * needed because we may have to propagate work from one table to another
	 * (specifically, ALTER TYPE on a foreign key's PK has to dispatch the
	 * re-adding of the foreign key constraint to the other table).  Work can
	 * only be propagated into later passes, however.
	 */
	for (pass = 0; pass < AT_NUM_PASSES; pass++)
	{
		/* Go through each table that needs to be processed */
		foreach(ltab, *wqueue)
		{
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
			List	   *subcmds = tab->subcmds[pass];
			Relation	rel;
			ListCell   *lcmd;

			if (subcmds == NIL)
				continue;

			/*
			 * Exclusive lock was obtained by phase 1, needn't get it again
			 */
			rel = relation_open(tab->relid, NoLock);

			foreach(lcmd, subcmds)
				ATExecCmd(tab, rel, (AlterTableCmd *) lfirst(lcmd));

			/*
			 * After the ALTER TYPE pass, do cleanup work (this is not done in
			 * ATExecAlterColumnType since it should be done only once if
			 * multiple columns of a table are altered).
			 */
			if (pass == AT_PASS_ALTER_TYPE)
				ATPostAlterTypeCleanup(wqueue, tab);

			relation_close(rel, NoLock);
		}
	}

	/*
	 * Check to see if a toast table must be added, if we executed any
	 * subcommands that might have added a column or changed column storage.
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		if (tab->relkind == RELKIND_RELATION &&
			(tab->subcmds[AT_PASS_ADD_COL] ||
			 tab->subcmds[AT_PASS_ALTER_TYPE] ||
			 tab->subcmds[AT_PASS_COL_ATTRS]))
			AlterTableCreateToastTable(tab->relid);
	}
}

/*
 * ATExecCmd: dispatch a subcommand to appropriate execution routine
 */
static void
ATExecCmd(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd)
{
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
			ATExecAddColumn(tab, rel, (ColumnDef *) cmd->def);
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */
			ATExecColumnDefault(rel, cmd->name, cmd->def);
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			ATExecDropNotNull(rel, cmd->name);
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			ATExecSetNotNull(tab, rel, cmd->name);
			break;
		case AT_SetStatistics:	/* ALTER COLUMN STATISTICS */
			ATExecSetStatistics(rel, cmd->name, cmd->def);
			break;
		case AT_SetStorage:		/* ALTER COLUMN STORAGE */
			ATExecSetStorage(rel, cmd->name, cmd->def);
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			ATExecDropColumn(rel, cmd->name, cmd->behavior, false, false);
			break;
		case AT_DropColumnRecurse:		/* DROP COLUMN with recursion */
			ATExecDropColumn(rel, cmd->name, cmd->behavior, true, false);
			break;
		case AT_AddIndex:		/* ADD INDEX */
			ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, false);
			break;
		case AT_ReAddIndex:		/* ADD INDEX */
			ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, true);
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			ATExecAddConstraint(tab, rel, cmd->def);
			break;
		case AT_DropConstraint:	/* DROP CONSTRAINT */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior, false);
			break;
		case AT_DropConstraintQuietly:	/* DROP CONSTRAINT for child */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior, true);
			break;
		case AT_AlterColumnType:		/* ALTER COLUMN TYPE */
			ATExecAlterColumnType(tab, rel, cmd->name, (TypeName *) cmd->def);
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			ATExecChangeOwner(RelationGetRelid(rel),
							  get_roleid_checked(cmd->name),
							  false);
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
			ATExecClusterOn(rel, cmd->name);
			break;
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATExecDropCluster(rel);
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */

			/*
			 * Nothing to do here; we'll have generated a DropColumn
			 * subcommand to do the real work
			 */
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */

			/*
			 * Nothing to do here; Phase 3 does the work
			 */
			break;
		case AT_SetRelOptions:	/* SET (...) */
			ATExecSetRelOptions(rel, (List *) cmd->def, false);
			break;
		case AT_ResetRelOptions:		/* RESET (...) */
			ATExecSetRelOptions(rel, (List *) cmd->def, true);
			break;

		case AT_EnableTrig:		/* ENABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_ORIGIN, false);
			break;
		case AT_EnableAlwaysTrig:		/* ENABLE ALWAYS TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ALWAYS, false);
			break;
		case AT_EnableReplicaTrig:		/* ENABLE REPLICA TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_REPLICA, false);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_DISABLED, false);
			break;
		case AT_EnableTrigAll:	/* ENABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, false);
			break;
		case AT_DisableTrigAll:	/* DISABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, false);
			break;
		case AT_EnableTrigUser:	/* ENABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, true);
			break;
		case AT_DisableTrigUser:		/* DISABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, true);
			break;

		case AT_EnableRule:		/* ENABLE RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_ORIGIN);
			break;
		case AT_EnableAlwaysRule:		/* ENABLE ALWAYS RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ALWAYS);
			break;
		case AT_EnableReplicaRule:		/* ENABLE REPLICA RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_REPLICA);
			break;
		case AT_DisableRule:	/* DISABLE RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_DISABLED);
			break;

		case AT_AddInherit:
			ATExecAddInherit(rel, (RangeVar *) cmd->def);
			break;
		case AT_DropInherit:
			ATExecDropInherit(rel, (RangeVar *) cmd->def);
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			break;
	}

	/*
	 * Bump the command counter to ensure the next subcommand in the sequence
	 * can see the changes so far
	 */
	CommandCounterIncrement();
}

/*
 * ATRewriteTables: ALTER TABLE phase 3
 */
static void
ATRewriteTables(List **wqueue)
{
	ListCell   *ltab;

	/* Go through each table that needs to be checked or rewritten */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/*
		 * We only need to rewrite the table if at least one column needs to
		 * be recomputed.
		 */
		if (tab->newvals != NIL)
		{
			/* Build a temporary relation and copy data */
			Oid			OIDNewHeap;
			char		NewHeapName[NAMEDATALEN];
			Oid			NewTableSpace;
			Relation	OldHeap;
			ObjectAddress object;

			OldHeap = heap_open(tab->relid, NoLock);

			/*
			 * We can never allow rewriting of shared or nailed-in-cache
			 * relations, because we can't support changing their relfilenode
			 * values.
			 */
			if (OldHeap->rd_rel->relisshared || OldHeap->rd_isnailed)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite system relation \"%s\"",
								RelationGetRelationName(OldHeap))));

			/*
			 * Don't allow rewrite on temp tables of other backends ... their
			 * local buffer manager is not going to cope.
			 */
			if (isOtherTempNamespace(RelationGetNamespace(OldHeap)))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot rewrite temporary tables of other sessions")));

			/*
			 * Select destination tablespace (same as original unless user
			 * requested a change)
			 */
			if (tab->newTableSpace)
				NewTableSpace = tab->newTableSpace;
			else
				NewTableSpace = OldHeap->rd_rel->reltablespace;

			heap_close(OldHeap, NoLock);

			/*
			 * Create the new heap, using a temporary name in the same
			 * namespace as the existing table.  NOTE: there is some risk of
			 * collision with user relnames.  Working around this seems more
			 * trouble than it's worth; in particular, we can't create the new
			 * heap in a different namespace from the old, or we will have
			 * problems with the TEMP status of temp tables.
			 */
			snprintf(NewHeapName, sizeof(NewHeapName),
					 "pg_temp_%u", tab->relid);

			OIDNewHeap = make_new_heap(tab->relid, NewHeapName, NewTableSpace);

			/*
			 * Copy the heap data into the new table with the desired
			 * modifications, and test the current data within the table
			 * against new constraints generated by ALTER TABLE commands.
			 */
			ATRewriteTable(tab, OIDNewHeap);

			/*
			 * Swap the physical files of the old and new heaps.  Since we are
			 * generating a new heap, we can use RecentXmin for the table's
			 * new relfrozenxid because we rewrote all the tuples on
			 * ATRewriteTable, so no older Xid remains on the table.
			 */
			swap_relation_files(tab->relid, OIDNewHeap, RecentXmin);

			CommandCounterIncrement();

			/* Destroy new heap with old filenode */
			object.classId = RelationRelationId;
			object.objectId = OIDNewHeap;
			object.objectSubId = 0;

			/*
			 * The new relation is local to our transaction and we know
			 * nothing depends on it, so DROP_RESTRICT should be OK.
			 */
			performDeletion(&object, DROP_RESTRICT);
			/* performDeletion does CommandCounterIncrement at end */

			/*
			 * Rebuild each index on the relation (but not the toast table,
			 * which is all-new anyway).  We do not need
			 * CommandCounterIncrement() because reindex_relation does it.
			 */
			reindex_relation(tab->relid, false);
		}
		else
		{
			/*
			 * Test the current data within the table against new constraints
			 * generated by ALTER TABLE commands, but don't rebuild data.
			 */
			if (tab->constraints != NIL || tab->new_notnull)
				ATRewriteTable(tab, InvalidOid);

			/*
			 * If we had SET TABLESPACE but no reason to reconstruct tuples,
			 * just do a block-by-block copy.
			 */
			if (tab->newTableSpace)
				ATExecSetTableSpace(tab->relid, tab->newTableSpace);
		}
	}

	/*
	 * Foreign key constraints are checked in a final pass, since (a) it's
	 * generally best to examine each one separately, and (b) it's at least
	 * theoretically possible that we have changed both relations of the
	 * foreign key, and we'd better have finished both rewrites before we try
	 * to read the tables.
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		Relation	rel = NULL;
		ListCell   *lcon;

		foreach(lcon, tab->constraints)
		{
			NewConstraint *con = lfirst(lcon);

			if (con->contype == CONSTR_FOREIGN)
			{
				FkConstraint *fkconstraint = (FkConstraint *) con->qual;
				Relation	refrel;

				if (rel == NULL)
				{
					/* Long since locked, no need for another */
					rel = heap_open(tab->relid, NoLock);
				}

				refrel = heap_open(con->refrelid, RowShareLock);

				validateForeignKeyConstraint(fkconstraint, rel, refrel,
											 con->conid);

				heap_close(refrel, NoLock);
			}
		}

		if (rel)
			heap_close(rel, NoLock);
	}
}

/*
 * ATRewriteTable: scan or rewrite one table
 *
 * OIDNewHeap is InvalidOid if we don't need to rewrite
 */
static void
ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap)
{
	Relation	oldrel;
	Relation	newrel;
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	bool		needscan = false;
	List	   *notnull_attrs;
	int			i;
	ListCell   *l;
	EState	   *estate;

	/*
	 * Open the relation(s).  We have surely already locked the existing
	 * table.
	 */
	oldrel = heap_open(tab->relid, NoLock);
	oldTupDesc = tab->oldDesc;
	newTupDesc = RelationGetDescr(oldrel);		/* includes all mods */

	if (OidIsValid(OIDNewHeap))
		newrel = heap_open(OIDNewHeap, AccessExclusiveLock);
	else
		newrel = NULL;

	/*
	 * If we need to rewrite the table, the operation has to be propagated to
	 * tables that use this table's rowtype as a column type.
	 *
	 * (Eventually this will probably become true for scans as well, but at
	 * the moment a composite type does not enforce any constraints, so it's
	 * not necessary/appropriate to enforce them just during ALTER.)
	 */
	if (newrel)
		find_composite_type_dependencies(oldrel->rd_rel->reltype,
										 RelationGetRelationName(oldrel),
										 NULL);

	/*
	 * Generate the constraint and default execution states
	 */

	estate = CreateExecutorState();

	/* Build the needed expression execution states */
	foreach(l, tab->constraints)
	{
		NewConstraint *con = lfirst(l);

		switch (con->contype)
		{
			case CONSTR_CHECK:
				needscan = true;
				con->qualstate = (List *)
					ExecPrepareExpr((Expr *) con->qual, estate);
				break;
			case CONSTR_FOREIGN:
				/* Nothing to do here */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	foreach(l, tab->newvals)
	{
		NewColumnValue *ex = lfirst(l);

		needscan = true;

		ex->exprstate = ExecPrepareExpr((Expr *) ex->expr, estate);
	}

	notnull_attrs = NIL;
	if (newrel || tab->new_notnull)
	{
		/*
		 * If we are rebuilding the tuples OR if we added any new NOT NULL
		 * constraints, check all not-null constraints.  This is a bit of
		 * overkill but it minimizes risk of bugs, and heap_attisnull is a
		 * pretty cheap test anyway.
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (newTupDesc->attrs[i]->attnotnull &&
				!newTupDesc->attrs[i]->attisdropped)
				notnull_attrs = lappend_int(notnull_attrs, i);
		}
		if (notnull_attrs)
			needscan = true;
	}

	if (needscan)
	{
		ExprContext *econtext;
		Datum	   *values;
		bool	   *isnull;
		TupleTableSlot *oldslot;
		TupleTableSlot *newslot;
		HeapScanDesc scan;
		HeapTuple	tuple;
		MemoryContext oldCxt;
		List	   *dropped_attrs = NIL;
		ListCell   *lc;

		econtext = GetPerTupleExprContext(estate);

		/*
		 * Make tuple slots for old and new tuples.  Note that even when the
		 * tuples are the same, the tupDescs might not be (consider ADD COLUMN
		 * without a default).
		 */
		oldslot = MakeSingleTupleTableSlot(oldTupDesc);
		newslot = MakeSingleTupleTableSlot(newTupDesc);

		/* Preallocate values/isnull arrays */
		i = Max(newTupDesc->natts, oldTupDesc->natts);
		values = (Datum *) palloc(i * sizeof(Datum));
		isnull = (bool *) palloc(i * sizeof(bool));
		memset(values, 0, i * sizeof(Datum));
		memset(isnull, true, i * sizeof(bool));

		/*
		 * Any attributes that are dropped according to the new tuple
		 * descriptor can be set to NULL. We precompute the list of dropped
		 * attributes to avoid needing to do so in the per-tuple loop.
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (newTupDesc->attrs[i]->attisdropped)
				dropped_attrs = lappend_int(dropped_attrs, i);
		}

		/*
		 * Scan through the rows, generating a new row if needed and then
		 * checking all the constraints.
		 */
		scan = heap_beginscan(oldrel, SnapshotNow, 0, NULL);

		/*
		 * Switch to per-tuple memory context and reset it for each tuple
		 * produced, so we don't leak memory.
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			if (newrel)
			{
				Oid			tupOid = InvalidOid;

				/* Extract data from old tuple */
				heap_deform_tuple(tuple, oldTupDesc, values, isnull);
				if (oldTupDesc->tdhasoid)
					tupOid = HeapTupleGetOid(tuple);

				/* Set dropped attributes to null in new tuple */
				foreach(lc, dropped_attrs)
					isnull[lfirst_int(lc)] = true;

				/*
				 * Process supplied expressions to replace selected columns.
				 * Expression inputs come from the old tuple.
				 */
				ExecStoreTuple(tuple, oldslot, InvalidBuffer, false);
				econtext->ecxt_scantuple = oldslot;

				foreach(l, tab->newvals)
				{
					NewColumnValue *ex = lfirst(l);

					values[ex->attnum - 1] = ExecEvalExpr(ex->exprstate,
														  econtext,
													 &isnull[ex->attnum - 1],
														  NULL);
				}

				/*
				 * Form the new tuple. Note that we don't explicitly pfree it,
				 * since the per-tuple memory context will be reset shortly.
				 */
				tuple = heap_form_tuple(newTupDesc, values, isnull);

				/* Preserve OID, if any */
				if (newTupDesc->tdhasoid)
					HeapTupleSetOid(tuple, tupOid);
			}

			/* Now check any constraints on the possibly-changed tuple */
			ExecStoreTuple(tuple, newslot, InvalidBuffer, false);
			econtext->ecxt_scantuple = newslot;

			foreach(l, notnull_attrs)
			{
				int			attn = lfirst_int(l);

				if (heap_attisnull(tuple, attn + 1))
					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("column \"%s\" contains null values",
								NameStr(newTupDesc->attrs[attn]->attname))));
			}

			foreach(l, tab->constraints)
			{
				NewConstraint *con = lfirst(l);

				switch (con->contype)
				{
					case CONSTR_CHECK:
						if (!ExecQual(con->qualstate, econtext, true))
							ereport(ERROR,
									(errcode(ERRCODE_CHECK_VIOLATION),
									 errmsg("check constraint \"%s\" is violated by some row",
											con->name)));
						break;
					case CONSTR_FOREIGN:
						/* Nothing to do here */
						break;
					default:
						elog(ERROR, "unrecognized constraint type: %d",
							 (int) con->contype);
				}
			}

			/* Write the tuple out to the new relation */
			if (newrel)
				simple_heap_insert(newrel, tuple);

			ResetExprContext(econtext);

			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		heap_endscan(scan);

		ExecDropSingleTupleTableSlot(oldslot);
		ExecDropSingleTupleTableSlot(newslot);
	}

	FreeExecutorState(estate);

	heap_close(oldrel, NoLock);
	if (newrel)
		heap_close(newrel, NoLock);
}

/*
 * ATGetQueueEntry: find or create an entry in the ALTER TABLE work queue
 */
static AlteredTableInfo *
ATGetQueueEntry(List **wqueue, Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	AlteredTableInfo *tab;
	ListCell   *ltab;

	foreach(ltab, *wqueue)
	{
		tab = (AlteredTableInfo *) lfirst(ltab);
		if (tab->relid == relid)
			return tab;
	}

	/*
	 * Not there, so add it.  Note that we make a copy of the relation's
	 * existing descriptor before anything interesting can happen to it.
	 */
	tab = (AlteredTableInfo *) palloc0(sizeof(AlteredTableInfo));
	tab->relid = relid;
	tab->relkind = rel->rd_rel->relkind;
	tab->oldDesc = CreateTupleDescCopy(RelationGetDescr(rel));

	*wqueue = lappend(*wqueue, tab);

	return tab;
}

/*
 * ATSimplePermissions
 *
 * - Ensure that it is a relation (or possibly a view)
 * - Ensure this user is the owner
 * - Ensure that it is not a system table
 */
static void
ATSimplePermissions(Relation rel, bool allowView)
{
	if (rel->rd_rel->relkind != RELKIND_RELATION)
	{
		if (allowView)
		{
			if (rel->rd_rel->relkind != RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a table or view",
								RelationGetRelationName(rel))));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table",
							RelationGetRelationName(rel))));
	}

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));
}

/*
 * ATSimplePermissionsRelationOrIndex
 *
 * - Ensure that it is a relation or an index
 * - Ensure this user is the owner
 * - Ensure that it is not a system table
 */
static void
ATSimplePermissionsRelationOrIndex(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or index",
						RelationGetRelationName(rel))));

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));
}

/*
 * ATSimpleRecursion
 *
 * Simple table recursion sufficient for most ALTER TABLE operations.
 * All direct and indirect children are processed in an unspecified order.
 * Note that if a child inherits from the original table via multiple
 * inheritance paths, it will be visited just once.
 */
static void
ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse)
{
	/*
	 * Propagate to children if desired.  Non-table relations never have
	 * children, so no need to search in that case.
	 */
	if (recurse && rel->rd_rel->relkind == RELKIND_RELATION)
	{
		Oid			relid = RelationGetRelid(rel);
		ListCell   *child;
		List	   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(relid);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;

			if (childrelid == relid)
				continue;
			childrel = relation_open(childrelid, AccessExclusiveLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");
			ATPrepCmd(wqueue, childrel, cmd, false, true);
			relation_close(childrel, NoLock);
		}
	}
}

/*
 * ATOneLevelRecursion
 *
 * Here, we visit only direct inheritance children.  It is expected that
 * the command's prep routine will recurse again to find indirect children.
 * When using this technique, a multiply-inheriting child will be visited
 * multiple times.
 */
static void
ATOneLevelRecursion(List **wqueue, Relation rel,
					AlterTableCmd *cmd)
{
	Oid			relid = RelationGetRelid(rel);
	ListCell   *child;
	List	   *children;

	/* this routine is actually in the planner */
	children = find_inheritance_children(relid);

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;

		childrel = relation_open(childrelid, AccessExclusiveLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");
		ATPrepCmd(wqueue, childrel, cmd, true, true);
		relation_close(childrel, NoLock);
	}
}


/*
 * find_composite_type_dependencies
 *
 * Check to see if a composite type is being used as a column in some
 * other table (possibly nested several levels deep in composite types!).
 * Eventually, we'd like to propagate the check or rewrite operation
 * into other such tables, but for now, just error out if we find any.
 *
 * Caller should provide either a table name or a type name (not both) to
 * report in the error message, if any.
 *
 * We assume that functions and views depending on the type are not reasons
 * to reject the ALTER.  (How safe is this really?)
 */
void
find_composite_type_dependencies(Oid typeOid,
								 const char *origTblName,
								 const char *origTypeName)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;
	Oid			arrayOid;

	/*
	 * We scan pg_depend to find those things that depend on the rowtype. (We
	 * assume we can ignore refobjsubid for a rowtype.)
	 */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 SnapshotNow, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		Relation	rel;
		Form_pg_attribute att;

		/* Ignore dependees that aren't user columns of relations */
		/* (we assume system columns are never of rowtypes) */
		if (pg_depend->classid != RelationRelationId ||
			pg_depend->objsubid <= 0)
			continue;

		rel = relation_open(pg_depend->objid, AccessShareLock);
		att = rel->rd_att->attrs[pg_depend->objsubid - 1];

		if (rel->rd_rel->relkind == RELKIND_RELATION)
		{
			if (origTblName)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter table \"%s\" because column \"%s\".\"%s\" uses its rowtype",
								origTblName,
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s\".\"%s\" uses it",
								origTypeName,
								RelationGetRelationName(rel),
								NameStr(att->attname))));
		}
		else if (OidIsValid(rel->rd_rel->reltype))
		{
			/*
			 * A view or composite type itself isn't a problem, but we must
			 * recursively check for indirect dependencies via its rowtype.
			 */
			find_composite_type_dependencies(rel->rd_rel->reltype,
											 origTblName, origTypeName);
		}

		relation_close(rel, AccessShareLock);
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	/*
	 * If there's an array type for the rowtype, must check for uses of it,
	 * too.
	 */
	arrayOid = get_array_type(typeOid);
	if (OidIsValid(arrayOid))
		find_composite_type_dependencies(arrayOid, origTblName, origTypeName);
}


/*
 * ALTER TABLE ADD COLUMN
 *
 * Adds an additional attribute to a relation making the assumption that
 * CHECK, NOT NULL, and FOREIGN KEY constraints will be removed from the
 * AT_AddColumn AlterTableCmd by parse_utilcmd.c and added as independent
 * AlterTableCmd's.
 */
static void
ATPrepAddColumn(List **wqueue, Relation rel, bool recurse,
				AlterTableCmd *cmd)
{
	/*
	 * Recurse to add the column to child classes, if requested.
	 *
	 * We must recurse one level at a time, so that multiply-inheriting
	 * children are visited the right number of times and end up with the
	 * right attinhcount.
	 */
	if (recurse)
	{
		AlterTableCmd *childCmd = copyObject(cmd);
		ColumnDef  *colDefChild = (ColumnDef *) childCmd->def;

		/* Child should see column as singly inherited */
		colDefChild->inhcount = 1;
		colDefChild->is_local = false;

		ATOneLevelRecursion(wqueue, rel, childCmd);
	}
	else
	{
		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables; else the addition would put them out of step.
		 */
		if (find_inheritance_children(RelationGetRelid(rel)) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column must be added to child tables too")));
	}
}

static void
ATExecAddColumn(AlteredTableInfo *tab, Relation rel,
				ColumnDef *colDef)
{
	Oid			myrelid = RelationGetRelid(rel);
	Relation	pgclass,
				attrdesc;
	HeapTuple	reltup;
	HeapTuple	attributeTuple;
	Form_pg_attribute attribute;
	FormData_pg_attribute attributeD;
	int			i;
	int			minattnum,
				maxatts;
	HeapTuple	typeTuple;
	Oid			typeOid;
	int32		typmod;
	Form_pg_type tform;
	Expr	   *defval;

	attrdesc = heap_open(AttributeRelationId, RowExclusiveLock);

	/*
	 * Are we adding the column to a recursion child?  If so, check whether to
	 * merge with an existing definition for the column.
	 */
	if (colDef->inhcount > 0)
	{
		HeapTuple	tuple;

		/* Does child already have a column by this name? */
		tuple = SearchSysCacheCopyAttName(myrelid, colDef->colname);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);
			Oid			ctypeId;
			int32		ctypmod;

			/* Okay if child matches by type */
			ctypeId = typenameTypeId(NULL, colDef->typename, &ctypmod);
			if (ctypeId != childatt->atttypid ||
				ctypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
							RelationGetRelationName(rel), colDef->colname)));

			/* Bump the existing child att's inhcount */
			childatt->attinhcount++;
			simple_heap_update(attrdesc, &tuple->t_self, tuple);
			CatalogUpdateIndexes(attrdesc, tuple);

			heap_freetuple(tuple);

			/* Inform the user about the merge */
			ereport(NOTICE,
			  (errmsg("merging definition of column \"%s\" for child \"%s\"",
					  colDef->colname, RelationGetRelationName(rel))));

			heap_close(attrdesc, RowExclusiveLock);
			return;
		}
	}

	pgclass = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(myrelid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", myrelid);

	/*
	 * this test is deliberately not attisdropped-aware, since if one tries to
	 * add a column matching a dropped column name, it's gonna fail anyway.
	 */
	if (SearchSysCacheExists(ATTNAME,
							 ObjectIdGetDatum(myrelid),
							 PointerGetDatum(colDef->colname),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
						colDef->colname, RelationGetRelationName(rel))));

	minattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
	maxatts = minattnum + 1;
	if (maxatts > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));
	i = minattnum + 1;

	typeTuple = typenameType(NULL, colDef->typename, &typmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	typeOid = HeapTupleGetOid(typeTuple);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colDef->colname, typeOid);

	attributeTuple = heap_addheader(Natts_pg_attribute,
									false,
									ATTRIBUTE_TUPLE_SIZE,
									(void *) &attributeD);

	attribute = (Form_pg_attribute) GETSTRUCT(attributeTuple);

	attribute->attrelid = myrelid;
	namestrcpy(&(attribute->attname), colDef->colname);
	attribute->atttypid = typeOid;
	attribute->attstattarget = -1;
	attribute->attlen = tform->typlen;
	attribute->attcacheoff = -1;
	attribute->atttypmod = typmod;
	attribute->attnum = i;
	attribute->attbyval = tform->typbyval;
	attribute->attndims = list_length(colDef->typename->arrayBounds);
	attribute->attstorage = tform->typstorage;
	attribute->attalign = tform->typalign;
	attribute->attnotnull = colDef->is_not_null;
	attribute->atthasdef = false;
	attribute->attisdropped = false;
	attribute->attislocal = colDef->is_local;
	attribute->attinhcount = colDef->inhcount;

	ReleaseSysCache(typeTuple);

	simple_heap_insert(attrdesc, attributeTuple);

	/* Update indexes on pg_attribute */
	CatalogUpdateIndexes(attrdesc, attributeTuple);

	heap_close(attrdesc, RowExclusiveLock);

	/*
	 * Update number of attributes in pg_class tuple
	 */
	((Form_pg_class) GETSTRUCT(reltup))->relnatts = maxatts;

	simple_heap_update(pgclass, &reltup->t_self, reltup);

	/* keep catalog indexes current */
	CatalogUpdateIndexes(pgclass, reltup);

	heap_freetuple(reltup);

	heap_close(pgclass, RowExclusiveLock);

	/* Make the attribute's catalog entry visible */
	CommandCounterIncrement();

	/*
	 * Store the DEFAULT, if any, in the catalogs
	 */
	if (colDef->raw_default)
	{
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attribute->attnum;
		rawEnt->raw_default = copyObject(colDef->raw_default);

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationRawConstraints(rel, list_make1(rawEnt), NIL);

		/* Make the additional catalog changes visible */
		CommandCounterIncrement();
	}

	/*
	 * Tell Phase 3 to fill in the default expression, if there is one.
	 *
	 * If there is no default, Phase 3 doesn't have to do anything, because
	 * that effectively means that the default is NULL.  The heap tuple access
	 * routines always check for attnum > # of attributes in tuple, and return
	 * NULL if so, so without any modification of the tuple data we will get
	 * the effect of NULL values in the new column.
	 *
	 * An exception occurs when the new column is of a domain type: the domain
	 * might have a NOT NULL constraint, or a check constraint that indirectly
	 * rejects nulls.  If there are any domain constraints then we construct
	 * an explicit NULL default value that will be passed through
	 * CoerceToDomain processing.  (This is a tad inefficient, since it causes
	 * rewriting the table which we really don't have to do, but the present
	 * design of domain processing doesn't offer any simple way of checking
	 * the constraints more directly.)
	 *
	 * Note: we use build_column_default, and not just the cooked default
	 * returned by AddRelationRawConstraints, so that the right thing happens
	 * when a datatype's default applies.
	 */
	defval = (Expr *) build_column_default(rel, attribute->attnum);

	if (!defval && GetDomainConstraints(typeOid) != NIL)
	{
		Oid			baseTypeId;
		int32		baseTypeMod;

		baseTypeMod = typmod;
		baseTypeId = getBaseTypeAndTypmod(typeOid, &baseTypeMod);
		defval = (Expr *) makeNullConst(baseTypeId, baseTypeMod);
		defval = (Expr *) coerce_to_target_type(NULL,
												(Node *) defval,
												baseTypeId,
												typeOid,
												typmod,
												COERCION_ASSIGNMENT,
												COERCE_IMPLICIT_CAST);
		if (defval == NULL)		/* should not happen */
			elog(ERROR, "failed to coerce base type to domain");
	}

	if (defval)
	{
		NewColumnValue *newval;

		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attribute->attnum;
		newval->expr = defval;

		tab->newvals = lappend(tab->newvals, newval);
	}

	/*
	 * Add needed dependency entries for the new column.
	 */
	add_column_datatype_dependency(myrelid, i, attribute->atttypid);
}

/*
 * Install a column's dependency on its datatype.
 */
static void
add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid)
{
	ObjectAddress myself,
				referenced;

	myself.classId = RelationRelationId;
	myself.objectId = relid;
	myself.objectSubId = attnum;
	referenced.classId = TypeRelationId;
	referenced.objectId = typid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 */
static void
ATExecDropNotNull(Relation rel, const char *colName)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * lookup the attribute
	 */
	attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Check that the attribute is not in a primary key
	 */

	/* Loop over all indexes on the relation */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;
		int			i;

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/* If the index is not a primary key, skip the check */
		if (indexStruct->indisprimary)
		{
			/*
			 * Loop over each attribute in the primary key and see if it
			 * matches the to-be-altered attribute
			 */
			for (i = 0; i < indexStruct->indnatts; i++)
			{
				if (indexStruct->indkey.values[i] == attnum)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("column \"%s\" is in a primary key",
									colName)));
			}
		}

		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/*
	 * Okay, actually perform the catalog change ... if needed
	 */
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
	{
		((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = FALSE;

		simple_heap_update(attr_rel, &tuple->t_self, tuple);

		/* keep the system catalog indexes current */
		CatalogUpdateIndexes(attr_rel, tuple);
	}

	heap_close(attr_rel, RowExclusiveLock);
}

/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 */
static void
ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;

	/*
	 * lookup the attribute
	 */
	attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Okay, actually perform the catalog change ... if needed
	 */
	if (!((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
	{
		((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = TRUE;

		simple_heap_update(attr_rel, &tuple->t_self, tuple);

		/* keep the system catalog indexes current */
		CatalogUpdateIndexes(attr_rel, tuple);

		/* Tell Phase 3 it needs to test the constraint */
		tab->new_notnull = true;
	}

	heap_close(attr_rel, RowExclusiveLock);
}

/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 */
static void
ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault)
{
	AttrNumber	attnum;

	/*
	 * get the number of the attribute
	 */
	attnum = get_attnum(RelationGetRelid(rel), colName);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Remove any old default for the column.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false);

	if (newDefault)
	{
		/* SET DEFAULT */
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationRawConstraints(rel, list_make1(rawEnt), NIL);
	}
}

/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 */
static void
ATPrepSetStatistics(Relation rel, const char *colName, Node *flagValue)
{
	/*
	 * We do our own permission checking because (a) we want to allow SET
	 * STATISTICS on indexes (for expressional index columns), and (b) we want
	 * to allow SET STATISTICS on system catalogs without requiring
	 * allowSystemTableMods to be turned on.
	 */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or index",
						RelationGetRelationName(rel))));

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));
}

static void
ATExecSetStatistics(Relation rel, const char *colName, Node *newValue)
{
	int			newtarget;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;

	Assert(IsA(newValue, Integer));
	newtarget = intVal(newValue);

	/*
	 * Limit target to a sane range
	 */
	if (newtarget < -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("statistics target %d is too low",
						newtarget)));
	}
	else if (newtarget > 1000)
	{
		newtarget = 1000;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lowering statistics target to %d",
						newtarget)));
	}

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	if (attrtuple->attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	attrtuple->attstattarget = newtarget;

	simple_heap_update(attrelation, &tuple->t_self, tuple);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, tuple);

	heap_freetuple(tuple);

	heap_close(attrelation, RowExclusiveLock);
}

/*
 * ALTER TABLE ALTER COLUMN SET STORAGE
 */
static void
ATExecSetStorage(Relation rel, const char *colName, Node *newValue)
{
	char	   *storagemode;
	char		newstorage;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;

	Assert(IsA(newValue, String));
	storagemode = strVal(newValue);

	if (pg_strcasecmp(storagemode, "plain") == 0)
		newstorage = 'p';
	else if (pg_strcasecmp(storagemode, "external") == 0)
		newstorage = 'e';
	else if (pg_strcasecmp(storagemode, "extended") == 0)
		newstorage = 'x';
	else if (pg_strcasecmp(storagemode, "main") == 0)
		newstorage = 'm';
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid storage type \"%s\"",
						storagemode)));
		newstorage = 0;			/* keep compiler quiet */
	}

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	if (attrtuple->attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * safety check: do not allow toasted storage modes unless column datatype
	 * is TOAST-aware.
	 */
	if (newstorage == 'p' || TypeIsToastable(attrtuple->atttypid))
		attrtuple->attstorage = newstorage;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s can only have storage PLAIN",
						format_type_be(attrtuple->atttypid))));

	simple_heap_update(attrelation, &tuple->t_self, tuple);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, tuple);

	heap_freetuple(tuple);

	heap_close(attrelation, RowExclusiveLock);
}


/*
 * ALTER TABLE DROP COLUMN
 *
 * DROP COLUMN cannot use the normal ALTER TABLE recursion mechanism,
 * because we have to decide at runtime whether to recurse or not depending
 * on whether attinhcount goes to zero or not.	(We can't check this in a
 * static pre-pass because it won't handle multiple inheritance situations
 * correctly.)	Since DROP COLUMN doesn't need to create any work queue
 * entries for Phase 3, it's okay to recurse internally in this routine
 * without considering the work queue.
 */
static void
ATExecDropColumn(Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing)
{
	HeapTuple	tuple;
	Form_pg_attribute targetatt;
	AttrNumber	attnum;
	List	   *children;
	ObjectAddress object;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, false);

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	targetatt = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = targetatt->attnum;

	/* Can't drop a system attribute, except OID */
	if (attnum <= 0 && attnum != ObjectIdAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop system column \"%s\"",
						colName)));

	/* Don't drop inherited columns */
	if (targetatt->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop inherited column \"%s\"",
						colName)));

	ReleaseSysCache(tuple);

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	children = find_inheritance_children(RelationGetRelid(rel));

	if (children)
	{
		Relation	attr_rel;
		ListCell   *child;

		attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;
			Form_pg_attribute childatt;

			childrel = heap_open(childrelid, AccessExclusiveLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");

			tuple = SearchSysCacheCopyAttName(childrelid, colName);
			if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
				elog(ERROR, "cache lookup failed for attribute \"%s\" of relation %u",
					 colName, childrelid);
			childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (childatt->attinhcount <= 0)		/* shouldn't happen */
				elog(ERROR, "relation %u has non-inherited attribute \"%s\"",
					 childrelid, colName);

			if (recurse)
			{
				/*
				 * If the child column has other definition sources, just
				 * decrement its inheritance count; if not, recurse to delete
				 * it.
				 */
				if (childatt->attinhcount == 1 && !childatt->attislocal)
				{
					/* Time to delete this child column, too */
					ATExecDropColumn(childrel, colName, behavior, true, true);
				}
				else
				{
					/* Child column must survive my deletion */
					childatt->attinhcount--;

					simple_heap_update(attr_rel, &tuple->t_self, tuple);

					/* keep the system catalog indexes current */
					CatalogUpdateIndexes(attr_rel, tuple);

					/* Make update visible */
					CommandCounterIncrement();
				}
			}
			else
			{
				/*
				 * If we were told to drop ONLY in this table (no recursion),
				 * we need to mark the inheritors' attribute as locally
				 * defined rather than inherited.
				 */
				childatt->attinhcount--;
				childatt->attislocal = true;

				simple_heap_update(attr_rel, &tuple->t_self, tuple);

				/* keep the system catalog indexes current */
				CatalogUpdateIndexes(attr_rel, tuple);

				/* Make update visible */
				CommandCounterIncrement();
			}

			heap_freetuple(tuple);

			heap_close(childrel, NoLock);
		}
		heap_close(attr_rel, RowExclusiveLock);
	}

	/*
	 * Perform the actual column deletion
	 */
	object.classId = RelationRelationId;
	object.objectId = RelationGetRelid(rel);
	object.objectSubId = attnum;

	performDeletion(&object, behavior);

	/*
	 * If we dropped the OID column, must adjust pg_class.relhasoids
	 */
	if (attnum == ObjectIdAttributeNumber)
	{
		Relation	class_rel;
		Form_pg_class tuple_class;

		class_rel = heap_open(RelationRelationId, RowExclusiveLock);

		tuple = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(RelationGetRelid(rel)),
								   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u",
				 RelationGetRelid(rel));
		tuple_class = (Form_pg_class) GETSTRUCT(tuple);

		tuple_class->relhasoids = false;
		simple_heap_update(class_rel, &tuple->t_self, tuple);

		/* Keep the catalog indexes up to date */
		CatalogUpdateIndexes(class_rel, tuple);

		heap_close(class_rel, RowExclusiveLock);
	}
}

/*
 * ALTER TABLE ADD INDEX
 *
 * There is no such command in the grammar, but parse_utilcmd.c converts
 * UNIQUE and PRIMARY KEY constraints into AT_AddIndex subcommands.  This lets
 * us schedule creation of the index at the appropriate time during ALTER.
 */
static void
ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild)
{
	bool		check_rights;
	bool		skip_build;
	bool		quiet;

	Assert(IsA(stmt, IndexStmt));

	/* suppress schema rights check when rebuilding existing index */
	check_rights = !is_rebuild;
	/* skip index build if phase 3 will have to rewrite table anyway */
	skip_build = (tab->newvals != NIL);
	/* suppress notices when rebuilding existing index */
	quiet = is_rebuild;

	/* The IndexStmt has already been through transformIndexStmt */

	DefineIndex(stmt->relation, /* relation */
				stmt->idxname,	/* index name */
				InvalidOid,		/* no predefined OID */
				stmt->accessMethod,		/* am name */
				stmt->tableSpace,
				stmt->indexParams,		/* parameters */
				(Expr *) stmt->whereClause,
				stmt->options,
				stmt->unique,
				stmt->primary,
				stmt->isconstraint,
				true,			/* is_alter_table */
				check_rights,
				skip_build,
				quiet,
				false);
}

/*
 * ALTER TABLE ADD CONSTRAINT
 */
static void
ATExecAddConstraint(AlteredTableInfo *tab, Relation rel, Node *newConstraint)
{
	switch (nodeTag(newConstraint))
	{
		case T_Constraint:
			{
				Constraint *constr = (Constraint *) newConstraint;

				/*
				 * Currently, we only expect to see CONSTR_CHECK nodes
				 * arriving here (see the preprocessing done in
				 * parse_utilcmd.c).  Use a switch anyway to make it easier to
				 * add more code later.
				 */
				switch (constr->contype)
				{
					case CONSTR_CHECK:
						{
							List	   *newcons;
							ListCell   *lcon;

							/*
							 * Call AddRelationRawConstraints to do the work.
							 * It returns a list of cooked constraints.
							 */
							newcons = AddRelationRawConstraints(rel, NIL,
														 list_make1(constr));
							/* Add each constraint to Phase 3's queue */
							foreach(lcon, newcons)
							{
								CookedConstraint *ccon = (CookedConstraint *) lfirst(lcon);
								NewConstraint *newcon;

								newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
								newcon->name = ccon->name;
								newcon->contype = ccon->contype;
								/* ExecQual wants implicit-AND format */
								newcon->qual = (Node *)
									make_ands_implicit((Expr *) ccon->expr);

								tab->constraints = lappend(tab->constraints,
														   newcon);
							}
							break;
						}
					default:
						elog(ERROR, "unrecognized constraint type: %d",
							 (int) constr->contype);
				}
				break;
			}
		case T_FkConstraint:
			{
				FkConstraint *fkconstraint = (FkConstraint *) newConstraint;

				/*
				 * Assign or validate constraint name
				 */
				if (fkconstraint->constr_name)
				{
					if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
											 RelationGetRelid(rel),
											 RelationGetNamespace(rel),
											 fkconstraint->constr_name))
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("constraint \"%s\" for relation \"%s\" already exists",
										fkconstraint->constr_name,
										RelationGetRelationName(rel))));
				}
				else
					fkconstraint->constr_name =
						ChooseConstraintName(RelationGetRelationName(rel),
									strVal(linitial(fkconstraint->fk_attrs)),
											 "fkey",
											 RelationGetNamespace(rel),
											 NIL);

				ATAddForeignKeyConstraint(tab, rel, fkconstraint);

				break;
			}
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(newConstraint));
	}
}

/*
 * Add a foreign-key constraint to a single table
 *
 * Subroutine for ATExecAddConstraint.	Must already hold exclusive
 * lock on the rel, and have done appropriate validity/permissions checks
 * for it.
 */
static void
ATAddForeignKeyConstraint(AlteredTableInfo *tab, Relation rel,
						  FkConstraint *fkconstraint)
{
	Relation	pkrel;
	AclResult	aclresult;
	int16		pkattnum[INDEX_MAX_KEYS];
	int16		fkattnum[INDEX_MAX_KEYS];
	Oid			pktypoid[INDEX_MAX_KEYS];
	Oid			fktypoid[INDEX_MAX_KEYS];
	Oid			opclasses[INDEX_MAX_KEYS];
	Oid			pfeqoperators[INDEX_MAX_KEYS];
	Oid			ppeqoperators[INDEX_MAX_KEYS];
	Oid			ffeqoperators[INDEX_MAX_KEYS];
	int			i;
	int			numfks,
				numpks;
	Oid			indexOid;
	Oid			constrOid;

	/*
	 * Grab an exclusive lock on the pk table, so that someone doesn't delete
	 * rows out from under us. (Although a lesser lock would do for that
	 * purpose, we'll need exclusive lock anyway to add triggers to the pk
	 * table; trying to start with a lesser lock will just create a risk of
	 * deadlock.)
	 */
	pkrel = heap_openrv(fkconstraint->pktable, AccessExclusiveLock);

	/*
	 * Validity and permissions checks
	 *
	 * Note: REFERENCES permissions checks are redundant with CREATE TRIGGER,
	 * but we may as well error out sooner instead of later.
	 */
	if (pkrel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	aclresult = pg_class_aclcheck(RelationGetRelid(pkrel), GetUserId(),
								  ACL_REFERENCES);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(pkrel));

	if (!allowSystemTableMods && IsSystemRelation(pkrel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(pkrel))));

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_REFERENCES);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	/*
	 * Disallow reference from permanent table to temp table or vice versa.
	 * (The ban on perm->temp is for fairly obvious reasons.  The ban on
	 * temp->perm is because other backends might need to run the RI triggers
	 * on the perm table, but they can't reliably see tuples the owning
	 * backend has created in the temp table, because non-shared buffers are
	 * used for temp tables.)
	 */
	if (isTempNamespace(RelationGetNamespace(pkrel)))
	{
		if (!isTempNamespace(RelationGetNamespace(rel)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot reference temporary table from permanent table constraint")));
	}
	else
	{
		if (isTempNamespace(RelationGetNamespace(rel)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot reference permanent table from temporary table constraint")));
	}

	/*
	 * Look up the referencing attributes to make sure they exist, and record
	 * their attnums and type OIDs.
	 */
	MemSet(pkattnum, 0, sizeof(pkattnum));
	MemSet(fkattnum, 0, sizeof(fkattnum));
	MemSet(pktypoid, 0, sizeof(pktypoid));
	MemSet(fktypoid, 0, sizeof(fktypoid));
	MemSet(opclasses, 0, sizeof(opclasses));
	MemSet(pfeqoperators, 0, sizeof(pfeqoperators));
	MemSet(ppeqoperators, 0, sizeof(ppeqoperators));
	MemSet(ffeqoperators, 0, sizeof(ffeqoperators));

	numfks = transformColumnNameList(RelationGetRelid(rel),
									 fkconstraint->fk_attrs,
									 fkattnum, fktypoid);

	/*
	 * If the attribute list for the referenced table was omitted, lookup the
	 * definition of the primary key and use it.  Otherwise, validate the
	 * supplied attribute list.  In either case, discover the index OID and
	 * index opclasses, and the attnums and type OIDs of the attributes.
	 */
	if (fkconstraint->pk_attrs == NIL)
	{
		numpks = transformFkeyGetPrimaryKey(pkrel, &indexOid,
											&fkconstraint->pk_attrs,
											pkattnum, pktypoid,
											opclasses);
	}
	else
	{
		numpks = transformColumnNameList(RelationGetRelid(pkrel),
										 fkconstraint->pk_attrs,
										 pkattnum, pktypoid);
		/* Look for an index matching the column list */
		indexOid = transformFkeyCheckAttrs(pkrel, numpks, pkattnum,
										   opclasses);
	}

	/*
	 * Look up the equality operators to use in the constraint.
	 *
	 * Note that we have to be careful about the difference between the actual
	 * PK column type and the opclass' declared input type, which might be
	 * only binary-compatible with it.	The declared opcintype is the right
	 * thing to probe pg_amop with.
	 */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	for (i = 0; i < numpks; i++)
	{
		Oid			pktype = pktypoid[i];
		Oid			fktype = fktypoid[i];
		Oid			fktyped;
		HeapTuple	cla_ht;
		Form_pg_opclass cla_tup;
		Oid			amid;
		Oid			opfamily;
		Oid			opcintype;
		Oid			pfeqop;
		Oid			ppeqop;
		Oid			ffeqop;
		int16		eqstrategy;

		/* We need several fields out of the pg_opclass entry */
		cla_ht = SearchSysCache(CLAOID,
								ObjectIdGetDatum(opclasses[i]),
								0, 0, 0);
		if (!HeapTupleIsValid(cla_ht))
			elog(ERROR, "cache lookup failed for opclass %u", opclasses[i]);
		cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
		amid = cla_tup->opcmethod;
		opfamily = cla_tup->opcfamily;
		opcintype = cla_tup->opcintype;
		ReleaseSysCache(cla_ht);

		/*
		 * Check it's a btree; currently this can never fail since no other
		 * index AMs support unique indexes.  If we ever did have other types
		 * of unique indexes, we'd need a way to determine which operator
		 * strategy number is equality.  (Is it reasonable to insist that
		 * every such index AM use btree's number for equality?)
		 */
		if (amid != BTREE_AM_OID)
			elog(ERROR, "only b-tree indexes are supported for foreign keys");
		eqstrategy = BTEqualStrategyNumber;

		/*
		 * There had better be a primary equality operator for the index.
		 * We'll use it for PK = PK comparisons.
		 */
		ppeqop = get_opfamily_member(opfamily, opcintype, opcintype,
									 eqstrategy);

		if (!OidIsValid(ppeqop))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 eqstrategy, opcintype, opcintype, opfamily);

		/*
		 * Are there equality operators that take exactly the FK type? Assume
		 * we should look through any domain here.
		 */
		fktyped = getBaseType(fktype);

		pfeqop = get_opfamily_member(opfamily, opcintype, fktyped,
									 eqstrategy);
		if (OidIsValid(pfeqop))
			ffeqop = get_opfamily_member(opfamily, fktyped, fktyped,
										 eqstrategy);
		else
			ffeqop = InvalidOid;	/* keep compiler quiet */

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
		{
			/*
			 * Otherwise, look for an implicit cast from the FK type to the
			 * opcintype, and if found, use the primary equality operator.
			 * This is a bit tricky because opcintype might be a generic type
			 * such as ANYARRAY, and so what we have to test is whether the
			 * two actual column types can be concurrently cast to that type.
			 * (Otherwise, we'd fail to reject combinations such as int[] and
			 * point[].)
			 */
			Oid			input_typeids[2];
			Oid			target_typeids[2];

			input_typeids[0] = pktype;
			input_typeids[1] = fktype;
			target_typeids[0] = opcintype;
			target_typeids[1] = opcintype;
			if (can_coerce_type(2, input_typeids, target_typeids,
								COERCION_IMPLICIT))
				pfeqop = ffeqop = ppeqop;
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("foreign key constraint \"%s\" "
							"cannot be implemented",
							fkconstraint->constr_name),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of incompatible types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktype),
							   format_type_be(pktype))));

		pfeqoperators[i] = pfeqop;
		ppeqoperators[i] = ppeqop;
		ffeqoperators[i] = ffeqop;
	}

	/*
	 * Record the FK constraint in pg_constraint.
	 */
	constrOid = CreateConstraintEntry(fkconstraint->constr_name,
									  RelationGetNamespace(rel),
									  CONSTRAINT_FOREIGN,
									  fkconstraint->deferrable,
									  fkconstraint->initdeferred,
									  RelationGetRelid(rel),
									  fkattnum,
									  numfks,
									  InvalidOid,		/* not a domain
														 * constraint */
									  RelationGetRelid(pkrel),
									  pkattnum,
									  pfeqoperators,
									  ppeqoperators,
									  ffeqoperators,
									  numpks,
									  fkconstraint->fk_upd_action,
									  fkconstraint->fk_del_action,
									  fkconstraint->fk_matchtype,
									  indexOid,
									  NULL,		/* no check constraint */
									  NULL,
									  NULL);

	/*
	 * Create the triggers that will enforce the constraint.
	 */
	createForeignKeyTriggers(rel, fkconstraint, constrOid);

	/*
	 * Tell Phase 3 to check that the constraint is satisfied by existing rows
	 * (we can skip this during table creation).
	 */
	if (!fkconstraint->skip_validation)
	{
		NewConstraint *newcon;

		newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
		newcon->name = fkconstraint->constr_name;
		newcon->contype = CONSTR_FOREIGN;
		newcon->refrelid = RelationGetRelid(pkrel);
		newcon->conid = constrOid;
		newcon->qual = (Node *) fkconstraint;

		tab->constraints = lappend(tab->constraints, newcon);
	}

	/*
	 * Close pk table, but keep lock until we've committed.
	 */
	heap_close(pkrel, NoLock);
}


/*
 * transformColumnNameList - transform list of column names
 *
 * Lookup each name and return its attnum and type OID
 */
static int
transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids)
{
	ListCell   *l;
	int			attnum;

	attnum = 0;
	foreach(l, colList)
	{
		char	   *attname = strVal(lfirst(l));
		HeapTuple	atttuple;

		atttuple = SearchSysCacheAttName(relId, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" referenced in foreign key constraint does not exist",
							attname)));
		if (attnum >= INDEX_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d keys in a foreign key",
							INDEX_MAX_KEYS)));
		attnums[attnum] = ((Form_pg_attribute) GETSTRUCT(atttuple))->attnum;
		atttypids[attnum] = ((Form_pg_attribute) GETSTRUCT(atttuple))->atttypid;
		ReleaseSysCache(atttuple);
		attnum++;
	}

	return attnum;
}

/*
 * transformFkeyGetPrimaryKey -
 *
 *	Look up the names, attnums, and types of the primary key attributes
 *	for the pkrel.	Also return the index OID and index opclasses of the
 *	index supporting the primary key.
 *
 *	All parameters except pkrel are output parameters.	Also, the function
 *	return value is the number of attributes in the primary key.
 *
 *	Used when the column list in the REFERENCES specification is omitted.
 */
static int
transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids,
						   Oid *opclasses)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	HeapTuple	indexTuple = NULL;
	Form_pg_index indexStruct = NULL;
	Datum		indclassDatum;
	bool		isnull;
	oidvector  *indclass;
	int			i;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).
	 */
	*indexOid = InvalidOid;

	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);
		if (indexStruct->indisprimary)
		{
			*indexOid = indexoid;
			break;
		}
		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/*
	 * Check that we found it
	 */
	if (!OidIsValid(*indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no primary key for referenced table \"%s\"",
						RelationGetRelationName(pkrel))));

	/* Must get indclass the hard way */
	indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
									Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * Now build the list of PK attributes from the indkey definition (we
	 * assume a primary key cannot have expressional elements)
	 */
	*attnamelist = NIL;
	for (i = 0; i < indexStruct->indnatts; i++)
	{
		int			pkattno = indexStruct->indkey.values[i];

		attnums[i] = pkattno;
		atttypids[i] = attnumTypeId(pkrel, pkattno);
		opclasses[i] = indclass->values[i];
		*attnamelist = lappend(*attnamelist,
			   makeString(pstrdup(NameStr(*attnumAttName(pkrel, pkattno)))));
	}

	ReleaseSysCache(indexTuple);

	return i;
}

/*
 * transformFkeyCheckAttrs -
 *
 *	Make sure that the attributes of a referenced table belong to a unique
 *	(or primary key) constraint.  Return the OID of the index supporting
 *	the constraint, as well as the opclasses associated with the index
 *	columns.
 */
static Oid
transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						Oid *opclasses) /* output parameter */
{
	Oid			indexoid = InvalidOid;
	bool		found = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache, and match unique indexes to the list
	 * of attnums we are given.
	 */
	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;
		int			i,
					j;

		indexoid = lfirst_oid(indexoidscan);
		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Must have the right number of columns; must be unique and not a
		 * partial index; forget it if there are any expressions, too
		 */
		if (indexStruct->indnatts == numattrs &&
			indexStruct->indisunique &&
			heap_attisnull(indexTuple, Anum_pg_index_indpred) &&
			heap_attisnull(indexTuple, Anum_pg_index_indexprs))
		{
			/* Must get indclass the hard way */
			Datum		indclassDatum;
			bool		isnull;
			oidvector  *indclass;

			indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
											Anum_pg_index_indclass, &isnull);
			Assert(!isnull);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/*
			 * The given attnum list may match the index columns in any order.
			 * Check that each list is a subset of the other.
			 */
			for (i = 0; i < numattrs; i++)
			{
				found = false;
				for (j = 0; j < numattrs; j++)
				{
					if (attnums[i] == indexStruct->indkey.values[j])
					{
						found = true;
						break;
					}
				}
				if (!found)
					break;
			}
			if (found)
			{
				for (i = 0; i < numattrs; i++)
				{
					found = false;
					for (j = 0; j < numattrs; j++)
					{
						if (attnums[j] == indexStruct->indkey.values[i])
						{
							opclasses[j] = indclass->values[i];
							found = true;
							break;
						}
					}
					if (!found)
						break;
				}
			}
		}
		ReleaseSysCache(indexTuple);
		if (found)
			break;
	}

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("there is no unique constraint matching given keys for referenced table \"%s\"",
						RelationGetRelationName(pkrel))));

	list_free(indexoidlist);

	return indexoid;
}

/*
 * Scan the existing rows in a table to verify they meet a proposed FK
 * constraint.
 *
 * Caller must have opened and locked both relations.
 */
static void
validateForeignKeyConstraint(FkConstraint *fkconstraint,
							 Relation rel,
							 Relation pkrel,
							 Oid constraintOid)
{
	HeapScanDesc scan;
	HeapTuple	tuple;
	Trigger		trig;

	/*
	 * Build a trigger call structure; we'll need it either way.
	 */
	MemSet(&trig, 0, sizeof(trig));
	trig.tgoid = InvalidOid;
	trig.tgname = fkconstraint->constr_name;
	trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
	trig.tgisconstraint = TRUE;
	trig.tgconstrrelid = RelationGetRelid(pkrel);
	trig.tgconstraint = constraintOid;
	trig.tgdeferrable = FALSE;
	trig.tginitdeferred = FALSE;
	/* we needn't fill in tgargs */

	/*
	 * See if we can do it with a single LEFT JOIN query.  A FALSE result
	 * indicates we must proceed with the fire-the-trigger method.
	 */
	if (RI_Initial_Check(&trig, rel, pkrel))
		return;

	/*
	 * Scan through each tuple, calling RI_FKey_check_ins (insert trigger) as
	 * if that tuple had just been inserted.  If any of those fail, it should
	 * ereport(ERROR) and that's that.
	 */
	scan = heap_beginscan(rel, SnapshotNow, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		FunctionCallInfoData fcinfo;
		TriggerData trigdata;

		/*
		 * Make a call to the trigger function
		 *
		 * No parameters are passed, but we do set a context
		 */
		MemSet(&fcinfo, 0, sizeof(fcinfo));

		/*
		 * We assume RI_FKey_check_ins won't look at flinfo...
		 */
		trigdata.type = T_TriggerData;
		trigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
		trigdata.tg_relation = rel;
		trigdata.tg_trigtuple = tuple;
		trigdata.tg_newtuple = NULL;
		trigdata.tg_trigger = &trig;
		trigdata.tg_trigtuplebuf = scan->rs_cbuf;
		trigdata.tg_newtuplebuf = InvalidBuffer;

		fcinfo.context = (Node *) &trigdata;

		RI_FKey_check_ins(&fcinfo);
	}

	heap_endscan(scan);
}

static void
CreateFKCheckTrigger(RangeVar *myRel, FkConstraint *fkconstraint,
					 Oid constraintOid, bool on_insert)
{
	CreateTrigStmt *fk_trigger;

	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = fkconstraint->constr_name;
	fk_trigger->relation = myRel;
	fk_trigger->before = false;
	fk_trigger->row = true;

	/* Either ON INSERT or ON UPDATE */
	if (on_insert)
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_ins");
		fk_trigger->actions[0] = 'i';
	}
	else
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_upd");
		fk_trigger->actions[0] = 'u';
	}
	fk_trigger->actions[1] = '\0';

	fk_trigger->isconstraint = true;
	fk_trigger->deferrable = fkconstraint->deferrable;
	fk_trigger->initdeferred = fkconstraint->initdeferred;
	fk_trigger->constrrel = fkconstraint->pktable;
	fk_trigger->args = NIL;

	(void) CreateTrigger(fk_trigger, constraintOid);

	/* Make changes-so-far visible */
	CommandCounterIncrement();
}

/*
 * Create the triggers that implement an FK constraint.
 */
static void
createForeignKeyTriggers(Relation rel, FkConstraint *fkconstraint,
						 Oid constraintOid)
{
	RangeVar   *myRel;
	CreateTrigStmt *fk_trigger;

	/*
	 * Reconstruct a RangeVar for my relation (not passed in, unfortunately).
	 */
	myRel = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
						 pstrdup(RelationGetRelationName(rel)));

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the CHECK
	 * action for both INSERTs and UPDATEs on the referencing table.
	 */
	CreateFKCheckTrigger(myRel, fkconstraint, constraintOid, true);
	CreateFKCheckTrigger(myRel, fkconstraint, constraintOid, false);

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * DELETE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = fkconstraint->constr_name;
	fk_trigger->relation = fkconstraint->pktable;
	fk_trigger->before = false;
	fk_trigger->row = true;
	fk_trigger->actions[0] = 'd';
	fk_trigger->actions[1] = '\0';

	fk_trigger->isconstraint = true;
	fk_trigger->constrrel = myRel;
	switch (fkconstraint->fk_del_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_del");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_del");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_del");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_del");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_del");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_del_action);
			break;
	}
	fk_trigger->args = NIL;

	(void) CreateTrigger(fk_trigger, constraintOid);

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * UPDATE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = fkconstraint->constr_name;
	fk_trigger->relation = fkconstraint->pktable;
	fk_trigger->before = false;
	fk_trigger->row = true;
	fk_trigger->actions[0] = 'u';
	fk_trigger->actions[1] = '\0';
	fk_trigger->isconstraint = true;
	fk_trigger->constrrel = myRel;
	switch (fkconstraint->fk_upd_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_upd");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_upd");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_upd");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_upd");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_upd");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_upd_action);
			break;
	}
	fk_trigger->args = NIL;

	(void) CreateTrigger(fk_trigger, constraintOid);
}

/*
 * ALTER TABLE DROP CONSTRAINT
 */
static void
ATPrepDropConstraint(List **wqueue, Relation rel,
					 bool recurse, AlterTableCmd *cmd)
{
	/*
	 * We don't want errors or noise from child tables, so we have to pass
	 * down a modified command.
	 */
	if (recurse)
	{
		AlterTableCmd *childCmd = copyObject(cmd);

		childCmd->subtype = AT_DropConstraintQuietly;
		ATSimpleRecursion(wqueue, rel, childCmd, recurse);
	}
}

static void
ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior, bool quiet)
{
	int			deleted;

	deleted = RemoveRelConstraints(rel, constrName, behavior);

	if (!quiet)
	{
		/* If zero constraints deleted, complain */
		if (deleted == 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" does not exist",
							constrName)));
		/* Otherwise if more than one constraint deleted, notify */
		else if (deleted > 1)
			ereport(NOTICE,
					(errmsg("multiple constraints named \"%s\" were dropped",
							constrName)));
	}
}

/*
 * ALTER COLUMN TYPE
 */
static void
ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd)
{
	char	   *colName = cmd->name;
	TypeName   *typename = (TypeName *) cmd->def;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Oid			targettype;
	int32		targettypmod;
	Node	   *transform;
	NewColumnValue *newval;
	ParseState *pstate = make_parsestate(NULL);

	/* lookup the attribute so we can check inheritance status */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't alter a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/* Don't alter inherited columns */
	if (attTup->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter inherited column \"%s\"",
						colName)));

	/* Look up the target type */
	targettype = typenameTypeId(NULL, typename, &targettypmod);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colName, targettype);

	/*
	 * Set up an expression to transform the old data value to the new type.
	 * If a USING option was given, transform and use that expression, else
	 * just take the old value and try to coerce it.  We do this first so that
	 * type incompatibility can be detected before we waste effort, and
	 * because we need the expression to be parsed against the original table
	 * rowtype.
	 */
	if (cmd->transform)
	{
		RangeTblEntry *rte;

		/* Expression must be able to access vars of old table */
		rte = addRangeTableEntryForRelation(pstate,
											rel,
											NULL,
											false,
											true);
		addRTEtoQuery(pstate, rte, false, true, true);

		transform = transformExpr(pstate, cmd->transform);

		/* It can't return a set */
		if (expression_returns_set(transform))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("transform expression must not return a set")));

		/* No subplans or aggregates, either... */
		if (pstate->p_hasSubLinks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot use subquery in transform expression")));
		if (pstate->p_hasAggs)
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
			errmsg("cannot use aggregate function in transform expression")));
	}
	else
	{
		transform = (Node *) makeVar(1, attnum,
									 attTup->atttypid, attTup->atttypmod,
									 0);
	}

	transform = coerce_to_target_type(pstate,
									  transform, exprType(transform),
									  targettype, targettypmod,
									  COERCION_ASSIGNMENT,
									  COERCE_IMPLICIT_CAST);
	if (transform == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" cannot be cast to type \"%s\"",
						colName, TypeNameToString(typename))));

	/*
	 * Add a work queue item to make ATRewriteTable update the column
	 * contents.
	 */
	newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
	newval->attnum = attnum;
	newval->expr = (Expr *) transform;

	tab->newvals = lappend(tab->newvals, newval);

	ReleaseSysCache(tuple);

	/*
	 * The recursion case is handled by ATSimpleRecursion.	However, if we are
	 * told not to recurse, there had better not be any child tables; else the
	 * alter would put them out of step.
	 */
	if (recurse)
		ATSimpleRecursion(wqueue, rel, cmd, recurse);
	else if (!recursing &&
			 find_inheritance_children(RelationGetRelid(rel)) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("type of inherited column \"%s\" must be changed in child tables too",
						colName)));
}

static void
ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  const char *colName, TypeName *typename)
{
	HeapTuple	heapTup;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	HeapTuple	typeTuple;
	Form_pg_type tform;
	Oid			targettype;
	int32		targettypmod;
	Node	   *defaultexpr;
	Relation	attrelation;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	/* Look up the target column */
	heapTup = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(heapTup))		/* shouldn't happen */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
	attnum = attTup->attnum;

	/* Check for multiple ALTER TYPE on same column --- can't cope */
	if (attTup->atttypid != tab->oldDesc->attrs[attnum - 1]->atttypid ||
		attTup->atttypmod != tab->oldDesc->attrs[attnum - 1]->atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter type of column \"%s\" twice",
						colName)));

	/* Look up the target type (should not fail, since prep found it) */
	typeTuple = typenameType(NULL, typename, &targettypmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	targettype = HeapTupleGetOid(typeTuple);

	/*
	 * If there is a default expression for the column, get it and ensure we
	 * can coerce it to the new datatype.  (We must do this before changing
	 * the column type, because build_column_default itself will try to
	 * coerce, and will not issue the error message we want if it fails.)
	 *
	 * We remove any implicit coercion steps at the top level of the old
	 * default expression; this has been agreed to satisfy the principle of
	 * least surprise.	(The conversion to the new column type should act like
	 * it started from what the user sees as the stored expression, and the
	 * implicit coercions aren't going to be shown.)
	 */
	if (attTup->atthasdef)
	{
		defaultexpr = build_column_default(rel, attnum);
		Assert(defaultexpr);
		defaultexpr = strip_implicit_coercions(defaultexpr);
		defaultexpr = coerce_to_target_type(NULL,		/* no UNKNOWN params */
										  defaultexpr, exprType(defaultexpr),
											targettype, targettypmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST);
		if (defaultexpr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			errmsg("default for column \"%s\" cannot be cast to type \"%s\"",
				   colName, TypeNameToString(typename))));
	}
	else
		defaultexpr = NULL;

	/*
	 * Find everything that depends on the column (constraints, indexes, etc),
	 * and record enough information to let us recreate the objects.
	 *
	 * The actual recreation does not happen here, but only after we have
	 * performed all the individual ALTER TYPE operations.	We have to save
	 * the info before executing ALTER TYPE, though, else the deparser will
	 * get confused.
	 *
	 * There could be multiple entries for the same object, so we must check
	 * to ensure we process each one only once.  Note: we assume that an index
	 * that implements a constraint will not show a direct dependency on the
	 * column.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		/* We don't expect any PIN dependencies on columns */
		if (foundDep->deptype == DEPENDENCY_PIN)
			elog(ERROR, "cannot alter type of a pinned column");

		foundObject.classId = foundDep->classid;
		foundObject.objectId = foundDep->objid;
		foundObject.objectSubId = foundDep->objsubid;

		switch (getObjectClass(&foundObject))
		{
			case OCLASS_CLASS:
				{
					char		relKind = get_rel_relkind(foundObject.objectId);

					if (relKind == RELKIND_INDEX)
					{
						Assert(foundObject.objectSubId == 0);
						if (!list_member_oid(tab->changedIndexOids, foundObject.objectId))
						{
							tab->changedIndexOids = lappend_oid(tab->changedIndexOids,
													   foundObject.objectId);
							tab->changedIndexDefs = lappend(tab->changedIndexDefs,
							   pg_get_indexdef_string(foundObject.objectId));
						}
					}
					else if (relKind == RELKIND_SEQUENCE)
					{
						/*
						 * This must be a SERIAL column's sequence.  We need
						 * not do anything to it.
						 */
						Assert(foundObject.objectSubId == 0);
					}
					else
					{
						/* Not expecting any other direct dependencies... */
						elog(ERROR, "unexpected object depending on column: %s",
							 getObjectDescription(&foundObject));
					}
					break;
				}

			case OCLASS_CONSTRAINT:
				Assert(foundObject.objectSubId == 0);
				if (!list_member_oid(tab->changedConstraintOids,
									 foundObject.objectId))
				{
					char	   *defstring = pg_get_constraintdef_string(foundObject.objectId);

					/*
					 * Put NORMAL dependencies at the front of the list and
					 * AUTO dependencies at the back.  This makes sure that
					 * foreign-key constraints depending on this column will
					 * be dropped before unique or primary-key constraints of
					 * the column; which we must have because the FK
					 * constraints depend on the indexes belonging to the
					 * unique constraints.
					 */
					if (foundDep->deptype == DEPENDENCY_NORMAL)
					{
						tab->changedConstraintOids =
							lcons_oid(foundObject.objectId,
									  tab->changedConstraintOids);
						tab->changedConstraintDefs =
							lcons(defstring,
								  tab->changedConstraintDefs);
					}
					else
					{
						tab->changedConstraintOids =
							lappend_oid(tab->changedConstraintOids,
										foundObject.objectId);
						tab->changedConstraintDefs =
							lappend(tab->changedConstraintDefs,
									defstring);
					}
				}
				break;

			case OCLASS_REWRITE:
				/* XXX someday see if we can cope with revising views */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used by a view or rule"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject),
								   colName)));
				break;

			case OCLASS_DEFAULT:

				/*
				 * Ignore the column's default expression, since we will fix
				 * it below.
				 */
				Assert(defaultexpr);
				break;

			case OCLASS_PROC:
			case OCLASS_TYPE:
			case OCLASS_CAST:
			case OCLASS_CONVERSION:
			case OCLASS_LANGUAGE:
			case OCLASS_OPERATOR:
			case OCLASS_OPCLASS:
			case OCLASS_OPFAMILY:
			case OCLASS_TRIGGER:
			case OCLASS_SCHEMA:
			case OCLASS_TSPARSER:
			case OCLASS_TSDICT:
			case OCLASS_TSTEMPLATE:
			case OCLASS_TSCONFIG:

				/*
				 * We don't expect any of these sorts of objects to depend on
				 * a column.
				 */
				elog(ERROR, "unexpected object depending on column: %s",
					 getObjectDescription(&foundObject));
				break;

			default:
				elog(ERROR, "unrecognized object class: %u",
					 foundObject.classId);
		}
	}

	systable_endscan(scan);

	/*
	 * Now scan for dependencies of this column on other things.  The only
	 * thing we should find is the dependency on the column datatype, which we
	 * want to remove.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);

		if (foundDep->deptype != DEPENDENCY_NORMAL)
			elog(ERROR, "found unexpected dependency type '%c'",
				 foundDep->deptype);
		if (foundDep->refclassid != TypeRelationId ||
			foundDep->refobjid != attTup->atttypid)
			elog(ERROR, "found unexpected dependency for column");

		simple_heap_delete(depRel, &depTup->t_self);
	}

	systable_endscan(scan);

	heap_close(depRel, RowExclusiveLock);

	/*
	 * Here we go --- change the recorded column type.	(Note heapTup is a
	 * copy of the syscache entry, so okay to scribble on.)
	 */
	attTup->atttypid = targettype;
	attTup->atttypmod = targettypmod;
	attTup->attndims = list_length(typename->arrayBounds);
	attTup->attlen = tform->typlen;
	attTup->attbyval = tform->typbyval;
	attTup->attalign = tform->typalign;
	attTup->attstorage = tform->typstorage;

	ReleaseSysCache(typeTuple);

	simple_heap_update(attrelation, &heapTup->t_self, heapTup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, heapTup);

	heap_close(attrelation, RowExclusiveLock);

	/* Install dependency on new datatype */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);

	/*
	 * Drop any pg_statistic entry for the column, since it's now wrong type
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	/*
	 * Update the default, if present, by brute force --- remove and re-add
	 * the default.  Probably unsafe to take shortcuts, since the new version
	 * may well have additional dependencies.  (It's okay to do this now,
	 * rather than after other ALTER TYPE commands, since the default won't
	 * depend on other column types.)
	 */
	if (defaultexpr)
	{
		/* Must make new row visible since it will be updated again */
		CommandCounterIncrement();

		/*
		 * We use RESTRICT here for safety, but at present we do not expect
		 * anything to depend on the default.
		 */
		RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true);

		StoreAttrDefault(rel, attnum, nodeToString(defaultexpr));
	}

	/* Cleanup */
	heap_freetuple(heapTup);
}

/*
 * Cleanup after we've finished all the ALTER TYPE operations for a
 * particular relation.  We have to drop and recreate all the indexes
 * and constraints that depend on the altered columns.
 */
static void
ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab)
{
	ObjectAddress obj;
	ListCell   *l;

	/*
	 * Re-parse the index and constraint definitions, and attach them to the
	 * appropriate work queue entries.	We do this before dropping because in
	 * the case of a FOREIGN KEY constraint, we might not yet have exclusive
	 * lock on the table the constraint is attached to, and we need to get
	 * that before dropping.  It's safe because the parser won't actually look
	 * at the catalogs to detect the existing entry.
	 */
	foreach(l, tab->changedIndexDefs)
		ATPostAlterTypeParse((char *) lfirst(l), wqueue);
	foreach(l, tab->changedConstraintDefs)
		ATPostAlterTypeParse((char *) lfirst(l), wqueue);

	/*
	 * Now we can drop the existing constraints and indexes --- constraints
	 * first, since some of them might depend on the indexes.  In fact, we
	 * have to delete FOREIGN KEY constraints before UNIQUE constraints, but
	 * we already ordered the constraint list to ensure that would happen. It
	 * should be okay to use DROP_RESTRICT here, since nothing else should be
	 * depending on these objects.
	 */
	foreach(l, tab->changedConstraintOids)
	{
		obj.classId = ConstraintRelationId;
		obj.objectId = lfirst_oid(l);
		obj.objectSubId = 0;
		performDeletion(&obj, DROP_RESTRICT);
	}

	foreach(l, tab->changedIndexOids)
	{
		obj.classId = RelationRelationId;
		obj.objectId = lfirst_oid(l);
		obj.objectSubId = 0;
		performDeletion(&obj, DROP_RESTRICT);
	}

	/*
	 * The objects will get recreated during subsequent passes over the work
	 * queue.
	 */
}

static void
ATPostAlterTypeParse(char *cmd, List **wqueue)
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *list_item;

	/*
	 * We expect that we will get only ALTER TABLE and CREATE INDEX
	 * statements. Hence, there is no need to pass them through
	 * parse_analyze() or the rewriter, but instead we need to pass them
	 * through parse_utilcmd.c to make them ready for execution.
	 */
	raw_parsetree_list = raw_parser(cmd);
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		Node	   *stmt = (Node *) lfirst(list_item);

		if (IsA(stmt, IndexStmt))
			querytree_list = lappend(querytree_list,
									 transformIndexStmt((IndexStmt *) stmt,
														cmd));
		else if (IsA(stmt, AlterTableStmt))
			querytree_list = list_concat(querytree_list,
							 transformAlterTableStmt((AlterTableStmt *) stmt,
													 cmd));
		else
			querytree_list = lappend(querytree_list, stmt);
	}

	/*
	 * Attach each generated command to the proper place in the work queue.
	 * Note this could result in creation of entirely new work-queue entries.
	 */
	foreach(list_item, querytree_list)
	{
		Node	   *stm = (Node *) lfirst(list_item);
		Relation	rel;
		AlteredTableInfo *tab;

		switch (nodeTag(stm))
		{
			case T_IndexStmt:
				{
					IndexStmt  *stmt = (IndexStmt *) stm;
					AlterTableCmd *newcmd;

					rel = relation_openrv(stmt->relation, AccessExclusiveLock);
					tab = ATGetQueueEntry(wqueue, rel);
					newcmd = makeNode(AlterTableCmd);
					newcmd->subtype = AT_ReAddIndex;
					newcmd->def = (Node *) stmt;
					tab->subcmds[AT_PASS_OLD_INDEX] =
						lappend(tab->subcmds[AT_PASS_OLD_INDEX], newcmd);
					relation_close(rel, NoLock);
					break;
				}
			case T_AlterTableStmt:
				{
					AlterTableStmt *stmt = (AlterTableStmt *) stm;
					ListCell   *lcmd;

					rel = relation_openrv(stmt->relation, AccessExclusiveLock);
					tab = ATGetQueueEntry(wqueue, rel);
					foreach(lcmd, stmt->cmds)
					{
						AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

						switch (cmd->subtype)
						{
							case AT_AddIndex:
								cmd->subtype = AT_ReAddIndex;
								tab->subcmds[AT_PASS_OLD_INDEX] =
									lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);
								break;
							case AT_AddConstraint:
								tab->subcmds[AT_PASS_OLD_CONSTR] =
									lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
								break;
							default:
								elog(ERROR, "unexpected statement type: %d",
									 (int) cmd->subtype);
						}
					}
					relation_close(rel, NoLock);
					break;
				}
			default:
				elog(ERROR, "unexpected statement type: %d",
					 (int) nodeTag(stm));
		}
	}
}


/*
 * ALTER TABLE OWNER
 *
 * recursing is true if we are recursing from a table to its indexes,
 * sequences, or toast table.  We don't allow the ownership of those things to
 * be changed separately from the parent table.  Also, we can skip permission
 * checks (this is necessary not just an optimization, else we'd fail to
 * handle toast tables properly).
 *
 * recursing is also true if ALTER TYPE OWNER is calling us to fix up a
 * free-standing composite type.
 */
void
ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing)
{
	Relation	target_rel;
	Relation	class_rel;
	HeapTuple	tuple;
	Form_pg_class tuple_class;

	/*
	 * Get exclusive lock till end of transaction on the target table. Use
	 * relation_open so that we can work on indexes and sequences.
	 */
	target_rel = relation_open(relationOid, AccessExclusiveLock);

	/* Get its pg_class tuple, too */
	class_rel = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relationOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationOid);
	tuple_class = (Form_pg_class) GETSTRUCT(tuple);

	/* Can we change the ownership of this tuple? */
	switch (tuple_class->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
			/* ok to change owner */
			break;
		case RELKIND_INDEX:
			if (!recursing)
			{
				/*
				 * Because ALTER INDEX OWNER used to be allowed, and in fact
				 * is generated by old versions of pg_dump, we give a warning
				 * and do nothing rather than erroring out.  Also, to avoid
				 * unnecessary chatter while restoring those old dumps, say
				 * nothing at all if the command would be a no-op anyway.
				 */
				if (tuple_class->relowner != newOwnerId)
					ereport(WARNING,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("cannot change owner of index \"%s\"",
									NameStr(tuple_class->relname)),
							 errhint("Change the ownership of the index's table, instead.")));
				/* quick hack to exit via the no-op path */
				newOwnerId = tuple_class->relowner;
			}
			break;
		case RELKIND_SEQUENCE:
			if (!recursing &&
				tuple_class->relowner != newOwnerId)
			{
				/* if it's an owned sequence, disallow changing it by itself */
				Oid			tableId;
				int32		colId;

				if (sequenceIsOwned(relationOid, &tableId, &colId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot change owner of sequence \"%s\"",
									NameStr(tuple_class->relname)),
					  errdetail("Sequence \"%s\" is linked to table \"%s\".",
								NameStr(tuple_class->relname),
								get_rel_name(tableId))));
			}
			break;
		case RELKIND_COMPOSITE_TYPE:
			if (recursing)
				break;
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a composite type",
							NameStr(tuple_class->relname)),
					 errhint("Use ALTER TYPE instead.")));
			break;
		case RELKIND_TOASTVALUE:
			if (recursing)
				break;
			/* FALL THRU */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table, view, or sequence",
							NameStr(tuple_class->relname))));
	}

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (tuple_class->relowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_class];
		char		repl_null[Natts_pg_class];
		char		repl_repl[Natts_pg_class];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* skip permission checks when recursing to index or toast table */
		if (!recursing)
		{
			/* Superusers can always do it */
			if (!superuser())
			{
				Oid			namespaceOid = tuple_class->relnamespace;
				AclResult	aclresult;

				/* Otherwise, must be owner of the existing object */
				if (!pg_class_ownercheck(relationOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
								   RelationGetRelationName(target_rel));

				/* Must be able to become new owner */
				check_is_member_of_role(GetUserId(), newOwnerId);

				/* New owner must have CREATE privilege on namespace */
				aclresult = pg_namespace_aclcheck(namespaceOid, newOwnerId,
												  ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
								   get_namespace_name(namespaceOid));
			}
		}

		memset(repl_null, ' ', sizeof(repl_null));
		memset(repl_repl, ' ', sizeof(repl_repl));

		repl_repl[Anum_pg_class_relowner - 1] = 'r';
		repl_val[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = SysCacheGetAttr(RELOID, tuple,
								   Anum_pg_class_relacl,
								   &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 tuple_class->relowner, newOwnerId);
			repl_repl[Anum_pg_class_relacl - 1] = 'r';
			repl_val[Anum_pg_class_relacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modifytuple(tuple, RelationGetDescr(class_rel), repl_val, repl_null, repl_repl);

		simple_heap_update(class_rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(class_rel, newtuple);

		heap_freetuple(newtuple);

		/*
		 * Update owner dependency reference, if any.  A composite type has
		 * none, because it's tracked for the pg_type entry instead of here;
		 * indexes and TOAST tables don't have their own entries either.
		 */
		if (tuple_class->relkind != RELKIND_COMPOSITE_TYPE &&
			tuple_class->relkind != RELKIND_INDEX &&
			tuple_class->relkind != RELKIND_TOASTVALUE)
			changeDependencyOnOwner(RelationRelationId, relationOid,
									newOwnerId);

		/*
		 * Also change the ownership of the table's rowtype, if it has one
		 */
		if (tuple_class->relkind != RELKIND_INDEX)
			AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId,
							 tuple_class->relkind == RELKIND_COMPOSITE_TYPE);

		/*
		 * If we are operating on a table, also change the ownership of any
		 * indexes and sequences that belong to the table, as well as the
		 * table's toast table (if it has one)
		 */
		if (tuple_class->relkind == RELKIND_RELATION ||
			tuple_class->relkind == RELKIND_TOASTVALUE)
		{
			List	   *index_oid_list;
			ListCell   *i;

			/* Find all the indexes belonging to this relation */
			index_oid_list = RelationGetIndexList(target_rel);

			/* For each index, recursively change its ownership */
			foreach(i, index_oid_list)
				ATExecChangeOwner(lfirst_oid(i), newOwnerId, true);

			list_free(index_oid_list);
		}

		if (tuple_class->relkind == RELKIND_RELATION)
		{
			/* If it has a toast table, recurse to change its ownership */
			if (tuple_class->reltoastrelid != InvalidOid)
				ATExecChangeOwner(tuple_class->reltoastrelid, newOwnerId,
								  true);

			/* If it has dependent sequences, recurse to change them too */
			change_owner_recurse_to_sequences(relationOid, newOwnerId);
		}
	}

	ReleaseSysCache(tuple);
	heap_close(class_rel, RowExclusiveLock);
	relation_close(target_rel, NoLock);
}

/*
 * change_owner_recurse_to_sequences
 *
 * Helper function for ATExecChangeOwner.  Examines pg_depend searching
 * for sequences that are dependent on serial columns, and changes their
 * ownership.
 */
static void
change_owner_recurse_to_sequences(Oid relationOid, Oid newOwnerId)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL sequences are those having an auto dependency on one of the
	 * table's columns (we don't care *which* column, exactly).
	 */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationOid));
	/* we leave refobjsubid unspecified */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* skip dependencies other than auto dependencies on columns */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			depForm->deptype != DEPENDENCY_AUTO)
			continue;

		/* Use relation_open just in case it's an index */
		seqRel = relation_open(depForm->objid, AccessExclusiveLock);

		/* skip non-sequence relations */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			relation_close(seqRel, AccessExclusiveLock);
			continue;
		}

		/* We don't need to close the sequence while we alter it. */
		ATExecChangeOwner(depForm->objid, newOwnerId, true);

		/* Now we can close it.  Keep the lock till end of transaction. */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}

/*
 * ALTER TABLE CLUSTER ON
 *
 * The only thing we have to do is to change the indisclustered bits.
 */
static void
ATExecClusterOn(Relation rel, const char *indexName)
{
	Oid			indexOid;

	indexOid = get_relname_relid(indexName, rel->rd_rel->relnamespace);

	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						indexName, RelationGetRelationName(rel))));

	/* Check index is valid to cluster on */
	check_index_is_clusterable(rel, indexOid, false);

	/* And do the work */
	mark_index_clustered(rel, indexOid);
}

/*
 * ALTER TABLE SET WITHOUT CLUSTER
 *
 * We have to find any indexes on the table that have indisclustered bit
 * set and turn it off.
 */
static void
ATExecDropCluster(Relation rel)
{
	mark_index_clustered(rel, InvalidOid);
}

/*
 * ALTER TABLE SET TABLESPACE
 */
static void
ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel, char *tablespacename)
{
	Oid			tablespaceId;
	AclResult	aclresult;

	/* Check that the tablespace exists */
	tablespaceId = get_tablespace_oid(tablespacename);
	if (!OidIsValid(tablespaceId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist", tablespacename)));

	/* Check its permissions */
	aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_TABLESPACE, tablespacename);

	/* Save info for Phase 3 to do the real work */
	if (OidIsValid(tab->newTableSpace))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot have multiple SET TABLESPACE subcommands")));
	tab->newTableSpace = tablespaceId;
}

/*
 * ALTER TABLE/INDEX SET (...) or RESET (...)
 */
static void
ATExecSetRelOptions(Relation rel, List *defList, bool isReset)
{
	Oid			relid;
	Relation	pgclass;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		datum;
	bool		isnull;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_class];
	char		repl_null[Natts_pg_class];
	char		repl_repl[Natts_pg_class];

	if (defList == NIL)
		return;					/* nothing to do */

	pgclass = heap_open(RelationRelationId, RowExclusiveLock);

	/* Get the old reloptions */
	relid = RelationGetRelid(rel);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);

	/* Generate new proposed reloptions (text array) */
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 defList, false, isReset);

	/* Validate */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
			(void) heap_reloptions(rel->rd_rel->relkind, newOptions, true);
			break;
		case RELKIND_INDEX:
			(void) index_reloptions(rel->rd_am->amoptions, newOptions, true);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table, index, or TOAST table",
							RelationGetRelationName(rel))));
			break;
	}

	/*
	 * All we need do here is update the pg_class row; the new options will be
	 * propagated into relcaches during post-commit cache inval.
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, ' ', sizeof(repl_null));
	memset(repl_repl, ' ', sizeof(repl_repl));

	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newOptions;
	else
		repl_null[Anum_pg_class_reloptions - 1] = 'n';

	repl_repl[Anum_pg_class_reloptions - 1] = 'r';

	newtuple = heap_modifytuple(tuple, RelationGetDescr(pgclass),
								repl_val, repl_null, repl_repl);

	simple_heap_update(pgclass, &newtuple->t_self, newtuple);

	CatalogUpdateIndexes(pgclass, newtuple);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	heap_close(pgclass, RowExclusiveLock);
}

/*
 * Execute ALTER TABLE SET TABLESPACE for cases where there is no tuple
 * rewriting to be done, so we just want to copy the data as fast as possible.
 */
static void
ATExecSetTableSpace(Oid tableOid, Oid newTableSpace)
{
	Relation	rel;
	Oid			oldTableSpace;
	Oid			reltoastrelid;
	Oid			reltoastidxid;
	RelFileNode newrnode;
	SMgrRelation dstrel;
	Relation	pg_class;
	HeapTuple	tuple;
	Form_pg_class rd_rel;

	/*
	 * Need lock here in case we are recursing to toast table or index
	 */
	rel = relation_open(tableOid, AccessExclusiveLock);

	/*
	 * We can never allow moving of shared or nailed-in-cache relations,
	 * because we can't support changing their reltablespace values.
	 */
	if (rel->rd_rel->relisshared || rel->rd_isnailed)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move system relation \"%s\"",
						RelationGetRelationName(rel))));

	/* Can't move a non-shared relation into pg_global */
	if (newTableSpace == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * Don't allow moving temp tables of other backends ... their local buffer
	 * manager is not going to cope.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move temporary tables of other sessions")));

	/*
	 * No work if no change in tablespace.
	 */
	oldTableSpace = rel->rd_rel->reltablespace;
	if (newTableSpace == oldTableSpace ||
		(newTableSpace == MyDatabaseTableSpace && oldTableSpace == 0))
	{
		relation_close(rel, NoLock);
		return;
	}

	reltoastrelid = rel->rd_rel->reltoastrelid;
	reltoastidxid = rel->rd_rel->reltoastidxid;

	/* Get a modifiable copy of the relation's pg_class row */
	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(tableOid),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", tableOid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* create another storage file. Is it a little ugly ? */
	/* NOTE: any conflict in relfilenode value will be caught here */
	newrnode = rel->rd_node;
	newrnode.spcNode = newTableSpace;

	dstrel = smgropen(newrnode);
	smgrcreate(dstrel, rel->rd_istemp, false);

	/* copy relation data to the new physical file */
	copy_relation_data(rel, dstrel);

	/* schedule unlinking old physical file */
	RelationOpenSmgr(rel);
	smgrscheduleunlink(rel->rd_smgr, rel->rd_istemp);

	/*
	 * Now drop smgr references.  The source was already dropped by
	 * smgrscheduleunlink.
	 */
	smgrclose(dstrel);

	/* update the pg_class row */
	rd_rel->reltablespace = (newTableSpace == MyDatabaseTableSpace) ? InvalidOid : newTableSpace;
	simple_heap_update(pg_class, &tuple->t_self, tuple);
	CatalogUpdateIndexes(pg_class, tuple);

	heap_freetuple(tuple);

	heap_close(pg_class, RowExclusiveLock);

	relation_close(rel, NoLock);

	/* Make sure the reltablespace change is visible */
	CommandCounterIncrement();

	/* Move associated toast relation and/or index, too */
	if (OidIsValid(reltoastrelid))
		ATExecSetTableSpace(reltoastrelid, newTableSpace);
	if (OidIsValid(reltoastidxid))
		ATExecSetTableSpace(reltoastidxid, newTableSpace);
}

/*
 * Copy data, block by block
 */
static void
copy_relation_data(Relation rel, SMgrRelation dst)
{
	SMgrRelation src;
	bool		use_wal;
	BlockNumber nblocks;
	BlockNumber blkno;
	char		buf[BLCKSZ];
	Page		page = (Page) buf;

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	/*
	 * We need to log the copied data in WAL iff WAL archiving is enabled AND
	 * it's not a temp rel.
	 */
	use_wal = XLogArchivingActive() && !rel->rd_istemp;

	nblocks = RelationGetNumberOfBlocks(rel);
	/* RelationGetNumberOfBlocks will certainly have opened rd_smgr */
	src = rel->rd_smgr;

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		smgrread(src, blkno, buf);

		/* XLOG stuff */
		if (use_wal)
			log_newpage(&dst->smgr_rnode, blkno, page);

		/*
		 * Now write the page.	We say isTemp = true even if it's not a temp
		 * rel, because there's no need for smgr to schedule an fsync for this
		 * write; we'll do it ourselves below.
		 */
		smgrextend(dst, blkno, buf, true);
	}

	/*
	 * If the rel isn't temp, we must fsync it down to disk before it's safe
	 * to commit the transaction.  (For a temp rel we don't care since the rel
	 * will be uninteresting after a crash anyway.)
	 *
	 * It's obvious that we must do this when not WAL-logging the copy. It's
	 * less obvious that we have to do it even if we did WAL-log the copied
	 * pages. The reason is that since we're copying outside shared buffers, a
	 * CHECKPOINT occurring during the copy has no way to flush the previously
	 * written data to disk (indeed it won't know the new rel even exists).  A
	 * crash later on would replay WAL from the checkpoint, therefore it
	 * wouldn't replay our earlier WAL entries. If we do not fsync those pages
	 * here, they might still not be on disk when the crash occurs.
	 */
	if (!rel->rd_istemp)
		smgrimmedsync(dst);
}

/*
 * ALTER TABLE ENABLE/DISABLE TRIGGER
 *
 * We just pass this off to trigger.c.
 */
static void
ATExecEnableDisableTrigger(Relation rel, char *trigname,
						   char fires_when, bool skip_system)
{
	EnableDisableTrigger(rel, trigname, fires_when, skip_system);
}

/*
 * ALTER TABLE ENABLE/DISABLE RULE
 *
 * We just pass this off to rewriteDefine.c.
 */
static void
ATExecEnableDisableRule(Relation rel, char *trigname,
						char fires_when)
{
	EnableDisableRule(rel, trigname, fires_when);
}

/*
 * ALTER TABLE INHERIT
 *
 * Add a parent to the child's parents. This verifies that all the columns and
 * check constraints of the parent appear in the child and that they have the
 * same data types and expressions.
 */
static void
ATExecAddInherit(Relation child_rel, RangeVar *parent)
{
	Relation	parent_rel,
				catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	int32		inhseqno;
	List	   *children;

	/*
	 * AccessShareLock on the parent is what's obtained during normal CREATE
	 * TABLE ... INHERITS ..., so should be enough here.
	 */
	parent_rel = heap_openrv(parent, AccessShareLock);

	/*
	 * Must be owner of both parent and child -- child was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(parent_rel, false);

	/* Permanent rels cannot inherit from temporary ones */
	if (!isTempNamespace(RelationGetNamespace(child_rel)) &&
		isTempNamespace(RelationGetNamespace(parent_rel)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation \"%s\"",
						RelationGetRelationName(parent_rel))));

	/*
	 * Check for duplicates in the list of parents, and determine the highest
	 * inhseqno already present; we'll use the next one for the new parent.
	 * (Note: get RowExclusiveLock because we will write pg_inherits below.)
	 *
	 * Note: we do not reject the case where the child already inherits from
	 * the parent indirectly; CREATE TABLE doesn't reject comparable cases.
	 */
	catalogRelation = heap_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, SnapshotNow, 1, &key);

	/* inhseqno sequences start at 1 */
	inhseqno = 0;
	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inheritsTuple);

		if (inh->inhparent == RelationGetRelid(parent_rel))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
			 errmsg("relation \"%s\" would be inherited from more than once",
					RelationGetRelationName(parent_rel))));
		if (inh->inhseqno > inhseqno)
			inhseqno = inh->inhseqno;
	}
	systable_endscan(scan);

	/*
	 * Prevent circularity by seeing if proposed parent inherits from child.
	 * (In particular, this disallows making a rel inherit from itself.)
	 *
	 * This is not completely bulletproof because of race conditions: in
	 * multi-level inheritance trees, someone else could concurrently be
	 * making another inheritance link that closes the loop but does not join
	 * either of the rels we have locked.  Preventing that seems to require
	 * exclusive locks on the entire inheritance tree, which is a cure worse
	 * than the disease.  find_all_inheritors() will cope with circularity
	 * anyway, so don't sweat it too much.
	 */
	children = find_all_inheritors(RelationGetRelid(child_rel));

	if (list_member_oid(children, RelationGetRelid(parent_rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   parent->relname,
						   RelationGetRelationName(child_rel))));

	/* If parent has OIDs then child must have OIDs */
	if (parent_rel->rd_rel->relhasoids && !child_rel->rd_rel->relhasoids)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("table \"%s\" without OIDs cannot inherit from table \"%s\" with OIDs",
						RelationGetRelationName(child_rel),
						RelationGetRelationName(parent_rel))));

	/* Match up the columns and bump attinhcount and attislocal */
	MergeAttributesIntoExisting(child_rel, parent_rel);

	/* Match up the constraints and make sure they're present in child */
	MergeConstraintsIntoExisting(child_rel, parent_rel);

	/*
	 * OK, it looks valid.	Make the catalog entries that show inheritance.
	 */
	StoreCatalogInheritance1(RelationGetRelid(child_rel),
							 RelationGetRelid(parent_rel),
							 inhseqno + 1,
							 catalogRelation);

	/* Now we're done with pg_inherits */
	heap_close(catalogRelation, RowExclusiveLock);

	/* keep our lock on the parent relation until commit */
	heap_close(parent_rel, NoLock);
}

/*
 * Obtain the source-text form of the constraint expression for a check
 * constraint, given its pg_constraint tuple
 */
static char *
decompile_conbin(HeapTuple contup, TupleDesc tupdesc)
{
	Form_pg_constraint con;
	bool		isnull;
	Datum		attr;
	Datum		expr;

	con = (Form_pg_constraint) GETSTRUCT(contup);
	attr = heap_getattr(contup, Anum_pg_constraint_conbin, tupdesc, &isnull);
	if (isnull)
		elog(ERROR, "null conbin for constraint %u", HeapTupleGetOid(contup));

	expr = DirectFunctionCall2(pg_get_expr, attr,
							   ObjectIdGetDatum(con->conrelid));
	return DatumGetCString(DirectFunctionCall1(textout, expr));
}

/*
 * Check columns in child table match up with columns in parent, and increment
 * their attinhcount.
 *
 * Called by ATExecAddInherit
 *
 * Currently all parent columns must be found in child. Missing columns are an
 * error.  One day we might consider creating new columns like CREATE TABLE
 * does.  However, that is widely unpopular --- in the common use case of
 * partitioned tables it's a foot-gun.
 *
 * The data type must match exactly. If the parent column is NOT NULL then
 * the child must be as well. Defaults are not compared, however.
 */
static void
MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	attrrel;
	AttrNumber	parent_attno;
	int			parent_natts;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	HeapTuple	tuple;

	attrrel = heap_open(AttributeRelationId, RowExclusiveLock);

	tupleDesc = RelationGetDescr(parent_rel);
	parent_natts = tupleDesc->natts;
	constr = tupleDesc->constr;

	for (parent_attno = 1; parent_attno <= parent_natts; parent_attno++)
	{
		Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore dropped columns in the parent. */
		if (attribute->attisdropped)
			continue;

		/* Find same column in child (matching on column name). */
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(child_rel),
										  attributeName);
		if (HeapTupleIsValid(tuple))
		{
			/* Check they are same type and typmod */
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (attribute->atttypid != childatt->atttypid ||
				attribute->atttypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName)));

			if (attribute->attnotnull && !childatt->attnotnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
				errmsg("column \"%s\" in child table must be marked NOT NULL",
					   attributeName)));

			/*
			 * OK, bump the child column's inheritance count.  (If we fail
			 * later on, this change will just roll back.)
			 */
			childatt->attinhcount++;
			simple_heap_update(attrrel, &tuple->t_self, tuple);
			CatalogUpdateIndexes(attrrel, tuple);
			heap_freetuple(tuple);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing column \"%s\"",
							attributeName)));
		}
	}

	heap_close(attrrel, RowExclusiveLock);
}

/*
 * Check constraints in child table match up with constraints in parent
 *
 * Called by ATExecAddInherit
 *
 * Currently all constraints in parent must be present in the child. One day we
 * may consider adding new constraints like CREATE TABLE does. We may also want
 * to allow an optional flag on parent table constraints indicating they are
 * intended to ONLY apply to the master table, not to the children. That would
 * make it possible to ensure no records are mistakenly inserted into the
 * master in partitioned tables rather than the appropriate child.
 *
 * XXX This is O(N^2) which may be an issue with tables with hundreds of
 * constraints. As long as tables have more like 10 constraints it shouldn't be
 * a problem though. Even 100 constraints ought not be the end of the world.
 */
static void
MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	TupleDesc	tupleDesc;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	constraintTuple;
	ListCell   *elem;
	List	   *constraints;

	/* First gather up the child's constraint definitions */
	catalogRelation = heap_open(ConstraintRelationId, AccessShareLock);
	tupleDesc = RelationGetDescr(catalogRelation);

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId,
							  true, SnapshotNow, 1, &key);

	constraints = NIL;
	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);

		if (con->contype != CONSTRAINT_CHECK)
			continue;

		constraints = lappend(constraints, heap_copytuple(constraintTuple));
	}

	systable_endscan(scan);

	/* Then scan through the parent's constraints looking for matches */
	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId, true,
							  SnapshotNow, 1, &key);

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint parent_con = (Form_pg_constraint) GETSTRUCT(constraintTuple);
		bool		found = false;
		Form_pg_constraint child_con = NULL;
		HeapTuple	child_contuple = NULL;

		if (parent_con->contype != CONSTRAINT_CHECK)
			continue;

		foreach(elem, constraints)
		{
			child_contuple = (HeapTuple) lfirst(elem);
			child_con = (Form_pg_constraint) GETSTRUCT(child_contuple);
			if (strcmp(NameStr(parent_con->conname),
					   NameStr(child_con->conname)) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing constraint \"%s\"",
							NameStr(parent_con->conname))));

		if (parent_con->condeferrable != child_con->condeferrable ||
			parent_con->condeferred != child_con->condeferred ||
			strcmp(decompile_conbin(constraintTuple, tupleDesc),
				   decompile_conbin(child_contuple, tupleDesc)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("constraint definition for check constraint \"%s\" does not match",
							NameStr(parent_con->conname))));

		/*
		 * TODO: add conislocal,coninhcount to constraints. This is where we
		 * would have to bump them just like attributes
		 */
	}

	systable_endscan(scan);
	heap_close(catalogRelation, AccessShareLock);
}

/*
 * ALTER TABLE NO INHERIT
 *
 * Drop a parent from the child's parents. This just adjusts the attinhcount
 * and attislocal of the columns and removes the pg_inherit and pg_depend
 * entries.
 *
 * If attinhcount goes to 0 then attislocal gets set to true. If it goes back
 * up attislocal stays true, which means if a child is ever removed from a
 * parent then its columns will never be automatically dropped which may
 * surprise. But at least we'll never surprise by dropping columns someone
 * isn't expecting to be dropped which would actually mean data loss.
 */
static void
ATExecDropInherit(Relation rel, RangeVar *parent)
{
	Relation	parent_rel;
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	inheritsTuple,
				attributeTuple,
				depTuple;
	bool		found = false;

	/*
	 * AccessShareLock on the parent is probably enough, seeing that DROP
	 * TABLE doesn't lock parent tables at all.  We need some lock since we'll
	 * be inspecting the parent's schema.
	 */
	parent_rel = heap_openrv(parent, AccessShareLock);

	/*
	 * We don't bother to check ownership of the parent table --- ownership of
	 * the child is presumed enough rights.
	 */

	/*
	 * Find and destroy the pg_inherits entry linking the two, or error out if
	 * there is none.
	 */
	catalogRelation = heap_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, SnapshotNow, 1, key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Oid			inhparent;

		inhparent = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhparent;
		if (inhparent == RelationGetRelid(parent_rel))
		{
			simple_heap_delete(catalogRelation, &inheritsTuple->t_self);
			found = true;
			break;
		}
	}

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" is not a parent of relation \"%s\"",
						RelationGetRelationName(parent_rel),
						RelationGetRelationName(rel))));

	/*
	 * Search through child columns looking for ones matching parent rel
	 */
	catalogRelation = heap_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(catalogRelation, AttributeRelidNumIndexId,
							  true, SnapshotNow, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);

		/* Ignore if dropped or not inherited */
		if (att->attisdropped)
			continue;
		if (att->attinhcount <= 0)
			continue;

		if (SearchSysCacheExistsAttName(RelationGetRelid(parent_rel),
										NameStr(att->attname)))
		{
			/* Decrement inhcount and possibly set islocal to true */
			HeapTuple	copyTuple = heap_copytuple(attributeTuple);
			Form_pg_attribute copy_att = (Form_pg_attribute) GETSTRUCT(copyTuple);

			copy_att->attinhcount--;
			if (copy_att->attinhcount == 0)
				copy_att->attislocal = true;

			simple_heap_update(catalogRelation, &copyTuple->t_self, copyTuple);
			CatalogUpdateIndexes(catalogRelation, copyTuple);
			heap_freetuple(copyTuple);
		}
	}
	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);

	/*
	 * Drop the dependency
	 *
	 * There's no convenient way to do this, so go trawling through pg_depend
	 */
	catalogRelation = heap_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(catalogRelation, DependDependerIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(depTuple);

		if (dep->refclassid == RelationRelationId &&
			dep->refobjid == RelationGetRelid(parent_rel) &&
			dep->refobjsubid == 0 &&
			dep->deptype == DEPENDENCY_NORMAL)
			simple_heap_delete(catalogRelation, &depTuple->t_self);
	}

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);

	/* keep our lock on the parent relation until commit */
	heap_close(parent_rel, NoLock);
}


/*
 * Execute ALTER TABLE SET SCHEMA
 *
 * Note: caller must have checked ownership of the relation already
 */
void
AlterTableNamespace(RangeVar *relation, const char *newschema)
{
	Relation	rel;
	Oid			relid;
	Oid			oldNspOid;
	Oid			nspOid;
	Relation	classRel;

	rel = relation_openrv(relation, AccessExclusiveLock);

	relid = RelationGetRelid(rel);
	oldNspOid = RelationGetNamespace(rel);

	/* Can we change the schema of this tuple? */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
			/* ok to change schema */
			break;
		case RELKIND_SEQUENCE:
			{
				/* if it's an owned sequence, disallow moving it by itself */
				Oid			tableId;
				int32		colId;

				if (sequenceIsOwned(relid, &tableId, &colId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot move an owned sequence into another schema"),
					  errdetail("Sequence \"%s\" is linked to table \"%s\".",
								RelationGetRelationName(rel),
								get_rel_name(tableId))));
			}
			break;
		case RELKIND_COMPOSITE_TYPE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a composite type",
							RelationGetRelationName(rel)),
					 errhint("Use ALTER TYPE instead.")));
			break;
		case RELKIND_INDEX:
		case RELKIND_TOASTVALUE:
			/* FALL THRU */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table, view, or sequence",
							RelationGetRelationName(rel))));
	}

	/* get schema OID and check its permissions */
	nspOid = LookupCreationNamespace(newschema);

	if (oldNspOid == nspOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" is already in schema \"%s\"",
						RelationGetRelationName(rel),
						newschema)));

	/* disallow renaming into or out of temp schemas */
	if (isAnyTempNamespace(nspOid) || isAnyTempNamespace(oldNspOid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("cannot move objects into or out of temporary schemas")));

	/* same for TOAST schema */
	if (nspOid == PG_TOAST_NAMESPACE || oldNspOid == PG_TOAST_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of TOAST schema")));

	/* OK, modify the pg_class row and pg_depend entry */
	classRel = heap_open(RelationRelationId, RowExclusiveLock);

	AlterRelationNamespaceInternal(classRel, relid, oldNspOid, nspOid, true);

	/* Fix the table's rowtype too */
	AlterTypeNamespaceInternal(rel->rd_rel->reltype, nspOid, false, false);

	/* Fix other dependent stuff */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		AlterIndexNamespaces(classRel, rel, oldNspOid, nspOid);
		AlterSeqNamespaces(classRel, rel, oldNspOid, nspOid, newschema);
		AlterConstraintNamespaces(relid, oldNspOid, nspOid, false);
	}

	heap_close(classRel, RowExclusiveLock);

	/* close rel, but keep lock until commit */
	relation_close(rel, NoLock);
}

/*
 * The guts of relocating a relation to another namespace: fix the pg_class
 * entry, and the pg_depend entry if any.  Caller must already have
 * opened and write-locked pg_class.
 */
void
AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry)
{
	HeapTuple	classTup;
	Form_pg_class classForm;

	classTup = SearchSysCacheCopy(RELOID,
								  ObjectIdGetDatum(relOid),
								  0, 0, 0);
	if (!HeapTupleIsValid(classTup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	Assert(classForm->relnamespace == oldNspOid);

	/* check for duplicate name (more friendly than unique-index failure) */
	if (get_relname_relid(NameStr(classForm->relname),
						  newNspOid) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists in schema \"%s\"",
						NameStr(classForm->relname),
						get_namespace_name(newNspOid))));

	/* classTup is a copy, so OK to scribble on */
	classForm->relnamespace = newNspOid;

	simple_heap_update(classRel, &classTup->t_self, classTup);
	CatalogUpdateIndexes(classRel, classTup);

	/* Update dependency on schema if caller said so */
	if (hasDependEntry &&
		changeDependencyFor(RelationRelationId, relOid,
							NamespaceRelationId, oldNspOid, newNspOid) != 1)
		elog(ERROR, "failed to change schema dependency for relation \"%s\"",
			 NameStr(classForm->relname));

	heap_freetuple(classTup);
}

/*
 * Move all indexes for the specified relation to another namespace.
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 */
static void
AlterIndexNamespaces(Relation classRel, Relation rel,
					 Oid oldNspOid, Oid newNspOid)
{
	List	   *indexList;
	ListCell   *l;

	indexList = RelationGetIndexList(rel);

	foreach(l, indexList)
	{
		Oid			indexOid = lfirst_oid(l);

		/*
		 * Note: currently, the index will not have its own dependency on the
		 * namespace, so we don't need to do changeDependencyFor(). There's no
		 * rowtype in pg_type, either.
		 */
		AlterRelationNamespaceInternal(classRel, indexOid,
									   oldNspOid, newNspOid,
									   false);
	}

	list_free(indexList);
}

/*
 * Move all SERIAL-column sequences of the specified relation to another
 * namespace.
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 */
static void
AlterSeqNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid, const char *newNspName)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL sequences are those having an auto dependency on one of the
	 * table's columns (we don't care *which* column, exactly).
	 */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	/* we leave refobjsubid unspecified */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* skip dependencies other than auto dependencies on columns */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			depForm->deptype != DEPENDENCY_AUTO)
			continue;

		/* Use relation_open just in case it's an index */
		seqRel = relation_open(depForm->objid, AccessExclusiveLock);

		/* skip non-sequence relations */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			relation_close(seqRel, AccessExclusiveLock);
			continue;
		}

		/* Fix the pg_class and pg_depend entries */
		AlterRelationNamespaceInternal(classRel, depForm->objid,
									   oldNspOid, newNspOid,
									   true);

		/*
		 * Sequences have entries in pg_type. We need to be careful to move
		 * them to the new namespace, too.
		 */
		AlterTypeNamespaceInternal(RelationGetForm(seqRel)->reltype,
								   newNspOid, false, false);

		/* Now we can close it.  Keep the lock till end of transaction. */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}


/*
 * This code supports
 *	CREATE TEMP TABLE ... ON COMMIT { DROP | PRESERVE ROWS | DELETE ROWS }
 *
 * Because we only support this for TEMP tables, it's sufficient to remember
 * the state in a backend-local data structure.
 */

/*
 * Register a newly-created relation's ON COMMIT action.
 */
void
register_on_commit_action(Oid relid, OnCommitAction action)
{
	OnCommitItem *oc;
	MemoryContext oldcxt;

	/*
	 * We needn't bother registering the relation unless there is an ON COMMIT
	 * action we need to take.
	 */
	if (action == ONCOMMIT_NOOP || action == ONCOMMIT_PRESERVE_ROWS)
		return;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	oc = (OnCommitItem *) palloc(sizeof(OnCommitItem));
	oc->relid = relid;
	oc->oncommit = action;
	oc->creating_subid = GetCurrentSubTransactionId();
	oc->deleting_subid = InvalidSubTransactionId;

	on_commits = lcons(oc, on_commits);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Unregister any ON COMMIT action when a relation is deleted.
 *
 * Actually, we only mark the OnCommitItem entry as to be deleted after commit.
 */
void
remove_on_commit_action(Oid relid)
{
	ListCell   *l;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		if (oc->relid == relid)
		{
			oc->deleting_subid = GetCurrentSubTransactionId();
			break;
		}
	}
}

/*
 * Perform ON COMMIT actions.
 *
 * This is invoked just before actually committing, since it's possible
 * to encounter errors.
 */
void
PreCommit_on_commit_actions(void)
{
	ListCell   *l;
	List	   *oids_to_truncate = NIL;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		/* Ignore entry if already dropped in this xact */
		if (oc->deleting_subid != InvalidSubTransactionId)
			continue;

		switch (oc->oncommit)
		{
			case ONCOMMIT_NOOP:
			case ONCOMMIT_PRESERVE_ROWS:
				/* Do nothing (there shouldn't be such entries, actually) */
				break;
			case ONCOMMIT_DELETE_ROWS:
				oids_to_truncate = lappend_oid(oids_to_truncate, oc->relid);
				break;
			case ONCOMMIT_DROP:
				{
					ObjectAddress object;

					object.classId = RelationRelationId;
					object.objectId = oc->relid;
					object.objectSubId = 0;
					performDeletion(&object, DROP_CASCADE);

					/*
					 * Note that table deletion will call
					 * remove_on_commit_action, so the entry should get marked
					 * as deleted.
					 */
					Assert(oc->deleting_subid != InvalidSubTransactionId);
					break;
				}
		}
	}
	if (oids_to_truncate != NIL)
	{
		heap_truncate(oids_to_truncate);
		CommandCounterIncrement();		/* XXX needed? */
	}
}

/*
 * Post-commit or post-abort cleanup for ON COMMIT management.
 *
 * All we do here is remove no-longer-needed OnCommitItem entries.
 *
 * During commit, remove entries that were deleted during this transaction;
 * during abort, remove those created during this transaction.
 */
void
AtEOXact_on_commit_actions(bool isCommit)
{
	ListCell   *cur_item;
	ListCell   *prev_item;

	prev_item = NULL;
	cur_item = list_head(on_commits);

	while (cur_item != NULL)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (isCommit ? oc->deleting_subid != InvalidSubTransactionId :
			oc->creating_subid != InvalidSubTransactionId)
		{
			/* cur_item must be removed */
			on_commits = list_delete_cell(on_commits, cur_item, prev_item);
			pfree(oc);
			if (prev_item)
				cur_item = lnext(prev_item);
			else
				cur_item = list_head(on_commits);
		}
		else
		{
			/* cur_item must be preserved */
			oc->creating_subid = InvalidSubTransactionId;
			oc->deleting_subid = InvalidSubTransactionId;
			prev_item = cur_item;
			cur_item = lnext(prev_item);
		}
	}
}

/*
 * Post-subcommit or post-subabort cleanup for ON COMMIT management.
 *
 * During subabort, we can immediately remove entries created during this
 * subtransaction.	During subcommit, just relabel entries marked during
 * this subtransaction as being the parent's responsibility.
 */
void
AtEOSubXact_on_commit_actions(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid)
{
	ListCell   *cur_item;
	ListCell   *prev_item;

	prev_item = NULL;
	cur_item = list_head(on_commits);

	while (cur_item != NULL)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (!isCommit && oc->creating_subid == mySubid)
		{
			/* cur_item must be removed */
			on_commits = list_delete_cell(on_commits, cur_item, prev_item);
			pfree(oc);
			if (prev_item)
				cur_item = lnext(prev_item);
			else
				cur_item = list_head(on_commits);
		}
		else
		{
			/* cur_item must be preserved */
			if (oc->creating_subid == mySubid)
				oc->creating_subid = parentSubid;
			if (oc->deleting_subid == mySubid)
				oc->deleting_subid = isCommit ? parentSubid : InvalidSubTransactionId;
			prev_item = cur_item;
			cur_item = lnext(prev_item);
		}
	}
}
