/*-------------------------------------------------------------------------
 *
 * testlo64.c
 *	  test using large objects with libpq using 64-bit APIs
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/test/examples/testlo64.c
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
		fprintf(stderr, "can't open unix file\"%s\"\n", filename);
	}

	/*
	 * create the large object
	 */
	lobjId = lo_creat(conn, INV_READ | INV_WRITE);
	if (lobjId == 0)
		fprintf(stderr, "can't create large object");

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
pickout(PGconn *conn, Oid lobjId, pg_int64 start, int len)
{
	int			lobj_fd;
	char	   *buf;
	int			nbytes;
	int			nread;
	pg_int64		pos;

	lobj_fd = lo_open(conn, lobjId, INV_READ);
	if (lobj_fd < 0)
		fprintf(stderr, "can't open large object %u", lobjId);

	if (lo_tell64(conn, lobj_fd) < 0)
	{
		fprintf(stderr, "error lo_tell64: %s\n", PQerrorMessage(conn));
	}

	if ((pos=lo_lseek64(conn, lobj_fd, start, SEEK_SET)) < 0)
	{
		fprintf(stderr, "error lo_lseek64: %s\n", PQerrorMessage(conn));
		return;
	}

	fprintf(stderr, "before read: retval of lo_lseek64 : %lld\n", (long long int) pos);

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

	pos = lo_tell64(conn, lobj_fd);
	fprintf(stderr, "after read: retval of lo_tell64 : %lld\n\n", (long long int) pos);

	lo_close(conn, lobj_fd);
}

static void
overwrite(PGconn *conn, Oid lobjId, pg_int64 start, int len)
{
	int			lobj_fd;
	char	   *buf;
	int			nbytes;
	int			nwritten;
	int			i;
	pg_int64		pos;

	lobj_fd = lo_open(conn, lobjId, INV_READ | INV_WRITE);
	if (lobj_fd < 0)
		fprintf(stderr, "can't open large object %u", lobjId);

	if ((pos=lo_lseek64(conn, lobj_fd, start, SEEK_SET)) < 0)
	{
		fprintf(stderr, "error lo_lseek64: %s\n", PQerrorMessage(conn));
		return;
	}
	fprintf(stderr, "before write: retval of lo_lseek64 : %lld\n", (long long int) pos);

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

	pos = lo_tell64(conn, lobj_fd);
	fprintf(stderr, "after write: retval of lo_tell64 : %lld\n\n", (long long int) pos);

	lo_close(conn, lobj_fd);
}

static void
my_truncate(PGconn *conn, Oid lobjId, size_t len)
{
	int			lobj_fd;

	lobj_fd = lo_open(conn, lobjId, INV_READ | INV_WRITE);
	if (lobj_fd < 0)
		fprintf(stderr, "can't open large object %u", lobjId);

	if (lo_truncate64(conn, lobj_fd, len) < 0)
	{
		fprintf(stderr, "error lo_truncate64: %s\n", PQerrorMessage(conn));
		return;
	}


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
	 * create an inversion "object"
	 */
	lobj_fd = lo_open(conn, lobjId, INV_READ);
	if (lobj_fd < 0)
		fprintf(stderr, "can't open large object %u", lobjId);

	/*
	 * open the file to be written to
	 */
	fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if (fd < 0)
	{							/* error */
		fprintf(stderr, "can't open unix file\"%s\"",
				filename);
	}

	/*
	 * read in from the Unix file and write to the inversion file
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

	return;
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
			   *out_filename,
			   *out_filename2;
	char	   *database;
	Oid			lobjOid;
	PGconn	   *conn;
	PGresult   *res;

	if (argc != 5)
	{
		fprintf(stderr, "Usage: %s database_name in_filename out_filename out_filename2\n",
				argv[0]);
		exit(1);
	}

	database = argv[1];
	in_filename = argv[2];
	out_filename = argv[3];
	out_filename2 = argv[4];

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

		printf("picking out bytes 4294967000-4294968000 of the large object\n");
		pickout(conn, lobjOid, 4294967000ULL, 1000);

		printf("overwriting bytes 4294967000-4294968000 of the large object with X's\n");
		overwrite(conn, lobjOid, 4294967000ULL, 1000);


		printf("exporting large object to file \"%s\" ...\n", out_filename);
/*		exportFile(conn, lobjOid, out_filename); */
		if (!lo_export(conn, lobjOid, out_filename))
			fprintf(stderr, "%s\n", PQerrorMessage(conn));

		printf("truncating to 3294968000 byte\n");
		my_truncate(conn, lobjOid, 3294968000ULL);

		printf("exporting truncated large object to file \"%s\" ...\n", out_filename2);
		if (!lo_export(conn, lobjOid, out_filename2))
			fprintf(stderr, "%s\n", PQerrorMessage(conn));

	}

	res = PQexec(conn, "end");
	PQclear(res);
	PQfinish(conn);
	return 0;
}
