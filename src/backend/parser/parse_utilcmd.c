/*-------------------------------------------------------------------------
 *
 * parse_utilcmd.c
 *	  Perform parse analysis work for various utility commands
 *
 * Formerly we did this work during parse_analyze() in analyze.c.  However
 * that is fairly unsafe in the presence of querytree caching, since any
 * database state that we depend on in making the transformations might be
 * obsolete by the time the utility command is executed; and utility commands
 * have no infrastructure for holding locks or rechecking plan validity.
 * Hence these functions are now called at the start of execution of their
 * respective utility commands.
 *
 * NOTE: in general we must avoid scribbling on the passed-in raw parse
 * tree, since it might be in a plan cache.  The simplest solution is
 * a quick copyObject() call before manipulating the query tree.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/parse_utilcmd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* State shared by transformCreateStmt and its subroutines */
typedef struct
{
	ParseState *pstate;			/* overall parser state */
	const char *stmtType;		/* "CREATE [FOREIGN] TABLE" or "ALTER TABLE" */
	RangeVar   *relation;		/* relation to create */
	Relation	rel;			/* opened/locked rel, if ALTER */
	List	   *inhRelations;	/* relations to inherit from */
	bool		isforeign;		/* true if CREATE/ALTER FOREIGN TABLE */
	bool		isalter;		/* true if altering existing table */
	bool		hasoids;		/* does relation have an OID column? */
	List	   *columns;		/* ColumnDef items */
	List	   *ckconstraints;	/* CHECK constraints */
	List	   *fkconstraints;	/* FOREIGN KEY constraints */
	List	   *ixconstraints;	/* index-creating constraints */
	List	   *inh_indexes;	/* cloned indexes from INCLUDING INDEXES */
	List	   *blist;			/* "before list" of things to do before
								 * creating the table */
	List	   *alist;			/* "after list" of things to do after creating
								 * the table */
	IndexStmt  *pkey;			/* PRIMARY KEY index, if any */
} CreateStmtContext;

/* State shared by transformCreateSchemaStmt and its subroutines */
typedef struct
{
	const char *stmtType;		/* "CREATE SCHEMA" or "ALTER SCHEMA" */
	char	   *schemaname;		/* name of schema */
	char	   *authid;			/* owner of schema */
	List	   *sequences;		/* CREATE SEQUENCE items */
	List	   *tables;			/* CREATE TABLE items */
	List	   *views;			/* CREATE VIEW items */
	List	   *indexes;		/* CREATE INDEX items */
	List	   *triggers;		/* CREATE TRIGGER items */
	List	   *grants;			/* GRANT items */
} CreateSchemaStmtContext;


static void transformColumnDefinition(CreateStmtContext *cxt,
						  ColumnDef *column);
static void transformTableConstraint(CreateStmtContext *cxt,
						 Constraint *constraint);
static void transformTableLikeClause(CreateStmtContext *cxt,
						 TableLikeClause *table_like_clause);
static void transformOfType(CreateStmtContext *cxt,
				TypeName *ofTypename);
static IndexStmt *generateClonedIndexStmt(CreateStmtContext *cxt,
						Relation source_idx,
						const AttrNumber *attmap, int attmap_length);
static List *get_collation(Oid collation, Oid actual_datatype);
static List *get_opclass(Oid opclass, Oid actual_datatype);
static void transformIndexConstraints(CreateStmtContext *cxt);
static IndexStmt *transformIndexConstraint(Constraint *constraint,
						 CreateStmtContext *cxt);
static void transformFKConstraints(CreateStmtContext *cxt,
					   bool skipValidation,
					   bool isAddConstraint);
static void transformConstraintAttrs(CreateStmtContext *cxt,
						 List *constraintList);
static void transformColumnType(CreateStmtContext *cxt, ColumnDef *column);
static void setSchemaName(char *context_schema, char **stmt_schema_name);


/*
 * transformCreateStmt -
 *	  parse analysis for CREATE TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed CreateStmt, but there may be additional actions
 * to be done before and after the actual DefineRelation() call.
 *
 * SQL allows constraints to be scattered all over, so thumb through
 * the columns and collect all constraints into one place.
 * If there are any implied indices (e.g. UNIQUE or PRIMARY KEY)
 * then expand those into multiple IndexStmt blocks.
 *	  - thomas 1997-12-02
 */
List *
transformCreateStmt(CreateStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *elements;
	Oid			namespaceid;
	Oid			existing_relid;

	/*
	 * We must not scribble on the passed-in CreateStmt, so copy it.  (This is
	 * overkill, but easy.)
	 */
	stmt = (CreateStmt *) copyObject(stmt);

	/*
	 * Look up the creation namespace.	This also checks permissions on the
	 * target namespace, locks it against concurrent drops, checks for a
	 * preexisting relation in that namespace with the same name, and updates
	 * stmt->relation->relpersistence if the select namespace is temporary.
	 */
	namespaceid =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock,
											 &existing_relid);

	/*
	 * If the relation already exists and the user specified "IF NOT EXISTS",
	 * bail out with a NOTICE.
	 */
	if (stmt->if_not_exists && OidIsValid(existing_relid))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists, skipping",
						stmt->relation->relname)));
		return NIL;
	}

	/*
	 * If the target relation name isn't schema-qualified, make it so.  This
	 * prevents some corner cases in which added-on rewritten commands might
	 * think they should apply to other relations that have the same name and
	 * are earlier in the search path.	But a local temp table is effectively
	 * specified to be in pg_temp, so no need for anything extra in that case.
	 */
	if (stmt->relation->schemaname == NULL
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		stmt->relation->schemaname = get_namespace_name(namespaceid);

	/* Set up pstate and CreateStmtContext */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	cxt.pstate = pstate;
	if (IsA(stmt, CreateForeignTableStmt))
	{
		cxt.stmtType = "CREATE FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "CREATE TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = NULL;
	cxt.inhRelations = stmt->inhRelations;
	cxt.isalter = false;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.inh_indexes = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.hasoids = interpretOidsOption(stmt->options, true);

	Assert(!stmt->ofTypename || !stmt->inhRelations);	/* grammar enforces */

	if (stmt->ofTypename)
		transformOfType(&cxt, stmt->ofTypename);

	/*
	 * Run through each primary element in the table creation clause. Separate
	 * column defs from constraints, and do preliminary analysis.
	 */
	foreach(elements, stmt->tableElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_ColumnDef:
				transformColumnDefinition(&cxt, (ColumnDef *) element);
				break;

			case T_Constraint:
				transformTableConstraint(&cxt, (Constraint *) element);
				break;

			case T_TableLikeClause:
				transformTableLikeClause(&cxt, (TableLikeClause *) element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
				break;
		}
	}

	/*
	 * transformIndexConstraints wants cxt.alist to contain only index
	 * statements, so transfer anything we already have into save_alist.
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	Assert(stmt->constraints == NIL);

	/*
	 * Postprocess constraints that give rise to index definitions.
	 */
	transformIndexConstraints(&cxt);

	/*
	 * Postprocess foreign-key constraints.
	 */
	transformFKConstraints(&cxt, true, false);

	/*
	 * Output results.
	 */
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}

/*
 * transformColumnDefinition -
 *		transform a single ColumnDef within CREATE TABLE
 *		Also used in ALTER TABLE ADD COLUMN
 */
static void
transformColumnDefinition(CreateStmtContext *cxt, ColumnDef *column)
{
	bool		is_serial;
	bool		saw_nullable;
	bool		saw_default;
	Constraint *constraint;
	ListCell   *clist;

	cxt->columns = lappend(cxt->columns, column);

	/* Check for SERIAL pseudo-types */
	is_serial = false;
	if (column->typeName
		&& list_length(column->typeName->names) == 1
		&& !column->typeName->pct_type)
	{
		char	   *typname = strVal(linitial(column->typeName->names));

		if (strcmp(typname, "smallserial") == 0 ||
			strcmp(typname, "serial2") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT2OID;
		}
		else if (strcmp(typname, "serial") == 0 ||
				 strcmp(typname, "serial4") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT4OID;
		}
		else if (strcmp(typname, "bigserial") == 0 ||
				 strcmp(typname, "serial8") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT8OID;
		}

		/*
		 * We have to reject "serial[]" explicitly, because once we've set
		 * typeid, LookupTypeName won't notice arrayBounds.  We don't need any
		 * special coding for serial(typmod) though.
		 */
		if (is_serial && column->typeName->arrayBounds != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array of serial is not implemented"),
					 parser_errposition(cxt->pstate,
										column->typeName->location)));
	}

	/* Do necessary work on the column type declaration */
	if (column->typeName)
		transformColumnType(cxt, column);

	/* Special actions for SERIAL pseudo-types */
	if (is_serial)
	{
		Oid			snamespaceid;
		char	   *snamespace;
		char	   *sname;
		char	   *qstring;
		A_Const    *snamenode;
		TypeCast   *castnode;
		FuncCall   *funccallnode;
		CreateSeqStmt *seqstmt;
		AlterSeqStmt *altseqstmt;
		List	   *attnamelist;

		/*
		 * Determine namespace and name to use for the sequence.
		 *
		 * Although we use ChooseRelationName, it's not guaranteed that the
		 * selected sequence name won't conflict; given sufficiently long
		 * field names, two different serial columns in the same table could
		 * be assigned the same sequence name, and we'd not notice since we
		 * aren't creating the sequence quite yet.  In practice this seems
		 * quite unlikely to be a problem, especially since few people would
		 * need two serial columns in one table.
		 */
		if (cxt->rel)
			snamespaceid = RelationGetNamespace(cxt->rel);
		else
		{
			snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
			RangeVarAdjustRelationPersistence(cxt->relation, snamespaceid);
		}
		snamespace = get_namespace_name(snamespaceid);
		sname = ChooseRelationName(cxt->relation->relname,
								   column->colname,
								   "seq",
								   snamespaceid);

		ereport(DEBUG1,
				(errmsg("%s will create implicit sequence \"%s\" for serial column \"%s.%s\"",
						cxt->stmtType, sname,
						cxt->relation->relname, column->colname)));

		/*
		 * Build a CREATE SEQUENCE command to create the sequence object, and
		 * add it to the list of things to be done before this CREATE/ALTER
		 * TABLE.
		 */
		seqstmt = makeNode(CreateSeqStmt);
		seqstmt->sequence = makeRangeVar(snamespace, sname, -1);
		seqstmt->options = NIL;

		/*
		 * If this is ALTER ADD COLUMN, make sure the sequence will be owned
		 * by the table's owner.  The current user might be someone else
		 * (perhaps a superuser, or someone who's only a member of the owning
		 * role), but the SEQUENCE OWNED BY mechanisms will bleat unless table
		 * and sequence have exactly the same owning role.
		 */
		if (cxt->rel)
			seqstmt->ownerId = cxt->rel->rd_rel->relowner;
		else
			seqstmt->ownerId = InvalidOid;

		cxt->blist = lappend(cxt->blist, seqstmt);

		/*
		 * Build an ALTER SEQUENCE ... OWNED BY command to mark the sequence
		 * as owned by this column, and add it to the list of things to be
		 * done after this CREATE/ALTER TABLE.
		 */
		altseqstmt = makeNode(AlterSeqStmt);
		altseqstmt->sequence = makeRangeVar(snamespace, sname, -1);
		attnamelist = list_make3(makeString(snamespace),
								 makeString(cxt->relation->relname),
								 makeString(column->colname));
		altseqstmt->options = list_make1(makeDefElem("owned_by",
													 (Node *) attnamelist));

		cxt->alist = lappend(cxt->alist, altseqstmt);

		/*
		 * Create appropriate constraints for SERIAL.  We do this in full,
		 * rather than shortcutting, so that we will detect any conflicting
		 * constraints the user wrote (like a different DEFAULT).
		 *
		 * Create an expression tree representing the function call
		 * nextval('sequencename').  We cannot reduce the raw tree to cooked
		 * form until after the sequence is created, but there's no need to do
		 * so.
		 */
		qstring = quote_qualified_identifier(snamespace, sname);
		snamenode = makeNode(A_Const);
		snamenode->val.type = T_String;
		snamenode->val.val.str = qstring;
		snamenode->location = -1;
		castnode = makeNode(TypeCast);
		castnode->typeName = SystemTypeName("regclass");
		castnode->arg = (Node *) snamenode;
		castnode->location = -1;
		funccallnode = makeNode(FuncCall);
		funccallnode->funcname = SystemFuncName("nextval");
		funccallnode->args = list_make1(castnode);
		funccallnode->agg_order = NIL;
		funccallnode->agg_star = false;
		funccallnode->agg_distinct = false;
		funccallnode->func_variadic = false;
		funccallnode->over = NULL;
		funccallnode->location = -1;

		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_DEFAULT;
		constraint->location = -1;
		constraint->raw_expr = (Node *) funccallnode;
		constraint->cooked_expr = NULL;
		column->constraints = lappend(column->constraints, constraint);

		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_NOTNULL;
		constraint->location = -1;
		column->constraints = lappend(column->constraints, constraint);
	}

	/* Process column constraints, if any... */
	transformConstraintAttrs(cxt, column->constraints);

	saw_nullable = false;
	saw_default = false;

	foreach(clist, column->constraints)
	{
		constraint = lfirst(clist);
		Assert(IsA(constraint, Constraint));

		switch (constraint->contype)
		{
			case CONSTR_NULL:
				if (saw_nullable && column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->is_not_null = FALSE;
				saw_nullable = true;
				break;

			case CONSTR_NOTNULL:
				if (saw_nullable && !column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->is_not_null = TRUE;
				saw_nullable = true;
				break;

			case CONSTR_DEFAULT:
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_default = true;
				break;

			case CONSTR_CHECK:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
				break;

			case CONSTR_PRIMARY:
			case CONSTR_UNIQUE:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				if (constraint->keys == NIL)
					constraint->keys = list_make1(makeString(column->colname));
				cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
				break;

			case CONSTR_EXCLUSION:
				/* grammar does not allow EXCLUDE as a column constraint */
				elog(ERROR, "column exclusion constraints are not supported");
				break;

			case CONSTR_FOREIGN:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));

				/*
				 * Fill in the current attribute's name and throw it into the
				 * list of FK constraints to be processed later.
				 */
				constraint->fk_attrs = list_make1(makeString(column->colname));
				cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				/* transformConstraintAttrs took care of these */
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 constraint->contype);
				break;
		}
	}

	/*
	 * If needed, generate ALTER FOREIGN TABLE ALTER COLUMN statement to add
	 * per-column foreign data wrapper options to this column after creation.
	 */
	if (column->fdwoptions != NIL)
	{
		AlterTableStmt *stmt;
		AlterTableCmd *cmd;

		cmd = makeNode(AlterTableCmd);
		cmd->subtype = AT_AlterColumnGenericOptions;
		cmd->name = column->colname;
		cmd->def = (Node *) column->fdwoptions;
		cmd->behavior = DROP_RESTRICT;
		cmd->missing_ok = false;

		stmt = makeNode(AlterTableStmt);
		stmt->relation = cxt->relation;
		stmt->cmds = NIL;
		stmt->relkind = OBJECT_FOREIGN_TABLE;
		stmt->cmds = lappend(stmt->cmds, cmd);

		cxt->alist = lappend(cxt->alist, stmt);
	}
}

/*
 * transformTableConstraint
 *		transform a Constraint node within CREATE TABLE or ALTER TABLE
 */
static void
transformTableConstraint(CreateStmtContext *cxt, Constraint *constraint)
{
	if (cxt->isforeign)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("constraints are not supported on foreign tables"),
				 parser_errposition(cxt->pstate,
									constraint->location)));

	switch (constraint->contype)
	{
		case CONSTR_PRIMARY:
		case CONSTR_UNIQUE:
		case CONSTR_EXCLUSION:
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_CHECK:
			cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
			break;

		case CONSTR_FOREIGN:
			cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
			break;

		case CONSTR_NULL:
		case CONSTR_NOTNULL:
		case CONSTR_DEFAULT:
		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			elog(ERROR, "invalid context for constraint type %d",
				 constraint->contype);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 constraint->contype);
			break;
	}
}

/*
 * transformTableLikeClause
 *
 * Change the LIKE <srctable> portion of a CREATE TABLE statement into
 * column definitions which recreate the user defined column portions of
 * <srctable>.
 */
static void
transformTableLikeClause(CreateStmtContext *cxt, TableLikeClause *table_like_clause)
{
	AttrNumber	parent_attno;
	Relation	relation;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	AttrNumber *attmap;
	AclResult	aclresult;
	char	   *comment;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, cxt->pstate,
									  table_like_clause->relation->location);

	/* we could support LIKE in many cases, but worry about it another day */
	if (cxt->isforeign)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE is not supported for creating foreign tables")));

	relation = relation_openrv(table_like_clause->relation, AccessShareLock);

	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_VIEW &&
		relation->rd_rel->relkind != RELKIND_MATVIEW &&
		relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
		relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, materialized view, composite type, or foreign table",
						RelationGetRelationName(relation))));

	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * Check for privileges
	 */
	if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		aclresult = pg_type_aclcheck(relation->rd_rel->reltype, GetUserId(),
									 ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TYPE,
						   RelationGetRelationName(relation));
	}
	else
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(relation), GetUserId(),
									  ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(relation));
	}

	tupleDesc = RelationGetDescr(relation);
	constr = tupleDesc->constr;

	/*
	 * Initialize column number map for map_variable_attnos().	We need this
	 * since dropped columns in the source table aren't copied, so the new
	 * table can have different column numbers.
	 */
	attmap = (AttrNumber *) palloc0(sizeof(AttrNumber) * tupleDesc->natts);

	/*
	 * Insert the copied attributes into the cxt for the new table definition.
	 */
	for (parent_attno = 1; parent_attno <= tupleDesc->natts;
		 parent_attno++)
	{
		Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
		char	   *attributeName = NameStr(attribute->attname);
		ColumnDef  *def;

		/*
		 * Ignore dropped columns in the parent.  attmap entry is left zero.
		 */
		if (attribute->attisdropped)
			continue;

		/*
		 * Create a new column, which is marked as NOT inherited.
		 *
		 * For constraints, ONLY the NOT NULL constraint is inherited by the
		 * new column definition per SQL99.
		 */
		def = makeNode(ColumnDef);
		def->colname = pstrdup(attributeName);
		def->typeName = makeTypeNameFromOid(attribute->atttypid,
											attribute->atttypmod);
		def->inhcount = 0;
		def->is_local = true;
		def->is_not_null = attribute->attnotnull;
		def->is_from_type = false;
		def->storage = 0;
		def->raw_default = NULL;
		def->cooked_default = NULL;
		def->collClause = NULL;
		def->collOid = attribute->attcollation;
		def->constraints = NIL;

		/*
		 * Add to column list
		 */
		cxt->columns = lappend(cxt->columns, def);

		attmap[parent_attno - 1] = list_length(cxt->columns);

		/*
		 * Copy default, if present and the default has been requested
		 */
		if (attribute->atthasdef &&
			(table_like_clause->options & CREATE_TABLE_LIKE_DEFAULTS))
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
			 * If default expr could contain any vars, we'd need to fix 'em,
			 * but it can't; so default is ready to apply to child.
			 */

			def->cooked_default = this_default;
		}

		/* Likewise, copy storage if requested */
		if (table_like_clause->options & CREATE_TABLE_LIKE_STORAGE)
			def->storage = attribute->attstorage;
		else
			def->storage = 0;

		/* Likewise, copy comment if requested */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
			(comment = GetComment(attribute->attrelid,
								  RelationRelationId,
								  attribute->attnum)) != NULL)
		{
			CommentStmt *stmt = makeNode(CommentStmt);

			stmt->objtype = OBJECT_COLUMN;
			stmt->objname = list_make3(makeString(cxt->relation->schemaname),
									   makeString(cxt->relation->relname),
									   makeString(def->colname));
			stmt->objargs = NIL;
			stmt->comment = comment;

			cxt->alist = lappend(cxt->alist, stmt);
		}
	}

	/*
	 * Copy CHECK constraints if requested, being careful to adjust attribute
	 * numbers so they match the child.
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_CONSTRAINTS) &&
		tupleDesc->constr)
	{
		int			ccnum;

		for (ccnum = 0; ccnum < tupleDesc->constr->num_check; ccnum++)
		{
			char	   *ccname = tupleDesc->constr->check[ccnum].ccname;
			char	   *ccbin = tupleDesc->constr->check[ccnum].ccbin;
			Constraint *n = makeNode(Constraint);
			Node	   *ccbin_node;
			bool		found_whole_row;

			ccbin_node = map_variable_attnos(stringToNode(ccbin),
											 1, 0,
											 attmap, tupleDesc->natts,
											 &found_whole_row);

			/*
			 * We reject whole-row variables because the whole point of LIKE
			 * is that the new table's rowtype might later diverge from the
			 * parent's.  So, while translation might be possible right now,
			 * it wouldn't be possible to guarantee it would work in future.
			 */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
								   ccname,
								   RelationGetRelationName(relation))));

			n->contype = CONSTR_CHECK;
			n->location = -1;
			n->conname = pstrdup(ccname);
			n->raw_expr = NULL;
			n->cooked_expr = nodeToString(ccbin_node);
			cxt->ckconstraints = lappend(cxt->ckconstraints, n);

			/* Copy comment on constraint */
			if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
				(comment = GetComment(get_relation_constraint_oid(RelationGetRelid(relation),
														  n->conname, false),
									  ConstraintRelationId,
									  0)) != NULL)
			{
				CommentStmt *stmt = makeNode(CommentStmt);

				stmt->objtype = OBJECT_CONSTRAINT;
				stmt->objname = list_make3(makeString(cxt->relation->schemaname),
										   makeString(cxt->relation->relname),
										   makeString(n->conname));
				stmt->objargs = NIL;
				stmt->comment = comment;

				cxt->alist = lappend(cxt->alist, stmt);
			}
		}
	}

	/*
	 * Likewise, copy indexes if requested
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_INDEXES) &&
		relation->rd_rel->relhasindex)
	{
		List	   *parent_indexes;
		ListCell   *l;

		parent_indexes = RelationGetIndexList(relation);

		foreach(l, parent_indexes)
		{
			Oid			parent_index_oid = lfirst_oid(l);
			Relation	parent_index;
			IndexStmt  *index_stmt;

			parent_index = index_open(parent_index_oid, AccessShareLock);

			/* Build CREATE INDEX statement to recreate the parent_index */
			index_stmt = generateClonedIndexStmt(cxt, parent_index,
												 attmap, tupleDesc->natts);

			/* Copy comment on index, if requested */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_index_oid, RelationRelationId, 0);

				/*
				 * We make use of IndexStmt's idxcomment option, so as not to
				 * need to know now what name the index will have.
				 */
				index_stmt->idxcomment = comment;
			}

			/* Save it in the inh_indexes list for the time being */
			cxt->inh_indexes = lappend(cxt->inh_indexes, index_stmt);

			index_close(parent_index, AccessShareLock);
		}
	}

	/*
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.	That will prevent someone else from deleting or ALTERing the
	 * parent before the child is committed.
	 */
	heap_close(relation, NoLock);
}

static void
transformOfType(CreateStmtContext *cxt, TypeName *ofTypename)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			i;
	Oid			ofTypeId;

	AssertArg(ofTypename);

	tuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(tuple);
	ofTypeId = HeapTupleGetOid(tuple);
	ofTypename->typeOid = ofTypeId;		/* cached for later */

	tupdesc = lookup_rowtype_tupdesc(ofTypeId, -1);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		ColumnDef  *n;

		if (attr->attisdropped)
			continue;

		n = makeNode(ColumnDef);
		n->colname = pstrdup(NameStr(attr->attname));
		n->typeName = makeTypeNameFromOid(attr->atttypid, attr->atttypmod);
		n->inhcount = 0;
		n->is_local = true;
		n->is_not_null = false;
		n->is_from_type = true;
		n->storage = 0;
		n->raw_default = NULL;
		n->cooked_default = NULL;
		n->collClause = NULL;
		n->collOid = attr->attcollation;
		n->constraints = NIL;
		cxt->columns = lappend(cxt->columns, n);
	}
	DecrTupleDescRefCount(tupdesc);

	ReleaseSysCache(tuple);
}

/*
 * Generate an IndexStmt node using information from an already existing index
 * "source_idx".  Attribute numbers should be adjusted according to attmap.
 */
static IndexStmt *
generateClonedIndexStmt(CreateStmtContext *cxt, Relation source_idx,
						const AttrNumber *attmap, int attmap_length)
{
	Oid			source_relid = RelationGetRelid(source_idx);
	Form_pg_attribute *attrs = RelationGetDescr(source_idx)->attrs;
	HeapTuple	ht_idxrel;
	HeapTuple	ht_idx;
	Form_pg_class idxrelrec;
	Form_pg_index idxrec;
	Form_pg_am	amrec;
	oidvector  *indcollation;
	oidvector  *indclass;
	IndexStmt  *index;
	List	   *indexprs;
	ListCell   *indexpr_item;
	Oid			indrelid;
	int			keyno;
	Oid			keycoltype;
	Datum		datum;
	bool		isnull;

	/*
	 * Fetch pg_class tuple of source index.  We can't use the copy in the
	 * relcache entry because it doesn't include optional fields.
	 */
	ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(source_relid));
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", source_relid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* Fetch pg_index tuple for source index from relcache entry */
	ht_idx = source_idx->rd_indextuple;
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);
	indrelid = idxrec->indrelid;

	/* Fetch pg_am tuple for source index from relcache entry */
	amrec = source_idx->rd_am;

	/* Extract indcollation from the pg_index tuple */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indcollation, &isnull);
	Assert(!isnull);
	indcollation = (oidvector *) DatumGetPointer(datum);

	/* Extract indclass from the pg_index tuple */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(datum);

	/* Begin building the IndexStmt */
	index = makeNode(IndexStmt);
	index->relation = cxt->relation;
	index->accessMethod = pstrdup(NameStr(amrec->amname));
	if (OidIsValid(idxrelrec->reltablespace))
		index->tableSpace = get_tablespace_name(idxrelrec->reltablespace);
	else
		index->tableSpace = NULL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNode = InvalidOid;
	index->unique = idxrec->indisunique;
	index->primary = idxrec->indisprimary;
	index->concurrent = false;

	/*
	 * We don't try to preserve the name of the source index; instead, just
	 * let DefineIndex() choose a reasonable name.
	 */
	index->idxname = NULL;

	/*
	 * If the index is marked PRIMARY or has an exclusion condition, it's
	 * certainly from a constraint; else, if it's not marked UNIQUE, it
	 * certainly isn't.  If it is or might be from a constraint, we have to
	 * fetch the pg_constraint record.
	 */
	if (index->primary || index->unique || idxrec->indisexclusion)
	{
		Oid			constraintId = get_index_constraint(source_relid);

		if (OidIsValid(constraintId))
		{
			HeapTuple	ht_constr;
			Form_pg_constraint conrec;

			ht_constr = SearchSysCache1(CONSTROID,
										ObjectIdGetDatum(constraintId));
			if (!HeapTupleIsValid(ht_constr))
				elog(ERROR, "cache lookup failed for constraint %u",
					 constraintId);
			conrec = (Form_pg_constraint) GETSTRUCT(ht_constr);

			index->isconstraint = true;
			index->deferrable = conrec->condeferrable;
			index->initdeferred = conrec->condeferred;

			/* If it's an exclusion constraint, we need the operator names */
			if (idxrec->indisexclusion)
			{
				Datum	   *elems;
				int			nElems;
				int			i;

				Assert(conrec->contype == CONSTRAINT_EXCLUSION);
				/* Extract operator OIDs from the pg_constraint tuple */
				datum = SysCacheGetAttr(CONSTROID, ht_constr,
										Anum_pg_constraint_conexclop,
										&isnull);
				if (isnull)
					elog(ERROR, "null conexclop for constraint %u",
						 constraintId);

				deconstruct_array(DatumGetArrayTypeP(datum),
								  OIDOID, sizeof(Oid), true, 'i',
								  &elems, NULL, &nElems);

				for (i = 0; i < nElems; i++)
				{
					Oid			operid = DatumGetObjectId(elems[i]);
					HeapTuple	opertup;
					Form_pg_operator operform;
					char	   *oprname;
					char	   *nspname;
					List	   *namelist;

					opertup = SearchSysCache1(OPEROID,
											  ObjectIdGetDatum(operid));
					if (!HeapTupleIsValid(opertup))
						elog(ERROR, "cache lookup failed for operator %u",
							 operid);
					operform = (Form_pg_operator) GETSTRUCT(opertup);
					oprname = pstrdup(NameStr(operform->oprname));
					/* For simplicity we always schema-qualify the op name */
					nspname = get_namespace_name(operform->oprnamespace);
					namelist = list_make2(makeString(nspname),
										  makeString(oprname));
					index->excludeOpNames = lappend(index->excludeOpNames,
													namelist);
					ReleaseSysCache(opertup);
				}
			}

			ReleaseSysCache(ht_constr);
		}
		else
			index->isconstraint = false;
	}
	else
		index->isconstraint = false;

	/* Get the index expressions, if any */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indexprs, &isnull);
	if (!isnull)
	{
		char	   *exprsString;

		exprsString = TextDatumGetCString(datum);
		indexprs = (List *) stringToNode(exprsString);
	}
	else
		indexprs = NIL;

	/* Build the list of IndexElem */
	index->indexParams = NIL;

	indexpr_item = list_head(indexprs);
	for (keyno = 0; keyno < idxrec->indnatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		int16		opt = source_idx->rd_indoption[keyno];

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* Simple index column */
			char	   *attname;

			attname = get_relid_attribute_name(indrelid, attnum);
			keycoltype = get_atttype(indrelid, attnum);

			iparam->name = attname;
			iparam->expr = NULL;
		}
		else
		{
			/* Expressional index */
			Node	   *indexkey;
			bool		found_whole_row;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexpr_item);

			/* Adjust Vars to match new table's column numbering */
			indexkey = map_variable_attnos(indexkey,
										   1, 0,
										   attmap, attmap_length,
										   &found_whole_row);

			/* As in transformTableLikeClause, reject whole-row variables */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Index \"%s\" contains a whole-row table reference.",
								   RelationGetRelationName(source_idx))));

			iparam->name = NULL;
			iparam->expr = indexkey;

			keycoltype = exprType(indexkey);
		}

		/* Copy the original index column name */
		iparam->indexcolname = pstrdup(NameStr(attrs[keyno]->attname));

		/* Add the collation name, if non-default */
		iparam->collation = get_collation(indcollation->values[keyno], keycoltype);

		/* Add the operator class name, if non-default */
		iparam->opclass = get_opclass(indclass->values[keyno], keycoltype);

		iparam->ordering = SORTBY_DEFAULT;
		iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;

		/* Adjust options if necessary */
		if (amrec->amcanorder)
		{
			/*
			 * If it supports sort ordering, copy DESC and NULLS opts. Don't
			 * set non-default settings unnecessarily, though, so as to
			 * improve the chance of recognizing equivalence to constraint
			 * indexes.
			 */
			if (opt & INDOPTION_DESC)
			{
				iparam->ordering = SORTBY_DESC;
				if ((opt & INDOPTION_NULLS_FIRST) == 0)
					iparam->nulls_ordering = SORTBY_NULLS_LAST;
			}
			else
			{
				if (opt & INDOPTION_NULLS_FIRST)
					iparam->nulls_ordering = SORTBY_NULLS_FIRST;
			}
		}

		index->indexParams = lappend(index->indexParams, iparam);
	}

	/* Copy reloptions if any */
	datum = SysCacheGetAttr(RELOID, ht_idxrel,
							Anum_pg_class_reloptions, &isnull);
	if (!isnull)
		index->options = untransformRelOptions(datum);

	/* If it's a partial index, decompile and append the predicate */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indpred, &isnull);
	if (!isnull)
	{
		char	   *pred_str;
		Node	   *pred_tree;
		bool		found_whole_row;

		/* Convert text string to node tree */
		pred_str = TextDatumGetCString(datum);
		pred_tree = (Node *) stringToNode(pred_str);

		/* Adjust Vars to match new table's column numbering */
		pred_tree = map_variable_attnos(pred_tree,
										1, 0,
										attmap, attmap_length,
										&found_whole_row);

		/* As in transformTableLikeClause, reject whole-row variables */
		if (found_whole_row)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert whole-row table reference"),
			  errdetail("Index \"%s\" contains a whole-row table reference.",
						RelationGetRelationName(source_idx))));

		index->whereClause = pred_tree;
	}

	/* Clean up */
	ReleaseSysCache(ht_idxrel);

	return index;
}

/*
 * get_collation		- fetch qualified name of a collation
 *
 * If collation is InvalidOid or is the default for the given actual_datatype,
 * then the return value is NIL.
 */
static List *
get_collation(Oid collation, Oid actual_datatype)
{
	List	   *result;
	HeapTuple	ht_coll;
	Form_pg_collation coll_rec;
	char	   *nsp_name;
	char	   *coll_name;

	if (!OidIsValid(collation))
		return NIL;				/* easy case */
	if (collation == get_typcollation(actual_datatype))
		return NIL;				/* just let it default */

	ht_coll = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
	if (!HeapTupleIsValid(ht_coll))
		elog(ERROR, "cache lookup failed for collation %u", collation);
	coll_rec = (Form_pg_collation) GETSTRUCT(ht_coll);

	/* For simplicity, we always schema-qualify the name */
	nsp_name = get_namespace_name(coll_rec->collnamespace);
	coll_name = pstrdup(NameStr(coll_rec->collname));
	result = list_make2(makeString(nsp_name), makeString(coll_name));

	ReleaseSysCache(ht_coll);
	return result;
}

/*
 * get_opclass			- fetch qualified name of an index operator class
 *
 * If the opclass is the default for the given actual_datatype, then
 * the return value is NIL.
 */
static List *
get_opclass(Oid opclass, Oid actual_datatype)
{
	List	   *result = NIL;
	HeapTuple	ht_opc;
	Form_pg_opclass opc_rec;

	ht_opc = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(ht_opc))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	opc_rec = (Form_pg_opclass) GETSTRUCT(ht_opc);

	if (GetDefaultOpClass(actual_datatype, opc_rec->opcmethod) != opclass)
	{
		/* For simplicity, we always schema-qualify the name */
		char	   *nsp_name = get_namespace_name(opc_rec->opcnamespace);
		char	   *opc_name = pstrdup(NameStr(opc_rec->opcname));

		result = list_make2(makeString(nsp_name), makeString(opc_name));
	}

	ReleaseSysCache(ht_opc);
	return result;
}


/*
 * transformIndexConstraints
 *		Handle UNIQUE, PRIMARY KEY, EXCLUDE constraints, which create indexes.
 *		We also merge in any index definitions arising from
 *		LIKE ... INCLUDING INDEXES.
 */
static void
transformIndexConstraints(CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *indexlist = NIL;
	ListCell   *lc;

	/*
	 * Run through the constraints that need to generate an index. For PRIMARY
	 * KEY, mark each column as NOT NULL and create an index. For UNIQUE or
	 * EXCLUDE, create an index as for PRIMARY KEY, but do not insist on NOT
	 * NULL.
	 */
	foreach(lc, cxt->ixconstraints)
	{
		Constraint *constraint = (Constraint *) lfirst(lc);

		Assert(IsA(constraint, Constraint));
		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE ||
			   constraint->contype == CONSTR_EXCLUSION);

		index = transformIndexConstraint(constraint, cxt);

		indexlist = lappend(indexlist, index);
	}

	/* Add in any indexes defined by LIKE ... INCLUDING INDEXES */
	foreach(lc, cxt->inh_indexes)
	{
		index = (IndexStmt *) lfirst(lc);

		if (index->primary)
		{
			if (cxt->pkey != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("multiple primary keys for table \"%s\" are not allowed",
								cxt->relation->relname)));
			cxt->pkey = index;
		}

		indexlist = lappend(indexlist, index);
	}

	/*
	 * Scan the index list and remove any redundant index specifications. This
	 * can happen if, for instance, the user writes UNIQUE PRIMARY KEY. A
	 * strict reading of SQL would suggest raising an error instead, but that
	 * strikes me as too anal-retentive. - tgl 2001-02-14
	 *
	 * XXX in ALTER TABLE case, it'd be nice to look for duplicate
	 * pre-existing indexes, too.
	 */
	Assert(cxt->alist == NIL);
	if (cxt->pkey != NULL)
	{
		/* Make sure we keep the PKEY index in preference to others... */
		cxt->alist = list_make1(cxt->pkey);
	}

	foreach(lc, indexlist)
	{
		bool		keep = true;
		ListCell   *k;

		index = lfirst(lc);

		/* if it's pkey, it's already in cxt->alist */
		if (index == cxt->pkey)
			continue;

		foreach(k, cxt->alist)
		{
			IndexStmt  *priorindex = lfirst(k);

			if (equal(index->indexParams, priorindex->indexParams) &&
				equal(index->whereClause, priorindex->whereClause) &&
				equal(index->excludeOpNames, priorindex->excludeOpNames) &&
				strcmp(index->accessMethod, priorindex->accessMethod) == 0 &&
				index->deferrable == priorindex->deferrable &&
				index->initdeferred == priorindex->initdeferred)
			{
				priorindex->unique |= index->unique;

				/*
				 * If the prior index is as yet unnamed, and this one is
				 * named, then transfer the name to the prior index. This
				 * ensures that if we have named and unnamed constraints,
				 * we'll use (at least one of) the names for the index.
				 */
				if (priorindex->idxname == NULL)
					priorindex->idxname = index->idxname;
				keep = false;
				break;
			}
		}

		if (keep)
			cxt->alist = lappend(cxt->alist, index);
	}
}

/*
 * transformIndexConstraint
 *		Transform one UNIQUE, PRIMARY KEY, or EXCLUDE constraint for
 *		transformIndexConstraints.
 */
static IndexStmt *
transformIndexConstraint(Constraint *constraint, CreateStmtContext *cxt)
{
	IndexStmt  *index;
	ListCell   *lc;

	index = makeNode(IndexStmt);

	index->unique = (constraint->contype != CONSTR_EXCLUSION);
	index->primary = (constraint->contype == CONSTR_PRIMARY);
	if (index->primary)
	{
		if (cxt->pkey != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
			 errmsg("multiple primary keys for table \"%s\" are not allowed",
					cxt->relation->relname),
					 parser_errposition(cxt->pstate, constraint->location)));
		cxt->pkey = index;

		/*
		 * In ALTER TABLE case, a primary index might already exist, but
		 * DefineIndex will check for it.
		 */
	}
	index->isconstraint = true;
	index->deferrable = constraint->deferrable;
	index->initdeferred = constraint->initdeferred;

	if (constraint->conname != NULL)
		index->idxname = pstrdup(constraint->conname);
	else
		index->idxname = NULL;	/* DefineIndex will choose name */

	index->relation = cxt->relation;
	index->accessMethod = constraint->access_method ? constraint->access_method : DEFAULT_INDEX_TYPE;
	index->options = constraint->options;
	index->tableSpace = constraint->indexspace;
	index->whereClause = constraint->where_clause;
	index->indexParams = NIL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNode = InvalidOid;
	index->concurrent = false;

	/*
	 * If it's ALTER TABLE ADD CONSTRAINT USING INDEX, look up the index and
	 * verify it's usable, then extract the implied column name list.  (We
	 * will not actually need the column name list at runtime, but we need it
	 * now to check for duplicate column entries below.)
	 */
	if (constraint->indexname != NULL)
	{
		char	   *index_name = constraint->indexname;
		Relation	heap_rel = cxt->rel;
		Oid			index_oid;
		Relation	index_rel;
		Form_pg_index index_form;
		oidvector  *indclass;
		Datum		indclassDatum;
		bool		isnull;
		int			i;

		/* Grammar should not allow this with explicit column list */
		Assert(constraint->keys == NIL);

		/* Grammar should only allow PRIMARY and UNIQUE constraints */
		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE);

		/* Must be ALTER, not CREATE, but grammar doesn't enforce that */
		if (!cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot use an existing index in CREATE TABLE"),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Look for the index in the same schema as the table */
		index_oid = get_relname_relid(index_name, RelationGetNamespace(heap_rel));

		if (!OidIsValid(index_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("index \"%s\" does not exist", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Open the index (this will throw an error if it is not an index) */
		index_rel = index_open(index_oid, AccessShareLock);
		index_form = index_rel->rd_index;

		/* Check that it does not have an associated constraint already */
		if (OidIsValid(get_index_constraint(index_oid)))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			   errmsg("index \"%s\" is already associated with a constraint",
					  index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Perform validity checks on the index */
		if (index_form->indrelid != RelationGetRelid(heap_rel))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" does not belong to table \"%s\"",
							index_name, RelationGetRelationName(heap_rel)),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (!IndexIsValid(index_form))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" is not valid", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (!index_form->indisunique)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a unique index", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (RelationGetIndexExpressions(index_rel) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" contains expressions", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (RelationGetIndexPredicate(index_rel) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a partial index", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		/*
		 * It's probably unsafe to change a deferred index to non-deferred. (A
		 * non-constraint index couldn't be deferred anyway, so this case
		 * should never occur; no need to sweat, but let's check it.)
		 */
		if (!index_form->indimmediate && !constraint->deferrable)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a deferrable index", index_name),
					 errdetail("Cannot create a non-deferrable constraint using a deferrable index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		/*
		 * Insist on it being a btree.	That's the only kind that supports
		 * uniqueness at the moment anyway; but we must have an index that
		 * exactly matches what you'd get from plain ADD CONSTRAINT syntax,
		 * else dump and reload will produce a different index (breaking
		 * pg_upgrade in particular).
		 */
		if (index_rel->rd_rel->relam != get_am_oid(DEFAULT_INDEX_TYPE, false))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a btree", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Must get indclass the hard way */
		indclassDatum = SysCacheGetAttr(INDEXRELID, index_rel->rd_indextuple,
										Anum_pg_index_indclass, &isnull);
		Assert(!isnull);
		indclass = (oidvector *) DatumGetPointer(indclassDatum);

		for (i = 0; i < index_form->indnatts; i++)
		{
			int16		attnum = index_form->indkey.values[i];
			Form_pg_attribute attform;
			char	   *attname;
			Oid			defopclass;

			/*
			 * We shouldn't see attnum == 0 here, since we already rejected
			 * expression indexes.	If we do, SystemAttributeDefinition will
			 * throw an error.
			 */
			if (attnum > 0)
			{
				Assert(attnum <= heap_rel->rd_att->natts);
				attform = heap_rel->rd_att->attrs[attnum - 1];
			}
			else
				attform = SystemAttributeDefinition(attnum,
											   heap_rel->rd_rel->relhasoids);
			attname = pstrdup(NameStr(attform->attname));

			/*
			 * Insist on default opclass and sort options.	While the index
			 * would still work as a constraint with non-default settings, it
			 * might not provide exactly the same uniqueness semantics as
			 * you'd get from a normally-created constraint; and there's also
			 * the dump/reload problem mentioned above.
			 */
			defopclass = GetDefaultOpClass(attform->atttypid,
										   index_rel->rd_rel->relam);
			if (indclass->values[i] != defopclass ||
				index_rel->rd_indoption[i] != 0)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("index \"%s\" does not have default sorting behavior", index_name),
						 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

			constraint->keys = lappend(constraint->keys, makeString(attname));
		}

		/* Close the index relation but keep the lock */
		relation_close(index_rel, NoLock);

		index->indexOid = index_oid;
	}

	/*
	 * If it's an EXCLUDE constraint, the grammar returns a list of pairs of
	 * IndexElems and operator names.  We have to break that apart into
	 * separate lists.
	 */
	if (constraint->contype == CONSTR_EXCLUSION)
	{
		foreach(lc, constraint->exclusions)
		{
			List	   *pair = (List *) lfirst(lc);
			IndexElem  *elem;
			List	   *opname;

			Assert(list_length(pair) == 2);
			elem = (IndexElem *) linitial(pair);
			Assert(IsA(elem, IndexElem));
			opname = (List *) lsecond(pair);
			Assert(IsA(opname, List));

			index->indexParams = lappend(index->indexParams, elem);
			index->excludeOpNames = lappend(index->excludeOpNames, opname);
		}

		return index;
	}

	/*
	 * For UNIQUE and PRIMARY KEY, we just have a list of column names.
	 *
	 * Make sure referenced keys exist.  If we are making a PRIMARY KEY index,
	 * also make sure they are NOT NULL, if possible. (Although we could leave
	 * it to DefineIndex to mark the columns NOT NULL, it's more efficient to
	 * get it right the first time.)
	 */
	foreach(lc, constraint->keys)
	{
		char	   *key = strVal(lfirst(lc));
		bool		found = false;
		ColumnDef  *column = NULL;
		ListCell   *columns;
		IndexElem  *iparam;

		foreach(columns, cxt->columns)
		{
			column = (ColumnDef *) lfirst(columns);
			Assert(IsA(column, ColumnDef));
			if (strcmp(column->colname, key) == 0)
			{
				found = true;
				break;
			}
		}
		if (found)
		{
			/* found column in the new table; force it to be NOT NULL */
			if (constraint->contype == CONSTR_PRIMARY)
				column->is_not_null = TRUE;
		}
		else if (SystemAttributeByName(key, cxt->hasoids) != NULL)
		{
			/*
			 * column will be a system column in the new table, so accept it.
			 * System columns can't ever be null, so no need to worry about
			 * PRIMARY/NOT NULL constraint.
			 */
			found = true;
		}
		else if (cxt->inhRelations)
		{
			/* try inherited tables */
			ListCell   *inher;

			foreach(inher, cxt->inhRelations)
			{
				RangeVar   *inh = (RangeVar *) lfirst(inher);
				Relation	rel;
				int			count;

				Assert(IsA(inh, RangeVar));
				rel = heap_openrv(inh, AccessShareLock);
				if (rel->rd_rel->relkind != RELKIND_RELATION)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						   errmsg("inherited relation \"%s\" is not a table",
								  inh->relname)));
				for (count = 0; count < rel->rd_att->natts; count++)
				{
					Form_pg_attribute inhattr = rel->rd_att->attrs[count];
					char	   *inhname = NameStr(inhattr->attname);

					if (inhattr->attisdropped)
						continue;
					if (strcmp(key, inhname) == 0)
					{
						found = true;

						/*
						 * We currently have no easy way to force an inherited
						 * column to be NOT NULL at creation, if its parent
						 * wasn't so already. We leave it to DefineIndex to
						 * fix things up in this case.
						 */
						break;
					}
				}
				heap_close(rel, NoLock);
				if (found)
					break;
			}
		}

		/*
		 * In the ALTER TABLE case, don't complain about index keys not
		 * created in the command; they may well exist already. DefineIndex
		 * will complain about them if not, and will also take care of marking
		 * them NOT NULL.
		 */
		if (!found && !cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" named in key does not exist", key),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Check for PRIMARY KEY(foo, foo) */
		foreach(columns, index->indexParams)
		{
			iparam = (IndexElem *) lfirst(columns);
			if (iparam->name && strcmp(key, iparam->name) == 0)
			{
				if (index->primary)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" appears twice in primary key constraint",
									key),
					 parser_errposition(cxt->pstate, constraint->location)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
					errmsg("column \"%s\" appears twice in unique constraint",
						   key),
					 parser_errposition(cxt->pstate, constraint->location)));
			}
		}

		/* OK, add it to the index definition */
		iparam = makeNode(IndexElem);
		iparam->name = pstrdup(key);
		iparam->expr = NULL;
		iparam->indexcolname = NULL;
		iparam->collation = NIL;
		iparam->opclass = NIL;
		iparam->ordering = SORTBY_DEFAULT;
		iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
		index->indexParams = lappend(index->indexParams, iparam);
	}

	return index;
}

/*
 * transformFKConstraints
 *		handle FOREIGN KEY constraints
 */
static void
transformFKConstraints(CreateStmtContext *cxt,
					   bool skipValidation, bool isAddConstraint)
{
	ListCell   *fkclist;

	if (cxt->fkconstraints == NIL)
		return;

	/*
	 * If CREATE TABLE or adding a column with NULL default, we can safely
	 * skip validation of FK constraints, and nonetheless mark them valid.
	 * (This will override any user-supplied NOT VALID flag.)
	 */
	if (skipValidation)
	{
		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);

			constraint->skip_validation = true;
			constraint->initially_valid = true;
		}
	}

	/*
	 * For CREATE TABLE or ALTER TABLE ADD COLUMN, gin up an ALTER TABLE ADD
	 * CONSTRAINT command to execute after the basic command is complete. (If
	 * called from ADD CONSTRAINT, that routine will add the FK constraints to
	 * its own subcommand list.)
	 *
	 * Note: the ADD CONSTRAINT command must also execute after any index
	 * creation commands.  Thus, this should run after
	 * transformIndexConstraints, so that the CREATE INDEX commands are
	 * already in cxt->alist.
	 */
	if (!isAddConstraint)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = cxt->relation;
		alterstmt->cmds = NIL;
		alterstmt->relkind = OBJECT_TABLE;

		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);
			AlterTableCmd *altercmd = makeNode(AlterTableCmd);

			altercmd->subtype = AT_ProcessedConstraint;
			altercmd->name = NULL;
			altercmd->def = (Node *) constraint;
			alterstmt->cmds = lappend(alterstmt->cmds, altercmd);
		}

		cxt->alist = lappend(cxt->alist, alterstmt);
	}
}

/*
 * transformIndexStmt - parse analysis for CREATE INDEX and ALTER TABLE
 *
 * Note: this is a no-op for an index not using either index expressions or
 * a predicate expression.	There are several code paths that create indexes
 * without bothering to call this, because they know they don't have any
 * such expressions to deal with.
 */
IndexStmt *
transformIndexStmt(IndexStmt *stmt, const char *queryString)
{
	Relation	rel;
	ParseState *pstate;
	RangeTblEntry *rte;
	ListCell   *l;

	/*
	 * We must not scribble on the passed-in IndexStmt, so copy it.  (This is
	 * overkill, but easy.)
	 */
	stmt = (IndexStmt *) copyObject(stmt);

	/*
	 * Open the parent table with appropriate locking.	We must do this
	 * because addRangeTableEntry() would acquire only AccessShareLock,
	 * leaving DefineIndex() needing to do a lock upgrade with consequent risk
	 * of deadlock.  Make sure this stays in sync with the type of lock
	 * DefineIndex() wants. If we are being called by ALTER TABLE, we will
	 * already hold a higher lock.
	 */
	rel = heap_openrv(stmt->relation,
				  (stmt->concurrent ? ShareUpdateExclusiveLock : ShareLock));

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Put the parent table into the rtable so that the expressions can refer
	 * to its fields without qualification.
	 */
	rte = addRangeTableEntry(pstate, stmt->relation, NULL, false, true);

	/* no to join list, yes to namespaces */
	addRTEtoQuery(pstate, rte, false, true, true);

	/* take care of the where clause */
	if (stmt->whereClause)
	{
		stmt->whereClause = transformWhereClause(pstate,
												 stmt->whereClause,
												 EXPR_KIND_INDEX_PREDICATE,
												 "WHERE");
		/* we have to fix its collations too */
		assign_expr_collations(pstate, stmt->whereClause);
	}

	/* take care of any index expressions */
	foreach(l, stmt->indexParams)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(l);

		if (ielem->expr)
		{
			/* Extract preliminary index col name before transforming expr */
			if (ielem->indexcolname == NULL)
				ielem->indexcolname = FigureIndexColname(ielem->expr);

			/* Now do parse transformation of the expression */
			ielem->expr = transformExpr(pstate, ielem->expr,
										EXPR_KIND_INDEX_EXPRESSION);

			/* We have to fix its collations too */
			assign_expr_collations(pstate, ielem->expr);

			/*
			 * transformExpr() should have already rejected subqueries,
			 * aggregates, and window functions, based on the EXPR_KIND_ for
			 * an index expression.
			 *
			 * Also reject expressions returning sets; this is for consistency
			 * with what transformWhereClause() checks for the predicate.
			 * DefineIndex() will make more checks.
			 */
			if (expression_returns_set(ielem->expr))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("index expression cannot return a set")));
		}
	}

	/*
	 * Check that only the base rel is mentioned.  (This should be dead code
	 * now that add_missing_from is history.)
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("index expressions and predicates can refer only to the table being indexed")));

	free_parsestate(pstate);

	/* Close relation, but keep the lock */
	heap_close(rel, NoLock);

	return stmt;
}


/*
 * transformRuleStmt -
 *	  transform a CREATE RULE Statement. The action is a list of parse
 *	  trees which is transformed into a list of query trees, and we also
 *	  transform the WHERE clause if any.
 *
 * actions and whereClause are output parameters that receive the
 * transformed results.
 *
 * Note that we must not scribble on the passed-in RuleStmt, so we do
 * copyObject() on the actions and WHERE clause.
 */
void
transformRuleStmt(RuleStmt *stmt, const char *queryString,
				  List **actions, Node **whereClause)
{
	Relation	rel;
	ParseState *pstate;
	RangeTblEntry *oldrte;
	RangeTblEntry *newrte;

	/*
	 * To avoid deadlock, make sure the first thing we do is grab
	 * AccessExclusiveLock on the target relation.	This will be needed by
	 * DefineQueryRewrite(), and we don't want to grab a lesser lock
	 * beforehand.
	 */
	rel = heap_openrv(stmt->relation, AccessExclusiveLock);

	if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rules on materialized views are not supported")));

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * NOTE: 'OLD' must always have a varno equal to 1 and 'NEW' equal to 2.
	 * Set up their RTEs in the main pstate for use in parsing the rule
	 * qualification.
	 */
	oldrte = addRangeTableEntryForRelation(pstate, rel,
										   makeAlias("old", NIL),
										   false, false);
	newrte = addRangeTableEntryForRelation(pstate, rel,
										   makeAlias("new", NIL),
										   false, false);
	/* Must override addRangeTableEntry's default access-check flags */
	oldrte->requiredPerms = 0;
	newrte->requiredPerms = 0;

	/*
	 * They must be in the namespace too for lookup purposes, but only add the
	 * one(s) that are relevant for the current kind of rule.  In an UPDATE
	 * rule, quals must refer to OLD.field or NEW.field to be unambiguous, but
	 * there's no need to be so picky for INSERT & DELETE.  We do not add them
	 * to the joinlist.
	 */
	switch (stmt->event)
	{
		case CMD_SELECT:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			break;
		case CMD_UPDATE:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			addRTEtoQuery(pstate, newrte, false, true, true);
			break;
		case CMD_INSERT:
			addRTEtoQuery(pstate, newrte, false, true, true);
			break;
		case CMD_DELETE:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			break;
		default:
			elog(ERROR, "unrecognized event type: %d",
				 (int) stmt->event);
			break;
	}

	/* take care of the where clause */
	*whereClause = transformWhereClause(pstate,
									  (Node *) copyObject(stmt->whereClause),
										EXPR_KIND_WHERE,
										"WHERE");
	/* we have to fix its collations too */
	assign_expr_collations(pstate, *whereClause);

	/* this is probably dead code without add_missing_from: */
	if (list_length(pstate->p_rtable) != 2)		/* naughty, naughty... */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("rule WHERE condition cannot contain references to other relations")));

	/*
	 * 'instead nothing' rules with a qualification need a query rangetable so
	 * the rewrite handler can add the negated rule qualification to the
	 * original query. We create a query with the new command type CMD_NOTHING
	 * here that is treated specially by the rewrite system.
	 */
	if (stmt->actions == NIL)
	{
		Query	   *nothing_qry = makeNode(Query);

		nothing_qry->commandType = CMD_NOTHING;
		nothing_qry->rtable = pstate->p_rtable;
		nothing_qry->jointree = makeFromExpr(NIL, NULL);		/* no join wanted */

		*actions = list_make1(nothing_qry);
	}
	else
	{
		ListCell   *l;
		List	   *newactions = NIL;

		/*
		 * transform each statement, like parse_sub_analyze()
		 */
		foreach(l, stmt->actions)
		{
			Node	   *action = (Node *) lfirst(l);
			ParseState *sub_pstate = make_parsestate(NULL);
			Query	   *sub_qry,
					   *top_subqry;
			bool		has_old,
						has_new;

			/*
			 * Since outer ParseState isn't parent of inner, have to pass down
			 * the query text by hand.
			 */
			sub_pstate->p_sourcetext = queryString;

			/*
			 * Set up OLD/NEW in the rtable for this statement.  The entries
			 * are added only to relnamespace, not varnamespace, because we
			 * don't want them to be referred to by unqualified field names
			 * nor "*" in the rule actions.  We decide later whether to put
			 * them in the joinlist.
			 */
			oldrte = addRangeTableEntryForRelation(sub_pstate, rel,
												   makeAlias("old", NIL),
												   false, false);
			newrte = addRangeTableEntryForRelation(sub_pstate, rel,
												   makeAlias("new", NIL),
												   false, false);
			oldrte->requiredPerms = 0;
			newrte->requiredPerms = 0;
			addRTEtoQuery(sub_pstate, oldrte, false, true, false);
			addRTEtoQuery(sub_pstate, newrte, false, true, false);

			/* Transform the rule action statement */
			top_subqry = transformStmt(sub_pstate,
									   (Node *) copyObject(action));

			/*
			 * We cannot support utility-statement actions (eg NOTIFY) with
			 * nonempty rule WHERE conditions, because there's no way to make
			 * the utility action execute conditionally.
			 */
			if (top_subqry->commandType == CMD_UTILITY &&
				*whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("rules with WHERE conditions can only have SELECT, INSERT, UPDATE, or DELETE actions")));

			/*
			 * If the action is INSERT...SELECT, OLD/NEW have been pushed down
			 * into the SELECT, and that's what we need to look at. (Ugly
			 * kluge ... try to fix this when we redesign querytrees.)
			 */
			sub_qry = getInsertSelectQuery(top_subqry, NULL);

			/*
			 * If the sub_qry is a setop, we cannot attach any qualifications
			 * to it, because the planner won't notice them.  This could
			 * perhaps be relaxed someday, but for now, we may as well reject
			 * such a rule immediately.
			 */
			if (sub_qry->setOperations != NULL && *whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			/*
			 * Validate action's use of OLD/NEW, qual too
			 */
			has_old =
				rangeTableEntry_used((Node *) sub_qry, PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_OLD_VARNO, 0);
			has_new =
				rangeTableEntry_used((Node *) sub_qry, PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_NEW_VARNO, 0);

			switch (stmt->event)
			{
				case CMD_SELECT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use OLD")));
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use NEW")));
					break;
				case CMD_UPDATE:
					/* both are OK */
					break;
				case CMD_INSERT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON INSERT rule cannot use OLD")));
					break;
				case CMD_DELETE:
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON DELETE rule cannot use NEW")));
					break;
				default:
					elog(ERROR, "unrecognized event type: %d",
						 (int) stmt->event);
					break;
			}

			/*
			 * OLD/NEW are not allowed in WITH queries, because they would
			 * amount to outer references for the WITH, which we disallow.
			 * However, they were already in the outer rangetable when we
			 * analyzed the query, so we have to check.
			 *
			 * Note that in the INSERT...SELECT case, we need to examine the
			 * CTE lists of both top_subqry and sub_qry.
			 *
			 * Note that we aren't digging into the body of the query looking
			 * for WITHs in nested sub-SELECTs.  A WITH down there can
			 * legitimately refer to OLD/NEW, because it'd be an
			 * indirect-correlated outer reference.
			 */
			if (rangeTableEntry_used((Node *) top_subqry->cteList,
									 PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used((Node *) sub_qry->cteList,
									 PRS2_OLD_VARNO, 0))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot refer to OLD within WITH query")));
			if (rangeTableEntry_used((Node *) top_subqry->cteList,
									 PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used((Node *) sub_qry->cteList,
									 PRS2_NEW_VARNO, 0))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot refer to NEW within WITH query")));

			/*
			 * For efficiency's sake, add OLD to the rule action's jointree
			 * only if it was actually referenced in the statement or qual.
			 *
			 * For INSERT, NEW is not really a relation (only a reference to
			 * the to-be-inserted tuple) and should never be added to the
			 * jointree.
			 *
			 * For UPDATE, we treat NEW as being another kind of reference to
			 * OLD, because it represents references to *transformed* tuples
			 * of the existing relation.  It would be wrong to enter NEW
			 * separately in the jointree, since that would cause a double
			 * join of the updated relation.  It's also wrong to fail to make
			 * a jointree entry if only NEW and not OLD is mentioned.
			 */
			if (has_old || (has_new && stmt->event == CMD_UPDATE))
			{
				/*
				 * If sub_qry is a setop, manipulating its jointree will do no
				 * good at all, because the jointree is dummy. (This should be
				 * a can't-happen case because of prior tests.)
				 */
				if (sub_qry->setOperations != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
				/* hack so we can use addRTEtoQuery() */
				sub_pstate->p_rtable = sub_qry->rtable;
				sub_pstate->p_joinlist = sub_qry->jointree->fromlist;
				addRTEtoQuery(sub_pstate, oldrte, true, false, false);
				sub_qry->jointree->fromlist = sub_pstate->p_joinlist;
			}

			newactions = lappend(newactions, top_subqry);

			free_parsestate(sub_pstate);
		}

		*actions = newactions;
	}

	free_parsestate(pstate);

	/* Close relation, but keep the exclusive lock */
	heap_close(rel, NoLock);
}


/*
 * transformAlterTableStmt -
 *		parse analysis for ALTER TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed AlterTableStmt, but there may be additional actions
 * to be done before and after the actual AlterTable() call.
 */
List *
transformAlterTableStmt(AlterTableStmt *stmt, const char *queryString)
{
	Relation	rel;
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *lcmd,
			   *l;
	List	   *newcmds = NIL;
	bool		skipValidation = true;
	AlterTableCmd *newcmd;
	LOCKMODE	lockmode;

	/*
	 * We must not scribble on the passed-in AlterTableStmt, so copy it. (This
	 * is overkill, but easy.)
	 */
	stmt = (AlterTableStmt *) copyObject(stmt);

	/*
	 * Determine the appropriate lock level for this list of subcommands.
	 */
	lockmode = AlterTableGetLockLevel(stmt->cmds);

	/*
	 * Acquire appropriate lock on the target relation, which will be held
	 * until end of transaction.  This ensures any decisions we make here
	 * based on the state of the relation will still be good at execution. We
	 * must get lock now because execution will later require it; taking a
	 * lower grade lock now and trying to upgrade later risks deadlock.  Any
	 * new commands we add after this must not upgrade the lock level
	 * requested here.
	 */
	rel = relation_openrv_extended(stmt->relation, lockmode, stmt->missing_ok);
	if (rel == NULL)
	{
		/* this message is consistent with relation_openrv */
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return NIL;
	}

	/* Set up pstate and CreateStmtContext */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	cxt.pstate = pstate;
	if (stmt->relkind == OBJECT_FOREIGN_TABLE)
	{
		cxt.stmtType = "ALTER FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "ALTER TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = rel;
	cxt.inhRelations = NIL;
	cxt.isalter = true;
	cxt.hasoids = false;		/* need not be right */
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.inh_indexes = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;

	/*
	 * The only subtypes that currently require parse transformation handling
	 * are ADD COLUMN and ADD CONSTRAINT.  These largely re-use code from
	 * CREATE TABLE.
	 */
	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		switch (cmd->subtype)
		{
			case AT_AddColumn:
			case AT_AddColumnToView:
				{
					ColumnDef  *def = (ColumnDef *) cmd->def;

					Assert(IsA(def, ColumnDef));
					transformColumnDefinition(&cxt, def);

					/*
					 * If the column has a non-null default, we can't skip
					 * validation of foreign keys.
					 */
					if (def->raw_default != NULL)
						skipValidation = false;

					/*
					 * All constraints are processed in other ways. Remove the
					 * original list
					 */
					def->constraints = NIL;

					newcmds = lappend(newcmds, cmd);
					break;
				}
			case AT_AddConstraint:

				/*
				 * The original AddConstraint cmd node doesn't go to newcmds
				 */
				if (IsA(cmd->def, Constraint))
				{
					transformTableConstraint(&cxt, (Constraint *) cmd->def);
					if (((Constraint *) cmd->def)->contype == CONSTR_FOREIGN)
						skipValidation = false;
				}
				else
					elog(ERROR, "unrecognized node type: %d",
						 (int) nodeTag(cmd->def));
				break;

			case AT_ProcessedConstraint:

				/*
				 * Already-transformed ADD CONSTRAINT, so just make it look
				 * like the standard case.
				 */
				cmd->subtype = AT_AddConstraint;
				newcmds = lappend(newcmds, cmd);
				break;

			default:
				newcmds = lappend(newcmds, cmd);
				break;
		}
	}

	/*
	 * transformIndexConstraints wants cxt.alist to contain only index
	 * statements, so transfer anything we already have into save_alist
	 * immediately.
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	/* Postprocess index and FK constraints */
	transformIndexConstraints(&cxt);

	transformFKConstraints(&cxt, skipValidation, true);

	/*
	 * Push any index-creation commands into the ALTER, so that they can be
	 * scheduled nicely by tablecmds.c.  Note that tablecmds.c assumes that
	 * the IndexStmt attached to an AT_AddIndex or AT_AddIndexConstraint
	 * subcommand has already been through transformIndexStmt.
	 */
	foreach(l, cxt.alist)
	{
		IndexStmt  *idxstmt = (IndexStmt *) lfirst(l);

		Assert(IsA(idxstmt, IndexStmt));
		idxstmt = transformIndexStmt(idxstmt, queryString);
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = OidIsValid(idxstmt->indexOid) ? AT_AddIndexConstraint : AT_AddIndex;
		newcmd->def = (Node *) idxstmt;
		newcmds = lappend(newcmds, newcmd);
	}
	cxt.alist = NIL;

	/* Append any CHECK or FK constraints to the commands list */
	foreach(l, cxt.ckconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}
	foreach(l, cxt.fkconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}

	/* Close rel but keep lock */
	relation_close(rel, NoLock);

	/*
	 * Output results.
	 */
	stmt->cmds = newcmds;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}


/*
 * Preprocess a list of column constraint clauses
 * to attach constraint attributes to their primary constraint nodes
 * and detect inconsistent/misplaced constraint attributes.
 *
 * NOTE: currently, attributes are only supported for FOREIGN KEY, UNIQUE,
 * EXCLUSION, and PRIMARY KEY constraints, but someday they ought to be
 * supported for other constraint types.
 */
static void
transformConstraintAttrs(CreateStmtContext *cxt, List *constraintList)
{
	Constraint *lastprimarycon = NULL;
	bool		saw_deferrability = false;
	bool		saw_initially = false;
	ListCell   *clist;

#define SUPPORTS_ATTRS(node)				\
	((node) != NULL &&						\
	 ((node)->contype == CONSTR_PRIMARY ||	\
	  (node)->contype == CONSTR_UNIQUE ||	\
	  (node)->contype == CONSTR_EXCLUSION || \
	  (node)->contype == CONSTR_FOREIGN))

	foreach(clist, constraintList)
	{
		Constraint *con = (Constraint *) lfirst(clist);

		if (!IsA(con, Constraint))
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(con));
		switch (con->contype)
		{
			case CONSTR_ATTR_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = true;
				break;

			case CONSTR_ATTR_NOT_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced NOT DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = false;
				if (saw_initially &&
					lastprimarycon->initdeferred)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_DEFERRED:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY DEFERRED clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = true;

				/*
				 * If only INITIALLY DEFERRED appears, assume DEFERRABLE
				 */
				if (!saw_deferrability)
					lastprimarycon->deferrable = true;
				else if (!lastprimarycon->deferrable)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_IMMEDIATE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY IMMEDIATE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = false;
				break;

			default:
				/* Otherwise it's not an attribute */
				lastprimarycon = con;
				/* reset flags for new primary node */
				saw_deferrability = false;
				saw_initially = false;
				break;
		}
	}
}

/*
 * Special handling of type definition for a column
 */
static void
transformColumnType(CreateStmtContext *cxt, ColumnDef *column)
{
	/*
	 * All we really need to do here is verify that the type is valid,
	 * including any collation spec that might be present.
	 */
	Type		ctype = typenameType(cxt->pstate, column->typeName, NULL);

	if (column->collClause)
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(ctype);

		LookupCollation(cxt->pstate,
						column->collClause->collname,
						column->collClause->location);
		/* Complain if COLLATE is applied to an uncollatable type */
		if (!OidIsValid(typtup->typcollation))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("collations are not supported by type %s",
							format_type_be(HeapTupleGetOid(ctype))),
					 parser_errposition(cxt->pstate,
										column->collClause->location)));
	}

	ReleaseSysCache(ctype);
}


/*
 * transformCreateSchemaStmt -
 *	  analyzes the CREATE SCHEMA statement
 *
 * Split the schema element list into individual commands and place
 * them in the result list in an order such that there are no forward
 * references (e.g. GRANT to a table created later in the list). Note
 * that the logic we use for determining forward references is
 * presently quite incomplete.
 *
 * SQL also allows constraints to make forward references, so thumb through
 * the table columns and move forward references to a posterior alter-table
 * command.
 *
 * The result is a list of parse nodes that still need to be analyzed ---
 * but we can't analyze the later commands until we've executed the earlier
 * ones, because of possible inter-object references.
 *
 * Note: this breaks the rules a little bit by modifying schema-name fields
 * within passed-in structs.  However, the transformation would be the same
 * if done over, so it should be all right to scribble on the input to this
 * extent.
 */
List *
transformCreateSchemaStmt(CreateSchemaStmt *stmt)
{
	CreateSchemaStmtContext cxt;
	List	   *result;
	ListCell   *elements;

	cxt.stmtType = "CREATE SCHEMA";
	cxt.schemaname = stmt->schemaname;
	cxt.authid = stmt->authid;
	cxt.sequences = NIL;
	cxt.tables = NIL;
	cxt.views = NIL;
	cxt.indexes = NIL;
	cxt.triggers = NIL;
	cxt.grants = NIL;

	/*
	 * Run through each schema element in the schema element list. Separate
	 * statements by type, and do preliminary analysis.
	 */
	foreach(elements, stmt->schemaElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_CreateSeqStmt:
				{
					CreateSeqStmt *elp = (CreateSeqStmt *) element;

					setSchemaName(cxt.schemaname, &elp->sequence->schemaname);
					cxt.sequences = lappend(cxt.sequences, element);
				}
				break;

			case T_CreateStmt:
				{
					CreateStmt *elp = (CreateStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);

					/*
					 * XXX todo: deal with constraints
					 */
					cxt.tables = lappend(cxt.tables, element);
				}
				break;

			case T_ViewStmt:
				{
					ViewStmt   *elp = (ViewStmt *) element;

					setSchemaName(cxt.schemaname, &elp->view->schemaname);

					/*
					 * XXX todo: deal with references between views
					 */
					cxt.views = lappend(cxt.views, element);
				}
				break;

			case T_IndexStmt:
				{
					IndexStmt  *elp = (IndexStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.indexes = lappend(cxt.indexes, element);
				}
				break;

			case T_CreateTrigStmt:
				{
					CreateTrigStmt *elp = (CreateTrigStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.triggers = lappend(cxt.triggers, element);
				}
				break;

			case T_GrantStmt:
				cxt.grants = lappend(cxt.grants, element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
		}
	}

	result = NIL;
	result = list_concat(result, cxt.sequences);
	result = list_concat(result, cxt.tables);
	result = list_concat(result, cxt.views);
	result = list_concat(result, cxt.indexes);
	result = list_concat(result, cxt.triggers);
	result = list_concat(result, cxt.grants);

	return result;
}

/*
 * setSchemaName
 *		Set or check schema name in an element of a CREATE SCHEMA command
 */
static void
setSchemaName(char *context_schema, char **stmt_schema_name)
{
	if (*stmt_schema_name == NULL)
		*stmt_schema_name = context_schema;
	else if (strcmp(context_schema, *stmt_schema_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_SCHEMA_DEFINITION),
				 errmsg("CREATE specifies a schema (%s) "
						"different from the one being created (%s)",
						*stmt_schema_name, context_schema)));
}
