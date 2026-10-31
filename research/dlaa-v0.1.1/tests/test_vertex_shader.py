import struct
import unittest
import numpy as np
from vertex_shader import PositionProgram, bitcast32

def instruction(op,*values):return [(len(values)+1)<<16|op,*values]
def program(branch=False):
    code=[]
    def emit(op,*values):code.extend(instruction(op,*values))
    emit(22,1,32);emit(23,2,1,4);emit(21,3,32,0)
    emit(43,3,4,0);emit(43,1,5,0x40400000) # zero index, float three
    emit(30,6,2);emit(32,7,3,6);emit(59,7,8,3)
    emit(72,6,0,11,0) # struct member is BuiltIn Position
    emit(32,9,3,1);emit(32,10,1,2);emit(59,10,11,1);emit(71,11,30,0)
    emit(32,12,1,1);emit(19,13);emit(33,14,13)
    emit(54,13,15,0,14);emit(248,16)
    emit(65,12,17,11,4);emit(61,1,18,17)
    emit(129,1,19,18,5) # input x + 3
    emit(65,9,20,8,4,4);emit(62,20,19)
    if branch:emit(249,16)
    emit(253);emit(56)
    return struct.pack('<%dI'%(len(code)+5),0x07230203,0x10000,0,30,0,*code)

class PositionProgramTests(unittest.TestCase):
    def test_integer_to_integer_bitcast_preserves_bone_index(self):
        np.testing.assert_equal(bitcast32(np.array([77],np.uint8),False),[77])

    def test_bitcast_reinterprets_float_bits_in_both_directions(self):
        np.testing.assert_equal(bitcast32(np.array([1.0],np.float32),False),[0x3f800000])
        np.testing.assert_equal(bitcast32(np.array([0x3f800000],np.uint32),True),[1.0])
    def test_executes_actual_instructions_on_input_values(self):
        shader=PositionProgram(program())
        result=shader.evaluate({0:np.array([[2,0,0,1],[-1,0,0,1]],np.float32)}, {})
        np.testing.assert_equal(result[:,0],[5,2])

    def test_control_flow_is_rejected_instead_of_ignored(self):
        with self.assertRaises(ValueError):PositionProgram(program(branch=True))

    def test_malformed_instruction_is_rejected(self):
        raw=bytearray(program());raw[20:24]=bytes(4)
        with self.assertRaises(ValueError):PositionProgram(bytes(raw))
