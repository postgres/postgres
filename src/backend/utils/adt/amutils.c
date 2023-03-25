/*-------------------------------------------------------------------------
 *
 * amutils.c
 *	  SQL-level APIs related to index access methods.
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/amutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/* Convert string property name to enum, for efficiency */
struct am_propname
{
	const char *name;
	IndexAMProperty prop;
};

static const struct am_propname am_propnames[] =
{
	{
		"asc", AMPROP_ASC
	},
	{
		"desc", AMPROP_DESC
	},
	{
		"nulls_first", AMPROP_NULLS_FIRST
	},
	{
		"nulls_last", AMPROP_NULLS_LAST
	},
	{
		"orderable", AMPROP_ORDERABLE
	},
	{
		"distance_orderable", AMPROP_DISTANCE_ORDERABLE
	},
	{
		"returnable", AMPROP_RETURNABLE
	},
	{
		"search_array", AMPROP_SEARCH_ARRAY
	},
	{
		"search_nulls", AMPROP_SEARCH_NULLS
	},
	{
		"clusterable", AMPROP_CLUSTERABLE
	},
	{
		"index_scan", AMPROP_INDEX_SCAN
	},
	{
		"bitmap_scan", AMPROP_BITMAP_SCAN
	},
	{
		"backward_scan", AMPROP_BACKWARD_SCAN
	},
	{
		"can_order", AMPROP_CAN_ORDER
	},
	{
		"can_unique", AMPROP_CAN_UNIQUE
	},
	{
		"can_multi_col", AMPROP_CAN_MULTI_COL
	},
	{
		"can_exclude", AMPROP_CAN_EXCLUDE
	},
	{
		"can_include", AMPROP_CAN_INCLUDE
	},
};

static IndexAMProperty
lookup_prop_name(const char *name)
{
	int			i;

	for (i = 0; i < lengthof(am_propnames); i++)
	{
		if (pg_strcasecmp(am_propnames[i].name, name) == 0)
			return am_propnames[i].prop;
	}

	/* We do not throw an error, so that AMs can define their own properties */
	return AMPROP_UNKNOWN;
}

/*
 * Common code for properties that are just bit tests of indoptions.
 *
 * tuple: the pg_index heaptuple
 * attno: identify the index column to test the indoptions of.
 * guard: if false, a boolean false result is forced (saves code in caller).
 * iopt_mask: mask for interesting indoption bit.
 * iopt_expect: value for a "true" result (should be 0 or iopt_mask).
 *
 * Returns false to indicate a NULL result (for "unknown/inapplicable"),
 * otherwise sets *res to the boolean value to return.
 */
static bool
test_indoption(HeapTuple tuple, int attno, bool guard,
			   int16 iopt_mask, int16 iopt_expect,
			   bool *res)
{
	Datum		datum;
	int2vector *indoption;
	int16		indoption_val;

	if (!guard)
	{
		*res = false;
		return true;
	}

	datum = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indoption);

	indoption = ((int2vector *) DatumGetPointer(datum));
	indoption_val = indoption->values[attno - 1];

	*res = (indoption_val & iopt_mask) == iopt_expect;

	return true;
}


/*
 * Test property of an index AM, index, or index column.
 *
 * This is common code for different SQL-level funcs, so the amoid and
 * index_oid parameters are mutually exclusive; we look up the amoid from the
 * index_oid if needed, or if no index oid is given, we're looking at AM-wide
 * properties.
 */
static Datum
indexam_property(FunctionCallInfo fcinfo,
				 const char *propname,
				 Oid amoid, Oid index_oid, int attno)
{
	bool		res = false;
	bool		isnull = false;
	int			natts = 0;
	IndexAMProperty prop;
	IndexAmRoutine *routine;

	/* Try to convert property name to enum (no error if not known) */
	prop = lookup_prop_name(propname);

	/* If we have an index OID, look up the AM, and get # of columns too */
	if (OidIsValid(index_oid))
	{
		HeapTuple	tuple;
		Form_pg_class rd_rel;

		Assert(!OidIsValid(amoid));
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(index_oid));
		if (!HeapTupleIsValid(tuple))
			PG_RETURN_NULL();
		rd_rel = (Form_pg_class) GETSTRUCT(tuple);
		if (rd_rel->relkind != RELKIND_INDEX &&
			rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			ReleaseSysCache(tuple);
			PG_RETURN_NULL();
		}
		amoid = rd_rel->relam;
		natts = rd_rel->relnatts;
		ReleaseSysCache(tuple);
	}

	/*
	 * At this point, either index_oid == InvalidOid or it's a valid index
	 * OID. Also, after this test and the one below, either attno == 0 for
	 * index-wide or AM-wide tests, or it's a valid column number in a valid
	 * index.
	 */
	if (attno < 0 || attno > natts)
		PG_RETURN_NULL();

	/*
	 * Get AM information.  If we don't have a valid AM OID, return NULL.
	 */
	routine = GetIndexAmRoutineByAmId(amoid, true);
	if (routine == NULL)
		PG_RETURN_NULL();

	/*
	 * If there's an AM property routine, give it a chance to override the
	 * generic logic.  Proceed if it returns false.
	 */
	if (routine->amproperty &&
		routine->amproperty(index_oid, attno, prop, propname,
							&res, &isnull))
	{
		if (isnull)
			PG_RETURN_NULL();
		PG_RETURN_BOOL(res);
	}

	if (attno > 0)
	{
		HeapTuple	tuple;
		Form_pg_index rd_index;
		bool		iskey = true;

		/*
		 * Handle column-level properties. Many of these need the pg_index row
		 * (which we also need to use to check for nonkey atts) so we fetch
		 * that first.
		 */
		tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
		if (!HeapTupleIsValid(tuple))
			PG_RETURN_NULL();
		rd_index = (Form_pg_index) GETSTRUCT(tuple);

		Assert(index_oid == rd_index->indexrelid);
		Assert(attno > 0 && attno <= rd_index->indnatts);

		isnull = true;

		/*
		 * If amcaninclude, we might be looking at an attno for a nonkey
		 * column, for which we (generically) assume that most properties are
		 * null.
		 */
		if (routine->amcaninclude
			&& attno > rd_index->indnkeyatts)
			iskey = false;

		switch (prop)
		{
			case AMPROP_ASC:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_DESC, 0, &res))
					isnull = false;
				break;

			case AMPROP_DESC:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_DESC, INDOPTION_DESC, &res))
					isnull = false;
				break;

			case AMPROP_NULLS_FIRST:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_NULLS_FIRST, INDOPTION_NULLS_FIRST, &res))
					isnull = false;
				break;

			case AMPROP_NULLS_LAST:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_NULLS_FIRST, 0, &res))
					isnull = false;
				break;

			case AMPROP_ORDERABLE:

				/*
				 * generic assumption is that nonkey columns are not orderable
				 */
				res = iskey ? routine->amcanorder : false;
				isnull = false;
				break;

			case AMPROP_DISTANCE_ORDERABLE:

				/*
				 * The conditions for whether a column is distance-orderable
				 * are really up to the AM (at time of writing, only GiST
				 * supports it at all). The planner has its own idea based on
				 * whether it finds an operator with amoppurpose 'o', but
				 * getting there from just the index column type seems like a
				 * lot of work. So instead we expect the AM to handle this in
				 * its amproperty routine. The generic result is to return
				 * false if the AM says it never supports this, or if this is
				 * a nonkey column, and null otherwise (meaning we don't
				 * know).
				 */
				if (!iskey || !routine->amcanorderbyop)
				{
					res = false;
					isnull = false;
				}
				break;

			case AMPROP_RETURNABLE:

				/* note that we ignore iskey for this property */

				isnull = false;
				res = false;

				if (routine->amcanreturn)
				{
					/*
					 * If possible, the AM should handle this test in its
					 * amproperty function without opening the rel. But this
					 * is the generic fallback if it does not.
					 */
					Relation	indexrel = index_open(index_oid, AccessShareLock);

					res = index_can_return(indexrel, attno);
					index_close(indexrel, AccessShareLock);
				}
				break;

			case AMPROP_SEARCH_ARRAY:
				if (iskey)
				{
					res = routine->amsearcharray;
					isnull = false;
				}
				break;

			case AMPROP_SEARCH_NULLS:
				if (iskey)
				{
					res = routine->amsearchnulls;
					isnull = false;
				}
				break;

			default:
				break;
		}

		ReleaseSysCache(tuple);

		if (!isnull)
			PG_RETURN_BOOL(res);
		PG_RETURN_NULL();
	}

	if (OidIsValid(index_oid))
	{
		/*
		 * Handle index-level properties.  Currently, these only depend on the
		 * AM, but that might not be true forever, so we make users name an
		 * index not just an AM.
		 */
		switch (prop)
		{
			case AMPROP_CLUSTERABLE:
				PG_RETURN_BOOL(routine->amclusterable);

			case AMPROP_INDEX_SCAN:
				PG_RETURN_BOOL(routine->amgettuple ? true : false);

			case AMPROP_BITMAP_SCAN:
				PG_RETURN_BOOL(routine->amgetbitmap ? true : false);

			case AMPROP_BACKWARD_SCAN:
				PG_RETURN_BOOL(routine->amcanbackward);

			default:
				PG_RETURN_NULL();
		}
	}

	/*
	 * Handle AM-level properties (those that control what you can say in
	 * CREATE INDEX).
	 */
	switch (prop)
	{
		case AMPROP_CAN_ORDER:
			PG_RETURN_BOOL(routine->amcanorder);

		case AMPROP_CAN_UNIQUE:
			PG_RETURN_BOOL(routine->amcanunique);

		case AMPROP_CAN_MULTI_COL:
			PG_RETURN_BOOL(routine->amcanmulticol);

		case AMPROP_CAN_EXCLUDE:
			PG_RETURN_BOOL(routine->amgettuple ? true : false);

		case AMPROP_CAN_INCLUDE:
			PG_RETURN_BOOL(routine->amcaninclude);

		default:
			PG_RETURN_NULL();
	}
}

/*
 * Test property of an AM specified by AM OID
 */
Datum
pg_indexam_has_property(PG_FUNCTION_ARGS)
{
	Oid			amoid = PG_GETARG_OID(0);
	char	   *propname = text_to_cstring(PG_GETARG_TEXT_PP(1));

	return indexam_property(fcinfo, propname, amoid, InvalidOid, 0);
}

/*
 * Test property of an index specified by index OID
 */
Datum
pg_index_has_property(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *propname = text_to_cstring(PG_GETARG_TEXT_PP(1));

	return indexam_property(fcinfo, propname, InvalidOid, relid, 0);
}

/*
 * Test property of an index column specified by index OID and column number
 */
Datum
pg_index_column_has_property(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		attno = PG_GETARG_INT32(1);
	char	   *propname = text_to_cstring(PG_GETARG_TEXT_PP(2));

	/* Reject attno 0 immediately, so that attno > 0 identifies this case */
	if (attno <= 0)
		PG_RETURN_NULL();

	return indexam_property(fcinfo, propname, InvalidOid, relid, attno);
}

/*
 * Return the name of the given phase, as used for progress reporting by the
 * given AM.
 */
Datum
pg_indexam_progress_phasename(PG_FUNCTION_ARGS)
{
	Oid			amoid = PG_GETARG_OID(0);
	int32		phasenum = PG_GETARG_INT32(1);
	IndexAmRoutine *routine;
	char	   *name;

	routine = GetIndexAmRoutineByAmId(amoid, true);
	if (routine == NULL || !routine->ambuildphasename)
		PG_RETURN_NULL();

	name = routine->ambuildphasename(phasenum);
	if (!name)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(CStringGetTextDatum(name));
}
