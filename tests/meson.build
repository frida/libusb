if not get_option('tests') \
    .disable_auto_if(meson.is_subproject() or not meson.can_run_host_binaries()) \
    .allowed()
  subdir_done()
endif

stress = executable(
  'stress',
  'stress.c',
  'testlib.c',
  dependencies: libusb_dep,
  include_directories: config_inc,
)

test('libusb_test', stress, timeout: 240)

umockdev_dep = dependency('umockdev', required: false)
if umockdev_dep.found()
  utest = executable(
    'umockdev',
    'umockdev.c',
    dependencies: libusb_dep,
    include_directories: config_inc,
  )

  test('umockdev_test', utest, timeout: 60)
endif
