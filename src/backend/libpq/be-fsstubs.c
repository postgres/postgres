/*-------------------------------------------------------------------------
 *
 * be-fsstubs.c
 *	  support for filesystem operations on large objects
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/be-fsstubs.c,v 1.38 1999/07/15 23:03:13 momjian Exp $
 *
 * NOTES
 *	  This should be moved to a more appropriate place.  It is here
 *	  for lack of a better place.
 *
 *	  Builtin functions for open/close/read/write operations on large objects.
 *
 *	  These functions operate in a private GlobalMemoryContext, which means
 *	  that large object descriptors hang around until we destroy the context.
 *	  That happens in lo_commit().  It'd be possible to prolong the lifetime
 *	  of the context so that LO FDs are good across transactions (for example,
 *	  we could release the context only if we see that no FDs remain open).
 *	  But we'd need additional state in order to do the right thing at the
 *	  end of an aborted transaction.  FDs opened during an aborted xact would
 *	  still need to be closed, since they might not be pointing at valid
 *	  relations at all.  For now, we'll stick with the existing documented
 *	  semantics of LO FDs: they're only good within a transaction.
 *
 *-------------------------------------------------------------------------
 */

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "postgres.h"

#include "libpq/libpq-fs.h"
#include <catalog/pg_shadow.h>	/* for superuser() */
#include "storage/large_object.h"
#include "libpq/be-fsstubs.h"

/* [PA] is Pascal André <andre@via.ecp.fr> */

/*#define FSDB 1*/
#define MAX_LOBJ_FDS	256
#define BUFSIZE			1024
#define FNAME_BUFSIZE	8192

static LargeObjectDesc *cookies[MAX_LOBJ_FDS];

static GlobalMemory fscxt = NULL;


static int	newLOfd(LargeObjectDesc *lobjCookie);
static void deleteLOfd(int fd);

/*****************************************************************************
 *	File Interfaces for Large Objects
 *****************************************************************************/

int
lo_open(Oid lobjId, int mode)
{
	LargeObjectDesc *lobjDesc;
	int			fd;
	MemoryContext currentContext;

#if FSDB
	elog(NOTICE, "LOopen(%u,%d)", lobjId, mode);
#endif

	if (fscxt == NULL)
		fscxt = CreateGlobalMemory("Filesystem");
	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	lobjDesc = inv_open(lobjId, mode);

	if (lobjDesc == NULL)
	{							/* lookup failed */
		MemoryContextSwitchTo(currentContext);
#if FSDB
		elog(NOTICE, "cannot open large object %u", lobjId);
#endif
		return -1;
	}

	fd = newLOfd(lobjDesc);

	/* switch context back to orig. */
	MemoryContextSwitchTo(currentContext);

#if FSDB
	if (fd < 0)					/* newLOfd couldn't find a slot */
		elog(NOTICE, "Out of space for large object FDs");
#endif

	return fd;
}

int
lo_close(int fd)
{
	MemoryContext currentContext;

	if (fd < 0 || fd >= MAX_LOBJ_FDS)
	{
		elog(ERROR, "lo_close: large obj descriptor (%d) out of range", fd);
		return -2;
	}
	if (cookies[fd] == NULL)
	{
		elog(ERROR, "lo_close: invalid large obj descriptor (%d)", fd);
		return -3;
	}
#if FSDB
	elog(NOTICE, "LOclose(%d)", fd);
#endif

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	inv_close(cookies[fd]);

	MemoryContextSwitchTo(currentContext);

	deleteLOfd(fd);
	return 0;
}

/*
 *	We assume the large object supports byte oriented reads and seeks so
 *	that our work is easier.
 */
int
lo_read(int fd, char *buf, int len)
{
	MemoryContext currentContext;
	int			status;

	if (fd < 0 || fd >= MAX_LOBJ_FDS)
	{
		elog(ERROR, "lo_read: large obj descriptor (%d) out of range", fd);
		return -2;
	}
	if (cookies[fd] == NULL)
	{
		elog(ERROR, "lo_read: invalid large obj descriptor (%d)", fd);
		return -3;
	}

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	status = inv_read(cookies[fd], buf, len);

	MemoryContextSwitchTo(currentContext);
	return (status);
}

int
lo_write(int fd, char *buf, int len)
{
	MemoryContext currentContext;
	int			status;

	if (fd < 0 || fd >= MAX_LOBJ_FDS)
	{
		elog(ERROR, "lo_write: large obj descriptor (%d) out of range", fd);
		return -2;
	}
	if (cookies[fd] == NULL)
	{
		elog(ERROR, "lo_write: invalid large obj descriptor (%d)", fd);
		return -3;
	}

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	status = inv_write(cookies[fd], buf, len);

	MemoryContextSwitchTo(currentContext);
	return (status);
}


int
lo_lseek(int fd, int offset, int whence)
{
	MemoryContext currentContext;
	int			status;

	if (fd < 0 || fd >= MAX_LOBJ_FDS)
	{
		elog(ERROR, "lo_lseek: large obj descriptor (%d) out of range", fd);
		return -2;
	}
	if (cookies[fd] == NULL)
	{
		elog(ERROR, "lo_lseek: invalid large obj descriptor (%d)", fd);
		return -3;
	}

	Assert(fscxt != NULL);
	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	status = inv_seek(cookies[fd], offset, whence);

	MemoryContextSwitchTo(currentContext);

	return status;
}

Oid
lo_creat(int mode)
{
	LargeObjectDesc *lobjDesc;
	MemoryContext currentContext;
	Oid			lobjId;

	if (fscxt == NULL)
		fscxt = CreateGlobalMemory("Filesystem");

	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	lobjDesc = inv_create(mode);

	if (lobjDesc == NULL)
	{
		MemoryContextSwitchTo(currentContext);
		return InvalidOid;
	}

	lobjId = RelationGetRelid(lobjDesc->heap_r);

	inv_close(lobjDesc);

	/* switch context back to original memory context */
	MemoryContextSwitchTo(currentContext);

	return lobjId;
}

int
lo_tell(int fd)
{
	if (fd < 0 || fd >= MAX_LOBJ_FDS)
	{
		elog(ERROR, "lo_tell: large object descriptor (%d) out of range", fd);
		return -2;
	}
	if (cookies[fd] == NULL)
	{
		elog(ERROR, "lo_tell: invalid large object descriptor (%d)", fd);
		return -3;
	}

	/*
	 * We assume we do not need to switch contexts for inv_tell.
	 * That is true for now, but is probably more than this module
	 * ought to assume...
	 */
	return inv_tell(cookies[fd]);
}

int
lo_unlink(Oid lobjId)
{
	/*
	 * inv_destroy does not need a context switch, indeed it doesn't
	 * touch any LO-specific data structures at all.  (Again, that's
	 * probably more than this module ought to be assuming.)
	 *
	 * XXX there ought to be some code to clean up any open LOs that
	 * reference the specified relation... as is, they remain "open".
	 */
	return inv_destroy(lobjId);
}

/*****************************************************************************
 *	Read/Write using varlena
 *****************************************************************************/

struct varlena *
loread(int fd, int len)
{
	struct varlena *retval;
	int			totalread = 0;

	retval = (struct varlena *) palloc(VARHDRSZ + len);
	totalread = lo_read(fd, VARDATA(retval), len);
	VARSIZE(retval) = totalread + VARHDRSZ;

	return retval;
}

int
lowrite(int fd, struct varlena * wbuf)
{
	int			totalwritten;
	int			bytestowrite;

	bytestowrite = VARSIZE(wbuf) - VARHDRSZ;
	totalwritten = lo_write(fd, VARDATA(wbuf), bytestowrite);
	return totalwritten;
}

/*****************************************************************************
 *	 Import/Export of Large Object
 *****************************************************************************/

/*
 * lo_import -
 *	  imports a file as an (inversion) large object.
 */
Oid
lo_import(text *filename)
{
	File		fd;
	int			nbytes,
				tmp;
	char		buf[BUFSIZE];
	char		fnamebuf[FNAME_BUFSIZE];
	LargeObjectDesc *lobj;
	Oid			lobjOid;

#ifndef ALLOW_DANGEROUS_LO_FUNCTIONS
	if (!superuser())
		elog(ERROR, "You must have Postgres superuser privilege to use "
			 "server-side lo_import().\n\tAnyone can use the "
			 "client-side lo_import() provided by libpq.");
#endif

	/*
	 * open the file to be read in
	 */
	nbytes = VARSIZE(filename) - VARHDRSZ + 1;
	if (nbytes > FNAME_BUFSIZE)
		nbytes = FNAME_BUFSIZE;
	StrNCpy(fnamebuf, VARDATA(filename), nbytes);
#ifndef __CYGWIN32__
	fd = PathNameOpenFile(fnamebuf, O_RDONLY, 0666);
#else
	fd = PathNameOpenFile(fnamebuf, O_RDONLY | O_BINARY, 0666);
#endif
	if (fd < 0)
	{							/* error */
		elog(ERROR, "lo_import: can't open unix file \"%s\": %m",
			 fnamebuf);
	}

	/*
	 * create an inversion "object"
	 */
	lobj = inv_create(INV_READ | INV_WRITE);
	if (lobj == NULL)
	{
		elog(ERROR, "lo_import: can't create inv object for \"%s\"",
			 fnamebuf);
	}

	/*
	 * the oid for the large object is just the oid of the relation
	 * XInv??? which contains the data.
	 */
	lobjOid = RelationGetRelid(lobj->heap_r);

	/*
	 * read in from the Unix file and write to the inversion file
	 */
	while ((nbytes = FileRead(fd, buf, BUFSIZE)) > 0)
	{
		tmp = inv_write(lobj, buf, nbytes);
		if (tmp < nbytes)
			elog(ERROR, "lo_import: error while reading \"%s\"",
				 fnamebuf);
	}

	FileClose(fd);
	inv_close(lobj);

	return lobjOid;
}

/*
 * lo_export -
 *	  exports an (inversion) large object.
 */
int4
lo_export(Oid lobjId, text *filename)
{
	File		fd;
	int			nbytes,
				tmp;
	char		buf[BUFSIZE];
	char		fnamebuf[FNAME_BUFSIZE];
	LargeObjectDesc *lobj;
	mode_t		oumask;

#ifndef ALLOW_DANGEROUS_LO_FUNCTIONS
	if (!superuser())
		elog(ERROR, "You must have Postgres superuser privilege to use "
			 "server-side lo_export().\n\tAnyone can use the "
			 "client-side lo_export() provided by libpq.");
#endif

	/*
	 * open the inversion "object"
	 */
	lobj = inv_open(lobjId, INV_READ);
	if (lobj == NULL)
		elog(ERROR, "lo_export: can't open inv object %u", lobjId);

	/*
	 * open the file to be written to
	 *
	 * Note: we reduce backend's normal 077 umask to the slightly
	 * friendlier 022.  This code used to drop it all the way to 0,
	 * but creating world-writable export files doesn't seem wise.
	 */
	nbytes = VARSIZE(filename) - VARHDRSZ + 1;
	if (nbytes > FNAME_BUFSIZE)
		nbytes = FNAME_BUFSIZE;
	StrNCpy(fnamebuf, VARDATA(filename), nbytes);
	oumask = umask((mode_t) 0022);
#ifndef __CYGWIN32__
	fd = PathNameOpenFile(fnamebuf, O_CREAT | O_WRONLY | O_TRUNC, 0666);
#else
	fd = PathNameOpenFile(fnamebuf, O_CREAT | O_WRONLY | O_TRUNC | O_BINARY, 0666);
#endif
	umask(oumask);
	if (fd < 0)
	{							/* error */
		elog(ERROR, "lo_export: can't open unix file \"%s\": %m",
			 fnamebuf);
	}

	/*
	 * read in from the Unix file and write to the inversion file
	 */
	while ((nbytes = inv_read(lobj, buf, BUFSIZE)) > 0)
	{
		tmp = FileWrite(fd, buf, nbytes);
		if (tmp < nbytes)
			elog(ERROR, "lo_export: error while writing \"%s\"",
				 fnamebuf);
	}

	inv_close(lobj);
	FileClose(fd);

	return 1;
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

	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	/* Clean out still-open index scans (not necessary if aborting)
	 * and clear cookies array so that LO fds are no longer good.
	 */
	for (i = 0; i < MAX_LOBJ_FDS; i++)
	{
		if (cookies[i] != NULL)
		{
			if (isCommit)
				inv_cleanindex(cookies[i]);
			cookies[i] = NULL;
		}
	}

	MemoryContextSwitchTo(currentContext);

	/* Release the LO memory context to prevent permanent memory leaks. */
	GlobalMemoryDestroy(fscxt);
	fscxt = NULL;
}


/*****************************************************************************
 *	Support routines for this file
 *****************************************************************************/

static int
newLOfd(LargeObjectDesc *lobjCookie)
{
	int			i;

	for (i = 0; i < MAX_LOBJ_FDS; i++)
	{

		if (cookies[i] == NULL)
		{
			cookies[i] = lobjCookie;
			return i;
		}
	}
	return -1;
}

static void
deleteLOfd(int fd)
{
	cookies[fd] = NULL;
}
