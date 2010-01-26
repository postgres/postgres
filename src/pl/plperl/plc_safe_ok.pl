

#  $PostgreSQL: pgsql/src/pl/plperl/plc_safe_ok.pl,v 1.3 2010/01/26 23:11:56 adunstan Exp $

use strict;
use vars qw($PLContainer);

$PLContainer = new Safe('PLPerl');
$PLContainer->permit_only(':default');
$PLContainer->permit(qw[:base_math !:base_io sort time require]);

$PLContainer->share(qw[&elog &return_next
	&spi_query &spi_fetchrow &spi_cursor_close &spi_exec_query
	&spi_prepare &spi_exec_prepared &spi_query_prepared &spi_freeplan
	&DEBUG &LOG &INFO &NOTICE &WARNING &ERROR %_SHARED
	&quote_literal &quote_nullable &quote_ident
	&encode_bytea &decode_bytea
	&encode_array_literal &encode_array_constructor
	&looks_like_number
]);

# Load widely useful pragmas into the container to make them available.
# (Temporarily enable caller here as work around for bug in perl 5.10,
# which changed the way its Safe.pm works. It is quite safe, as caller is
# informational only.)
$PLContainer->permit(qw[caller]);
::safe_eval(q{
	require strict;
	require feature if $] >= 5.010000;
	1;
}) or die $@;
$PLContainer->deny(qw[caller]);

sub ::safe_eval {
	my $ret = $PLContainer->reval(shift);
	$@ =~ s/\(eval \d+\) //g if $@;
	return $ret;
}

sub ::mksafefunc {
	return ::safe_eval(::mkfuncsrc(@_));
}
