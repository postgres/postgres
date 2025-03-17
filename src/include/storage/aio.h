/*-------------------------------------------------------------------------
 *
 * aio.h
 *    Main AIO interface
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_H
#define AIO_H



/* Enum for io_method GUC. */
typedef enum IoMethod
{
	IOMETHOD_SYNC = 0,
} IoMethod;

/* We'll default to synchronous execution. */
#define DEFAULT_IO_METHOD IOMETHOD_SYNC


struct dlist_node;
extern void pgaio_io_release_resowner(struct dlist_node *ioh_node, bool on_error);


/* GUCs */
extern PGDLLIMPORT int io_method;
extern PGDLLIMPORT int io_max_concurrency;


#endif							/* AIO_H */
