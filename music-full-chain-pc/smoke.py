"""Generated-fixture complete-graph smoke check, not a light-quality pass gate.

Works on Windows or Linux without numpy. Run the strict independent null and
8x true-peak qualification separately before drawing any quality conclusion.
"""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys


def graph(light=False, neutral=False):
    """Mirrors MusicEqualizer.liveChain's full/default-wideband test topology.

    It deliberately includes the actual lumendsp filter and native lumenout;
    substituting Python stereo arithmetic or an earlier core is not equivalent.
    """
    quality, sample_format, precision, volume = (
        (20, 'fltp', 'f32', 'float') if light else
        (28, 'dblp', 'f64', 'double'))
    gains = [0] * 10 if neutral else [0, 4.5, 2.5, -2, -1, -.5, 1, 1.5, .5, 0]
    frequencies = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    return ','.join([
        f'aresample=48000:resampler=soxr:precision={quality}:cutoff=0.97:osf={sample_format}',
        f'volume@pre=volume=0dB:precision={volume}',
        *[f'equalizer@eq{i}=f={frequency}:width_type=o:width=1:g={gain}:precision={precision}'
          for i, (frequency, gain) in enumerate(zip(frequencies, gains))],
        f'bass@tbass=f=90:width_type=q:width=.8:g={0 if neutral else 3}:precision={precision}',
        f'treble@ttreble=f=10000:width_type=q:width=.6:g={0 if neutral else 2}:precision={precision}',
        f'lumendsp@dsp=enabled=1:width={1 if neutral else 1.26}:mono=0:limiter=0:ceiling=.8910',
        'lumenout@out=volume=0:ceiling=-1:lookahead=3:release=500:knee=1:'
        f'bands=1:clip=0:protect={0 if neutral else 1}:mute=0',
    ])


def read_pcm(path):
    values = array.array('d')
    raw = path.read_bytes()
    if len(raw) % 16:
        raise ValueError('Incomplete stereo f64 frame')
    values.frombytes(raw)
    if sys.byteorder != 'little':
        values.byteswap()
    return values


def write_pcm(path, values):
    pcm = array.array('d', values)
    if sys.byteorder != 'little':
        pcm.byteswap()
    path.write_bytes(pcm.tobytes())


def run(ffmpeg, arguments):
    result = subprocess.run([str(ffmpeg), '-hide_banner', *arguments],
                            capture_output=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    ffmpeg, out = args.ffmpeg.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    required = ['aresample', 'equalizer', 'bass', 'treble', 'lumendsp', 'lumenout']
    for name in required:
        result = run(ffmpeg, ['-h', f'filter={name}'])
        text = result.stdout + result.stderr
        if f'Filter {name}\n'.encode() not in text.replace(b'\r\n', b'\n'):
            raise RuntimeError(f'Missing filter {name}')
        (out / f'{name}-options.txt').write_bytes(text)

    frames = 24000
    impulse = [0.] * (frames * 2)
    impulse[2000], impulse[2001] = .001, -.0007
    hot = [value for i in range(frames) for value in (
        .95 * math.sin(2 * math.pi * 62 * i / 48000),
        .83 * math.sin(2 * math.pi * 62 * i / 48000 + .37))]
    rows, outputs = [], {}
    for name, light, neutral, samples in [
            ('neutral_impulse', False, True, impulse),
            ('full_hot', False, False, hot),
            ('light_hot', True, False, hot)]:
        source, destination = out / f'{name}.input.f64', out / f'{name}.output.f64'
        write_pcm(source, samples)
        filter_graph = graph(light, neutral)
        result = run(ffmpeg, ['-v', 'verbose', '-f', 'f64le', '-ar', '48000', '-ac', '2',
            '-i', str(source), '-filter_threads', '1', '-af', filter_graph,
            '-c:a', 'pcm_f64le', '-f', 'f64le', str(destination)])
        (out / f'{name}.log').write_bytes(result.stderr)
        values = read_pcm(destination)
        same_length = len(values) == len(samples)
        finite = all(math.isfinite(value) for value in values)
        maximum = max(map(abs, values), default=0.)
        error = max((abs(a-b) for a, b in zip(values, samples)), default=0.)
        row = {'case': name, 'graph': filter_graph, 'frames': len(values) // 2,
               'same_length': same_length, 'finite': finite, 'sample_peak': maximum,
               'output_sha256': hashlib.sha256(destination.read_bytes()).hexdigest(),
               'neutral_peak_error': error if neutral else None,
               'pass': same_length and finite and (error <= 1e-12 if neutral else
                                                    0 < maximum <= 10 ** (-.95/20))}
        rows.append(row)
        outputs[name] = values
    delta = max(abs(a-b) for a, b in zip(outputs['full_hot'], outputs['light_hot']))
    report = {'schema_version': 1, 'scope': 'Complete native graph synthetic smoke only',
              'ffmpeg_sha256': hashlib.sha256(ffmpeg.read_bytes()).hexdigest(),
              'cases': rows, 'full_light_hot_peak_null_dbfs': 20 * math.log10(max(delta, 1e-30)),
              'full_chain_quality_qualified': False,
              'production_light_enablement_authorized_by_result': False,
              'excluded': ['independent 8x true peak', 'strict broad null matrix',
                           'device CPU/battery/underruns', 'owner listening'],
              'pass': all(row['pass'] for row in rows)}
    (out / 'smoke-checks.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
