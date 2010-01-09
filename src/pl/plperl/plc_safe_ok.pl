use vars qw($PLContainer);

$PLContainer = new Safe('PLPerl');
$PLContainer->permit_only(':default');
$PLContainer->permit(qw[:base_math !:base_io sort time]);

$PLContainer->share(qw[&elog &return_next
	&spi_query &spi_fetchrow &spi_cursor_close &spi_exec_query
	&spi_prepare &spi_exec_prepared &spi_query_prepared &spi_freeplan
	&_plperl_to_pg_array
	&DEBUG &LOG &INFO &NOTICE &WARNING &ERROR %_SHARED
]);

# Load strict into the container.
# The temporary enabling of the caller opcode here is to work around a
# bug in perl 5.10, which unkindly changed the way its Safe.pm works, without
# notice. It is quite safe, as caller is informational only, and in any case
# we only enable it while we load the 'strict' module.
$PLContainer->permit(qw[require caller]);
$PLContainer->reval('use strict;');
$PLContainer->deny(qw[require caller]);

sub ::mksafefunc {
	my $ret = $PLContainer->reval(qq[sub { $_[0] $_[1] }]);
	$@ =~ s/\(eval \d+\) //g if $@;
	return $ret;
}

sub ::mk_strict_safefunc {
	my $ret = $PLContainer->reval(qq[sub { BEGIN { strict->import(); } $_[0] $_[1] }]);
	$@ =~ s/\(eval \d+\) //g if $@;
	return $ret;
}
