
# Copyright (c) 2023-2024, PostgreSQL Global Development Group

# This regression test checks the behavior of the btree validation in the
# presence of breaking sort order changes.
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# Create a custom operator class and an index which uses it.
$node->safe_psql(
	'postgres', q(
	CREATE EXTENSION amcheck;

	CREATE FUNCTION ok_cmp (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT
			CASE WHEN $1 < $2 THEN -1
				 WHEN $1 > $2 THEN  1
				 ELSE 0
			END;
	$$;

	---
	--- Check 1: uniqueness violation.
	---
	CREATE FUNCTION ok_cmp1 (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT public.ok_cmp($1, $2);
	$$;

	---
	--- Make values 768 and 769 look equal.
	---
	CREATE FUNCTION bad_cmp1 (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT
			CASE WHEN ($1 = 768 AND $2 = 769) OR
					  ($1 = 769 AND $2 = 768) THEN 0
				 ELSE public.ok_cmp($1, $2)
			END;
	$$;

	---
	--- Check 2: uniqueness violation without deduplication.
	---
	CREATE FUNCTION ok_cmp2 (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT public.ok_cmp($1, $2);
	$$;

	CREATE FUNCTION bad_cmp2 (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT
			CASE WHEN $1 = $2 AND $1 = 400 THEN -1
			ELSE public.ok_cmp($1, $2)
		END;
	$$;

	---
	--- Check 3: uniqueness violation with deduplication.
	---
	CREATE FUNCTION ok_cmp3 (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT public.ok_cmp($1, $2);
	$$;

	CREATE FUNCTION bad_cmp3 (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT public.bad_cmp2($1, $2);
	$$;

	---
	--- Create data.
	---
	CREATE TABLE bttest_unique1 (i int4);
	INSERT INTO bttest_unique1
		(SELECT * FROM generate_series(1, 1024) gs);

	CREATE TABLE bttest_unique2 (i int4);
	INSERT INTO bttest_unique2(i)
		(SELECT * FROM generate_series(1, 400) gs);
	INSERT INTO bttest_unique2
		(SELECT * FROM generate_series(400, 1024) gs);

	CREATE TABLE bttest_unique3 (i int4);
	INSERT INTO bttest_unique3
		SELECT * FROM bttest_unique2;

	CREATE OPERATOR CLASS int4_custom_ops1 FOR TYPE int4 USING btree AS
		OPERATOR 1 < (int4, int4), OPERATOR 2 <= (int4, int4),
		OPERATOR 3 = (int4, int4), OPERATOR 4 >= (int4, int4),
		OPERATOR 5 > (int4, int4), FUNCTION 1 ok_cmp1(int4, int4);
	CREATE OPERATOR CLASS int4_custom_ops2 FOR TYPE int4 USING btree AS
		OPERATOR 1 < (int4, int4), OPERATOR 2 <= (int4, int4),
		OPERATOR 3 = (int4, int4), OPERATOR 4 >= (int4, int4),
		OPERATOR 5 > (int4, int4), FUNCTION 1 bad_cmp2(int4, int4);
	CREATE OPERATOR CLASS int4_custom_ops3 FOR TYPE int4 USING btree AS
		OPERATOR 1 < (int4, int4), OPERATOR 2 <= (int4, int4),
		OPERATOR 3 = (int4, int4), OPERATOR 4 >= (int4, int4),
		OPERATOR 5 > (int4, int4), FUNCTION 1 bad_cmp3(int4, int4);

	CREATE UNIQUE INDEX bttest_unique_idx1
						ON bttest_unique1
						USING btree (i int4_custom_ops1)
						WITH (deduplicate_items = off);
	CREATE UNIQUE INDEX bttest_unique_idx2
						ON bttest_unique2
						USING btree (i int4_custom_ops2)
						WITH (deduplicate_items = off);
	CREATE UNIQUE INDEX bttest_unique_idx3
						ON bttest_unique3
						USING btree (i int4_custom_ops3)
						WITH (deduplicate_items = on);
));

my ($result, $stdout, $stderr);

#
# Test 1.
#  - insert seq values
#  - create unique index
#  - break cmp function
#  - amcheck finds the uniqueness violation
#

# We have not yet broken the index, so we should get no corruption
$result = $node->safe_psql(
	'postgres', q(
	SELECT bt_index_check('bttest_unique_idx1', true, true);
));
is($result, '', 'run amcheck on non-broken bttest_unique_idx1');

# Change the operator class to use a function which considers certain different
# values to be equal.
$node->safe_psql(
	'postgres', q(
	UPDATE pg_catalog.pg_amproc SET
		   amproc = 'bad_cmp1'::regproc
	WHERE amproc = 'ok_cmp1'::regproc;
));

($result, $stdout, $stderr) = $node->psql(
	'postgres', q(
	SELECT bt_index_check('bttest_unique_idx1', true, true);
));
ok( $stderr =~ /index uniqueness is violated for index "bttest_unique_idx1"/,
	'detected uniqueness violation for index "bttest_unique_idx1"');

#
# Test 2.
#  - break cmp function
#  - insert seq values with duplicates
#  - create unique index
#  - make cmp function correct
#  - amcheck finds the uniqueness violation
#

# Due to bad cmp function we expect amcheck to detect item order violation,
# but no uniqueness violation.
($result, $stdout, $stderr) = $node->psql(
	'postgres', q(
	SELECT bt_index_check('bttest_unique_idx2', true, true);
));
ok( $stderr =~ /item order invariant violated for index "bttest_unique_idx2"/,
	'detected item order invariant violation for index "bttest_unique_idx2"');

$node->safe_psql(
	'postgres', q(
	UPDATE pg_catalog.pg_amproc SET
		   amproc = 'ok_cmp2'::regproc
	WHERE amproc = 'bad_cmp2'::regproc;
));

($result, $stdout, $stderr) = $node->psql(
	'postgres', q(
	SELECT bt_index_check('bttest_unique_idx2', true, true);
));
ok( $stderr =~ /index uniqueness is violated for index "bttest_unique_idx2"/,
	'detected uniqueness violation for index "bttest_unique_idx2"');

#
# Test 3.
#  - same as Test 2, but with index deduplication
#
# Then uniqueness violation is detected between different posting list
# entries inside one index entry.
#

# Due to bad cmp function we expect amcheck to detect item order violation,
# but no uniqueness violation.
($result, $stdout, $stderr) = $node->psql(
	'postgres', q(
	SELECT bt_index_check('bttest_unique_idx3', true, true);
));
ok( $stderr =~ /item order invariant violated for index "bttest_unique_idx3"/,
	'detected item order invariant violation for index "bttest_unique_idx3"');

# For unique index deduplication is possible only for same values, but
# with different visibility.
$node->safe_psql(
	'postgres', q(
	DELETE FROM bttest_unique3 WHERE 380 <= i AND i <= 420;
	INSERT INTO bttest_unique3 (SELECT * FROM generate_series(380, 420));
	INSERT INTO bttest_unique3 VALUES (400);
	DELETE FROM bttest_unique3 WHERE 380 <= i AND i <= 420;
	INSERT INTO bttest_unique3 (SELECT * FROM generate_series(380, 420));
	INSERT INTO bttest_unique3 VALUES (400);
	DELETE FROM bttest_unique3 WHERE 380 <= i AND i <= 420;
	INSERT INTO bttest_unique3 (SELECT * FROM generate_series(380, 420));
	INSERT INTO bttest_unique3 VALUES (400);
));

$node->safe_psql(
	'postgres', q(
	UPDATE pg_catalog.pg_amproc SET
		   amproc = 'ok_cmp3'::regproc
	WHERE amproc = 'bad_cmp3'::regproc;
));

($result, $stdout, $stderr) = $node->psql(
	'postgres', q(
	SELECT bt_index_check('bttest_unique_idx3', true, true);
));
ok( $stderr =~ /index uniqueness is violated for index "bttest_unique_idx3"/,
	'detected uniqueness violation for index "bttest_unique_idx3"');

$node->stop;
done_testing();
