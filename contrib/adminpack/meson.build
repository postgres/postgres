adminpack_sources = files(
  'adminpack.c',
)

if host_system == 'windows'
  adminpack_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'adminpack',
    '--FILEDESC', 'adminpack - support functions for pgAdmin',])
endif

adminpack = shared_module('adminpack',
  adminpack_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += adminpack

install_data(
  'adminpack.control',
  'adminpack--1.0.sql',
  'adminpack--1.0--1.1.sql',
  'adminpack--1.1--2.0.sql',
  'adminpack--2.0--2.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'adminpack',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': ['adminpack'],
  },
}
