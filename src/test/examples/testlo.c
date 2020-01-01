/*-------------------------------------------------------------------------
 *
 * testlo.c
 *	  test using large objects with libpq
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/test/examples/testlo.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#define BUFSIZE			1024

/*
 * importFile -
 *	  import file "in_filename" into database as large object "lobjOid"
 *
 */
static Oid
importFile(PGconn *conn, char *filename)
{
	Oid			lobjId;
	int			lobj_fd;
	char		buf[BUFSIZE];
	int			nbytes,
				tmp;
	int			fd;

	/*
	 * open the file to be read in
	 */
	fd = open(filename, O_RDONLY, 0666);
	if (fd < 0)
	{							/* error */
		fprintf(stderr, "cannot open unix file\"%s\"\n", filename);
	}

	/*
	 * create the large object
	 */
	lobjId = lo_creat(conn, INV_READ | INV_WRITE);
	if (lobjId == 0)
		fprintf(stderr, "cannot create large object");

	lobj_fd = lo_open(conn, lobjId, INV_WRITE);

	/*
	 * read in from the Unix file and write to the inversion file
	 */
	while ((nbytes = read(fd, buf, BUFSIZE)) > 0)
	{
		tmp = lo_write(conn, lobj_fd, buf, nbytes);
		if (tmp < nbytes)
			fprintf(stderr, "error while reading \"%s\"", filename);
	}

	close(fd);
	lo_close(conn, lobj_fd);

	return lobjId;
}

static void
pickout(PGconn *conn, Oid lobjId, int start, int len)
{
	int			lobj_fd;
	char	   *buf;
	int			nbytes;
	int			nread;

	lobj_fd = lo_open(conn, lobjId, INV_READ);
	if (lobj_fd < 0)
		fprintf(stderr, "cannot open large object %u", lobjId);

	lo_lseek(conn, lobj_fd, start, SEEK_SET);
	buf = malloc(len + 1);

	nread = 0;
	while (len - nread > 0)
	{
		nbytes = lo_read(conn, lobj_fd, buf, len - nread);
		buf[nbytes] = '\0';
		fprintf(stderr, ">>> %s", buf);
		nread += nbytes;
		if (nbytes <= 0)
			break;				/* no more data? */
	}
	free(buf);
	fprintf(stderr, "\n");
	lo_close(conn, lobj_fd);
}

static void
overwrite(PGconn *conn, Oid lobjId, int start, int len)
{
	int			lobj_fd;
	char	   *buf;
	int			nbytes;
	int			nwritten;
	int			i;

	lobj_fd = lo_open(conn, lobjId, INV_WRITE);
	if (lobj_fd < 0)
		fprintf(stderr, "cannot open large object %u", lobjId);

	lo_lseek(conn, lobj_fd, start, SEEK_SET);
	buf = malloc(len + 1);

	for (i = 0; i < len; i++)
		buf[i] = 'X';
	buf[i] = '\0';

	nwritten = 0;
	while (len - nwritten > 0)
	{
		nbytes = lo_write(conn, lobj_fd, buf + nwritten, len - nwritten);
		nwritten += nbytes;
		if (nbytes <= 0)
		{
			fprintf(stderr, "\nWRITE FAILED!\n");
			break;
		}
	}
	free(buf);
	fprintf(stderr, "\n");
	lo_close(conn, lobj_fd);
}


/*
 * exportFile -
 *	  export large object "lobjOid" to file "out_filename"
 *
 */
static void
exportFile(PGconn *conn, Oid lobjId, char *filename)
{
	int			lobj_fd;
	char		buf[BUFSIZE];
	int			nbytes,
				tmp;
	int			fd;

	/*
	 * open the large object
	 */
	lobj_fd = lo_open(conn, lobjId, INV_READ);
	if (lobj_fd < 0)
		fprintf(stderr, "cannot open large object %u", lobjId);

	/*
	 * open the file to be written to
	 */
	fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if (fd < 0)
	{							/* error */
		fprintf(stderr, "cannot open unix file\"%s\"",
				filename);
	}

	/*
	 * read in from the inversion file and write to the Unix file
	 */
	while ((nbytes = lo_read(conn, lobj_fd, buf, BUFSIZE)) > 0)
	{
		tmp = write(fd, buf, nbytes);
		if (tmp < nbytes)
		{
			fprintf(stderr, "error while writing \"%s\"",
					filename);
		}
	}

	lo_close(conn, lobj_fd);
	close(fd);
}

static void
exit_nicely(PGconn *conn)
{
	PQfinish(conn);
	exit(1);
}

int
main(int argc, char **argv)
{
	char	   *in_filename,
			   *out_filename;
	char	   *database;
	Oid			lobjOid;
	PGconn	   *conn;
	PGresult   *res;

	if (argc != 4)
	{
		fprintf(stderr, "Usage: %s database_name in_filename out_filename\n",
				argv[0]);
		exit(1);
	}

	database = argv[1];
	in_filename = argv[2];
	out_filename = argv[3];

	/*
	 * set up the connection
	 */
	conn = PQsetdb(NULL, NULL, NULL, NULL, database);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s",
				PQerrorMessage(conn));
		exit_nicely(conn);
	}

	/* Set always-secure search path, so malicious users can't take control. */
	res = PQexec(conn,
				 "SELECT pg_catalog.set_config('search_path', '', false)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "SET failed: %s", PQerrorMessage(conn));
		PQclear(res);
		exit_nicely(conn);
	}
	PQclear(res);

	res = PQexec(conn, "begin");
	PQclear(res);
	printf("importing file \"%s\" ...\n", in_filename);
/*	lobjOid = importFile(conn, in_filename); */
	lobjOid = lo_import(conn, in_filename);
	if (lobjOid == 0)
		fprintf(stderr, "%s\n", PQerrorMessage(conn));
	else
	{
		printf("\tas large object %u.\n", lobjOid);

		printf("picking out bytes 1000-2000 of the large object\n");
		pickout(conn, lobjOid, 1000, 1000);

		printf("overwriting bytes 1000-2000 of the large object with X's\n");
		overwrite(conn, lobjOid, 1000, 1000);

		printf("exporting large object to file \"%s\" ...\n", out_filename);
/*		exportFile(conn, lobjOid, out_filename); */
		if (lo_export(conn, lobjOid, out_filename) < 0)
			fprintf(stderr, "%s\n", PQerrorMessage(conn));
	}

	res = PQexec(conn, "end");
	PQclear(res);
	PQfinish(conn);
	return 0;
}
