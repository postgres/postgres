/*-------------------------------------------------------------------------
 *
 * smgr.h
 *	  storage manager switch public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: smgr.h,v 1.37 2003/08/04 02:40:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "access/xlog.h"
#include "storage/relfilenode.h"
#include "storage/block.h"
#include "utils/rel.h"


#define SM_FAIL			0
#define SM_SUCCESS		1

#define DEFAULT_SMGR	0

extern int	smgrinit(void);
extern int	smgrcreate(int16 which, Relation reln);
extern int	smgrunlink(int16 which, Relation reln);
extern int smgrextend(int16 which, Relation reln, BlockNumber blocknum,
		   char *buffer);
extern int	smgropen(int16 which, Relation reln, bool failOK);
extern int	smgrclose(int16 which, Relation reln);
extern int smgrread(int16 which, Relation reln, BlockNumber blocknum,
		 char *buffer);
extern int smgrwrite(int16 which, Relation reln, BlockNumber blocknum,
		  char *buffer);
extern int smgrblindwrt(int16 which, RelFileNode rnode,
			 BlockNumber blkno, char *buffer);
extern BlockNumber smgrnblocks(int16 which, Relation reln);
extern BlockNumber smgrtruncate(int16 which, Relation reln,
			 BlockNumber nblocks);
extern int	smgrDoPendingDeletes(bool isCommit);
extern int	smgrcommit(void);
extern int	smgrabort(void);
extern int	smgrsync(void);

extern void smgr_redo(XLogRecPtr lsn, XLogRecord *record);
extern void smgr_undo(XLogRecPtr lsn, XLogRecord *record);
extern void smgr_desc(char *buf, uint8 xl_info, char *rec);


/* internals: move me elsewhere -- ay 7/94 */

/* in md.c */
extern int	mdinit(void);
extern int	mdcreate(Relation reln);
extern int	mdunlink(RelFileNode rnode);
extern int	mdextend(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mdopen(Relation reln);
extern int	mdclose(Relation reln);
extern int	mdread(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mdwrite(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mdblindwrt(RelFileNode rnode, BlockNumber blkno, char *buffer);
extern BlockNumber mdnblocks(Relation reln);
extern BlockNumber mdtruncate(Relation reln, BlockNumber nblocks);
extern int	mdcommit(void);
extern int	mdabort(void);
extern int	mdsync(void);

/* mm.c */
extern int	mminit(void);
extern int	mmcreate(Relation reln);
extern int	mmunlink(RelFileNode rnode);
extern int	mmextend(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mmopen(Relation reln);
extern int	mmclose(Relation reln);
extern int	mmread(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mmwrite(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mmblindwrt(RelFileNode rnode, BlockNumber blkno, char *buffer);
extern BlockNumber mmnblocks(Relation reln);
extern BlockNumber mmtruncate(Relation reln, BlockNumber nblocks);
extern int	mmcommit(void);
extern int	mmabort(void);

extern int	mmshutdown(void);
extern int	MMShmemSize(void);

/* smgrtype.c */
extern Datum smgrout(PG_FUNCTION_ARGS);
extern Datum smgrin(PG_FUNCTION_ARGS);
extern Datum smgreq(PG_FUNCTION_ARGS);
extern Datum smgrne(PG_FUNCTION_ARGS);

#endif   /* SMGR_H */
