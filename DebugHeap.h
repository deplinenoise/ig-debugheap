#pragma once

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

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

//-----------------------------------------------------------------------------
// Debug heap functionality
//
// These set of functions implement a debug heap. It provides the following
// features:
//
// - Array indexing errors (positive) trigger crashes, because allocations are
//   aligned as closely as possible up to an inaccessible virtual memory page.
//
// - Using memory after freeing it trigger crashes most of the time.
//
// - Double frees are detected most of the time.
//
// - Unsynchronized multi-threaded access is detected.
//
// To improve the chances of crashing on use-after-free or double frees,
// increase the size of the heap. Freed blocks are kept on an "observation
// list" for as long as possible to flush out these error classes, but it will
// eventually be reused.
//
// This heap is terribly slow, and wastes tons of memory. You only want to use
// it to track down memory errors. One neat way of doing that is to provide a
// heap interface that can dynamically switch to this heap, maybe with a
// configuration option. You can then hunt for memory errors without recompiling.

typedef struct DebugHeap DebugHeap;

// Create and initialize a debug heap.
// The size must be a multiple of the page size (4k), and should be generously padded.
// At the very least you need 2 pages per sub-4k allocation, but the more the better.
// The implementation is 64-bit clean and you can throw more than 4 GB at it just fine.
DebugHeap* DebugHeapInit(size_t size);

// Nuke a debug heap. All memory is returned to the OS.
void DebugHeapDestroy(DebugHeap* heap);

// Allocate memory from a debug heap.
// Size can be any value, except zero.
// Alignment must be a power of two.
// Returns NULL if the heap is full.
void* DebugHeapAllocate(DebugHeap* heap, size_t size, size_t alignment);

// Free memory in a debug heap.
void DebugHeapFree(DebugHeap* heap, void* ptr);

// Return the allocation size for a previously allocated block.
size_t DebugHeapGetAllocSize(DebugHeap* heap, void* ptr);

// A quick and dirty range check to see if a buffer could have come from a debug heap.
// Doesn't validate that the buffer is actually allocated.
int DebugHeapOwns(DebugHeap* heap, void* buffer);

#if defined(__cplusplus)
}
#endif
