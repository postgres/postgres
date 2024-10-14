
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node;

#
# Test set-up
#
$node = PostgreSQL::Test::Cluster->new('test');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));

#
# Check a table with data loaded but no corruption, freezing, etc.
#
fresh_test_table('test');
check_all_options_uncorrupted('test', 'plain');

#
# Check a corrupt table
#
fresh_test_table('test');
corrupt_first_page('test');
detects_heap_corruption("verify_heapam('test')", "plain corrupted table");
detects_heap_corruption(
	"verify_heapam('test', skip := 'all-visible')",
	"plain corrupted table skipping all-visible");
detects_heap_corruption(
	"verify_heapam('test', skip := 'all-frozen')",
	"plain corrupted table skipping all-frozen");
detects_heap_corruption(
	"verify_heapam('test', check_toast := false)",
	"plain corrupted table skipping toast");
detects_heap_corruption(
	"verify_heapam('test', startblock := 0, endblock := 0)",
	"plain corrupted table checking only block zero");

#
# Check a corrupt table with all-frozen data
#
fresh_test_table('test');
$node->safe_psql('postgres', q(VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) test));
detects_no_corruption("verify_heapam('test')",
	"all-frozen not corrupted table");
corrupt_first_page('test');
detects_heap_corruption("verify_heapam('test')",
	"all-frozen corrupted table");
detects_no_corruption(
	"verify_heapam('test', skip := 'all-frozen')",
	"all-frozen corrupted table skipping all-frozen");

#
# Check a sequence with no corruption.  The current implementation of sequences
# doesn't require its own test setup, since sequences are really just heap
# tables under-the-hood.  To guard against future implementation changes made
# without remembering to update verify_heapam, we create and exercise a
# sequence, checking along the way that it passes corruption checks.
#
fresh_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');
advance_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');
set_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');
reset_test_sequence('test_seq');
check_all_options_uncorrupted('test_seq', 'plain');

# Returns the filesystem path for the named relation.
sub relation_filepath
{
	my ($relname) = @_;

	my $pgdata = $node->data_dir;
	my $rel = $node->safe_psql('postgres',
		qq(SELECT pg_relation_filepath('$relname')));
	die "path not found for relation $relname" unless defined $rel;
	return "$pgdata/$rel";
}

# (Re)create and populate a test table of the given name.
sub fresh_test_table
{
	my ($relname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		DROP TABLE IF EXISTS $relname CASCADE;
		CREATE TABLE $relname (a integer, b text);
		ALTER TABLE $relname SET (autovacuum_enabled=false);
		ALTER TABLE $relname ALTER b SET STORAGE external;
		INSERT INTO $relname (a, b)
			(SELECT gs, repeat('b',gs*10) FROM generate_series(1,1000) gs);
		BEGIN;
		SAVEPOINT s1;
		SELECT 1 FROM $relname WHERE a = 42 FOR UPDATE;
		UPDATE $relname SET b = b WHERE a = 42;
		RELEASE s1;
		SAVEPOINT s1;
		SELECT 1 FROM $relname WHERE a = 42 FOR UPDATE;
		UPDATE $relname SET b = b WHERE a = 42;
		COMMIT;
	));
}

# Create a test sequence of the given name.
sub fresh_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		DROP SEQUENCE IF EXISTS $seqname CASCADE;
		CREATE SEQUENCE $seqname
			INCREMENT BY 13
			MINVALUE 17
			START WITH 23;
		SELECT nextval('$seqname');
		SELECT setval('$seqname', currval('$seqname') + nextval('$seqname'));
	));
}

# Call SQL functions to increment the sequence
sub advance_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		SELECT nextval('$seqname');
	));
}

# Call SQL functions to set the sequence
sub set_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		SELECT setval('$seqname', 102);
	));
}

# Call SQL functions to reset the sequence
sub reset_test_sequence
{
	my ($seqname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		ALTER SEQUENCE $seqname RESTART WITH 51
	));
}

# Stops the test node, corrupts the first page of the named relation, and
# restarts the node.
sub corrupt_first_page
{
	my ($relname) = @_;
	my $relpath = relation_filepath($relname);

	$node->stop;

	my $fh;
	open($fh, '+<', $relpath)
	  or BAIL_OUT("open failed: $!");
	binmode $fh;

	# Corrupt some line pointers.  The values are chosen to hit the
	# various line-pointer-corruption checks in verify_heapam.c
	# on both little-endian and big-endian architectures.
	sysseek($fh, 32, 0)
	  or BAIL_OUT("sysseek failed: $!");
	syswrite(
		$fh,
		pack("L*",
			0xAAA15550, 0xAAA0D550, 0x00010000,
			0x00008000, 0x0000800F, 0x001e8000)
	) or BAIL_OUT("syswrite failed: $!");
	close($fh)
	  or BAIL_OUT("close failed: $!");

	$node->start;
}

sub detects_heap_corruption
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($function, $testname) = @_;

	detects_corruption(
		$function,
		$testname,
		qr/line pointer redirection to item at offset \d+ precedes minimum offset \d+/,
		qr/line pointer redirection to item at offset \d+ exceeds maximum offset \d+/,
		qr/line pointer to page offset \d+ is not maximally aligned/,
		qr/line pointer length \d+ is less than the minimum tuple header size \d+/,
		qr/line pointer to page offset \d+ with length \d+ ends beyond maximum page offset \d+/,
	);
}

sub detects_corruption
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($function, $testname, @re) = @_;

	my $result = $node->safe_psql('postgres', qq(SELECT * FROM $function));
	like($result, $_, $testname) for (@re);
}

sub detects_no_corruption
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($function, $testname) = @_;

	my $result = $node->safe_psql('postgres', qq(SELECT * FROM $function));
	is($result, '', $testname);
}

# Check various options are stable (don't abort) and do not report corruption
# when running verify_heapam on an uncorrupted test table.
#
# The relname *must* be an uncorrupted table, or this will fail.
#
# The prefix is used to identify the test, along with the options,
# and should be unique.
sub check_all_options_uncorrupted
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($relname, $prefix) = @_;

	for my $stop (qw(true false))
	{
		for my $check_toast (qw(true false))
		{
			for my $skip ("'none'", "'all-frozen'", "'all-visible'")
			{
				for my $startblock (qw(NULL 0))
				{
					for my $endblock (qw(NULL 0))
					{
						my $opts =
							"on_error_stop := $stop, "
						  . "check_toast := $check_toast, "
						  . "skip := $skip, "
						  . "startblock := $startblock, "
						  . "endblock := $endblock";

						detects_no_corruption(
							"verify_heapam('$relname', $opts)",
							"$prefix: $opts");
					}
				}
			}
		}
	}
}

done_testing();
