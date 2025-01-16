# Copyright (c) 2023-2025, PostgreSQL Global Development Group
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
$node->emit_wal(0);
$end_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$node->stop('immediate');
my $log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid record length at .*: expected at least 24, got 0", $log_size
	),
	"xl_tot_len zero");

# xl_tot_len is < 24 (presumably recycled garbage).
$node->emit_wal(0);
$end_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE, build_record_header(23));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid record length at .*: expected at least 24, got 23",
		$log_size),
	"xl_tot_len short");

# xl_tot_len in final position, not big enough to span into a new page but
# also not eligible for regular record header validation
$node->emit_wal(0);
$end_lsn = $node->advance_wal_to_record_splitting_zone($WAL_BLOCK_SIZE);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE, build_record_header(1));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"invalid record length at .*: expected at least 24, got 1", $log_size
	),
	"xl_tot_len short at end-of-page");

# Need more pages, but xl_prev check fails first.
$node->emit_wal(0);
$end_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"record with incorrect prev-link 0/DEADBEEF at .*", $log_size),
	"xl_prev bad");

# xl_crc check fails.
$node->emit_wal(0);
$node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$end_lsn = $node->emit_wal(10);
$node->stop('immediate');
# Corrupt a byte in that record, breaking its CRC.
$node->write_wal($TLI, $end_lsn - 8, $WAL_SEGMENT_SIZE, '!');
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
$node->emit_wal(0);
$prev_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$end_lsn = $node->emit_wal(0);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 0, $prev_lsn));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid magic number 0000 .* LSN .*", $log_size),
	"xlp_magic zero");

# Good xl_prev, we hit garbage page next (bad magic).
$node->emit_wal(0);
$prev_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$end_lsn = $node->emit_wal(0);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 0, $prev_lsn));
$node->write_wal($TLI, start_of_next_page($end_lsn),
	$WAL_SEGMENT_SIZE, build_page_header(0xcafe, 0, 1, 0));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid magic number CAFE .* LSN .*", $log_size),
	"xlp_magic bad");

# Good xl_prev, we hit typical recycled page (good xlp_magic, bad
# xlp_pageaddr).
$node->emit_wal(0);
$prev_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$end_lsn = $node->emit_wal(0);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 0, $prev_lsn));
$node->write_wal($TLI, start_of_next_page($end_lsn),
	$WAL_SEGMENT_SIZE, build_page_header($XLP_PAGE_MAGIC, 0, 1, 0xbaaaaaad));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"unexpected pageaddr 0/BAAAAAAD in .*, LSN .*,", $log_size),
	"xlp_pageaddr bad");

# Good xl_prev, xlp_magic, xlp_pageaddr, but bogus xlp_info.
$node->emit_wal(0);
$prev_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$end_lsn = $node->emit_wal(0);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 42, $prev_lsn));
$node->write_wal(
	$TLI,
	start_of_next_page($end_lsn),
	$WAL_SEGMENT_SIZE,
	build_page_header(
		$XLP_PAGE_MAGIC, 0x1234, 1, start_of_next_page($end_lsn)));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid info bits 1234 in .*, LSN .*,", $log_size),
	"xlp_info bad");

# Good xl_prev, xlp_magic, xlp_pageaddr, but xlp_info doesn't mention
# continuation record.
$node->emit_wal(0);
$prev_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$end_lsn = $node->emit_wal(0);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 42, $prev_lsn));
$node->write_wal($TLI, start_of_next_page($end_lsn),
	$WAL_SEGMENT_SIZE,
	build_page_header($XLP_PAGE_MAGIC, 0, 1, start_of_next_page($end_lsn)));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("there is no contrecord flag at .*", $log_size),
	"xlp_info lacks XLP_FIRST_IS_CONTRECORD");

# Good xl_prev, xlp_magic, xlp_pageaddr, xlp_info but xlp_rem_len doesn't add
# up.
$node->emit_wal(0);
$prev_lsn = $node->advance_wal_out_of_record_splitting_zone($WAL_BLOCK_SIZE);
$end_lsn = $node->emit_wal(0);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 42, $prev_lsn));
$node->write_wal(
	$TLI,
	start_of_next_page($end_lsn),
	$WAL_SEGMENT_SIZE,
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
$node->emit_wal(0);
$end_lsn = $node->advance_wal_to_record_splitting_zone($WAL_BLOCK_SIZE);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
$log_size = -s $node->logfile;
$node->start;
ok($node->log_contains("invalid magic number 0000 .* LSN .*", $log_size),
	"xlp_magic zero (split record header)");

# And we'll also check xlp_pageaddr before any header checks.
$node->emit_wal(0);
$end_lsn = $node->advance_wal_to_record_splitting_zone($WAL_BLOCK_SIZE);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
$node->write_wal(
	$TLI,
	start_of_next_page($end_lsn),
	$WAL_SEGMENT_SIZE,
	build_page_header(
		$XLP_PAGE_MAGIC, $XLP_FIRST_IS_CONTRECORD, 1, 0xbaaaaaad));
$log_size = -s $node->logfile;
$node->start;
ok( $node->log_contains(
		"unexpected pageaddr 0/BAAAAAAD in .*, LSN .*,", $log_size),
	"xlp_pageaddr bad (split record header)");

# We'll also discover that xlp_rem_len doesn't add up before any
# header checks,
$node->emit_wal(0);
$end_lsn = $node->advance_wal_to_record_splitting_zone($WAL_BLOCK_SIZE);
$node->stop('immediate');
$node->write_wal($TLI, $end_lsn, $WAL_SEGMENT_SIZE,
	build_record_header(2 * 1024 * 1024 * 1024, 0, 0xdeadbeef));
$node->write_wal(
	$TLI,
	start_of_next_page($end_lsn),
	$WAL_SEGMENT_SIZE,
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
