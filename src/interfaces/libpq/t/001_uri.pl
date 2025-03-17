# Copyright (c) 2021-2025, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;


# List of URIs tests. For each test the first element is the input string, the
# second the expected stdout and the third the expected stderr. Optionally,
# additional arguments may specify key/value pairs which will override
# environment variables for the duration of the test.
my @tests = (
	[
		q{postgresql://uri-user:secret@host:12345/db},
		q{user='uri-user' password='secret' dbname='db' host='host' port='12345' (inet)},
		q{},
	],
	[
		q{postgresql://uri-user@host:12345/db},
		q{user='uri-user' dbname='db' host='host' port='12345' (inet)}, q{},
	],
	[
		q{postgresql://uri-user@host/db},
		q{user='uri-user' dbname='db' host='host' (inet)}, q{},
	],
	[
		q{postgresql://host:12345/db},
		q{dbname='db' host='host' port='12345' (inet)}, q{},
	],
	[ q{postgresql://host/db}, q{dbname='db' host='host' (inet)}, q{}, ],
	[
		q{postgresql://uri-user@host:12345/},
		q{user='uri-user' host='host' port='12345' (inet)},
		q{},
	],
	[
		q{postgresql://uri-user@host/},
		q{user='uri-user' host='host' (inet)},
		q{},
	],
	[ q{postgresql://uri-user@}, q{user='uri-user' (local)}, q{}, ],
	[ q{postgresql://host:12345/}, q{host='host' port='12345' (inet)}, q{}, ],
	[ q{postgresql://host:12345}, q{host='host' port='12345' (inet)}, q{}, ],
	[ q{postgresql://host/db}, q{dbname='db' host='host' (inet)}, q{}, ],
	[ q{postgresql://host/}, q{host='host' (inet)}, q{}, ],
	[ q{postgresql://host}, q{host='host' (inet)}, q{}, ],
	[ q{postgresql://}, q{(local)}, q{}, ],
	[
		q{postgresql://?hostaddr=127.0.0.1}, q{hostaddr='127.0.0.1' (inet)},
		q{},
	],
	[
		q{postgresql://example.com?hostaddr=63.1.2.4},
		q{host='example.com' hostaddr='63.1.2.4' (inet)},
		q{},
	],
	[ q{postgresql://%68ost/}, q{host='host' (inet)}, q{}, ],
	[
		q{postgresql://host/db?user=uri-user},
		q{user='uri-user' dbname='db' host='host' (inet)},
		q{},
	],
	[
		q{postgresql://host/db?user=uri-user&port=12345},
		q{user='uri-user' dbname='db' host='host' port='12345' (inet)},
		q{},
	],
	[
		q{postgresql://host/db?u%73er=someotheruser&port=12345},
		q{user='someotheruser' dbname='db' host='host' port='12345' (inet)},
		q{},
	],
	[
		q{postgresql://host/db?u%7aer=someotheruser&port=12345}, q{},
		q{libpq_uri_regress: invalid URI query parameter: "uzer"},
	],
	[
		q{postgresql://host:12345?user=uri-user},
		q{user='uri-user' host='host' port='12345' (inet)},
		q{},
	],
	[
		q{postgresql://host?user=uri-user},
		q{user='uri-user' host='host' (inet)},
		q{},
	],
	[
		# Leading and trailing spaces, works.
		q{postgresql://host?  user = uri-user & port  = 12345 },
		q{user='uri-user' host='host' port='12345' (inet)},
		q{},
	],
	[
		# Trailing data in parameter.
		q{postgresql://host?  user user  =  uri  & port = 12345 12 },
		q{},
		q{libpq_uri_regress: unexpected spaces found in "  user user  ", use percent-encoded spaces (%20) instead},
	],
	[
		# Trailing data in value.
		q{postgresql://host?  user  =  uri-user  & port = 12345 12 },
		q{},
		q{libpq_uri_regress: unexpected spaces found in " 12345 12 ", use percent-encoded spaces (%20) instead},
	],
	[ q{postgresql://host?}, q{host='host' (inet)}, q{}, ],
	[
		q{postgresql://[::1]:12345/db},
		q{dbname='db' host='::1' port='12345' (inet)},
		q{},
	],
	[ q{postgresql://[::1]/db}, q{dbname='db' host='::1' (inet)}, q{}, ],
	[
		q{postgresql://[2001:db8::1234]/}, q{host='2001:db8::1234' (inet)},
		q{},
	],
	[
		q{postgresql://[200z:db8::1234]/}, q{host='200z:db8::1234' (inet)},
		q{},
	],
	[ q{postgresql://[::1]}, q{host='::1' (inet)}, q{}, ],
	[ q{postgres://}, q{(local)}, q{}, ],
	[ q{postgres:///}, q{(local)}, q{}, ],
	[ q{postgres:///db}, q{dbname='db' (local)}, q{}, ],
	[
		q{postgres://uri-user@/db}, q{user='uri-user' dbname='db' (local)},
		q{},
	],
	[
		q{postgres://?host=/path/to/socket/dir},
		q{host='/path/to/socket/dir' (local)},
		q{},
	],
	[
		q{postgresql://host?uzer=}, q{},
		q{libpq_uri_regress: invalid URI query parameter: "uzer"},
	],
	[
		q{postgre://},
		q{},
		q{libpq_uri_regress: missing "=" after "postgre://" in connection info string},
	],
	[
		q{postgres://[::1},
		q{},
		q{libpq_uri_regress: end of string reached when looking for matching "]" in IPv6 host address in URI: "postgres://[::1"},
	],
	[
		q{postgres://[]},
		q{},
		q{libpq_uri_regress: IPv6 host address may not be empty in URI: "postgres://[]"},
	],
	[
		q{postgres://[::1]z},
		q{},
		q{libpq_uri_regress: unexpected character "z" at position 17 in URI (expected ":" or "/"): "postgres://[::1]z"},
	],
	[
		q{postgresql://host?zzz},
		q{},
		q{libpq_uri_regress: missing key/value separator "=" in URI query parameter: "zzz"},
	],
	[
		q{postgresql://host?value1&value2},
		q{},
		q{libpq_uri_regress: missing key/value separator "=" in URI query parameter: "value1"},
	],
	[
		q{postgresql://host?key=key=value},
		q{},
		q{libpq_uri_regress: extra key/value separator "=" in URI query parameter: "key"},
	],
	[
		q{postgres://host?dbname=%XXfoo}, q{},
		q{libpq_uri_regress: invalid percent-encoded token: "%XXfoo"},
	],
	[
		q{postgresql://a%00b},
		q{},
		q{libpq_uri_regress: forbidden value %00 in percent-encoded value: "a%00b"},
	],
	[
		q{postgresql://%zz}, q{},
		q{libpq_uri_regress: invalid percent-encoded token: "%zz"},
	],
	[
		q{postgresql://%1}, q{},
		q{libpq_uri_regress: invalid percent-encoded token: "%1"},
	],
	[
		q{postgresql://%}, q{},
		q{libpq_uri_regress: invalid percent-encoded token: "%"},
	],
	[ q{postgres://@host}, q{host='host' (inet)}, q{}, ],
	[ q{postgres://host:/}, q{host='host' (inet)}, q{}, ],
	[ q{postgres://:12345/}, q{port='12345' (local)}, q{}, ],
	[
		q{postgres://otheruser@?host=/no/such/directory},
		q{user='otheruser' host='/no/such/directory' (local)},
		q{},
	],
	[
		q{postgres://otheruser@/?host=/no/such/directory},
		q{user='otheruser' host='/no/such/directory' (local)},
		q{},
	],
	[
		q{postgres://otheruser@:12345?host=/no/such/socket/path},
		q{user='otheruser' host='/no/such/socket/path' port='12345' (local)},
		q{},
	],
	[
		q{postgres://otheruser@:12345/db?host=/path/to/socket},
		q{user='otheruser' dbname='db' host='/path/to/socket' port='12345' (local)},
		q{},
	],
	[
		q{postgres://:12345/db?host=/path/to/socket},
		q{dbname='db' host='/path/to/socket' port='12345' (local)},
		q{},
	],
	[
		q{postgres://:12345?host=/path/to/socket},
		q{host='/path/to/socket' port='12345' (local)},
		q{},
	],
	[
		q{postgres://%2Fvar%2Flib%2Fpostgresql/dbname},
		q{dbname='dbname' host='/var/lib/postgresql' (local)},
		q{},
	],
	# Usually the default sslmode is 'prefer' (for libraries with SSL) or
	# 'disable' (for those without). This default changes to 'verify-full' if
	# the system CA store is in use.
	[
		q{postgresql://host?sslmode=disable},
		q{host='host' sslmode='disable' (inet)},
		q{},
		PGSSLROOTCERT => "system",
	],
	[
		q{postgresql://host?sslmode=prefer},
		q{host='host' sslmode='prefer' (inet)},
		q{},
		PGSSLROOTCERT => "system",
	],
	[
		q{postgresql://host?sslmode=verify-full},
		q{host='host' (inet)},
		q{}, PGSSLROOTCERT => "system",
	]);

# test to run for each of the above test definitions
sub test_uri
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	local %ENV = %ENV;

	my $uri;
	my %expect;
	my %envvars;
	my %result;

	($uri, $expect{stdout}, $expect{stderr}, %envvars) = @$_;

	$expect{'exit'} = $expect{stderr} eq '';
	%ENV = (%ENV, %envvars);

	my $cmd = [ 'libpq_uri_regress', $uri ];
	$result{exit} = IPC::Run::run $cmd,
	  '>' => \$result{stdout},
	  '2>' => \$result{stderr};

	chomp($result{stdout});
	chomp($result{stderr});

	# use is_deeply so there's one test result for each test above, without
	# losing the information whether stdout/stderr mismatched.
	is_deeply(\%result, \%expect, $uri);
}

foreach (@tests)
{
	test_uri($_);
}

done_testing();
