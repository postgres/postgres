/*-------------------------------------------------------------------------
 *
 * reconstruct.c
 *		Reconstruct full file from incremental file and backup chain.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_combinebackup/reconstruct.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>

#include "backup/basebackup_incremental.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "copy_file.h"
#include "lib/stringinfo.h"
#include "reconstruct.h"
#include "storage/block.h"

/*
 * An rfile stores the data that we need in order to be able to use some file
 * on disk for reconstruction. For any given output file, we create one rfile
 * per backup that we need to consult when we constructing that output file.
 *
 * If we find a full version of the file in the backup chain, then only
 * filename and fd are initialized; the remaining fields are 0 or NULL.
 * For an incremental file, header_length, num_blocks, relative_block_numbers,
 * and truncation_block_length are also set.
 *
 * num_blocks_read and highest_offset_read always start out as 0.
 */
typedef struct rfile
{
	char	   *filename;
	int			fd;
	size_t		header_length;
	unsigned	num_blocks;
	BlockNumber *relative_block_numbers;
	unsigned	truncation_block_length;
	unsigned	num_blocks_read;
	off_t		highest_offset_read;
} rfile;

static void debug_reconstruction(int n_source,
								 rfile **sources,
								 bool dry_run);
static unsigned find_reconstructed_block_length(rfile *s);
static rfile *make_incremental_rfile(char *filename);
static rfile *make_rfile(char *filename, bool missing_ok);
static void write_reconstructed_file(char *input_filename,
									 char *output_filename,
									 unsigned block_length,
									 rfile **sourcemap,
									 off_t *offsetmap,
									 pg_checksum_context *checksum_ctx,
									 CopyMethod copy_method,
									 bool debug,
									 bool dry_run);
static void read_bytes(rfile *rf, void *buffer, unsigned length);
static void write_block(int fd, char *output_filename,
						uint8 *buffer,
						pg_checksum_context *checksum_ctx);
static void read_block(rfile *s, off_t off, uint8 *buffer);

/*
 * Reconstruct a full file from an incremental file and a chain of prior
 * backups.
 *
 * input_filename should be the path to the incremental file, and
 * output_filename should be the path where the reconstructed file is to be
 * written.
 *
 * relative_path should be the path to the directory containing this file,
 * relative to the root of the backup (NOT relative to the root of the
 * tablespace). It must always end with a trailing slash. bare_file_name
 * should be the name of the file within that directory, without
 * "INCREMENTAL.".
 *
 * n_prior_backups is the number of prior backups, and prior_backup_dirs is
 * an array of pathnames where those backups can be found.
 */
void
reconstruct_from_incremental_file(char *input_filename,
								  char *output_filename,
								  char *relative_path,
								  char *bare_file_name,
								  int n_prior_backups,
								  char **prior_backup_dirs,
								  manifest_data **manifests,
								  char *manifest_path,
								  pg_checksum_type checksum_type,
								  int *checksum_length,
								  uint8 **checksum_payload,
								  CopyMethod copy_method,
								  bool debug,
								  bool dry_run)
{
	rfile	  **source;
	rfile	   *latest_source = NULL;
	rfile	  **sourcemap;
	off_t	   *offsetmap;
	unsigned	block_length;
	unsigned	i;
	unsigned	sidx = n_prior_backups;
	bool		full_copy_possible = true;
	int			copy_source_index = -1;
	rfile	   *copy_source = NULL;
	pg_checksum_context checksum_ctx;

	/* Sanity check the relative_path. */
	Assert(relative_path[0] != '\0');
	Assert(relative_path[strlen(relative_path) - 1] == '/');

	/*
	 * Every block must come either from the latest version of the file or
	 * from one of the prior backups.
	 */
	source = pg_malloc0(sizeof(rfile *) * (1 + n_prior_backups));

	/*
	 * Use the information from the latest incremental file to figure out how
	 * long the reconstructed file should be.
	 */
	latest_source = make_incremental_rfile(input_filename);
	source[n_prior_backups] = latest_source;
	block_length = find_reconstructed_block_length(latest_source);

	/*
	 * For each block in the output file, we need to know from which file we
	 * need to obtain it and at what offset in that file it's stored.
	 * sourcemap gives us the first of these things, and offsetmap the latter.
	 */
	sourcemap = pg_malloc0(sizeof(rfile *) * block_length);
	offsetmap = pg_malloc0(sizeof(off_t) * block_length);

	/*
	 * Every block that is present in the newest incremental file should be
	 * sourced from that file. If it precedes the truncation_block_length,
	 * it's a block that we would otherwise have had to find in an older
	 * backup and thus reduces the number of blocks remaining to be found by
	 * one; otherwise, it's an extra block that needs to be included in the
	 * output but would not have needed to be found in an older backup if it
	 * had not been present.
	 */
	for (i = 0; i < latest_source->num_blocks; ++i)
	{
		BlockNumber b = latest_source->relative_block_numbers[i];

		Assert(b < block_length);
		sourcemap[b] = latest_source;
		offsetmap[b] = latest_source->header_length + (i * BLCKSZ);

		/*
		 * A full copy of a file from an earlier backup is only possible if no
		 * blocks are needed from any later incremental file.
		 */
		full_copy_possible = false;
	}

	while (1)
	{
		char		source_filename[MAXPGPATH];
		rfile	   *s;

		/*
		 * Move to the next backup in the chain. If there are no more, then
		 * we're done.
		 */
		if (sidx == 0)
			break;
		--sidx;

		/*
		 * Look for the full file in the previous backup. If not found, then
		 * look for an incremental file instead.
		 */
		snprintf(source_filename, MAXPGPATH, "%s/%s%s",
				 prior_backup_dirs[sidx], relative_path, bare_file_name);
		if ((s = make_rfile(source_filename, true)) == NULL)
		{
			snprintf(source_filename, MAXPGPATH, "%s/%sINCREMENTAL.%s",
					 prior_backup_dirs[sidx], relative_path, bare_file_name);
			s = make_incremental_rfile(source_filename);
		}
		source[sidx] = s;

		/*
		 * If s->header_length == 0, then this is a full file; otherwise, it's
		 * an incremental file.
		 */
		if (s->header_length == 0)
		{
			struct stat sb;
			BlockNumber b;
			BlockNumber blocklength;

			/* We need to know the length of the file. */
			if (fstat(s->fd, &sb) < 0)
				pg_fatal("could not stat file \"%s\": %m", s->filename);

			/*
			 * Since we found a full file, source all blocks from it that
			 * exist in the file.
			 *
			 * Note that there may be blocks that don't exist either in this
			 * file or in any incremental file but that precede
			 * truncation_block_length. These are, presumably, zero-filled
			 * blocks that result from the server extending the file but
			 * taking no action on those blocks that generated any WAL.
			 *
			 * Sadly, we have no way of validating that this is really what
			 * happened, and neither does the server.  From its perspective,
			 * an unmodified block that contains data looks exactly the same
			 * as a zero-filled block that never had any data: either way,
			 * it's not mentioned in any WAL summary and the server has no
			 * reason to read it. From our perspective, all we know is that
			 * nobody had a reason to back up the block. That certainly means
			 * that the block didn't exist at the time of the full backup, but
			 * the supposition that it was all zeroes at the time of every
			 * later backup is one that we can't validate.
			 */
			blocklength = sb.st_size / BLCKSZ;
			for (b = 0; b < latest_source->truncation_block_length; ++b)
			{
				if (sourcemap[b] == NULL && b < blocklength)
				{
					sourcemap[b] = s;
					offsetmap[b] = b * BLCKSZ;
				}
			}

			/*
			 * If a full copy looks possible, check whether the resulting file
			 * should be exactly as long as the source file is. If so, a full
			 * copy is acceptable, otherwise not.
			 */
			if (full_copy_possible)
			{
				uint64		expected_length;

				expected_length =
					(uint64) latest_source->truncation_block_length;
				expected_length *= BLCKSZ;
				if (expected_length == sb.st_size)
				{
					copy_source = s;
					copy_source_index = sidx;
				}
			}

			/* We don't need to consider any further sources. */
			break;
		}

		/*
		 * Since we found another incremental file, source all blocks from it
		 * that we need but don't yet have.
		 */
		for (i = 0; i < s->num_blocks; ++i)
		{
			BlockNumber b = s->relative_block_numbers[i];

			if (b < latest_source->truncation_block_length &&
				sourcemap[b] == NULL)
			{
				sourcemap[b] = s;
				offsetmap[b] = s->header_length + (i * BLCKSZ);

				/*
				 * A full copy of a file from an earlier backup is only
				 * possible if no blocks are needed from any later incremental
				 * file.
				 */
				full_copy_possible = false;
			}
		}
	}

	/*
	 * If a checksum of the required type already exists in the
	 * backup_manifest for the relevant input directory, we can save some work
	 * by reusing that checksum instead of computing a new one.
	 */
	if (copy_source_index >= 0 && manifests[copy_source_index] != NULL &&
		checksum_type != CHECKSUM_TYPE_NONE)
	{
		manifest_file *mfile;

		mfile = manifest_files_lookup(manifests[copy_source_index]->files,
									  manifest_path);
		if (mfile == NULL)
		{
			char	   *path = psprintf("%s/backup_manifest",
										prior_backup_dirs[copy_source_index]);

			/*
			 * The directory is out of sync with the backup_manifest, so emit
			 * a warning.
			 */
			pg_log_warning("manifest file \"%s\" contains no entry for file \"%s\"",
						   path,
						   manifest_path);
			pfree(path);
		}
		else if (mfile->checksum_type == checksum_type)
		{
			*checksum_length = mfile->checksum_length;
			*checksum_payload = pg_malloc(*checksum_length);
			memcpy(*checksum_payload, mfile->checksum_payload,
				   *checksum_length);
			checksum_type = CHECKSUM_TYPE_NONE;
		}
	}

	/* Prepare for checksum calculation, if required. */
	pg_checksum_init(&checksum_ctx, checksum_type);

	/*
	 * If the full file can be created by copying a file from an older backup
	 * in the chain without needing to overwrite any blocks or truncate the
	 * result, then forget about performing reconstruction and just copy that
	 * file in its entirety.
	 *
	 * If we have only incremental files, and there's no full file at any
	 * point in the backup chain, something has gone wrong. Emit an error.
	 *
	 * Otherwise, reconstruct.
	 */
	if (copy_source != NULL)
		copy_file(copy_source->filename, output_filename,
				  &checksum_ctx, copy_method, dry_run);
	else if (sidx == 0 && source[0]->header_length != 0)
	{
		pg_fatal("full backup contains unexpected incremental file \"%s\"",
				 source[0]->filename);
	}
	else
	{
		write_reconstructed_file(input_filename, output_filename,
								 block_length, sourcemap, offsetmap,
								 &checksum_ctx, copy_method,
								 debug, dry_run);
		debug_reconstruction(n_prior_backups + 1, source, dry_run);
	}

	/* Save results of checksum calculation. */
	if (checksum_type != CHECKSUM_TYPE_NONE)
	{
		*checksum_payload = pg_malloc(PG_CHECKSUM_MAX_LENGTH);
		*checksum_length = pg_checksum_final(&checksum_ctx,
											 *checksum_payload);
	}

	/*
	 * Close files and release memory.
	 */
	for (i = 0; i <= n_prior_backups; ++i)
	{
		rfile	   *s = source[i];

		if (s == NULL)
			continue;
		if (close(s->fd) != 0)
			pg_fatal("could not close file \"%s\": %m", s->filename);
		if (s->relative_block_numbers != NULL)
			pfree(s->relative_block_numbers);
		pg_free(s->filename);
	}
	pfree(sourcemap);
	pfree(offsetmap);
	pfree(source);
}

/*
 * Perform post-reconstruction logging and sanity checks.
 */
static void
debug_reconstruction(int n_source, rfile **sources, bool dry_run)
{
	unsigned	i;

	for (i = 0; i < n_source; ++i)
	{
		rfile	   *s = sources[i];

		/* Ignore source if not used. */
		if (s == NULL)
			continue;

		/* If no data is needed from this file, we can ignore it. */
		if (s->num_blocks_read == 0)
			continue;

		/* Debug logging. */
		if (dry_run)
			pg_log_debug("would have read %u blocks from \"%s\"",
						 s->num_blocks_read, s->filename);
		else
			pg_log_debug("read %u blocks from \"%s\"",
						 s->num_blocks_read, s->filename);

		/*
		 * In dry-run mode, we don't actually try to read data from the file,
		 * but we do try to verify that the file is long enough that we could
		 * have read the data if we'd tried.
		 *
		 * If this fails, then it means that a non-dry-run attempt would fail,
		 * complaining of not being able to read the required bytes from the
		 * file.
		 */
		if (dry_run)
		{
			struct stat sb;

			if (fstat(s->fd, &sb) < 0)
				pg_fatal("could not stat file \"%s\": %m", s->filename);
			if (sb.st_size < s->highest_offset_read)
				pg_fatal("file \"%s\" is too short: expected %llu, found %llu",
						 s->filename,
						 (unsigned long long) s->highest_offset_read,
						 (unsigned long long) sb.st_size);
		}
	}
}

/*
 * When we perform reconstruction using an incremental file, the output file
 * should be at least as long as the truncation_block_length. Any blocks
 * present in the incremental file increase the output length as far as is
 * necessary to include those blocks.
 */
static unsigned
find_reconstructed_block_length(rfile *s)
{
	unsigned	block_length = s->truncation_block_length;
	unsigned	i;

	for (i = 0; i < s->num_blocks; ++i)
		if (s->relative_block_numbers[i] >= block_length)
			block_length = s->relative_block_numbers[i] + 1;

	return block_length;
}

/*
 * Initialize an incremental rfile, reading the header so that we know which
 * blocks it contains.
 */
static rfile *
make_incremental_rfile(char *filename)
{
	rfile	   *rf;
	unsigned	magic;

	rf = make_rfile(filename, false);

	/* Read and validate magic number. */
	read_bytes(rf, &magic, sizeof(magic));
	if (magic != INCREMENTAL_MAGIC)
		pg_fatal("file \"%s\" has bad incremental magic number (0x%x, expected 0x%x)",
				 filename, magic, INCREMENTAL_MAGIC);

	/* Read block count. */
	read_bytes(rf, &rf->num_blocks, sizeof(rf->num_blocks));
	if (rf->num_blocks > RELSEG_SIZE)
		pg_fatal("file \"%s\" has block count %u in excess of segment size %u",
				 filename, rf->num_blocks, RELSEG_SIZE);

	/* Read truncation block length. */
	read_bytes(rf, &rf->truncation_block_length,
			   sizeof(rf->truncation_block_length));
	if (rf->truncation_block_length > RELSEG_SIZE)
		pg_fatal("file \"%s\" has truncation block length %u in excess of segment size %u",
				 filename, rf->truncation_block_length, RELSEG_SIZE);

	/* Read block numbers if there are any. */
	if (rf->num_blocks > 0)
	{
		rf->relative_block_numbers =
			pg_malloc0(sizeof(BlockNumber) * rf->num_blocks);
		read_bytes(rf, rf->relative_block_numbers,
				   sizeof(BlockNumber) * rf->num_blocks);
	}

	/* Remember length of header. */
	rf->header_length = sizeof(magic) + sizeof(rf->num_blocks) +
		sizeof(rf->truncation_block_length) +
		sizeof(BlockNumber) * rf->num_blocks;

	/*
	 * Round header length to a multiple of BLCKSZ, so that blocks contents
	 * are properly aligned. Only do this when the file actually has data for
	 * some blocks.
	 */
	if ((rf->num_blocks > 0) && ((rf->header_length % BLCKSZ) != 0))
		rf->header_length += (BLCKSZ - (rf->header_length % BLCKSZ));

	return rf;
}

/*
 * Allocate and perform basic initialization of an rfile.
 */
static rfile *
make_rfile(char *filename, bool missing_ok)
{
	rfile	   *rf;

	rf = pg_malloc0(sizeof(rfile));
	rf->filename = pstrdup(filename);
	if ((rf->fd = open(filename, O_RDONLY | PG_BINARY, 0)) < 0)
	{
		if (missing_ok && errno == ENOENT)
		{
			pg_free(rf);
			return NULL;
		}
		pg_fatal("could not open file \"%s\": %m", filename);
	}

	return rf;
}

/*
 * Read the indicated number of bytes from an rfile into the buffer.
 */
static void
read_bytes(rfile *rf, void *buffer, unsigned length)
{
	int			rb = read(rf->fd, buffer, length);

	if (rb != length)
	{
		if (rb < 0)
			pg_fatal("could not read file \"%s\": %m", rf->filename);
		else
			pg_fatal("could not read file \"%s\": read %d of %u",
					 rf->filename, rb, length);
	}
}

/*
 * Write out a reconstructed file.
 */
static void
write_reconstructed_file(char *input_filename,
						 char *output_filename,
						 unsigned block_length,
						 rfile **sourcemap,
						 off_t *offsetmap,
						 pg_checksum_context *checksum_ctx,
						 CopyMethod copy_method,
						 bool debug,
						 bool dry_run)
{
	int			wfd = -1;
	unsigned	i;
	unsigned	zero_blocks = 0;

	/* Debugging output. */
	if (debug)
	{
		StringInfoData debug_buf;
		unsigned	start_of_range = 0;
		unsigned	current_block = 0;

		/* Basic information about the output file to be produced. */
		if (dry_run)
			pg_log_debug("would reconstruct \"%s\" (%u blocks, checksum %s)",
						 output_filename, block_length,
						 pg_checksum_type_name(checksum_ctx->type));
		else
			pg_log_debug("reconstructing \"%s\" (%u blocks, checksum %s)",
						 output_filename, block_length,
						 pg_checksum_type_name(checksum_ctx->type));

		/* Print out the plan for reconstructing this file. */
		initStringInfo(&debug_buf);
		while (current_block < block_length)
		{
			rfile	   *s = sourcemap[current_block];

			/* Extend range, if possible. */
			if (current_block + 1 < block_length &&
				s == sourcemap[current_block + 1])
			{
				++current_block;
				continue;
			}

			/* Add details about this range. */
			if (s == NULL)
			{
				if (current_block == start_of_range)
					appendStringInfo(&debug_buf, " %u:zero", current_block);
				else
					appendStringInfo(&debug_buf, " %u-%u:zero",
									 start_of_range, current_block);
			}
			else
			{
				if (current_block == start_of_range)
					appendStringInfo(&debug_buf, " %u:%s@" UINT64_FORMAT,
									 current_block, s->filename,
									 (uint64) offsetmap[current_block]);
				else
					appendStringInfo(&debug_buf, " %u-%u:%s@" UINT64_FORMAT,
									 start_of_range, current_block,
									 s->filename,
									 (uint64) offsetmap[current_block]);
			}

			/* Begin new range. */
			start_of_range = ++current_block;

			/* If the output is very long or we are done, dump it now. */
			if (current_block == block_length || debug_buf.len > 1024)
			{
				pg_log_debug("reconstruction plan:%s", debug_buf.data);
				resetStringInfo(&debug_buf);
			}
		}

		/* Free memory. */
		pfree(debug_buf.data);
	}

	/* Open the output file, except in dry_run mode. */
	if (!dry_run &&
		(wfd = open(output_filename,
					O_RDWR | PG_BINARY | O_CREAT | O_EXCL,
					pg_file_create_mode)) < 0)
		pg_fatal("could not open file \"%s\": %m", output_filename);

	/* Read and write the blocks as required. */
	for (i = 0; i < block_length; ++i)
	{
		uint8		buffer[BLCKSZ];
		rfile	   *s = sourcemap[i];

		/* Update accounting information. */
		if (s == NULL)
			++zero_blocks;
		else
		{
			s->num_blocks_read++;
			s->highest_offset_read = Max(s->highest_offset_read,
										 offsetmap[i] + BLCKSZ);
		}

		/* Skip the rest of this in dry-run mode. */
		if (dry_run)
			continue;

		/* Read or zero-fill the block as appropriate. */
		if (s == NULL)
		{
			/*
			 * New block not mentioned in the WAL summary. Should have been an
			 * uninitialized block, so just zero-fill it.
			 */
			memset(buffer, 0, BLCKSZ);

			/* Write out the block, update the checksum if needed. */
			write_block(wfd, output_filename, buffer, checksum_ctx);

			/* Nothing else to do for zero-filled blocks. */
			continue;
		}

		/* Copy the block using the appropriate copy method. */
		if (copy_method != COPY_METHOD_COPY_FILE_RANGE)
		{
			/*
			 * Read the block from the correct source file, and then write it
			 * out, possibly with a checksum update.
			 */
			read_block(s, offsetmap[i], buffer);
			write_block(wfd, output_filename, buffer, checksum_ctx);
		}
		else					/* use copy_file_range */
		{
#if defined(HAVE_COPY_FILE_RANGE)
			/* copy_file_range modifies the offset, so use a local copy */
			off_t		off = offsetmap[i];
			size_t		nwritten = 0;

			/*
			 * Retry until we've written all the bytes (the offset is updated
			 * by copy_file_range, and so is the wfd file offset).
			 */
			do
			{
				int			wb;

				wb = copy_file_range(s->fd, &off, wfd, NULL, BLCKSZ - nwritten, 0);

				if (wb < 0)
					pg_fatal("error while copying file range from \"%s\" to \"%s\": %m",
							 input_filename, output_filename);

				nwritten += wb;

			} while (BLCKSZ > nwritten);

			/*
			 * When checksum calculation not needed, we're done, otherwise
			 * read the block and pass it to the checksum calculation.
			 */
			if (checksum_ctx->type == CHECKSUM_TYPE_NONE)
				continue;

			read_block(s, offsetmap[i], buffer);

			if (pg_checksum_update(checksum_ctx, buffer, BLCKSZ) < 0)
				pg_fatal("could not update checksum of file \"%s\"",
						 output_filename);
#else
			pg_fatal("copy_file_range not supported on this platform");
#endif
		}
	}

	/* Debugging output. */
	if (zero_blocks > 0)
	{
		if (dry_run)
			pg_log_debug("would have zero-filled %u blocks", zero_blocks);
		else
			pg_log_debug("zero-filled %u blocks", zero_blocks);
	}

	/* Close the output file. */
	if (wfd >= 0 && close(wfd) != 0)
		pg_fatal("could not close file \"%s\": %m", output_filename);
}

/*
 * Write the block into the file (using the file descriptor), and
 * if needed update the checksum calculation.
 *
 * The buffer is expected to contain BLCKSZ bytes. The filename is
 * provided only for the error message.
 */
static void
write_block(int fd, char *output_filename,
			uint8 *buffer, pg_checksum_context *checksum_ctx)
{
	int			wb;

	if ((wb = write(fd, buffer, BLCKSZ)) != BLCKSZ)
	{
		if (wb < 0)
			pg_fatal("could not write file \"%s\": %m", output_filename);
		else
			pg_fatal("could not write file \"%s\": wrote %d of %d",
					 output_filename, wb, BLCKSZ);
	}

	/* Update the checksum computation. */
	if (pg_checksum_update(checksum_ctx, buffer, BLCKSZ) < 0)
		pg_fatal("could not update checksum of file \"%s\"",
				 output_filename);
}

/*
 * Read a block of data (BLCKSZ bytes) into the buffer.
 */
static void
read_block(rfile *s, off_t off, uint8 *buffer)
{
	int			rb;

	/* Read the block from the correct source, except if dry-run. */
	rb = pg_pread(s->fd, buffer, BLCKSZ, off);
	if (rb != BLCKSZ)
	{
		if (rb < 0)
			pg_fatal("could not read from file \"%s\": %m", s->filename);
		else
			pg_fatal("could not read from file \"%s\", offset %llu: read %d of %d",
					 s->filename, (unsigned long long) off, rb, BLCKSZ);
	}
}
