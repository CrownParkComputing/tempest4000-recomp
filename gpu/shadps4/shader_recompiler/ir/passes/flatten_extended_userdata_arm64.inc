// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// tempest: native ARM64 SRT walkers. Keep the current pass's dynamic offsets and buffer loads.
#include <CodeEmitter/Emitter.h>
#include <atomic>
#include <memory>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace {
using namespace ARMEmitter;
using namespace ARMEmitter::Reg;
constexpr auto S32 = ARMEmitter::Size::i32Bit;
constexpr auto S64 = ARMEmitter::Size::i64Bit;

class Arm64SrtEmitter {
public:
    template<class Emit> void EmitCode(Emit emit) {
        // Each invocation emits a bounded group of at most four instructions. The full
        // program grows in a vector, so shader complexity cannot overrun a fixed JIT arena.
        std::array<u8, 64> scratch{};
        ARMEmitter::Emitter emitter(scratch.data(), scratch.size());
        emit(emitter);
        ASSERT(emitter.GetCursorOffset() <= scratch.size());
        code.insert(code.end(), scratch.begin(), scratch.begin() + emitter.GetCursorOffset());
    }
    void Begin() { EmitCode([](auto& e) { e.mov(S64,r2,r0); }); }
    void End() { EmitCode([](auto& e) { e.ret(); }); }
    void Immediate(u32 reg, u32 value) {
        EmitCode([=](auto& e) {
            e.movz(S32,Register(reg),value & 0xffff);
            if (value >> 16) e.movk(S32,Register(reg),value >> 16,16);
        });
    }
    void Push(u32 reg) {
        EmitCode([=](auto& e) { e.sub(S64,r31,r31,16); e.str(XRegister(reg),r31); });
    }
    void Pop(u32 reg) {
        EmitCode([=](auto& e) { e.ldr(XRegister(reg),r31); e.add(S64,r31,r31,16); });
    }
    void Flat(u32 offset) {
        Immediate(4,offset);
        EmitCode([](auto& e) { e.add(S64,r6,r1,r4,ShiftType::LSL,2); e.ldr(WRegister(4),r6); });
    }
    void Pointer() {
        EmitCode([](auto& e) {
            e.add(S64,r6,r2,r4,ShiftType::LSL,2);
            e.ldr(XRegister(2),r6);
            e.ubfx(S64,r2,r2,0,48);
        });
    }
    void CopyDword(u32 destination) {
        EmitCode([](auto& e) { e.add(S64,r6,r2,r4,ShiftType::LSL,2); e.ldr(WRegister(3),r6); });
        Immediate(5,destination);
        EmitCode([](auto& e) { e.add(S64,r6,r1,r5,ShiftType::LSL,2); e.str(WRegister(3),r6); });
    }
    const u8* Data() const { return code.data(); }
    size_t Size() const { return code.size(); }
private:
    std::vector<u8> code;
};

// Range publication and fault recovery follow the pinned Bachata ARM64 walker. A bad guest
// table load supplies zero; native code outside registered walkers remains a fatal fault.
struct Arm64SrtMapping {
    void* address{};
    size_t size{};
    ~Arm64SrtMapping() { if (address) munmap(address,size); }
};
std::array<std::pair<uintptr_t,uintptr_t>,32768> arm64_srt_ranges{};
std::atomic<size_t> arm64_srt_count{};
std::mutex arm64_srt_mutex;
std::vector<std::unique_ptr<Arm64SrtMapping>> arm64_srt_mappings;
std::once_flag arm64_srt_signal;
bool Arm64SrtFault(void* raw, void*) {
    const auto pc = reinterpret_cast<uintptr_t>(Common::GetRip(raw));
    bool found = false;
    for (size_t i=0, count=arm64_srt_count.load(std::memory_order_acquire); i<count; ++i)
        if (pc>=arm64_srt_ranges[i].first && pc<arm64_srt_ranges[i].second) { found=true; break; }
    if (!found) return false;
    u32 instruction;
    std::memcpy(&instruction,reinterpret_cast<const void*>(pc),4);
    auto* context=static_cast<ucontext_t*>(raw);
    if (instruction==0xf94000c2) context->uc_mcontext.regs[2]=0; // ldr x2,[x6]
    else if (instruction==0xb94000c3) context->uc_mcontext.regs[3]=0; // ldr w3,[x6]
    else return false;
    context->uc_mcontext.pc+=4;
    return true;
}
}

namespace Shader {
PFN_SrtWalker RegisterWalkerCode(const u8* bytes, size_t size) {
    u32 first=0,last=0;
    if (bytes && size>=8 && size%4==0) {
        std::memcpy(&first,bytes,4); std::memcpy(&last,bytes+size-4,4);
    }
    if (first!=0xaa0003e2 || last!=0xd65f03c0) return nullptr;
    std::lock_guard lock(arm64_srt_mutex);
    const auto index=arm64_srt_count.load(std::memory_order_relaxed);
    ASSERT(index<arm64_srt_ranges.size());
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto memory=std::make_unique<Arm64SrtMapping>();
    memory->size=(size+page-1)&~(page-1);
    memory->address=mmap(nullptr,memory->size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (memory->address==MAP_FAILED) { memory->address=nullptr; UNREACHABLE_MSG("ARM64 SRT allocation failed"); }
    std::memcpy(memory->address,bytes,size);
    __builtin___clear_cache(static_cast<char*>(memory->address),static_cast<char*>(memory->address)+size);
    ASSERT(mprotect(memory->address,memory->size,PROT_READ|PROT_EXEC)==0);
    std::call_once(arm64_srt_signal,[] { Core::Signals::Instance()->RegisterAccessViolationHandler(Arm64SrtFault,1); });
    const auto begin=reinterpret_cast<uintptr_t>(memory->address);
    arm64_srt_mappings.push_back(std::move(memory));
    arm64_srt_ranges[index]={begin,begin+size};
    arm64_srt_count.store(index+1,std::memory_order_release);
    return reinterpret_cast<PFN_SrtWalker>(begin);
}
}

namespace {
using namespace Shader;
struct PassInfo {
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            // Keep order with the following criteria:
            // 1. If immediate, sort by immediate value
            // 2. If not immediate, sort by instruction order
            // 3. Immediates come first, then instructions
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block; // Sort by block address
                }

                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate(); // Immediates come first
            }
        }
    };

    // map offset to inst
    using PtrUserList =
        boost::container::flat_map<IR::Value, Shader::IR::Inst*, PointerListCompare>;

    Optimization::SrtGvnTable gvn_table;
    // keys are GetUserData, ReadConst or ReadConstBuffer instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u16 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
}

namespace Shader::Optimization {
namespace {
static inline u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        return inst_info.flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static inline void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        inst_info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(inst_info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}


static bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
        return true;
    default:
        return false;
    }
}



bool ComputeOffset(Arm64SrtEmitter& code, PassInfo& info, const IR::Value& value) {
    if (value.IsImmediate()) { code.Immediate(4,value.U32()); return true; }
    auto* inst=value.Inst();
    const auto op=inst->GetOpcode();
    if (op==IR::Opcode::GetUserData) { code.Flat(static_cast<u32>(inst->Arg(0).ScalarReg())); return true; }
    if (op==IR::Opcode::ReadConst || op==IR::Opcode::ReadConstBuffer) {
        u16 offset=GetFlatbufOffset(info.DeduplicateInstruction(inst));
        if (!offset) return false;
        code.Flat(offset); return true;
    }
    if (!IsAllowedOffsetInstruction(inst)) return false;
    if (!ComputeOffset(code,info,inst->Arg(0))) return false;
    if (op==IR::Opcode::BitwiseNot32) {
        code.EmitCode([](auto& e) { e.mvn(S32,r4,r4); }); return true;
    }
    code.Push(4);
    if (!ComputeOffset(code,info,inst->Arg(1))) return false;
    code.Pop(5);
    if (op==IR::Opcode::BitFieldUExtract) {
        code.EmitCode([](auto& e) { e.lsrv(S32,r5,r5,r4); });
        code.Push(5);
        if (!ComputeOffset(code,info,inst->Arg(2))) return false;
        code.EmitCode([](auto& e) {
            e.movz(S32,r6,1); e.lslv(S32,r6,r6,r4); e.sub(S32,r6,r6,1);
        });
        code.Immediate(7,UINT32_MAX);
        code.EmitCode([](auto& e) { e.cmp(S32,r4,32); e.csel(S32,r6,r7,r6,Condition::CC_HS); });
        code.Pop(5);
        code.EmitCode([](auto& e) { e.and_(S32,r4,r5,r6); });
        return true;
    }
    code.EmitCode([op](auto& e) {
        switch(op) {
        case IR::Opcode::IAdd32: e.add(S32,r4,r5,r4); break;
        case IR::Opcode::ISub32: e.sub(S32,r4,r5,r4); break;
        case IR::Opcode::IMul32: e.mul(S32,r4,r5,r4); break;
        case IR::Opcode::ShiftLeftLogical32: e.lslv(S32,r4,r5,r4); break;
        case IR::Opcode::ShiftRightLogical32: e.lsrv(S32,r4,r5,r4); break;
        case IR::Opcode::BitwiseAnd32: e.and_(S32,r4,r5,r4); break;
        case IR::Opcode::BitwiseOr32: e.orr(S32,r4,r5,r4); break;
        case IR::Opcode::BitwiseXor32: e.eor(S32,r4,r5,r4); break;
        case IR::Opcode::UMin32:
        case IR::Opcode::UMax32:
            e.cmp(S32,r5,r4); e.csel(S32,r4,r5,r4,op==IR::Opcode::UMin32 ? Condition::CC_LO : Condition::CC_HI); break;
        default: UNREACHABLE_MSG("Unsupported ARM64 SRT arithmetic");
        }
    });
    return true;
}
void VisitPointer(const IR::Value& offset, IR::Inst* subtree, PassInfo& info, Arm64SrtEmitter& code) {
    if ((subtree->GetOpcode()==IR::Opcode::ReadConst && !subtree->Flags<u16>()) ||
        (subtree->GetOpcode()==IR::Opcode::ReadConstBuffer && !subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw)) return;
    code.Push(2);
    ASSERT_MSG(ComputeOffset(code,info,offset),"Unable to compute ARM64 SRT pointer offset");
    code.Pointer();
    auto* uses=info.GetUsesAsPointer(subtree);
    ASSERT(uses);
    for (const auto& [source,use] : *uses) {
        ASSERT_MSG(ComputeOffset(code,info,source),"Unable to compute ARM64 SRT data offset");
        ASSERT(info.dst_off_dw<UINT16_MAX);
        code.CopyDword(info.dst_off_dw);
        SetFlatbufOffset(use,info.dst_off_dw++);
    }
    for (const auto& [source,use] : *uses)
        if (info.GetUsesAsPointer(use)) VisitPointer(source,use,info,code);
    code.Pop(2);
}
void GenerateSrtProgram(Info& info, PassInfo& pass) {
    if (pass.srt_roots.empty()) return;
    Arm64SrtEmitter code;
    code.Begin();
    pass.dst_off_dw=NUM_USER_DATA_REGS;
    ASSERT(pass.dst_off_dw==info.srt_info.flattened_bufsize_dw);
    for (const auto& [base,root] : pass.srt_roots) VisitPointer(IR::Value(static_cast<u32>(base)),root,pass,code);
    code.End();
    info.srt_info.walker_func=RegisterWalkerCode(code.Data(),code.Size());
    ASSERT(info.srt_info.walker_func);
    info.srt_info.walker_func_size=code.Size();
    info.srt_info.flattened_bufsize_dw=pass.dst_off_dw;
}

static bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    // This handles the following pattern by combining the addition with the offset
    // s_add_u32    s12, s2, <offset>
    // s_addc_u32   s13, s3, 0
    // s_load_dword s16, s[12:13], 0x0

    if (inst.GetOpcode() != IR::Opcode::ReadConst) {
        return;
    }
    IR::Inst* addr = inst.Arg(0).Inst();
    IR::Inst* hi = addr->Arg(1).TryInst();
    IR::Inst* lo = addr->Arg(0).TryInst();
    if (!hi || !lo) {
        return;
    }

    if (lo->GetOpcode() != IR::Opcode::IAdd32 || hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }

    IR::Value add_offset;
    IR::Value base;
    if (base = lo->Arg(0); IsReadConstSource(base)) {
        add_offset = lo->Arg(1);
    } else if (base = lo->Arg(1); IsReadConstSource(base)) {
        add_offset = lo->Arg(0);
    } else {
        return;
    }

    IR::Inst* sel = hi->Arg(1).TryInst();
    if (!sel || sel->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }

    IR::Value sel_true = sel->Arg(1);
    IR::Value sel_false = sel->Arg(2);
    if (!sel_true.IsImmediate() || !sel_false.IsImmediate() || sel_true.U32() != 1 ||
        sel_false.U32() != 0) {
        return;
    }

    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (auto [user, operand] : addr->Uses()) {
        ASSERT(user->GetOpcode() == IR::Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        IR::U32 offset = IR::U32{user->Arg(1)};
        IR::U32 dw_add_offset =
            add_offset.IsImmediate()
                ? ir.Imm32(add_offset.U32() >> 2u)
                : IR::U32{ir.ShiftRightLogical(IR::U32{add_offset}, ir.Imm32(2u))};
        if (offset.IsImmediate() && dw_add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + dw_add_offset.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, dw_add_offset));
        }
    }
}

} // Anonymous namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }

            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                // Only flatten ReadConstBuffer if it was marked as a sharp source in the
                // resource discovery pass
                auto inst_info = inst.Flags<IR::BufferInstInfo>();
                if (!inst_info.sharp_source) {
                    continue;
                }
            }

            SimplifyReadConstAddressAdd(inst);

            all_readconsts.push_back(&inst);

            auto offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }

            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* inst{queue.front()};
                queue.pop();

                if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = inst->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(inst);
                        auto offset = inst->Arg(1);
                        if (!offset.IsImmediate()) {
                            auto arg_inst = offset.Inst();
                            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                                visited.push_back(arg_inst);
                                queue.push(arg_inst);
                            }
                        }
                    }
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                    inst->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(inst)) {
                    continue;
                }
                for (size_t arg = inst->NumArgs(); arg--;) {
                    const IR::Value arg_value = inst->Arg(arg);
                    if (arg_value.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* arg_inst = arg_value.Inst();
                    if (std::ranges::find(visited, arg_inst) == visited.end()) {
                        visited.push_back(arg_inst);
                        queue.push(arg_inst);
                    }
                }
            }
        }
    }

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            // This is a duplicate of a readconst we've already visited
            continue;
        }

        IR::Inst* base = inst->Arg(0).Inst();
        if (auto* inst = base->Arg(0).TryInst();
            inst && inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        ASSERT_MSG(IsReadConstSource(base->Arg(0)), "ReadConst base low not from constant memory");
        ASSERT_MSG(IsReadConstSource(base->Arg(1)), "ReadConst base high not from constant memory");

        IR::Inst* ptr_lo = base->Arg(0).Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

        user_list[inst->Arg(1)] = inst;

        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        SetFlatbufOffset(readconst, GetFlatbufOffset(original));
    }

    program.info.RefreshFlatBuf();
}

} // namespace Shader::Optimization
