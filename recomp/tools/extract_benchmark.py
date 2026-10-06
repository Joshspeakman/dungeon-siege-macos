#!/usr/bin/env python3
"""extract_benchmark.py <DSBenchmark.EXE> <outdir>

Gas Powered Games' "Dungeon Siege Benchmark" (2001) is a Wise installer that adds two archives to a Dungeon Siege
installation: Resources/Benchmark.dsres (the benchmark's commands) and Maps/BenchmarkMap.dsmap (the benchmark_demo map,
a self-playing rolling demo). This takes them out of the installer without running it: the installer's payload is a run
of raw-deflate streams, two of which are those Tank archives. They are written to <outdir>/Resources and <outdir>/Maps.
"""
import hashlib, os, sys, zlib

SHA = '3c73a145bd7c77059495d75c2d85c3bdf5ddfc6e9b5738b4433fffcadedbbeb7'   # DSBenchmark.EXE, 5,319,358 bytes
KNOWN = [(630240, 'Maps/BenchmarkMap.dsmap', b'benchmark_demo'),
         (5260356, 'Resources/Benchmark.dsres', b'gpg_benchmark')]

def inflate(d, off):
    try:
        z = zlib.decompressobj(-15); out = z.decompress(d[off:]) + z.flush()
        return out if z.eof else None
    except zlib.error:
        return None

def tank_has(data, marker):
    return data[:8] == b'DSigTank' and marker in data

def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    exe, out = sys.argv[1], sys.argv[2]
    d = open(exe, 'rb').read()
    h = hashlib.sha256(d).hexdigest()
    if h != SHA: sys.exit(f'{exe}: not the Dungeon Siege Benchmark installer this knows (sha256 {h})')
    for off, rel, marker in KNOWN:
        data = inflate(d, off)
        if not data or not tank_has(data, marker): sys.exit(f'{exe}: no {rel} at {off:#x}')
        path = os.path.join(out, rel); os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(data)
        print(f'{rel}: {len(data)} bytes')

if __name__ == '__main__':
    main()
