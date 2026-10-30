"""Region pins and closed effect boundaries fail before code generation."""
import copy
import hashlib
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from lift_ac1 import native_instruction_hash,validate_region_bindings

class RegionBindings(unittest.TestCase):
    def setUp(self):
        self.raw=bytearray(0x103)
        self.raw[:7]=b'\xe8\xfb\x00\x00\x00\x90\xc3'
        self.raw[0x100:]=b'\x31\xc0\xc3'
        self.read=lambda a,n:bytes(self.raw[a-0x401000:a-0x401000+n])
        self.specs=[{'address':0x401000,'end':0x401007,'blocks':[
            {'start':0x401000,'instructions':[[0x401000,5]]},
            {'start':0x401005,'instructions':[[0x401005,1],[0x401006,1]]}]},
            {'address':0x401100,'end':0x401103,'blocks':[
            {'start':0x401100,'instructions':[[0x401100,2],[0x401102,1]]}]}]
        self.entry={'kind':'region','address':'0x00401000','boundary':'0x00401005',
            'implementation':'body','plan':'prepare','pending':'pending','context_bytes':64,
            'instruction_sha256':native_instruction_hash(self.specs[0],self.read),'members':[]}
        for spec,hi in zip(self.specs,[0x401005,0x401103]):
            lo=spec['address'];self.entry['members'].append({'address':hex(lo),
                'instruction_sha256':native_instruction_hash(spec,self.read),
                'subsumed_ranges':[{'start':hex(lo),'end':hex(hi),'sha256':hashlib.sha256(self.read(lo,hi-lo)).hexdigest()}]})
        self.md=Cs(CS_ARCH_X86,CS_MODE_32);self.md.detail=True
    def validate(self):
        return validate_region_bindings([self.entry],self.specs,{s['address'] for s in self.specs},self.read,self.md)
    def test_closed_prefix(self): self.assertIn(0x401000,self.validate())
    def test_member_pin(self):
        self.raw[0x100]=0x33
        with self.assertRaisesRegex(ValueError,'member.*hash changed'):self.validate()
    def test_range_pin(self):
        self.entry['members'][1]['subsumed_ranges'][0]['sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'range hash changed'):self.validate()
    def test_missing_closure(self):
        self.entry['members'].pop()
        with self.assertRaisesRegex(ValueError,'unpinned target'):self.validate()
    def test_uncovered_instruction(self):
        r=self.entry['members'][1]['subsumed_ranges'][0];r['end']='0x401102';r['sha256']=hashlib.sha256(self.read(0x401100,2)).hexdigest()
        with self.assertRaisesRegex(ValueError,'uncovered instruction'):self.validate()
    def test_boundary_and_capacity(self):
        self.entry['boundary']='0x401004'
        with self.assertRaisesRegex(ValueError,'boundary'):self.validate()
        self.entry['boundary']='0x401005';self.entry['context_bytes']=8193
        with self.assertRaisesRegex(ValueError,'bounded storage'):self.validate()
    def test_journal_guard_is_pinned_and_current(self):
        self.entry['verification']='write_journal'
        self.entry['members'][0]['subsumed_ranges'][0]={'start':'0x401000','end':'0x401007','sha256':hashlib.sha256(self.read(0x401000,7)).hexdigest()}
        self.entry['members'].pop()
        self.entry['guarded_transfers']=[{'site':'0x401000','target':'0x401100'}]
        self.assertIn(0x401000,self.validate())
        self.entry['guarded_transfers'][0]['site']='0x401001'
        with self.assertRaisesRegex(ValueError,'unpinned target'):self.validate()

if __name__=='__main__':unittest.main()
