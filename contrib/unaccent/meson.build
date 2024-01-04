# Copyright (c) 2022-2024, PostgreSQL Global Development Group

unaccent_sources = files(
  'unaccent.c',
)

if host_system == 'windows'
  unaccent_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'unaccent',
    '--FILEDESC', 'unaccent - text search dictionary that removes accents',])
endif

unaccent = shared_module('unaccent',
  unaccent_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += unaccent

install_data(
  'unaccent--1.0--1.1.sql',
  'unaccent--1.1.sql',
  'unaccent.control',
  kwargs: contrib_data_args,
)

install_data(
  'unaccent.rules',
  install_dir: dir_data / 'tsearch_data'
)

# XXX: Implement downlo
tests += {
  'name': 'unaccent',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'unaccent',
    ],
  },
}
