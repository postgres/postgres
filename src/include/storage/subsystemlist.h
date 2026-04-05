/*---------------------------------------------------------------------------
 * subsystemlist.h
 *
 * List of initialization callbacks of built-in subsystems. This is kept in
 * its own source file for possible use by automatic tools.
 * PG_SHMEM_SUBSYSTEM is defined in the callers depending on how the list is
 * used.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/subsystemlist.h
 *---------------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef SUBSYSTEMLIST_H here */

/*
 * Note: there are some inter-dependencies between these, so the order of some
 * of these matter.
 */

/* TODO: empty for now */
