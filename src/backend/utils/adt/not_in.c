/*-------------------------------------------------------------------------
 *
 * not_in.c
 *	  Executes the "not_in" operator for any data type
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/not_in.c,v 1.26 2001/03/22 03:59:52 momjian Exp $
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
Datum
int4notin(PG_FUNCTION_ARGS)
{
	int32		not_in_arg = PG_GETARG_INT32(0);
	text	   *relation_and_attr = PG_GETARG_TEXT_P(1);
	Relation	relation_to_scan;
	int32		integer_value;
	HeapTuple	current_tuple;
	HeapScanDesc scan_descriptor;
	bool		isNull,
				retval;
	int			attrid,
				strlength;
	char	   *relation,
			   *attribute;
	char		my_copy[NAMEDATALEN * 2 + 2];
	Datum		value;

	/* make a null-terminated copy of text */
	strlength = VARSIZE(relation_and_attr) - VARHDRSZ;
	if (strlength >= sizeof(my_copy))
		strlength = sizeof(my_copy) - 1;
	memcpy(my_copy, VARDATA(relation_and_attr), strlength);
	my_copy[strlength] = '\0';

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
							 &isNull);
		if (isNull)
			continue;
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

	PG_RETURN_BOOL(retval);
}

Datum
oidnotin(PG_FUNCTION_ARGS)
{
	Oid			the_oid = PG_GETARG_OID(0);

#ifdef NOT_USED
	text	   *relation_and_attr = PG_GETARG_TEXT_P(1);

#endif

	if (the_oid == InvalidOid)
		PG_RETURN_BOOL(false);
	/* XXX assume oid maps to int4 */
	return int4notin(fcinfo);
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
		if (namestrcmp(&rd->rd_att->attrs[i]->attname, a) == 0)
			return i + 1;
	}
	return -1;
}
