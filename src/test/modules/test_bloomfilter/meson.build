# FIXME: prevent install during main install, but not during test :/

test_bloomfilter_sources = files(
  'test_bloomfilter.c',
)

if host_system == 'windows'
  test_bloomfilter_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_bloomfilter',
    '--FILEDESC', 'test_bloomfilter - test code for Bloom filter library',])
endif

test_bloomfilter = shared_module('test_bloomfilter',
  test_bloomfilter_sources,
  kwargs: pg_mod_args,
)
testprep_targets += test_bloomfilter

install_data(
  'test_bloomfilter.control',
  'test_bloomfilter--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_bloomfilter',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_bloomfilter',
    ],
  },
}
