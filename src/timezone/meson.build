# Copyright (c) 2022-2025, PostgreSQL Global Development Group

# files to build into backend
timezone_sources = files(
  'localtime.c',
  'pgtz.c',
  'strftime.c',
)


timezone_inc = include_directories('.')

timezone_localtime_source = files('localtime.c')

# files needed to build zic utility program
zic_sources = files(
   'zic.c'
)

# we now distribute the timezone data as a single file
tzdata = files(
  'data/tzdata.zi'
)


if get_option('system_tzdata') == ''
  # FIXME: For cross builds, it would need a native built libpgport/pgcommon to
  # build our zic. But for that we'd need to run a good chunk of the configure
  # tests both natively and cross. Unclear if it's worth it.
  if meson.is_cross_build()
    zic = find_program(get_option('ZIC'), native: true, required: true)
  else
    if host_system == 'windows'
      zic_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
        '--NAME', 'zic',
        '--FILEDESC', 'zic - time zone compiler',])
    endif

    zic = executable('zic', zic_sources,
                     dependencies: [frontend_code],
                     kwargs: default_bin_args + {'install': false}
                    )
  endif

  tzdata = custom_target('tzdata',
    input: tzdata,
    output: ['timezone'],
    command: [zic, '-d', '@OUTPUT@', '@INPUT@'],
    install: true,
    install_dir: dir_data,
  )

  bin_targets += tzdata
endif

subdir('tznames')
