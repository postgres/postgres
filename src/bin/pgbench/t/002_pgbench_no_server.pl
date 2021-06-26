#
# pgbench tests which do not need a server
#

use strict;
use warnings;

use TestLib;
use Test::More;

# create a directory for scripts
my $testname = $0;
$testname =~ s,.*/,,;
$testname =~ s/\.pl$//;

my $testdir = "$TestLib::tmp_check/t_${testname}_stuff";
mkdir $testdir
  or BAIL_OUT("could not create test directory \"${testdir}\": $!");

# invoke pgbench
sub pgbench
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($opts, $stat, $out, $err, $name) = @_;
	command_checks_all([ 'pgbench', split(/\s+/, $opts) ],
		$stat, $out, $err, $name);
	return;
}

# invoke pgbench with scripts
sub pgbench_scripts
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($opts, $stat, $out, $err, $name, $files) = @_;
	my @cmd = ('pgbench', split /\s+/, $opts);
	my @filenames = ();
	if (defined $files)
	{
		for my $fn (sort keys %$files)
		{
			my $filename = $testdir . '/' . $fn;

			# cleanup file weight if any
			$filename =~ s/\@\d+$//;

			# cleanup from prior runs
			unlink $filename;
			append_to_file($filename, $$files{$fn});
			push @cmd, '-f', $filename;
		}
	}
	command_checks_all(\@cmd, $stat, $out, $err, $name);
	return;
}

#
# Option various errors
#

my @options = (

	# name, options, stderr checks
	[
		'bad option',
		'-h home -p 5432 -U calvin -d --bad-option',
		[qr{--help.*more information}]
	],
	[
		'no file',
		'-f no-such-file',
		[qr{could not open file "no-such-file":}]
	],
	[
		'no builtin',
		'-b no-such-builtin',
		[qr{no builtin script .* "no-such-builtin"}]
	],
	[
		'invalid weight',
		'--builtin=select-only@one',
		[qr{invalid weight specification: \@one}]
	],
	[
		'invalid weight',
		'-b select-only@-1',
		[qr{weight spec.* out of range .*: -1}]
	],
	[ 'too many scripts', '-S ' x 129, [qr{at most 128 SQL scripts}] ],
	[ 'bad #clients', '-c three', [qr{invalid number of clients: "three"}] ],
	[
		'bad #threads', '-j eleven', [qr{invalid number of threads: "eleven"}]
	],
	[ 'bad scale', '-i -s two', [qr{invalid scaling factor: "two"}] ],
	[
		'invalid #transactions',
		'-t zil',
		[qr{invalid number of transactions: "zil"}]
	],
	[ 'invalid duration', '-T ten', [qr{invalid duration: "ten"}] ],
	[
		'-t XOR -T',
		'-N -l --aggregate-interval=5 --log-prefix=notused -t 1000 -T 1',
		[qr{specify either }]
	],
	[
		'-T XOR -t',
		'-P 1 --progress-timestamp -l --sampling-rate=0.001 -T 10 -t 1000',
		[qr{specify either }]
	],
	[ 'bad variable', '--define foobla', [qr{invalid variable definition}] ],
	[ 'invalid fillfactor', '-F 1',            [qr{invalid fillfactor}] ],
	[ 'invalid query mode', '-M no-such-mode', [qr{invalid query mode}] ],
	[
		'invalid progress', '--progress=0',
		[qr{invalid thread progress delay}]
	],
	[ 'invalid rate',    '--rate=0.0',          [qr{invalid rate limit}] ],
	[ 'invalid latency', '--latency-limit=0.0', [qr{invalid latency limit}] ],
	[
		'invalid sampling rate', '--sampling-rate=0',
		[qr{invalid sampling rate}]
	],
	[
		'invalid aggregate interval', '--aggregate-interval=-3',
		[qr{invalid .* seconds for}]
	],
	[
		'weight zero',
		'-b se@0 -b si@0 -b tpcb@0',
		[qr{weight must not be zero}]
	],
	[ 'init vs run', '-i -S',    [qr{cannot be used in initialization}] ],
	[ 'run vs init', '-S -F 90', [qr{cannot be used in benchmarking}] ],
	[ 'ambiguous builtin', '-b s', [qr{ambiguous}] ],
	[
		'--progress-timestamp => --progress', '--progress-timestamp',
		[qr{allowed only under}]
	],
	[
		'-I without init option',
		'-I dtg',
		[qr{cannot be used in benchmarking mode}]
	],
	[
		'invalid init step',
		'-i -I dta',
		[
			qr{unrecognized initialization step},
			qr{Allowed step characters are}
		]
	],
	[
		'bad random seed',
		'--random-seed=one',
		[
			qr{unrecognized random seed option "one"},
			qr{Expecting an unsigned integer, "time" or "rand"},
			qr{error while setting random seed from --random-seed option}
		]
	],
	[
		'bad partition method',
		'-i --partition-method=BAD',
		[ qr{"range"}, qr{"hash"}, qr{"BAD"} ]
	],
	[
		'bad partition number',
		'-i --partitions -1',
		[qr{invalid number of partitions: "-1"}]
	],
	[
		'partition method without partitioning',
		'-i --partition-method=hash',
		[qr{partition-method requires greater than zero --partitions}]
	],

	# logging sub-options
	[
		'sampling => log', '--sampling-rate=0.01',
		[qr{log sampling .* only when}]
	],
	[
		'sampling XOR aggregate',
		'-l --sampling-rate=0.1 --aggregate-interval=3',
		[qr{sampling .* aggregation .* cannot be used at the same time}]
	],
	[
		'aggregate => log', '--aggregate-interval=3',
		[qr{aggregation .* only when}]
	],
	[ 'log-prefix => log', '--log-prefix=x', [qr{prefix .* only when}] ],
	[
		'duration & aggregation',
		'-l -T 1 --aggregate-interval=3',
		[qr{aggr.* not be higher}]
	],
	[
		'duration % aggregation',
		'-l -T 5 --aggregate-interval=3',
		[qr{multiple}]
	],);

for my $o (@options)
{
	my ($name, $opts, $err_checks) = @$o;
	pgbench($opts, 1, [qr{^$}], $err_checks,
		'pgbench option error: ' . $name);
}

# Help
pgbench(
	'--help', 0,
	[
		qr{benchmarking tool for PostgreSQL},
		qr{Usage},
		qr{Initialization options:},
		qr{Common options:},
		qr{Report bugs to}
	],
	[qr{^$}],
	'pgbench help');

# Version
pgbench('-V', 0, [qr{^pgbench .PostgreSQL. }], [qr{^$}], 'pgbench version');

# list of builtins
pgbench(
	'-b list',
	0,
	[qr{^$}],
	[
		qr{Available builtin scripts:}, qr{tpcb-like},
		qr{simple-update},              qr{select-only}
	],
	'pgbench builtin list');

# builtin listing
pgbench(
	'--show-script se',
	0,
	[qr{^$}],
	[
		qr{select-only: }, qr{SELECT abalance FROM pgbench_accounts WHERE},
		qr{(?!UPDATE)},    qr{(?!INSERT)}
	],
	'pgbench builtin listing');

my @script_tests = (

	# name, err, { file => contents }
	[
		'missing endif',
		[qr{\\if without matching \\endif}],
		{ 'if-noendif.sql' => '\if 1' }
	],
	[
		'missing if on elif',
		[qr{\\elif without matching \\if}],
		{ 'elif-noif.sql' => '\elif 1' }
	],
	[
		'missing if on else',
		[qr{\\else without matching \\if}],
		{ 'else-noif.sql' => '\else' }
	],
	[
		'missing if on endif',
		[qr{\\endif without matching \\if}],
		{ 'endif-noif.sql' => '\endif' }
	],
	[
		'elif after else',
		[qr{\\elif after \\else}],
		{ 'else-elif.sql' => "\\if 1\n\\else\n\\elif 0\n\\endif" }
	],
	[
		'else after else',
		[qr{\\else after \\else}],
		{ 'else-else.sql' => "\\if 1\n\\else\n\\else\n\\endif" }
	],
	[
		'if syntax error',
		[qr{syntax error in command "if"}],
		{ 'if-bad.sql' => "\\if\n\\endif\n" }
	],
	[
		'elif syntax error',
		[qr{syntax error in command "elif"}],
		{ 'elif-bad.sql' => "\\if 0\n\\elif +\n\\endif\n" }
	],
	[
		'else syntax error',
		[qr{unexpected argument in command "else"}],
		{ 'else-bad.sql' => "\\if 0\n\\else BAD\n\\endif\n" }
	],
	[
		'endif syntax error',
		[qr{unexpected argument in command "endif"}],
		{ 'endif-bad.sql' => "\\if 0\n\\endif BAD\n" }
	],
	[
		'not enough arguments for least',
		[qr{at least one argument expected \(least\)}],
		{ 'bad-least.sql' => "\\set i least()\n" }
	],
	[
		'not enough arguments for greatest',
		[qr{at least one argument expected \(greatest\)}],
		{ 'bad-greatest.sql' => "\\set i greatest()\n" }
	],
	[
		'not enough arguments for hash',
		[qr{unexpected number of arguments \(hash\)}],
		{ 'bad-hash-1.sql' => "\\set i hash()\n" }
	],
	[
		'too many arguments for hash',
		[qr{unexpected number of arguments \(hash\)}],
		{ 'bad-hash-2.sql' => "\\set i hash(1,2,3)\n" }
	],
	# overflow
	[
		'bigint overflow 1',
		[qr{bigint constant overflow}],
		{ 'overflow-1.sql' => "\\set i 100000000000000000000\n" }
	],
	[
		'double overflow 2',
		[qr{double constant overflow}],
		{ 'overflow-2.sql' => "\\set d 1.0E309\n" }
	],
	[
		'double overflow 3',
		[qr{double constant overflow}],
		{ 'overflow-3.sql' => "\\set d .1E310\n" }
	],
	[
		'set i',
		[ qr{set i 1 }, qr{\^ error found here} ],
		{ 'set_i_op' => "\\set i 1 +\n" }
	],);

for my $t (@script_tests)
{
	my ($name, $err, $files) = @$t;
	pgbench_scripts('', 1, [qr{^$}], $err, 'pgbench option error: ' . $name,
		$files);
}

done_testing();
