/*-------------------------------------------------------------------------
 *
 * tupdesc.c
 *	  POSTGRES tuple descriptor support code
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/tupdesc.c,v 1.82 2002/07/31 17:19:51 tgl Exp $
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
#include "utils/syscache.h"


/* ----------------------------------------------------------------
 *		CreateTemplateTupleDesc
 *
 *		This function allocates and zeros a tuple descriptor structure.
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTemplateTupleDesc(int natts, hasoid_t withoid)
{
	uint32		size;
	TupleDesc	desc;

	/*
	 * sanity checks
	 */
	AssertArg(natts >= 1);

	/*
	 * allocate enough memory for the tuple descriptor and zero it as
	 * TupleDescInitEntry assumes that the descriptor is filled with NULL
	 * pointers.
	 */
	size = natts * sizeof(Form_pg_attribute);
	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->attrs = (Form_pg_attribute *) palloc(size);
	desc->constr = NULL;
	MemSet(desc->attrs, 0, size);

	desc->natts = natts;
	desc->tdhasoid = withoid;

	return desc;
}

/* ----------------------------------------------------------------
 *		CreateTupleDesc
 *
 *		This function allocates a new TupleDesc from Form_pg_attribute array
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDesc(int natts, Form_pg_attribute *attrs)
{
	TupleDesc	desc;

	/*
	 * sanity checks
	 */
	AssertArg(natts >= 1);

	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->attrs = attrs;
	desc->natts = natts;
	desc->constr = NULL;
	desc->tdhasoid = UNDEFOID;

	return desc;
}

/* ----------------------------------------------------------------
 *		CreateTupleDescCopy
 *
 *		This function creates a new TupleDesc by copying from an existing
 *		TupleDesc
 *
 *		!!! Constraints are not copied !!!
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDescCopy(TupleDesc tupdesc)
{
	TupleDesc	desc;
	int			i,
				size;

	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->natts = tupdesc->natts;
	size = desc->natts * sizeof(Form_pg_attribute);
	desc->attrs = (Form_pg_attribute *) palloc(size);
	for (i = 0; i < desc->natts; i++)
	{
		desc->attrs[i] = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
		memmove(desc->attrs[i],
				tupdesc->attrs[i],
				ATTRIBUTE_TUPLE_SIZE);
		desc->attrs[i]->attnotnull = false;
		desc->attrs[i]->atthasdef = false;
	}
	desc->constr = NULL;
	desc->tdhasoid = tupdesc->tdhasoid;

	return desc;
}

/* ----------------------------------------------------------------
 *		CreateTupleDescCopyConstr
 *
 *		This function creates a new TupleDesc by copying from an existing
 *		TupleDesc (with Constraints)
 *
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDescCopyConstr(TupleDesc tupdesc)
{
	TupleDesc	desc;
	TupleConstr *constr = tupdesc->constr;
	int			i,
				size;

	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->natts = tupdesc->natts;
	size = desc->natts * sizeof(Form_pg_attribute);
	desc->attrs = (Form_pg_attribute *) palloc(size);
	for (i = 0; i < desc->natts; i++)
	{
		desc->attrs[i] = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
		memmove(desc->attrs[i],
				tupdesc->attrs[i],
				ATTRIBUTE_TUPLE_SIZE);
	}
	if (constr)
	{
		TupleConstr *cpy = (TupleConstr *) palloc(sizeof(TupleConstr));

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

	desc->tdhasoid = tupdesc->tdhasoid;
	return desc;
}

void
FreeTupleDesc(TupleDesc tupdesc)
{
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
		pfree(tupdesc->attrs[i]);
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

bool
equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	int			i,
				j,
				n;

	if (tupdesc1->natts != tupdesc2->natts)
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
		 */
		if (strcmp(NameStr(attr1->attname), NameStr(attr2->attname)) != 0)
			return false;
		if (attr1->atttypid != attr2->atttypid)
			return false;
		if (attr1->attstattarget != attr2->attstattarget)
			return false;
		if (attr1->atttypmod != attr2->atttypmod)
			return false;
		if (attr1->attstorage != attr2->attstorage)
			return false;
		if (attr1->attnotnull != attr2->attnotnull)
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
				   char *attributeName,
				   Oid oidtypeid,
				   int32 typmod,
				   int attdim,
				   bool attisset)
{
	HeapTuple	tuple;
	Form_pg_type typeForm;
	Form_pg_attribute att;

	/*
	 * sanity checks
	 */
	AssertArg(PointerIsValid(desc));
	AssertArg(attributeNumber >= 1);

	/*
	 * attributeName's are sometimes NULL, from resdom's.  I don't know
	 * why that is, though -- Jolly
	 */
/*	  AssertArg(NameIsValid(attributeName));*/

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

	if (attributeName != NULL)
		namestrcpy(&(att->attname), attributeName);
	else
		MemSet(NameStr(att->attname), 0, NAMEDATALEN);

	att->attstattarget = -1;
	att->attcacheoff = -1;
	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;
	att->attisset = attisset;

	att->attnotnull = false;
	att->atthasdef = false;

	tuple = SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(oidtypeid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "Unable to look up type id %u", oidtypeid);

	/*
	 * type info exists so we initialize our attribute information from
	 * the type tuple we found..
	 */
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	att->atttypid = HeapTupleGetOid(tuple);

	/*
	 * There are a couple of cases where we must override the information
	 * stored in pg_type.
	 *
	 * First: if this attribute is a set, what is really stored in the
	 * attribute is the OID of a tuple in the pg_proc catalog. The pg_proc
	 * tuple contains the query string which defines this set - i.e., the
	 * query to run to get the set. So the atttypid (just assigned above)
	 * refers to the type returned by this query, but the actual length of
	 * this attribute is the length (size) of an OID.
	 *
	 * (Why not just make the atttypid point to the OID type, instead of the
	 * type the query returns?	Because the executor uses the atttypid to
	 * tell the front end what type will be returned,
	 * and in the end the type returned will be the result of the query,
	 * not an OID.)
	 *
	 * (Why not wait until the return type of the set is known (i.e., the
	 * recursive call to the executor to execute the set has returned)
	 * before telling the front end what the return type will be?  Because
	 * the executor is a delicate thing, and making sure that the correct
	 * order of front-end commands is maintained is messy, especially
	 * considering that target lists may change as inherited attributes
	 * are considered, etc.  Ugh.)
	 *
	 * Second: if we are dealing with a complex type (a tuple type), then
	 * pg_type will say that the representation is the same as Oid.  But
	 * if typmod is sizeof(Pointer) then the internal representation is
	 * actually a pointer to a TupleTableSlot, and we have to substitute
	 * that information.
	 *
	 * A set of complex type is first and foremost a set, so its
	 * representation is Oid not pointer.  So, test that case first.
	 */
	if (attisset)
	{
		att->attlen = sizeof(Oid);
		att->attbyval = true;
		att->attalign = 'i';
		att->attstorage = 'p';
	}
	else if (typeForm->typtype == 'c' && typmod == sizeof(Pointer))
	{
		att->attlen = sizeof(Pointer);
		att->attbyval = true;
		att->attalign = 'd';	/* kluge to work with 8-byte pointers */
		/* XXX ought to have a separate attalign value for pointers ... */
		att->attstorage = 'p';
	}
	else
	{
		att->attlen = typeForm->typlen;
		att->attbyval = typeForm->typbyval;
		att->attalign = typeForm->typalign;
		att->attstorage = typeForm->typstorage;
	}

	ReleaseSysCache(tuple);
}


/*
 * BuildDescForRelation
 *
 * Given a relation schema (list of ColumnDef nodes), build a TupleDesc.
 */
TupleDesc
BuildDescForRelation(List *schema)
{
	int			natts;
	AttrNumber	attnum;
	List	   *p;
	TupleDesc	desc;
	AttrDefault *attrdef = NULL;
	TupleConstr *constr = (TupleConstr *) palloc(sizeof(TupleConstr));
	char	   *attname;
	int32		atttypmod;
	int			attdim;
	int			ndef = 0;
	bool		attisset;

	/*
	 * allocate a new tuple descriptor
	 */
	natts = length(schema);
	desc = CreateTemplateTupleDesc(natts, UNDEFOID);
	constr->has_not_null = false;

	attnum = 0;

	foreach(p, schema)
	{
		ColumnDef  *entry = lfirst(p);

		/*
		 * for each entry in the list, get the name and type information
		 * from the list and have TupleDescInitEntry fill in the attribute
		 * information we need.
		 */
		attnum++;

		attname = entry->colname;
		attisset = entry->typename->setof;
		atttypmod = entry->typename->typmod;
		attdim = length(entry->typename->arrayBounds);

		TupleDescInitEntry(desc, attnum, attname,
						   typenameTypeId(entry->typename),
						   atttypmod, attdim, attisset);

		/* This is for constraints */
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
RelationNameGetTupleDesc(char *relname)
{
	RangeVar   *relvar;
	Relation	rel;
	TupleDesc	tupdesc;
	List	   *relname_list;

	/* Open relation and get the tuple description */
	relname_list = stringToQualifiedNameList(relname, "RelationNameGetTupleDesc");
	relvar = makeRangeVarFromNameList(relname_list);
	rel = heap_openrv(relvar, AccessShareLock);
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
 * attnames.
 *
 * If the type is a base type, a single item alias List is required.
 */
TupleDesc
TypeGetTupleDesc(Oid typeoid, List *colaliases)
{
	Oid			relid = typeidTypeRelid(typeoid);
	TupleDesc	tupdesc;

	/*
	 * Build a suitable tupledesc representing the output rows
	 */
	if (OidIsValid(relid))
	{
		/* Composite data type, i.e. a table's row type */
		Relation	rel;
		int			natts;

		rel = relation_open(relid, AccessShareLock);
		tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
		natts = tupdesc->natts;
		relation_close(rel, AccessShareLock);

		/* check to see if we've given column aliases */
		if(colaliases != NIL)
		{
			char	   *label;
			int			varattno;

			/* does the List length match the number of attributes */
			if (length(colaliases) != natts)
				elog(ERROR, "TypeGetTupleDesc: number of aliases does not match number of attributes");

			/* OK, use the aliases instead */
			for (varattno = 0; varattno < natts; varattno++)
			{
				label = strVal(nth(varattno, colaliases));

				if (label != NULL)
					namestrcpy(&(tupdesc->attrs[varattno]->attname), label);
				else
					MemSet(NameStr(tupdesc->attrs[varattno]->attname), 0, NAMEDATALEN);
			}
		}
	}
	else
	{
		/* Must be a base data type, i.e. scalar */
		char	   *attname;

		/* the alias List is required for base types */
		if (colaliases == NIL)
			elog(ERROR, "TypeGetTupleDesc: no column alias was provided");

		/* the alias List length must be 1 */
		if (length(colaliases) != 1)
			elog(ERROR, "TypeGetTupleDesc: number of aliases does not match number of attributes");

		/* OK, get the column alias */
		attname = strVal(lfirst(colaliases));

		tupdesc = CreateTemplateTupleDesc(1, WITHOUTOID);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   typeoid,
						   -1,
						   0,
						   false);
	}

	return tupdesc;
}
