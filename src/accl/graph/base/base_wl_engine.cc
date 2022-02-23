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

#include "accl/graph/base/base_wl_engine.hh"

#include <string>

namespace gem5
{

BaseWLEngine::BaseWLEngine(const BaseWLEngineParams &params):
    BaseEngine(params),
    nextWLReadEvent([this]{ processNextWLReadEvent(); }, name()),
    nextWLReduceEvent([this]{ processNextWLReduceEvent(); }, name())
{}

bool
BaseWLEngine::handleWLUpdate(PacketPtr pkt)
{
    updateQueue.push(pkt);
    if(!nextWLReadEvent.scheduled()) {
        schedule(nextWLReadEvent, nextCycle());
    }
    return true;
}

void BaseWLEngine::processNextWLReadEvent()
{
    PacketPtr pkt = updateQueue.front();
    uint32_t data = *(pkt->getPtr<uint32_t>());

    Addr addr = pkt->getAddr();
    Addr req_addr = (addr / 64) * 64;
    Addr req_offset = addr % 64;

    PacketPtr memPkt = getReadPacket(req_addr, 64, requestorId);
    requestOffsetMap[memPkt->req] = req_offset;
    requestValueMap[memPkt->req] = value;

    if (memPortBlocked()) {
        sendMemReq(memPkt);
        updateQueue.pop();
    }
    if (!queue.empty() && !nextWLReadEvent.scheduled()) {
        schedule(nextWLReadEvent, nextCycle());
    }
}

void
BaseWLEngine::processNextMemRespEvent()
{
    PacketPtr resp = memRespQueue.front();
    uint8_t* respData = resp->getPtr<uint8_t>();
    Addr request_offset = requestOffsetMap[resp->req];
    uint32_t value = requestValueMap[resp->req];
    WorkListItem wl =  memoryToWorkList(data + request_offset);

    if (value < wl.temp_prop){
        //update prop with temp_prop
        wl.temp_prop = value;

        uint8_t* wlData = workListToMemory(wl);
        memcpy(respData + request_offset, wlData, sizeof(WorkListItem));
        PacketPtr writePkt  =
        getWritePacket(pkt->getAddr(), 64, respData, requestorId);

        if (!memPortBlocked()) {
            if (sendWLNotif(pkt->getAddr() + request_offset)) {
                sendMemReq(writePkt);
                memRespQueue.pop();
                // TODO: Erase map entries, delete wlData;
            }
        }
    }
    else {
        memRespQueue.pop();
    }
    if (!nextMemRespEvent.scheduled() && !memRespQueue.empty()){
            schedule(nextWLReduceEvent, nextCycle());
    }
}

}
