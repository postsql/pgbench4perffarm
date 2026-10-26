# Copyright (c) 2026, PostgreSQL Global Development Group

log2_hist_sources = files(
  'log2_hist.c',
)

if host_system == 'windows'
  log2_hist_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'log2_hist',
    '--FILEDESC', 'log2_hist - 64-bucket RLE-compressed log2(V+1) histogram data type',])
endif

log2_hist_ext = shared_module('log2_hist',
  log2_hist_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += log2_hist_ext

install_data(
  'log2_hist.control',
  'log2_hist--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'log2_hist',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'log2_hist',
    ],
  },
}
