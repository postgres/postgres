/*-------------------------------------------------------------------------
 *
 * fuzz_pglz.c
 *    Fuzzing harness for the PostgreSQL LZ decompressor
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_pglz.c
 *
 * This harness feeds arbitrary byte sequences to pglz_decompress(),
 * which decompresses PostgreSQL's native LZ-compressed data.  The
 * decompressor is a pure function with no global state, making it
 * ideal for fuzzing.
 *
 * The first 4 bytes of the fuzzer input are interpreted as the
 * claimed raw (uncompressed) size in little-endian byte order,
 * capped at 1 MB.  The remaining bytes are the compressed payload.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>
#include <string.h>

#include "common/pg_lzcompress.h"

#define MAX_RAW_SIZE (1024 * 1024)

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	int32		rawsize;
	char	   *dest;

	/* Need at least 4 bytes for the raw size, plus some compressed data */
	if (size < 5)
		return 0;

	/* Extract claimed raw size from first 4 bytes (little-endian) */
	rawsize = (int32) data[0] |
		((int32) data[1] << 8) |
		((int32) data[2] << 16) |
		((int32) data[3] << 24);

	/* Reject nonsensical sizes */
	if (rawsize <= 0 || rawsize > MAX_RAW_SIZE)
		return 0;

	dest = malloc(rawsize);
	if (!dest)
		return 0;

	/* Try decompression with completeness check */
	(void) pglz_decompress((const char *) data + 4,
						   (int32) (size - 4),
						   dest,
						   rawsize,
						   true);

	/* Also try without completeness check to exercise that path */
	(void) pglz_decompress((const char *) data + 4,
						   (int32) (size - 4),
						   dest,
						   rawsize,
						   false);

	free(dest);
	return 0;
}

#ifdef STANDALONE_FUZZ_TARGET
int
main(int argc, char **argv)
{
	int			i;
	int			ret = 0;

	for (i = 1; i < argc; i++)
	{
		FILE	   *f = fopen(argv[i], "rb");
		long		len;
		uint8_t    *buf;
		size_t		n_read;

		if (!f)
		{
			fprintf(stderr, "%s: could not open %s: %m\n", argv[0], argv[i]);
			ret = 1;
			continue;
		}

		fseek(f, 0, SEEK_END);
		len = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (len < 0)
		{
			fprintf(stderr, "%s: could not determine size of %s\n",
					argv[0], argv[i]);
			fclose(f);
			ret = 1;
			continue;
		}

		buf = malloc(len);
		if (!buf)
		{
			fprintf(stderr, "%s: out of memory\n", argv[0]);
			fclose(f);
			return 1;
		}

		n_read = fread(buf, 1, len, f);
		fclose(f);

		LLVMFuzzerTestOneInput(buf, n_read);
		free(buf);
	}

	return ret;
}
#endif							/* STANDALONE_FUZZ_TARGET */
