/*-------------------------------------------------------------------------
 *
 * smgr.h
 *	  storage manager switch public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: smgr.h,v 1.28 2001/03/22 04:01:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "access/xlog.h"
#include "storage/relfilenode.h"
#include "storage/block.h"
#include "storage/spin.h"
#include "utils/rel.h"

#define SM_FAIL			0
#define SM_SUCCESS		1

#define DEFAULT_SMGR	0

extern int	smgrinit(void);
extern int	smgrcreate(int16 which, Relation reln);
extern int	smgrunlink(int16 which, Relation reln);
extern int	smgrextend(int16 which, Relation reln, char *buffer);
extern int	smgropen(int16 which, Relation reln, bool failOK);
extern int	smgrclose(int16 which, Relation reln);
extern int smgrread(int16 which, Relation reln, BlockNumber blocknum,
		 char *buffer);
extern int smgrwrite(int16 which, Relation reln, BlockNumber blocknum,
		  char *buffer);
extern int smgrflush(int16 which, Relation reln, BlockNumber blocknum,
		  char *buffer);
extern int smgrblindwrt(int16 which, RelFileNode rnode,
			 BlockNumber blkno, char *buffer, bool dofsync);
extern int smgrblindmarkdirty(int16 which, RelFileNode rnode,
				   BlockNumber blkno);
extern int	smgrmarkdirty(int16 which, Relation reln, BlockNumber blkno);
extern int	smgrnblocks(int16 which, Relation reln);
extern int	smgrtruncate(int16 which, Relation reln, int nblocks);
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
extern int	mdextend(Relation reln, char *buffer);
extern int	mdopen(Relation reln);
extern int	mdclose(Relation reln);
extern int	mdread(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mdwrite(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mdflush(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mdmarkdirty(Relation reln, BlockNumber blkno);
extern int mdblindwrt(RelFileNode rnode, BlockNumber blkno,
		   char *buffer, bool dofsync);
extern int	mdblindmarkdirty(RelFileNode rnode, BlockNumber blkno);
extern int	mdnblocks(Relation reln);
extern int	mdtruncate(Relation reln, int nblocks);
extern int	mdcommit(void);
extern int	mdabort(void);
extern int	mdsync(void);

/* mm.c */
extern SPINLOCK MMCacheLock;

extern int	mminit(void);
extern int	mmcreate(Relation reln);
extern int	mmunlink(RelFileNode rnode);
extern int	mmextend(Relation reln, char *buffer);
extern int	mmopen(Relation reln);
extern int	mmclose(Relation reln);
extern int	mmread(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mmwrite(Relation reln, BlockNumber blocknum, char *buffer);
extern int	mmflush(Relation reln, BlockNumber blocknum, char *buffer);
extern int mmblindwrt(char *dbname, char *relname, Oid dbid, Oid relid,
		   BlockNumber blkno, char *buffer,
		   bool dofsync);
extern int	mmmarkdirty(Relation reln, BlockNumber blkno);
extern int mmblindmarkdirty(char *dbname, char *relname, Oid dbid, Oid relid,
				 BlockNumber blkno);
extern int	mmnblocks(Relation reln);
extern int	mmtruncate(Relation reln, int nblocks);
extern int	mmcommit(void);
extern int	mmabort(void);

extern int	mmshutdown(void);
extern int	MMShmemSize(void);

/* smgrtype.c */
extern Datum smgrout(PG_FUNCTION_ARGS);
extern Datum smgrin(PG_FUNCTION_ARGS);
extern Datum smgreq(PG_FUNCTION_ARGS);
extern Datum smgrne(PG_FUNCTION_ARGS);

#endif	 /* SMGR_H */
