/*-------------------------------------------------------------------------
 *
 * rtscan.h--
 *	  routines defined in access/rtree/rtscan.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtscan.h,v 1.5 1998/09/01 04:34:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSCAN_H
#define RTSCAN_H

#include <utils/rel.h>
#include <storage/block.h>
#include <storage/off.h>

void		rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif	 /* RTSCAN_H */
