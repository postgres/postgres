/*-------------------------------------------------------------------------
 *
 * miscadmin.h
 *	  this file contains general postgres administration and initialization
 *	  stuff that used to be spread out between the following files:
 *		globals.h						global variables
 *		pdir.h							directory path crud
 *		pinit.h							postgres initialization
 *		pmod.h							processing modes
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: miscadmin.h,v 1.123 2003/05/28 18:19:09 tgl Exp $
 *
 * NOTES
 *	  some of the information in this file should be moved to
 *	  other files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef MISCADMIN_H
#define MISCADMIN_H



/*****************************************************************************
 *	  System interrupt and critical section handling
 *
 * There are two types of interrupts that a running backend needs to accept
 * without messing up its state: QueryCancel (SIGINT) and ProcDie (SIGTERM).
 * In both cases, we need to be able to clean up the current transaction
 * gracefully, so we can't respond to the interrupt instantaneously ---
 * there's no guarantee that internal data structures would be self-consistent
 * if the code is interrupted at an arbitrary instant.	Instead, the signal
 * handlers set flags that are checked periodically during execution.
 *
 * The CHECK_FOR_INTERRUPTS() macro is called at strategically located spots
 * where it is normally safe to accept a cancel or die interrupt.  In some
 * cases, we invoke CHECK_FOR_INTERRUPTS() inside low-level subroutines that
 * might sometimes be called in contexts that do *not* want to allow a cancel
 * or die interrupt.  The HOLD_INTERRUPTS() and RESUME_INTERRUPTS() macros
 * allow code to ensure that no cancel or die interrupt will be accepted,
 * even if CHECK_FOR_INTERRUPTS() gets called in a subroutine.	The interrupt
 * will be held off until CHECK_FOR_INTERRUPTS() is done outside any
 * HOLD_INTERRUPTS() ... RESUME_INTERRUPTS() section.
 *
 * Special mechanisms are used to let an interrupt be accepted when we are
 * waiting for a lock or when we are waiting for command input (but, of
 * course, only if the interrupt holdoff counter is zero).	See the
 * related code for details.
 *
 * A related, but conceptually distinct, mechanism is the "critical section"
 * mechanism.  A critical section not only holds off cancel/die interrupts,
 * but causes any elog(ERROR) or elog(FATAL) to become elog(PANIC) --- that is,
 * a system-wide reset is forced.  Needless to say, only really *critical*
 * code should be marked as a critical section!  Currently, this mechanism
 * is only used for XLOG-related code.
 *
 *****************************************************************************/

/* in globals.c */
/* these are marked volatile because they are set by signal handlers: */
extern DLLIMPORT volatile bool InterruptPending;
extern volatile bool QueryCancelPending;
extern volatile bool ProcDiePending;

/* these are marked volatile because they are examined by signal handlers: */
extern volatile bool ImmediateInterruptOK;
extern volatile uint32 InterruptHoldoffCount;
extern volatile uint32 CritSectionCount;

/* in postgres.c */
extern void ProcessInterrupts(void);

#define CHECK_FOR_INTERRUPTS() \
	do { \
		if (InterruptPending) \
			ProcessInterrupts(); \
	} while(0)

#define HOLD_INTERRUPTS()  (InterruptHoldoffCount++)

#define RESUME_INTERRUPTS() \
	do { \
		Assert(InterruptHoldoffCount > 0); \
		InterruptHoldoffCount--; \
	} while(0)

#define START_CRIT_SECTION()  (CritSectionCount++)

#define END_CRIT_SECTION() \
	do { \
		Assert(CritSectionCount > 0); \
		CritSectionCount--; \
	} while(0)


/*****************************************************************************
 *	  globals.h --															 *
 *****************************************************************************/

/*
 * from postmaster/postmaster.c
 */
extern bool ClientAuthInProgress;
extern const bool ExecBackend;

extern int	PostmasterMain(int argc, char *argv[]);
extern void ClosePostmasterPorts(bool pgstat_too);

/*
 * from utils/init/globals.c
 */
extern bool IsPostmasterEnvironment;
extern bool IsUnderPostmaster;

extern bool ExitOnAnyError;

extern bool Noversion;
extern char *DataDir;

extern DLLIMPORT int MyProcPid;
extern struct Port *MyProcPort;
extern long MyCancelKey;

extern char OutputFileName[];
extern char pg_pathname[];

/*
 * done in storage/backendid.h for now.
 *
 * extern BackendId    MyBackendId;
 */
extern DLLIMPORT Oid MyDatabaseId;

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
 * HasCTZSet is true if user has set timezone as a numeric offset from UTC.
 * If so, CTimeZone is the timezone offset in seconds.
 */

#define MAXTZLEN		10		/* max TZ name len, not counting tr. null */

#define USE_POSTGRES_DATES		0
#define USE_ISO_DATES			1
#define USE_SQL_DATES			2
#define USE_GERMAN_DATES		3

extern int	DateStyle;
extern bool EuroDates;
extern bool HasCTZSet;
extern int	CTimeZone;

extern bool enableFsync;
extern bool allowSystemTableMods;
extern DLLIMPORT int SortMem;
extern int	VacuumMem;

/*
 *	A few postmaster startup options are exported here so the
 *	configuration file processor can access them.
 */

extern bool NetServer;
extern bool EnableSSL;
extern bool SilentMode;
extern int	MaxBackends;
#define DEF_MAXBACKENDS 32
extern int	ReservedBackends;
extern DLLIMPORT int	NBuffers;
#define DEF_NBUFFERS (DEF_MAXBACKENDS > 8 ? DEF_MAXBACKENDS * 2 : 16)
extern int	PostPortNumber;
extern int	Unix_socket_permissions;
extern char *Unix_socket_group;
extern char *UnixSocketDir;
extern char *VirtualHost;


/*****************************************************************************
 *	  pdir.h --																 *
 *			POSTGRES directory path definitions.							 *
 *****************************************************************************/

extern char *DatabaseName;
extern char *DatabasePath;

/* in utils/misc/database.c */
extern void GetRawDatabaseInfo(const char *name, Oid *db_id, char *path);
extern char *ExpandDatabasePath(const char *path);

/* now in utils/init/miscinit.c */
extern void SetDatabaseName(const char *name);
extern void SetDatabasePath(const char *path);

extern char *GetUserNameFromId(AclId userid);
extern AclId GetUserId(void);
extern void SetUserId(AclId userid);
extern AclId GetSessionUserId(void);
extern void SetSessionUserId(AclId userid);
extern void InitializeSessionUserId(const char *username);
extern void InitializeSessionUserIdStandalone(void);
extern void SetSessionAuthorization(AclId userid);

extern void SetDataDir(const char *dir);

extern int FindExec(char *full_path, const char *argv0,
		 const char *binary_name);
extern int	CheckPathAccess(char *path, char *name, int open_mode);

#ifdef CYR_RECODE
extern void SetCharSet(void);
extern char *convertstr(unsigned char *buff, int len, int dest);
#endif

/* in utils/misc/superuser.c */
extern bool superuser(void);	/* current user is superuser */
extern bool superuser_arg(AclId userid);	/* given user is superuser */
extern bool is_dbadmin(Oid dbid);		/* current user is owner of
										 * database */


/*****************************************************************************
 *	  pmod.h --																 *
 *			POSTGRES processing mode definitions.							 *
 *****************************************************************************/

/*
 * Description:
 *		There are three processing modes in POSTGRES.  They are
 * BootstrapProcessing or "bootstrap," InitProcessing or
 * "initialization," and NormalProcessing or "normal."
 *
 * The first two processing modes are used during special times. When the
 * system state indicates bootstrap processing, transactions are all given
 * transaction id "one" and are consequently guaranteed to commit. This mode
 * is used during the initial generation of template databases.
 *
 * Initialization mode: used while starting a backend, until all normal
 * initialization is complete.	Some code behaves differently when executed
 * in this mode to enable system bootstrapping.
 *
 * If a POSTGRES binary is in normal mode, then all code may be executed
 * normally.
 */

typedef enum ProcessingMode
{
	BootstrapProcessing,		/* bootstrap creation of template database */
	InitProcessing,				/* initializing system */
	NormalProcessing			/* normal processing */
} ProcessingMode;

extern ProcessingMode Mode;

#define IsBootstrapProcessingMode() ((bool)(Mode == BootstrapProcessing))
#define IsInitProcessingMode() ((bool)(Mode == InitProcessing))
#define IsNormalProcessingMode() ((bool)(Mode == NormalProcessing))

#define SetProcessingMode(mode) \
	do { \
		AssertArg((mode) == BootstrapProcessing || \
				  (mode) == InitProcessing || \
				  (mode) == NormalProcessing); \
		Mode = (mode); \
	} while(0)

#define GetProcessingMode() Mode


/*****************************************************************************
 *	  pinit.h --															 *
 *			POSTGRES initialization and cleanup definitions.				 *
 *****************************************************************************/

/* in utils/init/postinit.c */
extern void InitPostgres(const char *dbname, const char *username);
extern void BaseInit(void);

/* in utils/init/miscinit.c */
extern bool CreateDataDirLockFile(const char *datadir, bool amPostmaster);
extern bool CreateSocketLockFile(const char *socketfile, bool amPostmaster);
extern void TouchSocketLockFile(void);
extern void RecordSharedMemoryInLockFile(unsigned long id1,
							 unsigned long id2);

extern void ValidatePgVersion(const char *path);
extern void process_preload_libraries(char *preload_libraries_string);

/* these externs do not belong here... */
extern void IgnoreSystemIndexes(bool mode);
extern bool IsIgnoringSystemIndexes(void);
extern bool IsCacheInitialized(void);

#endif   /* MISCADMIN_H */
