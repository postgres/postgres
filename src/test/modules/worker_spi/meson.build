# FIXME: prevent install during main install, but not during test :/

test_worker_spi_sources = files(
  'worker_spi.c',
)

if host_system == 'windows'
  test_worker_spi_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'worker_spi',
    '--FILEDESC', 'worker_spi - background worker example',])
endif

test_worker_spi = shared_module('worker_spi',
  test_worker_spi_sources,
  kwargs: pg_mod_args,
)
testprep_targets += test_worker_spi

install_data(
  'worker_spi.control',
  'worker_spi--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'worker_spi',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'worker_spi',
    ],
    'regress_args': ['--temp-config', files('dynamic.conf'), '--dbname=contrib_regression'],
  },
}
