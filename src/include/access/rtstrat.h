/*-------------------------------------------------------------------------
 *
 * rtstrat.h--
 *	  routines defined in access/rtree/rtstrat.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtstrat.h,v 1.6 1998/01/24 22:48:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSTRAT_H
#define RTSTRAT_H

#include <utils/rel.h>
#include <access/attnum.h>

extern RegProcedure RTMapOperator(Relation r, AttrNumber attnum,
			  RegProcedure proc);

#endif							/* RTSTRAT_H */
