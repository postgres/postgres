/*-------------------------------------------------------------------------
 *
 * compress_none.c
 *	 Routines for archivers to read or write an uncompressed stream.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_none.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include <unistd.h>

#include "compress_none.h"
#include "pg_backup_utils.h"

/*----------------------
 * Compressor API
 *----------------------
 */

/*
 * We buffer outgoing data, just to ensure that data blocks written to the
 * archive file are of reasonable size.  The read side could use this struct,
 * but there's no need because it does not retain data across calls.
 */
typedef struct NoneCompressorState
{
	char	   *buffer;			/* buffer for unwritten data */
	size_t		buflen;			/* allocated size of buffer */
	size_t		bufdata;		/* amount of valid data currently in buffer */
} NoneCompressorState;

/*
 * Private routines
 */

static void
ReadDataFromArchiveNone(ArchiveHandle *AH, CompressorState *cs)
{
	size_t		cnt;
	char	   *buf;
	size_t		buflen;

	buflen = DEFAULT_IO_BUFFER_SIZE;
	buf = pg_malloc(buflen);

	while ((cnt = cs->readF(AH, &buf, &buflen)))
	{
		ahwrite(buf, 1, cnt, AH);
	}

	free(buf);
}


static void
WriteDataToArchiveNone(ArchiveHandle *AH, CompressorState *cs,
					   const void *data, size_t dLen)
{
	NoneCompressorState *nonecs = (NoneCompressorState *) cs->private_data;
	size_t		remaining = dLen;

	while (remaining > 0)
	{
		size_t		chunk;

		/* Dump buffer if full */
		if (nonecs->bufdata >= nonecs->buflen)
		{
			cs->writeF(AH, nonecs->buffer, nonecs->bufdata);
			nonecs->bufdata = 0;
		}
		/* And fill it */
		chunk = nonecs->buflen - nonecs->bufdata;
		if (chunk > remaining)
			chunk = remaining;
		memcpy(nonecs->buffer + nonecs->bufdata, data, chunk);
		nonecs->bufdata += chunk;
		data = ((const char *) data) + chunk;
		remaining -= chunk;
	}
}

static void
EndCompressorNone(ArchiveHandle *AH, CompressorState *cs)
{
	NoneCompressorState *nonecs = (NoneCompressorState *) cs->private_data;

	if (nonecs)
	{
		/* Dump buffer if nonempty */
		if (nonecs->bufdata > 0)
			cs->writeF(AH, nonecs->buffer, nonecs->bufdata);
		/* Free working state */
		pg_free(nonecs->buffer);
		pg_free(nonecs);
		cs->private_data = NULL;
	}
}

/*
 * Public interface
 */

void
InitCompressorNone(CompressorState *cs,
				   const pg_compress_specification compression_spec)
{
	cs->readData = ReadDataFromArchiveNone;
	cs->writeData = WriteDataToArchiveNone;
	cs->end = EndCompressorNone;

	cs->compression_spec = compression_spec;

	/*
	 * If the caller has defined a write function, prepare the necessary
	 * buffer.
	 */
	if (cs->writeF)
	{
		NoneCompressorState *nonecs;

		nonecs = pg_malloc_object(NoneCompressorState);
		nonecs->buflen = DEFAULT_IO_BUFFER_SIZE;
		nonecs->buffer = pg_malloc(nonecs->buflen);
		nonecs->bufdata = 0;

		cs->private_data = nonecs;
	}
}


/*----------------------
 * Compress File API
 *----------------------
 */

/*
 * Private routines
 */

static size_t
read_none(void *ptr, size_t size, CompressFileHandle *CFH)
{
	FILE	   *fp = (FILE *) CFH->private_data;
	size_t		ret;

	ret = fread(ptr, 1, size, fp);
	if (ferror(fp))
		pg_fatal("could not read from input file: %m");

	return ret;
}

static void
write_none(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	size_t		ret;

	errno = 0;
	ret = fwrite(ptr, 1, size, (FILE *) CFH->private_data);
	if (ret != size)
	{
		errno = (errno) ? errno : ENOSPC;
		pg_fatal("could not write to file: %m");
	}
}

static const char *
get_error_none(CompressFileHandle *CFH)
{
	return strerror(errno);
}

static char *
gets_none(char *ptr, int size, CompressFileHandle *CFH)
{
	return fgets(ptr, size, (FILE *) CFH->private_data);
}

static int
getc_none(CompressFileHandle *CFH)
{
	FILE	   *fp = (FILE *) CFH->private_data;
	int			ret;

	ret = fgetc(fp);
	if (ret == EOF)
	{
		if (!feof(fp))
			pg_fatal("could not read from input file: %m");
		else
			pg_fatal("could not read from input file: end of file");
	}

	return ret;
}

static bool
close_none(CompressFileHandle *CFH)
{
	FILE	   *fp = (FILE *) CFH->private_data;
	int			ret = 0;

	CFH->private_data = NULL;

	if (fp)
	{
		errno = 0;
		ret = fclose(fp);
		if (ret != 0)
			pg_log_error("could not close file: %m");
	}

	return ret == 0;
}

static bool
eof_none(CompressFileHandle *CFH)
{
	return feof((FILE *) CFH->private_data) != 0;
}

static bool
open_none(const char *path, int fd, const char *mode, CompressFileHandle *CFH)
{
	Assert(CFH->private_data == NULL);

	if (fd >= 0)
		CFH->private_data = fdopen(dup(fd), mode);
	else
		CFH->private_data = fopen(path, mode);

	if (CFH->private_data == NULL)
		return false;

	return true;
}

static bool
open_write_none(const char *path, const char *mode, CompressFileHandle *CFH)
{
	Assert(CFH->private_data == NULL);

	CFH->private_data = fopen(path, mode);
	if (CFH->private_data == NULL)
		return false;

	return true;
}

/*
 * Public interface
 */

void
InitCompressFileHandleNone(CompressFileHandle *CFH,
						   const pg_compress_specification compression_spec)
{
	CFH->open_func = open_none;
	CFH->open_write_func = open_write_none;
	CFH->read_func = read_none;
	CFH->write_func = write_none;
	CFH->gets_func = gets_none;
	CFH->getc_func = getc_none;
	CFH->close_func = close_none;
	CFH->eof_func = eof_none;
	CFH->get_error_func = get_error_none;

	CFH->private_data = NULL;
}
