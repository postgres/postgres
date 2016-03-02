=pod

=head1 NAME

PostgresNode - class representing PostgreSQL server instance

=head1 SYNOPSIS

  use PostgresNode;

  my $node = get_new_node('mynode');

  # Create a data directory with initdb
  $node->init();

  # Start the PostgreSQL server
  $node->start();

  # Change a setting and restart
  $node->append_conf('postgresql.conf', 'hot_standby = on');
  $node->restart('fast');

  # run a query with psql
  # like: psql -qAXt postgres -c 'SELECT 1;'
  $psql_stdout = $node->psql('postgres', 'SELECT 1');

  # run query every second until it returns 't'
  # or times out
  $node->poll_query_until('postgres', q|SELECT random() < 0.1;|')
    or print "timed out";

  # Do an online pg_basebackup
  my $ret = $node->backup('testbackup');

  # Restore it to create a new independent node (not a replica)
  my $replica = get_new_node('replica');
  $replica->init_from_backup($node, 'testbackup');
  $replica->start;

  # Stop the server
  $node->stop('fast');

=head1 DESCRIPTION

PostgresNode contains a set of routines able to work on a PostgreSQL node,
allowing to start, stop, backup and initialize it with various options.
The set of nodes managed by a given test is also managed by this module.

In addition to node management, PostgresNode instances have some wrappers
around Test::More functions to run commands with an environment set up to
point to the instance.

The IPC::Run module is required.

=cut

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

=pod

=head1 METHODS

=over

=item PostgresNode::new($class, $name, $pghost, $pgport)

Create a new PostgresNode instance. Does not initdb or start it.

You should generally prefer to use get_new_node() instead since it takes care
of finding port numbers, registering instances for cleanup, etc.

=cut

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
		_basedir  => TestLib::tempdir("data_" . $name),
		_name     => $name,
		_logfile  => "$TestLib::log_path/${testname}_${name}.log" };

	bless $self, $class;
	$self->dump_info;

	return $self;
}

=pod

=item $node->port()

Get the port number assigned to the host. This won't necessarily be a TCP port
open on the local host since we prefer to use unix sockets if possible.

Use $node->connstr() if you want a connection string.

=cut

sub port
{
	my ($self) = @_;
	return $self->{_port};
}

=pod

=item $node->host()

Return the host (like PGHOST) for this instance. May be a UNIX socket path.

Use $node->connstr() if you want a connection string.

=cut

sub host
{
	my ($self) = @_;
	return $self->{_host};
}

=pod

=item $node->basedir()

The directory all the node's files will be within - datadir, archive directory,
backups, etc.

=cut

sub basedir
{
	my ($self) = @_;
	return $self->{_basedir};
}

=pod

=item $node->name()

The name assigned to the node at creation time.

=cut

sub name
{
	my ($self) = @_;
	return $self->{_name};
}

=pod

=item $node->logfile()

Path to the PostgreSQL log file for this instance.

=cut

sub logfile
{
	my ($self) = @_;
	return $self->{_logfile};
}

=pod

=item $node->connstr()

Get a libpq connection string that will establish a connection to
this node. Suitable for passing to psql, DBD::Pg, etc.

=cut

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

=pod

=item $node->data_dir()

Returns the path to the data directory. postgresql.conf and pg_hba.conf are
always here.

=cut

sub data_dir
{
	my ($self) = @_;
	my $res = $self->basedir;
	return "$res/pgdata";
}

=pod

=item $node->archive_dir()

If archiving is enabled, WAL files go here.

=cut

sub archive_dir
{
	my ($self) = @_;
	my $basedir = $self->basedir;
	return "$basedir/archives";
}

=pod

=item $node->backup_dir()

The output path for backups taken with $node->backup()

=cut

sub backup_dir
{
	my ($self) = @_;
	my $basedir = $self->basedir;
	return "$basedir/backup";
}

=pod

=item $node->info()

Return a string containing human-readable diagnostic information (paths, etc)
about this node.

=cut

sub info
{
	my ($self) = @_;
	my $_info = '';
	open my $fh, '>', \$_info or die;
	print $fh "Name: " . $self->name . "\n";
	print $fh "Data directory: " . $self->data_dir . "\n";
	print $fh "Backup directory: " . $self->backup_dir . "\n";
	print $fh "Archive directory: " . $self->archive_dir . "\n";
	print $fh "Connection string: " . $self->connstr . "\n";
	print $fh "Log file: " . $self->logfile . "\n";
	close $fh or die;
	return $_info;
}

=pod

=item $node->dump_info()

Print $node->info()

=cut

sub dump_info
{
	my ($self) = @_;
	print $self->info;
}


# Internal method to set up trusted pg_hba.conf for replication.  Not
# documented because you shouldn't use it, it's called automatically if needed.
sub set_replication_conf
{
	my ($self) = @_;
	my $pgdata = $self->data_dir;

	$self->host eq $test_pghost
	  or die "set_replication_conf only works with the default host";

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

=pod

=item $node->init(...)

Initialize a new cluster for testing.

Authentication is set up so that only the current OS user can access the
cluster. On Unix, we use Unix domain socket connections, with the socket in
a directory that's only accessible to the current user to ensure that.
On Windows, we use SSPI authentication to ensure the same (by pg_regress
--config-auth).

pg_hba.conf is configured to allow replication connections. Pass the keyword
parameter hba_permit_replication => 0 to disable this.

WAL archiving can be enabled on this node by passing the keyword parameter
has_archiving => 1. This is disabled by default.

postgresql.conf can be set up for replication by passing the keyword
parameter allows_streaming => 1. This is disabled by default.

The new node is set up in a fast but unsafe configuration where fsync is
disabled.

=cut

sub init
{
	my ($self, %params) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $host   = $self->host;

	$params{hba_permit_replication} = 1
	  unless defined $params{hba_permit_replication};
	$params{allows_streaming} = 0 unless defined $params{allows_streaming};
	$params{has_archiving} = 0 unless defined $params{has_archiving};

	mkdir $self->backup_dir;
	mkdir $self->archive_dir;

	TestLib::system_or_bail('initdb', '-D', $pgdata, '-A', 'trust', '-N');
	TestLib::system_or_bail($ENV{PG_REGRESS}, '--config-auth', $pgdata);

	open my $conf, ">>$pgdata/postgresql.conf";
	print $conf "\n# Added by PostgresNode.pm\n";
	print $conf "fsync = off\n";
	print $conf "log_statement = all\n";
	print $conf "port = $port\n";

	if ($params{allows_streaming})
	{
		print $conf "wal_level = hot_standby\n";
		print $conf "max_wal_senders = 5\n";
		print $conf "wal_keep_segments = 20\n";
		print $conf "max_wal_size = 128MB\n";
		print $conf "shared_buffers = 1MB\n";
		print $conf "wal_log_hints = on\n";
		print $conf "hot_standby = on\n";
		print $conf "max_connections = 10\n";
	}

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

	$self->set_replication_conf if $params{hba_permit_replication};
	$self->enable_archiving if $params{has_archiving};
}

=pod

=item $node->append_conf(filename, str)

A shortcut method to append to files like pg_hba.conf and postgresql.conf.

Does no validation or sanity checking. Does not reload the configuration
after writing.

A newline is NOT automatically appended to the string.

=cut

sub append_conf
{
	my ($self, $filename, $str) = @_;

	my $conffile = $self->data_dir . '/' . $filename;

	TestLib::append_to_file($conffile, $str);
}

=pod

=item $node->backup(backup_name)

Create a hot backup with pg_basebackup in $node->backup_dir,
including the transaction logs. xlogs are fetched at the
end of the backup, not streamed.

You'll have to configure a suitable max_wal_senders on the
target server since it isn't done by default.

=cut

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

=pod

=item $node->init_from_backup(root_node, backup_name)

Initialize a node from a backup, which may come from this node or a different
node. root_node must be a PostgresNode reference, backup_name the string name
of a backup previously created on that node with $node->backup.

Does not start the node after initializing it.

A recovery.conf is not created.

pg_hba.conf is configured to allow replication connections. Pass the keyword
parameter hba_permit_replication => 0 to disable this.

Streaming replication can be enabled on this node by passing the keyword
parameter has_streaming => 1. This is disabled by default.

Restoring WAL segments from archives using restore_command can be enabled
by passing the keyword parameter has_restoring => 1. This is disabled by
default.

The backup is copied, leaving the original unmodified. pg_hba.conf is
unconditionally set to enable replication connections.

=cut

sub init_from_backup
{
	my ($self, $root_node, $backup_name, %params) = @_;
	my $backup_path = $root_node->backup_dir . '/' . $backup_name;
	my $port        = $self->port;
	my $node_name   = $self->name;
	my $root_name   = $root_node->name;

	$params{has_streaming} = 0 unless defined $params{has_streaming};
	$params{hba_permit_replication} = 1
	   unless defined $params{hba_permit_replication};
	$params{has_restoring} = 0 unless defined $params{has_restoring};

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
	$self->set_replication_conf if $params{hba_permit_replication};
	$self->enable_streaming($root_node) if $params{has_streaming};
	$self->enable_restoring($root_node) if $params{has_restoring};
}

=pod

=item $node->start()

Wrapper for pg_ctl -w start

Start the node and wait until it is ready to accept connections.

=cut

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

=pod

=item $node->stop(mode)

Stop the node using pg_ctl -m $mode and wait for it to stop.

=cut

sub stop
{
	my ($self, $mode) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $name   = $self->name;
	$mode = 'fast' unless defined $mode;
	print "### Stopping node \"$name\" using mode $mode\n";
	TestLib::system_log('pg_ctl', '-D', $pgdata, '-m', $mode, 'stop');
	$self->{_pid} = undef;
	$self->_update_pid;
}

=pod

=item $node->restart()

Wrapper for pg_ctl -w restart

=cut

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

=pod

=item $node->promote()

Wrapper for pg_ctl promote

=cut

sub promote
{
	my ($self)  = @_;
	my $port    = $self->port;
	my $pgdata  = $self->data_dir;
	my $logfile = $self->logfile;
	my $name    = $self->name;
	print "### Promoting node \"$name\"\n";
	TestLib::system_log('pg_ctl', '-D', $pgdata, '-l', $logfile,
						'promote');
}

# Internal routine to enable streaming replication on a standby node.
sub enable_streaming
{
	my ($self, $root_node)  = @_;
	my $root_connstr = $root_node->connstr;
	my $name    = $self->name;

	print "### Enabling streaming replication for node \"$name\"\n";
	$self->append_conf('recovery.conf', qq(
primary_conninfo='$root_connstr application_name=$name'
standby_mode=on
));
}

# Internal routine to enable archive recovery command on a standby node
sub enable_restoring
{
	my ($self, $root_node)  = @_;
	my $path = $root_node->archive_dir;
	my $name = $self->name;

	print "### Enabling WAL restore for node \"$name\"\n";

	# On Windows, the path specified in the restore command needs to use
	# double back-slashes to work properly and to be able to detect properly
	# the file targeted by the copy command, so the directory value used
	# in this routine, using only one back-slash, need to be properly changed
	# first. Paths also need to be double-quoted to prevent failures where
	# the path contains spaces.
	$path =~ s{\\}{\\\\}g if ($TestLib::windows_os);
	my $copy_command = $TestLib::windows_os ?
		qq{copy "$path\\\\%f" "%p"} :
		qq{cp $path/%f %p};

	$self->append_conf('recovery.conf', qq(
restore_command = '$copy_command'
standby_mode = on
));
}

# Internal routine to enable archiving
sub enable_archiving
{
	my ($self) = @_;
	my $path   = $self->archive_dir;
	my $name   = $self->name;

	print "### Enabling WAL archiving for node \"$name\"\n";

	# On Windows, the path specified in the restore command needs to use
	# double back-slashes to work properly and to be able to detect properly
	# the file targeted by the copy command, so the directory value used
	# in this routine, using only one back-slash, need to be properly changed
	# first. Paths also need to be double-quoted to prevent failures where
	# the path contains spaces.
	$path =~ s{\\}{\\\\}g if ($TestLib::windows_os);
	my $copy_command = $TestLib::windows_os ?
		qq{copy "%p" "$path\\\\%f"} :
		qq{cp %p $path/%f};

	# Enable archive_mode and archive_command on node
	$self->append_conf('postgresql.conf', qq(
archive_mode = on
archive_command = '$copy_command'
));
}

# Internal method
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

=pod

=item get_new_node(node_name)

Build a new PostgresNode object, assigning a free port number. Standalone
function that's automatically imported.

We also register the node, to avoid the port number from being reused
for another node even when this one is not active.

You should generally use this instead of PostgresNode::new(...).

=cut

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

# Attempt automatic cleanup
sub DESTROY
{
	my $self = shift;
	my $name = $self->name;
	return unless defined $self->{_pid};
	print "### Signalling QUIT to $self->{_pid} for node \"$name\"\n";
	TestLib::system_log('pg_ctl', 'kill', 'QUIT', $self->{_pid});
}

=pod

=item $node->teardown_node()

Do an immediate stop of the node

=cut

sub teardown_node
{
	my $self = shift;

	$self->stop('immediate');
}

=pod

=item $node->psql(dbname, sql)

Run a query with psql and return stdout, or on error print stderr.

Executes a query/script with psql and returns psql's standard output.  psql is
run in unaligned tuples-only quiet mode with psqlrc disabled so simple queries
will just return the result row(s) with fields separated by commas.

=cut

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

=pod

=item $node->poll_query_until(dbname, query)

Run a query once a second, until it returns 't' (i.e. SQL boolean true).
Continues polling if psql returns an error result. Times out after 90 seconds.

=cut

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

=pod

=item $node->command_ok(...)

Runs a shell command like TestLib::command_ok, but with PGPORT
set so that the command will default to connecting to this
PostgresNode.

=cut

sub command_ok
{
	my $self = shift;

	local $ENV{PGPORT} = $self->port;

	TestLib::command_ok(@_);
}

=pod

=item $node->command_fails(...) - TestLib::command_fails with our PGPORT

See command_ok(...)

=cut

sub command_fails
{
	my $self = shift;

	local $ENV{PGPORT} = $self->port;

	TestLib::command_fails(@_);
}

=pod

=item $node->command_like(...)

TestLib::command_like with our PGPORT. See command_ok(...)

=cut

sub command_like
{
	my $self = shift;

	local $ENV{PGPORT} = $self->port;

	TestLib::command_like(@_);
}

=pod

=item $node->issues_sql_like(cmd, expected_sql, test_name)

Run a command on the node, then verify that $expected_sql appears in the
server log file.

Reads the whole log file so be careful when working with large log outputs.
The log file is truncated prior to running the command, however.

=cut

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

=pod

=back

=cut

1;
