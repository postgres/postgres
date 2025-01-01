# Copyright (c) 2022-2025, PostgreSQL Global Development Group

# Note: the order of this list determines the order in which the catalog
# header files are assembled into postgres.bki.  BKI_BOOTSTRAP catalogs
# must appear first, and pg_statistic before pg_statistic_ext_data, and
# there are reputedly other, undocumented ordering dependencies.
catalog_headers = [
  'pg_proc.h',
  'pg_type.h',
  'pg_attribute.h',
  'pg_class.h',
  'pg_attrdef.h',
  'pg_constraint.h',
  'pg_inherits.h',
  'pg_index.h',
  'pg_operator.h',
  'pg_opfamily.h',
  'pg_opclass.h',
  'pg_am.h',
  'pg_amop.h',
  'pg_amproc.h',
  'pg_language.h',
  'pg_largeobject_metadata.h',
  'pg_largeobject.h',
  'pg_aggregate.h',
  'pg_statistic.h',
  'pg_statistic_ext.h',
  'pg_statistic_ext_data.h',
  'pg_rewrite.h',
  'pg_trigger.h',
  'pg_event_trigger.h',
  'pg_description.h',
  'pg_cast.h',
  'pg_enum.h',
  'pg_namespace.h',
  'pg_conversion.h',
  'pg_depend.h',
  'pg_database.h',
  'pg_db_role_setting.h',
  'pg_tablespace.h',
  'pg_authid.h',
  'pg_auth_members.h',
  'pg_shdepend.h',
  'pg_shdescription.h',
  'pg_ts_config.h',
  'pg_ts_config_map.h',
  'pg_ts_dict.h',
  'pg_ts_parser.h',
  'pg_ts_template.h',
  'pg_extension.h',
  'pg_foreign_data_wrapper.h',
  'pg_foreign_server.h',
  'pg_user_mapping.h',
  'pg_foreign_table.h',
  'pg_policy.h',
  'pg_replication_origin.h',
  'pg_default_acl.h',
  'pg_init_privs.h',
  'pg_seclabel.h',
  'pg_shseclabel.h',
  'pg_collation.h',
  'pg_parameter_acl.h',
  'pg_partitioned_table.h',
  'pg_range.h',
  'pg_transform.h',
  'pg_sequence.h',
  'pg_publication.h',
  'pg_publication_namespace.h',
  'pg_publication_rel.h',
  'pg_subscription.h',
  'pg_subscription_rel.h',
]

# The .dat files we need can just be listed alphabetically.
bki_data = [
  'pg_aggregate.dat',
  'pg_am.dat',
  'pg_amop.dat',
  'pg_amproc.dat',
  'pg_authid.dat',
  'pg_cast.dat',
  'pg_class.dat',
  'pg_collation.dat',
  'pg_conversion.dat',
  'pg_database.dat',
  'pg_language.dat',
  'pg_namespace.dat',
  'pg_opclass.dat',
  'pg_operator.dat',
  'pg_opfamily.dat',
  'pg_proc.dat',
  'pg_range.dat',
  'pg_tablespace.dat',
  'pg_ts_config.dat',
  'pg_ts_config_map.dat',
  'pg_ts_dict.dat',
  'pg_ts_parser.dat',
  'pg_ts_template.dat',
  'pg_type.dat',
]
bki_data_f = files(bki_data)


input = []
output_files = [
  'postgres.bki',
  'system_constraints.sql',
  'schemapg.h',
  'syscache_ids.h',
  'syscache_info.h',
  'system_fk_info.h',
]
output_install = [
  dir_data,
  dir_data,
  dir_include_server / 'catalog',
  dir_include_server / 'catalog',
  false,
  dir_include_server / 'catalog',
]

foreach h : catalog_headers
  fname = h.split('.h')[0] + '_d.h'
  input += files(h)
  output_files += fname
  output_install += dir_include_server / 'catalog'
endforeach

generated_catalog_headers = custom_target('generated_catalog_headers',
  output: output_files,
  install_dir: output_install,
  input: input,
  depend_files: bki_data_f + catalog_pm,
  build_by_default: true,
  install: true,
  command: [
    perl,
    files('../../backend/catalog/genbki.pl'),
    '--include-path=@SOURCE_ROOT@/src/include',
    '--set-version=' + pg_version_major.to_string(),
    '--output=@OUTDIR@', '@INPUT@'
  ],
)

generated_headers += generated_catalog_headers.to_list()

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/include/catalog': output_files + ['bki-stamp']}

# 'reformat-dat-files' is a convenience target for rewriting the
# catalog data files in our standard format.  This includes collapsing
# out any entries that are redundant with a BKI_DEFAULT annotation.
run_target('reformat-dat-files',
  command: [perl, files('reformat_dat_file.pl'), '--output', '@CURRENT_SOURCE_DIR@', bki_data_f],
)

# 'expand-dat-files' is a convenience target for expanding out all
# default values in the catalog data files.  This should be run before
# altering or removing any BKI_DEFAULT annotation.
run_target('expand-dat-files',
  command: [perl, files('reformat_dat_file.pl'), '--output', '@CURRENT_SOURCE_DIR@', bki_data_f, '--full-tuples'],
)
