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

// demo.c - quick demo of debug heap functionality

#include <stdio.h>
#include <stdlib.h>

#include "DebugHeap.h"

int main(int argc, char* argv[])
{
  DebugHeap *heap;

  if (argc < 2) {
    fprintf(stderr, "Usage: demo <testcase>\n");
    fprintf(stderr, "\nTest cases:\n");
    fprintf(stderr, "0: setup+teardown\n");
    fprintf(stderr, "1: array overrun (should crash)\n");
    fprintf(stderr, "2: double free (should assert)\n");
    fprintf(stderr, "3: use after free (should crash)\n");
    exit(1);
  }

  heap = DebugHeapInit(2 * 1024 * 1024);

  switch (atoi(argv[1])) {
    case 0:
      {
        char* ptr;
        ptr = DebugHeapAllocate(heap, 128, 4);
        ptr[127] = 'a';
        DebugHeapFree(heap, ptr);
      }
      break;

    case 1:
      {
        char* ptr;
        ptr = DebugHeapAllocate(heap, 128, 4);
        ptr[128] = 'a'; // should crash here
      }
      break;

    case 2:
      {
        char* ptr;
        ptr = DebugHeapAllocate(heap, 128, 4);
        DebugHeapFree(heap, ptr);
        DebugHeapFree(heap, ptr); // should assert here
        ptr[127] = 'a';
      }
      break;

    case 3:
      {
        char* ptr;
        ptr = DebugHeapAllocate(heap, 128, 4);
        DebugHeapFree(heap, ptr);
        ptr[0] = 'a'; // should crash here
      }
      break;

    default:
      fprintf(stderr, "Unsupported test case\n");
      break;
  }

  DebugHeapDestroy(heap);
  
  return 0;
}
