/*-------------------------------------------------------------------------
 *
 * parse_type.h
 *		handle type operations for parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_type.c,v 1.1 1997/11/25 22:05:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "fmgr.h"

#include <catalog/pg_type.h>
#include <parser/parse_target.h>
#include <parser/parse_type.h>
#include "utils/syscache.h"

#ifdef 0
#include "lib/dllist.h"
#include "utils/datum.h"

#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "catalog/catname.h"

#include "catalog/pg_inherits.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/indexing.h"
#include "catalog/catname.h"

#include "access/skey.h"
#include "access/relscan.h"
#include "access/tupdesc.h"
#include "access/htup.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/itup.h"
#include "access/tupmacs.h"

#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "storage/lmgr.h"

#include "port-protos.h"		/* strdup() */
#endif

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
char	   *
typeidTypeName(Oid id)
{
	HeapTuple	tup;
	TypeTupleForm typetuple;

	if (!(tup = SearchSysCacheTuple(TYPOID, ObjectIdGetDatum(id),
									0, 0, 0)))
	{
		elog(WARN, "type id lookup of %ud failed", id);
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
		elog(WARN, "type id lookup of %ud failed", id);
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
		elog(WARN, "type(): Null type");
	}

	if (!(tup = SearchSysCacheTuple(TYPNAME, PointerGetDatum(s), 0, 0, 0)))
	{
		elog(WARN, "type name lookup of %s failed", s);
	}
	return ((Type) tup);
}

/* given type, return the type OID */
Oid
typeTypeId(Type tp)
{
	if (tp == NULL)
		elog(WARN, "typeTypeId() called with NULL type struct");
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
char	   *
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
stringTypeString(Type tp, char *string, int typlen)
{
	Oid			op;
	Oid			typelem;

	op = ((TypeTupleForm) GETSTRUCT(tp))->typinput;
	typelem = ((TypeTupleForm) GETSTRUCT(tp))->typelem;	/* XXX - used for array_in */
	/* typlen is for bpcharin() and varcharin() */
	return ((char *) fmgr(op, string, typelem, typlen));
}

/* Given a type id, returns the out-conversion function of the type */
Oid
typeidRetoutfunc(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			outfunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(WARN, "typeidRetoutfunc: Invalid type - oid = %u", type_id);

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
		elog(WARN, "typeidTypeRelid: Invalid type - oid = %u", type_id);

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
		elog(WARN, "type id lookup of %u failed", type_id);
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
		elog(WARN, "GetArrayElementType: Cache lookup failed for type %d",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(WARN, "GetArrayElementType: type %s is not an array",
			 (Name) &(type_struct_array->typname.data[0]));
	}

	return (type_struct_array->typelem);
}

/* Given a type id, returns the in-conversion function of the type */
Oid
typeidRetinfunc(Oid type_id)
{
	HeapTuple	typeTuple;
	TypeTupleForm type;
	Oid			infunc;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type_id),
									0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(WARN, "typeidRetinfunc: Invalid type - oid = %u", type_id);

	type = (TypeTupleForm) GETSTRUCT(typeTuple);
	infunc = type->typinput;
	return (infunc);
}


#ifdef NOT_USED
char
FindDelimiter(char *typename)
{
	char		delim;
	HeapTuple	typeTuple;
	TypeTupleForm type;


	if (!(typeTuple = SearchSysCacheTuple(TYPNAME,
										  PointerGetDatum(typename),
										  0, 0, 0)))
	{
		elog(WARN, "type name lookup of %s failed", typename);
	}
	type = (TypeTupleForm) GETSTRUCT(typeTuple);

	delim = type->typdelim;
	return (delim);
}

#endif
