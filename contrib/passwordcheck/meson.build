# Copyright (c) 2022-2025, PostgreSQL Global Development Group

passwordcheck_sources = files(
  'passwordcheck.c',
)

passwordcheck_c_args = []
passwordcheck_deps = []

# uncomment the following two lines to enable cracklib support
# passwordcheck_c_args += ['-DUSE_CRACKLIB', '-DCRACKLIB_DICTPATH="/usr/lib/cracklib_dict"']
# passwordcheck_deps += [cc.find_library('crack')]

if host_system == 'windows'
  passwordcheck_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'passwordcheck',
    '--FILEDESC', 'passwordcheck - strengthen user password checks',])
endif

passwordcheck = shared_module('passwordcheck',
  passwordcheck_sources,
  c_args: passwordcheck_c_args,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args.get('dependencies') + passwordcheck_deps,
  }
)
contrib_targets += passwordcheck

tests += {
  'name': 'passwordcheck',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'passwordcheck',
    ],
  },
}
