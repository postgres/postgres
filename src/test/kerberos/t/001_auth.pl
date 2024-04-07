
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Sets up a KDC and then runs a variety of tests to make sure that the
# GSSAPI/Kerberos authentication and encryption are working properly,
# that the options in pg_hba.conf and pg_ident.conf are handled correctly,
# that the server-side pg_stat_gssapi view reports what we expect to
# see for each test and that SYSTEM_USER returns what we expect to see.
#
# Also test that GSSAPI delegation is working properly and that those
# credentials can be used to make dblink / postgres_fdw connections.
#
# Since this requires setting up a full KDC, it doesn't make much sense
# to have multiple test scripts (since they'd have to also create their
# own KDC and that could cause race conditions or other problems)- so
# just add whatever other tests are needed to here.
#
# See the README for additional information.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Kerberos;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{with_gssapi} ne 'yes')
{
	plan skip_all => 'GSSAPI/Kerberos not supported by this build';
}
elsif (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bkerberos\b/)
{
	plan skip_all =>
	  'Potentially unsafe test GSSAPI/Kerberos not enabled in PG_TEST_EXTRA';
}

my $pgpass = "${PostgreSQL::Test::Utils::tmp_check}/.pgpass";

my $dbname = 'postgres';
my $username = 'test1';
my $application = '001_auth.pl';

# Construct a pgpass file to make sure we don't use it
append_to_file($pgpass, '*:*:*:*:abc123');

chmod 0600, $pgpass or die $!;

note "setting up Kerberos";

my $host = 'auth-test-localhost.postgresql.example.com';
my $hostaddr = '127.0.0.1';
my $realm = 'EXAMPLE.COM';

my $krb = PostgreSQL::Test::Kerberos->new($host, $hostaddr, $realm);

my $test1_password = 'secret1';
$krb->create_principal('test1', $test1_password);

note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
listen_addresses = '$hostaddr'
krb_server_keyfile = '$krb->{keytab}'
log_connections = on
lc_messages = 'C'
});
$node->start;

my $port = $node->port();

$node->safe_psql('postgres', 'CREATE USER test1;');
$node->safe_psql('postgres',
	"CREATE USER test2 WITH ENCRYPTED PASSWORD 'abc123';");
$node->safe_psql('postgres', 'CREATE EXTENSION postgres_fdw;');
$node->safe_psql('postgres', 'CREATE EXTENSION dblink;');
$node->safe_psql('postgres',
	"CREATE SERVER s1 FOREIGN DATA WRAPPER postgres_fdw OPTIONS (host '$host', hostaddr '$hostaddr', port '$port', dbname 'postgres');"
);
$node->safe_psql('postgres',
	"CREATE SERVER s2 FOREIGN DATA WRAPPER postgres_fdw OPTIONS (port '$port', dbname 'postgres', passfile '$pgpass');"
);

$node->safe_psql('postgres', 'GRANT USAGE ON FOREIGN SERVER s1 TO test1;');

$node->safe_psql('postgres',
	"CREATE USER MAPPING FOR test1 SERVER s1 OPTIONS (user 'test1');");
$node->safe_psql('postgres',
	"CREATE USER MAPPING FOR test1 SERVER s2 OPTIONS (user 'test2');");

$node->safe_psql('postgres', "CREATE TABLE t1 (c1 int);");
$node->safe_psql('postgres', "INSERT INTO t1 VALUES (1);");
$node->safe_psql('postgres',
	"CREATE FOREIGN TABLE tf1 (c1 int) SERVER s1 OPTIONS (schema_name 'public', table_name 't1');"
);
$node->safe_psql('postgres', "GRANT SELECT ON t1 TO test1;");
$node->safe_psql('postgres', "GRANT SELECT ON tf1 TO test1;");

$node->safe_psql('postgres',
	"CREATE FOREIGN TABLE tf2 (c1 int) SERVER s2 OPTIONS (schema_name 'public', table_name 't1');"
);
$node->safe_psql('postgres', "GRANT SELECT ON tf2 TO test1;");

# Set up a table for SYSTEM_USER parallel worker testing.
$node->safe_psql('postgres',
	"CREATE TABLE ids (id) AS SELECT 'gss:test1\@$realm' FROM generate_series(1, 10);"
);

$node->safe_psql('postgres', 'GRANT SELECT ON ids TO public;');

note "running tests";

# Test connection success or failure, and if success, that query returns true.
sub test_access
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $query, $expected_res, $gssencmode, $test_name,
		@expect_log_msgs)
	  = @_;

	# need to connect over TCP/IP for Kerberos
	my $connstr = $node->connstr('postgres')
	  . " user=$role host=$host hostaddr=$hostaddr $gssencmode";

	my %params = (sql => $query,);

	if (@expect_log_msgs)
	{
		# Match every message literally.
		my @regexes = map { qr/\Q$_\E/ } @expect_log_msgs;

		$params{log_like} = \@regexes;
	}

	if ($expected_res eq 0)
	{
		# The result is assumed to match "true", or "t", here.
		$params{expected_stdout} = qr/^t$/;

		$node->connect_ok($connstr, $test_name, %params);
	}
	else
	{
		$node->connect_fails($connstr, $test_name, %params);
	}
}

# As above, but test for an arbitrary query result.
sub test_query
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $query, $expected, $gssencmode, $test_name) = @_;

	# need to connect over TCP/IP for Kerberos
	my $connstr = $node->connstr('postgres')
	  . " user=$role host=$host hostaddr=$hostaddr $gssencmode";

	$node->connect_ok(
		$connstr, $test_name,
		sql => $query,
		expected_stdout => $expected);
	return;
}

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
local all test2 scram-sha-256
host all all $hostaddr/32 gss map=mymap
});
$node->restart;

test_access($node, 'test1', 'SELECT true', 2, '', 'fails without ticket');

$krb->create_ticket('test1', $test1_password);

test_access(
	$node,
	'test1',
	'SELECT true',
	2,
	'',
	'fails without mapping',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"no match in usermap \"mymap\" for user \"test1\"");

$node->append_conf('pg_ident.conf', qq{mymap  /^(.*)\@$realm\$  \\1});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'',
	'succeeds with mapping with default gssencmode and host hba, ticket not forwardable',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=prefer',
	'succeeds with GSS-encrypted access preferred with host hba, ticket not forwardable',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=require',
	'succeeds with GSS-encrypted access required with host hba, ticket not forwardable',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=prefer gssdelegation=1',
	'succeeds with GSS-encrypted access preferred with host hba and credentials not delegated even though asked for (ticket not forwardable)',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=require gssdelegation=1',
	'succeeds with GSS-encrypted access required with host hba and credentials not delegated even though asked for (ticket not forwardable)',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);


# Test that we can transport a reasonable amount of data.
test_query(
	$node,
	'test1',
	'SELECT * FROM generate_series(1, 100000);',
	qr/^1\n.*\n1024\n.*\n9999\n.*\n100000$/s,
	'gssencmode=require',
	'receiving 100K lines works');

test_query(
	$node,
	'test1',
	"CREATE TEMP TABLE mytab (f1 int primary key);\n"
	  . "COPY mytab FROM STDIN;\n"
	  . join("\n", (1 .. 100000))
	  . "\n\\.\n"
	  . "SELECT COUNT(*) FROM mytab;",
	qr/^100000$/s,
	'gssencmode=require',
	'sending 100K lines works');

# require_auth=gss succeeds if required.
$node->connect_ok(
	$node->connstr('postgres')
	  . " user=test1 host=$host hostaddr=$hostaddr gssencmode=disable require_auth=gss",
	"GSS authentication requested, works with non-encrypted GSS");
$node->connect_ok(
	$node->connstr('postgres')
	  . " user=test1 host=$host hostaddr=$hostaddr gssencmode=require require_auth=gss",
	"GSS authentication requested, works with encrypted GSS auth");

# require_auth=sspi fails if required.
$node->connect_fails(
	$node->connstr('postgres')
	  . " user=test1 host=$host hostaddr=$hostaddr gssencmode=disable require_auth=sspi",
	"SSPI authentication requested, fails with non-encrypted GSS",
	expected_stderr =>
	  qr/authentication method requirement "sspi" failed: server requested GSSAPI authentication/
);
$node->connect_fails(
	$node->connstr('postgres')
	  . " user=test1 host=$host hostaddr=$hostaddr gssencmode=require require_auth=sspi",
	"SSPI authentication requested, fails with encrypted GSS",
	expected_stderr =>
	  qr/authentication method requirement "sspi" failed: server did not complete authentication/
);

# Test that SYSTEM_USER works.
test_query($node, 'test1', 'SELECT SYSTEM_USER;',
	qr/^gss:test1\@$realm$/s, 'gssencmode=require', 'testing system_user');

# Test that SYSTEM_USER works with parallel workers.
test_query(
	$node,
	'test1', qq(
	SET min_parallel_table_scan_size TO 0;
	SET parallel_setup_cost TO 0;
	SET parallel_tuple_cost TO 0;
	SET max_parallel_workers_per_gather TO 2;
	SELECT bool_and(SYSTEM_USER = id) FROM ids;),
	qr/^t$/s,
	'gssencmode=require',
	'testing system_user with parallel workers');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
    local all test2 scram-sha-256
	hostgssenc all all $hostaddr/32 gss map=mymap
});

# Re-create the ticket, with the forwardable flag set
$krb->create_ticket('test1', $test1_password, forwardable => 1);

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=prefer gssdelegation=1',
	'succeeds with GSS-encrypted access preferred and hostgssenc hba and credentials not forwarded (server does not accept them, default)',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=require gssdelegation=1',
	'succeeds with GSS-encrypted access required and hostgssenc hba and credentials not forwarded (server does not accept them, default)',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);

$node->append_conf('postgresql.conf', qq{gss_accept_delegation=off});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=prefer gssdelegation=1',
	'succeeds with GSS-encrypted access preferred and hostgssenc hba and credentials not forwarded (server does not accept them, explicitly disabled)',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=require gssdelegation=1',
	'succeeds with GSS-encrypted access required and hostgssenc hba and credentials not forwarded (server does not accept them, explicitly disabled)',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);

$node->append_conf('postgresql.conf', qq{gss_accept_delegation=on});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND credentials_delegated from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=prefer gssdelegation=1',
	'succeeds with GSS-encrypted access preferred and hostgssenc hba and credentials forwarded',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=yes, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND credentials_delegated from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'gssencmode=require gssdelegation=1',
	'succeeds with GSS-encrypted access required and hostgssenc hba and credentials forwarded',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=yes, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=prefer',
	'succeeds with GSS-encrypted access preferred and hostgssenc hba and credentials not forwarded',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND NOT credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=require gssdelegation=0',
	'succeeds with GSS-encrypted access required and hostgssenc hba and credentials explicitly not forwarded',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=no, principal=test1\@$realm)"
);

my $psql_out = '';
my $psql_stderr = '';
my $psql_rc = '';

$psql_rc = $node->psql(
	'postgres',
	"SELECT * FROM dblink('user=test1 dbname=$dbname host=$host hostaddr=$hostaddr port=$port','select 1') as t1(c1 int);",
	connstr =>
	  "user=test1 host=$host hostaddr=$hostaddr gssencmode=require gssdelegation=0",
	stdout => \$psql_out,
	stderr => \$psql_stderr);
is($psql_rc, '3', 'dblink attempt fails without delegated credentials');
like(
	$psql_stderr,
	qr/password or GSSAPI delegated credentials required/,
	'dblink does not work without delegated credentials');
like($psql_out, qr/^$/, 'dblink does not work without delegated credentials');

$psql_out = '';
$psql_stderr = '';

$psql_rc = $node->psql(
	'postgres',
	"SELECT * FROM dblink('user=test2 dbname=$dbname port=$port passfile=$pgpass','select 1') as t1(c1 int);",
	connstr =>
	  "user=test1 host=$host hostaddr=$hostaddr gssencmode=require gssdelegation=0",
	stdout => \$psql_out,
	stderr => \$psql_stderr);
is($psql_rc, '3',
	'dblink does not work without delegated credentials and with passfile');
like(
	$psql_stderr,
	qr/password or GSSAPI delegated credentials required/,
	'dblink does not work without delegated credentials and with passfile');
like($psql_out, qr/^$/,
	'dblink does not work without delegated credentials and with passfile');

$psql_out = '';
$psql_stderr = '';

$psql_rc = $node->psql(
	'postgres',
	"TABLE tf1;",
	connstr =>
	  "user=test1 host=$host hostaddr=$hostaddr gssencmode=require gssdelegation=0",
	stdout => \$psql_out,
	stderr => \$psql_stderr);
is($psql_rc, '3', 'postgres_fdw does not work without delegated credentials');
like(
	$psql_stderr,
	qr/password or GSSAPI delegated credentials required/,
	'postgres_fdw does not work without delegated credentials');
like($psql_out, qr/^$/,
	'postgres_fdw does not work without delegated credentials');

$psql_out = '';
$psql_stderr = '';

$psql_rc = $node->psql(
	'postgres',
	"TABLE tf2;",
	connstr =>
	  "user=test1 host=$host hostaddr=$hostaddr gssencmode=require gssdelegation=0",
	stdout => \$psql_out,
	stderr => \$psql_stderr);
is($psql_rc, '3',
	'postgres_fdw does not work without delegated credentials and with passfile'
);
like(
	$psql_stderr,
	qr/password or GSSAPI delegated credentials required/,
	'postgres_fdw does not work without delegated credentials and with passfile'
);
like($psql_out, qr/^$/,
	'postgres_fdw does not work without delegated credentials and with passfile'
);

test_access($node, 'test1', 'SELECT true', 2, 'gssencmode=disable',
	'fails with GSS encryption disabled and hostgssenc hba');

# require_auth=gss succeeds if required.
$node->connect_ok(
	$node->connstr('postgres')
	  . " user=test1 host=$host hostaddr=$hostaddr gssencmode=require require_auth=gss",
	"GSS authentication requested, works with GSS encryption");
$node->connect_ok(
	$node->connstr('postgres')
	  . " user=test1 host=$host hostaddr=$hostaddr gssencmode=require require_auth=gss,scram-sha-256",
	"multiple authentication types requested, works with GSS encryption");

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
    local all test2 scram-sha-256
	hostnogssenc all all $hostaddr/32 gss map=mymap
});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND NOT encrypted AND credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=prefer gssdelegation=1',
	'succeeds with GSS-encrypted access preferred and hostnogssenc hba, but no encryption',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=no, delegated_credentials=yes, principal=test1\@$realm)"
);
test_access($node, 'test1', 'SELECT true', 2, 'gssencmode=require',
	'fails with GSS-encrypted access required and hostnogssenc hba');
test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND NOT encrypted AND credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssencmode=disable gssdelegation=1',
	'succeeds with GSS encryption disabled and hostnogssenc hba',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=no, delegated_credentials=yes, principal=test1\@$realm)"
);

test_query(
	$node,
	'test1',
	"SELECT * FROM dblink('user=test1 dbname=$dbname host=$host hostaddr=$hostaddr port=$port','select 1') as t1(c1 int);",
	qr/^1$/s,
	'gssencmode=prefer gssdelegation=1',
	'dblink works not-encrypted (server not configured to accept encrypted GSSAPI connections)'
);

test_query(
	$node,
	'test1',
	"TABLE tf1;",
	qr/^1$/s,
	'gssencmode=prefer gssdelegation=1',
	'postgres_fdw works not-encrypted (server not configured to accept encrypted GSSAPI connections)'
);

$psql_out = '';
$psql_stderr = '';

$psql_rc = $node->psql(
	'postgres',
	"SELECT * FROM dblink('user=test2 dbname=$dbname port=$port passfile=$pgpass','select 1') as t1(c1 int);",
	connstr =>
	  "user=test1 host=$host hostaddr=$hostaddr gssencmode=prefer gssdelegation=1",
	stdout => \$psql_out,
	stderr => \$psql_stderr);
is($psql_rc, '3',
	'dblink does not work with delegated credentials and with passfile');
like(
	$psql_stderr,
	qr/password or GSSAPI delegated credentials required/,
	'dblink does not work with delegated credentials and with passfile');
like($psql_out, qr/^$/,
	'dblink does not work with delegated credentials and with passfile');

$psql_out = '';
$psql_stderr = '';

$psql_rc = $node->psql(
	'postgres',
	"TABLE tf2;",
	connstr =>
	  "user=test1 host=$host hostaddr=$hostaddr gssencmode=prefer gssdelegation=1",
	stdout => \$psql_out,
	stderr => \$psql_stderr);
is($psql_rc, '3',
	'postgres_fdw does not work with delegated credentials and with passfile'
);
like(
	$psql_stderr,
	qr/password or GSSAPI delegated credentials required/,
	'postgres_fdw does not work with delegated credentials and with passfile'
);
like($psql_out, qr/^$/,
	'postgres_fdw does not work with delegated credentials and with passfile'
);

truncate($node->data_dir . '/pg_ident.conf', 0);
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
    local all test2 scram-sha-256
	host all all $hostaddr/32 gss include_realm=0
});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted AND credentials_delegated FROM pg_stat_gssapi WHERE pid = pg_backend_pid();',
	0,
	'gssdelegation=1',
	'succeeds with include_realm=0 and defaults',
	"connection authenticated: identity=\"test1\@$realm\" method=gss",
	"connection authorized: user=$username database=$dbname application_name=$application GSS (authenticated=yes, encrypted=yes, delegated_credentials=yes, principal=test1\@$realm)"
);

test_query(
	$node,
	'test1',
	"SELECT * FROM dblink('user=test1 dbname=$dbname host=$host hostaddr=$hostaddr port=$port password=1234','select 1') as t1(c1 int);",
	qr/^1$/s,
	'gssencmode=require gssdelegation=1',
	'dblink works encrypted');

test_query(
	$node, 'test1', "TABLE tf1;", qr/^1$/s,
	'gssencmode=require gssdelegation=1',
	'postgres_fdw works encrypted');

# Reset pg_hba.conf, and cause a usermap failure with an authentication
# that has passed.
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf',
	qq{
    local all test2 scram-sha-256
	host all all $hostaddr/32 gss include_realm=0 krb_realm=EXAMPLE.ORG
});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT true',
	2,
	'',
	'fails with wrong krb_realm, but still authenticates',
	"connection authenticated: identity=\"test1\@$realm\" method=gss");

done_testing();
