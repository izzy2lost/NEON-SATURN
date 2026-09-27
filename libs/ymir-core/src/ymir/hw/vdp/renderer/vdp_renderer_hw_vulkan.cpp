#include <ymir/hw/vdp/renderer/vdp_renderer_hw_vulkan.hpp>

#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>

#include <ymir/util/bit_ops.hpp>
#include <ymir/util/dev_assert.hpp>
#include <ymir/util/dev_log.hpp>
#include <ymir/util/dirty_bitmap.hpp>
#include <ymir/util/inline.hpp>
#include <ymir/util/scope_guard.hpp>

#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
    #include <android/log.h>
#endif

#include <fmt/format.h>

#include <cmrc/cmrc.hpp>
CMRC_DECLARE(ymir_core_shaders);

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// This renderer is a port of the Direct3D 12 renderer (vdp_renderer_hw_d3d12.cpp) and runs the same HLSL shaders,
// compiled to SPIR-V. The VDP1 command processing and VDP2 parameter calculations are kept identical to that renderer
// so that fixes can be carried over between the two; only the GPU resource management and command submission differ.
//
// Differences from the Direct3D 12 renderer:
// - Root constants are push constants.
// - Every image stays in VK_IMAGE_LAYOUT_GENERAL, so resource transitions reduce to execution and memory
//   dependencies. BarrierTracker emits a single global memory barrier whenever a command was recorded since the last
//   barrier, which covers every hazard the Direct3D 12 renderer handles with its per-resource transitions.
// - Fence values are emulated with one VkFence per frame, since timeline semaphores are not available on every
//   Vulkan 1.1 device.
// - The finished frame is copied into a host-visible buffer and handed to the frontend on the CPU one frame later
//   instead of being copied into a frontend-provided texture.
// - Vulkan does not zero-initialize memory, so every resource is cleared on creation.

namespace ymir::vdp {

namespace grp {

    // -------------------------------------------------------------------------
    // Dev log groups

    // Hierarchy:
    //
    // vk_base
    //   vk_upload
    //   vk_vdp1
    //   vk_vdp2

    struct vk_base {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
        static constexpr std::string_view name = "VDP-VK";
    };

    struct vk_upload : public vk_base {
        // static constexpr devlog::Level level = devlog::level::trace;
        static constexpr std::string_view name = "VDP-VK-Upload";
    };

    struct vk_vdp1 : public vk_base {
        static constexpr std::string_view name = "VDP1-VK";
    };

    struct vk_vdp2 : public vk_base {
        static constexpr std::string_view name = "VDP2-VK";
    };

} // namespace grp

// ---------------------------------------------------------------------------------------------------------------------

/// @brief Reports a renderer problem. Development logs are compiled out of release frontends, so problems are also
/// sent to the system log where one is available (logcat on Android).
/// @tparam Group the dev log group
template <typename Group, typename... Args>
static void Report(devlog::Level level, fmt::format_string<Args...> format, Args &&...args) {
    const std::string message = fmt::format(format, std::forward<Args>(args)...);
    if (level == devlog::level::info) {
        devlog::info<Group>("{}", message);
    } else {
        devlog::warn<Group>("{}", message);
    }
#if defined(__ANDROID__)
    __android_log_print(level == devlog::level::info ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "YmirVulkan", "%s",
                        message.c_str());
#endif
}

// ---------------------------------------------------------------------------------------------------------------------

/// @brief Contains the compiled shader files from res/shaders/src.
cmrc::embedded_filesystem g_fsShaders = cmrc::ymir_core_shaders::get_filesystem();

using HLSLbool = uint32; // bools align to 4 bytes
using HLSLint = sint32;
using HLSLuint = uint32;

struct alignas(HLSLuint) ColorR8G8B8A8 {
    uint8 r, g, b, a;
};
static_assert(sizeof(ColorR8G8B8A8) == sizeof(uint32));

union HLSLuint2 {
    std::array<HLSLuint, 2> array;
    struct {
        HLSLuint x, y;
    };
    struct {
        HLSLuint r, g;
    };
};
static_assert(sizeof(HLSLuint2) == sizeof(HLSLuint) * 2);

union HLSLuint3 {
    std::array<HLSLuint, 3> array;
    struct {
        HLSLuint x, y, z;
    };
    struct {
        HLSLuint r, g, b;
    };
};
static_assert(sizeof(HLSLuint3) == sizeof(HLSLuint) * 3);

union HLSLuint4 {
    std::array<HLSLuint, 4> array;
    struct {
        HLSLuint x, y, z, w;
    };
    struct {
        HLSLuint r, g, b, a;
    };
};
static_assert(sizeof(HLSLuint4) == sizeof(HLSLuint) * 4);

union HLSLint2 {
    std::array<HLSLint, 2> array;
    struct {
        HLSLint x, y;
    };
    struct {
        HLSLint r, g;
    };
};
static_assert(sizeof(HLSLint2) == sizeof(HLSLint) * 2);

union HLSLint3 {
    std::array<HLSLint, 3> array;
    struct {
        HLSLint x, y, z;
    };
    struct {
        HLSLint r, g, b;
    };
};
static_assert(sizeof(HLSLint3) == sizeof(HLSLint) * 3);

/// @brief Packs up bool into the least significant bits of an unsigned integer.
/// @tparam T the unsigned integral type
/// @param[in] bools the bools to pack
/// @return the packed value
template <std::unsigned_integral T>
static uint32 PackBools(std::span<const bool> bools) {
    T value = 0;
    size_t count = std::min(bools.size(), sizeof(T) * 8);
    for (size_t i = 0; i < count; ++i) {
        if (bools[i]) {
            value |= static_cast<T>(1u) << static_cast<T>(i);
        }
    }
    return value;
}

// Base Xst, Yst, KA for params A and B relative to startY
struct alignas(16) VDP2RotParamBase {
    uint32 tableAddress;
    sint32 Xst, Yst;
    uint32 KA;
};
static_assert(sizeof(VDP2RotParamBase) == sizeof(uint32) * 4);

// ---------------------------------------------------------------------------------------------------------------------

/// @brief Maximum number of frames in flight.
static constexpr size_t kNumFrames = 3;

/// @brief Size of the upload buffers, in bytes.
/// Should be large enough to fit multiple worst case single transfers, but not waste space needlessly.
static constexpr VkDeviceSize kUploadBufferSize = (4 + 4 * kNumFrames) * 1024 * 1024;

// Descriptor bindings follow the shader registers. The shaders are compiled with -fvk-u-shift 16, so SRVs (t#) map to
// binding # and UAVs (u#) map to binding 16 + #.
static constexpr uint32 SRV(uint32 reg) {
    return reg;
}
static constexpr uint32 UAV(uint32 reg) {
    return 16 + reg;
}

// ---------------------------------------------------------------------------------------------------------------------
// Vulkan helpers

static const char *VkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    default: return "VkResult error";
    }
}

static util::ErrorMessage VkError(std::string_view what, VkResult result) {
    return util::ErrorMessage{fmt::format("{} ({} {})", what, VkResultName(result), static_cast<sint32>(result))};
}

/// @brief Intended use of a memory allocation.
enum class MemoryUsage {
    DeviceLocal, ///< GPU-only resources
    Upload,      ///< CPU-written staging memory
    Readback,    ///< CPU-read results
};

/// @brief Owns the Vulkan instance and device used by the renderer.
struct VulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32 queueFamily = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    VulkanDevice() = default;
    VulkanDevice(const VulkanDevice &) = delete;
    VulkanDevice &operator=(const VulkanDevice &) = delete;

    ~VulkanDevice() {
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    util::VoidResult<> Create() {
        // Optional validation layer for development (YMIR_VULKAN_VALIDATION=1)
        std::vector<const char *> layers{};
        if (const char *validation = std::getenv("YMIR_VULKAN_VALIDATION");
            validation != nullptr && std::string_view{validation} == "1") {
            uint32 count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> available(count);
            vkEnumerateInstanceLayerProperties(&count, available.data());
            for (const VkLayerProperties &layer : available) {
                if (std::string_view{layer.layerName} == "VK_LAYER_KHRONOS_validation") {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    break;
                }
            }
        }

        const VkApplicationInfo appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "Ymir",
            .applicationVersion = VK_MAKE_VERSION(Ymir_VERSION_MAJOR, Ymir_VERSION_MINOR, Ymir_VERSION_PATCH),
            .pEngineName = "Ymir VDP renderer",
            .engineVersion = 1,
            .apiVersion = VK_API_VERSION_1_1,
        };
        const VkInstanceCreateInfo instanceInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<uint32>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
        };
        if (VkResult res = vkCreateInstance(&instanceInfo, nullptr, &instance); res != VK_SUCCESS) {
            instance = VK_NULL_HANDLE;
            return VkError("Could not create Vulkan instance", res);
        }

        uint32 deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        if (devices.empty()) {
            return util::ErrorMessage{"No Vulkan devices found"};
        }

        // Pick the best suitable device: real GPUs over software implementations. A specific device can be requested
        // by name for testing with YMIR_VULKAN_DEVICE=<substring>.
        const char *requestedName = std::getenv("YMIR_VULKAN_DEVICE");
        std::string rejections{};
        int bestScore = -1;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (requestedName != nullptr && std::string_view{props.deviceName}.find(requestedName) == std::string_view::npos) {
                continue;
            }
            uint32 family = 0;
            if (auto result = CheckDeviceSupport(candidate, family); !result) {
                rejections += fmt::format("\n  {}: {}", props.deviceName, result.Error().message);
                continue;
            }
            int score = 0;
            switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 4; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 3; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 2; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 0; break;
            default: score = 1; break;
            }
            if (score > bestScore) {
                bestScore = score;
                physicalDevice = candidate;
                queueFamily = family;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            return util::ErrorMessage{fmt::format("No suitable Vulkan device found:{}", rejections)};
        }

        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

        // Scalar block layout is core in Vulkan 1.2; older devices expose it through an extension
        std::vector<const char *> extensions{};
        if (properties.apiVersion < VK_API_VERSION_1_2) {
            extensions.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
        }

        VkPhysicalDeviceScalarBlockLayoutFeatures scalarBlockLayoutFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
            .scalarBlockLayout = VK_TRUE,
        };
        VkPhysicalDeviceFeatures2 features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &scalarBlockLayoutFeatures,
        };
        features.features.shaderInt64 = VK_TRUE;
        features.features.shaderStorageImageExtendedFormats = VK_TRUE;
        // Direct3D 12 bounds-checks buffer accesses in shaders (out-of-bounds reads return zero and writes are
        // discarded), and the shaders were written with that in mind. robustBufferAccess is supported by every Vulkan
        // implementation and provides the same guarantee, which also prevents stray writes from corrupting memory.
        features.features.robustBufferAccess = VK_TRUE;

        const float queuePriority = 1.0f;
        const VkDeviceQueueCreateInfo queueInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queueFamily,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority,
        };
        const VkDeviceCreateInfo deviceInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueInfo,
            .enabledExtensionCount = static_cast<uint32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        if (VkResult res = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device); res != VK_SUCCESS) {
            device = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not create Vulkan device on {}", properties.deviceName), res);
        }
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        Report<grp::vk_base>(devlog::level::info, "Using {} (Vulkan {}.{}.{})", properties.deviceName,
                                   VK_API_VERSION_MAJOR(properties.apiVersion),
                                   VK_API_VERSION_MINOR(properties.apiVersion),
                                   VK_API_VERSION_PATCH(properties.apiVersion));
        return {};
    }

    /// @brief Checks if the given physical device supports every feature, format and limit the renderer needs.
    /// @param[in] candidate the physical device to check
    /// @param[out] outQueueFamily receives the index of a queue family with compute support
    /// @return nothing if the device is suitable, the reason why it's not otherwise
    static util::VoidResult<> CheckDeviceSupport(VkPhysicalDevice candidate, uint32 &outQueueFamily) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_1) {
            return util::ErrorMessage{"Vulkan 1.1 is required"};
        }

        if (props.apiVersion < VK_API_VERSION_1_2) {
            uint32 count = 0;
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> exts(count);
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, exts.data());
            const bool hasScalarBlockLayout = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties &e) {
                return std::string_view{e.extensionName} == VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME;
            });
            if (!hasScalarBlockLayout) {
                return util::ErrorMessage{"VK_EXT_scalar_block_layout is not supported"};
            }
        }

        VkPhysicalDeviceScalarBlockLayoutFeatures scalarBlockLayoutFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &scalarBlockLayoutFeatures,
        };
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!features.features.shaderInt64) {
            return util::ErrorMessage{"shaderInt64 is not supported"};
        }
        if (!features.features.shaderStorageImageExtendedFormats) {
            return util::ErrorMessage{"shaderStorageImageExtendedFormats is not supported"};
        }
        if (!scalarBlockLayoutFeatures.scalarBlockLayout) {
            return util::ErrorMessage{"scalarBlockLayout is not supported"};
        }

        // The largest texel buffers are the VDP1 internal sprite output and OIT list heads
        if (props.limits.maxTexelBufferElements < kVDP1FBRAMSize * 2 * 2) {
            return util::ErrorMessage{
                fmt::format("maxTexelBufferElements is too small ({})", props.limits.maxTexelBufferElements)};
        }

        struct FormatRequirement {
            VkFormat format;
            VkFormatFeatureFlags optimalTiling;
            VkFormatFeatureFlags buffer;
            const char *name;
        };
        static constexpr FormatRequirement kFormats[] = {
            {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, 0,
             "R8G8B8A8_UNORM"},
            {VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
             VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, "R8G8B8A8_UINT"},
            {VK_FORMAT_R16_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, 0,
             "R16_UINT"},
            {VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, 0, "R8_UINT"},
            {VK_FORMAT_R32_UINT, 0,
             VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
                 VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT,
             "R32_UINT"},
        };
        for (const FormatRequirement &req : kFormats) {
            VkFormatProperties formatProps{};
            vkGetPhysicalDeviceFormatProperties(candidate, req.format, &formatProps);
            if ((formatProps.optimalTilingFeatures & req.optimalTiling) != req.optimalTiling ||
                (formatProps.bufferFeatures & req.buffer) != req.buffer) {
                return util::ErrorMessage{fmt::format("Format {} lacks required features", req.name)};
            }
        }

        uint32 familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        // Prefer a graphics+compute family: it is usually the fastest one and always supports transfers
        for (uint32 pass = 0; pass < 2; ++pass) {
            for (uint32 i = 0; i < familyCount; ++i) {
                const VkQueueFlags flags = families[i].queueFlags;
                const bool compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
                const bool graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
                if (compute && (graphics || pass == 1)) {
                    outQueueFamily = i;
                    return {};
                }
            }
        }
        return util::ErrorMessage{"No compute queue"};
    }

    /// @brief Finds a memory type for an allocation.
    /// @param[in] typeBits the memory types allowed by the resource
    /// @param[in] usage the intended use of the memory
    /// @param[out] outCoherent receives whether the selected memory type is host-coherent
    /// @return the memory type index, or an error if no suitable type exists
    util::ValueResult<uint32> FindMemoryType(uint32 typeBits, MemoryUsage usage, bool &outCoherent) const {
        struct Candidate {
            VkMemoryPropertyFlags flags;
        };
        std::vector<VkMemoryPropertyFlags> candidates{};
        switch (usage) {
        case MemoryUsage::DeviceLocal:
            candidates = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
            break;
        case MemoryUsage::Upload:
            candidates = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
            break;
        case MemoryUsage::Readback:
            candidates = {
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            };
            break;
        }
        for (VkMemoryPropertyFlags required : candidates) {
            for (uint32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
                const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[i].propertyFlags;
                if ((typeBits & (1u << i)) && (flags & required) == required) {
                    outCoherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                    return i;
                }
            }
        }
        return util::ErrorMessage{"No suitable memory type"};
    }
};

/// @brief Owns a Vulkan object destroyed with a `vkDestroy*(VkDevice, T, const VkAllocationCallbacks *)` function.
template <typename T, auto fnDestroy>
class UniqueVk {
public:
    UniqueVk() = default;
    UniqueVk(const UniqueVk &) = delete;
    UniqueVk &operator=(const UniqueVk &) = delete;

    ~UniqueVk() {
        Reset();
    }

    void Assign(VkDevice device, T handle) {
        Reset();
        m_device = device;
        m_handle = handle;
    }

    void Reset() {
        if (m_handle != VK_NULL_HANDLE) {
            fnDestroy(m_device, m_handle, nullptr);
            m_handle = VK_NULL_HANDLE;
        }
    }

    T Get() const {
        return m_handle;
    }

    operator T() const {
        return m_handle;
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    T m_handle = VK_NULL_HANDLE;
};

using UniquePipeline = UniqueVk<VkPipeline, vkDestroyPipeline>;
using UniqueCommandPool = UniqueVk<VkCommandPool, vkDestroyCommandPool>;
using UniqueFence = UniqueVk<VkFence, vkDestroyFence>;
using UniqueDescriptorPool = UniqueVk<VkDescriptorPool, vkDestroyDescriptorPool>;

/// @brief A buffer with its own memory allocation and optional texel buffer view.
struct GpuBuffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBufferView view = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
    bool coherent = true;

    GpuBuffer() = default;
    GpuBuffer(const GpuBuffer &) = delete;
    GpuBuffer &operator=(const GpuBuffer &) = delete;

    ~GpuBuffer() {
        if (view != VK_NULL_HANDLE) {
            vkDestroyBufferView(device, view, nullptr);
        }
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr); // also unmaps
        }
    }

    /// @brief Creates the buffer and allocates its memory. Host-visible buffers are persistently mapped.
    util::VoidResult<> Create(const VulkanDevice &dev, VkDeviceSize bufferSize, VkBufferUsageFlags usage,
                              MemoryUsage memUsage, std::string_view name) {
        device = dev.device;
        size = bufferSize;
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bufferSize,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (VkResult res = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer); res != VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not create {}", name), res);
        }

        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(device, buffer, &reqs);
        auto memType = dev.FindMemoryType(reqs.memoryTypeBits, memUsage, coherent);
        if (!memType) {
            return util::ErrorMessage{fmt::format("Could not find memory for {}", name)};
        }
        const VkMemoryAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = memType.Value(),
        };
        if (VkResult res = vkAllocateMemory(device, &allocInfo, nullptr, &memory); res != VK_SUCCESS) {
            memory = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not allocate memory for {}", name), res);
        }
        if (VkResult res = vkBindBufferMemory(device, buffer, memory, 0); res != VK_SUCCESS) {
            return VkError(fmt::format("Could not bind memory for {}", name), res);
        }
        if (memUsage != MemoryUsage::DeviceLocal) {
            if (VkResult res = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped); res != VK_SUCCESS) {
                mapped = nullptr;
                return VkError(fmt::format("Could not map {}", name), res);
            }
        }
        return {};
    }

    /// @brief Creates a texel buffer view over the entire buffer.
    util::VoidResult<> CreateView(VkFormat format, std::string_view name) {
        const VkBufferViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer = buffer,
            .format = format,
            .offset = 0,
            .range = VK_WHOLE_SIZE,
        };
        if (VkResult res = vkCreateBufferView(device, &viewInfo, nullptr, &view); res != VK_SUCCESS) {
            view = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not create view for {}", name), res);
        }
        return {};
    }

    /// @brief Makes device writes visible to the CPU mapping for non-coherent memory.
    void Invalidate() const {
        if (!coherent && mapped != nullptr) {
            const VkMappedMemoryRange range{
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = memory,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
    }
};

/// @brief A 2D image or image array with its own memory allocation and a view of all layers.
/// Images are kept in VK_IMAGE_LAYOUT_GENERAL for their entire lifetime.
struct GpuImage {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32 width = 0;
    uint32 height = 0;
    uint32 layers = 0;

    GpuImage() = default;
    GpuImage(const GpuImage &) = delete;
    GpuImage &operator=(const GpuImage &) = delete;

    ~GpuImage() {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }

    /// @brief Creates the image, its memory and view.
    /// @param[in] array whether to create an array view (for Texture2DArray/RWTexture2DArray) or a 2D view
    util::VoidResult<> Create(const VulkanDevice &dev, uint32 w, uint32 h, uint32 numLayers, bool array,
                              VkFormat format, VkImageUsageFlags usage, std::string_view name) {
        device = dev.device;
        width = w;
        height = h;
        layers = numLayers;
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = numLayers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (VkResult res = vkCreateImage(device, &imageInfo, nullptr, &image); res != VK_SUCCESS) {
            image = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not create {}", name), res);
        }

        VkMemoryRequirements reqs{};
        vkGetImageMemoryRequirements(device, image, &reqs);
        bool coherent = false;
        auto memType = dev.FindMemoryType(reqs.memoryTypeBits, MemoryUsage::DeviceLocal, coherent);
        if (!memType) {
            return util::ErrorMessage{fmt::format("Could not find memory for {}", name)};
        }
        const VkMemoryAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = memType.Value(),
        };
        if (VkResult res = vkAllocateMemory(device, &allocInfo, nullptr, &memory); res != VK_SUCCESS) {
            memory = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not allocate memory for {}", name), res);
        }
        if (VkResult res = vkBindImageMemory(device, image, memory, 0); res != VK_SUCCESS) {
            return VkError(fmt::format("Could not bind memory for {}", name), res);
        }

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = numLayers,
                },
        };
        if (VkResult res = vkCreateImageView(device, &viewInfo, nullptr, &view); res != VK_SUCCESS) {
            view = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not create view for {}", name), res);
        }
        return {};
    }
};

/// @brief A descriptor binding in a compute pipeline layout.
struct LayoutBinding {
    uint32 binding;
    VkDescriptorType type;
};

/// @brief Descriptor set layout and pipeline layout for a family of compute shaders.
/// This is the equivalent of a Direct3D 12 root signature with 32-bit root constants and one descriptor table.
struct ComputeLayout {
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    uint32 pushConstantSize = 0;

    ComputeLayout() = default;
    ComputeLayout(const ComputeLayout &) = delete;
    ComputeLayout &operator=(const ComputeLayout &) = delete;

    ~ComputeLayout() {
        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        }
        if (setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        }
    }

    util::VoidResult<> Create(VkDevice dev, std::initializer_list<LayoutBinding> bindings, uint32 pcSize,
                              std::string_view name) {
        device = dev;
        pushConstantSize = pcSize;

        std::vector<VkDescriptorSetLayoutBinding> vkBindings{};
        for (const LayoutBinding &binding : bindings) {
            vkBindings.push_back({
                .binding = binding.binding,
                .descriptorType = binding.type,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            });
        }
        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32>(vkBindings.size()),
            .pBindings = vkBindings.data(),
        };
        if (VkResult res = vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout);
            res != VK_SUCCESS) {
            setLayout = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not create {} descriptor set layout", name), res);
        }

        const VkPushConstantRange pcRange{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = pcSize,
        };
        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &setLayout,
            .pushConstantRangeCount = pcSize > 0 ? 1u : 0u,
            .pPushConstantRanges = &pcRange,
        };
        if (VkResult res = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout); res != VK_SUCCESS) {
            pipelineLayout = VK_NULL_HANDLE;
            return VkError(fmt::format("Could not create {} pipeline layout", name), res);
        }
        return {};
    }
};

/// @brief Batches descriptor set updates.
class DescriptorWriter {
public:
    /// @brief Writes a storage buffer (StructuredBuffer, ByteAddressBuffer and their RW variants).
    DescriptorWriter &StorageBuffer(VkDescriptorSet set, uint32 binding, const GpuBuffer &buffer) {
        const VkDescriptorBufferInfo &info = m_bufferInfos.emplace_back(VkDescriptorBufferInfo{
            .buffer = buffer.buffer,
            .offset = 0,
            .range = VK_WHOLE_SIZE,
        });
        m_writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &info,
        });
        return *this;
    }

    /// @brief Writes a uniform texel buffer (Buffer<T>).
    DescriptorWriter &UniformTexelBuffer(VkDescriptorSet set, uint32 binding, const GpuBuffer &buffer) {
        return TexelBuffer(set, binding, buffer, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
    }

    /// @brief Writes a storage texel buffer (RWBuffer<T>).
    DescriptorWriter &StorageTexelBuffer(VkDescriptorSet set, uint32 binding, const GpuBuffer &buffer) {
        return TexelBuffer(set, binding, buffer, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
    }

    /// @brief Writes a sampled image (Texture2D<T>, Texture2DArray<T>).
    DescriptorWriter &SampledImage(VkDescriptorSet set, uint32 binding, const GpuImage &image) {
        return Image(set, binding, image, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    }

    /// @brief Writes a storage image (RWTexture2D<T>, RWTexture2DArray<T>).
    DescriptorWriter &StorageImage(VkDescriptorSet set, uint32 binding, const GpuImage &image) {
        return Image(set, binding, image, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }

    void Commit(VkDevice device) {
        vkUpdateDescriptorSets(device, static_cast<uint32>(m_writes.size()), m_writes.data(), 0, nullptr);
        m_writes.clear();
        m_bufferInfos.clear();
        m_imageInfos.clear();
        m_bufferViews.clear();
    }

private:
    std::vector<VkWriteDescriptorSet> m_writes;
    // Deques keep element addresses stable as they grow
    std::deque<VkDescriptorBufferInfo> m_bufferInfos;
    std::deque<VkDescriptorImageInfo> m_imageInfos;
    std::deque<VkBufferView> m_bufferViews;

    DescriptorWriter &TexelBuffer(VkDescriptorSet set, uint32 binding, const GpuBuffer &buffer,
                                  VkDescriptorType type) {
        assert(buffer.view != VK_NULL_HANDLE);
        const VkBufferView &view = m_bufferViews.emplace_back(buffer.view);
        m_writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pTexelBufferView = &view,
        });
        return *this;
    }

    DescriptorWriter &Image(VkDescriptorSet set, uint32 binding, const GpuImage &image, VkDescriptorType type) {
        const VkDescriptorImageInfo &info = m_imageInfos.emplace_back(VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = image.view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        });
        m_writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = &info,
        });
        return *this;
    }
};

// ---------------------------------------------------------------------------------------------------------------------

/// @brief A single allocation in an upload buffer.
struct UploadAllocation {
    size_t offset; ///< Offset (in bytes) into the upload buffer
    void *data;    ///< Mapped CPU pointer for writing
    size_t size;   ///< Requested size
};

/// @brief Chunk of data allocated for a frame in an upload buffer.
struct UploadFrameChunk {
    size_t endOffset;  ///< One past last byte used
    uint64 fenceValue; ///< Fence value of the frame that owns this chunk
};

/// @brief Manages per-frame allocations in an upload ring buffer.
class UploadRingBuffer {
public:
    /// @brief Creates the upload ring buffer.
    /// @param[in] device the device that will own the buffer
    /// @param[in] size the size (in bytes) of the upload buffer
    /// @return nothing on success, an error message otherwise
    util::VoidResult<> Create(const VulkanDevice &device, size_t size) {
        if (auto result = m_buffer.Create(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload,
                                          "upload buffer");
            !result) {
            return result;
        }
        m_basePtr = static_cast<uint8 *>(m_buffer.mapped);
        m_size = size;
        m_head = 0;
        m_tail = 0;
        m_chunks.clear();
        return {};
    }

    /// @brief Retrieves the buffer handle.
    /// @return the buffer handle
    VkBuffer GetBuffer() const {
        return m_buffer.buffer;
    }

    /// @brief Retrieves the allocated upload buffer size.
    /// @return the allocated buffer size
    size_t GetSize() const {
        return m_size;
    }

    /// @brief Sets a debug name for this buffer.
    /// @param[in] name the new debug name
    void SetDebugName(std::string_view name) {
        m_debugName = name;
    }

    /// @brief Retrieves the debug name for this buffer.
    /// @return this upload buffer's debug name
    std::string_view GetDebugName() const {
        return m_debugName;
    }

    /// @brief Attempts to allocate a chunk of memory from the upload buffer.
    /// @param[in] size the requested size
    /// @param[in] alignment the requested alignment
    /// @param[in] completedFenceValue the latest completed fence value, for reclaiming chunks from completed frames
    /// @param[out] outAlloc receives the allocation information
    /// @return `true` if allocation succeeded, `false` if there's no more room for allocations
    bool Allocate(size_t size, size_t alignment, uint64 completedFenceValue, UploadAllocation &outAlloc) {
        ReclaimCompletedChunks(completedFenceValue);

        // Check if there's enough contiguous space of the requested size starting from the aligned head position.
        // The head may be readjusted to the beginning of the upload buffer if there is not enough room at the end of
        // the buffer.
        size_t alignedHead = Align(m_head, alignment);
        if (!HasContiguousSpace(alignedHead, size, m_tail, &alignedHead)) {
            return false;
        }

        // Successfully allocated a chunk
        outAlloc.offset = alignedHead;
        outAlloc.data = static_cast<void *>(m_basePtr + alignedHead);
        outAlloc.size = size;
        devlog::trace<grp::vk_upload>("[{}] Allocated {:X}..{:X}, fence {} / {}", m_debugName, outAlloc.offset,
                                      outAlloc.offset + outAlloc.size, completedFenceValue, m_lastSubmittedFenceValue);

        // Update head position; wrap back to zero if needed
        m_head = alignedHead + size;
        if (m_head >= m_size) {
            m_head = 0;
        }

        return true;
    }

    /// @brief Finds the fence value to wait for which will have enough space for the requested allocation.
    /// @param[in] size the requested size
    /// @param[in] alignment the requested alignment
    /// @return the minimum fence number to wait for which frees up enough space for the requested allocation
    uint64 FindFenceValueForAllocation(size_t size, size_t alignment) {
        // Common early bail-outs:
        // - there's already enough room for the buffer, so there's no need to wait
        // - the chunk list is empty
        const size_t alignedHead = Align(m_head, alignment);
        if (HasContiguousSpace(alignedHead, size, m_tail)) {
            return m_lastCompletedFenceValue;
        }
        if (m_chunks.empty()) {
            return m_lastSubmittedFenceValue;
        }

        size_t queuePos = 0;
        size_t tail = m_tail;
        uint64 fenceValue = m_lastCompletedFenceValue;
        do {
            if (HasContiguousSpace(alignedHead, size, tail)) {
                return fenceValue;
            }
            tail = m_chunks[queuePos].endOffset;
            fenceValue = m_chunks[queuePos].fenceValue;
            ++queuePos;
        } while (queuePos < m_chunks.size());
        return m_lastSubmittedFenceValue;
    }

    /// @brief Records the end of a frame.
    /// @param[in] fenceValue the frame's fence value
    void EndFrame(uint64 fenceValue) {
        UploadFrameChunk &chunk = m_chunks.emplace_back();
        chunk.endOffset = m_head;
        chunk.fenceValue = fenceValue;
        devlog::trace<grp::vk_upload>("[{}] Frame ended, fence {}", m_debugName, fenceValue);
        m_lastSubmittedFenceValue = std::max(m_lastSubmittedFenceValue, fenceValue);
    }

private:
    GpuBuffer m_buffer;
    uint8 *m_basePtr = nullptr;
    size_t m_size = 0;
    size_t m_head = 0;
    size_t m_tail = 0;
    std::deque<UploadFrameChunk> m_chunks;
    uint64 m_lastSubmittedFenceValue = 0;
    uint64 m_lastCompletedFenceValue = 0;
    std::string m_debugName;

    /// @brief Reclaims allocated chunks from previously completed frames.
    /// @param[in] fenceValue the latest completed fence value
    void ReclaimCompletedChunks(uint64 fenceValue) {
        while (!m_chunks.empty() && m_chunks.front().fenceValue <= fenceValue) {
            m_tail = m_chunks.front().endOffset;
            devlog::trace<grp::vk_upload>("[{}] Reclaimed fence {}, tail={:X}", m_debugName,
                                          m_chunks.front().fenceValue, m_tail);
            m_chunks.pop_front();
        }
        m_lastCompletedFenceValue = std::max(m_lastCompletedFenceValue, fenceValue);
    }

    /// @brief Checks if there's enough free contiguous space from a starting point.
    /// @param[in] start the starting offset
    /// @param[in] size the requested allocation size
    /// @param[in] tail the allocation tail
    /// @param[out] outStart if specified, receives the updated start offset, either the provided start offset or zero
    /// @return `true` if the buffer has enough space in the specified area, `false` if not
    bool HasContiguousSpace(size_t start, size_t size, size_t tail, size_t *outStart = nullptr) const {
        if (outStart != nullptr) {
            *outStart = start;
        }
        if (tail <= start) {
            // Free region is [start, m_size) and [0, tail)
            size_t beforeWrap = m_size - start;
            if (size <= beforeWrap) {
                return true;
            }
            if (outStart != nullptr) {
                *outStart = 0;
            }
            return size <= tail;
        } else {
            // Free region is [start, tail)
            return (start + size) <= tail;
        }
    }

    /// @brief Aligns the value up to the specified alignment.
    /// @param[in] value the value to align
    /// @param[in] alignment the desired alignment, which must be a power of two.
    /// @return the value, aligned up to the specified alignment
    size_t Align(size_t value, size_t alignment) {
        assert(bit::is_power_of_two(alignment));
        return (value + alignment - 1) & ~(alignment - 1);
    }
};

/// @brief Inserts memory barriers between dependent GPU commands.
///
/// Every command recorded through the renderer's helpers marks the tracker as pending. `Flush` then emits one global
/// memory barrier covering compute and transfer work, which orders the pending commands before any following ones.
/// The Direct3D 12 renderer flushes its barrier tracker before every command that depends on previous work, so
/// flushing at the same points gives the same guarantees.
class BarrierTracker {
public:
    /// @brief Marks that a command was recorded since the last barrier.
    void MarkPending() {
        m_pending = true;
    }

    /// @brief Emits a barrier if any commands were recorded since the last one.
    /// @param[in] cmd the command buffer
    void Flush(VkCommandBuffer cmd) {
        if (!m_pending || cmd == VK_NULL_HANDLE) {
            return;
        }
        m_pending = false;
        const VkMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        static constexpr VkPipelineStageFlags kStages =
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        vkCmdPipelineBarrier(cmd, kStages, kStages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

private:
    bool m_pending = false;
};

// ---------------------------------------------------------------------------------------------------------------------

struct VulkanVDPRenderer::Impl {
    Impl(const VulkanRendererCallbacks &hwCallbacks, VDPState &state,
         const config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig,
         const config::VDP2DebugRender &vdp2DebugRenderOptions, const config::Enhancements &enhancements,
         uint32 resolutionScale)
        : vdpState(state)
        , enhancements(enhancements)
        , hwCallbacks(hwCallbacks)
        , resolutionScale(resolutionScale)
        , vdp2(vdp2AccessPatternsConfig, vdp2DebugRenderOptions) {}

    ~Impl() {
        Shutdown();
    }

    // Declared first so that it is destroyed after every other Vulkan object
    VulkanDevice vk;

    VDPState &vdpState;
    const config::Enhancements &enhancements;

    const VulkanRendererCallbacks &hwCallbacks;

    /// @brief Internal resolution scale factor. 1 renders at the native resolution.
    const uint32 resolutionScale;

    /// @brief Retrieves the size of a single VDP1 framebuffer scaled by the internal resolution.
    /// @return the size in bytes of a scaled VDP1 framebuffer
    size_t ScaledFBSize() const {
        return kVDP1FBRAMSize * resolutionScale * resolutionScale;
    }

    // =================================================================================================================
    // Common rendering parameters

    struct EnhancementsParams {
        HLSLuint deinterlace : 1;       //     0  Deinterlace
        HLSLuint transparentMeshes : 1; //     1  Render mesh sprites as transparent
        HLSLuint resolutionScale : 3;   //   2-4  Internal resolution scale minus one
    };
    static_assert(sizeof(EnhancementsParams) == sizeof(HLSLuint));

    // =================================================================================================================
    // VDP1 rendering
    //
    // The VDP1 rendering pipeline is submitted once per framebuffer swap.
    //
    // VDP1 erase dispatched done only once per swap.
    // Each VDP1 command is dispatched individually, sometimes multiple times depending on the number of lines in the
    // polygon.
    // CPU writes to FBRAM are handled at swap time.
    // CPU reads from FBRAM force a synchronization point (fence wait) for the most recent VDP1 frame.
    //
    // Root 32-bit constants hold renderer parameters shared across all VDP1 compute shaders such as relevant registers
    // and active enhancements, as well as per-shader parameters.

    /// @brief Common VDP1 rendering parameters shared by all shaders.
    struct alignas(16) VDP1CommonRenderParams {
        struct DisplayParams {                 //  bits  use
            HLSLuint fbSizeH : 1;              //     0  Framebuffer horizontal size shift    (512 << x)
            HLSLuint fbSizeV : 1;              //     1  Framebuffer vertical size shift      (256 << x)
            HLSLuint pixel8Bits : 1;           //     2  Pixel data size                      0=16 bits; 1=8 bits
            HLSLuint doubleDensity : 1;        //     3  Double-density interlace mode
            HLSLuint dblInterlaceEnable : 1;   //     4  Double interlace enable
            HLSLuint dblInterlaceDrawLine : 1; //     5  Double interlace line                0=even; 1=odd
            HLSLuint evenOddCoordSelect : 1;   //     6  Even/odd coordinate select (HSS)     0=even; 1=odd
            HLSLuint drawFB : 1;               //     7  Current draw framebuffer index
        } displayParams;
        static_assert(sizeof(DisplayParams) == sizeof(HLSLuint));

        EnhancementsParams enhancements;
    };

    /// @brief VDP1 erase parameters, appended to common rendering parameters in the erase shader.
    struct alignas(16) VDP1EraseParams {
        struct Coords {          //  bits  use
            HLSLuint x1 : 6;     //   0-5  Erase X1 (left) coordinate (x << 3)
            HLSLuint y1 : 9;     //  6-14  Erase Y1 (top) coordinate
            HLSLuint x3 : 7;     // 15-21  Erase X3 (right) coordinate (x << 3)
            HLSLuint y3 : 9;     // 22-30  Erase Y3 (bottom) coordinate
            HLSLuint scaleV : 1; //    31  Erase Y coordinate shift
        } coords;
        static_assert(sizeof(Coords) == sizeof(HLSLuint));

        struct Erase {                 //  bits  use
            HLSLuint writeValue : 16;  //  0-15  Erase write value
            HLSLuint addressShift : 1; //    16  Erase address shift (coordY << (x + 8))
        } erase;

        struct VBlankEraseParams { //  bits  use
            HLSLuint enable : 1;   //     0  VBlank erase active
            HLSLuint maxY : 9;     //   1-9  Last VBlank erase line
            HLSLuint maxX : 10;    // 10-19  Last VBlank erase pixel in line
        } vblank;
        static_assert(sizeof(VBlankEraseParams) == sizeof(HLSLuint));
    };

    /// @brief VDP1 polygon drawing parameters, appended to common rendering parameters in the polygon drawing shader.
    struct alignas(16) VDP1PolyDrawParams {
        HLSLuint numSpans; // Number of spans in the list
    };

    /// @brief VDP1 span drawing parameters.
    struct VDP1SpanParams {
        HLSLint2 coord0;        // Starting coordinates
        HLSLint2 coord1;        // Ending coordinates
        HLSLuint skip : 16;     // Initial skip steps
        HLSLuint cmdIndex : 16; // Command parameters index

        struct Attributes {             //  bits  use
            HLSLuint antialias : 1;     //     0  Antialias line
            HLSLuint textured : 1;      //     1  Textured polygon
            HLSLuint flipH : 1;         //     2  Horizontal flip
                                        // --- texture only ---
            HLSLuint texV : 10;         //  3-12  Texture V coordinate
            HLSLuint endCodeIndex : 10; // 13-22  U coordinate of the second end code
        } attrs;
        static_assert(sizeof(Attributes) == sizeof(HLSLuint));

        // Gouraud only parameters
        HLSLuint3 gouraud0; // Starting gouraud value
        HLSLuint3 gouraud1; // Ending gouraud value
    };

    /// @brief VDP1 command parameters. Referenced by spans.
    struct VDP1CommandParams {
        struct SysClip {     //  bits  use
            HLSLuint h : 16; //  0-15  System clipping area width
            HLSLuint v : 16; // 16-31  System clipping area height
        } sysClip;
        static_assert(sizeof(SysClip) == sizeof(HLSLuint));

        struct UserClip {    //  bits  use
            HLSLuint x : 16; //  0-15  User clipping area horizontal coordinate
            HLSLuint y : 16; // 16-31  User clipping area vertical coordinate
        };
        static_assert(sizeof(UserClip) == sizeof(HLSLuint));
        UserClip userClip0; // User clipping area top-left coordinate
        UserClip userClip1; // User clipping area bottom-right coordinate

        HLSLuint cmdpmod : 16; // CMDPMOD value
        HLSLuint cmdcolr : 16; // CMDCOLR value
        HLSLuint cmdsize : 16; // CMDSIZE value (textured only)
        HLSLuint cmdsrca : 16; // CMDSRCA value (textured only)
    };

    /// @brief A fragment used by the order-independent transparency algorithm.
    struct VDP1OITFragment {
        HLSLuint data;
        HLSLuint next;
    };

    /// @brief An FBRAM write entry.
    struct VDP1FBRAMWrite {
        HLSLuint address;
        HLSLuint andMask;
        HLSLuint orMask;
    };

    /// @brief Maximum number of spans to send per batch.
    static constexpr uint32 kMaxVDP1Spans = 65535;

    /// @brief Maximum number of commands to send per batch.
    static constexpr uint32 kMaxVDP1Commands = 4096;

    /// @brief Maximum number of non-OIT fragments per dispatch.
    static constexpr uint32 kMaxVDP1FragmentsPerDispatch = 4194240;

    /// @brief Maximum number of OIT fragments per dispatch.
    static constexpr uint32 kMaxVDP1OITFragmentsPerDispatch = 524288;

    // The polygon drawing shader uses the span index as a sequence number to enable parallel drawing.
    // This sequence has to fit in the top 16 bits of the output value, limiting the number of span drawn per
    // dispatch. We reserve zero as a special value indicating the previous dispatch's contents (or empty pixels).
    // Therefore, the absolute maximum number of spans that can be submitted per dispatch is 65535.
    static_assert(kMaxVDP1Spans <= 65535);

    // The command index is a 16-bit number packed into one of the VDP1 spans fields.
    static_assert(kMaxVDP1Commands <= 65535);

    // The absolute maximum limit for pixels per dispatch is dictated by the maximum number of compute dispatch groups.
    // Each group has 64 threads, as defined in the polygon drawing shader.
    static_assert(kMaxVDP1FragmentsPerDispatch <= 65535 * 64);
    static_assert(kMaxVDP1OITFragmentsPerDispatch <= 65535 * 64);

    /// @brief Tracks memory usage with a generation map.
    /// @tparam memorySize total size of the memory area to track
    /// @tparam blockSizeBits splits the memory into blocks with 2^blockSizeBits bytes
    template <size_t memorySize, unsigned blockSizeBits = 5>
    class MemoryUsageTracker {
        static constexpr uint32 kBlockSize = 1u << blockSizeBits;
        static constexpr uint32 kArraySize = memorySize >> blockSizeBits;

    public:
        /// @brief Clears usage across the whole memory.
        FORCE_INLINE void Clear() {
            // Clear the whole map if we wrap around.
            if (++m_currGen == 0) {
                std::fill(m_usage.begin(), m_usage.end(), 0u);
                m_currGen = 1;
            }
        }

        /// @brief Marks the specified range of bytes as in use.
        /// @param[in] address the base address
        /// @param[in] size the length of the range
        FORCE_INLINE void MarkRange(uint32 address, uint32 size) {
            // Zero-sized textures still read one texel, and texture reads wrap around the end of the memory
            size = std::clamp<uint32>(size, 1u, memorySize);
            const uint32 first = (address % memorySize) >> blockSizeBits;
            const uint32 count = ((((address % memorySize) & (kBlockSize - 1)) + size - 1) >> blockSizeBits) + 1;
            for (uint32 i = 0; i < std::min(count, kArraySize); ++i) {
                m_usage[(first + i) % kArraySize] = m_currGen;
            }
        }

        /// @brief Checks if a particular address is marked as in use.
        /// @param[in] address the address to check
        /// @return `true` if the address is marked, `false` if not.
        FORCE_INLINE bool IsInUse(uint32 address) const {
            return m_usage[(address % memorySize) >> blockSizeBits] == m_currGen;
        }

    private:
        std::array<uint32, kArraySize> m_usage{};
        uint32 m_currGen = 1u;
    };

    struct VDP1Resources {
        VDP1Resources() {
            memset(&cpuCommonRenderParams, 0, sizeof(cpuCommonRenderParams));
            memset(&cpuEraseParams, 0, sizeof(cpuEraseParams));
            memset(&cpuPolyDrawParams, 0, sizeof(cpuPolyDrawParams));
        }

        // VDP1 VRAM is exposed as a ByteAddressBuffer to shaders as they often need to access raw bytes in 8-bit and
        // 16-bit units.

        /// @brief VRAM buffer.
        GpuBuffer vramBuffer;

        /// @brief Bit shift for the granularity for VRAM dirty bitmap chunks.
        static constexpr size_t kVRAMDirtyBitmapChunkSizeShift = 8;

        /// @brief Granularity for VRAM dirty bitmap chunks, in bytes.
        static constexpr size_t kVRAMDirtyBitmapChunkSize = static_cast<size_t>(1) << kVRAMDirtyBitmapChunkSizeShift;

        /// @brief Number of bits in the VRAM dirty bitmap.
        static constexpr size_t kVRAMDirtyBitmapSize = kVDP1VRAMSize / kVRAMDirtyBitmapChunkSize;

        // Buffer transfers must be done in multiples of 4 bytes.
        // The chunk must not be larger than VDP1 VRAM itself. In fact, it shouldn't be too large as it wastes memory
        // and time with unnecessary copies of VRAM data.
        static_assert(kVRAMDirtyBitmapChunkSize >= sizeof(uint32) && kVRAMDirtyBitmapChunkSize <= kVDP1VRAMSize,
                      "VDP1 VRAM upload chunk size is out of range");

        /// @brief VRAM dirty bitmap.
        util::DirtyBitmap<kVRAMDirtyBitmapSize> vramDirty;
        /// @brief Tracks VRAM usage by textures in a batch of spans.
        MemoryUsageTracker<kVDP1VRAMSize> vramTexUsageTracker;

        // The VDP1 FBRAM buffer contains four FBRAM-sized buffers to accomodate the outputs of the enhancements.
        // The buffers are indexed as follows:
        //   [0] Main field
        //   [1] Alternate field (deinterlace)
        //   [2] Main mesh field
        //   [3] Alternate mesh field

        /// @brief FBRAM buffer.
        GpuBuffer fbramBuffer;

        /// @brief FBRAM dirty bitmap.
        util::DirtyBitmap<kVDP1FBRAMSize> fbramDirty;
        /// @brief FBRAM writes buffer.
        GpuBuffer fbramWritesBuffer;

        /// @brief FBRAM download buffer.
        GpuBuffer fbramDownloadBuffer;
        /// @brief Current version of downloaded FBRAM.
        /// @brief Set when GPU work may have modified FBRAM since it was last copied to the download buffer.
        bool fbramReadbackDirty = false;
        /// @brief Set when a copy of FBRAM to the download buffer was recorded but not yet read by the CPU.
        bool fbramReadbackCopyPending = false;
        /// @brief Set by `VDP1DebugSyncFB` to lazily sync FBRAM when convenient.
        std::atomic_bool fbramDebugSyncRequest{false};

        // ---------------------------------------------------------------------

        /// @brief Common rendering parameters, uploaded as 32-bit root constants.
        VDP1CommonRenderParams cpuCommonRenderParams{};

        /// @brief Erase parameters, uploaded as 32-bit root constants.
        VDP1EraseParams cpuEraseParams{};

        /// @brief Polygon drawing parameters, uploaded as 32-bit root constants.
        VDP1PolyDrawParams cpuPolyDrawParams{};

        /// @brief Root signature for writing to the framebuffer.
        ComputeLayout fbramWriteRootSig;
        /// @brief Descriptor range for writing to the framebuffer.
        VkDescriptorSet fbramWriteSet = VK_NULL_HANDLE;
        /// @brief Pipeline state object for writing to the framebuffer.
        UniquePipeline fbramWritePSO;

        /// @brief Root signature for erasing the framebuffer.
        ComputeLayout eraseRootSig;

        // The polygon drawing shader operates on consecutive polygons span batches that share the same properties:
        // - System and user clipping areas
        // - CMDPMOD, CMDCOLR, CMDSRCA and CMDSIZE values
        // - Shader specializations:
        //   - Checkerboard vs. transparent meshes
        //   - Color blending mode:
        //     - Copy: Replace and Half-Luminance modes
        //     - Right shift: Shadow mode
        //     - OIT: Half-Transparency mode
        //     - MSB: MSB mode

        /// @brief Root signature for drawing polygons.
        /// Applies to all non-OIT variants of the polygon drawing shader.
        ComputeLayout polyDrawRootSig;
        /// @brief Root signature for drawing polygons.
        /// Applies to the OIT variant of the polygon drawing shader.
        ComputeLayout polyDrawOITRootSig;

        // The polygon output merger shader applies the output of the polygon drawing shader to FBRAM, operating on
        // 32-bit values at a time.
        // Shader specializations:
        // - Checkerboard vs. transparent meshes
        // - Merging mode:
        //   - Copy: Replace and Half-Luminance modes
        //   - Right shift: Shadow mode
        //   - OIT: Half-Transparency mode
        // TODO: OIT might need a dedicated root signature

        /// @brief Root signature for merging polygon outputs (non-OIT variants).
        ComputeLayout outputMergerRootSig;
        /// @brief Root signature for merging polygon outputs (OIT variant only).
        ComputeLayout outputMergerOITRootSig;

        /// @brief Root signature for drawing polygons (MSB variants).
        ComputeLayout polyDrawMSBRootSig;

        /// @brief Native FBRAM contents downsampled from the scaled framebuffers (internal resolution scaling only).
        GpuBuffer fbramNativeBuffer;
        /// @brief Root signature for downsampling the framebuffers.
        ComputeLayout fbramDownsampleRootSig;
        /// @brief Pipeline for downsampling the framebuffers.
        UniquePipeline fbramDownsamplePSO;
        /// @brief Descriptor set for downsampling the framebuffers.
        VkDescriptorSet fbramDownsampleSet = VK_NULL_HANDLE;

        /// @brief Descriptor set for erasing the framebuffer.
        VkDescriptorSet eraseSet = VK_NULL_HANDLE;
        /// @brief Pipeline for erasing the framebuffer.
        UniquePipeline erasePSO;
        /// @brief Pipelines for drawing polygons.
        std::array<UniquePipeline, 2 * 4> polyDrawPSOs;
        /// @brief Pipelines for the output merger.
        std::array<UniquePipeline, 2 * 3> outputMergerPSOs;

        // ---------------------------------------------------------------------
        // Rendering state

        // Currently active polygon drawing shader
        size_t currPolyDrawShaderIndex = -1;
        // Currently active output merger shader
        size_t currOutputMergerShaderIndex = -1;
        // Set to true if any transparent mesh polygon was drawn
        bool transparentMeshDrawn = false;
        // Whether to double vertical coordinates when drawing VDP1 sprites
        bool doubleV = false;
    } vdp1;

    /// @brief Constructs a polygon drawing shader index from its variant options.
    /// @param[in] mode polygon drawing mode
    /// @return the shader index
    size_t MakeVDP1PolyDrawShaderIndex(VDP1Command::DrawMode mode) const {
        size_t value = 0;
        bit::deposit_into<0>(value, enhancements.transparentMeshes);
        if (mode.msbOn) {
            // MSB -> mode 3 (MSB)
            bit::deposit_into<1, 2>(value, 3);
        } else if (vdpState.regs1.pixel8Bits) {
            // 8-bit sprite data -> mode 0 (Copy) -- shading modes not supported
            bit::deposit_into<1, 2>(value, 0);
        } else if (mode.colorCalcBits == 3) {
            // Half-Transparency -> mode 2 (OIT)
            bit::deposit_into<1, 2>(value, 2);
        } else if (mode.colorCalcBits == 1) {
            // Shadow -> mode 1 (Shift)
            bit::deposit_into<1, 2>(value, 1);
        }
        // Other modes -> mode 0 (Copy)
        return value;
    }

    struct PolyDrawShaderIndex {
        size_t meshMode;
        size_t shadingMode;
    };

    /// @brief Expands a bit-packed VDP1 polygon drawing shader index into its components.
    /// @param[in] index the shader index
    /// @return the index's components
    PolyDrawShaderIndex ExpandVDP1PolyDrawShaderIndex(size_t index) {
        return {
            .meshMode = bit::extract<0>(index),
            .shadingMode = bit::extract<1, 2>(index),
        };
    }

    /// @brief Determines if the given polygon drawing shader variant uses OIT shading mode.
    /// @param[in] index the shader index
    /// @return `true` if the shader uses OIT shading mode, `false` otherwise
    bool IsVDP1PolyDrawShaderOIT(size_t index) {
        return bit::extract<1, 2>(index) == 2;
    }

    /// @brief Determines if the given polygon drawing shader variant uses MSB shading mode.
    /// @param[in] index the shader index
    /// @return `true` if the shader uses MSB shading mode, `false` otherwise
    bool IsVDP1PolyDrawShaderMSB(size_t index) {
        return bit::extract<1, 2>(index) == 3;
    }

    /// @brief Retrieves the name of a polygon drawing shader variant.
    /// @param[in] index the shader index
    /// @return the shader variant name of the form "Ms#-Sh#"
    std::string GetVDP1PolyDrawShaderVariantName(size_t index) {
        const PolyDrawShaderIndex components = ExpandVDP1PolyDrawShaderIndex(index);
        return fmt::format("Ms{}-Sh{}", components.meshMode, components.shadingMode);
    }

    /// @brief Constructs an output merger shader index from its variant options.
    /// @param[in] mode polygon drawing mode
    /// @return the shader index
    size_t MakeVDP1OutputMergerShaderIndex(VDP1Command::DrawMode mode) const {
        size_t value = 0;
        bit::deposit_into<0>(value, enhancements.transparentMeshes);
        if (vdpState.regs1.pixel8Bits) {
            // 8-bit sprite data -> mode 0 (Copy) -- shading modes not supported
            bit::deposit_into<1, 2>(value, 0);
        } else if (mode.colorCalcBits == 1) {
            // Shadow -> mode 1
            bit::deposit_into<1, 2>(value, 1);
        } else if (mode.colorCalcBits == 3) {
            // Half-Luminance -> mode 2
            bit::deposit_into<1, 2>(value, 2);
        }
        // Other modes -> mode 0
        // MSB doesn't invoke this shader.
        return value;
    }

    struct OutputMergerShaderIndex {
        size_t meshMode;
        size_t mergeMode;
    };

    /// @brief Expands a bit-packed VDP1 output merger shader index into its components.
    /// @param[in] index the shader index
    /// @return the index's components
    OutputMergerShaderIndex ExpandVDP1OutputMergerShaderIndex(size_t index) {
        return {
            .meshMode = bit::extract<0>(index),
            .mergeMode = bit::extract<1, 2>(index),
        };
    }

    /// @brief Determines if the given output merger shader variant uses OIT merging mode.
    /// @param[in] index the shader index
    /// @return `true` if the shader uses OIT merging mode, `false` otherwise
    bool IsVDP1OutputMergerShaderOIT(size_t index) {
        return bit::extract<1, 2>(index) == 2;
    }

    /// @brief Retrieves the name of an output merger shader variant.
    /// @param[in] index the shader index
    /// @return the shader variant name of the form "Ms#-Mg#"
    std::string GetVDP1OutputMergerShaderVariantName(size_t index) {
        const OutputMergerShaderIndex components = ExpandVDP1OutputMergerShaderIndex(index);
        return fmt::format("Ms{}-Mg{}", components.meshMode, components.mergeMode);
    }

    // =================================================================================================================
    // VDP2 rendering
    //
    // The VDP2 rendering pipeline is invoked at least once per frame. When VRAM, CRAM and/or register writes happen,
    // the renderer processes scanlines up to the previous VCNT and commits all changes before proceeding.
    //
    // Since the VDP2 rendering process has no visible effect on the rest of the Saturn's components, there is no need
    // for additional synchronization constraints on memory and register reads or writes. The VDP2 state is handled by
    // the VDP controller, and the renderer maintains a local independent copy of the VRAM, CRAM and VDP2 registers for
    // fully asynchronous rendering.
    //
    // Root 32-bit constants hold renderer parameters shared across all VDP2 compute shaders such as the starting line
    // for continuation of work interrupted by state changes, relevant registers and active enhancements.

    /// @brief Common VDP2 rendering parameters shared by all shaders.
    struct VDP2CommonRenderParams {
        // Top Y coordinate of target rendering area.
        HLSLuint startY;

        struct DisplayParams {                 //  bits  use
            HLSLuint displayEnable : 1;        //     0  Display enabled
                                               //          0 = display off
                                               //          1 = display on
            HLSLuint borderColorMode : 1;      //     1  Border color mode
                                               //          0 = black
                                               //          1 = back screen color
            HLSLuint interlaceMode : 2;        //   2-3  Interlace mode
                                               //          0 = progressive
                                               //          1 = invalid
                                               //          2 = single-density interlace
                                               //          3 = double-density interlace
            HLSLuint oddField : 1;             //     4  Field
                                               //          0 = even
                                               //          1 = odd
            HLSLuint exclusiveMonitor : 1;     //     5  Exclusive monitor mode
                                               //          0 = normal
                                               //          1 = exclusive
            HLSLuint colorRAMMode : 2;         //   6-7  Color RAM mode
                                               //          0 = RGB 5:5:5, 1024 words
                                               //          1 = RGB 5:5:5, 2048 words
                                               //          2 = RGB 8:8:8, 1024 words
                                               //          3 = RGB 8:8:8, 1024 words  (same as mode 2, undocumented)
            HLSLuint hiResH : 1;               //     8  Horizontal resolution
                                               //          0 = 320/352
                                               //          1 = 640/704
            HLSLuint palMode : 1;              //     9  Display standard (VDP2 TVSTAT.PAL)
                                               //          0 = NTSC
                                               //          1 = PAL
            HLSLuint hresMode : 3;             // 10-12  Horizontal resolution mode (VDP2 TVMD.HRESO2-0)
            HLSLuint vresMode : 2;             // 13-14  Vertical resolution mode   (VDP2 TVMD.VRESO1-0)
            HLSLuint dblInterlaceEnable : 1;   //    15  VDP1 double interlace enable flag (VDP1 FBCR.DIE)
            HLSLuint dblInterlaceDrawLine : 1; //    16  VDP1 double interlace draw line   (VDP1 FBCR.DIL)
        } displayParams;
        static_assert(sizeof(DisplayParams) == sizeof(HLSLuint));

        struct LayerParams {                   //  bits  use
            HLSLuint layerEnabled : 6;         //   0-5  Layer enable state based on BGON and other factors:
                                               //        bit  RBG0+RBG1   RBG0        RBG1        no RBGs
                                               //          0  Sprite      Sprite      Sprite      Sprite
                                               //          1  RBG0        RBG0        -           -
                                               //          2  RBG1        NBG0        RBG1        NBG0
                                               //          3  EXBG        NBG1/EXBG   NBG1/EXBG   NBG1/EXBG
                                               //          4  -           NBG2        NBG2        NBG2
                                               //          5  -           NBG3        NBG3        NBG3
            HLSLuint bgEnabled : 6;            //  6-11  Individual layer enable flags
                                               //        bit  layer
                                               //          8  NBG0
                                               //          9  NBG1
                                               //         10  NBG2
                                               //         11  NBG3
                                               //         12  RBG0
                                               //         13  RBG1
            HLSLuint lineColorEnableRBG0 : 1;  //    12  Line color screen enable for RBG0
            HLSLuint lineColorEnableRBG1 : 1;  //    13  Line color screen enable for RBG1
            HLSLuint mosaicH : 4;              // 14-17  Horizontal mosaic size (minus one)
            HLSLuint mosaicV : 4;              // 18-21  Vertical mosaic size (minus one)
            HLSLuint rotParamMode : 2;         // 22-23  Rotation parameter mode
                                               //          0 = always use A
                                               //          1 = always use B
                                               //          2 = select based on coefficient data
                                               //          3 = select based on window flag
            HLSLuint restrictedColorCalc : 1;  //    24  Restricted color calculations
            HLSLuint extendedColorCalc : 1;    //    25  Use extended color calculation
                                               //        (always disabled in hi-res modes)
            HLSLuint useAdditiveBlend : 1;     //    26  Blend mode
                                               //          0 = alpha
                                               //          1 = additive
            HLSLuint useSecondScreenRatio : 1; //    27  Use second screen ratio
            HLSLuint colorGradEnable : 1;      //    28  Color gradation enabled
            HLSLuint colorGradScreen : 3;      // 29-31  Color gradation screen
                                               //          0  Sprite
                                               //          1  RBG0
                                               //          2  NBG0/RBG1
                                               //          3  (invalid)
                                               //          4  NBG1/EXBG
                                               //          5  NBG2
                                               //          6  NBG3
                                               //          7  (invalid)
        } layerParams;
        static_assert(sizeof(LayerParams) == sizeof(HLSLuint));

        struct RotParams {                       //  bits  use
            HLSLuint coeffTableCRAM : 1;         //     0  Coefficient table location
                                                 //          0 = VRAM
                                                 //          1 = CRAM
            HLSLuint coeffDataAccess : 4;        //   1-4  Coefficient data access for VRAM banks:
                                                 //         bit  bank
                                                 //           0  A0/A
                                                 //           1  A1
                                                 //           2  B0/B
                                                 //           3  B1
            HLSLuint coeffDataPerDot : 1;        //     5  Per-dot coefficients
                                                 //          false = per line
                                                 //          true  = per dot
                                                 // ------------------------------------------------
            HLSLuint coeffATableEnable : 1;      //     6  Coefficient table A enabled
            HLSLuint coeffADataSize : 1;         //     7  Coefficient A data size
                                                 //          0 = 2 words
                                                 //          1 = 1 word
            HLSLuint coeffADataMode : 2;         //   8-9  Coefficient A data mode
                                                 //          0 = kx/ky
                                                 //          1 = kx
                                                 //          2 = ky
                                                 //          3 = Px
            HLSLuint coeffAUseLineColorData : 1; //    10  Use coefficient A line color data
                                                 // ------------------------------------------------
            HLSLuint coeffBTableEnable : 1;      //    11  Coefficient table B enabled
            HLSLuint coeffBDataSize : 1;         //    12  Coefficient B data size
                                                 //          0 = 2 words
                                                 //          1 = 1 word
            HLSLuint coeffBDataMode : 2;         // 13-14  Coefficient B data mode
                                                 //          0 = kx/ky
                                                 //          1 = kx
                                                 //          2 = ky
                                                 //          3 = Px
            HLSLuint coeffBUseLineColorData : 1; //    15  Use coefficient B line color data
        } rotParams;
        static_assert(sizeof(RotParams) == sizeof(HLSLuint));

        struct SpriteParams {             //  bits  use
            HLSLuint rotate : 1;          //     0  Sprite layer rotation
                                          //          0 = normal
                                          //          1 = use rotation parameter A
            HLSLuint pixel8Bits : 1;      //     1  VDP1 data size
                                          //          0 = 16-bit
                                          //          1 = 8-bit
            HLSLuint type : 4;            //   2-5  Sprite data type
            HLSLuint fbSizeH : 1;         //     6  VDP1 framebuffer horizontal size shift  (512 << x)
            HLSLuint fbSizeV : 1;         //     7  VDP1 framebuffer vertical size shift    (256 << x)
            HLSLuint inHalfResH : 1;      //     8  Sprite input at half resolution
            HLSLuint outHalfResH : 1;     //     9  Sprite output at half resolution
            HLSLuint mixedFormat : 1;     //    10  Sprite layer color format
                                          //          0 = palette only
                                          //          1 = mixed palette/RGB
            HLSLuint colorCalcEnable : 1; //    11  Sprite color calculation enable
            HLSLuint colorCalcValue : 3;  // 12-14  Sprite target color calculation value
            HLSLuint colorCalcCond : 2;   // 15-16  Special color calculation condition
            HLSLuint colorDataOffset : 3; // 17-19  Special color data offset in CRAM
            HLSLuint useSpriteWindow : 1; //    20  Use sprite window
            HLSLuint windowEnabled : 1;   //    21  Sprite window enabled for the sprite layer
            HLSLuint windowInverted : 1;  //    22  Sprite window inverted for the sprite layer
            HLSLuint displayFB : 1;       //    23  Current sprite display framebuffer index
        } spriteParams;
        static_assert(sizeof(SpriteParams) == sizeof(HLSLuint));

        // Packed 8x 3-bit sprite priorities + 5-bit color calculation ratios
        HLSLuint2 spritePriosRatios;

        struct VCellScroll {            //  bits  use
            HLSLuint tableAddress : 19; //  0-18  Vertical cell scroll table address
            HLSLuint inc : 3;           // 19-21  Vertical cell scroll address increment per cell  (x << 2)
        } vcellScroll;
        static_assert(sizeof(VCellScroll) == sizeof(HLSLuint));

        struct Windows {                       //  bits  use
            HLSLuint spriteWindowLogic : 1;    //     0  Sprite window logic        0=OR; 1=AND
            HLSLuint spriteW0Enable : 1;       //     1  Sprite W0 enable           0=disable; 1=enable
            HLSLuint spriteW0Invert : 1;       //     2  Sprite W0 invert           0=disable; 1=enable
            HLSLuint spriteW1Enable : 1;       //     3  Sprite W1 enable           0=disable; 1=enable
            HLSLuint spriteW1Invert : 1;       //     4  Sprite W1 invert           0=disable; 1=enable
            HLSLuint colorCalcWindowLogic : 1; //     5  Color calc. window logic   0=OR; 1=AND
            HLSLuint colorCalcW0Enable : 1;    //     6  Color calc. W0 enable      0=disable; 1=enable
            HLSLuint colorCalcW0Invert : 1;    //     7  Color calc. W0 invert      0=disable; 1=enable
            HLSLuint colorCalcW1Enable : 1;    //     8  Color calc. W1 enable      0=disable; 1=enable
            HLSLuint colorCalcW1Invert : 1;    //     9  Color calc. W1 invert      0=disable; 1=enable
            HLSLuint colorCalcSWEnable : 1;    //    10  Color calc. SW enable      0=disable; 1=enable
            HLSLuint colorCalcSWInvert : 1;    //    11  Color calc. SW invert      0=disable; 1=enable
        } windows;
        static_assert(sizeof(Windows) == sizeof(HLSLuint));

        EnhancementsParams enhancements;
    };

    /// @brief Global VDP2 window parameters.
    struct VDP2GlobalWindowParams {
        // Top-left coordinates
        HLSLuint2 start;

        // Bottom-right coordinates
        HLSLuint2 end;

        // Base address of the line window table
        HLSLuint lineWindowTableAddress;

        // Whether to use the line window table
        HLSLbool lineWindowTableEnable;
    };

    /// @brief Per-layer VDP2 window parameters.
    struct VDP2LayerWindowParams {
        // Window logic
        //   false = OR
        //   true = AND
        HLSLbool windowLogicAnd;

        // Window 0 enable
        HLSLbool window0Enable;

        // Window 0 invert
        HLSLbool window0Invert;

        // Window 1 enable
        HLSLbool window1Enable;

        // Window 1 invert
        HLSLbool window1Invert;
    };

    /// @brief VDP2 extended window parameters (includes sprite window).
    struct VDP2LayerWindowParamsS {
        VDP2LayerWindowParams base;

        // Sprite window enable
        HLSLbool spriteWindowEnable;

        // Sprite window invert
        HLSLbool spriteWindowInvert;
    };

    /// @brief Base VDP2 layer rendering parameters, common to NBGs and RBGs.
    struct VDP2BaseBGParams {
        // Background enabled
        HLSLbool enabled;

        // If true, honor transparency bit in color data.
        // Derived from BGON.xxTPON
        HLSLbool enableTransparency;

        // Whether the background uses cells (false) or a bitmap (true).
        // Derived from CHCTLA/CHCTLB.xxBMEN
        HLSLbool bitmap;

        // Priority number from 0 (transparent) to 7 (highest).
        // Derived from PRINA/PRINB/PRIR.xxPRINn
        HLSLuint priorityNumber;

        // Special priority mode.
        // Derived from SFPRMD.xxSPRMn
        HLSLuint priorityMode;

        // Special function select (0=A, 1=B).
        // Derived from SFSEL.xxSFCS
        HLSLuint specialFunctionSelect;

        // Cell size shift corresponding to the dimensions of a character pattern (0=1x1, 1=2x2).
        // Derived from CHCTLA/CHCTLB.xxCHSZ
        HLSLuint cellSizeShift;

        // Character color format.
        // Derived from CHCTLA/CHCTLB.xxCHCNn
        HLSLuint colorFormat;

        // Color RAM base offset.
        // Derived from CRAOFA/CRAOFB.xxCAOSn
        HLSLuint cramOffset;

        // Supplementary bits 4-0 for scroll screen character number, when using 1-word characters.
        // Derived from PNCNn/PNCR.xxSCNn
        HLSLuint supplScrollCharNum;

        // Supplementary bits 6-4 for palette number.
        // Used with scroll screen when using 1-word characters, or with bitmap screens.
        // The value is already shifted in place to optimize rendering calculations.
        // Derived from PNCNn/PNCR.xxSPLTn (scroll) or BMPNA/BMPNB.xxBMPn (bitmap)
        HLSLuint supplPalNum;

        // Supplementary special color calculation bit.
        // Derived from PNCNn/PNCR.xxSCC (scroll) or BMPNA/BMPNB.xxBMCC (bitmap)
        HLSLuint supplSpecialColorCalc;

        // Supplementary special priority bit.
        // Derived from PNCNn/PNCR.xxSPR (scroll) or BMPNA/BMPNB.xxBMPR (bitmap)
        HLSLuint supplSpecialPriority;

        // Enables the mosaic effect.
        // If vertical cell scroll is also enabled, the mosaic effect is bypassed.
        // Derived from MZCTL.xxMZE
        HLSLbool mosaicEnable;

        // Enables color calculation.
        // Derived from CCCTL.xxCCEN
        HLSLbool colorCalcEnable;

        // Character number width: 10 bits (false) or 12 bits (true).
        // When true, disables the horizontal and vertical flip bits in the character.
        // Derived from PNCNn/PNCR.xxCNSM
        HLSLbool extChar;

        // Whether characters use one (false) or two (true) words.
        // Derived from PNCNn/PNCR.xxPNB
        HLSLbool twoWordChar;

        // Whether pattern name data is accessible for each VRAM bank (bits 0 to 3: A0, A1, B0, B1).
        // Derived from CYCxn, RAMCTL and BGON (for RBG0/1 restrictions to NBGs)
        HLSLuint patNameAccess;

        // Whether character pattern data is accessible for each VRAM bank (bits 0 to 3: A0, A1, B0, B1).
        // Derived from CYCxn, RAMCTL and BGON (for RBG0/1 restrictions to NBGs)
        HLSLuint charPatAccess;

        // Whether accesses to character pattern data for this background is delayed per bank due to illegal VRAM access
        // patterns (bits 0 to 3: A0, A1, B0, B1). Derived from CYCxn, RAMCTL, ZMCTL and CHCTLA/CHCTLB.xxCHSZ
        HLSLuint charPatDelay;

        // Address offset for VRAM data for this background on each VRAM bank caused by illegal VRAM access patterns
        // (bits 0 to 3: A0, A1, B0, B1). Derived from CYCxn, RAMCTL and ZMCTL
        HLSLuint vramDataOffset;

        // Special color calculation mode.
        // Derived from SFCCMD.xxSCCMn
        HLSLuint specialColorCalcMode;

        // Page shifts are either 0 or 1, used when determining which plane a particular (x,y) coordinate belongs to.
        // A shift of 0 corresponds to 1 page per plane dimension.
        // A shift of 1 corresponds to 2 pages per plane dimension.
        // Derived from PLSZ.xxPLSZn
        HLSLuint2 pageShift;

        // Bitmap dimensions, when the screen is in bitmap mode.
        // Derived from CHCTLA/CHCTLB.xxBMSZ
        HLSLuint2 bitmapSize;

        // Base address of bitmap data.
        // Derived from MPOFN (NBG0-3) or MPOFR (RotParam A-B)
        HLSLuint bitmapBaseAddress;

        // Window parameters
        VDP2LayerWindowParamsS windowParams;
    };

    /// @brief VDP2 NBG layer rendering parameters.
    struct NBGParams {
        VDP2BaseBGParams base;

        // Screen scroll amount, in 11.8 fixed-point format.
        // Used in scroll NBGs.
        // Scroll amounts for NBGs 2 and 3 do not have a fractional part, but the values are still stored with 8
        // fractional bits here for consistency and ease of implementation. Derived from SCXINn and SCXDNn
        HLSLuint2 scrollAmount;

        // Screen scroll increment per pixel, in 11.8 fixed-point format.
        // NBGs 2 and 3 do not have increment registers; they always increment each coordinate by 1.0, which is stored
        // here for consistency and ease of implementation. Derived from ZMXINn and ZMXDNn
        HLSLuint2 scrollInc;

        // Page base addresses for NBG planes A-D.
        // Derived from MPOFN, MPABNn, MPCDNn, CHCTLA/CHCTLB.xxCHSZ, PNCNn.xxPNB and PLSZ.xxPLSZn
        std::array<HLSLuint, 4> pageBaseAddresses;

        // Whether to use the vertical cell scroll table in VRAM.
        // Only valid for NBG0 and NBG1.
        // Derived from SCRCTL.NnVCSC
        HLSLbool vcellScrollEnable;

        // Whether to use the horizontal line scroll table in VRAM.
        // Only valid for NBG0 and NBG1.
        // Derived from SCRCTL.NnLSCX
        HLSLbool lineScrollXEnable;

        // Whether to use the vertical line scroll table in VRAM.
        // Only valid for NBG0 and NBG1.
        // Derived from SCRCTL.NnLSCY
        HLSLbool lineScrollYEnable;

        // Whether to use horizontal line zoom/scaling.
        // Only valid for NBG0 and NBG1.
        // Derived from SCRCTL.NnLZMX
        HLSLbool lineZoomEnable;

        // Line scroll table interval shift. The interval is calculated as (1 << lineScrollInterval).
        // Only valid for NBG0 and NBG1.
        // Derived from SCRCTL.NnLSS1-0
        HLSLuint lineScrollInterval;

        // Line scroll table base address.
        // Only valid for NBG0 and NBG1.
        // Derived from LSTAnU/L
        HLSLuint lineScrollTableAddress;

        // Vertical cell scroll offset.
        // Only valid for NBG0 and NBG1.
        // Based on CYCA0/A1/B0/B1 parameters.
        HLSLuint vcellScrollOffset;

        // Is the vertical cell scroll read delayed by one cycle?
        // Only valid for NBG0 and NBG1.
        // Based on CYCA0/A1/B0/B1 parameters.
        HLSLuint vcellScrollDelay;

        // Is the first vertical cell scroll entry repeated?
        // Only valid for NBG0.
        // Based on CYCA0/A1/B0/B1 parameters.
        HLSLuint vcellScrollRepeat;
    };

    /// @brief VDP2 RBG layer rendering parameters.
    struct RBGParams {
        VDP2BaseBGParams base;

        // Rotation BG screen-over process.
        // Derived from PLSZ.RxOVRn
        HLSLuint screenOverProcess;

        // Screen-over pattern name value.
        // Derived from OVPNRA/B
        HLSLuint screenOverPatternName;

        /// @brief Page base addresses for RBG planes A-P using Rotation Parameters A and B.
        /// Indexing: [RotParam A/B][Plane A-P]
        /// Derived from `mapIndices`, `CHCTLA/CHCTLB.xxCHSZ`, `PNCR.xxPNB` and `PLSZ.xxPLSZn`.
        std::array<std::array<HLSLuint, 16>, 2> pageBaseAddresses;
    };

    /// @brief LNCL/BACK screen parameters.
    struct VDP2LineBackScreenParams {
        // Base address of color data
        HLSLuint baseAddress;

        // Use colors per line
        //   false = per screen
        //   true  = per line
        HLSLbool perLine;
    };

    /// @brief VDP2 layer rendering parameters.
    struct VDP2LayerRenderParams {
        NBGParams nbg[4];
        RBGParams rbg[2];

        VDP2GlobalWindowParams windows[2];

        VDP2LayerWindowParams rotWindows;

        VDP2LineBackScreenParams lineScreenParams;
        VDP2LineBackScreenParams backScreenParams;

        // Bit-packed special function code flags.
        //  bits  use
        //   0-7  Special function code A
        //  8-15  Special function code B
        HLSLuint specialFunctionCodes;
    };

    /// @brief VDP2 compositor parameters.
    struct VDP2ComposeParams {
        // Use color calculation per layer (0=disable; 1=enable)
        //   bit  layer
        //     0  Sprite
        //     1  RBG0
        //     2  RBG1/NBG0
        //     3  NBG1/EXBG
        //     4  NBG2
        //     5  NBG3
        //     6  Back screen
        //     7  Line screen
        HLSLuint colorCalcEnable;

        // Color offset enable per layer (0=disable; 1=enable)
        //   bit  layer
        //     0  Sprite
        //     1  RBG0
        //     2  RBG1/NBG0
        //     3  NBG1/EXBG
        //     4  NBG2
        //     5  NBG3
        //     6  Back screen
        HLSLuint colorOffsetEnable;

        // Color offset select per layer (0=A; 1=B)
        //   bit  layer
        //     0  Sprite
        //     1  RBG0
        //     2  RBG1/NBG0
        //     3  NBG1/EXBG
        //     4  NBG2
        //     5  NBG3
        //     6  Back screen
        HLSLuint colorOffsetSelect;

        // Line color enable per layer (0=disable; 1=enable)
        //   bit  layer
        //     0  Sprite
        //     1  RBG0
        //     2  RBG1/NBG0
        //     3  NBG1/EXBG
        //     4  NBG2
        //     5  NBG3
        //     6  Back screen (always false to simplify shader implementation)
        HLSLuint lineColorEnable;

        // Color offset A (RGB999)
        HLSLint3 colorOffsetA;

        // Color offset B (RGB999)
        HLSLint3 colorOffsetB;

        // NBG/RBG color calculation ratios
        // index  layer
        //     0  RBG0
        //     1  NBG0/RBG1
        //     2  NBG1/EXBG
        //     3  NBG2
        //     4  NBG3
        std::array<HLSLuint, 5> bgColorCalcRatios;

        // Back/line screen color calculation ratios
        // index  layer
        //     0  Back screen
        //     1  Line screen
        std::array<HLSLuint, 2> backLineColorCalcRatios;

        // Sprite shadow enable per layer (0=disable; 1=enable), applied when the layer is the topmost one
        //   bit  layer
        //     0  Sprite (always enabled; MSB shadows on the sprite layer itself)
        //     1  RBG0
        //     2  RBG1/NBG0
        //     3  NBG1/EXBG
        //     4  NBG2
        //     5  NBG3
        //     6  Back screen
        HLSLuint shadowEnable;
    };

    /// @brief Number of entries in the VDP2 CRAM color cache.
    /// VDP2 CRAM can have at most 2048 colors (in mode 1 - RGB 5:5:5 with access to full CRAM).
    static constexpr size_t kVDP2CRAMColorCacheEntries = kVDP2CRAMSize / sizeof(uint16);

    /// @brief VDP2 CRAM converted color cache array.
    using CRAMColorCache = std::array<ColorR8G8B8A8, kVDP2CRAMColorCacheEntries>;

    /// @brief Size of the VDP2 CRAM color buffer, in bytes.
    static constexpr uint32 kVDP2CRAMColorBufferSize = sizeof(CRAMColorCache);

    /// @brief Size of the VDP2 CRAM rotation coefficients buffer, in bytes.
    /// The second half of CRAM can be used for that purpose.
    static constexpr uint32 kVDP2CRAMRotCoeffBufferSize = kVDP2CRAMSize / 2;

    struct VDP2Resources {
        VDP2Resources(const config::VDP2AccessPatternsConfig &accessPatternsConfig,
                      const config::VDP2DebugRender &debugRenderOptions)
            : accessPatternsConfig(accessPatternsConfig)
            , debugRenderOptions(debugRenderOptions) {}

        // VDP2 VRAM is exposed as a ByteAddressBuffer to shaders as they often need to access raw bytes in 8-bit,
        // 16-bit and 32-bit units.

        /// @brief VRAM data buffer.
        GpuBuffer vramBuffer;

        /// @brief Bit shift for the granularity for VRAM dirty bitmap chunks.
        static constexpr size_t kVRAMDirtyBitmapChunkSizeShift = 8;

        /// @brief Granularity for VRAM dirty bitmap chunks, in bytes.
        static constexpr size_t kVRAMDirtyBitmapChunkSize = static_cast<size_t>(1) << kVRAMDirtyBitmapChunkSizeShift;

        /// @brief Number of bits in the VRAM dirty bitmap.
        static constexpr size_t kVRAMDirtyBitmapSize = kVDP2VRAMSize / kVRAMDirtyBitmapChunkSize;

        // Buffer transfers must be done in multiples of 4 bytes.
        // The chunk must not be larger than VDP2 VRAM itself. In fact, it shouldn't be too large as it wastes memory
        // and time with unnecessary copies of VRAM data.
        static_assert(kVRAMDirtyBitmapChunkSize >= sizeof(uint32) && kVRAMDirtyBitmapChunkSize <= kVDP2VRAMSize,
                      "VDP2 VRAM upload chunk size is out of range");

        /// @brief VRAM dirty bitmap.
        util::DirtyBitmap<kVRAMDirtyBitmapSize> vramDirty;

        // VDP2 CRAM is not directly exposed. Instead, shaders get two convenient views:
        // - CRAM converted to R8G8B8A8 colors based on the current color RAM mode
        // - Top half of raw CRAM bytes, for rotation coefficients

        /// @brief CPU-side CRAM color buffer.
        CRAMColorCache cpuCRAMColorCache{};

        /// @brief Current CRAM generation (dirty tracking).
        uint32 cramGeneration = 0;

        /// @brief CPU-side LNCL/BACK screen buffer (0=LNCL; 1=BACK).
        std::array<std::array<ColorR8G8B8A8, kMaxResV>, 2> cpuLnclBack{};

        /// @brief CPU-side VDP2 rotation parameter base values.
        std::array<VDP2RotParamBase, kMaxNormalResV * 2> cpuRotParamBases{};

        /// @brief 2D texture for the composited VDP2 output.
        /// This cannot be instantiated per frame because interlaced graphics are weaved into the same output frame.
        GpuImage compositeOutTexture;

        // ---------------------------------------------------------------------

        /// @brief Common rendering parameters, uploaded as 32-bit root constants.
        VDP2CommonRenderParams cpuCommonRenderParams{};

        /// @brief CPU-side layer rendering parameters.
        VDP2LayerRenderParams cpuLayerRenderParams{};

        /// @brief CPU-side layer composition parameters.
        VDP2ComposeParams cpuComposeParams{};

        /// @brief Root signature for drawing the sprite layer.
        ComputeLayout drawSpriteRootSig;

        /// @brief Root signature for drawing background layers.
        ComputeLayout drawBGsRootSig;

        /// @brief Root signature for compositing layers.
        ComputeLayout composeRootSig;

        /// @brief Pipelines for drawing the sprite layer, background layers and compositing layers.
        UniquePipeline drawSpritePSO;
        UniquePipeline drawBGsPSO;
        UniquePipeline composePSO;

        // ---------------------------------------------------------------------
        // Rendering state

        uint32 nextLayerRenderLine = 0;
        uint32 nextComposeLine = 0;

        uint32 layerRenderParamsGeneration = 0;
        uint32 composeParamsGeneration = 0;

        const config::VDP2AccessPatternsConfig &accessPatternsConfig;
        const config::VDP2DebugRender &debugRenderOptions;
    } vdp2;

    // =================================================================================================================
    // Per-frame and shared resources

    BarrierTracker barrierTracker;

    /// @brief GPU resources used while rendering.
    /// The GPU processes frames in submission order and every update to these resources is recorded into the command
    /// stream, so a single instance is shared by all frames in flight. This keeps memory usage in check with internal
    /// resolution scaling.
    struct SharedResources {

        /// @brief Span parameters buffer.
        GpuBuffer spanParamsBuffer;
        /// @brief Span prefix sum buffer.
        GpuBuffer spanPrefixSumsBuffer;

        /// @brief Command parameters buffer.
        GpuBuffer cmdParamsBuffer;

        /// @brief Internal sprite data output buffer.
        GpuBuffer internalSpriteOutBuffer;
        /// @brief Internal OIT fragments list heads buffer.
        GpuBuffer oitListHeadsBuffer;
        /// @brief Internal OIT fragment nodes buffer.
        GpuBuffer oitFragmentsBuffer;
        /// @brief Internal OIT counter buffer.
        GpuBuffer oitCounterBuffer;

        /// @brief Descriptor set for drawing polygons (Copy and Shift variants).
        VkDescriptorSet polyDrawSet = VK_NULL_HANDLE;
        /// @brief Descriptor set for drawing polygons (OIT variants).
        VkDescriptorSet polyDrawOITSet = VK_NULL_HANDLE;
        /// @brief Descriptor set for drawing polygons (MSB variants).
        VkDescriptorSet polyDrawMSBSet = VK_NULL_HANDLE;
        /// @brief Descriptor set for the output merger (non-OIT variants).
        VkDescriptorSet outputMergerSet = VK_NULL_HANDLE;
        /// @brief Descriptor set for the output merger (OIT variants).
        VkDescriptorSet outputMergerOITSet = VK_NULL_HANDLE;

        /// @brief CRAM color buffer.
        GpuBuffer cramColorBuffer;
        /// @brief Raw CRAM rotation coefficients buffer.
        GpuBuffer cramRotCoeffBuffer;
        /// @brief CRAM generation of the color buffer (dirty tracking).
        uint32 cramGeneration = 0xFFFFFFFF;
        /// @brief CRAM generation of the rotation coefficients buffer (dirty tracking).
        uint32 cramRotCoeffGeneration = 0xFFFFFFFF;

        /// @brief 2D texture array for the outputs of NBG0-3, RBG0-1, sprite and mesh layers (in that order).
        GpuImage layerOutTexture;
        /// @brief 2D texture array for RBG0-1 line color outputs (in that order).
        GpuImage rbgLineColorOutTexture;
        /// @brief Color calculation window 2D texture.
        GpuImage colorCalcWindowTexture;
        /// @brief LNCL/BACK screen buffer.
        GpuBuffer lnclBackBuffer;
        /// @brief VDP2 rotation parameter base values buffer.
        GpuBuffer rotParamBasesBuffer;
        /// @brief VDP2 sprite attributes 2D texture array (sprite then mesh).
        GpuImage spriteAttrsTexture;

        /// @brief Layer rendering parameters buffer.
        GpuBuffer layerRenderParamsBuffer;
        /// @brief Current layer rendering parameters generation (dirty tracking).
        uint32 layerRenderParamsGeneration = 0xFFFFFFFF;

        /// @brief Layer composition parameters buffer.
        GpuBuffer composeParamsBuffer;
        /// @brief Current composition parameters generation (dirty tracking).
        uint32 composeParamsGeneration = 0xFFFFFFFF;

        /// @brief Descriptor set for drawing the sprite layer.
        VkDescriptorSet drawSpriteSet = VK_NULL_HANDLE;
        /// @brief Descriptor set for drawing background layers.
        VkDescriptorSet drawBGsSet = VK_NULL_HANDLE;
        /// @brief Descriptor set for compositing layers.
        VkDescriptorSet composeSet = VK_NULL_HANDLE;
    } shared;

    /// @brief Resources for a single frame.
    struct FrameContext {
        UniqueCommandPool cmdPool;
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
        UniqueFence fence;
        uint64 signaledValue = 0; // fence value associated with this frame. 0 means never used

        /// @brief CPU-side span parameters buffer.
        std::array<VDP1SpanParams, kMaxVDP1Spans> cpuSpanParams{};
        /// @brief CPU-side span prefix sums buffer.
        /// The first entry is always 0 to simplify implementation.
        std::array<HLSLuint, kMaxVDP1Spans + 1> cpuSpanPrefixSums{};
        /// @brief Number of spans allocated so far.
        size_t cpuSpanCount = 0;
        /// @brief CPU-side command parameters buffer.
        std::array<VDP1CommandParams, kMaxVDP1Commands> cpuCmdParams{};
        /// @brief Number of commands allocated so far.
        size_t cpuCmdCount = 0;

        /// @brief Host-visible copy of the composited output of this frame.
        GpuBuffer readbackBuffer;
        /// @brief Whether the readback buffer holds a frame that has not been delivered to the frontend yet.
        bool readbackPending = false;
        /// @brief Dimensions of the frame in the readback buffer.
        uint32 readbackWidth = 0;
        uint32 readbackHeight = 0;
    };

    /// @brief Ring buffer of frame resources.
    /// Emulates a monotonically increasing fence value (like a Direct3D 12 fence) with one VkFence per frame.
    /// @tparam count number of frames
    template <size_t count>
    struct FrameSet {
        std::array<FrameContext, count> frames;
        size_t frameIndex = 0;
        uint64 currFenceValue = 0;
        uint64 completedValue = 0;

        FrameContext &GetCurrentFrame() {
            return frames[frameIndex];
        }
        const FrameContext &GetCurrentFrame() const {
            return frames[frameIndex];
        }

        uint64 GetNextFenceValue() const {
            return currFenceValue + 1;
        }

        /// @brief Retrieves the latest fence value known to be completed by the GPU.
        uint64 GetCompletedValue(VkDevice device) {
            // Submissions to a single queue complete in order, so the highest signaled value covers all prior ones
            for (FrameContext &frame : frames) {
                if (frame.signaledValue > completedValue && vkGetFenceStatus(device, frame.fence) == VK_SUCCESS) {
                    completedValue = frame.signaledValue;
                }
            }
            return completedValue;
        }

        /// @brief Blocks until the GPU completes the frame with the given fence value.
        void WaitForValue(VkDevice device, uint64 value) {
            if (GetCompletedValue(device) >= value) {
                return;
            }
            FrameContext *target = nullptr;
            for (FrameContext &frame : frames) {
                if (frame.signaledValue >= value && (target == nullptr || frame.signaledValue < target->signaledValue)) {
                    target = &frame;
                }
            }
            if (target == nullptr) {
                // Not submitted yet; nothing to wait for
                return;
            }
            VkFence fence = target->fence;
            if (VkResult res = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX); res != VK_SUCCESS) {
                Report<grp::vk_base>(devlog::level::warn, "Waiting for GPU work failed: {} ({})", VkResultName(res),
                                     static_cast<sint32>(res));
            }
            completedValue = std::max(completedValue, target->signaledValue);
        }

        void WaitForLatestFrame(VkDevice device) {
            WaitForValue(device, currFenceValue);
        }

        FrameContext &operator[](size_t index) {
            return frames[index];
        }
        const FrameContext &operator[](size_t index) const {
            return frames[index];
        }

        constexpr size_t Count() const {
            return count;
        }
    };

    /// @brief Per-frame resources.
    FrameSet<kNumFrames> frames;

    /// @brief Command buffer currently being recorded.
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    /// @brief Upload ring buffer.
    UploadRingBuffer uploadBuffer;

    /// @brief Descriptor pool for all descriptor sets.
    UniqueDescriptorPool descriptorPool;

    /// @brief Whether initialization completed successfully.
    bool initialized = false;

    /// @brief Set when the GPU fails to execute work (for example, VK_ERROR_DEVICE_LOST). No more commands are
    /// recorded afterwards.
    bool deviceLost = false;

    // =================================================================================================================
    // Command helpers

    /// @brief Records a copy between two buffers.
    void CopyBuffer(VkBuffer src, VkDeviceSize srcOffset, VkBuffer dst, VkDeviceSize dstOffset, VkDeviceSize size) {
        if (cmd == VK_NULL_HANDLE) {
            return;
        }
        const VkBufferCopy region{
            .srcOffset = srcOffset,
            .dstOffset = dstOffset,
            .size = size,
        };
        vkCmdCopyBuffer(cmd, src, dst, 1, &region);
        barrierTracker.MarkPending();
    }

    /// @brief Records a copy from the upload buffer.
    void CopyFromUpload(const GpuBuffer &dst, VkDeviceSize dstOffset, const UploadAllocation &alloc, VkDeviceSize size) {
        CopyBuffer(uploadBuffer.GetBuffer(), alloc.offset, dst.buffer, dstOffset, size);
    }

    /// @brief Records a fill of a buffer region with a repeated 32-bit value.
    void FillBuffer(const GpuBuffer &dst, VkDeviceSize offset, VkDeviceSize size, uint32 value) {
        if (cmd == VK_NULL_HANDLE) {
            return;
        }
        vkCmdFillBuffer(cmd, dst.buffer, offset, size, value);
        barrierTracker.MarkPending();
    }

    /// @brief Records a compute dispatch with the given pipeline, descriptor set and push constants.
    void Dispatch(VkPipeline pipeline, const ComputeLayout &layout, VkDescriptorSet set, const void *pushConstants,
                  uint32 pushConstantsSize, uint32 x, uint32 y, uint32 z) {
        assert(pushConstantsSize <= layout.pushConstantSize);
        if (cmd == VK_NULL_HANDLE) {
            return;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout.pipelineLayout, 0, 1, &set, 0, nullptr);
        if (pushConstantsSize > 0) {
            vkCmdPushConstants(cmd, layout.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantsSize,
                               pushConstants);
        }
        if (x > 0 && y > 0 && z > 0) {
            vkCmdDispatch(cmd, x, y, z);
        }
        barrierTracker.MarkPending();
    }

    /// @brief Begins recording the current frame's command buffer.
    util::VoidResult<> BeginCommands() {
        if (deviceLost) {
            // Nothing can be rendered anymore; commands are dropped until the renderer is recreated
            return {};
        }
        FrameContext &frame = frames.GetCurrentFrame();
        if (VkResult res = vkResetCommandPool(vk.device, frame.cmdPool, 0); res != VK_SUCCESS) {
            return VkError("Could not reset command pool", res);
        }
        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        if (VkResult res = vkBeginCommandBuffer(frame.cmdBuffer, &beginInfo); res != VK_SUCCESS) {
            return VkError("Could not begin command buffer", res);
        }
        cmd = frame.cmdBuffer;
        // Commands from previous submissions must complete before this command buffer uses their results
        barrierTracker.MarkPending();
        return {};
    }

    /// @brief Makes the results of recorded transfers visible to CPU reads once the command buffer completes.
    void RecordHostReadBarrier() {
        if (cmd == VK_NULL_HANDLE) {
            return;
        }
        const VkMemoryBarrier hostBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
    }

    /// @brief Ends and submits the current command buffer, signaling the next fence value.
    util::VoidResult<> SubmitCommands() {
        FrameContext &frame = frames.GetCurrentFrame();
        if (cmd == VK_NULL_HANDLE) {
            return {};
        }
        if (VkResult res = vkEndCommandBuffer(frame.cmdBuffer); res != VK_SUCCESS) {
            cmd = VK_NULL_HANDLE;
            deviceLost = true;
            return VkError("Could not end command buffer", res);
        }
        VkFence fence = frame.fence;
        vkResetFences(vk.device, 1, &fence);
        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &frame.cmdBuffer,
        };
        const uint64 signalValue = frames.GetNextFenceValue();
        uploadBuffer.EndFrame(signalValue);
        frame.signaledValue = signalValue;
        frames.currFenceValue = signalValue;
        cmd = VK_NULL_HANDLE;
        if (VkResult res = vkQueueSubmit(vk.queue, 1, &submitInfo, fence); res != VK_SUCCESS) {
            // Typically VK_ERROR_DEVICE_LOST: the GPU faulted or timed out. Stop rendering instead of crashing.
            deviceLost = true;
            frame.signaledValue = 0;
            return VkError("Could not submit command buffer; GPU rendering stopped", res);
        }
        return {};
    }

    /// @brief Hands a finished frame to the frontend, waiting for the GPU to finish it if necessary.
    void DeliverFrame(FrameContext &frame) {
        if (!frame.readbackPending) {
            return;
        }
        frame.readbackPending = false;
        if (deviceLost) {
            return;
        }
        frames.WaitForValue(vk.device, frame.signaledValue);
        frame.readbackBuffer.Invalidate();
        hwCallbacks.FrameReady(static_cast<uint32 *>(frame.readbackBuffer.mapped), frame.readbackWidth,
                               frame.readbackHeight);
    }

    /// @brief Switches to the next frame context, waiting for its previous use to complete, and starts recording.
    void MoveToNextFrame() {
        frames.frameIndex = (frames.frameIndex + 1) % kNumFrames;
        FrameContext &nextFrame = frames.GetCurrentFrame();
        if (nextFrame.signaledValue != 0) {
            frames.WaitForValue(vk.device, nextFrame.signaledValue);
        }
        if (auto result = BeginCommands(); !result) {
            Report<grp::vk_base>(devlog::level::warn, "{}", result.Error().message);
        }
    }

    // =================================================================================================================
    // Operations

    util::VoidResult<> Initialize() {
        UpdateEnhancements();
        if (auto result = vk.Create(); !result) {
            return result;
        }
        VkDevice device = vk.device;

        // Per-frame command pools, command buffers and fences
        for (size_t i = 0; i < frames.Count(); ++i) {
            FrameContext &frame = frames[i];

            const VkCommandPoolCreateInfo poolInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                .queueFamilyIndex = vk.queueFamily,
            };
            VkCommandPool pool = VK_NULL_HANDLE;
            if (VkResult res = vkCreateCommandPool(device, &poolInfo, nullptr, &pool); res != VK_SUCCESS) {
                return VkError(fmt::format("Could not create command pool #{}", i), res);
            }
            frame.cmdPool.Assign(device, pool);

            const VkCommandBufferAllocateInfo cmdInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            if (VkResult res = vkAllocateCommandBuffers(device, &cmdInfo, &frame.cmdBuffer); res != VK_SUCCESS) {
                return VkError(fmt::format("Could not allocate command buffer #{}", i), res);
            }

            const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence = VK_NULL_HANDLE;
            if (VkResult res = vkCreateFence(device, &fenceInfo, nullptr, &fence); res != VK_SUCCESS) {
                return VkError(fmt::format("Could not create fence #{}", i), res);
            }
            frame.fence.Assign(device, fence);
        }

        // Generic upload buffer
        // Leave room for uploading both scaled framebuffers at once
        const size_t uploadBufferSize = kUploadBufferSize + ScaledFBSize() * 2;
        if (auto result = uploadBuffer.Create(vk, uploadBufferSize); !result) {
            return util::ErrorMessage{fmt::format("Could not create VDP upload buffer: {}", result.Error().message)};
        }
        uploadBuffer.SetDebugName("VDP");

        // Descriptor pool with room for every descriptor set in the renderer
        {
            static constexpr uint32 kSetsPerFrame = 8;
            static constexpr uint32 kGlobalSets = 2;
            static constexpr uint32 kMaxSets = kSetsPerFrame * kNumFrames + kGlobalSets;
            static constexpr uint32 kDescsPerType = 16 * kNumFrames + 8;
            const VkDescriptorPoolSize poolSizes[] = {
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kDescsPerType * 2},
                {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kDescsPerType},
                {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, kDescsPerType},
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescsPerType},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDescsPerType},
            };
            const VkDescriptorPoolCreateInfo poolInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = kMaxSets,
                .poolSizeCount = static_cast<uint32>(std::size(poolSizes)),
                .pPoolSizes = poolSizes,
            };
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (VkResult res = vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool); res != VK_SUCCESS) {
                return VkError("Could not create descriptor pool", res);
            }
            descriptorPool.Assign(device, pool);
        }

        auto allocateSet = [&](const ComputeLayout &layout, VkDescriptorSet &outSet,
                               std::string_view name) -> util::VoidResult<> {
            const VkDescriptorSetAllocateInfo allocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = descriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout.setLayout,
            };
            if (VkResult res = vkAllocateDescriptorSets(device, &allocInfo, &outSet); res != VK_SUCCESS) {
                return VkError(fmt::format("Could not allocate {} descriptor set", name), res);
            }
            return {};
        };

        auto createPipeline = [&](UniquePipeline &outPipeline, const ComputeLayout &layout, const std::string &path,
                                  std::string_view name) -> util::VoidResult<> {
            auto codeResult = LoadShader(path.c_str());
            if (!codeResult) {
                return util::ErrorMessage{
                    fmt::format("Could not load {} compute shader: {}", name, codeResult.Error().message)};
            }
            const std::vector<uint32> &code = codeResult.Value();
            const VkShaderModuleCreateInfo moduleInfo{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = code.size() * sizeof(uint32),
                .pCode = code.data(),
            };
            VkShaderModule module = VK_NULL_HANDLE;
            if (VkResult res = vkCreateShaderModule(device, &moduleInfo, nullptr, &module); res != VK_SUCCESS) {
                return VkError(fmt::format("Could not create {} shader module", name), res);
            }
            util::ScopeGuard sgModule{[&] { vkDestroyShaderModule(device, module, nullptr); }};

            const VkComputePipelineCreateInfo pipelineInfo{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage =
                    {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                        .module = module,
                        .pName = "CSMain",
                    },
                .layout = layout.pipelineLayout,
            };
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (VkResult res =
                    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
                res != VK_SUCCESS) {
                return VkError(fmt::format("Could not create {} pipeline", name), res);
            }
            outPipeline.Assign(device, pipeline);
            return {};
        };

        static constexpr VkBufferUsageFlags kSSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        static constexpr VkBufferUsageFlags kUTB =
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        static constexpr VkBufferUsageFlags kSTB =
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        static constexpr VkImageUsageFlags kImageUsage =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        static constexpr VkDescriptorType kDescSSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        static constexpr VkDescriptorType kDescUTB = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        static constexpr VkDescriptorType kDescSTB = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        static constexpr VkDescriptorType kDescSampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        static constexpr VkDescriptorType kDescStorageImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

#define YMIR_VK_TRY(expr)                                                                                              \
    if (auto result_ = (expr); !result_) {                                                                             \
        return result_;                                                                                                \
    }

        // =============================================================================================================
        // VDP1

        // -------------------------------------------------------------------------------------------------------------
        // Common resources

        YMIR_VK_TRY(vdp1.vramBuffer.Create(vk, kVDP1VRAMSize, kSSBO, MemoryUsage::DeviceLocal, "VDP1 VRAM buffer"));

        // kVDP1FBRAMSize is the size of a single framebuffer.
        // VDP1 FBRAM actually contains two frames.
        // *2 for deinterlace alternate field
        // *2 for transparent mesh buffer
        // With internal resolution scaling, every framebuffer is scaled in both dimensions.
        YMIR_VK_TRY(vdp1.fbramBuffer.Create(vk, ScaledFBSize() * 2 * 2 * 2, kSSBO, MemoryUsage::DeviceLocal,
                                            "VDP1 FBRAM buffer"));
        if (resolutionScale > 1) {
            // Native copy of the scaled framebuffers, downloaded for CPU reads
            YMIR_VK_TRY(vdp1.fbramNativeBuffer.Create(vk, kVDP1FBRAMSize * 2, kSSBO, MemoryUsage::DeviceLocal,
                                                      "VDP1 native FBRAM buffer"));
        }
        YMIR_VK_TRY(vdp1.fbramWritesBuffer.Create(vk, sizeof(VDP1FBRAMWrite) * kVDP1FBRAMSize, kSSBO,
                                                  MemoryUsage::DeviceLocal, "VDP1 FBRAM writes buffer"));
        // Allocate enough room to download the standard VDP1 FBRAM.
        // TODO: add deinterlace and transparent mesh buffers to save state
        YMIR_VK_TRY(vdp1.fbramDownloadBuffer.Create(vk, kVDP1FBRAMSize * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                    MemoryUsage::Readback, "VDP1 FBRAM download buffer"));

        // -------------------------------------------------------------------------------------------------------------
        // Layouts and pipelines

        static constexpr uint32 kVDP1CommonSize = sizeof(VDP1CommonRenderParams);

        // Framebuffer write
        YMIR_VK_TRY(vdp1.fbramWriteRootSig.Create(device, {{SRV(1), kDescSSBO}, {UAV(1), kDescSSBO}},
                                                  kVDP1CommonSize + sizeof(uint32), "VDP1 framebuffer write"));
        YMIR_VK_TRY(createPipeline(vdp1.fbramWritePSO, vdp1.fbramWriteRootSig, "src/vdp/cs_vdp1_fbram_write.spv",
                                   "VDP1 framebuffer write"));
        YMIR_VK_TRY(allocateSet(vdp1.fbramWriteRootSig, vdp1.fbramWriteSet, "VDP1 framebuffer write"));

        // Framebuffer erase
        YMIR_VK_TRY(vdp1.eraseRootSig.Create(device, {{UAV(1), kDescSSBO}}, kVDP1CommonSize + sizeof(VDP1EraseParams),
                                             "VDP1 framebuffer erase"));
        YMIR_VK_TRY(createPipeline(vdp1.erasePSO, vdp1.eraseRootSig, "src/vdp/cs_vdp1_erase.spv",
                                   "VDP1 framebuffer erase"));
        YMIR_VK_TRY(allocateSet(vdp1.eraseRootSig, vdp1.eraseSet, "VDP1 framebuffer erase"));

        // Polygon drawing. Unlike Direct3D 12 root signatures, Vulkan descriptor set layouts specify the descriptor
        // types, so the Copy/Shift (RWBuffer), MSB (RWByteAddressBuffer) and OIT variants need separate layouts.
        static constexpr uint32 kPolyDrawPCSize = kVDP1CommonSize + sizeof(VDP1PolyDrawParams);
        YMIR_VK_TRY(vdp1.polyDrawRootSig.Create(device,
                                                {{SRV(1), kDescSSBO},
                                                 {SRV(2), kDescUTB},
                                                 {SRV(3), kDescSSBO},
                                                 {SRV(4), kDescSSBO},
                                                 {UAV(1), kDescSTB}},
                                                kPolyDrawPCSize, "VDP1 polygon drawing"));
        YMIR_VK_TRY(vdp1.polyDrawMSBRootSig.Create(device,
                                                   {{SRV(1), kDescSSBO},
                                                    {SRV(2), kDescUTB},
                                                    {SRV(3), kDescSSBO},
                                                    {SRV(4), kDescSSBO},
                                                    {UAV(1), kDescSSBO}},
                                                   kPolyDrawPCSize, "VDP1 polygon drawing MSB"));
        YMIR_VK_TRY(vdp1.polyDrawOITRootSig.Create(device,
                                                   {{SRV(1), kDescSSBO},
                                                    {SRV(2), kDescUTB},
                                                    {SRV(3), kDescSSBO},
                                                    {SRV(4), kDescSSBO},
                                                    {UAV(1), kDescSTB},
                                                    {UAV(2), kDescSSBO},
                                                    {UAV(3), kDescSSBO}},
                                                   kPolyDrawPCSize, "VDP1 polygon drawing OIT"));
        for (size_t shaderIndex = 0; shaderIndex < vdp1.polyDrawPSOs.size(); ++shaderIndex) {
            const auto [meshMode, shadingMode] = ExpandVDP1PolyDrawShaderIndex(shaderIndex);
            const std::string variantName = GetVDP1PolyDrawShaderVariantName(shaderIndex);
            const std::string filename = fmt::format("src/vdp/cs_vdp1_polydraw_{}_{}.spv", meshMode, shadingMode);
            YMIR_VK_TRY(createPipeline(vdp1.polyDrawPSOs[shaderIndex], GetPolyDrawLayout(shaderIndex), filename,
                                       fmt::format("VDP1 polygon drawing variant {}", variantName)));
        }

        // Polygon output merger
        YMIR_VK_TRY(vdp1.outputMergerRootSig.Create(device, {{UAV(1), kDescSSBO}, {UAV(2), kDescSTB}},
                                                    kVDP1CommonSize, "VDP1 output merger"));
        YMIR_VK_TRY(vdp1.outputMergerOITRootSig.Create(
            device, {{SRV(1), kDescSSBO}, {UAV(1), kDescSSBO}, {UAV(2), kDescSTB}}, kVDP1CommonSize,
            "VDP1 output merger OIT"));
        for (size_t shaderIndex = 0; shaderIndex < vdp1.outputMergerPSOs.size(); ++shaderIndex) {
            const auto [meshMode, mergeMode] = ExpandVDP1OutputMergerShaderIndex(shaderIndex);
            const std::string variantName = GetVDP1OutputMergerShaderVariantName(shaderIndex);
            const std::string filename = fmt::format("src/vdp/cs_vdp1_output_merger_{}_{}.spv", meshMode, mergeMode);
            const ComputeLayout &layout = IsVDP1OutputMergerShaderOIT(shaderIndex) ? vdp1.outputMergerOITRootSig
                                                                                    : vdp1.outputMergerRootSig;
            YMIR_VK_TRY(createPipeline(vdp1.outputMergerPSOs[shaderIndex], layout, filename,
                                       fmt::format("VDP1 output merger variant {}", variantName)));
        }

        // Framebuffer downsampling (internal resolution scaling only)
        if (resolutionScale > 1) {
            YMIR_VK_TRY(vdp1.fbramDownsampleRootSig.Create(device, {{SRV(1), kDescSSBO}, {UAV(1), kDescSSBO}},
                                                           kVDP1CommonSize, "VDP1 framebuffer downsample"));
            YMIR_VK_TRY(createPipeline(vdp1.fbramDownsamplePSO, vdp1.fbramDownsampleRootSig,
                                       "src/vdp/cs_vdp1_fbram_downsample.spv", "VDP1 framebuffer downsample"));
            YMIR_VK_TRY(allocateSet(vdp1.fbramDownsampleRootSig, vdp1.fbramDownsampleSet, "VDP1 framebuffer downsample"));
        }

        // -------------------------------------------------------------------------------------------------------------
        // Shared VDP1 rendering resources

        for (size_t i = 0; i < frames.Count(); ++i) {
            frames[i].cpuSpanPrefixSums[0] = 0;
        }

        YMIR_VK_TRY(shared.spanParamsBuffer.Create(vk, sizeof(FrameContext::cpuSpanParams), kSSBO,
                                                   MemoryUsage::DeviceLocal, "VDP1 span parameters buffer"));
        YMIR_VK_TRY(shared.spanPrefixSumsBuffer.Create(vk, sizeof(FrameContext::cpuSpanPrefixSums), kUTB,
                                                       MemoryUsage::DeviceLocal, "VDP1 span prefix sums buffer"));
        YMIR_VK_TRY(shared.spanPrefixSumsBuffer.CreateView(VK_FORMAT_R32_UINT, "VDP1 span prefix sums buffer"));
        YMIR_VK_TRY(shared.cmdParamsBuffer.Create(vk, sizeof(FrameContext::cpuCmdParams), kSSBO,
                                                  MemoryUsage::DeviceLocal, "VDP1 command parameters buffer"));

        // Each entry in these buffers represents a logical output pixel.
        // *2 for deinterlace alternate field
        // *2 for transparent mesh buffer
        const VkDeviceSize pixelEntriesSize = ScaledFBSize() * 2 * 2 * sizeof(HLSLuint);

        // Entries hold the sprite data in the 8 or 16 LSBs and the span index in the 16 MSBs to enable parallel
        // rendering with guaranteed pixel ordering.
        YMIR_VK_TRY(shared.internalSpriteOutBuffer.Create(vk, pixelEntriesSize, kSTB, MemoryUsage::DeviceLocal,
                                                          "VDP1 internal sprite data output buffer"));
        YMIR_VK_TRY(
            shared.internalSpriteOutBuffer.CreateView(VK_FORMAT_R32_UINT, "VDP1 internal sprite data output buffer"));

        // Entries hold the index of the head of the list.
        YMIR_VK_TRY(shared.oitListHeadsBuffer.Create(vk, pixelEntriesSize, kSTB, MemoryUsage::DeviceLocal,
                                                     "VDP1 OIT fragments list heads buffer"));
        YMIR_VK_TRY(shared.oitListHeadsBuffer.CreateView(VK_FORMAT_R32_UINT, "VDP1 OIT fragments list heads buffer"));

        YMIR_VK_TRY(shared.oitFragmentsBuffer.Create(vk, kMaxVDP1OITFragmentsPerDispatch * sizeof(VDP1OITFragment),
                                                     kSSBO, MemoryUsage::DeviceLocal, "VDP1 OIT fragments buffer"));
        // This buffer contains a single `uint` used as an atomic counter.
        YMIR_VK_TRY(shared.oitCounterBuffer.Create(vk, sizeof(HLSLuint), kSSBO, MemoryUsage::DeviceLocal,
                                                   "VDP1 OIT counter buffer"));

        YMIR_VK_TRY(allocateSet(vdp1.polyDrawRootSig, shared.polyDrawSet, "VDP1 polygon drawing"));
        YMIR_VK_TRY(allocateSet(vdp1.polyDrawMSBRootSig, shared.polyDrawMSBSet, "VDP1 polygon drawing MSB"));
        YMIR_VK_TRY(allocateSet(vdp1.polyDrawOITRootSig, shared.polyDrawOITSet, "VDP1 polygon drawing OIT"));
        YMIR_VK_TRY(allocateSet(vdp1.outputMergerRootSig, shared.outputMergerSet, "VDP1 output merger"));
        YMIR_VK_TRY(allocateSet(vdp1.outputMergerOITRootSig, shared.outputMergerOITSet, "VDP1 output merger OIT"));

        // =============================================================================================================
        // VDP2

        // -------------------------------------------------------------------------------------------------------------
        // Common resources

        YMIR_VK_TRY(vdp2.vramBuffer.Create(vk, kVDP2VRAMSize, kSSBO, MemoryUsage::DeviceLocal, "VDP2 VRAM buffer"));
        const uint32 scaledResH = kMaxResH * resolutionScale;
        const uint32 scaledResV = kMaxResV * resolutionScale;
        YMIR_VK_TRY(vdp2.compositeOutTexture.Create(
            vk, scaledResH, scaledResV, 1, false, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            "VDP2 composited output texture"));

        // -------------------------------------------------------------------------------------------------------------
        // Layouts and pipelines

        static constexpr uint32 kVDP2CommonSize = sizeof(VDP2CommonRenderParams);

        YMIR_VK_TRY(vdp2.drawSpriteRootSig.Create(device,
                                                  {{SRV(1), kDescSSBO},
                                                   {SRV(2), kDescSSBO},
                                                   {SRV(3), kDescUTB},
                                                   {SRV(4), kDescSSBO},
                                                   {SRV(5), kDescSSBO},
                                                   {UAV(0), kDescStorageImage},
                                                   {UAV(1), kDescStorageImage}},
                                                  kVDP2CommonSize, "VDP2 sprite layer rendering"));
        YMIR_VK_TRY(createPipeline(vdp2.drawSpritePSO, vdp2.drawSpriteRootSig, "src/vdp/cs_vdp2_render_sprite.spv",
                                   "VDP2 sprite layer rendering"));

        YMIR_VK_TRY(vdp2.drawBGsRootSig.Create(device,
                                               {{SRV(1), kDescSSBO},
                                                {SRV(2), kDescSSBO},
                                                {SRV(3), kDescUTB},
                                                {SRV(4), kDescSSBO},
                                                {SRV(5), kDescSSBO},
                                                {SRV(6), kDescSampled},
                                                {UAV(0), kDescStorageImage},
                                                {UAV(1), kDescStorageImage},
                                                {UAV(2), kDescStorageImage}},
                                               kVDP2CommonSize, "VDP2 layer rendering"));
        YMIR_VK_TRY(createPipeline(vdp2.drawBGsPSO, vdp2.drawBGsRootSig, "src/vdp/cs_vdp2_render_bgs.spv",
                                   "VDP2 layer rendering"));

        YMIR_VK_TRY(vdp2.composeRootSig.Create(device,
                                               {{SRV(1), kDescSSBO},
                                                {SRV(2), kDescSampled},
                                                {SRV(3), kDescUTB},
                                                {SRV(4), kDescSampled},
                                                {SRV(5), kDescSampled},
                                                {SRV(6), kDescSampled},
                                                {UAV(0), kDescStorageImage}},
                                               kVDP2CommonSize, "VDP2 layer compositing"));
        YMIR_VK_TRY(createPipeline(vdp2.composePSO, vdp2.composeRootSig, "src/vdp/cs_vdp2_compose.spv",
                                   "VDP2 layer compositing"));

        // -------------------------------------------------------------------------------------------------------------
        // Shared VDP2 rendering resources

        YMIR_VK_TRY(shared.cramColorBuffer.Create(vk, kVDP2CRAMColorBufferSize, kUTB, MemoryUsage::DeviceLocal,
                                                  "VDP2 CRAM color buffer"));
        YMIR_VK_TRY(shared.cramColorBuffer.CreateView(VK_FORMAT_R8G8B8A8_UINT, "VDP2 CRAM color buffer"));
        YMIR_VK_TRY(shared.cramRotCoeffBuffer.Create(vk, kVDP2CRAMRotCoeffBufferSize, kSSBO, MemoryUsage::DeviceLocal,
                                                     "VDP2 CRAM rotation coefficients buffer"));

        // The array contains:
        //   [0..3] NBG0-3
        //   [4..5] RBG0-1
        //      [6] Sprite
        //      [7] Transparent meshes
        // The alpha channel is used for pixel attributes:
        //   [0..2] Priority
        //      [6] Color format (0=RGB, 1=Palette)
        //      [7] Special color calculation flag
        YMIR_VK_TRY(shared.layerOutTexture.Create(vk, scaledResH, scaledResV, 4 + 2 + 1 + 1, true,
                                                  VK_FORMAT_R8G8B8A8_UINT, kImageUsage,
                                                  "VDP2 layer outputs texture array"));
        // Line colors are per native pixel
        YMIR_VK_TRY(shared.rbgLineColorOutTexture.Create(vk, kMaxNormalResH, kMaxNormalResV, 2, true,
                                                         VK_FORMAT_R8G8B8A8_UINT, kImageUsage,
                                                         "VDP2 RBG line color outputs texture array"));
        YMIR_VK_TRY(shared.colorCalcWindowTexture.Create(vk, scaledResH, scaledResV, 1, false, VK_FORMAT_R8_UINT,
                                                         kImageUsage, "VDP2 color calculation window texture"));
        YMIR_VK_TRY(shared.lnclBackBuffer.Create(vk, sizeof(vdp2.cpuLnclBack), kUTB, MemoryUsage::DeviceLocal,
                                                 "VDP2 LNCL/BACK screen buffer"));
        YMIR_VK_TRY(shared.lnclBackBuffer.CreateView(VK_FORMAT_R32_UINT, "VDP2 LNCL/BACK screen buffer"));
        YMIR_VK_TRY(shared.rotParamBasesBuffer.Create(vk, sizeof(vdp2.cpuRotParamBases), kSSBO,
                                                      MemoryUsage::DeviceLocal,
                                                      "VDP2 rotation parameter base values buffer"));
        YMIR_VK_TRY(shared.spriteAttrsTexture.Create(vk, scaledResH, scaledResV, 2, true, VK_FORMAT_R16_UINT,
                                                     kImageUsage, "VDP2 sprite attributes texture array"));
        YMIR_VK_TRY(shared.layerRenderParamsBuffer.Create(vk, sizeof(vdp2.cpuLayerRenderParams), kSSBO,
                                                          MemoryUsage::DeviceLocal,
                                                          "VDP2 layer rendering parameters buffer"));
        YMIR_VK_TRY(shared.composeParamsBuffer.Create(vk, sizeof(vdp2.cpuComposeParams), kSSBO,
                                                      MemoryUsage::DeviceLocal,
                                                      "VDP2 layer compositing parameters buffer"));

        YMIR_VK_TRY(allocateSet(vdp2.drawSpriteRootSig, shared.drawSpriteSet, "VDP2 sprite layer rendering"));
        YMIR_VK_TRY(allocateSet(vdp2.drawBGsRootSig, shared.drawBGsSet, "VDP2 layer rendering"));
        YMIR_VK_TRY(allocateSet(vdp2.composeRootSig, shared.composeSet, "VDP2 layer compositing"));

        // Per-frame output readback buffers
        for (size_t i = 0; i < frames.Count(); ++i) {
            YMIR_VK_TRY(frames[i].readbackBuffer.Create(vk, VkDeviceSize{scaledResH} * scaledResV * sizeof(uint32),
                                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback,
                                                        "VDP2 output readback buffer"));
        }

#undef YMIR_VK_TRY

        // =============================================================================================================
        // Descriptor sets

        {
            DescriptorWriter writer{};
            writer.StorageBuffer(vdp1.fbramWriteSet, SRV(1), vdp1.fbramWritesBuffer)
                .StorageBuffer(vdp1.fbramWriteSet, UAV(1), vdp1.fbramBuffer)
                .StorageBuffer(vdp1.eraseSet, UAV(1), vdp1.fbramBuffer);

            if (resolutionScale > 1) {
                writer.StorageBuffer(vdp1.fbramDownsampleSet, SRV(1), vdp1.fbramBuffer)
                    .StorageBuffer(vdp1.fbramDownsampleSet, UAV(1), vdp1.fbramNativeBuffer);
            }

            {
                for (VkDescriptorSet set : {shared.polyDrawSet, shared.polyDrawMSBSet, shared.polyDrawOITSet}) {
                    writer.StorageBuffer(set, SRV(1), shared.spanParamsBuffer)
                        .UniformTexelBuffer(set, SRV(2), shared.spanPrefixSumsBuffer)
                        .StorageBuffer(set, SRV(3), shared.cmdParamsBuffer)
                        .StorageBuffer(set, SRV(4), vdp1.vramBuffer);
                }
                writer.StorageTexelBuffer(shared.polyDrawSet, UAV(1), shared.internalSpriteOutBuffer);
                writer.StorageBuffer(shared.polyDrawMSBSet, UAV(1), vdp1.fbramBuffer);
                writer.StorageTexelBuffer(shared.polyDrawOITSet, UAV(1), shared.oitListHeadsBuffer)
                    .StorageBuffer(shared.polyDrawOITSet, UAV(2), shared.oitFragmentsBuffer)
                    .StorageBuffer(shared.polyDrawOITSet, UAV(3), shared.oitCounterBuffer);

                writer.StorageBuffer(shared.outputMergerSet, UAV(1), vdp1.fbramBuffer)
                    .StorageTexelBuffer(shared.outputMergerSet, UAV(2), shared.internalSpriteOutBuffer);
                writer.StorageBuffer(shared.outputMergerOITSet, SRV(1), shared.oitFragmentsBuffer)
                    .StorageBuffer(shared.outputMergerOITSet, UAV(1), vdp1.fbramBuffer)
                    .StorageTexelBuffer(shared.outputMergerOITSet, UAV(2), shared.oitListHeadsBuffer);

                writer.StorageBuffer(shared.drawSpriteSet, SRV(1), shared.layerRenderParamsBuffer)
                    .StorageBuffer(shared.drawSpriteSet, SRV(2), vdp2.vramBuffer)
                    .UniformTexelBuffer(shared.drawSpriteSet, SRV(3), shared.cramColorBuffer)
                    .StorageBuffer(shared.drawSpriteSet, SRV(4), shared.rotParamBasesBuffer)
                    .StorageBuffer(shared.drawSpriteSet, SRV(5), vdp1.fbramBuffer)
                    .StorageImage(shared.drawSpriteSet, UAV(0), shared.layerOutTexture)
                    .StorageImage(shared.drawSpriteSet, UAV(1), shared.spriteAttrsTexture);

                writer.StorageBuffer(shared.drawBGsSet, SRV(1), shared.layerRenderParamsBuffer)
                    .StorageBuffer(shared.drawBGsSet, SRV(2), vdp2.vramBuffer)
                    .UniformTexelBuffer(shared.drawBGsSet, SRV(3), shared.cramColorBuffer)
                    .StorageBuffer(shared.drawBGsSet, SRV(4), shared.cramRotCoeffBuffer)
                    .StorageBuffer(shared.drawBGsSet, SRV(5), shared.rotParamBasesBuffer)
                    .SampledImage(shared.drawBGsSet, SRV(6), shared.spriteAttrsTexture)
                    .StorageImage(shared.drawBGsSet, UAV(0), shared.layerOutTexture)
                    .StorageImage(shared.drawBGsSet, UAV(1), shared.rbgLineColorOutTexture)
                    .StorageImage(shared.drawBGsSet, UAV(2), shared.colorCalcWindowTexture);

                writer.StorageBuffer(shared.composeSet, SRV(1), shared.composeParamsBuffer)
                    .SampledImage(shared.composeSet, SRV(2), shared.layerOutTexture)
                    .UniformTexelBuffer(shared.composeSet, SRV(3), shared.lnclBackBuffer)
                    .SampledImage(shared.composeSet, SRV(4), shared.rbgLineColorOutTexture)
                    .SampledImage(shared.composeSet, SRV(5), shared.spriteAttrsTexture)
                    .SampledImage(shared.composeSet, SRV(6), shared.colorCalcWindowTexture)
                    .StorageImage(shared.composeSet, UAV(0), vdp2.compositeOutTexture);
            }
            writer.Commit(device);
        }

        // =============================================================================================================
        // Initial contents

        if (auto result = BeginCommands(); !result) {
            return result;
        }

        // Move all images to the GENERAL layout and clear them
        {
            const GpuImage *images[] = {&vdp2.compositeOutTexture, &shared.layerOutTexture,
                                        &shared.rbgLineColorOutTexture, &shared.colorCalcWindowTexture,
                                        &shared.spriteAttrsTexture};
            std::vector<VkImageMemoryBarrier> barriers{};
            for (const GpuImage *image : images) {
                barriers.push_back({
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image->image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image->layers},
                });
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, static_cast<uint32>(barriers.size()), barriers.data());

            const VkClearColorValue clearValue{};
            for (const GpuImage *image : images) {
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image->layers};
                vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &range);
            }
            barrierTracker.MarkPending();
        }

        // Zero-fill buffers, matching the zero-initialized committed resources of Direct3D 12
        {
            for (const GpuBuffer *buffer : {&vdp1.vramBuffer, &vdp1.fbramBuffer, &vdp1.fbramWritesBuffer,
                                            &vdp2.vramBuffer}) {
                FillBuffer(*buffer, 0, VK_WHOLE_SIZE, 0);
            }
            for (const GpuBuffer *buffer :
                 {&shared.spanParamsBuffer, &shared.spanPrefixSumsBuffer, &shared.cmdParamsBuffer,
                  &shared.internalSpriteOutBuffer, &shared.oitFragmentsBuffer, &shared.oitCounterBuffer,
                  &shared.cramColorBuffer, &shared.cramRotCoeffBuffer, &shared.lnclBackBuffer,
                  &shared.rotParamBasesBuffer, &shared.layerRenderParamsBuffer, &shared.composeParamsBuffer}) {
                FillBuffer(*buffer, 0, VK_WHOLE_SIZE, 0);
            }
            if (resolutionScale > 1) {
                FillBuffer(vdp1.fbramNativeBuffer, 0, VK_WHOLE_SIZE, 0);
            }
            // Empty OIT fragment lists
            FillBuffer(shared.oitListHeadsBuffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFF);
        }
        barrierTracker.Flush(cmd);

        initialized = true;
        Reset();

        return {};
    }

    void Shutdown() {
        if (vk.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vk.device);
        }
    }

    /// @brief Selects the descriptor set layout for a polygon drawing shader variant.
    /// @param[in] index the shader index
    /// @return the layout used by the variant
    const ComputeLayout &GetPolyDrawLayout(size_t index) const {
        const size_t shadingMode = bit::extract<1, 2>(index);
        return shadingMode == 2   ? vdp1.polyDrawOITRootSig
               : shadingMode == 3 ? vdp1.polyDrawMSBRootSig
                                  : vdp1.polyDrawRootSig;
    }

    util::ValueResult<std::vector<uint32>> LoadShader(const char *path) {
        if (!g_fsShaders.is_file(path)) {
            return util::ErrorMessage{fmt::format("Embedded file not found: {}", path)};
        }
        auto file = g_fsShaders.open(path);
        const size_t size = file.size();
        if (size == 0 || size % sizeof(uint32) != 0) {
            return util::ErrorMessage{fmt::format("Invalid SPIR-V file: {}", path)};
        }
        // Copy into a uint32 vector to guarantee alignment for vkCreateShaderModule
        std::vector<uint32> code(size / sizeof(uint32));
        std::memcpy(code.data(), file.begin(), size);
        return code;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // State

    void Reset() {
        // VDP1
        vdp1.vramDirty.SetAll();
        if (auto result = VDP1UploadFBRAM(); !result) {
            Report<grp::vk_base>(devlog::level::warn, "Failed to upload VDP1 FBRAM: {}", result.Error().message);
        }

        // VDP2
        vdp2.vramDirty.SetAll();
        ++vdp2.cramGeneration;
        ++vdp2.layerRenderParamsGeneration;
        ++vdp2.composeParamsGeneration;

        vdp2.nextLayerRenderLine = 0;
        vdp2.nextComposeLine = 0;

        VDP2CacheAllCRAMColors();
        VDP2UpdateEnabledLayers();
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Common

    /// @brief Attempts to allocate a chunk of data in the specified upload buffer, waiting to free up space if needed.
    /// @param[in] uploadBuffer the upload buffer
    /// @param[in] size the requested size
    /// @param[in] alignment the requested alignment
    /// @param[out] outAlloc receives the allocation information
    /// @return nothing on success, an error message on failure
    util::VoidResult<> AllocateUploadBuffer(UploadRingBuffer &uploadBuffer, size_t size, size_t alignment,
                                            UploadAllocation &outAlloc) {
        // Sanity check: the upload buffer can hold transfers of this size
        YMIR_DEV_ASSERT(size < uploadBuffer.GetSize());

        if (!uploadBuffer.Allocate(size, alignment, frames.GetCompletedValue(vk.device), outAlloc)) {
            // Block until next fence completes and retry
            const uint64 waitValue = uploadBuffer.FindFenceValueForAllocation(size, alignment);
            frames.WaitForValue(vk.device, waitValue);

            // At this point, we really should be able to allocate the buffer
            if (!uploadBuffer.Allocate(size, alignment, frames.GetCompletedValue(vk.device), outAlloc)) {
                // TODO: consider increasing the upload buffer size or allocating overflow buffers.
                // For now we'll just log the error and fail
                YMIR_DEV_CHECK();
                std::string message = fmt::format("Failed to allocate {} bytes (align {}) in {} upload buffer", size,
                                                  alignment, uploadBuffer.GetDebugName());
                Report<grp::vk_vdp2>(devlog::level::warn, "{}", message);
                return util::ErrorMessage{std::move(message)};
            }
        }
        return {};
    }

    void UpdateEnhancements() {
        VDP1CommonRenderParams &params1 = vdp1.cpuCommonRenderParams;
        params1.enhancements.deinterlace = enhancements.deinterlace;
        params1.enhancements.transparentMeshes = enhancements.transparentMeshes;
        params1.enhancements.resolutionScale = resolutionScale - 1;

        vdp2.cpuCommonRenderParams.enhancements = params1.enhancements;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // VDP1 rendering

    void VDP1WriteVRAM(uint32 address) {
        // Submit spans if the target address is in use by a textured polygon in the current batch
        if (vdp1.vramTexUsageTracker.IsInUse(address)) [[unlikely]] {
            devlog::debug<grp::vk_vdp1>("VDP1 VRAM write to {:05X} which is in use by pending spans; submitting now",
                                          address);
            VDP1SubmitSpans();
        }
        vdp1.vramDirty.Set(address >> VDP1Resources::kVRAMDirtyBitmapChunkSizeShift);
    }

    void VDP1DownloadFBRAM() {
        // Bail out if there have been no changes
        if (!vdp1.fbramReadbackCopyPending) {
            return;
        }
        vdp1.fbramReadbackCopyPending = false;

        // Submit the work recorded so far, wait for it and continue recording the same frame
        RecordHostReadBarrier();
        const uint64 fenceValue = frames.GetNextFenceValue();
        if (auto result = SubmitCommands(); !result) {
            Report<grp::vk_base>(devlog::level::warn, "{}", result.Error().message);
            return;
        }
        frames.WaitForValue(vk.device, fenceValue);
        if (auto result = BeginCommands(); !result) {
            Report<grp::vk_base>(devlog::level::warn, "{}", result.Error().message);
            return;
        }
        if (deviceLost) {
            return;
        }

        // Copy downloaded FBRAM
        vdp1.fbramDownloadBuffer.Invalidate();
        memcpy(vdpState.mem1.FBRAM.data(), vdp1.fbramDownloadBuffer.mapped, kVDP1FBRAMSize * 2);
    }

    void VDP1SyncFB() {
        // Submit pending spans to ensure we're synced as far as possible
        VDP1SubmitSpans();

        // Copy FBRAM to the download buffer if modified
        VDP1CopyFBRAMToReadback();

        // Download FBRAM from the download buffer
        VDP1DownloadFBRAM();
    }

    void VDP1DebugSyncFB() {
        vdp1.fbramDebugSyncRequest.store(true, std::memory_order_release);
    }

    void VDP1WriteFB(uint32 address, uint32 size) {
        for (uint32 i = 0; i < size; ++i) {
            vdp1.fbramDirty.Set(address + i);
        }
    }

    [[nodiscard]] util::VoidResult<> VDP1FlushVRAM() {
        if (!vdp1.vramDirty) {
            return {};
        }

        GpuBuffer &dstResource = vdp1.vramBuffer;

        // Emit barrier transition
        barrierTracker.Flush(cmd);

        // Upload all modified VRAM chunks
        size_t pos, count = 0;
        UploadAllocation alloc{};
        for (pos = vdp1.vramDirty.FindNext(count); pos < vdp1.vramDirty.Size();
             pos = vdp1.vramDirty.FindNext(count, pos + count)) {
            const uint32 vramOffset = pos << VDP2Resources::kVRAMDirtyBitmapChunkSizeShift;
            const uint32 size = count << VDP2Resources::kVRAMDirtyBitmapChunkSizeShift;

            // Get upload buffer chunk for this transfer
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{
                    fmt::format("Failed to allocate upload buffer for VDP1 VRAM chunk: {}", result.Error().message)};
            }

            // Upload VRAM chunk
            memcpy(alloc.data, &vdpState.mem1.VRAM[vramOffset], size);
            CopyFromUpload(dstResource, vramOffset, alloc, size);
        }
        vdp1.vramDirty.ClearAll();

        return {};
    }

    [[nodiscard]] util::VoidResult<> VDP1UploadFBRAM() {
        // Size of both framebuffers, scaled by the internal resolution
        const size_t frameSize = ScaledFBSize() * 2;

        GpuBuffer &dstResource = vdp1.fbramBuffer;

        // Transition to copy
        barrierTracker.Flush(cmd);

        // Get upload buffer chunk
        UploadAllocation alloc{};
        if (auto result = AllocateUploadBuffer(uploadBuffer, frameSize, 4, alloc); !result) {
            return util::ErrorMessage{
                fmt::format("Failed to allocate upload buffer for VDP1 FBRAM: {}", result.Error().message)};
        }

        // Upload buffer
        if (resolutionScale == 1) {
            memcpy(alloc.data, vdpState.mem1.FBRAM.data(), frameSize);
        } else {
            // Expand every native pixel into a block, following the framebuffer line layout used by the shaders
            const VDP1Regs &regs1 = vdpState.regs1;
            const size_t pixelSize = regs1.pixel8Bits ? 1 : 2;
            const size_t lineSize = regs1.fbSizeH * pixelSize;
            const size_t scaledLineSize = lineSize * resolutionScale;
            const size_t numLines = kVDP1FBRAMSize / lineSize;
            for (size_t fb = 0; fb < 2; ++fb) {
                const uint8 *src = vdpState.mem1.FBRAM[fb].data();
                uint8 *dst = static_cast<uint8 *>(alloc.data) + fb * ScaledFBSize();
                for (size_t line = 0; line < numLines; ++line) {
                    uint8 *scaledLine = dst + line * resolutionScale * scaledLineSize;
                    for (size_t pixel = 0; pixel < lineSize / pixelSize; ++pixel) {
                        const uint8 *srcPixel = src + line * lineSize + pixel * pixelSize;
                        for (size_t dx = 0; dx < resolutionScale; ++dx) {
                            memcpy(scaledLine + (pixel * resolutionScale + dx) * pixelSize, srcPixel, pixelSize);
                        }
                    }
                    for (size_t dy = 1; dy < resolutionScale; ++dy) {
                        memcpy(scaledLine + dy * scaledLineSize, scaledLine, scaledLineSize);
                    }
                }
            }
        }
        CopyFromUpload(dstResource, frameSize * 0, alloc, frameSize);
        if (enhancements.deinterlace) {
            CopyFromUpload(dstResource, frameSize * 1, alloc, frameSize);
        }

        if (enhancements.transparentMeshes) {
            // Clear mesh buffers
            // Buffers 2 and 3 are the main and alternate mesh buffers respectively
            const size_t endBuffer = enhancements.deinterlace ? 4 : 3;
            FillBuffer(dstResource, frameSize * 2, frameSize * (endBuffer - 2), 0);
        }

        // No longer dirty
        vdp1.fbramDirty.ClearAll();

        return {};
    }

    [[nodiscard]] util::VoidResult<> VDP1FlushFBRAM() {
        if (!vdp1.fbramDirty) {
            return {};
        }

        GpuBuffer &dstResource = vdp1.fbramWritesBuffer;

        // Transition to copy
        barrierTracker.Flush(cmd);

        // Because we're syncing FBRAM at the beginning of a VDP2 frame, the display framebuffer bit has already been
        // flipped by the VDP1 swap framebuffers operation. We'll have to pick the opposite buffer here to copy into.
        auto &fb = vdpState.mem1.FBRAM[vdpState.fbIndex.draw];

        // Group modified FBRAM writes into 32-bit chunks
        std::vector<VDP1FBRAMWrite> writes{};
        size_t pos, count = 0;
        const uint64 *bitmapData = vdp1.fbramDirty.GetData();
        for (pos = vdp1.fbramDirty.FindNextGroup<4>(count); pos < vdp1.fbramDirty.Size();
             pos = vdp1.fbramDirty.FindNextGroup<4>(count, pos + count)) {
            const uint32 baseAddress = pos;

            for (size_t i = 0; i < count; i += 4) {
                VDP1FBRAMWrite &write = writes.emplace_back();
                write.address = baseAddress + i;

                const uint8 bits = bitmapData[write.address >> 6u] >> (write.address & 63u);
                write.andMask = 0;
                for (uint32 j = 0; j < 4; ++j) {
                    const uint32 shift = j * 8u;
                    if (((bits >> j) & 1u) == 0u) {
                        write.andMask |= (0xFFu << shift);
                    }
                }
                write.orMask = util::ReadLE<uint32>(&fb[write.address]);
                write.orMask &= ~write.andMask;
            }
        }
        vdp1.fbramDirty.ClearAll();

        assert(!writes.empty());

        // Get upload buffer chunk for this transfer
        UploadAllocation alloc{};
        const size_t size = writes.size() * sizeof(VDP1FBRAMWrite);
        if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
            return util::ErrorMessage{
                fmt::format("Failed to allocate upload buffer for VDP1 FBRAM writes list: {}", result.Error().message)};
        }

        // Upload list
        memcpy(alloc.data, writes.data(), size);
        CopyFromUpload(dstResource, 0, alloc, size);

        // Transition to SRV usage
        barrierTracker.Flush(cmd);

        // Dispatch FBRAM write compute shader
        const uint32 writeCount = writes.size();
        struct {
            VDP1CommonRenderParams common;
            HLSLuint writeCount;
        } pushConstants{vdp1.cpuCommonRenderParams, writeCount};
        static_assert(offsetof(decltype(pushConstants), writeCount) == sizeof(VDP1CommonRenderParams));
        Dispatch(vdp1.fbramWritePSO, vdp1.fbramWriteRootSig, vdp1.fbramWriteSet, &pushConstants,
                 sizeof(VDP1CommonRenderParams) + sizeof(HLSLuint), (writeCount + 63) / 64, 1, 1);

        // Mark GPU-side FBRAM as dirty
        vdp1.fbramReadbackDirty = true;

        return {};
    }

    void VDP1EraseFramebuffer(uint64 cycles) {
        // Vertical scale is doubled in double-interlace mode
        const VDP1Regs &regs1 = vdpState.regs1;
        const VDP2Regs &regs2 = vdpState.regs2;
        const bool doubleDensity = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;
        const uint32 scaleV = doubleDensity ? 1u : 0u;

        // Constrain erase area to certain limits based on current resolution
        const uint32 maxH = (regs2.TVMD.HRESOn & 1) ? 428 : 400;
        const uint32 maxV = VRes >> scaleV;

        vdp1.cpuEraseParams.coords.x1 = std::min<uint32>(regs1.eraseX1Latch, maxH) >> 3u;
        vdp1.cpuEraseParams.coords.y1 = std::min<uint32>(regs1.eraseY1Latch, maxV);
        vdp1.cpuEraseParams.coords.x3 = std::min<uint32>(regs1.eraseX3Latch, maxH) >> 3u;
        vdp1.cpuEraseParams.coords.y3 = std::min<uint32>(regs1.eraseY3Latch, maxV);
        vdp1.cpuEraseParams.coords.scaleV = scaleV;

        vdp1.cpuEraseParams.erase.writeValue = bit::byte_swap<uint16>(regs1.eraseWriteValueLatch);
        vdp1.cpuEraseParams.erase.addressShift = regs1.eraseOffsetShift - 8;

        vdp1.cpuEraseParams.vblank.enable = cycles != 0;
        if (vdp1.cpuEraseParams.vblank.enable) {
            // Compute last line and pixel that can be drawn with the given cycle budget
            const uint32 lineWidth = (vdp1.cpuEraseParams.coords.x3 << 3u) - (vdp1.cpuEraseParams.coords.x1 << 3u);
            if (lineWidth > 0) {
                vdp1.cpuEraseParams.vblank.maxY = cycles / lineWidth;
                vdp1.cpuEraseParams.vblank.maxX = cycles % lineWidth;
            } else {
                vdp1.cpuEraseParams.vblank.maxY = 0;
                vdp1.cpuEraseParams.vblank.maxX = 0;
            }
        }

        // Do framebuffer erase
        const auto &erase = vdp1.cpuEraseParams;
        const uint32 width = (erase.coords.x3 << 3) - (erase.coords.x1 << 3) + 1;
        const uint32 height = erase.coords.y3 - erase.coords.y1 + 1;

        VDP1UpdateCommonRenderParams();
        vdp1.cpuCommonRenderParams.displayParams.drawFB = vdpState.fbIndex.display;

        // Transition FBRAM to UAV usage
        barrierTracker.Flush(cmd);

        // Dispatch erase shader
        const struct {
            VDP1CommonRenderParams common;
            VDP1EraseParams erase;
        } pushConstants{vdp1.cpuCommonRenderParams, vdp1.cpuEraseParams};
        if (resolutionScale > 1) {
            // The scaled erase shader processes the entire framebuffer in groups of 1024 32-bit words
            const uint32 numGroups = static_cast<uint32>((ScaledFBSize() / sizeof(uint32) + 1023) / 1024);
            Dispatch(vdp1.erasePSO, vdp1.eraseRootSig, vdp1.eraseSet, &pushConstants, sizeof(pushConstants),
                     numGroups, 1, 1);
        } else {
            Dispatch(vdp1.erasePSO, vdp1.eraseRootSig, vdp1.eraseSet, &pushConstants, sizeof(pushConstants),
                     (width + 63) / 64, (height + 31) / 32, 1);
            // NOTE: works on 32-bit units, so two writes per thread, hence why (width+63)/64 instead of +31/32
        }

        // Mark GPU-side FBRAM as dirty
        vdp1.fbramReadbackDirty = true;
    }

    void VDP1SwapFramebuffer() {
        // Submit any pending spans
        auto spanResult = VDP1SubmitSpans();
        if (spanResult) {
            if (!spanResult.Value()) {
                // Still have to update the rendering parameters
                VDP1UpdateCommonRenderParams();
            }
        } else {
            Report<grp::vk_vdp1>(devlog::level::warn, "VDP1 span submission failed: {}", spanResult.Error().message);
        }

        VDP1CopyFBRAMToReadback();
    }

    /// @brief Copies GPU-modified FBRAM to the download buffer.
    void VDP1CopyFBRAMToReadback() {
        if (!vdp1.fbramReadbackDirty) {
            return;
        }
        vdp1.fbramReadbackDirty = false;
        vdp1.fbramReadbackCopyPending = true;

        // Download FBRAM
        barrierTracker.Flush(cmd);
        if (resolutionScale > 1) {
            // Convert the scaled framebuffers into native FBRAM contents first
            Dispatch(vdp1.fbramDownsamplePSO, vdp1.fbramDownsampleRootSig, vdp1.fbramDownsampleSet,
                     &vdp1.cpuCommonRenderParams, sizeof(vdp1.cpuCommonRenderParams),
                     (kVDP1FBRAMSize * 2 / sizeof(uint32) + 63) / 64, 1, 1);
            barrierTracker.Flush(cmd);
            CopyBuffer(vdp1.fbramNativeBuffer.buffer, 0, vdp1.fbramDownloadBuffer.buffer, 0, kVDP1FBRAMSize * 2);
        } else {
            CopyBuffer(vdp1.fbramBuffer.buffer, 0, vdp1.fbramDownloadBuffer.buffer, 0, kVDP1FBRAMSize * 2);
        }
    }

    void VDP1BeginFrame() {
        const VDP1Regs &regs1 = vdpState.regs1;
        const VDP2Regs &regs2 = vdpState.regs2;
        vdp1.doubleV =
            enhancements.deinterlace && regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity && !regs1.dblInterlaceEnable;
    }

    void VDP1ExecuteCommand(uint32 cmdAddress, VDP1Command::Control control) {
        switch (control.command) {
        case VDP1Command::CommandType::DrawNormalSprite: VDP1Cmd_DrawNormalSprite(cmdAddress, control); break;
        case VDP1Command::CommandType::DrawScaledSprite: VDP1Cmd_DrawScaledSprite(cmdAddress, control); break;
        case VDP1Command::CommandType::DrawDistortedSprite: [[fallthrough]];
        case VDP1Command::CommandType::DrawDistortedSpriteAlt: VDP1Cmd_DrawDistortedSprite(cmdAddress, control); break;

        case VDP1Command::CommandType::DrawPolygon: VDP1Cmd_DrawPolygon(cmdAddress); break;
        case VDP1Command::CommandType::DrawPolylines: [[fallthrough]];
        case VDP1Command::CommandType::DrawPolylinesAlt: VDP1Cmd_DrawPolylines(cmdAddress); break;
        case VDP1Command::CommandType::DrawLine: VDP1Cmd_DrawLine(cmdAddress); break;

        case VDP1Command::CommandType::UserClipping: [[fallthrough]];
        case VDP1Command::CommandType::UserClippingAlt: VDP1Cmd_SetUserClipping(cmdAddress); break;
        case VDP1Command::CommandType::SystemClipping: VDP1Cmd_SetSystemClipping(cmdAddress); break;
        case VDP1Command::CommandType::SetLocalCoordinates: VDP1Cmd_SetLocalCoordinates(cmdAddress); break;
        }
    }

    struct VDP1CommandData {
        VDP1Command::DrawMode mode;
        uint16 color;

        uint32 charAddr;
        VDP1Command::Size size;
    };

    struct VDP1SpanData {
        uint16 cmdIndex;
        VDP1Command::DrawMode mode;
        Color555 gouraud0;
        Color555 gouraud1;

        uint32 charAddr;
        VDP1Command::Size size;
        uint32 texV;
        bool flipH;
        uint32 endCodeIndex;
    };

    util::ValueResult<bool> VDP1SubmitSpans() {
        FrameContext &frameCtx = frames.GetCurrentFrame();
        if (frameCtx.cpuSpanCount == 0) {
            // No spans to dispatch
            frameCtx.cpuCmdCount = 0;
            return false;
        }

        // Clear counters even if we fail to submit them to avoid crashes on extreme cases.
        // Errors should never happen, however.
        util::ScopeGuard sgClearCounters{[&] {
            frameCtx.cpuSpanCount = 0;
            frameCtx.cpuCmdCount = 0;
            vdp1.vramTexUsageTracker.Clear();
        }};

        // We should have a shader selected by now
        assert(vdp1.currPolyDrawShaderIndex != -1);

        VDP1UpdateCommonRenderParams();
        vdp1.cpuPolyDrawParams.numSpans = frameCtx.cpuSpanCount;

        if (auto result = VDP1FlushVRAM(); !result) {
            Report<grp::vk_vdp1>(devlog::level::warn, "VDP1 VRAM flush failed: {}", result.Error().message);
        }
        if (auto result = VDP1FlushFBRAM(); !result) {
            Report<grp::vk_vdp1>(devlog::level::warn, "VDP1 FBRAM flush failed: {}", result.Error().message);
        }

        UploadAllocation alloc{};

        barrierTracker.Flush(cmd);

        // Upload spans
        {
            const size_t size = sizeof(VDP1SpanParams) * frameCtx.cpuSpanCount;
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{fmt::format("Failed to allocate upload buffer for VDP1 span parameters: {}",
                                                      result.Error().message)};
            }
            memcpy(alloc.data, &frameCtx.cpuSpanParams, size);

            CopyFromUpload(shared.spanParamsBuffer, 0, alloc, size);
        }

        // Upload prefix sums
        {
            const size_t size = sizeof(HLSLuint) * (frameCtx.cpuSpanCount + 1);
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{fmt::format("Failed to allocate upload buffer for VDP1 span prefix sums: {}",
                                                      result.Error().message)};
            }
            memcpy(alloc.data, &frameCtx.cpuSpanPrefixSums, size);

            CopyFromUpload(shared.spanPrefixSumsBuffer, 0, alloc, size);
        }

        // Upload commands
        {
            const size_t size = sizeof(VDP1CommandParams) * frameCtx.cpuCmdCount;
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{fmt::format(
                    "Failed to allocate upload buffer for VDP1 commands parameters: {}", result.Error().message)};
            }
            memcpy(alloc.data, &frameCtx.cpuCmdParams, size);

            CopyFromUpload(shared.cmdParamsBuffer, 0, alloc, size);
        }

        const bool isOIT = IsVDP1PolyDrawShaderOIT(vdp1.currPolyDrawShaderIndex);
        const bool isMSB = IsVDP1PolyDrawShaderMSB(vdp1.currPolyDrawShaderIndex);

        if (isOIT) {
            barrierTracker.Flush(cmd);

            // Reset atomic counter
            FillBuffer(shared.oitCounterBuffer, 0, sizeof(HLSLuint), 0);
        }
        barrierTracker.Flush(cmd);

        // Dispatch polygon drawing shader
        const VkDescriptorSet polyDrawSet = isOIT   ? shared.polyDrawOITSet
                                            : isMSB ? shared.polyDrawMSBSet
                                                    : shared.polyDrawSet;
        const struct {
            VDP1CommonRenderParams common;
            VDP1PolyDrawParams polyDraw;
        } polyDrawPushConstants{vdp1.cpuCommonRenderParams, vdp1.cpuPolyDrawParams};
        Dispatch(vdp1.polyDrawPSOs[vdp1.currPolyDrawShaderIndex], GetPolyDrawLayout(vdp1.currPolyDrawShaderIndex),
                 polyDrawSet, &polyDrawPushConstants, sizeof(polyDrawPushConstants),
                 (frameCtx.cpuSpanPrefixSums[frameCtx.cpuSpanCount] + 63) / 64, 1, 1);

        // Merge output into FBRAM if needed.
        // MSB shader writes directly to FBRAM, no merging needed.
        if (!isMSB) {
            const bool isMergerOIT = IsVDP1OutputMergerShaderOIT(vdp1.currOutputMergerShaderIndex);
            barrierTracker.Flush(cmd);

            // Set up parameters
            const ComputeLayout &rootSig = isMergerOIT ? vdp1.outputMergerOITRootSig : vdp1.outputMergerRootSig;
            const VkDescriptorSet mergerSet = isMergerOIT ? shared.outputMergerOITSet : shared.outputMergerSet;
            const VDP1Regs &regs1 = vdpState.regs1;
            const VDP2Regs &regs2 = vdpState.regs2;
            const uint32 pixelsPerEntry = regs1.pixel8Bits ? 4u : 2u; // each entry is 32 bits
            const uint32 mergeW = regs1.fbSizeH * resolutionScale / pixelsPerEntry;
            const uint32 mergeH = regs1.fbSizeV * resolutionScale;
            const uint32 mergeZ = regs2.TVMD.IsInterlaced() && enhancements.deinterlace ? 2 : 1;

            // Dispatch output merger shader
            Dispatch(vdp1.outputMergerPSOs[vdp1.currOutputMergerShaderIndex], rootSig, mergerSet,
                     &vdp1.cpuCommonRenderParams, sizeof(vdp1.cpuCommonRenderParams), (mergeW + 7) / 8,
                     (mergeH + 7) / 8, mergeZ);
        }

        vdp1.fbramReadbackDirty = true;

        return true;
    }

    void VDP1UpdateCommonRenderParams() {
        VDP1CommonRenderParams &params = vdp1.cpuCommonRenderParams;
        const VDP1Regs &regs1 = vdpState.regs1;
        const VDP2Regs &regs2 = vdpState.regs2;
        const bool doubleDensity = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;

        auto &displayParams = params.displayParams;
        displayParams.fbSizeH = std::countr_zero(regs1.fbSizeH) - 9u;
        displayParams.fbSizeV = std::countr_zero(regs1.fbSizeV) - 8u;
        displayParams.pixel8Bits = regs1.pixel8Bits;
        displayParams.doubleDensity = doubleDensity;
        displayParams.dblInterlaceEnable = regs1.dblInterlaceEnable;
        displayParams.dblInterlaceDrawLine = regs1.dblInterlaceDrawLine;
        displayParams.evenOddCoordSelect = regs1.evenOddCoordSelect;
        displayParams.drawFB = vdpState.fbIndex.draw;
    }

    void VDP1SelectPolyDrawShader(VDP1Command::DrawMode mode) {
        // Submit existing spans before switching shaders
        const size_t polyDrawIndex = MakeVDP1PolyDrawShaderIndex(mode);
        const size_t outputMergerIndex = MakeVDP1OutputMergerShaderIndex(mode);
        if (vdp1.currPolyDrawShaderIndex != polyDrawIndex || vdp1.currOutputMergerShaderIndex != outputMergerIndex) {
            VDP1SubmitSpans();
            vdp1.currPolyDrawShaderIndex = polyDrawIndex;
            vdp1.currOutputMergerShaderIndex = outputMergerIndex;
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Internal resolution scaling of VDP1 coordinates
    //
    // Every native pixel becomes a block of resolutionScale x resolutionScale pixels. Clipping areas are scaled to
    // cover whole blocks. Quad vertices on the right/bottom extremes are placed on the far side of their block and
    // vertices on the left/top extremes on the near side, so that rectangles cover exactly the same blocks as the
    // native pixels they span and adjacent sprites and tiles line up without gaps. Vertices in between are placed at
    // the center of their block. Every quad only grows as a result, so polygons sharing edges overlap slightly instead
    // of leaving cracks between them. Lines are drawn through the center of the blocks.

    /// @brief Scales an inclusive lower bound coordinate.
    sint32 ScaleNear(sint32 value) const {
        return value * static_cast<sint32>(resolutionScale);
    }

    /// @brief Scales an inclusive upper bound coordinate.
    sint32 ScaleFar(sint32 value) const {
        return value * static_cast<sint32>(resolutionScale) + static_cast<sint32>(resolutionScale) - 1;
    }

    /// @brief Scales a coordinate to the center of its block.
    sint32 ScaleCenter(sint32 value) const {
        return value * static_cast<sint32>(resolutionScale) + static_cast<sint32>(resolutionScale - 1) / 2;
    }

    /// @brief Scales the vertices of a quad given in VDP1 order (A, B, C, D).
    void ScaleQuad(CoordS32 &a, CoordS32 &b, CoordS32 &c, CoordS32 &d) const {
        if (resolutionScale == 1) {
            return;
        }
        std::array<CoordS32 *, 4> vertices{&a, &b, &c, &d};
        for (uint32 axis = 0; axis < 2; ++axis) {
            sint32 minValue = a.elements[axis];
            sint32 maxValue = a.elements[axis];
            for (const CoordS32 *vertex : vertices) {
                minValue = std::min(minValue, vertex->elements[axis]);
                maxValue = std::max(maxValue, vertex->elements[axis]);
            }
            for (uint32 i = 0; i < 4; ++i) {
                sint32 &value = vertices[i]->elements[axis];
                bool farSide;
                if (minValue == maxValue) {
                    // Zero-sized along this axis: use the vertex roles of a sprite.
                    // B and C are on the right side; C and D are on the bottom side.
                    farSide = axis == 0 ? (i == 1 || i == 2) : (i == 2 || i == 3);
                } else if (value == maxValue) {
                    farSide = true;
                } else if (value == minValue) {
                    farSide = false;
                } else {
                    value = ScaleCenter(value);
                    continue;
                }
                value = farSide ? ScaleFar(value) : ScaleNear(value);
            }
        }
    }

    /// @brief Scales the endpoints of a line.
    void ScaleLine(CoordS32 &a, CoordS32 &b) const {
        if (resolutionScale == 1) {
            return;
        }
        for (CoordS32 *vertex : {&a, &b}) {
            vertex->x() = ScaleCenter(vertex->x());
            vertex->y() = ScaleCenter(vertex->y());
        }
    }

    /// @brief Retrieves the scaled system clipping area limits.
    CoordS32 GetScaledSystemClip() const {
        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        const sint32 sysClipH = vdpState.state1.sysClipH;
        const sint32 sysClipV = (vdpState.state1.sysClipV << doubleV) | doubleV;
        return {ScaleFar(sysClipH), ScaleFar(sysClipV)};
    }

    uint16 VDP1AddCommand(const VDP1CommandData &data, bool textured) {
        FrameContext &frameCtx = frames.GetCurrentFrame();

        // If list is full, flush it
        if (frameCtx.cpuCmdCount + 1 >= kMaxVDP1Commands) {
            VDP1SubmitSpans();
        }

        // Switch polygon drawing shader based on the current settings
        VDP1SelectPolyDrawShader(data.mode);

        const uint16 cmdIndex = frameCtx.cpuCmdCount++;
        VDP1CommandParams &cmdParams = frameCtx.cpuCmdParams[cmdIndex];

        const VDP1State &state = vdpState.state1;
        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        cmdParams.userClip0.x = ScaleNear(state.userClipX0);
        cmdParams.userClip0.y = ScaleNear((state.userClipY0 << doubleV) | doubleV);
        cmdParams.userClip1.x = ScaleFar(state.userClipX1);
        cmdParams.userClip1.y = ScaleFar((state.userClipY1 << doubleV) | doubleV);
        const CoordS32 sysClip = GetScaledSystemClip();
        cmdParams.sysClip.h = sysClip.x();
        cmdParams.sysClip.v = sysClip.y();

        cmdParams.cmdcolr = data.color;
        cmdParams.cmdpmod = data.mode.u16;
        if (textured) {
            cmdParams.cmdsize = data.size.u16;
            cmdParams.cmdsrca = data.charAddr >> 3u;
        }

        return cmdIndex;
    }

    /// @brief Submits the pending spans in the middle of a command, then re-adds the command's parameters so that the
    /// command's remaining spans can keep referencing them.
    void VDP1SubmitSpansMidCommand(VDP1SpanData &data) {
        FrameContext &frameCtx = frames.GetCurrentFrame();
        const VDP1CommandParams cmdParams = frameCtx.cpuCmdParams[data.cmdIndex];
        VDP1SubmitSpans();
        frameCtx.cpuCmdParams[0] = cmdParams;
        frameCtx.cpuCmdCount = 1;
        data.cmdIndex = 0;
    }

    bool VDP1AddSpan(CoordS32 coord0, CoordS32 coord1, VDP1SpanData &data, bool textured, bool antialias) {
        // Discard if completely out of bounds
        if (coord0.x() < 0 && coord1.x() < 0) {
            return false;
        }
        if (coord0.y() < 0 && coord1.y() < 0) {
            return false;
        }
        const CoordS32 sysClip = GetScaledSystemClip();
        const sint32 sysClipH = sysClip.x();
        if (coord0.x() > sysClipH && coord1.x() > sysClipH) {
            return false;
        }
        const sint32 sysClipV = sysClip.y();
        if (coord0.y() > sysClipV && coord1.y() > sysClipV) {
            return false;
        }

        // Mark transparent mesh drawn if that's the case
        if (data.mode.meshEnable && enhancements.transparentMeshes) {
            vdp1.transparentMeshDrawn = true;
        }

        // Determine span length
        LineStepper line{coord0, coord1};

        const uint32 skip = line.SystemClip(sysClipH, sysClipV);
        const uint32 length = line.Length();

        if (length == 0) {
            // Entire line was clipped
            return false;
        }

        const bool isOIT = IsVDP1PolyDrawShaderOIT(vdp1.currPolyDrawShaderIndex);
        const uint32 fragLimit = isOIT ? kMaxVDP1OITFragmentsPerDispatch : kMaxVDP1FragmentsPerDispatch;

        // Submit spans now if the total fragment count would exceed the limit
        FrameContext &frameCtx = frames.GetCurrentFrame();
        if (frameCtx.cpuSpanPrefixSums[frameCtx.cpuSpanCount] + length >= fragLimit) {
            VDP1SubmitSpansMidCommand(data);
        }

        // Append span to list
        VDP1SpanParams &spanParams = frameCtx.cpuSpanParams[frameCtx.cpuSpanCount];

        const auto [x0, y0] = coord0;
        const auto [x1, y1] = coord1;

        spanParams.coord0 = {x0, y0};
        spanParams.coord1 = {x1, y1};
        spanParams.cmdIndex = data.cmdIndex;

        const uint32 dx = abs(x1 - x0);
        const uint32 dy = abs(y1 - y0);
        spanParams.skip = skip;
        spanParams.attrs.antialias = antialias;

        if (data.mode.gouraudEnable) {
            spanParams.gouraud0.r = data.gouraud0.r;
            spanParams.gouraud0.g = data.gouraud0.g;
            spanParams.gouraud0.b = data.gouraud0.b;
            spanParams.gouraud1.r = data.gouraud1.r;
            spanParams.gouraud1.g = data.gouraud1.g;
            spanParams.gouraud1.b = data.gouraud1.b;
        }

        spanParams.attrs.textured = textured;
        if (textured) {
            spanParams.attrs.texV = data.texV;
            spanParams.attrs.flipH = data.flipH;
            spanParams.attrs.endCodeIndex = data.endCodeIndex;
        }

        // Update prefix sum
        HLSLuint &nextSum = frameCtx.cpuSpanPrefixSums[frameCtx.cpuSpanCount + 1];
        const HLSLuint currSum = frameCtx.cpuSpanPrefixSums[frameCtx.cpuSpanCount];
        nextSum = currSum + length;

        // If list is full, flush it
        ++frameCtx.cpuSpanCount;
        if (frameCtx.cpuSpanCount >= frameCtx.cpuSpanParams.size()) {
            VDP1SubmitSpansMidCommand(data);
        }

        // Indicate that the span was drawn
        return true;
    }

    void VDP1PlotTexturedQuad(VDP1SpanData &data, uint32 cmdAddress, VDP1Command::Control control, CoordS32 coordA,
                              CoordS32 coordB, CoordS32 coordC, CoordS32 coordD) {
        QuadStepper quad{coordA, coordB, coordC, coordD};

        if (data.mode.gouraudEnable) {
            const uint32 gouraudTable = static_cast<uint32>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x1C)) << 3u;
            Color555 gouraudA;
            Color555 gouraudB;
            Color555 gouraudC;
            Color555 gouraudD;
            gouraudA.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 0u);
            gouraudB.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 2u);
            gouraudC.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 4u);
            gouraudD.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 6u);
            quad.SetupGouraud(gouraudA, gouraudB, gouraudC, gouraudD);
        }

        // A width of zero results in the first fetched texel being used for the entire texture.
        // We simulate this by reducing the texture height to one.
        const uint32 charSizeH = data.size.H * 8;
        const uint32 charSizeV = data.size.V;
        const bool flipV = control.flipV;
        TextureStepper texVStepper{};
        quad.SetupTexture(texVStepper, charSizeH == 0 ? 1 : charSizeV, flipV);

        data.texV = charSizeV; // out of range value to ensure first iteration fetches end codes

        // Optimization for the case where the quad goes outside the system clipping area.
        // Skip rendering the rest of the quad when a line is clipped after plotting at least one line.
        // The first few lines of the quad could also be clipped; that is accounted for by requiring at least one
        // plotted line. The point is to skip the calculations once the quad iterator reaches a point where no more
        // lines can be plotted because they all sit outside the system clip area.
        //
        // This also handles a degenerate case with a bowtie quad sitting outside the corner of the screen with two
        // points poking into the screen area in a configuration similar to this:
        //
        //                       D
        //                        B
        //   +-----------------+
        //   |            A    |
        //   |               C |
        //   |                 |
        //   |                 |
        //   |                 |
        //   +-----------------+
        //
        // In this case, the line gets fully clipped partway through the quad, but comes back into view at the end, so
        // we need to check for two sequences of plotted lines rather than one.
        bool linePlotted = false;
        int plottedSegmentsCount = 0;
        const int plottedSegmentsMax = quad.IsDegenerate() ? 2 : 1;

        // Mark VRAM as in use for this texture
        uint32 texSize = charSizeH * charSizeV;
        switch (data.mode.colorMode) {
        case 0: [[fallthrough]];       // 4 bpp, 16 colors, bank mode
        case 1: texSize >>= 1u; break; // 4 bpp, 16 colors, lookup table mode
        case 2: [[fallthrough]];       // 8 bpp, 64 colors, bank mode
        case 3: [[fallthrough]];       // 8 bpp, 128 colors, bank mode
        case 4: break;                 // 8 bpp, 256 colors, bank mode
        case 5: texSize <<= 1u; break; // 16 bpp, 32768 colors, RGB mode
        }
        devlog::trace<grp::vk_vdp1>("Tracking VRAM usage for texture: {:05X}..{:05X}", data.charAddr,
                                      data.charAddr + texSize - 1);
        vdp1.vramTexUsageTracker.MarkRange(data.charAddr, texSize);

        // TODO: cache this
        auto findEndCodeIndex = [&](uint32 v) -> uint32 {
            if (data.mode.endCodeDisable) {
                return charSizeH;
            }

            int endCodeCount = 0;

            for (uint32 i = 0; i < charSizeH; ++i) {
                const uint32 u = control.flipH ? charSizeH - 1 - i : i;

                const uint32 charIndex = u + v * charSizeH;

                auto processEndCode = [&](bool endCode) -> bool {
                    if (endCode && !data.mode.endCodeDisable) {
                        ++endCodeCount;
                    }
                    return endCodeCount >= 2;
                };

                // Read next texel
                uint32 color;
                switch (data.mode.colorMode) {
                case 0: // 4 bpp, 16 colors, bank mode
                    color = vdpState.mem1.ReadVRAM<uint8>(data.charAddr + (charIndex >> 1));
                    color = (color >> ((~u & 1) * 4)) & 0xF;
                    if (processEndCode(color == 0xF)) {
                        return u;
                    }
                    break;
                case 1: // 4 bpp, 16 colors, lookup table mode
                    color = vdpState.mem1.ReadVRAM<uint8>(data.charAddr + (charIndex >> 1));
                    color = (color >> ((~u & 1) * 4)) & 0xF;
                    if (processEndCode(color == 0xF)) {
                        return u;
                    }
                    break;
                case 2: // 8 bpp, 64 colors, bank mode
                    color = vdpState.mem1.ReadVRAM<uint8>(data.charAddr + charIndex);
                    if (processEndCode(color == 0xFF)) {
                        return u;
                    }
                    break;
                case 3: // 8 bpp, 128 colors, bank mode
                    color = vdpState.mem1.ReadVRAM<uint8>(data.charAddr + charIndex);
                    if (processEndCode(color == 0xFF)) {
                        return u;
                    }
                    break;
                case 4: // 8 bpp, 256 colors, bank mode
                    color = vdpState.mem1.ReadVRAM<uint8>(data.charAddr + charIndex);
                    if (processEndCode(color == 0xFF)) {
                        return u;
                    }
                    break;
                case 5: // 16 bpp, 32768 colors, RGB mode
                    color = vdpState.mem1.ReadVRAM<uint16>(data.charAddr + charIndex * sizeof(uint16));
                    if (processEndCode(color == 0x7FFF)) {
                        return u;
                    }
                    break;
                }
            };
            return charSizeH;
        };

        // Interpolate linearly over edges A-D and B-C
        for (; quad.CanStep(); quad.Step()) {
            // Plot lines between the interpolated points
            const CoordS32 coordL = quad.LeftEdge().Coord();
            const CoordS32 coordR = quad.RightEdge().Coord();

            while (texVStepper.ShouldStepTexel()) {
                texVStepper.StepTexel();
            }
            texVStepper.StepPixel();

            const uint32 newTexV = texVStepper.Value();
            if (newTexV != data.texV) {
                data.texV = newTexV;
                data.endCodeIndex = findEndCodeIndex(newTexV);
            }

            if (data.mode.gouraudEnable) {
                data.gouraud0 = quad.LeftEdge().GouraudValue();
                data.gouraud1 = quad.RightEdge().GouraudValue();
            }

            if (VDP1AddSpan(coordL, coordR, data, true, true)) {
                if (!linePlotted) {
                    linePlotted = true;
                    ++plottedSegmentsCount;
                }
            } else if (plottedSegmentsCount >= plottedSegmentsMax) {
                // No more lines can be drawn past this point
                break;
            } else {
                linePlotted = false;
            }
        }
    }

    void VDP1Cmd_DrawNormalSprite(uint32 cmdAddress, VDP1Command::Control control) {
        if (!vdpState.state2.layerEnabled[0]) {
            return;
        }
        const VDP1State &state = vdpState.state1;

        const VDP1Command::DrawMode mode{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x04)};
        const uint16 color = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x06);
        const uint32 charAddr = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x08) << 3u;
        const VDP1Command::Size size{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0A)};
        const sint32 xa = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C)) + state.localCoordX;
        const sint32 ya = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E)) + state.localCoordY;
        const uint32 charSizeH = size.H * 8;
        const uint32 charSizeV = size.V;

        const sint32 xb = xa + std::max(charSizeH, 1u) - 1u; // right X
        const sint32 yb = ya + std::max(charSizeV, 1u) - 1u; // bottom Y

        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        const sint32 yAdd = enhancements.deinterlace ? doubleV : 0;

        CoordS32 coordA{xa, ya << doubleV};
        CoordS32 coordB{xb, ya << doubleV};
        CoordS32 coordC{xb, (yb << doubleV) + yAdd};
        CoordS32 coordD{xa, (yb << doubleV) + yAdd};
        ScaleQuad(coordA, coordB, coordC, coordD);

        const VDP1CommandData cmdData{
            .mode = mode,
            .color = color,
            .charAddr = charAddr,
            .size = size,
        };
        const uint16 cmdIndex = VDP1AddCommand(cmdData, true);

        VDP1SpanData spanData{
            .cmdIndex = cmdIndex,
            .mode = mode,
            .charAddr = charAddr,
            .size = size,
            .flipH = control.flipH,
        };

        VDP1PlotTexturedQuad(spanData, cmdAddress, control, coordA, coordB, coordC, coordD);
    }

    void VDP1Cmd_DrawScaledSprite(uint32 cmdAddress, VDP1Command::Control control) {
        if (!vdpState.state2.layerEnabled[0]) {
            return;
        }

        const VDP1State &state = vdpState.state1;

        const VDP1Command::DrawMode mode{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x04)};
        const uint16 color = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x06);
        const uint32 charAddr = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x08) << 3u;
        const VDP1Command::Size size{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0A)};
        const sint32 xa = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C));
        const sint32 ya = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E));

        // Calculated quad coordinates
        sint32 qxa = xa;
        sint32 qya = ya;
        sint32 qxb = xa;
        sint32 qyb = ya;
        sint32 qxc = xa;
        sint32 qyc = ya;
        sint32 qxd = xa;
        sint32 qyd = ya;

        const uint8 zoomPointH = bit::extract<0, 1>(control.zoomPoint);
        const uint8 zoomPointV = bit::extract<2, 3>(control.zoomPoint);

        if (zoomPointH == 0) {
            const sint32 xc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x14));

            qxb = xc;
            qxc = xc;
        } else {
            const sint32 xb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x10));

            switch (zoomPointH) {
            case 1:
                qxb += xb;
                qxc += xb;
                break;
            case 2:
                qxa -= xb >> 1;
                qxb += (xb + 1) >> 1;
                qxc += (xb + 1) >> 1;
                qxd -= xb >> 1;
                break;
            case 3:
                qxa -= xb;
                qxd -= xb;
                break;
            }
        }

        if (zoomPointV == 0) {
            const sint32 yc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x16));

            qyc = yc;
            qyd = yc;
        } else {
            const sint32 yb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x12));

            switch (zoomPointV) {
            case 1:
                qyc += yb;
                qyd += yb;
                break;
            case 2:
                qya -= yb >> 1;
                qyb -= yb >> 1;
                qyc += (yb + 1) >> 1;
                qyd += (yb + 1) >> 1;
                break;
            case 3:
                qya -= yb;
                qyb -= yb;
                break;
            }
        }

        qxa += state.localCoordX;
        qya += state.localCoordY;
        qxb += state.localCoordX;
        qyb += state.localCoordY;
        qxc += state.localCoordX;
        qyc += state.localCoordY;
        qxd += state.localCoordX;
        qyd += state.localCoordY;

        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        const sint32 yAdd = enhancements.deinterlace ? doubleV : 0;

        CoordS32 coordA{qxa, qya << doubleV};
        CoordS32 coordB{qxb, qyb << doubleV};
        CoordS32 coordC{qxc, (qyc << doubleV) + yAdd};
        CoordS32 coordD{qxd, (qyd << doubleV) + yAdd};
        ScaleQuad(coordA, coordB, coordC, coordD);

        const VDP1CommandData cmdData{
            .mode = mode,
            .color = color,
            .charAddr = charAddr,
            .size = size,
        };
        const uint16 cmdIndex = VDP1AddCommand(cmdData, true);

        VDP1SpanData spanData{
            .cmdIndex = cmdIndex,
            .mode = mode,
            .charAddr = charAddr,
            .size = size,
            .flipH = control.flipH,
        };

        VDP1PlotTexturedQuad(spanData, cmdAddress, control, coordA, coordB, coordC, coordD);
    }

    void VDP1Cmd_DrawDistortedSprite(uint32 cmdAddress, VDP1Command::Control control) {
        if (!vdpState.state2.layerEnabled[0]) {
            return;
        }

        const VDP1State &state = vdpState.state1;

        const VDP1Command::DrawMode mode{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x04)};
        const uint16 color = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x06);
        const uint32 charAddr = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x08) << 3u;
        const VDP1Command::Size size{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0A)};
        const sint32 xa = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C)) + state.localCoordX;
        const sint32 ya = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E)) + state.localCoordY;
        const sint32 xb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x10)) + state.localCoordX;
        const sint32 yb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x12)) + state.localCoordY;
        const sint32 xc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x14)) + state.localCoordX;
        const sint32 yc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x16)) + state.localCoordY;
        const sint32 xd = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x18)) + state.localCoordX;
        const sint32 yd = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x1A)) + state.localCoordY;

        const bool isRegularRect = (xa == xd) && (xb == xc) && (ya == yb) && (yc == yd);

        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        const sint32 yAddAB = enhancements.deinterlace && isRegularRect && (ya >= yc) ? doubleV : 0;
        const sint32 yAddCD = enhancements.deinterlace && isRegularRect && (ya < yc) ? doubleV : 0;

        CoordS32 coordA{xa, (ya << doubleV) + yAddAB};
        CoordS32 coordB{xb, (yb << doubleV) + yAddAB};
        CoordS32 coordC{xc, (yc << doubleV) + yAddCD};
        CoordS32 coordD{xd, (yd << doubleV) + yAddCD};
        ScaleQuad(coordA, coordB, coordC, coordD);

        const VDP1CommandData cmdData{
            .mode = mode,
            .color = color,
            .charAddr = charAddr,
            .size = size,
        };
        const uint16 cmdIndex = VDP1AddCommand(cmdData, true);

        VDP1SpanData spanData{
            .cmdIndex = cmdIndex,
            .mode = mode,
            .charAddr = charAddr,
            .size = size,
            .flipH = control.flipH,
        };

        VDP1PlotTexturedQuad(spanData, cmdAddress, control, coordA, coordB, coordC, coordD);
    }

    void VDP1Cmd_DrawPolygon(uint32 cmdAddress) {
        if (!vdpState.state2.layerEnabled[0]) {
            return;
        }

        const VDP1State &state = vdpState.state1;
        const VDP1Command::DrawMode mode{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x04)};

        const uint16 color = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x06);
        const sint32 xa = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C)) + state.localCoordX;
        const sint32 ya = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E)) + state.localCoordY;
        const sint32 xb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x10)) + state.localCoordX;
        const sint32 yb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x12)) + state.localCoordY;
        const sint32 xc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x14)) + state.localCoordX;
        const sint32 yc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x16)) + state.localCoordY;
        const sint32 xd = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x18)) + state.localCoordX;
        const sint32 yd = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x1A)) + state.localCoordY;
        const uint32 gouraudTable = static_cast<uint32>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x1C)) << 3u;

        const bool isRegularRect = (xa == xd) && (xb == xc) && (ya == yb) && (yc == yd);

        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        const sint32 yAddAB = enhancements.deinterlace && isRegularRect && (ya >= yc) ? doubleV : 0;
        const sint32 yAddCD = enhancements.deinterlace && isRegularRect && (ya < yc) ? doubleV : 0;

        CoordS32 coordA{xa, (ya << doubleV) + yAddAB};
        CoordS32 coordB{xb, (yb << doubleV) + yAddAB};
        CoordS32 coordC{xc, (yc << doubleV) + yAddCD};
        CoordS32 coordD{xd, (yd << doubleV) + yAddCD};
        ScaleQuad(coordA, coordB, coordC, coordD);

        Color555 gouraudA;
        Color555 gouraudB;
        Color555 gouraudC;
        Color555 gouraudD;
        if (mode.gouraudEnable) {
            gouraudA.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 0u);
            gouraudB.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 2u);
            gouraudC.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 4u);
            gouraudD.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 6u);
        }

        const VDP1CommandData cmdData{
            .mode = mode,
            .color = color,
        };
        const uint16 cmdIndex = VDP1AddCommand(cmdData, false);

        VDP1SpanData spanData{
            .cmdIndex = cmdIndex,
            .mode = mode,
        };

        QuadStepper quad{coordA, coordB, coordC, coordD};

        if (mode.gouraudEnable) {
            quad.SetupGouraud(gouraudA, gouraudB, gouraudC, gouraudD);
        }

        // Optimization for the case where the quad goes outside the system clipping area.
        // Skip rendering the rest of the quad when a line is clipped after plotting at least one line.
        // The first few lines of the quad could also be clipped; that is accounted for by requiring at least one
        // plotted line. The point is to skip the calculations once the quad iterator reaches a point where no more
        // lines can be plotted because they all sit outside the system clip area.
        //
        // This also handles a degenerate case with a bowtie quad sitting outside the corner of the screen with two
        // points poking into the screen area in a configuration similar to this:
        //
        //                       D
        //                        B
        //   +-----------------+
        //   |            A    |
        //   |               C |
        //   |                 |
        //   |                 |
        //   |                 |
        //   +-----------------+
        //
        // In this case, the line gets fully clipped partway through the quad, but comes back into view at the end, so
        // we need to check for two sequences of plotted lines rather than one.
        bool linePlotted = false;
        int plottedSegmentsCount = 0;
        const int plottedSegmentsMax = quad.IsDegenerate() ? 2 : 1;

        // Interpolate linearly over edges A-D and B-C
        for (; quad.CanStep(); quad.Step()) {
            // Plot lines between the interpolated points
            const CoordS32 coordL = quad.LeftEdge().Coord();
            const CoordS32 coordR = quad.RightEdge().Coord();

            if (mode.gouraudEnable) {
                spanData.gouraud0 = quad.LeftEdge().GouraudValue();
                spanData.gouraud1 = quad.RightEdge().GouraudValue();
            }

            if (VDP1AddSpan(coordL, coordR, spanData, false, true)) {
                if (!linePlotted) {
                    linePlotted = true;
                    ++plottedSegmentsCount;
                }
            } else if (plottedSegmentsCount >= plottedSegmentsMax) {
                // No more lines can be drawn past this point
                break;
            } else {
                linePlotted = false;
            }
        }
    }

    void VDP1Cmd_DrawPolylines(uint32 cmdAddress) {
        if (!vdpState.state2.layerEnabled[0]) {
            return;
        }

        const VDP1State &state = vdpState.state1;
        const VDP1Command::DrawMode mode{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x04)};

        const uint16 color = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x06);
        const sint32 xa = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C)) + state.localCoordX;
        const sint32 ya = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E)) + state.localCoordY;
        const sint32 xb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x10)) + state.localCoordX;
        const sint32 yb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x12)) + state.localCoordY;
        const sint32 xc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x14)) + state.localCoordX;
        const sint32 yc = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x16)) + state.localCoordY;
        const sint32 xd = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x18)) + state.localCoordX;
        const sint32 yd = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x1A)) + state.localCoordY;
        const uint32 gouraudTable = static_cast<uint32>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x1C)) << 3u;

        const bool isRegularRect = (xa == xd) && (xb == xc) && (ya == yb) && (yc == yd);

        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        const sint32 yAddAB = enhancements.deinterlace && isRegularRect && (ya >= yc) ? doubleV : 0;
        const sint32 yAddCD = enhancements.deinterlace && isRegularRect && (ya < yc) ? doubleV : 0;

        CoordS32 coordA{xa, (ya << doubleV) + yAddAB};
        CoordS32 coordB{xb, (yb << doubleV) + yAddAB};
        CoordS32 coordC{xc, (yc << doubleV) + yAddCD};
        CoordS32 coordD{xd, (yd << doubleV) + yAddCD};
        ScaleQuad(coordA, coordB, coordC, coordD);

        Color555 gouraudA;
        Color555 gouraudB;
        Color555 gouraudC;
        Color555 gouraudD;
        if (mode.gouraudEnable) {
            gouraudA.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 0u);
            gouraudB.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 2u);
            gouraudC.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 4u);
            gouraudD.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 6u);
        }

        const VDP1CommandData cmdData{
            .mode = mode,
            .color = color,
        };
        const uint16 cmdIndex = VDP1AddCommand(cmdData, false);

        VDP1SpanData spanData{
            .cmdIndex = cmdIndex,
            .mode = mode,
        };

        if (mode.gouraudEnable) {
            spanData.gouraud0 = gouraudA;
            spanData.gouraud1 = gouraudB;
        }
        VDP1AddSpan(coordA, coordB, spanData, false, false);
        if (mode.gouraudEnable) {
            spanData.gouraud0 = gouraudB;
            spanData.gouraud1 = gouraudC;
        }
        VDP1AddSpan(coordB, coordC, spanData, false, false);
        if (mode.gouraudEnable) {
            spanData.gouraud0 = gouraudC;
            spanData.gouraud1 = gouraudD;
        }
        VDP1AddSpan(coordC, coordD, spanData, false, false);
        if (mode.gouraudEnable) {
            spanData.gouraud0 = gouraudD;
            spanData.gouraud1 = gouraudA;
        }
        VDP1AddSpan(coordD, coordA, spanData, false, false);
    }

    void VDP1Cmd_DrawLine(uint32 cmdAddress) {
        if (!vdpState.state2.layerEnabled[0]) {
            return;
        }

        const VDP1State &state = vdpState.state1;
        const VDP1Command::DrawMode mode{.u16 = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x04)};

        const uint16 color = vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x06);
        const sint32 xa = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C)) + state.localCoordX;
        const sint32 ya = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E)) + state.localCoordY;
        const sint32 xb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x10)) + state.localCoordX;
        const sint32 yb = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x12)) + state.localCoordY;

        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        CoordS32 coordA{xa, ya << doubleV};
        CoordS32 coordB{xb, yb << doubleV};
        ScaleLine(coordA, coordB);

        const VDP1CommandData cmdData{
            .mode = mode,
            .color = color,
        };
        const uint16 cmdIndex = VDP1AddCommand(cmdData, false);

        VDP1SpanData spanData{
            .cmdIndex = cmdIndex,
            .mode = mode,
        };

        if (mode.gouraudEnable) {
            const uint32 gouraudTable = static_cast<uint32>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x1C)) << 3u;
            spanData.gouraud0.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 0u);
            spanData.gouraud1.u16 = vdpState.mem1.ReadVRAM<uint16>(gouraudTable + 2u);
        }

        VDP1AddSpan(coordA, coordB, spanData, false, false);
    }

    void VDP1Cmd_SetUserClipping(uint32 cmdAddress) {
        VDP1State &state = vdpState.state1;
        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        state.userClipX0 = bit::extract<0, 9>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C));
        state.userClipX1 = bit::extract<0, 9>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x14));
        state.userClipY0 = bit::extract<0, 8>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E));
        state.userClipY1 = bit::extract<0, 8>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x16));
    }

    void VDP1Cmd_SetSystemClipping(uint32 cmdAddress) {
        VDP1State &state = vdpState.state1;
        const sint32 doubleV = vdp1.doubleV ? 1 : 0;
        state.sysClipH = bit::extract<0, 9>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x14));
        state.sysClipV = bit::extract<0, 8>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x16));
    }

    void VDP1Cmd_SetLocalCoordinates(uint32 cmdAddress) {
        VDP1State &state = vdpState.state1;
        state.localCoordX = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0C));
        state.localCoordY = bit::sign_extend<13>(vdpState.mem1.ReadVRAM<uint16>(cmdAddress + 0x0E));
    }

    // -----------------------------------------------------------------------------------------------------------------
    // VDP2 rendering

    uint32 HRes = kDefaultResH;
    uint32 VRes = kDefaultResV;
    bool exclusiveMonitor = false;

    void VDP2CacheCRAMColor(uint32 address) {
        CRAMColorCache &colorCache = vdp2.cpuCRAMColorCache;
        switch (vdpState.regs2.vramControl.colorRAMMode) {
        case 0: { // RGB 5:5:5, half CRAM
            const auto value = vdpState.mem2.ReadCRAM<uint16>(address & ~1u);
            const Color555 color5{.u16 = value};
            const Color888 color8 = ConvertRGB555to888(color5);
            ColorR8G8B8A8 &color = colorCache[address >> 1u];
            color.r = color8.r;
            color.g = color8.g;
            color.b = color8.b;
            color.a = color8.msb;
            break;
        }
        case 1: { // RGB 5:5:5, full CRAM
            const auto value = vdpState.mem2.ReadCRAM<uint16>(address & ~1u);
            const Color555 color5{.u16 = value};
            const Color888 color8 = ConvertRGB555to888(color5);
            ColorR8G8B8A8 &color = colorCache[address >> 1u];
            color.r = color8.r;
            color.g = color8.g;
            color.b = color8.b;
            color.a = color8.msb;
            break;
        }
        case 2: [[fallthrough]]; // RGB 8:8:8, full CRAM
        case 3: [[fallthrough]]; // RGB 8:8:8, full CRAM
        default: {
            address = vdpState.mem2.UnmapCRAMAddress<uint8>(address);
            const auto value = vdpState.mem2.ReadCRAM<uint32>(address & ~3u);
            const Color888 color8{.u32 = value};
            ColorR8G8B8A8 &color = colorCache[address >> 2u];
            color.r = color8.r;
            color.g = color8.g;
            color.b = color8.b;
            color.a = color8.msb;
            break;
        }
        }
    }

    void VDP2CacheAllCRAMColors() {
        CRAMColorCache &colorCache = vdp2.cpuCRAMColorCache;
        switch (vdpState.regs2.vramControl.colorRAMMode) {
        case 0: // RGB 5:5:5, half CRAM
            for (uint32 i = 0; i < 1024; ++i) {
                const auto value = vdpState.mem2.ReadCRAM<uint16>(i * sizeof(uint16));
                const Color555 color5{.u16 = value};
                const Color888 color8 = ConvertRGB555to888(color5);
                colorCache[i].r = color8.r;
                colorCache[i].g = color8.g;
                colorCache[i].b = color8.b;
                colorCache[i].a = color8.msb;
            }
            break;
        case 1: // RGB 5:5:5, full CRAM
            for (uint32 i = 0; i < 2048; ++i) {
                const auto value = vdpState.mem2.ReadCRAM<uint16>(i * sizeof(uint16));
                const Color555 color5{.u16 = value};
                const Color888 color8 = ConvertRGB555to888(color5);
                colorCache[i].r = color8.r;
                colorCache[i].g = color8.g;
                colorCache[i].b = color8.b;
                colorCache[i].a = color8.msb;
            }
            break;
        case 2: [[fallthrough]]; // RGB 8:8:8, full CRAM
        case 3: [[fallthrough]]; // RGB 8:8:8, full CRAM
        default:
            for (uint32 i = 0; i < 1024; ++i) {
                const auto value = vdpState.mem2.ReadCRAM<uint32>(i * sizeof(uint32));
                const Color888 color8{.u32 = value};
                colorCache[i].r = color8.r;
                colorCache[i].g = color8.g;
                colorCache[i].b = color8.b;
                colorCache[i].a = color8.msb;
            }
            break;
        }
    }

    void VDP2WriteVRAM(uint32 address) {
        vdp2.vramDirty.Set(address >> VDP2Resources::kVRAMDirtyBitmapChunkSizeShift);
    }

    void VDP2WriteCRAM(uint32 address) {
        ++vdp2.cramGeneration;
        VDP2CacheCRAMColor(address);
    }

    void VDP2WriteReg(uint32 address, uint16 value) {
        struct DirtyFlags {
            bool render = false;
            bool compose = false;
            bool enabledLayers = false;
            bool cram = false;
        };
        static constexpr auto kDirtyFlags = [] {
            std::array<DirtyFlags, 0x11E / sizeof(uint16) + 1> arr{};

            for (uint32 addr : {
                     0x000 /*TVMD*/,   0x002 /*EXTEN*/,  0x006 /*VRSIZE*/, 0x00E /*RAMCTL*/, 0x010 /*CYCA0L*/,
                     0x012 /*CYCA0U*/, 0x014 /*CYCA1L*/, 0x016 /*CYCA1U*/, 0x018 /*CYCB0L*/, 0x01A /*CYCB0U*/,
                     0x01C /*CYCB1L*/, 0x01E /*CYCB1U*/, 0x020 /*BGON*/,   0x022 /*MZCTL*/,  0x024 /*SFSEL*/,
                     0x026 /*SFCODE*/, 0x028 /*CHCTLA*/, 0x02A /*CHCTLB*/, 0x02C /*BMPNA*/,  0x02E /*BMPNB*/,
                     0x030 /*PNCNA*/,  0x032 /*PNCNB*/,  0x034 /*PNCNC*/,  0x036 /*PNCND*/,  0x038 /*PNCR*/,
                     0x03A /*PLSZ*/,   0x03C /*MPOFN*/,  0x03E /*MPOFR*/,  0x040 /*MPABN0*/, 0x042 /*MPCDN0*/,
                     0x044 /*MPABN1*/, 0x046 /*MPCDN1*/, 0x048 /*MPABN2*/, 0x04A /*MPCDN2*/, 0x04C /*MPABN3*/,
                     0x04E /*MPCDN3*/, 0x050 /*MPABRA*/, 0x052 /*MPCDRA*/, 0x054 /*MPEFRA*/, 0x056 /*MPGHRA*/,
                     0x058 /*MPIJRA*/, 0x05A /*MPKLRA*/, 0x05C /*MPMNRA*/, 0x05E /*MPOPRA*/, 0x060 /*MPABRB*/,
                     0x062 /*MPCDRB*/, 0x064 /*MPEFRB*/, 0x066 /*MPGHRB*/, 0x068 /*MPIJRB*/, 0x06A /*MPKLRB*/,
                     0x06C /*MPMNRB*/, 0x06E /*MPOPRB*/, 0x070 /*SCXIN0*/, 0x072 /*SCXDN0*/, 0x074 /*SCYIN0*/,
                     0x076 /*SCYDN0*/, 0x078 /*ZMXIN0*/, 0x07A /*ZMXDN0*/, 0x07C /*ZMYIN0*/, 0x07E /*ZMYDN0*/,
                     0x080 /*SCXIN1*/, 0x082 /*SCXDN1*/, 0x084 /*SCYIN1*/, 0x086 /*SCYDN1*/, 0x088 /*ZMXIN1*/,
                     0x08A /*ZMXDN1*/, 0x08C /*ZMYIN1*/, 0x08E /*ZMYDN1*/, 0x090 /*SCXN2*/,  0x092 /*SCYN2*/,
                     0x094 /*SCXN3*/,  0x096 /*SCYN3*/,  0x098 /*ZMCTL*/,  0x09A /*SCRCTL*/, 0x09C /*VCSTAU*/,
                     0x09E /*VCSTAL*/, 0x0A0 /*LSTA0U*/, 0x0A2 /*LSTA0L*/, 0x0A4 /*LSTA1U*/, 0x0A6 /*LSTA1L*/,
                     0x0A8 /*LCTAU*/,  0x0AA /*LCTAL*/,  0x0AC /*BKTAU*/,  0x0AE /*BKTAL*/,  0x0B0 /*RPMD*/,
                     0x0B2 /*RPRCTL*/, 0x0B4 /*KTCTL*/,  0x0B6 /*KTAOF*/,  0x0B8 /*OVPNRA*/, 0x0BA /*OVPNRB*/,
                     0x0BC /*RPTAU*/,  0x0BE /*RPTAL*/,  0x0C0 /*WPSX0*/,  0x0C2 /*WPSY0*/,  0x0C4 /*WPEX0*/,
                     0x0C6 /*WPEY0*/,  0x0C8 /*WPSX1*/,  0x0CA /*WPSY1*/,  0x0CC /*WPEX1*/,  0x0CE /*WPEY1*/,
                     0x0D0 /*WCTLA*/,  0x0D2 /*WCTLB*/,  0x0D4 /*WCTLC*/,  0x0D6 /*WCTLD*/,  0x0D8 /*LWTA0U*/,
                     0x0DA /*LWTA0L*/, 0x0DC /*LWTA1U*/, 0x0DE /*LWTA1L*/, 0x0E0 /*SPCTL*/,  0x0E2 /*SDCTL*/,
                     0x0E4 /*CRAOFA*/, 0x0E6 /*CRAOFB*/, 0x0E8 /*LNCLEN*/, 0x0EA /*SFPRMD*/, 0x0EC /*CCCTL*/,
                     0x0EE /*SFCCMD*/, 0x0F0 /*PRISA*/,  0x0F2 /*PRISB*/,  0x0F4 /*PRISC*/,  0x0F6 /*PRISD*/,
                     0x0F8 /*PRINA*/,  0x0FA /*PRINB*/,  0x0FC /*PRIR*/,
                 }) {
                arr[addr / sizeof(uint16)].render = true;
            }

            for (uint32 addr : {
                     0x000 /*TVMD*/,   0x006 /*VRSIZE*/, 0x020 /*BGON*/,  0x0E0 /*SPCTL*/, 0x0E2 /*SDCTL*/,
                     0x0E8 /*LNCLEN*/, 0x0EC /*CCCTL*/,  0x100 /*CCRSA*/, 0x102 /*CCRSB*/, 0x104 /*CCRSC*/,
                     0x106 /*CCRSD*/,  0x108 /*CCRNA*/,  0x10A /*CCRNB*/, 0x10C /*CCRR*/,  0x10E /*CCRLB*/,
                     0x110 /*CLOFEN*/, 0x112 /*CLOFSL*/, 0x114 /*COAR*/,  0x116 /*COAG*/,  0x118 /*COAB*/,
                     0x11A /*COBR*/,   0x11C /*COBG*/,   0x11E /*COBB*/,
                 }) {
                arr[addr / sizeof(uint16)].compose = true;
            }

            for (uint32 addr : {0x020 /*BGON*/, 0x028 /*CHCTLA*/, 0x02A /*CHCTLB*/}) {
                arr[addr / sizeof(uint16)].enabledLayers = true;
            }

            arr[0x00E / sizeof(uint16) /*RAMCTL*/].cram = true;

            return arr;
        }();

        if (address <= 0x11E) {
            const auto &dirtyFlags = kDirtyFlags[address / sizeof(uint16)];
            if (dirtyFlags.render) {
                ++vdp2.layerRenderParamsGeneration;
            }
            if (dirtyFlags.compose) {
                ++vdp2.composeParamsGeneration;
            }

            if (dirtyFlags.enabledLayers) {
                VDP2UpdateEnabledLayers();
            }

            if (dirtyFlags.cram) {
                ++vdp2.cramGeneration;
                VDP2CacheAllCRAMColors();
            }
        }
    }

    [[nodiscard]] util::VoidResult<> VDP2FlushVRAM() {
        if (!vdp2.vramDirty) {
            return {};
        }

        GpuBuffer &dstResource = vdp2.vramBuffer;

        // Emit barrier transition
        barrierTracker.Flush(cmd);

        // Upload all modified VRAM chunks
        size_t pos, count = 0;
        UploadAllocation alloc{};
        for (pos = vdp2.vramDirty.FindNext(count); pos < vdp2.vramDirty.Size();
             pos = vdp2.vramDirty.FindNext(count, pos + count)) {
            const uint32 vramOffset = pos << VDP2Resources::kVRAMDirtyBitmapChunkSizeShift;
            const uint32 size = count << VDP2Resources::kVRAMDirtyBitmapChunkSizeShift;

            // Get upload buffer chunk for this transfer
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{
                    fmt::format("Failed to allocate upload buffer for VDP2 VRAM chunk: {}", result.Error().message)};
            }

            // Upload VRAM chunk
            memcpy(alloc.data, &vdpState.mem2.VRAM[vramOffset], size);
            CopyFromUpload(dstResource, vramOffset, alloc, size);
        }
        vdp2.vramDirty.ClearAll();

        return {};
    }

    [[nodiscard]] util::VoidResult<> VDP2FlushCRAM() {
        FrameContext &frameCtx = frames.GetCurrentFrame();

        // The rotation coefficients are only uploaded while an RBG reads them from CRAM, so they are tracked
        // separately from the color cache: enabling the coefficient table after CRAM was written still needs an
        // upload even though the CRAM contents did not change.
        const VDP2Regs &regs2 = vdpState.regs2;
        const bool rotCoeffInCRAM =
            (regs2.bgEnabled[4] || regs2.bgEnabled[5]) && regs2.vramControl.colorRAMCoeffTableEnable;
        const bool colorsDirty = shared.cramGeneration != vdp2.cramGeneration;
        const bool rotCoeffDirty = rotCoeffInCRAM && shared.cramRotCoeffGeneration != vdp2.cramGeneration;
        if (!colorsDirty && !rotCoeffDirty) {
            return {};
        }

        UploadAllocation alloc{};

        // Update color cache
        if (colorsDirty) {
            shared.cramGeneration = vdp2.cramGeneration;

            const size_t size = sizeof(CRAMColorCache);
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{fmt::format("Failed to allocate upload buffer for VDP2 CRAM color cache: {}",
                                                      result.Error().message)};
            }
            memcpy(alloc.data, vdp2.cpuCRAMColorCache.data(), size);

            barrierTracker.Flush(cmd);
            CopyFromUpload(shared.cramColorBuffer, 0, alloc, size);
        }

        // Update rotation coefficients view
        if (rotCoeffDirty) {
            shared.cramRotCoeffGeneration = vdp2.cramGeneration;

            const size_t size = kVDP2CRAMRotCoeffBufferSize;
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{
                    fmt::format("Failed to allocate upload buffer for VDP2 CRAM rotation coefficients: {}",
                                result.Error().message)};
            }
            memcpy(alloc.data, &vdpState.mem2.CRAM[kVDP2CRAMSize / 2], size);

            barrierTracker.Flush(cmd);
            CopyFromUpload(shared.cramRotCoeffBuffer, 0, alloc, size);
        }

        return {};
    }

    void VDP2UpdateCommonRenderParams() {
        // vdp2.cpuCommonRenderParams.startY is updated by the line rendering functions.

        const VDP1Regs &regs1 = vdpState.regs1;
        const VDP2Regs &regs2 = vdpState.regs2;
        VDP2CommonRenderParams &params = vdp2.cpuCommonRenderParams;

        params.displayParams.displayEnable = regs2.TVMD.DISP;
        params.displayParams.borderColorMode = regs2.TVMD.BDCLMD;
        params.displayParams.interlaceMode = static_cast<HLSLuint>(regs2.TVMD.LSMDn);
        params.displayParams.oddField = regs2.TVSTAT.ODD;
        params.displayParams.exclusiveMonitor = exclusiveMonitor;
        params.displayParams.colorRAMMode = regs2.vramControl.colorRAMMode;
        params.displayParams.hiResH = bit::test<1>(regs2.TVMD.HRESOn);
        params.displayParams.palMode = regs2.TVSTAT.PAL;
        params.displayParams.hresMode = regs2.TVMD.HRESOn;
        params.displayParams.vresMode = regs2.TVMD.VRESOn;
        params.displayParams.dblInterlaceEnable = regs1.dblInterlaceEnable;
        params.displayParams.dblInterlaceDrawLine = regs1.dblInterlaceDrawLine;

        params.layerParams.layerEnabled = PackBools<HLSLuint>(vdpState.state2.layerEnabled);
        params.layerParams.bgEnabled = PackBools<HLSLuint>(regs2.bgEnabled);
        params.layerParams.lineColorEnableRBG0 = regs2.bgParams[0].lineColorScreenEnable;
        params.layerParams.lineColorEnableRBG1 = regs2.bgParams[1].lineColorScreenEnable;
        params.layerParams.mosaicH = regs2.mosaicH - 1;
        params.layerParams.mosaicV = regs2.mosaicV - 1;
        params.layerParams.rotParamMode = static_cast<HLSLuint>(regs2.commonRotParams.rotParamMode);
        params.layerParams.restrictedColorCalc = regs2.restrictedColorCalc;
        params.layerParams.extendedColorCalc = regs2.colorCalcParams.extendedColorCalcEnable && regs2.TVMD.HRESOn < 2;
        params.layerParams.useAdditiveBlend = regs2.colorCalcParams.useAdditiveBlend;
        params.layerParams.useSecondScreenRatio = regs2.colorCalcParams.useSecondScreenRatio;
        params.layerParams.colorGradEnable = regs2.colorCalcParams.colorGradEnable;
        params.layerParams.colorGradScreen = static_cast<HLSLuint>(regs2.colorCalcParams.colorGradScreen);

        auto isCoeff = [](RotDataBankSel sel) { return sel == RotDataBankSel::Coefficients; };

        const VRAMControl &vramCtl = regs2.vramControl;
        params.rotParams.coeffTableCRAM = vramCtl.colorRAMCoeffTableEnable;
        params.rotParams.coeffDataAccess = 0;
        if (isCoeff(vramCtl.rotDataBankSelA0)) {
            params.rotParams.coeffDataAccess |= 1u << 0u;
        }
        if (isCoeff(vramCtl.partitionVRAMA ? vramCtl.rotDataBankSelA1 : vramCtl.rotDataBankSelA0)) {
            params.rotParams.coeffDataAccess |= 1u << 1u;
        }
        if (isCoeff(vramCtl.rotDataBankSelB0)) {
            params.rotParams.coeffDataAccess |= 1u << 2u;
        }
        if (isCoeff(vramCtl.partitionVRAMB ? vramCtl.rotDataBankSelB1 : vramCtl.rotDataBankSelB0)) {
            params.rotParams.coeffDataAccess |= 1u << 3u;
        }
        params.rotParams.coeffDataPerDot = vramCtl.perDotRotationCoeffs;

        const RotationParams &rotParamsA = regs2.rotParams[0];
        params.rotParams.coeffATableEnable = rotParamsA.coeffTableEnable;
        params.rotParams.coeffAUseLineColorData = rotParamsA.coeffUseLineColorData;
        params.rotParams.coeffADataSize = rotParamsA.coeffDataSize;
        params.rotParams.coeffADataMode = static_cast<HLSLuint>(rotParamsA.coeffDataMode);

        const RotationParams &rotParamsB = regs2.rotParams[1];
        params.rotParams.coeffBTableEnable = rotParamsB.coeffTableEnable;
        params.rotParams.coeffBUseLineColorData = rotParamsB.coeffUseLineColorData;
        params.rotParams.coeffBDataSize = rotParamsB.coeffDataSize;
        params.rotParams.coeffBDataMode = static_cast<HLSLuint>(rotParamsB.coeffDataMode);

        params.spriteParams.rotate = regs1.fbRotEnable;
        params.spriteParams.pixel8Bits = regs1.pixel8Bits;
        params.spriteParams.type = regs2.spriteParams.type;
        params.spriteParams.fbSizeH = std::countr_zero(regs1.fbSizeH) - 9;
        params.spriteParams.fbSizeV = std::countr_zero(regs1.fbSizeV) - 8;
        params.spriteParams.inHalfResH = false;
        params.spriteParams.outHalfResH = false;
        if (!regs1.hdtvEnable && !regs1.fbRotEnable) {
            if (regs1.pixel8Bits) {
                params.spriteParams.inHalfResH = (regs2.TVMD.HRESOn & 0b110) == 0b000;
            } else {
                params.spriteParams.outHalfResH = (regs2.TVMD.HRESOn & 0b110) == 0b010;
            }
        }
        params.spriteParams.mixedFormat = regs2.spriteParams.mixedFormat;
        params.spriteParams.colorCalcEnable = regs2.spriteParams.colorCalcEnable;
        params.spriteParams.colorCalcValue = regs2.spriteParams.colorCalcValue;
        params.spriteParams.colorCalcCond = static_cast<HLSLuint>(regs2.spriteParams.colorCalcCond);
        params.spriteParams.colorDataOffset = regs2.spriteParams.colorDataOffset >> 8u;
        params.spriteParams.useSpriteWindow = regs2.spriteParams.useSpriteWindow;
        params.spriteParams.windowEnabled = regs2.spriteParams.spriteWindowEnabled;
        params.spriteParams.windowInverted = regs2.spriteParams.spriteWindowInverted;
        params.spriteParams.displayFB = vdpState.fbIndex.display;

        params.spritePriosRatios.x = 0;
        params.spritePriosRatios.y = 0;
        for (uint32 i = 0; i < 4; i++) {
            params.spritePriosRatios.x |= regs2.spriteParams.priorities[i] << (8 * i);
            params.spritePriosRatios.x |= regs2.spriteParams.colorCalcRatios[i] << (8 * i + 3);

            params.spritePriosRatios.y |= regs2.spriteParams.priorities[i + 4] << (8 * i);
            params.spritePriosRatios.y |= regs2.spriteParams.colorCalcRatios[i + 4] << (8 * i + 3);
        }

        params.vcellScroll.tableAddress = regs2.vcellScrollTableAddress;
        params.vcellScroll.inc = regs2.vcellScrollInc >> 2u;

        params.windows.spriteWindowLogic = regs2.spriteParams.windowSet.logic == WindowLogic::And;
        params.windows.spriteW0Enable = regs2.spriteParams.windowSet.enabled[0];
        params.windows.spriteW0Invert = regs2.spriteParams.windowSet.inverted[0];
        params.windows.spriteW1Enable = regs2.spriteParams.windowSet.enabled[1];
        params.windows.spriteW1Invert = regs2.spriteParams.windowSet.inverted[1];

        params.windows.colorCalcWindowLogic = regs2.colorCalcParams.windowSet.logic == WindowLogic::And;
        params.windows.colorCalcW0Enable = regs2.colorCalcParams.windowSet.enabled[0];
        params.windows.colorCalcW0Invert = regs2.colorCalcParams.windowSet.inverted[0];
        params.windows.colorCalcW1Enable = regs2.colorCalcParams.windowSet.enabled[1];
        params.windows.colorCalcW1Invert = regs2.colorCalcParams.windowSet.inverted[1];
        params.windows.colorCalcSWEnable = regs2.colorCalcParams.windowSet.enabled[2];
        params.windows.colorCalcSWInvert = regs2.colorCalcParams.windowSet.inverted[2];

        // NOTE: this is uploaded as 32-bit root constants, not through the upload buffer.
        // No uploads or barriers are needed here.
    }

    util::VoidResult<> VDP2UpdateLayerRenderParams() {
        FrameContext &frameCtx = frames.GetCurrentFrame();
        if (shared.layerRenderParamsGeneration == vdp2.layerRenderParamsGeneration) {
            return {};
        }
        shared.layerRenderParamsGeneration = vdp2.layerRenderParamsGeneration;

        const VDP2Regs &regs2 = vdpState.regs2;

        // These can be either 0 or 8, but we'll condense them to single bits
        auto packVRAMDataOffsets = [](const std::array<uint32, 4> values) {
            uint32 value = 0;
            for (size_t i = 0; i < values.size(); ++i) {
                if (values[i] != 0u) {
                    value |= 1u << i;
                }
            }
            return value;
        };

        // NBG0-3
        for (int i = 0; i < 4; ++i) {
            const BGParams &bgParams = regs2.bgParams[i + 1];
            const NBGLayerState &bgState = vdpState.state2.nbgLayerStates[i];

            const bool bitmap = bgParams.bitmap;

            NBGParams &renderParams = vdp2.cpuLayerRenderParams.nbg[i];
            renderParams.base.enabled = regs2.bgEnabled[i];
            renderParams.base.enableTransparency = bgParams.enableTransparency;
            renderParams.base.bitmap = bgParams.bitmap;
            renderParams.base.priorityNumber = bgParams.priorityNumber;
            renderParams.base.priorityMode = static_cast<HLSLuint>(bgParams.priorityMode);
            renderParams.base.specialFunctionSelect = bgParams.specialFunctionSelect;
            renderParams.base.cellSizeShift = bgParams.cellSizeShift;
            renderParams.base.colorFormat = static_cast<HLSLuint>(bgParams.colorFormat);
            renderParams.base.cramOffset = bgParams.cramOffset;
            renderParams.base.supplScrollCharNum = bgParams.supplScrollCharNum;
            renderParams.base.supplPalNum = bitmap ? bgParams.supplBitmapPalNum : bgParams.supplScrollPalNum;
            renderParams.base.supplSpecialColorCalc =
                bitmap ? bgParams.supplBitmapSpecialColorCalc : bgParams.supplScrollSpecialColorCalc;
            renderParams.base.supplSpecialPriority =
                bitmap ? bgParams.supplBitmapSpecialPriority : bgParams.supplScrollSpecialPriority;
            renderParams.base.mosaicEnable = bgParams.mosaicEnable;
            renderParams.base.colorCalcEnable = bgParams.colorCalcEnable;
            renderParams.base.extChar = bgParams.extChar;
            renderParams.base.twoWordChar = bgParams.twoWordChar;
            renderParams.base.patNameAccess = PackBools<HLSLuint>(bgParams.patNameAccess);
            renderParams.base.charPatAccess = PackBools<HLSLuint>(bgParams.charPatAccess);
            renderParams.base.charPatDelay = PackBools<HLSLuint>(bgParams.charPatDelay);
            renderParams.base.vramDataOffset = packVRAMDataOffsets(bgParams.vramDataOffset);
            renderParams.base.specialColorCalcMode = static_cast<HLSLuint>(bgParams.specialColorCalcMode);
            renderParams.base.pageShift = {bgParams.pageShiftH, bgParams.pageShiftV};
            renderParams.base.bitmapSize = {bgParams.bitmapSizeH, bgParams.bitmapSizeV};
            renderParams.base.bitmapBaseAddress = bgParams.bitmapBaseAddress;
            renderParams.base.windowParams.base.windowLogicAnd = bgParams.windowSet.logic == WindowLogic::And;
            renderParams.base.windowParams.base.window0Enable = bgParams.windowSet.enabled[0];
            renderParams.base.windowParams.base.window0Invert = bgParams.windowSet.inverted[0];
            renderParams.base.windowParams.base.window1Enable = bgParams.windowSet.enabled[1];
            renderParams.base.windowParams.base.window1Invert = bgParams.windowSet.inverted[1];
            renderParams.base.windowParams.spriteWindowEnable = bgParams.windowSet.enabled[2];
            renderParams.base.windowParams.spriteWindowInvert = bgParams.windowSet.inverted[2];

            renderParams.scrollAmount = {bgParams.scrollAmountH, bgParams.scrollAmountV};
            renderParams.scrollInc = {bgParams.scrollIncH, bgParams.scrollIncV};
            renderParams.pageBaseAddresses = bgParams.pageBaseAddresses;
            renderParams.vcellScrollEnable = bgParams.vcellScrollEnable;
            renderParams.lineScrollXEnable = bgParams.lineScrollXEnable;
            renderParams.lineScrollYEnable = bgParams.lineScrollYEnable;
            renderParams.lineZoomEnable = bgParams.lineZoomEnable;
            renderParams.lineScrollInterval = bgParams.lineScrollInterval;
            renderParams.lineScrollTableAddress = bgState.lineScrollTableAddress;
            renderParams.vcellScrollOffset = bgState.vcellScrollOffset;
            renderParams.vcellScrollDelay = bgState.vcellScrollDelay;
            renderParams.vcellScrollRepeat = bgState.vcellScrollRepeat;
        }

        // RBG0-1 / RotParam A-B
        for (int i = 0; i < 2; ++i) {
            const BGParams &bgParams = regs2.bgParams[i];
            const RotationParams &rotParams = regs2.rotParams[i];

            const bool bitmap = bgParams.bitmap;

            RBGParams &renderParams = vdp2.cpuLayerRenderParams.rbg[i];
            renderParams.base.enabled = regs2.bgEnabled[i + 4];
            renderParams.base.enableTransparency = bgParams.enableTransparency;
            renderParams.base.bitmap = bgParams.bitmap;
            renderParams.base.priorityNumber = bgParams.priorityNumber;
            renderParams.base.priorityMode = static_cast<HLSLuint>(bgParams.priorityMode);
            renderParams.base.specialFunctionSelect = bgParams.specialFunctionSelect;
            renderParams.base.cellSizeShift = bgParams.cellSizeShift;
            renderParams.base.colorFormat = static_cast<HLSLuint>(bgParams.colorFormat);
            renderParams.base.cramOffset = bgParams.cramOffset;
            renderParams.base.supplScrollCharNum = bgParams.supplScrollCharNum;
            renderParams.base.supplPalNum = bitmap ? bgParams.supplBitmapPalNum : bgParams.supplScrollPalNum;
            renderParams.base.supplSpecialColorCalc =
                bitmap ? bgParams.supplBitmapSpecialColorCalc : bgParams.supplScrollSpecialColorCalc;
            renderParams.base.supplSpecialPriority =
                bitmap ? bgParams.supplBitmapSpecialPriority : bgParams.supplScrollSpecialPriority;
            renderParams.base.mosaicEnable = bgParams.mosaicEnable;
            renderParams.base.colorCalcEnable = bgParams.colorCalcEnable;
            renderParams.base.extChar = bgParams.extChar;
            renderParams.base.twoWordChar = bgParams.twoWordChar;
            renderParams.base.patNameAccess = PackBools<HLSLuint>(bgParams.patNameAccess);
            renderParams.base.charPatAccess = PackBools<HLSLuint>(bgParams.charPatAccess);
            renderParams.base.charPatDelay = PackBools<HLSLuint>(bgParams.charPatDelay);
            renderParams.base.vramDataOffset = packVRAMDataOffsets(bgParams.vramDataOffset);
            renderParams.base.specialColorCalcMode = static_cast<HLSLuint>(bgParams.specialColorCalcMode);
            renderParams.base.pageShift = {rotParams.pageShiftH, rotParams.pageShiftV};
            renderParams.base.bitmapSize = {bgParams.bitmapSizeH, bgParams.bitmapSizeV};
            renderParams.base.bitmapBaseAddress = rotParams.bitmapBaseAddress;
            renderParams.base.windowParams.base.windowLogicAnd = bgParams.windowSet.logic == WindowLogic::And;
            renderParams.base.windowParams.base.window0Enable = bgParams.windowSet.enabled[0];
            renderParams.base.windowParams.base.window0Invert = bgParams.windowSet.inverted[0];
            renderParams.base.windowParams.base.window1Enable = bgParams.windowSet.enabled[1];
            renderParams.base.windowParams.base.window1Invert = bgParams.windowSet.inverted[1];
            renderParams.base.windowParams.spriteWindowEnable = bgParams.windowSet.enabled[2];
            renderParams.base.windowParams.spriteWindowInvert = bgParams.windowSet.inverted[2];

            renderParams.screenOverProcess = static_cast<HLSLuint>(rotParams.screenOverProcess);
            renderParams.screenOverPatternName = rotParams.screenOverPatternName;
            renderParams.pageBaseAddresses[0] = vdpState.state2.rbgPageBaseAddresses[0][i];
            renderParams.pageBaseAddresses[1] = vdpState.state2.rbgPageBaseAddresses[1][i];
        }

        // Windows 0 and 1
        for (int i = 0; i < 2; ++i) {
            const WindowParams &windowParams = regs2.windowParams[i];
            VDP2GlobalWindowParams &renderParams = vdp2.cpuLayerRenderParams.windows[i];
            renderParams.start = {windowParams.startX, windowParams.startY};
            renderParams.end = {windowParams.endX, windowParams.endY};
            renderParams.lineWindowTableAddress = windowParams.lineWindowTableAddress;
            renderParams.lineWindowTableEnable = windowParams.lineWindowTableEnable;
        }

        VDP2LayerWindowParams &rotWindows = vdp2.cpuLayerRenderParams.rotWindows;
        rotWindows.windowLogicAnd = regs2.commonRotParams.windowSet.logic == WindowLogic::And;
        rotWindows.window0Enable = regs2.commonRotParams.windowSet.enabled[0];
        rotWindows.window0Invert = regs2.commonRotParams.windowSet.inverted[0];
        rotWindows.window1Enable = regs2.commonRotParams.windowSet.enabled[1];
        rotWindows.window1Invert = regs2.commonRotParams.windowSet.inverted[1];

        VDP2LineBackScreenParams &lnclParams = vdp2.cpuLayerRenderParams.lineScreenParams;
        lnclParams.baseAddress = regs2.lineScreenParams.baseAddress;
        lnclParams.perLine = regs2.lineScreenParams.perLine;

        VDP2LineBackScreenParams &backParams = vdp2.cpuLayerRenderParams.backScreenParams;
        backParams.baseAddress = regs2.backScreenParams.baseAddress;
        backParams.perLine = regs2.backScreenParams.perLine;

        // Special function codes
        vdp2.cpuLayerRenderParams.specialFunctionCodes =
            PackBools<uint32>(regs2.specialFunctionCodes[0].colorMatches) |
            (PackBools<uint32>(regs2.specialFunctionCodes[1].colorMatches) << 8u);

        // Update buffer
        {
            FrameContext &frameCtx = frames.GetCurrentFrame();

            GpuBuffer &dstResource = shared.layerRenderParamsBuffer;
            const size_t size = sizeof(vdp2.cpuLayerRenderParams);
            UploadAllocation alloc{};
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{
                    fmt::format("Failed to allocate upload buffer for VDP2 layer rendering parameters: {}",
                                result.Error().message)};
            }
            memcpy(alloc.data, &vdp2.cpuLayerRenderParams, size);

            // Emit barrier transition
            barrierTracker.Flush(cmd);

            CopyFromUpload(dstResource, 0, alloc, size);
        }

        return {};
    }

    util::VoidResult<> VDP2UpdateComposeParams() {
        FrameContext &frameCtx = frames.GetCurrentFrame();
        if (shared.composeParamsGeneration == vdp2.composeParamsGeneration) {
            return {};
        }
        shared.composeParamsGeneration = vdp2.composeParamsGeneration;

        const VDP2Regs &regs2 = vdpState.regs2;

        auto &params = vdp2.cpuComposeParams;
        params.colorCalcEnable = 0                                               //
                                 | (regs2.spriteParams.colorCalcEnable << 0)     //
                                 | (regs2.bgParams[0].colorCalcEnable << 1)      //
                                 | (regs2.bgParams[1].colorCalcEnable << 2)      //
                                 | (regs2.bgParams[2].colorCalcEnable << 3)      //
                                 | (regs2.bgParams[3].colorCalcEnable << 4)      //
                                 | (regs2.bgParams[4].colorCalcEnable << 5)      //
                                 | (regs2.backScreenParams.colorCalcEnable << 6) //
                                 | (regs2.lineScreenParams.colorCalcEnable << 7) //
            ;
        params.colorOffsetEnable = PackBools<HLSLuint>(regs2.colorOffsetEnable);
        params.colorOffsetSelect = PackBools<HLSLuint>(regs2.colorOffsetSelect);
        params.lineColorEnable = 0                                                 //
                                 | (regs2.spriteParams.lineColorScreenEnable << 0) //
                                 | (regs2.bgParams[0].lineColorScreenEnable << 1)  //
                                 | (regs2.bgParams[1].lineColorScreenEnable << 2)  //
                                 | (regs2.bgParams[2].lineColorScreenEnable << 3)  //
                                 | (regs2.bgParams[3].lineColorScreenEnable << 4)  //
                                 | (regs2.bgParams[4].lineColorScreenEnable << 5)  //
            ;

        params.colorOffsetA.r = bit::sign_extend<9>(regs2.colorOffset[0].r);
        params.colorOffsetA.g = bit::sign_extend<9>(regs2.colorOffset[0].g);
        params.colorOffsetA.b = bit::sign_extend<9>(regs2.colorOffset[0].b);

        params.colorOffsetB.r = bit::sign_extend<9>(regs2.colorOffset[1].r);
        params.colorOffsetB.g = bit::sign_extend<9>(regs2.colorOffset[1].g);
        params.colorOffsetB.b = bit::sign_extend<9>(regs2.colorOffset[1].b);

        for (int i = 0; i < 5; ++i) {
            params.bgColorCalcRatios[i] = regs2.bgParams[i].colorCalcRatio;
        }
        params.backLineColorCalcRatios[0] = regs2.backScreenParams.colorCalcRatio;
        params.backLineColorCalcRatios[1] = regs2.lineScreenParams.colorCalcRatio;

        params.shadowEnable = 1u; // sprite layer
        for (uint32 i = 0; i < 5; ++i) {
            params.shadowEnable |= static_cast<HLSLuint>(regs2.bgParams[i].shadowEnable) << (i + 1u);
        }
        params.shadowEnable |= static_cast<HLSLuint>(regs2.backScreenParams.shadowEnable) << 6u;

        // Update buffer
        {
            FrameContext &frameCtx = frames.GetCurrentFrame();

            GpuBuffer &dstResource = shared.composeParamsBuffer;
            const size_t size = sizeof(vdp2.cpuComposeParams);

            UploadAllocation alloc{};
            if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
                return util::ErrorMessage{
                    fmt::format("Failed to allocate upload buffer for VDP2 layer compositing parameters: {}",
                                result.Error().message)};
            }
            memcpy(alloc.data, &vdp2.cpuComposeParams, size);

            // Emit barrier transition
            barrierTracker.Flush(cmd);

            CopyFromUpload(dstResource, 0, alloc, size);
        }

        return {};
    }

    void VDP2CalcAccessPatterns() {
        if (vdpState.regs2.accessPatternsDirty) {
            ++vdp2.layerRenderParamsGeneration;
        }
        vdpState.state2.CalcAccessPatterns(vdpState.regs2, vdp2.accessPatternsConfig);
    }

    void VDP2InitNBGs() {
        const VDP2Regs &regs2 = vdpState.regs2;

        for (uint32 i = 0; i < 4; ++i) {
            const BGParams &bgParams = regs2.bgParams[i + 1];
            NBGLayerState &nbgState = vdpState.state2.nbgLayerStates[i];

            // NOTE: fracScrollX/Y are computed from scratch in the shader
            nbgState.scrollIncH = bgParams.scrollIncH;

            if (i < 2) {
                nbgState.lineScrollTableAddress = bgParams.lineScrollTableAddress;
            }
        }

        ++vdp2.layerRenderParamsGeneration;
    }

    void VDP2UpdateEnabledLayers() {
        vdpState.state2.UpdateEnabledBGs(vdpState.regs2, vdp2.debugRenderOptions);
    }

    void VDP2CalcVCellScrollDelay() {
        if (vdpState.regs2.accessPatternsDirty) {
            ++vdp2.layerRenderParamsGeneration;
        }
        vdpState.state2.CalcVCellScrollDelay(vdpState.regs2);
    }

    void VDP2DrawLineColorBackScreens(uint32 y) {
        const VDP2Regs &regs = vdpState.regs2;

        if (regs.displayEnabledLatch || y == 0) {
            // Read line color screen color
            const LineBackScreenParams &lineParams = regs.lineScreenParams;
            const uint32 lnclY = lineParams.perLine ? y : 0;
            const uint32 lineAddress = lineParams.baseAddress + lnclY * sizeof(uint16);
            const uint32 cramAddress = vdpState.mem2.ReadVRAM<uint16>(lineAddress);
            vdp2.cpuLnclBack[0][y] = vdp2.cpuCRAMColorCache[cramAddress & 0x7FF];

            // Read back screen color
            const LineBackScreenParams &backParams = regs.backScreenParams;
            const uint32 backY = backParams.perLine ? y : 0;
            const uint32 backAddress = backParams.baseAddress + backY * sizeof(Color555);
            const Color555 color5{.u16 = vdpState.mem2.ReadVRAM<uint16>(backAddress)};
            const Color888 color8 = ConvertRGB555to888(color5);
            vdp2.cpuLnclBack[1][y].r = color8.r;
            vdp2.cpuLnclBack[1][y].g = color8.g;
            vdp2.cpuLnclBack[1][y].b = color8.b;
            vdp2.cpuLnclBack[1][y].a = color8.msb;
        } else {
            vdp2.cpuLnclBack[0][y] = vdp2.cpuLnclBack[0][y - 1];
            vdp2.cpuLnclBack[1][y] = vdp2.cpuLnclBack[1][y - 1];
        }
    }

    util::VoidResult<> VDP2UploadLineColorBackScreens() {
        FrameContext &frameCtx = frames.GetCurrentFrame();

        GpuBuffer &dstResource = shared.lnclBackBuffer;
        const size_t size = sizeof(vdp2.cpuLnclBack);

        UploadAllocation alloc{};
        if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
            return util::ErrorMessage{
                fmt::format("Failed to allocate upload buffer for VDP2 LNCL/BACK screens: {}", result.Error().message)};
        }
        memcpy(alloc.data, &vdp2.cpuLnclBack, size);

        // Emit barrier transition
        barrierTracker.Flush(cmd);

        CopyFromUpload(dstResource, 0, alloc, size);

        return {};
    }

    void VDP2UpdateRotationParameterBases(uint16 y) {
        VDP2Regs &regs2 = vdpState.regs2;
        if (!regs2.bgEnabled[4] && !regs2.bgEnabled[5]) {
            // Skip if no RBGs are enabled
            return;
        }

        const bool readAll = y == 0;

        const uint32 baseAddress = regs2.commonRotParams.baseAddress & 0xFFF7C; // mask bit 6 (shifted left by 1)
        for (uint32 i = 0; i < 2; ++i) {
            VDP2RotParamBase &base = vdp2.cpuRotParamBases[i * kMaxNormalResV + y];
            RotationParams &src = regs2.rotParams[i];

            const uint32 address = baseAddress + i * 0x80;

            base.tableAddress = address;

            if (readAll || src.readXst) {
                base.Xst = bit::extract_signed<6, 28, sint32>(vdpState.mem2.ReadVRAM<uint32>(address + 0x00));
                src.readXst = false;
            } else {
                const VDP2RotParamBase &prevBase = vdp2.cpuRotParamBases[i * kMaxNormalResV + y - 1];
                base.Xst =
                    prevBase.Xst + bit::extract_signed<6, 18, sint32>(vdpState.mem2.ReadVRAM<uint32>(address + 0x0C));
            }

            if (readAll || src.readYst) {
                base.Yst = bit::extract_signed<6, 28, sint32>(vdpState.mem2.ReadVRAM<uint32>(address + 0x04));
                src.readYst = false;
            } else {
                const VDP2RotParamBase &prevBase = vdp2.cpuRotParamBases[i * kMaxNormalResV + y - 1];
                base.Yst =
                    prevBase.Yst + bit::extract_signed<6, 18, sint32>(vdpState.mem2.ReadVRAM<uint32>(address + 0x10));
            }

            if (readAll || src.readKAst) {
                const uint32 KAst = bit::extract<6, 31>(vdpState.mem2.ReadVRAM<uint32>(address + 0x54));
                base.KA = src.coeffTableAddressOffset + KAst;
                src.readKAst = false;
            } else {
                const VDP2RotParamBase &prevBase = vdp2.cpuRotParamBases[i * kMaxNormalResV + y - 1];
                base.KA = prevBase.KA + bit::extract_signed<6, 25>(vdpState.mem2.ReadVRAM<uint32>(address + 0x58));
            }
        }
    }

    util::VoidResult<> VDP2UploadRotationParameterBases() {
        FrameContext &frameCtx = frames.GetCurrentFrame();

        GpuBuffer &dstResource = shared.rotParamBasesBuffer;
        const size_t size = sizeof(vdp2.cpuRotParamBases);

        UploadAllocation alloc{};
        if (auto result = AllocateUploadBuffer(uploadBuffer, size, 4, alloc); !result) {
            return util::ErrorMessage{fmt::format(
                "Failed to allocate upload buffer for VDP2 rotation parameter bases: {}", result.Error().message)};
        }
        memcpy(alloc.data, &vdp2.cpuRotParamBases, size);

        // Emit barrier transition
        barrierTracker.Flush(cmd);

        CopyFromUpload(dstResource, 0, alloc, size);

        return {};
    }

    void VDP2UpdateState() {
        if (auto result = VDP2FlushVRAM(); !result) {
            Report<grp::vk_vdp2>(devlog::level::warn, "VDP2 VRAM flush failed: {}", result.Error().message);
        }
        if (auto result = VDP2FlushCRAM(); !result) {
            Report<grp::vk_vdp2>(devlog::level::warn, "VDP2 CRAM flush failed: {}", result.Error().message);
        }
        VDP2UpdateCommonRenderParams();
        VDP2UpdateLayerRenderParams();
        VDP2UpdateComposeParams();
    }

    void VDP2BeginFrame() {
        vdp2.nextLayerRenderLine = 0;
        vdp2.nextComposeLine = 0;

        VDP2CalcAccessPatterns();
        VDP2InitNBGs();

        VDP2UpdateState();

        // VDP2 consumes VDP1 FBRAM, so it needs to be synced here
        if (auto result = VDP1FlushFBRAM(); !result) {
            Report<grp::vk_vdp1>(devlog::level::warn, "VDP1 FBRAM flush failed: {}", result.Error().message);
        }

        // If the frontend requested a VDP1 FBRAM sync via the debugger, do so now
        bool expect = true;
        if (vdp1.fbramDebugSyncRequest.compare_exchange_strong(expect, false)) {
            VDP1DownloadFBRAM();
        }
    }

    void VDP2RenderLayerLines(uint32 y) {
        // Bail out if there's nothing to render
        if (y < vdp2.nextLayerRenderLine) {
            return;
        }

        // FIXME: this should not be needed
        VDP1SubmitSpans();

        FrameContext &frameCtx = frames.GetCurrentFrame();

        const bool deinterlace = enhancements.deinterlace && vdpState.regs2.TVMD.IsInterlaced();
        const uint32 yShift = deinterlace ? 1u : 0u;

        const uint32 startY = vdp2.nextLayerRenderLine;

        // Determine how many lines to draw and update next scanline counter
        const uint32 baseNumLines = y - startY + 1;
        const uint32 numLines = baseNumLines << yShift;
        vdp2.nextLayerRenderLine = y + 1;

        vdp2.cpuCommonRenderParams.startY = startY << yShift;

        // ---------------------------------------------------------------------

        // Transition resources for rendering layers

        // Compute rotation parameters if any RBGs are enabled
        if (vdpState.regs2.bgEnabled[4] || vdpState.regs2.bgEnabled[5]) {
            VDP2UploadRotationParameterBases();
        }

        // ---------------------------------------------------------------------

        // Transition resources for drawing the sprite layer
        if (vdp2.cpuCommonRenderParams.spriteParams.rotate) {
        }
        barrierTracker.Flush(cmd);

        // Draw sprite layer
        Dispatch(vdp2.drawSpritePSO, vdp2.drawSpriteRootSig, shared.drawSpriteSet, &vdp2.cpuCommonRenderParams,
                 sizeof(vdp2.cpuCommonRenderParams), (HRes * resolutionScale + 31) / 32, numLines * resolutionScale,
                 enhancements.transparentMeshes ? 2 : 1);

        // ---------------------------------------------------------------------

        // Transition resources for drawing background layers
        if (vdpState.regs2.bgEnabled[4] || vdpState.regs2.bgEnabled[5]) {
        }
        barrierTracker.Flush(cmd);

        // Draw NBGs and RBGs
        Dispatch(vdp2.drawBGsPSO, vdp2.drawBGsRootSig, shared.drawBGsSet, &vdp2.cpuCommonRenderParams,
                 sizeof(vdp2.cpuCommonRenderParams), HRes * resolutionScale / 32, numLines * resolutionScale, 1);
    }

    /// @brief Converts a field line into the last row to compose for it.
    /// The compose shader works on output rows when deinterlacing (two per field line) and on field lines otherwise.
    /// @param[in] y the field line
    /// @return the corresponding compose row
    uint32 VDP2GetComposeRow(uint32 y) const {
        const bool interlaced = vdpState.regs2.TVMD.IsInterlaced() && !exclusiveMonitor;
        if (interlaced && enhancements.deinterlace) {
            return (y << 1u) | 1u;
        }
        return y;
    }

    void VDP2ComposeLines(uint32 y) {
        // Bail out if there's nothing to render
        if (y < vdp2.nextComposeLine) {
            return;
        }

        FrameContext &frameCtx = frames.GetCurrentFrame();

        vdp2.cpuCommonRenderParams.startY = vdp2.nextComposeLine;
        VDP2UploadLineColorBackScreens();

        // Determine how many lines to draw and update next scanline counter
        const uint32 numLines = y - vdp2.nextComposeLine + 1;
        vdp2.nextComposeLine = y + 1;

        // ---------------------------------------------------------------------

        // Transition resources for compositing layers
        barrierTracker.Flush(cmd);

        // Compose final image
        Dispatch(vdp2.composePSO, vdp2.composeRootSig, shared.composeSet, &vdp2.cpuCommonRenderParams,
                 sizeof(vdp2.cpuCommonRenderParams), (HRes * resolutionScale + 31) / 32, numLines * resolutionScale,
                 1);
    }

    void VDP2RenderLine(uint32 y) {
        VDP2CalcAccessPatterns();
        VDP2CalcVCellScrollDelay();
        VDP2DrawLineColorBackScreens(y);
        VDP2UpdateRotationParameterBases(y);
        vdpState.state2.UpdateRotationPageBaseAddresses(vdpState.regs2);

        // When Y=0, the changes happened during vblank (or, more precisely, between the last Y of the previous frame
        // and the first line of this frame). Otherwise, the changes happened between Y-1 and Y. Therefore, we need to
        // render lines up to Y-1 then sync the state, unless Y=0, in which case we just sync the state.

        if (y > 0) {
            const FrameContext &frameCtx = frames.GetCurrentFrame();
            const bool cramDirty = vdp2.cramGeneration != shared.cramGeneration;
            const bool layerRenderParamsDirty =
                vdp2.layerRenderParamsGeneration != shared.layerRenderParamsGeneration;
            const bool composeParamsDirty = vdp2.composeParamsGeneration != shared.composeParamsGeneration;
            const bool renderLayers = vdp2.vramDirty || cramDirty || layerRenderParamsDirty || composeParamsDirty;
            const bool compose = composeParamsDirty;
            if (renderLayers) {
                VDP2RenderLayerLines(y - 1);
            }
            if (compose) {
                VDP2ComposeLines(VDP2GetComposeRow(y - 1));
            }
        }

        VDP2UpdateState();
    }

    void VDP2EndFrame() {
        const uint32 vShift = vdpState.regs2.TVMD.IsInterlaced() ? 1u : 0u;
        const uint32 vres = VRes >> vShift;
        VDP2RenderLayerLines(vres - 1);
        // Without deinterlacing, each frame composes the lines of the current field into alternating output rows
        const bool fieldOnly =
            vdpState.regs2.TVMD.IsInterlaced() && !exclusiveMonitor && !enhancements.deinterlace;
        VDP2ComposeLines((fieldOnly ? vres : VRes) - 1);

        FrameContext &frameCtx = frames.GetCurrentFrame();

        // Copy the display area of the composited output to this frame's readback buffer
        barrierTracker.Flush(cmd);
        const uint32 outWidth = HRes * resolutionScale;
        const uint32 outHeight = VRes * resolutionScale;
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = outWidth,
            .bufferImageHeight = outHeight,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {outWidth, outHeight, 1},
        };
        if (cmd != VK_NULL_HANDLE) {
            vkCmdCopyImageToBuffer(cmd, vdp2.compositeOutTexture.image, VK_IMAGE_LAYOUT_GENERAL,
                                   frameCtx.readbackBuffer.buffer, 1, &region);
        }
        frameCtx.readbackPending = cmd != VK_NULL_HANDLE;
        frameCtx.readbackWidth = outWidth;
        frameCtx.readbackHeight = outHeight;

        // Make the readback and the FBRAM download visible to the host
        RecordHostReadBarrier();

        // Close and submit command buffer
        if (auto result = SubmitCommands(); !result) {
            Report<grp::vk_base>(devlog::level::warn, "{}", result.Error().message);
        }

        // Deliver the previous frame. It was submitted a whole emulated frame ago, so the GPU has almost certainly
        // finished it already and the wait is typically free, whereas waiting for this frame would stall the emulator
        // until the GPU catches up.
        DeliverFrame(frames[(frames.frameIndex + kNumFrames - 1) % kNumFrames]);

        // Advance frame
        MoveToNextFrame();
    }
};

// ---------------------------------------------------------------------------------------------------------------------

VulkanVDPRenderer::VulkanVDPRenderer(VDPState &state, const config::VDP2DebugRender &vdp2DebugRenderOptions,
                                     const config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig,
                                     uint32 resolutionScale)
    : HardwareVDPRendererBase(VDPRendererType::Vulkan)
    , m_impl(std::make_unique<Impl>(HwCallbacks, state, vdp2AccessPatternsConfig, vdp2DebugRenderOptions,
                                    m_enhancements, resolutionScale)) {}

VulkanVDPRenderer::~VulkanVDPRenderer() {
    m_impl->Shutdown();
}

util::VoidResult<> VulkanVDPRenderer::Initialize() {
    return m_impl->Initialize();
}

util::ObjectResult<VulkanVDPRenderer>
VulkanVDPRenderer::Create(VDPState &state, const config::VDP2DebugRender &vdp2DebugRenderOptions,
                          const config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig,
                          uint32 resolutionScale) {
    if (resolutionScale < 1 || resolutionScale > kMaxResolutionScale) {
        return util::ErrorMessage{fmt::format("Unsupported internal resolution scale: {}", resolutionScale)};
    }
    std::unique_ptr<VulkanVDPRenderer> renderer{
        new VulkanVDPRenderer(state, vdp2DebugRenderOptions, vdp2AccessPatternsConfig, resolutionScale)};
    util::VoidResult<> result = renderer->Initialize();
    if (!result) {
        return result.Error();
    }
    return renderer;
}

std::string VulkanVDPRenderer::GetDeviceName() const {
    return m_impl->vk.properties.deviceName;
}

uint32 VulkanVDPRenderer::GetResolutionScale() const {
    return m_impl->resolutionScale;
}

// -----------------------------------------------------------------------------
// Configuration

void VulkanVDPRenderer::UpdateEnhancements() {
    m_impl->UpdateEnhancements();
}

// -----------------------------------------------------------------------------
// Basics

bool VulkanVDPRenderer::IsValid() const {
    return m_impl->initialized;
}

void VulkanVDPRenderer::Reset(bool hard) {
    m_impl->Reset();
}

// -----------------------------------------------------------------------------
// Save states

void VulkanVDPRenderer::PreSaveStateSync() {
    // Save states include FBRAM, which is only brought back from the GPU on demand
    m_impl->VDP1SyncFB();
}

void VulkanVDPRenderer::PostLoadStateSync() {
    m_impl->vdp1.vramDirty.SetAll();
    if (auto result = m_impl->VDP1UploadFBRAM(); !result) {
        Report<grp::vk_base>(devlog::level::warn, "Failed to upload VDP1 FBRAM: {}", result.Error().message);
    }

    m_impl->VDP2CacheAllCRAMColors();
    m_impl->VDP2UpdateEnabledLayers();
    m_impl->vdp2.vramDirty.SetAll();
    ++m_impl->vdp2.cramGeneration;
    ++m_impl->vdp2.layerRenderParamsGeneration;
    ++m_impl->vdp2.composeParamsGeneration;
}

void VulkanVDPRenderer::SaveState(savestate::VDPSaveState::VDPRendererSaveState &state) {}

bool VulkanVDPRenderer::ValidateState(const savestate::VDPSaveState::VDPRendererSaveState &state) const {
    return true;
}

void VulkanVDPRenderer::LoadState(const savestate::VDPSaveState::VDPRendererSaveState &state) {}

// -----------------------------------------------------------------------------
// VDP1 memory and register writes

void VulkanVDPRenderer::VDP1WriteVRAM(uint32 address, uint8 value) {
    m_impl->VDP1WriteVRAM(address);
}

void VulkanVDPRenderer::VDP1WriteVRAM(uint32 address, uint16 value) {
    // The address is always word-aligned
    m_impl->VDP1WriteVRAM(address);
}

void VulkanVDPRenderer::VDP1SyncFB() {
    m_impl->VDP1SyncFB();
}

void VulkanVDPRenderer::VDP1DebugSyncFB() {
    m_impl->VDP1DebugSyncFB();
}

void VulkanVDPRenderer::VDP1WriteFB(uint32 address, uint8 value) {
    m_impl->VDP1WriteFB(address, 1);
}

void VulkanVDPRenderer::VDP1WriteFB(uint32 address, uint16 value) {
    m_impl->VDP1WriteFB(address, 2);
}

void VulkanVDPRenderer::VDP1WriteReg(uint32 address, uint16 value) {
    // All important registers are passed as root 32-bit constants.
    // Nothing needs to be marked dirty as a result of VDP1 register changes.
}

// -------------------------------------------------------------------------
// VDP2 memory and register writes

void VulkanVDPRenderer::VDP2WriteVRAM(uint32 address, uint8 value) {
    m_impl->VDP2WriteVRAM(address);
}

void VulkanVDPRenderer::VDP2WriteVRAM(uint32 address, uint16 value) {
    // The address is always word-aligned
    m_impl->VDP2WriteVRAM(address);
}

void VulkanVDPRenderer::VDP2WriteCRAM(uint32 address, uint8 value) {
    m_impl->VDP2WriteCRAM(address);
}

void VulkanVDPRenderer::VDP2WriteCRAM(uint32 address, uint16 value) {
    // The address is always word-aligned
    m_impl->VDP2WriteCRAM(address);
}

void VulkanVDPRenderer::VDP2WriteReg(uint32 address, uint16 value) {
    m_impl->VDP2WriteReg(address, value);
}

// -----------------------------------------------------------------------------
// Debugger

void VulkanVDPRenderer::UpdateEnabledLayers() {
    m_impl->VDP2UpdateEnabledLayers();
}

// -----------------------------------------------------------------------------
// Utilities

void VulkanVDPRenderer::DumpExtraVDP1Framebuffers(std::ostream &out) const {
    // TODO: pause the world, download mesh buffers, copy to output
}

// -----------------------------------------------------------------------------
// Rendering process

void VulkanVDPRenderer::VDP1EraseFramebuffer(uint64 cycles) {
    m_impl->VDP1EraseFramebuffer(cycles);
}

void VulkanVDPRenderer::VDP1SwapFramebuffer() {
    m_impl->VDP1SwapFramebuffer();
    Callbacks.VDP1FramebufferSwap();
}

void VulkanVDPRenderer::VDP1BeginFrame() {
    m_impl->VDP1BeginFrame();
}

void VulkanVDPRenderer::VDP1ExecuteCommand(uint32 cmdAddress, VDP1Command::Control control) {
    m_impl->VDP1ExecuteCommand(cmdAddress, control);
}

void VulkanVDPRenderer::VDP1EndFrame() {
    Callbacks.VDP1DrawFinished();
}

void VulkanVDPRenderer::VDP2SetResolution(uint32 h, uint32 v, bool exclusive) {
    m_impl->HRes = h;
    m_impl->VRes = v;
    m_impl->exclusiveMonitor = exclusive;
}

void VulkanVDPRenderer::VDP2SetField(bool odd) {
    // Nothing to do. We're using the main VDP2 state for this.
}

void VulkanVDPRenderer::VDP2LatchTVMD() {
    // Nothing to do. We're using the main VDP2 state for this.
}

void VulkanVDPRenderer::VDP2BeginFrame() {
    m_impl->VDP2BeginFrame();
}

void VulkanVDPRenderer::VDP2RenderLine(uint32 y) {
    m_impl->VDP2RenderLine(y);
}

void VulkanVDPRenderer::VDP2EndFrame() {
    m_impl->VDP2EndFrame();
    Callbacks.VDP2DrawFinished();
}

} // namespace ymir::vdp
