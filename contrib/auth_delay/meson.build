auth_delay_sources = files(
  'auth_delay.c',
)

if host_system == 'windows'
  auth_delay_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'auth_delay',
    '--FILEDESC', 'auth_delay - delay authentication failure reports',])
endif

autoinc = shared_module('auth_delay',
  auth_delay_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += autoinc
