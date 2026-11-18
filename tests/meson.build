# Synthetic recordings resembling DVB-T/T2 broadcasts, generated with the
# ffmpeg CLI at build time. The CLI is only needed for tests.
ffmpeg = find_program('ffmpeg', required: false)
if not ffmpeg.found()
  warning('ffmpeg CLI not found: tests disabled')
  subdir_done()
endif

probe_test = executable('probe_test', 'probe_test.cpp',
  dependencies: media_dep,
)
index_test = executable('index_test', 'index_test.cpp',
  dependencies: media_dep,
)

common_in = [
  '-hide_banner', '-loglevel', 'error', '-y',
  '-f', 'lavfi', '-i', 'testsrc2=size=@SIZE@:rate=25,format=yuv420p',
  '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
  '-f', 'lavfi', '-i', 'sine=frequency=880:sample_rate=48000',
  '-t', '6',
  '-map', '0:v', '-map', '1:a', '-map', '2:a',
  '-metadata:s:a:0', 'language=ita', '-metadata:s:a:1', 'language=eng',
]

# name, size, video args, audio args, output args, expected w, h, aspect, extra test args
fixtures = [
  # SD MPEG-2, anamorphic 16:9, interlaced (encoder writes BFF), MP2 + AC-3 (classic DVB-T)
  ['mpeg2_sd.ts', '720x576',
   ['-c:v', 'mpeg2video', '-b:v', '4M', '-g', '12', '-bf', '2',
    '-flags', '+ildct+ilme', '-aspect', '16:9'],
   ['-c:a:0', 'mp2', '-b:a:0', '192k', '-c:a:1', 'ac3', '-b:a:1', '192k'],
   ['-f', 'mpegts'],
   '720', '576', '1.7778', []],
  # HD H.264 with B-frames and open GOP, MP2 + E-AC-3 (DVB-T HD)
  ['h264_hd.ts', '1920x1080',
   ['-c:v', 'libx264', '-preset', 'ultrafast', '-bf', '3', '-g', '25',
    '-x264-params', 'open-gop=1:repeat-headers=1'],
   ['-c:a:0', 'mp2', '-b:a:0', '192k', '-c:a:1', 'eac3', '-b:a:1', '192k'],
   ['-f', 'mpegts'],
   '1920', '1080', '1.7778', []],
  # HEVC with HE-AAC-style LATM audio (DVB-T2)
  ['hevc_latm.ts', '1280x720',
   ['-c:v', 'libx265', '-preset', 'ultrafast', '-x265-params', 'log-level=error:keyint=25:bframes=3'],
   ['-c:a', 'aac', '-b:a', '128k'],
   ['-f', 'mpegts', '-mpegts_flags', 'latm'],
   '1280', '720', '1.7778', ['--expect-unexportable', 'aac_latm']],
  # Matroska input (e.g. a previous AdsCutter export)
  ['h264_aac.mkv', '1024x576',
   ['-c:v', 'libx264', '-preset', 'ultrafast', '-bf', '2', '-g', '50'],
   ['-c:a', 'aac', '-b:a', '128k'],
   ['-f', 'matroska'],
   '1024', '576', '1.7778', []],
]

# H.264 + MP2 in MPEG-TS, the base for the timestamp tests below.
h264_sd = [
  ['-c:v', 'libx264', '-preset', 'ultrafast', '-bf', '2', '-g', '25',
   '-x264-params', 'open-gop=1:repeat-headers=1', '-aspect', '16:9'],
  ['-c:a', 'mp2', '-b:a', '128k'],
]

# 6 s files starting about 2.3 s before the 33-bit PTS clock wraps
# (2^33 / 90 kHz = 95443.7 s; the muxer adds 1.4 s of delay).
fixtures += [
  ['wrap.ts', '720x576', h264_sd[0], h264_sd[1],
   ['-f', 'mpegts', '-output_ts_offset', '95440'],
   '720', '576', '1.7778', []],
  # Halves of the stream-restart recordings below.
  ['part_low.ts', '720x576', h264_sd[0], h264_sd[1],
   ['-f', 'mpegts'],
   '720', '576', '1.7778', []],
  ['part_high.ts', '720x576', h264_sd[0], h264_sd[1],
   ['-f', 'mpegts', '-output_ts_offset', '5000'],
   '720', '576', '1.7778', []],
]

fixture_targets = {}
foreach f : fixtures
  input_args = []
  foreach a : common_in
    input_args += a.replace('@SIZE@', f[1])
  endforeach
  fixture = custom_target(f[0],
    output: f[0],
    command: [ffmpeg] + input_args + f[2] + f[3] + f[4] + ['@OUTPUT@'],
    build_by_default: false,
  )
  fixture_targets += {f[0]: fixture}
  test('probe ' + f[0], probe_test,
    args: [fixture, f[5], f[6], f[7]] + f[8],
    depends: fixture,
    timeout: 60,
  )
endforeach

# A channel restarting its encoder mid-recording: the clock jumps forward
# or backward by more than an hour.
cat = find_program('cat')
fixture_targets += {
  'jump_forward.ts': custom_target('jump_forward.ts',
    input: [fixture_targets['part_low.ts'], fixture_targets['part_high.ts']],
    output: 'jump_forward.ts',
    command: [cat, '@INPUT@'],
    capture: true,
    build_by_default: false,
  ),
  'jump_backward.ts': custom_target('jump_backward.ts',
    input: [fixture_targets['part_high.ts'], fixture_targets['part_low.ts']],
    output: 'jump_backward.ts',
    command: [cat, '@INPUT@'],
    capture: true,
    build_by_default: false,
  ),
}

# file, frames, duration (s), discontinuities, extra args
index_cases = [
  ['mpeg2_sd.ts', '150', '6.0', '0', []],
  ['h264_hd.ts', '150', '6.0', '0', []],
  ['hevc_latm.ts', '150', '6.0', '0', []],
  ['h264_aac.mkv', '150', '6.0', '0', []],
  ['wrap.ts', '150', '6.0', '0', []],
  # libavformat normally unwraps the clock itself; make sure the index
  # copes when it does not.
  ['wrap.ts', '150', '6.0', '0', ['--raw-timestamps']],
  ['jump_forward.ts', '300', '12.0', '1', []],
  ['jump_backward.ts', '300', '12.0', '1', []],
]
foreach c : index_cases
  fixture = fixture_targets[c[0]]
  test('index ' + c[0] + (c[4].length() > 0 ? ' ' + ' '.join(c[4]) : ''), index_test,
    args: [fixture, c[1], c[2], c[3]] + c[4],
    depends: fixture,
    timeout: 120,
  )
endforeach

job_test = executable('job_test', 'job_test.cpp',
  dependencies: media_dep,
)
test('index job + cache', job_test,
  args: [fixture_targets['h264_hd.ts']],
  depends: fixture_targets['h264_hd.ts'],
  timeout: 60,
)

seek_test = executable('seek_test', 'seek_test.cpp',
  dependencies: media_dep,
)
foreach f : ['mpeg2_sd.ts', 'h264_hd.ts', 'hevc_latm.ts', 'h264_aac.mkv', 'wrap.ts',
             'jump_forward.ts', 'jump_backward.ts']
  test('seek ' + f, seek_test,
    args: [fixture_targets[f]],
    depends: fixture_targets[f],
    timeout: 180,
  )
endforeach

qt_test = dependency('qt6', modules: ['Test'], required: false)
if qt_test.found()
  gui_test = executable('gui_test', 'gui_test.cpp',
    dependencies: [ui_dep, qt_test],
  )
  gui_env = environment()
  gui_env.set('QT_QPA_PLATFORM', 'offscreen')
  # Keep test indexes out of the user's cache.
  gui_env.set('XDG_CACHE_HOME', meson.current_build_dir() / 'cache')
  test('gui navigation', gui_test,
    args: [fixture_targets['mpeg2_sd.ts']],
    depends: fixture_targets['mpeg2_sd.ts'],
    env: gui_env,
    timeout: 60,
  )
endif
