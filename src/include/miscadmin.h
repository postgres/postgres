/*-------------------------------------------------------------------------
 *
 * miscadmin.h--
 *    this file contains general postgres administration and initialization
 *    stuff that used to be spread out between the following files:
 *	globals.h			global variables
 *	magic.h				PG_RELEASE, PG_VERSION, etc defines
 *	pdir.h				directory path crud
 *	pinit.h				postgres initialization
 *	pmod.h				processing modes
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: miscadmin.h,v 1.3 1996/11/12 06:47:10 bryanh Exp $
 *
 * NOTES
 *    some of the information in this file will be moved to
 *    other files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef MISCADMIN_H
#define MISCADMIN_H

/*****************************************************************************
 *    globals.h --                                                           *
 *****************************************************************************/

/*
 * from postmaster/postmaster.c
 */
extern int PostmasterMain(int argc, char* argv[]);

/*
 * from utils/init/globals.c
 */
extern int Portfd;
extern int Noversion;    /* moved from magic.c	*/
extern int MasterPid;    /* declared and defined in utils/initglobals.c */
extern int Quiet;
extern char *DataDir;   

extern char	  OutputFileName[];
extern void 	  InitGlobals(void);

/*
 * done in storage/backendid.h for now.
 *
 * extern BackendId    MyBackendId;
 * extern BackendTag   MyBackendTag;
 */
extern bool	    MyDatabaseIdIsInitialized;
extern Oid	    MyDatabaseId;
extern bool	    TransactionInitWasProcessed;

extern bool	    IsUnderPostmaster;
extern bool	    IsPostmaster;

extern short	    DebugLvl;

extern Oid	    LastOidProcessed;	/* for query rewrite */

#define MAX_PARSE_BUFFER 8192

/* 
 *	default number of buffers in buffer pool
 * 
 */
#define NDBUFS 64

/*****************************************************************************
 *    pdir.h --                                                              *
 *	    POSTGRES directory path definitions.                             *
 *****************************************************************************/

/* now in utils/init/miscinit.c */
extern char *GetDatabasePath(void);
extern char *GetDatabaseName(void);
extern void SetDatabaseName(char *name);
extern void SetDatabasePath(char *path);
extern char *GetPgUserName(void);
extern void SetPgUserName(void);
extern Oid GetUserId(void);
extern void SetUserId(void);
extern char *GetPGHome(void);
extern char *GetPGData(void);
extern int ValidateBackend(char *path);
extern int FindBackend(char *backend, char *argv0);
extern int CheckPathAccess(char *path, char *name, int open_mode);


/*****************************************************************************
 *    pmod.h --                                                              *
 *	    POSTGRES processing mode definitions.                            *
 *****************************************************************************/
/*
 * Description:
 *	There are four processing modes in POSTGRES.  They are NoProcessing
 * or "none," BootstrapProcessing or "bootstrap," InitProcessing or
 * "initialization," and NormalProcessing or "normal."
 *
 *	If a POSTGRES binary is in normal mode, then all code may be executed
 * normally.  In the none mode, only bookkeeping code may be called.  In
 * particular, access method calls may not occur in this mode since the
 * execution state is outside a transaction.
 *
 *	The final two processing modes are used during special times.  When the
 * system state indicates bootstrap processing, transactions are all given
 * transaction id "one" and are consequently guarenteed to commit.  This mode
 * is used during the initial generation of template databases.
 *
 * Finally, the execution state is in initialization mode until all normal
 * initialization is complete.  Some code behaves differently when executed in
 * this mode to enable system bootstrapping.
 */

typedef enum ProcessingMode {
    NoProcessing,		/* "nothing" can be done */
    BootstrapProcessing,	/* bootstrap creation of template database */
    InitProcessing,		/* initializing system */
    NormalProcessing		/* normal processing */
} ProcessingMode;


/*****************************************************************************
 *    pinit.h --                                                             *
 *	    POSTGRES initialization and cleanup definitions.                 *
 *****************************************************************************/
/*
 * Note:
 *	XXX AddExitHandler not defined yet.
 */

typedef	int16	ExitStatus;

#define	NormalExitStatus	(0)
#define	FatalExitStatus		(127)
/* XXX are there any other meaningful exit codes? */

/* in utils/init/postinit.c */
extern void InitMyDatabaseId(void);
extern void InitUserid(void);
extern void InitCommunication(void);
extern void InitStdio(void);

extern bool PostgresIsInitialized;

extern void InitPostgres(char *name);

/* in miscinit.c */
extern void ExitPostgres(ExitStatus status);
extern void AbortPostgres(void);
extern void StatusBackendExit(int status);
extern void StatusPostmasterExit(int status);

extern bool IsNoProcessingMode(void);
extern bool IsBootstrapProcessingMode(void);
extern bool IsInitProcessingMode(void);
extern bool IsNormalProcessingMode(void);
extern void SetProcessingMode(ProcessingMode mode);
extern ProcessingMode GetProcessingMode(void);

#endif	/* MISCADMIN_H */
