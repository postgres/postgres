/*
 *
 * xlog.h
 *
 * Postgres transaction log manager
 *
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/rmgr.h"
#include "access/transam.h"

typedef struct XLogRecPtr
{
	uint32		xlogid;			/* log file #, 0 based */
	uint32		xrecoff;		/* offset of record in log file */
} XLogRecPtr;

typedef struct XLogRecord
{
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */
	XLogRecPtr	xl_xact_prev;	/* ptr to previous record of this xact */
	TransactionId xl_xid;		/* xact id */
	uint16		xl_len;			/* len of record *data* on this page */
	uint8		xl_info;
	RmgrId		xl_rmid;		/* resource manager inserted this record */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogRecord;

#define SizeOfXLogRecord	DOUBLEALIGN(sizeof(XLogRecord))
#define MAXLOGRECSZ			(2 * BLCKSZ)

#define XLogRecGetData(record)	\
	((char*)record + SizeOfXLogRecord)

/*
 * When there is no space on current page we continue on the next
 * page with subrecord.
 */
typedef struct XLogSubRecord
{
	uint16		xl_len;
	uint8		xl_info;

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogSubRecord;

#define SizeOfXLogSubRecord DOUBLEALIGN(sizeof(XLogSubRecord))

/*
 * XLOG uses only low 4 bits of xl_info. High 4 bits may be used
 * by rmgr...
 */
#define XLR_TO_BE_CONTINUED		0x01
#define	XLR_INFO_MASK			0x0F

#define XLOG_PAGE_MAGIC 0x17345168

typedef struct XLogPageHeaderData
{
	uint32		xlp_magic;
	uint16		xlp_info;
} XLogPageHeaderData;

#define SizeOfXLogPHD	DOUBLEALIGN(sizeof(XLogPageHeaderData))

typedef XLogPageHeaderData *XLogPageHeader;

#define XLP_FIRST_IS_SUBRECORD	0x0001

#define XLByteLT(left, right)		\
			(right.xlogid > left.xlogid || \
			(right.xlogid == left.xlogid && right.xrecoff > left.xrecoff))

#define XLByteLE(left, right)		\
			(right.xlogid > left.xlogid || \
			(right.xlogid == left.xlogid && right.xrecoff >=  left.xrecoff))

#define XLByteEQ(left, right)		\
			(right.xlogid == left.xlogid && right.xrecoff ==  left.xrecoff)

/*
 * StartUpID (SUI) - system startups counter.
 * It's to allow removing pg_log after shutdown.
 */
typedef	uint32		StartUpID;
extern	StartUpID	ThisStartUpID;
extern	bool		InRecovery;
extern	XLogRecPtr	MyLastRecPtr;

typedef struct RmgrData
{
	char	   *rm_name;
	void	   (*rm_redo)(XLogRecPtr lsn, XLogRecord *rptr);
	void	   (*rm_undo)(XLogRecPtr lsn, XLogRecord *rptr);
	void	   (*rm_desc)(char *buf, uint8 xl_info, char *rec);
} RmgrData;

extern RmgrData RmgrTable[];

extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info, 
			char *hdr, uint32 hdrlen,
			char *buf, uint32 buflen);
extern void XLogFlush(XLogRecPtr RecPtr);

#endif	 /* XLOG_H */
