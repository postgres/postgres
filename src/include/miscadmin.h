/*-------------------------------------------------------------------------
 *
 * miscadmin.h--
 *	  this file contains general postgres administration and initialization
 *	  stuff that used to be spread out between the following files:
 *		globals.h						global variables
 *		pdir.h							directory path crud
 *		pinit.h							postgres initialization
 *		pmod.h							processing modes
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: miscadmin.h,v 1.23 1998/05/19 18:05:52 momjian Exp $
 *
 * NOTES
 *	  some of the information in this file will be moved to
 *	  other files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef MISCADMIN_H
#define MISCADMIN_H

/*****************************************************************************
 *	  globals.h --															 *
 *****************************************************************************/

/*
 * from postmaster/postmaster.c
 */
extern int	PostmasterMain(int argc, char *argv[]);

/*
 * from utils/init/globals.c
 */
extern int	Portfd;
extern bool	Noversion;
extern bool	Quiet;
extern bool	QueryCancel;
extern char *DataDir;

extern int	MyProcPid;

extern char OutputFileName[];

/*
 * done in storage/backendid.h for now.
 *
 * extern BackendId    MyBackendId;
 * extern BackendTag   MyBackendTag;
 */
extern bool MyDatabaseIdIsInitialized;
extern Oid	MyDatabaseId;
extern bool TransactionInitWasProcessed;

extern bool IsUnderPostmaster;
extern bool IsPostmaster;

extern short DebugLvl;

/* Date/Time Configuration
 *
 * Constants to pass info from runtime environment:
 *	USE_POSTGRES_DATES specifies traditional postgres format for output.
 *	USE_ISO_DATES specifies ISO-compliant format for output.
 *	USE_SQL_DATES specified Oracle/Ingres-compliant format for output.
 *	USE_GERMAN_DATES specifies German-style dd.mm/yyyy date format.
 *
 * DateStyle specifies preference for date formatting for output.
 * EuroDates if client prefers dates interpreted and written w/European conventions.
 *
 * HasCTZSet if client timezone is specified by client.
 * CDayLight is the apparent daylight savings time status.
 * CTimeZone is the timezone offset in seconds.
 * CTZName is the timezone label.
 */

#define MAXTZLEN		7

#define USE_POSTGRES_DATES		0
#define USE_ISO_DATES			1
#define USE_SQL_DATES			2
#define USE_GERMAN_DATES		3

extern int	DateStyle;
extern bool EuroDates;
extern bool HasCTZSet;
extern bool CDayLight;
extern int	CTimeZone;
extern char CTZName[];

extern char FloatFormat[];
extern char DateFormat[];

extern int	fsyncOff;
extern int	SortMem;

extern Oid	LastOidProcessed;	/* for query rewrite */

#define MAX_PARSE_BUFFER 8192

/*
 *		default number of buffers in buffer pool
 *
 */
#define NDBUFS 64

/*****************************************************************************
 *	  pdir.h --																 *
 *			POSTGRES directory path definitions.							 *
 *****************************************************************************/

extern char *DatabaseName;
extern char *DatabasePath;

/* in utils/misc/database.c */
extern void GetRawDatabaseInfo(char *name, Oid *owner, Oid *db_id, char *path);
extern int	GetDatabaseInfo(char *name, Oid *owner, char *path);
extern char *ExpandDatabasePath(char *path);

/* now in utils/init/miscinit.c */
extern void SetDatabaseName(char *name);
extern void SetDatabasePath(char *path);
extern char *getpgusername(void);
extern void SetPgUserName(void);
extern Oid	GetUserId(void);
extern void SetUserId(void);
extern int	ValidateBackend(char *path);
extern int	FindBackend(char *backend, char *argv0);
extern int	CheckPathAccess(char *path, char *name, int open_mode);

/* lower case version for case-insensitive SQL referenced in pg_proc.h */
#define GetPgUserName() getpgusername()

/*****************************************************************************
 *	  pmod.h --																 *
 *			POSTGRES processing mode definitions.							 *
 *****************************************************************************/
/*
 * Description:
 *		There are four processing modes in POSTGRES.  They are NoProcessing
 * or "none," BootstrapProcessing or "bootstrap," InitProcessing or
 * "initialization," and NormalProcessing or "normal."
 *
 *		If a POSTGRES binary is in normal mode, then all code may be executed
 * normally.  In the none mode, only bookkeeping code may be called.  In
 * particular, access method calls may not occur in this mode since the
 * execution state is outside a transaction.
 *
 *		The final two processing modes are used during special times.  When the
 * system state indicates bootstrap processing, transactions are all given
 * transaction id "one" and are consequently guarenteed to commit.	This mode
 * is used during the initial generation of template databases.
 *
 * Finally, the execution state is in initialization mode until all normal
 * initialization is complete.	Some code behaves differently when executed in
 * this mode to enable system bootstrapping.
 */

typedef enum ProcessingMode
{
	NoProcessing,				/* "nothing" can be done */
	BootstrapProcessing,		/* bootstrap creation of template database */
	InitProcessing,				/* initializing system */
	NormalProcessing			/* normal processing */
} ProcessingMode;


/*****************************************************************************
 *	  pinit.h --															 *
 *			POSTGRES initialization and cleanup definitions.				 *
 *****************************************************************************/
/*
 * Note:
 *		XXX AddExitHandler not defined yet.
 */

typedef int16 ExitStatus;

#define NormalExitStatus		(0)
#define FatalExitStatus			(127)
/* XXX are there any other meaningful exit codes? */

/* in utils/init/postinit.c */

extern bool PostgresIsInitialized;

extern void InitPostgres(char *name);

/* in miscinit.c */
extern void ExitPostgres(ExitStatus status);
extern void StatusBackendExit(int status);
extern void StatusPostmasterExit(int status);

extern bool IsNoProcessingMode(void);
extern bool IsBootstrapProcessingMode(void);
extern bool IsInitProcessingMode(void);
extern bool IsNormalProcessingMode(void);
extern void SetProcessingMode(ProcessingMode mode);
extern ProcessingMode GetProcessingMode(void);

#endif							/* MISCADMIN_H */
