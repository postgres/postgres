/*-------------------------------------------------------------------------
 *
 * fuzz_typeinput.c
 *    Fuzzing harness for PostgreSQL type input functions
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_typeinput.c
 *
 * This harness feeds arbitrary byte sequences to the backend's type
 * input functions: numeric_in, date_in, timestamp_in, timestamptz_in,
 * and interval_in.  These functions parse textual representations of
 * data types and are a key part of PostgreSQL's input validation.
 *
 * The first byte of input selects which type parser to call; the
 * remaining bytes are the type-input string.  All functions support
 * soft error handling via ErrorSaveContext, so errors are caught
 * without ereport/PG_TRY.  PG_TRY/PG_CATCH is used as a safety net
 * for any unexpected hard errors.
 *
 * The harness links against postgres_lib using archive semantics.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>

#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "pgtime.h"
#include "postmaster/postmaster.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/palloc.h"
#include "utils/timestamp.h"

/* Stubs for symbols from main/main.c */
const char *progname = "fuzz_typeinput";

DispatchOption
parse_dispatch_option(const char *name)
{
	return DISPATCH_POSTMASTER;
}

/* Type selector values */
#define FUZZ_NUMERIC	0
#define FUZZ_DATE		1
#define FUZZ_TIMESTAMP	2
#define FUZZ_TIMESTAMPTZ 3
#define FUZZ_INTERVAL	4
#define FUZZ_NTYPES		5

static bool initialized = false;

static void fuzz_initialize(void);

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
fuzz_initialize(void)
{
	MemoryContextInit();
	SetDatabaseEncoding(PG_UTF8);
	SetMessageEncoding(PG_UTF8);

	/*
	 * Initialize timezone subsystem.  Use "GMT" because it is resolved
	 * without filesystem access (the timezone data directory may not exist in
	 * a fuzzing build).
	 */
	pg_timezone_initialize();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char	   *str;
	int			type_sel;

	LOCAL_FCINFO(fcinfo, 3);
	ErrorSaveContext escontext;
	MemoryContext fuzz_context;
	MemoryContext oldcontext;

	if (!initialized)
	{
		fuzz_initialize();
		initialized = true;
	}

	/* Need at least type selector + 1 byte of input */
	if (size < 2)
		return 0;

	type_sel = data[0] % FUZZ_NTYPES;
	data++;
	size--;

	fuzz_context = AllocSetContextCreate(TopMemoryContext,
										 "Fuzz Context",
										 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(fuzz_context);

	/* Build a NUL-terminated string from the input */
	str = palloc(size + 1);
	memcpy(str, data, size);
	str[size] = '\0';

	/* Set up ErrorSaveContext for soft error handling */
	memset(&escontext, 0, sizeof(escontext));
	escontext.type = T_ErrorSaveContext;
	escontext.error_occurred = false;
	escontext.details_wanted = false;

	/* Set up FunctionCallInfo */
	memset(fcinfo, 0, SizeForFunctionCallInfo(3));
	fcinfo->nargs = 3;
	fcinfo->args[0].value = CStringGetDatum(str);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(InvalidOid);	/* typelem */
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(-1);	/* typmod */
	fcinfo->args[2].isnull = false;
	fcinfo->context = (Node *) &escontext;

	PG_TRY();
	{
		switch (type_sel)
		{
			case FUZZ_NUMERIC:
				(void) numeric_in(fcinfo);
				break;
			case FUZZ_DATE:
				(void) date_in(fcinfo);
				break;
			case FUZZ_TIMESTAMP:
				(void) timestamp_in(fcinfo);
				break;
			case FUZZ_TIMESTAMPTZ:
				(void) timestamptz_in(fcinfo);
				break;
			case FUZZ_INTERVAL:
				(void) interval_in(fcinfo);
				break;
		}
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
