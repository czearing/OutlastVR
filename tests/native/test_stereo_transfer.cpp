#include "isolated_d3d9.h"
#include "d3d9_on12_transfer.h"
#include <iostream>
#include <cstring>
#include <atomic>
#include <future>
#include <d3dcompiler.h>
#include <cmath>
#include <algorithm>

using namespace olvr;

DWORD color_ramp(unsigned eye,unsigned x) {
    const auto value=eye?255-x:x;
    return 0xff000000 | (value<<16) | (((value*17)&255)<<8) | ((value*37)&255);
}

void verify_display_encoding(ID3D12Device& device,ID3D12CommandQueue& queue,ID3D12Resource& eyes,
                             ID3D12Resource& readback,ID3D12CommandAllocator& allocator,
                             ID3D12GraphicsCommandList& commands,ID3D12Fence& fence,
                             DXGI_FORMAT format) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors=1;heap_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Com<ID3D12DescriptorHeap> heap;
    gpu_check(device.CreateDescriptorHeap(&heap_desc,__uuidof(ID3D12DescriptorHeap),
        reinterpret_cast<void**>(heap.put())),"Create compositor color-test descriptor heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels=1;view.Texture2DArray.ArraySize=2;
    device.CreateShaderResourceView(&eyes,&view,heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={1,&range};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters=2;root_desc.pParameters=parameters;
    Com<ID3DBlob> signature,errors,shader;
    gpu_check(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,
        signature.put(),errors.put()),"Serialize compositor color-test root");
    Com<ID3D12RootSignature> root;
    gpu_check(device.CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),
        __uuidof(ID3D12RootSignature),reinterpret_cast<void**>(root.put())),"Create compositor color-test root");
    const char code[]=
        "Texture2DArray<float4> image:register(t0); RWStructuredBuffer<float4> result:register(u0);"
        "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID)"
        "{result[id.x]=image.Load(int4(id.x%256,0,id.x/256,0));}";
    gpu_check(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,
        shader.put(),errors.put()),"Compile independent compositor sRGB sampling test");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature=root.get();
    pipeline_desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    Com<ID3D12PipelineState> pipeline;
    gpu_check(device.CreateComputePipelineState(&pipeline_desc,__uuidof(ID3D12PipelineState),
        reinterpret_cast<void**>(pipeline.put())),"Create compositor sampling pipeline");
    D3D12_RESOURCE_DESC buffer{};
    constexpr std::size_t output_bytes=512*4*sizeof(float);
    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=output_bytes;
    buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_DEFAULT;
    Com<ID3D12Resource> output;
    gpu_check(device.CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),
        reinterpret_cast<void**>(output.put())),"Create linear-light sampling output");
    gpu_check(allocator.Reset(),"Reset compositor test allocator");
    gpu_check(commands.Reset(&allocator,pipeline.get()),"Reset compositor test list");
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={&eyes,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    commands.ResourceBarrier(1,&barrier);
    commands.SetComputeRootSignature(root.get());
    ID3D12DescriptorHeap* heaps[]{heap.get()};commands.SetDescriptorHeaps(1,heaps);
    commands.SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    commands.SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
    commands.Dispatch(512,1,1);
    barrier.Transition={output.get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    commands.ResourceBarrier(1,&barrier);
    commands.CopyBufferRegion(&readback,0,output.get(),0,output_bytes);
    barrier.Transition={&eyes,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET};
    commands.ResourceBarrier(1,&barrier);
    gpu_check(commands.Close(),"Close compositor sampling commands");
    ID3D12CommandList* lists[]{&commands};queue.ExecuteCommandLists(1,lists);
    gpu_check(queue.Signal(&fence,17),"Signal compositor sampling completion");
    const auto deadline=GetTickCount64()+10000;
    while (fence.GetCompletedValue()<17) {
        if (GetTickCount64()>=deadline) throw std::runtime_error("Compositor color sampling timed out");
        Sleep(1);
    }
    if (fence.GetCompletedValue()==UINT64_MAX) throw std::runtime_error("Compositor sampling device removed");
    float* values=nullptr;
    D3D12_RANGE read{0,output_bytes};
    gpu_check(readback.Map(0,&read,reinterpret_cast<void**>(&values)),"Read actual linear-light color samples");
    std::array<float,512*4> measured{};std::copy_n(values,measured.size(),measured.begin());
    D3D12_RANGE written{0,0};readback.Unmap(0,&written);
    double max_code_error=0;
    for (unsigned eye=0;eye<2;++eye)
        for (unsigned x=0;x<256;++x)
            for (unsigned channel=0;channel<4;++channel) {
                const auto byte=(color_ramp(eye,x)>>(channel==3?24:(2-channel)*8))&255;
                const double actual=measured[(eye*256+x)*4+channel];
                const double encoded=channel==3?actual:
                    actual<=.0031308?actual*12.92:1.055*std::pow(actual,1/2.4)-.055;
                const double error=std::abs(encoded*255-byte);
                max_code_error=std::max(max_code_error,error);
                // Every channel must round-trip to its exact original native
                // byte. Hardware lookup approximation cannot change a level.
                if (!std::isfinite(actual) || actual<0 || actual>1 || error>.5 ||
                    (byte==0 && actual!=0) || (byte==255 && actual!=1))
                    throw std::runtime_error("Actual compositor sRGB decode disagrees with independent linear-light oracle; eye="+
                        std::to_string(eye)+"; channel="+std::to_string(channel)+"; byte="+std::to_string(byte)+
                        "; actual="+std::to_string(actual)+"; roundtrip_code_error="+std::to_string(error));
            }
    if (measured[0]!=0 || !(measured[16*4]>0 && measured[16*4]<.006f))
        throw std::runtime_error("Black or near-black native detail was lifted or crushed");
    std::cout<<"PASS: actual_gpu_srgb_decode; black_zero; shadow16_linear="<<measured[16*4]
        <<"; white_one; native_bytes_preserved; no_gamma_shader; srgb_levels=256; both_eyes; max_code_error="
        <<max_code_error<<'\n';
}

int main(int argc, char** argv) {
    try {
        const bool swapped = argc == 2 && std::string(argv[1]) == "--fault-swapped";
        const bool typeless = argc == 2 && std::string(argv[1]) == "--typeless";
        const bool linear_color = argc == 2 && std::string(argv[1]) == "--fault-linear-color";
        const bool srgb = argc == 2 && std::string(argv[1]) == "--srgb";
        const bool color_test=srgb || linear_color;
        const bool serial_wait = argc == 2 && std::string(argv[1]) == "--fault-flight-wait";
        const bool queued_drain = argc == 2 && std::string(argv[1]) == "--queued-drain";
        const bool queued = serial_wait || queued_drain || (argc == 2 && std::string(argv[1]) == "--queued-contexts");
        if (argc > 2 || (argc == 2 && !swapped && !typeless && !queued && !color_test))
            throw std::invalid_argument("Usage: test_stereo_transfer.exe [--fault-swapped|--typeless|--queued-contexts|--queued-drain|--fault-flight-wait]");
        D3D9On12Factory owner;
        HiddenWindow window(512, 256);
        D3DPRESENT_PARAMETERS presentation{};
        presentation.Windowed = TRUE;
        presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
        presentation.BackBufferWidth = 512;
        presentation.BackBufferHeight = 256;
        presentation.BackBufferFormat = D3DFMT_A8R8G8B8;
        presentation.hDeviceWindow = window.get();
        Com<IDirect3DDevice9> legacy;
        gpu_check(owner.get()->CreateDevice(owner.adapter_ordinal(), D3DDEVTYPE_HAL, window.get(),
                                            D3DCREATE_HARDWARE_VERTEXPROCESSING, &presentation,
                                            legacy.put()), "Create isolated classic translation device");
        Com<IDirect3DTexture9> atlas;
        gpu_check(legacy->CreateTexture(512, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, atlas.put(), nullptr), "Create test atlas");
        Com<IDirect3DSurface9> surface;
        gpu_check(atlas->GetSurfaceLevel(0, surface.put()), "Get test atlas surface");
        gpu_check(legacy->SetRenderTarget(0, surface.get()), "Bind test atlas");
        auto& device = *owner.device();
        auto& queue = *owner.queue();
        D3D12_RESOURCE_DESC eye_description{};
        eye_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        eye_description.Width = eye_description.Height = 256;
        eye_description.DepthOrArraySize = queued?6:2;
        eye_description.MipLevels = 1;
        eye_description.Format = typeless?DXGI_FORMAT_B8G8R8A8_TYPELESS:
            srgb?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM;
        eye_description.SampleDesc.Count = 1;
        eye_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Com<ID3D12Resource> eye_array;
        gpu_check(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &eye_description,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                 __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(eye_array.put())),
                  "Create persistent two-layer eye image");
        D3D9On12StereoTransfer transport(*legacy.get(), device, queue);
        std::array<D3D12EyeTarget, 2> targets{{{eye_array.get(), 0}, {eye_array.get(), 1}}};
        UINT refusals = 0;
        for (UINT fault = 0; fault < 4; ++fault) {
            auto invalid = targets;
            if (fault == 0) invalid[1].subresource = 0;
            if (fault == 1) invalid[0].subresource = eye_description.DepthOrArraySize;
            if (fault == 2) invalid[0].resource = nullptr;
            if (fault == 3) invalid[0].acquired_state = D3D12_RESOURCE_STATE_COMMON;
            try {
                transport.copy(*atlas.get(), invalid);
            } catch (const std::invalid_argument&) {
                ++refusals;
            }
        }
        if (refusals != 4)
            throw std::runtime_error("Stereo transport failed an invalid-target rejection");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[6]{};
        UINT64 bytes = 0;
        device.GetCopyableFootprints(&eye_description, 0, eye_description.DepthOrArraySize,
            0, footprints, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        Com<ID3D12Resource> readback;
        gpu_check(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(readback.put())), "Create measurement buffer");
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> commands;
        Com<ID3D12Fence> fence;
        gpu_check(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                __uuidof(ID3D12CommandAllocator),
                                                reinterpret_cast<void**>(allocator.put())), "Create measurement allocator");
        gpu_check(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                           __uuidof(ID3D12GraphicsCommandList),
                                           reinterpret_cast<void**>(commands.put())), "Create measurement list");
        gpu_check(commands->Close(), "Initialize measurement list");
        gpu_check(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                     reinterpret_cast<void**>(fence.put())), "Create measurement fence");
        if (queued) {
            std::array<Com<IDirect3DTexture9>,3> sources;
            const DWORD colors[6]{0xff171343,0xffa72919,0xff2467ab,0xffcb7921,0xffa152c7,0xff65b431};
            for (unsigned frame=0;frame<3;++frame) {
                gpu_check(legacy->CreateTexture(512,256,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT,sources[frame].put(),nullptr),"Prepare independent queued atlas");
                Com<IDirect3DSurface9> target;
                gpu_check(sources[frame]->GetSurfaceLevel(0,target.put()),"Get queued atlas surface");
                for (unsigned eye=0;eye<2;++eye) {
                    const RECT region{LONG(eye*256),0,LONG((eye+1)*256),256};
                    gpu_check(legacy->ColorFill(target.get(),&region,colors[frame*2+eye]),
                        "Initialize queued atlas contents");
                }
            }
            Com<ID3D12Fence> gate;
            gpu_check(device.CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),
                reinterpret_cast<void**>(gate.put())),"Create blocked-queue transfer gate");
            std::atomic<bool> all_queued{false},gate_opened{false},serialized{false};
            gpu_check(queue.Wait(gate.get(),1),"Block GPU before independent queued transfers");
            auto watchdog=std::async(std::launch::async,[&] {
                const auto deadline=GetTickCount64()+2000;
                while (!all_queued.load() && GetTickCount64()<deadline) Sleep(1);
                serialized.store(!all_queued.load());
                Sleep(100);
                gate_opened.store(true);
                return gate->Signal(1);
            });
            for (unsigned frame=0;frame<3;++frame) {
                if (serial_wait && frame) transport.wait();
                const std::array<D3D12EyeTarget,2> output{{{eye_array.get(),frame*2},
                    {eye_array.get(),frame*2+1}}};
                if (transport.copy(*sources[frame].get(),output)!=frame+1)
                    throw std::runtime_error("Queued transfer serial changed");
            }
            const bool submitted_before_completion=!gate_opened.load() && gate->GetCompletedValue()==0;
            all_queued.store(true);
            bool fourth_waited=true;
            if (queued_drain) {
                for (auto& source:sources) source.reset();
                if (gate->GetCompletedValue()!=0)
                    throw std::runtime_error("Source references were not released while the GPU was blocked");
            } else {
                if (transport.copy(*sources[0].get(),targets)!=4)
                    throw std::runtime_error("Reused transfer serial changed");
                fourth_waited=gate_opened.load();
            }
            gpu_check(watchdog.get(),"Release blocked-queue transfer gate");
            transport.wait();
            if (serialized.load() || !submitted_before_completion)
                throw std::runtime_error("Transfer serialized before three queued GPU copies were submitted");
            if (!fourth_waited)
                throw std::runtime_error("Transfer reused a still-pending allocator");
            gpu_check(allocator->Reset(),"Reset queued measurement allocator");
            gpu_check(commands->Reset(allocator.get(),nullptr),"Reset queued measurement commands");
            for (unsigned image=0;image<6;++image) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition={eye_array.get(),image,D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_COPY_SOURCE};
                commands->ResourceBarrier(1,&barrier);
                D3D12_TEXTURE_COPY_LOCATION from{},to{};
                from.pResource=eye_array.get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex=image;
                to.pResource=readback.get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint=footprints[image];
                commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
                commands->ResourceBarrier(1,&barrier);
            }
            gpu_check(commands->Close(),"Close queued measurement commands");
            ID3D12CommandList* lists[]{commands.get()};queue.ExecuteCommandLists(1,lists);
            gpu_check(queue.Signal(fence.get(),1),"Signal queued pixel measurement");
            const auto deadline=GetTickCount64()+10000;
            while (fence->GetCompletedValue()<1) {
                if (GetTickCount64()>=deadline) throw std::runtime_error("Queued measurement timed out");
                Sleep(1);
            }
            if (fence->GetCompletedValue()==UINT64_MAX)
                throw std::runtime_error("Queued measurement device removed");
            void* pointer=nullptr;
            D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
            gpu_check(readback->Map(0,&range,&pointer),"Inspect all queued eye images");
            bool valid=true;
            for (unsigned image=0;image<6;++image)
                for (unsigned y=0;y<256;++y) for (unsigned x=0;x<256;++x) {
                    DWORD pixel=0;
                    std::memcpy(&pixel,static_cast<const char*>(pointer)+footprints[image].Offset+
                        y*footprints[image].Footprint.RowPitch+x*4,4);
                    valid=valid && pixel==colors[image];
                }
            D3D12_RANGE written{0,0};readback->Unmap(0,&written);
            if (!valid) throw std::runtime_error("Queued transfer corrupted eye content or order");
            if (serial_wait) throw std::runtime_error("Serial-wait fault incorrectly passed");
            for (auto& source:sources) source.reset();
            Com<IDirect3DSurface9> backbuffer;
            gpu_check(legacy->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,backbuffer.put()),
                "Get native backbuffer before drained reset");
            gpu_check(legacy->SetRenderTarget(0,backbuffer.get()),"Detach atlas before drained reset");
            backbuffer.reset();surface.reset();atlas.reset();
            gpu_check(legacy->Reset(&presentation),"Reset native D3D9 after all transfer contexts drained");
            std::cout<<"PASS: queued transfer contexts=3; submitted_before_gpu_completion=3; "
                <<(queued_drain?"all_three_sources_released_before_completion; ":"fourth_reuse_fenced; ")
                <<"exact_pixels=393216; drained_source_ownership; native_reset=1\n";
            return 0;
        }
        UINT64 measured = 0;
        std::array<DWORD,2> colors{};
        for (UINT frame = 1; frame <= 16; ++frame) {
            colors=color_test?std::array<DWORD,2>{0xff000000 | (frame<<8) | frame*2,0xff80c0ff}:
                std::array<DWORD,2>{0xff000077 | (frame << 16) | ((frame * 11) << 8),
                                   0xff005500 | ((frame * 7) << 16) | frame};
            for (UINT eye = 0; eye < 2; ++eye) {
                D3DRECT rectangle{static_cast<LONG>(eye * 256), 0, static_cast<LONG>((eye + 1) * 256), 256};
                gpu_check(legacy->Clear(1, &rectangle, D3DCLEAR_TARGET, colors[eye], 1, 0),
                          "Render changing eye content");
                if (color_test)
                    for (UINT x=0;x<256;++x) {
                        const RECT strip{LONG(eye*256+x),0,LONG(eye*256+x+1),256};
                        gpu_check(legacy->ColorFill(surface.get(),&strip,color_ramp(eye,x)),
                            "Render complete native encoded color ramp");
                    }
            }
            auto frame_targets = targets;
            if (swapped)
                std::swap(frame_targets[0], frame_targets[1]);
            if (transport.copy(*atlas.get(), frame_targets) != frame)
                throw std::runtime_error("Stereo transfer fence serial did not advance exactly once");
            transport.wait();
            gpu_check(allocator->Reset(), "Reuse measurement allocator");
            gpu_check(commands->Reset(allocator.get(), nullptr), "Reuse measurement commands");
            for (UINT eye = 0; eye < 2; ++eye) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = eye_array.get();
                barrier.Transition.Subresource = eye;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                commands->ResourceBarrier(1, &barrier);
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = eye_array.get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = eye;
                to.pResource = readback.get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = footprints[eye];
                commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                commands->ResourceBarrier(1, &barrier);
            }
            gpu_check(commands->Close(), "Close measurement commands");
            ID3D12CommandList* lists[]{commands.get()};
            queue.ExecuteCommandLists(1, lists);
            gpu_check(queue.Signal(fence.get(), frame), "Signal measurement completion");
            const auto deadline = GetTickCount64() + 10000;
            while (fence->GetCompletedValue() < frame) {
                if (GetTickCount64() >= deadline)
                    throw std::runtime_error("Eye transfer measurement timed out");
                Sleep(1);
            }
            if (fence->GetCompletedValue() == UINT64_MAX)
                throw std::runtime_error("Eye transfer measurement device removed");
            void* pointer = nullptr;
            D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
            gpu_check(readback->Map(0, &range, &pointer), "Inspect actual transferred eye images");
            bool valid = true;
            for (UINT eye = 0; eye < 2; ++eye)
                for (UINT y = 0; y < 256; ++y)
                    for (UINT x = 0; x < 256; ++x) {
                        DWORD pixel;
                        std::memcpy(&pixel, static_cast<const char*>(pointer) + footprints[eye].Offset +
                                    y * footprints[eye].Footprint.RowPitch + x * 4, 4);
                        valid = valid && pixel == (color_test?color_ramp(eye,x):colors[eye]);
                        ++measured;
                    }
            D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
            if (!valid) {
                std::cout << "FAIL: actual eye order/content mismatch at frame " << frame << '\n';
                return 1;
            }
        }
        if (swapped)
            throw std::runtime_error("Swapped-eye negative control incorrectly passed");
        if (color_test)
            verify_display_encoding(device,queue,*eye_array.get(),*readback.get(),*allocator.get(),
                *commands.get(),*fence.get(),eye_description.Format);
        std::cout << "PASS: reusable stereo transfer; frames=16; exact_pixels=" << measured
                  << "; invalid_targets_rejected=" << refusals << "; two persistent eye subresources\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
