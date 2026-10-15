# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a crashed updating transaction that is part of a multixid is treated
# as aborted.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->start;

$node->safe_psql(
	'postgres', q(
    CREATE TABLE mx_crashed (id int PRIMARY KEY, payload text);
    INSERT INTO mx_crashed VALUES (1, 'initial');
));

# Keep the updater open while another transaction locks the same row.  This
# creates a multixid with the updating and locking xids as members.
my $updater = $node->background_psql('postgres');
$updater->query_safe('BEGIN');
$updater->query_safe(
	q(UPDATE mx_crashed SET payload = 'crashed' WHERE id = 1));

$node->safe_psql(
	'postgres', q(
    BEGIN;
    SELECT id FROM mx_crashed WHERE id = 1 FOR KEY SHARE;
    PREPARE TRANSACTION 'mx_crashed_locker';
));

# The PREPARE TRANSACTION flushed the preceding heap update and multixact WAL
# records, too.  Kill the server to leave the updating transaction as crashed,
# ie. it is marked neither as committed nor aborted in pg_xact.
$node->stop('immediate');
$updater->{run}->finish;
$node->start;

# Update the row again after restart. The crashed updater should be ignored, but
# the FOR KEY SHARE lock should remain.
my ($stdout, $stderr);
my $ret = $node->psql(
	'postgres', q(
    BEGIN ISOLATION LEVEL REPEATABLE READ;
    UPDATE mx_crashed SET payload = 'after crash' WHERE id = 1;
    COMMIT;
),
	stdout => \$stdout,
	stderr => \$stderr);
is($ret, 0, 'repeatable read update ignores the crashed updater')
  or diag($stderr);

is( $node->safe_psql(
		'postgres', q(SELECT payload FROM mx_crashed WHERE id = 1)),
	'after crash',
	'row was updated');

# Check that the row remains locked for the locking transaction
$node->psql(
	'postgres',
	'SELECT * FROM mx_crashed FOR UPDATE NOWAIT',
	stderr => \$stderr);
like(
	$stderr,
	qr/could not obtain lock on row in relation/,
	"prepared share locker survives after crash");

$node->stop();
done_testing();
