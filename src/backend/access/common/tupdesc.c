/*-------------------------------------------------------------------------
 *
 * tupdesc.c
 *	  POSTGRES tuple descriptor support code
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/common/tupdesc.c,v 1.107 2004/10/20 16:04:47 tgl Exp $
 *
 * NOTES
 *	  some of the executor utility code such as "ExecTypeFromTL" should be
 *	  moved here.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* ----------------------------------------------------------------
 *		CreateTemplateTupleDesc
 *
 *		This function allocates and zeros a tuple descriptor structure.
 *
 * Tuple type ID information is initially set for an anonymous record type;
 * caller can overwrite this if needed.
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTemplateTupleDesc(int natts, bool hasoid)
{
	TupleDesc	desc;

	/*
	 * sanity checks
	 */
	AssertArg(natts >= 0);

	/*
	 * Allocate enough memory for the tuple descriptor, and zero the
	 * attrs[] array since TupleDescInitEntry assumes that the array is
	 * filled with NULL pointers.
	 */
	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));

	if (natts > 0)
		desc->attrs = (Form_pg_attribute *)
			palloc0(natts * sizeof(Form_pg_attribute));
	else
		desc->attrs = NULL;

	/*
	 * Initialize other fields of the tupdesc.
	 */
	desc->natts = natts;
	desc->constr = NULL;
	desc->tdtypeid = RECORDOID;
	desc->tdtypmod = -1;
	desc->tdhasoid = hasoid;

	return desc;
}

/* ----------------------------------------------------------------
 *		CreateTupleDesc
 *
 *		This function allocates a new TupleDesc pointing to a given
 *		Form_pg_attribute array
 *
 * Tuple type ID information is initially set for an anonymous record type;
 * caller can overwrite this if needed.
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDesc(int natts, bool hasoid, Form_pg_attribute *attrs)
{
	TupleDesc	desc;

	/*
	 * sanity checks
	 */
	AssertArg(natts >= 0);

	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->attrs = attrs;
	desc->natts = natts;
	desc->constr = NULL;
	desc->tdtypeid = RECORDOID;
	desc->tdtypmod = -1;
	desc->tdhasoid = hasoid;

	return desc;
}

/* ----------------------------------------------------------------
 *		CreateTupleDescCopy
 *
 *		This function creates a new TupleDesc by copying from an existing
 *		TupleDesc
 *
 *		!!! Constraints and defaults are not copied !!!
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDescCopy(TupleDesc tupdesc)
{
	TupleDesc	desc;
	int			i;

	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->natts = tupdesc->natts;
	if (desc->natts > 0)
	{
		desc->attrs = (Form_pg_attribute *)
			palloc(desc->natts * sizeof(Form_pg_attribute));
		for (i = 0; i < desc->natts; i++)
		{
			desc->attrs[i] = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
			memcpy(desc->attrs[i], tupdesc->attrs[i], ATTRIBUTE_TUPLE_SIZE);
			desc->attrs[i]->attnotnull = false;
			desc->attrs[i]->atthasdef = false;
		}
	}
	else
		desc->attrs = NULL;

	desc->constr = NULL;

	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;
	desc->tdhasoid = tupdesc->tdhasoid;

	return desc;
}

/* ----------------------------------------------------------------
 *		CreateTupleDescCopyConstr
 *
 *		This function creates a new TupleDesc by copying from an existing
 *		TupleDesc (including its constraints and defaults)
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDescCopyConstr(TupleDesc tupdesc)
{
	TupleDesc	desc;
	TupleConstr *constr = tupdesc->constr;
	int			i;

	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->natts = tupdesc->natts;
	if (desc->natts > 0)
	{
		desc->attrs = (Form_pg_attribute *)
			palloc(desc->natts * sizeof(Form_pg_attribute));
		for (i = 0; i < desc->natts; i++)
		{
			desc->attrs[i] = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
			memcpy(desc->attrs[i], tupdesc->attrs[i], ATTRIBUTE_TUPLE_SIZE);
		}
	}
	else
		desc->attrs = NULL;

	if (constr)
	{
		TupleConstr *cpy = (TupleConstr *) palloc0(sizeof(TupleConstr));

		cpy->has_not_null = constr->has_not_null;

		if ((cpy->num_defval = constr->num_defval) > 0)
		{
			cpy->defval = (AttrDefault *) palloc(cpy->num_defval * sizeof(AttrDefault));
			memcpy(cpy->defval, constr->defval, cpy->num_defval * sizeof(AttrDefault));
			for (i = cpy->num_defval - 1; i >= 0; i--)
			{
				if (constr->defval[i].adbin)
					cpy->defval[i].adbin = pstrdup(constr->defval[i].adbin);
			}
		}

		if ((cpy->num_check = constr->num_check) > 0)
		{
			cpy->check = (ConstrCheck *) palloc(cpy->num_check * sizeof(ConstrCheck));
			memcpy(cpy->check, constr->check, cpy->num_check * sizeof(ConstrCheck));
			for (i = cpy->num_check - 1; i >= 0; i--)
			{
				if (constr->check[i].ccname)
					cpy->check[i].ccname = pstrdup(constr->check[i].ccname);
				if (constr->check[i].ccbin)
					cpy->check[i].ccbin = pstrdup(constr->check[i].ccbin);
			}
		}

		desc->constr = cpy;
	}
	else
		desc->constr = NULL;

	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;
	desc->tdhasoid = tupdesc->tdhasoid;

	return desc;
}

/*
 * Free a TupleDesc including all substructure
 */
void
FreeTupleDesc(TupleDesc tupdesc)
{
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
		pfree(tupdesc->attrs[i]);
	if (tupdesc->attrs)
		pfree(tupdesc->attrs);
	if (tupdesc->constr)
	{
		if (tupdesc->constr->num_defval > 0)
		{
			AttrDefault *attrdef = tupdesc->constr->defval;

			for (i = tupdesc->constr->num_defval - 1; i >= 0; i--)
			{
				if (attrdef[i].adbin)
					pfree(attrdef[i].adbin);
			}
			pfree(attrdef);
		}
		if (tupdesc->constr->num_check > 0)
		{
			ConstrCheck *check = tupdesc->constr->check;

			for (i = tupdesc->constr->num_check - 1; i >= 0; i--)
			{
				if (check[i].ccname)
					pfree(check[i].ccname);
				if (check[i].ccbin)
					pfree(check[i].ccbin);
			}
			pfree(check);
		}
		pfree(tupdesc->constr);
	}

	pfree(tupdesc);
}

/*
 * Compare two TupleDesc structures for logical equality
 *
 * Note: we deliberately do not check the attrelid and tdtypmod fields.
 * This allows typcache.c to use this routine to see if a cached record type
 * matches a requested type, and is harmless for relcache.c's uses.
 */
bool
equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	int			i,
				j,
				n;

	if (tupdesc1->natts != tupdesc2->natts)
		return false;
	if (tupdesc1->tdtypeid != tupdesc2->tdtypeid)
		return false;
	if (tupdesc1->tdhasoid != tupdesc2->tdhasoid)
		return false;

	for (i = 0; i < tupdesc1->natts; i++)
	{
		Form_pg_attribute attr1 = tupdesc1->attrs[i];
		Form_pg_attribute attr2 = tupdesc2->attrs[i];

		/*
		 * We do not need to check every single field here: we can
		 * disregard attrelid, attnum (it was used to place the row in the
		 * attrs array) and everything derived from the column datatype.
		 * Also, attcacheoff must NOT be checked since it's possibly not
		 * set in both copies.
		 */
		if (strcmp(NameStr(attr1->attname), NameStr(attr2->attname)) != 0)
			return false;
		if (attr1->atttypid != attr2->atttypid)
			return false;
		if (attr1->attstattarget != attr2->attstattarget)
			return false;
		if (attr1->attndims != attr2->attndims)
			return false;
		if (attr1->atttypmod != attr2->atttypmod)
			return false;
		if (attr1->attstorage != attr2->attstorage)
			return false;
		if (attr1->attnotnull != attr2->attnotnull)
			return false;
		if (attr1->atthasdef != attr2->atthasdef)
			return false;
		if (attr1->attisdropped != attr2->attisdropped)
			return false;
		if (attr1->attislocal != attr2->attislocal)
			return false;
		if (attr1->attinhcount != attr2->attinhcount)
			return false;
	}

	if (tupdesc1->constr != NULL)
	{
		TupleConstr *constr1 = tupdesc1->constr;
		TupleConstr *constr2 = tupdesc2->constr;

		if (constr2 == NULL)
			return false;
		if (constr1->has_not_null != constr2->has_not_null)
			return false;
		n = constr1->num_defval;
		if (n != (int) constr2->num_defval)
			return false;
		for (i = 0; i < n; i++)
		{
			AttrDefault *defval1 = constr1->defval + i;
			AttrDefault *defval2 = constr2->defval;

			/*
			 * We can't assume that the items are always read from the
			 * system catalogs in the same order; so use the adnum field
			 * to identify the matching item to compare.
			 */
			for (j = 0; j < n; defval2++, j++)
			{
				if (defval1->adnum == defval2->adnum)
					break;
			}
			if (j >= n)
				return false;
			if (strcmp(defval1->adbin, defval2->adbin) != 0)
				return false;
		}
		n = constr1->num_check;
		if (n != (int) constr2->num_check)
			return false;
		for (i = 0; i < n; i++)
		{
			ConstrCheck *check1 = constr1->check + i;
			ConstrCheck *check2 = constr2->check;

			/*
			 * Similarly, don't assume that the checks are always read in
			 * the same order; match them up by name and contents. (The
			 * name *should* be unique, but...)
			 */
			for (j = 0; j < n; check2++, j++)
			{
				if (strcmp(check1->ccname, check2->ccname) == 0 &&
					strcmp(check1->ccbin, check2->ccbin) == 0)
					break;
			}
			if (j >= n)
				return false;
		}
	}
	else if (tupdesc2->constr != NULL)
		return false;
	return true;
}

/* ----------------------------------------------------------------
 *		TupleDescInitEntry
 *
 *		This function initializes a single attribute structure in
 *		a preallocated tuple descriptor.
 * ----------------------------------------------------------------
 */
void
TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   const char *attributeName,
				   Oid oidtypeid,
				   int32 typmod,
				   int attdim)
{
	HeapTuple	tuple;
	Form_pg_type typeForm;
	Form_pg_attribute att;

	/*
	 * sanity checks
	 */
	AssertArg(PointerIsValid(desc));
	AssertArg(attributeNumber >= 1);
	AssertArg(attributeNumber <= desc->natts);
	AssertArg(!PointerIsValid(desc->attrs[attributeNumber - 1]));

	/*
	 * allocate storage for this attribute
	 */

	att = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
	desc->attrs[attributeNumber - 1] = att;

	/*
	 * initialize the attribute fields
	 */
	att->attrelid = 0;			/* dummy value */

	/*
	 * Note: attributeName can be NULL, because the planner doesn't always
	 * fill in valid resname values in targetlists, particularly for
	 * resjunk attributes.
	 */
	if (attributeName != NULL)
		namestrcpy(&(att->attname), attributeName);
	else
		MemSet(NameStr(att->attname), 0, NAMEDATALEN);

	att->attstattarget = -1;
	att->attcacheoff = -1;
	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;

	att->attnotnull = false;
	att->atthasdef = false;
	att->attisdropped = false;
	att->attislocal = true;
	att->attinhcount = 0;

	tuple = SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(oidtypeid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", oidtypeid);
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	att->atttypid = oidtypeid;
	att->attlen = typeForm->typlen;
	att->attbyval = typeForm->typbyval;
	att->attalign = typeForm->typalign;
	att->attstorage = typeForm->typstorage;

	ReleaseSysCache(tuple);
}


/*
 * BuildDescForRelation
 *
 * Given a relation schema (list of ColumnDef nodes), build a TupleDesc.
 *
 * Note: the default assumption is no OIDs; caller may modify the returned
 * TupleDesc if it wants OIDs.	Also, tdtypeid will need to be filled in
 * later on.
 */
TupleDesc
BuildDescForRelation(List *schema)
{
	int			natts;
	AttrNumber	attnum;
	ListCell   *l;
	TupleDesc	desc;
	AttrDefault *attrdef = NULL;
	TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));
	char	   *attname;
	int32		atttypmod;
	int			attdim;
	int			ndef = 0;

	/*
	 * allocate a new tuple descriptor
	 */
	natts = list_length(schema);
	desc = CreateTemplateTupleDesc(natts, false);
	constr->has_not_null = false;

	attnum = 0;

	foreach(l, schema)
	{
		ColumnDef  *entry = lfirst(l);

		/*
		 * for each entry in the list, get the name and type information
		 * from the list and have TupleDescInitEntry fill in the attribute
		 * information we need.
		 */
		attnum++;

		attname = entry->colname;
		atttypmod = entry->typename->typmod;
		attdim = list_length(entry->typename->arrayBounds);

		if (entry->typename->setof)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" cannot be declared SETOF",
							attname)));

		TupleDescInitEntry(desc, attnum, attname,
						   typenameTypeId(entry->typename),
						   atttypmod, attdim);

		/* Fill in additional stuff not handled by TupleDescInitEntry */
		if (entry->is_not_null)
			constr->has_not_null = true;
		desc->attrs[attnum - 1]->attnotnull = entry->is_not_null;

		/*
		 * Note we copy only pre-cooked default expressions. Digestion of
		 * raw ones is someone else's problem.
		 */
		if (entry->cooked_default != NULL)
		{
			if (attrdef == NULL)
				attrdef = (AttrDefault *) palloc(natts * sizeof(AttrDefault));
			attrdef[ndef].adnum = attnum;
			attrdef[ndef].adbin = pstrdup(entry->cooked_default);
			ndef++;
			desc->attrs[attnum - 1]->atthasdef = true;
		}

		desc->attrs[attnum - 1]->attislocal = entry->is_local;
		desc->attrs[attnum - 1]->attinhcount = entry->inhcount;
	}

	if (constr->has_not_null || ndef > 0)
	{
		desc->constr = constr;

		if (ndef > 0)			/* DEFAULTs */
		{
			if (ndef < natts)
				constr->defval = (AttrDefault *)
					repalloc(attrdef, ndef * sizeof(AttrDefault));
			else
				constr->defval = attrdef;
			constr->num_defval = ndef;
		}
		else
		{
			constr->defval = NULL;
			constr->num_defval = 0;
		}
		constr->check = NULL;
		constr->num_check = 0;
	}
	else
	{
		pfree(constr);
		desc->constr = NULL;
	}

	return desc;
}


/*
 * RelationNameGetTupleDesc
 *
 * Given a (possibly qualified) relation name, build a TupleDesc.
 */
TupleDesc
RelationNameGetTupleDesc(const char *relname)
{
	RangeVar   *relvar;
	Relation	rel;
	TupleDesc	tupdesc;
	List	   *relname_list;

	/* Open relation and copy the tuple description */
	relname_list = stringToQualifiedNameList(relname, "RelationNameGetTupleDesc");
	relvar = makeRangeVarFromNameList(relname_list);
	rel = relation_openrv(relvar, AccessShareLock);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	relation_close(rel, AccessShareLock);

	return tupdesc;
}

/*
 * TypeGetTupleDesc
 *
 * Given a type Oid, build a TupleDesc.
 *
 * If the type is composite, *and* a colaliases List is provided, *and*
 * the List is of natts length, use the aliases instead of the relation
 * attnames.  (NB: this usage is deprecated since it may result in
 * creation of unnecessary transient record types.)
 *
 * If the type is a base type, a single item alias List is required.
 */
TupleDesc
TypeGetTupleDesc(Oid typeoid, List *colaliases)
{
	TypeFuncClass functypclass = get_type_func_class(typeoid);
	TupleDesc	tupdesc = NULL;

	/*
	 * Build a suitable tupledesc representing the output rows
	 */
	if (functypclass == TYPEFUNC_COMPOSITE)
	{
		/* Composite data type, e.g. a table's row type */
		tupdesc = CreateTupleDescCopy(lookup_rowtype_tupdesc(typeoid, -1));

		if (colaliases != NIL)
		{
			int			natts = tupdesc->natts;
			int			varattno;

			/* does the list length match the number of attributes? */
			if (list_length(colaliases) != natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("number of aliases does not match number of columns")));

			/* OK, use the aliases instead */
			for (varattno = 0; varattno < natts; varattno++)
			{
				char	   *label = strVal(list_nth(colaliases, varattno));

				if (label != NULL)
					namestrcpy(&(tupdesc->attrs[varattno]->attname), label);
			}

			/* The tuple type is now an anonymous record type */
			tupdesc->tdtypeid = RECORDOID;
			tupdesc->tdtypmod = -1;
		}
	}
	else if (functypclass == TYPEFUNC_SCALAR)
	{
		/* Base data type, i.e. scalar */
		char	   *attname;

		/* the alias list is required for base types */
		if (colaliases == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("no column alias was provided")));

		/* the alias list length must be 1 */
		if (list_length(colaliases) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("number of aliases does not match number of columns")));

		/* OK, get the column alias */
		attname = strVal(linitial(colaliases));

		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   typeoid,
						   -1,
						   0);
	}
	else if (functypclass == TYPEFUNC_RECORD)
	{
		/* XXX can't support this because typmod wasn't passed in ... */
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not determine row description for function returning record")));
	}
	else
	{
		/* crummy error message, but parser should have caught this */
		elog(ERROR, "function in FROM has unsupported return type");
	}

	return tupdesc;
}
