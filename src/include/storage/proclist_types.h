/*-------------------------------------------------------------------------
 *
 * proclist_types.h
 *		doubly-linked lists of pgprocnos
 *
 * See proclist.h for functions that operate on these types.
 *
 * Portions Copyright (c) 2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist_types.h
 *-------------------------------------------------------------------------
 */

#ifndef PROCLIST_TYPES_H
#define PROCLIST_TYPES_H

/*
 * A node in a list of processes.
 */
typedef struct proclist_node
{
	int			next;			/* pgprocno of the next PGPROC */
	int			prev;			/* pgprocno of the prev PGPROC */
} proclist_node;

/*
 * Head of a doubly-linked list of PGPROCs, identified by pgprocno.
 */
typedef struct proclist_head
{
	int			head;			/* pgprocno of the head PGPROC */
	int			tail;			/* pgprocno of the tail PGPROC */
} proclist_head;

/*
 * List iterator allowing some modifications while iterating.
 */
typedef struct proclist_mutable_iter
{
	int			cur;			/* pgprocno of the current PGPROC */
	int			next;			/* pgprocno of the next PGPROC */
} proclist_mutable_iter;

#endif
