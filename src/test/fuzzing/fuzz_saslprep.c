/*-------------------------------------------------------------------------
 *
 * fuzz_saslprep.c
 *    Fuzzing harness for pg_saslprep()
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_saslprep.c
 *
 * This harness feeds arbitrary byte sequences to pg_saslprep(),
 * which performs SASLprep normalization (RFC 4013) on UTF-8 strings.
 * This involves Unicode NFKC normalization, character mapping, and
 * prohibited character detection.  The function is standalone and
 * requires no database connection.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>

#include "common/saslprep.h"

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char	   *str;
	char	   *output = NULL;

	if (size == 0)
		return 0;

	/* pg_saslprep expects a NUL-terminated string */
	str = malloc(size + 1);
	if (!str)
		return 0;
	memcpy(str, data, size);
	str[size] = '\0';

	(void) pg_saslprep(str, &output);

	if (output)
		free(output);

	free(str);
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
