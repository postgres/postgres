# Copyright (c) 2024, PostgreSQL Global Development Group

pg_logicalinspect_sources = files('pg_logicalinspect.c')

if host_system == 'windows'
  pg_logicalinspect_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_logicalinspect',
    '--FILEDESC', 'pg_logicalinspect - functions to inspect logical decoding components',])
endif

pg_logicalinspect = shared_module('pg_logicalinspect',
  pg_logicalinspect_sources,
  kwargs: contrib_mod_args + {
      'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_logicalinspect

install_data(
  'pg_logicalinspect.control',
  'pg_logicalinspect--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_logicalinspect',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'specs': [
      'logical_inspect',
    ],
    'regress_args': [
      '--temp-config', files('logicalinspect.conf'),
    ],
    # see above
    'runningcheck': false,
  },
}
