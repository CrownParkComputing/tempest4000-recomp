/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the real ARM64 shader-table walkers, including refresh, nesting and bad pointers. */
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "core/signals.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <signal.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#undef NDEBUG
#include <cassert>

// This CPU-only test links the renderer without initializing graphics or a guest process.
namespace Core::GuestCpu { class HleCallAdapter; }
void tempest_fex_register_adapter(uintptr_t, std::shared_ptr<Core::GuestCpu::HleCallAdapter>) { std::abort(); }
extern "C" PFN_vkGetInstanceProcAddr tempest_android_vulkan_proc() { return nullptr; }

int main() {
    setvbuf(stdout,nullptr,_IOLBF,0);
    struct sigaction signal{};
    signal.sa_flags=SA_SIGINFO;
    signal.sa_sigaction=[](int number,siginfo_t* info,void* context) {
        if (!Core::Signals::Instance()->DispatchAccessViolation(context,info->si_addr)) _exit(128+number);
    };
    sigemptyset(&signal.sa_mask);
    assert(sigaction(SIGSEGV,&signal,nullptr)==0);
    std::array<u32,256> table{}, leaf{};
    for (u32 i=0; i<table.size(); ++i) { table[i]=0xa000+i; leaf[i]=0xb000+i; }
    std::array<u32,16> ud{};
    uint64_t pointer=reinterpret_cast<uintptr_t>(table.data());
    std::memcpy(ud.data(),&pointer,8);
    pointer=reinterpret_cast<uintptr_t>(leaf.data());
    std::memcpy(table.data()+64,&pointer,8);
    ud[2]=7;
    ud[3]=1; ud[4]=2;
    Shader::Info info{};
    info.user_data=ud;
    Shader::Pools pools;
    auto* block=pools.block_pool.Create(pools.inst_pool);
    Shader::IR::Program program(info);
    program.blocks.push_back(block); program.post_order_blocks.push_back(block);
    Shader::IR::IREmitter ir(*block);
    using namespace Shader::IR;
    auto base=ir.CompositeConstruct(ir.GetUserData(ScalarReg::S0),ir.GetUserData(ScalarReg::S1));
    auto index=ir.GetUserData(ScalarReg::S2);
    struct Expected { U32 read; std::function<u32(u32)> offset; const std::array<u32,256>* source; };
    std::vector<Expected> expected;
    auto read=[&](const U32& offset,std::function<u32(u32)> value) {
        expected.push_back({ir.ReadConst(base,offset),value,&table});
    };
    read(U32{ir.IAdd(index,ir.Imm32(3u))},[](u32 n) { return n+3; });
    read(U32{ir.ISub(ir.Imm32(20u),index)},[](u32 n) { return 20-n; });
    read(U32{ir.IMul(index,ir.Imm32(3u))},[](u32 n) { return n*3; });
    read(U32{ir.ShiftLeftLogical(index,ir.Imm32(1u))},[](u32 n) { return n<<1; });
    read(U32{ir.ShiftRightLogical(index,ir.Imm32(1u))},[](u32 n) { return n>>1; });
    read(U32{ir.BitwiseAnd(index,ir.Imm32(6u))},[](u32 n) { return n&6; });
    read(U32{ir.BitwiseOr(index,ir.Imm32(8u))},[](u32 n) { return n|8; });
    read(U32{ir.BitwiseXor(index,ir.Imm32(3u))},[](u32 n) { return n^3; });
    read(U32{ir.BitwiseAnd(ir.BitwiseNot(index),ir.Imm32(63u))},[](u32 n) { return (~n)&63; });
    read(ir.UMin(index,ir.Imm32(6u)),[](u32 n) { return std::min(n,6u); });
    read(ir.UMax(index,ir.Imm32(9u)),[](u32 n) { return std::max(n,9u); });
    read(ir.BitFieldExtract(index,ir.Imm32(1u),ir.Imm32(2u)),[](u32 n) { return (n>>1)&3; });
    read(ir.BitFieldExtract(index,ir.GetUserData(ScalarReg::S3),ir.GetUserData(ScalarReg::S4)),
        [&](u32 n) { return ud[4]>=32 ? n>>ud[3] : (n>>ud[3])&((1u<<ud[4])-1); });
    read(U32{ir.BitwiseAnd(ir.IAdd(index,ir.Imm32(0xfffffff8u)),ir.Imm32(63u))},
        [](u32 n) { return (n-8u)&63u; });
    BufferInstInfo buffer{};
    buffer.sharp_source.Assign(1);
    auto buffer_read=ir.ReadConstBuffer(base,ir.Imm32(5u),buffer);
    auto child=ir.CompositeConstruct(ir.ReadConst(base,ir.Imm32(64u)),ir.ReadConst(base,ir.Imm32(65u)));
    expected.push_back({ir.ReadConst(child,index),[](u32 n) { return n; },&leaf});
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    assert(info.srt_info.walker_func);
    auto verify=[&](bool valid) {
        info.RefreshFlatBuf();
        for (const auto& item : expected) {
            u32 offset=item.read.Inst()->Flags<u16>();
            assert(offset>=16 && offset<info.flattened_ud_buf.size());
            assert(info.flattened_ud_buf[offset]==(valid ? (*item.source)[item.offset(ud[2])] : 0));
        }
        u32 offset=buffer_read.Inst()->Flags<BufferInstInfo>().flatbuf_off_dw;
        assert(info.flattened_ud_buf[offset]==(valid ? table[5] : 0));
    };
    verify(true);
    ud[2]=3; verify(true); // Cached code must read the new per-draw user data.
    ud[3]=0; ud[4]=32; verify(true);
    ud[4]=0; verify(true);
    auto clone=Shader::RegisterWalkerCode(reinterpret_cast<const u8*>(info.srt_info.walker_func),info.srt_info.walker_func_size);
    assert(clone); info.srt_info.walker_func=clone; verify(true);
    ud[0]=0x1000; ud[1]=0; verify(false);
    const u8 x86_cache[]={0xc3,0,0,0};
    assert(!Shader::RegisterWalkerCode(x86_cache,sizeof(x86_cache)));
    puts("ARM64 SRT: dynamic offsets, nested/buffer loads, per-draw refresh, code reload and bad pointers passed");
    return 0;
}
