# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'clog.c',
  'commit_ts.c',
  'generic_xlog.c',
  'multixact.c',
  'parallel.c',
  'rmgr.c',
  'slru.c',
  'subtrans.c',
  'timeline.c',
  'transam.c',
  'twophase.c',
  'twophase_rmgr.c',
  'varsup.c',
  'xact.c',
  'xlog.c',
  'xlogarchive.c',
  'xlogbackup.c',
  'xlogfuncs.c',
  'xloginsert.c',
  'xlogprefetcher.c',
  'xlogrecovery.c',
  'xlogstats.c',
  'xlogutils.c',
)

# used by frontend programs to build a frontend xlogreader
xlogreader_sources = files(
  'xlogreader.c',
)

backend_sources += xlogreader_sources
