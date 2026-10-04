#!/usr/bin/env python3
"""Small byte patches applied to the copy of DungeonSiege.exe that the native build recompiles (recomp/build.sh). Only
the user's own copy is touched, after checking its exact original bytes; anything unrecognised is refused.

  patch_exe.py <DungeonSiege.exe> [--revert] [--status]      GOG Dungeon Siege 1.11.1 only (SHA-256 checked)
    fpscap     the engine's hard-coded 60 fps limiter (maxfps=60.0f stored at VA 0x41617d) -> 0.0 (= no cap); the
               native renderer paces frames instead.
    mmc        the driver flag manual_mouse_copy forced off (init at VA 0x656793, driver-db store at VA 0x51d18f): the
               software cursor's save/restore uses Blt instead of locking the whole frame.
    vidcursor  the cursor's 128x128 work surface (caps at VA 0x656b19) created in video memory.
"""
import hashlib, os, shutil, struct, sys

EXE_SHA_ORIG = '41f14b145e030f2decd95e9f434ccd1de0729ba13d1c5628c4bd9536ee938a02'   # GOG 1.11.1 DungeonSiege.exe
EXE_SHA_PATCHED = 'bd0ff29165ecdeb7a277fc307bacb3e3d62d18bd02096a53a872a215de251bc9'  # with all three patches below

# (name, file offset, original bytes, patched bytes, context check (offset, bytes) or None)
EXE_PATCHES = [
    ('fpscap',    0x1617f, bytes.fromhex('00007042'),     bytes.fromhex('00000000'),     None),
    ('mmc-init',  0x256799, bytes.fromhex('01'),          bytes.fromhex('00'),           (0x256793, bytes.fromhex('c68646010000'))),
    ('mmc-store', 0x11d18f, bytes.fromhex('888146010000'), bytes.fromhex('909090909090'), None),
    ('vidcursor', 0x256b19, struct.pack('<I', 0x840),    struct.pack('<I', 0x4040),    (0x256b13, bytes.fromhex('c783f800'))),
]

def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()

def backup(path, suffix):
    b = path + suffix
    if not os.path.exists(b): shutil.copy2(path, b)
    return b

def patch(path, revert=False, status=False):
    data = bytearray(open(path, 'rb').read()); h = hashlib.sha256(data).hexdigest()
    states = []
    for name, off, orig, new, ctx in EXE_PATCHES:
        if ctx and data[ctx[0]:ctx[0] + len(ctx[1])] != ctx[1]: sys.exit(f'{name}: unexpected context bytes at {ctx[0]:#x}; not GOG 1.11.1?')
        cur = bytes(data[off:off + len(orig)])
        states.append((name, 'original' if cur == orig else 'patched' if cur == new else 'unknown'))
    if status:
        print(f'{path}: sha256 {h}' + (' (original GOG 1.11.1)' if h == EXE_SHA_ORIG else ' (fully patched)' if h == EXE_SHA_PATCHED else ''))
        for n, s in states: print(f'  {n:10s} {s}')
        return
    if any(s == 'unknown' for _, s in states): sys.exit(f'unrecognised bytes: {states}; refusing to patch')
    if not revert and h not in (EXE_SHA_ORIG, EXE_SHA_PATCHED) and all(s == 'original' for _, s in states):
        sys.exit(f'{path}: SHA-256 {h} is not GOG Dungeon Siege 1.11.1; refusing to patch')
    if not revert: backup(path, '.orig')
    for name, off, orig, new, ctx in EXE_PATCHES:
        data[off:off + len(orig)] = orig if revert else new
    open(path, 'wb').write(data)
    print(f'{"reverted" if revert else "patched"} {path} (sha256 {sha(path)[:16]}...)')


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__)
    patch(sys.argv[1], revert='--revert' in sys.argv, status='--status' in sys.argv)
