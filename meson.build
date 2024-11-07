# Copyright (c) 2022-2024, PostgreSQL Global Development Group

# Entry point for building PostgreSQL with meson
#
# Good starting points for writing meson.build files are:
#  - https://mesonbuild.com/Syntax.html
#  - https://mesonbuild.com/Reference-manual.html

project('postgresql',
  ['c'],
  version: '18devel',
  license: 'PostgreSQL',

  # We want < 0.56 for python 3.5 compatibility on old platforms. EPEL for
  # RHEL 7 has 0.55. < 0.54 would require replacing some uses of the fs
  # module, < 0.53 all uses of fs. So far there's no need to go to >=0.56.
  meson_version: '>=0.54',
  default_options: [
    'warning_level=1', #-Wall equivalent
    'b_pch=false',
    'buildtype=debugoptimized', # -O2 + debug
    # For compatibility with the autoconf build, set a default prefix. This
    # works even on windows, where it's a drive-relative path (i.e. when on
    # d:/somepath it'll install to d:/usr/local/pgsql)
    'prefix=/usr/local/pgsql',
  ]
)



###############################################################
# Basic prep
###############################################################

fs = import('fs')
pkgconfig = import('pkgconfig')

host_system = host_machine.system()
build_system = build_machine.system()
host_cpu = host_machine.cpu_family()

cc = meson.get_compiler('c')

not_found_dep = dependency('', required: false)
thread_dep = dependency('threads')
auto_features = get_option('auto_features')



###############################################################
# Safety first
###############################################################

# It's very easy to get into confusing states when the source directory
# contains an in-place build. E.g. the wrong pg_config.h will be used. So just
# refuse to build in that case.
#
# There's a more elaborate check later, that checks for conflicts around all
# generated files. But we can only do that much further down the line, so this
# quick check seems worth it. Adhering to this advice should clean up the
# conflict, but won't protect against somebody doing make distclean or just
# removing pg_config.h
errmsg_nonclean_base = '''
****
Non-clean source code directory detected.

To build with meson the source tree may not have an in-place, ./configure
style, build configured. You can have both meson and ./configure style builds
for the same source tree by building out-of-source / VPATH with
configure. Alternatively use a separate check out for meson based builds.

@0@
****'''
if fs.exists(meson.current_source_dir() / 'src' / 'include' / 'pg_config.h')
  errmsg_cleanup = 'To clean up, run make distclean in the source tree.'
  error(errmsg_nonclean_base.format(errmsg_cleanup))
endif



###############################################################
# Variables to be determined
###############################################################

postgres_inc_d = ['src/include']
postgres_inc_d += get_option('extra_include_dirs')

postgres_lib_d = get_option('extra_lib_dirs')

cppflags = []

cflags = []
cxxflags = []
cflags_warn = []
cxxflags_warn = []
cflags_mod = []
cxxflags_mod = []

ldflags = []
ldflags_be = []
ldflags_sl = []
ldflags_mod = []

test_c_args = []

os_deps = []
backend_both_deps = []
backend_deps = []
libpq_deps = []

pg_sysroot = ''

# source of data for pg_config.h etc
cdata = configuration_data()



###############################################################
# Version and other metadata
###############################################################

pg_version = meson.project_version()

if pg_version.endswith('devel')
  pg_version_arr = [pg_version.split('devel')[0], '0']
elif pg_version.contains('beta')
  pg_version_arr = [pg_version.split('beta')[0], '0']
elif pg_version.contains('rc')
  pg_version_arr = [pg_version.split('rc')[0], '0']
else
  pg_version_arr = pg_version.split('.')
endif

pg_version_major = pg_version_arr[0].to_int()
pg_version_minor = pg_version_arr[1].to_int()
pg_version_num = (pg_version_major * 10000) + pg_version_minor

pg_url = 'https://www.postgresql.org/'

cdata.set_quoted('PACKAGE_NAME', 'PostgreSQL')
cdata.set_quoted('PACKAGE_BUGREPORT', 'pgsql-bugs@lists.postgresql.org')
cdata.set_quoted('PACKAGE_URL', pg_url)
cdata.set_quoted('PACKAGE_VERSION', pg_version)
cdata.set_quoted('PACKAGE_STRING', 'PostgreSQL @0@'.format(pg_version))
cdata.set_quoted('PACKAGE_TARNAME', 'postgresql')

pg_version += get_option('extra_version')
cdata.set_quoted('PG_VERSION', pg_version)
cdata.set_quoted('PG_MAJORVERSION', pg_version_major.to_string())
cdata.set('PG_MAJORVERSION_NUM', pg_version_major)
cdata.set('PG_MINORVERSION_NUM', pg_version_minor)
cdata.set('PG_VERSION_NUM', pg_version_num)
# PG_VERSION_STR is built later, it depends on compiler test results
cdata.set_quoted('CONFIGURE_ARGS', '')



###############################################################
# Basic platform specific configuration
###############################################################

exesuffix = '' # overridden below where necessary
dlsuffix = '.so' # overridden below where necessary
library_path_var = 'LD_LIBRARY_PATH'

# Format of file to control exports from libraries, and how to pass them to
# the compiler. For export_fmt @0@ is the path to the file export file.
export_file_format = 'gnu'
export_file_suffix = 'list'
export_fmt = '-Wl,--version-script=@0@'

# Flags to add when linking a postgres extension, @0@ is path to
# the relevant object on the platform.
mod_link_args_fmt = []

memset_loop_limit = 1024

# Choice of shared memory and semaphore implementation
shmem_kind = 'sysv'
sema_kind = 'sysv'

# We implement support for some operating systems by pretending they're
# another. Map here, before determining system properties below
if host_system == 'dragonfly'
  # apparently the most similar
  host_system = 'netbsd'
elif host_system == 'android'
  # while android isn't quite a normal linux, it seems close enough
  # for our purposes so far
  host_system = 'linux'
endif

# meson's system names don't quite map to our "traditional" names. In some
# places we need the "traditional" name, e.g., for mapping
# src/include/port/$os.h to src/include/pg_config_os.h. Define portname for
# that purpose.
portname = host_system

if host_system == 'cygwin'
  sema_kind = 'unnamed_posix'
  cppflags += '-D_GNU_SOURCE'
  dlsuffix = '.dll'
  mod_link_args_fmt = ['@0@']
  mod_link_with_name = 'lib@0@.a'
  mod_link_with_dir = 'libdir'

elif host_system == 'darwin'
  dlsuffix = '.dylib'
  library_path_var = 'DYLD_LIBRARY_PATH'

  export_file_format = 'darwin'
  export_fmt = '-Wl,-exported_symbols_list,@0@'

  mod_link_args_fmt = ['-bundle_loader', '@0@']
  mod_link_with_dir = 'bindir'
  mod_link_with_name = '@0@'

  sysroot_args = [files('src/tools/darwin_sysroot'), get_option('darwin_sysroot')]
  pg_sysroot = run_command(sysroot_args, check:true).stdout().strip()
  message('darwin sysroot: @0@'.format(pg_sysroot))
  if pg_sysroot != ''
    cflags += ['-isysroot', pg_sysroot]
    ldflags += ['-isysroot', pg_sysroot]
  endif

  # meson defaults to -Wl,-undefined,dynamic_lookup for modules, which we
  # don't want because a) it's different from what we do for autoconf, b) it
  # causes warnings in macOS Ventura. But using -Wl,-undefined,error causes a
  # warning starting in Sonoma. So only add -Wl,-undefined,error if it does
  # not cause a warning.
  if cc.has_multi_link_arguments('-Wl,-undefined,error', '-Werror')
    ldflags_mod += '-Wl,-undefined,error'
  endif

  # Starting in Sonoma, the linker warns about the same library being
  # linked twice.  Which can easily happen when multiple dependencies
  # depend on the same library. Quiesce the ill considered warning.
  ldflags += cc.get_supported_link_arguments('-Wl,-no_warn_duplicate_libraries')

elif host_system == 'freebsd'
  sema_kind = 'unnamed_posix'

elif host_system == 'linux'
  sema_kind = 'unnamed_posix'
  cppflags += '-D_GNU_SOURCE'

elif host_system == 'netbsd'
  # We must resolve all dynamic linking in the core server at program start.
  # Otherwise the postmaster can self-deadlock due to signals interrupting
  # resolution of calls, since NetBSD's linker takes a lock while doing that
  # and some postmaster signal handlers do things that will also acquire that
  # lock.  As long as we need "-z now", might as well specify "-z relro" too.
  # While there's not a hard reason to adopt these settings for our other
  # executables, there's also little reason not to, so just add them to
  # LDFLAGS.
  ldflags += ['-Wl,-z,now', '-Wl,-z,relro']

elif host_system == 'openbsd'
  # you're ok

elif host_system == 'sunos'
  portname = 'solaris'
  export_fmt = '-Wl,-M@0@'
  cppflags += '-D_POSIX_PTHREAD_SEMANTICS'

elif host_system == 'windows'
  portname = 'win32'
  exesuffix = '.exe'
  dlsuffix = '.dll'
  library_path_var = ''
  if cc.get_id() != 'msvc'
    # define before including <time.h> for getting localtime_r() etc. on MinGW
    cppflags += '-D_POSIX_C_SOURCE'
  endif

  export_file_format = 'win'
  export_file_suffix = 'def'
  if cc.get_id() == 'msvc'
    export_fmt = '/DEF:@0@'
    mod_link_with_name = '@0@.lib'
  else
    export_fmt = '@0@'
    mod_link_with_name = 'lib@0@.a'
  endif
  mod_link_args_fmt = ['@0@']
  mod_link_with_dir = 'libdir'

  shmem_kind = 'win32'
  sema_kind = 'win32'

  cdata.set('WIN32_STACK_RLIMIT', 4194304)
  if cc.get_id() == 'msvc'
    ldflags += '/INCREMENTAL:NO'
    ldflags += '/STACK:@0@'.format(cdata.get('WIN32_STACK_RLIMIT'))
    # ldflags += '/nxcompat' # generated by msbuild, should have it for ninja?
  else
    ldflags += '-Wl,--stack,@0@'.format(cdata.get('WIN32_STACK_RLIMIT'))
    # Need to allow multiple definitions, we e.g. want to override getopt.
    ldflags += '-Wl,--allow-multiple-definition'
    # Ensure we get MSVC-like linking behavior.
    ldflags += '-Wl,--disable-auto-import'
  endif

  os_deps += cc.find_library('ws2_32', required: true)
  secur32_dep = cc.find_library('secur32', required: true)
  backend_deps += secur32_dep
  libpq_deps += secur32_dep

  postgres_inc_d += 'src/include/port/win32'
  if cc.get_id() == 'msvc'
    postgres_inc_d += 'src/include/port/win32_msvc'
  endif

  windows = import('windows')

else
  # XXX: Should we add an option to override the host_system as an escape
  # hatch?
  error('unknown host system: @0@'.format(host_system))
endif



###############################################################
# Program paths
###############################################################

# External programs
perl = find_program(get_option('PERL'), required: true, native: true)
python = find_program(get_option('PYTHON'), required: true, native: true)
flex = find_program(get_option('FLEX'), native: true, version: '>= 2.5.35')
bison = find_program(get_option('BISON'), native: true, version: '>= 2.3')
sed = find_program(get_option('SED'), 'sed', native: true, required: false)
prove = find_program(get_option('PROVE'), native: true, required: false)
tar = find_program(get_option('TAR'), native: true, required: false)
gzip = find_program(get_option('GZIP'), native: true, required: false)
program_lz4 = find_program(get_option('LZ4'), native: true, required: false)
openssl = find_program(get_option('OPENSSL'), native: true, required: false)
program_zstd = find_program(get_option('ZSTD'), native: true, required: false)
dtrace = find_program(get_option('DTRACE'), native: true, required: get_option('dtrace'))
missing = find_program('config/missing', native: true)
cp = find_program('cp', required: false, native: true)
xmllint_bin = find_program(get_option('XMLLINT'), native: true, required: false)
xsltproc_bin = find_program(get_option('XSLTPROC'), native: true, required: false)

bison_flags = []
if bison.found()
  bison_version_c = run_command(bison, '--version', check: true)
  # bison version string helpfully is something like
  # >>bison (GNU bison) 3.8.1<<
  bison_version = bison_version_c.stdout().split(' ')[3].split('\n')[0]
  if bison_version.version_compare('>=3.0')
    bison_flags += ['-Wno-deprecated']
  endif
endif
bison_cmd = [bison, bison_flags, '-o', '@OUTPUT0@', '-d', '@INPUT@']
bison_kw = {
  'output': ['@BASENAME@.c', '@BASENAME@.h'],
  'command': bison_cmd,
}

flex_flags = []
if flex.found()
  flex_version_c = run_command(flex, '--version', check: true)
  flex_version = flex_version_c.stdout().split(' ')[1].split('\n')[0]
endif
flex_wrapper = files('src/tools/pgflex')
flex_cmd = [python, flex_wrapper,
  '--builddir', '@BUILD_ROOT@',
  '--srcdir', '@SOURCE_ROOT@',
  '--privatedir', '@PRIVATE_DIR@',
  '--flex', flex, '--perl', perl,
  '-i', '@INPUT@', '-o', '@OUTPUT0@',
]

wget = find_program('wget', required: false, native: true)
wget_flags = ['-O', '@OUTPUT0@', '--no-use-server-timestamps']

install_files = files('src/tools/install_files')



###############################################################
# Path to meson (for tests etc)
###############################################################

# NB: this should really be part of meson, see
# https://github.com/mesonbuild/meson/issues/8511
meson_binpath_r = run_command(python, 'src/tools/find_meson', check: true)

if meson_binpath_r.stdout() == ''
  error('huh, could not run find_meson.\nerrcode: @0@\nstdout: @1@\nstderr: @2@'.format(
    meson_binpath_r.returncode(),
    meson_binpath_r.stdout(),
    meson_binpath_r.stderr()))
endif

meson_binpath_s = meson_binpath_r.stdout().split('\n')
meson_binpath_len = meson_binpath_s.length()

if meson_binpath_len < 1
  error('unexpected introspect line @0@'.format(meson_binpath_r.stdout()))
endif

i = 0
meson_impl = ''
meson_binpath = ''
meson_args = []
foreach e : meson_binpath_s
  if i == 0
    meson_impl = e
  elif i == 1
    meson_binpath = e
  else
    meson_args += e
  endif
  i += 1
endforeach

if meson_impl not in ['muon', 'meson']
  error('unknown meson implementation "@0@"'.format(meson_impl))
endif

meson_bin = find_program(meson_binpath, native: true)



###############################################################
# Option Handling
###############################################################

cdata.set('USE_ASSERT_CHECKING', get_option('cassert') ? 1 : false)
cdata.set('USE_INJECTION_POINTS', get_option('injection_points') ? 1 : false)

blocksize = get_option('blocksize').to_int() * 1024

if get_option('segsize_blocks') != 0
  if get_option('segsize') != 1
    warning('both segsize and segsize_blocks specified, segsize_blocks wins')
  endif

  segsize = get_option('segsize_blocks')
else
  segsize = (get_option('segsize') * 1024 * 1024 * 1024) / blocksize
endif

cdata.set('BLCKSZ', blocksize, description:
'''Size of a disk block --- this also limits the size of a tuple. You can set
   it bigger if you need bigger tuples (although TOAST should reduce the need
   to have large tuples, since fields can be spread across multiple tuples).
   BLCKSZ must be a power of 2. The maximum possible value of BLCKSZ is
   currently 2^15 (32768). This is determined by the 15-bit widths of the
   lp_off and lp_len fields in ItemIdData (see include/storage/itemid.h).
   Changing BLCKSZ requires an initdb.''')

cdata.set('XLOG_BLCKSZ', get_option('wal_blocksize').to_int() * 1024)
cdata.set('RELSEG_SIZE', segsize)
cdata.set('DEF_PGPORT', get_option('pgport'))
cdata.set_quoted('DEF_PGPORT_STR', get_option('pgport').to_string())
cdata.set_quoted('PG_KRB_SRVNAM', get_option('krb_srvnam'))
if get_option('system_tzdata') != ''
  cdata.set_quoted('SYSTEMTZDIR', get_option('system_tzdata'))
endif



###############################################################
# Directories
###############################################################

# These are set by the equivalent --xxxdir configure options.  We
# append "postgresql" to some of them, if the string does not already
# contain "pgsql" or "postgres", in order to avoid directory clutter.

pkg = 'postgresql'

dir_prefix = get_option('prefix')

dir_prefix_contains_pg = (dir_prefix.contains('pgsql') or dir_prefix.contains('postgres'))

dir_bin = get_option('bindir')

dir_data = get_option('datadir')
if not (dir_prefix_contains_pg or dir_data.contains('pgsql') or dir_data.contains('postgres'))
  dir_data = dir_data / pkg
endif

dir_sysconf = get_option('sysconfdir')
if not (dir_prefix_contains_pg or dir_sysconf.contains('pgsql') or dir_sysconf.contains('postgres'))
  dir_sysconf = dir_sysconf / pkg
endif

dir_lib = get_option('libdir')

dir_lib_pkg = dir_lib
if not (dir_prefix_contains_pg or dir_lib_pkg.contains('pgsql') or dir_lib_pkg.contains('postgres'))
  dir_lib_pkg = dir_lib_pkg / pkg
endif

dir_pgxs = dir_lib_pkg / 'pgxs'

dir_include = get_option('includedir')

dir_include_pkg = dir_include
dir_include_pkg_rel = ''
if not (dir_prefix_contains_pg or dir_include_pkg.contains('pgsql') or dir_include_pkg.contains('postgres'))
  dir_include_pkg = dir_include_pkg / pkg
  dir_include_pkg_rel = pkg
endif

dir_man = get_option('mandir')

# FIXME: These used to be separately configurable - worth adding?
dir_doc = get_option('datadir') / 'doc'
if not (dir_prefix_contains_pg or dir_doc.contains('pgsql') or dir_doc.contains('postgres'))
  dir_doc = dir_doc / pkg
endif
dir_doc_html = dir_doc / 'html'

dir_locale = get_option('localedir')


# Derived values
dir_bitcode = dir_lib_pkg / 'bitcode'
dir_include_internal = dir_include_pkg / 'internal'
dir_include_server = dir_include_pkg / 'server'
dir_include_extension = dir_include_server / 'extension'
dir_data_extension = dir_data / 'extension'
dir_doc_extension = dir_doc / 'extension'



###############################################################
# Search paths, preparation for compiler tests
#
# NB: Arguments added later are not automatically used for subsequent
# configuration-time checks (so they are more isolated). If they should be
# used, they need to be added to test_c_args as well.
###############################################################

postgres_inc = [include_directories(postgres_inc_d)]
test_lib_d = postgres_lib_d
test_c_args = cppflags + cflags



###############################################################
# Library: bsd-auth
###############################################################

bsd_authopt = get_option('bsd_auth')
bsd_auth = not_found_dep
if cc.check_header('bsd_auth.h', required: bsd_authopt,
    args: test_c_args, include_directories: postgres_inc)
  cdata.set('USE_BSD_AUTH', 1)
  bsd_auth = declare_dependency()
endif



###############################################################
# Library: bonjour
#
# For now don't search for DNSServiceRegister in a library - only Apple's
# Bonjour implementation, which is always linked, works.
###############################################################

bonjouropt = get_option('bonjour')
bonjour = not_found_dep
if cc.check_header('dns_sd.h', required: bonjouropt,
    args: test_c_args, include_directories: postgres_inc) and \
   cc.has_function('DNSServiceRegister',
    args: test_c_args, include_directories: postgres_inc)
  cdata.set('USE_BONJOUR', 1)
  bonjour = declare_dependency()
endif



###############################################################
# Option: docs in HTML and man page format
###############################################################

docs_opt = get_option('docs')
docs_dep = not_found_dep
if not docs_opt.disabled()
  if xmllint_bin.found() and xsltproc_bin.found()
    docs_dep = declare_dependency()
  elif docs_opt.enabled()
    error('missing required tools (xmllint and xsltproc needed) for docs in HTML / man page format')
  endif
endif



###############################################################
# Option: docs in PDF format
###############################################################

docs_pdf_opt = get_option('docs_pdf')
docs_pdf_dep = not_found_dep
if not docs_pdf_opt.disabled()
  fop = find_program(get_option('FOP'), native: true, required: docs_pdf_opt)
  if xmllint_bin.found() and xsltproc_bin.found() and fop.found()
    docs_pdf_dep = declare_dependency()
  elif docs_pdf_opt.enabled()
    error('missing required tools for docs in PDF format')
  endif
endif



###############################################################
# Library: GSSAPI
###############################################################

gssapiopt = get_option('gssapi')
krb_srvtab = ''
have_gssapi = false
if not gssapiopt.disabled()
  gssapi = dependency('krb5-gssapi', required: false)
  have_gssapi = gssapi.found()

  if have_gssapi
      gssapi_deps = [gssapi]
  elif not have_gssapi
    # Hardcoded lookup for gssapi. This is necessary as gssapi on windows does
    # not install neither pkg-config nor cmake dependency information.
    if host_system == 'windows'
      is_64  = cc.sizeof('void *', args: test_c_args) == 8
      if is_64
        gssapi_search_libs = ['gssapi64', 'krb5_64', 'comerr64']
      else
        gssapi_search_libs = ['gssapi32', 'krb5_32', 'comerr32']
      endif
    else
      gssapi_search_libs = ['gssapi_krb5']
    endif

    gssapi_deps = []
    foreach libname : gssapi_search_libs
      lib = cc.find_library(libname, dirs: test_lib_d, required: false)
      if lib.found()
        have_gssapi = true
        gssapi_deps += lib
      endif
    endforeach

    if have_gssapi
      # Meson before 0.57.0 did not support using check_header() etc with
      # declare_dependency(). Thus the tests below use the library looked up
      # above.  Once we require a newer meson version, we can simplify.
      gssapi = declare_dependency(dependencies: gssapi_deps)
    endif
  endif

  if not have_gssapi
  elif cc.check_header('gssapi/gssapi.h', dependencies: gssapi_deps, required: false,
      args: test_c_args, include_directories: postgres_inc)
    cdata.set('HAVE_GSSAPI_GSSAPI_H', 1)
  elif cc.check_header('gssapi.h', dependencies: gssapi_deps, required: gssapiopt,
      args: test_c_args, include_directories: postgres_inc)
    cdata.set('HAVE_GSSAPI_H', 1)
  else
    have_gssapi = false
  endif

  if not have_gssapi
  elif cc.check_header('gssapi/gssapi_ext.h', dependencies: gssapi_deps, required: false,
      args: test_c_args, include_directories: postgres_inc)
    cdata.set('HAVE_GSSAPI_GSSAPI_EXT_H', 1)
  elif cc.check_header('gssapi_ext.h', dependencies: gssapi_deps, required: gssapiopt,
      args: test_c_args, include_directories: postgres_inc)
    cdata.set('HAVE_GSSAPI_EXT_H', 1)
  else
    have_gssapi = false
  endif

  if not have_gssapi
  elif cc.has_function('gss_store_cred_into', dependencies: gssapi_deps,
      args: test_c_args, include_directories: postgres_inc)
    cdata.set('ENABLE_GSS', 1)

    krb_srvtab = 'FILE:/@0@/krb5.keytab)'.format(get_option('sysconfdir'))
    cdata.set_quoted('PG_KRB_SRVTAB', krb_srvtab)
  elif gssapiopt.enabled()
    error('''could not find function 'gss_store_cred_into' required for GSSAPI''')
  else
    have_gssapi = false
  endif

  if not have_gssapi and gssapiopt.enabled()
    error('dependency lookup for gssapi failed')
  endif

endif
if not have_gssapi
  gssapi = not_found_dep
endif



###############################################################
# Library: ldap
###############################################################

ldapopt = get_option('ldap')
if ldapopt.disabled()
  ldap = not_found_dep
  ldap_r = not_found_dep
elif host_system == 'windows'
  ldap = cc.find_library('wldap32', required: ldapopt)
  ldap_r = ldap
else
  # macos framework dependency is buggy for ldap (one can argue whether it's
  # Apple's or meson's fault), leading to an endless recursion with ldap.h
  # including itself. See https://github.com/mesonbuild/meson/issues/10002
  # Luckily we only need pkg-config support, so the workaround isn't
  # complicated.
  ldap = dependency('ldap', method: 'pkg-config', required: false)
  ldap_r = ldap

  # Before 2.5 openldap didn't have a pkg-config file, and it might not be
  # installed
  if not ldap.found()
    ldap = cc.find_library('ldap', required: ldapopt, dirs: test_lib_d,
      has_headers: 'ldap.h', header_include_directories: postgres_inc)

    # The separate ldap_r library only exists in OpenLDAP < 2.5, and if we
    # have 2.5 or later, we shouldn't even probe for ldap_r (we might find a
    # library from a separate OpenLDAP installation).  The most reliable
    # way to check that is to check for a function introduced in 2.5.
    if not ldap.found()
      # don't have ldap, we shouldn't check for ldap_r
    elif cc.has_function('ldap_verify_credentials',
        dependencies: ldap, args: test_c_args)
      ldap_r = ldap # ldap >= 2.5, no need for ldap_r
    else

      # Use ldap_r for FE if available, else assume ldap is thread-safe.
      ldap_r = cc.find_library('ldap_r', required: false, dirs: test_lib_d,
        has_headers: 'ldap.h', header_include_directories: postgres_inc)
      if not ldap_r.found()
        ldap_r = ldap
      else
        # On some platforms ldap_r fails to link without PTHREAD_LIBS.
        ldap_r = declare_dependency(dependencies: [ldap_r, thread_dep])
      endif

      # PostgreSQL sometimes loads libldap_r and plain libldap into the same
      # process.  Check for OpenLDAP versions known not to tolerate doing so;
      # assume non-OpenLDAP implementations are safe.  The dblink test suite
      # exercises the hazardous interaction directly.
      compat_test_code = '''
#include <ldap.h>
#if !defined(LDAP_VENDOR_VERSION) || \
     (defined(LDAP_API_FEATURE_X_OPENLDAP) && \
      LDAP_VENDOR_VERSION >= 20424 && LDAP_VENDOR_VERSION <= 20431)
choke me
#endif
'''
      if not cc.compiles(compat_test_code,
          name: 'LDAP implementation compatible',
          dependencies: ldap, args: test_c_args)
        warning('''
*** With OpenLDAP versions 2.4.24 through 2.4.31, inclusive, each backend
*** process that loads libpq (via WAL receiver, dblink, or postgres_fdw) and
*** also uses LDAP will crash on exit.''')
      endif
    endif
  endif

  if ldap.found() and cc.has_function('ldap_initialize',
      dependencies: ldap, args: test_c_args)
    cdata.set('HAVE_LDAP_INITIALIZE', 1)
  endif
endif

if ldap.found()
  assert(ldap_r.found())
  cdata.set('USE_LDAP', 1)
else
  assert(not ldap_r.found())
endif



###############################################################
# Library: LLVM
###############################################################

llvmopt = get_option('llvm')
llvm = not_found_dep
if add_languages('cpp', required: llvmopt, native: false)
  llvm = dependency('llvm', version: '>=14', method: 'config-tool', required: llvmopt)

  if llvm.found()

    cdata.set('USE_LLVM', 1)

    cpp = meson.get_compiler('cpp')

    llvm_binpath = llvm.get_variable(configtool: 'bindir')

    ccache = find_program('ccache', native: true, required: false)

    # Some distros put LLVM and clang in different paths, so fallback to
    # find via PATH, too.
    clang = find_program(llvm_binpath / 'clang', 'clang', required: true)
  endif
elif llvmopt.auto()
  message('llvm requires a C++ compiler')
endif



###############################################################
# Library: icu
###############################################################

icuopt = get_option('icu')
if not icuopt.disabled()
  icu = dependency('icu-uc', required: false)
  if icu.found()
    icu_i18n = dependency('icu-i18n', required: true)
  endif

  # Unfortunately the dependency is named differently with cmake
  if not icu.found() # combine with above once meson 0.60.0 is required
    icu = dependency('ICU', required: icuopt,
                     components: ['uc'], modules: ['ICU::uc'], method: 'cmake')
    if icu.found()
      icu_i18n = dependency('ICU', required: true,
                            components: ['i18n'], modules: ['ICU::i18n'])
    endif
  endif

  if icu.found()
    cdata.set('USE_ICU', 1)
  else
    icu_i18n = not_found_dep
  endif

else
  icu = not_found_dep
  icu_i18n = not_found_dep
endif



###############################################################
# Library: libxml
###############################################################

libxmlopt = get_option('libxml')
if not libxmlopt.disabled()
  libxml = dependency('libxml-2.0', required: false, version: '>= 2.6.23')
  # Unfortunately the dependency is named differently with cmake
  if not libxml.found() # combine with above once meson 0.60.0 is required
    libxml = dependency('LibXml2', required: libxmlopt, version: '>= 2.6.23',
      method: 'cmake')
  endif

  if libxml.found()
    cdata.set('USE_LIBXML', 1)
  endif
else
  libxml = not_found_dep
endif



###############################################################
# Library: libxslt
###############################################################

libxsltopt = get_option('libxslt')
if not libxsltopt.disabled()
  libxslt = dependency('libxslt', required: false)
  # Unfortunately the dependency is named differently with cmake
  if not libxslt.found() # combine with above once meson 0.60.0 is required
    libxslt = dependency('LibXslt', required: libxsltopt, method: 'cmake')
  endif

  if libxslt.found()
    cdata.set('USE_LIBXSLT', 1)
  endif
else
  libxslt = not_found_dep
endif



###############################################################
# Library: lz4
###############################################################

lz4opt = get_option('lz4')
if not lz4opt.disabled()
  lz4 = dependency('liblz4', required: false)
  # Unfortunately the dependency is named differently with cmake
  if not lz4.found() # combine with above once meson 0.60.0 is required
    lz4 = dependency('lz4', required: lz4opt,
                     method: 'cmake', modules: ['LZ4::lz4_shared'],
                    )
  endif

  if lz4.found()
    cdata.set('USE_LZ4', 1)
    cdata.set('HAVE_LIBLZ4', 1)
  endif

else
  lz4 = not_found_dep
endif



###############################################################
# Library: Tcl (for pltcl)
#
# NB: tclConfig.sh is used in autoconf build for getting
# TCL_SHARED_BUILD, TCL_INCLUDE_SPEC, TCL_LIBS and TCL_LIB_SPEC
# variables. For now we have not seen a need to copy
# that behaviour to the meson build.
###############################################################

tclopt = get_option('pltcl')
tcl_version = get_option('tcl_version')
tcl_dep = not_found_dep
if not tclopt.disabled()

  # via pkg-config
  tcl_dep = dependency(tcl_version, required: false)

  if not tcl_dep.found()
    tcl_dep = cc.find_library(tcl_version,
      required: tclopt,
      dirs: test_lib_d)
  endif

  if not cc.has_header('tcl.h', dependencies: tcl_dep, required: tclopt)
    tcl_dep = not_found_dep
  endif
endif



###############################################################
# Library: pam
###############################################################

pamopt = get_option('pam')
if not pamopt.disabled()
  pam = dependency('pam', required: false)

  if not pam.found()
    pam = cc.find_library('pam', required: pamopt, dirs: test_lib_d)
  endif

  if pam.found()
    pam_header_found = false

    # header file <security/pam_appl.h> or <pam/pam_appl.h> is required for PAM.
    if cc.check_header('security/pam_appl.h', dependencies: pam, required: false,
        args: test_c_args, include_directories: postgres_inc)
      cdata.set('HAVE_SECURITY_PAM_APPL_H', 1)
      pam_header_found = true
    elif cc.check_header('pam/pam_appl.h', dependencies: pam, required: pamopt,
        args: test_c_args, include_directories: postgres_inc)
      cdata.set('HAVE_PAM_PAM_APPL_H', 1)
      pam_header_found = true
    endif

    if pam_header_found
      cdata.set('USE_PAM', 1)
    else
      pam = not_found_dep
    endif
  endif
else
  pam = not_found_dep
endif



###############################################################
# Library: Perl (for plperl)
###############################################################

perlopt = get_option('plperl')
perl_dep = not_found_dep
if not perlopt.disabled()
  perl_may_work = true

  # First verify that perl has the necessary dependencies installed
  perl_mods = run_command(
    [perl,
     '-MConfig', '-MOpcode', '-MExtUtils::Embed', '-MExtUtils::ParseXS',
     '-e', ''],
    check: false)
  if perl_mods.returncode() != 0
    perl_may_work = false
    perl_msg = 'perl installation does not have the required modules'
  endif

  # Then inquire perl about its configuration
  if perl_may_work
    perl_conf_cmd = [perl, '-MConfig', '-e', 'print $Config{$ARGV[0]}']
    perlversion = run_command(perl_conf_cmd, 'api_versionstring', check: true).stdout()
    archlibexp = run_command(perl_conf_cmd, 'archlibexp', check: true).stdout()
    privlibexp = run_command(perl_conf_cmd, 'privlibexp', check: true).stdout()
    useshrplib = run_command(perl_conf_cmd, 'useshrplib', check: true).stdout()

    perl_inc_dir = '@0@/CORE'.format(archlibexp)

    if perlversion.version_compare('< 5.14')
      perl_may_work = false
      perl_msg = 'Perl version 5.14 or later is required, but this is @0@'.format(perlversion)
    elif useshrplib != 'true'
      perl_may_work = false
      perl_msg = 'need a shared perl'
    endif
  endif

  if perl_may_work
    # On most platforms, archlibexp is also where the Perl include files live ...
    perl_ccflags = ['-I@0@'.format(perl_inc_dir)]
    # ... but on newer macOS versions, we must use -iwithsysroot to look
    # under sysroot
    if not fs.is_file('@0@/perl.h'.format(perl_inc_dir)) and \
       fs.is_file('@0@@1@/perl.h'.format(pg_sysroot, perl_inc_dir))
      perl_ccflags = ['-iwithsysroot', perl_inc_dir]
    endif

    # check compiler finds header
    if not cc.has_header('perl.h', required: false,
        args: test_c_args + perl_ccflags, include_directories: postgres_inc)
      perl_may_work = false
      perl_msg = 'missing perl.h'
    endif
  endif

  if perl_may_work
    perl_ccflags_r = run_command(perl_conf_cmd, 'ccflags', check: true).stdout()

    # See comments for PGAC_CHECK_PERL_EMBED_CCFLAGS in perl.m4
    foreach flag : perl_ccflags_r.split(' ')
      if flag.startswith('-D') and \
          (not flag.startswith('-D_') or flag == '_USE_32BIT_TIME_T')
        perl_ccflags += flag
      endif
    endforeach

    if host_system == 'windows'
      perl_ccflags += ['-DPLPERL_HAVE_UID_GID']

      if cc.get_id() == 'msvc'
        # prevent binary mismatch between MSVC built plperl and Strawberry or
        # msys ucrt perl libraries
        perl_v = run_command(perl, '-V').stdout()
        if not perl_v.contains('USE_THREAD_SAFE_LOCALE')
          perl_ccflags += ['-DNO_THREAD_SAFE_LOCALE']
        endif
      endif
    endif

    message('CCFLAGS recommended by perl: @0@'.format(perl_ccflags_r))
    message('CCFLAGS for embedding perl: @0@'.format(' '.join(perl_ccflags)))

    # We are after Embed's ldopts, but without the subset mentioned in
    # Config's ccdlflags and ldflags.  (Those are the choices of those who
    # built the Perl installation, which are not necessarily appropriate
    # for building PostgreSQL.)
    perl_ldopts = run_command(perl, '-e', '''
use ExtUtils::Embed;
use Text::ParseWords;
# tell perl to suppress including these in ldopts
*ExtUtils::Embed::_ldflags =*ExtUtils::Embed::_ccdlflags = sub { return ""; };
# adding an argument to ldopts makes it return a value instead of printing
# print one of these per line so splitting will preserve spaces in file names.
# shellwords eats backslashes, so we need to escape them.
(my $opts = ldopts(undef)) =~ s!\\!\\\\!g;
print "$_\n" foreach shellwords($opts);
''',
     check: true).stdout().strip().split('\n')

    message('LDFLAGS for embedding perl: "@0@"'.format(' '.join(perl_ldopts)))

    perl_dep_int = declare_dependency(
      compile_args: perl_ccflags,
      link_args: perl_ldopts,
      version: perlversion,
    )

    # While we're at it, check that we can link to libperl.
    # On most platforms, if perl.h is there then libperl.so will be too, but
    # at this writing Debian packages them separately.
    perl_link_test = '''
/* see plperl.h */
#ifdef _MSC_VER
#define __inline__ inline
#endif
#include <EXTERN.h>
#include <perl.h>
int main(void)
{
perl_alloc();
}'''
    if not cc.links(perl_link_test, name: 'libperl',
          args: test_c_args + perl_ccflags + perl_ldopts,
          include_directories: postgres_inc)
      perl_may_work = false
      perl_msg = 'missing libperl'
    endif

  endif # perl_may_work

  if perl_may_work
    perl_dep = perl_dep_int
  else
    if perlopt.enabled()
      error('dependency plperl failed: @0@'.format(perl_msg))
    else
      message('disabling optional dependency plperl: @0@'.format(perl_msg))
    endif
  endif
endif



###############################################################
# Library: Python (for plpython)
###############################################################

pyopt = get_option('plpython')
python3_dep = not_found_dep
if not pyopt.disabled()
  pm = import('python')
  python3_inst = pm.find_installation(python.path(), required: pyopt)
  if python3_inst.found()
    python3_dep = python3_inst.dependency(embed: true, required: pyopt)
    # Remove this check after we depend on Meson >= 1.1.0
    if not cc.check_header('Python.h', dependencies: python3_dep, required: pyopt, include_directories: postgres_inc)
      python3_dep = not_found_dep
    endif
  endif
endif



###############################################################
# Library: Readline
###############################################################

if not get_option('readline').disabled()
  libedit_preferred = get_option('libedit_preferred')
  # Set the order of readline dependencies.
  # cc.find_library breaks and throws on the first dependency which
  # is marked as required=true and can't be found. Thus, we only mark
  # the last dependency to look up as required, to not throw too early.
  check_readline_deps = [
    {
      'name': libedit_preferred ? 'libedit' : 'readline',
      'required': false
    },
    {
      'name': libedit_preferred ? 'readline' : 'libedit',
      'required': get_option('readline')
    }
  ]

  foreach readline_dep : check_readline_deps
    readline = dependency(readline_dep['name'], required: false)
    if not readline.found()
      readline = cc.find_library(readline_dep['name'],
        required: readline_dep['required'],
        dirs: test_lib_d)
    endif
    if readline.found()
      break
    endif
  endforeach

  if readline.found()
    cdata.set('HAVE_LIBREADLINE', 1)

    editline_prefix = {
      'header_prefix': 'editline/',
      'flag_prefix': 'EDITLINE_',
    }
    readline_prefix = {
      'header_prefix': 'readline/',
      'flag_prefix': 'READLINE_',
    }
    default_prefix = {
      'header_prefix': '',
      'flag_prefix': '',
    }

    # Set the order of prefixes
    prefixes = libedit_preferred ? \
      [editline_prefix, default_prefix, readline_prefix] : \
      [readline_prefix, default_prefix, editline_prefix]

    at_least_one_header_found = false
    foreach header : ['history', 'readline']
      is_found = false
      foreach prefix : prefixes
        header_file = '@0@@1@.h'.format(prefix['header_prefix'], header)
        # Check history.h and readline.h
        if not is_found and cc.has_header(header_file,
            args: test_c_args, include_directories: postgres_inc,
            dependencies: [readline], required: false)
          if header == 'readline'
            readline_h = header_file
          endif
          cdata.set('HAVE_@0@@1@_H'.format(prefix['flag_prefix'], header).to_upper(), 1)
          is_found = true
          at_least_one_header_found = true
        endif
      endforeach
    endforeach

    if not at_least_one_header_found
      error('''readline header not found
If you have @0@ already installed, see meson-logs/meson-log.txt for details on the
failure. It is possible the compiler isn't looking in the proper directory.
Use -Dreadline=disabled to disable readline support.'''.format(readline_dep))
    endif

    check_funcs = [
      'append_history',
      'history_truncate_file',
      'rl_completion_matches',
      'rl_filename_completion_function',
      'rl_reset_screen_size',
      'rl_variable_bind',
    ]

    foreach func : check_funcs
      found = cc.has_function(func, dependencies: [readline],
        args: test_c_args, include_directories: postgres_inc)
      cdata.set('HAVE_' + func.to_upper(), found ? 1 : false)
    endforeach

    check_vars = [
      'rl_completion_suppress_quote',
      'rl_filename_quote_characters',
      'rl_filename_quoting_function',
    ]

    foreach var : check_vars
      cdata.set('HAVE_' + var.to_upper(),
        cc.has_header_symbol(readline_h, var,
          args: test_c_args, include_directories: postgres_inc,
          prefix: '#include <stdio.h>',
          dependencies: [readline]) ? 1 : false)
    endforeach

    # If found via cc.find_library() ensure headers are found when using the
    # dependency. On meson < 0.57 one cannot do compiler checks using the
    # dependency returned by declare_dependency(), so we can't do this above.
    if readline.type_name() == 'library'
      readline = declare_dependency(dependencies: readline,
        include_directories: postgres_inc)
    endif

    # On windows with mingw readline requires auto-import to successfully
    # link, as the headers don't use declspec(dllimport)
    if host_system == 'windows' and cc.get_id() != 'msvc'
      readline = declare_dependency(dependencies: readline,
        link_args: '-Wl,--enable-auto-import')
    endif
  endif

  # XXX: Figure out whether to implement mingw warning equivalent
else
  readline = not_found_dep
endif



###############################################################
# Library: selinux
###############################################################

selinux = not_found_dep
selinuxopt = get_option('selinux')
if meson.version().version_compare('>=0.59')
  selinuxopt = selinuxopt.disable_auto_if(host_system != 'linux')
endif
selinux = dependency('libselinux', required: selinuxopt, version: '>= 2.1.10')
cdata.set('HAVE_LIBSELINUX',
  selinux.found() ? 1 : false)



###############################################################
# Library: systemd
###############################################################

systemd = not_found_dep
systemdopt = get_option('systemd')
if meson.version().version_compare('>=0.59')
  systemdopt = systemdopt.disable_auto_if(host_system != 'linux')
endif
systemd = dependency('libsystemd', required: systemdopt)
cdata.set('USE_SYSTEMD', systemd.found() ? 1 : false)



###############################################################
# Library: SSL
###############################################################

ssl = not_found_dep
ssl_library = 'none'
sslopt = get_option('ssl')

if sslopt == 'auto' and auto_features.disabled()
  sslopt = 'none'
endif

if sslopt in ['auto', 'openssl']
  openssl_required = (sslopt == 'openssl')

  # Try to find openssl via pkg-config et al, if that doesn't work
  # (e.g. because it's provided as part of the OS, like on FreeBSD), look for
  # the library names that we know about.

  # via pkg-config et al
  ssl = dependency('openssl', required: false)
  # only meson >= 0.57 supports declare_dependency() in cc.has_function(), so
  # we pass cc.find_library() results if necessary
  ssl_int = []

  # via library + headers
  if not ssl.found()
    ssl_lib = cc.find_library('ssl',
      dirs: test_lib_d,
      header_include_directories: postgres_inc,
      has_headers: ['openssl/ssl.h', 'openssl/err.h'],
      required: openssl_required)
    crypto_lib = cc.find_library('crypto',
      dirs: test_lib_d,
      required: openssl_required)
    if ssl_lib.found() and crypto_lib.found()
      ssl_int = [ssl_lib, crypto_lib]
      ssl = declare_dependency(dependencies: ssl_int, include_directories: postgres_inc)
    endif
  elif cc.has_header('openssl/ssl.h', args: test_c_args, dependencies: ssl, required: openssl_required) and \
       cc.has_header('openssl/err.h', args: test_c_args, dependencies: ssl, required: openssl_required)
    ssl_int = [ssl]
  else
    ssl = not_found_dep
  endif

  if ssl.found()
    check_funcs = [
      ['CRYPTO_new_ex_data', {'required': true}],
      ['SSL_new', {'required': true}],

      # Functions introduced in OpenSSL 1.1.1.
      ['SSL_CTX_set_ciphersuites', {'required': true}],

      # Function introduced in OpenSSL 1.0.2, not in LibreSSL.
      ['SSL_CTX_set_cert_cb'],

      # Function introduced in OpenSSL 1.1.1, not in LibreSSL.
      ['X509_get_signature_info'],
      ['SSL_CTX_set_num_tickets'],
    ]

    are_openssl_funcs_complete = true
    foreach c : check_funcs
      func = c.get(0)
      val = cc.has_function(func, args: test_c_args, dependencies: ssl_int)
      required = c.get(1, {}).get('required', false)
      if required and not val
        are_openssl_funcs_complete = false
        if openssl_required
          error('openssl function @0@ is required'.format(func))
        endif
        break
      elif not required
        cdata.set('HAVE_' + func.to_upper(), val ? 1 : false)
      endif
    endforeach

    if are_openssl_funcs_complete
      cdata.set('USE_OPENSSL', 1,
                description: 'Define to 1 to build with OpenSSL support. (-Dssl=openssl)')
      cdata.set('OPENSSL_API_COMPAT', '0x10101000L',
                description: 'Define to the OpenSSL API version in use. This avoids deprecation warnings from newer OpenSSL versions.')
      ssl_library = 'openssl'
    else
      ssl = not_found_dep
    endif
  endif
endif

if sslopt == 'auto' and auto_features.enabled() and not ssl.found()
  error('no SSL library found')
endif



###############################################################
# Library: uuid
###############################################################

uuidopt = get_option('uuid')
if uuidopt != 'none'
  uuidname = uuidopt.to_upper()
  if uuidopt == 'e2fs'
    uuid = dependency('uuid', required: true)
    uuidfunc = 'uuid_generate'
    uuidheader = 'uuid/uuid.h'
  elif uuidopt == 'bsd'
    # libc should have uuid function
    uuid = declare_dependency()
    uuidfunc = 'uuid_to_string'
    uuidheader = 'uuid.h'
  elif uuidopt == 'ossp'
    # In upstream, the package and library is called just 'uuid', but many
    # distros change it to 'ossp-uuid'.
    uuid = dependency('ossp-uuid', 'uuid', required: false)
    uuidfunc = 'uuid_export'
    uuidheader = 'uuid.h'

    # Hardcoded lookup for ossp-uuid. This is necessary as ossp-uuid on
    # windows installs neither a pkg-config nor a cmake dependency
    # information. Nor is there another supported uuid implementation
    # available on windows.
    if not uuid.found()
      uuid = cc.find_library('ossp-uuid',
        required: false, dirs: test_lib_d,
        has_headers: uuidheader, header_include_directories: postgres_inc)
    endif
    if not uuid.found()
      uuid = cc.find_library('uuid',
        required: true, dirs: test_lib_d,
        has_headers: uuidheader, header_include_directories: postgres_inc)
    endif
  else
    error('unknown uuid build option value: @0@'.format(uuidopt))
  endif

  if not cc.has_header_symbol(uuidheader, uuidfunc,
                              args: test_c_args,
                              include_directories: postgres_inc,
                              dependencies: uuid)
    error('uuid library @0@ missing required function @1@'.format(uuidopt, uuidfunc))
  endif
  cdata.set('HAVE_@0@'.format(uuidheader.underscorify().to_upper()), 1)

  cdata.set('HAVE_UUID_@0@'.format(uuidname), 1,
           description: 'Define to 1 if you have @0@ UUID support.'.format(uuidname))
else
  uuid = not_found_dep
endif



###############################################################
# Library: zlib
###############################################################

zlibopt = get_option('zlib')
zlib = not_found_dep
if not zlibopt.disabled()
  zlib_t = dependency('zlib', required: zlibopt)

  if zlib_t.type_name() == 'internal'
    # if fallback was used, we don't need to test if headers are present (they
    # aren't built yet, so we can't test)
    zlib = zlib_t
  elif not zlib_t.found()
    warning('did not find zlib')
  elif not cc.has_header('zlib.h',
      args: test_c_args, include_directories: postgres_inc,
      dependencies: [zlib_t], required: zlibopt)
    warning('zlib header not found')
  else
    zlib = zlib_t
  endif

  if zlib.found()
    cdata.set('HAVE_LIBZ', 1)
  endif
endif



###############################################################
# Library: tap test dependencies
###############################################################

# Check whether tap tests are enabled or not
tap_tests_enabled = false
tapopt = get_option('tap_tests')
if not tapopt.disabled()
  # Checking for perl modules for tap tests
  perl_ipc_run_check = run_command(perl, 'config/check_modules.pl', check: false)
  if perl_ipc_run_check.returncode() != 0
    message(perl_ipc_run_check.stderr().strip())
    if tapopt.enabled()
      error('Additional Perl modules are required to run TAP tests.')
    else
      warning('Additional Perl modules are required to run TAP tests.')
    endif
  else
    tap_tests_enabled = true
  endif
endif



###############################################################
# Library: zstd
###############################################################

zstdopt = get_option('zstd')
if not zstdopt.disabled()
  zstd = dependency('libzstd', required: false, version: '>=1.4.0')
  # Unfortunately the dependency is named differently with cmake
  if not zstd.found() # combine with above once meson 0.60.0 is required
    zstd = dependency('zstd', required: zstdopt, version: '>=1.4.0',
                      method: 'cmake', modules: ['zstd::libzstd_shared'])
  endif

  if zstd.found()
    cdata.set('USE_ZSTD', 1)
    cdata.set('HAVE_LIBZSTD', 1)
  endif

else
  zstd = not_found_dep
endif



###############################################################
# Compiler tests
###############################################################

# Do we need -std=c99 to compile C99 code? We don't want to add -std=c99
# unnecessarily, because we optionally rely on newer features.
c99_test = '''
#include <stdbool.h>
#include <complex.h>
#include <tgmath.h>
#include <inttypes.h>

struct named_init_test {
  int a;
  int b;
};

extern void structfunc(struct named_init_test);

int main(int argc, char **argv)
{
  struct named_init_test nit = {
    .a = 3,
    .b = 5,
  };

  for (int loop_var = 0; loop_var < 3; loop_var++)
  {
    nit.a += nit.b;
  }

  structfunc((struct named_init_test){1, 0});

  return nit.a != 0;
}
'''

if not cc.compiles(c99_test, name: 'c99', args: test_c_args)
  if cc.compiles(c99_test, name: 'c99 with -std=c99',
        args: test_c_args + ['-std=c99'])
    test_c_args += '-std=c99'
    cflags += '-std=c99'
  else
    error('C compiler does not support C99')
  endif
endif

sizeof_long = cc.sizeof('long', args: test_c_args)
cdata.set('SIZEOF_LONG', sizeof_long)
if sizeof_long == 8
  cdata.set('HAVE_LONG_INT_64', 1)
  pg_int64_type = 'long int'
  cdata.set_quoted('INT64_MODIFIER', 'l')
elif sizeof_long == 4 and cc.sizeof('long long', args: test_c_args) == 8
  cdata.set('HAVE_LONG_LONG_INT_64', 1)
  pg_int64_type = 'long long int'
  cdata.set_quoted('INT64_MODIFIER', 'll')
else
  error('do not know how to get a 64bit int')
endif
cdata.set('PG_INT64_TYPE', pg_int64_type)

if host_machine.endian() == 'big'
  cdata.set('WORDS_BIGENDIAN', 1)
endif

# Determine memory alignment requirements for the basic C data types.

alignof_types = ['short', 'int', 'long', 'double']
foreach t : alignof_types
  align = cc.alignment(t, args: test_c_args)
  cdata.set('ALIGNOF_@0@'.format(t.to_upper()), align)
endforeach

# Compute maximum alignment of any basic type.
#
# We require 'double' to have the strictest alignment among the basic types,
# because otherwise the C ABI might impose 8-byte alignment on some of the
# other C types that correspond to TYPALIGN_DOUBLE SQL types.  That could
# cause a mismatch between the tuple layout and the C struct layout of a
# catalog tuple.  We used to carefully order catalog columns such that any
# fixed-width, attalign=4 columns were at offsets divisible by 8 regardless
# of MAXIMUM_ALIGNOF to avoid that, but we no longer support any platforms
# where TYPALIGN_DOUBLE != MAXIMUM_ALIGNOF.
#
# We assume without checking that int64's alignment is at least as strong
# as long, char, short, or int.  Note that we intentionally do not consider
# any types wider than 64 bits, as allowing MAXIMUM_ALIGNOF to exceed 8
# would be too much of a penalty for disk and memory space.
alignof_double = cdata.get('ALIGNOF_DOUBLE')
if cc.alignment(pg_int64_type, args: test_c_args) > alignof_double
  error('alignment of int64 is greater than the alignment of double')
endif
cdata.set('MAXIMUM_ALIGNOF', alignof_double)

cdata.set('SIZEOF_VOID_P', cc.sizeof('void *', args: test_c_args))
cdata.set('SIZEOF_SIZE_T', cc.sizeof('size_t', args: test_c_args))


# Check if __int128 is a working 128 bit integer type, and if so
# define PG_INT128_TYPE to that typename.
#
# This currently only detects a GCC/clang extension, but support for other
# environments may be added in the future.
#
# For the moment we only test for support for 128bit math; support for
# 128bit literals and snprintf is not required.
if cc.links('''
  /*
   * We don't actually run this test, just link it to verify that any support
   * functions needed for __int128 are present.
   *
   * These are globals to discourage the compiler from folding all the
   * arithmetic tests down to compile-time constants.  We do not have
   * convenient support for 128bit literals at this point...
   */
  __int128 a = 48828125;
  __int128 b = 97656250;

  int main(void)
  {
      __int128 c,d;
      a = (a << 12) + 1; /* 200000000001 */
      b = (b << 12) + 5; /* 400000000005 */
      /* try the most relevant arithmetic ops */
      c = a * b;
      d = (c + b) / b;
      /* must use the results, else compiler may optimize arithmetic away */
      return d != a+1;
  }''',
  name: '__int128',
  args: test_c_args)

  buggy_int128 = false

  # Use of non-default alignment with __int128 tickles bugs in some compilers.
  # If not cross-compiling, we can test for bugs and disable use of __int128
  # with buggy compilers.  If cross-compiling, hope for the best.
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83925
  if not meson.is_cross_build()
    r = cc.run('''
    /* This must match the corresponding code in c.h: */
    #if defined(__GNUC__) || defined(__SUNPRO_C)
    #define pg_attribute_aligned(a) __attribute__((aligned(a)))
    #elif defined(_MSC_VER)
    #define pg_attribute_aligned(a) __declspec(align(a))
    #endif
    typedef __int128 int128a
    #if defined(pg_attribute_aligned)
    pg_attribute_aligned(8)
    #endif
    ;

    int128a holder;
    void pass_by_val(void *buffer, int128a par) { holder = par; }

    int main(void)
    {
        long int i64 = 97656225L << 12;
        int128a q;
        pass_by_val(main, (int128a) i64);
        q = (int128a) i64;
        return q != holder;
    }''',
    name: '__int128 alignment bug',
    args: test_c_args)
    assert(r.compiled())
    if r.returncode() != 0
      buggy_int128 = true
      message('__int128 support present but buggy and thus disabled')
    endif
  endif

  if not buggy_int128
    cdata.set('PG_INT128_TYPE', '__int128')
    cdata.set('ALIGNOF_PG_INT128_TYPE', cc.alignment('__int128', args: test_c_args))
  endif
endif


# Check if the C compiler knows computed gotos (gcc extension, also
# available in at least clang).  If so, define HAVE_COMPUTED_GOTO.
#
# Checking whether computed gotos are supported syntax-wise ought to
# be enough, as the syntax is otherwise illegal.
if cc.compiles('''
    static inline int foo(void)
    {
      void *labeladdrs[] = {&&my_label};
      goto *labeladdrs[0];
      my_label:
      return 1;
    }''',
    args: test_c_args)
  cdata.set('HAVE_COMPUTED_GOTO', 1)
endif


# Check if the C compiler understands _Static_assert(),
# and define HAVE__STATIC_ASSERT if so.
#
# We actually check the syntax ({ _Static_assert(...) }), because we need
# gcc-style compound expressions to be able to wrap the thing into macros.
if cc.compiles('''
    int main(int arg, char **argv)
    {
        ({ _Static_assert(1, "foo"); });
    }
    ''',
    args: test_c_args)
  cdata.set('HAVE__STATIC_ASSERT', 1)
endif


# We use <stdbool.h> if we have it and it declares type bool as having
# size 1.  Otherwise, c.h will fall back to declaring bool as unsigned char.
if cc.has_type('_Bool', args: test_c_args) \
    and cc.has_type('bool', prefix: '#include <stdbool.h>', args: test_c_args) \
    and cc.sizeof('bool', prefix: '#include <stdbool.h>', args: test_c_args) == 1
  cdata.set('HAVE__BOOL', 1)
  cdata.set('PG_USE_STDBOOL', 1)
endif


# Need to check a call with %m because netbsd supports gnu_printf but emits a
# warning for each use of %m.
printf_attributes = ['gnu_printf', '__syslog__', 'printf']
testsrc = '''
extern void emit_log(int ignore, const char *fmt,...) __attribute__((format(@0@, 2,3)));
static void call_log(void)
{
    emit_log(0, "error: %s: %m", "foo");
}
'''
attrib_error_args = cc.get_supported_arguments('-Werror=format', '-Werror=ignored-attributes')
foreach a : printf_attributes
  if cc.compiles(testsrc.format(a),
      args: test_c_args + attrib_error_args, name: 'format ' + a)
    cdata.set('PG_PRINTF_ATTRIBUTE', a)
    break
  endif
endforeach


if cc.has_function_attribute('visibility:default') and \
    cc.has_function_attribute('visibility:hidden')
  cdata.set('HAVE_VISIBILITY_ATTRIBUTE', 1)

  # Only newer versions of meson know not to apply gnu_symbol_visibility =
  # inlineshidden to C code as well... And either way, we want to put these
  # flags into exported files (pgxs, .pc files).
  cflags_mod += '-fvisibility=hidden'
  cxxflags_mod += ['-fvisibility=hidden', '-fvisibility-inlines-hidden']
  ldflags_mod += '-fvisibility=hidden'
endif


# Check if various builtins exist. Some builtins are tested separately,
# because we want to test something more complicated than the generic case.
builtins = [
  'bswap16',
  'bswap32',
  'bswap64',
  'clz',
  'ctz',
  'constant_p',
  'frame_address',
  'popcount',
  'unreachable',
]

foreach builtin : builtins
  fname = '__builtin_@0@'.format(builtin)
  if cc.has_function(fname, args: test_c_args)
    cdata.set('HAVE@0@'.format(fname.to_upper()), 1)
  endif
endforeach


# Check if the C compiler understands __builtin_types_compatible_p,
# and define HAVE__BUILTIN_TYPES_COMPATIBLE_P if so.
#
# We check usage with __typeof__, though it's unlikely any compiler would
# have the former and not the latter.
if cc.compiles('''
    static int x;
    static int y[__builtin_types_compatible_p(__typeof__(x), int)];
    ''',
    name: '__builtin_types_compatible_p',
    args: test_c_args)
  cdata.set('HAVE__BUILTIN_TYPES_COMPATIBLE_P', 1)
endif


# Check if the C compiler understands __builtin_$op_overflow(),
# and define HAVE__BUILTIN_OP_OVERFLOW if so.
#
# Check for the most complicated case, 64 bit multiplication, as a
# proxy for all of the operations.  To detect the case where the compiler
# knows the function but library support is missing, we must link not just
# compile, and store the results in global variables so the compiler doesn't
# optimize away the call.
if cc.links('''
    INT64 a = 1;
    INT64 b = 1;
    INT64 result;

    int main(void)
    {
        return __builtin_mul_overflow(a, b, &result);
    }''',
    name: '__builtin_mul_overflow',
    args: test_c_args + ['-DINT64=@0@'.format(cdata.get('PG_INT64_TYPE'))],
    )
  cdata.set('HAVE__BUILTIN_OP_OVERFLOW', 1)
endif


# XXX: The configure.ac check for __cpuid() is broken, we don't copy that
# here. To prevent problems due to two detection methods working, stop
# checking after one.
if cc.links('''
    #include <cpuid.h>
    int main(int arg, char **argv)
    {
        unsigned int exx[4] = {0, 0, 0, 0};
        __get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
    }
    ''', name: '__get_cpuid',
    args: test_c_args)
  cdata.set('HAVE__GET_CPUID', 1)
elif cc.links('''
    #include <intrin.h>
    int main(int arg, char **argv)
    {
        unsigned int exx[4] = {0, 0, 0, 0};
        __cpuid(exx, 1);
    }
    ''', name: '__cpuid',
    args: test_c_args)
  cdata.set('HAVE__CPUID', 1)
endif


# Check for __get_cpuid_count() and __cpuidex() in a similar fashion.
if cc.links('''
    #include <cpuid.h>
    int main(int arg, char **argv)
    {
        unsigned int exx[4] = {0, 0, 0, 0};
        __get_cpuid_count(7, 0, &exx[0], &exx[1], &exx[2], &exx[3]);
    }
    ''', name: '__get_cpuid_count',
    args: test_c_args)
  cdata.set('HAVE__GET_CPUID_COUNT', 1)
elif cc.links('''
    #include <intrin.h>
    int main(int arg, char **argv)
    {
        unsigned int exx[4] = {0, 0, 0, 0};
        __cpuidex(exx, 7, 0);
    }
    ''', name: '__cpuidex',
    args: test_c_args)
  cdata.set('HAVE__CPUIDEX', 1)
endif


# Defend against clang being used on x86-32 without SSE2 enabled.  As current
# versions of clang do not understand -fexcess-precision=standard, the use of
# x87 floating point operations leads to problems like isinf possibly returning
# false for a value that is infinite when converted from the 80bit register to
# the 8byte memory representation.
#
# Only perform the test if the compiler doesn't understand
# -fexcess-precision=standard, that way a potentially fixed compiler will work
# automatically.
if '-fexcess-precision=standard' not in cflags
  if not cc.compiles('''
#if defined(__clang__) && defined(__i386__) && !defined(__SSE2_MATH__)
choke me
#endif''',
      name: '', args: test_c_args)
    error('Compiling PostgreSQL with clang, on 32bit x86, requires SSE2 support. Use -msse2 or use gcc.')
  endif
endif



###############################################################
# Compiler flags
###############################################################

common_functional_flags = [
  # Disable strict-aliasing rules; needed for gcc 3.3+
  '-fno-strict-aliasing',
  # Disable optimizations that assume no overflow; needed for gcc 4.3+
  '-fwrapv',
  '-fexcess-precision=standard',
]

cflags += cc.get_supported_arguments(common_functional_flags)
if llvm.found()
  cxxflags += cpp.get_supported_arguments(common_functional_flags)
endif

vectorize_cflags = cc.get_supported_arguments(['-ftree-vectorize'])
unroll_loops_cflags = cc.get_supported_arguments(['-funroll-loops'])

common_warning_flags = [
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  # Really don't want VLAs to be used in our dialect of C
  '-Werror=vla',
  # On macOS, complain about usage of symbols newer than the deployment target
  '-Werror=unguarded-availability-new',
  '-Wendif-labels',
  '-Wmissing-format-attribute',
  '-Wimplicit-fallthrough=3',
  '-Wcast-function-type',
  '-Wshadow=compatible-local',
  # This was included in -Wall/-Wformat in older GCC versions
  '-Wformat-security',
]

cflags_warn += cc.get_supported_arguments(common_warning_flags)
if llvm.found()
  cxxflags_warn += cpp.get_supported_arguments(common_warning_flags)
endif

# A few places with imported code get a pass on -Wdeclaration-after-statement, remember
# the result for them
cflags_no_decl_after_statement = []
if cc.has_argument('-Wdeclaration-after-statement')
  cflags_warn += '-Wdeclaration-after-statement'
  cflags_no_decl_after_statement += '-Wno-declaration-after-statement'
endif

# Some code is not clean for -Wmissing-variable-declarations, so we
# make the "no" option available.  Also, while clang supports this
# option for C++, gcc does not, so for consistency, leave it off for
# C++.
cflags_no_missing_var_decls = []
if cc.has_argument('-Wmissing-variable-declarations')
  cflags_warn += '-Wmissing-variable-declarations'
  cflags_no_missing_var_decls += '-Wno-missing-variable-declarations'
endif


# The following tests want to suppress various unhelpful warnings by adding
# -Wno-foo switches.  But gcc won't complain about unrecognized -Wno-foo
# switches, so we have to test for the positive form and if that works,
# add the negative form.

negative_warning_flags = [
  # Suppress clang's unhelpful unused-command-line-argument warnings.
  'unused-command-line-argument',

  # Remove clang 12+'s compound-token-split-by-macro, as this causes a lot
  # of warnings when building plperl because of usages in the Perl headers.
  'compound-token-split-by-macro',

  # Similarly disable useless truncation warnings from gcc 8+
  'format-truncation',
  'stringop-truncation',

  # Suppress clang 16's strict warnings about function casts
  'cast-function-type-strict',

  # To make warning_level=2 / -Wextra work, we'd need at least the following
  # 'clobbered',
  # 'missing-field-initializers',
  # 'sign-compare',
  # 'unused-parameter',
]

foreach w : negative_warning_flags
  if cc.has_argument('-W' + w)
    cflags_warn += '-Wno-' + w
  endif
  if llvm.found() and cpp.has_argument('-W' + w)
    cxxflags_warn += '-Wno-' + w
  endif
endforeach


if cc.get_id() == 'msvc'
  cflags_warn += [
    '/wd4018', # signed/unsigned mismatch
    '/wd4244', # conversion from 'type1' to 'type2', possible loss of data
    '/wd4273', # inconsistent DLL linkage
    '/wd4101', # unreferenced local variable
    '/wd4102', # unreferenced label
    '/wd4090', # different 'modifier' qualifiers
    '/wd4267', # conversion from 'size_t' to 'type', possible loss of data
  ]

  cppflags += [
    '/DWIN32',
    '/DWINDOWS',
    '/D__WINDOWS__',
    '/D__WIN32__',
    '/D_CRT_SECURE_NO_DEPRECATE',
    '/D_CRT_NONSTDC_NO_DEPRECATE',
  ]

  # We never need export libraries. As link.exe reports their creation, they
  # are unnecessarily noisy. Similarly, we don't need import library for
  # modules, we only import them dynamically, and they're also noisy.
  ldflags += '/NOEXP'
  ldflags_mod += '/NOIMPLIB'
endif


# Compute flags that are built into Meson.  We need these to
# substitute into Makefile.global and for pg_config.  We only compute
# the flags for Unix-style compilers, since that's the only style that
# would use Makefile.global or pg_config.

# We don't use get_option('warning_level') here, because the other
# warning levels are not useful with PostgreSQL source code.
common_builtin_flags = ['-Wall']

if get_option('debug')
  common_builtin_flags += ['-g']
endif

optimization = get_option('optimization')
if optimization == '0'
  common_builtin_flags += ['-O0']
elif optimization == '1'
  common_builtin_flags += ['-O1']
elif optimization == '2'
  common_builtin_flags += ['-O2']
elif optimization == '3'
  common_builtin_flags += ['-O3']
elif optimization == 's'
  common_builtin_flags += ['-Os']
endif

cflags_builtin = cc.get_supported_arguments(common_builtin_flags)
if llvm.found()
  cxxflags_builtin = cpp.get_supported_arguments(common_builtin_flags)
endif



###############################################################
# Atomics
###############################################################

atomic_checks = [
  {'name': 'HAVE_GCC__SYNC_CHAR_TAS',
   'desc': '__sync_lock_test_and_set(char)',
   'test': '''
char lock = 0;
__sync_lock_test_and_set(&lock, 1);
__sync_lock_release(&lock);'''},

  {'name': 'HAVE_GCC__SYNC_INT32_TAS',
   'desc': '__sync_lock_test_and_set(int32)',
   'test': '''
int lock = 0;
__sync_lock_test_and_set(&lock, 1);
__sync_lock_release(&lock);'''},

  {'name': 'HAVE_GCC__SYNC_INT32_CAS',
   'desc': '__sync_val_compare_and_swap(int32)',
   'test': '''
int val = 0;
__sync_val_compare_and_swap(&val, 0, 37);'''},

  {'name': 'HAVE_GCC__SYNC_INT64_CAS',
   'desc': '__sync_val_compare_and_swap(int64)',
   'test': '''
INT64 val = 0;
__sync_val_compare_and_swap(&val, 0, 37);'''},

  {'name': 'HAVE_GCC__ATOMIC_INT32_CAS',
   'desc': ' __atomic_compare_exchange_n(int32)',
   'test': '''
int val = 0;
int expect = 0;
__atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);'''},

  {'name': 'HAVE_GCC__ATOMIC_INT64_CAS',
   'desc': ' __atomic_compare_exchange_n(int64)',
   'test': '''
INT64 val = 0;
INT64 expect = 0;
__atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);'''},
]

foreach check : atomic_checks
  test = '''
int main(void)
{
@0@
}'''.format(check['test'])

  cdata.set(check['name'],
    cc.links(test,
      name: check['desc'],
      args: test_c_args + ['-DINT64=@0@'.format(cdata.get('PG_INT64_TYPE'))]) ? 1 : false
  )
endforeach


###############################################################
# Check for the availability of XSAVE intrinsics.
###############################################################

if host_cpu == 'x86' or host_cpu == 'x86_64'

  prog = '''
#include <immintrin.h>

#if defined(__has_attribute) && __has_attribute (target)
__attribute__((target("xsave")))
#endif
int main(void)
{
    return _xgetbv(0) & 0xe0;
}
'''

  if cc.links(prog, name: 'XSAVE intrinsics', args: test_c_args)
    cdata.set('HAVE_XSAVE_INTRINSICS', 1)
  endif

endif


###############################################################
# Check for the availability of AVX-512 popcount intrinsics.
###############################################################

if host_cpu == 'x86_64'

  prog = '''
#include <immintrin.h>

#if defined(__has_attribute) && __has_attribute (target)
__attribute__((target("avx512vpopcntdq,avx512bw")))
#endif
int main(void)
{
    const char buf[sizeof(__m512i)];
    INT64 popcnt = 0;
    __m512i accum = _mm512_setzero_si512();
    const __m512i val = _mm512_maskz_loadu_epi8((__mmask64) 0xf0f0f0f0f0f0f0f0, (const __m512i *) buf);
    const __m512i cnt = _mm512_popcnt_epi64(val);
    accum = _mm512_add_epi64(accum, cnt);
    popcnt = _mm512_reduce_add_epi64(accum);
    /* return computed value, to prevent the above being optimized away */
    return popcnt == 0;
}
'''

  if cc.links(prog, name: 'AVX-512 popcount',
        args: test_c_args + ['-DINT64=@0@'.format(cdata.get('PG_INT64_TYPE'))])
    cdata.set('USE_AVX512_POPCNT_WITH_RUNTIME_CHECK', 1)
  endif

endif


###############################################################
# Select CRC-32C implementation.
#
# If we are targeting a processor that has Intel SSE 4.2 instructions, we can
# use the special CRC instructions for calculating CRC-32C. If we're not
# targeting such a processor, but we can nevertheless produce code that uses
# the SSE intrinsics, perhaps with some extra CFLAGS, compile both
# implementations and select which one to use at runtime, depending on whether
# SSE 4.2 is supported by the processor we're running on.
#
# Similarly, if we are targeting an ARM processor that has the CRC
# instructions that are part of the ARMv8 CRC Extension, use them. And if
# we're not targeting such a processor, but can nevertheless produce code that
# uses the CRC instructions, compile both, and select at runtime.
###############################################################

have_optimized_crc = false
cflags_crc = []
if host_cpu == 'x86' or host_cpu == 'x86_64'

  if cc.get_id() == 'msvc'
    cdata.set('USE_SSE42_CRC32C', false)
    cdata.set('USE_SSE42_CRC32C_WITH_RUNTIME_CHECK', 1)
    have_optimized_crc = true
  else

    prog = '''
#include <nmmintrin.h>

int main(void)
{
    unsigned int crc = 0;
    crc = _mm_crc32_u8(crc, 0);
    crc = _mm_crc32_u32(crc, 0);
    /* return computed value, to prevent the above being optimized away */
    return crc == 0;
}
'''

    if cc.links(prog, name: '_mm_crc32_u8 and _mm_crc32_u32 without -msse4.2',
          args: test_c_args)
      # Use Intel SSE 4.2 unconditionally.
      cdata.set('USE_SSE42_CRC32C', 1)
      have_optimized_crc = true
    elif cc.links(prog, name: '_mm_crc32_u8 and _mm_crc32_u32 with -msse4.2',
          args: test_c_args + ['-msse4.2'])
      # Use Intel SSE 4.2, with runtime check. The CPUID instruction is needed for
      # the runtime check.
      cflags_crc += '-msse4.2'
      cdata.set('USE_SSE42_CRC32C', false)
      cdata.set('USE_SSE42_CRC32C_WITH_RUNTIME_CHECK', 1)
      have_optimized_crc = true
    endif

  endif

elif host_cpu == 'arm' or host_cpu == 'aarch64'

  prog = '''
#include <arm_acle.h>

int main(void)
{
    unsigned int crc = 0;
    crc = __crc32cb(crc, 0);
    crc = __crc32ch(crc, 0);
    crc = __crc32cw(crc, 0);
    crc = __crc32cd(crc, 0);

    /* return computed value, to prevent the above being optimized away */
    return crc == 0;
}
'''

  if cc.links(prog, name: '__crc32cb, __crc32ch, __crc32cw, and __crc32cd without -march=armv8-a+crc',
      args: test_c_args)
    # Use ARM CRC Extension unconditionally
    cdata.set('USE_ARMV8_CRC32C', 1)
    have_optimized_crc = true
  elif cc.links(prog, name: '__crc32cb, __crc32ch, __crc32cw, and __crc32cd with -march=armv8-a+crc',
      args: test_c_args + ['-march=armv8-a+crc'])
    # Use ARM CRC Extension, with runtime check
    cflags_crc += '-march=armv8-a+crc'
    cdata.set('USE_ARMV8_CRC32C', false)
    cdata.set('USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK', 1)
    have_optimized_crc = true
  endif

elif host_cpu == 'loongarch64'

  prog = '''
int main(void)
{
    unsigned int crc = 0;
    crc = __builtin_loongarch_crcc_w_b_w(0, crc);
    crc = __builtin_loongarch_crcc_w_h_w(0, crc);
    crc = __builtin_loongarch_crcc_w_w_w(0, crc);
    crc = __builtin_loongarch_crcc_w_d_w(0, crc);

    /* return computed value, to prevent the above being optimized away */
    return crc == 0;
}
'''

  if cc.links(prog, name: '__builtin_loongarch_crcc_w_b_w, __builtin_loongarch_crcc_w_h_w, __builtin_loongarch_crcc_w_w_w, and __builtin_loongarch_crcc_w_d_w',
      args: test_c_args)
    # Use LoongArch CRC instruction unconditionally
    cdata.set('USE_LOONGARCH_CRC32C', 1)
    have_optimized_crc = true
  endif

endif

if not have_optimized_crc
  # fall back to slicing-by-8 algorithm, which doesn't require any special CPU
  # support.
  cdata.set('USE_SLICING_BY_8_CRC32C', 1)
endif



###############################################################
# Other CPU specific stuff
###############################################################

if host_cpu == 'x86_64'

  if cc.compiles('''
      void main(void)
      {
          long long x = 1; long long r;
          __asm__ __volatile__ (" popcntq %1,%0\n" : "=q"(r) : "rm"(x));
      }''',
      name: '@0@: popcntq instruction'.format(host_cpu),
      args: test_c_args)
    cdata.set('HAVE_X86_64_POPCNTQ', 1)
  endif

elif host_cpu == 'ppc' or host_cpu == 'ppc64'
  # Check if compiler accepts "i"(x) when __builtin_constant_p(x).
  if cdata.has('HAVE__BUILTIN_CONSTANT_P')
    if cc.compiles('''
      static inline int
      addi(int ra, int si)
      {
          int res = 0;
          if (__builtin_constant_p(si))
              __asm__ __volatile__(
                  " addi %0,%1,%2\n" : "=r"(res) : "b"(ra), "i"(si));
          return res;
      }
      int test_adds(int x) { return addi(3, x) + addi(x, 5); }
      ''',
      args: test_c_args)
      cdata.set('HAVE_I_CONSTRAINT__BUILTIN_CONSTANT_P', 1)
    endif
  endif
endif



###############################################################
# Library / OS tests
###############################################################

# XXX: Might be worth conditioning some checks on the OS, to avoid doing
# unnecessary checks over and over, particularly on windows.
header_checks = [
  'atomic.h',
  'copyfile.h',
  'crtdefs.h',
  'execinfo.h',
  'getopt.h',
  'ifaddrs.h',
  'mbarrier.h',
  'stdbool.h',
  'strings.h',
  'sys/epoll.h',
  'sys/event.h',
  'sys/personality.h',
  'sys/prctl.h',
  'sys/procctl.h',
  'sys/signalfd.h',
  'sys/ucred.h',
  'termios.h',
  'ucred.h',
  'xlocale.h',
]

foreach header : header_checks
  varname = 'HAVE_' + header.underscorify().to_upper()

  # Emulate autoconf behaviour of not-found->undef, found->1
  found = cc.has_header(header,
    include_directories: postgres_inc, args: test_c_args)
  cdata.set(varname, found ? 1 : false,
            description: 'Define to 1 if you have the <@0@> header file.'.format(header))
endforeach


decl_checks = [
  ['F_FULLFSYNC', 'fcntl.h'],
  ['fdatasync', 'unistd.h'],
  ['posix_fadvise', 'fcntl.h'],
  ['strlcat', 'string.h'],
  ['strlcpy', 'string.h'],
  ['strnlen', 'string.h'],
  ['strsep',  'string.h'],
]

# Need to check for function declarations for these functions, because
# checking for library symbols wouldn't handle deployment target
# restrictions on macOS
decl_checks += [
  ['preadv', 'sys/uio.h'],
  ['pwritev', 'sys/uio.h'],
]

# Check presence of some optional LLVM functions.
if llvm.found()
  decl_checks += [
    ['LLVMCreateGDBRegistrationListener', 'llvm-c/ExecutionEngine.h'],
    ['LLVMCreatePerfJITEventListener', 'llvm-c/ExecutionEngine.h'],
  ]
endif

foreach c : decl_checks
  func = c.get(0)
  header = c.get(1)
  args = c.get(2, {})
  varname = 'HAVE_DECL_' + func.underscorify().to_upper()

  found = cc.has_header_symbol(header, func,
    args: test_c_args, include_directories: postgres_inc,
    kwargs: args)
  cdata.set10(varname, found, description:
'''Define to 1 if you have the declaration of `@0@', and to 0 if you
   don't.'''.format(func))
endforeach


if cc.has_type('struct option',
    args: test_c_args, include_directories: postgres_inc,
    prefix: '@0@'.format(cdata.get('HAVE_GETOPT_H')) == '1' ? '#include <getopt.h>' : '')
  cdata.set('HAVE_STRUCT_OPTION', 1)
endif


foreach c : ['opterr', 'optreset']
  varname = 'HAVE_INT_' + c.underscorify().to_upper()

  if cc.links('''
#include <unistd.h>
int main(void)
{
    extern int @0@;
    @0@ = 1;
}
'''.format(c), name: c, args: test_c_args)
    cdata.set(varname, 1)
  else
    cdata.set(varname, false)
  endif
endforeach

if cc.has_type('socklen_t',
    args: test_c_args, include_directories: postgres_inc,
    prefix: '''
#include <sys/socket.h>''')
  cdata.set('HAVE_SOCKLEN_T', 1)
endif

if cc.has_member('struct sockaddr', 'sa_len',
    args: test_c_args, include_directories: postgres_inc,
    prefix: '''
#include <sys/types.h>
#include <sys/socket.h>''')
  cdata.set('HAVE_STRUCT_SOCKADDR_SA_LEN', 1)
endif

if cc.has_member('struct tm', 'tm_zone',
    args: test_c_args, include_directories: postgres_inc,
    prefix: '''
#include <sys/types.h>
#include <time.h>
''')
  cdata.set('HAVE_STRUCT_TM_TM_ZONE', 1)
endif

if cc.compiles('''
#include <time.h>
extern int foo(void);
int foo(void)
{
    return timezone / 60;
}
''',
    name: 'global variable `timezone\' exists',
    args: test_c_args, include_directories: postgres_inc)
  cdata.set('HAVE_INT_TIMEZONE', 1)
else
  cdata.set('HAVE_INT_TIMEZONE', false)
endif

if cc.has_type('union semun',
    args: test_c_args,
    include_directories: postgres_inc,
    prefix: '''
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
''')
  cdata.set('HAVE_UNION_SEMUN', 1)
endif

if cc.compiles('''
#include <string.h>
int main(void)
{
  char buf[100];
  switch (strerror_r(1, buf, sizeof(buf)))
  { case 0: break; default: break; }
}''',
    name: 'strerror_r',
    args: test_c_args, include_directories: postgres_inc)
  cdata.set('STRERROR_R_INT', 1)
else
  cdata.set('STRERROR_R_INT', false)
endif

# Check if the C compiler understands typeof or a variant.  Define
# HAVE_TYPEOF if so, and define 'typeof' to the actual key word.
foreach kw : ['typeof', '__typeof__', 'decltype']
  if cc.compiles('''
int main(void)
{
    int x = 0;
    @0@(x) y;
    y = x;
    return y;
}
'''.format(kw),
    name: 'typeof()',
    args: test_c_args, include_directories: postgres_inc)

    cdata.set('HAVE_TYPEOF', 1)
    if kw != 'typeof'
      cdata.set('typeof', kw)
    endif

    break
  endif
endforeach


# MSVC doesn't cope well with defining restrict to __restrict, the spelling it
# understands, because it conflicts with __declspec(restrict). Therefore we
# define pg_restrict to the appropriate definition, which presumably won't
# conflict.
#
# We assume C99 support, so we don't need to make this conditional.
cdata.set('pg_restrict', '__restrict')


# Most libraries are included only if they demonstrably provide a function we
# need, but libm is an exception: always include it, because there are too
# many compilers that play cute optimization games that will break probes for
# standard functions such as pow().
os_deps += cc.find_library('m', required: false)

rt_dep = cc.find_library('rt', required: false)

dl_dep = cc.find_library('dl', required: false)

util_dep = cc.find_library('util', required: false)

getopt_dep = cc.find_library('getopt', required: false)
gnugetopt_dep = cc.find_library('gnugetopt', required: false)
# Check if we want to replace getopt/getopt_long even if provided by the system
# - Mingw has adopted a GNU-centric interpretation of optind/optreset,
#   so always use our version on Windows
# - On OpenBSD and Solaris, getopt() doesn't do what we want for long options
#   (i.e., allow '-' as a flag character), so use our version on those platforms
# - We want to use system's getopt_long() only if the system provides struct
#   option
always_replace_getopt = host_system in ['windows', 'cygwin', 'openbsd', 'solaris']
always_replace_getopt_long = host_system in ['windows', 'cygwin'] or not cdata.has('HAVE_STRUCT_OPTION')

# Required on BSDs
execinfo_dep = cc.find_library('execinfo', required: false)

if host_system == 'cygwin'
  cygipc_dep = cc.find_library('cygipc', required: false)
else
  cygipc_dep = not_found_dep
endif

if host_system == 'sunos'
  socket_dep = cc.find_library('socket', required: false)
else
  socket_dep = not_found_dep
endif

# XXX: Might be worth conditioning some checks on the OS, to avoid doing
# unnecessary checks over and over, particularly on windows.
func_checks = [
  ['_configthreadlocale', {'skip': host_system != 'windows'}],
  ['backtrace_symbols', {'dependencies': [execinfo_dep]}],
  ['clock_gettime', {'dependencies': [rt_dep], 'define': false}],
  ['copyfile'],
  ['copy_file_range'],
  # gcc/clang's sanitizer helper library provides dlopen but not dlsym, thus
  # when enabling asan the dlopen check doesn't notice that -ldl is actually
  # required. Just checking for dlsym() ought to suffice.
  ['dlsym', {'dependencies': [dl_dep], 'define': false}],
  ['explicit_bzero'],
  ['getifaddrs'],
  ['getopt', {'dependencies': [getopt_dep, gnugetopt_dep], 'skip': always_replace_getopt}],
  ['getopt_long', {'dependencies': [getopt_dep, gnugetopt_dep], 'skip': always_replace_getopt_long}],
  ['getpeereid'],
  ['getpeerucred'],
  ['inet_aton'],
  ['inet_pton'],
  ['kqueue'],
  ['mbstowcs_l'],
  ['memset_s'],
  ['mkdtemp'],
  ['posix_fadvise'],
  ['posix_fallocate'],
  ['ppoll'],
  ['pthread_barrier_wait', {'dependencies': [thread_dep]}],
  ['pthread_is_threaded_np', {'dependencies': [thread_dep]}],
  ['sem_init', {'dependencies': [rt_dep, thread_dep], 'skip': sema_kind != 'unnamed_posix', 'define': false}],
  ['setproctitle', {'dependencies': [util_dep]}],
  ['setproctitle_fast'],
  ['shm_open', {'dependencies': [rt_dep], 'define': false}],
  ['shm_unlink', {'dependencies': [rt_dep], 'define': false}],
  ['shmget', {'dependencies': [cygipc_dep], 'define': false}],
  ['socket', {'dependencies': [socket_dep], 'define': false}],
  ['strchrnul'],
  ['strerror_r', {'dependencies': [thread_dep]}],
  ['strlcat'],
  ['strlcpy'],
  ['strnlen'],
  ['strsep'],
  ['strsignal'],
  ['sync_file_range'],
  ['syncfs'],
  ['uselocale'],
  ['wcstombs_l'],
]

func_check_results = {}
foreach c : func_checks
  func = c.get(0)
  kwargs = c.get(1, {})
  deps = kwargs.get('dependencies', [])

  if kwargs.get('skip', false)
    continue
  endif

  found = cc.has_function(func, args: test_c_args)

  if not found
    foreach dep : deps
      if not dep.found()
        continue
      endif
      found = cc.has_function(func, args: test_c_args,
                              dependencies: [dep])
      if found
        os_deps += dep
        break
      endif
    endforeach
  endif

  func_check_results += {func: found}

  if kwargs.get('define', true)
    # Emulate autoconf behaviour of not-found->undef, found->1
    cdata.set('HAVE_' + func.underscorify().to_upper(),
              found  ? 1 : false,
              description: 'Define to 1 if you have the `@0@\' function.'.format(func))
  endif
endforeach


if cc.has_function('syslog', args: test_c_args) and \
    cc.check_header('syslog.h', args: test_c_args)
  cdata.set('HAVE_SYSLOG', 1)
endif


# if prerequisites for unnamed posix semas aren't fulfilled, fall back to sysv
# semaphores
if sema_kind == 'unnamed_posix' and \
   not func_check_results.get('sem_init', false)
  sema_kind = 'sysv'
endif

cdata.set('USE_@0@_SHARED_MEMORY'.format(shmem_kind.to_upper()), 1)
cdata.set('USE_@0@_SEMAPHORES'.format(sema_kind.to_upper()), 1)

cdata.set('MEMSET_LOOP_LIMIT', memset_loop_limit)
cdata.set_quoted('DLSUFFIX', dlsuffix)


# built later than the rest of the version metadata, we need SIZEOF_VOID_P
cdata.set_quoted('PG_VERSION_STR',
  'PostgreSQL @0@ on @1@-@2@, compiled by @3@-@4@, @5@-bit'.format(
    pg_version, host_machine.cpu_family(), host_system,
    cc.get_id(), cc.version(), cdata.get('SIZEOF_VOID_P') * 8,
  )
)


###############################################################
# NLS / Gettext
###############################################################

nlsopt = get_option('nls')
libintl = not_found_dep

if not nlsopt.disabled()
  # otherwise there'd be lots of
  # "Gettext not found, all translation (po) targets will be ignored."
  # warnings if not found.
  msgfmt = find_program('msgfmt', required: nlsopt, native: true)

  # meson 0.59 has this wrapped in dependency('intl')
  if (msgfmt.found() and
      cc.check_header('libintl.h', required: nlsopt,
        args: test_c_args, include_directories: postgres_inc))

    # in libc
    if cc.has_function('ngettext')
      libintl = declare_dependency()
    else
      libintl = cc.find_library('intl',
        has_headers: ['libintl.h'], required: nlsopt,
        header_include_directories: postgres_inc,
        dirs: test_lib_d)
    endif
  endif

  if libintl.found()
    i18n = import('i18n')
    cdata.set('ENABLE_NLS', 1)
  endif
endif



###############################################################
# Build
###############################################################

# Set up compiler / linker arguments to be used everywhere, individual targets
# can add further args directly, or indirectly via dependencies
add_project_arguments(cflags, language: ['c'])
add_project_arguments(cppflags, language: ['c'])
add_project_arguments(cflags_warn, language: ['c'])
add_project_arguments(cxxflags, language: ['cpp'])
add_project_arguments(cppflags, language: ['cpp'])
add_project_arguments(cxxflags_warn, language: ['cpp'])
add_project_link_arguments(ldflags, language: ['c', 'cpp'])


# Collect a number of lists of things while recursing through the source
# tree. Later steps then can use those.

# list of targets for various alias targets
backend_targets = []
bin_targets = []
pl_targets = []
contrib_targets = []
testprep_targets = []
nls_targets = []


# Define the tests to distribute them to the correct test styles later
test_deps = []
tests = []


# Default options for targets

# First identify rpaths
bin_install_rpaths = []
lib_install_rpaths = []
mod_install_rpaths = []


# Don't add rpaths on darwin for now - as long as only absolute references to
# libraries are needed, absolute LC_ID_DYLIB ensures libraries can be found in
# their final destination.
if host_system != 'darwin'
  # Add absolute path to libdir to rpath. This ensures installed binaries /
  # libraries find our libraries (mainly libpq).
  bin_install_rpaths += dir_prefix / dir_lib
  lib_install_rpaths += dir_prefix / dir_lib
  mod_install_rpaths += dir_prefix / dir_lib

  # Add extra_lib_dirs to rpath. This ensures we find libraries we depend on.
  #
  # Not needed on darwin even if we use relative rpaths for our own libraries,
  # as the install_name of libraries in extra_lib_dirs will point to their
  # location anyway.
  bin_install_rpaths += postgres_lib_d
  lib_install_rpaths += postgres_lib_d
  mod_install_rpaths += postgres_lib_d
endif


# Define arguments for default targets

default_target_args = {
  'implicit_include_directories': false,
  'install': true,
}

default_lib_args = default_target_args + {
  'name_prefix': '',
}

internal_lib_args = default_lib_args + {
  'build_by_default': false,
  'install': false,
}

default_mod_args = default_lib_args + {
  'name_prefix': '',
  'install_dir': dir_lib_pkg,
}

default_bin_args = default_target_args + {
  'install_dir': dir_bin,
}

if get_option('rpath')
  default_lib_args += {
    'install_rpath': ':'.join(lib_install_rpaths),
  }

  default_mod_args += {
    'install_rpath': ':'.join(mod_install_rpaths),
  }

  default_bin_args += {
    'install_rpath': ':'.join(bin_install_rpaths),
  }
endif


# Helper for exporting a limited number of symbols
gen_export_kwargs = {
  'input': 'exports.txt',
  'output': '@BASENAME@.'+export_file_suffix,
  'command': [perl, files('src/tools/gen_export.pl'),
   '--format', export_file_format,
   '--input', '@INPUT0@', '--output', '@OUTPUT0@'],
  'build_by_default': false,
  'install': false,
}



###
### Helpers for custom targets used across the tree
###

catalog_pm = files('src/backend/catalog/Catalog.pm')
perfect_hash_pm = files('src/tools/PerfectHash.pm')
gen_kwlist_deps = [perfect_hash_pm]
gen_kwlist_cmd = [
  perl, '-I', '@SOURCE_ROOT@/src/tools',
  files('src/tools/gen_keywordlist.pl'),
  '--output', '@OUTDIR@', '@INPUT@']



###
### windows resources related stuff
###

if host_system == 'windows'
  pg_ico = meson.source_root() / 'src' / 'port' / 'win32.ico'
  win32ver_rc = files('src/port/win32ver.rc')
  rcgen = find_program('src/tools/rcgen', native: true)

  rcgen_base_args = [
    '--srcdir', '@SOURCE_DIR@',
    '--builddir', meson.build_root(),
    '--rcout', '@OUTPUT0@',
    '--out', '@OUTPUT1@',
    '--input', '@INPUT@',
    '@EXTRA_ARGS@',
  ]

  if cc.get_argument_syntax() == 'msvc'
    rc = find_program('rc', required: true)
    rcgen_base_args += ['--rc', rc.path()]
    rcgen_outputs = ['@BASENAME@.rc', '@BASENAME@.res']
  else
    windres = find_program('windres', required: true)
    rcgen_base_args += ['--windres', windres.path()]
    rcgen_outputs = ['@BASENAME@.rc', '@BASENAME@.obj']
  endif

  # msbuild backend doesn't support this atm
  if meson.backend() == 'ninja'
    rcgen_base_args += ['--depfile', '@DEPFILE@']
  endif

  rcgen_bin_args = rcgen_base_args + [
    '--VFT_TYPE', 'VFT_APP',
    '--FILEENDING', 'exe',
    '--ICO', pg_ico
  ]

  rcgen_lib_args = rcgen_base_args + [
    '--VFT_TYPE', 'VFT_DLL',
    '--FILEENDING', 'dll',
  ]

  rc_bin_gen = generator(rcgen,
    depfile: '@BASENAME@.d',
    arguments: rcgen_bin_args,
    output: rcgen_outputs,
  )

  rc_lib_gen = generator(rcgen,
    depfile: '@BASENAME@.d',
    arguments: rcgen_lib_args,
    output: rcgen_outputs,
  )
endif



# headers that the whole build tree depends on
generated_headers = []
# headers that the backend build depends on
generated_backend_headers = []
# configure_files() output, needs a way of converting to file names
configure_files = []

# generated files that might conflict with a partial in-tree autoconf build
generated_sources = []
# same, for paths that differ between autoconf / meson builds
# elements are [dir, [files]]
generated_sources_ac = {}


# First visit src/include - all targets creating headers are defined
# within. That makes it easy to add the necessary dependencies for the
# subsequent build steps.

subdir('src/include')

subdir('config')

# Then through src/port and src/common, as most other things depend on them

frontend_port_code = declare_dependency(
  compile_args: ['-DFRONTEND'],
  include_directories: [postgres_inc],
  dependencies: os_deps,
)

backend_port_code = declare_dependency(
  compile_args: ['-DBUILDING_DLL'],
  include_directories: [postgres_inc],
  sources: [errcodes], # errcodes.h is needed due to use of ereport
  dependencies: os_deps,
)

subdir('src/port')

frontend_common_code = declare_dependency(
  compile_args: ['-DFRONTEND'],
  include_directories: [postgres_inc],
  sources: generated_headers,
  dependencies: [os_deps, zlib, zstd, lz4],
)

backend_common_code = declare_dependency(
  compile_args: ['-DBUILDING_DLL'],
  include_directories: [postgres_inc],
  sources: generated_headers,
  dependencies: [os_deps, zlib, zstd],
)

subdir('src/common')

# all shared libraries should depend on shlib_code
shlib_code = declare_dependency(
  link_args: ldflags_sl,
)

# all static libraries not part of the backend should depend on this
frontend_stlib_code = declare_dependency(
  include_directories: [postgres_inc],
  link_with: [common_static, pgport_static],
  sources: generated_headers,
  dependencies: [os_deps, libintl],
)

# all shared libraries not part of the backend should depend on this
frontend_shlib_code = declare_dependency(
  include_directories: [postgres_inc],
  link_with: [common_shlib, pgport_shlib],
  sources: generated_headers,
  dependencies: [shlib_code, os_deps, libintl],
)

# Dependencies both for static and shared libpq
libpq_deps += [
  thread_dep,

  gssapi,
  ldap_r,
  libintl,
  ssl,
]

subdir('src/interfaces/libpq')
# fe_utils depends on libpq
subdir('src/fe_utils')

# for frontend binaries
frontend_code = declare_dependency(
  include_directories: [postgres_inc],
  link_with: [fe_utils, common_static, pgport_static],
  sources: generated_headers,
  dependencies: [os_deps, libintl],
)

backend_both_deps += [
  thread_dep,
  bsd_auth,
  gssapi,
  icu,
  icu_i18n,
  ldap,
  libintl,
  libxml,
  lz4,
  pam,
  ssl,
  systemd,
  zlib,
  zstd,
]

backend_mod_deps = backend_both_deps + os_deps

backend_code = declare_dependency(
  compile_args: ['-DBUILDING_DLL'],
  include_directories: [postgres_inc],
  link_args: ldflags_be,
  link_with: [],
  sources: generated_headers + generated_backend_headers,
  dependencies: os_deps + backend_both_deps + backend_deps,
)

# install these files only during test, not main install
test_install_data = []
test_install_libs = []

# src/backend/meson.build defines backend_mod_code used for extension
# libraries.


# Then through the main sources. That way contrib can have dependencies on
# main sources. Note that this explicitly doesn't enter src/test, right now a
# few regression tests depend on contrib files.

subdir('src')

subdir('contrib')

subdir('src/test')
subdir('src/interfaces/libpq/test')
subdir('src/interfaces/ecpg/test')

subdir('doc/src/sgml')

generated_sources_ac += {'': ['GNUmakefile']}

# After processing src/test, add test_install_libs to the testprep_targets
# to build them
testprep_targets += test_install_libs


# If there are any files in the source directory that we also generate in the
# build directory, they might get preferred over the newly generated files,
# e.g. because of a #include "file", which always will search in the current
# directory first.
message('checking for file conflicts between source and build directory')
conflicting_files = []
potentially_conflicting_files_t = []
potentially_conflicting_files_t += generated_headers
potentially_conflicting_files_t += generated_backend_headers
potentially_conflicting_files_t += generated_backend_sources
potentially_conflicting_files_t += generated_sources

potentially_conflicting_files = []

# convert all sources of potentially conflicting files into uniform shape
foreach t : potentially_conflicting_files_t
  potentially_conflicting_files += t.full_path()
endforeach
foreach t1 : configure_files
  if meson.version().version_compare('>=0.59')
    t = fs.parent(t1) / fs.name(t1)
  else
    t = '@0@'.format(t1)
  endif
  potentially_conflicting_files += meson.current_build_dir() / t
endforeach
foreach sub, fnames : generated_sources_ac
  sub = meson.build_root() / sub
  foreach fname : fnames
    potentially_conflicting_files += sub / fname
  endforeach
endforeach

# find and report conflicting files
foreach build_path : potentially_conflicting_files
  build_path = host_system == 'windows' ? fs.as_posix(build_path) : build_path
  # str.replace is in 0.56
  src_path = meson.current_source_dir() / build_path.split(meson.current_build_dir() / '')[1]
  if fs.exists(src_path) or fs.is_symlink(src_path)
    conflicting_files += src_path
  endif
endforeach
# XXX: Perhaps we should generate a file that would clean these up? The list
# can be long.
if conflicting_files.length() > 0
  errmsg_cleanup = '''
Conflicting files in source directory:
  @0@

The conflicting files need to be removed, either by removing the files listed
above, or by running configure and then make maintainer-clean.
'''
  errmsg_cleanup = errmsg_cleanup.format(' '.join(conflicting_files))
  error(errmsg_nonclean_base.format(errmsg_cleanup))
endif



###############################################################
# Install targets
###############################################################


# We want to define additional install targets beyond what meson provides. For
# that we need to define targets depending on nearly everything. We collected
# the results of i18n.gettext() invocations into nls_targets, that also
# includes maintainer targets though. Collect the ones we want as a dependency.
#
# i18n.gettext() doesn't return the dependencies before 0.60 - but the gettext
# generation happens during install, so that's not a real issue.
nls_mo_targets = []
if libintl.found() and meson.version().version_compare('>=0.60')
  # use range() to avoid the flattening of the list that foreach() would do
  foreach off : range(0, nls_targets.length())
    # i18n.gettext() list containing 1) list of built .mo files 2) maintainer
    # -pot target 3) maintainer -pot target
    nls_mo_targets += nls_targets[off][0]
  endforeach
  alias_target('nls', nls_mo_targets)
endif


all_built = [
  backend_targets,
  bin_targets,
  libpq_st,
  pl_targets,
  contrib_targets,
  nls_mo_targets,
  testprep_targets,
  ecpg_targets,
]

# Meson's default install target is quite verbose. Provide one that is quiet.
install_quiet = custom_target('install-quiet',
  output: 'install-quiet',
  build_always_stale: true,
  build_by_default: false,
  command: [meson_bin, meson_args, 'install', '--quiet', '--no-rebuild'],
  depends: all_built,
)

# Target to install files used for tests, which aren't installed by default
install_test_files_args = [
  install_files,
  '--prefix', dir_prefix,
  '--install', contrib_data_dir, test_install_data,
  '--install', dir_lib_pkg, test_install_libs,
]
run_target('install-test-files',
  command: [python] + install_test_files_args,
  depends: testprep_targets,
)



###############################################################
# Test prep
###############################################################

# DESTDIR for the installation we'll run tests in
test_install_destdir = meson.build_root() / 'tmp_install/'

# DESTDIR + prefix appropriately munged
if build_system != 'windows'
  # On unixoid systems this is trivial, we just prepend the destdir
  assert(dir_prefix.startswith('/')) # enforced by meson
  temp_install_bindir = '@0@@1@'.format(test_install_destdir, dir_prefix / dir_bin)
  temp_install_libdir = '@0@@1@'.format(test_install_destdir, dir_prefix / dir_lib)
else
  # drives, drive-relative paths, etc make this complicated on windows, call
  # into a copy of meson's logic for it
  command = [
    python, '-c',
    'import sys; from pathlib import PurePath; d1=sys.argv[1]; d2=sys.argv[2]; print(str(PurePath(d1, *PurePath(d2).parts[1:])))',
    test_install_destdir]
  temp_install_bindir = run_command(command, dir_prefix / dir_bin, check: true).stdout().strip()
  temp_install_libdir = run_command(command, dir_prefix / dir_lib, check: true).stdout().strip()
endif

meson_install_args = meson_args + ['install'] + {
    'meson': ['--quiet', '--only-changed', '--no-rebuild'],
    'muon': []
}[meson_impl]

# setup tests should be run first,
# so define priority for these
setup_tests_priority = 100
test('tmp_install',
    meson_bin, args: meson_install_args ,
    env: {'DESTDIR':test_install_destdir},
    priority: setup_tests_priority,
    timeout: 300,
    is_parallel: false,
    suite: ['setup'])

test('install_test_files',
    python,
    args: install_test_files_args + ['--destdir', test_install_destdir],
    priority: setup_tests_priority,
    is_parallel: false,
    suite: ['setup'])

test_result_dir = meson.build_root() / 'testrun'


# XXX: pg_regress doesn't assign unique ports on windows. To avoid the
# inevitable conflicts from running tests in parallel, hackishly assign
# different ports for different tests.

testport = 40000

test_env = environment()

test_initdb_template = meson.build_root() / 'tmp_install' / 'initdb-template'
test_env.set('PG_REGRESS', pg_regress.full_path())
test_env.set('REGRESS_SHLIB', regress_module.full_path())
test_env.set('INITDB_TEMPLATE', test_initdb_template)

# Add the temporary installation to the library search path on platforms where
# that works (everything but windows, basically). On windows everything
# library-like gets installed into bindir, solving that issue.
if library_path_var != ''
  test_env.prepend(library_path_var, temp_install_libdir)
endif


# Create (and remove old) initdb template directory. Tests use that, where
# possible, to make it cheaper to run tests.
#
# Use python to remove the old cached initdb, as we cannot rely on a working
# 'rm' binary on windows.
test('initdb_cache',
     python,
     args: [
       '-c', '''
import shutil
import sys
import subprocess

shutil.rmtree(sys.argv[1], ignore_errors=True)
sp = subprocess.run(sys.argv[2:] + [sys.argv[1]])
sys.exit(sp.returncode)
''',
       test_initdb_template,
       temp_install_bindir / 'initdb',
       '--auth', 'trust', '--no-sync', '--no-instructions', '--lc-messages=C',
       '--no-clean'
     ],
     priority: setup_tests_priority - 1,
     timeout: 300,
     is_parallel: false,
     env: test_env,
     suite: ['setup'])



###############################################################
# Test Generation
###############################################################

# When using a meson version understanding exclude_suites, define a
# 'tmp_install' test setup (the default) that excludes tests running against a
# pre-existing install and a 'running' setup that conflicts with creation of
# the temporary installation and tap tests (which don't support running
# against a running server).

running_suites = []
install_suites = []
if meson.version().version_compare('>=0.57')
  runningcheck = true
else
  runningcheck = false
endif

testwrap = files('src/tools/testwrap')

foreach test_dir : tests
  testwrap_base = [
    testwrap,
    '--basedir', meson.build_root(),
    '--srcdir', test_dir['sd'],
    # Some test suites are not run by default but can be run if selected by the
    # user via variable PG_TEST_EXTRA. Pass configuration time value of
    # PG_TEST_EXTRA as an argument to testwrap so that it can be overridden by
    # run time value, if any.
    '--pg-test-extra', get_option('PG_TEST_EXTRA'),
  ]

  foreach kind, v : test_dir
    if kind in ['sd', 'bd', 'name']
      continue
    endif

    t = test_dir[kind]

    if kind in ['regress', 'isolation', 'ecpg']
      if kind == 'regress'
        runner = pg_regress
        fallback_dbname = 'regression_@0@'
      elif kind == 'isolation'
        runner = pg_isolation_regress
        fallback_dbname = 'isolation_regression_@0@'
      elif kind == 'ecpg'
        runner = pg_regress_ecpg
        fallback_dbname = 'ecpg_regression_@0@'
      endif

      test_group = test_dir['name']
      test_group_running = test_dir['name'] + '-running'

      test_output = test_result_dir / test_group / kind
      test_output_running = test_result_dir / test_group_running/ kind

      # Unless specified by the test, choose a non-conflicting database name,
      # to avoid conflicts when running against existing server.
      dbname = t.get('dbname',
        fallback_dbname.format(test_dir['name']))

      test_command_base = [
        runner.full_path(),
        '--inputdir', t.get('inputdir', test_dir['sd']),
        '--expecteddir', t.get('expecteddir', test_dir['sd']),
        '--bindir', '',
        '--dlpath', test_dir['bd'],
        '--max-concurrent-tests=20',
        '--dbname', dbname,
      ] + t.get('regress_args', [])

      test_selection = []
      if t.has_key('schedule')
        test_selection += ['--schedule', t['schedule'],]
      endif

      if kind == 'isolation'
        test_selection += t.get('specs', [])
      else
        test_selection += t.get('sql', [])
      endif

      env = test_env
      env.prepend('PATH', temp_install_bindir, test_dir['bd'])

      test_kwargs = {
        'protocol': 'tap',
        'priority': 10,
        'timeout': 1000,
        'depends': test_deps + t.get('deps', []),
        'env': env,
      } + t.get('test_kwargs', {})

      test(test_group / kind,
        python,
        args: [
          testwrap_base,
          '--testgroup', test_group,
          '--testname', kind,
          '--',
          test_command_base,
          '--outputdir', test_output,
          '--temp-instance', test_output / 'tmp_check',
          '--port', testport.to_string(),
          test_selection,
        ],
        suite: test_group,
        kwargs: test_kwargs,
      )
      install_suites += test_group

      # some tests can't support running against running DB
      if runningcheck and t.get('runningcheck', true)
        test(test_group_running / kind,
          python,
          args: [
            testwrap_base,
            '--testgroup', test_group_running,
            '--testname', kind,
            '--',
            test_command_base,
            '--outputdir', test_output_running,
            test_selection,
          ],
          is_parallel: t.get('runningcheck-parallel', true),
          suite: test_group_running,
          kwargs: test_kwargs,
        )
        running_suites += test_group_running
      endif

      testport += 1
    elif kind == 'tap'
      testwrap_tap = testwrap_base
      if not tap_tests_enabled
        testwrap_tap += ['--skip', 'TAP tests not enabled']
      endif

      test_command = [
        perl.path(),
        '-I', meson.source_root() / 'src/test/perl',
        '-I', test_dir['sd'],
      ]

      # Add temporary install, the build directory for non-installed binaries and
      # also test/ for non-installed test binaries built separately.
      env = test_env
      env.prepend('PATH', temp_install_bindir, test_dir['bd'], test_dir['bd'] / 'test')

      foreach name, value : t.get('env', {})
        env.set(name, value)
      endforeach

      test_group = test_dir['name']
      test_kwargs = {
        'protocol': 'tap',
        'suite': test_group,
        'timeout': 1000,
        'depends': test_deps + t.get('deps', []),
        'env': env,
      } + t.get('test_kwargs', {})

      foreach onetap : t['tests']
        # Make tap test names prettier, remove t/ and .pl
        onetap_p = onetap
        if onetap_p.startswith('t/')
          onetap_p = onetap.split('t/')[1]
        endif
        if onetap_p.endswith('.pl')
          onetap_p = fs.stem(onetap_p)
        endif

        test(test_dir['name'] / onetap_p,
          python,
          kwargs: test_kwargs,
          args: testwrap_tap + [
            '--testgroup', test_dir['name'],
            '--testname', onetap_p,
            '--', test_command,
            test_dir['sd'] / onetap,
          ],
        )
      endforeach
      install_suites += test_group
    else
      error('unknown kind @0@ of test in @1@'.format(kind, test_dir['sd']))
    endif

  endforeach # kinds of tests

endforeach # directories with tests

# repeat condition so meson realizes version dependency
if meson.version().version_compare('>=0.57')
  add_test_setup('tmp_install',
    is_default: true,
    exclude_suites: running_suites)
  add_test_setup('running',
    exclude_suites: ['setup'] + install_suites)
endif



###############################################################
# Pseudo targets
###############################################################

alias_target('backend', backend_targets)
alias_target('bin', bin_targets + [libpq_st])
alias_target('pl', pl_targets)
alias_target('contrib', contrib_targets)
alias_target('testprep', testprep_targets)

alias_target('world', all_built, docs)
alias_target('install-world', install_quiet, installdocs)

run_target('help',
  command: [
    perl, '-ne', 'next if /^#/; print',
    files('doc/src/sgml/targets-meson.txt'),
  ]
)



###############################################################
# Distribution archive
###############################################################

# Meson has its own distribution building command (meson dist), but we
# are not using that at this point.  The main problem is that, the way
# they have implemented it, it is not deterministic.  Also, we want it
# to be equivalent to the "make" version for the time being.  But the
# target name "dist" in meson is reserved for that reason, so we call
# the custom target "pgdist".

git = find_program('git', required: false, native: true, disabler: true)
bzip2 = find_program('bzip2', required: false, native: true)

distdir = meson.project_name() + '-' + meson.project_version()

pg_git_revision = get_option('PG_GIT_REVISION')

# Note: core.autocrlf=false is needed to avoid line-ending conversion
# in case the environment has a different setting.  Without this, a
# tarball created on Windows might be different than on, and unusable
# on, Unix machines.

tar_gz = custom_target('tar.gz',
  build_always_stale: true,
  command: [git, '-C', '@SOURCE_ROOT@',
            '-c', 'core.autocrlf=false',
            'archive',
            '--format', 'tar.gz',
            '-9',
            '--prefix', distdir + '/',
            '-o', join_paths(meson.build_root(), '@OUTPUT@'),
            pg_git_revision],
  output: distdir + '.tar.gz',
)

if bzip2.found()
  tar_bz2 = custom_target('tar.bz2',
    build_always_stale: true,
    command: [git, '-C', '@SOURCE_ROOT@',
              '-c', 'core.autocrlf=false',
              '-c', 'tar.tar.bz2.command="@0@" -c'.format(bzip2.path()),
              'archive',
              '--format', 'tar.bz2',
              '--prefix', distdir + '/',
              '-o', join_paths(meson.build_root(), '@OUTPUT@'),
              pg_git_revision],
    output: distdir + '.tar.bz2',
  )
else
  tar_bz2 = custom_target('tar.bz2',
    command: [perl, '-e', 'exit 1'],
    output: distdir + '.tar.bz2',
  )
endif

alias_target('pgdist', [tar_gz, tar_bz2])

# Make the standard "dist" command fail, to prevent accidental use.
# But not if we are in a subproject, in case the parent project wants to
# create a dist using the standard Meson command.
if not meson.is_subproject()
  # We can only pass the identifier perl here when we depend on >= 0.55
  if meson.version().version_compare('>=0.55')
    meson.add_dist_script(perl, '-e', 'exit 1')
  endif
endif



###############################################################
# The End, The End, My Friend
###############################################################

if meson.version().version_compare('>=0.57')

  summary(
    {
      'data block size': '@0@ kB'.format(cdata.get('BLCKSZ') / 1024),
      'WAL block size': '@0@ kB'.format(cdata.get('XLOG_BLCKSZ') / 1024),
      'segment size': get_option('segsize_blocks') != 0 ?
        '@0@ blocks'.format(cdata.get('RELSEG_SIZE')) :
        '@0@ GB'.format(get_option('segsize')),
    },
    section: 'Data layout',
  )

  summary(
    {
      'host system': '@0@ @1@'.format(host_system, host_cpu),
      'build system': '@0@ @1@'.format(build_machine.system(),
                                       build_machine.cpu_family()),
    },
    section: 'System',
  )

  summary(
    {
      'linker': '@0@'.format(cc.get_linker_id()),
      'C compiler': '@0@ @1@'.format(cc.get_id(), cc.version()),
    },
    section: 'Compiler',
  )

  summary(
    {
      'CPP FLAGS': ' '.join(cppflags),
      'C FLAGS, functional': ' '.join(cflags),
      'C FLAGS, warnings': ' '.join(cflags_warn),
      'C FLAGS, modules': ' '.join(cflags_mod),
      'C FLAGS, user specified': ' '.join(get_option('c_args')),
      'LD FLAGS': ' '.join(ldflags + get_option('c_link_args')),
    },
    section: 'Compiler Flags',
  )

  if llvm.found()
    summary(
      {
        'C++ compiler': '@0@ @1@'.format(cpp.get_id(), cpp.version()),
      },
      section: 'Compiler',
    )

    summary(
      {
        'C++ FLAGS, functional': ' '.join(cxxflags),
        'C++ FLAGS, warnings': ' '.join(cxxflags_warn),
        'C++ FLAGS, user specified': ' '.join(get_option('cpp_args')),
      },
      section: 'Compiler Flags',
    )
  endif

  summary(
    {
      'bison': '@0@ @1@'.format(bison.full_path(), bison_version),
      'dtrace': dtrace,
      'flex': '@0@ @1@'.format(flex.full_path(), flex_version),
    },
    section: 'Programs',
  )

  summary(
    {
      'bonjour': bonjour,
      'bsd_auth': bsd_auth,
      'docs': docs_dep,
      'docs_pdf': docs_pdf_dep,
      'gss': gssapi,
      'icu': icu,
      'ldap': ldap,
      'libxml': libxml,
      'libxslt': libxslt,
      'llvm': llvm,
      'lz4': lz4,
      'nls': libintl,
      'openssl': ssl,
      'pam': pam,
      'plperl': perl_dep,
      'plpython': python3_dep,
      'pltcl': tcl_dep,
      'readline': readline,
      'selinux': selinux,
      'systemd': systemd,
      'uuid': uuid,
      'zlib': zlib,
      'zstd': zstd,
    },
    section: 'External libraries',
  )

endif
