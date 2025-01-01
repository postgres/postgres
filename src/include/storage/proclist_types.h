/*-------------------------------------------------------------------------
 *
 * proclist_types.h
 *		doubly-linked lists of pgprocnos
 *
 * See proclist.h for functions that operate on these types.
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist_types.h
 *-------------------------------------------------------------------------
 */

#ifndef PROCLIST_TYPES_H
#define PROCLIST_TYPES_H

#include "storage/procnumber.h"

/*
 * A node in a doubly-linked list of processes.  The link fields contain
 * the 0-based PGPROC indexes of the next and previous process, or
 * INVALID_PROC_NUMBER in the next-link of the last node and the prev-link
 * of the first node.  A node that is currently not in any list
 * should have next == prev == 0; this is not a possible state for a node
 * that is in a list, because we disallow circularity.
 */
typedef struct proclist_node
{
	ProcNumber	next;			/* pgprocno of the next PGPROC */
	ProcNumber	prev;			/* pgprocno of the prev PGPROC */
} proclist_node;

/*
 * Header of a doubly-linked list of PGPROCs, identified by pgprocno.
 * An empty list is represented by head == tail == INVALID_PROC_NUMBER.
 */
typedef struct proclist_head
{
	ProcNumber	head;			/* pgprocno of the head PGPROC */
	ProcNumber	tail;			/* pgprocno of the tail PGPROC */
} proclist_head;

/*
 * List iterator allowing some modifications while iterating.
 */
typedef struct proclist_mutable_iter
{
	ProcNumber	cur;			/* pgprocno of the current PGPROC */
	ProcNumber	next;			/* pgprocno of the next PGPROC */
} proclist_mutable_iter;

#endif							/* PROCLIST_TYPES_H */
