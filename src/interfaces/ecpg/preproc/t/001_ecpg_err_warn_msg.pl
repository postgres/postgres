
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('ecpg');
program_version_ok('ecpg');
program_options_handling_ok('ecpg');
command_fails(['ecpg'], 'ecpg without arguments fails');

# Test that the ecpg command correctly detects unsupported or disallowed
# statements in the input file and reports the appropriate error or
# warning messages.
command_checks_all(
	[ 'ecpg', 't/err_warn_msg.pgc' ],
	3,
	[qr//],
	[
		qr/ERROR: AT option not allowed in CONNECT statement/,
		qr/ERROR: AT option not allowed in DISCONNECT statement/,
		qr/ERROR: AT option not allowed in SET CONNECTION statement/,
		qr/ERROR: AT option not allowed in TYPE statement/,
		qr/ERROR: AT option not allowed in WHENEVER statement/,
		qr/ERROR: AT option not allowed in VAR statement/,
		qr/WARNING: COPY FROM STDIN is not implemented/,
		qr/ERROR: using variable "cursor_var" in different declare statements is not supported/,
		qr/ERROR: cursor "duplicate_cursor" is already defined/,
		qr/ERROR: SHOW ALL is not implemented/,
		qr/WARNING: no longer supported LIMIT/,
		qr/WARNING: cursor "duplicate_cursor" has been declared but not opened/,
		qr/WARNING: cursor "duplicate_cursor" has been declared but not opened/,
		qr/WARNING: cursor ":cursor_var" has been declared but not opened/,
		qr/WARNING: cursor ":cursor_var" has been declared but not opened/
	],
	'ecpg with errors and warnings');

done_testing();
