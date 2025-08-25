package PostgreSQL::Test::TdeCluster;

use parent 'PostgreSQL::Test::Cluster';

use strict;
use warnings FATAL => 'all';

use List::Util                      ();
use PostgreSQL::Test::RecursiveCopy ();
use PostgreSQL::Test::Utils         ();

our ($tde_template_dir);

BEGIN
{
	$ENV{TDE_MODE_NOSKIP} = 0 unless defined($ENV{TDE_MODE_NOSKIP});
}

sub init
{
	my ($self, %params) = @_;

	$self->SUPER::init(%params);

	$self->SUPER::append_conf('postgresql.conf',
		'shared_preload_libraries = pg_tde');

	$self->_tde_init_principal_key;

	return;
}

sub append_conf
{
	my ($self, $filename, $str) = @_;

	if ($filename eq 'postgresql.conf' or $filename eq 'postgresql.auto.conf')
	{
		# TODO: Will not work with shared_preload_libraries= without any
		#       libraries, but no TAP test currently do that.
		$str =~
		  s/shared_preload_libraries *= *'?([^'\n]+)'?/shared_preload_libraries = 'pg_tde,$1'/;
	}

	$self->SUPER::append_conf($filename, $str);
}

sub pg_tde_dir
{
	my ($self) = @_;
	return $self->data_dir . '/pg_tde';
}

sub _tde_init_principal_key
{
	my ($self) = @_;

	my $tde_template_dir =
	  $PostgreSQL::Test::Utils::tmp_check . '/pg_tde_template';

	unless (-e $tde_template_dir)
	{
		my $temp_dir = PostgreSQL::Test::Utils::tempdir();
		mkdir $tde_template_dir;

		PostgreSQL::Test::Utils::system_log(
			'initdb',
			'-D' => $temp_dir,
			'--set' => 'shared_preload_libraries=pg_tde');

		_tde_init_sql_command(
			$temp_dir, 'postgres', qq(
			CREATE EXTENSION pg_tde;
			SELECT pg_tde_add_global_key_provider_file('global_test_provider', '$tde_template_dir/pg_tde_test_keys');
			SELECT pg_tde_create_key_using_global_key_provider('default_test_key', 'global_test_provider');
			SELECT pg_tde_set_default_key_using_global_key_provider('default_test_key', 'global_test_provider');
		));

		PostgreSQL::Test::Utils::system_log('cp', '-R', '-P', '-p',
			$temp_dir . '/pg_tde',
			$tde_template_dir);
	}

	PostgreSQL::Test::Utils::system_log('cp', '-R', '-P', '-p',
		$tde_template_dir . '/pg_tde',
		$self->pg_tde_dir);

	# We don't want clusters sharing the KMS file as any concurrent writes will
	# mess it up.
	PostgreSQL::Test::Utils::system_log(
		'cp', '-R', '-P', '-p',
		$tde_template_dir . '/pg_tde_test_keys',
		$self->basedir . '/pg_tde_test_keys');

	PostgreSQL::Test::Utils::system_log(
		'pg_tde_change_key_provider',
		'-D' => $self->data_dir,
		'1664',
		'global_test_provider',
		'file',
		$self->basedir . '/pg_tde_test_keys');
}

sub _tde_init_sql_command
{
	my ($datadir, $database, $sql) = @_;
	PostgreSQL::Test::Utils::run_log(
		[
			'postgres',
			'--single', '-j', '-F',
			'-D' => $datadir,
			'-c' => 'exit_on_error=true',
			'-c' => 'log_checkpoints=false',
			$database,
		],
		'<',
		\$sql);
}

1;
