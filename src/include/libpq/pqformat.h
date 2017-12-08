/*-------------------------------------------------------------------------
 *
 * pqformat.h
 *		Definitions for formatting and parsing frontend/backend messages
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqformat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQFORMAT_H
#define PQFORMAT_H

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "port/pg_bswap.h"

extern void pq_beginmessage(StringInfo buf, char msgtype);
extern void pq_beginmessage_reuse(StringInfo buf, char msgtype);
extern void pq_endmessage(StringInfo buf);
extern void pq_endmessage_reuse(StringInfo buf);

extern void pq_sendbytes(StringInfo buf, const char *data, int datalen);
extern void pq_sendcountedtext(StringInfo buf, const char *str, int slen,
				   bool countincludesself);
extern void pq_sendtext(StringInfo buf, const char *str, int slen);
extern void pq_sendstring(StringInfo buf, const char *str);
extern void pq_send_ascii_string(StringInfo buf, const char *str);
extern void pq_sendfloat4(StringInfo buf, float4 f);
extern void pq_sendfloat8(StringInfo buf, float8 f);

extern void pq_sendfloat4(StringInfo buf, float4 f);
extern void pq_sendfloat8(StringInfo buf, float8 f);

/*
 * Append an int8 to a StringInfo buffer, which already has enough space
 * preallocated.
 *
 * The use of pg_restrict allows the compiler to optimize the code based on
 * the assumption that buf, buf->len, buf->data and *buf->data don't
 * overlap. Without the annotation buf->len etc cannot be kept in a register
 * over subsequent pq_writeintN calls.
 *
 * The use of StringInfoData * rather than StringInfo is due to MSVC being
 * overly picky and demanding a * before a restrict.
 */
static inline void
pq_writeint8(StringInfoData *pg_restrict buf, int8 i)
{
	int8		ni = i;

	Assert(buf->len + (int) sizeof(int8) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(int8));
	buf->len += sizeof(int8);
}

/*
 * Append an int16 to a StringInfo buffer, which already has enough space
 * preallocated.
 */
static inline void
pq_writeint16(StringInfoData *pg_restrict buf, int16 i)
{
	int16		ni = pg_hton16(i);

	Assert(buf->len + (int) sizeof(int16) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(int16));
	buf->len += sizeof(int16);
}

/*
 * Append an int32 to a StringInfo buffer, which already has enough space
 * preallocated.
 */
static inline void
pq_writeint32(StringInfoData *pg_restrict buf, int32 i)
{
	int32		ni = pg_hton32(i);

	Assert(buf->len + (int) sizeof(int32) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(int32));
	buf->len += sizeof(int32);
}

/*
 * Append an int64 to a StringInfo buffer, which already has enough space
 * preallocated.
 */
static inline void
pq_writeint64(StringInfoData *pg_restrict buf, int64 i)
{
	int64		ni = pg_hton64(i);

	Assert(buf->len + (int) sizeof(int64) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(int64));
	buf->len += sizeof(int64);
}

/*
 * Append a null-terminated text string (with conversion) to a buffer with
 * preallocated space.
 *
 * NB: The pre-allocated space needs to be sufficient for the string after
 * converting to client encoding.
 *
 * NB: passed text string must be null-terminated, and so is the data
 * sent to the frontend.
 */
static inline void
pq_writestring(StringInfoData *pg_restrict buf, const char *pg_restrict str)
{
	int			slen = strlen(str);
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* actual conversion has been done? */
		slen = strlen(p);

	Assert(buf->len + slen + 1 <= buf->maxlen);

	memcpy(((char *pg_restrict) buf->data + buf->len), p, slen + 1);
	buf->len += slen + 1;

	if (p != str)
		pfree(p);
}

/* append a binary int8 to a StringInfo buffer */
static inline void
pq_sendint8(StringInfo buf, int8 i)
{
	enlargeStringInfo(buf, sizeof(int8));
	pq_writeint8(buf, i);
}

/* append a binary int16 to a StringInfo buffer */
static inline void
pq_sendint16(StringInfo buf, int16 i)
{
	enlargeStringInfo(buf, sizeof(int16));
	pq_writeint16(buf, i);
}

/* append a binary int32 to a StringInfo buffer */
static inline void
pq_sendint32(StringInfo buf, int32 i)
{
	enlargeStringInfo(buf, sizeof(int32));
	pq_writeint32(buf, i);
}

/* append a binary int64 to a StringInfo buffer */
static inline void
pq_sendint64(StringInfo buf, int64 i)
{
	enlargeStringInfo(buf, sizeof(int64));
	pq_writeint64(buf, i);
}

/* append a binary byte to a StringInfo buffer */
static inline void
pq_sendbyte(StringInfo buf, int8 byt)
{
	pq_sendint8(buf, byt);
}

/*
 * Append a binary integer to a StringInfo buffer
 *
 * This function is deprecated; prefer use of the functions above.
 */
static inline void
pq_sendint(StringInfo buf, int i, int b)
{
	switch (b)
	{
		case 1:
			pq_sendint8(buf, (int8) i);
			break;
		case 2:
			pq_sendint16(buf, (int16) i);
			break;
		case 4:
			pq_sendint32(buf, (int32) i);
			break;
		default:
			elog(ERROR, "unsupported integer size %d", b);
			break;
	}
}


extern void pq_begintypsend(StringInfo buf);
extern bytea *pq_endtypsend(StringInfo buf);

extern void pq_puttextmessage(char msgtype, const char *str);
extern void pq_putemptymessage(char msgtype);

extern int	pq_getmsgbyte(StringInfo msg);
extern unsigned int pq_getmsgint(StringInfo msg, int b);
extern int64 pq_getmsgint64(StringInfo msg);
extern float4 pq_getmsgfloat4(StringInfo msg);
extern float8 pq_getmsgfloat8(StringInfo msg);
extern const char *pq_getmsgbytes(StringInfo msg, int datalen);
extern void pq_copymsgbytes(StringInfo msg, char *buf, int datalen);
extern char *pq_getmsgtext(StringInfo msg, int rawbytes, int *nbytes);
extern const char *pq_getmsgstring(StringInfo msg);
extern const char *pq_getmsgrawstring(StringInfo msg);
extern void pq_getmsgend(StringInfo msg);

#endif							/* PQFORMAT_H */
