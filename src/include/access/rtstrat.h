/*-------------------------------------------------------------------------
 *
 * rtstrat.h
 *	  routines defined in access/rtree/rtstrat.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtstrat.h,v 1.9.2.1 1999/08/02 05:25:22 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSTRAT_H
#define RTSTRAT_H

#include "utils/rel.h"

extern RegProcedure RTMapOperator(Relation r, AttrNumber attnum,
			  RegProcedure proc);

#endif	 /* RTSTRAT_H */
