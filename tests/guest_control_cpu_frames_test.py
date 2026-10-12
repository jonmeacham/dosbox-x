"""Compile the real guest-control parser, capture block and receipt serializer.

Only host/emulator dependencies are substituted; no emulator or guest is started.
"""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'src/guest_control.cpp'


class CpuFrameReceiptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        source = SOURCE.read_text()
        keys = sorted(set(re.findall(r'\bKBD_\w+', source)) - {'KBD_KEYS'})
        declarations = source[source.index('constexpr size_t MAX_LINE'):source.index('bool write_all(')]
        quote = source[source.index('std::string quote('):source.index('void disconnect()')]
        record = source[source.index('void record('):source.index('bool number(')]
        parser = source[source.index('bool number('):source.index('const char *dump(')]
        capture = source[source.index('    case Event::CpuState: {'):source.index('    }\n    record(e, error ?')]
        cpp = r'''
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#define C_DEBUG 1
'''
        cpp += 'enum KBD_KEYS {' + ','.join(keys) + '};\n' + declarations + quote
        cpp += r'''
std::string retained;
bool write_all(int, const char *bytes, size_t count) { retained.assign(bytes,count); return true; }
void disconnect() { s.client=-1; s.output.clear(); s.output_bytes=0; }
double PIC_FullIndex() { return 42.25; }
'''
        cpp += record + parser
        cpp += r'''
using LinearPt = uint32_t;
constexpr int cs=0,ss=1;
uint32_t reg_eip=0x5ED52,reg_esp=0x24000,reg_ebp=0x24300;
uint32_t SegValue(int which) { return which==cs ? 128 : 136; }
uint32_t SegPhys(int which) { return which==cs ? 0x1DA000 : 0x300000; }
int CPU_IsDynamicCore() { return 0; }
uint32_t reads=0, fail_word=0xFFFFFFFFu;
bool mem_readd_checked(LinearPt address,uint32_t *value) {
    uint32_t ordinal=(address-0x324000)/4; ++reads;
    if (ordinal==fail_word) return true;
    *value=0xFFFFFFFFu-ordinal; return false;
}
void capture(Event &e) {
    const char *error=nullptr;
    switch(e.kind) {
'''
        cpp += capture
        cpp += r'''
    default: break;
    }
    record(e,error ? "error" : "done",error ? error : "");
}
int main(int argc,char **argv) {
    Event e; e.id=7; e.session=3;
    s.log=1; s.client=1; s.session=3;
    if (!parse(argv[1],e)) { std::cout<<"{\"accepted\":false}"; return 0; }
    if (argc>2) fail_word=uint32_t(std::strtoul(argv[2],nullptr,10));
    capture(e);
    std::cout<<retained;
    std::cerr<<reads;
}
'''
        root = Path(cls.temp.name)
        (root/'receipt.cpp').write_text(cpp)
        cls.binary = root/'receipt'
        subprocess.run(['clang++','-std=c++14','-Wall','-Wextra',str(root/'receipt.cpp'),'-o',str(cls.binary)],check=True,capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def receipt(self, command, fail=None):
        args=[str(self.binary),command]+([] if fail is None else [str(fail)])
        result=subprocess.run(args,check=True,capture_output=True,text=True)
        return json.loads(result.stdout),int(result.stderr or 0),len(result.stdout.encode())

    def test_legacy_state_retains_exact_shape_and128_reads(self):
        receipt,reads,_=self.receipt('CPU STATE 0')
        self.assertEqual(receipt['status'],'done')
        self.assertEqual(set(receipt['cpu']),{'cs','dynamicCore','eip','linear','ss','esp','stack'})
        self.assertEqual(len(receipt['cpu']['stack']),128)
        self.assertEqual(reads,128)

    def test_optional_frames_atomic_capture_addresses_prefix_and_output_bound(self):
        receipt,reads,size=self.receipt('CPU STATE 0 FRAMES')
        self.assertEqual(receipt['status'],'done')
        cpu=receipt['cpu']; frame=cpu['frameSnapshot']
        self.assertEqual(frame['schema'],'native-active-frames/v1')
        self.assertEqual(frame['ebp'],0x24300)
        self.assertEqual(frame['linearESP'],0x324000)
        self.assertEqual(frame['linearEBP'],0x324300)
        self.assertEqual(cpu['stack'],frame['words'][:128])
        self.assertEqual(len(frame['words']),1024)
        self.assertEqual(reads,1024)
        self.assertEqual(frame['words'][-1],0xFFFFFFFF-1023)
        self.assertLess(size,65536)

    def test_extended_failure_has_no_partial_cpu_proof_and_legacy_unaffected(self):
        receipt,reads,_=self.receipt('CPU STATE 0 FRAMES',128)
        self.assertEqual(receipt['status'],'error')
        self.assertEqual(receipt['reason'],'read')
        self.assertNotIn('cpu',receipt)
        self.assertEqual(reads,129)
        legacy,reads,_=self.receipt('CPU STATE 0',128)
        self.assertEqual(legacy['status'],'done'); self.assertEqual(reads,128)

    def test_invalid_extra_arguments_and_delays_are_rejected(self):
        for command in ('CPU STATE 0 extra','CPU STATE 0 FRAMES extra','CPU STATE -1 FRAMES','CPU STATE 600001 FRAMES','CPU STATE FRAMES'):
            with self.subTest(command=command):
                receipt,reads,_=self.receipt(command)
                self.assertEqual(receipt,{'accepted':False}); self.assertEqual(reads,0)
        receipt,reads,_=self.receipt('cpu state 600000 frames')
        self.assertEqual(receipt['status'],'done'); self.assertEqual(reads,1024)


if __name__ == '__main__': unittest.main()
