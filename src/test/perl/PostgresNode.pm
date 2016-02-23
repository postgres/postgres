# PostgresNode, class representing a data directory and postmaster.
#
# This contains a basic set of routines able to work on a PostgreSQL node,
# allowing to start, stop, backup and initialize it with various options.
# The set of nodes managed by a given test is also managed by this module.

package PostgresNode;

use strict;
use warnings;

use Config;
use Cwd;
use Exporter 'import';
use File::Basename;
use File::Spec;
use File::Temp ();
use IPC::Run;
use PostgresNode;
use RecursiveCopy;
use Test::More;
use TestLib ();

our @EXPORT = qw(
  get_new_node
);

our ($test_pghost, $last_port_assigned, @all_nodes);

INIT
{
	# PGHOST is set once and for all through a single series of tests when
	# this module is loaded.
	$test_pghost =
	  $TestLib::windows_os ? "127.0.0.1" : TestLib::tempdir_short;
	$ENV{PGHOST}     = $test_pghost;
	$ENV{PGDATABASE} = 'postgres';

	# Tracking of last port value assigned to accelerate free port lookup.
	$last_port_assigned = int(rand() * 16384) + 49152;
}

sub new
{
	my $class  = shift;
	my $name   = shift;
	my $pghost = shift;
	my $pgport = shift;
	my $testname = basename($0);
	$testname =~ s/\.[^.]+$//;
	my $self   = {
		_port     => $pgport,
		_host     => $pghost,
		_basedir  => TestLib::tempdir,
		_name     => $name,
		_logfile  => "$TestLib::log_path/${testname}_${name}.log" };

	bless $self, $class;
	$self->dump_info;

	return $self;
}

sub port
{
	my ($self) = @_;
	return $self->{_port};
}

sub host
{
	my ($self) = @_;
	return $self->{_host};
}

sub basedir
{
	my ($self) = @_;
	return $self->{_basedir};
}

sub name
{
	my ($self) = @_;
	return $self->{_name};
}

sub logfile
{
	my ($self) = @_;
	return $self->{_logfile};
}

sub connstr
{
	my ($self, $dbname) = @_;
	my $pgport = $self->port;
	my $pghost = $self->host;
	if (!defined($dbname))
	{
		return "port=$pgport host=$pghost";
	}
	return "port=$pgport host=$pghost dbname=$dbname";
}

sub data_dir
{
	my ($self) = @_;
	my $res = $self->basedir;
	return "$res/pgdata";
}

sub archive_dir
{
	my ($self) = @_;
	my $basedir = $self->basedir;
	return "$basedir/archives";
}

sub backup_dir
{
	my ($self) = @_;
	my $basedir = $self->basedir;
	return "$basedir/backup";
}

# Dump node information
sub dump_info
{
	my ($self) = @_;
	print "Name: " . $self->name . "\n";
	print "Data directory: " . $self->data_dir . "\n";
	print "Backup directory: " . $self->backup_dir . "\n";
	print "Archive directory: " . $self->archive_dir . "\n";
	print "Connection string: " . $self->connstr . "\n";
	print "Log file: " . $self->logfile . "\n";
}

sub set_replication_conf
{
	my ($self) = @_;
	my $pgdata = $self->data_dir;

	open my $hba, ">>$pgdata/pg_hba.conf";
	print $hba "\n# Allow replication (set up by PostgresNode.pm)\n";
	if (!$TestLib::windows_os)
	{
		print $hba "local replication all trust\n";
	}
	else
	{
		print $hba
"host replication all 127.0.0.1/32 sspi include_realm=1 map=regress\n";
	}
	close $hba;
}

# Initialize a new cluster for testing.
#
# Authentication is set up so that only the current OS user can access the
# cluster. On Unix, we use Unix domain socket connections, with the socket in
# a directory that's only accessible to the current user to ensure that.
# On Windows, we use SSPI authentication to ensure the same (by pg_regress
# --config-auth).
sub init
{
	my ($self, %params) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $host   = $self->host;

	$params{hba_permit_replication} = 1
	  if (!defined($params{hba_permit_replication}));

	mkdir $self->backup_dir;
	mkdir $self->archive_dir;

	TestLib::system_or_bail('initdb', '-D', $pgdata, '-A', 'trust', '-N');
	TestLib::system_or_bail($ENV{PG_REGRESS}, '--config-auth', $pgdata);

	open my $conf, ">>$pgdata/postgresql.conf";
	print $conf "\n# Added by PostgresNode.pm\n";
	print $conf "fsync = off\n";
	print $conf "log_statement = all\n";
	print $conf "port = $port\n";
	if ($TestLib::windows_os)
	{
		print $conf "listen_addresses = '$host'\n";
	}
	else
	{
		print $conf "unix_socket_directories = '$host'\n";
		print $conf "listen_addresses = ''\n";
	}
	close $conf;

	$self->set_replication_conf if ($params{hba_permit_replication});
}

sub append_conf
{
	my ($self, $filename, $str) = @_;

	my $conffile = $self->data_dir . '/' . $filename;

	TestLib::append_to_file($conffile, $str);
}

sub backup
{
	my ($self, $backup_name) = @_;
	my $backup_path = $self->backup_dir . '/' . $backup_name;
	my $port        = $self->port;
	my $name        = $self->name;

	print "# Taking backup $backup_name from node \"$name\"\n";
	TestLib::system_or_bail("pg_basebackup -D $backup_path -p $port -x");
	print "# Backup finished\n";
}

sub init_from_backup
{
	my ($self, $root_node, $backup_name) = @_;
	my $backup_path = $root_node->backup_dir . '/' . $backup_name;
	my $port        = $self->port;
	my $node_name   = $self->name;
	my $root_name   = $root_node->name;

	print
"# Initializing node \"$node_name\" from backup \"$backup_name\" of node \"$root_name\"\n";
	die "Backup \"$backup_name\" does not exist at $backup_path"
	  unless -d $backup_path;

	mkdir $self->backup_dir;
	mkdir $self->archive_dir;

	my $data_path = $self->data_dir;
	rmdir($data_path);
	RecursiveCopy::copypath($backup_path, $data_path);
	chmod(0700, $data_path);

	# Base configuration for this node
	$self->append_conf(
		'postgresql.conf',
		qq(
port = $port
));
	$self->set_replication_conf;
}

sub start
{
	my ($self) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $name   = $self->name;
	print("### Starting node \"$name\"\n");
	my $ret = TestLib::system_log('pg_ctl', '-w', '-D', $self->data_dir, '-l',
		$self->logfile, 'start');

	if ($ret != 0)
	{
		print "# pg_ctl failed; logfile:\n";
		print TestLib::slurp_file($self->logfile);
		BAIL_OUT("pg_ctl failed");
	}

	$self->_update_pid;
}

sub stop
{
	my ($self, $mode) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $name   = $self->name;
	$mode = 'fast' if (!defined($mode));
	print "### Stopping node \"$name\" using mode $mode\n";
	TestLib::system_log('pg_ctl', '-D', $pgdata, '-m', $mode, 'stop');
	$self->{_pid} = undef;
	$self->_update_pid;
}

sub restart
{
	my ($self)  = @_;
	my $port    = $self->port;
	my $pgdata  = $self->data_dir;
	my $logfile = $self->logfile;
	my $name    = $self->name;
	print "### Restarting node \"$name\"\n";
	TestLib::system_log('pg_ctl', '-D', $pgdata, '-w', '-l', $logfile,
		'restart');
	$self->_update_pid;
}

sub _update_pid
{
	my $self = shift;
	my $name = $self->name;

	# If we can open the PID file, read its first line and that's the PID we
	# want.  If the file cannot be opened, presumably the server is not
	# running; don't be noisy in that case.
	if (open my $pidfile, $self->data_dir . "/postmaster.pid")
	{
		chomp($self->{_pid} = <$pidfile>);
		print "# Postmaster PID for node \"$name\" is $self->{_pid}\n";
		close $pidfile;
		return;
	}

	$self->{_pid} = undef;
	print "# No postmaster PID\n";
}

#
# Cluster management functions
#

# Build a new PostgresNode object, assigning a free port number.
#
# We also register the node, to avoid the port number from being reused
# for another node even when this one is not active.
sub get_new_node
{
	my $name  = shift;
	my $found = 0;
	my $port  = $last_port_assigned;

	while ($found == 0)
	{
		# wrap correctly around range end
		$port = 49152 if ++$port >= 65536;
		print "# Checking for port $port\n";
		if (!TestLib::run_log([ 'pg_isready', '-p', $port ]))
		{
			$found = 1;

			# Found a potential candidate port number.  Check first that it is
			# not included in the list of registered nodes.
			foreach my $node (@all_nodes)
			{
				$found = 0 if ($node->port == $port);
			}
		}
	}

	print "# Found free port $port\n";

	# Lock port number found by creating a new node
	my $node = new PostgresNode($name, $test_pghost, $port);

	# Add node to list of nodes
	push(@all_nodes, $node);

	# And update port for next time
	$last_port_assigned = $port;

	return $node;
}

sub DESTROY
{
	my $self = shift;
	my $name = $self->name;
	return if not defined $self->{_pid};
	print "### Signalling QUIT to $self->{_pid} for node \"$name\"\n";
	TestLib::system_log('pg_ctl', 'kill', 'QUIT', $self->{_pid});
}

sub teardown_node
{
	my $self = shift;

	$self->stop('immediate');
}

sub psql
{
	my ($self, $dbname, $sql) = @_;

	my ($stdout, $stderr);
	my $name = $self->name;
	print("### Running SQL command on node \"$name\": $sql\n");

	IPC::Run::run [ 'psql', '-XAtq', '-d', $self->connstr($dbname), '-f',
		'-' ], '<', \$sql, '>', \$stdout, '2>', \$stderr
	  or die;

	if ($stderr ne "")
	{
		print "#### Begin standard error\n";
		print $stderr;
		print "#### End standard error\n";
	}
	chomp $stdout;
	$stdout =~ s/\r//g if $Config{osname} eq 'msys';
	return $stdout;
}

# Run a query once a second, until it returns 't' (i.e. SQL boolean true).
sub poll_query_until
{
	my ($self, $dbname, $query) = @_;

	my $max_attempts = 90;
	my $attempts     = 0;
	my ($stdout, $stderr);

	while ($attempts < $max_attempts)
	{
		my $cmd =
		  [ 'psql', '-XAt', '-c', $query, '-d', $self->connstr($dbname) ];
		my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;

		chomp($stdout);
		$stdout =~ s/\r//g if $Config{osname} eq 'msys';
		if ($stdout eq "t")
		{
			return 1;
		}

		# Wait a second before retrying.
		sleep 1;
		$attempts++;
	}

	# The query result didn't change in 90 seconds. Give up. Print the stderr
	# from the last attempt, hopefully that's useful for debugging.
	diag $stderr;
	return 0;
}

sub command_ok
{
	my $self = shift;

	local $ENV{PGPORT} = $self->port;

	TestLib::command_ok(@_);
}

sub command_fails
{
	my $self = shift;

	local $ENV{PGPORT} = $self->port;

	TestLib::command_fails(@_);
}

sub command_like
{
	my $self = shift;

	local $ENV{PGPORT} = $self->port;

	TestLib::command_like(@_);
}

# Run a command on the node, then verify that $expected_sql appears in the
# server log file.
sub issues_sql_like
{
	my ($self, $cmd, $expected_sql, $test_name) = @_;

	local $ENV{PGPORT} = $self->port;

	truncate $self->logfile, 0;
	my $result = TestLib::run_log($cmd);
	ok($result, "@$cmd exit code 0");
	my $log = TestLib::slurp_file($self->logfile);
	like($log, $expected_sql, "$test_name: SQL found in server log");
}

1;
