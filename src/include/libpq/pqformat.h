/*-------------------------------------------------------------------------
 *
 * pqformat.h
 *		Definitions for formatting and parsing frontend/backend messages
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pqformat.h,v 1.13 2002/09/04 23:31:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQFORMAT_H
#define PQFORMAT_H

#include "lib/stringinfo.h"

#define pq_beginmessage(buf)  initStringInfo(buf)

extern void pq_sendbyte(StringInfo buf, int byt);
extern void pq_sendbytes(StringInfo buf, const char *data, int datalen);
extern void pq_sendcountedtext(StringInfo buf, const char *str, int slen);
extern void pq_sendstring(StringInfo buf, const char *str);
extern void pq_sendint(StringInfo buf, int i, int b);
extern void pq_endmessage(StringInfo buf);

extern int	pq_puttextmessage(char msgtype, const char *str);

extern int	pq_getint(int *result, int b);
extern int	pq_getstr_bounded(StringInfo s, int maxlen);

#define pq_getstr(s)	pq_getstr_bounded(s, 0)

#endif   /* PQFORMAT_H */
