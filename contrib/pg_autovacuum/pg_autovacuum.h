/* pg_autovacuum.h
 * Header file for pg_autovacuum.c
 * (c) 2003 Matthew T. O'Connor
 *
 * $PostgreSQL: pgsql/contrib/pg_autovacuum/pg_autovacuum.h,v 1.13 2004/11/17 16:54:15 tgl Exp $
 */

#ifndef _PG_AUTOVACUUM_H
#define _PG_AUTOVACUUM_H

#include "libpq-fe.h"
#include "lib/dllist.h"

#define AUTOVACUUM_DEBUG	0
#define VACBASETHRESHOLD	1000
#define VACSCALINGFACTOR	2
#define SLEEPBASEVALUE		300
#define SLEEPSCALINGFACTOR	2
#define UPDATE_INTERVAL		2

/* these two constants are used to tell update_table_stats what operation we just perfomred */
#define VACUUM_ANALYZE		0
#define ANALYZE_ONLY		1


#define TABLE_STATS_QUERY	"select a.oid,a.relname,a.relnamespace,a.relpages,a.relisshared,a.reltuples,b.schemaname,b.n_tup_ins,b.n_tup_upd,b.n_tup_del from pg_class a, pg_stat_all_tables b where a.oid=b.relid and a.relkind = 'r'"

#define PAGES_QUERY "select oid,reltuples,relpages from pg_class where oid=%u"
#define FROZENOID_QUERY "select oid,age(datfrozenxid) from pg_database where datname = 'template1'"
#define FROZENOID_QUERY2 "select oid,datname,age(datfrozenxid) from pg_database where datname!='template0'"

/* Log levels */
enum
{
	LVL_DEBUG = 1,
	LVL_INFO,
	LVL_WARNING,
	LVL_ERROR,
	LVL_EXTRA
};

/* define cmd_args stucture */
typedef struct cmdargs
{
	int			vacuum_base_threshold,
				analyze_base_threshold,
				sleep_base_value,
				debug,
				
				/*
				 * Cost-Based Vacuum Delay Settings for pg_autovacuum
				 */
				av_vacuum_cost_delay,
				av_vacuum_cost_page_hit,
				av_vacuum_cost_page_miss,
				av_vacuum_cost_page_dirty,
				av_vacuum_cost_limit,
				
#ifndef WIN32
				daemonize;
#else
				install_as_service,
				remove_as_service;
#endif
	float		vacuum_scaling_factor,
				analyze_scaling_factor,
				sleep_scaling_factor;
	char	   *user,
			   *password,
#ifdef WIN32
			   *service_user,
			   *service_password,
#endif
			   *host,
			   *logfile,
			   *port;
} cmd_args;

/*
 * Might need to add a time value for last time the whole database was
 * vacuumed.  We need to guarantee this happens approx every 1Billion TX's
 */
typedef struct dbinfo
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
} db_info;

typedef struct tableinfo
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
} tbl_info;

#endif /* _PG_AUTOVACUUM_H */
