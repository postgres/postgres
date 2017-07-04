# Basic logical replication test
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 4;

# Initialize publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Setup structure on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_fk (bid int PRIMARY KEY);");
$node_publisher->safe_psql('postgres',
"CREATE TABLE tab_fk_ref (id int PRIMARY KEY, bid int REFERENCES tab_fk (bid));"
);

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_fk (bid int PRIMARY KEY);");
$node_subscriber->safe_psql('postgres',
"CREATE TABLE tab_fk_ref (id int PRIMARY KEY, bid int REFERENCES tab_fk (bid));"
);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR ALL TABLES;");

my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres',
"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub WITH (copy_data = false)"
);

# Wait for subscriber to finish initialization
my $caughtup_query =
"SELECT pg_current_wal_lsn() <= replay_lsn FROM pg_stat_replication WHERE application_name = '$appname';";
$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk (bid) VALUES (1);");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid) VALUES (1, 1);");

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# Check data on subscriber
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk;");
is($result, qq(1|1|1), 'check replicated tab_fk inserts on subscriber');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(1|1|1), 'check replicated tab_fk_ref inserts on subscriber');

# Drop the fk on publisher
$node_publisher->safe_psql('postgres', "DROP TABLE tab_fk CASCADE;");

# Insert data
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid) VALUES (2, 2);");

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# FK is not enforced on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2), 'check FK ignored on subscriber');

# Add replica trigger
$node_subscriber->safe_psql(
	'postgres', qq{
CREATE FUNCTION filter_basic_dml_fn() RETURNS TRIGGER AS \$\$
BEGIN
    IF (TG_OP = 'INSERT') THEN
        IF (NEW.id < 10) THEN
            RETURN NEW;
        ELSE
            RETURN NULL;
        END IF;
    ELSE
        RAISE WARNING 'Unknown action';
        RETURN NULL;
    END IF;
END;
\$\$ LANGUAGE plpgsql;
CREATE TRIGGER filter_basic_dml_trg
    BEFORE INSERT ON tab_fk_ref
    FOR EACH ROW EXECUTE PROCEDURE filter_basic_dml_fn();
ALTER TABLE tab_fk_ref ENABLE REPLICA TRIGGER filter_basic_dml_trg;
});

# Insert data
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_fk_ref (id, bid) VALUES (10, 10);");

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# The row should be skipped on subscriber
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(bid), max(bid) FROM tab_fk_ref;");
is($result, qq(2|1|2), 'check replica trigger applied on subscriber');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');
