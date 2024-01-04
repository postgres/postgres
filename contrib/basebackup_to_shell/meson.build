# Copyright (c) 2022-2024, PostgreSQL Global Development Group

basebackup_to_shell_sources = files(
  'basebackup_to_shell.c',
)

if host_system == 'windows'
  basebackup_to_shell_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'basebackup_to_shell',
    '--FILEDESC', 'basebackup_to_shell - target basebackup to shell command',])
endif

basebackup_to_shell = shared_module('basebackup_to_shell',
  basebackup_to_shell_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += basebackup_to_shell

tests += {
  'name': 'basebackup_to_shell',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
    'env': {'GZIP_PROGRAM': gzip.found() ? gzip.path() : '',
            'TAR': tar.found() ? tar.path() : '' },
  },
}
