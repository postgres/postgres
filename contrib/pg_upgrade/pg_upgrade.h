/*
 *	pg_upgrade.h
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/pg_upgrade.h,v 1.15.2.1 2010/07/25 03:47:33 momjian Exp $
 */

#include "postgres.h"

#include <unistd.h>
#include <assert.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "libpq-fe.h"

/* Allocate for null byte */
#define USER_NAME_SIZE		128

#define MAX_STRING			1024
#define LINE_ALLOC			4096
#define QUERY_ALLOC			8192

#define MIGRATOR_API_VERSION	1

#define MESSAGE_WIDTH		"60"

#define OVERWRITE_MESSAGE	"  %-" MESSAGE_WIDTH "." MESSAGE_WIDTH "s\r"
#define GET_MAJOR_VERSION(v)	((v) / 100)

#define ALL_DUMP_FILE		"pg_upgrade_dump_all.sql"
/* contains both global db information and CREATE DATABASE commands */
#define GLOBALS_DUMP_FILE	"pg_upgrade_dump_globals.sql"
#define DB_DUMP_FILE		"pg_upgrade_dump_db.sql"

#ifndef WIN32
#define pg_copy_file		copy_file
#define pg_mv_file			rename
#define pg_link_file		link
#define PATH_SEPARATOR      '/'
#define RM_CMD				"rm -f"
#define RMDIR_CMD			"rm -rf"
#define SHELL_EXT			"sh"
#else
#define pg_copy_file		CopyFile
#define pg_mv_file			pgrename
#define pg_link_file		win32_pghardlink
#define sleep(x)			Sleep(x * 1000)
#define PATH_SEPARATOR      '\\'
#define RM_CMD				"DEL /q"
#define RMDIR_CMD			"RMDIR /s/q"
#define SHELL_EXT			"bat"
#define EXE_EXT				".exe"
#endif

#if defined(WIN32) && !defined(__CYGWIN__)

		/*
		 * XXX This does not work for all terminal environments or for output
		 * containing non-ASCII characters; see comments in simple_prompt().
		 */
#define DEVTTY	"con"
#else
#define DEVTTY	"/dev/tty"
#endif

#define CLUSTERNAME(cluster)	((cluster) == CLUSTER_OLD ? "old" : "new")

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

/* OID system catalog preservation added during PG 9.0 development */
#define TABLE_SPACE_SUBDIRS 201001111

/*
 * Each relation is represented by a relinfo structure.
 */
typedef struct
{
	char		nspname[NAMEDATALEN];	/* namespace name */
	char		relname[NAMEDATALEN];	/* relation name */
	Oid			reloid;			/* relation oid				 */
	Oid			relfilenode;	/* relation relfile node	 */
	Oid			toastrelid;		/* oid of the toast relation */
	/* relation tablespace path, or "" for the cluster default */
	char		tablespace[MAXPGPATH];	
} RelInfo;

typedef struct
{
	RelInfo    *rels;
	int			nrels;
} RelInfoArr;

/*
 * The following structure represents a relation mapping.
 */
typedef struct
{
	Oid			old;			/* Relfilenode of the old relation */
	Oid			new;			/* Relfilenode of the new relation */
	char		old_file[MAXPGPATH];
	char		new_file[MAXPGPATH];
	char		old_nspname[NAMEDATALEN];		/* old name of the namespace */
	char		old_relname[NAMEDATALEN];		/* old name of the relation */
	char		new_nspname[NAMEDATALEN];		/* new name of the namespace */
	char		new_relname[NAMEDATALEN];		/* new name of the relation */
} FileNameMap;

/*
 * Structure to store database information
 */
typedef struct
{
	Oid			db_oid;			/* oid of the database */
	char		db_name[NAMEDATALEN];	/* database name */
	char		db_tblspace[MAXPGPATH]; /* database default tablespace path */
	RelInfoArr	rel_arr;		/* array of all user relinfos */
} DbInfo;

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
	uint32		logid;
	uint32		nxtlogseg;
	uint32		chkpnt_tli;
	uint32		chkpnt_nxtxid;
	uint32		chkpnt_nxtoid;
	uint32		align;
	uint32		blocksz;
	uint32		largesz;
	uint32		walsz;
	uint32		walseg;
	uint32		ident;
	uint32		index;
	uint32		toast;
	bool		date_is_int;
	bool		float8_pass_by_value;
	char	   *lc_collate;
	char	   *lc_ctype;
	char	   *encoding;
} ControlData;

/*
 * Enumeration to denote link modes
 */
typedef enum
{
	TRANSFER_MODE_COPY,
	TRANSFER_MODE_LINK
} transferMode;

/*
 * Enumeration to denote pg_log modes
 */
typedef enum
{
	PG_INFO,
	PG_REPORT,
	PG_WARNING,
	PG_FATAL,
	PG_DEBUG
} eLogType;

/*
 * Enumeration to distinguish between old cluster and new cluster
 */
typedef enum
{
	NONE = 0,					/* used for no running servers */
	CLUSTER_OLD,
	CLUSTER_NEW
} Cluster;

typedef long pgpid_t;


/*
 * cluster
 *
 *	information about each cluster
 */
typedef struct
{
	ControlData controldata;	/* pg_control information */
	DbInfoArr	dbarr;			/* dbinfos array */
	char	   *pgdata;			/* pathname for cluster's $PGDATA directory */
	char	   *bindir;			/* pathname for cluster's executable directory */
	unsigned short port;		/* port number where postmaster is waiting */
	uint32		major_version;	/* PG_VERSION of cluster */
	char	   *major_version_str;		/* string PG_VERSION of cluster */
	Oid			pg_database_oid;	/* OID of pg_database relation */
	char	   *libpath;		/* pathname for cluster's pkglibdir */
	char	   *tablespace_suffix;		/* directory specification */
} ClusterInfo;


/*
 * migratorContext
 *
 *	We create a migratorContext object to store all of the information
 *	that we need to migrate a single cluster.
 */
typedef struct
{
	ClusterInfo old,
				new;			/* old and new cluster information */
	const char *progname;		/* complete pathname for this program */
	char	   *exec_path;		/* full path to my executable */
	char	   *user;			/* username for clusters */
	char		cwd[MAXPGPATH]; /* current working directory, used for output */
	char	  **tablespaces;	/* tablespaces */
	int			num_tablespaces;
	char	  **libraries;		/* loadable libraries */
	int			num_libraries;
	pgpid_t		postmasterPID;	/* PID of currently running postmaster */
	Cluster		running_cluster;

	char	   *logfile;		/* name of log file (may be /dev/null) */
	FILE	   *log_fd;			/* log FILE */
	FILE	   *debug_fd;		/* debug-level log FILE */
	bool		check;			/* TRUE -> ask user for permission to make
								 * changes */
	bool		verbose;		/* TRUE -> be verbose in messages */
	bool		debug;			/* TRUE -> log more information */
	transferMode transfer_mode; /* copy files or link them? */
} migratorContext;


/*
 * Global variables
 */
extern char scandir_file_pattern[];


/* check.c */

void		output_check_banner(migratorContext *ctx, bool *live_check);
void check_old_cluster(migratorContext *ctx, bool live_check,
				  char **sequence_script_file_name);
void		check_new_cluster(migratorContext *ctx);
void		report_clusters_compatible(migratorContext *ctx);
void issue_warnings(migratorContext *ctx,
			   char *sequence_script_file_name);
void output_completion_banner(migratorContext *ctx,
						 char *deletion_script_file_name);
void		check_cluster_versions(migratorContext *ctx);
void		check_cluster_compatibility(migratorContext *ctx, bool live_check);
void create_script_for_old_cluster_deletion(migratorContext *ctx,
									   char **deletion_script_file_name);


/* controldata.c */

void		get_control_data(migratorContext *ctx, ClusterInfo *cluster, bool live_check);
void check_control_data(migratorContext *ctx, ControlData *oldctrl,
				   ControlData *newctrl);


/* dump.c */

void		generate_old_dump(migratorContext *ctx);
void		split_old_dump(migratorContext *ctx);


/* exec.c */

int exec_prog(migratorContext *ctx, bool throw_error,
		  const char *cmd,...);
void		verify_directories(migratorContext *ctx);
bool		is_server_running(migratorContext *ctx, const char *datadir);
void		rename_old_pg_control(migratorContext *ctx);


/* file.c */

#ifdef PAGE_CONVERSION
typedef const char *(*pluginStartup) (uint16 migratorVersion,
								uint16 *pluginVersion, uint16 newPageVersion,
								   uint16 oldPageVersion, void **pluginData);
typedef const char *(*pluginConvertFile) (void *pluginData,
								   const char *dstName, const char *srcName);
typedef const char *(*pluginConvertPage) (void *pluginData,
								   const char *dstPage, const char *srcPage);
typedef const char *(*pluginShutdown) (void *pluginData);

typedef struct
{
	uint16		oldPageVersion; /* Page layout version of the old cluster		*/
	uint16		newPageVersion; /* Page layout version of the new cluster		*/
	uint16		pluginVersion;	/* API version of converter plugin */
	void	   *pluginData;		/* Plugin data (set by plugin) */
	pluginStartup startup;		/* Pointer to plugin's startup function */
	pluginConvertFile convertFile;		/* Pointer to plugin's file converter
										 * function */
	pluginConvertPage convertPage;		/* Pointer to plugin's page converter
										 * function */
	pluginShutdown shutdown;	/* Pointer to plugin's shutdown function */
} pageCnvCtx;

const char *setupPageConverter(migratorContext *ctx, pageCnvCtx **result);
#else
/* dummy */
typedef void *pageCnvCtx;
#endif

int			dir_matching_filenames(const struct dirent * scan_ent);
int pg_scandir(migratorContext *ctx, const char *dirname,
		   struct dirent *** namelist,
		   int (*selector) (const struct dirent *));
const char *copyAndUpdateFile(migratorContext *ctx,
				  pageCnvCtx *pageConverter, const char *src,
				  const char *dst, bool force);
const char *linkAndUpdateFile(migratorContext *ctx,
				pageCnvCtx *pageConverter, const char *src, const char *dst);

void		check_hard_link(migratorContext *ctx);

/* function.c */

void		install_support_functions(migratorContext *ctx);
void		uninstall_support_functions(migratorContext *ctx);
void		get_loadable_libraries(migratorContext *ctx);
void		check_loadable_libraries(migratorContext *ctx);

/* info.c */

FileNameMap *gen_db_file_maps(migratorContext *ctx, DbInfo *old_db,
				 DbInfo *new_db, int *nmaps, const char *old_pgdata,
				 const char *new_pgdata);
void get_db_and_rel_infos(migratorContext *ctx, DbInfoArr *db_arr,
					 Cluster whichCluster);
DbInfo	   *dbarr_lookup_db(DbInfoArr *db_arr, const char *db_name);
void		dbarr_free(DbInfoArr *db_arr);
void print_maps(migratorContext *ctx, FileNameMap *maps, int n,
		   const char *dbName);

/* option.c */

void		parseCommandLine(migratorContext *ctx, int argc, char *argv[]);

/* relfilenode.c */

void		get_pg_database_relfilenode(migratorContext *ctx, Cluster whichCluster);
const char *transfer_all_new_dbs(migratorContext *ctx, DbInfoArr *olddb_arr,
				   DbInfoArr *newdb_arr, char *old_pgdata, char *new_pgdata);


/* tablespace.c */

void		init_tablespaces(migratorContext *ctx);


/* server.c */

PGconn *connectToServer(migratorContext *ctx, const char *db_name,
				Cluster whichCluster);
PGresult *executeQueryOrDie(migratorContext *ctx, PGconn *conn,
				  const char *fmt,...);

void		start_postmaster(migratorContext *ctx, Cluster whichCluster, bool quiet);
void		stop_postmaster(migratorContext *ctx, bool fast, bool quiet);
uint32 get_major_server_version(migratorContext *ctx, char **verstr,
						 Cluster whichCluster);
void		check_for_libpq_envvars(migratorContext *ctx);


/* util.c */

void		exit_nicely(migratorContext *ctx, bool need_cleanup);
void	   *pg_malloc(migratorContext *ctx, int n);
void		pg_free(void *p);
char	   *pg_strdup(migratorContext *ctx, const char *s);
char	   *quote_identifier(migratorContext *ctx, const char *s);
int			get_user_info(migratorContext *ctx, char **user_name);
void		check_ok(migratorContext *ctx);
void		report_status(migratorContext *ctx, eLogType type, const char *fmt,...);
void		pg_log(migratorContext *ctx, eLogType type, char *fmt,...);
void		prep_status(migratorContext *ctx, const char *fmt,...);
void		check_ok(migratorContext *ctx);
char	   *pg_strdup(migratorContext *ctx, const char *s);
void	   *pg_malloc(migratorContext *ctx, int size);
void		pg_free(void *ptr);
const char *getErrorText(int errNum);
unsigned int str2uint(const char *str);


/* version.c */

void new_9_0_populate_pg_largeobject_metadata(migratorContext *ctx,
									  bool check_mode, Cluster whichCluster);

/* version_old_8_3.c */

void old_8_3_check_for_name_data_type_usage(migratorContext *ctx,
									   Cluster whichCluster);
void old_8_3_check_for_tsquery_usage(migratorContext *ctx,
								Cluster whichCluster);
void old_8_3_check_ltree_usage(migratorContext *ctx,
								Cluster whichCluster);
void old_8_3_rebuild_tsvector_tables(migratorContext *ctx,
								bool check_mode, Cluster whichCluster);
void old_8_3_invalidate_hash_gin_indexes(migratorContext *ctx,
									bool check_mode, Cluster whichCluster);
void old_8_3_invalidate_bpchar_pattern_ops_indexes(migratorContext *ctx,
									  bool check_mode, Cluster whichCluster);
char *old_8_3_create_sequence_script(migratorContext *ctx,
							   Cluster whichCluster);
