/*-------------------------------------------------------------------------
 *
 * lsyscache.c--
 *	  Routines to access information within system caches
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/lsyscache.c,v 1.7 1997/11/17 16:59:23 momjian Exp $
 *
 * NOTES
 *	  Eventually, the index information should go through here, too.
 *
 *	  Most of these routines call SearchSysCacheStruct() and thus simply
 *	  (1) allocate some space for the return struct and (2) call it.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "nodes/pg_list.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "access/tupmacs.h"
#include "utils/rel.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "access/attnum.h"
#include "access/heapam.h"

#include "catalog/pg_amop.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"

/*				---------- AMOP CACHES ----------						 */

/*
 * op_class -
 *
 *		Return t iff operator 'opno' is in operator class 'opclass'.
 *
 */
bool
op_class(Oid opno, int32 opclass, Oid amopid)
{
	FormData_pg_amop amoptup;

	if (SearchSysCacheStruct(AMOPOPID,
							 (char *) &amoptup,
							 ObjectIdGetDatum(opclass),
							 ObjectIdGetDatum(opno),
							 ObjectIdGetDatum(amopid),
							 0))
		return (true);
	else
		return (false);
}

/*				---------- ATTRIBUTE CACHES ----------					 */

/*
 * get_attname -
 *
 *		Given the relation id and the attribute number,
 *		return the "attname" field from the attribute relation.
 *
 */
char	   *
get_attname(Oid relid, AttrNumber attnum)
{
	FormData_pg_attribute att_tup;
	char	   *retval;

	if (SearchSysCacheStruct(ATTNUM,
							 (char *) &att_tup,
							 ObjectIdGetDatum(relid),
							 UInt16GetDatum(attnum),
							 0, 0))
	{
		retval = pstrdup(att_tup.attname.data);

		return (retval);
	}
	else
		return (NULL);
}

/*
 * get_attnum -
 *
 *		Given the relation id and the attribute name,
 *		return the "attnum" field from the attribute relation.
 *
 */
AttrNumber
get_attnum(Oid relid, char *attname)
{
	FormData_pg_attribute att_tup;

	if (SearchSysCacheStruct(ATTNAME, (char *) &att_tup,
							 ObjectIdGetDatum(relid),
							 PointerGetDatum(attname),
							 0, 0))
		return (att_tup.attnum);
	else
		return (InvalidAttrNumber);
}

/*
 * get_atttype -
 *
 *		Given the relation OID and the attribute number with the relation,
 *		return the attribute type OID.
 *
 */
Oid
get_atttype(Oid relid, AttrNumber attnum)
{
	AttributeTupleForm att_tup = (AttributeTupleForm) palloc(sizeof(*att_tup));

	if (SearchSysCacheStruct(ATTNUM,
							 (char *) att_tup,
							 ObjectIdGetDatum(relid),
							 UInt16GetDatum(attnum),
							 0, 0))
		return (att_tup->atttypid);
	else
		return ((Oid) NULL);
}

/* This routine uses the attname instead of the attnum because it
 * replaces the routine find_atttype, which is called sometimes when
 * only the attname, not the attno, is available.
 */
bool
get_attisset(Oid relid, char *attname)
{
	HeapTuple	htup;
	AttrNumber	attno;
	AttributeTupleForm att_tup;

	attno = get_attnum(relid, attname);

	htup = SearchSysCacheTuple(ATTNAME,
							   ObjectIdGetDatum(relid),
							   PointerGetDatum(attname),
							   0, 0);
	if (!HeapTupleIsValid(htup))
		elog(WARN, "get_attisset: no attribute %s in relation %d",
			 attname, relid);
	if (heap_attisnull(htup, attno))
		return (false);
	else
	{
		att_tup = (AttributeTupleForm) GETSTRUCT(htup);
		return (att_tup->attisset);
	}
}

/*				---------- INDEX CACHE ----------						 */

/*		watch this space...
 */

/*				---------- OPERATOR CACHE ----------					 */

/*
 * get_opcode -
 *
 *		Returns the regproc id of the routine used to implement an
 *		operator given the operator uid.
 *
 */
RegProcedure
get_opcode(Oid opno)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0))
		return (optup.oprcode);
	else
		return ((RegProcedure) NULL);
}

/*
 * get_opname -
 *	  returns the name of the operator with the given opno
 *
 * Note: return the struct so that it gets copied.
 */
char	   *
get_opname(Oid opno)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0))
		return (pstrdup(optup.oprname.data));
	else
	{
		elog(WARN, "can't look up operator %d\n", opno);
		return NULL;
	}
}

/*
 * op_mergesortable -
 *
 *		Returns the left and right sort operators and types corresponding to a
 *		mergesortable operator, or nil if the operator is not mergesortable.
 *
 */
bool
op_mergesortable(Oid opno, Oid ltype, Oid rtype, Oid *leftOp, Oid *rightOp)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0) &&
		optup.oprlsortop &&
		optup.oprrsortop &&
		optup.oprleft == ltype &&
		optup.oprright == rtype)
	{

		*leftOp = ObjectIdGetDatum(optup.oprlsortop);
		*rightOp = ObjectIdGetDatum(optup.oprrsortop);
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

/*
 * op_hashjoinable--
 *
 * Returns the hash operator corresponding to a hashjoinable operator,
 * or nil if the operator is not hashjoinable.
 *
 */
Oid
op_hashjoinable(Oid opno, Oid ltype, Oid rtype)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0) &&
		optup.oprcanhash &&
		optup.oprleft == ltype &&
		optup.oprright == rtype)
		return (opno);
	else
		return (InvalidOid);
}

/*
 * get_commutator -
 *
 *		Returns the corresponding commutator of an operator.
 *
 */
Oid
get_commutator(Oid opno)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0))
		return (optup.oprcom);
	else
		return ((Oid) NULL);
}

HeapTuple
get_operator_tuple(Oid opno)
{
	HeapTuple	optup;

	if ((optup = SearchSysCacheTuple(OPROID,
									 ObjectIdGetDatum(opno),
									 0, 0, 0)))
		return (optup);
	else
		return ((HeapTuple) NULL);
}

/*
 * get_negator -
 *
 *		Returns the corresponding negator of an operator.
 *
 */
Oid
get_negator(Oid opno)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0))
		return (optup.oprnegate);
	else
		return ((Oid) NULL);
}

/*
 * get_oprrest -
 *
 *		Returns procedure id for computing selectivity of an operator.
 *
 */
RegProcedure
get_oprrest(Oid opno)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0))
		return (optup.oprrest);
	else
		return ((RegProcedure) NULL);
}

/*
 * get_oprjoin -
 *
 *		Returns procedure id for computing selectivity of a join.
 *
 */
RegProcedure
get_oprjoin(Oid opno)
{
	FormData_pg_operator optup;

	if (SearchSysCacheStruct(OPROID, (char *) &optup,
							 ObjectIdGetDatum(opno),
							 0, 0, 0))
		return (optup.oprjoin);
	else
		return ((RegProcedure) NULL);
}

/*				---------- RELATION CACHE ----------					 */

/*
 * get_relnatts -
 *
 *		Returns the number of attributes for a given relation.
 *
 */
int
get_relnatts(Oid relid)
{
	FormData_pg_class reltup;

	if (SearchSysCacheStruct(RELOID, (char *) &reltup,
							 ObjectIdGetDatum(relid),
							 0, 0, 0))
		return (reltup.relnatts);
	else
		return (InvalidAttrNumber);
}

/*
 * get_rel_name -
 *
 *		Returns the name of a given relation.
 *
 */
char	   *
get_rel_name(Oid relid)
{
	FormData_pg_class reltup;

	if ((SearchSysCacheStruct(RELOID,
							  (char *) &reltup,
							  ObjectIdGetDatum(relid),
							  0, 0, 0)))
	{
		return (pstrdup(reltup.relname.data));
	}
	else
		return (NULL);
}

/*				---------- TYPE CACHE ----------						 */

/*
 * get_typlen -
 *
 *		Given the type OID, return the length of the type.
 *
 */
int16
get_typlen(Oid typid)
{
	TypeTupleFormData typtup;

	if (SearchSysCacheStruct(TYPOID, (char *) &typtup,
							 ObjectIdGetDatum(typid),
							 0, 0, 0))
		return (typtup.typlen);
	else
		return ((int16) NULL);
}

/*
 * get_typbyval -
 *
 *		Given the type OID, determine whether the type is returned by value or
 *		not.  Returns 1 if by value, 0 if by reference.
 *
 */
bool
get_typbyval(Oid typid)
{
	TypeTupleFormData typtup;

	if (SearchSysCacheStruct(TYPOID, (char *) &typtup,
							 ObjectIdGetDatum(typid),
							 0, 0, 0))
		return ((bool) typtup.typbyval);
	else
		return (false);
}

/*
 * get_typbyval -
 *
 *		Given the type OID, determine whether the type is returned by value or
 *		not.  Returns 1 if by value, 0 if by reference.
 *
 */
#ifdef NOT_USED
char
get_typalign(Oid typid)
{
	TypeTupleFormData typtup;

	if (SearchSysCacheStruct(TYPOID, (char *) &typtup,
							 ObjectIdGetDatum(typid),
							 0, 0, 0))
		return (typtup.typalign);
	else
		return ('i');
}

#endif

/*
 * get_typdefault -
 *
 *		Given the type OID, return the default value of the ADT.
 *
 */
struct varlena *
get_typdefault(Oid typid)
{
	struct varlena *typdefault =
	(struct varlena *) TypeDefaultRetrieve(typid);

	return (typdefault);
}

/*
 * get_typtype -
 *
 *		Given the type OID, find if it is a basic type, a named relation
 *		or the generic type 'relation'.
 *		It returns the null char if the cache lookup fails...
 *
 */
#ifdef NOT_USED
char
get_typtype(Oid typid)
{
	TypeTupleFormData typtup;

	if (SearchSysCacheStruct(TYPOID, (char *) &typtup,
							 ObjectIdGetDatum(typid),
							 0, 0, 0))
	{
		return (typtup.typtype);
	}
	else
	{
		return ('\0');
	}
}

#endif
