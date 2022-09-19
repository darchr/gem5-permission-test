#include "cpu/probes/looppoint.hh"

namespace gem5
{

LoopPoint::LoopPoint(const LoopPointParams &p)
    : ProbeListenerObject(p),
    targetPC(p.target_pc),
    cpuptr(p.core),
    manager(p.lpmanager)
{
}

LoopPoint::~LoopPoint()
{}

void
LoopPoint::init()
{}

void
LoopPoint::regProbeListeners()
{
    typedef ProbeListenerArg<LoopPoint, Addr> LoopPointListener;
    listeners.push_back(new LoopPointListener(this, "RetiredInstsPC",
                                             &LoopPoint::check_pc));
}

void
LoopPoint::check_pc(const Addr& pc)
{
    for (int i =0; i<targetPC.size();i++)
    {
        if(pc == targetPC[i])
        {
            if (manager->check_count(pc))
                cpuptr->scheduleInstStop(0,1,"simpoint starting point found");
        }
    }
}

}
