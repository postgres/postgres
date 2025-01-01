
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

#  src/pl/plperl/plc_trusted.pl

#<<< protect next line from perltidy so perlcritic annotation works
package PostgreSQL::InServer::safe; ## no critic (RequireFilenameMatchesPackage)
#>>>

# Load widely useful pragmas into plperl to make them available.
#
# SECURITY RISKS:
#
# Since these modules are free to compile unsafe opcodes they must
# be trusted to now allow any code containing unsafe opcodes to be abused.
# That's much harder than it sounds.
#
# Be aware that perl provides a wide variety of ways to subvert
# pre-compiled code. For some examples, see this presentation:
# http://www.slideshare.net/cdman83/barely-legal-xxx-perl-presentation
#
# If in ANY doubt about a module, or ANY of the modules down the chain of
# dependencies it loads, then DO NOT add it to this list.
#
# To check if any of these modules use "unsafe" opcodes you can compile
# plperl with the PLPERL_ENABLE_OPMASK_EARLY macro defined. See plperl.c

require strict;
require Carp;
require Carp::Heavy;
require warnings;
require feature if $] >= 5.010000;

#<<< protect next line from perltidy so perlcritic annotation works
package PostgreSQL::InServer::WarnEnv; ## no critic (RequireFilenameMatchesPackage)
#>>>

use strict;
use warnings;
use Tie::Hash;
our @ISA = qw(Tie::StdHash);

sub STORE  { warn "attempted alteration of \$ENV{$_[1]}"; }
sub DELETE { warn "attempted deletion of \$ENV{$_[1]}"; }
sub CLEAR  { warn "attempted clearance of ENV hash"; }

# Remove magic property of %ENV. Changes to this will now not be reflected in
# the process environment.
*main::ENV = {%ENV};

# Block %ENV changes from trusted PL/Perl, and warn. We changed %ENV to just a
# normal hash, yet the application may be expecting the usual Perl %ENV
# magic. Blocking and warning avoids silent application breakage. The user can
# untie or otherwise disable this, e.g. if the lost mutation is unimportant
# and modifying the code to stop that mutation would be onerous.
tie %main::ENV, 'PostgreSQL::InServer::WarnEnv', %ENV or die $!;
