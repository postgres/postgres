/*-------------------------------------------------------------------------
 *
 * gistscan.h--
 *    routines defined in access/gisr/gistscan.c
 *
 *
 *
 * rtscan.h,v 1.2 1995/06/14 00:06:58 jolly Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef GISTSCAN_H

#include <storage/off.h>
#include <storage/block.h>
#include <utils/rel.h>

void gistadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif /* GISTSCAN_H */
