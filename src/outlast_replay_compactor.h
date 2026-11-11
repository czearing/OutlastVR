#pragma once
#include "d3d9_state_restore.h"
#include "d3d9_stereo_pipeline.h"
#include <cstring>

namespace olvr {

// Lives only around one original indexed RHI call. That call still owns statistics,
// cooperative checks, software stream transitions and native cache cleanup.
class OutlastReplayCompactor {
    struct Source {
        Com<IDirect3DVertexBuffer9> buffer;
        UINT step=0,stride=0,extent=0;
    };
    std::array<Source,16> sources_;
    IDirect3DDevice9& device_;
    const std::byte* rhi_;
    std::size_t extent_;
    UINT objects_,mask_,calls_=0;
    Draw expected_;
    SubmissionResult first_{};
    bool poisoned_=false;

    template<class T> T read(std::size_t offset) const {
        if (offset>extent_ || sizeof(T)>extent_-offset)
            throw std::invalid_argument("Replay field exceeds the supplied native RHI extent");
        T value{};
        std::memcpy(&value,rhi_+offset,sizeof(value));
        return value;
    }
    static UINT element_size(BYTE type) {
        const UINT sizes[]{4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};
        if (type>=std::size(sizes))
            throw std::invalid_argument("Unsupported replay vertex element type");
        return sizes[type];
    }
    static bool same_draw(const Draw& a,const Draw& b) {
        return a.primitive_type==b.primitive_type && a.base_vertex==b.base_vertex &&
            a.min_vertex==b.min_vertex && a.vertex_count==b.vertex_count &&
            a.start_index==b.start_index && a.primitive_count==b.primitive_count;
    }
public:
    OutlastReplayCompactor(const void* rhi,std::size_t extent,IDirect3DDevice9& device,const Draw& draw)
        :device_(device),rhi_(static_cast<const std::byte*>(rhi)),extent_(extent),
         objects_(0),mask_(0),expected_(draw) {
        if (!rhi_ || read<IDirect3DDevice9*>(0x34)!=&device)
            throw std::invalid_argument("Replay RHI does not own the selected device");
        objects_=read<UINT>(0x284);mask_=read<UINT>(0x288);
        if (objects_<2 || objects_>frequency_mask/2 || !mask_ || (mask_&0xffff0001u) ||
            !draw.vertex_count || !draw.primitive_count ||
            (draw.primitive_type!=D3DPT_TRIANGLELIST &&
             draw.primitive_type!=D3DPT_TRIANGLESTRIP && draw.primitive_type!=D3DPT_LINELIST))
            throw std::invalid_argument("Native replay cannot be represented by bounded object instances");
        D3DCAPS9 caps{};
        gpu_check(device.GetDeviceCaps(&caps),"Query replay capabilities");
        if (caps.MaxStreams>16 || !caps.MaxStreams || (mask_>>caps.MaxStreams))
            throw std::invalid_argument("Replay mask exceeds native device streams");
        UINT frequency=0;
        gpu_check(device.GetStreamSourceFreq(0,&frequency),"Read replay geometry frequency");
        if (frequency!=1)
            throw std::invalid_argument("Software object replay cannot nest existing hardware instancing");
        Com<IDirect3DVertexDeclaration9> declaration;
        gpu_check(device.GetVertexDeclaration(declaration.put()),"Read replay declaration");
        if (!declaration.get()) throw std::invalid_argument("Replay requires an explicit declaration");
        UINT count=0;
        gpu_check(declaration->GetDeclaration(nullptr,&count),"Read replay declaration length");
        if (count<2 || count>MAXD3DDECLLENGTH)
            throw std::invalid_argument("Invalid replay declaration length");
        std::vector<D3DVERTEXELEMENT9> elements(count);
        gpu_check(declaration->GetDeclaration(elements.data(),&count),"Read replay declaration elements");
        if (elements.back().Stream!=0xff || elements.back().Type!=D3DDECLTYPE_UNUSED)
            throw std::invalid_argument("Unterminated replay declaration");
        for (std::size_t index=0;index+1<elements.size();++index) {
            const auto& element=elements[index];
            if (element.Stream>=caps.MaxStreams)
                throw std::invalid_argument("Replay declaration stream exceeds device limits");
            sources_[element.Stream].extent=std::max(sources_[element.Stream].extent,
                UINT(element.Offset)+element_size(element.Type));
        }
        for (UINT slot=1;slot<caps.MaxStreams;++slot) {
            if (!(mask_&(1u<<slot))) continue;
            auto& source=sources_[slot];
            const auto record=0x28c+slot*0x14;
            const auto handle=read<std::uintptr_t>(record);
            source.step=read<UINT>(record+8);source.stride=read<UINT>(record+12);
            // A software source with nonzero vertex stride varies within each
            // object, and cannot be replaced by one hardware instance record.
            if (handle<8 || !source.extent || source.step<source.extent ||
                source.stride!=0 || source.step>caps.MaxStreamStride)
                throw std::invalid_argument("Replay source has invalid handle, declaration or record stride");
            UINT offset=0,stride=0;
            gpu_check(device.GetStreamSource(slot,source.buffer.put(),&offset,&stride),"Read replay source binding");
            if (reinterpret_cast<std::uintptr_t>(source.buffer.get())!=handle-8 ||
                offset || stride!=source.stride)
                throw std::invalid_argument("Replay initial binding differs from the recovered native records");
            gpu_check(device.GetStreamSourceFreq(slot,&frequency),"Read replay object frequency");
            if (frequency!=1)
                throw std::invalid_argument("Replay object source already uses hardware instancing");
            D3DVERTEXBUFFER_DESC description{};
            gpu_check(source.buffer->GetDesc(&description),"Bound replay object buffer extent");
            if (std::uint64_t(objects_-1)*source.step+source.extent>description.Size)
                throw std::invalid_argument("Replay object records exceed their actual buffer");
        }
    }
    UINT object_count() const noexcept {return objects_;}
    UINT observed_calls() const noexcept {return calls_;}
    SubmissionResult consume(const Draw& draw,D3D9StereoPipeline& pipeline,
                             const StereoViewConstants& views,const RECT* logical_scissor=nullptr) {
        if (poisoned_ || calls_>=objects_ || !same_draw(draw,expected_) ||
            read<UINT>(0x284)!=objects_ || read<UINT>(0x288)!=mask_)
            throw std::invalid_argument("Draw is not the next call in the owned native replay");
        for (UINT slot=1;slot<16;++slot) {
            if (!(mask_&(1u<<slot))) continue;
            Com<IDirect3DVertexBuffer9> current;
            UINT offset=0,stride=0;
            gpu_check(device_.GetStreamSource(slot,current.put(),&offset,&stride),"Validate native replay transition");
            const auto& source=sources_[slot];
            if (current.get()!=source.buffer.get() ||
                offset!=std::uint64_t(calls_)*source.step || stride!=source.stride)
                throw std::invalid_argument("Native replay source transition differs from the compacted records");
        }
        if (!calls_) {
            StateRestore restore(device_);
            gpu_check(device_.SetStreamSourceFreq(0,D3DSTREAMSOURCE_INDEXEDDATA|objects_),
                "Set compacted native object count");
            for (UINT slot=1;slot<16;++slot) {
                if (!(mask_&(1u<<slot))) continue;
                auto& source=sources_[slot];
                gpu_check(device_.SetStreamSource(slot,source.buffer.get(),0,source.step),
                    "Bind compacted native object records");
                gpu_check(device_.SetStreamSourceFreq(slot,D3DSTREAMSOURCE_INSTANCEDATA|1),
                    "Set compacted native object divisor");
            }
            poisoned_=true;
            first_=pipeline.draw_indexed(draw,views,1,logical_scissor);
            const auto status=restore.apply();
            if (FAILED(status) && first_.restoration_status>=0) first_.restoration_status=status;
            poisoned_=!first_.succeeded();
        }
        ++calls_;
        return first_;
    }
    void finish(bool cooperative_guard_returned=false) const {
        if (poisoned_ || (cooperative_guard_returned?calls_!=0:calls_!=objects_))
            throw std::runtime_error("Native replay did not complete its exact compacted draw sequence");
        if (cooperative_guard_returned) return;
        if (read<UINT>(0x284)!=1 || read<UINT>(0x288)!=0)
            throw std::runtime_error("Original native replay did not reset its instance count and mask");
        for (UINT slot=1;slot<16;++slot)
            if ((mask_&(1u<<slot)) && read<std::uintptr_t>(0x28c+slot*0x14))
                throw std::runtime_error("Original native replay did not clear its cached source handles");
    }
};

}
