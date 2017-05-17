# This tests that more complex datatypes are replicated correctly
# by logical replication
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 3;

# Initialize publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create some preexisting content on publisher
my $ddl = qq(
	CREATE EXTENSION hstore WITH SCHEMA public;
	CREATE TABLE public.tst_one_array (
		a INTEGER PRIMARY KEY,
		b INTEGER[]
		);
	CREATE TABLE public.tst_arrays (
		a INTEGER[] PRIMARY KEY,
		b TEXT[],
		c FLOAT[],
		d INTERVAL[]
		);

	CREATE TYPE public.tst_enum_t AS ENUM ('a', 'b', 'c', 'd', 'e');
	CREATE TABLE public.tst_one_enum (
		a INTEGER PRIMARY KEY,
		b public.tst_enum_t
		);
	CREATE TABLE public.tst_enums (
		a public.tst_enum_t PRIMARY KEY,
		b public.tst_enum_t[]
		);

	CREATE TYPE public.tst_comp_basic_t AS (a FLOAT, b TEXT, c INTEGER);
	CREATE TYPE public.tst_comp_enum_t AS (a FLOAT, b public.tst_enum_t, c INTEGER);
	CREATE TYPE public.tst_comp_enum_array_t AS (a FLOAT, b public.tst_enum_t[], c INTEGER);
	CREATE TABLE public.tst_one_comp (
		a INTEGER PRIMARY KEY,
		b public.tst_comp_basic_t
		);
	CREATE TABLE public.tst_comps (
		a public.tst_comp_basic_t PRIMARY KEY,
		b public.tst_comp_basic_t[]
		);
	CREATE TABLE public.tst_comp_enum (
		a INTEGER PRIMARY KEY,
		b public.tst_comp_enum_t
		);
	CREATE TABLE public.tst_comp_enum_array (
		a public.tst_comp_enum_t PRIMARY KEY,
		b public.tst_comp_enum_t[]
		);
	CREATE TABLE public.tst_comp_one_enum_array (
		a INTEGER PRIMARY KEY,
		b public.tst_comp_enum_array_t
		);
	CREATE TABLE public.tst_comp_enum_what (
		a public.tst_comp_enum_array_t PRIMARY KEY,
		b public.tst_comp_enum_array_t[]
		);

	CREATE TYPE public.tst_comp_mix_t AS (
		a public.tst_comp_basic_t,
		b public.tst_comp_basic_t[],
		c public.tst_enum_t,
		d public.tst_enum_t[]
		);
	CREATE TABLE public.tst_comp_mix_array (
		a public.tst_comp_mix_t PRIMARY KEY,
		b public.tst_comp_mix_t[]
		);
	CREATE TABLE public.tst_range (
		a INTEGER PRIMARY KEY,
		b int4range
	);
	CREATE TABLE public.tst_range_array (
		a INTEGER PRIMARY KEY,
		b TSTZRANGE,
		c int8range[]
	);
	CREATE TABLE public.tst_hstore (
		a INTEGER PRIMARY KEY,
		b public.hstore
	););

# Setup structure on both nodes
$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR ALL TABLES");

my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres',
"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub WITH (slot_name = tap_sub_slot)"
);

# Wait for subscriber to finish initialization
my $caughtup_query =
"SELECT pg_current_wal_lsn() <= replay_lsn FROM pg_stat_replication WHERE application_name = '$appname';";
$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# Wait for initial sync to finish as well
my $synced_query =
"SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('s', 'r');";
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# Insert initial test data
$node_publisher->safe_psql(
	'postgres', qq(
	-- test_tbl_one_array_col
	INSERT INTO tst_one_array (a, b) VALUES
		(1, '{1, 2, 3}'),
		(2, '{2, 3, 1}'),
		(3, '{3, 2, 1}'),
		(4, '{4, 3, 2}'),
		(5, '{5, NULL, 3}');

	-- test_tbl_arrays
	INSERT INTO tst_arrays (a, b, c, d) VALUES
		('{1, 2, 3}', '{"a", "b", "c"}', '{1.1, 2.2, 3.3}', '{"1 day", "2 days", "3 days"}'),
		('{2, 3, 1}', '{"b", "c", "a"}', '{2.2, 3.3, 1.1}', '{"2 minutes", "3 minutes", "1 minute"}'),
		('{3, 1, 2}', '{"c", "a", "b"}', '{3.3, 1.1, 2.2}', '{"3 years", "1 year", "2 years"}'),
		('{4, 1, 2}', '{"d", "a", "b"}', '{4.4, 1.1, 2.2}', '{"4 years", "1 year", "2 years"}'),
		('{5, NULL, NULL}', '{"e", NULL, "b"}', '{5.5, 1.1, NULL}', '{"5 years", NULL, NULL}');

	-- test_tbl_single_enum
	INSERT INTO tst_one_enum (a, b) VALUES
		(1, 'a'),
		(2, 'b'),
		(3, 'c'),
		(4, 'd'),
		(5, NULL);

	-- test_tbl_enums
	INSERT INTO tst_enums (a, b) VALUES
		('a', '{b, c}'),
		('b', '{c, a}'),
		('c', '{b, a}'),
		('d', '{c, b}'),
		('e', '{d, NULL}');

	-- test_tbl_single_composites
	INSERT INTO tst_one_comp (a, b) VALUES
		(1, ROW(1.0, 'a', 1)),
		(2, ROW(2.0, 'b', 2)),
		(3, ROW(3.0, 'c', 3)),
		(4, ROW(4.0, 'd', 4)),
		(5, ROW(NULL, NULL, 5));

	-- test_tbl_composites
	INSERT INTO tst_comps (a, b) VALUES
		(ROW(1.0, 'a', 1), ARRAY[ROW(1, 'a', 1)::tst_comp_basic_t]),
		(ROW(2.0, 'b', 2), ARRAY[ROW(2, 'b', 2)::tst_comp_basic_t]),
		(ROW(3.0, 'c', 3), ARRAY[ROW(3, 'c', 3)::tst_comp_basic_t]),
		(ROW(4.0, 'd', 4), ARRAY[ROW(4, 'd', 3)::tst_comp_basic_t]),
		(ROW(5.0, 'e', NULL), ARRAY[NULL, ROW(5, NULL, 5)::tst_comp_basic_t]);

	-- test_tbl_composite_with_enums
	INSERT INTO tst_comp_enum (a, b) VALUES
		(1, ROW(1.0, 'a', 1)),
		(2, ROW(2.0, 'b', 2)),
		(3, ROW(3.0, 'c', 3)),
		(4, ROW(4.0, 'd', 4)),
		(5, ROW(NULL, 'e', NULL));

	-- test_tbl_composite_with_enums_array
	INSERT INTO tst_comp_enum_array (a, b) VALUES
		(ROW(1.0, 'a', 1), ARRAY[ROW(1, 'a', 1)::tst_comp_enum_t]),
		(ROW(2.0, 'b', 2), ARRAY[ROW(2, 'b', 2)::tst_comp_enum_t]),
		(ROW(3.0, 'c', 3), ARRAY[ROW(3, 'c', 3)::tst_comp_enum_t]),
		(ROW(4.0, 'd', 3), ARRAY[ROW(3, 'd', 3)::tst_comp_enum_t]),
		(ROW(5.0, 'e', 3), ARRAY[ROW(3, 'e', 3)::tst_comp_enum_t, NULL]);

	-- test_tbl_composite_with_single_enums_array_in_composite
	INSERT INTO tst_comp_one_enum_array (a, b) VALUES
		(1, ROW(1.0, '{a, b, c}', 1)),
		(2, ROW(2.0, '{a, b, c}', 2)),
		(3, ROW(3.0, '{a, b, c}', 3)),
		(4, ROW(4.0, '{c, b, d}', 4)),
		(5, ROW(5.0, '{NULL, e, NULL}', 5));

	-- test_tbl_composite_with_enums_array_in_composite
	INSERT INTO tst_comp_enum_what (a, b) VALUES
		(ROW(1.0, '{a, b, c}', 1), ARRAY[ROW(1, '{a, b, c}', 1)::tst_comp_enum_array_t]),
		(ROW(2.0, '{b, c, a}', 2), ARRAY[ROW(2, '{b, c, a}', 1)::tst_comp_enum_array_t]),
		(ROW(3.0, '{c, a, b}', 1), ARRAY[ROW(3, '{c, a, b}', 1)::tst_comp_enum_array_t]),
		(ROW(4.0, '{c, b, d}', 4), ARRAY[ROW(4, '{c, b, d}', 4)::tst_comp_enum_array_t]),
		(ROW(5.0, '{c, NULL, b}', NULL), ARRAY[ROW(5, '{c, e, b}', 1)::tst_comp_enum_array_t]);

	-- test_tbl_mixed_composites
	INSERT INTO tst_comp_mix_array (a, b) VALUES
		(ROW(
			ROW(1,'a',1),
			ARRAY[ROW(1,'a',1)::tst_comp_basic_t, ROW(2,'b',2)::tst_comp_basic_t],
			'a',
			'{a,b,NULL,c}'),
		ARRAY[
			ROW(
				ROW(1,'a',1),
				ARRAY[
					ROW(1,'a',1)::tst_comp_basic_t,
					ROW(2,'b',2)::tst_comp_basic_t,
					NULL
					],
				'a',
				'{a,b,c}'
				)::tst_comp_mix_t
			]
		);

	-- test_tbl_range
	INSERT INTO tst_range (a, b) VALUES
		(1, '[1, 10]'),
		(2, '[2, 20]'),
		(3, '[3, 30]'),
		(4, '[4, 40]'),
		(5, '[5, 50]');

	-- test_tbl_range_array
	INSERT INTO tst_range_array (a, b, c) VALUES
		(1, tstzrange('Mon Aug 04 00:00:00 2014 CEST'::timestamptz, 'infinity'), '{"[1,2]", "[10,20]"}'),
		(2, tstzrange('Mon Aug 04 00:00:00 2014 CEST'::timestamptz - interval '2 days', 'Mon Aug 04 00:00:00 2014 CEST'::timestamptz), '{"[2,3]", "[20,30]"}'),
		(3, tstzrange('Mon Aug 04 00:00:00 2014 CEST'::timestamptz - interval '3 days', 'Mon Aug 04 00:00:00 2014 CEST'::timestamptz), '{"[3,4]"}'),
		(4, tstzrange('Mon Aug 04 00:00:00 2014 CEST'::timestamptz - interval '4 days', 'Mon Aug 04 00:00:00 2014 CEST'::timestamptz), '{"[4,5]", NULL, "[40,50]"}'),
		(5, NULL, NULL);

	-- tst_hstore
	INSERT INTO tst_hstore (a, b) VALUES
		(1, '"a"=>"1"'),
		(2, '"zzz"=>"foo"'),
		(3, '"123"=>"321"'),
		(4, '"yellow horse"=>"moaned"');
));

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# Check the data on subscriber
my $result = $node_subscriber->safe_psql(
	'postgres', qq(
	SET timezone = '+2';
	SELECT a, b FROM tst_one_array ORDER BY a;
	SELECT a, b, c, d FROM tst_arrays ORDER BY a;
	SELECT a, b FROM tst_one_enum ORDER BY a;
	SELECT a, b FROM tst_enums ORDER BY a;
	SELECT a, b FROM tst_one_comp ORDER BY a;
	SELECT a, b FROM tst_comps ORDER BY a;
	SELECT a, b FROM tst_comp_enum ORDER BY a;
	SELECT a, b FROM tst_comp_enum_array ORDER BY a;
	SELECT a, b FROM tst_comp_one_enum_array ORDER BY a;
	SELECT a, b FROM tst_comp_enum_what ORDER BY a;
	SELECT a, b FROM tst_comp_mix_array ORDER BY a;
	SELECT a, b FROM tst_range ORDER BY a;
	SELECT a, b, c FROM tst_range_array ORDER BY a;
	SELECT a, b FROM tst_hstore ORDER BY a;
));

is( $result, '1|{1,2,3}
2|{2,3,1}
3|{3,2,1}
4|{4,3,2}
5|{5,NULL,3}
{1,2,3}|{a,b,c}|{1.1,2.2,3.3}|{"1 day","2 days","3 days"}
{2,3,1}|{b,c,a}|{2.2,3.3,1.1}|{00:02:00,00:03:00,00:01:00}
{3,1,2}|{c,a,b}|{3.3,1.1,2.2}|{"3 years","1 year","2 years"}
{4,1,2}|{d,a,b}|{4.4,1.1,2.2}|{"4 years","1 year","2 years"}
{5,NULL,NULL}|{e,NULL,b}|{5.5,1.1,NULL}|{"5 years",NULL,NULL}
1|a
2|b
3|c
4|d
5|
a|{b,c}
b|{c,a}
c|{b,a}
d|{c,b}
e|{d,NULL}
1|(1,a,1)
2|(2,b,2)
3|(3,c,3)
4|(4,d,4)
5|(,,5)
(1,a,1)|{"(1,a,1)"}
(2,b,2)|{"(2,b,2)"}
(3,c,3)|{"(3,c,3)"}
(4,d,4)|{"(4,d,3)"}
(5,e,)|{NULL,"(5,,5)"}
1|(1,a,1)
2|(2,b,2)
3|(3,c,3)
4|(4,d,4)
5|(,e,)
(1,a,1)|{"(1,a,1)"}
(2,b,2)|{"(2,b,2)"}
(3,c,3)|{"(3,c,3)"}
(4,d,3)|{"(3,d,3)"}
(5,e,3)|{"(3,e,3)",NULL}
1|(1,"{a,b,c}",1)
2|(2,"{a,b,c}",2)
3|(3,"{a,b,c}",3)
4|(4,"{c,b,d}",4)
5|(5,"{NULL,e,NULL}",5)
(1,"{a,b,c}",1)|{"(1,\"{a,b,c}\",1)"}
(2,"{b,c,a}",2)|{"(2,\"{b,c,a}\",1)"}
(3,"{c,a,b}",1)|{"(3,\"{c,a,b}\",1)"}
(4,"{c,b,d}",4)|{"(4,\"{c,b,d}\",4)"}
(5,"{c,NULL,b}",)|{"(5,\"{c,e,b}\",1)"}
("(1,a,1)","{""(1,a,1)"",""(2,b,2)""}",a,"{a,b,NULL,c}")|{"(\"(1,a,1)\",\"{\"\"(1,a,1)\"\",\"\"(2,b,2)\"\",NULL}\",a,\"{a,b,c}\")"}
1|[1,11)
2|[2,21)
3|[3,31)
4|[4,41)
5|[5,51)
1|["2014-08-04 00:00:00+02",infinity)|{"[1,3)","[10,21)"}
2|["2014-08-02 00:00:00+02","2014-08-04 00:00:00+02")|{"[2,4)","[20,31)"}
3|["2014-08-01 00:00:00+02","2014-08-04 00:00:00+02")|{"[3,5)"}
4|["2014-07-31 00:00:00+02","2014-08-04 00:00:00+02")|{"[4,6)",NULL,"[40,51)"}
5||
1|"a"=>"1"
2|"zzz"=>"foo"
3|"123"=>"321"
4|"yellow horse"=>"moaned"',
	'check replicated inserts on subscriber');

# Run batch of updates
$node_publisher->safe_psql(
	'postgres', qq(
	UPDATE tst_one_array SET b = '{4, 5, 6}' WHERE a = 1;
	UPDATE tst_one_array SET b = '{4, 5, 6, 1}' WHERE a > 3;
	UPDATE tst_arrays SET b = '{"1a", "2b", "3c"}', c = '{1.0, 2.0, 3.0}', d = '{"1 day 1 second", "2 days 2 seconds", "3 days 3 second"}' WHERE a = '{1, 2, 3}';
	UPDATE tst_arrays SET b = '{"c", "d", "e"}', c = '{3.0, 4.0, 5.0}', d = '{"3 day 1 second", "4 days 2 seconds", "5 days 3 second"}' WHERE a[1] > 3;
	UPDATE tst_one_enum SET b = 'c' WHERE a = 1;
	UPDATE tst_one_enum SET b = NULL WHERE a > 3;
	UPDATE tst_enums SET b = '{e, NULL}' WHERE a = 'a';
	UPDATE tst_enums SET b = '{e, d}' WHERE a > 'c';
	UPDATE tst_one_comp SET b = ROW(1.0, 'A', 1) WHERE a = 1;
	UPDATE tst_one_comp SET b = ROW(NULL, 'x', -1) WHERE a > 3;
	UPDATE tst_comps SET b = ARRAY[ROW(9, 'x', -1)::tst_comp_basic_t] WHERE (a).a = 1.0;
	UPDATE tst_comps SET b = ARRAY[NULL, ROW(9, 'x', NULL)::tst_comp_basic_t] WHERE (a).a > 3.9;
	UPDATE tst_comp_enum SET b = ROW(1.0, NULL, NULL) WHERE a = 1;
	UPDATE tst_comp_enum SET b = ROW(4.0, 'd', 44) WHERE a > 3;
	UPDATE tst_comp_enum_array SET b = ARRAY[NULL, ROW(3, 'd', 3)::tst_comp_enum_t] WHERE a = ROW(1.0, 'a', 1)::tst_comp_enum_t;
	UPDATE tst_comp_enum_array SET b = ARRAY[ROW(1, 'a', 1)::tst_comp_enum_t, ROW(2, 'b', 2)::tst_comp_enum_t] WHERE (a).a > 3;
	UPDATE tst_comp_one_enum_array SET b = ROW(1.0, '{a, e, c}', NULL) WHERE a = 1;
	UPDATE tst_comp_one_enum_array SET b = ROW(4.0, '{c, b, d}', 4) WHERE a > 3;
	UPDATE tst_comp_enum_what SET b = ARRAY[NULL, ROW(1, '{a, b, c}', 1)::tst_comp_enum_array_t, ROW(NULL, '{a, e, c}', 2)::tst_comp_enum_array_t] WHERE (a).a = 1;
	UPDATE tst_comp_enum_what SET b = ARRAY[ROW(5, '{a, b, c}', 5)::tst_comp_enum_array_t] WHERE (a).a > 3;
	UPDATE tst_comp_mix_array SET b[2] = NULL WHERE ((a).a).a = 1;
	UPDATE tst_range SET b = '[100, 1000]' WHERE a = 1;
	UPDATE tst_range SET b = '(1, 90)' WHERE a > 3;
	UPDATE tst_range_array SET c = '{"[100, 1000]"}' WHERE a = 1;
	UPDATE tst_range_array SET b = tstzrange('Mon Aug 04 00:00:00 2014 CEST'::timestamptz, 'infinity'), c = '{NULL, "[11,9999999]"}' WHERE a > 3;
	UPDATE tst_hstore SET b = '"updated"=>"value"' WHERE a < 3;
	UPDATE tst_hstore SET b = '"also"=>"updated"' WHERE a = 3;
));

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# Check the data on subscriber
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SET timezone = '+2';
	SELECT a, b FROM tst_one_array ORDER BY a;
	SELECT a, b, c, d FROM tst_arrays ORDER BY a;
	SELECT a, b FROM tst_one_enum ORDER BY a;
	SELECT a, b FROM tst_enums ORDER BY a;
	SELECT a, b FROM tst_one_comp ORDER BY a;
	SELECT a, b FROM tst_comps ORDER BY a;
	SELECT a, b FROM tst_comp_enum ORDER BY a;
	SELECT a, b FROM tst_comp_enum_array ORDER BY a;
	SELECT a, b FROM tst_comp_one_enum_array ORDER BY a;
	SELECT a, b FROM tst_comp_enum_what ORDER BY a;
	SELECT a, b FROM tst_comp_mix_array ORDER BY a;
	SELECT a, b FROM tst_range ORDER BY a;
	SELECT a, b, c FROM tst_range_array ORDER BY a;
	SELECT a, b FROM tst_hstore ORDER BY a;
));

is( $result, '1|{4,5,6}
2|{2,3,1}
3|{3,2,1}
4|{4,5,6,1}
5|{4,5,6,1}
{1,2,3}|{1a,2b,3c}|{1,2,3}|{"1 day 00:00:01","2 days 00:00:02","3 days 00:00:03"}
{2,3,1}|{b,c,a}|{2.2,3.3,1.1}|{00:02:00,00:03:00,00:01:00}
{3,1,2}|{c,a,b}|{3.3,1.1,2.2}|{"3 years","1 year","2 years"}
{4,1,2}|{c,d,e}|{3,4,5}|{"3 days 00:00:01","4 days 00:00:02","5 days 00:00:03"}
{5,NULL,NULL}|{c,d,e}|{3,4,5}|{"3 days 00:00:01","4 days 00:00:02","5 days 00:00:03"}
1|c
2|b
3|c
4|
5|
a|{e,NULL}
b|{c,a}
c|{b,a}
d|{e,d}
e|{e,d}
1|(1,A,1)
2|(2,b,2)
3|(3,c,3)
4|(,x,-1)
5|(,x,-1)
(1,a,1)|{"(9,x,-1)"}
(2,b,2)|{"(2,b,2)"}
(3,c,3)|{"(3,c,3)"}
(4,d,4)|{NULL,"(9,x,)"}
(5,e,)|{NULL,"(9,x,)"}
1|(1,,)
2|(2,b,2)
3|(3,c,3)
4|(4,d,44)
5|(4,d,44)
(1,a,1)|{NULL,"(3,d,3)"}
(2,b,2)|{"(2,b,2)"}
(3,c,3)|{"(3,c,3)"}
(4,d,3)|{"(1,a,1)","(2,b,2)"}
(5,e,3)|{"(1,a,1)","(2,b,2)"}
1|(1,"{a,e,c}",)
2|(2,"{a,b,c}",2)
3|(3,"{a,b,c}",3)
4|(4,"{c,b,d}",4)
5|(4,"{c,b,d}",4)
(1,"{a,b,c}",1)|{NULL,"(1,\"{a,b,c}\",1)","(,\"{a,e,c}\",2)"}
(2,"{b,c,a}",2)|{"(2,\"{b,c,a}\",1)"}
(3,"{c,a,b}",1)|{"(3,\"{c,a,b}\",1)"}
(4,"{c,b,d}",4)|{"(5,\"{a,b,c}\",5)"}
(5,"{c,NULL,b}",)|{"(5,\"{a,b,c}\",5)"}
("(1,a,1)","{""(1,a,1)"",""(2,b,2)""}",a,"{a,b,NULL,c}")|{"(\"(1,a,1)\",\"{\"\"(1,a,1)\"\",\"\"(2,b,2)\"\",NULL}\",a,\"{a,b,c}\")",NULL}
1|[100,1001)
2|[2,21)
3|[3,31)
4|[2,90)
5|[2,90)
1|["2014-08-04 00:00:00+02",infinity)|{"[100,1001)"}
2|["2014-08-02 00:00:00+02","2014-08-04 00:00:00+02")|{"[2,4)","[20,31)"}
3|["2014-08-01 00:00:00+02","2014-08-04 00:00:00+02")|{"[3,5)"}
4|["2014-08-04 00:00:00+02",infinity)|{NULL,"[11,10000000)"}
5|["2014-08-04 00:00:00+02",infinity)|{NULL,"[11,10000000)"}
1|"updated"=>"value"
2|"updated"=>"value"
3|"also"=>"updated"
4|"yellow horse"=>"moaned"',
	'check replicated updates on subscriber');

# Run batch of deletes
$node_publisher->safe_psql(
	'postgres', qq(
	DELETE FROM tst_one_array WHERE a = 1;
	DELETE FROM tst_one_array WHERE b = '{2, 3, 1}';
	DELETE FROM tst_arrays WHERE a = '{1, 2, 3}';
	DELETE FROM tst_arrays WHERE a[1] = 2;
	DELETE FROM tst_one_enum WHERE a = 1;
	DELETE FROM tst_one_enum WHERE b = 'b';
	DELETE FROM tst_enums WHERE a = 'a';
	DELETE FROM tst_enums WHERE b[1] = 'b';
	DELETE FROM tst_one_comp WHERE a = 1;
	DELETE FROM tst_one_comp WHERE (b).a = 2.0;
	DELETE FROM tst_comps WHERE (a).b = 'a';
	DELETE FROM tst_comps WHERE ROW(3, 'c', 3)::tst_comp_basic_t = ANY(b);
	DELETE FROM tst_comp_enum WHERE a = 1;
	DELETE FROM tst_comp_enum WHERE (b).a = 2.0;
	DELETE FROM tst_comp_enum_array WHERE a = ROW(1.0, 'a', 1)::tst_comp_enum_t;
	DELETE FROM tst_comp_enum_array WHERE ROW(3, 'c', 3)::tst_comp_enum_t = ANY(b);
	DELETE FROM tst_comp_one_enum_array WHERE a = 1;
	DELETE FROM tst_comp_one_enum_array WHERE 'a' = ANY((b).b);
	DELETE FROM tst_comp_enum_what WHERE (a).a = 1;
	DELETE FROM tst_comp_enum_what WHERE (b[1]).b = '{c, a, b}';
	DELETE FROM tst_comp_mix_array WHERE ((a).a).a = 1;
	DELETE FROM tst_range WHERE a = 1;
	DELETE FROM tst_range WHERE '[10,20]' && b;
	DELETE FROM tst_range_array WHERE a = 1;
	DELETE FROM tst_range_array WHERE tstzrange('Mon Aug 04 00:00:00 2014 CEST'::timestamptz, 'Mon Aug 05 00:00:00 2014 CEST'::timestamptz) && b;
	DELETE FROM tst_hstore WHERE a = 1;
));

$node_publisher->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for subscriber to catch up";

# Check the data on subscriber
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SET timezone = '+2';
	SELECT a, b FROM tst_one_array ORDER BY a;
	SELECT a, b, c, d FROM tst_arrays ORDER BY a;
	SELECT a, b FROM tst_one_enum ORDER BY a;
	SELECT a, b FROM tst_enums ORDER BY a;
	SELECT a, b FROM tst_one_comp ORDER BY a;
	SELECT a, b FROM tst_comps ORDER BY a;
	SELECT a, b FROM tst_comp_enum ORDER BY a;
	SELECT a, b FROM tst_comp_enum_array ORDER BY a;
	SELECT a, b FROM tst_comp_one_enum_array ORDER BY a;
	SELECT a, b FROM tst_comp_enum_what ORDER BY a;
	SELECT a, b FROM tst_comp_mix_array ORDER BY a;
	SELECT a, b FROM tst_range ORDER BY a;
	SELECT a, b, c FROM tst_range_array ORDER BY a;
	SELECT a, b FROM tst_hstore ORDER BY a;
));

is( $result, '3|{3,2,1}
4|{4,5,6,1}
5|{4,5,6,1}
{3,1,2}|{c,a,b}|{3.3,1.1,2.2}|{"3 years","1 year","2 years"}
{4,1,2}|{c,d,e}|{3,4,5}|{"3 days 00:00:01","4 days 00:00:02","5 days 00:00:03"}
{5,NULL,NULL}|{c,d,e}|{3,4,5}|{"3 days 00:00:01","4 days 00:00:02","5 days 00:00:03"}
3|c
4|
5|
b|{c,a}
d|{e,d}
e|{e,d}
3|(3,c,3)
4|(,x,-1)
5|(,x,-1)
(2,b,2)|{"(2,b,2)"}
(4,d,4)|{NULL,"(9,x,)"}
(5,e,)|{NULL,"(9,x,)"}
3|(3,c,3)
4|(4,d,44)
5|(4,d,44)
(2,b,2)|{"(2,b,2)"}
(4,d,3)|{"(1,a,1)","(2,b,2)"}
(5,e,3)|{"(1,a,1)","(2,b,2)"}
4|(4,"{c,b,d}",4)
5|(4,"{c,b,d}",4)
(2,"{b,c,a}",2)|{"(2,\"{b,c,a}\",1)"}
(4,"{c,b,d}",4)|{"(5,\"{a,b,c}\",5)"}
(5,"{c,NULL,b}",)|{"(5,\"{a,b,c}\",5)"}
2|["2014-08-02 00:00:00+02","2014-08-04 00:00:00+02")|{"[2,4)","[20,31)"}
3|["2014-08-01 00:00:00+02","2014-08-04 00:00:00+02")|{"[3,5)"}
2|"updated"=>"value"
3|"also"=>"updated"
4|"yellow horse"=>"moaned"',
	'check replicated deletes on subscriber');

$node_subscriber->stop('fast');
$node_publisher->stop('fast');
