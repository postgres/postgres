/*
 * xlog.h
 *
 * PostgreSQL transaction log manager
 *
 * $Header: /cvsroot/pgsql/src/include/access/xlog.h,v 1.16 2001/01/12 21:54:01 tgl Exp $
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/rmgr.h"
#include "access/transam.h"
#include "access/xlogdefs.h"
#include "access/xlogutils.h"

typedef struct crc64
{
	uint32		crc1;
	uint32		crc2;
} crc64;

typedef struct XLogRecord
{
	crc64		xl_crc;
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */
	XLogRecPtr	xl_xact_prev;	/* ptr to previous record of this xact */
	TransactionId xl_xid;		/* xact id */
	uint16		xl_len;			/* total len of record *data* */
	uint8		xl_info;
	RmgrId		xl_rmid;		/* resource manager inserted this record */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogRecord;

#define SizeOfXLogRecord	DOUBLEALIGN(sizeof(XLogRecord))
#define MAXLOGRECSZ			(2 * BLCKSZ)

#define XLogRecGetData(record)	\
	((char*)record + SizeOfXLogRecord)

/*
 * When there is no space on current page we continue
 * on the next page with subrecord.
 */
typedef struct XLogSubRecord
{
	uint16		xl_len;			/* len of data left */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogSubRecord;

#define	SizeOfXLogSubRecord	DOUBLEALIGN(sizeof(XLogSubRecord))

/*
 * XLOG uses only low 4 bits of xl_info.
 * High 4 bits may be used by rmgr...
 *
 * We support backup of 2 blocks per record only.
 * If we backed up some of these blocks then we use
 * flags below to signal rmgr about this on recovery.
 */
#define XLR_SET_BKP_BLOCK(iblk)	(0x08 >> iblk)
#define XLR_BKP_BLOCK_1			XLR_SET_BKP_BLOCK(0)	/* 0x08 */
#define XLR_BKP_BLOCK_2			XLR_SET_BKP_BLOCK(1)	/* 0x04 */
#define	XLR_INFO_MASK			0x0F

/*
 * Sometimes we log records which are out of transaction control.
 * Rmgr may use flag below for this purpose.
 */
#define	XLOG_NO_TRAN			XLR_INFO_MASK

#define XLOG_PAGE_MAGIC 0x17345168

typedef struct XLogPageHeaderData
{
	uint32		xlp_magic;
	uint16		xlp_info;
} XLogPageHeaderData;

#define SizeOfXLogPHD	DOUBLEALIGN(sizeof(XLogPageHeaderData))

typedef XLogPageHeaderData *XLogPageHeader;

/* When record crosses page boundary */
#define XLP_FIRST_IS_SUBRECORD	0x0001

#define XLByteLT(left, right)		\
			(right.xlogid > left.xlogid || \
			(right.xlogid == left.xlogid && right.xrecoff > left.xrecoff))

#define XLByteLE(left, right)		\
			(right.xlogid > left.xlogid || \
			(right.xlogid == left.xlogid && right.xrecoff >=  left.xrecoff))

#define XLByteEQ(left, right)		\
			(right.xlogid == left.xlogid && right.xrecoff ==  left.xrecoff)

extern	StartUpID	ThisStartUpID;	/* current SUI */
extern	bool		InRecovery;
extern	XLogRecPtr	MyLastRecPtr;
extern volatile uint32		CritSectionCount;

typedef struct RmgrData
{
	char	   *rm_name;
	void	   (*rm_redo)(XLogRecPtr lsn, XLogRecord *rptr);
	void	   (*rm_undo)(XLogRecPtr lsn, XLogRecord *rptr);
	void	   (*rm_desc)(char *buf, uint8 xl_info, char *rec);
} RmgrData;

extern RmgrData RmgrTable[];

/*
 * List of these structs is used to pass data to XLOG.
 * If buffer is valid then XLOG will check if buffer must
 * be backup-ed. For backup-ed buffer data will not be
 * inserted into record (and XLOG sets
 * XLR_BKP_BLOCK_X bit in xl_info).
 */
typedef struct XLogRecData
{
	Buffer				buffer;		/* buffer associated with this data */
	char			   *data;
	uint32				len;
	struct XLogRecData *next;
} XLogRecData;

extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata);
extern void XLogFlush(XLogRecPtr RecPtr);

extern void CreateCheckPoint(bool shutdown);

extern void xlog_redo(XLogRecPtr lsn, XLogRecord *record);
extern void xlog_undo(XLogRecPtr lsn, XLogRecord *record);
extern void xlog_desc(char *buf, uint8 xl_info, char* rec);

extern void UpdateControlFile(void);
extern int XLOGShmemSize(void);
extern void XLOGShmemInit(void);
extern void XLOGPathInit(void);
extern void BootStrapXLOG(void);
extern void StartupXLOG(void);
extern void ShutdownXLOG(void);
extern void CreateCheckPoint(bool shutdown);
extern void SetThisStartUpID(void);

#endif	 /* XLOG_H */
