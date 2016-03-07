/*-------------------------------------------------------------------------
 *
 * controldata_utils.c
 *		Common code for control data file output.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/controldata_utils.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "port/pg_crc32c.h"

/*
 * get_controlfile(char *DataDir, const char *progname)
 *
 * Get controlfile values. The caller is responsible
 * for pfreeing the result.
 */
ControlFileData *
get_controlfile(char *DataDir, const char *progname)
{
	ControlFileData	   *ControlFile;
	int					fd;
	char				ControlFilePath[MAXPGPATH];
	pg_crc32c			crc;

	ControlFile = palloc(sizeof(ControlFileData));
	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);

	if ((fd = open(ControlFilePath, O_RDONLY | PG_BINARY, 0)) == -1)
#ifndef FRONTEND
		ereport(ERROR,
				(errcode_for_file_access(),
				errmsg("could not open file \"%s\" for reading: %m",
					   ControlFilePath)));
#else
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, ControlFilePath, strerror(errno));
		exit(EXIT_FAILURE);
	}
#endif

	if (read(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
#ifndef FRONTEND
		ereport(ERROR,
				(errcode_for_file_access(),
				errmsg("could not read file \"%s\": %m", ControlFilePath)));
#else
	{
		fprintf(stderr, _("%s: could not read file \"%s\": %s\n"),
				progname, ControlFilePath, strerror(errno));
		exit(EXIT_FAILURE);
	}
#endif

	close(fd);

	/* Check the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
			   (char *) ControlFile,
			   offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, ControlFile->crc))
#ifndef FRONTEND
		elog(ERROR, _("calculated CRC checksum does not match value stored in file"));
#else
		printf(_("WARNING: Calculated CRC checksum does not match value stored in file.\n"
				 "Either the file is corrupt, or it has a different layout than this program\n"
				 "is expecting.  The results below are untrustworthy.\n\n"));
#endif

	/* Make sure the control file is valid byte order. */
	if (ControlFile->pg_control_version % 65536 == 0 &&
		ControlFile->pg_control_version / 65536 != 0)
#ifndef FRONTEND
		elog(ERROR, _("byte ordering mismatch"));
#else
		printf(_("WARNING: possible byte ordering mismatch\n"
				 "The byte ordering used to store the pg_control file might not match the one\n"
				 "used by this program.  In that case the results below would be incorrect, and\n"
				 "the PostgreSQL installation would be incompatible with this data directory.\n"));
#endif

	return ControlFile;
}
