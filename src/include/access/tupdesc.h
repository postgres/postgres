/*-------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES tuple descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tupdesc.h,v 1.37 2002/07/20 05:16:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"


typedef struct attrDefault
{
	AttrNumber	adnum;
	char	   *adbin;			/* nodeToString representation of expr */
} AttrDefault;

typedef struct constrCheck
{
	char	   *ccname;
	char	   *ccbin;			/* nodeToString representation of expr */
} ConstrCheck;

/* This structure contains constraints of a tuple */
typedef struct tupleConstr
{
	AttrDefault *defval;		/* array */
	ConstrCheck *check;			/* array */
	uint16		num_defval;
	uint16		num_check;
	bool		has_not_null;
} TupleConstr;

typedef char hasoid_t;
#define WITHOID 'C'
#define WITHOUTOID 'S'
#define UNDEFOID '?'
#define BoolToHasOid(b) ((b) ? WITHOID : WITHOUTOID)
/*
 * This structure contains all information (i.e. from Classes
 * pg_attribute, pg_attrdef, pg_constraint) for a tuple.
 */
typedef struct tupleDesc
{
	int			natts;			/* Number of attributes in the tuple */
	Form_pg_attribute *attrs;
	/* attrs[N] is a pointer to the description of Attribute Number N+1.  */
	TupleConstr *constr;
	hasoid_t	tdhasoid;		/* Tuple has an oid attribute in its header */
}	*TupleDesc;

#ifdef DEBUG_TUPLE_ACCESS

#define AssertTupleDescHasOidIsValid(td) \
	Assert(((td)->tdhasoid == WITHOID) || ((td)->tdhasoid == WITHOUTOID))
#define AssertTupleDescHasOid(td) \
	Assert((td)->tdhasoid == WITHOID)
#define AssertTupleDescHasNoOid(td) \
	Assert((td)->tdhasoid == WITHOUTOID)

#else

#define AssertTupleDescHasOidIsValid(td)
#define AssertTupleDescHasOid(td)
#define AssertTupleDescHasNoOid(td)

#endif

extern TupleDesc CreateTemplateTupleDesc(int natts, hasoid_t withoid);

extern TupleDesc CreateTupleDesc(int natts, Form_pg_attribute *attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

extern void FreeTupleDesc(TupleDesc tupdesc);

extern bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);

extern void TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   char *attributeName,
				   Oid oidtypeid,
				   int32 typmod,
				   int attdim,
				   bool attisset);

extern TupleDesc BuildDescForRelation(List *schema);

#endif   /* TUPDESC_H */
