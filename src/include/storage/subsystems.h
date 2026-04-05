/*-------------------------------------------------------------------------
 *
 * subsystems.h
 *	  Provide extern declarations for all the built-in subsystem callbacks
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/subsystems.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBSYSTEMS_H
#define SUBSYSTEMS_H

#include "storage/shmem.h"

/*
 * Extern declarations of all the built-in subsystem callbacks
 *
 * The actual list is in subsystemlist.h, so that the same list can be used
 * for other purposes.
 */
#define PG_SHMEM_SUBSYSTEM(callbacks) \
	extern const ShmemCallbacks callbacks;
#include "storage/subsystemlist.h"
#undef PG_SHMEM_SUBSYSTEM

#endif							/* SUBSYSTEMS_H */
