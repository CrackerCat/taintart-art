/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_OAT_QUICK_METHOD_HEADER_H_
#define ART_RUNTIME_OAT_QUICK_METHOD_HEADER_H_

#include "arch/instruction_set.h"
#include "base/macros.h"
#include "base/utils.h"
#include "quick/quick_method_frame_info.h"
#include "stack_map.h"

namespace art {

class ArtMethod;

// OatQuickMethodHeader precedes the raw code chunk generated by the compiler.
class PACKED(4) OatQuickMethodHeader {
 public:
  OatQuickMethodHeader() = default;
  OatQuickMethodHeader(uint32_t vmap_table_offset,
                       uint32_t code_size)
      : vmap_table_offset_(vmap_table_offset),
        code_size_(code_size) {
  }

  static OatQuickMethodHeader* FromCodePointer(const void* code_ptr) {
    uintptr_t code = reinterpret_cast<uintptr_t>(code_ptr);
    uintptr_t header = code - OFFSETOF_MEMBER(OatQuickMethodHeader, code_);
    DCHECK(IsAlignedParam(code, GetInstructionSetAlignment(kRuntimeISA)) ||
           IsAlignedParam(header, GetInstructionSetAlignment(kRuntimeISA)))
        << std::hex << code << " " << std::hex << header;
    return reinterpret_cast<OatQuickMethodHeader*>(header);
  }

  static OatQuickMethodHeader* FromEntryPoint(const void* entry_point) {
    return FromCodePointer(EntryPointToCodePointer(entry_point));
  }

  OatQuickMethodHeader(const OatQuickMethodHeader&) = default;
  OatQuickMethodHeader& operator=(const OatQuickMethodHeader&) = default;

  uintptr_t NativeQuickPcOffset(const uintptr_t pc) const {
    return pc - reinterpret_cast<uintptr_t>(GetEntryPoint());
  }

  bool IsOptimized() const {
    return GetCodeSize() != 0 && vmap_table_offset_ != 0;
  }

  const uint8_t* GetOptimizedCodeInfoPtr() const {
    DCHECK(IsOptimized());
    return code_ - vmap_table_offset_;
  }

  uint8_t* GetOptimizedCodeInfoPtr() {
    DCHECK(IsOptimized());
    return code_ - vmap_table_offset_;
  }

  const uint8_t* GetCode() const {
    return code_;
  }

  uint32_t GetCodeSize() const {
    // ART compiled method are prefixed with header, but we can also easily
    // accidentally use a function pointer to one of the stubs/trampolines.
    // We prefix those with 0xFF in the aseembly so that we can do DCHECKs.
    CHECK_NE(code_size_, 0xFFFFFFFF) << code_;
    return code_size_ & kCodeSizeMask;
  }

  const uint32_t* GetCodeSizeAddr() const {
    return &code_size_;
  }

  uint32_t GetVmapTableOffset() const {
    return vmap_table_offset_;
  }

  void SetVmapTableOffset(uint32_t offset) {
    vmap_table_offset_ = offset;
  }

  const uint32_t* GetVmapTableOffsetAddr() const {
    return &vmap_table_offset_;
  }

  const uint8_t* GetVmapTable() const {
    CHECK(!IsOptimized()) << "Unimplemented vmap table for optimizing compiler";
    return (vmap_table_offset_ == 0) ? nullptr : code_ - vmap_table_offset_;
  }

  bool Contains(uintptr_t pc) const {
    uintptr_t code_start = reinterpret_cast<uintptr_t>(code_);
    static_assert(kRuntimeISA != InstructionSet::kThumb2, "kThumb2 cannot be a runtime ISA");
    if (kRuntimeISA == InstructionSet::kArm) {
      // On Thumb-2, the pc is offset by one.
      code_start++;
    }
    return code_start <= pc && pc <= (code_start + GetCodeSize());
  }

  const uint8_t* GetEntryPoint() const {
    // When the runtime architecture is ARM, `kRuntimeISA` is set to `kArm`
    // (not `kThumb2`), *but* we always generate code for the Thumb-2
    // instruction set anyway. Thumb-2 requires the entrypoint to be of
    // offset 1.
    static_assert(kRuntimeISA != InstructionSet::kThumb2, "kThumb2 cannot be a runtime ISA");
    return (kRuntimeISA == InstructionSet::kArm)
        ? reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(code_) | 1)
        : code_;
  }

  template <bool kCheckFrameSize = true>
  uint32_t GetFrameSizeInBytes() const {
    uint32_t result = GetFrameInfo().FrameSizeInBytes();
    if (kCheckFrameSize) {
      DCHECK_ALIGNED(result, kStackAlignment);
    }
    return result;
  }

  QuickMethodFrameInfo GetFrameInfo() const {
    DCHECK(IsOptimized());
    return CodeInfo::DecodeFrameInfo(GetOptimizedCodeInfoPtr());
  }

  uintptr_t ToNativeQuickPc(ArtMethod* method,
                            const uint32_t dex_pc,
                            bool is_for_catch_handler,
                            bool abort_on_failure = true) const;

  uint32_t ToDexPc(ArtMethod* method, const uintptr_t pc, bool abort_on_failure = true) const;

  void SetHasShouldDeoptimizeFlag() {
    DCHECK_EQ(code_size_ & kShouldDeoptimizeMask, 0u);
    code_size_ |= kShouldDeoptimizeMask;
  }

  bool HasShouldDeoptimizeFlag() const {
    return (code_size_ & kShouldDeoptimizeMask) != 0;
  }

 private:
  static constexpr uint32_t kShouldDeoptimizeMask = 0x80000000;
  static constexpr uint32_t kCodeSizeMask = ~kShouldDeoptimizeMask;

  // The offset in bytes from the start of the vmap table to the end of the header.
  uint32_t vmap_table_offset_ = 0u;
  // The code size in bytes. The highest bit is used to signify if the compiled
  // code with the method header has should_deoptimize flag.
  uint32_t code_size_ = 0u;
  // The actual code.
  uint8_t code_[0];
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_QUICK_METHOD_HEADER_H_
