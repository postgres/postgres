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

#include <access/relscan.h>

extern IndexScanDesc gistbeginscan(Relation r, bool fromEnd,
			  uint16 nkeys, ScanKey key);
extern void gistrescan(IndexScanDesc s, bool fromEnd, ScanKey key);
extern void gistmarkpos(IndexScanDesc s);
extern void gistrestrpos(IndexScanDesc s);
extern void gistendscan(IndexScanDesc s);
extern void gistadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif	 /* GISTSCAN_H */
