# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'auth-sasl.c',
  'auth-scram.c',
  'auth.c',
  'be-fsstubs.c',
  'be-secure-common.c',
  'be-secure.c',
  'crypt.c',
  'hba.c',
  'ifaddr.c',
  'pqcomm.c',
  'pqformat.c',
  'pqmq.c',
  'pqsignal.c',
)

if ssl.found()
  backend_sources += files('be-secure-openssl.c')
endif

if gssapi.found()
  backend_sources += files(
    'be-secure-gssapi.c',
    'be-gssapi-common.c'
  )
endif

install_data(
  'pg_hba.conf.sample',
  'pg_ident.conf.sample',
  install_dir: dir_data,
)
