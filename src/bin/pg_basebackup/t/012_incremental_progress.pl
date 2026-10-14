# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that incremental backup size estimates account for files sent in
# incremental format, rather than counting every file at its full size.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'summarize_wal = on');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# Put unchanged data in a separate tablespace to exercise size estimation
# for both PGDATA and user-defined tablespaces.
$node->safe_psql('postgres', <<EOM);
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE inplace LOCATION '';
CREATE TABLE unchanged TABLESPACE inplace AS
  SELECT g AS i, repeat('x', 500) AS pad FROM generate_series(1, 20000) g;
CREATE TABLE changed AS SELECT 1 AS i;
EOM

# Run a backup with --progress and return the streamed and total amounts
# from the final progress report, in kB.
sub backup_with_progress
{
	my ($path, @extra) = @_;
	my ($stdout, $stderr) = ('', '');

	my $result = run_log(
		[
			'pg_basebackup',
			'--host' => $node->host,
			'--port' => $node->port,
			'--pgdata' => $path,
			'--format' => 'tar',
			'--wal-method' => 'none',
			'--no-sync',
			'--progress',
			'--checkpoint' => 'fast',
			@extra
		],
		'>' => \$stdout,
		'2>' => \$stderr);
	ok($result, "backup into $path succeeded")
	  or die "pg_basebackup into $path failed: $stderr";

	# Use the last progress line, allowing either newline or carriage return.
	my @lines = grep { /kB/ } split(/[\r\n]+/, $stderr);
	my $line = $lines[-1] // '';
	my ($streamed, $total) = $line =~ m{^\s*(\d+)/(\d+) kB\b};
	ok(defined($total), "progress was reported for $path")
	  or die "could not parse progress output for $path: $stderr";
	note "backup into $path streamed $streamed of $total kB";
	return ($streamed, $total);
}

my $backup_dir = $node->backup_dir;
my (undef, $full_total) = backup_with_progress("$backup_dir/full");

# Make a small change before taking the incremental backup.
$node->safe_psql('postgres',
	'INSERT INTO changed SELECT g FROM generate_series(1, 1000) g');

my ($incr_streamed, $incr_total) = backup_with_progress("$backup_dir/incr",
	'--incremental' => "$backup_dir/full/backup_manifest");

# The incremental backup estimate should be much smaller than the full one.
cmp_ok($incr_total, '<', $full_total / 2,
	'incremental backup estimate is much smaller than a full backup estimate'
);

# Check for a substantial overestimate. pg_basebackup raises the displayed
# total when the amount streamed exceeds the original estimate.
cmp_ok(
	$incr_total, '<',
	$incr_streamed * 2,
	'incremental backup estimate is not substantially overestimated');

done_testing();
