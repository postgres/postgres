/*
 * pg_controldata
 *
 * reads the data from $PGDATA/global/pg_control
 *
 * copyright (c) Oliver Elphick <olly@lfix.co.uk>, 2001;
 * licence: BSD
 *
 * $Header: /cvsroot/pgsql/contrib/pg_controldata/Attic/pg_controldata.c,v 1.2 2001/03/13 01:17:40 tgl Exp $
 */
#include "postgres.h"

#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "catalog/pg_control.h"


static const char *
dbState(DBState state)
{
	switch (state)
	{
		case DB_STARTUP:
			return "STARTUP";
		case DB_SHUTDOWNED:
			return "SHUTDOWNED";
		case DB_SHUTDOWNING:
			return "SHUTDOWNING";
		case DB_IN_RECOVERY:
			return "IN_RECOVERY";
		case DB_IN_PRODUCTION:
			return "IN_PRODUCTION";
	}
	return "unrecognized status code";
}


int
main()
{
	ControlFileData ControlFile;
	int fd;
	char ControlFilePath[MAXPGPATH];
	char *DataDir;
	crc64 crc;
	char pgctime_str[32];
	char ckpttime_str[32];

	DataDir = getenv("PGDATA");
	if ( DataDir == NULL ) {
		fprintf(stderr,"PGDATA is not defined\n");
		exit(1);
	}

	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);

	if ((fd = open(ControlFilePath, O_RDONLY)) == -1)
	{
		perror("Failed to open $PGDATA/global/pg_control for reading");
		exit(2);
	}

	if (read(fd, &ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
	{
		perror("Failed to read $PGDATA/global/pg_control");
		exit(2);
	}
	close(fd);

	/* Check the CRC. */
	INIT_CRC64(crc);
	COMP_CRC64(crc, 
			   (char*) &ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(crc);

	if (!EQ_CRC64(crc, ControlFile.crc))
		printf("WARNING: Calculated CRC checksum does not match value stored in file.\n"
			   "Either the file is corrupt, or it has a different layout than this program\n"
			   "is expecting.  The results below are untrustworthy.\n\n");

	strftime(pgctime_str, 32, "%c",
			 localtime(&(ControlFile.time)));
	strftime(ckpttime_str, 32, "%c",
			 localtime(&(ControlFile.checkPointCopy.time)));

	printf("pg_control version number:            %u\n"
		   "Catalog version number:               %u\n"
		   "Database state:                       %s\n"
		   "pg_control last modified:             %s\n"
		   "Current log file id:                  %u\n"
	       "Next log file segment:                %u\n"
		   "Latest checkpoint location:           %X/%X\n"
		   "Prior checkpoint location:            %X/%X\n"
		   "Latest checkpoint's REDO location:    %X/%X\n"
		   "Latest checkpoint's UNDO location:    %X/%X\n"
		   "Latest checkpoint's StartUpID:        %u\n"
		   "Latest checkpoint's NextXID:          %u\n"
		   "Latest checkpoint's NextOID:          %u\n"
		   "Time of latest checkpoint:            %s\n"
		   "Database block size:                  %u\n"
		   "Blocks per segment of large relation: %u\n"
		   "LC_COLLATE:                           %s\n"
		   "LC_CTYPE:                             %s\n",

		   ControlFile.pg_control_version,
		   ControlFile.catalog_version_no,
		   dbState(ControlFile.state),
		   pgctime_str,
		   ControlFile.logId,
		   ControlFile.logSeg,
		   ControlFile.checkPoint.xlogid,
		   ControlFile.checkPoint.xrecoff,
		   ControlFile.prevCheckPoint.xlogid,
		   ControlFile.prevCheckPoint.xrecoff,
		   ControlFile.checkPointCopy.redo.xlogid,
		   ControlFile.checkPointCopy.redo.xrecoff,
		   ControlFile.checkPointCopy.undo.xlogid,
		   ControlFile.checkPointCopy.undo.xrecoff,
		   ControlFile.checkPointCopy.ThisStartUpID,
		   ControlFile.checkPointCopy.nextXid,
		   ControlFile.checkPointCopy.nextOid,
		   ckpttime_str,
		   ControlFile.blcksz,
		   ControlFile.relseg_size,
		   ControlFile.lc_collate,
		   ControlFile.lc_ctype);

	return (0);
}
