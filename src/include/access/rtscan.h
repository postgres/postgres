/*-------------------------------------------------------------------------
 *
 * rtscan.h--
 *	  routines defined in access/rtree/rtscan.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtscan.h,v 1.3 1997/09/08 02:34:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTSCAN_H

void		rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum);

#endif							/* RTSCAN_H */
