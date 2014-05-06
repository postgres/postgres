/*-------------------------------------------------------------------------
 *
 * pqexpbuffer.h
 *	  Declarations/definitions for "PQExpBuffer" functions.
 *
 * PQExpBuffer provides an indefinitely-extensible string data type.
 * It can be used to buffer either ordinary C strings (null-terminated text)
 * or arbitrary binary data.  All storage is allocated with malloc().
 *
 * This module is essentially the same as the backend's StringInfo data type,
 * but it is intended for use in frontend libpq and client applications.
 * Thus, it does not rely on palloc() nor elog().
 *
 * It does rely on vsnprintf(); if configure finds that libc doesn't provide
 * a usable vsnprintf(), then a copy of our own implementation of it will
 * be linked into libpq.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/interfaces/libpq/pqexpbuffer.h,v 1.24 2010/01/02 16:58:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQEXPBUFFER_H
#define PQEXPBUFFER_H

/*-------------------------
 * PQExpBufferData holds information about an extensible string.
 *		data	is the current buffer for the string (allocated with malloc).
 *		len		is the current string length.  There is guaranteed to be
 *				a terminating '\0' at data[len], although this is not very
 *				useful when the string holds binary data rather than text.
 *		maxlen	is the allocated size in bytes of 'data', i.e. the maximum
 *				string size (including the terminating '\0' char) that we can
 *				currently store in 'data' without having to reallocate
 *				more space.  We must always have maxlen > len.
 *
 * An exception occurs if we failed to allocate enough memory for the string
 * buffer.  In that case data points to a statically allocated empty string,
 * and len = maxlen = 0.
 *-------------------------
 */
typedef struct PQExpBufferData
{
	char	   *data;
	size_t		len;
	size_t		maxlen;
} PQExpBufferData;

typedef PQExpBufferData *PQExpBuffer;

/*------------------------
 * Test for a broken (out of memory) PQExpBuffer.
 * When a buffer is "broken", all operations except resetting or deleting it
 * are no-ops.
 *------------------------
 */
#define PQExpBufferBroken(str)	\
	((str) == NULL || (str)->maxlen == 0)

/*------------------------
 * Initial size of the data buffer in a PQExpBuffer.
 * NB: this must be large enough to hold error messages that might
 * be returned by PQrequestCancel().
 *------------------------
 */
#define INITIAL_EXPBUFFER_SIZE	256

/*------------------------
 * There are two ways to create a PQExpBuffer object initially:
 *
 * PQExpBuffer stringptr = createPQExpBuffer();
 *		Both the PQExpBufferData and the data buffer are malloc'd.
 *
 * PQExpBufferData string;
 * initPQExpBuffer(&string);
 *		The data buffer is malloc'd but the PQExpBufferData is presupplied.
 *		This is appropriate if the PQExpBufferData is a field of another
 *		struct.
 *-------------------------
 */

/*------------------------
 * createPQExpBuffer
 * Create an empty 'PQExpBufferData' & return a pointer to it.
 */
extern PQExpBuffer createPQExpBuffer(void);

/*------------------------
 * initPQExpBuffer
 * Initialize a PQExpBufferData struct (with previously undefined contents)
 * to describe an empty string.
 */
extern void initPQExpBuffer(PQExpBuffer str);

/*------------------------
 * To destroy a PQExpBuffer, use either:
 *
 * destroyPQExpBuffer(str);
 *		free()s both the data buffer and the PQExpBufferData.
 *		This is the inverse of createPQExpBuffer().
 *
 * termPQExpBuffer(str)
 *		free()s the data buffer but not the PQExpBufferData itself.
 *		This is the inverse of initPQExpBuffer().
 *
 * NOTE: some routines build up a string using PQExpBuffer, and then
 * release the PQExpBufferData but return the data string itself to their
 * caller.  At that point the data string looks like a plain malloc'd
 * string.
 */
extern void destroyPQExpBuffer(PQExpBuffer str);
extern void termPQExpBuffer(PQExpBuffer str);

/*------------------------
 * resetPQExpBuffer
 *		Reset a PQExpBuffer to empty
 *
 * Note: if possible, a "broken" PQExpBuffer is returned to normal.
 */
extern void resetPQExpBuffer(PQExpBuffer str);

/*------------------------
 * enlargePQExpBuffer
 * Make sure there is enough space for 'needed' more bytes in the buffer
 * ('needed' does not include the terminating null).
 *
 * Returns 1 if OK, 0 if failed to enlarge buffer.  (In the latter case
 * the buffer is left in "broken" state.)
 */
extern int	enlargePQExpBuffer(PQExpBuffer str, size_t needed);

/*------------------------
 * printfPQExpBuffer
 * Format text data under the control of fmt (an sprintf-like format string)
 * and insert it into str.  More space is allocated to str if necessary.
 * This is a convenience routine that does the same thing as
 * resetPQExpBuffer() followed by appendPQExpBuffer().
 */
extern void
printfPQExpBuffer(PQExpBuffer str, const char *fmt,...)
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 2, 3)));

/*------------------------
 * appendPQExpBuffer
 * Format text data under the control of fmt (an sprintf-like format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 */
extern void
appendPQExpBuffer(PQExpBuffer str, const char *fmt,...)
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 2, 3)));

/*------------------------
 * appendPQExpBufferStr
 * Append the given string to a PQExpBuffer, allocating more space
 * if necessary.
 */
extern void appendPQExpBufferStr(PQExpBuffer str, const char *data);

/*------------------------
 * appendPQExpBufferChar
 * Append a single byte to str.
 * Like appendPQExpBuffer(str, "%c", ch) but much faster.
 */
extern void appendPQExpBufferChar(PQExpBuffer str, char ch);

/*------------------------
 * appendBinaryPQExpBuffer
 * Append arbitrary binary data to a PQExpBuffer, allocating more space
 * if necessary.
 */
extern void appendBinaryPQExpBuffer(PQExpBuffer str,
						const char *data, size_t datalen);

#endif   /* PQEXPBUFFER_H */
