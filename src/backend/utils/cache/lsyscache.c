/*-------------------------------------------------------------------------
 *
 * lsyscache.c
 *	  Routines to access information within system caches
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/lsyscache.c,v 1.29 1999/07/15 22:40:04 momjian Exp $
 *
 * NOTES
 *	  Eventually, the index information should go through here, too.
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "utils/syscache.h"
#include "utils/lsyscache.h"

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
op_class(Oid oprno, int32 opclass, Oid amopid)
{
	if (HeapTupleIsValid(SearchSysCacheTuple(AMOPOPID,
											 ObjectIdGetDatum(opclass),
											 ObjectIdGetDatum(oprno),
											 ObjectIdGetDatum(amopid),
											 0)))
		return true;
	else
		return false;
}

/*				---------- ATTRIBUTE CACHES ----------					 */

/*
 * get_attname -
 *
 *		Given the relation id and the attribute number,
 *		return the "attname" field from the attribute relation.
 *
 */
char *
get_attname(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(ATTNUM,
							 ObjectIdGetDatum(relid),
							 UInt16GetDatum(attnum),
							 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		return pstrdup(att_tup->attname.data);
	}
	else
		return NULL;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(ATTNAME,
							 ObjectIdGetDatum(relid),
							 PointerGetDatum(attname),
							 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		return att_tup->attnum;
	}
	else
		return InvalidAttrNumber;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(ATTNUM,
							 ObjectIdGetDatum(relid),
							 UInt16GetDatum(attnum),
							 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		return att_tup->atttypid;
	}
	else
		return InvalidOid;
}

/* This routine uses the attname instead of the attnum because it
 * replaces the routine find_atttype, which is called sometimes when
 * only the attname, not the attno, is available.
 */
bool
get_attisset(Oid relid, char *attname)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(ATTNAME,
							 ObjectIdGetDatum(relid),
							 PointerGetDatum(attname),
							 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		return att_tup->attisset;
	}
	else
		return false;
}

/*
 * get_atttypmod -
 *
 *		Given the relation id and the attribute number,
 *		return the "atttypmod" field from the attribute relation.
 *
 */
int32
get_atttypmod(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(ATTNUM,
							 ObjectIdGetDatum(relid),
							 UInt16GetDatum(attnum),
							 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		return att_tup->atttypmod;
	}
	else
		return -1;
}

/*				---------- INDEX CACHE ----------						 */

/*		watch this space...
 */

/*				---------- OPERATOR CACHE ----------					 */

/*
 * get_opcode -
 *
 *		Returns the regproc id of the routine used to implement an
 *		operator given the operator oid.
 *
 */
RegProcedure
get_opcode(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		return optup->oprcode;
	}
	else
		return (RegProcedure) NULL;
}

/*
 * get_opname -
 *	  returns the name of the operator with the given opno
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such operator.
 */
char *
get_opname(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		return pstrdup(optup->oprname.data);
	}
	else
		return NULL;
}

/*
 * op_mergejoinable -
 *
 *		Returns the left and right sort operators and types corresponding to a
 *		mergejoinable operator, or nil if the operator is not mergejoinable.
 *
 */
bool
op_mergejoinable(Oid opno, Oid ltype, Oid rtype, Oid *leftOp, Oid *rightOp)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

		if (optup->oprlsortop &&
			optup->oprrsortop &&
			optup->oprleft == ltype &&
			optup->oprright == rtype)
		{
			*leftOp = ObjectIdGetDatum(optup->oprlsortop);
			*rightOp = ObjectIdGetDatum(optup->oprrsortop);
			return true;
		}
	}
	return false;
}

/*
 * op_hashjoinable
 *
 * Returns the hash operator corresponding to a hashjoinable operator,
 * or nil if the operator is not hashjoinable.
 *
 */
Oid
op_hashjoinable(Oid opno, Oid ltype, Oid rtype)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

		if (optup->oprcanhash &&
			optup->oprleft == ltype &&
			optup->oprright == rtype)
			return opno;
	}
	return InvalidOid;
}

HeapTuple
get_operator_tuple(Oid opno)
{
	HeapTuple	optup;

	if ((optup = SearchSysCacheTuple(OPROID,
									 ObjectIdGetDatum(opno),
									 0, 0, 0)))
		return optup;
	else
		return (HeapTuple) NULL;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		return optup->oprcom;
	}
	else
		return InvalidOid;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		return optup->oprnegate;
	}
	else
		return InvalidOid;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		return optup->oprrest;
	}
	else
		return (RegProcedure) NULL;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(OPROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		return optup->oprjoin;
	}
	else
		return (RegProcedure) NULL;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(RELOID,
							 ObjectIdGetDatum(relid),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		return reltup->relnatts;
	}
	else
		return InvalidAttrNumber;
}

/*
 * get_rel_name -
 *
 *		Returns the name of a given relation.
 *
 */
char *
get_rel_name(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(RELOID,
							 ObjectIdGetDatum(relid),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		return pstrdup(reltup->relname.data);
	}
	else
		return NULL;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(TYPOID,
							 ObjectIdGetDatum(typid),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		return typtup->typlen;
	}
	else
		return 0;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(TYPOID,
							 ObjectIdGetDatum(typid),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		return (bool) typtup->typbyval;
	}
	else
		return false;
}

#ifdef NOT_USED
char
get_typalign(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCacheTuple(TYPOID,
							 ObjectIdGetDatum(typid),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		return typtup->typalign;
	}
	else
		return 'i';
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
	struct varlena *typdefault = (struct varlena *) TypeDefaultRetrieve(typid);

	return typdefault;
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
	HeapTuple	tp;

	tp = SearchSysCacheTuple(TYPOID,
							 ObjectIdGetDatum(typid),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		return typtup->typtype;
	}
	else
		return '\0';
}

#endif
