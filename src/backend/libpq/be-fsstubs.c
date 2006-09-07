/*-------------------------------------------------------------------------
 *
 * be-fsstubs.c
 *	  Builtin functions for open/close/read/write operations on large objects
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/libpq/be-fsstubs.c,v 1.83 2006/09/07 15:37:25 momjian Exp $
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
#include "utils/memutils.h"


/*#define FSDB 1*/
#define BUFSIZE			8192

/*
 * LO "FD"s are indexes into the cookies array.
 *
 * A non-null entry is a pointer to a LargeObjectDesc allocated in the
 * LO private memory context "fscxt".  The cookies array itself is also
 * dynamically allocated in that context.  Its current allocated size is
 * cookies_len entries, of which any unused entries will be NULL.
 */
static LargeObjectDesc **cookies = NULL;
static int	cookies_size = 0;

static MemoryContext fscxt = NULL;

#define CreateFSContext() \
	do { \
		if (fscxt == NULL) \
			fscxt = AllocSetContextCreate(TopMemoryContext, \
										  "Filesystem", \
										  ALLOCSET_DEFAULT_MINSIZE, \
										  ALLOCSET_DEFAULT_INITSIZE, \
										  ALLOCSET_DEFAULT_MAXSIZE); \
	} while (0)


static int	newLOfd(LargeObjectDesc *lobjCookie);
static void deleteLOfd(int fd);


/*****************************************************************************
 *	File Interfaces for Large Objects
 *****************************************************************************/

Datum
lo_open(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);
	int32		mode = PG_GETARG_INT32(1);
	LargeObjectDesc *lobjDesc;
	int			fd;

#if FSDB
	elog(DEBUG4, "lo_open(%u,%d)", lobjId, mode);
#endif

	CreateFSContext();

	lobjDesc = inv_open(lobjId, mode, fscxt);

	if (lobjDesc == NULL)
	{							/* lookup failed */
#if FSDB
		elog(DEBUG4, "could not open large object %u", lobjId);
#endif
		PG_RETURN_INT32(-1);
	}

	fd = newLOfd(lobjDesc);

	PG_RETURN_INT32(fd);
}

Datum
lo_close(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		PG_RETURN_INT32(-1);
	}
#if FSDB
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

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		return -1;
	}

	status = inv_read(cookies[fd], buf, len);

	return status;
}

int
lo_write(int fd, const char *buf, int len)
{
	int			status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		return -1;
	}

	if ((cookies[fd]->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			  errmsg("large object descriptor %d was not opened for writing",
					 fd)));

	status = inv_write(cookies[fd], buf, len);

	return status;
}


Datum
lo_lseek(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		offset = PG_GETARG_INT32(1);
	int32		whence = PG_GETARG_INT32(2);
	int			status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		PG_RETURN_INT32(-1);
	}

	status = inv_seek(cookies[fd], offset, whence);

	PG_RETURN_INT32(status);
}

Datum
lo_creat(PG_FUNCTION_ARGS)
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
lo_create(PG_FUNCTION_ARGS)
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
lo_tell(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		PG_RETURN_INT32(-1);
	}

	PG_RETURN_INT32(inv_tell(cookies[fd]));
}

Datum
lo_unlink(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);

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
loread(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		len = PG_GETARG_INT32(1);
	bytea	   *retval;
	int			totalread;

	if (len < 0)
		len = 0;

	retval = (bytea *) palloc(VARHDRSZ + len);
	totalread = lo_read(fd, VARDATA(retval), len);
	VARATT_SIZEP(retval) = totalread + VARHDRSZ;

	PG_RETURN_BYTEA_P(retval);
}

Datum
lowrite(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	bytea	   *wbuf = PG_GETARG_BYTEA_P(1);
	int			bytestowrite;
	int			totalwritten;

	bytestowrite = VARSIZE(wbuf) - VARHDRSZ;
	totalwritten = lo_write(fd, VARDATA(wbuf), bytestowrite);
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
lo_import(PG_FUNCTION_ARGS)
{
	text	   *filename = PG_GETARG_TEXT_P(0);
	File		fd;
	int			nbytes,
				tmp;
	char		buf[BUFSIZE];
	char		fnamebuf[MAXPGPATH];
	LargeObjectDesc *lobj;
	Oid			lobjOid;

#ifndef ALLOW_DANGEROUS_LO_FUNCTIONS
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use server-side lo_import()"),
				 errhint("Anyone can use the client-side lo_import() provided by libpq.")));
#endif

	CreateFSContext();

	/*
	 * open the file to be read in
	 */
	nbytes = VARSIZE(filename) - VARHDRSZ;
	if (nbytes >= MAXPGPATH)
		nbytes = MAXPGPATH - 1;
	memcpy(fnamebuf, VARDATA(filename), nbytes);
	fnamebuf[nbytes] = '\0';
	fd = PathNameOpenFile(fnamebuf, O_RDONLY | PG_BINARY, 0666);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open server file \"%s\": %m",
						fnamebuf)));

	/*
	 * create an inversion object
	 */
	lobjOid = inv_create(InvalidOid);

	/*
	 * read in from the filesystem and write to the inversion object
	 */
	lobj = inv_open(lobjOid, INV_WRITE, fscxt);

	while ((nbytes = FileRead(fd, buf, BUFSIZE)) > 0)
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
	FileClose(fd);

	PG_RETURN_OID(lobjOid);
}

/*
 * lo_export -
 *	  exports an (inversion) large object.
 */
Datum
lo_export(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);
	text	   *filename = PG_GETARG_TEXT_P(1);
	File		fd;
	int			nbytes,
				tmp;
	char		buf[BUFSIZE];
	char		fnamebuf[MAXPGPATH];
	LargeObjectDesc *lobj;
	mode_t		oumask;

#ifndef ALLOW_DANGEROUS_LO_FUNCTIONS
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use server-side lo_export()"),
				 errhint("Anyone can use the client-side lo_export() provided by libpq.")));
#endif

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
	nbytes = VARSIZE(filename) - VARHDRSZ;
	if (nbytes >= MAXPGPATH)
		nbytes = MAXPGPATH - 1;
	memcpy(fnamebuf, VARDATA(filename), nbytes);
	fnamebuf[nbytes] = '\0';
	oumask = umask((mode_t) 0022);
	fd = PathNameOpenFile(fnamebuf, O_CREAT | O_WRONLY | O_TRUNC | PG_BINARY, 0666);
	umask(oumask);
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
		tmp = FileWrite(fd, buf, nbytes);
		if (tmp != nbytes)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write server file \"%s\": %m",
							fnamebuf)));
	}

	FileClose(fd);
	inv_close(lobj);

	PG_RETURN_INT32(1);
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
