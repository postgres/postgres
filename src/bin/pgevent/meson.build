# Copyright (c) 2022-2023, PostgreSQL Global Development Group

if host_system != 'windows'
  subdir_done()
endif

pgevent_sources = files(
  'pgevent.c',
)

pgevent_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
  '--NAME', 'pgevent',
  '--FILEDESC', 'Eventlog message formatter',])

pgevent_sources += windows.compile_resources('pgmsgevent.rc')

# FIXME: copied from Mkvcbuild.pm, but I don't think that's the right approach
pgevent_link_args = []
if cc.get_id() == 'msvc'
  pgevent_link_args += '/ignore:4104'
endif

pgevent = shared_library('pgevent',
  pgevent_sources,
  dependencies: [frontend_code],
  link_args: pgevent_link_args,
  vs_module_defs: 'pgevent.def',
  kwargs: default_lib_args + {
    'name_prefix': '',
  },
)
bin_targets += pgevent
