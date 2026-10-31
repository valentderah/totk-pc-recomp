#version 450
#extension GL_GOOGLE_include_directive : require
#include "motion_uniform.glsl"
layout(location=0) in vec4 position;
layout(location=6) in uvec4 indices0;
#if INFLUENCES > 1
layout(location=4) in vec4 weights0;
#endif
#if INFLUENCES == 7
layout(location=5) in vec4 weights1;
layout(location=7) in uvec4 indices1;
#endif
layout(location=0) out vec4 previousClip;
layout(location=1) flat out uint poseValid;
vec4 project(Pose pose,uint count,out bool valid) {
    valid=true;
    vec4 a=vec4(0), b=vec4(0), c=vec4(0);
    for(int i=0;i<INFLUENCES;i++) {
#if INFLUENCES == 1
        uint index=indices0.x; float weight=1;
#elif INFLUENCES == 7
        uint index=i<4?indices0[i]:indices1[i-4]; float weight=i<4?weights0[i]:weights1[i-4];
#else
        uint index=indices0[i]; float weight=weights0[i];
#endif
        if(index>=count || index>=128u) { valid=false; continue; }
        a+=pose.bones[index*3]*weight;
        b+=pose.bones[index*3+1]*weight;
        c+=pose.bones[index*3+2]*weight;
    }
    vec4 p=vec4(position.xyz,1);
    vec4 world=vec4(vec3(dot(a,p),dot(b,p),dot(c,p))+pose.origin.xyz,1);
    vec4 view=vec4(dot(pose.view[0],world),dot(pose.view[1],world),dot(pose.view[2],world),1);
    return vec4(dot(pose.projection[0],view),dot(pose.projection[1],view),dot(pose.projection[2],view),dot(pose.projection[3],view));
}
void main() {
    bool currentValid,previousValid;
    previousClip=project(u.old,uint(u.viewport.w),previousValid);
    gl_Position=project(u.now,uint(u.viewport.z),currentValid);
    poseValid=(currentValid?1u:0u)|(previousValid?2u:0u);
    gl_Position.z=(gl_Position.z+gl_Position.w)*0.5;
}
