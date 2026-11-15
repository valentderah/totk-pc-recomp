#include <assert.h>
#include <math.h>
#include <string.h>
#include "rsf_fg_map.h"
static int close_to(float a,float b){return fabsf(a-b)<1e-4f;}
static void identity(float m[16]) {memset(m,0,64);for(int i=0;i<4;i++)m[i*5]=1;}
static void transform(const float p[4],const float m[16],float out[4]) {for(int c=0;c<4;c++){out[c]=0;for(int r=0;r<4;r++)out[c]+=p[r]*m[r*4+c];}}
static rsf_frame_record record(void) {
    rsf_frame_record r={.render_width=1600,.render_height=900,.camera={.render_width=1600,.render_height=900,.jitter_pixels={0.25f,-0.5f}}};
    identity(r.camera.view_to_clip);identity(r.camera.clip_to_view);identity(r.camera.clip_to_previous_clip);return r;
}
int main(void) {
    RsfFgMap full={1920,1080,2560,1440,{0,0,1920,1080},{0,0,2560,1440}};
    int p[2];float scale[2]; assert(rsf_fg_map_valid(&full));
    assert(rsf_fg_map_sample(&full,1600,900,0,0,p)&&p[0]==0&&p[1]==0);
    assert(rsf_fg_map_sample(&full,1600,900,2559,1439,p)&&p[0]==1599&&p[1]==899);
    rsf_fg_map_motion(&full,800,450,scale);assert(close_to(scale[0],3.2f)&&close_to(scale[1],3.2f));
    rsf_frame_record r=record();assert(rsf_fg_map_camera(&full,NULL,&r)); assert(r.flags&RSF_FRAME_FLAG_RESET);
    assert(r.render_width==2560&&r.camera.render_height==1440&&close_to(r.camera.jitter_pixels[0],0.4f));
    RsfFgMap bars={1920,1080,2560,1600,{0,0,1920,1080},{0,80,2560,1440}};
    assert(!rsf_fg_map_sample(&bars,1600,900,0,79,p));assert(rsf_fg_map_sample(&bars,1600,900,0,80,p)&&p[1]==0);
    assert(!rsf_fg_map_sample(&bars,1600,900,2559,1520,p));
    r=record();assert(rsf_fg_map_camera(&bars,&bars,&r)); assert(!(r.flags&RSF_FRAME_FLAG_RESET)); assert(close_to(r.camera.view_to_clip[5],0.9f));
    RsfFgMap crop={1920,1080,1200,900,{240,120,1280,720},{0,112,1200,675}};
    assert(rsf_fg_map_sample(&crop,1600,900,0,112,p)&&p[0]==200&&p[1]==100);
    r=record();rsf_frame_record original=r;assert(rsf_fg_map_camera(&crop,&bars,&r));
    float a[4]={0.2f,-0.3f,0.5f,1},b[4],c[4],previous[4];
    transform(a,r.camera.view_to_clip,b);transform(b,r.camera.clip_to_view,c);
    for(int k=0;k<4;k++)assert(close_to(a[k],c[k]));
    // Reprojection: host crop coordinates -> original clip -> old host bars.
    transform(b,r.camera.clip_to_previous_clip,previous);
    rsf_frame_record old=original;assert(rsf_fg_map_camera(&bars,&bars,&old));transform(a,old.camera.view_to_clip,c);
    for(int k=0;k<4;k++)assert(close_to(previous[k],c[k]));
    assert(r.flags&RSF_FRAME_FLAG_RESET);
    // Exhaustively cover fractional game-to-render crop edges and all bar pixels.
    for(int y=0;y<900;y++)for(int x=0;x<1200;x++) {
        int inside=rsf_fg_map_sample(&crop,853,479,x,y,p);
        assert(inside==(y>=112&&y<787));
        if(inside){int ex=(int)floor(((x+0.5)/1200*1280+240)*853/1920);assert(p[0]==ex);assert(p[0]>=0&&p[0]<853&&p[1]>=0&&p[1]<479);}
    }
    RsfFgMap bad=crop;bad.crop[2]=2000;assert(!rsf_fg_map_valid(&bad));bad=crop;bad.dst[0]=-1;assert(!rsf_fg_map_valid(&bad));
    return 0;
}
