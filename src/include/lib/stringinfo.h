/*-------------------------------------------------------------------------
 *
 * stringinfo.h
 *	  Declarations/definitions for "StringInfo" functions.
 *
 * StringInfo provides an extensible string data type (currently limited to a
 * length of 1GB).  It can be used to buffer either ordinary C strings
 * (null-terminated text) or arbitrary binary data.  All storage is allocated
 * with palloc() (falling back to malloc in frontend code).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/lib/stringinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRINGINFO_H
#define STRINGINFO_H

/*-------------------------
 * StringInfoData holds information about an extensible string.
 *		data	is the current buffer for the string.
 *		len		is the current string length.  Except in the case of read-only
 *				strings described below, there is guaranteed to be a
 *				terminating '\0' at data[len].
 *		maxlen	is the allocated size in bytes of 'data', i.e. the maximum
 *				string size (including the terminating '\0' char) that we can
 *				currently store in 'data' without having to reallocate
 *				more space.  We must always have maxlen > len, except
 *				in the read-only case described below.
 *		cursor	is initialized to zero by makeStringInfo, initStringInfo,
 *				initReadOnlyStringInfo and initStringInfoFromString but is not
 *				otherwise touched by the stringinfo.c routines.  Some routines
 *				use it to scan through a StringInfo.
 *
 * As a special case, a StringInfoData can be initialized with a read-only
 * string buffer.  In this case "data" does not necessarily point at a
 * palloc'd chunk, and management of the buffer storage is the caller's
 * responsibility.  maxlen is set to zero to indicate that this is the case.
 * Read-only StringInfoDatas cannot be appended to or reset.
 * Also, it is caller's option whether a read-only string buffer has a
 * terminating '\0' or not.  This depends on the intended usage.
 *-------------------------
 */
typedef struct StringInfoData
{
	char	   *data;
	int			len;
	int			maxlen;
	int			cursor;
} StringInfoData;

typedef StringInfoData *StringInfo;


/*------------------------
 * There are six ways to create a StringInfo object initially:
 *
 * StringInfo stringptr = makeStringInfo();
 *		Both the StringInfoData and the data buffer are palloc'd.
 *
 * StringInfo stringptr = makeStringInfoExt(initsize);
 *		Same as makeStringInfo except the data buffer is allocated
 *		with size 'initsize'.
 *
 * StringInfoData string;
 * initStringInfo(&string);
 *		The data buffer is palloc'd but the StringInfoData is just local.
 *		This is the easiest approach for a StringInfo object that will
 *		only live as long as the current routine.
 *
 * StringInfoData string;
 * initStringInfoExt(&string, initsize);
 *		Same as initStringInfo except the data buffer is allocated
 *		with size 'initsize'.
 *
 * StringInfoData string;
 * initReadOnlyStringInfo(&string, existingbuf, len);
 *		The StringInfoData's data field is set to point directly to the
 *		existing buffer and the StringInfoData's len is set to the given len.
 *		The given buffer can point to memory that's not managed by palloc or
 *		is pointing partway through a palloc'd chunk.  The maxlen field is set
 *		to 0.  A read-only StringInfo cannot be appended to using any of the
 *		appendStringInfo functions or reset with resetStringInfo().  The given
 *		buffer can optionally omit the trailing NUL.
 *
 * StringInfoData string;
 * initStringInfoFromString(&string, palloced_buf, len);
 *		The StringInfoData's data field is set to point directly to the given
 *		buffer and the StringInfoData's len is set to the given len.  This
 *		method of initialization is useful when the buffer already exists.
 *		StringInfos initialized this way can be appended to using the
 *		appendStringInfo functions and reset with resetStringInfo().  The
 *		given buffer must be NUL-terminated.  The palloc'd buffer is assumed
 *		to be len + 1 in size.
 *
 * To destroy a StringInfo, pfree() the data buffer, and then pfree() the
 * StringInfoData if it was palloc'd.  For StringInfos created with
 * makeStringInfo(), destroyStringInfo() is provided for this purpose.
 * However, if the StringInfo was initialized using initReadOnlyStringInfo()
 * then the caller will need to consider if it is safe to pfree the data
 * buffer.
 *
 * NOTE: some routines build up a string using StringInfo, and then
 * release the StringInfoData but return the data string itself to their
 * caller.  At that point the data string looks like a plain palloc'd
 * string.
 *-------------------------
 */

#define STRINGINFO_DEFAULT_SIZE 1024	/* default initial allocation size */

/*------------------------
 * makeStringInfo
 * Create an empty 'StringInfoData' & return a pointer to it.
 */
extern StringInfo makeStringInfo(void);

/*------------------------
 * makeStringInfoExt
 * Create an empty 'StringInfoData' & return a pointer to it.
 * The data buffer is allocated with size 'initsize'.
 * The valid range for 'initsize' is 1 to MaxAllocSize.
 */
extern StringInfo makeStringInfoExt(int initsize);

/*------------------------
 * initStringInfo
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 */
extern void initStringInfo(StringInfo str);

/*------------------------
 * initStringInfoExt
 * Initialize a StringInfoData struct (with previously undefined contents) to
 * describe an empty string. The data buffer is allocated with size
 * 'initsize'. The valid range for 'initsize' is 1 to MaxAllocSize.
 */
extern void initStringInfoExt(StringInfo str, int initsize);

/*------------------------
 * initReadOnlyStringInfo
 * Initialize a StringInfoData struct from an existing string without copying
 * the string.  The caller is responsible for ensuring the given string
 * remains valid as long as the StringInfoData does.  Calls to this are used
 * in performance critical locations where allocating a new buffer and copying
 * would be too costly.  Read-only StringInfoData's may not be appended to
 * using any of the appendStringInfo functions or reset with
 * resetStringInfo().
 *
 * 'data' does not need to point directly to a palloc'd chunk of memory and may
 * omit the NUL termination character at data[len].
 */
static inline void
initReadOnlyStringInfo(StringInfo str, char *data, int len)
{
	str->data = data;
	str->len = len;
	str->maxlen = 0;			/* read-only */
	str->cursor = 0;
}

/*------------------------
 * initStringInfoFromString
 * Initialize a StringInfoData struct from an existing string without copying
 * the string.  'data' must be a valid palloc'd chunk of memory that can have
 * repalloc() called should more space be required during a call to any of the
 * appendStringInfo functions.
 *
 * 'data' must be NUL terminated at 'len' bytes.
 */
static inline void
initStringInfoFromString(StringInfo str, char *data, int len)
{
	Assert(data[len] == '\0');

	str->data = data;
	str->len = len;
	str->maxlen = len + 1;
	str->cursor = 0;
}

/*------------------------
 * resetStringInfo
 * Clears the current content of the StringInfo, if any. The
 * StringInfo remains valid.
 */
extern void resetStringInfo(StringInfo str);

/*------------------------
 * appendStringInfo
 * Format text data under the control of fmt (an sprintf-style format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 */
extern void appendStringInfo(StringInfo str, const char *fmt,...) pg_attribute_printf(2, 3);

/*------------------------
 * appendStringInfoVA
 * Attempt to format text data under the control of fmt (an sprintf-style
 * format string) and append it to whatever is already in str.  If successful
 * return zero; if not (because there's not enough space), return an estimate
 * of the space needed, without modifying str.  Typically the caller should
 * pass the return value to enlargeStringInfo() before trying again; see
 * appendStringInfo for standard usage pattern.
 */
extern int	appendStringInfoVA(StringInfo str, const char *fmt, va_list args) pg_attribute_printf(2, 0);

/*------------------------
 * appendStringInfoString
 * Append a null-terminated string to str.
 * Like appendStringInfo(str, "%s", s) but faster.
 */
extern void appendStringInfoString(StringInfo str, const char *s);

/*------------------------
 * appendStringInfoChar
 * Append a single byte to str.
 * Like appendStringInfo(str, "%c", ch) but much faster.
 */
extern void appendStringInfoChar(StringInfo str, char ch);

/*------------------------
 * appendStringInfoCharMacro
 * As above, but a macro for even more speed where it matters.
 * Caution: str argument will be evaluated multiple times.
 */
#define appendStringInfoCharMacro(str,ch) \
	(((str)->len + 1 >= (str)->maxlen) ? \
	 appendStringInfoChar(str, ch) : \
	 (void)((str)->data[(str)->len] = (ch), (str)->data[++(str)->len] = '\0'))

/*------------------------
 * appendStringInfoSpaces
 * Append a given number of spaces to str.
 */
extern void appendStringInfoSpaces(StringInfo str, int count);

/*------------------------
 * appendBinaryStringInfo
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary.
 */
extern void appendBinaryStringInfo(StringInfo str,
								   const void *data, int datalen);

/*------------------------
 * appendBinaryStringInfoNT
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary. Does not ensure a trailing null-byte exists.
 */
extern void appendBinaryStringInfoNT(StringInfo str,
									 const void *data, int datalen);

/*------------------------
 * enlargeStringInfo
 * Make sure a StringInfo's buffer can hold at least 'needed' more bytes.
 */
extern void enlargeStringInfo(StringInfo str, int needed);

/*------------------------
 * destroyStringInfo
 * Frees a StringInfo and its buffer (opposite of makeStringInfo()).
 */
extern void destroyStringInfo(StringInfo str);

#endif							/* STRINGINFO_H */
