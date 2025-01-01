
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# A simple 'tee' implementation, using perl tie.
#
# Whenever you print to the handle, it gets forwarded to a list of
# handles. The list of output filehandles is passed to the constructor.
#
# This is similar to IO::Tee, but only used for output. Only the PRINT
# method is currently implemented; that's all we need. We don't want to
# depend on IO::Tee just for this.

# The package is enhanced to add timestamp and elapsed time decorations to
# the log file traces sent through this interface from Test::More functions
# (ok, is, note, diag etc.). Elapsed time is shown as the time since the last
# log trace.

package PostgreSQL::Test::SimpleTee;
use strict;
use warnings FATAL => 'all';

use Time::HiRes qw(time);

my $last_time;

BEGIN { $last_time = time; }

sub _time_str
{
	my $tm = time;
	my $diff = $tm - $last_time;
	$last_time = $tm;
	my ($sec, $min, $hour) = localtime($tm);
	my $msec = int(1000 * ($tm - int($tm)));
	return sprintf("[%.2d:%.2d:%.2d.%.3d](%.3fs) ",
		$hour, $min, $sec, $msec, $diff);
}

sub TIEHANDLE
{
	my $self = shift;
	return bless \@_, $self;
}

sub PRINT
{
	my $self = shift;
	my $ok = 1;
	# The first file argument passed to tiehandle in PostgreSQL::Test::Utils is
	# the original stdout, which is what PROVE sees. Additional decorations
	# confuse it, so only put out the time string on files after the first.
	my $skip = 1;
	my $ts = _time_str;
	for my $fh (@$self)
	{
		print $fh ($skip ? "" : $ts), @_ or $ok = 0;
		$fh->flush or $ok = 0;
		$skip = 0;
	}
	return $ok;
}

1;
