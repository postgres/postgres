
#include <stdio.h>	
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>                            
#include <time.h>

#include <libpq-fe.h>
#include <libpq/libpq-fs.h>

#include "pg_dumplo.h"

extern int 	errno;        

#define LOAD_LOLIST_QUERY "\
	SELECT c.relname, a.attname \
	FROM pg_class c, pg_attribute a, pg_type t \
	WHERE a.attnum > 0 \
		AND a.attrelid = c.oid \
		AND a.atttypid = t.oid \
		AND t.typname = 'oid' \
		AND c.relname NOT LIKE 'pg_%'"


void 
load_lolist( LODumpMaster *pgLO ) 
{
	LOlist		*ll;
	int		i;
	int		n;

 	/* ----------
 	 * Now find any candidate tables who have columns of type oid (the
   	 * column oid is ignored, as it has attnum < 1)
 	 * ----------
 	 */	
	if (!(pgLO->res = PQexec(pgLO->conn, LOAD_LOLIST_QUERY))) {
		
		fprintf(stderr, "%s: Select from pg_class failed.\n", progname);
    		exit(RE_ERROR);
  	}
	
	if ((n = PQntuples(pgLO->res)) == 0) {
		
		fprintf(stderr, "%s: No large objects in the database.\n", progname);
		exit(RE_ERROR);
 	}
	
	pgLO->lolist = (LOlist *) malloc((n + 1) * sizeof(LOlist));
	
	if (!pgLO->lolist) {
		fprintf(stderr, "%s: can't allocate memory\n", progname);
		exit(RE_ERROR);
  	}
  	
	for (i = 0, ll = pgLO->lolist; i < n; i++, ll++) {
		ll->lo_table = strdup(PQgetvalue(pgLO->res, i, 0));
		ll->lo_attr  = strdup(PQgetvalue(pgLO->res, i, 1));
	}
	
	PQclear(pgLO->res);
 	ll++;
 	ll->lo_table = ll->lo_attr = (char *) NULL;
}

void 
pglo_export(LODumpMaster *pgLO)
{
	LOlist		*ll;
	int		tuples;
	char		path[BUFSIZ],
			Qbuff[QUERY_BUFSIZ];
	
	if (pgLO->action != ACTION_SHOW) {
		time_t	t;
		time(&t);
		fprintf(pgLO->index, "#\n# This is the PostgreSQL large object dump index\n#\n");
		fprintf(pgLO->index, "#\tDate:     %s", ctime(&t));
		fprintf(pgLO->index, "#\tHost:     %s\n", pgLO->host);
		fprintf(pgLO->index, "#\tDatabase: %s\n", pgLO->db);
		fprintf(pgLO->index, "#\tUser:     %s\n", pgLO->user);
		fprintf(pgLO->index, "#\n# oid\ttable\tattribut\tinfile\n#\n");
	}
	
	pgLO->counter = 0;

	for(ll=pgLO->lolist; ll->lo_table != NULL; ll++) {
		
		/* ----------
		 * Query
		 * ----------
		 */
		sprintf(Qbuff, "SELECT DISTINCT x.\"%s\" FROM \"%s\" x, pg_largeobject l WHERE x.\"%s\" = l.loid",
			ll->lo_attr, ll->lo_table, ll->lo_attr);
		
		/* puts(Qbuff); */
			
		pgLO->res = PQexec(pgLO->conn, Qbuff);
	
		if ((tuples = PQntuples(pgLO->res)) == 0) {
		
			if (!pgLO->quiet && pgLO->action == ACTION_EXPORT_ATTR)
				printf("%s: no large objects in '%s'\n",
					   progname, ll->lo_table);	
			continue;
		
		} else if (check_res(pgLO)) {
		
			int	t;
			char	*val;
	
			/* ----------
			 * Create DIR/FILE
			 * ----------
			 */
			if (tuples && pgLO->action != ACTION_SHOW) {
			
				sprintf(path, "%s/%s/%s", pgLO->space, pgLO->db, ll->lo_table);          

				if (mkdir(path, DIR_UMASK) == -1) {
					if (errno != EEXIST) {
						perror(path);
						exit(RE_ERROR);					
					}	
				}
				
				sprintf(path, "%s/%s", path, ll->lo_attr);          
				
				if (mkdir(path, DIR_UMASK) == -1) {
					if (errno != EEXIST) {
						perror(path);
						exit(RE_ERROR);					
					}	
				}
				
				if (!pgLO->quiet)
					printf("dump %s.%s (%d large obj)\n", 
						ll->lo_table, ll->lo_attr, tuples);
			}

			pgLO->counter += tuples;
			
			for(t=0; t<tuples; t++) {
				
				Oid lo = (Oid) 0;
				
				val = PQgetvalue(pgLO->res, t, 0);
				
				if (!val)
					continue;
				else
					lo = (Oid) atol(val);
				
				if (pgLO->action == ACTION_SHOW) {
					printf("%s.%s: %ld\n", ll->lo_table, 
						ll->lo_attr, (long) lo);
					continue;
				}
				
				sprintf(path, "%s/%s/%s/%s/%s", pgLO->space, 
					pgLO->db, ll->lo_table, ll->lo_attr, val);
				
				if (lo_export(pgLO->conn, lo, path) < 0) 
					fprintf(stderr, "%s: %s\n", PQerrorMessage(pgLO->conn), progname);
					
				else 
					fprintf(pgLO->index, "%s\t%s\t%s\t%s/%s/%s/%s\n", val, 
						ll->lo_table, ll->lo_attr, pgLO->db, ll->lo_table, ll->lo_attr, val);
			}
		}
	}	
 }

