/*-------------------------------------------------------------------------
 *
 * tupdesc.h--
 *	  POSTGRES tuple descriptor definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tupdesc.h,v 1.18 1998/08/19 02:03:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include <nodes/pg_list.h>
#include <access/attnum.h>
#include <catalog/pg_attribute.h>


typedef struct attrDefault
{
	AttrNumber	adnum;
	char	   *adbin;
	char	   *adsrc;
} AttrDefault;

typedef struct constrCheck
{
	char	   *ccname;
	char	   *ccbin;
	char	   *ccsrc;
} ConstrCheck;

/* This structure contains constraints of a tuple */
typedef struct tupleConstr
{
	AttrDefault *defval;
	ConstrCheck *check;
	uint16		num_defval;
	uint16		num_check;
	bool		has_not_null;
} TupleConstr;

/*
 * This structure contains all information (i.e. from Classes
 * pg_attribute, pg_attrdef, pg_relcheck) for a tuple.
 */
typedef struct tupleDesc
{
	int			natts;
	/* Number of attributes in the tuple */
	AttributeTupleForm *attrs;
	/* attrs[N] is a pointer to the description of Attribute Number N+1.  */
	TupleConstr *constr;
} *TupleDesc;

extern TupleDesc CreateTemplateTupleDesc(int natts);

extern TupleDesc CreateTupleDesc(int natts, AttributeTupleForm *attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

extern void FreeTupleDesc(TupleDesc tupdesc);

extern bool
TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   char *attributeName,
				   Oid typeid,
				   int32 typmod,
				   int attdim,
				   bool attisset);

extern TupleDesc BuildDescForRelation(List *schema, char *relname);

#endif							/* TUPDESC_H */
