#version 450
layout(std140,set=0,binding=0) uniform Camera {vec4 rows[4];vec4 viewport;} u;
layout(set=0,binding=1) uniform sampler2D currentDepth;
layout(set=0,binding=2) uniform sampler2D previousDepth;
layout(set=0,binding=3) uniform sampler2D skinMotion;
layout(set=0,binding=4) uniform sampler2D currentColor;
layout(set=0,binding=5) uniform sampler2D previousColor;
layout(location=0) out vec4 result;
layout(location=1) out vec4 composite;
layout(location=2) out float uncertainty;
layout(location=3) out vec2 ngxMotion;
vec4 cameraMotion(){
    ivec2 size=ivec2(u.viewport.xy),pixel=ivec2(gl_FragCoord.xy);
    float z=texelFetch(currentDepth,pixel,0).r;
    if(isnan(z)||isinf(z)||z<0||z>=1)return vec4(0);
    vec4 clip=vec4(2*gl_FragCoord.x/u.viewport.x-1,1-2*gl_FragCoord.y/u.viewport.y,2*z-1,1);
    vec4 oldClip=vec4(dot(u.rows[0],clip),dot(u.rows[1],clip),dot(u.rows[2],clip),dot(u.rows[3],clip));
    if(any(isnan(oldClip))||any(isinf(oldClip))||oldClip.w<=1e-10)return vec4(0);
    vec3 old=oldClip.xyz/oldClip.w;
    vec2 previousPixel=vec2((old.x+1)*.5*u.viewport.x,(1-old.y)*.5*u.viewport.y);
    if(any(lessThan(previousPixel,vec2(0)))||any(greaterThanEqual(previousPixel,u.viewport.xy))||old.z< -1||old.z>1)return vec4(0);
    vec2 samplePixel=clamp(previousPixel-.5,vec2(0),vec2(size-1));
    ivec2 p=ivec2(floor(samplePixel)),q=min(p+1,size-1);vec2 f=fract(samplePixel);
    float a=mix(texelFetch(previousDepth,p,0).r,texelFetch(previousDepth,ivec2(q.x,p.y),0).r,f.x);
    float b=mix(texelFetch(previousDepth,ivec2(p.x,q.y),0).r,texelFetch(previousDepth,q,0).r,f.x);
    float oldZ=mix(a,b,f.y);
    if(isnan(oldZ)||isinf(oldZ)||oldZ<0||oldZ>=1||abs(oldZ-(old.z+1)*.5)>.000002)return vec4(0);
    return vec4(previousPixel-gl_FragCoord.xy,z,1);
}
void main(){
    result=cameraMotion();composite=vec4(0);uncertainty=1;ngxMotion=vec2(0);
    if(u.viewport.z<.5)return;
    ivec2 pixel=ivec2(gl_FragCoord.xy),size=ivec2(u.viewport.xy);
    vec4 skin=texelFetch(skinMotion,pixel,0);bool known=skin.w>.5;
    if(!known&&(skin.w<0||result.w<.5))return;
    vec4 chosen=known?skin:result;
    vec2 old=gl_FragCoord.xy+chosen.xy;
    if(any(lessThan(old,vec2(0)))||any(greaterThanEqual(old,u.viewport.xy)))return;
    vec2 samplePixel=clamp(old-.5,vec2(0),vec2(size-1));ivec2 p=ivec2(floor(samplePixel)),q=min(p+1,size-1);vec2 f=fract(samplePixel);
    vec3 a=mix(texelFetch(previousColor,p,0).rgb,texelFetch(previousColor,ivec2(q.x,p.y),0).rgb,f.x);
    vec3 b=mix(texelFetch(previousColor,ivec2(p.x,q.y),0).rgb,texelFetch(previousColor,q,0).rgb,f.x);
    vec3 previous=mix(a,b,f.y),current=texelFetch(currentColor,pixel,0).rgb;
    vec2 uv=gl_FragCoord.xy/u.viewport.xy;
    // Controlled diagnostic input only; never modify the game's color image.
    if(u.viewport.w>.5&&uv.x>.1&&uv.x<.2&&uv.y>.1&&uv.y<.2)current+=vec3(1);
    if(any(isnan(current))||any(isinf(current))||any(isnan(previous))||any(isinf(previous))||any(lessThan(current,vec3(0)))||any(lessThan(previous,vec3(0))))return;
    vec3 delta=abs(log2(1+current)-log2(1+previous));
    uncertainty=clamp((max(delta.x,max(delta.y,delta.z))-.04)/.16,0,1);
    if(known||uncertainty<.5){composite=vec4(chosen.xyz,known?1:2);ngxMotion=chosen.xy;}
}
