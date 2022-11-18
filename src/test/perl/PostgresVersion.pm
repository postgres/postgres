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

PostgresVersion encapsulates Postgres version numbers, providing parsing
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

	chomp $arg;

	# Accept standard formats, in case caller has handed us the output of a
	# postgres command line tool
	my $devel;
	($arg, $devel) = ($1, $2)
	  if (
		$arg =~ m!^                             # beginning of line
          (?:\(?PostgreSQL\)?\s)?         # ignore PostgreSQL marker
          (\d+(?:\.\d+)*)                 # version number, dotted notation
          (devel|(?:alpha|beta|rc)\d+)?   # dev marker - see version_stamp.pl
		 !x);

	# Split into an array
	my @numbers = split(/\./, $arg);

	# Treat development versions as having a minor/micro version one less than
	# the first released version of that branch.
	push @numbers, -1 if ($devel);

	$devel ||= "";

	return bless { str => "$arg$devel", num => \@numbers }, $class;
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
	my ($a, $b, $swapped) = @_;

	$b = __PACKAGE__->new($b) unless blessed($b);

	($a, $b) = ($b, $a) if $swapped;

	my ($an, $bn) = ($a->{num}, $b->{num});

	for (my $idx = 0;; $idx++)
	{
		return 0
		  if ($idx >= @$an && $idx >= @$bn);
		# treat a missing number as 0
		my ($anum, $bnum) = ($an->[$idx] || 0, $bn->[$idx] || 0);
		return $anum <=> $bnum
		  if ($anum <=> $bnum);
	}
}

# Render the version number using the saved string.
sub _stringify
{
	my $self = shift;
	return $self->{str};
}

1;
