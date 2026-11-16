// Standalone gamepad panel: its window thread reads input_overlay_state, never the game's merged input path.
#include "input_panel.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cfg.h"
#include "input.h"
#include "host.h"
#include "input_map.h"
#include "ui_text.h"
#include "uiwindow.h"
#include "platform/platform.h"

static UiWindow *window;
static PlatMutex owner_lock = PLAT_MUTEX_INIT;
static int stopping;
static int suppressed;
static const char *owner_title;
// Only the window thread uses these caches; the owner resets them before creation or after join.
static InputOverlayState previous;
static int previous_w, previous_h, valid;
static const char *previous_title;
static COLORREF background = RGB(18,22,27), idle = RGB(58,63,70), text_color = RGB(210,218,229);
static COLORREF active = RGB(78,161,255), outline = RGB(105,117,133);

typedef struct { HDC dc; double scale, ox, oy; } Canvas;
static int px(Canvas *c, double x) { return (int)lround(c->ox + x * c->scale); }
static int py(Canvas *c, double y) { return (int)lround(c->oy + y * c->scale); }
static void shape(Canvas *c, double x, double y, double w, double h, COLORREF fill, COLORREF edge, int round) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ oldb = SelectObject(c->dc, b), oldp = SelectObject(c->dc, p);
    if (round) Ellipse(c->dc, px(c,x),py(c,y),px(c,x+w),py(c,y+h));
    else RoundRect(c->dc,px(c,x),py(c,y),px(c,x+w),py(c,y+h),px(c,8)-px(c,0),py(c,8)-py(c,0));
    SelectObject(c->dc, oldb); SelectObject(c->dc, oldp); DeleteObject(b); DeleteObject(p);
}
static void line(Canvas *c, double x, double y, double xx, double yy, COLORREF color) {
    HPEN p = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old = SelectObject(c->dc,p);
    MoveToEx(c->dc,px(c,x),py(c,y),NULL); LineTo(c->dc,px(c,xx),py(c,yy));
    SelectObject(c->dc,old); DeleteObject(p);
}
static void label(Canvas *c, double x, double y, double w, double h, const char *text, int center) {
    wchar_t wide[512];
    if (!MultiByteToWideChar(CP_UTF8,0,text,-1,wide,512)) return;
    RECT r = {px(c,x),py(c,y),px(c,x+w),py(c,y+h)};
    DrawTextW(c->dc,wide,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|(center ? DT_CENTER : DT_LEFT));
}
static int down(const InputOverlayState *s, int bit) { return (s->buttons & (1ull << bit)) != 0; }
static void button(Canvas *c, const InputOverlayState *s, int bit, const char *name, int x, int y, int w, int h, int round) {
    shape(c,x,y,w,h,down(s,bit) ? active : idle,outline,round);
    label(c,x,y,w,h,name,1);
}
static void stick_draw(Canvas *c, const InputOverlayState *s, int i, int x, int y) {
    const int r = 55;
    shape(c,x-r,y-r,2*r,2*r,background,outline,1);
    double dz = r*s->deadzone[i]/100.;
    shape(c,x-dz,y-dz,2*dz,2*dz,background,RGB(72,89,107),1);
    line(c,x-r,y,x+r,y,RGB(43,52,65)); line(c,x,y-r,x,y+r,RGB(43,52,65));
    double a = s->raw[2*i]/32768., b = s->raw[2*i+1]/32768.;
    shape(c,x+a*r-5,y-b*r-5,10,10,active,active,1);
    a = s->out[2*i]/32767.; b = s->out[2*i+1]/32767.;
    line(c,x+a*r-6,y-b*r,x+a*r+6,y-b*r,RGB(249,199,101));
    line(c,x+a*r,y-b*r-6,x+a*r,y-b*r+6,RGB(249,199,101));
    char buf[96];
    snprintf(buf,sizeof buf,"%s  %d%%",i ? "R" : "L",s->deadzone[i]); label(c,x-r,y+r+4,2*r,20,buf,1);
    snprintf(buf,sizeof buf,"%d, %d",s->raw[2*i],s->raw[2*i+1]); label(c,x-r-20,345,2*r+40,20,buf,1);
    snprintf(buf,sizeof buf,"%d, %d",s->out[2*i],s->out[2*i+1]); label(c,x-r-20,365,2*r+40,20,buf,1);
}
static const char *physical_label(int n) {
    static const char *names[] = {"?","A","B","X","Y","Cross","Circle","Square","Triangle"};
    return n >= 0 && n < (int)(sizeof names / sizeof *names) ? names[n] : "?";
}
static void draw(void *user, void *dc, int w, int h) {
    (void)user;
    InputOverlayState s;
    input_overlay_state(&s);
    const char *title = menu_text(UI_INPUT_TITLE);
    if (valid && w == previous_w && h == previous_h && title == previous_title && !memcmp(&s,&previous,sizeof s)) return;
    previous = s; previous_w = w; previous_h = h; previous_title = title; valid = 1;
    HDC hdc = dc;
    RECT client = {0,0,w,h}; HBRUSH bg = CreateSolidBrush(background); FillRect(hdc,&client,bg); DeleteObject(bg);
    Canvas c = {hdc, fmin(w/620.,h/500.)*.95,0,0};
    if (c.scale <= 0) return;
    c.ox = (w-620*c.scale)/2; c.oy = (h-500*c.scale)/2;
    HFONT font = CreateFontW(-(int)fmax(9,lround(15*c.scale)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HGDIOBJ oldfont = SelectObject(hdc,font); SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,text_color);
    if (!s.connected) { label(&c,0,200,620,50,menu_text(UI_INPUT_NO_PAD),1); goto done; }
    char buf[512];
    snprintf(buf,sizeof buf,menu_text(UI_INPUT_MAPPING),s.layout == PAD_LAYOUT_XBOX ? "Xbox" : "Nintendo");
    label(&c,80,0,460,25,buf,0);
    button(&c,&s,B_L,"L",100,38,90,28,0); button(&c,&s,B_R,"R",430,38,90,28,0);
    for (int i=0;i<2;i++) {
        int x = i ? 555 : 25;
        shape(&c,x,40,25,100,idle,outline,0);
        int fill = (int)lround(96*s.trigger[i]/32767.);
        if (fill) shape(&c,x+2,138-fill,21,fill,active,active,0);
        // The chip is a mapped action; the meter and number remain physical analog input.
        button(&c,&s,i ? B_ZR : B_ZL,i ? "ZR" : "ZL",x-12,15,50,23,0);
        snprintf(buf,sizeof buf,"%d",s.trigger[i]); label(&c,x-22,145,70,20,buf,1);
    }
    button(&c,&s,B_MINUS,"-",245,50,45,25,0); button(&c,&s,B_PLUS,"+",330,50,45,25,0);
    // The diagram shows mapped logical actions, not hardware labels or custom binding names.
    int xbox = s.layout == PAD_LAYOUT_XBOX;
    button(&c,&s,xbox ? B_A : B_B,xbox ? "A" : "B",475,158,38,38,1);
    button(&c,&s,xbox ? B_B : B_A,xbox ? "B" : "A",516,117,38,38,1);
    button(&c,&s,xbox ? B_X : B_Y,xbox ? "X" : "Y",434,117,38,38,1);
    button(&c,&s,xbox ? B_Y : B_X,xbox ? "Y" : "X",475,76,38,38,1);
    button(&c,&s,B_LEFT,"<",85,266,32,32,0); button(&c,&s,B_UP,"^",119,232,32,32,0);
    button(&c,&s,B_RIGHT,">",153,266,32,32,0); button(&c,&s,B_DOWN,"v",119,300,32,32,0);
    stick_draw(&c,&s,0,160,130); stick_draw(&c,&s,1,415,265);
    button(&c,&s,B_STICK_L,"L3",243,218,45,30,1); button(&c,&s,B_STICK_R,"R3",320,218,45,30,1);
    label(&c,12,385,596,24,menu_text(UI_INPUT_STICK_LEGEND),1);
    char labels[160]; snprintf(labels,sizeof labels,"%s / %s / %s / %s",physical_label(s.labels[0]),physical_label(s.labels[1]),
                              physical_label(s.labels[2]),physical_label(s.labels[3]));
    snprintf(buf,sizeof buf,menu_text(UI_INPUT_LABELS),labels); label(&c,12,414,596,24,buf,1);
    if (s.motion_known) snprintf(buf,sizeof buf,"%s  X %+.2f  Y %+.2f  Z %+.2f",menu_text(UI_INPUT_GYRO),s.gyro[0],s.gyro[1],s.gyro[2]);
    else snprintf(buf,sizeof buf,"%s: %s",menu_text(UI_INPUT_GYRO),menu_text(!s.motion_available || s.motion_enabled ? UI_INPUT_UNKNOWN : UI_INPUT_DISABLED));
    label(&c,12,448,596,30,buf,1);
done:
    SelectObject(hdc,oldfont); DeleteObject(font);
}
static void close_window(void) {
    if (window) { uiwindow_close(window); window = NULL; }
    valid = 0;
}
void input_panel_close(void) {
    plat_mutex_lock(&owner_lock);
    stopping = 1;
    close_window();
    plat_mutex_unlock(&owner_lock);
}
void input_panel_tick(void) {
    plat_mutex_lock(&owner_lock);
    if (stopping) { plat_mutex_unlock(&owner_lock); return; }
    int close_requested = window && uiwindow_take_close_requested(window);
    int open = window && uiwindow_is_open(window);
    // WM_CLOSE publishes its notification before clearing open. If it raced the
    // first take, collect it after observing the stopped window and before free.
    if (window && !open && !close_requested)
        close_requested = uiwindow_take_close_requested(window);
    if (close_requested) {
        suppressed = 1;
        cfg_set("TOTK_INPUT_WINDOW","0"); cfg_save();
        close_window();
    }
    int enabled = cfg_bool("TOTK_INPUT_WINDOW");
    if (!enabled) { suppressed = 0; close_window(); plat_mutex_unlock(&owner_lock); return; }
    if (window && !open) {
        suppressed = 1;
        rt_log("input panel: window stopped; toggle input_window off/on to retry");
        close_window();
    }
    if (!window && !suppressed) {
        valid = 0;
        owner_title = menu_text(UI_INPUT_TITLE);
        window = uiwindow_open(owner_title,460,380,60,draw,NULL);
        if (!window) {
            suppressed = 1; // Do not retry a failed creation every frame; rearm after the setting is switched off and on.
            rt_log("input panel: cannot create window; toggle input_window off/on to retry");
        }
    }
    const char *title = menu_text(UI_INPUT_TITLE);
    if (window && title != owner_title) { owner_title = title; uiwindow_set_title(window,title); }
    plat_mutex_unlock(&owner_lock);
}
#else
void input_panel_tick(void) {}
void input_panel_close(void) {}
#endif
