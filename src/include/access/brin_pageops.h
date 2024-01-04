/*
 * brin_pageops.h
 *		Prototypes for operating on BRIN pages.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/access/brin_pageops.h
 */
#ifndef BRIN_PAGEOPS_H
#define BRIN_PAGEOPS_H

#include "access/brin_revmap.h"

extern bool brin_doupdate(Relation idxrel, BlockNumber pagesPerRange,
						  BrinRevmap *revmap, BlockNumber heapBlk,
						  Buffer oldbuf, OffsetNumber oldoff,
						  const BrinTuple *origtup, Size origsz,
						  const BrinTuple *newtup, Size newsz,
						  bool samepage);
extern bool brin_can_do_samepage_update(Buffer buffer, Size origsz,
										Size newsz);
extern OffsetNumber brin_doinsert(Relation idxrel, BlockNumber pagesPerRange,
								  BrinRevmap *revmap, Buffer *buffer, BlockNumber heapBlk,
								  BrinTuple *tup, Size itemsz);

extern void brin_page_init(Page page, uint16 type);
extern void brin_metapage_init(Page page, BlockNumber pagesPerRange,
							   uint16 version);

extern bool brin_start_evacuating_page(Relation idxRel, Buffer buf);
extern void brin_evacuate_page(Relation idxRel, BlockNumber pagesPerRange,
							   BrinRevmap *revmap, Buffer buf);

extern void brin_page_cleanup(Relation idxrel, Buffer buf);

#endif							/* BRIN_PAGEOPS_H */
