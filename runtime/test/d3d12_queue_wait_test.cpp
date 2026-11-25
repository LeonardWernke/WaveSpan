// D3D-97: cross-queue Wait-before-Signal must make progress. No game files.
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
template<class T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
};

bool wait_for(ID3D12Fence* fence, UINT64 value, DWORD timeout_ms = 3000) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    if (FAILED(fence->SetEventOnCompletion(value, event))) {
        CloseHandle(event);  // no wait was registered
        return false;
    }
    if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) {
        // The fence dispatcher can still hold this event, and a queued GPU Wait can keep the queue destructor
        // blocked forever. End the failed test without closing live handles or waiting for blocked queues.
        std::fputs("cross-queue Wait-before-Signal timed out\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    CloseHandle(event);
    return true;
}

int test_host_import(ID3D12Device* inner) {
    Ref<ID3D12Device> device;
    inner->AddRef();
    if (FAILED(rt::d3d12_xs::create_device_proxy(inner, IID_PPV_ARGS(&device.p)))) {
        inner->Release();
        return 1;
    }
    rt::d3d12::set_device_outer(inner, device.p);
    constexpr std::size_t bytes = 64 * 1024;
    struct HostAllocation {
        void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        ~HostAllocation() { if (p) VirtualFree(p, 0, MEM_RELEASE); }
    } source, destination, heap_memory, guarded_desc;
    if (!source.p || !destination.p || !heap_memory.p || !guarded_desc.p) return 1;
    std::memset(source.p, 0x65, bytes);
    std::memset(destination.p, 0x12, bytes);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Ref<ID3D12Resource> src, dst;
    using SlotFn = uint64_t(WINAPI*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    const auto import_slot = reinterpret_cast<SlotFn>((*reinterpret_cast<void***>(device.p))[133]);
    const auto import = [&](void* ptr, ID3D12Resource** out) {
        return static_cast<HRESULT>(import_slot(device.p, reinterpret_cast<uint64_t>(ptr),
            reinterpret_cast<uint64_t>(&desc), UINT(-1), 0, reinterpret_cast<uint64_t>(&__uuidof(ID3D12Resource)),
            reinterpret_cast<uint64_t>(out), 0, 0, 0, 0, 0, 0, 0, 0));
    };
    const HRESULT imported = import(source.p, &src.p);
    if (imported == E_NOTIMPL) return 77;
    if (FAILED(imported) || FAILED(import(destination.p, &dst.p))) {
        std::fprintf(stderr, "host buffer import failed, source HRESULT 0x%08lx\n", static_cast<unsigned long>(imported));
        return 1;
    }
    void* mapped = nullptr;
    if (FAILED(src->Map(0, nullptr, &mapped)) || mapped != source.p) return 1;
    src->Unmap(0, nullptr);
    // Check that the allocation was neither replaced nor zeroed and that later CPU writes are visible too.
    if (static_cast<unsigned char*>(source.p)[0] != 0x65) return 1;
    void* unaligned = static_cast<unsigned char*>(source.p) + 1;
    ID3D12Resource* rejected = nullptr;
    if (import(unaligned, &rejected) != E_INVALIDARG || rejected) return 1;
    std::memset(source.p, 0xa6, bytes);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> done;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))) ||
        FAILED(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator.p))) ||
        FAILED(device->CreateCommandList(0, queue_desc.Type, allocator.p, nullptr, IID_PPV_ARGS(&list.p))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done.p)))) return 1;
    list->CopyBufferRegion(dst.p, 0, src.p, 0, bytes);
    if (FAILED(list->Close())) return 1;
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(done.p, 1)) || !wait_for(done.p, 1)) std::_Exit(1);
    if (std::memcmp(source.p, destination.p, bytes) != 0) {
        std::fputs("GPU copy did not update the original host allocation\n", stderr);
        return 1;
    }
    std::puts("GPU read and wrote the original host memory");

    struct XboxHeapDesc {
        D3D12_DESCRIPTOR_HEAP_DESC pc;
        void* address;
        std::size_t bytes;
    } heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1}};
    using HeapHandle = uint64_t(WINAPI*)(void*);
    const auto heap_handle = [](ID3D12DescriptorHeap* heap, unsigned slot) {
        return reinterpret_cast<HeapHandle>((*reinterpret_cast<void***>(heap))[slot])(heap);
    };
    for (void* allocation : {source.p, heap_memory.p}) {
        heap_desc.address = static_cast<std::byte*>(allocation) + 32;
        heap_desc.bytes = heap_desc.pc.NumDescriptors * device->GetDescriptorHandleIncrementSize(heap_desc.pc.Type);
        Ref<ID3D12DescriptorHeap> heap;
        if (FAILED(device->CreateDescriptorHeap(&heap_desc.pc, IID_PPV_ARGS(&heap.p))) ||
            heap_handle(heap.p, 9) != reinterpret_cast<uint64_t>(heap_desc.address) || !heap_handle(heap.p, 10)) {
            std::fputs("Xbox descriptor heap did not preserve its supplied backing address\n", stderr);
            return 1;
        }
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* boundary = static_cast<std::byte*>(guarded_desc.p) + system.dwPageSize;
    DWORD old_protection = 0;
    if (!VirtualProtect(boundary, system.dwPageSize, PAGE_NOACCESS, &old_protection)) return 1;
    auto* pc_desc = reinterpret_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(boundary - sizeof(heap_desc.pc));
    std::memcpy(pc_desc, &heap_desc.pc, sizeof(*pc_desc));
    Ref<ID3D12DescriptorHeap> standard_heap;
    if (FAILED(device->CreateDescriptorHeap(pc_desc, IID_PPV_ARGS(&standard_heap.p))) ||
        !heap_handle(standard_heap.p, 9)) return 1;
    std::puts("Xbox heaps preserve caller backing and desktop descriptors need only their public bytes");
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    Ref<ID3D12Device> device;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p)))) return 77;
    if (argc == 2 && std::strcmp(argv[1], "--host-import") == 0) return test_host_import(device.p);
    if (argc == 2 && std::strcmp(argv[1], "--timeout-control") == 0) {
        Ref<ID3D12Fence> never_signaled;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&never_signaled.p)))) return 1;
        // Expected to leave the process with code 1 promptly, without destroying the pending event.
        wait_for(never_signaled.p, 1, 20);
        return 0;  // a completed wait here means the control did not exercise the timeout path
    }
    D3D12_COMMAND_QUEUE_DESC desc{};
    Ref<ID3D12CommandQueue> copy, compute, direct_a, direct_b;
    desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&copy.p)))) return 77;
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&compute.p)))) return 77;
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&direct_a.p))) ||
        FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&direct_b.p)))) return 77;

    Ref<ID3D12Fence> gate_copy, gate_direct, done_copy, done_direct;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_copy.p))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_direct.p))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_copy.p))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_direct.p)))) return 1;

    // Submission order is deliberately opposite the dependency order.
    const bool submitted = SUCCEEDED(copy->Wait(gate_copy.p, 1)) &&
        SUCCEEDED(copy->Signal(done_copy.p, 1)) &&
        SUCCEEDED(direct_a->Wait(gate_direct.p, 1)) &&
        SUCCEEDED(direct_a->Signal(done_direct.p, 1)) &&
        SUCCEEDED(compute->Signal(gate_copy.p, 2)) &&
        SUCCEEDED(direct_b->Signal(gate_direct.p, 2));
    const bool copy_complete = submitted && wait_for(done_copy.p, 1);
    const bool direct_complete = submitted && wait_for(done_direct.p, 1);
    if (!copy_complete || !direct_complete) {
        // A failed submit can leave a GPU Wait pending. Gate 1 is not a valid fallback once queue signals
        // have claimed 2, so do not destroy queues or fences while their work may still be outstanding.
        std::fputs("cross-queue Wait-before-Signal did not complete\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    std::puts("copy/compute and direct/direct Wait-before-Signal completed");
    return 0;
}
