
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# OVERVIEW
# --------
#
# Test negotiation of SSL and GSSAPI encryption
#
# We test all combinations of:
#
# - all the libpq client options that affect the protocol negotiations
#   (gssencmode, sslmode, sslnegotiation)
# - server accepting or rejecting the authentication due to
#   pg_hba.conf entries
# - SSL and GSS enabled/disabled in the server
#
# That's a lot of combinations, so we use a table-driven approach.
# Each combination is represented by a line in a table. The line lists
# the options specifying the test case, and an expected outcome. The
# expected outcome includes whether the connection succeeds or fails,
# and whether it uses SSL, GSS or no encryption. It also includes a
# condensed trace of what steps were taken during the negotiation.
# That can catch cases like useless retries, or if the encryption
# methods are attempted in wrong order, even when it doesn't affect
# the final outcome.
#
# TEST TABLE FORMAT
# -----------------
#
# Example of the test table format:
#
# # USER     GSSENCMODE  SSLMODE    EVENTS                     -> OUTCOME
# testuser   disable     allow      connect, authok            -> plain
# .          .           prefer     connect, sslaccept, authok -> ssl
# testuser   require     *          connect, gssreject         -> fail
#
# USER, GSSENCMODE and SSLMODE fields are the libpq 'user',
# 'gssencmode' and 'sslmode' options used in the test. As a shorthand,
# a single dot ('.') can be used in the USER, GSSENCMODE, and SSLMODE
# fields, to indicate "same as on previous line". A '*' can be used
# as a wildcard; it is expanded to mean all possible values of that
# field.
#
# The EVENTS field is a condensed trace of expected steps during the
# negotiation:
#
#  connect:   a TCP connection was established
#  reconnect: TCP connection was disconnected, and a new one was established
#  sslaccept: client requested SSL encryption and server accepted it
#  sslreject: client requested SSL encryption but server rejected it
#  gssaccept: client requested GSSAPI encryption and server accepted it
#  gssreject: client requested GSSAPI encryption but server rejected it
#  authok:    client sent startup packet and authentication was performed successfully
#  authfail:  client sent startup packet but server rejected the authentication
#
# The event trace can be used to verify that the client negotiated the
# connection properly in more detail than just by looking at the
# outcome. For example, if the client opens spurious extra TCP
# connections, that would show up in the EVENTS.
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

# Only run the GSSAPI tests when compiled with GSSAPI support and
# PG_TEST_EXTRA includes 'kerberos'
my $gss_supported = $ENV{with_gssapi} eq 'yes';
my $kerberos_enabled =
  $ENV{PG_TEST_EXTRA} && $ENV{PG_TEST_EXTRA} =~ /\bkerberos\b/;
my $ssl_supported = $ENV{with_ssl} eq 'openssl';

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

# Capturing the EVENTS that occur during tests requires these settings
log_connections = 'receipt,authentication,authorization'
log_disconnections = on
trace_connection_negotiation = on
lc_messages = 'C'
});
my $pgdata = $node->data_dir;

my $dbname = 'postgres';
my $username = 'enctest';
my $application = '001_negotiate_encryption.pl';

my $gssuser_password = 'secret1';

my $krb;

if ($gss_supported != 0 && $kerberos_enabled != 0)
{
	note "setting up Kerberos";

	my $realm = 'EXAMPLE.COM';
	$krb = PostgreSQL::Test::Kerberos->new($host, $hostaddr, $realm);
	$node->append_conf('postgresql.conf',
		"krb_server_keyfile = '$krb->{keytab}'\n");
}

if ($ssl_supported != 0)
{
	my $certdir = dirname(__FILE__) . "/../../../test/ssl/ssl";

	copy "$certdir/server-cn-only.crt", "$pgdata/server.crt"
	  || die "copying server.crt: $!";
	copy "$certdir/server-cn-only.key", "$pgdata/server.key"
	  || die "copying server.key: $!";
	chmod(0600, "$pgdata/server.key")
	  or die "failed to change permissions on server keys: $!";

	# Start with SSL disabled.
	$node->append_conf('postgresql.conf', "ssl = off\n");
}

$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
my $injection_points_supported = $node->check_extension('injection_points');

$node->safe_psql('postgres', 'CREATE USER localuser;');
$node->safe_psql('postgres', 'CREATE USER testuser;');
$node->safe_psql('postgres', 'CREATE USER ssluser;');
$node->safe_psql('postgres', 'CREATE USER nossluser;');
$node->safe_psql('postgres', 'CREATE USER gssuser;');
$node->safe_psql('postgres', 'CREATE USER nogssuser;');
if ($injection_points_supported != 0)
{
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
}

my $unixdir = $node->safe_psql('postgres', 'SHOW unix_socket_directories;');
chomp($unixdir);

# Helper function that returns the encryption method in use in the
# connection.
$node->safe_psql(
	'postgres', q{
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
open my $hba, '>', "$pgdata/pg_hba.conf" or die $!;
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
} if ($gss_supported != 0 && $kerberos_enabled != 0);
close $hba;
$node->reload;

# Ok, all prepared. Run the tests.

my @all_test_users =
  ('testuser', 'ssluser', 'nossluser', 'gssuser', 'nogssuser');
my @all_gssencmodes = ('disable', 'prefer', 'require');
my @all_sslmodes = ('disable', 'allow', 'prefer', 'require');
my @all_sslnegotiations = ('postgres', 'direct');

###
### Run tests with GSS and SSL disabled in the server
###
my $test_table;
if ($ssl_supported)
{
	$test_table = q{
# USER      GSSENCMODE   SSLMODE      SSLNEGOTIATION EVENTS                      -> OUTCOME
testuser    disable      disable      postgres       connect, authok             -> plain
.           .            allow        postgres       connect, authok             -> plain
.           .            prefer       postgres       connect, sslreject, authok  -> plain
.           .            require      postgres       connect, sslreject          -> fail
.           .            .            direct         connect, directsslreject    -> fail
.           prefer       disable      postgres       connect, authok             -> plain
.           .            allow        postgres       connect, authok             -> plain
.           .            prefer       postgres       connect, sslreject, authok  -> plain
.           .            require      postgres       connect, sslreject          -> fail
.           .            .            direct         connect, directsslreject    -> fail

# sslnegotiation=direct is not accepted unless sslmode=require or stronger
*           *            disable      direct         -     -> fail
*           *            allow        direct         -     -> fail
*           *            prefer       direct         -     -> fail
};
}
else
{
	# Compiled without SSL support
	$test_table = q{
# USER      GSSENCMODE   SSLMODE      SSLNEGOTIATION EVENTS                      -> OUTCOME
testuser    disable      disable      postgres       connect, authok             -> plain
.           .            allow        postgres       connect, authok             -> plain
.           .            prefer       postgres       connect, authok             -> plain
.           prefer       disable      postgres       connect, authok             -> plain
.           .            allow        postgres       connect, authok             -> plain
.           .            prefer       postgres       connect, authok             -> plain

# Without SSL support, sslmode=require and sslnegotiation=direct are
# not accepted at all
*           *            require      *              -     -> fail
*           *            *            direct         -     -> fail
	};
}

# All attempts with gssencmode=require fail without connecting because
# no credential cache has been configured in the client. (Or if GSS
# support is not compiled in, they will fail because of that.)
$test_table .= q{
testuser    require      *            *              - -> fail
};

note("Running tests with SSL and GSS disabled in the server");
test_matrix($node,
	['testuser'], \@all_gssencmodes, \@all_sslmodes, \@all_sslnegotiations,
	parse_table($test_table));


###
### Run tests with GSS disabled and SSL enabled in the server
###
SKIP:
{
	skip "SSL not supported by this build" if $ssl_supported == 0;

	$test_table = q{
# USER      GSSENCMODE   SSLMODE      SSLNEGOTIATION EVENTS                                          -> OUTCOME
testuser    disable      disable      postgres       connect, authok                                 -> plain
.           .            allow        postgres       connect, authok                                 -> plain
.           .            prefer       postgres       connect, sslaccept, authok                      -> ssl
.           .            require      postgres       connect, sslaccept, authok                      -> ssl
.           .            .            direct         connect, directsslaccept, authok                -> ssl
ssluser     .            disable      postgres       connect, authfail                               -> fail
.           .            allow        postgres       connect, authfail, reconnect, sslaccept, authok -> ssl
.           .            prefer       postgres       connect, sslaccept, authok                      -> ssl
.           .            require      postgres       connect, sslaccept, authok                      -> ssl
.           .            .            direct         connect, directsslaccept, authok                -> ssl
nossluser   .            disable      postgres       connect, authok                                 -> plain
.           .            allow        postgres       connect, authok                                 -> plain
.           .            prefer       postgres       connect, sslaccept, authfail, reconnect, authok -> plain
.           .            require      postgres       connect, sslaccept, authfail                    -> fail
.           .            require      direct         connect, directsslaccept, authfail              -> fail

# sslnegotiation=direct is not accepted unless sslmode=require or stronger
*           *            disable      direct         -     -> fail
*           *            allow        direct         -     -> fail
*           *            prefer       direct         -     -> fail
};

	# Enable SSL in the server
	$node->adjust_conf('postgresql.conf', 'ssl', 'on');
	$node->reload;

	note("Running tests with SSL enabled in server");
	test_matrix($node, [ 'testuser', 'ssluser', 'nossluser' ],
		['disable'], \@all_sslmodes, \@all_sslnegotiations,
		parse_table($test_table));

	if ($injection_points_supported != 0)
	{
		$node->safe_psql(
			'postgres',
			"SELECT injection_points_attach('backend-initialize', 'error');",
			connstr => "user=localuser host=$unixdir");
		connect_test(
			$node,
			"user=testuser sslmode=prefer",
			'connect, backenderror -> fail');
		$node->restart;

		$node->safe_psql(
			'postgres',
			"SELECT injection_points_attach('backend-initialize-v2-error', 'error');",
			connstr => "user=localuser host=$unixdir");
		connect_test(
			$node,
			"user=testuser sslmode=prefer",
			'connect, v2error -> fail');
		$node->restart;

		$node->safe_psql(
			'postgres',
			"SELECT injection_points_attach('backend-ssl-startup', 'error');",
			connstr => "user=localuser host=$unixdir");
		connect_test(
			$node,
			"user=testuser sslmode=prefer",
			'connect, sslaccept, backenderror, reconnect, authok -> plain');
		$node->restart;
	}

	# Disable SSL again
	$node->adjust_conf('postgresql.conf', 'ssl', 'off');
	$node->reload;
}

###
### Run tests with GSS enabled, SSL disabled in the server
###
SKIP:
{
	skip "GSSAPI/Kerberos not supported by this build" if $gss_supported == 0;
	skip "kerberos not enabled in PG_TEST_EXTRA" if $kerberos_enabled == 0;

	$krb->create_principal('gssuser', $gssuser_password);
	$krb->create_ticket('gssuser', $gssuser_password);

	$test_table = q{
# USER      GSSENCMODE   SSLMODE      SSLNEGOTIATION EVENTS                       -> OUTCOME
testuser    disable      disable      postgres       connect, authok              -> plain
.           .            allow        postgres       connect, authok              -> plain
.           .            prefer       postgres       connect, sslreject, authok   -> plain
.           .            require      postgres       connect, sslreject                -> fail
.           .            .            direct         connect, directsslreject          -> fail
.           prefer       *            postgres       connect, gssaccept, authok        -> gss
.           prefer       require      direct         connect, gssaccept, authok        -> gss
.           require      *            postgres       connect, gssaccept, authok        -> gss
.           .            require      direct         connect, gssaccept, authok        -> gss

gssuser     disable      disable      postgres       connect, authfail                  -> fail
.           .            allow        postgres       connect, authfail, reconnect, sslreject -> fail
.           .            prefer       postgres       connect, sslreject, authfail       -> fail
.           .            require      postgres       connect, sslreject                 -> fail
.           .            .            direct         connect, directsslreject           -> fail
.           prefer       *            postgres       connect, gssaccept, authok   -> gss
.           prefer       require      direct         connect, gssaccept, authok   -> gss
.           require      *            postgres       connect, gssaccept, authok   -> gss
.           .            require      direct         connect, gssaccept, authok   -> gss

nogssuser   disable      disable      postgres       connect, authok              -> plain
.           .            allow        postgres       connect, authok              -> plain
.           .            prefer       postgres       connect, sslreject, authok   -> plain
.           .            require      postgres       connect, sslreject                 -> fail
.           .            .            direct         connect, directsslreject           -> fail
.           prefer       disable      postgres       connect, gssaccept, authfail, reconnect, authok             -> plain
.           .            allow        postgres       connect, gssaccept, authfail, reconnect, authok             -> plain
.           .            prefer       postgres       connect, gssaccept, authfail, reconnect, sslreject, authok  -> plain
.           .            require      postgres       connect, gssaccept, authfail, reconnect, sslreject          -> fail
.           .            .            direct         connect, gssaccept, authfail, reconnect, directsslreject          -> fail
.           require      disable      postgres       connect, gssaccept, authfail -> fail
.           .            allow        postgres       connect, gssaccept, authfail -> fail
.           .            prefer       postgres       connect, gssaccept, authfail -> fail
.           .            require      postgres       connect, gssaccept, authfail -> fail   # If both GSSAPI and sslmode are required, and GSS is not available -> fail
.           .            .            direct         connect, gssaccept, authfail -> fail   # If both GSSAPI and sslmode are required, and GSS is not available -> fail

# sslnegotiation=direct is not accepted unless sslmode=require or stronger
*           *            disable      direct         -     -> fail
*           *            allow        direct         -     -> fail
*           *            prefer       direct         -     -> fail
	};

	# The expected events and outcomes above assume that SSL support
	# is enabled. When libpq is compiled without SSL support, all
	# attempts to connect with sslmode=require or
	# sslnegotiation=direct would fail immediately without even
	# connecting to the server. Skip those, because we tested them
	# earlier already.
	my ($sslmodes, $sslnegotiations);
	if ($ssl_supported != 0)
	{
		($sslmodes, $sslnegotiations) =
		  (\@all_sslmodes, \@all_sslnegotiations);
	}
	else
	{
		($sslmodes, $sslnegotiations) = (['disable'], ['postgres']);
	}

	note("Running tests with GSS enabled in server");
	test_matrix($node, [ 'testuser', 'gssuser', 'nogssuser' ],
		\@all_gssencmodes, $sslmodes, $sslnegotiations,
		parse_table($test_table));

	if ($injection_points_supported != 0)
	{
		$node->safe_psql(
			'postgres',
			"SELECT injection_points_attach('backend-initialize', 'error');",
			connstr => "user=localuser host=$unixdir");
		connect_test(
			$node,
			"user=testuser gssencmode=prefer sslmode=disable",
			'connect, backenderror, reconnect, backenderror -> fail');
		$node->restart;

		$node->safe_psql(
			'postgres',
			"SELECT injection_points_attach('backend-initialize-v2-error', 'error');",
			connstr => "user=localuser host=$unixdir");
		connect_test(
			$node,
			"user=testuser gssencmode=prefer sslmode=disable",
			'connect, v2error, reconnect, v2error -> fail');
		$node->restart;

		$node->safe_psql(
			'postgres',
			"SELECT injection_points_attach('backend-gssapi-startup', 'error');",
			connstr => "user=localuser host=$unixdir");
		connect_test(
			$node,
			"user=testuser gssencmode=prefer sslmode=disable",
			'connect, gssaccept, backenderror, reconnect, authok -> plain');
		$node->restart;
	}
}

###
### Tests with both GSS and SSL enabled in the server
###
SKIP:
{
	skip "SSL not supported by this build" if $ssl_supported == 0;
	skip "GSSAPI/Kerberos not supported by this build" if $gss_supported == 0;
	skip "kerberos not enabled in PG_TEST_EXTRA" if $kerberos_enabled == 0;

	# Sanity check that GSSAPI is still enabled from previous test.
	connect_test(
		$node,
		'user=testuser gssencmode=prefer sslmode=prefer',
		'connect, gssaccept, authok -> gss');

	# Enable SSL
	$node->adjust_conf('postgresql.conf', 'ssl', 'on');
	$node->reload;

	$test_table = q{
# USER      GSSENCMODE   SSLMODE      SSLNEGOTIATION EVENTS                       -> OUTCOME
testuser    disable      disable      postgres       connect, authok              -> plain
.           .            allow        postgres       connect, authok              -> plain
.           .            prefer       postgres       connect, sslaccept, authok   -> ssl
.           .            require      postgres       connect, sslaccept, authok   -> ssl
.           .            .            direct         connect, directsslaccept, authok   -> ssl
.           prefer       disable      postgres       connect, gssaccept, authok   -> gss
.           .            allow        postgres       connect, gssaccept, authok   -> gss
.           .            prefer       postgres       connect, gssaccept, authok   -> gss
.           .            require      postgres       connect, gssaccept, authok   -> gss     # If both GSS and SSL is possible, GSS is chosen over SSL, even if sslmode=require
.           .            .            direct         connect, gssaccept, authok   -> gss
.           require      disable      postgres       connect, gssaccept, authok   -> gss
.           .            allow        postgres       connect, gssaccept, authok   -> gss
.           .            prefer       postgres       connect, gssaccept, authok   -> gss
.           .            require      postgres       connect, gssaccept, authok   -> gss     # If both GSS and SSL is possible, GSS is chosen over SSL, even if sslmode=require
.           .            .            direct         connect, gssaccept, authok   -> gss

gssuser     disable      disable      postgres       connect, authfail            -> fail
.           .            allow        postgres       connect, authfail, reconnect, sslaccept, authfail -> fail
.           .            prefer       postgres       connect, sslaccept, authfail, reconnect, authfail -> fail
.           .            require      postgres       connect, sslaccept, authfail       -> fail
.           .            .            direct         connect, directsslaccept, authfail -> fail
.           prefer       disable      postgres       connect, gssaccept, authok   -> gss
.           .            allow        postgres       connect, gssaccept, authok   -> gss
.           .            prefer       postgres       connect, gssaccept, authok   -> gss
.           .            require      postgres       connect, gssaccept, authok   -> gss   # GSS is chosen over SSL, even though sslmode=require
.           .            .            direct         connect, gssaccept, authok   -> gss
.           require      disable      postgres       connect, gssaccept, authok   -> gss
.           .            allow        postgres       connect, gssaccept, authok   -> gss
.           .            prefer       postgres       connect, gssaccept, authok   -> gss
.           .            require      postgres       connect, gssaccept, authok   -> gss     # If both GSS and SSL is possible, GSS is chosen over SSL, even if sslmode=require
.           .            .            direct         connect, gssaccept, authok   -> gss

ssluser     disable      disable      postgres       connect, authfail            -> fail
.           .            allow        postgres       connect, authfail, reconnect, sslaccept, authok       -> ssl
.           .            prefer       postgres       connect, sslaccept, authok         -> ssl
.           .            require      postgres       connect, sslaccept, authok         -> ssl
.           .            .            direct         connect, directsslaccept, authok   -> ssl
.           prefer       disable      postgres       connect, gssaccept, authfail, reconnect, authfail -> fail
.           .            allow        postgres       connect, gssaccept, authfail, reconnect, authfail, reconnect, sslaccept, authok       -> ssl
.           .            prefer       postgres       connect, gssaccept, authfail, reconnect, sslaccept, authok       -> ssl
.           .            require      postgres       connect, gssaccept, authfail, reconnect, sslaccept, authok       -> ssl
.           .            .            direct         connect, gssaccept, authfail, reconnect, directsslaccept, authok -> ssl
.           require      disable      postgres       connect, gssaccept, authfail -> fail
.           .            allow        postgres       connect, gssaccept, authfail -> fail
.           .            prefer       postgres       connect, gssaccept, authfail -> fail
.           .            require      postgres       connect, gssaccept, authfail -> fail         # If both GSS and SSL are required, the sslmode=require is effectively ignored and GSS is required
.           .            .            direct         connect, gssaccept, authfail -> fail

nogssuser   disable      disable      postgres       connect, authok              -> plain
.           .            allow        postgres       connect, authok              -> plain
.           .            prefer       postgres       connect, sslaccept, authok   -> ssl
.           .            require      postgres       connect, sslaccept, authok   -> ssl
.           .            .            direct         connect, directsslaccept, authok   -> ssl
.           prefer       disable      postgres       connect, gssaccept, authfail, reconnect, authok              -> plain
.           .            allow        postgres       connect, gssaccept, authfail, reconnect, authok              -> plain
.           .            prefer       postgres       connect, gssaccept, authfail, reconnect, sslaccept, authok         -> ssl
.           .            require      postgres       connect, gssaccept, authfail, reconnect, sslaccept, authok         -> ssl
.           .            .            direct         connect, gssaccept, authfail, reconnect, directsslaccept, authok   -> ssl
.           require      disable      postgres       connect, gssaccept, authfail -> fail
.           .            allow        postgres       connect, gssaccept, authfail -> fail
.           .            prefer       postgres       connect, gssaccept, authfail -> fail
.           .            require      postgres       connect, gssaccept, authfail -> fail   # If both GSS and SSL are required, the sslmode=require is effectively ignored and GSS is required
.           .            .            direct         connect, gssaccept, authfail -> fail

nossluser   disable      disable      postgres       connect, authok              -> plain
.           .            allow        postgres       connect, authok              -> plain
.           .            prefer       postgres       connect, sslaccept, authfail, reconnect, authok       -> plain
.           .            require      postgres       connect, sslaccept, authfail       -> fail
.           .            .            direct         connect, directsslaccept, authfail -> fail
.           prefer       *            postgres       connect, gssaccept, authok   -> gss
.           .            require      direct         connect, gssaccept, authok   -> gss
.           require      *            postgres       connect, gssaccept, authok   -> gss
.           .            require      direct         connect, gssaccept, authok   -> gss

# sslnegotiation=direct is not accepted unless sslmode=require or stronger
*           *            disable      direct         -     -> fail
*           *            allow        direct         -     -> fail
*           *            prefer       direct         -     -> fail
	};

	note("Running tests with both GSS and SSL enabled in server");
	test_matrix(
		$node,
		[ 'testuser', 'gssuser', 'ssluser', 'nogssuser', 'nossluser' ],
		\@all_gssencmodes,
		\@all_sslmodes,
		\@all_sslnegotiations,
		parse_table($test_table));
}

###
### Test negotiation over unix domain sockets.
###
SKIP:
{
	skip "Unix domain sockets not supported" unless ($unixdir ne "");

	# libpq doesn't attempt SSL or GSSAPI over Unix domain
	# sockets. The server would reject them too.
	connect_test(
		$node,
		"user=localuser gssencmode=prefer sslmode=prefer host=$unixdir",
		'connect, authok -> plain');
	connect_test($node,
		"user=localuser gssencmode=require sslmode=prefer host=$unixdir",
		'- -> fail');
}

done_testing();


### Helper functions

# Test the cube of parameters: user, gssencmode, sslmode, and sslnegotiation
sub test_matrix
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($pg_node, $test_users, $gssencmodes, $sslmodes, $sslnegotiations,
		%expected)
	  = @_;

	foreach my $test_user (@{$test_users})
	{
		foreach my $gssencmode (@{$gssencmodes})
		{
			foreach my $client_mode (@{$sslmodes})
			{
				# sslnegotiation only makes a difference if SSL is enabled. This saves a few combinations.
				my ($key, $expected_events);
				foreach my $negotiation (@{$sslnegotiations})
				{
					$key = "$test_user $gssencmode $client_mode $negotiation";
					$expected_events = $expected{$key};
					if (!defined($expected_events))
					{
						$expected_events =
						  "<line missing from expected output table>";
					}
					connect_test(
						$pg_node,
						"user=$test_user gssencmode=$gssencmode sslmode=$client_mode sslnegotiation=$negotiation",
						$expected_events);
				}
			}
		}
	}
}

# Try to establish a connection to the server using libpq. Verify the
# negotiation events and outcome.
sub connect_test
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $connstr, $expected_events_and_outcome) = @_;

	my $test_name = " '$connstr' -> $expected_events_and_outcome";

	my $connstr_full = "";
	$connstr_full .= "dbname=postgres " unless $connstr =~ m/dbname=/;
	$connstr_full .= "host=$host hostaddr=$hostaddr "
	  unless $connstr =~ m/host=/;
	$connstr_full .= $connstr;

	# Get the current size of the logfile before running the test.
	# After the test, we can then check just the new lines that have
	# appeared. (This is the same approach that the $node->log_contains
	# function uses).
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
		extra_params => [
			'--no-password', '--command' => 'SELECT current_enc()',
		],
		connstr => "$connstr_full",
		on_error_stop => 0);

	my $outcome = $ret == 0 ? $stdout : 'fail';

	# Parse the EVENTS from the log file.
	my $log_contents =
	  PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_location);
	my @events = parse_log_events($log_contents);

	# Check that the events and outcome match the expected events and
	# outcome
	my $events_and_outcome = join(', ', @events) . " -> $outcome";
	is($events_and_outcome, $expected_events_and_outcome, $test_name)
	  or diag("$stderr");
}

# Parse a test table. See comment at top of the file for the format.
sub parse_table
{
	my ($table) = @_;
	my @lines = split /\n/, $table;

	my %expected;

	my ($user, $gssencmode, $sslmode, $sslnegotiation);
	foreach my $line (@lines)
	{

		# Trim comments
		$line =~ s/#.*$//;

		# Trim whitespace at beginning and end
		$line =~ s/^\s+//;
		$line =~ s/\s+$//;

		# Ignore empty lines (includes comment-only lines)
		next if $line eq '';

		$line =~ m/^(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S.*)\s*->\s*(\S+)\s*$/
		  or die "could not parse line \"$line\"";
		$user = $1 unless $1 eq ".";
		$gssencmode = $2 unless $2 eq ".";
		$sslmode = $3 unless $3 eq ".";
		$sslnegotiation = $4 unless $4 eq ".";

		# Normalize the whitespace in the "EVENTS -> OUTCOME" part
		my @events = split /,\s*/, $5;
		my $outcome = $6;
		my $events_str = join(', ', @events);
		$events_str =~ s/\s+$//;    # trim whitespace
		my $events_and_outcome = "$events_str -> $outcome";

		my %expanded =
		  expand_expected_line($user, $gssencmode, $sslmode, $sslnegotiation,
			$events_and_outcome);
		%expected = (%expected, %expanded);
	}
	return %expected;
}

# Expand wildcards on a test table line
sub expand_expected_line
{
	my ($user, $gssencmode, $sslmode, $sslnegotiation, $expected) = @_;

	my %result;
	if ($user eq '*')
	{
		foreach my $x (@all_test_users)
		{
			%result = (
				%result,
				expand_expected_line(
					$x, $gssencmode, $sslmode, $sslnegotiation, $expected));
		}
	}
	elsif ($gssencmode eq '*')
	{
		foreach my $x (@all_gssencmodes)
		{
			%result = (
				%result,
				expand_expected_line(
					$user, $x, $sslmode, $sslnegotiation, $expected));
		}
	}
	elsif ($sslmode eq '*')
	{
		foreach my $x (@all_sslmodes)
		{
			%result = (
				%result,
				expand_expected_line(
					$user, $gssencmode, $x, $sslnegotiation, $expected));
		}
	}
	elsif ($sslnegotiation eq '*')
	{
		foreach my $x (@all_sslnegotiations)
		{
			%result = (
				%result,
				expand_expected_line(
					$user, $gssencmode, $sslmode, $x, $expected));
		}
	}
	else
	{
		$result{"$user $gssencmode $sslmode $sslnegotiation"} = $expected;
	}
	return %result;
}

# Scrape the server log for the negotiation events that match the
# EVENTS field of the test tables.
sub parse_log_events
{
	my ($log_contents) = (@_);

	my @events = ();

	my @lines = split /\n/, $log_contents;
	foreach my $line (@lines)
	{
		push @events, "reconnect"
		  if $line =~ /connection received/ && scalar(@events) > 0;
		push @events, "connect"
		  if $line =~ /connection received/ && scalar(@events) == 0;
		push @events, "sslaccept" if $line =~ /SSLRequest accepted/;
		push @events, "sslreject" if $line =~ /SSLRequest rejected/;
		push @events, "directsslaccept"
		  if $line =~ /direct SSL connection accepted/;
		push @events, "directsslreject"
		  if $line =~ /direct SSL connection rejected/;
		push @events, "gssaccept" if $line =~ /GSSENCRequest accepted/;
		push @events, "gssreject" if $line =~ /GSSENCRequest rejected/;
		push @events, "authfail" if $line =~ /no pg_hba.conf entry/;
		push @events, "authok" if $line =~ /connection authenticated/;
		push @events, "backenderror"
		  if $line =~ /error triggered for injection point backend-/;
		push @events, "v2error"
		  if $line =~ /protocol version 2 error triggered/;
	}

	# No events at all is represented by "-"
	if (scalar @events == 0)
	{
		push @events, "-";
	}

	return @events;
}
