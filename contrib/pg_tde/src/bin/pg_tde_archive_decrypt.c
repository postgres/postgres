#include "postgres_fe.h"

#include "access/xlog_internal.h"
#include "access/xlog_smgr.h"
#include "common/logging.h"
#include "common/percentrepl.h"

#include "access/pg_tde_fe_init.h"
#include "access/pg_tde_xlog_smgr.h"

#define TMPFS_DIRECTORY "/dev/shm"

static bool
is_segment(const char *filename)
{
	return strspn(filename, "0123456789ABCDEF") == XLOG_FNAME_LEN &&
		(filename[XLOG_FNAME_LEN] == '\0' || strcmp(filename + XLOG_FNAME_LEN, ".partial") == 0);
}

static void
write_decrypted_segment(const char *segpath, const char *segname, const char *tmppath)
{
	int			segfd;
	int			tmpfd;
	off_t		fsize;
	int			r;
	int			w;
	TimeLineID	tli;
	XLogSegNo	segno;
	PGAlignedXLogBlock buf;
	off_t		pos = 0;

	segfd = open(segpath, O_RDONLY | PG_BINARY, 0);
	if (segfd < 0)
		pg_fatal("could not open file \"%s\": %m", segpath);

	tmpfd = open(tmppath, O_CREAT | O_WRONLY | PG_BINARY, 0666);
	if (tmpfd < 0)
		pg_fatal("could not open file \"%s\": %m", tmppath);

	/*
	 * WalSegSz extracted from the first page header but it might be
	 * encrypted. But we need to know the segment seize to decrypt it (it's
	 * required for encryption offset calculations). So we get the segment
	 * size from the file's actual size. XLogLongPageHeaderData->xlp_seg_size
	 * there is "just as a cross-check" anyway.
	 */
	fsize = lseek(segfd, 0, SEEK_END);
	XLogFromFileName(segname, &tli, &segno, fsize);

	r = xlog_smgr->seg_read(segfd, buf.data, XLOG_BLCKSZ, pos, tli, segno, fsize);

	if (r == XLOG_BLCKSZ)
	{
		XLogLongPageHeader longhdr = (XLogLongPageHeader) buf.data;
		int			walsegsz = longhdr->xlp_seg_size;

		if (walsegsz != fsize)
			pg_fatal("mismatch of segment size in WAL file \"%s\" (header: %d bytes, file size: %ld bytes)",
					 segname, walsegsz, fsize);

		if (!IsValidWalSegSize(walsegsz))
		{
			pg_log_error(ngettext("invalid WAL segment size in WAL file \"%s\" (%d byte)",
								  "invalid WAL segment size in WAL file \"%s\" (%d bytes)",
								  walsegsz),
						 segname, walsegsz);
			pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
			exit(1);
		}
	}
	else if (r < 0)
		pg_fatal("could not read file \"%s\": %m",
				 segpath);
	else
		pg_fatal("could not read file \"%s\": read %d of %d",
				 segpath, r, XLOG_BLCKSZ);

	pos += r;

	w = write(tmpfd, buf.data, XLOG_BLCKSZ);

	if (w < 0)
		pg_fatal("could not write file \"%s\": %m", tmppath);
	else if (w != r)
		pg_fatal("could not write file \"%s\": wrote %d of %d",
				 tmppath, w, r);

	while (1)
	{
		r = xlog_smgr->seg_read(segfd, buf.data, XLOG_BLCKSZ, pos, tli, segno, fsize);

		if (r == 0)
			break;
		else if (r < 0)
			pg_fatal("could not read file \"%s\": %m", segpath);

		pos += r;

		w = write(tmpfd, buf.data, r);

		if (w < 0)
			pg_fatal("could not write file \"%s\": %m", tmppath);
		else if (w != r)
			pg_fatal("could not write file \"%s\": wrote %d of %d",
					 tmppath, w, r);
	}

	close(tmpfd);
	close(segfd);
}

static void
usage(const char *progname)
{
	printf(_("%s wraps an archive command to give the command unencrypted WAL.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]\n"), progname);
	printf(_("  %s DEST-NAME SOURCE-PATH ARCHIVE-COMMAND\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -V, --version   output version information, then exit\n"));
	printf(_("  -?, --help      show this help, then exit\n"));
	printf(_("  DEST-NAME       name of the WAL file to send to archive\n"));
	printf(_("  SOURCE-PATH     path of the source WAL segment to decrypt\n"));
	printf(_("  ARCHIVE-COMMAND archive command to wrap, %%p will be replaced with the\n"
			 "                  absolute path of the decrypted WAL segment, %%f with the name\n"));
	printf(_("\n"));
	printf(_("Note that any %%f or %%p parameter in ARCHIVE-COMMAND will have to be escaped\n"
			 "as %%%%f or %%%%p respectively if used as archive_command in postgresql.conf.\n"
			 "e.g.\n"
			 "  archive_command='%s %%f %%p \"cp %%%%p /mnt/server/archivedir/%%%%f\"'\n"
			 "or\n"
			 "  archive_command='%s %%f %%p \"pgbackrest --stanza=your_stanza archive-push %%%%p\"'\n"
			 "\n"), progname, progname);
}

int
main(int argc, char *argv[])
{
	const char *progname;
	char	   *targetname;
	char	   *sourcepath;
	char	   *command;
	char	   *sep;
	char	   *sourcename;
	char		tmpdir[MAXPGPATH] = TMPFS_DIRECTORY "/pg_tde_archiveXXXXXX";
	char		tmppath[MAXPGPATH];
	bool		issegment;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_tde_archive_decrypt (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	if (argc != 4)
	{
		pg_log_error("wrong number of arguments, 3 expected");
		pg_log_error_detail("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	targetname = argv[1];
	sourcepath = argv[2];
	command = argv[3];

	pg_tde_fe_init("pg_tde");
	TDEXLogSmgrInit();

	sep = strrchr(sourcepath, '/');

	if (sep != NULL)
		sourcename = sep + 1;
	else
		sourcename = sourcepath;

	issegment = is_segment(targetname);

	if (issegment)
	{
		char	   *s;

		if (mkdtemp(tmpdir) == NULL)
			pg_fatal("could not create temporary directory \"%s\": %m", tmpdir);

		s = stpcpy(tmppath, tmpdir);
		s = stpcpy(s, "/");
		stpcpy(s, sourcename);

		command = replace_percent_placeholders(command,
											   "ARCHIVE-COMMAND", "fp",
											   targetname, tmppath);

		write_decrypted_segment(sourcepath, targetname, tmppath);
	}
	else
		command = replace_percent_placeholders(command,
											   "ARCHIVE-COMMAND", "fp",
											   targetname, sourcepath);

	if (system(command) != 0)
		pg_fatal("ARCHIVE-COMMAND \"%s\" failed: %m", command);

	free(command);

	if (issegment)
	{
		if (unlink(tmppath) < 0)
			pg_log_warning("could not remove file \"%s\": %m", tmppath);
		if (rmdir(tmpdir) < 0)
			pg_log_warning("could not remove directory \"%s\": %m", tmpdir);
	}

	return 0;
}
