

#  $PostgreSQL: pgsql/src/pl/plperl/plc_safe_ok.pl,v 1.5 2010/02/16 21:39:52 adunstan Exp $

package PostgreSQL::InServer::safe;
 
use strict;
use warnings;
use Safe;

# @EvalInSafe    = ( [ "string to eval", "extra,opcodes,to,allow" ], ...)
# @ShareIntoSafe = ( [ from_class => \@symbols ], ...)

# these are currently declared "my" so they can't be monkeyed with using init
# code. If we later decide to change that policy, we could change one or more
# to make them visible by using "use vars".
my($PLContainer,$SafeClass,@EvalInSafe,@ShareIntoSafe);
  
# --- configuration ---

# ensure we only alter the configuration variables once to avoid any
# problems if this code is run multiple times due to an exception generated
# from plperl.on_trusted_init code leaving the interp_state unchanged.

if (not our $_init++) {

  # Load widely useful pragmas into the container to make them available.
  # These must be trusted to not expose a way to execute a string eval
  # or any kind of unsafe action that the untrusted code could exploit.
  # If in ANY doubt about a module then DO NOT add it to this list.

  unshift @EvalInSafe,
      [ 'require strict',   'caller' ],
      [ 'require Carp',     'caller,entertry'  ], # load Carp before warnings
      [ 'require warnings', 'caller'  ];
  push @EvalInSafe,
      [ 'require feature' ] if $] >= 5.010000;

  push @ShareIntoSafe, [
      main => [ qw(
          &elog &DEBUG &LOG &INFO &NOTICE &WARNING &ERROR
          &spi_query &spi_fetchrow &spi_cursor_close &spi_exec_query
          &spi_prepare &spi_exec_prepared &spi_query_prepared &spi_freeplan
          &return_next &_SHARED
          &quote_literal &quote_nullable &quote_ident
          &encode_bytea &decode_bytea &looks_like_number
          &encode_array_literal &encode_array_constructor
      ) ],
  ];
}

# --- create and initialize a new container ---

$SafeClass ||= 'Safe';
$PLContainer = $SafeClass->new('PostgreSQL::InServer::safe_container');

$PLContainer->permit_only(':default');
$PLContainer->permit(qw[:base_math !:base_io sort time require]);

for my $do (@EvalInSafe) {
  my $perform = sub { # private closure
      my ($container, $src, $ops) = @_;
      my $mask = $container->mask;
      $container->permit(split /\s*,\s*/, $ops);
      my $ok = safe_eval("$src; 1");
      $container->mask($mask);
      main::elog(main::ERROR(), "$src failed: $@") unless $ok;
  };
  
  my $ops = $do->[1] || '';
  # For old perls we add entereval if entertry is listed
  # due to http://rt.perl.org/rt3/Ticket/Display.html?id=70970
  # Testing with a recent perl (>=5.11.4) ensures this doesn't
  # allow any use of actual entereval (eval "...") opcodes.
  $ops = "entereval,$ops"
      if $] < 5.011004 and $ops =~ /\bentertry\b/;

  $perform->($PLContainer, $do->[0], $ops);
}

$PLContainer->share_from(@$_) for @ShareIntoSafe;


# --- runtime interface ---

# called directly for plperl.on_trusted_init and @EvalInSafe
sub safe_eval {
	my $ret = $PLContainer->reval(shift);
	$@ =~ s/\(eval \d+\) //g if $@;
	return $ret;
}

sub mksafefunc {
!   return safe_eval(PostgreSQL::InServer::mkfuncsrc(@_));
}
