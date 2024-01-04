# Copyright (c) 2022-2024, PostgreSQL Global Development Group

if not ssl.found()
  subdir_done()
endif

ssl_passphrase_callback_sources = files(
  'ssl_passphrase_func.c',
)

if host_system == 'windows'
  ssl_passphrase_callback_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ssl_passphrase_func',
    '--FILEDESC', 'callback function to provide a passphrase',])
endif

ssl_passphrase_callback = shared_module('ssl_passphrase_func',
  ssl_passphrase_callback_sources,
  kwargs: pg_test_mod_args + {
    'dependencies': [ssl, pg_mod_args['dependencies']],
  },
)
test_install_libs += ssl_passphrase_callback

# Targets to generate or remove the ssl certificate and key. Need to be copied
# to the source afterwards. Normally not needed.

if openssl.found()
  cert = custom_target('server.crt',
    output: ['server.crt', 'server.ckey'],
    command: [openssl, 'req', '-new', '-x509', '-days', '10000', '-nodes', '-out', '@OUTPUT0@',
      '-keyout', '@OUTPUT1@', '-subj', '/CN=localhost'],
    build_by_default: false,
    install: false,
  )

  # needs to agree with what's in the test script
  pass = 'FooBaR1'

  custom_target('server.key',
    input: [cert[1]],
    output: ['server.key'],
    command: [openssl, 'pkey', '-aes256', '-in', '@INPUT0@', '-out', '@OUTPUT0@', '-passout', 'pass:@0@'.format(pass)]
  )
endif

tests += {
  'name': 'ssl_passphrase_callback',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_testfunc.pl',
    ],
    'env': {'with_ssl': 'openssl'},
  },
}
