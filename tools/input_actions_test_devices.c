// Bounded physical-input fixture for research builds; no input is supplied without its explicit script environment.
#include "input_actions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
extern int64_t frame_step_world_us(void);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static struct { int key,pad;int64_t start,end; } pulses[256];
static unsigned count;
static void init(void) {
    const char *path=getenv("TOTK_ACTION_TEST_SCRIPT");if(!path || !*path)return;
    FILE *f=fopen(path,"r");if(!f)return;
    char line[256];int valid=1;
    while(fgets(line,sizeof line,f)) {
        if(line[0]=='#' || strspn(line," \r\n\t")==strlen(line))continue;
        long long start,duration;char device[80],extra;
        if(sscanf(line,"%lld %79s %lld %c",&start,device,&duration,&extra)!=3 || start<0 || start>3600000 ||
           duration<=0 || duration>60000 || count==256){valid=0;break;}
        int key=0,pad=0;
        if(!strncasecmp(device,"key:",4)) for(int i=1;i<KEY_COUNT;i++)if(!strcasecmp(device+4,input_key_name(i)))key=i;
        if(!strncasecmp(device,"pad:",4)) for(int i=1;i<PAD_COUNT;i++)if(!strcasecmp(device+4,input_pad_name(i)))pad=i;
        if(!key && !pad){valid=0;break;}
        pulses[count].key=key;pulses[count].pad=pad;pulses[count].start=start*1000;pulses[count].end=(start+duration)*1000;count++;
    }
    if(ferror(f))valid=0;fclose(f);if(!valid)count=0;
}
void input_actions_test_devices(uint8_t keys[KEY_COUNT],uint8_t pads[PAD_COUNT]) {
    pthread_once(&once,init);int64_t clock=frame_step_world_us();
    for(unsigned i=0;i<count;i++)if(clock>=pulses[i].start && clock<pulses[i].end) {
        if(pulses[i].key)keys[pulses[i].key]=1;if(pulses[i].pad)pads[pulses[i].pad]=1;
    }
}

int input_actions_test_scripted(void) { pthread_once(&once,init);return count!=0; }
