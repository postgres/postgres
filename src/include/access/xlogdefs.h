/*
 *
 * xlogdefs.h
 *
 * Postgres transaction log manager record pointer and
 * system stratup number definitions
 *
 */
#ifndef XLOG_DEFS_H
#define XLOG_DEFS_H

typedef struct XLogRecPtr
{
	uint32		xlogid;			/* log file #, 0 based */
	uint32		xrecoff;		/* offset of record in log file */
} XLogRecPtr;

/*
 * StartUpID (SUI) - system startups counter. It's to allow removing
 * pg_log after shutdown, in future.
 */
typedef	uint32		StartUpID;

#endif	 /* XLOG_DEFS_H */
