/*-------------------------------------------------------------------------
 *
 * parse_type.c
 *		handle type operations for parser
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_type.c,v 1.30 2000/05/30 04:24:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
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
	return (SearchSysCacheTuple(TYPEOID,
								ObjectIdGetDatum(id),
								0, 0, 0) != NULL);
}

/* return a type name, given a typeid */
char *
typeidTypeName(Oid id)
{
	HeapTuple	tup;
	Form_pg_type typetuple;

	if (!(tup = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(id),
									0, 0, 0)))
	{
		elog(ERROR, "Unable to locate type oid %u in catalog", id);
		return NULL;
	}
	typetuple = (Form_pg_type) GETSTRUCT(tup);
	return NameStr(typetuple->typname);
}

/* return a Type structure, given a type id */
Type
typeidType(Oid id)
{
	HeapTuple	tup;

	if (!(tup = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(id),
									0, 0, 0)))
	{
		elog(ERROR, "Unable to locate type oid %u in catalog", id);
		return NULL;
	}
	return (Type) tup;
}

/* return a Type structure, given type name */
Type
typenameType(char *s)
{
	HeapTuple	tup;

	if (s == NULL)
		elog(ERROR, "type(): Null type");

	if (!(tup = SearchSysCacheTuple(TYPENAME,
									PointerGetDatum(s),
									0, 0, 0)))
		elog(ERROR, "Unable to locate type name '%s' in catalog", s);
	return (Type) tup;
}

/* given type, return the type OID */
Oid
typeTypeId(Type tp)
{
	if (tp == NULL)
		elog(ERROR, "typeTypeId() called with NULL type struct");
	return tp->t_data->t_oid;
}

/* given type (as type struct), return the length of type */
int16
typeLen(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typlen;
}

/* given type (as type struct), return the value of its 'byval' attribute.*/
bool
typeByVal(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typbyval;
}

/* given type (as type struct), return the name of type */
char *
typeTypeName(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return NameStr(typ->typname);
}

/* given a type, return its typetype ('c' for 'c'atalog types) */
char
typeTypeFlag(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typtype;
}

/* Given a type structure and a string, returns the internal form of
   that string */
Datum
stringTypeDatum(Type tp, char *string, int32 atttypmod)
{
	Oid			op;
	Oid			typelem;

	op = ((Form_pg_type) GETSTRUCT(tp))->typinput;
	typelem = ((Form_pg_type) GETSTRUCT(tp))->typelem;	/* XXX - used for
														 * array_in */
	return OidFunctionCall3(op,
							CStringGetDatum(string),
							ObjectIdGetDatum(typelem),
							Int32GetDatum(atttypmod));
}

/* Given a type id, returns the out-conversion function of the type */
#ifdef NOT_USED
Oid
typeidOutfunc(Oid type_id)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	Oid			outfunc;

	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "typeidOutfunc: Invalid type - oid = %u", type_id);

	type = (Form_pg_type) GETSTRUCT(typeTuple);
	outfunc = type->typoutput;
	return outfunc;
}

#endif

Oid
typeidTypeRelid(Oid type_id)
{
	HeapTuple	typeTuple;
	Form_pg_type type;

	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "typeidTypeRelid: Invalid type - oid = %u", type_id);

	type = (Form_pg_type) GETSTRUCT(typeTuple);
	return type->typrelid;
}

Oid
typeTypeRelid(Type typ)
{
	Form_pg_type typtup;

	typtup = (Form_pg_type) GETSTRUCT(typ);

	return typtup->typrelid;
}

Oid
typeTypElem(Type typ)
{
	Form_pg_type typtup;

	typtup = (Form_pg_type) GETSTRUCT(typ);

	return typtup->typelem;
}

/* Given the attribute type of an array return the attribute type of
   an element of the array */

Oid
GetArrayElementType(Oid typearray)
{
	HeapTuple	type_tuple;
	Form_pg_type type_struct_array;

	type_tuple = SearchSysCacheTuple(TYPEOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "GetArrayElementType: Cache lookup failed for type %u",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (Form_pg_type) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(ERROR, "GetArrayElementType: type %s is not an array",
			 NameStr(type_struct_array->typname));
	}

	return type_struct_array->typelem;
}

/* Given a type structure, return the in-conversion function of the type */
Oid
typeInfunc(Type typ)
{
	Form_pg_type typtup;

	typtup = (Form_pg_type) GETSTRUCT(typ);

	return typtup->typinput;
}

/* Given a type structure, return the out-conversion function of the type */
Oid
typeOutfunc(Type typ)
{
	Form_pg_type typtup;

	typtup = (Form_pg_type) GETSTRUCT(typ);

	return typtup->typoutput;
}
