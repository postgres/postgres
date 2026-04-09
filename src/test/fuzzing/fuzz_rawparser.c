/*-------------------------------------------------------------------------
 *
 * fuzz_rawparser.c
 *    Fuzzing harness for the PostgreSQL raw SQL parser
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_rawparser.c
 *
 * This harness feeds arbitrary byte sequences to raw_parser(), which
 * performs lexical and grammatical analysis of SQL statements.  It
 * performs minimal backend initialization (just the memory-context
 * subsystem) and catches all parser errors via PG_TRY/PG_CATCH.
 *
 * The harness links against postgres_lib using archive semantics.
 * It provides stub definitions for symbols normally supplied by
 * main/main.c (progname, parse_dispatch_option) so that the linker
 * does not pull in main.o and conflict with the harness's own main().
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parser.h"
#include "postmaster/postmaster.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

/*
 * Stub definitions for symbols that main/main.c normally provides.
 * By defining them here we prevent the archive linker from pulling in
 * main.o (which defines its own main()).
 */
const char *progname = "fuzz_rawparser";

DispatchOption
parse_dispatch_option(const char *name)
{
	return DISPATCH_POSTMASTER;
}

static bool initialized = false;

static void fuzz_initialize(void);

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * One-time initialization: set up memory contexts and encoding.
 */
static void
fuzz_initialize(void)
{
	MemoryContextInit();
	SetDatabaseEncoding(PG_UTF8);
	SetMessageEncoding(PG_UTF8);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char	   *str;
	MemoryContext fuzz_context;
	MemoryContext oldcontext;

	if (!initialized)
	{
		fuzz_initialize();
		initialized = true;
	}

	if (size == 0)
		return 0;

	/*
	 * Create a temporary memory context for each parse attempt so that all
	 * allocations made by the parser are freed afterwards.
	 */
	fuzz_context = AllocSetContextCreate(TopMemoryContext,
										 "Fuzz Context",
										 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(fuzz_context);

	/* raw_parser() expects a NUL-terminated string */
	str = palloc(size + 1);
	memcpy(str, data, size);
	str[size] = '\0';

	PG_TRY();
	{
		(void) raw_parser(str, RAW_PARSE_DEFAULT);
	}
	PG_CATCH();
	{
		FlushErrorState();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(fuzz_context);

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
