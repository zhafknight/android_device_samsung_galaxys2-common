#define LOG_TAG "N7000Camera3"

#include "Metadata.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hardware/camera.h>
#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <android/sync.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>
#include <utils/Timers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#ifndef GRALLOC_USAGE_HW_VIDEO_ENCODER
#define GRALLOC_USAGE_HW_VIDEO_ENCODER 0x00010000U
#endif

namespace n7000::camera3 {
namespace {

constexpr int kMaxCameras = 2;

class ScopedReconfigure {
public:
    ScopedReconfigure(std::mutex& mutex, bool& reconfiguring, bool& configured)
        : mutex_(mutex), reconfiguring_(reconfiguring), configured_(configured) {
        std::lock_guard<std::mutex> lock(mutex_);
        reconfiguring_ = true;
        configured_ = false;
    }

    ~ScopedReconfigure() {
        std::lock_guard<std::mutex> lock(mutex_);
        configured_ = success_;
        reconfiguring_ = false;
    }

    void succeed() {
        success_ = true;
    }

private:
    std::mutex& mutex_;
    bool& reconfiguring_;
    bool& configured_;
    bool success_ = false;
};

constexpr const char* kFallbackBlobPaths[] = {
        "/vendor/lib/hw/camera.smdk4210-hal1.so",
        "/system/vendor/lib/hw/camera.smdk4210-hal1.so",
        "/vendor/lib/hw/camera.exynos4.so",
        "/system/lib/hw/camera.exynos4.so",
};

class LegacyModule {
public:
    static LegacyModule& get() {
        static LegacyModule instance;
        return instance;
    }

    bool ensureLoaded() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (module_ != nullptr) {
            return true;
        }

        for (const char* candidate : kFallbackBlobPaths) {
            const std::string path(candidate);
            void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr) {
                ALOGW("dlopen(%s) failed: %s", path.c_str(), dlerror());
                continue;
            }
            auto* module = reinterpret_cast<camera_module_t*>(
                    dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR));
            if (module == nullptr) {
                ALOGE("%s does not export %s", path.c_str(), HAL_MODULE_INFO_SYM_AS_STR);
                dlclose(handle);
                continue;
            }
            if (module->common.id == nullptr ||
                strcmp(module->common.id, CAMERA_HARDWARE_MODULE_ID) != 0) {
                ALOGE("%s exports an invalid camera module", path.c_str());
                dlclose(handle);
                continue;
            }
            handle_ = handle;
            module_ = module;
            path_ = path;
            ALOGI("N7000 HAL3-on-HAL1 build 20260803-front-video-fps-v3");
            ALOGI("Loaded HAL1 camera module from %s, module API 0x%x", path.c_str(),
                  module_->common.module_api_version);
            return true;
        }
        ALOGE("No usable HAL1 camera module found");
        return false;
    }

    int cameraCount() {
        if (!ensureLoaded() || module_->get_number_of_cameras == nullptr) {
            return 0;
        }
        return std::clamp(module_->get_number_of_cameras(), 0, kMaxCameras);
    }

    int cameraInfo(int id, camera_info* info) {
        if (!ensureLoaded() || module_->get_camera_info == nullptr) {
            return -ENODEV;
        }
        return module_->get_camera_info(id, info);
    }

    int openCamera(int id, camera_device_t** device) {
        if (!ensureLoaded() || module_->common.methods == nullptr ||
            module_->common.methods->open == nullptr || device == nullptr) {
            return -ENODEV;
        }
        std::string idString = std::to_string(id);
        hw_device_t* hwDevice = nullptr;
        const int rc = module_->common.methods->open(&module_->common, idString.c_str(), &hwDevice);
        if (rc != 0 || hwDevice == nullptr) {
            ALOGE("HAL1 open camera %d failed: %d", id, rc);
            return rc != 0 ? rc : -ENODEV;
        }
        *device = reinterpret_cast<camera_device_t*>(hwDevice);
        return 0;
    }

    const std::string& path() const { return path_; }

private:
    std::mutex mutex_;
    void* handle_ = nullptr;
    camera_module_t* module_ = nullptr;
    std::string path_;
};

struct LegacyMemory {
    camera_memory_t camera{};
    void* allocation = nullptr;
    size_t allocationSize = 0;
    bool mapped = false;
};

void releaseLegacyMemory(camera_memory_t* memory) {
    if (memory == nullptr) {
        return;
    }
    auto* holder = reinterpret_cast<LegacyMemory*>(
            reinterpret_cast<uint8_t*>(memory) - offsetof(LegacyMemory, camera));
    if (holder->allocation != nullptr) {
        if (holder->mapped) {
            munmap(holder->allocation, holder->allocationSize);
        } else {
            free(holder->allocation);
        }
    }
    delete holder;
}

camera_memory_t* requestLegacyMemory(int fd, size_t bufferSize, unsigned int bufferCount,
                                     void*) {
    if (bufferSize == 0 || bufferCount == 0 || bufferSize > SIZE_MAX / bufferCount) {
        return nullptr;
    }
    const size_t totalSize = bufferSize * bufferCount;
    auto* holder = new (std::nothrow) LegacyMemory();
    if (holder == nullptr) {
        return nullptr;
    }
    if (fd >= 0) {
        holder->allocation = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (holder->allocation == MAP_FAILED) {
            ALOGE("mmap camera memory fd=%d size=%zu failed: %s", fd, totalSize,
                  strerror(errno));
            holder->allocation = nullptr;
            delete holder;
            return nullptr;
        }
        holder->mapped = true;
    } else {
        holder->allocation = calloc(1, totalSize);
        if (holder->allocation == nullptr) {
            delete holder;
            return nullptr;
        }
    }
    holder->allocationSize = totalSize;
    holder->camera.data = holder->allocation;
    holder->camera.size = totalSize;
    holder->camera.handle = holder;
    holder->camera.release = releaseLegacyMemory;
    return &holder->camera;
}

class ParameterMap {
public:
    explicit ParameterMap(const char* flattened) {
        if (flattened == nullptr) {
            return;
        }
        std::stringstream stream(flattened);
        std::string item;
        while (std::getline(stream, item, ';')) {
            const size_t separator = item.find('=');
            if (separator == std::string::npos) {
                continue;
            }
            values_[item.substr(0, separator)] = item.substr(separator + 1);
        }
    }

    void set(const std::string& key, const std::string& value) { values_[key] = value; }
    void setInt(const std::string& key, int value) { values_[key] = std::to_string(value); }
    void erase(const std::string& key) { values_.erase(key); }

    std::string flatten() const {
        std::string output;
        for (const auto& [key, value] : values_) {
            if (!output.empty()) {
                output.push_back(';');
            }
            output += key;
            output.push_back('=');
            output += value;
        }
        return output;
    }

private:
    std::map<std::string, std::string> values_;
};

constexpr std::array<int32_t, 31> kBackZoomRatios = {
        100, 102, 104, 109, 111, 113, 119, 121, 124, 131, 134,
        138, 146, 150, 155, 159, 165, 170, 182, 189, 200, 213,
        222, 232, 243, 255, 283, 300, 319, 364, 400,
};

std::array<int32_t, 4> resolveCropRegion(const camera_metadata_t* settings, int cameraId,
                                         int* legacyZoomIndex) {
    const CameraDescriptor& descriptor = getCameraDescriptor(cameraId);
    const std::array<int32_t, 4> fullCrop = {
            0, 0, descriptor.maxWidth, descriptor.maxHeight};

    if (legacyZoomIndex != nullptr) {
        *legacyZoomIndex = 0;
    }
    if (cameraId != 0 || settings == nullptr) {
        return fullCrop;
    }

    camera_metadata_ro_entry_t cropEntry{};
    if (find_camera_metadata_ro_entry(settings, ANDROID_SCALER_CROP_REGION,
                                      &cropEntry) != 0 ||
        cropEntry.count < 4 || cropEntry.data.i32[2] <= 0 ||
        cropEntry.data.i32[3] <= 0) {
        return fullCrop;
    }

    // HAL1 exposes a center-only integer zoom table. Convert the Camera2 crop
    // width to the nearest exact N7000 zoom ratio, following the useful part
    // of acroreiser's HAL3on1 approach without mutating framework streams.
    const int32_t requestedWidth =
            std::clamp(cropEntry.data.i32[2], 1, descriptor.maxWidth);
    const int32_t requestedRatio = std::clamp(
            static_cast<int32_t>((static_cast<int64_t>(descriptor.maxWidth) * 100 +
                                  requestedWidth / 2) /
                                 requestedWidth),
            kBackZoomRatios.front(), kBackZoomRatios.back());

    size_t bestIndex = 0;
    int32_t bestDifference = INT32_MAX;
    for (size_t i = 0; i < kBackZoomRatios.size(); ++i) {
        const int32_t difference =
                std::abs(kBackZoomRatios[i] - requestedRatio);
        if (difference < bestDifference) {
            bestDifference = difference;
            bestIndex = i;
        }
    }

    if (legacyZoomIndex != nullptr) {
        *legacyZoomIndex = static_cast<int>(bestIndex);
    }

    const int32_t ratio = kBackZoomRatios[bestIndex];
    int32_t width = static_cast<int32_t>(
            static_cast<int64_t>(descriptor.maxWidth) * 100 / ratio);
    int32_t height = static_cast<int32_t>(
            static_cast<int64_t>(descriptor.maxHeight) * 100 / ratio);
    width = std::max(2, width & ~1);
    height = std::max(2, height & ~1);
    const int32_t left = (descriptor.maxWidth - width) / 2;
    const int32_t top = (descriptor.maxHeight - height) / 2;
    return {left, top, width, height};
}

const char* legacySceneMode(uint8_t sceneMode) {
    switch (sceneMode) {
        case ANDROID_CONTROL_SCENE_MODE_ACTION:
            return "action";
        case ANDROID_CONTROL_SCENE_MODE_PORTRAIT:
            return "portrait";
        case ANDROID_CONTROL_SCENE_MODE_LANDSCAPE:
            return "landscape";
        case ANDROID_CONTROL_SCENE_MODE_NIGHT:
            return "night";
        case ANDROID_CONTROL_SCENE_MODE_BEACH:
            return "beach";
        case ANDROID_CONTROL_SCENE_MODE_SNOW:
            return "snow";
        case ANDROID_CONTROL_SCENE_MODE_SUNSET:
            return "sunset";
        case ANDROID_CONTROL_SCENE_MODE_FIREWORKS:
            return "fireworks";
        case ANDROID_CONTROL_SCENE_MODE_PARTY:
            return "party";
        case ANDROID_CONTROL_SCENE_MODE_CANDLELIGHT:
            return "candlelight";
        case ANDROID_CONTROL_SCENE_MODE_DISABLED:
        default:
            return "auto";
    }
}

struct PendingFrame {
    uint32_t frameNumber = 0;
    uint32_t generation = 0;
    int64_t timestamp = 0;
    std::array<int32_t, 4> cropRegion{};
    std::optional<camera3_stream_buffer_t> previewBuffer;
    std::optional<camera3_stream_buffer_t> videoBuffer;
    std::optional<camera3_stream_buffer_t> analysisBuffer;
    std::optional<camera3_stream_buffer_t> jpegBuffer;
    bool metadataReturned = false;
    bool requestErrorNotified = false;
};

void notifyTorchStatus(int status);

class Camera3Shim {
public:
    explicit Camera3Shim(int id) : id_(id) {
        memset(&device_, 0, sizeof(device_));
        memset(&ops_, 0, sizeof(ops_));
        memset(&previewWindow_, 0, sizeof(previewWindow_));

        device_.common.tag = HARDWARE_DEVICE_TAG;
        device_.common.version = CAMERA_DEVICE_API_VERSION_3_2;
        device_.common.close = closeDevice;
        device_.ops = &ops_;
        device_.priv = this;

        ops_.initialize = initializeDevice;
        ops_.configure_streams = configureStreamsDevice;
        ops_.register_stream_buffers = nullptr;
        ops_.construct_default_request_settings = constructDefaultRequestSettingsDevice;
        ops_.process_capture_request = processCaptureRequestDevice;
        ops_.get_metadata_vendor_tag_ops = nullptr;
        ops_.dump = dumpDevice;
        ops_.flush = flushDevice;

        previewWindow_.owner = this;
        previewWindow_.ops.dequeue_buffer = previewDequeueBuffer;
        previewWindow_.ops.enqueue_buffer = previewEnqueueBuffer;
        previewWindow_.ops.cancel_buffer = previewCancelBuffer;
        previewWindow_.ops.set_buffer_count = previewSetBufferCount;
        previewWindow_.ops.set_buffers_geometry = previewSetBuffersGeometry;
        previewWindow_.ops.set_crop = previewSetCrop;
        previewWindow_.ops.set_usage = previewSetUsage;
        previewWindow_.ops.set_swap_interval = previewSetSwapInterval;
        previewWindow_.ops.get_min_undequeued_buffer_count = previewGetMinUndequeuedCount;
        previewWindow_.ops.lock_buffer = previewLockBuffer;
        previewWindow_.ops.set_timestamp = previewSetTimestamp;

        const hw_module_t* grallocModule = nullptr;
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &grallocModule) == 0) {
            gralloc_ = reinterpret_cast<const gralloc_module_t*>(grallocModule);
            const int allocRc = gralloc_open(grallocModule, &grallocAlloc_);
            if (allocRc != 0) {
                ALOGE("Could not open gralloc allocator: %d", allocRc);
                grallocAlloc_ = nullptr;
            }
        }

        worker_ = std::thread(&Camera3Shim::workerLoop, this);
    }

    ~Camera3Shim() {
        closeInternal();
        releaseScratchPreviewBuffers();
        releaseDrainPreviewBuffer();
        if (grallocAlloc_ != nullptr) {
            gralloc_close(grallocAlloc_);
            grallocAlloc_ = nullptr;
        }
        for (camera_metadata_t* settings : defaultRequests_) {
            if (settings != nullptr) {
                free_camera_metadata(settings);
            }
        }
        if (lastSettings_ != nullptr) {
            free_camera_metadata(lastSettings_);
        }
    }

    camera3_device_t* device() { return &device_; }

    int openLegacy() {
        const int rc = LegacyModule::get().openCamera(id_, &legacyDevice_);
        if (rc != 0 || legacyDevice_ == nullptr || legacyDevice_->ops == nullptr) {
            return rc != 0 ? rc : -ENODEV;
        }
        return 0;
    }

private:
    enum class WorkerTask { StartPreview, TakePicture, Exit };

    struct WorkerCommand {
        WorkerTask task;
        uint32_t generation;
    };

    struct PreviewWindow {
        preview_stream_ops_t ops;
        Camera3Shim* owner;
    };

    struct ScratchPreviewBuffer {
        buffer_handle_t handle = nullptr;
        int stride = 0;
        camera3_stream_t stream{};
        camera3_stream_buffer_t buffer{};
        bool inUse = false;
    };

    static constexpr size_t kScratchPreviewBufferCount = 4;

    static Camera3Shim* from(const camera3_device_t* device) {
        return device == nullptr ? nullptr : static_cast<Camera3Shim*>(device->priv);
    }

    static PreviewWindow* from(preview_stream_ops_t* window) {
        return reinterpret_cast<PreviewWindow*>(window);
    }

    static int closeDevice(hw_device_t* device) {
        if (device == nullptr) {
            return -EINVAL;
        }
        auto* camera = reinterpret_cast<camera3_device_t*>(device);
        delete from(camera);
        return 0;
    }

    static int initializeDevice(const camera3_device_t* device,
                                const camera3_callback_ops_t* callbacks) {
        Camera3Shim* self = from(device);
        return self == nullptr ? -EINVAL : self->initialize(callbacks);
    }

    static int configureStreamsDevice(const camera3_device_t* device,
                                      camera3_stream_configuration_t* streams) {
        Camera3Shim* self = from(device);
        return self == nullptr ? -EINVAL : self->configureStreams(streams);
    }

    static const camera_metadata_t* constructDefaultRequestSettingsDevice(
            const camera3_device_t* device, int type) {
        Camera3Shim* self = from(device);
        return self == nullptr ? nullptr : self->constructDefaultRequestSettings(type);
    }

    static int processCaptureRequestDevice(const camera3_device_t* device,
                                           camera3_capture_request_t* request) {
        Camera3Shim* self = from(device);
        return self == nullptr ? -EINVAL : self->processCaptureRequest(request);
    }

    static void dumpDevice(const camera3_device_t* device, int fd) {
        Camera3Shim* self = from(device);
        if (self != nullptr) {
            self->dump(fd);
        }
    }

    static int flushDevice(const camera3_device_t* device) {
        Camera3Shim* self = from(device);
        return self == nullptr ? -EINVAL : self->flush();
    }

    int initialize(const camera3_callback_ops_t* callbacks) {
        if (callbacks == nullptr || callbacks->notify == nullptr ||
            callbacks->process_capture_result == nullptr || legacyDevice_ == nullptr ||
            legacyDevice_->ops == nullptr || legacyDevice_->ops->set_callbacks == nullptr ||
            legacyDevice_->ops->set_preview_window == nullptr ||
            legacyDevice_->ops->enable_msg_type == nullptr) {
            return -EINVAL;
        }
        callbacks_ = callbacks;
        legacyDevice_->ops->set_callbacks(legacyDevice_, legacyNotifyCallback,
                                          legacyDataCallback, legacyTimestampCallback,
                                          requestLegacyMemory, this);
        const int rc = legacyDevice_->ops->set_preview_window(
                legacyDevice_, &previewWindow_.ops);
        if (rc != 0) {
            ALOGE("set_preview_window failed: %d", rc);
            callbacks_ = nullptr;
            return rc;
        }
        legacyDevice_->ops->enable_msg_type(
                legacyDevice_, CAMERA_MSG_ERROR | CAMERA_MSG_FOCUS |
                               CAMERA_MSG_SHUTTER | CAMERA_MSG_COMPRESSED_IMAGE);
        initialized_ = true;
        return 0;
    }

    bool supportedPreviewSize(int width, int height) const {
        const std::vector<std::pair<int, int>> sizes = id_ == 0
                ? std::vector<std::pair<int, int>>{{1280, 720}, {800, 480}, {720, 480},
                                                   {640, 480}, {352, 288}, {320, 240},
                                                   {176, 144}}
                : std::vector<std::pair<int, int>>{{640, 480}, {352, 288},
                                                   {320, 240}, {176, 144}};
        return std::find(sizes.begin(), sizes.end(), std::make_pair(width, height)) != sizes.end();
    }

    bool supportedJpegSize(int width, int height) const {
        const std::vector<std::pair<int, int>> sizes = id_ == 0
                ? std::vector<std::pair<int, int>>{{3264, 2448}, {3264, 1968},
                                                   {2048, 1536}, {2048, 1232},
                                                   {1280, 960}, {800, 480}, {640, 480}}
                : std::vector<std::pair<int, int>>{{1600, 1200}, {640, 480}};
        return std::find(sizes.begin(), sizes.end(), std::make_pair(width, height)) != sizes.end();
    }

    int configureStreams(camera3_stream_configuration_t* configuration) {
        if (!initialized_ || configuration == nullptr || configuration->num_streams == 0 ||
            configuration->streams == nullptr) {
            return -EINVAL;
        }
        // Keep process_capture_request() from racing an old repeating request
        // against a new set of stream pointers. CameraX can submit one last
        // request while the previous session is being flushed.
        ScopedReconfigure reconfigure(stateMutex_, reconfiguring_, configured_);

        // This device reports HAL 3.2. operation_mode was added in HAL 3.3,
        // so a 3.2 implementation must not inspect it.
        flush();
        releaseScratchPreviewBuffers();
        releaseDrainPreviewBuffer();
        useVideoAsSource_ = false;

        camera3_stream_t* preview = nullptr;
        camera3_stream_t* video = nullptr;
        camera3_stream_t* analysis = nullptr;
        camera3_stream_t* jpeg = nullptr;
        for (uint32_t i = 0; i < configuration->num_streams; ++i) {
            camera3_stream_t* stream = configuration->streams[i];
            // rotation was added in HAL 3.3. Do not inspect it from a 3.2 HAL.
            if (stream == nullptr || stream->stream_type != CAMERA3_STREAM_OUTPUT) {
                return -EINVAL;
            }
            if (stream->format == HAL_PIXEL_FORMAT_BLOB) {
                if (jpeg != nullptr || !supportedJpegSize(stream->width, stream->height)) {
                    return -EINVAL;
                }
                jpeg = stream;
                stream->max_buffers = 1;
                // Preserve the legacy stream negotiation used by the
                // previously working N7000 Camera3 wrapper.
                stream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
                continue;
            }

            if (stream->format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
                if (analysis != nullptr || stream->width > 640 || stream->height > 480 ||
                    !supportedPreviewSize(stream->width, stream->height)) {
                    return -EINVAL;
                }
                if (gralloc_ == nullptr || gralloc_->lock_ycbcr == nullptr) {
                    ALOGE("ImageAnalysis requires gralloc lock_ycbcr support");
                    return -EINVAL;
                }
                analysis = stream;
                // ImageAnalysis is filled by copying from the real NV21
                // preview/video source. Never expose the flexible YUV buffer
                // directly to the legacy preview window.
                stream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
                stream->max_buffers = 2;
                continue;
            }

            if (stream->format != HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED &&
                stream->format != HAL_PIXEL_FORMAT_YCrCb_420_SP) {
                return -EINVAL;
            }
            if (!supportedPreviewSize(stream->width, stream->height)) {
                return -EINVAL;
            }

            const bool encoderStream =
                    (stream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) != 0;
            if (encoderStream) {
                if (video != nullptr) {
                    return -EINVAL;
                }
                video = stream;
            } else if (preview == nullptr) {
                preview = stream;
            } else if (video == nullptr) {
                // Some legacy consumers do not set VIDEO_ENCODER early enough.
                // Treat the second YUV stream as the recording target.
                ALOGW("Treating second %ux%u YUV stream as video output",
                      stream->width, stream->height);
                video = stream;
            } else {
                return -EINVAL;
            }

            if (stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                // Restore the exact device-side negotiation used by the
                // previously working wrapper. No gralloc modification is
                // required: the legacy provider accepts an NV21 override for
                // an IMPLEMENTATION_DEFINED output stream.
                ALOGI("Device-only stream override %s: IMPLEMENTATION_DEFINED -> NV21 "
                      "%ux%u inputUsage=0x%llx",
                      encoderStream ? "video" : "preview",
                      stream->width, stream->height,
                      static_cast<unsigned long long>(stream->usage));
                stream->format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
            }
            stream->usage |= GRALLOC_USAGE_SW_READ_OFTEN |
                             GRALLOC_USAGE_SW_WRITE_OFTEN |
                             GRALLOC_USAGE_HW_CAMERA_WRITE;
            ALOGI("Device-only configured %s stream: format=0x%x usage=0x%llx "
                  "size=%ux%u",
                  encoderStream ? "video" : "preview", stream->format,
                  static_cast<unsigned long long>(stream->usage),
                  stream->width, stream->height);
            // CameraX keeps several preview/video buffers in flight. Two
            // buffers starve the recording pipeline before HAL1 can return the
            // first frame, so retain the working four-buffer depth here.
            stream->max_buffers = 4;
        }

        if (preview == nullptr && video == nullptr && analysis == nullptr &&
            jpeg == nullptr) {
            return -EINVAL;
        }
        bool useVideoAsSource = false;
        if (preview != nullptr && video != nullptr &&
            (preview->width != video->width || preview->height != video->height)) {
            // CameraX pairs a 640x480 display preview with a 1280x720 encoder
            // surface for HD recording. HAL1 can only produce one preview size,
            // so use the 720p video buffer as the real source and derive the
            // smaller preview with a bounded NV21 center-crop/downscale.
            if (id_ == 0 && preview->width == 640 && preview->height == 480 &&
                video->width == 1280 && video->height == 720) {
                useVideoAsSource = true;
                ALOGI("Using video 1280x720 as HAL1 source; deriving preview 640x480");
            } else {
                ALOGE("Unsupported preview/video bridge pair: %ux%u + %ux%u",
                      preview->width, preview->height, video->width, video->height);
                return -EINVAL;
            }
        }
        if (analysis != nullptr) {
            camera3_stream_t* analysisSource = useVideoAsSource
                    ? video
                    : (preview != nullptr ? preview : video);
            if (analysisSource == nullptr) {
                ALOGE("ImageAnalysis requires a preview or video source stream");
                return -EINVAL;
            }
            const bool sameAnalysisSize =
                    analysis->width == analysisSource->width &&
                    analysis->height == analysisSource->height;
            const bool hdToVgaAnalysis =
                    analysisSource->width == 1280 && analysisSource->height == 720 &&
                    analysis->width == 640 && analysis->height == 480;
            if (!sameAnalysisSize && !hdToVgaAnalysis) {
                ALOGE("Unsupported analysis bridge %ux%u -> %ux%u",
                      analysisSource->width, analysisSource->height,
                      analysis->width, analysis->height);
                return -EINVAL;
            }
        }

        previewStream_ = preview;
        videoStream_ = video;
        analysisStream_ = analysis;
        sourceStream_ = useVideoAsSource
                ? video
                : (preview != nullptr ? preview : video);
        if (sourceStream_ == nullptr) {
            sourceStream_ = analysis;
        }
        jpegStream_ = jpeg;
        previewWidth_ = sourceStream_ != nullptr ? sourceStream_->width : 0;
        previewHeight_ = sourceStream_ != nullptr ? sourceStream_->height : 0;
        videoWidth_ = video != nullptr ? video->width : 0;
        videoHeight_ = video != nullptr ? video->height : 0;
        analysisWidth_ = analysis != nullptr ? analysis->width : 0;
        analysisHeight_ = analysis != nullptr ? analysis->height : 0;
        jpegWidth_ = jpeg != nullptr ? jpeg->width : 0;
        jpegHeight_ = jpeg != nullptr ? jpeg->height : 0;
        useVideoAsSource_ = useVideoAsSource;

        // The Exynos HAL1 preview loop does not check dequeue_buffer()'s
        // return value. Keep one valid internal NV21 buffer available so a
        // flush or still-capture transition can wake that loop safely.
        if (sourceStream_ != nullptr) {
            const int drainRc = allocateDrainPreviewBuffer(previewWidth_, previewHeight_);
            if (drainRc != 0) {
                ALOGE("Could not allocate %dx%d drain preview buffer: %d",
                      previewWidth_, previewHeight_, drainRc);
                return drainRc;
            }
        }

        // Internal source buffers are required when the configured HAL1 source
        // is not present in an individual request (for example a preview-only
        // request in a preview+HD-video session, or an ImageAnalysis-only
        // request). HAL1 still needs a real NV21 destination for that frame.
        if (useVideoAsSource_ || analysisStream_ != nullptr) {
            const int scratchRc = allocateScratchPreviewBuffers(previewWidth_, previewHeight_);
            if (scratchRc != 0) {
                ALOGE("Could not allocate %dx%d internal preview buffers: %d",
                      previewWidth_, previewHeight_, scratchRc);
                useVideoAsSource_ = false;
                releaseDrainPreviewBuffer();
                return scratchRc;
            }
        }

        int rc = updateLegacyParameters(nullptr);
        if (rc != 0) {
            ALOGE("Initial HAL1 parameter configuration failed: %d", rc);
            releaseScratchPreviewBuffers();
            releaseDrainPreviewBuffer();
            return rc;
        }
        haveRequestSettings_ = false;
        reconfigure.succeed();
        return 0;
    }

    const camera_metadata_t* constructDefaultRequestSettings(int type) {
        if (type <= 0 || type >= CAMERA3_TEMPLATE_COUNT) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (defaultRequests_[type] == nullptr) {
            defaultRequests_[type] = buildDefaultRequest(id_, type);
        }
        return defaultRequests_[type];
    }

    int processCaptureRequest(camera3_capture_request_t* request) {
        if (callbacks_ == nullptr || request == nullptr ||
            request->num_output_buffers == 0 || request->output_buffers == nullptr ||
            request->input_buffer != nullptr) {
            return -EINVAL;
        }
        for (uint32_t i = 0; i < request->num_output_buffers; ++i) {
            if (request->output_buffers[i].stream == nullptr ||
                request->output_buffers[i].buffer == nullptr) {
                return -EINVAL;
            }
        }

        const camera_metadata_t* effectiveSettings = request->settings;
        uint32_t requestGeneration = 0;
        bool rejectForTransition = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            rejectForTransition = closing_ || flushing_ || reconfiguring_;
            if (!rejectForTransition && !configured_) {
                return -EINVAL;
            }
            if (!rejectForTransition && effectiveSettings == nullptr) {
                if (!haveRequestSettings_ || lastSettings_ == nullptr) {
                    return -EINVAL;
                }
                effectiveSettings = lastSettings_;
            }
            requestGeneration = sessionGeneration_.load();
        }
        if (rejectForTransition) {
            rejectRawRequest(request, "session transition");
            return 0;
        }

        auto frame = std::make_shared<PendingFrame>();
        frame->frameNumber = request->frame_number;
        frame->generation = requestGeneration;
        frame->timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
        frame->cropRegion = resolveCropRegion(effectiveSettings, id_, nullptr);

        // Validate the complete request before taking ownership of any acquire
        // fence. On -EINVAL the framework retains ownership of every buffer.
        for (uint32_t i = 0; i < request->num_output_buffers; ++i) {
            camera3_stream_buffer_t buffer = request->output_buffers[i];
            if (buffer.stream == nullptr || buffer.buffer == nullptr) {
                return -EINVAL;
            }
            if (buffer.stream == jpegStream_) {
                if (frame->jpegBuffer.has_value()) {
                    return -EINVAL;
                }
                frame->jpegBuffer = buffer;
            } else if (buffer.stream == previewStream_) {
                if (frame->previewBuffer.has_value()) {
                    return -EINVAL;
                }
                frame->previewBuffer = buffer;
            } else if (buffer.stream == videoStream_) {
                if (frame->videoBuffer.has_value()) {
                    return -EINVAL;
                }
                frame->videoBuffer = buffer;
            } else if (buffer.stream == analysisStream_) {
                if (frame->analysisBuffer.has_value()) {
                    return -EINVAL;
                }
                frame->analysisBuffer = buffer;
            } else {
                return -EINVAL;
            }
        }
        if (!frame->previewBuffer.has_value() && !frame->videoBuffer.has_value() &&
            !frame->analysisBuffer.has_value() && !frame->jpegBuffer.has_value()) {
            return -EINVAL;
        }

        // The HAL owns acquire fences after this request is accepted. Wait for
        // them on the worker/preview thread, not here: process_capture_request()
        // is required to stay non-blocking.
        if (frame->previewBuffer.has_value()) {
            frame->previewBuffer->release_fence = -1;
            frame->previewBuffer->status = CAMERA3_BUFFER_STATUS_OK;
        }
        if (frame->videoBuffer.has_value()) {
            frame->videoBuffer->release_fence = -1;
            frame->videoBuffer->status = CAMERA3_BUFFER_STATUS_OK;
        }
        if (frame->analysisBuffer.has_value()) {
            frame->analysisBuffer->release_fence = -1;
            frame->analysisBuffer->status = CAMERA3_BUFFER_STATUS_OK;
        }
        if (frame->jpegBuffer.has_value()) {
            frame->jpegBuffer->release_fence = -1;
            frame->jpegBuffer->status = CAMERA3_BUFFER_STATUS_OK;
        }

        if (request->settings != nullptr) {
            camera_metadata_t* cloned = clone_camera_metadata(request->settings);
            if (cloned == nullptr) {
                failFrame(frame);
                return 0;
            }
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (lastSettings_ != nullptr) {
                free_camera_metadata(lastSettings_);
            }
            lastSettings_ = cloned;
            haveRequestSettings_ = true;
            effectiveSettings = lastSettings_;
        }

        // A null settings pointer repeats the previous request. Reapplying the
        // full Camera1 parameter string for every repeating frame is expensive
        // and can serialize process_capture_request() behind still capture.
        if (request->settings != nullptr) {
            if (updateLegacyParameters(effectiveSettings) != 0) {
                ALOGW("Could not apply all request settings to HAL1");
            }
            handleAfTrigger(effectiveSettings);
        }

        bool reserveRejected = false;
        bool reservedStill = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            reserveRejected = closing_ || flushing_ ||
                    frame->generation != sessionGeneration_.load();
            if (!reserveRejected && frame->jpegBuffer.has_value()) {
                // process_capture_request() must not block behind an older
                // JPEG. Reject an overlapping still request cleanly instead
                // of waiting while CameraService is trying to drain.
                if (stillQueued_) {
                    reserveRejected = true;
                } else {
                    stillQueued_ = true;
                    reservedStill = true;
                }
            }
        }
        if (reserveRejected) {
            failFrame(frame);
            return 0;
        }

        // Publish metadata before this frame becomes visible to asynchronous
        // HAL1 workers. JPEG completion may be delayed, but metadata must remain
        // strictly ordered by frame number.
        sendShutter(frame->frameNumber, frame->timestamp);
        sendMetadata(frame);

        bool startPreview = false;
        bool takePicture = false;
        bool sessionChanged = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            sessionChanged = closing_ || flushing_ ||
                    frame->generation != sessionGeneration_.load();
            if (!sessionChanged &&
                (frame->previewBuffer.has_value() || frame->videoBuffer.has_value() ||
                 frame->analysisBuffer.has_value())) {
                previewQueue_.push_back(frame);
                startPreview = true;
            } else if (!sessionChanged) {
                pendingJpeg_ = frame;
                previewAbort_ = true;
                takePicture = true;
            } else if (reservedStill) {
                stillQueued_ = false;
                stillCv_.notify_all();
            }
        }
        if (sessionChanged) {
            failFrame(frame);
            return 0;
        }

        if (startPreview) {
            previewCv_.notify_all();
            postTask(WorkerTask::StartPreview, frame->generation);
        } else if (takePicture) {
            previewCv_.notify_all();
            postTask(WorkerTask::TakePicture, frame->generation);
        }
        return 0;
    }

    int updateLegacyParameters(const camera_metadata_t* settings) {
        if (legacyDevice_ == nullptr || legacyDevice_->ops == nullptr ||
            legacyDevice_->ops->get_parameters == nullptr ||
            legacyDevice_->ops->set_parameters == nullptr) {
            return -ENODEV;
        }
        std::lock_guard<std::mutex> lock(legacyOpsMutex_);
        char* oldParameters = legacyDevice_->ops->get_parameters(legacyDevice_);
        ParameterMap parameters(oldParameters);
        if (oldParameters != nullptr) {
            if (legacyDevice_->ops->put_parameters != nullptr) {
                legacyDevice_->ops->put_parameters(legacyDevice_, oldParameters);
            } else {
                free(oldParameters);
            }
        }

        if (sourceStream_ != nullptr) {
            parameters.set("preview-size", std::to_string(previewWidth_) + "x" +
                                           std::to_string(previewHeight_));
            parameters.set("preview-format", "yuv420sp");
            int minFps = id_ == 1 ? 15 : (videoStream_ != nullptr ? 30 : 15);
            int maxFps = id_ == 1 ? 15 : 30;
            camera_metadata_ro_entry_t fpsEntry{};
            if (settings != nullptr && id_ == 0 && videoStream_ == nullptr &&
                find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
                                              &fpsEntry) == 0 &&
                fpsEntry.count >= 2 && fpsEntry.data.i32[0] >= 30 &&
                fpsEntry.data.i32[1] >= 30) {
                minFps = 30;
                maxFps = 30;
            }
            parameters.set("preview-fps-range", std::to_string(minFps * 1000) + "," +
                                                std::to_string(maxFps * 1000));
            parameters.setInt("preview-frame-rate", maxFps);
        }
        if (jpegStream_ != nullptr) {
            parameters.set("picture-size", std::to_string(jpegWidth_) + "x" +
                                           std::to_string(jpegHeight_));
            parameters.set("picture-format", "jpeg");
        }
        if (videoStream_ != nullptr) {
            parameters.set("recording-hint", "true");
            parameters.set("video-size", std::to_string(videoWidth_) + "x" +
                                         std::to_string(videoHeight_));
            parameters.set("video-frame-format", "yuv420sp");
        } else {
            parameters.set("recording-hint", "false");
        }

        camera_metadata_ro_entry_t entry{};
        bool useSceneMode = false;
        const char* selectedSceneMode = "auto";
        if (id_ == 0 && settings != nullptr) {
            uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
            if (find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_MODE,
                                              &entry) == 0 &&
                entry.count > 0) {
                controlMode = entry.data.u8[0];
            }
            if (controlMode == ANDROID_CONTROL_MODE_USE_SCENE_MODE &&
                find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_SCENE_MODE,
                                              &entry) == 0 &&
                entry.count > 0 &&
                entry.data.u8[0] != ANDROID_CONTROL_SCENE_MODE_DISABLED) {
                selectedSceneMode = legacySceneMode(entry.data.u8[0]);
                useSceneMode = strcmp(selectedSceneMode, "auto") != 0;
            }
        }

        if (settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_JPEG_QUALITY, &entry) == 0 &&
            entry.count > 0) {
            parameters.setInt("jpeg-quality", entry.data.u8[0]);
        }
        if (settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_JPEG_THUMBNAIL_QUALITY, &entry) == 0 &&
            entry.count > 0) {
            parameters.setInt("jpeg-thumbnail-quality", entry.data.u8[0]);
        }
        if (settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_JPEG_THUMBNAIL_SIZE, &entry) == 0 &&
            entry.count >= 2) {
            parameters.setInt("jpeg-thumbnail-width", entry.data.i32[0]);
            parameters.setInt("jpeg-thumbnail-height", entry.data.i32[1]);
        }
        if (settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_JPEG_ORIENTATION, &entry) == 0 &&
            entry.count > 0) {
            parameters.setInt("rotation", entry.data.i32[0]);
        }
        if (!useSceneMode && settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
                                          &entry) == 0 &&
            entry.count > 0) {
            parameters.setInt("exposure-compensation", entry.data.i32[0]);
        }

        if (id_ == 0) {
            int legacyZoomIndex = 0;
            resolveCropRegion(settings, id_, &legacyZoomIndex);
            parameters.setInt("zoom", legacyZoomIndex);
            parameters.set("scene-mode", selectedSceneMode);
        }

        if (settings != nullptr) {
            camera_metadata_ro_entry_t gpsEntry{};
            if (find_camera_metadata_ro_entry(settings, ANDROID_JPEG_GPS_COORDINATES,
                                              &gpsEntry) == 0 &&
                gpsEntry.count >= 3) {
                char coordinate[64];
                snprintf(coordinate, sizeof(coordinate), "%.8f", gpsEntry.data.d[0]);
                parameters.set("gps-latitude", coordinate);
                snprintf(coordinate, sizeof(coordinate), "%.8f", gpsEntry.data.d[1]);
                parameters.set("gps-longitude", coordinate);
                snprintf(coordinate, sizeof(coordinate), "%.3f", gpsEntry.data.d[2]);
                parameters.set("gps-altitude", coordinate);
            } else {
                parameters.erase("gps-latitude");
                parameters.erase("gps-longitude");
                parameters.erase("gps-altitude");
            }

            if (find_camera_metadata_ro_entry(settings, ANDROID_JPEG_GPS_TIMESTAMP,
                                              &gpsEntry) == 0 &&
                gpsEntry.count > 0) {
                parameters.set("gps-timestamp",
                               std::to_string(gpsEntry.data.i64[0]));
            } else {
                parameters.erase("gps-timestamp");
            }

            if (find_camera_metadata_ro_entry(
                        settings, ANDROID_JPEG_GPS_PROCESSING_METHOD,
                        &gpsEntry) == 0 &&
                gpsEntry.count > 0) {
                const char* methodData =
                        reinterpret_cast<const char*>(gpsEntry.data.u8);
                size_t methodLength = 0;
                while (methodLength < gpsEntry.count &&
                       methodData[methodLength] != '\0') {
                    ++methodLength;
                }
                const std::string method(methodData, methodLength);
                if (!method.empty()) {
                    parameters.set("gps-processing-method", method);
                } else {
                    parameters.erase("gps-processing-method");
                }
            } else {
                parameters.erase("gps-processing-method");
            }
        }

        if (id_ == 0 && !useSceneMode) {
            const char* legacyFlashMode = "off";

            if (settings != nullptr &&
                find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AE_MODE, &entry) == 0 &&
                entry.count > 0) {
                switch (entry.data.u8[0]) {
                    case ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH:
                    case ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE:
                        legacyFlashMode = "auto";
                        break;
                    case ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH:
                        legacyFlashMode = "on";
                        break;
                    default:
                        legacyFlashMode = "off";
                        break;
                }
            }

            if (settings != nullptr &&
                find_camera_metadata_ro_entry(settings, ANDROID_FLASH_MODE, &entry) == 0 &&
                entry.count > 0) {
                switch (entry.data.u8[0]) {
                    case ANDROID_FLASH_MODE_TORCH:
                        legacyFlashMode = "torch";
                        break;
                    case ANDROID_FLASH_MODE_SINGLE:
                        legacyFlashMode = "on";
                        break;
                    case ANDROID_FLASH_MODE_OFF:
                    default:
                        break;
                }
            }

            parameters.set("flash-mode", legacyFlashMode);
        }

        if (!useSceneMode && settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AF_MODE, &entry) == 0 &&
            entry.count > 0) {
            switch (entry.data.u8[0]) {
                case ANDROID_CONTROL_AF_MODE_MACRO:
                    parameters.set("focus-mode", "macro");
                    break;
                case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
                    parameters.set("focus-mode", "continuous-video");
                    break;
                case ANDROID_CONTROL_AF_MODE_OFF:
                    parameters.set("focus-mode", id_ == 0 ? "infinity" : "fixed");
                    break;
                case ANDROID_CONTROL_AF_MODE_AUTO:
                default:
                    parameters.set("focus-mode", id_ == 0 ? "auto" : "fixed");
                    break;
            }
        }
        if (id_ == 0 && !useSceneMode && settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AWB_MODE, &entry) == 0 &&
            entry.count > 0) {
            const char* value = "auto";
            switch (entry.data.u8[0]) {
                case ANDROID_CONTROL_AWB_MODE_INCANDESCENT: value = "incandescent"; break;
                case ANDROID_CONTROL_AWB_MODE_FLUORESCENT: value = "fluorescent"; break;
                case ANDROID_CONTROL_AWB_MODE_DAYLIGHT: value = "daylight"; break;
                case ANDROID_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT: value = "cloudy-daylight"; break;
                default: break;
            }
            parameters.set("whitebalance", value);
        }
        if (id_ == 0 && settings != nullptr &&
            find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_EFFECT_MODE, &entry) == 0 &&
            entry.count > 0) {
            const char* value = "none";
            switch (entry.data.u8[0]) {
                case ANDROID_CONTROL_EFFECT_MODE_MONO: value = "mono"; break;
                case ANDROID_CONTROL_EFFECT_MODE_NEGATIVE: value = "negative"; break;
                case ANDROID_CONTROL_EFFECT_MODE_SEPIA: value = "sepia"; break;
                case ANDROID_CONTROL_EFFECT_MODE_AQUA: value = "aqua"; break;
                default: break;
            }
            parameters.set("effect", value);
        }

        const std::string flattened = parameters.flatten();
        return legacyDevice_->ops->set_parameters(legacyDevice_, flattened.c_str());
    }

    void handleAfTrigger(const camera_metadata_t* settings) {
        if (settings == nullptr || legacyDevice_ == nullptr || id_ != 0) {
            return;
        }
        camera_metadata_ro_entry_t entry{};
        if (find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AF_TRIGGER, &entry) != 0 ||
            entry.count == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(legacyOpsMutex_);
        if (entry.data.u8[0] == ANDROID_CONTROL_AF_TRIGGER_START &&
            legacyDevice_->ops->auto_focus != nullptr) {
            afState_.store(ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN);
            legacyDevice_->ops->auto_focus(legacyDevice_);
        } else if (entry.data.u8[0] == ANDROID_CONTROL_AF_TRIGGER_CANCEL &&
                   legacyDevice_->ops->cancel_auto_focus != nullptr) {
            legacyDevice_->ops->cancel_auto_focus(legacyDevice_);
            afState_.store(ANDROID_CONTROL_AF_STATE_INACTIVE);
        }
    }

    static int waitAndCloseAcquireFence(camera3_stream_buffer_t* buffer) {
        if (buffer == nullptr || buffer->acquire_fence < 0) {
            return 0;
        }
        const int fence = buffer->acquire_fence;
        const int rc = sync_wait(fence, 1000);
        const int savedErrno = errno;
        if (rc == 0) {
            close(fence);
            buffer->acquire_fence = -1;
        } else {
            errno = savedErrno;
        }
        return rc;
    }

    static void prepareErrorBuffer(camera3_stream_buffer_t* buffer) {
        if (buffer == nullptr) {
            return;
        }

        // If the HAL did not wait on an acquire fence, Camera3 requires the
        // same fence to be returned as the release fence. This transfers fence
        // ownership back to the framework without leaking or closing a fence
        // that may still be unsignaled.
        const int pendingAcquireFence = buffer->acquire_fence;
        buffer->status = CAMERA3_BUFFER_STATUS_ERROR;
        buffer->acquire_fence = -1;
        buffer->release_fence = pendingAcquireFence;
    }

    void releasePreviewBuffer(ScratchPreviewBuffer* buffer) {
        if (buffer == nullptr) {
            return;
        }
        if (grallocAlloc_ != nullptr && buffer->handle != nullptr) {
            grallocAlloc_->free(grallocAlloc_, buffer->handle);
        }
        *buffer = ScratchPreviewBuffer{};
    }

    int allocatePreviewBuffer(ScratchPreviewBuffer* buffer, int width, int height) {
        if (buffer == nullptr || grallocAlloc_ == nullptr || width <= 0 || height <= 0) {
            return -ENODEV;
        }

        releasePreviewBuffer(buffer);
        const int usage = GRALLOC_USAGE_SW_READ_OFTEN |
                          GRALLOC_USAGE_SW_WRITE_OFTEN |
                          GRALLOC_USAGE_HW_CAMERA_WRITE;
        int rc = grallocAlloc_->alloc(grallocAlloc_, width, height,
                                      HAL_PIXEL_FORMAT_YCrCb_420_SP, usage,
                                      &buffer->handle, &buffer->stride);
        if (rc != 0 || buffer->handle == nullptr) {
            releasePreviewBuffer(buffer);
            return rc != 0 ? rc : -ENOMEM;
        }

        buffer->stream.stream_type = CAMERA3_STREAM_OUTPUT;
        buffer->stream.width = static_cast<uint32_t>(width);
        buffer->stream.height = static_cast<uint32_t>(height);
        buffer->stream.format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
        buffer->stream.usage = static_cast<uint32_t>(usage);
        buffer->stream.max_buffers = 1;
        buffer->buffer.stream = &buffer->stream;
        buffer->buffer.buffer = &buffer->handle;
        buffer->buffer.status = CAMERA3_BUFFER_STATUS_OK;
        buffer->buffer.acquire_fence = -1;
        buffer->buffer.release_fence = -1;
        buffer->inUse = false;
        return 0;
    }

    void releaseDrainPreviewBuffer() {
        releasePreviewBuffer(&drainPreviewBuffer_);
    }

    int allocateDrainPreviewBuffer(int width, int height) {
        return allocatePreviewBuffer(&drainPreviewBuffer_, width, height);
    }

    int returnDrainPreviewBufferLocked(buffer_handle_t** buffer, int* stride) {
        if (buffer == nullptr || stride == nullptr ||
            drainPreviewBuffer_.handle == nullptr) {
            return -ENODEV;
        }

        // This buffer is owned entirely by the wrapper. It only exists to let
        // the legacy Exynos preview loop leave dequeue_buffer() safely while a
        // still capture, flush, or close operation stops that loop.
        *buffer = &drainPreviewBuffer_.handle;
        *stride = drainPreviewBuffer_.stride;
        return 0;
    }

    void releaseScratchPreviewBuffers() {
        for (auto& scratch : scratchPreviewBuffers_) {
            releasePreviewBuffer(&scratch);
        }
    }

    int allocateScratchPreviewBuffers(int width, int height) {
        if (grallocAlloc_ == nullptr || width <= 0 || height <= 0) {
            return -ENODEV;
        }

        releaseScratchPreviewBuffers();
        for (size_t i = 0; i < scratchPreviewBuffers_.size(); ++i) {
            auto& scratch = scratchPreviewBuffers_[i];
            const int rc = allocatePreviewBuffer(&scratch, width, height);
            if (rc != 0) {
                ALOGE("Scratch preview allocation %zu/%zu failed for %dx%d: %d",
                      i + 1, scratchPreviewBuffers_.size(), width, height, rc);
                releaseScratchPreviewBuffers();
                return rc;
            }
        }

        ALOGI("Allocated %zu scratch NV21 buffers for preview-only %dx%d requests",
              scratchPreviewBuffers_.size(), width, height);
        return 0;
    }

    ScratchPreviewBuffer* freeScratchPreviewBufferLocked() {
        for (auto& scratch : scratchPreviewBuffers_) {
            if (scratch.handle != nullptr && !scratch.inUse) {
                return &scratch;
            }
        }
        return nullptr;
    }

    bool hasFreeScratchPreviewBufferLocked() const {
        for (const auto& scratch : scratchPreviewBuffers_) {
            if (scratch.handle != nullptr && !scratch.inUse) {
                return true;
            }
        }
        return false;
    }

    ScratchPreviewBuffer* scratchPreviewBufferForHandle(buffer_handle_t* handle) {
        if (handle == nullptr) {
            return nullptr;
        }
        for (auto& scratch : scratchPreviewBuffers_) {
            if (&scratch.handle == handle) {
                return &scratch;
            }
        }
        return nullptr;
    }

    bool needsScratchPreviewSource(const std::shared_ptr<PendingFrame>& frame) const {
        if (frame == nullptr || sourceStream_ == nullptr) {
            return false;
        }

        // Flexible YUV buffers cannot be passed to the Camera1 preview window,
        // and a request is not required to target every configured stream.
        // Supply an internal NV21 destination whenever this request does not
        // contain a usable direct HAL1 source buffer.
        return sourceBuffer(frame) == nullptr &&
               (frame->previewBuffer.has_value() || frame->videoBuffer.has_value() ||
                frame->analysisBuffer.has_value());
    }

    camera3_stream_buffer_t* sourceBuffer(
            const std::shared_ptr<PendingFrame>& frame) const {
        if (frame == nullptr) {
            return nullptr;
        }
        if (sourceStream_ == videoStream_) {
            if (frame->videoBuffer.has_value()) {
                return &*frame->videoBuffer;
            }
            // Falling back to a differently sized preview buffer would let
            // HAL1 write 720p into a 640x480 allocation. Only permit fallback
            // when both streams have the configured source dimensions.
            if (frame->previewBuffer.has_value() && previewStream_ != nullptr &&
                previewStream_->width == static_cast<uint32_t>(previewWidth_) &&
                previewStream_->height == static_cast<uint32_t>(previewHeight_)) {
                return &*frame->previewBuffer;
            }
            return nullptr;
        }
        if (sourceStream_ == previewStream_) {
            if (frame->previewBuffer.has_value()) {
                return &*frame->previewBuffer;
            }
            if (frame->videoBuffer.has_value() && videoStream_ != nullptr &&
                videoStream_->width == static_cast<uint32_t>(previewWidth_) &&
                videoStream_->height == static_cast<uint32_t>(previewHeight_)) {
                return &*frame->videoBuffer;
            }
            return nullptr;
        }
        // Flexible YUV analysis buffers are never handed directly to HAL1.
        // They are populated through lock_ycbcr() after the NV21 source frame
        // has completed.
        return nullptr;
    }

    int copyNv21Buffer(const std::shared_ptr<PendingFrame>& frame,
                       camera3_stream_buffer_t& source,
                       camera3_stream_buffer_t& target) {
        if (frame == nullptr || source.stream == nullptr || target.stream == nullptr ||
            source.buffer == nullptr || target.buffer == nullptr || gralloc_ == nullptr) {
            return -EINVAL;
        }
        if (waitAndCloseAcquireFence(&target) != 0) {
            ALOGE("Timed out waiting for derived-buffer acquire fence for frame %u: %s",
                  frame->frameNumber, strerror(errno));
            return -errno;
        }

        const size_t sourceWidth = source.stream->width;
        const size_t sourceHeight = source.stream->height;
        const size_t targetWidth = target.stream->width;
        const size_t targetHeight = target.stream->height;
        const bool sameSize = sourceWidth == targetWidth &&
                              sourceHeight == targetHeight;
        const bool hdToVgaPreview = sourceWidth == 1280 && sourceHeight == 720 &&
                                    targetWidth == 640 && targetHeight == 480;
        if (!sameSize && !hdToVgaPreview) {
            ALOGE("Unsupported NV21 copy %zux%zu -> %zux%zu for frame %u",
                  sourceWidth, sourceHeight, targetWidth, targetHeight,
                  frame->frameNumber);
            return -EINVAL;
        }

        void* sourceData = nullptr;
        void* targetData = nullptr;
        int rc = gralloc_->lock(gralloc_, *source.buffer, GRALLOC_USAGE_SW_READ_OFTEN,
                                0, 0, static_cast<int>(sourceWidth),
                                static_cast<int>(sourceHeight), &sourceData);
        if (rc != 0 || sourceData == nullptr) {
            ALOGE("Could not lock NV21 source for frame %u: %d",
                  frame->frameNumber, rc);
            return rc != 0 ? rc : -EIO;
        }
        rc = gralloc_->lock(gralloc_, *target.buffer, GRALLOC_USAGE_SW_WRITE_OFTEN,
                            0, 0, static_cast<int>(targetWidth),
                            static_cast<int>(targetHeight), &targetData);
        if (rc != 0 || targetData == nullptr) {
            ALOGE("Could not lock NV21 target for frame %u: %d",
                  frame->frameNumber, rc);
            gralloc_->unlock(gralloc_, *source.buffer);
            return rc != 0 ? rc : -EIO;
        }

        const auto* sourceBytes = static_cast<const uint8_t*>(sourceData);
        auto* targetBytes = static_cast<uint8_t*>(targetData);
        if (sameSize) {
            const size_t frameSize = sourceWidth * sourceHeight * 3U / 2U;
            memcpy(targetBytes, sourceBytes, frameSize);
        } else {
            // CameraX uses a 4:3 640x480 display preview beside the 16:9
            // 1280x720 recording stream. Center-crop the 720p source to
            // 960x720, then downscale by the exact 3:2 ratio. Keeping this
            // path specialized avoids per-pixel integer division on Cortex-A9.
            constexpr size_t kCropX = 160;
            for (size_t y = 0; y < 480; ++y) {
                const size_t sourceY = (y * 3U) >> 1U;
                for (size_t x = 0; x < 640; ++x) {
                    const size_t sourceX = kCropX + ((x * 3U) >> 1U);
                    targetBytes[y * 640U + x] =
                            sourceBytes[sourceY * 1280U + sourceX];
                }
            }

            const auto* sourceVu = sourceBytes + 1280U * 720U;
            auto* targetVu = targetBytes + 640U * 480U;
            constexpr size_t kCropChromaX = kCropX / 2U;
            for (size_t y = 0; y < 240; ++y) {
                const size_t sourceY = (y * 3U) >> 1U;
                for (size_t x = 0; x < 320; ++x) {
                    const size_t sourceX = kCropChromaX + ((x * 3U) >> 1U);
                    const size_t sourceOffset = sourceY * 1280U + sourceX * 2U;
                    const size_t targetOffset = y * 640U + x * 2U;
                    targetVu[targetOffset] = sourceVu[sourceOffset];
                    targetVu[targetOffset + 1U] = sourceVu[sourceOffset + 1U];
                }
            }
        }

        gralloc_->unlock(gralloc_, *target.buffer);
        gralloc_->unlock(gralloc_, *source.buffer);
        return 0;
    }

    int copyPreviewToAnalysis(const std::shared_ptr<PendingFrame>& frame,
                              camera3_stream_buffer_t* sourceOverride = nullptr) {
        if (frame == nullptr || !frame->analysisBuffer.has_value() || gralloc_ == nullptr) {
            return -EINVAL;
        }

        camera3_stream_buffer_t* source = sourceOverride != nullptr
                ? sourceOverride
                : sourceBuffer(frame);
        if (source == nullptr) {
            return -EINVAL;
        }

        camera3_stream_buffer_t& target = *frame->analysisBuffer;
        if (waitAndCloseAcquireFence(&target) != 0) {
            ALOGE("Timed out waiting for analysis acquire fence for frame %u: %s",
                  frame->frameNumber, strerror(errno));
            return -errno;
        }
        if (source->stream == nullptr) {
            return -EINVAL;
        }
        const size_t sourceWidth = source->stream->width;
        const size_t sourceHeight = source->stream->height;
        const size_t targetWidth = static_cast<size_t>(analysisWidth_);
        const size_t targetHeight = static_cast<size_t>(analysisHeight_);
        const bool sameSize = sourceWidth == targetWidth && sourceHeight == targetHeight;
        const bool hdToVgaAnalysis = sourceWidth == 1280 && sourceHeight == 720 &&
                                     targetWidth == 640 && targetHeight == 480;
        if (!sameSize && !hdToVgaAnalysis) {
            ALOGE("Unsupported analysis copy %zux%zu -> %zux%zu for frame %u",
                  sourceWidth, sourceHeight, targetWidth, targetHeight,
                  frame->frameNumber);
            return -EINVAL;
        }
        if (gralloc_->lock_ycbcr == nullptr) {
            ALOGE("gralloc does not expose lock_ycbcr for ImageAnalysis");
            return -ENOSYS;
        }

        void* sourceData = nullptr;
        android_ycbcr targetYcbcr{};
        int rc = gralloc_->lock(gralloc_, *source->buffer, GRALLOC_USAGE_SW_READ_OFTEN,
                                0, 0, static_cast<int>(sourceWidth),
                                static_cast<int>(sourceHeight), &sourceData);
        if (rc != 0 || sourceData == nullptr) {
            return rc != 0 ? rc : -EIO;
        }
        rc = gralloc_->lock_ycbcr(gralloc_, *target.buffer,
                                  GRALLOC_USAGE_SW_WRITE_OFTEN,
                                  0, 0, analysisWidth_, analysisHeight_, &targetYcbcr);
        if (rc != 0 || targetYcbcr.y == nullptr || targetYcbcr.cb == nullptr ||
            targetYcbcr.cr == nullptr || targetYcbcr.chroma_step == 0) {
            ALOGE("Could not lock analysis YCbCr planes for frame %u: %d",
                  frame->frameNumber, rc);
            gralloc_->unlock(gralloc_, *source->buffer);
            return rc != 0 ? rc : -EIO;
        }

        const auto* sourceBytes = static_cast<const uint8_t*>(sourceData);
        auto* targetY = static_cast<uint8_t*>(targetYcbcr.y);
        auto* targetCb = static_cast<uint8_t*>(targetYcbcr.cb);
        auto* targetCr = static_cast<uint8_t*>(targetYcbcr.cr);

        if (sameSize) {
            for (size_t row = 0; row < targetHeight; ++row) {
                memcpy(targetY + row * targetYcbcr.ystride,
                       sourceBytes + row * sourceWidth, targetWidth);
            }

            const uint8_t* sourceVu = sourceBytes + sourceWidth * sourceHeight;
            for (size_t row = 0; row < targetHeight / 2U; ++row) {
                uint8_t* cbRow = targetCb + row * targetYcbcr.cstride;
                uint8_t* crRow = targetCr + row * targetYcbcr.cstride;
                const uint8_t* sourceRow = sourceVu + row * sourceWidth;
                for (size_t column = 0; column < targetWidth / 2U; ++column) {
                    crRow[column * targetYcbcr.chroma_step] = sourceRow[column * 2U];
                    cbRow[column * targetYcbcr.chroma_step] = sourceRow[column * 2U + 1U];
                }
            }
        } else {
            // Match the 1280x720 -> 640x480 preview bridge: crop the center
            // 960x720 region, then downscale it by the exact 3:2 ratio.
            constexpr size_t kCropX = 160;
            for (size_t row = 0; row < 480; ++row) {
                uint8_t* targetRow = targetY + row * targetYcbcr.ystride;
                const size_t sourceY = (row * 3U) >> 1U;
                for (size_t column = 0; column < 640; ++column) {
                    const size_t sourceX = kCropX + ((column * 3U) >> 1U);
                    targetRow[column] = sourceBytes[sourceY * 1280U + sourceX];
                }
            }

            const uint8_t* sourceVu = sourceBytes + 1280U * 720U;
            constexpr size_t kCropChromaX = kCropX / 2U;
            for (size_t row = 0; row < 240; ++row) {
                uint8_t* cbRow = targetCb + row * targetYcbcr.cstride;
                uint8_t* crRow = targetCr + row * targetYcbcr.cstride;
                const size_t sourceY = (row * 3U) >> 1U;
                for (size_t column = 0; column < 320; ++column) {
                    const size_t sourceX = kCropChromaX + ((column * 3U) >> 1U);
                    const size_t sourceOffset = sourceY * 1280U + sourceX * 2U;
                    crRow[column * targetYcbcr.chroma_step] = sourceVu[sourceOffset];
                    cbRow[column * targetYcbcr.chroma_step] = sourceVu[sourceOffset + 1U];
                }
            }
        }

        gralloc_->unlock(gralloc_, *target.buffer);
        gralloc_->unlock(gralloc_, *source->buffer);
        return 0;
    }

    void sendShutter(uint32_t frameNumber, int64_t timestamp) {
        if (callbacks_ == nullptr || callbacks_->notify == nullptr) {
            return;
        }
        camera3_notify_msg_t message{};
        message.type = CAMERA3_MSG_SHUTTER;
        message.message.shutter.frame_number = frameNumber;
        message.message.shutter.timestamp = timestamp;
        callbacks_->notify(callbacks_, &message);
    }

    void sendRequestError(uint32_t frameNumber) {
        if (callbacks_ == nullptr || callbacks_->notify == nullptr) {
            return;
        }
        camera3_notify_msg_t message{};
        message.type = CAMERA3_MSG_ERROR;
        message.message.error.frame_number = frameNumber;
        message.message.error.error_stream = nullptr;
        message.message.error.error_code = CAMERA3_MSG_ERROR_REQUEST;
        callbacks_->notify(callbacks_, &message);
    }

    void returnBufferError(uint32_t frameNumber, camera3_stream_buffer_t buffer,
                           bool notifyBuffer) {
        prepareErrorBuffer(&buffer);
        if (callbacks_ == nullptr || callbacks_->process_capture_result == nullptr) {
            if (buffer.release_fence >= 0) {
                close(buffer.release_fence);
            }
            return;
        }
        if (notifyBuffer && callbacks_->notify != nullptr) {
            camera3_notify_msg_t message{};
            message.type = CAMERA3_MSG_ERROR;
            message.message.error.frame_number = frameNumber;
            message.message.error.error_stream = buffer.stream;
            message.message.error.error_code = CAMERA3_MSG_ERROR_BUFFER;
            callbacks_->notify(callbacks_, &message);
        }
        camera3_capture_result_t result{};
        result.frame_number = frameNumber;
        result.num_output_buffers = 1;
        result.output_buffers = &buffer;
        callbacks_->process_capture_result(callbacks_, &result);
    }

    void sendBufferError(uint32_t frameNumber, camera3_stream_buffer_t buffer) {
        returnBufferError(frameNumber, buffer, true);
    }

    void rejectRawRequest(camera3_capture_request_t* request, const char* reason) {
        if (request == nullptr || callbacks_ == nullptr ||
            callbacks_->process_capture_result == nullptr) {
            return;
        }

        ALOGW("Rejecting frame %u during %s without failing the camera device",
              request->frame_number, reason != nullptr ? reason : "transition");
        sendRequestError(request->frame_number);

        std::vector<camera3_stream_buffer_t> buffers;
        buffers.reserve(request->num_output_buffers);
        for (uint32_t i = 0; i < request->num_output_buffers; ++i) {
            camera3_stream_buffer_t buffer = request->output_buffers[i];
            prepareErrorBuffer(&buffer);
            buffers.push_back(buffer);
        }

        camera3_capture_result_t result{};
        result.frame_number = request->frame_number;
        result.num_output_buffers = static_cast<uint32_t>(buffers.size());
        result.output_buffers = buffers.data();
        callbacks_->process_capture_result(callbacks_, &result);
    }

    void failFrame(const std::shared_ptr<PendingFrame>& frame) {
        if (frame == nullptr) {
            return;
        }

        const bool hadJpeg = frame->jpegBuffer.has_value();

        if (!frame->metadataReturned && !frame->requestErrorNotified) {
            sendRequestError(frame->frameNumber);
            frame->requestErrorNotified = true;
        }
        const bool notifyBuffer = !frame->requestErrorNotified;

        auto failBuffer = [this, frame, notifyBuffer](
                                  std::optional<camera3_stream_buffer_t>* buffer) {
            if (buffer == nullptr || !buffer->has_value()) {
                return;
            }
            returnBufferError(frame->frameNumber, **buffer, notifyBuffer);
            buffer->reset();
        };
        failBuffer(&frame->previewBuffer);
        failBuffer(&frame->videoBuffer);
        failBuffer(&frame->analysisBuffer);
        failBuffer(&frame->jpegBuffer);

        if (hadJpeg) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (pendingJpeg_ == frame) {
                pendingJpeg_.reset();
            }
            stillQueued_ = false;
            if (!flushing_ && !closing_) {
                previewAbort_ = false;
            }
            stillCv_.notify_all();
            previewCv_.notify_all();
        }
    }

    void sendMetadata(const std::shared_ptr<PendingFrame>& frame) {
        if (frame == nullptr || frame->metadataReturned || callbacks_ == nullptr ||
            callbacks_->process_capture_result == nullptr) {
            return;
        }

        camera_metadata_t* metadata =
                buildResultMetadata(id_, frame->timestamp, afState_.load(),
                                    frame->cropRegion.data());
        if (metadata == nullptr) {
            ALOGE("Could not build result metadata for frame %u", frame->frameNumber);
            return;
        }

        camera3_capture_result_t result{};
        result.frame_number = frame->frameNumber;
        result.result = metadata;
        result.partial_result = 1;
        callbacks_->process_capture_result(callbacks_, &result);
        free_camera_metadata(metadata);
        frame->metadataReturned = true;
    }

    void sendResult(const std::shared_ptr<PendingFrame>& frame,
                    camera3_stream_buffer_t buffer, bool includeMetadata) {
        if (callbacks_ == nullptr || callbacks_->process_capture_result == nullptr) {
            return;
        }
        buffer.acquire_fence = -1;
        buffer.release_fence = -1;
        camera_metadata_t* metadata = includeMetadata
                ? buildResultMetadata(id_, frame->timestamp, afState_.load(),
                                      frame->cropRegion.data())
                : nullptr;
        camera3_capture_result_t result{};
        result.frame_number = frame->frameNumber;
        result.result = metadata;
        result.num_output_buffers = 1;
        result.output_buffers = &buffer;
        result.partial_result = metadata != nullptr ? 1 : 0;
        callbacks_->process_capture_result(callbacks_, &result);
        if (metadata != nullptr) {
            free_camera_metadata(metadata);
        }
    }

    int flush() {
        std::deque<std::shared_ptr<PendingFrame>> queued;
        std::vector<std::shared_ptr<PendingFrame>> inFlight;
        std::shared_ptr<PendingFrame> jpeg;
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            flushing_ = true;
            previewAbort_ = true;
            sessionGeneration_.fetch_add(1);
            previewCv_.notify_all();
            stillCv_.notify_all();
            jpegWorkerCv_.wait(lock, [this] {
                return !jpegWorkerActive_ && jpegCallbacksActive_ == 0;
            });
            queued.swap(previewQueue_);
            inFlight.reserve(inFlightPreview_.size());
            for (const auto& [buffer, frame] : inFlightPreview_) {
                (void)buffer;
                inFlight.push_back(frame);
            }
            inFlightPreview_.clear();
            for (auto& scratch : scratchPreviewBuffers_) {
                scratch.inUse = false;
            }
            jpeg.swap(pendingJpeg_);
            workerTasks_.erase(
                    std::remove_if(workerTasks_.begin(), workerTasks_.end(),
                                   [](const WorkerCommand& command) {
                                       return command.task != WorkerTask::Exit;
                                   }),
                    workerTasks_.end());
        }

        {
            std::lock_guard<std::mutex> lock(legacyOpsMutex_);
            if (legacyDevice_ != nullptr && legacyDevice_->ops != nullptr) {
                if (legacyDevice_->ops->cancel_picture != nullptr) {
                    legacyDevice_->ops->cancel_picture(legacyDevice_);
                }
                if (legacyDevice_->ops->stop_preview != nullptr) {
                    legacyDevice_->ops->stop_preview(legacyDevice_);
                }
            }
        }
        previewStarted_.store(false);

        for (const auto& frame : queued) failFrame(frame);
        for (const auto& frame : inFlight) failFrame(frame);
        failFrame(jpeg);

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stillQueued_ = false;
            flushing_ = false;
            previewAbort_ = false;
            stillCv_.notify_all();
            previewCv_.notify_all();
        }
        return 0;
    }

    void dump(int fd) {
        dprintf(fd, "N7000 thin HAL3 wrapper\n");
        dprintf(fd, "camera id: %d\n", id_);
        dprintf(fd, "legacy module: %s\n", LegacyModule::get().path().c_str());
        dprintf(fd, "configured: %d previewStarted: %d\n", configured_,
                previewStarted_.load());
        dprintf(fd, "preview source: %dx%d video: %dx%d analysis: %dx%d jpeg: %dx%d\n",
                previewWidth_, previewHeight_, videoWidth_, videoHeight_,
                analysisWidth_, analysisHeight_, jpegWidth_, jpegHeight_);
    }

    void closeInternal() {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            closing_ = true;
            previewAbort_ = true;
            sessionGeneration_.fetch_add(1);
            workerTasks_.clear();
            previewCv_.notify_all();
            stillCv_.notify_all();
            postTaskLocked(WorkerTask::Exit, sessionGeneration_.load());
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        {
            std::lock_guard<std::mutex> lock(legacyOpsMutex_);
            // The legacy JPEG thread is detached. Stop any callback that starts
            // during close from retaining this shim as its user pointer. An
            // already-running callback is tracked and drained by flush().
            if (legacyDevice_ != nullptr && legacyDevice_->ops != nullptr &&
                legacyDevice_->ops->set_callbacks != nullptr) {
                legacyDevice_->ops->set_callbacks(
                        legacyDevice_, legacyNotifyCallback, legacyDataCallback,
                        legacyTimestampCallback, requestLegacyMemory, nullptr);
            }
        }
        flush();
        {
            std::lock_guard<std::mutex> lock(legacyOpsMutex_);
            if (legacyDevice_ != nullptr) {
                // Do not call both HAL1 release() and common.close(). The N7000
                // backend deinitializes in both paths, so doing both double-frees it.
                if (legacyDevice_->common.close != nullptr) {
                    legacyDevice_->common.close(&legacyDevice_->common);
                } else if (legacyDevice_->ops != nullptr &&
                           legacyDevice_->ops->release != nullptr) {
                    legacyDevice_->ops->release(legacyDevice_);
                }
                legacyDevice_ = nullptr;
            }
        }
        if (id_ == 0) {
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
        }
    }

    void postTask(WorkerTask task, uint32_t generation) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        postTaskLocked(task, generation);
    }

    void postTaskLocked(WorkerTask task, uint32_t generation) {
        if (task == WorkerTask::Exit) {
            workerTasks_.clear();
        } else if (task == WorkerTask::StartPreview) {
            const bool alreadyQueued = std::any_of(
                    workerTasks_.begin(), workerTasks_.end(),
                    [generation](const WorkerCommand& command) {
                        return command.task == WorkerTask::StartPreview &&
                               command.generation == generation;
                    });
            if (alreadyQueued) {
                return;
            }
        }
        workerTasks_.push_back({task, generation});
        workerCv_.notify_all();
    }

    void workerLoop() {
        while (true) {
            WorkerCommand command{};
            {
                std::unique_lock<std::mutex> lock(stateMutex_);
                workerCv_.wait(lock, [this] { return !workerTasks_.empty(); });
                command = workerTasks_.front();
                workerTasks_.pop_front();
            }
            if (command.task == WorkerTask::Exit) {
                return;
            }
            if (command.generation != sessionGeneration_.load()) {
                ALOGV("Dropping stale camera worker task generation %u",
                      command.generation);
                continue;
            }
            if (command.task == WorkerTask::StartPreview) {
                startPreviewWorker(command.generation);
            } else if (command.task == WorkerTask::TakePicture) {
                takePictureWorker(command.generation);
            }
        }
    }

    void startPreviewWorker(uint32_t generation) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (generation != sessionGeneration_.load() || closing_ || flushing_ ||
                !configured_ || previewQueue_.empty() || previewStarted_.load() ||
                sourceStream_ == nullptr) {
                return;
            }
            previewAbort_ = false;
        }
        std::lock_guard<std::mutex> lock(legacyOpsMutex_);
        if (generation != sessionGeneration_.load() || legacyDevice_ == nullptr ||
            legacyDevice_->ops == nullptr ||
            legacyDevice_->ops->start_preview == nullptr) {
            return;
        }

        // exynos_camera_preview_stop(), including the stop performed by
        // take_picture(), clears HAL1's preview_window pointer. Reattach the
        // shim before every preview start; otherwise the legacy thread waits
        // forever for a preview window and Camera3 retains all output buffers.
        if (legacyDevice_->ops->set_preview_window != nullptr) {
            const int windowRc = legacyDevice_->ops->set_preview_window(
                    legacyDevice_, &previewWindow_.ops);
            if (windowRc != 0) {
                ALOGE("HAL1 set_preview_window before restart failed: %d", windowRc);
                return;
            }
        }

        const int rc = legacyDevice_->ops->start_preview(legacyDevice_);
        if (rc == 0) {
            previewStarted_.store(true);
        } else {
            ALOGE("HAL1 start_preview failed: %d", rc);
        }
    }

    void takePictureWorker(uint32_t generation) {
        std::shared_ptr<PendingFrame> frame;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            frame = pendingJpeg_;
            if (generation != sessionGeneration_.load() || closing_ || flushing_ ||
                frame == nullptr || frame->generation != generation ||
                !frame->jpegBuffer.has_value()) {
                return;
            }
            jpegWorkerActive_ = true;
            // The HAL1 backend stops and joins its preview thread from
            // take_picture(). Wake a preview dequeue that is waiting for the
            // next camera3 buffer first, otherwise that join can deadlock.
            previewAbort_ = true;
            previewCv_.notify_all();
        }

        auto finishWorker = [this] {
            std::lock_guard<std::mutex> lock(stateMutex_);
            jpegWorkerActive_ = false;
            jpegWorkerCv_.notify_all();
        };

        if (waitAndCloseAcquireFence(&*frame->jpegBuffer) != 0) {
            ALOGE("Timed out waiting for JPEG acquire fence for frame %u: %s",
                  frame->frameNumber, strerror(errno));
            const bool current = generation == sessionGeneration_.load();
            finishWorker();
            if (!current) {
                return;
            }
            failFrame(frame);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (pendingJpeg_ == frame) {
                    pendingJpeg_.reset();
                }
                stillQueued_ = false;
                previewAbort_ = false;
                stillCv_.notify_all();
                previewCv_.notify_all();
            }
            postTask(WorkerTask::StartPreview, generation);
            return;
        }

        int rc = -ENODEV;
        {
            std::lock_guard<std::mutex> lock(legacyOpsMutex_);
            if (generation == sessionGeneration_.load() && legacyDevice_ != nullptr &&
                legacyDevice_->ops != nullptr &&
                legacyDevice_->ops->take_picture != nullptr) {
                rc = legacyDevice_->ops->take_picture(legacyDevice_);
            }
        }
        finishWorker();
        previewStarted_.store(false);

        if (generation != sessionGeneration_.load()) {
            return;
        }
        if (rc != 0) {
            ALOGE("HAL1 take_picture failed: %d", rc);
            failFrame(frame);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (pendingJpeg_ == frame) {
                    pendingJpeg_.reset();
                }
                stillQueued_ = false;
                previewAbort_ = false;
                stillCv_.notify_all();
                previewCv_.notify_all();
            }
            postTask(WorkerTask::StartPreview, generation);
        }
    }

    static int previewDequeueBuffer(preview_stream_ops_t* window,
                                    buffer_handle_t** buffer, int* stride) {
        PreviewWindow* wrapper = from(window);
        return wrapper == nullptr ? -EINVAL : wrapper->owner->dequeuePreview(buffer, stride);
    }

    int dequeuePreview(buffer_handle_t** buffer, int* stride) {
        if (buffer == nullptr || stride == nullptr) {
            return -EINVAL;
        }
        while (true) {
            std::shared_ptr<PendingFrame> frame;
            camera3_stream_buffer_t* source = nullptr;
            ScratchPreviewBuffer* scratch = nullptr;
            buffer_handle_t* handle = nullptr;
            bool invalidFrame = false;
            {
                std::unique_lock<std::mutex> lock(stateMutex_);
                previewCv_.wait(lock, [this] {
                    return !previewQueue_.empty() || previewAbort_ || closing_ || flushing_;
                });
                if (previewAbort_ || closing_ || flushing_) {
                    return returnDrainPreviewBufferLocked(buffer, stride);
                }

                frame = previewQueue_.front();
                source = sourceBuffer(frame);
                if (source == nullptr && needsScratchPreviewSource(frame)) {
                    scratch = freeScratchPreviewBufferLocked();
                    if (scratch == nullptr) {
                        previewCv_.wait(lock, [this] {
                            return hasFreeScratchPreviewBufferLocked() ||
                                   previewAbort_ || closing_ || flushing_;
                        });
                        if (previewAbort_ || closing_ || flushing_) {
                            return returnDrainPreviewBufferLocked(buffer, stride);
                        }
                        continue;
                    }
                    scratch->inUse = true;
                    source = &scratch->buffer;
                }

                previewQueue_.pop_front();
                if (source == nullptr || source->buffer == nullptr) {
                    if (scratch != nullptr) {
                        scratch->inUse = false;
                    }
                    invalidFrame = true;
                } else {
                    handle = source->buffer;
                    const auto [it, inserted] =
                            inFlightPreview_.emplace(handle, frame);
                    if (!inserted) {
                        ALOGE("Preview buffer %p is already in flight", handle);
                        if (scratch != nullptr) {
                            scratch->inUse = false;
                        }
                        invalidFrame = true;
                    }
                }
            }

            if (invalidFrame) {
                failFrame(frame);
                previewCv_.notify_all();
                continue;
            }

            if (waitAndCloseAcquireFence(source) != 0) {
                ALOGE("Timed out waiting for source acquire fence for frame %u: %s",
                      frame->frameNumber, strerror(errno));
                const bool hadJpeg = frame->jpegBuffer.has_value();
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    const auto it = inFlightPreview_.find(handle);
                    if (it != inFlightPreview_.end() && it->second == frame) {
                        inFlightPreview_.erase(it);
                    }
                    ScratchPreviewBuffer* failedScratch =
                            scratchPreviewBufferForHandle(handle);
                    if (failedScratch != nullptr) {
                        failedScratch->inUse = false;
                    }
                    if (hadJpeg) {
                        stillQueued_ = false;
                        previewAbort_ = false;
                        stillCv_.notify_all();
                    }
                }
                failFrame(frame);
                previewCv_.notify_all();
                continue;
            }

            bool requeuedForStillCapture = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                const auto it = inFlightPreview_.find(handle);
                if (it == inFlightPreview_.end() || it->second != frame ||
                    previewAbort_ || closing_ || flushing_) {
                    if (it != inFlightPreview_.end() && it->second == frame) {
                        inFlightPreview_.erase(it);
                        if (previewAbort_ && !closing_ && !flushing_ &&
                            frame->generation == sessionGeneration_.load()) {
                            previewQueue_.push_front(frame);
                            requeuedForStillCapture = true;
                        }
                    }
                    ScratchPreviewBuffer* abandonedScratch =
                            scratchPreviewBufferForHandle(handle);
                    if (abandonedScratch != nullptr) {
                        abandonedScratch->inUse = false;
                    }
                    previewCv_.notify_all();
                    const int drainRc = returnDrainPreviewBufferLocked(buffer, stride);
                    if (drainRc != 0) {
                        ALOGE("No drain preview buffer during session transition: %d",
                              drainRc);
                    }
                    if (requeuedForStillCapture) {
                        ALOGV("Requeued frame %u across still-capture preview stop",
                              frame->frameNumber);
                    }
                    return drainRc;
                }
            }
            *buffer = handle;
            ScratchPreviewBuffer* activeScratch = scratchPreviewBufferForHandle(handle);
            *stride = activeScratch != nullptr ? activeScratch->stride : previewWidth_;
            return 0;
        }
    }

    static int previewEnqueueBuffer(preview_stream_ops_t* window,
                                    buffer_handle_t* buffer) {
        PreviewWindow* wrapper = from(window);
        return wrapper == nullptr
                ? -EINVAL
                : wrapper->owner->completePreview(buffer, false);
    }

    static int previewCancelBuffer(preview_stream_ops_t* window,
                                   buffer_handle_t* buffer) {
        PreviewWindow* wrapper = from(window);
        return wrapper == nullptr
                ? -EINVAL
                : wrapper->owner->completePreview(buffer, true);
    }

    int completePreview(buffer_handle_t* buffer, bool error) {
        if (buffer == nullptr) {
            return -EINVAL;
        }

        std::shared_ptr<PendingFrame> frame;
        ScratchPreviewBuffer* scratch = nullptr;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (buffer == &drainPreviewBuffer_.handle) {
                return 0;
            }
            const auto it = inFlightPreview_.find(buffer);
            if (it == inFlightPreview_.end()) {
                ALOGW("HAL1 returned an unknown preview buffer %p", buffer);
                return -ENOENT;
            }
            frame = it->second;
            inFlightPreview_.erase(it);
            scratch = scratchPreviewBufferForHandle(buffer);
        }
        if (frame == nullptr ||
            (!frame->previewBuffer.has_value() && !frame->videoBuffer.has_value() &&
             !frame->analysisBuffer.has_value())) {
            return -EINVAL;
        }

        if (error) {
            const bool hadJpeg = frame->jpegBuffer.has_value();
            failFrame(frame);
            if (hadJpeg) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                stillQueued_ = false;
                previewAbort_ = false;
                stillCv_.notify_all();
            }
        } else if (scratch != nullptr) {
            bool metadataReturned = frame->metadataReturned;
            if (frame->previewBuffer.has_value()) {
                const int copyRc = copyNv21Buffer(
                        frame, scratch->buffer, *frame->previewBuffer);
                if (copyRc == 0) {
                    sendResult(frame, *frame->previewBuffer, !metadataReturned);
                    metadataReturned = true;
                } else {
                    sendBufferError(frame->frameNumber, *frame->previewBuffer);
                }
            }
            if (frame->analysisBuffer.has_value()) {
                const int analysisRc = copyPreviewToAnalysis(frame, &scratch->buffer);
                if (analysisRc == 0) {
                    sendResult(frame, *frame->analysisBuffer, !metadataReturned);
                    metadataReturned = true;
                } else {
                    sendBufferError(frame->frameNumber, *frame->analysisBuffer);
                }
            }
            if (frame->videoBuffer.has_value()) {
                sendBufferError(frame->frameNumber, *frame->videoBuffer);
            }
            frame->metadataReturned = metadataReturned;
        } else {
            bool metadataReturned = frame->metadataReturned;
            camera3_stream_buffer_t* source = sourceBuffer(frame);
            camera3_stream_buffer_t* derived = nullptr;
            int derivedRc = 0;
            int analysisRc = 0;

            if (source != nullptr && frame->previewBuffer.has_value() &&
                frame->videoBuffer.has_value()) {
                if (source == &*frame->videoBuffer) {
                    derived = &*frame->previewBuffer;
                } else if (source == &*frame->previewBuffer) {
                    derived = &*frame->videoBuffer;
                }
                derivedRc = derived != nullptr
                        ? copyNv21Buffer(frame, *source, *derived)
                        : -EINVAL;
            }
            if (frame->analysisBuffer.has_value()) {
                analysisRc = copyPreviewToAnalysis(frame, source);
            }

            // Do not return the direct HAL1 source buffer until every derived
            // output has finished reading it. Once process_capture_result() is
            // called, the framework may immediately recycle that buffer.
            if (source != nullptr) {
                sendResult(frame, *source, !metadataReturned);
                metadataReturned = true;
                if (derived != nullptr) {
                    if (derivedRc == 0) {
                        sendResult(frame, *derived, !metadataReturned);
                        metadataReturned = true;
                    } else {
                        sendBufferError(frame->frameNumber, *derived);
                    }
                }
            } else {
                // A configured non-scratch frame must always have a direct
                // preview source. Return every affected output explicitly so
                // CameraService cannot wait forever for a missing buffer.
                if (frame->previewBuffer.has_value()) {
                    sendBufferError(frame->frameNumber, *frame->previewBuffer);
                }
                if (frame->videoBuffer.has_value()) {
                    sendBufferError(frame->frameNumber, *frame->videoBuffer);
                }
            }
            if (frame->analysisBuffer.has_value()) {
                if (analysisRc == 0) {
                    sendResult(frame, *frame->analysisBuffer, !metadataReturned);
                    metadataReturned = true;
                } else {
                    sendBufferError(frame->frameNumber, *frame->analysisBuffer);
                }
            }
            frame->metadataReturned = metadataReturned;
        }

        if (scratch != nullptr) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                scratch->inUse = false;
            }
            previewCv_.notify_all();
        }

        // These buffers have been returned to the framework; do not retain
        // them in pendingJpeg_, otherwise flush() could return them twice.
        frame->previewBuffer.reset();
        frame->videoBuffer.reset();
        frame->analysisBuffer.reset();
        if (frame->jpegBuffer.has_value()) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                pendingJpeg_ = frame;
                previewAbort_ = true;
            }
            previewCv_.notify_all();
            postTask(WorkerTask::TakePicture, frame->generation);
        }
        return 0;
    }

    static int previewSetBufferCount(preview_stream_ops_t*, int) { return 0; }
    static int previewSetBuffersGeometry(preview_stream_ops_t*, int, int, int) { return 0; }
    static int previewSetCrop(preview_stream_ops_t*, int, int, int, int) { return 0; }
    static int previewSetUsage(preview_stream_ops_t*, int) { return 0; }
    static int previewSetSwapInterval(preview_stream_ops_t*, int) { return 0; }
    static int previewGetMinUndequeuedCount(const preview_stream_ops_t*, int* count) {
        if (count == nullptr) return -EINVAL;
        // The Exynos4 HAL expects a conventional BufferQueue reserve.
        *count = 2;
        return 0;
    }
    static int previewLockBuffer(preview_stream_ops_t*, buffer_handle_t*) { return 0; }
    static int previewSetTimestamp(preview_stream_ops_t*, int64_t) { return 0; }

    static void legacyNotifyCallback(int32_t messageType, int32_t ext1, int32_t,
                                     void* user) {
        auto* self = static_cast<Camera3Shim*>(user);
        if (self == nullptr) return;
        if (messageType == CAMERA_MSG_FOCUS) {
            self->afState_.store(ext1 != 0
                    ? ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED
                    : ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED);
        } else if (messageType == CAMERA_MSG_ERROR && self->callbacks_ != nullptr &&
                   self->callbacks_->notify != nullptr) {
            camera3_notify_msg_t message{};
            message.type = CAMERA3_MSG_ERROR;
            message.message.error.frame_number = 0;
            message.message.error.error_stream = nullptr;
            message.message.error.error_code = CAMERA3_MSG_ERROR_DEVICE;
            self->callbacks_->notify(self->callbacks_, &message);
        }
    }

    static void legacyDataCallback(int32_t messageType, const camera_memory_t* data,
                                   unsigned int index, camera_frame_metadata_t*, void* user) {
        auto* self = static_cast<Camera3Shim*>(user);
        if (self != nullptr && messageType == CAMERA_MSG_COMPRESSED_IMAGE) {
            self->handleJpeg(data, index);
        }
    }

    static void legacyTimestampCallback(int64_t, int32_t, const camera_memory_t*,
                                        unsigned int, void*) {}

    void handleJpeg(const camera_memory_t* memory, unsigned int index) {
        std::shared_ptr<PendingFrame> frame;
        bool callbackActive = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            // The legacy picture thread is detached. flush() must wait until
            // every callback has stopped touching this shim, including a stale
            // callback whose pending request was already removed.
            ++jpegCallbacksActive_;
            callbackActive = true;
            frame.swap(pendingJpeg_);
            stillQueued_ = false;
            if (!flushing_ && !closing_) {
                previewAbort_ = false;
            }
            stillCv_.notify_all();
            previewCv_.notify_all();
        }

        auto finishCallback = [this, &callbackActive] {
            if (!callbackActive) {
                return;
            }
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (jpegCallbacksActive_ > 0) {
                --jpegCallbacksActive_;
            }
            callbackActive = false;
            jpegWorkerCv_.notify_all();
        };

        if (frame == nullptr || !frame->jpegBuffer.has_value()) {
            ALOGW("Ignoring JPEG callback without a pending camera3 request");
            finishCallback();
            return;
        }

        const uint32_t generation = frame->generation;
        bool staleGeneration = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            staleGeneration = generation != sessionGeneration_.load() ||
                    closing_ || flushing_;
        }
        if (staleGeneration) {
            ALOGW("Returning stale JPEG frame %u from generation %u",
                  frame->frameNumber, generation);
            failFrame(frame);
            finishCallback();
            return;
        }

        camera3_stream_buffer_t output = *frame->jpegBuffer;
        if (memory == nullptr || memory->data == nullptr || gralloc_ == nullptr) {
            ALOGE("JPEG callback did not provide usable image memory");
            if (frame->metadataReturned) {
                sendBufferError(frame->frameNumber, output);
                frame->jpegBuffer.reset();
            } else {
                failFrame(frame);
            }
        } else {
            const CameraDescriptor& descriptor = getCameraDescriptor(id_);
            if (index != 0) {
                ALOGW("Compressed-image callback used unexpected index %u", index);
            }
            const uint8_t* source = static_cast<const uint8_t*>(memory->data);
            const size_t sourceSize = memory->size;
            void* destination = nullptr;
            int rc = gralloc_->lock(gralloc_, *output.buffer, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                    0, 0, output.stream->width, output.stream->height,
                                    &destination);
            if (rc != 0 || destination == nullptr) {
                ALOGE("Could not lock JPEG output buffer: %d", rc);
                if (frame->metadataReturned) {
                    sendBufferError(frame->frameNumber, output);
                    frame->jpegBuffer.reset();
                } else {
                    failFrame(frame);
                }
            } else {
                const size_t footerSize = sizeof(camera3_jpeg_blob_t);
                const size_t capacity = static_cast<size_t>(descriptor.maxJpegSize);
                if (capacity <= footerSize || sourceSize > capacity - footerSize) {
                    ALOGE("JPEG size %zu exceeds camera3 BLOB capacity %zu",
                          sourceSize, capacity);
                    gralloc_->unlock(gralloc_, *output.buffer);
                    if (frame->metadataReturned) {
                        sendBufferError(frame->frameNumber, output);
                        frame->jpegBuffer.reset();
                    } else {
                        failFrame(frame);
                    }
                } else if (generation != sessionGeneration_.load()) {
                    gralloc_->unlock(gralloc_, *output.buffer);
                    failFrame(frame);
                } else {
                    memcpy(destination, source, sourceSize);
                    auto* footer = reinterpret_cast<camera3_jpeg_blob_t*>(
                            static_cast<uint8_t*>(destination) + capacity - footerSize);
                    footer->jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
                    footer->jpeg_size = static_cast<uint32_t>(sourceSize);
                    gralloc_->unlock(gralloc_, *output.buffer);
                    const bool includeMetadata = !frame->metadataReturned;
                    sendResult(frame, output, includeMetadata);
                    frame->metadataReturned = true;
                    frame->jpegBuffer.reset();
                }
            }
        }

        bool restartPreview = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            restartPreview = generation == sessionGeneration_.load() &&
                    !closing_ && !flushing_ && !previewQueue_.empty();
        }
        if (restartPreview) {
            postTask(WorkerTask::StartPreview, generation);
        }
        finishCallback();
    }

    int id_;
    camera3_device_t device_{};
    camera3_device_ops_t ops_{};
    camera_device_t* legacyDevice_ = nullptr;
    const gralloc_module_t* gralloc_ = nullptr;
    alloc_device_t* grallocAlloc_ = nullptr;
    const camera3_callback_ops_t* callbacks_ = nullptr;
    PreviewWindow previewWindow_{};

    std::mutex stateMutex_;
    std::mutex legacyOpsMutex_;
    std::condition_variable previewCv_;
    std::condition_variable stillCv_;
    std::condition_variable workerCv_;
    std::condition_variable jpegWorkerCv_;
    std::deque<std::shared_ptr<PendingFrame>> previewQueue_;
    std::map<buffer_handle_t*, std::shared_ptr<PendingFrame>> inFlightPreview_;
    std::array<ScratchPreviewBuffer, kScratchPreviewBufferCount> scratchPreviewBuffers_{};
    ScratchPreviewBuffer drainPreviewBuffer_{};
    std::shared_ptr<PendingFrame> pendingJpeg_;
    std::deque<WorkerCommand> workerTasks_;
    std::thread worker_;

    camera3_stream_t* previewStream_ = nullptr;
    camera3_stream_t* videoStream_ = nullptr;
    camera3_stream_t* analysisStream_ = nullptr;
    camera3_stream_t* sourceStream_ = nullptr;
    camera3_stream_t* jpegStream_ = nullptr;
    int previewWidth_ = 0;
    int previewHeight_ = 0;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    int analysisWidth_ = 0;
    int analysisHeight_ = 0;
    int jpegWidth_ = 0;
    int jpegHeight_ = 0;

    bool initialized_ = false;
    bool configured_ = false;
    bool useVideoAsSource_ = false;
    bool reconfiguring_ = false;
    bool haveRequestSettings_ = false;
    bool stillQueued_ = false;
    bool previewAbort_ = false;
    bool flushing_ = false;
    bool closing_ = false;
    bool jpegWorkerActive_ = false;
    size_t jpegCallbacksActive_ = 0;
    std::atomic<bool> previewStarted_{false};
    std::atomic<uint32_t> sessionGeneration_{1};
    std::atomic<bool> closed_{false};
    std::atomic<uint8_t> afState_{ANDROID_CONTROL_AF_STATE_INACTIVE};
    camera_metadata_t* lastSettings_ = nullptr;
    std::array<camera_metadata_t*, CAMERA3_TEMPLATE_COUNT> defaultRequests_{};
};

std::mutex gModuleCallbackMutex;
const camera_module_callbacks_t* gModuleCallbacks = nullptr;
std::mutex gTorchMutex;
camera_device_t* gTorchDevice = nullptr;
hw_module_methods_t gModuleMethods{};

void notifyTorchStatus(int status) {
    const camera_module_callbacks_t* callbacks = nullptr;
    {
        std::lock_guard<std::mutex> lock(gModuleCallbackMutex);
        callbacks = gModuleCallbacks;
    }
    if (callbacks != nullptr && callbacks->torch_mode_status_change != nullptr) {
        callbacks->torch_mode_status_change(callbacks, "0", status);
    }
}

void closeLegacyCameraDevice(camera_device_t* device) {
    if (device == nullptr) {
        return;
    }
    if (device->common.close != nullptr) {
        device->common.close(&device->common);
    } else if (device->ops != nullptr && device->ops->release != nullptr) {
        device->ops->release(device);
    }
}

int setLegacyTorchParameter(camera_device_t* device, bool enabled) {
    if (device == nullptr || device->ops == nullptr ||
        device->ops->get_parameters == nullptr ||
        device->ops->set_parameters == nullptr) {
        return -ENODEV;
    }

    char* oldParameters = device->ops->get_parameters(device);
    ParameterMap parameters(oldParameters);
    if (oldParameters != nullptr) {
        if (device->ops->put_parameters != nullptr) {
            device->ops->put_parameters(device, oldParameters);
        } else {
            free(oldParameters);
        }
    }

    parameters.set("flash-mode", enabled ? "torch" : "off");
    const std::string flattened = parameters.flatten();
    return device->ops->set_parameters(device, flattened.c_str());
}

void releaseTorchForCameraOpen() {
    std::lock_guard<std::mutex> lock(gTorchMutex);
    if (gTorchDevice == nullptr) {
        return;
    }
    setLegacyTorchParameter(gTorchDevice, false);
    closeLegacyCameraDevice(gTorchDevice);
    gTorchDevice = nullptr;
}

int getNumberOfCameras() {
    return LegacyModule::get().cameraCount();
}

int getCameraInfo(int cameraId, camera_info* info) {
    if (info == nullptr || cameraId < 0 || cameraId >= getNumberOfCameras()) {
        return -EINVAL;
    }
    camera_info legacyInfo{};
    int rc = LegacyModule::get().cameraInfo(cameraId, &legacyInfo);
    if (rc != 0) {
        return rc;
    }
    memset(info, 0, sizeof(*info));
    info->facing = legacyInfo.facing;
    info->orientation = legacyInfo.orientation;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_2;
    info->static_camera_characteristics = getStaticMetadata(
            cameraId, legacyInfo.facing, legacyInfo.orientation);
    info->resource_cost = 100;
    return info->static_camera_characteristics == nullptr ? -ENOMEM : 0;
}

int setCallbacks(const camera_module_callbacks_t* callbacks) {
    {
        std::lock_guard<std::mutex> lock(gModuleCallbackMutex);
        gModuleCallbacks = callbacks;
    }

    if (callbacks != nullptr && callbacks->torch_mode_status_change != nullptr) {
        callbacks->torch_mode_status_change(
                callbacks, "0", TORCH_MODE_STATUS_AVAILABLE_OFF);
        callbacks->torch_mode_status_change(
                callbacks, "1", TORCH_MODE_STATUS_NOT_AVAILABLE);
    }
    return 0;
}

int setTorchMode(const char* idString, bool enabled) {
    if (idString == nullptr) {
        return -EINVAL;
    }

    char* end = nullptr;
    const long id = strtol(idString, &end, 10);
    if (end == idString || *end != '\0' || id < 0 || id >= getNumberOfCameras()) {
        return -EINVAL;
    }
    if (id != 0) {
        return -ENOSYS;
    }

    std::lock_guard<std::mutex> lock(gTorchMutex);
    if (enabled) {
        if (gTorchDevice != nullptr) {
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_ON);
            return 0;
        }

        camera_device_t* device = nullptr;
        int rc = LegacyModule::get().openCamera(0, &device);
        if (rc != 0 || device == nullptr) {
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
            return rc != 0 ? rc : -ENODEV;
        }

        rc = setLegacyTorchParameter(device, true);
        if (rc != 0) {
            closeLegacyCameraDevice(device);
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
            return rc;
        }

        gTorchDevice = device;
        notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_ON);
        return 0;
    }

    if (gTorchDevice != nullptr) {
        const int rc = setLegacyTorchParameter(gTorchDevice, false);
        closeLegacyCameraDevice(gTorchDevice);
        gTorchDevice = nullptr;
        notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
        return rc;
    }

    notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
    return 0;
}

int initModule() {
    return LegacyModule::get().ensureLoaded() ? 0 : -ENODEV;
}

int openCamera(const hw_module_t* module, const char* idString, hw_device_t** device) {
    if (module == nullptr || idString == nullptr || device == nullptr) {
        return -EINVAL;
    }
    char* end = nullptr;
    const long id = strtol(idString, &end, 10);
    if (end == idString || *end != '\0' || id < 0 || id >= getNumberOfCameras()) {
        return -EINVAL;
    }
    if (id == 0) {
        releaseTorchForCameraOpen();
        notifyTorchStatus(TORCH_MODE_STATUS_NOT_AVAILABLE);
    }

    auto camera = std::make_unique<Camera3Shim>(static_cast<int>(id));
    int rc = camera->openLegacy();
    if (rc != 0) {
        if (id == 0) {
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
        }
        return rc;
    }
    camera->device()->common.module = const_cast<hw_module_t*>(module);
    Camera3Shim* rawCamera = camera.release();
    *device = &rawCamera->device()->common;
    return 0;
}

}  // namespace
}  // namespace n7000::camera3

extern "C" {

__attribute__((visibility("default"))) camera_module_t HAL_MODULE_INFO_SYM;

__attribute__((constructor)) static void initializeCameraModule() {
    using namespace n7000::camera3;
    memset(&HAL_MODULE_INFO_SYM, 0, sizeof(HAL_MODULE_INFO_SYM));
    memset(&gModuleMethods, 0, sizeof(gModuleMethods));
    gModuleMethods.open = openCamera;

    HAL_MODULE_INFO_SYM.common.tag = HARDWARE_MODULE_TAG;
    HAL_MODULE_INFO_SYM.common.module_api_version = CAMERA_MODULE_API_VERSION_2_4;
    HAL_MODULE_INFO_SYM.common.hal_api_version = HARDWARE_HAL_API_VERSION;
    HAL_MODULE_INFO_SYM.common.id = CAMERA_HARDWARE_MODULE_ID;
    HAL_MODULE_INFO_SYM.common.name = "N7000 HAL3 to HAL1 C ABI wrapper";
    HAL_MODULE_INFO_SYM.common.author = "ZhafKnight";
    HAL_MODULE_INFO_SYM.common.methods = &gModuleMethods;
    HAL_MODULE_INFO_SYM.get_number_of_cameras = getNumberOfCameras;
    HAL_MODULE_INFO_SYM.get_camera_info = getCameraInfo;
    HAL_MODULE_INFO_SYM.set_callbacks = setCallbacks;
    HAL_MODULE_INFO_SYM.set_torch_mode = setTorchMode;
    HAL_MODULE_INFO_SYM.init = initModule;
}

}  // extern "C"
