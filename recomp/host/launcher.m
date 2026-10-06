// The launch window: resolution, view distance, frame rate and single player or multiplayer, remembered in
// <data>/launcher.plist, then Play.
// Its look comes from the game itself, read at launch from the player's own Resources/Objects.dsres (the main menu's
// stone wall, leather plaque, brass trim and wooden buttons) and set in Copperplate, the typeface of the game's UI.
// Nothing from the game is stored in this project; without the archive the window falls back to plain colours.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include <zlib.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

// ---------------------------------------------------------------- reading textures from a tank (.dsres) archive
static NSData *tank_read(NSString *tankPath, NSString *want)
{
    NSData *file = [NSData dataWithContentsOfFile:tankPath options:NSDataReadingMappedIfSafe error:nil];
    if (file.length < 64) return nil;
    const uint8_t *d = file.bytes; size_t n = file.length;
    #define U32(o) (*(const uint32_t *)(d + (o)))
    #define U16(o) (*(const uint16_t *)(d + (o)))
    if (memcmp(d, "DSigTank", 8)) return nil;
    uint32_t dirset = U32(12), fileset = U32(16), dataoff = U32(24);
    if (dirset >= n || fileset >= n) return nil;
    NSMutableDictionary<NSNumber *, NSString *> *dirs = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSNumber *, NSNumber *> *parents = [NSMutableDictionary dictionary];
    for (uint32_t k = 0, nd = U32(dirset); k < nd; k++) {
        uint32_t off = U32(dirset + 4 + 4 * k), o = dirset + off; uint16_t len = U16(o + 16);
        dirs[@(off)] = [[NSString alloc] initWithBytes:d + o + 18 length:len encoding:NSISOLatin1StringEncoding];
        parents[@(off)] = @(U32(o));
    }
    NSString *(^path)(uint32_t) = ^NSString *(uint32_t off) {
        NSMutableArray *parts = [NSMutableArray array];
        for (int guard = 0; guard < 64 && dirs[@(off)].length; guard++) { [parts insertObject:dirs[@(off)] atIndex:0]; uint32_t p = parents[@(off)].unsignedIntValue; if (p == off) break; off = p; }
        return [parts componentsJoinedByString:@"/"];
    };
    for (uint32_t k = 0, nf = U32(fileset); k < nf; k++) {
        uint32_t o = fileset + U32(fileset + 4 + 4 * k), parent = U32(o), size = U32(o + 4), foff = U32(o + 8); uint16_t fmt = U16(o + 24), len = U16(o + 28);
        NSString *name = [[NSString alloc] initWithBytes:d + o + 30 length:len encoding:NSISOLatin1StringEncoding];
        if ([[NSString stringWithFormat:@"%@/%@", path(parent), name] caseInsensitiveCompare:want] != NSOrderedSame) continue;
        uint32_t o2 = (o + 30 + len + 1 + 3) & ~3u;
        if (fmt == 0) return dataoff + foff + size <= n ? [NSData dataWithBytes:d + dataoff + foff length:size] : nil;
        uint32_t chunk = U32(o2 + 4); o2 += 8;
        NSMutableData *out = [NSMutableData dataWithCapacity:size];
        for (uint32_t c = 0; c < (size + chunk - 1) / chunk; c++) {
            uint32_t usz = U32(o2 + 16 * c), csz = U32(o2 + 16 * c + 4), extra = U32(o2 + 16 * c + 8), coff = U32(o2 + 16 * c + 12);
            const uint8_t *src = d + dataoff + foff + coff;
            if (dataoff + foff + coff + csz + extra > n) return nil;
            if (csz < usz) { NSMutableData *u = [NSMutableData dataWithLength:usz]; uLongf ul = usz;
                if (uncompress(u.mutableBytes, &ul, src, csz) != Z_OK) return nil; u.length = ul; [out appendData:u]; }
            else [out appendBytes:src length:usz];
            if (extra) [out appendBytes:src + csz length:extra];
        }
        out.length = size; return out;
    }
    return nil;
    #undef U32
    #undef U16
}
/* Siege .raw texture ("ipaR", format "8888", width, height, then BGRA rows) -> CGImage */
static CGImageRef raw_image(NSData *raw)
{
    if (raw.length < 16 || memcmp(raw.bytes, "ipaR8888", 8)) return NULL;
    const uint16_t *h = raw.bytes; size_t w = h[6], ht = h[7];
    if (16 + w * ht * 4 > raw.length) return NULL;
    CGDataProviderRef pr = CGDataProviderCreateWithCFData((__bridge CFDataRef)[raw subdataWithRange:NSMakeRange(16, w * ht * 4)]);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate(w, ht, 8, 32, w * 4, cs, kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaFirst, pr, NULL, NO, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs); CGDataProviderRelease(pr);
    return img;
}
static NSImage *nsimg(CGImageRef img, CGRect crop)
{
    if (!img) return nil;
    CGImageRef c = CGRectIsEmpty(crop) ? CGImageRetain(img) : CGImageCreateWithImageInRect(img, crop);
    NSImage *r = [[NSImage alloc] initWithCGImage:c size:NSZeroSize]; CGImageRelease(c); return r;
}
/* the part of an image with any opacity */
static CGRect opaque_bounds(CGImageRef img)
{
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img); uint8_t *px = calloc(w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB(); CGContextRef c = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(c, CGRectMake(0, 0, w, h), img);
    size_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) if (px[(y * w + x) * 4 + 3] > 8) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    CGContextRelease(c); CGColorSpaceRelease(cs); free(px);
    return x1 >= x0 ? CGRectMake(x0, y0, x1 - x0 + 1, y1 - y0 + 1) : CGRectZero;   /* bitmap rows run top-down here */
}

// ---------------------------------------------------------------- settings
typedef struct { NSString *value, *label, *note; } Choice;
@interface DSRow : NSObject
@property NSString *title; @property NSArray<NSDictionary *> *choices; @property NSInteger index;
@property (copy) void (^activate)(void);     /* a row that opens something instead of cycling (Mods) */
@end
@implementation DSRow @end

static NSColor *gold(void) { return [NSColor colorWithSRGBRed:0.86 green:0.77 blue:0.55 alpha:1]; }
static NSColor *parchment(void) { return [NSColor colorWithSRGBRed:0.94 green:0.91 blue:0.84 alpha:1]; }
static NSColor *dim(void) { return [NSColor colorWithSRGBRed:0.66 green:0.61 blue:0.52 alpha:1]; }
static NSFont *cp(CGFloat size, BOOL bold)
{
    NSFont *f = [NSFont fontWithName:bold ? @"Copperplate-Bold" : @"Copperplate-Light" size:size] ?: [NSFont fontWithName:@"Copperplate" size:size];
    return f ?: [NSFont systemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
}

// ---------------------------------------------------------------- the view
/* The window: the banner, the Game row, and a button for each group of settings (Display, Graphics, Mods, Updates).
 * A group's button opens it as a panel over the window, with Back where Quit was; Mods and Updates open their own. */
@interface DSSection : NSObject
@property NSString *title; @property NSArray<DSRow *> *rows;
@property (copy) void (^activate)(void); @property (copy) NSString *(^summary)(void);
@end
@implementation DSSection @end

@interface DSLaunchView : NSView
@property NSArray<DSRow *> *top; @property NSArray<DSSection *> *sections; @property NSInteger open;   /* the open group, -1 none */
@property NSInteger focus; @property NSInteger hover; @property NSInteger pressed;
@property NSImage *banner, *stone, *plaque, *trim, *wood, *woodHover, *woodDown; @property NSString *note;
@property (copy) void (^onPlay)(void); @property (copy) void (^onQuit)(void); @property (copy) void (^onChange)(void);
@end

enum { HIT_NONE = -1, HIT_PLAY = 100, HIT_QUIT = 101, HIT_BACK = 102, HIT_SECTION = 200 };   /* rows: 10*row + 0 (left arrow) / 1 (value) / 2 (right arrow) */
enum { BANNER_H = 252, TOP_Y = 270 };

@implementation DSLaunchView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (NSArray<DSRow *> *)rows { return self.open >= 0 ? self.sections[self.open].rows : self.top; }
- (NSInteger)items { return (NSInteger)self.rows.count + (self.open >= 0 ? 0 : (NSInteger)self.sections.count); }   /* what the keyboard moves between */
- (NSRect)flyRect { NSRect b = self.bounds; return NSMakeRect(40, 168, b.size.width - 80, b.size.height - 268); }
- (NSRect)rowRect:(NSInteger)k { CGFloat y = self.open >= 0 ? self.flyRect.origin.y + 70 : TOP_Y + 14; return NSMakeRect(70, y + 74 * k, self.bounds.size.width - 140, 64); }
- (NSRect)arrow:(NSInteger)k right:(BOOL)right { NSRect r = [self rowRect:k]; return NSMakeRect(right ? NSMaxX(r) - 34 : NSMaxX(r) - 330, r.origin.y + 6, 30, 30); }
- (NSRect)valueRect:(NSInteger)k { NSRect r = [self rowRect:k]; return NSMakeRect(NSMaxX(r) - 300, r.origin.y + 9, 262, 30); }
- (NSRect)sectionRect:(NSInteger)i
{
    NSInteger n = (NSInteger)self.sections.count, per = 2, line = i / per, inLine = MIN(per, n - line * per), col = i % per;
    CGFloat w = 290, g = 28, x0 = (self.bounds.size.width - (inLine * w + (inLine - 1) * g)) / 2;
    return NSMakeRect(x0 + col * (w + g), TOP_Y + 74 * self.top.count + 44 + line * 100, w, 58);
}
- (NSRect)playRect { NSRect b = self.bounds; return NSMakeRect(NSMidX(b) + 12, b.size.height - 84, 220, 56); }
- (NSRect)quitRect { NSRect b = self.bounds; return NSMakeRect(NSMidX(b) - 232, b.size.height - 84, 220, 56); }
- (NSInteger)hit:(NSPoint)p
{
    if (NSPointInRect(p, self.playRect)) return HIT_PLAY;
    if (NSPointInRect(p, self.quitRect)) return self.open >= 0 ? HIT_BACK : HIT_QUIT;
    if (self.open < 0) for (NSInteger i = 0; i < (NSInteger)self.sections.count; i++) if (NSPointInRect(p, [self sectionRect:i])) return HIT_SECTION + i;
    for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
        if (NSPointInRect(p, [self arrow:k right:NO])) return 10 * k;
        if (NSPointInRect(p, [self arrow:k right:YES])) return 10 * k + 2;
        if (NSPointInRect(p, [self valueRect:k])) return 10 * k + 1;
    }
    if (self.open >= 0 && !NSPointInRect(p, self.flyRect)) return HIT_BACK;   /* a click outside the panel closes it */
    return HIT_NONE;
}
- (void)openSection:(NSInteger)i
{
    DSSection *s = self.sections[i]; self.focus = (NSInteger)self.top.count + i;
    if (s.activate) { s.activate(); self.needsDisplay = YES; return; }
    self.open = i; self.focus = 0; self.hover = HIT_NONE; self.needsDisplay = YES;
}
- (void)closeSection { if (self.open < 0) return; self.focus = (NSInteger)self.top.count + self.open; self.open = -1; self.hover = HIT_NONE; self.needsDisplay = YES; }
- (void)updateTrackingAreas
{
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil]];
}
- (void)mouseMoved:(NSEvent *)e { NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]]; if (h != self.hover) { self.hover = h; self.needsDisplay = YES; } }
- (void)mouseDown:(NSEvent *)e { self.pressed = [self hit:[self convertPoint:e.locationInWindow fromView:nil]]; self.needsDisplay = YES; }
- (void)mouseUp:(NSEvent *)e
{
    NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]], p = self.pressed; self.pressed = HIT_NONE; self.needsDisplay = YES;
    if (h != p || h == HIT_NONE) return;
    if (h == HIT_PLAY) { if (self.onPlay) self.onPlay(); return; }
    if (h == HIT_QUIT) { if (self.onQuit) self.onQuit(); return; }
    if (h == HIT_BACK) { [self closeSection]; return; }
    if (h >= HIT_SECTION) { [self openSection:h - HIT_SECTION]; return; }
    NSInteger k = h / 10; self.focus = k;
    if (self.rows[k].activate) { self.rows[k].activate(); self.needsDisplay = YES; return; }
    [self step:k by:(h % 10 == 0) ? -1 : 1];
}
- (void)step:(NSInteger)k by:(NSInteger)dir
{
    if (k >= (NSInteger)self.rows.count) { NSInteger n = [self items]; self.focus = (self.focus + dir + n) % n; self.needsDisplay = YES; return; }   /* the group buttons */
    DSRow *r = self.rows[k]; NSInteger n = (NSInteger)r.choices.count;
    if (r.activate) { r.activate(); self.needsDisplay = YES; return; }
    r.index = (r.index + dir + n) % n; self.needsDisplay = YES;
    if (self.onChange) self.onChange();
}
- (void)keyDown:(NSEvent *)e
{
    NSInteger n = [self items], rc = (NSInteger)self.rows.count;
    switch (e.keyCode) {
    case 0x24: case 0x4c:                                                /* Return, Enter */
        if (self.focus >= rc) { [self openSection:self.focus - rc]; return; }
        if (self.rows[self.focus].activate) { self.rows[self.focus].activate(); self.needsDisplay = YES; return; }
        if (self.open >= 0) { [self closeSection]; return; }
        if (self.onPlay) self.onPlay(); return;
    case 0x35: if (self.open >= 0) { [self closeSection]; return; }      /* Escape */
               if (self.onQuit) self.onQuit(); return;
    case 0x7e: self.focus = (self.focus + n - 1) % n; self.needsDisplay = YES; return;
    case 0x7d: self.focus = (self.focus + 1) % n; self.needsDisplay = YES; return;
    case 0x7b: [self step:self.focus by:-1]; return;
    case 0x7c: [self step:self.focus by:1]; return;
    }
    [super keyDown:e];
}
static void draw_text(NSString *s, NSFont *f, NSColor *c, NSRect r, NSTextAlignment al, NSShadow *sh)
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = al; ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSMutableDictionary *a = [@{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: ps} mutableCopy];
    if (sh) a[NSShadowAttributeName] = sh;
    [s drawInRect:r withAttributes:a];
}
- (void)drawTitle:(NSRect)r
{
    /* "DUNGEON SIEGE" in the logo's manner: brushed silver letters, a copper rim, a dark drop shadow */
    NSString *t = @"DUNGEON SIEGE"; NSFont *f = cp(46, YES);
    NSDictionary *at = @{NSFontAttributeName: f, NSKernAttributeName: @3};
    NSSize sz = [t sizeWithAttributes:at]; NSPoint o = NSMakePoint(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2 - 2);
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.85]; sh.shadowOffset = NSMakeSize(0, -3); sh.shadowBlurRadius = 5;
    NSMutableDictionary *rim = [at mutableCopy]; rim[NSStrokeColorAttributeName] = [NSColor colorWithSRGBRed:0.62 green:0.33 blue:0.16 alpha:1]; rim[NSStrokeWidthAttributeName] = @(9); rim[NSShadowAttributeName] = sh;
    rim[NSForegroundColorAttributeName] = [NSColor colorWithSRGBRed:0.62 green:0.33 blue:0.16 alpha:1];
    [t drawAtPoint:o withAttributes:rim];
    /* silver fill: the glyphs as a clip, a vertical gradient through it */
    NSImage *mask = [NSImage imageWithSize:sz flipped:NO drawingHandler:^BOOL(NSRect dst) {
        NSMutableDictionary *m = [at mutableCopy]; m[NSForegroundColorAttributeName] = NSColor.whiteColor; [t drawAtPoint:NSZeroPoint withAttributes:m];
        NSGradient *g = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0.98 alpha:1], 0.0, [NSColor colorWithWhite:0.72 alpha:1], 0.45,
                         [NSColor colorWithWhite:0.42 alpha:1], 0.5, [NSColor colorWithWhite:0.86 alpha:1], 0.8, [NSColor colorWithWhite:0.6 alpha:1], 1.0, nil];
        [[NSGraphicsContext currentContext] setCompositingOperation:NSCompositingOperationSourceIn];
        [g drawInRect:dst angle:90];
        return YES;
    }];
    [mask drawInRect:NSMakeRect(o.x, o.y, sz.width, sz.height) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
}
- (void)drawButton:(NSRect)r title:(NSString *)title code:(NSInteger)code primary:(BOOL)primary
{
    NSImage *img = self.pressed == code ? (self.woodDown ?: self.wood) : self.hover == code ? (self.woodHover ?: self.wood) : self.wood;
    if (img) [img drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    else { [[NSColor colorWithSRGBRed:0.36 green:0.22 blue:0.12 alpha:1] setFill]; [[NSBezierPath bezierPathWithRoundedRect:r xRadius:6 yRadius:6] fill]; }
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.9]; sh.shadowOffset = NSMakeSize(0, -1.5); sh.shadowBlurRadius = 2;
    NSColor *c = self.hover == code ? [NSColor colorWithSRGBRed:1 green:0.93 blue:0.72 alpha:1] : primary ? gold() : parchment();
    /* the capitals centred on the button (the line's own box sits them high: it leaves room below for descenders) */
    NSFont *f = cp(primary ? 24 : 20, YES); NSDictionary *at = @{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSShadowAttributeName: sh};
    NSSize sz = [title sizeWithAttributes:at];
    [title drawAtPoint:NSMakePoint(NSMidX(r) - sz.width / 2, round(NSMidY(r) + f.capHeight / 2 - f.ascender)) withAttributes:at];
}
- (void)drawArrow:(NSRect)r right:(BOOL)right lit:(BOOL)lit
{
    NSBezierPath *p = [NSBezierPath bezierPath]; CGFloat x0 = r.origin.x + 7, x1 = NSMaxX(r) - 7, y0 = r.origin.y + 6, y1 = NSMaxY(r) - 6, ym = NSMidY(r);
    if (right) { [p moveToPoint:NSMakePoint(x0, y0)]; [p lineToPoint:NSMakePoint(x1, ym)]; [p lineToPoint:NSMakePoint(x0, y1)]; }
    else { [p moveToPoint:NSMakePoint(x1, y0)]; [p lineToPoint:NSMakePoint(x0, ym)]; [p lineToPoint:NSMakePoint(x1, y1)]; }
    [p closePath];
    NSGradient *g = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:lit ? 1.0 : 0.88 alpha:1] endingColor:[NSColor colorWithWhite:lit ? 0.7 : 0.45 alpha:1]];
    [g drawInBezierPath:p angle:90];
    [[NSColor colorWithSRGBRed:0.55 green:0.3 blue:0.14 alpha:1] setStroke]; p.lineWidth = 1.5; [p stroke];
}
/* a riveted iron band around a plaque, like the frames of the game's menus */
- (void)drawIronFrame:(NSRect)r
{
    NSBezierPath *outer = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 3, 3)];
    outer.lineWidth = 7; [[NSColor colorWithWhite:0.08 alpha:1] setStroke]; [outer stroke];
    outer.lineWidth = 5; [[NSColor colorWithSRGBRed:0.45 green:0.46 blue:0.47 alpha:1] setStroke]; [outer stroke];
    NSBezierPath *hi = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 1.5, 1.5)]; hi.lineWidth = 1; [[NSColor colorWithWhite:0.78 alpha:0.8] setStroke]; [hi stroke];
    NSBezierPath *lo = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 6, 6)]; lo.lineWidth = 1.2; [[NSColor colorWithWhite:0.05 alpha:0.9] setStroke]; [lo stroke];
    void (^rivet)(CGFloat, CGFloat) = ^(CGFloat x, CGFloat y) {
        NSRect d = NSMakeRect(x - 4, y - 4, 8, 8);
        NSGradient *g = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithSRGBRed:0.98 green:0.86 blue:0.55 alpha:1], 0.0,
                         [NSColor colorWithSRGBRed:0.62 green:0.42 blue:0.16 alpha:1], 0.6, [NSColor colorWithSRGBRed:0.25 green:0.15 blue:0.05 alpha:1], 1.0, nil];
        [g drawInBezierPath:[NSBezierPath bezierPathWithOvalInRect:d] relativeCenterPosition:NSMakePoint(-0.35, 0.35)];
    };
    for (CGFloat x = r.origin.x + 14; x <= NSMaxX(r) - 13; x += (r.size.width - 28) / 8) { rivet(x, r.origin.y + 3.5); rivet(x, NSMaxY(r) - 3.5); }
    rivet(r.origin.x + 3.5, NSMidY(r)); rivet(NSMaxX(r) - 3.5, NSMidY(r));
}
- (void)drawRows:(NSShadow *)sh
{
    for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
        DSRow *row = self.rows[k]; NSRect r = [self rowRect:k]; NSDictionary *ch = row.choices[row.index];
        if (k == self.focus) { [[NSColor colorWithSRGBRed:0.86 green:0.6 blue:0.25 alpha:0.12] setFill]; [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, -10, -2) xRadius:3 yRadius:3] fill]; }
        draw_text(row.title.uppercaseString, cp(19, YES), gold(), NSMakeRect(r.origin.x, r.origin.y + 8, 300, 28), NSTextAlignmentLeft, sh);
        [self drawArrow:[self arrow:k right:NO] right:NO lit:self.hover == 10 * k];
        [self drawArrow:[self arrow:k right:YES] right:YES lit:self.hover == 10 * k + 2];
        draw_text(ch[@"label"], cp(18, NO), self.hover == 10 * k + 1 ? NSColor.whiteColor : parchment(), [self valueRect:k], NSTextAlignmentCenter, sh);
        draw_text(ch[@"note"] ?: @"", cp(11, NO), dim(), NSMakeRect(NSMaxX(r) - 330, r.origin.y + 42, 330, 18), NSTextAlignmentCenter, nil);
        if (k + 1 < (NSInteger)self.rows.count) { [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.35] setFill]; NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) + 5, r.size.width, 1)); }
    }
}
- (void)drawPanel:(NSRect)panel
{
    [[NSColor colorWithWhite:0 alpha:0.5] setFill]; [[NSBezierPath bezierPathWithRoundedRect:panel xRadius:4 yRadius:4] fill];
    [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.9] setStroke]; NSBezierPath *bp = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(panel, 0.5, 0.5) xRadius:4 yRadius:4]; bp.lineWidth = 1.5; [bp stroke];
}
- (void)drawRect:(NSRect)dirty
{
    NSRect b = self.bounds;
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.9]; sh.shadowOffset = NSMakeSize(0, -1); sh.shadowBlurRadius = 2;
    /* stone wall */
    if (self.stone) { [[NSColor colorWithPatternImage:self.stone] setFill]; NSRectFill(b); } else { [[NSColor colorWithWhite:0.11 alpha:1] setFill]; NSRectFill(b); }
    NSGradient *vig = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0.15], 0.0, [NSColor colorWithWhite:0 alpha:0.62], 1.0, nil];
    [vig drawInRect:b relativeCenterPosition:NSMakePoint(0, 0.1)];
    if (self.banner) {
        /* the game's key art across the top (downloaded at install), fading into the wall */
        NSSize is = self.banner.size; CGFloat h = b.size.width * is.height / is.width;
        NSRect br = NSMakeRect(0, 0, b.size.width, BANNER_H), ir = NSMakeRect(0, (BANNER_H - h) / 2, b.size.width, h);
        [NSColor.blackColor setFill]; NSRectFill(br);
        [self.banner drawInRect:ir fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES
                          hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
        NSGradient *fade = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:0 alpha:0] endingColor:[NSColor colorWithWhite:0 alpha:0.9]];
        [fade drawInRect:NSMakeRect(0, BANNER_H - 40, b.size.width, 40) angle:90];
        [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.9] setFill]; NSRectFill(NSMakeRect(0, BANNER_H, b.size.width, 1.5));
        /* the subtitle beside the hook of the logo's S, under "SIEGE" (placed on the art, so it follows it) */
        NSFont *sf = cp(14, NO); NSRect sr = NSMakeRect(ir.origin.x + 0.299 * ir.size.width, ir.origin.y + 0.675 * ir.size.height, 0.312 * ir.size.width, 0.15 * ir.size.height);
        NSDictionary *sa = @{NSFontAttributeName: sf, NSForegroundColorAttributeName: gold(), NSShadowAttributeName: sh, NSKernAttributeName: @1};
        NSString *st = @"NATIVE EDITION FOR macOS"; NSSize ss = [st sizeWithAttributes:sa];
        [st drawAtPoint:NSMakePoint(NSMidX(sr) - ss.width / 2, round(NSMidY(sr) + sf.capHeight / 2 - sf.ascender)) withAttributes:sa];
    } else {
        /* without it: the main menu's title plaque */
        NSRect pr = NSMakeRect(NSMidX(b) - 290, 60, 580, 128);
        if (self.plaque) [self.plaque drawInRect:NSInsetRect(pr, 6, 6) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
        else { [[NSColor colorWithSRGBRed:0.42 green:0.25 blue:0.15 alpha:1] setFill]; NSRectFill(NSInsetRect(pr, 6, 6)); }
        [self drawIronFrame:pr];
        [self drawTitle:NSMakeRect(pr.origin.x, pr.origin.y + 6, pr.size.width, pr.size.height - 30)];
        draw_text(@"NATIVE EDITION FOR macOS", cp(13, NO), gold(), NSMakeRect(pr.origin.x, NSMaxY(pr) - 34, pr.size.width, 18), NSTextAlignmentCenter, sh);
    }
    /* the top level: the Game row, then a button for each group with a summary under it */
    NSInteger fly = self.open, foc = self.focus, hov = self.hover; self.open = -1;
    if (fly >= 0) self.focus = self.hover = HIT_NONE;           /* under the panel: nothing lit */
    [self drawPanel:NSMakeRect(54, TOP_Y, b.size.width - 108, 74 * self.top.count + 14)];
    [self drawRows:sh];
    for (NSInteger i = 0; i < (NSInteger)self.sections.count; i++) {
        DSSection *s = self.sections[i]; NSRect r = [self sectionRect:i]; NSInteger code = HIT_SECTION + i;
        if (fly < 0 && self.focus == (NSInteger)self.top.count + i) { [[NSColor colorWithSRGBRed:0.86 green:0.6 blue:0.25 alpha:0.28] setFill]; [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, -6, -5) xRadius:6 yRadius:6] fill]; }
        [self drawButton:r title:s.title.uppercaseString code:code primary:NO];
        NSString *sum = s.summary ? s.summary() : nil;
        if (!sum) { NSMutableArray *l = [NSMutableArray array]; for (DSRow *row in s.rows) [l addObject:row.choices[row.index][@"label"]]; sum = [l componentsJoinedByString:@" · "]; }
        draw_text(sum, cp(11, NO), dim(), NSMakeRect(r.origin.x - 10, NSMaxY(r) + 6, r.size.width + 20, 32), NSTextAlignmentCenter, nil);
    }
    if (self.note.length) draw_text(self.note, cp(11, NO), dim(), NSMakeRect(54, [self sectionRect:(NSInteger)self.sections.count - 1].origin.y + 104, b.size.width - 108, 30), NSTextAlignmentCenter, nil);
    self.open = fly; self.focus = foc; self.hover = hov;
    /* an open group: a panel over the window */
    if (self.open >= 0) {
        [[NSColor colorWithWhite:0 alpha:0.6] setFill]; NSRectFillUsingOperation(b, NSCompositingOperationSourceOver);
        NSRect f = self.flyRect;
        if (self.stone) { [[NSColor colorWithPatternImage:self.stone] setFill]; NSRectFill(NSInsetRect(f, 6, 6)); }
        [[NSColor colorWithWhite:0 alpha:0.55] setFill]; NSRectFillUsingOperation(NSInsetRect(f, 6, 6), NSCompositingOperationSourceOver);
        [self drawIronFrame:f];
        draw_text(self.sections[self.open].title.uppercaseString, cp(24, YES), gold(), NSMakeRect(f.origin.x, f.origin.y + 22, f.size.width, 32), NSTextAlignmentCenter, sh);
        [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.6] setFill]; NSRectFill(NSMakeRect(f.origin.x + 30, f.origin.y + 60, f.size.width - 60, 1));
        [self drawRows:sh];
    }
    [self drawButton:self.quitRect title:self.open >= 0 ? @"BACK" : @"QUIT" code:self.open >= 0 ? HIT_BACK : HIT_QUIT primary:NO];
    [self drawButton:self.playRect title:@"PLAY" code:HIT_PLAY primary:YES];
}
@end

// ---------------------------------------------------------------- choices, settings file, the window
static NSArray<NSDictionary *> *resolution_choices(void)
{
    NSScreen *s = NSScreen.mainScreen; int W = (int)s.frame.size.width, H = (int)s.frame.size.height; double sc = s.backingScaleFactor;
    if (@available(macOS 12.0, *)) H -= (int)s.safeAreaInsets.top;   /* full screen sits below a notch */
    NSMutableArray *a = [NSMutableArray array]; NSMutableSet *seen = [NSMutableSet set];
    void (^add)(int, int, NSString *) = ^(int w, int h, NSString *note) {
        NSString *v = [NSString stringWithFormat:@"%dx%d", w, h]; if ([seen containsObject:v]) return; [seen addObject:v];
        [a addObject:@{@"value": v, @"label": [NSString stringWithFormat:@"%d × %d", w, h], @"note": note}];
    };
    add(W, H, @"This display (recommended)");
    if (sc > 1.01) add((int)(W * sc + 0.5), (int)(H * sc + 0.5), @"Full Retina detail; the interface is drawn very small");
    static const int std[][2] = {{2560,1600},{2560,1440},{1920,1200},{1920,1080},{1680,1050},{1600,900},{1440,900},{1366,768},{1280,800},{1280,720},{1024,768},{800,600}};
    for (size_t k = 0; k < sizeof std / sizeof *std; k++) if (std[k][0] <= W && std[k][1] <= H) add(std[k][0], std[k][1], @"Scaled to fill the screen");
    return a;
}
static NSArray<NSDictionary *> *distance_choices(void)
{
    return @[@{@"value": @"100", @"label": @"Original", @"note": @"As the game shipped"},
             @{@"value": @"125", @"label": @"Far", @"note": @"125% of the original"},
             @{@"value": @"150", @"label": @"Farther", @"note": @"150%, about what the SeeFar mod gives"},
             @{@"value": @"200", @"label": @"Very Far", @"note": @"200% of the original"},
             @{@"value": @"250", @"label": @"Horizon", @"note": @"250%; heavier on large outdoor areas"},
             @{@"value": @"300", @"label": @"Maximum", @"note": @"300%; may slow down in the biggest vistas"}];
}
/* the view distance to start with: Farther (150%) everywhere; Very Far (200%) on the big-GPU chips (Max and Ultra, with
 * 2-4 times the GPU of the M1 Pro it was measured on), so a more powerful Mac starts higher; a saved choice wins */
static NSInteger default_distance_index(void)
{
    NSString *gpu = MTLCreateSystemDefaultDevice().name ?: @"";
    return [gpu containsString:@" Max"] || [gpu containsString:@" Ultra"] ? 3 : 2;
}
/* character shadows: the silhouette texture's size (the game's own is 64) and a softening of its edges (renderer.m) */
static NSArray<NSDictionary *> *shadow_detail_choices(void)
{
    return @[@{@"value": @"64", @"label": @"Original", @"note": @"64 pixels, as the game shipped"},
             @{@"value": @"128", @"label": @"128", @"note": @"Twice the original detail"},
             @{@"value": @"256", @"label": @"256", @"note": @"Sharp shadows at any resolution (recommended)"},
             @{@"value": @"512", @"label": @"512", @"note": @"Finer still"},
             @{@"value": @"1024", @"label": @"1024", @"note": @"The most detail; more GPU memory and work"}];
}
static NSArray<NSDictionary *> *shadow_edge_choices(void)
{
    return @[@{@"value": @"off", @"label": @"Original", @"note": @"Hard edges, as the game shipped"},
             @{@"value": @"soft", @"label": @"Soft", @"note": @"Smoothed edges (recommended)"},
             @{@"value": @"softer", @"label": @"Softer", @"note": @"Wider, softer edges"}];
}
static void apply_shadows(NSString *detail, NSString *edges)
{
    if (![detail isEqualToString:@"64"]) setenv("DS_SHADOW_RESOLUTION", detail.UTF8String, 1);   /* Original: the game's own value */
    setenv("DSR_SHADOW_FILTER", edges.UTF8String, 1);
}
/* ---- the game's own graphics options (its Options > Video page), kept in the game's prefs.gas: read when the window opens
 * and written when Play is pressed, so they're set here and the in-game page needn't be used (where a change of
 * resolution can also reset other things) ---- */
static NSString *prefs_path(NSString *data, BOOL loa)
{
    return [data stringByAppendingFormat:@"/drive_c/Users/player/Documents/%@/prefs.gas", loa ? @"Dungeon Siege LOA" : @"Dungeon Siege"];
}
static NSString *prefs_get(NSString *path, NSString *key)          /* "key = value;" in the [prefs] block */
{
    NSString *t = [NSString stringWithContentsOfFile:path encoding:NSISOLatin1StringEncoding error:nil]; if (!t) return nil;
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"^[ \\t]*%@[ \\t]*=[ \\t]*([^;\\r\\n]*);", key]
                                                                        options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSTextCheckingResult *m = [re firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
    return m ? [[t substringWithRange:[m rangeAtIndex:1]] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] : nil;
}
static void prefs_set(NSString *path, NSDictionary<NSString *, NSString *> *kv)
{
    NSMutableString *t = [[NSString stringWithContentsOfFile:path encoding:NSISOLatin1StringEncoding error:nil] mutableCopy];
    NSString *nl = !t || [t containsString:@"\r\n"] ? @"\r\n" : @"\n";
    if (!t) t = [NSMutableString stringWithFormat:@"[prefs]%@{%@}%@", nl, nl, nl];
    for (NSString *key in kv) {
        NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"^([ \\t]*%@[ \\t]*=[ \\t]*)[^;\\r\\n]*;", key]
                                                                            options:NSRegularExpressionAnchorsMatchLines error:nil];
        NSTextCheckingResult *m = [re firstMatchInString:t options:0 range:NSMakeRange(0, t.length)];
        if (m) { [t replaceCharactersInRange:m.range withString:[NSString stringWithFormat:@"%@%@;", [t substringWithRange:[m rangeAtIndex:1]], kv[key]]]; continue; }
        NSRange open = [t rangeOfString:@"{"];                              /* the [prefs] block's opening brace */
        if (open.location == NSNotFound) continue;
        [t insertString:[NSString stringWithFormat:@"%@\t%@ = %@;", nl, key, kv[key]] atIndex:NSMaxRange(open)];
    }
    [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    [t writeToFile:path atomically:YES encoding:NSISOLatin1StringEncoding error:nil];
}
static NSArray<NSDictionary *> *game_shadow_choices(void)
{
    return @[@{@"value": @"none", @"label": @"Off", @"note": @"No character shadows"},
             @{@"value": @"simple", @"label": @"Simple", @"note": @"Round shadows under everyone"},
             @{@"value": @"complex_party", @"label": @"Party Complex", @"note": @"True shadows for your party (as the game shipped)"},
             @{@"value": @"complex", @"label": @"All Complex", @"note": @"True shadows for every character (recommended)"}];
}
static NSArray<NSDictionary *> *filtering_choices(void)
{
    return @[@{@"value": @"bilinear", @"label": @"Bilinear", @"note": @"As the game shipped"},
             @{@"value": @"trilinear", @"label": @"Trilinear", @"note": @"Smoother distant textures (recommended)"}];
}
static NSArray<NSDictionary *> *detail_choices(void)
{
    return @[@{@"value": @"0.000000", @"label": @"Lowest", @"note": @"The fewest small objects"},
             @{@"value": @"0.250000", @"label": @"Low", @"note": @"A quarter of the small scenery"},
             @{@"value": @"0.500000", @"label": @"Medium", @"note": @"Half of the small scenery"},
             @{@"value": @"0.750000", @"label": @"High", @"note": @"Most of the small scenery"},
             @{@"value": @"1.000000", @"label": @"Full", @"note": @"Everything (as the game shipped)"}];
}
static NSArray<NSDictionary *> *gamma_choices(void)    /* the in-game slider's steps: 0.5 to 1.5 */
{
    NSMutableArray *a = [NSMutableArray array];
    for (int k = 0; k <= 10; k++) {
        double g = 0.5 + k / 10.0;
        [a addObject:@{@"value": [NSString stringWithFormat:@"%f", g], @"label": [NSString stringWithFormat:@"%.1f", g],
                       @"note": k == 5 ? @"As the game shipped" : k < 5 ? @"Darker" : @"Brighter"}];
    }
    return a;
}
/* a row's index for a value from prefs.gas: numbers compared as numbers; a value not offered becomes a "Custom" choice */
static NSInteger pref_index(DSRow *r, NSString *v, NSInteger dflt)
{
    if (!v.length) return dflt;
    for (NSUInteger k = 0; k < r.choices.count; k++) {
        NSString *c = r.choices[k][@"value"];
        if ([c isEqualToString:v] || (isdigit([c characterAtIndex:0]) && fabs(c.doubleValue - v.doubleValue) < 0.005)) return (NSInteger)k;
    }
    NSString *shown = isdigit([v characterAtIndex:0]) ? [NSString stringWithFormat:@"%.2f", v.doubleValue] : v;
    r.choices = [r.choices arrayByAddingObject:@{@"value": v, @"label": [NSString stringWithFormat:@"Custom (%@)", shown], @"note": @"Set in the game's own options"}];
    return (NSInteger)r.choices.count - 1;
}
static NSArray<NSDictionary *> *framerate_choices(void)
{
    NSInteger hz = NSScreen.mainScreen.maximumFramesPerSecond; if (hz <= 0) hz = 60;
    NSMutableArray *a = [NSMutableArray arrayWithObject:@{@"value": @"auto", @"label": @"Automatic",
        @"note": hz >= 120 ? @"Smooth 120 fps, 60 fps in the heaviest scenes" : @"Matches the display"}];
    if (hz >= 120) [a addObject:@{@"value": @"120", @"label": @"120 FPS", @"note": @"Locked to 120"}];
    [a addObject:@{@"value": @"60", @"label": @"60 FPS", @"note": @"Locked to 60; cooler and quieter"}];
    [a addObject:@{@"value": @"30", @"label": @"30 FPS", @"note": @"Saves battery"}];
    [a addObject:@{@"value": @"unlimited", @"label": @"Unlimited", @"note": @"No cap and no vertical sync (may tear)"}];
    return a;
}
/* the addresses other players type to join this Mac: its LAN address and, if there is one, a 100.64/10 address
 * (Tailscale and similar virtual networks) */
static NSString *join_addresses(void)
{
    NSString *lan = nil, *vpn = nil; struct ifaddrs *ifs = 0;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
            char ip[64]; inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
            if ((a >> 22) == (100u << 2 | 1)) { if (!vpn) vpn = @(ip); }                       /* 100.64.0.0/10 */
            else if (((a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8) && !lan) lan = @(ip);   /* private ranges */
        }
        freeifaddrs(ifs);
    }
    if (lan && vpn) return [NSString stringWithFormat:@"This Mac: %@ · VPN %@", lan, vpn];
    if (lan || vpn) return [NSString stringWithFormat:@"This Mac: %@", lan ?: vpn];
    return nil;
}
static NSString *expansion_dir(NSString *data) { return [data stringByAppendingPathComponent:@"expansion"]; }
static BOOL have_expansion(NSString *data)       /* install.sh --expansion: Legends of Aranna's data */
{
    return [NSFileManager.defaultManager fileExistsAtPath:[expansion_dir(data) stringByAppendingPathComponent:@"Resources/Expansion.dsres"]];
}
/* install.sh --benchmark: Gas Powered Games' Dungeon Siege Benchmark (Resources/Benchmark.dsres, Maps/BenchmarkMap.dsmap),
 * kept apart from the mods: linked in only for a benchmark run, as it changes the game's content (and so its multiplayer
 * identity) */
static NSString *benchmark_dir(NSString *data) { return [data stringByAppendingPathComponent:@"benchmark"]; }
static BOOL have_benchmark(NSString *data)
{
    NSString *b = benchmark_dir(data); NSFileManager *fm = NSFileManager.defaultManager;
    return [fm fileExistsAtPath:[b stringByAppendingPathComponent:@"Resources/Benchmark.dsres"]] && [fm fileExistsAtPath:[b stringByAppendingPathComponent:@"Maps/BenchmarkMap.dsmap"]];
}
static NSArray<NSDictionary *> *mode_choices(NSString *data)
{
    NSString *ip = join_addresses();
    NSMutableArray *a = [@[@{@"value": @"single", @"label": @"Single Player", @"note": @"The Kingdom of Ehb campaign"},
                           @{@"value": @"multi", @"label": @"Multiplayer", @"note": ip ?: @"LAN and internet games, with Mac and Windows players"}] mutableCopy];
    if (have_expansion(data)) {
        [a addObject:@{@"value": @"loa", @"label": @"Legends of Aranna", @"note": @"The expansion's campaign, with its own saves"}];
        [a addObject:@{@"value": @"loa-multi", @"label": @"Aranna Multiplayer", @"note": ip ?: @"Legends of Aranna's multiplayer maps"}];
    }
    if (have_benchmark(data))
        [a addObject:@{@"value": @"benchmark", @"label": @"Benchmark", @"note": @"GPG's 3-minute demo, run uncapped and without mods"}];
    return a;
}
static NSInteger index_of(NSArray<NSDictionary *> *c, NSString *v, NSInteger dflt)
{
    for (NSUInteger k = 0; k < c.count; k++) if ([c[k][@"value"] isEqualToString:v ?: @""]) return (NSInteger)k;
    return dflt;
}
// ---------------------------------------------------------------- mods
// Mods are the archives in <data>/mods (.dsres, and .dsmap for maps). Each game mode has its own ticked set, kept in
// launcher.plist; at launch the ticked ones are linked into the data folder's view of the game folder (<data>/game/
// Resources and Maps), which the game reads as its own, and the others' links are removed. Real files there are left
// alone. Known mods have names, authors and defaults in the catalog below (credits: docs/MODS.md).
@interface DSBlockTarget : NSObject      /* a button action as a block */
@property (copy) void (^fire)(NSInteger tag);
- (void)clicked:(NSButton *)b;
@end
@implementation DSBlockTarget
- (void)clicked:(NSButton *)b { if (self.fire) self.fire(b.tag); }
@end
enum { MOD_BASE = 1, MOD_LOA = 2 };
typedef struct { const char *id, *name, *author, *about; int games, on; } ModInfo;
static const ModInfo mod_catalog[] = {
    {"yesterhaven", "Yesterhaven", "Gas Powered Games", "Free multiplayer adventure map", MOD_BASE | MOD_LOA, MOD_BASE | MOD_LOA},
    {"fairyfix", "Fairy Fix", "unknown author", "A small fix used alongside Yesterhaven", MOD_BASE | MOD_LOA, MOD_BASE | MOD_LOA},
    {"ikkyo_mpsave_beta_6", "Multiplayer Quest Save (beta 6)", "Jason \"Ikkyo\" Gripp", "Keeps multiplayer quest progress between sessions", MOD_BASE | MOD_LOA, MOD_BASE | MOD_LOA},
    {"sf_resolutionfix", "SeeFar2020 Resolution Fix", "antonior (SeeFar2020, after SeeFar by Jeff Kretz and Irwin Ryan)", "Interface layout for wide resolutions", MOD_BASE | MOD_LOA, MOD_BASE | MOD_LOA},
    {"uberui_loa_v0.02", "UberUI for Legends of Aranna (0.02)", "unknown author", "Extended character screen", MOD_LOA, MOD_LOA},
    {"ds1_difficulty_patch", "DS1 Difficulty Patch", "unknown author", "Rebalanced monsters for Legends of Aranna", MOD_LOA, MOD_LOA},
};
static NSString *mods_dir(NSString *data) { return [data stringByAppendingPathComponent:@"mods"]; }
/* the mods present: id -> files; names and defaults from the catalog, else from the file name */
static NSArray<NSDictionary *> *mods_found(NSString *data)
{
    NSMutableDictionary<NSString *, NSMutableArray *> *files = [NSMutableDictionary dictionary];
    for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:mods_dir(data) error:nil]) {
        NSString *ext = f.pathExtension.lowercaseString; if (![ext isEqualToString:@"dsres"] && ![ext isEqualToString:@"dsmap"]) continue;
        NSString *mid = f.stringByDeletingPathExtension.lowercaseString;
        if (!files[mid]) files[mid] = [NSMutableArray array];
        [files[mid] addObject:f];
    }
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *mid in [files.allKeys sortedArrayUsingSelector:@selector(caseInsensitiveCompare:)]) {
        const ModInfo *m = 0; for (size_t k = 0; k < sizeof mod_catalog / sizeof *mod_catalog; k++) if (!strcmp(mod_catalog[k].id, mid.UTF8String)) m = &mod_catalog[k];
        [out addObject:@{@"id": mid, @"files": files[mid], @"name": m ? @(m->name) : [files[mid][0] stringByDeletingPathExtension],
                         @"author": m ? @(m->author) : @"", @"about": m ? @(m->about) : @"",
                         @"games": @(m ? m->games : MOD_BASE | MOD_LOA), @"on": @(m ? m->on : 0)}];
    }
    return out;
}
static NSString *mods_key(BOOL loa) { return loa ? @"modsAranna" : @"modsDungeonSiege"; }
/* the ticked set for a game: as saved, or the defaults (new mods take their default when first seen) */
static NSMutableSet<NSString *> *mods_enabled(NSDictionary *saved, NSArray<NSDictionary *> *found, BOOL loa)
{
    NSArray *list = saved[mods_key(loa)]; NSArray *known = saved[[mods_key(loa) stringByAppendingString:@"Seen"]] ?: @[];
    NSMutableSet *on = list ? [NSMutableSet setWithArray:list] : [NSMutableSet set];
    for (NSDictionary *m in found) {
        if (!([m[@"games"] intValue] & (loa ? MOD_LOA : MOD_BASE))) { [on removeObject:m[@"id"]]; continue; }
        if ((!list || ![known containsObject:m[@"id"]]) && ([m[@"on"] intValue] & (loa ? MOD_LOA : MOD_BASE))) [on addObject:m[@"id"]];
    }
    return on;
}
/* the tick list: returns NO if cancelled */
static BOOL mods_panel(NSWindow *parent, NSString *data, NSArray<NSDictionary *> *found, BOOL loa, NSMutableSet<NSString *> *on)
{
    NSPanel *pn = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 560, 120) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    pn.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; pn.title = loa ? @"Mods for Legends of Aranna" : @"Mods for Dungeon Siege";
    NSStackView *st = [NSStackView stackViewWithViews:@[]]; st.orientation = NSUserInterfaceLayoutOrientationVertical; st.alignment = NSLayoutAttributeLeading; st.spacing = 6;
    st.edgeInsets = NSEdgeInsetsMake(16, 18, 16, 18);
    NSTextField *hd = [NSTextField wrappingLabelWithString:@"Ticked mods are loaded when this game starts. To play together, everyone needs the same mods. Add others by dropping their .dsres or .dsmap files into the Mods folder."];
    hd.textColor = NSColor.secondaryLabelColor; hd.preferredMaxLayoutWidth = 520; [st addArrangedSubview:hd];
    NSMutableArray<NSButton *> *boxes = [NSMutableArray array]; NSMutableArray *ids = [NSMutableArray array];
    for (NSDictionary *m in found) {
        if (!([m[@"games"] intValue] & (loa ? MOD_LOA : MOD_BASE))) continue;
        NSButton *b = [NSButton checkboxWithTitle:m[@"name"] target:nil action:nil]; b.state = [on containsObject:m[@"id"]] ? NSControlStateValueOn : NSControlStateValueOff;
        b.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
        [st addArrangedSubview:b]; [boxes addObject:b]; [ids addObject:m[@"id"]];
        NSString *sub = [NSString stringWithFormat:@"%@%@%@", [m[@"author"] length] ? m[@"author"] : @"", [m[@"author"] length] && [m[@"about"] length] ? @" — " : @"", m[@"about"]];
        if (!sub.length) sub = [m[@"files"] componentsJoinedByString:@", "];
        NSTextField *l = [NSTextField labelWithString:sub]; l.textColor = NSColor.secondaryLabelColor; l.font = [NSFont systemFontOfSize:11];
        [st addArrangedSubview:l]; [st setCustomSpacing:10 afterView:l];
        [NSLayoutConstraint activateConstraints:@[[l.leadingAnchor constraintEqualToAnchor:b.leadingAnchor constant:20]]];
    }
    if (!boxes.count) { NSTextField *e = [NSTextField labelWithString:@"No mods installed yet."]; [st addArrangedSubview:e]; }
    __block BOOL ok = NO;
    NSButton *folder = [NSButton buttonWithTitle:@"Open Mods Folder" target:nil action:nil];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:nil action:nil], *done = [NSButton buttonWithTitle:@"Done" target:nil action:nil];
    done.keyEquivalent = @"\r"; cancel.keyEquivalent = @"\033";
    NSStackView *btns = [NSStackView stackViewWithViews:@[folder, [NSView new], cancel, done]]; btns.distribution = NSStackViewDistributionFill;
    [st addArrangedSubview:btns]; [NSLayoutConstraint activateConstraints:@[[btns.widthAnchor constraintEqualToConstant:524]]];
    DSBlockTarget *t = [DSBlockTarget new];
    t.fire = ^(NSInteger tag) {
        if (tag == 3) { [NSFileManager.defaultManager createDirectoryAtPath:mods_dir(data) withIntermediateDirectories:YES attributes:nil error:nil];
                        [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:mods_dir(data)]]; return; }
        ok = tag == 1; [NSApp stopModal];
    };
    for (NSButton *b in @[done, cancel, folder]) { b.target = t; b.action = @selector(clicked:); }
    done.tag = 1; cancel.tag = 2; folder.tag = 3;
    pn.contentView = st; [pn setContentSize:st.fittingSize];
    [parent beginSheet:pn completionHandler:nil];
    [NSApp runModalForWindow:pn];
    [parent endSheet:pn]; [pn orderOut:nil];
    if (ok) for (NSUInteger k = 0; k < boxes.count; k++) { if (boxes[k].state == NSControlStateValueOn) [on addObject:ids[k]]; else [on removeObject:ids[k]]; }
    return ok;
}
/* links the ticked mods' files into <data>/game/Resources and Maps and removes the links of the others (DS_MODS: the
 * ticked files, comma separated; set by the launch window, or by hand for tests) */
void ds_mods_sync(const char *data_dir)
{
    const char *e = getenv("DS_MODS"); if (!e) return;
    NSString *data = @(data_dir), *mods = mods_dir(data), *bench = benchmark_dir(data); NSFileManager *fm = NSFileManager.defaultManager;
    BOOL bench_on = getenv("DS_BENCHMARK") && have_benchmark(data);
    NSSet *want = [NSSet setWithArray:[[@(e) lowercaseString] componentsSeparatedByString:@","]];
    for (NSString *sub in @[@"Resources", @"Maps"]) {
        NSString *dir = [[data stringByAppendingPathComponent:@"game"] stringByAppendingPathComponent:sub];
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        for (NSString *f in [fm contentsOfDirectoryAtPath:dir error:nil]) {          /* our links whose mod is not ticked */
            NSString *path = [dir stringByAppendingPathComponent:f], *dest = [fm destinationOfSymbolicLinkAtPath:path error:nil];
            if (dest && [dest hasPrefix:mods] && ![want containsObject:f.lowercaseString]) [fm removeItemAtPath:path error:nil];
            if (dest && [dest hasPrefix:bench] && !bench_on) [fm removeItemAtPath:path error:nil];   /* the benchmark's, after its run */
        }
        if (bench_on) for (NSString *f in [fm contentsOfDirectoryAtPath:[bench stringByAppendingPathComponent:sub] error:nil]) {
            NSString *link = [dir stringByAppendingPathComponent:f];
            if (![fm fileExistsAtPath:link]) [fm createSymbolicLinkAtPath:link withDestinationPath:[[bench stringByAppendingPathComponent:sub] stringByAppendingPathComponent:f] error:nil];
        }
    }
    for (NSString *f in [fm contentsOfDirectoryAtPath:mods error:nil]) {
        if (![want containsObject:f.lowercaseString]) continue;
        NSString *ext = f.pathExtension.lowercaseString, *sub = [ext isEqualToString:@"dsmap"] ? @"Maps" : [ext isEqualToString:@"dsres"] ? @"Resources" : nil;
        if (!sub) continue;
        NSString *link = [[[data stringByAppendingPathComponent:@"game"] stringByAppendingPathComponent:sub] stringByAppendingPathComponent:f];
        if ([fm fileExistsAtPath:link] || [fm destinationOfSymbolicLinkAtPath:link error:nil]) continue;   /* already there (link or a real copy) */
        [fm createSymbolicLinkAtPath:link withDestinationPath:[mods stringByAppendingPathComponent:f] error:nil];
    }
}
static NSString *mods_files(NSArray<NSDictionary *> *found, NSSet<NSString *> *on)
{
    NSMutableArray *a = [NSMutableArray array];
    for (NSDictionary *m in found) if ([on containsObject:m[@"id"]]) [a addObjectsFromArray:m[@"files"]];
    return [a componentsJoinedByString:@","];
}
static NSArray<NSDictionary *> *mods_choice(NSArray<NSDictionary *> *found, NSSet<NSString *> *on, BOOL loa)
{
    NSMutableArray *names = [NSMutableArray array]; NSInteger avail = 0;
    for (NSDictionary *m in found) if ([m[@"games"] intValue] & (loa ? MOD_LOA : MOD_BASE)) { avail++; if ([on containsObject:m[@"id"]]) [names addObject:m[@"name"]]; }
    NSString *label = !avail ? @"None installed" : names.count ? [NSString stringWithFormat:@"%lu of %ld on", (unsigned long)names.count, (long)avail] : @"All off";
    return @[@{@"value": @"mods", @"label": label, @"note": names.count ? [names componentsJoinedByString:@", "] : @"Click to choose"}];
}

/* the chosen settings as environment for the runtime (read later at start-up) */
static void apply(NSString *res, NSString *dist, NSString *fps, NSString *mode, NSString *data, NSString *mods)
{
    BOOL bench = [mode isEqualToString:@"benchmark"] && have_benchmark(data);
    if (bench) {   /* the benchmark's own map, no mods, no frame cap or vertical sync: what this Mac can do */
        setenv("DS_BENCHMARK", "1", 1); setenv("DS_MODS", "", 1);
        const char *old = getenv("DS_ARGS"); NSString *b = @"demo=true map=benchmark_demo teleport=island fpslog=true minfps=0";
        setenv("DS_ARGS", (old && *old ? [NSString stringWithFormat:@"%s %@", old, b] : b).UTF8String, 1);
    }
    if (!getenv("DS_MODS")) setenv("DS_MODS", mods.UTF8String, 1);    /* the ticked mods' files, linked in at start-up */
    if ([mode hasPrefix:@"loa"] && have_expansion(data)) setenv("DS_EXPANSION", expansion_dir(data).fileSystemRepresentation, 1);
    if ([mode hasSuffix:@"multi"]) {   /* the game's own switch for its multiplayer screens */
        const char *old = getenv("DS_ARGS"); NSString *args = old && *old ? [NSString stringWithFormat:@"%s zonematch=true", old] : @"zonematch=true";
        if (!(old && strstr(old, "zonematch"))) setenv("DS_ARGS", args.UTF8String, 1);
    }
    setenv("DS_RESOLUTION", res.UTF8String, 1);
    setenv("DS_DRAW_DISTANCE", dist.UTF8String, 1);
    if ([fps isEqualToString:@"unlimited"] || bench) { setenv("DSR_FPSCAP", "0", 1); setenv("DSR_VSYNC", "0", 1); }
    else setenv("DSR_FPSCAP", fps.UTF8String, 1);
}

// ---------------------------------------------------------------- updates
/* install.sh gives each app an updater (Contents/MacOS/update, passed in DS_UPDATER): it fetches the branch the app
 * follows (main, or nightly for Nightly) from GitHub into a worktree of the checkout the app was installed from and
 * rebuilds the app from it, keeping the game folder, saves, settings and mods. The launcher asks GitHub (in the
 * background, each time it opens) whether that branch moved past the commit the app was built from. */
enum { UPD_NONE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_ERROR };
static void on_main(dispatch_block_t b) { CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, b); CFRunLoopWakeUp(CFRunLoopGetMain()); }
static NSString *short_commit(NSString *c) { return c.length > 7 ? [c substringToIndex:7] : (c ?: @""); }
static NSString *update_check(NSString *updater)   /* the newest commit of the branch on GitHub; nil if it can't be reached */
{
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/bin/bash"]; t.arguments = @[updater, @"--check"];
    NSPipe *p = [NSPipe pipe]; t.standardOutput = p; t.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![t launchAndReturnError:nil]) return nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{ if (t.running) [t terminate]; });
    NSData *d = [p.fileHandleForReading readDataToEndOfFile]; [t waitUntilExit];
    NSString *out = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return t.terminationStatus == 0 && out.length >= 40 ? [out substringToIndex:40] : nil;
}
static NSArray<NSDictionary *> *update_choice(int state, NSString *branch, NSString *built, NSString *latest)
{
    NSString *label = @"Not available", *note = @"This copy wasn't installed from the GitHub repository";
    switch (state) {
    case UPD_CHECKING: label = @"Checking..."; note = [NSString stringWithFormat:@"Asking GitHub for the newest %@", branch]; break;
    case UPD_CURRENT: label = @"Up to date"; note = [NSString stringWithFormat:@"%@ %@", branch, short_commit(built)]; break;
    case UPD_AVAILABLE: label = @"Update available"; note = [NSString stringWithFormat:@"%@ -> %@: press to install", short_commit(built), short_commit(latest)]; break;
    case UPD_ERROR: label = @"Couldn't check"; note = @"GitHub unreachable or not signed in: press to retry"; break;
    }
    return @[@{@"value": @"", @"label": label, @"note": note}];
}
/* the phase of install.sh's output, in words */
static NSString *update_phase(NSString *line, NSString *branch)
{
    if ([line hasPrefix:@"== fetching"]) return @"Downloading the update from GitHub";
    if ([line hasPrefix:[NSString stringWithFormat:@"== %@:", branch]]) return [@"Got " stringByAppendingString:[line substringFromIndex:branch.length + 4]];
    if ([line hasPrefix:@"== analysis"]) return @"Rebuilding from your game: analysing its code (1 of 4)";
    if ([line hasPrefix:@"== lifting"]) return @"Rebuilding from your game: translating to native code (2 of 4)";
    if ([line hasPrefix:@"== compiling"]) return @"Rebuilding from your game: compiling (3 of 4)";
    if ([line hasPrefix:@"== installing"]) return @"Installing the new app (4 of 4)";
    return nil;
}
/* runs the updater with a progress window (modal); returns YES when the app was rebuilt */
static BOOL update_run(NSWindow *parent, NSString *updater, NSString *branch, NSString *logPath)
{
    NSPanel *pn = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 170) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    pn.title = @"Updating";
    NSTextField *title = [NSTextField labelWithString:@"Updating to the newest version"]; title.font = [NSFont boldSystemFontOfSize:14]; title.frame = NSMakeRect(20, 128, 480, 22);
    NSTextField *status = [NSTextField wrappingLabelWithString:@"Starting..."]; status.frame = NSMakeRect(20, 64, 480, 56);
    NSProgressIndicator *spin = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 44, 480, 12)]; spin.indeterminate = YES; [spin startAnimation:nil];
    NSTextField *time = [NSTextField labelWithString:@"This takes a few minutes. Your saves, settings and mods are kept."]; time.font = [NSFont systemFontOfSize:11]; time.textColor = NSColor.secondaryLabelColor; time.frame = NSMakeRect(20, 14, 360, 18);
    NSButton *btn = [NSButton buttonWithTitle:@"Close" target:nil action:nil]; btn.frame = NSMakeRect(400, 8, 100, 30); btn.hidden = YES;
    for (NSView *x in @[title, status, spin, time, btn]) [pn.contentView addSubview:x];
    if (parent) [pn setFrameOrigin:NSMakePoint(NSMidX(parent.frame) - 260, NSMidY(parent.frame) - 85)]; else [pn center];
    [[NSFileManager defaultManager] createFileAtPath:logPath contents:nil attributes:nil];
    NSFileHandle *log = [NSFileHandle fileHandleForWritingAtPath:logPath];
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/bin/bash"]; t.arguments = @[updater];
    NSPipe *pipe = [NSPipe pipe]; t.standardOutput = pipe; t.standardError = pipe;
    NSMutableString *pending = [NSMutableString new], *tail = [NSMutableString new];
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData; if (!d.length) return;
        [log writeData:d];
        NSString *chunk = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
        @synchronized (pending) {
            [pending appendString:chunk];
            NSRange nl;
            while ((nl = [pending rangeOfString:@"\n"]).location != NSNotFound) {
                NSString *line = [pending substringToIndex:nl.location]; [pending deleteCharactersInRange:NSMakeRange(0, nl.location + 1)];
                [tail appendFormat:@"%@\n", line]; if (tail.length > 600) [tail deleteCharactersInRange:NSMakeRange(0, tail.length - 600)];
                NSString *ph = update_phase(line, branch);
                if (ph) on_main(^{ status.stringValue = ph; });
            }
        }
    };
    __block BOOL ok = NO;
    __attribute__((objc_precise_lifetime)) DSBlockTarget *bt = [DSBlockTarget new];   /* a button's target is weak: kept here */
    bt.fire = ^(NSInteger tag) { (void)tag; [NSApp stopModal]; };
    btn.target = bt; btn.action = @selector(clicked:);
    t.terminationHandler = ^(NSTask *task) {
        int code = task.terminationStatus;
        on_main(^{
            pipe.fileHandleForReading.readabilityHandler = nil; [log closeFile];
            [spin stopAnimation:nil]; spin.hidden = YES; btn.hidden = NO;
            if (code == 0) { ok = YES; title.stringValue = @"Updated"; status.stringValue = @"The new version is installed. The app restarts when you press Restart."; btn.title = @"Restart"; }
            else {
                NSString *last; @synchronized (pending) { last = [[tail componentsSeparatedByString:@"\n"] componentsJoinedByString:@" "]; }
                title.stringValue = @"The update didn't finish";
                status.stringValue = [NSString stringWithFormat:@"Nothing was changed if it stopped before installing. Details: %@  (%@)", logPath,
                                      last.length > 160 ? [last substringFromIndex:last.length - 160] : last];
                time.stringValue = @"";
            }
        });
    };
    if (![t launchAndReturnError:nil]) { [log closeFile]; return NO; }
    [pn makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:pn];
    [pn orderOut:nil];
    return ok;
}

/* Shows the launch window (modal) unless DS_NO_LAUNCHER is set; returns 0 to quit. DS_LAUNCHER_SHOT=<png> renders it
 * to an image instead (development). */
int ds_launcher_run(const char *game_dir, const char *data_dir)
{
    NSString *data = @(data_dir), *plistPath = [data stringByAppendingPathComponent:@"launcher.plist"];
    NSDictionary *saved = [NSDictionary dictionaryWithContentsOfFile:plistPath] ?: @{};
    DSRow *res = [DSRow new], *dist = [DSRow new], *fps = [DSRow new], *mode = [DSRow new], *shd = [DSRow new], *she = [DSRow new];
    res.title = @"Resolution"; res.choices = resolution_choices(); res.index = index_of(res.choices, saved[@"resolution"], 0);
    dist.title = @"View Distance"; dist.choices = distance_choices(); dist.index = index_of(dist.choices, saved[@"viewDistance"], default_distance_index());
    fps.title = @"Frame Rate"; fps.choices = framerate_choices(); fps.index = index_of(fps.choices, saved[@"frameRate"], 0);
    shd.title = @"Shadow Detail"; shd.choices = shadow_detail_choices(); shd.index = index_of(shd.choices, saved[@"shadowDetail"], 2);
    she.title = @"Shadow Edges"; she.choices = shadow_edge_choices(); she.index = index_of(she.choices, saved[@"shadowEdges"], 1);
    mode.title = @"Game"; mode.choices = mode_choices(data); mode.index = index_of(mode.choices, saved[@"mode"], 0);
    /* the game's own graphics options, from the chosen game's prefs.gas (Legends of Aranna keeps its own) */
    DSRow *gsh = [DSRow new], *flt = [DSRow new], *det = [DSRow new], *gam = [DSRow new];
    gsh.title = @"Shadows"; flt.title = @"Texture Filtering"; det.title = @"Object Detail"; gam.title = @"Gamma";
    NSArray *prefRows = @[gsh, flt, det, gam], *prefKeys = @[@"video_shadows", @"texture_filtering", @"object_detail_level", @"video_gamma"];
    NSArray *(^prefChoices)(NSInteger) = ^NSArray *(NSInteger k) { return k == 0 ? game_shadow_choices() : k == 1 ? filtering_choices() : k == 2 ? detail_choices() : gamma_choices(); };
    NSArray<NSNumber *> *prefDefault = @[@2, @0, @4, @5];           /* as the game shipped (config/options.gas) */
    NSMutableDictionary<NSNumber *, NSMutableDictionary *> *prefRead = [NSMutableDictionary dictionary], *prefNow = [NSMutableDictionary dictionary];
    void (^prefsLoad)(BOOL) = ^(BOOL loa) {
        if (!prefRead[@(loa)]) {
            NSMutableDictionary *r = [NSMutableDictionary dictionary]; NSString *pp = prefs_path(data, loa);
            for (NSInteger k = 0; k < 4; k++) { NSString *v = prefs_get(pp, prefKeys[k]); if (v) r[prefKeys[k]] = v; }
            prefRead[@(loa)] = r; prefNow[@(loa)] = [r mutableCopy];
        }
        for (NSInteger k = 0; k < 4; k++) {
            DSRow *row = prefRows[k]; row.choices = prefChoices(k);
            row.index = pref_index(row, prefNow[@(loa)][prefKeys[k]], prefDefault[k].integerValue);
        }
    };
    void (^prefsKeep)(BOOL) = ^(BOOL loa) {                         /* the rows' values, kept for that game */
        for (NSInteger k = 0; k < 4; k++) { DSRow *row = prefRows[k]; prefNow[@(loa)][prefKeys[k]] = row.choices[row.index][@"value"]; }
    };
    void (^prefsSave)(void) = ^{                                    /* only what was changed; a new player's file is left to the game */
        for (NSNumber *loa in prefNow) {
            NSMutableDictionary *ch = [NSMutableDictionary dictionary];
            for (NSString *key in prefKeys) {
                NSString *now = prefNow[loa][key], *was = prefRead[loa][key];
                NSInteger k = (NSInteger)[prefKeys indexOfObject:key];
                if (!was) was = prefChoices(k)[prefDefault[k].integerValue][@"value"];
                if (now && ![now isEqualToString:was]) ch[key] = now;
            }
            if (ch.count) prefs_set(prefs_path(data, loa.boolValue), ch);
        }
    };
    NSArray<NSDictionary *> *found = mods_found(data);
    NSMutableSet *onBase = mods_enabled(saved, found, NO), *onLoa = mods_enabled(saved, found, YES);
    BOOL (^isLoa)(void) = ^BOOL { return [mode.choices[mode.index][@"value"] hasPrefix:@"loa"]; };
    BOOL (^isBench)(void) = ^BOOL { return [mode.choices[mode.index][@"value"] isEqualToString:@"benchmark"]; };
    NSArray *(^modsRow)(void) = ^NSArray * { return isBench() ? @[@{@"value": @"mods", @"label": @"Not used", @"note": @"The benchmark runs without mods"}]
                                                              : mods_choice(found, isLoa() ? onLoa : onBase, isLoa()); };
    DSRow *mods = [DSRow new]; mods.title = @"Mods"; mods.choices = modsRow();
    NSString *(^modFiles)(void) = ^NSString * { return mods_files(found, isLoa() ? onLoa : onBase); };
    __block BOOL prefsLoa = isLoa(); prefsLoad(prefsLoa);
    const char *shot = getenv("DS_LAUNCHER_SHOT");
    if (getenv("DS_NO_LAUNCHER") && !shot) {
        if (saved.count) { apply(res.choices[res.index][@"value"], dist.choices[dist.index][@"value"], fps.choices[fps.index][@"value"], mode.choices[mode.index][@"value"], data, modFiles());
                           apply_shadows(shd.choices[shd.index][@"value"], she.choices[she.index][@"value"]); }
        return 1;
    }

    NSString *updater = getenv("DS_UPDATER") ? @(getenv("DS_UPDATER")) : nil, *branch = getenv("DS_UPDATE_BRANCH") ? @(getenv("DS_UPDATE_BRANCH")) : @"main";
    NSString *built = getenv("DS_BUILT_COMMIT") ? @(getenv("DS_BUILT_COMMIT")) : @"";
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:updater ?: @""]) updater = nil;
    DSRow *upd = [DSRow new]; upd.title = @"Updates";
    __block int ustate = updater ? UPD_CHECKING : UPD_NONE; __block NSString *latest = nil;
    upd.choices = update_choice(ustate, branch, built, nil);
    BOOL shotMode = shot != NULL;
    shd.title = @"Shadow Detail"; she.title = @"Shadow Edges";
    DSSection *display = [DSSection new], *graphics = [DSSection new], *modsSec = [DSSection new], *updSec = [DSSection new];
    display.title = @"Display"; display.rows = @[res, fps, dist, gam];
    graphics.title = @"Graphics"; graphics.rows = @[flt, det, gsh, shd, she];
    modsSec.title = @"Mods"; modsSec.summary = ^NSString * { return mods.choices[mods.index][@"label"]; };
    updSec.title = @"Updates"; updSec.summary = ^NSString * { NSDictionary *c = upd.choices[upd.index]; return [c[@"note"] length] ? [NSString stringWithFormat:@"%@ · %@", c[@"label"], c[@"note"]] : c[@"label"]; };
    DSLaunchView *v = [[DSLaunchView alloc] initWithFrame:NSMakeRect(0, 0, 780, 760)];
    v.top = @[mode]; v.sections = @[display, graphics, modsSec, updSec]; v.open = -1; v.hover = v.pressed = HIT_NONE;
    __weak DSLaunchView *wv = v; __weak DSRow *wu = upd;
    void (^check)(void) = ^{
        if (!updater) return;
        ustate = UPD_CHECKING; wu.choices = update_choice(ustate, branch, built, nil); wv.needsDisplay = YES;
        void (^done)(NSString *) = ^(NSString *l) {
            latest = l; ustate = !l ? UPD_ERROR : [l isEqualToString:built] ? UPD_CURRENT : UPD_AVAILABLE;
            wu.choices = update_choice(ustate, branch, built, l); wv.needsDisplay = YES;
        };
        if (shotMode) { done(update_check(updater)); return; }               /* development: the result in the image */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ NSString *l = update_check(updater); on_main(^{ done(l); }); });
    };
    check();
    upd.activate = ^{
        if (ustate == UPD_CHECKING) return;
        if (ustate != UPD_AVAILABLE) { check(); return; }
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"Update to the newest %@?", [branch isEqualToString:@"nightly"] ? @"Nightly" : @"version"];
        a.informativeText = [NSString stringWithFormat:@"The %@ branch (%@) is downloaded from GitHub and the app is rebuilt from your own copy of the game, "
                             "which takes a few minutes. Your game folder, saves, settings and mods are kept. The app restarts when it is done.", branch, short_commit(latest)];
        [a addButtonWithTitle:@"Update"]; [a addButtonWithTitle:@"Cancel"];
        if ([a runModal] != NSAlertFirstButtonReturn) return;
        NSString *logPath = [data stringByAppendingPathComponent:@"update.log"];
        if (update_run(wv.window, updater, branch, logPath)) {   /* restart: the new app, then this one ends */
            NSString *app = [[[updater stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
            NSTask *o = [NSTask new]; o.executableURL = [NSURL fileURLWithPath:@"/usr/bin/open"]; o.arguments = @[@"-n", app];
            [o launchAndReturnError:nil]; [o waitUntilExit];
            exit(0);
        }
        check();
    };
    v.onChange = ^{                                                 /* the Game row decides which mods and which prefs.gas */
        mods.choices = modsRow();
        prefsKeep(prefsLoa);
        if (isLoa() != prefsLoa) { prefsLoa = isLoa(); prefsLoad(prefsLoa); }
    };
    __weak DSRow *wm = mods;
    modsSec.activate = ^{ if (wm.activate) wm.activate(); };
    updSec.activate = ^{ if (wu.activate) wu.activate(); };
    mods.activate = ^{
        if (isBench()) return;
        if (mods_panel(wv.window, data, found, isLoa(), isLoa() ? onLoa : onBase)) wm.choices = modsRow();
    };
    NSString *tank = [@(game_dir) stringByAppendingPathComponent:@"Resources/Objects.dsres"], *m = @"art/bitmaps/gui/front_end/menus/main/b_gui_fe_m_mn_3d_";
    CGImageRef stone = raw_image(tank_read(tank, [m stringByAppendingString:@"background-05.raw"]));
    CGImageRef bars = raw_image(tank_read(tank, [m stringByAppendingString:@"menubars.raw"]));
    if (stone) { v.stone = nsimg(stone, CGRectZero); CGImageRelease(stone); }
    if (bars) { v.plaque = nsimg(bars, CGRectMake(66, 14, 168, 62)); CGImageRelease(bars); }   /* the leather of the main menu's title plaque */
    NSArray *woods = @[@"button_wood_up.raw", @"button_wood_hov.raw", @"button_wood_down.raw"]; NSImage *wi[3] = {nil, nil, nil};
    for (int k = 0; k < 3; k++) { CGImageRef w = raw_image(tank_read(tank, [m stringByAppendingString:woods[k]])); if (w) { wi[k] = nsimg(w, opaque_bounds(w)); CGImageRelease(w); } }
    v.wood = wi[0]; v.woodHover = wi[1]; v.woodDown = wi[2];
    BOOL seefar = NO;
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:[@(game_dir) stringByAppendingPathComponent:@"Resources"] error:nil])
        if ([f.lowercaseString hasPrefix:@"sf_seefar"]) seefar = YES;
    NSImage *banner = [[NSImage alloc] initWithContentsOfFile:[data stringByAppendingPathComponent:@"art/banner.jpg"]];   /* install.sh: the game's key art */
    if (banner.size.width > 400) v.banner = banner;
    if (seefar) v.note = @"The SeeFar mod is installed: it is used at Original view distance, and set aside when a farther distance is chosen.";
    if (shot) {   /* development: the window as an image (DS_LAUNCHER_PAGE: a group opened) */
        if (getenv("DS_LAUNCHER_PAGE")) v.open = atoi(getenv("DS_LAUNCHER_PAGE"));
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds]; [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(shot) atomically:YES];
        return 0;
    }

    NSWindow *w = [[NSWindow alloc] initWithContentRect:v.frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO;   /* ARC owns it: AppKit's default would release it again when the close button closes it (a crash on exit) */
    w.titlebarAppearsTransparent = YES; w.titleVisibility = NSWindowTitleHidden; w.movableByWindowBackground = YES; w.title = @"Dungeon Siege";
    w.backgroundColor = NSColor.blackColor; w.contentView = v; [w center]; [w makeFirstResponder:v];
    __block int result = 0;
    v.onPlay = ^{ result = 1; [NSApp stopModal]; };
    v.onQuit = ^{ result = 0; [NSApp stopModal]; };
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:w queue:nil usingBlock:^(NSNotification *n) { (void)n; [NSApp stopModal]; }];
    [NSApp activateIgnoringOtherApps:YES];
    [w makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:w];
    [w orderOut:nil];
    if (!result) return 0;
    NSString *rv = res.choices[res.index][@"value"], *dv = dist.choices[dist.index][@"value"], *fv = fps.choices[fps.index][@"value"], *mv = mode.choices[mode.index][@"value"];
    NSMutableArray *seenBase = [NSMutableArray array], *seenLoa = [NSMutableArray array];
    for (NSDictionary *m in found) { if ([m[@"games"] intValue] & MOD_BASE) [seenBase addObject:m[@"id"]]; if ([m[@"games"] intValue] & MOD_LOA) [seenLoa addObject:m[@"id"]]; }
    NSString *sdv = shd.choices[shd.index][@"value"], *sev = she.choices[she.index][@"value"];
    prefsKeep(prefsLoa); prefsSave();
    [@{@"resolution": rv, @"viewDistance": dv, @"frameRate": fv, @"mode": mv, @"shadowDetail": sdv, @"shadowEdges": sev,
       mods_key(NO): onBase.allObjects, mods_key(YES): onLoa.allObjects,
       [mods_key(NO) stringByAppendingString:@"Seen"]: seenBase, [mods_key(YES) stringByAppendingString:@"Seen"]: seenLoa} writeToFile:plistPath atomically:YES];
    apply(rv, dv, fv, mv, data, modFiles()); apply_shadows(sdv, sev);
    return 1;
}
