/*-------------------------------------------------------------------------
 *
 * rtscan.h--
 *	  routines defined in access/rtree/rtscan.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtscan.h,v 1.2 1997/09/07 04:56:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSCAN_H

void			rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif							/* RTSCAN_H */
