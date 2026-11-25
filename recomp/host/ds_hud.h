// Action Mode overlay (included by main.m): the ability bar along the bottom of the screen and short messages in the
// middle, drawn by Core Animation over the game's Metal layer from what the game side publishes in action_hud
// (runtime/win32/action.c). Nothing is drawn with Action Mode off, outside the world, or while a game window is open.
#import <QuartzCore/QuartzCore.h>

static CALayer *hud_root, *hud_tile[ACTION_SLOTS], *hud_icon[ACTION_SLOTS], *hud_cool[ACTION_SLOTS];
static CATextLayer *hud_key[ACTION_SLOTS], *hud_name[ACTION_SLOTS], *hud_toast;
static CGColorRef hud_rgb(double r, double g, double b, double a) { return [NSColor colorWithSRGBRed:r green:g blue:b alpha:a].CGColor; }
static NSFont *hud_font(double size, int bold)
{
    NSFont *f = [NSFont fontWithName:bold ? @"Palatino-Bold" : @"Palatino-Roman" size:size];
    return f ?: [NSFont boldSystemFontOfSize:size];
}
static CATextLayer *hud_text(double size, int bold, CGColorRef color)
{
    CATextLayer *t = [CATextLayer layer]; NSFont *f = hud_font(size, bold);
    t.font = (__bridge CFTypeRef)f; t.fontSize = size; t.foregroundColor = color; t.alignmentMode = kCAAlignmentCenter;
    t.contentsScale = 2; t.shadowOpacity = 0.9; t.shadowRadius = 2; t.shadowOffset = CGSizeMake(0, -1); t.shadowColor = hud_rgb(0, 0, 0, 1);
    return t;
}
// an SF Symbol for each ability, drawn in old gold
static NSString *hud_symbol(const char *name)
{
    static const struct { const char *name, *symbol; } m[] = {
        {"Dodge", "wind"}, {"Cleave", "burst.fill"}, {"Volley", "arrow.up.forward"}, {"Nova", "sparkles"}, {"Second Wind", "heart.fill"},
        {"Charge", "bolt.horizontal.fill"}, {"Power Shot", "scope"}, {"Chain Lightning", "bolt.fill"}, {"Rally", "flag.fill"}};
    for (size_t k = 0; k < sizeof m / sizeof *m; k++) if (!strcmp(m[k].name, name)) return @(m[k].symbol);
    return @"circle.fill";
}
static id hud_icon_image(const char *name, int ready)
{
    NSImage *img = [NSImage imageWithSystemSymbolName:hud_symbol(name) accessibilityDescription:nil];
    if (!img) return nil;
    NSColor *tint = ready ? [NSColor colorWithSRGBRed:0.93 green:0.80 blue:0.50 alpha:1] : [NSColor colorWithSRGBRed:0.55 green:0.50 blue:0.42 alpha:1];
    NSImageSymbolConfiguration *cfg = [[NSImageSymbolConfiguration configurationWithPointSize:64 weight:NSFontWeightSemibold]
                                       configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithHierarchicalColor:tint]];
    img = [img imageWithSymbolConfiguration:cfg];
    return [img layerContentsForContentsScale:2];
}
static void hud_build(NSView *v)
{
    hud_root = [CALayer layer]; hud_root.zPosition = 100; hud_root.hidden = YES;
    for (int i = 0; i < ACTION_SLOTS; i++) {
        CALayer *t = [CALayer layer];
        t.backgroundColor = hud_rgb(0.07, 0.06, 0.05, 0.84); t.cornerRadius = 7; t.borderWidth = 2; t.borderColor = hud_rgb(0.62, 0.47, 0.25, 1);
        t.shadowOpacity = 0.7; t.shadowRadius = 4; t.shadowOffset = CGSizeMake(0, -2); t.masksToBounds = NO;
        CALayer *ic = [CALayer layer]; ic.contentsGravity = kCAGravityResizeAspect; [t addSublayer:ic];
        CALayer *cd = [CALayer layer]; cd.backgroundColor = hud_rgb(0, 0, 0, 0.62); cd.cornerRadius = 5; [t addSublayer:cd];
        CATextLayer *k = hud_text(11, 1, hud_rgb(1, 0.93, 0.75, 1)); [t addSublayer:k];
        CATextLayer *n = hud_text(11, 0, hud_rgb(0.92, 0.86, 0.72, 1)); [t addSublayer:n];
        [hud_root addSublayer:t];
        hud_tile[i] = t; hud_icon[i] = ic; hud_cool[i] = cd; hud_key[i] = k; hud_name[i] = n;
    }
    hud_toast = hud_text(22, 1, hud_rgb(1, 0.9, 0.62, 1)); hud_toast.opacity = 0; [hud_root addSublayer:hud_toast];
    [v.layer addSublayer:hud_root];
}
static void hud_update(NSView *v)
{
    if (!action_on() || !v.layer) return;
    if (!hud_root) hud_build(v);
    static uint32_t seq = ~0u; static int was_pad = -1; static CGSize was_size;
    int show = action_hud.in_game && !action_hud.menu_open, pad = action_pad.connected;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    hud_root.hidden = !show;
    CGSize sz = v.bounds.size; hud_root.frame = v.bounds;
    double tile = fmin(fmax(sz.height * 0.058, 40), 70), gap = tile * 0.22;
    int n = 0; for (int i = 0; i < ACTION_SLOTS; i++) if (action_hud.slot[i].name[0]) n++;
    double x0 = (sz.width - (n * tile + (n - 1) * gap)) / 2, y0 = tile * 0.6;
    int changed = seq != action_hud.seq || was_pad != pad || !CGSizeEqualToSize(was_size, sz);
    for (int i = 0, k = 0; i < ACTION_SLOTS; i++) {
        volatile typeof(action_hud.slot[0]) *s = &action_hud.slot[i];
        CALayer *t = hud_tile[i];
        if (!s->name[0]) { t.hidden = YES; continue; }
        t.hidden = NO; t.frame = CGRectMake(x0 + k * (tile + gap), y0, tile, tile); k++;
        double cd = s->cooldown < 0 ? 0 : s->cooldown > 1 ? 1 : s->cooldown;
        hud_cool[i].frame = CGRectMake(2, 2, tile - 4, (tile - 4) * cd);
        t.borderColor = s->ready ? hud_rgb(0.80, 0.62, 0.30, 1) : hud_rgb(0.40, 0.32, 0.22, 1);
        if (changed) {
            char nm[sizeof s->name]; memcpy(nm, (const void *)s->name, sizeof nm); nm[sizeof nm - 1] = 0;
            hud_icon[i].frame = CGRectInset(CGRectMake(0, 0, tile, tile), tile * 0.2, tile * 0.2);
            hud_icon[i].contents = hud_icon_image(nm, 1);
            static const char *pad_keys[ACTION_SLOTS] = {"B", "X", "Y", "RB", "RT", "LS"};
            char key[16]; if (pad) snprintf(key, sizeof key, "%s", pad_keys[i]); else { memcpy(key, (const void *)s->key, sizeof s->key); key[sizeof s->key - 1] = 0; }
            hud_key[i].string = @(key); hud_key[i].fontSize = tile * 0.22;               // under the tile
            hud_key[i].frame = CGRectMake(-tile * 0.25, -tile * 0.34, tile * 1.5, tile * 0.3);
            hud_name[i].string = @(nm); hud_name[i].fontSize = tile * 0.2;
            hud_name[i].frame = CGRectMake(-tile * 0.5, tile + 2, tile * 2, tile * 0.28);
        }
    }
    // the message: shown while it is current, fading over its last half second
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double mono = ts.tv_sec + ts.tv_nsec / 1e9, left = action_hud.toast_until - mono;
    if (changed) { char m[sizeof action_hud.toast]; memcpy(m, (const void *)action_hud.toast, sizeof m); m[sizeof m - 1] = 0; hud_toast.string = @(m); }
    hud_toast.fontSize = fmax(18, sz.height * 0.028);
    hud_toast.frame = CGRectMake(0, sz.height * 0.70, sz.width, hud_toast.fontSize * 1.4);
    hud_toast.opacity = left <= 0 ? 0 : left < 0.5 ? left / 0.5 : 1;
    [CATransaction commit];
    seq = action_hud.seq; was_pad = pad; was_size = sz;
}
static void hud_start(NSView *v)
{
    static dispatch_source_t t;
    if (t || !action_on()) return;
    t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(t, DISPATCH_TIME_NOW, 33 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{ hud_update(v); });
    dispatch_resume(t);
}
// test mode: the overlay alone, rendered into a PNG ("hudshot:<path>"), over a dark backdrop so the tiles read
static void hud_shot(NSView *v, const char *path)
{
    if (!hud_root) { fprintf(stderr, "DungeonSiegeNative: hudshot: no overlay\n"); return; }
    CGSize sz = v.bounds.size; size_t w = (size_t)sz.width * 2, h = (size_t)sz.height * 2;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextSetRGBFillColor(cg, 0.25, 0.3, 0.2, 1); CGContextFillRect(cg, CGRectMake(0, 0, w, h));
    CGContextScaleCTM(cg, 2, 2);
    BOOL hidden = hud_root.hidden; hud_root.hidden = NO; [hud_root renderInContext:cg]; hud_root.hidden = hidden;
    CGImageRef img = CGBitmapContextCreateImage(cg);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES];
    CGImageRelease(img); CGContextRelease(cg); CGColorSpaceRelease(cs);
    fprintf(stderr, "DungeonSiegeNative: overlay saved to %s\n", path);
}
