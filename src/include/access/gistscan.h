/*-------------------------------------------------------------------------
 *
 * gistscan.h
 *	  routines defined in access/gisr/gistscan.c
 *
 *
 *
 * rtscan.h,v 1.2 1995/06/14 00:06:58 jolly Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef GISTSCAN_H
#define GISTSCAN_H

#include "access/relscan.h"

extern Datum gistbeginscan(PG_FUNCTION_ARGS);
extern Datum gistrescan(PG_FUNCTION_ARGS);
extern Datum gistmarkpos(PG_FUNCTION_ARGS);
extern Datum gistrestrpos(PG_FUNCTION_ARGS);
extern Datum gistendscan(PG_FUNCTION_ARGS);
extern void gistadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif	 /* GISTSCAN_H */
