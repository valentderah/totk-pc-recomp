"""Reject ambiguous selectors and check generated selector behavior without game data."""
import subprocess
import tempfile
import unittest
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from input_special_plan import source

class Selectors(unittest.TestCase):
    def test_validation(self):
        verbs=[{'name':'synthetic'}]
        row={'verb':'synthetic','ability':2,'context':'FUSE'}
        for rows in [[],[row,row],[row,dict(row,ability=-1)],[dict(row,verb='missing')],
                     [dict(row,context='unknown')],[dict(row,ability=True)]]:
            with self.assertRaises(ValueError):source(rows,verbs)
    def test_generated_selector(self):
        code=source([{'verb':'synthetic','ability':2,'context':'FUSE'},
                     {'verb':'other','ability':-1,'context':'SCOPE'}],
                    [{'name':'empty'},{'name':'synthetic'},{'name':'other'}])
        code='#include "input_actions.h"\n#include <assert.h>\n'+code+'''
int main(void){int32_t v[5]={0,1,0,0,0};
assert(input_actions_special_context(v,2)==IC_FUSE);
assert(input_actions_special_context(v,1)==-1);
v[1]=0;v[4]=2;assert(input_actions_special_context(v,1)==IC_SCOPE);return 0;}
'''
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'test.c').write_text(code)
            subprocess.run(['clang','-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
