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

#include "utils/rel.h"
#include "storage/block.h"
#include "storage/off.h"

void gistadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif /* GISTSCAN_H */
