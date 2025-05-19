
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bsepgsql\b/)
{
	plan skip_all =>
	  'Potentially unsafe test sepgsql not enabled in PG_TEST_EXTRA';
}

note "checking selinux environment";

# matchpathcon must be present to assess whether the installation environment
# is OK.
note "checking for matchpathcon";
if (system('matchpathcon -n . >/dev/null 2>&1') != 0)
{
	diag <<EOS;

The matchpathcon command must be available.
Please install it or update your PATH to include it
(it is typically in '/usr/sbin', which might not be in your PATH).
matchpathcon is typically included in the libselinux-utils package.
EOS
	die;
}

# runcon must be present to launch psql using the correct environment
note "checking for runcon";
if (system('runcon --help >/dev/null 2>&1') != 0)
{
	diag <<EOS;

The runcon command must be available.
runcon is typically included in the coreutils package.
EOS
	die;
}

# check sestatus too, since that lives in yet another package
note "checking for sestatus";
if (system('sestatus >/dev/null 2>&1') != 0)
{
	diag <<EOS;

The sestatus command must be available.
sestatus is typically included in the policycoreutils package.
EOS
	die;
}

# check that the user is running in the unconfined_t domain
note "checking current user domain";
my $DOMAIN = (split /:/, `id -Z 2>/dev/null`)[2];
note "current user domain is '$DOMAIN'";
if ($DOMAIN ne 'unconfined_t')
{
	diag <<'EOS';

The regression tests must be launched from the unconfined_t domain.

The unconfined_t domain is typically the default domain for user
shell processes.  If the default has been changed on your system,
you can revert the changes like this:

  $ sudo semanage login -d `whoami`

Or, you can add a setting to log in using the unconfined_t domain:

  $ sudo semanage login -a -s unconfined_u -r s0-s0:c0.c255 `whoami`

EOS
	die;
}

# SELinux must be configured in enforcing mode
note "checking selinux operating mode";
my $CURRENT_MODE =
  (split /: */, `LANG=C sestatus | grep '^Current mode:'`)[1];
chomp $CURRENT_MODE;
note "current operating mode is '$CURRENT_MODE'";
if ($CURRENT_MODE eq 'enforcing')
{
	# OK
}
elsif ($CURRENT_MODE eq 'permissive' || $CURRENT_MODE eq 'disabled')
{
	diag <<'EOS';

Before running the regression tests, SELinux must be enabled and
must be running in enforcing mode.

If SELinux is currently running in permissive mode, you can
switch to enforcing mode using the 'setenforce' command.

  $ sudo setenforce 1

The system default setting is configured in /etc/selinux/config,
or using a kernel boot parameter.
EOS
	die;
}
else
{
	diag <<EOS;

Unable to determine the current selinux operating mode.  Please
verify that the sestatus command is installed and in your PATH.
EOS
	die;
}

# 'sepgsql-regtest' policy module must be loaded
note "checking for sepgsql-regtest policy";
my $SELINUX_MNT = (split /: */, `sestatus | grep '^SELinuxfs mount:'`)[1];
chomp $SELINUX_MNT;
if ($SELINUX_MNT eq "")
{
	diag <<EOS;

Unable to find SELinuxfs mount point.

The sestatus command should report the location where SELinuxfs
is mounted, but did not do so.
EOS
	die;
}
if (!-e "${SELINUX_MNT}/booleans/sepgsql_regression_test_mode")
{
	diag <<'EOS';

The 'sepgsql-regtest' policy module appears not to be installed.
Without this policy installed, the regression tests will fail.
You can install this module using the following commands:

  $ make -f /usr/share/selinux/devel/Makefile
  $ sudo semodule -u sepgsql-regtest.pp

To confirm that the policy package is installed, use this command:

  $ sudo semodule -l | grep sepgsql

EOS
	die;
}

# Verify that sepgsql_regression_test_mode is active.
note "checking whether policy is enabled";
foreach
  my $policy ('sepgsql_regression_test_mode', 'sepgsql_enable_users_ddl')
{
	my $POLICY_STATUS = (split ' ', `getsebool $policy`)[2];
	note "$policy is '$POLICY_STATUS'";
	if ($POLICY_STATUS ne "on")
	{
		diag <<EOS;

The SELinux boolean '$policy' must be
turned on in order to enable the rules necessary to run the
regression tests.

EOS

		if ($POLICY_STATUS eq "")
		{
			diag <<EOS;
We attempted to determine the state of this Boolean using
'getsebool', but that command did not produce the expected
output.  Please verify that getsebool is available and in
your PATH.
EOS
		}
		else
		{
			diag <<EOS;
You can turn on this variable using the following commands:

  \$ sudo setsebool $policy on

For security reasons, it is suggested that you turn off this
variable when regression testing is complete and the associated
rules are no longer needed.
EOS
		}
		die;
	}
}


#
# checking complete - let's run the tests
#

note "running sepgsql regression tests";

my $node;

$node = PostgreSQL::Test::Cluster->new('test');
$node->init;
$node->append_conf('postgresql.conf', 'log_statement=none');

{
	local %ENV = $node->_get_env();

	my $result = run_log(
		[
			'postgres', '--single', '-F',
			'-c' => 'exit_on_error=true',
			'-D' => $node->data_dir,
			'template0'
		],
		'<' => $ENV{share_contrib_dir} . '/sepgsql.sql');
	ok($result, 'sepgsql installation script');
}

$node->append_conf('postgresql.conf', 'shared_preload_libraries=sepgsql');
$node->start;

my @tests = qw(label dml ddl alter misc);

# Check if the truncate permission exists in the loaded policy, and if so,
# run the truncate test
#
# Testing the TRUNCATE regression test can be done by manually adding
# the permission with CIL if necessary:
#     sudo semodule -cE base
#     sudo sed -i -E 's/(class db_table.*?) \)/\1 truncate\)/' base.cil
#     sudo semodule -i base.cil
push @tests, 'truncate' if -f '/sys/fs/selinux/class/db_table/perms/truncate';

$node->command_ok(
	[
		$ENV{PG_REGRESS},
		'--bindir' => '',
		'--inputdir' => '.',
		'--launcher' => './launcher',
		@tests
	],
	'sepgsql tests');

done_testing();
