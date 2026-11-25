// Game controller for Action Mode (DS_ACTION=1; included by main.m after its input functions).
//
// The controller is read 250 times a second on the main thread, where the keyboard and mouse come in too. Its raw
// state goes to the game side (runtime/win32/action.c: steering, attack, dodge, abilities); the rest is played through
// the keyboard and mouse paths the game already reads:
//
//   in the world                                  in menus, the inventory and the front end
//   left stick    walk (action.c)                 left or right stick   pointer
//   right stick   pointer                         A                     click
//   A             attack / talk / pick up         X                     right click
//   B             dodge                           B, Menu               back (Esc)
//   X, Y, RB, RT  abilities 1-4                   View                  inventory (in the game)
//   LT            health potion (H)
//   LB            mana potion (M)
//   D-pad         left/right: previous/next hero (, .)  up: quick weapon select (Q)  down: collect loot (Z)
//   View          inventory (I)      Menu  options (Esc)      L3  map (Tab)
//   R3            right stick: pointer <-> camera (turn left/right, zoom)
//
// Which column applies is the game side's call (action_hud.in_game / menu_open). Test mode: the "stick:x,y" and
// "pad:buttons,frames" script commands stand in for a controller (x, y in percent; buttons as action PAD_* bits).
#import <GameController/GameController.h>

static void pad_key(int vk, int down)                   // through the keyboard's own path, as the key with that VK
{
    for (uint16_t k = 0; k < 128; k++) if (keymap[k].vk == vk) { key_event(k, down, 0); return; }
}
static void pad_button(int right, int down)
{
    uint32_t bit = right ? 2 : 1;
    if (down) { buttons |= bit; w32_keys[right ? 2 : 1] = 0x80; post(right ? 0x204 : 0x201, mk(), pos_lp()); }
    else { buttons &= ~bit; w32_keys[right ? 2 : 1] = 0; post(right ? 0x205 : 0x202, mk(), pos_lp()); }
}
static float pad_curve(float v) { float a = fabsf(v); if (a < 0.15f) return 0; a = (a - 0.15f) / 0.85f; return copysignf(a * a, v); }
static void pad_tick(GCExtendedGamepad *g, double dt)
{
    ActionPad s = {0};
    if (g && NSApp.active) {
        s.connected = 1;
        s.lx = g.leftThumbstick.xAxis.value; s.ly = g.leftThumbstick.yAxis.value;
        s.rx = g.rightThumbstick.xAxis.value; s.ry = g.rightThumbstick.yAxis.value;
        s.lt = g.leftTrigger.value; s.rt = g.rightTrigger.value;
        const struct { GCControllerButtonInput *in; uint32_t b; } m[] = {
            {g.buttonA, PAD_A}, {g.buttonB, PAD_B}, {g.buttonX, PAD_X}, {g.buttonY, PAD_Y}, {g.leftShoulder, PAD_LB}, {g.rightShoulder, PAD_RB},
            {g.buttonOptions, PAD_VIEW}, {g.buttonMenu, PAD_MENU}, {g.leftThumbstickButton, PAD_LS}, {g.rightThumbstickButton, PAD_RS},
            {g.dpad.up, PAD_UP}, {g.dpad.down, PAD_DOWN}, {g.dpad.left, PAD_LEFT}, {g.dpad.right, PAD_RIGHT}};
        for (size_t k = 0; k < sizeof m / sizeof *m; k++) if (m[k].in && m[k].in.pressed) s.buttons |= m[k].b;
        if (s.lt > 0.5f) s.buttons |= PAD_LT;
        if (s.rt > 0.5f) s.buttons |= PAD_RT;
    }
    if (test_mode && (test_lx || test_ly || test_pad_frames)) {         // scripted controller (test mode only)
        s.connected = 1; s.lx = test_lx; s.ly = test_ly;
        if (test_pad_frames > 0) { s.buttons |= test_pad_buttons; test_pad_frames--; }
    }
    uint32_t raw = s.buttons;                                            // as pressed, whichever mode
    int world = action_hud.in_game && !action_hud.menu_open;
    static int mode_world = -1, clicking, rclicking;
    if (mode_world != world) {                                           // let go of anything held in the other mode
        if (clicking) { pad_button(0, 0); clicking = 0; }
        if (rclicking) { pad_button(1, 0); rclicking = 0; }
        mode_world = world;
    }
    // A in the world: a click where the pointer is when the right stick moved it a moment ago, else the game side's
    // attack / talk / pick up on the nearest thing ahead
    static double pointer_moved; static int a_click; double tnow = CACurrentMediaTime();
    static uint32_t aprev; uint32_t apress = raw & ~aprev & PAD_A, arel = aprev & ~raw & PAD_A; aprev = raw;
    if (world && apress && tnow - pointer_moved < 2.5) { a_click = 1; pad_button(0, 1); }
    if (a_click && arel) { a_click = 0; pad_button(0, 0); }
    ActionPad e = s; if (!world) { e.lx = e.ly = 0; e.buttons = 0; }   // the game side only acts in the world
    if (a_click) e.buttons &= ~(uint32_t)PAD_A;
    action_pad = e;
    if (!s.connected) return;

    static uint32_t prev; uint32_t rp = raw & ~prev, rr = prev & ~raw; prev = raw;
    // R3 in the world: the right stick works the camera (turn, zoom) instead of the pointer, until R3 again
    static int camera; static int held_left, held_right; static double zoom_acc;
    if (world && (rp & PAD_RS)) {
        camera = !camera;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        snprintf((char *)action_hud.toast, sizeof action_hud.toast, "%s", camera ? "Right stick: camera (R3 for the pointer)" : "Right stick: pointer (R3 for the camera)");
        action_hud.toast_until = ts.tv_sec + ts.tv_nsec / 1e9 + 2.0; action_hud.seq++;
    }
    int turn_l = 0, turn_r = 0;
    if (world && camera) {
        float cx = pad_curve(s.rx), cy = pad_curve(s.ry);
        turn_l = cx < -0.15f; turn_r = cx > 0.15f;
        zoom_acc += cy * dt * 6; while (zoom_acc >= 1 || zoom_acc <= -1) { int d = zoom_acc > 0 ? 1 : -1; zoom_acc -= d; post(0x20a, ((uint32_t)(int16_t)(120 * d) << 16), pos_lp()); }
    } else {
        // the pointer: right stick (either stick outside the world), faster the further it is pushed
        uint32_t mw, mh; dsr_mode_size(&mw, &mh); double speed = (mh ? mh : 600) * 1.3 * dt;
        float sx = s.rx, sy = s.ry;
        if (!world && fabsf(s.lx) + fabsf(s.ly) > fabsf(s.rx) + fabsf(s.ry)) { sx = s.lx; sy = s.ly; }
        float px = pad_curve(sx), py = pad_curve(sy);
        if (px || py) { rel_motion(px * speed, -py * speed); pointer_moved = tnow; }
    }
    if (turn_l != held_left) { pad_key(0x25, turn_l); held_left = turn_l; }     // the arrow keys turn the camera while held
    if (turn_r != held_right) { pad_key(0x27, turn_r); held_right = turn_r; }

    if (world) {
        const struct { uint32_t b; int vk; } keys[] = {{PAD_LT, 'H'}, {PAD_LB, 'M'}, {PAD_LEFT, 0xbc}, {PAD_RIGHT, 0xbe}, {PAD_UP, 'Q'},
                                                       {PAD_DOWN, 'Z'}, {PAD_VIEW, 'I'}, {PAD_MENU, 0x1b}, {PAD_LS, 0x09}};
        for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
            if (rp & keys[k].b) pad_key(keys[k].vk, 1);
            if (rr & keys[k].b) pad_key(keys[k].vk, 0);
        }
    } else {
        if (rp & PAD_A) { pad_button(0, 1); clicking = 1; }
        if ((rr & PAD_A) && clicking) { pad_button(0, 0); clicking = 0; }
        if (rp & PAD_X) { pad_button(1, 1); rclicking = 1; }
        if ((rr & PAD_X) && rclicking) { pad_button(1, 0); rclicking = 0; }
        if (rp & (PAD_B | PAD_MENU)) pad_key(0x1b, 1);
        if (rr & (PAD_B | PAD_MENU)) pad_key(0x1b, 0);
        if (action_hud.in_game && (rp & PAD_VIEW)) pad_key('I', 1);
        if (action_hud.in_game && (rr & PAD_VIEW)) pad_key('I', 0);
    }
}
static void pad_start(void)
{
    static dispatch_source_t t; static double last;
    if (t || !action_on()) return;
    t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(t, DISPATCH_TIME_NOW, 4 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(t, ^{
        double now = CACurrentMediaTime(), dt = last ? fmin(now - last, 0.05) : 0; last = now;
        pad_tick((GCController.current ?: GCController.controllers.firstObject).extendedGamepad, dt);
    });
    dispatch_resume(t);
}
