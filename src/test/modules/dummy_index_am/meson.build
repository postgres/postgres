# FIXME: prevent install during main install, but not during test :/

dummy_index_am_sources = files(
  'dummy_index_am.c',
)

if host_system == 'windows'
  dummy_index_am_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dummy_index_am',
    '--FILEDESC', 'dummy_index_am - index access method template',])
endif

dummy_index_am = shared_module('dummy_index_am',
  dummy_index_am_sources,
  kwargs: pg_mod_args,
)
testprep_targets += dummy_index_am

install_data(
  'dummy_index_am.control',
  'dummy_index_am--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'dummy_index_am',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'reloptions',
    ],
  },
}
