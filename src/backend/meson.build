# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_build_deps = [backend_code]
backend_sources = []
backend_link_with = [pgport_srv, common_srv]

generated_backend_sources = []
post_export_backend_sources = []

subdir('access')
subdir('archive')
subdir('backup')
subdir('bootstrap')
subdir('catalog')
subdir('commands')
subdir('executor')
subdir('foreign')
subdir('jit')
subdir('lib')
subdir('libpq')
subdir('main')
subdir('nodes')
subdir('optimizer')
subdir('parser')
subdir('partitioning')
subdir('port')
subdir('postmaster')
subdir('regex')
subdir('replication')
subdir('rewrite')
subdir('statistics')
subdir('storage')
subdir('tcop')
subdir('tsearch')
subdir('utils')

subdir('po', if_found: libintl)


backend_link_args = []
backend_link_depends = []


# On windows when compiling with msvc we need to make postgres export all its
# symbols so that extension libraries can use them. For that we need to scan
# the constituting objects and generate a file specifying all the functions as
# exported (variables need an "import" declaration in the header, hence
# PGDLLEXPORT, but functions work without that, due to import libraries
# basically being trampolines).
#
# For dtrace probes we need to invoke dtrace on all input files, before
# linking the final executable (see more below).
#
#
# On meson there's currently no easy way to do this that I found. So we build
# a static library with all the input objects, run our script to generate
# exports, and build the final executable using that static library
#
# We could do that only if either dtrace or msvc is in use, but it seems
# easier to just always do so.
#
# Can't name the static library 'postgres', because msbuild ends up with a
# conflict for the .pdb file otherwise.

postgres_lib = static_library('postgres_lib',
  backend_sources + timezone_sources + generated_backend_sources,
  link_whole: backend_link_with,
  dependencies: backend_build_deps,
  c_pch: pch_postgres_h,
  kwargs: internal_lib_args,
)

if cc.get_id() == 'msvc'
  postgres_def = custom_target('postgres.def',
    command: [perl, files('../tools/msvc_gendef.pl'),
              '--arch', host_cpu,
              '--tempdir', '@PRIVATE_DIR@',
              '--deffile', '@OUTPUT@',
              '@INPUT@'],
    input: [postgres_lib, common_srv, pgport_srv],
    output: 'postgres.def',
    depends: [postgres_lib, common_srv, pgport_srv],
    install: false,
    build_by_default: false,
  )

  backend_link_args += '/DEF:@0@'.format(postgres_def.full_path())
  backend_link_depends += postgres_def
  # Due to the way msvc and meson's precompiled headers implementation
  # interact, we need to have symbols from the full library available. Could
  # be restricted to b_pch=true.
  backend_link_with += postgres_lib

endif

backend_input = []
backend_objs = [postgres_lib.extract_all_objects(recursive: false)]

# As of 1/2010:
# The probes.o file is necessary for dtrace support on Solaris, and on recent
# versions of systemtap.  (Older systemtap releases just produce an empty
# file, but that's okay.)  However, macOS's dtrace doesn't use it and doesn't
# even recognize the -G option.  So, build probes.o except on macOS.
# This might need adjustment as other platforms add dtrace support.
#
# On at least linux we don't actually need to pass in all the objects, but
# at least on FreeBSD and Solaris we have to.
#
# XXX: The reason we don't use the objects for generated sources is that doing
# so is not supported in older meson versions. Luckily we don't have probes in
# generated sources...
if dtrace.found() and host_system != 'darwin'
  backend_input += custom_target(
    'probes.o',
    input: ['utils/probes.d', postgres_lib.extract_objects(backend_sources, timezone_sources)],
    output: 'probes.o',
    command: [dtrace, '-C', '-G', '-o', '@OUTPUT@', '-s', '@INPUT@'],
    install: false,
  )
endif

if host_system == 'windows'
  post_export_backend_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'postgres',
    '--FILEDESC', 'PostgreSQL Server',])
endif

postgres = executable('postgres',
  backend_input,
  sources: post_export_backend_sources,
  objects: backend_objs,
  link_args: backend_link_args,
  link_with: backend_link_with,
  link_depends: backend_link_depends,
  export_dynamic: true,
  implib: 'postgres',
  dependencies: backend_build_deps,
  kwargs: default_bin_args,
)

backend_targets += postgres

pg_mod_c_args = cflags_mod
pg_mod_cpp_args = cxxflags_mod
pg_mod_link_args = ldflags_sl + ldflags_mod
pg_mod_link_depend = []

# A few platforms like MacOS and Windows link shared modules against postgres,
# or a [import] library derived from it. Set up the link flags for that.
if mod_link_args_fmt.length() > 0
  # To avoid unnecessary build-time dependencies on other operating systems,
  # only add the dependency when necessary.
  pg_mod_link_depend += postgres

  name = mod_link_with_name.format('postgres')
  link_with_uninst = meson.current_build_dir() / name
  link_with_inst = '${@0@}/@1@'.format(mod_link_with_dir, name)

  foreach el : mod_link_args_fmt
    pg_mod_link_args += el.format(link_with_uninst)
  endforeach
endif


# Note there's intentionally no dependency on pgport/common here - we want the
# symbols from the main binary for extension modules, rather than the
# extension linking separately to pgport/common.
backend_mod_code = declare_dependency(
  compile_args: pg_mod_c_args,
  include_directories: postgres_inc,
  link_args: pg_mod_link_args,
  sources: generated_headers + generated_backend_headers,
  dependencies: backend_mod_deps,
)

# normal extension modules
pg_mod_args = default_mod_args + {
  'dependencies': [backend_mod_code],
  'cpp_args': pg_mod_cpp_args,
  'link_depends': pg_mod_link_depend,
}

# extension modules that shouldn't be installed by default, as they're only
# for testing
pg_test_mod_args = pg_mod_args + {
  'install': false
}

###############################################################
# Define a .pc file that can be used to build server extensions
###############################################################

pg_ext_vars = []
pg_ext_vars_inst = []
pg_ext_vars_uninst = []

pg_ext_cflags = pg_mod_c_args
pg_ext_libs = [backend_mod_code, thread_dep, ldflags, ldflags_mod]
pg_ext_subdirs = ['']

# Compute directories to add include directories to the .pc files for.
# This is a bit more complicated due to port/win32 etc.
i = 0
foreach incdir : postgres_inc_d
  if incdir.startswith('src/include')
    subincdir = dir_include_pkg_rel / 'server' / incdir.split('src/include/').get(1, '')
  else
    subincdir = ''
  endif
  pg_ext_subdirs += subincdir

  # Add directories in source / build dir containing headers to cflags for the
  # -uninstalled.pc
  pg_ext_vars_uninst += [
    'build_inc@0@=-I${prefix}/@1@'.format(i, incdir),
    'src_inc@0@=-I${srcdir}/@1@'.format(i, incdir),
  ]
  pg_ext_cflags += [
    '${build_inc@0@}'.format(i),
    '${src_inc@0@}'.format(i)
  ]

  i += 1
endforeach


# Extension modules should likely also use -fwrapv etc. But it it's a bit odd
# to expose it to a .pc file?
pg_ext_cflags += cflags

# Directories for extensions to install into
# XXX: more needed
pg_ext_vars += 'pkglibdir=${prefix}/@0@'.format(dir_lib_pkg)
pg_ext_vars += 'dir_mod=${pkglibdir}'
pg_ext_vars += 'dir_data=${prefix}/@0@'.format(dir_data_extension)
# referenced on some platforms, via mod_link_with_dir
pg_ext_vars += 'bindir=${prefix}/@0@'.format(dir_bin)

# XXX: Define variables making it easy to define tests, too

# Some platforms need linker flags to link with binary, they are the same
# between building with meson and .pc file, except that we have have to
# reference a variable to make it work for both normal and -uninstalled .pc
# files.
if mod_link_args_fmt.length() != 0
  assert(link_with_inst != '')
  assert(link_with_uninst != '')

  pg_ext_vars_inst += 'mod_link_with=@0@'.format(link_with_inst)
  pg_ext_vars_uninst += 'mod_link_with=@0@'.format(link_with_uninst)

  foreach el : mod_link_args_fmt
    pg_ext_libs += el.format('${mod_link_with}')
  endforeach
endif

# main .pc to build extensions
pkgconfig.generate(
  name: 'postgresql-extension',
  description: 'PostgreSQL Extension Support',
  url: pg_url,

  subdirs: pg_ext_subdirs,
  libraries: pg_ext_libs,
  extra_cflags: pg_ext_cflags,

  variables: pg_ext_vars + pg_ext_vars_inst,
  uninstalled_variables: pg_ext_vars + pg_ext_vars_uninst,
)

# a .pc depending on the above, but with all our warnings enabled
pkgconfig.generate(
  name: 'postgresql-extension-warnings',
  description: 'PostgreSQL Extension Support - Compiler Warnings',
  requires: 'postgresql-extension',
  url: pg_url,
  extra_cflags: cflags_warn,

  variables: pg_ext_vars + pg_ext_vars_inst,
  uninstalled_variables: pg_ext_vars + pg_ext_vars_uninst,
)



# Shared modules that, on some system, link against the server binary. Only
# enter these after we defined the server build.

subdir('jit/llvm')
subdir('replication/libpqwalreceiver')
subdir('replication/pgoutput')
subdir('snowball')
subdir('utils/mb/conversion_procs')