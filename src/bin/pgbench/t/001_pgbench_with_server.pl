use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More;

# start a pgbench specific server
my $node = get_new_node('main');
$node->init;
$node->start;

# invoke pgbench
sub pgbench
{
	my ($opts, $stat, $out, $err, $name, $files) = @_;
	my @cmd = ('pgbench', split /\s+/, $opts);
	my @filenames = ();
	if (defined $files)
	{

		# note: files are ordered for determinism
		for my $fn (sort keys %$files)
		{
			my $filename = $node->basedir . '/' . $fn;
			push @cmd, '-f', $filename;

			# cleanup file weight
			$filename =~ s/\@\d+$//;

			#push @filenames, $filename;
			append_to_file($filename, $$files{$fn});
		}
	}
	$node->command_checks_all(\@cmd, $stat, $out, $err, $name);

	# cleanup?
	#unlink @filenames or die "cannot unlink files (@filenames): $!";
}

# Test concurrent insertion into table with UNIQUE oid column.  DDL expects
# GetNewOidWithIndex() to successfully avoid violating uniqueness for indexes
# like pg_class_oid_index and pg_proc_oid_index.  This indirectly exercises
# LWLock and spinlock concurrency.  This test makes a 5-MiB table.

$node->safe_psql('postgres',
	    'CREATE UNLOGGED TABLE oid_tbl () WITH OIDS; '
	  . 'ALTER TABLE oid_tbl ADD UNIQUE (oid);');

pgbench(
	'--no-vacuum --client=5 --protocol=prepared --transactions=25',
	0,
	[qr{processed: 125/125}],
	[qr{^$}],
	'concurrency OID generation',
	{   '001_pgbench_concurrent_oid_generation' =>
		  'INSERT INTO oid_tbl SELECT FROM generate_series(1,1000);' });

# cleanup
$node->safe_psql('postgres', 'DROP TABLE oid_tbl;');

# Trigger various connection errors
pgbench(
	'no-such-database',
	1,
	[qr{^$}],
	[   qr{connection to database "no-such-database" failed},
		qr{FATAL:  database "no-such-database" does not exist} ],
	'no such database');

pgbench(
	'-S -t 1', 1, [qr{^$}],
	[qr{Perhaps you need to do initialization}],
	'run without init');

# Initialize pgbench tables scale 1
pgbench(
	'-i', 0, [qr{^$}],
	[ qr{creating tables}, qr{vacuuming}, qr{creating primary keys}, qr{done\.} ],
	'pgbench scale 1 initialization',);

# Again, with all possible options
pgbench(
'--initialize --init-steps=dtpvg --scale=1 --unlogged-tables --fillfactor=98 --foreign-keys --quiet --tablespace=pg_default --index-tablespace=pg_default',
	0,
	[qr{^$}i],
	[   qr{dropping old tables},
		qr{creating tables},
		qr{vacuuming},
		qr{creating primary keys},
		qr{creating foreign keys},
		qr{done\.} ],
	'pgbench scale 1 initialization');

# Test interaction of --init-steps with legacy step-selection options
pgbench(
	'--initialize --init-steps=dtpvgvv --no-vacuum --foreign-keys --unlogged-tables',
	0, [qr{^$}],
	[   qr{dropping old tables},
		qr{creating tables},
		qr{creating primary keys},
		qr{.* of .* tuples \(.*\) done},
		qr{creating foreign keys},
		qr{done\.} ],
	'pgbench --init-steps');

# Run all builtin scripts, for a few transactions each
pgbench(
	'--transactions=5 -Dfoo=bla --client=2 --protocol=simple --builtin=t'
	  . ' --connect -n -v -n',
	0,
	[   qr{builtin: TPC-B},
		qr{clients: 2\b},
		qr{processed: 10/10},
		qr{mode: simple} ],
	[qr{^$}],
	'pgbench tpcb-like');

pgbench(
'--transactions=20 --client=5 -M extended --builtin=si -C --no-vacuum -s 1',
	0,
	[   qr{builtin: simple update},
		qr{clients: 5\b},
		qr{threads: 1\b},
		qr{processed: 100/100},
		qr{mode: extended} ],
	[qr{scale option ignored}],
	'pgbench simple update');

pgbench(
	'-t 100 -c 7 -M prepared -b se --debug',
	0,
	[   qr{builtin: select only},
		qr{clients: 7\b},
		qr{threads: 1\b},
		qr{processed: 700/700},
		qr{mode: prepared} ],
	[   qr{vacuum},    qr{client 0}, qr{client 1}, qr{sending},
		qr{receiving}, qr{executing} ],
	'pgbench select only');

# check if threads are supported
my $nthreads = 2;

{
	my ($stderr);
	run_log([ 'pgbench', '-j', '2', '--bad-option' ], '2>', \$stderr);
	$nthreads = 1 if $stderr =~ m/threads are not supported on this platform/;
}

# run custom scripts
pgbench(
	"-t 100 -c 1 -j $nthreads -M prepared -n",
	0,
	[   qr{type: multiple scripts},
		qr{mode: prepared},
		qr{script 1: .*/001_pgbench_custom_script_1},
		qr{weight: 2},
		qr{script 2: .*/001_pgbench_custom_script_2},
		qr{weight: 1},
		qr{processed: 100/100} ],
	[qr{^$}],
	'pgbench custom scripts',
	{   '001_pgbench_custom_script_1@1' => q{-- select only
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
} });

pgbench(
	'-n -t 10 -c 1 -M simple',
	0,
	[   qr{type: .*/001_pgbench_custom_script_3},
		qr{processed: 10/10},
		qr{mode: simple} ],
	[qr{^$}],
	'pgbench custom script',
	{   '001_pgbench_custom_script_3' => q{-- select only variant
\set aid random(1, :scale * 100000)
BEGIN;
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
COMMIT;
} });

pgbench(
	'-n -t 10 -c 2 -M extended',
	0,
	[   qr{type: .*/001_pgbench_custom_script_4},
		qr{processed: 20/20},
		qr{mode: extended} ],
	[qr{^$}],
	'pgbench custom script',
	{   '001_pgbench_custom_script_4' => q{-- select only variant
\set aid random(1, :scale * 100000)
BEGIN;
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
COMMIT;
} });

# test expressions
pgbench(
	'-t 1 -Dfoo=-10.1 -Dbla=false -Di=+3 -Dminint=-9223372036854775808',
	0,
	[ qr{type: .*/001_pgbench_expressions}, qr{processed: 1/1} ],
	[   qr{command=4.: int 4\b},
		qr{command=5.: int 5\b},
		qr{command=6.: int 6\b},
		qr{command=7.: int 7\b},
		qr{command=8.: int 8\b},
		qr{command=9.: int 9\b},
		qr{command=10.: int 10\b},
		qr{command=11.: int 11\b},
		qr{command=12.: int 12\b},
		qr{command=13.: double 13\b},
		qr{command=14.: double 14\b},
		qr{command=15.: double 15\b},
		qr{command=16.: double 16\b},
		qr{command=17.: double 17\b},
		qr{command=18.: double 18\b},
		qr{command=19.: double 19\b},
		qr{command=20.: double 20\b},
		qr{command=21.: int 9223372036854775807\b},
		qr{command=23.: int [1-9]\b}, ],
	'pgbench expressions',
	{   '001_pgbench_expressions' => q{-- integer functions
\set i1 debug(random(1, 100))
\set i2 debug(random_exponential(1, 100, 10.0))
\set i3 debug(random_gaussian(1, 100, 10.0))
\set i4 debug(abs(-4))
\set i5 debug(greatest(5, 4, 3, 2))
\set i6 debug(11 + least(-5, -4, -3, -2))
\set i7 debug(int(7.3))
-- integer operators
\set i8 debug(17 / 5 + 5)
\set i9 debug(- (3 * 4 - 3) / -1 + 3 % -1)
\set ia debug(10 + (0 + 0 * 0 - 0 / 1))
\set ib debug(:ia + :scale)
\set ic debug(64 % 13)
-- double functions
\set d1 debug(sqrt(3.0) * abs(-0.8E1))
\set d2 debug(double(1 + 1) * 7)
\set pi debug(pi() * 4.9)
\set d4 debug(greatest(4, 2, -1.17) * 4.0)
\set d5 debug(least(-5.18, .0E0, 1.0/0) * -3.3)
-- double operators
\set d6 debug((0.5 * 12.1 - 0.05) * (31.0 / 10))
\set d7 debug(11.1 + 7.9)
\set d8 debug(:foo * -2)
-- forced overflow
\set maxint debug(:minint - 1)
-- reset a variable
\set i1 0
-- yet another integer function
\set id debug(random_zipfian(1, 9, 1.3))
} });

# backslash commands
pgbench(
	'-t 1', 0,
	[   qr{type: .*/001_pgbench_backslash_commands},
		qr{processed: 1/1},
		qr{shell-echo-output} ],
	[qr{command=8.: int 2\b}],
	'pgbench backslash commands',
	{   '001_pgbench_backslash_commands' => q{-- run set
\set zero 0
\set one 1.0
-- sleep
\sleep :one ms
\sleep 100 us
\sleep 0 s
\sleep :zero
-- setshell and continuation
\setshell two\
  expr \
    1 + :one
\set n debug(:two)
-- shell
\shell echo shell-echo-output
} });

# trigger many expression errors
my @errors = (

	# [ test name, script number, status, stderr match ]
	# SQL
	[   'sql syntax error',
		0,
		[   qr{ERROR:  syntax error}, qr{prepared statement .* does not exist}
		],
		q{-- SQL syntax error
    SELECT 1 + ;
} ],
	[   'sql too many args', 1, [qr{statement has too many arguments.*\b9\b}],
		q{-- MAX_ARGS=10 for prepared
\set i 0
SELECT LEAST(:i, :i, :i, :i, :i, :i, :i, :i, :i, :i, :i);
} ],

	# SHELL
	[   'shell bad command',               0,
		[qr{meta-command 'shell' failed}], q{\shell no-such-command} ],
	[   'shell undefined variable', 0,
		[qr{undefined variable ":nosuchvariable"}],
		q{-- undefined variable in shell
\shell echo ::foo :nosuchvariable
} ],
	[ 'shell missing command', 1, [qr{missing command }], q{\shell} ],
	[   'shell too many args', 1, [qr{too many arguments in command "shell"}],
		q{-- 257 arguments to \shell
\shell echo \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F \
 0 1 2 3 4 5 6 7 8 9 A B C D E F
} ],

	# SET
	[   'set syntax error',                  1,
		[qr{syntax error in command "set"}], q{\set i 1 +} ],
	[   'set no such function',         1,
		[qr{unexpected function name}], q{\set i noSuchFunction()} ],
	[   'set invalid variable name', 0,
		[qr{invalid variable name}], q{\set . 1} ],
	[   'set int overflow',                   0,
		[qr{double to int overflow for 100}], q{\set i int(1E32)} ],
	[ 'set division by zero', 0, [qr{division by zero}], q{\set i 1/0} ],
	[   'set bigint out of range', 0,
		[qr{bigint out of range}], q{\set i 9223372036854775808 / -1} ],
	[   'set undefined variable',
		0,
		[qr{undefined variable "nosuchvariable"}],
		q{\set i :nosuchvariable} ],
	[ 'set unexpected char', 1, [qr{unexpected character .;.}], q{\set i ;} ],
	[   'set too many args',
		0,
		[qr{too many function arguments}],
		q{\set i least(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16)} ],
	[   'set empty random range',          0,
		[qr{empty range given to random}], q{\set i random(5,3)} ],
	[   'set random range too large',
		0,
		[qr{random range is too large}],
		q{\set i random(-9223372036854775808, 9223372036854775807)} ],
	[   'set gaussian param too small',
		0,
		[qr{gaussian param.* at least 2}],
		q{\set i random_gaussian(0, 10, 1.0)} ],
	[   'set exponential param greater 0',
		0,
		[qr{exponential parameter must be greater }],
		q{\set i random_exponential(0, 10, 0.0)} ],
	[	'set zipfian param to 1',
		0,
		[qr{zipfian parameter must be in range \(0, 1\) U \(1, \d+\]}],
		q{\set i random_zipfian(0, 10, 1)} ],
	[	'set zipfian param too large',
		0,
		[qr{zipfian parameter must be in range \(0, 1\) U \(1, \d+\]}],
		q{\set i random_zipfian(0, 10, 1000000)} ],
	[   'set non numeric value',                     0,
		[qr{malformed variable "foo" value: "bla"}], q{\set i :foo + 1} ],
	[ 'set no expression',    1, [qr{syntax error}],      q{\set i} ],
	[ 'set missing argument', 1, [qr{missing argument}i], q{\set} ],

	# SETSHELL
	[   'setshell not an int',                0,
		[qr{command must return an integer}], q{\setshell i echo -n one} ],
	[ 'setshell missing arg', 1, [qr{missing argument }], q{\setshell var} ],
	[   'setshell no such command',   0,
		[qr{could not read result }], q{\setshell var no-such-command} ],

	# SLEEP
	[   'sleep undefined variable',      0,
		[qr{sleep: undefined variable}], q{\sleep :nosuchvariable} ],
	[   'sleep too many args',    1,
		[qr{too many arguments}], q{\sleep too many args} ],
	[   'sleep missing arg', 1,
		[ qr{missing argument}, qr{\\sleep} ], q{\sleep} ],
	[   'sleep unknown unit',         1,
		[qr{unrecognized time unit}], q{\sleep 1 week} ],

	# MISC
	[   'misc invalid backslash command',         1,
		[qr{invalid command .* "nosuchcommand"}], q{\nosuchcommand} ],
	[ 'misc empty script', 1, [qr{empty command list for script}], q{} ],);

for my $e (@errors)
{
	my ($name, $status, $re, $script) = @$e;
	my $n = '001_pgbench_error_' . $name;
	$n =~ s/ /_/g;
	pgbench(
		'-n -t 1 -Dfoo=bla -M prepared',
		$status,
		[ $status ? qr{^$} : qr{processed: 0/1} ],
		$re,
		'pgbench script error: ' . $name,
		{ $n => $script });
}

# zipfian cache array overflow
pgbench(
	'-t 1', 0,
	[ qr{processed: 1/1}, qr{zipfian cache array overflowed 1 time\(s\)} ],
	[ qr{^} ],
	'pgbench zipfian array overflow on random_zipfian',
	{   '001_pgbench_random_zipfian' => q{
\set i random_zipfian(1, 100, 0.5)
\set i random_zipfian(2, 100, 0.5)
\set i random_zipfian(3, 100, 0.5)
\set i random_zipfian(4, 100, 0.5)
\set i random_zipfian(5, 100, 0.5)
\set i random_zipfian(6, 100, 0.5)
\set i random_zipfian(7, 100, 0.5)
\set i random_zipfian(8, 100, 0.5)
\set i random_zipfian(9, 100, 0.5)
\set i random_zipfian(10, 100, 0.5)
\set i random_zipfian(11, 100, 0.5)
\set i random_zipfian(12, 100, 0.5)
\set i random_zipfian(13, 100, 0.5)
\set i random_zipfian(14, 100, 0.5)
\set i random_zipfian(15, 100, 0.5)
\set i random_zipfian(16, 100, 0.5)
} });

# throttling
pgbench(
	'-t 100 -S --rate=100000 --latency-limit=1000000 -c 2 -n -r',
	0,
	[ qr{processed: 200/200}, qr{builtin: select only} ],
	[qr{^$}],
	'pgbench throttling');

pgbench(

   # given the expected rate and the 2 ms tx duration, at most one is executed
	'-t 10 --rate=100000 --latency-limit=1 -n -r',
	0,
	[   qr{processed: [01]/10},
		qr{type: .*/001_pgbench_sleep},
		qr{above the 1.0 ms latency limit: [01]/} ],
	[qr{^$}i],
	'pgbench late throttling',
	{ '001_pgbench_sleep' => q{\sleep 2ms} });

# check log contents and cleanup
sub check_pgbench_logs
{
	my ($prefix, $nb, $min, $max, $re) = @_;

	my @logs = <$prefix.*>;
	ok(@logs == $nb, "number of log files");
	ok(grep(/^$prefix\.\d+(\.\d+)?$/, @logs) == $nb, "file name format");

	my $log_number = 0;
	for my $log (sort @logs)
	{
		eval {
			open LOG, $log or die "$@";
			my @contents = <LOG>;
			my $clen     = @contents;
			ok( $min <= $clen && $clen <= $max,
				"transaction count for $log ($clen)");
			ok( grep($re, @contents) == $clen,
				"transaction format for $prefix");
			close LOG or die "$@";
		};
	}
	ok(unlink(@logs), "remove log files");
}

# with sampling rate
pgbench(
'-n -S -t 50 -c 2 --log --log-prefix=001_pgbench_log_2 --sampling-rate=0.5',
	0,
	[ qr{select only}, qr{processed: 100/100} ],
	[qr{^$}],
	'pgbench logs');

check_pgbench_logs('001_pgbench_log_2', 1, 8, 92,
	qr{^0 \d{1,2} \d+ \d \d+ \d+$});

# check log file in some detail
pgbench(
	'-n -b se -t 10 -l --log-prefix=001_pgbench_log_3',
	0, [ qr{select only}, qr{processed: 10/10} ],
	[qr{^$}], 'pgbench logs contents');

check_pgbench_logs('001_pgbench_log_3', 1, 10, 10,
	qr{^\d \d{1,2} \d+ \d \d+ \d+$});

# done
$node->stop;
done_testing();
