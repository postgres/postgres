/*
 *	Definitions for pg_backup_db.c
 *
 *	IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_db.h
 */

#ifndef PG_BACKUP_DB_H
#define PG_BACKUP_DB_H

#include "pg_backup_archiver.h"

extern int	ExecuteSqlCommandBuf(ArchiveHandle *AH, const char *buf, size_t bufLen);

extern void ExecuteSqlStatement(Archive *AHX, const char *query);
extern PGresult *ExecuteSqlQuery(Archive *AHX, const char *query,
				ExecStatusType status);

extern void EndDBCopyMode(ArchiveHandle *AH, struct _tocEntry * te);

extern void StartTransaction(ArchiveHandle *AH);
extern void CommitTransaction(ArchiveHandle *AH);

#endif
