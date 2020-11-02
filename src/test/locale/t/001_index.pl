use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;
use Test::More;

if ($ENV{with_icu} eq 'yes')
{
	plan tests => 10;
}
else
{
	plan skip_all => 'ICU not supported by this build';
}

#### Set up the server

note "setting up data directory";
my $node = get_new_node('main');
$node->init(extra => [ '--encoding=UTF8' ]);

$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

sub test_index
{
	my ($err_like, $err_comm) = @_;
	my ($ret, $out, $err) = $node->psql('postgres', "SELECT * FROM icu1");
	is($ret, 0, 'SELECT should succeed.');
	like($err, $err_like, $err_comm);
}

$node->safe_psql('postgres', 'CREATE TABLE icu1(val text);');
$node->safe_psql('postgres', 'CREATE INDEX icu1_fr ON icu1 (val COLLATE "fr-x-icu");');

test_index(qr/^$/, 'No warning should be raised');

# Simulate different collation version
$node->safe_psql('postgres',
	"UPDATE pg_depend SET refobjversion = 'not_a_version'"
	. " WHERE refobjversion IS NOT NULL"
	. " AND objid::regclass::text = 'icu1_fr';");

test_index(qr/index "icu1_fr" depends on collation "fr-x-icu" version "not_a_version", but the current version is/,
	'Different collation version warning should be raised.');

$node->safe_psql('postgres', 'ALTER INDEX icu1_fr ALTER COLLATION "fr-x-icu" REFRESH VERSION;');

test_index(qr/^$/, 'No warning should be raised');

# Simulate different collation version
$node->safe_psql('postgres',
	"UPDATE pg_depend SET refobjversion = 'not_a_version'"
	. " WHERE refobjversion IS NOT NULL"
	. " AND objid::regclass::text = 'icu1_fr';");

test_index(qr/index "icu1_fr" depends on collation "fr-x-icu" version "not_a_version", but the current version is/,
	'Different collation version warning should be raised.');

$node->safe_psql('postgres', 'REINDEX TABLE icu1;');

test_index(qr/^$/, 'No warning should be raised');

$node->stop;
