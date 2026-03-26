# Copyright (c) 2024-2025, PostgreSQL Global Development Group

=pod

=head1 NAME

TestAio - helpers for writing AIO related tests

=cut

package TestAio;

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


=pod

=head1 METHODS

=over

=item TestAio::supported_io_methods()

Return an array of all the supported values for the io_method GUC

=cut

sub supported_io_methods()
{
	my @io_methods = ('worker');

	push(@io_methods, "io_uring") if have_io_uring();

	# Return sync last, as it will least commonly fail
	push(@io_methods, "sync");

	return @io_methods;
}


=item TestAio::configure()

Prepare a cluster for AIO test

=cut

sub configure
{
	my $node = shift;

	$node->append_conf(
		'postgresql.conf', qq(
shared_preload_libraries=test_aio
log_min_messages = 'DEBUG3'
log_statement=all
log_error_verbosity=default
restart_after_crash=false
temp_buffers=100
));

}


=pod

=item TestAio::have_io_uring()

Return if io_uring is supported

=cut

sub have_io_uring
{
	# To detect if io_uring is supported, we look at the error message for
	# assigning an invalid value to an enum GUC, which lists all the valid
	# options. We need to use -C to deal with running as administrator on
	# windows, the superuser check is omitted if -C is used.
	my ($stdout, $stderr) =
	  run_command [qw(postgres -C invalid -c io_method=invalid)];
	die "can't determine supported io_method values"
	  unless $stderr =~ m/Available values: ([^\.]+)\./;
	my $methods = $1;
	note "supported io_method values are: $methods";

	return ($methods =~ m/io_uring/) ? 1 : 0;
}

1;
