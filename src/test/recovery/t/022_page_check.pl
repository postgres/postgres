# Emulate on-disk corruptions of relation pages and find such corruptions
# using pg_relation_check_pages().

use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 20;

our $CHECKSUM_UINT16_OFFSET = 4;
our $PD_UPPER_UINT16_OFFSET = 7;
our $BLOCKSIZE;
our $TOTAL_NB_ERR = 0;

# Grab a relation page worth a size of BLOCKSIZE from given $filename.
# $blkno is the same block number as for a relation file.
sub read_page
{
	my ($filename, $blkno) = @_;
	my $block;

	open(my $infile, '<', $filename) or die;
	binmode($infile);

	my $success = read($infile, $block, $BLOCKSIZE, ($blkno * $BLOCKSIZE));
	die($!) if !defined($success);

	close($infile);

	return ($block);
}

# Update an existing page of size BLOCKSIZE with new contents in given
# $filename.  $blkno is the block number assigned in the relation file.
sub write_page
{
	my ($filename, $block, $blkno) = @_;

	open(my $outfile, '>', $filename) or die;
	binmode($outfile);

	my $nb = syswrite($outfile, $block, $BLOCKSIZE, ($blkno * $BLOCKSIZE));

	die($!) if not defined $nb;
	die("Write error") if ($nb != $BLOCKSIZE);

	$outfile->flush();

	close($outfile);
	return;
}

# Read 2 bytes from relation page at a given offset.
sub get_uint16_from_page
{
	my ($block, $offset) = @_;

	return (unpack("S*", $block))[$offset];
}

# Write 2 bytes to relation page at a given offset.
sub set_uint16_to_page
{
	my ($block, $data, $offset) = @_;

	my $pack = pack("S", $data);

	# vec with 16B or more won't preserve endianness.
	vec($block, 2 * $offset, 8) = (unpack('C*', $pack))[0];
	vec($block, (2 * $offset) + 1, 8) = (unpack('C*', $pack))[1];

	return $block;
}

# Sanity check on pg_stat_database looking after the number of checksum
# failures.
sub check_pg_stat_database
{
	my ($node, $test_prefix) = @_;

	my $stdout = $node->safe_psql('postgres',
		    "SELECT "
		  . " sum(checksum_failures)"
		  . " FROM pg_catalog.pg_stat_database");
	is($stdout, $TOTAL_NB_ERR,
		"$test_prefix: pg_stat_database should have $TOTAL_NB_ERR error");

	return;
}

# Run a round of page checks for any relation present in this test run.
# $expected_broken is the psql output marking all the pages found as
# corrupted using relname|blkno as format for each tuple returned.  $nb
# is the new number of checksum errors added to the global counter
# matched with the contents of pg_stat_database.
#
# Note that this has no need to check system relations as these would have
# no corruptions: this test does not manipulate them and should by no mean
# break the cluster.
sub run_page_checks
{
	my ($node, $num_checksum, $expected_broken, $test_prefix) = @_;

	my $stdout = $node->safe_psql('postgres',
		    "SELECT relname, failed_block_num"
		  . " FROM (SELECT relname, (pg_catalog.pg_relation_check_pages(oid)).*"
		  . "   FROM pg_class "
		  . "   WHERE relkind in ('r','i', 'm') AND oid >= 16384) AS s");

	# Check command result
	is($stdout, $expected_broken,
		"$test_prefix: output mismatch with pg_relation_check_pages()");

	$TOTAL_NB_ERR += $num_checksum;
	return;
}

# Perform various tests that modify a specified block at the given
# offset, checking that a page corruption is correctly detected.  The
# original contents of the page are restored back once done.
# $broken_pages is the set of pages that are expected to be broken
# as of the returned result of pg_relation_check_pages().  $num_checksum
# is the number of checksum failures expected to be added to the contents
# of pg_stat_database after this function is done.
sub corrupt_and_test_block
{
	my ($node, $filename, $blkno, $offset, $broken_pages, $num_checksum,
		$test_prefix)
	  = @_;
	my $fake_data = hex '0x0000';

	# Stop the server cleanly to flush any pages, and to prevent any
	# concurrent updates on what is going to be updated.
	$node->stop;
	my $original_block = read_page($filename, 0);
	my $original_data = get_uint16_from_page($original_block, $offset);

	isnt($original_data, $fake_data,
		"$test_prefix: fake data at offset $offset should be different from the existing one"
	);

	my $new_block = set_uint16_to_page($original_block, $fake_data, $offset);
	isnt(
		$original_data,
		get_uint16_from_page($new_block, $offset),
		"$test_prefix: The data at offset $offset should have been changed in memory"
	);

	write_page($filename, $new_block, 0);

	my $written_data = get_uint16_from_page(read_page($filename, 0), $offset);

	# Some offline checks to validate that the corrupted data is in place.
	isnt($original_data, $written_data,
		"$test_prefix: data written at offset $offset should be different from the original one"
	);
	is( get_uint16_from_page($new_block, $offset),
		$written_data,
		"$test_prefix: data written at offset $offset should be the same as the one in memory"
	);
	is($written_data, $fake_data,
		"$test_prefix: The data written at offset $offset should be the one we wanted to write"
	);

	# The corruption is in place, start the server to run the checks.
	$node->start;
	run_page_checks($node, $num_checksum, $broken_pages, $test_prefix);

	# Stop the server, put the original page back in place.
	$node->stop;

	$new_block = set_uint16_to_page($original_block, $original_data, $offset);
	is( $original_data,
		get_uint16_from_page($new_block, $offset),
		"$test_prefix: data at offset $offset should have been restored in memory"
	);

	write_page($filename, $new_block, 0);
	is( $original_data,
		get_uint16_from_page(read_page($filename, $blkno), $offset),
		"$test_prefix: data at offset $offset should have been restored on disk"
	);

	# There should be no errors now that the contents are back in place.
	$node->start;
	run_page_checks($node, 0, '', $test_prefix);
}

# Data checksums are necessary for this test.
my $node = get_new_node('main');
$node->init(extra => ['--data-checksums']);
$node->start;

my $stdout =
  $node->safe_psql('postgres', "SELECT" . " current_setting('block_size')");

$BLOCKSIZE = $stdout;

# Basic schema to corrupt and check
$node->safe_psql(
	'postgres', q|
	CREATE TABLE public.t1(id integer);
	INSERT INTO public.t1 SELECT generate_series(1, 100);
	CHECKPOINT;
|);

# Get the path to the relation file that will get manipulated by the
# follow-up tests with some on-disk corruptions.
$stdout = $node->safe_psql('postgres',
	    "SELECT"
	  . " current_setting('data_directory') || '/' || pg_relation_filepath('t1')"
);

my $filename = $stdout;

# Normal case without corruptions, this passes, with pg_stat_database
# reporting no errors.
check_pg_stat_database($node, 'start');

# Test with a modified checksum.
corrupt_and_test_block($node, $filename, 0, $CHECKSUM_UINT16_OFFSET, 't1|0',
	1, 'broken checksum');

# Test corruption making the block looking like it validates PageIsNew().
corrupt_and_test_block($node, $filename, 0, $PD_UPPER_UINT16_OFFSET, 't1|0',
	0, 'new page');

# Check that the number of errors in pg_stat_database match what we
# expect with the corruptions previously introduced.
check_pg_stat_database($node, 'end');
