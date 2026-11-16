#include "rsf_fg_map.h"
#include <math.h>
#include <string.h>
int rsf_fg_map_valid(const RsfFgMap *m) {
    if (!m || !m->game_w || !m->game_h || !m->out_w || !m->out_h) return 0;
    for (int k=0;k<2;k++) {
        unsigned g=k?m->game_h:m->game_w, o=k?m->out_h:m->out_w;
        if (m->crop[k]<0 || m->crop[k+2]<=0 || m->dst[k]<0 || m->dst[k+2]<=0 ||
            (uint64_t)m->crop[k]+m->crop[k+2]>g || (uint64_t)m->dst[k]+m->dst[k+2]>o) return 0;
    }
    return 1;
}
int rsf_fg_map_equal(const RsfFgMap *a,const RsfFgMap *b) { return !memcmp(a,b,sizeof *a); }
void rsf_fg_map_motion(const RsfFgMap *m,unsigned sw,unsigned sh,float s[2]) {
    s[0]=(float)((double)m->dst[2]*m->game_w/(m->crop[2]*(double)sw));
    s[1]=(float)((double)m->dst[3]*m->game_h/(m->crop[3]*(double)sh));
}
static void mul(const float a[16],const float b[16],float out[16]) {
    float t[16]={0}; for(int r=0;r<4;r++) for(int c=0;c<4;c++) for(int k=0;k<4;k++) t[r*4+c]+=a[r*4+k]*b[k*4+c];
    memcpy(out,t,sizeof t);
}
static void affine(const RsfFgMap *m,float a[16],float inv[16]) {
    memset(a,0,64); a[10]=a[15]=1;
    if(inv) { memset(inv,0,64); inv[10]=inv[15]=1; }
    for(int k=0;k<2;k++) {
        double g=k?m->game_h:m->game_w,o=k?m->out_h:m->out_w;
        double s=m->dst[k+2]*g/(m->crop[k+2]*o);
        double t=(2.0*m->dst[k]+m->dst[k+2])/o-1-s*((2.0*m->crop[k]+m->crop[k+2])/g-1);
        if(k) t=-t; // Window Y points down; clip Y points up.
        a[k*5]=(float)s; a[12+k]=(float)t;
        if(inv) { inv[k*5]=(float)(1/s); inv[12+k]=(float)(-t/s); }
    }
}
int rsf_fg_map_camera(const RsfFgMap *m,const RsfFgMap *old,rsf_frame_record *r) {
    if(!rsf_fg_map_valid(m)||!r||!r->render_width||!r->render_height) return 0;
    if(!old||!rsf_fg_map_valid(old)) { old=m; r->flags|=RSF_FRAME_FLAG_RESET; }
    float a[16],ai[16],p[16],s[2]; affine(m,a,ai); affine(old,p,NULL);
    mul(r->camera.view_to_clip,a,r->camera.view_to_clip);
    mul(ai,r->camera.clip_to_view,r->camera.clip_to_view);
    mul(ai,r->camera.clip_to_previous_clip,r->camera.clip_to_previous_clip);
    mul(r->camera.clip_to_previous_clip,p,r->camera.clip_to_previous_clip);
    rsf_fg_map_motion(m,r->render_width,r->render_height,s);
    for(int k=0;k<2;k++) r->camera.jitter_pixels[k]*=s[k];
    rsf_fg_map_motion(old,r->render_width,r->render_height,s);
    for(int k=0;k<2;k++) r->camera.previous_jitter_pixels[k]*=s[k];
    r->render_width=r->camera.render_width=m->out_w; r->render_height=r->camera.render_height=m->out_h;
    r->output_width=r->camera.output_width=m->out_w; r->output_height=r->camera.output_height=m->out_h;
    // Full host frustum, including off-centre crop and bars: evaluate top/bottom rays.
    float *v=r->camera.clip_to_view;
    float top[3],bot[3];
    for(int k=0;k<3;k++) { top[k]=v[4+k]+v[12+k]; bot[k]=-v[4+k]+v[12+k]; }
    // Perspective directions need the z=1 clip plane, not the origin.
    for(int k=0;k<3;k++) { top[k]+=v[8+k]; bot[k]+=v[8+k]; }
    if(fabs(top[2])>1e-12&&fabs(bot[2])>1e-12)
        r->camera.vertical_fov_radians=(float)fabs(atan2(top[1],fabs(top[2]))-atan2(bot[1],fabs(bot[2])));
    if(!rsf_fg_map_equal(m,old)) r->flags|=RSF_FRAME_FLAG_RESET;
    return 1;
}
