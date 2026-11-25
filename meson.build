project('adscutter', 'cpp',
  version: '0.1.1',
  license: 'MIT',
  license_files: 'LICENSE',
  meson_version: '>= 1.3.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'buildtype=debugoptimized',
  ],
)

qt6 = import('qt6')
qt_core_gui = dependency('qt6', modules: ['Core', 'Gui', 'Multimedia'])
qt_widgets = dependency('qt6', modules: ['Core', 'Gui', 'Widgets'])

av_deps = [
  dependency('libavformat', version: '>= 61'),
  dependency('libavcodec', version: '>= 61'),
  dependency('libavutil', version: '>= 59'),
  dependency('libswscale', version: '>= 8'),
  dependency('libswresample', version: '>= 5'),
]

inc = include_directories('src')

# Media layer: no widgets, so it can be used by tests and future CLI tools.
media_lib = static_library('adscutter-media',
  files(
    'src/analysis/AnalysisData.cpp',
    'src/analysis/AnalysisJob.cpp',
    'src/analysis/Analyzer.cpp',
    'src/analysis/Proposal.cpp',
    'src/analysis/RegionDetector.cpp',
    'src/analysis/RegionJob.cpp',
    'src/media/AudioDecoder.cpp',
    'src/media/AudioOutput.cpp',
    'src/media/Bitstream.cpp',
    'src/media/Deinterlace.cpp',
    'src/media/ExportJob.cpp',
    'src/media/Exporter.cpp',
    'src/media/FrameConverter.cpp',
    'src/media/FrameDecoder.cpp',
    'src/media/FrameServer.cpp',
    'src/media/H264Splice.cpp',
    'src/media/HwDecode.cpp',
    'src/media/IndexBuilder.cpp',
    'src/media/IndexJob.cpp',
    'src/media/Latm.cpp',
    'src/media/MediaFile.cpp',
    'src/media/PacketIndex.cpp',
    'src/media/RandomAccessDecoder.cpp',
    'src/model/CutList.cpp',
  ),
  qt6.compile_moc(
    headers: files('src/analysis/AnalysisJob.h', 'src/analysis/RegionJob.h', 'src/media/AudioOutput.h', 'src/media/ExportJob.h', 'src/media/FrameServer.h', 'src/media/IndexJob.h'),
    include_directories: inc,
    dependencies: qt_core_gui,
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
    'src/ui/NavigationBar.h',
    'src/ui/Playback.h',
    'src/ui/RegionPanel.h',
    'src/ui/SegmentPanel.h',
    'src/ui/Timeline.h',
    'src/ui/VideoView.h',
    'src/ui/StreamPanel.h',
  ),
  include_directories: inc,
  dependencies: qt_widgets,
)

ui_lib = static_library('adscutter-ui',
  files(
    'src/ui/MainWindow.cpp',
    'src/ui/NavigationBar.cpp',
    'src/ui/Playback.cpp',
    'src/ui/RegionPanel.cpp',
    'src/ui/SegmentPanel.cpp',
    'src/ui/Timeline.cpp',
    'src/ui/VideoView.cpp',
    'src/ui/StreamPanel.cpp',
  ),
  ui_moc,
  include_directories: inc,
  dependencies: [media_dep, qt_widgets],
)
ui_dep = declare_dependency(
  link_with: ui_lib,
  include_directories: inc,
  dependencies: [media_dep, qt_widgets],
)

executable('adscutter',
  files('src/main.cpp'),
  # Single source of truth for the version shown in --version and About.
  cpp_args: '-DADSCUTTER_VERSION="@0@"'.format(meson.project_version()),
  dependencies: ui_dep,
  install: true,
)

subdir('tests')

install_data('data/adscutter.desktop',
  install_dir: get_option('datadir') / 'applications')
install_man(configure_file(
  input: 'data/adscutter.1.in',
  output: 'adscutter.1',
  configuration: {'VERSION': meson.project_version()},
))
install_data('data/icons/adscutter.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

metainfo = files('data/io.github.mbeniamino.adscutter.metainfo.xml')
install_data(metainfo, install_dir: get_option('datadir') / 'metainfo')

desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('desktop file', desktop_file_validate, args: files('data/adscutter.desktop'))
endif
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('metainfo', appstreamcli, args: ['validate', '--no-net', metainfo])
endif
# Catches a release whose version was bumped without a metainfo entry.
test('metainfo version', find_program('python3'),
  args: ['-c', '''import sys, xml.etree.ElementTree as ET
r = ET.parse(sys.argv[1]).find('releases/release')
sys.exit(0 if r is not None and r.get('version') == sys.argv[2] else 'newest metainfo release is not ' + sys.argv[2])''',
    metainfo, meson.project_version()])
