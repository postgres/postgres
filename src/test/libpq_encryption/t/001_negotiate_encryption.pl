
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# OVERVIEW
# --------
#
# Test negotiation of SSL and GSSAPI encryption
#
# We test all combinations of:
#
# - all the libpq client options that affect the protocol negotiations
#   (gssencmode, sslmode)
# - server accepting or rejecting the authentication due to
#   pg_hba.conf entries
# - SSL and GSS enabled/disabled in the server
#
# That's a lot of combinations, so we use a table-driven approach.
# Each combination is represented by a line in a table. The line lists
# the options specifying the test case, and an expected outcome. The
# expected outcome includes whether the connection succeeds or fails,
# and whether it uses SSL, GSS or no encryption.
#
# TEST TABLE FORMAT
# -----------------
#
# Example of the test table format:
#
# # USER     GSSENCMODE  SSLMODE    OUTCOME
# testuser   disable     allow      plain
# .          .           prefer     ssl
# testuser   require     *          fail
#
# USER, GSSENCMODE and SSLMODE fields are the libpq 'user',
# 'gssencmode' and 'sslmode' options used in the test. As a shorthand,
# a single dot ('.') can be used in the USER, GSSENCMODE, and SSLMODE
# fields, to indicate "same as on previous line". A '*' can be used
# as a wildcard; it is expanded to mean all possible values of that
# field.
#
# The OUTCOME field indicates the expected result of the test:
#
#  plain:     an unencrypted connection was established
#  ssl:       SSL connection was established
#  gss:       GSSAPI encrypted connection was established
#  fail:      the connection attempt failed
#
# Empty lines are ignored. '#' can be used to mark the rest of the
# line as a comment.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Kerberos;
use File::Basename;
use File::Copy;
use Test::More;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\blibpq_encryption\b/)
{
	plan skip_all =>
	  'Potentially unsafe test libpq_encryption not enabled in PG_TEST_EXTRA';
}

my $ssl_supported = $ENV{with_ssl} eq 'openssl';
my $gss_supported = $ENV{with_gssapi} eq 'yes';

###
### Prepare test server for GSSAPI and SSL authentication, with a few
### different test users and helper functions. We don't actually
### enable SSL and kerberos in the server yet, we will do that later.
###

my $host = 'enc-test-localhost.postgresql.example.com';
my $hostaddr = '127.0.0.1';
my $servercidr = '127.0.0.1/32';

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
listen_addresses = '$hostaddr'
log_connections = on
lc_messages = 'C'
});
my $pgdata = $node->data_dir;

my $dbname = 'postgres';
my $username = 'enctest';
my $application = '001_negotiate_encryption.pl';

my $gssuser_password = 'secret1';

my $krb;

if ($gss_supported != 0)
{
	note "setting up Kerberos";

	my $realm = 'EXAMPLE.COM';
	$krb = PostgreSQL::Test::Kerberos->new($host, $hostaddr, $realm);
	$node->append_conf('postgresql.conf', "krb_server_keyfile = '$krb->{keytab}'\n");
}

if ($ssl_supported != 0)
{
	my $certdir = dirname(__FILE__) . "/../../ssl/ssl";

	copy "$certdir/server-cn-only.crt", "$pgdata/server.crt"
	  || die "copying server.crt: $!";
	copy "$certdir/server-cn-only.key", "$pgdata/server.key"
	  || die "copying server.key: $!";
	chmod(0600, "$pgdata/server.key");

	# Start with SSL disabled.
	$node->append_conf('postgresql.conf', "ssl = off\n");
}

$node->start;

$node->safe_psql('postgres', 'CREATE USER localuser;');
$node->safe_psql('postgres', 'CREATE USER testuser;');
$node->safe_psql('postgres', 'CREATE USER ssluser;');
$node->safe_psql('postgres', 'CREATE USER nossluser;');
$node->safe_psql('postgres', 'CREATE USER gssuser;');
$node->safe_psql('postgres', 'CREATE USER nogssuser;');

my $unixdir = $node->safe_psql('postgres', 'SHOW unix_socket_directories;');
chomp($unixdir);

# Helper function that returns the encryption method in use in the
# connection.
$node->safe_psql('postgres', q{
CREATE FUNCTION current_enc() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
  ssl_in_use bool;
  gss_in_use bool;
BEGIN
  ssl_in_use = (SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid());
  gss_in_use = (SELECT encrypted FROM pg_stat_gssapi WHERE pid = pg_backend_pid());

  raise log 'ssl %  gss %', ssl_in_use, gss_in_use;

  IF ssl_in_use AND gss_in_use THEN
    RETURN 'ssl+gss';   -- shouldn't happen
  ELSIF ssl_in_use THEN
    RETURN 'ssl';
  ELSIF gss_in_use THEN
    RETURN 'gss';
  ELSE
    RETURN 'plain';
  END IF;
END;
$$;
});

# Only accept SSL connections from $servercidr. Our tests don't depend on this
# but seems best to keep it as narrow as possible for security reasons.
open my $hba, '>', "$pgdata/pg_hba.conf";
print $hba qq{
# TYPE        DATABASE        USER            ADDRESS                 METHOD             OPTIONS
local         postgres        localuser                               trust
host          postgres        testuser        $servercidr             trust
hostnossl     postgres        nossluser       $servercidr             trust
hostnogssenc  postgres        nogssuser       $servercidr             trust
};

print $hba qq{
hostssl       postgres        ssluser         $servercidr             trust
} if ($ssl_supported != 0);

print $hba qq{
hostgssenc    postgres        gssuser         $servercidr             trust
} if ($gss_supported != 0);
close $hba;
$node->reload;

# Ok, all prepared. Run the tests.

my @all_test_users = ('testuser', 'ssluser', 'nossluser', 'gssuser', 'nogssuser');
my @all_gssencmodes = ('disable', 'prefer', 'require');
my @all_sslmodes = ('disable', 'allow', 'prefer', 'require');

my $server_config = {
	server_ssl => 0,
	server_gss => 0,
};

###
### Run tests with GSS and SSL disabled in the server
###
my $test_table = q{
# USER      GSSENCMODE   SSLMODE      OUTCOME
testuser    disable      disable      plain
.           .            allow        plain
.           .            prefer       plain
.           .            require      fail
.           prefer       disable      plain
.           .            allow        plain
.           .            prefer       plain
.           .            require      fail

# All attempts with gssencmode=require fail because no credential
# cache has been configured in the client (and the server isn't
# configured for GSS either)
.           require      *            fail
};
note("Running tests with SSL and GSS disabled in the server");
test_matrix($node, $server_config,
			['testuser'],
			\@all_sslmodes, \@all_gssencmodes, parse_table($test_table));

###
### Run tests with GSS disabled and SSL enabled in the server
###
SKIP:
{
	skip "SSL not supported by this build" if $ssl_supported == 0;

	$test_table = q{
# USER      GSSENCMODE   SSLMODE      OUTCOME
testuser    disable      disable      plain
.           .            allow        plain
.           .            prefer       ssl
.           .            require      ssl
ssluser     .            disable      fail
.           .            allow        ssl
.           .            prefer       ssl
.           .            require      ssl
nossluser   .            disable      plain
.           .            allow        plain
.           .            prefer       plain
.           .            require      fail
};

	# Enable SSL in the server
	$node->adjust_conf('postgresql.conf', 'ssl', 'on');
	$node->reload;
	$server_config->{server_ssl} = 1;

	note("Running tests with SSL enabled in server");
	test_matrix($node, $server_config,
				['testuser', 'ssluser', 'nossluser'],
				\@all_sslmodes, ['disable'], parse_table($test_table));

	# Disable SSL again
	$node->adjust_conf('postgresql.conf', 'ssl', 'off');
	$node->reload;
	$server_config->{server_ssl} = 0;
}

###
### Run tests with GSS enabled, SSL disabled in the server
###
SKIP:
{
	skip "GSSAPI/Kerberos not supported by this build" if $gss_supported == 0;
	$test_table = q{
# USER      GSSENCMODE   SSLMODE      OUTCOME
testuser    disable      disable      plain
.           .            allow        plain
.           .            prefer       plain
.           .            require      fail
.           require      *            gss
.           prefer       *            gss

gssuser     disable      disable      fail
.           .            allow        fail
.           .            prefer       fail
.           .            require      fail
.           prefer       *            gss
.           require      *            gss

nogssuser   disable      disable      plain
.           .            allow        plain
.           .            prefer       plain
.           .            require      fail
.           prefer       disable      plain
.           .            allow        plain
.           .            prefer       plain
.           .            require      fail
.           require      *            fail
};

	# Sanity check that the connection fails when no kerberos ticket
	# is present in the client
	connect_test($node, 'user=testuser gssencmode=require sslmode=disable', 'fail');

	$krb->create_principal('gssuser', $gssuser_password);
	$krb->create_ticket('gssuser', $gssuser_password);
	$server_config->{server_gss} = 1;

	note("Running tests with GSS enabled in server");
	test_matrix($node, $server_config,
				['testuser', 'gssuser', 'nogssuser'],
				\@all_sslmodes, \@all_gssencmodes, parse_table($test_table));

	# Check that logs match the expected 'no pg_hba.conf entry' line, too, as
	# that is not tested by test_matrix.
	connect_test($node, 'user=nogssuser gssencmode=require sslmode=prefer', 'fail',
				 'no pg_hba.conf entry for host "127.0.0.1", user "nogssuser", database "postgres", GSS encryption');

	# With 'gssencmode=prefer', libpq will first negotiate GSSAPI
	# encryption, but the connection will fail because pg_hba.conf
	# forbids GSSAPI encryption for this user. It will then reconnect
	# with SSL, but the server doesn't support it, so it will continue
	# with no encryption.
	connect_test($node, 'user=nogssuser gssencmode=prefer sslmode=prefer', 'plain',
				 'no pg_hba.conf entry for host "127.0.0.1", user "nogssuser", database "postgres", GSS encryption');
}

###
### Tests with both GSS and SSL enabled in the server
###
SKIP:
{
	skip "GSSAPI/Kerberos or SSL not supported by this build" unless ($ssl_supported && $gss_supported);

	$test_table = q{
# USER      GSSENCMODE   SSLMODE      OUTCOME
testuser    disable      disable      plain
.           .            allow        plain
.           .            prefer       ssl
.           .            require      ssl
.           prefer       disable      gss
.           .            allow        gss
.           .            prefer       gss
.           .            require      gss         # If both GSS and SSL is possible, GSS is chosen over SSL, even if sslmode=require
.           require      disable      gss
.           .            allow        gss
.           .            prefer       gss
.           .            require      gss         # If both GSS and SSL is possible, GSS is chosen over SSL, even if sslmode=require

gssuser     disable      *            fail
.           prefer       *            gss
.           require      *            gss

ssluser     disable      disable      fail
.           .            allow        ssl
.           .            prefer       ssl
.           .            require      ssl
.           prefer       disable      fail
.           .            allow        ssl
.           .            prefer       ssl
.           .            require      ssl
.           require      disable      fail
.           .            allow        fail
.           .            prefer       fail
.           .            require      fail         # If both GSS and SSL are required, the sslmode=require is effectively ignored and GSS is required

nogssuser   disable      disable      plain
.           .            allow        plain
.           .            prefer       ssl
.           .            require      ssl
.           prefer       disable      plain
.           .            allow        plain
.           .            prefer       ssl
.           .            require      ssl
.           require      *            fail

nossluser   disable      disable      plain
.           .            allow        plain
.           .            prefer       plain
.           .            require      fail
.           prefer       *            gss
.           require      *            gss
};

	# Sanity check that GSSAPI is still enabled from previous test.
	connect_test($node, 'user=testuser gssencmode=prefer sslmode=prefer', 'gss');

	# Enable SSL
	$node->adjust_conf('postgresql.conf', 'ssl', 'on');
	$node->reload;
	$server_config->{server_ssl} = 1;

	note("Running tests with both GSS and SSL enabled in server");
	test_matrix($node, $server_config,
				['testuser', 'gssuser', 'ssluser', 'nogssuser', 'nossluser'],
				\@all_sslmodes, \@all_gssencmodes, parse_table($test_table));

	# Test case that server supports GSSAPI, but it's not allowed for
	# this user. Special cased because we check output
	connect_test($node, 'user=nogssuser gssencmode=require sslmode=prefer', 'fail',
				 'no pg_hba.conf entry for host "127.0.0.1", user "nogssuser", database "postgres", GSS encryption');

	# with 'gssencmode=prefer', libpq will first negotiate GSSAPI
	# encryption, but the connection will fail because pg_hba.conf
	# forbids GSSAPI encryption for this user. It will then reconnect
	# with SSL.
	connect_test($node, 'user=nogssuser gssencmode=prefer sslmode=prefer', 'ssl',
				 'no pg_hba.conf entry for host "127.0.0.1", user "nogssuser", database "postgres", GSS encryption');

	# Setting both gssencmode=require and sslmode=require fails if
	# GSSAPI is not available.
	connect_test($node, 'user=nogssuser gssencmode=require sslmode=require ', 'fail');
}

###
### Test negotiation over unix domain sockets.
###
SKIP:
{
	skip "Unix domain sockets not supported" unless ($unixdir ne "");

	connect_test($node, "user=localuser gssencmode=prefer sslmode=require host=$unixdir", 'plain');
	connect_test($node, "user=localuser gssencmode=require sslmode=prefer host=$unixdir", 'fail');
}

done_testing();


### Helper functions

# Test the cube of parameters: user, sslmode, and gssencmode
sub test_matrix
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($pg_node, $node_conf,
		$test_users, $ssl_modes, $gss_modes, %expected) = @_;

	foreach my $test_user (@{$test_users})
	{
		foreach my $gssencmode (@{$gss_modes})
		{
			foreach my $client_mode (@{$ssl_modes})
			{
				my %params = (
					server_ssl=>$node_conf->{server_ssl},
					server_gss=>$node_conf->{server_gss},
					user=>$test_user,
					gssencmode=>$gssencmode,
					sslmode=>$client_mode,
				);
				my $key = "$test_user $gssencmode $client_mode";
				my $res = $expected{$key};
				if (!defined $res) {
					$res = "<expected result missing>";
				}
				connect_test($pg_node, "user=$test_user gssencmode=$gssencmode sslmode=$client_mode", $res);
			}
		}
	}
}

sub connect_test
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $connstr, $expected_outcome, @expect_log_msgs)
	  = @_;

	my $test_name = " '$connstr' -> $expected_outcome";

	my $connstr_full = "";
	$connstr_full .= "dbname=postgres " unless $connstr =~ m/dbname=/;
	$connstr_full .= "host=$host hostaddr=$hostaddr " unless $connstr =~ m/host=/;
	$connstr_full .= $connstr;

	my $log_location = -s $node->logfile;

	# XXX: Pass command with -c, because I saw intermittent test
	# failures like this:
	#
	# ack Broken pipe: write( 13, 'SELECT current_enc()' ) at /usr/local/lib/perl5/site_perl/IPC/Run/IO.pm line 550.
	#
	# I think that happens if the connection fails before we write the
	# query to its stdin. This test gets a lot of connection failures
	# on purpose.
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		'',
		extra_params => ['-w', '-c', 'SELECT current_enc()'],
		connstr => "$connstr_full",
		on_error_stop => 0);

	my $outcome = $ret == 0 ? $stdout : 'fail';

	is($outcome, $expected_outcome, $test_name) or diag("$stderr");

	if (@expect_log_msgs)
	{
		# Match every message literally.
		my @regexes = map { qr/\Q$_\E/ } @expect_log_msgs;
		my %params = ();
		$params{log_like} = \@regexes;
		$node->log_check($test_name, $log_location, %params);
	}
}

sub parse_table
{
	my ($table) = @_;
	my @lines = split /\n/, $table;

	my %expected;

	my ($user, $gssencmode, $sslmode);
	foreach my $line (@lines) {

		# Trim comments
		$line =~ s/#.*$//;

		# Trim whitespace at beginning and end
		$line =~ s/^\s+//;
		$line =~ s/\s+$//;

		# Ignore empty lines (includes comment-only lines)
		next if $line eq '';

		my @cols = split /\s+/, $line;
		die "test table line \"$line\" has incorrect number of columns\n" if scalar(@cols) != 4 ;

		$user = $cols[0] unless $cols[0] eq ".";
		$gssencmode = $cols[1] unless $cols[1] eq ".";
		$sslmode = $cols[2] unless $cols[2] eq ".";
		my $outcome = $cols[3];

		my %expanded = expand_expected_line($user, $gssencmode, $sslmode, $outcome);
		%expected = (%expected, %expanded);
	}
	return %expected;
}

# Expand wildcards on a test table line
sub expand_expected_line
{
	my ($user, $gssencmode, $sslmode, $expected) = @_;

	my %result;
	if ($user eq '*') {
		foreach my $x (@all_test_users) {
			%result = (%result, expand_expected_line($x, $gssencmode, $sslmode, $expected));
		}
	} elsif ($gssencmode eq '*') {
		foreach my $x (@all_gssencmodes) {
			%result = (%result, expand_expected_line($user, $x, $sslmode, $expected));
		}
	} elsif ($sslmode eq '*') {
		foreach my $x (@all_sslmodes) {
			%result = (%result, expand_expected_line($user, $gssencmode, $x, $expected));
		}
	} else {
		$result{"$user $gssencmode $sslmode"} = $expected;
	}
	return %result;
}
