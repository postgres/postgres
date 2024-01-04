# Copyright (c) 2022-2024, PostgreSQL Global Development Group

if not uuid.found()
  subdir_done()
endif

uuid_ossp_sources = files(
  'uuid-ossp.c',
)

if host_system == 'windows'
  uuid_ossp_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'uuid-ossp',
    '--FILEDESC', 'uuid-ossp - UUID generation',])
endif

uuid_ossp = shared_module('uuid-ossp',
  uuid_ossp_sources,
  kwargs: contrib_mod_args + {
    'dependencies': [uuid, contrib_mod_args['dependencies']],
  },
)
contrib_targets += uuid_ossp

install_data(
  'uuid-ossp--1.0--1.1.sql',
  'uuid-ossp--1.1.sql',
  'uuid-ossp.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'uuid-ossp',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'uuid_ossp',
    ],
  },
}
