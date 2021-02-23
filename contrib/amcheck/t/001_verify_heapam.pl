use strict;
use warnings;

use PostgresNode;
use TestLib;

use Test::More tests => 80;

my ($node, $result);

#
# Test set-up
#
$node = get_new_node('test');
$node->init;
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
detects_no_corruption(
	"verify_heapam('test')",
	"all-frozen not corrupted table");
corrupt_first_page('test');
detects_heap_corruption("verify_heapam('test')",
	"all-frozen corrupted table");
detects_no_corruption(
	"verify_heapam('test', skip := 'all-frozen')",
	"all-frozen corrupted table skipping all-frozen");

# Returns the filesystem path for the named relation.
sub relation_filepath
{
	my ($relname) = @_;

	my $pgdata = $node->data_dir;
	my $rel    = $node->safe_psql('postgres',
		qq(SELECT pg_relation_filepath('$relname')));
	die "path not found for relation $relname" unless defined $rel;
	return "$pgdata/$rel";
}

# Returns the fully qualified name of the toast table for the named relation
sub get_toast_for
{
	my ($relname) = @_;

	return $node->safe_psql(
		'postgres', qq(
		SELECT 'pg_toast.' || t.relname
			FROM pg_catalog.pg_class c, pg_catalog.pg_class t
			WHERE c.relname = '$relname'
			  AND c.reltoastrelid = t.oid));
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
	seek($fh, 32, 0)
	  or BAIL_OUT("seek failed: $!");
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
	my ($function, $testname, @re) = @_;

	my $result = $node->safe_psql('postgres', qq(SELECT * FROM $function));
	like($result, $_, $testname) for (@re);
}

sub detects_no_corruption
{
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
