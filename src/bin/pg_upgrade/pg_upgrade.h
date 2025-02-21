/*
 *	pg_upgrade.h
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/pg_upgrade.h
 */

#include <unistd.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "common/relpath.h"
#include "libpq-fe.h"

/* For now, pg_upgrade does not use common/logging.c; use our own pg_fatal */
#undef pg_fatal

/* Use port in the private/dynamic port number range */
#define DEF_PGUPORT			50432

#define MAX_STRING			1024
#define QUERY_ALLOC			8192

#define MESSAGE_WIDTH		62

#define GET_MAJOR_VERSION(v)	((v) / 100)

/* contains both global db information and CREATE DATABASE commands */
#define GLOBALS_DUMP_FILE	"pg_upgrade_dump_globals.sql"
#define DB_DUMP_FILE_MASK	"pg_upgrade_dump_%u.custom"

/*
 * Base directories that include all the files generated internally, from the
 * root path of the new cluster.  The paths are dynamically built as of
 * BASE_OUTPUTDIR/$timestamp/{LOG_OUTPUTDIR,DUMP_OUTPUTDIR} to ensure their
 * uniqueness in each run.
 */
#define BASE_OUTPUTDIR		"pg_upgrade_output.d"
#define LOG_OUTPUTDIR		 "log"
#define DUMP_OUTPUTDIR		 "dump"

#define DB_DUMP_LOG_FILE_MASK	"pg_upgrade_dump_%u.log"
#define SERVER_LOG_FILE		"pg_upgrade_server.log"
#define UTILITY_LOG_FILE	"pg_upgrade_utility.log"
#define INTERNAL_LOG_FILE	"pg_upgrade_internal.log"

extern char *output_files[];

/*
 * WIN32 files do not accept writes from multiple processes
 *
 * On Win32, we can't send both pg_upgrade output and command output to the
 * same file because we get the error: "The process cannot access the file
 * because it is being used by another process." so send the pg_ctl
 * command-line output to a new file, rather than into the server log file.
 * Ideally we could use UTILITY_LOG_FILE for this, but some Windows platforms
 * keep the pg_ctl output file open by the running postmaster, even after
 * pg_ctl exits.
 *
 * We could use the Windows pgwin32_open() flags to allow shared file
 * writes but is unclear how all other tools would use those flags, so
 * we just avoid it and log a little differently on Windows;  we adjust
 * the error message appropriately.
 */
#ifndef WIN32
#define SERVER_START_LOG_FILE	SERVER_LOG_FILE
#define SERVER_STOP_LOG_FILE	SERVER_LOG_FILE
#else
#define SERVER_START_LOG_FILE	"pg_upgrade_server_start.log"
/*
 *	"pg_ctl start" keeps SERVER_START_LOG_FILE and SERVER_LOG_FILE open
 *	while the server is running, so we use UTILITY_LOG_FILE for "pg_ctl
 *	stop".
 */
#define SERVER_STOP_LOG_FILE	UTILITY_LOG_FILE
#endif


#ifndef WIN32
#define pg_mv_file			rename
#define PATH_SEPARATOR		'/'
#define PATH_QUOTE	'\''
#define RM_CMD				"rm -f"
#define RMDIR_CMD			"rm -rf"
#define SCRIPT_PREFIX		"./"
#define SCRIPT_EXT			"sh"
#define ECHO_QUOTE	"'"
#define ECHO_BLANK	""
#else
#define pg_mv_file			pgrename
#define PATH_SEPARATOR		'\\'
#define PATH_QUOTE	'"'
/* @ prefix disables command echo in .bat files */
#define RM_CMD				"@DEL /q"
#define RMDIR_CMD			"@RMDIR /s/q"
#define SCRIPT_PREFIX		""
#define SCRIPT_EXT			"bat"
#define ECHO_QUOTE	""
#define ECHO_BLANK	"."
#endif


/*
 * The format of visibility map was changed with this 9.6 commit.
 */
#define VISIBILITY_MAP_FROZEN_BIT_CAT_VER 201603011

/*
 * pg_multixact format changed in 9.3 commit 0ac5ad5134f2769ccbaefec73844f85,
 * ("Improve concurrency of foreign key locking") which also updated catalog
 * version to this value.  pg_upgrade behavior depends on whether old and new
 * server versions are both newer than this, or only the new one is.
 */
#define MULTIXACT_FORMATCHANGE_CAT_VER 201301231

/*
 * large object chunk size added to pg_controldata,
 * commit 5f93c37805e7485488480916b4585e098d3cc883
 */
#define LARGE_OBJECT_SIZE_PG_CONTROL_VER 942

/*
 * change in JSONB format during 9.4 beta
 */
#define JSONB_FORMAT_CHANGE_CAT_VER 201409291

/*
 * The control file was changed to have the default char signedness,
 * commit 44fe30fdab6746a287163e7cc093fd36cda8eb92
 */
#define DEFAULT_CHAR_SIGNEDNESS_CAT_VER 202502212

/*
 * Each relation is represented by a relinfo structure.
 */
typedef struct
{
	/* Can't use NAMEDATALEN; not guaranteed to be same on client */
	char	   *nspname;		/* namespace name */
	char	   *relname;		/* relation name */
	Oid			reloid;			/* relation OID */
	RelFileNumber relfilenumber;	/* relation file number */
	Oid			indtable;		/* if index, OID of its table, else 0 */
	Oid			toastheap;		/* if toast table, OID of base table, else 0 */
	char	   *tablespace;		/* tablespace path; "" for cluster default */
	bool		nsp_alloc;		/* should nspname be freed? */
	bool		tblsp_alloc;	/* should tablespace be freed? */
} RelInfo;

typedef struct
{
	RelInfo    *rels;
	int			nrels;
} RelInfoArr;

/*
 * Structure to store logical replication slot information.
 */
typedef struct
{
	char	   *slotname;		/* slot name */
	char	   *plugin;			/* plugin */
	bool		two_phase;		/* can the slot decode 2PC? */
	bool		caught_up;		/* has the slot caught up to latest changes? */
	bool		invalid;		/* if true, the slot is unusable */
	bool		failover;		/* is the slot designated to be synced to the
								 * physical standby? */
} LogicalSlotInfo;

typedef struct
{
	int			nslots;			/* number of logical slot infos */
	LogicalSlotInfo *slots;		/* array of logical slot infos */
} LogicalSlotInfoArr;

/*
 * The following structure represents a relation mapping.
 */
typedef struct
{
	const char *old_tablespace;
	const char *new_tablespace;
	const char *old_tablespace_suffix;
	const char *new_tablespace_suffix;
	Oid			db_oid;
	RelFileNumber relfilenumber;
	/* the rest are used only for logging and error reporting */
	char	   *nspname;		/* namespaces */
	char	   *relname;
} FileNameMap;

/*
 * Structure to store database information
 */
typedef struct
{
	Oid			db_oid;			/* oid of the database */
	char	   *db_name;		/* database name */
	char		db_tablespace[MAXPGPATH];	/* database default tablespace
											 * path */
	RelInfoArr	rel_arr;		/* array of all user relinfos */
	LogicalSlotInfoArr slot_arr;	/* array of all LogicalSlotInfo */
} DbInfo;

/*
 * Locale information about a database.
 */
typedef struct
{
	char	   *db_collate;
	char	   *db_ctype;
	char		db_collprovider;
	char	   *db_locale;
	int			db_encoding;
} DbLocaleInfo;

typedef struct
{
	DbInfo	   *dbs;			/* array of db infos */
	int			ndbs;			/* number of db infos */
} DbInfoArr;

/*
 * The following structure is used to hold pg_control information.
 * Rather than using the backend's control structure we use our own
 * structure to avoid pg_control version issues between releases.
 */
typedef struct
{
	uint32		ctrl_ver;
	uint32		cat_ver;
	char		nextxlogfile[25];
	uint32		chkpnt_nxtxid;
	uint32		chkpnt_nxtepoch;
	uint32		chkpnt_nxtoid;
	uint32		chkpnt_nxtmulti;
	uint32		chkpnt_nxtmxoff;
	uint32		chkpnt_oldstMulti;
	uint32		chkpnt_oldstxid;
	uint32		align;
	uint32		blocksz;
	uint32		largesz;
	uint32		walsz;
	uint32		walseg;
	uint32		ident;
	uint32		index;
	uint32		toast;
	uint32		large_object;
	bool		date_is_int;
	bool		float8_pass_by_value;
	uint32		data_checksum_version;
	bool		default_char_signedness;
} ControlData;

/*
 * Enumeration to denote transfer modes
 */
typedef enum
{
	TRANSFER_MODE_CLONE,
	TRANSFER_MODE_COPY,
	TRANSFER_MODE_COPY_FILE_RANGE,
	TRANSFER_MODE_LINK,
} transferMode;

/*
 * Enumeration to denote pg_log modes
 */
typedef enum
{
	PG_VERBOSE,
	PG_STATUS,					/* these messages do not get a newline added */
	PG_REPORT_NONL,				/* these too */
	PG_REPORT,
	PG_WARNING,
	PG_FATAL,
} eLogType;


/*
 * cluster
 *
 *	information about each cluster
 */
typedef struct
{
	ControlData controldata;	/* pg_control information */
	DbLocaleInfo *template0;	/* template0 locale info */
	DbInfoArr	dbarr;			/* dbinfos array */
	char	   *pgdata;			/* pathname for cluster's $PGDATA directory */
	char	   *pgconfig;		/* pathname for cluster's config file
								 * directory */
	char	   *bindir;			/* pathname for cluster's executable directory */
	char	   *pgopts;			/* options to pass to the server, like pg_ctl
								 * -o */
	char	   *sockdir;		/* directory for Unix Domain socket, if any */
	unsigned short port;		/* port number where postmaster is waiting */
	uint32		major_version;	/* PG_VERSION of cluster */
	char		major_version_str[64];	/* string PG_VERSION of cluster */
	uint32		bin_version;	/* version returned from pg_ctl */
	const char *tablespace_suffix;	/* directory specification */
	int			nsubs;			/* number of subscriptions */
} ClusterInfo;


/*
 *	LogOpts
*/
typedef struct
{
	FILE	   *internal;		/* internal log FILE */
	bool		verbose;		/* true -> be verbose in messages */
	bool		retain;			/* retain log files on success */
	/* Set of internal directories for output files */
	char	   *rootdir;		/* Root directory, aka pg_upgrade_output.d */
	char	   *basedir;		/* Base output directory, with timestamp */
	char	   *dumpdir;		/* Dumps */
	char	   *logdir;			/* Log files */
	bool		isatty;			/* is stdout a tty */
} LogOpts;


/*
 *	UserOpts
*/
typedef struct
{
	bool		check;			/* check clusters only, don't change any data */
	bool		live_check;		/* check clusters only, old server is running */
	bool		do_sync;		/* flush changes to disk */
	transferMode transfer_mode; /* copy files or link them? */
	int			jobs;			/* number of processes/threads to use */
	char	   *socketdir;		/* directory to use for Unix sockets */
	char	   *sync_method;
	bool		do_statistics;	/* carry over statistics from old cluster */
	int			char_signedness;	/* default char signedness: -1 for initial
									 * value, 1 for "signed" and 0 for
									 * "unsigned" */
} UserOpts;

typedef struct
{
	char	   *name;
	int			dbnum;
} LibraryInfo;

/*
 * OSInfo
 */
typedef struct
{
	const char *progname;		/* complete pathname for this program */
	char	   *user;			/* username for clusters */
	bool		user_specified; /* user specified on command-line */
	char	  **old_tablespaces;	/* tablespaces */
	int			num_old_tablespaces;
	LibraryInfo *libraries;		/* loadable libraries */
	int			num_libraries;
	ClusterInfo *running_cluster;
} OSInfo;


/* Function signature for data type check version hook */
typedef bool (*DataTypesUsageVersionCheck) (ClusterInfo *cluster);

/*
 * Global variables
 */
extern LogOpts log_opts;
extern UserOpts user_opts;
extern ClusterInfo old_cluster,
			new_cluster;
extern OSInfo os_info;


/* check.c */

void		output_check_banner(void);
void		check_and_dump_old_cluster(void);
void		check_new_cluster(void);
void		report_clusters_compatible(void);
void		issue_warnings_and_set_wal_level(void);
void		output_completion_banner(char *deletion_script_file_name);
void		check_cluster_versions(void);
void		check_cluster_compatibility(void);
void		create_script_for_old_cluster_deletion(char **deletion_script_file_name);


/* controldata.c */

void		get_control_data(ClusterInfo *cluster);
void		check_control_data(ControlData *oldctrl, ControlData *newctrl);
void		disable_old_cluster(void);


/* dump.c */

void		generate_old_dump(void);


/* exec.c */

#define EXEC_PSQL_ARGS "--echo-queries --set ON_ERROR_STOP=on --no-psqlrc --dbname=template1"

bool		exec_prog(const char *log_filename, const char *opt_log_file,
					  bool report_error, bool exit_on_error, const char *fmt,...) pg_attribute_printf(5, 6);
void		verify_directories(void);
bool		pid_lock_file_exists(const char *datadir);


/* file.c */

void		cloneFile(const char *src, const char *dst,
					  const char *schemaName, const char *relName);
void		copyFile(const char *src, const char *dst,
					 const char *schemaName, const char *relName);
void		copyFileByRange(const char *src, const char *dst,
							const char *schemaName, const char *relName);
void		linkFile(const char *src, const char *dst,
					 const char *schemaName, const char *relName);
void		rewriteVisibilityMap(const char *fromfile, const char *tofile,
								 const char *schemaName, const char *relName);
void		check_file_clone(void);
void		check_copy_file_range(void);
void		check_hard_link(void);

/* fopen_priv() is no longer different from fopen() */
#define fopen_priv(path, mode)	fopen(path, mode)

/* function.c */

void		get_loadable_libraries(void);
void		check_loadable_libraries(void);

/* info.c */

FileNameMap *gen_db_file_maps(DbInfo *old_db,
							  DbInfo *new_db, int *nmaps, const char *old_pgdata,
							  const char *new_pgdata);
void		get_db_rel_and_slot_infos(ClusterInfo *cluster);
int			count_old_cluster_logical_slots(void);
void		get_subscription_count(ClusterInfo *cluster);

/* option.c */

void		parseCommandLine(int argc, char *argv[]);
void		adjust_data_dir(ClusterInfo *cluster);
void		get_sock_dir(ClusterInfo *cluster);

/* relfilenumber.c */

void		transfer_all_new_tablespaces(DbInfoArr *old_db_arr,
										 DbInfoArr *new_db_arr, char *old_pgdata, char *new_pgdata);
void		transfer_all_new_dbs(DbInfoArr *old_db_arr,
								 DbInfoArr *new_db_arr, char *old_pgdata, char *new_pgdata,
								 char *old_tablespace);

/* tablespace.c */

void		init_tablespaces(void);


/* server.c */

PGconn	   *connectToServer(ClusterInfo *cluster, const char *db_name);
PGresult   *executeQueryOrDie(PGconn *conn, const char *fmt,...) pg_attribute_printf(2, 3);

char	   *cluster_conn_opts(ClusterInfo *cluster);

bool		start_postmaster(ClusterInfo *cluster, bool report_and_exit_on_error);
void		stop_postmaster(bool in_atexit);
uint32		get_major_server_version(ClusterInfo *cluster);
void		check_pghost_envvar(void);


/* util.c */

char	   *quote_identifier(const char *s);
int			get_user_info(char **user_name_p);
void		check_ok(void);
void		report_status(eLogType type, const char *fmt,...) pg_attribute_printf(2, 3);
void		pg_log(eLogType type, const char *fmt,...) pg_attribute_printf(2, 3);
void		pg_fatal(const char *fmt,...) pg_attribute_printf(1, 2) pg_attribute_noreturn();
void		end_progress_output(void);
void		cleanup_output_dirs(void);
void		prep_status(const char *fmt,...) pg_attribute_printf(1, 2);
void		prep_status_progress(const char *fmt,...) pg_attribute_printf(1, 2);
unsigned int str2uint(const char *str);


/* version.c */

bool		jsonb_9_4_check_applicable(ClusterInfo *cluster);
void		old_9_6_invalidate_hash_indexes(ClusterInfo *cluster,
											bool check_mode);

void		report_extension_updates(ClusterInfo *cluster);

/* parallel.c */
void		parallel_exec_prog(const char *log_file, const char *opt_log_file,
							   const char *fmt,...) pg_attribute_printf(3, 4);
void		parallel_transfer_all_new_dbs(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
										  char *old_pgdata, char *new_pgdata,
										  char *old_tablespace);
bool		reap_child(bool wait_for_child);

/* task.c */

typedef void (*UpgradeTaskProcessCB) (DbInfo *dbinfo, PGresult *res, void *arg);

/* struct definition is private to task.c */
typedef struct UpgradeTask UpgradeTask;

UpgradeTask *upgrade_task_create(void);
void		upgrade_task_add_step(UpgradeTask *task, const char *query,
								  UpgradeTaskProcessCB process_cb, bool free_result,
								  void *arg);
void		upgrade_task_run(const UpgradeTask *task, const ClusterInfo *cluster);
void		upgrade_task_free(UpgradeTask *task);

/* convenient type for common private data needed by several tasks */
typedef struct
{
	FILE	   *file;
	char		path[MAXPGPATH];
} UpgradeTaskReport;
