"""Conservative dependency audit, not an evaluator or a skeleton decoder.

Opcode/operand descriptions are from the official Khronos SPIR-V JSON grammar
saved in dependencies/spirv.core.grammar.json. All game shader data stays local.
"""
import json
import struct
from pathlib import Path

root=Path(__file__).parent
grammar=json.loads((root/'dependencies/spirv.core.grammar.json').read_text())
specs={x['opcode']:x for x in grammar['instructions']}
shader='68e66bf9643995b0'
blob=(root/'captures'/'link-objects'/('sh_'+shader+'.spv')).read_bytes()
words=struct.unpack('<%dI'%(len(blob)//4),blob)
if words[0]!=0x07230203:raise ValueError('Expected little-endian SPIR-V')
instructions=[];names={};locations={};variables={};definitions={};member_builtins={}
cursor=5
while cursor<len(words):
    count=words[cursor]>>16;op=words[cursor]&65535
    if not count or cursor+count>len(words):raise ValueError('Malformed instruction')
    values=list(words[cursor+1:cursor+count]);instructions.append((op,values))
    for k,operand in enumerate(specs[op].get('operands',[])):
        if operand['kind']=='IdResult':definitions[values[k]]=(op,values)
    if op==5:names[values[0]]=struct.pack('<%dI'%(len(values)-1),*values[1:]).split(b'\0')[0].decode()
    if op==71 and values[1]==30:locations[values[0]]=values[2]
    if op==72 and values[2]==11:member_builtins[(values[0],values[1])]=values[3]
    if op==59:variables[values[1]]=values[2]
    cursor+=count
taint={id:{name} for id,name in names.items() if name.startswith('vp_c')}
taint.update({id:{'input_location_'+str(loc)} for id,loc in locations.items() if variables.get(id)==1})
converged=False
for iteration in range(100):
    changed=False
    for op,v in instructions:
        spec=specs[op];deps=[];result=None
        if spec['opname'].startswith('OpConstant'):continue
        for k,operand in enumerate(spec.get('operands',[])):
            if operand['kind']=='IdResult':result=v[k]
            if operand['kind'] in ('IdRef','IdScope','IdMemorySemantics'):
                deps+=v[k:] if operand.get('quantifier')=='*' else v[k:k+1]
        if op==62:result=v[0];deps=[v[1]]
        if result is not None:
            old=taint.get(result,set());new=old|set().union(*(taint.get(j,set()) for j in deps))
            if new!=old:taint[result]=new;changed=True
    if not changed:converged=True;break
if not converged:raise ValueError('Dependency audit did not converge')

def constant(id):
    op,v=definitions[id]
    if op!=43:raise ValueError('Expected a scalar OpConstant')
    return v[2]

def is_position_pointer(id):
    op,v=definitions[id]
    if op!=65:return False
    base=v[2]
    if base not in variables or variables[base]!=3:return False
    _,variable=definitions[base]
    _,pointer=definitions[variable[0]]
    structure=pointer[2]
    return member_builtins.get((structure,constant(v[3])))==0

accesses=[];position_stores=[]
for op,v in instructions:
    if op==65 and names.get(v[2])=='vp_c5':
        accesses.append(dict(pointer_id=v[1],index_sources=sorted(set().union(*(taint.get(j,set()) for j in v[3:])))))
    if op==62 and is_position_pointer(v[0]):
        position_stores.append(dict(pointer_id=v[0],value_sources=sorted(taint.get(v[1],set()))))
result=dict(shader=shader,method='Conservative fixed-point dependency propagation including local stores; not execution.',
    converged_iterations=iteration+1,c5_accesses=accesses,position_output_stores=position_stores,
    limitations=['This proves a dependency path, not the numerical skinning formula or blend weights.',
        'The shader has not been definitively associated with Link.',
        'Only this captured shader is audited; other animation paths can differ.'],
    grammar_source='https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/main/include/spirv/unified1/spirv.core.grammar.json')
out=root/'results'/'link-objects'/'skin-shader-evidence.json'
out.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
