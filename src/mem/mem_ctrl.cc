/*
 * Copyright (c) 2010-2020 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2013 Amin Farmahini-Farahani
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

#include "mem/mem_ctrl.hh"

#include "base/trace.hh"
#include "debug/DRAM.hh"
#include "debug/Drain.hh"
#include "debug/MemCtrl.hh"
#include "debug/NVM.hh"
#include "debug/QOS.hh"
#include "mem/mem_interface.hh"
#include "sim/system.hh"

MemCtrl::MemCtrl(const MemCtrlParams &p) :
    QoS::MemCtrl(p),
    port(name() + ".port", *this), isTimingMode(false),
    retryRdReq(false), retryWrReq(false),
    retryNVMRdReq(false), retryNVMWrReq(false), retryDRAMFillReq(false),
    retryRespEvent(false), dramHit(false),
    nextReqEvent([this]{ processNextReqEvent(); }, name()),
    respondEvent([this]{ processRespondEvent(); }, name()),
    dram(p.dram), nvm(p.nvm),
    readBufferSize((dram ? dram->readBufferSize : 0) +
                   (nvm ? nvm->readBufferSize : 0)),
    writeBufferSize((dram ? dram->writeBufferSize : 0) +
                    (nvm ? nvm->writeBufferSize : 0)),
    writeHighThreshold(writeBufferSize * p.write_high_thresh_perc / 100.0),
    writeLowThreshold(writeBufferSize * p.write_low_thresh_perc / 100.0),
    minWritesPerSwitch(p.min_writes_per_switch),
    writesThisTime(0), readsThisTime(0),
    maxReadQueueSize(p.max_read_queue_size),
    maxWriteQueueSize(p.max_write_queue_size),
    maxNvmReadQueueSize(p.max_nvm_read_queue_size),
    maxNvmWriteQueueSize(p.max_nvm_write_queue_size),
    maxDramFillQueueSize(p.max_dram_fill_queue_size),
    nvmReadQueueSize(0),
    nvmWriteQueueSize(0),
    dramFillQueueSize(0),
    totalReceivedPkts(0),
    memSchedPolicy(p.mem_sched_policy),
    frontendLatency(p.static_frontend_latency),
    backendLatency(p.static_backend_latency),
    tagCheckLatency(p.static_tagcheck_latency),
    commandWindow(p.command_window),
    nextBurstAt(0), prevArrival(0),
    nextReqTime(0),
    dramCacheSize(p.dram_cache_size),
    numEntries((p.dram_cache_size/64)),
    writeAllocatePolicy(p.write_allocate_policy),
    stats(*this)
{
    DPRINTF(MemCtrl, "Setting up controller\n");
    readQueue.resize(p.qos_priorities);
    nvmReadQueue.resize(p.qos_priorities);
    writeQueue.resize(p.qos_priorities);
    nvmWriteQueue.resize(p.qos_priorities);
    dramFillQueue.resize(p.qos_priorities);

    // Hook up interfaces to the controller
    if (dram)
        dram->setCtrl(this, commandWindow);
    if (nvm)
        nvm->setCtrl(this, commandWindow);

    fatal_if(!dram && !nvm, "Memory controller must have an interface");

    // perform a basic check of the write thresholds
    if (p.write_low_thresh_perc >= p.write_high_thresh_perc)
        fatal("Write buffer low threshold %d must be smaller than the "
              "high threshold %d\n", p.write_low_thresh_perc,
              p.write_high_thresh_perc);

    tagStoreDC.resize(numEntries);
}

void
MemCtrl::init()
{
   if (!port.isConnected()) {
        fatal("MemCtrl %s is unconnected!\n", name());
    } else {
        port.sendRangeChange();
    }
}

void
MemCtrl::startup()
{
    // remember the memory system mode of operation
    isTimingMode = system()->isTimingMode();

    if (isTimingMode) {
        // shift the bus busy time sufficiently far ahead that we never
        // have to worry about negative values when computing the time for
        // the next request, this will add an insignificant bubble at the
        // start of simulation
        nextBurstAt = curTick() + (dram ? dram->commandOffset() :
                                          nvm->commandOffset());
    }
}

/**
 * helper functions for debugging
 */
void
MemCtrl::printQueues(int i){
    if (i==0){
        std::cout << "readQueue: " << readQueue[0].size() << "\n";
        for (auto queue = readQueue.rbegin();
        queue != readQueue.rend(); ++queue) {
            for (int j=0; j< queue->size(); j++){
                std::cout << queue->at(j)->addr << "|"<<
                queue->at(j)->readyTime << ", ";
            }
            std::cout << "\n";

        }
        return;
    }
    else if (i==1){
        std::cout << "writeQueue: " << writeQueue[0].size() << "\n";
        for (auto queue = writeQueue.rbegin();
        queue != writeQueue.rend(); ++queue) {
            for (int j=0; j< queue->size(); j++){
                std::cout << queue->at(j)->addr << "|"<<
                queue->at(j)->readyTime << ", ";
            }
                std::cout << "\n";

        }
        return;
    }
    else if (i==2){
        std::cout << "nvmReadQueue: " << nvmReadQueue[0].size() << "\n";
        for (auto queue = nvmReadQueue.rbegin();
        queue != nvmReadQueue.rend(); ++queue) {
            for (int j=0; j< queue->size(); j++){
                std::cout << queue->at(j)->addr << "|" <<
                queue->at(j)->readyTime << ", ";
            }
                std::cout << "\n";

        }
        return;
    }
    else if (i==3){
        std::cout << "nvmWriteQueue: " << nvmWriteQueue[0].size() << "\n";
        for (auto queue = nvmWriteQueue.rbegin();
        queue != nvmWriteQueue.rend(); ++queue) {
            for (int j=0; j< queue->size(); j++){
                std::cout << queue->at(j)->addr << "|"<<
                queue->at(j)->readyTime << ", ";
            }
                std::cout << "\n";

        }
        return;
    }
    else if (i==4){
        std::cout << "dramFillQueue: " << dramFillQueue[0].size() << "\n";
        for (auto queue = dramFillQueue.rbegin();
        queue != dramFillQueue.rend(); ++queue) {
            for (int j=0; j< queue->size(); j++){
                std::cout << queue->at(j)->addr << "|"<<
                queue->at(j)->readyTime << ", ";
            }
                std::cout << "\n";

        }
        return;
    }
    else if (i==5){
        std::cout << "respQueue: " << respQueue.size() << "\n";
        if (!respQueue.empty()) {
            std::priority_queue<entry, std::vector<entry>,
                            std::greater<entry> > temp = respQueue;
            while (!temp.empty()){
                std::cout << temp.top().second->origRequestorPkt->addr
                << "|" << temp.top().second->readyTime <<
                ", ";
                temp.pop();
            }
        }
        std::cout << "\n\n";
        return;
    }
}

void
MemCtrl::printDramCache(){
    std::cout << "Dram Cache Content: " << tagStoreDC.size() << "\n";
    for (int i=0; i<tagStoreDC.size(); i++) {
        std::cout << i << ": " << tagStoreDC[i].index <<
        ", " << tagStoreDC[i].tag << ", "
        <<  tagStoreDC[i].valid_line << ", " <<
        tagStoreDC[i].dirty_line << "\n";
    }
}

Tick
MemCtrl::recvAtomic(PacketPtr origRequestorPkt)
{
    DPRINTF(MemCtrl, "recvAtomic: %s 0x%x\n",
                     origRequestorPkt->cmdString(),
                     origRequestorPkt->getAddr());

    panic_if(origRequestorPkt->cacheResponding(),
            "Should not see packets where cache is responding");

    Tick latency = 0;
    // do the actual memory access and turn the packet into a response
    if (dram &&
        dram->getAddrRange().contains(origRequestorPkt->getAddr())) {
        dram->access(origRequestorPkt);

        if (origRequestorPkt->hasData()) {
            // this value is not supposed to be accurate, just enough to
            // keep things going, mimic a closed page
            latency = dram->accessLatency();
        }
    } else if (nvm &&
               nvm->getAddrRange().contains(origRequestorPkt->getAddr())) {
        nvm->access(origRequestorPkt);

        if (origRequestorPkt->hasData()) {
            // this value is not supposed to be accurate, just enough to
            // keep things going, mimic a closed page
            latency = nvm->accessLatency();
        }
    } else {
        panic("Can't handle address range for packet %s\n",
              origRequestorPkt->print());
    }

    return latency;
}

Tick
MemCtrl::recvAtomicBackdoor(PacketPtr origRequestorPkt,
                            MemBackdoorPtr &backdoor)
{
    Tick latency = recvAtomic(origRequestorPkt);
    if (dram) {
        dram->getBackdoor(backdoor);
    } else if (nvm) {
        nvm->getBackdoor(backdoor);
    }
    return latency;
}

bool
MemCtrl::readQueueFull(unsigned int neededEntries) const
{
    DPRINTF(MemCtrl,
            "Read queue limit %d, current size %d, entries needed %d\n",
            readBufferSize, totalReadQueueSize + respQueue.size(),
            neededEntries);

    auto rdsize_new = totalReadQueueSize + respQueue.size() + neededEntries;
    return rdsize_new > readBufferSize;
}

bool
MemCtrl::writeQueueFull(unsigned int neededEntries) const
{
    DPRINTF(MemCtrl,
            "Write queue limit %d, current size %d, entries needed %d\n",
            writeBufferSize, totalWriteQueueSize, neededEntries);

    auto wrsize_new = (totalWriteQueueSize + neededEntries);
    return  wrsize_new > writeBufferSize;
}

bool
MemCtrl::nvmWriteQueueFull(unsigned int neededEntries) const
{
    int size = (nvmWriteQueueSize + neededEntries);
    // random size to compare with for now
    //return  size_new > 64;
    return  size > maxNvmWriteQueueSize;
}


bool
MemCtrl::nvmReadQueueFull(unsigned int neededEntries) const
{
    auto size = (nvmReadQueueSize + neededEntries);
    // random size to compare with for now
    //return  size_new > 64;
    return  size > maxNvmReadQueueSize;
}


bool
MemCtrl::dramFillQueueFull(unsigned int neededEntries) const
{
    auto size = (dramFillQueueSize + neededEntries);
    // random size to compare with for now
    //return  size_new > 64;
    return  size > maxDramFillQueueSize;
}

void
MemCtrl::addToReadQueue(PacketPtr origRequestorPkt,
                        unsigned int pkt_count, bool is_dram)
{
    // only add to the read queue here. whenever the request is
    // eventually done, set the readyTime, and call schedule()

    assert(origRequestorPkt->isRead() && !origRequestorPkt->isWrite());

    assert(pkt_count != 0);

    assert(is_dram);

    // if the request size is larger than burst size, the
    // origRequestorPkt is split into multiple packets (mem pkts)
    // Note if the origRequestorPkt starting address is not aligened
    // to burst size, the address of first packet is kept unaliged.
    // Subsequent packets are aligned to burst size boundaries.
    // This is to ensure we accurately check read packets against
    // packets in write queue.
    const Addr base_addr = origRequestorPkt->getAddr();
    Addr addr = base_addr;
    unsigned pktsServicedByWrQ = 0;
    unsigned pktsServicedByDRAMFillQ = 0;
    unsigned pktsServicedByNVMWrQ = 0;
    BurstHelper* burst_helper = NULL;

    uint32_t burst_size = is_dram ? dram->bytesPerBurst() :
                                    nvm->bytesPerBurst();

    // This flag is used for accessAndRespond function
    // for which we should specify to access either
    // DRAM or NVM.
    bool foundInDRAM = false;

    for (int cnt = 0; cnt < pkt_count; ++cnt) {
        unsigned size = std::min((addr | (burst_size - 1)) + 1,
                        base_addr + origRequestorPkt->getSize()) - addr;
        stats.readPktSize[ceilLog2(size)]++;
        stats.readBursts++;
        stats.requestorReadAccesses[origRequestorPkt->requestorId()]++;

        // First check write buffer to see if the data is already at
        // the controller
        bool foundInWrQ = false;
        Addr burst_addr = burstAlign(addr, is_dram);

        // if the burst address is not present then there is no need
        // looking any further
        if (isInWriteQueue.find(burst_addr) != isInWriteQueue.end()) {
            for (const auto& vec : writeQueue) {
                for (const auto& p : vec) {
                    // check if the read is subsumed in the write queue
                    // packet we are looking at
                    if (p->addr <= addr &&
                       ((addr + size) <= (p->addr + p->size)) &&
                          !p->is_waiting_for_nvm_read) {
                        foundInDRAM = false;
                        foundInWrQ = true;
                        stats.servicedByWrQ++;
                        pktsServicedByWrQ++;
                        DPRINTF(MemCtrl,
                                "Read to addr %lld with size %d serviced by "
                                "write queue\n",
                                addr, size);
                        stats.bytesReadWrQ += burst_size;
                        break;
                    }
                }
            }
        }

        // Also check in dramFillQueue
        bool foundInDRAMFillQ = false;

        for (const auto& vec : dramFillQueue) {
            for (const auto& p : vec) {
                // check if the read is subsumed in the write queue
                // packet we are looking at
                if (p->addr <= addr &&
                    ((addr + size) <= (p->addr + p->size)) &&
                        !p->is_waiting_for_nvm_read) {
                    foundInDRAM = true;
                    foundInDRAMFillQ = true;
                    //stats.servicedByWrQ++;
                    pktsServicedByDRAMFillQ++;
                    DPRINTF(MemCtrl,
                            "Read to addr %lld with size %d serviced by "
                            "dram fill queue\n",
                            addr, size);
                    //revisit stats
                    stats.bytesReadWrQ += burst_size;
                    break;
                }
            }
        }

        // Also check in NVMWriteQ
        bool foundInNVMWriteQ = false;
        for (const auto& vec : nvmWriteQueue) {
            for (const auto& p : vec) {
                // check if the read is subsumed in the write queue
                // packet we are looking at
                if (p->addr <= addr &&
                    ((addr + size) <= (p->addr + p->size)) &&
                       !p->is_waiting_for_nvm_read) {
                    foundInDRAM = false;
                    foundInNVMWriteQ = true;
                    //stats.servicedByWrQ++;
                    pktsServicedByNVMWrQ++;
                    DPRINTF(MemCtrl,
                            "Read to addr %lld with size %d serviced by "
                            "nvm write queue\n",
                            addr, size);

                    //revisit stats
                    stats.bytesReadWrQ += burst_size;
                    break;
                }
            }
        }

        // If not found in the write qs, make a memory packet and
        // push it onto the read queue
        if (!foundInWrQ && !foundInDRAMFillQ && !foundInNVMWriteQ) {
            DPRINTF(MemCtrl,
                            "Read to addr %lld with size %d was not "
                            "serviced by Forwarding checks!\n",
                            addr, size);
            // Make the burst helper for split packets
            if (pkt_count > 1 && burst_helper == NULL) {
                DPRINTF(MemCtrl, "Read to addr %lld translates to %d "
                        "memory requests\n",
                        origRequestorPkt->getAddr(), pkt_count);
                burst_helper = new BurstHelper(pkt_count);
            }

            MemPacket* mem_pkt;
            assert(is_dram && dram);

            mem_pkt = dram->decodePacket(origRequestorPkt,
                                         addr, size, true, true);
            // increment read entries of the rank
            dram->setupRank(mem_pkt->rank, true);

            mem_pkt->burstHelper = burst_helper;

            assert(!readQueueFull(1));
            stats.rdQLenPdf[totalReadQueueSize + respQueue.size()]++;

            DPRINTF(MemCtrl, "Adding to read queue\n");

            readQueue[mem_pkt->qosValue()].push_back(mem_pkt);

            // log packet
            logRequest(MemCtrl::READ,
                        origRequestorPkt->requestorId(),
                        origRequestorPkt->qosValue(),
                        mem_pkt->addr, 1, 0);

            // Update stats
            stats.avgRdQLen = totalReadQueueSize + respQueue.size();
        }

        // Starting address of next memory origRequestorPkt
        // (aligned to burst boundary)
        addr = (addr | (burst_size - 1)) + 1;
    }

    // If all packets are serviced by write queue, we send the repsonse back
    if (pktsServicedByWrQ == pkt_count ||
        pktsServicedByDRAMFillQ == pkt_count ||
        pktsServicedByNVMWrQ == pkt_count) {
        accessAndRespond(origRequestorPkt,
                        frontendLatency,
                        foundInDRAM);
        return;
    }

    // Update how many split packets are serviced by write queue
    if (burst_helper != NULL)
        burst_helper->burstsServiced = pktsServicedByWrQ;

    // If we are not already scheduled to get a request out of the
    // queue, do so now
    if (!nextReqEvent.scheduled()) {
        DPRINTF(MemCtrl, "Request scheduled immediately\n");
        schedule(nextReqEvent, curTick());
    }
}

void
MemCtrl::addToWriteQueue(PacketPtr origRequestorPkt,
                         unsigned int pkt_count, bool is_dram)
{
    // only add to the write queue here. whenever the request is
    // eventually done, set the readyTime, and call schedule()
    assert(!origRequestorPkt->isRead() && origRequestorPkt->isWrite());

    assert(pkt_count != 0);

    assert(is_dram);

    // if the request size is larger than burst size,
    // the origRequestorPkt is split into
    // multiple packets
    const Addr base_addr = origRequestorPkt->getAddr();
    Addr addr = base_addr;
    uint32_t burst_size = is_dram ? dram->bytesPerBurst() :
                                    nvm->bytesPerBurst();
    for (int cnt = 0; cnt < pkt_count; ++cnt) {

        unsigned size = std::min((addr | (burst_size - 1)) + 1,
                        base_addr + origRequestorPkt->getSize()) - addr;
        stats.writePktSize[ceilLog2(size)]++;
        stats.writeBursts++;
        stats.requestorWriteAccesses[origRequestorPkt->requestorId()]++;

        // see if we can merge with an existing item in the write
        // queue and keep track of whether we have merged or not
        bool merged = isInWriteQueue.find(burstAlign(addr, is_dram)) !=
            isInWriteQueue.end();

        // if the item was not merged we need to create a new write
        // and enqueue it
        if (!merged) {
            MemPacket* mem_pkt;

            // Every write packet received by write request queue,
            // will initiate a read to check tag and metadata. Thus,
            // we create a read packet and set 'read_before_write' flag
            // to show this is actually a write packet in the state of
            // checking tags and metadata. Later on, if needed, will set
            // 'read_before_write' to false and set the packet to write.
            mem_pkt = dram->decodePacket(origRequestorPkt,
                                         addr, size, true, true);
            mem_pkt->read_before_write = true;
            dram->setupRank(mem_pkt->rank, false);

            // FIX: probably need to add other wr q sizes here as well.
            assert(totalWriteQueueSize < writeBufferSize);
            stats.wrQLenPdf[totalWriteQueueSize]++;

            DPRINTF(MemCtrl, "Adding to write queue\n");

            writeQueue[mem_pkt->qosValue()].push_back(mem_pkt);
            isInWriteQueue.insert(burstAlign(addr, is_dram));

            // log packet
            logRequest(MemCtrl::WRITE, origRequestorPkt->requestorId(),
                       origRequestorPkt->qosValue(),
                       mem_pkt->addr, 1, 1);

            assert(totalWriteQueueSize == isInWriteQueue.size());

            // Update stats
            stats.avgWrQLen = totalWriteQueueSize;

        } else {
            DPRINTF(MemCtrl,
                    "Merging write burst with existing queue entry\n");

            // keep track of the fact that this burst effectively
            // disappeared as it was merged with an existing one
            stats.mergedWrBursts++;
        }

        // Starting address of next memory origRequestorPkt
        // (aligned to burst_size boundary)
        addr = (addr | (burst_size - 1)) + 1;
    }

    // COMMENT: This comment seems important!
    // we do not wait for the writes to be send to the actual memory,
    // but instead take responsibility for the consistency here and
    // snoop the write queue for any upcoming reads
    // @todo, if a origRequestorPkt size is larger than
    // burst size, we might need a different front end latency
    // COMMENT: this origRequestorPkt is not MemPacket, rather this
    // is the packet from the outer world

    // TODO: what should be the tag check latency
    //accessAndRespond(origRequestorPkt, frontendLatency
    //                 tagCheckLatency, false);


    // If we are not already scheduled to get a request out of the
    // queue, do so now
    if (!nextReqEvent.scheduled()) {
        DPRINTF(MemCtrl, "Request scheduled immediately\n");
        schedule(nextReqEvent, curTick());
    }
}

void
MemCtrl::addToDRAMFillQueue(const MemPacket *mem_pkt,
bool is_waiting_for_nvm_read)
{
    // this is the packet that came from resp queue
    // and is sent ot nvm read queue

    assert(!dramFillQueueFull(1));

    MemPacket* fill_pkt = dram->decodePacket(mem_pkt->origRequestorPkt,
                                             mem_pkt->getAddr(),
                                             mem_pkt->getSize(),
                                             false,
                                             true);
    dram->setupRank(fill_pkt->rank, false);

    fill_pkt->readyTime = MaxTick;

    if (is_waiting_for_nvm_read) {
        fill_pkt->is_waiting_for_nvm_read = true;
    }

    // the mem_pkt needs to become a write request
    // now
    dramFillQueue[fill_pkt->qosValue()].push_back(fill_pkt);
    logRequest(MemCtrl::WRITE, fill_pkt->requestorId(),
    fill_pkt->qosValue(),
                       fill_pkt->addr, 1, 4);

    dramFillQueueSize++;

    // update the DRAM tags as well

    int index = bits(fill_pkt->getAddr(),
            ceilLog2(64)+ceilLog2(numEntries), ceilLog2(64));

    tagStoreDC[index].tag = returnTag(fill_pkt->getAddr());

    tagStoreDC[index].nvmAddr = fill_pkt->getAddr();

    // make sure that the block is set to be valid and clean
    tagStoreDC[index].valid_line = true;
    tagStoreDC[index].dirty_line = true;
    //isInWriteQueue.insert(burstAlign(addr, is_dram));

    if (!nextReqEvent.scheduled()) {
        schedule(nextReqEvent, curTick());
    }
}

void
MemCtrl::updateMemPktInDRAMFillQueue(const MemPacket *mem_pkt)
{
    bool found = false;
    for (auto queue = dramFillQueue.rbegin();
        queue != dramFillQueue.rend(); ++queue) {
            assert(queue->size() != 0);
            // If we are changing command type, incorporate the minimum
            // bus turnaround delay
            for (int i=0; i< queue->size(); i++) {
                if (queue->at(i)->addr == mem_pkt->addr &&
                    queue->at(i)->is_waiting_for_nvm_read) {
                        found = true;
                        queue->at(i)->origRequestorPkt->data =
                               mem_pkt->origRequestorPkt->data;
                        queue->at(i)->is_waiting_for_nvm_read = false;
                        break;
                    }
            }
            if (found) {
                break;
            }
        }
    assert(found);
}

void
MemCtrl::addToNVMReadQueue(const MemPacket* mem_pkt)
{
    // return true if this request can succeed
    // false otherwise

    // COMMENT: Do we need to snoop the write queue?

    // COMMENT: Can there be another read request
    // to the same address in this queue
    // If yes, then they should be merged

    //assert(mem_pkt->isRead());
    assert(!nvmReadQueueFull(1));

    // TODO: delete the old mem origRequestorPkt object
    // COMMENT: Should overwrite the mem_pkt?
    // COMMENT: Currently, we are assuming that
    // a origRequestorPkt will be decomposed in only one mem_pkt
    // which probably is not a reasonable assumption
    MemPacket* nvm_pkt = nvm->decodePacket(mem_pkt->origRequestorPkt,
                                           mem_pkt->getAddr(),
                                           mem_pkt->getSize(),
                                           true,
                                           false);
    nvm->setupRank(nvm_pkt->rank, true);
    nvm_pkt->readyTime = MaxTick;
    nvmReadQueue[nvm_pkt->qosValue()].push_back(nvm_pkt);
    logRequest(MemCtrl::READ, nvm_pkt->requestorId(),
                nvm_pkt->qosValue(),
                nvm_pkt->addr, 1, 2);
    nvmReadQueueSize++;

    if (!nextReqEvent.scheduled()) {
        schedule(nextReqEvent, curTick());
    }
}

void
MemCtrl::addToNVMWriteQueue(const MemPacket* mem_pkt)
{
    //assert(mem_pkt->isWrite());
    assert(!nvmWriteQueueFull(1));

    // COMMENT: Should overwrite the mem_pkt?
    // COMMENT: Currently, we are assuming that
    // a origRequestorPkt will be decomposed in only one mem_pkt
    // which probably is not a reasonable assumption
    MemPacket* nvm_pkt = nvm->decodePacket(mem_pkt->origRequestorPkt,
                                            mem_pkt->getAddr(),
                                            mem_pkt->getSize(), false, false);
    nvm->setupRank(nvm_pkt->rank, true);
    nvm_pkt->readyTime = MaxTick;

    nvmWriteQueue[nvm_pkt->qosValue()].push_back(nvm_pkt);
    logRequest(MemCtrl::WRITE, nvm_pkt->requestorId()
    , nvm_pkt->qosValue(),
                       nvm_pkt->addr, 1, 3);
    nvmWriteQueueSize++;

    if (!nextReqEvent.scheduled()) {
        schedule(nextReqEvent, curTick());
    }

}

void
MemCtrl::handleHit(MemPacket* mem_pkt)
{
    if (mem_pkt->isRead() && !mem_pkt->read_before_write) {
        dramHit = true;
        stats.dramCacheHit++;
        // Nothing's required to be done,
        // just send the respond to requestor

    } else if (mem_pkt->isRead() && mem_pkt->read_before_write) {
        // write packet
        if (!dramFillQueueFull(1)) {
            addToDRAMFillQueue(mem_pkt, false);
            dramHit = true;
            stats.dramCacheHit++;
            // jump to pop respQ and afterwards
        } else {
            retryDRAMFillReq = true;
            retryRespEvent = true;
            dramHit = false;
        }
    }

}

void
MemCtrl::handleCleanMiss(MemPacket* mem_pkt)
{
    dramHit = false;
    if (mem_pkt->isRead() && !mem_pkt->read_before_write) {
        if (!nvmReadQueueFull(1) && !dramFillQueueFull(1)) {
            addToNVMReadQueue(mem_pkt);
            addToDRAMFillQueue(mem_pkt, true);
            stats.dramCacheMiss++;
        } else {
            if (nvmReadQueueFull(1)) {
                retryNVMRdReq = true;
            }
            if (dramFillQueueFull(1)) {
                retryDRAMFillReq = true;
            }
            retryRespEvent = true;
        }
    } else { // write packet
        if (!writeAllocatePolicy) { // false = no allocate on writes
            if (!nvmWriteQueueFull(1)) {
                //FIX: Mark said that even in this case
                // it does a read from NVM before wrting it
                // to NVM. (non-temporal store is a proof
                // on Intel Cascade)
                addToNVMWriteQueue(mem_pkt);
                stats.dramCacheMiss++;
                // jump to pop respQ and afterwards
            } else {
                retryNVMWrReq = true;
                retryRespEvent = true;
            }
        } else { // true = allocate on writes
            if (!nvmReadQueueFull(1) && !dramFillQueueFull(1)) {
                addToNVMReadQueue(mem_pkt);
                addToDRAMFillQueue(mem_pkt, true);
                stats.dramCacheMiss++;
                // jump to pop respQ and afterwards
            } else {
                if (nvmReadQueueFull(1)) {
                    retryNVMRdReq = true;
                }
                if (dramFillQueueFull(1)) {
                    retryDRAMFillReq = true;
                }
                retryRespEvent = true;
                // keep the top of respQ and reschedule it
                // and
                // reschedule for reqEvent to make
                // room available in the queues
            }
        }
    }
}

MemPacket*
MemCtrl::createVictimMemPkt(MemPacket* mem_pkt)
{
    int index = bits(mem_pkt->getAddr(),
                        ceilLog2(64)+ceilLog2(numEntries), ceilLog2(64));

    Addr base_addr = tagStoreDC[index].nvmAddr;
    Addr addr = base_addr;

    uint32_t burst_size = nvm->bytesPerBurst();
    unsigned size = std::min((addr | (burst_size - 1)) + 1,
                        base_addr + mem_pkt->getSize()) - addr;
    MemPacket* victim = dram->decodePacket(mem_pkt->origRequestorPkt,
                                           addr, size, false, false);
    return victim;
}

void
MemCtrl::handleDirtyMiss(MemPacket* mem_pkt)
{
    dramHit = false;
    MemPacket* victim = createVictimMemPkt(mem_pkt);
    if (mem_pkt->isRead() && !mem_pkt->read_before_write) {
        if (!nvmReadQueueFull(1) &&
            !nvmWriteQueueFull(1) &&
            !dramFillQueueFull(1)) {
                addToNVMWriteQueue(victim);
                addToNVMReadQueue(mem_pkt);
                addToDRAMFillQueue(mem_pkt, true);
                stats.dramCacheMiss++;

        } else {
            if (nvmReadQueueFull(1)) {
                retryNVMRdReq = true;
            }
            if (nvmWriteQueueFull(1)) {
                retryNVMWrReq = true;
            }
            if (dramFillQueueFull(1)) {
                retryDRAMFillReq = true;
            }
            retryRespEvent = true;
        }
    } else { // write packet
        if (!writeAllocatePolicy) { // false = no allocate on writes
            if (!nvmWriteQueueFull(1) ) {
                addToNVMWriteQueue(victim);
                stats.dramCacheMiss++;
                // jump to pop respQ and afterwards
            } else {
                retryNVMWrReq = true;
                retryRespEvent = true;
            }
        } else { // true = allocate on writes
            if (!nvmReadQueueFull(1) &&
                !nvmWriteQueueFull(1) &&
                !dramFillQueueFull(1)) {
                    addToNVMReadQueue(mem_pkt);
                    addToNVMWriteQueue(victim);
                    addToDRAMFillQueue(mem_pkt, true);
                    stats.dramCacheMiss++;
                    // jump to pop respQ and afterwards
            } else {
                if (nvmReadQueueFull(1)) {
                    retryNVMRdReq = true;
                }
                if (nvmWriteQueueFull(1)) {
                    retryNVMWrReq = true;
                }
                if (dramFillQueueFull(1)) {
                    retryDRAMFillReq = true;
                }
                retryRespEvent = true;
            }

        }
    }
}


void
MemCtrl::printQs() const
{
#if TRACING_ON
    DPRINTF(MemCtrl, "===READ QUEUE===\n\n");
    for (const auto& queue : readQueue) {
        for (const auto& packet : queue) {
            DPRINTF(MemCtrl, "Read %lu\n", packet->addr);
        }
    }

    DPRINTF(MemCtrl, "\n===RESP QUEUE===\n\n");
    //A priority queue's interface does not allow iteration.
    //So, relying on the following workaround (but there should
    //be better alternatives)
    typedef std::pair<Tick, MemPacket*> entry;
    std::priority_queue<entry, std::vector<entry>,
                        std::greater<entry> > temp = respQueue;

    while (!temp.empty()){
        DPRINTF(MemCtrl, "Response %lu\n", temp.top().second->addr);
        temp.pop();
    }

    DPRINTF(MemCtrl, "\n===WRITE QUEUE===\n\n");
    for (const auto& queue : writeQueue) {
        for (const auto& packet : queue) {
            DPRINTF(MemCtrl, "Write %lu\n", packet->addr);
        }
    }
#endif // TRACING_ON
}

inline Addr
MemCtrl::returnTag(Addr request_addr)
{
    int index_bits = ceilLog2(numEntries);
    int block_bits = ceilLog2(64);
    return bits(request_addr, 63, (index_bits+block_bits));
}

bool
MemCtrl::recvTimingReq(PacketPtr origRequestorPkt)
{
    // std::cout << "In recvTimingReq-> tick: " << curTick() << " addr: "
    // << origRequestorPkt->getAddr() << " // " << totalReceivedPkts << "\n";
    // printQueues(0);
    // printQueues(1);
    // printQueues(2);
    // printQueues(3);
    // printQueues(4);
    // printQueues(5);
    // std::cout << "Finished printing queues in recvTimingReq\n\n";

    // This is where we enter from the outside world
    DPRINTF(MemCtrl, "recvTimingReq: request %s addr %lld size %d\n",
            origRequestorPkt->cmdString(), origRequestorPkt->getAddr(),
            origRequestorPkt->getSize());

    panic_if(origRequestorPkt->cacheResponding(),
            "Should not see packets where cache is responding");

    panic_if(!(origRequestorPkt->isRead() || origRequestorPkt->isWrite()),
             "Should only see read and writes at memory controller\n");

    // Calc avg gap between requests
    if (prevArrival != 0) {
        stats.totGap += curTick() - prevArrival;
    }
    prevArrival = curTick();

    // What type of media does this packet access?
    //bool is_dram = false;

    // MARYAM: the next line is a MUST to make sure every single packet
    // checks DRAM first. Don't change it.
    bool is_dram = true;

    // COMMENT: is_dram kind of now means if this request should be
    // forwarded to DRAM or not.

    // COMMENT: Don't think, this will happen in the same
    // way

    // COMMENT: DRAM's range (is also not a constant) is a subset of NVM
    // One way: check tags here... if they match, is_dram would be true
    // if they do not is_dram : false. In both cases add tag check latency
    // later on when the request is sent to memory

    // just validate that origRequestorPkt's address maps to the nvm
    assert(nvm && nvm->getAddrRange().contains(origRequestorPkt->getAddr()));
    assert(dram && dram->getAddrRange().contains(origRequestorPkt->getAddr()));

    // Find out how many memory packets a origRequestorPkt translates to
    // If the burst size is equal or larger than the origRequestorPkt size,
    // then a origRequestorPkt translates to only one memory packet.
    // Otherwise, a origRequestorPkt translates to multiple memory packets

    // COMMENT: A memory packet can't be bigger than the burst size
    // COMMENT: This implements a no-allocate on write miss policy
    // which means that on a write miss in dram, we should send this packet
    // to nvm
    // COMMENT: Also, DRAMCache is following a writeback policy, which means
    // that we should write a block back to nvm if it is valid and dirty and
    // needs to be evicted from DRAM cache.

    unsigned size = origRequestorPkt->getSize();
    //uint32_t burst_size = is_dram ? dram->bytesPerBurst() :
    //                                nvm->bytesPerBurst();
    uint32_t burst_size = dram->bytesPerBurst();
    unsigned offset = origRequestorPkt->getAddr() & (burst_size - 1);
    unsigned int pkt_count = divCeil(offset + size, burst_size);

    // COMMENT: This pkt_count is probably memory packet count

    // COMMENT: We never pass any QoS priority value, so I think
    // the packet's pkt_priority will stay 0.
    //
    // run the QoS scheduler and assign a QoS priority value to the packet
    qosSchedule( { &readQueue, &writeQueue }, burst_size, origRequestorPkt);

    // check local buffers and do not accept if full
    if (origRequestorPkt->isWrite()) {
        assert(size != 0);
        if (writeQueueFull(pkt_count)) {
            DPRINTF(MemCtrl, "Write queue full, not accepting\n");
            // remember that we have to retry this port
            retryWrReq = true;
            stats.numWrRetry++;
            return false;
        } else {
            addToWriteQueue(origRequestorPkt, pkt_count, is_dram);
            totalReceivedPkts = totalReceivedPkts + pkt_count;
            stats.writeReqs++;
            stats.bytesWrittenSys += size;
        }
    } else { //read
        assert(origRequestorPkt->isRead());
        assert(size != 0);
        if (readQueueFull(pkt_count)) {
            DPRINTF(MemCtrl, "Read queue full, not accepting\n");
            // remember that we have to retry this port
            retryRdReq = true;
            stats.numRdRetry++;
            return false;
        } else {
            addToReadQueue(origRequestorPkt, pkt_count, is_dram);
            totalReceivedPkts = totalReceivedPkts + pkt_count;
            // COMMENT: revisit stats
            stats.readReqs++;
            stats.bytesReadSys += size;
        }
    }

    // std::cout << "_____________________________________________________\n";

    return true;
}

void
MemCtrl::processRespondEvent()
{
    // std::cout << "In processRespondEvent: " << curTick() << "\n";
    // printQueues(0);
    // printQueues(1);
    // printQueues(2);
    // printQueues(3);
    // printQueues(4);
    // printQueues(5);
    // std::cout << "Finished printing queues in processRespondEvent\n\n";


    DPRINTF(MemCtrl,
            "processRespondEvent(): Some req has reached its readyTime\n");

    // COMMENT: What to do if the response is coming from
    // nvm. And also should we take care of it here or in the processNext
    // ReqEvent where we schedule the respond event

    // COMMENT: A read response from nvm should also
    // move data to the dram
    // COMMENT: things to do: update tags in MC
    // and sent data to dram prob by creating a write req (dummy)

    MemPacket* mem_pkt = respQueue.top().second;

    dramHit = false;

    //TODO: add assert lines for cases that potentially
    // go to respQueue.

    //check tag and metadata
    if (mem_pkt->isDram()) {
        int index = bits(mem_pkt->getAddr(),
                        ceilLog2(64)+ceilLog2(numEntries), ceilLog2(64));
        Addr currTag = returnTag(mem_pkt->getAddr());

        // THE ENTRY IS INVALID, POPULATE
        if (!(tagStoreDC[index].valid_line)) {
            handleCleanMiss(mem_pkt);
        }

        // DRAM CACHE HIT
        else if (tagStoreDC[index].tag == currTag &&
                 tagStoreDC[index].valid_line) {
            handleHit(mem_pkt);
        }

        // DRAM CACHE MISS, CLEAN
        else if (tagStoreDC[index].tag != currTag &&
                 tagStoreDC[index].valid_line &&
                 !(tagStoreDC[index].dirty_line)) {
            handleCleanMiss(mem_pkt);
        }

        // DRAM CACHE MISS, Dirty
        else if (tagStoreDC[index].tag != currTag &&
                 tagStoreDC[index].valid_line &&
                 tagStoreDC[index].dirty_line) {
            handleDirtyMiss(mem_pkt);
        }
    }

    //NVM ACCESS, no need to check tag and metadata
    else {
        assert(mem_pkt->isRead());
        updateMemPktInDRAMFillQueue(mem_pkt);
        if (mem_pkt->origRequestorPkt->isWrite()) {
            // jump to pop respQ and afterwards
        } else if (mem_pkt->origRequestorPkt->isRead()) {
            // nothing's required to be done.
            // continue the procRespEventFunc
            // same as for a usual read packet.
        }

    }

    if (retryRespEvent) {
        if (!nextReqEvent.scheduled()) {
                schedule(nextReqEvent, curTick());
        }
        retryRespEvent = false;
        dramHit = false;
        return;
    }

    //-------------------------------------------------------------------//
    if (mem_pkt->isDram()) {
            // media specific checks and functions
            // when read response is complete
            dram->respondEvent(mem_pkt->rank);
    }

    if (mem_pkt->origRequestorPkt->isRead()) {
        if (mem_pkt->burstHelper) {
            // it is a split packet
            //panic("Split packets are not handled properly!\n");

            mem_pkt->burstHelper->burstsServiced++;
            if (mem_pkt->burstHelper->burstsServiced ==
                mem_pkt->burstHelper->burstCount) {
                // we have now serviced all children packets of a system packet
                // so we can now respond to the requestor
                // @todo we probably want to have a different
                // front end and back end latency for split packets
                if ((mem_pkt->isDram() && dramHit) || (!mem_pkt->isDram())) {
                    accessAndRespond(mem_pkt->origRequestorPkt,
                    frontendLatency + backendLatency,
                    mem_pkt->isDram());
                    delete mem_pkt->burstHelper;
                    mem_pkt->burstHelper = NULL;
                }
            }
        } else {
            // it is not a split packet
            if ((mem_pkt->isDram() && dramHit) || (!mem_pkt->isDram())) {
                    accessAndRespond(mem_pkt->origRequestorPkt,
                    frontendLatency + backendLatency,
                    mem_pkt->isDram());

            }
        }
    }
    //-------------------------------------------------------------------//

    //update the ready_time of all the packets in the resp Q
    // if needed
    // if (curTick() > respQueue.top().second->readyTime) {
    //     int delay = curTick() - respQueue.top().second->readyTime;
    //     std::priority_queue<entry, std::vector<entry>,
    //                     std::greater<entry> > temp;
    //     while (!respQueue.empty()) {
    //         respQueue.top().second->readyTime += delay;
    //         temp.push(respQueue.top());
    //         respQueue.pop();
    //     }
    //     while (!temp.empty()) {
    //         respQueue.push(temp.top());
    //         temp.pop();
    //     }
    // }

    // delete the mem origRequestorPkt from resp queue
    delete respQueue.top().second;
    respQueue.pop();

    if (!respQueue.empty()) {
        assert(respQueue.top().second->readyTime >= curTick());
        assert(!respondEvent.scheduled());
        schedule(respondEvent, respQueue.top().second->readyTime);
    } else {
        // if there is nothing left in any queue, signal a drain
        if (drainState() == DrainState::Draining &&
            !totalWriteQueueSize && !totalReadQueueSize &&
            allIntfDrained()) {

            DPRINTF(Drain, "Controller done draining\n");
            signalDrainDone();
        } else if (mem_pkt->isDram()) {
            // check the refresh state and kick the refresh event loop
            // into action again if banks already closed and just waiting
            // for read to complete
            dram->checkRefreshState(mem_pkt->rank);
        }
    }

    // We have made a location in the queue available at this point,
    // so if there is a read that was forced to wait, retry now
    if (retryRdReq) {
        retryRdReq = false;
        port.sendRetryReq();
    }
    dramHit = false;
    // std::cout << "_____________________________________________________\n";
}

MemPacketQueue::iterator
MemCtrl::chooseNext(MemPacketQueue& queue, Tick extra_col_delay)
{
    // COMMENT: What policies scheduling can have
    // if DRAM cache is used
    // This method does the arbitration between requests.
    MemPacketQueue::iterator ret = queue.end();

    if (!queue.empty()) {
        if (queue.size() == 1) {
            // available rank corresponds to state refresh idle
            MemPacket* mem_pkt = *(queue.begin());
            if (packetReady(mem_pkt)) {
                ret = queue.begin();
                DPRINTF(MemCtrl, "Single request, going to a free rank\n");
            } else {
                DPRINTF(MemCtrl, "Single request, going to a busy rank\n");
            }
        } else if (memSchedPolicy == Enums::fcfs) {
            // check if there is a packet going to a free rank
            for (auto i = queue.begin(); i != queue.end(); ++i) {
                MemPacket* mem_pkt = *i;
                if (packetReady(mem_pkt)) {
                    ret = i;
                    break;
                }
            }
        } else if (memSchedPolicy == Enums::frfcfs) {
            // COMMENT: I think this frfcfs is the policy we can safely assume
            ret = chooseNextFRFCFS(queue, extra_col_delay);
        } else {
            panic("No scheduling policy chosen\n");
        }
    }
    return ret;
}

MemPacketQueue::iterator
MemCtrl::chooseNextFRFCFS(MemPacketQueue& queue, Tick extra_col_delay)
{
    auto selected_pkt_it = queue.end();
    Tick col_allowed_at = MaxTick;

    // time we need to issue a column command to be seamless
    const Tick min_col_at = std::max(nextBurstAt + extra_col_delay, curTick());

    // find optimal packet for each interface
    if (dram && nvm) {
        // create 2nd set of parameters for NVM
        auto nvm_pkt_it = queue.end();
        Tick nvm_col_at = MaxTick;

        // Select packet by default to give priority if both
        // can issue at the same time or seamlessly

        // COMMENT: interfaces pick the optimal packet
        // from the read queue
        std::tie(selected_pkt_it, col_allowed_at) =
                 dram->chooseNextFRFCFS(queue, min_col_at);
        std::tie(nvm_pkt_it, nvm_col_at) =
                 nvm->chooseNextFRFCFS(queue, min_col_at);

        // Compare DRAM and NVM and select NVM if it can issue
        // earlier than the DRAM packet
        // COMMENT: note that nvm is given preference only if it can
        // issue earlier than DRAM
        if (col_allowed_at > nvm_col_at) {
            selected_pkt_it = nvm_pkt_it;
        }
    } else if (dram) {
        std::tie(selected_pkt_it, col_allowed_at) =
                 dram->chooseNextFRFCFS(queue, min_col_at);
    } else if (nvm) {
        std::tie(selected_pkt_it, col_allowed_at) =
                 nvm->chooseNextFRFCFS(queue, min_col_at);
    }

    if (selected_pkt_it == queue.end()) {
        DPRINTF(MemCtrl, "%s no available packets found\n", __func__);
    }

    return selected_pkt_it;
}

MemPacketQueue::iterator
MemCtrl::chooseNextDC(MemPacketQueue& queue,
                      Tick extra_col_delay, bool is_dram)
{
    // COMMENT: What policies scheduling can have
    // if DRAM cache is used
    // This method does the arbitration between requests.
    MemPacketQueue::iterator ret = queue.end();

    if (!queue.empty()) {
        if (queue.size() == 1) {
            // available rank corresponds to state refresh idle
            MemPacket* mem_pkt = *(queue.begin());
            if (packetReady(mem_pkt)) {
                ret = queue.begin();
                DPRINTF(MemCtrl, "Single request, going to a free rank\n");
            } else {
                DPRINTF(MemCtrl, "Single request, going to a busy rank\n");
            }
        } else if (memSchedPolicy == Enums::fcfs) {
            // check if there is a packet going to a free rank
            for (auto i = queue.begin(); i != queue.end(); ++i) {
                MemPacket* mem_pkt = *i;
                if (packetReady(mem_pkt)) {
                    ret = i;
                    break;
                }
            }
        } else if (memSchedPolicy == Enums::frfcfs) {
            // COMMENT: I think this frfcfs is the policy we can safely assume
            ret = chooseNextFRFCFSDC(queue, extra_col_delay, is_dram);
        } else {
            panic("No scheduling policy chosen\n");
        }
    }
    return ret;
}

MemPacketQueue::iterator
MemCtrl::chooseNextFRFCFSDC(MemPacketQueue& queue,
                            Tick extra_col_delay, bool is_dram)
{
    auto selected_pkt_it = queue.end();
    Tick col_allowed_at = MaxTick;

    // time we need to issue a column command to be seamless
    const Tick min_col_at = std::max(nextBurstAt + extra_col_delay, curTick());

    // find optimal packet for each interface
    if (is_dram) {
        std::tie(selected_pkt_it, col_allowed_at) =
                 dram->chooseNextFRFCFS(queue, min_col_at);
    } else {
        std::tie(selected_pkt_it, col_allowed_at) =
                 nvm->chooseNextFRFCFS(queue, min_col_at);
    }

    if (selected_pkt_it == queue.end()) {
        DPRINTF(MemCtrl, "%s no available packets found\n", __func__);
    }

    return selected_pkt_it;
}

void
MemCtrl::accessAndRespond(PacketPtr origRequestorPkt, Tick static_latency,
                          bool inDRAM)
{
    DPRINTF(MemCtrl, "Responding to Address %lld.. \n",
            origRequestorPkt->getAddr());

    // COMMENT: Wouldn't all packets need some kind of response?
    bool needsResponse = origRequestorPkt->needsResponse();
    // do the actual memory access which also turns the packet into a
    // response

    //COMMENT: Here the actual memory access is happening!!!
    //COMMENT: this is not clear. I thought, we were going to service
    // A read request form the write queue. Then why there is need for
    // an actual access in the device

    // COMMENT: access function of base AbstractMemory (which is
    // untimed access) and dram.nvm interfaces inherit from AbstractMemory
    // converting a req packet to a response packet (if our original request)
    // needed a response

    if (inDRAM && dram &&
                dram->getAddrRange().contains(origRequestorPkt->getAddr())) {
        dram->access(origRequestorPkt);
        //COMMENT: access is from the abstract memory. does it mean
        // device is not accessed here?
        //COMMENT: It seems like this will access the memory instantly

    } else if (!inDRAM && nvm &&
                nvm->getAddrRange().contains(origRequestorPkt->getAddr())) {

        nvm->access(origRequestorPkt);
    } else {
        panic("Can't handle address range for packet %s\n",
              origRequestorPkt->print());
    }

    // turn packet around to go back to requestor if response expected
    if (needsResponse) {
        // access already turned the packet into a response
        assert(origRequestorPkt->isResponse());
        // response_time consumes the static latency and is charged also
        // with headerDelay that takes into account the delay provided by
        // the xbar and also the payloadDelay that takes into account the
        // number of data beats.
        Tick response_time = curTick() + static_latency +
                             origRequestorPkt->headerDelay +
                             origRequestorPkt->payloadDelay;
        // Here we reset the timing of the packet before sending it out.
        origRequestorPkt->headerDelay = origRequestorPkt->payloadDelay = 0;

        // queue the packet in the response queue to be sent out after
        // the static latency has passed
        // COMMENT: this is the incoming port...
        // schedule the response on this port
        port.schedTimingResp(origRequestorPkt, response_time);
    } else {
        // @todo the packet is going to be deleted, and the MemPacket
        // is still having a pointer to it
        pendingDelete.reset(origRequestorPkt);
    }

    DPRINTF(MemCtrl, "Done\n");

    return;
}

void
MemCtrl::pruneBurstTick()
{
    auto it = burstTicks.begin();
    while (it != burstTicks.end()) {
        auto current_it = it++;
        if (curTick() > *current_it) {
            DPRINTF(MemCtrl, "Removing burstTick for %d\n", *current_it);
            burstTicks.erase(current_it);
        }
    }
}

Tick
MemCtrl::getBurstWindow(Tick cmd_tick)
{
    // get tick aligned to burst window
    Tick burst_offset = cmd_tick % commandWindow;
    return (cmd_tick - burst_offset);
}

Tick
MemCtrl::verifySingleCmd(Tick cmd_tick, Tick max_cmds_per_burst)
{
    // start with assumption that there is no contention on command bus
    Tick cmd_at = cmd_tick;

    // get tick aligned to burst window
    Tick burst_tick = getBurstWindow(cmd_tick);

    // verify that we have command bandwidth to issue the command
    // if not, iterate over next window(s) until slot found
    while (burstTicks.count(burst_tick) >= max_cmds_per_burst) {
        DPRINTF(MemCtrl, "Contention found on command bus at %d\n",
                burst_tick);
        burst_tick += commandWindow;
        cmd_at = burst_tick;
    }

    // add command into burst window and return corresponding Tick
    burstTicks.insert(burst_tick);
    return cmd_at;
}

Tick
MemCtrl::verifyMultiCmd(Tick cmd_tick, Tick max_cmds_per_burst,
                         Tick max_multi_cmd_split)
{
    // start with assumption that there is no contention on command bus
    Tick cmd_at = cmd_tick;

    // get tick aligned to burst window
    Tick burst_tick = getBurstWindow(cmd_tick);

    // Command timing requirements are from 2nd command
    // Start with assumption that 2nd command will issue at cmd_at and
    // find prior slot for 1st command to issue
    // Given a maximum latency of max_multi_cmd_split between the commands,
    // find the burst at the maximum latency prior to cmd_at
    Tick burst_offset = 0;
    Tick first_cmd_offset = cmd_tick % commandWindow;
    while (max_multi_cmd_split > (first_cmd_offset + burst_offset)) {
        burst_offset += commandWindow;
    }
    // get the earliest burst aligned address for first command
    // ensure that the time does not go negative
    Tick first_cmd_tick = burst_tick - std::min(burst_offset, burst_tick);

    // Can required commands issue?
    bool first_can_issue = false;
    bool second_can_issue = false;
    // verify that we have command bandwidth to issue the command(s)
    while (!first_can_issue || !second_can_issue) {
        bool same_burst = (burst_tick == first_cmd_tick);
        auto first_cmd_count = burstTicks.count(first_cmd_tick);
        auto second_cmd_count = same_burst ? first_cmd_count + 1 :
                                   burstTicks.count(burst_tick);

        first_can_issue = first_cmd_count < max_cmds_per_burst;
        second_can_issue = second_cmd_count < max_cmds_per_burst;

        if (!second_can_issue) {
            DPRINTF(MemCtrl, "Contention (cmd2) found on command bus at %d\n",
                    burst_tick);
            burst_tick += commandWindow;
            cmd_at = burst_tick;
        }

        // Verify max_multi_cmd_split isn't violated when command 2 is shifted
        // If commands initially were issued in same burst, they are
        // now in consecutive bursts and can still issue B2B
        bool gap_violated = !same_burst &&
             ((burst_tick - first_cmd_tick) > max_multi_cmd_split);

        if (!first_can_issue || (!second_can_issue && gap_violated)) {
            DPRINTF(MemCtrl, "Contention (cmd1) found on command bus at %d\n",
                    first_cmd_tick);
            first_cmd_tick += commandWindow;
        }
    }

    // Add command to burstTicks
    burstTicks.insert(burst_tick);
    burstTicks.insert(first_cmd_tick);

    return cmd_at;
}

bool
MemCtrl::inReadBusState(bool next_state) const
{
    // check the bus state
    if (next_state) {
        // use busStateNext to get the state that will be used
        // for the next burst
        return (busStateNext == MemCtrl::READ);
    } else {
        return (busState == MemCtrl::READ);
    }
}

bool
MemCtrl::inWriteBusState(bool next_state) const
{
    // check the bus state
    if (next_state) {
        // use busStateNext to get the state that will be used
        // for the next burst
        return (busStateNext == MemCtrl::WRITE);
    } else {
        return (busState == MemCtrl::WRITE);
    }
}

void
MemCtrl::doBurstAccess(MemPacket* mem_pkt)
{
    // first clean up the burstTick set, removing old entries
    // before adding new entries for next burst
    pruneBurstTick();

    // When was command issued?
    Tick cmd_at;

    // Issue the next burst and update bus state to reflect
    // when previous command was issued
    if (mem_pkt->isDram()) {
        std::vector<MemPacketQueue>& queue = selQueue(mem_pkt->isRead());
        std::tie(cmd_at, nextBurstAt) =
                 dram->doBurstAccess(mem_pkt, nextBurstAt, queue);

        //COMMENT: The above call returns the tick of current burst issue
        // and the tick of when the next burst can be issued

        // Update timing for NVM ranks if NVM is configured on this channel
        if (nvm)
            nvm->addRankToRankDelay(cmd_at);

    } else {
        std::tie(cmd_at, nextBurstAt) =
                 nvm->doBurstAccess(mem_pkt, nextBurstAt);

        // Update timing for NVM ranks if NVM is configured on this channel
        if (dram)
            dram->addRankToRankDelay(cmd_at);

    }

    DPRINTF(MemCtrl, "Access to %lld, ready at %lld next burst at %lld.\n",
            mem_pkt->addr, mem_pkt->readyTime, nextBurstAt);

    // Update the minimum timing between the requests, this is a
    // conservative estimate of when we have to schedule the next
    // request to not introduce any unecessary bubbles. In most cases
    // we will wake up sooner than we have to.
    nextReqTime = nextBurstAt - (dram ? dram->commandOffset() :
                                        nvm->commandOffset());


    // Update the common bus stats
    if (mem_pkt->isRead()) {
        ++readsThisTime;
        // Update latency stats
        stats.requestorReadTotalLat[mem_pkt->requestorId()] +=
            mem_pkt->readyTime - mem_pkt->entryTime;
        stats.requestorReadBytes[mem_pkt->requestorId()] += mem_pkt->size;
    } else {
        ++writesThisTime;
        stats.requestorWriteBytes[mem_pkt->requestorId()] += mem_pkt->size;
        stats.requestorWriteTotalLat[mem_pkt->requestorId()] +=
            mem_pkt->readyTime - mem_pkt->entryTime;
    }
}

void
MemCtrl::processNextReqEvent()
{
    // std::cout << "In processNextReqEvent: " << curTick() << "\n";
    // printQueues(0);
    // printQueues(1);
    // printQueues(2);
    // printQueues(3);
    // printQueues(4);
    // printQueues(5);
    // std::cout << "Finished printing queues in processNextReqEvent\n\n";


    // transition is handled by QoS algorithm if enabled
    if (turnPolicy) {
        // select bus state - only done if QoS algorithms are in use
        busStateNext = selectNextBusState();
    }

    // detect bus state change
    bool switched_cmd_type = (busState != busStateNext);
    // record stats
    recordTurnaroundStats();

    DPRINTF(MemCtrl, "QoS Turnarounds selected state %s %s\n",
            (busState==MemCtrl::READ)?"READ":"WRITE",
            switched_cmd_type?"[turnaround triggered]":"");

    if (switched_cmd_type) {
        if (busState == MemCtrl::READ) {
            DPRINTF(MemCtrl,
                    "Switching to writes after %d reads with %d reads "
                    "waiting\n", readsThisTime, totalReadQueueSize);
            stats.rdPerTurnAround.sample(readsThisTime);
            readsThisTime = 0;
        } else {
            DPRINTF(MemCtrl,
                    "Switching to reads after %d writes with %d writes "
                    "waiting\n", writesThisTime, totalWriteQueueSize);
            stats.wrPerTurnAround.sample(writesThisTime);
            writesThisTime = 0;
        }
    }

    // updates current state
    busState = busStateNext;

    if (nvm) {

        for (auto queue = nvmReadQueue.rbegin();
             queue != nvmReadQueue.rend(); ++queue) {
             // select non-deterministic NVM read to issue
             // assume that we have the command bandwidth to issue this
             // along with additional RD/WR burst with needed bank
             // operations
             if (nvm->readsWaitingToIssue()) {
                 // select non-deterministic NVM read to issue
                 nvm->chooseRead(*queue);
             }
        }
    }

    // check ranks for refresh/wakeup - uses busStateNext, so done after
    // turnaround decisions
    // Default to busy status and update based on interface specifics
    bool dram_busy = dram ? dram->isBusy() : true;
    bool nvm_busy = true;
    bool all_writes_nvm = false;
    if (nvm) {
        all_writes_nvm = nvm->numWritesQueued == nvmWriteQueueSize;
        bool read_queue_empty = nvmReadQueueSize == 0;
        nvm_busy = nvm->isBusy(read_queue_empty, all_writes_nvm);
    }
    // Default state of unused interface is 'true'
    // Simply AND the busy signals to determine if system is busy
    if (dram_busy && nvm_busy) {
        // if all ranks are refreshing wait for them to finish
        // and stall this state machine without taking any further
        // action, and do not schedule a new nextReqEvent
        return;
    }

    // when we get here it is either a read or a write
    if (busState == READ) {
        // track if we should switch or not
        bool switch_to_writes = false;
        if (totalReadQueueSize == 0 && nvmReadQueueSize == 0) {
            // In the case there is no read request to go next,
            // trigger writes if we have passed the low threshold (or
            // if we are draining)
            if ((totalWriteQueueSize != 0 ||
                 nvmWriteQueueSize != 0 ||
                 dramFillQueueSize != 0) &&
                (drainState() == DrainState::Draining ||
                 (totalWriteQueueSize + nvmWriteQueueSize + dramFillQueueSize)
                 > writeLowThreshold)) {
                    DPRINTF(MemCtrl,
                            "Switching to writes due to read queue empty\n");
                    switch_to_writes = true;

            } else {
                // check if we are drained
                // not done draining until in PWR_IDLE state
                // ensuring all banks are closed and
                // have exited low power states
                if (drainState() == DrainState::Draining &&
                    respQueue.empty() && allIntfDrained()) {
                    DPRINTF(Drain, "MemCtrl controller done draining\n");
                    signalDrainDone();
                }
                // nothing to do, not even any point in scheduling an
                // event for the next request
                return;
            }

        } else {
            // COMMENT: we have something in read queue
            bool read_found = false;
            bool nvm_read_found = false;
            MemPacketQueue::iterator to_read;
            uint8_t prio = numPriorities();

            // First check NVM Read Queue
            for (auto queue = nvmReadQueue.rbegin();
                 queue != nvmReadQueue.rend(); ++queue) {
                if (!queue->size()) {
                    break;
                }
                prio--;

                DPRINTF(QOS,
                        "Checking NVM READ queue [%d]"
                        "priority [%d elements]\n",
                        prio, queue->size());

                // Figure out which nvm read request goes next
                // If we are changing command type, incorporate the minimum
                // bus turnaround delay which will be rank to rank delay
                to_read = chooseNextDC((*queue), switched_cmd_type ?
                minWriteToReadDataGap() : 0, false);
                if (to_read != queue->end()) {
                    // candidate read found
                    nvm_read_found = true;
                    break;
                }
            }
            // If we did not find a read mem packet in
            // the nvm read queue, go to the read Queue
            if (!nvm_read_found) {
                for (auto queue = readQueue.rbegin();
                 queue != readQueue.rend(); ++queue) {
                    if (!queue->size()) {
                        break;
                    }
                    prio--;

                    DPRINTF(QOS,
                            "Checking READ queue [%d] "
                            "priority [%d elements]\n",
                            prio, queue->size());

                    // Figure out which read request goes next
                    // If we are changing command type, incorporate the minimum
                    // bus turnaround delay which will be rank to rank delay
                    to_read = chooseNextDC((*queue), switched_cmd_type ?
                    minWriteToReadDataGap() : 0, true);

                    if (to_read != queue->end()) {
                        // candidate read found
                        read_found = true;
                        break;
                    }
                }
            }
            // if no read to an available rank is found then return
            // at this point. There could be writes to the available ranks
            // which are above the required threshold. However, to
            // avoid adding more complexity to the code, return and wait
            // for a refresh event to kick things into action again.
            if (!read_found && !nvm_read_found) {
                DPRINTF(MemCtrl, "No Reads Found - exiting\n");
                return;
            }

            auto mem_pkt = *to_read;

            doBurstAccess(mem_pkt);

            // sanity check
            assert(mem_pkt->size <= (mem_pkt->isDram() ?
                                      dram->bytesPerBurst() :
                                      nvm->bytesPerBurst()) );

            assert(mem_pkt->readyTime >= curTick());

            // log the response
            if (read_found) {
                logResponse(MemCtrl::READ, mem_pkt->requestorId(),
                        mem_pkt->qosValue(), mem_pkt->getAddr(), 1,
                        mem_pkt->readyTime - mem_pkt->entryTime, 0);
            }
            else if (nvm_read_found) {
                logResponse(MemCtrl::READ, mem_pkt->requestorId(),
                        mem_pkt->qosValue(), mem_pkt->getAddr(), 1,
                        mem_pkt->readyTime - mem_pkt->entryTime, 2);
            }

            // Insert into response queue. It will be sent back to the
            // requestor at its readyTime
            if (respQueue.empty()) {
                assert(!respondEvent.scheduled());
                schedule(respondEvent, mem_pkt->readyTime);
            } else {
                assert(respQueue.top().second->readyTime
                        <= mem_pkt->readyTime);
                if (!respondEvent.scheduled()){
                    schedule(respondEvent,
                    std::max(respQueue.top().second->readyTime, curTick()));
                }
            }
            respQueue.push(std::make_pair(mem_pkt->readyTime, mem_pkt));

            // we have so many writes that we have to transition
            // don't transition if the writeRespQueue is full and
            // there are no other writes that can issue
            if ((totalWriteQueueSize + nvmWriteQueueSize + dramFillQueueSize
                > writeHighThreshold) &&
               !(nvm && all_writes_nvm && nvm->writeRespQueueFull())) {
                switch_to_writes = true;
            }

            // we can probably give priority to dramfill queueu
            // and check if its size is above a threshold and
            // switch to writes to move contents out of dramfill
            // making sure that we have enough space in DRAM fill queue
            // (i.e. we are below the threshold)

            // And then we can move to main write queue, by changing
            // the bus state
            // to READ and moving contents out of main write queue

            // erase the packet depending on which
            // queue is it taken from
            if (nvm_read_found) {
                nvmReadQueue[mem_pkt->qosValue()].erase(to_read);
                nvmReadQueueSize--;
                if (retryNVMRdReq) {
                    // if we could not process a response because
                    // NVM READ queue was full, let's schedule it now
                    retryNVMRdReq = false;
                    if (!respondEvent.scheduled())
                        schedule(respondEvent, curTick());
                }
            }
            else if (read_found) {
                readQueue[mem_pkt->qosValue()].erase(to_read);
            }

        }

        // checking for write packets in the writeQueue which
        // needs a read first, to check tag and metadata.
        if (totalReadQueueSize == 0 && nvmReadQueueSize == 0 &&
            nvmWriteQueueSize == 0 && dramFillQueueSize == 0 &&
            totalWriteQueueSize != 0 && switch_to_writes) {
            switch_to_writes = false;
            bool write_found = false;
            MemPacketQueue::iterator to_write;
            uint8_t prio = numPriorities();

            for (auto queue = writeQueue.rbegin();
                queue != writeQueue.rend(); ++queue) {
                if (!queue->size()) {
                        break;
                }
                prio--;

                DPRINTF(QOS,
                        "Checking WRITE queue [%d]"
                        " priority [%d elements]\n",
                        prio, queue->size());

                // If we are changing command type, incorporate the minimum
                // bus turnaround delay
                // TODO: how to choose next when we have
                // DRAM cache and nvm packets
                // in the queue
                to_write = chooseNextDC((*queue),
                        switched_cmd_type ? minReadToWriteDataGap()
                        : 0, true);

                if (to_write != queue->end()) {
                    write_found = true;
                    break;
                }
            }
            // if there are no writes to a rank that is available to service
            // requests (i.e. rank is in refresh idle state) are found then
            // return. There could be reads to the available ranks. However, to
            // avoid adding more complexity to the code,
            // return at this point and
            // wait for a refresh event to kick things into action again.
            if (!write_found) {
                DPRINTF(MemCtrl, "No Writes Found in Write "
                "Request Queue - exiting\n");
                return;
            }

            auto mem_pkt = *to_write;

            doBurstAccess(mem_pkt);

            // sanity check
            assert(mem_pkt->size <= (mem_pkt->isDram() ?
                                      dram->bytesPerBurst() :
                                      nvm->bytesPerBurst()) );

            assert(mem_pkt->readyTime >= curTick());
            //COMMENT: In comparison to reads, nothing is written to
            // response queue

            isInWriteQueue.erase(burstAlign(mem_pkt->addr, mem_pkt->isDram()));

            // log the response

            logResponse(MemCtrl::WRITE, mem_pkt->requestorId(),
                        mem_pkt->qosValue(), mem_pkt->getAddr(), 1,
                        mem_pkt->readyTime - mem_pkt->entryTime, 1);


            if (respQueue.empty()) {
                assert(!respondEvent.scheduled());
                schedule(respondEvent, mem_pkt->readyTime);
            } else {
                assert(respQueue.top().second->readyTime
                        <= mem_pkt->readyTime);
                assert(respondEvent.scheduled());
            }

            respQueue.push(std::make_pair(mem_pkt->readyTime, mem_pkt));

            // we have so many writes that we have to transition
            // don't transition if the writeRespQueue is full and
            // there are no other writes that can issue
            if ((totalWriteQueueSize + nvmWriteQueueSize + dramFillQueueSize
                > writeHighThreshold) &&
               !(nvm && all_writes_nvm && nvm->writeRespQueueFull())) {
                switch_to_writes = true;
            }

            // We should update the dirty bits right before deleting
            // the packet.
            // int index = bits(mem_pkt->getAddr(),
            // ceilLog2(64)+ceilLog2(numEntries), ceilLog2(64));
            // // setting the dirty bit here
            // // FIX NEEDED: what if the entry's already occupied and dirty?
            // tagStoreDC[index].dirty_line = true;

            // remove the request from the queue
            // - the iterator is no longer valid
            writeQueue[mem_pkt->qosValue()].erase(to_write);

            // We should not delete mem_paket in case of write,
            // because we still need it as we are doing a read
            // access, untill tag and metadata check is finished.
            // delete mem_pkt;

            // If we emptied the write queue, or got sufficiently below the
            // threshold (using the minWritesPerSwitch as the hysteresis) and
            // are not draining, or we have reads waiting and have done enough
            // writes, then switch to reads.
            // If we are interfacing to NVM and have filled the writeRespQueue,
            // with only NVM writes in Q, then switch to reads
            bool below_threshold =
                totalWriteQueueSize + minWritesPerSwitch < writeLowThreshold;

            if (totalWriteQueueSize == 0 ||
                (below_threshold && drainState() != DrainState::Draining) ||
                (totalReadQueueSize && writesThisTime >= minWritesPerSwitch) ||
                (totalReadQueueSize && nvm && nvm->writeRespQueueFull() &&
                all_writes_nvm)) {
                // turn the bus back around for reads again
                busStateNext = MemCtrl::READ;

                // note that the we switch back to reads also in the idle
                // case, which eventually will check for any draining and
                // also pause any further scheduling if there is really
                // nothing to do
            }
        }
        // switching to writes, either because the read queue is empty
        // and the writes have passed the low threshold (or we are
        // draining), or because the writes hit the hight threshold
        if (switch_to_writes) {
            // transition to writing
            busStateNext = WRITE;
        }
    } // end of busState == READ
    else { // write
        bool nvm_write_found = false;
        bool dfill_write_found = false;
        MemPacketQueue::iterator to_write;
        uint8_t prio = numPriorities();

        // First check NVM Write Queue

        for (auto queue = dramFillQueue.rbegin();
            queue != dramFillQueue.rend(); ++queue) {
            if (!queue->size()){
                break;
            }
            prio--;

            DPRINTF(QOS,
                    "Checking dram fill queue [%d] priority [%d elements]\n",
                    prio, queue->size());

            // If we are changing command type, incorporate the minimum
            // bus turnaround delay
            to_write = chooseNextDC((*queue),
                     switched_cmd_type ? minReadToWriteDataGap() : 0, true);

            if (to_write != queue->end()) {
                dfill_write_found = true;
                break;
            }
        }

        if (!dfill_write_found) {
            // next check in nvm write queue
            for (auto queue = nvmWriteQueue.rbegin();
                queue != nvmWriteQueue.rend(); ++queue) {
                if (!queue->size()){
                    break;
                }
                prio--;

                DPRINTF(QOS,
                        "Checking nvm write queue [%d] "
                        "priority [%d elements]\n",
                        prio, queue->size());

                // If we are changing command type, incorporate the minimum
                // bus turnaround delay
                to_write = chooseNextDC((*queue),
                        switched_cmd_type ? minReadToWriteDataGap()
                        : 0, false);

                if (to_write != queue->end()) {
                    nvm_write_found = true;
                    break;
                }
            }
        }

        // if there are no writes to a rank that is available to service
        // requests (i.e. rank is in refresh idle state) are found then
        // return. There could be reads to the available ranks. However, to
        // avoid adding more complexity to the code, return at this point and
        // wait for a refresh event to kick things into action again.
        if (!dfill_write_found && !nvm_write_found) {
            DPRINTF(MemCtrl, "No writes found in nvm and "
            "dfill queues- exiting\n");
            busStateNext = MemCtrl::READ;
            return;
        }

        auto mem_pkt = *to_write;

        if (dfill_write_found && mem_pkt->is_waiting_for_nvm_read) {
            busStateNext = MemCtrl::READ;
            // if (!nextReqEvent.scheduled()) {
            //     schedule(nextReqEvent, std::max(nextReqTime, curTick()));
            // }
            return;
        }

        // sanity check
        assert(mem_pkt->size <= (mem_pkt->isDram() ?
                                  dram->bytesPerBurst() :
                                  nvm->bytesPerBurst()) );
        doBurstAccess(mem_pkt);

        // log the response
        if (nvm_write_found) {
            logResponse(MemCtrl::WRITE, mem_pkt->requestorId(),
                    mem_pkt->qosValue(), mem_pkt->getAddr(), 1,
                    mem_pkt->readyTime - mem_pkt->entryTime, 3);
        }
        else if (dfill_write_found) {
            logResponse(MemCtrl::WRITE, mem_pkt->requestorId(),
                    mem_pkt->qosValue(), mem_pkt->getAddr(), 1,
                    mem_pkt->readyTime - mem_pkt->entryTime, 4);
        }

        // remove the request from the queue - the iterator is no longer valid
        if (dfill_write_found) {
            dramFillQueue[mem_pkt->qosValue()].erase(to_write);
            dramFillQueueSize--;
            if (retryDRAMFillReq) {
                // retry processing respond event if we
                // could not do it before becasue dram fill
                // queue was full
                retryDRAMFillReq = false;
                if (!respondEvent.scheduled())
                    schedule(respondEvent, curTick());
            }
        }
        else if (nvm_write_found) {
            nvmWriteQueue[mem_pkt->qosValue()].erase(to_write);
            nvmWriteQueueSize--;
             if (retryNVMWrReq) {
                // retry processing respond event if we could
                // not do it before becasue NVMWriteQueue was full
                retryNVMWrReq = false;
                if (!respondEvent.scheduled())
                    schedule(respondEvent, curTick());
            }
        }

        delete mem_pkt;

        // If we emptied the write queue, or got sufficiently below the
        // threshold (using the minWritesPerSwitch as the hysteresis) and
        // are not draining, or we have reads waiting and have done enough
        // writes, then switch to reads.
        // If we are interfacing to NVM and have filled the writeRespQueue,
        // with only NVM writes in Q, then switch to reads
        bool below_threshold =
            totalWriteQueueSize + minWritesPerSwitch
            + nvmWriteQueueSize + dramFillQueueSize < writeLowThreshold;

        if ((totalWriteQueueSize + nvmWriteQueueSize
        + dramFillQueueSize == 0) ||
            (below_threshold && drainState() != DrainState::Draining) ||
            (totalReadQueueSize && writesThisTime >= minWritesPerSwitch) ||
            (totalReadQueueSize && nvm && nvm->writeRespQueueFull() &&
             all_writes_nvm)) {
            // turn the bus back around for reads again
            busStateNext = MemCtrl::READ;

            // note that the we switch back to reads also in the idle
            // case, which eventually will check for any draining and
            // also pause any further scheduling if there is really
            // nothing to do
        }
    }
    // COMMENT: Not sure what this comment means
    // It is possible that a refresh to another rank kicks things back into
    // action before reaching this point.
    if (!nextReqEvent.scheduled()) {
        schedule(nextReqEvent, std::max(nextReqTime, curTick()));
    }

    // If there is space available and we have writes waiting then let
    // them retry. This is done here to ensure that the retry does not
    // cause a nextReqEvent to be scheduled before we do so as part of
    // the next request processing

    // FIX: This might be better to be moved in the WriteQueue
    // processing section
    if (retryWrReq && (totalWriteQueueSize + nvmWriteQueueSize
    + dramFillQueueSize) < writeBufferSize) {
        retryWrReq = false;
        port.sendRetryReq();
    }
    // std::cout << "_____________________________________________________\n";
}

bool
MemCtrl::packetReady(MemPacket* pkt)
{
    return (pkt->isDram() ?
        dram->burstReady(pkt) : nvm->burstReady(pkt));
}

Tick
MemCtrl::minReadToWriteDataGap()
{
    Tick dram_min = dram ?  dram->minReadToWriteDataGap() : MaxTick;
    Tick nvm_min = nvm ?  nvm->minReadToWriteDataGap() : MaxTick;
    return std::min(dram_min, nvm_min);
}

Tick
MemCtrl::minWriteToReadDataGap()
{
    Tick dram_min = dram ? dram->minWriteToReadDataGap() : MaxTick;
    Tick nvm_min = nvm ?  nvm->minWriteToReadDataGap() : MaxTick;
    return std::min(dram_min, nvm_min);
}

Addr
MemCtrl::burstAlign(Addr addr, bool is_dram) const
{
    if (is_dram)
        return (addr & ~(Addr(dram->bytesPerBurst() - 1)));
    else
        return (addr & ~(Addr(nvm->bytesPerBurst() - 1)));
}

MemCtrl::CtrlStats::CtrlStats(MemCtrl &_ctrl)
    : Stats::Group(&_ctrl),
    ctrl(_ctrl),

    ADD_STAT(dramCacheHit, UNIT_COUNT, "Number of hits in DRAM Cache"),
    ADD_STAT(dramCacheMiss, UNIT_COUNT, "Number of misses in DRAM Cache"),
    ADD_STAT(dramCacheRdFw, UNIT_COUNT, "Number of misses in DRAM Cache"),
    ADD_STAT(readReqs, UNIT_COUNT, "Number of read requests accepted"),
    ADD_STAT(writeReqs, UNIT_COUNT, "Number of write requests accepted"),

    ADD_STAT(readBursts, UNIT_COUNT,
             "Number of controller read bursts, including those serviced by "
             "the write queue"),
    ADD_STAT(writeBursts, UNIT_COUNT,
             "Number of controller write bursts, including those merged in "
             "the write queue"),
    ADD_STAT(servicedByWrQ, UNIT_COUNT,
             "Number of controller read bursts serviced by the write queue"),
    ADD_STAT(mergedWrBursts, UNIT_COUNT,
             "Number of controller write bursts merged with an existing one"),

    ADD_STAT(neitherReadNorWriteReqs, UNIT_COUNT,
             "Number of requests that are neither read nor write"),

    ADD_STAT(avgRdQLen,
             UNIT_RATE(Stats::Units::Count, Stats::Units::Tick),
             "Average read queue length when enqueuing"),
    ADD_STAT(avgWrQLen,
             UNIT_RATE(Stats::Units::Count, Stats::Units::Tick),
             "Average write queue length when enqueuing"),

    ADD_STAT(numRdRetry, UNIT_COUNT,
             "Number of times read queue was full causing retry"),
    ADD_STAT(numWrRetry, UNIT_COUNT,
             "Number of times write queue was full causing retry"),

    ADD_STAT(readPktSize, UNIT_COUNT, "Read request sizes (log2)"),
    ADD_STAT(writePktSize, UNIT_COUNT, "Write request sizes (log2)"),

    ADD_STAT(rdQLenPdf, UNIT_COUNT,
             "What read queue length does an incoming req see"),
    ADD_STAT(wrQLenPdf, UNIT_COUNT,
             "What write queue length does an incoming req see"),

    ADD_STAT(rdPerTurnAround, UNIT_COUNT,
             "Reads before turning the bus around for writes"),
    ADD_STAT(wrPerTurnAround, UNIT_COUNT,
             "Writes before turning the bus around for reads"),

    ADD_STAT(bytesReadWrQ, UNIT_BYTE,
             "Total number of bytes read from write queue"),
    ADD_STAT(bytesReadSys, UNIT_BYTE,
             "Total read bytes from the system interface side"),
    ADD_STAT(bytesWrittenSys, UNIT_BYTE,
             "Total written bytes from the system interface side"),

    ADD_STAT(avgRdBWSys, UNIT_RATE(Stats::Units::Byte, Stats::Units::Second),
             "Average system read bandwidth in Byte/s"),
    ADD_STAT(avgWrBWSys, UNIT_RATE(Stats::Units::Byte, Stats::Units::Second),
             "Average system write bandwidth in Byte/s"),

    ADD_STAT(totGap, UNIT_TICK, "Total gap between requests"),
    ADD_STAT(avgGap, UNIT_RATE(Stats::Units::Tick, Stats::Units::Count),
             "Average gap between requests"),

    ADD_STAT(requestorReadBytes, UNIT_BYTE,
             "Per-requestor bytes read from memory"),
    ADD_STAT(requestorWriteBytes, UNIT_BYTE,
             "Per-requestor bytes write to memory"),
    ADD_STAT(requestorReadRate,
             UNIT_RATE(Stats::Units::Byte, Stats::Units::Second),
             "Per-requestor bytes read from memory rate"),
    ADD_STAT(requestorWriteRate,
             UNIT_RATE(Stats::Units::Byte, Stats::Units::Second),
             "Per-requestor bytes write to memory rate"),
    ADD_STAT(requestorReadAccesses, UNIT_COUNT,
             "Per-requestor read serviced memory accesses"),
    ADD_STAT(requestorWriteAccesses, UNIT_COUNT,
             "Per-requestor write serviced memory accesses"),
    ADD_STAT(requestorReadTotalLat, UNIT_TICK,
             "Per-requestor read total memory access latency"),
    ADD_STAT(requestorWriteTotalLat, UNIT_TICK,
             "Per-requestor write total memory access latency"),
    ADD_STAT(requestorReadAvgLat,
             UNIT_RATE(Stats::Units::Tick, Stats::Units::Count),
             "Per-requestor read average memory access latency"),
    ADD_STAT(requestorWriteAvgLat,
             UNIT_RATE(Stats::Units::Tick, Stats::Units::Count),
             "Per-requestor write average memory access latency")

{
}

void
MemCtrl::CtrlStats::regStats()
{
    using namespace Stats;

    assert(ctrl.system());
    const auto max_requestors = ctrl.system()->maxRequestors();

    avgRdQLen.precision(2);
    avgWrQLen.precision(2);

    readPktSize.init(ceilLog2(ctrl.system()->cacheLineSize()) + 1);
    writePktSize.init(ceilLog2(ctrl.system()->cacheLineSize()) + 1);

    rdQLenPdf.init(ctrl.readBufferSize);
    wrQLenPdf.init(ctrl.writeBufferSize);

    rdPerTurnAround
        .init(ctrl.readBufferSize)
        .flags(nozero);
    wrPerTurnAround
        .init(ctrl.writeBufferSize)
        .flags(nozero);

    avgRdBWSys.precision(8);
    avgWrBWSys.precision(8);
    avgGap.precision(2);

    // per-requestor bytes read and written to memory
    requestorReadBytes
        .init(max_requestors)
        .flags(nozero | nonan);

    requestorWriteBytes
        .init(max_requestors)
        .flags(nozero | nonan);

    // per-requestor bytes read and written to memory rate
    requestorReadRate
        .flags(nozero | nonan)
        .precision(12);

    requestorReadAccesses
        .init(max_requestors)
        .flags(nozero);

    requestorWriteAccesses
        .init(max_requestors)
        .flags(nozero);

    requestorReadTotalLat
        .init(max_requestors)
        .flags(nozero | nonan);

    requestorReadAvgLat
        .flags(nonan)
        .precision(2);

    requestorWriteRate
        .flags(nozero | nonan)
        .precision(12);

    requestorWriteTotalLat
        .init(max_requestors)
        .flags(nozero | nonan);

    requestorWriteAvgLat
        .flags(nonan)
        .precision(2);

    for (int i = 0; i < max_requestors; i++) {
        const std::string requestor = ctrl.system()->getRequestorName(i);
        requestorReadBytes.subname(i, requestor);
        requestorReadRate.subname(i, requestor);
        requestorWriteBytes.subname(i, requestor);
        requestorWriteRate.subname(i, requestor);
        requestorReadAccesses.subname(i, requestor);
        requestorWriteAccesses.subname(i, requestor);
        requestorReadTotalLat.subname(i, requestor);
        requestorReadAvgLat.subname(i, requestor);
        requestorWriteTotalLat.subname(i, requestor);
        requestorWriteAvgLat.subname(i, requestor);
    }

    // Formula stats
    avgRdBWSys = (bytesReadSys) / simSeconds;
    avgWrBWSys = (bytesWrittenSys) / simSeconds;

    avgGap = totGap / (readReqs + writeReqs);

    requestorReadRate = requestorReadBytes / simSeconds;
    requestorWriteRate = requestorWriteBytes / simSeconds;
    requestorReadAvgLat = requestorReadTotalLat / requestorReadAccesses;
    requestorWriteAvgLat = requestorWriteTotalLat / requestorWriteAccesses;
}

void
MemCtrl::recvFunctional(PacketPtr origRequestorPkt)
{
    if (dram && dram->getAddrRange().contains(origRequestorPkt->getAddr())) {
        // rely on the abstract memory
        dram->functionalAccess(origRequestorPkt);
    } else if (nvm &&
               nvm->getAddrRange().contains(origRequestorPkt->getAddr())) {
        // rely on the abstract memory
        nvm->functionalAccess(origRequestorPkt);
   } else {
        panic("Can't handle address range for packet %s\n",
              origRequestorPkt->print());
   }
}

Port &
MemCtrl::getPort(const std::string &if_name, PortID idx)
{
    if (if_name != "port") {
        return QoS::MemCtrl::getPort(if_name, idx);
    } else {
        return port;
    }
}

bool
MemCtrl::allIntfDrained() const
{
   // ensure dram is in power down and refresh IDLE states
   bool dram_drained = !dram || dram->allRanksDrained();
   // No outstanding NVM writes
   // All other queues verified as needed with calling logic
   bool nvm_drained = !nvm || nvm->allRanksDrained();
   return (dram_drained && nvm_drained);
}

DrainState
MemCtrl::drain()
{
    // if there is anything in any of our internal queues, keep track
    // of that as well
    if (!(!totalWriteQueueSize &&
            !totalReadQueueSize &&
            !nvmReadQueueSize &&
            !nvmWriteQueueSize &&
            !dramFillQueueSize &&
            respQueue.empty() &&
            allIntfDrained())) {

        DPRINTF(Drain, "Memory controller not drained, write: %d, read: %d,"
                " resp: %d\n", totalWriteQueueSize, totalReadQueueSize,
                respQueue.size());

        // the only queue that is not drained automatically over time
        // is the write queue, thus kick things into action if needed
        if ((!totalWriteQueueSize ||
            !nvmWriteQueueSize ||
            !dramFillQueueSize) &&
            !nextReqEvent.scheduled()) {
            schedule(nextReqEvent, curTick());
        }

        if (dram)
            dram->drainRanks();

        return DrainState::Draining;
    } else {
        return DrainState::Drained;
    }
}

void
MemCtrl::drainResume()
{
    if (!isTimingMode && system()->isTimingMode()) {
        // if we switched to timing mode, kick things into action,
        // and behave as if we restored from a checkpoint
        startup();
        dram->startup();
    } else if (isTimingMode && !system()->isTimingMode()) {
        // if we switch from timing mode, stop the refresh events to
        // not cause issues with KVM
        if (dram)
            dram->suspend();
    }

    // update the mode
    isTimingMode = system()->isTimingMode();
}

MemCtrl::MemoryPort::MemoryPort(const std::string& name, MemCtrl& _ctrl)
    : QueuedResponsePort(name, &_ctrl, queue), queue(_ctrl, *this, true),
      ctrl(_ctrl)
{ }

AddrRangeList
MemCtrl::MemoryPort::getAddrRanges() const
{
    AddrRangeList ranges;
    if (ctrl.dram) {
        DPRINTF(DRAM, "Pushing DRAM ranges to port\n");
        ranges.push_back(ctrl.dram->getAddrRange());
    }
    if (ctrl.nvm) {
        DPRINTF(NVM, "Pushing NVM ranges to port\n");
        ranges.push_back(ctrl.nvm->getAddrRange());
    }
    return ranges;
}

void
MemCtrl::MemoryPort::recvFunctional(PacketPtr origRequestorPkt)
{
    origRequestorPkt->pushLabel(ctrl.name());

    if (!queue.trySatisfyFunctional(origRequestorPkt)) {
        // Default implementation of SimpleTimingPort::recvFunctional()
        // calls recvAtomic() and throws away the latency; we can save a
        // little here by just not calculating the latency.
        ctrl.recvFunctional(origRequestorPkt);
    }

    origRequestorPkt->popLabel();
}

Tick
MemCtrl::MemoryPort::recvAtomic(PacketPtr origRequestorPkt)
{
    return ctrl.recvAtomic(origRequestorPkt);
}

Tick
MemCtrl::MemoryPort::recvAtomicBackdoor(
        PacketPtr origRequestorPkt, MemBackdoorPtr &backdoor)
{
    return ctrl.recvAtomicBackdoor(origRequestorPkt, backdoor);
}

bool
MemCtrl::MemoryPort::recvTimingReq(PacketPtr origRequestorPkt)
{
    // pass it to the memory controller
    return ctrl.recvTimingReq(origRequestorPkt);
}
