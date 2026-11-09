# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test composite datums and attribute versions across pg_upgrade.
#
# Pointing oldinstall to an installation predating versioning allows the
# highest fidelity testing. Otherwise the regress library's
# make_tuple_unversioned() is used to simulate its unversioned datums.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $old =
  PostgreSQL::Test::Cluster->new('old', install_path => $ENV{oldinstall});
my $new = PostgreSQL::Test::Cluster->new('new');

# Checksums are now enabled by default, but weren't before 18, so pass '-k' to
# initdb on older versions so that upgrades work. But don't do it otherwise,
# as passing options prevents using the INITDB_TEMPLATE optimization.
if (defined($ENV{oldinstall}))
{
	$old->init(extra => ['-k']);
}
else
{
	$old->init();
}

$new->init();

$old->start;

# Older releases may or may not have the attribute-versioning
# backpatch. Identify those by checking if they have the
# binary_upgrade_set_next_attribute_versions() function - a sufficient test,
# as we newly initdb'd the source cluster.
my $has_versioning = $old->safe_psql(
	'postgres', q{
	SELECT EXISTS (SELECT FROM pg_proc
	               WHERE pronamespace = 'pg_catalog'::regnamespace
	                 AND proname = 'binary_upgrade_set_next_attribute_versions')
}) eq 't';
my $clear_version = 'attoptions = NULL';

# An unpatched $old forms unversioned datums naturally.  A versioned $old
# needs the helper from the regress library of its own build.  For an older
# installation, that is the one its regression database dump refers to.
my $has_helper = 0;
if ($has_versioning)
{
	my $regresslib;

	if (!defined($ENV{oldinstall}))
	{
		$regresslib = $ENV{REGRESS_SHLIB};
	}
	elsif (defined($ENV{olddump})
		&& slurp_file($ENV{olddump}) =~ m{'([^']*/regress(?:\.[^'/]*)?)'})
	{
		$regresslib = $1;
	}

	if (!defined($regresslib))
	{
		note 'make_tuple_unversioned(): no old regress library known';
	}
	else
	{
		my ($ret, $stdout, $stderr) = $old->psql(
			'postgres',
			"CREATE FUNCTION make_tuple_unversioned(anyelement) RETURNS anyelement
			 AS '$regresslib' LANGUAGE C STRICT");
		$has_helper = $ret == 0;
		note "could not create make_tuple_unversioned(): $stderr"
		  if !$has_helper;
	}
}
BAIL_OUT('make_tuple_unversioned not available from this build')
  if !defined($ENV{oldinstall}) && !$has_helper;

my $test_unversioned = !$has_versioning || $has_helper;

$old->safe_psql(
	'postgres', q{
	-- A stored composite datum with fewer attributes than the type, due
	-- to a subsequent ADD ATTRIBUTE.
	CREATE TYPE rowver_comp AS (a int);
	CREATE TABLE rowver_tbl (i int, c rowver_comp);
	INSERT INTO rowver_tbl VALUES (1, '(1)');
	ALTER TYPE rowver_comp ADD ATTRIBUTE b int;
	INSERT INTO rowver_tbl VALUES (2, '(2,22)');

	-- A stored composite datum with attributes that are later dropped.
	CREATE TABLE rowdrop_src (a int);
	ALTER TABLE rowdrop_src ADD COLUMN b int;
	CREATE TABLE rowdrop_tbl (r rowdrop_src);
	INSERT INTO rowdrop_tbl VALUES (ROW(1,2));
	ALTER TABLE rowdrop_src DROP COLUMN b;

	-- The same for a composite type, whose versions pg_dump collects
	-- separately.
	CREATE TYPE rowdropt_comp AS (a int);
	ALTER TYPE rowdropt_comp ADD ATTRIBUTE b int;
	CREATE TABLE rowdropt_tbl (c rowdropt_comp);
	INSERT INTO rowdropt_tbl VALUES (ROW(1,2));
	ALTER TYPE rowdropt_comp DROP ATTRIBUTE b;

	-- Restore the attribute versions of a view with a stored datum made
	-- narrower than the view by a subsequent CREATE OR REPLACE VIEW.
	CREATE VIEW rowview AS SELECT 1 AS a;
	CREATE TABLE rowview_tbl (r rowview);
	INSERT INTO rowview_tbl SELECT v FROM rowview v;
	CREATE OR REPLACE VIEW rowview AS SELECT 1 AS a, 2 AS b;

	-- A partition key can embed a constant of the table's own row type.
	-- The final versions must be in place while CREATE TABLE is parsed.
	CREATE TABLE rowself(a int, b int, k int)
	  PARTITION BY LIST ((CASE WHEN k > 0 THEN '(1,2,0)'::rowself
	                                      ELSE '(3,4,0)'::rowself END));
	CREATE TABLE rowself_1 PARTITION OF rowself FOR VALUES IN ('(1,2,0)');
	CREATE TABLE rowself_2 PARTITION OF rowself FOR VALUES IN ('(3,4,0)');
	INSERT INTO rowself VALUES (7,8,1), (9,10,-1);

	-- A self-referencing view needs its versions restored when pg_dump
	-- creates the dummy view, before CREATE OR REPLACE VIEW is parsed.
	-- Field selection deforms the constant when the view is planned.
	CREATE VIEW rowselfv AS SELECT 'firstver' AS a;
	CREATE OR REPLACE VIEW rowselfv AS
	  SELECT ('(secondver)'::rowselfv).a;

	CREATE TYPE rowunver_comp AS (a int, b int);

	-- A table whose column versions are cleared below, to simulate columns
	-- created before updating to a release with attribute versioning.
	CREATE TABLE rowunvdrop_src (a int, b int, c int);
	CREATE TABLE rowunvdrop_tbl (r rowunvdrop_src);

	-- Further relations with a row type, with differing attribute
	-- versions, to verify pg_upgrade preserves their versions.
	CREATE TABLE rowalt (a int, b int, c int);
	ALTER TABLE rowalt ALTER COLUMN b TYPE bigint;
	CREATE TABLE rowinh_parent (a int);
	CREATE TABLE rowinh_child (b int) INHERITS (rowinh_parent);
	ALTER TABLE rowinh_parent ADD COLUMN c int;
	CREATE TYPE rowtyped_comp AS (a int, b int);
	CREATE TABLE rowtyped_tbl OF rowtyped_comp;
	ALTER TYPE rowtyped_comp ADD ATTRIBUTE c int CASCADE;
	CREATE MATERIALIZED VIEW rowmatview AS SELECT 1 AS a, 2 AS b;
	CREATE FOREIGN DATA WRAPPER rowfdw;
	CREATE SERVER rowserver FOREIGN DATA WRAPPER rowfdw;
	CREATE FOREIGN TABLE rowforeign (a int, b int) SERVER rowserver;
	ALTER FOREIGN TABLE rowforeign ALTER COLUMN b TYPE bigint;
	CREATE TYPE rowzero_comp AS ();
});

# With versioning, clearing the type's versions produces a versioned datum
# with version zero.  This is distinct from an unversioned datum.
if ($has_versioning)
{
	$old->safe_psql(
		'postgres', qq{
		UPDATE pg_attribute SET $clear_version
		WHERE attrelid IN ('rowunver_comp'::regclass,
		                   'rowunvdrop_src'::regclass);
	});
}
$old->safe_psql(
	'postgres', q{
	CREATE TABLE rowunver_tbl (c rowunver_comp);
	INSERT INTO rowunver_tbl VALUES ('(3,4)');

	-- Other attribute options and dropping a column must cope with columns
	-- lacking a version.
	ALTER TABLE rowunvdrop_src ALTER COLUMN c SET (n_distinct = 5);
	INSERT INTO rowunvdrop_tbl VALUES (ROW(1, 2, 3));
	ALTER TABLE rowunvdrop_src DROP COLUMN b;
});

if ($test_unversioned)
{
	my $unversion = $has_helper ? 'make_tuple_unversioned' : '';
	$old->safe_psql(
		'postgres', qq{
		CREATE TYPE rowold_comp AS (a int);
		CREATE TABLE rowold_tbl (key text, c rowold_comp);
		INSERT INTO rowold_tbl
		VALUES ('prefix', $unversion(ROW(0)::rowold_comp));
		ALTER TYPE rowold_comp ADD ATTRIBUTE b int;
		INSERT INTO rowold_tbl
		VALUES ('before-alter', $unversion(ROW(1,2)::rowold_comp));
	});

	if ($has_versioning)
	{
		$old->safe_psql(
			'postgres', qq{
			UPDATE pg_attribute SET $clear_version
			WHERE attrelid = 'rowold_comp'::regclass;
		});
	}

	$old->safe_psql(
		'postgres', qq{
		ALTER TYPE rowold_comp ADD ATTRIBUTE c int;
		INSERT INTO rowold_tbl
		VALUES ('after-alter', $unversion(ROW(3,4,5)::rowold_comp));
	});

	# pg_upgrade checks that the new installation can load the libraries of
	# all C functions, which fails for an older build's regress library.
	$old->safe_psql('postgres',
		'DROP FUNCTION make_tuple_unversioned(anyelement)')
	  if $has_helper;
}

# Remember the attribute versions of all relations with a row type, to verify
# they are preserved exactly.
my $new_version_expr = q{
    COALESCE((SELECT substr(o, length('pg_attver=') + 1)::numeric
	  FROM unnest(a.attoptions) o
	  WHERE starts_with(o, 'pg_attver=')), 0)
};

# Without versioning, all versions are zero.
my $old_version_expr = '0';
if ($has_versioning)
{
	$old_version_expr = $new_version_expr;
}

my $old_versions =
  $old->safe_psql('postgres', versions_query($old_version_expr));

$old->stop;

# Keep pg_upgrade's generated files in the test directory for VPATH builds.
chdir ${PostgreSQL::Test::Utils::tmp_check};
command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $old->config_data('--bindir'),
		'--new-bindir' => $new->config_data('--bindir'),
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
	],
	'upgrade composite datums in copy mode');

$new->start;

my $new_versions =
  $new->safe_psql('postgres', versions_query($new_version_expr));
is($new_versions, $old_versions,
	'attribute versions preserved by pg_upgrade');

# Versions assigned after the upgrade have to exceed all restored ones.
is( $new->safe_psql(
		'postgres', qq{
		SELECT max($new_version_expr) < pg_current_wal_insert_lsn() - '0/0'
		FROM pg_attribute a WHERE a.attrelid >= 16384}),
	't',
	'restored attribute versions precede new versions');

# Versioned sources require exact attribute versions to be restored;
# unversioned sources need to restore a version of 0.
is($new->safe_psql('postgres', 'SELECT c FROM rowver_tbl ORDER BY i'),
	"(1,)\n(2,22)", 'composite datums readable after pg_upgrade');
is($new->safe_psql('postgres', 'SELECT c FROM rowunver_tbl'),
	'(3,4)', 'zero-version datum readable after pg_upgrade');
is($new->safe_psql('postgres', 'SELECT r FROM rowdrop_tbl'),
	'(1)', 'datum with dropped last column readable after pg_upgrade');
is( $new->safe_psql('postgres', 'SELECT c FROM rowdropt_tbl'),
	'(1)',
	'datum with dropped last attribute of composite type readable after pg_upgrade'
);
is($new->safe_psql('postgres', 'SELECT r FROM rowunvdrop_tbl'),
	'(1,3)',
	'datum with dropped zero-version column readable after pg_upgrade');
is( $new->safe_psql(
		'postgres', q{
		SELECT attoptions FROM pg_attribute
		WHERE attrelid = 'rowunvdrop_src'::regclass AND attname = 'c'}),
	'{n_distinct=5}',
	'attribute options of zero-version column preserved by pg_upgrade');
is($new->safe_psql('postgres', 'SELECT r FROM rowview_tbl'),
	'(1,)', 'view-rowtype datum readable after pg_upgrade');

is( $new->safe_psql(
		'postgres', q{
		SELECT regexp_replace(pg_get_partkeydef('rowself'::regclass) || ' ' ||
		                      pg_get_expr(relpartbound, oid), '\s+', ' ', 'g')
		FROM pg_class WHERE relname = 'rowself_1'}),
	q{LIST (( CASE WHEN (k > 0) THEN '(1,2,0)'::rowself ELSE '(3,4,0)'::rowself END)) FOR VALUES IN ('(1,2,0)')},
	'self-referencing partition key and bound readable after pg_upgrade');
$new->safe_psql('postgres', 'INSERT INTO rowself VALUES (11,12,1)');
is($new->safe_psql('postgres', 'SELECT count(*) FROM rowself_1'),
	'2', 'routing by self-referencing partition key after pg_upgrade');
is($new->safe_psql('postgres', 'SELECT a FROM rowselfv'),
	'secondver', 'self-referencing view usable after pg_upgrade');

# Check changed versions as well as readability.  Hold cursors retain the
# datums after dropping their table, allowing the type to be altered.
my ($code, $stdout, $stderr) = $new->psql(
	'postgres', q{
	BEGIN;
	DECLARE prefix CURSOR WITH HOLD FOR SELECT c FROM rowver_tbl WHERE i = 1;
	DECLARE whole CURSOR WITH HOLD FOR SELECT c FROM rowver_tbl WHERE i = 2;
	COMMIT;
	BEGIN;
	DROP TABLE rowver_tbl;
	ALTER TYPE rowver_comp ALTER ATTRIBUTE b TYPE bigint;
	FETCH FIRST FROM prefix;
	FETCH FIRST FROM whole;
});
is($stdout, '(1,)', 'unchanged prefix readable after altering upgraded type');
is($code, 3, 'changed attribute rejected after altering upgraded type');
like(
	$stderr,
	qr/row type of datum tuple was altered after the datum's creation/,
	'changed attribute reports a version mismatch');

SKIP:
{
	skip 'old versioned installation lacks make_tuple_unversioned'
	  unless $test_unversioned;

	is( $new->safe_psql(
			'postgres', "SELECT c FROM rowold_tbl WHERE key = 'before-alter'"
		),
		'(1,2,)',
		'unversioned prefix readable after pg_upgrade');

	($code, $stdout, $stderr) = $new->psql('postgres',
		"SELECT c FROM rowold_tbl WHERE key = 'after-alter'");
	if ($has_versioning)
	{
		is($code, 3,
			'unversioned datum containing versioned attribute rejected');
		like(
			$stderr,
			qr/row type of datum tuple was altered after the datum's creation/,
			'unversioned datum reports a version mismatch');
	}
	else
	{
		is($code, 0, 'unversioned datum from unpatched source accepted');
		is($stdout, '(3,4,5)', 'all original attributes readable');
	}

	($code, $stdout, $stderr) = $new->psql(
		'postgres', q{
		BEGIN;
		DECLARE prefix CURSOR WITH HOLD FOR
		  SELECT c FROM rowold_tbl WHERE key = 'prefix';
		DECLARE whole CURSOR WITH HOLD FOR
		  SELECT c FROM rowold_tbl WHERE key = 'before-alter';
		COMMIT;
		BEGIN;
		DROP TABLE rowold_tbl;
		ALTER TYPE rowold_comp ALTER ATTRIBUTE b TYPE bigint;
		FETCH FIRST FROM prefix;
		FETCH FIRST FROM whole;
	});
	is($stdout, '(0,,)',
		'unversioned prefix readable after altering upgraded type');
	is($code, 3, 'unversioned datum with changed attribute rejected');
	like(
		$stderr,
		qr/row type of datum tuple was altered after the datum's creation/,
		'unversioned datum reports a version mismatch after type alteration');
}

sub versions_query
{
	my ($version_expr) = @_;
	return qq{
		SELECT c.relname, a.attnum, $version_expr
		FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid
		WHERE c.relnamespace = 'public'::regnamespace
		  AND c.relkind IN ('r', 'v', 'm', 'c', 'f', 'p') AND a.attnum > 0
		ORDER BY c.relname, a.attnum};
}


$new->stop;
done_testing();
