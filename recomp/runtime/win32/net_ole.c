/* WSOCK32 (multiplayer: reports no network for now) and OLE32 basics. */
#include "w32.h"
IMPL(wsock32, ord115)                                                 /* WSAStartup(version, WSADATA*) */
{
    uint32_t d = ARG(1); memset(GP(d), 0, 400);
    rt_w16(G_MEM, d, ARG(0) & 0xffff); rt_w16(G_MEM, d + 2, 0x0202);
    strcpy((char *)GP(d + 4), "WinSock 2.0"); strcpy((char *)GP(d + 261), "Running");
    rt_w16(G_MEM, d + 390, 100); rt_w16(G_MEM, d + 392, 65467);
    RET(0, 2);
}
IMPL(wsock32, ord116) { RET(0, 0); }                                  /* WSACleanup */
IMPL(wsock32, ord52) { RET(0, 1); }                                   /* gethostbyname: no network */
IMPL(wsock32, ord57) { if (ARG(1) >= 10) { strcpy((char *)GP(ARG(0)), "localhost"); RET(0, 2); } RET(0xffffffffu, 2); }   /* gethostname */
IMPL(wsock32, ord10)                                                  /* inet_addr */
{
    unsigned a, b, c2, d; if (sscanf(GS(ARG(0)), "%u.%u.%u.%u", &a, &b, &c2, &d) == 4) RET((d << 24) | (c2 << 16) | (b << 8) | a, 1);
    RET(0xffffffffu, 1);
}
IMPL(wsock32, ord14) { RET(__builtin_bswap32(ARG(0)), 1); }           /* ntohl */
IMPL(ole32, CoInitializeEx) { RET(0, 2); }
IMPL(ole32, CoUninitialize) { RET(0, 0); }
IMPL(ole32, CoCreateInstance)
{
    uint32_t clsid = ARG(0), out = ARG(4);
    fprintf(stderr, "w32: CoCreateInstance({%08x-...}) -> class not registered\n", rt_r32(G_MEM, clsid));
    if (out) rt_w32(G_MEM, out, 0);
    RET(0x80040154u, 5);
}
IMPL(ole32, CoCreateGuid)
{
    static uint32_t n = 1; uint32_t g = ARG(0);
    for (int k = 0; k < 4; k++) rt_w32(G_MEM, g + 4 * (uint32_t)k, w32_deterministic ? 0x12340000u + n++ : arc4random());
    RET(0, 1);
}
IMPL(ole32, StringFromGUID2)
{
    uint32_t g = ARG(0), out = ARG(1), cap = ARG(2); uint8_t b[16]; memcpy(b, GP(g), 16); char s[40];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rt_r32(G_MEM, g), rt_r16(G_MEM, g + 4), rt_r16(G_MEM, g + 6),
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    if (cap < 39) RET(0, 3);
    w32_mb_to_wide(1252, (const uint8_t *)s, 39, (uint16_t *)GP(out), 39); RET(39, 3);
}
IMPL(ole32, IIDFromString)
{
    char s[64]; w32_wide_to_mb(1252, GW(ARG(0)), -1, (uint8_t *)s, sizeof s);
    unsigned a, b, c2, d[8];
    if (sscanf(s, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &a, &b, &c2, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) RET(0x80040064u, 2);
    uint32_t g = ARG(1); rt_w32(G_MEM, g, a); rt_w16(G_MEM, g + 4, b); rt_w16(G_MEM, g + 6, c2);
    for (int k = 0; k < 8; k++) rt_w8(G_MEM, g + 8 + (uint32_t)k, d[k]);
    RET(0, 2);
}
