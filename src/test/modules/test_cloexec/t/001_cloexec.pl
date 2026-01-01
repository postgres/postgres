# Test O_CLOEXEC flag handling on Windows
#
# This test verifies that file handles opened with O_CLOEXEC are not
# inherited by child processes, while handles opened without O_CLOEXEC
# are inherited.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(run);
use File::Spec;
use Cwd 'abs_path';

if (!$PostgreSQL::Test::Utils::windows_os)
{
	plan skip_all => 'test is Windows-specific';
}

plan tests => 1;

my $test_prog;
foreach my $dir (split(/$Config::Config{path_sep}/, $ENV{PATH}))
{
	my $candidate = File::Spec->catfile($dir, 'test_cloexec.exe');
	if (-f $candidate && -x $candidate)
	{
		$test_prog = $candidate;
		last;
	}
}

if (!$test_prog)
{
	$test_prog = './test_cloexec.exe';
}

if (!-f $test_prog)
{
	BAIL_OUT("test program not found: $test_prog");
}

note("Using test program: $test_prog");

my ($stdout, $stderr);
my $result = run [ $test_prog ], '>', \$stdout, '2>', \$stderr;

note("Test program output:");
note($stdout) if $stdout;

if ($stderr)
{
	diag("Test program stderr:");
	diag($stderr);
}

ok($result && $stdout =~ /SUCCESS.*O_CLOEXEC behavior verified/s,
   "O_CLOEXEC prevents handle inheritance");

done_testing();
