#include <assert.h>
#include <string.h>
#include "rsf_fg_history.h"
static rsf_frame_record record(uint64_t frame, uint32_t generation) {
    return (rsf_frame_record){.struct_size=sizeof(rsf_frame_record),.abi_version=RSF_GAME_FRAME_ABI_VERSION,
        .session_id=3,.frame_id=frame,.view_id=2,.resource_generation=generation};
}
int main(void) {
    RsfFgHistory h={0};
    RsfFgMap map={.game_w=1600,.game_h=900,.out_w=1920,.out_h=1080,
        .crop={0,0,1600,900},.dst={0,0,1920,1080}};
    rsf_frame_record n=record(100,7), next=record(101,8);
    assert(rsf_fg_history_remember(&h,&n,&map));
    assert(rsf_fg_history_remember(&h,&next,&map));
    // Destroy caller/backing metadata: accepted CPU history owns a value copy.
    n.resource_generation=0;next.resource_generation=999;
    RsfFgSnapshot pending;
    assert(rsf_fg_history_snapshot(&h,100,&map,&pending));
    assert(pending.record.frame_id==100 && pending.record.session_id==3 && pending.record.view_id==2 && pending.record.resource_generation==7);
    rsf_frame_record expected=record(100,7);
    assert(rsf_fg_history_matches(&h,&expected));
    expected.resource_generation=8;assert(!rsf_fg_history_matches(&h,&expected));
    expected.resource_generation=0;assert(!rsf_fg_history_matches(&h,&expected));
    RsfFgMap changed=map;changed.dst[0]=1;changed.dst[2]--;
    RsfFgSnapshot refused;
    assert(!rsf_fg_history_snapshot(&h,100,&changed,&refused) && !refused.record.frame_id);
    assert(!rsf_fg_history_snapshot(&h,100,NULL,&refused));
    assert(!rsf_fg_history_snapshot(&h,0,&map,&refused));
    assert(!rsf_fg_history_snapshot(&h,999,&map,&refused));
    for(unsigned i=0;i<RSF_FG_HISTORY_COUNT;i++) {
        rsf_frame_record later=record(200+i,10+i);
        assert(rsf_fg_history_remember(&h,&later,&map));
    }
    assert(!rsf_fg_history_snapshot(&h,100,&map,&refused)); // truly expired, never latest
    assert(pending.record.frame_id==100 && pending.record.resource_generation==7); // pending image immutable
    rsf_fg_history_clear(&h);
    assert(!rsf_fg_history_snapshot(&h,263,&map,&refused));
    expected=record(500,1);assert(rsf_fg_history_remember(&h,&expected,&map));
    expected.resource_generation=2;assert(!rsf_fg_history_remember(&h,&expected,&map));
    assert(!rsf_fg_history_snapshot(&h,500,&map,&refused)); // ambiguous source refused
    rsf_fg_history_clear(&h);
    expected=record(600,0);assert(!rsf_fg_history_remember(&h,&expected,&map));
    expected.resource_generation=1;expected.flags=RSF_FRAME_FLAG_NO_FG;assert(!rsf_fg_history_remember(&h,&expected,&map));
    return 0;
}
