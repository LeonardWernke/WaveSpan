// CPU benchmark of the D3D12 -> Vulkan replay: how long the layer itself takes per command to record a list
// (CommandList), check it (prepare) and write it (emit). Every Vulkan entry point is a no-op that only counts its
// calls, and resources are fake bindings, so no GPU, driver or game files are involved and the numbers are the
// layer's own cost. The real ExecuteCommandLists adds the driver's vkCmd* cost on top.
// A binary occlusion resolve counts as one mock call at the emit boundary. The benchmark excludes recording
// the Vulkan commands inside the queue's binary resolve helper.
//
// Two measurements:
//   - per command type: a list of N commands of one kind (transitions come in pairs or batches, queries as Begin+End),
//     timed on its own, so one slow kind stands out;
//   - a frame: L lists of P passes, each pass a mix of what a frame records (barriers, whole-view and rectangle
//     clears, buffer and texture copies, a UAV clear, draws, dispatches with descriptors, queries and predication),
//     plus a multi-batch submission case reusing PreparedList and EmitScratch across batches.
// prepare reuses PreparedList storage across submissions and plans the query batch. emit reuses EmitScratch
// and commits query state, as VulkanQueue::execute does.
// Each figure is the median over --reps runs after --warmup runs. Heap allocations (global operator new) and
// Vulkan calls are counted per command; both are deterministic, unlike the times.
//
// Usage:
//   replay_cpu_bench [--lists L] [--passes P] [--count N] [--reps R] [--warmup W] [--csv] [--self-test]
//                    [--check-budget]
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../src/vulkan_replay.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <new>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Heap allocations, counted for the whole process; read around one phase at a time.
static std::size_t g_allocations = 0;

void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace wavespan;

namespace {

std::size_t g_vk_calls = 0;

// A no-op for any vkCmd* signature that counts its calls.
template<typename F>
struct Nop;
template<typename R, typename... A>
struct Nop<R(A...)> {
    static R VKAPI_CALL call(A...) {
        ++g_vk_calls;
        if constexpr (!std::is_void_v<R>) return R{};
    }
};
template<typename PFN>
PFN nop() {
    return &Nop<std::remove_pointer_t<PFN>>::call;
}

template<typename H>
H fake_handle(std::uintptr_t value) {
    return reinterpret_cast<H>(value);
}

constexpr VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM;

struct ViewKey {
    VkImage image = VK_NULL_HANDLE;
    VkImageAspectFlags aspect = 0;
    std::uint32_t base_mip = 0;
    std::uint32_t base_layer = 0;
    std::uint32_t layer_count = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool operator==(const ViewKey&) const = default;
};

struct ViewKeyHash {
    std::size_t operator()(const ViewKey& k) const noexcept {
        std::uint64_t h = reinterpret_cast<std::uintptr_t>(k.image);
        const auto mix = [&](std::uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(k.aspect);
        mix((std::uint64_t(k.base_mip) << 32) | k.base_layer);
        mix((std::uint64_t(k.layer_count) << 32) | static_cast<std::uint32_t>(k.format));
        return static_cast<std::size_t>(h ^ (h >> 32));
    }
};

// The resources one frame uses, registered like VulkanContext does: bindings by ID in an unordered_map, image
// views cached in a flat packed-key unordered_map.
struct Scene {
    enum : ResourceId {
        color = 1,
        depth,
        texture,  // 256x256 RGBA8, 9 mips: a copy destination and shader resource
        upload,   // 64 MiB upload buffer: copy source
        constants,
        uav,
        predicate,
        results,  // resolved query data
        vertex_buf,
        index_buf,
        occlusion_heap = 100,
        timestamp_heap,
        churn_img_base = 1000,
        churn_buf_base = 2000,
    };
    static constexpr std::uint32_t churn_pool = 64;
    static constexpr std::uint32_t queries = 4096;
    static constexpr std::uintptr_t gfx_pipe_id = 0x1001;
    static constexpr std::uintptr_t cmp_pipe_id = 0x1002;
    static constexpr std::uint64_t vb_va = 0x10000000ull;
    static constexpr std::uint64_t ib_va = 0x20000000ull;

    std::unordered_map<ResourceId, ResourceBinding> resources;
    std::unordered_map<ViewKey, VkImageView, ViewKeyHash> views;
    GraphicsPipeline gfx_pipeline{};
    ComputePipeline cmp_pipeline{};
    QueryPool occlusion, timestamps;
    LayoutTracker tracker;
    ReplayFunctions vk;

    Scene() {
        const auto image = [&](ResourceId id, VkFormat format, VkImageAspectFlags aspects, std::uint32_t w,
                               std::uint32_t h, std::uint32_t mips, bool attachment) {
            ResourceBinding b{};
            b.image = fake_handle<VkImage>(0x1000 + id);
            b.format = format;
            b.aspects = aspects;
            b.width = w;
            b.height = h;
            b.mip_levels = mips;
            b.color_attachment = attachment;
            resources[id] = b;
        };
        const auto buffer = [&](ResourceId id, VkDeviceSize size, std::uint64_t va = 0) {
            ResourceBinding b{};
            b.buffer = fake_handle<VkBuffer>(0x2000 + id);
            b.size = size;
            b.va = va;
            resources[id] = b;
        };
        image(color, rgba8, VK_IMAGE_ASPECT_COLOR_BIT, 1920, 1080, 1, true);
        image(depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1920, 1080, 1, false);
        image(texture, rgba8, VK_IMAGE_ASPECT_COLOR_BIT, 256, 256, 9, false);
        buffer(upload, 64ull << 20);
        buffer(constants, 1 << 20);
        buffer(uav, 1 << 20);
        buffer(predicate, 4096);
        buffer(results, queries * 8);
        buffer(vertex_buf, 4ull << 20, vb_va);
        buffer(index_buf, 4ull << 20, ib_va);
        tracker.track(color, resources[color], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tracker.track(depth, resources[depth], VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        tracker.track(texture, resources[texture], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (std::uint32_t c = 0; c < churn_pool; ++c) {
            const ResourceId img_id = churn_img_base + c;
            const ResourceId buf_id = churn_buf_base + c;
            image(img_id, rgba8, VK_IMAGE_ASPECT_COLOR_BIT, 512, 512, 1, true);
            buffer(buf_id, 65536);
            tracker.track(img_id, resources[img_id], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        occlusion = {fake_handle<VkQueryPool>(0x3000), d3d12_query_heap::occlusion, queries, true,
                     std::vector<bool>(queries)};
        timestamps = {fake_handle<VkQueryPool>(0x3001), d3d12_query_heap::timestamp, queries, false,
                      std::vector<bool>(queries)};

        gfx_pipeline.pipeline = fake_handle<VkPipeline>(0x7000);
        gfx_pipeline.layout = fake_handle<VkPipelineLayout>(0x7100);
        gfx_pipeline.push_stages = VK_SHADER_STAGE_ALL_GRAPHICS;
        gfx_pipeline.bindless_set = fake_handle<VkDescriptorSet>(0x6000);
        gfx_pipeline.color_formats = {rgba8};
        gfx_pipeline.depth_format = VK_FORMAT_D32_SFLOAT;
        gfx_pipeline.depth_aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        gfx_pipeline.vertex_slots = 0x3u;

        cmp_pipeline.pipeline = fake_handle<VkPipeline>(0x7001);
        cmp_pipeline.layout = fake_handle<VkPipelineLayout>(0x7101);
        cmp_pipeline.bindless_set = fake_handle<VkDescriptorSet>(0x6000);

        vk.pipeline_barrier = nop<PFN_vkCmdPipelineBarrier>();
        vk.clear_color_image = nop<PFN_vkCmdClearColorImage>();
        vk.clear_depth_stencil_image = nop<PFN_vkCmdClearDepthStencilImage>();
        vk.pipeline_barrier2 = nop<PFN_vkCmdPipelineBarrier2>();
        vk.begin_rendering = nop<PFN_vkCmdBeginRendering>();
        vk.end_rendering = nop<PFN_vkCmdEndRendering>();
        vk.clear_attachments = nop<PFN_vkCmdClearAttachments>();
        vk.copy_buffer = nop<PFN_vkCmdCopyBuffer>();
        vk.copy_buffer_to_image = nop<PFN_vkCmdCopyBufferToImage>();
        vk.copy_image_to_buffer = nop<PFN_vkCmdCopyImageToBuffer>();
        vk.copy_image = nop<PFN_vkCmdCopyImage>();
        vk.resolve_image = nop<PFN_vkCmdResolveImage>();
        vk.fill_buffer = nop<PFN_vkCmdFillBuffer>();
        vk.update_buffer = nop<PFN_vkCmdUpdateBuffer>();
        vk.begin_conditional_rendering = nop<PFN_vkCmdBeginConditionalRenderingEXT>();
        vk.end_conditional_rendering = nop<PFN_vkCmdEndConditionalRenderingEXT>();
        vk.prepare_predicate = [](VkBuffer source, VkDeviceSize offset, PredicateCapture& out) {
            out = {source, offset, fake_handle<VkBuffer>(0x5000), 0, VK_NULL_HANDLE};
            return true;
        };
        vk.capture_predicate = [](VkCommandBuffer, const PredicateCapture&) {};
        vk.reset_query_pool = nop<PFN_vkCmdResetQueryPool>();
        vk.begin_query = nop<PFN_vkCmdBeginQuery>();
        vk.end_query = nop<PFN_vkCmdEndQuery>();
        vk.write_timestamp = nop<PFN_vkCmdWriteTimestamp>();
        vk.copy_query_pool_results = nop<PFN_vkCmdCopyQueryPoolResults>();
        vk.resolve_binary_occlusion = [](VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer,
                                          VkDeviceSize) { ++g_vk_calls; };
        vk.bind_pipeline = nop<PFN_vkCmdBindPipeline>();
        vk.push_constants = nop<PFN_vkCmdPushConstants>();
        vk.bind_descriptor_sets = nop<PFN_vkCmdBindDescriptorSets>();
        vk.dispatch = nop<PFN_vkCmdDispatch>();
        vk.compute_pipeline = [this](std::uintptr_t id) -> const ComputePipeline* {
            return id == cmp_pipe_id ? &cmp_pipeline : nullptr;
        };
        vk.bind_vertex_buffers2 = nop<PFN_vkCmdBindVertexBuffers2>();
        vk.bind_index_buffer = nop<PFN_vkCmdBindIndexBuffer>();
        vk.set_viewport = nop<PFN_vkCmdSetViewport>();
        vk.set_scissor = nop<PFN_vkCmdSetScissor>();
        vk.set_primitive_topology = nop<PFN_vkCmdSetPrimitiveTopology>();
        vk.set_primitive_restart_enable = nop<PFN_vkCmdSetPrimitiveRestartEnable>();
        vk.set_stencil_reference = nop<PFN_vkCmdSetStencilReference>();
        vk.set_blend_constants = nop<PFN_vkCmdSetBlendConstants>();
        vk.draw = nop<PFN_vkCmdDraw>();
        vk.draw_indexed = nop<PFN_vkCmdDrawIndexed>();
        vk.graphics_pipeline = [this](std::uintptr_t id) -> const GraphicsPipeline* {
            return id == gfx_pipe_id ? &gfx_pipeline : nullptr;
        };
        vk.zero_buffer = fake_handle<VkBuffer>(0x20FF);
        vk.zero_offset = 0;
        vk.zero_size = 4096;
        vk.buffer_at = [this](std::uint64_t va, std::uint64_t& size, VkBuffer& buf, VkDeviceSize& off) {
            for (const auto id : {vertex_buf, index_buf}) {
                const auto& r = resources.at(id);
                if (va >= r.va && va < r.va + r.size) {
                    buf = r.buffer;
                    off = va - r.va;
                    size = std::min<std::uint64_t>(size, r.size - off);
                    return true;
                }
            }
            return false;
        };
        vk.image_view = [this](const ResourceBinding& r, const VkImageSubresourceRange& range) {
            const ViewKey key{r.image, range.aspectMask, range.baseMipLevel, range.baseArrayLayer,
                              range.layerCount, r.format};
            if (const auto it = views.find(key); it != views.end()) return it->second;
            const VkImageView view = fake_handle<VkImageView>(0x4000 + views.size());
            views[key] = view;
            return view;
        };
        vk.query_pool = [this](QueryHeapId id) -> QueryPool* {
            if (id == occlusion_heap) return &occlusion;
            if (id == timestamp_heap) return &timestamps;
            return nullptr;
        };
    }

    const ResourceBinding* resolve(ResourceId id) const {
        const auto it = resources.find(id);
        return it == resources.end() ? nullptr : &it->second;
    }
};

namespace s = d3d12_state;
namespace y = d3d12_sync;
namespace a = d3d12_access;
namespace q = d3d12_query;

constexpr ClearColor black{0, 0, 0, 1};
constexpr Rect two_rects[] = {{0, 0, 640, 360}, {1280, 720, 1920, 1080}};
const TargetView color_view{Scene::color, 0, 0, 1};
const TargetView depth_view{Scene::depth, 0, 0, 1};

// One kind of command, recorded `count` times (a pair or a Begin+End counts as two).
struct Kind {
    const char* name;
    bool (*record)(CommandList&, std::uint32_t index);
    std::uint32_t commands_per_record;
};

bool texture_copy(CommandList& list, std::uint32_t index) {
    // A 64x64 block into mip 0 from a placed footprint (row pitch 256, offsets 512-aligned).
    CopyTexture copy{};
    copy.dst = {Scene::texture, false, 0, 0, 0, 0, 0, 0};
    copy.src = {Scene::upload, true, 0, (index % 1024) * 16384ull, 64, 64, 1, 256};
    copy.x = (index % 4) * 64;
    copy.y = (index / 4 % 4) * 64;
    return list.copy_texture(copy);
}

bool uav_clear(CommandList& list, std::uint32_t index) {
    const UavView view{Scene::uav, true, VK_FORMAT_R32_UINT, (index % 256) * 4096ull, 4096, 4, 0, 0, 1};
    return list.clear_unordered_access(view, false, {index, 0, 0, 0});
}

bool record_dispatch(CommandList& list, std::uint32_t index) {
    Dispatch d{};
    d.pipeline = Scene::cmp_pipe_id;
    d.user_data[0] = index;
    d.x = 8;
    d.y = 8;
    d.z = 1;
    return list.dispatch(d);
}

bool record_draw(CommandList& list, std::uint32_t index) {
    Draw d{};
    d.pipeline = Scene::gfx_pipe_id;
    d.user_data[0] = index;
    d.render_targets = {color_view};
    d.depth = depth_view;
    d.vertex_buffers = {{Scene::vb_va, 4096, 32}, {Scene::vb_va + 4096, 4096, 16}};
    d.index = {Scene::ib_va, 4096, 42};
    d.topology = 4;
    d.viewport = {0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f};
    d.scissor = {0, 0, 1920, 1080};
    d.blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
    d.indexed = true;
    d.count = 36;
    d.instances = 1;
    return list.draw(std::move(d));
}

const Kind kinds[] = {
    {"transition pair",
     [](CommandList& l, std::uint32_t) {
         return l.transition({Scene::color, all_subresources, s::pixel_shader_resource, s::render_target}) &&
                l.transition({Scene::color, all_subresources, s::render_target, s::pixel_shader_resource});
     },
     2},
    {"transition batch (4)",
     [](CommandList& l, std::uint32_t) {
         return l.transition({Scene::color, all_subresources, s::pixel_shader_resource, s::render_target}) &&
                l.transition({Scene::texture, 0, s::pixel_shader_resource, s::copy_dest}) &&
                l.transition({Scene::uav, all_subresources, s::common, s::unordered_access}) &&
                l.transition({Scene::constants, all_subresources, s::copy_dest, s::vertex_and_constant_buffer});
     },
     4},
    {"buffer barrier (enhanced)",
     [](CommandList& l, std::uint32_t) {
         return l.barrier(BufferBarrier{y::copy, y::pixel_shading, a::copy_dest, a::shader_resource, Scene::constants});
     },
     1},
    {"texture barrier (enhanced)",
     [](CommandList& l, std::uint32_t) {
         return l.barrier(TextureBarrier{y::copy, y::pixel_shading, a::copy_dest, a::shader_resource,
                                         d3d12_layout::copy_dest, d3d12_layout::shader_resource, Scene::texture,
                                         BarrierSubresourceRange{0, 1, 0, 1, 0, 1}, 0}) &&
                l.barrier(TextureBarrier{y::pixel_shading, y::copy, a::shader_resource, a::copy_dest,
                                         d3d12_layout::shader_resource, d3d12_layout::copy_dest, Scene::texture,
                                         BarrierSubresourceRange{0, 1, 0, 1, 0, 1}, 0});
     },
     2},
    {"clear RTV, whole view", [](CommandList& l, std::uint32_t) { return l.clear_render_target(color_view, black); }, 1},
    {"clear RTV, 2 rects",
     [](CommandList& l, std::uint32_t) { return l.clear_render_target(color_view, black, two_rects); }, 1},
    {"clear DSV, whole view",
     [](CommandList& l, std::uint32_t) { return l.clear_depth_stencil(depth_view, clear_depth, 1, 0); },
     1},
    {"copy buffer region",
     [](CommandList& l, std::uint32_t i) {
         return l.copy_buffer({Scene::constants, (i % 256) * 256ull, Scene::upload, (i % 4096) * 256ull, 256});
     },
     1},
    {"copy texture region", texture_copy, 1},
    {"clear UAV (buffer)", uav_clear, 1},
    {"dispatch (descriptors)", record_dispatch, 1},
    {"draw (indexed, RT+DS+desc)", record_draw, 1},
    {"occlusion Begin+End",
     [](CommandList& l, std::uint32_t i) {
         const std::uint32_t index = i % Scene::queries;
         return l.begin_query({Scene::occlusion_heap, q::binary_occlusion, index}) &&
                l.end_query({Scene::occlusion_heap, q::binary_occlusion, index});
     },
     2},
    {"timestamp",
     [](CommandList& l, std::uint32_t i) {
         return l.end_query({Scene::timestamp_heap, q::timestamp, i % Scene::queries});
     },
     1},
    {"resolve 16 queries",
     [](CommandList& l, std::uint32_t i) {
         const std::uint32_t first = (i * 16) % Scene::queries;
         return l.resolve_queries({Scene::occlusion_heap, q::binary_occlusion, first, 16, Scene::results, first * 8ull});
     },
     1},
    {"predication on+off",
     [](CommandList& l, std::uint32_t) {
         return l.set_predication({Scene::predicate, 0, d3d12_predication::equal_zero}) &&
                l.set_predication({0, 0, 0});
     },
     2},
};

// One pass of the frame: barriers, clears, copies, UAV clear, draws, dispatches with descriptors, queries, predication.
bool frame_pass(CommandList& l, std::uint32_t pass) {
    const std::uint32_t query = (pass * 2) % Scene::queries;
    return l.end_query({Scene::timestamp_heap, q::timestamp, query}) &&
           l.transition({Scene::color, all_subresources, s::pixel_shader_resource, s::render_target}) &&
           l.transition({Scene::uav, all_subresources, s::common, s::unordered_access}) &&
           l.clear_render_target(color_view, black) && l.clear_depth_stencil(depth_view, clear_depth, 1, 0) &&
           record_draw(l, pass) &&
           l.begin_query({Scene::occlusion_heap, q::binary_occlusion, query}) &&
           l.clear_render_target(color_view, black, two_rects) &&
           record_draw(l, pass) &&
           l.end_query({Scene::occlusion_heap, q::binary_occlusion, query}) &&
           l.transition({Scene::color, all_subresources, s::render_target, s::pixel_shader_resource}) &&
           l.copy_buffer({Scene::constants, (pass % 256) * 256ull, Scene::upload, (pass % 4096) * 256ull, 256}) &&
           l.barrier(BufferBarrier{y::copy, y::pixel_shading, a::copy_dest, a::shader_resource, Scene::constants}) &&
           l.barrier(TextureBarrier{y::pixel_shading, y::copy, a::shader_resource, a::copy_dest,
                                    d3d12_layout::shader_resource, d3d12_layout::copy_dest, Scene::texture,
                                    BarrierSubresourceRange{0, 1, 0, 1, 0, 1}, 0}) &&
           texture_copy(l, pass) &&
           l.barrier(TextureBarrier{y::copy, y::pixel_shading, a::copy_dest, a::shader_resource,
                                    d3d12_layout::copy_dest, d3d12_layout::shader_resource, Scene::texture,
                                    BarrierSubresourceRange{0, 1, 0, 1, 0, 1}, 0}) &&
           uav_clear(l, pass) &&
           record_dispatch(l, pass) &&
           l.transition({Scene::uav, all_subresources, s::unordered_access, s::common}) &&
           l.set_predication({Scene::predicate, 0, d3d12_predication::not_equal_zero}) &&
           l.clear_render_target({Scene::color, 0, 0, 1}, black, two_rects) && l.set_predication({0, 0, 0}) &&
           l.end_query({Scene::timestamp_heap, q::timestamp, query + 1}) &&
           l.resolve_queries({Scene::timestamp_heap, q::timestamp, query, 2, Scene::results, query * 8ull});
}

// One pass of resource churn: rotating transient images/buffers, coalesced transitions, clears, copies, and draws.
bool churn_pass(CommandList& l, std::uint32_t pass) {
    const ResourceId img_a = Scene::churn_img_base + (pass % Scene::churn_pool);
    const ResourceId img_b = Scene::churn_img_base + ((pass + 1) % Scene::churn_pool);
    const ResourceId buf_a = Scene::churn_buf_base + (pass % Scene::churn_pool);
    const TargetView tv_a{img_a, 0, 0, 1};
    return l.transition({img_a, all_subresources, s::pixel_shader_resource, s::render_target}) &&
           l.transition({img_b, all_subresources, s::pixel_shader_resource, s::copy_dest}) &&
           l.transition({buf_a, all_subresources, s::common, s::copy_dest}) &&
           l.clear_render_target(tv_a, black) &&
           l.copy_buffer({buf_a, 0, Scene::upload, (pass % 4096) * 256ull, 256}) &&
           l.barrier(BufferBarrier{y::copy, y::pixel_shading, a::copy_dest, a::shader_resource, buf_a}) &&
           l.barrier(TextureBarrier{y::copy, y::pixel_shading, a::copy_dest, a::shader_resource,
                                    d3d12_layout::copy_dest, d3d12_layout::shader_resource, img_b,
                                    BarrierSubresourceRange{0, 1, 0, 1, 0, 1}, 0}) &&
           record_draw(l, pass) &&
           l.transition({img_a, all_subresources, s::render_target, s::pixel_shader_resource}) &&
           l.transition({buf_a, all_subresources, s::copy_dest, s::common});
}

struct Phase {
    double ns = 0;            // median, per command
    double allocations = 0;   // per command
    double vk_calls = 0;      // per command (emit only)
};

struct Result {
    std::size_t commands = 0;
    Phase record, prepare, emit;
    ReplayCounters counters;
    ReplayResult replay = ReplayResult::success;
    bool recorded = true;
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

// Records `lists` lists with `build`, prepares and emits them across `batches` submission batches (reusing
// PreparedList and EmitScratch storage), `reps` times after `warmup` runs.
template<typename Build>
Result run(Scene& scene, std::size_t lists, Build&& build, int reps, int warmup, std::size_t batches = 1) {
    using clock = std::chrono::steady_clock;
    std::vector<CommandList> recorded(lists);
    std::vector<PreparedList> prepared(lists);
    EmitScratch scratch;
    const VkCommandBuffer command_buffer = fake_handle<VkCommandBuffer>(0x10);
    const auto resolve = [&scene](ResourceId id) { return scene.resolve(id); };
    std::vector<double> t_record, t_prepare, t_emit;
    Result out;
    const std::size_t batch_count = std::max<std::size_t>(1, std::min(batches, lists));
    for (int rep = 0; rep < warmup + reps; ++rep) {
        const std::size_t a0 = g_allocations;
        const auto t0 = clock::now();
        for (auto& list : recorded) {
            list.reset();
            if (!build(list)) out.recorded = false;
            list.close();
        }
        const auto t1 = clock::now();
        const std::size_t a1 = g_allocations;
        std::chrono::nanoseconds prepare_ns{0}, emit_ns{0};
        std::size_t prepare_allocs = 0, emit_allocs = 0, emit_vk_calls = 0;
        for (std::size_t b = 0; b < batch_count; ++b) {
            const std::size_t begin = (b * lists) / batch_count;
            const std::size_t end = ((b + 1) * lists) / batch_count;
            const std::size_t ba0 = g_allocations;
            const auto bt0 = clock::now();
            QueryReplay queries;
            for (std::size_t i = begin; i < end; ++i) {
                if (const auto r = prepare(recorded[i], resolve, scene.vk, prepared[i]); r != ReplayResult::success) {
                    out.replay = r;
                    break;
                }
            }
            if (out.replay == ReplayResult::success) {
                for (std::size_t i = begin; i < end; ++i) {
                    if (const auto r = queries.plan(prepared[i]); r != ReplayResult::success) {
                        out.replay = r;
                        break;
                    }
                }
            }
            const auto bt1 = clock::now();
            const std::size_t ba1 = g_allocations;
            const std::size_t bv1 = g_vk_calls;
            ReplayCounters rep_counters{};
            if (out.replay == ReplayResult::success) {
                std::uint32_t open_labels = 0;
                for (std::size_t i = begin; i < end; ++i)
                    emit(prepared[i], command_buffer, scene.vk, &scene.tracker, &open_labels, &scratch,
                         &rep_counters);
                close_labels(command_buffer, scene.vk, open_labels);
                queries.commit();  // every simulated submission completes immediately
            }
            if (rep >= warmup) {
                if (b == 0) out.counters = rep_counters;
                else {
                    out.counters.submissions += rep_counters.submissions;
                    out.counters.commands += rep_counters.commands;
                    out.counters.barriers += rep_counters.barriers;
                    out.counters.barrier_flushes += rep_counters.barrier_flushes;
                    out.counters.coalesced_barriers += rep_counters.coalesced_barriers;
                }
            }
            const auto bt2 = clock::now();
            const std::size_t ba2 = g_allocations;
            const std::size_t bv2 = g_vk_calls;
            prepare_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(bt1 - bt0);
            emit_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(bt2 - bt1);
            prepare_allocs += (ba1 - ba0);
            emit_allocs += (ba2 - ba1);
            emit_vk_calls += (bv2 - bv1);
        }

        std::size_t commands = 0;
        for (const auto& list : recorded) commands += list.commands().size();
        out.commands = commands;
        if (rep < warmup || !commands) continue;
        t_record.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / double(commands));
        t_prepare.push_back(double(prepare_ns.count()) / double(commands));
        t_emit.push_back(double(emit_ns.count()) / double(commands));
        out.record.allocations = double(a1 - a0) / double(commands);
        out.prepare.allocations = double(prepare_allocs) / double(commands);
        out.emit.allocations = double(emit_allocs) / double(commands);
        out.emit.vk_calls = double(emit_vk_calls) / double(commands);
    }
    out.record.ns = median(t_record);
    out.prepare.ns = median(t_prepare);
    out.emit.ns = median(t_emit);
    return out;
}

const char* result_name(ReplayResult r) {
    switch (r) {
        case ReplayResult::success: return "success";
        case ReplayResult::list_open: return "list_open";
        case ReplayResult::unresolved_resource: return "unresolved_resource";
        case ReplayResult::invalid_subresource: return "invalid_subresource";
        case ReplayResult::invalid_clear: return "invalid_clear";
        case ReplayResult::invalid_copy: return "invalid_copy";
        case ReplayResult::invalid_predication: return "invalid_predication";
        case ReplayResult::invalid_query: return "invalid_query";
        case ReplayResult::invalid_indirect_arguments: return "invalid_indirect_arguments";
        case ReplayResult::unsupported: return "unsupported";
    }
    return "?";
}

void print_header(bool csv) {
    if (csv)
        std::printf("case,commands,record_ns,prepare_ns,emit_ns,total_ns,record_allocs,prepare_allocs,emit_allocs,"
                    "vk_calls\n");
    else
        std::printf("%-28s %8s | %9s %9s %9s %9s | %6s %6s %6s | %5s\n", "ns per command", "commands", "record",
                    "prepare", "emit", "total", "alloc:r", "p", "e", "vk");
}

void print_row(const char* name, const Result& r, bool csv) {
    const double total = r.record.ns + r.prepare.ns + r.emit.ns;
    if (csv)
        std::printf("%s,%zu,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f\n", name, r.commands, r.record.ns, r.prepare.ns,
                    r.emit.ns, total, r.record.allocations, r.prepare.allocations, r.emit.allocations, r.emit.vk_calls);
    else
        std::printf("%-28s %8zu | %9.1f %9.1f %9.1f %9.1f | %6.2f %6.2f %6.2f | %5.2f\n", name, r.commands,
                    r.record.ns, r.prepare.ns, r.emit.ns, total, r.record.allocations, r.prepare.allocations,
                    r.emit.allocations, r.emit.vk_calls);
}

int usage() {
    std::fprintf(stderr,
                 "usage: replay_cpu_bench [--lists L] [--passes P] [--count N] [--reps R] [--warmup W] [--csv] "
                 "[--self-test] [--check-budget]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t lists = 8, passes = 128, count = 4096;
    int reps = 25, warmup = 5;
    bool csv = false, self_test = false, check_budget = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](auto& out) {
            if (i + 1 >= argc) return false;
            const long long v = std::atoll(argv[++i]);
            if (v <= 0) return false;
            out = static_cast<std::remove_reference_t<decltype(out)>>(v);
            return true;
        };
        if (arg == "--lists") { if (!value(lists)) return usage(); }
        else if (arg == "--passes") { if (!value(passes)) return usage(); }
        else if (arg == "--count") { if (!value(count)) return usage(); }
        else if (arg == "--reps") { if (!value(reps)) return usage(); }
        else if (arg == "--warmup") { if (!value(warmup)) return usage(); }
        else if (arg == "--csv") csv = true;
        else if (arg == "--self-test") self_test = true;
        else if (arg == "--check-budget") check_budget = true;
        else return usage();
    }
    if (self_test) {  // small and quick: every kind and the frame must record and replay, deterministically
        lists = 2;
        passes = 4;
        count = 16;
        reps = 2;
        warmup = 1;
    }

    Scene scene;
    int failures = 0;
    const auto check = [&](const char* name, const Result& r) {
        if (!r.recorded || r.replay != ReplayResult::success || !r.commands || r.emit.vk_calls <= 0) {
            std::fprintf(stderr, "FAIL %s: recorded=%d replay=%s commands=%zu vk_calls=%.2f\n", name, r.recorded,
                         result_name(r.replay), r.commands, r.emit.vk_calls);
            ++failures;
            return;
        }
        if (check_budget && r.emit.allocations != 0.0) {
            std::fprintf(stderr, "BUDGET FAIL %s: emit_allocs=%.4f (expected 0.0000)\n", name, r.emit.allocations);
            ++failures;
        }
    };

    if (!self_test) print_header(csv);
    for (const Kind& kind : kinds) {
        const std::uint32_t records = std::uint32_t(std::max<std::size_t>(1, count / kind.commands_per_record));
        const Result r = run(
            scene, 1,
            [&](CommandList& list) {
                for (std::uint32_t i = 0; i < records; ++i)
                    if (!kind.record(list, i)) return false;
                return true;
            },
            reps, warmup);
        check(kind.name, r);
        if (!self_test) print_row(kind.name, r, csv);
    }
    const auto frame = [&](CommandList& list) {
        for (std::uint32_t pass = 0; pass < passes; ++pass)
            if (!frame_pass(list, pass)) return false;
        return true;
    };
    const auto churn_frame = [&](CommandList& list) {
        for (std::uint32_t pass = 0; pass < passes; ++pass)
            if (!churn_pass(list, pass)) return false;
        return true;
    };
    const Result f = run(scene, lists, frame, reps, warmup);
    check("frame", f);
    const std::size_t batches = std::min<std::size_t>(4, lists);
    const Result fb = run(scene, lists, frame, reps, warmup, batches);
    check("frame (batched)", fb);
    const Result fc = run(scene, lists, churn_frame, reps, warmup);
    check("frame (resource churn)", fc);
    if (self_test || check_budget) {
        // The counts don't depend on timing: a second run must make exactly the same allocations and calls.
        const Result again = run(scene, lists, frame, reps, warmup);
        const Result churn_again = run(scene, lists, churn_frame, reps, warmup);
        const auto same_counts = [](const Result& a, const Result& b) {
            return a.commands == b.commands && a.emit.vk_calls == b.emit.vk_calls &&
                   a.prepare.allocations == b.prepare.allocations && a.emit.allocations == b.emit.allocations &&
                   a.counters.submissions == b.counters.submissions && a.counters.commands == b.counters.commands &&
                   a.counters.barriers == b.counters.barriers &&
                   a.counters.barrier_flushes == b.counters.barrier_flushes &&
                   a.counters.coalesced_barriers == b.counters.coalesced_barriers;
        };
        if (!same_counts(again, f) || !same_counts(churn_again, fc)) {
            std::fprintf(stderr, "FAIL determinism: barrier/command/alloc counts differ between runs\n");
            ++failures;
        }
        if (check_budget) {
            if (f.counters.submissions != lists || f.counters.commands != f.commands || f.counters.barriers == 0 ||
                fc.counters.coalesced_barriers == 0) {
                std::fprintf(stderr,
                             "BUDGET FAIL counters: frame(sub=%llu cmd=%llu bar=%llu) churn(coalesced=%llu)\n",
                             static_cast<unsigned long long>(f.counters.submissions),
                             static_cast<unsigned long long>(f.counters.commands),
                             static_cast<unsigned long long>(f.counters.barriers),
                             static_cast<unsigned long long>(fc.counters.coalesced_barriers));
                ++failures;
            }
        }
    }
    if (self_test) {
        std::printf("replay_cpu_bench self-test: %zu kinds and a %zu-command frame recorded and replayed, %s\n",
                    std::size(kinds), f.commands, failures ? "FAILED" : "ok");
        return failures ? 1 : 0;
    }
    char name[64];
    std::snprintf(name, sizeof name, "frame (%zu lists x %zu passes)", lists, passes);
    print_row(name, f, csv);
    std::snprintf(name, sizeof name, "frame (%zu batches)", batches);
    print_row(name, fb, csv);
    std::snprintf(name, sizeof name, "frame (churn %zu x %zu)", lists, passes);
    print_row(name, fc, csv);

    // The floor under prepare and emit: making and freeing one PreparedList entry per command, which execute does
    // for every list before any command is looked at.
    std::vector<double> floor_ns;
    for (int rep = 0; rep < warmup + reps; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        {
            PreparedList list;
            list.commands.assign(f.commands, {});
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (rep >= warmup)
            floor_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / double(f.commands));
    }
    const double floor = median(floor_ns);
    if (csv) {
        std::printf("prepared_entries_only,%zu,0,%.1f,0,%.1f,0,0,0,0\n", f.commands, floor, floor);
        return failures ? 1 : 0;
    }
    std::printf("%-28s %8zu | %9s %9.1f %9s %9.1f |\n", "  PreparedList entries only", f.commands, "", floor, "",
                floor);
    std::printf("\nframe: %.1f us per ExecuteCommandLists of %zu commands, layer only (no driver)\n",
                (f.record.ns + f.prepare.ns + f.emit.ns) * double(f.commands) / 1000.0, f.commands);
    std::printf("one PreparedList entry (detail::Prepared): %zu bytes; one recorded Command: %zu bytes\n",
                sizeof(detail::Prepared), sizeof(Command));
    return failures ? 1 : 0;
}
