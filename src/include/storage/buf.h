/*-------------------------------------------------------------------------
 *
 * buf.h
 *	  Basic buffer manager data types.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: buf.h,v 1.7 2000/01/26 05:58:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUF_H
#define BUF_H

#define InvalidBuffer	(0)
#define UnknownBuffer	(-99999)

typedef long Buffer;

/*
 * BufferIsInvalid
 *		True iff the buffer is invalid.
 */
#define BufferIsInvalid(buffer) ((buffer) == InvalidBuffer)

/*
 * BufferIsUnknown
 *		True iff the buffer is unknown.
 */
#define BufferIsUnknown(buffer) ((buffer) == UnknownBuffer)

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
