/*-------------------------------------------------------------------------
1 *
 * zenith_nonrelxlogreader.c
 *		Read WAL in nonrelwal format
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/zenith_nonrelxlogreader.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogreader.h"


/*
 * On first call, we scan pg_wal and collect information about all non-rel WAL
 * files in 'nonrelwal_files' list. It is sorted by 'startptr'.
 */
typedef struct
{
	const char *filename;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
} nonrelwal_file_info;

static List *nonrelwal_files;
static bool nonrelwal_scanned = false;

/*
 * Currently open non-rel WAL file and position within it.
 */
static int	current_nonrelwal_idx;
static FILE *current_nonrelwal_fp;
static XLogRecPtr current_recptr;

static void scan_nonrelwal_files(void);
static int nonrelwal_entry_cmp(const ListCell *a, const ListCell *b);
static bool parse_nonrelwal_filename(const char *path, XLogRecPtr *startptr, XLogRecPtr *endptr);

static void
fread_noerr(void *buf, size_t size, FILE *fp)
{
	size_t		nread;

	nread = fread(buf, 1, size, fp);
	if (nread != size)
		elog(ERROR, "error reading");
}

/*
 * Read next record from currently open non-rel WAL file.
 */
static bool
read_next_record(XLogReaderState *xlogreader, XLogRecPtr *recstartptr, XLogRecPtr *recendptr)
{
	uint32		xl_tot_len;
	size_t		nread;

	nread = fread(recstartptr, 1, sizeof(XLogRecPtr), current_nonrelwal_fp);
	if (nread == 0)
		return false;
	if (nread != sizeof(XLogRecPtr))
		elog(ERROR, "error reading"); /* FIXME: proper error message */

	/* on entry, we are positioned between the startptr and endptr of a record. */
	fread_noerr(recendptr, sizeof(XLogRecPtr), current_nonrelwal_fp);
	fread_noerr(&xl_tot_len, sizeof(uint32), current_nonrelwal_fp);
	if (xlogreader->readRecordBufSize < xl_tot_len)
	{
		pfree(xlogreader->readRecordBuf);
		xlogreader->readRecordBuf = palloc(xl_tot_len);
		xlogreader->readRecordBufSize = xl_tot_len;
	}
	memcpy(xlogreader->readRecordBuf, &xl_tot_len, sizeof(uint32));
	fread_noerr(xlogreader->readRecordBuf + sizeof(uint32),
				xl_tot_len - sizeof(uint32),
				current_nonrelwal_fp);

	return true;
}

/*
 * Try to read a record from a non-rel WAL file.
 */
XLogRecord *
nonrelwal_read_record(XLogReaderState *xlogreader, int emode,
					  bool fetching_ckpt)
{
	XLogRecPtr	recptr = xlogreader->EndRecPtr;
	int			nfiles;
	nonrelwal_file_info *e;

	/*
	 * Scan the pg_wal directory for non-rel WAL files on first call.
	 */
	if (!nonrelwal_scanned)
	{
		scan_nonrelwal_files();
		nonrelwal_scanned = true;
	}

	nfiles = list_length(nonrelwal_files);
	if (nfiles == 0)
		return NULL;
	e = list_nth(nonrelwal_files, current_nonrelwal_idx);

	/*
	 * Try to open a non-rel WAL file containing 'recptr', if it's not
	 * open already.
	 */
	if (current_nonrelwal_fp == NULL ||
		current_recptr == InvalidXLogRecPtr ||
		current_recptr > recptr ||
		e->startptr > recptr ||
		recptr >= e->endptr)
	{
		char		path[MAXPGPATH];

		if (current_nonrelwal_fp)
		{
			FreeFile(current_nonrelwal_fp);
			current_nonrelwal_fp = NULL;
		}

		/* Find the right file in the list of files. */
		while (e->startptr > recptr && current_nonrelwal_idx > 0)
		{
			current_nonrelwal_idx--;
			e = list_nth(nonrelwal_files, current_nonrelwal_idx);
		}

		while (recptr >= e->endptr && current_nonrelwal_idx < nfiles - 1)
		{
			current_nonrelwal_idx++;
			e = list_nth(nonrelwal_files, current_nonrelwal_idx);
		}

		/* We should now be positioned at the right file, if any */
		if (recptr < e->startptr ||
			recptr >= e->endptr)
		{
			elog(LOG, "out of non-rel WAL");
			return NULL;
		}

		/* Open this file */
		snprintf(path, sizeof(path), "pg_wal/nonrelwal/%s", e->filename);
		current_nonrelwal_fp = AllocateFile(path, PG_BINARY_R);
		if (current_nonrelwal_fp == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m",
							path)));
	}

	/* Seek within the file */
	for (;;)
	{
		XLogRecPtr recstartptr;
		XLogRecPtr recendptr;
		XLogRecord *record;
		pg_crc32c	crc;

		if (!read_next_record(xlogreader, &recstartptr, &recendptr))
			return NULL;

		record = (XLogRecord *) xlogreader->readRecordBuf;
		INIT_CRC32C(crc);
		COMP_CRC32C(crc, ((char *) record) + SizeOfXLogRecord, record->xl_tot_len - SizeOfXLogRecord);
		COMP_CRC32C(crc, (char *) record, offsetof(XLogRecord, xl_crc));
		FIN_CRC32C(crc);
		if (crc != record->xl_crc)
			elog(ERROR, "CRC mismatch");

		if (recstartptr == recptr || (!fetching_ckpt && recstartptr > recptr))
		{
			char	   *errormsg;

			current_recptr = recstartptr;
			xlogreader->ReadRecPtr = recstartptr;
			xlogreader->EndRecPtr = recendptr;

			if (!DecodeXLogRecord(xlogreader, (XLogRecord *) xlogreader->readRecordBuf,
								  &errormsg))
			{
				elog(ERROR, "could not decode WAL record: %s", errormsg);
			}

			return (XLogRecord *) xlogreader->readRecordBuf;
		}
	}

	return NULL;
}


static void
scan_nonrelwal_files(void)
{
	DIR		   *xldir;
	struct dirent *xlde;

	xldir = AllocateDir("pg_wal/nonrelwal");
	if (xldir == NULL && errno == ENOENT)
	{
		return;
	}
	while ((xlde = ReadDir(xldir, "pg_wal/nonrelwal")) != NULL)
	{
		XLogRecPtr	startptr;
		XLogRecPtr	endptr;

		if (parse_nonrelwal_filename(xlde->d_name, &startptr, &endptr))
		{
			nonrelwal_file_info *entry;
			entry = palloc(sizeof(nonrelwal_file_info));
			entry->filename = pstrdup(xlde->d_name);
			entry->startptr = startptr;
			entry->endptr = endptr;

			nonrelwal_files = lappend(nonrelwal_files, entry);
		}
	}
	FreeDir(xldir);

	elog(LOG, "there are %d non-rel WAL files", list_length(nonrelwal_files));

	list_sort(nonrelwal_files, nonrelwal_entry_cmp);
}

static int
nonrelwal_entry_cmp(const ListCell *a, const ListCell *b)
{
	nonrelwal_file_info *aentry = (nonrelwal_file_info *) lfirst(a);
	nonrelwal_file_info *bentry = (nonrelwal_file_info *) lfirst(b);

	if (aentry->startptr > bentry->startptr)
		return 1;
	else if (aentry->startptr < bentry->startptr)
		return -1;
	else
		return 0;
}


static bool
parse_nonrelwal_filename(const char *fname, XLogRecPtr *startptr, XLogRecPtr *endptr)
{
	static const char pattern[] = "nonrel_XXXXXXXXXXXXXXXX-XXXXXXXXXXXXXXXX";
	uint32		startptr_hi;
	uint32		startptr_lo;
	uint32		endptr_hi;
	uint32		endptr_lo;

	if (strlen(fname) != strlen(pattern))
		return false;

	if (sscanf(fname, "nonrel_%08X%08X-%08X%08X",
			   &startptr_hi, &startptr_lo,
			   &endptr_hi, &endptr_lo) != 4)
		return false;

	*startptr = (uint64) startptr_hi << 32 | startptr_lo;
	*endptr = (uint64) endptr_hi << 32 | endptr_lo;
	return true;
}
