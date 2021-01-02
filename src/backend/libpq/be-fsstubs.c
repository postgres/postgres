/*-------------------------------------------------------------------------
 *
 * be-fsstubs.c
 *	  Builtin functions for open/close/read/write operations on large objects
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-fsstubs.c
 *
 * NOTES
 *	  This should be moved to a more appropriate place.  It is here
 *	  for lack of a better place.
 *
 *	  These functions store LargeObjectDesc structs in a private MemoryContext,
 *	  which means that large object descriptors hang around until we destroy
 *	  the context at transaction end.  It'd be possible to prolong the lifetime
 *	  of the context so that LO FDs are good across transactions (for example,
 *	  we could release the context only if we see that no FDs remain open).
 *	  But we'd need additional state in order to do the right thing at the
 *	  end of an aborted transaction.  FDs opened during an aborted xact would
 *	  still need to be closed, since they might not be pointing at valid
 *	  relations at all.  Locking semantics are also an interesting problem
 *	  if LOs stay open across transactions.  For now, we'll stick with the
 *	  existing documented semantics of LO FDs: they're only good within a
 *	  transaction.
 *
 *	  As of PostgreSQL 8.0, much of the angst expressed above is no longer
 *	  relevant, and in fact it'd be pretty easy to allow LO FDs to stay
 *	  open across transactions.  (Snapshot relevancy would still be an issue.)
 *	  However backwards compatibility suggests that we should stick to the
 *	  status quo.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libpq/be-fsstubs.h"
#include "libpq/libpq-fs.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/large_object.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/* define this to enable debug logging */
/* #define FSDB 1 */
/* chunk size for lo_import/lo_export transfers */
#define BUFSIZE			8192

/*
 * LO "FD"s are indexes into the cookies array.
 *
 * A non-null entry is a pointer to a LargeObjectDesc allocated in the
 * LO private memory context "fscxt".  The cookies array itself is also
 * dynamically allocated in that context.  Its current allocated size is
 * cookies_size entries, of which any unused entries will be NULL.
 */
static LargeObjectDesc **cookies = NULL;
static int	cookies_size = 0;

static MemoryContext fscxt = NULL;

#define CreateFSContext() \
	do { \
		if (fscxt == NULL) \
			fscxt = AllocSetContextCreate(TopMemoryContext, \
										  "Filesystem", \
										  ALLOCSET_DEFAULT_SIZES); \
	} while (0)


static int	newLOfd(LargeObjectDesc *lobjCookie);
static void deleteLOfd(int fd);
static Oid	lo_import_internal(text *filename, Oid lobjOid);


/*****************************************************************************
 *	File Interfaces for Large Objects
 *****************************************************************************/

Datum
be_lo_open(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);
	int32		mode = PG_GETARG_INT32(1);
	LargeObjectDesc *lobjDesc;
	int			fd;

#ifdef FSDB
	elog(DEBUG4, "lo_open(%u,%d)", lobjId, mode);
#endif

	CreateFSContext();

	lobjDesc = inv_open(lobjId, mode, fscxt);

	fd = newLOfd(lobjDesc);

	PG_RETURN_INT32(fd);
}

Datum
be_lo_close(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

#ifdef FSDB
	elog(DEBUG4, "lo_close(%d)", fd);
#endif

	inv_close(cookies[fd]);

	deleteLOfd(fd);

	PG_RETURN_INT32(0);
}


/*****************************************************************************
 *	Bare Read/Write operations --- these are not fmgr-callable!
 *
 *	We assume the large object supports byte oriented reads and seeks so
 *	that our work is easier.
 *
 *****************************************************************************/

int
lo_read(int fd, char *buf, int len)
{
	int			status;
	LargeObjectDesc *lobj;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
	lobj = cookies[fd];

	/*
	 * Check state.  inv_read() would throw an error anyway, but we want the
	 * error to be about the FD's state not the underlying privilege; it might
	 * be that the privilege exists but user forgot to ask for read mode.
	 */
	if ((lobj->flags & IFS_RDLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("large object descriptor %d was not opened for reading",
						fd)));

	status = inv_read(lobj, buf, len);

	return status;
}

int
lo_write(int fd, const char *buf, int len)
{
	int			status;
	LargeObjectDesc *lobj;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
	lobj = cookies[fd];

	/* see comment in lo_read() */
	if ((lobj->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("large object descriptor %d was not opened for writing",
						fd)));

	status = inv_write(lobj, buf, len);

	return status;
}

Datum
be_lo_lseek(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		offset = PG_GETARG_INT32(1);
	int32		whence = PG_GETARG_INT32(2);
	int64		status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	status = inv_seek(cookies[fd], offset, whence);

	/* guard against result overflow */
	if (status != (int32) status)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("lo_lseek result out of range for large-object descriptor %d",
						fd)));

	PG_RETURN_INT32((int32) status);
}

Datum
be_lo_lseek64(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		offset = PG_GETARG_INT64(1);
	int32		whence = PG_GETARG_INT32(2);
	int64		status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	status = inv_seek(cookies[fd], offset, whence);

	PG_RETURN_INT64(status);
}

Datum
be_lo_creat(PG_FUNCTION_ARGS)
{
	Oid			lobjId;

	/*
	 * We don't actually need to store into fscxt, but create it anyway to
	 * ensure that AtEOXact_LargeObject knows there is state to clean up
	 */
	CreateFSContext();

	lobjId = inv_create(InvalidOid);

	PG_RETURN_OID(lobjId);
}

Datum
be_lo_create(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);

	/*
	 * We don't actually need to store into fscxt, but create it anyway to
	 * ensure that AtEOXact_LargeObject knows there is state to clean up
	 */
	CreateFSContext();

	lobjId = inv_create(lobjId);

	PG_RETURN_OID(lobjId);
}

Datum
be_lo_tell(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		offset;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	offset = inv_tell(cookies[fd]);

	/* guard against result overflow */
	if (offset != (int32) offset)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("lo_tell result out of range for large-object descriptor %d",
						fd)));

	PG_RETURN_INT32((int32) offset);
}

Datum
be_lo_tell64(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		offset;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	offset = inv_tell(cookies[fd]);

	PG_RETURN_INT64(offset);
}

Datum
be_lo_unlink(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);

	/*
	 * Must be owner of the large object.  It would be cleaner to check this
	 * in inv_drop(), but we want to throw the error before not after closing
	 * relevant FDs.
	 */
	if (!lo_compat_privileges &&
		!pg_largeobject_ownercheck(lobjId, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of large object %u", lobjId)));

	/*
	 * If there are any open LO FDs referencing that ID, close 'em.
	 */
	if (fscxt != NULL)
	{
		int			i;

		for (i = 0; i < cookies_size; i++)
		{
			if (cookies[i] != NULL && cookies[i]->id == lobjId)
			{
				inv_close(cookies[i]);
				deleteLOfd(i);
			}
		}
	}

	/*
	 * inv_drop does not create a need for end-of-transaction cleanup and
	 * hence we don't need to have created fscxt.
	 */
	PG_RETURN_INT32(inv_drop(lobjId));
}

/*****************************************************************************
 *	Read/Write using bytea
 *****************************************************************************/

Datum
be_loread(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		len = PG_GETARG_INT32(1);
	bytea	   *retval;
	int			totalread;

	if (len < 0)
		len = 0;

	retval = (bytea *) palloc(VARHDRSZ + len);
	totalread = lo_read(fd, VARDATA(retval), len);
	SET_VARSIZE(retval, totalread + VARHDRSZ);

	PG_RETURN_BYTEA_P(retval);
}

Datum
be_lowrite(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	bytea	   *wbuf = PG_GETARG_BYTEA_PP(1);
	int			bytestowrite;
	int			totalwritten;

	bytestowrite = VARSIZE_ANY_EXHDR(wbuf);
	totalwritten = lo_write(fd, VARDATA_ANY(wbuf), bytestowrite);
	PG_RETURN_INT32(totalwritten);
}

/*****************************************************************************
 *	 Import/Export of Large Object
 *****************************************************************************/

/*
 * lo_import -
 *	  imports a file as an (inversion) large object.
 */
Datum
be_lo_import(PG_FUNCTION_ARGS)
{
	text	   *filename = PG_GETARG_TEXT_PP(0);

	PG_RETURN_OID(lo_import_internal(filename, InvalidOid));
}

/*
 * lo_import_with_oid -
 *	  imports a file as an (inversion) large object specifying oid.
 */
Datum
be_lo_import_with_oid(PG_FUNCTION_ARGS)
{
	text	   *filename = PG_GETARG_TEXT_PP(0);
	Oid			oid = PG_GETARG_OID(1);

	PG_RETURN_OID(lo_import_internal(filename, oid));
}

static Oid
lo_import_internal(text *filename, Oid lobjOid)
{
	int			fd;
	int			nbytes,
				tmp PG_USED_FOR_ASSERTS_ONLY;
	char		buf[BUFSIZE];
	char		fnamebuf[MAXPGPATH];
	LargeObjectDesc *lobj;
	Oid			oid;

	CreateFSContext();

	/*
	 * open the file to be read in
	 */
	text_to_cstring_buffer(filename, fnamebuf, sizeof(fnamebuf));
	fd = OpenTransientFile(fnamebuf, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open server file \"%s\": %m",
						fnamebuf)));

	/*
	 * create an inversion object
	 */
	oid = inv_create(lobjOid);

	/*
	 * read in from the filesystem and write to the inversion object
	 */
	lobj = inv_open(oid, INV_WRITE, fscxt);

	while ((nbytes = read(fd, buf, BUFSIZE)) > 0)
	{
		tmp = inv_write(lobj, buf, nbytes);
		Assert(tmp == nbytes);
	}

	if (nbytes < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read server file \"%s\": %m",
						fnamebuf)));

	inv_close(lobj);

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						fnamebuf)));

	return oid;
}

/*
 * lo_export -
 *	  exports an (inversion) large object.
 */
Datum
be_lo_export(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);
	text	   *filename = PG_GETARG_TEXT_PP(1);
	int			fd;
	int			nbytes,
				tmp;
	char		buf[BUFSIZE];
	char		fnamebuf[MAXPGPATH];
	LargeObjectDesc *lobj;
	mode_t		oumask;

	CreateFSContext();

	/*
	 * open the inversion object (no need to test for failure)
	 */
	lobj = inv_open(lobjId, INV_READ, fscxt);

	/*
	 * open the file to be written to
	 *
	 * Note: we reduce backend's normal 077 umask to the slightly friendlier
	 * 022. This code used to drop it all the way to 0, but creating
	 * world-writable export files doesn't seem wise.
	 */
	text_to_cstring_buffer(filename, fnamebuf, sizeof(fnamebuf));
	oumask = umask(S_IWGRP | S_IWOTH);
	PG_TRY();
	{
		fd = OpenTransientFilePerm(fnamebuf, O_CREAT | O_WRONLY | O_TRUNC | PG_BINARY,
								   S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	}
	PG_FINALLY();
	{
		umask(oumask);
	}
	PG_END_TRY();
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create server file \"%s\": %m",
						fnamebuf)));

	/*
	 * read in from the inversion file and write to the filesystem
	 */
	while ((nbytes = inv_read(lobj, buf, BUFSIZE)) > 0)
	{
		tmp = write(fd, buf, nbytes);
		if (tmp != nbytes)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write server file \"%s\": %m",
							fnamebuf)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						fnamebuf)));

	inv_close(lobj);

	PG_RETURN_INT32(1);
}

/*
 * lo_truncate -
 *	  truncate a large object to a specified length
 */
static void
lo_truncate_internal(int32 fd, int64 len)
{
	LargeObjectDesc *lobj;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
	lobj = cookies[fd];

	/* see comment in lo_read() */
	if ((lobj->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("large object descriptor %d was not opened for writing",
						fd)));

	inv_truncate(lobj, len);
}

Datum
be_lo_truncate(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		len = PG_GETARG_INT32(1);

	lo_truncate_internal(fd, len);
	PG_RETURN_INT32(0);
}

Datum
be_lo_truncate64(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		len = PG_GETARG_INT64(1);

	lo_truncate_internal(fd, len);
	PG_RETURN_INT32(0);
}

/*
 * AtEOXact_LargeObject -
 *		 prepares large objects for transaction commit
 */
void
AtEOXact_LargeObject(bool isCommit)
{
	int			i;

	if (fscxt == NULL)
		return;					/* no LO operations in this xact */

	/*
	 * Close LO fds and clear cookies array so that LO fds are no longer good.
	 * On abort we skip the close step.
	 */
	for (i = 0; i < cookies_size; i++)
	{
		if (cookies[i] != NULL)
		{
			if (isCommit)
				inv_close(cookies[i]);
			deleteLOfd(i);
		}
	}

	/* Needn't actually pfree since we're about to zap context */
	cookies = NULL;
	cookies_size = 0;

	/* Release the LO memory context to prevent permanent memory leaks. */
	MemoryContextDelete(fscxt);
	fscxt = NULL;

	/* Give inv_api.c a chance to clean up, too */
	close_lo_relation(isCommit);
}

/*
 * AtEOSubXact_LargeObject
 *		Take care of large objects at subtransaction commit/abort
 *
 * Reassign LOs created/opened during a committing subtransaction
 * to the parent subtransaction.  On abort, just close them.
 */
void
AtEOSubXact_LargeObject(bool isCommit, SubTransactionId mySubid,
						SubTransactionId parentSubid)
{
	int			i;

	if (fscxt == NULL)			/* no LO operations in this xact */
		return;

	for (i = 0; i < cookies_size; i++)
	{
		LargeObjectDesc *lo = cookies[i];

		if (lo != NULL && lo->subid == mySubid)
		{
			if (isCommit)
				lo->subid = parentSubid;
			else
			{
				/*
				 * Make sure we do not call inv_close twice if it errors out
				 * for some reason.  Better a leak than a crash.
				 */
				deleteLOfd(i);
				inv_close(lo);
			}
		}
	}
}

/*****************************************************************************
 *	Support routines for this file
 *****************************************************************************/

static int
newLOfd(LargeObjectDesc *lobjCookie)
{
	int			i,
				newsize;

	/* Try to find a free slot */
	for (i = 0; i < cookies_size; i++)
	{
		if (cookies[i] == NULL)
		{
			cookies[i] = lobjCookie;
			return i;
		}
	}

	/* No free slot, so make the array bigger */
	if (cookies_size <= 0)
	{
		/* First time through, arbitrarily make 64-element array */
		i = 0;
		newsize = 64;
		cookies = (LargeObjectDesc **)
			MemoryContextAllocZero(fscxt, newsize * sizeof(LargeObjectDesc *));
		cookies_size = newsize;
	}
	else
	{
		/* Double size of array */
		i = cookies_size;
		newsize = cookies_size * 2;
		cookies = (LargeObjectDesc **)
			repalloc(cookies, newsize * sizeof(LargeObjectDesc *));
		MemSet(cookies + cookies_size, 0,
			   (newsize - cookies_size) * sizeof(LargeObjectDesc *));
		cookies_size = newsize;
	}

	Assert(cookies[i] == NULL);
	cookies[i] = lobjCookie;
	return i;
}

static void
deleteLOfd(int fd)
{
	cookies[fd] = NULL;
}

/*****************************************************************************
 *	Wrappers oriented toward SQL callers
 *****************************************************************************/

/*
 * Read [offset, offset+nbytes) within LO; when nbytes is -1, read to end.
 */
static bytea *
lo_get_fragment_internal(Oid loOid, int64 offset, int32 nbytes)
{
	LargeObjectDesc *loDesc;
	int64		loSize;
	int64		result_length;
	int			total_read PG_USED_FOR_ASSERTS_ONLY;
	bytea	   *result = NULL;

	/*
	 * We don't actually need to store into fscxt, but create it anyway to
	 * ensure that AtEOXact_LargeObject knows there is state to clean up
	 */
	CreateFSContext();

	loDesc = inv_open(loOid, INV_READ, fscxt);

	/*
	 * Compute number of bytes we'll actually read, accommodating nbytes == -1
	 * and reads beyond the end of the LO.
	 */
	loSize = inv_seek(loDesc, 0, SEEK_END);
	if (loSize > offset)
	{
		if (nbytes >= 0 && nbytes <= loSize - offset)
			result_length = nbytes; /* request is wholly inside LO */
		else
			result_length = loSize - offset;	/* adjust to end of LO */
	}
	else
		result_length = 0;		/* request is wholly outside LO */

	/*
	 * A result_length calculated from loSize may not fit in a size_t.  Check
	 * that the size will satisfy this and subsequently-enforced size limits.
	 */
	if (result_length > MaxAllocSize - VARHDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("large object read request is too large")));

	result = (bytea *) palloc(VARHDRSZ + result_length);

	inv_seek(loDesc, offset, SEEK_SET);
	total_read = inv_read(loDesc, VARDATA(result), result_length);
	Assert(total_read == result_length);
	SET_VARSIZE(result, result_length + VARHDRSZ);

	inv_close(loDesc);

	return result;
}

/*
 * Read entire LO
 */
Datum
be_lo_get(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	bytea	   *result;

	result = lo_get_fragment_internal(loOid, 0, -1);

	PG_RETURN_BYTEA_P(result);
}

/*
 * Read range within LO
 */
Datum
be_lo_get_fragment(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	int64		offset = PG_GETARG_INT64(1);
	int32		nbytes = PG_GETARG_INT32(2);
	bytea	   *result;

	if (nbytes < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("requested length cannot be negative")));

	result = lo_get_fragment_internal(loOid, offset, nbytes);

	PG_RETURN_BYTEA_P(result);
}

/*
 * Create LO with initial contents given by a bytea argument
 */
Datum
be_lo_from_bytea(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	bytea	   *str = PG_GETARG_BYTEA_PP(1);
	LargeObjectDesc *loDesc;
	int			written PG_USED_FOR_ASSERTS_ONLY;

	CreateFSContext();

	loOid = inv_create(loOid);
	loDesc = inv_open(loOid, INV_WRITE, fscxt);
	written = inv_write(loDesc, VARDATA_ANY(str), VARSIZE_ANY_EXHDR(str));
	Assert(written == VARSIZE_ANY_EXHDR(str));
	inv_close(loDesc);

	PG_RETURN_OID(loOid);
}

/*
 * Update range within LO
 */
Datum
be_lo_put(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	int64		offset = PG_GETARG_INT64(1);
	bytea	   *str = PG_GETARG_BYTEA_PP(2);
	LargeObjectDesc *loDesc;
	int			written PG_USED_FOR_ASSERTS_ONLY;

	CreateFSContext();

	loDesc = inv_open(loOid, INV_WRITE, fscxt);

	/* Permission check */
	if (!lo_compat_privileges &&
		pg_largeobject_aclcheck_snapshot(loDesc->id,
										 GetUserId(),
										 ACL_UPDATE,
										 loDesc->snapshot) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						loDesc->id)));

	inv_seek(loDesc, offset, SEEK_SET);
	written = inv_write(loDesc, VARDATA_ANY(str), VARSIZE_ANY_EXHDR(str));
	Assert(written == VARSIZE_ANY_EXHDR(str));
	inv_close(loDesc);

	PG_RETURN_VOID();
}
