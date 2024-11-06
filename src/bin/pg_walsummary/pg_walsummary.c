/*-------------------------------------------------------------------------
 *
 * pg_walsummary.c
 *		Prints the contents of WAL summary files.
 *
 * Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_walsummary/pg_walsummary.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <limits.h>

#include "common/blkreftable.h"
#include "common/int.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "getopt_long.h"

typedef struct ws_options
{
	bool		individual;
	bool		quiet;
} ws_options;

typedef struct ws_file_info
{
	int			fd;
	char	   *filename;
} ws_file_info;

static BlockNumber *block_buffer = NULL;
static unsigned block_buffer_size = 512;	/* Initial size. */

static void dump_one_relation(ws_options *opt, RelFileLocator *rlocator,
							  ForkNumber forknum, BlockNumber limit_block,
							  BlockRefTableReader *reader);
static void help(const char *progname);
static int	compare_block_numbers(const void *a, const void *b);
static int	walsummary_read_callback(void *callback_arg, void *data,
									 int length);
static void walsummary_error_callback(void *callback_arg, char *fmt,...) pg_attribute_printf(2, 3);

/*
 * Main program.
 */
int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"individual", no_argument, NULL, 'i'},
		{"quiet", no_argument, NULL, 'q'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;
	ws_options	opt;

	memset(&opt, 0, sizeof(ws_options));

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_walsummary"));
	handle_help_version_opts(argc, argv, progname, help);

	/* process command-line options */
	while ((c = getopt_long(argc, argv, "iq",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'i':
				opt.individual = true;
				break;
			case 'q':
				opt.quiet = true;
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (optind >= argc)
	{
		pg_log_error("no input files specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	while (optind < argc)
	{
		ws_file_info ws;
		BlockRefTableReader *reader;
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber limit_block;

		ws.filename = argv[optind++];
		if ((ws.fd = open(ws.filename, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %m", ws.filename);

		reader = CreateBlockRefTableReader(walsummary_read_callback, &ws,
										   ws.filename,
										   walsummary_error_callback, NULL);
		while (BlockRefTableReaderNextRelation(reader, &rlocator, &forknum,
											   &limit_block))
			dump_one_relation(&opt, &rlocator, forknum, limit_block, reader);

		DestroyBlockRefTableReader(reader);
		close(ws.fd);
	}

	exit(0);
}

/*
 * Dump details for one relation.
 */
static void
dump_one_relation(ws_options *opt, RelFileLocator *rlocator,
				  ForkNumber forknum, BlockNumber limit_block,
				  BlockRefTableReader *reader)
{
	unsigned	i = 0;
	unsigned	nblocks;
	BlockNumber startblock = InvalidBlockNumber;
	BlockNumber endblock = InvalidBlockNumber;

	/* Dump limit block, if any. */
	if (limit_block != InvalidBlockNumber)
		printf("TS %u, DB %u, REL %u, FORK %s: limit %u\n",
			   rlocator->spcOid, rlocator->dbOid, rlocator->relNumber,
			   forkNames[forknum], limit_block);

	/* If we haven't allocated a block buffer yet, do that now. */
	if (block_buffer == NULL)
		block_buffer = palloc_array(BlockNumber, block_buffer_size);

	/* Try to fill the block buffer. */
	nblocks = BlockRefTableReaderGetBlocks(reader,
										   block_buffer,
										   block_buffer_size);

	/* If we filled the block buffer completely, we must enlarge it. */
	while (nblocks >= block_buffer_size)
	{
		unsigned	new_size;

		/* Double the size, being careful about overflow. */
		new_size = block_buffer_size * 2;
		if (new_size < block_buffer_size)
			new_size = PG_UINT32_MAX;
		block_buffer = repalloc_array(block_buffer, BlockNumber, new_size);

		/* Try to fill the newly-allocated space. */
		nblocks +=
			BlockRefTableReaderGetBlocks(reader,
										 block_buffer + block_buffer_size,
										 new_size - block_buffer_size);

		/* Save the new size for later calls. */
		block_buffer_size = new_size;
	}

	/* If we don't need to produce any output, skip the rest of this. */
	if (opt->quiet)
		return;

	/*
	 * Sort the returned block numbers. If the block reference table was using
	 * the bitmap representation for a given chunk, the block numbers in that
	 * chunk will already be sorted, but when the array-of-offsets
	 * representation is used, we can receive block numbers here out of order.
	 */
	qsort(block_buffer, nblocks, sizeof(BlockNumber), compare_block_numbers);

	/* Dump block references. */
	while (i < nblocks)
	{
		/*
		 * Find the next range of blocks to print, but if --individual was
		 * specified, then consider each block a separate range.
		 */
		startblock = endblock = block_buffer[i++];
		if (!opt->individual)
		{
			while (i < nblocks && block_buffer[i] == endblock + 1)
			{
				endblock++;
				i++;
			}
		}

		/* Format this range of block numbers as a string. */
		if (startblock == endblock)
			printf("TS %u, DB %u, REL %u, FORK %s: block %u\n",
				   rlocator->spcOid, rlocator->dbOid, rlocator->relNumber,
				   forkNames[forknum], startblock);
		else
			printf("TS %u, DB %u, REL %u, FORK %s: blocks %u..%u\n",
				   rlocator->spcOid, rlocator->dbOid, rlocator->relNumber,
				   forkNames[forknum], startblock, endblock);
	}
}

/*
 * Quicksort comparator for block numbers.
 */
static int
compare_block_numbers(const void *a, const void *b)
{
	BlockNumber aa = *(BlockNumber *) a;
	BlockNumber bb = *(BlockNumber *) b;

	return pg_cmp_u32(aa, bb);
}

/*
 * Error callback.
 */
void
walsummary_error_callback(void *callback_arg, char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	pg_log_generic_v(PG_LOG_ERROR, PG_LOG_PRIMARY, fmt, ap);
	va_end(ap);

	exit(1);
}

/*
 * Read callback.
 */
int
walsummary_read_callback(void *callback_arg, void *data, int length)
{
	ws_file_info *ws = callback_arg;
	int			rc;

	if ((rc = read(ws->fd, data, length)) < 0)
		pg_fatal("could not read file \"%s\": %m", ws->filename);

	return rc;
}

/*
 * help
 *
 * Prints help page for the program
 *
 * progname: the name of the executed program, such as "pg_walsummary"
 */
static void
help(const char *progname)
{
	printf(_("%s prints the contents of a WAL summary file.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... FILE...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -i, --individual          list block numbers individually, not as ranges\n"));
	printf(_("  -q, --quiet               don't print anything, just parse the files\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  -?, --help                show this help, then exit\n"));

	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
