/*-------------------------------------------------------------------------
 *
 * syslogger.h
 *	  Exports from postmaster/syslogger.c.
 *
 * Copyright (c) 2004, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/syslogger.h,v 1.2 2004/08/29 05:06:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYSLOGGER_H
#define _SYSLOGGER_H

/* GUC options */
extern bool Redirect_stderr;
extern int	Log_RotationAge;
extern int	Log_RotationSize;
extern char *Log_directory;
extern char *Log_filename_prefix;

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
