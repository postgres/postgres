/*-------------------------------------------------------------------------
 *
 * tupdesc.h--
 *    POSTGRES tuple descriptor definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tupdesc.h,v 1.1 1996/08/27 21:50:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	TUPDESC_H
#define TUPDESC_H

#include "postgres.h"
#include "access/attnum.h"
#include "nodes/pg_list.h"	/* for List */
#include "catalog/pg_attribute.h"

/*
 * a TupleDesc is an array of AttributeTupleForms, each of which is a
 * pointer to a AttributeTupleForm
 */
/* typedef AttributeTupleForm      *TupleDesc; */

/* a TupleDesc is a pointer to a structure which includes an array of */
/* AttributeTupleForms, i.e. pg_attribute information, and the size of */
/* the array, i.e. the number of attributes */
/* in short, a TupleDesc completely captures the attribute information */
/* for a tuple */

typedef struct tupleDesc {
    int  natts;
    AttributeTupleForm *attrs;
} *TupleDesc;

extern TupleDesc CreateTemplateTupleDesc(int natts);

extern TupleDesc CreateTupleDesc(int natts, AttributeTupleForm *attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern bool TupleDescInitEntry(TupleDesc desc,
			       AttrNumber attributeNumber,
			       char *attributeName, 
			       char *typeName, 
			       int attdim, 
			       bool attisset);

extern TupleDesc BuildDescForRelation(List *schema, char *relname);

#endif	/* TUPDESC_H */
