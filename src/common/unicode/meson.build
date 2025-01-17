# Copyright (c) 2022-2025, PostgreSQL Global Development Group

UNICODE_VERSION = '15.1.0'

unicode_data = {}
unicode_baseurl = 'https://www.unicode.org/Public/@0@/ucd/@1@'

if not wget.found() or not cp.found()
  subdir_done()
endif

# These files are part of the Unicode Character Database. Download them on
# demand.
foreach f : ['CompositionExclusions.txt', 'DerivedCoreProperties.txt', 'DerivedNormalizationProps.txt', 'EastAsianWidth.txt', 'NormalizationTest.txt', 'PropList.txt', 'SpecialCasing.txt', 'UnicodeData.txt']
  url = unicode_baseurl.format(UNICODE_VERSION, f)
  target = custom_target(f,
    output: f,
    command: [wget, wget_flags, url],
    build_by_default: false,
  )
  unicode_data += {f: target}
endforeach


update_unicode_targets = []

update_unicode_targets += \
  custom_target('unicode_case_table.h',
    input: [unicode_data['SpecialCasing.txt'], unicode_data['UnicodeData.txt']],
    output: ['unicode_case_table.h'],
    command: [
      perl, files('generate-unicode_case_table.pl'),
      '--outdir', '@OUTDIR@', '@INPUT@'],
    build_by_default: false,
  )

update_unicode_targets += \
  custom_target('unicode_category_table.h',
    input: [unicode_data['UnicodeData.txt'], unicode_data['DerivedCoreProperties.txt'], unicode_data['PropList.txt']],
    output: ['unicode_category_table.h'],
    command: [
      perl, files('generate-unicode_category_table.pl'),
      '--outdir', '@OUTDIR@', '@INPUT@'],
    build_by_default: false,
  )

update_unicode_targets += \
  custom_target('unicode_east_asian_fw_table.h',
    input: [unicode_data['EastAsianWidth.txt']],
    output: ['unicode_east_asian_fw_table.h'],
    command: [perl, files('generate-unicode_east_asian_fw_table.pl'), '@INPUT@'],
    build_by_default: false,
    capture: true,
  )

update_unicode_targets += \
  custom_target('unicode_nonspacing_table.h',
    input: [unicode_data['UnicodeData.txt']],
    output: ['unicode_nonspacing_table.h'],
    depend_files: perfect_hash_pm,
    command: [perl, files('generate-unicode_nonspacing_table.pl'), '@INPUT@'],
    build_by_default: false,
    capture: true,
  )

update_unicode_targets += \
  custom_target('unicode_norm_table.h',
    input: [unicode_data['UnicodeData.txt'], unicode_data['CompositionExclusions.txt']],
    output: ['unicode_norm_table.h', 'unicode_norm_hashfunc.h'],
    depend_files: perfect_hash_pm,
    command: [
      perl, files('generate-unicode_norm_table.pl'),
      '--outdir', '@OUTDIR@', '@INPUT@'],
    build_by_default: false,
  )

update_unicode_targets += \
  custom_target('unicode_normprops_table.h',
    input: [unicode_data['DerivedNormalizationProps.txt']],
    output: ['unicode_normprops_table.h'],
    depend_files: perfect_hash_pm,
    command: [perl, files('generate-unicode_normprops_table.pl'), '@INPUT@'],
    build_by_default: false,
    capture: true,
  )

update_unicode_targets += \
  custom_target('unicode_version.h',
    output: ['unicode_version.h'],
    command: [
      perl, files('generate-unicode_version.pl'),
      '--outdir', '@OUTDIR@', '--version', UNICODE_VERSION],
    build_by_default: false,
  )

norm_test_table = custom_target('norm_test_table.h',
    input: [unicode_data['NormalizationTest.txt']],
    output: ['norm_test_table.h'],
    command: [perl, files('generate-norm_test_table.pl'), '@INPUT@', '@OUTPUT@'],
    build_by_default: false,
  )

inc = include_directories('.')

case_test = executable('case_test',
  ['case_test.c'],
  dependencies: [frontend_port_code, icu, libintl],
  include_directories: inc,
  link_with: [common_static, pgport_static],
  build_by_default: false,
  kwargs: default_bin_args + {
    'install': false,
  }
)

category_test = executable('category_test',
  ['category_test.c'],
  dependencies: [frontend_port_code, icu, libintl],
  include_directories: inc,
  link_with: [common_static, pgport_static],
  build_by_default: false,
  kwargs: default_bin_args + {
    'install': false,
  }
)

norm_test = executable('norm_test',
  ['norm_test.c', norm_test_table],
  dependencies: [frontend_port_code, libintl],
  include_directories: inc,
  link_with: [common_static, pgport_static],
  build_by_default: false,
  kwargs: default_bin_args + {
    'install': false,
  }
)

update_unicode_dep = []

if not meson.is_cross_build()
  update_unicode_dep += custom_target('case_test.run',
    output: 'case_test.run',
    input: update_unicode_targets,
    command: [case_test, UNICODE_VERSION],
    build_by_default: false,
    build_always_stale: true,
  )
endif

if not meson.is_cross_build()
  update_unicode_dep += custom_target('category_test.run',
    output: 'category_test.run',
    input: update_unicode_targets,
    command: [category_test, UNICODE_VERSION],
    build_by_default: false,
    build_always_stale: true,
  )
endif

if not meson.is_cross_build()
  update_unicode_dep += custom_target('norm_test.run',
    output: 'norm_test.run',
    input: update_unicode_targets,
    command: [norm_test],
    build_by_default: false,
    build_always_stale: true,
  )
endif


# Use a custom target, as run targets serialize the output, making this harder
# to debug, and don't deal well with targets with multiple outputs.
update_unicode = custom_target('update-unicode',
  depends: update_unicode_dep,
  output: ['dont-exist'],
  input: update_unicode_targets,
  command: [cp, '@INPUT@', '@SOURCE_ROOT@/src/include/common/'],
  build_by_default: false,
  build_always_stale: true,
)

alias_target('update-unicode', update_unicode)
