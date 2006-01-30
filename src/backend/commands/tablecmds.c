/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  Commands for creating and altering table structures and settings
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/tablecmds.c,v 1.174.2.2 2006/01/30 16:19:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/tuptoaster.h"
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
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parser.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
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
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
	/* Objects to rebuild after completing ALTER TYPE operations */
	List	   *changedConstraintOids;	/* OIDs of constraints to rebuild */
	List	   *changedConstraintDefs;	/* string definitions of same */
	List	   *changedIndexOids;		/* OIDs of indexes to rebuild */
	List	   *changedIndexDefs;		/* string definitions of same */
} AlteredTableInfo;

/* Struct describing one new constraint to check in Phase 3 scan */
typedef struct NewConstraint
{
	char	   *name;			/* Constraint name, or NULL if none */
	ConstrType	contype;		/* CHECK, NOT_NULL, or FOREIGN */
	AttrNumber	attnum;			/* only relevant for NOT_NULL */
	Oid			refrelid;		/* PK rel, if FOREIGN */
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


static List *MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr, int *supOidCount);
static bool change_varattnos_of_a_node(Node *node, const AttrNumber *newattno);
static void StoreCatalogInheritance(Oid relationId, List *supers);
static int	findAttrByName(const char *attributeName, List *schema);
static void setRelhassubclassInRelation(Oid relationId, bool relhassubclass);
static bool needs_toast_table(Relation rel);
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
							 Relation rel, Relation pkrel);
static void createForeignKeyTriggers(Relation rel, FkConstraint *fkconstraint,
						 Oid constrOid);
static char *fkMatchTypeToString(char match_type);
static void ATController(Relation rel, List *cmds, bool recurse);
static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing);
static void ATRewriteCatalogs(List **wqueue);
static void ATExecCmd(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd);
static void ATRewriteTables(List **wqueue);
static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap);
static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
static void ATSimplePermissions(Relation rel, bool allowView);
static void ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse);
static void ATOneLevelRecursion(List **wqueue, Relation rel,
					AlterTableCmd *cmd);
static void find_composite_type_dependencies(Oid typeOid,
								 const char *origTblName);
static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse,
				AlterTableCmd *cmd);
static void ATExecAddColumn(AlteredTableInfo *tab, Relation rel,
				ColumnDef *colDef);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void add_column_support_dependency(Oid relid, int32 attnum,
							  RangeVar *support);
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
static void ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing);
static void change_owner_recurse_to_sequences(Oid relationOid,
								  Oid newOwnerId);
static void ATExecClusterOn(Relation rel, const char *indexName);
static void ATExecDropCluster(Relation rel);
static void ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel,
					char *tablespacename);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace);
static void ATExecEnableDisableTrigger(Relation rel, char *trigname,
						   bool enable, bool skip_system);
static void copy_relation_data(Relation rel, SMgrRelation dst);
static void update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname);


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
	ListCell   *listptr;
	int			i;
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
	 * Select tablespace to use.  If not specified, use default_tablespace
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
		tablespaceId = GetDefaultTablespace();
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

	localHasOids = interpretOidsOption(stmt->hasoids);
	descriptor->tdhasoid = (localHasOids || parentOidCount > 0);

	if (old_constraints != NIL)
	{
		ConstrCheck *check = (ConstrCheck *)
		palloc0(list_length(old_constraints) * sizeof(ConstrCheck));
		int			ncheck = 0;

		foreach(listptr, old_constraints)
		{
			Constraint *cdef = (Constraint *) lfirst(listptr);
			bool		dup = false;

			if (cdef->contype != CONSTR_CHECK)
				continue;
			Assert(cdef->name != NULL);
			Assert(cdef->raw_expr == NULL && cdef->cooked_expr != NULL);

			/*
			 * In multiple-inheritance situations, it's possible to inherit
			 * the same grandparent constraint through multiple parents.
			 * Hence, discard inherited constraints that match as to both name
			 * and expression.	Otherwise, gripe if the names conflict.
			 */
			for (i = 0; i < ncheck; i++)
			{
				if (strcmp(check[i].ccname, cdef->name) != 0)
					continue;
				if (strcmp(check[i].ccbin, cdef->cooked_expr) == 0)
				{
					dup = true;
					break;
				}
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("duplicate check constraint name \"%s\"",
								cdef->name)));
			}
			if (!dup)
			{
				check[ncheck].ccname = cdef->name;
				check[ncheck].ccbin = pstrdup(cdef->cooked_expr);
				ncheck++;
			}
		}
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
	 * Another task that's conveniently done at this step is to add dependency
	 * links between columns and supporting relations (such as SERIAL
	 * sequences).
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

		/* Create dependency for supporting relation for this column */
		if (colDef->support != NULL)
			add_column_support_dependency(relationId, attnum, colDef->support);
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
 * This is a multi-relation truncate.  It first opens and grabs exclusive
 * locks on all relations involved, checking permissions and otherwise
 * verifying that the relation is OK for truncation.  When they are all
 * open, it checks foreign key references on them, namely that FK references
 * are all internal to the group that's being truncated.  Finally all
 * relations are truncated and reindexed.
 */
void
ExecuteTruncate(List *relations)
{
	List	   *rels = NIL;
	ListCell   *cell;

	foreach(cell, relations)
	{
		RangeVar   *rv = lfirst(cell);
		Relation	rel;

		/* Grab exclusive lock in preparation for truncate */
		rel = heap_openrv(rv, AccessExclusiveLock);

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
		 * We can never allow truncation of shared or nailed-in-cache
		 * relations, because we can't support changing their relfilenode
		 * values.
		 */
		if (rel->rd_rel->relisshared || rel->rd_isnailed)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot truncate system relation \"%s\"",
							RelationGetRelationName(rel))));

		/*
		 * Don't allow truncate on temp tables of other backends ... their
		 * local buffer manager is not going to cope.
		 */
		if (isOtherTempNamespace(RelationGetNamespace(rel)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("cannot truncate temporary tables of other sessions")));

		/* Save it into the list of rels to truncate */
		rels = lappend(rels, rel);
	}

	/*
	 * Check foreign key references.
	 */
	heap_truncate_check_FKs(rels, false);

	/*
	 * OK, truncate each table.
	 */
	foreach(cell, rels)
	{
		Relation	rel = lfirst(cell);
		Oid			heap_relid;
		Oid			toast_relid;

		/*
		 * Create a new empty storage file for the relation, and assign it as
		 * the relfilenode value.	The old storage file is scheduled for
		 * deletion at commit.
		 */
		setNewRelfilenode(rel);

		heap_relid = RelationGetRelid(rel);
		toast_relid = rel->rd_rel->reltoastrelid;

		heap_close(rel, NoLock);

		/*
		 * The same for the toast table, if any.
		 */
		if (OidIsValid(toast_relid))
		{
			rel = relation_open(toast_relid, AccessExclusiveLock);
			setNewRelfilenode(rel);
			heap_close(rel, NoLock);
		}

		/*
		 * Reconstruct the indexes to match, and we're done.
		 */
		reindex_relation(heap_relid, true);
	}
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
						 errmsg("column \"%s\" duplicated",
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
					 errmsg("inherited relation \"%s\" duplicated",
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
			TypeName   *typename;

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
				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				ereport(NOTICE,
						(errmsg("merging multiple inherited definitions of column \"%s\"",
								attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				if (typenameTypeId(def->typename) != attribute->atttypid ||
					def->typename->typmod != attribute->atttypmod)
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
				typename = makeNode(TypeName);
				typename->typeid = attribute->atttypid;
				typename->typmod = attribute->atttypmod;
				def->typename = typename;
				def->inhcount = 1;
				def->is_local = false;
				def->is_not_null = attribute->attnotnull;
				def->raw_default = NULL;
				def->cooked_default = NULL;
				def->constraints = NIL;
				def->support = NULL;
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

				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type and typmod.
				 */
				ereport(NOTICE,
				   (errmsg("merging column \"%s\" with inherited definition",
						   attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				if (typenameTypeId(def->typename) != typenameTypeId(newdef->typename) ||
					def->typename->typmod != newdef->typename->typmod)
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
 * complementary static functions for MergeAttributes().
 *
 * Varattnos of pg_constraint.conbin must be rewritten when subclasses inherit
 * constraints from parent classes, since the inherited attributes could
 * be given different column numbers in multiple-inheritance cases.
 *
 * Note that the passed node tree is modified in place!
 */
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

static bool
change_varattnos_of_a_node(Node *node, const AttrNumber *newattno)
{
	return change_varattnos_walker(node, newattno);
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
	TupleDesc	desc;
	int16		seqNumber;
	ListCell   *entry;
	HeapTuple	tuple;

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
	desc = RelationGetDescr(relation);

	seqNumber = 1;
	foreach(entry, supers)
	{
		Oid			parentOid = lfirst_oid(entry);
		Datum		datum[Natts_pg_inherits];
		char		nullarr[Natts_pg_inherits];
		ObjectAddress childobject,
					parentobject;

		datum[0] = ObjectIdGetDatum(relationId);		/* inhrel */
		datum[1] = ObjectIdGetDatum(parentOid); /* inhparent */
		datum[2] = Int16GetDatum(seqNumber);	/* inhseqno */

		nullarr[0] = ' ';
		nullarr[1] = ' ';
		nullarr[2] = ' ';

		tuple = heap_formtuple(desc, datum, nullarr);

		simple_heap_insert(relation, tuple);

		CatalogUpdateIndexes(relation, tuple);

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

		seqNumber += 1;
	}

	heap_close(relation, RowExclusiveLock);
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

	/*
	 * Update att name in any RI triggers associated with the relation.
	 */
	if (targetrelation->rd_rel->reltriggers > 0)
	{
		/* update tgargs column reference where att is primary key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   false, false);
		/* update tgargs column reference where att is foreign key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   true, false);
	}

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
renamerel(Oid myrelid, const char *newrelname)
{
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	reltup;
	Oid			namespaceId;
	char	   *oldrelname;
	char		relkind;
	bool		relhastriggers;

	/*
	 * Grab an exclusive lock on the target table or index, which we will NOT
	 * release until end of transaction.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);

	oldrelname = pstrdup(RelationGetRelationName(targetrelation));
	namespaceId = RelationGetNamespace(targetrelation);

	if (!allowSystemTableMods && IsSystemRelation(targetrelation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(targetrelation))));

	relkind = targetrelation->rd_rel->relkind;
	relhastriggers = (targetrelation->rd_rel->reltriggers > 0);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in use.
	 */
	relrelation = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								PointerGetDatum(myrelid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						newrelname)));

	/*
	 * Update pg_class tuple with new relname.	(Scribbling on reltup is OK
	 * because it's a copy...)
	 */
	namestrcpy(&(((Form_pg_class) GETSTRUCT(reltup))->relname), newrelname);

	simple_heap_update(relrelation, &reltup->t_self, reltup);

	/* keep the system catalog indexes current */
	CatalogUpdateIndexes(relrelation, reltup);

	heap_freetuple(reltup);
	heap_close(relrelation, RowExclusiveLock);

	/*
	 * Also rename the associated type, if any.
	 */
	if (relkind != RELKIND_INDEX)
		TypeRename(oldrelname, namespaceId, newrelname);

	/*
	 * Update rel name in any RI triggers associated with the relation.
	 */
	if (relhastriggers)
	{
		/* update tgargs where relname is primary key */
		update_ri_trigger_args(myrelid,
							   oldrelname,
							   newrelname,
							   false, true);
		/* update tgargs where relname is foreign key */
		update_ri_trigger_args(myrelid,
							   oldrelname,
							   newrelname,
							   true, true);
	}

	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrelation, NoLock);
}

/*
 * Scan pg_trigger for RI triggers that are on the specified relation
 * (if fk_scan is false) or have it as the tgconstrrel (if fk_scan
 * is true).  Update RI trigger args fields matching oldname to contain
 * newname instead.  If update_relname is true, examine the relname
 * fields; otherwise examine the attname fields.
 */
static void
update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname)
{
	Relation	tgrel;
	ScanKeyData skey[1];
	SysScanDesc trigscan;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	char		nulls[Natts_pg_trigger];
	char		replaces[Natts_pg_trigger];

	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);
	if (fk_scan)
	{
		ScanKeyInit(&skey[0],
					Anum_pg_trigger_tgconstrrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		trigscan = systable_beginscan(tgrel, TriggerConstrRelidIndexId,
									  true, SnapshotNow,
									  1, skey);
	}
	else
	{
		ScanKeyInit(&skey[0],
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relid));
		trigscan = systable_beginscan(tgrel, TriggerRelidNameIndexId,
									  true, SnapshotNow,
									  1, skey);
	}

	while ((tuple = systable_getnext(trigscan)) != NULL)
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);
		bytea	   *val;
		bytea	   *newtgargs;
		bool		isnull;
		int			tg_type;
		bool		examine_pk;
		bool		changed;
		int			tgnargs;
		int			i;
		int			newlen;
		const char *arga[RI_MAX_ARGUMENTS];
		const char *argp;

		tg_type = RI_FKey_trigger_type(pg_trigger->tgfoid);
		if (tg_type == RI_TRIGGER_NONE)
		{
			/* Not an RI trigger, forget it */
			continue;
		}

		/*
		 * It is an RI trigger, so parse the tgargs bytea.
		 *
		 * NB: we assume the field will never be compressed or moved out of
		 * line; so does trigger.c ...
		 */
		tgnargs = pg_trigger->tgnargs;
		val = (bytea *)
			DatumGetPointer(fastgetattr(tuple,
										Anum_pg_trigger_tgargs,
										tgrel->rd_att, &isnull));
		if (isnull || tgnargs < RI_FIRST_ATTNAME_ARGNO ||
			tgnargs > RI_MAX_ARGUMENTS)
		{
			/* This probably shouldn't happen, but ignore busted triggers */
			continue;
		}
		argp = (const char *) VARDATA(val);
		for (i = 0; i < tgnargs; i++)
		{
			arga[i] = argp;
			argp += strlen(argp) + 1;
		}

		/*
		 * Figure out which item(s) to look at.  If the trigger is primary-key
		 * type and attached to my rel, I should look at the PK fields; if it
		 * is foreign-key type and attached to my rel, I should look at the FK
		 * fields.	But the opposite rule holds when examining triggers found
		 * by tgconstrrel search.
		 */
		examine_pk = (tg_type == RI_TRIGGER_PK) == (!fk_scan);

		changed = false;
		if (update_relname)
		{
			/* Change the relname if needed */
			i = examine_pk ? RI_PK_RELNAME_ARGNO : RI_FK_RELNAME_ARGNO;
			if (strcmp(arga[i], oldname) == 0)
			{
				arga[i] = newname;
				changed = true;
			}
		}
		else
		{
			/* Change attname(s) if needed */
			i = examine_pk ? RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_PK_IDX :
				RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_FK_IDX;
			for (; i < tgnargs; i += 2)
			{
				if (strcmp(arga[i], oldname) == 0)
				{
					arga[i] = newname;
					changed = true;
				}
			}
		}

		if (!changed)
		{
			/* Don't need to update this tuple */
			continue;
		}

		/*
		 * Construct modified tgargs bytea.
		 */
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
			newlen += strlen(arga[i]) + 1;
		newtgargs = (bytea *) palloc(newlen);
		VARATT_SIZEP(newtgargs) = newlen;
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
		{
			strcpy(((char *) newtgargs) + newlen, arga[i]);
			newlen += strlen(arga[i]) + 1;
		}

		/*
		 * Build modified tuple.
		 */
		for (i = 0; i < Natts_pg_trigger; i++)
		{
			values[i] = (Datum) 0;
			replaces[i] = ' ';
			nulls[i] = ' ';
		}
		values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum(newtgargs);
		replaces[Anum_pg_trigger_tgargs - 1] = 'r';

		tuple = heap_modifytuple(tuple, RelationGetDescr(tgrel), values, nulls, replaces);

		/*
		 * Update pg_trigger and its indexes
		 */
		simple_heap_update(tgrel, &tuple->t_self, tuple);

		CatalogUpdateIndexes(tgrel, tuple);

		/*
		 * Invalidate trigger's relation's relcache entry so that other
		 * backends (and this one too!) are sent SI message to make them
		 * rebuild relcache entries.  (Ideally this should happen
		 * automatically...)
		 *
		 * We can skip this for triggers on relid itself, since that relcache
		 * flush will happen anyway due to the table or column rename.	We
		 * just need to catch the far ends of RI relationships.
		 */
		pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);
		if (pg_trigger->tgrelid != relid)
			CacheInvalidateRelcacheByRelid(pg_trigger->tgrelid);

		/* free up our scratch memory */
		pfree(newtgargs);
		heap_freetuple(tuple);
	}

	systable_endscan(trigscan);

	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Increment cmd counter to make updates visible; this is needed in case
	 * the same tuple has to be updated again by next pass (can happen in case
	 * of a self-referential FK relationship).
	 */
	CommandCounterIncrement();
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
	ATController(relation_openrv(stmt->relation, AccessExclusiveLock),
				 stmt->cmds,
				 interpretInhOption(stmt->relation->inhOpt));
}

/*
 * AlterTableInternal
 *
 * ALTER TABLE with target specified by OID
 */
void
AlterTableInternal(Oid relid, List *cmds, bool recurse)
{
	ATController(relation_open(relid, AccessExclusiveLock),
				 cmds,
				 recurse);
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
			pass = AT_PASS_ADD_CONSTR;
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
		case AT_ToastTable:		/* CREATE TOAST TABLE */
			ATSimplePermissions(rel, false);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
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
			/* This command never recurses */
			ATPrepSetTableSpace(tab, rel, cmd->name);
			pass = AT_PASS_MISC;	/* doesn't actually matter */
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER variants */
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* DISABLE TRIGGER variants */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
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
	 * Do an implicit CREATE TOAST TABLE if we executed any subcommands that
	 * might have added a column or changed column storage.
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		if (tab->relkind == RELKIND_RELATION &&
			(tab->subcmds[AT_PASS_ADD_COL] ||
			 tab->subcmds[AT_PASS_ALTER_TYPE] ||
			 tab->subcmds[AT_PASS_COL_ATTRS]))
			AlterTableCreateToastTable(tab->relid, true);
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
		case AT_ToastTable:		/* CREATE TOAST TABLE */
			AlterTableCreateToastTable(RelationGetRelid(rel), false);
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
		case AT_EnableTrig:		/* ENABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name, true, false);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name, false, false);
			break;
		case AT_EnableTrigAll:	/* ENABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL, true, false);
			break;
		case AT_DisableTrigAll:	/* DISABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL, false, false);
			break;
		case AT_EnableTrigUser:	/* ENABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL, true, true);
			break;
		case AT_DisableTrigUser:		/* DISABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL, false, true);
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

			/* Swap the physical files of the old and new heaps. */
			swap_relation_files(tab->relid, OIDNewHeap);

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
			if (tab->constraints != NIL)
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

				validateForeignKeyConstraint(fkconstraint, rel, refrel);

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
										 RelationGetRelationName(oldrel));

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
			case CONSTR_NOTNULL:
				needscan = true;
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
					case CONSTR_NOTNULL:
						{
							Datum		d;
							bool		isnull;

							d = heap_getattr(tuple, con->attnum, newTupDesc,
											 &isnull);
							if (isnull)
								ereport(ERROR,
										(errcode(ERRCODE_NOT_NULL_VIOLATION),
								 errmsg("column \"%s\" contains null values",
										get_attname(tab->relid,
													con->attnum))));
						}
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
		ATPrepCmd(wqueue, childrel, cmd, true, true);
		relation_close(childrel, NoLock);
	}
}


/*
 * find_composite_type_dependencies
 *
 * Check to see if a table's rowtype is being used as a column in some
 * other table (possibly nested several levels deep in composite types!).
 * Eventually, we'd like to propagate the check or rewrite operation
 * into other such tables, but for now, just error out if we find any.
 *
 * We assume that functions and views depending on the type are not reasons
 * to reject the ALTER.  (How safe is this really?)
 */
static void
find_composite_type_dependencies(Oid typeOid, const char *origTblName)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

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
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter table \"%s\" because column \"%s\".\"%s\" uses its rowtype",
							origTblName,
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
											 origTblName);
		}

		relation_close(rel, AccessShareLock);
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);
}


/*
 * ALTER TABLE ADD COLUMN
 *
 * Adds an additional attribute to a relation making the assumption that
 * CHECK, NOT NULL, and FOREIGN KEY constraints will be removed from the
 * AT_AddColumn AlterTableCmd by analyze.c and added as independent
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
		/* and don't make a support dependency on the child */
		colDefChild->support = NULL;

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

			/* Okay if child matches by type */
			if (typenameTypeId(colDef->typename) != childatt->atttypid ||
				colDef->typename->typmod != childatt->atttypmod)
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

	typeTuple = typenameType(colDef->typename);
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
	attribute->atttypmod = colDef->typename->typmod;
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
		Oid			basetype = getBaseType(typeOid);

		defval = (Expr *) makeNullConst(basetype);
		defval = (Expr *) coerce_to_target_type(NULL,
												(Node *) defval,
												basetype,
												typeOid,
												colDef->typename->typmod,
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
	if (colDef->support != NULL)
		add_column_support_dependency(myrelid, i, colDef->support);
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
 * Install a dependency for a column's supporting relation (serial sequence).
 */
static void
add_column_support_dependency(Oid relid, int32 attnum, RangeVar *support)
{
	ObjectAddress colobject,
				suppobject;

	colobject.classId = RelationRelationId;
	colobject.objectId = relid;
	colobject.objectSubId = attnum;
	suppobject.classId = RelationRelationId;
	suppobject.objectId = RangeVarGetRelid(support, false);
	suppobject.objectSubId = 0;
	recordDependencyOn(&suppobject, &colobject, DEPENDENCY_INTERNAL);
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
	NewConstraint *newcon;

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

		/* Tell Phase 3 to test the constraint */
		newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
		newcon->contype = CONSTR_NOTNULL;
		newcon->attnum = attnum;
		newcon->name = "NOT NULL";

		tab->constraints = lappend(tab->constraints, newcon);
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
 * There is no such command in the grammar, but the parser converts UNIQUE
 * and PRIMARY KEY constraints into AT_AddIndex subcommands.  This lets us
 * schedule creation of the index at the appropriate time during ALTER.
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

	DefineIndex(stmt->relation, /* relation */
				stmt->idxname,	/* index name */
				InvalidOid,		/* no predefined OID */
				stmt->accessMethod,		/* am name */
				stmt->tableSpace,
				stmt->indexParams,		/* parameters */
				(Expr *) stmt->whereClause,
				stmt->rangetable,
				stmt->unique,
				stmt->primary,
				stmt->isconstraint,
				true,			/* is_alter_table */
				check_rights,
				skip_build,
				quiet);
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
				 * parser/analyze.c).  Use a switch anyway to make it easier
				 * to add more code later.
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
								newcon->attnum = ccon->attnum;
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

	/* Be sure referencing and referenced column types are comparable */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	for (i = 0; i < numpks; i++)
	{
		/*
		 * pktypoid[i] is the primary key table's i'th key's type fktypoid[i]
		 * is the foreign key table's i'th key's type
		 *
		 * Note that we look for an operator with the PK type on the left;
		 * when the types are different this is critical because the PK index
		 * will need operators with the indexkey on the left. (Ordinarily both
		 * commutator operators will exist if either does, but we won't get
		 * the right answer from the test below on opclass membership unless
		 * we select the proper operator.)
		 */
		Operator	o = oper(list_make1(makeString("=")),
							 pktypoid[i], fktypoid[i], true);

		if (o == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("foreign key constraint \"%s\" "
							"cannot be implemented",
							fkconstraint->constr_name),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of incompatible types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktypoid[i]),
							   format_type_be(pktypoid[i]))));

		/*
		 * Check that the found operator is compatible with the PK index, and
		 * generate a warning if not, since otherwise costly seqscans will be
		 * incurred to check FK validity.
		 */
		if (!op_in_opclass(oprid(o), opclasses[i]))
			ereport(WARNING,
					(errmsg("foreign key constraint \"%s\" "
							"will require costly sequential scans",
							fkconstraint->constr_name),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of different types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktypoid[i]),
							   format_type_be(pktypoid[i]))));

		ReleaseSysCache(o);
	}

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
		newcon->qual = (Node *) fkconstraint;

		tab->constraints = lappend(tab->constraints, newcon);
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
							 Relation pkrel)
{
	HeapScanDesc scan;
	HeapTuple	tuple;
	Trigger		trig;
	ListCell   *list;
	int			count;

	/*
	 * See if we can do it with a single LEFT JOIN query.  A FALSE result
	 * indicates we must proceed with the fire-the-trigger method.
	 */
	if (RI_Initial_Check(fkconstraint, rel, pkrel))
		return;

	/*
	 * Scan through each tuple, calling RI_FKey_check_ins (insert trigger) as
	 * if that tuple had just been inserted.  If any of those fail, it should
	 * ereport(ERROR) and that's that.
	 */
	MemSet(&trig, 0, sizeof(trig));
	trig.tgoid = InvalidOid;
	trig.tgname = fkconstraint->constr_name;
	trig.tgenabled = TRUE;
	trig.tgisconstraint = TRUE;
	trig.tgconstrrelid = RelationGetRelid(pkrel);
	trig.tgdeferrable = FALSE;
	trig.tginitdeferred = FALSE;

	trig.tgargs = (char **) palloc(sizeof(char *) *
								   (4 + list_length(fkconstraint->fk_attrs)
									+ list_length(fkconstraint->pk_attrs)));

	trig.tgargs[0] = trig.tgname;
	trig.tgargs[1] = RelationGetRelationName(rel);
	trig.tgargs[2] = RelationGetRelationName(pkrel);
	trig.tgargs[3] = fkMatchTypeToString(fkconstraint->fk_matchtype);
	count = 4;
	foreach(list, fkconstraint->fk_attrs)
	{
		char	   *fk_at = strVal(lfirst(list));

		trig.tgargs[count] = fk_at;
		count += 2;
	}
	count = 5;
	foreach(list, fkconstraint->pk_attrs)
	{
		char	   *pk_at = strVal(lfirst(list));

		trig.tgargs[count] = pk_at;
		count += 2;
	}
	trig.tgnargs = count - 1;

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

	pfree(trig.tgargs);
}

static void
CreateFKCheckTrigger(RangeVar *myRel, FkConstraint *fkconstraint,
					 ObjectAddress *constrobj, ObjectAddress *trigobj,
					 bool on_insert)
{
	CreateTrigStmt *fk_trigger;
	ListCell   *fk_attr;
	ListCell   *pk_attr;

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
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->constr_name));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(myRel->relname));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->pktable->relname));
	fk_trigger->args = lappend(fk_trigger->args,
				makeString(fkMatchTypeToString(fkconstraint->fk_matchtype)));
	if (list_length(fkconstraint->fk_attrs) != list_length(fkconstraint->pk_attrs))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	forboth(fk_attr, fkconstraint->fk_attrs,
			pk_attr, fkconstraint->pk_attrs)
	{
		fk_trigger->args = lappend(fk_trigger->args, lfirst(fk_attr));
		fk_trigger->args = lappend(fk_trigger->args, lfirst(pk_attr));
	}

	trigobj->objectId = CreateTrigger(fk_trigger, true);

	/* Register dependency from trigger to constraint */
	recordDependencyOn(trigobj, constrobj, DEPENDENCY_INTERNAL);

	/* Make changes-so-far visible */
	CommandCounterIncrement();
}

/*
 * Create the triggers that implement an FK constraint.
 */
static void
createForeignKeyTriggers(Relation rel, FkConstraint *fkconstraint,
						 Oid constrOid)
{
	RangeVar   *myRel;
	CreateTrigStmt *fk_trigger;
	ListCell   *fk_attr;
	ListCell   *pk_attr;
	ObjectAddress trigobj,
				constrobj;

	/*
	 * Reconstruct a RangeVar for my relation (not passed in, unfortunately).
	 */
	myRel = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
						 pstrdup(RelationGetRelationName(rel)));

	/*
	 * Preset objectAddress fields
	 */
	constrobj.classId = ConstraintRelationId;
	constrobj.objectId = constrOid;
	constrobj.objectSubId = 0;
	trigobj.classId = TriggerRelationId;
	trigobj.objectSubId = 0;

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the CHECK
	 * action for both INSERTs and UPDATEs on the referencing table.
	 */
	CreateFKCheckTrigger(myRel, fkconstraint, &constrobj, &trigobj, true);
	CreateFKCheckTrigger(myRel, fkconstraint, &constrobj, &trigobj, false);

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
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->constr_name));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(myRel->relname));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->pktable->relname));
	fk_trigger->args = lappend(fk_trigger->args,
				makeString(fkMatchTypeToString(fkconstraint->fk_matchtype)));
	forboth(fk_attr, fkconstraint->fk_attrs,
			pk_attr, fkconstraint->pk_attrs)
	{
		fk_trigger->args = lappend(fk_trigger->args, lfirst(fk_attr));
		fk_trigger->args = lappend(fk_trigger->args, lfirst(pk_attr));
	}

	trigobj.objectId = CreateTrigger(fk_trigger, true);

	/* Register dependency from trigger to constraint */
	recordDependencyOn(&trigobj, &constrobj, DEPENDENCY_INTERNAL);

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
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->constr_name));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(myRel->relname));
	fk_trigger->args = lappend(fk_trigger->args,
							   makeString(fkconstraint->pktable->relname));
	fk_trigger->args = lappend(fk_trigger->args,
				makeString(fkMatchTypeToString(fkconstraint->fk_matchtype)));
	forboth(fk_attr, fkconstraint->fk_attrs,
			pk_attr, fkconstraint->pk_attrs)
	{
		fk_trigger->args = lappend(fk_trigger->args, lfirst(fk_attr));
		fk_trigger->args = lappend(fk_trigger->args, lfirst(pk_attr));
	}

	trigobj.objectId = CreateTrigger(fk_trigger, true);

	/* Register dependency from trigger to constraint */
	recordDependencyOn(&trigobj, &constrobj, DEPENDENCY_INTERNAL);
}

/*
 * fkMatchTypeToString -
 *	  convert FKCONSTR_MATCH_xxx code to string to use in trigger args
 */
static char *
fkMatchTypeToString(char match_type)
{
	switch (match_type)
	{
		case FKCONSTR_MATCH_FULL:
			return pstrdup("FULL");
		case FKCONSTR_MATCH_PARTIAL:
			return pstrdup("PARTIAL");
		case FKCONSTR_MATCH_UNSPECIFIED:
			return pstrdup("UNSPECIFIED");
		default:
			elog(ERROR, "unrecognized match type: %d",
				 (int) match_type);
	}
	return NULL;				/* can't get here */
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
	targettype = LookupTypeName(typename);
	if (!OidIsValid(targettype))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typename))));

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
									  targettype, typename->typmod,
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
	typeTuple = typenameType(typename);
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
											targettype, typename->typmod,
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
					char *defstring = pg_get_constraintdef_string(foundObject.objectId);

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
			case OCLASS_TRIGGER:
			case OCLASS_SCHEMA:

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
	attTup->atttypmod = typename->typmod;
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
	 * have to delete FOREIGN KEY constraints before UNIQUE constraints,
	 * but we already ordered the constraint list to ensure that would happen.
	 * It should be okay to use DROP_RESTRICT here, since nothing else should
	 * be depending on these objects.
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
	 * We expect that we only have to do raw parsing and parse analysis, not
	 * any rule rewriting, since these will all be utility statements.
	 */
	raw_parsetree_list = raw_parser(cmd);
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);

		querytree_list = list_concat(querytree_list,
									 parse_analyze(parsetree, NULL, 0));
	}

	/*
	 * Attach each generated command to the proper place in the work queue.
	 * Note this could result in creation of entirely new work-queue entries.
	 */
	foreach(list_item, querytree_list)
	{
		Query	   *query = (Query *) lfirst(list_item);
		Relation	rel;
		AlteredTableInfo *tab;

		Assert(IsA(query, Query));
		Assert(query->commandType == CMD_UTILITY);
		switch (nodeTag(query->utilityStmt))
		{
			case T_IndexStmt:
				{
					IndexStmt  *stmt = (IndexStmt *) query->utilityStmt;
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
					AlterTableStmt *stmt = (AlterTableStmt *) query->utilityStmt;
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
					 (int) nodeTag(query->utilityStmt));
		}
	}
}


/*
 * ALTER TABLE OWNER
 *
 * recursing is true if we are recursing from a table to its indexes or
 * toast table.  We don't allow the ownership of those things to be
 * changed separately from the parent table.  Also, we can skip permission
 * checks (this is necessary not just an optimization, else we'd fail to
 * handle toast tables properly).
 */
static void
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
		case RELKIND_SEQUENCE:
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

		/* Update owner dependency reference */
		changeDependencyOnOwner(RelationRelationId, relationOid, newOwnerId);

		/*
		 * Also change the ownership of the table's rowtype, if it has one
		 */
		if (tuple_class->relkind != RELKIND_INDEX)
			AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId);

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
	 * SERIAL sequences are those having an internal dependency on one of the
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

		/* skip dependencies other than internal dependencies on columns */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			depForm->deptype != DEPENDENCY_INTERNAL)
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
		ATExecChangeOwner(depForm->objid, newOwnerId, false);

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

	/*
	 * We do our own permission checking because we want to allow this on
	 * indexes.
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

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

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

	rel = relation_open(tableOid, NoLock);

	/*
	 * We can never allow moving of shared or nailed-in-cache relations,
	 * because we can't support changing their reltablespace values.
	 */
	if (rel->rd_rel->relisshared || rel->rd_isnailed)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move system relation \"%s\"",
						RelationGetRelationName(rel))));

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
		{
			xl_heap_newpage xlrec;
			XLogRecPtr	recptr;
			XLogRecData rdata[2];

			/* NO ELOG(ERROR) from here till newpage op is logged */
			START_CRIT_SECTION();

			xlrec.node = dst->smgr_rnode;
			xlrec.blkno = blkno;

			rdata[0].data = (char *) &xlrec;
			rdata[0].len = SizeOfHeapNewpage;
			rdata[0].buffer = InvalidBuffer;
			rdata[0].next = &(rdata[1]);

			rdata[1].data = (char *) page;
			rdata[1].len = BLCKSZ;
			rdata[1].buffer = InvalidBuffer;
			rdata[1].next = NULL;

			recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_NEWPAGE, rdata);

			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);

			END_CRIT_SECTION();
		}

		/*
		 * Now write the page.	We say isTemp = true even if it's not a temp
		 * rel, because there's no need for smgr to schedule an fsync for this
		 * write; we'll do it ourselves below.
		 */
		smgrwrite(dst, blkno, buf, true);
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
						   bool enable, bool skip_system)
{
	EnableDisableTrigger(rel, trigname, enable, skip_system);
}

/*
 * ALTER TABLE CREATE TOAST TABLE
 *
 * Note: this is also invoked from outside this module; in such cases we
 * expect the caller to have verified that the relation is a table and we
 * have all the right permissions.	Callers expect this function
 * to end with CommandCounterIncrement if it makes any changes.
 */
void
AlterTableCreateToastTable(Oid relOid, bool silent)
{
	Relation	rel;
	HeapTuple	reltup;
	TupleDesc	tupdesc;
	bool		shared_relation;
	Relation	class_rel;
	Oid			toast_relid;
	Oid			toast_idxid;
	char		toast_relname[NAMEDATALEN];
	char		toast_idxname[NAMEDATALEN];
	IndexInfo  *indexInfo;
	Oid			classObjectId[2];
	ObjectAddress baseobject,
				toastobject;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.  (This is probably redundant in all present
	 * uses...)
	 */
	rel = heap_open(relOid, AccessExclusiveLock);

	/*
	 * Toast table is shared if and only if its parent is.
	 *
	 * We cannot allow toasting a shared relation after initdb (because
	 * there's no way to mark it toasted in other databases' pg_class).
	 * Unfortunately we can't distinguish initdb from a manually started
	 * standalone backend (toasting happens after the bootstrap phase, so
	 * checking IsBootstrapProcessingMode() won't work).  However, we can at
	 * least prevent this mistake under normal multi-user operation.
	 */
	shared_relation = rel->rd_rel->relisshared;
	if (shared_relation && IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("shared tables cannot be toasted after initdb")));

	/*
	 * Is it already toasted?
	 */
	if (rel->rd_rel->reltoastrelid != InvalidOid)
	{
		if (silent)
		{
			heap_close(rel, NoLock);
			return;
		}

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" already has a TOAST table",
						RelationGetRelationName(rel))));
	}

	/*
	 * Check to see whether the table actually needs a TOAST table.
	 */
	if (!needs_toast_table(rel))
	{
		if (silent)
		{
			heap_close(rel, NoLock);
			return;
		}

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" does not need a TOAST table",
						RelationGetRelationName(rel))));
	}

	/*
	 * Create the toast table and its index
	 */
	snprintf(toast_relname, sizeof(toast_relname),
			 "pg_toast_%u", relOid);
	snprintf(toast_idxname, sizeof(toast_idxname),
			 "pg_toast_%u_index", relOid);

	/* this is pretty painful...  need a tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(3, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1,
					   "chunk_id",
					   OIDOID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2,
					   "chunk_seq",
					   INT4OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3,
					   "chunk_data",
					   BYTEAOID,
					   -1, 0);

	/*
	 * Ensure that the toast table doesn't itself get toasted, or we'll be
	 * toast :-(.  This is essential for chunk_data because type bytea is
	 * toastable; hit the other two just to be sure.
	 */
	tupdesc->attrs[0]->attstorage = 'p';
	tupdesc->attrs[1]->attstorage = 'p';
	tupdesc->attrs[2]->attstorage = 'p';

	/*
	 * Note: the toast relation is placed in the regular pg_toast namespace
	 * even if its master relation is a temp table.  There cannot be any
	 * naming collision, and the toast rel will be destroyed when its master
	 * is, so there's no need to handle the toast rel as temp.
	 */
	toast_relid = heap_create_with_catalog(toast_relname,
										   PG_TOAST_NAMESPACE,
										   rel->rd_rel->reltablespace,
										   InvalidOid,
										   rel->rd_rel->relowner,
										   tupdesc,
										   RELKIND_TOASTVALUE,
										   shared_relation,
										   true,
										   0,
										   ONCOMMIT_NOOP,
										   true);

	/* make the toast relation visible, else index creation will fail */
	CommandCounterIncrement();

	/*
	 * Create unique index on chunk_id, chunk_seq.
	 *
	 * NOTE: the normal TOAST access routines could actually function with a
	 * single-column index on chunk_id only. However, the slice access
	 * routines use both columns for faster access to an individual chunk. In
	 * addition, we want it to be unique as a check against the possibility of
	 * duplicate TOAST chunk OIDs. The index might also be a little more
	 * efficient this way, since btree isn't all that happy with large numbers
	 * of equal keys.
	 */

	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = 2;
	indexInfo->ii_KeyAttrNumbers[0] = 1;
	indexInfo->ii_KeyAttrNumbers[1] = 2;
	indexInfo->ii_Expressions = NIL;
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_Predicate = NIL;
	indexInfo->ii_PredicateState = NIL;
	indexInfo->ii_Unique = true;

	classObjectId[0] = OID_BTREE_OPS_OID;
	classObjectId[1] = INT4_BTREE_OPS_OID;

	toast_idxid = index_create(toast_relid, toast_idxname, InvalidOid,
							   indexInfo,
							   BTREE_AM_OID,
							   rel->rd_rel->reltablespace,
							   classObjectId,
							   true, false, true, false);

	/*
	 * Update toast rel's pg_class entry to show that it has an index. The
	 * index OID is stored into the reltoastidxid field for easy access by the
	 * tuple toaster.
	 */
	setRelhasindex(toast_relid, true, true, toast_idxid);

	/*
	 * Store the toast table's OID in the parent relation's pg_class row
	 */
	class_rel = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELOID,
								ObjectIdGetDatum(relOid),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	((Form_pg_class) GETSTRUCT(reltup))->reltoastrelid = toast_relid;

	simple_heap_update(class_rel, &reltup->t_self, reltup);

	/* Keep catalog indexes current */
	CatalogUpdateIndexes(class_rel, reltup);

	heap_freetuple(reltup);

	heap_close(class_rel, RowExclusiveLock);

	/*
	 * Register dependency from the toast table to the master, so that the
	 * toast table will be deleted if the master is.
	 */
	baseobject.classId = RelationRelationId;
	baseobject.objectId = relOid;
	baseobject.objectSubId = 0;
	toastobject.classId = RelationRelationId;
	toastobject.objectId = toast_relid;
	toastobject.objectSubId = 0;

	recordDependencyOn(&toastobject, &baseobject, DEPENDENCY_INTERNAL);

	/*
	 * Clean up and make changes visible
	 */
	heap_close(rel, NoLock);

	CommandCounterIncrement();
}

/*
 * Check to see whether the table needs a TOAST table.	It does only if
 * (1) there are any toastable attributes, and (2) the maximum length
 * of a tuple could exceed TOAST_TUPLE_THRESHOLD.  (We don't want to
 * create a toast table for something like "f1 varchar(20)".)
 */
static bool
needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc;
	Form_pg_attribute *att;
	int32		tuple_length;
	int			i;

	tupdesc = rel->rd_att;
	att = tupdesc->attrs;

	for (i = 0; i < tupdesc->natts; i++)
	{
		if (att[i]->attisdropped)
			continue;
		data_length = att_align(data_length, att[i]->attalign);
		if (att[i]->attlen > 0)
		{
			/* Fixed-length types are never toastable */
			data_length += att[i]->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att[i]->atttypid,
												   att[i]->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att[i]->attstorage != 'p')
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* nothing to toast? */
	if (maxlength_unknown)
		return true;			/* any unlimited-length attrs? */
	tuple_length = MAXALIGN(offsetof(HeapTupleHeaderData, t_bits) +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
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

	rel = heap_openrv(relation, AccessExclusiveLock);

	/* heap_openrv allows TOAST, but we don't want to */
	if (rel->rd_rel->relkind == RELKIND_TOASTVALUE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a TOAST relation",
						RelationGetRelationName(rel))));

	relid = RelationGetRelid(rel);
	oldNspOid = RelationGetNamespace(rel);

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
	AlterTypeNamespaceInternal(rel->rd_rel->reltype, nspOid, false);

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
	 * SERIAL sequences are those having an internal dependency on one of the
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

		/* skip dependencies other than internal dependencies on columns */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			depForm->deptype != DEPENDENCY_INTERNAL)
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
								   newNspOid, false);

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
