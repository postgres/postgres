/*-------------------------------------------------------------------------
 *
 * buf.h
 *	  Basic buffer manager data types.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: buf.h,v 1.10 2001/10/25 05:50:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUF_H
#define BUF_H

/*
 * Buffer identifiers.
 *
 * Zero is invalid, positive is the index of a shared buffer (1..NBuffers),
 * negative is the index of a local buffer (-1 .. -NLocBuffer).
 */
typedef int Buffer;

#define InvalidBuffer	0

/*
 * BufferIsInvalid
 *		True iff the buffer is invalid.
 */
#define BufferIsInvalid(buffer) ((buffer) == InvalidBuffer)

/*
 * BufferIsLocal
 *		True iff the buffer is local (not visible to other servers).
 */
#define BufferIsLocal(buffer)	((buffer) < 0)

/*
 * If NO_BUFFERISVALID is defined, all error checking using BufferIsValid()
 * are suppressed.	Decision-making using BufferIsValid is not affected.
 * This should be set only if one is sure there will be no errors.
 * - plai 9/10/90
 */
#undef NO_BUFFERISVALID
#endif	 /* BUF_H */
