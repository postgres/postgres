/*-------------------------------------------------------------------------
 *
 * xlog.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/access/transam/xlog.c,v 1.51 2001/01/24 19:42:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef USE_LOCALE
#include <locale.h>
#endif

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catversion.h"
#include "storage/sinval.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "storage/s_lock.h"
#include "storage/bufpage.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "utils/builtins.h"
#include "utils/relcache.h"

#include "miscadmin.h"

int			XLOGbuffers = 8;
int			XLOGfiles = 0;	/* how many files to pre-allocate */
XLogRecPtr	MyLastRecPtr = {0, 0};
bool		InRecovery = false;
StartUpID	ThisStartUpID = 0;
XLogRecPtr	RedoRecPtr;

int			XLOG_DEBUG = 0;

/* To read/update control file and create new log file */
SPINLOCK	ControlFileLockId;

/* To generate new xid */
SPINLOCK	XidGenLockId;

static char		XLogDir[MAXPGPATH];
static char		ControlFilePath[MAXPGPATH];

#define MinXLOGbuffers	4

typedef struct XLgwrRqst
{
	XLogRecPtr	Write;			/* byte (1-based) to write out */
	XLogRecPtr	Flush;			/* byte (1-based) to flush */
} XLgwrRqst;

typedef struct XLgwrResult
{
	XLogRecPtr	Write;			/* bytes written out */
	XLogRecPtr	Flush;			/* bytes flushed */
} XLgwrResult;

typedef struct XLogCtlInsert
{
	XLgwrResult		LgwrResult;
	XLogRecPtr		PrevRecord;
	uint16			curridx;		/* current block index in cache */
	XLogPageHeader	currpage;
	char		   *currpos;
	XLogRecPtr		RedoRecPtr;
} XLogCtlInsert;

typedef struct XLogCtlWrite
{
	XLgwrResult LgwrResult;
	uint16		curridx;		/* index of next block to write */
} XLogCtlWrite;


typedef struct XLogCtlData
{
	XLogCtlInsert	Insert;
	XLgwrRqst		LgwrRqst;
	XLgwrResult		LgwrResult;
	XLogCtlWrite	Write;
	char		   *pages;
	XLogRecPtr	   *xlblocks;		/* 1st byte ptr-s + BLCKSZ */
	uint32			XLogCacheByte;
	uint32			XLogCacheBlck;
	StartUpID		ThisStartUpID;
	XLogRecPtr		RedoRecPtr;		/* for postmaster */
	slock_t			insert_lck;
	slock_t			info_lck;
	slock_t			lgwr_lck;
	slock_t			chkp_lck;		/* checkpoint lock */
} XLogCtlData;

static XLogCtlData *XLogCtl = NULL;

/*
 * Contents of pg_control
 */

typedef enum DBState
{
	DB_STARTUP = 0,
	DB_SHUTDOWNED,
	DB_SHUTDOWNING,
	DB_IN_RECOVERY,
	DB_IN_PRODUCTION
} DBState;

#define LOCALE_NAME_BUFLEN  128

typedef struct ControlFileData
{
	crc64		crc;
	uint32		logId;			/* current log file id */
	uint32		logSeg;			/* current log file segment (1-based) */
	XLogRecPtr	checkPoint;		/* last check point record ptr */
	time_t		time;			/* time stamp of last modification */
	DBState		state;			/* see enum above */

	/*
	 * this data is used to make sure that configuration of this DB is
	 * compatible with the backend executable
	 */
	uint32		blcksz;			/* block size for this DB */
	uint32		relseg_size;	/* blocks per segment of large relation */
	uint32		catalog_version_no;		/* internal version number */
	/* active locales --- "C" if compiled without USE_LOCALE: */
	char		lc_collate[LOCALE_NAME_BUFLEN];
	char		lc_ctype[LOCALE_NAME_BUFLEN];

	/*
	 * important directory locations
	 */
	char		archdir[MAXPGPATH];		/* where to move offline log files */
} ControlFileData;

static ControlFileData *ControlFile = NULL;

typedef struct CheckPoint
{
	XLogRecPtr		redo;		/* next RecPtr available when we */
								/* began to create CheckPoint */
								/* (i.e. REDO start point) */
	XLogRecPtr		undo;		/* first record of oldest in-progress */
								/* transaction when we started */
								/* (i.e. UNDO end point) */
	StartUpID		ThisStartUpID;
	TransactionId	nextXid;
	Oid				nextOid;
	bool			Shutdown;
} CheckPoint;

#define XLOG_CHECKPOINT		0x00
#define XLOG_NEXTOID		0x10

typedef struct BkpBlock
{
	crc64			crc;
	RelFileNode		node;
	BlockNumber		block;
} BkpBlock;

/*
 * We break each log file in 16Mb segments
 */
#define XLogSegSize		(16*1024*1024)
#define XLogLastSeg		(0xffffffff / XLogSegSize)
#define XLogFileSize	(XLogLastSeg * XLogSegSize)

#define NextLogSeg(_logId, _logSeg)		\
{\
	if (_logSeg >= XLogLastSeg)\
	{\
		_logId++;\
		_logSeg = 0;\
	}\
	else\
		_logSeg++;\
}


#define XLogFileName(path, log, seg)	\
			snprintf(path, MAXPGPATH, "%s%c%08X%08X",	\
					 XLogDir, SEP_CHAR, log, seg)

#define XLogTempFileName(path, log, seg)	\
			snprintf(path, MAXPGPATH, "%s%cT%08X%08X",	\
					 XLogDir, SEP_CHAR, log, seg)

#define PrevBufIdx(curridx)		\
		((curridx == 0) ? XLogCtl->XLogCacheBlck : (curridx - 1))

#define NextBufIdx(curridx)		\
		((curridx == XLogCtl->XLogCacheBlck) ? 0 : (curridx + 1))

#define InitXLBuffer(curridx)	(\
				XLogCtl->xlblocks[curridx].xrecoff = \
				(XLogCtl->xlblocks[Insert->curridx].xrecoff == XLogFileSize) ? \
				BLCKSZ : (XLogCtl->xlblocks[Insert->curridx].xrecoff + BLCKSZ), \
				XLogCtl->xlblocks[curridx].xlogid = \
				(XLogCtl->xlblocks[Insert->curridx].xrecoff == XLogFileSize) ? \
				(XLogCtl->xlblocks[Insert->curridx].xlogid + 1) : \
				XLogCtl->xlblocks[Insert->curridx].xlogid, \
				Insert->curridx = curridx, \
				Insert->currpage = (XLogPageHeader) (XLogCtl->pages + curridx * BLCKSZ), \
				Insert->currpos = \
					((char*) Insert->currpage) + SizeOfXLogPHD, \
				Insert->currpage->xlp_magic = XLOG_PAGE_MAGIC, \
				Insert->currpage->xlp_info = 0 \
				)

#define XRecOffIsValid(xrecoff) \
		(xrecoff % BLCKSZ >= SizeOfXLogPHD && \
		(BLCKSZ - xrecoff % BLCKSZ) >= SizeOfXLogRecord)

#define _INTL_MAXLOGRECSZ	(3 * MAXLOGRECSZ)

extern uint32	crc_table[];
#define INIT_CRC64(crc)		(crc.crc1 = 0xffffffff, crc.crc2 = 0xffffffff)
#define FIN_CRC64(crc)		(crc.crc1 ^= 0xffffffff, crc.crc2 ^= 0xffffffff)
#define COMP_CRC64(crc, data, len)	\
{\
	uint32		__c1 = crc.crc1;\
	uint32		__c2 = crc.crc2;\
	char	   *__data = data;\
	uint32		__len = len;\
\
	while (__len >= 2)\
	{\
		__c1 = crc_table[(__c1 ^ *__data++) & 0xff] ^ (__c1 >> 8);\
		__c2 = crc_table[(__c2 ^ *__data++) & 0xff] ^ (__c2 >> 8);\
		__len -= 2;\
	}\
	if (__len > 0)\
		__c1 = crc_table[(__c1 ^ *__data++) & 0xff] ^ (__c1 >> 8);\
	crc.crc1 = __c1;\
	crc.crc2 = __c2;\
}

void SetRedoRecPtr(void);
void GetRedoRecPtr(void);

static void GetFreeXLBuffer(void);
static void XLogWrite(char *buffer);
static int	XLogFileInit(uint32 log, uint32 seg, bool *usexistent);
static int	XLogFileOpen(uint32 log, uint32 seg, bool econt);
static XLogRecord *ReadRecord(XLogRecPtr *RecPtr, char *buffer);
static void WriteControlFile(void);
static void ReadControlFile(void);
static char *str_time(time_t tnow);
static void xlog_outrec(char *buf, XLogRecord *record);

static XLgwrResult LgwrResult = {{0, 0}, {0, 0}};
static XLgwrRqst LgwrRqst = {{0, 0}, {0, 0}};

static int	logFile = -1;
static uint32 logId = 0;
static uint32 logSeg = 0;
static uint32 logOff = 0;

static XLogRecPtr ReadRecPtr;
static XLogRecPtr EndRecPtr;
static int	readFile = -1;
static uint32 readId = 0;
static uint32 readSeg = 0;
static uint32 readOff = 0;
static char readBuf[BLCKSZ];
static XLogRecord *nextRecord = NULL;

static bool InRedo = false;

XLogRecPtr
XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata)
{
	XLogCtlInsert  *Insert = &XLogCtl->Insert;
	XLogRecord	   *record;
	XLogSubRecord  *subrecord;
	XLogRecPtr		RecPtr;
	uint32			freespace;
	uint16			curridx;
	XLogRecData	   *rdt;
	Buffer			dtbuf[2] = {InvalidBuffer, InvalidBuffer};
	bool			dtbuf_bkp[2] = {false, false};
	XLogRecData		dtbuf_rdt[4];
	BkpBlock		dtbuf_xlg[2];
	XLogRecPtr		dtbuf_lsn[2];
	crc64			dtbuf_crc[2],
					rdata_crc;
	uint32			len;
	unsigned		i;
	bool			updrqst = false;
	bool			repeat = false;
	bool			no_tran = (rmid == RM_XLOG_ID) ? true : false;

	if (info & XLR_INFO_MASK)
	{
		if ((info & XLR_INFO_MASK) != XLOG_NO_TRAN)
			elog(STOP, "XLogInsert: invalid info mask %02X", 
				(info & XLR_INFO_MASK));
		no_tran = true;
		info &= ~XLR_INFO_MASK;
	}

	if (IsBootstrapProcessingMode() && rmid != RM_XLOG_ID)
	{
		RecPtr.xlogid = 0;
		RecPtr.xrecoff = SizeOfXLogPHD;	/* start of 1st checkpoint record */
		return (RecPtr);
	}

begin:;
	INIT_CRC64(rdata_crc);
	for (len = 0, rdt = rdata; ; )
	{
		if (rdt->buffer == InvalidBuffer)
		{
			len += rdt->len;
			COMP_CRC64(rdata_crc, rdt->data, rdt->len);
			if (rdt->next == NULL)
				break;
			rdt = rdt->next;
			continue;
		}
		for (i = 0; i < 2; i++)
		{
			if (rdt->buffer == dtbuf[i])
			{
				if (dtbuf_bkp[i])
					rdt->data = NULL;
				else if (rdt->data)
				{
					len += rdt->len;
					COMP_CRC64(rdata_crc, rdt->data, rdt->len);
				}
				break;
			}
			if (dtbuf[i] == InvalidBuffer)
			{
				dtbuf[i] = rdt->buffer;
				dtbuf_lsn[i] = *((XLogRecPtr*)(BufferGetBlock(rdt->buffer)));
				if (XLByteLE(dtbuf_lsn[i], RedoRecPtr))
				{
					crc64	crc;

					dtbuf_bkp[i] = true;
					rdt->data = NULL;
					INIT_CRC64(crc);
					COMP_CRC64(crc, ((char*)BufferGetBlock(dtbuf[i])), BLCKSZ);
					dtbuf_crc[i] = crc;
				}
				else if (rdt->data)
				{
					len += rdt->len;
					COMP_CRC64(rdata_crc, rdt->data, rdt->len);
				}
				break;
			}
		}
		if (i >= 2)
			elog(STOP, "XLogInsert: can backup 2 blocks at most");
		if (rdt->next == NULL)
			break;
		rdt = rdt->next;
	}

	if (len == 0 || len > MAXLOGRECSZ)
		elog(STOP, "XLogInsert: invalid record len %u", len);

	START_CRIT_SECTION();

	/* obtain xlog insert lock */
	if (TAS(&(XLogCtl->insert_lck)))	/* busy */
	{
		bool		do_lgwr = true;

		for (i = 0;;)
		{
			/* try to read LgwrResult while waiting for insert lock */
			if (!TAS(&(XLogCtl->info_lck)))
			{
				LgwrRqst = XLogCtl->LgwrRqst;
				LgwrResult = XLogCtl->LgwrResult;
				S_UNLOCK(&(XLogCtl->info_lck));

				/*
				 * If cache is half filled then try to acquire lgwr lock
				 * and do LGWR work, but only once.
				 */
				if (do_lgwr &&
					(LgwrRqst.Write.xlogid != LgwrResult.Write.xlogid ||
					 (LgwrRqst.Write.xrecoff - LgwrResult.Write.xrecoff >=
					  XLogCtl->XLogCacheByte / 2)))
				{
					if (!TAS(&(XLogCtl->lgwr_lck)))
					{
						LgwrResult = XLogCtl->Write.LgwrResult;
						if (!TAS(&(XLogCtl->info_lck)))
						{
							LgwrRqst = XLogCtl->LgwrRqst;
							S_UNLOCK(&(XLogCtl->info_lck));
						}
						if (XLByteLT(LgwrResult.Write, LgwrRqst.Write))
						{
							XLogWrite(NULL);
							do_lgwr = false;
						}
						S_UNLOCK(&(XLogCtl->lgwr_lck));
					}
				}
			}
			S_LOCK_SLEEP(&(XLogCtl->insert_lck), i++);
			if (!TAS(&(XLogCtl->insert_lck)))
				break;
		}
	}

	/* Race condition: RedoRecPtr was changed */
	RedoRecPtr = Insert->RedoRecPtr;
	repeat = false;
	for (i = 0; i < 2; i++)
	{
		if (dtbuf[i] == InvalidBuffer)
			continue;
		if (dtbuf_bkp[i] == false &&
			XLByteLE(dtbuf_lsn[i], RedoRecPtr))
		{
			dtbuf[i] = InvalidBuffer;
			repeat = true;
		}
	}
	if (repeat)
	{
		S_UNLOCK(&(XLogCtl->insert_lck));
		END_CRIT_SECTION();
		goto begin;
	}

	/* Attach backup blocks to record data */
	for (i = 0; i < 2; i++)
	{
		if (dtbuf[i] == InvalidBuffer || !(dtbuf_bkp[i]))
			continue;

		info |= (XLR_SET_BKP_BLOCK(i));

		dtbuf_xlg[i].node = BufferGetFileNode(dtbuf[i]);
		dtbuf_xlg[i].block = BufferGetBlockNumber(dtbuf[i]);
		COMP_CRC64(dtbuf_crc[i], 
			((char*)&(dtbuf_xlg[i]) + offsetof(BkpBlock, node)),
			(sizeof(BkpBlock) - offsetof(BkpBlock, node)));
		FIN_CRC64(dtbuf_crc[i]);
		dtbuf_xlg[i].crc = dtbuf_crc[i];

		rdt->next = &(dtbuf_rdt[2 * i]);

		dtbuf_rdt[2 * i].data = (char*)&(dtbuf_xlg[i]);
		dtbuf_rdt[2 * i].len = sizeof(BkpBlock);
		len += sizeof(BkpBlock);

		rdt = dtbuf_rdt[2 * i].next = &(dtbuf_rdt[2 * i + 1]);

		dtbuf_rdt[2 * i + 1].data = (char*)(BufferGetBlock(dtbuf[i]));
		dtbuf_rdt[2 * i + 1].len = BLCKSZ;
		len += BLCKSZ;
		dtbuf_rdt[2 * i + 1].next = NULL;
	}

	/* Insert record */

	freespace = ((char *) Insert->currpage) + BLCKSZ - Insert->currpos;
	if (freespace < SizeOfXLogRecord)
	{
		curridx = NextBufIdx(Insert->curridx);
		if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			InitXLBuffer(curridx);
		else
			GetFreeXLBuffer();
		freespace = BLCKSZ - SizeOfXLogPHD;
	}
	else
		curridx = Insert->curridx;

	freespace -= SizeOfXLogRecord;
	record = (XLogRecord *) Insert->currpos;
	record->xl_prev = Insert->PrevRecord;
	if (no_tran)
	{
		record->xl_xact_prev.xlogid = 0;
		record->xl_xact_prev.xrecoff = 0;
	}
	else
		record->xl_xact_prev = MyLastRecPtr;

	record->xl_xid = GetCurrentTransactionId();
	record->xl_len = len;
	record->xl_info = info;
	record->xl_rmid = rmid;

	COMP_CRC64(rdata_crc, ((char*)record + offsetof(XLogRecord, xl_prev)), 
				(SizeOfXLogRecord - offsetof(XLogRecord, xl_prev)));
	FIN_CRC64(rdata_crc);
	record->xl_crc = rdata_crc;

	RecPtr.xlogid = XLogCtl->xlblocks[curridx].xlogid;
	RecPtr.xrecoff =
		XLogCtl->xlblocks[curridx].xrecoff - BLCKSZ +
		Insert->currpos - ((char *) Insert->currpage);
	if (MyLastRecPtr.xrecoff == 0 && !no_tran)
	{
		SpinAcquire(SInvalLock);
		MyProc->logRec = RecPtr;
		SpinRelease(SInvalLock);
	}
	Insert->PrevRecord = RecPtr;

	if (XLOG_DEBUG)
	{
		char	buf[8192];

		sprintf(buf, "INSERT @ %u/%u: ", RecPtr.xlogid, RecPtr.xrecoff);
		xlog_outrec(buf, record);
		if (rdata->data != NULL)
		{
			strcat(buf, " - ");
			RmgrTable[record->xl_rmid].rm_desc(buf, record->xl_info, rdata->data);
		}
		strcat(buf, "\n");
		write(2, buf, strlen(buf));
	}

	MyLastRecPtr = RecPtr;	/* begin of record */
	Insert->currpos += SizeOfXLogRecord;

	while (len)
	{
		while (rdata->data == NULL)
			rdata = rdata->next;

		if (freespace > 0)
		{
			if (rdata->len > freespace)
			{
				memcpy(Insert->currpos, rdata->data, freespace);
				rdata->data += freespace;
				rdata->len -= freespace;
				len -= freespace;
			}
			else
			{
				memcpy(Insert->currpos, rdata->data, rdata->len);
				freespace -= rdata->len;
				len -= rdata->len;
				Insert->currpos += rdata->len;
				rdata = rdata->next;
				continue;
			}
		}

		/* Use next buffer */
		curridx = NextBufIdx(curridx);
		if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
		{
			InitXLBuffer(curridx);
			updrqst = true;
		}
		else
			GetFreeXLBuffer();
		freespace = BLCKSZ - SizeOfXLogPHD - SizeOfXLogSubRecord;
		Insert->currpage->xlp_info |= XLP_FIRST_IS_SUBRECORD;
		subrecord = (XLogSubRecord *) Insert->currpos;
		subrecord->xl_len = len;
		Insert->currpos += SizeOfXLogSubRecord;
	}

	Insert->currpos = ((char *) Insert->currpage) +
			MAXALIGN(Insert->currpos - ((char *) Insert->currpage));
	freespace = ((char *) Insert->currpage) + BLCKSZ - Insert->currpos;

	/*
	 * Begin of the next record will be stored as LSN for
	 * changed data page...
	 */
	RecPtr.xlogid = XLogCtl->xlblocks[curridx].xlogid;
	RecPtr.xrecoff =
		XLogCtl->xlblocks[curridx].xrecoff - BLCKSZ +
		Insert->currpos - ((char *) Insert->currpage);

	/* Need to update global LgwrRqst if some block was filled up */
	if (freespace < SizeOfXLogRecord)
		updrqst = true;	/* curridx is filled and available for writing out */
	else
		curridx = PrevBufIdx(curridx);
	LgwrRqst.Write = XLogCtl->xlblocks[curridx];

	S_UNLOCK(&(XLogCtl->insert_lck));

	if (updrqst)
	{
		S_LOCK(&(XLogCtl->info_lck));
		if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrRqst.Write))
			XLogCtl->LgwrRqst.Write = LgwrRqst.Write;
		S_UNLOCK(&(XLogCtl->info_lck));
	}

	END_CRIT_SECTION();
	return (RecPtr);
}

void
XLogFlush(XLogRecPtr record)
{
	XLogRecPtr	WriteRqst;
	char		buffer[BLCKSZ];
	char	   *usebuf = NULL;
	unsigned	spins = 0;
	bool		force_lgwr = false;

	if (XLOG_DEBUG)
	{
		fprintf(stderr, "XLogFlush%s%s: rqst %u/%u; wrt %u/%u; flsh %u/%u\n",
			(IsBootstrapProcessingMode()) ? "(bootstrap)" : "",
			(InRedo) ? "(redo)" : "",
			record.xlogid, record.xrecoff,
			LgwrResult.Write.xlogid, LgwrResult.Write.xrecoff,
			LgwrResult.Flush.xlogid, LgwrResult.Flush.xrecoff);
		fflush(stderr);
	}

	if (InRedo)
		return;
	if (XLByteLE(record, LgwrResult.Flush))
		return;

	START_CRIT_SECTION();

	WriteRqst = LgwrRqst.Write;
	for (;;)
	{
		/* try to read LgwrResult */
		if (!TAS(&(XLogCtl->info_lck)))
		{
			LgwrResult = XLogCtl->LgwrResult;
			if (XLByteLE(record, LgwrResult.Flush))
			{
				S_UNLOCK(&(XLogCtl->info_lck));
				END_CRIT_SECTION();
				return;
			}
			if (XLByteLT(XLogCtl->LgwrRqst.Flush, record))
				XLogCtl->LgwrRqst.Flush = record;
			if (XLByteLT(WriteRqst, XLogCtl->LgwrRqst.Write))
			{
				WriteRqst = XLogCtl->LgwrRqst.Write;
				usebuf = NULL;
			}
			S_UNLOCK(&(XLogCtl->info_lck));
		}
		/* if something was added to log cache then try to flush this too */
		if (!TAS(&(XLogCtl->insert_lck)))
		{
			XLogCtlInsert *Insert = &XLogCtl->Insert;
			uint32		freespace =
			((char *) Insert->currpage) + BLCKSZ - Insert->currpos;

			if (freespace < SizeOfXLogRecord)	/* buffer is full */
			{
				usebuf = NULL;
				LgwrRqst.Write = WriteRqst = XLogCtl->xlblocks[Insert->curridx];
			}
			else
			{
				usebuf = buffer;
				memcpy(usebuf, Insert->currpage, BLCKSZ - freespace);
				memset(usebuf + BLCKSZ - freespace, 0, freespace);
				WriteRqst = XLogCtl->xlblocks[Insert->curridx];
				WriteRqst.xrecoff = WriteRqst.xrecoff - BLCKSZ +
					Insert->currpos - ((char *) Insert->currpage);
			}
			S_UNLOCK(&(XLogCtl->insert_lck));
			force_lgwr = true;
		}
		if (force_lgwr || WriteRqst.xlogid > record.xlogid ||
			(WriteRqst.xlogid == record.xlogid &&
			 WriteRqst.xrecoff >= record.xrecoff + BLCKSZ))
		{
			if (!TAS(&(XLogCtl->lgwr_lck)))
			{
				LgwrResult = XLogCtl->Write.LgwrResult;
				if (XLByteLE(record, LgwrResult.Flush))
				{
					S_UNLOCK(&(XLogCtl->lgwr_lck));
					END_CRIT_SECTION();
					return;
				}
				if (XLByteLT(LgwrResult.Write, WriteRqst))
				{
					LgwrRqst.Flush = LgwrRqst.Write = WriteRqst;
					XLogWrite(usebuf);
					S_UNLOCK(&(XLogCtl->lgwr_lck));
					if (XLByteLT(LgwrResult.Flush, record))
						elog(STOP, "XLogFlush: request is not satisfied");
					END_CRIT_SECTION();
					return;
				}
				break;
			}
		}
		S_LOCK_SLEEP(&(XLogCtl->lgwr_lck), spins++);
	}

	if (logFile >= 0 && (LgwrResult.Write.xlogid != logId ||
				 (LgwrResult.Write.xrecoff - 1) / XLogSegSize != logSeg))
	{
		if (close(logFile) != 0)
			elog(STOP, "close(logfile %u seg %u) failed: %m",
				 logId, logSeg);
		logFile = -1;
	}

	if (logFile < 0)
	{
		logId = LgwrResult.Write.xlogid;
		logSeg = (LgwrResult.Write.xrecoff - 1) / XLogSegSize;
		logOff = 0;
		logFile = XLogFileOpen(logId, logSeg, false);
	}

	if (pg_fsync(logFile) != 0)
		elog(STOP, "fsync(logfile %u seg %u) failed: %m",
			 logId, logSeg);
	LgwrResult.Flush = LgwrResult.Write;

	S_LOCK(&(XLogCtl->info_lck));
	XLogCtl->LgwrResult = LgwrResult;
	if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrResult.Write))
		XLogCtl->LgwrRqst.Write = LgwrResult.Write;
	S_UNLOCK(&(XLogCtl->info_lck));

	XLogCtl->Write.LgwrResult = LgwrResult;

	S_UNLOCK(&(XLogCtl->lgwr_lck));

	END_CRIT_SECTION();
	return;

}

static void
GetFreeXLBuffer()
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	XLogCtlWrite *Write = &XLogCtl->Write;
	uint16		curridx = NextBufIdx(Insert->curridx);
	unsigned	spins = 0;

	LgwrRqst.Write = XLogCtl->xlblocks[Insert->curridx];
	for (;;)
	{
		if (!TAS(&(XLogCtl->info_lck)))
		{
			LgwrResult = XLogCtl->LgwrResult;
			XLogCtl->LgwrRqst.Write = LgwrRqst.Write;
			S_UNLOCK(&(XLogCtl->info_lck));
			if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			{
				Insert->LgwrResult = LgwrResult;
				InitXLBuffer(curridx);
				return;
			}
		}

		/*
		 * LgwrResult lock is busy or un-updated. Try to acquire lgwr lock
		 * and write full blocks.
		 */
		if (!TAS(&(XLogCtl->lgwr_lck)))
		{
			LgwrResult = Write->LgwrResult;
			if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			{
				S_UNLOCK(&(XLogCtl->lgwr_lck));
				Insert->LgwrResult = LgwrResult;
				InitXLBuffer(curridx);
				return;
			}

			/*
			 * Have to write buffers while holding insert lock - not
			 * good...
			 */
			XLogWrite(NULL);
			S_UNLOCK(&(XLogCtl->lgwr_lck));
			Insert->LgwrResult = LgwrResult;
			InitXLBuffer(curridx);
			return;
		}
		S_LOCK_SLEEP(&(XLogCtl->lgwr_lck), spins++);
	}
}

static void
XLogWrite(char *buffer)
{
	XLogCtlWrite *Write = &XLogCtl->Write;
	char	   *from;
	uint32		wcnt = 0;
	bool		usexistent;

	for (; XLByteLT(LgwrResult.Write, LgwrRqst.Write);)
	{
		LgwrResult.Write = XLogCtl->xlblocks[Write->curridx];
		if (LgwrResult.Write.xlogid != logId ||
			(LgwrResult.Write.xrecoff - 1) / XLogSegSize != logSeg)
		{
			if (wcnt > 0)
			{
				if (pg_fsync(logFile) != 0)
					elog(STOP, "fsync(logfile %u seg %u) failed: %m",
						 logId, logSeg);
				if (LgwrResult.Write.xlogid != logId)
					LgwrResult.Flush.xrecoff = XLogFileSize;
				else
					LgwrResult.Flush.xrecoff = LgwrResult.Write.xrecoff - BLCKSZ;
				LgwrResult.Flush.xlogid = logId;
				if (!TAS(&(XLogCtl->info_lck)))
				{
					XLogCtl->LgwrResult.Flush = LgwrResult.Flush;
					XLogCtl->LgwrResult.Write = LgwrResult.Flush;
					if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrResult.Flush))
						XLogCtl->LgwrRqst.Write = LgwrResult.Flush;
					if (XLByteLT(XLogCtl->LgwrRqst.Flush, LgwrResult.Flush))
						XLogCtl->LgwrRqst.Flush = LgwrResult.Flush;
					S_UNLOCK(&(XLogCtl->info_lck));
				}
			}
			if (logFile >= 0)
			{
				if (close(logFile) != 0)
					elog(STOP, "close(logfile %u seg %u) failed: %m",
						 logId, logSeg);
				logFile = -1;
			}
			logId = LgwrResult.Write.xlogid;
			logSeg = (LgwrResult.Write.xrecoff - 1) / XLogSegSize;
			logOff = 0;
			SpinAcquire(ControlFileLockId);
			/* create/use new log file */
			usexistent = true;
			logFile = XLogFileInit(logId, logSeg, &usexistent);
			ControlFile->logId = logId;
			ControlFile->logSeg = logSeg + 1;
			ControlFile->time = time(NULL);
			UpdateControlFile();
			SpinRelease(ControlFileLockId);
			if (!usexistent)	/* there was no file */
				elog(LOG, "XLogWrite: new log file created - "
					"try to increase WAL_FILES");
		}

		if (logFile < 0)
		{
			logId = LgwrResult.Write.xlogid;
			logSeg = (LgwrResult.Write.xrecoff - 1) / XLogSegSize;
			logOff = 0;
			logFile = XLogFileOpen(logId, logSeg, false);
		}

		if (logOff != (LgwrResult.Write.xrecoff - BLCKSZ) % XLogSegSize)
		{
			logOff = (LgwrResult.Write.xrecoff - BLCKSZ) % XLogSegSize;
			if (lseek(logFile, (off_t) logOff, SEEK_SET) < 0)
				elog(STOP, "lseek(logfile %u seg %u off %u) failed: %m",
					 logId, logSeg, logOff);
		}

		if (buffer != NULL && XLByteLT(LgwrRqst.Write, LgwrResult.Write))
			from = buffer;
		else
			from = XLogCtl->pages + Write->curridx * BLCKSZ;

		if (write(logFile, from, BLCKSZ) != BLCKSZ)
			elog(STOP, "write(logfile %u seg %u off %u) failed: %m",
				 logId, logSeg, logOff);

		wcnt++;
		logOff += BLCKSZ;

		if (from != buffer)
			Write->curridx = NextBufIdx(Write->curridx);
		else
			LgwrResult.Write = LgwrRqst.Write;
	}
	if (wcnt == 0)
		elog(STOP, "XLogWrite: nothing written");

	if (XLByteLT(LgwrResult.Flush, LgwrRqst.Flush) &&
		XLByteLE(LgwrRqst.Flush, LgwrResult.Write))
	{
		if (pg_fsync(logFile) != 0)
			elog(STOP, "fsync(logfile %u seg %u) failed: %m",
				 logId, logSeg);
		LgwrResult.Flush = LgwrResult.Write;
	}

	S_LOCK(&(XLogCtl->info_lck));
	XLogCtl->LgwrResult = LgwrResult;
	if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrResult.Write))
		XLogCtl->LgwrRqst.Write = LgwrResult.Write;
	S_UNLOCK(&(XLogCtl->info_lck));

	Write->LgwrResult = LgwrResult;
}

static int
XLogFileInit(uint32 log, uint32 seg, bool *usexistent)
{
	char		path[MAXPGPATH];
	char		tpath[MAXPGPATH];
	int			fd;

	XLogFileName(path, log, seg);

	/*
	 * Try to use existent file (checkpoint maker
	 * creates it sometime).
	 */
	if (*usexistent)
	{
		fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			if (errno != ENOENT)
				elog(STOP, "InitOpen(logfile %u seg %u) failed: %m",
					logId, logSeg);
		}
		else
			return(fd);
		*usexistent = false;
	}

	XLogTempFileName(tpath, log, seg);
	unlink(tpath);
	unlink(path);

	fd = BasicOpenFile(tpath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "InitCreate(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (lseek(fd, XLogSegSize - 1, SEEK_SET) != (off_t) (XLogSegSize - 1))
		elog(STOP, "lseek(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (write(fd, "", 1) != 1)
		elog(STOP, "write(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (pg_fsync(fd) != 0)
		elog(STOP, "fsync(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (lseek(fd, 0, SEEK_SET) < 0)
		elog(STOP, "lseek(logfile %u seg %u off %u) failed: %m",
			 log, seg, 0);

	close(fd);

#ifndef __BEOS__
	if (link(tpath, path) < 0)
#else
	if (rename(tpath, path) < 0)
#endif
		elog(STOP, "InitRelink(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	unlink(tpath);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "InitReopen(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	return (fd);
}

static int
XLogFileOpen(uint32 log, uint32 seg, bool econt)
{
	char		path[MAXPGPATH];
	int			fd;

	XLogFileName(path, log, seg);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		if (econt && errno == ENOENT)
		{
			elog(LOG, "open(logfile %u seg %u) failed: %m",
				 logId, logSeg);
			return (fd);
		}
		abort();
		elog(STOP, "open(logfile %u seg %u) failed: %m",
			 logId, logSeg);
	}

	return (fd);
}

/*
 * (Re)move offline log files older or equal to passwd one
 */
static void
MoveOfflineLogs(char *archdir, uint32 _logId, uint32 _logSeg)
{
	DIR			   *xldir;
	struct dirent  *xlde;
	char			lastoff[32];
	char			path[MAXPGPATH];

	Assert(archdir[0] == 0);	/* ! implemented yet */

	xldir = opendir(XLogDir);
	if (xldir == NULL)
		elog(STOP, "MoveOfflineLogs: cannot open xlog dir: %m");

	sprintf(lastoff, "%08X%08X", _logId, _logSeg);

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) != 16 || 
			strspn(xlde->d_name, "0123456789ABCDEF") != 16)
			continue;
		if (strcmp(xlde->d_name, lastoff) > 0)
		{
			errno = 0;
			continue;
		}
		elog(LOG, "MoveOfflineLogs: %s %s", (archdir[0]) ? 
			"archive" : "remove", xlde->d_name);
		sprintf(path, "%s%c%s",	XLogDir, SEP_CHAR, xlde->d_name);
		if (archdir[0] == 0)
			unlink(path);
		errno = 0;
	}
	if (errno)
		elog(STOP, "MoveOfflineLogs: cannot read xlog dir: %m");
	closedir(xldir);
}

static void
RestoreBkpBlocks(XLogRecord *record, XLogRecPtr lsn)
{
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BkpBlock	bkpb;
	char	   *blk;
	int			i;

	for (i = 0, blk = (char*)XLogRecGetData(record) + record->xl_len; i < 2; i++)
	{
		if (!(record->xl_info & (XLR_SET_BKP_BLOCK(i))))
			continue;

		memcpy((char*)&bkpb, blk, sizeof(BkpBlock));
		blk += sizeof(BkpBlock);

		reln = XLogOpenRelation(true, record->xl_rmid, bkpb.node);

		if (reln)
		{
			buffer = XLogReadBuffer(true, reln, bkpb.block);
			if (BufferIsValid(buffer))
			{
				page = (Page) BufferGetPage(buffer);
				memcpy((char*)page, blk, BLCKSZ);
				PageSetLSN(page, lsn);
				PageSetSUI(page, ThisStartUpID);
				UnlockAndWriteBuffer(buffer);
			}
		}

		blk += BLCKSZ;
	}
}

static bool
RecordIsValid(XLogRecord *record, XLogRecPtr recptr, int emode)
{
	crc64		crc;
	crc64		cbuf;
	int			i;
	uint32		len = record->xl_len;
	char	   *blk;

	for (i = 0; i < 2; i++)
	{
		if (!(record->xl_info & (XLR_SET_BKP_BLOCK(i))))
			continue;

		if (len <= (sizeof(BkpBlock) + BLCKSZ))
		{
			elog(emode, "ReadRecord: record at %u/%u is too short to keep bkp block",
				recptr.xlogid, recptr.xrecoff);
			return(false);
		}
		len -= sizeof(BkpBlock);
		len -= BLCKSZ;
	}

	/* CRC of rmgr data */
	INIT_CRC64(crc);
	COMP_CRC64(crc, ((char*)XLogRecGetData(record)), len);
	COMP_CRC64(crc, ((char*)record + offsetof(XLogRecord, xl_prev)), 
				(SizeOfXLogRecord - offsetof(XLogRecord, xl_prev)));
	FIN_CRC64(crc);

	if (record->xl_crc.crc1 != crc.crc1 || record->xl_crc.crc2 != crc.crc2)
	{
		elog(emode, "ReadRecord: bad rmgr data CRC in record at %u/%u",
			recptr.xlogid, recptr.xrecoff);
		return(false);
	}

	if (record->xl_len == len)
		return(true);

	for (i = 0, blk = (char*)XLogRecGetData(record) + len; i < 2; i++)
	{
		if (!(record->xl_info & (XLR_SET_BKP_BLOCK(i))))
			continue;

		INIT_CRC64(crc);
		COMP_CRC64(crc, (blk + sizeof(BkpBlock)), BLCKSZ);
		COMP_CRC64(crc, (blk + offsetof(BkpBlock, node)),
			(sizeof(BkpBlock) - offsetof(BkpBlock, node)));
		FIN_CRC64(crc);
		memcpy((char*)&cbuf, blk, sizeof(crc64));

		if (cbuf.crc1 != crc.crc1 || cbuf.crc2 != crc.crc2)
		{
			elog(emode, "ReadRecord: bad bkp block %d CRC in record at %u/%u",
				i + 1, recptr.xlogid, recptr.xrecoff);
			return(false);
		}
		blk += sizeof(BkpBlock);
		blk += BLCKSZ;
	}

	record->xl_len = len;	/* !!! */

	return(true);
}

static XLogRecord *
ReadRecord(XLogRecPtr *RecPtr, char *buffer)
{
	XLogRecord *record;
	XLogRecPtr	tmpRecPtr = EndRecPtr;
	uint32		len;
	bool		nextmode = (RecPtr == NULL);
	int			emode = (nextmode) ? LOG : STOP;
	bool		noBlck = false;

	if (nextmode)
	{
		RecPtr = &tmpRecPtr;
		if (nextRecord != NULL)
		{
			record = nextRecord;
			goto got_record;
		}
		if (tmpRecPtr.xrecoff % BLCKSZ != 0)
			tmpRecPtr.xrecoff += (BLCKSZ - tmpRecPtr.xrecoff % BLCKSZ);
		if (tmpRecPtr.xrecoff >= XLogFileSize)
		{
			(tmpRecPtr.xlogid)++;
			tmpRecPtr.xrecoff = 0;
		}
		tmpRecPtr.xrecoff += SizeOfXLogPHD;
	}
	else if (!XRecOffIsValid(RecPtr->xrecoff))
		elog(STOP, "ReadRecord: invalid record offset at (%u, %u)",
			 RecPtr->xlogid, RecPtr->xrecoff);

	if (readFile >= 0 && (RecPtr->xlogid != readId ||
						  RecPtr->xrecoff / XLogSegSize != readSeg))
	{
		close(readFile);
		readFile = -1;
	}
	readId = RecPtr->xlogid;
	readSeg = RecPtr->xrecoff / XLogSegSize;
	if (readFile < 0)
	{
		noBlck = true;
		readFile = XLogFileOpen(readId, readSeg, nextmode);
		if (readFile < 0)
			goto next_record_is_invalid;
	}

	if (noBlck || readOff != (RecPtr->xrecoff % XLogSegSize) / BLCKSZ)
	{
		readOff = (RecPtr->xrecoff % XLogSegSize) / BLCKSZ;
		if (lseek(readFile, (off_t) (readOff * BLCKSZ), SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
			elog(STOP, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (((XLogPageHeader) readBuf)->xlp_magic != XLOG_PAGE_MAGIC)
		{
			elog(emode, "ReadRecord: invalid magic number %u in logfile %u seg %u off %u",
				 ((XLogPageHeader) readBuf)->xlp_magic,
				 readId, readSeg, readOff);
			goto next_record_is_invalid;
		}
	}
	if ((((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_SUBRECORD) &&
		RecPtr->xrecoff % BLCKSZ == SizeOfXLogPHD)
	{
		elog(emode, "ReadRecord: subrecord is requested by (%u, %u)",
			 RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	record = (XLogRecord *) ((char *) readBuf + RecPtr->xrecoff % BLCKSZ);

got_record:;
	if (record->xl_len == 0)
	{
		elog(emode, "ReadRecord: record with zero len at (%u, %u)",
			RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	if (record->xl_len > _INTL_MAXLOGRECSZ)
	{
		elog(emode, "ReadRecord: too long record len %u at (%u, %u)",
			record->xl_len, RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	if (record->xl_rmid > RM_MAX_ID)
	{
		elog(emode, "ReadRecord: invalid resource managed id %u at (%u, %u)",
			 record->xl_rmid, RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	nextRecord = NULL;
	len = BLCKSZ - RecPtr->xrecoff % BLCKSZ - SizeOfXLogRecord;
	if (record->xl_len > len)
	{
		XLogSubRecord  *subrecord;
		uint32			gotlen = len;

		memcpy(buffer, record, len + SizeOfXLogRecord);
		record = (XLogRecord *) buffer;
		buffer += len + SizeOfXLogRecord;
		for (;;)
		{
			readOff++;
			if (readOff == XLogSegSize / BLCKSZ)
			{
				readSeg++;
				if (readSeg == XLogLastSeg)
				{
					readSeg = 0;
					readId++;
				}
				close(readFile);
				readOff = 0;
				readFile = XLogFileOpen(readId, readSeg, nextmode);
				if (readFile < 0)
					goto next_record_is_invalid;
			}
			if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
				elog(STOP, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
					 readId, readSeg, readOff);
			if (((XLogPageHeader) readBuf)->xlp_magic != XLOG_PAGE_MAGIC)
			{
				elog(emode, "ReadRecord: invalid magic number %u in logfile %u seg %u off %u",
					 ((XLogPageHeader) readBuf)->xlp_magic,
					 readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			if (!(((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_SUBRECORD))
			{
				elog(emode, "ReadRecord: there is no subrecord flag in logfile %u seg %u off %u",
					 readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			subrecord = (XLogSubRecord *) ((char *) readBuf + SizeOfXLogPHD);
			if (subrecord->xl_len == 0 || 
				record->xl_len < (subrecord->xl_len + gotlen))
			{
				elog(emode, "ReadRecord: invalid subrecord len %u in logfile %u seg %u off %u",
					 subrecord->xl_len, readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			len = BLCKSZ - SizeOfXLogPHD - SizeOfXLogSubRecord;

			if (subrecord->xl_len > len)
			{
				memcpy(buffer, (char *) subrecord + SizeOfXLogSubRecord, len);
				gotlen += len;
				buffer += len;
				continue;
			}
			if (record->xl_len != (subrecord->xl_len + gotlen))
			{
				elog(emode, "ReadRecord: invalid len %u of constracted record in logfile %u seg %u off %u",
					 subrecord->xl_len + gotlen, readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			memcpy(buffer, (char *) subrecord + SizeOfXLogSubRecord, subrecord->xl_len);
			break;
		}
		if (!RecordIsValid(record, *RecPtr, emode))
			goto next_record_is_invalid;
		if (BLCKSZ - SizeOfXLogRecord >= MAXALIGN(subrecord->xl_len) + 
			SizeOfXLogPHD + SizeOfXLogSubRecord)
		{
			nextRecord = (XLogRecord *) ((char *) subrecord + 
				MAXALIGN(subrecord->xl_len) + SizeOfXLogSubRecord);
		}
		EndRecPtr.xlogid = readId;
		EndRecPtr.xrecoff = readSeg * XLogSegSize + readOff * BLCKSZ +
			SizeOfXLogPHD + SizeOfXLogSubRecord + 
			MAXALIGN(subrecord->xl_len);
		ReadRecPtr = *RecPtr;
		return (record);
	}
	if (!RecordIsValid(record, *RecPtr, emode))
		goto next_record_is_invalid;
	if (BLCKSZ - SizeOfXLogRecord >= MAXALIGN(record->xl_len) + 
		RecPtr->xrecoff % BLCKSZ + SizeOfXLogRecord)
		nextRecord = (XLogRecord *) ((char *) record + 
			MAXALIGN(record->xl_len) + SizeOfXLogRecord);
	EndRecPtr.xlogid = RecPtr->xlogid;
	EndRecPtr.xrecoff = RecPtr->xrecoff + 
		MAXALIGN(record->xl_len) + SizeOfXLogRecord;
	ReadRecPtr = *RecPtr;

	return (record);

next_record_is_invalid:;
	close(readFile);
	readFile = -1;
	nextRecord = NULL;
	memset(buffer, 0, SizeOfXLogRecord);
	record = (XLogRecord *) buffer;

	/*
	 * If we assumed that next record began on the same page where
	 * previous one ended - zero end of page.
	 */
	if (XLByteEQ(tmpRecPtr, EndRecPtr))
	{
		Assert(EndRecPtr.xrecoff % BLCKSZ > (SizeOfXLogPHD + SizeOfXLogSubRecord) &&
			   BLCKSZ - EndRecPtr.xrecoff % BLCKSZ >= SizeOfXLogRecord);
		readId = EndRecPtr.xlogid;
		readSeg = EndRecPtr.xrecoff / XLogSegSize;
		readOff = (EndRecPtr.xrecoff % XLogSegSize) / BLCKSZ;
		elog(LOG, "Formatting logfile %u seg %u block %u at offset %u",
			 readId, readSeg, readOff, EndRecPtr.xrecoff % BLCKSZ);
		readFile = XLogFileOpen(readId, readSeg, false);
		if (lseek(readFile, (off_t) (readOff * BLCKSZ), SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
			elog(STOP, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		memset(readBuf + EndRecPtr.xrecoff % BLCKSZ, 0,
			   BLCKSZ - EndRecPtr.xrecoff % BLCKSZ);
		if (lseek(readFile, (off_t) (readOff * BLCKSZ), SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (write(readFile, readBuf, BLCKSZ) != BLCKSZ)
			elog(STOP, "ReadRecord: write(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		readOff++;
	}
	else
	{
		Assert(EndRecPtr.xrecoff % BLCKSZ == 0 ||
			   BLCKSZ - EndRecPtr.xrecoff % BLCKSZ < SizeOfXLogRecord);
		readId = tmpRecPtr.xlogid;
		readSeg = tmpRecPtr.xrecoff / XLogSegSize;
		readOff = (tmpRecPtr.xrecoff % XLogSegSize) / BLCKSZ;
		Assert(readOff > 0);
	}
	if (readOff > 0)
	{
		if (!XLByteEQ(tmpRecPtr, EndRecPtr))
			elog(LOG, "Formatting logfile %u seg %u block %u at offset 0",
				 readId, readSeg, readOff);
		readOff *= BLCKSZ;
		memset(readBuf, 0, BLCKSZ);
		readFile = XLogFileOpen(readId, readSeg, false);
		if (lseek(readFile, (off_t) readOff, SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		while (readOff < XLogSegSize)
		{
			if (write(readFile, readBuf, BLCKSZ) != BLCKSZ)
				elog(STOP, "ReadRecord: write(logfile %u seg %u off %u) failed: %m",
					 readId, readSeg, readOff);
			readOff += BLCKSZ;
		}
	}
	if (readFile >= 0)
	{
		if (pg_fsync(readFile) < 0)
			elog(STOP, "ReadRecord: fsync(logfile %u seg %u) failed: %m",
				 readId, readSeg);
		close(readFile);
		readFile = -1;
	}

	readId = EndRecPtr.xlogid;
	readSeg = (EndRecPtr.xrecoff - 1) / XLogSegSize + 1;
	elog(LOG, "The last logId/logSeg is (%u, %u)", readId, readSeg - 1);
	if (ControlFile->logId != readId || ControlFile->logSeg != readSeg)
	{
		elog(LOG, "Set logId/logSeg in control file");
		ControlFile->logId = readId;
		ControlFile->logSeg = readSeg;
		ControlFile->time = time(NULL);
		UpdateControlFile();
	}
	if (readSeg == XLogLastSeg)
	{
		readSeg = 0;
		readId++;
	}
	{
		char		path[MAXPGPATH];

		XLogFileName(path, readId, readSeg);
		unlink(path);
	}

	return (record);
}

/*
 * I/O routines for pg_control
 *
 * *ControlFile is a buffer in shared memory that holds an image of the
 * contents of pg_control.  WriteControlFile() initializes pg_control
 * given a preloaded buffer, ReadControlFile() loads the buffer from
 * the pg_control file (during postmaster or standalone-backend startup),
 * and UpdateControlFile() rewrites pg_control after we modify xlog state.
 *
 * For simplicity, WriteControlFile() initializes the fields of pg_control
 * that are related to checking backend/database compatibility, and
 * ReadControlFile() verifies they are correct.  We could split out the
 * I/O and compatibility-check functions, but there seems no need currently.
 */

void
XLOGPathInit(void)
{
	/* Init XLOG file paths */
	snprintf(XLogDir, MAXPGPATH, "%s/pg_xlog", DataDir);
	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);
}

static void
WriteControlFile(void)
{
	int			fd;
	char		buffer[BLCKSZ];
#ifdef USE_LOCALE
	char	   *localeptr;
#endif

	/*
	 * Initialize compatibility-check fields
	 */
	ControlFile->blcksz = BLCKSZ;
	ControlFile->relseg_size = RELSEG_SIZE;
	ControlFile->catalog_version_no = CATALOG_VERSION_NO;
#ifdef USE_LOCALE
	localeptr = setlocale(LC_COLLATE, NULL);
	if (!localeptr)
		elog(STOP, "Invalid LC_COLLATE setting");
	StrNCpy(ControlFile->lc_collate, localeptr, LOCALE_NAME_BUFLEN);
	localeptr = setlocale(LC_CTYPE, NULL);
	if (!localeptr)
		elog(STOP, "Invalid LC_CTYPE setting");
	StrNCpy(ControlFile->lc_ctype, localeptr, LOCALE_NAME_BUFLEN);
	/*
	 * Issue warning notice if initdb'ing in a locale that will not permit
	 * LIKE index optimization.  This is not a clean place to do it, but
	 * I don't see a better place either...
	 */
	if (!locale_is_like_safe())
		elog(NOTICE, "Initializing database with %s collation order."
			 "\n\tThis locale setting will prevent use of index optimization for"
			 "\n\tLIKE and regexp searches.  If you are concerned about speed of"
			 "\n\tsuch queries, you may wish to set LC_COLLATE to \"C\" and"
			 "\n\tre-initdb.  For more information see the Administrator's Guide.",
			 ControlFile->lc_collate);
#else
	strcpy(ControlFile->lc_collate, "C");
	strcpy(ControlFile->lc_ctype, "C");
#endif

	/*
	 * We write out BLCKSZ bytes into pg_control, zero-padding the
	 * excess over sizeof(ControlFileData).  This reduces the odds
	 * of premature-EOF errors when reading pg_control.  We'll still
	 * fail when we check the contents of the file, but hopefully with
	 * a more specific error than "couldn't read pg_control".
	 */
	if (sizeof(ControlFileData) > BLCKSZ)
		elog(STOP, "sizeof(ControlFileData) is too large ... fix xlog.c");

	INIT_CRC64(ControlFile->crc);
	COMP_CRC64(ControlFile->crc, 
		((char*)ControlFile + offsetof(ControlFileData, logId)), 
		(sizeof(ControlFileData) - offsetof(ControlFileData, logId)));
	FIN_CRC64(ControlFile->crc);

	memset(buffer, 0, BLCKSZ);
	memcpy(buffer, ControlFile, sizeof(ControlFileData));

	fd = BasicOpenFile(ControlFilePath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "WriteControlFile failed to create control file (%s): %m",
			 ControlFilePath);

	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
		elog(STOP, "WriteControlFile failed to write control file: %m");

	if (pg_fsync(fd) != 0)
		elog(STOP, "WriteControlFile failed to fsync control file: %m");

	close(fd);
}

static void
ReadControlFile(void)
{
	crc64		crc;
	int			fd;

	/*
	 * Read data...
	 */
	fd = BasicOpenFile(ControlFilePath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "open(\"%s\") failed: %m", ControlFilePath);

	if (read(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
		elog(STOP, "read(\"%s\") failed: %m", ControlFilePath);

	close(fd);

	INIT_CRC64(crc);
	COMP_CRC64(crc, 
		((char*)ControlFile + offsetof(ControlFileData, logId)), 
		(sizeof(ControlFileData) - offsetof(ControlFileData, logId)));
	FIN_CRC64(crc);

	if (crc.crc1 != ControlFile->crc.crc1 || crc.crc2 != ControlFile->crc.crc2)
		elog(STOP, "Invalid CRC in control file");

	/*
	 * Do compatibility checking immediately.  We do this here for 2 reasons:
	 *
	 * (1) if the database isn't compatible with the backend executable,
	 * we want to abort before we can possibly do any damage;
	 *
	 * (2) this code is executed in the postmaster, so the setlocale() will
	 * propagate to forked backends, which aren't going to read this file
	 * for themselves.  (These locale settings are considered critical
	 * compatibility items because they can affect sort order of indexes.)
	 */
	if (ControlFile->blcksz != BLCKSZ)
		elog(STOP, "database was initialized with BLCKSZ %d,\n\tbut the backend was compiled with BLCKSZ %d.\n\tlooks like you need to initdb.",
			 ControlFile->blcksz, BLCKSZ);
	if (ControlFile->relseg_size != RELSEG_SIZE)
		elog(STOP, "database was initialized with RELSEG_SIZE %d,\n\tbut the backend was compiled with RELSEG_SIZE %d.\n\tlooks like you need to initdb.",
			 ControlFile->relseg_size, RELSEG_SIZE);
	if (ControlFile->catalog_version_no != CATALOG_VERSION_NO)
		elog(STOP, "database was initialized with CATALOG_VERSION_NO %d,\n\tbut the backend was compiled with CATALOG_VERSION_NO %d.\n\tlooks like you need to initdb.",
			 ControlFile->catalog_version_no, CATALOG_VERSION_NO);
#ifdef USE_LOCALE
	if (setlocale(LC_COLLATE, ControlFile->lc_collate) == NULL)
		elog(STOP, "database was initialized with LC_COLLATE '%s',\n\twhich is not recognized by setlocale().\n\tlooks like you need to initdb.",
			 ControlFile->lc_collate);
	if (setlocale(LC_CTYPE, ControlFile->lc_ctype) == NULL)
		elog(STOP, "database was initialized with LC_CTYPE '%s',\n\twhich is not recognized by setlocale().\n\tlooks like you need to initdb.",
			 ControlFile->lc_ctype);
#else
	if (strcmp(ControlFile->lc_collate, "C") != 0 ||
		strcmp(ControlFile->lc_ctype, "C") != 0)
		elog(STOP, "database was initialized with LC_COLLATE '%s' and LC_CTYPE '%s',\n\tbut the backend was compiled without locale support.\n\tlooks like you need to initdb or recompile.",
			 ControlFile->lc_collate, ControlFile->lc_ctype);
#endif
}

void
UpdateControlFile(void)
{
	int			fd;

	INIT_CRC64(ControlFile->crc);
	COMP_CRC64(ControlFile->crc, 
		((char*)ControlFile + offsetof(ControlFileData, logId)), 
		(sizeof(ControlFileData) - offsetof(ControlFileData, logId)));
	FIN_CRC64(ControlFile->crc);

	fd = BasicOpenFile(ControlFilePath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "open(\"%s\") failed: %m", ControlFilePath);

	if (write(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
		elog(STOP, "write(cntlfile) failed: %m");

	if (pg_fsync(fd) != 0)
		elog(STOP, "fsync(cntlfile) failed: %m");

	close(fd);
}

/*
 * Management of shared memory for XLOG
 */

int
XLOGShmemSize(void)
{
	if (XLOGbuffers < MinXLOGbuffers)
		XLOGbuffers = MinXLOGbuffers;

	return (sizeof(XLogCtlData) + BLCKSZ * XLOGbuffers +
			sizeof(XLogRecPtr) * XLOGbuffers +
			sizeof(ControlFileData));
}

void
XLOGShmemInit(void)
{
	bool		found;

	/* this must agree with space requested by XLOGShmemSize() */
	if (XLOGbuffers < MinXLOGbuffers)
		XLOGbuffers = MinXLOGbuffers;

	XLogCtl = (XLogCtlData *)
		ShmemInitStruct("XLOG Ctl", sizeof(XLogCtlData) + BLCKSZ * XLOGbuffers +
						sizeof(XLogRecPtr) * XLOGbuffers, &found);
	Assert(!found);
	ControlFile = (ControlFileData *)
		ShmemInitStruct("Control File", sizeof(ControlFileData), &found);
	Assert(!found);

	/*
	 * If we are not in bootstrap mode, pg_control should already exist.
	 * Read and validate it immediately (see comments in ReadControlFile()
	 * for the reasons why).
	 */
	if (!IsBootstrapProcessingMode())
		ReadControlFile();
}

/*
 * This func must be called ONCE on system install
 */
void
BootStrapXLOG()
{
	CheckPoint	checkPoint;
	char		buffer[BLCKSZ];
	bool        usexistent = false;
	XLogPageHeader page = (XLogPageHeader) buffer;
	XLogRecord *record;
	crc64		crc;

	checkPoint.redo.xlogid = 0;
	checkPoint.redo.xrecoff = SizeOfXLogPHD;
	checkPoint.undo = checkPoint.redo;
	checkPoint.nextXid = FirstTransactionId;
	checkPoint.nextOid = BootstrapObjectIdData;
	checkPoint.ThisStartUpID = 0;
	checkPoint.Shutdown = true;

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	memset(buffer, 0, BLCKSZ);
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = 0;
	record = (XLogRecord *) ((char *) page + SizeOfXLogPHD);
	record->xl_prev.xlogid = 0;
	record->xl_prev.xrecoff = 0;
	record->xl_xact_prev = record->xl_prev;
	record->xl_xid = InvalidTransactionId;
	record->xl_len = sizeof(checkPoint);
	record->xl_info = 0;
	record->xl_rmid = RM_XLOG_ID;
	memcpy((char *) record + SizeOfXLogRecord, &checkPoint, sizeof(checkPoint));

	INIT_CRC64(crc);
	COMP_CRC64(crc, ((char*)&checkPoint), sizeof(checkPoint));
	COMP_CRC64(crc, ((char*)record + offsetof(XLogRecord, xl_prev)), 
				(SizeOfXLogRecord - offsetof(XLogRecord, xl_prev)));
	FIN_CRC64(crc);
	record->xl_crc = crc;

	logFile = XLogFileInit(0, 0, &usexistent);

	if (write(logFile, buffer, BLCKSZ) != BLCKSZ)
		elog(STOP, "BootStrapXLOG failed to write logfile: %m");

	if (pg_fsync(logFile) != 0)
		elog(STOP, "BootStrapXLOG failed to fsync logfile: %m");

	close(logFile);
	logFile = -1;

	memset(ControlFile, 0, sizeof(ControlFileData));
	ControlFile->logId = 0;
	ControlFile->logSeg = 1;
	ControlFile->checkPoint = checkPoint.redo;
	ControlFile->time = time(NULL);
	ControlFile->state = DB_SHUTDOWNED;
	/* some additional ControlFile fields are set in WriteControlFile() */

	WriteControlFile();
}

static char *
str_time(time_t tnow)
{
	static char buf[20];

	strftime(buf, sizeof(buf),
			 "%Y-%m-%d %H:%M:%S",
			 localtime(&tnow));

	return buf;
}

/*
 * This func must be called ONCE on system startup
 */
void
StartupXLOG()
{
	XLogCtlInsert *Insert;
	CheckPoint	checkPoint;
	XLogRecPtr	RecPtr,
				LastRec;
	XLogRecord *record;
	char		buffer[_INTL_MAXLOGRECSZ + SizeOfXLogRecord];

	elog(LOG, "starting up");
	CritSectionCount++;

	XLogCtl->xlblocks = (XLogRecPtr *) (((char *) XLogCtl) + sizeof(XLogCtlData));
	XLogCtl->pages = ((char *) XLogCtl->xlblocks + sizeof(XLogRecPtr) * XLOGbuffers);
	XLogCtl->XLogCacheByte = BLCKSZ * XLOGbuffers;
	XLogCtl->XLogCacheBlck = XLOGbuffers - 1;
	memset(XLogCtl->xlblocks, 0, sizeof(XLogRecPtr) * XLOGbuffers);
	XLogCtl->LgwrRqst = LgwrRqst;
	XLogCtl->LgwrResult = LgwrResult;
	XLogCtl->Insert.LgwrResult = LgwrResult;
	XLogCtl->Insert.curridx = 0;
	XLogCtl->Insert.currpage = (XLogPageHeader) (XLogCtl->pages);
	XLogCtl->Write.LgwrResult = LgwrResult;
	XLogCtl->Write.curridx = 0;
	S_INIT_LOCK(&(XLogCtl->insert_lck));
	S_INIT_LOCK(&(XLogCtl->info_lck));
	S_INIT_LOCK(&(XLogCtl->lgwr_lck));
	S_INIT_LOCK(&(XLogCtl->chkp_lck));

	/*
	 * Read control file and check XLOG status looks valid.
	 *
	 * Note: in most control paths, *ControlFile is already valid and we
	 * need not do ReadControlFile() here, but might as well do it to be sure.
	 */
	ReadControlFile();

	if (ControlFile->logSeg == 0 ||
		ControlFile->time <= 0 ||
		ControlFile->state < DB_SHUTDOWNED ||
		ControlFile->state > DB_IN_PRODUCTION ||
		!XRecOffIsValid(ControlFile->checkPoint.xrecoff))
		elog(STOP, "control file context is broken");

	if (ControlFile->state == DB_SHUTDOWNED)
		elog(LOG, "database system was shut down at %s",
			 str_time(ControlFile->time));
	else if (ControlFile->state == DB_SHUTDOWNING)
		elog(LOG, "database system shutdown was interrupted at %s",
			 str_time(ControlFile->time));
	else if (ControlFile->state == DB_IN_RECOVERY)
		elog(LOG, "database system was interrupted being in recovery at %s\n"
			 "\tThis propably means that some data blocks are corrupted\n"
			 "\tand you will have to use last backup for recovery.",
			 str_time(ControlFile->time));
	else if (ControlFile->state == DB_IN_PRODUCTION)
		elog(LOG, "database system was interrupted at %s",
			 str_time(ControlFile->time));

	LastRec = RecPtr = ControlFile->checkPoint;
	if (!XRecOffIsValid(RecPtr.xrecoff))
		elog(STOP, "Invalid checkPoint in control file");
	elog(LOG, "CheckPoint record at (%u, %u)", RecPtr.xlogid, RecPtr.xrecoff);

	record = ReadRecord(&RecPtr, buffer);
	if (record->xl_rmid != RM_XLOG_ID)
		elog(STOP, "Invalid RMID in checkPoint record");
	if (record->xl_len != sizeof(checkPoint))
		elog(STOP, "Invalid length of checkPoint record");
	checkPoint = *((CheckPoint *) ((char *) record + SizeOfXLogRecord));

	elog(LOG, "Redo record at (%u, %u); Undo record at (%u, %u); Shutdown %s",
		 checkPoint.redo.xlogid, checkPoint.redo.xrecoff,
		 checkPoint.undo.xlogid, checkPoint.undo.xrecoff,
		 (checkPoint.Shutdown) ? "TRUE" : "FALSE");
	elog(LOG, "NextTransactionId: %u; NextOid: %u",
		 checkPoint.nextXid, checkPoint.nextOid);
	if (checkPoint.nextXid < FirstTransactionId ||
		checkPoint.nextOid < BootstrapObjectIdData)
		elog(STOP, "Invalid NextTransactionId/NextOid");

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	ThisStartUpID = checkPoint.ThisStartUpID;
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = 
		XLogCtl->RedoRecPtr = checkPoint.redo;

	if (XLByteLT(RecPtr, checkPoint.redo))
		elog(STOP, "Invalid redo in checkPoint record");
	if (checkPoint.undo.xrecoff == 0)
		checkPoint.undo = RecPtr;

	if (XLByteLT(checkPoint.undo, RecPtr) || 
		XLByteLT(checkPoint.redo, RecPtr))
	{
		if (checkPoint.Shutdown)
			elog(STOP, "Invalid Redo/Undo record in shutdown checkpoint");
		if (ControlFile->state == DB_SHUTDOWNED)
			elog(STOP, "Invalid Redo/Undo record in shut down state");
		InRecovery = true;
	}
	else if (ControlFile->state != DB_SHUTDOWNED)
	{
		InRecovery = true;
	}

	/* REDO */
	if (InRecovery)
	{
		elog(LOG, "database system was not properly shut down; "
			 "automatic recovery in progress...");
		ControlFile->state = DB_IN_RECOVERY;
		ControlFile->time = time(NULL);
		UpdateControlFile();

		XLogOpenLogRelation();	/* open pg_log */
		XLogInitRelationCache();

		/* Is REDO required ? */
		if (XLByteLT(checkPoint.redo, RecPtr))
			record = ReadRecord(&(checkPoint.redo), buffer);
		else	/* read past CheckPoint record */
			record = ReadRecord(NULL, buffer);

		if (record->xl_len != 0)
		{
			InRedo = true;
			elog(LOG, "redo starts at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
			do
			{
				if (record->xl_xid >= ShmemVariableCache->nextXid)
					ShmemVariableCache->nextXid = record->xl_xid + 1;
				if (XLOG_DEBUG)
				{
					char	buf[8192];

					sprintf(buf, "REDO @ %u/%u; LSN %u/%u: ", 
						ReadRecPtr.xlogid, ReadRecPtr.xrecoff,
						EndRecPtr.xlogid, EndRecPtr.xrecoff);
					xlog_outrec(buf, record);
					strcat(buf, " - ");
					RmgrTable[record->xl_rmid].rm_desc(buf, 
						record->xl_info, XLogRecGetData(record));
					strcat(buf, "\n");
					write(2, buf, strlen(buf));
				}

				if (record->xl_info & (XLR_BKP_BLOCK_1|XLR_BKP_BLOCK_2))
					RestoreBkpBlocks(record, EndRecPtr);

				RmgrTable[record->xl_rmid].rm_redo(EndRecPtr, record);
				record = ReadRecord(NULL, buffer);
			} while (record->xl_len != 0);
			elog(LOG, "redo done at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
			LastRec = ReadRecPtr;
			InRedo = false;
		}
		else
			elog(LOG, "redo is not required");
	}

	/* Init xlog buffer cache */
	record = ReadRecord(&LastRec, buffer);
	logId = EndRecPtr.xlogid;
	logSeg = (EndRecPtr.xrecoff - 1) / XLogSegSize;
	logOff = 0;
	logFile = XLogFileOpen(logId, logSeg, false);
	XLogCtl->xlblocks[0].xlogid = logId;
	XLogCtl->xlblocks[0].xrecoff =
		((EndRecPtr.xrecoff - 1) / BLCKSZ + 1) * BLCKSZ;
	Insert = &XLogCtl->Insert;
	memcpy((char *) (Insert->currpage), readBuf, BLCKSZ);
	Insert->currpos = ((char *) Insert->currpage) +
		(EndRecPtr.xrecoff + BLCKSZ - XLogCtl->xlblocks[0].xrecoff);
	Insert->PrevRecord = LastRec;

	LgwrRqst.Write = LgwrRqst.Flush =
	LgwrResult.Write = LgwrResult.Flush = EndRecPtr;

	XLogCtl->Write.LgwrResult = LgwrResult;
	Insert->LgwrResult = LgwrResult;

	XLogCtl->LgwrRqst = LgwrRqst;
	XLogCtl->LgwrResult = LgwrResult;

#ifdef NOT_USED
	/* UNDO */
	if (InRecovery)
	{
		RecPtr = ReadRecPtr;
		if (XLByteLT(checkPoint.undo, RecPtr))
		{
			elog(LOG, "undo starts at (%u, %u)",
				 RecPtr.xlogid, RecPtr.xrecoff);
			do
			{
				record = ReadRecord(&RecPtr, buffer);
				if (TransactionIdIsValid(record->xl_xid) &&
					!TransactionIdDidCommit(record->xl_xid))
					RmgrTable[record->xl_rmid].rm_undo(EndRecPtr, record);
				RecPtr = record->xl_prev;
			} while (XLByteLE(checkPoint.undo, RecPtr));
			elog(LOG, "undo done at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
		}
		else
			elog(LOG, "undo is not required");
	}
#endif

	if (InRecovery)
	{
		CreateCheckPoint(true);
		XLogCloseRelationCache();
	}

	if (XLOGfiles > 0)		/* pre-allocate log files */
	{
		uint32	_logId = logId,
				_logSeg = logSeg;
		int		lf, i;
		bool	usexistent;

		for (i = 1; i <= XLOGfiles; i++)
		{
			NextLogSeg(_logId, _logSeg);
			usexistent = false;
			lf = XLogFileInit(_logId, _logSeg, &usexistent);
			close(lf);
		}
	}

	InRecovery = false;

	ControlFile->state = DB_IN_PRODUCTION;
	ControlFile->time = time(NULL);
	UpdateControlFile();

	ThisStartUpID++;
	XLogCtl->ThisStartUpID = ThisStartUpID;

	elog(LOG, "database system is in production state");
	CritSectionCount--;

	return;
}

/*
 * Postmaster uses it to set ThisStartUpID & RedoRecPtr from
 * XLogCtlData located in shmem after successful startup.
 */
void
SetThisStartUpID(void)
{
	ThisStartUpID = XLogCtl->ThisStartUpID;
	RedoRecPtr = XLogCtl->RedoRecPtr;
}

/*
 * CheckPoint-er called by postmaster creates copy of RedoRecPtr
 * for postmaster in shmem. Postmaster uses GetRedoRecPtr after
 * that to update its own copy of RedoRecPtr.
 */
void
SetRedoRecPtr(void)
{
	XLogCtl->RedoRecPtr = RedoRecPtr;
}

void
GetRedoRecPtr(void)
{
	RedoRecPtr = XLogCtl->RedoRecPtr;
}

/*
 * This func must be called ONCE on system shutdown
 */
void
ShutdownXLOG()
{
	elog(LOG, "shutting down");

	CritSectionCount++;
	CreateDummyCaches();
	CreateCheckPoint(true);
	CritSectionCount--;

	elog(LOG, "database system is shut down");
}

extern XLogRecPtr	GetUndoRecPtr(void);

void
CreateCheckPoint(bool shutdown)
{
	CheckPoint	checkPoint;
	XLogRecPtr	recptr;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	XLogRecData	rdata;
	uint32		freespace;
	uint16		curridx;
	uint32		_logId;
	uint32		_logSeg;
	char		archdir[MAXPGPATH];
	unsigned	spins = 0;

	if (MyLastRecPtr.xrecoff != 0)
		elog(ERROR, "CreateCheckPoint: cannot be called inside transaction block");
 
	START_CRIT_SECTION();

	/* Grab lock, using larger than normal sleep between tries (1 sec) */
	while (TAS(&(XLogCtl->chkp_lck)))
	{
		S_LOCK_SLEEP_INTERVAL(&(XLogCtl->chkp_lck), spins++, 1000000);
	}

	memset(&checkPoint, 0, sizeof(checkPoint));
	if (shutdown)
	{
		ControlFile->state = DB_SHUTDOWNING;
		ControlFile->time = time(NULL);
		UpdateControlFile();
	}
	checkPoint.ThisStartUpID = ThisStartUpID;
	checkPoint.Shutdown = shutdown;

	/* Get REDO record ptr */
	S_LOCK(&(XLogCtl->insert_lck));
	freespace = ((char *) Insert->currpage) + BLCKSZ - Insert->currpos;
	if (freespace < SizeOfXLogRecord)
	{
		curridx = NextBufIdx(Insert->curridx);
		if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			InitXLBuffer(curridx);
		else
			GetFreeXLBuffer();
		freespace = BLCKSZ - SizeOfXLogPHD;
	}
	else
		curridx = Insert->curridx;
	checkPoint.redo.xlogid = XLogCtl->xlblocks[curridx].xlogid;
	checkPoint.redo.xrecoff = XLogCtl->xlblocks[curridx].xrecoff - BLCKSZ +
		Insert->currpos - ((char *) Insert->currpage);
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;
	S_UNLOCK(&(XLogCtl->insert_lck));

	SpinAcquire(XidGenLockId);
	checkPoint.nextXid = ShmemVariableCache->nextXid;
	SpinRelease(XidGenLockId);
	SpinAcquire(OidGenLockId);
	checkPoint.nextOid = ShmemVariableCache->nextOid;
	if (!shutdown)
		checkPoint.nextOid += ShmemVariableCache->oidCount;

	SpinRelease(OidGenLockId);

	FlushBufferPool();

	/* Get UNDO record ptr - should use oldest of PROC->logRec */
	checkPoint.undo = GetUndoRecPtr();

	if (shutdown && checkPoint.undo.xrecoff != 0)
		elog(STOP, "Active transaction while data base is shutting down");

	rdata.buffer = InvalidBuffer;
	rdata.data = (char *)(&checkPoint);
	rdata.len = sizeof(checkPoint);
	rdata.next = NULL;

	recptr = XLogInsert(RM_XLOG_ID, XLOG_CHECKPOINT, &rdata);

	if (shutdown && !XLByteEQ(checkPoint.redo, MyLastRecPtr))
		elog(STOP, "XLog concurrent activity while data base is shutting down");

	XLogFlush(recptr);

	SpinAcquire(ControlFileLockId);
	if (shutdown)
	{
		/* probably should delete extra log files */
		ControlFile->state = DB_SHUTDOWNED;
	}
	else	/* create new log file(s) */
	{
		int		lf;
		bool	usexistent = true;

		_logId = recptr.xlogid;
		_logSeg = (recptr.xrecoff - 1) / XLogSegSize;
		if (XLOGfiles > 0)
		{
			struct timeval	delay;
			int				i;

			for (i = 1; i <= XLOGfiles; i++)
			{
				usexistent = true;
				NextLogSeg(_logId, _logSeg);
				lf = XLogFileInit(_logId, _logSeg, &usexistent);
				close(lf);
				/*
				 * Give up ControlFileLockId for 1/50 sec to let other
				 * backends switch to new log file in XLogWrite()
				 */
				SpinRelease(ControlFileLockId);
				delay.tv_sec = 0;
				delay.tv_usec = 20000;
				(void) select(0, NULL, NULL, NULL, &delay);
				SpinAcquire(ControlFileLockId);
			}
		}
		else if ((recptr.xrecoff - 1) % XLogSegSize >= 
			(uint32) (0.75 * XLogSegSize))
		{
			NextLogSeg(_logId, _logSeg);
			lf = XLogFileInit(_logId, _logSeg, &usexistent);
			close(lf);
		}
	}

	ControlFile->checkPoint = MyLastRecPtr;
	strcpy(archdir, ControlFile->archdir);
	ControlFile->time = time(NULL);
	UpdateControlFile();
	SpinRelease(ControlFileLockId);

	/*
	 * Delete offline log files. Get oldest online
	 * log file from redo or undo record, whatever
	 * is older.
	 */
	if (checkPoint.undo.xrecoff != 0 && 
		XLByteLT(checkPoint.undo, checkPoint.redo))
	{
		_logId = checkPoint.undo.xlogid;
		_logSeg = checkPoint.undo.xrecoff / XLogSegSize;
	}
	else
	{
		_logId = checkPoint.redo.xlogid;
		_logSeg = checkPoint.redo.xrecoff / XLogSegSize;
	}
	if (_logId || _logSeg)
	{
		if (_logSeg)
			_logSeg--;
		else
		{
			_logId--;
			_logSeg = 0;
		}
		MoveOfflineLogs(archdir, _logId, _logSeg);
	}

	S_UNLOCK(&(XLogCtl->chkp_lck));

	MyLastRecPtr.xrecoff = 0;	/* to avoid commit record */
	END_CRIT_SECTION();

	return;
}

void XLogPutNextOid(Oid nextOid);

void
XLogPutNextOid(Oid nextOid)
{
	XLogRecData		rdata;

	rdata.buffer = InvalidBuffer;
	rdata.data = (char *)(&nextOid);
	rdata.len = sizeof(Oid);
	rdata.next = NULL;
	(void) XLogInsert(RM_XLOG_ID, XLOG_NEXTOID, &rdata);
}

void
xlog_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8	info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_NEXTOID)
	{
		Oid		nextOid;

		memcpy(&nextOid, XLogRecGetData(record), sizeof(Oid));
		if (ShmemVariableCache->nextOid < nextOid)
			ShmemVariableCache->nextOid = nextOid;
	}
}
 
void
xlog_undo(XLogRecPtr lsn, XLogRecord *record)
{
}
 
void
xlog_desc(char *buf, uint8 xl_info, char* rec)
{
	uint8	info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_CHECKPOINT)
	{
		CheckPoint	*checkpoint = (CheckPoint*) rec;
		sprintf(buf + strlen(buf), "checkpoint: redo %u/%u; undo %u/%u; "
		"sui %u; xid %u; oid %u; %s",
			checkpoint->redo.xlogid, checkpoint->redo.xrecoff,
			checkpoint->undo.xlogid, checkpoint->undo.xrecoff,
			checkpoint->ThisStartUpID, checkpoint->nextXid, 
			checkpoint->nextOid,
			(checkpoint->Shutdown) ? "shutdown" : "online");
	}
	else if (info == XLOG_NEXTOID)
	{
		Oid		nextOid;

		memcpy(&nextOid, rec, sizeof(Oid));
		sprintf(buf + strlen(buf), "nextOid: %u", nextOid);
	}
	else
		strcat(buf, "UNKNOWN");
}

static void
xlog_outrec(char *buf, XLogRecord *record)
{
	int		bkpb;
	int		i;

	sprintf(buf + strlen(buf), "prev %u/%u; xprev %u/%u; xid %u",
		record->xl_prev.xlogid, record->xl_prev.xrecoff,
		record->xl_xact_prev.xlogid, record->xl_xact_prev.xrecoff,
		record->xl_xid);

	for (i = 0, bkpb = 0; i < 2; i++)
	{
		if (!(record->xl_info & (XLR_SET_BKP_BLOCK(i))))
			continue;
		bkpb++;
	}

	if (bkpb)
		sprintf(buf + strlen(buf), "; bkpb %d", bkpb);

	sprintf(buf + strlen(buf), ": %s",
		RmgrTable[record->xl_rmid].rm_name);
}
