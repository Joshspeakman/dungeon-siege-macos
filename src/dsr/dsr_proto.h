/* DSR command stream: a sequence of { u32 op; u32 size (payload bytes, multiple of 4); payload } records.
 * All objects are referred to by ids allocated by the DirectDraw layer. Vertex data is copied into the stream at draw time. */
#ifndef DSR_PROTO_H
#define DSR_PROTO_H
enum {
    DSR_NOP = 0,
    DSR_RENDER_STATE,      /* u32 state, u32 value */
    DSR_TSS,               /* u32 stage, u32 type, u32 value */
    DSR_TEXTURE,           /* u32 stage, u32 surface id (0 = none) */
    DSR_TRANSFORM,         /* u32 type, float m[16] */
    DSR_VIEWPORT,          /* u32 x, y, w, h; float minz, maxz */
    DSR_CLEAR,             /* u32 flags, u32 color, float z, u32 stencil, u32 nrects, RECT rects[n] */
    DSR_DRAW,              /* u32 prim, u32 fvf, u32 nverts, u32 nindices, verts[nverts*stride], u16 indices[n] (padded to 4) */
    DSR_SET_RT,            /* u32 color id, u32 depth id */
    DSR_SURFACE_CREATE,    /* u32 id, w, h, u32 kind (1 rt, 2 depth, 3 texture, 4 offscreen), u32 format (1 argb, 2 xrgb, 3 z32), u32 parent id, u32 level */
    DSR_SURFACE_DESTROY,   /* u32 id */
    DSR_SURFACE_UPLOAD,    /* u32 id, u32 x, y, w, h, u32 pitch, pixels[h*pitch] */
    DSR_BLT,               /* u32 dst, RECT dstrect, u32 src (0 = fill), RECT srcrect, u32 flags, u32 fillcolor */
    DSR_PRESENT,           /* u32 surface id [, u32 qpc_lo, qpc_hi, qpc_freq: when the game finished the frame] */
    DSR_GAMMA,             /* u16 red[256], green[256], blue[256] */
    DSR_MODE,              /* u32 w, h */
    DSR_SCENE,             /* u32 begin (1) / end (0) */
    DSR_READBACK,          /* u32 surface id, u32 x, y, w, h, u32 cookie: write pixels into the readback area, then ack */
    DSR_UPLOAD_OVER,       /* as SURFACE_UPLOAD, but composited over the GPU copy as premultiplied alpha (ONE, INVSRCALPHA) */
};
#endif
