
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

=pod

=head1 NAME

SSL::Server - Class for setting up SSL in a PostgreSQL Cluster

=head1 SYNOPSIS

  use PostgreSQL::Test::Cluster;
  use SSL::Server;

  # Create a new cluster
  my $node = PostgreSQL::Test::Cluster->new('primary');

  # Initialize and start the new cluster
  $node->init;
  $node->start;

  # Initialize SSL Server functionality for the cluster
  my $ssl_server = SSL::Server->new();

  # Configure SSL on the newly formed cluster
  $server->configure_test_server_for_ssl($node, '127.0.0.1', '127.0.0.1/32', 'trust');

=head1 DESCRIPTION

SSL::Server configures an existing test cluster, for the SSL regression tests.

The server is configured as follows:

=over

=item * SSL enabled, with the server certificate specified by arguments to switch_server_cert function.

=item * reject non-SSL connections

=item * a database called trustdb that lets anyone in

=item * another database called certdb that uses certificate authentication, ie.  the client must present a valid certificate signed by the client CA

=back

The server is configured to only accept connections from localhost. If you
want to run the client from another host, you'll have to configure that
manually.

Note: Someone running these test could have key or certificate files in their
~/.postgresql/, which would interfere with the tests.  The way to override that
is to specify sslcert=invalid and/or sslrootcert=invalid if no actual
certificate is used for a particular test.  libpq will ignore specifications
that name nonexisting files.  (sslkey and sslcrl do not need to specified
explicitly because an invalid sslcert or sslrootcert, respectively, causes
those to be ignored.)

The SSL::Server module presents a SSL library abstraction to the test writer,
which in turn use modules in SSL::Backend which implements the SSL library
specific infrastructure. Currently only OpenSSL is supported.

=cut

package SSL::Server;

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use SSL::Backend::OpenSSL;

# Force SSL tests nodes to begin in TCP mode. They won't work in Unix Socket
# mode and this way they will find a port to run on in a more robust way.
# Use an INIT block so it runs after the BEGIN block in Utils.pm.

INIT { $PostgreSQL::Test::Utils::use_unix_sockets = 0; }

=pod

=head1 METHODS

=over

=item SSL::Server->new(flavor)

Create a new SSL Server object for configuring a PostgreSQL test cluster
node for accepting SSL connections using the with B<flavor> selected SSL
backend. If B<flavor> isn't set, the C<with_ssl> environment variable will
be used for selecting backend. Currently only C<openssl> is supported.

=cut

sub new
{
	my $class = shift;
	my $flavor = shift || $ENV{with_ssl};
	die "SSL flavor not defined" unless $flavor;
	my $self = {};
	bless $self, $class;
	if ($flavor =~ /\Aopenssl\z/i)
	{
		$self->{flavor} = 'openssl';
		$self->{backend} = SSL::Backend::OpenSSL->new();
	}
	else
	{
		die "SSL flavor $flavor unknown";
	}
	return $self;
}

=pod

=item sslkey(filename)

Return a C<sslkey> construct for the specified key for use in a connection
string.

=cut

sub sslkey
{
	my $self = shift;
	my $keyfile = shift;
	my $backend = $self->{backend};

	return $backend->get_sslkey($keyfile);
}

=pod

=item $server->configure_test_server_for_ssl(node, host, cidr, auth, params)

Configure the cluster specified by B<node> or listening on SSL connections.
The following databases will be created in the cluster: trustdb, certdb,
certdb_dn, certdb_dn_re, certdb_cn, verifydb. The following users will be
created in the cluster: ssltestuser, md5testuser, anotheruser, yetanotheruser.
If B<< $params{password} >> is set, it will be used as password for all users
with the password encoding B<< $params{password_enc} >> (except for md5testuser
which always have MD5).  Extensions defined in B<< @{$params{extension}} >>
will be created in all the above created databases. B<host> is used for
C<listen_addresses> and B<cidr> for configuring C<pg_hba.conf>.

=cut

sub configure_test_server_for_ssl
{
	my $self = shift;
	my ($node, $serverhost, $servercidr, $authmethod, %params) = @_;
	my $backend = $self->{backend};
	my $pgdata = $node->data_dir;

	my @databases = (
		'trustdb', 'certdb', 'certdb_dn', 'certdb_dn_re',
		'certdb_cn', 'verifydb');

	# Create test users and databases
	$node->psql('postgres', "CREATE USER ssltestuser");
	$node->psql('postgres', "CREATE USER md5testuser");
	$node->psql('postgres', "CREATE USER anotheruser");
	$node->psql('postgres', "CREATE USER yetanotheruser");

	foreach my $db (@databases)
	{
		$node->psql('postgres', "CREATE DATABASE $db");
	}

	# Update password of each user as needed.
	if (defined($params{password}))
	{
		die "Password encryption must be specified when password is set"
		  unless defined($params{password_enc});

		$node->psql('postgres',
			"SET password_encryption='$params{password_enc}'; ALTER USER ssltestuser PASSWORD '$params{password}';"
		);
		# A special user that always has an md5-encrypted password
		$node->psql('postgres',
			"SET password_encryption='md5'; ALTER USER md5testuser PASSWORD '$params{password}';"
		);
		$node->psql('postgres',
			"SET password_encryption='$params{password_enc}'; ALTER USER anotheruser PASSWORD '$params{password}';"
		);
	}

	# Create any extensions requested in the setup
	if (defined($params{extensions}))
	{
		foreach my $extension (@{ $params{extensions} })
		{
			foreach my $db (@databases)
			{
				$node->psql($db, "CREATE EXTENSION $extension CASCADE;");
			}
		}
	}

	# enable logging etc.
	$node->append_conf(
		'postgresql.conf', <<EOF
fsync=off
log_connections=all
log_hostname=on
listen_addresses='$serverhost'
log_statement=all
EOF
	);

	# enable SSL and set up server key
	$node->append_conf('postgresql.conf', "include 'sslconfig.conf'");

	# SSL configuration will be placed here
	open my $sslconf, '>', "$pgdata/sslconfig.conf" or die $!;
	close $sslconf;

	# Perform backend specific configuration
	$backend->init($pgdata);

	# Stop and restart server to load new listen_addresses.
	$node->restart;

	# Change pg_hba after restart because hostssl requires ssl=on
	_configure_hba_for_ssl($node, $servercidr, $authmethod);

	return;
}

=pod

=item $server->ssl_library()

Get the name of the currently used SSL backend.

=cut

sub ssl_library
{
	my $self = shift;
	my $backend = $self->{backend};

	return $backend->get_library();
}

=pod

=item $server->is_libressl()

Detect whether the currently used SSL backend is LibreSSL.
(Ideally we'd not need this hack, but presently we do.)

=cut

sub is_libressl
{
	my $self = shift;
	my $backend = $self->{backend};

	return $backend->library_is_libressl();
}

=pod

=item switch_server_cert(params)

Change the configuration to use the given set of certificate, key, ca and
CRL, and potentially reload the configuration by restarting the server so
that the configuration takes effect.  Restarting is the default, passing
B<< $params{restart} >> => 'no' opts out of it leaving the server running.
The following params are supported:

=over

=item cafile => B<value>

The CA certificate to use. Implementation is SSL backend specific.

=item certfile => B<value>

The certificate file to use. Implementation is SSL backend specific.

=item keyfile => B<value>

The private key file to use. Implementation is SSL backend specific.

=item crlfile => B<value>

The CRL file to use. Implementation is SSL backend specific.

=item crldir => B<value>

The CRL directory to use. Implementation is SSL backend specific.

=item passphrase_cmd => B<value>

The passphrase command to use. If not set, an empty passphrase command will
be set.

=item restart => B<value>

If set to 'no', the server won't be restarted after updating the settings.
If omitted, or any other value is passed, the server will be restarted before
returning.

=back

=cut

sub switch_server_cert
{
	my $self = shift;
	my $node = shift;
	my $backend = $self->{backend};
	my %params = @_;
	my $pgdata = $node->data_dir;

	ok(unlink($node->data_dir . '/sslconfig.conf'));
	$node->append_conf('sslconfig.conf', "ssl=on");
	$node->append_conf('sslconfig.conf', $backend->set_server_cert(\%params));
	# use lists of ECDH curves and cipher suites for syntax testing
	$node->append_conf('sslconfig.conf', 'ssl_groups=X25519:prime256v1:secp521r1');
	$node->append_conf('sslconfig.conf',
		'ssl_tls13_ciphers=TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256');

	$node->append_conf('sslconfig.conf',
		"ssl_passphrase_command='" . $params{passphrase_cmd} . "'")
	  if defined $params{passphrase_cmd};

	return if (defined($params{restart}) && $params{restart} eq 'no');

	$node->restart;
	return;
}


# Internal function for configuring pg_hba.conf for SSL connections.
sub _configure_hba_for_ssl
{
	my ($node, $servercidr, $authmethod) = @_;
	my $pgdata = $node->data_dir;

	# Only accept SSL connections from $servercidr. Our tests don't depend on this
	# but seems best to keep it as narrow as possible for security reasons.
	#
	# When connecting to certdb, also check the client certificate.
	ok(unlink($node->data_dir . '/pg_hba.conf'));
	$node->append_conf(
		'pg_hba.conf', <<EOF
# TYPE  DATABASE      USER            ADDRESS       METHOD         OPTIONS
hostssl trustdb       md5testuser     $servercidr   md5
hostssl trustdb       all             $servercidr   $authmethod
hostssl verifydb      ssltestuser     $servercidr   $authmethod    clientcert=verify-full
hostssl verifydb      anotheruser     $servercidr   $authmethod    clientcert=verify-full
hostssl verifydb      yetanotheruser  $servercidr   $authmethod    clientcert=verify-ca
hostssl certdb        all             $servercidr   cert
hostssl certdb_dn     all             $servercidr   cert clientname=DN map=dn
hostssl certdb_dn_re  all             $servercidr   cert clientname=DN map=dnre
hostssl certdb_cn     all             $servercidr   cert clientname=CN map=cn
EOF
	);

	# Also set the ident maps. Note: fields with commas must be quoted
	ok(unlink($node->data_dir . '/pg_ident.conf'));
	$node->append_conf(
		'pg_ident.conf', <<EOF
# MAPNAME SYSTEM-USERNAME                                         PG-USERNAME
dn        "CN=ssltestuser-dn,OU=Testing,OU=Engineering,O=PGDG"    ssltestuser
dnre      "/^.*OU=Testing,.*\$"                                   ssltestuser
cn        ssltestuser-dn                                          ssltestuser
EOF
	);
	return;
}

=pod

=back

=cut

1;
