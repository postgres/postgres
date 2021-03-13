use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 72;

# Test set-up
my ($node, $port);
$node = get_new_node('test');
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
	1,
	[ qr/^$/ ],
	[ qr/FATAL:  database "qqq" does not exist/ ],
	'checking a non-existent database');

# Failing to resolve a database pattern is an error by default.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'qqq', '-d', 'postgres' ],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: error: no connectable databases to check matching "qqq"/ ],
	'checking an unresolvable database pattern');

# But only a warning under --no-strict-names
$node->command_checks_all(
	[ 'pg_amcheck', '--no-strict-names', '-d', 'qqq', '-d', 'postgres' ],
	0,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: no connectable databases to check matching "qqq"/ ],
	'checking an unresolvable database pattern under --no-strict-names');

# Check that a substring of an existent database name does not get interpreted
# as a matching pattern.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'post', '-d', 'postgres' ],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: error: no connectable databases to check matching "post"/ ],
	'checking an unresolvable database pattern (substring of existent database)');

# Check that a superstring of an existent database name does not get interpreted
# as a matching pattern.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgresql', '-d', 'postgres' ],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: error: no connectable databases to check matching "postgresql"/ ],
	'checking an unresolvable database pattern (superstring of existent database)');

#########################################
# Test connecting with a non-existent user

# Failing to connect to the initial database due to bad username is an error.
$node->command_checks_all(
	[ 'pg_amcheck', '-U', 'no_such_user', 'postgres' ],
	1,
	[ qr/^$/ ],
	[ ],
	'checking with a non-existent user');

#########################################
# Test checking databases without amcheck installed

# Attempting to check a database by name where amcheck is not installed should
# raise a warning.  If all databases are skipped, having no relations to check
# raises an error.
$node->command_checks_all(
	[ 'pg_amcheck', 'template1' ],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
	  qr/pg_amcheck: error: no relations to check/ ],
	'checking a database by name without amcheck installed, no other databases');

# Again, but this time with another database to check, so no error is raised.
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'template1', '-d', 'postgres' ],
	0,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/ ],
	'checking a database by name without amcheck installed, with other databases');

# Again, but by way of checking all databases
$node->command_checks_all(
	[ 'pg_amcheck', '--all' ],
	0,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/ ],
	'checking a database by pattern without amcheck installed, with other databases');

#########################################
# Test unreasonable patterns

# Check three-part unreasonable pattern that has zero-length names
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgres', '-t', '..' ],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: error: no connectable databases to check matching "\.\."/ ],
	'checking table pattern ".."');

# Again, but with non-trivial schema and relation parts
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgres', '-t', '.foo.bar' ],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: error: no connectable databases to check matching "\.foo\.bar"/ ],
	'checking table pattern ".foo.bar"');

# Check two-part unreasonable pattern that has zero-length names
$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgres', '-t', '.' ],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: error: no heap tables to check matching "\."/ ],
	'checking table pattern "."');

#########################################
# Test checking non-existent databases, schemas, tables, and indexes

# Use --no-strict-names and a single existent table so we only get warnings
# about the failed pattern matches
$node->command_checks_all(
	[ 'pg_amcheck', '--no-strict-names',
		'-t', 'no_such_table',
		'-t', 'no*such*table',
		'-i', 'no_such_index',
		'-i', 'no*such*index',
		'-r', 'no_such_relation',
		'-r', 'no*such*relation',
		'-d', 'no_such_database',
		'-d', 'no*such*database',
		'-r', 'none.none',
		'-r', 'none.none.none',
		'-r', 'this.is.a.really.long.dotted.string',
		'-r', 'postgres.none.none',
		'-r', 'postgres.long.dotted.string',
		'-r', 'postgres.pg_catalog.none',
		'-r', 'postgres.none.pg_class',
		'-t', 'postgres.pg_catalog.pg_class',	# This exists
	],
	0,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: no heap tables to check matching "no_such_table"/,
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
	  qr/pg_amcheck: warning: no connectable databases to check matching "this\.is\.a\.really\.long\.dotted\.string"/,
	  qr/pg_amcheck: warning: no relations to check matching "postgres\.none\.none"/,
	  qr/pg_amcheck: warning: no relations to check matching "postgres\.long\.dotted\.string"/,
	  qr/pg_amcheck: warning: no relations to check matching "postgres\.pg_catalog\.none"/,
	  qr/pg_amcheck: warning: no relations to check matching "postgres\.none\.pg_class"/,
	],
	'many unmatched patterns and one matched pattern under --no-strict-names');

#########################################
# Test checking otherwise existent objects but in databases where they do not exist

$node->safe_psql('postgres', q(
	CREATE TABLE public.foo (f integer);
	CREATE INDEX foo_idx ON foo(f);
));
$node->safe_psql('postgres', q(CREATE DATABASE another_db));

$node->command_checks_all(
	[ 'pg_amcheck', '-d', 'postgres', '--no-strict-names',
		'-t', 'template1.public.foo',
		'-t', 'another_db.public.foo',
		'-t', 'no_such_database.public.foo',
		'-i', 'template1.public.foo_idx',
		'-i', 'another_db.public.foo_idx',
		'-i', 'no_such_database.public.foo_idx',
	],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
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
	[ 'pg_amcheck', '--all', '--no-strict-names',
		'-S', 'public',
		'-S', 'pg_catalog',
		'-S', 'pg_toast',
		'-S', 'information_schema',
	],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
	  qr/pg_amcheck: error: no relations to check/ ],
	'schema exclusion patterns exclude all relations');

# Check with schema exclusion patterns overriding relation and schema inclusion patterns
$node->command_checks_all(
	[ 'pg_amcheck', '--all', '--no-strict-names',
		'-s', 'public',
		'-s', 'pg_catalog',
		'-s', 'pg_toast',
		'-s', 'information_schema',
		'-t', 'pg_catalog.pg_class',
		'-S*'
	],
	1,
	[ qr/^$/ ],
	[ qr/pg_amcheck: warning: skipping database "template1": amcheck is not installed/,
	  qr/pg_amcheck: error: no relations to check/ ],
	'schema exclusion pattern overrides all inclusion patterns');
