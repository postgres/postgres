/*-------------------------------------------------------------------------
 *
 * be-fsstubs.c--
 *	  support for filesystem operations on large objects
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/be-fsstubs.c,v 1.17 1997/12/08 04:42:45 momjian Exp $
 *
 * NOTES
 *	  This should be moved to a more appropriate place.  It is here
 *	  for lack of a better place.
 *
 *	  Builtin functions for open/close/read/write operations on large objects.
 *
 *	  These functions operate in the current portal variable context, which
 *	  means the large object descriptors hang around between transactions and
 *	  are not deallocated until explicitly closed, or until the portal is
 *	  closed.
 *-------------------------------------------------------------------------
 */

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <postgres.h>

#include <lib/dllist.h>
#include <libpq/libpq.h>
#include <libpq/libpq-fs.h>
#include <nodes/nodes.h>
#include <utils/memutils.h>
#include <lib/fstack.h>
#include <utils/mcxt.h>
#include <storage/fd.h>			/* for O_ */
#include <storage/large_object.h>
#include <libpq/be-fsstubs.h>

/*#define FSDB 1*/
#define MAX_LOBJ_FDS 256

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
	elog(NOTICE, "LOopen(%d,%d)", lobjId, mode);
#endif

	if (fscxt == NULL)
	{
		fscxt = CreateGlobalMemory("Filesystem");
	}
	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	lobjDesc = inv_open(lobjId, mode);

	if (lobjDesc == NULL)
	{							/* lookup failed */
		MemoryContextSwitchTo(currentContext);
#if FSDB
		elog(NOTICE, "cannot open large object %d", lobjId);
#endif
		return -1;
	}

	fd = newLOfd(lobjDesc);

	/* switch context back to orig. */
	MemoryContextSwitchTo(currentContext);

	return fd;
}

int
lo_close(int fd)
{
	MemoryContext currentContext;

	if (fd >= MAX_LOBJ_FDS)
	{
		elog(WARN, "lo_close: large obj descriptor (%d) out of range", fd);
		return -2;
	}
	if (cookies[fd] == NULL)
	{
		elog(WARN, "lo_close: invalid large obj descriptor (%d)", fd);
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
	Assert(cookies[fd] != NULL);
	return inv_read(cookies[fd], buf, len);
}

int
lo_write(int fd, char *buf, int len)
{
	Assert(cookies[fd] != NULL);
	return inv_write(cookies[fd], buf, len);
}


int
lo_lseek(int fd, int offset, int whence)
{
	MemoryContext currentContext;
	int			ret;

	if (fd >= MAX_LOBJ_FDS)
	{
		elog(WARN, "lo_seek: large obj descriptor (%d) out of range", fd);
		return -2;
	}

	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	ret = inv_seek(cookies[fd], offset, whence);

	MemoryContextSwitchTo(currentContext);

	return ret;
}

Oid
lo_creat(int mode)
{
	LargeObjectDesc *lobjDesc;
	MemoryContext currentContext;
	Oid			lobjId;

	if (fscxt == NULL)
	{
		fscxt = CreateGlobalMemory("Filesystem");
	}

	currentContext = MemoryContextSwitchTo((MemoryContext) fscxt);

	lobjDesc = inv_create(mode);

	if (lobjDesc == NULL)
	{
		MemoryContextSwitchTo(currentContext);
		return InvalidOid;
	}

	lobjId = lobjDesc->heap_r->rd_id;

	inv_close(lobjDesc);

	/* switch context back to original memory context */
	MemoryContextSwitchTo(currentContext);

	return lobjId;
}

int
lo_tell(int fd)
{
	if (fd >= MAX_LOBJ_FDS)
	{
		elog(WARN, "lo_tell: large object descriptor (%d) out of range", fd);
		return -2;
	}
	if (cookies[fd] == NULL)
	{
		elog(WARN, "lo_tell: invalid large object descriptor (%d)", fd);
		return -3;
	}
	return inv_tell(cookies[fd]);
}

int
lo_unlink(Oid lobjId)
{
	return (inv_destroy(lobjId));
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
	int			fd;
	int			nbytes,
				tmp;

#define BUFSIZE		   1024
	char		buf[BUFSIZE];
	char		fnamebuf[8192];
	LargeObjectDesc *lobj;
	Oid			lobjOid;

	/*
	 * open the file to be read in
	 */
	StrNCpy(fnamebuf, VARDATA(filename), VARSIZE(filename) - VARHDRSZ + 1);
	fd = open(fnamebuf, O_RDONLY, 0666);
	if (fd < 0)
	{							/* error */
		elog(WARN, "be_lo_import: can't open unix file\"%s\"\n",
			 fnamebuf);
	}

	/*
	 * create an inversion "object"
	 */
	lobj = inv_create(INV_READ | INV_WRITE);
	if (lobj == NULL)
	{
		elog(WARN, "lo_import: can't create inv object for \"%s\"",
			 fnamebuf);
	}

	/*
	 * the oid for the large object is just the oid of the relation
	 * XInv??? which contains the data.
	 */
	lobjOid = lobj->heap_r->rd_id;

	/*
	 * read in from the Unix file and write to the inversion file
	 */
	while ((nbytes = read(fd, buf, BUFSIZE)) > 0)
	{
		tmp = inv_write(lobj, buf, nbytes);
		if (tmp < nbytes)
		{
			elog(WARN, "lo_import: error while reading \"%s\"",
				 fnamebuf);
		}
	}

	close(fd);
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
	int			fd;
	int			nbytes,
				tmp;

#define BUFSIZE		   1024
	char		buf[BUFSIZE];
	char		fnamebuf[8192];
	LargeObjectDesc *lobj;
	mode_t		oumask;

	/*
	 * create an inversion "object"
	 */
	lobj = inv_open(lobjId, INV_READ);
	if (lobj == NULL)
	{
		elog(WARN, "lo_export: can't open inv object %d",
			 lobjId);
	}

	/*
	 * open the file to be written to
	 */
	oumask = umask((mode_t) 0);
	StrNCpy(fnamebuf, VARDATA(filename), VARSIZE(filename) - VARHDRSZ + 1);
	fd = open(fnamebuf, O_CREAT | O_WRONLY, 0666);
	umask(oumask);
	if (fd < 0)
	{							/* error */
		elog(WARN, "lo_export: can't open unix file\"%s\"",
			 fnamebuf);
	}

	/*
	 * read in from the Unix file and write to the inversion file
	 */
	while ((nbytes = inv_read(lobj, buf, BUFSIZE)) > 0)
	{
		tmp = write(fd, buf, nbytes);
		if (tmp < nbytes)
		{
			elog(WARN, "lo_export: error while writing \"%s\"",
				 fnamebuf);
		}
	}

	inv_close(lobj);
	close(fd);

	return 1;
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
