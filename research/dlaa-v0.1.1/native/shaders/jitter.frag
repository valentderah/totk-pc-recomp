#version 450
layout(std140,set=0,binding=0) uniform Calibration {vec4 now[4];vec4 old[4];vec4 size;} u;
layout(location=0) in vec4 previousClip;
layout(location=0) out vec4 result;
void main(){
    vec2 previous=previousClip.xy/previousClip.w;
    vec2 oldPixel=vec2((previous.x+1)*.5*u.size.x,(1-previous.y)*.5*u.size.y);
    result=vec4(oldPixel-gl_FragCoord.xy,gl_FragCoord.z,1);
}
