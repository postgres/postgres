/*-------------------------------------------------------------------------
 *
 * rtstrat.h--
 *    routines defined in access/rtree/rtstrat.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtstrat.h,v 1.1 1996/08/27 21:50:23 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSTRAT_H

extern RegProcedure RTMapOperator(Relation r,  AttrNumber attnum,
				  RegProcedure proc);

#endif /* RTSTRAT_H */
