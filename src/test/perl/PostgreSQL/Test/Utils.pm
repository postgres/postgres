# Copyright (c) 2022, PostgreSQL Global Development Group

# allow use of release 15+ perl namespace in older branches
# just 'use' the older module name.
# We export the same names as the v15 module.
# See TestLib.pm for alias assignment that makes this all work.

package PostgreSQL::Test::Utils;

use strict;
use warnings;

use Exporter 'import';

use TestLib;

our @EXPORT = qw(
  generate_ascii_string
  slurp_dir
  slurp_file
  append_to_file
  check_mode_recursive
  chmod_recursive
  check_pg_config
  dir_symlink
  system_or_bail
  system_log
  run_log
  run_command
  pump_until

  command_ok
  command_fails
  command_exit_is
  program_help_ok
  program_version_ok
  program_options_handling_ok
  command_like
  command_like_safe
  command_fails_like
  command_checks_all

  $windows_os
  $is_msys2
  $use_unix_sockets
);

1;
