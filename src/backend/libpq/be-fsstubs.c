/*-------------------------------------------------------------------------
 *
 * be-fsstubs.c
 *	  support for filesystem operations on large objects
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/be-fsstubs.c,v 1.68 2003/08/04 02:39:59 momjian Exp $
 *
 * NOTES
 *	  This should be moved to a more appropriate place.  It is here
 *	  for lack of a better place.
 *
 *	  Builtin functions for open/close/read/write operations on large objects.
 *
 *	  These functions operate in a private MemoryContext, which means
 *	  that large object descriptors hang around until we destroy the context.
 *	  That happens in lo_commit().	It'd be possible to prolong the lifetime
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
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libpq/be-fsstubs.h"
#include "libpq/libpq-fs.h"
#include "miscadmin.h"
#include "storage/large_object.h"
#include "utils/memutils.h"


/* [PA] is Pascal André <andre@via.ecp.fr> */

/*#define FSDB 1*/
#define BUFSIZE			8192

/*
 * LO "FD"s are indexes into the cookies array.
 *
 * A non-null entry is a pointer to a LargeObjectDesc allocated in the
 * LO private memory context.  The cookies array itself is also dynamically
 * allocated in that context.  Its current allocated size is cookies_len
 * entries, of which any unused entries will be NULL.
 */
static LargeObjectDesc **cookies = NULL;
static int	cookies_size = 0;

static MemoryContext fscxt = NULL;


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
	MemoryContext currentContext;

#if FSDB
	elog(DEBUG4, "lo_open(%u,%d)", lobjId, mode);
#endif

	if (fscxt == NULL)
		fscxt = AllocSetContextCreate(TopMemoryContext,
									  "Filesystem",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	currentContext = MemoryContextSwitchTo(fscxt);

	lobjDesc = inv_open(lobjId, mode);

	if (lobjDesc == NULL)
	{							/* lookup failed */
		MemoryContextSwitchTo(currentContext);
#if FSDB
		elog(DEBUG4, "could not open large object %u", lobjId);
#endif
		PG_RETURN_INT32(-1);
	}

	fd = newLOfd(lobjDesc);

	MemoryContextSwitchTo(currentContext);

	PG_RETURN_INT32(fd);
}

Datum
lo_close(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	MemoryContext currentContext;

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

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo(fscxt);

	inv_close(cookies[fd]);

	deleteLOfd(fd);

	MemoryContextSwitchTo(currentContext);

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
	MemoryContext currentContext;
	int			status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		return -1;
	}

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo(fscxt);

	status = inv_read(cookies[fd], buf, len);

	MemoryContextSwitchTo(currentContext);

	return status;
}

int
lo_write(int fd, char *buf, int len)
{
	MemoryContext currentContext;
	int			status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		return -1;
	}

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo(fscxt);

	status = inv_write(cookies[fd], buf, len);

	MemoryContextSwitchTo(currentContext);

	return status;
}


Datum
lo_lseek(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		offset = PG_GETARG_INT32(1);
	int32		whence = PG_GETARG_INT32(2);
	MemoryContext currentContext;
	int			status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
		PG_RETURN_INT32(-1);
	}

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo(fscxt);

	status = inv_seek(cookies[fd], offset, whence);

	MemoryContextSwitchTo(currentContext);

	PG_RETURN_INT32(status);
}

Datum
lo_creat(PG_FUNCTION_ARGS)
{
	int32		mode = PG_GETARG_INT32(0);
	LargeObjectDesc *lobjDesc;
	MemoryContext currentContext;
	Oid			lobjId;

	if (fscxt == NULL)
		fscxt = AllocSetContextCreate(TopMemoryContext,
									  "Filesystem",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	currentContext = MemoryContextSwitchTo(fscxt);

	lobjDesc = inv_create(mode);

	if (lobjDesc == NULL)
	{
		MemoryContextSwitchTo(currentContext);
		PG_RETURN_OID(InvalidOid);
	}

	lobjId = lobjDesc->id;

	inv_close(lobjDesc);

	MemoryContextSwitchTo(currentContext);

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

	/*
	 * We assume we do not need to switch contexts for inv_tell. That is
	 * true for now, but is probably more than this module ought to
	 * assume...
	 */
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
		MemoryContext currentContext;
		int			i;

		currentContext = MemoryContextSwitchTo(fscxt);
		for (i = 0; i < cookies_size; i++)
		{
			if (cookies[i] != NULL && cookies[i]->id == lobjId)
			{
				inv_close(cookies[i]);
				deleteLOfd(i);
			}
		}
		MemoryContextSwitchTo(currentContext);
	}

	/*
	 * inv_drop does not need a context switch, indeed it doesn't touch
	 * any LO-specific data structures at all.	(Again, that's probably
	 * more than this module ought to be assuming.)
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
	lobj = inv_create(INV_READ | INV_WRITE);
	lobjOid = lobj->id;

	/*
	 * read in from the Unix file and write to the inversion file
	 */
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

	FileClose(fd);
	inv_close(lobj);

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

	/*
	 * open the inversion object (no need to test for failure)
	 */
	lobj = inv_open(lobjId, INV_READ);

	/*
	 * open the file to be written to
	 *
	 * Note: we reduce backend's normal 077 umask to the slightly friendlier
	 * 022.  This code used to drop it all the way to 0, but creating
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
	 * read in from the inversion file and write to the Unix file
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
 * lo_commit -
 *		 prepares large objects for transaction commit [PA, 7/17/98]
 */
void
lo_commit(bool isCommit)
{
	int			i;
	MemoryContext currentContext;

	if (fscxt == NULL)
		return;					/* no LO operations in this xact */

	currentContext = MemoryContextSwitchTo(fscxt);

	/*
	 * Clean out still-open index scans (not necessary if aborting) and
	 * clear cookies array so that LO fds are no longer good.
	 */
	for (i = 0; i < cookies_size; i++)
	{
		if (cookies[i] != NULL)
		{
			if (isCommit)
				inv_close(cookies[i]);
			cookies[i] = NULL;
		}
	}

	/* Needn't actually pfree since we're about to zap context */
	cookies = NULL;
	cookies_size = 0;

	MemoryContextSwitchTo(currentContext);

	/* Release the LO memory context to prevent permanent memory leaks. */
	MemoryContextDelete(fscxt);
	fscxt = NULL;
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
			palloc(newsize * sizeof(LargeObjectDesc *));
		MemSet(cookies, 0, newsize * sizeof(LargeObjectDesc *));
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
