/*-------------------------------------------------------------------------
 *
 * dbcommands_xlog.h
 *		Database resource manager XLOG definitions (create/drop database).
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/dbcommands_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_XLOG_H
#define DBCOMMANDS_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/* record types */
#define XLOG_DBASE_CREATE		0x00
#define XLOG_DBASE_DROP			0x10

typedef struct xl_dbase_create_rec
{
	/* Records copying of a single subdirectory incl. contents */
	Oid			db_id;
	Oid			tablespace_id;
	Oid			src_db_id;
	Oid			src_tablespace_id;
} xl_dbase_create_rec;

typedef struct xl_dbase_drop_rec
{
	/* Records dropping of a single subdirectory incl. contents */
	Oid			db_id;
	Oid			tablespace_id;
} xl_dbase_drop_rec;

extern void dbase_redo(XLogReaderState *rptr);
extern void dbase_desc(StringInfo buf, XLogReaderState *rptr);
extern const char *dbase_identify(uint8 info);

#endif   /* DBCOMMANDS_XLOG_H */
