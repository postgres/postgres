
#  $PostgreSQL: pgsql/src/pl/plperl/plc_perlboot.pl,v 1.2 2010/01/20 01:08:21 adunstan Exp $

PostgreSQL::InServer::Util::bootstrap();
PostgreSQL::InServer::SPI::bootstrap();

use strict;
use warnings;
use vars qw(%_SHARED);

sub ::plperl_warn {
	(my $msg = shift) =~ s/\(eval \d+\) //g;
	chomp $msg;
	&elog(&NOTICE, $msg);
}
$SIG{__WARN__} = \&::plperl_warn;

sub ::plperl_die {
	(my $msg = shift) =~ s/\(eval \d+\) //g;
	die $msg;
}
$SIG{__DIE__} = \&::plperl_die;


sub ::mkunsafefunc {
	my $ret = eval(qq[ sub { $_[0] $_[1] } ]);
	$@ =~ s/\(eval \d+\) //g if $@;
	return $ret;
}
  
use strict;

sub ::mk_strict_unsafefunc {
	my $ret = eval(qq[ sub { use strict; $_[0] $_[1] } ]);
	$@ =~ s/\(eval \d+\) //g if $@;
	return $ret;
}

sub ::encode_array_literal {
	my ($arg, $delim) = @_;
	return $arg
		if ref $arg ne 'ARRAY';
	$delim = ', ' unless defined $delim;
	my $res = '';
	foreach my $elem (@$arg) {
		$res .= $delim if length $res;
		if (ref $elem) {
			$res .= ::encode_array_literal($elem, $delim);
		}
		elsif (defined $elem) {
			(my $str = $elem) =~ s/(["\\])/\\$1/g;
			$res .= qq("$str");
		}
		else {
			$res .= 'NULL';
		}
	}
	return qq({$res});
}

sub ::encode_array_constructor {
	my $arg = shift;
	return quote_nullable($arg)
		if ref $arg ne 'ARRAY';
	my $res = join ", ", map {
		(ref $_) ? ::encode_array_constructor($_)
				 : ::quote_nullable($_)
	} @$arg;
	return "ARRAY[$res]";
}

