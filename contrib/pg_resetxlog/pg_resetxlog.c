/*-------------------------------------------------------------------------
 *
 * pg_resetxlog.c
 *	  A utility to "zero out" the xlog when it's corrupt beyond recovery.
 *	  Can also rebuild pg_control if needed.
 *
 * The theory of operation is fairly simple:
 *    1. Read the existing pg_control (which will include the last
 *		 checkpoint record).  If it is an old format then update to
 *		 current format.
 *	  2. If pg_control is corrupt, attempt to intuit reasonable values,
 *		 by scanning the old xlog if necessary.
 *	  3. Modify pg_control to reflect a "shutdown" state with a checkpoint
 *	     record at the start of xlog.
 *	  4. Flush the existing xlog files and write a new segment with
 *	     just a checkpoint record in it.  The new segment is positioned
 *		 just past the end of the old xlog, so that existing LSNs in
 *		 data pages will appear to be "in the past".
 * This is all pretty straightforward except for the intuition part of
 * step 2 ...
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/contrib/pg_resetxlog/Attic/pg_resetxlog.c,v 1.2 2001/03/16 05:08:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#ifdef USE_LOCALE
#include <locale.h>
#endif

#include "access/xlog.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"


/******************** stuff copied from xlog.c ********************/

/* Increment an xlogid/segment pair */
#define NextLogSeg(logId, logSeg)	\
	do { \
		if ((logSeg) >= XLogSegsPerFile-1) \
		{ \
			(logId)++; \
			(logSeg) = 0; \
		} \
		else \
			(logSeg)++; \
	} while (0)

/*
 * Compute ID and segment from an XLogRecPtr.
 *
 * For XLByteToSeg, do the computation at face value.  For XLByteToPrevSeg,
 * a boundary byte is taken to be in the previous segment.  This is suitable
 * for deciding which segment to write given a pointer to a record end,
 * for example.
 */
#define XLByteToSeg(xlrp, logId, logSeg)	\
	( logId = (xlrp).xlogid, \
	  logSeg = (xlrp).xrecoff / XLogSegSize \
	)
#define XLByteToPrevSeg(xlrp, logId, logSeg)	\
	( logId = (xlrp).xlogid, \
	  logSeg = ((xlrp).xrecoff - 1) / XLogSegSize \
	)

/*
 * Is an XLogRecPtr within a particular XLOG segment?
 *
 * For XLByteInSeg, do the computation at face value.  For XLByteInPrevSeg,
 * a boundary byte is taken to be in the previous segment.
 */
#define XLByteInSeg(xlrp, logId, logSeg)	\
	((xlrp).xlogid == (logId) && \
	 (xlrp).xrecoff / XLogSegSize == (logSeg))

#define XLByteInPrevSeg(xlrp, logId, logSeg)	\
	((xlrp).xlogid == (logId) && \
	 ((xlrp).xrecoff - 1) / XLogSegSize == (logSeg))


#define XLogFileName(path, log, seg)	\
			snprintf(path, MAXPGPATH, "%s%c%08X%08X",	\
					 XLogDir, SEP_CHAR, log, seg)

/*
 * _INTL_MAXLOGRECSZ: max space needed for a record including header and
 * any backup-block data.
 */
#define _INTL_MAXLOGRECSZ	(SizeOfXLogRecord + MAXLOGRECSZ + \
							 XLR_MAX_BKP_BLOCKS * (sizeof(BkpBlock) + BLCKSZ))

/******************** end of stuff copied from xlog.c ********************/


static char *DataDir;			/* locations of important stuff */
static char XLogDir[MAXPGPATH];
static char ControlFilePath[MAXPGPATH];

static ControlFileData ControlFile;	/* pg_control values */
static uint32 newXlogId, newXlogSeg; /* ID/Segment of new XLOG segment */
static bool guessed = false;	/* T if we had to guess at any values */


static bool CheckControlVersion0(char *buffer, int len);


static int
XLogFileOpen(uint32 log, uint32 seg)
{
	char		path[MAXPGPATH];
	int			fd;

	XLogFileName(path, log, seg);

	fd = open(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	return (fd);
}


/*
 * Try to read the existing pg_control file.
 *
 * This routine is also responsible for updating old pg_control versions
 * to the current format.
 */
static bool
ReadControlFile(void)
{
	int		fd;
	int		len;
	char   *buffer;
	crc64	crc;

	if ((fd = open(ControlFilePath, O_RDONLY)) < 0)
	{
		/*
		 * If pg_control is not there at all, or we can't read it,
		 * the odds are we've been handed a bad DataDir path, so give up.
		 * User can do "touch pg_control" to force us to proceed.
		 */
		perror("Failed to open $PGDATA/global/pg_control for reading");
		if (errno == ENOENT)
			fprintf(stderr, "If you're sure the PGDATA path is correct, do\n"
					"  touch %s\n"
					"and try again.\n", ControlFilePath);
		exit(1);
	}

	/* Use malloc to ensure we have a maxaligned buffer */
	buffer = (char *) malloc(BLCKSZ);

	len = read(fd, buffer, BLCKSZ);
	if (len < 0)
	{
		perror("Failed to read $PGDATA/global/pg_control");
		exit(1);
	}
	close(fd);

	if (len >= sizeof(ControlFileData) &&
		((ControlFileData *) buffer)->pg_control_version == PG_CONTROL_VERSION)
	{
		/* Seems to be current version --- check the CRC. */
		INIT_CRC64(crc);
		COMP_CRC64(crc, 
				   buffer + sizeof(crc64),
				   sizeof(ControlFileData) - sizeof(crc64));
		FIN_CRC64(crc);

		if (EQ_CRC64(crc, ((ControlFileData *) buffer)->crc))
		{
			/* Valid data... */
			memcpy(&ControlFile, buffer, sizeof(ControlFile));
			return true;
		}

		fprintf(stderr, "pg_control exists but has invalid CRC; proceed with caution.\n");
		/* We will use the data anyway, but treat it as guessed. */
		memcpy(&ControlFile, buffer, sizeof(ControlFile));
		guessed = true;
		return true;
	}
	/*
	 * Maybe it's a 7.1beta pg_control.
	 */
	if (CheckControlVersion0(buffer, len))
		return true;

	/* Looks like it's a mess. */
	fprintf(stderr, "pg_control exists but is broken or unknown version; ignoring it.\n");
	return false;
}


/******************* routines for old XLOG format *******************/


/*
 * This format was in use in 7.1 beta releases through 7.1beta5.  The
 * pg_control layout was different, and so were the XLOG page headers.
 * The XLOG record header format was physically the same as 7.1 release,
 * but interpretation of the xl_len field was not.
 */

typedef struct crc64V0
{
	uint32			crc1;
	uint32			crc2;
} crc64V0;

static uint32 crc_tableV0[] = {
0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

#define INIT_CRC64V0(crc)	((crc).crc1 = 0xffffffff, (crc).crc2 = 0xffffffff)
#define FIN_CRC64V0(crc)	((crc).crc1 ^= 0xffffffff, (crc).crc2 ^= 0xffffffff)
#define COMP_CRC64V0(crc, data, len)	\
{\
        uint32       __c1 = (crc).crc1;\
        uint32       __c2 = (crc).crc2;\
        char        *__data = (char *) (data);\
        uint32       __len = (len);\
\
        while (__len >= 2)\
        {\
                __c1 = crc_tableV0[(__c1 ^ *__data++) & 0xff] ^ (__c1 >> 8);\
                __c2 = crc_tableV0[(__c2 ^ *__data++) & 0xff] ^ (__c2 >> 8);\
                __len -= 2;\
        }\
        if (__len > 0)\
                __c1 = crc_tableV0[(__c1 ^ *__data++) & 0xff] ^ (__c1 >> 8);\
        (crc).crc1 = __c1;\
        (crc).crc2 = __c2;\
}

#define EQ_CRC64V0(c1,c2)  ((c1).crc1 == (c2).crc1 && (c1).crc2 == (c2).crc2)


#define LOCALE_NAME_BUFLEN_V0  128

typedef struct ControlFileDataV0
{
	crc64V0			crc;
	uint32			logId;		/* current log file id */
	uint32			logSeg;		/* current log file segment (1-based) */
	XLogRecPtr		checkPoint;	/* last check point record ptr */
	time_t			time;		/* time stamp of last modification */
	DBState			state;		/* see enum above */
	uint32			blcksz;		/* block size for this DB */
	uint32			relseg_size; /* blocks per segment of large relation */
	uint32			catalog_version_no;	/* internal version number */
	char			lc_collate[LOCALE_NAME_BUFLEN_V0];
	char			lc_ctype[LOCALE_NAME_BUFLEN_V0];
	char			archdir[MAXPGPATH];	/* where to move offline log files */
} ControlFileDataV0;

typedef struct CheckPointV0
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
} CheckPointV0;

typedef struct XLogRecordV0
{
	crc64V0			xl_crc;
	XLogRecPtr		xl_prev;	/* ptr to previous record in log */
	XLogRecPtr		xl_xact_prev; /* ptr to previous record of this xact */
	TransactionId	xl_xid;		/* xact id */
	uint16			xl_len;		/* total len of record *data* */
	uint8			xl_info;
	RmgrId			xl_rmid;	/* resource manager inserted this record */
} XLogRecordV0;

#define SizeOfXLogRecordV0	DOUBLEALIGN(sizeof(XLogRecordV0))

typedef struct XLogContRecordV0
{
	uint16			xl_len;		/* len of data left */
} XLogContRecordV0;

#define SizeOfXLogContRecordV0	DOUBLEALIGN(sizeof(XLogContRecordV0))

#define XLOG_PAGE_MAGIC_V0 0x17345168

typedef struct XLogPageHeaderDataV0
{
	uint32                xlp_magic;
	uint16                xlp_info;
} XLogPageHeaderDataV0;

#define SizeOfXLogPHDV0   DOUBLEALIGN(sizeof(XLogPageHeaderDataV0))

typedef XLogPageHeaderDataV0 *XLogPageHeaderV0;


static bool RecordIsValidV0(XLogRecordV0 *record);
static XLogRecordV0 *ReadRecordV0(XLogRecPtr *RecPtr, char *buffer);
static bool ValidXLOGHeaderV0(XLogPageHeaderV0 hdr);


/*
 * Try to interpret pg_control contents as "version 0" format.
 */
static bool
CheckControlVersion0(char *buffer, int len)
{
	crc64V0		crc;
	ControlFileDataV0 *oldfile;
	XLogRecordV0 *record;
	CheckPointV0 *oldchkpt;

	if (len < sizeof(ControlFileDataV0))
		return false;
	/* Check CRC the version-0 way. */
	INIT_CRC64V0(crc);
	COMP_CRC64V0(crc, 
				 buffer + sizeof(crc64V0),
				 sizeof(ControlFileDataV0) - sizeof(crc64V0));
	FIN_CRC64V0(crc);

	if (!EQ_CRC64V0(crc, ((ControlFileDataV0 *) buffer)->crc))
		return false;

	/* Valid data, convert useful fields to new-style pg_control format */
	oldfile = (ControlFileDataV0 *) buffer;

	memset(&ControlFile, 0, sizeof(ControlFile));

	ControlFile.pg_control_version = PG_CONTROL_VERSION;
	ControlFile.catalog_version_no = oldfile->catalog_version_no;

	ControlFile.state = oldfile->state;
	ControlFile.logId = oldfile->logId;
	ControlFile.logSeg = oldfile->logSeg;

	ControlFile.blcksz = oldfile->blcksz;
	ControlFile.relseg_size = oldfile->relseg_size;
	strcpy(ControlFile.lc_collate, oldfile->lc_collate);
	strcpy(ControlFile.lc_ctype, oldfile->lc_ctype);

	/*
	 * Since this format did not include a copy of the latest checkpoint
	 * record, we have to go rooting in the old XLOG to get that.
	 */
	record = ReadRecordV0(&oldfile->checkPoint,
						  (char *) malloc(_INTL_MAXLOGRECSZ));
	if (record == NULL)
	{
		/*
		 * We have to guess at the checkpoint contents.
		 */
		guessed = true;
		ControlFile.checkPointCopy.ThisStartUpID = 0;
		ControlFile.checkPointCopy.nextXid = (TransactionId) 514; /* XXX */
		ControlFile.checkPointCopy.nextOid = BootstrapObjectIdData;
		return true;
	}
	oldchkpt = (CheckPointV0 *) XLogRecGetData(record);

	ControlFile.checkPointCopy.ThisStartUpID = oldchkpt->ThisStartUpID;
	ControlFile.checkPointCopy.nextXid = oldchkpt->nextXid;
	ControlFile.checkPointCopy.nextOid = oldchkpt->nextOid;

	return true;
}

/*
 * CRC-check an XLOG V0 record.  We do not believe the contents of an XLOG
 * record (other than to the minimal extent of computing the amount of
 * data to read in) until we've checked the CRCs.
 *
 * We assume all of the record has been read into memory at *record.
 */
static bool
RecordIsValidV0(XLogRecordV0 *record)
{
	crc64V0		crc;
	uint32		len = record->xl_len;

	/*
	 * NB: this code is not right for V0 records containing backup blocks,
	 * but for now it's only going to be applied to checkpoint records,
	 * so I'm not going to worry about it...
	 */
	INIT_CRC64V0(crc);
	COMP_CRC64V0(crc, XLogRecGetData(record), len);
	COMP_CRC64V0(crc, (char*) record + sizeof(crc64V0),
				 SizeOfXLogRecordV0 - sizeof(crc64V0));
	FIN_CRC64V0(crc);

	if (!EQ_CRC64V0(record->xl_crc, crc))
		return false;

	return(true);
}

/*
 * Attempt to read an XLOG V0 record at recptr.
 *
 * If no valid record is available, returns NULL.
 *
 * buffer is a workspace at least _INTL_MAXLOGRECSZ bytes long.  It is needed
 * to reassemble a record that crosses block boundaries.  Note that on
 * successful return, the returned record pointer always points at buffer.
 */
static XLogRecordV0 *
ReadRecordV0(XLogRecPtr *RecPtr, char *buffer)
{
	static int	readFile = -1;
	static uint32 readId = 0;
	static uint32 readSeg = 0;
	static uint32 readOff = 0;
	static char *readBuf = NULL;

	XLogRecordV0 *record;
	uint32		len,
				total_len;
	uint32		targetPageOff;

	if (readBuf == NULL)
		readBuf = (char *) malloc(BLCKSZ);

	XLByteToSeg(*RecPtr, readId, readSeg);
	if (readFile < 0)
	{
		readFile = XLogFileOpen(readId, readSeg);
		if (readFile < 0)
			goto next_record_is_invalid;
		readOff = (uint32) (-1); /* force read to occur below */
	}

	targetPageOff = ((RecPtr->xrecoff % XLogSegSize) / BLCKSZ) * BLCKSZ;
	if (readOff != targetPageOff)
	{
		readOff = targetPageOff;
		if (lseek(readFile, (off_t) readOff, SEEK_SET) < 0)
			goto next_record_is_invalid;
		if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
			goto next_record_is_invalid;
		if (!ValidXLOGHeaderV0((XLogPageHeaderV0) readBuf))
			goto next_record_is_invalid;
	}
	if ((((XLogPageHeaderV0) readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
		RecPtr->xrecoff % BLCKSZ == SizeOfXLogPHDV0)
		goto next_record_is_invalid;
	record = (XLogRecordV0 *) ((char *) readBuf + RecPtr->xrecoff % BLCKSZ);

	if (record->xl_len == 0)
		goto next_record_is_invalid;
	/*
	 * Compute total length of record including any appended backup blocks.
	 */
	total_len = SizeOfXLogRecordV0 + record->xl_len;
	/*
	 * Make sure it will fit in buffer (currently, it is mechanically
	 * impossible for this test to fail, but it seems like a good idea
	 * anyway).
	 */
	if (total_len > _INTL_MAXLOGRECSZ)
		goto next_record_is_invalid;
	len = BLCKSZ - RecPtr->xrecoff % BLCKSZ;
	if (total_len > len)
	{
		/* Need to reassemble record */
		XLogContRecordV0 *contrecord;
		uint32			gotlen = len;

		memcpy(buffer, record, len);
		record = (XLogRecordV0 *) buffer;
		buffer += len;
		for (;;)
		{
			readOff += BLCKSZ;
			if (readOff >= XLogSegSize)
			{
				close(readFile);
				readFile = -1;
				NextLogSeg(readId, readSeg);
				readFile = XLogFileOpen(readId, readSeg);
				if (readFile < 0)
					goto next_record_is_invalid;
				readOff = 0;
			}
			if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
				goto next_record_is_invalid;
			if (!ValidXLOGHeaderV0((XLogPageHeaderV0) readBuf))
				goto next_record_is_invalid;
			if (!(((XLogPageHeaderV0) readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD))
				goto next_record_is_invalid;
			contrecord = (XLogContRecordV0 *) ((char *) readBuf + SizeOfXLogPHDV0);
			if (contrecord->xl_len == 0 || 
				total_len != (contrecord->xl_len + gotlen))
				goto next_record_is_invalid;
			len = BLCKSZ - SizeOfXLogPHDV0 - SizeOfXLogContRecordV0;
			if (contrecord->xl_len > len)
			{
				memcpy(buffer, (char *)contrecord + SizeOfXLogContRecordV0, len);
				gotlen += len;
				buffer += len;
				continue;
			}
			memcpy(buffer, (char *) contrecord + SizeOfXLogContRecordV0,
				   contrecord->xl_len);
			break;
		}
		if (!RecordIsValidV0(record))
			goto next_record_is_invalid;
		return record;
	}

	/* Record does not cross a page boundary */
	if (!RecordIsValidV0(record))
		goto next_record_is_invalid;
	memcpy(buffer, record, total_len);
	return (XLogRecordV0 *) buffer;

next_record_is_invalid:;
	close(readFile);
	readFile = -1;
	return NULL;
}

/*
 * Check whether the xlog header of a page just read in looks valid.
 *
 * This is just a convenience subroutine to avoid duplicated code in
 * ReadRecord.  It's not intended for use from anywhere else.
 */
static bool
ValidXLOGHeaderV0(XLogPageHeaderV0 hdr)
{
	if (hdr->xlp_magic != XLOG_PAGE_MAGIC_V0)
		return false;
	if ((hdr->xlp_info & ~XLP_ALL_FLAGS) != 0)
		return false;
	return true;
}

/******************* end of routines for old XLOG format *******************/


/*
 * Guess at pg_control values when we can't read the old ones.
 */
static void
GuessControlValues(void)
{
#ifdef USE_LOCALE
	char	   *localeptr;
#endif

	/*
	 * Set up a completely default set of pg_control values.
	 */
	guessed = true;
	memset(&ControlFile, 0, sizeof(ControlFile));

	ControlFile.pg_control_version = PG_CONTROL_VERSION;
	ControlFile.catalog_version_no = CATALOG_VERSION_NO;

	ControlFile.checkPointCopy.redo.xlogid = 0;
	ControlFile.checkPointCopy.redo.xrecoff = SizeOfXLogPHD;
	ControlFile.checkPointCopy.undo = ControlFile.checkPointCopy.redo;
	ControlFile.checkPointCopy.ThisStartUpID = 0;
	ControlFile.checkPointCopy.nextXid = (TransactionId) 514; /* XXX */
	ControlFile.checkPointCopy.nextOid = BootstrapObjectIdData;
	ControlFile.checkPointCopy.time = time(NULL);

	ControlFile.state = DB_SHUTDOWNED;
	ControlFile.time = time(NULL);
	ControlFile.logId = 0;
	ControlFile.logSeg = 1;
	ControlFile.checkPoint = ControlFile.checkPointCopy.redo;

	ControlFile.blcksz = BLCKSZ;
	ControlFile.relseg_size = RELSEG_SIZE;
#ifdef USE_LOCALE
	localeptr = setlocale(LC_COLLATE, "");
	if (!localeptr)
	{
		fprintf(stderr, "Invalid LC_COLLATE setting\n");
		exit(1);
	}
	StrNCpy(ControlFile.lc_collate, localeptr, LOCALE_NAME_BUFLEN);
	localeptr = setlocale(LC_CTYPE, "");
	if (!localeptr)
	{
		fprintf(stderr, "Invalid LC_CTYPE setting\n");
		exit(1);
	}
	StrNCpy(ControlFile.lc_ctype, localeptr, LOCALE_NAME_BUFLEN);
#else
	strcpy(ControlFile.lc_collate, "C");
	strcpy(ControlFile.lc_ctype, "C");
#endif

	/*
	 * XXX eventually, should try to grovel through old XLOG to develop
	 * more accurate values for startupid, nextXID, and nextOID.
	 */
}


/*
 * Print the guessed pg_control values when we had to guess.
 *
 * NB: this display should be just those fields that will not be
 * reset by RewriteControlFile().
 */
static void
PrintControlValues(void)
{
	printf("Guessed-at pg_control values:\n\n"
		   "pg_control version number:            %u\n"
		   "Catalog version number:               %u\n"
		   "Current log file id:                  %u\n"
	       "Next log file segment:                %u\n"
		   "Latest checkpoint's StartUpID:        %u\n"
		   "Latest checkpoint's NextXID:          %u\n"
		   "Latest checkpoint's NextOID:          %u\n"
		   "Database block size:                  %u\n"
		   "Blocks per segment of large relation: %u\n"
		   "LC_COLLATE:                           %s\n"
		   "LC_CTYPE:                             %s\n",

		   ControlFile.pg_control_version,
		   ControlFile.catalog_version_no,
		   ControlFile.logId,
		   ControlFile.logSeg,
		   ControlFile.checkPointCopy.ThisStartUpID,
		   ControlFile.checkPointCopy.nextXid,
		   ControlFile.checkPointCopy.nextOid,
		   ControlFile.blcksz,
		   ControlFile.relseg_size,
		   ControlFile.lc_collate,
		   ControlFile.lc_ctype);
}


/*
 * Write out the new pg_control file.
 */
static void
RewriteControlFile(void)
{
	int			fd;
	char		buffer[BLCKSZ];	/* need not be aligned */

	/*
	 * Adjust fields as needed to force an empty XLOG starting at the
	 * next available segment.
	 */
	newXlogId = ControlFile.logId;
	newXlogSeg = ControlFile.logSeg;
	/* be sure we wrap around correctly at end of a logfile */
	NextLogSeg(newXlogId, newXlogSeg);

	ControlFile.checkPointCopy.redo.xlogid = newXlogId;
	ControlFile.checkPointCopy.redo.xrecoff =
		newXlogSeg * XLogSegSize + SizeOfXLogPHD;
	ControlFile.checkPointCopy.undo = ControlFile.checkPointCopy.redo;
	ControlFile.checkPointCopy.time = time(NULL);

	ControlFile.state = DB_SHUTDOWNED;
	ControlFile.time = time(NULL);
	ControlFile.logId = newXlogId;
	ControlFile.logSeg = newXlogSeg + 1;
	ControlFile.checkPoint = ControlFile.checkPointCopy.redo;
	ControlFile.prevCheckPoint.xlogid = 0;
	ControlFile.prevCheckPoint.xrecoff = 0;

	/* Contents are protected with a CRC */
	INIT_CRC64(ControlFile.crc);
	COMP_CRC64(ControlFile.crc, 
			   (char*) &ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(ControlFile.crc);

	/*
	 * We write out BLCKSZ bytes into pg_control, zero-padding the
	 * excess over sizeof(ControlFileData).  This reduces the odds
	 * of premature-EOF errors when reading pg_control.  We'll still
	 * fail when we check the contents of the file, but hopefully with
	 * a more specific error than "couldn't read pg_control".
	 */
	if (sizeof(ControlFileData) > BLCKSZ)
	{
		fprintf(stderr, "sizeof(ControlFileData) is too large ... fix xlog.c\n");
		exit(1);
	}

	memset(buffer, 0, BLCKSZ);
	memcpy(buffer, &ControlFile, sizeof(ControlFileData));

	unlink(ControlFilePath);

	fd = open(ControlFilePath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		perror("RewriteControlFile failed to create pg_control file");
		exit(1);
	}

	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
	{
		perror("RewriteControlFile failed to write pg_control file");
		exit(1);
	}

	if (fsync(fd) != 0)
	{
		perror("fsync");
		exit(1);
	}

	close(fd);
}


/*
 * Remove existing XLOG files
 */
static void
KillExistingXLOG(void)
{
	DIR			   *xldir;
	struct dirent  *xlde;
	char			path[MAXPGPATH];

	xldir = opendir(XLogDir);
	if (xldir == NULL)
	{
		perror("KillExistingXLOG: cannot open $PGDATA/pg_xlog directory");
		exit(1);
	}

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) == 16 &&
			strspn(xlde->d_name, "0123456789ABCDEF") == 16)
		{
			sprintf(path, "%s%c%s",	XLogDir, SEP_CHAR, xlde->d_name);
			if (unlink(path) < 0)
			{
				perror(path);
				exit(1);
			}
		}
		errno = 0;
	}
	if (errno)
	{
		perror("KillExistingXLOG: cannot read $PGDATA/pg_xlog directory");
		exit(1);
	}
	closedir(xldir);
}


/*
 * Write an empty XLOG file, containing only the checkpoint record
 * already set up in ControlFile.
 */
static void
WriteEmptyXLOG(void)
{
	char	   *buffer;
	XLogPageHeader page;
	XLogRecord *record;
	crc64		crc;
	char		path[MAXPGPATH];
	int			fd;
	int			nbytes;

	/* Use malloc() to ensure buffer is MAXALIGNED */
	buffer = (char *) malloc(BLCKSZ);
	page = (XLogPageHeader) buffer;

	/* Set up the first page with initial record */
	memset(buffer, 0, BLCKSZ);
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = 0;
	page->xlp_sui = ControlFile.checkPointCopy.ThisStartUpID;
	record = (XLogRecord *) ((char *) page + SizeOfXLogPHD);
	record->xl_prev.xlogid = 0;
	record->xl_prev.xrecoff = 0;
	record->xl_xact_prev = record->xl_prev;
	record->xl_xid = InvalidTransactionId;
	record->xl_len = sizeof(CheckPoint);
	record->xl_info = XLOG_CHECKPOINT_SHUTDOWN;
	record->xl_rmid = RM_XLOG_ID;
	memcpy(XLogRecGetData(record), &ControlFile.checkPointCopy,
		   sizeof(CheckPoint));

	INIT_CRC64(crc);
	COMP_CRC64(crc, &ControlFile.checkPointCopy, sizeof(CheckPoint));
	COMP_CRC64(crc, (char*) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(crc);
	record->xl_crc = crc;

	/* Write the first page */
	XLogFileName(path, newXlogId, newXlogSeg);

	unlink(path);

	fd = open(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
			  S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		perror(path);
		exit(1);
	}

	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
	{
		perror("WriteEmptyXLOG: failed to write xlog file");
		exit(1);
	}

	/* Fill the rest of the file with zeroes */
	memset(buffer, 0, BLCKSZ);
	for (nbytes = BLCKSZ; nbytes < XLogSegSize; nbytes += BLCKSZ)
	{
		if (write(fd, buffer, BLCKSZ) != BLCKSZ)
		{
			perror("WriteEmptyXLOG: failed to write xlog file");
			exit(1);
		}
	}

	if (fsync(fd) != 0)
	{
		perror("fsync");
		exit(1);
	}

	close(fd);
}


static void
usage(void)
{
	fprintf(stderr, "Usage: pg_resetxlog [-f] [-n] PGDataDirectory\n\n"
			"  -f\tforce update to be done\n"
			"  -n\tno update, just show extracted pg_control values (for testing)\n");
	exit(1);
}


int
main(int argc, char ** argv)
{
	int		argn;
	bool	force = false;
	bool	noupdate = false;
	int		fd;
	char	path[MAXPGPATH];

	for (argn = 1; argn < argc; argn++)
	{
		if (argv[argn][0] != '-')
			break;				/* end of switches */
		if (strcmp(argv[argn], "-f") == 0)
			force = true;
		else if (strcmp(argv[argn], "-n") == 0)
			noupdate = true;
		else
			usage();
	}

	if (argn != argc-1)			/* one required non-switch argument */
		usage();

	DataDir = argv[argn++];

	snprintf(XLogDir, MAXPGPATH, "%s%cpg_xlog", DataDir, SEP_CHAR);

	snprintf(ControlFilePath, MAXPGPATH, "%s%cglobal%cpg_control",
			 DataDir, SEP_CHAR, SEP_CHAR);

	/*
	 * Check for a postmaster lock file --- if there is one, refuse to
	 * proceed, on grounds we might be interfering with a live installation.
	 */
	snprintf(path, MAXPGPATH, "%s%cpostmaster.pid", DataDir, SEP_CHAR);

	if ((fd = open(path, O_RDONLY)) < 0)
	{
		if (errno != ENOENT)
		{
			perror("Failed to open $PGDATA/postmaster.pid for reading");
			exit(1);
		}
	}
	else
	{
		fprintf(stderr, "Lock file '%s' exists --- is a postmaster running?\n"
				"If not, delete the lock file and try again.\n",
				path);
		exit(1);
	}

	/*
	 * Attempt to read the existing pg_control file
	 */
	if (!ReadControlFile())
		GuessControlValues();

	/*
	 * If we had to guess anything, and -f was not given, just print
	 * the guessed values and exit.  Also print if -n is given.
	 */
	if ((guessed && !force) || noupdate)
	{
		PrintControlValues();
		if (!noupdate)
			printf("\nIf these values seem acceptable, use -f to force reset.\n");
		exit(1);
	}

	/*
	 * Don't reset from a dirty pg_control without -f, either.
	 */
	if (ControlFile.state != DB_SHUTDOWNED && !force)
	{
		printf("The database was not shut down cleanly.\n"
			   "Resetting the xlog may cause data to be lost!\n"
			   "If you want to proceed anyway, use -f to force reset.\n");
		exit(1);
	}

	/*
	 * Else, do the dirty deed.
	 */
	RewriteControlFile();
	KillExistingXLOG();
	WriteEmptyXLOG();

	printf("XLOG reset.\n");
	return 0;
}
