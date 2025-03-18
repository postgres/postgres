/*-------------------------------------------------------------------------
 *
 * aio_subsys.h
 *    Interaction with AIO as a subsystem, rather than actually issuing AIO
 *
 * This header is for AIO related functionality that's being called by files
 * that don't perform AIO, but interact with the AIO subsystem in some
 * form. E.g. postmaster.c and shared memory initialization need to initialize
 * AIO but don't perform AIO.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_subsys.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_SUBSYS_H
#define AIO_SUBSYS_H


/* aio_init.c */
extern Size AioShmemSize(void);
extern void AioShmemInit(void);

extern void pgaio_init_backend(void);


/* aio.c */
extern void pgaio_error_cleanup(void);
extern void AtEOXact_Aio(bool is_commit);


/* aio_worker.c */
extern bool pgaio_workers_enabled(void);

#endif							/* AIO_SUBSYS_H */
