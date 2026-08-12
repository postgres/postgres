/*-------------------------------------------------------------------------
 *
 * pg_parse_lsn.h
 *	  Parse a WAL location (LSN) in its text form.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/pg_parse_lsn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PARSE_LSN_H
#define PG_PARSE_LSN_H

#include "access/xlogdefs.h"

extern bool pg_parse_lsn(const char *str, XLogRecPtr *result);

#endif							/* PG_PARSE_LSN_H */
