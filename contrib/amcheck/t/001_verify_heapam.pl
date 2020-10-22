use strict;
use warnings;

use PostgresNode;
use TestLib;

use Test::More tests => 65;

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
detects_corruption(
	"verify_heapam('test')",
	"plain corrupted table");
detects_corruption(
	"verify_heapam('test', skip := 'all-visible')",
	"plain corrupted table skipping all-visible");
detects_corruption(
	"verify_heapam('test', skip := 'all-frozen')",
	"plain corrupted table skipping all-frozen");
detects_corruption(
	"verify_heapam('test', check_toast := false)",
	"plain corrupted table skipping toast");
detects_corruption(
	"verify_heapam('test', startblock := 0, endblock := 0)",
	"plain corrupted table checking only block zero");

#
# Check a corrupt table with all-frozen data
#
fresh_test_table('test');
$node->safe_psql('postgres', q(VACUUM FREEZE test));
corrupt_first_page('test');
detects_corruption(
	"verify_heapam('test')",
	"all-frozen corrupted table");
detects_no_corruption(
	"verify_heapam('test', skip := 'all-frozen')",
	"all-frozen corrupted table skipping all-frozen");

#
# Check a corrupt table with corrupt page header
#
fresh_test_table('test');
corrupt_first_page_and_header('test');
detects_corruption(
	"verify_heapam('test')",
	"corrupted test table with bad page header");

#
# Check an uncorrupted table with corrupt toast page header
#
fresh_test_table('test');
my $toast = get_toast_for('test');
corrupt_first_page_and_header($toast);
detects_corruption(
	"verify_heapam('test', check_toast := true)",
	"table with corrupted toast page header checking toast");
detects_no_corruption(
	"verify_heapam('test', check_toast := false)",
	"table with corrupted toast page header skipping toast");
detects_corruption(
	"verify_heapam('$toast')",
	"corrupted toast page header");

#
# Check an uncorrupted table with corrupt toast
#
fresh_test_table('test');
$toast = get_toast_for('test');
corrupt_first_page($toast);
detects_corruption(
	"verify_heapam('test', check_toast := true)",
	"table with corrupted toast checking toast");
detects_no_corruption(
	"verify_heapam('test', check_toast := false)",
	"table with corrupted toast skipping toast");
detects_corruption(
	"verify_heapam('$toast')",
	"corrupted toast table");

#
# Check an uncorrupted all-frozen table with corrupt toast
#
fresh_test_table('test');
$node->safe_psql('postgres', q(VACUUM FREEZE test));
$toast = get_toast_for('test');
corrupt_first_page($toast);
detects_corruption(
	"verify_heapam('test', check_toast := true)",
	"all-frozen table with corrupted toast checking toast");
detects_no_corruption(
	"verify_heapam('test', check_toast := false)",
	"all-frozen table with corrupted toast skipping toast");
detects_corruption(
	"verify_heapam('$toast')",
	"corrupted toast table of all-frozen table");

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

# Returns the fully qualified name of the toast table for the named relation
sub get_toast_for
{
	my ($relname) = @_;
	$node->safe_psql('postgres', qq(
		SELECT 'pg_toast.' || t.relname
			FROM pg_catalog.pg_class c, pg_catalog.pg_class t
			WHERE c.relname = '$relname'
			  AND c.reltoastrelid = t.oid));
}

# (Re)create and populate a test table of the given name.
sub fresh_test_table
{
	my ($relname) = @_;
	$node->safe_psql('postgres', qq(
		DROP TABLE IF EXISTS $relname CASCADE;
		CREATE TABLE $relname (a integer, b text);
		ALTER TABLE $relname SET (autovacuum_enabled=false);
		ALTER TABLE $relname ALTER b SET STORAGE external;
		INSERT INTO $relname (a, b)
			(SELECT gs, repeat('b',gs*10) FROM generate_series(1,1000) gs);
	));
}

# Stops the test node, corrupts the first page of the named relation, and
# restarts the node.
sub corrupt_first_page_internal
{
	my ($relname, $corrupt_header) = @_;
	my $relpath = relation_filepath($relname);

	$node->stop;
	my $fh;
	open($fh, '+<', $relpath);
	binmode $fh;

	# If we corrupt the header, postgres won't allow the page into the buffer.
	syswrite($fh, '\xFF\xFF\xFF\xFF', 8) if ($corrupt_header);

	# Corrupt at least the line pointers.  Exactly what this corrupts will
	# depend on the page, as it may run past the line pointers into the user
	# data.  We stop short of writing 2048 bytes (2k), the smallest supported
	# page size, as we don't want to corrupt the next page.
	seek($fh, 32, 0);
	syswrite($fh, '\x77\x77\x77\x77', 500);
	close($fh);
	$node->start;
}

sub corrupt_first_page
{
	corrupt_first_page_internal($_[0], undef);
}

sub corrupt_first_page_and_header
{
	corrupt_first_page_internal($_[0], 1);
}

sub detects_corruption
{
	my ($function, $testname) = @_;

	my $result = $node->safe_psql('postgres',
		qq(SELECT COUNT(*) > 0 FROM $function));
	is($result, 't', $testname);
}

sub detects_no_corruption
{
	my ($function, $testname) = @_;

	my $result = $node->safe_psql('postgres',
		qq(SELECT COUNT(*) = 0 FROM $function));
	is($result, 't', $testname);
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
						my $opts = "on_error_stop := $stop, " .
								   "check_toast := $check_toast, " .
								   "skip := $skip, " .
								   "startblock := $startblock, " .
								   "endblock := $endblock";

						detects_no_corruption(
							"verify_heapam('$relname', $opts)",
							"$prefix: $opts");
					}
				}
			}
		}
	}
}
