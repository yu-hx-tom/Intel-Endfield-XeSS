// Self-owned feasibility test only. Two real swapchains, distinct HWNDs, one GPU.
// No game hooks, texture bridge, XeFG, or claim of game presentation compatibility.
#define XESS_WSI_EMBEDDED
#include "vulkan_wsi_smoke.cpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <array>
#include <iostream>

namespace {
template<class T> struct CoexistCom {
    T* p{};
    ~CoexistCom() { if(p) p->Release(); }
    T* operator->() const { return p; }
};
void checked(HRESULT result,const char* label) {
    if(FAILED(result)) throw std::runtime_error(std::string(label)+" HRESULT="+std::to_string(result));
}
struct Output {
    HWND window{};
    CoexistCom<IDXGIFactory4> factory;
    CoexistCom<IDXGIAdapter1> adapter;
    CoexistCom<ID3D12Device> device;
    CoexistCom<ID3D12CommandQueue> queue;
    CoexistCom<IDXGISwapChain3> swap;
    CoexistCom<ID3D12DescriptorHeap> heap;
    std::array<CoexistCom<ID3D12Resource>,2> buffers;
    CoexistCom<ID3D12CommandAllocator> allocator;
    CoexistCom<ID3D12GraphicsCommandList> command;
    CoexistCom<ID3D12Fence> fence;
    HANDLE event{};
    UINT stride{};
    UINT64 fenceValue{};
    unsigned presentCalls{};
    bool pending{};
    ~Output() {
        // Destruction follows the final finite queue wait. On a hung device the
        // self-owned process watchdog is the last bound, not a false completion.
        if(pending) { try { wait(); } catch(...) { TerminateProcess(GetCurrentProcess(),125); } }
        if(event) CloseHandle(event);
        if(window) DestroyWindow(window);
    }
    void wait() {
        if(fence->GetCompletedValue()==UINT64_MAX) throw std::runtime_error("D3D12 device removed");
        if(fence->GetCompletedValue()<fenceValue) {
            checked(fence->SetEventOnCompletion(fenceValue,event),"SetEventOnCompletion");
            if(WaitForSingleObject(event,3000)!=WAIT_OBJECT_0 || fence->GetCompletedValue()<fenceValue || fence->GetCompletedValue()==UINT64_MAX)
                throw std::runtime_error("Output queue completion not confirmed");
        }
        pending=false;
    }
    void initialize(const Smoke& smoke) {
        if(IsWindowVisible(smoke.window)) throw std::runtime_error("Vulkan render window unexpectedly visible");
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext=&id;
        const auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(smoke.gipa(smoke.instance,"vkGetPhysicalDeviceProperties2"));
        if(!properties) throw std::runtime_error("Missing physical device ID query");
        properties(smoke.physical,&props);
        if(!id.deviceLUIDValid) throw std::runtime_error("Vulkan LUID unavailable");
        LUID luid{}; std::memcpy(&luid,id.deviceLUID,sizeof(luid));
        checked(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory.p)),"CreateDXGIFactory2");
        checked(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter.p)),"EnumAdapterByLuid");
        DXGI_ADAPTER_DESC1 desc{}; checked(adapter->GetDesc1(&desc),"GetDesc1");
        if(std::memcmp(&desc.AdapterLuid,&luid,sizeof(luid))) throw std::runtime_error("Adapter LUID mismatch");
        checked(D3D12CreateDevice(adapter.p,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device.p)),"D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        checked(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue.p)),"CreateCommandQueue");
        window=CreateWindowExW(0,L"XeSSVulkanObserverSmoke",L"Vulkan / DXGI window coexistence (auto closes)",
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,640,360,nullptr,nullptr,smoke.module,nullptr);
        if(!window || window==smoke.window) throw std::runtime_error("Distinct output HWND unavailable");
        DXGI_SWAP_CHAIN_DESC1 swapDesc{}; swapDesc.Width=640; swapDesc.Height=360;
        swapDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; swapDesc.SampleDesc.Count=1;
        swapDesc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; swapDesc.BufferCount=2; swapDesc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        CoexistCom<IDXGISwapChain1> first;
        checked(factory->CreateSwapChainForHwnd(queue.p,window,&swapDesc,nullptr,nullptr,&first.p),"CreateSwapChainForHwnd");
        checked(first->QueryInterface(IID_PPV_ARGS(&swap.p)),"Query swapchain3");
        checked(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER),"MakeWindowAssociation");
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{}; heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors=2;
        checked(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap.p)),"CreateDescriptorHeap");
        stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto descriptor=heap->GetCPUDescriptorHandleForHeapStart();
        for(unsigned i=0;i<2;++i) {
            checked(swap->GetBuffer(i,IID_PPV_ARGS(&buffers[i].p)),"GetBuffer");
            device->CreateRenderTargetView(buffers[i].p,nullptr,descriptor); descriptor.ptr+=stride;
        }
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator.p)),"CreateCommandAllocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.p,nullptr,IID_PPV_ARGS(&command.p)),"CreateCommandList");
        checked(command->Close(),"Close(initial)");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence.p)),"CreateFence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) throw std::runtime_error("CreateEvent");
        ShowWindow(window,SW_SHOWNOACTIVATE);
    }
    void present(uint32_t frame) {
        if(pending) wait();
        checked(allocator->Reset(),"Allocator Reset"); checked(command->Reset(allocator.p,nullptr),"Command Reset");
        const auto index=swap->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=buffers[index].p; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        command->ResourceBarrier(1,&barrier);
        auto rtv=heap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr+=index*stride;
        const float color[]{0.1f,0.2f+0.5f*(frame%24)/24.0f,0.35f,1.0f};
        command->ClearRenderTargetView(rtv,color,0,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); command->ResourceBarrier(1,&barrier);
        checked(command->Close(),"Command Close");
        ID3D12CommandList* list=command.p; queue->ExecuteCommandLists(1,&list); pending=true;
        ++fenceValue; checked(queue->Signal(fence.p,fenceValue),"Signal");
        const auto result=swap->Present(0,0);
        if(result!=S_OK) throw std::runtime_error("DXGI Present not visible-success: "+std::to_string(result));
        ++presentCalls; wait();
    }
};
}
int main() {
    try {
        Watchdog guard;
        Smoke hidden; hidden.initialize(VK_API_VERSION_1_3,false,true);
        Output output; output.initialize(hidden);
        hidden.run(24,[](void* p,uint32_t frame) { static_cast<Output*>(p)->present(frame); },&output);
        if(IsWindowVisible(hidden.window) || !IsWindowVisible(output.window) || output.presentCalls!=24)
            throw std::runtime_error("Window visibility / frame accounting failed");
        std::cout<<"{\"passed\":true,\"hiddenVulkanPresents\":24,\"visibleDxgiPresents\":24,\"sameAdapterLuid\":true,\"distinctHwnds\":true,\"pixelTransferVerified\":false,\"xefgTested\":false,\"gameTested\":false}\n";
    } catch(const std::exception& error) { std::cerr<<"COEXIST_FAIL: "<<error.what()<<'\n'; return 1; }
}
