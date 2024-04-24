#ifndef XLOG_SMGR_H
#define XLOG_SMGR_H

#include "postgres.h"

#include <unistd.h>

/* XLog storage manager interface */
typedef struct XLogSmgr {
	ssize_t (*seg_read) (int fd, void *buf, size_t count, off_t offset);

 	ssize_t (*seg_write) (int fd, const void *buf, size_t count, off_t offset);
} XLogSmgr;

/* Default (standard) XLog storage manager */
static const XLogSmgr xlog_smgr_standard = {
	.seg_read = pg_pread,
	.seg_write = pg_pwrite,
};

extern XLogSmgr *xlog_smgr;
extern void SetXLogSmgr(XLogSmgr *xlsmgr);

#endif							/* XLOG_SMGR_H */
