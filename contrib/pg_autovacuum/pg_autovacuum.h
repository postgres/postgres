/* pg_autovacuum.h
 * Header file for pg_autovacuum.c
 * (c) 2003 Matthew T. O'Connor
 */
#include "postgres_fe.h"

#include <unistd.h>
#include <sys/time.h>

#include "libpq-fe.h"
#include "lib/dllist.h"

#define AUTOVACUUM_DEBUG    1
#define BASETHRESHOLD       100
#define SCALINGFACTOR       2
#define SLEEPVALUE          1
#define SLEEPSCALINGFACTOR	0
#define UPDATE_INTERVAL     2
#define TABLE_STATS_ALL     "select a.relfilenode,a.relname,a.relnamespace,a.relpages,a.reltuples,b.schemaname,b.n_tup_ins,b.n_tup_upd,b.n_tup_del from pg_class a, pg_stat_all_tables b where a.relfilenode=b.relid"
#define TABLE_STATS_USER    "select a.relfilenode,a.relname,a.relnamespace,a.relpages,a.reltuples,b.schemaname,b.n_tup_ins,b.n_tup_upd,b.n_tup_del from pg_class a, pg_stat_user_tables b where a.relfilenode=b.relid"
#define FRONTEND

struct cmdargs{
  int tuple_base_threshold,sleep_base_value,debug;
  float tuple_scaling_factor,sleep_scaling_factor;
  char *user, *password, *host, *port;
}; typedef struct cmdargs cmd_args;

/* define cmd_args as global so we can get to them everywhere */
cmd_args  *args;

struct tableinfo{
  char *schema_name,*table_name;
  int insertThreshold,deleteThreshold;
  int relfilenode,reltuples,relpages;
  long InsertsAtLastAnalyze; /* equal to: inserts + updates as of the last analyze or initial values at startup */
  long DeletesAtLastVacuum; /* equal to: deletes + updates as of the last vacuum or initial values at startup */
  }; typedef struct tableinfo tbl_info;

/* Might need to add a time value for last time the whold database was vacuumed.
    I think we need to guarantee this happens approx every 1Million TX's  */
struct dbinfo{
  int			oid,age;
  int			insertThreshold,deleteThreshold; /* Use these as defaults for table thresholds */
  PGconn  *conn;
  char    *dbname,*username,*password;
  Dllist  *table_list;
  }; typedef struct dbinfo db_info;

/* Functions for dealing with command line arguements */
static cmd_args *get_cmd_args(int argc,char *argv[]);
static void print_cmd_args(void);
static void free_cmd_args(void);

/* Functions for managing database lists */
static Dllist *init_db_list(void);
static db_info *init_dbinfo(char *dbname,int oid,int age);
static void update_db_list(Dllist *db_list);
static void remove_db_from_list(Dlelem *db_to_remove);
static void print_db_info(db_info *dbi,int print_table_list);
static void print_db_list(Dllist *db_list,int print_table_lists);
static int  xid_wraparound_check(db_info *dbi);
static void free_db_list(Dllist *db_list);

/* Functions for managing table lists */
static tbl_info *init_table_info(PGresult *conn, int row);
static void update_table_list(db_info *dbi);
static void remove_table_from_list(Dlelem *tbl_to_remove);
static void print_table_list(Dllist *tbl_node);
static void print_table_info(tbl_info *tbl);
static void update_table_thresholds(db_info *dbi,tbl_info *tbl);
static void free_tbl_list(Dllist *tbl_list);

/* A few database helper functions */
static int	check_stats_enabled(db_info *dbi);
static PGconn *db_connect(db_info *dbi);
static void db_disconnect(db_info *dbi);
static PGresult *send_query(const char *query,db_info *dbi);
static char *query_table_stats(db_info *dbi);


