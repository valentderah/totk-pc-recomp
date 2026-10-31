#version 450
layout(std140,set=0,binding=0) uniform Calibration {vec4 now[4];vec4 old[4];vec4 size;} u;
layout(location=0) out vec4 previousClip;
void main(){
    // Pixel-aligned triangle plus binary fractional jitter avoids vertex subpixel rounding.
    // Distinct homogeneous W exercises perspective-correct interpolation, not just an affine quad.
    const vec4 vertices[3]=vec4[3](vec4(-.625,-.5,-.5,1),vec4(1.5,-1,0,2),vec4(-.125,.375,.25,.5));
    vec4 p=vertices[gl_VertexIndex];
    previousClip=vec4(dot(u.old[0],p),dot(u.old[1],p),dot(u.old[2],p),dot(u.old[3],p));
    gl_Position=vec4(dot(u.now[0],p),dot(u.now[1],p),dot(u.now[2],p),dot(u.now[3],p));
    gl_Position.z=(gl_Position.z+gl_Position.w)*.5;
}
