/* pg_autovacuum.h
 * Header file for pg_autovacuum.c
 * (c) 2003 Matthew T. O'Connor
 */

#include "postgres_fe.h"

#include <unistd.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <time.h>
#include <sys/time.h>

/* These next two lines are correct when pg_autovaccum is compiled
   from within the postgresql source tree  */
#include "libpq-fe.h"
#include "lib/dllist.h"
/* Had to change the last two lines to compile on
   Redhat outside of postgresql source tree */
/*
#include "/usr/include/libpq-fe.h"
#include "/usr/include/pgsql/server/lib/dllist.h"
*/

#define AUTOVACUUM_DEBUG	1
#define VACBASETHRESHOLD	1000
#define VACSCALINGFACTOR	2
#define SLEEPBASEVALUE		300
#define SLEEPSCALINGFACTOR	2
#define UPDATE_INTERVAL		2

/* these two constants are used to tell update_table_stats what operation we just perfomred */
#define VACUUM_ANALYZE		0
#define ANALYZE_ONLY		1

#define TABLE_STATS_QUERY	"select a.oid,a.relname,a.relnamespace,a.relpages,a.relisshared,a.reltuples,b.schemaname,b.n_tup_ins,b.n_tup_upd,b.n_tup_del from pg_class a, pg_stat_all_tables b where a.oid=b.relid and a.relkind = 'r' and schemaname not like 'pg_temp_%'"

#define FRONTEND
#define PAGES_QUERY "select oid,reltuples,relpages from pg_class where oid=%u"
#define FROZENOID_QUERY "select oid,age(datfrozenxid) from pg_database where datname = 'template1'"
#define FROZENOID_QUERY2 "select oid,datname,age(datfrozenxid) from pg_database where datname!='template0'"

/* define atooid */
#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

/* define cmd_args stucture */
struct cmdargs
{
	int			vacuum_base_threshold,
				analyze_base_threshold,
				sleep_base_value,
				debug,
				daemonize;
	float		vacuum_scaling_factor,
				analyze_scaling_factor,
				sleep_scaling_factor;
	char	   *user,
			   *password,
			   *host,
			   *logfile,
			   *port;
};
typedef struct cmdargs cmd_args;

/* define cmd_args as global so we can get to them everywhere */
cmd_args   *args;

/* Might need to add a time value for last time the whold database was vacuumed.
	I think we need to guarantee this happens approx every 1Million TX's  */
struct dbinfo
{
	Oid			oid;
	long		age;
	long		analyze_threshold,
				vacuum_threshold;		/* Use these as defaults for table
										 * thresholds */
	PGconn	   *conn;
	char	   *dbname,
			   *username,
			   *password;
	Dllist	   *table_list;
};
typedef struct dbinfo db_info;

struct tableinfo
{
	char	   *schema_name,
			   *table_name;
	float		reltuples;
	int			relisshared;
	Oid			relid,
				relpages;
	long		analyze_threshold,
				vacuum_threshold;
	long		CountAtLastAnalyze;		/* equal to: inserts + updates as
										 * of the last analyze or initial
										 * values at startup */
	long		CountAtLastVacuum;		/* equal to: deletes + updates as
										 * of the last vacuum or initial
										 * values at startup */
	long		curr_analyze_count,
				curr_vacuum_count;		/* Latest values from stats system */
	db_info    *dbi;			/* pointer to the database that this table
								 * belongs to */
};
typedef struct tableinfo tbl_info;

/* Functions for dealing with command line arguements */
static cmd_args *get_cmd_args(int argc, char *argv[]);
static void print_cmd_args(void);
static void free_cmd_args(void);
static void usage(void);

/* Functions for managing database lists */
static Dllist *init_db_list(void);
static db_info *init_dbinfo(char *dbname, Oid oid, long age);
static void update_db_list(Dllist *db_list);
static void remove_db_from_list(Dlelem *db_to_remove);
static void print_db_info(db_info * dbi, int print_table_list);
static void print_db_list(Dllist *db_list, int print_table_lists);
static int	xid_wraparound_check(db_info * dbi);
static void free_db_list(Dllist *db_list);

/* Functions for managing table lists */
static tbl_info *init_table_info(PGresult *conn, int row, db_info * dbi);
static void update_table_list(db_info * dbi);
static void remove_table_from_list(Dlelem *tbl_to_remove);
static void print_table_list(Dllist *tbl_node);
static void print_table_info(tbl_info * tbl);
static void update_table_thresholds(db_info * dbi, tbl_info * tbl, int vacuum_type);
static void free_tbl_list(Dllist *tbl_list);

/* A few database helper functions */
static int	check_stats_enabled(db_info * dbi);
static PGconn *db_connect(db_info * dbi);
static void db_disconnect(db_info * dbi);
static PGresult *send_query(const char *query, db_info * dbi);

/* Other Generally needed Functions */
static void daemonize(void);
static void log_entry(const char *logentry);
