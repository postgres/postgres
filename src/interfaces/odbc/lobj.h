
/* File:			lobj.h
 *
 * Description:		See "lobj.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __LOBJ_H__
#define __LOBJ_H__


#include "psqlodbc.h"

struct lo_arg
{
	int			isint;
	int			len;
	union
	{
		int			integer;
		char	   *ptr;
	}			u;
};

#define LO_CREAT		957
#define LO_OPEN			952
#define LO_CLOSE		953
#define LO_READ			954
#define LO_WRITE		955
#define LO_LSEEK		956
#define LO_TELL			958
#define LO_UNLINK		964

#define INV_WRITE		0x00020000
#define INV_READ		0x00040000

Oid			lo_creat(ConnectionClass * conn, int mode);
int			lo_open(ConnectionClass * conn, int lobjId, int mode);
int			lo_close(ConnectionClass * conn, int fd);
int			lo_read(ConnectionClass * conn, int fd, char *buf, int len);
int			lo_write(ConnectionClass * conn, int fd, char *buf, int len);
int			lo_lseek(ConnectionClass * conn, int fd, int offset, int len);
int			lo_tell(ConnectionClass * conn, int fd);
int			lo_unlink(ConnectionClass * conn, Oid lobjId);

#endif
