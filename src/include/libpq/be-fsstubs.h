/*-------------------------------------------------------------------------
 *
 * be-fsstubs.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: be-fsstubs.h,v 1.3 1997/09/07 04:58:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BE_FSSTUBS_H
#define BE_FSSTUBS_H

/* Redefine names LOread() and LOwrite() to be lowercase to allow calling
 *	using the new v6.1 case-insensitive SQL parser. Define macros to allow
 *	the existing code to stay the same. - tgl 97/05/03
 */

#define LOread(f,l) loread(f,l)
#define LOwrite(f,b) lowrite(f,b)

extern Oid		lo_import(text * filename);
extern int4		lo_export(Oid lobjId, text * filename);

extern Oid		lo_creat(int mode);

extern int		lo_open(Oid lobjId, int mode);
extern int		lo_close(int fd);
extern int		lo_read(int fd, char *buf, int len);
extern int		lo_write(int fd, char *buf, int len);
extern int		lo_lseek(int fd, int offset, int whence);
extern int		lo_tell(int fd);
extern int		lo_unlink(Oid lobjId);

extern struct varlena *loread(int fd, int len);
extern int		lowrite(int fd, struct varlena * wbuf);

#endif							/* BE_FSSTUBS_H */
