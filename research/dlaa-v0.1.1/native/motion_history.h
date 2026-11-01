#pragma once
#include <stdint.h>
#include <string.h>
#define MH_CAP 128
typedef struct { uint64_t a, b; int duplicate; } MhSlot;
typedef struct { MhSlot slot[2][MH_CAP]; int n[2], side, ready, sealed, invalid; uint64_t frame; } Mh;
static float mh_camera_distance2(const float now[3][4],const float old[3][4]) {
    float distance=0;
    for(int j=0;j<3;j++) {
        float a=0,b=0;
        for(int i=0;i<3;i++)a-=now[i][j]*now[i][3],b-=old[i][j]*old[i][3];
        float d=a-b;distance+=d*d;
    }
    return distance;
}
static void mh_begin(Mh *h, uint64_t frame) {
    if (h->frame == frame) return;
    h->ready = h->sealed == 1 && h->frame && frame == h->frame + 1;
    h->side ^= 1; h->n[h->side] = 0; h->frame = frame; h->sealed = 0; h->invalid = 0;
}
static int mh_add(Mh *h, uint64_t a, uint64_t b) {
    if (h->sealed || h->n[h->side] == MH_CAP) { if (!h->sealed) h->ready = 0, h->invalid = 1; return -1; }
    int index = h->n[h->side]++; MhSlot *s = &h->slot[h->side][index];
    *s = (MhSlot){a, b, 0};
    for (int i = 0; i < index; i++)
        if (h->slot[h->side][i].a == a && h->slot[h->side][i].b == b)
            h->slot[h->side][i].duplicate = s->duplicate = 1;
    return index;
}
static int mh_previous(const Mh *h, int index) {
    if (!h->ready || h->invalid || index < 0 || index >= h->n[h->side]) return -1;
    const MhSlot *s = &h->slot[h->side][index];
    if (s->duplicate) return -1;
    int result = -1;
    for (int i = 0; i < h->n[h->side ^ 1]; i++) {
        const MhSlot *p = &h->slot[h->side ^ 1][i];
        if (s->a == p->a && s->b == p->b) {
            if (p->duplicate || result >= 0) return -1;
            result = i;
        }
    }
    return result;
}
static int mh_seal(Mh *h) {
    if (h->sealed || h->invalid) { h->sealed = 2; h->ready = 0; return 0; }
    h->sealed = 1; return 1;
}
