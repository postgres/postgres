/*-------------------------------------------------------------------------
 *
 * lsyscache.c
 *	  Convenience routines for common queries in the system catalog cache.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/lsyscache.c,v 1.40 2000/02/16 01:00:23 tgl Exp $
 *
 * NOTES
 *	  Eventually, the index information should go through here, too.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*				---------- AMOP CACHES ----------						 */

/*
 * op_class -
 *
 *		Return t iff operator 'opid' is in operator class 'opclass' for
 *		access method 'amopid'.
 *
 */
bool
op_class(Oid opid, Oid opclass, Oid amopid)
{
	if (HeapTupleIsValid(SearchSysCacheTuple(AMOPOPID,
											 ObjectIdGetDatum(opclass),
											 ObjectIdGetDatum(opid),
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
		return pstrdup(NameStr(att_tup->attname));
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

/*
 * get_attdisbursion
 *
 *	  Retrieve the disbursion statistic for an attribute,
 *	  or produce an estimate if no info is available.
 *
 * min_estimate is the minimum estimate to return if insufficient data
 * is available to produce a reliable value.  This value may vary
 * depending on context.  (For example, when deciding whether it is
 * safe to use a hashjoin, we want to be more conservative than when
 * estimating the number of tuples produced by an equijoin.)
 */
double
get_attdisbursion(Oid relid, AttrNumber attnum, double min_estimate)
{
	HeapTuple	atp;
	Form_pg_attribute att_tup;
	double		disbursion;
	int32		ntuples;

	atp = SearchSysCacheTuple(ATTNUM,
							  ObjectIdGetDatum(relid),
							  Int16GetDatum(attnum),
							  0, 0);
	if (!HeapTupleIsValid(atp))
	{
		/* this should not happen */
		elog(ERROR, "get_attdisbursion: no attribute tuple %u %d",
			 relid, attnum);
		return min_estimate;
	}
	att_tup = (Form_pg_attribute) GETSTRUCT(atp);

	disbursion = att_tup->attdisbursion;
	if (disbursion > 0.0)
		return disbursion;		/* we have a specific estimate from VACUUM */

	/*
	 * Special-case boolean columns: the disbursion of a boolean is highly
	 * unlikely to be anywhere near 1/numtuples, instead it's probably more
	 * like 0.5.
	 *
	 * Are there any other cases we should wire in special estimates for?
	 */
	if (att_tup->atttypid == BOOLOID)
		return 0.5;

	/*
	 * Disbursion is either 0 (no data available) or -1 (disbursion
	 * is 1/numtuples).  Either way, we need the relation size.
	 */

	atp = SearchSysCacheTuple(RELOID,
							  ObjectIdGetDatum(relid),
							  0, 0, 0);
	if (!HeapTupleIsValid(atp))
	{
		/* this should not happen */
		elog(ERROR, "get_attdisbursion: no relation tuple %u", relid);
		return min_estimate;
	}

	ntuples = ((Form_pg_class) GETSTRUCT(atp))->reltuples;

	if (ntuples == 0)
		return min_estimate;	/* no data available */

	if (disbursion < 0.0)		/* VACUUM thinks there are no duplicates */
		return 1.0 / (double) ntuples;

	/*
	 * VACUUM ANALYZE does not compute disbursion for system attributes,
	 * but some of them can reasonably be assumed unique anyway.
	 */
	if (attnum == ObjectIdAttributeNumber ||
		attnum == SelfItemPointerAttributeNumber)
		return 1.0 / (double) ntuples;

	/*
	 * VACUUM ANALYZE has not been run for this table.
	 * Produce an estimate = 1/numtuples.  This may produce
	 * unreasonably small estimates for large tables, so limit
	 * the estimate to no less than min_estimate.
	 */
	disbursion = 1.0 / (double) ntuples;
	if (disbursion < min_estimate)
		disbursion = min_estimate;

	return disbursion;
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

	tp = SearchSysCacheTuple(OPEROID,
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

	tp = SearchSysCacheTuple(OPEROID,
							 ObjectIdGetDatum(opno),
							 0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		return pstrdup(NameStr(optup->oprname));
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

	tp = SearchSysCacheTuple(OPEROID,
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

	tp = SearchSysCacheTuple(OPEROID,
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

	if ((optup = SearchSysCacheTuple(OPEROID,
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

	tp = SearchSysCacheTuple(OPEROID,
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

	tp = SearchSysCacheTuple(OPEROID,
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

	tp = SearchSysCacheTuple(OPEROID,
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

	tp = SearchSysCacheTuple(OPEROID,
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

/*				---------- FUNCTION CACHE ----------					 */

/*
 * get_func_rettype
 *		Given procedure id, return the function's result type.
 */
Oid
get_func_rettype(Oid funcid)
{
	HeapTuple	func_tuple;
	Oid			funcrettype;

	func_tuple = SearchSysCacheTuple(PROCOID,
									 ObjectIdGetDatum(funcid),
									 0, 0, 0);

	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "Function OID %u does not exist", funcid);

	funcrettype = (Oid)
		((Form_pg_proc) GETSTRUCT(func_tuple))->prorettype;

	return funcrettype;
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
		return pstrdup(NameStr(reltup->relname));
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

	tp = SearchSysCacheTuple(TYPEOID,
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

	tp = SearchSysCacheTuple(TYPEOID,
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

	tp = SearchSysCacheTuple(TYPEOID,
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
 *	  Given a type OID, return the typdefault field associated with that
 *	  type, or Datum(NULL) if there is no typdefault.  (This implies
 *	  that pass-by-value types can't have a default value that has
 *	  a representation of zero.  Not worth fixing now.)
 *	  The result points to palloc'd storage for non-pass-by-value types.
 */
Datum
get_typdefault(Oid typid)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	struct varlena *typDefault;
	bool		isNull;
	int32		dataSize;
	int32		typLen;
	bool		typByVal;
	Datum		returnValue;

	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(typid),
									0, 0, 0);

	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "get_typdefault: failed to lookup type %u", typid);

	type = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * First, see if there is a non-null typdefault field (usually there isn't)
	 */
	typDefault = (struct varlena *) SysCacheGetAttr(TYPEOID,
													typeTuple,
													Anum_pg_type_typdefault,
													&isNull);

	if (isNull)
		return PointerGetDatum(NULL);

	/*
	 * Otherwise, extract/copy the value.
	 */
	dataSize = VARSIZE(typDefault) - VARHDRSZ;
	typLen = type->typlen;
	typByVal = type->typbyval;

	if (typByVal)
	{
		int8		i8;
		int16		i16;
		int32		i32 = 0;

		if (dataSize == typLen)
		{
			switch (typLen)
			{
				case sizeof(int8):
					memcpy((char *) &i8, VARDATA(typDefault), sizeof(int8));
					i32 = i8;
					break;
				case sizeof(int16):
					memcpy((char *) &i16, VARDATA(typDefault), sizeof(int16));
					i32 = i16;
					break;
				case sizeof(int32):
					memcpy((char *) &i32, VARDATA(typDefault), sizeof(int32));
					break;
			}
			returnValue = Int32GetDatum(i32);
		}
		else
			returnValue = PointerGetDatum(NULL);
	}
	else if (typLen < 0)
	{
		/* variable-size type */
		if (dataSize < 0)
			returnValue = PointerGetDatum(NULL);
		else
		{
			returnValue = PointerGetDatum(palloc(VARSIZE(typDefault)));
			memcpy((char *) DatumGetPointer(returnValue),
				   (char *) typDefault,
				   (int) VARSIZE(typDefault));
		}
	}
	else
	{
		/* fixed-size pass-by-ref type */
		if (dataSize != typLen)
			returnValue = PointerGetDatum(NULL);
		else
		{
			returnValue = PointerGetDatum(palloc(dataSize));
			memcpy((char *) DatumGetPointer(returnValue),
				   VARDATA(typDefault),
				   (int) dataSize);
		}
	}

	return returnValue;
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

	tp = SearchSysCacheTuple(TYPEOID,
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
