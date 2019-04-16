/*# Copyright (c) 2019 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Nima Ganjehloo
*/

#ifndef __VERILATOR_DPI_MANAGER_HH__
#define __VERILATOR_DPI_MANAGER_HH__
//verilator inlcudes
#include "VTop__Dpi.h"
#include "svdpi.h"

//gem5 includes
#include "base/logging.hh"

VerilatorMemBlackBox * memBlkBox;

//will run a doFetch from within blackbox wrapper
int ifetch (int imem_address, void* handle){
  DPRINTF(Verilator, "DPI INST FETCH MADE\n");
  VerilatorMemBlackBox* hndl = static_cast<VerilatorMemBlackBox *>(handle);
  hndl->doFetch();
  return hndl->blkbox->imem_dataout;
}
//will run a doMem from within blackbox wrapper
int datareq (int dmem_address, int dmem_writedata, unsigned char dmem_memread,
        unsigned char dmem_memwrite, const svBitVecVal* dmem_maskmode,
        unsigned char dmem_sext, void* handle){
   DPRINTF(Verilator, "DPI INST FETCH MADE\n");
  VerilatorMemBlackBox* hndl = static_cast<VerilatorMemBlackBox *>(handle);
  hndl->doMem();
  return hndl->blkbox->dmem_dataout;

}

//gives Dulaportedmemoryblackbox handle to verilatormemblkbox class
void* setGen5Handle (){
  panic_if( memBlkBox != nullptr,
          "Verilog should not try to access null gem5 model!");
  return (void*) blkbox;
}


#endif
