/*-------------------------------------------------------------------------
 *
 * statscmds.c
 *	  Commands for creating and altering extended statistics
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/statscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_statistic_ext.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "statistics/statistics.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* used for sorting the attnums in CreateStatistics */
static int
compare_int16(const void *a, const void *b)
{
	return memcmp(a, b, sizeof(int16));
}

/*
 *		CREATE STATISTICS
 */
ObjectAddress
CreateStatistics(CreateStatsStmt *stmt)
{
	int			i;
	ListCell   *l;
	int16		attnums[STATS_MAX_DIMENSIONS];
	int			numcols = 0;
	ObjectAddress address = InvalidObjectAddress;
	char	   *namestr;
	NameData	staname;
	Oid			statoid;
	Oid			namespaceId;
	HeapTuple	htup;
	Datum		values[Natts_pg_statistic_ext];
	bool		nulls[Natts_pg_statistic_ext];
	int2vector *stakeys;
	Relation	statrel;
	Relation	rel;
	Oid			relid;
	ObjectAddress parentobject,
				childobject;
	Datum		types[1];		/* only ndistinct defined now */
	int			ntypes;
	ArrayType  *staenabled;
	bool		build_ndistinct;
	bool		requested_type = false;

	Assert(IsA(stmt, CreateStatsStmt));

	/* resolve the pieces of the name (namespace etc.) */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->defnames, &namestr);
	namestrcpy(&staname, namestr);

	/*
	 * If if_not_exists was given and the statistics already exists, bail out.
	 */
	if (SearchSysCacheExists2(STATEXTNAMENSP,
							  PointerGetDatum(&staname),
							  ObjectIdGetDatum(namespaceId)))
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("statistics \"%s\" already exist, skipping",
							namestr)));
			return InvalidObjectAddress;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("statistics \"%s\" already exist", namestr)));
	}

	rel = heap_openrv(stmt->relation, AccessExclusiveLock);
	relid = RelationGetRelid(rel);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a table or materialized view",
						RelationGetRelationName(rel))));

	/*
	 * Transform column names to array of attnums. While at it, enforce some
	 * constraints.
	 */
	foreach(l, stmt->keys)
	{
		char	   *attname = strVal(lfirst(l));
		HeapTuple	atttuple;
		Form_pg_attribute attForm;
		TypeCacheEntry *type;

		atttuple = SearchSysCacheAttName(relid, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
			  errmsg("column \"%s\" referenced in statistics does not exist",
					 attname)));
		attForm = (Form_pg_attribute) GETSTRUCT(atttuple);

		/* Disallow use of system attributes in extended stats */
		if (attForm->attnum < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("statistic creation on system columns is not supported")));

		/* Disallow data types without a less-than operator */
		type = lookup_type_cache(attForm->atttypid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only scalar types can be used in extended statistics")));

		/* Make sure no more than STATS_MAX_DIMENSIONS columns are used */
		if (numcols >= STATS_MAX_DIMENSIONS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d keys in statistics",
							STATS_MAX_DIMENSIONS)));

		attnums[numcols] = ((Form_pg_attribute) GETSTRUCT(atttuple))->attnum;
		ReleaseSysCache(atttuple);
		numcols++;
	}

	/*
	 * Check that at least two columns were specified in the statement. The
	 * upper bound was already checked in the loop above.
	 */
	if (numcols < 2)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("statistics require at least 2 columns")));

	/*
	 * Sort the attnums, which makes detecting duplicies somewhat easier, and
	 * it does not hurt (it does not affect the efficiency, unlike for
	 * indexes, for example).
	 */
	qsort(attnums, numcols, sizeof(int16), compare_int16);

	/*
	 * Look for duplicities in the list of columns. The attnums are sorted so
	 * just check consecutive elements.
	 */
	for (i = 1; i < numcols; i++)
		if (attnums[i] == attnums[i - 1])
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
				  errmsg("duplicate column name in statistics definition")));

	stakeys = buildint2vector(attnums, numcols);

	/*
	 * Parse the statistics options.  Currently only statistics types are
	 * recognized.
	 */
	build_ndistinct = false;
	foreach(l, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(l);

		if (strcmp(opt->defname, "ndistinct") == 0)
		{
			build_ndistinct = defGetBoolean(opt);
			requested_type = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized STATISTICS option \"%s\"",
							opt->defname)));
	}
	/* If no statistic type was specified, build them all. */
	if (!requested_type)
		build_ndistinct = true;

	/* construct the char array of enabled statistic types */
	ntypes = 0;
	if (build_ndistinct)
		types[ntypes++] = CharGetDatum(STATS_EXT_NDISTINCT);
	Assert(ntypes > 0);
	staenabled = construct_array(types, ntypes, CHAROID, 1, true, 'c');

	/*
	 * Everything seems fine, so let's build the pg_statistic_ext tuple.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	values[Anum_pg_statistic_ext_starelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_statistic_ext_staname - 1] = NameGetDatum(&staname);
	values[Anum_pg_statistic_ext_stanamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_statistic_ext_staowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_statistic_ext_stakeys - 1] = PointerGetDatum(stakeys);
	values[Anum_pg_statistic_ext_staenabled - 1] = PointerGetDatum(staenabled);

	/* no statistics build yet */
	nulls[Anum_pg_statistic_ext_standistinct - 1] = true;

	/* insert it into pg_statistic_ext */
	statrel = heap_open(StatisticExtRelationId, RowExclusiveLock);
	htup = heap_form_tuple(statrel->rd_att, values, nulls);
	CatalogTupleInsert(statrel, htup);
	statoid = HeapTupleGetOid(htup);
	heap_freetuple(htup);
	heap_close(statrel, RowExclusiveLock);

	/*
	 * Invalidate relcache so that others see the new statistics.
	 */
	CacheInvalidateRelcache(rel);

	relation_close(rel, NoLock);

	/*
	 * Add a dependency on a table, so that stats get dropped on DROP TABLE.
	 */
	ObjectAddressSet(parentobject, RelationRelationId, relid);
	ObjectAddressSet(childobject, StatisticExtRelationId, statoid);
	recordDependencyOn(&childobject, &parentobject, DEPENDENCY_AUTO);

	/*
	 * Also add dependency on the schema.  This is required to ensure that we
	 * drop the statistics on DROP SCHEMA.  This is not handled automatically
	 * by DROP TABLE because the statistics are not an object in the table's
	 * schema.
	 */
	ObjectAddressSet(parentobject, NamespaceRelationId, namespaceId);
	recordDependencyOn(&childobject, &parentobject, DEPENDENCY_AUTO);

	ObjectAddressSet(address, StatisticExtRelationId, statoid);

	return address;
}

/*
 * Guts of statistics deletion.
 */
void
RemoveStatisticsById(Oid statsOid)
{
	Relation	relation;
	Oid			relid;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_statistic_ext statext;

	/*
	 * Delete the pg_statistic_ext tuple.
	 */
	relation = heap_open(StatisticExtRelationId, RowExclusiveLock);

	tup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statsOid));

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for statistics %u", statsOid);

	statext = (Form_pg_statistic_ext) GETSTRUCT(tup);
	relid = statext->starelid;

	rel = heap_open(relid, AccessExclusiveLock);

	simple_heap_delete(relation, &tup->t_self);

	CacheInvalidateRelcache(rel);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
	heap_close(rel, NoLock);
}
