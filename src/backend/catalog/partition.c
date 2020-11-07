/*-------------------------------------------------------------------------
 *
 * partition.c
 *		  Partitioning related data structures and functions.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		  src/backend/catalog/partition.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/attmap.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_partitioned_table.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "partitioning/partbounds.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid	get_partition_parent_worker(Relation inhRel, Oid relid);
static void get_partition_ancestors_worker(Relation inhRel, Oid relid,
										   List **ancestors);

/*
 * get_partition_parent
 *		Obtain direct parent of given relation
 *
 * Returns inheritance parent of a partition by scanning pg_inherits
 *
 * Note: Because this function assumes that the relation whose OID is passed
 * as an argument will have precisely one parent, it should only be called
 * when it is known that the relation is a partition.
 */
Oid
get_partition_parent(Oid relid)
{
	Relation	catalogRelation;
	Oid			result;

	catalogRelation = table_open(InheritsRelationId, AccessShareLock);

	result = get_partition_parent_worker(catalogRelation, relid);

	if (!OidIsValid(result))
		elog(ERROR, "could not find tuple for parent of relation %u", relid);

	table_close(catalogRelation, AccessShareLock);

	return result;
}

/*
 * get_partition_parent_worker
 *		Scan the pg_inherits relation to return the OID of the parent of the
 *		given relation
 */
static Oid
get_partition_parent_worker(Relation inhRel, Oid relid)
{
	SysScanDesc scan;
	ScanKeyData key[2];
	Oid			result = InvalidOid;
	HeapTuple	tuple;

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_inherits_inhseqno,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(1));

	scan = systable_beginscan(inhRel, InheritsRelidSeqnoIndexId, true,
							  NULL, 2, key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_inherits form = (Form_pg_inherits) GETSTRUCT(tuple);

		result = form->inhparent;
	}

	systable_endscan(scan);

	return result;
}

/*
 * get_partition_ancestors
 *		Obtain ancestors of given relation
 *
 * Returns a list of ancestors of the given relation.
 *
 * Note: Because this function assumes that the relation whose OID is passed
 * as an argument and each ancestor will have precisely one parent, it should
 * only be called when it is known that the relation is a partition.
 */
List *
get_partition_ancestors(Oid relid)
{
	List	   *result = NIL;
	Relation	inhRel;

	inhRel = table_open(InheritsRelationId, AccessShareLock);

	get_partition_ancestors_worker(inhRel, relid, &result);

	table_close(inhRel, AccessShareLock);

	return result;
}

/*
 * get_partition_ancestors_worker
 *		recursive worker for get_partition_ancestors
 */
static void
get_partition_ancestors_worker(Relation inhRel, Oid relid, List **ancestors)
{
	Oid			parentOid;

	/* Recursion ends at the topmost level, ie., when there's no parent */
	parentOid = get_partition_parent_worker(inhRel, relid);
	if (parentOid == InvalidOid)
		return;

	*ancestors = lappend_oid(*ancestors, parentOid);
	get_partition_ancestors_worker(inhRel, parentOid, ancestors);
}

/*
 * index_get_partition
 *		Return the OID of index of the given partition that is a child
 *		of the given index, or InvalidOid if there isn't one.
 */
Oid
index_get_partition(Relation partition, Oid indexId)
{
	List	   *idxlist = RelationGetIndexList(partition);
	ListCell   *l;

	foreach(l, idxlist)
	{
		Oid			partIdx = lfirst_oid(l);
		HeapTuple	tup;
		Form_pg_class classForm;
		bool		ispartition;

		tup = SearchSysCache1(RELOID, ObjectIdGetDatum(partIdx));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for relation %u", partIdx);
		classForm = (Form_pg_class) GETSTRUCT(tup);
		ispartition = classForm->relispartition;
		ReleaseSysCache(tup);
		if (!ispartition)
			continue;
		if (get_partition_parent(partIdx) == indexId)
		{
			list_free(idxlist);
			return partIdx;
		}
	}

	list_free(idxlist);
	return InvalidOid;
}

/*
 * map_partition_varattnos - maps varattnos of all Vars in 'expr' (that have
 * varno 'fromrel_varno') from the attnums of 'from_rel' to the attnums of
 * 'to_rel', each of which may be either a leaf partition or a partitioned
 * table, but both of which must be from the same partitioning hierarchy.
 *
 * We need this because even though all of the same column names must be
 * present in all relations in the hierarchy, and they must also have the
 * same types, the attnums may be different.
 *
 * Note: this will work on any node tree, so really the argument and result
 * should be declared "Node *".  But a substantial majority of the callers
 * are working on Lists, so it's less messy to do the casts internally.
 */
List *
map_partition_varattnos(List *expr, int fromrel_varno,
						Relation to_rel, Relation from_rel)
{
	if (expr != NIL)
	{
		AttrMap    *part_attmap;
		bool		found_whole_row;

		part_attmap = build_attrmap_by_name(RelationGetDescr(to_rel),
											RelationGetDescr(from_rel));
		expr = (List *) map_variable_attnos((Node *) expr,
											fromrel_varno, 0,
											part_attmap,
											RelationGetForm(to_rel)->reltype,
											&found_whole_row);
		/* Since we provided a to_rowtype, we may ignore found_whole_row. */
	}

	return expr;
}

/*
 * Checks if any of the 'attnums' is a partition key attribute for rel
 *
 * Sets *used_in_expr if any of the 'attnums' is found to be referenced in some
 * partition key expression.  It's possible for a column to be both used
 * directly and as part of an expression; if that happens, *used_in_expr may
 * end up as either true or false.  That's OK for current uses of this
 * function, because *used_in_expr is only used to tailor the error message
 * text.
 */
bool
has_partition_attrs(Relation rel, Bitmapset *attnums, bool *used_in_expr)
{
	PartitionKey key;
	int			partnatts;
	List	   *partexprs;
	ListCell   *partexprs_item;
	int			i;

	if (attnums == NULL || rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return false;

	key = RelationGetPartitionKey(rel);
	partnatts = get_partition_natts(key);
	partexprs = get_partition_exprs(key);

	partexprs_item = list_head(partexprs);
	for (i = 0; i < partnatts; i++)
	{
		AttrNumber	partattno = get_partition_col_attnum(key, i);

		if (partattno != 0)
		{
			if (bms_is_member(partattno - FirstLowInvalidHeapAttributeNumber,
							  attnums))
			{
				if (used_in_expr)
					*used_in_expr = false;
				return true;
			}
		}
		else
		{
			/* Arbitrary expression */
			Node	   *expr = (Node *) lfirst(partexprs_item);
			Bitmapset  *expr_attrs = NULL;

			/* Find all attributes referenced */
			pull_varattnos(expr, 1, &expr_attrs);
			partexprs_item = lnext(partexprs, partexprs_item);

			if (bms_overlap(attnums, expr_attrs))
			{
				if (used_in_expr)
					*used_in_expr = true;
				return true;
			}
		}
	}

	return false;
}

/*
 * get_default_partition_oid
 *
 * Given a relation OID, return the OID of the default partition, if one
 * exists.  Use get_default_oid_from_partdesc where possible, for
 * efficiency.
 */
Oid
get_default_partition_oid(Oid parentId)
{
	HeapTuple	tuple;
	Oid			defaultPartId = InvalidOid;

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(parentId));

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_partitioned_table part_table_form;

		part_table_form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
		defaultPartId = part_table_form->partdefid;
		ReleaseSysCache(tuple);
	}

	return defaultPartId;
}

/*
 * update_default_partition_oid
 *
 * Update pg_partitioned_table.partdefid with a new default partition OID.
 */
void
update_default_partition_oid(Oid parentId, Oid defaultPartId)
{
	HeapTuple	tuple;
	Relation	pg_partitioned_table;
	Form_pg_partitioned_table part_table_form;

	pg_partitioned_table = table_open(PartitionedRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(parentId));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 parentId);

	part_table_form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
	part_table_form->partdefid = defaultPartId;
	CatalogTupleUpdate(pg_partitioned_table, &tuple->t_self, tuple);

	heap_freetuple(tuple);
	table_close(pg_partitioned_table, RowExclusiveLock);
}

/*
 * get_proposed_default_constraint
 *
 * This function returns the negation of new_part_constraints, which
 * would be an integral part of the default partition constraints after
 * addition of the partition to which the new_part_constraints belongs.
 */
List *
get_proposed_default_constraint(List *new_part_constraints)
{
	Expr	   *defPartConstraint;

	defPartConstraint = make_ands_explicit(new_part_constraints);

	/*
	 * Derive the partition constraints of default partition by negating the
	 * given partition constraints. The partition constraint never evaluates
	 * to NULL, so negating it like this is safe.
	 */
	defPartConstraint = makeBoolExpr(NOT_EXPR,
									 list_make1(defPartConstraint),
									 -1);

	/* Simplify, to put the negated expression into canonical form */
	defPartConstraint =
		(Expr *) eval_const_expressions(NULL,
										(Node *) defPartConstraint);
	defPartConstraint = canonicalize_qual(defPartConstraint, true);

	return make_ands_implicit(defPartConstraint);
}
