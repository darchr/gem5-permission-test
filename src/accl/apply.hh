/*
 * Copyright (c) 2020 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ACCL_APPLY_HH__
#define __ACCL_APPLY_HH__

#include <queue>
#include <unordered_map>

#include "accl/util.hh"
#include "base/addr_range.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/Apply.hh"
#include "sim/clocked_object.hh"
#include "sim/port.hh"
#include "sim/system.hh"

namespace gem5
{

class Apply : public ClockedObject
{
  private:

    struct ApplyQueue{
      std::queue<PacketPtr> applyQueue;
      const uint32_t queueSize;
      bool sendPktRetry;

      bool blocked(){
        return (applyQueue.size() == queueSize);
      }
      bool empty(){
        return applyQueue.empty();
      }
      void push(PacketPtr pkt){
        applyQueue.push(pkt);
      }

      void pop(){
        applyQueue.pop();
      }

      void front(){
        applyQueue.front();
      }

      ApplyQueue(uint32_t qSize):
        queueSize(qSize){}
    };

    class ApplyRespPort : public ResponsePort
    {
      private:
        Apply *owner;
        bool _blocked;
        PacketPtr blockedPacket;

      public:
        ApplyRespPort(const std::string& name, Apply* owner):
          ResponsePort(name, owner), owner(owner),
          _blocked(false), blockedPacket(nullptr)
        {}

      protected:
        void trySendRetry();
        virtual AddrRangeList getAddrRanges();
        virtual bool recvTimingReq(PacketPtr pkt);
    };

    class ApplyReqPort : public RequestPort
    {
      private:
        Apply *owner;
        bool _blocked;
        PacketPtr blockedPacket;

      public:
        ApplyReqPort(const std::string& name, Apply* owner):
          RequestPort(name, owner), owner(owner),
          _blocked(false), blockedPacket(nullptr)
        {}

        void sendPacket(PacketPtr pkt);
        bool blocked() { return _blocked; }

      protected:
        void recvReqRetry() override;
        virtual bool recvTimingResp(PacketPtr pkt);
    };

    class ApplyMemPort : public RequestPort
    {
      private:
        Apply *owner;
        bool _blocked;
        PacketPtr blockedPacket;

      public:
        ApplyMemPort(const std::string& name, Apply* owner):
          RequestPort(name, owner), owner(owner),
          _blocked(false), blockedPacket(nullptr)
        {}

        void sendPacket(PacketPtr pkt);
        void trySendRetry();
        bool blocked(){ return _blocked;}

      protected:
        virtual bool recvTimingResp(PacketPtr pkt);
        void recvReqRetry() override;
    };

    bool handleWL(PacketPtr pkt);
    bool sendPacket();
    //one queue for write and one for read a priotizes write over read
    void readApplyBuffer();
    bool handleMemResp(PacketPtr resp);
    void writePushBuffer();

    //Events
    void processNextApplyCheckEvent();
    EventFunctionWrapper nextApplyCheckEvent;
    /* Syncronously checked
       If there are any active vertecies:
       create memory read packets + MPU::MPU::MemPortsendTimingReq
    */
    void processNextApplyEvent();
    EventFunctionWrapper nextApplyEvent;
    /* Activated by MPU::MPUMemPort::recvTimingResp and handleMemResp
       Perform apply and send the write request and read edgeList
       read + write
       Write edgelist loc in buffer
    */

    System* const system;
    const RequestorID requestorId;

    AddrRangeList getAddrRanges() const;

    ApplyQueue applyReadQueue;
    ApplyQueue applyWriteQueue;

    ApplyMemPort memPort;
    ApplyRespPort respPort;
    ApplyReqPort reqPort;

    std::unordered_map<RequestPtr, int> requestOffset;

  public:
    Apply(const ApplyParams &apply);

    Port& getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;
};

}

#endif // __ACCL_APPLY_HH__
