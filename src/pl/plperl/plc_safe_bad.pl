use vars qw($PLContainer);

$PLContainer = new Safe('PLPerl');
$PLContainer->permit_only(':default');
$PLContainer->share(qw[&elog &ERROR]);

my $msg = 'trusted Perl functions disabled - please upgrade Perl Safe module to version 2.09 or later';
sub ::mksafefunc {
  return $PLContainer->reval(qq[sub { elog(ERROR,'$msg') }]);
}

sub ::mk_strict_safefunc {
  return $PLContainer->reval(qq[sub { elog(ERROR,'$msg') }]);
}

