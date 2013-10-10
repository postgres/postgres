/*-------------------------------------------------------------------------
 *
 * dsm.h
 *	  manage dynamic shared memory segments
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSM_H
#define DSM_H

#include "storage/dsm_impl.h"

typedef struct dsm_segment dsm_segment;

/* Initialization function. */
extern void dsm_postmaster_startup(void);

/* Functions that create, update, or remove mappings. */
extern dsm_segment *dsm_create(uint64 size);
extern dsm_segment *dsm_attach(dsm_handle h);
extern void *dsm_resize(dsm_segment *seg, uint64 size);
extern void *dsm_remap(dsm_segment *seg);
extern void dsm_detach(dsm_segment *seg);

/* Resource management functions. */
extern void dsm_keep_mapping(dsm_segment *seg);
extern dsm_segment *dsm_find_mapping(dsm_handle h);

/* Informational functions. */
extern void *dsm_segment_address(dsm_segment *seg);
extern uint64 dsm_segment_map_length(dsm_segment *seg);
extern dsm_handle dsm_segment_handle(dsm_segment *seg);

#endif   /* DSM_H */
