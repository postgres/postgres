/*--------------------------------------------------------------------------
 *
 * test_shm_mq.h
 *		Definitions for shared memory message queues
 *
 * Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_shm_mq/test_shm_mq.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef TEST_SHM_MQ_H
#define TEST_SHM_MQ_H

#include "storage/dsm.h"
#include "storage/shm_mq.h"
#include "storage/spin.h"

/* Identifier for shared memory segments used by this extension. */
#define		PG_TEST_SHM_MQ_MAGIC		0x79fb2447

/*
 * This structure is stored in the dynamic shared memory segment.  We use
 * it to determine whether all workers started up OK and successfully
 * attached to their respective shared message queues.
 */
typedef struct
{
	slock_t		mutex;
	int			workers_total;
	int			workers_attached;
	int			workers_ready;
} test_shm_mq_header;

/* Set up dynamic shared memory and background workers for test run. */
extern void test_shm_mq_setup(int64 queue_size, int32 nworkers,
							  dsm_segment **seg, shm_mq_handle **output,
							  shm_mq_handle **input);

/* Main entrypoint for a worker. */
extern void test_shm_mq_main(Datum) pg_attribute_noreturn();

#endif
