/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "debugger_interface.h"

#include <android-base/logging.h>

#include "base/mutex.h"
#include "thread-current-inl.h"
#include "thread.h"

#include <unordered_map>

namespace art {

// -------------------------------------------------------------------
// Binary GDB JIT Interface as described in
//   http://sourceware.org/gdb/onlinedocs/gdb/Declarations.html
// -------------------------------------------------------------------
extern "C" {
  typedef enum {
    JIT_NOACTION = 0,
    JIT_REGISTER_FN,
    JIT_UNREGISTER_FN
  } JITAction;

  struct JITCodeEntry {
    JITCodeEntry* next_;
    JITCodeEntry* prev_;
    const uint8_t *symfile_addr_;
    uint64_t symfile_size_;
    uint32_t ref_count;  // ART internal field.
  };

  struct JITDescriptor {
    uint32_t version_;
    uint32_t action_flag_;
    JITCodeEntry* relevant_entry_;
    JITCodeEntry* first_entry_;
  };

  // GDB will place breakpoint into this function.
  // To prevent GCC from inlining or removing it we place noinline attribute
  // and inline assembler statement inside.
  void __attribute__((noinline)) __jit_debug_register_code();
  void __attribute__((noinline)) __jit_debug_register_code() {
    __asm__("");
  }

  // Call __jit_debug_register_code indirectly via global variable.
  // This gives the debugger an easy way to inject custom code to handle the events.
  void (*__jit_debug_register_code_ptr)() = __jit_debug_register_code;

  // GDB will inspect contents of this descriptor.
  // Static initialization is necessary to prevent GDB from seeing
  // uninitialized descriptor.
  JITDescriptor __jit_debug_descriptor = { 1, JIT_NOACTION, nullptr, nullptr };

  // Incremented whenever __jit_debug_descriptor is modified.
  uint32_t __jit_debug_descriptor_timestamp = 0;

  struct DEXFileEntry {
    DEXFileEntry* next_;
    DEXFileEntry* prev_;
    const void* dexfile_;
  };

  DEXFileEntry* __art_debug_dexfiles = nullptr;

  // Incremented whenever __art_debug_dexfiles is modified.
  uint32_t __art_debug_dexfiles_timestamp = 0;
}

static size_t g_jit_debug_mem_usage
    GUARDED_BY(Locks::native_debug_interface_lock_) = 0;

static std::unordered_map<const void*, DEXFileEntry*> g_dexfile_entries
    GUARDED_BY(Locks::native_debug_interface_lock_);

void RegisterDexFileForNative(Thread* current_thread, const void* dexfile_header) {
  MutexLock mu(current_thread, *Locks::native_debug_interface_lock_);
  if (g_dexfile_entries.count(dexfile_header) == 0) {
    DEXFileEntry* entry = new DEXFileEntry();
    CHECK(entry != nullptr);
    entry->dexfile_ = dexfile_header;
    entry->prev_ = nullptr;
    entry->next_ = __art_debug_dexfiles;
    if (entry->next_ != nullptr) {
      entry->next_->prev_ = entry;
    }
    __art_debug_dexfiles = entry;
    __art_debug_dexfiles_timestamp++;
    g_dexfile_entries.emplace(dexfile_header, entry);
  }
}

void DeregisterDexFileForNative(Thread* current_thread, const void* dexfile_header) {
  MutexLock mu(current_thread, *Locks::native_debug_interface_lock_);
  auto it = g_dexfile_entries.find(dexfile_header);
  // We register dex files in the class linker and free them in DexFile_closeDexFile,
  // but might be cases where we load the dex file without using it in the class linker.
  if (it != g_dexfile_entries.end()) {
    DEXFileEntry* entry = it->second;
    if (entry->prev_ != nullptr) {
      entry->prev_->next_ = entry->next_;
    } else {
      __art_debug_dexfiles = entry->next_;
    }
    if (entry->next_ != nullptr) {
      entry->next_->prev_ = entry->prev_;
    }
    __art_debug_dexfiles_timestamp++;
    delete entry;
    g_dexfile_entries.erase(it);
  }
}

JITCodeEntry* CreateJITCodeEntry(const std::vector<uint8_t>& symfile) {
  DCHECK_NE(symfile.size(), 0u);

  // Make a copy of the buffer. We want to shrink it anyway.
  uint8_t* symfile_copy = new uint8_t[symfile.size()];
  CHECK(symfile_copy != nullptr);
  memcpy(symfile_copy, symfile.data(), symfile.size());

  JITCodeEntry* entry = new JITCodeEntry;
  CHECK(entry != nullptr);
  entry->symfile_addr_ = symfile_copy;
  entry->symfile_size_ = symfile.size();
  entry->prev_ = nullptr;
  entry->ref_count = 0;
  entry->next_ = __jit_debug_descriptor.first_entry_;
  if (entry->next_ != nullptr) {
    entry->next_->prev_ = entry;
  }
  g_jit_debug_mem_usage += sizeof(JITCodeEntry) + entry->symfile_size_;
  __jit_debug_descriptor.first_entry_ = entry;
  __jit_debug_descriptor.relevant_entry_ = entry;
  __jit_debug_descriptor.action_flag_ = JIT_REGISTER_FN;
  __jit_debug_descriptor_timestamp++;
  (*__jit_debug_register_code_ptr)();
  return entry;
}

void DeleteJITCodeEntry(JITCodeEntry* entry) {
  if (entry->prev_ != nullptr) {
    entry->prev_->next_ = entry->next_;
  } else {
    __jit_debug_descriptor.first_entry_ = entry->next_;
  }

  if (entry->next_ != nullptr) {
    entry->next_->prev_ = entry->prev_;
  }

  g_jit_debug_mem_usage -= sizeof(JITCodeEntry) + entry->symfile_size_;
  __jit_debug_descriptor.relevant_entry_ = entry;
  __jit_debug_descriptor.action_flag_ = JIT_UNREGISTER_FN;
  __jit_debug_descriptor_timestamp++;
  (*__jit_debug_register_code_ptr)();
  delete[] entry->symfile_addr_;
  delete entry;
}

// Mapping from code address to entry. Used to manage life-time of the entries.
static std::unordered_map<uintptr_t, JITCodeEntry*> g_jit_code_entries
    GUARDED_BY(Locks::native_debug_interface_lock_);

void IncrementJITCodeEntryRefcount(JITCodeEntry* entry, uintptr_t code_address) {
  DCHECK(entry != nullptr);
  DCHECK_EQ(g_jit_code_entries.count(code_address), 0u);
  entry->ref_count++;
  g_jit_code_entries.emplace(code_address, entry);
}

void DecrementJITCodeEntryRefcount(JITCodeEntry* entry, uintptr_t code_address) {
  DCHECK(entry != nullptr);
  DCHECK(g_jit_code_entries[code_address] == entry);
  if (--entry->ref_count == 0) {
    DeleteJITCodeEntry(entry);
  }
  g_jit_code_entries.erase(code_address);
}

JITCodeEntry* GetJITCodeEntry(uintptr_t code_address) {
  auto it = g_jit_code_entries.find(code_address);
  return it == g_jit_code_entries.end() ? nullptr : it->second;
}

size_t GetJITCodeEntryMemUsage() {
  return g_jit_debug_mem_usage + g_jit_code_entries.size() * 2 * sizeof(void*);
}

}  // namespace art
