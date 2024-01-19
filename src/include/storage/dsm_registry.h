/*-------------------------------------------------------------------------
 *
 * dsm_registry.h
 *	  Functions for interfacing with the dynamic shared memory registry.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm_registry.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSM_REGISTRY_H
#define DSM_REGISTRY_H

extern void *GetNamedDSMSegment(const char *name, size_t size,
								void (*init_callback) (void *ptr),
								bool *found);

extern Size DSMRegistryShmemSize(void);
extern void DSMRegistryShmemInit(void);

#endif							/* DSM_REGISTRY_H */
