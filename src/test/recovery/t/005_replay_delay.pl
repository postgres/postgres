# Checks for recovery_min_apply_delay
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 2;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;

# And some content
$node_master->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,10) AS a");

# Take backup
my $backup_name = 'my_backup';
$node_master->backup($backup_name);

# Create streaming standby from backup
my $node_standby = get_new_node('standby');
$node_standby->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby->append_conf(
	'recovery.conf', qq(
recovery_min_apply_delay = '2s'
));
$node_standby->start;

# Make new content on master and check its presence in standby
# depending on the delay of 2s applied above.
$node_master->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(11,20))");
sleep 1;

# Here we should have only 10 rows
my $result = $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int");
is($result, qq(10), 'check content with delay of 1s');

# Now wait for replay to complete on standby
my $until_lsn =
  $node_master->safe_psql('postgres', "SELECT pg_current_xlog_location();");
my $caughtup_query =
  "SELECT '$until_lsn'::pg_lsn <= pg_last_xlog_replay_location()";
$node_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";
$result = $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int");
is($result, qq(20), 'check content with delay of 2s');
