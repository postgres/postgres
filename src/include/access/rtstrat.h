/*-------------------------------------------------------------------------
 *
 * rtstrat.h--
 *	  routines defined in access/rtree/rtstrat.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtstrat.h,v 1.3 1997/09/08 02:34:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSTRAT_H

extern		RegProcedure
RTMapOperator(Relation r, AttrNumber attnum,
			  RegProcedure proc);

#endif							/* RTSTRAT_H */
