
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

=pod

=head1 NAME

SSL::Backend::OpenSSL

=head1 SYNOPSIS

  use SSL::Backend::OpenSSL;

  my $backend = SSL::Backend::OpenSSL->new();

  $backend->init($pgdata);

=head1 DESCRIPTION

SSL::Backend::OpenSSL implements the library specific parts in SSL::Server
for a PostgreSQL cluster compiled against OpenSSL.

=cut

package SSL::Backend::OpenSSL;

use strict;
use warnings FATAL => 'all';
use File::Basename;
use File::Copy;

=pod

=head1 METHODS

=over

=item SSL::Backend::OpenSSL->new()

Create a new instance of the OpenSSL backend.

=cut

sub new
{
	my ($class) = @_;

	my $self = { _library => 'OpenSSL', key => {} };

	bless $self, $class;

	return $self;
}

=pod

=item $backend->init(pgdata)

Install certificates, keys and CRL files required to run the tests against an
OpenSSL backend.

=cut

sub init
{
	my ($self, $pgdata) = @_;

	# Install server certificates and keys into the cluster data directory.
	_copy_files("ssl/server-*.crt", $pgdata);
	_copy_files("ssl/server-*.key", $pgdata);
	chmod(0600, glob "$pgdata/server-*.key")
	  or die "failed to change permissions on server keys: $!";
	_copy_files("ssl/root+client_ca.crt", $pgdata);
	_copy_files("ssl/root_ca.crt", $pgdata);
	_copy_files("ssl/root+client.crl", $pgdata);
	mkdir("$pgdata/root+client-crldir")
	  or die "unable to create server CRL dir $pgdata/root+client-crldir: $!";
	_copy_files("ssl/root+client-crldir/*", "$pgdata/root+client-crldir/");

	# The client's private key must not be world-readable, so take a copy
	# of the key stored in the code tree and update its permissions.
	#
	# This changes to using keys stored in a temporary path for the rest of
	# the tests. To get the full path for inclusion in connection strings, the
	# %key hash can be interrogated.
	my $cert_tempdir = PostgreSQL::Test::Utils::tempdir();
	my @keys = (
		"client.key", "client-revoked.key",
		"client-der.key", "client-encrypted-pem.key",
		"client-encrypted-der.key", "client-dn.key",
		"client_ext.key", "client-long.key",
		"client-revoked-utf8.key");
	foreach my $keyfile (@keys)
	{
		copy("ssl/$keyfile", "$cert_tempdir/$keyfile")
		  or die
		  "couldn't copy ssl/$keyfile to $cert_tempdir/$keyfile for permissions change: $!";
		chmod 0600, "$cert_tempdir/$keyfile"
		  or die "failed to change permissions on $cert_tempdir/$keyfile: $!";
		$self->{key}->{$keyfile} = "$cert_tempdir/$keyfile";
		$self->{key}->{$keyfile} =~ s!\\!/!g
		  if $PostgreSQL::Test::Utils::windows_os;
	}

	# Also make a copy of client.key explicitly world-readable in order to be
	# able to test incorrect permissions.  We can't necessarily rely on the
	# file in the source tree having those permissions.
	copy("ssl/client.key", "$cert_tempdir/client_wrongperms.key")
	  or die
	  "couldn't copy ssl/client_key to $cert_tempdir/client_wrongperms.key for permission change: $!";
	chmod 0644, "$cert_tempdir/client_wrongperms.key"
	  or die
	  "failed to change permissions on $cert_tempdir/client_wrongperms.key: $!";
	$self->{key}->{'client_wrongperms.key'} =
	  "$cert_tempdir/client_wrongperms.key";
	$self->{key}->{'client_wrongperms.key'} =~ s!\\!/!g
	  if $PostgreSQL::Test::Utils::windows_os;
}

=pod

=item $backend->get_sslkey(key)

Get an 'sslkey' connection string parameter for the specified B<key> which has
the correct path for direct inclusion in a connection string.

=cut

sub get_sslkey
{
	my ($self, $keyfile) = @_;

	return " sslkey=$self->{key}->{$keyfile}";
}

=pod

=item $backend->set_server_cert(params)

Change the configuration to use given server cert, key and crl file(s). The
following parameters are supported:

=over

=item cafile => B<value>

The CA certificate file to use for the C<ssl_ca_file> GUC. If omitted it will
default to 'root+client_ca.crt'.

=item certfile => B<value>

The server certificate file to use for the C<ssl_cert_file> GUC.

=item keyfile => B<value>

The private key file to use for the C<ssl_key_file GUC>. If omitted it will
default to the B<certfile>.key.

=item crlfile => B<value>

The CRL file to use for the C<ssl_crl_file> GUC. If omitted it will default to
'root+client.crl'.

=item crldir => B<value>

The CRL directory to use for the C<ssl_crl_dir> GUC. If omitted,
C<no ssl_crl_dir> configuration parameter will be set.

=back

=cut

sub set_server_cert
{
	my ($self, $params) = @_;

	$params->{cafile} = 'root+client_ca' unless defined $params->{cafile};
	$params->{crlfile} = 'root+client.crl' unless defined $params->{crlfile};
	$params->{keyfile} = $params->{certfile}
	  unless defined $params->{keyfile};

	my $sslconf =
		"ssl_ca_file='$params->{cafile}.crt'\n"
	  . "ssl_cert_file='$params->{certfile}.crt'\n"
	  . "ssl_key_file='$params->{keyfile}.key'\n"
	  . "ssl_crl_file='$params->{crlfile}'\n";
	$sslconf .= "ssl_crl_dir='$params->{crldir}'\n"
	  if defined $params->{crldir};

	return $sslconf;
}

=pod

=item $backend->get_library()

Returns the name of the SSL library, in this case "OpenSSL".

=cut

sub get_library
{
	my ($self) = @_;

	return $self->{_library};
}

# Internal method for copying a set of files, taking into account wildcards
sub _copy_files
{
	my $orig = shift;
	my $dest = shift;

	my @orig_files = glob $orig;
	foreach my $orig_file (@orig_files)
	{
		my $base_file = basename($orig_file);
		copy($orig_file, "$dest/$base_file")
		  or die "Could not copy $orig_file to $dest";
	}
	return;
}

=pod

=back

=cut

1;
