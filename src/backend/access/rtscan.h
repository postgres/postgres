/*-------------------------------------------------------------------------
 *
 * rtscan.h--
 *    routines defined in access/rtree/rtscan.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtscan.h,v 1.1.1.1 1996/07/09 06:21:09 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSCAN_H

void rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif /* RTSCAN_H */
