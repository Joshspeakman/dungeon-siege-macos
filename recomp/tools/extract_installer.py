#!/usr/bin/env python3
"""extract_installer.py <installer.exe> <outdir>

Takes the Dungeon Siege archives out of two free add-ons' Wise installers, without running them. A Wise installer's
payload is a run of raw-deflate streams; for each installer this knows (by its SHA-256), the streams that are the
add-on's archives are inflated and checked against their own SHA-256:

  DSBenchmark.EXE           Gas Powered Games' Dungeon Siege Benchmark (2001): Resources/Benchmark.dsres and
                            Maps/BenchmarkMap.dsmap (written under <outdir>/Resources and <outdir>/Maps)
  DungeonSiegeYesterhaven.exe  Gas Powered Games' Yesterhaven multiplayer adventure: Yesterhaven.dsres and
                            Yesterhaven.dsmap (written to <outdir>); the copy the Internet Archive keeps
"""
import hashlib, os, sys, zlib

KNOWN = {
    '3c73a145bd7c77059495d75c2d85c3bdf5ddfc6e9b5738b4433fffcadedbbeb7': ('the Dungeon Siege Benchmark', [
        (630240, 'Maps/BenchmarkMap.dsmap', None),
        (5260356, 'Resources/Benchmark.dsres', None)]),
    '4dfd9941148abda2c14a814192e738505d6e888570c7ade14e4c966fc01a7264': ('Yesterhaven', [
        (375177, 'Yesterhaven.dsres', 'f6e520c38d09177f'),
        (32838726, 'Yesterhaven.dsmap', '70f5574bf4188631')]),
}
CONTENT = {'Maps/BenchmarkMap.dsmap': b'benchmark_demo', 'Resources/Benchmark.dsres': b'gpg_benchmark'}

def inflate(d, off):
    try:
        z = zlib.decompressobj(-15); out = z.decompress(d[off:]) + z.flush()
        return out if z.eof else None
    except zlib.error:
        return None

def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    exe, out = sys.argv[1], sys.argv[2]
    d = open(exe, 'rb').read(); h = hashlib.sha256(d).hexdigest()
    if h not in KNOWN: sys.exit(f'{exe}: not an installer this knows (sha256 {h})')
    name, files = KNOWN[h]
    for off, rel, sha in files:
        data = inflate(d, off)
        ok = data is not None and data[:8] == b'DSigTank'
        if ok and sha: ok = hashlib.sha256(data).hexdigest().startswith(sha)
        if ok and rel in CONTENT: ok = CONTENT[rel] in data
        if not ok: sys.exit(f'{exe}: {rel} is not where it should be')
        path = os.path.join(out, rel); os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
        open(path, 'wb').write(data)
        print(f'{name}: {rel}, {len(data)} bytes')

if __name__ == '__main__':
    main()
