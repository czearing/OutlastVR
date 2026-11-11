#pragma once
#include "d3d9_resource.h"
#include <array>

namespace olvr {
class D3D9FrameProfile {
    struct Slot {
        Com<IDirect3DQuery9> start,end,frequency,disjoint;
        UINT frame=0;
        bool pending=false;
    };
    std::array<Slot,8> slots_;
    Slot* active_=nullptr;
    UINT cursor_=0;
public:
    UINT status=0,frame=0;
    UINT64 microseconds=0;
    explicit D3D9FrameProfile(IDirect3DDevice9& device) {
        for (auto type:{D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMPFREQ,D3DQUERYTYPE_TIMESTAMPDISJOINT}) {
            const auto result=device.CreateQuery(type,nullptr);
            if (result==D3DERR_NOTAVAILABLE) return;
            gpu_check(result,"Check nonblocking GPU timer support");
        }
        for (auto& slot:slots_) {
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMP,slot.start.put()),"Create GPU start timestamp");
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMP,slot.end.put()),"Create GPU end timestamp");
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,slot.frequency.put()),"Create GPU timer frequency");
            gpu_check(device.CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,slot.disjoint.put()),"Create GPU timer disjoint query");
        }
        status=1;
    }
    void begin(UINT serial) {
        if (!status) return;
        if (active_) throw std::logic_error("Overlapping GPU profile frames");
        auto& slot=slots_[cursor_++%slots_.size()];
        if (slot.pending) {
            UINT64 start=0,end=0,frequency=0;
            BOOL disjoint=FALSE;
            const HRESULT results[]{
                slot.start->GetData(&start,sizeof(start),0),
                slot.end->GetData(&end,sizeof(end),0),
                slot.frequency->GetData(&frequency,sizeof(frequency),0),
                slot.disjoint->GetData(&disjoint,sizeof(disjoint),0)};
            bool ready=true;
            for (const auto result:results) {
                gpu_check(result,"Read asynchronous GPU frame timing");
                ready &= result==S_OK;
            }
            if (!ready) return;
            frame=slot.frame;
            status=disjoint || !frequency || end<start?3:2;
            microseconds=status==2?UINT64(double(end-start)*1000000/double(frequency)):0;
            slot.pending=false;
        }
        gpu_check(slot.disjoint->Issue(D3DISSUE_BEGIN),"Begin GPU timer disjoint interval");
        gpu_check(slot.start->Issue(D3DISSUE_END),"Issue GPU frame start timestamp");
        slot.frame=serial;
        active_=&slot;
    }
    void end() {
        if (!active_) return;
        gpu_check(active_->end->Issue(D3DISSUE_END),"Issue GPU frame end timestamp");
        gpu_check(active_->frequency->Issue(D3DISSUE_END),"Issue GPU timer frequency");
        gpu_check(active_->disjoint->Issue(D3DISSUE_END),"End GPU timer disjoint interval");
        active_->pending=true;
        active_=nullptr;
    }
};
}
