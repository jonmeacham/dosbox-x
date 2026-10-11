#!/usr/bin/env python3
"""Compile and execute the actual guest_control.cpp with deterministic device stubs."""
import os, pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='gct-', dir=os.environ['TMPDIR']) as temporary:
    out = pathlib.Path(temporary)
    headers = {
        'dosbox.h': '#pragma once\n#include <cstdint>\n#include <cstddef>\nusing Bitu=uintptr_t; using LinearPt=uint32_t;\n',
        'cpu.h': '#pragma once\nint CPU_IsDynamicCore(void);\nextern uint32_t test_reg_eip,test_reg_esp,test_seg_base[2]; extern uint16_t test_seg_value[2]; enum {cs=0,ss=1}; inline uint32_t SegPhys(int i){return test_seg_base[i];} inline uint16_t SegValue(int i){return test_seg_value[i];}\n#define reg_eip test_reg_eip\n#define reg_esp test_reg_esp\n',
        'keyboard.h': '#include "' + str(root / 'include/keyboard.h') + '"\n',
        'mouse.h': '#pragma once\nvoid Mouse_CursorInjected(float,float,float,float,bool);\nvoid Mouse_ButtonPressed(uint8_t); void Mouse_ButtonReleased(uint8_t);\n',
        'pic.h': '#pragma once\nusing PIC_EventHandler=void(*)(Bitu);\ndouble PIC_FullIndex(); void PIC_AddEvent(PIC_EventHandler,double,Bitu); void PIC_RemoveEvents(PIC_EventHandler);\n',
        'mem.h': '#pragma once\n',
        'paging.h': '#pragma once\nbool mem_readb_checked(LinearPt,uint8_t*); bool mem_readd_checked(LinearPt,uint32_t*); bool mem_writed_checked(LinearPt,uint32_t);\n',
    }
    for name, content in headers.items(): (out/name).write_text(content)
    binary = out/'test'
    subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-Werror', '-I'+str(out),'-I'+str(root/'include'), str(root/'scripts/guest-control-tests/module.cpp'), '-o',str(binary)],check=True)
    subprocess.run([str(binary),str(out)],check=True)
