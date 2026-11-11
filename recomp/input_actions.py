"""Install-time independent-controls adapter. No game code, verb table or generated configuration is shipped here.

The authored 1.4.3 compatibility layout follows the versioned-layout convention of runtime/mouse_direct.c.
The player fields and guard-key design adapt Weeping of the Empire's totk_player_state.cpp and
patches/sites_v143.py at fe2345e82a43156183242abfdcc0eb1f5526eba6, Copyright (c) 2026 daniel-madrid-07,
MIT License (tools/licenses/weeping-player-state.txt). Other selectors implement this port's action semantics.
Only the upstream-verified executable is accepted. Its actual enum table and instructions are read and checked
on the user's machine; resulting data and rewritten code belong in the generated output directory.
"""
import bisect
import copy
import hashlib
import json
from collections import OrderedDict
from pathlib import Path
import re
import struct

# Compatibility interfaces, not a dumped game table. Offsets are relative to the module or containing object.
PUBLISHER = 0xf23d70
INTERACTION = 0x5e2d54
PLAYER_GLOBAL = 0x3969918
REGISTRY_GLOBAL = 0x3add388
GUARD_CALLS = (0x23eb604, 0x23ebba8, 0x23ebbc8, 0x23eb010, 0x23f5220, 0x27891bc)
GUARD_BYTE = 0x3e3edc
FIELDS = dict(root_link=0x20, link_data=8, link_id=0x10, unit_proc=0x40, unit_id=0x48,
              proc_state=0x24, actor_components=0x228, as_component=0x158, player_component=0x3a8,
              attention_component=0x128, power_component=0x478, as_slots=0x1f0, slot_verb=0x28,
              slot_alternate=0x4c, state_bytes=0x145c, swim_mask=1<<4, climb_mask=1<<10,
              attention_state=0xe8, power_current=0x6c, power_hand=0x28, hand_state=0x18, holding_ability=1)
BODY_GROUPS = {
    1<<1: 'WallSlip WallJump Clamber DashUp Climb ClimbLadder WaterfallClimbing',
    1<<2: 'Glide Jump JumpToCeiling JumpOffCeiling AirDive GuideDive RideJump Fall JumpAttack RideJumpAttack InterruptFall FallAttack',
    1<<3: 'HangingFlight ParasailGlide AnimalGlide',
    1<<4: 'WaterDive Swim WaterJump',
    1<<9: 'Ride RideDragon',
    1<<10: 'RideRobot RideSled RideTelescopeBase',
    1<<11: 'Surf',
}
ANY_GROUPS = {1<<5: 'UseBow DrawnBow', 1<<6: 'GuardKeep', 1<<8: 'UseSpecialPower',
              1<<12: 'Talk Listen', 1<<13: 'Scope TakingPhoto'}
POWERS = {1:'ULTRAHAND', 2:'FUSE', 3:'ASCEND', 4:'RECALL', 5:'AUTOBUILD', 6:'CAMERA', 8:'AMIIBO'}
NATIVE = {'Scope':'SCOPE', 'TakingPhoto':'CAMERA', 'Surf':'SURF', 'RideRobot':'ROBOT', 'RideSled':'SLED'}


def murmur3(data):
    """MurmurHash3 x86-32, seed zero: the registry hashes its row names this way."""
    mask, h = 0xffffffff, 0
    def rot(x, n): return ((x << n) | (x >> (32-n))) & mask
    end = len(data)//4*4
    for i in range(0,end,4):
        k = int.from_bytes(data[i:i+4], 'little')
        k = rot(k*0xcc9e2d51 & mask,15)*0x1b873593 & mask
        h = (rot(h^k,13)*5+0xe6546b64) & mask
    if end != len(data):
        h ^= rot(int.from_bytes(data[end:],'little')*0xcc9e2d51 & mask,15)*0x1b873593 & mask
    h ^= len(data); h ^= h>>16; h = h*0x85ebca6b & mask; h ^= h>>13; h = h*0xc2b2ae35 & mask
    return h^(h>>16)


def verb_names(module):
    """Locate the enum through relocated string pointers; never distribute its extracted contents."""
    rel = {at:value for at,kind,_,value in module.nso.relocs if kind==1027}
    def name(at):
        value = rel.get(at, -1)
        if not 0<=value<len(module.image):return None
        end = module.image.find(b'\0',value,min(value+128,len(module.image)))
        if end<0:return None
        text = module.image[value:end]
        return text.decode('ascii') if re.fullmatch(rb'[A-Za-z][A-Za-z0-9_]*',text) else None
    candidates = [at for at in rel if name(at)=='None' and name(at+8)=='Walk' and name(at+16)=='WalkBackwards']
    if len(candidates)!=1:raise ValueError('action enum table is ambiguous or absent')
    rows=[];at=candidates[0]
    while (value:=name(at+8*len(rows))) is not None and value not in rows:
        rows.append(value)
        if len(rows)>1024:raise ValueError('unbounded action enum table')
    if len(rows)!=284:raise ValueError('action enum layout differs from supported 1.4.3')
    required=set(NATIVE)|{'Ride','UseSpecialPower','Manipulate'}
    for group in [*BODY_GROUPS.values(),*ANY_GROUPS.values()]:required.update(group.split())
    if not required<=set(rows):raise ValueError('required action enum names are absent')
    return rows


def discover(module, path, starts, patched):
    """Return a validated adapter or an explicit native-controls fallback. Malformed known files fail closed."""
    result={'supported':False, 'reason':'unsupported executable'}
    expected=Path(__file__).resolve().parents[1]/'tools/data/expected-v1.4.3.json'
    digest=hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if module.name!='main' or not expected.is_file():return result
    known=json.loads(expected.read_text())['exefs_sha256']['main']
    if digest!=known:return result
    try:
        names=verb_names(module);ordered=sorted(map(int,starts))
        bounds=dict(zip(ordered,ordered[1:]+[module.hi]))
        for off in (PUBLISHER,INTERACTION):
            if module.base+off not in bounds:raise ValueError('action hook is not a discovered function')
        guards=[]
        for off in GUARD_CALLS:
            word=module.read(module.base+off,4)
            if word!=0x52800041:raise ValueError('guard argument is not MOV W1, #2')
            guards.append((off,word,0x52800361))  # virtual guard key 27, preserving native targeting key 2
        word=module.read(module.base+GUARD_BYTE,4)
        if word!=0x39400108:raise ValueError('inline guard byte read differs')
        guards.append((GUARD_BYTE,word,word|(3<<10)))
        word=module.read(module.base+GUARD_BYTE+4,4)
        if (word&0x7f00001f)!=0x37000008 or (word>>19)&31!=2:
            raise ValueError('inline guard bit test differs')
        guards.append((GUARD_BYTE+4,word,(word&~(31<<19))|(3<<19)))
        owners={module.base+PUBLISHER,module.base+INTERACTION}
        for off,_,_ in guards:
            at=module.base+off
            if not module.in_text(at):raise ValueError('guard instruction outside text')
            i=bisect.bisect_right(ordered,at)-1
            if i<0:raise ValueError('guard instruction has no function')
            owners.add(ordered[i])
        if any(lo<=site<bounds[lo] for lo in owners for site in patched):
            raise ValueError('a code patch overlaps an independent-controls consumer')
        if any(not module.in_image(module.base+off,8) or off%8 for off in (PLAYER_GLOBAL,REGISTRY_GLOBAL)):
            raise ValueError('adapter globals are outside module image')
        result.update(supported=True,reason='verified 1.4.3',base=module.base,end=module.base+len(module.image),
                      names=names,guards=guards,main_sha256=digest)
    except (ValueError,KeyError,IndexError,struct.error) as error:
        result['reason']=str(error)
    return result


def shadow_module(module, config):
    if not config.get('supported'):return module
    image=bytearray(module.image)
    for offset,before,after in config['guards']:
        if struct.unpack_from('<I',image,offset)[0]!=before:raise ValueError('guard instruction changed after validation')
        struct.pack_into('<I',image,offset,after)
    out=copy.copy(module);out.image=bytes(image)
    import numpy as np
    out.words=np.frombuffer(out.image,dtype='<u4',count=len(module.words),offset=module.lo-module.base)
    out._cache=OrderedDict()
    return out


def wrap_function(lines, entry, config):
    """Only gameplay/prompt hooks, with the original Cpu result preserved. No query probes or log calls."""
    if not config.get('supported'):return lines
    role=entry-config['base']
    if role not in (PUBLISHER,INTERACTION):return lines
    definition=f'void f_{entry:x}(Cpu *c) {{'
    if lines.count(definition)!=1:raise ValueError('unsupported action hook function shape')
    lines=list(lines);native=f'input_actions_original_{entry:x}'
    lines[lines.index(definition)]=f'static __attribute__((noinline)) void {native}(Cpu *c) {{'
    wrapper=[f'void f_{entry:x}(Cpu *c) {{','    uint64_t object=c->x[0];',f'    {native}(c);']
    if role==PUBLISHER:
        wrapper+=['    extern void input_actions_bridge_begin(uint64_t, uint64_t);',
                  '    extern uint64_t input_actions_bridge_apply(unsigned, uint64_t);',
                  f'    input_actions_bridge_begin({entry:#x}ull, object);']
        for phase,offset in enumerate((0x28,0x38,0x48)):
            wrapper += [f'    uint64_t p{phase}=object?LD64(object+{offset}):0;',
                        f'    if(p{phase})ST64(p{phase},input_actions_bridge_apply({phase},LD64(p{phase})));']
    else:
        wrapper+=['    extern int plat_mem_read(uint64_t,void *,size_t);',
                  '    extern void input_actions_interaction_observe(uint64_t,int64_t,int);',
                  '    int32_t selected=-2;uint64_t target=c->x[0];uint32_t value=0;',
                  '    if(!object || !plat_mem_read(object+0x38,&selected,4))selected=-2;',
                  '    if(!target || !plat_mem_read(target,&target,8))target=0;',
                  '    int valid=target && plat_mem_read(target+0x34,&value,4);',
                  '    input_actions_interaction_observe(object,selected,valid);']
    return lines+wrapper+['}']


def header(config, marker):
    out=['// '+marker,'// Install-time independent-controls configuration. Do not edit.',
         f'int input_actions_game_supported(void){{return {int(config.get("supported",False))};}}']
    supported=config.get('supported',False);names=config.get('names',[])
    def scalar(kind,name,value):out.append(f'{kind} {name}(void){{return {value};}}')
    base=config.get('base',0)
    scalar('uint64_t','input_actions_registry_slot',str(base+REGISTRY_GLOBAL if supported else 0)+'ull')
    for fn,name in [('pause','Custom_Controller_Player'),('off','Custom_Player'),('ui_pause','Custom_Controller_UI'),
                    ('menu_pause','Custom_ActorCalcDrawExceptPouchActor')]:
        scalar('uint32_t','input_actions_'+fn+'_hash',str(murmur3(name.encode()) if supported else 0)+'u')
    scalar('uint32_t','input_actions_lockout_offset',str(0x10fc if supported else 0))
    scalar('int','input_actions_ride_verb',str(names.index('Ride') if supported else -1))
    scalar('uint64_t','input_actions_guard_mask',str(1<<27 if supported else 0)+'ull')
    scalar('uint64_t','input_actions_guard_source',str(1<<2 if supported else 0)+'ull')
    # Virtual-key API: Jump, Sprint, Crouch, Interact, Attack, Bow, Throw, Guard, Cancel.
    masks=[[sum(1<<n for n in (0,10,19)),2,8,0,1<<9,0,0,0,1<<5],
           [1,2,0,0,1<<9,0,0,0,1<<5],[0]*9] if supported else [[0]*9 for _ in range(3)]
    out+=['static const uint64_t action_masks[3][9]={'+','.join('{'+','.join(str(x)+'ull' for x in row)+'}' for row in masks)+'};',
          'const uint64_t *input_actions_virtual_masks(unsigned phase){return action_masks[phase];}']
    owned=[]
    for phase,row in enumerate(masks):
        value=0
        for x in row:value|=x
        if supported and phase==0:value|=1<<26
        owned.append(value)
    out+=['uint64_t input_actions_virtual_owned(unsigned phase){static const uint64_t m[3]={'+','.join(str(x)+'ull' for x in owned)+'};return m[phase];}',
          'uint64_t input_actions_jump_mask(int kind,unsigned phase){']
    if supported:
        out+=['    if(kind==IC_SWIM){static const uint64_t m[3]={'+str((1<<19)|1)+'ull,1,0};return m[phase];}',
              '    if(kind==IC_RIDING){static const uint64_t m[3]={'+str((1<<26)|1)+'ull,1,0};return m[phase];}']
    else:out+=['    (void)kind;']
    out+=['    return action_masks[phase][IA_JUMP];}',
          'uint64_t input_actions_lockout_mask(unsigned phase){static const uint64_t m[3]={'+
          ','.join(str(x)+'ull' for x in ([0xf6000c,0xf40004,0] if supported else [0,0,0]))+'};return m[phase];}',
          'int input_actions_special_context(const int32_t verbs[5],int ability){']
    if supported:
        out+=['    for(unsigned i=0;i<5;i++){']
        for name,context in NATIVE.items():out.append(f'        if(verbs[i]=={names.index(name)})return IC_{context};')
        out.append(f'        if(verbs[i]=={names.index("UseSpecialPower")})switch(ability){{')
        out += [f'            case {n}:return IC_{context};' for n,context in POWERS.items()]
        out+=['        }','    }']
    else:out+=['    (void)verbs;(void)ability;']
    out+=['    return -1;}']
    if supported:
        body=[0]*len(names);any_flags=[0]*len(names);unknown=[0]*len(names)
        for mapping,values in ((BODY_GROUPS,body),(ANY_GROUPS,any_flags)):
            for flag,group in mapping.items():
                for name in group.split():values[names.index(name)]|=flag
        for name in ('RideJump','RideJumpAttack'):unknown[names.index(name)]|=1<<9
        # Manipulate also covers held devices: its name alone cannot establish steering-stick operation.
        unknown[names.index('Manipulate')]|=1<<10
        for label,values in [('body_flags',body),('any_flags',any_flags),('body_unknown',unknown)]:
            out.append('static const uint32_t '+label+'[]={'+','.join(map(str,values))+'};')
        def union(values):
            v=0
            for x in values:v|=x
            return v
        layout=dict(main_start=base,main_end=config['end'],global_slot=base+PLAYER_GLOBAL,site=base+PUBLISHER,
                    **FIELDS,invalid_verb=len(names),verb_count=len(names),body_known=union(body),any_known=union(any_flags))
        out+=['static const struct InputPlayerLayout player_layout={']
        out += [f'    .{key}={value}ull,' for key,value in layout.items()]
        out+=['    .body_flags=body_flags,.any_flags=any_flags,.body_unknown=body_unknown','};']
    else:out+=['static const struct InputPlayerLayout player_layout={0};']
    out+=['const struct InputPlayerLayout *input_player_state_config(void){return &player_layout;}']
    return '\n'.join(out)+'\n'


def emitter(base_type, module, starts, config, **kwargs):
    """Keep the lifter unchanged; compose the production callbacks around its ordinary function emission."""
    class ActionEmitter(base_type):
        def function(self, entry, end):
            lines, refs = super().function(entry, end)
            return wrap_function(lines, entry, config), refs
    return ActionEmitter(shadow_module(module, config), starts, **kwargs)
