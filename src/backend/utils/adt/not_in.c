/*-------------------------------------------------------------------------
 *
 * not_in.c
 *	  Executes the "not_in" operator for any data type
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/not_in.c,v 1.22 2000/01/26 05:57:14 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *
 * XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 * X HACK WARNING!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! X
 * XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 *
 * This code is the OLD not-in code that is HACKED
 * into place until operators that can have arguments as
 * columns are ******REALLY****** implemented!!!!!!!!!!!
 *
 */

#include "postgres.h"
#include "access/heapam.h"
#include "utils/builtins.h"

static int	my_varattno(Relation rd, char *a);

/* ----------------------------------------------------------------
 *
 * ----------------------------------------------------------------
 */
bool
int4notin(int32 not_in_arg, char *relation_and_attr)
{
	Relation	relation_to_scan;
	int32		integer_value;
	HeapTuple	current_tuple;
	HeapScanDesc scan_descriptor;
	bool		dummy,
				retval;
	int			attrid;
	char	   *relation,
			   *attribute;
	char		my_copy[NAMEDATALEN * 2 + 2];
	Datum		value;

	strncpy(my_copy, relation_and_attr, sizeof(my_copy));
	my_copy[sizeof(my_copy) - 1] = '\0';

	relation = (char *) strtok(my_copy, ".");
	attribute = (char *) strtok(NULL, ".");
	if (attribute == NULL)
		elog(ERROR, "int4notin: must provide relationname.attributename");

	/* Open the relation and get a relation descriptor */

	relation_to_scan = heap_openr(relation, AccessShareLock);

	/* Find the column to search */

	attrid = my_varattno(relation_to_scan, attribute);
	if (attrid < 0)
	{
		elog(ERROR, "int4notin: unknown attribute %s for relation %s",
			 attribute, relation);
	}

	scan_descriptor = heap_beginscan(relation_to_scan, false, SnapshotNow,
									 0, (ScanKey) NULL);

	retval = true;

	/* do a scan of the relation, and do the check */
	while (HeapTupleIsValid(current_tuple = heap_getnext(scan_descriptor, 0)))
	{
		value = heap_getattr(current_tuple,
							 (AttrNumber) attrid,
							 RelationGetDescr(relation_to_scan),
							 &dummy);
		integer_value = DatumGetInt32(value);
		if (not_in_arg == integer_value)
		{
			retval = false;
			break;				/* can stop scanning now */
		}
	}

	/* close the relation */
	heap_endscan(scan_descriptor);
	heap_close(relation_to_scan, AccessShareLock);

	return retval;
}

bool
oidnotin(Oid the_oid, char *compare)
{
	if (the_oid == InvalidOid)
		return false;
	return int4notin(the_oid, compare);
}

/*
 * XXX
 * If varattno (in parser/catalog_utils.h) ever is added to
 * cinterface.a, this routine should go away
 */
static int
my_varattno(Relation rd, char *a)
{
	int			i;

	for (i = 0; i < rd->rd_rel->relnatts; i++)
	{
		if (!namestrcmp(&rd->rd_att->attrs[i]->attname, a))
			return i + 1;
	}
	return -1;
}
