/* -------------------------------------------------------------------------
 * pg_dumplo.h
 *
 * $PostgreSQL: pgsql/contrib/pg_dumplo/pg_dumplo.h,v 1.11 2004/11/28 23:49:49 tgl Exp $
 *
 * Karel Zak 1999-2004
 * -------------------------------------------------------------------------
 */

#ifndef PG_DUMPLO_H
#define PG_DUMPLO_H

#include "postgres_ext.h"

/* ----------
 * Define
 * ----------
 */
#define QUERY_BUFSIZ	(8*1024)
#define DIR_UMASK	0755
#define FILE_UMASK	0644

#define TRUE		1
#define FALSE		0
#define RE_OK		0
#define RE_ERROR	1

#define MAX_SCHEMA_NAME	128
#define MAX_TABLE_NAME	128
#define MAX_ATTR_NAME	128

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

/* ----------
 * LO struct
 * ----------
 */
typedef struct
{
	char	*lo_schema,   
		*lo_table,
		*lo_attr;
	Oid	lo_oid;
} LOlist;

typedef struct
{
	int			action;
	LOlist	   *lolist;
	char	  **argv,
			   *user,
			   *db,
			   *host,
			   *port,
			   *space;
	FILE	   *index;
	int			counter,
				argc,
				lolist_start,
				remove,
				quiet;
	PGresult   *res;
	PGconn	   *conn;
}	LODumpMaster;

typedef enum
{
	ACTION_NONE,
	ACTION_SHOW,
	ACTION_EXPORT_ATTR,
	ACTION_EXPORT_ALL,
	ACTION_IMPORT
}	PGLODUMP_ACTIONS;

extern char *progname;

extern void notice(LODumpMaster * pgLO, int set);
extern void index_file(LODumpMaster * pgLO);
extern void load_lolist(LODumpMaster * pgLO);
extern void pglo_export(LODumpMaster * pgLO);
extern void pglo_import(LODumpMaster * pgLO);

#endif   /* PG_DUMPLO_H */
