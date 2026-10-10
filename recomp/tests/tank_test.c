/* Synthetic Tank fixtures, including the unaligned file tables written by game saves.
 * clang -O2 recomp/tests/tank_test.c recomp/runtime/win32/tank.c -lz -o /tmp/tank_test && /tmp/tank_test */
#include "../runtime/win32/tank.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static uint8_t image[1024], pixels[96];
static void put32(size_t o, uint32_t v) { memcpy(image + o, &v, 4); }
static void put16(size_t o, uint16_t v) { memcpy(image + o, &v, 2); }
static size_t fixture(unsigned skew, int broken, int tail)
{
    memset(image, 0, sizeof image); memcpy(image, "DSigTank", 8);
    for (unsigned i = 0; i < sizeof pixels; i++) pixels[i] = (uint8_t)('a' + i % 4);
    size_t files = 260 + skew, entry = files + 8, chunks = entry + 44;
    put32(12, 32); put32(16, (uint32_t)files); put32(24, 64); /* empty directories; data starts at 64 */
    put32(files, 1); put32(files + 4, 8);
    put32(entry + 4, sizeof pixels); put16(entry + 24, 1);
    put16(entry + 28, 12); memcpy(image + entry + 30, "portrait.bmp", 13);
    uLongf packed = 128; assert(compress(image + 64, &packed, pixels, sizeof pixels - tail) == Z_OK);
    memcpy(image + 64 + packed, pixels + sizeof pixels - tail, tail);
    put32(chunks, (uint32_t)packed + tail); put32(chunks + 4, 16384);
    put32(chunks + 8, sizeof pixels); put32(chunks + 12, (uint32_t)packed);
    put32(chunks + 16, tail);
    if (broken == 1) image[64] ^= 0xff;                    /* invalid compressed stream */
    if (broken == 2) put32(chunks + 8, sizeof pixels - 1); /* decoded size disagrees with metadata */
    if (broken == 3) put32(chunks + 4, 0);                /* no chunk size */
    if (broken == 4) put32(entry + 4, sizeof pixels + 1); /* incomplete file */
    if (broken == 5) put32(entry + 8, sizeof image);      /* data outside the archive */
    if (broken == 6) put16(entry + 24, 99);              /* unsupported compression */
    if (broken == 7) put32(chunks + 16, sizeof pixels + 1); /* impossible raw tail */
    return chunks + 24;
}
int main(void)
{
    char path[] = "/tmp/ds-tank-test.XXXXXX"; int fd = mkstemp(path); assert(fd >= 0);
    for (unsigned skew = 0; skew < 4; skew++) for (int tail = 0; tail <= 16; tail += 16) for (int broken = 0; broken <= 7; broken++) {
        size_t n = fixture(skew, broken, tail); assert(!ftruncate(fd, 0)); assert(lseek(fd, 0, SEEK_SET) == 0);
        assert(write(fd, image, n) == (ssize_t)n);
        uint8_t *out = NULL; size_t length = 0;
        int rc = tank_read(path, "portrait.bmp", &out, &length);
        if (!broken) {
            assert(rc == 0 && length == sizeof pixels && !memcmp(out, pixels, sizeof pixels));
        } else assert(rc != 0 && out == NULL && length == 0);
        free(out);
    }
    close(fd); unlink(path);
    puts("Tank: all four table alignments, raw tails, corrupt streams, sizes and offsets passed");
    return 0;
}
