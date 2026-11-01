#version 450
#extension GL_GOOGLE_include_directive : require
#include "motion_uniform.glsl"
layout(set=0,binding=1) uniform sampler2D currentDepth;
layout(set=0,binding=2) uniform sampler2D previousDepth;
layout(location=0) in vec4 previousClip;
layout(location=1) flat in uint historyValid;
layout(location=0) out vec4 result;
void main() {
    ivec2 size=ivec2(u.viewport.xy), pixel=ivec2(gl_FragCoord.xy);
    float z=texelFetch(currentDepth,pixel,0).r;
    if(z<0 || z>=1 || abs(z-gl_FragCoord.z)>0.000002) discard;
    result=vec4(0,0,gl_FragCoord.z,-1);
    if(historyValid==0 || previousClip.w<=0) return;
    vec3 old=previousClip.xyz/previousClip.w;
    vec2 previousPixel=vec2((old.x+1)*0.5*u.viewport.x,(1-old.y)*0.5*u.viewport.y);
    if(any(lessThan(previousPixel,vec2(0))) || any(greaterThanEqual(previousPixel,u.viewport.xy)) || old.z< -1 || old.z>1) return;
    vec2 samplePixel=clamp(previousPixel-0.5,vec2(0),vec2(size-1));
    ivec2 p=ivec2(floor(samplePixel)), q=min(p+1,size-1);vec2 f=fract(samplePixel);
    float za=mix(texelFetch(previousDepth,p,0).r,texelFetch(previousDepth,ivec2(q.x,p.y),0).r,f.x);
    float zb=mix(texelFetch(previousDepth,ivec2(p.x,q.y),0).r,texelFetch(previousDepth,q,0).r,f.x);
    float oldZ=mix(za,zb,f.y);
    if(oldZ<0 || oldZ>=1 || abs(oldZ-(old.z+1)*0.5)>0.000002) return;
    result=vec4(previousPixel-gl_FragCoord.xy,gl_FragCoord.z,1);
}
