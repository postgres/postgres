
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# start a pgbench specific server
my $node = PostgreSQL::Test::Cluster->new('main');
# Set to untranslated messages, to be able to compare program output with
# expected strings.
$node->init(extra => [ '--locale', 'C' ]);
$node->start;

# tablespace for testing, because partitioned tables cannot use pg_default
# explicitly and we want to test that table creation with tablespace works
# for partitioned tables.
my $ts = $node->basedir . '/regress_pgbench_tap_1_ts_dir';
mkdir $ts or die "cannot create directory $ts";

# the next commands will issue a syntax error if the path contains a "'"
$node->safe_psql('postgres',
	"CREATE TABLESPACE regress_pgbench_tap_1_ts LOCATION '$ts';");

# Test concurrent OID generation via pg_enum_oid_index.  This indirectly
# exercises LWLock and spinlock concurrency.
my $labels = join ',', map { "'l$_'" } 1 .. 1000;
$node->pgbench(
	'--no-vacuum --client=5 --protocol=prepared --transactions=25',
	0,
	[qr{processed: 125/125}],
	[qr{^$}],
	'concurrent OID generation',
	{
		'001_pgbench_concurrent_insert' =>
		  "CREATE TYPE pg_temp.e AS ENUM ($labels); DROP TYPE pg_temp.e;"
	});

# Trigger various connection errors
$node->pgbench(
	'no-such-database',
	1,
	[qr{^$}],
	[
		qr{connection to server .* failed},
		qr{FATAL:  database "no-such-database" does not exist}
	],
	'no such database');

$node->pgbench(
	'-S -t 1', 1, [],
	[qr{Perhaps you need to do initialization}],
	'run without init');

# Initialize pgbench tables scale 1
$node->pgbench(
	'-i', 0,
	[qr{^$}],
	[
		qr{creating tables},
		qr{vacuuming},
		qr{creating primary keys},
		qr{done in \d+\.\d\d s }
	],
	'pgbench scale 1 initialization',);

# Again, with all possible options
$node->pgbench(
	'--initialize --init-steps=dtpvg --scale=1 --unlogged-tables --fillfactor=98 --foreign-keys --quiet --tablespace=regress_pgbench_tap_1_ts --index-tablespace=regress_pgbench_tap_1_ts --partitions=2 --partition-method=hash',
	0,
	[qr{^$}i],
	[
		qr{dropping old tables},
		qr{creating tables},
		qr{creating 2 partitions},
		qr{vacuuming},
		qr{creating primary keys},
		qr{creating foreign keys},
		qr{(?!vacuuming)},    # no vacuum
		qr{done in \d+\.\d\d s }
	],
	'pgbench scale 1 initialization');

# Test interaction of --init-steps with legacy step-selection options
$node->pgbench(
	'--initialize --init-steps=dtpvGvv --no-vacuum --foreign-keys --unlogged-tables --partitions=3',
	0,
	[qr{^$}],
	[
		qr{dropping old tables},
		qr{creating tables},
		qr{creating 3 partitions},
		qr{creating primary keys},
		qr{generating data \(server-side\)},
		qr{creating foreign keys},
		qr{(?!vacuuming)},    # no vacuum
		qr{done in \d+\.\d\d s }
	],
	'pgbench --init-steps');

# Run all builtin scripts, for a few transactions each
$node->pgbench(
	'--transactions=5 -Dfoo=bla --client=2 --protocol=simple --builtin=t'
	  . ' --connect -n -v -n',
	0,
	[
		qr{builtin: TPC-B},
		qr{clients: 2\b},
		qr{processed: 10/10},
		qr{mode: simple},
		qr{maximum number of tries: 1}
	],
	[qr{^$}],
	'pgbench tpcb-like');

$node->pgbench(
	'--transactions=20 --client=5 -M extended --builtin=si -C --no-vacuum -s 1',
	0,
	[
		qr{builtin: simple update},
		qr{clients: 5\b},
		qr{threads: 1\b},
		qr{processed: 100/100},
		qr{mode: extended}
	],
	[qr{scale option ignored}],
	'pgbench simple update');

$node->pgbench(
	'-t 100 -c 7 -M prepared -b se --debug',
	0,
	[
		qr{builtin: select only},
		qr{clients: 7\b},
		qr{threads: 1\b},
		qr{processed: 700/700},
		qr{mode: prepared}
	],
	[
		qr{vacuum},    qr{client 0}, qr{client 1}, qr{sending},
		qr{receiving}, qr{executing}
	],
	'pgbench select only');

# check if threads are supported
my $nthreads = 2;

{
	my ($stderr);
	run_log([ 'pgbench', '-j', '2', '--bad-option' ], '2>', \$stderr);
	$nthreads = 1 if $stderr =~ m/threads are not supported on this platform/;
}

# run custom scripts
$node->pgbench(
	"-t 100 -c 1 -j $nthreads -M prepared -n",
	0,
	[
		qr{type: multiple scripts},
		qr{mode: prepared},
		qr{script 1: .*/001_pgbench_custom_script_1},
		qr{weight: 2},
		qr{script 2: .*/001_pgbench_custom_script_2},
		qr{weight: 1},
		qr{processed: 100/100}
	],
	[qr{^$}],
	'pgbench custom scripts',
	{
		'001_pgbench_custom_script_1@1' => q{-- select only
\set aid random(1, :scale * 100000)
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
},
		'001_pgbench_custom_script_2@2' => q{-- special variables
BEGIN;
\set foo 1
-- cast are needed for typing under -M prepared
SELECT :foo::INT + :scale::INT * :client_id::INT AS bla;
COMMIT;
}
	});

$node->pgbench(
	'-n -t 10 -c 1 -M simple',
	0,
	[
		qr{type: .*/001_pgbench_custom_script_3},
		qr{processed: 10/10},
		qr{mode: simple}
	],
	[qr{^$}],
	'pgbench custom script',
	{
		'001_pgbench_custom_script_3' => q{-- select only variant
\set aid random(1, :scale * 100000)
BEGIN;
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
COMMIT;
}
	});

$node->pgbench(
	'-n -t 10 -c 2 -M extended',
	0,
	[
		qr{type: .*/001_pgbench_custom_script_4},
		qr{processed: 20/20},
		qr{mode: extended}
	],
	[qr{^$}],
	'pgbench custom script',
	{
		'001_pgbench_custom_script_4' => q{-- select only variant
\set aid random(1, :scale * 100000)
BEGIN;
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
COMMIT;
}
	});

# Verify server logging of query parameters.
# (This doesn't really belong here, but pgbench is a convenient way
# to issue commands using extended query mode with parameters.)

# 1. Logging neither with errors nor with statements
$node->append_conf('postgresql.conf',
	    "log_min_duration_statement = 0\n"
	  . "log_parameter_max_length = 0\n"
	  . "log_parameter_max_length_on_error = 0");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type json},
		qr{(?!unnamed portal with parameters)}
	],
	'server parameter logging',
	{
		'001_param_1' => q[select '{ invalid ' as value \gset
select $$'Valame Dios!' dijo Sancho; 'no le dije yo a vuestra merced que mirase bien lo que hacia?'$$ as long \gset
select column1::jsonb from (values (:value), (:long)) as q;
]
	});
my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
unlike(
	$log,
	qr[DETAIL:  parameters: \$1 = '\{ invalid ',],
	"no parameters logged");
$log = undef;

# 2. Logging truncated parameters on error, full with statements
$node->append_conf('postgresql.conf',
	    "log_parameter_max_length = -1\n"
	  . "log_parameter_max_length_on_error = 64");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  division by zero},
		qr{CONTEXT:  unnamed portal with parameters: \$1 = '1', \$2 = NULL}
	],
	'server parameter logging',
	{
		'001_param_2' => q{select '1' as one \gset
SELECT 1 / (random() / 2)::int, :one::int, :two::int;
}
	});
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type json},
		qr[CONTEXT:  JSON data, line 1: \{ invalid\.\.\.[\r\n]+unnamed portal with parameters: \$1 = '\{ invalid ', \$2 = '''Valame Dios!'' dijo Sancho; ''no le dije yo a vuestra merced que \.\.\.']m
	],
	'server parameter logging',
	{
		'001_param_3' => q[select '{ invalid ' as value \gset
select $$'Valame Dios!' dijo Sancho; 'no le dije yo a vuestra merced que mirase bien lo que hacia?'$$ as long \gset
select column1::jsonb from (values (:value), (:long)) as q;
]
	});
$log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
like(
	$log,
	qr[DETAIL:  parameters: \$1 = '\{ invalid ', \$2 = '''Valame Dios!'' dijo Sancho; ''no le dije yo a vuestra merced que mirase bien lo que hacia\?'''],
	"parameter report does not truncate");
$log = undef;

# 3. Logging full parameters on error, truncated with statements
$node->append_conf('postgresql.conf',
	    "log_min_duration_statement = -1\n"
	  . "log_parameter_max_length = 7\n"
	  . "log_parameter_max_length_on_error = -1");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  division by zero},
		qr{CONTEXT:  unnamed portal with parameters: \$1 = '1', \$2 = NULL}
	],
	'server parameter logging',
	{
		'001_param_4' => q{select '1' as one \gset
SELECT 1 / (random() / 2)::int, :one::int, :two::int;
}
	});

$node->append_conf('postgresql.conf', "log_min_duration_statement = 0");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type json},
		qr[CONTEXT:  JSON data, line 1: \{ invalid\.\.\.[\r\n]+unnamed portal with parameters: \$1 = '\{ invalid ', \$2 = '''Valame Dios!'' dijo Sancho; ''no le dije yo a vuestra merced que mirase bien lo que hacia\?']m
	],
	'server parameter logging',
	{
		'001_param_5' => q[select '{ invalid ' as value \gset
select $$'Valame Dios!' dijo Sancho; 'no le dije yo a vuestra merced que mirase bien lo que hacia?'$$ as long \gset
select column1::jsonb from (values (:value), (:long)) as q;
]
	});
$log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
like(
	$log,
	qr[DETAIL:  parameters: \$1 = '\{ inval\.\.\.', \$2 = '''Valame\.\.\.'],
	"parameter report truncates");
$log = undef;

# Check that bad parameters are reported during typinput phase of BIND
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type smallint: "1a"},
		qr{CONTEXT:  unnamed portal parameter \$2 = '1a'}
	],
	'server parameter logging',
	{
		'001_param_6' => q{select 42 as value1, '1a' as value2 \gset
select :value1::smallint, :value2::smallint;
}
	});

# Restore default logging config
$node->append_conf('postgresql.conf',
	    "log_min_duration_statement = -1\n"
	  . "log_parameter_max_length_on_error = 0\n"
	  . "log_parameter_max_length = -1");
$node->reload;

# test expressions
$node->pgbench(
	'--random-seed=5432 -t 1 -Dfoo=-10.1 -Dbla=false -Di=+3 -Dn=null -Dt=t -Df=of -Dd=1.0',
	0,
	[ qr{type: .*/001_pgbench_expressions}, qr{processed: 1/1} ],
	[
		qr{setting random seed to 5432\b},

		# After explicit seeding, the four random checks (1-3,20) are
		# deterministic; but see also magic values in checks 111,113.
		qr{command=1.: int 17\b},      # uniform random
		qr{command=2.: int 104\b},     # exponential random
		qr{command=3.: int 1498\b},    # gaussian random
		qr{command=4.: int 4\b},
		qr{command=5.: int 5\b},
		qr{command=6.: int 6\b},
		qr{command=7.: int 7\b},
		qr{command=8.: int 8\b},
		qr{command=9.: int 9\b},
		qr{command=10.: int 10\b},
		qr{command=11.: int 11\b},
		qr{command=12.: int 12\b},
		qr{command=15.: double 15\b},
		qr{command=16.: double 16\b},
		qr{command=17.: double 17\b},
		qr{command=20.: int 3\b},    # zipfian random
		qr{command=21.: double -27\b},
		qr{command=22.: double 1024\b},
		qr{command=23.: double 1\b},
		qr{command=24.: double 1\b},
		qr{command=25.: double -0.125\b},
		qr{command=26.: double -0.125\b},
		qr{command=27.: double -0.00032\b},
		qr{command=28.: double 8.50705917302346e\+0?37\b},
		qr{command=29.: double 1e\+0?30\b},
		qr{command=30.: boolean false\b},
		qr{command=31.: boolean true\b},
		qr{command=32.: int 32\b},
		qr{command=33.: int 33\b},
		qr{command=34.: double 34\b},
		qr{command=35.: int 35\b},
		qr{command=36.: int 36\b},
		qr{command=37.: double 37\b},
		qr{command=38.: int 38\b},
		qr{command=39.: int 39\b},
		qr{command=40.: boolean true\b},
		qr{command=41.: null\b},
		qr{command=42.: null\b},
		qr{command=43.: boolean true\b},
		qr{command=44.: boolean true\b},
		qr{command=45.: boolean true\b},
		qr{command=46.: int 46\b},
		qr{command=47.: boolean true\b},
		qr{command=48.: boolean true\b},
		qr{command=49.: int -5817877081768721676\b},
		qr{command=50.: boolean true\b},
		qr{command=51.: int -7793829335365542153\b},
		qr{command=52.: int -?\d+\b},
		qr{command=53.: boolean true\b},
		qr{command=65.: int 65\b},
		qr{command=74.: int 74\b},
		qr{command=83.: int 83\b},
		qr{command=86.: int 86\b},
		qr{command=93.: int 93\b},
		qr{command=95.: int 0\b},
		qr{command=96.: int 1\b},                       # :scale
		qr{command=97.: int 0\b},                       # :client_id
		qr{command=98.: int 5432\b},                    # :random_seed
		qr{command=99.: int -9223372036854775808\b},    # min int
		qr{command=100.: int 9223372036854775807\b},    # max int
		    # pseudorandom permutation tests
		qr{command=101.: boolean true\b},
		qr{command=102.: boolean true\b},
		qr{command=103.: boolean true\b},
		qr{command=104.: boolean true\b},
		qr{command=105.: boolean true\b},
		qr{command=109.: boolean true\b},
		qr{command=110.: boolean true\b},
		qr{command=111.: boolean true\b},
		qr{command=113.: boolean true\b},
	],
	'pgbench expressions',
	{
		'001_pgbench_expressions' => q{-- integer functions
\set i1 debug(random(10, 19))
\set i2 debug(random_exponential(100, 199, 10.0))
\set i3 debug(random_gaussian(1000, 1999, 10.0))
\set i4 debug(abs(-4))
\set i5 debug(greatest(5, 4, 3, 2))
\set i6 debug(11 + least(-5, -4, -3, -2))
\set i7 debug(int(7.3))
-- integer arithmetic and bit-wise operators
\set i8 debug(17 / (4|1) + ( 4 + (7 >> 2)))
\set i9 debug(- (3 * 4 - (-(~ 1) + -(~ 0))) / -1 + 3 % -1)
\set ia debug(10 + (0 + 0 * 0 - 0 / 1))
\set ib debug(:ia + :scale)
\set ic debug(64 % (((2 + 1 * 2 + (1 # 2) | 4 * (2 & 11)) - (1 << 2)) + 2))
-- double functions and operators
\set d1 debug(sqrt(+1.5 * 2.0) * abs(-0.8E1))
\set d2 debug(double(1 + 1) * (-75.0 / :foo))
\set pi debug(pi() * 4.9)
\set d4 debug(greatest(4, 2, -1.17) * 4.0 * Ln(Exp(1.0)))
\set d5 debug(least(-5.18, .0E0, 1.0/0) * -3.3)
-- reset variables
\set i1 0
\set d1 false
-- yet another integer function
\set id debug(random_zipfian(1, 9, 1.3))
--- pow and power
\set poweri debug(pow(-3,3))
\set powerd debug(pow(2.0,10))
\set poweriz debug(pow(0,0))
\set powerdz debug(pow(0.0,0.0))
\set powernegi debug(pow(-2,-3))
\set powernegd debug(pow(-2.0,-3.0))
\set powernegd2 debug(power(-5.0,-5.0))
\set powerov debug(pow(9223372036854775807, 2))
\set powerov2 debug(pow(10,30))
-- comparisons and logical operations
\set c0 debug(1.0 = 0.0 and 1.0 != 0.0)
\set c1 debug(0 = 1 Or 1.0 = 1)
\set c4 debug(case when 0 < 1 then 32 else 0 end)
\set c5 debug(case when true then 33 else 0 end)
\set c6 debug(case when false THEN -1 when 1 = 1 then 13 + 19 + 2.0 end )
\set c7 debug(case when (1 > 0) and (1 >= 0) and (0 < 1) and (0 <= 1) and (0 != 1) and (0 = 0) and (0 <> 1) then 35 else 0 end)
\set c8 debug(CASE \
                WHEN (1.0 > 0.0) AND (1.0 >= 0.0) AND (0.0 < 1.0) AND (0.0 <= 1.0) AND \
                     (0.0 != 1.0) AND (0.0 = 0.0) AND (0.0 <> 1.0) AND (0.0 = 0.0) \
                  THEN 36 \
                  ELSE 0 \
              END)
\set c9 debug(CASE WHEN NOT FALSE THEN 3 * 12.3333334 END)
\set ca debug(case when false then 0 when 1-1 <> 0 then 1 else 38 end)
\set cb debug(10 + mod(13 * 7 + 12, 13) - mod(-19 * 11 - 17, 19))
\set cc debug(NOT (0 > 1) AND (1 <= 1) AND NOT (0 >= 1) AND (0 < 1) AND \
    NOT (false and true) AND (false OR TRUE) AND (NOT :f) AND (NOT FALSE) AND \
    NOT (NOT TRUE))
-- NULL value and associated operators
\set n0 debug(NULL + NULL * exp(NULL))
\set n1 debug(:n0)
\set n2 debug(NOT (:n0 IS NOT NULL OR :d1 IS NULL))
\set n3 debug(:n0 IS NULL AND :d1 IS NOT NULL AND :d1 NOTNULL)
\set n4 debug(:n0 ISNULL AND NOT :n0 IS TRUE AND :n0 IS NOT FALSE)
\set n5 debug(CASE WHEN :n IS NULL THEN 46 ELSE NULL END)
-- use a variables of all types
\set n6 debug(:n IS NULL AND NOT :f AND :t)
-- conditional truth
\set cs debug(CASE WHEN 1 THEN TRUE END AND CASE WHEN 1.0 THEN TRUE END AND CASE WHEN :n THEN NULL ELSE TRUE END)
-- hash functions
\set h0 debug(hash(10, 5432))
\set h1 debug(:h0 = hash_murmur2(10, 5432))
\set h3 debug(hash_fnv1a(10, 5432))
\set h4 debug(hash(10))
\set h5 debug(hash(10) = hash(10, :default_seed))
-- lazy evaluation
\set zy 0
\set yz debug(case when :zy = 0 then -1 else (1 / :zy) end)
\set yz debug(case when :zy = 0 or (1 / :zy) < 0 then -1 else (1 / :zy) end)
\set yz debug(case when :zy > 0 and (1 / :zy) < 0 then (1 / :zy) else 1 end)
-- substitute variables of all possible types
\set v0 NULL
\set v1 TRUE
\set v2 5432
\set v3 -54.21E-2
SELECT :v0, :v1, :v2, :v3;
-- if tests
\set nope 0
\if 1 > 0
\set id debug(65)
\elif 0
\set nope 1
\else
\set nope 1
\endif
\if 1 < 0
\set nope 1
\elif 1 > 0
\set ie debug(74)
\else
\set nope 1
\endif
\if 1 < 0
\set nope 1
\elif 1 < 0
\set nope 1
\else
\set if debug(83)
\endif
\if 1 = 1
\set ig debug(86)
\elif 0
\set nope 1
\endif
\if 1 = 0
\set nope 1
\elif 1 <> 0
\set ih debug(93)
\endif
-- must be zero if false branches where skipped
\set nope debug(:nope)
-- check automatic variables
\set sc debug(:scale)
\set ci debug(:client_id)
\set rs debug(:random_seed)
-- minint constant parsing
\set min debug(-9223372036854775808)
\set max debug(-(:min + 1))
-- parametric pseudorandom permutation function
\set t debug(permute(0, 2) + permute(1, 2) = 1)
\set t debug(permute(0, 3) + permute(1, 3) + permute(2, 3) = 3)
\set t debug(permute(0, 4) + permute(1, 4) + permute(2, 4) + permute(3, 4) = 6)
\set t debug(permute(0, 5) + permute(1, 5) + permute(2, 5) + permute(3, 5) + permute(4, 5) = 10)
\set t debug(permute(0, 16) + permute(1, 16) + permute(2, 16) + permute(3, 16) + \
             permute(4, 16) + permute(5, 16) + permute(6, 16) + permute(7, 16) + \
             permute(8, 16) + permute(9, 16) + permute(10, 16) + permute(11, 16) + \
             permute(12, 16) + permute(13, 16) + permute(14, 16) + permute(15, 16) = 120)
-- random sanity checks
\set size random(2, 1000)
\set v random(0, :size - 1)
\set p permute(:v, :size)
\set t debug(0 <= :p and :p < :size and :p = permute(:v + :size, :size) and :p <> permute(:v + 1, :size))
-- actual values
\set t debug(permute(:v, 1) = 0)
\set t debug(permute(0, 2, 5431) = 0 and permute(1, 2, 5431) = 1 and \
             permute(0, 2, 5433) = 1 and permute(1, 2, 5433) = 0)
-- check permute's portability across architectures
\set size debug(:max - 10)
\set t debug(permute(:size-1, :size, 5432) = 520382784483822430 and \
             permute(:size-2, :size, 5432) = 1143715004660802862 and \
             permute(:size-3, :size, 5432) = 447293596416496998 and \
             permute(:size-4, :size, 5432) = 916527772266572956 and \
             permute(:size-5, :size, 5432) = 2763809008686028849 and \
             permute(:size-6, :size, 5432) = 8648551549198294572 and \
             permute(:size-7, :size, 5432) = 4542876852200565125)
}
	});

# random determinism when seeded
$node->safe_psql('postgres',
	'CREATE UNLOGGED TABLE seeded_random(seed INT8 NOT NULL, rand TEXT NOT NULL, val INTEGER NOT NULL);'
);

# same value to check for determinism
my $seed = int(rand(1000000000));
for my $i (1, 2)
{
	$node->pgbench(
		"--random-seed=$seed -t 1",
		0,
		[qr{processed: 1/1}],
		[qr{setting random seed to $seed\b}],
		"random seeded with $seed",
		{
			"001_pgbench_random_seed_$i" => q{-- test random functions
\set ur random(1000, 1999)
\set er random_exponential(2000, 2999, 2.0)
\set gr random_gaussian(3000, 3999, 3.0)
\set zr random_zipfian(4000, 4999, 1.5)
INSERT INTO seeded_random(seed, rand, val) VALUES
  (:random_seed, 'uniform', :ur),
  (:random_seed, 'exponential', :er),
  (:random_seed, 'gaussian', :gr),
  (:random_seed, 'zipfian', :zr);
}
		});
}

# check that all runs generated the same 4 values
my ($ret, $out, $err) = $node->psql('postgres',
	'SELECT seed, rand, val, COUNT(*) FROM seeded_random GROUP BY seed, rand, val'
);

ok($ret == 0,  "psql seeded_random count ok");
ok($err eq '', "psql seeded_random count stderr is empty");
ok($out =~ /\b$seed\|uniform\|1\d\d\d\|2/,
	"psql seeded_random count uniform");
ok( $out =~ /\b$seed\|exponential\|2\d\d\d\|2/,
	"psql seeded_random count exponential");
ok( $out =~ /\b$seed\|gaussian\|3\d\d\d\|2/,
	"psql seeded_random count gaussian");
ok($out =~ /\b$seed\|zipfian\|4\d\d\d\|2/,
	"psql seeded_random count zipfian");

$node->safe_psql('postgres', 'DROP TABLE seeded_random;');

# backslash commands
$node->pgbench(
	'-t 1', 0,
	[
		qr{type: .*/001_pgbench_backslash_commands},
		qr{processed: 1/1},
		qr{shell-echo-output}
	],
	[qr{command=8.: int 1\b}],
	'pgbench backslash commands',
	{
		'001_pgbench_backslash_commands' => q{-- run set
\set zero 0
\set one 1.0
-- sleep
\sleep :one ms
\sleep 100 us
\sleep 0 s
\sleep :zero
-- setshell and continuation
\setshell another_one\
  echo \
    :one
\set n debug(:another_one)
-- shell
\shell echo shell-echo-output
}
	});

# working \gset
$node->pgbench(
	'-t 1', 0,
	[ qr{type: .*/001_pgbench_gset}, qr{processed: 1/1} ],
	[
		qr{command=3.: int 0\b},
		qr{command=5.: int 1\b},
		qr{command=6.: int 2\b},
		qr{command=8.: int 3\b},
		qr{command=10.: int 4\b},
		qr{command=12.: int 5\b}
	],
	'pgbench gset command',
	{
		'001_pgbench_gset' => q{-- test gset
-- no columns
SELECT \gset
-- one value
SELECT 0 AS i0 \gset
\set i debug(:i0)
-- two values
SELECT 1 AS i1, 2 AS i2 \gset
\set i debug(:i1)
\set i debug(:i2)
-- with prefix
SELECT 3 AS i3 \gset x_
\set i debug(:x_i3)
-- overwrite existing variable
SELECT 0 AS i4, 4 AS i4 \gset
\set i debug(:i4)
-- work on the last SQL command under \;
\; \; SELECT 0 AS i5 \; SELECT 5 AS i5 \; \; \gset
\set i debug(:i5)
}
	});
# \gset cannot accept more than one row, causing command to fail.
$node->pgbench(
	'-t 1', 2,
	[ qr{type: .*/001_pgbench_gset_two_rows}, qr{processed: 0/1} ],
	[qr{expected one row, got 2\b}],
	'pgbench gset command with two rows',
	{
		'001_pgbench_gset_two_rows' => q{
SELECT 5432 AS fail UNION SELECT 5433 ORDER BY 1 \gset
}
	});

# working \aset
# Valid cases.
$node->pgbench(
	'-t 1', 0,
	[ qr{type: .*/001_pgbench_aset}, qr{processed: 1/1} ],
	[ qr{command=3.: int 8\b},       qr{command=4.: int 7\b} ],
	'pgbench aset command',
	{
		'001_pgbench_aset' => q{
-- test aset, which applies to a combined query
\; SELECT 6 AS i6 \; SELECT 7 AS i7 \; \aset
-- unless it returns more than one row, last is kept
SELECT 8 AS i6 UNION SELECT 9 ORDER BY 1 DESC \aset
\set i debug(:i6)
\set i debug(:i7)
}
	});
# Empty result set with \aset, causing command to fail.
$node->pgbench(
	'-t 1', 2,
	[ qr{type: .*/001_pgbench_aset_empty}, qr{processed: 0/1} ],
	[
		qr{undefined variable \"i8\"},
		qr{evaluation of meta-command failed\b}
	],
	'pgbench aset command with empty result',
	{
		'001_pgbench_aset_empty' => q{
-- empty result
\; SELECT 5432 AS i8 WHERE FALSE \; \aset
\set i debug(:i8)
}
	});

# Working \startpipeline
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[ qr{type: .*/001_pgbench_pipeline}, qr{actually processed: 1/1} ],
	[],
	'working \startpipeline',
	{
		'001_pgbench_pipeline' => q{
-- test startpipeline
\startpipeline
} . "select 1;\n" x 10 . q{
\endpipeline
}
	});

# Working \startpipeline in prepared query mode
$node->pgbench(
	'-t 1 -n -M prepared',
	0,
	[ qr{type: .*/001_pgbench_pipeline_prep}, qr{actually processed: 1/1} ],
	[],
	'working \startpipeline',
	{
		'001_pgbench_pipeline_prep' => q{
-- test startpipeline
\startpipeline
} . "select 1;\n" x 10 . q{
\endpipeline
}
	});

# Try \startpipeline twice
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[qr{already in pipeline mode}],
	'error: call \startpipeline twice',
	{
		'001_pgbench_pipeline_2' => q{
-- startpipeline twice
\startpipeline
\startpipeline
}
	});

# Try to end a pipeline that hasn't started
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[qr{not in pipeline mode}],
	'error: \endpipeline with no start',
	{
		'001_pgbench_pipeline_3' => q{
-- pipeline not started
\endpipeline
}
	});

# Try \gset in pipeline mode
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[qr{gset is not allowed in pipeline mode}],
	'error: \gset not allowed in pipeline mode',
	{
		'001_pgbench_pipeline_4' => q{
\startpipeline
select 1 \gset f
\endpipeline
}
	});


# trigger many expression errors
my @errors = (

	# [ test name, expected status, expected stderr, script ]
	# SQL
	[
		'sql syntax error',
		2,
		[
			qr{ERROR:  syntax error},
			qr{prepared statement .* does not exist}
		],
		q{-- SQL syntax error
    SELECT 1 + ;
}
	],
	[
		'sql too many args', 1,
		[qr{statement has too many arguments.*\b255\b}],
		q{-- MAX_ARGS=256 for prepared
\set i 0
SELECT LEAST(} . join(', ', (':i') x 256) . q{)}
	],

	# SHELL
	[
		'shell bad command',                    2,
		[qr{\(shell\) .* meta-command failed}], q{\shell no-such-command}
	],
	[
		'shell undefined variable', 2,
		[qr{undefined variable ":nosuchvariable"}],
		q{-- undefined variable in shell
\shell echo ::foo :nosuchvariable
}
	],
	[ 'shell missing command', 1, [qr{missing command }], q{\shell} ],
	[
		'shell too many args', 1, [qr{too many arguments in command "shell"}],
		q{-- 256 arguments to \shell
\shell echo } . join(' ', ('arg') x 255)
	],

	# SET
	[
		'set syntax error',                  1,
		[qr{syntax error in command "set"}], q{\set i 1 +}
	],
	[
		'set no such function',         1,
		[qr{unexpected function name}], q{\set i noSuchFunction()}
	],
	[
		'set invalid variable name', 2,
		[qr{invalid variable name}], q{\set . 1}
	],
	[ 'set division by zero', 2, [qr{division by zero}], q{\set i 1/0} ],
	[
		'set undefined variable',
		2,
		[qr{undefined variable "nosuchvariable"}],
		q{\set i :nosuchvariable}
	],
	[ 'set unexpected char', 1, [qr{unexpected character .;.}], q{\set i ;} ],
	[
		'set too many args',
		2,
		[qr{too many function arguments}],
		q{\set i least(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16)}
	],
	[
		'set empty random range',          2,
		[qr{empty range given to random}], q{\set i random(5,3)}
	],
	[
		'set random range too large',    2,
		[qr{random range is too large}], q{\set i random(:minint, :maxint)}
	],
	[
		'set gaussian param too small',
		2,
		[qr{gaussian param.* at least 2}],
		q{\set i random_gaussian(0, 10, 1.0)}
	],
	[
		'set exponential param greater 0',
		2,
		[qr{exponential parameter must be greater }],
		q{\set i random_exponential(0, 10, 0.0)}
	],
	[
		'set zipfian param to 1',
		2,
		[qr{zipfian parameter must be in range \[1\.001, 1000\]}],
		q{\set i random_zipfian(0, 10, 1)}
	],
	[
		'set zipfian param too large',
		2,
		[qr{zipfian parameter must be in range \[1\.001, 1000\]}],
		q{\set i random_zipfian(0, 10, 1000000)}
	],
	[
		'set non numeric value',                     2,
		[qr{malformed variable "foo" value: "bla"}], q{\set i :foo + 1}
	],
	[ 'set no expression',    1, [qr{syntax error}],      q{\set i} ],
	[ 'set missing argument', 1, [qr{missing argument}i], q{\set} ],
	[
		'set not a bool',                      2,
		[qr{cannot coerce double to boolean}], q{\set b NOT 0.0}
	],
	[
		'set not an int',                   2,
		[qr{cannot coerce boolean to int}], q{\set i TRUE + 2}
	],
	[
		'set not a double',                    2,
		[qr{cannot coerce boolean to double}], q{\set d ln(TRUE)}
	],
	[
		'set case error',
		1,
		[qr{syntax error in command "set"}],
		q{\set i CASE TRUE THEN 1 ELSE 0 END}
	],
	[
		'set random error',                 2,
		[qr{cannot coerce boolean to int}], q{\set b random(FALSE, TRUE)}
	],
	[
		'set number of args mismatch',        1,
		[qr{unexpected number of arguments}], q{\set d ln(1.0, 2.0))}
	],
	[
		'set at least one arg',               1,
		[qr{at least one argument expected}], q{\set i greatest())}
	],

	# SET: ARITHMETIC OVERFLOW DETECTION
	[
		'set double to int overflow',         2,
		[qr{double to int overflow for 100}], q{\set i int(1E32)}
	],
	[
		'set bigint add overflow', 2,
		[qr{int add out}],         q{\set i (1<<62) + (1<<62)}
	],
	[
		'set bigint sub overflow',
		2, [qr{int sub out}], q{\set i 0 - (1<<62) - (1<<62) - (1<<62)}
	],
	[
		'set bigint mul overflow', 2,
		[qr{int mul out}], q{\set i 2 * (1<<62)}
	],
	[
		'set bigint div out of range', 2,
		[qr{bigint div out of range}], q{\set i :minint / -1}
	],

	# SETSHELL
	[
		'setshell not an int',                2,
		[qr{command must return an integer}], q{\setshell i echo -n one}
	],
	[ 'setshell missing arg', 1, [qr{missing argument }], q{\setshell var} ],
	[
		'setshell no such command',   2,
		[qr{could not read result }], q{\setshell var no-such-command}
	],

	# SLEEP
	[
		'sleep undefined variable',      2,
		[qr{sleep: undefined variable}], q{\sleep :nosuchvariable}
	],
	[
		'sleep too many args',    1,
		[qr{too many arguments}], q{\sleep too many args}
	],
	[
		'sleep missing arg', 1,
		[ qr{missing argument}, qr{\\sleep} ], q{\sleep}
	],
	[
		'sleep unknown unit',         1,
		[qr{unrecognized time unit}], q{\sleep 1 week}
	],

	# MISC
	[
		'misc invalid backslash command',         1,
		[qr{invalid command .* "nosuchcommand"}], q{\nosuchcommand}
	],
	[ 'misc empty script', 1, [qr{empty command list for script}], q{} ],
	[
		'bad boolean',                     2,
		[qr{malformed variable.*trueXXX}], q{\set b :badtrue or true}
	],
	[
		'invalid permute size',
		2,
		[qr{permute size parameter must be greater than zero}],
		q{\set i permute(0, 0)}
	],

	# GSET
	[
		'gset no row',                   2,
		[qr{expected one row, got 0\b}], q{SELECT WHERE FALSE \gset}
	],
	[ 'gset alone', 1, [qr{gset must follow an SQL command}], q{\gset} ],
	[
		'gset no SQL',                         1,
		[qr{gset must follow an SQL command}], q{\set i +1
\gset}
	],
	[
		'gset too many arguments', 1,
		[qr{too many arguments}],  q{SELECT 1 \gset a b}
	],
	[
		'gset after gset',                     1,
		[qr{gset must follow an SQL command}], q{SELECT 1 AS i \gset
\gset}
	],
	[
		'gset non SELECT',
		2,
		[qr{expected one row, got 0}],
		q{DROP TABLE IF EXISTS no_such_table \gset}
	],
	[
		'gset bad default name',                      2,
		[qr{error storing into variable \?column\?}], q{SELECT 1 \gset}
	],
	[
		'gset bad name',
		2,
		[qr{error storing into variable bad name!}],
		q{SELECT 1 AS "bad name!" \gset}
	],);

for my $e (@errors)
{
	my ($name, $status, $re, $script, $no_prepare) = @$e;
	$status != 0 or die "invalid expected status for test \"$name\"";
	my $n = '001_pgbench_error_' . $name;
	$n =~ s/ /_/g;
	$node->pgbench(
		'-n -t 1 -Dfoo=bla -Dnull=null -Dtrue=true -Done=1 -Dzero=0.0 -Dbadtrue=trueXXX'
		  . ' -Dmaxint=9223372036854775807 -Dminint=-9223372036854775808'
		  . ($no_prepare ? '' : ' -M prepared'),
		$status,
		[ $status == 1 ? qr{^$} : qr{processed: 0/1} ],
		$re,
		'pgbench script error: ' . $name,
		{ $n => $script });
}

# throttling
$node->pgbench(
	'-t 100 -S --rate=100000 --latency-limit=1000000 -c 2 -n -r',
	0,
	[ qr{processed: 200/200}, qr{builtin: select only} ],
	[qr{^$}],
	'pgbench throttling');

$node->pgbench(

	# given the expected rate and the 2 ms tx duration, at most one is executed
	'-t 10 --rate=100000 --latency-limit=1 -n -r',
	0,
	[
		qr{processed: [01]/10},
		qr{type: .*/001_pgbench_sleep},
		qr{above the 1.0 ms latency limit: [01]/}
	],
	[qr{^$}],
	'pgbench late throttling',
	{ '001_pgbench_sleep' => q{\sleep 2ms} });

# return a list of files from directory $dir matching regexpr $re
# this works around glob portability and escaping issues
sub list_files
{
	my ($dir, $re) = @_;
	opendir my $dh, $dir or die "cannot opendir $dir: $!";
	my @files = grep /$re/, readdir $dh;
	closedir $dh or die "cannot closedir $dir: $!";
	return map { $dir . '/' . $_ } @files;
}

# Check log contents and clean them up:
#   $dir: directory holding logs
#   $prefix: file prefix for per-thread logs
#   $nb: number of expected files
#   $min/$max: minimum and maximum number of lines in log files
#   $re: regular expression each log line has to match
sub check_pgbench_logs
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($dir, $prefix, $nb, $min, $max, $re) = @_;

	# $prefix is simple enough, thus does not need escaping
	my @logs = list_files($dir, qr{^$prefix\..*$});
	ok(@logs == $nb, "number of log files");
	ok(grep(/\/$prefix\.\d+(\.\d+)?$/, @logs) == $nb, "file name format");

	my $log_number = 0;
	for my $log (sort @logs)
	{
		# Check the contents of each log file.
		my $contents_raw = slurp_file($log);

		my @contents = split(/\n/, $contents_raw);
		my $clen = @contents;
		ok( $min <= $clen && $clen <= $max,
			"transaction count for $log ($clen)");
		my $clen_match = grep(/$re/, @contents);
		ok($clen_match == $clen, "transaction format for $prefix");

		# Show more information if some logs don't match
		# to help with debugging.
		if ($clen_match != $clen)
		{
			foreach my $log (@contents)
			{
				print "# Log entry not matching: $log\n"
				  unless $log =~ /$re/;
			}
		}
	}
	return;
}

my $bdir = $node->basedir;

# Run with sampling rate, 2 clients with 50 transactions each.
$node->pgbench(
	"-n -S -t 50 -c 2 --log --sampling-rate=0.5", 0,
	[ qr{select only}, qr{processed: 100/100} ], [qr{^$}],
	'pgbench logs', undef,
	"--log-prefix=$bdir/001_pgbench_log_2");
# The IDs of the clients (1st field) in the logs should be either 0 or 1.
check_pgbench_logs($bdir, '001_pgbench_log_2', 1, 8, 92,
	qr{^[01] \d{1,2} \d+ \d \d+ \d+$});

# Run with different read-only option pattern, 1 client with 10 transactions.
$node->pgbench(
	"-n -b select-only -t 10 -l", 0,
	[ qr{select only}, qr{processed: 10/10} ], [qr{^$}],
	'pgbench logs contents', undef,
	"--log-prefix=$bdir/001_pgbench_log_3");
# The ID of a single client (1st field) should match 0.
check_pgbench_logs($bdir, '001_pgbench_log_3', 1, 10, 10,
	qr{^0 \d{1,2} \d+ \d \d+ \d+$});

# abortion of the client if the script contains an incomplete transaction block
$node->pgbench(
	'--no-vacuum',
	2,
	[qr{processed: 1/10}],
	[
		qr{client 0 aborted: end of script reached without completing the last transaction}
	],
	'incomplete transaction block',
	{ '001_pgbench_incomplete_transaction_block' => q{BEGIN;SELECT 1;} });

# Test the concurrent update in the table row and deadlocks.

$node->safe_psql('postgres',
	    'CREATE UNLOGGED TABLE first_client_table (value integer); '
	  . 'CREATE UNLOGGED TABLE xy (x integer, y integer); '
	  . 'INSERT INTO xy VALUES (1, 2);');

# Serialization error and retry

local $ENV{PGOPTIONS} = "-c default_transaction_isolation=repeatable\\ read";

# Check that we have a serialization error and the same random value of the
# delta variable in the next try
my $err_pattern =
    "(client (0|1) sending UPDATE xy SET y = y \\+ -?\\d+\\b).*"
  . "client \\2 got an error in command 3 \\(SQL\\) of script 0; "
  . "ERROR:  could not serialize access due to concurrent update\\b.*"
  . "\\1";

$node->pgbench(
	"-n -c 2 -t 1 -d --verbose-errors --max-tries 2",
	0,
	[
		qr{processed: 2/2\b},
		qr{number of transactions retried: 1\b},
		qr{total number of retries: 1\b}
	],
	[qr/$err_pattern/s],
	'concurrent update with retrying',
	{
		'001_pgbench_serialization' => q{
-- What's happening:
-- The first client starts the transaction with the isolation level Repeatable
-- Read:
--
-- BEGIN;
-- UPDATE xy SET y = ... WHERE x = 1;
--
-- The second client starts a similar transaction with the same isolation level:
--
-- BEGIN;
-- UPDATE xy SET y = ... WHERE x = 1;
-- <waiting for the first client>
--
-- The first client commits its transaction, and the second client gets a
-- serialization error.

\set delta random(-5000, 5000)

-- The second client will stop here
SELECT pg_advisory_lock(0);

-- Start transaction with concurrent update
BEGIN;
UPDATE xy SET y = y + :delta WHERE x = 1 AND pg_advisory_lock(1) IS NOT NULL;

-- Wait for the second client
DO $$
DECLARE
  exists boolean;
  waiters integer;
BEGIN
  -- The second client always comes in second, and the number of rows in the
  -- table first_client_table reflect this. Here the first client inserts a row,
  -- so the second client will see a non-empty table when repeating the
  -- transaction after the serialization error.
  SELECT EXISTS (SELECT * FROM first_client_table) INTO STRICT exists;
  IF NOT exists THEN
	-- Let the second client begin
	PERFORM pg_advisory_unlock(0);
	-- And wait until the second client tries to get the same lock
	LOOP
	  SELECT COUNT(*) INTO STRICT waiters FROM pg_locks WHERE
	  locktype = 'advisory' AND objsubid = 1 AND
	  ((classid::bigint << 32) | objid::bigint = 1::bigint) AND NOT granted;
	  IF waiters = 1 THEN
		INSERT INTO first_client_table VALUES (1);

		-- Exit loop
		EXIT;
	  END IF;
	END LOOP;
  END IF;
END$$;

COMMIT;
SELECT pg_advisory_unlock_all();
}
	});

# Clean up

$node->safe_psql('postgres', 'DELETE FROM first_client_table;');

local $ENV{PGOPTIONS} = "-c default_transaction_isolation=read\\ committed";

# Deadlock error and retry

# Check that we have a deadlock error
$err_pattern =
    "client (0|1) got an error in command (3|5) \\(SQL\\) of script 0; "
  . "ERROR:  deadlock detected\\b";

$node->pgbench(
	"-n -c 2 -t 1 --max-tries 2 --verbose-errors",
	0,
	[
		qr{processed: 2/2\b},
		qr{number of transactions retried: 1\b},
		qr{total number of retries: 1\b}
	],
	[qr{$err_pattern}],
	'deadlock with retrying',
	{
		'001_pgbench_deadlock' => q{
-- What's happening:
-- The first client gets the lock 2.
-- The second client gets the lock 3 and tries to get the lock 2.
-- The first client tries to get the lock 3 and one of them gets a deadlock
-- error.
--
-- A client that does not get a deadlock error must hold a lock at the
-- transaction start. Thus in the end it releases all of its locks before the
-- client with the deadlock error starts a retry (we do not want any errors
-- again).

-- Since the client with the deadlock error has not released the blocking locks,
-- let's do this here.
SELECT pg_advisory_unlock_all();

-- The second client and the client with the deadlock error stop here
SELECT pg_advisory_lock(0);
SELECT pg_advisory_lock(1);

-- The second client and the client with the deadlock error always come after
-- the first and the number of rows in the table first_client_table reflects
-- this. Here the first client inserts a row, so in the future the table is
-- always non-empty.
DO $$
DECLARE
  exists boolean;
BEGIN
  SELECT EXISTS (SELECT * FROM first_client_table) INTO STRICT exists;
  IF exists THEN
	-- We are the second client or the client with the deadlock error

	-- The first client will take care by itself of this lock (see below)
	PERFORM pg_advisory_unlock(0);

	PERFORM pg_advisory_lock(3);

	-- The second client can get a deadlock here
	PERFORM pg_advisory_lock(2);
  ELSE
	-- We are the first client

	-- This code should not be used in a new transaction after an error
	INSERT INTO first_client_table VALUES (1);

	PERFORM pg_advisory_lock(2);
  END IF;
END$$;

DO $$
DECLARE
  num_rows integer;
  waiters integer;
BEGIN
  -- Check if we are the first client
  SELECT COUNT(*) FROM first_client_table INTO STRICT num_rows;
  IF num_rows = 1 THEN
	-- This code should not be used in a new transaction after an error
	INSERT INTO first_client_table VALUES (2);

	-- Let the second client begin
	PERFORM pg_advisory_unlock(0);
	PERFORM pg_advisory_unlock(1);

	-- Make sure the second client is ready for deadlock
	LOOP
	  SELECT COUNT(*) INTO STRICT waiters FROM pg_locks WHERE
	  locktype = 'advisory' AND
	  objsubid = 1 AND
	  ((classid::bigint << 32) | objid::bigint = 2::bigint) AND
	  NOT granted;

	  IF waiters = 1 THEN
	    -- Exit loop
		EXIT;
	  END IF;
	END LOOP;

	PERFORM pg_advisory_lock(0);
    -- And the second client took care by itself of the lock 1
  END IF;
END$$;

-- The first client can get a deadlock here
SELECT pg_advisory_lock(3);

SELECT pg_advisory_unlock_all();
}
	});

# Clean up
$node->safe_psql('postgres', 'DROP TABLE first_client_table, xy;');


# done
$node->safe_psql('postgres', 'DROP TABLESPACE regress_pgbench_tap_1_ts');
$node->stop;
done_testing();
