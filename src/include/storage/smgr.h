/*-------------------------------------------------------------------------
 *
 * smgr.h--
 *    storage manager switch public interface declarations.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: smgr.h,v 1.2 1996/11/05 06:11:07 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include <storage/spin.h>
#include <storage/block.h>
#include <utils/rel.h>

#define SM_FAIL		0
#define	SM_SUCCESS	1

#define	DEFAULT_SMGR	0

extern int smgrinit(void);
extern void smgrshutdown(int dummy);
extern int smgrcreate(int16 which, Relation reln);
extern int smgrunlink(int16 which, Relation reln);
extern int smgrextend(int16 which, Relation reln, char *buffer);
extern int smgropen(int16 which, Relation reln);
extern int smgrclose(int16 which, Relation reln);
extern int smgrread(int16 which, Relation reln, BlockNumber blocknum,
		    char *buffer);
extern int smgrwrite(int16 which, Relation reln, BlockNumber blocknum,
		     char *buffer);
extern int smgrflush(int16 which, Relation reln, BlockNumber blocknum,
		     char *buffer);
extern int smgrblindwrt(int16 which, char *dbname, char *relname, Oid dbid,
			Oid relid, BlockNumber blkno, char *buffer);
extern int smgrnblocks(int16 which, Relation reln);
extern int smgrcommit(void);
extern int smgrabort(void);
extern bool smgriswo(int16 smgrno);



/* internals: move me elsewhere -- ay 7/94 */

/* in md.c */
extern int mdinit(void);
extern int mdcreate(Relation reln);
extern int mdunlink(Relation reln);
extern int mdextend(Relation reln, char *buffer);
extern int mdopen(Relation reln);
extern int mdclose(Relation reln);
extern int mdread(Relation reln, BlockNumber blocknum, char *buffer);
extern int mdwrite(Relation reln, BlockNumber blocknum, char *buffer);
extern int mdflush(Relation reln, BlockNumber blocknum, char *buffer);
extern int mdblindwrt(char *dbstr, char *relstr, Oid dbid, Oid relid,
		      BlockNumber blkno, char *buffer);
extern int mdnblocks(Relation reln);
extern int mdcommit(void);
extern int mdabort(void);

/* mm.c */
extern SPINLOCK MMCacheLock;

extern int mminit(void);
extern int mmshutdown(void);
extern int mmcreate(Relation reln);
extern int mmunlink(Relation reln);
extern int mmextend(Relation reln, char *buffer);
extern int mmopen(Relation reln);
extern int mmclose(Relation reln);
extern int mmread(Relation reln, BlockNumber blocknum, char *buffer);
extern int mmwrite(Relation reln, BlockNumber blocknum, char *buffer);
extern int mmflush(Relation reln, BlockNumber blocknum, char *buffer);
extern int mmblindwrt(char *dbstr, char *relstr, Oid dbid, Oid relid,
		      BlockNumber blkno, char *buffer);
extern int mmnblocks(Relation reln);
extern int mmcommit(void);
extern int mmabort(void);
extern int MMShmemSize(void);

#endif	/* SMGR_H */
