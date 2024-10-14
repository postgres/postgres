
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

# This regression test demonstrates that the pg_amcheck binary correctly
# identifies specific kinds of corruption within pages.  To test this, we need
# a mechanism to create corrupt pages with predictable, repeatable corruption.
# The postgres backend cannot be expected to help us with this, as its design
# is not consistent with the goal of intentionally corrupting pages.
#
# Instead, we create a table to corrupt, and with careful consideration of how
# postgresql lays out heap pages, we seek to offsets within the page and
# overwrite deliberately chosen bytes with specific values calculated to
# corrupt the page in expected ways.  We then verify that pg_amcheck reports
# the corruption, and that it runs without crashing.  Note that the backend
# cannot simply be started to run queries against the corrupt table, as the
# backend will crash, at least for some of the corruption types we generate.
#
# Autovacuum potentially touching the table in the background makes the exact
# behavior of this test harder to reason about.  We turn it off to keep things
# simpler.  We use a "belt and suspenders" approach, turning it off for the
# system generally in postgresql.conf, and turning it off specifically for the
# test table.
#
# This test depends on the table being written to the heap file exactly as we
# expect it to be, so we take care to arrange the columns of the table, and
# insert rows of the table, that give predictable sizes and locations within
# the table page.
#
# The HeapTupleHeaderData has 23 bytes of fixed size fields before the variable
# length t_bits[] array.  We have exactly 3 columns in the table, so natts = 3,
# t_bits is 1 byte long, and t_hoff = MAXALIGN(23 + 1) = 24.
#
# We're not too fussy about which datatypes we use for the test, but we do care
# about some specific properties.  We'd like to test both fixed size and
# varlena types.  We'd like some varlena data inline and some toasted.  And
# we'd like the layout of the table such that the datums land at predictable
# offsets within the tuple.  We choose a structure without padding on all
# supported architectures:
#
#	a BIGINT
#	b TEXT
#	c TEXT
#
# We always insert a 7-ascii character string into field 'b', which with a
# 1-byte varlena header gives an 8 byte inline value.  We always insert a long
# text string in field 'c', long enough to force toast storage.
#
# We choose to read and write binary copies of our table's tuples, using perl's
# pack() and unpack() functions.  Perl uses a packing code system in which:
#
#	l = "signed 32-bit Long",
#	L = "Unsigned 32-bit Long",
#	S = "Unsigned 16-bit Short",
#	C = "Unsigned 8-bit Octet",
#
# Each tuple in our table has a layout as follows:
#
#    xx xx xx xx            t_xmin: xxxx		offset = 0		L
#    xx xx xx xx            t_xmax: xxxx		offset = 4		L
#    xx xx xx xx          t_field3: xxxx		offset = 8		L
#    xx xx                   bi_hi: xx			offset = 12		S
#    xx xx                   bi_lo: xx			offset = 14		S
#    xx xx                ip_posid: xx			offset = 16		S
#    xx xx             t_infomask2: xx			offset = 18		S
#    xx xx              t_infomask: xx			offset = 20		S
#    xx                     t_hoff: x			offset = 22		C
#    xx                     t_bits: x			offset = 23		C
#    xx xx xx xx xx xx xx xx   'a': xxxxxxxx	offset = 24		LL
#    xx xx xx xx xx xx xx xx   'b': xxxxxxxx	offset = 32		CCCCCCCC
#    xx xx xx xx xx xx xx xx   'c': xxxxxxxx	offset = 40		CCllLL
#    xx xx xx xx xx xx xx xx      : xxxxxxxx	 ...continued
#    xx xx                        : xx			 ...continued
#
# We could choose to read and write columns 'b' and 'c' in other ways, but
# it is convenient enough to do it this way.  We define packing code
# constants here, where they can be compared easily against the layout.

use constant HEAPTUPLE_PACK_CODE => 'LLLSSSSSCCLLCCCCCCCCCCllLL';
use constant HEAPTUPLE_PACK_LENGTH => 58;    # Total size

# Read a tuple of our table from a heap page.
#
# Takes an open filehandle to the heap file, and the offset of the tuple.
#
# Rather than returning the binary data from the file, unpacks the data into a
# perl hash with named fields.  These fields exactly match the ones understood
# by write_tuple(), below.  Returns a reference to this hash.
#
sub read_tuple
{
	my ($fh, $offset) = @_;
	my ($buffer, %tup);
	sysseek($fh, $offset, 0)
	  or BAIL_OUT("sysseek failed: $!");
	defined(sysread($fh, $buffer, HEAPTUPLE_PACK_LENGTH))
	  or BAIL_OUT("sysread failed: $!");

	@_ = unpack(HEAPTUPLE_PACK_CODE, $buffer);
	%tup = (
		t_xmin => shift,
		t_xmax => shift,
		t_field3 => shift,
		bi_hi => shift,
		bi_lo => shift,
		ip_posid => shift,
		t_infomask2 => shift,
		t_infomask => shift,
		t_hoff => shift,
		t_bits => shift,
		a_1 => shift,
		a_2 => shift,
		b_header => shift,
		b_body1 => shift,
		b_body2 => shift,
		b_body3 => shift,
		b_body4 => shift,
		b_body5 => shift,
		b_body6 => shift,
		b_body7 => shift,
		c_va_header => shift,
		c_va_vartag => shift,
		c_va_rawsize => shift,
		c_va_extinfo => shift,
		c_va_valueid => shift,
		c_va_toastrelid => shift);
	# Stitch together the text for column 'b'
	$tup{b} = join('', map { chr($tup{"b_body$_"}) } (1 .. 7));
	return \%tup;
}

# Write a tuple of our table to a heap page.
#
# Takes an open filehandle to the heap file, the offset of the tuple, and a
# reference to a hash with the tuple values, as returned by read_tuple().
# Writes the tuple fields from the hash into the heap file.
#
# The purpose of this function is to write a tuple back to disk with some
# subset of fields modified.  The function does no error checking.  Use
# cautiously.
#
sub write_tuple
{
	my ($fh, $offset, $tup) = @_;
	my $buffer = pack(
		HEAPTUPLE_PACK_CODE,
		$tup->{t_xmin}, $tup->{t_xmax},
		$tup->{t_field3}, $tup->{bi_hi},
		$tup->{bi_lo}, $tup->{ip_posid},
		$tup->{t_infomask2}, $tup->{t_infomask},
		$tup->{t_hoff}, $tup->{t_bits},
		$tup->{a_1}, $tup->{a_2},
		$tup->{b_header}, $tup->{b_body1},
		$tup->{b_body2}, $tup->{b_body3},
		$tup->{b_body4}, $tup->{b_body5},
		$tup->{b_body6}, $tup->{b_body7},
		$tup->{c_va_header}, $tup->{c_va_vartag},
		$tup->{c_va_rawsize}, $tup->{c_va_extinfo},
		$tup->{c_va_valueid}, $tup->{c_va_toastrelid});
	sysseek($fh, $offset, 0)
	  or BAIL_OUT("sysseek failed: $!");
	defined(syswrite($fh, $buffer, HEAPTUPLE_PACK_LENGTH))
	  or BAIL_OUT("syswrite failed: $!");
	return;
}

# Set umask so test directories and files are created with default permissions
umask(0077);

my $pred_xmax;
my $pred_posid;
my $aborted_xid;
# Set up the node.  Once we create and corrupt the table,
# autovacuum workers visiting the table could crash the backend.
# Disable autovacuum so that won't happen.
my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->append_conf('postgresql.conf', 'max_prepared_transactions=10');

# Start the node and load the extensions.  We depend on both
# amcheck and pageinspect for this test.
$node->start;
my $port = $node->port;
my $pgdata = $node->data_dir;
$node->safe_psql('postgres', "CREATE EXTENSION amcheck");
$node->safe_psql('postgres', "CREATE EXTENSION pageinspect");

# Get a non-zero datfrozenxid
$node->safe_psql('postgres', qq(VACUUM FREEZE));

# Create the test table with precisely the schema that our corruption function
# expects.
$node->safe_psql(
	'postgres', qq(
		CREATE TABLE public.test (a BIGINT, b TEXT, c TEXT);
		ALTER TABLE public.test SET (autovacuum_enabled=false);
		ALTER TABLE public.test ALTER COLUMN c SET STORAGE EXTERNAL;
		CREATE INDEX test_idx ON public.test(a, b);
	));

# We want (0 < datfrozenxid < test.relfrozenxid).  To achieve this, we freeze
# an otherwise unused table, public.junk, prior to inserting data and freezing
# public.test
$node->safe_psql(
	'postgres', qq(
		CREATE TABLE public.junk AS SELECT 'junk'::TEXT AS junk_column;
		ALTER TABLE public.junk SET (autovacuum_enabled=false);
		VACUUM FREEZE public.junk
	));

my $rel = $node->safe_psql('postgres',
	qq(SELECT pg_relation_filepath('public.test')));
my $relpath = "$pgdata/$rel";

# Initial setup for the public.test table.
# $ROWCOUNT is the total number of rows that we expect to insert into the page.
# $ROWCOUNT_BASIC is the number of those rows that are related to basic
# tuple validation, rather than update chain validation.
my $ROWCOUNT = 44;
my $ROWCOUNT_BASIC = 16;

# First insert data needed for tests unrelated to update chain validation.
# Then freeze the page. These tuples are at offset numbers 1 to 16.
$node->safe_psql(
	'postgres', qq(
	INSERT INTO public.test (a, b, c)
		SELECT
			x'DEADF9F9DEADF9F9'::bigint,
			'abcdefg',
			repeat('w', 10000)
	FROM generate_series(1, $ROWCOUNT_BASIC);
	VACUUM FREEZE public.test;)
);

# Create some simple HOT update chains for line pointer validation. After
# the page is HOT pruned, we'll have two redirects line pointers each pointing
# to a tuple. We'll then change the second redirect to point to the same
# tuple as the first one and verify that we can detect corruption.
$node->safe_psql(
	'postgres', qq(
		INSERT INTO public.test (a, b, c)
			VALUES ( x'DEADF9F9DEADF9F9'::bigint, 'abcdefg',
					 generate_series(1,2)); -- offset numbers 17 and 18
		UPDATE public.test SET c = 'a' WHERE c = '1'; -- offset number 19
		UPDATE public.test SET c = 'a' WHERE c = '2'; -- offset number 20
	));

# Create some more HOT update chains.
$node->safe_psql(
	'postgres', qq(
		INSERT INTO public.test (a, b, c)
			VALUES ( x'DEADF9F9DEADF9F9'::bigint, 'abcdefg',
					 generate_series(3,6)); -- offset numbers 21 through 24
		UPDATE public.test SET c = 'a' WHERE c = '3'; -- offset number 25
		UPDATE public.test SET c = 'a' WHERE c = '4'; -- offset number 26
	));

# Negative test case of HOT-pruning with aborted tuple.
$node->safe_psql(
	'postgres', qq(
		BEGIN;
		UPDATE public.test SET c = 'a' WHERE c = '5'; -- offset number 27
		ABORT;
		VACUUM FREEZE public.test;
	));

# Next update on any tuple will be stored at the same place of tuple inserted
# by aborted transaction. This should not cause the table to appear corrupt.
$node->safe_psql(
	'postgres', qq(
		UPDATE public.test SET c = 'a' WHERE c = '6'; -- offset number 27 again
		VACUUM FREEZE public.test;
	));

# Data for HOT chain validation, so not calling VACUUM FREEZE.
$node->safe_psql(
	'postgres', qq(
		INSERT INTO public.test (a, b, c)
			VALUES ( x'DEADF9F9DEADF9F9'::bigint, 'abcdefg',
					 generate_series(7,15)); -- offset numbers 28 to 36
		UPDATE public.test SET c = 'a' WHERE c = '7'; -- offset number 37
		UPDATE public.test SET c = 'a' WHERE c = '10'; -- offset number 38
		UPDATE public.test SET c = 'a' WHERE c = '11'; -- offset number 39
		UPDATE public.test SET c = 'a' WHERE c = '12'; -- offset number 40
		UPDATE public.test SET c = 'a' WHERE c = '13'; -- offset number 41
		UPDATE public.test SET c = 'a' WHERE c = '14'; -- offset number 42
		UPDATE public.test SET c = 'a' WHERE c = '15'; -- offset number 43
	));

# Need one aborted transaction to test corruption in HOT chains.
$node->safe_psql(
	'postgres', qq(
		BEGIN;
		UPDATE public.test SET c = 'a' WHERE c = '9'; -- offset number 44
		ABORT;
	));

# Need one in-progress transaction to test few corruption in HOT chains.
# We are creating PREPARE TRANSACTION here as these will not be aborted
# even if we stop the node.
$node->safe_psql(
	'postgres', qq(
		BEGIN;
		PREPARE TRANSACTION 'in_progress_tx';
	));
my $in_progress_xid = $node->safe_psql(
	'postgres', qq(
		SELECT transaction FROM pg_prepared_xacts;
	));

my $relfrozenxid = $node->safe_psql('postgres',
	q(select relfrozenxid from pg_class where relname = 'test'));
my $datfrozenxid = $node->safe_psql('postgres',
	q(select datfrozenxid from pg_database where datname = 'postgres'));

# Sanity check that our 'test' table has a relfrozenxid newer than the
# datfrozenxid for the database, and that the datfrozenxid is greater than the
# first normal xid.  We rely on these invariants in some of our tests.
if ($datfrozenxid <= 3 || $datfrozenxid >= $relfrozenxid)
{
	$node->clean_node;
	plan skip_all =>
	  "Xid thresholds not as expected: got datfrozenxid = $datfrozenxid, relfrozenxid = $relfrozenxid";
	exit;
}

# Find where each of the tuples is located on the page. If a particular
# line pointer is a redirect rather than a tuple, we record the offset as -1.
my @lp_off = split '\n', $node->safe_psql(
	'postgres', qq(
	    SELECT CASE WHEN lp_flags = 2 THEN -1 ELSE lp_off END
	    FROM heap_page_items(get_raw_page('test', 'main', 0))
    )
);
scalar @lp_off == $ROWCOUNT or BAIL_OUT("row offset counts mismatch");

# Sanity check that our 'test' table on disk layout matches expectations.  If
# this is not so, we will have to skip the test until somebody updates the test
# to work on this platform.
$node->stop;
my $file;
open($file, '+<', $relpath)
  or BAIL_OUT("open failed: $!");
binmode $file;

my $ENDIANNESS;
for (my $tupidx = 0; $tupidx < $ROWCOUNT; $tupidx++)
{
	my $offnum = $tupidx + 1;    # offnum is 1-based, not zero-based
	my $offset = $lp_off[$tupidx];
	next if $offset == -1;       # ignore redirect line pointers
	my $tup = read_tuple($file, $offset);

	# Sanity-check that the data appears on the page where we expect.
	my $a_1 = $tup->{a_1};
	my $a_2 = $tup->{a_2};
	my $b = $tup->{b};
	if ($a_1 != 0xDEADF9F9 || $a_2 != 0xDEADF9F9 || $b ne 'abcdefg')
	{
		close($file);    # ignore errors on close; we're exiting anyway
		$node->clean_node;
		plan skip_all => sprintf(
			"Page layout of index %d differs from our expectations: expected (%x, %x, \"%s\"), got (%x, %x, \"%s\")",
			$tupidx, 0xDEADF9F9, 0xDEADF9F9, "abcdefg", $a_1, $a_2,
			# escape non-word characters to avoid confusing the terminal
			$b =~ s{(\W)}{ sprintf '\x%02x', ord($1) }aegr);
		exit;
	}

	# Determine endianness of current platform from the 1-byte varlena header
	$ENDIANNESS = $tup->{b_header} == 0x11 ? "little" : "big";
}
close($file)
  or BAIL_OUT("close failed: $!");
$node->start;

# Ok, Xids and page layout look ok.  We can run corruption tests.

# Check that pg_amcheck runs against the uncorrupted table without error.
$node->command_ok(
	[ 'pg_amcheck', '-p', $port, 'postgres' ],
	'pg_amcheck test table, prior to corruption');

# Check that pg_amcheck runs against the uncorrupted table and index without error.
$node->command_ok([ 'pg_amcheck', '-p', $port, 'postgres' ],
	'pg_amcheck test table and index, prior to corruption');

$node->stop;

# Some #define constants from access/htup_details.h for use while corrupting.
use constant HEAP_HASNULL => 0x0001;
use constant HEAP_XMAX_LOCK_ONLY => 0x0080;
use constant HEAP_XMIN_COMMITTED => 0x0100;
use constant HEAP_XMIN_INVALID => 0x0200;
use constant HEAP_XMAX_COMMITTED => 0x0400;
use constant HEAP_XMAX_INVALID => 0x0800;
use constant HEAP_NATTS_MASK => 0x07FF;
use constant HEAP_XMAX_IS_MULTI => 0x1000;
use constant HEAP_KEYS_UPDATED => 0x2000;
use constant HEAP_HOT_UPDATED => 0x4000;
use constant HEAP_ONLY_TUPLE => 0x8000;
use constant HEAP_UPDATED => 0x2000;

# Helper function to generate a regular expression matching the header we
# expect verify_heapam() to return given which fields we expect to be non-null.
sub header
{
	my ($blkno, $offnum, $attnum) = @_;
	return
	  qr/heap table "postgres\.public\.test", block $blkno, offset $offnum, attribute $attnum:\s+/ms
	  if (defined $attnum);
	return
	  qr/heap table "postgres\.public\.test", block $blkno, offset $offnum:\s+/ms
	  if (defined $offnum);
	return qr/heap table "postgres\.public\.test", block $blkno:\s+/ms
	  if (defined $blkno);
	return qr/heap table "postgres\.public\.test":\s+/ms;
}

# Corrupt the tuples, one type of corruption per tuple.  Some types of
# corruption cause verify_heapam to skip to the next tuple without
# performing any remaining checks, so we can't exercise the system properly if
# we focus all our corruption on a single tuple.
#
my @expected;
open($file, '+<', $relpath)
  or BAIL_OUT("open failed: $!");
binmode $file;

for (my $tupidx = 0; $tupidx < $ROWCOUNT; $tupidx++)
{
	my $offnum = $tupidx + 1;    # offnum is 1-based, not zero-based
	my $offset = $lp_off[$tupidx];
	my $header = header(0, $offnum, undef);

	# Read tuple, if there is one.
	my $tup = $offset == -1 ? undef : read_tuple($file, $offset);

	if ($offnum == 1)
	{
		# Corruptly set xmin < relfrozenxid
		my $xmin = $relfrozenxid - 1;
		$tup->{t_xmin} = $xmin;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
		$tup->{t_infomask} &= ~HEAP_XMIN_INVALID;

		# Expected corruption report
		push @expected,
		  qr/${header}xmin $xmin precedes relation freeze threshold 0:\d+/;
	}
	elsif ($offnum == 2)
	{
		# Corruptly set xmin < datfrozenxid
		my $xmin = 3;
		$tup->{t_xmin} = $xmin;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
		$tup->{t_infomask} &= ~HEAP_XMIN_INVALID;

		push @expected,
		  qr/${$header}xmin $xmin precedes oldest valid transaction ID 0:\d+/;
	}
	elsif ($offnum == 3)
	{
		# Corruptly set xmin < datfrozenxid, further back, noting circularity
		# of xid comparison.
		my $xmin = 4026531839;
		$tup->{t_xmin} = $xmin;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
		$tup->{t_infomask} &= ~HEAP_XMIN_INVALID;

		push @expected,
		  qr/${$header}xmin ${xmin} precedes oldest valid transaction ID 0:\d+/;
	}
	elsif ($offnum == 4)
	{
		# Corruptly set xmax < relminmxid;
		my $xmax = 4026531839;
		$tup->{t_xmax} = $xmax;
		$tup->{t_infomask} &= ~HEAP_XMAX_INVALID;

		push @expected,
		  qr/${$header}xmax ${xmax} precedes oldest valid transaction ID 0:\d+/;
	}
	elsif ($offnum == 5)
	{
		# Corrupt the tuple t_hoff, but keep it aligned properly
		$tup->{t_hoff} += 128;

		push @expected,
		  qr/${$header}data begins at offset 152 beyond the tuple length 58/,
		  qr/${$header}tuple data should begin at byte 24, but actually begins at byte 152 \(3 attributes, no nulls\)/;
	}
	elsif ($offnum == 6)
	{
		# Corrupt the tuple t_hoff, wrong alignment
		$tup->{t_hoff} += 3;

		push @expected,
		  qr/${$header}tuple data should begin at byte 24, but actually begins at byte 27 \(3 attributes, no nulls\)/;
	}
	elsif ($offnum == 7)
	{
		# Corrupt the tuple t_hoff, underflow but correct alignment
		$tup->{t_hoff} -= 8;

		push @expected,
		  qr/${$header}tuple data should begin at byte 24, but actually begins at byte 16 \(3 attributes, no nulls\)/;
	}
	elsif ($offnum == 8)
	{
		# Corrupt the tuple t_hoff, underflow and wrong alignment
		$tup->{t_hoff} -= 3;

		push @expected,
		  qr/${$header}tuple data should begin at byte 24, but actually begins at byte 21 \(3 attributes, no nulls\)/;
	}
	elsif ($offnum == 9)
	{
		# Corrupt the tuple to look like it has lots of attributes, not just 3
		$tup->{t_infomask2} |= HEAP_NATTS_MASK;

		push @expected,
		  qr/${$header}number of attributes 2047 exceeds maximum expected for table 3/;
	}
	elsif ($offnum == 10)
	{
		# Corrupt the tuple to look like it has lots of attributes, some of
		# them null.  This falsely creates the impression that the t_bits
		# array is longer than just one byte, but t_hoff still says otherwise.
		$tup->{t_infomask} |= HEAP_HASNULL;
		$tup->{t_infomask2} |= HEAP_NATTS_MASK;
		$tup->{t_bits} = 0xAA;

		push @expected,
		  qr/${$header}tuple data should begin at byte 280, but actually begins at byte 24 \(2047 attributes, has nulls\)/;
	}
	elsif ($offnum == 11)
	{
		# Same as above, but this time t_hoff plays along
		$tup->{t_infomask} |= HEAP_HASNULL;
		$tup->{t_infomask2} |= (HEAP_NATTS_MASK & 0x40);
		$tup->{t_bits} = 0xAA;
		$tup->{t_hoff} = 32;

		push @expected,
		  qr/${$header}number of attributes 67 exceeds maximum expected for table 3/;
	}
	elsif ($offnum == 12)
	{
		# Overwrite column 'b' 1-byte varlena header and initial characters to
		# look like a long 4-byte varlena
		#
		# On little endian machines, bytes ending in two zero bits (xxxxxx00 bytes)
		# are 4-byte length word, aligned, uncompressed data (up to 1G).  We set the
		# high six bits to 111111 and the lower two bits to 00, then the next three
		# bytes with 0xFF using 0xFCFFFFFF.
		#
		# On big endian machines, bytes starting in two zero bits (00xxxxxx bytes)
		# are 4-byte length word, aligned, uncompressed data (up to 1G).  We set the
		# low six bits to 111111 and the high two bits to 00, then the next three
		# bytes with 0xFF using 0x3FFFFFFF.
		#
		$tup->{b_header} = $ENDIANNESS eq 'little' ? 0xFC : 0x3F;
		$tup->{b_body1} = 0xFF;
		$tup->{b_body2} = 0xFF;
		$tup->{b_body3} = 0xFF;

		$header = header(0, $offnum, 1);
		push @expected,
		  qr/${header}attribute with length \d+ ends at offset \d+ beyond total tuple length \d+/;
	}
	elsif ($offnum == 13)
	{
		# Corrupt the bits in column 'c' toast pointer
		$tup->{c_va_valueid} = 0xFFFFFFFF;

		$header = header(0, $offnum, 2);
		push @expected, qr/${header}toast value \d+ not found in toast table/;
	}
	elsif ($offnum == 14)
	{
		# Set both HEAP_XMAX_COMMITTED and HEAP_XMAX_IS_MULTI
		$tup->{t_infomask} |= HEAP_XMAX_COMMITTED;
		$tup->{t_infomask} |= HEAP_XMAX_IS_MULTI;
		$tup->{t_xmax} = 4;

		push @expected,
		  qr/${header}multitransaction ID 4 equals or exceeds next valid multitransaction ID 1/;
	}
	elsif ($offnum == 15)
	{
		# Set both HEAP_XMAX_COMMITTED and HEAP_XMAX_IS_MULTI
		$tup->{t_infomask} |= HEAP_XMAX_COMMITTED;
		$tup->{t_infomask} |= HEAP_XMAX_IS_MULTI;
		$tup->{t_xmax} = 4000000000;

		push @expected,
		  qr/${header}multitransaction ID 4000000000 precedes relation minimum multitransaction ID threshold 1/;
	}
	elsif ($offnum == 16)    # Last offnum must equal ROWCOUNT
	{
		# Corruptly set xmin > next_xid to be in the future.
		my $xmin = 123456;
		$tup->{t_xmin} = $xmin;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
		$tup->{t_infomask} &= ~HEAP_XMIN_INVALID;

		push @expected,
		  qr/${$header}xmin ${xmin} equals or exceeds next valid transaction ID 0:\d+/;
	}
	elsif ($offnum == 17)
	{
		# at offnum 19 we will unset HEAP_ONLY_TUPLE flag
		die "offnum $offnum should be a redirect" if defined $tup;
		push @expected,
		  qr/${header}redirected line pointer points to a non-heap-only tuple at offset \d+/;
	}
	elsif ($offnum == 18)
	{
		# rewrite line pointer with lp_off = 17, lp_flags = 2, lp_len = 0.
		die "offnum $offnum should be a redirect" if defined $tup;
		sysseek($file, 92, 0) or BAIL_OUT("sysseek failed: $!");
		syswrite($file,
			pack("L", $ENDIANNESS eq 'little' ? 0x00010011 : 0x00230000))
		  or BAIL_OUT("syswrite failed: $!");
		push @expected,
		  qr/${header}redirected line pointer points to another redirected line pointer at offset \d+/;
	}
	elsif ($offnum == 19)
	{
		# unset HEAP_ONLY_TUPLE flag, so that update chain validation will
		# complain about offset 17
		$tup->{t_infomask2} &= ~HEAP_ONLY_TUPLE;
	}
	elsif ($offnum == 22)
	{
		# rewrite line pointer with lp.off = 25, lp_flags = 2, lp_len = 0
		sysseek($file, 108, 0) or BAIL_OUT("sysseek failed: $!");
		syswrite($file,
			pack("L", $ENDIANNESS eq 'little' ? 0x00010019 : 0x00330000))
		  or BAIL_OUT("syswrite failed: $!");
		push @expected,
		  qr/${header}redirect line pointer points to offset \d+, but offset \d+ also points there/;
	}
	elsif ($offnum == 28)
	{
		$tup->{t_infomask2} &= ~HEAP_HOT_UPDATED;
		push @expected,
		  qr/${header}non-heap-only update produced a heap-only tuple at offset \d+/;

		# Save these values so we can insert them into the tuple at offnum 29.
		$pred_xmax = $tup->{t_xmax};
		$pred_posid = $tup->{ip_posid};
	}
	elsif ($offnum == 29)
	{
		# Copy these values from the tuple at offset 28.
		$tup->{t_xmax} = $pred_xmax;
		$tup->{ip_posid} = $pred_posid;
		push @expected,
		  qr/${header}tuple points to new version at offset \d+, but offset \d+ also points there/;
	}
	elsif ($offnum == 30)
	{
		# Save xid, so we can insert into into tuple at offset 31.
		$aborted_xid = $tup->{t_xmax};
	}
	elsif ($offnum == 31)
	{
		# Set xmin to xmax of tuple at offset 30.
		$tup->{t_xmin} = $aborted_xid;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
		push @expected,
		  qr/${header}tuple with aborted xmin \d+ was updated to produce a tuple at offset \d+ with committed xmin \d+/;
	}
	elsif ($offnum == 32)
	{
		$tup->{t_infomask2} |= HEAP_ONLY_TUPLE;
		push @expected,
		  qr/${header}tuple is root of chain but is marked as heap-only tuple/;
		push @expected,
		  qr/${header}tuple is heap only, but not the result of an update/;
	}
	elsif ($offnum == 33)
	{
		# Tuple at offset 40 is the successor of this one; we'll corrupt it to
		# be non-heap-only.
		push @expected,
		  qr/${header}heap-only update produced a non-heap only tuple at offset \d+/;
	}
	elsif ($offnum == 34)
	{
		$tup->{t_xmax} = 0;
		push @expected,
		  qr/${header}tuple has been HOT updated, but xmax is 0/;
	}
	elsif ($offnum == 35)
	{
		$tup->{t_xmin} = $in_progress_xid;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
		push @expected,
		  qr/${header}tuple with in-progress xmin \d+ was updated to produce a tuple at offset \d+ with committed xmin \d+/;
	}
	elsif ($offnum == 36)
	{
		# Tuple at offset 43 is the successor of this one; we'll corrupt it to
		# have xmin = $in_progress_xid. By setting the xmax of this tuple to
		# the same value, we make it look like an update chain with an
		# in-progress XID following a committed one.
		$tup->{t_xmin} = $aborted_xid;
		$tup->{t_xmax} = $in_progress_xid;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
		push @expected,
		  qr/${header}tuple with aborted xmin \d+ was updated to produce a tuple at offset \d+ with in-progress xmin \d+/;
	}
	elsif ($offnum == 40)
	{
		# Tuple at offset 33 is the predecessor of this one; the error will
		# be reported there.
		$tup->{t_infomask2} &= ~HEAP_ONLY_TUPLE;
	}
	elsif ($offnum == 43)
	{
		# Tuple at offset 36 is the predecessor of this one; the error will
		# be reported there.
		$tup->{t_xmin} = $in_progress_xid;
		$tup->{t_infomask} &= ~HEAP_XMIN_COMMITTED;
	}
	else
	{
		# The tests for update chain validation end up creating a bunch of
		# tuples that aren't corrupted in any way e.g. because only one of
		# the two tuples in the update chain needs to be corrupted for the
		# test, or because one update chain is being made to erroneously
		# point into the middle of another that has nothing wrong with it.
		# In all such cases we need not write the tuple back to the file.
		next;
	}

	write_tuple($file, $offset, $tup) if defined $tup;
}
close($file)
  or BAIL_OUT("close failed: $!");
$node->start;

# Run pg_amcheck against the corrupt table with epoch=0, comparing actual
# corruption messages against the expected messages
$node->command_checks_all(
	[ 'pg_amcheck', '--no-dependent-indexes', '-p', $port, 'postgres' ],
	2, [@expected], [], 'Expected corruption message output');
$node->safe_psql(
	'postgres', qq(
                        COMMIT PREPARED 'in_progress_tx';
        ));

$node->teardown_node;
$node->clean_node;

done_testing();
