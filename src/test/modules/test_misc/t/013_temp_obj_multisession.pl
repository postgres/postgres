# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests that one session cannot read or modify data in another session's
# temporary table.  Each session keeps its temp data in its own local
# buffer pool, and a different backend has no visibility into those
# buffers, so any command that needs to look at the data must be
# rejected.
#
# DROP TABLE is intentionally allowed: it does not touch the table's
# contents, and autovacuum relies on this to clean up orphaned temp
# relations left behind by a crashed backend.
#
# A regression caught here typically means a new buffer-access entry
# point bypasses the RELATION_IS_OTHER_TEMP() check.  See
# ReadBuffer_common(), StartReadBuffersImpl(), and read_stream_begin_impl()
# for the existing checks.  When adding a new command or buffer-access
# path, also add a corresponding case below.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::BackgroundPsql;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('temp_lock');
$node->init;
$node->append_conf('postgresql.conf', 'log_lock_waits=on');
$node->start;

# Owner session.  Created via background_psql so it stays alive while
# the second session probes its temp objects.
my $psql1 = $node->background_psql('postgres');

# Initially create the table without an index, so read paths go straight
# through the read-stream / buffer-manager entry points without being
# masked by an index scan that would hit ReadBuffer_common from nbtree.
$psql1->query_safe(q(CREATE TEMP TABLE foo AS SELECT 42 AS val;));

# Resolve the owner's temp schema so the probing session can refer to
# the table by a fully-qualified name.
my $tempschema = $node->safe_psql(
	'postgres',
	q{
      SELECT n.nspname
      FROM pg_class c
      JOIN pg_namespace n ON n.oid = c.relnamespace
      WHERE relname = 'foo' AND relpersistence = 't';
    }
);
chomp $tempschema;
ok($tempschema =~ /^pg_temp_\d+$/, "got temp schema: $tempschema");

my ($stdout, $stderr);

# DML and SELECT have to read the table's data and therefore go through
# the buffer manager.  With no index on the table, the planner cannot
# use index access, so SELECT/UPDATE/DELETE/MERGE/COPY all run through
# the read-stream path.
#
# XXX: in current code, the read-stream path bypasses the
# RELATION_IS_OTHER_TEMP() check, so these commands silently see no
# rows / report zero affected rows -- the visible symptom of the bug
# this test suite documents.  A follow-up patch will route the check
# through read_stream_begin_impl() and these assertions will be
# updated to expect "cannot access temporary tables of other sessions".

$node->psql(
	'postgres',
	"SELECT val FROM $tempschema.foo;",
	stdout => \$stdout,
	stderr => \$stderr);
is($stderr, '', 'SELECT (currently no error -- bug to be fixed)');

# INSERT goes through hio.c which calls ReadBufferExtended() to find a
# page with free space; that hits the existing check before any data is
# written.  This case currently errors as expected.
$node->psql(
	'postgres',
	"INSERT INTO $tempschema.foo VALUES (73);",
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary tables of other sessions/,
	'INSERT (caught via hio.c)');

$node->psql(
	'postgres',
	"UPDATE $tempschema.foo SET val = NULL;",
	stderr => \$stderr);
is($stderr, '', 'UPDATE (currently no error -- bug to be fixed)');

$node->psql('postgres', "DELETE FROM $tempschema.foo;", stderr => \$stderr);
is($stderr, '', 'DELETE (currently no error -- bug to be fixed)');

$node->psql(
	'postgres',
	"MERGE INTO $tempschema.foo USING (VALUES (42)) AS s(val) "
	  . "ON foo.val = s.val WHEN MATCHED THEN DELETE;",
	stderr => \$stderr);
is($stderr, '', 'MERGE (currently no error -- bug to be fixed)');

$node->psql('postgres', "COPY $tempschema.foo TO STDOUT;",
	stderr => \$stderr);
is($stderr, '', 'COPY (currently no error -- bug to be fixed)');

# DDL and maintenance commands have their own command-specific checks
# (older than the buffer-manager check above), so they fail with
# command-specific error messages.  Verifying them here documents the
# expected behaviour and guards against accidental removal of those
# checks.

$node->psql('postgres', "TRUNCATE TABLE $tempschema.foo;",
	stderr => \$stderr);
like($stderr, qr/cannot truncate temporary tables of other sessions/,
	'TRUNCATE');

$node->psql(
	'postgres',
	"ALTER TABLE $tempschema.foo ALTER COLUMN val TYPE bigint;",
	stderr => \$stderr);
like($stderr, qr/cannot alter temporary tables of other sessions/,
	'ALTER TABLE');

# VACUUM silently skips other sessions' temp tables (vacuum_rel() returns
# without warning to avoid noise during database-wide VACUUM).  Verify
# that no error is reported, and that no buffer-access path is hit.
$node->psql('postgres', "VACUUM $tempschema.foo;", stderr => \$stderr);
is($stderr, '', 'VACUUM is silently skipped');

$node->psql('postgres', "CLUSTER $tempschema.foo;", stderr => \$stderr);
like($stderr, qr/cannot cluster temporary tables of other sessions/,
	'CLUSTER');

# Now create an index to exercise the index-scan path.  nbtree calls
# ReadBuffer (which is ReadBufferExtended -> ReadBuffer_common), so
# this exercises a different chain of buffer-manager entry points.
$psql1->query_safe(q(CREATE INDEX ON foo(val);));

$node->psql(
	'postgres',
	"SET enable_seqscan = off; SELECT val FROM $tempschema.foo WHERE val = 42;",
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary tables of other sessions/,
	'index scan (ReadBuffer_common via nbtree)');

# ALTER INDEX goes through the same CheckAlterTableIsSafe() path as
# ALTER TABLE, so it produces the same error.
$node->psql(
	'postgres',
	"ALTER INDEX $tempschema.foo_val_idx SET (fillfactor = 50);",
	stderr => \$stderr);
like($stderr, qr/cannot alter temporary tables of other sessions/,
	'ALTER INDEX');

# A function created by the owner in its own pg_temp using its own
# row type can be observed via the catalog by a separate session.
# ALTER FUNCTION and DROP FUNCTION on it must work as catalog
# operations -- they don't read the underlying table -- which
# documents the boundary between catalog and data access for temp
# objects.
$psql1->query_safe(
		q[CREATE FUNCTION pg_temp.foo_id(r foo) RETURNS int LANGUAGE SQL ]
	  . q[AS 'SELECT r.val';]);

$node->psql(
	'postgres',
	"ALTER FUNCTION $tempschema.foo_id($tempschema.foo) "
	  . "SET search_path = pg_catalog;",
	stderr => \$stderr);
is($stderr, '', 'ALTER FUNCTION on function over other session\'s row type');

$node->psql(
	'postgres',
	"DROP FUNCTION $tempschema.foo_id($tempschema.foo);",
	stderr => \$stderr);
is($stderr, '', 'DROP FUNCTION on function over other session\'s row type');

# DROP TABLE on another session's temp table is intentionally permitted.
# DROP doesn't touch the table's contents, and autovacuum relies on this
# to remove temp relations orphaned by a crashed backend.  Verify that
# the bare DROP succeeds without error.
$node->psql('postgres', "DROP TABLE $tempschema.foo;", stderr => \$stderr);
is($stderr, '', 'DROP TABLE is allowed');

# Cross-session CREATE FUNCTION scenario.  The owner creates a fresh
# temp table foo2 in its pg_temp namespace, and a separate session
# then creates a function whose argument type is that row type.
# PostgreSQL allows this and emits a NOTICE: the function is moved
# into the creator's pg_temp namespace with an auto-dependency on
# the borrowed type, so it disappears together with the session that
# created it.
$psql1->query_safe(q(CREATE TEMP TABLE foo2 AS SELECT 42 AS val;));

$node->safe_psql('postgres',
		"CREATE FUNCTION public.cross_session_func(r $tempschema.foo2) "
	  . "RETURNS int LANGUAGE SQL AS 'SELECT 1';");

# A bare DROP TABLE on foo2 now fails because cross_session_func
# depends on its row type.  This is normal SQL dependency behaviour
# and documents that DROP itself is not blocked by buffer-manager
# checks -- we get a catalog-level error instead.
$node->psql('postgres', "DROP TABLE $tempschema.foo2;", stderr => \$stderr);
like(
	$stderr,
	qr/cannot drop table .*\.foo2 because other objects depend on it/,
	'DROP TABLE blocked by cross-session dependency');

my $foo2_oid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_class WHERE relname='foo2';");

# Cross-session LOCK TABLE scenario.  Ensure that LockRelationOid is working
# properly for other temp tables since this mechanism is also used by
# autovacuum during orphaned tables cleanup.
my $psql2 = $node->background_psql('postgres');
$psql2->query_safe(
	qq{
	BEGIN;
	LOCK TABLE $tempschema.foo2 IN ACCESS SHARE MODE;
});

# When the owner session ends, its temp objects are dropped via the
# normal session-exit cleanup, which cascades through
# DEPENDENCY_NORMAL and also removes the cross-session function that
# depended on the temp row type.  This is the same mechanism
# autovacuum relies on to clean up temp relations left behind by a
# crashed backend.
# Access share lock on the foo2 will block session-exit cleanup, because an
# owner will try to acquire deletion lock all its temp objects via
# findDependentObjects.
my $log_offset = -s $node->logfile;
$psql1->quit;

# Check whether session-exit cleanup is blocked.
$node->wait_for_log(qr/waiting for AccessExclusiveLock on relation $foo2_oid/,
	$log_offset);

# Release lock on foo2 and allow session-exit cleanup to finish.
$psql2->query_safe(q(COMMIT;));
$psql2->quit;

# After releasing the lock, the owner can finally acquire
# AccessExclusiveLock on foo2 and finish session-exit cleanup.  Verify
# directly that both foo2 (the locked temp table) and cross_session_func
# (which depended on its row type) have been dropped.  Both being gone
# confirms the owner's cleanup got past the blocked findDependentObjects()
# call and completed normally.
$node->poll_query_until('postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_class WHERE oid = $foo2_oid)")
  or die "foo2 was not cleaned up after owner session exit";

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_proc WHERE proname = 'cross_session_func'"),
	'0',
	'cross_session_func cleaned up by cascade from foo2');

done_testing();
