/* -------------------------------------------------------------------------
 * pg_dumplo
 *
 * $Header: /cvsroot/pgsql/contrib/pg_dumplo/Attic/utils.c,v 1.3 2001/01/24 19:42:45 momjian Exp $
 *
 *					Karel Zak 1999-2000
 * -------------------------------------------------------------------------
 */

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
