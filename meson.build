
curldep = dependency('libcurl')

pg_tde_version = '1.0.0-beta2'

conf_data = configuration_data()
conf_data.set_quoted('PACKAGE_NAME', 'pg_tde')
conf_data.set_quoted('PACKAGE_BUGREPORT', 'https://github.com/percona/pg_tde/issues')
conf_data.set_quoted('PACKAGE_VERSION', pg_tde_version)
conf_data.set_quoted('PACKAGE_STRING', 'pg_tde ' + pg_tde_version)
conf_data.set_quoted('PACKAGE_TARNAME', 'pg_tde')

configure_file(output : 'config.h',
               configuration : conf_data)

pg_version = meson.project_version().substring(0,2)
src_version = 'src' + pg_version

pg_tde_sources = files(
        'src/pg_tde.c',
        'src/transam/pg_tde_xact_handler.c',
        'src/access/pg_tde_tdemap.c',
        'src/access/pg_tde_slot.c',
        src_version / 'access/pg_tdeam.c',
        src_version / 'access/pg_tdeam_handler.c',
        src_version / 'access/pg_tdeam_visibility.c',
        src_version / 'access/pg_tdetoast.c',
        src_version / 'access/pg_tde_io.c',
        src_version / 'access/pg_tde_prune.c',
        src_version / 'access/pg_tde_rewrite.c',
        src_version / 'access/pg_tde_vacuumlazy.c',
        src_version / 'access/pg_tde_visibilitymap.c',
        'src/access/pg_tde_ddl.c',
        'src/access/pg_tde_xlog.c',
        'src/access/pg_tde_xlog_encrypt.c',

        'src/encryption/enc_tde.c',
        'src/encryption/enc_aes.c',

        'src/keyring/keyring_curl.c',
        'src/keyring/keyring_file.c',
        'src/keyring/keyring_vault.c',
        'src/keyring/keyring_api.c',

        'src/smgr/pg_tde_smgr.c',

        'src/catalog/tde_global_space.c',
        'src/catalog/tde_keyring.c',
        'src/catalog/tde_keyring_parse_opts.c',
        'src/catalog/tde_principal_key.c',
        'src/common/pg_tde_shmem.c',
        'src/common/pg_tde_utils.c',
        'src/pg_tde_defs.c',
        'src/pg_tde.c',
        'src/pg_tde_event_capture.c',
)

incdir = include_directories(src_version / 'include', 'src/include', '.')

deps_update = {'dependencies': contrib_mod_args.get('dependencies') + [curldep]}

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


sql_tests = [
      'toast_decrypt_basic',
      'toast_extended_storage_basic',
      'move_large_tuples_basic',
      'non_sorted_off_compact_basic',
      'update_compare_indexes_basic',
      'pg_tde_is_encrypted_basic',
      'test_issue_153_fix_basic',
      'multi_insert_basic',
      'keyprovider_dependency_basic',
      'trigger_on_view_basic',
      'change_access_method_basic',
      'insert_update_delete_basic',
      'vault_v2_test_basic',
      'alter_index_basic',
]

tap_tests = [
      't/001_basic.pl',
      't/002_rotate_key.pl',
      't/003_remote_config.pl',
      't/004_file_config.pl',
      't/005_multiple_extensions.pl',
      't/006_remote_vault_config.pl',
      't/007_access_control.pl',
    ]

if get_variable('percona_ext', false)
  sql_tests += [
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
      'vault_v2_test',
      'alter_index',
  ]

  tap_tests += [
      't/008_tde_heap.pl',
  ]
endif



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
