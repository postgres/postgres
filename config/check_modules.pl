
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

#
# Verify that required Perl modules are available,
# in at least the required minimum versions.
# (The required minimum versions are all quite ancient now,
# but specify them anyway for documentation's sake.)
#
use strict;
use warnings FATAL => 'all';
use Config;

use IPC::Run 0.79;

# Test::More and Time::HiRes are supposed to be part of core Perl,
# but some distros omit them in a minimal installation.
use Test::More 0.98;
use Time::HiRes 1.52;

# While here, we might as well report exactly what versions we found.
diag("IPC::Run::VERSION: $IPC::Run::VERSION");
diag("Test::More::VERSION: $Test::More::VERSION");
diag("Time::HiRes::VERSION: $Time::HiRes::VERSION");

# Check that if prove is using msys perl it is for an msys target
ok( ($ENV{__CONFIG_HOST_OS__} || "") eq 'msys',
	"Msys perl used for correct target") if $Config{osname} eq 'msys';
ok(1);
done_testing();
