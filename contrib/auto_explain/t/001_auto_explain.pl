
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Runs the specified query and returns the emitted server log.
# If any parameters are specified, these are set in postgresql.conf,
# and reset after the query is run.
sub query_log
{
	my ($node, $sql, $params) = @_;
	$params ||= {};

	if (keys %$params)
	{
		for my $key (keys %$params)
		{
			$node->append_conf('postgresql.conf', "$key = $params->{$key}\n");
		}
		$node->reload;
	}

	my $log    = $node->logfile();
	my $offset = -s $log;

	$node->safe_psql("postgres", $sql);

	my $log_contents = slurp_file($log, $offset);

	if (keys %$params)
	{
		for my $key (keys %$params)
		{
			$node->adjust_conf('postgresql.conf', $key, undef);
		}
		$node->reload;
	}

	return $log_contents;
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'auto_explain'");
$node->append_conf('postgresql.conf', "auto_explain.log_min_duration = 0");
$node->append_conf('postgresql.conf', "auto_explain.log_analyze = on");
$node->start;

# Simple query.
my $log_contents = query_log($node, "SELECT * FROM pg_class;");

like(
	$log_contents,
	qr/Query Text: SELECT \* FROM pg_class;/,
	"query text logged, text mode");

unlike(
	$log_contents,
	qr/Query Parameters:/,
	"no query parameters logged when none, text mode");

like(
	$log_contents,
	qr/Seq Scan on pg_class/,
	"sequential scan logged, text mode");

# Prepared query.
$log_contents = query_log($node,
	q{PREPARE get_proc(name) AS SELECT * FROM pg_proc WHERE proname = $1; EXECUTE get_proc('int4pl');}
);

like(
	$log_contents,
	qr/Query Text: PREPARE get_proc\(name\) AS SELECT \* FROM pg_proc WHERE proname = \$1;/,
	"prepared query text logged, text mode");

like(
	$log_contents,
	qr/Query Parameters: \$1 = 'int4pl'/,
	"query parameters logged, text mode");

like(
	$log_contents,
	qr/Index Scan using pg_proc_proname_args_nsp_index on pg_proc/,
	"index scan logged, text mode");


# Prepared query with truncated parameters.
$log_contents = query_log(
	$node,
	q{PREPARE get_type(name) AS SELECT * FROM pg_type WHERE typname = $1; EXECUTE get_type('float8');},
	{ "auto_explain.log_parameter_max_length" => 3 });

like(
	$log_contents,
	qr/Query Text: PREPARE get_type\(name\) AS SELECT \* FROM pg_type WHERE typname = \$1;/,
	"prepared query text logged, text mode");

like(
	$log_contents,
	qr/Query Parameters: \$1 = 'flo\.\.\.'/,
	"query parameters truncated, text mode");

# Prepared query with parameter logging disabled.
$log_contents = query_log(
	$node,
	q{PREPARE get_type(name) AS SELECT * FROM pg_type WHERE typname = $1; EXECUTE get_type('float8');},
	{ "auto_explain.log_parameter_max_length" => 0 });

like(
	$log_contents,
	qr/Query Text: PREPARE get_type\(name\) AS SELECT \* FROM pg_type WHERE typname = \$1;/,
	"prepared query text logged, text mode");

unlike(
	$log_contents,
	qr/Query Parameters:/,
	"query parameters not logged when disabled, text mode");

# JSON format.
$log_contents = query_log(
	$node,
	"SELECT * FROM pg_proc;",
	{ "auto_explain.log_format" => "json" });

like(
	$log_contents,
	qr/"Query Text": "SELECT \* FROM pg_proc;"/,
	"query text logged, json mode");

unlike(
	$log_contents,
	qr/"Query Parameters":/,
	"query parameters not logged when none, json mode");

like(
	$log_contents,
	qr/"Node Type": "Seq Scan"[^}]*"Relation Name": "pg_proc"/s,
	"sequential scan logged, json mode");

# Prepared query in JSON format.
$log_contents = query_log(
	$node,
	q{PREPARE get_class(name) AS SELECT * FROM pg_class WHERE relname = $1; EXECUTE get_class('pg_class');},
	{ "auto_explain.log_format" => "json" });

like(
	$log_contents,
	qr/"Query Text": "PREPARE get_class\(name\) AS SELECT \* FROM pg_class WHERE relname = \$1;"/,
	"prepared query text logged, json mode");

like(
	$log_contents,
	qr/"Node Type": "Index Scan"[^}]*"Index Name": "pg_class_relname_nsp_index"/s,
	"index scan logged, json mode");

done_testing();
