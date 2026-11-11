#pragma once
#include "d3d9_on12_factory.h"
#include "isolated_vtable_entry.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace olvr {

class IsolatedD3D12DrawCounter {
    using Draw = void (WINAPI*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
    using Indexed = void (WINAPI*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
    using Reset = HRESULT (WINAPI*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    using Execute = void (WINAPI*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    Com<ID3D12CommandAllocator> allocator_;
    Com<ID3D12GraphicsCommandList> example_;
    IsolatedVtableEntry plain_, indexed_, reset_, execute_;
    Draw original_draw_ = nullptr;
    Indexed original_indexed_ = nullptr;
    Reset original_reset_ = nullptr;
    Execute original_execute_ = nullptr;
    ID3D12CommandQueue* captured_queue_ = nullptr;
    static inline std::atomic<IsolatedD3D12DrawCounter*> active_{nullptr};
    bool capture_ = false;
    std::mutex events_mutex_;
public:
    struct Event {bool indexed; UINT elements, instances;};
private:
    std::vector<Event> events_;
    std::unordered_map<void*,std::vector<Event>> pending_;
    void record(void* list, bool indexed, UINT elements, UINT count) {
        if (!capture_) return;
        std::lock_guard<std::mutex> lock(events_mutex_);
        auto& recording=pending_[list];
        if (recording.size() >= 65536)
            throw std::runtime_error("Native driver command-list capture exceeded its bound");
        recording.push_back({indexed,elements,count});
    }
    static HRESULT WINAPI reset_list(ID3D12GraphicsCommandList* list,
        ID3D12CommandAllocator* allocator, ID3D12PipelineState* state) {
        auto& counter=*active_.load();
        const auto result=counter.original_reset_(list,allocator,state);
        if (SUCCEEDED(result)) {
            std::lock_guard<std::mutex> lock(counter.events_mutex_);
            counter.pending_.erase(list);
        }
        return result;
    }
    static void WINAPI execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
        auto& counter=*active_.load();
        if (queue!=counter.captured_queue_) {
            counter.original_execute_(queue,count,lists);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(counter.events_mutex_);
            for (UINT i=0;i<count;++i) {
                const auto found=counter.pending_.find(lists[i]);
                if (found==counter.pending_.end()) continue;
                if (found->second.size()>65536-counter.events_.size())
                    throw std::runtime_error("Native executed draw capture exceeded its frame bound");
                counter.events_.insert(counter.events_.end(),found->second.begin(),found->second.end());
            }
        }
        counter.original_execute_(queue,count,lists);
    }
    static void WINAPI draw(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances,
                            UINT start_vertex, UINT start_instance) {
        auto& counter = *active_.load();
        ++counter.plain_calls;
        counter.instances.fetch_add(instances);
        counter.record(list,false,vertices,instances);
        counter.original_draw_(list, vertices, instances, start_vertex, start_instance);
    }
    static void WINAPI indexed(ID3D12GraphicsCommandList* list, UINT indices, UINT instances,
                               UINT start_index, INT base_vertex, UINT start_instance) {
        auto& counter = *active_.load();
        ++counter.indexed_calls;
        counter.instances.fetch_add(instances);
        counter.record(list,true,indices,instances);
        counter.original_indexed_(list, indices, instances, start_index, base_vertex, start_instance);
    }
public:
    std::atomic<UINT> plain_calls{0}, indexed_calls{0}, instances{0};
    explicit IsolatedD3D12DrawCounter(ID3D12Device& device, bool capture = false,
                                     ID3D12CommandQueue* queue = nullptr) : capture_(capture) {
        if (active_)
            throw std::logic_error("Only one isolated D3D12 counter may be active");
        if (capture && !queue)
            throw std::invalid_argument("Executed native draw capture requires the actual submission queue");
        gpu_check(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                __uuidof(ID3D12CommandAllocator),
                                                reinterpret_cast<void**>(allocator_.put())),
                  "Create native-counter allocator");
        gpu_check(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.get(), nullptr,
                                           __uuidof(ID3D12GraphicsCommandList),
                                           reinterpret_cast<void**>(example_.put())),
                  "Resolve native graphics command-list entries");
        gpu_check(example_->Close(), "Close unsubmitted counter reference list");
        void** table = nullptr;
        std::memcpy(&table, example_.get(), sizeof(table));
        original_draw_ = reinterpret_cast<Draw>(table[12]);
        original_indexed_ = reinterpret_cast<Indexed>(table[13]);
        active_ = this;
        try {
            plain_.install(&table[12], reinterpret_cast<void*>(&draw));
            indexed_.install(&table[13], reinterpret_cast<void*>(&indexed));
            if (capture) {
                captured_queue_=queue;
                original_reset_=reinterpret_cast<Reset>(table[10]);
                reset_.install(&table[10],reinterpret_cast<void*>(&reset_list));
                void** queue_table=nullptr;
                std::memcpy(&queue_table,queue,sizeof(queue_table));
                original_execute_=reinterpret_cast<Execute>(queue_table[10]);
                execute_.install(&queue_table[10],reinterpret_cast<void*>(&execute));
            }
        } catch (...) {
            execute_.detach();
            reset_.detach();
            indexed_.detach();
            plain_.detach();
            active_ = nullptr;
            throw;
        }
    }
    ~IsolatedD3D12DrawCounter() {
        execute_.detach();
        reset_.detach();
        indexed_.detach();
        plain_.detach();
        active_ = nullptr;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(events_mutex_);
        plain_calls = 0;
        indexed_calls = 0;
        instances = 0;
        events_.clear();
    }
    std::vector<Event> events() {
        std::lock_guard<std::mutex> lock(events_mutex_);
        return events_;
    }
};

} // namespace olvr
