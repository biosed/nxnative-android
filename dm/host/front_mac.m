// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−, Space=shake the controller.
// Game controllers (GameController framework) go through dm_pad_set as on iOS, buttons by position; DM_SHOW_FPS=1 shows the frame rate at the top left.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "hle.h"
#include <signal.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
static _Atomic int g_screen_fps;   // the window's screen's maximumFramesPerSecond, kept up to date when the window moves to another screen
int front_screen_max_fps(void) { return g_screen_fps; }
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    dm_input_set(g_btn, lx, ly, 0, 0);
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return 10; case 48: return 11;                                          // Return Tab → + −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
static void on_key(NSEvent *e, int down) {
    unsigned short c = e.keyCode;
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    int b = key_bit(c);
    uint64_t m = b >= 0 ? 1ull << b : c == 49 ? DM_BTN_SHAKE : 0;   // Space: shaken while held
    g_btn = down ? (g_btn | m) : (g_btn & ~m);
    push();
}

// Physical controllers: same mapping as the iOS app (DmGamepad.m), by position: right A, bottom B, top X, left Y
static int32_t pad_axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * DM_STICK_MAX); }
static void read_pad(GCExtendedGamepad *g) {
    uint64_t m = 0;
    #define K(btn, bit) if (btn.isPressed) m |= bit
    K(g.buttonB, DM_BTN_A); K(g.buttonA, DM_BTN_B); K(g.buttonY, DM_BTN_X); K(g.buttonX, DM_BTN_Y);
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R); K(g.leftTrigger, DM_BTN_ZL); K(g.rightTrigger, DM_BTN_ZR);
    K(g.buttonMenu, DM_BTN_PLUS); if (g.buttonOptions) K(g.buttonOptions, DM_BTN_MINUS);
    if (g.leftThumbstickButton) K(g.leftThumbstickButton, DM_BTN_LS); if (g.rightThumbstickButton) K(g.rightThumbstickButton, DM_BTN_RS);
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
    #undef K
    dm_pad_set(m, pad_axis(g.leftThumbstick.xAxis.value), pad_axis(g.leftThumbstick.yAxis.value),
               pad_axis(g.rightThumbstick.xAxis.value), pad_axis(g.rightThumbstick.yAxis.value));
}
static void pads_changed(void) {
    int any = 0;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad) {
        if (!any) LOG("front end: controller '%s'", c.vendorName.UTF8String ?: "?");
        any = 1; c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; read_pad(g); };
        read_pad(c.extendedGamepad);
    }
    if (!any) dm_pad_set(0, 0, 0, 0, 0);
}

// Settings, kept in the app's user defaults: the start-up window sets them, the Options menu changes some of them while playing. An environment switch set at
// launch (DM_OPMODE, DM_FPS, DM_SHOW_FPS, DM_LANG, DM_CHEATS_ON) wins over a saved setting and the window does not offer it.
#define K_OPMODE @"DMOpMode"
#define K_FPS @"DMFps"
#define K_SHOWFPS @"DMShowFPS"
#define K_LANG @"DMLang"
#define K_WINSIZE @"DMWindowSize"
#define K_CHEATS @"DMCheatsOn"
#define K_SKIPMENU @"DMSkipStartMenu"
#define K_FORCEMENU @"DMForceStartMenu"
int dm_opmode(void);

// The cheat names of DM_CHEATS, numbered as hle_cheat.c numbers them (entries with code only)
static NSArray<NSString *> *cheat_names(void) {
    const char *p = getenv("DM_CHEATS"); NSMutableArray *out = [NSMutableArray array];
    NSString *txt = p ? [NSString stringWithContentsOfFile:@(p) encoding:NSUTF8StringEncoding error:nil] : nil;
    if (!txt) txt = p ? [NSString stringWithContentsOfFile:@(p) encoding:NSISOLatin1StringEncoding error:nil] : nil;
    NSString *cur = nil; BOOL code = NO;
    for (NSString *raw in [txt componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *l = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([l hasPrefix:@"["] || [l hasPrefix:@"{"]) {
            if (cur && code) [out addObject:cur];
            NSString *close = [l hasPrefix:@"["] ? @"]" : @"}"; NSRange r = [l rangeOfString:close];
            cur = r.location != NSNotFound ? [l substringWithRange:NSMakeRange(1, r.location - 1)] : nil; code = NO;
            if (cur && [l hasPrefix:@"{"]) cur = nil;   // master codes always run and are not listed
        } else if (l.length >= 8) code = YES;
    }
    if (cur && code) [out addObject:cur];
    return out;
}
// Language code for "the system's language": the Mac's first preferred language, as the games' codes spell it
static NSString *system_lang(void) {
    NSString *l = NSLocale.preferredLanguages.firstObject ?: @"en-US";
    if ([l hasPrefix:@"es"]) return [l hasPrefix:@"es-ES"] ? @"es" : @"es-419";
    if ([l hasPrefix:@"en"]) return [l hasPrefix:@"en-US"] ? @"en-US" : @"en-GB";
    if ([l hasPrefix:@"pt"]) return [l hasPrefix:@"pt-BR"] ? @"pt-BR" : @"pt";
    if ([l hasPrefix:@"fr"]) return [l hasPrefix:@"fr-CA"] ? @"fr-CA" : @"fr";
    if ([l hasPrefix:@"zh-Hant"] || [l hasPrefix:@"zh-TW"] || [l hasPrefix:@"zh-HK"]) return @"zh-Hant";
    if ([l hasPrefix:@"zh"]) return @"zh-Hans";
    return [l componentsSeparatedByString:@"-"].firstObject;
}
static NSPopUpButton *popup(NSArray<NSString *> *titles, NSInteger sel) {
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 260, 26) pullsDown:NO];
    [p addItemsWithTitles:titles]; [p selectItemAtIndex:MAX(0, MIN(sel, (NSInteger)titles.count - 1))]; return p;
}
static NSTextField *label(NSString *s) { NSTextField *t = [NSTextField labelWithString:s]; t.alignment = NSTextAlignmentRight; return t; }

// Start-up settings window (Spanish): shown at launch unless "no volver a mostrar" was ticked; holding Option at launch, or Options ▸ Configuración… (which
// relaunches the app), shows it again. Returns the window size to open (0 = full screen), or -1 when the player chose to quit.
static int startup_settings(void) {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d registerDefaults:@{K_OPMODE: @(dm_opmode()), K_FPS: @60, K_SHOWFPS: @NO, K_LANG: @"es-419", K_WINSIZE: @1280, K_CHEATS: @[], K_SKIPMENU: @NO}];
    NSArray<NSString *> *cheats = cheat_names();
    BOOL show = ![d boolForKey:K_SKIPMENU] || [d boolForKey:K_FORCEMENU] || (NSEvent.modifierFlags & NSEventModifierFlagOption);
    [d removeObjectForKey:K_FORCEMENU];
    if (show) {
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"%s — Configuración", DM_GAME_NAME];
        a.informativeText = @"Elige cómo quieres jugar. Puedes cambiarlo luego con ⌘, o manteniendo ⌥ al abrir el juego.";
        [a addButtonWithTitle:@"Jugar"]; [a addButtonWithTitle:@"Salir"];
        NSInteger fpsSel = [d integerForKey:K_FPS] == 30 ? 0 : [d integerForKey:K_FPS] == 60 ? 1 : 2;
        NSArray *sizes = @[@1280, @1600, @1920, @0]; NSInteger sizeSel = [sizes indexOfObject:@([d integerForKey:K_WINSIZE])]; if (sizeSel == NSNotFound) sizeSel = 0;
        NSArray *langs = @[@"es-419", @"en-US", @"system"]; NSInteger langSel = [langs indexOfObject:[d stringForKey:K_LANG]]; if (langSel == NSNotFound) langSel = 0;
        NSPopUpButton *mode = popup(@[@"Dock (TV, más resolución)", @"Portátil (más ligero)"], [d integerForKey:K_OPMODE] == 0 ? 1 : 0);
        NSPopUpButton *fps = popup(@[@"30", @"60 (predeterminado)", @"Sin límite (lo que pida el juego)"], fpsSel);
        NSPopUpButton *win = popup(@[@"1280 × 720", @"1600 × 900", @"1920 × 1080", @"Pantalla completa"], sizeSel);
        NSPopUpButton *lang = popup(@[@"Español (Latinoamérica)", @"English", [NSString stringWithFormat:@"Idioma del sistema (%@)", system_lang()]], langSel);
        NSButton *showFps = [NSButton checkboxWithTitle:@"Mostrar los FPS en pantalla" target:nil action:nil]; showFps.state = [d boolForKey:K_SHOWFPS];
        NSGridView *g = [NSGridView gridViewWithViews:@[@[label(@"Modo de consola:"), mode], @[label(@"Cuadros por segundo:"), fps], @[label(@"Tamaño de la ventana:"), win],
                                                        @[label(@"Idioma:"), lang], @[[NSGridCell emptyContentView], showFps]]];
        g.rowSpacing = 8; g.columnSpacing = 8;
        NSStackView *stack = [NSStackView stackViewWithViews:@[g]]; stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 12;
        NSMutableArray<NSButton *> *boxes = [NSMutableArray array]; NSArray *on = [d arrayForKey:K_CHEATS] ?: @[];
        if (cheats.count) {
            NSTextField *t = [NSTextField labelWithString:@"Trucos (activa solo los que quieras):"]; t.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize]; [stack addArrangedSubview:t];
            NSStackView *list = [NSStackView new]; list.orientation = NSUserInterfaceLayoutOrientationVertical; list.alignment = NSLayoutAttributeLeading; list.spacing = 4;
            for (NSUInteger i = 0; i < cheats.count; i++) { NSButton *b = [NSButton checkboxWithTitle:cheats[i] target:nil action:nil]; b.state = [on containsObject:@(i + 1)]; [boxes addObject:b]; [list addArrangedSubview:b]; }
            NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 420, MIN(220, 24 * cheats.count + 8))];
            sv.hasVerticalScroller = YES; sv.drawsBackground = NO; sv.borderType = NSBezelBorder;
            NSClipView *clip = sv.contentView; list.translatesAutoresizingMaskIntoConstraints = NO; sv.documentView = list;
            [NSLayoutConstraint activateConstraints:@[[list.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor constant:6], [list.topAnchor constraintEqualToAnchor:clip.topAnchor constant:4],
                                                      [list.widthAnchor constraintGreaterThanOrEqualToAnchor:clip.widthAnchor constant:-12]]];
            [sv.widthAnchor constraintEqualToConstant:420].active = YES; [sv.heightAnchor constraintEqualToConstant:MIN(220, 24 * cheats.count + 8)].active = YES;
            [stack addArrangedSubview:sv];
        } else [stack addArrangedSubview:[NSTextField labelWithString:@"Este juego no tiene trucos para esta versión."]];
        NSButton *skip = [NSButton checkboxWithTitle:@"No volver a mostrar este menú" target:nil action:nil]; skip.state = [d boolForKey:K_SKIPMENU]; [stack addArrangedSubview:skip];
        stack.frame = NSMakeRect(0, 0, 440, stack.fittingSize.height); a.accessoryView = stack;
        [NSApp finishLaunching]; [NSApp activate];   // a launch from Finder may not make the app active before the modal starts: bring the window forward
        [a layout]; a.window.level = NSFloatingWindowLevel; [a.window orderFrontRegardless];
        if ([a runModal] != NSAlertFirstButtonReturn) return -1;
        [d setInteger:mode.indexOfSelectedItem == 1 ? 0 : 1 forKey:K_OPMODE];
        [d setInteger:fps.indexOfSelectedItem == 0 ? 30 : fps.indexOfSelectedItem == 1 ? 60 : 0 forKey:K_FPS];
        [d setInteger:[sizes[win.indexOfSelectedItem] integerValue] forKey:K_WINSIZE];
        [d setObject:langs[lang.indexOfSelectedItem] forKey:K_LANG];
        [d setBool:showFps.state == NSControlStateValueOn forKey:K_SHOWFPS];
        NSMutableArray *sel = [NSMutableArray array]; for (NSUInteger i = 0; i < boxes.count; i++) if (boxes[i].state == NSControlStateValueOn) [sel addObject:@(i + 1)];
        [d setObject:sel forKey:K_CHEATS]; [d setBool:skip.state == NSControlStateValueOn forKey:K_SKIPMENU];
    }
    char v[32];
    if (!getenv("DM_OPMODE")) { snprintf(v, sizeof v, "%d", (int)[d integerForKey:K_OPMODE]); setenv("DM_OPMODE", v, 1); }
    if (!getenv("DM_FPS") && [d integerForKey:K_FPS]) { snprintf(v, sizeof v, "%d", (int)[d integerForKey:K_FPS]); setenv("DM_FPS", v, 1); }
    if (!getenv("DM_SHOW_FPS")) setenv("DM_SHOW_FPS", [d boolForKey:K_SHOWFPS] ? "1" : "0", 1);
    if (!getenv("DM_LANG")) { NSString *l = [d stringForKey:K_LANG]; if ([l isEqual:@"system"]) l = system_lang(); setenv("DM_LANG", l.UTF8String, 1); }
    if (!getenv("DM_CHEATS_ON")) { NSString *s = [[d arrayForKey:K_CHEATS] componentsJoinedByString:@","]; setenv("DM_CHEATS_ON", s.UTF8String ?: "", 1); }
    LOG("front end: start-up settings: mode %s, frame rate %s, language %s, cheats [%s], window %d", getenv("DM_OPMODE"), getenv("DM_FPS") ?: "the game's own", getenv("DM_LANG"), getenv("DM_CHEATS_ON"), (int)[d integerForKey:K_WINSIZE]);
    return (int)[d integerForKey:K_WINSIZE];
}

// Frame-rate counter (DM_SHOW_FPS=1, or Options ▸ Show Frame Rate): the frames put on screen in the last second, as on iOS (front_ios.m)
uint64_t mtl_present_count(void);
static CATextLayer *g_fps_layer;
static void fps_counter(CALayer *host, CGFloat scale) {
    const char *e = getenv("DM_SHOW_FPS");
    CATextLayer *l = g_fps_layer = [CATextLayer layer]; l.hidden = !(e && atoi(e));
    l.font = (__bridge CFTypeRef)[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightSemibold]; l.fontSize = 13;
    l.foregroundColor = NSColor.whiteColor.CGColor; l.backgroundColor = [NSColor colorWithWhite:0 alpha:0.45].CGColor;
    l.alignmentMode = kCAAlignmentCenter; l.cornerRadius = 6; l.contentsScale = scale;
    l.frame = CGRectMake(8, host.bounds.size.height - 30, 64, 22); l.autoresizingMask = kCALayerMinYMargin | kCALayerMaxXMargin;   // top left (AppKit layers start at the bottom)
    l.string = @"-- fps"; [host addSublayer:l];
    __block uint64_t n0 = mtl_present_count(); __block double t0 = CACurrentMediaTime();
    [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        (void)t; uint64_t n = mtl_present_count(); double now = CACurrentMediaTime();
        if (now - t0 < 1.5) l.string = [NSString stringWithFormat:@"%.0f fps", (n - n0) / (now - t0)];   // a longer span is time minimized: no reading
        n0 = n; t0 = now;
    }];
}

// Menu bar: the app menu (Quit), Options (docked / handheld, frame rate, frame-rate counter) and Window (sizes, full screen)
int dm_opmode(void);
int mtl_fps_target(void);
void mtl_set_fps_pin(int fps);
static void quit_game(const char *why) {   // graceful exit off the main thread: dm_host_shutdown waits for in-flight graphics, which may need the main thread, and _exits itself
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ dm_host_shutdown(why); });
}
@interface DmMenus : NSObject <NSMenuItemValidation>
@property (weak) NSWindow *win;
@end
@implementation DmMenus
- (void)quit:(id)s { (void)s; quit_game("Quit"); }
- (void)settings:(id)s {   // Configuración…: the start-up window needs a fresh launch (the game reads its mode, language and cheats once), so relaunch the app with the window forced
    (void)s; [NSUserDefaults.standardUserDefaults setBool:YES forKey:K_FORCEMENU]; [NSUserDefaults.standardUserDefaults synchronize];
    NSString *app = NSBundle.mainBundle.bundlePath;
    if ([app hasSuffix:@".app"]) [NSTask launchedTaskWithLaunchPath:@"/bin/sh" arguments:@[@"-c", [NSString stringWithFormat:@"sleep 2; /usr/bin/open -n '%@'", app]]];
    quit_game("Configuración");
}
- (void)opMode:(NSMenuItem *)s { dm_set_opmode((int)s.tag); [NSUserDefaults.standardUserDefaults setInteger:s.tag forKey:K_OPMODE]; }
- (void)fps:(NSMenuItem *)s { mtl_set_fps_pin((int)s.tag); [NSUserDefaults.standardUserDefaults setInteger:s.tag forKey:K_FPS]; }
- (void)showFps:(id)s { (void)s; g_fps_layer.hidden = !g_fps_layer.hidden; [NSUserDefaults.standardUserDefaults setBool:!g_fps_layer.hidden forKey:K_SHOWFPS]; }
- (void)size:(NSMenuItem *)s {
    NSWindow *w = self.win; if (w.styleMask & NSWindowStyleMaskFullScreen) [w toggleFullScreen:nil];
    NSRect f = [w frameRectForContentRect:NSMakeRect(0, 0, s.tag, s.tag * 9 / 16)], vis = (w.screen ?: NSScreen.mainScreen).visibleFrame;
    if (f.size.width > vis.size.width || f.size.height > vis.size.height) { CGFloat k = fmin(vis.size.width / f.size.width, vis.size.height / f.size.height); f = [w frameRectForContentRect:NSMakeRect(0, 0, floor(s.tag * k), floor(s.tag * k * 9 / 16))]; }
    [w setFrame:f display:YES]; [w center];
}
- (BOOL)validateMenuItem:(NSMenuItem *)m {
    if (m.action == @selector(opMode:)) m.state = dm_opmode() == m.tag;
    else if (m.action == @selector(fps:)) m.state = mtl_fps_target() == m.tag;
    else if (m.action == @selector(showFps:)) m.state = !g_fps_layer.hidden;
    return YES;
}
@end
static NSMenuItem *menu_item(NSMenu *m, NSString *title, SEL act, NSString *key, NSInteger tag, id target) {
    NSMenuItem *i = [m addItemWithTitle:title action:act keyEquivalent:key]; i.tag = tag; i.target = target; return i;
}
static void build_menus(DmMenus *t) {
    NSMenu *bar = [NSMenu new]; NSString *name = @DM_GAME_NAME;
    NSMenu *app = [[NSMenu alloc] initWithTitle:name];
    menu_item(app, @"Configuración…", @selector(settings:), @",", 0, t);
    [app addItem:NSMenuItem.separatorItem];
    menu_item(app, [@"Hide " stringByAppendingString:name], @selector(hide:), @"h", 0, NSApp);
    [app addItem:NSMenuItem.separatorItem];
    menu_item(app, [@"Quit " stringByAppendingString:name], @selector(quit:), @"q", 0, t);
    NSMenu *opt = [[NSMenu alloc] initWithTitle:@"Options"];
    menu_item(opt, @"TV Mode (docked, sharper)", @selector(opMode:), @"1", 1, t);
    menu_item(opt, @"Handheld Mode (lighter)", @selector(opMode:), @"2", 0, t);
    [opt addItem:NSMenuItem.separatorItem];
    menu_item(opt, @"60 fps", @selector(fps:), @"6", 60, t);
    menu_item(opt, @"30 fps (saves battery)", @selector(fps:), @"3", 30, t);
    [opt addItem:NSMenuItem.separatorItem];
    menu_item(opt, @"Show Frame Rate", @selector(showFps:), @"f", 0, t);
    NSMenu *wm = [[NSMenu alloc] initWithTitle:@"Window"];
    menu_item(wm, @"1280×720", @selector(size:), @"", 1280, t);
    menu_item(wm, @"1600×900", @selector(size:), @"", 1600, t);
    menu_item(wm, @"1920×1080", @selector(size:), @"", 1920, t);
    [wm addItem:NSMenuItem.separatorItem];
    NSMenuItem *fs = menu_item(wm, @"Enter Full Screen", @selector(toggleFullScreen:), @"f", 0, nil); fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    menu_item(wm, @"Minimize", @selector(performMiniaturize:), @"m", 0, nil);
    for (NSMenu *m in @[app, opt, wm]) { NSMenuItem *top = [bar addItemWithTitle:m.title action:nil keyEquivalent:@""]; top.submenu = m; }
    NSApp.mainMenu = bar; NSApp.windowsMenu = wm;
}

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

void front_run(void (*guest)(void)) {
    void dm_cheats_start(void);
    if (!getenv("DM_WINDOW")) { dm_cheats_start(); guest(); return; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        int size = startup_settings();
        if (size < 0) exit(0);
        dm_cheats_start();
        NSRect r = NSMakeRect(0, 0, size ? size : 1280, (size ? size : 1280) * 9 / 16);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @DM_GAME_NAME;   // the game profile's name (game name label)
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary; win.contentAspectRatio = NSMakeSize(16, 9);
        static DmMenus *menus; menus = [DmMenus new]; menus.win = win; build_menus(menus);
        NSView *v = win.contentView;
        v.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        v.layer = g_layer;
        g_layer.frame = v.bounds; fps_counter(g_layer, win.backingScaleFactor);
        [win center];
        if (!size) dispatch_async(dispatch_get_main_queue(), ^{ [win toggleFullScreen:nil]; });
        [win makeKeyAndOrderFront:nil];
        g_screen_fps = (int)(win.screen ?: NSScreen.mainScreen).maximumFramesPerSecond;
        [NSApp activateIgnoringOtherApps:YES];
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // Swallow it to avoid the system beep
        }];
        // Mouse as a single-point touchscreen (for testing touch on the local machine, front.h dm_touch_set): left button down / drag = finger 0 at the normalized coordinates, release = lifted
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;
            if (e.type == NSEventTypeLeftMouseUp) { dm_touch_set(NULL, 0); return nil; }
            NSPoint p = [v convertPoint:e.locationInWindow fromView:nil]; NSSize sz = v.bounds.size;
            if (sz.width <= 0 || sz.height <= 0) return e;
            dm_touch t = {0, (float)(p.x / sz.width), (float)(1.0 - p.y / sz.height)};   // The AppKit view origin is at bottom left, the touchscreen origin is at top left
            dm_touch_set(&t, 1); return nil;
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; quit_game("window closed"); }];   // exit() here could hang in atexit work while guest threads run
        [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; pads_changed(); }];
        [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; pads_changed(); }];
        pads_changed();
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1)
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidChangeScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (win.screen) g_screen_fps = (int)win.screen.maximumFramesPerSecond; }];
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (!win.miniaturized) dm_focus_set(2); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? @(title) : @"Input";
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        a.informativeText = info;
        [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"];
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        f.stringValue = initial ? @(initial) : @"";
        if (guide && *guide) f.placeholderString = @(guide);
        a.accessoryView = f; a.window.initialFirstResponder = f;
        r = [a runModal] == NSAlertFirstButtonReturn;
        text = f.stringValue;
    });
    if (!r) return 0;
    (void)max_chars;                              // Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
