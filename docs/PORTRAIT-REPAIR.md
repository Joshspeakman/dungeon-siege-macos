# Repairing a saved black portrait

Some characters created under Wine/CrossOver have an entirely black `portrait-N.bmp` inside their save. The game
stores that creation-time image permanently. New characters created by the native app capture their portraits
correctly; loading an old save cannot recover pixels that were never saved.

`recomp/tools/repair_portrait.py` can copy a valid portrait from another save **with the same character appearance**.
Create a temporary native character with the same sex/template, head, skin, hair and clothing choices, then save it.
Keep the original save backed up and quit the game before replacing it.

```sh
python3 recomp/tools/repair_portrait.py old.dssave reference.dsqsave --output repaired.dssave
```

The default party slot is 0; `--slot N` selects another slot present in both saves. The tool checks the saved
template, head and complete skin mapping, requires an entirely black destination and a visible 64×64 reference,
and verifies the Tank index, data and individual resource checksums. It refuses unsupported archive layouts.
It writes a new file and refuses to overwrite an existing output or either input.

Only the selected portrait's compressed payload, offsets and checksums change. After the rewrite, every other
resource is decompressed and compared byte-for-byte, including the party and world state. Load the repaired copy
in a separate test profile before replacing the original. This is a repair tool, not automatic portrait generation
for arbitrary old characters; a matching reference is required.

Synthetic tests (no game assets needed):

```sh
python3 -B recomp/tests/repair_portrait_test.py
clang -O2 -fsanitize=address,undefined recomp/tests/tank_test.c recomp/runtime/win32/tank.c -lz -o /tmp/tank_test
/tmp/tank_test
```
