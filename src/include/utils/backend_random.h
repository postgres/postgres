/*-------------------------------------------------------------------------
 *
 * backend_random.h
 *		Declarations for backend random number generation
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 *
 *	  src/include/utils/backend_random.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKEND_RANDOM_H
#define BACKEND_RANDOM_H

extern Size BackendRandomShmemSize(void);
extern void BackendRandomShmemInit(void);
extern bool pg_backend_random(char *dst, int len);

#endif							/* BACKEND_RANDOM_H */
