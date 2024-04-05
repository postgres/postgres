/*-------------------------------------------------------------------------
 *
 * blkreftable.c
 *	  Block reference tables.
 *
 * A block reference table is used to keep track of which blocks have
 * been modified by WAL records within a certain LSN range.
 *
 * For each relation fork, we keep track of all blocks that have appeared
 * in block reference in the WAL. We also keep track of the "limit block",
 * which is the smallest relation length in blocks known to have occurred
 * during that range of WAL records.  This should be set to 0 if the relation
 * fork is created or destroyed, and to the post-truncation length if
 * truncated.
 *
 * Whenever we set the limit block, we also forget about any modified blocks
 * beyond that point. Those blocks don't exist any more. Such blocks can
 * later be marked as modified again; if that happens, it means the relation
 * was re-extended.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/common/blkreftable.c
 *
 *-------------------------------------------------------------------------
 */


#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef FRONTEND
#include "common/logging.h"
#endif

#include "common/blkreftable.h"
#include "common/hashfn.h"
#include "port/pg_crc32c.h"

/*
 * A block reference table keeps track of the status of each relation
 * fork individually.
 */
typedef struct BlockRefTableKey
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
} BlockRefTableKey;

/*
 * We could need to store data either for a relation in which only a
 * tiny fraction of the blocks have been modified or for a relation in
 * which nearly every block has been modified, and we want a
 * space-efficient representation in both cases. To accomplish this,
 * we divide the relation into chunks of 2^16 blocks and choose between
 * an array representation and a bitmap representation for each chunk.
 *
 * When the number of modified blocks in a given chunk is small, we
 * essentially store an array of block numbers, but we need not store the
 * entire block number: instead, we store each block number as a 2-byte
 * offset from the start of the chunk.
 *
 * When the number of modified blocks in a given chunk is large, we switch
 * to a bitmap representation.
 *
 * These same basic representational choices are used both when a block
 * reference table is stored in memory and when it is serialized to disk.
 *
 * In the in-memory representation, we initially allocate each chunk with
 * space for a number of entries given by INITIAL_ENTRIES_PER_CHUNK and
 * increase that as necessary until we reach MAX_ENTRIES_PER_CHUNK.
 * Any chunk whose allocated size reaches MAX_ENTRIES_PER_CHUNK is converted
 * to a bitmap, and thus never needs to grow further.
 */
#define BLOCKS_PER_CHUNK		(1 << 16)
#define BLOCKS_PER_ENTRY		(BITS_PER_BYTE * sizeof(uint16))
#define MAX_ENTRIES_PER_CHUNK	(BLOCKS_PER_CHUNK / BLOCKS_PER_ENTRY)
#define INITIAL_ENTRIES_PER_CHUNK	16
typedef uint16 *BlockRefTableChunk;

/*
 * State for one relation fork.
 *
 * 'rlocator' and 'forknum' identify the relation fork to which this entry
 * pertains.
 *
 * 'limit_block' is the shortest known length of the relation in blocks
 * within the LSN range covered by a particular block reference table.
 * It should be set to 0 if the relation fork is created or dropped. If the
 * relation fork is truncated, it should be set to the number of blocks that
 * remain after truncation.
 *
 * 'nchunks' is the allocated length of each of the three arrays that follow.
 * We can only represent the status of block numbers less than nchunks *
 * BLOCKS_PER_CHUNK.
 *
 * 'chunk_size' is an array storing the allocated size of each chunk.
 *
 * 'chunk_usage' is an array storing the number of elements used in each
 * chunk. If that value is less than MAX_ENTRIES_PER_CHUNK, the corresponding
 * chunk is used as an array; else the corresponding chunk is used as a bitmap.
 * When used as a bitmap, the least significant bit of the first array element
 * is the status of the lowest-numbered block covered by this chunk.
 *
 * 'chunk_data' is the array of chunks.
 */
struct BlockRefTableEntry
{
	BlockRefTableKey key;
	BlockNumber limit_block;
	char		status;
	uint32		nchunks;
	uint16	   *chunk_size;
	uint16	   *chunk_usage;
	BlockRefTableChunk *chunk_data;
};

/* Declare and define a hash table over type BlockRefTableEntry. */
#define SH_PREFIX blockreftable
#define SH_ELEMENT_TYPE BlockRefTableEntry
#define SH_KEY_TYPE BlockRefTableKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) \
	hash_bytes((const unsigned char *) &key, sizeof(BlockRefTableKey))
#define SH_EQUAL(tb, a, b) (memcmp(&a, &b, sizeof(BlockRefTableKey)) == 0)
#define SH_SCOPE static inline
#ifdef FRONTEND
#define SH_RAW_ALLOCATOR pg_malloc0
#endif
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

/*
 * A block reference table is basically just the hash table, but we don't
 * want to expose that to outside callers.
 *
 * We keep track of the memory context in use explicitly too, so that it's
 * easy to place all of our allocations in the same context.
 */
struct BlockRefTable
{
	blockreftable_hash *hash;
#ifndef FRONTEND
	MemoryContext mcxt;
#endif
};

/*
 * On-disk serialization format for block reference table entries.
 */
typedef struct BlockRefTableSerializedEntry
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber limit_block;
	uint32		nchunks;
} BlockRefTableSerializedEntry;

/*
 * Buffer size, so that we avoid doing many small I/Os.
 */
#define BUFSIZE					65536

/*
 * Ad-hoc buffer for file I/O.
 */
typedef struct BlockRefTableBuffer
{
	io_callback_fn io_callback;
	void	   *io_callback_arg;
	char		data[BUFSIZE];
	int			used;
	int			cursor;
	pg_crc32c	crc;
} BlockRefTableBuffer;

/*
 * State for keeping track of progress while incrementally reading a block
 * table reference file from disk.
 *
 * total_chunks means the number of chunks for the RelFileLocator/ForkNumber
 * combination that is currently being read, and consumed_chunks is the number
 * of those that have been read. (We always read all the information for
 * a single chunk at one time, so we don't need to be able to represent the
 * state where a chunk has been partially read.)
 *
 * chunk_size is the array of chunk sizes. The length is given by total_chunks.
 *
 * chunk_data holds the current chunk.
 *
 * chunk_position helps us figure out how much progress we've made in returning
 * the block numbers for the current chunk to the caller. If the chunk is a
 * bitmap, it's the number of bits we've scanned; otherwise, it's the number
 * of chunk entries we've scanned.
 */
struct BlockRefTableReader
{
	BlockRefTableBuffer buffer;
	char	   *error_filename;
	report_error_fn error_callback;
	void	   *error_callback_arg;
	uint32		total_chunks;
	uint32		consumed_chunks;
	uint16	   *chunk_size;
	uint16		chunk_data[MAX_ENTRIES_PER_CHUNK];
	uint32		chunk_position;
};

/*
 * State for keeping track of progress while incrementally writing a block
 * reference table file to disk.
 */
struct BlockRefTableWriter
{
	BlockRefTableBuffer buffer;
};

/* Function prototypes. */
static int	BlockRefTableComparator(const void *a, const void *b);
static void BlockRefTableFlush(BlockRefTableBuffer *buffer);
static void BlockRefTableRead(BlockRefTableReader *reader, void *data,
							  int length);
static void BlockRefTableWrite(BlockRefTableBuffer *buffer, void *data,
							   int length);
static void BlockRefTableFileTerminate(BlockRefTableBuffer *buffer);

/*
 * Create an empty block reference table.
 */
BlockRefTable *
CreateEmptyBlockRefTable(void)
{
	BlockRefTable *brtab = palloc(sizeof(BlockRefTable));

	/*
	 * Even completely empty database has a few hundred relation forks, so it
	 * seems best to size the hash on the assumption that we're going to have
	 * at least a few thousand entries.
	 */
#ifdef FRONTEND
	brtab->hash = blockreftable_create(4096, NULL);
#else
	brtab->mcxt = CurrentMemoryContext;
	brtab->hash = blockreftable_create(brtab->mcxt, 4096, NULL);
#endif

	return brtab;
}

/*
 * Set the "limit block" for a relation fork and forget any modified blocks
 * with equal or higher block numbers.
 *
 * The "limit block" is the shortest known length of the relation within the
 * range of WAL records covered by this block reference table.
 */
void
BlockRefTableSetLimitBlock(BlockRefTable *brtab,
						   const RelFileLocator *rlocator,
						   ForkNumber forknum,
						   BlockNumber limit_block)
{
	BlockRefTableEntry *brtentry;
	BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
	bool		found;

	memcpy(&key.rlocator, rlocator, sizeof(RelFileLocator));
	key.forknum = forknum;
	brtentry = blockreftable_insert(brtab->hash, key, &found);

	if (!found)
	{
		/*
		 * We have no existing data about this relation fork, so just record
		 * the limit_block value supplied by the caller, and make sure other
		 * parts of the entry are properly initialized.
		 */
		brtentry->limit_block = limit_block;
		brtentry->nchunks = 0;
		brtentry->chunk_size = NULL;
		brtentry->chunk_usage = NULL;
		brtentry->chunk_data = NULL;
		return;
	}

	BlockRefTableEntrySetLimitBlock(brtentry, limit_block);
}

/*
 * Mark a block in a given relation fork as known to have been modified.
 */
void
BlockRefTableMarkBlockModified(BlockRefTable *brtab,
							   const RelFileLocator *rlocator,
							   ForkNumber forknum,
							   BlockNumber blknum)
{
	BlockRefTableEntry *brtentry;
	BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
	bool		found;
#ifndef FRONTEND
	MemoryContext oldcontext = MemoryContextSwitchTo(brtab->mcxt);
#endif

	memcpy(&key.rlocator, rlocator, sizeof(RelFileLocator));
	key.forknum = forknum;
	brtentry = blockreftable_insert(brtab->hash, key, &found);

	if (!found)
	{
		/*
		 * We want to set the initial limit block value to something higher
		 * than any legal block number. InvalidBlockNumber fits the bill.
		 */
		brtentry->limit_block = InvalidBlockNumber;
		brtentry->nchunks = 0;
		brtentry->chunk_size = NULL;
		brtentry->chunk_usage = NULL;
		brtentry->chunk_data = NULL;
	}

	BlockRefTableEntryMarkBlockModified(brtentry, forknum, blknum);

#ifndef FRONTEND
	MemoryContextSwitchTo(oldcontext);
#endif
}

/*
 * Get an entry from a block reference table.
 *
 * If the entry does not exist, this function returns NULL. Otherwise, it
 * returns the entry and sets *limit_block to the value from the entry.
 */
BlockRefTableEntry *
BlockRefTableGetEntry(BlockRefTable *brtab, const RelFileLocator *rlocator,
					  ForkNumber forknum, BlockNumber *limit_block)
{
	BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
	BlockRefTableEntry *entry;

	Assert(limit_block != NULL);

	memcpy(&key.rlocator, rlocator, sizeof(RelFileLocator));
	key.forknum = forknum;
	entry = blockreftable_lookup(brtab->hash, key);

	if (entry != NULL)
		*limit_block = entry->limit_block;

	return entry;
}

/*
 * Get block numbers from a table entry.
 *
 * 'blocks' must point to enough space to hold at least 'nblocks' block
 * numbers, and any block numbers we manage to get will be written there.
 * The return value is the number of block numbers actually written.
 *
 * We do not return block numbers unless they are greater than or equal to
 * start_blkno and strictly less than stop_blkno.
 */
int
BlockRefTableEntryGetBlocks(BlockRefTableEntry *entry,
							BlockNumber start_blkno,
							BlockNumber stop_blkno,
							BlockNumber *blocks,
							int nblocks)
{
	uint32		start_chunkno;
	uint32		stop_chunkno;
	uint32		chunkno;
	int			nresults = 0;

	Assert(entry != NULL);

	/*
	 * Figure out which chunks could potentially contain blocks of interest.
	 *
	 * We need to be careful about overflow here, because stop_blkno could be
	 * InvalidBlockNumber or something very close to it.
	 */
	start_chunkno = start_blkno / BLOCKS_PER_CHUNK;
	stop_chunkno = stop_blkno / BLOCKS_PER_CHUNK;
	if ((stop_blkno % BLOCKS_PER_CHUNK) != 0)
		++stop_chunkno;
	if (stop_chunkno > entry->nchunks)
		stop_chunkno = entry->nchunks;

	/*
	 * Loop over chunks.
	 */
	for (chunkno = start_chunkno; chunkno < stop_chunkno; ++chunkno)
	{
		uint16		chunk_usage = entry->chunk_usage[chunkno];
		BlockRefTableChunk chunk_data = entry->chunk_data[chunkno];
		unsigned	start_offset = 0;
		unsigned	stop_offset = BLOCKS_PER_CHUNK;

		/*
		 * If the start and/or stop block number falls within this chunk, the
		 * whole chunk may not be of interest. Figure out which portion we
		 * care about, if it's not the whole thing.
		 */
		if (chunkno == start_chunkno)
			start_offset = start_blkno % BLOCKS_PER_CHUNK;
		if (chunkno == stop_chunkno - 1)
		{
			Assert(stop_blkno > chunkno * BLOCKS_PER_CHUNK);
			stop_offset = stop_blkno - (chunkno * BLOCKS_PER_CHUNK);
			Assert(stop_offset <= BLOCKS_PER_CHUNK);
		}

		/*
		 * Handling differs depending on whether this is an array of offsets
		 * or a bitmap.
		 */
		if (chunk_usage == MAX_ENTRIES_PER_CHUNK)
		{
			unsigned	i;

			/* It's a bitmap, so test every relevant bit. */
			for (i = start_offset; i < stop_offset; ++i)
			{
				uint16		w = chunk_data[i / BLOCKS_PER_ENTRY];

				if ((w & (1 << (i % BLOCKS_PER_ENTRY))) != 0)
				{
					BlockNumber blkno = chunkno * BLOCKS_PER_CHUNK + i;

					blocks[nresults++] = blkno;

					/* Early exit if we run out of output space. */
					if (nresults == nblocks)
						return nresults;
				}
			}
		}
		else
		{
			unsigned	i;

			/* It's an array of offsets, so check each one. */
			for (i = 0; i < chunk_usage; ++i)
			{
				uint16		offset = chunk_data[i];

				if (offset >= start_offset && offset < stop_offset)
				{
					BlockNumber blkno = chunkno * BLOCKS_PER_CHUNK + offset;

					blocks[nresults++] = blkno;

					/* Early exit if we run out of output space. */
					if (nresults == nblocks)
						return nresults;
				}
			}
		}
	}

	return nresults;
}

/*
 * Serialize a block reference table to a file.
 */
void
WriteBlockRefTable(BlockRefTable *brtab,
				   io_callback_fn write_callback,
				   void *write_callback_arg)
{
	BlockRefTableSerializedEntry *sdata = NULL;
	BlockRefTableBuffer buffer;
	uint32		magic = BLOCKREFTABLE_MAGIC;

	/* Prepare buffer. */
	memset(&buffer, 0, sizeof(BlockRefTableBuffer));
	buffer.io_callback = write_callback;
	buffer.io_callback_arg = write_callback_arg;
	INIT_CRC32C(buffer.crc);

	/* Write magic number. */
	BlockRefTableWrite(&buffer, &magic, sizeof(uint32));

	/* Write the entries, assuming there are some. */
	if (brtab->hash->members > 0)
	{
		unsigned	i = 0;
		blockreftable_iterator it;
		BlockRefTableEntry *brtentry;

		/* Extract entries into serializable format and sort them. */
		sdata =
			palloc(brtab->hash->members * sizeof(BlockRefTableSerializedEntry));
		blockreftable_start_iterate(brtab->hash, &it);
		while ((brtentry = blockreftable_iterate(brtab->hash, &it)) != NULL)
		{
			BlockRefTableSerializedEntry *sentry = &sdata[i++];

			sentry->rlocator = brtentry->key.rlocator;
			sentry->forknum = brtentry->key.forknum;
			sentry->limit_block = brtentry->limit_block;
			sentry->nchunks = brtentry->nchunks;

			/* trim trailing zero entries */
			while (sentry->nchunks > 0 &&
				   brtentry->chunk_usage[sentry->nchunks - 1] == 0)
				sentry->nchunks--;
		}
		Assert(i == brtab->hash->members);
		qsort(sdata, i, sizeof(BlockRefTableSerializedEntry),
			  BlockRefTableComparator);

		/* Loop over entries in sorted order and serialize each one. */
		for (i = 0; i < brtab->hash->members; ++i)
		{
			BlockRefTableSerializedEntry *sentry = &sdata[i];
			BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
			unsigned	j;

			/* Write the serialized entry itself. */
			BlockRefTableWrite(&buffer, sentry,
							   sizeof(BlockRefTableSerializedEntry));

			/* Look up the original entry so we can access the chunks. */
			memcpy(&key.rlocator, &sentry->rlocator, sizeof(RelFileLocator));
			key.forknum = sentry->forknum;
			brtentry = blockreftable_lookup(brtab->hash, key);
			Assert(brtentry != NULL);

			/* Write the untruncated portion of the chunk length array. */
			if (sentry->nchunks != 0)
				BlockRefTableWrite(&buffer, brtentry->chunk_usage,
								   sentry->nchunks * sizeof(uint16));

			/* Write the contents of each chunk. */
			for (j = 0; j < brtentry->nchunks; ++j)
			{
				if (brtentry->chunk_usage[j] == 0)
					continue;
				BlockRefTableWrite(&buffer, brtentry->chunk_data[j],
								   brtentry->chunk_usage[j] * sizeof(uint16));
			}
		}
	}

	/* Write out appropriate terminator and CRC and flush buffer. */
	BlockRefTableFileTerminate(&buffer);
}

/*
 * Prepare to incrementally read a block reference table file.
 *
 * 'read_callback' is a function that can be called to read data from the
 * underlying file (or other data source) into our internal buffer.
 *
 * 'read_callback_arg' is an opaque argument to be passed to read_callback.
 *
 * 'error_filename' is the filename that should be included in error messages
 * if the file is found to be malformed. The value is not copied, so the
 * caller should ensure that it remains valid until done with this
 * BlockRefTableReader.
 *
 * 'error_callback' is a function to be called if the file is found to be
 * malformed. This is not used for I/O errors, which must be handled internally
 * by read_callback.
 *
 * 'error_callback_arg' is an opaque argument to be passed to error_callback.
 */
BlockRefTableReader *
CreateBlockRefTableReader(io_callback_fn read_callback,
						  void *read_callback_arg,
						  char *error_filename,
						  report_error_fn error_callback,
						  void *error_callback_arg)
{
	BlockRefTableReader *reader;
	uint32		magic;

	/* Initialize data structure. */
	reader = palloc0(sizeof(BlockRefTableReader));
	reader->buffer.io_callback = read_callback;
	reader->buffer.io_callback_arg = read_callback_arg;
	reader->error_filename = error_filename;
	reader->error_callback = error_callback;
	reader->error_callback_arg = error_callback_arg;
	INIT_CRC32C(reader->buffer.crc);

	/* Verify magic number. */
	BlockRefTableRead(reader, &magic, sizeof(uint32));
	if (magic != BLOCKREFTABLE_MAGIC)
		error_callback(error_callback_arg,
					   "file \"%s\" has wrong magic number: expected %u, found %u",
					   error_filename,
					   BLOCKREFTABLE_MAGIC, magic);

	return reader;
}

/*
 * Read next relation fork covered by this block reference table file.
 *
 * After calling this function, you must call BlockRefTableReaderGetBlocks
 * until it returns 0 before calling it again.
 */
bool
BlockRefTableReaderNextRelation(BlockRefTableReader *reader,
								RelFileLocator *rlocator,
								ForkNumber *forknum,
								BlockNumber *limit_block)
{
	BlockRefTableSerializedEntry sentry;
	BlockRefTableSerializedEntry zentry = {{0}};

	/*
	 * Sanity check: caller must read all blocks from all chunks before moving
	 * on to the next relation.
	 */
	Assert(reader->total_chunks == reader->consumed_chunks);

	/* Read serialized entry. */
	BlockRefTableRead(reader, &sentry,
					  sizeof(BlockRefTableSerializedEntry));

	/*
	 * If we just read the sentinel entry indicating that we've reached the
	 * end, read and check the CRC.
	 */
	if (memcmp(&sentry, &zentry, sizeof(BlockRefTableSerializedEntry)) == 0)
	{
		pg_crc32c	expected_crc;
		pg_crc32c	actual_crc;

		/*
		 * We want to know the CRC of the file excluding the 4-byte CRC
		 * itself, so copy the current value of the CRC accumulator before
		 * reading those bytes, and use the copy to finalize the calculation.
		 */
		expected_crc = reader->buffer.crc;
		FIN_CRC32C(expected_crc);

		/* Now we can read the actual value. */
		BlockRefTableRead(reader, &actual_crc, sizeof(pg_crc32c));

		/* Throw an error if there is a mismatch. */
		if (!EQ_CRC32C(expected_crc, actual_crc))
			reader->error_callback(reader->error_callback_arg,
								   "file \"%s\" has wrong checksum: expected %08X, found %08X",
								   reader->error_filename, expected_crc, actual_crc);

		return false;
	}

	/* Read chunk size array. */
	if (reader->chunk_size != NULL)
		pfree(reader->chunk_size);
	reader->chunk_size = palloc(sentry.nchunks * sizeof(uint16));
	BlockRefTableRead(reader, reader->chunk_size,
					  sentry.nchunks * sizeof(uint16));

	/* Set up for chunk scan. */
	reader->total_chunks = sentry.nchunks;
	reader->consumed_chunks = 0;

	/* Return data to caller. */
	memcpy(rlocator, &sentry.rlocator, sizeof(RelFileLocator));
	*forknum = sentry.forknum;
	*limit_block = sentry.limit_block;
	return true;
}

/*
 * Get modified blocks associated with the relation fork returned by
 * the most recent call to BlockRefTableReaderNextRelation.
 *
 * On return, block numbers will be written into the 'blocks' array, whose
 * length should be passed via 'nblocks'. The return value is the number of
 * entries actually written into the 'blocks' array, which may be less than
 * 'nblocks' if we run out of modified blocks in the relation fork before
 * we run out of room in the array.
 */
unsigned
BlockRefTableReaderGetBlocks(BlockRefTableReader *reader,
							 BlockNumber *blocks,
							 int nblocks)
{
	unsigned	blocks_found = 0;

	/* Must provide space for at least one block number to be returned. */
	Assert(nblocks > 0);

	/* Loop collecting blocks to return to caller. */
	for (;;)
	{
		uint16		next_chunk_size;

		/*
		 * If we've read at least one chunk, maybe it contains some block
		 * numbers that could satisfy caller's request.
		 */
		if (reader->consumed_chunks > 0)
		{
			uint32		chunkno = reader->consumed_chunks - 1;
			uint16		chunk_size = reader->chunk_size[chunkno];

			if (chunk_size == MAX_ENTRIES_PER_CHUNK)
			{
				/* Bitmap format, so search for bits that are set. */
				while (reader->chunk_position < BLOCKS_PER_CHUNK &&
					   blocks_found < nblocks)
				{
					uint16		chunkoffset = reader->chunk_position;
					uint16		w;

					w = reader->chunk_data[chunkoffset / BLOCKS_PER_ENTRY];
					if ((w & (1u << (chunkoffset % BLOCKS_PER_ENTRY))) != 0)
						blocks[blocks_found++] =
							chunkno * BLOCKS_PER_CHUNK + chunkoffset;
					++reader->chunk_position;
				}
			}
			else
			{
				/* Not in bitmap format, so each entry is a 2-byte offset. */
				while (reader->chunk_position < chunk_size &&
					   blocks_found < nblocks)
				{
					blocks[blocks_found++] = chunkno * BLOCKS_PER_CHUNK
						+ reader->chunk_data[reader->chunk_position];
					++reader->chunk_position;
				}
			}
		}

		/* We found enough blocks, so we're done. */
		if (blocks_found >= nblocks)
			break;

		/*
		 * We didn't find enough blocks, so we must need the next chunk. If
		 * there are none left, though, then we're done anyway.
		 */
		if (reader->consumed_chunks == reader->total_chunks)
			break;

		/*
		 * Read data for next chunk and reset scan position to beginning of
		 * chunk. Note that the next chunk might be empty, in which case we
		 * consume the chunk without actually consuming any bytes from the
		 * underlying file.
		 */
		next_chunk_size = reader->chunk_size[reader->consumed_chunks];
		if (next_chunk_size > 0)
			BlockRefTableRead(reader, reader->chunk_data,
							  next_chunk_size * sizeof(uint16));
		++reader->consumed_chunks;
		reader->chunk_position = 0;
	}

	return blocks_found;
}

/*
 * Release memory used while reading a block reference table from a file.
 */
void
DestroyBlockRefTableReader(BlockRefTableReader *reader)
{
	if (reader->chunk_size != NULL)
	{
		pfree(reader->chunk_size);
		reader->chunk_size = NULL;
	}
	pfree(reader);
}

/*
 * Prepare to write a block reference table file incrementally.
 *
 * Caller must be able to supply BlockRefTableEntry objects sorted in the
 * appropriate order.
 */
BlockRefTableWriter *
CreateBlockRefTableWriter(io_callback_fn write_callback,
						  void *write_callback_arg)
{
	BlockRefTableWriter *writer;
	uint32		magic = BLOCKREFTABLE_MAGIC;

	/* Prepare buffer and CRC check and save callbacks. */
	writer = palloc0(sizeof(BlockRefTableWriter));
	writer->buffer.io_callback = write_callback;
	writer->buffer.io_callback_arg = write_callback_arg;
	INIT_CRC32C(writer->buffer.crc);

	/* Write magic number. */
	BlockRefTableWrite(&writer->buffer, &magic, sizeof(uint32));

	return writer;
}

/*
 * Append one entry to a block reference table file.
 *
 * Note that entries must be written in the proper order, that is, sorted by
 * tablespace, then database, then relfilenumber, then fork number. Caller
 * is responsible for supplying data in the correct order. If that seems hard,
 * use an in-memory BlockRefTable instead.
 */
void
BlockRefTableWriteEntry(BlockRefTableWriter *writer, BlockRefTableEntry *entry)
{
	BlockRefTableSerializedEntry sentry;
	unsigned	j;

	/* Convert to serialized entry format. */
	sentry.rlocator = entry->key.rlocator;
	sentry.forknum = entry->key.forknum;
	sentry.limit_block = entry->limit_block;
	sentry.nchunks = entry->nchunks;

	/* Trim trailing zero entries. */
	while (sentry.nchunks > 0 && entry->chunk_usage[sentry.nchunks - 1] == 0)
		sentry.nchunks--;

	/* Write the serialized entry itself. */
	BlockRefTableWrite(&writer->buffer, &sentry,
					   sizeof(BlockRefTableSerializedEntry));

	/* Write the untruncated portion of the chunk length array. */
	if (sentry.nchunks != 0)
		BlockRefTableWrite(&writer->buffer, entry->chunk_usage,
						   sentry.nchunks * sizeof(uint16));

	/* Write the contents of each chunk. */
	for (j = 0; j < entry->nchunks; ++j)
	{
		if (entry->chunk_usage[j] == 0)
			continue;
		BlockRefTableWrite(&writer->buffer, entry->chunk_data[j],
						   entry->chunk_usage[j] * sizeof(uint16));
	}
}

/*
 * Finalize an incremental write of a block reference table file.
 */
void
DestroyBlockRefTableWriter(BlockRefTableWriter *writer)
{
	BlockRefTableFileTerminate(&writer->buffer);
	pfree(writer);
}

/*
 * Allocate a standalone BlockRefTableEntry.
 *
 * When we're manipulating a full in-memory BlockRefTable, the entries are
 * part of the hash table and are allocated by simplehash. This routine is
 * used by callers that want to write out a BlockRefTable to a file without
 * needing to store the whole thing in memory at once.
 *
 * Entries allocated by this function can be manipulated using the functions
 * BlockRefTableEntrySetLimitBlock and BlockRefTableEntryMarkBlockModified
 * and then written using BlockRefTableWriteEntry and freed using
 * BlockRefTableFreeEntry.
 */
BlockRefTableEntry *
CreateBlockRefTableEntry(RelFileLocator rlocator, ForkNumber forknum)
{
	BlockRefTableEntry *entry = palloc0(sizeof(BlockRefTableEntry));

	memcpy(&entry->key.rlocator, &rlocator, sizeof(RelFileLocator));
	entry->key.forknum = forknum;
	entry->limit_block = InvalidBlockNumber;

	return entry;
}

/*
 * Update a BlockRefTableEntry with a new value for the "limit block" and
 * forget any equal-or-higher-numbered modified blocks.
 *
 * The "limit block" is the shortest known length of the relation within the
 * range of WAL records covered by this block reference table.
 */
void
BlockRefTableEntrySetLimitBlock(BlockRefTableEntry *entry,
								BlockNumber limit_block)
{
	unsigned	chunkno;
	unsigned	limit_chunkno;
	unsigned	limit_chunkoffset;
	BlockRefTableChunk limit_chunk;

	/* If we already have an equal or lower limit block, do nothing. */
	if (limit_block >= entry->limit_block)
		return;

	/* Record the new limit block value. */
	entry->limit_block = limit_block;

	/*
	 * Figure out which chunk would store the state of the new limit block,
	 * and which offset within that chunk.
	 */
	limit_chunkno = limit_block / BLOCKS_PER_CHUNK;
	limit_chunkoffset = limit_block % BLOCKS_PER_CHUNK;

	/*
	 * If the number of chunks is not large enough for any blocks with equal
	 * or higher block numbers to exist, then there is nothing further to do.
	 */
	if (limit_chunkno >= entry->nchunks)
		return;

	/* Discard entire contents of any higher-numbered chunks. */
	for (chunkno = limit_chunkno + 1; chunkno < entry->nchunks; ++chunkno)
		entry->chunk_usage[chunkno] = 0;

	/*
	 * Next, we need to discard any offsets within the chunk that would
	 * contain the limit_block. We must handle this differently depending on
	 * whether the chunk that would contain limit_block is a bitmap or an
	 * array of offsets.
	 */
	limit_chunk = entry->chunk_data[limit_chunkno];
	if (entry->chunk_usage[limit_chunkno] == MAX_ENTRIES_PER_CHUNK)
	{
		unsigned	chunkoffset;

		/* It's a bitmap. Unset bits. */
		for (chunkoffset = limit_chunkoffset; chunkoffset < BLOCKS_PER_CHUNK;
			 ++chunkoffset)
			limit_chunk[chunkoffset / BLOCKS_PER_ENTRY] &=
				~(1 << (chunkoffset % BLOCKS_PER_ENTRY));
	}
	else
	{
		unsigned	i,
					j = 0;

		/* It's an offset array. Filter out large offsets. */
		for (i = 0; i < entry->chunk_usage[limit_chunkno]; ++i)
		{
			Assert(j <= i);
			if (limit_chunk[i] < limit_chunkoffset)
				limit_chunk[j++] = limit_chunk[i];
		}
		Assert(j <= entry->chunk_usage[limit_chunkno]);
		entry->chunk_usage[limit_chunkno] = j;
	}
}

/*
 * Mark a block in a given BlockRefTableEntry as known to have been modified.
 */
void
BlockRefTableEntryMarkBlockModified(BlockRefTableEntry *entry,
									ForkNumber forknum,
									BlockNumber blknum)
{
	unsigned	chunkno;
	unsigned	chunkoffset;
	unsigned	i;

	/*
	 * Which chunk should store the state of this block? And what is the
	 * offset of this block relative to the start of that chunk?
	 */
	chunkno = blknum / BLOCKS_PER_CHUNK;
	chunkoffset = blknum % BLOCKS_PER_CHUNK;

	/*
	 * If 'nchunks' isn't big enough for us to be able to represent the state
	 * of this block, we need to enlarge our arrays.
	 */
	if (chunkno >= entry->nchunks)
	{
		unsigned	max_chunks;
		unsigned	extra_chunks;

		/*
		 * New array size is a power of 2, at least 16, big enough so that
		 * chunkno will be a valid array index.
		 */
		max_chunks = Max(16, entry->nchunks);
		while (max_chunks < chunkno + 1)
			max_chunks *= 2;
		extra_chunks = max_chunks - entry->nchunks;

		if (entry->nchunks == 0)
		{
			entry->chunk_size = palloc0(sizeof(uint16) * max_chunks);
			entry->chunk_usage = palloc0(sizeof(uint16) * max_chunks);
			entry->chunk_data =
				palloc0(sizeof(BlockRefTableChunk) * max_chunks);
		}
		else
		{
			entry->chunk_size = repalloc(entry->chunk_size,
										 sizeof(uint16) * max_chunks);
			memset(&entry->chunk_size[entry->nchunks], 0,
				   extra_chunks * sizeof(uint16));
			entry->chunk_usage = repalloc(entry->chunk_usage,
										  sizeof(uint16) * max_chunks);
			memset(&entry->chunk_usage[entry->nchunks], 0,
				   extra_chunks * sizeof(uint16));
			entry->chunk_data = repalloc(entry->chunk_data,
										 sizeof(BlockRefTableChunk) * max_chunks);
			memset(&entry->chunk_data[entry->nchunks], 0,
				   extra_chunks * sizeof(BlockRefTableChunk));
		}
		entry->nchunks = max_chunks;
	}

	/*
	 * If the chunk that covers this block number doesn't exist yet, create it
	 * as an array and add the appropriate offset to it. We make it pretty
	 * small initially, because there might only be 1 or a few block
	 * references in this chunk and we don't want to use up too much memory.
	 */
	if (entry->chunk_size[chunkno] == 0)
	{
		entry->chunk_data[chunkno] =
			palloc(sizeof(uint16) * INITIAL_ENTRIES_PER_CHUNK);
		entry->chunk_size[chunkno] = INITIAL_ENTRIES_PER_CHUNK;
		entry->chunk_data[chunkno][0] = chunkoffset;
		entry->chunk_usage[chunkno] = 1;
		return;
	}

	/*
	 * If the number of entries in this chunk is already maximum, it must be a
	 * bitmap. Just set the appropriate bit.
	 */
	if (entry->chunk_usage[chunkno] == MAX_ENTRIES_PER_CHUNK)
	{
		BlockRefTableChunk chunk = entry->chunk_data[chunkno];

		chunk[chunkoffset / BLOCKS_PER_ENTRY] |=
			1 << (chunkoffset % BLOCKS_PER_ENTRY);
		return;
	}

	/*
	 * There is an existing chunk and it's in array format. Let's find out
	 * whether it already has an entry for this block. If so, we do not need
	 * to do anything.
	 */
	for (i = 0; i < entry->chunk_usage[chunkno]; ++i)
	{
		if (entry->chunk_data[chunkno][i] == chunkoffset)
			return;
	}

	/*
	 * If the number of entries currently used is one less than the maximum,
	 * it's time to convert to bitmap format.
	 */
	if (entry->chunk_usage[chunkno] == MAX_ENTRIES_PER_CHUNK - 1)
	{
		BlockRefTableChunk newchunk;
		unsigned	j;

		/* Allocate a new chunk. */
		newchunk = palloc0(MAX_ENTRIES_PER_CHUNK * sizeof(uint16));

		/* Set the bit for each existing entry. */
		for (j = 0; j < entry->chunk_usage[chunkno]; ++j)
		{
			unsigned	coff = entry->chunk_data[chunkno][j];

			newchunk[coff / BLOCKS_PER_ENTRY] |=
				1 << (coff % BLOCKS_PER_ENTRY);
		}

		/* Set the bit for the new entry. */
		newchunk[chunkoffset / BLOCKS_PER_ENTRY] |=
			1 << (chunkoffset % BLOCKS_PER_ENTRY);

		/* Swap the new chunk into place and update metadata. */
		pfree(entry->chunk_data[chunkno]);
		entry->chunk_data[chunkno] = newchunk;
		entry->chunk_size[chunkno] = MAX_ENTRIES_PER_CHUNK;
		entry->chunk_usage[chunkno] = MAX_ENTRIES_PER_CHUNK;
		return;
	}

	/*
	 * OK, we currently have an array, and we don't need to convert to a
	 * bitmap, but we do need to add a new element. If there's not enough
	 * room, we'll have to expand the array.
	 */
	if (entry->chunk_usage[chunkno] == entry->chunk_size[chunkno])
	{
		unsigned	newsize = entry->chunk_size[chunkno] * 2;

		Assert(newsize <= MAX_ENTRIES_PER_CHUNK);
		entry->chunk_data[chunkno] = repalloc(entry->chunk_data[chunkno],
											  newsize * sizeof(uint16));
		entry->chunk_size[chunkno] = newsize;
	}

	/* Now we can add the new entry. */
	entry->chunk_data[chunkno][entry->chunk_usage[chunkno]] =
		chunkoffset;
	entry->chunk_usage[chunkno]++;
}

/*
 * Release memory for a BlockRefTableEntry that was created by
 * CreateBlockRefTableEntry.
 */
void
BlockRefTableFreeEntry(BlockRefTableEntry *entry)
{
	if (entry->chunk_size != NULL)
	{
		pfree(entry->chunk_size);
		entry->chunk_size = NULL;
	}

	if (entry->chunk_usage != NULL)
	{
		pfree(entry->chunk_usage);
		entry->chunk_usage = NULL;
	}

	if (entry->chunk_data != NULL)
	{
		pfree(entry->chunk_data);
		entry->chunk_data = NULL;
	}

	pfree(entry);
}

/*
 * Comparator for BlockRefTableSerializedEntry objects.
 *
 * We make the tablespace OID the first column of the sort key to match
 * the on-disk tree structure.
 */
static int
BlockRefTableComparator(const void *a, const void *b)
{
	const BlockRefTableSerializedEntry *sa = a;
	const BlockRefTableSerializedEntry *sb = b;

	if (sa->rlocator.spcOid > sb->rlocator.spcOid)
		return 1;
	if (sa->rlocator.spcOid < sb->rlocator.spcOid)
		return -1;

	if (sa->rlocator.dbOid > sb->rlocator.dbOid)
		return 1;
	if (sa->rlocator.dbOid < sb->rlocator.dbOid)
		return -1;

	if (sa->rlocator.relNumber > sb->rlocator.relNumber)
		return 1;
	if (sa->rlocator.relNumber < sb->rlocator.relNumber)
		return -1;

	if (sa->forknum > sb->forknum)
		return 1;
	if (sa->forknum < sb->forknum)
		return -1;

	return 0;
}

/*
 * Flush any buffered data out of a BlockRefTableBuffer.
 */
static void
BlockRefTableFlush(BlockRefTableBuffer *buffer)
{
	buffer->io_callback(buffer->io_callback_arg, buffer->data, buffer->used);
	buffer->used = 0;
}

/*
 * Read data from a BlockRefTableBuffer, and update the running CRC
 * calculation for the returned data (but not any data that we may have
 * buffered but not yet actually returned).
 */
static void
BlockRefTableRead(BlockRefTableReader *reader, void *data, int length)
{
	BlockRefTableBuffer *buffer = &reader->buffer;

	/* Loop until read is fully satisfied. */
	while (length > 0)
	{
		if (buffer->cursor < buffer->used)
		{
			/*
			 * If any buffered data is available, use that to satisfy as much
			 * of the request as possible.
			 */
			int			bytes_to_copy = Min(length, buffer->used - buffer->cursor);

			memcpy(data, &buffer->data[buffer->cursor], bytes_to_copy);
			COMP_CRC32C(buffer->crc, &buffer->data[buffer->cursor],
						bytes_to_copy);
			buffer->cursor += bytes_to_copy;
			data = ((char *) data) + bytes_to_copy;
			length -= bytes_to_copy;
		}
		else if (length >= BUFSIZE)
		{
			/*
			 * If the request length is long, read directly into caller's
			 * buffer.
			 */
			int			bytes_read;

			bytes_read = buffer->io_callback(buffer->io_callback_arg,
											 data, length);
			COMP_CRC32C(buffer->crc, data, bytes_read);
			data = ((char *) data) + bytes_read;
			length -= bytes_read;

			/* If we didn't get anything, that's bad. */
			if (bytes_read == 0)
				reader->error_callback(reader->error_callback_arg,
									   "file \"%s\" ends unexpectedly",
									   reader->error_filename);
		}
		else
		{
			/*
			 * Refill our buffer.
			 */
			buffer->used = buffer->io_callback(buffer->io_callback_arg,
											   buffer->data, BUFSIZE);
			buffer->cursor = 0;

			/* If we didn't get anything, that's bad. */
			if (buffer->used == 0)
				reader->error_callback(reader->error_callback_arg,
									   "file \"%s\" ends unexpectedly",
									   reader->error_filename);
		}
	}
}

/*
 * Supply data to a BlockRefTableBuffer for write to the underlying File,
 * and update the running CRC calculation for that data.
 */
static void
BlockRefTableWrite(BlockRefTableBuffer *buffer, void *data, int length)
{
	/* Update running CRC calculation. */
	COMP_CRC32C(buffer->crc, data, length);

	/* If the new data can't fit into the buffer, flush the buffer. */
	if (buffer->used + length > BUFSIZE)
	{
		buffer->io_callback(buffer->io_callback_arg, buffer->data,
							buffer->used);
		buffer->used = 0;
	}

	/* If the new data would fill the buffer, or more, write it directly. */
	if (length >= BUFSIZE)
	{
		buffer->io_callback(buffer->io_callback_arg, data, length);
		return;
	}

	/* Otherwise, copy the new data into the buffer. */
	memcpy(&buffer->data[buffer->used], data, length);
	buffer->used += length;
	Assert(buffer->used <= BUFSIZE);
}

/*
 * Generate the sentinel and CRC required at the end of a block reference
 * table file and flush them out of our internal buffer.
 */
static void
BlockRefTableFileTerminate(BlockRefTableBuffer *buffer)
{
	BlockRefTableSerializedEntry zentry = {{0}};
	pg_crc32c	crc;

	/* Write a sentinel indicating that there are no more entries. */
	BlockRefTableWrite(buffer, &zentry,
					   sizeof(BlockRefTableSerializedEntry));

	/*
	 * Writing the checksum will perturb the ongoing checksum calculation, so
	 * copy the state first and finalize the computation using the copy.
	 */
	crc = buffer->crc;
	FIN_CRC32C(crc);
	BlockRefTableWrite(buffer, &crc, sizeof(pg_crc32c));

	/* Flush any leftover data out of our buffer. */
	BlockRefTableFlush(buffer);
}
