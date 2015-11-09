/*
Copyright (c) 2014, Insomniac Games
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "DebugHeap.h"
#include <stdint.h>
#include <assert.h>
#include <string.h>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__) || defined(linux)
#include <sys/mman.h>
#else
# error What are you?!
#endif

//-----------------------------------------------------------------------------
// Preliminaries

// An assert macro that kills the program.
// Substitute your own assert macro that takes formatted text.
#define ASSERT_FATAL(expr, message, ...) \
  assert(expr && message)

// Routines that wrap platform-specific virtual memory functionality.

static void* VmAllocate(size_t size);
static void VmFree(void* ptr, size_t size);
static void VmCommit(void* ptr, size_t size);
static void VmDecommit(void* ptr, size_t size);

// Windows virtual memory support.
#if defined(_WIN32)
typedef volatile LONG DebugHeapAtomicType;

static void* VmAllocate(size_t size)
{
  void* result = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
  ASSERT_FATAL(result, "Couldn't allocate address space");
  return result;
}

static void VmFree(void* ptr, size_t size)
{
  BOOL result = VirtualFree(ptr, 0, MEM_RELEASE);
  ASSERT_FATAL(result, "Failed to free memory");
  (void) size;
}

static void VmCommit(void* ptr, size_t size)
{
  LPVOID result = VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
  ASSERT_FATAL(result, "Failed to commit memory");
}

static void VmDecommit(void* ptr, size_t size)
{
  BOOL result = VirtualFree(ptr, size, MEM_DECOMMIT);
  ASSERT_FATAL(result, "Failed to decommit memory");
}

static DebugHeapAtomicType AtomicInc32(DebugHeapAtomicType *var)
{
  return InterlockedIncrement(var);
}

static DebugHeapAtomicType AtomicDec32(DebugHeapAtomicType *var)
{
  return InterlockedDecrement(var);
}
#endif

#if defined(__APPLE__) || defined(linux)
typedef volatile uint32_t DebugHeapAtomicType;

static void* VmAllocate(size_t size)
{
  void* result = mmap(NULL, size, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0);
  ASSERT_FATAL(result, "Couldn't allocate address space");
  return result;
}

static void VmFree(void* ptr, size_t size)
{
  int result = munmap(ptr, size);
  ASSERT_FATAL(0 == result, "Failed to free memory");
}

static void VmCommit(void* ptr, size_t size)
{
  int result = mprotect(ptr, size, PROT_READ|PROT_WRITE);
  ASSERT_FATAL(0 == result, "Failed to commit memory");
}

static void VmDecommit(void* ptr, size_t size)
{
  int result = madvise(ptr, size, MADV_DONTNEED);
  ASSERT_FATAL(0 == result, "madvise() failed");
  result = mprotect(ptr, size, PROT_NONE);
  ASSERT_FATAL(0 == result, "Failed to decommit memory");
}

static DebugHeapAtomicType AtomicInc32(DebugHeapAtomicType *var)
{
  return __sync_add_and_fetch(var, 1);
}

static DebugHeapAtomicType AtomicDec32(DebugHeapAtomicType *var)
{
  return __sync_sub_and_fetch(var, 1);
}
#endif


// We want to use the smallest page size possible, and that happens to be 4k on x86/x64.
// Using larger pages sizes would waste enormous amounts of memory.
enum
{
  kPageSize       = 4096,
};

typedef struct DebugBlockInfo
{
  uint32_t               m_Allocated    : 1;
  uint32_t               m_PageCount    : 31;
  uint32_t               m_PendingFree  : 1;
  uint32_t               m_PageIndex    : 31;
  struct DebugBlockInfo *m_Prev;
  struct DebugBlockInfo *m_Next;
} DebugBlockInfo;

struct DebugHeap
{
  uint32_t         m_MaxAllocs;
  uint32_t         m_PageCount;

  char*            m_BaseAddress;

  uint32_t         m_FreeListSize;
  DebugBlockInfo** m_FreeList;

  uint32_t         m_PendingListSize;
  DebugBlockInfo** m_PendingList;

  DebugBlockInfo** m_BlockLookup;
  DebugBlockInfo*  m_FirstUnusedBlockInfo;

  DebugBlockInfo*  m_Blocks;

  DebugHeapAtomicType m_ReentrancyGuard;
};

#define DEBUG_THREAD_GUARD_ENTER(heap) \
  ASSERT_FATAL(1 == AtomicInc32(&heap->m_ReentrancyGuard), "Unsynchronized MT usage detected")

#define DEBUG_THREAD_GUARD_LEAVE(heap) \
  ASSERT_FATAL(0 == AtomicDec32(&heap->m_ReentrancyGuard), "Unsynchronized MT usage detected")

static void* AdvancePtr(void* src, size_t amount)
{
  return (char*)src + amount;
}

DebugBlockInfo* AllocBlockInfo(DebugHeap* heap)
{
  DebugBlockInfo* result = heap->m_FirstUnusedBlockInfo;
  ASSERT_FATAL((uint32_t)result->m_Allocated, "Block info corrupted");
  ASSERT_FATAL((uint32_t)result->m_PendingFree, "Block info corrupted");
  heap->m_FirstUnusedBlockInfo = result->m_Next;

  memset(result, 0, sizeof *result);

  return result;
}

void FreeBlockInfo(DebugHeap* heap, DebugBlockInfo* block_info)
{
  block_info->m_Allocated = 1;
  block_info->m_PendingFree = 1;
  block_info->m_Prev = NULL;
  block_info->m_Next = heap->m_FirstUnusedBlockInfo;
  heap->m_FirstUnusedBlockInfo = block_info;
}

DebugHeap* DebugHeapInit(size_t mem_size_bytes)
{
  DebugHeap* self;

  const size_t mem_page_count    = mem_size_bytes / kPageSize;
  const size_t max_allocs        = mem_page_count / 2;

  const size_t bookkeeping_bytes =
    sizeof(DebugHeap) +
    (3 * mem_page_count * sizeof(DebugBlockInfo*)) +
    sizeof(DebugBlockInfo) * mem_page_count;

  const size_t bookkeeping_pages = (bookkeeping_bytes + kPageSize - 1) / kPageSize;
  const size_t total_pages       = bookkeeping_pages + mem_page_count;
  const size_t total_bytes       = total_pages * kPageSize;

  char* range = (char *)VmAllocate(total_bytes);
  if (!range)
  {
    return NULL;
  }

  VmCommit(range, bookkeeping_pages * kPageSize);

  self = (DebugHeap*) range;

  self->m_MaxAllocs       = (uint32_t) max_allocs;
  self->m_BaseAddress     = range + kPageSize * bookkeeping_pages;
  self->m_PageCount       = (uint32_t) mem_page_count;
  self->m_FreeList        = (DebugBlockInfo**) AdvancePtr(range, sizeof(DebugHeap));
  self->m_PendingList     = (DebugBlockInfo**) AdvancePtr(self->m_FreeList,     sizeof(DebugBlockInfo*) * mem_page_count);
  self->m_BlockLookup     = (DebugBlockInfo**) AdvancePtr(self->m_PendingList,  sizeof(DebugBlockInfo*) * mem_page_count);
  self->m_Blocks          = (DebugBlockInfo*)  AdvancePtr(self->m_BlockLookup,  sizeof(DebugBlockInfo*) * mem_page_count);
  self->m_FreeListSize    = 1;
  self->m_PendingListSize = 0;
  self->m_ReentrancyGuard = 0;

  // Initialize block allocation linked list
  {
    uint32_t i, count;
    for (i = 0, count = self->m_MaxAllocs; i < count; ++i)
    {
      self->m_Blocks[i].m_Allocated = 1;        // flag invalid
      self->m_Blocks[i].m_PendingFree = 1;      // flag invalid
      self->m_Blocks[i].m_Prev = NULL;
      self->m_Blocks[i].m_Next = (i + 1) < count ? &self->m_Blocks[i+1] : NULL;
    }
  }

  self->m_FirstUnusedBlockInfo = &self->m_Blocks[0];

  {
    DebugBlockInfo* root_block = AllocBlockInfo(self);

    root_block->m_PageIndex = 0;
    root_block->m_Allocated = 0;
    root_block->m_PendingFree = 0;
    root_block->m_PageCount = (uint32_t) mem_page_count;
    root_block->m_Prev = NULL;
    root_block->m_Next = NULL;

    self->m_FreeList[0] = root_block;
  }

  return self;
}

void DebugHeapDestroy(DebugHeap* heap)
{
  VmFree(heap, heap->m_PageCount * kPageSize);
}

static void* AllocFromFreeList(DebugHeap* heap, size_t page_req)
{
  // Cache in register to avoid repeated memory derefs
  DebugBlockInfo** const free_list = heap->m_FreeList;

  // Keep track of the best fitting block so far.
  DebugBlockInfo* best_block = NULL;
  uint32_t best_block_size = ~0u;
  uint32_t best_freelist_index = 0;
  uint32_t i, count;

  // First try the free list. This is slow. That's OK. It's a debug heap.
  for (i = 0, count = heap->m_FreeListSize; i < count; ++i)
  {
    DebugBlockInfo* block = free_list[i];
    uint32_t block_count = block->m_PageCount;
    ASSERT_FATAL(!block->m_Allocated, "block info corrupted");
    ASSERT_FATAL(!block->m_PendingFree, "block info corrupted");

    if (block_count >= page_req && block_count < best_block_size)
    {
      best_block = block;
      best_block_size = block_count;
      best_freelist_index = i;
    }
  }

  if (!best_block)
    return NULL;

  // Take this block off the free list.
  if (heap->m_FreeListSize > 1)
  {
    heap->m_FreeList[best_freelist_index] = heap->m_FreeList[heap->m_FreeListSize - 1];
  }
  heap->m_FreeListSize--;

  // Carve out the number of pages we need from our best block.
  {
    uint32_t unused_page_count = (uint32_t) (best_block_size - page_req);

    if (unused_page_count > 0)
    {
      // Allocate a new block to keep track of the tail end.
      DebugBlockInfo* tail_block = AllocBlockInfo(heap);
      tail_block->m_Allocated = 0;
      tail_block->m_PendingFree = 0;
      tail_block->m_PageIndex = best_block->m_PageIndex + best_block->m_PageCount - unused_page_count;
      tail_block->m_PageCount = unused_page_count;

      // Link it in to the chain.
      tail_block->m_Next = best_block->m_Next;
      tail_block->m_Prev = best_block;
      best_block->m_Next = tail_block;

      // Add it to the free list
      heap->m_FreeList[heap->m_FreeListSize++] = tail_block;

      // Patch up this block
      best_block->m_PageCount = (uint32_t) page_req;
    }
  }

  best_block->m_Allocated = 1;

  ASSERT_FATAL(heap->m_BlockLookup[best_block->m_PageIndex] == NULL, "block lookup corrupted");
  heap->m_BlockLookup[best_block->m_PageIndex] = best_block;

  {
    uint32_t i, max;
    for (i = 1, max = best_block->m_PageCount; i < max; ++i)
    {
      ASSERT_FATAL(heap->m_BlockLookup[best_block->m_PageIndex + i] == NULL, "block lookup corrupted");
    }
  }

  return heap->m_BaseAddress + ((uint64_t)(best_block->m_PageIndex)) * kPageSize;
}

static void* FinalizeAlloc(void* ptr_in, size_t user_size, size_t pages_allocated, size_t user_alignment)
{
  char* ptr = (char*) ptr_in;
  uint32_t ideal_offset, aligned_offset;

  // Commit pages in user-accessible section.
  VmCommit(ptr, (pages_allocated - 1) * kPageSize);

  // Decommit guard page to force crashes for stepping over bounds
  VmDecommit(ptr + (pages_allocated - 1) * kPageSize, kPageSize);

  // Align user allocation towards end of page, respecting user alignment.

  // Ideally the offset would be kPageSize - user_size % kPageSize.
  ideal_offset = ((uint32_t)(kPageSize - user_size)) % kPageSize;

  // Align down to meet user minimum alignment.
  aligned_offset = ideal_offset & ~((uint32_t)(user_alignment-1));

  // Garbage fill start of page.
  memset(ptr, 0xfc, aligned_offset);

  return ptr + aligned_offset;
}

static void FlushPendingFrees(DebugHeap* heap)
{
  uint32_t i, count;
  for (i = 0, count = heap->m_PendingListSize; i < count; ++i)
  {
    int block_removed = 0;

    DebugBlockInfo* block = heap->m_PendingList[i];
    DebugBlockInfo* prev;
    DebugBlockInfo* next;

    // Attempt to merge into an adjacent block to the left.
    // We can only merge with blocks that are free and not on this same pending list.
    if (NULL != (prev = block->m_Prev))
    {
      if (!prev->m_Allocated && !prev->m_PendingFree && prev->m_PageIndex + prev->m_PageCount == block->m_PageIndex)
      {
        // Linked list setup.
        prev->m_Next = block->m_Next;

        if (block->m_Next)
          block->m_Next->m_Prev = prev;

        // Increase size of left neighbor.
        prev->m_PageCount += block->m_PageCount;

        // Kill this pending block.
        FreeBlockInfo(heap, block);

        // Attempt to do right side coalescing with this other block instead.
        block = prev;

        // Don't try to delete this block later - we've already done that.
        block_removed = 1;
      }
    }

    // Attempt to merge into an adjacent block to the right.
    if (NULL != (next = block->m_Next))
    {
      if (!next->m_Allocated && !next->m_PendingFree && next->m_PageIndex == block->m_PageIndex + block->m_PageCount)
      {
        uint32_t fi, fcount;
        // Linked list setup.
        block->m_Next = next->m_Next;
        if (block->m_Next)
          block->m_Next->m_Prev = block;
        block->m_PageCount += next->m_PageCount;

        // Find this thing on the free list and remove it. This is slow.
        for (fi = 0, fcount = heap->m_FreeListSize; fi < fcount; ++fi)
        {
          if (heap->m_FreeList[fi] == next)
          {
            heap->m_FreeList[fi] = heap->m_FreeList[heap->m_FreeListSize-1];
            --heap->m_FreeListSize;
            break;
          }
        }

        // Free the R neighbor block now that we're done with it.
        FreeBlockInfo(heap, next);
      }
    }

    if (!block_removed)
    {
      // This block goes on the free list.
      block->m_PendingFree = 0;
      heap->m_FreeList[heap->m_FreeListSize++] = block;
    }
  }

  heap->m_PendingListSize = 0;
}

void* DebugHeapAllocate(DebugHeap* heap, size_t size, size_t alignment)
{
  void* ptr;
  uint32_t page_req;

  DEBUG_THREAD_GUARD_ENTER(heap);

  // Figure out how many pages we're going to need.
  // Always increment by one so we have room for a guard page at the end.
  page_req = 1 + (uint32_t) ((size + kPageSize - 1) / kPageSize);

  if (NULL != (ptr = AllocFromFreeList(heap, page_req)))
  {
    void* result = FinalizeAlloc(ptr, size, page_req, alignment);
    DEBUG_THREAD_GUARD_LEAVE(heap);
    return result;
  }

  // We couldn't find a block off the free list. Consolidate pending frees.
  FlushPendingFrees(heap);

  // Try again.
  if (NULL != (ptr = AllocFromFreeList(heap, page_req)))
  {
    void* result = FinalizeAlloc(ptr, size, page_req, alignment);
    DEBUG_THREAD_GUARD_LEAVE(heap);
    return result;
  }

  // Out of memory.
  DEBUG_THREAD_GUARD_LEAVE(heap);
  return NULL;
}

void DebugHeapFree(DebugHeap* heap, void* ptr_in)
{
  uintptr_t       ptr;
  uintptr_t       relative_offset;
  uint32_t        page_index;
  DebugBlockInfo *block;
  char           *block_base;

  DEBUG_THREAD_GUARD_ENTER(heap);

  // Figure out what page this belongs to.
  ptr = (uintptr_t) ptr_in;

  relative_offset = ptr - (uintptr_t) heap->m_BaseAddress;
  page_index = (uint32_t) (relative_offset / kPageSize);

  ASSERT_FATAL(page_index < heap->m_PageCount, "Invalid pointer %p freed", ptr_in);

  block = heap->m_BlockLookup[page_index];

  ASSERT_FATAL(block, "Double free of %p", ptr_in);

  ASSERT_FATAL((uint32_t)block->m_Allocated, "Block state corrupted");
  ASSERT_FATAL(!block->m_PendingFree, "Block state corrupted");

  // TODO: Check the fill pattern before the user pointer.

  block->m_Allocated = 0;
  block->m_PendingFree = 1;

  // Zero out this block in the lookup to catch double frees.
  heap->m_BlockLookup[page_index] = NULL;

  {
    uint32_t i, max;
    for (i = 1, max = block->m_PageCount; i < max; ++i)
    {
      ASSERT_FATAL(heap->m_BlockLookup[page_index + i] == NULL, "block lookup corrupted");
    }
  }

  // Add the block to the pending free list
  heap->m_PendingList[heap->m_PendingListSize++] = block;

  // Protect these blocks from reading or writing completely by decommiting the pages.
  // The last page is already inaccessible.
  block_base = heap->m_BaseAddress + ((uint64_t)block->m_PageIndex) * kPageSize;
  VmDecommit(block_base, ((uint64_t)(block->m_PageCount - 1)) * kPageSize);

  DEBUG_THREAD_GUARD_LEAVE(heap);
}

size_t DebugHeapGetAllocSize(DebugHeap* heap, void* ptr_in)
{
  uintptr_t ptr;
  uintptr_t relative_offset;
  uint32_t page_index;
  size_t result;
  DebugBlockInfo* block;

  DEBUG_THREAD_GUARD_ENTER(heap);

  // Figure out what page this belongs to.
  ptr = (uintptr_t) ptr_in;

  relative_offset = ptr - (uintptr_t) heap->m_BaseAddress;
  page_index = (uint32_t) (relative_offset / kPageSize);

  ASSERT_FATAL(page_index < heap->m_PageCount, "Invalid pointer %p", ptr_in);

  block = heap->m_BlockLookup[page_index];

  result = (block->m_PageCount - 1) * kPageSize - ptr % kPageSize;

  DEBUG_THREAD_GUARD_LEAVE(heap);

  return result;
}

int DebugHeapOwns(DebugHeap* heap, void* buffer)
{
  uintptr_t ptr;
  uintptr_t base;
  uintptr_t end;
  int status;

  DEBUG_THREAD_GUARD_ENTER(heap);

  ptr = (uintptr_t) buffer;
  base = (uintptr_t) heap->m_BaseAddress;
  end = base + ((uint64_t)heap->m_PageCount) * kPageSize;
  status = ptr >= base && ptr <= end;

  DEBUG_THREAD_GUARD_LEAVE(heap);
  return status;
}
