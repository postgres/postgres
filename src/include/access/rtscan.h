/*-------------------------------------------------------------------------
 *
 * rtscan.h--
 *	  routines defined in access/rtree/rtscan.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtscan.h,v 1.4 1997/11/26 01:12:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSCAN_H
#define RTSCAN_H

#include <utils/rel.h>
#include <storage/block.h>
#include <storage/off.h>

void		rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif							/* RTSCAN_H */
