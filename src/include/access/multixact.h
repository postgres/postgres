/*
 * multixact.h
 *
 * PostgreSQL multi-transaction-log manager
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/multixact.h
 */
#ifndef MULTIXACT_H
#define MULTIXACT_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"


/*
 * The first two MultiXactId values are reserved to store the truncation Xid
 * and epoch of the first segment, so we start assigning multixact values from
 * 2.
 */
#define InvalidMultiXactId	((MultiXactId) 0)
#define FirstMultiXactId	((MultiXactId) 1)
#define MaxMultiXactId		((MultiXactId) 0xFFFFFFFF)

#define MultiXactIdIsValid(multi) ((multi) != InvalidMultiXactId)

#define MaxMultiXactOffset	((MultiXactOffset) 0xFFFFFFFF)

/* Number of SLRU buffers to use for multixact */
#define NUM_MULTIXACTOFFSET_BUFFERS		8
#define NUM_MULTIXACTMEMBER_BUFFERS		16

/*
 * Possible multixact lock modes ("status").  The first four modes are for
 * tuple locks (FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE, FOR UPDATE); the
 * next two are used for update and delete modes.
 */
typedef enum
{
	MultiXactStatusForKeyShare = 0x00,
	MultiXactStatusForShare = 0x01,
	MultiXactStatusForNoKeyUpdate = 0x02,
	MultiXactStatusForUpdate = 0x03,
	/* an update that doesn't touch "key" columns */
	MultiXactStatusNoKeyUpdate = 0x04,
	/* other updates, and delete */
	MultiXactStatusUpdate = 0x05
} MultiXactStatus;

#define MaxMultiXactStatus MultiXactStatusUpdate

/* does a status value correspond to a tuple update? */
#define ISUPDATE_from_mxstatus(status) \
			((status) > MultiXactStatusForUpdate)


typedef struct MultiXactMember
{
	TransactionId xid;
	MultiXactStatus status;
} MultiXactMember;


/* ----------------
 *		multixact-related XLOG entries
 * ----------------
 */

#define XLOG_MULTIXACT_ZERO_OFF_PAGE	0x00
#define XLOG_MULTIXACT_ZERO_MEM_PAGE	0x10
#define XLOG_MULTIXACT_CREATE_ID		0x20
#define XLOG_MULTIXACT_TRUNCATE_ID		0x30

typedef struct xl_multixact_create
{
	MultiXactId mid;			/* new MultiXact's ID */
	MultiXactOffset moff;		/* its starting offset in members file */
	int32		nmembers;		/* number of member XIDs */
	MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} xl_multixact_create;

#define SizeOfMultiXactCreate (offsetof(xl_multixact_create, members))

typedef struct xl_multixact_truncate
{
	Oid			oldestMultiDB;

	/* to-be-truncated range of multixact offsets */
	MultiXactId startTruncOff;	/* just for completeness' sake */
	MultiXactId endTruncOff;

	/* to-be-truncated range of multixact members */
	MultiXactOffset startTruncMemb;
	MultiXactOffset endTruncMemb;
} xl_multixact_truncate;

#define SizeOfMultiXactTruncate (sizeof(xl_multixact_truncate))


extern MultiXactId MultiXactIdCreate(TransactionId xid1,
									 MultiXactStatus status1, TransactionId xid2,
									 MultiXactStatus status2);
extern MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid,
									 MultiXactStatus status);
extern MultiXactId MultiXactIdCreateFromMembers(int nmembers,
												MultiXactMember *members);

extern MultiXactId ReadNextMultiXactId(void);
extern bool MultiXactIdIsRunning(MultiXactId multi, bool isLockOnly);
extern void MultiXactIdSetOldestMember(void);
extern int	GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **xids,
								  bool allow_old, bool isLockOnly);
extern bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2);
extern bool MultiXactIdPrecedesOrEquals(MultiXactId multi1,
										MultiXactId multi2);

extern void AtEOXact_MultiXact(void);
extern void AtPrepare_MultiXact(void);
extern void PostPrepare_MultiXact(TransactionId xid);

extern Size MultiXactShmemSize(void);
extern void MultiXactShmemInit(void);
extern void BootStrapMultiXact(void);
extern void StartupMultiXact(void);
extern void TrimMultiXact(void);
extern void ShutdownMultiXact(void);
extern void SetMultiXactIdLimit(MultiXactId oldest_datminmxid,
								Oid oldest_datoid,
								bool is_startup);
extern void MultiXactGetCheckptMulti(bool is_shutdown,
									 MultiXactId *nextMulti,
									 MultiXactOffset *nextMultiOffset,
									 MultiXactId *oldestMulti,
									 Oid *oldestMultiDB);
extern void CheckPointMultiXact(void);
extern MultiXactId GetOldestMultiXactId(void);
extern void TruncateMultiXact(MultiXactId oldestMulti, Oid oldestMultiDB);
extern void MultiXactSetNextMXact(MultiXactId nextMulti,
								  MultiXactOffset nextMultiOffset);
extern void MultiXactAdvanceNextMXact(MultiXactId minMulti,
									  MultiXactOffset minMultiOffset);
extern void MultiXactAdvanceOldest(MultiXactId oldestMulti, Oid oldestMultiDB);
extern int	MultiXactMemberFreezeThreshold(void);

extern void multixact_twophase_recover(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
extern void multixact_twophase_postcommit(TransactionId xid, uint16 info,
										  void *recdata, uint32 len);
extern void multixact_twophase_postabort(TransactionId xid, uint16 info,
										 void *recdata, uint32 len);

extern void multixact_redo(XLogReaderState *record);
extern void multixact_desc(StringInfo buf, XLogReaderState *record);
extern const char *multixact_identify(uint8 info);
extern char *mxid_to_string(MultiXactId multi, int nmembers,
							MultiXactMember *members);

#endif							/* MULTIXACT_H */
