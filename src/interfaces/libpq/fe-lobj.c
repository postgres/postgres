/*-------------------------------------------------------------------------
 *
 * fe-lobj.c
 *	  Front-end large object interface
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-lobj.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef WIN32
/*
 *	As unlink/rename are #define'd in port.h (via postgres_fe.h), io.h
 *	must be included first on MS C.  Might as well do it for all WIN32's
 *	here.
 */
#include <io.h>
#endif

#include "postgres_fe.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <netinet/in.h>			/* for ntohl/htonl */
#include <arpa/inet.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/libpq-fs.h"		/* must come after sys/stat.h */

#define LO_BUFSIZE		  8192

static int	lo_initialize(PGconn *conn);
static Oid	lo_import_internal(PGconn *conn, const char *filename, Oid oid);
static pg_int64 lo_hton64(pg_int64 host64);
static pg_int64 lo_ntoh64(pg_int64 net64);

/*
 * lo_open
 *	  opens an existing large object
 *
 * returns the file descriptor for use in later lo_* calls
 * return -1 upon failure.
 */
int
lo_open(PGconn *conn, Oid lobjId, int mode)
{
	int			fd;
	int			result_len;
	PQArgBlock	argv[2];
	PGresult   *res;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = mode;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_open, &fd, &result_len, 1, argv, 2);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return fd;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_close
 *	  closes an existing large object
 *
 * returns 0 upon success
 * returns -1 upon failure.
 */
int
lo_close(PGconn *conn, int fd)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;
	res = PQfn(conn, conn->lobjfuncs->fn_lo_close,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_truncate
 *	  truncates an existing large object to the given size
 *
 * returns 0 upon success
 * returns -1 upon failure
 */
int
lo_truncate(PGconn *conn, int fd, size_t len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	/* Must check this on-the-fly because it's not there pre-8.3 */
	if (conn->lobjfuncs->fn_lo_truncate == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			libpq_gettext("cannot determine OID of function lo_truncate\n"));
		return -1;
	}

	/*
	 * Long ago, somebody thought it'd be a good idea to declare this function
	 * as taking size_t ... but the underlying backend function only accepts a
	 * signed int32 length.  So throw error if the given value overflows
	 * int32.  (A possible alternative is to automatically redirect the call
	 * to lo_truncate64; but if the caller wanted to rely on that backend
	 * function being available, he could have called lo_truncate64 for
	 * himself.)
	 */
	if (len > (size_t) INT_MAX)
	{
		printfPQExpBuffer(&conn->errorMessage,
		   libpq_gettext("argument of lo_truncate exceeds integer range\n"));
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = (int) len;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_truncate,
			   &retval, &result_len, 1, argv, 2);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_truncate64
 *	  truncates an existing large object to the given size
 *
 * returns 0 upon success
 * returns -1 upon failure
 */
int
lo_truncate64(PGconn *conn, int fd, pg_int64 len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	if (conn->lobjfuncs->fn_lo_truncate64 == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
		  libpq_gettext("cannot determine OID of function lo_truncate64\n"));
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	len = lo_hton64(len);
	argv[1].isint = 0;
	argv[1].len = 8;
	argv[1].u.ptr = (int *) &len;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_truncate64,
			   &retval, &result_len, 1, argv, 2);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_read
 *	  read len bytes of the large object into buf
 *
 * returns the number of bytes read, or -1 on failure.
 * the CALLER must have allocated enough space to hold the result returned
 */

int
lo_read(PGconn *conn, int fd, char *buf, size_t len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	/*
	 * Long ago, somebody thought it'd be a good idea to declare this function
	 * as taking size_t ... but the underlying backend function only accepts a
	 * signed int32 length.  So throw error if the given value overflows
	 * int32.
	 */
	if (len > (size_t) INT_MAX)
	{
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("argument of lo_read exceeds integer range\n"));
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = (int) len;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_read,
			   (void *) buf, &result_len, 0, argv, 2);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return result_len;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_write
 *	  write len bytes of buf into the large object fd
 *
 * returns the number of bytes written, or -1 on failure.
 */
int
lo_write(PGconn *conn, int fd, const char *buf, size_t len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			result_len;
	int			retval;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	/*
	 * Long ago, somebody thought it'd be a good idea to declare this function
	 * as taking size_t ... but the underlying backend function only accepts a
	 * signed int32 length.  So throw error if the given value overflows
	 * int32.
	 */
	if (len > (size_t) INT_MAX)
	{
		printfPQExpBuffer(&conn->errorMessage,
			  libpq_gettext("argument of lo_write exceeds integer range\n"));
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 0;
	argv[1].len = (int) len;
	argv[1].u.ptr = (int *) buf;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_write,
			   &retval, &result_len, 1, argv, 2);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_lseek
 *	  change the current read or write location on a large object
 */
int
lo_lseek(PGconn *conn, int fd, int offset, int whence)
{
	PQArgBlock	argv[3];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
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
			   &retval, &result_len, 1, argv, 3);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_lseek64
 *	  change the current read or write location on a large object
 */
pg_int64
lo_lseek64(PGconn *conn, int fd, pg_int64 offset, int whence)
{
	PQArgBlock	argv[3];
	PGresult   *res;
	pg_int64	retval;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	if (conn->lobjfuncs->fn_lo_lseek64 == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			 libpq_gettext("cannot determine OID of function lo_lseek64\n"));
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	offset = lo_hton64(offset);
	argv[1].isint = 0;
	argv[1].len = 8;
	argv[1].u.ptr = (int *) &offset;

	argv[2].isint = 1;
	argv[2].len = 4;
	argv[2].u.integer = whence;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_lseek64,
			   (void *) &retval, &result_len, 0, argv, 3);
	if (PQresultStatus(res) == PGRES_COMMAND_OK && result_len == 8)
	{
		PQclear(res);
		return lo_ntoh64(retval);
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_creat
 *	  create a new large object
 * the mode is ignored (once upon a time it had a use)
 *
 * returns the oid of the large object created or
 * InvalidOid upon failure
 */
Oid
lo_creat(PGconn *conn, int mode)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return InvalidOid;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = mode;
	res = PQfn(conn, conn->lobjfuncs->fn_lo_creat,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return (Oid) retval;
	}
	else
	{
		PQclear(res);
		return InvalidOid;
	}
}

/*
 * lo_create
 *	  create a new large object
 * if lobjId isn't InvalidOid, it specifies the OID to (attempt to) create
 *
 * returns the oid of the large object created or
 * InvalidOid upon failure
 */
Oid
lo_create(PGconn *conn, Oid lobjId)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return InvalidOid;
	}

	/* Must check this on-the-fly because it's not there pre-8.1 */
	if (conn->lobjfuncs->fn_lo_create == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			  libpq_gettext("cannot determine OID of function lo_create\n"));
		return InvalidOid;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;
	res = PQfn(conn, conn->lobjfuncs->fn_lo_create,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return (Oid) retval;
	}
	else
	{
		PQclear(res);
		return InvalidOid;
	}
}


/*
 * lo_tell
 *	  returns the current seek location of the large object
 */
int
lo_tell(PGconn *conn, int fd)
{
	int			retval;
	PQArgBlock	argv[1];
	PGresult   *res;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_tell,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_tell64
 *	  returns the current seek location of the large object
 */
pg_int64
lo_tell64(PGconn *conn, int fd)
{
	pg_int64	retval;
	PQArgBlock	argv[1];
	PGresult   *res;
	int			result_len;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	if (conn->lobjfuncs->fn_lo_tell64 == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			  libpq_gettext("cannot determine OID of function lo_tell64\n"));
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_tell64,
			   (void *) &retval, &result_len, 0, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK && result_len == 8)
	{
		PQclear(res);
		return lo_ntoh64(retval);
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_unlink
 *	  delete a file
 */

int
lo_unlink(PGconn *conn, Oid lobjId)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			result_len;
	int			retval;

	if (conn == NULL || conn->lobjfuncs == NULL)
	{
		if (lo_initialize(conn) < 0)
			return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_unlink,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_import -
 *	  imports a file as an (inversion) large object.
 *
 * returns the oid of that object upon success,
 * returns InvalidOid upon failure
 */

Oid
lo_import(PGconn *conn, const char *filename)
{
	return lo_import_internal(conn, filename, InvalidOid);
}

/*
 * lo_import_with_oid -
 *	  imports a file as an (inversion) large object.
 *	  large object id can be specified.
 *
 * returns the oid of that object upon success,
 * returns InvalidOid upon failure
 */

Oid
lo_import_with_oid(PGconn *conn, const char *filename, Oid lobjId)
{
	return lo_import_internal(conn, filename, lobjId);
}

static Oid
lo_import_internal(PGconn *conn, const char *filename, Oid oid)
{
	int			fd;
	int			nbytes,
				tmp;
	char		buf[LO_BUFSIZE];
	Oid			lobjOid;
	int			lobj;
	char		sebuf[256];

	/*
	 * open the file to be read in
	 */
	fd = open(filename, O_RDONLY | PG_BINARY, 0666);
	if (fd < 0)
	{							/* error */
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not open file \"%s\": %s\n"),
						  filename, pqStrerror(errno, sebuf, sizeof(sebuf)));
		return InvalidOid;
	}

	/*
	 * create an inversion object
	 */
	if (oid == InvalidOid)
		lobjOid = lo_creat(conn, INV_READ | INV_WRITE);
	else
		lobjOid = lo_create(conn, oid);

	if (lobjOid == InvalidOid)
	{
		/* we assume lo_create() already set a suitable error message */
		(void) close(fd);
		return InvalidOid;
	}

	lobj = lo_open(conn, lobjOid, INV_WRITE);
	if (lobj == -1)
	{
		/* we assume lo_open() already set a suitable error message */
		(void) close(fd);
		return InvalidOid;
	}

	/*
	 * read in from the file and write to the large object
	 */
	while ((nbytes = read(fd, buf, LO_BUFSIZE)) > 0)
	{
		tmp = lo_write(conn, lobj, buf, nbytes);
		if (tmp != nbytes)
		{
			/*
			 * If lo_write() failed, we are now in an aborted transaction so
			 * there's no need for lo_close(); furthermore, if we tried it
			 * we'd overwrite the useful error result with a useless one. So
			 * just nail the doors shut and get out of town.
			 */
			(void) close(fd);
			return InvalidOid;
		}
	}

	if (nbytes < 0)
	{
		/* We must do lo_close before setting the errorMessage */
		int			save_errno = errno;

		(void) lo_close(conn, lobj);
		(void) close(fd);
		printfPQExpBuffer(&conn->errorMessage,
					  libpq_gettext("could not read from file \"%s\": %s\n"),
						  filename,
						  pqStrerror(save_errno, sebuf, sizeof(sebuf)));
		return InvalidOid;
	}

	(void) close(fd);

	if (lo_close(conn, lobj) != 0)
	{
		/* we assume lo_close() already set a suitable error message */
		return InvalidOid;
	}

	return lobjOid;
}

/*
 * lo_export -
 *	  exports an (inversion) large object.
 * returns -1 upon failure, 1 if OK
 */
int
lo_export(PGconn *conn, Oid lobjId, const char *filename)
{
	int			result = 1;
	int			fd;
	int			nbytes,
				tmp;
	char		buf[LO_BUFSIZE];
	int			lobj;
	char		sebuf[256];

	/*
	 * open the large object.
	 */
	lobj = lo_open(conn, lobjId, INV_READ);
	if (lobj == -1)
	{
		/* we assume lo_open() already set a suitable error message */
		return -1;
	}

	/*
	 * create the file to be written to
	 */
	fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | PG_BINARY, 0666);
	if (fd < 0)
	{
		/* We must do lo_close before setting the errorMessage */
		int			save_errno = errno;

		(void) lo_close(conn, lobj);
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("could not open file \"%s\": %s\n"),
						  filename,
						  pqStrerror(save_errno, sebuf, sizeof(sebuf)));
		return -1;
	}

	/*
	 * read in from the large object and write to the file
	 */
	while ((nbytes = lo_read(conn, lobj, buf, LO_BUFSIZE)) > 0)
	{
		tmp = write(fd, buf, nbytes);
		if (tmp != nbytes)
		{
			/* We must do lo_close before setting the errorMessage */
			int			save_errno = errno;

			(void) lo_close(conn, lobj);
			(void) close(fd);
			printfPQExpBuffer(&conn->errorMessage,
					   libpq_gettext("could not write to file \"%s\": %s\n"),
							  filename,
							  pqStrerror(save_errno, sebuf, sizeof(sebuf)));
			return -1;
		}
	}

	/*
	 * If lo_read() failed, we are now in an aborted transaction so there's no
	 * need for lo_close(); furthermore, if we tried it we'd overwrite the
	 * useful error result with a useless one. So skip lo_close() if we got a
	 * failure result.
	 */
	if (nbytes < 0 ||
		lo_close(conn, lobj) != 0)
	{
		/* assume lo_read() or lo_close() left a suitable error message */
		result = -1;
	}

	/* if we already failed, don't overwrite that msg with a close error */
	if (close(fd) && result >= 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
					   libpq_gettext("could not write to file \"%s\": %s\n"),
						  filename, pqStrerror(errno, sebuf, sizeof(sebuf)));
		result = -1;
	}

	return result;
}


/*
 * lo_initialize
 *
 * Initialize the large object interface for an existing connection.
 * We ask the backend about the functions OID's in pg_proc for all
 * functions that are required for large object operations.
 */
static int
lo_initialize(PGconn *conn)
{
	PGresult   *res;
	PGlobjfuncs *lobjfuncs;
	int			n;
	const char *query;
	const char *fname;
	Oid			foid;

	if (!conn)
		return -1;

	/*
	 * Allocate the structure to hold the functions OID's
	 */
	lobjfuncs = (PGlobjfuncs *) malloc(sizeof(PGlobjfuncs));
	if (lobjfuncs == NULL)
	{
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("out of memory\n"));
		return -1;
	}
	MemSet((char *) lobjfuncs, 0, sizeof(PGlobjfuncs));

	/*
	 * Execute the query to get all the functions at once.  In 7.3 and later
	 * we need to be schema-safe.  lo_create only exists in 8.1 and up.
	 * lo_truncate only exists in 8.3 and up.
	 */
	if (conn->sversion >= 70300)
		query = "select proname, oid from pg_catalog.pg_proc "
			"where proname in ("
			"'lo_open', "
			"'lo_close', "
			"'lo_creat', "
			"'lo_create', "
			"'lo_unlink', "
			"'lo_lseek', "
			"'lo_lseek64', "
			"'lo_tell', "
			"'lo_tell64', "
			"'lo_truncate', "
			"'lo_truncate64', "
			"'loread', "
			"'lowrite') "
			"and pronamespace = (select oid from pg_catalog.pg_namespace "
			"where nspname = 'pg_catalog')";
	else
		query = "select proname, oid from pg_proc "
			"where proname = 'lo_open' "
			"or proname = 'lo_close' "
			"or proname = 'lo_creat' "
			"or proname = 'lo_unlink' "
			"or proname = 'lo_lseek' "
			"or proname = 'lo_tell' "
			"or proname = 'loread' "
			"or proname = 'lowrite'";

	res = PQexec(conn, query);
	if (res == NULL)
	{
		free(lobjfuncs);
		return -1;
	}

	if (res->resultStatus != PGRES_TUPLES_OK)
	{
		free(lobjfuncs);
		PQclear(res);
		printfPQExpBuffer(&conn->errorMessage,
						  libpq_gettext("query to initialize large object functions did not return data\n"));
		return -1;
	}

	/*
	 * Examine the result and put the OID's into the struct
	 */
	for (n = 0; n < PQntuples(res); n++)
	{
		fname = PQgetvalue(res, n, 0);
		foid = (Oid) atoi(PQgetvalue(res, n, 1));
		if (strcmp(fname, "lo_open") == 0)
			lobjfuncs->fn_lo_open = foid;
		else if (strcmp(fname, "lo_close") == 0)
			lobjfuncs->fn_lo_close = foid;
		else if (strcmp(fname, "lo_creat") == 0)
			lobjfuncs->fn_lo_creat = foid;
		else if (strcmp(fname, "lo_create") == 0)
			lobjfuncs->fn_lo_create = foid;
		else if (strcmp(fname, "lo_unlink") == 0)
			lobjfuncs->fn_lo_unlink = foid;
		else if (strcmp(fname, "lo_lseek") == 0)
			lobjfuncs->fn_lo_lseek = foid;
		else if (strcmp(fname, "lo_lseek64") == 0)
			lobjfuncs->fn_lo_lseek64 = foid;
		else if (strcmp(fname, "lo_tell") == 0)
			lobjfuncs->fn_lo_tell = foid;
		else if (strcmp(fname, "lo_tell64") == 0)
			lobjfuncs->fn_lo_tell64 = foid;
		else if (strcmp(fname, "lo_truncate") == 0)
			lobjfuncs->fn_lo_truncate = foid;
		else if (strcmp(fname, "lo_truncate64") == 0)
			lobjfuncs->fn_lo_truncate64 = foid;
		else if (strcmp(fname, "loread") == 0)
			lobjfuncs->fn_lo_read = foid;
		else if (strcmp(fname, "lowrite") == 0)
			lobjfuncs->fn_lo_write = foid;
	}

	PQclear(res);

	/*
	 * Finally check that we got all required large object interface functions
	 * (ones that have been added later than the stone age are instead checked
	 * only if used)
	 */
	if (lobjfuncs->fn_lo_open == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("cannot determine OID of function lo_open\n"));
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_close == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("cannot determine OID of function lo_close\n"));
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_creat == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("cannot determine OID of function lo_creat\n"));
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_unlink == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			  libpq_gettext("cannot determine OID of function lo_unlink\n"));
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_lseek == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
			   libpq_gettext("cannot determine OID of function lo_lseek\n"));
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_tell == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("cannot determine OID of function lo_tell\n"));
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_read == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
				 libpq_gettext("cannot determine OID of function loread\n"));
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_write == 0)
	{
		printfPQExpBuffer(&conn->errorMessage,
				libpq_gettext("cannot determine OID of function lowrite\n"));
		free(lobjfuncs);
		return -1;
	}

	/*
	 * Put the structure into the connection control
	 */
	conn->lobjfuncs = lobjfuncs;
	return 0;
}

/*
 * lo_hton64
 *	  converts a 64-bit integer from host byte order to network byte order
 */
static pg_int64
lo_hton64(pg_int64 host64)
{
	union
	{
		pg_int64	i64;
		uint32		i32[2];
	}			swap;
	uint32		t;

	/* High order half first, since we're doing MSB-first */
	t = (uint32) (host64 >> 32);
	swap.i32[0] = htonl(t);

	/* Now the low order half */
	t = (uint32) host64;
	swap.i32[1] = htonl(t);

	return swap.i64;
}

/*
 * lo_ntoh64
 *	  converts a 64-bit integer from network byte order to host byte order
 */
static pg_int64
lo_ntoh64(pg_int64 net64)
{
	union
	{
		pg_int64	i64;
		uint32		i32[2];
	}			swap;
	pg_int64	result;

	swap.i64 = net64;

	result = (uint32) ntohl(swap.i32[0]);
	result <<= 32;
	result |= (uint32) ntohl(swap.i32[1]);

	return result;
}
