#include "postgres_fe.h"

#include "access/xlog_internal.h"
#include "access/xlog_smgr.h"
#include "common/logging.h"
#include "common/percentrepl.h"

#include "access/pg_tde_fe_init.h"
#include "access/pg_tde_xlog_smgr.h"

#define TMPFS_DIRECTORY "/dev/shm"

/*
 * Partial WAL segments are archived but never automatically fetched from the
 * archive by the restore_command. We support them here for symmetry though
 * since if someone would want to fetch a partial segment from the archive and
 * write it to pg_wal then they would want it encrypted.
 */
static bool
is_segment(const char *filename)
{
	return strspn(filename, "0123456789ABCDEF") == XLOG_FNAME_LEN &&
		(filename[XLOG_FNAME_LEN] == '\0' || strcmp(filename + XLOG_FNAME_LEN, ".partial") == 0);
}

static void
write_encrypted_segment(const char *segpath, const char *segname, const char *tmppath)
{
	int			tmpfd;
	int			segfd;
	PGAlignedXLogBlock buf;
	int			r;
	int			w;
	int			pos = 0;
	XLogLongPageHeader longhdr;
	int			walsegsz;
	TimeLineID	tli;
	XLogSegNo	segno;

	tmpfd = open(tmppath, O_RDONLY | PG_BINARY, 0);
	if (tmpfd < 0)
		pg_fatal("could not open file \"%s\": %m", tmppath);

	segfd = open(segpath, O_CREAT | O_WRONLY | PG_BINARY, 0666);
	if (segfd < 0)
		pg_fatal("could not open file \"%s\": %m", segpath);

	r = read(tmpfd, buf.data, XLOG_BLCKSZ);

	if (r < 0)
		pg_fatal("could not read file \"%s\": %m", tmppath);
	else if (r != XLOG_BLCKSZ)
		pg_fatal("could not read file \"%s\": read %d of %d",
				 tmppath, r, XLOG_BLCKSZ);

	longhdr = (XLogLongPageHeader) buf.data;
	walsegsz = longhdr->xlp_seg_size;

	if (!IsValidWalSegSize(walsegsz))
	{
		pg_log_error(ngettext("invalid WAL segment size in WAL file \"%s\" (%d byte)",
							  "invalid WAL segment size in WAL file \"%s\" (%d bytes)",
							  walsegsz),
					 segname, walsegsz);
		pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
		exit(1);
	}

	XLogFromFileName(segname, &tli, &segno, walsegsz);

	TDEXLogSmgrInitWriteOldKeys();

	w = xlog_smgr->seg_write(segfd, buf.data, r, pos, tli, segno, walsegsz);

	if (w < 0)
		pg_fatal("could not write file \"%s\": %m", segpath);
	else if (w != r)
		pg_fatal("could not write file \"%s\": wrote %d of %d",
				 segpath, w, r);

	pos += w;

	while (1)
	{
		r = read(tmpfd, buf.data, XLOG_BLCKSZ);

		if (r == 0)
			break;
		else if (r < 0)
			pg_fatal("could not read file \"%s\": %m", tmppath);

		w = xlog_smgr->seg_write(segfd, buf.data, r, pos, tli, segno, walsegsz);

		if (w < 0)
			pg_fatal("could not write file \"%s\": %m", segpath);
		else if (w != r)
			pg_fatal("could not write file \"%s\": wrote %d of %d",
					 segpath, w, r);

		pos += w;
	}

	close(segfd);
	close(tmpfd);
}

static void
usage(const char *progname)
{
	printf(_("%s wraps a restore command to encrypt its returned WAL.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]\n"), progname);
	printf(_("  %s SOURCE-NAME DEST-PATH RESTORE-COMMAND\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -V, --version   output version information, then exit\n"));
	printf(_("  -?, --help      show this help, then exit\n"));
	printf(_("  SOURCE-NAME     name of the WAL file to retrieve from archive\n"));
	printf(_("  DEST-PATH       path where the encrypted WAL segment should be written\n"));
	printf(_("  RESTORE-COMMAND restore command to wrap, %%p will be replaced with the path\n"
			 "                  where it should write the unencrypted WAL segment, %%f with\n"
			 "                  the WAL segment's name\n"));
	printf(_("\n"));
	printf(_("Note that any %%f or %%p parameter in RESTORE-COMMAND will have to be escaped\n"
			 "as %%%%f or %%%%p respectively if used as restore_command in postgresql.conf.\n"
			 "e.g.\n"
			 "  restore_command='%s %%f %%p \"cp /mnt/server/archivedir/%%%%f %%%%p\"'\n"
			 "or\n"
			 "  restore_command='%s %%f %%p \"pgbackrest --stanza=your_stanza archive-get %%%%f \\\"%%%%p\\\"\"'\n"
			 "\n"), progname, progname);
}

int
main(int argc, char *argv[])
{
	const char *progname;
	char	   *sourcename;
	char	   *targetpath;
	char	   *command;
	char	   *sep;
	char	   *targetname;
	char		tmpdir[MAXPGPATH] = TMPFS_DIRECTORY "/pg_tde_restoreXXXXXX";
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
			puts("pg_tde_restore_encrypt (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	if (argc != 4)
	{
		pg_log_error("wrong number of arguments, 3 expected");
		pg_log_error_detail("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	sourcename = argv[1];
	targetpath = argv[2];
	command = argv[3];

	pg_tde_fe_init("pg_tde");
	TDEXLogSmgrInit();

	sep = strrchr(targetpath, '/');

	if (sep != NULL)
		targetname = sep + 1;
	else
		targetname = targetpath;

	issegment = is_segment(sourcename);

	if (issegment)
	{
		char	   *s;

		if (mkdtemp(tmpdir) == NULL)
			pg_fatal("could not create temporary directory \"%s\": %m", tmpdir);

		s = stpcpy(tmppath, tmpdir);
		s = stpcpy(s, "/");
		stpcpy(s, targetname);

		command = replace_percent_placeholders(command,
											   "RESTORE-COMMAND", "fp",
											   sourcename, tmppath);
	}
	else
		command = replace_percent_placeholders(command,
											   "RESTORE-COMMAND", "fp",
											   sourcename, targetpath);

	if (system(command) != 0)
		pg_fatal("RESTORE-COMMAND \"%s\" failed: %m", command);

	free(command);

	if (issegment)
	{
		write_encrypted_segment(targetpath, sourcename, tmppath);

		if (unlink(tmppath) < 0)
			pg_log_warning("could not remove file \"%s\": %m", tmppath);
		if (rmdir(tmpdir) < 0)
			pg_log_warning("could not remove directory \"%s\": %m", tmpdir);
	}

	return 0;
}
