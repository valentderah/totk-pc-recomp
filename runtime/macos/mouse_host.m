// Cursor ownership stays on AppKit's main thread. The input owner merges the
// raw deltas with the keyboard and gamepad once per game frame.
#import "mouse_host.h"
#import <GameController/GameController.h>
#include "mouse_input.h"
#include <math.h>
#include <stdio.h>

static __weak NSWindow *game_window;
static BOOL captured, text_entry;
static double sensitivity = 1.0;
static BOOL invert_y;
static BOOL discard_warp_delta;
static NSTimeInterval captured_at;
static NSMutableSet<GCMouse *> *raw_mice;
static void set_capture(BOOL on, BOOL user);

void native_mouse_release(void) { set_capture(NO,NO); }
void native_mouse_text_entry(int active) {
    text_entry = active != 0;
    if (active) set_capture(NO,NO);
}

static void set_capture(BOOL on, BOOL user) {
    NSWindow *window = game_window;
    if (on && (!window.isKeyWindow || !NSApp.isActive || text_entry || sensitivity == 0)) return;
    if (on == captured) return;
    if (on) {
        NSRect rect = [window convertRectToScreen:[window.contentView convertRect:window.contentView.bounds toView:nil]];
        NSPoint center = NSMakePoint(NSMidX(rect), NSMidY(rect));
        // AppKit uses a bottom-left origin; Quartz uses the main display's top left.
        CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
        if (CGAssociateMouseAndMouseCursorPosition(false) != kCGErrorSuccess) {
            fprintf(stderr, "[totk-input] mouse capture unavailable\n"); return;
        }
        CGWarpMouseCursorPosition(CGPointMake(center.x, top-center.y));
        // AppKit can fold the warp into its next relative event. Raw device
        // deltas do not contain it; discard only that first fallback event.
        discard_warp_delta = YES;
        captured_at = NSProcessInfo.processInfo.systemUptime;
        [NSCursor hide];
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
    }
    captured = on;
    runtime_input_mouse_capture(on,user);
    fprintf(stderr, "[totk-input] mouse %s\n", on ? "captured" : "released");
}

void runtime_front_mouse_capture(int on) {
    dispatch_async(dispatch_get_main_queue(), ^{ set_capture(on != 0,NO); });
}

BOOL native_mouse_key_event(NSEvent *event, BOOL down) {
    if (event.keyCode != 53) return NO;
    if (down) set_capture(NO,NO);
    return NO;
}

void native_mouse_button_event(NSEvent *event, BOOL down) {
    if (!captured) { if (down) set_capture(YES,YES); return; }
    if (!game_window.isKeyWindow || text_entry) return;
    runtime_input_mouse_button((unsigned)event.buttonNumber, down, YES);
}

void native_mouse_motion_event(NSEvent *event) {
    // GameController supplies unaccelerated deltas. Trackpads without a raw
    // profile use AppKit's relative events; never count both copies of a move.
    if (!captured || raw_mice.count || !game_window.isKeyWindow || text_entry || event.timestamp < captured_at) return;
    if (discard_warp_delta) { discard_warp_delta=NO; return; }
    runtime_input_mouse_motion(event.deltaX, -event.deltaY, YES);
}

NSString *native_mouse_hint(void) {
    if (!sensitivity) return @"Mouse camera disabled in Controls";
    return captured ? @"Esc  Release mouse" : @"Click to use mouse  |  Esc to release";
}

static void attach_mouse(GCMouse *mouse) {
    if (!mouse.mouseInput || [raw_mice containsObject:mouse]) return;
    [raw_mice addObject:mouse];
    mouse.handlerQueue = dispatch_get_main_queue();
    mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput *input, float dx, float dy) {
        (void)input;
        if (captured && game_window.isKeyWindow && !text_entry)
            runtime_input_mouse_motion(dx, dy, YES);
    };
    fprintf(stderr, "[totk-input] raw mouse connected: %s\n", (mouse.vendorName ?: @"Mouse").UTF8String);
}

@interface NativeMouseControls : NSObject<NSMenuItemValidation>
@end
@implementation NativeMouseControls
- (void)toggleCapture:(id)sender { (void)sender; set_capture(!captured,YES); }
- (void)setSensitivity:(NSMenuItem *)item {
    sensitivity = item.tag / 100.0;
    [NSUserDefaults.standardUserDefaults setDouble:sensitivity forKey:@"MouseSensitivity"];
    runtime_input_mouse_configure(sensitivity, invert_y);
    if (!sensitivity) set_capture(NO,NO);
}
- (void)toggleInvert:(id)sender {
    (void)sender; invert_y = !invert_y;
    [NSUserDefaults.standardUserDefaults setBool:invert_y forKey:@"MouseInvertY"];
    runtime_input_mouse_configure(sensitivity, invert_y);
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(toggleCapture:)) {
        item.title = captured ? @"Release Mouse" : @"Capture Mouse";
        return !text_entry && sensitivity > 0;
    }
    if (item.action == @selector(setSensitivity:)) item.state = fabs(sensitivity-item.tag/100.0)<0.001 ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleInvert:)) item.state = invert_y ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end
static NativeMouseControls *controls;

NSMenu *native_mouse_controls_menu(void) {
    controls = [NativeMouseControls new];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Controls"];
    NSMenuItem *capture = [[NSMenuItem alloc] initWithTitle:@"Capture Mouse" action:@selector(toggleCapture:) keyEquivalent:@"m"];
    capture.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    capture.target = controls; [menu addItem:capture];
    NSMenuItem *speed = [[NSMenuItem alloc] initWithTitle:@"Mouse Sensitivity" action:nil keyEquivalent:@""];
    speed.submenu = [[NSMenu alloc] initWithTitle:speed.title];
    for (NSNumber *value in @[@0, @50, @100, @150, @200, @300, @500]) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:value.intValue ? [NSString stringWithFormat:@"%g x",value.doubleValue/100] : @"Off" action:@selector(setSensitivity:) keyEquivalent:@""];
        item.tag = value.intValue; item.target = controls; [speed.submenu addItem:item];
    }
    [menu addItem:speed];
    NSMenuItem *invert = [[NSMenuItem alloc] initWithTitle:@"Invert Mouse Y" action:@selector(toggleInvert:) keyEquivalent:@""];
    invert.target = controls; [menu addItem:invert];
    return menu;
}

void native_mouse_start(NSWindow *window) {
    game_window = window; window.acceptsMouseMovedEvents = YES;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{@"MouseSensitivity": @1.0, @"MouseInvertY": @NO}];
    sensitivity = [defaults doubleForKey:@"MouseSensitivity"];
    invert_y = [defaults boolForKey:@"MouseInvertY"];
    const char *setting = getenv("TOTK_MOUSE_SENS");
    if (setting) { char *end; double value = strtod(setting,&end); if (end!=setting && !*end && isfinite(value)) sensitivity=value; }
    setting = getenv("TOTK_MOUSE_INVERT_Y");
    if (setting) invert_y = atoi(setting) != 0;
    sensitivity = isfinite(sensitivity) ? fmin(10.0,fmax(0.0,sensitivity)) : 1.0;
    runtime_input_mouse_configure(sensitivity,invert_y);
    raw_mice = [NSMutableSet new];
    NSNotificationCenter *notifications = NSNotificationCenter.defaultCenter;
    [notifications addObserverForName:GCMouseDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *note){ attach_mouse(note.object); }];
    [notifications addObserverForName:GCMouseDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *note){ GCMouse *mouse=note.object; mouse.mouseInput.mouseMovedHandler=nil; [raw_mice removeObject:mouse]; set_capture(NO,NO); }];
    for (GCMouse *mouse in GCMouse.mice) attach_mouse(mouse);
    for (NSNotificationName name in @[NSWindowDidResignKeyNotification, NSWindowWillMiniaturizeNotification,
            NSWindowWillMoveNotification, NSWindowWillStartLiveResizeNotification,
            NSWindowWillEnterFullScreenNotification, NSWindowWillExitFullScreenNotification, NSWindowWillCloseNotification])
        [notifications addObserverForName:name object:window queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *note){ (void)note; set_capture(NO,NO); }];
    [notifications addObserverForName:NSApplicationWillResignActiveNotification object:NSApp queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *note){ (void)note; set_capture(NO,NO); }];
}
