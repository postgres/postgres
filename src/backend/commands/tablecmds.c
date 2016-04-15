/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  Commands for creating and altering table structures and settings
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablecmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_fn.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/policy.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "executor/executor.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "pgstat.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/typcache.h"


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

#define AT_PASS_UNSET			-1		/* UNSET will cause ERROR */
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
	int			rewrite;		/* Reason for forced rewrite, if any */
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
	bool		chgPersistence; /* T if SET LOGGED/UNLOGGED is used */
	char		newrelpersistence;		/* if above is true */
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
	Oid			refindid;		/* OID of PK's index, if FOREIGN */
	Oid			conid;			/* OID of pg_constraint entry, if FOREIGN */
	Node	   *qual;			/* Check expr or CONSTR_FOREIGN Constraint */
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

/*
 * Error-reporting support for RemoveRelations
 */
struct dropmsgstrings
{
	char		kind;
	int			nonexistent_code;
	const char *nonexistent_msg;
	const char *skipping_msg;
	const char *nota_msg;
	const char *drophint_msg;
};

static const struct dropmsgstrings dropmsgstringarray[] = {
	{RELKIND_RELATION,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("table \"%s\" does not exist"),
		gettext_noop("table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a table"),
	gettext_noop("Use DROP TABLE to remove a table.")},
	{RELKIND_SEQUENCE,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("sequence \"%s\" does not exist"),
		gettext_noop("sequence \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a sequence"),
	gettext_noop("Use DROP SEQUENCE to remove a sequence.")},
	{RELKIND_VIEW,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("view \"%s\" does not exist"),
		gettext_noop("view \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a view"),
	gettext_noop("Use DROP VIEW to remove a view.")},
	{RELKIND_MATVIEW,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("materialized view \"%s\" does not exist"),
		gettext_noop("materialized view \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a materialized view"),
	gettext_noop("Use DROP MATERIALIZED VIEW to remove a materialized view.")},
	{RELKIND_INDEX,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("index \"%s\" does not exist"),
		gettext_noop("index \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not an index"),
	gettext_noop("Use DROP INDEX to remove an index.")},
	{RELKIND_COMPOSITE_TYPE,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("type \"%s\" does not exist"),
		gettext_noop("type \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a type"),
	gettext_noop("Use DROP TYPE to remove a type.")},
	{RELKIND_FOREIGN_TABLE,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("foreign table \"%s\" does not exist"),
		gettext_noop("foreign table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a foreign table"),
	gettext_noop("Use DROP FOREIGN TABLE to remove a foreign table.")},
	{'\0', 0, NULL, NULL, NULL, NULL}
};

struct DropRelationCallbackState
{
	char		relkind;
	Oid			heapOid;
	bool		concurrent;
};

/* Alter table target-type flags for ATSimplePermissions */
#define		ATT_TABLE				0x0001
#define		ATT_VIEW				0x0002
#define		ATT_MATVIEW				0x0004
#define		ATT_INDEX				0x0008
#define		ATT_COMPOSITE_TYPE		0x0010
#define		ATT_FOREIGN_TABLE		0x0020

static void truncate_check_rel(Relation rel);
static List *MergeAttributes(List *schema, List *supers, char relpersistence,
				List **supOids, List **supconstr, int *supOidCount);
static bool MergeCheckConstraint(List *constraints, char *name, Node *expr);
static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel);
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel);
static void StoreCatalogInheritance(Oid relationId, List *supers);
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int16 seqNumber, Relation inhRelation);
static int	findAttrByName(const char *attributeName, List *schema);
static void AlterIndexNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved);
static void AlterSeqNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved,
				   LOCKMODE lockmode);
static ObjectAddress ATExecAlterConstraint(Relation rel, AlterTableCmd *cmd,
					  bool recurse, bool recursing, LOCKMODE lockmode);
static ObjectAddress ATExecValidateConstraint(Relation rel, char *constrName,
						 bool recurse, bool recursing, LOCKMODE lockmode);
static int transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids);
static int transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids,
						   Oid *opclasses);
static Oid transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						Oid *opclasses);
static void checkFkeyPermissions(Relation rel, int16 *attnums, int natts);
static CoercionPathType findFkeyCast(Oid targetTypeId, Oid sourceTypeId,
			 Oid *funcid);
static void validateCheckConstraint(Relation rel, HeapTuple constrtup);
static void validateForeignKeyConstraint(char *conname,
							 Relation rel, Relation pkrel,
							 Oid pkindOid, Oid constraintOid);
static void createForeignKeyTriggers(Relation rel, Oid refRelOid,
						 Constraint *fkconstraint,
						 Oid constraintOid, Oid indexOid);
static void ATController(AlterTableStmt *parsetree,
			 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode);
static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing, LOCKMODE lockmode);
static void ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode);
static void ATExecCmd(List **wqueue, AlteredTableInfo *tab, Relation rel,
		  AlterTableCmd *cmd, LOCKMODE lockmode);
static void ATRewriteTables(AlterTableStmt *parsetree,
				List **wqueue, LOCKMODE lockmode);
static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap, LOCKMODE lockmode);
static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
static void ATSimplePermissions(Relation rel, int allowed_targets);
static void ATWrongRelkindError(Relation rel, int allowed_targets);
static void ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode);
static void ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  LOCKMODE lockmode);
static List *find_typed_table_dependencies(Oid typeOid, const char *typeName,
							  DropBehavior behavior);
static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode);
static ObjectAddress ATExecAddColumn(List **wqueue, AlteredTableInfo *tab,
				Relation rel, ColumnDef *colDef, bool isOid,
				bool recurse, bool recursing, LOCKMODE lockmode);
static void check_for_column_name_collision(Relation rel, const char *colname);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void add_column_collation_dependency(Oid relid, int32 attnum, Oid collid);
static void ATPrepAddOids(List **wqueue, Relation rel, bool recurse,
			  AlterTableCmd *cmd, LOCKMODE lockmode);
static ObjectAddress ATExecDropNotNull(Relation rel, const char *colName, LOCKMODE lockmode);
static ObjectAddress ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName, LOCKMODE lockmode);
static ObjectAddress ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault, LOCKMODE lockmode);
static void ATPrepSetStatistics(Relation rel, const char *colName,
					Node *newValue, LOCKMODE lockmode);
static ObjectAddress ATExecSetStatistics(Relation rel, const char *colName,
					Node *newValue, LOCKMODE lockmode);
static ObjectAddress ATExecSetOptions(Relation rel, const char *colName,
				 Node *options, bool isReset, LOCKMODE lockmode);
static ObjectAddress ATExecSetStorage(Relation rel, const char *colName,
				 Node *newValue, LOCKMODE lockmode);
static void ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				 AlterTableCmd *cmd, LOCKMODE lockmode);
static ObjectAddress ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing,
				 bool missing_ok, LOCKMODE lockmode);
static ObjectAddress ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode);
static ObjectAddress ATExecAddConstraint(List **wqueue,
					AlteredTableInfo *tab, Relation rel,
					Constraint *newConstraint, bool recurse, bool is_readd,
					LOCKMODE lockmode);
static ObjectAddress ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
						 IndexStmt *stmt, LOCKMODE lockmode);
static ObjectAddress ATAddCheckConstraint(List **wqueue,
					 AlteredTableInfo *tab, Relation rel,
					 Constraint *constr,
					 bool recurse, bool recursing, bool is_readd,
					 LOCKMODE lockmode);
static ObjectAddress ATAddForeignKeyConstraint(AlteredTableInfo *tab, Relation rel,
						  Constraint *fkconstraint, LOCKMODE lockmode);
static void ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior,
					 bool recurse, bool recursing,
					 bool missing_ok, LOCKMODE lockmode);
static void ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd, LOCKMODE lockmode);
static bool ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno);
static ObjectAddress ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  AlterTableCmd *cmd, LOCKMODE lockmode);
static ObjectAddress ATExecAlterColumnGenericOptions(Relation rel, const char *colName,
								List *options, LOCKMODE lockmode);
static void ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab,
					   LOCKMODE lockmode);
static void ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId,
					 char *cmd, List **wqueue, LOCKMODE lockmode,
					 bool rewrite);
static void RebuildConstraintComment(AlteredTableInfo *tab, int pass,
						 Oid objid, Relation rel, char *conname);
static void TryReuseIndex(Oid oldId, IndexStmt *stmt);
static void TryReuseForeignKey(Oid oldId, Constraint *con);
static void change_owner_fix_column_acls(Oid relationOid,
							 Oid oldOwnerId, Oid newOwnerId);
static void change_owner_recurse_to_sequences(Oid relationOid,
								  Oid newOwnerId, LOCKMODE lockmode);
static ObjectAddress ATExecClusterOn(Relation rel, const char *indexName,
				LOCKMODE lockmode);
static void ATExecDropCluster(Relation rel, LOCKMODE lockmode);
static bool ATPrepChangePersistence(Relation rel, bool toLogged);
static void ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel,
					char *tablespacename, LOCKMODE lockmode);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode);
static void ATExecSetRelOptions(Relation rel, List *defList,
					AlterTableType operation,
					LOCKMODE lockmode);
static void ATExecEnableDisableTrigger(Relation rel, char *trigname,
					   char fires_when, bool skip_system, LOCKMODE lockmode);
static void ATExecEnableDisableRule(Relation rel, char *rulename,
						char fires_when, LOCKMODE lockmode);
static void ATPrepAddInherit(Relation child_rel);
static ObjectAddress ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode);
static ObjectAddress ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode);
static void drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid);
static ObjectAddress ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode);
static void ATExecDropOf(Relation rel, LOCKMODE lockmode);
static void ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode);
static void ATExecGenericOptions(Relation rel, List *options);
static void ATExecEnableRowSecurity(Relation rel);
static void ATExecDisableRowSecurity(Relation rel);
static void ATExecForceNoForceRowSecurity(Relation rel, bool force_rls);

static void copy_relation_data(SMgrRelation rel, SMgrRelation dst,
				   ForkNumber forkNum, char relpersistence);
static const char *storage_name(char c);

static void RangeVarCallbackForDropRelation(const RangeVar *rel, Oid relOid,
								Oid oldRelOid, void *arg);
static void RangeVarCallbackForAlterRelation(const RangeVar *rv, Oid relid,
								 Oid oldrelid, void *arg);


/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 *
 * stmt carries parsetree information from an ordinary CREATE TABLE statement.
 * The other arguments are used to extend the behavior for other cases:
 * relkind: relkind to assign to the new relation
 * ownerId: if not InvalidOid, use this as the new relation's owner.
 * typaddress: if not null, it's set to the pg_type entry's address.
 *
 * Note that permissions checks are done against current user regardless of
 * ownerId.  A nonzero ownerId is used when someone is creating a relation
 * "on behalf of" someone else, so we still want to see that the current user
 * has permissions to do it.
 *
 * If successful, returns the address of the new relation.
 * ----------------------------------------------------------------
 */
ObjectAddress
DefineRelation(CreateStmt *stmt, char relkind, Oid ownerId,
			   ObjectAddress *typaddress)
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
	List	   *cookedDefaults;
	Datum		reloptions;
	ListCell   *listptr;
	AttrNumber	attnum;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	Oid			ofTypeId;
	ObjectAddress address;

	/*
	 * Truncate relname to appropriate length (probably a waste of time, as
	 * parser should have done this already).
	 */
	StrNCpy(relname, stmt->relation->relname, NAMEDATALEN);

	/*
	 * Check consistency of arguments
	 */
	if (stmt->oncommit != ONCOMMIT_NOOP
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("ON COMMIT can only be used on temporary tables")));

	/*
	 * Look up the namespace in which we are supposed to create the relation,
	 * check we have permission to create there, lock it against concurrent
	 * drop, and mark stmt->relation as RELPERSISTENCE_TEMP if a temporary
	 * namespace is selected.
	 */
	namespaceId =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, NULL);

	/*
	 * Security check: disallow creating temp tables from security-restricted
	 * code.  This is needed because calling code might not expect untrusted
	 * tables to appear in pg_temp at the front of its search path.
	 */
	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP
		&& InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot create temporary table within security-restricted operation")));

	/*
	 * Select tablespace to use.  If not specified, use default tablespace
	 * (which may in turn default to database's default).
	 */
	if (stmt->tablespacename)
	{
		tablespaceId = get_tablespace_oid(stmt->tablespacename, false);
	}
	else
	{
		tablespaceId = GetDefaultTablespace(stmt->relation->relpersistence);
		/* note InvalidOid is OK in this case */
	}

	/* Check permissions except when using database's default */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/* In all cases disallow placing user relations in pg_global */
	if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/* Identify user ID that will own the table */
	if (!OidIsValid(ownerId))
		ownerId = GetUserId();

	/*
	 * Parse and validate reloptions, if any.
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options, NULL, validnsps,
									 true, false);

	if (relkind == RELKIND_VIEW)
		(void) view_reloptions(reloptions, true);
	else
		(void) heap_reloptions(relkind, reloptions, true);

	if (stmt->ofTypename)
	{
		AclResult	aclresult;

		ofTypeId = typenameTypeId(NULL, stmt->ofTypename);

		aclresult = pg_type_aclcheck(ofTypeId, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, ofTypeId);
	}
	else
		ofTypeId = InvalidOid;

	/*
	 * Look up inheritance ancestors and generate relation schema, including
	 * inherited attributes.
	 */
	schema = MergeAttributes(schema, stmt->inhRelations,
							 stmt->relation->relpersistence,
							 &inheritOids, &old_constraints, &parentOidCount);

	/*
	 * Create a tuple descriptor from the relation schema.  Note that this
	 * deals with column names, types, and NOT NULL constraints, but not
	 * default values or CHECK constraints; we handle those below.
	 */
	descriptor = BuildDescForRelation(schema);

	/*
	 * Notice that we allow OIDs here only for plain tables, even though some
	 * other relkinds can support them.  This is necessary because the
	 * default_with_oids GUC must apply only to plain tables and not any other
	 * relkind; doing otherwise would break existing pg_dump files.  We could
	 * allow explicit "WITH OIDS" while not allowing default_with_oids to
	 * affect other relkinds, but it would complicate interpretOidsOption().
	 */
	localHasOids = interpretOidsOption(stmt->options,
									   (relkind == RELKIND_RELATION));
	descriptor->tdhasoid = (localHasOids || parentOidCount > 0);

	/*
	 * Find columns with default values and prepare for insertion of the
	 * defaults.  Pre-cooked (that is, inherited) defaults go into a list of
	 * CookedConstraint structs that we'll pass to heap_create_with_catalog,
	 * while raw defaults go into a list of RawColumnDefault structs that will
	 * be processed by AddRelationNewConstraints.  (We can't deal with raw
	 * expressions until we can do transformExpr.)
	 *
	 * We can set the atthasdef flags now in the tuple descriptor; this just
	 * saves StoreAttrDefault from having to do an immediate update of the
	 * pg_attribute rows.
	 */
	rawDefaults = NIL;
	cookedDefaults = NIL;
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
			descriptor->attrs[attnum - 1]->atthasdef = true;
		}
		else if (colDef->cooked_default != NULL)
		{
			CookedConstraint *cooked;

			cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
			cooked->contype = CONSTR_DEFAULT;
			cooked->conoid = InvalidOid;		/* until created */
			cooked->name = NULL;
			cooked->attnum = attnum;
			cooked->expr = colDef->cooked_default;
			cooked->skip_validation = false;
			cooked->is_local = true;	/* not used for defaults */
			cooked->inhcount = 0;		/* ditto */
			cooked->is_no_inherit = false;
			cookedDefaults = lappend(cookedDefaults, cooked);
			descriptor->attrs[attnum - 1]->atthasdef = true;
		}
	}

	/*
	 * Create the relation.  Inherited defaults and constraints are passed in
	 * for immediate handling --- since they don't need parsing, they can be
	 * stored immediately.
	 */
	relationId = heap_create_with_catalog(relname,
										  namespaceId,
										  tablespaceId,
										  InvalidOid,
										  InvalidOid,
										  ofTypeId,
										  ownerId,
										  descriptor,
										  list_concat(cookedDefaults,
													  old_constraints),
										  relkind,
										  stmt->relation->relpersistence,
										  false,
										  false,
										  localHasOids,
										  parentOidCount,
										  stmt->oncommit,
										  reloptions,
										  true,
										  allowSystemTableMods,
										  false,
										  typaddress);

	/* Store inheritance information for new rel. */
	StoreCatalogInheritance(relationId, inheritOids);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire exclusive lock on it.  This isn't
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
	 */
	if (rawDefaults || stmt->constraints)
		AddRelationNewConstraints(rel, rawDefaults, stmt->constraints,
								  true, true, false);

	ObjectAddressSet(address, RelationRelationId, relationId);

	/*
	 * Clean up.  We keep lock on new relation (although it shouldn't be
	 * visible to anyone else anyway, until commit).
	 */
	relation_close(rel, NoLock);

	return address;
}

/*
 * Emit the right error or warning message for a "DROP" command issued on a
 * non-existent relation
 */
static void
DropErrorMsgNonExistent(RangeVar *rel, char rightkind, bool missing_ok)
{
	const struct dropmsgstrings *rentry;

	if (rel->schemaname != NULL &&
		!OidIsValid(LookupNamespaceNoError(rel->schemaname)))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
				   errmsg("schema \"%s\" does not exist", rel->schemaname)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("schema \"%s\" does not exist, skipping",
							rel->schemaname)));
		}
		return;
	}

	for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++)
	{
		if (rentry->kind == rightkind)
		{
			if (!missing_ok)
			{
				ereport(ERROR,
						(errcode(rentry->nonexistent_code),
						 errmsg(rentry->nonexistent_msg, rel->relname)));
			}
			else
			{
				ereport(NOTICE, (errmsg(rentry->skipping_msg, rel->relname)));
				break;
			}
		}
	}

	Assert(rentry->kind != '\0');		/* Should be impossible */
}

/*
 * Emit the right error message for a "DROP" command issued on a
 * relation of the wrong type
 */
static void
DropErrorMsgWrongType(const char *relname, char wrongkind, char rightkind)
{
	const struct dropmsgstrings *rentry;
	const struct dropmsgstrings *wentry;

	for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	for (wentry = dropmsgstringarray; wentry->kind != '\0'; wentry++)
		if (wentry->kind == wrongkind)
			break;
	/* wrongkind could be something we don't have in our table... */

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg(rentry->nota_msg, relname),
	   (wentry->kind != '\0') ? errhint("%s", _(wentry->drophint_msg)) : 0));
}

/*
 * RemoveRelations
 *		Implements DROP TABLE, DROP INDEX, DROP SEQUENCE, DROP VIEW,
 *		DROP MATERIALIZED VIEW, DROP FOREIGN TABLE
 */
void
RemoveRelations(DropStmt *drop)
{
	ObjectAddresses *objects;
	char		relkind;
	ListCell   *cell;
	int			flags = 0;
	LOCKMODE	lockmode = AccessExclusiveLock;

	/* DROP CONCURRENTLY uses a weaker lock, and has some restrictions */
	if (drop->concurrent)
	{
		flags |= PERFORM_DELETION_CONCURRENTLY;
		lockmode = ShareUpdateExclusiveLock;
		Assert(drop->removeType == OBJECT_INDEX);
		if (list_length(drop->objects) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DROP INDEX CONCURRENTLY does not support dropping multiple objects")));
		if (drop->behavior == DROP_CASCADE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("DROP INDEX CONCURRENTLY does not support CASCADE")));
	}

	/*
	 * First we identify all the relations, then we delete them in a single
	 * performMultipleDeletions() call.  This is to avoid unwanted DROP
	 * RESTRICT errors if one of the relations depends on another.
	 */

	/* Determine required relkind */
	switch (drop->removeType)
	{
		case OBJECT_TABLE:
			relkind = RELKIND_RELATION;
			break;

		case OBJECT_INDEX:
			relkind = RELKIND_INDEX;
			break;

		case OBJECT_SEQUENCE:
			relkind = RELKIND_SEQUENCE;
			break;

		case OBJECT_VIEW:
			relkind = RELKIND_VIEW;
			break;

		case OBJECT_MATVIEW:
			relkind = RELKIND_MATVIEW;
			break;

		case OBJECT_FOREIGN_TABLE:
			relkind = RELKIND_FOREIGN_TABLE;
			break;

		default:
			elog(ERROR, "unrecognized drop object type: %d",
				 (int) drop->removeType);
			relkind = 0;		/* keep compiler quiet */
			break;
	}

	/* Lock and validate each relation; build a list of object addresses */
	objects = new_object_addresses();

	foreach(cell, drop->objects)
	{
		RangeVar   *rel = makeRangeVarFromNameList((List *) lfirst(cell));
		Oid			relOid;
		ObjectAddress obj;
		struct DropRelationCallbackState state;

		/*
		 * These next few steps are a great deal like relation_openrv, but we
		 * don't bother building a relcache entry since we don't need it.
		 *
		 * Check for shared-cache-inval messages before trying to access the
		 * relation.  This is needed to cover the case where the name
		 * identifies a rel that has been dropped and recreated since the
		 * start of our transaction: if we don't flush the old syscache entry,
		 * then we'll latch onto that entry and suffer an error later.
		 */
		AcceptInvalidationMessages();

		/* Look up the appropriate relation using namespace search. */
		state.relkind = relkind;
		state.heapOid = InvalidOid;
		state.concurrent = drop->concurrent;
		relOid = RangeVarGetRelidExtended(rel, lockmode, true,
										  false,
										  RangeVarCallbackForDropRelation,
										  (void *) &state);

		/* Not there? */
		if (!OidIsValid(relOid))
		{
			DropErrorMsgNonExistent(rel, relkind, drop->missing_ok);
			continue;
		}

		/* OK, we're ready to delete this one */
		obj.classId = RelationRelationId;
		obj.objectId = relOid;
		obj.objectSubId = 0;

		add_exact_object_address(&obj, objects);
	}

	performMultipleDeletions(objects, drop->behavior, flags);

	free_object_addresses(objects);
}

/*
 * Before acquiring a table lock, check whether we have sufficient rights.
 * In the case of DROP INDEX, also try to lock the table before the index.
 */
static void
RangeVarCallbackForDropRelation(const RangeVar *rel, Oid relOid, Oid oldRelOid,
								void *arg)
{
	HeapTuple	tuple;
	struct DropRelationCallbackState *state;
	char		relkind;
	Form_pg_class classform;
	LOCKMODE	heap_lockmode;

	state = (struct DropRelationCallbackState *) arg;
	relkind = state->relkind;
	heap_lockmode = state->concurrent ?
		ShareUpdateExclusiveLock : AccessExclusiveLock;

	/*
	 * If we previously locked some other index's heap, and the name we're
	 * looking up no longer refers to that relation, release the now-useless
	 * lock.
	 */
	if (relOid != oldRelOid && OidIsValid(state->heapOid))
	{
		UnlockRelationOid(state->heapOid, heap_lockmode);
		state->heapOid = InvalidOid;
	}

	/* Didn't find a relation, so no need for locking or permission checks. */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped, so nothing to do */
	classform = (Form_pg_class) GETSTRUCT(tuple);

	if (classform->relkind != relkind)
		DropErrorMsgWrongType(rel->relname, classform->relkind, relkind);

	/* Allow DROP to either table owner or schema owner */
	if (!pg_class_ownercheck(relOid, GetUserId()) &&
		!pg_namespace_ownercheck(classform->relnamespace, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   rel->relname);

	if (!allowSystemTableMods && IsSystemClass(relOid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rel->relname)));

	ReleaseSysCache(tuple);

	/*
	 * In DROP INDEX, attempt to acquire lock on the parent table before
	 * locking the index.  index_drop() will need this anyway, and since
	 * regular queries lock tables before their indexes, we risk deadlock if
	 * we do it the other way around.  No error if we don't find a pg_index
	 * entry, though --- the relation may have been dropped.
	 */
	if (relkind == RELKIND_INDEX && relOid != oldRelOid)
	{
		state->heapOid = IndexGetRelation(relOid, true);
		if (OidIsValid(state->heapOid))
			LockRelationOid(state->heapOid, heap_lockmode);
	}
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
	List	   *seq_relids = NIL;
	EState	   *estate;
	ResultRelInfo *resultRelInfos;
	ResultRelInfo *resultRelInfo;
	SubTransactionId mySubid;
	ListCell   *cell;

	/*
	 * Open, exclusive-lock, and check all the explicitly-specified relations
	 */
	foreach(cell, stmt->relations)
	{
		RangeVar   *rv = lfirst(cell);
		Relation	rel;
		bool		recurse = interpretInhOption(rv->inhOpt);
		Oid			myrelid;

		rel = heap_openrv(rv, AccessExclusiveLock);
		myrelid = RelationGetRelid(rel);
		/* don't throw error for "TRUNCATE foo, foo" */
		if (list_member_oid(relids, myrelid))
		{
			heap_close(rel, AccessExclusiveLock);
			continue;
		}
		truncate_check_rel(rel);
		rels = lappend(rels, rel);
		relids = lappend_oid(relids, myrelid);

		if (recurse)
		{
			ListCell   *child;
			List	   *children;

			children = find_all_inheritors(myrelid, AccessExclusiveLock, NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);

				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors already got lock */
				rel = heap_open(childrelid, NoLock);
				truncate_check_rel(rel);
				rels = lappend(rels, rel);
				relids = lappend_oid(relids, childrelid);
			}
		}
	}

	/*
	 * In CASCADE mode, suck in all referencing relations as well.  This
	 * requires multiple iterations to find indirectly-dependent relations. At
	 * each phase, we need to exclusive-lock new rels before looking for their
	 * dependencies, else we might miss something.  Also, we check each rel as
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
	 * If we are asked to restart sequences, find all the sequences, lock them
	 * (we need AccessExclusiveLock for ResetSequence), and check permissions.
	 * We want to do this early since it's pointless to do all the truncation
	 * work only to fail on sequence permissions.
	 */
	if (stmt->restart_seqs)
	{
		foreach(cell, rels)
		{
			Relation	rel = (Relation) lfirst(cell);
			List	   *seqlist = getOwnedSequences(RelationGetRelid(rel));
			ListCell   *seqcell;

			foreach(seqcell, seqlist)
			{
				Oid			seq_relid = lfirst_oid(seqcell);
				Relation	seq_rel;

				seq_rel = relation_open(seq_relid, AccessExclusiveLock);

				/* This check must match AlterSequence! */
				if (!pg_class_ownercheck(seq_relid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
								   RelationGetRelationName(seq_rel));

				seq_relids = lappend_oid(seq_relids, seq_relid);

				relation_close(seq_rel, NoLock);
			}
		}
	}

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	/*
	 * To fire triggers, we'll need an EState as well as a ResultRelInfo for
	 * each relation.  We don't need to call ExecOpenIndices, though.
	 */
	estate = CreateExecutorState();
	resultRelInfos = (ResultRelInfo *)
		palloc(list_length(rels) * sizeof(ResultRelInfo));
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		InitResultRelInfo(resultRelInfo,
						  rel,
						  0,	/* dummy rangetable index */
						  0);
		resultRelInfo++;
	}
	estate->es_result_relations = resultRelInfos;
	estate->es_num_result_relations = list_length(rels);

	/*
	 * Process all BEFORE STATEMENT TRUNCATE triggers before we begin
	 * truncating (this is because one of them might throw an error). Also, if
	 * we were to allow them to prevent statement execution, that would need
	 * to be handled here.
	 */
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		estate->es_result_relation_info = resultRelInfo;
		ExecBSTruncateTriggers(estate, resultRelInfo);
		resultRelInfo++;
	}

	/*
	 * OK, truncate each table.
	 */
	mySubid = GetCurrentSubTransactionId();

	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		/*
		 * Normally, we need a transaction-safe truncation here.  However, if
		 * the table was either created in the current (sub)transaction or has
		 * a new relfilenode in the current (sub)transaction, then we can just
		 * truncate it in-place, because a rollback would cause the whole
		 * table or the current physical file to be thrown away anyway.
		 */
		if (rel->rd_createSubid == mySubid ||
			rel->rd_newRelfilenodeSubid == mySubid)
		{
			/* Immediate, non-rollbackable truncation is OK */
			heap_truncate_one_rel(rel);
		}
		else
		{
			Oid			heap_relid;
			Oid			toast_relid;
			MultiXactId minmulti;

			/*
			 * This effectively deletes all rows in the table, and may be done
			 * in a serializable transaction.  In that case we must record a
			 * rw-conflict in to this transaction from each transaction
			 * holding a predicate lock on the table.
			 */
			CheckTableForSerializableConflictIn(rel);

			minmulti = GetOldestMultiXactId();

			/*
			 * Need the full transaction-safe pushups.
			 *
			 * Create a new empty storage file for the relation, and assign it
			 * as the relfilenode value. The old storage file is scheduled for
			 * deletion at commit.
			 */
			RelationSetNewRelfilenode(rel, rel->rd_rel->relpersistence,
									  RecentXmin, minmulti);
			if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
				heap_create_init_fork(rel);

			heap_relid = RelationGetRelid(rel);
			toast_relid = rel->rd_rel->reltoastrelid;

			/*
			 * The same for the toast table, if any.
			 */
			if (OidIsValid(toast_relid))
			{
				rel = relation_open(toast_relid, AccessExclusiveLock);
				RelationSetNewRelfilenode(rel, rel->rd_rel->relpersistence,
										  RecentXmin, minmulti);
				if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
					heap_create_init_fork(rel);
				heap_close(rel, NoLock);
			}

			/*
			 * Reconstruct the indexes to match, and we're done.
			 */
			reindex_relation(heap_relid, REINDEX_REL_PROCESS_TOAST, 0);
		}

		pgstat_count_truncate(rel);
	}

	/*
	 * Restart owned sequences if we were asked to.
	 */
	foreach(cell, seq_relids)
	{
		Oid			seq_relid = lfirst_oid(cell);

		ResetSequence(seq_relid);
	}

	/*
	 * Process all AFTER STATEMENT TRUNCATE triggers.
	 */
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		estate->es_result_relation_info = resultRelInfo;
		ExecASTruncateTriggers(estate, resultRelInfo);
		resultRelInfo++;
	}

	/* Handle queued AFTER triggers */
	AfterTriggerEndQuery(estate);

	/* We can clean up the EState now */
	FreeExecutorState(estate);

	/* And close the rels (can't do this while EState still holds refs) */
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		heap_close(rel, NoLock);
	}
}

/*
 * Check that a given rel is safe to truncate.  Subroutine for ExecuteTruncate
 */
static void
truncate_check_rel(Relation rel)
{
	AclResult	aclresult;

	/* Only allow truncate on regular tables */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	/* Permissions checks */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_TRUNCATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/*
	 * Don't allow truncate on temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("cannot truncate temporary tables of other sessions")));

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 */
	CheckTableNotInUse(rel, "TRUNCATE");
}

/*
 * storage_name
 *	  returns the name corresponding to a typstorage/attstorage enum value
 */
static const char *
storage_name(char c)
{
	switch (c)
	{
		case 'p':
			return "PLAIN";
		case 'm':
			return "MAIN";
		case 'x':
			return "EXTENDED";
		case 'e':
			return "EXTERNAL";
		default:
			return "???";
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
 * 'relpersistence' is a persistence type of the table.
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
MergeAttributes(List *schema, List *supers, char relpersistence,
				List **supOids, List **supconstr, int *supOidCount)
{
	ListCell   *entry;
	List	   *inhSchema = NIL;
	List	   *parentOids = NIL;
	List	   *constraints = NIL;
	int			parentsWithOids = 0;
	bool		have_bogus_defaults = false;
	int			child_attno;
	static Node bogus_marker = {0};		/* marks conflicting defaults */

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
		ListCell   *rest = lnext(entry);
		ListCell   *prev = entry;

		if (coldef->typeName == NULL)

			/*
			 * Typed table column option that does not belong to a column from
			 * the type.  This works because the columns from the type come
			 * first in the list.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							coldef->colname)));

		while (rest != NULL)
		{
			ColumnDef  *restdef = lfirst(rest);
			ListCell   *next = lnext(rest);		/* need to save it in case we
												 * delete it */

			if (strcmp(coldef->colname, restdef->colname) == 0)
			{
				if (coldef->is_from_type)
				{
					/*
					 * merge the column options into the column from the type
					 */
					coldef->is_not_null = restdef->is_not_null;
					coldef->raw_default = restdef->raw_default;
					coldef->cooked_default = restdef->cooked_default;
					coldef->constraints = restdef->constraints;
					coldef->is_from_type = false;
					list_delete_cell(schema, rest, prev);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									coldef->colname)));
			}
			prev = rest;
			rest = next;
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

		/*
		 * A self-exclusive lock is needed here.  If two backends attempt to
		 * add children to the same parent simultaneously, and that parent has
		 * no pre-existing children, then both will attempt to update the
		 * parent's relhassubclass field, leading to a "tuple concurrently
		 * updated" error.  Also, this interlocks against a concurrent ANALYZE
		 * on the parent table, which might otherwise be attempting to clear
		 * the parent's relhassubclass field, if its previous children were
		 * recently dropped.
		 */
		relation = heap_openrv(parent, ShareUpdateExclusiveLock);

		if (relation->rd_rel->relkind != RELKIND_RELATION &&
			relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("inherited relation \"%s\" is not a table or foreign table",
							parent->relname)));
		/* Permanent rels cannot inherit from temporary ones */
		if (relpersistence != RELPERSISTENCE_TEMP &&
			relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from temporary relation \"%s\"",
							parent->relname)));

		/* If existing rel is temp, it must belong to this session */
		if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
			!relation->rd_islocaltemp)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from temporary relation of another session")));

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
			palloc0(tupleDesc->natts * sizeof(AttrNumber));

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
				continue;		/* leave newattno entry as zero */

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				Oid			defTypeId;
				int32		deftypmod;
				Oid			defCollId;

				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type, typmod, and collation.
				 */
				ereport(NOTICE,
						(errmsg("merging multiple inherited definitions of column \"%s\"",
								attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				typenameTypeIdAndMod(NULL, def->typeName, &defTypeId, &deftypmod);
				if (defTypeId != attribute->atttypid ||
					deftypmod != attribute->atttypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("inherited column \"%s\" has a type conflict",
							   attributeName),
							 errdetail("%s versus %s",
									   TypeNameToString(def->typeName),
									   format_type_be(attribute->atttypid))));
				defCollId = GetColumnDefCollation(NULL, def, defTypeId);
				if (defCollId != attribute->attcollation)
					ereport(ERROR,
							(errcode(ERRCODE_COLLATION_MISMATCH),
					errmsg("inherited column \"%s\" has a collation conflict",
						   attributeName),
							 errdetail("\"%s\" versus \"%s\"",
									   get_collation_name(defCollId),
							  get_collation_name(attribute->attcollation))));

				/* Copy storage parameter */
				if (def->storage == 0)
					def->storage = attribute->attstorage;
				else if (def->storage != attribute->attstorage)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("inherited column \"%s\" has a storage parameter conflict",
									attributeName),
							 errdetail("%s versus %s",
									   storage_name(def->storage),
									   storage_name(attribute->attstorage))));

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
				def->typeName = makeTypeNameFromOid(attribute->atttypid,
													attribute->atttypmod);
				def->inhcount = 1;
				def->is_local = false;
				def->is_not_null = attribute->attnotnull;
				def->is_from_type = false;
				def->storage = attribute->attstorage;
				def->raw_default = NULL;
				def->cooked_default = NULL;
				def->collClause = NULL;
				def->collOid = attribute->attcollation;
				def->constraints = NIL;
				def->location = -1;
				inhSchema = lappend(inhSchema, def);
				newattno[parent_attno - 1] = ++child_attno;
			}

			/*
			 * Copy default if any
			 */
			if (attribute->atthasdef)
			{
				Node	   *this_default = NULL;
				AttrDefault *attrdef;
				int			i;

				/* Find default in constraint structure */
				Assert(constr != NULL);
				attrdef = constr->defval;
				for (i = 0; i < constr->num_defval; i++)
				{
					if (attrdef[i].adnum == parent_attno)
					{
						this_default = stringToNode(attrdef[i].adbin);
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
					def->cooked_default = this_default;
				else if (!equal(def->cooked_default, this_default))
				{
					def->cooked_default = &bogus_marker;
					have_bogus_defaults = true;
				}
			}
		}

		/*
		 * Now copy the CHECK constraints of this parent, adjusting attnos
		 * using the completed newattno[] map.  Identically named constraints
		 * are merged if possible, else we throw error.
		 */
		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;
			int			i;

			for (i = 0; i < constr->num_check; i++)
			{
				char	   *name = check[i].ccname;
				Node	   *expr;
				bool		found_whole_row;

				/* ignore if the constraint is non-inheritable */
				if (check[i].ccnoinherit)
					continue;

				/* Adjust Vars to match new table's column numbering */
				expr = map_variable_attnos(stringToNode(check[i].ccbin),
										   1, 0,
										   newattno, tupleDesc->natts,
										   &found_whole_row);

				/*
				 * For the moment we have to reject whole-row variables. We
				 * could convert them, if we knew the new table's rowtype OID,
				 * but that hasn't been assigned yet.
				 */
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						  errmsg("cannot convert whole-row table reference"),
							 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
									   name,
									   RelationGetRelationName(relation))));

				/* check for duplicate */
				if (!MergeCheckConstraint(constraints, name, expr))
				{
					/* nope, this is a new one */
					CookedConstraint *cooked;

					cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
					cooked->contype = CONSTR_CHECK;
					cooked->conoid = InvalidOid;		/* until created */
					cooked->name = pstrdup(name);
					cooked->attnum = 0; /* not used for constraints */
					cooked->expr = expr;
					cooked->skip_validation = false;
					cooked->is_local = false;
					cooked->inhcount = 1;
					cooked->is_no_inherit = false;
					constraints = lappend(constraints, cooked);
				}
			}
		}

		pfree(newattno);

		/*
		 * Close the parent rel, but keep our ShareUpdateExclusiveLock on it
		 * until xact commit.  That will prevent someone else from deleting or
		 * ALTERing the parent before the child is committed.
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
		int			schema_attno = 0;

		foreach(entry, schema)
		{
			ColumnDef  *newdef = lfirst(entry);
			char	   *attributeName = newdef->colname;
			int			exist_attno;

			schema_attno++;

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
				Oid			defcollid,
							newcollid;

				/*
				 * Yes, try to merge the two column definitions. They must
				 * have the same type, typmod, and collation.
				 */
				if (exist_attno == schema_attno)
					ereport(NOTICE,
					(errmsg("merging column \"%s\" with inherited definition",
							attributeName)));
				else
					ereport(NOTICE,
							(errmsg("moving and merging column \"%s\" with inherited definition", attributeName),
							 errdetail("User-specified column moved to the position of the inherited column.")));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);
				typenameTypeIdAndMod(NULL, def->typeName, &defTypeId, &deftypmod);
				typenameTypeIdAndMod(NULL, newdef->typeName, &newTypeId, &newtypmod);
				if (defTypeId != newTypeId || deftypmod != newtypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("column \"%s\" has a type conflict",
									attributeName),
							 errdetail("%s versus %s",
									   TypeNameToString(def->typeName),
									   TypeNameToString(newdef->typeName))));
				defcollid = GetColumnDefCollation(NULL, def, defTypeId);
				newcollid = GetColumnDefCollation(NULL, newdef, newTypeId);
				if (defcollid != newcollid)
					ereport(ERROR,
							(errcode(ERRCODE_COLLATION_MISMATCH),
							 errmsg("column \"%s\" has a collation conflict",
									attributeName),
							 errdetail("\"%s\" versus \"%s\"",
									   get_collation_name(defcollid),
									   get_collation_name(newcollid))));

				/* Copy storage parameter */
				if (def->storage == 0)
					def->storage = newdef->storage;
				else if (newdef->storage != 0 && def->storage != newdef->storage)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" has a storage parameter conflict",
							attributeName),
							 errdetail("%s versus %s",
									   storage_name(def->storage),
									   storage_name(newdef->storage))));

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

			if (def->cooked_default == &bogus_marker)
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
 * MergeCheckConstraint
 *		Try to merge an inherited CHECK constraint with previous ones
 *
 * If we inherit identically-named constraints from multiple parents, we must
 * merge them, or throw an error if they don't have identical definitions.
 *
 * constraints is a list of CookedConstraint structs for previous constraints.
 *
 * Returns TRUE if merged (constraint is a duplicate), or FALSE if it's
 * got a so-far-unique name, or throws error if conflict.
 */
static bool
MergeCheckConstraint(List *constraints, char *name, Node *expr)
{
	ListCell   *lc;

	foreach(lc, constraints)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lc);

		Assert(ccon->contype == CONSTR_CHECK);

		/* Non-matching names never conflict */
		if (strcmp(ccon->name, name) != 0)
			continue;

		if (equal(expr, ccon->expr))
		{
			/* OK to merge */
			ccon->inhcount++;
			return true;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("check constraint name \"%s\" appears multiple times but with different expressions",
						name)));
	}

	return false;
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
	Datum		values[Natts_pg_inherits];
	bool		nulls[Natts_pg_inherits];
	ObjectAddress childobject,
				parentobject;
	HeapTuple	tuple;

	/*
	 * Make the pg_inherits entry
	 */
	values[Anum_pg_inherits_inhrelid - 1] = ObjectIdGetDatum(relationId);
	values[Anum_pg_inherits_inhparent - 1] = ObjectIdGetDatum(parentOid);
	values[Anum_pg_inherits_inhseqno - 1] = Int16GetDatum(seqNumber);

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(desc, values, nulls);

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
	 * Post creation hook of this inheritance. Since object_access_hook
	 * doesn't take multiple object identifiers, we relay oid of parent
	 * relation using auxiliary_id argument.
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 relationId, 0,
								 parentOid, false);

	/*
	 * Mark the parent as having subclasses.
	 */
	SetRelationHasSubclass(parentOid, true);
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
 * SetRelationHasSubclass
 *		Set the value of the relation's relhassubclass field in pg_class.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 * ShareUpdateExclusiveLock is sufficient.
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing plans
 * referencing the relation to be rebuilt with the new list of children.
 * This must happen even if we find that no change is needed in the pg_class
 * row.
 */
void
SetRelationHasSubclass(Oid relationId, bool relhassubclass)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Form_pg_class classtuple;

	/*
	 * Fetch a modifiable copy of the tuple, modify it, update pg_class.
	 */
	relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relationId));
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
 *		renameatt_check			- basic sanity checks before attribute rename
 */
static void
renameatt_check(Oid myrelid, Form_pg_class classform, bool recursing)
{
	char		relkind = classform->relkind;

	if (classform->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot rename column of typed table")));

	/*
	 * Renaming the columns of sequences or toast tables doesn't actually
	 * break anything from the system's point of view, since internal
	 * references are by attnum.  But it doesn't seem right to allow users to
	 * change names that are hardcoded into the system, hence the following
	 * restriction.
	 */
	if (relkind != RELKIND_RELATION &&
		relkind != RELKIND_VIEW &&
		relkind != RELKIND_MATVIEW &&
		relkind != RELKIND_COMPOSITE_TYPE &&
		relkind != RELKIND_INDEX &&
		relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, materialized view, composite type, index, or foreign table",
						NameStr(classform->relname))));

	/*
	 * permissions checking.  only the owner of a class can change its schema.
	 */
	if (!pg_class_ownercheck(myrelid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   NameStr(classform->relname));
	if (!allowSystemTableMods && IsSystemClass(myrelid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						NameStr(classform->relname))));
}

/*
 *		renameatt_internal		- workhorse for renameatt
 *
 * Return value is the attribute number in the 'myrelid' relation.
 */
static AttrNumber
renameatt_internal(Oid myrelid,
				   const char *oldattname,
				   const char *newattname,
				   bool recurse,
				   bool recursing,
				   int expected_parents,
				   DropBehavior behavior)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	atttup;
	Form_pg_attribute attform;
	AttrNumber	attnum;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);
	renameatt_check(myrelid, RelationGetForm(targetrelation), recursing);

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
		List	   *child_oids,
				   *child_numparents;
		ListCell   *lo,
				   *li;

		/*
		 * we need the number of parents for each child so that the recursive
		 * calls to renameatt() can determine whether there are any parents
		 * outside the inheritance hierarchy being processed.
		 */
		child_oids = find_all_inheritors(myrelid, AccessExclusiveLock,
										 &child_numparents);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 */
		forboth(lo, child_oids, li, child_numparents)
		{
			Oid			childrelid = lfirst_oid(lo);
			int			numparents = lfirst_int(li);

			if (childrelid == myrelid)
				continue;
			/* note we need not recurse again */
			renameatt_internal(childrelid, oldattname, newattname, false, true, numparents, behavior);
		}
	}
	else
	{
		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables; else the rename would put them out of step.
		 *
		 * expected_parents will only be 0 if we are not already recursing.
		 */
		if (expected_parents == 0 &&
			find_inheritance_children(myrelid, NoLock) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("inherited column \"%s\" must be renamed in child tables too",
							oldattname)));
	}

	/* rename attributes in typed tables of composite type */
	if (targetrelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		List	   *child_oids;
		ListCell   *lo;

		child_oids = find_typed_table_dependencies(targetrelation->rd_rel->reltype,
									 RelationGetRelationName(targetrelation),
												   behavior);

		foreach(lo, child_oids)
			renameatt_internal(lfirst_oid(lo), oldattname, newattname, true, true, 0, behavior);
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
	 * if the attribute is inherited, forbid the renaming.  if this is a
	 * top-level call to renameatt(), then expected_parents will be 0, so the
	 * effect of this code will be to prohibit the renaming if the attribute
	 * is inherited at all.  if this is a recursive call to renameatt(),
	 * expected_parents will be the number of parents the current relation has
	 * within the inheritance hierarchy being processed, so we'll prohibit the
	 * renaming only if there are additional parents from elsewhere.
	 */
	if (attform->attinhcount > expected_parents)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot rename inherited column \"%s\"",
						oldattname)));

	/* new name should not already exist */
	check_for_column_name_collision(targetrelation, newattname);

	/* apply the update */
	namestrcpy(&(attform->attname), newattname);

	simple_heap_update(attrelation, &atttup->t_self, atttup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, atttup);

	InvokeObjectPostAlterHook(RelationRelationId, myrelid, attnum);

	heap_freetuple(atttup);

	heap_close(attrelation, RowExclusiveLock);

	relation_close(targetrelation, NoLock);		/* close rel but keep lock */

	return attnum;
}

/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 */
static void
RangeVarCallbackForRenameAttribute(const RangeVar *rv, Oid relid, Oid oldrelid,
								   void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
	form = (Form_pg_class) GETSTRUCT(tuple);
	renameatt_check(relid, form, false);
	ReleaseSysCache(tuple);
}

/*
 *		renameatt		- changes the name of an attribute in a relation
 *
 * The returned ObjectAddress is that of the renamed column.
 */
ObjectAddress
renameatt(RenameStmt *stmt)
{
	Oid			relid;
	AttrNumber	attnum;
	ObjectAddress address;

	/* lock level taken here should match renameatt_internal */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 stmt->missing_ok, false,
									 RangeVarCallbackForRenameAttribute,
									 NULL);

	if (!OidIsValid(relid))
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return InvalidObjectAddress;
	}

	attnum =
		renameatt_internal(relid,
						   stmt->subname,		/* old att name */
						   stmt->newname,		/* new att name */
						   interpretInhOption(stmt->relation->inhOpt),	/* recursive? */
						   false,		/* recursing? */
						   0,	/* expected inhcount */
						   stmt->behavior);

	ObjectAddressSubSet(address, RelationRelationId, relid, attnum);

	return address;
}

/*
 * same logic as renameatt_internal
 */
static ObjectAddress
rename_constraint_internal(Oid myrelid,
						   Oid mytypid,
						   const char *oldconname,
						   const char *newconname,
						   bool recurse,
						   bool recursing,
						   int expected_parents)
{
	Relation	targetrelation = NULL;
	Oid			constraintOid;
	HeapTuple	tuple;
	Form_pg_constraint con;
	ObjectAddress address;

	AssertArg(!myrelid || !mytypid);

	if (mytypid)
	{
		constraintOid = get_domain_constraint_oid(mytypid, oldconname, false);
	}
	else
	{
		targetrelation = relation_open(myrelid, AccessExclusiveLock);

		/*
		 * don't tell it whether we're recursing; we allow changing typed
		 * tables here
		 */
		renameatt_check(myrelid, RelationGetForm(targetrelation), false);

		constraintOid = get_relation_constraint_oid(myrelid, oldconname, false);
	}

	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u",
			 constraintOid);
	con = (Form_pg_constraint) GETSTRUCT(tuple);

	if (myrelid && con->contype == CONSTRAINT_CHECK && !con->connoinherit)
	{
		if (recurse)
		{
			List	   *child_oids,
					   *child_numparents;
			ListCell   *lo,
					   *li;

			child_oids = find_all_inheritors(myrelid, AccessExclusiveLock,
											 &child_numparents);

			forboth(lo, child_oids, li, child_numparents)
			{
				Oid			childrelid = lfirst_oid(lo);
				int			numparents = lfirst_int(li);

				if (childrelid == myrelid)
					continue;

				rename_constraint_internal(childrelid, InvalidOid, oldconname, newconname, false, true, numparents);
			}
		}
		else
		{
			if (expected_parents == 0 &&
				find_inheritance_children(myrelid, NoLock) != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("inherited constraint \"%s\" must be renamed in child tables too",
								oldconname)));
		}

		if (con->coninhcount > expected_parents)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot rename inherited constraint \"%s\"",
							oldconname)));
	}

	if (con->conindid
		&& (con->contype == CONSTRAINT_PRIMARY
			|| con->contype == CONSTRAINT_UNIQUE
			|| con->contype == CONSTRAINT_EXCLUSION))
		/* rename the index; this renames the constraint as well */
		RenameRelationInternal(con->conindid, newconname, false);
	else
		RenameConstraintById(constraintOid, newconname);

	ObjectAddressSet(address, ConstraintRelationId, constraintOid);

	ReleaseSysCache(tuple);

	if (targetrelation)
		relation_close(targetrelation, NoLock); /* close rel but keep lock */

	return address;
}

ObjectAddress
RenameConstraint(RenameStmt *stmt)
{
	Oid			relid = InvalidOid;
	Oid			typid = InvalidOid;

	if (stmt->renameType == OBJECT_DOMCONSTRAINT)
	{
		Relation	rel;
		HeapTuple	tup;

		typid = typenameTypeId(NULL, makeTypeNameFromNameList(stmt->object));
		rel = heap_open(TypeRelationId, RowExclusiveLock);
		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typid);
		checkDomainOwner(tup);
		ReleaseSysCache(tup);
		heap_close(rel, NoLock);
	}
	else
	{
		/* lock level taken here should match rename_constraint_internal */
		relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
										 stmt->missing_ok, false,
										 RangeVarCallbackForRenameAttribute,
										 NULL);
		if (!OidIsValid(relid))
		{
			ereport(NOTICE,
					(errmsg("relation \"%s\" does not exist, skipping",
							stmt->relation->relname)));
			return InvalidObjectAddress;
		}
	}

	return
		rename_constraint_internal(relid, typid,
								   stmt->subname,
								   stmt->newname,
		 stmt->relation ? interpretInhOption(stmt->relation->inhOpt) : false,	/* recursive? */
								   false,		/* recursing? */
								   0 /* expected inhcount */ );

}

/*
 * Execute ALTER TABLE/INDEX/SEQUENCE/VIEW/MATERIALIZED VIEW/FOREIGN TABLE
 * RENAME
 */
ObjectAddress
RenameRelation(RenameStmt *stmt)
{
	Oid			relid;
	ObjectAddress address;

	/*
	 * Grab an exclusive lock on the target table, index, sequence, view,
	 * materialized view, or foreign table, which we will NOT release until
	 * end of transaction.
	 *
	 * Lock level used here should match RenameRelationInternal, to avoid lock
	 * escalation.
	 */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 stmt->missing_ok, false,
									 RangeVarCallbackForAlterRelation,
									 (void *) stmt);

	if (!OidIsValid(relid))
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return InvalidObjectAddress;
	}

	/* Do the work */
	RenameRelationInternal(relid, stmt->newname, false);

	ObjectAddressSet(address, RelationRelationId, relid);

	return address;
}

/*
 *		RenameRelationInternal - change the name of a relation
 *
 *		XXX - When renaming sequences, we don't bother to modify the
 *			  sequence name that is stored within the sequence itself
 *			  (this would cause problems with MVCC). In the future,
 *			  the sequence name should probably be removed from the
 *			  sequence, AFAIK there's no need for it to be there.
 */
void
RenameRelationInternal(Oid myrelid, const char *newrelname, bool is_internal)
{
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			namespaceId;

	/*
	 * Grab an exclusive lock on the target table, index, sequence, view,
	 * materialized view, or foreign table, which we will NOT release until
	 * end of transaction.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);
	namespaceId = RelationGetNamespace(targetrelation);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in use.
	 */
	relrelation = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						newrelname)));

	/*
	 * Update pg_class tuple with new relname.  (Scribbling on reltup is OK
	 * because it's a copy...)
	 */
	namestrcpy(&(relform->relname), newrelname);

	simple_heap_update(relrelation, &reltup->t_self, reltup);

	/* keep the system catalog indexes current */
	CatalogUpdateIndexes(relrelation, reltup);

	InvokeObjectPostAlterHookArg(RelationRelationId, myrelid, 0,
								 InvalidOid, is_internal);

	heap_freetuple(reltup);
	heap_close(relrelation, RowExclusiveLock);

	/*
	 * Also rename the associated type, if any.
	 */
	if (OidIsValid(targetrelation->rd_rel->reltype))
		RenameTypeInternal(targetrelation->rd_rel->reltype,
						   newrelname, namespaceId);

	/*
	 * Also rename the associated constraint, if any.
	 */
	if (targetrelation->rd_rel->relkind == RELKIND_INDEX)
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
 * We need this check because our lock doesn't protect us against stomping
 * on our own foot, only other people's feet!
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
 * AlterTableLookupRelation
 *		Look up, and lock, the OID for the relation named by an alter table
 *		statement.
 */
Oid
AlterTableLookupRelation(AlterTableStmt *stmt, LOCKMODE lockmode)
{
	return RangeVarGetRelidExtended(stmt->relation, lockmode, stmt->missing_ok, false,
									RangeVarCallbackForAlterRelation,
									(void *) stmt);
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
 * it.  The intention of this design is to allow multiple independent
 * updates of the table schema to be performed with only one pass over the
 * data.
 *
 * ATPrepCmd performs phase 1.  A "work queue" entry is created for
 * each table to be affected (there may be multiple affected tables if the
 * commands traverse a table inheritance hierarchy).  Also we do preliminary
 * validation of the subcommands, including parse transformation of those
 * expressions that need to be evaluated with respect to the old table
 * schema.
 *
 * ATRewriteCatalogs performs phase 2 for each affected table.  (Note that
 * phases 2 and 3 normally do no explicit recursion, since phase 1 already
 * did it --- although some subcommands have to recurse in phase 2 instead.)
 * Certain subcommands need to be performed before others to avoid
 * unnecessary conflicts; for example, DROP COLUMN should come before
 * ADD COLUMN.  Therefore phase 1 divides the subcommands into multiple
 * lists, one for each logical "pass" of phase 2.
 *
 * ATRewriteTables performs phase 3 for those tables that need it.
 *
 * Thanks to the magic of MVCC, an error anywhere along the way rolls back
 * the whole operation; we don't have to do anything special to clean up.
 *
 * The caller must lock the relation, with an appropriate lock level
 * for the subcommands requested, using AlterTableGetLockLevel(stmt->cmds)
 * or higher. We pass the lock level down
 * so that we can apply it recursively to inherited tables. Note that the
 * lock level we want as we recurse might well be higher than required for
 * that specific subcommand. So we pass down the overall lock requirement,
 * rather than reassess it at lower levels.
 */
void
AlterTable(Oid relid, LOCKMODE lockmode, AlterTableStmt *stmt)
{
	Relation	rel;

	/* Caller is required to provide an adequate lock. */
	rel = relation_open(relid, NoLock);

	CheckTableNotInUse(rel, "ALTER TABLE");

	ATController(stmt,
				 rel, stmt->cmds, interpretInhOption(stmt->relation->inhOpt),
				 lockmode);
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
	Relation	rel;
	LOCKMODE	lockmode = AlterTableGetLockLevel(cmds);

	rel = relation_open(relid, lockmode);

	EventTriggerAlterTableRelid(relid);

	ATController(NULL, rel, cmds, recurse, lockmode);
}

/*
 * AlterTableGetLockLevel
 *
 * Sets the overall lock level required for the supplied list of subcommands.
 * Policy for doing this set according to needs of AlterTable(), see
 * comments there for overall explanation.
 *
 * Function is called before and after parsing, so it must give same
 * answer each time it is called. Some subcommands are transformed
 * into other subcommand types, so the transform must never be made to a
 * lower lock level than previously assigned. All transforms are noted below.
 *
 * Since this is called before we lock the table we cannot use table metadata
 * to influence the type of lock we acquire.
 *
 * There should be no lockmodes hardcoded into the subcommand functions. All
 * lockmode decisions for ALTER TABLE are made here only. The one exception is
 * ALTER TABLE RENAME which is treated as a different statement type T_RenameStmt
 * and does not travel through this section of code and cannot be combined with
 * any of the subcommands given here.
 *
 * Note that Hot Standby only knows about AccessExclusiveLocks on the master
 * so any changes that might affect SELECTs running on standbys need to use
 * AccessExclusiveLocks even if you think a lesser lock would do, unless you
 * have a solution for that also.
 *
 * Also note that pg_dump uses only an AccessShareLock, meaning that anything
 * that takes a lock less than AccessExclusiveLock can change object definitions
 * while pg_dump is running. Be careful to check that the appropriate data is
 * derived by pg_dump using an MVCC snapshot, rather than syscache lookups,
 * otherwise we might end up with an inconsistent dump that can't restore.
 */
LOCKMODE
AlterTableGetLockLevel(List *cmds)
{
	/*
	 * This only works if we read catalog tables using MVCC snapshots.
	 */
	ListCell   *lcmd;
	LOCKMODE	lockmode = ShareUpdateExclusiveLock;

	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		LOCKMODE	cmd_lockmode = AccessExclusiveLock; /* default for compiler */

		switch (cmd->subtype)
		{
				/*
				 * These subcommands rewrite the heap, so require full locks.
				 */
			case AT_AddColumn:	/* may rewrite heap, in some cases and visible
								 * to SELECT */
			case AT_SetTableSpace:		/* must rewrite heap */
			case AT_AlterColumnType:	/* must rewrite heap */
			case AT_AddOids:	/* must rewrite heap */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands may require addition of toast tables. If
				 * we add a toast table to a table currently being scanned, we
				 * might miss data added to the new toast table by concurrent
				 * insert transactions.
				 */
			case AT_SetStorage:/* may add toast tables, see
								 * ATRewriteCatalogs() */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Removing constraints can affect SELECTs that have been
				 * optimised assuming the constraint holds true.
				 */
			case AT_DropConstraint:		/* as DROP INDEX */
			case AT_DropNotNull:		/* may change some SQL plans */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Subcommands that may be visible to concurrent SELECTs
				 */
			case AT_DropColumn:	/* change visible to SELECT */
			case AT_AddColumnToView:	/* CREATE VIEW */
			case AT_DropOids:	/* calls AT_DropColumn */
			case AT_EnableAlwaysRule:	/* may change SELECT rules */
			case AT_EnableReplicaRule:	/* may change SELECT rules */
			case AT_EnableRule:	/* may change SELECT rules */
			case AT_DisableRule:		/* may change SELECT rules */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing owner may remove implicit SELECT privileges
				 */
			case AT_ChangeOwner:		/* change visible to SELECT */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing foreign table options may affect optimisation.
				 */
			case AT_GenericOptions:
			case AT_AlterColumnGenericOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect write operations only.
				 */
			case AT_EnableTrig:
			case AT_EnableAlwaysTrig:
			case AT_EnableReplicaTrig:
			case AT_EnableTrigAll:
			case AT_EnableTrigUser:
			case AT_DisableTrig:
			case AT_DisableTrigAll:
			case AT_DisableTrigUser:
				cmd_lockmode = ShareRowExclusiveLock;
				break;

				/*
				 * These subcommands affect write operations only. XXX
				 * Theoretically, these could be ShareRowExclusiveLock.
				 */
			case AT_ColumnDefault:
			case AT_AlterConstraint:
			case AT_AddIndex:	/* from ADD CONSTRAINT */
			case AT_AddIndexConstraint:
			case AT_ReplicaIdentity:
			case AT_SetNotNull:
			case AT_EnableRowSecurity:
			case AT_DisableRowSecurity:
			case AT_ForceRowSecurity:
			case AT_NoForceRowSecurity:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_AddConstraint:
			case AT_ProcessedConstraint:		/* becomes AT_AddConstraint */
			case AT_AddConstraintRecurse:		/* becomes AT_AddConstraint */
			case AT_ReAddConstraint:	/* becomes AT_AddConstraint */
				if (IsA(cmd->def, Constraint))
				{
					Constraint *con = (Constraint *) cmd->def;

					switch (con->contype)
					{
						case CONSTR_EXCLUSION:
						case CONSTR_PRIMARY:
						case CONSTR_UNIQUE:

							/*
							 * Cases essentially the same as CREATE INDEX. We
							 * could reduce the lock strength to ShareLock if
							 * we can work out how to allow concurrent catalog
							 * updates. XXX Might be set down to
							 * ShareRowExclusiveLock but requires further
							 * analysis.
							 */
							cmd_lockmode = AccessExclusiveLock;
							break;
						case CONSTR_FOREIGN:

							/*
							 * We add triggers to both tables when we add a
							 * Foreign Key, so the lock level must be at least
							 * as strong as CREATE TRIGGER.
							 */
							cmd_lockmode = ShareRowExclusiveLock;
							break;

						default:
							cmd_lockmode = AccessExclusiveLock;
					}
				}
				break;

				/*
				 * These subcommands affect inheritance behaviour. Queries
				 * started before us will continue to see the old inheritance
				 * behaviour, while queries started after we commit will see
				 * new behaviour. No need to prevent reads or writes to the
				 * subtable while we hook it up though. Changing the TupDesc
				 * may be a problem, so keep highest lock.
				 */
			case AT_AddInherit:
			case AT_DropInherit:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect implicit row type conversion. They
				 * have affects similar to CREATE/DROP CAST on queries. don't
				 * provide for invalidating parse trees as a result of such
				 * changes, so we keep these at AccessExclusiveLock.
				 */
			case AT_AddOf:
			case AT_DropOf:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Only used by CREATE OR REPLACE VIEW which must conflict
				 * with an SELECTs currently using the view.
				 */
			case AT_ReplaceRelOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect general strategies for performance
				 * and maintenance, though don't change the semantic results
				 * from normal data reads and writes. Delaying an ALTER TABLE
				 * behind currently active writes only delays the point where
				 * the new strategy begins to take effect, so there is no
				 * benefit in waiting. In this case the minimum restriction
				 * applies: we don't currently allow concurrent catalog
				 * updates.
				 */
			case AT_SetStatistics:		/* Uses MVCC in getTableAttrs() */
			case AT_ClusterOn:	/* Uses MVCC in getIndexes() */
			case AT_DropCluster:		/* Uses MVCC in getIndexes() */
			case AT_SetOptions:	/* Uses MVCC in getTableAttrs() */
			case AT_ResetOptions:		/* Uses MVCC in getTableAttrs() */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_SetLogged:
			case AT_SetUnLogged:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_ValidateConstraint: /* Uses MVCC in
												 * getConstraints() */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

				/*
				 * Rel options are more complex than first appears. Options
				 * are set here for tables, views and indexes; for historical
				 * reasons these can all be used with ALTER TABLE, so we can't
				 * decide between them using the basic grammar.
				 *
				 * XXX Look in detail at each option to determine lock level,
				 * e.g. cmd_lockmode = GetRelOptionsLockLevel((List *)
				 * cmd->def);
				 */
			case AT_SetRelOptions:		/* Uses MVCC in getIndexes() and
										 * getTables() */
			case AT_ResetRelOptions:	/* Uses MVCC in getIndexes() and
										 * getTables() */
				cmd_lockmode = AccessExclusiveLock;
				break;

			default:			/* oops */
				elog(ERROR, "unrecognized alter table type: %d",
					 (int) cmd->subtype);
				break;
		}

		/*
		 * Take the greatest lockmode from any subcommand
		 */
		if (cmd_lockmode > lockmode)
			lockmode = cmd_lockmode;
	}

	return lockmode;
}

/*
 * ATController provides top level control over the phases.
 *
 * parsetree is passed in to allow it to be passed to event triggers
 * when requested.
 */
static void
ATController(AlterTableStmt *parsetree,
			 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode)
{
	List	   *wqueue = NIL;
	ListCell   *lcmd;

	/* Phase 1: preliminary examination of commands, create work queue */
	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		ATPrepCmd(&wqueue, rel, cmd, recurse, false, lockmode);
	}

	/* Close the relation, but keep lock until commit */
	relation_close(rel, NoLock);

	/* Phase 2: update system catalogs */
	ATRewriteCatalogs(&wqueue, lockmode);

	/* Phase 3: scan/rewrite tables as needed */
	ATRewriteTables(parsetree, &wqueue, lockmode);
}

/*
 * ATPrepCmd
 *
 * Traffic cop for ALTER TABLE Phase 1 operations, including simple
 * recursion and permission checks.
 *
 * Caller must have acquired appropriate lock type on relation already.
 * This lock should be held until commit.
 */
static void
ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing, LOCKMODE lockmode)
{
	AlteredTableInfo *tab;
	int			pass = AT_PASS_UNSET;

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
			ATSimplePermissions(rel,
						 ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, false, cmd,
							lockmode);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_AddColumnToView:		/* add column via CREATE OR REPLACE
										 * VIEW */
			ATSimplePermissions(rel, ATT_VIEW);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, true, cmd,
							lockmode);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */

			/*
			 * We allow defaults on views so that INSERT into a view can have
			 * default-ish behavior.  This works because the rewriter
			 * substitutes default values into INSERTs before it expands
			 * rules.
			 */
			ATSimplePermissions(rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
			/* No command-specific prep needed */
			pass = cmd->def ? AT_PASS_ADD_CONSTR : AT_PASS_DROP;
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
			/* No command-specific prep needed */
			pass = AT_PASS_DROP;
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
			/* Performs own permission checks */
			ATPrepSetStatistics(rel, cmd->name, cmd->def, lockmode);
			pass = AT_PASS_MISC;
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( options ) */
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( options ) */
			ATSimplePermissions(rel, ATT_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			pass = AT_PASS_MISC;
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
			ATSimplePermissions(rel, ATT_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			ATSimplePermissions(rel,
						 ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepDropColumn(wqueue, rel, recurse, recursing, cmd, lockmode);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_DROP;
			break;
		case AT_AddIndex:		/* ADD INDEX */
			ATSimplePermissions(rel, ATT_TABLE);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_INDEX;
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* Recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_AddConstraintRecurse;
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_AddIndexConstraint:		/* ADD CONSTRAINT USING INDEX */
			ATSimplePermissions(rel, ATT_TABLE);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_DropConstraint:	/* DROP CONSTRAINT */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* Recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_DropConstraintRecurse;
			pass = AT_PASS_DROP;
			break;
		case AT_AlterColumnType:		/* ALTER COLUMN TYPE */
			ATSimplePermissions(rel,
						 ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			/* Performs own recursion */
			ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd, lockmode);
			pass = AT_PASS_ALTER_TYPE;
			break;
		case AT_AlterColumnGenericOptions:
			ATSimplePermissions(rel, ATT_FOREIGN_TABLE);
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
			ATSimplePermissions(rel, ATT_TABLE | ATT_MATVIEW);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_SetLogged:		/* SET LOGGED */
			ATSimplePermissions(rel, ATT_TABLE);
			tab->chgPersistence = ATPrepChangePersistence(rel, true);
			/* force rewrite if necessary; see comment in ATRewriteTables */
			if (tab->chgPersistence)
			{
				tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
				tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
			}
			pass = AT_PASS_MISC;
			break;
		case AT_SetUnLogged:	/* SET UNLOGGED */
			ATSimplePermissions(rel, ATT_TABLE);
			tab->chgPersistence = ATPrepChangePersistence(rel, false);
			/* force rewrite if necessary; see comment in ATRewriteTables */
			if (tab->chgPersistence)
			{
				tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
				tab->newrelpersistence = RELPERSISTENCE_UNLOGGED;
			}
			pass = AT_PASS_MISC;
			break;
		case AT_AddOids:		/* SET WITH OIDS */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			if (!rel->rd_rel->relhasoids || recursing)
				ATPrepAddOids(wqueue, rel, recurse, cmd, lockmode);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* Performs own recursion */
			if (rel->rd_rel->relhasoids)
			{
				AlterTableCmd *dropCmd = makeNode(AlterTableCmd);

				dropCmd->subtype = AT_DropColumn;
				dropCmd->name = pstrdup("oid");
				dropCmd->behavior = cmd->behavior;
				ATPrepCmd(wqueue, rel, dropCmd, recurse, false, lockmode);
			}
			pass = AT_PASS_DROP;
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */
			ATSimplePermissions(rel, ATT_TABLE | ATT_MATVIEW | ATT_INDEX);
			/* This command never recurses */
			ATPrepSetTableSpace(tab, rel, cmd->name, lockmode);
			pass = AT_PASS_MISC;	/* doesn't actually matter */
			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:		/* RESET (...) */
		case AT_ReplaceRelOptions:		/* reset them all, then set just these */
			ATSimplePermissions(rel, ATT_TABLE | ATT_VIEW | ATT_MATVIEW | ATT_INDEX);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_AddInherit:		/* INHERIT */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			ATPrepAddInherit(rel);
			pass = AT_PASS_MISC;
			break;
		case AT_DropInherit:	/* NO INHERIT */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_AlterConstraint:		/* ALTER CONSTRAINT */
			ATSimplePermissions(rel, ATT_TABLE);
			pass = AT_PASS_MISC;
			break;
		case AT_ValidateConstraint:		/* VALIDATE CONSTRAINT */
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* Recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_ValidateConstraintRecurse;
			pass = AT_PASS_MISC;
			break;
		case AT_ReplicaIdentity:		/* REPLICA IDENTITY ... */
			ATSimplePermissions(rel, ATT_TABLE | ATT_MATVIEW);
			pass = AT_PASS_MISC;
			/* This command never recurses */
			/* No command-specific prep needed */
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER variants */
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* DISABLE TRIGGER variants */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
			ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			pass = AT_PASS_MISC;
			break;
		case AT_EnableRule:		/* ENABLE/DISABLE RULE variants */
		case AT_EnableAlwaysRule:
		case AT_EnableReplicaRule:
		case AT_DisableRule:
		case AT_AddOf:			/* OF */
		case AT_DropOf: /* NOT OF */
		case AT_EnableRowSecurity:
		case AT_DisableRowSecurity:
		case AT_ForceRowSecurity:
		case AT_NoForceRowSecurity:
			ATSimplePermissions(rel, ATT_TABLE);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_GenericOptions:
			ATSimplePermissions(rel, ATT_FOREIGN_TABLE);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			pass = AT_PASS_UNSET;		/* keep compiler quiet */
			break;
	}
	Assert(pass > AT_PASS_UNSET);

	/* Add the subcommand to the appropriate list for phase 2 */
	tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}

/*
 * ATRewriteCatalogs
 *
 * Traffic cop for ALTER TABLE Phase 2 operations.  Subcommands are
 * dispatched in a "safe" execution order (designed to avoid unnecessary
 * conflicts).
 */
static void
ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode)
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
			 * Appropriate lock was obtained by phase 1, needn't get it again
			 */
			rel = relation_open(tab->relid, NoLock);

			foreach(lcmd, subcmds)
				ATExecCmd(wqueue, tab, rel, (AlterTableCmd *) lfirst(lcmd), lockmode);

			/*
			 * After the ALTER TYPE pass, do cleanup work (this is not done in
			 * ATExecAlterColumnType since it should be done only once if
			 * multiple columns of a table are altered).
			 */
			if (pass == AT_PASS_ALTER_TYPE)
				ATPostAlterTypeCleanup(wqueue, tab, lockmode);

			relation_close(rel, NoLock);
		}
	}

	/* Check to see if a toast table must be added. */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		if (tab->relkind == RELKIND_RELATION ||
			tab->relkind == RELKIND_MATVIEW)
			AlterTableCreateToastTable(tab->relid, (Datum) 0, lockmode);
	}
}

/*
 * ATExecCmd: dispatch a subcommand to appropriate execution routine
 */
static void
ATExecCmd(List **wqueue, AlteredTableInfo *tab, Relation rel,
		  AlterTableCmd *cmd, LOCKMODE lockmode)
{
	ObjectAddress address = InvalidObjectAddress;

	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
		case AT_AddColumnToView:		/* add column via CREATE OR REPLACE
										 * VIEW */
			address = ATExecAddColumn(wqueue, tab, rel, (ColumnDef *) cmd->def,
									  false, false, false, lockmode);
			break;
		case AT_AddColumnRecurse:
			address = ATExecAddColumn(wqueue, tab, rel, (ColumnDef *) cmd->def,
									  false, true, false, lockmode);
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */
			address = ATExecColumnDefault(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			address = ATExecDropNotNull(rel, cmd->name, lockmode);
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			address = ATExecSetNotNull(tab, rel, cmd->name, lockmode);
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
			address = ATExecSetStatistics(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( options ) */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, false, lockmode);
			break;
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( options ) */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, true, lockmode);
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
			address = ATExecSetStorage(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			address = ATExecDropColumn(wqueue, rel, cmd->name,
									   cmd->behavior, false, false,
									   cmd->missing_ok, lockmode);
			break;
		case AT_DropColumnRecurse:		/* DROP COLUMN with recursion */
			address = ATExecDropColumn(wqueue, rel, cmd->name,
									   cmd->behavior, true, false,
									   cmd->missing_ok, lockmode);
			break;
		case AT_AddIndex:		/* ADD INDEX */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, false,
									 lockmode);
			break;
		case AT_ReAddIndex:		/* ADD INDEX */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, true,
									 lockmode);
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			address =
				ATExecAddConstraint(wqueue, tab, rel, (Constraint *) cmd->def,
									false, false, lockmode);
			break;
		case AT_AddConstraintRecurse:	/* ADD CONSTRAINT with recursion */
			address =
				ATExecAddConstraint(wqueue, tab, rel, (Constraint *) cmd->def,
									true, false, lockmode);
			break;
		case AT_ReAddConstraint:		/* Re-add pre-existing check
										 * constraint */
			address =
				ATExecAddConstraint(wqueue, tab, rel, (Constraint *) cmd->def,
									true, true, lockmode);
			break;
		case AT_ReAddComment:	/* Re-add existing comment */
			address = CommentObject((CommentStmt *) cmd->def);
			break;
		case AT_AddIndexConstraint:		/* ADD CONSTRAINT USING INDEX */
			address = ATExecAddIndexConstraint(tab, rel, (IndexStmt *) cmd->def,
											   lockmode);
			break;
		case AT_AlterConstraint:		/* ALTER CONSTRAINT */
			address = ATExecAlterConstraint(rel, cmd, false, false, lockmode);
			break;
		case AT_ValidateConstraint:		/* VALIDATE CONSTRAINT */
			address = ATExecValidateConstraint(rel, cmd->name, false, false,
											   lockmode);
			break;
		case AT_ValidateConstraintRecurse:		/* VALIDATE CONSTRAINT with
												 * recursion */
			address = ATExecValidateConstraint(rel, cmd->name, true, false,
											   lockmode);
			break;
		case AT_DropConstraint:	/* DROP CONSTRAINT */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior,
								 false, false,
								 cmd->missing_ok, lockmode);
			break;
		case AT_DropConstraintRecurse:	/* DROP CONSTRAINT with recursion */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior,
								 true, false,
								 cmd->missing_ok, lockmode);
			break;
		case AT_AlterColumnType:		/* ALTER COLUMN TYPE */
			address = ATExecAlterColumnType(tab, rel, cmd, lockmode);
			break;
		case AT_AlterColumnGenericOptions:		/* ALTER COLUMN OPTIONS */
			address =
				ATExecAlterColumnGenericOptions(rel, cmd->name,
												(List *) cmd->def, lockmode);
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			ATExecChangeOwner(RelationGetRelid(rel),
							  get_rolespec_oid(cmd->newowner, false),
							  false, lockmode);
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
			address = ATExecClusterOn(rel, cmd->name, lockmode);
			break;
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATExecDropCluster(rel, lockmode);
			break;
		case AT_SetLogged:		/* SET LOGGED */
		case AT_SetUnLogged:	/* SET UNLOGGED */
			break;
		case AT_AddOids:		/* SET WITH OIDS */
			/* Use the ADD COLUMN code, unless prep decided to do nothing */
			if (cmd->def != NULL)
				address =
					ATExecAddColumn(wqueue, tab, rel, (ColumnDef *) cmd->def,
									true, false, false, lockmode);
			break;
		case AT_AddOidsRecurse:	/* SET WITH OIDS */
			/* Use the ADD COLUMN code, unless prep decided to do nothing */
			if (cmd->def != NULL)
				address =
					ATExecAddColumn(wqueue, tab, rel, (ColumnDef *) cmd->def,
									true, true, false, lockmode);
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
		case AT_ResetRelOptions:		/* RESET (...) */
		case AT_ReplaceRelOptions:		/* replace entire option list */
			ATExecSetRelOptions(rel, (List *) cmd->def, cmd->subtype, lockmode);
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
								   TRIGGER_FIRES_ON_ORIGIN, false, lockmode);
			break;
		case AT_EnableAlwaysTrig:		/* ENABLE ALWAYS TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ALWAYS, false, lockmode);
			break;
		case AT_EnableReplicaTrig:		/* ENABLE REPLICA TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
								  TRIGGER_FIRES_ON_REPLICA, false, lockmode);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_DISABLED, false, lockmode);
			break;
		case AT_EnableTrigAll:	/* ENABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
								   TRIGGER_FIRES_ON_ORIGIN, false, lockmode);
			break;
		case AT_DisableTrigAll:	/* DISABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, false, lockmode);
			break;
		case AT_EnableTrigUser:	/* ENABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									TRIGGER_FIRES_ON_ORIGIN, true, lockmode);
			break;
		case AT_DisableTrigUser:		/* DISABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, true, lockmode);
			break;

		case AT_EnableRule:		/* ENABLE RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_ORIGIN, lockmode);
			break;
		case AT_EnableAlwaysRule:		/* ENABLE ALWAYS RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ALWAYS, lockmode);
			break;
		case AT_EnableReplicaRule:		/* ENABLE REPLICA RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_REPLICA, lockmode);
			break;
		case AT_DisableRule:	/* DISABLE RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_DISABLED, lockmode);
			break;

		case AT_AddInherit:
			address = ATExecAddInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_DropInherit:
			address = ATExecDropInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_AddOf:
			address = ATExecAddOf(rel, (TypeName *) cmd->def, lockmode);
			break;
		case AT_DropOf:
			ATExecDropOf(rel, lockmode);
			break;
		case AT_ReplicaIdentity:
			ATExecReplicaIdentity(rel, (ReplicaIdentityStmt *) cmd->def, lockmode);
			break;
		case AT_EnableRowSecurity:
			ATExecEnableRowSecurity(rel);
			break;
		case AT_DisableRowSecurity:
			ATExecDisableRowSecurity(rel);
			break;
		case AT_ForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, true);
			break;
		case AT_NoForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, false);
			break;
		case AT_GenericOptions:
			ATExecGenericOptions(rel, (List *) cmd->def);
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			break;
	}

	/*
	 * Report the subcommand to interested event triggers.
	 */
	EventTriggerCollectAlterTableSubcmd((Node *) cmd, address);

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
ATRewriteTables(AlterTableStmt *parsetree, List **wqueue, LOCKMODE lockmode)
{
	ListCell   *ltab;

	/* Go through each table that needs to be checked or rewritten */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/* Foreign tables have no storage. */
		if (tab->relkind == RELKIND_FOREIGN_TABLE)
			continue;

		/*
		 * If we change column data types or add/remove OIDs, the operation
		 * has to be propagated to tables that use this table's rowtype as a
		 * column type.  tab->newvals will also be non-NULL in the case where
		 * we're adding a column with a default.  We choose to forbid that
		 * case as well, since composite types might eventually support
		 * defaults.
		 *
		 * (Eventually we'll probably need to check for composite type
		 * dependencies even when we're just scanning the table without a
		 * rewrite, but at the moment a composite type does not enforce any
		 * constraints, so it's not necessary/appropriate to enforce them just
		 * during ALTER.)
		 */
		if (tab->newvals != NIL || tab->rewrite > 0)
		{
			Relation	rel;

			rel = heap_open(tab->relid, NoLock);
			find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
			heap_close(rel, NoLock);
		}

		/*
		 * We only need to rewrite the table if at least one column needs to
		 * be recomputed, we are adding/removing the OID column, or we are
		 * changing its persistence.
		 *
		 * There are two reasons for requiring a rewrite when changing
		 * persistence: on one hand, we need to ensure that the buffers
		 * belonging to each of the two relations are marked with or without
		 * BM_PERMANENT properly.  On the other hand, since rewriting creates
		 * and assigns a new relfilenode, we automatically create or drop an
		 * init fork for the relation as appropriate.
		 */
		if (tab->rewrite > 0)
		{
			/* Build a temporary relation and copy data */
			Relation	OldHeap;
			Oid			OIDNewHeap;
			Oid			NewTableSpace;
			char		persistence;

			OldHeap = heap_open(tab->relid, NoLock);

			/*
			 * We don't support rewriting of system catalogs; there are too
			 * many corner cases and too little benefit.  In particular this
			 * is certainly not going to work for mapped catalogs.
			 */
			if (IsSystemRelation(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite system relation \"%s\"",
								RelationGetRelationName(OldHeap))));

			if (RelationIsUsedAsCatalogTable(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot rewrite table \"%s\" used as a catalog table",
					   RelationGetRelationName(OldHeap))));

			/*
			 * Don't allow rewrite on temp tables of other backends ... their
			 * local buffer manager is not going to cope.
			 */
			if (RELATION_IS_OTHER_TEMP(OldHeap))
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

			/*
			 * Select persistence of transient table (same as original unless
			 * user requested a change)
			 */
			persistence = tab->chgPersistence ?
				tab->newrelpersistence : OldHeap->rd_rel->relpersistence;

			heap_close(OldHeap, NoLock);

			/*
			 * Fire off an Event Trigger now, before actually rewriting the
			 * table.
			 *
			 * We don't support Event Trigger for nested commands anywhere,
			 * here included, and parsetree is given NULL when coming from
			 * AlterTableInternal.
			 *
			 * And fire it only once.
			 */
			if (parsetree)
				EventTriggerTableRewrite((Node *) parsetree,
										 tab->relid,
										 tab->rewrite);

			/*
			 * Create transient table that will receive the modified data.
			 *
			 * Ensure it is marked correctly as logged or unlogged.  We have
			 * to do this here so that buffers for the new relfilenode will
			 * have the right persistence set, and at the same time ensure
			 * that the original filenode's buffers will get read in with the
			 * correct setting (i.e. the original one).  Otherwise a rollback
			 * after the rewrite would possibly result with buffers for the
			 * original filenode having the wrong persistence setting.
			 *
			 * NB: This relies on swap_relation_files() also swapping the
			 * persistence. That wouldn't work for pg_class, but that can't be
			 * unlogged anyway.
			 */
			OIDNewHeap = make_new_heap(tab->relid, NewTableSpace, persistence,
									   lockmode);

			/*
			 * Copy the heap data into the new table with the desired
			 * modifications, and test the current data within the table
			 * against new constraints generated by ALTER TABLE commands.
			 */
			ATRewriteTable(tab, OIDNewHeap, lockmode);

			/*
			 * Swap the physical files of the old and new heaps, then rebuild
			 * indexes and discard the old heap.  We can use RecentXmin for
			 * the table's new relfrozenxid because we rewrote all the tuples
			 * in ATRewriteTable, so no older Xid remains in the table.  Also,
			 * we never try to swap toast tables by content, since we have no
			 * interest in letting this code work on system catalogs.
			 */
			finish_heap_swap(tab->relid, OIDNewHeap,
							 false, false, true,
							 !OidIsValid(tab->newTableSpace),
							 RecentXmin,
							 ReadNextMultiXactId(),
							 persistence);
		}
		else
		{
			/*
			 * Test the current data within the table against new constraints
			 * generated by ALTER TABLE commands, but don't rebuild data.
			 */
			if (tab->constraints != NIL || tab->new_notnull)
				ATRewriteTable(tab, InvalidOid, lockmode);

			/*
			 * If we had SET TABLESPACE but no reason to reconstruct tuples,
			 * just do a block-by-block copy.
			 */
			if (tab->newTableSpace)
				ATExecSetTableSpace(tab->relid, tab->newTableSpace, lockmode);
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
				Constraint *fkconstraint = (Constraint *) con->qual;
				Relation	refrel;

				if (rel == NULL)
				{
					/* Long since locked, no need for another */
					rel = heap_open(tab->relid, NoLock);
				}

				refrel = heap_open(con->refrelid, RowShareLock);

				validateForeignKeyConstraint(fkconstraint->conname, rel, refrel,
											 con->refindid,
											 con->conid);

				/*
				 * No need to mark the constraint row as validated, we did
				 * that when we inserted the row earlier.
				 */

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
ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap, LOCKMODE lockmode)
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
	CommandId	mycid;
	BulkInsertState bistate;
	int			hi_options;

	/*
	 * Open the relation(s).  We have surely already locked the existing
	 * table.
	 */
	oldrel = heap_open(tab->relid, NoLock);
	oldTupDesc = tab->oldDesc;
	newTupDesc = RelationGetDescr(oldrel);		/* includes all mods */

	if (OidIsValid(OIDNewHeap))
		newrel = heap_open(OIDNewHeap, lockmode);
	else
		newrel = NULL;

	/*
	 * Prepare a BulkInsertState and options for heap_insert. Because we're
	 * building a new heap, we can skip WAL-logging and fsync it to disk at
	 * the end instead (unless WAL-logging is required for archiving or
	 * streaming replication). The FSM is empty too, so don't bother using it.
	 */
	if (newrel)
	{
		mycid = GetCurrentCommandId(true);
		bistate = GetBulkInsertState();

		hi_options = HEAP_INSERT_SKIP_FSM;
		if (!XLogIsNeeded())
			hi_options |= HEAP_INSERT_SKIP_WAL;
	}
	else
	{
		/* keep compiler quiet about using these uninitialized */
		mycid = 0;
		bistate = NULL;
		hi_options = 0;
	}

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

		/* expr already planned */
		ex->exprstate = ExecInitExpr((Expr *) ex->expr, NULL);
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

	if (newrel || needscan)
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
		Snapshot	snapshot;

		if (newrel)
			ereport(DEBUG1,
					(errmsg("rewriting table \"%s\"",
							RelationGetRelationName(oldrel))));
		else
			ereport(DEBUG1,
					(errmsg("verifying table \"%s\"",
							RelationGetRelationName(oldrel))));

		if (newrel)
		{
			/*
			 * All predicate locks on the tuples or pages are about to be made
			 * invalid, because we move tuples around.  Promote them to
			 * relation locks.
			 */
			TransferPredicateLocksToHeapRelation(oldrel);
		}

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
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = heap_beginscan(oldrel, snapshot, 0, NULL);

		/*
		 * Switch to per-tuple memory context and reset it for each tuple
		 * produced, so we don't leak memory.
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			if (tab->rewrite > 0)
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

				/*
				 * Constraints might reference the tableoid column, so
				 * initialize t_tableOid before evaluating them.
				 */
				tuple->t_tableOid = RelationGetRelid(oldrel);
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
								  NameStr(newTupDesc->attrs[attn]->attname)),
							 errtablecol(oldrel, attn + 1)));
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
											con->name),
									 errtableconstraint(oldrel, con->name)));
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
				heap_insert(newrel, tuple, mycid, hi_options, bistate);

			ResetExprContext(econtext);

			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		heap_endscan(scan);
		UnregisterSnapshot(snapshot);

		ExecDropSingleTupleTableSlot(oldslot);
		ExecDropSingleTupleTableSlot(newslot);
	}

	FreeExecutorState(estate);

	heap_close(oldrel, NoLock);
	if (newrel)
	{
		FreeBulkInsertState(bistate);

		/* If we skipped writing WAL, then we need to sync the heap. */
		if (hi_options & HEAP_INSERT_SKIP_WAL)
			heap_sync(newrel);

		heap_close(newrel, NoLock);
	}
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
	tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
	tab->chgPersistence = false;

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
ATSimplePermissions(Relation rel, int allowed_targets)
{
	int			actual_target;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
			actual_target = ATT_TABLE;
			break;
		case RELKIND_VIEW:
			actual_target = ATT_VIEW;
			break;
		case RELKIND_MATVIEW:
			actual_target = ATT_MATVIEW;
			break;
		case RELKIND_INDEX:
			actual_target = ATT_INDEX;
			break;
		case RELKIND_COMPOSITE_TYPE:
			actual_target = ATT_COMPOSITE_TYPE;
			break;
		case RELKIND_FOREIGN_TABLE:
			actual_target = ATT_FOREIGN_TABLE;
			break;
		default:
			actual_target = 0;
			break;
	}

	/* Wrong target type? */
	if ((actual_target & allowed_targets) == 0)
		ATWrongRelkindError(rel, allowed_targets);

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
 * ATWrongRelkindError
 *
 * Throw an error when a relation has been determined to be of the wrong
 * type.
 */
static void
ATWrongRelkindError(Relation rel, int allowed_targets)
{
	char	   *msg;

	switch (allowed_targets)
	{
		case ATT_TABLE:
			msg = _("\"%s\" is not a table");
			break;
		case ATT_TABLE | ATT_VIEW:
			msg = _("\"%s\" is not a table or view");
			break;
		case ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, view, or foreign table");
			break;
		case ATT_TABLE | ATT_VIEW | ATT_MATVIEW | ATT_INDEX:
			msg = _("\"%s\" is not a table, view, materialized view, or index");
			break;
		case ATT_TABLE | ATT_MATVIEW:
			msg = _("\"%s\" is not a table or materialized view");
			break;
		case ATT_TABLE | ATT_MATVIEW | ATT_INDEX:
			msg = _("\"%s\" is not a table, materialized view, or index");
			break;
		case ATT_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, materialized view, or foreign table");
			break;
		case ATT_TABLE | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table or foreign table");
			break;
		case ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, composite type, or foreign table");
			break;
		case ATT_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, materialized view, index, or foreign table");
			break;
		case ATT_VIEW:
			msg = _("\"%s\" is not a view");
			break;
		case ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a foreign table");
			break;
		default:
			/* shouldn't get here, add all necessary cases above */
			msg = _("\"%s\" is of the wrong type");
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg(msg, RelationGetRelationName(rel))));
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
				  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode)
{
	/*
	 * Propagate to children if desired.  Only plain tables and foreign tables
	 * have children, so no need to search for other relkinds.
	 */
	if (recurse &&
		(rel->rd_rel->relkind == RELKIND_RELATION ||
		 rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE))
	{
		Oid			relid = RelationGetRelid(rel);
		ListCell   *child;
		List	   *children;

		children = find_all_inheritors(relid, lockmode, NULL);

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
			/* find_all_inheritors already got lock */
			childrel = relation_open(childrelid, NoLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode);
			relation_close(childrel, NoLock);
		}
	}
}

/*
 * ATTypedTableRecursion
 *
 * Propagate ALTER TYPE operations to the typed tables of that type.
 * Also check the RESTRICT/CASCADE behavior.  Given CASCADE, also permit
 * recursion to inheritance children of the typed tables.
 */
static void
ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  LOCKMODE lockmode)
{
	ListCell   *child;
	List	   *children;

	Assert(rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

	children = find_typed_table_dependencies(rel->rd_rel->reltype,
											 RelationGetRelationName(rel),
											 cmd->behavior);

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;

		childrel = relation_open(childrelid, lockmode);
		CheckTableNotInUse(childrel, "ALTER TABLE");
		ATPrepCmd(wqueue, childrel, cmd, true, true, lockmode);
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
find_composite_type_dependencies(Oid typeOid, Relation origRelation,
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
								 NULL, 2, key);

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

		if (rel->rd_rel->relkind == RELKIND_RELATION ||
			rel->rd_rel->relkind == RELKIND_MATVIEW)
		{
			if (origTypeName)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
								origTypeName,
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else if (origRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else if (origRelation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter foreign table \"%s\" because column \"%s.%s\" uses its row type",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter table \"%s\" because column \"%s.%s\" uses its row type",
								RelationGetRelationName(origRelation),
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
											 origRelation, origTypeName);
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
		find_composite_type_dependencies(arrayOid, origRelation, origTypeName);
}


/*
 * find_typed_table_dependencies
 *
 * Check to see if a composite type is being used as the type of a
 * typed table.  Abort if any are found and behavior is RESTRICT.
 * Else return the list of tables.
 */
static List *
find_typed_table_dependencies(Oid typeOid, const char *typeName, DropBehavior behavior)
{
	Relation	classRel;
	ScanKeyData key[1];
	HeapScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	classRel = heap_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_reloftype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = heap_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		if (behavior == DROP_RESTRICT)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot alter type \"%s\" because it is the type of a typed table",
							typeName),
			errhint("Use ALTER ... CASCADE to alter the typed tables too.")));
		else
			result = lappend_oid(result, HeapTupleGetOid(tuple));
	}

	heap_endscan(scan);
	heap_close(classRel, AccessShareLock);

	return result;
}


/*
 * check_of_type
 *
 * Check whether a type is suitable for CREATE TABLE OF/ALTER TABLE OF.  If it
 * isn't suitable, throw an error.  Currently, we require that the type
 * originated with CREATE TYPE AS.  We could support any row type, but doing so
 * would require handling a number of extra corner cases in the DDL commands.
 */
void
check_of_type(HeapTuple typetuple)
{
	Form_pg_type typ = (Form_pg_type) GETSTRUCT(typetuple);
	bool		typeOk = false;

	if (typ->typtype == TYPTYPE_COMPOSITE)
	{
		Relation	typeRelation;

		Assert(OidIsValid(typ->typrelid));
		typeRelation = relation_open(typ->typrelid, AccessShareLock);
		typeOk = (typeRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

		/*
		 * Close the parent rel, but keep our AccessShareLock on it until xact
		 * commit.  That will prevent someone else from deleting or ALTERing
		 * the type before the typed table creation/conversion commits.
		 */
		relation_close(typeRelation, NoLock);
	}
	if (!typeOk)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("type %s is not a composite type",
						format_type_be(HeapTupleGetOid(typetuple)))));
}


/*
 * ALTER TABLE ADD COLUMN
 *
 * Adds an additional attribute to a relation making the assumption that
 * CHECK, NOT NULL, and FOREIGN KEY constraints will be removed from the
 * AT_AddColumn AlterTableCmd by parse_utilcmd.c and added as independent
 * AlterTableCmd's.
 *
 * ADD COLUMN cannot use the normal ALTER TABLE recursion mechanism, because we
 * have to decide at runtime whether to recurse or not depending on whether we
 * actually add a column or merely merge with an existing column.  (We can't
 * check this in a static pre-pass because it won't handle multiple inheritance
 * situations correctly.)
 */
static void
ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode);

	if (recurse && !is_view)
		cmd->subtype = AT_AddColumnRecurse;
}

/*
 * Add a column to a table; this handles the AT_AddOids cases as well.  The
 * return value is the address of the new column in the parent relation.
 */
static ObjectAddress
ATExecAddColumn(List **wqueue, AlteredTableInfo *tab, Relation rel,
				ColumnDef *colDef, bool isOid,
				bool recurse, bool recursing, LOCKMODE lockmode)
{
	Oid			myrelid = RelationGetRelid(rel);
	Relation	pgclass,
				attrdesc;
	HeapTuple	reltup;
	FormData_pg_attribute attribute;
	int			newattnum;
	char		relkind;
	HeapTuple	typeTuple;
	Oid			typeOid;
	int32		typmod;
	Oid			collOid;
	Form_pg_type tform;
	Expr	   *defval;
	List	   *children;
	ListCell   *child;
	AclResult	aclresult;
	ObjectAddress address;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	attrdesc = heap_open(AttributeRelationId, RowExclusiveLock);

	/*
	 * Are we adding the column to a recursion child?  If so, check whether to
	 * merge with an existing definition for the column.  If we do merge, we
	 * must not recurse.  Children will already have the column, and recursing
	 * into them would mess up attinhcount.
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
			Oid			ccollid;

			/* Child column must match on type, typmod, and collation */
			typenameTypeIdAndMod(NULL, colDef->typeName, &ctypeId, &ctypmod);
			if (ctypeId != childatt->atttypid ||
				ctypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
							RelationGetRelationName(rel), colDef->colname)));
			ccollid = GetColumnDefCollation(NULL, colDef, ctypeId);
			if (ccollid != childatt->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
							  RelationGetRelationName(rel), colDef->colname),
						 errdetail("\"%s\" versus \"%s\"",
								   get_collation_name(ccollid),
							   get_collation_name(childatt->attcollation))));

			/* If it's OID, child column must actually be OID */
			if (isOid && childatt->attnum != ObjectIdAttributeNumber)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("child table \"%s\" has a conflicting \"%s\" column",
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
			return InvalidObjectAddress;
		}
	}

	pgclass = heap_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relkind = ((Form_pg_class) GETSTRUCT(reltup))->relkind;

	/* new name should not already exist */
	check_for_column_name_collision(rel, colDef->colname);

	/* Determine the new attribute's number */
	if (isOid)
		newattnum = ObjectIdAttributeNumber;
	else
	{
		newattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts + 1;
		if (newattnum > MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("tables can have at most %d columns",
							MaxHeapAttributeNumber)));
	}

	typeTuple = typenameType(NULL, colDef->typeName, &typmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	typeOid = HeapTupleGetOid(typeTuple);

	aclresult = pg_type_aclcheck(typeOid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, typeOid);

	collOid = GetColumnDefCollation(NULL, colDef, typeOid);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colDef->colname, typeOid, collOid,
					   list_make1_oid(rel->rd_rel->reltype),
					   false);

	/* construct new attribute's pg_attribute entry */
	attribute.attrelid = myrelid;
	namestrcpy(&(attribute.attname), colDef->colname);
	attribute.atttypid = typeOid;
	attribute.attstattarget = (newattnum > 0) ? -1 : 0;
	attribute.attlen = tform->typlen;
	attribute.attcacheoff = -1;
	attribute.atttypmod = typmod;
	attribute.attnum = newattnum;
	attribute.attbyval = tform->typbyval;
	attribute.attndims = list_length(colDef->typeName->arrayBounds);
	attribute.attstorage = tform->typstorage;
	attribute.attalign = tform->typalign;
	attribute.attnotnull = colDef->is_not_null;
	attribute.atthasdef = false;
	attribute.attisdropped = false;
	attribute.attislocal = colDef->is_local;
	attribute.attinhcount = colDef->inhcount;
	attribute.attcollation = collOid;
	/* attribute.attacl is handled by InsertPgAttributeTuple */

	ReleaseSysCache(typeTuple);

	InsertPgAttributeTuple(attrdesc, &attribute, NULL);

	heap_close(attrdesc, RowExclusiveLock);

	/*
	 * Update pg_class tuple as appropriate
	 */
	if (isOid)
		((Form_pg_class) GETSTRUCT(reltup))->relhasoids = true;
	else
		((Form_pg_class) GETSTRUCT(reltup))->relnatts = newattnum;

	simple_heap_update(pgclass, &reltup->t_self, reltup);

	/* keep catalog indexes current */
	CatalogUpdateIndexes(pgclass, reltup);

	heap_freetuple(reltup);

	/* Post creation hook for new attribute */
	InvokeObjectPostCreateHook(RelationRelationId, myrelid, newattnum);

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
		rawEnt->attnum = attribute.attnum;
		rawEnt->raw_default = copyObject(colDef->raw_default);

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false);

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
	 * returned by AddRelationNewConstraints, so that the right thing happens
	 * when a datatype's default applies.
	 *
	 * We skip this step completely for views and foreign tables.  For a view,
	 * we can only get here from CREATE OR REPLACE VIEW, which historically
	 * doesn't set up defaults, not even for domain-typed columns.  And in any
	 * case we mustn't invoke Phase 3 on a view or foreign table, since they
	 * have no storage.
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE
		&& relkind != RELKIND_FOREIGN_TABLE && attribute.attnum > 0)
	{
		defval = (Expr *) build_column_default(rel, attribute.attnum);

		if (!defval && DomainHasConstraints(typeOid))
		{
			Oid			baseTypeId;
			int32		baseTypeMod;
			Oid			baseTypeColl;

			baseTypeMod = typmod;
			baseTypeId = getBaseTypeAndTypmod(typeOid, &baseTypeMod);
			baseTypeColl = get_typcollation(baseTypeId);
			defval = (Expr *) makeNullConst(baseTypeId, baseTypeMod, baseTypeColl);
			defval = (Expr *) coerce_to_target_type(NULL,
													(Node *) defval,
													baseTypeId,
													typeOid,
													typmod,
													COERCION_ASSIGNMENT,
													COERCE_IMPLICIT_CAST,
													-1);
			if (defval == NULL) /* should not happen */
				elog(ERROR, "failed to coerce base type to domain");
		}

		if (defval)
		{
			NewColumnValue *newval;

			newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
			newval->attnum = attribute.attnum;
			newval->expr = expression_planner(defval);

			tab->newvals = lappend(tab->newvals, newval);
			tab->rewrite |= AT_REWRITE_DEFAULT_VAL;
		}

		/*
		 * If the new column is NOT NULL, tell Phase 3 it needs to test that.
		 * (Note we don't do this for an OID column.  OID will be marked not
		 * null, but since it's filled specially, there's no need to test
		 * anything.)
		 */
		tab->new_notnull |= colDef->is_not_null;
	}

	/*
	 * If we are adding an OID column, we have to tell Phase 3 to rewrite the
	 * table to fix that.
	 */
	if (isOid)
		tab->rewrite |= AT_REWRITE_ALTER_OID;

	/*
	 * Add needed dependency entries for the new column.
	 */
	add_column_datatype_dependency(myrelid, newattnum, attribute.atttypid);
	add_column_collation_dependency(myrelid, newattnum, attribute.attcollation);

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	children = find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * If we are told not to recurse, there had better not be any child
	 * tables; else the addition would put them out of step.
	 */
	if (children && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column must be added to child tables too")));

	/* Children should see column as singly inherited */
	if (!recursing)
	{
		colDef = copyObject(colDef);
		colDef->inhcount = 1;
		colDef->is_local = false;
	}

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children already got lock */
		childrel = heap_open(childrelid, NoLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");

		/* Find or create work queue entry for this table */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to child; return value is ignored */
		ATExecAddColumn(wqueue, childtab, childrel,
						colDef, isOid, recurse, true, lockmode);

		heap_close(childrel, NoLock);
	}

	ObjectAddressSubSet(address, RelationRelationId, myrelid, newattnum);
	return address;
}

/*
 * If a new or renamed column will collide with the name of an existing
 * column, error out.
 */
static void
check_for_column_name_collision(Relation rel, const char *colname)
{
	HeapTuple	attTuple;
	int			attnum;

	/*
	 * this test is deliberately not attisdropped-aware, since if one tries to
	 * add a column matching a dropped column name, it's gonna fail anyway.
	 */
	attTuple = SearchSysCache2(ATTNAME,
							   ObjectIdGetDatum(RelationGetRelid(rel)),
							   PointerGetDatum(colname));
	if (!HeapTupleIsValid(attTuple))
		return;

	attnum = ((Form_pg_attribute) GETSTRUCT(attTuple))->attnum;
	ReleaseSysCache(attTuple);

	/*
	 * We throw a different error message for conflicts with system column
	 * names, since they are normally not shown and the user might otherwise
	 * be confused about the reason for the conflict.
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
			 errmsg("column name \"%s\" conflicts with a system column name",
					colname)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
						colname, RelationGetRelationName(rel))));
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
 * Install a column's dependency on its collation.
 */
static void
add_column_collation_dependency(Oid relid, int32 attnum, Oid collid)
{
	ObjectAddress myself,
				referenced;

	/* We know the default collation is pinned, so don't bother recording it */
	if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID)
	{
		myself.classId = RelationRelationId;
		myself.objectId = relid;
		myself.objectSubId = attnum;
		referenced.classId = CollationRelationId;
		referenced.objectId = collid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}

/*
 * ALTER TABLE SET WITH OIDS
 *
 * Basically this is an ADD COLUMN for the special OID column.  We have
 * to cons up a ColumnDef node because the ADD COLUMN code needs one.
 */
static void
ATPrepAddOids(List **wqueue, Relation rel, bool recurse, AlterTableCmd *cmd, LOCKMODE lockmode)
{
	/* If we're recursing to a child table, the ColumnDef is already set up */
	if (cmd->def == NULL)
	{
		ColumnDef  *cdef = makeNode(ColumnDef);

		cdef->colname = pstrdup("oid");
		cdef->typeName = makeTypeNameFromOid(OIDOID, -1);
		cdef->inhcount = 0;
		cdef->is_local = true;
		cdef->is_not_null = true;
		cdef->storage = 0;
		cdef->location = -1;
		cmd->def = (Node *) cdef;
	}
	ATPrepAddColumn(wqueue, rel, recurse, false, false, cmd, lockmode);

	if (recurse)
		cmd->subtype = AT_AddOidsRecurse;
}

/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 *
 * Return the address of the modified column.  If the column was already
 * nullable, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecDropNotNull(Relation rel, const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	ObjectAddress address;

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
	 *
	 * Note: we'll throw error even if the pkey index is not valid.
	 */

	/* Loop over all indexes on the relation */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;
		int			i;

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
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

		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	heap_close(attr_rel, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 *
 * Return the address of the modified column.  If the column was already NOT
 * NULL, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	ObjectAddress address;

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

		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	heap_close(attr_rel, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault, LOCKMODE lockmode)
{
	AttrNumber	attnum;
	ObjectAddress address;

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
	 *
	 * We treat removing the existing default as an internal operation when it
	 * is preparatory to adding a new default, but as a user-initiated
	 * operation when the user asked for a drop.
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  newDefault == NULL ? false : true);

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
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 */
static void
ATPrepSetStatistics(Relation rel, const char *colName, Node *newValue, LOCKMODE lockmode)
{
	/*
	 * We do our own permission checking because (a) we want to allow SET
	 * STATISTICS on indexes (for expressional index columns), and (b) we want
	 * to allow SET STATISTICS on system catalogs without requiring
	 * allowSystemTableMods to be turned on.
	 */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW &&
		rel->rd_rel->relkind != RELKIND_INDEX &&
		rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, materialized view, index, or foreign table",
						RelationGetRelationName(rel))));

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));
}

/*
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetStatistics(Relation rel, const char *colName, Node *newValue, LOCKMODE lockmode)
{
	int			newtarget;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;

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
	else if (newtarget > 10000)
	{
		newtarget = 10000;
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

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	attrtuple->attstattarget = newtarget;

	simple_heap_update(attrelation, &tuple->t_self, tuple);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	heap_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetOptions(Relation rel, const char *colName, Node *options,
				 bool isReset, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple,
				newtuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	Datum		datum,
				newOptions;
	bool		isnull;
	ObjectAddress address;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];

	attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/* Generate new proposed attoptions (text array) */
	Assert(IsA(options, List));
	datum = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions,
							&isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 (List *) options, NULL, NULL, false,
									 isReset);
	/* Validate new options */
	(void) attribute_reloptions(newOptions, true);

	/* Build new tuple. */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_attribute_attoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_attribute_attoptions - 1] = true;
	repl_repl[Anum_pg_attribute_attoptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrelation),
								 repl_val, repl_null, repl_repl);

	/* Update system catalog. */
	simple_heap_update(attrelation, &newtuple->t_self, newtuple);
	CatalogUpdateIndexes(attrelation, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	heap_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET STORAGE
 *
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetStorage(Relation rel, const char *colName, Node *newValue, LOCKMODE lockmode)
{
	char	   *storagemode;
	char		newstorage;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;

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

	attnum = attrtuple->attnum;
	if (attnum <= 0)
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

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);

	heap_freetuple(tuple);

	heap_close(attrelation, RowExclusiveLock);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * ALTER TABLE DROP COLUMN
 *
 * DROP COLUMN cannot use the normal ALTER TABLE recursion mechanism,
 * because we have to decide at runtime whether to recurse or not depending
 * on whether attinhcount goes to zero or not.  (We can't check this in a
 * static pre-pass because it won't handle multiple inheritance situations
 * correctly.)
 */
static void
ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				 AlterTableCmd *cmd, LOCKMODE lockmode)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop column from typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode);

	if (recurse)
		cmd->subtype = AT_DropColumnRecurse;
}

/*
 * Return value is the address of the dropped column.
 */
static ObjectAddress
ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing,
				 bool missing_ok, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute targetatt;
	AttrNumber	attnum;
	List	   *children;
	ObjectAddress object;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colName, RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" does not exist, skipping",
							colName, RelationGetRelationName(rel))));
			return InvalidObjectAddress;
		}
	}
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
	children = find_inheritance_children(RelationGetRelid(rel), lockmode);

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

			/* find_inheritance_children already got lock */
			childrel = heap_open(childrelid, NoLock);
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
					ATExecDropColumn(wqueue, childrel, colName,
									 behavior, true, true,
									 false, lockmode);
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
				 * we need to mark the inheritors' attributes as locally
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

	performDeletion(&object, behavior, 0);

	/*
	 * If we dropped the OID column, must adjust pg_class.relhasoids and tell
	 * Phase 3 to physically get rid of the column.  We formerly left the
	 * column in place physically, but this caused subtle problems.  See
	 * http://archives.postgresql.org/pgsql-hackers/2009-02/msg00363.php
	 */
	if (attnum == ObjectIdAttributeNumber)
	{
		Relation	class_rel;
		Form_pg_class tuple_class;
		AlteredTableInfo *tab;

		class_rel = heap_open(RelationRelationId, RowExclusiveLock);

		tuple = SearchSysCacheCopy1(RELOID,
									ObjectIdGetDatum(RelationGetRelid(rel)));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u",
				 RelationGetRelid(rel));
		tuple_class = (Form_pg_class) GETSTRUCT(tuple);

		tuple_class->relhasoids = false;
		simple_heap_update(class_rel, &tuple->t_self, tuple);

		/* Keep the catalog indexes up to date */
		CatalogUpdateIndexes(class_rel, tuple);

		heap_close(class_rel, RowExclusiveLock);

		/* Find or create work queue entry for this table */
		tab = ATGetQueueEntry(wqueue, rel);

		/* Tell Phase 3 to physically remove the OID column */
		tab->rewrite |= AT_REWRITE_ALTER_OID;
	}

	return object;
}

/*
 * ALTER TABLE ADD INDEX
 *
 * There is no such command in the grammar, but parse_utilcmd.c converts
 * UNIQUE and PRIMARY KEY constraints into AT_AddIndex subcommands.  This lets
 * us schedule creation of the index at the appropriate time during ALTER.
 *
 * Return value is the address of the new index.
 */
static ObjectAddress
ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	bool		check_rights;
	bool		skip_build;
	bool		quiet;
	ObjectAddress address;

	Assert(IsA(stmt, IndexStmt));
	Assert(!stmt->concurrent);

	/* The IndexStmt has already been through transformIndexStmt */
	Assert(stmt->transformed);

	/* suppress schema rights check when rebuilding existing index */
	check_rights = !is_rebuild;
	/* skip index build if phase 3 will do it or we're reusing an old one */
	skip_build = tab->rewrite > 0 || OidIsValid(stmt->oldNode);
	/* suppress notices when rebuilding existing index */
	quiet = is_rebuild;

	address = DefineIndex(RelationGetRelid(rel),
						  stmt,
						  InvalidOid,	/* no predefined OID */
						  true, /* is_alter_table */
						  check_rights,
						  skip_build,
						  quiet);

	/*
	 * If TryReuseIndex() stashed a relfilenode for us, we used it for the new
	 * index instead of building from scratch.  The DROP of the old edition of
	 * this index will have scheduled the storage for deletion at commit, so
	 * cancel that pending deletion.
	 */
	if (OidIsValid(stmt->oldNode))
	{
		Relation	irel = index_open(address.objectId, NoLock);

		RelationPreserveStorage(irel->rd_node, true);
		index_close(irel, NoLock);
	}

	return address;
}

/*
 * ALTER TABLE ADD CONSTRAINT USING INDEX
 *
 * Returns the address of the new constraint.
 */
static ObjectAddress
ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
						 IndexStmt *stmt, LOCKMODE lockmode)
{
	Oid			index_oid = stmt->indexOid;
	Relation	indexRel;
	char	   *indexName;
	IndexInfo  *indexInfo;
	char	   *constraintName;
	char		constraintType;
	ObjectAddress address;

	Assert(IsA(stmt, IndexStmt));
	Assert(OidIsValid(index_oid));
	Assert(stmt->isconstraint);

	indexRel = index_open(index_oid, AccessShareLock);

	indexName = pstrdup(RelationGetRelationName(indexRel));

	indexInfo = BuildIndexInfo(indexRel);

	/* this should have been checked at parse time */
	if (!indexInfo->ii_Unique)
		elog(ERROR, "index \"%s\" is not unique", indexName);

	/*
	 * Determine name to assign to constraint.  We require a constraint to
	 * have the same name as the underlying index; therefore, use the index's
	 * existing name as the default constraint name, and if the user
	 * explicitly gives some other name for the constraint, rename the index
	 * to match.
	 */
	constraintName = stmt->idxname;
	if (constraintName == NULL)
		constraintName = indexName;
	else if (strcmp(constraintName, indexName) != 0)
	{
		ereport(NOTICE,
				(errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX will rename index \"%s\" to \"%s\"",
						indexName, constraintName)));
		RenameRelationInternal(index_oid, constraintName, false);
	}

	/* Extra checks needed if making primary key */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, true);

	/* Note we currently don't support EXCLUSION constraints here */
	if (stmt->primary)
		constraintType = CONSTRAINT_PRIMARY;
	else
		constraintType = CONSTRAINT_UNIQUE;

	/* Create the catalog entries for the constraint */
	address = index_constraint_create(rel,
									  index_oid,
									  indexInfo,
									  constraintName,
									  constraintType,
									  stmt->deferrable,
									  stmt->initdeferred,
									  stmt->primary,
									  true,		/* update pg_index */
									  true,		/* remove old dependencies */
									  allowSystemTableMods,
									  false);	/* is_internal */

	index_close(indexRel, NoLock);

	return address;
}

/*
 * ALTER TABLE ADD CONSTRAINT
 *
 * Return value is the address of the new constraint; if no constraint was
 * added, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecAddConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					Constraint *newConstraint, bool recurse, bool is_readd,
					LOCKMODE lockmode)
{
	ObjectAddress address = InvalidObjectAddress;

	Assert(IsA(newConstraint, Constraint));

	/*
	 * Currently, we only expect to see CONSTR_CHECK and CONSTR_FOREIGN nodes
	 * arriving here (see the preprocessing done in parse_utilcmd.c).  Use a
	 * switch anyway to make it easier to add more code later.
	 */
	switch (newConstraint->contype)
	{
		case CONSTR_CHECK:
			address =
				ATAddCheckConstraint(wqueue, tab, rel,
									 newConstraint, recurse, false, is_readd,
									 lockmode);
			break;

		case CONSTR_FOREIGN:

			/*
			 * Note that we currently never recurse for FK constraints, so the
			 * "recurse" flag is silently ignored.
			 *
			 * Assign or validate constraint name
			 */
			if (newConstraint->conname)
			{
				if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
										 RelationGetRelid(rel),
										 RelationGetNamespace(rel),
										 newConstraint->conname))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("constraint \"%s\" for relation \"%s\" already exists",
									newConstraint->conname,
									RelationGetRelationName(rel))));
			}
			else
				newConstraint->conname =
					ChooseConstraintName(RelationGetRelationName(rel),
								   strVal(linitial(newConstraint->fk_attrs)),
										 "fkey",
										 RelationGetNamespace(rel),
										 NIL);

			address = ATAddForeignKeyConstraint(tab, rel, newConstraint,
												lockmode);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 (int) newConstraint->contype);
	}

	return address;
}

/*
 * Add a check constraint to a single table and its children.  Returns the
 * address of the constraint added to the parent relation, if one gets added,
 * or InvalidObjectAddress otherwise.
 *
 * Subroutine for ATExecAddConstraint.
 *
 * We must recurse to child tables during execution, rather than using
 * ALTER TABLE's normal prep-time recursion.  The reason is that all the
 * constraints *must* be given the same name, else they won't be seen as
 * related later.  If the user didn't explicitly specify a name, then
 * AddRelationNewConstraints would normally assign different names to the
 * child constraints.  To fix that, we must capture the name assigned at
 * the parent table and pass that down.
 */
static ObjectAddress
ATAddCheckConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					 Constraint *constr, bool recurse, bool recursing,
					 bool is_readd, LOCKMODE lockmode)
{
	List	   *newcons;
	ListCell   *lcon;
	List	   *children;
	ListCell   *child;
	ObjectAddress address = InvalidObjectAddress;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/*
	 * Call AddRelationNewConstraints to do the work, making sure it works on
	 * a copy of the Constraint so transformExpr can't modify the original. It
	 * returns a list of cooked constraints.
	 *
	 * If the constraint ends up getting merged with a pre-existing one, it's
	 * omitted from the returned list, which is what we want: we do not need
	 * to do any validation work.  That can only happen at child tables,
	 * though, since we disallow merging at the top level.
	 */
	newcons = AddRelationNewConstraints(rel, NIL,
										list_make1(copyObject(constr)),
										recursing | is_readd,	/* allow_merge */
										!recursing,		/* is_local */
										is_readd);		/* is_internal */

	/* we don't expect more than one constraint here */
	Assert(list_length(newcons) <= 1);

	/* Add each to-be-validated constraint to Phase 3's queue */
	foreach(lcon, newcons)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lcon);

		if (!ccon->skip_validation)
		{
			NewConstraint *newcon;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = ccon->name;
			newcon->contype = ccon->contype;
			/* ExecQual wants implicit-AND format */
			newcon->qual = (Node *) make_ands_implicit((Expr *) ccon->expr);

			tab->constraints = lappend(tab->constraints, newcon);
		}

		/* Save the actually assigned name if it was defaulted */
		if (constr->conname == NULL)
			constr->conname = ccon->name;

		ObjectAddressSet(address, ConstraintRelationId, ccon->conoid);
	}

	/* At this point we must have a locked-down name to use */
	Assert(constr->conname != NULL);

	/* Advance command counter in case same table is visited multiple times */
	CommandCounterIncrement();

	/*
	 * If the constraint got merged with an existing constraint, we're done.
	 * We mustn't recurse to child tables in this case, because they've
	 * already got the constraint, and visiting them again would lead to an
	 * incorrect value for coninhcount.
	 */
	if (newcons == NIL)
		return address;

	/*
	 * If adding a NO INHERIT constraint, no need to find our children.
	 */
	if (constr->is_no_inherit)
		return address;

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	children = find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * Check if ONLY was specified with ALTER TABLE.  If so, allow the
	 * contraint creation only if there are no children currently.  Error out
	 * otherwise.
	 */
	if (!recurse && children != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("constraint must be added to child tables too")));

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children already got lock */
		childrel = heap_open(childrelid, NoLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");

		/* Find or create work queue entry for this table */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to child */
		ATAddCheckConstraint(wqueue, childtab, childrel,
							 constr, recurse, true, is_readd, lockmode);

		heap_close(childrel, NoLock);
	}

	return address;
}

/*
 * Add a foreign-key constraint to a single table; return the new constraint's
 * address.
 *
 * Subroutine for ATExecAddConstraint.  Must already hold exclusive
 * lock on the rel, and have done appropriate validity checks for it.
 * We do permissions checks here, however.
 */
static ObjectAddress
ATAddForeignKeyConstraint(AlteredTableInfo *tab, Relation rel,
						  Constraint *fkconstraint, LOCKMODE lockmode)
{
	Relation	pkrel;
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
	bool		old_check_ok;
	ObjectAddress address;
	ListCell   *old_pfeqop_item = list_head(fkconstraint->old_conpfeqop);

	/*
	 * Grab ShareRowExclusiveLock on the pk table, so that someone doesn't
	 * delete rows out from under us.
	 */
	if (OidIsValid(fkconstraint->old_pktable_oid))
		pkrel = heap_open(fkconstraint->old_pktable_oid, ShareRowExclusiveLock);
	else
		pkrel = heap_openrv(fkconstraint->pktable, ShareRowExclusiveLock);

	/*
	 * Validity checks (permission checks wait till we have the column
	 * numbers)
	 */
	if (pkrel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	if (!allowSystemTableMods && IsSystemRelation(pkrel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(pkrel))));

	/*
	 * References from permanent or unlogged tables to temp tables, and from
	 * permanent tables to unlogged tables, are disallowed because the
	 * referenced data can vanish out from under us.  References from temp
	 * tables to any other table type are also disallowed, because other
	 * backends might need to run the RI triggers on the perm table, but they
	 * can't reliably see tuples in the local buffers of other backends.
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_PERMANENT:
			if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on permanent tables may reference only permanent tables")));
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT
				&& pkrel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on unlogged tables may reference only permanent or unlogged tables")));
			break;
		case RELPERSISTENCE_TEMP:
			if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables may reference only temporary tables")));
			if (!pkrel->rd_islocaltemp || !rel->rd_islocaltemp)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables must involve temporary tables of this session")));
			break;
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
	 * Now we can check permissions.
	 */
	checkFkeyPermissions(pkrel, pkattnum, numpks);
	checkFkeyPermissions(rel, fkattnum, numfks);

	/*
	 * Look up the equality operators to use in the constraint.
	 *
	 * Note that we have to be careful about the difference between the actual
	 * PK column type and the opclass' declared input type, which might be
	 * only binary-compatible with it.  The declared opcintype is the right
	 * thing to probe pg_amop with.
	 */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	/*
	 * On the strength of a previous constraint, we might avoid scanning
	 * tables to validate this one.  See below.
	 */
	old_check_ok = (fkconstraint->old_conpfeqop != NIL);
	Assert(!old_check_ok || numfks == list_length(fkconstraint->old_conpfeqop));

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
		Oid			pfeqop_right;

		/* We need several fields out of the pg_opclass entry */
		cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclasses[i]));
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
		{
			pfeqop_right = fktyped;
			ffeqop = get_opfamily_member(opfamily, fktyped, fktyped,
										 eqstrategy);
		}
		else
		{
			/* keep compiler quiet */
			pfeqop_right = InvalidOid;
			ffeqop = InvalidOid;
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
		{
			/*
			 * Otherwise, look for an implicit cast from the FK type to the
			 * opcintype, and if found, use the primary equality operator.
			 * This is a bit tricky because opcintype might be a polymorphic
			 * type such as ANYARRAY or ANYENUM; so what we have to test is
			 * whether the two actual column types can be concurrently cast to
			 * that type.  (Otherwise, we'd fail to reject combinations such
			 * as int[] and point[].)
			 */
			Oid			input_typeids[2];
			Oid			target_typeids[2];

			input_typeids[0] = pktype;
			input_typeids[1] = fktype;
			target_typeids[0] = opcintype;
			target_typeids[1] = opcintype;
			if (can_coerce_type(2, input_typeids, target_typeids,
								COERCION_IMPLICIT))
			{
				pfeqop = ffeqop = ppeqop;
				pfeqop_right = opcintype;
			}
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("foreign key constraint \"%s\" "
							"cannot be implemented",
							fkconstraint->conname),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of incompatible types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktype),
							   format_type_be(pktype))));

		if (old_check_ok)
		{
			/*
			 * When a pfeqop changes, revalidate the constraint.  We could
			 * permit intra-opfamily changes, but that adds subtle complexity
			 * without any concrete benefit for core types.  We need not
			 * assess ppeqop or ffeqop, which RI_Initial_Check() does not use.
			 */
			old_check_ok = (pfeqop == lfirst_oid(old_pfeqop_item));
			old_pfeqop_item = lnext(old_pfeqop_item);
		}
		if (old_check_ok)
		{
			Oid			old_fktype;
			Oid			new_fktype;
			CoercionPathType old_pathtype;
			CoercionPathType new_pathtype;
			Oid			old_castfunc;
			Oid			new_castfunc;

			/*
			 * Identify coercion pathways from each of the old and new FK-side
			 * column types to the right (foreign) operand type of the pfeqop.
			 * We may assume that pg_constraint.conkey is not changing.
			 */
			old_fktype = tab->oldDesc->attrs[fkattnum[i] - 1]->atttypid;
			new_fktype = fktype;
			old_pathtype = findFkeyCast(pfeqop_right, old_fktype,
										&old_castfunc);
			new_pathtype = findFkeyCast(pfeqop_right, new_fktype,
										&new_castfunc);

			/*
			 * Upon a change to the cast from the FK column to its pfeqop
			 * operand, revalidate the constraint.  For this evaluation, a
			 * binary coercion cast is equivalent to no cast at all.  While
			 * type implementors should design implicit casts with an eye
			 * toward consistency of operations like equality, we cannot
			 * assume here that they have done so.
			 *
			 * A function with a polymorphic argument could change behavior
			 * arbitrarily in response to get_fn_expr_argtype().  Therefore,
			 * when the cast destination is polymorphic, we only avoid
			 * revalidation if the input type has not changed at all.  Given
			 * just the core data types and operator classes, this requirement
			 * prevents no would-be optimizations.
			 *
			 * If the cast converts from a base type to a domain thereon, then
			 * that domain type must be the opcintype of the unique index.
			 * Necessarily, the primary key column must then be of the domain
			 * type.  Since the constraint was previously valid, all values on
			 * the foreign side necessarily exist on the primary side and in
			 * turn conform to the domain.  Consequently, we need not treat
			 * domains specially here.
			 *
			 * Since we require that all collations share the same notion of
			 * equality (which they do, because texteq reduces to bitwise
			 * equality), we don't compare collation here.
			 *
			 * We need not directly consider the PK type.  It's necessarily
			 * binary coercible to the opcintype of the unique index column,
			 * and ri_triggers.c will only deal with PK datums in terms of
			 * that opcintype.  Changing the opcintype also changes pfeqop.
			 */
			old_check_ok = (new_pathtype == old_pathtype &&
							new_castfunc == old_castfunc &&
							(!IsPolymorphicType(pfeqop_right) ||
							 new_fktype == old_fktype));

		}

		pfeqoperators[i] = pfeqop;
		ppeqoperators[i] = ppeqop;
		ffeqoperators[i] = ffeqop;
	}

	/*
	 * Record the FK constraint in pg_constraint.
	 */
	constrOid = CreateConstraintEntry(fkconstraint->conname,
									  RelationGetNamespace(rel),
									  CONSTRAINT_FOREIGN,
									  fkconstraint->deferrable,
									  fkconstraint->initdeferred,
									  fkconstraint->initially_valid,
									  RelationGetRelid(rel),
									  fkattnum,
									  numfks,
									  InvalidOid,		/* not a domain
														 * constraint */
									  indexOid,
									  RelationGetRelid(pkrel),
									  pkattnum,
									  pfeqoperators,
									  ppeqoperators,
									  ffeqoperators,
									  numpks,
									  fkconstraint->fk_upd_action,
									  fkconstraint->fk_del_action,
									  fkconstraint->fk_matchtype,
									  NULL,		/* no exclusion constraint */
									  NULL,		/* no check constraint */
									  NULL,
									  NULL,
									  true,		/* islocal */
									  0,		/* inhcount */
									  true,		/* isnoinherit */
									  false);	/* is_internal */
	ObjectAddressSet(address, ConstraintRelationId, constrOid);

	/*
	 * Create the triggers that will enforce the constraint.
	 */
	createForeignKeyTriggers(rel, RelationGetRelid(pkrel), fkconstraint,
							 constrOid, indexOid);

	/*
	 * Tell Phase 3 to check that the constraint is satisfied by existing
	 * rows. We can skip this during table creation, when requested explicitly
	 * by specifying NOT VALID in an ADD FOREIGN KEY command, and when we're
	 * recreating a constraint following a SET DATA TYPE operation that did
	 * not impugn its validity.
	 */
	if (!old_check_ok && !fkconstraint->skip_validation)
	{
		NewConstraint *newcon;

		newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
		newcon->name = fkconstraint->conname;
		newcon->contype = CONSTR_FOREIGN;
		newcon->refrelid = RelationGetRelid(pkrel);
		newcon->refindid = indexOid;
		newcon->conid = constrOid;
		newcon->qual = (Node *) fkconstraint;

		tab->constraints = lappend(tab->constraints, newcon);
	}

	/*
	 * Close pk table, but keep lock until we've committed.
	 */
	heap_close(pkrel, NoLock);

	return address;
}

/*
 * ALTER TABLE ALTER CONSTRAINT
 *
 * Update the attributes of a constraint.
 *
 * Currently only works for Foreign Key constraints.
 * Foreign keys do not inherit, so we purposely ignore the
 * recursion bit here, but we keep the API the same for when
 * other constraint types are supported.
 *
 * If the constraint is modified, returns its address; otherwise, return
 * InvalidObjectAddress.
 */
static ObjectAddress
ATExecAlterConstraint(Relation rel, AlterTableCmd *cmd,
					  bool recurse, bool recursing, LOCKMODE lockmode)
{
	Constraint *cmdcon;
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	contuple;
	Form_pg_constraint currcon = NULL;
	bool		found = false;
	ObjectAddress address;

	Assert(IsA(cmd->def, Constraint));
	cmdcon = (Constraint *) cmd->def;

	conrel = heap_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and check the target constraint
	 */
	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(conrel, ConstraintRelidIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(contuple = systable_getnext(scan)))
	{
		currcon = (Form_pg_constraint) GETSTRUCT(contuple);
		if (strcmp(NameStr(currcon->conname), cmdcon->conname) == 0)
		{
			found = true;
			break;
		}
	}

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						cmdcon->conname, RelationGetRelationName(rel))));

	if (currcon->contype != CONSTRAINT_FOREIGN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key constraint",
						cmdcon->conname, RelationGetRelationName(rel))));

	if (currcon->condeferrable != cmdcon->deferrable ||
		currcon->condeferred != cmdcon->initdeferred)
	{
		HeapTuple	copyTuple;
		HeapTuple	tgtuple;
		Form_pg_constraint copy_con;
		List	   *otherrelids = NIL;
		ScanKeyData tgkey;
		SysScanDesc tgscan;
		Relation	tgrel;
		ListCell   *lc;

		/*
		 * Now update the catalog, while we have the door open.
		 */
		copyTuple = heap_copytuple(contuple);
		copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
		copy_con->condeferrable = cmdcon->deferrable;
		copy_con->condeferred = cmdcon->initdeferred;
		simple_heap_update(conrel, &copyTuple->t_self, copyTuple);
		CatalogUpdateIndexes(conrel, copyTuple);

		InvokeObjectPostAlterHook(ConstraintRelationId,
								  HeapTupleGetOid(contuple), 0);

		heap_freetuple(copyTuple);

		/*
		 * Now we need to update the multiple entries in pg_trigger that
		 * implement the constraint.
		 */
		tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

		ScanKeyInit(&tgkey,
					Anum_pg_trigger_tgconstraint,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(HeapTupleGetOid(contuple)));

		tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
									NULL, 1, &tgkey);

		while (HeapTupleIsValid(tgtuple = systable_getnext(tgscan)))
		{
			Form_pg_trigger copy_tg;

			copyTuple = heap_copytuple(tgtuple);
			copy_tg = (Form_pg_trigger) GETSTRUCT(copyTuple);

			/* Remember OIDs of other relation(s) involved in FK constraint */
			if (copy_tg->tgrelid != RelationGetRelid(rel))
				otherrelids = list_append_unique_oid(otherrelids,
													 copy_tg->tgrelid);

			copy_tg->tgdeferrable = cmdcon->deferrable;
			copy_tg->tginitdeferred = cmdcon->initdeferred;
			simple_heap_update(tgrel, &copyTuple->t_self, copyTuple);
			CatalogUpdateIndexes(tgrel, copyTuple);

			InvokeObjectPostAlterHook(TriggerRelationId,
									  HeapTupleGetOid(tgtuple), 0);

			heap_freetuple(copyTuple);
		}

		systable_endscan(tgscan);

		heap_close(tgrel, RowExclusiveLock);

		/*
		 * Invalidate relcache so that others see the new attributes.  We must
		 * inval both the named rel and any others having relevant triggers.
		 * (At present there should always be exactly one other rel, but
		 * there's no need to hard-wire such an assumption here.)
		 */
		CacheInvalidateRelcache(rel);
		foreach(lc, otherrelids)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(lc));
		}

		ObjectAddressSet(address, ConstraintRelationId,
						 HeapTupleGetOid(contuple));
	}
	else
		address = InvalidObjectAddress;

	systable_endscan(scan);

	heap_close(conrel, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE VALIDATE CONSTRAINT
 *
 * XXX The reason we handle recursion here rather than at Phase 1 is because
 * there's no good way to skip recursing when handling foreign keys: there is
 * no need to lock children in that case, yet we wouldn't be able to avoid
 * doing so at that level.
 *
 * Return value is the address of the validated constraint.  If the constraint
 * was already validated, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecValidateConstraint(Relation rel, char *constrName, bool recurse,
						 bool recursing, LOCKMODE lockmode)
{
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	Form_pg_constraint con = NULL;
	bool		found = false;
	ObjectAddress address;

	conrel = heap_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and check the target constraint
	 */
	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(conrel, ConstraintRelidIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		con = (Form_pg_constraint) GETSTRUCT(tuple);
		if (strcmp(NameStr(con->conname), constrName) == 0)
		{
			found = true;
			break;
		}
	}

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						constrName, RelationGetRelationName(rel))));

	if (con->contype != CONSTRAINT_FOREIGN &&
		con->contype != CONSTRAINT_CHECK)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key or check constraint",
						constrName, RelationGetRelationName(rel))));

	if (!con->convalidated)
	{
		HeapTuple	copyTuple;
		Form_pg_constraint copy_con;

		if (con->contype == CONSTRAINT_FOREIGN)
		{
			Relation	refrel;

			/*
			 * Triggers are already in place on both tables, so a concurrent
			 * write that alters the result here is not possible. Normally we
			 * can run a query here to do the validation, which would only
			 * require AccessShareLock. In some cases, it is possible that we
			 * might need to fire triggers to perform the check, so we take a
			 * lock at RowShareLock level just in case.
			 */
			refrel = heap_open(con->confrelid, RowShareLock);

			validateForeignKeyConstraint(constrName, rel, refrel,
										 con->conindid,
										 HeapTupleGetOid(tuple));
			heap_close(refrel, NoLock);

			/*
			 * Foreign keys do not inherit, so we purposely ignore the
			 * recursion bit here
			 */
		}
		else if (con->contype == CONSTRAINT_CHECK)
		{
			List	   *children = NIL;
			ListCell   *child;

			/*
			 * If we're recursing, the parent has already done this, so skip
			 * it.
			 */
			if (!recursing)
				children = find_all_inheritors(RelationGetRelid(rel),
											   lockmode, NULL);

			/*
			 * For CHECK constraints, we must ensure that we only mark the
			 * constraint as validated on the parent if it's already validated
			 * on the children.
			 *
			 * We recurse before validating on the parent, to reduce risk of
			 * deadlocks.
			 */
			foreach(child, children)
			{
				Oid			childoid = lfirst_oid(child);
				Relation	childrel;

				if (childoid == RelationGetRelid(rel))
					continue;

				/*
				 * If we are told not to recurse, there had better not be any
				 * child tables; else the addition would put them out of step.
				 */
				if (!recurse)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("constraint must be validated on child tables too")));

				/* find_all_inheritors already got lock */
				childrel = heap_open(childoid, NoLock);

				ATExecValidateConstraint(childrel, constrName, false,
										 true, lockmode);
				heap_close(childrel, NoLock);
			}

			validateCheckConstraint(rel, tuple);

			/*
			 * Invalidate relcache so that others see the new validated
			 * constraint.
			 */
			CacheInvalidateRelcache(rel);
		}

		/*
		 * Now update the catalog, while we have the door open.
		 */
		copyTuple = heap_copytuple(tuple);
		copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
		copy_con->convalidated = true;
		simple_heap_update(conrel, &copyTuple->t_self, copyTuple);
		CatalogUpdateIndexes(conrel, copyTuple);

		InvokeObjectPostAlterHook(ConstraintRelationId,
								  HeapTupleGetOid(tuple), 0);

		heap_freetuple(copyTuple);

		ObjectAddressSet(address, ConstraintRelationId,
						 HeapTupleGetOid(tuple));
	}
	else
		address = InvalidObjectAddress; /* already validated */

	systable_endscan(scan);

	heap_close(conrel, RowExclusiveLock);

	return address;
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
 *	for the pkrel.  Also return the index OID and index opclasses of the
 *	index supporting the primary key.
 *
 *	All parameters except pkrel are output parameters.  Also, the function
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
	 * (hopefully there isn't more than one such).  Insist it's valid, too.
	 */
	*indexOid = InvalidOid;

	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);
		if (indexStruct->indisprimary && IndexIsValid(indexStruct))
		{
			/*
			 * Refuse to use a deferrable primary key.  This is per SQL spec,
			 * and there would be a lot of interesting semantic problems if we
			 * tried to allow it.
			 */
			if (!indexStruct->indimmediate)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot use a deferrable primary key for referenced table \"%s\"",
								RelationGetRelationName(pkrel))));

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
	bool		found_deferrable = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i,
				j;

	/*
	 * Reject duplicate appearances of columns in the referenced-columns list.
	 * Such a case is forbidden by the SQL standard, and even if we thought it
	 * useful to allow it, there would be ambiguity about how to match the
	 * list to unique indexes (in particular, it'd be unclear which index
	 * opclass goes with which FK column).
	 */
	for (i = 0; i < numattrs; i++)
	{
		for (j = i + 1; j < numattrs; j++)
		{
			if (attnums[i] == attnums[j])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FOREIGN_KEY),
						 errmsg("foreign key referenced-columns list must not contain duplicates")));
		}
	}

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

		indexoid = lfirst_oid(indexoidscan);
		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Must have the right number of columns; must be unique and not a
		 * partial index; forget it if there are any expressions, too. Invalid
		 * indexes are out as well.
		 */
		if (indexStruct->indnatts == numattrs &&
			indexStruct->indisunique &&
			IndexIsValid(indexStruct) &&
			heap_attisnull(indexTuple, Anum_pg_index_indpred) &&
			heap_attisnull(indexTuple, Anum_pg_index_indexprs))
		{
			Datum		indclassDatum;
			bool		isnull;
			oidvector  *indclass;

			/* Must get indclass the hard way */
			indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
											Anum_pg_index_indclass, &isnull);
			Assert(!isnull);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/*
			 * The given attnum list may match the index columns in any order.
			 * Check for a match, and extract the appropriate opclasses while
			 * we're at it.
			 *
			 * We know that attnums[] is duplicate-free per the test at the
			 * start of this function, and we checked above that the number of
			 * index columns agrees, so if we find a match for each attnums[]
			 * entry then we must have a one-to-one match in some order.
			 */
			for (i = 0; i < numattrs; i++)
			{
				found = false;
				for (j = 0; j < numattrs; j++)
				{
					if (attnums[i] == indexStruct->indkey.values[j])
					{
						opclasses[i] = indclass->values[j];
						found = true;
						break;
					}
				}
				if (!found)
					break;
			}

			/*
			 * Refuse to use a deferrable unique/primary key.  This is per SQL
			 * spec, and there would be a lot of interesting semantic problems
			 * if we tried to allow it.
			 */
			if (found && !indexStruct->indimmediate)
			{
				/*
				 * Remember that we found an otherwise matching index, so that
				 * we can generate a more appropriate error message.
				 */
				found_deferrable = true;
				found = false;
			}
		}
		ReleaseSysCache(indexTuple);
		if (found)
			break;
	}

	if (!found)
	{
		if (found_deferrable)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot use a deferrable unique constraint for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("there is no unique constraint matching given keys for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
	}

	list_free(indexoidlist);

	return indexoid;
}

/*
 * findFkeyCast -
 *
 *	Wrapper around find_coercion_pathway() for ATAddForeignKeyConstraint().
 *	Caller has equal regard for binary coercibility and for an exact match.
*/
static CoercionPathType
findFkeyCast(Oid targetTypeId, Oid sourceTypeId, Oid *funcid)
{
	CoercionPathType ret;

	if (targetTypeId == sourceTypeId)
	{
		ret = COERCION_PATH_RELABELTYPE;
		*funcid = InvalidOid;
	}
	else
	{
		ret = find_coercion_pathway(targetTypeId, sourceTypeId,
									COERCION_IMPLICIT, funcid);
		if (ret == COERCION_PATH_NONE)
			/* A previously-relied-upon cast is now gone. */
			elog(ERROR, "could not find cast from %u to %u",
				 sourceTypeId, targetTypeId);
	}

	return ret;
}

/* Permissions checks for ADD FOREIGN KEY */
static void
checkFkeyPermissions(Relation rel, int16 *attnums, int natts)
{
	Oid			roleid = GetUserId();
	AclResult	aclresult;
	int			i;

	/* Okay if we have relation-level REFERENCES permission */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), roleid,
								  ACL_REFERENCES);
	if (aclresult == ACLCHECK_OK)
		return;
	/* Else we must have REFERENCES on each column */
	for (i = 0; i < natts; i++)
	{
		aclresult = pg_attribute_aclcheck(RelationGetRelid(rel), attnums[i],
										  roleid, ACL_REFERENCES);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));
	}
}

/*
 * Scan the existing rows in a table to verify they meet a proposed
 * CHECK constraint.
 *
 * The caller must have opened and locked the relation appropriately.
 */
static void
validateCheckConstraint(Relation rel, HeapTuple constrtup)
{
	EState	   *estate;
	Datum		val;
	char	   *conbin;
	Expr	   *origexpr;
	List	   *exprstate;
	TupleDesc	tupdesc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	ExprContext *econtext;
	MemoryContext oldcxt;
	TupleTableSlot *slot;
	Form_pg_constraint constrForm;
	bool		isnull;
	Snapshot	snapshot;

	/* VALIDATE CONSTRAINT is a no-op for foreign tables */
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		return;

	constrForm = (Form_pg_constraint) GETSTRUCT(constrtup);

	estate = CreateExecutorState();

	/*
	 * XXX this tuple doesn't really come from a syscache, but this doesn't
	 * matter to SysCacheGetAttr, because it only wants to be able to fetch
	 * the tupdesc
	 */
	val = SysCacheGetAttr(CONSTROID, constrtup, Anum_pg_constraint_conbin,
						  &isnull);
	if (isnull)
		elog(ERROR, "null conbin for constraint %u",
			 HeapTupleGetOid(constrtup));
	conbin = TextDatumGetCString(val);
	origexpr = (Expr *) stringToNode(conbin);
	exprstate = (List *)
		ExecPrepareExpr((Expr *) make_ands_implicit(origexpr), estate);

	econtext = GetPerTupleExprContext(estate);
	tupdesc = RelationGetDescr(rel);
	slot = MakeSingleTupleTableSlot(tupdesc);
	econtext->ecxt_scantuple = slot;

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = heap_beginscan(rel, snapshot, 0, NULL);

	/*
	 * Switch to per-tuple memory context and reset it for each tuple
	 * produced, so we don't leak memory.
	 */
	oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		ExecStoreTuple(tuple, slot, InvalidBuffer, false);

		if (!ExecQual(exprstate, econtext, true))
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("check constraint \"%s\" is violated by some row",
							NameStr(constrForm->conname)),
					 errtableconstraint(rel, NameStr(constrForm->conname))));

		ResetExprContext(econtext);
	}

	MemoryContextSwitchTo(oldcxt);
	heap_endscan(scan);
	UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
}

/*
 * Scan the existing rows in a table to verify they meet a proposed FK
 * constraint.
 *
 * Caller must have opened and locked both relations appropriately.
 */
static void
validateForeignKeyConstraint(char *conname,
							 Relation rel,
							 Relation pkrel,
							 Oid pkindOid,
							 Oid constraintOid)
{
	HeapScanDesc scan;
	HeapTuple	tuple;
	Trigger		trig;
	Snapshot	snapshot;

	ereport(DEBUG1,
			(errmsg("validating foreign key constraint \"%s\"", conname)));

	/*
	 * Build a trigger call structure; we'll need it either way.
	 */
	MemSet(&trig, 0, sizeof(trig));
	trig.tgoid = InvalidOid;
	trig.tgname = conname;
	trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
	trig.tgisinternal = TRUE;
	trig.tgconstrrelid = RelationGetRelid(pkrel);
	trig.tgconstrindid = pkindOid;
	trig.tgconstraint = constraintOid;
	trig.tgdeferrable = FALSE;
	trig.tginitdeferred = FALSE;
	/* we needn't fill in tgargs or tgqual */

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
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = heap_beginscan(rel, snapshot, 0, NULL);

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
	UnregisterSnapshot(snapshot);
}

static void
CreateFKCheckTrigger(Oid myRelOid, Oid refRelOid, Constraint *fkconstraint,
					 Oid constraintOid, Oid indexOid, bool on_insert)
{
	CreateTrigStmt *fk_trigger;

	/*
	 * Note: for a self-referential FK (referencing and referenced tables are
	 * the same), it is important that the ON UPDATE action fires before the
	 * CHECK action, since both triggers will fire on the same row during an
	 * UPDATE event; otherwise the CHECK trigger will be checking a non-final
	 * state of the row.  Triggers fire in name order, so we ensure this by
	 * using names like "RI_ConstraintTrigger_a_NNNN" for the action triggers
	 * and "RI_ConstraintTrigger_c_NNNN" for the check triggers.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = "RI_ConstraintTrigger_c";
	fk_trigger->relation = NULL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;

	/* Either ON INSERT or ON UPDATE */
	if (on_insert)
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_ins");
		fk_trigger->events = TRIGGER_TYPE_INSERT;
	}
	else
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_upd");
		fk_trigger->events = TRIGGER_TYPE_UPDATE;
	}

	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->isconstraint = true;
	fk_trigger->deferrable = fkconstraint->deferrable;
	fk_trigger->initdeferred = fkconstraint->initdeferred;
	fk_trigger->constrrel = NULL;
	fk_trigger->args = NIL;

	(void) CreateTrigger(fk_trigger, NULL, myRelOid, refRelOid, constraintOid,
						 indexOid, true);

	/* Make changes-so-far visible */
	CommandCounterIncrement();
}

/*
 * Create the triggers that implement an FK constraint.
 */
static void
createForeignKeyTriggers(Relation rel, Oid refRelOid, Constraint *fkconstraint,
						 Oid constraintOid, Oid indexOid)
{
	Oid			myRelOid;
	CreateTrigStmt *fk_trigger;

	myRelOid = RelationGetRelid(rel);

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * DELETE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_DELETE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->isconstraint = true;
	fk_trigger->constrrel = NULL;
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

	(void) CreateTrigger(fk_trigger, NULL, refRelOid, myRelOid, constraintOid,
						 indexOid, true);

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * UPDATE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_UPDATE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->isconstraint = true;
	fk_trigger->constrrel = NULL;
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

	(void) CreateTrigger(fk_trigger, NULL, refRelOid, myRelOid, constraintOid,
						 indexOid, true);

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute CREATE CONSTRAINT TRIGGER statements for the CHECK
	 * action for both INSERTs and UPDATEs on the referencing table.
	 */
	CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint, constraintOid,
						 indexOid, true);
	CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint, constraintOid,
						 indexOid, false);
}

/*
 * ALTER TABLE DROP CONSTRAINT
 *
 * Like DROP COLUMN, we can't use the normal ALTER TABLE recursion mechanism.
 */
static void
ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior,
					 bool recurse, bool recursing,
					 bool missing_ok, LOCKMODE lockmode)
{
	List	   *children;
	ListCell   *child;
	Relation	conrel;
	Form_pg_constraint con;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	bool		found = false;
	bool		is_no_inherit_constraint = false;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	conrel = heap_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and drop the target constraint
	 */
	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(conrel, ConstraintRelidIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress conobj;

		con = (Form_pg_constraint) GETSTRUCT(tuple);

		if (strcmp(NameStr(con->conname), constrName) != 0)
			continue;

		/* Don't drop inherited constraints */
		if (con->coninhcount > 0 && !recursing)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop inherited constraint \"%s\" of relation \"%s\"",
							constrName, RelationGetRelationName(rel))));

		is_no_inherit_constraint = con->connoinherit;

		/*
		 * Perform the actual constraint deletion
		 */
		conobj.classId = ConstraintRelationId;
		conobj.objectId = HeapTupleGetOid(tuple);
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);

		found = true;

		/* constraint found and dropped -- no need to keep looping */
		break;
	}

	systable_endscan(scan);

	if (!found)
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("constraint \"%s\" of relation \"%s\" does not exist",
					   constrName, RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("constraint \"%s\" of relation \"%s\" does not exist, skipping",
							constrName, RelationGetRelationName(rel))));
			heap_close(conrel, RowExclusiveLock);
			return;
		}
	}

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	if (!is_no_inherit_constraint)
		children = find_inheritance_children(RelationGetRelid(rel), lockmode);
	else
		children = NIL;

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		HeapTuple	copy_tuple;

		/* find_inheritance_children already got lock */
		childrel = heap_open(childrelid, NoLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");

		ScanKeyInit(&key,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(childrelid));
		scan = systable_beginscan(conrel, ConstraintRelidIndexId,
								  true, NULL, 1, &key);

		/* scan for matching tuple - there should only be one */
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			con = (Form_pg_constraint) GETSTRUCT(tuple);

			/* Right now only CHECK constraints can be inherited */
			if (con->contype != CONSTRAINT_CHECK)
				continue;

			if (strcmp(NameStr(con->conname), constrName) == 0)
				break;
		}

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("constraint \"%s\" of relation \"%s\" does not exist",
					   constrName,
					   RelationGetRelationName(childrel))));

		copy_tuple = heap_copytuple(tuple);

		systable_endscan(scan);

		con = (Form_pg_constraint) GETSTRUCT(copy_tuple);

		if (con->coninhcount <= 0)		/* shouldn't happen */
			elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
				 childrelid, constrName);

		if (recurse)
		{
			/*
			 * If the child constraint has other definition sources, just
			 * decrement its inheritance count; if not, recurse to delete it.
			 */
			if (con->coninhcount == 1 && !con->conislocal)
			{
				/* Time to delete this child constraint, too */
				ATExecDropConstraint(childrel, constrName, behavior,
									 true, true,
									 false, lockmode);
			}
			else
			{
				/* Child constraint must survive my deletion */
				con->coninhcount--;
				simple_heap_update(conrel, &copy_tuple->t_self, copy_tuple);
				CatalogUpdateIndexes(conrel, copy_tuple);

				/* Make update visible */
				CommandCounterIncrement();
			}
		}
		else
		{
			/*
			 * If we were told to drop ONLY in this table (no recursion), we
			 * need to mark the inheritors' constraints as locally defined
			 * rather than inherited.
			 */
			con->coninhcount--;
			con->conislocal = true;

			simple_heap_update(conrel, &copy_tuple->t_self, copy_tuple);
			CatalogUpdateIndexes(conrel, copy_tuple);

			/* Make update visible */
			CommandCounterIncrement();
		}

		heap_freetuple(copy_tuple);

		heap_close(childrel, NoLock);
	}

	heap_close(conrel, RowExclusiveLock);
}

/*
 * ALTER COLUMN TYPE
 */
static void
ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd, LOCKMODE lockmode)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	Node	   *transform = def->cooked_default;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	NewColumnValue *newval;
	ParseState *pstate = make_parsestate(NULL);
	AclResult	aclresult;

	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter column type of typed table")));

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
	typenameTypeIdAndMod(NULL, typeName, &targettype, &targettypmod);

	aclresult = pg_type_aclcheck(targettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettype);

	/* And the collation */
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colName, targettype, targetcollid,
					   list_make1_oid(rel->rd_rel->reltype),
					   false);

	if (tab->relkind == RELKIND_RELATION)
	{
		/*
		 * Set up an expression to transform the old data value to the new
		 * type. If a USING option was given, use the expression as
		 * transformed by transformAlterTableStmt, else just take the old
		 * value and try to coerce it.  We do this first so that type
		 * incompatibility can be detected before we waste effort, and because
		 * we need the expression to be parsed against the original table row
		 * type.
		 */
		if (!transform)
		{
			transform = (Node *) makeVar(1, attnum,
										 attTup->atttypid, attTup->atttypmod,
										 attTup->attcollation,
										 0);
		}

		transform = coerce_to_target_type(pstate,
										  transform, exprType(transform),
										  targettype, targettypmod,
										  COERCION_ASSIGNMENT,
										  COERCE_IMPLICIT_CAST,
										  -1);
		if (transform == NULL)
		{
			/* error text depends on whether USING was specified or not */
			if (def->cooked_default != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("result of USING clause for column \"%s\""
								" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
						 errhint("You might need to add an explicit cast.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
				/* translator: USING is SQL, don't translate it */
					   errhint("You might need to specify \"USING %s::%s\".",
							   quote_identifier(colName),
							   format_type_with_typemod(targettype,
														targettypmod))));
		}

		/* Fix collations after all else */
		assign_expr_collations(pstate, transform);

		/* Plan the expr now so we can accurately assess the need to rewrite. */
		transform = (Node *) expression_planner((Expr *) transform);

		/*
		 * Add a work queue item to make ATRewriteTable update the column
		 * contents.
		 */
		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attnum;
		newval->expr = (Expr *) transform;

		tab->newvals = lappend(tab->newvals, newval);
		if (ATColumnChangeRequiresRewrite(transform, attnum))
			tab->rewrite |= AT_REWRITE_COLUMN_REWRITE;
	}
	else if (transform)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (tab->relkind == RELKIND_COMPOSITE_TYPE ||
		tab->relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * For composite types, do this check now.  Tables will check it later
		 * when the table is being rewritten.
		 */
		find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
	}

	ReleaseSysCache(tuple);

	/*
	 * The recursion case is handled by ATSimpleRecursion.  However, if we are
	 * told not to recurse, there had better not be any child tables; else the
	 * alter would put them out of step.
	 */
	if (recurse)
		ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
	else if (!recursing &&
			 find_inheritance_children(RelationGetRelid(rel), NoLock) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("type of inherited column \"%s\" must be changed in child tables too",
						colName)));

	if (tab->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode);
}

/*
 * When the data type of a column is changed, a rewrite might not be required
 * if the new type is sufficiently identical to the old one, and the USING
 * clause isn't trying to insert some other value.  It's safe to skip the
 * rewrite if the old type is binary coercible to the new type, or if the
 * new type is an unconstrained domain over the old type.  In the case of a
 * constrained domain, we could get by with scanning the table and checking
 * the constraint rather than actually rewriting it, but we don't currently
 * try to do that.
 */
static bool
ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno)
{
	Assert(expr != NULL);

	for (;;)
	{
		/* only one varno, so no need to check that */
		if (IsA(expr, Var) &&((Var *) expr)->varattno == varattno)
			return false;
		else if (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;
		else if (IsA(expr, CoerceToDomain))
		{
			CoerceToDomain *d = (CoerceToDomain *) expr;

			if (DomainHasConstraints(d->resulttype))
				return true;
			expr = (Node *) d->arg;
		}
		else
			return true;
	}
}

/*
 * ALTER COLUMN .. SET DATA TYPE
 *
 * Return the address of the modified column.
 */
static ObjectAddress
ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  AlterTableCmd *cmd, LOCKMODE lockmode)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	HeapTuple	heapTup;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	HeapTuple	typeTuple;
	Form_pg_type tform;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	Node	   *defaultexpr;
	Relation	attrelation;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;
	ObjectAddress address;

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
	typeTuple = typenameType(NULL, typeName, &targettypmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	targettype = HeapTupleGetOid(typeTuple);
	/* And the collation */
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/*
	 * If there is a default expression for the column, get it and ensure we
	 * can coerce it to the new datatype.  (We must do this before changing
	 * the column type, because build_column_default itself will try to
	 * coerce, and will not issue the error message we want if it fails.)
	 *
	 * We remove any implicit coercion steps at the top level of the old
	 * default expression; this has been agreed to satisfy the principle of
	 * least surprise.  (The conversion to the new column type should act like
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
											COERCE_IMPLICIT_CAST,
											-1);
		if (defaultexpr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("default for column \"%s\" cannot be cast automatically to type %s",
							colName, format_type_be(targettype))));
	}
	else
		defaultexpr = NULL;

	/*
	 * Find everything that depends on the column (constraints, indexes, etc),
	 * and record enough information to let us recreate the objects.
	 *
	 * The actual recreation does not happen here, but only after we have
	 * performed all the individual ALTER TYPE operations.  We have to save
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
							  NULL, 3, key);

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
					char	   *defstring = pg_get_constraintdef_command(foundObject.objectId);

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

			case OCLASS_TRIGGER:

				/*
				 * A trigger can depend on a column because the column is
				 * specified as an update target, or because the column is
				 * used in the trigger's WHEN condition.  The first case would
				 * not require any extra work, but the second case would
				 * require updating the WHEN expression, which will take a
				 * significant amount of new code.  Since we can't easily tell
				 * which case applies, we punt for both.  FIXME someday.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used in a trigger definition"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject),
								   colName)));
				break;

			case OCLASS_POLICY:

				/*
				 * A policy can depend on a column because the column is
				 * specified in the policy's USING or WITH CHECK qual
				 * expressions.  It might be possible to rewrite and recheck
				 * the policy expression, but punt for now.  It's certainly
				 * easy enough to remove and recreate the policy; still, FIXME
				 * someday.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used in a policy definition"),
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
			case OCLASS_COLLATION:
			case OCLASS_CONVERSION:
			case OCLASS_LANGUAGE:
			case OCLASS_LARGEOBJECT:
			case OCLASS_OPERATOR:
			case OCLASS_OPCLASS:
			case OCLASS_OPFAMILY:
			case OCLASS_AMOP:
			case OCLASS_AMPROC:
			case OCLASS_SCHEMA:
			case OCLASS_TSPARSER:
			case OCLASS_TSDICT:
			case OCLASS_TSTEMPLATE:
			case OCLASS_TSCONFIG:
			case OCLASS_ROLE:
			case OCLASS_DATABASE:
			case OCLASS_TBLSPACE:
			case OCLASS_FDW:
			case OCLASS_FOREIGN_SERVER:
			case OCLASS_USER_MAPPING:
			case OCLASS_DEFACL:
			case OCLASS_EXTENSION:

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
	 * want to remove, and possibly a collation dependency.
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
							  NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);

		if (foundDep->deptype != DEPENDENCY_NORMAL)
			elog(ERROR, "found unexpected dependency type '%c'",
				 foundDep->deptype);
		if (!(foundDep->refclassid == TypeRelationId &&
			  foundDep->refobjid == attTup->atttypid) &&
			!(foundDep->refclassid == CollationRelationId &&
			  foundDep->refobjid == attTup->attcollation))
			elog(ERROR, "found unexpected dependency for column");

		simple_heap_delete(depRel, &depTup->t_self);
	}

	systable_endscan(scan);

	heap_close(depRel, RowExclusiveLock);

	/*
	 * Here we go --- change the recorded column type and collation.  (Note
	 * heapTup is a copy of the syscache entry, so okay to scribble on.)
	 */
	attTup->atttypid = targettype;
	attTup->atttypmod = targettypmod;
	attTup->attcollation = targetcollid;
	attTup->attndims = list_length(typeName->arrayBounds);
	attTup->attlen = tform->typlen;
	attTup->attbyval = tform->typbyval;
	attTup->attalign = tform->typalign;
	attTup->attstorage = tform->typstorage;

	ReleaseSysCache(typeTuple);

	simple_heap_update(attrelation, &heapTup->t_self, heapTup);

	/* keep system catalog indexes current */
	CatalogUpdateIndexes(attrelation, heapTup);

	heap_close(attrelation, RowExclusiveLock);

	/* Install dependencies on new datatype and collation */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);
	add_column_collation_dependency(RelationGetRelid(rel), attnum, targetcollid);

	/*
	 * Drop any pg_statistic entry for the column, since it's now wrong type
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

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
		RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true,
						  true);

		StoreAttrDefault(rel, attnum, defaultexpr, true);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	/* Cleanup */
	heap_freetuple(heapTup);

	return address;
}

/*
 * Returns the address of the modified column
 */
static ObjectAddress
ATExecAlterColumnGenericOptions(Relation rel,
								const char *colName,
								List *options,
								LOCKMODE lockmode)
{
	Relation	ftrel;
	Relation	attrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Datum		datum;
	Form_pg_foreign_table fttableform;
	Form_pg_attribute atttableform;
	AttrNumber	attnum;
	ObjectAddress address;

	if (options == NIL)
		return InvalidObjectAddress;

	/* First, determine FDW validator associated to the foreign table. */
	ftrel = heap_open(ForeignTableRelationId, AccessShareLock);
	tuple = SearchSysCache1(FOREIGNTABLEREL, rel->rd_id);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	fttableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(fttableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	heap_close(ftrel, AccessShareLock);
	ReleaseSysCache(tuple);

	attrel = heap_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* Prevent them from altering a system attribute */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", colName)));


	/* Initialize buffers for new tuple values */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the current options */
	datum = SysCacheGetAttr(ATTNAME,
							tuple,
							Anum_pg_attribute_attfdwoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the options */
	datum = transformGenericOptions(AttributeRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_attribute_attfdwoptions - 1] = datum;
	else
		repl_null[Anum_pg_attribute_attfdwoptions - 1] = true;

	repl_repl[Anum_pg_attribute_attfdwoptions - 1] = true;

	/* Everything looks good - update the tuple */

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrel),
								 repl_val, repl_null, repl_repl);

	simple_heap_update(attrel, &newtuple->t_self, newtuple);
	CatalogUpdateIndexes(attrel, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  atttableform->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	ReleaseSysCache(tuple);

	heap_close(attrel, RowExclusiveLock);

	heap_freetuple(newtuple);

	return address;
}

/*
 * Cleanup after we've finished all the ALTER TYPE operations for a
 * particular relation.  We have to drop and recreate all the indexes
 * and constraints that depend on the altered columns.
 */
static void
ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab, LOCKMODE lockmode)
{
	ObjectAddress obj;
	ListCell   *def_item;
	ListCell   *oid_item;

	/*
	 * Re-parse the index and constraint definitions, and attach them to the
	 * appropriate work queue entries.  We do this before dropping because in
	 * the case of a FOREIGN KEY constraint, we might not yet have exclusive
	 * lock on the table the constraint is attached to, and we need to get
	 * that before dropping.  It's safe because the parser won't actually look
	 * at the catalogs to detect the existing entry.
	 *
	 * We can't rely on the output of deparsing to tell us which relation to
	 * operate on, because concurrent activity might have made the name
	 * resolve differently.  Instead, we've got to use the OID of the
	 * constraint or index we're processing to figure out which relation to
	 * operate on.
	 */
	forboth(oid_item, tab->changedConstraintOids,
			def_item, tab->changedConstraintDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		HeapTuple	tup;
		Form_pg_constraint con;
		Oid			relid;
		Oid			confrelid;
		bool		conislocal;

		tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
		if (!HeapTupleIsValid(tup))		/* should not happen */
			elog(ERROR, "cache lookup failed for constraint %u", oldId);
		con = (Form_pg_constraint) GETSTRUCT(tup);
		relid = con->conrelid;
		confrelid = con->confrelid;
		conislocal = con->conislocal;
		ReleaseSysCache(tup);

		/*
		 * If the constraint is inherited (only), we don't want to inject a
		 * new definition here; it'll get recreated when ATAddCheckConstraint
		 * recurses from adding the parent table's constraint.  But we had to
		 * carry the info this far so that we can drop the constraint below.
		 */
		if (!conislocal)
			continue;

		ATPostAlterTypeParse(oldId, relid, confrelid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);
	}
	forboth(oid_item, tab->changedIndexOids,
			def_item, tab->changedIndexDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = IndexGetRelation(oldId, false);
		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);
	}

	/*
	 * Now we can drop the existing constraints and indexes --- constraints
	 * first, since some of them might depend on the indexes.  In fact, we
	 * have to delete FOREIGN KEY constraints before UNIQUE constraints, but
	 * we already ordered the constraint list to ensure that would happen. It
	 * should be okay to use DROP_RESTRICT here, since nothing else should be
	 * depending on these objects.
	 */
	foreach(oid_item, tab->changedConstraintOids)
	{
		obj.classId = ConstraintRelationId;
		obj.objectId = lfirst_oid(oid_item);
		obj.objectSubId = 0;
		performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
	}

	foreach(oid_item, tab->changedIndexOids)
	{
		obj.classId = RelationRelationId;
		obj.objectId = lfirst_oid(oid_item);
		obj.objectSubId = 0;
		performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
	}

	/*
	 * The objects will get recreated during subsequent passes over the work
	 * queue.
	 */
}

static void
ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId, char *cmd,
					 List **wqueue, LOCKMODE lockmode, bool rewrite)
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *list_item;
	Relation	rel;

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
									 transformIndexStmt(oldRelId,
														(IndexStmt *) stmt,
														cmd));
		else if (IsA(stmt, AlterTableStmt))
			querytree_list = list_concat(querytree_list,
										 transformAlterTableStmt(oldRelId,
													 (AlterTableStmt *) stmt,
																 cmd));
		else
			querytree_list = lappend(querytree_list, stmt);
	}

	/* Caller should already have acquired whatever lock we need. */
	rel = relation_open(oldRelId, NoLock);

	/*
	 * Attach each generated command to the proper place in the work queue.
	 * Note this could result in creation of entirely new work-queue entries.
	 *
	 * Also note that we have to tweak the command subtypes, because it turns
	 * out that re-creation of indexes and constraints has to act a bit
	 * differently from initial creation.
	 */
	foreach(list_item, querytree_list)
	{
		Node	   *stm = (Node *) lfirst(list_item);
		AlteredTableInfo *tab;

		tab = ATGetQueueEntry(wqueue, rel);

		if (IsA(stm, IndexStmt))
		{
			IndexStmt  *stmt = (IndexStmt *) stm;
			AlterTableCmd *newcmd;

			if (!rewrite)
				TryReuseIndex(oldId, stmt);
			/* keep the index's comment */
			stmt->idxcomment = GetComment(oldId, RelationRelationId, 0);

			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = AT_ReAddIndex;
			newcmd->def = (Node *) stmt;
			tab->subcmds[AT_PASS_OLD_INDEX] =
				lappend(tab->subcmds[AT_PASS_OLD_INDEX], newcmd);
		}
		else if (IsA(stm, AlterTableStmt))
		{
			AlterTableStmt *stmt = (AlterTableStmt *) stm;
			ListCell   *lcmd;

			foreach(lcmd, stmt->cmds)
			{
				AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

				if (cmd->subtype == AT_AddIndex)
				{
					IndexStmt  *indstmt;
					Oid			indoid;

					Assert(IsA(cmd->def, IndexStmt));

					indstmt = (IndexStmt *) cmd->def;
					indoid = get_constraint_index(oldId);

					if (!rewrite)
						TryReuseIndex(indoid, indstmt);
					/* keep any comment on the index */
					indstmt->idxcomment = GetComment(indoid,
													 RelationRelationId, 0);

					cmd->subtype = AT_ReAddIndex;
					tab->subcmds[AT_PASS_OLD_INDEX] =
						lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);

					/* recreate any comment on the constraint */
					RebuildConstraintComment(tab,
											 AT_PASS_OLD_INDEX,
											 oldId,
											 rel, indstmt->idxname);
				}
				else if (cmd->subtype == AT_AddConstraint)
				{
					Constraint *con;

					Assert(IsA(cmd->def, Constraint));

					con = (Constraint *) cmd->def;
					con->old_pktable_oid = refRelId;
					/* rewriting neither side of a FK */
					if (con->contype == CONSTR_FOREIGN &&
						!rewrite && tab->rewrite == 0)
						TryReuseForeignKey(oldId, con);
					cmd->subtype = AT_ReAddConstraint;
					tab->subcmds[AT_PASS_OLD_CONSTR] =
						lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

					/* recreate any comment on the constraint */
					RebuildConstraintComment(tab,
											 AT_PASS_OLD_CONSTR,
											 oldId,
											 rel, con->conname);
				}
				else
					elog(ERROR, "unexpected statement subtype: %d",
						 (int) cmd->subtype);
			}
		}
		else
			elog(ERROR, "unexpected statement type: %d",
				 (int) nodeTag(stm));
	}

	relation_close(rel, NoLock);
}

/*
 * Subroutine for ATPostAlterTypeParse() to recreate a comment entry for
 * a constraint that is being re-added.
 */
static void
RebuildConstraintComment(AlteredTableInfo *tab, int pass, Oid objid,
						 Relation rel, char *conname)
{
	CommentStmt *cmd;
	char	   *comment_str;
	AlterTableCmd *newcmd;

	/* Look for comment for object wanted, and leave if none */
	comment_str = GetComment(objid, ConstraintRelationId, 0);
	if (comment_str == NULL)
		return;

	/* Build node CommentStmt */
	cmd = makeNode(CommentStmt);
	cmd->objtype = OBJECT_TABCONSTRAINT;
	cmd->objname = list_make3(
				   makeString(get_namespace_name(RelationGetNamespace(rel))),
							  makeString(RelationGetRelationName(rel)),
							  makeString(conname));
	cmd->objargs = NIL;
	cmd->comment = comment_str;

	/* Append it to list of commands */
	newcmd = makeNode(AlterTableCmd);
	newcmd->subtype = AT_ReAddComment;
	newcmd->def = (Node *) cmd;
	tab->subcmds[pass] = lappend(tab->subcmds[pass], newcmd);
}

/*
 * Subroutine for ATPostAlterTypeParse().  Calls out to CheckIndexCompatible()
 * for the real analysis, then mutates the IndexStmt based on that verdict.
 */
static void
TryReuseIndex(Oid oldId, IndexStmt *stmt)
{
	if (CheckIndexCompatible(oldId,
							 stmt->accessMethod,
							 stmt->indexParams,
							 stmt->excludeOpNames))
	{
		Relation	irel = index_open(oldId, NoLock);

		stmt->oldNode = irel->rd_node.relNode;
		index_close(irel, NoLock);
	}
}

/*
 * Subroutine for ATPostAlterTypeParse().
 *
 * Stash the old P-F equality operator into the Constraint node, for possible
 * use by ATAddForeignKeyConstraint() in determining whether revalidation of
 * this constraint can be skipped.
 */
static void
TryReuseForeignKey(Oid oldId, Constraint *con)
{
	HeapTuple	tup;
	Datum		adatum;
	bool		isNull;
	ArrayType  *arr;
	Oid		   *rawarr;
	int			numkeys;
	int			i;

	Assert(con->contype == CONSTR_FOREIGN);
	Assert(con->old_conpfeqop == NIL);	/* already prepared this node */

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for constraint %u", oldId);

	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_conpfeqop, &isNull);
	if (isNull)
		elog(ERROR, "null conpfeqop for constraint %u", oldId);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	/* test follows the one in ri_FetchConstraintInfo() */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conpfeqop is not a 1-D Oid array");
	rawarr = (Oid *) ARR_DATA_PTR(arr);

	/* stash a List of the operator Oids in our Constraint node */
	for (i = 0; i < numkeys; i++)
		con->old_conpfeqop = lcons_oid(rawarr[i], con->old_conpfeqop);

	ReleaseSysCache(tup);
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
ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing, LOCKMODE lockmode)
{
	Relation	target_rel;
	Relation	class_rel;
	HeapTuple	tuple;
	Form_pg_class tuple_class;

	/*
	 * Get exclusive lock till end of transaction on the target table. Use
	 * relation_open so that we can work on indexes and sequences.
	 */
	target_rel = relation_open(relationOid, lockmode);

	/* Get its pg_class tuple, too */
	class_rel = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationOid);
	tuple_class = (Form_pg_class) GETSTRUCT(tuple);

	/* Can we change the ownership of this tuple? */
	switch (tuple_class->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
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
			errmsg("\"%s\" is not a table, view, sequence, or foreign table",
				   NameStr(tuple_class->relname))));
	}

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (tuple_class->relowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_class];
		bool		repl_null[Natts_pg_class];
		bool		repl_repl[Natts_pg_class];
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

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_class_relowner - 1] = true;
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
			repl_repl[Anum_pg_class_relacl - 1] = true;
			repl_val[Anum_pg_class_relacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(class_rel), repl_val, repl_null, repl_repl);

		simple_heap_update(class_rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(class_rel, newtuple);

		heap_freetuple(newtuple);

		/*
		 * We must similarly update any per-column ACLs to reflect the new
		 * owner; for neatness reasons that's split out as a subroutine.
		 */
		change_owner_fix_column_acls(relationOid,
									 tuple_class->relowner,
									 newOwnerId);

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
		 * Also change the ownership of the table's row type, if it has one
		 */
		if (tuple_class->relkind != RELKIND_INDEX)
			AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId);

		/*
		 * If we are operating on a table or materialized view, also change
		 * the ownership of any indexes and sequences that belong to the
		 * relation, as well as its toast table (if it has one).
		 */
		if (tuple_class->relkind == RELKIND_RELATION ||
			tuple_class->relkind == RELKIND_MATVIEW ||
			tuple_class->relkind == RELKIND_TOASTVALUE)
		{
			List	   *index_oid_list;
			ListCell   *i;

			/* Find all the indexes belonging to this relation */
			index_oid_list = RelationGetIndexList(target_rel);

			/* For each index, recursively change its ownership */
			foreach(i, index_oid_list)
				ATExecChangeOwner(lfirst_oid(i), newOwnerId, true, lockmode);

			list_free(index_oid_list);
		}

		if (tuple_class->relkind == RELKIND_RELATION ||
			tuple_class->relkind == RELKIND_MATVIEW)
		{
			/* If it has a toast table, recurse to change its ownership */
			if (tuple_class->reltoastrelid != InvalidOid)
				ATExecChangeOwner(tuple_class->reltoastrelid, newOwnerId,
								  true, lockmode);

			/* If it has dependent sequences, recurse to change them too */
			change_owner_recurse_to_sequences(relationOid, newOwnerId, lockmode);
		}
	}

	InvokeObjectPostAlterHook(RelationRelationId, relationOid, 0);

	ReleaseSysCache(tuple);
	heap_close(class_rel, RowExclusiveLock);
	relation_close(target_rel, NoLock);
}

/*
 * change_owner_fix_column_acls
 *
 * Helper function for ATExecChangeOwner.  Scan the columns of the table
 * and fix any non-null column ACLs to reflect the new owner.
 */
static void
change_owner_fix_column_acls(Oid relationOid, Oid oldOwnerId, Oid newOwnerId)
{
	Relation	attRelation;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	attributeTuple;

	attRelation = heap_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationOid));
	scan = systable_beginscan(attRelation, AttributeRelidNumIndexId,
							  true, NULL, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);
		Datum		repl_val[Natts_pg_attribute];
		bool		repl_null[Natts_pg_attribute];
		bool		repl_repl[Natts_pg_attribute];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Ignore dropped columns */
		if (att->attisdropped)
			continue;

		aclDatum = heap_getattr(attributeTuple,
								Anum_pg_attribute_attacl,
								RelationGetDescr(attRelation),
								&isNull);
		/* Null ACLs do not require changes */
		if (isNull)
			continue;

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		newAcl = aclnewowner(DatumGetAclP(aclDatum),
							 oldOwnerId, newOwnerId);
		repl_repl[Anum_pg_attribute_attacl - 1] = true;
		repl_val[Anum_pg_attribute_attacl - 1] = PointerGetDatum(newAcl);

		newtuple = heap_modify_tuple(attributeTuple,
									 RelationGetDescr(attRelation),
									 repl_val, repl_null, repl_repl);

		simple_heap_update(attRelation, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(attRelation, newtuple);

		heap_freetuple(newtuple);
	}
	systable_endscan(scan);
	heap_close(attRelation, RowExclusiveLock);
}

/*
 * change_owner_recurse_to_sequences
 *
 * Helper function for ATExecChangeOwner.  Examines pg_depend searching
 * for sequences that are dependent on serial columns, and changes their
 * ownership.
 */
static void
change_owner_recurse_to_sequences(Oid relationOid, Oid newOwnerId, LOCKMODE lockmode)
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
							  NULL, 2, key);

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
		seqRel = relation_open(depForm->objid, lockmode);

		/* skip non-sequence relations */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* We don't need to close the sequence while we alter it. */
		ATExecChangeOwner(depForm->objid, newOwnerId, true, lockmode);

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
 *
 * Return the address of the new clustering index.
 */
static ObjectAddress
ATExecClusterOn(Relation rel, const char *indexName, LOCKMODE lockmode)
{
	Oid			indexOid;
	ObjectAddress address;

	indexOid = get_relname_relid(indexName, rel->rd_rel->relnamespace);

	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						indexName, RelationGetRelationName(rel))));

	/* Check index is valid to cluster on */
	check_index_is_clusterable(rel, indexOid, false, lockmode);

	/* And do the work */
	mark_index_clustered(rel, indexOid, false);

	ObjectAddressSet(address,
					 RelationRelationId, indexOid);

	return address;
}

/*
 * ALTER TABLE SET WITHOUT CLUSTER
 *
 * We have to find any indexes on the table that have indisclustered bit
 * set and turn it off.
 */
static void
ATExecDropCluster(Relation rel, LOCKMODE lockmode)
{
	mark_index_clustered(rel, InvalidOid, false);
}

/*
 * ALTER TABLE SET TABLESPACE
 */
static void
ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel, char *tablespacename, LOCKMODE lockmode)
{
	Oid			tablespaceId;

	/* Check that the tablespace exists */
	tablespaceId = get_tablespace_oid(tablespacename, false);

	/* Check permissions except when moving to database's default */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE, tablespacename);
	}

	/* Save info for Phase 3 to do the real work */
	if (OidIsValid(tab->newTableSpace))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot have multiple SET TABLESPACE subcommands")));

	tab->newTableSpace = tablespaceId;
}

/*
 * Set, reset, or replace reloptions.
 */
static void
ATExecSetRelOptions(Relation rel, List *defList, AlterTableType operation,
					LOCKMODE lockmode)
{
	Oid			relid;
	Relation	pgclass;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		datum;
	bool		isnull;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_class];
	bool		repl_null[Natts_pg_class];
	bool		repl_repl[Natts_pg_class];
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

	if (defList == NIL && operation != AT_ReplaceRelOptions)
		return;					/* nothing to do */

	pgclass = heap_open(RelationRelationId, RowExclusiveLock);

	/* Fetch heap tuple */
	relid = RelationGetRelid(rel);
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	if (operation == AT_ReplaceRelOptions)
	{
		/*
		 * If we're supposed to replace the reloptions list, we just pretend
		 * there were none before.
		 */
		datum = (Datum) 0;
		isnull = true;
	}
	else
	{
		/* Get the old reloptions */
		datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								&isnull);
	}

	/* Generate new proposed reloptions (text array) */
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 defList, NULL, validnsps, false,
									 operation == AT_ResetRelOptions);

	/* Validate */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_MATVIEW:
			(void) heap_reloptions(rel->rd_rel->relkind, newOptions, true);
			break;
		case RELKIND_VIEW:
			(void) view_reloptions(newOptions, true);
			break;
		case RELKIND_INDEX:
			(void) index_reloptions(rel->rd_am->amoptions, newOptions, true);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table, view, materialized view, index, or TOAST table",
							RelationGetRelationName(rel))));
			break;
	}

	/* Special-case validation of view options */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		Query	   *view_query = get_view_query(rel);
		List	   *view_options = untransformRelOptions(newOptions);
		ListCell   *cell;
		bool		check_option = false;

		foreach(cell, view_options)
		{
			DefElem    *defel = (DefElem *) lfirst(cell);

			if (pg_strcasecmp(defel->defname, "check_option") == 0)
				check_option = true;
		}

		/*
		 * If the check option is specified, look to see if the view is
		 * actually auto-updatable or not.
		 */
		if (check_option)
		{
			const char *view_updatable_error =
			view_query_is_auto_updatable(view_query, true);

			if (view_updatable_error)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("WITH CHECK OPTION is supported only on automatically updatable views"),
						 errhint("%s", view_updatable_error)));
		}
	}

	/*
	 * All we need do here is update the pg_class row; the new options will be
	 * propagated into relcaches during post-commit cache inval.
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newOptions;
	else
		repl_null[Anum_pg_class_reloptions - 1] = true;

	repl_repl[Anum_pg_class_reloptions - 1] = true;

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
								 repl_val, repl_null, repl_repl);

	simple_heap_update(pgclass, &newtuple->t_self, newtuple);

	CatalogUpdateIndexes(pgclass, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	/* repeat the whole exercise for the toast table, if there's one */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;
		Oid			toastid = rel->rd_rel->reltoastrelid;

		toastrel = heap_open(toastid, lockmode);

		/* Fetch heap tuple */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);

		if (operation == AT_ReplaceRelOptions)
		{
			/*
			 * If we're supposed to replace the reloptions list, we just
			 * pretend there were none before.
			 */
			datum = (Datum) 0;
			isnull = true;
		}
		else
		{
			/* Get the old reloptions */
			datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									&isnull);
		}

		newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
										 defList, "toast", validnsps, false,
										 operation == AT_ResetRelOptions);

		(void) heap_reloptions(RELKIND_TOASTVALUE, newOptions, true);

		memset(repl_val, 0, sizeof(repl_val));
		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		if (newOptions != (Datum) 0)
			repl_val[Anum_pg_class_reloptions - 1] = newOptions;
		else
			repl_null[Anum_pg_class_reloptions - 1] = true;

		repl_repl[Anum_pg_class_reloptions - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
									 repl_val, repl_null, repl_repl);

		simple_heap_update(pgclass, &newtuple->t_self, newtuple);

		CatalogUpdateIndexes(pgclass, newtuple);

		InvokeObjectPostAlterHookArg(RelationRelationId,
									 RelationGetRelid(toastrel), 0,
									 InvalidOid, true);

		heap_freetuple(newtuple);

		ReleaseSysCache(tuple);

		heap_close(toastrel, NoLock);
	}

	heap_close(pgclass, RowExclusiveLock);
}

/*
 * Execute ALTER TABLE SET TABLESPACE for cases where there is no tuple
 * rewriting to be done, so we just want to copy the data as fast as possible.
 */
static void
ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode)
{
	Relation	rel;
	Oid			oldTableSpace;
	Oid			reltoastrelid;
	Oid			newrelfilenode;
	RelFileNode newrnode;
	SMgrRelation dstrel;
	Relation	pg_class;
	HeapTuple	tuple;
	Form_pg_class rd_rel;
	ForkNumber	forkNum;
	List	   *reltoastidxids = NIL;
	ListCell   *lc;

	/*
	 * Need lock here in case we are recursing to toast table or index
	 */
	rel = relation_open(tableOid, lockmode);

	/*
	 * No work if no change in tablespace.
	 */
	oldTableSpace = rel->rd_rel->reltablespace;
	if (newTableSpace == oldTableSpace ||
		(newTableSpace == MyDatabaseTableSpace && oldTableSpace == 0))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel), 0);

		relation_close(rel, NoLock);
		return;
	}

	/*
	 * We cannot support moving mapped relations into different tablespaces.
	 * (In particular this eliminates all shared catalogs.)
	 */
	if (RelationIsMapped(rel))
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
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move temporary tables of other sessions")));

	reltoastrelid = rel->rd_rel->reltoastrelid;
	/* Fetch the list of indexes on toast relation if necessary */
	if (OidIsValid(reltoastrelid))
	{
		Relation	toastRel = relation_open(reltoastrelid, lockmode);

		reltoastidxids = RelationGetIndexList(toastRel);
		relation_close(toastRel, lockmode);
	}

	/* Get a modifiable copy of the relation's pg_class row */
	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(tableOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", tableOid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	/*
	 * Relfilenodes are not unique in databases across tablespaces, so we need
	 * to allocate a new one in the new tablespace.
	 */
	newrelfilenode = GetNewRelFileNode(newTableSpace, NULL,
									   rel->rd_rel->relpersistence);

	/* Open old and new relation */
	newrnode = rel->rd_node;
	newrnode.relNode = newrelfilenode;
	newrnode.spcNode = newTableSpace;
	dstrel = smgropen(newrnode, rel->rd_backend);

	RelationOpenSmgr(rel);

	/*
	 * Create and copy all forks of the relation, and schedule unlinking of
	 * old physical files.
	 *
	 * NOTE: any conflict in relfilenode value will be caught in
	 * RelationCreateStorage().
	 */
	RelationCreateStorage(newrnode, rel->rd_rel->relpersistence);

	/* copy main fork */
	copy_relation_data(rel->rd_smgr, dstrel, MAIN_FORKNUM,
					   rel->rd_rel->relpersistence);

	/* copy those extra forks that exist */
	for (forkNum = MAIN_FORKNUM + 1; forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(rel->rd_smgr, forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 */
			if (rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(&newrnode, forkNum);
			copy_relation_data(rel->rd_smgr, dstrel, forkNum,
							   rel->rd_rel->relpersistence);
		}
	}

	/* drop old relation, and close new one */
	RelationDropStorage(rel);
	smgrclose(dstrel);

	/* update the pg_class row */
	rd_rel->reltablespace = (newTableSpace == MyDatabaseTableSpace) ? InvalidOid : newTableSpace;
	rd_rel->relfilenode = newrelfilenode;
	simple_heap_update(pg_class, &tuple->t_self, tuple);
	CatalogUpdateIndexes(pg_class, tuple);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	heap_freetuple(tuple);

	heap_close(pg_class, RowExclusiveLock);

	relation_close(rel, NoLock);

	/* Make sure the reltablespace change is visible */
	CommandCounterIncrement();

	/* Move associated toast relation and/or indexes, too */
	if (OidIsValid(reltoastrelid))
		ATExecSetTableSpace(reltoastrelid, newTableSpace, lockmode);
	foreach(lc, reltoastidxids)
		ATExecSetTableSpace(lfirst_oid(lc), newTableSpace, lockmode);

	/* Clean up */
	list_free(reltoastidxids);
}

/*
 * Alter Table ALL ... SET TABLESPACE
 *
 * Allows a user to move all objects of some type in a given tablespace in the
 * current database to another tablespace.  Objects can be chosen based on the
 * owner of the object also, to allow users to move only their objects.
 * The user must have CREATE rights on the new tablespace, as usual.   The main
 * permissions handling is done by the lower-level table move function.
 *
 * All to-be-moved objects are locked first. If NOWAIT is specified and the
 * lock can't be acquired then we ereport(ERROR).
 */
Oid
AlterTableMoveAll(AlterTableMoveAllStmt *stmt)
{
	List	   *relations = NIL;
	ListCell   *l;
	ScanKeyData key[1];
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Oid			orig_tablespaceoid;
	Oid			new_tablespaceoid;
	List	   *role_oids = roleSpecsToIds(stmt->roles);

	/* Ensure we were not asked to move something we can't */
	if (stmt->objtype != OBJECT_TABLE && stmt->objtype != OBJECT_INDEX &&
		stmt->objtype != OBJECT_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only tables, indexes, and materialized views exist in tablespaces")));

	/* Get the orig and new tablespace OIDs */
	orig_tablespaceoid = get_tablespace_oid(stmt->orig_tablespacename, false);
	new_tablespaceoid = get_tablespace_oid(stmt->new_tablespacename, false);

	/* Can't move shared relations in to or out of pg_global */
	/* This is also checked by ATExecSetTableSpace, but nice to stop earlier */
	if (orig_tablespaceoid == GLOBALTABLESPACE_OID ||
		new_tablespaceoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move relations in to or out of pg_global tablespace")));

	/*
	 * Must have CREATE rights on the new tablespace, unless it is the
	 * database default tablespace (which all users implicitly have CREATE
	 * rights on).
	 */
	if (OidIsValid(new_tablespaceoid) && new_tablespaceoid != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(new_tablespaceoid, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   get_tablespace_name(new_tablespaceoid));
	}

	/*
	 * Now that the checks are done, check if we should set either to
	 * InvalidOid because it is our database's default tablespace.
	 */
	if (orig_tablespaceoid == MyDatabaseTableSpace)
		orig_tablespaceoid = InvalidOid;

	if (new_tablespaceoid == MyDatabaseTableSpace)
		new_tablespaceoid = InvalidOid;

	/* no-op */
	if (orig_tablespaceoid == new_tablespaceoid)
		return new_tablespaceoid;

	/*
	 * Walk the list of objects in the tablespace and move them. This will
	 * only find objects in our database, of course.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_class_reltablespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(orig_tablespaceoid));

	rel = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 1, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			relOid = HeapTupleGetOid(tuple);
		Form_pg_class relForm;

		relForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Do not move objects in pg_catalog as part of this, if an admin
		 * really wishes to do so, they can issue the individual ALTER
		 * commands directly.
		 *
		 * Also, explicitly avoid any shared tables, temp tables, or TOAST
		 * (TOAST will be moved with the main table).
		 */
		if (IsSystemNamespace(relForm->relnamespace) || relForm->relisshared ||
			isAnyTempNamespace(relForm->relnamespace) ||
			relForm->relnamespace == PG_TOAST_NAMESPACE)
			continue;

		/* Only move the object type requested */
		if ((stmt->objtype == OBJECT_TABLE &&
			 relForm->relkind != RELKIND_RELATION) ||
			(stmt->objtype == OBJECT_INDEX &&
			 relForm->relkind != RELKIND_INDEX) ||
			(stmt->objtype == OBJECT_MATVIEW &&
			 relForm->relkind != RELKIND_MATVIEW))
			continue;

		/* Check if we are only moving objects owned by certain roles */
		if (role_oids != NIL && !list_member_oid(role_oids, relForm->relowner))
			continue;

		/*
		 * Handle permissions-checking here since we are locking the tables
		 * and also to avoid doing a bunch of work only to fail part-way. Note
		 * that permissions will also be checked by AlterTableInternal().
		 *
		 * Caller must be considered an owner on the table to move it.
		 */
		if (!pg_class_ownercheck(relOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
						   NameStr(relForm->relname));

		if (stmt->nowait &&
			!ConditionalLockRelationOid(relOid, AccessExclusiveLock))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("aborting because lock on relation \"%s.%s\" is not available",
							get_namespace_name(relForm->relnamespace),
							NameStr(relForm->relname))));
		else
			LockRelationOid(relOid, AccessExclusiveLock);

		/* Add to our list of objects to move */
		relations = lappend_oid(relations, relOid);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	if (relations == NIL)
		ereport(NOTICE,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("no matching relations in tablespace \"%s\" found",
					orig_tablespaceoid == InvalidOid ? "(database default)" :
						get_tablespace_name(orig_tablespaceoid))));

	/* Everything is locked, loop through and move all of the relations. */
	foreach(l, relations)
	{
		List	   *cmds = NIL;
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_SetTableSpace;
		cmd->name = stmt->new_tablespacename;

		cmds = lappend(cmds, cmd);

		EventTriggerAlterTableStart((Node *) stmt);
		/* OID is set by AlterTableInternal */
		AlterTableInternal(lfirst_oid(l), cmds, false);
		EventTriggerAlterTableEnd();
	}

	return new_tablespaceoid;
}

/*
 * Copy data, block by block
 */
static void
copy_relation_data(SMgrRelation src, SMgrRelation dst,
				   ForkNumber forkNum, char relpersistence)
{
	char	   *buf;
	Page		page;
	bool		use_wal;
	bool		copying_initfork;
	BlockNumber nblocks;
	BlockNumber blkno;

	/*
	 * palloc the buffer so that it's MAXALIGN'd.  If it were just a local
	 * char[] array, the compiler might align it on any byte boundary, which
	 * can seriously hurt transfer speed to and from the kernel; not to
	 * mention possibly making log_newpage's accesses to the page header fail.
	 */
	buf = (char *) palloc(BLCKSZ);
	page = (Page) buf;

	/*
	 * The init fork for an unlogged relation in many respects has to be
	 * treated the same as normal relation, changes need to be WAL logged and
	 * it needs to be synced to disk.
	 */
	copying_initfork = relpersistence == RELPERSISTENCE_UNLOGGED &&
		forkNum == INIT_FORKNUM;

	/*
	 * We need to log the copied data in WAL iff WAL archiving/streaming is
	 * enabled AND it's a permanent relation.
	 */
	use_wal = XLogIsNeeded() &&
		(relpersistence == RELPERSISTENCE_PERMANENT || copying_initfork);

	nblocks = smgrnblocks(src, forkNum);

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		/* If we got a cancel signal during the copy of the data, quit */
		CHECK_FOR_INTERRUPTS();

		smgrread(src, forkNum, blkno, buf);

		if (!PageIsVerified(page, blkno))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid page in block %u of relation %s",
							blkno,
							relpathbackend(src->smgr_rnode.node,
										   src->smgr_rnode.backend,
										   forkNum))));

		/*
		 * WAL-log the copied page. Unfortunately we don't know what kind of a
		 * page this is, so we have to log the full page including any unused
		 * space.
		 */
		if (use_wal)
			log_newpage(&dst->smgr_rnode.node, forkNum, blkno, page, false);

		PageSetChecksumInplace(page, blkno);

		/*
		 * Now write the page.  We say isTemp = true even if it's not a temp
		 * rel, because there's no need for smgr to schedule an fsync for this
		 * write; we'll do it ourselves below.
		 */
		smgrextend(dst, forkNum, blkno, buf, true);
	}

	pfree(buf);

	/*
	 * If the rel is WAL-logged, must fsync before commit.  We use heap_sync
	 * to ensure that the toast table gets fsync'd too.  (For a temp or
	 * unlogged rel we don't care since the data will be gone after a crash
	 * anyway.)
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
	if (relpersistence == RELPERSISTENCE_PERMANENT || copying_initfork)
		smgrimmedsync(dst, forkNum);
}

/*
 * ALTER TABLE ENABLE/DISABLE TRIGGER
 *
 * We just pass this off to trigger.c.
 */
static void
ATExecEnableDisableTrigger(Relation rel, char *trigname,
						char fires_when, bool skip_system, LOCKMODE lockmode)
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
						char fires_when, LOCKMODE lockmode)
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
ATPrepAddInherit(Relation child_rel)
{
	if (child_rel->rd_rel->reloftype)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of typed table")));
}

/*
 * Return the address of the new parent relation.
 */
static ObjectAddress
ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode)
{
	Relation	parent_rel,
				catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	int32		inhseqno;
	List	   *children;
	ObjectAddress address;

	/*
	 * A self-exclusive lock is needed here.  See the similar case in
	 * MergeAttributes() for a full explanation.
	 */
	parent_rel = heap_openrv(parent, ShareUpdateExclusiveLock);

	/*
	 * Must be owner of both parent and child -- child was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(parent_rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/* Permanent rels cannot inherit from temporary ones */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		child_rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation \"%s\"",
						RelationGetRelationName(parent_rel))));

	/* If parent rel is temp, it must belong to this session */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!parent_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
		errmsg("cannot inherit from temporary relation of another session")));

	/* Ditto for the child */
	if (child_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!child_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
		 errmsg("cannot inherit to temporary relation of another session")));

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
							  true, NULL, 1, &key);

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
	 *
	 * We use weakest lock we can on child's children, namely AccessShareLock.
	 */
	children = find_all_inheritors(RelationGetRelid(child_rel),
								   AccessShareLock, NULL);

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

	/* Match up the columns and bump attinhcount as needed */
	MergeAttributesIntoExisting(child_rel, parent_rel);

	/* Match up the constraints and bump coninhcount as needed */
	MergeConstraintsIntoExisting(child_rel, parent_rel);

	/*
	 * OK, it looks valid.  Make the catalog entries that show inheritance.
	 */
	StoreCatalogInheritance1(RelationGetRelid(child_rel),
							 RelationGetRelid(parent_rel),
							 inhseqno + 1,
							 catalogRelation);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* Now we're done with pg_inherits */
	heap_close(catalogRelation, RowExclusiveLock);

	/* keep our lock on the parent relation until commit */
	heap_close(parent_rel, NoLock);

	return address;
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
	return TextDatumGetCString(expr);
}

/*
 * Determine whether two check constraints are functionally equivalent
 *
 * The test we apply is to see whether they reverse-compile to the same
 * source string.  This insulates us from issues like whether attributes
 * have the same physical column numbers in parent and child relations.
 */
static bool
constraints_equivalent(HeapTuple a, HeapTuple b, TupleDesc tupleDesc)
{
	Form_pg_constraint acon = (Form_pg_constraint) GETSTRUCT(a);
	Form_pg_constraint bcon = (Form_pg_constraint) GETSTRUCT(b);

	if (acon->condeferrable != bcon->condeferrable ||
		acon->condeferred != bcon->condeferred ||
		strcmp(decompile_conbin(a, tupleDesc),
			   decompile_conbin(b, tupleDesc)) != 0)
		return false;
	else
		return true;
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
	HeapTuple	tuple;

	attrrel = heap_open(AttributeRelationId, RowExclusiveLock);

	tupleDesc = RelationGetDescr(parent_rel);
	parent_natts = tupleDesc->natts;

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
			/* Check they are same type, typmod, and collation */
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (attribute->atttypid != childatt->atttypid ||
				attribute->atttypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName)));

			if (attribute->attcollation != childatt->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName)));

			/*
			 * Check child doesn't discard NOT NULL property.  (Other
			 * constraints are checked elsewhere.)
			 */
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
 * Check constraints in child table match up with constraints in parent,
 * and increment their coninhcount.
 *
 * Constraints that are marked ONLY in the parent are ignored.
 *
 * Called by ATExecAddInherit
 *
 * Currently all constraints in parent must be present in the child. One day we
 * may consider adding new constraints like CREATE TABLE does.
 *
 * XXX This is O(N^2) which may be an issue with tables with hundreds of
 * constraints. As long as tables have more like 10 constraints it shouldn't be
 * a problem though. Even 100 constraints ought not be the end of the world.
 *
 * XXX See MergeWithExistingConstraint too if you change this code.
 */
static void
MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	catalog_relation;
	TupleDesc	tuple_desc;
	SysScanDesc parent_scan;
	ScanKeyData parent_key;
	HeapTuple	parent_tuple;

	catalog_relation = heap_open(ConstraintRelationId, RowExclusiveLock);
	tuple_desc = RelationGetDescr(catalog_relation);

	/* Outer loop scans through the parent's constraint definitions */
	ScanKeyInit(&parent_key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	parent_scan = systable_beginscan(catalog_relation, ConstraintRelidIndexId,
									 true, NULL, 1, &parent_key);

	while (HeapTupleIsValid(parent_tuple = systable_getnext(parent_scan)))
	{
		Form_pg_constraint parent_con = (Form_pg_constraint) GETSTRUCT(parent_tuple);
		SysScanDesc child_scan;
		ScanKeyData child_key;
		HeapTuple	child_tuple;
		bool		found = false;

		if (parent_con->contype != CONSTRAINT_CHECK)
			continue;

		/* if the parent's constraint is marked NO INHERIT, it's not inherited */
		if (parent_con->connoinherit)
			continue;

		/* Search for a child constraint matching this one */
		ScanKeyInit(&child_key,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(child_rel)));
		child_scan = systable_beginscan(catalog_relation, ConstraintRelidIndexId,
										true, NULL, 1, &child_key);

		while (HeapTupleIsValid(child_tuple = systable_getnext(child_scan)))
		{
			Form_pg_constraint child_con = (Form_pg_constraint) GETSTRUCT(child_tuple);
			HeapTuple	child_copy;

			if (child_con->contype != CONSTRAINT_CHECK)
				continue;

			if (strcmp(NameStr(parent_con->conname),
					   NameStr(child_con->conname)) != 0)
				continue;

			if (!constraints_equivalent(parent_tuple, child_tuple, tuple_desc))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different definition for check constraint \"%s\"",
								RelationGetRelationName(child_rel),
								NameStr(parent_con->conname))));

			/* If the constraint is "no inherit" then cannot merge */
			if (child_con->connoinherit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with non-inherited constraint on child table \"%s\"",
								NameStr(child_con->conname),
								RelationGetRelationName(child_rel))));

			/*
			 * OK, bump the child constraint's inheritance count.  (If we fail
			 * later on, this change will just roll back.)
			 */
			child_copy = heap_copytuple(child_tuple);
			child_con = (Form_pg_constraint) GETSTRUCT(child_copy);
			child_con->coninhcount++;
			simple_heap_update(catalog_relation, &child_copy->t_self, child_copy);
			CatalogUpdateIndexes(catalog_relation, child_copy);
			heap_freetuple(child_copy);

			found = true;
			break;
		}

		systable_endscan(child_scan);

		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing constraint \"%s\"",
							NameStr(parent_con->conname))));
	}

	systable_endscan(parent_scan);
	heap_close(catalog_relation, RowExclusiveLock);
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
 *
 * coninhcount and conislocal for inherited constraints are adjusted in
 * exactly the same way.
 *
 * Return value is the address of the relation that is no longer parent.
 */
static ObjectAddress
ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode)
{
	Relation	parent_rel;
	Oid			parent_oid;
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	inheritsTuple,
				attributeTuple,
				constraintTuple;
	List	   *connames;
	bool		found = false;
	ObjectAddress address;

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
							  true, NULL, 1, key);

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
							  true, NULL, 1, key);
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
	 * Likewise, find inherited check constraints and disinherit them. To do
	 * this, we first need a list of the names of the parent's check
	 * constraints.  (We cheat a bit by only checking for name matches,
	 * assuming that the expressions will match.)
	 */
	catalogRelation = heap_open(ConstraintRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId,
							  true, NULL, 1, key);

	connames = NIL;

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);

		if (con->contype == CONSTRAINT_CHECK)
			connames = lappend(connames, pstrdup(NameStr(con->conname)));
	}

	systable_endscan(scan);

	/* Now scan the child's constraints */
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);
		bool		match;
		ListCell   *lc;

		if (con->contype != CONSTRAINT_CHECK)
			continue;

		match = false;
		foreach(lc, connames)
		{
			if (strcmp(NameStr(con->conname), (char *) lfirst(lc)) == 0)
			{
				match = true;
				break;
			}
		}

		if (match)
		{
			/* Decrement inhcount and possibly set islocal to true */
			HeapTuple	copyTuple = heap_copytuple(constraintTuple);
			Form_pg_constraint copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);

			if (copy_con->coninhcount <= 0)		/* shouldn't happen */
				elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
					 RelationGetRelid(rel), NameStr(copy_con->conname));

			copy_con->coninhcount--;
			if (copy_con->coninhcount == 0)
				copy_con->conislocal = true;

			simple_heap_update(catalogRelation, &copyTuple->t_self, copyTuple);
			CatalogUpdateIndexes(catalogRelation, copyTuple);
			heap_freetuple(copyTuple);
		}
	}

	parent_oid = RelationGetRelid(parent_rel);

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);

	drop_parent_dependency(RelationGetRelid(rel),
						   RelationRelationId,
						   RelationGetRelid(parent_rel));

	/*
	 * Post alter hook of this inherits. Since object_access_hook doesn't take
	 * multiple object identifiers, we relay oid of parent relation using
	 * auxiliary_id argument.
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 RelationGetRelid(rel), 0,
								 RelationGetRelid(parent_rel), false);

	/* keep our lock on the parent relation until commit */
	heap_close(parent_rel, NoLock);

	ObjectAddressSet(address, RelationRelationId, parent_oid);

	return address;
}

/*
 * Drop the dependency created by StoreCatalogInheritance1 (CREATE TABLE
 * INHERITS/ALTER TABLE INHERIT -- refclassid will be RelationRelationId) or
 * heap_create_with_catalog (CREATE TABLE OF/ALTER TABLE OF -- refclassid will
 * be TypeRelationId).  There's no convenient way to do this, so go trawling
 * through pg_depend.
 */
static void
drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	depTuple;

	catalogRelation = heap_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(catalogRelation, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(depTuple);

		if (dep->refclassid == refclassid &&
			dep->refobjid == refobjid &&
			dep->refobjsubid == 0 &&
			dep->deptype == DEPENDENCY_NORMAL)
			simple_heap_delete(catalogRelation, &depTuple->t_self);
	}

	systable_endscan(scan);
	heap_close(catalogRelation, RowExclusiveLock);
}

/*
 * ALTER TABLE OF
 *
 * Attach a table to a composite type, as though it had been created with CREATE
 * TABLE OF.  All attname, atttypid, atttypmod and attcollation must match.  The
 * subject table must not have inheritance parents.  These restrictions ensure
 * that you cannot create a configuration impossible with CREATE TABLE OF alone.
 *
 * The address of the type is returned.
 */
static ObjectAddress
ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Type		typetuple;
	Oid			typeid;
	Relation	inheritsRelation,
				relationRelation;
	SysScanDesc scan;
	ScanKeyData key;
	AttrNumber	table_attno,
				type_attno;
	TupleDesc	typeTupleDesc,
				tableTupleDesc;
	ObjectAddress tableobj,
				typeobj;
	HeapTuple	classtuple;

	/* Validate the type. */
	typetuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(typetuple);
	typeid = HeapTupleGetOid(typetuple);

	/* Fail if the table has any inheritance parents. */
	inheritsRelation = heap_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(inheritsRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("typed tables cannot inherit")));
	systable_endscan(scan);
	heap_close(inheritsRelation, AccessShareLock);

	/*
	 * Check the tuple descriptors for compatibility.  Unlike inheritance, we
	 * require that the order also match.  However, attnotnull need not match.
	 * Also unlike inheritance, we do not require matching relhasoids.
	 */
	typeTupleDesc = lookup_rowtype_tupdesc(typeid, -1);
	tableTupleDesc = RelationGetDescr(rel);
	table_attno = 1;
	for (type_attno = 1; type_attno <= typeTupleDesc->natts; type_attno++)
	{
		Form_pg_attribute type_attr,
					table_attr;
		const char *type_attname,
				   *table_attname;

		/* Get the next non-dropped type attribute. */
		type_attr = typeTupleDesc->attrs[type_attno - 1];
		if (type_attr->attisdropped)
			continue;
		type_attname = NameStr(type_attr->attname);

		/* Get the next non-dropped table attribute. */
		do
		{
			if (table_attno > tableTupleDesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table is missing column \"%s\"",
								type_attname)));
			table_attr = tableTupleDesc->attrs[table_attno++ - 1];
		} while (table_attr->attisdropped);
		table_attname = NameStr(table_attr->attname);

		/* Compare name. */
		if (strncmp(table_attname, type_attname, NAMEDATALEN) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("table has column \"%s\" where type requires \"%s\"",
						table_attname, type_attname)));

		/* Compare type. */
		if (table_attr->atttypid != type_attr->atttypid ||
			table_attr->atttypmod != type_attr->atttypmod ||
			table_attr->attcollation != type_attr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
				  errmsg("table \"%s\" has different type for column \"%s\"",
						 RelationGetRelationName(rel), type_attname)));
	}
	DecrTupleDescRefCount(typeTupleDesc);

	/* Any remaining columns at the end of the table had better be dropped. */
	for (; table_attno <= tableTupleDesc->natts; table_attno++)
	{
		Form_pg_attribute table_attr = tableTupleDesc->attrs[table_attno - 1];

		if (!table_attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has extra column \"%s\"",
							NameStr(table_attr->attname))));
	}

	/* If the table was already typed, drop the existing dependency. */
	if (rel->rd_rel->reloftype)
		drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype);

	/* Record a dependency on the new type. */
	tableobj.classId = RelationRelationId;
	tableobj.objectId = relid;
	tableobj.objectSubId = 0;
	typeobj.classId = TypeRelationId;
	typeobj.objectId = typeid;
	typeobj.objectSubId = 0;
	recordDependencyOn(&tableobj, &typeobj, DEPENDENCY_NORMAL);

	/* Update pg_class.reloftype */
	relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
	classtuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classtuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(classtuple))->reloftype = typeid;
	simple_heap_update(relationRelation, &classtuple->t_self, classtuple);
	CatalogUpdateIndexes(relationRelation, classtuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(classtuple);
	heap_close(relationRelation, RowExclusiveLock);

	ReleaseSysCache(typetuple);

	return typeobj;
}

/*
 * ALTER TABLE NOT OF
 *
 * Detach a typed table from its originating type.  Just clear reloftype and
 * remove the dependency.
 */
static void
ATExecDropOf(Relation rel, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Relation	relationRelation;
	HeapTuple	tuple;

	if (!OidIsValid(rel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a typed table",
						RelationGetRelationName(rel))));

	/*
	 * We don't bother to check ownership of the type --- ownership of the
	 * table is presumed enough rights.  No lock required on the type, either.
	 */

	drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype);

	/* Clear pg_class.reloftype */
	relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(tuple))->reloftype = InvalidOid;
	simple_heap_update(relationRelation, &tuple->t_self, tuple);
	CatalogUpdateIndexes(relationRelation, tuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(tuple);
	heap_close(relationRelation, RowExclusiveLock);
}

/*
 * relation_mark_replica_identity: Update a table's replica identity
 *
 * Iff ri_type = REPLICA_IDENTITY_INDEX, indexOid must be the Oid of a suitable
 * index. Otherwise, it should be InvalidOid.
 */
static void
relation_mark_replica_identity(Relation rel, char ri_type, Oid indexOid,
							   bool is_internal)
{
	Relation	pg_index;
	Relation	pg_class;
	HeapTuple	pg_class_tuple;
	HeapTuple	pg_index_tuple;
	Form_pg_class pg_class_form;
	Form_pg_index pg_index_form;

	ListCell   *index;

	/*
	 * Check whether relreplident has changed, and update it if so.
	 */
	pg_class = heap_open(RelationRelationId, RowExclusiveLock);
	pg_class_tuple = SearchSysCacheCopy1(RELOID,
									ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(pg_class_tuple))
		elog(ERROR, "cache lookup failed for relation \"%s\"",
			 RelationGetRelationName(rel));
	pg_class_form = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	if (pg_class_form->relreplident != ri_type)
	{
		pg_class_form->relreplident = ri_type;
		simple_heap_update(pg_class, &pg_class_tuple->t_self, pg_class_tuple);
		CatalogUpdateIndexes(pg_class, pg_class_tuple);
	}
	heap_close(pg_class, RowExclusiveLock);
	heap_freetuple(pg_class_tuple);

	/*
	 * Check whether the correct index is marked indisreplident; if so, we're
	 * done.
	 */
	if (OidIsValid(indexOid))
	{
		Assert(ri_type == REPLICA_IDENTITY_INDEX);

		pg_index_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexOid));
		if (!HeapTupleIsValid(pg_index_tuple))
			elog(ERROR, "cache lookup failed for index %u", indexOid);
		pg_index_form = (Form_pg_index) GETSTRUCT(pg_index_tuple);

		if (pg_index_form->indisreplident)
		{
			ReleaseSysCache(pg_index_tuple);
			return;
		}
		ReleaseSysCache(pg_index_tuple);
	}

	/*
	 * Clear the indisreplident flag from any index that had it previously,
	 * and set it for any index that should have it now.
	 */
	pg_index = heap_open(IndexRelationId, RowExclusiveLock);
	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);
		bool		dirty = false;

		pg_index_tuple = SearchSysCacheCopy1(INDEXRELID,
											 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(pg_index_tuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		pg_index_form = (Form_pg_index) GETSTRUCT(pg_index_tuple);

		/*
		 * Unset the bit if set.  We know it's wrong because we checked this
		 * earlier.
		 */
		if (pg_index_form->indisreplident)
		{
			dirty = true;
			pg_index_form->indisreplident = false;
		}
		else if (thisIndexOid == indexOid)
		{
			dirty = true;
			pg_index_form->indisreplident = true;
		}

		if (dirty)
		{
			simple_heap_update(pg_index, &pg_index_tuple->t_self, pg_index_tuple);
			CatalogUpdateIndexes(pg_index, pg_index_tuple);
			InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
										 InvalidOid, is_internal);
		}
		heap_freetuple(pg_index_tuple);
	}

	heap_close(pg_index, RowExclusiveLock);
}

/*
 * ALTER TABLE <name> REPLICA IDENTITY ...
 */
static void
ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode)
{
	Oid			indexOid;
	Relation	indexRel;
	int			key;

	if (stmt->identity_type == REPLICA_IDENTITY_DEFAULT)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_FULL)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_NOTHING)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_INDEX)
	{
		 /* fallthrough */ ;
	}
	else
		elog(ERROR, "unexpected identity type %u", stmt->identity_type);


	/* Check that the index exists */
	indexOid = get_relname_relid(stmt->name, rel->rd_rel->relnamespace);
	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						stmt->name, RelationGetRelationName(rel))));

	indexRel = index_open(indexOid, ShareLock);

	/* Check that the index is on the relation we're altering. */
	if (indexRel->rd_index == NULL ||
		indexRel->rd_index->indrelid != RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(indexRel),
						RelationGetRelationName(rel))));
	/* The AM must support uniqueness, and the index must in fact be unique. */
	if (!indexRel->rd_am->amcanunique || !indexRel->rd_index->indisunique)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("cannot use non-unique index \"%s\" as replica identity",
					RelationGetRelationName(indexRel))));
	/* Deferred indexes are not guaranteed to be always unique. */
	if (!indexRel->rd_index->indimmediate)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		  errmsg("cannot use non-immediate index \"%s\" as replica identity",
				 RelationGetRelationName(indexRel))));
	/* Expression indexes aren't supported. */
	if (RelationGetIndexExpressions(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot use expression index \"%s\" as replica identity",
					RelationGetRelationName(indexRel))));
	/* Predicate indexes aren't supported. */
	if (RelationGetIndexPredicate(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use partial index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* And neither are invalid indexes. */
	if (!IndexIsValid(indexRel->rd_index))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use invalid index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));

	/* Check index for nullable columns. */
	for (key = 0; key < indexRel->rd_index->indnatts; key++)
	{
		int16		attno = indexRel->rd_index->indkey.values[key];
		Form_pg_attribute attr;

		/* Allow OID column to be indexed; it's certainly not nullable */
		if (attno == ObjectIdAttributeNumber)
			continue;

		/*
		 * Reject any other system columns.  (Going forward, we'll disallow
		 * indexes containing such columns in the first place, but they might
		 * exist in older branches.)
		 */
		if (attno <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("index \"%s\" cannot be used as replica identity because column %d is a system column",
							RelationGetRelationName(indexRel), attno)));

		attr = rel->rd_att->attrs[attno - 1];
		if (!attr->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" cannot be used as replica identity because column \"%s\" is nullable",
							RelationGetRelationName(indexRel),
							NameStr(attr->attname))));
	}

	/* This index is suitable for use as a replica identity. Mark it. */
	relation_mark_replica_identity(rel, stmt->identity_type, indexOid, true);

	index_close(indexRel, NoLock);
}

/*
 * ALTER TABLE ENABLE/DISABLE ROW LEVEL SECURITY
 */
static void
ATExecEnableRowSecurity(Relation rel)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relrowsecurity = true;
	simple_heap_update(pg_class, &tuple->t_self, tuple);

	/* keep catalog indexes current */
	CatalogUpdateIndexes(pg_class, tuple);

	heap_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}

static void
ATExecDisableRowSecurity(Relation rel)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	/* Pull the record for this relation and update it */
	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relrowsecurity = false;
	simple_heap_update(pg_class, &tuple->t_self, tuple);

	/* keep catalog indexes current */
	CatalogUpdateIndexes(pg_class, tuple);

	heap_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER TABLE FORCE/NO FORCE ROW LEVEL SECURITY
 */
static void
ATExecForceNoForceRowSecurity(Relation rel, bool force_rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	pg_class = heap_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relforcerowsecurity = force_rls;
	simple_heap_update(pg_class, &tuple->t_self, tuple);

	/* keep catalog indexes current */
	CatalogUpdateIndexes(pg_class, tuple);

	heap_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER FOREIGN TABLE <name> OPTIONS (...)
 */
static void
ATExecGenericOptions(Relation rel, List *options)
{
	Relation	ftrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_foreign_table];
	bool		repl_null[Natts_pg_foreign_table];
	bool		repl_repl[Natts_pg_foreign_table];
	Datum		datum;
	Form_pg_foreign_table tableform;

	if (options == NIL)
		return;

	ftrel = heap_open(ForeignTableRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(FOREIGNTABLEREL, rel->rd_id);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	tableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(tableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the current options */
	datum = SysCacheGetAttr(FOREIGNTABLEREL,
							tuple,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the options */
	datum = transformGenericOptions(ForeignTableRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_foreign_table_ftoptions - 1] = datum;
	else
		repl_null[Anum_pg_foreign_table_ftoptions - 1] = true;

	repl_repl[Anum_pg_foreign_table_ftoptions - 1] = true;

	/* Everything looks good - update the tuple */

	tuple = heap_modify_tuple(tuple, RelationGetDescr(ftrel),
							  repl_val, repl_null, repl_repl);

	simple_heap_update(ftrel, &tuple->t_self, tuple);
	CatalogUpdateIndexes(ftrel, tuple);

	InvokeObjectPostAlterHook(ForeignTableRelationId,
							  RelationGetRelid(rel), 0);

	heap_close(ftrel, RowExclusiveLock);

	heap_freetuple(tuple);
}

/*
 * Preparation phase for SET LOGGED/UNLOGGED
 *
 * This verifies that we're not trying to change a temp table.  Also,
 * existing foreign key constraints are checked to avoid ending up with
 * permanent tables referencing unlogged tables.
 *
 * Return value is false if the operation is a no-op (in which case the
 * checks are skipped), otherwise true.
 */
static bool
ATPrepChangePersistence(Relation rel, bool toLogged)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/*
	 * Disallow changing status for a temp table.  Also verify whether we can
	 * get away with doing nothing; in such cases we don't need to run the
	 * checks below, either.
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change logged status of table \"%s\" because it is temporary",
							RelationGetRelationName(rel)),
					 errtable(rel)));
			break;
		case RELPERSISTENCE_PERMANENT:
			if (toLogged)
				/* nothing to do */
				return false;
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!toLogged)
				/* nothing to do */
				return false;
			break;
	}

	/*
	 * Check existing foreign key constraints to preserve the invariant that
	 * permanent tables cannot reference unlogged ones.  Self-referencing
	 * foreign keys can safely be ignored.
	 */
	pg_constraint = heap_open(ConstraintRelationId, AccessShareLock);

	/*
	 * Scan conrelid if changing to permanent, else confrelid.  This also
	 * determines whether a useful index exists.
	 */
	ScanKeyInit(&skey[0],
				toLogged ? Anum_pg_constraint_conrelid :
				Anum_pg_constraint_confrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(pg_constraint,
							  toLogged ? ConstraintRelidIndexId : InvalidOid,
							  true, NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		if (con->contype == CONSTRAINT_FOREIGN)
		{
			Oid			foreignrelid;
			Relation	foreignrel;

			/* the opposite end of what we used as scankey */
			foreignrelid = toLogged ? con->confrelid : con->conrelid;

			/* ignore if self-referencing */
			if (RelationGetRelid(rel) == foreignrelid)
				continue;

			foreignrel = relation_open(foreignrelid, AccessShareLock);

			if (toLogged)
			{
				if (foreignrel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to logged because it references unlogged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}
			else
			{
				if (foreignrel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to unlogged because it references logged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}

			relation_close(foreignrel, AccessShareLock);
		}
	}

	systable_endscan(scan);

	heap_close(pg_constraint, AccessShareLock);

	return true;
}

/*
 * Execute ALTER TABLE SET SCHEMA
 */
ObjectAddress
AlterTableNamespace(AlterObjectSchemaStmt *stmt, Oid *oldschema)
{
	Relation	rel;
	Oid			relid;
	Oid			oldNspOid;
	Oid			nspOid;
	RangeVar   *newrv;
	ObjectAddresses *objsMoved;
	ObjectAddress myself;

	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 stmt->missing_ok, false,
									 RangeVarCallbackForAlterRelation,
									 (void *) stmt);

	if (!OidIsValid(relid))
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return InvalidObjectAddress;
	}

	rel = relation_open(relid, NoLock);

	oldNspOid = RelationGetNamespace(rel);

	/* If it's an owned sequence, disallow moving it by itself. */
	if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
	{
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

	/* Get and lock schema OID and check its permissions. */
	newrv = makeRangeVar(stmt->newschema, RelationGetRelationName(rel), -1);
	nspOid = RangeVarGetAndCheckCreationNamespace(newrv, NoLock, NULL);

	/* common checks on switching namespaces */
	CheckSetNamespace(oldNspOid, nspOid, RelationRelationId, relid);

	objsMoved = new_object_addresses();
	AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);
	free_object_addresses(objsMoved);

	ObjectAddressSet(myself, RelationRelationId, relid);

	if (oldschema)
		*oldschema = oldNspOid;

	/* close rel, but keep lock until commit */
	relation_close(rel, NoLock);

	return myself;
}

/*
 * The guts of relocating a table or materialized view to another namespace:
 * besides moving the relation itself, its dependent objects are relocated to
 * the new schema.
 */
void
AlterTableNamespaceInternal(Relation rel, Oid oldNspOid, Oid nspOid,
							ObjectAddresses *objsMoved)
{
	Relation	classRel;

	Assert(objsMoved != NULL);

	/* OK, modify the pg_class row and pg_depend entry */
	classRel = heap_open(RelationRelationId, RowExclusiveLock);

	AlterRelationNamespaceInternal(classRel, RelationGetRelid(rel), oldNspOid,
								   nspOid, true, objsMoved);

	/* Fix the table's row type too */
	AlterTypeNamespaceInternal(rel->rd_rel->reltype,
							   nspOid, false, false, objsMoved);

	/* Fix other dependent stuff */
	if (rel->rd_rel->relkind == RELKIND_RELATION ||
		rel->rd_rel->relkind == RELKIND_MATVIEW)
	{
		AlterIndexNamespaces(classRel, rel, oldNspOid, nspOid, objsMoved);
		AlterSeqNamespaces(classRel, rel, oldNspOid, nspOid,
						   objsMoved, AccessExclusiveLock);
		AlterConstraintNamespaces(RelationGetRelid(rel), oldNspOid, nspOid,
								  false, objsMoved);
	}

	heap_close(classRel, RowExclusiveLock);
}

/*
 * The guts of relocating a relation to another namespace: fix the pg_class
 * entry, and the pg_depend entry if any.  Caller must already have
 * opened and write-locked pg_class.
 */
void
AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry,
							   ObjectAddresses *objsMoved)
{
	HeapTuple	classTup;
	Form_pg_class classForm;
	ObjectAddress thisobj;

	classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(classTup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	Assert(classForm->relnamespace == oldNspOid);

	thisobj.classId = RelationRelationId;
	thisobj.objectId = relOid;
	thisobj.objectSubId = 0;

	/*
	 * Do nothing when there's nothing to do.
	 */
	if (!object_address_present(&thisobj, objsMoved))
	{
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
			changeDependencyFor(RelationRelationId,
								relOid,
								NamespaceRelationId,
								oldNspOid,
								newNspOid) != 1)
			elog(ERROR, "failed to change schema dependency for relation \"%s\"",
				 NameStr(classForm->relname));

		add_exact_object_address(&thisobj, objsMoved);

		InvokeObjectPostAlterHook(RelationRelationId, relOid, 0);
	}

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
					 Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved)
{
	List	   *indexList;
	ListCell   *l;

	indexList = RelationGetIndexList(rel);

	foreach(l, indexList)
	{
		Oid			indexOid = lfirst_oid(l);
		ObjectAddress thisobj;

		thisobj.classId = RelationRelationId;
		thisobj.objectId = indexOid;
		thisobj.objectSubId = 0;

		/*
		 * Note: currently, the index will not have its own dependency on the
		 * namespace, so we don't need to do changeDependencyFor(). There's no
		 * row type in pg_type, either.
		 *
		 * XXX this objsMoved test may be pointless -- surely we have a single
		 * dependency link from a relation to each index?
		 */
		if (!object_address_present(&thisobj, objsMoved))
		{
			AlterRelationNamespaceInternal(classRel, indexOid,
										   oldNspOid, newNspOid,
										   false, objsMoved);
			add_exact_object_address(&thisobj, objsMoved);
		}
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
				   Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved,
				   LOCKMODE lockmode)
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
							  NULL, 2, key);

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
		seqRel = relation_open(depForm->objid, lockmode);

		/* skip non-sequence relations */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* Fix the pg_class and pg_depend entries */
		AlterRelationNamespaceInternal(classRel, depForm->objid,
									   oldNspOid, newNspOid,
									   true, objsMoved);

		/*
		 * Sequences have entries in pg_type. We need to be careful to move
		 * them to the new namespace, too.
		 */
		AlterTypeNamespaceInternal(RelationGetForm(seqRel)->reltype,
								   newNspOid, false, false, objsMoved);

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

				/*
				 * If this transaction hasn't accessed any temporary
				 * relations, we can skip truncating ON COMMIT DELETE ROWS
				 * tables, as they must still be empty.
				 */
				if (MyXactAccessedTempRel)
					oids_to_truncate = lappend_oid(oids_to_truncate, oc->relid);
				break;
			case ONCOMMIT_DROP:
				{
					ObjectAddress object;

					object.classId = RelationRelationId;
					object.objectId = oc->relid;
					object.objectSubId = 0;

					/*
					 * Since this is an automatic drop, rather than one
					 * directly initiated by the user, we pass the
					 * PERFORM_DELETION_INTERNAL flag.
					 */
					performDeletion(&object,
									DROP_CASCADE, PERFORM_DELETION_INTERNAL);

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
 * subtransaction.  During subcommit, just relabel entries marked during
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

/*
 * This is intended as a callback for RangeVarGetRelidExtended().  It allows
 * the relation to be locked only if (1) it's a plain table, materialized
 * view, or TOAST table and (2) the current user is the owner (or the
 * superuser).  This meets the permission-checking needs of CLUSTER, REINDEX
 * TABLE, and REFRESH MATERIALIZED VIEW; we expose it here so that it can be
 * used by all.
 */
void
RangeVarCallbackOwnsTable(const RangeVar *relation,
						  Oid relId, Oid oldRelId, void *arg)
{
	char		relkind;

	/* Nothing to do if the relation was not found. */
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
	if (relkind != RELKIND_RELATION && relkind != RELKIND_TOASTVALUE &&
		relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or materialized view", relation->relname)));

	/* Check permissions */
	if (!pg_class_ownercheck(relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, relation->relname);
}

/*
 * Callback to RangeVarGetRelidExtended(), similar to
 * RangeVarCallbackOwnsTable() but without checks on the type of the relation.
 */
void
RangeVarCallbackOwnsRelation(const RangeVar *relation,
							 Oid relId, Oid oldRelId, void *arg)
{
	HeapTuple	tuple;

	/* Nothing to do if the relation was not found. */
	if (!OidIsValid(relId))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR, "cache lookup failed for relation %u", relId);

	if (!pg_class_ownercheck(relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   relation->relname);

	if (!allowSystemTableMods &&
		IsSystemClass(relId, (Form_pg_class) GETSTRUCT(tuple)))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						relation->relname)));

	ReleaseSysCache(tuple);
}

/*
 * Common RangeVarGetRelid callback for rename, set schema, and alter table
 * processing.
 */
static void
RangeVarCallbackForAlterRelation(const RangeVar *rv, Oid relid, Oid oldrelid,
								 void *arg)
{
	Node	   *stmt = (Node *) arg;
	ObjectType	reltype;
	HeapTuple	tuple;
	Form_pg_class classform;
	AclResult	aclresult;
	char		relkind;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	relkind = classform->relkind;

	/* Must own relation. */
	if (!pg_class_ownercheck(relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, rv->relname);

	/* No system table modifications unless explicitly allowed. */
	if (!allowSystemTableMods && IsSystemClass(relid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/*
	 * Extract the specified relation type from the statement parse tree.
	 *
	 * Also, for ALTER .. RENAME, check permissions: the user must (still)
	 * have CREATE rights on the containing namespace.
	 */
	if (IsA(stmt, RenameStmt))
	{
		aclresult = pg_namespace_aclcheck(classform->relnamespace,
										  GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
						   get_namespace_name(classform->relnamespace));
		reltype = ((RenameStmt *) stmt)->renameType;
	}
	else if (IsA(stmt, AlterObjectSchemaStmt))
		reltype = ((AlterObjectSchemaStmt *) stmt)->objectType;

	else if (IsA(stmt, AlterTableStmt))
		reltype = ((AlterTableStmt *) stmt)->relkind;
	else
	{
		reltype = OBJECT_TABLE; /* placate compiler */
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(stmt));
	}

	/*
	 * For compatibility with prior releases, we allow ALTER TABLE to be used
	 * with most other types of relations (but not composite types). We allow
	 * similar flexibility for ALTER INDEX in the case of RENAME, but not
	 * otherwise.  Otherwise, the user must select the correct form of the
	 * command for the relation at issue.
	 */
	if (reltype == OBJECT_SEQUENCE && relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence", rv->relname)));

	if (reltype == OBJECT_VIEW && relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a view", rv->relname)));

	if (reltype == OBJECT_MATVIEW && relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a materialized view", rv->relname)));

	if (reltype == OBJECT_FOREIGN_TABLE && relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a foreign table", rv->relname)));

	if (reltype == OBJECT_TYPE && relkind != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a composite type", rv->relname)));

	if (reltype == OBJECT_INDEX && relkind != RELKIND_INDEX
		&& !IsA(stmt, RenameStmt))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", rv->relname)));

	/*
	 * Don't allow ALTER TABLE on composite types. We want people to use ALTER
	 * TYPE for that.
	 */
	if (reltype != OBJECT_TYPE && relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type", rv->relname),
				 errhint("Use ALTER TYPE instead.")));

	/*
	 * Don't allow ALTER TABLE .. SET SCHEMA on relations that can't be moved
	 * to a different schema, such as indexes and TOAST tables.
	 */
	if (IsA(stmt, AlterObjectSchemaStmt) &&
		relkind != RELKIND_RELATION &&
		relkind != RELKIND_VIEW &&
		relkind != RELKIND_MATVIEW &&
		relkind != RELKIND_SEQUENCE &&
		relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, materialized view, sequence, or foreign table",
						rv->relname)));

	ReleaseSysCache(tuple);
}
