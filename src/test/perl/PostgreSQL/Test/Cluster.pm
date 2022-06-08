
# Copyright (c) 2022, PostgreSQL Global Development Group

# allow use of release 15+ perl namespace in older branches
# just 'use' the older module name.
# See PostgresNode.pm for function implementations

package PostgreSQL::Test::Cluster;

use strict;
use warnings;

use PostgresNode;

1;
