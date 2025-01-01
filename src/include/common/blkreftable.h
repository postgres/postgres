/*-------------------------------------------------------------------------
 *
 * blkreftable.h
 *	  Block reference tables.
 *
 * A block reference table is used to keep track of which blocks have
 * been modified by WAL records within a certain LSN range.
 *
 * For each relation fork, there is a "limit block number". All existing
 * blocks greater than or equal to the limit block number must be
 * considered modified; for those less than the limit block number,
 * we maintain a bitmap. When a relation fork is created or dropped,
 * the limit block number should be set to 0. When it's truncated,
 * the limit block number should be set to the length in blocks to
 * which it was truncated.
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * src/include/common/blkreftable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BLKREFTABLE_H
#define BLKREFTABLE_H

#include "storage/block.h"
#include "storage/relfilelocator.h"

/* Magic number for serialization file format. */
#define BLOCKREFTABLE_MAGIC			0x652b137b

typedef struct BlockRefTable BlockRefTable;
typedef struct BlockRefTableEntry BlockRefTableEntry;
typedef struct BlockRefTableReader BlockRefTableReader;
typedef struct BlockRefTableWriter BlockRefTableWriter;

/*
 * The return value of io_callback_fn should be the number of bytes read
 * or written. If an error occurs, the functions should report it and
 * not return. When used as a write callback, short writes should be retried
 * or treated as errors, so that if the callback returns, the return value
 * is always the request length.
 *
 * report_error_fn should not return.
 */
typedef int (*io_callback_fn) (void *callback_arg, void *data, int length);
typedef void (*report_error_fn) (void *callback_arg, char *msg,...) pg_attribute_printf(2, 3);


/*
 * Functions for manipulating an entire in-memory block reference table.
 */
extern BlockRefTable *CreateEmptyBlockRefTable(void);
extern void BlockRefTableSetLimitBlock(BlockRefTable *brtab,
									   const RelFileLocator *rlocator,
									   ForkNumber forknum,
									   BlockNumber limit_block);
extern void BlockRefTableMarkBlockModified(BlockRefTable *brtab,
										   const RelFileLocator *rlocator,
										   ForkNumber forknum,
										   BlockNumber blknum);
extern void WriteBlockRefTable(BlockRefTable *brtab,
							   io_callback_fn write_callback,
							   void *write_callback_arg);

extern BlockRefTableEntry *BlockRefTableGetEntry(BlockRefTable *brtab,
												 const RelFileLocator *rlocator,
												 ForkNumber forknum,
												 BlockNumber *limit_block);
extern int	BlockRefTableEntryGetBlocks(BlockRefTableEntry *entry,
										BlockNumber start_blkno,
										BlockNumber stop_blkno,
										BlockNumber *blocks,
										int nblocks);

/*
 * Functions for reading a block reference table incrementally from disk.
 */
extern BlockRefTableReader *CreateBlockRefTableReader(io_callback_fn read_callback,
													  void *read_callback_arg,
													  char *error_filename,
													  report_error_fn error_callback,
													  void *error_callback_arg);
extern bool BlockRefTableReaderNextRelation(BlockRefTableReader *reader,
											RelFileLocator *rlocator,
											ForkNumber *forknum,
											BlockNumber *limit_block);
extern unsigned BlockRefTableReaderGetBlocks(BlockRefTableReader *reader,
											 BlockNumber *blocks,
											 int nblocks);
extern void DestroyBlockRefTableReader(BlockRefTableReader *reader);

/*
 * Functions for writing a block reference table incrementally to disk.
 *
 * Note that entries must be written in the proper order, that is, sorted by
 * database, then tablespace, then relfilenumber, then fork number. Caller
 * is responsible for supplying data in the correct order. If that seems hard,
 * use an in-memory BlockRefTable instead.
 */
extern BlockRefTableWriter *CreateBlockRefTableWriter(io_callback_fn write_callback,
													  void *write_callback_arg);
extern void BlockRefTableWriteEntry(BlockRefTableWriter *writer,
									BlockRefTableEntry *entry);
extern void DestroyBlockRefTableWriter(BlockRefTableWriter *writer);

extern BlockRefTableEntry *CreateBlockRefTableEntry(RelFileLocator rlocator,
													ForkNumber forknum);
extern void BlockRefTableEntrySetLimitBlock(BlockRefTableEntry *entry,
											BlockNumber limit_block);
extern void BlockRefTableEntryMarkBlockModified(BlockRefTableEntry *entry,
												ForkNumber forknum,
												BlockNumber blknum);
extern void BlockRefTableFreeEntry(BlockRefTableEntry *entry);

#endif							/* BLKREFTABLE_H */
