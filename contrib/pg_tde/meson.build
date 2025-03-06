
curldep = dependency('libcurl')

pg_tde_sources = files(
        'src/pg_tde.c',
        'src/transam/pg_tde_xact_handler.c',
        'src/access/pg_tde_tdemap.c',
        'src/access/pg_tde_xlog.c',
        'src/access/pg_tde_xlog_encrypt.c',

        'src/encryption/enc_tde.c',
        'src/encryption/enc_aes.c',

        'src/keyring/keyring_curl.c',
        'src/keyring/keyring_file.c',
        'src/keyring/keyring_vault.c',
        'src/keyring/keyring_kmip.c',
        'src/keyring/keyring_kmip_ereport.c',
        'src/keyring/keyring_api.c',

        'src/smgr/pg_tde_smgr.c',

        'src/catalog/tde_keyring.c',
        'src/catalog/tde_keyring_parse_opts.c',
        'src/catalog/tde_principal_key.c',
        'src/common/pg_tde_shmem.c',
        'src/common/pg_tde_utils.c',
        'src/pg_tde_defs.c',
        'src/pg_tde_event_capture.c',
        'src/pg_tde_guc.c',
        'src/pg_tde.c',
)

incdir = include_directories('src/include', '.', 'src/libkmip/libkmip/include/')

kmip = static_library(
  'kmip',
  files( 
        'src/libkmip/libkmip/src/kmip.c',
        'src/libkmip/libkmip/src/kmip_bio.c',
        'src/libkmip/libkmip/src/kmip_locate.c',
        'src/libkmip/libkmip/src/kmip_memset.c'
        ),
  c_args: [ '-w' ], # This is a 3rd party, disable warnings completely
  include_directories: incdir
)

deps_update = {'dependencies': contrib_mod_args.get('dependencies') + [curldep]}

mod_args = contrib_mod_args + deps_update

pg_tde = shared_module('pg_tde',
  pg_tde_sources,
  c_pch: pch_postgres_h,
  kwargs: mod_args,
  include_directories: incdir,
  link_whole: [kmip]
)
contrib_targets += pg_tde

ldflags = []
if host_system == 'darwin'
  # On MacOS Shared Libraries and Loadable Modules are different things,
  # so we need to pass an extra flag to the linker.
  ldflags += '-bundle'
endif

install_data(
  'pg_tde.control',
  'pg_tde--1.0-rc.sql',
  kwargs: contrib_data_args,
)


sql_tests = [
      'cache_alloc',
      'key_provider',
      'relocate',
      'toast_decrypt',
      'toast_extended_storage',
      'move_large_tuples',
      'non_sorted_off_compact',
      'update_compare_indexes',
      'update',
      'pg_tde_is_encrypted',
      'test_issue_153_fix',
      'multi_insert',
      'keyprovider_dependency',
      'trigger_on_view',
      'change_access_method',
      'insert_update_delete',
      'tablespace',
      'vault_v2_test',
      'kmip_test',
      'alter_index',
      'merge_join',
      'default_principal_key',
      'access_control',
]

tap_tests = [
      't/001_basic.pl',
      't/002_rotate_key.pl',
      't/003_remote_config.pl',
      't/004_file_config.pl',
      't/005_multiple_extensions.pl',
      't/006_remote_vault_config.pl',
      't/007_tde_heap.pl',
      't/008_key_rotate_tablespace.pl',
      't/009_wal_encrypt.pl',
]

tests += {
  'name': 'pg_tde',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': sql_tests,
    'regress_args': ['--temp-config', files('pg_tde.conf')],
    'runningcheck': false,
  },
  'tap': {
    'tests': tap_tests  },
}

# TODO: do not duplicate
tde_decrypt_sources = files(
   'src/access/pg_tde_tdemap.c',
   'src/access/pg_tde_xlog_encrypt.c',
   'src/catalog/tde_keyring.c',
   'src/catalog/tde_keyring_parse_opts.c',
   'src/catalog/tde_principal_key.c',
   'src/common/pg_tde_utils.c',
   'src/encryption/enc_aes.c',
   'src/encryption/enc_tde.c',
   'src/keyring/keyring_api.c',
   'src/keyring/keyring_curl.c',
   'src/keyring/keyring_file.c',
   'src/keyring/keyring_vault.c',
   'src/keyring/keyring_kmip.c',
   'src/keyring/keyring_kmip_ereport.c',
 )

pg_tde_inc = incdir

pg_tde_frontend = static_library('pg_tde_frontend',
  tde_decrypt_sources,
  c_pch: pch_postgres_h,
  c_args: ['-DFRONTEND'],
  kwargs: mod_args,
  include_directories: incdir,
  link_whole: [kmip]
)

pg_tde_alter_key_provider_sources = files(
  'src/pg_tde_alter_key_provider.c',
)

pg_tde_alter_key_provider = executable('pg_tde_alter_key_provider',
  pg_tde_alter_key_provider_sources,
  dependencies: [frontend_code, lz4, zstd],
  c_args: ['-DFRONTEND'], # needed for xlogreader et al
  kwargs: default_bin_args,
  include_directories: [postgres_inc, pg_tde_inc],
  link_with: [pg_tde_frontend]
)
contrib_targets += pg_tde_alter_key_provider
