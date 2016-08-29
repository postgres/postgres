/*-------------------------------------------------------------------------
 *
 * hash_xlog.h
 *	  header file for Postgres hash AM implementation
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hash_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_XLOG_H
#define HASH_XLOG_H

#include "access/hash.h"
#include "access/xlogreader.h"


extern void hash_redo(XLogReaderState *record);
extern void hash_desc(StringInfo buf, XLogReaderState *record);
extern const char *hash_identify(uint8 info);

#endif   /* HASH_XLOG_H */
