/*-------------------------------------------------------------------------
 *
 * syslogger.h
 *	  Exports from postmaster/syslogger.c.
 *
 * Copyright (c) 2004-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/syslogger.h,v 1.7.2.1 2007/06/14 01:49:39 adunstan Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYSLOGGER_H
#define _SYSLOGGER_H

#include <limits.h>				/* for PIPE_BUF */


/* 
 * Primitive protocol structure for writing to syslogger pipe(s).  The idea
 * here is to divide long messages into chunks that are not more than
 * PIPE_BUF bytes long, which according to POSIX spec must be written into
 * the pipe atomically.  The pipe reader then uses the protocol headers to
 * reassemble the parts of a message into a single string.  The reader can
 * also cope with non-protocol data coming down the pipe, though we cannot
 * guarantee long strings won't get split apart.
 *
 * We use 't' or 'f' instead of a bool for is_last to make the protocol a tiny
 * bit more robust against finding a false double nul byte prologue.  But we
 * still might find it in the len and/or pid bytes unless we're careful.
 */

#ifdef PIPE_BUF
/* Are there any systems with PIPE_BUF > 64K?  Unlikely, but ... */
#if PIPE_BUF > 65536
#define PIPE_CHUNK_SIZE  65536
#else
#define PIPE_CHUNK_SIZE  ((int) PIPE_BUF)
#endif
#else  /* not defined */
/* POSIX says the value of PIPE_BUF must be at least 512, so use that */
#define PIPE_CHUNK_SIZE  512
#endif

typedef struct 
{
	char		nuls[2];		/* always \0\0 */
	uint16		len;			/* size of this chunk (counts data only) */
	int32		pid;			/* writer's pid */
	char		is_last;		/* last chunk of message? 't' or 'f' */
	char		data[1];		/* data payload starts here */
} PipeProtoHeader;

typedef union
{
	PipeProtoHeader proto;
	char		filler[PIPE_CHUNK_SIZE];
} PipeProtoChunk;

#define PIPE_HEADER_SIZE  offsetof(PipeProtoHeader, data)
#define PIPE_MAX_PAYLOAD  ((int) (PIPE_CHUNK_SIZE - PIPE_HEADER_SIZE))


/* GUC options */
extern bool Redirect_stderr;
extern int	Log_RotationAge;
extern int	Log_RotationSize;
extern DLLIMPORT char *Log_directory;
extern DLLIMPORT char *Log_filename;
extern bool Log_truncate_on_rotation;

extern bool am_syslogger;

#ifndef WIN32
extern int	syslogPipe[2];
#else
extern HANDLE syslogPipe[2];
#endif


extern int	SysLogger_Start(void);

extern void write_syslogger_file(const char *buffer, int count);

#ifdef EXEC_BACKEND
extern void SysLoggerMain(int argc, char *argv[]);
#endif

#endif   /* _SYSLOGGER_H */
