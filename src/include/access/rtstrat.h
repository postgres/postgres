/*-------------------------------------------------------------------------
 *
 * rtstrat.h--
 *	  routines defined in access/rtree/rtstrat.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtstrat.h,v 1.8 1998/09/01 04:34:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSTRAT_H
#define RTSTRAT_H

#include <utils/rel.h>
#include <access/attnum.h>

extern RegProcedure RTMapOperator(Relation r, AttrNumber attnum,
			  RegProcedure proc);

#endif	 /* RTSTRAT_H */
