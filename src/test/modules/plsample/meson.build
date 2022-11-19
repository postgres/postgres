# FIXME: prevent install during main install, but not during test :/

plsample_sources = files(
  'plsample.c',
)

if host_system == 'windows'
  plsample_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'plsample',
    '--FILEDESC', 'PL/Sample - template for procedural language',])
endif

plsample = shared_module('plsample',
  plsample_sources,
  kwargs: pg_mod_args,
)
testprep_targets += plsample

install_data(
  'plsample.control',
  'plsample--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'plsample',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'plsample',
    ],
  },
}
