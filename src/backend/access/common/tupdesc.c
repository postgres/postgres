/*-------------------------------------------------------------------------
 *
 * tupdesc.c
 *	  POSTGRES tuple descriptor support code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/tupdesc.c,v 1.48 1999/02/13 23:14:14 momjian Exp $
 *
 * NOTES
 *	  some of the executor utility code such as "ExecTypeFromTL" should be
 *	  moved here.
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>

#include <postgres.h>

#include <catalog/pg_type.h>
#include <nodes/parsenodes.h>
#include <parser/parse_type.h>
#include <utils/builtins.h>
#include <utils/fcache.h>
#include <utils/syscache.h>

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

/* ----------------------------------------------------------------
 *		CreateTemplateTupleDesc
 *
 *		This function allocates and zeros a tuple descriptor structure.
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTemplateTupleDesc(int natts)
{
	uint32		size;
	TupleDesc	desc;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	AssertArg(natts >= 1);

	/* ----------------
	 *	allocate enough memory for the tuple descriptor and
	 *	zero it as TupleDescInitEntry assumes that the descriptor
	 *	is filled with NULL pointers.
	 * ----------------
	 */
	size = natts * sizeof(Form_pg_attribute);
	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->attrs = (Form_pg_attribute *) palloc(size);
	desc->constr = NULL;
	MemSet(desc->attrs, 0, size);

	desc->natts = natts;

	return desc;
}

/* ----------------------------------------------------------------
 *		CreateTupleDesc
 *
 *		This function allocates a new TupleDesc from Form_pg_attribute array
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDesc(int natts, Form_pg_attribute * attrs)
{
	TupleDesc	desc;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	AssertArg(natts >= 1);

	desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
	desc->attrs = attrs;
	desc->natts = natts;
	desc->constr = NULL;

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
				if (constr->defval[i].adsrc)
					cpy->defval[i].adsrc = pstrdup(constr->defval[i].adsrc);
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
				if (constr->check[i].ccsrc)
					cpy->check[i].ccsrc = pstrdup(constr->check[i].ccsrc);
			}
		}

		desc->constr = cpy;
	}
	else
		desc->constr = NULL;

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
				if (attrdef[i].adsrc)
					pfree(attrdef[i].adsrc);
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
				if (check[i].ccsrc)
					pfree(check[i].ccsrc);
			}
			pfree(check);
		}
		pfree(tupdesc->constr);
	}

	pfree(tupdesc);

}

/* ----------------------------------------------------------------
 *		TupleDescInitEntry
 *
 *		This function initializes a single attribute structure in
 *		a preallocated tuple descriptor.
 * ----------------------------------------------------------------
 */
bool
TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   char *attributeName,
				   Oid typeid,
				   int32 typmod,
				   int attdim,
				   bool attisset)
{
	HeapTuple	tuple;
	Form_pg_type typeForm;
	Form_pg_attribute att;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	AssertArg(PointerIsValid(desc));
	AssertArg(attributeNumber >= 1);

	/*
	 * attributeName's are sometimes NULL, from resdom's.  I don't know
	 * why that is, though -- Jolly
	 */
/*	  AssertArg(NameIsValid(attributeName));*/

	AssertArg(!PointerIsValid(desc->attrs[attributeNumber - 1]));


	/* ----------------
	 *	allocate storage for this attribute
	 * ----------------
	 */

	att = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
	desc->attrs[attributeNumber - 1] = att;

	/* ----------------
	 *	initialize some of the attribute fields
	 * ----------------
	 */
	att->attrelid = 0;			/* dummy value */

	if (attributeName != NULL)
		namestrcpy(&(att->attname), attributeName);
	else
		MemSet(att->attname.data, 0, NAMEDATALEN);


	att->attdisbursion = 0;		/* dummy value */
	att->attcacheoff = -1;
	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attnelems = attdim;
	att->attisset = attisset;

	att->attnotnull = false;
	att->atthasdef = false;

	/* ----------------
	 *	search the system cache for the type tuple of the attribute
	 *	we are creating so that we can get the typeid and some other
	 *	stuff.
	 *
	 *	Note: in the special case of
	 *
	 *		create EMP (name = text, manager = EMP)
	 *
	 *	RelationNameCreateHeapRelation() calls BuildDesc() which
	 *	calls this routine and since EMP does not exist yet, the
	 *	system cache lookup below fails.  That's fine, but rather
	 *	then doing a elog(ERROR) we just leave that information
	 *	uninitialized, return false, then fix things up later.
	 *	-cim 6/14/90
	 * ----------------
	 */
	tuple = SearchSysCacheTuple(TYPOID,
								ObjectIdGetDatum(typeid),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		/* ----------------
		 *	 here type info does not exist yet so we just fill
		 *	 the attribute with dummy information and return false.
		 * ----------------
		 */
		att->atttypid = InvalidOid;
		att->attlen = (int16) 0;
		att->attbyval = (bool) 0;
		att->attalign = 'i';
		return false;
	}

	/* ----------------
	 *	type info exists so we initialize our attribute
	 *	information from the type tuple we found..
	 * ----------------
	 */
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	att->atttypid = tuple->t_data->t_oid;
	att->attalign = typeForm->typalign;

	/* ------------------------
	   If this attribute is a set, what is really stored in the
	   attribute is the OID of a tuple in the pg_proc catalog.
	   The pg_proc tuple contains the query string which defines
	   this set - i.e., the query to run to get the set.
	   So the atttypid (just assigned above) refers to the type returned
	   by this query, but the actual length of this attribute is the
	   length (size) of an OID.

	   Why not just make the atttypid point to the OID type, instead
	   of the type the query returns?  Because the executor uses the atttypid
	   to tell the front end what type will be returned (in BeginCommand),
	   and in the end the type returned will be the result of the query, not
	   an OID.

	   Why not wait until the return type of the set is known (i.e., the
	   recursive call to the executor to execute the set has returned)
	   before telling the front end what the return type will be?  Because
	   the executor is a delicate thing, and making sure that the correct
	   order of front-end commands is maintained is messy, especially
	   considering that target lists may change as inherited attributes
	   are considered, etc.  Ugh.
	   -----------------------------------------
	   */
	if (attisset)
	{
		Type		t = typeidType(OIDOID);

		att->attlen = typeLen(t);
		att->attbyval = typeByVal(t);
	}
	else
	{
		att->attlen = typeForm->typlen;
		att->attbyval = typeForm->typbyval;
	}


	return true;
}


/* ----------------------------------------------------------------
 *		TupleDescMakeSelfReference
 *
 *		This function initializes a "self-referential" attribute like
 *		manager in "create EMP (name=text, manager = EMP)".
 *		It calls TypeShellMake() which inserts a "shell" type
 *		tuple into pg_type.  A self-reference is one kind of set, so
 *		its size and byval are the same as for a set.  See the comments
 *		above in TupleDescInitEntry.
 * ----------------------------------------------------------------
 */
static void
TupleDescMakeSelfReference(TupleDesc desc,
						   AttrNumber attnum,
						   char *relname)
{
	Form_pg_attribute att;
	Type		t = typeidType(OIDOID);

	att = desc->attrs[attnum - 1];
	att->atttypid = TypeShellMake(relname);
	att->attlen = typeLen(t);
	att->attbyval = typeByVal(t);
	att->attnelems = 0;
}

/* ----------------------------------------------------------------
 *		BuildDescForRelation
 *
 *		This is a general purpose function identical to BuildDesc
 *		but is used by the DefineRelation() code to catch the
 *		special case where you
 *
 *				create FOO ( ..., x = FOO )
 *
 *		here, the initial type lookup for "x = FOO" will fail
 *		because FOO isn't in the catalogs yet.  But since we
 *		are creating FOO, instead of doing an elog() we add
 *		a shell type tuple to pg_type and fix things later
 *		in amcreate().
 * ----------------------------------------------------------------
 */
TupleDesc
BuildDescForRelation(List *schema, char *relname)
{
	int			natts;
	AttrNumber	attnum;
	List	   *p;
	TupleDesc	desc;
	AttrDefault *attrdef = NULL;
	TupleConstr *constr = (TupleConstr *) palloc(sizeof(TupleConstr));
	char	   *attname;
	char	   *typename;
	int32		atttypmod;
	int			attdim;
	int			ndef = 0;
	bool		attisset;

	/* ----------------
	 *	allocate a new tuple descriptor
	 * ----------------
	 */
	natts = length(schema);
	desc = CreateTemplateTupleDesc(natts);
	constr->has_not_null = false;

	attnum = 0;

	typename = palloc(NAMEDATALEN);

	foreach(p, schema)
	{
		ColumnDef  *entry;
		List	   *arry;

		/* ----------------
		 *		for each entry in the list, get the name and type
		 *		information from the list and have TupleDescInitEntry
		 *		fill in the attribute information we need.
		 * ----------------
		 */
		attnum++;

		entry = lfirst(p);
		attname = entry->colname;
		arry = entry->typename->arrayBounds;
		attisset = entry->typename->setof;
		atttypmod = entry->typename->typmod;

		if (arry != NIL)
		{
			/* array of XXX is _XXX */
			snprintf(typename, NAMEDATALEN,
							 "_%.*s", NAMEDATALEN - 2, entry->typename->name);
			attdim = length(arry);
		}
		else
		{
			StrNCpy(typename, entry->typename->name, NAMEDATALEN);
			attdim = 0;
		}

		if (!TupleDescInitEntry(desc, attnum, attname,
								typeTypeId(typenameType(typename)),
								atttypmod, attdim, attisset))
		{
			/* ----------------
			 *	if TupleDescInitEntry() fails, it means there is
			 *	no type in the system catalogs.  So now we check if
			 *	the type name equals the relation name.  If so we
			 *	have a self reference, otherwise it's an error.
			 * ----------------
			 */
			if (!strcmp(typename, relname))
				TupleDescMakeSelfReference(desc, attnum, relname);
			else
				elog(ERROR, "DefineRelation: no such type %s",
					 typename);
		}

		desc->attrs[attnum - 1]->atttypmod = entry->typename->typmod;

		/* This is for constraints */
		if (entry->is_not_null)
			constr->has_not_null = true;
		desc->attrs[attnum - 1]->attnotnull = entry->is_not_null;

		if (entry->defval != NULL)
		{
			if (attrdef == NULL)
				attrdef = (AttrDefault *) palloc(natts * sizeof(AttrDefault));
			attrdef[ndef].adnum = attnum;
			attrdef[ndef].adbin = NULL;
			attrdef[ndef].adsrc = entry->defval;
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
			constr->num_defval = 0;
		constr->num_check = 0;
	}
	else
	{
		pfree(constr);
		desc->constr = NULL;
	}
	return desc;
}
