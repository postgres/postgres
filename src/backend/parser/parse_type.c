/*-------------------------------------------------------------------------
 *
 * parse_type.c
 *		handle type operations for parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_type.c,v 1.10 1998/05/29 14:00:24 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "fmgr.h"

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/parse_node.h"

#include "catalog/pg_type.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/syscache.h"


/* check to see if a type id is valid,
 * returns true if it is. By using this call before calling
 * typeidType or typeidTypeName, more meaningful error messages
 * can be produced because the caller typically has more context of
 *	what's going on                 - jolly
 */
bool
typeidIsValid(Oid id)
{
	return (SearchSysCacheTuple(TYPOID,
								ObjectIdGetDatum(id),
								0, 0, 0) != NULL);
}

/* return a type name, given a typeid */
char *
typeidTypeName(Oid id)
{
	HeapTuple	tup;
	TypeTupleForm typetuple;

	if (!(tup = SearchSysCacheTuple(TYPOID, ObjectIdGetDatum(id),
									0, 0, 0)))
	{
		elog(ERROR, "type id lookup of %u failed", id);
		return (NULL);
	}
	typetuple = (TypeTupleForm) GETSTRUCT(tup);
	return (typetuple->typname).data;
}

/* return a Type structure, given an typid */
Type
typeidType(Oid id)
{
	HeapTuple	tup;

	if (!(tup = SearchSysCacheTuple(TYPOID, ObjectIdGetDatum(id),
									0, 0, 0)))
	{
		elog(ERROR, "type id lookup of %u failed", id);
		return (NULL);
	}
	return ((Type) tup);
}

/* return a Type structure, given type name */
Type
typenameType(char *s)
{
	HeapTuple	tup;

	if (s == NULL)
	{
		elog(ERROR, "type(): Null type");
	}

	if (!(tup = SearchSysCacheTuple(TYPNAME, PointerGetDatum(s), 0, 0, 0)))
	{
		elog(ERROR, "type name lookup of %s failed", s);
	}
	return ((Type) tup);
}

/* given type, return the type OID */
Oid
typeTypeId(Type tp)
{
	if (tp == NULL)
		elog(ERROR, "typeTypeId() called with NULL type struct");
	return (tp->t_oid);
}

/* given type (as type struct), return the length of type */
int16
typeLen(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typlen);
}

/* given type (as type struct), return the value of its 'byval' attribute.*/
bool
typeByVal(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typbyval);
}

/* given type (as type struct), return the name of type */
char *
typeTypeName(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typname).data;
}

/* given a type, return its typetype ('c' for 'c'atalog types) */
char
typeTypeFlag(Type t)
{
	TypeTupleForm typ;

	typ = (TypeTupleForm) GETSTRUCT(t);
	return (typ->typtype);
}

/* Given a type structure and a string, returns the internal form of
   that string */
char *
stringTypeString(Type tp, char *string, int16 atttypmod)
{
	Oid			op;
	Oid			typelem;

	op = ((TypeTupleForm) GETSTRUCT(tp))->typinput;
	typelem = ((TypeTupleForm) GETSTRUCT(tp))->typelem; /* XXX - used for
														 * array_in */
	return ((char *) fmgr(op, string, typelem, atttypmod));
}

/* Given a type id, returns the out-conversion function of the type */
Oid
typeidOutfunc(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			outfunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "typeidOutfunc: Invalid type - oid = %u", type_id);

	type = (TypeTupleForm) GETSTRUCT(typeTuple);
	outfunc = type->typoutput;
	return (outfunc);
}

Oid
typeidTypeRelid(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			infunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "typeidTypeRelid: Invalid type - oid = %u", type_id);

	type = (TypeTupleForm) GETSTRUCT(typeTuple);
	infunc = type->typrelid;
	return (infunc);
}

Oid
typeTypeRelid(Type typ)
{
	TypeTupleForm typtup;

	typtup = (TypeTupleForm) GETSTRUCT(typ);

	return (typtup->typrelid);
}

Oid
typeidTypElem(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;

	if (!(typeTuple = SearchSysCacheTuple(TYPOID,
										  ObjectIdGetDatum(type_id),
										  0, 0, 0)))
	{
		elog(ERROR, "type id lookup of %u failed", type_id);
	}
	type = (TypeTupleForm) GETSTRUCT(typeTuple);

	return (type->typelem);
}

/* Given the attribute type of an array return the arrtribute type of
   an element of the array */

Oid
GetArrayElementType(Oid typearray)
{
	HeapTuple	type_tuple;
	TypeTupleForm type_struct_array;

	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "GetArrayElementType: Cache lookup failed for type %d",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(ERROR, "GetArrayElementType: type %s is not an array",
			 (Name) &(type_struct_array->typname.data[0]));
	}

	return (type_struct_array->typelem);
}

/* Given a type id, returns the in-conversion function of the type */
Oid
typeidInfunc(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			infunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "typeidInfunc: Invalid type - oid = %u", type_id);

	type = (TypeTupleForm) GETSTRUCT(typeTuple);
	infunc = type->typinput;
	return (infunc);
}
