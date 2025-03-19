
# Copyright (c) 2025, PostgreSQL Global Development Group

=pod

=head1 NAME

OAuth::Server - runs a mock OAuth authorization server for testing

=head1 SYNOPSIS

  use OAuth::Server;

  my $server = OAuth::Server->new();
  $server->run;

  my $port = $server->port;
  my $issuer = "http://127.0.0.1:$port";

  # test against $issuer...

  $server->stop;

=head1 DESCRIPTION

This is glue API between the Perl tests and the Python authorization server
daemon implemented in t/oauth_server.py. (Python has a fairly usable HTTP server
in its standard library, so the implementation was ported from Perl.)

This authorization server does not use TLS (it implements a nonstandard, unsafe
issuer at "http://127.0.0.1:<port>"), so libpq in particular will need to set
PGOAUTHDEBUG=UNSAFE to be able to talk to it.

=cut

package OAuth::Server;

use warnings;
use strict;
use Scalar::Util;
use Test::More;

=pod

=head1 METHODS

=over

=item OAuth::Server->new()

Create a new OAuth Server object.

=cut

sub new
{
	my $class = shift;

	my $self = {};
	bless($self, $class);

	return $self;
}

=pod

=item $server->port()

Returns the port in use by the server.

=cut

sub port
{
	my $self = shift;

	return $self->{'port'};
}

=pod

=item $server->run()

Runs the authorization server daemon in t/oauth_server.py.

=cut

sub run
{
	my $self = shift;
	my $port;

	my $pid = open(my $read_fh, "-|", $ENV{PYTHON}, "t/oauth_server.py")
	  or die "failed to start OAuth server: $!";

	# Get the port number from the daemon. It closes stdout afterwards; that way
	# we can slurp in the entire contents here rather than worrying about the
	# number of bytes to read.
	$port = do { local $/ = undef; <$read_fh> }
	  // die "failed to read port number: $!";
	chomp $port;
	die "server did not advertise a valid port"
	  unless Scalar::Util::looks_like_number($port);

	$self->{'pid'} = $pid;
	$self->{'port'} = $port;
	$self->{'child'} = $read_fh;

	note("OAuth provider (PID $pid) is listening on port $port\n");
}

=pod

=item $server->stop()

Sends SIGTERM to the authorization server and waits for it to exit.

=cut

sub stop
{
	my $self = shift;

	note("Sending SIGTERM to OAuth provider PID: $self->{'pid'}\n");

	kill(15, $self->{'pid'});
	$self->{'pid'} = undef;

	# Closing the popen() handle waits for the process to exit.
	close($self->{'child'});
	$self->{'child'} = undef;
}

=pod

=back

=cut

1;
