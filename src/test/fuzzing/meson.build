# Copyright (c) 2026, PostgreSQL Global Development Group

# Fuzzing harnesses for security testing.
#
# Build with:
#   meson setup build -Dfuzzing=true
#
# For libFuzzer (recommended), also pass sanitizer flags:
#   meson setup build -Dfuzzing=true \
#     -Dc_args='-fsanitize=fuzzer-no-link' \
#     -Dc_link_args='-fsanitize=fuzzer'
#
# Without a fuzzer engine the harnesses are built in standalone mode:
# each reads input from files named on the command line.

if not get_option('fuzzing')
  subdir_done()
endif

# Detect whether a fuzzer engine (e.g. libFuzzer) is available.
# If so, link fuzzer executables with -fsanitize=fuzzer so that the
# engine provides main().  Otherwise compile with STANDALONE_FUZZ_TARGET
# so the harnesses supply their own main() that reads from files.

fuzz_c_args = []
fuzz_link_args = []

if cc.has_argument('-fsanitize=fuzzer-no-link')
  fuzzer_has_engine = cc.links('''
    #include <stdint.h>
    #include <stddef.h>
    int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
    { return 0; }
  ''',
    args: ['-fsanitize=fuzzer'],
    name: 'libFuzzer support')
else
  fuzzer_has_engine = false
endif

if fuzzer_has_engine
  fuzz_link_args += ['-fsanitize=fuzzer']
else
  fuzz_c_args += ['-DSTANDALONE_FUZZ_TARGET']
endif

# --- Frontend targets (no backend dependencies) ---

fuzz_json = executable('fuzz_json',
  'fuzz_json.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_json_incremental = executable('fuzz_json_incremental',
  'fuzz_json_incremental.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_conninfo = executable('fuzz_conninfo',
  'fuzz_conninfo.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_pglz = executable('fuzz_pglz',
  'fuzz_pglz.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_unescapebytea = executable('fuzz_unescapebytea',
  'fuzz_unescapebytea.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_b64decode = executable('fuzz_b64decode',
  'fuzz_b64decode.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_saslprep = executable('fuzz_saslprep',
  'fuzz_saslprep.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_parsepgarray = executable('fuzz_parsepgarray',
  'fuzz_parsepgarray.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  },
)

# The pgbench expression parser is built from generated Bison/Flex
# sources.  We reference the same custom_target outputs used by the
# pgbench build, and provide our own syntax_error() / strtoint64() /
# strtodouble() so we don't pull in pgbench.c (which has its own
# main() and calls exit() on parse errors).

exprscan_fuzz = custom_target('exprscan_fuzz',
  input: files('../../bin/pgbench/exprscan.l'),
  output: 'exprscan.c',
  command: flex_cmd,
)

exprparse_fuzz = custom_target('exprparse_fuzz',
  input: files('../../bin/pgbench/exprparse.y'),
  kwargs: bison_kw,
)

fuzz_pgbench_expr = executable('fuzz_pgbench_expr',
  'fuzz_pgbench_expr.c',
  exprscan_fuzz,
  exprparse_fuzz,
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  include_directories: include_directories('../../bin/pgbench'),
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  },
)

# --- Backend targets ---
#
# These link against postgres_lib using standard archive semantics
# (link_with), so only objects needed to resolve symbols are pulled in.
# The harness provides stub definitions for symbols exported by
# main/main.c, preventing the archive linker from pulling in main.o
# (which would conflict with the harness's own main()).
#
# Backend code uses function-pointer casts in hash tables (dynahash.c)
# that trigger UBSan's -fsanitize=function check.  This is a known
# benign pattern; when using -fsanitize=undefined, also pass
# -fno-sanitize=function in the top-level c_args to suppress it.

fuzz_rawparser = executable('fuzz_rawparser',
  'fuzz_rawparser.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  link_with: [postgres_lib],
  dependencies: backend_build_deps,
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_regex = executable('fuzz_regex',
  'fuzz_regex.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  link_with: [postgres_lib],
  dependencies: backend_build_deps,
  kwargs: default_bin_args + {
    'install': false,
  },
)

fuzz_typeinput = executable('fuzz_typeinput',
  'fuzz_typeinput.c',
  c_args: fuzz_c_args,
  link_args: fuzz_link_args,
  link_with: [postgres_lib],
  dependencies: backend_build_deps,
  kwargs: default_bin_args + {
    'install': false,
  },
)
