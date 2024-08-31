# Copyright (c) 2023-2024, PostgreSQL Global Development Group
#
# Test detecting end-of-WAL conditions.  This test suite generates
# fake defective page and record headers to trigger various failure
# scenarios.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Fcntl qw(SEEK_SET);

use integer;    # causes / operator to use integer math

# Is this a big-endian system ("network" byte order)?  We can't use 'Q' in
# pack() calls because it's not available in some perl builds, so we need to
# break 64 bit LSN values into two 'I' values.  Fortunately we don't need to
# deal with high values, so we can just write 0 for the high order 32 bits, but
# we need to know the endianness to do that.
my $BIG_ENDIAN = pack("L", 0x12345678) eq pack("N", 0x12345678);

# Header size of record header.
my $RECORD_HEADER_SIZE = 24;

# Fields retrieved from code headers.
my @scan_result = scan_server_header('access/xlog_internal.h',
	'#define\s+XLOG_PAGE_MAGIC\s+(\w+)');
my $XLP_PAGE_MAGIC = hex($scan_result[0]);
@scan_result = scan_server_header('access/xlog_internal.h',
	'#define\s+XLP_FIRST_IS_CONTRECORD\s+(\w+)');
my $XLP_FIRST_IS_CONTRECORD = hex($scan_result[0]);

# Values queried from the server
my $WAL_SEGMENT_SIZE;
my $WAL_BLOCK_SIZE;
my $TLI;

# Build path of a WAL segment.
sub wal_segment_path
{
	my $node = shift;
	my $tli = shift;
	my $segment = shift;
	my $wal_path =
	  sprintf("%s/pg_wal/%08X%08X%08X", $node->data_dir, $tli, 0, $segment);
	return $wal_path;
}

# Calculate from a LSN (in bytes) its segment number and its offset.
sub lsn_to_segment_and_offset
{
	my $lsn = shift;
	return ($lsn / $WAL_SEGMENT_SIZE, $lsn % $WAL_SEGMENT_SIZE);
}

# Write some arbitrary data in WAL for the given segment at LSN.
# This should be called while the cluster is not running.
sub write_wal
{
	my $node = shift;
	my $tli = shift;
	my $lsn = shift;
	my $data = shift;

	my ($segment, $offset) = lsn_to_segment_and_offset($lsn);
	my $path = wal_segment_path($node, $tli, $segment);

	open my $fh, "+<:raw", $path or die;
	seek($fh, $offset, SEEK_SET) or die;
	print $fh $data;
	close $fh;
}

# Emit a WAL record of arbitrary size.  Returns the end LSN of the
# record inserted, in bytes.
sub emit_message
{
	my $node = shift;
	my $size = shift;
	return int(
		$node->safe_psql(
			'postgres',
			"SELECT pg_logical_emit_message(true, '', repeat('a', $size)) - '0/0'"
		));
}

# Get the current insert LSN of a node, in bytes.
sub get_insert_lsn
{
	my $node = shift;
	return int(
		$node->safe_psql(
			'postgres', "SELECT pg_current_wal_insert_lsn() - '0/0'"));
}

# Get GUC value, converted to an int.
sub get_int_setting
{
	my $node = shift;
	my $name = shift;
	return int(
		$node->safe_psql(
			'postgres',
			"SELECT setting FROM pg_settings WHERE name = '$name'"));
}

sub start_of_page
{
	my $lsn = shift;
	return $lsn & ~($WAL_BLOCK_SIZE - 1);
}

sub start_of_next_page
{
	my $lsn = shift;
	return start_of_page($lsn) + $WAL_BLOCK_SIZE;
}

# Build a fake WAL record header based on the data given by the caller.
# This needs to follow the format of the C structure XLogRecord.  To
# be inserted with write_wal().
sub build_record_header
{
	my $xl_tot_len = shift;
	my $xl_xid = shift || 0;
	my $xl_prev = shift || 0;
	my $xl_info = shift || 0;
	my $xl_rmid = shift || 0;
	my $xl_crc = shift || 0;

	# This needs to follow the structure XLogRecord:
	# I for xl_tot_len
	# I for xl_xid
	# II for xl_prev
	# C for xl_info
	# C for xl_rmid
	# BB for two bytes of padding
	# I for xl_crc
	return pack("IIIICCBBI",
		$xl_tot_len, $xl_xid,
		$BIG_ENDIAN ? 0        : $xl_prev,
		$BIG_ENDIAN ? $xl_prev : 0,
		$xl_info, $xl_rmid, 0, 0, $xl_crc);
}

# Build a fake WAL page header, based on the data given by the caller
# This needs to follow the format of the C structure XLogPageHeaderData.
# To be inserted with write_wal().
sub build_page_header
{
	my $xlp_magic = shift;
	my $xlp_info = shift || 0;
	my $xlp_tli = shift || 0;
	my $xlp_pageaddr = shift || 0;
	my $xlp_rem_len = shift || 0;

	# This needs to follow the structure XLogPageHeaderData:
	# S for xlp_magic
	# S for xlp_info
	# I for xlp_tli
	# II for xlp_pageaddr
	# I for xlp_rem_len
	return pack("SSIIII",
		$xlp_magic, $xlp_info, $xlp_tli,
		$BIG_ENDIAN ? 0             : $xlp_pageaddr,
		$BIG_ENDIAN ? $xlp_pageaddr : 0, $xlp_rem_len);
}

# Make sure we are far away enough from the end of a page that we could insert
# a couple of small records.  This inserts a few records of a fixed size, until
# the threshold gets close enough to the end of the WAL page inserting records
# to.
sub advance_out_of_record_splitting_zone
{
	my $node = shift;

	my $page_threshold = $WAL_BLOCK_SIZE / 4;
	my $end_lsn = get_insert_lsn($node);
	my $page_offset = $end_lsn % $WAL_BLOCK_SIZE;
	while ($page_offset >= $WAL_BLOCK_SIZE - $page_threshold)
	{
		emit_message($node, $page_threshold);
		$end_lsn = get_insert_lsn($node);
		$page_offset = $end_lsn % $WAL_BLOCK_SIZE;
	}
	return $end_lsn;
}

# Advance so close to the end of a page that an XLogRecordHeader would not
# fit on it.
sub advance_to_record_splitting_zone
{
	my $node = shift;

	my $end_lsn = get_insert_lsn($node);
	my $page_offset = $end_lsn % $WAL_BLOCK_SIZE;

	# Get fairly close to the end of a page in big steps
	while ($page_offset <= $WAL_BLOCK_SIZE - 512)
	{
		emit_message($node, $WAL_BLOCK_SIZE - $page_offset - 256);
		$end_lsn = get_insert_lsn($node);
		$page_offset = $end_lsn % $WAL_BLOCK_SIZE;
	}

	# Calibrate our message size so that we can get closer 8 bytes at
	# a time.
	my $message_size = $WAL_BLOCK_SIZE - 80;
	while ($page_offset <= $WAL_BLOCK_SIZE - $RECORD_HEADER_SIZE)
	{
		emit_message($node, $message_size);
		$end_lsn = get_insert_lsn($node);

		my $old_offset = $page_offset;
		$page_offset = $end_lsn % $WAL_BLOCK_SIZE;

		# Adjust the message size until it causes 8 bytes changes in
		# offset, enough to be able to split a record header.
		my $delta = $page_offset - $old_offset;
		if ($delta > 8)
		{
			$message_size -= 8;
		}
		elsif ($delta <= 0)
		{
			$message_size += 8;
		}
	}
	return $end_lsn;
}

# Setup a new node.  The configuration chosen here minimizes the number
# of arbitrary records that could get generated in a cluster.  Enlarging
# checkpoint_timeout avoids noise with checkpoint activity.  wal_level
# set to "minimal" avoids random standby snapshot records.  Autovacuum
# could also trigger randomly, generating random WAL activity of its own.
my $node = PostgreSQL::Test::Cluster->new("node");
$node->init;
$node->append_conf(
	'postgresql.conf',
	q[wal_level = minimal
					 autovacuum = off
					 checkpoint_timeout = '30min'
]);
$node->start;
$node->safe_psql('postgres', "CREATE TABLE t AS SELECT 42");

$WAL_SEGMENT_SIZE = get_int_setting($node, 'wal_segment_size');
$WAL_BLOCK_SIZE = get_int_setting($node, 'wal_block_size');
$TLI = $node->safe_psql('postgres',
	"SELECT timeline_id FROM pg_control_checkpoint();");

# Initial LSN may vary across systems due to different catalog contents set up
# by initdb.  Switch to a new WAL file so all systems start out in the same
# place.  The first test depends on trailing zeroes on a page with a valid
# header.
$node->safe_psql('postgres', "SELECT pg_switch_wal();");

my $end_lsn;
my $prev_lsn;

###########################################################################
note "Single-page end-of-WAL detection";
###########################################################################

# xl_tot_len is 0 (a common case, we hit trailing zeroes).
emit_message($node, 0);
$end_lsn = advance_out_of_record_splitting_zone($node);
$node->stop('immediate');
my $log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid record length at .*: expected at least 24, got 0", $log_size
	),
	"xl_tot_len zero");

# xl_tot_len is < 24 (presumably recycled garbage).
emit_message($node, 0);
$end_lsn = advance_out_of_record_splitting_zone($node);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn, build_record_header(23));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid record length at .*: expected at least 24, got 23",
		$log_size),
	"xl_tot_len short");

# xl_tot_len in final position, not big enough to span into a new page but
# also not eligible for regular record header validation
emit_message($node, 0);
$end_lsn = advance_to_record_splitting_zone($node);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn, build_record_header(1));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid record length at .*: expected at least 24, got 1", $log_size
	),
	"xl_tot_len short at end-of-page");

# Need more pages, but xl_prev check fails first.
emit_message($node, 0);
$end_lsn = advance_out_of_record_splitting_zone($node);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"record with incorrect prev-link 0/DEADBEEF at .*", $log_size),
	"xl_prev bad");

# xl_crc check fails.
emit_message($node, 0);
advance_out_of_record_splitting_zone($node);
$end_lsn = emit_message($node, 10);
$node->stop('immediate');
# Corrupt a byte in that record, breaking its CRC.
write_wal($node, $TLI, $end_lsn - 8, '!');
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"incorrect resource manager data checksum in record at .*", $log_size
	),
	"xl_crc bad");


###########################################################################
note "Multi-page end-of-WAL detection, header is not split";
###########################################################################

# This series of tests requires a valid xl_prev set in the record header
# written to WAL.

# Good xl_prev, we hit zero page next (zero magic).
emit_message($node, 0);
$prev_lsn = advance_out_of_record_splitting_zone($node);
$end_lsn = emit_message($node, 0);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 0, $prev_lsn));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid magic number 0000 .* LSN .*", $log_size),
	"xlp_magic zero");

# Good xl_prev, we hit garbage page next (bad magic).
emit_message($node, 0);
$prev_lsn = advance_out_of_record_splitting_zone($node);
$end_lsn = emit_message($node, 0);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 0, $prev_lsn));
write_wal(
	$node, $TLI,
	start_of_next_page($end_lsn),
	build_page_header(0xcafe, 0, 1, 0));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid magic number CAFE .* LSN .*", $log_size),
	"xlp_magic bad");

# Good xl_prev, we hit typical recycled page (good xlp_magic, bad
# xlp_pageaddr).
emit_message($node, 0);
$prev_lsn = advance_out_of_record_splitting_zone($node);
$end_lsn = emit_message($node, 0);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 0, $prev_lsn));
write_wal(
	$node, $TLI,
	start_of_next_page($end_lsn),
	build_page_header($XLP_PAGE_MAGIC, 0, 1, 0xbaaaaaad));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"unexpected pageaddr 0/BAAAAAAD in .*, LSN .*,", $log_size),
	"xlp_pageaddr bad");

# Good xl_prev, xlp_magic, xlp_pageaddr, but bogus xlp_info.
emit_message($node, 0);
$prev_lsn = advance_out_of_record_splitting_zone($node);
$end_lsn = emit_message($node, 0);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 42, $prev_lsn));
write_wal(
	$node, $TLI,
	start_of_next_page($end_lsn),
	build_page_header(
		$XLP_PAGE_MAGIC, 0x1234, 1, start_of_next_page($end_lsn)));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid info bits 1234 in .*, LSN .*,", $log_size),
	"xlp_info bad");

# Good xl_prev, xlp_magic, xlp_pageaddr, but xlp_info doesn't mention
# continuation record.
emit_message($node, 0);
$prev_lsn = advance_out_of_record_splitting_zone($node);
$end_lsn = emit_message($node, 0);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 42, $prev_lsn));
write_wal(
	$node, $TLI,
	start_of_next_page($end_lsn),
	build_page_header($XLP_PAGE_MAGIC, 0, 1, start_of_next_page($end_lsn)));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("there is no contrecord flag at .*", $log_size),
	"xlp_info lacks XLP_FIRST_IS_CONTRECORD");

# Good xl_prev, xlp_magic, xlp_pageaddr, xlp_info but xlp_rem_len doesn't add
# up.
emit_message($node, 0);
$prev_lsn = advance_out_of_record_splitting_zone($node);
$end_lsn = emit_message($node, 0);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 42, $prev_lsn));
write_wal(
	$node, $TLI,
	start_of_next_page($end_lsn),
	build_page_header(
		$XLP_PAGE_MAGIC, $XLP_FIRST_IS_CONTRECORD,
		1, start_of_next_page($end_lsn),
		123456));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid contrecord length 123456 .* at .*", $log_size),
	"xlp_rem_len bad");


###########################################################################
note "Multi-page, but header is split, so page checks are done first";
###########################################################################

# xl_prev is bad and xl_tot_len is too big, but we'll check xlp_magic first.
emit_message($node, 0);
$end_lsn = advance_to_record_splitting_zone($node);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid magic number 0000 .* LSN .*", $log_size),
	"xlp_magic zero (split record header)");

# And we'll also check xlp_pageaddr before any header checks.
emit_message($node, 0);
$end_lsn = advance_to_record_splitting_zone($node);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
write_wal(
	$node, $TLI,
	start_of_next_page($end_lsn),
	build_page_header(
		$XLP_PAGE_MAGIC, $XLP_FIRST_IS_CONTRECORD, 1, 0xbaaaaaad));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"unexpected pageaddr 0/BAAAAAAD in .*, LSN .*,", $log_size),
	"xlp_pageaddr bad (split record header)");

# We'll also discover that xlp_rem_len doesn't add up before any
# header checks,
emit_message($node, 0);
$end_lsn = advance_to_record_splitting_zone($node);
$node->stop('immediate');
write_wal($node, $TLI, $end_lsn,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
write_wal(
	$node, $TLI,
	start_of_next_page($end_lsn),
	build_page_header(
		$XLP_PAGE_MAGIC, $XLP_FIRST_IS_CONTRECORD,
		1, start_of_next_page($end_lsn),
		123456));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid contrecord length 123456 .* at .*", $log_size),
	"xlp_rem_len bad (split record header)");

done_testing();
