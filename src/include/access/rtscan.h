/*-------------------------------------------------------------------------
 *
 * rtscan.h
 *	  routines defined in access/rtree/rtscan.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtscan.h,v 1.8 1999/07/16 17:07:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSCAN_H
#define RTSCAN_H

#include "storage/block.h"
#include "storage/off.h"
#include "utils/rel.h"

void		rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif	 /* RTSCAN_H */
