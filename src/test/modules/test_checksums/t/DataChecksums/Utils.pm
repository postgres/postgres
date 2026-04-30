
# Copyright (c) 2026, PostgreSQL Global Development Group

=pod

=head1 NAME

DataChecksums::Utils - Utility functions for testing data checksums in a running cluster

=head1 SYNOPSIS

  use PostgreSQL::Test::Cluster;
  use DataChecksums::Utils qw( .. );

  # Create, and start, a new cluster
  my $node = PostgreSQL::Test::Cluster->new('primary');
  $node->init;
  $node->start;

  test_checksum_state($node, 'off');

  enable_data_checksums($node);

  wait_for_checksum_state($node, 'on');


=cut

package DataChecksums::Utils;

use strict;
use warnings FATAL => 'all';
use Exporter 'import';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(
  cointoss
  disable_data_checksums
  enable_data_checksums
  random_sleep
  stopmode
  test_checksum_state
  wait_for_checksum_state
);

=pod

=head1 METHODS

=over

=item test_checksum_state(node, state)

Test that the current value of the data checksum GUC in the server running
at B<node> matches B<state>.  If the values differ, a test failure is logged.
Returns True if the values match, otherwise False.

=cut

sub test_checksum_state
{
	my ($postgresnode, $state) = @_;

	my $result = $postgresnode->safe_psql('postgres',
		"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';"
	);
	is($result, $state,
			'ensure checksums are set to '
		  . $state . ' on '
		  . $postgresnode->name());
	return $result eq $state;
}

=item wait_for_checksum_state(node, state)

Test the value of the data checksum GUC in the server running at B<node>
repeatedly until it matches B<state> or times out.  Processing will run for
$PostgreSQL::Test::Utils::timeout_default seconds before timing out.  If the
values differ when the process times out, False is returned and a test failure
is logged, otherwise True.

=cut

sub wait_for_checksum_state
{
	my ($postgresnode, $state) = @_;

	my $res = $postgresnode->poll_query_until(
		'postgres',
		"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';",
		$state);
	is($res, 1,
			'ensure data checksums are transitioned to '
		  . $state . ' on '
		  . $postgresnode->name());
	return $res == 1;
}

=item enable_data_checksums($node, %params)

Function for enabling data checksums in the cluster running at B<node>.

=over

=item cost_delay

The B<cost_delay> to use when enabling data checksums, default is 0.

=item cost_limit

The B<cost_limit> to use when enabling data checksums, default is 100.

=item wait

If defined, the function will wait for the state defined in this parameter,
waiting timing out, before returning.  The function will wait for
$PostgreSQL::Test::Utils::timeout_default seconds before timing out.

=back

=cut

sub enable_data_checksums
{
	my $postgresnode = shift;
	my %params = @_;

	# Set sane defaults for the parameters
	$params{cost_delay} = 0 unless (defined($params{cost_delay}));
	$params{cost_limit} = 100 unless (defined($params{cost_limit}));

	my $query = <<'EOQ';
SELECT pg_enable_data_checksums(%s, %s);
EOQ

	$postgresnode->safe_psql('postgres',
		sprintf($query, $params{cost_delay}, $params{cost_limit}));

	if (defined($params{wait}))
	{
		wait_for_checksum_state($postgresnode, $params{wait});
		# If we are tasked with waiting for an end state, also wait for the
		# launcher to exit.
		if ($params{wait} eq 'on' || $params{wait} eq 'off')
		{
			$postgresnode->poll_query_until('postgres',
					"SELECT count(*) = 0 "
				  . "FROM pg_catalog.pg_stat_activity "
				  . "WHERE backend_type = 'datachecksum launcher';");
		}
	}
}

=item disable_data_checksums($node, %params)

Function for disabling data checksums in the cluster running at B<node>.

=over

=item wait

If defined, the function will wait for the state to turn to B<off>, or
waiting timing out, before returning.  The function will wait for
$PostgreSQL::Test::Utils::timeout_default seconds before timing out.
Unlike in C<enable_data_checksums> the value of the parameter is discarded.

=back

=cut

sub disable_data_checksums
{
	my $postgresnode = shift;
	my %params = @_;

	$postgresnode->safe_psql('postgres',
		'SELECT pg_disable_data_checksums();');

	if (defined($params{wait}))
	{
		wait_for_checksum_state($postgresnode, 'off');
		$postgresnode->poll_query_until('postgres',
				"SELECT count(*) = 0 "
			  . "FROM pg_catalog.pg_stat_activity "
			  . "WHERE backend_type = 'datachecksum launcher';");
	}
}

=item cointoss

Helper for retrieving a binary value with random distribution for deciding
whether to turn things off during testing.

=back

=cut

sub cointoss
{
	return int(rand() < 0.5);
}

=item random_sleep(max)

Helper for injecting random sleeps here and there in the testrun. The sleep
duration will be in the range (0,B<max>), but won't be predictable in order to
avoid sleep patterns that manage to avoid race conditions and timing bugs.
The default B<max> is 3 seconds.

=back

=cut

sub random_sleep
{
	my $max = shift;
	return if (defined($max) && ($max == 0));
	sleep(int(rand(defined($max) ? $max : 3))) if cointoss;
}

=item stopmode

Small helper function for randomly selecting a valid stopmode.

=back

=cut

sub stopmode
{
	return 'immediate' if (cointoss);
	return 'fast';
}

=pod

=back

=cut

1;
