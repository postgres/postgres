if not ldap.found()
  subdir_done()
endif

ldap_password_func_sources = files(
  'ldap_password_func.c',
)

if host_system == 'windows'
  ldap_password_func_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ldap_password_func',
    '--FILEDESC', 'set hook to mutate ldapbindpasswd',])
endif

ldap_password_func = shared_module('ldap_password_func',
  ldap_password_func_sources,
  kwargs: pg_test_mod_args + {
    'dependencies': [ldap, pg_mod_args['dependencies']],
  },
)
test_install_libs += ldap_password_func

tests += {
  'name': 'ldap_password_func',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_mutated_bindpasswd.pl',
    ],
  'env': {'with_ldap': 'yes'}
  },
}
