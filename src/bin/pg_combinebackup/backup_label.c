/*-------------------------------------------------------------------------
 *
 * Read and manipulate backup label files
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/backup_label.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>

#include "access/xlogdefs.h"
#include "backup_label.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "write_manifest.h"

static int	get_eol_offset(StringInfo buf);
static bool line_starts_with(char *s, char *e, char *match, char **sout);
static bool parse_lsn(char *s, char *e, XLogRecPtr *lsn, char **c);
static bool parse_tli(char *s, char *e, TimeLineID *tli);

/*
 * Parse a backup label file, starting at buf->cursor.
 *
 * We expect to find a START WAL LOCATION line, followed by a LSN, followed
 * by a space; the resulting LSN is stored into *start_lsn.
 *
 * We expect to find a START TIMELINE line, followed by a TLI, followed by
 * a newline; the resulting TLI is stored into *start_tli.
 *
 * We expect to find either both INCREMENTAL FROM LSN and INCREMENTAL FROM TLI
 * or neither. If these are found, they should be followed by an LSN or TLI
 * respectively and then by a newline, and the values will be stored into
 * *previous_lsn and *previous_tli, respectively.
 *
 * Other lines in the provided backup_label data are ignored. filename is used
 * for error reporting; errors are fatal.
 */
void
parse_backup_label(char *filename, StringInfo buf,
				   TimeLineID *start_tli, XLogRecPtr *start_lsn,
				   TimeLineID *previous_tli, XLogRecPtr *previous_lsn)
{
	int			found = 0;

	*start_tli = 0;
	*start_lsn = InvalidXLogRecPtr;
	*previous_tli = 0;
	*previous_lsn = InvalidXLogRecPtr;

	while (buf->cursor < buf->len)
	{
		char	   *s = &buf->data[buf->cursor];
		int			eo = get_eol_offset(buf);
		char	   *e = &buf->data[eo];
		char	   *c;

		if (line_starts_with(s, e, "START WAL LOCATION: ", &s))
		{
			if (!parse_lsn(s, e, start_lsn, &c))
				pg_fatal("%s: could not parse %s",
						 filename, "START WAL LOCATION");
			if (c >= e || *c != ' ')
				pg_fatal("%s: improper terminator for %s",
						 filename, "START WAL LOCATION");
			found |= 1;
		}
		else if (line_starts_with(s, e, "START TIMELINE: ", &s))
		{
			if (!parse_tli(s, e, start_tli))
				pg_fatal("%s: could not parse TLI for %s",
						 filename, "START TIMELINE");
			if (*start_tli == 0)
				pg_fatal("%s: invalid TLI", filename);
			found |= 2;
		}
		else if (line_starts_with(s, e, "INCREMENTAL FROM LSN: ", &s))
		{
			if (!parse_lsn(s, e, previous_lsn, &c))
				pg_fatal("%s: could not parse %s",
						 filename, "INCREMENTAL FROM LSN");
			if (c >= e || *c != '\n')
				pg_fatal("%s: improper terminator for %s",
						 filename, "INCREMENTAL FROM LSN");
			found |= 4;
		}
		else if (line_starts_with(s, e, "INCREMENTAL FROM TLI: ", &s))
		{
			if (!parse_tli(s, e, previous_tli))
				pg_fatal("%s: could not parse %s",
						 filename, "INCREMENTAL FROM TLI");
			if (*previous_tli == 0)
				pg_fatal("%s: invalid TLI", filename);
			found |= 8;
		}

		buf->cursor = eo;
	}

	if ((found & 1) == 0)
		pg_fatal("%s: could not find %s", filename, "START WAL LOCATION");
	if ((found & 2) == 0)
		pg_fatal("%s: could not find %s", filename, "START TIMELINE");
	if ((found & 4) != 0 && (found & 8) == 0)
		pg_fatal("%s: %s requires %s", filename,
				 "INCREMENTAL FROM LSN", "INCREMENTAL FROM TLI");
	if ((found & 8) != 0 && (found & 4) == 0)
		pg_fatal("%s: %s requires %s", filename,
				 "INCREMENTAL FROM TLI", "INCREMENTAL FROM LSN");
}

/*
 * Write a backup label file to the output directory.
 *
 * This will be identical to the provided backup_label file, except that the
 * INCREMENTAL FROM LSN and INCREMENTAL FROM TLI lines will be omitted.
 *
 * The new file will be checksummed using the specified algorithm. If
 * mwriter != NULL, it will be added to the manifest.
 */
void
write_backup_label(char *output_directory, StringInfo buf,
				   pg_checksum_type checksum_type, manifest_writer *mwriter)
{
	char		output_filename[MAXPGPATH];
	int			output_fd;
	pg_checksum_context checksum_ctx;
	uint8		checksum_payload[PG_CHECKSUM_MAX_LENGTH];
	int			checksum_length;

	pg_checksum_init(&checksum_ctx, checksum_type);

	snprintf(output_filename, MAXPGPATH, "%s/backup_label", output_directory);

	if ((output_fd = open(output_filename,
						  O_WRONLY | O_CREAT | O_EXCL | PG_BINARY,
						  pg_file_create_mode)) < 0)
		pg_fatal("could not open file \"%s\": %m", output_filename);

	while (buf->cursor < buf->len)
	{
		char	   *s = &buf->data[buf->cursor];
		int			eo = get_eol_offset(buf);
		char	   *e = &buf->data[eo];

		if (!line_starts_with(s, e, "INCREMENTAL FROM LSN: ", NULL) &&
			!line_starts_with(s, e, "INCREMENTAL FROM TLI: ", NULL))
		{
			ssize_t		wb;

			wb = write(output_fd, s, e - s);
			if (wb != e - s)
			{
				if (wb < 0)
					pg_fatal("could not write file \"%s\": %m", output_filename);
				else
					pg_fatal("could not write file \"%s\": wrote %d of %d",
							 output_filename, (int) wb, (int) (e - s));
			}
			if (pg_checksum_update(&checksum_ctx, (uint8 *) s, e - s) < 0)
				pg_fatal("could not update checksum of file \"%s\"",
						 output_filename);
		}

		buf->cursor = eo;
	}

	if (close(output_fd) != 0)
		pg_fatal("could not close file \"%s\": %m", output_filename);

	checksum_length = pg_checksum_final(&checksum_ctx, checksum_payload);

	if (mwriter != NULL)
	{
		struct stat sb;

		/*
		 * We could track the length ourselves, but must stat() to get the
		 * mtime.
		 */
		if (stat(output_filename, &sb) < 0)
			pg_fatal("could not stat file \"%s\": %m", output_filename);
		add_file_to_manifest(mwriter, "backup_label", sb.st_size,
							 sb.st_mtime, checksum_type,
							 checksum_length, checksum_payload);
	}
}

/*
 * Return the offset at which the next line in the buffer starts, or there
 * is none, the offset at which the buffer ends.
 *
 * The search begins at buf->cursor.
 */
static int
get_eol_offset(StringInfo buf)
{
	int			eo = buf->cursor;

	while (eo < buf->len)
	{
		if (buf->data[eo] == '\n')
			return eo + 1;
		++eo;
	}

	return eo;
}

/*
 * Test whether the line that runs from s to e (inclusive of *s, but not
 * inclusive of *e) starts with the match string provided, and return true
 * or false according to whether or not this is the case.
 *
 * If the function returns true and if *sout != NULL, stores a pointer to the
 * byte following the match into *sout.
 */
static bool
line_starts_with(char *s, char *e, char *match, char **sout)
{
	while (s < e && *match != '\0' && *s == *match)
		++s, ++match;

	if (*match == '\0' && sout != NULL)
		*sout = s;

	return (*match == '\0');
}

/*
 * Parse an LSN starting at s and not stopping at or before e. The return value
 * is true on success and otherwise false. On success, stores the result into
 * *lsn and sets *c to the first character that is not part of the LSN.
 */
static bool
parse_lsn(char *s, char *e, XLogRecPtr *lsn, char **c)
{
	char		save = *e;
	int			nchars;
	bool		success;
	unsigned	hi;
	unsigned	lo;

	*e = '\0';
	success = (sscanf(s, "%X/%X%n", &hi, &lo, &nchars) == 2);
	*e = save;

	if (success)
	{
		*lsn = ((XLogRecPtr) hi) << 32 | (XLogRecPtr) lo;
		*c = s + nchars;
	}

	return success;
}

/*
 * Parse a TLI starting at s and stopping at or before e. The return value is
 * true on success and otherwise false. On success, stores the result into
 * *tli. If the first character that is not part of the TLI is anything other
 * than a newline, that is deemed a failure.
 */
static bool
parse_tli(char *s, char *e, TimeLineID *tli)
{
	char		save = *e;
	int			nchars;
	bool		success;

	*e = '\0';
	success = (sscanf(s, "%u%n", tli, &nchars) == 1);
	*e = save;

	if (success && s[nchars] != '\n')
		success = false;

	return success;
}
