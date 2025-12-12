/*-------------------------------------------------------------------------
 *
 * test_toast_ext.c
 *		Test module for extended TOAST header structures (Phase 0)
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "access/detoast.h"
#include "access/toast_compression.h"
#include "utils/builtins.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/*
 * Test structure sizes for extended TOAST pointers
 */
PG_FUNCTION_INFO_V1(test_toast_structure_sizes);

Datum
test_toast_structure_sizes(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	bool		all_passed = true;

	initStringInfo(&buf);

	/* Test standard structure size */
	if (sizeof(varatt_external) != 16)
	{
		appendStringInfo(&buf, "FAIL: varatt_external is %zu bytes, expected 16\n",
						 sizeof(varatt_external));
		all_passed = false;
	}
	else
		appendStringInfo(&buf, "PASS: varatt_external is 16 bytes\n");

	/* Test extended structure size */
	if (sizeof(varatt_external_extended) != 20)
	{
		appendStringInfo(&buf, "FAIL: varatt_external_extended is %zu bytes, expected 20\n",
						 sizeof(varatt_external_extended));
		all_passed = false;
	}
	else
		appendStringInfo(&buf, "PASS: varatt_external_extended is 20 bytes\n");

	/* Test TOAST pointer sizes */
	if (TOAST_POINTER_SIZE != 18)
	{
		appendStringInfo(&buf, "FAIL: TOAST_POINTER_SIZE is %zu, expected 18\n",
						 (Size) TOAST_POINTER_SIZE);
		all_passed = false;
	}
	else
		appendStringInfo(&buf, "PASS: TOAST_POINTER_SIZE is 18 bytes\n");

	if (TOAST_POINTER_SIZE_EXTENDED != 22)
	{
		appendStringInfo(&buf, "FAIL: TOAST_POINTER_SIZE_EXTENDED is %zu, expected 22\n",
						 (Size) TOAST_POINTER_SIZE_EXTENDED);
		all_passed = false;
	}
	else
		appendStringInfo(&buf, "PASS: TOAST_POINTER_SIZE_EXTENDED is 22 bytes\n");

	/* Test field offsets */
	if (offsetof(varatt_external_extended, va_rawsize) != 0)
		appendStringInfo(&buf, "FAIL: va_rawsize offset\n"), all_passed = false;
	if (offsetof(varatt_external_extended, va_extinfo) != 4)
		appendStringInfo(&buf, "FAIL: va_extinfo offset\n"), all_passed = false;
	if (offsetof(varatt_external_extended, va_flags) != 8)
		appendStringInfo(&buf, "FAIL: va_flags offset\n"), all_passed = false;
	if (offsetof(varatt_external_extended, va_data) != 9)
		appendStringInfo(&buf, "FAIL: va_data offset\n"), all_passed = false;
	if (offsetof(varatt_external_extended, va_valueid) != 12)
		appendStringInfo(&buf, "FAIL: va_valueid offset\n"), all_passed = false;
	if (offsetof(varatt_external_extended, va_toastrelid) != 16)
		appendStringInfo(&buf, "FAIL: va_toastrelid offset\n"), all_passed = false;
	else
		appendStringInfo(&buf, "PASS: All field offsets correct (no padding)\n");

	if (all_passed)
		appendStringInfo(&buf, "\nResult: ALL TESTS PASSED\n");
	else
		appendStringInfo(&buf, "\nResult: SOME TESTS FAILED\n");

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * Test flag validation macros
 */
PG_FUNCTION_INFO_V1(test_toast_flag_validation);

Datum
test_toast_flag_validation(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	bool		all_passed = true;

	initStringInfo(&buf);

	/* Test valid flags */
	if (!ExtendedFlagsAreValid(0x00))
		appendStringInfo(&buf, "FAIL: flags 0x00 should be valid\n"), all_passed = false;
	if (!ExtendedFlagsAreValid(0x01))
		appendStringInfo(&buf, "FAIL: flags 0x01 should be valid\n"), all_passed = false;
	if (!ExtendedFlagsAreValid(0x02))
		appendStringInfo(&buf, "FAIL: flags 0x02 should be valid\n"), all_passed = false;
	if (!ExtendedFlagsAreValid(0x03))
		appendStringInfo(&buf, "FAIL: flags 0x03 should be valid\n"), all_passed = false;
	else
		appendStringInfo(&buf, "PASS: Valid flags (0x00-0x03) accepted\n");

	/* Test invalid flags */
	if (ExtendedFlagsAreValid(0x04))
		appendStringInfo(&buf, "FAIL: flags 0x04 should be invalid\n"), all_passed = false;
	if (ExtendedFlagsAreValid(0x08))
		appendStringInfo(&buf, "FAIL: flags 0x08 should be invalid\n"), all_passed = false;
	if (ExtendedFlagsAreValid(0xFF))
		appendStringInfo(&buf, "FAIL: flags 0xFF should be invalid\n"), all_passed = false;
	else
		appendStringInfo(&buf, "PASS: Invalid flags (0x04+) rejected\n");

	/* Test compression method validation */
	if (!ExtendedCompressionMethodIsValid(0))
		appendStringInfo(&buf, "FAIL: method 0 should be valid\n"), all_passed = false;
	if (!ExtendedCompressionMethodIsValid(255))
		appendStringInfo(&buf, "FAIL: method 255 should be valid\n"), all_passed = false;
	else
		appendStringInfo(&buf, "PASS: Compression methods 0-255 valid\n");

	/* Test compression method IDs */
	if (TOAST_PGLZ_EXT_METHOD != 0)
		appendStringInfo(&buf, "FAIL: TOAST_PGLZ_EXT_METHOD should be 0\n"), all_passed = false;
	if (TOAST_LZ4_EXT_METHOD != 1)
		appendStringInfo(&buf, "FAIL: TOAST_LZ4_EXT_METHOD should be 1\n"), all_passed = false;
	if (TOAST_ZSTD_EXT_METHOD != 2)
		appendStringInfo(&buf, "FAIL: TOAST_ZSTD_EXT_METHOD should be 2\n"), all_passed = false;
	if (TOAST_UNCOMPRESSED_EXT_METHOD != 3)
		appendStringInfo(&buf, "FAIL: TOAST_UNCOMPRESSED_EXT_METHOD should be 3\n"), all_passed = false;
	else
		appendStringInfo(&buf, "PASS: Compression method IDs correct\n");

	if (all_passed)
		appendStringInfo(&buf, "\nResult: ALL TESTS PASSED\n");
	else
		appendStringInfo(&buf, "\nResult: SOME TESTS FAILED\n");

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * Test compression ID constants
 */
PG_FUNCTION_INFO_V1(test_toast_compression_ids);

Datum
test_toast_compression_ids(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	bool		all_passed = true;

	initStringInfo(&buf);

	/* Standard compression IDs */
	if (TOAST_PGLZ_COMPRESSION_ID != 0)
		appendStringInfo(&buf, "FAIL: TOAST_PGLZ_COMPRESSION_ID != 0\n"), all_passed = false;
	if (TOAST_LZ4_COMPRESSION_ID != 1)
		appendStringInfo(&buf, "FAIL: TOAST_LZ4_COMPRESSION_ID != 1\n"), all_passed = false;
	if (TOAST_INVALID_COMPRESSION_ID != 2)
		appendStringInfo(&buf, "FAIL: TOAST_INVALID_COMPRESSION_ID != 2\n"), all_passed = false;
	if (TOAST_EXTENDED_COMPRESSION_ID != 3)
		appendStringInfo(&buf, "FAIL: TOAST_EXTENDED_COMPRESSION_ID != 3\n"), all_passed = false;
	else
		appendStringInfo(&buf, "PASS: Standard compression IDs correct (0,1,2,3)\n");

	/* Extended compression IDs match standard where applicable */
	if (TOAST_PGLZ_EXT_METHOD != TOAST_PGLZ_COMPRESSION_ID)
		appendStringInfo(&buf, "FAIL: PGLZ IDs don't match (standard=%d, extended=%d)\n",
						 TOAST_PGLZ_COMPRESSION_ID, TOAST_PGLZ_EXT_METHOD), all_passed = false;
	if (TOAST_LZ4_EXT_METHOD != TOAST_LZ4_COMPRESSION_ID)
		appendStringInfo(&buf, "FAIL: LZ4 IDs don't match (standard=%d, extended=%d)\n",
						 TOAST_LZ4_COMPRESSION_ID, TOAST_LZ4_EXT_METHOD), all_passed = false;
	else
		appendStringInfo(&buf, "PASS: PGLZ/LZ4 IDs consistent between formats\n");

	if (all_passed)
		appendStringInfo(&buf, "\nResult: ALL TESTS PASSED\n");
	else
		appendStringInfo(&buf, "\nResult: SOME TESTS FAILED\n");

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}
