/*-------------------------------------------------------------------------
 *
 * beard.c
 *	  sample routines to use large objects
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/tutorial/beard.c,v 1.13 2005/10/15 02:49:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

typedef struct ImageHdr
{
	int			size;
}	ImageHdr;

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

	pic_fd = DatumGetInt32(DirectFunctionCall2(lo_open,
											   ObjectIdGetDatum(picture),
											   Int32GetDatum(INV_READ)));
	if (pic_fd < 0)
		elog(ERROR, "Cannot access picture large object");

	if (lo_read(pic_fd, (char *) &ihdr, sizeof(ihdr)) != sizeof(ihdr))
		elog(ERROR, "Picture large object corrupted");

	beardOffset = (ihdr.size / 3) * 2;

	/*
	 * new large object
	 */
	beard = DatumGetObjectId(DirectFunctionCall1(lo_creat,
												 Int32GetDatum(INV_MD)));
	if (beard == InvalidOid)
		elog(ERROR, "Cannot create new large object");

	beard_fd = DatumGetInt32(DirectFunctionCall2(lo_open,
												 ObjectIdGetDatum(beard),
												 Int32GetDatum(INV_WRITE)));
	if (beard_fd < 0)
		elog(ERROR, "Cannot access beard large object");

	if (DatumGetInt32(DirectFunctionCall3(lo_lseek,
										  Int32GetDatum(pic_fd),
										  Int32GetDatum(beardOffset),
										  Int32GetDatum(SEEK_SET))) < 0)
		elog(ERROR, "Cannot seek in picture large object");

	while ((cc = lo_read(pic_fd, buf, BUFSIZE)) > 0)
	{
		if (lo_write(beard_fd, buf, cc) != cc)
			elog(ERROR, "error while writing large object");
	}

	DirectFunctionCall1(lo_close, Int32GetDatum(pic_fd));
	DirectFunctionCall1(lo_close, Int32GetDatum(beard_fd));

	return beard;
}
