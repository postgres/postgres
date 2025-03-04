
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

# Test that the ecpg command in INFORMIX mode correctly detects
# unsupported or disallowed statements in the input file and reports
# the appropriate error or warning messages.
command_checks_all(
	[ 'ecpg', '-C', 'INFORMIX', 't/err_warn_msg_informix.pgc' ],
	3,
	[qr//],
	[
		qr/ERROR: AT option not allowed in CLOSE DATABASE statement/,
		qr/ERROR: "database" cannot be used as cursor name in INFORMIX mode/
	],
	'ecpg in INFORMIX mode with errors and warnings');

done_testing();
