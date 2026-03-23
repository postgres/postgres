# Copyright (c) 2026, PostgreSQL Global Development Group

# Test all ranges of valid UTF-8 codepoints under SASLprep.
#
# This test is expensive and enabled with PG_TEST_EXTRA, which is
# why it exists as a TAP test.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bsaslprep\b/)
{
	plan skip_all => "test saslprep not enabled in PG_TEST_EXTRA";
}

# Initialize node
my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_saslprep;');

# Among all the valid UTF-8 codepoint ranges, our implementation of
# SASLprep should never return an empty password if the operation is
# considered a success.
my $result = $node->safe_psql(
	'postgres', qq[SELECT * FROM test_saslprep_ranges()
  WHERE status = 'SUCCESS' AND res IN (NULL, '')
]);

is($result, '', "valid codepoints returning an empty password");

$node->stop;
done_testing();
