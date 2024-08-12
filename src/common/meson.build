# Copyright (c) 2022-2024, PostgreSQL Global Development Group

common_sources = files(
  'archive.c',
  'base64.c',
  'binaryheap.c',
  'blkreftable.c',
  'checksum_helper.c',
  'compression.c',
  'controldata_utils.c',
  'encnames.c',
  'exec.c',
  'file_perm.c',
  'file_utils.c',
  'hashfn.c',
  'ip.c',
  'jsonapi.c',
  'keywords.c',
  'kwlookup.c',
  'link-canary.c',
  'md5_common.c',
  'parse_manifest.c',
  'percentrepl.c',
  'pg_get_line.c',
  'pg_lzcompress.c',
  'pg_prng.c',
  'pgfnames.c',
  'psprintf.c',
  'relpath.c',
  'rmtree.c',
  'saslprep.c',
  'scram-common.c',
  'string.c',
  'stringinfo.c',
  'unicode_case.c',
  'unicode_category.c',
  'unicode_norm.c',
  'username.c',
  'wait_error.c',
  'wchar.c',
)

if ssl.found()
  common_sources += files(
    'cryptohash_openssl.c',
    'hmac_openssl.c',
    'protocol_openssl.c',
  )
else
  common_sources += files(
    'cryptohash.c',
    'hmac.c',
    'md5.c',
    'sha1.c',
    'sha2.c',
  )
endif

common_kwlist = custom_target('kwlist',
  input: files('../include/parser/kwlist.h'),
  output: 'kwlist_d.h',
  depend_files: gen_kwlist_deps,
  command: [gen_kwlist_cmd, '--extern'])
generated_sources += common_kwlist
common_sources += common_kwlist

# The code imported from Ryu gets a pass on declaration-after-statement,
# in order to keep it more closely aligned with its upstream.
ryu_sources = files(
  'd2s.c',
  'f2s.c',
)
ryu_cflags = []

ryu_cflags += cflags_no_decl_after_statement

config_info_sources = files('config_info.c',)
config_info_cflags = [
  '-DVAL_CC="@0@"'.format(var_cc),
  '-DVAL_CPPFLAGS="@0@"'.format(var_cppflags),
  '-DVAL_CFLAGS="@0@"'.format(var_cflags),
  '-DVAL_CFLAGS_SL="@0@"'.format(var_cflags_sl),
  '-DVAL_LDFLAGS="@0@"'.format(var_ldflags),
  '-DVAL_LDFLAGS_EX="@0@"'.format(var_ldflags_ex),
  '-DVAL_LDFLAGS_SL="@0@"'.format(var_ldflags_sl),
  '-DVAL_LIBS="@0@"'.format(var_libs),
]

# Some files need to be built with different cflags. The different sets are
# defined here.
common_cflags = {
  'ryu': ryu_cflags,
  'config_info': config_info_cflags,
}
common_sources_cflags = {
  'ryu': ryu_sources,
  'config_info': config_info_sources
}


# A few files are currently only built for frontend, not server.
# logging.c is excluded from OBJS_FRONTEND_SHLIB (shared library) as
# a matter of policy, because it is not appropriate for general purpose
# libraries such as libpq to report errors directly.  fe_memutils.c is
# excluded because libpq must not exit() on allocation failure.

common_sources_frontend_shlib = common_sources
common_sources_frontend_shlib += files(
  'restricted_token.c',
  'sprompt.c',
)

common_sources_frontend_static = common_sources_frontend_shlib
common_sources_frontend_static += files(
  'fe_memutils.c',
  'logging.c',
)

# Build pgcommon once for backend, once for use in frontend binaries, and
# once for use in shared libraries
#
# XXX: in most environments we could probably link_whole pgcommon_shlib
# against pgcommon_static, instead of compiling twice.
#
# For the server build of pgcommon, depend on lwlocknames_h and because at
# least cryptohash_openssl.c, hmac_openssl.c depend on it.
# controldata_utils.c depends on wait_event_types_h. That's arguably a
# layering violation, but ...
pgcommon = {}
pgcommon_variants = {
  '_srv': internal_lib_args + {
    'sources': common_sources + [lwlocknames_h] + [wait_event_types_h],
    'dependencies': [backend_common_code],
  },
  '': default_lib_args + {
    'sources': common_sources_frontend_static,
    'dependencies': [frontend_common_code],
    # Files in libpgcommon.a should use/export the "xxx_private" versions
    # of pg_char_to_encoding() and friends.
    'c_args': ['-DUSE_PRIVATE_ENCODING_FUNCS'],
  },
  '_shlib': default_lib_args + {
    'pic': true,
    'sources': common_sources_frontend_shlib,
    'dependencies': [frontend_common_code],
  },
}

foreach name, opts : pgcommon_variants

  # Build internal static libraries for sets of files that need to be built
  # with different cflags
  cflag_libs = []
  foreach cflagname, sources : common_sources_cflags
    if sources.length() == 0
      continue
    endif
    c_args = opts.get('c_args', []) + common_cflags[cflagname]
    cflag_libs += static_library('libpgcommon@0@_@1@'.format(name, cflagname),
      c_pch: pch_c_h,
      include_directories: include_directories('.'),
      kwargs: opts + {
        'sources': sources,
        'c_args': c_args,
        'build_by_default': false,
        'install': false,
      },
    )
  endforeach

  lib = static_library('libpgcommon@0@'.format(name),
      link_with: cflag_libs,
      c_pch: pch_c_h,
      include_directories: include_directories('.'),
      kwargs: opts + {
        'dependencies': opts['dependencies'] + [ssl],
      }
    )
  pgcommon += {name: lib}
endforeach

common_srv = pgcommon['_srv']
common_shlib = pgcommon['_shlib']
common_static = pgcommon['']

subdir('unicode')
