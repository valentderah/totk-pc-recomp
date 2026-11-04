// Test the pure policy functions used by the present path without the game or renderer.
#include <stdio.h>
#include "ui/perf_mode.h"
int main(void) {
    const char *values[] = {"off", "fps", "full"};
    int failures = 0;
    for (int display=0; display<3; display++) {
        int mode=perf_display_mode(values[display]);
        for (int window=0; window<2; window++) {
            int sample=perf_sampling_mode(mode,window);
            if (mode!=display || sample!=(window?2:display)) failures++;
            printf("display=%s window=%d -> in_game=%d sampling=%d\n",values[display],window,mode,sample);
        }
        if(perf_sampling_mode(mode,0)!=display) failures++;
        if((mode+1)%3 != perf_display_mode(values[(display+1)%3])) failures++;
    }
    if(perf_display_mode(NULL)!=0 || perf_display_mode("invalid")!=0) failures++;
    printf("perf mode probe: %d failures\n",failures); return failures?1:0;
}
