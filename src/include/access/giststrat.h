/*-------------------------------------------------------------------------
 *
 * giststrat.h
 *	  routines defined in access/gist/giststrat.c
 *
 *
 *
 * rtstrat.h,v 1.2 1995/02/12 02:54:51 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef GISTSTRAT_H
#define GISTSTRAT_H

#include "utils/rel.h"

extern StrategyNumber RelationGetGISTStrategy(Relation r,
						AttrNumber attnum, RegProcedure proc);

#endif	 /* GISTSTRAT_H */
