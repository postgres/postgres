# Copyright (c) 2022-2024, PostgreSQL Global Development Group

### Compute pgxs_data, used in src/meson.build to generate Makefile.global
### etc, that's complete enough for PGXS to work.


# Emulation of PGAC_CHECK_STRIP
strip_bin = find_program(get_option('STRIP'), required: false, native: true)
strip_cmd = strip_bin.found() ? [strip_bin.path()] : [':']

working_strip = false
if strip_bin.found()
  strip_version = run_command(strip_bin, '-V', check: false)

  if strip_version.returncode() == 0 and (
      strip_version.stdout().contains('GNU strip') or
      strip_version.stderr().contains('GNU strip'))
    working_strip = true
    strip_static_cmd = strip_cmd + ['--strip-unneeded']
    strip_shared_cmd = strip_cmd + ['--strip-unneeded']
  elif host_system == 'darwin'
    working_strip = true
    strip_static_cmd = strip_cmd + ['-x']
    strip_shared_cmd = strip_cmd + ['-x']
  endif
endif

if not working_strip
  strip_cmd = [':']
  strip_static_cmd = [':']
  strip_shared_cmd = [':']
endif


pgxs_kv = {
  'PACKAGE_URL': pg_url,
  'PACKAGE_VERSION': pg_version,
  'PG_MAJORVERSION': pg_version_major,
  'PG_VERSION_NUM': pg_version_num,
  'configure_input': 'meson',

  'vpath_build': 'yes',
  'autodepend': cc.get_argument_syntax() == 'gcc' ? 'yes' : 'no',

  'host_cpu': host_cpu,
  'host': '@0@-@1@'.format(host_cpu, host_system),
  'host_os': host_system,
  'build_os': build_machine.system(),
  'PORTNAME': portname,
  'PG_SYSROOT': pg_sysroot,

  'abs_top_builddir': meson.build_root(),
  'abs_top_srcdir': meson.source_root(),

  'enable_rpath': get_option('rpath') ? 'yes' : 'no',
  'enable_nls': libintl.found() ? 'yes' : 'no',
  'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
  'enable_tap_tests': tap_tests_enabled ? 'yes' : 'no',
  'enable_debug': get_option('debug') ? 'yes' : 'no',
  'enable_coverage': 'no',
  'enable_dtrace': dtrace.found() ? 'yes' : 'no',

  'DLSUFFIX': dlsuffix,
  'EXEEXT': exesuffix,

  'SUN_STUDIO_CC': 'no', # not supported so far

  # want the chosen option, rather than the library
  'with_ssl' : ssl_library,
  'with_uuid': uuidopt,

  'default_port': get_option('pgport'),
  'with_system_tzdata': get_option('system_tzdata'),

  'with_krb_srvnam': get_option('krb_srvnam'),
  'krb_srvtab': krb_srvtab,

  'STRIP': ' '.join(strip_cmd),
  'STRIP_STATIC_LIB': ' '.join(strip_static_cmd),
  'STRIP_SHARED_LIB': ' '.join(strip_shared_cmd),

  # these seem to be standard these days
  'MKDIR_P': 'mkdir -p',
  'LN_S': 'ln -s',
  # Just always use the install_sh fallback that autoconf uses. Unlikely to
  # matter performance-wise for extensions. If it turns out to do, we can
  'install_bin': '$(SHELL) $(top_srcdir)/config/install-sh -c',

  'CC': var_cc,
  'CPP': var_cpp,
  'GCC': cc.get_argument_syntax() == 'gcc' ? 'yes' : 'no',

  'CPPFLAGS': var_cppflags,
  'CFLAGS': var_cflags,
  'CXXFLAGS': var_cxxflags,
  'CFLAGS_SL': var_cflags_sl,
  'CFLAGS_SL_MODULE': ' '.join(cflags_mod),
  'CXXFLAGS_SL_MODULE': ' '.join(cxxflags_mod),
  'PERMIT_DECLARATION_AFTER_STATEMENT':
    ' '.join(cflags_no_decl_after_statement),
  'PERMIT_MISSING_VARIABLE_DECLARATIONS':
    ' '.join(cflags_no_missing_var_decls),

  'CFLAGS_CRC': ' '.join(cflags_crc),
  'CFLAGS_POPCNT': ' '.join(cflags_popcnt),
  'CFLAGS_UNROLL_LOOPS': ' '.join(unroll_loops_cflags),
  'CFLAGS_VECTORIZE': ' '.join(vectorize_cflags),
  'CFLAGS_XSAVE': ' '.join(cflags_xsave),

  'LDFLAGS': var_ldflags,
  'LDFLAGS_EX': var_ldflags_ex,
  'LDFLAGS_EX_BE':
    ' '.join(cc.get_supported_link_arguments('-Wl,--export-dynamic')),
  'LDFLAGS_SL': var_ldflags_sl,

  # TODO: requires bitcode generation to be implemented for meson
  'BITCODE_CFLAGS': '',
  'BITCODE_CXXFLAGS': '',

  'BISONFLAGS': ' '.join(bison_flags),
  'FLEXFLAGS': ' '.join(flex_flags),

  'LIBS': var_libs,
}

if llvm.found()
  pgxs_kv += {
    'CLANG': clang.path(),
    'CXX': ' '.join(cpp.cmd_array()),
    'LLVM_BINPATH': llvm_binpath,
  }
else
  pgxs_kv += {
    'CLANG': '',
    'CXX': '',
    'LLVM_BINPATH': '',
  }
endif

pgxs_bins = {
  'AR':
    find_program(['ar'], native: true, required: false),
  'AWK':
    find_program(['gawk', 'mawk', 'nawk', 'awk'], native: true, required: false),
  'BISON': bison,
  'FLEX': flex,
  'GZIP': gzip,
  'LZ4': program_lz4,
  'OPENSSL': openssl,
  'PERL': perl,
  'PROVE': prove,
  'PYTHON': python,
  'TAR': tar,
  'ZSTD': program_zstd,
  'DTRACE': dtrace,
}

pgxs_empty = [
  'ICU_CFLAGS', # needs to be added, included by public server headers

  # hard to see why we'd need either?
  'ZIC',
  'TCLSH',

  # docs don't seem to be supported by pgxs
  'XMLLINT',
  'XSLTPROC',
  'DBTOEPUB',
  'FOP',

  # supporting coverage for pgxs-in-meson build doesn't seem worth it
  'GENHTML',
  'LCOV',
  'GCOV',
  'MSGFMT_FLAGS',

  # translation doesn't appear to be supported by pgxs
  'MSGFMT',
  'XGETTEXT',
  'MSGMERGE',
  'WANTED_LANGUAGES',

  # Not needed because we don't build the server / PLs with the generated makefile
  'LIBOBJS', 'PG_CRC32C_OBJS', 'PG_POPCNT_OBJS', 'TAS',
  'DTRACEFLAGS', # only server has dtrace probes

  'perl_archlibexp', 'perl_embed_ccflags', 'perl_embed_ldflags', 'perl_includespec', 'perl_privlibexp',
  'python_additional_libs', 'python_includespec', 'python_libdir', 'python_libspec', 'python_majorversion', 'python_version',

  # possible that some of these are referenced explicitly in pgxs makefiles?
  # For now not worth it.
  'TCL_INCLUDE_SPEC', 'TCL_LIBS', 'TCL_LIB_SPEC', 'TCL_SHARED_BUILD',

  'LLVM_CFLAGS', 'LLVM_CPPFLAGS', 'LLVM_CXXFLAGS', 'LLVM_LIBS',

  'LDAP_LIBS_BE', 'LDAP_LIBS_FE',

  'UUID_LIBS',

  'PTHREAD_CFLAGS', 'PTHREAD_LIBS',

  'ICU_LIBS',
]

if host_system == 'windows' and cc.get_argument_syntax() != 'msvc'
  pgxs_bins += {'WINDRES': windres}
else
  pgxs_empty += 'WINDRES'
endif

pgxs_dirs = {
  'prefix': get_option('prefix'),

  'bindir': '${exec_prefix}' / get_option('bindir'),
  'datarootdir': '${prefix}' / get_option('datadir'),
  'datadir': '${datarootdir}',
  'docdir': '${prefix}' / dir_doc,
  'exec_prefix': '${prefix}',
  'htmldir': '${docdir}',
  'includedir': '${prefix}' / get_option('includedir'),
  'libdir': '${exec_prefix}' / get_option('libdir'),
  'localedir': '${prefix}' / get_option('localedir'),
  'mandir': '${prefix}' / get_option('mandir'),
  'sysconfdir': '${prefix}' / get_option('sysconfdir'),
}

pgxs_deps = {
  'bonjour': bonjour,
  'bsd_auth': bsd_auth,
  'gssapi': gssapi,
  'icu': icu,
  'ldap': ldap,
  'libxml': libxml,
  'libxslt': libxslt,
  'llvm': llvm,
  'lz4': lz4,
  'nls': libintl,
  'pam': pam,
  'perl': perl_dep,
  'python': python3_dep,
  'readline': readline,
  'selinux': selinux,
  'systemd': systemd,
  'tcl': tcl_dep,
  'zlib': zlib,
  'zstd': zstd,
}


pgxs_cdata = configuration_data(pgxs_kv)

foreach b, p : pgxs_bins
  pgxs_cdata.set(b, p.found() ? p.path() : '')
endforeach

foreach pe : pgxs_empty
  pgxs_cdata.set(pe, '')
endforeach

foreach d, p : pgxs_dirs
  pgxs_cdata.set(d, p)
endforeach

foreach d, v : pgxs_deps
  pgxs_cdata.set('with_@0@'.format(d), v.found() ? 'yes' : 'no')
endforeach
