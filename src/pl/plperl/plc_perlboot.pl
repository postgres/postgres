#  src/pl/plperl/plc_perlboot.pl

use strict;

use vars qw(%_SHARED $_TD);

PostgreSQL::InServer::Util::bootstrap();

# globals

sub ::is_array_ref
{
	return ref($_[0]) =~ m/^(?:PostgreSQL::InServer::)?ARRAY$/;
}

sub ::encode_array_literal
{
	my ($arg, $delim) = @_;
	return $arg unless (::is_array_ref($arg));
	$delim = ', ' unless defined $delim;
	my $res = '';
	foreach my $elem (@$arg)
	{
		$res .= $delim if length $res;
		if (ref $elem)
		{
			$res .= ::encode_array_literal($elem, $delim);
		}
		elsif (defined $elem)
		{
			(my $str = $elem) =~ s/(["\\])/\\$1/g;
			$res .= qq("$str");
		}
		else
		{
			$res .= 'NULL';
		}
	}
	return qq({$res});
}

sub ::encode_array_constructor
{
	my $arg = shift;
	return ::quote_nullable($arg) unless ::is_array_ref($arg);
	my $res = join ", ",
	  map { (ref $_) ? ::encode_array_constructor($_) : ::quote_nullable($_) }
	  @$arg;
	return "ARRAY[$res]";
}

{
#<<< protect next line from perltidy so perlcritic annotation works
	package PostgreSQL::InServer;  ## no critic (RequireFilenameMatchesPackage)
#>>>
	use strict;
	use warnings;

	sub plperl_warn
	{
		(my $msg = shift) =~ s/\(eval \d+\) //g;
		chomp $msg;
		&::elog(&::WARNING, $msg);
		return;
	}
	$SIG{__WARN__} = \&plperl_warn;

	sub plperl_die
	{
		(my $msg = shift) =~ s/\(eval \d+\) //g;
		die $msg;
	}
	$SIG{__DIE__} = \&plperl_die;

	sub mkfuncsrc
	{
		my ($name, $imports, $prolog, $src) = @_;

		my $BEGIN = join "\n", map {
			my $names = $imports->{$_} || [];
			"$_->import(qw(@$names));"
		} sort keys %$imports;
		$BEGIN &&= "BEGIN { $BEGIN }";

		return qq[ package main; sub { $BEGIN $prolog $src } ];
	}

	sub mkfunc
	{
		## no critic (ProhibitNoStrict, ProhibitStringyEval);
		no strict;      # default to no strict for the eval
		no warnings;    # default to no warnings for the eval
		my $ret = eval(mkfuncsrc(@_));
		$@ =~ s/\(eval \d+\) //g if $@;
		return $ret;
		## use critic
	}

	1;
}

{

	package PostgreSQL::InServer::ARRAY;
	use strict;
	use warnings;

	use overload
	  '""'  => \&to_str,
	  '@{}' => \&to_arr;

	sub to_str
	{
		my $self = shift;
		return ::encode_typed_literal($self->{'array'}, $self->{'typeoid'});
	}

	sub to_arr
	{
		return shift->{'array'};
	}

	1;
}
