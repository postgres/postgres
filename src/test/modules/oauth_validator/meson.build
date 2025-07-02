# Copyright (c) 2025, PostgreSQL Global Development Group

validator_sources = files(
  'validator.c',
)

if host_system == 'windows'
  validator_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'validator',
    '--FILEDESC', 'validator - test OAuth validator module',])
endif

validator = shared_module('validator',
  validator_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += validator

fail_validator_sources = files(
  'fail_validator.c',
)

if host_system == 'windows'
  fail_validator_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'fail_validator',
    '--FILEDESC', 'fail_validator - failing OAuth validator module',])
endif

fail_validator = shared_module('fail_validator',
  fail_validator_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += fail_validator

magic_validator_sources = files(
  'magic_validator.c',
)

if host_system == 'windows'
  magic_validator_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'magic_validator',
    '--FILEDESC', 'magic_validator - ABI incompatible OAuth validator module',])
endif

magic_validator = shared_module('magic_validator',
  magic_validator_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += magic_validator

oauth_hook_client_sources = files(
  'oauth_hook_client.c',
)

if host_system == 'windows'
  oauth_hook_client_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'oauth_hook_client',
    '--FILEDESC', 'oauth_hook_client - test program for libpq OAuth hooks',])
endif

oauth_hook_client = executable('oauth_hook_client',
  oauth_hook_client_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  },
)
testprep_targets += oauth_hook_client

tests += {
  'name': 'oauth_validator',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_server.pl',
      't/002_client.pl',
    ],
    'env': {
      'PYTHON': python.full_path(),
      'with_libcurl': oauth_flow_supported ? 'yes' : 'no',
      'with_python': 'yes',
    },
  },
}
