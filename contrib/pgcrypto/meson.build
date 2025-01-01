# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not ssl.found()
  subdir_done()
endif

pgcrypto_sources = files(
  'crypt-blowfish.c',
  'crypt-des.c',
  'crypt-gensalt.c',
  'crypt-md5.c',
  'mbuf.c',
  'pgcrypto.c',
  'pgp-armor.c',
  'pgp-cfb.c',
  'pgp-compress.c',
  'pgp-decrypt.c',
  'pgp-encrypt.c',
  'pgp-info.c',
  'pgp-mpi.c',
  'pgp-pgsql.c',
  'pgp-pubdec.c',
  'pgp-pubenc.c',
  'pgp-pubkey.c',
  'pgp-s2k.c',
  'pgp.c',
  'px-crypt.c',
  'px-hmac.c',
  'px.c',
)

pgcrypto_regress = [
  'init',
  'md5',
  'sha1',
  'hmac-md5',
  'hmac-sha1',
  'blowfish',
  'rijndael',
  'sha2',
  'des',
  '3des',
  'cast5',
  'crypt-des',
  'crypt-md5',
  'crypt-blowfish',
  'crypt-xdes',
  'pgp-armor',
  'pgp-decrypt',
  'pgp-encrypt',
  'pgp-encrypt-md5',
  'pgp-pubkey-decrypt',
  'pgp-pubkey-encrypt',
  'pgp-info',
]

pgcrypto_openssl_sources = files(
  'openssl.c',
  'pgp-mpi-openssl.c',
)

pgcrypto_deps = []
pgcrypto_link_with = []

pgcrypto_deps += ssl
pgcrypto_sources += pgcrypto_openssl_sources

if zlib.found()
  pgcrypto_deps += zlib
  pgcrypto_regress += 'pgp-compression'
else
  pgcrypto_regress += 'pgp-zlib-DISABLED'
endif

if host_system == 'windows'
  pgcrypto_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgcrypto',
    '--FILEDESC', 'pgcrypto - cryptographic functions',])
endif

pgcrypto = shared_module('pgcrypto',
  pgcrypto_sources,
  link_with: pgcrypto_link_with,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args + {
    'dependencies': [pgcrypto_deps, contrib_mod_args['dependencies']]
  },
)
contrib_targets += pgcrypto

install_data(
  'pgcrypto--1.0--1.1.sql',
  'pgcrypto--1.1--1.2.sql',
  'pgcrypto--1.2--1.3.sql',
  'pgcrypto--1.3.sql',
  'pgcrypto.control',
  kwargs: contrib_data_args,
)


tests += {
  'name': 'pgcrypto',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      pgcrypto_regress,
    ],
  },
}
