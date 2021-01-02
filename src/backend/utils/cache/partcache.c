/*-------------------------------------------------------------------------
 *
 * partcache.c
 *		Support routines for manipulating partition information cached in
 *		relcache
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/utils/cache/partcache.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_partitioned_table.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "partitioning/partbounds.h"
#include "rewrite/rewriteHandler.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static void RelationBuildPartitionKey(Relation relation);
static List *generate_partition_qual(Relation rel);

/*
 * RelationGetPartitionKey -- get partition key, if relation is partitioned
 *
 * Note: partition keys are not allowed to change after the partitioned rel
 * is created.  RelationClearRelation knows this and preserves rd_partkey
 * across relcache rebuilds, as long as the relation is open.  Therefore,
 * even though we hand back a direct pointer into the relcache entry, it's
 * safe for callers to continue to use that pointer as long as they hold
 * the relation open.
 */
PartitionKey
RelationGetPartitionKey(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return NULL;

	if (unlikely(rel->rd_partkey == NULL))
		RelationBuildPartitionKey(rel);

	return rel->rd_partkey;
}

/*
 * RelationBuildPartitionKey
 *		Build partition key data of relation, and attach to relcache
 *
 * Partitioning key data is a complex structure; to avoid complicated logic to
 * free individual elements whenever the relcache entry is flushed, we give it
 * its own memory context, a child of CacheMemoryContext, which can easily be
 * deleted on its own.  To avoid leaking memory in that context in case of an
 * error partway through this function, the context is initially created as a
 * child of CurTransactionContext and only re-parented to CacheMemoryContext
 * at the end, when no further errors are possible.  Also, we don't make this
 * context the current context except in very brief code sections, out of fear
 * that some of our callees allocate memory on their own which would be leaked
 * permanently.
 */
static void
RelationBuildPartitionKey(Relation relation)
{
	Form_pg_partitioned_table form;
	HeapTuple	tuple;
	bool		isnull;
	int			i;
	PartitionKey key;
	AttrNumber *attrs;
	oidvector  *opclass;
	oidvector  *collation;
	ListCell   *partexprs_item;
	Datum		datum;
	MemoryContext partkeycxt,
				oldcxt;
	int16		procnum;

	tuple = SearchSysCache1(PARTRELID,
							ObjectIdGetDatum(RelationGetRelid(relation)));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 RelationGetRelid(relation));

	partkeycxt = AllocSetContextCreate(CurTransactionContext,
									   "partition key",
									   ALLOCSET_SMALL_SIZES);
	MemoryContextCopyAndSetIdentifier(partkeycxt,
									  RelationGetRelationName(relation));

	key = (PartitionKey) MemoryContextAllocZero(partkeycxt,
												sizeof(PartitionKeyData));

	/* Fixed-length attributes */
	form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
	key->strategy = form->partstrat;
	key->partnatts = form->partnatts;

	/*
	 * We can rely on the first variable-length attribute being mapped to the
	 * relevant field of the catalog's C struct, because all previous
	 * attributes are non-nullable and fixed-length.
	 */
	attrs = form->partattrs.values;

	/* But use the hard way to retrieve further variable-length attributes */
	/* Operator class */
	datum = SysCacheGetAttr(PARTRELID, tuple,
							Anum_pg_partitioned_table_partclass, &isnull);
	Assert(!isnull);
	opclass = (oidvector *) DatumGetPointer(datum);

	/* Collation */
	datum = SysCacheGetAttr(PARTRELID, tuple,
							Anum_pg_partitioned_table_partcollation, &isnull);
	Assert(!isnull);
	collation = (oidvector *) DatumGetPointer(datum);

	/* Expressions */
	datum = SysCacheGetAttr(PARTRELID, tuple,
							Anum_pg_partitioned_table_partexprs, &isnull);
	if (!isnull)
	{
		char	   *exprString;
		Node	   *expr;

		exprString = TextDatumGetCString(datum);
		expr = stringToNode(exprString);
		pfree(exprString);

		/*
		 * Run the expressions through const-simplification since the planner
		 * will be comparing them to similarly-processed qual clause operands,
		 * and may fail to detect valid matches without this step; fix
		 * opfuncids while at it.  We don't need to bother with
		 * canonicalize_qual() though, because partition expressions should be
		 * in canonical form already (ie, no need for OR-merging or constant
		 * elimination).
		 */
		expr = eval_const_expressions(NULL, expr);
		fix_opfuncids(expr);

		oldcxt = MemoryContextSwitchTo(partkeycxt);
		key->partexprs = (List *) copyObject(expr);
		MemoryContextSwitchTo(oldcxt);
	}

	/* Allocate assorted arrays in the partkeycxt, which we'll fill below */
	oldcxt = MemoryContextSwitchTo(partkeycxt);
	key->partattrs = (AttrNumber *) palloc0(key->partnatts * sizeof(AttrNumber));
	key->partopfamily = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->partopcintype = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->partsupfunc = (FmgrInfo *) palloc0(key->partnatts * sizeof(FmgrInfo));

	key->partcollation = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->parttypid = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->parttypmod = (int32 *) palloc0(key->partnatts * sizeof(int32));
	key->parttyplen = (int16 *) palloc0(key->partnatts * sizeof(int16));
	key->parttypbyval = (bool *) palloc0(key->partnatts * sizeof(bool));
	key->parttypalign = (char *) palloc0(key->partnatts * sizeof(char));
	key->parttypcoll = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	MemoryContextSwitchTo(oldcxt);

	/* determine support function number to search for */
	procnum = (key->strategy == PARTITION_STRATEGY_HASH) ?
		HASHEXTENDED_PROC : BTORDER_PROC;

	/* Copy partattrs and fill other per-attribute info */
	memcpy(key->partattrs, attrs, key->partnatts * sizeof(int16));
	partexprs_item = list_head(key->partexprs);
	for (i = 0; i < key->partnatts; i++)
	{
		AttrNumber	attno = key->partattrs[i];
		HeapTuple	opclasstup;
		Form_pg_opclass opclassform;
		Oid			funcid;

		/* Collect opfamily information */
		opclasstup = SearchSysCache1(CLAOID,
									 ObjectIdGetDatum(opclass->values[i]));
		if (!HeapTupleIsValid(opclasstup))
			elog(ERROR, "cache lookup failed for opclass %u", opclass->values[i]);

		opclassform = (Form_pg_opclass) GETSTRUCT(opclasstup);
		key->partopfamily[i] = opclassform->opcfamily;
		key->partopcintype[i] = opclassform->opcintype;

		/* Get a support function for the specified opfamily and datatypes */
		funcid = get_opfamily_proc(opclassform->opcfamily,
								   opclassform->opcintype,
								   opclassform->opcintype,
								   procnum);
		if (!OidIsValid(funcid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing support function %d for type %s",
							NameStr(opclassform->opcname),
							(key->strategy == PARTITION_STRATEGY_HASH) ?
							"hash" : "btree",
							procnum,
							format_type_be(opclassform->opcintype))));

		fmgr_info_cxt(funcid, &key->partsupfunc[i], partkeycxt);

		/* Collation */
		key->partcollation[i] = collation->values[i];

		/* Collect type information */
		if (attno != 0)
		{
			Form_pg_attribute att = TupleDescAttr(relation->rd_att, attno - 1);

			key->parttypid[i] = att->atttypid;
			key->parttypmod[i] = att->atttypmod;
			key->parttypcoll[i] = att->attcollation;
		}
		else
		{
			if (partexprs_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");

			key->parttypid[i] = exprType(lfirst(partexprs_item));
			key->parttypmod[i] = exprTypmod(lfirst(partexprs_item));
			key->parttypcoll[i] = exprCollation(lfirst(partexprs_item));

			partexprs_item = lnext(key->partexprs, partexprs_item);
		}
		get_typlenbyvalalign(key->parttypid[i],
							 &key->parttyplen[i],
							 &key->parttypbyval[i],
							 &key->parttypalign[i]);

		ReleaseSysCache(opclasstup);
	}

	ReleaseSysCache(tuple);

	/* Assert that we're not leaking any old data during assignments below */
	Assert(relation->rd_partkeycxt == NULL);
	Assert(relation->rd_partkey == NULL);

	/*
	 * Success --- reparent our context and make the relcache point to the
	 * newly constructed key
	 */
	MemoryContextSetParent(partkeycxt, CacheMemoryContext);
	relation->rd_partkeycxt = partkeycxt;
	relation->rd_partkey = key;
}

/*
 * RelationGetPartitionQual
 *
 * Returns a list of partition quals
 */
List *
RelationGetPartitionQual(Relation rel)
{
	/* Quick exit */
	if (!rel->rd_rel->relispartition)
		return NIL;

	return generate_partition_qual(rel);
}

/*
 * get_partition_qual_relid
 *
 * Returns an expression tree describing the passed-in relation's partition
 * constraint.
 *
 * If the relation is not found, or is not a partition, or there is no
 * partition constraint, return NULL.  We must guard against the first two
 * cases because this supports a SQL function that could be passed any OID.
 * The last case can happen even if relispartition is true, when a default
 * partition is the only partition.
 */
Expr *
get_partition_qual_relid(Oid relid)
{
	Expr	   *result = NULL;

	/* Do the work only if this relation exists and is a partition. */
	if (get_rel_relispartition(relid))
	{
		Relation	rel = relation_open(relid, AccessShareLock);
		List	   *and_args;

		and_args = generate_partition_qual(rel);

		/* Convert implicit-AND list format to boolean expression */
		if (and_args == NIL)
			result = NULL;
		else if (list_length(and_args) > 1)
			result = makeBoolExpr(AND_EXPR, and_args, -1);
		else
			result = linitial(and_args);

		/* Keep the lock, to allow safe deparsing against the rel by caller. */
		relation_close(rel, NoLock);
	}

	return result;
}

/*
 * generate_partition_qual
 *
 * Generate partition predicate from rel's partition bound expression. The
 * function returns a NIL list if there is no predicate.
 *
 * We cache a copy of the result in the relcache entry, after constructing
 * it using the caller's context.  This approach avoids leaking any data
 * into long-lived cache contexts, especially if we fail partway through.
 */
static List *
generate_partition_qual(Relation rel)
{
	HeapTuple	tuple;
	MemoryContext oldcxt;
	Datum		boundDatum;
	bool		isnull;
	List	   *my_qual = NIL,
			   *result = NIL;
	Relation	parent;

	/* Guard against stack overflow due to overly deep partition tree */
	check_stack_depth();

	/* If we already cached the result, just return a copy */
	if (rel->rd_partcheckvalid)
		return copyObject(rel->rd_partcheck);

	/* Grab at least an AccessShareLock on the parent table */
	parent = relation_open(get_partition_parent(RelationGetRelid(rel)),
						   AccessShareLock);

	/* Get pg_class.relpartbound */
	tuple = SearchSysCache1(RELOID, RelationGetRelid(rel));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

	boundDatum = SysCacheGetAttr(RELOID, tuple,
								 Anum_pg_class_relpartbound,
								 &isnull);
	if (!isnull)
	{
		PartitionBoundSpec *bound;

		bound = castNode(PartitionBoundSpec,
						 stringToNode(TextDatumGetCString(boundDatum)));

		my_qual = get_qual_from_partbound(rel, parent, bound);
	}

	ReleaseSysCache(tuple);

	/* Add the parent's quals to the list (if any) */
	if (parent->rd_rel->relispartition)
		result = list_concat(generate_partition_qual(parent), my_qual);
	else
		result = my_qual;

	/*
	 * Change Vars to have partition's attnos instead of the parent's. We do
	 * this after we concatenate the parent's quals, because we want every Var
	 * in it to bear this relation's attnos. It's safe to assume varno = 1
	 * here.
	 */
	result = map_partition_varattnos(result, 1, rel, parent);

	/* Assert that we're not leaking any old data during assignments below */
	Assert(rel->rd_partcheckcxt == NULL);
	Assert(rel->rd_partcheck == NIL);

	/*
	 * Save a copy in the relcache.  The order of these operations is fairly
	 * critical to avoid memory leaks and ensure that we don't leave a corrupt
	 * relcache entry if we fail partway through copyObject.
	 *
	 * If, as is definitely possible, the partcheck list is NIL, then we do
	 * not need to make a context to hold it.
	 */
	if (result != NIL)
	{
		rel->rd_partcheckcxt = AllocSetContextCreate(CacheMemoryContext,
													 "partition constraint",
													 ALLOCSET_SMALL_SIZES);
		MemoryContextCopyAndSetIdentifier(rel->rd_partcheckcxt,
										  RelationGetRelationName(rel));
		oldcxt = MemoryContextSwitchTo(rel->rd_partcheckcxt);
		rel->rd_partcheck = copyObject(result);
		MemoryContextSwitchTo(oldcxt);
	}
	else
		rel->rd_partcheck = NIL;
	rel->rd_partcheckvalid = true;

	/* Keep the parent locked until commit */
	relation_close(parent, NoLock);

	/* Return the working copy to the caller */
	return result;
}
