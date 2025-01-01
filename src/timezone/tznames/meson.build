# Copyright (c) 2022-2025, PostgreSQL Global Development Group

tznames = files(
  'Africa.txt',
  'America.txt',
  'Antarctica.txt',
  'Asia.txt',
  'Atlantic.txt',
  'Australia.txt',
  'Etc.txt',
  'Europe.txt',
  'Indian.txt',
  'Pacific.txt',
)

tznames_sets = files(
  'Default',
  'Australia',
  'India')

install_data(tznames, tznames_sets,
  install_dir: dir_data / 'timezonesets',
)
