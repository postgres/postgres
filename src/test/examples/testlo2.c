/*-------------------------------------------------------------------------
 *
 * lotest.c--
 *    test using large objects with libpq
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/test/examples/Attic/testlo2.c,v 1.1.1.1 1996/07/09 06:22:23 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#define BUFSIZE		1024

/*
 * importFile -
 *    import file "in_filename" into database as large object "lobjOid"
 *
 */
Oid importFile(PGconn *conn, char *filename)
{
    Oid lobjId;
    int lobj_fd;
    char buf[BUFSIZE];
    int nbytes, tmp;
    int fd;

    /*
     * open the file to be read in
     */
    fd = open(filename, O_RDONLY, 0666);
    if (fd < 0)  {   /* error */
	fprintf(stderr, "can't open unix file\"%s\"\n", filename);
    }

    /*
     * create the large object
     */
    lobjId = lo_creat(conn, INV_READ|INV_WRITE);
    if (lobjId == 0) {
	fprintf(stderr, "can't create large object");
    }
    
    lobj_fd = lo_open(conn, lobjId, INV_WRITE);
    /*
     * read in from the Unix file and write to the inversion file
     */
    while ((nbytes = read(fd, buf, BUFSIZE)) > 0) {
	tmp = lo_write(conn, lobj_fd, buf, nbytes);
	if (tmp < nbytes) {
	    fprintf(stderr, "error while reading \"%s\"", filename);
	}
    }
    
    (void) close(fd);
    (void) lo_close(conn, lobj_fd);

    return lobjId;
}

void pickout(PGconn *conn, Oid lobjId, int start, int len)
{
    int lobj_fd;
    char* buf;
    int nbytes;
    int nread;

    lobj_fd = lo_open(conn, lobjId, INV_READ);
    if (lobj_fd < 0) {
	fprintf(stderr,"can't open large object %d",
		lobjId);
    }

    lo_lseek(conn, lobj_fd, start, SEEK_SET);
    buf = malloc(len+1);
    
    nread = 0;
    while (len - nread > 0) {
	nbytes = lo_read(conn, lobj_fd, buf, len - nread);
	buf[nbytes] = '\0';
	fprintf(stderr,">>> %s", buf);
	nread += nbytes;
    }
    fprintf(stderr,"\n");
    lo_close(conn, lobj_fd);
}

void overwrite(PGconn *conn, Oid lobjId, int start, int len)
{
    int lobj_fd;
    char* buf;
    int nbytes;
    int nwritten;
    int i;

    lobj_fd = lo_open(conn, lobjId, INV_READ);
    if (lobj_fd < 0) {
	fprintf(stderr,"can't open large object %d",
		lobjId);
    }

    lo_lseek(conn, lobj_fd, start, SEEK_SET);
    buf = malloc(len+1);
    
    for (i=0;i<len;i++)
	buf[i] = 'X';
    buf[i] = '\0';

    nwritten = 0;
    while (len - nwritten > 0) {
	nbytes = lo_write(conn, lobj_fd, buf + nwritten, len - nwritten);
	nwritten += nbytes;
    }
    fprintf(stderr,"\n");
    lo_close(conn, lobj_fd);
}


/*
 * exportFile -
 *    export large object "lobjOid" to file "out_filename"
 *
 */
void exportFile(PGconn *conn, Oid lobjId, char *filename)
{
    int lobj_fd;
    char buf[BUFSIZE];
    int nbytes, tmp;
    int fd;

    /*
     * create an inversion "object"
     */
    lobj_fd = lo_open(conn, lobjId, INV_READ);
    if (lobj_fd < 0) {
	fprintf(stderr,"can't open large object %d",
		lobjId);
    }

    /*
     * open the file to be written to
     */
    fd = open(filename, O_CREAT|O_WRONLY, 0666);
    if (fd < 0)  {   /* error */
	fprintf(stderr, "can't open unix file\"%s\"",
		filename);
    }

    /*
     * read in from the Unix file and write to the inversion file
     */
    while ((nbytes = lo_read(conn, lobj_fd, buf, BUFSIZE)) > 0) {
	tmp = write(fd, buf, nbytes);
        if (tmp < nbytes) {
	    fprintf(stderr,"error while writing \"%s\"",
		    filename);
	}
    }

    (void) lo_close(conn, lobj_fd);
    (void) close(fd);

    return;
}

void 
exit_nicely(PGconn* conn)
{
  PQfinish(conn);
  exit(1);
}

int
main(int argc, char **argv)
{
    char *in_filename, *out_filename;
    char *database;
    Oid lobjOid;
    PGconn *conn;
    PGresult *res;

    if (argc != 4) {
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
    if (PQstatus(conn) == CONNECTION_BAD) {
	fprintf(stderr,"Connection to database '%s' failed.\n", database);
	fprintf(stderr,"%s",PQerrorMessage(conn));
	exit_nicely(conn);
    }
	
    res = PQexec(conn, "begin");
    PQclear(res);

    printf("importing file \"%s\" ...\n", in_filename);
/*    lobjOid = importFile(conn, in_filename); */
    lobjOid = lo_import(conn, in_filename);
/*
    printf("\tas large object %d.\n", lobjOid);

    printf("picking out bytes 1000-2000 of the large object\n");
    pickout(conn, lobjOid, 1000, 1000);

    printf("overwriting bytes 1000-2000 of the large object with X's\n");
    overwrite(conn, lobjOid, 1000, 1000);
*/

    printf("exporting large object to file \"%s\" ...\n", out_filename);
/*    exportFile(conn, lobjOid, out_filename); */
    lo_export(conn, lobjOid,out_filename);

    res = PQexec(conn, "end");
    PQclear(res);
    PQfinish(conn);
    exit(0);
}
