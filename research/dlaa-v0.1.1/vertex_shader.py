"""Bounded offline evaluator of the position slice of a straight-line SPIR-V VS.

No control flow, calls, images, storage buffers or general SPIR-V execution.
Unsupported instructions raise if reached by the position slice. GPU arithmetic
is approximated with float32 and a float64 intermediate for FMA.
"""
import json
import struct
from pathlib import Path
import numpy as np

class Node:
    def __init__(self,op,*args):self.op=op;self.args=args

def literal(node):
    if node.op!='constant':raise ValueError('Dynamic local/output address is unsupported')
    return int(node.args[0])

def bitcast32(value,to_float):
    a=np.asarray(value)
    if to_float:
        return a.astype(np.float32) if a.dtype.kind=='f' else a.astype(np.uint32).view(np.float32)
    return a.astype(np.float32).view(np.uint32) if a.dtype.kind=='f' else a.astype(np.uint32)

class PositionProgram:
    def __init__(self,blob):
        if len(blob)<20 or len(blob)%4:raise ValueError('Malformed SPIR-V size')
        words=struct.unpack('<%dI'%(len(blob)//4),blob)
        if words[0]!=0x07230203:raise ValueError('Invalid SPIR-V magic')
        grammar=json.loads((Path(__file__).parent/'dependencies/spirv.core.grammar.json').read_text())
        specs={i['opcode']:i for i in grammar['instructions']}
        instructions=[];cursor=5;self.types={};names={};locations={};builtins={};variables={};defs={}
        while cursor<len(words):
            n=words[cursor]>>16;op=words[cursor]&65535
            if not n or cursor+n>len(words) or op not in specs:raise ValueError('Malformed instruction')
            v=list(words[cursor+1:cursor+n]);instructions.append((op,v))
            for k,operand in enumerate(specs[op].get('operands',[])):
                if operand['kind']=='IdResult':defs[v[k]]=(op,v)
            if op==5:names[v[0]]=struct.pack('<%dI'%(len(v)-1),*v[1:]).split(b'\0')[0].decode()
            if op==71 and v[1]==30:locations[v[0]]=v[2]
            if op==72 and v[2]==11:builtins[(v[0],v[1])]=v[3]
            if op in (21,22,23,32):self.types[v[0]]=(op,v[1:])
            if op==59:variables[v[1]]=v[2]
            cursor+=n
        if sum(op==54 for op,v in instructions)!=1 or sum(op==248 for op,v in instructions)!=1:
            raise ValueError('Only one straight-line function is supported')
        if any(op in (57,245,246,247,249,250,251,252) for op,v in instructions):
            raise ValueError('Control flow and function calls are unsupported')
        value={};pointers={};memory={};self.position={}
        def load(pointer):
            base,path=pointers[pointer]
            if base in names and names[base].startswith('vp_c'):
                return Node('uniform',int(names[base][4:]),path)
            if variables.get(base)==1 and base in locations:
                return Node('input',locations[base],tuple(literal(x) for x in path))
            key=(base,tuple(literal(x) for x in path))
            return memory.get(key,Node('unsupported','uninitialized local/support load'))
        for op,v in instructions:
            if op==43:
                kind=self.types[v[0]][0]
                value[v[1]]=Node('constant',np.frombuffer(struct.pack('<I',v[2]),dtype='<f4')[0] if kind==22 else np.uint32(v[2]))
            elif op==59:pointers[v[1]]=(v[1],())
            elif op==65:
                base,path=pointers[v[2]];pointers[v[1]]=(base,path+tuple(value[x] for x in v[3:]))
            elif op==61:value[v[1]]=load(v[2])
            elif op==62:
                base,path=pointers[v[0]];indices=tuple(literal(x) for x in path)
                memory[(base,indices)]=value[v[1]]
                if variables.get(base)==3 and len(indices)==2:
                    _,var=defs[base];_,pointer_type=defs[var[0]];structure=pointer_type[2]
                    if builtins.get((structure,indices[0]))==0:self.position[indices[1]]=value[v[1]]
            elif op in (124,128,129,132,133,194,199):
                value[v[1]]=Node(specs[op]['opname'],v[0],*(value[x] for x in v[2:]))
            elif op==12:value[v[1]]=Node('ext',v[3],*(value[x] for x in v[4:]))
            else:
                # Keep unsupported SSA values as explicit traps, never fabricated zero.
                for k,operand in enumerate(specs[op].get('operands',[])):
                    if operand['kind']=='IdResult' and v[k] not in value and op not in (19,21,22,23,28,30,32,33,54,248):
                        value[v[k]]=Node('unsupported',specs[op]['opname'])
        if not self.position:raise ValueError('No scalar stores to BuiltIn Position found')
        self.input_locations=set();self.input_components={};seen=set()
        def visit(value):
            if isinstance(value,Node):
                if value in seen:return
                seen.add(value)
                if value.op=='input':
                    self.input_locations.add(value.args[0])
                    self.input_components.setdefault(value.args[0],set()).add(value.args[1][0])
                for child in value.args:visit(child)
            elif isinstance(value,tuple):
                for child in value:visit(child)
        for node in self.position.values():visit(node)

    def evaluate(self,inputs,uniforms):
        count=len(next(iter(inputs.values())));cache={}
        def evaluate(node):
            if node in cache:return cache[node]
            op=node.op;args=node.args
            if op=='constant':result=args[0]
            elif op=='input':
                slot,path=args
                if len(path)!=1:raise ValueError('Only scalar vector input loads are supported')
                result=np.asarray(inputs[slot])[:,path[0]]
            elif op=='uniform':
                slot,path=args
                if len(path)!=3 or literal(path[0])!=0:raise ValueError('Unsupported uniform layout')
                rows=np.asarray(uniforms[slot],dtype=np.float32).reshape(-1,4)
                row=np.asarray(evaluate(path[1]),dtype=np.int64);component=np.asarray(evaluate(path[2]),dtype=np.int64)
                if np.any(row<0) or np.any(row>=len(rows)) or np.any(component<0) or np.any(component>3):
                    raise ValueError(f'Shader c{slot} read exceeds {len(rows)} rows: rows {row.min()}..{row.max()}, components {component.min()}..{component.max()}')
                result=rows[row,component]
            elif op=='unsupported':raise ValueError('Unsupported position dependency: '+str(args[0]))
            elif op=='ext':
                values=[evaluate(x) for x in args[1:]]
                if args[0]==50:result=(np.asarray(values[0],np.float64)*np.asarray(values[1],np.float64)+np.asarray(values[2],np.float64)).astype(np.float32)
                elif args[0]==32:result=np.float32(1)/np.sqrt(np.asarray(values[0],np.float32))
                else:raise ValueError('Unsupported GLSL extended instruction')
            else:
                kind=args[0];a=evaluate(args[1]);b=evaluate(args[2]) if len(args)>2 else None
                if op=='OpBitcast':
                    target=self.types[kind][0]
                    result=bitcast32(a,target==22)
                elif op=='OpIAdd':result=np.asarray(a,np.uint32)+np.asarray(b,np.uint32)
                elif op=='OpIMul':result=np.asarray(a,np.uint32)*np.asarray(b,np.uint32)
                elif op=='OpShiftRightLogical':result=np.asarray(a,np.uint32)>>np.asarray(b,np.uint32)
                elif op=='OpBitwiseAnd':result=np.asarray(a,np.uint32)&np.asarray(b,np.uint32)
                elif op=='OpFAdd':result=np.asarray(a,np.float32)+np.asarray(b,np.float32)
                elif op=='OpFMul':result=np.asarray(a,np.float32)*np.asarray(b,np.float32)
                else:raise ValueError('Unsupported position operation: '+op)
            cache[node]=result;return result
        output=np.full((count,4),np.nan,np.float32)
        for component,node in self.position.items():output[:,component]=evaluate(node)
        return output
