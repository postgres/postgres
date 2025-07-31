#include "postgres_fe.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "access/xlog_smgr.h"
#include "common/logging.h"

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
	printf(_("%s wraps an archive command to make it archive unencrypted WAL.\n\n"), progname);
	printf(_("Usage:\n  %s %%p <archive command>\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -V, --version output version information, then exit\n"));
	printf(_("  -?, --help    show this help, then exit\n"));
}

int
main(int argc, char *argv[])
{
	const char *progname;
	char	   *sourcepath;
	char	   *sep;
	char	   *sourcename;
	char		tmpdir[MAXPGPATH] = TMPFS_DIRECTORY "/pg_tde_archiveXXXXXX";
	char		tmppath[MAXPGPATH];
	bool		issegment;
	pid_t		child;
	int			status;
	int			r;

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
			puts("pg_tde_archive_deceypt (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	if (argc < 3)
	{
		pg_log_error("too few arguments");
		pg_log_error_detail("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	sourcepath = argv[1];

	pg_tde_fe_init("pg_tde");
	TDEXLogSmgrInit();

	sep = strrchr(sourcepath, '/');

	if (sep != NULL)
		sourcename = sep + 1;
	else
		sourcename = sourcepath;

	issegment = is_segment(sourcename);

	if (issegment)
	{
		char	   *s;

		if (mkdtemp(tmpdir) == NULL)
			pg_fatal("could not create temporary directory \"%s\": %m", tmpdir);

		s = stpcpy(tmppath, tmpdir);
		s = stpcpy(s, "/");
		stpcpy(s, sourcename);

		for (int i = 2; i < argc; i++)
			if (strcmp(sourcepath, argv[i]) == 0)
				argv[i] = tmppath;

		write_decrypted_segment(sourcepath, sourcename, tmppath);
	}

	child = fork();
	if (child == 0)
	{
		if (execvp(argv[2], argv + 2) < 0)
			pg_fatal("exec failed: %m");
	}
	else if (child < 0)
		pg_fatal("could not create background process: %m");

	r = waitpid(child, &status, 0);
	if (r == (pid_t) -1)
		pg_fatal("could not wait for child process: %m");
	if (r != child)
		pg_fatal("child %d died, expected %d", (int) r, (int) child);
	if (status != 0)
	{
		char	   *reason = wait_result_to_str(status);

		pg_fatal("%s", reason);
		/* keep lsan happy */
		free(reason);
	}

	if (issegment)
	{
		if (unlink(tmppath) < 0)
			pg_log_warning("could not remove file \"%s\": %m", tmppath);
		if (rmdir(tmpdir) < 0)
			pg_log_warning("could not remove directory \"%s\": %m", tmpdir);
	}

	return 0;
}
