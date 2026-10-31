#version 450
layout(triangles) in;
layout(triangle_strip,max_vertices=3) out;
layout(location=0) in vec4 inputPreviousClip[];
layout(location=1) flat in uint poseValid[];
layout(location=0) out vec4 previousClip;
layout(location=1) flat out uint historyValid;
void main() {
    if((poseValid[0]&1u)==0 || (poseValid[1]&1u)==0 || (poseValid[2]&1u)==0) return;
    for(int i=0;i<3;i++) {
        gl_Position=gl_in[i].gl_Position;
        previousClip=inputPreviousClip[i];
        historyValid=((poseValid[0]&poseValid[1]&poseValid[2]&2u)!=0)?1u:0u;
        EmitVertex();
    }
    EndPrimitive();
}
