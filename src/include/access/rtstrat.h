/*-------------------------------------------------------------------------
 *
 * rtstrat.h--
 *	  routines defined in access/rtree/rtstrat.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtstrat.h,v 1.2 1997/09/07 04:56:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSTRAT_H

extern RegProcedure
RTMapOperator(Relation r, AttrNumber attnum,
			  RegProcedure proc);

#endif							/* RTSTRAT_H */
