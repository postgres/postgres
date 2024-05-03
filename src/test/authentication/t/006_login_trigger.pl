# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests of authentication via login trigger. Mostly for rejection via
# exception, because this scenario cannot be covered with *.sql/*.out regress
# tests.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
if (!$use_unix_sockets)
{
	plan skip_all =>
	  "authentication tests cannot run without Unix-domain sockets";
}

# Execute a psql command and compare its output towards given regexps
sub psql_command
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($node, $sql, $expected_ret, $test_name, %params) = @_;

	my $connstr;
	if (defined($params{connstr}))
	{
		$connstr = $params{connstr};
	}
	else
	{
		$connstr = '';
	}

	# Execute command
	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', $sql, connstr => "$connstr");

	# Check return code
	is($ret, $expected_ret, "$test_name: exit code $expected_ret");

	# Check stdout
	if (defined($params{log_like}))
	{
		my @log_like = @{ $params{log_like} };
		while (my $regex = shift @log_like)
		{
			like($stdout, $regex, "$test_name: log matches");
		}
	}
	if (defined($params{log_unlike}))
	{
		my @log_unlike = @{ $params{log_unlike} };
		while (my $regex = shift @log_unlike)
		{
			unlike($stdout, $regex, "$test_name: log unmatches");
		}
	}
	if (defined($params{log_exact}))
	{
		is($stdout, $params{log_exact}, "$test_name: log equals");
	}

	# Check stderr
	if (defined($params{err_like}))
	{
		my @err_like = @{ $params{err_like} };
		while (my $regex = shift @err_like)
		{
			like($stderr, $regex, "$test_name: err matches");
		}
	}
	if (defined($params{err_unlike}))
	{
		my @err_unlike = @{ $params{err_unlike} };
		while (my $regex = shift @err_unlike)
		{
			unlike($stderr, $regex, "$test_name: err unmatches");
		}
	}
	if (defined($params{err_exact}))
	{
		is($stderr, $params{err_exact}, "$test_name: err equals");
	}

	return;
}

# New node
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(extra => [ '--locale=C', '--encoding=UTF8' ]);
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
});
$node->start;

# Create temporary roles and log table
psql_command(
	$node, 'CREATE ROLE regress_alice WITH LOGIN;
CREATE ROLE regress_mallory WITH LOGIN;
CREATE TABLE user_logins(id serial, who text);
GRANT SELECT ON user_logins TO public;
', 0, 'create tmp objects',
	log_exact => '',
	err_exact => ''),
  ;

# Create login event function and trigger
psql_command(
	$node,
	'CREATE FUNCTION on_login_proc() RETURNS event_trigger AS $$
BEGIN
  INSERT INTO user_logins (who) VALUES (SESSION_USER);
  IF SESSION_USER = \'regress_mallory\' THEN
    RAISE EXCEPTION \'Hello %! You are NOT welcome here!\', SESSION_USER;
  END IF;
  RAISE NOTICE \'Hello %! You are welcome!\', SESSION_USER;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
', 0, 'create trigger function',
	log_exact => '',
	err_exact => '');

psql_command(
	$node,
	'CREATE EVENT TRIGGER on_login_trigger '
	  . 'ON login EXECUTE PROCEDURE on_login_proc();', 0,
	'create event trigger',
	log_exact => '',
	err_exact => '');
psql_command(
	$node, 'ALTER EVENT TRIGGER on_login_trigger ENABLE ALWAYS;', 0,
	'alter event trigger',
	log_exact => '',
	err_like => [qr/You are welcome/]);

# Check the two requests were logged via login trigger
psql_command(
	$node, 'SELECT COUNT(*) FROM user_logins;', 0, 'select count',
	log_exact => '2',
	err_like => [qr/You are welcome/]);

# Try to login as allowed Alice.  We don't check the Mallory login, because
# FATAL error could cause a timing-dependant panic of IPC::Run.
psql_command(
	$node, 'SELECT 1;', 0, 'try regress_alice',
	connstr => 'user=regress_alice',
	log_exact => '1',
	err_like => [qr/You are welcome/],
	err_unlike => [qr/You are NOT welcome/]);

# Check that Alice's login record is here
psql_command(
	$node, 'SELECT * FROM user_logins;', 0, 'select *',
	log_like => [qr/3\|regress_alice/],
	log_unlike => [qr/regress_mallory/],
	err_like => [qr/You are welcome/]);

# Check total number of successful logins so far
psql_command(
	$node, 'SELECT COUNT(*) FROM user_logins;', 0, 'select count',
	log_exact => '5',
	err_like => [qr/You are welcome/]);

# Cleanup the temporary stuff
psql_command(
	$node, 'DROP EVENT TRIGGER on_login_trigger;', 0,
	'drop event trigger',
	log_exact => '',
	err_like => [qr/You are welcome/]);
psql_command(
	$node, 'DROP TABLE user_logins;
DROP FUNCTION on_login_proc;
DROP ROLE regress_mallory;
DROP ROLE regress_alice;
', 0, 'cleanup',
	log_exact => '',
	err_exact => '');

done_testing();
