/*-------------------------------------------------------------------------
 *
 * attmap.c
 *	  Attribute mapping support.
 *
 * This file provides utility routines to build and manage attribute
 * mappings by comparing input and output TupleDescs.  Such mappings
 * are typically used by DDL operating on inheritance and partition trees
 * to do a conversion between rowtypes logically equivalent but with
 * columns in a different order, taking into account dropped columns.
 * They are also used by the tuple conversion routines in tupconvert.c.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/attmap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/attmap.h"
#include "utils/builtins.h"


static bool check_attrmap_match(TupleDesc indesc,
								TupleDesc outdesc,
								AttrMap *attrMap);

/*
 * make_attrmap
 *
 * Utility routine to allocate an attribute map in the current memory
 * context.
 */
AttrMap *
make_attrmap(int maplen)
{
	AttrMap    *res;

	res = (AttrMap *) palloc0(sizeof(AttrMap));
	res->maplen = maplen;
	res->attnums = (AttrNumber *) palloc0(sizeof(AttrNumber) * maplen);
	return res;
}

/*
 * free_attrmap
 *
 * Utility routine to release an attribute map.
 */
void
free_attrmap(AttrMap *map)
{
	pfree(map->attnums);
	pfree(map);
}

/*
 * build_attrmap_by_position
 *
 * Return a palloc'd bare attribute map for tuple conversion, matching input
 * and output columns by position.  Dropped columns are ignored in both input
 * and output, marked as 0.  This is normally a subroutine for
 * convert_tuples_by_position in tupconvert.c, but it can be used standalone.
 *
 * Note: the errdetail messages speak of indesc as the "returned" rowtype,
 * outdesc as the "expected" rowtype.  This is okay for current uses but
 * might need generalization in future.
 */
AttrMap *
build_attrmap_by_position(TupleDesc indesc,
						  TupleDesc outdesc,
						  const char *msg)
{
	AttrMap    *attrMap;
	int			nincols;
	int			noutcols;
	int			n;
	int			i;
	int			j;
	bool		same;

	/*
	 * The length is computed as the number of attributes of the expected
	 * rowtype as it includes dropped attributes in its count.
	 */
	n = outdesc->natts;
	attrMap = make_attrmap(n);

	j = 0;						/* j is next physical input attribute */
	nincols = noutcols = 0;		/* these count non-dropped attributes */
	same = true;
	for (i = 0; i < n; i++)
	{
		Form_pg_attribute att = TupleDescAttr(outdesc, i);
		Oid			atttypid;
		int32		atttypmod;

		if (att->attisdropped)
			continue;			/* attrMap->attnums[i] is already 0 */
		noutcols++;
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		for (; j < indesc->natts; j++)
		{
			att = TupleDescAttr(indesc, j);
			if (att->attisdropped)
				continue;
			nincols++;

			/* Found matching column, now check type */
			if (atttypid != att->atttypid ||
				(atttypmod != att->atttypmod && atttypmod >= 0))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg_internal("%s", _(msg)),
						 errdetail("Returned type %s does not match expected type %s in column %d.",
								   format_type_with_typemod(att->atttypid,
															att->atttypmod),
								   format_type_with_typemod(atttypid,
															atttypmod),
								   noutcols)));
			attrMap->attnums[i] = (AttrNumber) (j + 1);
			j++;
			break;
		}
		if (attrMap->attnums[i] == 0)
			same = false;		/* we'll complain below */
	}

	/* Check for unused input columns */
	for (; j < indesc->natts; j++)
	{
		if (TupleDescCompactAttr(indesc, j)->attisdropped)
			continue;
		nincols++;
		same = false;			/* we'll complain below */
	}

	/* Report column count mismatch using the non-dropped-column counts */
	if (!same)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg_internal("%s", _(msg)),
				 errdetail("Number of returned columns (%d) does not match "
						   "expected column count (%d).",
						   nincols, noutcols)));

	/* Check if the map has a one-to-one match */
	if (check_attrmap_match(indesc, outdesc, attrMap))
	{
		/* Runtime conversion is not needed */
		free_attrmap(attrMap);
		return NULL;
	}

	return attrMap;
}

/*
 * build_attrmap_by_name
 *
 * Return a palloc'd bare attribute map for tuple conversion, matching input
 * and output columns by name.  (Dropped columns are ignored in both input and
 * output.)  This is normally a subroutine for convert_tuples_by_name in
 * tupconvert.c, but can be used standalone.
 *
 * If 'missing_ok' is true, a column from 'outdesc' not being present in
 * 'indesc' is not flagged as an error; AttrMap.attnums[] entry for such an
 * outdesc column will be 0 in that case.
 */
AttrMap *
build_attrmap_by_name(TupleDesc indesc,
					  TupleDesc outdesc,
					  bool missing_ok)
{
	AttrMap    *attrMap;
	int			outnatts;
	int			innatts;
	int			i;
	int			nextindesc = -1;

	outnatts = outdesc->natts;
	innatts = indesc->natts;

	attrMap = make_attrmap(outnatts);
	for (i = 0; i < outnatts; i++)
	{
		Form_pg_attribute outatt = TupleDescAttr(outdesc, i);
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		int			j;

		if (outatt->attisdropped)
			continue;			/* attrMap->attnums[i] is already 0 */
		attname = NameStr(outatt->attname);
		atttypid = outatt->atttypid;
		atttypmod = outatt->atttypmod;

		/*
		 * Now search for an attribute with the same name in the indesc. It
		 * seems likely that a partitioned table will have the attributes in
		 * the same order as the partition, so the search below is optimized
		 * for that case.  It is possible that columns are dropped in one of
		 * the relations, but not the other, so we use the 'nextindesc'
		 * counter to track the starting point of the search.  If the inner
		 * loop encounters dropped columns then it will have to skip over
		 * them, but it should leave 'nextindesc' at the correct position for
		 * the next outer loop.
		 */
		for (j = 0; j < innatts; j++)
		{
			Form_pg_attribute inatt;

			nextindesc++;
			if (nextindesc >= innatts)
				nextindesc = 0;

			inatt = TupleDescAttr(indesc, nextindesc);
			if (inatt->attisdropped)
				continue;
			if (strcmp(attname, NameStr(inatt->attname)) == 0)
			{
				/* Found it, check type */
				if (atttypid != inatt->atttypid || atttypmod != inatt->atttypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not convert row type"),
							 errdetail("Attribute \"%s\" of type %s does not match corresponding attribute of type %s.",
									   attname,
									   format_type_be(outdesc->tdtypeid),
									   format_type_be(indesc->tdtypeid))));
				attrMap->attnums[i] = inatt->attnum;
				break;
			}
		}
		if (attrMap->attnums[i] == 0 && !missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not convert row type"),
					 errdetail("Attribute \"%s\" of type %s does not exist in type %s.",
							   attname,
							   format_type_be(outdesc->tdtypeid),
							   format_type_be(indesc->tdtypeid))));
	}
	return attrMap;
}

/*
 * build_attrmap_by_name_if_req
 *
 * Returns mapping created by build_attrmap_by_name, or NULL if no
 * conversion is required.  This is a convenience routine for
 * convert_tuples_by_name() in tupconvert.c and other functions, but it
 * can be used standalone.
 */
AttrMap *
build_attrmap_by_name_if_req(TupleDesc indesc,
							 TupleDesc outdesc,
							 bool missing_ok)
{
	AttrMap    *attrMap;

	/* Verify compatibility and prepare attribute-number map */
	attrMap = build_attrmap_by_name(indesc, outdesc, missing_ok);

	/* Check if the map has a one-to-one match */
	if (check_attrmap_match(indesc, outdesc, attrMap))
	{
		/* Runtime conversion is not needed */
		free_attrmap(attrMap);
		return NULL;
	}

	return attrMap;
}

/*
 * check_attrmap_match
 *
 * Check to see if the map is a one-to-one match, in which case we need
 * not to do a tuple conversion, and the attribute map is not necessary.
 */
static bool
check_attrmap_match(TupleDesc indesc,
					TupleDesc outdesc,
					AttrMap *attrMap)
{
	int			i;

	/* no match if attribute numbers are not the same */
	if (indesc->natts != outdesc->natts)
		return false;

	for (i = 0; i < attrMap->maplen; i++)
	{
		CompactAttribute *inatt = TupleDescCompactAttr(indesc, i);
		CompactAttribute *outatt;

		/*
		 * If the input column has a missing attribute, we need a conversion.
		 */
		if (inatt->atthasmissing)
			return false;

		if (attrMap->attnums[i] == (i + 1))
			continue;

		outatt = TupleDescCompactAttr(outdesc, i);

		/*
		 * If it's a dropped column and the corresponding input column is also
		 * dropped, we don't need a conversion.  However, attlen and
		 * attalignby must agree.
		 */
		if (attrMap->attnums[i] == 0 &&
			inatt->attisdropped &&
			inatt->attlen == outatt->attlen &&
			inatt->attalignby == outatt->attalignby)
			continue;

		return false;
	}

	return true;
}
