/*-------------------------------------------------------------------------
 *
 * fe-lobj.c--
 *    Front-end large object interface
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-lobj.c,v 1.6 1996/12/28 02:13:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "postgres.h"
#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#define LO_BUFSIZE        1024

static int lo_initialize(PGconn *conn);

/*
 * lo_open
 *    opens an existing large object
 *
 * returns the file descriptor for use in later lo_* calls
 * return -1 upon failure.
 */
int
lo_open(PGconn* conn, Oid lobjId, int mode)
{
    int fd;
    int result_len;
    PQArgBlock argv[2];
    PGresult *res;

    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = lobjId;

    argv[1].isint = 1;
    argv[1].len = 4;
    argv[1].u.integer = mode;

    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    res = PQfn(conn, conn->lobjfuncs->fn_lo_open,&fd,&result_len,1,argv,2); 
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);

	/* have to do this to reset offset in shared fd cache */
	/* but only if fd is valid */
	if (fd >= 0 && lo_lseek(conn, fd, 0L, SEEK_SET) < 0)
	    return -1;
	return fd;
    } else
	return -1;
}

/*
 * lo_close
 *    closes an existing large object
 *
 * returns 0 upon success
 * returns -1 upon failure.
 */
int
lo_close(PGconn *conn, int fd)
{
    PQArgBlock argv[1];
    PGresult *res;
    int retval;
    int result_len;

    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = fd;
    res = PQfn(conn, conn->lobjfuncs->fn_lo_close,
    		&retval,&result_len,1,argv,1);
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);
	return retval;
    } else
	return -1;
}

/*
 * lo_read
 *    read len bytes of the large object into buf
 *
 * returns the length of bytes read.
 * the CALLER must have allocated enough space to hold the result returned
 */

int
lo_read(PGconn *conn, int fd, char *buf, int len)
{
    PQArgBlock argv[2];
    PGresult *res;
    int result_len;

    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = fd;

    argv[1].isint = 1;
    argv[1].len = 4;
    argv[1].u.integer = len;

    res = PQfn(conn, conn->lobjfuncs->fn_lo_read,
    		(int*)buf,&result_len,0,argv,2);
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);
	return result_len;
    } else
	return -1;
}

/*
 * lo_write
 *    write len bytes of buf into the large object fd
 *
 */
int
lo_write(PGconn *conn, int fd, char *buf, int len)
{
    PQArgBlock argv[2];
    PGresult *res;
    int result_len;
    int retval;

    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    if (len <= 0)
	return 0;

    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = fd;

    argv[1].isint = 0;
    argv[1].len = len;
    argv[1].u.ptr = (int*)buf;

    res = PQfn(conn, conn->lobjfuncs->fn_lo_write,
    		&retval,&result_len,1,argv,2);
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);
	return retval;
    } else
	return -1;
}

/*
 * lo_lseek
 *    change the current read or write location on a large object
 * currently, only L_SET is a legal value for whence
 *
 */

int
lo_lseek(PGconn *conn, int fd, int offset, int whence)
{
    PQArgBlock argv[3];
    PGresult *res;
    int retval; 
    int result_len;
    
    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = fd;
    
    argv[1].isint = 1;
    argv[1].len = 4;
    argv[1].u.integer = offset;

    argv[2].isint = 1;
    argv[2].len = 4;
    argv[2].u.integer = whence;

    res = PQfn(conn, conn->lobjfuncs->fn_lo_lseek,
    		&retval,&result_len,1,argv,3);
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);
	return retval;
    } else
	return -1;
}

/*
 * lo_creat
 *    create a new large object
 * the mode is a bitmask describing different attributes of the new object
 *
 * returns the oid of the large object created or
 * InvalidOid upon failure
 */

Oid
lo_creat(PGconn *conn, int mode)
{
    PQArgBlock argv[1];
    PGresult *res;
    int retval;
    int result_len;

    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = mode;
    res  = PQfn(conn, conn->lobjfuncs->fn_lo_creat,
    		&retval,&result_len,1,argv,1);
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);
	return (Oid)retval;
    } else
	return InvalidOid;
}


/*
 * lo_tell
 *    returns the current seek location of the large object
 *
 */

int
lo_tell(PGconn *conn, int fd)
{
    int retval;
    PQArgBlock argv[1];
    PGresult *res;
    int result_len;

    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = fd;

    res = PQfn(conn, conn->lobjfuncs->fn_lo_tell,
    		&retval,&result_len,1,argv,1);
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);
	return retval;
    } else
	return -1;
}

/*
 * lo_unlink
 *    delete a file
 *
 */

int
lo_unlink(PGconn *conn, Oid lobjId)
{
    PQArgBlock argv[1];
    PGresult *res;
    int result_len;
    int retval;

    if(conn->lobjfuncs == (PGlobjfuncs *)NULL) {
        if(lo_initialize(conn) < 0) {
	    return -1;
	}
    }
    
    argv[0].isint = 1;
    argv[0].len = 4;
    argv[0].u.integer = lobjId;

    res = PQfn(conn, conn->lobjfuncs->fn_lo_unlink,
    		&retval,&result_len,1,argv,1);
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
	PQclear(res);
	return retval;
    } else
	return -1;
}

/*
 * lo_import -
 *    imports a file as an (inversion) large object.
 *      returns the oid of that object upon success,
 * returns InvalidOid upon failure
 *
 */

Oid
lo_import(PGconn *conn, char* filename)
{
    int fd;
    int nbytes, tmp;
    char buf[LO_BUFSIZE];
    Oid lobjOid;
    int lobj;
    
    /*
     * open the file to be read in
     */
    fd = open(filename, O_RDONLY, 0666);
    if (fd < 0)  {   /* error */
	sprintf(conn->errorMessage,
		"lo_import: can't open unix file\"%s\"\n", filename);
	return InvalidOid;
    }

    /*
     * create an inversion "object"
     */
    lobjOid = lo_creat(conn, INV_READ|INV_WRITE);
    if (lobjOid == InvalidOid) {
	sprintf(conn->errorMessage,
	        "lo_import: can't create inv object for \"%s\"", filename);
	return InvalidOid;
    }

    lobj = lo_open(conn, lobjOid, INV_WRITE);
    if (lobj == -1) {
	sprintf(conn->errorMessage,
		"lo_import: could not open inv object oid %d",lobjOid);
	return InvalidOid;
    }
    /*
     * read in from the Unix file and write to the inversion file
     */
    while ((nbytes = read(fd, buf, LO_BUFSIZE)) > 0) {
	tmp = lo_write(conn,lobj, buf, nbytes);
        if (tmp < nbytes) {
	    sprintf(conn->errorMessage,
		    "lo_import: error while reading \"%s\"",filename);
	    return InvalidOid;
	}
    }

    (void) close(fd);
    (void) lo_close(conn, lobj);

    return lobjOid;
}

/*
 * lo_export -
 *    exports an (inversion) large object.
 * returns -1 upon failure, 1 otherwise
 */
int
lo_export(PGconn *conn, Oid lobjId, char *filename)
{
    int fd;
    int nbytes, tmp;
    char buf[LO_BUFSIZE];
    int lobj;

    /*
     * create an inversion "object"
     */
    lobj = lo_open(conn, lobjId, INV_READ);
    if (lobj == -1) {
	sprintf(conn->errorMessage,
		"lo_export: can't open inv object %d",lobjId);
	return -1;
    }

    /*
     * open the file to be written to
     */
    fd = open(filename, O_CREAT|O_WRONLY, 0666);
    if (fd < 0)  {   /* error */
	sprintf(conn->errorMessage,
		"lo_export: can't open unix file\"%s\"",filename);
	return 0;
    }

    /*
     * read in from the Unix file and write to the inversion file
     */
    while ((nbytes = lo_read(conn, lobj, buf, LO_BUFSIZE)) > 0) {
	tmp = write(fd, buf, nbytes);
        if (tmp < nbytes) {
	    sprintf(conn->errorMessage,
		    "lo_export: error while writing \"%s\"",
		    filename);
	    return -1;
	}
    }

    (void) lo_close(conn,lobj);
    (void) close(fd);

    return 1;
}


/* ----------------
 * lo_initialize
 *
 * Initialize the large object interface for an existing connection.
 * We ask the backend about the functions OID's in pg_proc for all
 * functions that are required for large object operations.
 * ----------------
 */
static int lo_initialize(PGconn *conn)
{
    PGresult	*res;
    PGlobjfuncs *lobjfuncs;
    int		n;
    char	*fname;
    Oid		foid;

    /* ----------------
     * Allocate the structure to hold the functions OID's
     * ----------------
     */
    lobjfuncs = (PGlobjfuncs *)malloc(sizeof(PGlobjfuncs));
    if (lobjfuncs == (PGlobjfuncs *)NULL) {
        strcpy(conn->errorMessage, 
	    "FATAL: malloc() failed in lo_initialize()\n");
        return -1;
    }
    memset((char *)lobjfuncs, 0, sizeof(PGlobjfuncs));

    /* ----------------
     * Execute the query to get all the functions at once
     * ----------------
     */
    res = PQexec(conn, "select proname, oid from pg_proc	\
    		where proname = 'lo_open'	\
		   or proname = 'lo_close'	\
		   or proname = 'lo_creat'	\
		   or proname = 'lo_unlink'	\
		   or proname = 'lo_lseek'	\
		   or proname = 'lo_tell'	\
		   or proname = 'LOread'	\
		   or proname = 'LOwrite'");
    if (res == (PGresult *)NULL) {
	free(lobjfuncs);
        return -1;
    }

    if (res->resultStatus != PGRES_TUPLES_OK) {
        free(lobjfuncs);
	PQclear(res);
	strcpy(conn->errorMessage,
	    "ERROR: SELECT didn't return data in lo_initialize()\n");
	return -1;
    }

    /* ----------------
     * Examine the result and put the OID's into the struct
     * ----------------
     */
    for(n = 0; n < PQntuples(res); n++) {
        fname = PQgetvalue(res, n, 0);
	foid  = (Oid)atoi(PQgetvalue(res, n, 1));
	if(!strcmp(fname, "lo_open")) {
	    lobjfuncs->fn_lo_open = foid;
	} else
	if(!strcmp(fname, "lo_close")) {
	    lobjfuncs->fn_lo_close = foid;
	} else
	if(!strcmp(fname, "lo_creat")) {
	    lobjfuncs->fn_lo_creat = foid;
	} else
	if(!strcmp(fname, "lo_unlink")) {
	    lobjfuncs->fn_lo_unlink = foid;
	} else
	if(!strcmp(fname, "lo_lseek")) {
	    lobjfuncs->fn_lo_lseek = foid;
	} else
	if(!strcmp(fname, "lo_tell")) {
	    lobjfuncs->fn_lo_tell = foid;
	} else
	if(!strcmp(fname, "LOread")) {
	    lobjfuncs->fn_lo_read = foid;
	} else
	if(!strcmp(fname, "LOwrite")) {
	    lobjfuncs->fn_lo_write = foid;
	}
    }

    PQclear(res);

    /* ----------------
     * Finally check that we really got all large object
     * interface functions.
     * ----------------
     */
    if(lobjfuncs->fn_lo_open == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function lo_open\n");
	free(lobjfuncs);
	return -1;
    }
    if(lobjfuncs->fn_lo_close == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function lo_close\n");
	free(lobjfuncs);
	return -1;
    }
    if(lobjfuncs->fn_lo_creat == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function lo_creat\n");
	free(lobjfuncs);
	return -1;
    }
    if(lobjfuncs->fn_lo_unlink == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function lo_unlink\n");
	free(lobjfuncs);
	return -1;
    }
    if(lobjfuncs->fn_lo_lseek == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function lo_lseek\n");
	free(lobjfuncs);
	return -1;
    }
    if(lobjfuncs->fn_lo_tell == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function lo_tell\n");
	free(lobjfuncs);
	return -1;
    }
    if(lobjfuncs->fn_lo_read == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function LOread\n");
	free(lobjfuncs);
	return -1;
    }
    if(lobjfuncs->fn_lo_write == 0) {
        strcpy(conn->errorMessage,
	    "ERROR: Cannot determine OID for function LOwrite\n");
	free(lobjfuncs);
	return -1;
    }

    /* ----------------
     * Put the structure into the connection control
     * ----------------
     */
    conn->lobjfuncs = lobjfuncs;
    return 0;
}


