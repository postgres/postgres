/*-------------------------------------------------------------------------
 * lockwaitpolicy.h
 *	  Header file for LockWaitPolicy enum.
 *
 * Copyright (c) 2014, PostgreSQL Global Development Group
 *
 * src/include/utils/lockwaitpolicy.h
 *-------------------------------------------------------------------------
 */
#ifndef LOCKWAITPOLICY_H
#define LOCKWAITPOLICY_H

/*
 * This enum controls how to deal with rows being locked by FOR UPDATE/SHARE
 * clauses (i.e., NOWAIT and SKIP LOCKED clauses).  The ordering here is
 * important, because the highest numerical value takes precedence when a
 * RTE is specified multiple ways.  See applyLockingClause.
 */
typedef enum
{
	/* Wait for the lock to become available (default behavior) */
	LockWaitBlock,

	/* Skip rows that can't be locked (SKIP LOCKED) */
	LockWaitSkip,

	/* Raise an error if a row cannot be locked (NOWAIT) */
	LockWaitError
} LockWaitPolicy;

#endif   /* LOCKWAITPOLICY_H */
