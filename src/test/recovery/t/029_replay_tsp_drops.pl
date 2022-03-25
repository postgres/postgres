# Copyright (c) 2022, PostgreSQL Global Development Group

# Test recovery involving tablespace removal.  If recovery stops
# after once tablespace is removed, the next recovery should properly
# ignore the operations within the removed tablespaces.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary1');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
$node_primary->psql('postgres',
qq[
	SET allow_in_place_tablespaces=on;
	CREATE TABLESPACE dropme_ts1 LOCATION '';
	CREATE TABLESPACE dropme_ts2 LOCATION '';
	CREATE TABLESPACE source_ts  LOCATION '';
	CREATE TABLESPACE target_ts  LOCATION '';
    CREATE DATABASE template_db IS_TEMPLATE = true;
]);
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('standby1');
$node_standby->init_from_backup($node_primary, $backup_name, has_streaming => 1);
$node_standby->start;

# Make sure connection is made
$node_primary->poll_query_until(
	'postgres', 'SELECT count(*) = 1 FROM pg_stat_replication');

$node_standby->safe_psql('postgres', 'CHECKPOINT');

# Do immediate shutdown just after a sequence of CREATE DATABASE / DROP
# DATABASE / DROP TABLESPACE. This causes CREATE DATABASE WAL records
# to be applied to already-removed directories.
$node_primary->safe_psql('postgres',
						q[CREATE DATABASE dropme_db1 WITH TABLESPACE dropme_ts1;
						  CREATE DATABASE dropme_db2 WITH TABLESPACE dropme_ts2;
						  CREATE DATABASE moveme_db TABLESPACE source_ts;
						  ALTER DATABASE moveme_db SET TABLESPACE target_ts;
						  CREATE DATABASE newdb TEMPLATE template_db;
						  ALTER DATABASE template_db IS_TEMPLATE = false;
						  DROP DATABASE dropme_db1;
						  DROP DATABASE dropme_db2; DROP TABLESPACE dropme_ts2;
						  DROP TABLESPACE source_ts;
						  DROP DATABASE template_db;]);

$node_primary->wait_for_catchup($node_standby, 'replay',
							   $node_primary->lsn('replay'));
$node_standby->stop('immediate');

# Should restart ignoring directory creation error.
is($node_standby->start, 1, "standby started successfully");

my $log = PostgreSQL::Test::Utils::slurp_file($node_standby->logfile);
like(
	$log,
	qr[WARNING:  skipping replay of database creation WAL record],
	"warning message is logged");

done_testing();
