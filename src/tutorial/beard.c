/*-------------------------------------------------------------------------
 *
 * beard.c
 *	  sample routines to use large objects
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/tutorial/Attic/beard.c,v 1.2 1999/02/13 23:22:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

typedef struct ImageHdr
{
	int			size;
}			ImageHdr;

#define BUFSIZE 10

/*
 * beard -
 *	 clips lower 1/3 of picture and return as large object
 */
Oid
beard(Oid picture)
{
	Oid			beard;
	int			pic_fd,
				beard_fd;
	ImageHdr	ihdr;
	char		buf[BUFSIZE];
	int			cc;

	if ((pic_fd = lo_open(picture, INV_READ)) == -1)
		elog(ERROR, "Cannot access picture large object");

	if (lo_read(pic_fd, (char *) &ihdr, sizeof(ihdr)) != sizeof(ihdr))
		elog(ERROR, "Picture large object corrupted");

	beardOffset = (ihdr.size / 3) * 2;

	/*
	 * new large object
	 */
	if ((beard = lo_creat(INV_MD)) == 0)		/* ?? is this right? */
		elog(ERROR, "Cannot create new large object");

	if ((beard_fd = lo_open(beard, INV_WRITE)) == -1)
		elog(ERROR, "Cannot access beard large object");

	lo_lseek(pic_fd, beardOffset, SET_CUR);
	while ((cc = lo_read(pic_fd, buf, BUFSIZE)) > 0)
	{
		if (lo_write(beard_fd, buf, cc) != cc)
			elog(ERROR, "error while writing large object");
	}

	lo_close(pic_fd);
	lo_close(beard_fd);

	return beard;
}
