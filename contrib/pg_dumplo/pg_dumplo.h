
#ifndef _PG_LODUMP_H_
#define _PG_LODUMP_H_

#define VERSION "0.0.5"

/* ----------
 * Define
 * ----------
 */        
#define QUERY_BUFSIZ	(8*1024)
#define DIR_UMASK	0755
#define FILE_UMASK	0666 

#define	TRUE		1
#define FALSE		0
#define RE_OK		0
#define RE_ERROR	1

#define MAX_TABLE_NAME	128
#define MAX_ATTR_NAME	128

extern char *progname;

/* ----------
 * LO struct
 * ----------
 */
typedef struct { 
	char		*lo_table,
			*lo_attr;
	long		lo_oid;
} LOlist;

typedef struct {
	int		action;
	LOlist		*lolist;
	char		**argv,
			*user,
			*db,
			*host,
			*space;
	FILE		*index;
	int		counter,
			argc,
			lolist_start,
			remove,
			quiet;
	PGresult	*res;
	PGconn		*conn;
} LODumpMaster;

typedef enum {	
	ACTION_NONE,
	ACTION_SHOW,
	ACTION_EXPORT_ATTR,	
	ACTION_EXPORT_ALL,	
	ACTION_IMPORT
} PGLODUMP_ACTIONS;

extern void	notice		(LODumpMaster *pgLO, int set);
extern int	check_res	(LODumpMaster *pgLO);
extern void	index_file	(LODumpMaster *pgLO);
extern void	load_lolist	(LODumpMaster *pgLO);
extern void	pglo_export	(LODumpMaster *pgLO);
extern void	pglo_import	(LODumpMaster *pgLO);

#endif /* _PG_LODUMP_H */
