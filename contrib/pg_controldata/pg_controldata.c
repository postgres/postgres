/* pg_controldata
 *
 * reads the data from $PGDATA/global/pg_control
 *
 * copyright (c) Oliver Elphick <olly@lfix.co.uk>, 2001;
 * licence: BSD
 *
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


typedef unsigned int uint32;

#include "config.h"
#include "access/xlogdefs.h"

/*
 * #include "access/xlog.h"
 * #include "c.h"
 */

/* The following definitions are extracted from access/xlog.h and its
 * recursive includes. There is too much initialisation needed if
 * they are included direct. Perhaps someone more knowledgeable can
 * fix that.
 */
typedef struct crc64
{
	uint32      crc1;
	uint32      crc2;
} crc64;

#define LOCALE_NAME_BUFLEN  128

typedef enum DBState
{
	DB_STARTUP = 0,
	DB_SHUTDOWNED,
	DB_SHUTDOWNING,
	DB_IN_RECOVERY,
	DB_IN_PRODUCTION
} DBState;


typedef struct ControlFileData
{
   crc64    crc;
   uint32      logId;         /* current log file id */
   uint32      logSeg;        /* current log file segment (1-based) */
   struct 
	XLogRecPtr	checkPoint;    /* last check point record ptr */
   time_t      time;       /* time stamp of last modification */
   DBState     state;         /* see enum above */

   /*
    * this data is used to make sure that configuration of this DB is
    * compatible with the backend executable
    */
   uint32      blcksz;        /* block size for this DB */
   uint32      relseg_size;   /* blocks per segment of large relation */
   uint32      catalog_version_no;     /* internal version number */
   /* active locales --- "C" if compiled without USE_LOCALE: */
   char     lc_collate[LOCALE_NAME_BUFLEN];
   char     lc_ctype[LOCALE_NAME_BUFLEN];

   /*
    * important directory locations
    */
   char     archdir[MAXPGPATH];     /* where to move offline log files */
} ControlFileData;

int main() {
	ControlFileData ControlFile;
	int fd;
	char ControlFilePath[MAXPGPATH];
	char *DataDir;
	char tmdt[32];

	DataDir = getenv("PGDATA");
	if ( DataDir == NULL ) {
		fprintf(stderr,"PGDATA is not defined\n");
		exit(1);
	}

	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);

	if ((fd = open(ControlFilePath, O_RDONLY)) == -1) {
		perror("Failed to open $PGDATA/global/pg_control for reading");
		exit(2);
	}

	read(fd, &ControlFile, sizeof(ControlFileData));
	strftime(tmdt, 32, "%c", localtime(&(ControlFile.time)));

	printf("Log file id:                          %u\n"
	       "Log file segment:                     %u\n"
			 "Last modified:                        %s\n"
			 "Database block size:                  %u\n"
			 "Blocks per segment of large relation: %u\n"
			 "Catalog version number:               %u\n"
			 "LC_COLLATE:                           %s\n"
			 "LC_CTYPE:                             %s\n"
			 "Log archive directory:                %s\n",
			 ControlFile.logId,
			 ControlFile.logSeg,
			 tmdt,
			 ControlFile.blcksz,
			 ControlFile.relseg_size,
			 ControlFile.catalog_version_no,
			 ControlFile.lc_collate,
			 ControlFile.lc_ctype,
			 ControlFile.archdir);
	
	return (0);
}

