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
  test('probe ' + f[0], probe_test,
    args: [fixture, f[5], f[6], f[7]] + f[8],
    depends: fixture,
    timeout: 60,
  )
endforeach
