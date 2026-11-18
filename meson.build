project('adscutter', 'cpp',
  version: '0.1.0',
  meson_version: '>= 1.3.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'buildtype=debugoptimized',
  ],
)

qt6 = import('qt6')
qt_core_gui = dependency('qt6', modules: ['Core', 'Gui'])
qt_widgets = dependency('qt6', modules: ['Core', 'Gui', 'Widgets'])

av_deps = [
  dependency('libavformat', version: '>= 61'),
  dependency('libavcodec', version: '>= 61'),
  dependency('libavutil', version: '>= 59'),
  dependency('libswscale', version: '>= 8'),
]

inc = include_directories('src')

# Media layer: no widgets, so it can be used by tests and future CLI tools.
media_lib = static_library('adscutter-media',
  files(
    'src/media/MediaFile.cpp',
    'src/media/FrameDecoder.cpp',
  ),
  include_directories: inc,
  dependencies: [qt_core_gui] + av_deps,
)
media_dep = declare_dependency(
  link_with: media_lib,
  include_directories: inc,
  dependencies: [qt_core_gui] + av_deps,
)

ui_moc = qt6.compile_moc(
  headers: files(
    'src/ui/MainWindow.h',
    'src/ui/VideoView.h',
    'src/ui/StreamPanel.h',
  ),
  include_directories: inc,
  dependencies: qt_widgets,
)

executable('adscutter',
  files(
    'src/main.cpp',
    'src/ui/MainWindow.cpp',
    'src/ui/VideoView.cpp',
    'src/ui/StreamPanel.cpp',
  ),
  ui_moc,
  include_directories: inc,
  dependencies: [media_dep, qt_widgets],
  install: true,
)

subdir('tests')

install_data('data/adscutter.desktop',
  install_dir: get_option('datadir') / 'applications')
