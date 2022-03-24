
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Verify that pg_verifybackup handles hex-encoded filenames correctly.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
my $backup_path = $primary->backup_dir . '/test_encoding';
$primary->command_ok(
	[
		'pg_basebackup', '-D',
		$backup_path,    '--no-sync',
		'-cfast',        '--manifest-force-encode'
	],
	"backup ok with forced hex encoding");

my $manifest = slurp_file("$backup_path/backup_manifest");
my $count_of_encoded_path_in_manifest = (() = $manifest =~ /Encoded-Path/mig);
cmp_ok($count_of_encoded_path_in_manifest,
	'>', 100, "many paths are encoded in the manifest");

command_like(
	[ 'pg_verifybackup', '-s', $backup_path ],
	qr/backup successfully verified/,
	'backup with forced encoding verified');

done_testing();
