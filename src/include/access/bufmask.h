/*-------------------------------------------------------------------------
 *
 * bufmask.h
 *	  Definitions for buffer masking routines, used to mask certain bits
 *	  in a page which can be different when the WAL is generated
 *	  and when the WAL is applied. This is really the job of each
 *	  individual rmgr, but we make things easier by providing some
 *	  common routines to handle cases which occur in multiple rmgrs.
 *
 * Portions Copyright (c) 2016-2022, PostgreSQL Global Development Group
 *
 * src/include/access/bufmask.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BUFMASK_H
#define BUFMASK_H

#include "storage/block.h"
#include "storage/bufmgr.h"

/* Marker used to mask pages consistently */
#define MASK_MARKER		0

extern void mask_page_lsn_and_checksum(Page page);
extern void mask_page_hint_bits(Page page);
extern void mask_unused_space(Page page);
extern void mask_lp_flags(Page page);
extern void mask_page_content(Page page);

#endif
