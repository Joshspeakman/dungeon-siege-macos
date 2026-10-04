// The launch window: resolution, view distance, frame rate and single player or multiplayer, remembered in
// <data>/launcher.plist, then Play.
// Its look comes from the game itself, read at launch from the player's own Resources/Objects.dsres (the main menu's
// stone wall, leather plaque, brass trim and wooden buttons) and set in Copperplate, the typeface of the game's UI.
// Nothing from the game is stored in this project; without the archive the window falls back to plain colours.
#import <AppKit/AppKit.h>
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
@interface DSLaunchView : NSView
@property NSArray<DSRow *> *rows; @property NSInteger focus; @property NSInteger hover; @property NSInteger pressed;
@property NSImage *stone, *plaque, *trim, *wood, *woodHover, *woodDown; @property NSString *note;
@property (copy) void (^onPlay)(void); @property (copy) void (^onQuit)(void);
@end

enum { HIT_NONE = -1, HIT_PLAY = 100, HIT_QUIT = 101 };   /* rows: 10*row + 0 (left arrow) / 1 (value) / 2 (right arrow) */

@implementation DSLaunchView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (NSRect)rowRect:(NSInteger)k { return NSMakeRect(70, 196 + 74 * k, self.bounds.size.width - 140, 64); }
- (NSRect)arrow:(NSInteger)k right:(BOOL)right { NSRect r = [self rowRect:k]; return NSMakeRect(right ? NSMaxX(r) - 34 : NSMaxX(r) - 330, r.origin.y + 6, 30, 30); }
- (NSRect)valueRect:(NSInteger)k { NSRect r = [self rowRect:k]; return NSMakeRect(NSMaxX(r) - 300, r.origin.y + 9, 262, 30); }
- (NSRect)playRect { NSRect b = self.bounds; return NSMakeRect(NSMidX(b) + 12, b.size.height - 84, 220, 56); }
- (NSRect)quitRect { NSRect b = self.bounds; return NSMakeRect(NSMidX(b) - 232, b.size.height - 84, 220, 56); }
- (NSInteger)hit:(NSPoint)p
{
    if (NSPointInRect(p, self.playRect)) return HIT_PLAY;
    if (NSPointInRect(p, self.quitRect)) return HIT_QUIT;
    for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
        if (NSPointInRect(p, [self arrow:k right:NO])) return 10 * k;
        if (NSPointInRect(p, [self arrow:k right:YES])) return 10 * k + 2;
        if (NSPointInRect(p, [self valueRect:k])) return 10 * k + 1;
    }
    return HIT_NONE;
}
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
    NSInteger k = h / 10; self.focus = k; [self step:k by:(h % 10 == 0) ? -1 : 1];
}
- (void)step:(NSInteger)k by:(NSInteger)dir
{
    DSRow *r = self.rows[k]; NSInteger n = (NSInteger)r.choices.count;
    r.index = (r.index + dir + n) % n; self.needsDisplay = YES;
}
- (void)keyDown:(NSEvent *)e
{
    switch (e.keyCode) {
    case 0x24: case 0x4c: if (self.onPlay) self.onPlay(); return;      /* Return, Enter */
    case 0x35: if (self.onQuit) self.onQuit(); return;                  /* Escape */
    case 0x7e: self.focus = (self.focus + (NSInteger)self.rows.count - 1) % (NSInteger)self.rows.count; self.needsDisplay = YES; return;
    case 0x7d: self.focus = (self.focus + 1) % (NSInteger)self.rows.count; self.needsDisplay = YES; return;
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
    draw_text(title, cp(primary ? 24 : 20, YES), c, NSMakeRect(r.origin.x, NSMidY(r) - (primary ? 15 : 13), r.size.width, 32), NSTextAlignmentCenter, sh);
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
- (void)drawRect:(NSRect)dirty
{
    NSRect b = self.bounds;
    /* stone wall */
    if (self.stone) { [[NSColor colorWithPatternImage:self.stone] setFill]; NSRectFill(b); } else { [[NSColor colorWithWhite:0.11 alpha:1] setFill]; NSRectFill(b); }
    NSGradient *vig = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0.15], 0.0, [NSColor colorWithWhite:0 alpha:0.62], 1.0, nil];
    [vig drawInRect:b relativeCenterPosition:NSMakePoint(0, 0.1)];
    /* title plaque */
    NSRect pr = NSMakeRect(NSMidX(b) - 290, 22, 580, 128);
    if (self.plaque) [self.plaque drawInRect:NSInsetRect(pr, 6, 6) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    else { [[NSColor colorWithSRGBRed:0.42 green:0.25 blue:0.15 alpha:1] setFill]; NSRectFill(NSInsetRect(pr, 6, 6)); }
    [self drawIronFrame:pr];
    [self drawTitle:NSMakeRect(pr.origin.x, pr.origin.y + 6, pr.size.width, pr.size.height - 30)];
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.9]; sh.shadowOffset = NSMakeSize(0, -1); sh.shadowBlurRadius = 2;
    draw_text(@"NATIVE EDITION FOR macOS", cp(13, NO), gold(), NSMakeRect(pr.origin.x, NSMaxY(pr) - 34, pr.size.width, 18), NSTextAlignmentCenter, sh);
    /* the settings */
    NSRect panel = NSMakeRect(54, 176, b.size.width - 108, 74 * self.rows.count + 30);
    [[NSColor colorWithWhite:0 alpha:0.5] setFill]; [[NSBezierPath bezierPathWithRoundedRect:panel xRadius:4 yRadius:4] fill];
    [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.9] setStroke]; NSBezierPath *bp = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(panel, 0.5, 0.5) xRadius:4 yRadius:4]; bp.lineWidth = 1.5; [bp stroke];
    for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
        DSRow *row = self.rows[k]; NSRect r = [self rowRect:k]; NSDictionary *ch = row.choices[row.index];
        if (k == self.focus) { [[NSColor colorWithSRGBRed:0.86 green:0.6 blue:0.25 alpha:0.12] setFill]; [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, -10, -2) xRadius:3 yRadius:3] fill]; }
        draw_text(row.title.uppercaseString, cp(19, YES), gold(), NSMakeRect(r.origin.x, r.origin.y + 8, 260, 28), NSTextAlignmentLeft, sh);
        [self drawArrow:[self arrow:k right:NO] right:NO lit:self.hover == 10 * k];
        [self drawArrow:[self arrow:k right:YES] right:YES lit:self.hover == 10 * k + 2];
        draw_text(ch[@"label"], cp(18, NO), self.hover == 10 * k + 1 ? NSColor.whiteColor : parchment(), [self valueRect:k], NSTextAlignmentCenter, sh);
        draw_text(ch[@"note"] ?: @"", cp(11, NO), dim(), NSMakeRect(NSMaxX(r) - 330, r.origin.y + 42, 330, 18), NSTextAlignmentCenter, nil);
        if (k + 1 < (NSInteger)self.rows.count) { [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.35] setFill]; NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) + 5, r.size.width, 1)); }
    }
    if (self.note.length) draw_text(self.note, cp(11, NO), dim(), NSMakeRect(54, NSMaxY(panel) + 10, b.size.width - 108, 30), NSTextAlignmentCenter, nil);
    [self drawButton:self.quitRect title:@"QUIT" code:HIT_QUIT primary:NO];
    [self drawButton:self.playRect title:@"PLAY" code:HIT_PLAY primary:YES];
}
@end

// ---------------------------------------------------------------- choices, settings file, the window
static NSArray<NSDictionary *> *resolution_choices(void)
{
    NSScreen *s = NSScreen.mainScreen; int W = (int)s.frame.size.width, H = (int)s.frame.size.height; double sc = s.backingScaleFactor;
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
static NSArray<NSDictionary *> *mode_choices(void)
{
    NSString *ip = join_addresses();
    return @[@{@"value": @"single", @"label": @"Single Player", @"note": @"The Kingdom of Ehb campaign"},
             @{@"value": @"multi", @"label": @"Multiplayer", @"note": ip ?: @"LAN and internet games, with Mac and Windows players"}];
}
static NSInteger index_of(NSArray<NSDictionary *> *c, NSString *v, NSInteger dflt)
{
    for (NSUInteger k = 0; k < c.count; k++) if ([c[k][@"value"] isEqualToString:v ?: @""]) return (NSInteger)k;
    return dflt;
}
/* the chosen settings as environment for the runtime (read later at start-up) */
static void apply(NSString *res, NSString *dist, NSString *fps, NSString *mode)
{
    if ([mode isEqualToString:@"multi"]) {   /* the game's own switch for its multiplayer screens */
        const char *old = getenv("DS_ARGS"); NSString *args = old && *old ? [NSString stringWithFormat:@"%s zonematch=true", old] : @"zonematch=true";
        if (!(old && strstr(old, "zonematch"))) setenv("DS_ARGS", args.UTF8String, 1);
    }
    setenv("DS_RESOLUTION", res.UTF8String, 1);
    setenv("DS_DRAW_DISTANCE", dist.UTF8String, 1);
    if ([fps isEqualToString:@"unlimited"]) { setenv("DSR_FPSCAP", "0", 1); setenv("DSR_VSYNC", "0", 1); }
    else setenv("DSR_FPSCAP", fps.UTF8String, 1);
}

/* Shows the launch window (modal) unless DS_NO_LAUNCHER is set; returns 0 to quit. DS_LAUNCHER_SHOT=<png> renders it
 * to an image instead (development). */
int ds_launcher_run(const char *game_dir, const char *data_dir)
{
    NSString *data = @(data_dir), *plistPath = [data stringByAppendingPathComponent:@"launcher.plist"];
    NSDictionary *saved = [NSDictionary dictionaryWithContentsOfFile:plistPath] ?: @{};
    DSRow *res = [DSRow new], *dist = [DSRow new], *fps = [DSRow new], *mode = [DSRow new];
    res.title = @"Resolution"; res.choices = resolution_choices(); res.index = index_of(res.choices, saved[@"resolution"], 0);
    dist.title = @"View Distance"; dist.choices = distance_choices(); dist.index = index_of(dist.choices, saved[@"viewDistance"], 2);
    fps.title = @"Frame Rate"; fps.choices = framerate_choices(); fps.index = index_of(fps.choices, saved[@"frameRate"], 0);
    mode.title = @"Game"; mode.choices = mode_choices(); mode.index = index_of(mode.choices, saved[@"mode"], 0);
    const char *shot = getenv("DS_LAUNCHER_SHOT");
    if (getenv("DS_NO_LAUNCHER") && !shot) {
        if (saved.count) apply(res.choices[res.index][@"value"], dist.choices[dist.index][@"value"], fps.choices[fps.index][@"value"], mode.choices[mode.index][@"value"]);
        return 1;
    }

    DSLaunchView *v = [[DSLaunchView alloc] initWithFrame:NSMakeRect(0, 0, 780, 626)];
    v.rows = @[mode, res, dist, fps]; v.hover = v.pressed = HIT_NONE;
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
    if (seefar) v.note = @"The SeeFar mod is installed: it is used at Original view distance, and set aside when a farther distance is chosen.";
    if (shot) {   /* development: the window as an image */
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds]; [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(shot) atomically:YES];
        return 0;
    }

    NSWindow *w = [[NSWindow alloc] initWithContentRect:v.frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered defer:NO];
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
    [@{@"resolution": rv, @"viewDistance": dv, @"frameRate": fv, @"mode": mv} writeToFile:plistPath atomically:YES];
    apply(rv, dv, fv, mv);
    return 1;
}
