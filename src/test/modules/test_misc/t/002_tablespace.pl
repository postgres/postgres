
# Copyright (c) 2024, PostgreSQL Global Development Group

# Simple tablespace tests that can't be replicated on the same host
# due to the use of absolute paths, so we keep them out of the regular
# regression tests.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Create a couple of directories to use as tablespaces.
my $basedir = $node->basedir();
my $TS1_LOCATION = "$basedir/ts1";
$TS1_LOCATION =~ s/\/\.\//\//g;    # collapse foo/./bar to foo/bar
mkdir($TS1_LOCATION);
my $TS2_LOCATION = "$basedir/ts2";
$TS2_LOCATION =~ s/\/\.\//\//g;
mkdir($TS2_LOCATION);

my $result;

# Create a tablespace with an absolute path
$result = $node->psql('postgres',
	"CREATE TABLESPACE regress_ts1 LOCATION '$TS1_LOCATION'");
ok($result == 0, 'create tablespace with absolute path');

# Can't create a tablespace where there is one already
$result = $node->psql('postgres',
	"CREATE TABLESPACE regress_ts1 LOCATION '$TS1_LOCATION'");
ok($result != 0, 'clobber tablespace with absolute path');

# Create table in it
$result = $node->psql('postgres', "CREATE TABLE t () TABLESPACE regress_ts1");
ok($result == 0, 'create table in tablespace with absolute path');

# Can't drop a tablespace that still has a table in it
$result = $node->psql('postgres', "DROP TABLESPACE regress_ts1");
ok($result != 0, 'drop tablespace with absolute path');

# Drop the table
$result = $node->psql('postgres', "DROP TABLE t");
ok($result == 0, 'drop table in tablespace with absolute path');

# Drop the tablespace
$result = $node->psql('postgres', "DROP TABLESPACE regress_ts1");
ok($result == 0, 'drop tablespace with absolute path');

# Create two absolute tablespaces and two in-place tablespaces, so we can
# testing various kinds of tablespace moves.
$result = $node->psql('postgres',
	"CREATE TABLESPACE regress_ts1 LOCATION '$TS1_LOCATION'");
ok($result == 0, 'create tablespace 1 with absolute path');
$result = $node->psql('postgres',
	"CREATE TABLESPACE regress_ts2 LOCATION '$TS2_LOCATION'");
ok($result == 0, 'create tablespace 2 with absolute path');
$result = $node->psql('postgres',
	"SET allow_in_place_tablespaces=on; CREATE TABLESPACE regress_ts3 LOCATION ''"
);
ok($result == 0, 'create tablespace 3 with in-place directory');
$result = $node->psql('postgres',
	"SET allow_in_place_tablespaces=on; CREATE TABLESPACE regress_ts4 LOCATION ''"
);
ok($result == 0, 'create tablespace 4 with in-place directory');

# Create a table and test moving between absolute and in-place tablespaces
$result = $node->psql('postgres', "CREATE TABLE t () TABLESPACE regress_ts1");
ok($result == 0, 'create table in tablespace 1');
$result = $node->psql('postgres', "ALTER TABLE t SET tablespace regress_ts2");
ok($result == 0, 'move table abs->abs');
$result = $node->psql('postgres', "ALTER TABLE t SET tablespace regress_ts3");
ok($result == 0, 'move table abs->in-place');
$result = $node->psql('postgres', "ALTER TABLE t SET tablespace regress_ts4");
ok($result == 0, 'move table in-place->in-place');
$result = $node->psql('postgres', "ALTER TABLE t SET tablespace regress_ts1");
ok($result == 0, 'move table in-place->abs');

# Drop everything
$result = $node->psql('postgres', "DROP TABLE t");
ok($result == 0, 'create table in tablespace 1');
$result = $node->psql('postgres', "DROP TABLESPACE regress_ts1");
ok($result == 0, 'drop tablespace 1');
$result = $node->psql('postgres', "DROP TABLESPACE regress_ts2");
ok($result == 0, 'drop tablespace 2');
$result = $node->psql('postgres', "DROP TABLESPACE regress_ts3");
ok($result == 0, 'drop tablespace 3');
$result = $node->psql('postgres', "DROP TABLESPACE regress_ts4");
ok($result == 0, 'drop tablespace 4');

$node->stop;

done_testing();
