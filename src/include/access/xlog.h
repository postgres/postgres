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
	uint32		xlogid;		/* log file #, 0 based */
	uint32		xrecoff;	/* offset of record in log file */
} XLogRecPtr;

typedef struct XLogRecord
{
	XLogRecPtr		xl_prev;		/* ptr to previous record in log */
	XLogRecPtr		xl_xact_prev;	/* ptr to previous record of this xact */
	TransactionId	xl_xid;			/* xact id */
	uint16			xl_len;			/* len of record on this page */
	uint8			xl_info;
	RmgrId			xl_rmid;		/* resource manager inserted this record */

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogRecord;

#define	SizeOfXLogRecord	DOUBLEALIGN(sizeof(XLogRecord))
#define	MAXLOGRECSZ			(2 * BLCKSZ)
/*
 * When there is no space on current page we continue on the next
 * page with subrecord.
 */
typedef struct XLogSubRecord
{
	uint16			xl_len;
	uint8			xl_info;

	/* ACTUAL LOG DATA FOLLOWS AT END OF STRUCT */

} XLogSubRecord;

#define	SizeOfXLogSubRecord	DOUBLEALIGN(sizeof(XLogSubRecord))

#define	XLR_TO_BE_CONTINUED		0x01

#define	XLOG_PAGE_MAGIC	0x17345168

typedef struct XLogPageHeaderData
{
	uint32		xlp_magic;
	uint16		xlp_info;
} XLogPageHeaderData;

#define	SizeOfXLogPHD	DOUBLEALIGN(sizeof(XLogPageHeaderData))

typedef XLogPageHeaderData *XLogPageHeader;

#define	XLP_FIRST_IS_SUBRECORD	0x0001

extern XLogRecPtr		XLogInsert(RmgrId rmid, char *hdr, uint32 hdrlen, 
									char *buf, uint32 buflen);
extern void				XLogFlush(XLogRecPtr RecPtr);

#endif	/* XLOG_H */
