/* Module:			lobj.c
 *
 * Description:		This module contains routines related to manipulating
 *					large objects.
 *
 * Classes:			none
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#include "lobj.h"
#include "psqlodbc.h"
#include "connection.h"

Oid
lo_creat(ConnectionClass *conn, int mode)
{
	LO_ARG		argv[1];
	int			retval,
				result_len;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = mode;

	if (!CC_send_function(conn, LO_CREAT, &retval, &result_len, 1, argv, 1))
		return 0;				/* invalid oid */
	else
		return retval;


}

int
lo_open(ConnectionClass *conn, int lobjId, int mode)
{
	int			fd;
	int			result_len;
	LO_ARG		argv[2];


	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = mode;

	if (!CC_send_function(conn, LO_OPEN, &fd, &result_len, 1, argv, 2))
		return -1;

	if (fd >= 0 && lo_lseek(conn, fd, 0L, SEEK_SET) < 0)
		return -1;

	return fd;
}

int
lo_close(ConnectionClass *conn, int fd)
{
	LO_ARG		argv[1];
	int			retval,
				result_len;


	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	if (!CC_send_function(conn, LO_CLOSE, &retval, &result_len, 1, argv, 1))
		return -1;

	else
		return retval;

}


int
lo_read(ConnectionClass *conn, int fd, char *buf, int len)
{
	LO_ARG		argv[2];
	int			result_len;


	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = len;

	if (!CC_send_function(conn, LO_READ, (int *) buf, &result_len, 0, argv, 2))
		return -1;

	else
		return result_len;
}

int
lo_write(ConnectionClass *conn, int fd, char *buf, int len)
{
	LO_ARG		argv[2];
	int			retval,
				result_len;


	if (len <= 0)
		return 0;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 0;
	argv[1].len = len;
	argv[1].u.ptr = (char *) buf;

	if (!CC_send_function(conn, LO_WRITE, &retval, &result_len, 1, argv, 2))
		return -1;

	else
		return retval;
}

int
lo_lseek(ConnectionClass *conn, int fd, int offset, int whence)
{
	LO_ARG		argv[3];
	int			retval,
				result_len;


	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = offset;

	argv[2].isint = 1;
	argv[2].len = 4;
	argv[2].u.integer = whence;

	if (!CC_send_function(conn, LO_LSEEK, &retval, &result_len, 1, argv, 3))
		return -1;

	else
		return retval;
}

int
lo_tell(ConnectionClass *conn, int fd)
{
	LO_ARG		argv[1];
	int			retval,
				result_len;


	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	if (!CC_send_function(conn, LO_TELL, &retval, &result_len, 1, argv, 1))
		return -1;

	else
		return retval;
}

int
lo_unlink(ConnectionClass *conn, Oid lobjId)
{
	LO_ARG		argv[1];
	int			retval,
				result_len;


	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;

	if (!CC_send_function(conn, LO_UNLINK, &retval, &result_len, 1, argv, 1))
		return -1;

	else
		return retval;
}
