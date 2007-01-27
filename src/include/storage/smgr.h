/*-------------------------------------------------------------------------
 *
 * smgr.h
 *	  storage manager switch public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/smgr.h,v 1.55.2.1 2007/01/27 20:15:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "access/xlog.h"
#include "fmgr.h"
#include "storage/block.h"
#include "storage/relfilenode.h"


/*
 * smgr.c maintains a table of SMgrRelation objects, which are essentially
 * cached file handles.  An SMgrRelation is created (if not already present)
 * by smgropen(), and destroyed by smgrclose().  Note that neither of these
 * operations imply I/O, they just create or destroy a hashtable entry.
 * (But smgrclose() may release associated resources, such as OS-level file
 * descriptors.)
 *
 * An SMgrRelation may have an "owner", which is just a pointer to it from
 * somewhere else; smgr.c will clear this pointer if the SMgrRelation is
 * closed.	We use this to avoid dangling pointers from relcache to smgr
 * without having to make the smgr explicitly aware of relcache.  There
 * can't be more than one "owner" pointer per SMgrRelation, but that's
 * all we need.
 */
typedef struct SMgrRelationData
{
	/* rnode is the hashtable lookup key, so it must be first! */
	RelFileNode smgr_rnode;		/* relation physical identifier */

	/* pointer to owning pointer, or NULL if none */
	struct SMgrRelationData **smgr_owner;

	/* additional public fields may someday exist here */

	/*
	 * Fields below here are intended to be private to smgr.c and its
	 * submodules.	Do not touch them from elsewhere.
	 */
	int			smgr_which;		/* storage manager selector */

	struct _MdfdVec *md_fd;		/* for md.c; NULL if not open */
} SMgrRelationData;

typedef SMgrRelationData *SMgrRelation;


extern void smgrinit(void);
extern SMgrRelation smgropen(RelFileNode rnode);
extern void smgrsetowner(SMgrRelation *owner, SMgrRelation reln);
extern void smgrclose(SMgrRelation reln);
extern void smgrcloseall(void);
extern void smgrclosenode(RelFileNode rnode);
extern void smgrcreate(SMgrRelation reln, bool isTemp, bool isRedo);
extern void smgrscheduleunlink(SMgrRelation reln, bool isTemp);
extern void smgrdounlink(SMgrRelation reln, bool isTemp, bool isRedo);
extern void smgrextend(SMgrRelation reln, BlockNumber blocknum, char *buffer,
		   bool isTemp);
extern void smgrread(SMgrRelation reln, BlockNumber blocknum, char *buffer);
extern void smgrwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer,
		  bool isTemp);
extern BlockNumber smgrnblocks(SMgrRelation reln);
extern BlockNumber smgrtruncate(SMgrRelation reln, BlockNumber nblocks,
			 bool isTemp);
extern void smgrimmedsync(SMgrRelation reln);
extern void smgrDoPendingDeletes(bool isCommit);
extern int	smgrGetPendingDeletes(bool forCommit, RelFileNode **ptr);
extern void AtSubCommit_smgr(void);
extern void AtSubAbort_smgr(void);
extern void PostPrepare_smgr(void);
extern void smgrcommit(void);
extern void smgrabort(void);
extern void smgrsync(void);

extern void smgr_redo(XLogRecPtr lsn, XLogRecord *record);
extern void smgr_desc(StringInfo buf, uint8 xl_info, char *rec);


/* internals: move me elsewhere -- ay 7/94 */

/* in md.c */
extern bool mdinit(void);
extern bool mdclose(SMgrRelation reln);
extern bool mdcreate(SMgrRelation reln, bool isRedo);
extern bool mdunlink(RelFileNode rnode, bool isRedo);
extern bool mdextend(SMgrRelation reln, BlockNumber blocknum, char *buffer,
		 bool isTemp);
extern bool mdread(SMgrRelation reln, BlockNumber blocknum, char *buffer);
extern bool mdwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer,
		bool isTemp);
extern BlockNumber mdnblocks(SMgrRelation reln);
extern BlockNumber mdtruncate(SMgrRelation reln, BlockNumber nblocks,
		   bool isTemp);
extern bool mdimmedsync(SMgrRelation reln);
extern bool mdsync(void);

extern void RememberFsyncRequest(RelFileNode rnode, BlockNumber segno);
extern void ForgetRelationFsyncRequests(RelFileNode rnode);
extern void ForgetDatabaseFsyncRequests(Oid dbid);

/* smgrtype.c */
extern Datum smgrout(PG_FUNCTION_ARGS);
extern Datum smgrin(PG_FUNCTION_ARGS);
extern Datum smgreq(PG_FUNCTION_ARGS);
extern Datum smgrne(PG_FUNCTION_ARGS);

#endif   /* SMGR_H */
