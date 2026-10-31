#ifndef TOTK_MOUSE_HOST_H
#define TOTK_MOUSE_HOST_H
#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
void native_mouse_start(NSWindow *window);
NSMenu *native_mouse_controls_menu(void);
BOOL native_mouse_key_event(NSEvent *event, BOOL down);
void native_mouse_button_event(NSEvent *event, BOOL down);
void native_mouse_motion_event(NSEvent *event);
NSString *native_mouse_hint(void);
#endif
void native_mouse_release(void);
void native_mouse_text_entry(int active);
void runtime_front_mouse_capture(int captured);
#endif
