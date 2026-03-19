/*--------------------------------------------------------------------------
 *
 * test_saslprep.c
 *		Test harness for the SASLprep implementation.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_saslprep/test_saslprep.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "common/saslprep.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

static const char *
saslprep_status_to_text(pg_saslprep_rc rc)
{
	const char *status = "???";

	switch (rc)
	{
		case SASLPREP_OOM:
			status = "OOM";
			break;
		case SASLPREP_SUCCESS:
			status = "SUCCESS";
			break;
		case SASLPREP_INVALID_UTF8:
			status = "INVALID_UTF8";
			break;
		case SASLPREP_PROHIBITED:
			status = "PROHIBITED";
			break;
	}

	return status;
}

/*
 * Simple function to test SASLprep with arbitrary bytes as input.
 *
 * This takes a bytea in input, returning in output the generating data as
 * bytea with the status returned by pg_saslprep().
 */
PG_FUNCTION_INFO_V1(test_saslprep);
Datum
test_saslprep(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	char	   *src;
	Size		src_len;
	char	   *input_data;
	char	   *result;
	Size		result_len;
	bytea	   *result_bytea = NULL;
	const char *status = NULL;
	Datum	   *values;
	bool	   *nulls;
	TupleDesc	tupdesc;
	pg_saslprep_rc rc;

	/* determine result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values = palloc0_array(Datum, tupdesc->natts);
	nulls = palloc0_array(bool, tupdesc->natts);

	src_len = VARSIZE_ANY_EXHDR(string);
	src = VARDATA_ANY(string);

	/*
	 * Copy the input given, to make SASLprep() act on a sanitized string.
	 */
	input_data = palloc0(src_len + 1);
	strlcpy(input_data, src, src_len + 1);

	rc = pg_saslprep(input_data, &result);
	status = saslprep_status_to_text(rc);

	if (result)
	{
		result_len = strlen(result);
		result_bytea = palloc(result_len + VARHDRSZ);
		SET_VARSIZE(result_bytea, result_len + VARHDRSZ);
		memcpy(VARDATA(result_bytea), result, result_len);
		values[0] = PointerGetDatum(result_bytea);
	}
	else
		nulls[0] = true;

	values[1] = CStringGetTextDatum(status);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* Context structure for set-returning function with ranges */
typedef struct
{
	int			current_range;
	char32_t	current_codepoint;
} pg_saslprep_test_context;

/*
 * UTF-8 code point ranges.
 */
typedef struct
{
	char32_t	start_codepoint;
	char32_t	end_codepoint;
} pg_utf8_codepoint_range;

static const pg_utf8_codepoint_range pg_utf8_test_ranges[] = {
	/* 1, 2, 3 bytes */
	{0x0000, 0xD7FF},			/* Basic Multilingual Plane, before surrogates */
	{0xE000, 0xFFFF},			/* Basic Multilingual Plane, after surrogates */
	/* 4 bytes */
	{0x10000, 0x1FFFF},			/* Supplementary Multilingual Plane */
	{0x20000, 0x2FFFF},			/* Supplementary Ideographic Plane */
	{0x30000, 0x3FFFF},			/* Tertiary Ideographic Plane */
	{0x40000, 0xDFFFF},			/* Unassigned planes */
	{0xE0000, 0xEFFFF},			/* Supplementary Special-purpose Plane */
	{0xF0000, 0xFFFFF},			/* Private Use Area A */
	{0x100000, 0x10FFFF},		/* Private Use Area B */
};

#define PG_UTF8_TEST_RANGES_LEN \
	(sizeof(pg_utf8_test_ranges) / sizeof(pg_utf8_test_ranges[0]))


/*
 * test_saslprep_ranges
 *
 * Test SASLprep across various UTF-8 ranges.
 */
PG_FUNCTION_INFO_V1(test_saslprep_ranges);
Datum
test_saslprep_ranges(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	pg_saslprep_test_context *ctx;
	HeapTuple	tuple;
	Datum		result;

	/* First call setup */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = tupdesc;

		/* Allocate context with range setup */
		ctx = (pg_saslprep_test_context *) palloc(sizeof(pg_saslprep_test_context));
		ctx->current_range = 0;
		ctx->current_codepoint = pg_utf8_test_ranges[0].start_codepoint;
		funcctx->user_fctx = ctx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	ctx = (pg_saslprep_test_context *) funcctx->user_fctx;

	while (ctx->current_range < PG_UTF8_TEST_RANGES_LEN)
	{
		char32_t	codepoint = ctx->current_codepoint;
		unsigned char utf8_buf[5];
		char		input_str[6];
		char	   *output = NULL;
		pg_saslprep_rc rc;
		int			utf8_len;
		const char *status;
		bytea	   *input_bytea;
		bytea	   *output_bytea;
		char		codepoint_str[16];
		Datum		values[4] = {0};
		bool		nulls[4] = {0};
		const pg_utf8_codepoint_range *range =
			&pg_utf8_test_ranges[ctx->current_range];

		CHECK_FOR_INTERRUPTS();

		/* Switch to next range if finished with the previous one */
		if (ctx->current_codepoint > range->end_codepoint)
		{
			ctx->current_range++;
			if (ctx->current_range < PG_UTF8_TEST_RANGES_LEN)
				ctx->current_codepoint =
					pg_utf8_test_ranges[ctx->current_range].start_codepoint;
			continue;
		}

		codepoint = ctx->current_codepoint;

		/* Convert code point to UTF-8 */
		utf8_len = unicode_utf8len(codepoint);
		if (utf8_len == 0)
		{
			ctx->current_codepoint++;
			continue;
		}
		unicode_to_utf8(codepoint, utf8_buf);

		/* Create null-terminated string */
		memcpy(input_str, utf8_buf, utf8_len);
		input_str[utf8_len] = '\0';

		/* Test with pg_saslprep */
		rc = pg_saslprep(input_str, &output);

		/* Prepare output values */
		memset(nulls, false, sizeof(nulls));

		/* codepoint as text U+XXXX format */
		if (codepoint <= 0xFFFF)
			snprintf(codepoint_str, sizeof(codepoint_str), "U+%04X", codepoint);
		else
			snprintf(codepoint_str, sizeof(codepoint_str), "U+%06X", codepoint);
		values[0] = CStringGetTextDatum(codepoint_str);

		/* status */
		status = saslprep_status_to_text(rc);
		values[1] = CStringGetTextDatum(status);

		/* input_bytes */
		input_bytea = (bytea *) palloc(VARHDRSZ + utf8_len);
		SET_VARSIZE(input_bytea, VARHDRSZ + utf8_len);
		memcpy(VARDATA(input_bytea), utf8_buf, utf8_len);
		values[2] = PointerGetDatum(input_bytea);

		/* output_bytes */
		if (output != NULL)
		{
			int			output_len = strlen(output);

			output_bytea = (bytea *) palloc(VARHDRSZ + output_len);
			SET_VARSIZE(output_bytea, VARHDRSZ + output_len);
			memcpy(VARDATA(output_bytea), output, output_len);
			values[3] = PointerGetDatum(output_bytea);
			pfree(output);
		}
		else
		{
			nulls[3] = true;
			values[3] = (Datum) 0;
		}

		/* Build and return tuple */
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		/* Move to next code point */
		ctx->current_codepoint++;

		SRF_RETURN_NEXT(funcctx, result);
	}

	/* All done */
	SRF_RETURN_DONE(funcctx);
}
