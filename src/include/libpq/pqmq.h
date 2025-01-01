/*-------------------------------------------------------------------------
 *
 * pqmq.h
 *	  Use the frontend/backend protocol for communication over a shm_mq
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqmq.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQMQ_H
#define PQMQ_H

#include "lib/stringinfo.h"
#include "storage/shm_mq.h"

extern void pq_redirect_to_shm_mq(dsm_segment *seg, shm_mq_handle *mqh);
extern void pq_set_parallel_leader(pid_t pid, ProcNumber procNumber);

extern void pq_parse_errornotice(StringInfo msg, ErrorData *edata);

#endif							/* PQMQ_H */
