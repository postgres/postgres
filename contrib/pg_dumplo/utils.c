
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

static void Dummy_NoticeProcessor(void * arg, const char * message);
static void Default_NoticeProcessor(void * arg, const char * message);
    

void
index_file(LODumpMaster *pgLO)
{
	char	path[BUFSIZ];

	if (pgLO->action == ACTION_SHOW)
		return;
	
	sprintf(path, "%s/%s", pgLO->space, pgLO->db); 
	
	if (pgLO->action == ACTION_EXPORT_ATTR ||
	    pgLO->action == ACTION_EXPORT_ALL) {
	    
		if (mkdir(path, DIR_UMASK) == -1) {
			if (errno != EEXIST) {
				perror(path);
				exit(RE_ERROR);	
			}	
		}
		
		sprintf(path, "%s/lo_dump.index", path);          

		if ((pgLO->index = fopen(path, "w")) == NULL) {
			perror(path);
			exit(RE_ERROR);
		}
		
	} else if (pgLO->action != ACTION_NONE ) {
	
		sprintf(path, "%s/lo_dump.index", path);          

		if ((pgLO->index = fopen(path, "r")) == NULL) {
			perror(path);
			exit(RE_ERROR);
		}
	}
}

int 
check_res(LODumpMaster *pgLO) 
{
	if (!pgLO->res && PQresultStatus(pgLO->res) != PGRES_COMMAND_OK) {
        	fprintf(stderr, "%s: %s\n", progname, PQerrorMessage(pgLO->conn));
                PQclear(pgLO->res);
                return FALSE;
        }
        if (PQresultStatus(pgLO->res) != PGRES_TUPLES_OK) {
                fprintf(stderr, "%s: Tuples is not OK.\n", progname);
                PQclear(pgLO->res);
                return FALSE;
        }
        return TRUE;        
}

static 
void Dummy_NoticeProcessor(void * arg, const char * message)
{
    ;
}

static 
void Default_NoticeProcessor(void * arg, const char * message)
{
     fprintf(stderr, "%s", message);
}

void 
notice(LODumpMaster *pgLO, int set)
{         
	if (set)PQsetNoticeProcessor(pgLO->conn, Default_NoticeProcessor, NULL);
	else	PQsetNoticeProcessor(pgLO->conn, Dummy_NoticeProcessor, NULL);	
}
