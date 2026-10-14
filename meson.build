project(
  'poor_httpd',
  'c',
  version: '0.1.0',
  license: 'MIT',
  license_files: 'LICENSE',
  meson_version: '>= 1.4.0',
  default_options: ['c_std=c23', 'warning_level=3', 'default_library=static'],
)

# First, so a bundled poor_loop provides its bundled liburing and poor_base too
poor_loop = dependency('poor_loop')
liburing = dependency('liburing', version: '>=2.7', include_type: 'system')
poor_base = dependency('poor_base')

poor_httpd = executable(
  'poor_httpd',
  'src/conn.c',
  'src/http.c',
  'src/listener.c',
  'src/log.c',
  'src/main.c',
  'src/server.c',
  dependencies: [poor_loop, liburing, poor_base],
  install: true,
)

subdir('tests')
subdir('bench')
