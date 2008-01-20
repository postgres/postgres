/*-------------------------------------------------------------------------
 *
 * pg_enum.c
 *	  routines to support manipulation of the pg_enum relation
 *
 * Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_enum.c,v 1.5 2008/01/20 17:50:41 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_enum.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

static int	oid_cmp(const void *p1, const void *p2);


/*
 * EnumValuesCreate
 *		Create an entry in pg_enum for each of the supplied enum values.
 *
 * vals is a list of Value strings.
 */
void
EnumValuesCreate(Oid enumTypeOid, List *vals)
{
	Relation	pg_enum;
	TupleDesc	tupDesc;
	NameData	enumlabel;
	Oid		   *oids;
	int			i,
				n;
	Datum		values[Natts_pg_enum];
	char		nulls[Natts_pg_enum];
	ListCell   *lc;
	HeapTuple	tup;

	n = list_length(vals);

	/*
	 * XXX we do not bother to check the list of values for duplicates --- if
	 * you have any, you'll get a less-than-friendly unique-index violation.
	 * Is it worth trying harder?
	 */

	pg_enum = heap_open(EnumRelationId, RowExclusiveLock);
	tupDesc = pg_enum->rd_att;

	/*
	 * Allocate oids.  While this method does not absolutely guarantee that we
	 * generate no duplicate oids (since we haven't entered each oid into the
	 * table before allocating the next), trouble could only occur if the oid
	 * counter wraps all the way around before we finish. Which seems
	 * unlikely.
	 */
	oids = (Oid *) palloc(n * sizeof(Oid));
	for (i = 0; i < n; i++)
	{
		oids[i] = GetNewOid(pg_enum);
	}

	/* sort them, just in case counter wrapped from high to low */
	qsort(oids, n, sizeof(Oid), oid_cmp);

	/* and make the entries */
	memset(nulls, ' ', sizeof(nulls));

	i = 0;
	foreach(lc, vals)
	{
		char	   *lab = strVal(lfirst(lc));

		/*
		 * labels are stored in a name field, for easier syscache lookup, so
		 * check the length to make sure it's within range.
		 */
		if (strlen(lab) > (NAMEDATALEN - 1))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("invalid enum label \"%s\"", lab),
					 errdetail("Labels must be %d characters or less.",
							   NAMEDATALEN - 1)));

		values[Anum_pg_enum_enumtypid - 1] = ObjectIdGetDatum(enumTypeOid);
		namestrcpy(&enumlabel, lab);
		values[Anum_pg_enum_enumlabel - 1] = NameGetDatum(&enumlabel);

		tup = heap_formtuple(tupDesc, values, nulls);
		HeapTupleSetOid(tup, oids[i]);

		simple_heap_insert(pg_enum, tup);
		CatalogUpdateIndexes(pg_enum, tup);
		heap_freetuple(tup);

		i++;
	}

	/* clean up */
	pfree(oids);
	heap_close(pg_enum, RowExclusiveLock);
}


/*
 * EnumValuesDelete
 *		Remove all the pg_enum entries for the specified enum type.
 */
void
EnumValuesDelete(Oid enumTypeOid)
{
	Relation	pg_enum;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	pg_enum = heap_open(EnumRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumTypeOid));

	scan = systable_beginscan(pg_enum, EnumTypIdLabelIndexId, true,
							  SnapshotNow, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		simple_heap_delete(pg_enum, &tup->t_self);
	}

	systable_endscan(scan);

	heap_close(pg_enum, RowExclusiveLock);
}


/* qsort comparison function */
static int
oid_cmp(const void *p1, const void *p2)
{
	Oid			v1 = *((const Oid *) p1);
	Oid			v2 = *((const Oid *) p2);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}
