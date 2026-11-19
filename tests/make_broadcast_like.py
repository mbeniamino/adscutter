"""Generates a short recording shaped like a Canale 5 (Mediaset) broadcast.

1280x720, 50 fps, H.264 (I B B P, a cut point every 96 frames), Vorbis audio,
in Matroska, like the real recordings. A semi-transparent "5 HD" logo is
always shown; below it a semi-transparent "PRIMA" during the programme and
"MEDIASET" during the break. As on the real channel, a break goes: programme
fading to black (the sound fades but does not go silent), a 2 s channel
ident whose last 0.2 s are silent, the spots separated by 12 black and silent
frames, and the programme cutting straight back in.

Frame layout (50 fps):
    0- 499  programme (moving picture), fading to black over frames 475-499
  500- 599  channel ident (silent over 590-599)
  600- 899  spot 1
  900- 911  black, silent
  912-1211  spot 2
 1212-1223  black, silent
 1224-1823  programme (moving picture)

usage: make_broadcast_like.py FFMPEG OUTPUT.mkv
"""
import subprocess
import sys

ffmpeg, output = sys.argv[1], sys.argv[2]
size, rate = '1280x720', 50

# (video source, duration, audio tone Hz or 0 for silence, fade)
parts = [
    ('moving', 10.00, 440, 'out'),
    ('smptehdbars', 2.00, 1000, 'ident'),
    ('testsrc', 6.00, 660, None),
    ('black', 0.24, 0, None),
    ('mandelbrot', 6.00, 880, None),
    ('black', 0.24, 0, None),
    ('moving', 12.00, 440, None),
]

inputs, chains, concat = [], [], ''
for i, (source, duration, tone, fade) in enumerate(parts):
    if source == 'black':
        inputs += ['-f', 'lavfi', '-i', f'color=black:s={size}:r={rate}:d={duration}']
    elif source == 'mandelbrot':
        inputs += ['-f', 'lavfi', '-i', f'mandelbrot=s={size}:r={rate}']
    elif source == 'moving':
        # A programme picture: colours slowly cycling and a box moving across
        # (deterministic, unlike the 'gradients' source).
        inputs += ['-f', 'lavfi', '-i', f'color=c=0x305080:s={size}:r={rate}:d={duration},'
                                        f"hue=H=2*PI*(t+{i})/8,drawbox=x='mod(t*300+{i * 100}\\,1280)':"
                                        f'y=420:w=220:h=160:color=0xc08040:t=fill']
    else:
        inputs += ['-f', 'lavfi', '-i', f'{source}=s={size}:r={rate}:d={duration}']
    if tone:
        inputs += ['-f', 'lavfi', '-i', f'sine=frequency={tone}:sample_rate=48000:d={duration}']
    else:
        inputs += ['-f', 'lavfi', '-i', f'anullsrc=r=48000:cl=mono:d={duration}']
    v, a = 2 * i, 2 * i + 1
    vf = f'trim=duration={duration},setpts=PTS-STARTPTS,format=yuv420p'
    af = f'atrim=duration={duration},asetpts=PTS-STARTPTS,aformat=sample_rates=48000:channel_layouts=stereo'
    if fade == 'out':
        # The sound only dips (to about -47 dBFS), it does not go silent.
        vf += f',fade=t=out:st={duration - 0.5}:d=0.5'
        af += f",volume='if(gte(t,{duration - 0.5}),0.05,1)':eval=frame"
    elif fade == 'ident':
        af += f",volume='if(gte(t,{duration - 0.2}),0,1)':eval=frame"
    chains.append(f'[{v}:v]{vf}[v{i}];[{a}:a]{af}[a{i}]')
    concat += f'[v{i}][a{i}]'

programme = 'lt(t,10)+gte(t,24.48)'
# Semi-transparent with a faint outline, like real channel overlays (made to
# stay readable over bright pictures).
text = "fontsize={size}:fontcolor=white@0.45:borderw=1:bordercolor=black@0.45:x={x}:y={y}"
overlays = (
    f"drawtext=text='5 HD':{text.format(size=56, x='w-210', y='h-170')},"
    f"drawtext=text='PRIMA':{text.format(size=30, x='w-200', y='h-100')}:enable='{programme}',"
    f"drawtext=text='MEDIASET':{text.format(size=30, x='w-225', y='h-100')}:enable='not({programme})'"
)
graph = ';'.join(chains) + f';{concat}concat=n={len(parts)}:v=1:a=1[cv][ca];[cv]{overlays}[outv]'

subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', *inputs,
                '-filter_complex', graph, '-map', '[outv]', '-map', '[ca]',
                '-c:v', 'libx264', '-preset', 'veryfast', '-bf', '2', '-g', '96', '-keyint_min', '96',
                '-sc_threshold', '0', '-c:a', 'libvorbis', '-q:a', '3',
                '-metadata:s:a:0', 'language=ita', output], check=True)
