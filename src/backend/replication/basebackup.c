/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it to a standby
 *
 * Portions Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/basebackup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "access/xlog_internal.h"		/* for pg_start/stop_backup */
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "nodes/pg_list.h"
#include "replication/basebackup.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

static int64 sendDir(char *path, int basepathlen, bool sizeonly);
static void sendFile(char *path, int basepathlen, struct stat * statbuf);
static void _tarWriteHeader(char *filename, char *linktarget,
				struct stat * statbuf);
static void send_int8_string(StringInfoData *buf, int64 intval);
static void SendBackupHeader(List *tablespaces);
static void SendBackupDirectory(char *location, char *spcoid);
static void base_backup_cleanup(int code, Datum arg);
static void perform_base_backup(const char *backup_label, bool progress, DIR *tblspcdir);

typedef struct
{
	char	   *oid;
	char	   *path;
	int64		size;
}	tablespaceinfo;


/*
 * Called when ERROR or FATAL happens in perform_base_backup() after
 * we have started the backup - make sure we end it!
 */
static void
base_backup_cleanup(int code, Datum arg)
{
	do_pg_abort_backup();
}

/*
 * Actually do a base backup for the specified tablespaces.
 *
 * This is split out mainly to avoid complaints about "variable might be
 * clobbered by longjmp" from stupider versions of gcc.
 */
static void
perform_base_backup(const char *backup_label, bool progress, DIR *tblspcdir)
{
	do_pg_start_backup(backup_label, true);

	PG_ENSURE_ERROR_CLEANUP(base_backup_cleanup, (Datum) 0);
	{
		List	   *tablespaces = NIL;
		ListCell   *lc;
		struct dirent *de;
		tablespaceinfo *ti;


		/* Add a node for the base directory */
		ti = palloc0(sizeof(tablespaceinfo));
		ti->size = progress ? sendDir(".", 1, true) : -1;
		tablespaces = lappend(tablespaces, ti);

		/* Collect information about all tablespaces */
		while ((de = ReadDir(tblspcdir, "pg_tblspc")) != NULL)
		{
			char		fullpath[MAXPGPATH];
			char		linkpath[MAXPGPATH];

			/* Skip special stuff */
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;

			snprintf(fullpath, sizeof(fullpath), "pg_tblspc/%s", de->d_name);

			MemSet(linkpath, 0, sizeof(linkpath));
			if (readlink(fullpath, linkpath, sizeof(linkpath) - 1) == -1)
			{
				ereport(WARNING,
						(errmsg("unable to read symbolic link %s: %m", fullpath)));
				continue;
			}

			ti = palloc(sizeof(tablespaceinfo));
			ti->oid = pstrdup(de->d_name);
			ti->path = pstrdup(linkpath);
			ti->size = progress ? sendDir(linkpath, strlen(linkpath), true) : -1;
			tablespaces = lappend(tablespaces, ti);
		}


		/* Send tablespace header */
		SendBackupHeader(tablespaces);

		/* Send off our tablespaces one by one */
		foreach(lc, tablespaces)
		{
			tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);

			SendBackupDirectory(ti->path, ti->oid);
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(base_backup_cleanup, (Datum) 0);

	do_pg_stop_backup();
}

/*
 * SendBaseBackup() - send a complete base backup.
 *
 * The function will take care of running pg_start_backup() and
 * pg_stop_backup() for the user.
 */
void
SendBaseBackup(const char *backup_label, bool progress)
{
	DIR		   *dir;
	MemoryContext backup_context;
	MemoryContext old_context;

	backup_context = AllocSetContextCreate(CurrentMemoryContext,
										   "Streaming base backup context",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);
	old_context = MemoryContextSwitchTo(backup_context);

	WalSndSetState(WALSNDSTATE_BACKUP);

	if (backup_label == NULL)
		backup_label = "base backup";

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "sending backup \"%s\"",
				 backup_label);
		set_ps_display(activitymsg, false);
	}

	/* Make sure we can open the directory with tablespaces in it */
	dir = AllocateDir("pg_tblspc");
	if (!dir)
		ereport(ERROR,
				(errmsg("unable to open directory pg_tblspc: %m")));

	perform_base_backup(backup_label, progress, dir);

	FreeDir(dir);

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(backup_context);
}

static void
send_int8_string(StringInfoData *buf, int64 intval)
{
	char		is[32];

	sprintf(is, INT64_FORMAT, intval);
	pq_sendint(buf, strlen(is), 4);
	pq_sendbytes(buf, is, strlen(is));
}

static void
SendBackupHeader(List *tablespaces)
{
	StringInfoData buf;
	ListCell   *lc;

	/* Construct and send the directory information */
	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint(&buf, 3, 2);		/* 3 fields */

	/* First field - spcoid */
	pq_sendstring(&buf, "spcoid");
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, OIDOID, 4);	/* type oid */
	pq_sendint(&buf, 4, 2);		/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* Second field - spcpath */
	pq_sendstring(&buf, "spclocation");
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_sendint(&buf, TEXTOID, 4);
	pq_sendint(&buf, -1, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);

	/* Third field - size */
	pq_sendstring(&buf, "size");
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_sendint(&buf, INT8OID, 4);
	pq_sendint(&buf, 8, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);

	foreach(lc, tablespaces)
	{
		tablespaceinfo *ti = lfirst(lc);

		/* Send one datarow message */
		pq_beginmessage(&buf, 'D');
		pq_sendint(&buf, 3, 2); /* number of columns */
		if (ti->path == NULL)
		{
			pq_sendint(&buf, -1, 4);	/* Length = -1 ==> NULL */
			pq_sendint(&buf, -1, 4);
		}
		else
		{
			pq_sendint(&buf, strlen(ti->oid), 4);		/* length */
			pq_sendbytes(&buf, ti->oid, strlen(ti->oid));
			pq_sendint(&buf, strlen(ti->path), 4);		/* length */
			pq_sendbytes(&buf, ti->path, strlen(ti->path));
		}
		if (ti->size >= 0)
			send_int8_string(&buf, ti->size / 1024);
		else
			pq_sendint(&buf, -1, 4);	/* NULL */

		pq_endmessage(&buf);
	}

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

static void
SendBackupDirectory(char *location, char *spcoid)
{
	StringInfoData buf;

	/* Send CopyOutResponse message */
	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, 0);		/* overall format */
	pq_sendint(&buf, 0, 2);		/* natts */
	pq_endmessage(&buf);

	/* tar up the data directory if NULL, otherwise the tablespace */
	sendDir(location == NULL ? "." : location,
			location == NULL ? 1 : strlen(location),
			false);

	/* Send CopyDone message */
	pq_putemptymessage('c');
}


static int64
sendDir(char *path, int basepathlen, bool sizeonly)
{
	DIR		   *dir;
	struct dirent *de;
	char		pathbuf[MAXPGPATH];
	struct stat statbuf;
	int64		size = 0;

	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		/* Skip special stuff */
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		/* Skip temporary files */
		if (strncmp(de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
			continue;

		/*
		 * Check if the postmaster has signaled us to exit, and abort
		 * with an error in that case. The error handler further up
		 * will call do_pg_abort_backup() for us.
		 */
		if (walsender_shutdown_requested || walsender_ready_to_stop)
			ereport(ERROR,
					(errmsg("shutdown requested, aborting active base backup")));

		snprintf(pathbuf, MAXPGPATH, "%s/%s", path, de->d_name);

		/* Skip postmaster.pid in the data directory */
		if (strcmp(pathbuf, "./postmaster.pid") == 0)
			continue;

		if (lstat(pathbuf, &statbuf) != 0)
		{
			if (errno != ENOENT)
				ereport(ERROR,
						(errcode(errcode_for_file_access()),
						 errmsg("could not stat file or directory \"%s\": %m",
								pathbuf)));

			/* If the file went away while scanning, it's no error. */
			continue;
		}

		/*
		 * We can skip pg_xlog, the WAL segments need to be fetched from the
		 * WAL archive anyway. But include it as an empty directory anyway, so
		 * we get permissions right.
		 */
		if (strcmp(pathbuf, "./pg_xlog") == 0)
		{
			if (!sizeonly)
				_tarWriteHeader(pathbuf + basepathlen + 1, NULL, &statbuf);
			size += 512;		/* Size of the header just added */
			continue;			/* don't recurse into pg_xlog */
		}

#ifndef WIN32
		if (S_ISLNK(statbuf.st_mode) && strcmp(path, "./pg_tblspc") == 0)
#else
		if (pgwin32_is_junction(pathbuf) && strcmp(path, "./pg_tblspc") == 0)
#endif
		{
			/* Allow symbolic links in pg_tblspc */
			char		linkpath[MAXPGPATH];

			MemSet(linkpath, 0, sizeof(linkpath));
			if (readlink(pathbuf, linkpath, sizeof(linkpath) - 1) == -1)
				ereport(ERROR,
						(errcode(errcode_for_file_access()),
						 errmsg("could not read symbolic link \"%s\": %m",
								pathbuf)));
			if (!sizeonly)
				_tarWriteHeader(pathbuf + basepathlen + 1, linkpath, &statbuf);
			size += 512;		/* Size of the header just added */
		}
		else if (S_ISDIR(statbuf.st_mode))
		{
			/*
			 * Store a directory entry in the tar file so we can get the
			 * permissions right.
			 */
			if (!sizeonly)
				_tarWriteHeader(pathbuf + basepathlen + 1, NULL, &statbuf);
			size += 512;		/* Size of the header just added */

			/* call ourselves recursively for a directory */
			size += sendDir(pathbuf, basepathlen, sizeonly);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			/* Add size, rounded up to 512byte block */
			size += ((statbuf.st_size + 511) & ~511);
			if (!sizeonly)
				sendFile(pathbuf, basepathlen, &statbuf);
			size += 512;		/* Size of the header of the file */
		}
		else
			ereport(WARNING,
					(errmsg("skipping special file \"%s\"", pathbuf)));
	}
	FreeDir(dir);
	return size;
}

/*****
 * Functions for handling tar file format
 *
 * Copied from pg_dump, but modified to work with libpq for sending
 */


/*
 * Utility routine to print possibly larger than 32 bit integers in a
 * portable fashion.  Filled with zeros.
 */
static void
print_val(char *s, uint64 val, unsigned int base, size_t len)
{
	int			i;

	for (i = len; i > 0; i--)
	{
		int			digit = val % base;

		s[i - 1] = '0' + digit;
		val = val / base;
	}
}

/*
 * Maximum file size for a tar member: The limit inherent in the
 * format is 2^33-1 bytes (nearly 8 GB).  But we don't want to exceed
 * what we can represent in pgoff_t.
 */
#define MAX_TAR_MEMBER_FILELEN (((int64) 1 << Min(33, sizeof(pgoff_t)*8 - 1)) - 1)

static int
_tarChecksum(char *header)
{
	int			i,
				sum;

	sum = 0;
	for (i = 0; i < 512; i++)
		if (i < 148 || i >= 156)
			sum += 0xFF & header[i];
	return sum + 256;			/* Assume 8 blanks in checksum field */
}

/* Given the member, write the TAR header & send the file */
static void
sendFile(char *filename, int basepathlen, struct stat * statbuf)
{
	FILE	   *fp;
	char		buf[32768];
	size_t		cnt;
	pgoff_t		len = 0;
	size_t		pad;

	fp = AllocateFile(filename, "rb");
	if (fp == NULL)
		ereport(ERROR,
				(errcode(errcode_for_file_access()),
				 errmsg("could not open file \"%s\": %m", filename)));

	/*
	 * Some compilers will throw a warning knowing this test can never be true
	 * because pgoff_t can't exceed the compared maximum on their platform.
	 */
	if (statbuf->st_size > MAX_TAR_MEMBER_FILELEN)
		ereport(ERROR,
				(errmsg("archive member \"%s\" too large for tar format",
						filename)));

	_tarWriteHeader(filename + basepathlen + 1, NULL, statbuf);

	while ((cnt = fread(buf, 1, Min(sizeof(buf), statbuf->st_size - len), fp)) > 0)
	{
		/* Send the chunk as a CopyData message */
		if (pq_putmessage('d', buf, cnt))
			ereport(ERROR,
					(errmsg("base backup could not send data, aborting backup")));

		len += cnt;

		if (len >= statbuf->st_size)
		{
			/*
			 * Reached end of file. The file could be longer, if it was
			 * extended while we were sending it, but for a base backup we can
			 * ignore such extended data. It will be restored from WAL.
			 */
			break;
		}
	}

	/* If the file was truncated while we were sending it, pad it with zeros */
	if (len < statbuf->st_size)
	{
		MemSet(buf, 0, sizeof(buf));
		while (len < statbuf->st_size)
		{
			cnt = Min(sizeof(buf), statbuf->st_size - len);
			pq_putmessage('d', buf, cnt);
			len += cnt;
		}
	}

	/* Pad to 512 byte boundary, per tar format requirements */
	pad = ((len + 511) & ~511) - len;
	if (pad > 0)
	{
		MemSet(buf, 0, pad);
		pq_putmessage('d', buf, pad);
	}

	FreeFile(fp);
}


static void
_tarWriteHeader(char *filename, char *linktarget, struct stat * statbuf)
{
	char		h[512];
	int			lastSum = 0;
	int			sum;

	memset(h, 0, sizeof(h));

	/* Name 100 */
	sprintf(&h[0], "%.99s", filename);
	if (linktarget != NULL || S_ISDIR(statbuf->st_mode))
	{
		/*
		 * We only support symbolic links to directories, and this is
		 * indicated in the tar format by adding a slash at the end of the
		 * name, the same as for regular directories.
		 */
		h[strlen(filename)] = '/';
		h[strlen(filename) + 1] = '\0';
	}

	/* Mode 8 */
	sprintf(&h[100], "%07o ", statbuf->st_mode);

	/* User ID 8 */
	sprintf(&h[108], "%07o ", statbuf->st_uid);

	/* Group 8 */
	sprintf(&h[117], "%07o ", statbuf->st_gid);

	/* File size 12 - 11 digits, 1 space, no NUL */
	if (linktarget != NULL || S_ISDIR(statbuf->st_mode))
		/* Symbolic link or directory has size zero */
		print_val(&h[124], 0, 8, 11);
	else
		print_val(&h[124], statbuf->st_size, 8, 11);
	sprintf(&h[135], " ");

	/* Mod Time 12 */
	sprintf(&h[136], "%011o ", (int) statbuf->st_mtime);

	/* Checksum 8 */
	sprintf(&h[148], "%06o ", lastSum);

	if (linktarget != NULL)
	{
		/* Type - Symbolic link */
		sprintf(&h[156], "2");
		strcpy(&h[157], linktarget);
	}
	else if (S_ISDIR(statbuf->st_mode))
		/* Type - directory */
		sprintf(&h[156], "5");
	else
		/* Type - regular file */
		sprintf(&h[156], "0");

	/* Link tag 100 (NULL) */

	/* Magic 6 + Version 2 */
	sprintf(&h[257], "ustar00");

	/* User 32 */
	/* XXX: Do we need to care about setting correct username? */
	sprintf(&h[265], "%.31s", "postgres");

	/* Group 32 */
	/* XXX: Do we need to care about setting correct group name? */
	sprintf(&h[297], "%.31s", "postgres");

	/* Maj Dev 8 */
	sprintf(&h[329], "%6o ", 0);

	/* Min Dev 8 */
	sprintf(&h[337], "%6o ", 0);

	while ((sum = _tarChecksum(h)) != lastSum)
	{
		sprintf(&h[148], "%06o ", sum);
		lastSum = sum;
	}

	pq_putmessage('d', h, 512);
}
