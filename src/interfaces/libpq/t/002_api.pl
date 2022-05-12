# Copyright (c) 2022, PostgreSQL Global Development Group
use strict;
use warnings;

use PostgreSQL::Test::Utils;
use Test::More;

# Test PQsslAttribute(NULL, "library")
my ($out, $err) = run_command([ 'libpq_testclient', '--ssl' ]);

if ($ENV{with_ssl} eq 'openssl')
{
	is($out, 'OpenSSL', 'PQsslAttribute(NULL, "library") returns "OpenSSL"');
}
else
{
	is( $err,
		'SSL is not enabled',
		'PQsslAttribute(NULL, "library") returns NULL');
}

done_testing();
