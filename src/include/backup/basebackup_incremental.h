/*-------------------------------------------------------------------------
 *
 * basebackup_incremental.h
 *	  API for incremental backup support
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup_incremental.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BASEBACKUP_INCREMENTAL_H
#define BASEBACKUP_INCREMENTAL_H

#include "access/xlogbackup.h"
#include "common/relpath.h"
#include "storage/block.h"
#include "utils/palloc.h"

#define INCREMENTAL_MAGIC			0xd3ae1f0d

typedef enum
{
	BACK_UP_FILE_FULLY,
	BACK_UP_FILE_INCREMENTALLY
} FileBackupMethod;

struct IncrementalBackupInfo;
typedef struct IncrementalBackupInfo IncrementalBackupInfo;

extern IncrementalBackupInfo *CreateIncrementalBackupInfo(MemoryContext);

extern void AppendIncrementalManifestData(IncrementalBackupInfo *ib,
										  const char *data,
										  int len);
extern void FinalizeIncrementalManifest(IncrementalBackupInfo *ib);

extern void PrepareForIncrementalBackup(IncrementalBackupInfo *ib,
										BackupState *backup_state);

extern char *GetIncrementalFilePath(Oid dboid, Oid spcoid,
									RelFileNumber relfilenumber,
									ForkNumber forknum, unsigned segno);
extern FileBackupMethod GetFileBackupMethod(IncrementalBackupInfo *ib,
											const char *path,
											Oid dboid, Oid spcoid,
											RelFileNumber relfilenumber,
											ForkNumber forknum,
											unsigned segno, size_t size,
											unsigned *num_blocks_required,
											BlockNumber *relative_block_numbers,
											unsigned *truncation_block_length);
extern size_t GetIncrementalFileSize(unsigned num_blocks_required);
extern size_t GetIncrementalHeaderSize(unsigned num_blocks_required);

#endif
