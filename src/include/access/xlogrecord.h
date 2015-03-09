/*
 * xlogrecord.h
 *
 * Definitions for the WAL record format.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecord.h
 */
#ifndef XLOGRECORD_H
#define XLOGRECORD_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "common/pg_crc.h"
#include "storage/block.h"
#include "storage/relfilenode.h"

/*
 * The overall layout of an XLOG record is:
 *		Fixed-size header (XLogRecord struct)
 *		XLogRecordBlockHeader struct
 *		XLogRecordBlockHeader struct
 *		...
 *		XLogRecordDataHeader[Short|Long] struct
 *		block data
 *		block data
 *		...
 *		main data
 *
 * There can be zero or more XLogRecordBlockHeaders, and 0 or more bytes of
 * rmgr-specific data not associated with a block.  XLogRecord structs
 * always start on MAXALIGN boundaries in the WAL files, but the rest of
 * the fields are not aligned.
 *
 * The XLogRecordBlockHeader, XLogRecordDataHeaderShort and
 * XLogRecordDataHeaderLong structs all begin with a single 'id' byte. It's
 * used to distinguish between block references, and the main data structs.
 */
typedef struct XLogRecord
{
	uint32		xl_tot_len;		/* total len of entire record */
	TransactionId xl_xid;		/* xact id */
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */
	uint8		xl_info;		/* flag bits, see below */
	RmgrId		xl_rmid;		/* resource manager for this record */
	/* 2 bytes of padding here, initialize to zero */
	pg_crc32	xl_crc;			/* CRC for this record */

	/* XLogRecordBlockHeaders and XLogRecordDataHeader follow, no padding */

} XLogRecord;

#define SizeOfXLogRecord	(offsetof(XLogRecord, xl_crc) + sizeof(pg_crc32))

/*
 * The high 4 bits in xl_info may be used freely by rmgr. The
 * XLR_SPECIAL_REL_UPDATE bit can be passed by XLogInsert caller. The rest
 * are set internally by XLogInsert.
 */
#define XLR_INFO_MASK			0x0F
#define XLR_RMGR_INFO_MASK		0xF0

/*
 * If a WAL record modifies any relation files, in ways not covered by the
 * usual block references, this flag is set. This is not used for anything
 * by PostgreSQL itself, but it allows external tools that read WAL and keep
 * track of modified blocks to recognize such special record types.
 */
#define XLR_SPECIAL_REL_UPDATE	0x01

/*
 * Header info for block data appended to an XLOG record.
 *
 * 'data_length' is the length of the rmgr-specific payload data associated
 * with this block. It does not include the possible full page image, nor
 * XLogRecordBlockHeader struct itself.
 *
 * Note that we don't attempt to align the XLogRecordBlockHeader struct!
 * So, the struct must be copied to aligned local storage before use.
 */
typedef struct XLogRecordBlockHeader
{
	uint8		id;				/* block reference ID */
	uint8		fork_flags;		/* fork within the relation, and flags */
	uint16		data_length;	/* number of payload bytes (not including page
								 * image) */

	/* If BKPBLOCK_HAS_IMAGE, an XLogRecordBlockImageHeader struct follows */
	/* If BKPBLOCK_SAME_REL is not set, a RelFileNode follows */
	/* BlockNumber follows */
} XLogRecordBlockHeader;

#define SizeOfXLogRecordBlockHeader (offsetof(XLogRecordBlockHeader, data_length) + sizeof(uint16))

/*
 * Additional header information when a full-page image is included
 * (i.e. when BKPBLOCK_HAS_IMAGE is set).
 *
 * As a trivial form of data compression, the XLOG code is aware that
 * PG data pages usually contain an unused "hole" in the middle, which
 * contains only zero bytes.  If hole_length > 0 then we have removed
 * such a "hole" from the stored data (and it's not counted in the
 * XLOG record's CRC, either).  Hence, the amount of block data actually
 * present is BLCKSZ - hole_length bytes.
 */
typedef struct XLogRecordBlockImageHeader
{
	uint16		hole_offset;	/* number of bytes before "hole" */
	uint16		hole_length;	/* number of bytes in "hole" */
} XLogRecordBlockImageHeader;

#define SizeOfXLogRecordBlockImageHeader sizeof(XLogRecordBlockImageHeader)

/*
 * Maximum size of the header for a block reference. This is used to size a
 * temporary buffer for constructing the header.
 */
#define MaxSizeOfXLogRecordBlockHeader \
	(SizeOfXLogRecordBlockHeader + \
	 SizeOfXLogRecordBlockImageHeader + \
	 sizeof(RelFileNode) + \
	 sizeof(BlockNumber))

/*
 * The fork number fits in the lower 4 bits in the fork_flags field. The upper
 * bits are used for flags.
 */
#define BKPBLOCK_FORK_MASK	0x0F
#define BKPBLOCK_FLAG_MASK	0xF0
#define BKPBLOCK_HAS_IMAGE	0x10	/* block data is an XLogRecordBlockImage */
#define BKPBLOCK_HAS_DATA	0x20
#define BKPBLOCK_WILL_INIT	0x40	/* redo will re-init the page */
#define BKPBLOCK_SAME_REL	0x80	/* RelFileNode omitted, same as previous */

/*
 * XLogRecordDataHeaderShort/Long are used for the "main data" portion of
 * the record. If the length of the data is less than 256 bytes, the short
 * form is used, with a single byte to hold the length. Otherwise the long
 * form is used.
 *
 * (These structs are currently not used in the code, they are here just for
 * documentation purposes).
 */
typedef struct XLogRecordDataHeaderShort
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_SHORT */
	uint8		data_length;	/* number of payload bytes */
} XLogRecordDataHeaderShort;

#define SizeOfXLogRecordDataHeaderShort (sizeof(uint8) * 2)

typedef struct XLogRecordDataHeaderLong
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_LONG */
	/* followed by uint32 data_length, unaligned */
} XLogRecordDataHeaderLong;

#define SizeOfXLogRecordDataHeaderLong (sizeof(uint8) + sizeof(uint32))

/*
 * Block IDs used to distinguish different kinds of record fragments. Block
 * references are numbered from 0 to XLR_MAX_BLOCK_ID. A rmgr is free to use
 * any ID number in that range (although you should stick to small numbers,
 * because the WAL machinery is optimized for that case). A couple of ID
 * numbers are reserved to denote the "main" data portion of the record.
 *
 * The maximum is currently set at 32, quite arbitrarily. Most records only
 * need a handful of block references, but there are a few exceptions that
 * need more.
 */
#define XLR_MAX_BLOCK_ID			32

#define XLR_BLOCK_ID_DATA_SHORT		255
#define XLR_BLOCK_ID_DATA_LONG		254

#endif   /* XLOGRECORD_H */
