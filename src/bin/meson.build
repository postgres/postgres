# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tde_decrypt_sources = []
tde_include = []
tde_deps = []

if percona_ext == true
# TODO: should be in pg_tde, ideally as a static lib
tde_decrypt_sources = files(
   '../../contrib/pg_tde/src/access/pg_tde_tdemap.c',
   '../../contrib/pg_tde/src/access/pg_tde_xlog_encrypt.c',
   '../../contrib/pg_tde/src/catalog/tde_global_space.c',
   '../../contrib/pg_tde/src/catalog/tde_keyring.c',
   '../../contrib/pg_tde/src/catalog/tde_keyring_parse_opts.c',
   '../../contrib/pg_tde/src/catalog/tde_principal_key.c',
   '../../contrib/pg_tde/src/common/pg_tde_utils.c',
   '../../contrib/pg_tde/src/encryption/enc_aes.c',
   '../../contrib/pg_tde/src/encryption/enc_tde.c',
   '../../contrib/pg_tde/src/keyring/keyring_api.c',
   '../../contrib/pg_tde/src/keyring/keyring_curl.c',
   '../../contrib/pg_tde/src/keyring/keyring_file.c',
   '../../contrib/pg_tde/src/keyring/keyring_vault.c',
   '../../contrib/pg_tde/src/keyring/keyring_kmip.c',
   '../../contrib/pg_tde/src/keyring/keyring_kmip_ereport.c',

   '../../contrib/pg_tde/src/libkmip/libkmip/src/kmip.c',
   '../../contrib/pg_tde/src/libkmip/libkmip/src/kmip_bio.c',
   '../../contrib/pg_tde/src/libkmip/libkmip/src/kmip_locate.c',
   '../../contrib/pg_tde/src/libkmip/libkmip/src/kmip_memset.c',
 )

tde_include = include_directories(
	'../../contrib/pg_tde/src/include',
	'../../contrib/pg_tde/src/libkmip/libkmip/include'
	)

curldep = dependency('libcurl')
tde_deps = [curldep]

endif

subdir('initdb')
subdir('pg_amcheck')
subdir('pg_archivecleanup')
subdir('pg_basebackup')
subdir('pg_checksums')
subdir('pg_combinebackup')
subdir('pg_config')
subdir('pg_controldata')
subdir('pg_ctl')
subdir('pg_dump')
subdir('pg_resetwal')
subdir('pg_rewind')
subdir('pg_test_fsync')
subdir('pg_test_timing')
subdir('pg_upgrade')
subdir('pg_verifybackup')
subdir('pg_waldump')
subdir('pg_walsummary')
subdir('pgbench')
subdir('pgevent')
subdir('psql')
subdir('scripts')
