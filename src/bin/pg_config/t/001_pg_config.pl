use strict;
use warnings;
use TestLib;
use Test::More tests => 20;

program_help_ok('pg_config');
program_version_ok('pg_config');
program_options_handling_ok('pg_config');
command_like([ 'pg_config', '--bindir' ], qr/bin/, 'pg_config single option')
  ;    # XXX might be wrong
command_like([ 'pg_config', '--bindir', '--libdir' ],
	qr/bin.*\n.*lib/, 'pg_config two options');
command_like([ 'pg_config', '--libdir', '--bindir' ],
	qr/lib.*\n.*bin/, 'pg_config two options different order');
command_like(['pg_config'], qr/.*\n.*\n.*/,
	'pg_config without options prints many lines');
