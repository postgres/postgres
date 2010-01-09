SPI::bootstrap();
use vars qw(%_SHARED);

sub ::plperl_warn {
	(my $msg = shift) =~ s/\(eval \d+\) //g;
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

sub ::_plperl_to_pg_array {
  my $arg = shift;
  ref $arg eq 'ARRAY' || return $arg;
  my $res = '';
  my $first = 1;
  foreach my $elem (@$arg) {
    $res .= ', ' unless $first; $first = undef;
    if (ref $elem) {
      $res .= _plperl_to_pg_array($elem);
    }
    elsif (defined($elem)) {
      my $str = qq($elem);
      $str =~ s/([\"\\])/\\$1/g;
      $res .= qq(\"$str\");
    }
    else {
      $res .= 'NULL' ;
    }
  }
  return qq({$res});
}
