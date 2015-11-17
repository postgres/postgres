# A simple 'tee' implementation, using perl tie.
#
# Whenever you print to the handle, it gets forwarded to a list of
# handles. The list of output filehandles is passed to the constructor.
#
# This is similar to IO::Tee, but only used for output. Only the PRINT
# method is currently implemented; that's all we need. We don't want to
# depend on IO::Tee just for this.

package SimpleTee;
use strict;

sub TIEHANDLE {
	my $self = shift;
	bless \@_, $self;
}

sub PRINT {
	my $self = shift;
	my $ok = 1;
	for my $fh (@$self) {
		print $fh @_ or $ok = 0;
		$fh->flush or $ok = 0;
	}
	return $ok;
}

1;
