/*--------------------------------------------------------------------------
 *
 * test_custom_rmgrs.c
 *		Code for testing custom WAL resource managers.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_custom_rmgrs/test_custom_rmgrs.c
 *
 * Custom WAL resource manager for records containing a simple textual
 * payload, no-op redo, and no decoding.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "fmgr.h"
#include "utils/pg_lsn.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/*
 * test_custom_rmgrs WAL record message.
 */
typedef struct xl_testcustomrmgrs_message
{
	Size		message_size;	/* size of the message */
	char		message[FLEXIBLE_ARRAY_MEMBER]; /* payload */
} xl_testcustomrmgrs_message;

#define SizeOfTestCustomRmgrsMessage	(offsetof(xl_testcustomrmgrs_message, message))
#define XLOG_TEST_CUSTOM_RMGRS_MESSAGE	0x00

/*
 * While developing or testing, use RM_EXPERIMENTAL_ID for rmid. For a real
 * extension, reserve a new resource manager ID to avoid conflicting with
 * other extensions; see:
 * https://wiki.postgresql.org/wiki/CustomWALResourceManagers
 */
#define RM_TESTCUSTOMRMGRS_ID			RM_EXPERIMENTAL_ID
#define TESTCUSTOMRMGRS_NAME			"test_custom_rmgrs"

/* RMGR API, see xlog_internal.h */
void		testcustomrmgrs_redo(XLogReaderState *record);
void		testcustomrmgrs_desc(StringInfo buf, XLogReaderState *record);
const char *testcustomrmgrs_identify(uint8 info);

static const RmgrData testcustomrmgrs_rmgr = {
	.rm_name = TESTCUSTOMRMGRS_NAME,
	.rm_redo = testcustomrmgrs_redo,
	.rm_desc = testcustomrmgrs_desc,
	.rm_identify = testcustomrmgrs_identify
};

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our own custom resource manager, we have to be
	 * loaded via shared_preload_libraries. Otherwise, registration will fail.
	 */
	RegisterCustomRmgr(RM_TESTCUSTOMRMGRS_ID, &testcustomrmgrs_rmgr);
}

/* RMGR API implementation */

/*
 * Redo is just a noop for this module, because we aren't testing recovery of
 * any real structure.
 */
void
testcustomrmgrs_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info != XLOG_TEST_CUSTOM_RMGRS_MESSAGE)
		elog(PANIC, "testcustomrmgrs_redo: unknown op code %u", info);
}

void
testcustomrmgrs_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TEST_CUSTOM_RMGRS_MESSAGE)
	{
		xl_testcustomrmgrs_message *xlrec = (xl_testcustomrmgrs_message *) rec;

		appendStringInfo(buf, "payload (%zu bytes): ", xlrec->message_size);
		appendBinaryStringInfo(buf, xlrec->message, xlrec->message_size);
	}
}

const char *
testcustomrmgrs_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_TEST_CUSTOM_RMGRS_MESSAGE)
		return "TEST_CUSTOM_RMGRS_MESSAGE";

	return NULL;
}

/*
 * SQL function for writing a simple message into WAL with the help of custom
 * WAL resource manager.
 */
PG_FUNCTION_INFO_V1(test_custom_rmgrs_insert_wal_record);
Datum
test_custom_rmgrs_insert_wal_record(PG_FUNCTION_ARGS)
{
	text	   *arg = PG_GETARG_TEXT_PP(0);
	char	   *payload = VARDATA_ANY(arg);
	Size		len = VARSIZE_ANY_EXHDR(arg);
	XLogRecPtr	lsn;
	xl_testcustomrmgrs_message xlrec;

	xlrec.message_size = len;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfTestCustomRmgrsMessage);
	XLogRegisterData(payload, len);

	/* Let's mark this record as unimportant, just in case. */
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);

	lsn = XLogInsert(RM_TESTCUSTOMRMGRS_ID, XLOG_TEST_CUSTOM_RMGRS_MESSAGE);

	PG_RETURN_LSN(lsn);
}
