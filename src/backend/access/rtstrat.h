/*-------------------------------------------------------------------------
 *
 * rtstrat.h--
 *    routines defined in access/rtree/rtstrat.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtstrat.h,v 1.1.1.1 1996/07/09 06:21:09 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSTRAT_H

extern RegProcedure RTMapOperator(Relation r,  AttrNumber attnum,
				  RegProcedure proc);

#endif /* RTSTRAT_H */
