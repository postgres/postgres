#
# Verify that required Perl modules are available,
# in at least the required minimum versions.
# (The required minimum versions are all quite ancient now,
# but specify them anyway for documentation's sake.)
#
use strict;
use warnings;

use IPC::Run 0.79;

# Test::More and Time::HiRes are supposed to be part of core Perl,
# but some distros omit them in a minimal installation.
use Test::More 0.87;
use Time::HiRes 1.52;

# While here, we might as well report exactly what versions we found.
diag("IPC::Run::VERSION: $IPC::Run::VERSION");
diag("Test::More::VERSION: $Test::More::VERSION");
diag("Time::HiRes::VERSION: $Time::HiRes::VERSION");

ok(1);
done_testing();
