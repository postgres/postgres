use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 4;

my $node = get_new_node('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'auto_explain'");
$node->append_conf('postgresql.conf', "auto_explain.log_min_duration = 0");
$node->append_conf('postgresql.conf', "auto_explain.log_analyze = on");
$node->start;

# run a couple of queries
$node->safe_psql("postgres", "SELECT * FROM pg_class;");
$node->safe_psql("postgres",
	"SELECT * FROM pg_proc WHERE proname = 'int4pl';");

# emit some json too
$node->append_conf('postgresql.conf', "auto_explain.log_format = json");
$node->reload;
$node->safe_psql("postgres", "SELECT * FROM pg_proc;");
$node->safe_psql("postgres",
	"SELECT * FROM pg_class WHERE relname = 'pg_class';");

$node->stop('fast');

my $log = $node->logfile();

my $log_contents = slurp_file($log);

like(
	$log_contents,
	qr/Seq Scan on pg_class/,
	"sequential scan logged, text mode");

like(
	$log_contents,
	qr/Index Scan using pg_proc_proname_args_nsp_index on pg_proc/,
	"index scan logged, text mode");

like(
	$log_contents,
	qr/"Node Type": "Seq Scan"[^}]*"Relation Name": "pg_proc"/s,
	"sequential scan logged, json mode");

like(
	$log_contents,
	qr/"Node Type": "Index Scan"[^}]*"Index Name": "pg_class_relname_nsp_index"/s,
	"index scan logged, json mode");
