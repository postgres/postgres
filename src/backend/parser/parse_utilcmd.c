/*-------------------------------------------------------------------------
 *
 * parse_utilcmd.c
 *	  Perform parse analysis work for various utility commands
 *
 * Formerly we did this work during parse_analyze_*() in analyze.c.  However
 * that is fairly unsafe in the presence of querytree caching, since any
 * database state that we depend on in making the transformations might be
 * obsolete by the time the utility command is executed; and utility commands
 * have no infrastructure for holding locks or rechecking plan validity.
 * Hence these functions are now called at the start of execution of their
 * respective utility commands.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/parse_utilcmd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/toast_compression.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
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
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
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
	List	   *columns;		/* ColumnDef items */
	List	   *ckconstraints;	/* CHECK constraints */
	List	   *nnconstraints;	/* NOT NULL constraints */
	List	   *fkconstraints;	/* FOREIGN KEY constraints */
	List	   *ixconstraints;	/* index-creating constraints */
	List	   *likeclauses;	/* LIKE clauses that need post-processing */
	List	   *blist;			/* "before list" of things to do before
								 * creating the table */
	List	   *alist;			/* "after list" of things to do after creating
								 * the table */
	IndexStmt  *pkey;			/* PRIMARY KEY index, if any */
	bool		ispartitioned;	/* true if table is partitioned */
	PartitionBoundSpec *partbound;	/* transformed FOR VALUES */
	bool		ofType;			/* true if statement contains OF typename */
} CreateStmtContext;

/* State shared by transformCreateSchemaStmtElements and its subroutines */
typedef struct
{
	const char *schemaname;		/* name of schema */
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
static CreateStatsStmt *generateClonedExtStatsStmt(RangeVar *heapRel,
												   Oid heapRelid,
												   Oid source_statsid,
												   const AttrMap *attmap);
static List *get_collation(Oid collation, Oid actual_datatype);
static List *get_opclass(Oid opclass, Oid actual_datatype);
static void transformIndexConstraints(CreateStmtContext *cxt);
static IndexStmt *transformIndexConstraint(Constraint *constraint,
										   CreateStmtContext *cxt);
static void transformFKConstraints(CreateStmtContext *cxt,
								   bool skipValidation,
								   bool isAddConstraint);
static void transformCheckConstraints(CreateStmtContext *cxt,
									  bool skipValidation);
static void transformConstraintAttrs(CreateStmtContext *cxt,
									 List *constraintList);
static void transformColumnType(CreateStmtContext *cxt, ColumnDef *column);
static void setSchemaName(const char *context_schema, char **stmt_schema_name);
static void transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd);
static List *transformPartitionRangeBounds(ParseState *pstate, List *blist,
										   Relation parent);
static void validateInfiniteBounds(ParseState *pstate, List *blist);
static Const *transformPartitionBoundValue(ParseState *pstate, Node *val,
										   const char *colName, Oid colType, int32 colTypmod,
										   Oid partCollation);


/*
 * transformCreateStmt -
 *	  parse analysis for CREATE TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed CreateStmt, but there may be additional actions
 * to be done before and after the actual DefineRelation() call.
 * In addition to normal utility commands such as AlterTableStmt and
 * IndexStmt, the result list may contain TableLikeClause(s), representing
 * the need to perform additional parse analysis after DefineRelation().
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
	ParseCallbackState pcbstate;

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Look up the creation namespace.  This also checks permissions on the
	 * target namespace, locks it against concurrent drops, checks for a
	 * preexisting relation in that namespace with the same name, and updates
	 * stmt->relation->relpersistence if the selected namespace is temporary.
	 */
	setup_parser_errposition_callback(&pcbstate, pstate,
									  stmt->relation->location);
	namespaceid =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock,
											 &existing_relid);
	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * If the relation already exists and the user specified "IF NOT EXISTS",
	 * bail out with a NOTICE.
	 */
	if (stmt->if_not_exists && OidIsValid(existing_relid))
	{
		/*
		 * If we are in an extension script, insist that the pre-existing
		 * object be a member of the extension, to avoid security risks.
		 */
		ObjectAddress address;

		ObjectAddressSet(address, RelationRelationId, existing_relid);
		checkMembershipInCurrentExtension(&address);

		/* OK to skip */
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
	 * are earlier in the search path.  But a local temp table is effectively
	 * specified to be in pg_temp, so no need for anything extra in that case.
	 */
	if (stmt->relation->schemaname == NULL
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		stmt->relation->schemaname = get_namespace_name(namespaceid);

	/* Set up CreateStmtContext */
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
	cxt.nnconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.likeclauses = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = stmt->partspec != NULL;
	cxt.partbound = stmt->partbound;
	cxt.ofType = (stmt->ofTypename != NULL);

	Assert(!stmt->ofTypename || !stmt->inhRelations);	/* grammar enforces */

	if (stmt->ofTypename)
		transformOfType(&cxt, stmt->ofTypename);

	if (stmt->partspec)
	{
		if (stmt->inhRelations && !stmt->partbound)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot create partitioned table as inheritance child")));
	}

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
	 * Transfer anything we already have in cxt.alist into save_alist, to keep
	 * it separate from the output of transformIndexConstraints.  (This may
	 * not be necessary anymore, but we'll keep doing it to preserve the
	 * historical order of execution of the alist commands.)
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	Assert(stmt->constraints == NIL);

	/*
	 * Before processing index constraints, which could include a primary key,
	 * we must scan all not-null constraints to propagate the is_not_null flag
	 * to each corresponding ColumnDef.  This is necessary because table-level
	 * not-null constraints have not been marked in each ColumnDef, and the PK
	 * processing code needs to know whether one constraint has already been
	 * declared in order not to declare a redundant one.
	 */
	foreach_node(Constraint, nn, cxt.nnconstraints)
	{
		char	   *colname = strVal(linitial(nn->keys));

		foreach_node(ColumnDef, cd, cxt.columns)
		{
			/* not our column? */
			if (strcmp(cd->colname, colname) != 0)
				continue;
			/* Already marked not-null? Nothing to do */
			if (cd->is_not_null)
				break;
			/* Bingo, we're done for this constraint */
			cd->is_not_null = true;
			break;
		}
	}

	/*
	 * Postprocess constraints that give rise to index definitions.
	 */
	transformIndexConstraints(&cxt);

	/*
	 * Re-consideration of LIKE clauses should happen after creation of
	 * indexes, but before creation of foreign keys.  This order is critical
	 * because a LIKE clause may attempt to create a primary key.  If there's
	 * also a pkey in the main CREATE TABLE list, creation of that will not
	 * check for a duplicate at runtime (since index_check_primary_key()
	 * expects that we rejected dups here).  Creation of the LIKE-generated
	 * pkey behaves like ALTER TABLE ADD, so it will check, but obviously that
	 * only works if it happens second.  On the other hand, we want to make
	 * pkeys before foreign key constraints, in case the user tries to make a
	 * self-referential FK.
	 */
	cxt.alist = list_concat(cxt.alist, cxt.likeclauses);

	/*
	 * Postprocess foreign-key constraints.
	 */
	transformFKConstraints(&cxt, true, false);

	/*
	 * Postprocess check constraints.
	 *
	 * For regular tables all constraints can be marked valid immediately,
	 * because the table is new therefore empty. Not so for foreign tables.
	 */
	transformCheckConstraints(&cxt, !cxt.isforeign);

	/*
	 * Output results.
	 */
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;
	stmt->nnconstraints = cxt.nnconstraints;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}

/*
 * generateSerialExtraStmts
 *		Generate CREATE SEQUENCE and ALTER SEQUENCE ... OWNED BY statements
 *		to create the sequence for a serial or identity column.
 *
 * This includes determining the name the sequence will have.  The caller
 * can ask to get back the name components by passing non-null pointers
 * for snamespace_p and sname_p.
 */
static void
generateSerialExtraStmts(CreateStmtContext *cxt, ColumnDef *column,
						 Oid seqtypid, List *seqoptions,
						 bool for_identity, bool col_exists,
						 char **snamespace_p, char **sname_p)
{
	ListCell   *option;
	DefElem    *nameEl = NULL;
	DefElem    *loggedEl = NULL;
	Oid			snamespaceid;
	char	   *snamespace;
	char	   *sname;
	char		seqpersistence;
	CreateSeqStmt *seqstmt;
	AlterSeqStmt *altseqstmt;
	List	   *attnamelist;

	/* Make a copy of this as we may end up modifying it in the code below */
	seqoptions = list_copy(seqoptions);

	/*
	 * Check for non-SQL-standard options (not supported within CREATE
	 * SEQUENCE, because they'd be redundant), and remove them from the
	 * seqoptions list if found.
	 */
	foreach(option, seqoptions)
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "sequence_name") == 0)
		{
			if (nameEl)
				errorConflictingDefElem(defel, cxt->pstate);
			nameEl = defel;
			seqoptions = foreach_delete_current(seqoptions, option);
		}
		else if (strcmp(defel->defname, "logged") == 0 ||
				 strcmp(defel->defname, "unlogged") == 0)
		{
			if (loggedEl)
				errorConflictingDefElem(defel, cxt->pstate);
			loggedEl = defel;
			seqoptions = foreach_delete_current(seqoptions, option);
		}
	}

	/*
	 * Determine namespace and name to use for the sequence.
	 */
	if (nameEl)
	{
		/* Use specified name */
		RangeVar   *rv = makeRangeVarFromNameList(castNode(List, nameEl->arg));

		snamespace = rv->schemaname;
		if (!snamespace)
		{
			/* Given unqualified SEQUENCE NAME, select namespace */
			if (cxt->rel)
				snamespaceid = RelationGetNamespace(cxt->rel);
			else
				snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
			snamespace = get_namespace_name(snamespaceid);
		}
		sname = rv->relname;
	}
	else
	{
		/*
		 * Generate a name.
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
								   snamespaceid,
								   false);
	}

	ereport(DEBUG1,
			(errmsg_internal("%s will create implicit sequence \"%s\" for serial column \"%s.%s\"",
							 cxt->stmtType, sname,
							 cxt->relation->relname, column->colname)));

	/*
	 * Determine the persistence of the sequence.  By default we copy the
	 * persistence of the table, but if LOGGED or UNLOGGED was specified, use
	 * that (as long as the table isn't TEMP).
	 *
	 * For CREATE TABLE, we get the persistence from cxt->relation, which
	 * comes from the CreateStmt in progress.  For ALTER TABLE, the parser
	 * won't set cxt->relation->relpersistence, but we have cxt->rel as the
	 * existing table, so we copy the persistence from there.
	 */
	seqpersistence = cxt->rel ? cxt->rel->rd_rel->relpersistence : cxt->relation->relpersistence;
	if (loggedEl)
	{
		if (seqpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot set logged status of a temporary sequence"),
					 parser_errposition(cxt->pstate, loggedEl->location)));
		else if (strcmp(loggedEl->defname, "logged") == 0)
			seqpersistence = RELPERSISTENCE_PERMANENT;
		else
			seqpersistence = RELPERSISTENCE_UNLOGGED;
	}

	/*
	 * Build a CREATE SEQUENCE command to create the sequence object, and add
	 * it to the list of things to be done before this CREATE/ALTER TABLE.
	 */
	seqstmt = makeNode(CreateSeqStmt);
	seqstmt->for_identity = for_identity;
	seqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	seqstmt->sequence->relpersistence = seqpersistence;
	seqstmt->options = seqoptions;

	/*
	 * If a sequence data type was specified, add it to the options.  Prepend
	 * to the list rather than append; in case a user supplied their own AS
	 * clause, the "redundant options" error will point to their occurrence,
	 * not our synthetic one.
	 */
	if (seqtypid)
		seqstmt->options = lcons(makeDefElem("as",
											 (Node *) makeTypeNameFromOid(seqtypid, -1),
											 -1),
								 seqstmt->options);

	/*
	 * If this is ALTER ADD COLUMN, make sure the sequence will be owned by
	 * the table's owner.  The current user might be someone else (perhaps a
	 * superuser, or someone who's only a member of the owning role), but the
	 * SEQUENCE OWNED BY mechanisms will bleat unless table and sequence have
	 * exactly the same owning role.
	 */
	if (cxt->rel)
		seqstmt->ownerId = cxt->rel->rd_rel->relowner;
	else
		seqstmt->ownerId = InvalidOid;

	cxt->blist = lappend(cxt->blist, seqstmt);

	/*
	 * Store the identity sequence name that we decided on.  ALTER TABLE ...
	 * ADD COLUMN ... IDENTITY needs this so that it can fill the new column
	 * with values from the sequence, while the association of the sequence
	 * with the table is not set until after the ALTER TABLE.
	 */
	column->identitySequence = seqstmt->sequence;

	/*
	 * Build an ALTER SEQUENCE ... OWNED BY command to mark the sequence as
	 * owned by this column, and add it to the appropriate list of things to
	 * be done along with this CREATE/ALTER TABLE.  In a CREATE or ALTER ADD
	 * COLUMN, it must be done after the statement because we don't know the
	 * column's attnum yet.  But if we do have the attnum (in AT_AddIdentity),
	 * we can do the marking immediately, which improves some ALTER TABLE
	 * behaviors.
	 */
	altseqstmt = makeNode(AlterSeqStmt);
	altseqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	attnamelist = list_make3(makeString(snamespace),
							 makeString(cxt->relation->relname),
							 makeString(column->colname));
	altseqstmt->options = list_make1(makeDefElem("owned_by",
												 (Node *) attnamelist, -1));
	altseqstmt->for_identity = for_identity;

	if (col_exists)
		cxt->blist = lappend(cxt->blist, altseqstmt);
	else
		cxt->alist = lappend(cxt->alist, altseqstmt);

	if (snamespace_p)
		*snamespace_p = snamespace;
	if (sname_p)
		*sname_p = sname;
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
	bool		saw_identity;
	bool		saw_generated;
	bool		need_notnull = false;
	bool		disallow_noinherit_notnull = false;
	Constraint *notnull_constraint = NULL;

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
		char	   *snamespace;
		char	   *sname;
		char	   *qstring;
		A_Const    *snamenode;
		TypeCast   *castnode;
		FuncCall   *funccallnode;
		Constraint *constraint;

		generateSerialExtraStmts(cxt, column,
								 column->typeName->typeOid, NIL,
								 false, false,
								 &snamespace, &sname);

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
		snamenode->val.node.type = T_String;
		snamenode->val.sval.sval = qstring;
		snamenode->location = -1;
		castnode = makeNode(TypeCast);
		castnode->typeName = SystemTypeName("regclass");
		castnode->arg = (Node *) snamenode;
		castnode->location = -1;
		funccallnode = makeFuncCall(SystemFuncName("nextval"),
									list_make1(castnode),
									COERCE_EXPLICIT_CALL,
									-1);
		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_DEFAULT;
		constraint->location = -1;
		constraint->raw_expr = (Node *) funccallnode;
		constraint->cooked_expr = NULL;
		column->constraints = lappend(column->constraints, constraint);

		/* have a not-null constraint added later */
		need_notnull = true;
		disallow_noinherit_notnull = true;
	}

	/* Process column constraints, if any... */
	transformConstraintAttrs(cxt, column->constraints);

	/*
	 * First, scan the column's constraints to see if a not-null constraint
	 * that we add must be prevented from being NO INHERIT.  This should be
	 * enforced only for PRIMARY KEY, not IDENTITY or SERIAL.  However, if the
	 * not-null constraint is specified as a table constraint rather than as a
	 * column constraint, AddRelationNotNullConstraints would raise an error
	 * if a NO INHERIT mismatch is found.  To avoid inconsistently disallowing
	 * it in the table constraint case but not the column constraint case, we
	 * disallow it here as well.  Maybe AddRelationNotNullConstraints can be
	 * improved someday, so that it doesn't complain, and then we can remove
	 * the restriction for SERIAL and IDENTITY here as well.
	 */
	if (!disallow_noinherit_notnull)
	{
		foreach_node(Constraint, constraint, column->constraints)
		{
			switch (constraint->contype)
			{
				case CONSTR_IDENTITY:
				case CONSTR_PRIMARY:
					disallow_noinherit_notnull = true;
					break;
				default:
					break;
			}
		}
	}

	/* Now scan them again to do full processing */
	saw_nullable = false;
	saw_default = false;
	saw_identity = false;
	saw_generated = false;

	foreach_node(Constraint, constraint, column->constraints)
	{
		switch (constraint->contype)
		{
			case CONSTR_NULL:
				if ((saw_nullable && column->is_not_null) || need_notnull)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->is_not_null = false;
				saw_nullable = true;
				break;

			case CONSTR_NOTNULL:
				if (cxt->ispartitioned && constraint->is_no_inherit)
					ereport(ERROR,
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("not-null constraints on partitioned tables cannot be NO INHERIT"));

				/* Disallow conflicting [NOT] NULL markings */
				if (saw_nullable && !column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));

				if (disallow_noinherit_notnull && constraint->is_no_inherit)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("conflicting NO INHERIT declarations for not-null constraints on column \"%s\"",
								   column->colname));

				/*
				 * If this is the first time we see this column being marked
				 * not-null, add the constraint entry and keep track of it.
				 * Also, remove previous markings that we need one.
				 *
				 * If this is a redundant not-null specification, just check
				 * that it doesn't conflict with what was specified earlier.
				 *
				 * Any conflicts with table constraints will be further
				 * checked in AddRelationNotNullConstraints().
				 */
				if (!column->is_not_null)
				{
					column->is_not_null = true;
					saw_nullable = true;
					need_notnull = false;

					constraint->keys = list_make1(makeString(column->colname));
					notnull_constraint = constraint;
					cxt->nnconstraints = lappend(cxt->nnconstraints, constraint);
				}
				else if (notnull_constraint)
				{
					if (constraint->conname &&
						notnull_constraint->conname &&
						strcmp(notnull_constraint->conname, constraint->conname) != 0)
						elog(ERROR, "conflicting not-null constraint names \"%s\" and \"%s\"",
							 notnull_constraint->conname, constraint->conname);

					if (notnull_constraint->is_no_inherit != constraint->is_no_inherit)
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("conflicting NO INHERIT declarations for not-null constraints on column \"%s\"",
									   column->colname));

					if (!notnull_constraint->conname && constraint->conname)
						notnull_constraint->conname = constraint->conname;
				}

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

			case CONSTR_IDENTITY:
				{
					Type		ctype;
					Oid			typeOid;

					if (cxt->ofType)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on typed tables")));
					if (cxt->partbound)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on partitions")));

					ctype = typenameType(cxt->pstate, column->typeName, NULL);
					typeOid = ((Form_pg_type) GETSTRUCT(ctype))->oid;
					ReleaseSysCache(ctype);

					if (saw_identity)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple identity specifications for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));

					generateSerialExtraStmts(cxt, column,
											 typeOid, constraint->options,
											 true, false,
											 NULL, NULL);

					column->identity = constraint->generated_when;
					saw_identity = true;

					/*
					 * Identity columns are always NOT NULL, but we may have a
					 * constraint already.
					 */
					if (!saw_nullable)
						need_notnull = true;
					else if (!column->is_not_null)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));
					break;
				}

			case CONSTR_GENERATED:
				if (cxt->ofType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on typed tables")));
				if (saw_generated)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple generation clauses specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->generated = constraint->generated_kind;
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_generated = true;
				break;

			case CONSTR_CHECK:
				cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
				break;

			case CONSTR_PRIMARY:
				if (saw_nullable && !column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				need_notnull = true;

				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("primary key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				/* FALL THRU */

			case CONSTR_UNIQUE:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unique constraints are not supported on foreign tables"),
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
							 errmsg("foreign key constraints are not supported on foreign tables"),
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
			case CONSTR_ATTR_ENFORCED:
			case CONSTR_ATTR_NOT_ENFORCED:
				/* transformConstraintAttrs took care of these */
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 constraint->contype);
				break;
		}

		if (saw_default && saw_identity)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and identity specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_default && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_identity && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both identity and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));
	}

	/*
	 * If we need a not-null constraint for PRIMARY KEY, SERIAL or IDENTITY,
	 * and one was not explicitly specified, add one now.
	 */
	if (need_notnull && !(saw_nullable && column->is_not_null))
	{
		column->is_not_null = true;
		notnull_constraint = makeNotNullConstraint(makeString(column->colname));
		cxt->nnconstraints = lappend(cxt->nnconstraints, notnull_constraint);
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
		stmt->objtype = OBJECT_FOREIGN_TABLE;
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
	switch (constraint->contype)
	{
		case CONSTR_PRIMARY:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("primary key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_UNIQUE:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unique constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_EXCLUSION:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("exclusion constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_CHECK:
			cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
			break;

		case CONSTR_NOTNULL:
			if (cxt->ispartitioned && constraint->is_no_inherit)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("not-null constraints on partitioned tables cannot be NO INHERIT"));

			cxt->nnconstraints = lappend(cxt->nnconstraints, constraint);
			break;

		case CONSTR_FOREIGN:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("foreign key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
			break;

		case CONSTR_NULL:
		case CONSTR_DEFAULT:
		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
		case CONSTR_ATTR_ENFORCED:
		case CONSTR_ATTR_NOT_ENFORCED:
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
 * column definitions that recreate the user defined column portions of
 * <srctable>.  Also, if there are any LIKE options that we can't fully
 * process at this point, add the TableLikeClause to cxt->likeclauses, which
 * will cause utility.c to call expandTableLikeClause() after the new
 * table has been created.
 *
 * Some options are ignored.  For example, as foreign tables have no storage,
 * these INCLUDING options have no effect: STORAGE, COMPRESSION, IDENTITY
 * and INDEXES.  Similarly, INCLUDING INDEXES is ignored from a view.
 */
static void
transformTableLikeClause(CreateStmtContext *cxt, TableLikeClause *table_like_clause)
{
	AttrNumber	parent_attno;
	Relation	relation;
	TupleDesc	tupleDesc;
	AclResult	aclresult;
	char	   *comment;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, cxt->pstate,
									  table_like_clause->relation->location);

	/* Open the relation referenced by the LIKE clause */
	relation = relation_openrv(table_like_clause->relation, AccessShareLock);

	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_VIEW &&
		relation->rd_rel->relkind != RELKIND_MATVIEW &&
		relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
		relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is invalid in LIKE clause",
						RelationGetRelationName(relation)),
				 errdetail_relkind_not_supported(relation->rd_rel->relkind)));

	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * Check for privileges
	 */
	if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		aclresult = object_aclcheck(TypeRelationId, relation->rd_rel->reltype, GetUserId(),
									ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TYPE,
						   RelationGetRelationName(relation));
	}
	else
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(relation), GetUserId(),
									  ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(relation->rd_rel->relkind),
						   RelationGetRelationName(relation));
	}

	tupleDesc = RelationGetDescr(relation);

	/*
	 * Insert the copied attributes into the cxt for the new table definition.
	 * We must do this now so that they appear in the table in the relative
	 * position where the LIKE clause is, as required by SQL99.
	 */
	for (parent_attno = 1; parent_attno <= tupleDesc->natts;
		 parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);
		ColumnDef  *def;

		/*
		 * Ignore dropped columns in the parent.
		 */
		if (attribute->attisdropped)
			continue;

		/*
		 * Create a new column definition
		 */
		def = makeColumnDef(NameStr(attribute->attname), attribute->atttypid,
							attribute->atttypmod, attribute->attcollation);

		/*
		 * Add to column list
		 */
		cxt->columns = lappend(cxt->columns, def);

		/*
		 * Although we don't transfer the column's default/generation
		 * expression now, we need to mark it GENERATED if appropriate.
		 */
		if (attribute->atthasdef && attribute->attgenerated &&
			(table_like_clause->options & CREATE_TABLE_LIKE_GENERATED))
			def->generated = attribute->attgenerated;

		/*
		 * Copy identity if requested
		 */
		if (attribute->attidentity &&
			(table_like_clause->options & CREATE_TABLE_LIKE_IDENTITY) &&
			!cxt->isforeign)
		{
			Oid			seq_relid;
			List	   *seq_options;

			/*
			 * find sequence owned by old column; extract sequence parameters;
			 * build new create sequence command
			 */
			seq_relid = getIdentitySequence(relation, attribute->attnum, false);
			seq_options = sequence_options(seq_relid);
			generateSerialExtraStmts(cxt, def,
									 InvalidOid, seq_options,
									 true, false,
									 NULL, NULL);
			def->identity = attribute->attidentity;
		}

		/* Likewise, copy storage if requested */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_STORAGE) &&
			!cxt->isforeign)
			def->storage = attribute->attstorage;
		else
			def->storage = 0;

		/* Likewise, copy compression if requested */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMPRESSION) != 0 &&
			CompressionMethodIsValid(attribute->attcompression) &&
			!cxt->isforeign)
			def->compression =
				pstrdup(GetCompressionMethodName(attribute->attcompression));
		else
			def->compression = NULL;

		/* Likewise, copy comment if requested */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
			(comment = GetComment(attribute->attrelid,
								  RelationRelationId,
								  attribute->attnum)) != NULL)
		{
			CommentStmt *stmt = makeNode(CommentStmt);

			stmt->objtype = OBJECT_COLUMN;
			stmt->object = (Node *) list_make3(makeString(cxt->relation->schemaname),
											   makeString(cxt->relation->relname),
											   makeString(def->colname));
			stmt->comment = comment;

			cxt->alist = lappend(cxt->alist, stmt);
		}
	}

	/*
	 * Reproduce not-null constraints, if any, by copying them.  We do this
	 * regardless of options given.
	 */
	if (tupleDesc->constr && tupleDesc->constr->has_not_null)
	{
		List	   *lst;

		lst = RelationGetNotNullConstraints(RelationGetRelid(relation), false,
											true);
		cxt->nnconstraints = list_concat(cxt->nnconstraints, lst);
	}

	/*
	 * We cannot yet deal with defaults, CHECK constraints, indexes, or
	 * statistics, since we don't yet know what column numbers the copied
	 * columns will have in the finished table.  If any of those options are
	 * specified, add the LIKE clause to cxt->likeclauses so that
	 * expandTableLikeClause will be called after we do know that.
	 *
	 * In order for this to work, we remember the relation OID so that
	 * expandTableLikeClause is certain to open the same table.
	 */
	if (table_like_clause->options &
		(CREATE_TABLE_LIKE_DEFAULTS |
		 CREATE_TABLE_LIKE_GENERATED |
		 CREATE_TABLE_LIKE_CONSTRAINTS |
		 CREATE_TABLE_LIKE_INDEXES |
		 CREATE_TABLE_LIKE_STATISTICS))
	{
		table_like_clause->relationOid = RelationGetRelid(relation);
		cxt->likeclauses = lappend(cxt->likeclauses, table_like_clause);
	}

	/*
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.  That will prevent someone else from deleting or ALTERing the
	 * parent before we can run expandTableLikeClause.
	 */
	table_close(relation, NoLock);
}

/*
 * expandTableLikeClause
 *
 * Process LIKE options that require knowing the final column numbers
 * assigned to the new table's columns.  This executes after we have
 * run DefineRelation for the new table.  It returns a list of utility
 * commands that should be run to generate indexes etc.
 */
List *
expandTableLikeClause(RangeVar *heapRel, TableLikeClause *table_like_clause)
{
	List	   *result = NIL;
	List	   *atsubcmds = NIL;
	AttrNumber	parent_attno;
	Relation	relation;
	Relation	childrel;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	AttrMap    *attmap;
	char	   *comment;

	/*
	 * Open the relation referenced by the LIKE clause.  We should still have
	 * the table lock obtained by transformTableLikeClause (and this'll throw
	 * an assertion failure if not).  Hence, no need to recheck privileges
	 * etc.  We must open the rel by OID not name, to be sure we get the same
	 * table.
	 */
	if (!OidIsValid(table_like_clause->relationOid))
		elog(ERROR, "expandTableLikeClause called on untransformed LIKE clause");

	relation = relation_open(table_like_clause->relationOid, NoLock);

	tupleDesc = RelationGetDescr(relation);
	constr = tupleDesc->constr;

	/*
	 * Open the newly-created child relation; we have lock on that too.
	 */
	childrel = relation_openrv(heapRel, NoLock);

	/*
	 * Construct a map from the LIKE relation's attnos to the child rel's.
	 * This re-checks type match etc, although it shouldn't be possible to
	 * have a failure since both tables are locked.
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(childrel),
								   tupleDesc,
								   false);

	/*
	 * Process defaults, if required.
	 */
	if ((table_like_clause->options &
		 (CREATE_TABLE_LIKE_DEFAULTS | CREATE_TABLE_LIKE_GENERATED)) &&
		constr != NULL)
	{
		for (parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
														parent_attno - 1);

			/*
			 * Ignore dropped columns in the parent.
			 */
			if (attribute->attisdropped)
				continue;

			/*
			 * Copy default, if present and it should be copied.  We have
			 * separate options for plain default expressions and GENERATED
			 * defaults.
			 */
			if (attribute->atthasdef &&
				(attribute->attgenerated ?
				 (table_like_clause->options & CREATE_TABLE_LIKE_GENERATED) :
				 (table_like_clause->options & CREATE_TABLE_LIKE_DEFAULTS)))
			{
				Node	   *this_default;
				AlterTableCmd *atsubcmd;
				bool		found_whole_row;

				this_default = TupleDescGetDefault(tupleDesc, parent_attno);
				if (this_default == NULL)
					elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
						 parent_attno, RelationGetRelationName(relation));

				atsubcmd = makeNode(AlterTableCmd);
				atsubcmd->subtype = AT_CookedColumnDefault;
				atsubcmd->num = attmap->attnums[parent_attno - 1];
				atsubcmd->def = map_variable_attnos(this_default,
													1, 0,
													attmap,
													InvalidOid,
													&found_whole_row);

				/*
				 * Prevent this for the same reason as for constraints below.
				 * Note that defaults cannot contain any vars, so it's OK that
				 * the error message refers to generated columns.
				 */
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
									   NameStr(attribute->attname),
									   RelationGetRelationName(relation))));

				atsubcmds = lappend(atsubcmds, atsubcmd);
			}
		}
	}

	/*
	 * Copy CHECK constraints if requested, being careful to adjust attribute
	 * numbers so they match the child.
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_CONSTRAINTS) &&
		constr != NULL)
	{
		int			ccnum;

		for (ccnum = 0; ccnum < constr->num_check; ccnum++)
		{
			char	   *ccname = constr->check[ccnum].ccname;
			char	   *ccbin = constr->check[ccnum].ccbin;
			bool		ccenforced = constr->check[ccnum].ccenforced;
			bool		ccvalid = constr->check[ccnum].ccvalid;
			bool		ccnoinherit = constr->check[ccnum].ccnoinherit;
			Node	   *ccbin_node;
			bool		found_whole_row;
			Constraint *n;
			AlterTableCmd *atsubcmd;

			ccbin_node = map_variable_attnos(stringToNode(ccbin),
											 1, 0,
											 attmap,
											 InvalidOid, &found_whole_row);

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

			n = makeNode(Constraint);
			n->contype = CONSTR_CHECK;
			n->conname = pstrdup(ccname);
			n->location = -1;
			n->is_enforced = ccenforced;
			n->initially_valid = ccvalid;
			n->is_no_inherit = ccnoinherit;
			n->raw_expr = NULL;
			n->cooked_expr = nodeToString(ccbin_node);

			/* We can skip validation, since the new table should be empty. */
			n->skip_validation = true;

			atsubcmd = makeNode(AlterTableCmd);
			atsubcmd->subtype = AT_AddConstraint;
			atsubcmd->def = (Node *) n;
			atsubcmds = lappend(atsubcmds, atsubcmd);

			/* Copy comment on constraint */
			if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
				(comment = GetComment(get_relation_constraint_oid(RelationGetRelid(relation),
																  n->conname, false),
									  ConstraintRelationId,
									  0)) != NULL)
			{
				CommentStmt *stmt = makeNode(CommentStmt);

				stmt->objtype = OBJECT_TABCONSTRAINT;
				stmt->object = (Node *) list_make3(makeString(heapRel->schemaname),
												   makeString(heapRel->relname),
												   makeString(n->conname));
				stmt->comment = comment;

				result = lappend(result, stmt);
			}
		}
	}

	/*
	 * If we generated any ALTER TABLE actions above, wrap them into a single
	 * ALTER TABLE command.  Stick it at the front of the result, so it runs
	 * before any CommentStmts we made above.
	 */
	if (atsubcmds)
	{
		AlterTableStmt *atcmd = makeNode(AlterTableStmt);

		atcmd->relation = copyObject(heapRel);
		atcmd->cmds = atsubcmds;
		atcmd->objtype = OBJECT_TABLE;
		atcmd->missing_ok = false;
		result = lcons(atcmd, result);
	}

	/*
	 * Process indexes if required.
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_INDEXES) &&
		relation->rd_rel->relhasindex &&
		childrel->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
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
			index_stmt = generateClonedIndexStmt(heapRel,
												 parent_index,
												 attmap,
												 NULL);

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

			result = lappend(result, index_stmt);

			index_close(parent_index, AccessShareLock);
		}
	}

	/*
	 * Process extended statistics if required.
	 */
	if (table_like_clause->options & CREATE_TABLE_LIKE_STATISTICS)
	{
		List	   *parent_extstats;
		ListCell   *l;

		parent_extstats = RelationGetStatExtList(relation);

		foreach(l, parent_extstats)
		{
			Oid			parent_stat_oid = lfirst_oid(l);
			CreateStatsStmt *stats_stmt;

			stats_stmt = generateClonedExtStatsStmt(heapRel,
													RelationGetRelid(childrel),
													parent_stat_oid,
													attmap);

			/* Copy comment on statistics object, if requested */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_stat_oid, StatisticExtRelationId, 0);

				/*
				 * We make use of CreateStatsStmt's stxcomment option, so as
				 * not to need to know now what name the statistics will have.
				 */
				stats_stmt->stxcomment = comment;
			}

			result = lappend(result, stats_stmt);
		}

		list_free(parent_extstats);
	}

	/* Done with child rel */
	table_close(childrel, NoLock);

	/*
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.  That will prevent someone else from deleting or ALTERing the
	 * parent before the child is committed.
	 */
	table_close(relation, NoLock);

	return result;
}

static void
transformOfType(CreateStmtContext *cxt, TypeName *ofTypename)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			i;
	Oid			ofTypeId;

	Assert(ofTypename);

	tuple = typenameType(cxt->pstate, ofTypename, NULL);
	check_of_type(tuple);
	ofTypeId = ((Form_pg_type) GETSTRUCT(tuple))->oid;
	ofTypename->typeOid = ofTypeId; /* cached for later */

	tupdesc = lookup_rowtype_tupdesc(ofTypeId, -1);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		ColumnDef  *n;

		if (attr->attisdropped)
			continue;

		n = makeColumnDef(NameStr(attr->attname), attr->atttypid,
						  attr->atttypmod, attr->attcollation);
		n->is_from_type = true;

		cxt->columns = lappend(cxt->columns, n);
	}
	ReleaseTupleDesc(tupdesc);

	ReleaseSysCache(tuple);
}

/*
 * Generate an IndexStmt node using information from an already existing index
 * "source_idx".
 *
 * heapRel is stored into the IndexStmt's relation field, but we don't use it
 * otherwise; some callers pass NULL, if they don't need it to be valid.
 * (The target relation might not exist yet, so we mustn't try to access it.)
 *
 * Attribute numbers in expression Vars are adjusted according to attmap.
 *
 * If constraintOid isn't NULL, we store the OID of any constraint associated
 * with the index there.
 *
 * Unlike transformIndexConstraint, we don't make any effort to force primary
 * key columns to be not-null.  The larger cloning process this is part of
 * should have cloned their not-null status separately (and DefineIndex will
 * complain if that fails to happen).
 */
IndexStmt *
generateClonedIndexStmt(RangeVar *heapRel, Relation source_idx,
						const AttrMap *attmap,
						Oid *constraintOid)
{
	Oid			source_relid = RelationGetRelid(source_idx);
	HeapTuple	ht_idxrel;
	HeapTuple	ht_idx;
	HeapTuple	ht_am;
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

	if (constraintOid)
		*constraintOid = InvalidOid;

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

	/* Fetch the pg_am tuple of the index' access method */
	ht_am = SearchSysCache1(AMOID, ObjectIdGetDatum(idxrelrec->relam));
	if (!HeapTupleIsValid(ht_am))
		elog(ERROR, "cache lookup failed for access method %u",
			 idxrelrec->relam);
	amrec = (Form_pg_am) GETSTRUCT(ht_am);

	/* Extract indcollation from the pg_index tuple */
	datum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
								   Anum_pg_index_indcollation);
	indcollation = (oidvector *) DatumGetPointer(datum);

	/* Extract indclass from the pg_index tuple */
	datum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx, Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(datum);

	/* Begin building the IndexStmt */
	index = makeNode(IndexStmt);
	index->relation = heapRel;
	index->accessMethod = pstrdup(NameStr(amrec->amname));
	if (OidIsValid(idxrelrec->reltablespace))
		index->tableSpace = get_tablespace_name(idxrelrec->reltablespace);
	else
		index->tableSpace = NULL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNumber = InvalidRelFileNumber;
	index->oldCreateSubid = InvalidSubTransactionId;
	index->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;
	index->unique = idxrec->indisunique;
	index->nulls_not_distinct = idxrec->indnullsnotdistinct;
	index->primary = idxrec->indisprimary;
	index->iswithoutoverlaps = (idxrec->indisprimary || idxrec->indisunique) && idxrec->indisexclusion;
	index->transformed = true;	/* don't need transformIndexStmt */
	index->concurrent = false;
	index->if_not_exists = false;
	index->reset_default_tblspc = false;

	/*
	 * We don't try to preserve the name of the source index; instead, just
	 * let DefineIndex() choose a reasonable name.  (If we tried to preserve
	 * the name, we'd get duplicate-relation-name failures unless the source
	 * table was in a different schema.)
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

			if (constraintOid)
				*constraintOid = constraintId;

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

				Assert(conrec->contype == CONSTRAINT_EXCLUSION ||
					   (index->iswithoutoverlaps &&
						(conrec->contype == CONSTRAINT_PRIMARY || conrec->contype == CONSTRAINT_UNIQUE)));
				/* Extract operator OIDs from the pg_constraint tuple */
				datum = SysCacheGetAttrNotNull(CONSTROID, ht_constr,
											   Anum_pg_constraint_conexclop);
				deconstruct_array_builtin(DatumGetArrayTypeP(datum), OIDOID, &elems, NULL, &nElems);

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
	index->indexIncludingParams = NIL;

	indexpr_item = list_head(indexprs);
	for (keyno = 0; keyno < idxrec->indnkeyatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(source_idx),
											   keyno);
		int16		opt = source_idx->rd_indoption[keyno];

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* Simple index column */
			char	   *attname;

			attname = get_attname(indrelid, attnum, false);
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
			indexpr_item = lnext(indexprs, indexpr_item);

			/* Adjust Vars to match new table's column numbering */
			indexkey = map_variable_attnos(indexkey,
										   1, 0,
										   attmap,
										   InvalidOid, &found_whole_row);

			/* As in expandTableLikeClause, reject whole-row variables */
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
		iparam->indexcolname = pstrdup(NameStr(attr->attname));

		/* Add the collation name, if non-default */
		iparam->collation = get_collation(indcollation->values[keyno], keycoltype);

		/* Add the operator class name, if non-default */
		iparam->opclass = get_opclass(indclass->values[keyno], keycoltype);
		iparam->opclassopts =
			untransformRelOptions(get_attoptions(source_relid, keyno + 1));

		iparam->ordering = SORTBY_DEFAULT;
		iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;

		/* Adjust options if necessary */
		if (source_idx->rd_indam->amcanorder)
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

	/* Handle included columns separately */
	for (keyno = idxrec->indnkeyatts; keyno < idxrec->indnatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(source_idx),
											   keyno);

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* Simple index column */
			char	   *attname;

			attname = get_attname(indrelid, attnum, false);

			iparam->name = attname;
			iparam->expr = NULL;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("expressions are not supported in included columns")));

		/* Copy the original index column name */
		iparam->indexcolname = pstrdup(NameStr(attr->attname));

		index->indexIncludingParams = lappend(index->indexIncludingParams, iparam);
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
										attmap,
										InvalidOid, &found_whole_row);

		/* As in expandTableLikeClause, reject whole-row variables */
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
	ReleaseSysCache(ht_am);

	return index;
}

/*
 * Generate a CreateStatsStmt node using information from an already existing
 * extended statistic "source_statsid", for the rel identified by heapRel and
 * heapRelid.
 *
 * Attribute numbers in expression Vars are adjusted according to attmap.
 */
static CreateStatsStmt *
generateClonedExtStatsStmt(RangeVar *heapRel, Oid heapRelid,
						   Oid source_statsid, const AttrMap *attmap)
{
	HeapTuple	ht_stats;
	Form_pg_statistic_ext statsrec;
	CreateStatsStmt *stats;
	List	   *stat_types = NIL;
	List	   *def_names = NIL;
	bool		isnull;
	Datum		datum;
	ArrayType  *arr;
	char	   *enabled;
	int			i;

	Assert(OidIsValid(heapRelid));
	Assert(heapRel != NULL);

	/*
	 * Fetch pg_statistic_ext tuple of source statistics object.
	 */
	ht_stats = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(source_statsid));
	if (!HeapTupleIsValid(ht_stats))
		elog(ERROR, "cache lookup failed for statistics object %u", source_statsid);
	statsrec = (Form_pg_statistic_ext) GETSTRUCT(ht_stats);

	/* Determine which statistics types exist */
	datum = SysCacheGetAttrNotNull(STATEXTOID, ht_stats,
								   Anum_pg_statistic_ext_stxkind);
	arr = DatumGetArrayTypeP(datum);
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "stxkind is not a 1-D char array");
	enabled = (char *) ARR_DATA_PTR(arr);
	for (i = 0; i < ARR_DIMS(arr)[0]; i++)
	{
		if (enabled[i] == STATS_EXT_NDISTINCT)
			stat_types = lappend(stat_types, makeString("ndistinct"));
		else if (enabled[i] == STATS_EXT_DEPENDENCIES)
			stat_types = lappend(stat_types, makeString("dependencies"));
		else if (enabled[i] == STATS_EXT_MCV)
			stat_types = lappend(stat_types, makeString("mcv"));
		else if (enabled[i] == STATS_EXT_EXPRESSIONS)
			/* expression stats are not exposed to users */
			continue;
		else
			elog(ERROR, "unrecognized statistics kind %c", enabled[i]);
	}

	/* Determine which columns the statistics are on */
	for (i = 0; i < statsrec->stxkeys.dim1; i++)
	{
		StatsElem  *selem = makeNode(StatsElem);
		AttrNumber	attnum = statsrec->stxkeys.values[i];

		selem->name = get_attname(heapRelid, attnum, false);
		selem->expr = NULL;

		def_names = lappend(def_names, selem);
	}

	/*
	 * Now handle expressions, if there are any. The order (with respect to
	 * regular attributes) does not really matter for extended stats, so we
	 * simply append them after simple column references.
	 *
	 * XXX Some places during build/estimation treat expressions as if they
	 * are before attributes, but for the CREATE command that's entirely
	 * irrelevant.
	 */
	datum = SysCacheGetAttr(STATEXTOID, ht_stats,
							Anum_pg_statistic_ext_stxexprs, &isnull);

	if (!isnull)
	{
		ListCell   *lc;
		List	   *exprs = NIL;
		char	   *exprsString;

		exprsString = TextDatumGetCString(datum);
		exprs = (List *) stringToNode(exprsString);

		foreach(lc, exprs)
		{
			Node	   *expr = (Node *) lfirst(lc);
			StatsElem  *selem = makeNode(StatsElem);
			bool		found_whole_row;

			/* Adjust Vars to match new table's column numbering */
			expr = map_variable_attnos(expr,
									   1, 0,
									   attmap,
									   InvalidOid,
									   &found_whole_row);

			selem->name = NULL;
			selem->expr = expr;

			def_names = lappend(def_names, selem);
		}

		pfree(exprsString);
	}

	/* finally, build the output node */
	stats = makeNode(CreateStatsStmt);
	stats->defnames = NULL;
	stats->stat_types = stat_types;
	stats->exprs = def_names;
	stats->relations = list_make1(heapRel);
	stats->stxcomment = NULL;
	stats->transformed = true;	/* don't need transformStatsStmt again */
	stats->if_not_exists = false;

	/* Clean up */
	ReleaseSysCache(ht_stats);

	return stats;
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
	List	   *finalindexlist = NIL;
	ListCell   *lc;

	/*
	 * Run through the constraints that need to generate an index, and do so.
	 *
	 * For PRIMARY KEY, this queues not-null constraints for each column, if
	 * needed.
	 */
	foreach(lc, cxt->ixconstraints)
	{
		Constraint *constraint = lfirst_node(Constraint, lc);

		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE ||
			   constraint->contype == CONSTR_EXCLUSION);

		index = transformIndexConstraint(constraint, cxt);

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
	if (cxt->pkey != NULL)
	{
		/* Make sure we keep the PKEY index in preference to others... */
		finalindexlist = list_make1(cxt->pkey);
	}

	foreach(lc, indexlist)
	{
		bool		keep = true;
		ListCell   *k;

		index = lfirst(lc);

		/* if it's pkey, it's already in finalindexlist */
		if (index == cxt->pkey)
			continue;

		foreach(k, finalindexlist)
		{
			IndexStmt  *priorindex = lfirst(k);

			if (equal(index->indexParams, priorindex->indexParams) &&
				equal(index->indexIncludingParams, priorindex->indexIncludingParams) &&
				equal(index->whereClause, priorindex->whereClause) &&
				equal(index->excludeOpNames, priorindex->excludeOpNames) &&
				strcmp(index->accessMethod, priorindex->accessMethod) == 0 &&
				index->nulls_not_distinct == priorindex->nulls_not_distinct &&
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
			finalindexlist = lappend(finalindexlist, index);
	}

	/*
	 * Now append all the IndexStmts to cxt->alist.
	 */
	cxt->alist = list_concat(cxt->alist, finalindexlist);
}

/*
 * transformIndexConstraint
 *		Transform one UNIQUE, PRIMARY KEY, or EXCLUDE constraint for
 *		transformIndexConstraints. An IndexStmt is returned.
 *
 * For a PRIMARY KEY constraint, we additionally create not-null constraints
 * for columns that don't already have them.
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
	index->nulls_not_distinct = constraint->nulls_not_distinct;
	index->isconstraint = true;
	index->iswithoutoverlaps = constraint->without_overlaps;
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
	index->indexIncludingParams = NIL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNumber = InvalidRelFileNumber;
	index->oldCreateSubid = InvalidSubTransactionId;
	index->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;
	index->transformed = false;
	index->concurrent = false;
	index->if_not_exists = false;
	index->reset_default_tblspc = constraint->reset_default_tblspc;

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

		if (!index_form->indisvalid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" is not valid", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/*
		 * Today we forbid non-unique indexes, but we could permit GiST
		 * indexes whose last entry is a range type and use that to create a
		 * WITHOUT OVERLAPS constraint (i.e. a temporal constraint).
		 */
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
		 * Insist on it being a btree.  We must have an index that exactly
		 * matches what you'd get from plain ADD CONSTRAINT syntax, else dump
		 * and reload will produce a different index (breaking pg_upgrade in
		 * particular).
		 */
		if (index_rel->rd_rel->relam != get_index_am_oid(DEFAULT_INDEX_TYPE, false))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a btree", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Must get indclass the hard way */
		indclassDatum = SysCacheGetAttrNotNull(INDEXRELID,
											   index_rel->rd_indextuple,
											   Anum_pg_index_indclass);
		indclass = (oidvector *) DatumGetPointer(indclassDatum);

		for (i = 0; i < index_form->indnatts; i++)
		{
			int16		attnum = index_form->indkey.values[i];
			const FormData_pg_attribute *attform;
			char	   *attname;
			Oid			defopclass;

			/*
			 * We shouldn't see attnum == 0 here, since we already rejected
			 * expression indexes.  If we do, SystemAttributeDefinition will
			 * throw an error.
			 */
			if (attnum > 0)
			{
				Assert(attnum <= heap_rel->rd_att->natts);
				attform = TupleDescAttr(heap_rel->rd_att, attnum - 1);
			}
			else
				attform = SystemAttributeDefinition(attnum);
			attname = pstrdup(NameStr(attform->attname));

			if (i < index_form->indnkeyatts)
			{
				/*
				 * Insist on default opclass, collation, and sort options.
				 * While the index would still work as a constraint with
				 * non-default settings, it might not provide exactly the same
				 * uniqueness semantics as you'd get from a normally-created
				 * constraint; and there's also the dump/reload problem
				 * mentioned above.
				 */
				Datum		attoptions =
					get_attoptions(RelationGetRelid(index_rel), i + 1);

				defopclass = GetDefaultOpClass(attform->atttypid,
											   index_rel->rd_rel->relam);
				if (indclass->values[i] != defopclass ||
					attform->attcollation != index_rel->rd_indcollation[i] ||
					attoptions != (Datum) 0 ||
					index_rel->rd_indoption[i] != 0)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("index \"%s\" column number %d does not have default sorting behavior", index_name, i + 1),
							 errdetail("Cannot create a primary key or unique constraint using such an index."),
							 parser_errposition(cxt->pstate, constraint->location)));

				/* If a PK, ensure the columns get not null constraints */
				if (constraint->contype == CONSTR_PRIMARY)
					cxt->nnconstraints =
						lappend(cxt->nnconstraints,
								makeNotNullConstraint(makeString(attname)));

				constraint->keys = lappend(constraint->keys, makeString(attname));
			}
			else
				constraint->including = lappend(constraint->including, makeString(attname));
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
			elem = linitial_node(IndexElem, pair);
			opname = lsecond_node(List, pair);

			index->indexParams = lappend(index->indexParams, elem);
			index->excludeOpNames = lappend(index->excludeOpNames, opname);
		}
	}

	/*
	 * For UNIQUE and PRIMARY KEY, we just have a list of column names.
	 *
	 * Make sure referenced keys exist.  If we are making a PRIMARY KEY index,
	 * also make sure they are not-null.  For WITHOUT OVERLAPS constraints, we
	 * make sure the last part is a range or multirange.
	 */
	else
	{
		foreach(lc, constraint->keys)
		{
			char	   *key = strVal(lfirst(lc));
			bool		found = false;
			ColumnDef  *column = NULL;
			ListCell   *columns;
			IndexElem  *iparam;
			Oid			typid = InvalidOid;

			/* Make sure referenced column exists. */
			foreach(columns, cxt->columns)
			{
				column = lfirst_node(ColumnDef, columns);
				if (strcmp(column->colname, key) == 0)
				{
					found = true;
					break;
				}
			}
			if (!found)
				column = NULL;

			if (found)
			{
				/*
				 * column is defined in the new table.  For CREATE TABLE with
				 * a PRIMARY KEY, we can apply the not-null constraint cheaply
				 * here.  If the not-null constraint already exists, we can
				 * (albeit not so cheaply) verify that it's not a NO INHERIT
				 * constraint.
				 *
				 * Note that ALTER TABLE never needs either check, because
				 * those constraints have already been added by
				 * ATPrepAddPrimaryKey.
				 */
				if (constraint->contype == CONSTR_PRIMARY &&
					!cxt->isalter)
				{
					if (column->is_not_null)
					{
						foreach_node(Constraint, nn, cxt->nnconstraints)
						{
							if (strcmp(strVal(linitial(nn->keys)), key) == 0)
							{
								if (nn->is_no_inherit)
									ereport(ERROR,
											errcode(ERRCODE_SYNTAX_ERROR),
											errmsg("conflicting NO INHERIT declaration for not-null constraint on column \"%s\"",
												   key));
								break;
							}
						}
					}
					else
					{
						column->is_not_null = true;
						cxt->nnconstraints =
							lappend(cxt->nnconstraints,
									makeNotNullConstraint(makeString(key)));
					}
				}
				else if (constraint->contype == CONSTR_PRIMARY)
					Assert(column->is_not_null);
			}
			else if (SystemAttributeByName(key) != NULL)
			{
				/*
				 * column will be a system column in the new table, so accept
				 * it. System columns can't ever be null, so no need to worry
				 * about PRIMARY/NOT NULL constraint.
				 */
				found = true;
			}
			else if (cxt->inhRelations)
			{
				/* try inherited tables */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = lfirst_node(RangeVar, inher);
					Relation	rel;
					int			count;

					rel = table_openrv(inh, AccessShareLock);
					/* check user requested inheritance from valid relkind */
					if (rel->rd_rel->relkind != RELKIND_RELATION &&
						rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
						rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table or foreign table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
																  count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(key, inhname) == 0)
						{
							found = true;
							typid = inhattr->atttypid;

							if (constraint->contype == CONSTR_PRIMARY)
								cxt->nnconstraints =
									lappend(cxt->nnconstraints,
											makeNotNullConstraint(makeString(pstrdup(inhname))));
							break;
						}
					}
					table_close(rel, NoLock);
					if (found)
						break;
				}
			}

			/*
			 * In the ALTER TABLE case, don't complain about index keys not
			 * created in the command; they may well exist already.
			 * DefineIndex will complain about them if not.
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

			/*
			 * The WITHOUT OVERLAPS part (if any) must be a range or
			 * multirange type.
			 */
			if (constraint->without_overlaps && lc == list_last_cell(constraint->keys))
			{
				if (!found && cxt->isalter)
				{
					/*
					 * Look up the column type on existing table. If we can't
					 * find it, let things fail in DefineIndex.
					 */
					Relation	rel = cxt->rel;

					for (int i = 0; i < rel->rd_att->natts; i++)
					{
						Form_pg_attribute attr = TupleDescAttr(rel->rd_att, i);
						const char *attname;

						if (attr->attisdropped)
							break;

						attname = NameStr(attr->attname);
						if (strcmp(attname, key) == 0)
						{
							found = true;
							typid = attr->atttypid;
							break;
						}
					}
				}
				if (found)
				{
					if (!OidIsValid(typid) && column)
						typid = typenameTypeId(NULL, column->typeName);

					if (!OidIsValid(typid) || !(type_is_range(typid) || type_is_multirange(typid)))
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("column \"%s\" in WITHOUT OVERLAPS is not a range or multirange type", key),
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
			iparam->opclassopts = NIL;
			iparam->ordering = SORTBY_DEFAULT;
			iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
			index->indexParams = lappend(index->indexParams, iparam);
		}

		if (constraint->without_overlaps)
		{
			/*
			 * This enforces that there is at least one equality column
			 * besides the WITHOUT OVERLAPS columns.  This is per SQL
			 * standard.  XXX Do we need this?
			 */
			if (list_length(constraint->keys) < 2)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("constraint using WITHOUT OVERLAPS needs at least two columns"));

			/* WITHOUT OVERLAPS requires a GiST index */
			index->accessMethod = "gist";
		}

	}

	/*
	 * Add included columns to index definition.  This is much like the
	 * simple-column-name-list code above, except that we don't worry about
	 * NOT NULL marking; included columns in a primary key should not be
	 * forced NOT NULL.  We don't complain about duplicate columns, either,
	 * though maybe we should?
	 */
	foreach(lc, constraint->including)
	{
		char	   *key = strVal(lfirst(lc));
		bool		found = false;
		ColumnDef  *column = NULL;
		ListCell   *columns;
		IndexElem  *iparam;

		foreach(columns, cxt->columns)
		{
			column = lfirst_node(ColumnDef, columns);
			if (strcmp(column->colname, key) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			if (SystemAttributeByName(key) != NULL)
			{
				/*
				 * column will be a system column in the new table, so accept
				 * it.
				 */
				found = true;
			}
			else if (cxt->inhRelations)
			{
				/* try inherited tables */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = lfirst_node(RangeVar, inher);
					Relation	rel;
					int			count;

					rel = table_openrv(inh, AccessShareLock);
					/* check user requested inheritance from valid relkind */
					if (rel->rd_rel->relkind != RELKIND_RELATION &&
						rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
						rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table or foreign table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
																  count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(key, inhname) == 0)
						{
							found = true;
							break;
						}
					}
					table_close(rel, NoLock);
					if (found)
						break;
				}
			}
		}

		/*
		 * In the ALTER TABLE case, don't complain about index keys not
		 * created in the command; they may well exist already. DefineIndex
		 * will complain about them if not.
		 */
		if (!found && !cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" named in key does not exist", key),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* OK, add it to the index definition */
		iparam = makeNode(IndexElem);
		iparam->name = pstrdup(key);
		iparam->expr = NULL;
		iparam->indexcolname = NULL;
		iparam->collation = NIL;
		iparam->opclass = NIL;
		iparam->opclassopts = NIL;
		index->indexIncludingParams = lappend(index->indexIncludingParams, iparam);
	}

	return index;
}

/*
 * transformCheckConstraints
 *		handle CHECK constraints
 *
 * Right now, there's nothing to do here when called from ALTER TABLE,
 * but the other constraint-transformation functions are called in both
 * the CREATE TABLE and ALTER TABLE paths, so do the same here, and just
 * don't do anything if we're not authorized to skip validation.
 */
static void
transformCheckConstraints(CreateStmtContext *cxt, bool skipValidation)
{
	ListCell   *ckclist;

	if (cxt->ckconstraints == NIL)
		return;

	/*
	 * When creating a new table (but not a foreign table), we can safely skip
	 * the validation of check constraints and mark them as valid based on the
	 * constraint enforcement flag, since NOT ENFORCED constraints must always
	 * be marked as NOT VALID. (This will override any user-supplied NOT VALID
	 * flag.)
	 */
	if (skipValidation)
	{
		foreach(ckclist, cxt->ckconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(ckclist);

			constraint->skip_validation = true;
			constraint->initially_valid = constraint->is_enforced;
		}
	}
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
	 * skip validation of FK constraints, and mark them as valid based on the
	 * constraint enforcement flag, since NOT ENFORCED constraints must always
	 * be marked as NOT VALID. (This will override any user-supplied NOT VALID
	 * flag.)
	 */
	if (skipValidation)
	{
		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);

			constraint->skip_validation = true;
			constraint->initially_valid = constraint->is_enforced;
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
	 * already in cxt->alist.  See also the handling of cxt->likeclauses.
	 */
	if (!isAddConstraint)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = cxt->relation;
		alterstmt->cmds = NIL;
		alterstmt->objtype = OBJECT_TABLE;

		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);
			AlterTableCmd *altercmd = makeNode(AlterTableCmd);

			altercmd->subtype = AT_AddConstraint;
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
 * a predicate expression.  There are several code paths that create indexes
 * without bothering to call this, because they know they don't have any
 * such expressions to deal with.
 *
 * To avoid race conditions, it's important that this function rely only on
 * the passed-in relid (and not on stmt->relation) to determine the target
 * relation.
 */
IndexStmt *
transformIndexStmt(Oid relid, IndexStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ListCell   *l;
	Relation	rel;

	/* Nothing to do if statement already transformed. */
	if (stmt->transformed)
		return stmt;

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Put the parent table into the rtable so that the expressions can refer
	 * to its fields without qualification.  Caller is responsible for locking
	 * relation, but we still need to open it.
	 */
	rel = relation_open(relid, NoLock);
	nsitem = addRangeTableEntryForRelation(pstate, rel,
										   AccessShareLock,
										   NULL, false, true);

	/* no to join list, yes to namespaces */
	addNSItemToQuery(pstate, nsitem, false, true, true);

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
			 * aggregates, window functions, and SRFs, based on the EXPR_KIND_
			 * for an index expression.
			 *
			 * DefineIndex() will make more checks.
			 */
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

	/* Close relation */
	table_close(rel, NoLock);

	/* Mark statement as successfully transformed */
	stmt->transformed = true;

	return stmt;
}

/*
 * transformStatsStmt - parse analysis for CREATE STATISTICS
 *
 * To avoid race conditions, it's important that this function relies only on
 * the passed-in relid (and not on stmt->relation) to determine the target
 * relation.
 */
CreateStatsStmt *
transformStatsStmt(Oid relid, CreateStatsStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ListCell   *l;
	Relation	rel;

	/* Nothing to do if statement already transformed. */
	if (stmt->transformed)
		return stmt;

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Put the parent table into the rtable so that the expressions can refer
	 * to its fields without qualification.  Caller is responsible for locking
	 * relation, but we still need to open it.
	 */
	rel = relation_open(relid, NoLock);
	nsitem = addRangeTableEntryForRelation(pstate, rel,
										   AccessShareLock,
										   NULL, false, true);

	/* no to join list, yes to namespaces */
	addNSItemToQuery(pstate, nsitem, false, true, true);

	/* take care of any expressions */
	foreach(l, stmt->exprs)
	{
		StatsElem  *selem = (StatsElem *) lfirst(l);

		if (selem->expr)
		{
			/* Now do parse transformation of the expression */
			selem->expr = transformExpr(pstate, selem->expr,
										EXPR_KIND_STATS_EXPRESSION);

			/* We have to fix its collations too */
			assign_expr_collations(pstate, selem->expr);
		}
	}

	/*
	 * Check that only the base rel is mentioned.  (This should be dead code
	 * now that add_missing_from is history.)
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("statistics expressions can refer only to the table being referenced")));

	free_parsestate(pstate);

	/* Close relation */
	table_close(rel, NoLock);

	/* Mark statement as successfully transformed */
	stmt->transformed = true;

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
 */
void
transformRuleStmt(RuleStmt *stmt, const char *queryString,
				  List **actions, Node **whereClause)
{
	Relation	rel;
	ParseState *pstate;
	ParseNamespaceItem *oldnsitem;
	ParseNamespaceItem *newnsitem;

	/*
	 * To avoid deadlock, make sure the first thing we do is grab
	 * AccessExclusiveLock on the target relation.  This will be needed by
	 * DefineQueryRewrite(), and we don't want to grab a lesser lock
	 * beforehand.
	 */
	rel = table_openrv(stmt->relation, AccessExclusiveLock);

	if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rules on materialized views are not supported")));

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * NOTE: 'OLD' must always have a varno equal to 1 and 'NEW' equal to 2.
	 * Set up their ParseNamespaceItems in the main pstate for use in parsing
	 * the rule qualification.
	 */
	oldnsitem = addRangeTableEntryForRelation(pstate, rel,
											  AccessShareLock,
											  makeAlias("old", NIL),
											  false, false);
	newnsitem = addRangeTableEntryForRelation(pstate, rel,
											  AccessShareLock,
											  makeAlias("new", NIL),
											  false, false);

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
			addNSItemToQuery(pstate, oldnsitem, false, true, true);
			break;
		case CMD_UPDATE:
			addNSItemToQuery(pstate, oldnsitem, false, true, true);
			addNSItemToQuery(pstate, newnsitem, false, true, true);
			break;
		case CMD_INSERT:
			addNSItemToQuery(pstate, newnsitem, false, true, true);
			break;
		case CMD_DELETE:
			addNSItemToQuery(pstate, oldnsitem, false, true, true);
			break;
		default:
			elog(ERROR, "unrecognized event type: %d",
				 (int) stmt->event);
			break;
	}

	/* take care of the where clause */
	*whereClause = transformWhereClause(pstate,
										stmt->whereClause,
										EXPR_KIND_WHERE,
										"WHERE");
	/* we have to fix its collations too */
	assign_expr_collations(pstate, *whereClause);

	/* this is probably dead code without add_missing_from: */
	if (list_length(pstate->p_rtable) != 2) /* naughty, naughty... */
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
		nothing_qry->rteperminfos = pstate->p_rteperminfos;
		nothing_qry->jointree = makeFromExpr(NIL, NULL);	/* no join wanted */

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
			oldnsitem = addRangeTableEntryForRelation(sub_pstate, rel,
													  AccessShareLock,
													  makeAlias("old", NIL),
													  false, false);
			newnsitem = addRangeTableEntryForRelation(sub_pstate, rel,
													  AccessShareLock,
													  makeAlias("new", NIL),
													  false, false);
			addNSItemToQuery(sub_pstate, oldnsitem, false, true, false);
			addNSItemToQuery(sub_pstate, newnsitem, false, true, false);

			/* Transform the rule action statement */
			top_subqry = transformStmt(sub_pstate, action);

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
				RangeTblRef *rtr;

				/*
				 * If sub_qry is a setop, manipulating its jointree will do no
				 * good at all, because the jointree is dummy. (This should be
				 * a can't-happen case because of prior tests.)
				 */
				if (sub_qry->setOperations != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
				/* hackishly add OLD to the already-built FROM clause */
				rtr = makeNode(RangeTblRef);
				rtr->rtindex = oldnsitem->p_rtindex;
				sub_qry->jointree->fromlist =
					lappend(sub_qry->jointree->fromlist, rtr);
			}

			newactions = lappend(newactions, top_subqry);

			free_parsestate(sub_pstate);
		}

		*actions = newactions;
	}

	free_parsestate(pstate);

	/* Close relation, but keep the exclusive lock */
	table_close(rel, NoLock);
}


/*
 * transformAlterTableStmt -
 *		parse analysis for ALTER TABLE
 *
 * Returns the transformed AlterTableStmt.  There may be additional actions
 * to be done before and after the transformed statement, which are returned
 * in *beforeStmts and *afterStmts as lists of utility command parsetrees.
 *
 * To avoid race conditions, it's important that this function rely only on
 * the passed-in relid (and not on stmt->relation) to determine the target
 * relation.
 */
AlterTableStmt *
transformAlterTableStmt(Oid relid, AlterTableStmt *stmt,
						const char *queryString,
						List **beforeStmts, List **afterStmts)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *save_alist;
	ListCell   *lcmd,
			   *l;
	List	   *newcmds = NIL;
	bool		skipValidation = true;
	AlterTableCmd *newcmd;
	ParseNamespaceItem *nsitem;

	/* Caller is responsible for locking the relation */
	rel = relation_open(relid, NoLock);
	tupdesc = RelationGetDescr(rel);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	nsitem = addRangeTableEntryForRelation(pstate,
										   rel,
										   AccessShareLock,
										   NULL,
										   false,
										   true);
	addNSItemToQuery(pstate, nsitem, false, true, true);

	/* Set up CreateStmtContext */
	cxt.pstate = pstate;
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
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
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.nnconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.likeclauses = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	cxt.partbound = NULL;
	cxt.ofType = false;

	/*
	 * Transform ALTER subcommands that need it (most don't).  These largely
	 * re-use code from CREATE TABLE.
	 */
	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		switch (cmd->subtype)
		{
			case AT_AddColumn:
				{
					ColumnDef  *def = castNode(ColumnDef, cmd->def);

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

			case AT_AlterColumnType:
				{
					ColumnDef  *def = castNode(ColumnDef, cmd->def);
					AttrNumber	attnum;

					/*
					 * For ALTER COLUMN TYPE, transform the USING clause if
					 * one was specified.
					 */
					if (def->raw_default)
					{
						def->cooked_default =
							transformExpr(pstate, def->raw_default,
										  EXPR_KIND_ALTER_COL_TRANSFORM);
					}

					/*
					 * For identity column, create ALTER SEQUENCE command to
					 * change the data type of the sequence. Identity sequence
					 * is associated with the top level partitioned table.
					 * Hence ignore partitions.
					 */
					if (!RelationGetForm(rel)->relispartition)
					{
						attnum = get_attnum(relid, cmd->name);
						if (attnum == InvalidAttrNumber)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_COLUMN),
									 errmsg("column \"%s\" of relation \"%s\" does not exist",
											cmd->name, RelationGetRelationName(rel))));

						if (attnum > 0 &&
							TupleDescAttr(tupdesc, attnum - 1)->attidentity)
						{
							Oid			seq_relid = getIdentitySequence(rel, attnum, false);
							Oid			typeOid = typenameTypeId(pstate, def->typeName);
							AlterSeqStmt *altseqstmt = makeNode(AlterSeqStmt);

							altseqstmt->sequence
								= makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
											   get_rel_name(seq_relid),
											   -1);
							altseqstmt->options = list_make1(makeDefElem("as",
																		 (Node *) makeTypeNameFromOid(typeOid, -1),
																		 -1));
							altseqstmt->for_identity = true;
							cxt.blist = lappend(cxt.blist, altseqstmt);
						}
					}

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AddIdentity:
				{
					Constraint *def = castNode(Constraint, cmd->def);
					ColumnDef  *newdef = makeNode(ColumnDef);
					AttrNumber	attnum;

					newdef->colname = cmd->name;
					newdef->identity = def->generated_when;
					cmd->def = (Node *) newdef;

					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					generateSerialExtraStmts(&cxt, newdef,
											 get_atttype(relid, attnum),
											 def->options, true, true,
											 NULL, NULL);

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_SetIdentity:
				{
					/*
					 * Create an ALTER SEQUENCE statement for the internal
					 * sequence of the identity column.
					 */
					ListCell   *lc;
					List	   *newseqopts = NIL;
					List	   *newdef = NIL;
					AttrNumber	attnum;
					Oid			seq_relid;

					/*
					 * Split options into those handled by ALTER SEQUENCE and
					 * those for ALTER TABLE proper.
					 */
					foreach(lc, castNode(List, cmd->def))
					{
						DefElem    *def = lfirst_node(DefElem, lc);

						if (strcmp(def->defname, "generated") == 0)
							newdef = lappend(newdef, def);
						else
							newseqopts = lappend(newseqopts, def);
					}

					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					seq_relid = getIdentitySequence(rel, attnum, true);

					if (seq_relid)
					{
						AlterSeqStmt *seqstmt;

						seqstmt = makeNode(AlterSeqStmt);
						seqstmt->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
														 get_rel_name(seq_relid), -1);
						seqstmt->options = newseqopts;
						seqstmt->for_identity = true;
						seqstmt->missing_ok = false;

						cxt.blist = lappend(cxt.blist, seqstmt);
					}

					/*
					 * If column was not an identity column, we just let the
					 * ALTER TABLE command error out later.  (There are cases
					 * this fails to cover, but we'll need to restructure
					 * where creation of the sequence dependency linkage
					 * happens before we can fix it.)
					 */

					cmd->def = (Node *) newdef;
					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AttachPartition:
			case AT_DetachPartition:
				{
					PartitionCmd *partcmd = (PartitionCmd *) cmd->def;

					transformPartitionCmd(&cxt, partcmd);
					/* assign transformed value of the partition bound */
					partcmd->bound = cxt.partbound;
				}

				newcmds = lappend(newcmds, cmd);
				break;

			default:

				/*
				 * Currently, we shouldn't actually get here for subcommand
				 * types that don't require transformation; but if we do, just
				 * emit them unchanged.
				 */
				newcmds = lappend(newcmds, cmd);
				break;
		}
	}

	/*
	 * Transfer anything we already have in cxt.alist into save_alist, to keep
	 * it separate from the output of transformIndexConstraints.
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	/* Postprocess constraints */
	transformIndexConstraints(&cxt);
	transformFKConstraints(&cxt, skipValidation, true);
	transformCheckConstraints(&cxt, false);

	/*
	 * Push any index-creation commands into the ALTER, so that they can be
	 * scheduled nicely by tablecmds.c.  Note that tablecmds.c assumes that
	 * the IndexStmt attached to an AT_AddIndex or AT_AddIndexConstraint
	 * subcommand has already been through transformIndexStmt.
	 */
	foreach(l, cxt.alist)
	{
		Node	   *istmt = (Node *) lfirst(l);

		/*
		 * We assume here that cxt.alist contains only IndexStmts generated
		 * from primary key constraints.
		 */
		if (IsA(istmt, IndexStmt))
		{
			IndexStmt  *idxstmt = (IndexStmt *) istmt;

			idxstmt = transformIndexStmt(relid, idxstmt, queryString);
			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = OidIsValid(idxstmt->indexOid) ? AT_AddIndexConstraint : AT_AddIndex;
			newcmd->def = (Node *) idxstmt;
			newcmds = lappend(newcmds, newcmd);
		}
		else
			elog(ERROR, "unexpected stmt type %d", (int) nodeTag(istmt));
	}
	cxt.alist = NIL;

	/* Append any CHECK, NOT NULL or FK constraints to the commands list */
	foreach_node(Constraint, def, cxt.ckconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) def;
		newcmds = lappend(newcmds, newcmd);
	}
	foreach_node(Constraint, def, cxt.nnconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) def;
		newcmds = lappend(newcmds, newcmd);
	}
	foreach_node(Constraint, def, cxt.fkconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) def;
		newcmds = lappend(newcmds, newcmd);
	}

	/* Close rel */
	relation_close(rel, NoLock);

	/*
	 * Output results.
	 */
	stmt->cmds = newcmds;

	*beforeStmts = cxt.blist;
	*afterStmts = list_concat(cxt.alist, save_alist);

	return stmt;
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
	bool		saw_enforced = false;
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

			case CONSTR_ATTR_ENFORCED:
				if (lastprimarycon == NULL ||
					(lastprimarycon->contype != CONSTR_CHECK &&
					 lastprimarycon->contype != CONSTR_FOREIGN))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced ENFORCED clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_enforced)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple ENFORCED/NOT ENFORCED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_enforced = true;
				lastprimarycon->is_enforced = true;
				break;

			case CONSTR_ATTR_NOT_ENFORCED:
				if (lastprimarycon == NULL ||
					(lastprimarycon->contype != CONSTR_CHECK &&
					 lastprimarycon->contype != CONSTR_FOREIGN))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced NOT ENFORCED clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_enforced)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple ENFORCED/NOT ENFORCED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_enforced = true;
				lastprimarycon->is_enforced = false;

				/* A NOT ENFORCED constraint must be marked as invalid. */
				lastprimarycon->skip_validation = true;
				lastprimarycon->initially_valid = false;
				break;

			default:
				/* Otherwise it's not an attribute */
				lastprimarycon = con;
				/* reset flags for new primary node */
				saw_deferrability = false;
				saw_initially = false;
				saw_enforced = false;
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
							format_type_be(typtup->oid)),
					 parser_errposition(cxt->pstate,
										column->collClause->location)));
	}

	ReleaseSysCache(ctype);
}


/*
 * transformCreateSchemaStmtElements -
 *	  analyzes the elements of a CREATE SCHEMA statement
 *
 * Split the schema element list from a CREATE SCHEMA statement into
 * individual commands and place them in the result list in an order
 * such that there are no forward references (e.g. GRANT to a table
 * created later in the list). Note that the logic we use for determining
 * forward references is presently quite incomplete.
 *
 * "schemaName" is the name of the schema that will be used for the creation
 * of the objects listed, that may be compiled from the schema name defined
 * in the statement or a role specification.
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
transformCreateSchemaStmtElements(List *schemaElts, const char *schemaName)
{
	CreateSchemaStmtContext cxt;
	List	   *result;
	ListCell   *elements;

	cxt.schemaname = schemaName;
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
	foreach(elements, schemaElts)
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
setSchemaName(const char *context_schema, char **stmt_schema_name)
{
	if (*stmt_schema_name == NULL)
		*stmt_schema_name = unconstify(char *, context_schema);
	else if (strcmp(context_schema, *stmt_schema_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_SCHEMA_DEFINITION),
				 errmsg("CREATE specifies a schema (%s) "
						"different from the one being created (%s)",
						*stmt_schema_name, context_schema)));
}

/*
 * transformPartitionCmd
 *		Analyze the ATTACH/DETACH PARTITION command
 *
 * In case of the ATTACH PARTITION command, cxt->partbound is set to the
 * transformed value of cmd->bound.
 */
static void
transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd)
{
	Relation	parentRel = cxt->rel;

	switch (parentRel->rd_rel->relkind)
	{
		case RELKIND_PARTITIONED_TABLE:
			/* transform the partition bound, if any */
			Assert(RelationGetPartitionKey(parentRel) != NULL);
			if (cmd->bound != NULL)
				cxt->partbound = transformPartitionBound(cxt->pstate, parentRel,
														 cmd->bound);
			break;
		case RELKIND_PARTITIONED_INDEX:

			/*
			 * A partitioned index cannot have a partition bound set.  ALTER
			 * INDEX prevents that with its grammar, but not ALTER TABLE.
			 */
			if (cmd->bound != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("\"%s\" is not a partitioned table",
								RelationGetRelationName(parentRel))));
			break;
		case RELKIND_RELATION:
			/* the table must be partitioned */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("table \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		case RELKIND_INDEX:
			/* the index must be partitioned */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("index \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		default:
			/* parser shouldn't let this case through */
			elog(ERROR, "\"%s\" is not a partitioned table or index",
				 RelationGetRelationName(parentRel));
			break;
	}
}

/*
 * transformPartitionBound
 *
 * Transform a partition bound specification
 */
PartitionBoundSpec *
transformPartitionBound(ParseState *pstate, Relation parent,
						PartitionBoundSpec *spec)
{
	PartitionBoundSpec *result_spec;
	PartitionKey key = RelationGetPartitionKey(parent);
	char		strategy = get_partition_strategy(key);
	int			partnatts = get_partition_natts(key);
	List	   *partexprs = get_partition_exprs(key);

	/* Avoid scribbling on input */
	result_spec = copyObject(spec);

	if (spec->is_default)
	{
		/*
		 * Hash partitioning does not support a default partition; there's no
		 * use case for it (since the set of partitions to create is perfectly
		 * defined), and if users do get into it accidentally, it's hard to
		 * back out from it afterwards.
		 */
		if (strategy == PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("a hash-partitioned table may not have a default partition")));

		/*
		 * In case of the default partition, parser had no way to identify the
		 * partition strategy. Assign the parent's strategy to the default
		 * partition bound spec.
		 */
		result_spec->strategy = strategy;

		return result_spec;
	}

	if (strategy == PARTITION_STRATEGY_HASH)
	{
		if (spec->strategy != PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a hash partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		if (spec->modulus <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("modulus for hash partition must be an integer value greater than zero")));

		Assert(spec->remainder >= 0);

		if (spec->remainder >= spec->modulus)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("remainder for hash partition must be less than modulus")));
	}
	else if (strategy == PARTITION_STRATEGY_LIST)
	{
		ListCell   *cell;
		char	   *colname;
		Oid			coltype;
		int32		coltypmod;
		Oid			partcollation;

		if (spec->strategy != PARTITION_STRATEGY_LIST)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a list partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		/* Get the only column's name in case we need to output an error */
		if (key->partattrs[0] != 0)
			colname = get_attname(RelationGetRelid(parent),
								  key->partattrs[0], false);
		else
			colname = deparse_expression((Node *) linitial(partexprs),
										 deparse_context_for(RelationGetRelationName(parent),
															 RelationGetRelid(parent)),
										 false, false);
		/* Need its type data too */
		coltype = get_partition_col_typid(key, 0);
		coltypmod = get_partition_col_typmod(key, 0);
		partcollation = get_partition_col_collation(key, 0);

		result_spec->listdatums = NIL;
		foreach(cell, spec->listdatums)
		{
			Node	   *expr = lfirst(cell);
			Const	   *value;
			ListCell   *cell2;
			bool		duplicate;

			value = transformPartitionBoundValue(pstate, expr,
												 colname, coltype, coltypmod,
												 partcollation);

			/* Don't add to the result if the value is a duplicate */
			duplicate = false;
			foreach(cell2, result_spec->listdatums)
			{
				Const	   *value2 = lfirst_node(Const, cell2);

				if (equal(value, value2))
				{
					duplicate = true;
					break;
				}
			}
			if (duplicate)
				continue;

			result_spec->listdatums = lappend(result_spec->listdatums,
											  value);
		}
	}
	else if (strategy == PARTITION_STRATEGY_RANGE)
	{
		if (spec->strategy != PARTITION_STRATEGY_RANGE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a range partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		if (list_length(spec->lowerdatums) != partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("FROM must specify exactly one value per partitioning column")));
		if (list_length(spec->upperdatums) != partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("TO must specify exactly one value per partitioning column")));

		/*
		 * Convert raw parse nodes into PartitionRangeDatum nodes and perform
		 * any necessary validation.
		 */
		result_spec->lowerdatums =
			transformPartitionRangeBounds(pstate, spec->lowerdatums,
										  parent);
		result_spec->upperdatums =
			transformPartitionRangeBounds(pstate, spec->upperdatums,
										  parent);
	}
	else
		elog(ERROR, "unexpected partition strategy: %d", (int) strategy);

	return result_spec;
}

/*
 * transformPartitionRangeBounds
 *		This converts the expressions for range partition bounds from the raw
 *		grammar representation to PartitionRangeDatum structs
 */
static List *
transformPartitionRangeBounds(ParseState *pstate, List *blist,
							  Relation parent)
{
	List	   *result = NIL;
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *partexprs = get_partition_exprs(key);
	ListCell   *lc;
	int			i,
				j;

	i = j = 0;
	foreach(lc, blist)
	{
		Node	   *expr = lfirst(lc);
		PartitionRangeDatum *prd = NULL;

		/*
		 * Infinite range bounds -- "minvalue" and "maxvalue" -- get passed in
		 * as ColumnRefs.
		 */
		if (IsA(expr, ColumnRef))
		{
			ColumnRef  *cref = (ColumnRef *) expr;
			char	   *cname = NULL;

			/*
			 * There should be a single field named either "minvalue" or
			 * "maxvalue".
			 */
			if (list_length(cref->fields) == 1 &&
				IsA(linitial(cref->fields), String))
				cname = strVal(linitial(cref->fields));

			if (cname == NULL)
			{
				/*
				 * ColumnRef is not in the desired single-field-name form. For
				 * consistency between all partition strategies, let the
				 * expression transformation report any errors rather than
				 * doing it ourselves.
				 */
			}
			else if (strcmp("minvalue", cname) == 0)
			{
				prd = makeNode(PartitionRangeDatum);
				prd->kind = PARTITION_RANGE_DATUM_MINVALUE;
				prd->value = NULL;
			}
			else if (strcmp("maxvalue", cname) == 0)
			{
				prd = makeNode(PartitionRangeDatum);
				prd->kind = PARTITION_RANGE_DATUM_MAXVALUE;
				prd->value = NULL;
			}
		}

		if (prd == NULL)
		{
			char	   *colname;
			Oid			coltype;
			int32		coltypmod;
			Oid			partcollation;
			Const	   *value;

			/* Get the column's name in case we need to output an error */
			if (key->partattrs[i] != 0)
				colname = get_attname(RelationGetRelid(parent),
									  key->partattrs[i], false);
			else
			{
				colname = deparse_expression((Node *) list_nth(partexprs, j),
											 deparse_context_for(RelationGetRelationName(parent),
																 RelationGetRelid(parent)),
											 false, false);
				++j;
			}

			/* Need its type data too */
			coltype = get_partition_col_typid(key, i);
			coltypmod = get_partition_col_typmod(key, i);
			partcollation = get_partition_col_collation(key, i);

			value = transformPartitionBoundValue(pstate, expr,
												 colname,
												 coltype, coltypmod,
												 partcollation);
			if (value->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot specify NULL in range bound")));
			prd = makeNode(PartitionRangeDatum);
			prd->kind = PARTITION_RANGE_DATUM_VALUE;
			prd->value = (Node *) value;
			++i;
		}

		prd->location = exprLocation(expr);

		result = lappend(result, prd);
	}

	/*
	 * Once we see MINVALUE or MAXVALUE for one column, the remaining columns
	 * must be the same.
	 */
	validateInfiniteBounds(pstate, result);

	return result;
}

/*
 * validateInfiniteBounds
 *
 * Check that a MAXVALUE or MINVALUE specification in a partition bound is
 * followed only by more of the same.
 */
static void
validateInfiniteBounds(ParseState *pstate, List *blist)
{
	ListCell   *lc;
	PartitionRangeDatumKind kind = PARTITION_RANGE_DATUM_VALUE;

	foreach(lc, blist)
	{
		PartitionRangeDatum *prd = lfirst_node(PartitionRangeDatum, lc);

		if (kind == prd->kind)
			continue;

		switch (kind)
		{
			case PARTITION_RANGE_DATUM_VALUE:
				kind = prd->kind;
				break;

			case PARTITION_RANGE_DATUM_MAXVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MAXVALUE must also be MAXVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));
				break;

			case PARTITION_RANGE_DATUM_MINVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MINVALUE must also be MINVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));
				break;
		}
	}
}

/*
 * Transform one entry in a partition bound spec, producing a constant.
 */
static Const *
transformPartitionBoundValue(ParseState *pstate, Node *val,
							 const char *colName, Oid colType, int32 colTypmod,
							 Oid partCollation)
{
	Node	   *value;

	/* Transform raw parsetree */
	value = transformExpr(pstate, val, EXPR_KIND_PARTITION_BOUND);

	/*
	 * transformExpr() should have already rejected column references,
	 * subqueries, aggregates, window functions, and SRFs, based on the
	 * EXPR_KIND_ of a partition bound expression.
	 */
	Assert(!contain_var_clause(value));

	/*
	 * Coerce to the correct type.  This might cause an explicit coercion step
	 * to be added on top of the expression, which must be evaluated before
	 * returning the result to the caller.
	 */
	value = coerce_to_target_type(pstate,
								  value, exprType(value),
								  colType,
								  colTypmod,
								  COERCION_ASSIGNMENT,
								  COERCE_IMPLICIT_CAST,
								  -1);

	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("specified value cannot be cast to type %s for column \"%s\"",
						format_type_be(colType), colName),
				 parser_errposition(pstate, exprLocation(val))));

	/*
	 * Evaluate the expression, if needed, assigning the partition key's data
	 * type and collation to the resulting Const node.
	 */
	if (!IsA(value, Const))
	{
		assign_expr_collations(pstate, value);
		value = (Node *) expression_planner((Expr *) value);
		value = (Node *) evaluate_expr((Expr *) value, colType, colTypmod,
									   partCollation);
		if (!IsA(value, Const))
			elog(ERROR, "could not evaluate partition bound expression");
	}
	else
	{
		/*
		 * If the expression is already a Const, as is often the case, we can
		 * skip the rather expensive steps above.  But we still have to insert
		 * the right collation, since coerce_to_target_type doesn't handle
		 * that.
		 */
		((Const *) value)->constcollid = partCollation;
	}

	/*
	 * Attach original expression's parse location to the Const, so that
	 * that's what will be reported for any later errors related to this
	 * partition bound.
	 */
	((Const *) value)->location = exprLocation(val);

	return (Const *) value;
}
