
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test set-up
my ($node, $port);
$node = PostgreSQL::Test::Cluster->new('test');
$node->init;
$node->start;
$port = $node->port;

# Load the amcheck extension, upon which pg_amcheck depends
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));

#########################################
# Test non-existent databases

# Failing to connect to the initial database is an error.
$node->command_checks_all(
	[ 'pg_amcheck', 'qqq' ],
	1, [qr/^$/],
	[qr/FATAL:  database "qqq" does not exist/],
	'checking a non-existent database');

# Failing to resolve a database pattern is an error by default.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'qqq', '-d', 'postgres' ],
	1,
	[qr/^$/],
	[qr/pg_amcheck: error: no connectable databases to check matching "qqq"/],
	'checking an unresolvable database pattern');

# But only a warning under --no-strict-names
$node->command_checks_all(
	[ 'pg_amcheck', '--no-strict-names', '-d', 'qqq', '-d', 'postgres' ],
	0,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: no connectable databases to check matching "qqq"/
	],
	'checking an unresolvable database pattern under --no-strict-names');

# Check that a substring of an existent database name does not get interpreted
# as a matching pattern.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'post', '-d', 'postgres' ],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: error: no connectable databases to check matching "post"/
	],
	'checking an unresolvable database pattern (substring of existent database)'
);

# Check that a superstring of an existent database name does not get interpreted
# as a matching pattern.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgresql', '-d', 'postgres' ],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: error: no connectable databases to check matching "postgresql"/
	],
	'checking an unresolvable database pattern (superstring of existent database)'
);

#########################################
# Test connecting with a non-existent user

# Failing to connect to the initial database due to bad username is an error.
$node->command_checks_all([ 'pg_amcheck', '-U', 'no_such_user', 'postgres' ],
	1, [qr/^$/], [], 'checking with a non-existent user');

#########################################
# Test checking databases without amcheck installed

# Attempting to check a database by name where amcheck is not installed should
# raise a warning.  If all databases are skipped, having no relations to check
# raises an error.
$node->command_checks_all(
	[ 'pg_amcheck', 'template1' ],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
		qr/pg_amcheck: error: no relations to check/
	],
	'checking a database by name without amcheck installed, no other databases'
);

# Again, but this time with another database to check, so no error is raised.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'template1', '-d', 'postgres' ],
	0,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/
	],
	'checking a database by name without amcheck installed, with other databases'
);

# Again, but by way of checking all databases
$node->command_checks_all(
	[ 'pg_amcheck', '--all' ],
	0,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/
	],
	'checking a database by pattern without amcheck installed, with other databases'
);

#########################################
# Test unreasonable patterns

# Check three-part unreasonable pattern that has zero-length names
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgres', '-t', '..' ],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: error: no connectable databases to check matching "\.\."/
	],
	'checking table pattern ".."');

# Again, but with non-trivial schema and relation parts
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgres', '-t', '.foo.bar' ],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: error: no connectable databases to check matching "\.foo\.bar"/
	],
	'checking table pattern ".foo.bar"');

# Check two-part unreasonable pattern that has zero-length names
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgres', '-t', '.' ],
	1,
	[qr/^$/],
	[qr/pg_amcheck: error: no heap tables to check matching "\."/],
	'checking table pattern "."');

# Check that a multipart database name is rejected
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'localhost.postgres' ],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper qualified name \(too many dotted names\): localhost\.postgres/
	],
	'multipart database patterns are rejected');

# Check that a three-part schema name is rejected
$node->command_checks_all(
	[ 'pg_amcheck', '-s', 'localhost.postgres.pg_catalog' ],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper qualified name \(too many dotted names\): localhost\.postgres\.pg_catalog/
	],
	'three part schema patterns are rejected');

# Check that a four-part table name is rejected
$node->command_checks_all(
	[ 'pg_amcheck', '-t', 'localhost.postgres.pg_catalog.pg_class' ],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper relation name \(too many dotted names\): localhost\.postgres\.pg_catalog\.pg_class/
	],
	'four part table patterns are rejected');

# Check that too many dotted names still draws an error under --no-strict-names
# That flag means that it is ok for the object to be missing, not that it is ok
# for the object name to be ungrammatical
$node->command_checks_all(
	[
		'pg_amcheck', '--no-strict-names',
		'-t',         'this.is.a.really.long.dotted.string'
	],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper relation name \(too many dotted names\): this\.is\.a\.really\.long\.dotted\.string/
	],
	'ungrammatical table names still draw errors under --no-strict-names');
$node->command_checks_all(
	[
		'pg_amcheck', '--no-strict-names', '-s',
		'postgres.long.dotted.string'
	],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper qualified name \(too many dotted names\): postgres\.long\.dotted\.string/
	],
	'ungrammatical schema names still draw errors under --no-strict-names');
$node->command_checks_all(
	[
		'pg_amcheck', '--no-strict-names', '-d',
		'postgres.long.dotted.string'
	],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper qualified name \(too many dotted names\): postgres\.long\.dotted\.string/
	],
	'ungrammatical database names still draw errors under --no-strict-names');

# Likewise for exclusion patterns
$node->command_checks_all(
	[ 'pg_amcheck', '--no-strict-names', '-T', 'a.b.c.d' ],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper relation name \(too many dotted names\): a\.b\.c\.d/
	],
	'ungrammatical table exclusions still draw errors under --no-strict-names'
);
$node->command_checks_all(
	[ 'pg_amcheck', '--no-strict-names', '-S', 'a.b.c' ],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper qualified name \(too many dotted names\): a\.b\.c/
	],
	'ungrammatical schema exclusions still draw errors under --no-strict-names'
);
$node->command_checks_all(
	[ 'pg_amcheck', '--no-strict-names', '-D', 'a.b' ],
	2,
	[qr/^$/],
	[
		qr/pg_amcheck: error: improper qualified name \(too many dotted names\): a\.b/
	],
	'ungrammatical database exclusions still draw errors under --no-strict-names'
);


#########################################
# Test checking non-existent databases, schemas, tables, and indexes

# Use --no-strict-names and a single existent table so we only get warnings
# about the failed pattern matches
$node->command_checks_all(
	[
		'pg_amcheck', '--no-strict-names',
		'-t',         'no_such_table',
		'-t',         'no*such*table',
		'-i',         'no_such_index',
		'-i',         'no*such*index',
		'-r',         'no_such_relation',
		'-r',         'no*such*relation',
		'-d',         'no_such_database',
		'-d',         'no*such*database',
		'-r',         'none.none',
		'-r',         'none.none.none',
		'-r',         'postgres.none.none',
		'-r',         'postgres.pg_catalog.none',
		'-r',         'postgres.none.pg_class',
		'-t',         'postgres.pg_catalog.pg_class',    # This exists
	],
	0,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: no heap tables to check matching "no_such_table"/,
		qr/pg_amcheck: warning: no heap tables to check matching "no\*such\*table"/,
		qr/pg_amcheck: warning: no btree indexes to check matching "no_such_index"/,
		qr/pg_amcheck: warning: no btree indexes to check matching "no\*such\*index"/,
		qr/pg_amcheck: warning: no relations to check matching "no_such_relation"/,
		qr/pg_amcheck: warning: no relations to check matching "no\*such\*relation"/,
		qr/pg_amcheck: warning: no heap tables to check matching "no\*such\*table"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "no_such_database"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "no\*such\*database"/,
		qr/pg_amcheck: warning: no relations to check matching "none\.none"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "none\.none\.none"/,
		qr/pg_amcheck: warning: no relations to check matching "postgres\.none\.none"/,
		qr/pg_amcheck: warning: no relations to check matching "postgres\.pg_catalog\.none"/,
		qr/pg_amcheck: warning: no relations to check matching "postgres\.none\.pg_class"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "no_such_database"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "no\*such\*database"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "none\.none\.none"/,
	],
	'many unmatched patterns and one matched pattern under --no-strict-names'
);


#########################################
# Test that an invalid / partially dropped database won't be targeted

$node->safe_psql(
	'postgres', q(
	CREATE DATABASE regression_invalid;
	UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'regression_invalid';
));

$node->command_checks_all(
	[
		'pg_amcheck', '-d', 'regression_invalid'
	],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: error: no connectable databases to check matching "regression_invalid"/,
	],
	'checking handling of invalid database');

$node->command_checks_all(
	[
	    'pg_amcheck', '-d', 'postgres',
		'-t', 'regression_invalid.public.foo',
	],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: error: no connectable databases to check matching "regression_invalid.public.foo"/,
	],
	'checking handling of object in invalid database');


#########################################
# Test checking otherwise existent objects but in databases where they do not exist

$node->safe_psql(
	'postgres', q(
	CREATE TABLE public.foo (f integer);
	CREATE INDEX foo_idx ON foo(f);
));
$node->safe_psql('postgres', q(CREATE DATABASE another_db));

$node->command_checks_all(
	[
		'pg_amcheck', '-d',
		'postgres',   '--no-strict-names',
		'-t',         'template1.public.foo',
		'-t',         'another_db.public.foo',
		'-t',         'no_such_database.public.foo',
		'-i',         'template1.public.foo_idx',
		'-i',         'another_db.public.foo_idx',
		'-i',         'no_such_database.public.foo_idx',
	],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
		qr/pg_amcheck: warning: no heap tables to check matching "template1\.public\.foo"/,
		qr/pg_amcheck: warning: no heap tables to check matching "another_db\.public\.foo"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "no_such_database\.public\.foo"/,
		qr/pg_amcheck: warning: no btree indexes to check matching "template1\.public\.foo_idx"/,
		qr/pg_amcheck: warning: no btree indexes to check matching "another_db\.public\.foo_idx"/,
		qr/pg_amcheck: warning: no connectable databases to check matching "no_such_database\.public\.foo_idx"/,
		qr/pg_amcheck: error: no relations to check/,
	],
	'checking otherwise existent objets in the wrong databases');


#########################################
# Test schema exclusion patterns

# Check with only schema exclusion patterns
$node->command_checks_all(
	[
		'pg_amcheck', '--all', '--no-strict-names', '-S',
		'public',     '-S',    'pg_catalog',        '-S',
		'pg_toast',   '-S',    'information_schema',
	],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
		qr/pg_amcheck: error: no relations to check/
	],
	'schema exclusion patterns exclude all relations');

# Check with schema exclusion patterns overriding relation and schema inclusion patterns
$node->command_checks_all(
	[
		'pg_amcheck',          '--all', '--no-strict-names',  '-s',
		'public',              '-s',    'pg_catalog',         '-s',
		'pg_toast',            '-s',    'information_schema', '-t',
		'pg_catalog.pg_class', '-S*'
	],
	1,
	[qr/^$/],
	[
		qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
		qr/pg_amcheck: error: no relations to check/
	],
	'schema exclusion pattern overrides all inclusion patterns');

done_testing();
