/*
 *	Definitions for pg_backup_db.c
 *
 *	IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_db.h
 */

#ifndef PG_BACKUP_DB_H
#define PG_BACKUP_DB_H

#include "pg_backup.h"


extern int	ExecuteSqlCommandBuf(Archive *AHX, const char *buf, size_t bufLen);

extern void ExecuteSqlStatement(Archive *AHX, const char *query);
extern PGresult *ExecuteSqlQuery(Archive *AHX, const char *query,
				ExecStatusType status);
extern PGresult *ExecuteSqlQueryForSingleRow(Archive *fout, char *query);

extern void EndDBCopyMode(Archive *AHX, const char *tocEntryTag);

extern void StartTransaction(Archive *AHX);
extern void CommitTransaction(Archive *AHX);

#endif
