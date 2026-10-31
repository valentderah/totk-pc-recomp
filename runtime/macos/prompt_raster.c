#include "input_prompts.h"
#include "input_bindings.h"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rounded(CGContextRef c, CGRect r, double radius, bool fill) {
    CGPathRef p = CGPathCreateWithRoundedRect(r, radius, radius, NULL);
    CGContextAddPath(c, p);
    if (fill) CGContextFillPath(c); else CGContextStrokePath(c);
    CGPathRelease(p);
}
static void line(CGContextRef c, double x1, double y1, double x2, double y2) {
    CGContextMoveToPoint(c,x1,y1); CGContextAddLineToPoint(c,x2,y2); CGContextStrokePath(c);
}
static void label(CGContextRef c, const char *text, CGRect r) {
    if (!text || !*text) return;
    CFStringRef value = CFStringCreateWithCString(NULL,text,kCFStringEncodingUTF8);
    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica-Bold"),64,NULL);
    const void *keys[] = {kCTFontAttributeName,kCTForegroundColorFromContextAttributeName};
    const void *values[] = {font,kCFBooleanTrue};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL,keys,values,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef string = CFAttributedStringCreate(NULL,value,attrs);
    CTLineRef text_line = CTLineCreateWithAttributedString(string);
    CGRect b = CTLineGetBoundsWithOptions(text_line,kCTLineBoundsUseGlyphPathBounds);
    if (b.size.width > 0 && b.size.height > 0) {
        double scale = fmin(r.size.width/b.size.width,r.size.height/b.size.height);
        CGContextSaveGState(c);
        CGContextTranslateCTM(c,r.origin.x+(r.size.width-b.size.width*scale)/2,
                              r.origin.y+(r.size.height-b.size.height*scale)/2);
        CGContextScaleCTM(c,scale,scale);
        CGContextSetTextPosition(c,-b.origin.x,-b.origin.y);
        CTLineDraw(text_line,c);
        CGContextRestoreGState(c);
    }
    CFRelease(text_line); CFRelease(string); CFRelease(attrs); CFRelease(font); CFRelease(value);
}
static void arrow(CGContextRef c, double x, double y, double angle, double size) {
    CGContextSaveGState(c); CGContextTranslateCTM(c,x,y); CGContextRotateCTM(c,angle);
    CGContextMoveToPoint(c,-size,-size*.55); CGContextAddLineToPoint(c,0,0);
    CGContextAddLineToPoint(c,-size,size*.55); CGContextStrokePath(c); CGContextRestoreGState(c);
}
static void arrows(CGContextRef c, unsigned bits, double w, double h) {
    double size=fmin(w,h)*.12;
    if (bits&1) arrow(c,w*.05,h*.5,M_PI,size);
    if (bits&2) arrow(c,w*.5,h*.95,M_PI_2,size);
    if (bits&4) arrow(c,w*.95,h*.5,0,size);
    if (bits&8) arrow(c,w*.5,h*.05,-M_PI_2,size);
}
bool native_prompt_raster(const NativePromptGlyph *g, uint8_t *pixels, unsigned width, unsigned height) {
    if (!g || !pixels || !width || !height || width>1024 || height>1024) return false;
    memset(pixels,0,(size_t)width*height);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef c = CGBitmapContextCreate(pixels,width,height,8,width,gray,kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!c) return false;
    double w=width,h=height,unit=fmin(w,h);
    CGContextSetAllowsAntialiasing(c,true); CGContextSetShouldAntialias(c,true);
    CGContextSetShouldSmoothFonts(c,false);
    CGContextSetGrayFillColor(c,1,1); CGContextSetGrayStrokeColor(c,1,1);
    CGContextSetLineWidth(c,fmax(1.1,unit*.055));
    CGContextSetLineJoin(c,kCGLineJoinRound); CGContextSetLineCap(c,kCGLineCapRound);
    CGRect inset=CGRectMake(w*.035,h*.035,w*.93,h*.93);
    switch (g->kind) {
    case NATIVE_GLYPH_KEY_GROUP: {
        char text[64]={0};size_t used=0;
        for(unsigned bit=0;bit<16;bit++)if(g->detail&(1u<<bit)) {
            int n=snprintf(text+used,sizeof text-used,"%s%s",used?" ":"",native_key_label(1u<<bit));
            if(n<0 || (size_t)n>=sizeof text-used)break;
            used+=(size_t)n;
        }
        rounded(c,inset,unit*.10,true);CGContextSetGrayFillColor(c,0,1);
        label(c,text,CGRectMake(w*.12,h*.19,w*.76,h*.62));
        break;
    }
    case NATIVE_GLYPH_KEY:
    case NATIVE_GLYPH_SHOULDER:
        rounded(c,inset,unit*.10,true);
        CGContextSetGrayFillColor(c,0,1); CGContextSetGrayStrokeColor(c,0,1);
        if (!strcmp(g->label,"Space")) {
            line(c,w*.22,h*.55,w*.22,h*.36); line(c,w*.22,h*.36,w*.78,h*.36); line(c,w*.78,h*.36,w*.78,h*.55);
        } else if (!strcmp(g->label,"Enter")) {
            line(c,w*.75,h*.7,w*.75,h*.4); line(c,w*.75,h*.4,w*.25,h*.4); arrow(c,w*.25,h*.4,M_PI,unit*.20);
        } else if (!strcmp(g->label,"Tab")) {
            line(c,w*.2,h*.5,w*.74,h*.5); arrow(c,w*.74,h*.5,0,unit*.2); line(c,w*.8,h*.28,w*.8,h*.72);
        } else label(c,g->label,CGRectMake(w*.14,h*.19,w*.72,h*.62));
        break;
    case NATIVE_GLYPH_CIRCLE:
        if (*g->label) {
            CGContextFillEllipseInRect(c,inset); CGContextSetGrayFillColor(c,0,1);
            label(c,g->label,CGRectMake(w*.18,h*.20,w*.64,h*.60));
        } else CGContextStrokeEllipseInRect(c,CGRectMake(w*.18,h*.18,w*.64,h*.64));
        break;
    case NATIVE_GLYPH_CROSS:
        line(c,w*.23,h*.23,w*.77,h*.77);line(c,w*.23,h*.77,w*.77,h*.23);break;
    case NATIVE_GLYPH_TRIANGLE:
        CGContextMoveToPoint(c,w*.5,h*.86);CGContextAddLineToPoint(c,w*.13,h*.20);CGContextAddLineToPoint(c,w*.87,h*.20);CGContextClosePath(c);CGContextStrokePath(c);break;
    case NATIVE_GLYPH_SQUARE:
        CGContextStrokeRect(c,CGRectMake(w*.2,h*.2,w*.6,h*.6));break;
    case NATIVE_GLYPH_MOUSE:
    case NATIVE_GLYPH_MOUSE_MOVE: {
        CGRect body=CGRectMake(w*.22,h*.08,w*.56,h*.84);
        rounded(c,body,unit*.22,false);
        if (g->kind==NATIVE_GLYPH_MOUSE) {
            CGContextSaveGState(c);
            CGPathRef p=CGPathCreateWithRoundedRect(body,unit*.22,unit*.22,NULL);CGContextAddPath(c,p);CGContextClip(c);CGPathRelease(p);
            if (g->detail==0) CGContextFillRect(c,CGRectMake(w*.22,h*.55,w*.28,h*.4));
            if (g->detail==1) CGContextFillRect(c,CGRectMake(w*.5,h*.55,w*.28,h*.4));
            CGContextRestoreGState(c);
            line(c,w*.24,h*.55,w*.76,h*.55);
            CGContextSetGrayStrokeColor(c,0,1);line(c,w*.5,h*.55,w*.5,h*.92);CGContextSetGrayStrokeColor(c,1,1);
            if (g->detail==2) rounded(c,CGRectMake(w*.44,h*.59,w*.12,h*.22),unit*.05,true);
        } else {
            rounded(c,CGRectMake(w*.46,h*.65,w*.08,h*.13),unit*.04,true);
            arrows(c,g->detail,w,h);
        }
        break;
    }
    case NATIVE_GLYPH_STICK:
        CGContextStrokeEllipseInRect(c,CGRectMake(w*.22,h*.22,w*.56,h*.56));
        label(c,g->label,CGRectMake(w*.29,h*.32,w*.42,h*.36));
        if (g->detail&16) { line(c,w*.35,h*.15,w*.65,h*.15); arrow(c,w*.5,h*.04,-M_PI_2,unit*.13); }
        else arrows(c,g->detail,w,h);
        break;
    case NATIVE_GLYPH_DPAD: {
        const CGRect arms[]={CGRectMake(w*.08,h*.39,w*.31,h*.22),CGRectMake(w*.39,h*.61,w*.22,h*.31),CGRectMake(w*.61,h*.39,w*.31,h*.22),CGRectMake(w*.39,h*.08,w*.22,h*.31)};
        for(unsigned i=0;i<4;i++) { if(g->detail&(1u<<i))CGContextFillRect(c,arms[i]);else CGContextStrokeRect(c,arms[i]); }
        break;
    }
    case NATIVE_GLYPH_KEYBOARD:
        rounded(c,CGRectMake(w*.04,h*.15,w*.72,h*.70),unit*.06,false);
        for(unsigned y=0;y<3;y++)for(unsigned x=0;x<5;x++)CGContextFillRect(c,CGRectMake(w*(.11+x*.12),h*(.63-y*.15),w*.07,h*.08));
        CGContextFillRect(c,CGRectMake(w*.23,h*.24,w*.36,h*.06));
        rounded(c,CGRectMake(w*.83,h*.21,w*.14,h*.49),unit*.06,false);break;
    case NATIVE_GLYPH_GAMEPAD:
        CGContextMoveToPoint(c,w*.22,h*.78);CGContextAddCurveToPoint(c,w*.12,h*.80,w*.04,h*.20,w*.15,h*.16);
        CGContextAddCurveToPoint(c,w*.26,h*.08,w*.32,h*.38,w*.39,h*.38);CGContextAddLineToPoint(c,w*.61,h*.38);
        CGContextAddCurveToPoint(c,w*.68,h*.38,w*.74,h*.08,w*.85,h*.16);CGContextAddCurveToPoint(c,w*.96,h*.20,w*.88,h*.80,w*.78,h*.78);
        CGContextClosePath(c);CGContextStrokePath(c);line(c,w*.22,h*.6,w*.38,h*.6);line(c,w*.3,h*.5,w*.3,h*.7);
        for(unsigned i=0;i<4;i++) {double angle=i*M_PI_2;CGContextFillEllipseInRect(c,CGRectMake(w*(.70+.08*cos(angle))-.025*w,h*(.60+.10*sin(angle))-.025*h,w*.05,h*.05));}break;
    }
    CGContextRelease(c);
    return true;
}

static void diagram_glyph(CGContextRef c,const NativePromptGlyph *g,CGRect r) {
    unsigned width=(unsigned)ceil(r.size.width*2),height=(unsigned)ceil(r.size.height*2);
    uint8_t *pixels=malloc((size_t)width*height);
    if(!pixels)return;
    if(native_prompt_raster(g,pixels,width,height)) {
        CGDataProviderRef provider=CGDataProviderCreateWithData(NULL,pixels,(size_t)width*height,NULL);
        CGColorSpaceRef gray=CGColorSpaceCreateDeviceGray();
        CGImageRef mask=CGImageCreate(width,height,8,8,width,gray,kCGImageAlphaNone,provider,NULL,false,kCGRenderingIntentDefault);
        CGContextSaveGState(c);CGContextClipToMask(c,r,mask);
        CGContextSetRGBFillColor(c,.96,.96,.90,1);CGContextFillRect(c,r);CGContextRestoreGState(c);
        CGImageRelease(mask);CGColorSpaceRelease(gray);CGDataProviderRelease(provider);
    }
    free(pixels);
}
static void diagram_prompt(CGContextRef c,const NativePromptSnapshot *s,unsigned code,double x,double y,double w,double h) {
    NativePromptGlyph g;
    if(native_prompt_glyph(code,s,&g))diagram_glyph(c,&g,CGRectMake(x-w/2,340-y-h/2,w,h));
}
static void diagram_key(CGContextRef c,const char *name,double x,double y) {
    NativePromptGlyph g={NATIVE_GLYPH_KEY,name,0};
    diagram_glyph(c,&g,CGRectMake(x-17,340-y-17,34,34));
}

bool native_prompt_diagram(const NativePromptSnapshot *s,uint8_t *pixels,unsigned width,unsigned height) {
    if(!s || !pixels || !width || !height || width>1024 || height>1024)return false;
    memset(pixels,0,(size_t)width*height*4);
    CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
    CGContextRef c=CGBitmapContextCreate(pixels,width,height,8,width*4,rgb,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(rgb);if(!c)return false;
    CGContextScaleCTM(c,width/580.0,height/340.0);
    CGContextSetRGBFillColor(c,.12,.14,.15,.92);
    CGContextSetRGBStrokeColor(c,.83,.85,.80,.8);CGContextSetLineWidth(c,2);
    // Functional groups keep the game's callout anchors while avoiding a
    // picture of a different controller. Every button uses the live mapping.
    const CGRect groups[]={CGRectMake(64,181,158,126),CGRectMake(175,120,94,90),
        CGRectMake(329,110,82,118),CGRectMake(410,164,132,130),CGRectMake(333,264,70,42),CGRectMake(134,309,70,30)};
    for(unsigned i=0;i<sizeof groups/sizeof *groups;i++) {
        rounded(c,groups[i],12,true);rounded(c,groups[i],12,false);
    }
    if(s->device==NATIVE_PROMPT_KEYBOARD) {
        diagram_key(c,"W",141,66);diagram_key(c,"A",102,105);
        diagram_key(c,"S",141,105);diagram_key(c,"D",180,105);
        diagram_prompt(c,s,0xe056,141,143,30,24);
    }else diagram_prompt(c,s,0xe056,141,105,78,78);
    diagram_prompt(c,s,0xe046,169,15,58,25);
    diagram_prompt(c,s,0xe04b,368,55,55,32);
    diagram_prompt(c,s,0xe049,222,175,70,65);
    diagram_prompt(c,s,0xe068,370,170,63,90);
    diagram_prompt(c,s,0xe042,476,66,34,31);
    diagram_prompt(c,s,0xe043,438,105,34,34);
    diagram_prompt(c,s,0xe040,514,105,34,34);
    diagram_prompt(c,s,0xe041,476,144,40,31);
    static const char *names[]={"Keyboard + Mouse","Xbox","PlayStation","Nintendo Switch","Controller"};
    CGContextSetRGBFillColor(c,.13,.15,.16,1);
    label(c,names[s->device],CGRectMake(95,45,390,24));
    CGContextRelease(c);
    // UI textures use straight alpha; CoreGraphics produces premultiplied RGBA.
    for(size_t i=0;i<(size_t)width*height;i++) {
        uint8_t *p=pixels+i*4;unsigned a=p[3];
        if(a && a<255)for(unsigned channel=0;channel<3;channel++)
            p[channel]=(uint8_t)((p[channel]*255u+a/2)/a);
    }
    return true;
}
