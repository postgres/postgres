use strict;
use warnings;

use TestLib;
use Test::More tests => 8;

#########################################
# Basic checks

program_help_ok('oid2name');
program_version_ok('oid2name');
program_options_handling_ok('oid2name');
