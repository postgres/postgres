# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test dump+restore round trips that don't fit in 002_pg_upgrade.pl, e.g.,
# options too minor to justify testing on a full regression database.
use strict;
use warnings FATAL => 'all';

use Carp;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
my $backupdir = $node->backup_dir;

$node->safe_psql(
	'postgres', q{
	CREATE DATABASE regression_utf8
		TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C'});

# Verify that pg_restore can restore a small database from each dump format,
# with or without each option that has format-specific code.  Currently,
# --inserts is the option having format-specific code.  (Search pg_backup*.c
# for "--inserts".)  Test more --inserts variants while here.
my $setup = <<'EOSQL';
CREATE ROLE pubowner;
ALTER SCHEMA public OWNER TO pubowner;
CREATE TABLE t (
	drop int,
	c0 bool,
	pk int GENERATED ALWAYS AS IDENTITY,
	c1 numeric,
	gc0 int GENERATED ALWAYS AS (7) STORED,
	c2 varbit,
	c3 text
);
INSERT INTO t (c0, c1, c2, c3) VALUES
	('t',  -9e2, '101', E'\n'),
	-- in SJIS, last byte of the multibyte character is a backslash
	('f', 'NaN', '010', U&'\30BDn'),
	('t', 12e-1,  NULL, '\N');
ALTER TABLE t DROP COLUMN drop;
CREATE STATISTICS t_stats ON c1, c3 FROM t;

CREATE TABLE generated_only (c int GENERATED ALWAYS AS (7) STORED);
INSERT INTO generated_only DEFAULT VALUES;

-- Meet getPartitioningInfo() "unsafe" condition, but use just one partition
-- so dump output is stable.
CREATE TYPE pp_colors AS
	ENUM ('green', 'blue', 'black');
CREATE TABLE pp_enumpart (a pp_colors)
	PARTITION BY HASH (a);
CREATE TABLE pp_enumpart1 PARTITION OF pp_enumpart
	FOR VALUES WITH (MODULUS 1, REMAINDER 0);
INSERT INTO pp_enumpart VALUES ('green'), ('blue'), ('black');

-- exercise mechanism that pg_dump calls "dummy_view", using approach from
-- create_index.sql
CREATE TABLE view_base_table (key int PRIMARY KEY, data varchar(20));
CREATE VIEW key_dependent_view AS
	SELECT * FROM view_base_table GROUP BY key;

-- Exercise pg_init_privs treatment of pg_language outside an extension.  The
-- ALTER would be silly in a non-test database.
ALTER EXTENSION plpgsql DROP LANGUAGE plpgsql;
REVOKE USAGE ON LANGUAGE plpgsql FROM public;
EOSQL
# restore runs an identity sequence setval that prints a "3"
my $want_psql_stdout = qr/\n+3/;
$node->safe_psql('regression_utf8', $setup);
my @fmt_cases = (
	{ fmt => 'custom' },
	{ fmt => 'directory' },
	{ fmt => 'tar' },
	{
		fmt => 'custom',
		extra_dump => [qw(--column-inserts --rows-per-insert=2)],
		extra_restore => ['-j2']
	},
	{
		fmt => 'directory',
		extra_dump =>
		  [qw(--inserts --on-conflict-do-nothing --rows-per-insert=2)],
		extra_restore => ['-j2']
	},
	{ fmt => 'tar', extra_dump => [qw(--encoding=SJIS --inserts)] });
my $want_dump_sql;
foreach my $case (@fmt_cases)
{
	my $fmt = $case->{fmt};
	my $extra_dump = $case->{extra_dump} || [];
	my $extra_restore = $case->{extra_restore} || [];
	my $dump_file = join('', $backupdir, '/', $fmt, @$extra_dump, '-dump');
	my $space_dextra = $extra_dump ? join(' ', '', @$extra_dump) : '';
	my $space_rextra = $extra_restore ? join(' ', '', @$extra_restore) : '';

	# dump
	$node->command_ok(
		[
			'pg_dump',
			'--format' => $fmt,
			@$extra_dump,
			'--no-sync',
			'--file' => $dump_file,
			$node->connstr('regression_utf8'),
		],
		"dump$space_dextra in format $fmt");

	# pg_restore to plain format $sql_from_pg_restore
	my ($stdout, $stderr);
	my $result = run_log [
		'pg_restore',
		@$extra_restore,
		'--clean',
		'--restrict-key' => 'test',
		'--file' => '-',
		$dump_file,
	  ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
	ok($result,
		"pg_restore$space_rextra $fmt$space_dextra => plain: succeeds");
	isnt($stdout, '',
		"pg_restore$space_rextra $fmt$space_dextra => plain: has stdout");
	is($stderr, '',
		"pg_restore$space_rextra $fmt$space_dextra => plain: no stderr");
	my $sql_from_pg_restore = $stdout;
	# first time through, save for comparison
	$want_dump_sql = $sql_from_pg_restore unless $want_dump_sql;

	# direct pg_restore to database
	$node->command_ok(
		[
			'pg_restore', @$extra_restore,
			'--verbose', '--clean',
			'--dbname' => $node->connstr('regression_utf8'),
			$dump_file,
		],
		"restore$space_rextra from $fmt$space_dextra");

	# compare round trip
	$result = run_log [
		'pg_dump', '--clean',
		'--restrict-key' => 'test',
		$node->connstr('regression_utf8')
	  ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
	ok($result,
		"comparison dump after restore$space_rextra from $fmt$space_dextra: succeeds"
	);
	is($stdout, $want_dump_sql,
		"comparison dump after restore$space_rextra from $fmt$space_dextra: expected stdout"
	);
	is($stderr, '',
		"comparison dump after restore$space_rextra from $fmt$space_dextra: no stderr"
	);

	# restore via plain format
	($result, $stdout, $stderr) =
	  $node->psql('regression_utf8', $sql_from_pg_restore);
	is($result, 0,
		"restore$space_rextra via $fmt$space_dextra => plain: succeeds");
	like($stdout, $want_psql_stdout,
		"restore$space_rextra via $fmt$space_dextra => plain: expected stdout"
	);
	is($stderr, '',
		"restore$space_rextra via $fmt$space_dextra => plain: no stderr");

	# tar fmt: test restore.sql
	next unless $fmt eq 'tar';
  SKIP:
	{
		my $unpacked = $dump_file . '.unpacked';
		my $tar = $ENV{TAR};
		skip "no tar program available", 1
		  if (!defined $tar || $tar eq '');

		# Unpack tar; fetch and transform restore.sql from its root.  $$PATH$$
		# will appear in a line like:
		#
		#	-- default settings
		#	COPY public.t (c) FROM '$$PATH$$/4162.dat';
		#	-- inserts
		#	\i $$PATH$$/4162.dat
		#
		# We don't support a build directory containing a single quote.
		# Hence, the former needs no escaping, and plain single quotes suffice
		# for the latter.
		mkdir($unpacked) || croak "mkdir $unpacked: $!";
		system_or_bail($tar, 'xf' => $dump_file, '-C' => $unpacked);
		my $restore_dot_sql = slurp_file("$unpacked/restore.sql");
		$restore_dot_sql =~ s/'\$\$PATH\$\$/'$unpacked/g;
		$restore_dot_sql =~ s/\\i \$\$PATH\$\$/\\i '$unpacked'/g;

		Test::More->builder->todo_start(
			"--inserts restore.sql not updated for CVE-2025-8714 fix: data rows not restored"
		) if $space_dextra =~ /inserts/;

		# restore.sql drops and creates the database like "pg_dump --clean
		# --create", so connect to some other database.
		($result, $stdout, $stderr) =
		  $node->psql('postgres', $restore_dot_sql);
		is($result, 0, "restore.sql$space_dextra: succeeds");
		like($stdout, $want_psql_stdout,
			"restore.sql$space_dextra: expected stdout");
		is($stderr, '', "restore.sql$space_dextra: no stderr");

		# compare round trip
		$result = run_log [
			'pg_dump', '--clean',
			'--restrict-key' => 'test',
			$node->connstr('regression_utf8')
		  ],
		  '>' => \$stdout,
		  '2>' => \$stderr;
		ok($result,
			"comparison dump after restore.sql$space_dextra: succeeds");
		is($stdout, $want_dump_sql,
			"comparison dump after restore.sql$space_dextra: expected stdout"
		);
		is($stderr, '',
			"comparison dump after restore.sql$space_dextra: no stderr");

		Test::More->builder->todo_end if $space_dextra =~ /inserts/;
	}
}

# Confirm that a "pg_dumpall --clean" can be restored cleanly.
$node->safe_psql('postgres', 'CREATE USER other_super SUPERUSER;');

my @dumpall_cases = (
	{ name => 'basic', extra_args => [] },
	{
		name => 'with --role',
		extra_args => [ '--role' => 'other_super' ],
	},);
my $i = 0;

foreach my $c (@dumpall_cases)
{
	my $file = "$backupdir/dumpall-clean-$i";
	$i++;

	$node->command_ok(
		[
			'pg_dumpall',
			'--clean',
			'--verbose',
			'--no-sync',
			'--file' => $file,
			'--dbname' => $node->connstr('postgres'),
			@{ $c->{extra_args} },
		],
		"dump for: restore from pg_dumpall --clean ($c->{name})");
	$node->command_ok(
		[
			'psql', '--no-psqlrc',
			'--file' => $file,
			'--dbname' => $node->connstr('postgres'),
		],
		"restore from pg_dumpall --clean ($c->{name})");
}


done_testing();
