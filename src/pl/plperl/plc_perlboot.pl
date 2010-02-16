
#  $PostgreSQL: pgsql/src/pl/plperl/plc_perlboot.pl,v 1.5 2010/02/16 21:39:52 adunstan Exp $

use 5.008001;

PostgreSQL::InServer::Util::bootstrap();

package PostgreSQL::InServer;

use strict;
use warnings;
use vars qw(%_SHARED);

sub plperl_warn {
	(my $msg = shift) =~ s/\(eval \d+\) //g;
	chomp $msg;
	&::elog(&::WARNING, $msg);
}
$SIG{__WARN__} = \&plperl_warn;

sub plperl_die {
	(my $msg = shift) =~ s/\(eval \d+\) //g;
	die $msg;
}
$SIG{__DIE__} = \&plperl_die;

sub mkfuncsrc {
	my ($name, $imports, $prolog, $src) = @_;

	my $BEGIN = join "\n", map {
		my $names = $imports->{$_} || [];
		"$_->import(qw(@$names));"
	} sort keys %$imports;
	$BEGIN &&= "BEGIN { $BEGIN }";

	$name =~ s/\\/\\\\/g;
	$name =~ s/::|'/_/g; # avoid package delimiters

	return qq[ package main; undef *{'$name'}; *{'$name'} = sub { $BEGIN $prolog $src } ];
}

# see also mksafefunc() in plc_safe_ok.pl
sub mkunsafefunc {
	no strict; # default to no strict for the eval
	my $ret = eval(mkfuncsrc(@_));
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
	return ::quote_nullable($arg)
		if ref $arg ne 'ARRAY';
	my $res = join ", ", map {
		(ref $_) ? ::encode_array_constructor($_)
		         : ::quote_nullable($_)
	} @$arg;
	return "ARRAY[$res]";
}

