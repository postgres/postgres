/*-------------------------------------------------------------------------
 *
 * be-fsstubs.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: be-fsstubs.h,v 1.1 1996/08/28 07:22:56 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	BE_FSSTUBS_H
#define	BE_FSSTUBS_H

extern Oid lo_import(text *filename);
extern int4 lo_export(Oid lobjId, text *filename);

extern Oid lo_creat(int mode);

extern int lo_open(Oid lobjId, int mode);
extern int lo_close(int fd);
extern int lo_read(int fd, char *buf, int len);
extern int lo_write(int fd, char *buf, int len);
extern int lo_lseek(int fd, int offset, int whence);
extern int lo_tell(int fd);
extern int lo_unlink(Oid lobjId);

extern struct varlena *LOread(int fd, int len);
extern int LOwrite(int fd, struct varlena *wbuf);
     
#endif	/* BE_FSSTUBS_H */
