/*
 *	Definitions for pg_backup_db.c
 *
 *	IDENTIFICATION
 *		$PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_db.h,v 1.9 2003/11/29 19:52:05 pgsql Exp $
 */

#define BLOB_XREF_TABLE "pg_dump_blob_xref"		/* MUST be lower case */

extern void FixupBlobRefs(ArchiveHandle *AH, TocEntry *te);
extern int	ExecuteSqlCommand(ArchiveHandle *AH, PQExpBuffer qry, char *desc, bool use_blob);
extern int	ExecuteSqlCommandBuf(ArchiveHandle *AH, void *qry, size_t bufLen);

extern void CreateBlobXrefTable(ArchiveHandle *AH);
extern void InsertBlobXref(ArchiveHandle *AH, Oid old, Oid new);
extern void StartTransaction(ArchiveHandle *AH);
extern void StartTransactionXref(ArchiveHandle *AH);
extern void CommitTransaction(ArchiveHandle *AH);
extern void CommitTransactionXref(ArchiveHandle *AH);
