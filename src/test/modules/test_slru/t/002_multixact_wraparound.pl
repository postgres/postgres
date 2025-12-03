# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Test multixact wraparound

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_slru'");

# Set the cluster's next multitransaction close to wraparound
my $node_pgdata = $node->data_dir;
command_ok(
	[
		'pg_resetwal',
		'--multixact-ids' => '0xFFFFFFF8,0xFFFFFFF8',
		$node_pgdata
	],
	"set the cluster's next multitransaction to 0xFFFFFFF8");

# Fixup the SLRU files to match the state we reset to.

# initialize SLRU file with zeros (65536 entries * 4 bytes = 262144 bytes)
my $slru_file = "$node_pgdata/pg_multixact/offsets/FFFF";
open my $fh, ">", $slru_file
  or die "could not open \"$slru_file\": $!";
binmode $fh;
# Write 65536 entries of 4 bytes each (all zeros)
syswrite($fh, "\0" x 262144) == 262144
  or die "could not write to \"$slru_file\": $!";
close $fh;

# remove old SLRU file
unlink("$node_pgdata/pg_multixact/offsets/0000")
  or die "could not unlink \"$node_pgdata/pg_multixact/offsets/0000\": $!";

# Consume multixids to wrap around.  We start at 0xFFFFFFF8, so after
# creating 16 multixacts we should definitely have wrapped around.
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION test_slru));

my @multixact_ids;
foreach my $i (1 .. 16)
{
	my $multi =
	  $node->safe_psql('postgres', q{SELECT test_create_multixact();});
	push @multixact_ids, $multi;
}

# Verify that wraparound occurred (last_multi should be numerically
# smaller than first_multi)
my $first_multi = $multixact_ids[0];
my $last_multi = $multixact_ids[-1];
ok( $last_multi < $first_multi,
	"multixact wraparound occurred (first: $first_multi, last: $last_multi)");

# Verify that all the multixacts we created are readable
foreach my $i (0 .. $#multixact_ids)
{
	my $multi = $multixact_ids[$i];
	is( $node->safe_psql(
			'postgres', qq{SELECT test_read_multixact('$multi');}),
		'',
		"multixact $i (ID: $multi) is readable after wraparound");
}

done_testing();
