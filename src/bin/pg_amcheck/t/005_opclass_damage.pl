
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

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
$node->start;

# Create a custom operator class and an index which uses it.
$node->safe_psql(
	'postgres', q(
	CREATE EXTENSION amcheck;

	CREATE FUNCTION int4_asc_cmp (a int4, b int4) RETURNS int LANGUAGE sql AS $$
		SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 > $2 THEN 1 ELSE -1 END; $$;

	CREATE FUNCTION ok_cmp (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT
			CASE WHEN $1 < $2 THEN -1
				 WHEN $1 > $2 THEN  1
				 ELSE 0
			END;
	$$;

	CREATE OPERATOR CLASS int4_fickle_ops FOR TYPE int4 USING btree AS
	    OPERATOR 1 < (int4, int4), OPERATOR 2 <= (int4, int4),
	    OPERATOR 3 = (int4, int4), OPERATOR 4 >= (int4, int4),
	    OPERATOR 5 > (int4, int4), FUNCTION 1 int4_asc_cmp(int4, int4);

	CREATE OPERATOR CLASS int4_unique_ops FOR TYPE int4 USING btree AS
		OPERATOR 1 < (int4, int4), OPERATOR 2 <= (int4, int4),
		OPERATOR 3 = (int4, int4), OPERATOR 4 >= (int4, int4),
		OPERATOR 5 > (int4, int4), FUNCTION 1 ok_cmp(int4, int4);

	CREATE TABLE int4tbl (i int4);
	INSERT INTO int4tbl (SELECT * FROM generate_series(1,1000) gs);
	CREATE INDEX fickleidx ON int4tbl USING btree (i int4_fickle_ops);
	CREATE UNIQUE INDEX bttest_unique_idx
						ON int4tbl
						USING btree (i int4_unique_ops)
						WITH (deduplicate_items = off);
));

# We have not yet broken the index, so we should get no corruption
$node->command_like([ 'pg_amcheck', '-p', $node->port, 'postgres' ],
	qr/^$/,
	'pg_amcheck all schemas, tables and indexes reports no corruption');

# Change the operator class to use a function which sorts in a different
# order to corrupt the btree index
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION int4_desc_cmp (int4, int4) RETURNS int LANGUAGE sql AS $$
		SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 > $2 THEN -1 ELSE 1 END; $$;
	UPDATE pg_catalog.pg_amproc
		SET amproc = 'int4_desc_cmp'::regproc
		WHERE amproc = 'int4_asc_cmp'::regproc
));

# Index corruption should now be reported
$node->command_checks_all(
	[ 'pg_amcheck', '-p', $node->port, 'postgres' ],
	2,
	[qr/item order invariant violated for index "fickleidx"/],
	[],
	'pg_amcheck all schemas, tables and indexes reports fickleidx corruption'
);

#
# Check unique constraints
#

# Repair broken opclass for check unique tests.
$node->safe_psql(
	'postgres', q(
	UPDATE pg_catalog.pg_amproc
		SET amproc = 'int4_asc_cmp'::regproc
		WHERE amproc = 'int4_desc_cmp'::regproc
));

# We should get no corruptions
$node->command_like(
	[ 'pg_amcheck', '--checkunique', '-p', $node->port, 'postgres' ],
	qr/^$/,
	'pg_amcheck all schemas, tables and indexes reports no corruption');

# Break opclass for check unique tests.
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION bad_cmp (int4, int4)
	RETURNS int LANGUAGE sql AS
	$$
		SELECT
			CASE WHEN ($1 = 768 AND $2 = 769) OR
					  ($1 = 769 AND $2 = 768) THEN 0
				 WHEN $1 < $2 THEN -1
				 WHEN $1 > $2 THEN  1
				 ELSE 0
			END;
	$$;

	UPDATE pg_catalog.pg_amproc
		SET amproc = 'bad_cmp'::regproc
		WHERE amproc = 'ok_cmp'::regproc
));

# Unique index corruption should now be reported
$node->command_checks_all(
	[ 'pg_amcheck', '--checkunique', '-p', $node->port, 'postgres' ],
	2,
	[qr/index uniqueness is violated for index "bttest_unique_idx"/],
	[],
	'pg_amcheck all schemas, tables and indexes reports bttest_unique_idx corruption'
);
done_testing();
