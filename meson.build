
# libjson-c-dev on ubuntu
jsondep = dependency('json-c')
curldep = dependency('libcurl')

pg_tde_sources = files(
        'src/pg_tde.c',
        'src/transam/pg_tde_xact_handler.c',
        'src/access/pg_tde_tdemap.c',
        'src/access/pg_tdeam.c',
        'src/access/pg_tdeam_handler.c',
        'src/access/pg_tdeam_visibility.c',
        'src/access/pg_tdetoast.c',
        'src/access/pg_tde_io.c',
        'src/access/pg_tde_prune.c',
        'src/access/pg_tde_rewrite.c',
        'src/access/pg_tde_vacuumlazy.c',
        'src/access/pg_tde_visibilitymap.c',
        'src/access/pg_tde_ddl.c',

        'src/encryption/enc_tde.c',
        'src/encryption/enc_aes.c',

        'src/keyring/keyring_config.c',
        'src/keyring/keyring_file.c',
        'src/keyring/keyring_vault.c',
        'src/keyring/keyring_api.c',

        'src/catalog/tde_keyring.c',
        'src/catalog/tde_master_key.c',
        'src/common/pg_tde_shmem.c',
        'src/common/pg_tde_utils.c',
        'src/pg_tde.c',
)

incdir = include_directories('src/include')

deps_update = {'dependencies': contrib_mod_args.get('dependencies') + [jsondep, curldep]}

mod_args = contrib_mod_args + deps_update

pg_tde = shared_module('pg_tde',
  pg_tde_sources,
  c_pch: pch_postgres_h,
  kwargs: mod_args,
  include_directories: incdir,
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
  'pg_tde--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_tde',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'toast_decrypt',
      'toast_extended_storage',
      'move_large_tuples',
      'non_sorted_off_compact',
      'update_compare_indexes',
      'pgtde_is_encrypted',
      'multi_insert',
      'keyprovider_dependency',
      'trigger_on_view',
      'insert_update_delete',
      'vault_v2_test',
    ],
    'regress_args': ['--temp-config', files('pg_tde.conf')],
    'runningcheck': false,
  },
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}
