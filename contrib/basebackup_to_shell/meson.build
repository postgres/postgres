basebackup_to_shell_sources = files(
  'basebackup_to_shell.c',
)

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
    'env': {'GZIP_PROGRAM': gzip.path(),
            'TAR': tar.path()},
  },
}
