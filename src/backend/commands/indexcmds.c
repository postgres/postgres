/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/indexcmds.c,v 1.149.2.2 2007/09/10 22:02:05 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static void CheckPredicate(Expr *predicate);
static void ComputeIndexAttrs(IndexInfo *indexInfo, Oid *classOidP,
				  List *attList,
				  Oid relId,
				  char *accessMethodName, Oid accessMethodId,
				  bool isconstraint);
static Oid GetIndexOpClass(List *opclass, Oid attrType,
				char *accessMethodName, Oid accessMethodId);
static bool relationHasPrimaryKey(Relation rel);


/*
 * DefineIndex
 *		Creates a new index.
 *
 * 'heapRelation': the relation the index will apply to.
 * 'indexRelationName': the name for the new index, or NULL to indicate
 *		that a nonconflicting default name should be picked.
 * 'indexRelationId': normally InvalidOid, but during bootstrap can be
 *		nonzero to specify a preselected OID for the index.
 * 'accessMethodName': name of the AM to use.
 * 'tableSpaceName': name of the tablespace to create the index in.
 *		NULL specifies using the appropriate default.
 * 'attributeList': a list of IndexElem specifying columns and expressions
 *		to index on.
 * 'predicate': the partial-index condition, or NULL if none.
 * 'rangetable': needed to interpret the predicate.
 * 'options': reloptions from WITH (in list-of-DefElem form).
 * 'unique': make the index enforce uniqueness.
 * 'primary': mark the index as a primary key in the catalogs.
 * 'isconstraint': index is for a PRIMARY KEY or UNIQUE constraint,
 *		so build a pg_constraint entry for it.
 * 'is_alter_table': this is due to an ALTER rather than a CREATE operation.
 * 'check_rights': check for CREATE rights in the namespace.  (This should
 *		be true except when ALTER is deleting/recreating an index.)
 * 'skip_build': make the catalog entries but leave the index file empty;
 *		it will be filled later.
 * 'quiet': suppress the NOTICE chatter ordinarily provided for constraints.
 * 'concurrent': avoid blocking writers to the table while building.
 */
void
DefineIndex(RangeVar *heapRelation,
			char *indexRelationName,
			Oid indexRelationId,
			char *accessMethodName,
			char *tableSpaceName,
			List *attributeList,
			Expr *predicate,
			List *rangetable,
			List *options,
			bool unique,
			bool primary,
			bool isconstraint,
			bool is_alter_table,
			bool check_rights,
			bool skip_build,
			bool quiet,
			bool concurrent)
{
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
	Oid			namespaceId;
	Oid			tablespaceId;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_am	accessMethodForm;
	RegProcedure amoptions;
	Datum		reloptions;
	IndexInfo  *indexInfo;
	int			numberOfAttributes;
	List	   *old_xact_list;
	ListCell   *lc;
	uint32		ixcnt;
	LockRelId	heaprelid;
	LOCKTAG		heaplocktag;
	Snapshot	snapshot;
	Relation	pg_index;
	HeapTuple	indexTuple;
	Form_pg_index indexForm;

	/*
	 * count attributes in index
	 */
	numberOfAttributes = list_length(attributeList);
	if (numberOfAttributes <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("must specify at least one column")));
	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d columns in an index",
						INDEX_MAX_KEYS)));

	/*
	 * Open heap relation, acquire a suitable lock on it, remember its OID
	 *
	 * Only SELECT ... FOR UPDATE/SHARE are allowed while doing a standard
	 * index build; but for concurrent builds we allow INSERT/UPDATE/DELETE
	 * (but not VACUUM).
	 */
	rel = heap_openrv(heapRelation,
					  (concurrent ? ShareUpdateExclusiveLock : ShareLock));

	relationId = RelationGetRelid(rel);
	namespaceId = RelationGetNamespace(rel);

	/* Note: during bootstrap may see uncataloged relation */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_UNCATALOGED)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						heapRelation->relname)));

	/*
	 * Don't try to CREATE INDEX on temp tables of other backends.
	 */
	if (isOtherTempNamespace(namespaceId))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot create indexes on temporary tables of other sessions")));

	/*
	 * Verify we (still) have CREATE rights in the rel's namespace.
	 * (Presumably we did when the rel was created, but maybe not anymore.)
	 * Skip check if caller doesn't want it.  Also skip check if
	 * bootstrapping, since permissions machinery may not be working yet.
	 */
	if (check_rights && !IsBootstrapProcessingMode())
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
	if (tableSpaceName)
	{
		tablespaceId = get_tablespace_oid(tableSpaceName);
		if (!OidIsValid(tablespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							tableSpaceName)));
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
	 * Force shared indexes into the pg_global tablespace.	This is a bit of a
	 * hack but seems simpler than marking them in the BKI commands.
	 */
	if (rel->rd_rel->relisshared)
		tablespaceId = GLOBALTABLESPACE_OID;

	/*
	 * Select name for index if caller didn't specify
	 */
	if (indexRelationName == NULL)
	{
		if (primary)
			indexRelationName = ChooseRelationName(RelationGetRelationName(rel),
												   NULL,
												   "pkey",
												   namespaceId);
		else
		{
			IndexElem  *iparam = (IndexElem *) linitial(attributeList);

			indexRelationName = ChooseRelationName(RelationGetRelationName(rel),
												   iparam->name,
												   "key",
												   namespaceId);
		}
	}

	/*
	 * look up the access method, verify it can handle the requested features
	 */
	tuple = SearchSysCache(AMNAME,
						   PointerGetDatum(accessMethodName),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		/*
		 * Hack to provide more-or-less-transparent updating of old RTREE
		 * indexes to GIST: if RTREE is requested and not found, use GIST.
		 */
		if (strcmp(accessMethodName, "rtree") == 0)
		{
			ereport(NOTICE,
					(errmsg("substituting access method \"gist\" for obsolete method \"rtree\"")));
			accessMethodName = "gist";
			tuple = SearchSysCache(AMNAME,
								   PointerGetDatum(accessMethodName),
								   0, 0, 0);
		}

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("access method \"%s\" does not exist",
							accessMethodName)));
	}
	accessMethodId = HeapTupleGetOid(tuple);
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);

	if (unique && !accessMethodForm->amcanunique)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("access method \"%s\" does not support unique indexes",
					  accessMethodName)));
	if (numberOfAttributes > 1 && !accessMethodForm->amcanmulticol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		  errmsg("access method \"%s\" does not support multicolumn indexes",
				 accessMethodName)));

	amoptions = accessMethodForm->amoptions;

	ReleaseSysCache(tuple);

	/*
	 * If a range table was created then check that only the base rel is
	 * mentioned.
	 */
	if (rangetable != NIL)
	{
		if (list_length(rangetable) != 1 || getrelid(1, rangetable) != relationId)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("index expressions and predicates may refer only to the table being indexed")));
	}

	/*
	 * Validate predicate, if given
	 */
	if (predicate)
		CheckPredicate(predicate);

	/*
	 * Extra checks when creating a PRIMARY KEY index.
	 */
	if (primary)
	{
		List	   *cmds;
		ListCell   *keys;

		/*
		 * If ALTER TABLE, check that there isn't already a PRIMARY KEY. In
		 * CREATE TABLE, we have faith that the parser rejected multiple pkey
		 * clauses; and CREATE INDEX doesn't have a way to say PRIMARY KEY, so
		 * it's no problem either.
		 */
		if (is_alter_table &&
			relationHasPrimaryKey(rel))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
			 errmsg("multiple primary keys for table \"%s\" are not allowed",
					RelationGetRelationName(rel))));
		}

		/*
		 * Check that all of the attributes in a primary key are marked as not
		 * null, otherwise attempt to ALTER TABLE .. SET NOT NULL
		 */
		cmds = NIL;
		foreach(keys, attributeList)
		{
			IndexElem  *key = (IndexElem *) lfirst(keys);
			HeapTuple	atttuple;

			if (!key->name)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("primary keys cannot be expressions")));

			/* System attributes are never null, so no problem */
			if (SystemAttributeByName(key->name, rel->rd_rel->relhasoids))
				continue;

			atttuple = SearchSysCacheAttName(relationId, key->name);
			if (HeapTupleIsValid(atttuple))
			{
				if (!((Form_pg_attribute) GETSTRUCT(atttuple))->attnotnull)
				{
					/* Add a subcommand to make this one NOT NULL */
					AlterTableCmd *cmd = makeNode(AlterTableCmd);

					cmd->subtype = AT_SetNotNull;
					cmd->name = key->name;

					cmds = lappend(cmds, cmd);
				}
				ReleaseSysCache(atttuple);
			}
			else
			{
				/*
				 * This shouldn't happen during CREATE TABLE, but can happen
				 * during ALTER TABLE.	Keep message in sync with
				 * transformIndexConstraints() in parser/analyze.c.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in key does not exist",
								key->name)));
			}
		}

		/*
		 * XXX: Shouldn't the ALTER TABLE .. SET NOT NULL cascade to child
		 * tables?	Currently, since the PRIMARY KEY itself doesn't cascade,
		 * we don't cascade the notnull constraint(s) either; but this is
		 * pretty debatable.
		 *
		 * XXX: possible future improvement: when being called from ALTER
		 * TABLE, it would be more efficient to merge this with the outer
		 * ALTER TABLE, so as to avoid two scans.  But that seems to
		 * complicate DefineIndex's API unduly.
		 */
		if (cmds)
			AlterTableInternal(relationId, cmds, false);
	}

	/*
	 * Parse AM-specific options, convert to text array form, validate.
	 */
	reloptions = transformRelOptions((Datum) 0, options, false, false);

	(void) index_reloptions(amoptions, reloptions, true);

	/*
	 * Prepare arguments for index_create, primarily an IndexInfo structure.
	 * Note that ii_Predicate must be in implicit-AND format.
	 */
	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = numberOfAttributes;
	indexInfo->ii_Expressions = NIL;	/* for now */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_Predicate = make_ands_implicit(predicate);
	indexInfo->ii_PredicateState = NIL;
	indexInfo->ii_Unique = unique;
	indexInfo->ii_Concurrent = concurrent;

	classObjectId = (Oid *) palloc(numberOfAttributes * sizeof(Oid));
	ComputeIndexAttrs(indexInfo, classObjectId, attributeList,
					  relationId, accessMethodName, accessMethodId,
					  isconstraint);

	/*
	 * Report index creation if appropriate (delay this till after most of the
	 * error checks)
	 */
	if (isconstraint && !quiet)
		ereport(NOTICE,
		  (errmsg("%s %s will create implicit index \"%s\" for table \"%s\"",
				  is_alter_table ? "ALTER TABLE / ADD" : "CREATE TABLE /",
				  primary ? "PRIMARY KEY" : "UNIQUE",
				  indexRelationName, RelationGetRelationName(rel))));

	/* save lockrelid for below, then close rel */
	heaprelid = rel->rd_lockInfo.lockRelId;
	heap_close(rel, NoLock);

	indexRelationId =
		index_create(relationId, indexRelationName, indexRelationId,
					 indexInfo, accessMethodId, tablespaceId, classObjectId,
					 reloptions, primary, isconstraint,
					 allowSystemTableMods, skip_build, concurrent);

	if (!concurrent)
		return;					/* We're done, in the standard case */

	/*
	 * Phase 2 of concurrent index build (see comments for validate_index()
	 * for an overview of how this works)
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

	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * Now we must wait until no running transaction could have the table open
	 * with the old list of indexes.  To do this, inquire which xacts
	 * currently would conflict with ShareLock on the table -- ie, which ones
	 * have a lock that permits writing the table.	Then wait for each of
	 * these xacts to commit or abort.	Note we do not need to worry about
	 * xacts that open the table for writing after this point; they will see
	 * the new index when they open it.
	 *
	 * Note: GetLockConflicts() never reports our own xid, hence we need not
	 * check for that.
	 */
	SET_LOCKTAG_RELATION(heaplocktag, heaprelid.dbId, heaprelid.relId);
	old_xact_list = GetLockConflicts(&heaplocktag, ShareLock);

	foreach(lc, old_xact_list)
	{
		TransactionId xid = lfirst_xid(lc);

		XactLockTableWait(xid);
	}

	/*
	 * Now take the "reference snapshot" that will be used by validate_index()
	 * to filter candidate tuples.	All other transactions running at this
	 * time will have to be out-waited before we can commit, because we can't
	 * guarantee that tuples deleted just before this will be in the index.
	 *
	 * We also set ActiveSnapshot to this snap, since functions in indexes may
	 * need a snapshot.
	 */
	snapshot = CopySnapshot(GetTransactionSnapshot());
	ActiveSnapshot = snapshot;

	/*
	 * Scan the index and the heap, insert any missing index entries.
	 */
	validate_index(relationId, indexRelationId, snapshot);

	/*
	 * The index is now valid in the sense that it contains all currently
	 * interesting tuples.	But since it might not contain tuples deleted just
	 * before the reference snap was taken, we have to wait out any
	 * transactions older than the reference snap.	We can do this by waiting
	 * for each xact explicitly listed in the snap.
	 *
	 * Note: GetSnapshotData() never stores our own xid into a snap, hence we
	 * need not check for that.
	 */
	for (ixcnt = 0; ixcnt < snapshot->xcnt; ixcnt++)
		XactLockTableWait(snapshot->xip[ixcnt]);

	/* Index can now be marked valid -- update its pg_index entry */
	pg_index = heap_open(IndexRelationId, RowExclusiveLock);

	indexTuple = SearchSysCacheCopy(INDEXRELID,
									ObjectIdGetDatum(indexRelationId),
									0, 0, 0);
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "cache lookup failed for index %u", indexRelationId);
	indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

	Assert(indexForm->indexrelid = indexRelationId);
	Assert(!indexForm->indisvalid);

	indexForm->indisvalid = true;

	simple_heap_update(pg_index, &indexTuple->t_self, indexTuple);
	CatalogUpdateIndexes(pg_index, indexTuple);

	heap_close(pg_index, RowExclusiveLock);

	/*
	 * Last thing to do is release the session-level lock on the parent table.
	 */
	UnlockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);
}


/*
 * CheckPredicate
 *		Checks that the given partial-index predicate is valid.
 *
 * This used to also constrain the form of the predicate to forms that
 * indxpath.c could do something with.	However, that seems overly
 * restrictive.  One useful application of partial indexes is to apply
 * a UNIQUE constraint across a subset of a table, and in that scenario
 * any evaluatable predicate will work.  So accept any predicate here
 * (except ones requiring a plan), and let indxpath.c fend for itself.
 */
static void
CheckPredicate(Expr *predicate)
{
	/*
	 * We don't currently support generation of an actual query plan for a
	 * predicate, only simple scalar expressions; hence these restrictions.
	 */
	if (contain_subplans((Node *) predicate))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use subquery in index predicate")));
	if (contain_agg_clause((Node *) predicate))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate in index predicate")));

	/*
	 * A predicate using mutable functions is probably wrong, for the same
	 * reasons that we don't allow an index expression to use one.
	 */
	if (contain_mutable_functions((Node *) predicate))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
		   errmsg("functions in index predicate must be marked IMMUTABLE")));
}

static void
ComputeIndexAttrs(IndexInfo *indexInfo,
				  Oid *classOidP,
				  List *attList,	/* list of IndexElem's */
				  Oid relId,
				  char *accessMethodName,
				  Oid accessMethodId,
				  bool isconstraint)
{
	ListCell   *rest;
	int			attn = 0;

	/*
	 * process attributeList
	 */
	foreach(rest, attList)
	{
		IndexElem  *attribute = (IndexElem *) lfirst(rest);
		Oid			atttype;

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
			indexInfo->ii_KeyAttrNumbers[attn] = attform->attnum;
			atttype = attform->atttypid;
			ReleaseSysCache(atttuple);
		}
		else if (attribute->expr && IsA(attribute->expr, Var))
		{
			/* Tricky tricky, he wrote (column) ... treat as simple attr */
			Var		   *var = (Var *) attribute->expr;

			indexInfo->ii_KeyAttrNumbers[attn] = var->varattno;
			atttype = get_atttype(relId, var->varattno);
		}
		else
		{
			/* Index expression */
			Assert(attribute->expr != NULL);
			indexInfo->ii_KeyAttrNumbers[attn] = 0;		/* marks expression */
			indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions,
												attribute->expr);
			atttype = exprType(attribute->expr);

			/*
			 * We don't currently support generation of an actual query plan
			 * for an index expression, only simple scalar expressions; hence
			 * these restrictions.
			 */
			if (contain_subplans(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot use subquery in index expression")));
			if (contain_agg_clause(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_GROUPING_ERROR),
				errmsg("cannot use aggregate function in index expression")));

			/*
			 * A expression using mutable functions is probably wrong, since
			 * if you aren't going to get the same result for the same data
			 * every time, it's not clear what the index entries mean at all.
			 */
			if (contain_mutable_functions(attribute->expr))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("functions in index expression must be marked IMMUTABLE")));
		}

		classOidP[attn] = GetIndexOpClass(attribute->opclass,
										  atttype,
										  accessMethodName,
										  accessMethodId);
		attn++;
	}
}

/*
 * Resolve possibly-defaulted operator class specification
 */
static Oid
GetIndexOpClass(List *opclass, Oid attrType,
				char *accessMethodName, Oid accessMethodId)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	Oid			opClassId,
				opInputType;

	/*
	 * Release 7.0 removed network_ops, timespan_ops, and datetime_ops, so we
	 * ignore those opclass names so the default *_ops is used.  This can be
	 * removed in some later release.  bjm 2000/02/07
	 *
	 * Release 7.1 removes lztext_ops, so suppress that too for a while.  tgl
	 * 2000/07/30
	 *
	 * Release 7.2 renames timestamp_ops to timestamptz_ops, so suppress that
	 * too for awhile.	I'm starting to think we need a better approach. tgl
	 * 2000/10/01
	 *
	 * Release 8.0 removes bigbox_ops (which was dead code for a long while
	 * anyway).  tgl 2003/11/11
	 */
	if (list_length(opclass) == 1)
	{
		char	   *claname = strVal(linitial(opclass));

		if (strcmp(claname, "network_ops") == 0 ||
			strcmp(claname, "timespan_ops") == 0 ||
			strcmp(claname, "datetime_ops") == 0 ||
			strcmp(claname, "lztext_ops") == 0 ||
			strcmp(claname, "timestamp_ops") == 0 ||
			strcmp(claname, "bigbox_ops") == 0)
			opclass = NIL;
	}

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

		namespaceId = LookupExplicitNamespace(schemaname);
		tuple = SearchSysCache(CLAAMNAMENSP,
							   ObjectIdGetDatum(accessMethodId),
							   PointerGetDatum(opcname),
							   ObjectIdGetDatum(namespaceId),
							   0);
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
		tuple = SearchSysCache(CLAOID,
							   ObjectIdGetDatum(opClassId),
							   0, 0, 0);
	}

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclass), accessMethodName)));

	/*
	 * Verify that the index operator class accepts this datatype.	Note we
	 * will accept binary compatibility.
	 */
	opClassId = HeapTupleGetOid(tuple);
	opInputType = ((Form_pg_opclass) GETSTRUCT(tuple))->opcintype;

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
 * operator class, if any.	Returns InvalidOid if there is none.
 */
Oid
GetDefaultOpClass(Oid type_id, Oid am_id)
{
	int			nexact = 0;
	int			ncompatible = 0;
	Oid			exactOid = InvalidOid;
	Oid			compatibleOid = InvalidOid;
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;

	/* If it's a domain, look at the base type instead */
	type_id = getBaseType(type_id);

	/*
	 * We scan through all the opclasses available for the access method,
	 * looking for one that is marked default and matches the target type
	 * (either exactly or binary-compatibly, but prefer an exact match).
	 *
	 * We could find more than one binary-compatible match, in which case we
	 * require the user to specify which one he wants.	If we find more than
	 * one exact match, then someone put bogus entries in pg_opclass.
	 */
	rel = heap_open(OperatorClassRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_opclass_opcamid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(am_id));

	scan = systable_beginscan(rel, OpclassAmNameNspIndexId, true,
							  SnapshotNow, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

		if (opclass->opcdefault)
		{
			if (opclass->opcintype == type_id)
			{
				nexact++;
				exactOid = HeapTupleGetOid(tup);
			}
			else if (IsBinaryCoercible(type_id, opclass->opcintype))
			{
				ncompatible++;
				compatibleOid = HeapTupleGetOid(tup);
			}
		}
	}

	systable_endscan(scan);

	heap_close(rel, AccessShareLock);

	if (nexact == 1)
		return exactOid;
	if (nexact != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
		errmsg("there are multiple default operator classes for data type %s",
			   format_type_be(type_id))));
	if (ncompatible == 1)
		return compatibleOid;

	return InvalidOid;
}

/*
 *	makeObjectName()
 *
 *	Create a name for an implicitly created index, sequence, constraint, etc.
 *
 *	The parameters are typically: the original table name, the original field
 *	name, and a "type" string (such as "seq" or "pkey").	The field name
 *	and/or type can be NULL if not relevant.
 *
 *	The result is a palloc'd string.
 *
 *	The basic result we want is "name1_name2_label", omitting "_name2" or
 *	"_label" when those parameters are NULL.  However, we must generate
 *	a name with less than NAMEDATALEN characters!  So, we truncate one or
 *	both names if necessary to make a short-enough string.	The label part
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
	 * If we must truncate,  preferentially truncate the longer name. This
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
				   const char *label, Oid namespace)
{
	int			pass = 0;
	char	   *relname = NULL;
	char		modlabel[NAMEDATALEN];

	/* try the unmodified label first */
	StrNCpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		relname = makeObjectName(name1, name2, modlabel);

		if (!OidIsValid(get_relname_relid(relname, namespace)))
			break;

		/* found a conflict, so try a new name component */
		pfree(relname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	return relname;
}

/*
 * relationHasPrimaryKey -
 *
 *	See whether an existing relation has a primary key.
 */
static bool
relationHasPrimaryKey(Relation rel)
{
	bool		result = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).
	 */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexoid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))		/* should not happen */
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		result = ((Form_pg_index) GETSTRUCT(indexTuple))->indisprimary;
		ReleaseSysCache(indexTuple);
		if (result)
			break;
	}

	list_free(indexoidlist);

	return result;
}


/*
 * RemoveIndex
 *		Deletes an index.
 */
void
RemoveIndex(RangeVar *relation, DropBehavior behavior)
{
	Oid			indOid;
	char		relkind;
	ObjectAddress object;

	indOid = RangeVarGetRelid(relation, false);
	relkind = get_rel_relkind(indOid);
	if (relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						relation->relname)));

	object.classId = RelationRelationId;
	object.objectId = indOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * ReindexIndex
 *		Recreate a specific index.
 */
void
ReindexIndex(RangeVar *indexRelation)
{
	Oid			indOid;
	HeapTuple	tuple;

	indOid = RangeVarGetRelid(indexRelation, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(indOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", indOid);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						indexRelation->relname)));

	/* Check permissions */
	if (!pg_class_ownercheck(indOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   indexRelation->relname);

	ReleaseSysCache(tuple);

	reindex_index(indOid);
}

/*
 * ReindexTable
 *		Recreate all indexes of a table (and of its toast table, if any)
 */
void
ReindexTable(RangeVar *relation)
{
	Oid			heapOid;
	HeapTuple	tuple;

	heapOid = RangeVarGetRelid(relation, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(heapOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))		/* shouldn't happen */
		elog(ERROR, "cache lookup failed for relation %u", heapOid);

	if (((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_RELATION &&
		((Form_pg_class) GETSTRUCT(tuple))->relkind != RELKIND_TOASTVALUE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						relation->relname)));

	/* Check permissions */
	if (!pg_class_ownercheck(heapOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   relation->relname);

	/* Can't reindex shared tables except in standalone mode */
	if (((Form_pg_class) GETSTRUCT(tuple))->relisshared && IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("shared table \"%s\" can only be reindexed in stand-alone mode",
						relation->relname)));

	ReleaseSysCache(tuple);

	if (!reindex_relation(heapOid, true))
		ereport(NOTICE,
				(errmsg("table \"%s\" has no indexes",
						relation->relname)));
}

/*
 * ReindexDatabase
 *		Recreate indexes of a database.
 *
 * To reduce the probability of deadlocks, each table is reindexed in a
 * separate transaction, so we can release the lock on it right away.
 */
void
ReindexDatabase(const char *databaseName, bool do_system, bool do_user)
{
	Relation	relationRelation;
	HeapScanDesc scan;
	HeapTuple	tuple;
	MemoryContext private_context;
	MemoryContext old;
	List	   *relids = NIL;
	ListCell   *l;

	AssertArg(databaseName);

	if (strcmp(databaseName, get_database_name(MyDatabaseId)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("can only reindex the currently open database")));

	if (!pg_database_ownercheck(MyDatabaseId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   databaseName);

	/*
	 * We cannot run inside a user transaction block; if we were inside a
	 * transaction, then our commit- and start-transaction-command calls would
	 * not have the intended effect!
	 */
	PreventTransactionChain((void *) databaseName, "REINDEX DATABASE");

	/*
	 * Create a memory context that will survive forced transaction commits we
	 * do below.  Since it is a child of PortalContext, it will go away
	 * eventually even if we suffer an error; there's no need for special
	 * abort cleanup logic.
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexDatabase",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * We always want to reindex pg_class first.  This ensures that if there
	 * is any corruption in pg_class' indexes, they will be fixed before we
	 * process any other tables.  This is critical because reindexing itself
	 * will try to update pg_class.
	 */
	if (do_system)
	{
		old = MemoryContextSwitchTo(private_context);
		relids = lappend_oid(relids, RelationRelationId);
		MemoryContextSwitchTo(old);
	}

	/*
	 * Scan pg_class to build a list of the relations we need to reindex.
	 *
	 * We only consider plain relations here (toast rels will be processed
	 * indirectly by reindex_relation).
	 */
	relationRelation = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan(relationRelation, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classtuple = (Form_pg_class) GETSTRUCT(tuple);

		if (classtuple->relkind != RELKIND_RELATION)
			continue;

		/* Skip temp tables of other backends; we can't reindex them at all */
		if (isOtherTempNamespace(classtuple->relnamespace))
			continue;

		/* Check user/system classification, and optionally skip */
		if (IsSystemClass(classtuple))
		{
			if (!do_system)
				continue;
		}
		else
		{
			if (!do_user)
				continue;
		}

		if (IsUnderPostmaster)	/* silently ignore shared tables */
		{
			if (classtuple->relisshared)
				continue;
		}

		if (HeapTupleGetOid(tuple) == RelationRelationId)
			continue;			/* got it already */

		old = MemoryContextSwitchTo(private_context);
		relids = lappend_oid(relids, HeapTupleGetOid(tuple));
		MemoryContextSwitchTo(old);
	}
	heap_endscan(scan);
	heap_close(relationRelation, AccessShareLock);

	/* Now reindex each rel in a separate transaction */
	CommitTransactionCommand();
	foreach(l, relids)
	{
		Oid			relid = lfirst_oid(l);

		StartTransactionCommand();
		/* functions in indexes may want a snapshot set */
		ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
		if (reindex_relation(relid, true))
			ereport(NOTICE,
					(errmsg("table \"%s\" was reindexed",
							get_rel_name(relid))));
		CommitTransactionCommand();
	}
	StartTransactionCommand();

	MemoryContextDelete(private_context);
}
