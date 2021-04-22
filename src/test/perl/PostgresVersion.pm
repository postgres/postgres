############################################################################
#
# PostgresVersion.pm
#
# Module encapsulating Postgres Version numbers
#
# Copyright (c) 2021, PostgreSQL Global Development Group
#
############################################################################

=pod

=head1 NAME

PostgresVersion - class representing PostgreSQL version numbers

=head1 SYNOPSIS

  use PostgresVersion;

  my $version = PostgresVersion->new($version_arg);

  # compare two versions
  my $bool = $version1 <= $version2;

  # or compare with a number
  $bool = $version < 12;

  # or with a string
  $bool = $version lt "13.1";

  # interpolate in a string
  my $stringyval = "version: $version";

=head1 DESCRIPTION

PostgresVersion encapsulated Postgres version numbers, providing parsing
of common version formats and comparison operations.

=cut

package PostgresVersion;

use strict;
use warnings;

use Scalar::Util qw(blessed);

use overload
  '<=>' => \&_version_cmp,
  'cmp' => \&_version_cmp,
  '""'  => \&_stringify;

=pod

=head1 METHODS

=over

=item PostgresVersion->new($version)

Create a new PostgresVersion instance.

The argument can be a number like 12, or a string like '12.2' or the output
of a Postgres command like `psql --version` or `pg_config --version`;

=back

=cut

sub new
{
	my $class = shift;
	my $arg   = shift;

	# Accept standard formats, in case caller has handed us the output of a
	# postgres command line tool
	$arg = $1
	  if ($arg =~ m/\(?PostgreSQL\)? (\d+(?:\.\d+)*(?:devel)?)/);

	# Split into an array
	my @result = split(/\./, $arg);

	# Treat development versions as having a minor/micro version one less than
	# the first released version of that branch.
	if ($result[$#result] =~ m/^(\d+)devel$/)
	{
		pop(@result);
		push(@result, $1, -1);
	}

	my $res = [@result];
	bless $res, $class;
	return $res;
}


# Routine which compares the _pg_version_array obtained for the two
# arguments and returns -1, 0, or 1, allowing comparison between two
# PostgresVersion objects or a PostgresVersion and a version string or number.
#
# If the second argument is not a blessed object we call the constructor
# to make one.
#
# Because we're overloading '<=>' and 'cmp' this function supplies us with
# all the comparison operators ('<' and friends, 'gt' and friends)
#
sub _version_cmp
{
	my ($a, $b) = @_;

	$b = __PACKAGE__->new($b) unless blessed($b);

	for (my $idx = 0;; $idx++)
	{
		return 0 unless (defined $a->[$idx] && defined $b->[$idx]);
		return $a->[$idx] <=> $b->[$idx]
		  if ($a->[$idx] <=> $b->[$idx]);
	}
}

# Render the version number in the standard "joined by dots" notation if
# interpolated into a string. Put back 'devel' if we previously turned it
# into a -1.
sub _stringify
{
	my $self     = shift;
	my @sections = @$self;
	if ($sections[-1] == -1)
	{
		pop @sections;
		$sections[-1] = "$sections[-1]devel";
	}
	return join('.', @sections);
}

1;
