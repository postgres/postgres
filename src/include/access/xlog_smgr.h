#ifndef XLOG_SMGR_H
#define XLOG_SMGR_H

#include "postgres.h"

#include <unistd.h>

/* XLog storage manager interface */
typedef struct XLogSmgr
{
	ssize_t		(*seg_read) (int fd, void *buf, size_t count, off_t offset,
							 TimeLineID tli, XLogSegNo segno, int segSize);

	ssize_t		(*seg_write) (int fd, const void *buf, size_t count, off_t offset,
							  TimeLineID tli, XLogSegNo segno);
} XLogSmgr;

static inline ssize_t
default_seg_write(int fd, const void *buf, size_t count, off_t offset,
				  TimeLineID tli, XLogSegNo segno)
{
	return pg_pwrite(fd, buf, count, offset);
}

static inline ssize_t
default_seg_read(int fd, void *buf, size_t count, off_t offset,
				 TimeLineID tli, XLogSegNo segno, int segSize)
{
	return pg_pread(fd, buf, count, offset);
}

/* Default (standard) XLog storage manager */
static const XLogSmgr xlog_smgr_standard = {
	.seg_read = default_seg_read,
	.seg_write = default_seg_write,
};

extern const XLogSmgr *xlog_smgr;
extern void SetXLogSmgr(const XLogSmgr *xlsmgr);

#endif							/* XLOG_SMGR_H */
