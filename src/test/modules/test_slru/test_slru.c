/*--------------------------------------------------------------------------
 *
 * test_slru.c
 *		Test correctness of SLRU functions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_slru/test_slru.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/slru.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/*
 * SQL-callable entry points
 */
PG_FUNCTION_INFO_V1(test_slru_page_write);
PG_FUNCTION_INFO_V1(test_slru_page_writeall);
PG_FUNCTION_INFO_V1(test_slru_page_read);
PG_FUNCTION_INFO_V1(test_slru_page_readonly);
PG_FUNCTION_INFO_V1(test_slru_page_exists);
PG_FUNCTION_INFO_V1(test_slru_page_sync);
PG_FUNCTION_INFO_V1(test_slru_page_delete);
PG_FUNCTION_INFO_V1(test_slru_page_truncate);
PG_FUNCTION_INFO_V1(test_slru_delete_all);

/* Number of SLRU page slots */
#define NUM_TEST_BUFFERS		16

/* SLRU control lock */
LWLock		TestSLRULock;
#define TestSLRULock (&TestSLRULock)

static SlruCtlData TestSlruCtlData;
#define TestSlruCtl			(&TestSlruCtlData)

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* LWLock name */
const char	test_tranche_name[] = "test_slru_tranche";

static bool
test_slru_scan_cb(SlruCtl ctl, char *filename, int segpage, void *data)
{
	elog(NOTICE, "Calling test_slru_scan_cb()");
	return SlruScanDirCbDeleteAll(ctl, filename, segpage, data);
}

Datum
test_slru_page_write(PG_FUNCTION_ARGS)
{
	int			pageno = PG_GETARG_INT32(0);
	char	   *data = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int			slotno;

	LWLockAcquire(TestSLRULock, LW_EXCLUSIVE);

	slotno = SimpleLruZeroPage(TestSlruCtl, pageno);

	/* these should match */
	Assert(TestSlruCtl->shared->page_number[slotno] == pageno);

	/* mark the page as dirty so as it would get written */
	TestSlruCtl->shared->page_dirty[slotno] = true;
	TestSlruCtl->shared->page_status[slotno] = SLRU_PAGE_VALID;

	/* write given data to the page, up to the limit of the page */
	strncpy(TestSlruCtl->shared->page_buffer[slotno], data,
			BLCKSZ - 1);

	SimpleLruWritePage(TestSlruCtl, slotno);
	LWLockRelease(TestSLRULock);

	PG_RETURN_VOID();
}

Datum
test_slru_page_writeall(PG_FUNCTION_ARGS)
{
	SimpleLruWriteAll(TestSlruCtl, true);
	PG_RETURN_VOID();
}

Datum
test_slru_page_read(PG_FUNCTION_ARGS)
{
	int			pageno = PG_GETARG_INT32(0);
	bool		write_ok = PG_GETARG_BOOL(1);
	char	   *data = NULL;
	int			slotno;

	/* find page in buffers, reading it if necessary */
	LWLockAcquire(TestSLRULock, LW_EXCLUSIVE);
	slotno = SimpleLruReadPage(TestSlruCtl, pageno,
							   write_ok, InvalidTransactionId);
	data = (char *) TestSlruCtl->shared->page_buffer[slotno];
	LWLockRelease(TestSLRULock);

	PG_RETURN_TEXT_P(cstring_to_text(data));
}

Datum
test_slru_page_readonly(PG_FUNCTION_ARGS)
{
	int			pageno = PG_GETARG_INT32(0);
	char	   *data = NULL;
	int			slotno;

	/* find page in buffers, reading it if necessary */
	slotno = SimpleLruReadPage_ReadOnly(TestSlruCtl,
										pageno,
										InvalidTransactionId);
	Assert(LWLockHeldByMe(TestSLRULock));
	data = (char *) TestSlruCtl->shared->page_buffer[slotno];
	LWLockRelease(TestSLRULock);

	PG_RETURN_TEXT_P(cstring_to_text(data));
}

Datum
test_slru_page_exists(PG_FUNCTION_ARGS)
{
	int			pageno = PG_GETARG_INT32(0);
	bool		found;

	LWLockAcquire(TestSLRULock, LW_EXCLUSIVE);
	found = SimpleLruDoesPhysicalPageExist(TestSlruCtl, pageno);
	LWLockRelease(TestSLRULock);

	PG_RETURN_BOOL(found);
}

Datum
test_slru_page_sync(PG_FUNCTION_ARGS)
{
	int			pageno = PG_GETARG_INT32(0);
	FileTag		ftag;
	char		path[MAXPGPATH];

	/* note that this flushes the full file a segment is located in */
	ftag.segno = pageno / SLRU_PAGES_PER_SEGMENT;
	SlruSyncFileTag(TestSlruCtl, &ftag, path);

	elog(NOTICE, "Called SlruSyncFileTag() for segment %d on path %s",
		 ftag.segno, path);

	PG_RETURN_VOID();
}

Datum
test_slru_page_delete(PG_FUNCTION_ARGS)
{
	int			pageno = PG_GETARG_INT32(0);
	FileTag		ftag;

	ftag.segno = pageno / SLRU_PAGES_PER_SEGMENT;
	SlruDeleteSegment(TestSlruCtl, ftag.segno);

	elog(NOTICE, "Called SlruDeleteSegment() for segment %d", ftag.segno);

	PG_RETURN_VOID();
}

Datum
test_slru_page_truncate(PG_FUNCTION_ARGS)
{
	int			pageno = PG_GETARG_INT32(0);

	SimpleLruTruncate(TestSlruCtl, pageno);
	PG_RETURN_VOID();
}

Datum
test_slru_delete_all(PG_FUNCTION_ARGS)
{
	/* this calls SlruScanDirCbDeleteAll() internally, ensuring deletion */
	SlruScanDirectory(TestSlruCtl, test_slru_scan_cb, NULL);

	PG_RETURN_VOID();
}

/*
 * Module load callbacks and initialization.
 */

static void
test_slru_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	/* reserve shared memory for the test SLRU */
	RequestAddinShmemSpace(SimpleLruShmemSize(NUM_TEST_BUFFERS, 0));
}

static bool
test_slru_page_precedes_logically(int page1, int page2)
{
	return page1 < page2;
}

static void
test_slru_shmem_startup(void)
{
	const char	slru_dir_name[] = "pg_test_slru";
	int			test_tranche_id;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/*
	 * Create the SLRU directory if it does not exist yet, from the root of
	 * the data directory.
	 */
	(void) MakePGDirectory(slru_dir_name);

	/* initialize the SLRU facility */
	test_tranche_id = LWLockNewTrancheId();
	LWLockRegisterTranche(test_tranche_id, "test_slru_tranche");
	LWLockInitialize(TestSLRULock, test_tranche_id);

	TestSlruCtl->PagePrecedes = test_slru_page_precedes_logically;
	SimpleLruInit(TestSlruCtl, "TestSLRU",
				  NUM_TEST_BUFFERS, 0, TestSLRULock, slru_dir_name,
				  test_tranche_id, SYNC_HANDLER_NONE);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("cannot load \"%s\" after startup", "test_slru"),
				 errdetail("\"%s\" must be loaded with shared_preload_libraries.",
						   "test_slru")));

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_slru_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_slru_shmem_startup;
}
