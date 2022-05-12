
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

command_exit_is([ 'pg_ctl', 'status', '-D', "$tempdir/nonexistent" ],
	4, 'pg_ctl status with nonexistent directory');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

command_exit_is([ 'pg_ctl', 'status', '-D', $node->data_dir ],
	3, 'pg_ctl status with server not running');

system_or_bail 'pg_ctl', '-l', "$tempdir/logfile", '-D',
  $node->data_dir, '-w', 'start';
command_exit_is([ 'pg_ctl', 'status', '-D', $node->data_dir ],
	0, 'pg_ctl status with server running');

system_or_bail 'pg_ctl', 'stop', '-D', $node->data_dir;

done_testing();
