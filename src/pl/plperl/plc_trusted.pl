
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

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
