/*-------------------------------------------------------------------------
 *
 * not_in.c--
 *	  Executes the "not_in" operator for any data type
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/not_in.c,v 1.5 1997/09/08 02:30:52 momjian Exp $
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
#include <stdio.h>
#include <string.h>
#include "postgres.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "utils/builtins.h"		/* where function decls go */

static int	my_varattno(Relation rd, char *a);

/* ----------------------------------------------------------------
 *
 * ----------------------------------------------------------------
 */
bool
int4notin(int16 not_in_arg, char *relation_and_attr)
{
	Relation	relation_to_scan;
	int			left_side_argument,
				integer_value;
	HeapTuple	current_tuple;
	HeapScanDesc scan_descriptor;
	bool		dummy,
				retval;
	int			attrid;
	char	   *relation,
			   *attribute;
	char		my_copy[32];
	Datum		value;
	NameData	relNameData;
	ScanKeyData skeyData;

	strcpy(my_copy, relation_and_attr);

	relation = (char *) strtok(my_copy, ".");
	attribute = (char *) strtok(NULL, ".");


	/* fetch tuple OID */

	left_side_argument = not_in_arg;

	/* Open the relation and get a relation descriptor */

	namestrcpy(&relNameData, relation);
	relation_to_scan = heap_openr(relNameData.data);
	attrid = my_varattno(relation_to_scan, attribute);

	/* the last argument should be a ScanKey, not an integer! - jolly */
	/* it looks like the arguments are out of order, too */
	/* but skeyData is never initialized! does this work?? - ay 2/95 */
	scan_descriptor = heap_beginscan(relation_to_scan, false, NULL, 0,
									 &skeyData);

	retval = true;

	/* do a scan of the relation, and do the check */
	for (current_tuple = heap_getnext(scan_descriptor, 0, NULL);
		 current_tuple != NULL && retval;
		 current_tuple = heap_getnext(scan_descriptor, 0, NULL))
	{
		value = PointerGetDatum(heap_getattr(current_tuple,
											 InvalidBuffer,
											 (AttrNumber) attrid,
							RelationGetTupleDescriptor(relation_to_scan),
											 &dummy));

		integer_value = DatumGetInt16(value);
		if (left_side_argument == integer_value)
		{
			retval = false;
		}
	}

	/* close the relation */
	heap_close(relation_to_scan);
	return (retval);
}

bool
oidnotin(Oid the_oid, char *compare)
{
	if (the_oid == InvalidOid)
		return false;
	return (int4notin(the_oid, compare));
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
		{
			return (i + 1);
		}
	}
	return (-1);
}
