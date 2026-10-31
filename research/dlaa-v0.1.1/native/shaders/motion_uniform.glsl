struct Pose { vec4 view[3]; vec4 projection[4]; vec4 origin; vec4 bones[384]; };
layout(std140,set=0,binding=0) uniform Transforms { Pose now; Pose old; vec4 viewport; } u;
