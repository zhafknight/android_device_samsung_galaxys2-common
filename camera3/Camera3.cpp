#define LOG_TAG "Camera3"

#include <log/log.h>

#include "Metadata.h"
#include "SecFimc.h"
#include "exynos_camera_backend.h"
#include "gralloc_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <hardware/camera.h>
#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
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
#include <sys/resource.h>
#include <unistd.h>

#ifndef GRALLOC_USAGE_HW_VIDEO_ENCODER
#define GRALLOC_USAGE_HW_VIDEO_ENCODER 0x00010000U
#endif

namespace camera3 {
namespace {

constexpr int kMaxCameras = 2;
constexpr int kAndroidPriorityUrgentDisplay = -8;

bool isNativeHdRecordingSize(uint32_t width, uint32_t height) {
    return (width == 1280 && height == 720) ||
           (width == 1920 && height == 1080);
}

void setNativeCameraThreadPriority(const char* role, int priority) {
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, priority) != 0) {
        ALOGW("Could not set %s thread priority to %d: %s",
              role, priority, strerror(errno));
        return;
    }
    ALOGI("Using Android priority %d for %s thread", priority, role);
}

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


struct BackendMemory {
    camera_memory_t camera{};
    void* allocation = nullptr;
    size_t allocationSize = 0;
    bool mapped = false;
};

void releaseBackendMemory(camera_memory_t* memory) {
    if (memory == nullptr) {
        return;
    }
    auto* holder = reinterpret_cast<BackendMemory*>(
            reinterpret_cast<uint8_t*>(memory) - offsetof(BackendMemory, camera));
    if (holder->allocation != nullptr) {
        if (holder->mapped) {
            munmap(holder->allocation, holder->allocationSize);
        } else {
            free(holder->allocation);
        }
    }
    delete holder;
}

camera_memory_t* requestBackendMemory(int fd, size_t bufferSize, unsigned int bufferCount,
                                     void*) {
    if (bufferSize == 0 || bufferCount == 0 || bufferSize > SIZE_MAX / bufferCount) {
        return nullptr;
    }
    const size_t totalSize = bufferSize * bufferCount;
    auto* holder = new (std::nothrow) BackendMemory();
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
    holder->camera.release = releaseBackendMemory;
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

    // Based on acroreiser's HAL3on1 zoom approach.
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
    std::string jpegParameters;
    bool metadataReturned = false;
    bool requestErrorNotified = false;
};

void notifyTorchStatus(int status);

class NativeCamera3Device {
public:
    explicit NativeCamera3Device(int id) : id_(id) {
        memset(&device_, 0, sizeof(device_));
        memset(&ops_, 0, sizeof(ops_));

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

        const hw_module_t* grallocModule = nullptr;
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &grallocModule) == 0) {
            gralloc_ = reinterpret_cast<const gralloc_module_t*>(grallocModule);
            const int allocRc = gralloc_open(grallocModule, &grallocAlloc_);
            if (allocRc != 0) {
                ALOGE("Could not open gralloc allocator: %d", allocRc);
                grallocAlloc_ = nullptr;
            }
        }

        worker_ = std::thread(&NativeCamera3Device::workerLoop, this);
        resultWorker_ = std::thread(&NativeCamera3Device::resultLoop, this);
        previewScalerWorker_ = std::thread(&NativeCamera3Device::previewScalerLoop, this);
        videoBlitWorker_ = std::thread(&NativeCamera3Device::videoBlitLoop, this);
    }

    ~NativeCamera3Device() {
        closeInternal();
        stopVideoBlitWorker();
        stopPreviewScaler();
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

    int openBackend() {
        const int rc = exynos_camera_backend_open(id_, &backend_);
        if (rc != 0 || backend_ == nullptr) {
            ALOGE("Native Exynos backend open camera %d failed: %d", id_, rc);
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

    struct ScratchPreviewBuffer {
        buffer_handle_t handle = nullptr;
        int stride = 0;
        camera3_stream_t stream{};
        camera3_stream_buffer_t buffer{};
        bool inUse = false;
    };

    struct NativeResultJob {
        std::shared_ptr<PendingFrame> frame;
        std::array<camera3_stream_buffer_t, 3> buffers{};
        uint32_t bufferCount = 0;
        bool sendShutter = false;
        bool includeMetadata = false;
    };

    struct NativeVideoBlitJob {
        std::shared_ptr<PendingFrame> frame;
        const void* source = nullptr;
        size_t sourceSize = 0;
        uint32_t sourceYAddr = 0;
        uint32_t sourceCbcrAddr = 0;
        int width = 0;
        int height = 0;
        camera3_stream_buffer_t* target = nullptr;
        uint64_t sequence = 0;
    };

    // The legacy preview window is strictly serial: it dequeues one target,
    // copies one V4L2 frame, and enqueues that target before requesting the
    // next. One scratch target is therefore sufficient. Keeping four 720p
    // scratch allocations consumed about 4 MiB extra contiguous memory and
    // left the V4L2 driver with only its minimum three capture buffers.
    static constexpr size_t kScratchPreviewBufferCount = 1;

    static NativeCamera3Device* from(const camera3_device_t* device) {
        return device == nullptr ? nullptr : static_cast<NativeCamera3Device*>(device->priv);
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
        NativeCamera3Device* self = from(device);
        return self == nullptr ? -EINVAL : self->initialize(callbacks);
    }

    static int configureStreamsDevice(const camera3_device_t* device,
                                      camera3_stream_configuration_t* streams) {
        NativeCamera3Device* self = from(device);
        return self == nullptr ? -EINVAL : self->configureStreams(streams);
    }

    static const camera_metadata_t* constructDefaultRequestSettingsDevice(
            const camera3_device_t* device, int type) {
        NativeCamera3Device* self = from(device);
        return self == nullptr ? nullptr : self->constructDefaultRequestSettings(type);
    }

    static int processCaptureRequestDevice(const camera3_device_t* device,
                                           camera3_capture_request_t* request) {
        NativeCamera3Device* self = from(device);
        return self == nullptr ? -EINVAL : self->processCaptureRequest(request);
    }

    static void dumpDevice(const camera3_device_t* device, int fd) {
        NativeCamera3Device* self = from(device);
        if (self != nullptr) {
            self->dump(fd);
        }
    }

    static int flushDevice(const camera3_device_t* device) {
        NativeCamera3Device* self = from(device);
        return self == nullptr ? -EINVAL : self->flush();
    }

    int initialize(const camera3_callback_ops_t* callbacks) {
        if (callbacks == nullptr || callbacks->notify == nullptr ||
            callbacks->process_capture_result == nullptr || backend_ == nullptr) {
            return -EINVAL;
        }
        callbacks_ = callbacks;
        exynos_camera_backend_set_callbacks(backend_, backendNotifyCallback,
                                            backendDataCallback, backendTimestampCallback,
                                            requestBackendMemory, this);
        exynos_camera_backend_set_frame_callback(backend_, backendFrameCallback, this);
        ALOGI("Direct V4L2-to-Camera3 frame delivery enabled");
        ALOGI("Asynchronous FIFO Camera3 result delivery enabled");
        exynos_camera_backend_enable_messages(
                backend_, CAMERA_MSG_ERROR | CAMERA_MSG_FOCUS |
                          CAMERA_MSG_SHUTTER | CAMERA_MSG_COMPRESSED_IMAGE);
        initialized_ = true;
        return 0;
    }

    bool supportedPreviewSize(int width, int height) const {
        const std::vector<std::pair<int, int>> sizes = id_ == 0
                ? std::vector<std::pair<int, int>>{{1920, 1080}, {1280, 720},
                                                   {800, 480}, {720, 480}, {640, 480},
                                                   {640, 360},
                                                   {352, 288}, {320, 240}, {176, 144}}
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
        const bool hadConfiguredNonVideoSession = configured_ && videoStream_ == nullptr;

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
                // previously working Camera3 wrapper.
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
                stream->format != HAL_PIXEL_FORMAT_YCrCb_420_SP &&
                stream->format != HAL_PIXEL_FORMAT_YCbCr_420_SP) {
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
            const bool nativeVideoStream = video == stream;

            if (stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                // Preview remains in the sensor-native NV21 layout. Recording
                // uses linear NV12, which is the native MFC input layout; FIMC
                // performs the NV21-to-NV12 conversion in hardware. HIDL
                // Camera3 explicitly permits a concrete format override when
                // the requested format is IMPLEMENTATION_DEFINED. It is also
                // mandatory here: the old Exynos4 EGL importer aborts when it
                // receives format 0x22, as seen in cam18.
                const int nativeFormat = nativeVideoStream
                        ? HAL_PIXEL_FORMAT_YCbCr_420_SP
                        : HAL_PIXEL_FORMAT_YCrCb_420_SP;
                ALOGI("Device-only stream override %s: "
                      "IMPLEMENTATION_DEFINED -> %s %ux%u inputUsage=0x%llx",
                      nativeVideoStream ? "video" : "preview",
                      nativeVideoStream ? "NV12" : "NV21",
                      stream->width, stream->height,
                      static_cast<unsigned long long>(stream->usage));
                stream->format = nativeFormat;
            }
            if (nativeVideoStream) {
                // FIMC1 writes an MFC-aligned contiguous NV12 surface and the
                // hardware encoder consumes its physical planes directly.
                // No CPU agent owns the recording stream.
                stream->usage |= GRALLOC_USAGE_HW_CAMERA_WRITE |
                                 GRALLOC_USAGE_HW_VIDEO_ENCODER |
                                 GRALLOC_USAGE_CAMERA3_CONTIGUOUS;
                stream->usage &= ~(GRALLOC_USAGE_SW_READ_MASK |
                                   GRALLOC_USAGE_SW_WRITE_MASK);
            } else {
                stream->usage |= GRALLOC_USAGE_SW_READ_OFTEN |
                                 GRALLOC_USAGE_SW_WRITE_OFTEN |
                                 GRALLOC_USAGE_HW_CAMERA_WRITE |
                                 GRALLOC_USAGE_CAMERA3_CONTIGUOUS;
            }
            ALOGI("Device-only configured %s stream: format=0x%x usage=0x%llx "
                  "size=%ux%u",
                  nativeVideoStream ? "video" : "preview", stream->format,
                  static_cast<unsigned long long>(stream->usage),
                  stream->width, stream->height);
            if (nativeVideoStream) {
                stream->max_buffers = stream->width == 1920 && stream->height == 1080
                        ? 4 : 6;
            } else {
                stream->max_buffers = 8;
            }
        }

        if (preview == nullptr && video == nullptr && analysis == nullptr &&
            jpeg == nullptr) {
            return -EINVAL;
        }

        if (hadConfiguredNonVideoSession && video != nullptr) {
            std::lock_guard<std::mutex> lock(backendOpsMutex_);
            const int resetRc = exynos_camera_backend_reset_capture_nodes(backend_);
            if (resetRc != 0) {
                ALOGE("Could not reset native capture route for photo-to-video: %d",
                      resetRc);
                return resetRc;
            }
        }
        bool useVideoAsSource = false;
        if (preview != nullptr && video != nullptr &&
            (preview->width != video->width || preview->height != video->height)) {
            // CameraX normally pairs a display-sized preview with a 1280x720
            // or 1920x1080 encoder surface. The Exynos4 camera pipeline has
            // two native capture nodes for this layout: FIMC0 scales the M5MO
            // monitor feed to preview while FIMC2 retains the full recording
            // size. Accept every advertised preview which is no larger than
            // that feed; no CPU bridge is involved.
            if (id_ == 0 && isNativeHdRecordingSize(video->width, video->height) &&
                preview->width <= video->width && preview->height <= video->height) {
                ALOGI("Using native dual Exynos streams: preview %ux%u + video %ux%u",
                      preview->width, preview->height, video->width, video->height);
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
        sourceStream_ = preview != nullptr ? preview : analysis;
        if (sourceStream_ == nullptr) {
            sourceStream_ = video;
        }
        jpegStream_ = jpeg;
        previewWidth_ = sourceStream_ != nullptr ? sourceStream_->width : 0;
        previewHeight_ = sourceStream_ != nullptr ? sourceStream_->height : 0;
        videoWidth_ = video != nullptr ? video->width : 0;
        videoHeight_ = video != nullptr ? video->height : 0;
        nativeVideoWidth_ = videoWidth_;
        nativeVideoHeight_ = videoHeight_;
        if (id_ == 0 && videoWidth_ == 1280 && videoHeight_ == 720) {
            nativeVideoWidth_ = 1072;
            nativeVideoHeight_ = 800;
            ALOGI("Full-FOV 720p path: M5MO 1072x800 -> "
                  "960x720 content in 1280x720 encoder stream");
        }
        nativePreviewWidth_ = previewWidth_;
        nativePreviewHeight_ = previewHeight_;
        if (id_ == 0 && video != nullptr && previewWidth_ == 640 &&
            previewHeight_ == 480 && videoWidth_ * 9 == videoHeight_ * 16 &&
            !(videoWidth_ == 1280 && videoHeight_ == 720)) {
            nativePreviewHeight_ = 360;
            ALOGI("Aspect-correct native preview path: M5MO 16:9 -> "
                  "%dx%d content in %dx%d Camera3 stream",
                  nativePreviewWidth_, nativePreviewHeight_,
                  previewWidth_, previewHeight_);
        }
        analysisWidth_ = analysis != nullptr ? analysis->width : 0;
        analysisHeight_ = analysis != nullptr ? analysis->height : 0;
        jpegWidth_ = jpeg != nullptr ? jpeg->width : 0;
        jpegHeight_ = jpeg != nullptr ? jpeg->height : 0;
        useVideoAsSource_ = useVideoAsSource;
        nativeFrameWindowStartNs_ = 0;
        nativeFrameCount_ = 0;
        nativeFrameProcessingNs_ = 0;
        nativeDequeueWaitNs_ = 0;
        nativePreviewCopyNs_ = 0;
        nativeResultDeliveryNs_ = 0;
        nativeResultDeliveryCount_ = 0;
        nativeShutterDeliveryNs_ = 0;
        nativeShutterDeliveryCount_ = 0;
        previewBlitterUnavailable_ = false;
        previewLetterboxBuffers_.clear();
        videoPillarboxBuffers_.clear();
        previewDmaCount_ = 0;
        previewCpuCopyCount_ = 0;
        videoBlitCount_ = 0;
        videoFenceWaitNs_ = 0;
        videoBlitProcessingNs_ = 0;
        resetPreviewScaler(sessionGeneration_.load());
        if (videoStream_ != nullptr && previewStream_ != nullptr &&
            isNativeHdRecordingSize(videoStream_->width, videoStream_->height) &&
            previewStream_->width <= videoStream_->width &&
            previewStream_->height <= videoStream_->height) {
            ALOGI("Native node0/node2 recording path enabled: preview %ux%u, "
                  "video %ux%u; parallel FIMC1/FIMC3 DMA",
                  previewStream_->width, previewStream_->height,
                  videoStream_->width, videoStream_->height);
        }

        // The Exynos V4L2 preview loop does not check dequeue_buffer()'s
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

        // Internal source buffers are required when the configured V4L2 source
        // is not present in an individual request (for example a preview-only
        // request in a preview+HD-video session, or an ImageAnalysis-only
        // request). The capture engine still needs an NV21 destination.
        if ((videoStream_ != nullptr && previewStream_ != nullptr &&
             (videoStream_->width != previewStream_->width ||
              videoStream_->height != previewStream_->height)) ||
            analysisStream_ != nullptr ||
            (jpegStream_ != nullptr && sourceStream_ != nullptr)) {
            const int scratchRc = allocateScratchPreviewBuffers(previewWidth_, previewHeight_);
            if (scratchRc != 0) {
                ALOGE("Could not allocate %dx%d internal preview buffers: %d",
                      previewWidth_, previewHeight_, scratchRc);
                useVideoAsSource_ = false;
                releaseDrainPreviewBuffer();
                return scratchRc;
            }
        }

        int rc = updateBackendParameters(nullptr);
        if (rc != 0) {
            ALOGE("Initial native backend parameter configuration failed: %d", rc);
            releaseScratchPreviewBuffers();
            releaseDrainPreviewBuffer();
            return rc;
        }
        {
            std::lock_guard<std::mutex> lock(backendOpsMutex_);
            exynos_camera_backend_set_recording_stream(
                    backend_, videoStream_ != nullptr ? 1 : 0);
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
        // full sensor parameter string for every frame is expensive
        // and can serialize process_capture_request() behind still capture.
        if (request->settings != nullptr || frame->jpegBuffer.has_value()) {
            std::string* jpegParameters = frame->jpegBuffer.has_value()
                    ? &frame->jpegParameters
                    : nullptr;
            if (updateBackendParameters(effectiveSettings, jpegParameters) != 0) {
                ALOGW("Could not apply all request settings to native backend");
            }
            if (request->settings != nullptr) {
                handleAfTrigger(effectiveSettings);
            }
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

        const bool hasRealtimeOutput = frame->previewBuffer.has_value() ||
                frame->videoBuffer.has_value() || frame->analysisBuffer.has_value();
        const bool needsOrderedPreviewCapture = hasRealtimeOutput ||
                (frame->jpegBuffer.has_value() && sourceStream_ != nullptr);
        // Every request in an active preview session, including JPEG-only, is
        // sequenced through the native V4L2 callback. This preserves Camera3's
        // strictly increasing shutter order when repeating preview frames were
        // already queued ahead of a still capture. A standalone JPEG session has
        // no preview source and therefore uses the monotonic fallback.
        if (!needsOrderedPreviewCapture) {
            sendShutter(frame->frameNumber, frame->timestamp);
            sendMetadata(frame);
        }

        bool startPreview = false;
        bool takePicture = false;
        bool sessionChanged = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            sessionChanged = closing_ || flushing_ ||
                    frame->generation != sessionGeneration_.load();
            if (!sessionChanged && needsOrderedPreviewCapture) {
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

    int updateBackendParameters(const camera_metadata_t* settings,
                                std::string* appliedParameters = nullptr) {
        if (backend_ == nullptr) {
            return -ENODEV;
        }
        std::lock_guard<std::mutex> lock(backendOpsMutex_);
        char* oldParameters = exynos_camera_backend_get_parameters(backend_);
        ParameterMap parameters(oldParameters);
        if (oldParameters != nullptr) {
            exynos_camera_backend_free_parameters(oldParameters);
        }

        if (sourceStream_ != nullptr) {
            parameters.set("preview-size", std::to_string(nativePreviewWidth_) + "x" +
                                           std::to_string(nativePreviewHeight_));
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
            parameters.set("video-size", std::to_string(nativeVideoWidth_) + "x" +
                                         std::to_string(nativeVideoHeight_));
            parameters.set("video-frame-format", "yuv420sp");
        } else {
            parameters.set("recording-hint", "false");
            if (sourceStream_ != nullptr) {
                parameters.set("video-size", std::to_string(nativePreviewWidth_) + "x" +
                                             std::to_string(nativePreviewHeight_));
            }
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
        if (appliedParameters != nullptr) {
            *appliedParameters = flattened;
        }
        return exynos_camera_backend_set_parameters(backend_, flattened.c_str());
    }

    void handleAfTrigger(const camera_metadata_t* settings) {
        if (settings == nullptr || backend_ == nullptr || id_ != 0) {
            return;
        }
        camera_metadata_ro_entry_t entry{};
        if (find_camera_metadata_ro_entry(settings, ANDROID_CONTROL_AF_TRIGGER, &entry) != 0 ||
            entry.count == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(backendOpsMutex_);
        if (entry.data.u8[0] == ANDROID_CONTROL_AF_TRIGGER_START) {
            afState_.store(ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN);
            exynos_camera_backend_auto_focus(backend_);
        } else if (entry.data.u8[0] == ANDROID_CONTROL_AF_TRIGGER_CANCEL) {
            exynos_camera_backend_cancel_auto_focus(backend_);
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

        // Flexible YUV buffers cannot be passed to the V4L2 stream adapter,
        // and a request is not required to target every configured stream.
        // Supply an internal NV21 destination whenever this request does not
        // contain a usable direct NV21 source buffer.
        return sourceBuffer(frame) == nullptr &&
               (frame->previewBuffer.has_value() || frame->videoBuffer.has_value() ||
                frame->analysisBuffer.has_value() || frame->jpegBuffer.has_value());
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
            // the backend write 720p into a 640x480 allocation. Only permit fallback
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
        // Flexible YUV analysis buffers are never handed directly to V4L2.
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

    static void scaleHdNv21ToVga(const uint8_t* source, uint8_t* target) {
        constexpr size_t kSourceWidth = 1280;
        constexpr size_t kSourceHeight = 720;
        constexpr size_t kTargetWidth = 640;
        constexpr size_t kTargetHeight = 480;
        constexpr size_t kCropByteX = 160;

        // The 960x720 center crop scales to 640x480 by keeping two pixels
        // from every group of three. Process two output rows at a time so the
        // Cortex-A9 loop contains no multiplies or divides per pixel.
        for (size_t rowPair = 0; rowPair < kTargetHeight / 2U; ++rowPair) {
            const uint8_t* sourceRow0 = source + (rowPair * 3U) * kSourceWidth +
                    kCropByteX;
            const uint8_t* sourceRow1 = sourceRow0 + kSourceWidth;
            uint8_t* targetRow0 = target + (rowPair * 2U) * kTargetWidth;
            uint8_t* targetRow1 = targetRow0 + kTargetWidth;
            for (size_t group = 0; group < kTargetWidth / 2U; ++group) {
                const size_t sourceOffset = group * 3U;
                const size_t targetOffset = group * 2U;
                targetRow0[targetOffset] = sourceRow0[sourceOffset];
                targetRow0[targetOffset + 1U] = sourceRow0[sourceOffset + 1U];
                targetRow1[targetOffset] = sourceRow1[sourceOffset];
                targetRow1[targetOffset + 1U] = sourceRow1[sourceOffset + 1U];
            }
        }

        const uint8_t* sourceVu = source + kSourceWidth * kSourceHeight;
        uint8_t* targetVu = target + kTargetWidth * kTargetHeight;
        for (size_t rowPair = 0; rowPair < kTargetHeight / 4U; ++rowPair) {
            const uint8_t* sourceRow0 = sourceVu +
                    (rowPair * 3U) * kSourceWidth + kCropByteX;
            const uint8_t* sourceRow1 = sourceRow0 + kSourceWidth;
            uint8_t* targetRow0 = targetVu + (rowPair * 2U) * kTargetWidth;
            uint8_t* targetRow1 = targetRow0 + kTargetWidth;
            // Each iteration copies two VU pairs and skips the third pair.
            for (size_t group = 0; group < kTargetWidth / 4U; ++group) {
                memcpy(targetRow0 + group * 4U, sourceRow0 + group * 6U, 4U);
                memcpy(targetRow1 + group * 4U, sourceRow1 + group * 6U, 4U);
            }
        }
    }

    bool schedulePreviewCacheUpdate(camera3_stream_buffer_t& source,
                                    uint32_t generation) {
        if (gralloc_ == nullptr || source.stream == nullptr || source.buffer == nullptr ||
            source.stream->width != 1280 || source.stream->height != 720) {
            return false;
        }

        constexpr size_t kHdNv21Size = 1280U * 720U * 3U / 2U;
        std::lock_guard<std::mutex> lock(previewScalerMutex_);
        const bool cacheReady = previewCacheValid_ &&
                previewCacheGeneration_ == generation;
        const int64_t nowNs = systemTime(SYSTEM_TIME_MONOTONIC);
        constexpr int64_t kPreviewScaleIntervalNs = 100000000LL;
        if (previewScalerExit_ || previewScalePending_ || previewScaleActive_ ||
            generation != previewScalerGeneration_ ||
            (cacheReady && nowNs - previewScaleLastQueuedNs_ <
                     kPreviewScaleIntervalNs)) {
            return cacheReady;
        }

        void* sourceData = nullptr;
        const int rc = gralloc_->lock(gralloc_, *source.buffer,
                                      GRALLOC_USAGE_SW_READ_OFTEN,
                                      0, 0, 1280, 720, &sourceData);
        if (rc != 0 || sourceData == nullptr) {
            ALOGW("Could not stage 720p preview cache input: %d", rc);
            return cacheReady;
        }
        previewScaleInput_.resize(kHdNv21Size);
        memcpy(previewScaleInput_.data(), sourceData, kHdNv21Size);
        gralloc_->unlock(gralloc_, *source.buffer);

        previewScaleJobGeneration_ = generation;
        previewScaleLastQueuedNs_ = nowNs;
        previewScalePending_ = true;
        previewScalerCv_.notify_one();
        return cacheReady;
    }

    int copyPreviewCacheToBuffer(const std::shared_ptr<PendingFrame>& frame,
                                 camera3_stream_buffer_t& target) {
        if (frame == nullptr || gralloc_ == nullptr || target.stream == nullptr ||
            target.buffer == nullptr || target.stream->width != 640 ||
            target.stream->height != 480) {
            return -EINVAL;
        }
        if (waitAndCloseAcquireFence(&target) != 0) {
            return -errno;
        }

        void* targetData = nullptr;
        int rc = gralloc_->lock(gralloc_, *target.buffer,
                                GRALLOC_USAGE_SW_WRITE_OFTEN,
                                0, 0, 640, 480, &targetData);
        if (rc != 0 || targetData == nullptr) {
            return rc != 0 ? rc : -EIO;
        }

        {
            std::lock_guard<std::mutex> lock(previewScalerMutex_);
            if (!previewCacheValid_ ||
                previewCacheGeneration_ != frame->generation ||
                previewCache_.size() != 640U * 480U * 3U / 2U) {
                rc = -EAGAIN;
            } else {
                memcpy(targetData, previewCache_.data(), previewCache_.size());
            }
        }
        gralloc_->unlock(gralloc_, *target.buffer);
        return rc;
    }

    void resetPreviewScaler(uint32_t generation) {
        std::lock_guard<std::mutex> lock(previewScalerMutex_);
        previewScalerGeneration_ = generation;
        previewScalePending_ = false;
        previewCacheValid_ = false;
        previewScaleLastQueuedNs_ = 0;
        previewCache_.clear();
    }

    void resetVideoBlitter() {
        if (videoBlitter_.flagCreate() && !videoBlitter_.destroy()) {
            ALOGW("FIMC1 video blitter did not shut down cleanly");
        }
        videoBlitterWidth_ = 0;
        videoBlitterHeight_ = 0;
        videoBlitterDestinationWidth_ = 0;
        videoBlitterDestinationHeight_ = 0;
        videoBlitterFormat_ = 0;
    }

    uint64_t queueVideoBlit(const std::shared_ptr<PendingFrame>& frame,
                            const void* source, size_t sourceSize,
                            uint32_t sourceYAddr, uint32_t sourceCbcrAddr,
                            camera3_stream_buffer_t* target) {
        if (frame == nullptr || target == nullptr) {
            return 0;
        }

        std::unique_lock<std::mutex> lock(videoBlitMutex_);
        videoBlitDoneCv_.wait(lock, [this] {
            return videoBlitExit_ || (!videoBlitPending_ && !videoBlitActive_);
        });
        if (videoBlitExit_) {
            return 0;
        }

        NativeVideoBlitJob job;
        job.frame = frame;
        job.source = source;
        job.sourceSize = sourceSize;
        job.sourceYAddr = sourceYAddr;
        job.sourceCbcrAddr = sourceCbcrAddr;
        job.width = nativeVideoWidth_;
        job.height = nativeVideoHeight_;
        job.target = target;
        job.sequence = ++videoBlitSubmittedSequence_;
        videoBlitJob_ = std::move(job);
        videoBlitPending_ = true;
        videoBlitCv_.notify_one();
        return videoBlitSubmittedSequence_;
    }

    int waitForVideoBlit(uint64_t sequence) {
        if (sequence == 0) {
            return -ECANCELED;
        }
        std::unique_lock<std::mutex> lock(videoBlitMutex_);
        videoBlitDoneCv_.wait(lock, [this, sequence] {
            return videoBlitCompletedSequence_ >= sequence ||
                    (videoBlitExit_ && !videoBlitPending_ && !videoBlitActive_);
        });
        return videoBlitCompletedSequence_ >= sequence
                ? videoBlitResult_
                : -ECANCELED;
    }

    void videoBlitLoop() {
        setNativeCameraThreadPriority("FIMC1 video", kAndroidPriorityUrgentDisplay);
        while (true) {
            NativeVideoBlitJob job;
            {
                std::unique_lock<std::mutex> lock(videoBlitMutex_);
                videoBlitCv_.wait(lock, [this] {
                    return videoBlitExit_ || videoBlitPending_;
                });
                if (videoBlitExit_ && !videoBlitPending_) {
                    return;
                }
                job = std::move(videoBlitJob_);
                videoBlitPending_ = false;
                videoBlitActive_ = true;
            }

            const int rc = copyNativeNv21ToBuffer(
                    job.frame, job.source, job.sourceSize,
                    job.sourceYAddr, job.sourceCbcrAddr,
                    job.width, job.height, job.target);

            {
                std::lock_guard<std::mutex> lock(videoBlitMutex_);
                videoBlitResult_ = rc;
                videoBlitCompletedSequence_ = job.sequence;
                videoBlitActive_ = false;
            }
            videoBlitDoneCv_.notify_all();
        }
    }

    void stopVideoBlitWorker() {
        {
            std::lock_guard<std::mutex> lock(videoBlitMutex_);
            videoBlitExit_ = true;
        }
        videoBlitCv_.notify_all();
        videoBlitDoneCv_.notify_all();
        if (videoBlitWorker_.joinable()) {
            videoBlitWorker_.join();
        }
    }

    void resetPreviewBlitter() {
        if (previewBlitter_.flagCreate() && !previewBlitter_.destroy()) {
            ALOGW("FIMC3 preview blitter did not shut down cleanly");
        }
        previewBlitterSourceWidth_ = 0;
        previewBlitterSourceHeight_ = 0;
        previewBlitterWidth_ = 0;
        previewBlitterHeight_ = 0;
        previewBlitterFormat_ = 0;
    }

    void previewScalerLoop() {
        constexpr size_t kVgaNv21Size = 640U * 480U * 3U / 2U;
        while (true) {
            uint32_t generation = 0;
            {
                std::unique_lock<std::mutex> lock(previewScalerMutex_);
                previewScalerCv_.wait(lock, [this] {
                    return previewScalerExit_ || previewScalePending_;
                });
                if (previewScalerExit_) {
                    return;
                }
                generation = previewScaleJobGeneration_;
                previewScalePending_ = false;
                previewScaleActive_ = true;
            }

            previewScaleOutput_.resize(kVgaNv21Size);
            scaleHdNv21ToVga(previewScaleInput_.data(), previewScaleOutput_.data());

            {
                std::lock_guard<std::mutex> lock(previewScalerMutex_);
                if (generation == previewScalerGeneration_) {
                    previewCache_.swap(previewScaleOutput_);
                    previewCacheGeneration_ = generation;
                    previewCacheValid_ = true;
                }
                previewScaleActive_ = false;
                previewScalerCv_.notify_all();
            }
        }
    }

    void stopPreviewScaler() {
        {
            std::lock_guard<std::mutex> lock(previewScalerMutex_);
            previewScalerExit_ = true;
            previewScalePending_ = false;
            previewScalerCv_.notify_all();
        }
        if (previewScalerWorker_.joinable()) {
            previewScalerWorker_.join();
        }
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
        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        camera3_capture_result_t result{};
        result.frame_number = frameNumber;
        result.num_output_buffers = 1;
        result.output_buffers = &buffer;
        callbacks_->process_capture_result(callbacks_, &result);
    }

    void sendBufferError(uint32_t frameNumber, camera3_stream_buffer_t buffer) {
        returnBufferError(frameNumber, buffer, true);
    }

    void notifyBufferError(uint32_t frameNumber, camera3_stream_t* stream) {
        if (callbacks_ == nullptr || callbacks_->notify == nullptr || stream == nullptr) {
            return;
        }
        camera3_notify_msg_t message{};
        message.type = CAMERA3_MSG_ERROR;
        message.message.error.frame_number = frameNumber;
        message.message.error.error_stream = stream;
        message.message.error.error_code = CAMERA3_MSG_ERROR_BUFFER;
        callbacks_->notify(callbacks_, &message);
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

        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
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

        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
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
        sendResults(frame, &buffer, 1, includeMetadata);
    }

    void sendResults(const std::shared_ptr<PendingFrame>& frame,
                     camera3_stream_buffer_t* buffers, uint32_t bufferCount,
                     bool includeMetadata) {
        if (callbacks_ == nullptr || callbacks_->process_capture_result == nullptr) {
            return;
        }
        for (uint32_t i = 0; i < bufferCount; ++i) {
            if (buffers[i].status == CAMERA3_BUFFER_STATUS_OK) {
                buffers[i].acquire_fence = -1;
                buffers[i].release_fence = -1;
            }
        }
        camera_metadata_t* metadata = includeMetadata
                ? buildResultMetadata(id_, frame->timestamp, afState_.load(),
                                      frame->cropRegion.data())
                : nullptr;
        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        camera3_capture_result_t result{};
        result.frame_number = frame->frameNumber;
        result.result = metadata;
        result.num_output_buffers = bufferCount;
        result.output_buffers = buffers;
        result.partial_result = metadata != nullptr ? 1 : 0;
        callbacks_->process_capture_result(callbacks_, &result);
        if (metadata != nullptr) {
            free_camera_metadata(metadata);
        }
    }

    void queueNativeResult(NativeResultJob&& job) {
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            resultQueue_.push_back(std::move(job));
        }
        resultCv_.notify_one();
    }

    void resultLoop() {
        // process_capture_result() returns the preview and video buffers to
        // CameraX.  At 720p a delayed result therefore starves both consumers
        // even though FIMC and MFC have already finished their work.  Keep the
        // result dispatcher in the same urgent-display class as the native
        // capture/FIMC workers so app-side GC cannot extend that ownership
        // window by another frame interval.
        setNativeCameraThreadPriority("Camera3 result",
                                      kAndroidPriorityUrgentDisplay);
        while (true) {
            NativeResultJob job;
            {
                std::unique_lock<std::mutex> lock(resultMutex_);
                resultCv_.wait(lock, [this] {
                    return resultWorkerExit_ || !resultQueue_.empty();
                });
                if (resultWorkerExit_ && resultQueue_.empty()) {
                    return;
                }
                job = std::move(resultQueue_.front());
                resultQueue_.pop_front();
                resultWorkerActive_ = true;
            }

            if (job.sendShutter && job.frame != nullptr) {
                const int64_t shutterStartNs = systemTime(SYSTEM_TIME_MONOTONIC);
                sendShutter(job.frame->frameNumber, job.frame->timestamp);
                nativeShutterDeliveryNs_.fetch_add(
                        systemTime(SYSTEM_TIME_MONOTONIC) - shutterStartNs,
                        std::memory_order_relaxed);
                nativeShutterDeliveryCount_.fetch_add(1, std::memory_order_relaxed);
            }

            // A mixed-success capture must advertise each failed buffer before
            // returning it with CAMERA3_BUFFER_STATUS_ERROR. Keep these HIDL
            // notifications off the V4L2 capture thread as well.
            if (job.frame != nullptr) {
                for (uint32_t i = 0; i < job.bufferCount; ++i) {
                    if (job.buffers[i].status == CAMERA3_BUFFER_STATUS_ERROR) {
                        notifyBufferError(job.frame->frameNumber,
                                          job.buffers[i].stream);
                    }
                }
            }

            const int64_t resultStartNs = systemTime(SYSTEM_TIME_MONOTONIC);
            sendResults(job.frame, job.buffers.data(), job.bufferCount,
                        job.includeMetadata);
            nativeResultDeliveryNs_.fetch_add(
                    systemTime(SYSTEM_TIME_MONOTONIC) - resultStartNs,
                    std::memory_order_relaxed);
            nativeResultDeliveryCount_.fetch_add(1, std::memory_order_relaxed);

            if (job.frame != nullptr) {
                if (job.includeMetadata) {
                    job.frame->metadataReturned = true;
                }

                // process_capture_result() transfers these buffers back to the
                // framework. Drop every local reference before handling a JPEG
                // continuation so flush() can never return one twice.
                job.frame->previewBuffer.reset();
                job.frame->videoBuffer.reset();
                job.frame->analysisBuffer.reset();

                if (job.frame->jpegBuffer.has_value()) {
                    bool scheduleJpeg = false;
                    {
                        std::lock_guard<std::mutex> lock(stateMutex_);
                        scheduleJpeg = !closing_ && !flushing_ &&
                                job.frame->generation == sessionGeneration_.load();
                        if (scheduleJpeg) {
                            pendingJpeg_ = job.frame;
                            previewAbort_ = true;
                        }
                    }
                    if (scheduleJpeg) {
                        previewCv_.notify_all();
                        postTask(WorkerTask::TakePicture, job.frame->generation);
                    } else {
                        // Only the JPEG buffer remains owned here. The realtime
                        // buffers above have already been delivered successfully.
                        failFrame(job.frame);
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(resultMutex_);
                resultWorkerActive_ = false;
            }
            resultIdleCv_.notify_all();
        }
    }

    void stopResultWorker() {
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            resultWorkerExit_ = true;
        }
        resultCv_.notify_all();
        if (resultWorker_.joinable()) {
            resultWorker_.join();
        }
    }

    int flush() {
        std::deque<std::shared_ptr<PendingFrame>> queued;
        std::deque<NativeResultJob> pendingResults;
        std::vector<std::shared_ptr<PendingFrame>> inFlight;
        std::shared_ptr<PendingFrame> jpeg;
        uint32_t newGeneration = 0;
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            flushing_ = true;
            previewAbort_ = true;
            newGeneration = sessionGeneration_.fetch_add(1) + 1;
            previewCv_.notify_all();
            stillCv_.notify_all();
            jpegWorkerCv_.wait(lock, [this] {
                return !jpegWorkerActive_ && jpegCallbacksActive_ == 0;
            });
            queued.swap(previewQueue_);
            jpeg.swap(pendingJpeg_);
            workerTasks_.erase(
                    std::remove_if(workerTasks_.begin(), workerTasks_.end(),
                                   [](const WorkerCommand& command) {
                                       return command.task != WorkerTask::Exit;
                                   }),
                    workerTasks_.end());
        }
        // Invalidate queued scaler work before returning any framework buffer.
        // An already-running scaler only owns its private CPU staging vectors
        // and will discard the result because its generation no longer matches.
        resetPreviewScaler(newGeneration);

        {
            std::lock_guard<std::mutex> lock(backendOpsMutex_);
            if (backend_ != nullptr) {
                exynos_camera_backend_cancel_picture(backend_);
                exynos_camera_backend_stop_preview(backend_);
            }
        }
        previewStarted_.store(false);

        // stop_preview() has joined the native frame callback, so no FIMC1 or
        // FIMC3 QBUF/DQBUF can still be active. Tear the persistent DMA
        // sessions down before returning or reconfiguring framework buffers.
        resetVideoBlitter();
        resetPreviewBlitter();

        // stop_preview() synchronizes with preview_mutex, which is held for
        // the complete native V4L2 callback. Collect in-flight Camera3 buffers
        // only after that callback has left. Clearing this map before stopping
        // caused a stale callback to return -ENOENT and kill the preview thread.
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            inFlight.reserve(inFlightPreview_.size());
            for (const auto& [buffer, frame] : inFlightPreview_) {
                (void)buffer;
                inFlight.push_back(frame);
            }
            inFlightPreview_.clear();
            for (auto& scratch : scratchPreviewBuffers_) {
                scratch.inUse = false;
            }
        }

        // A native frame leaves inFlightPreview_ once all CPU/FIMC writes are
        // complete, then waits here only for its Camera3 callback. Remove jobs
        // which have not started, and wait for the one callback which may
        // already be executing. This preserves FIFO result order and guarantees
        // that flush() returns every framework buffer exactly once.
        {
            std::unique_lock<std::mutex> lock(resultMutex_);
            pendingResults.swap(resultQueue_);
            resultIdleCv_.wait(lock, [this] { return !resultWorkerActive_; });
        }

        for (const auto& frame : queued) failFrame(frame);
        for (const auto& frame : inFlight) failFrame(frame);
        for (const auto& job : pendingResults) failFrame(job.frame);
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
        dprintf(fd, "Camera3 HAL\n");
        dprintf(fd, "camera id: %d\n", id_);
        dprintf(fd, "camera backend: direct Exynos4 V4L2/FIMC\n");
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
            std::lock_guard<std::mutex> lock(backendOpsMutex_);
            // The JPEG thread is detached. Stop any callback that starts
            // during close from retaining this HAL instance as its user pointer. An
            // already-running callback is tracked and drained by flush().
            if (backend_ != nullptr) {
                exynos_camera_backend_set_callbacks(
                        backend_, backendNotifyCallback, backendDataCallback,
                        backendTimestampCallback, requestBackendMemory, nullptr);
            }
        }
        flush();
        stopResultWorker();
        {
            std::lock_guard<std::mutex> lock(backendOpsMutex_);
            if (backend_ != nullptr) {
                exynos_camera_backend_close(backend_);
                backend_ = nullptr;
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
        std::lock_guard<std::mutex> lock(backendOpsMutex_);
        if (generation != sessionGeneration_.load() || backend_ == nullptr) {
            return;
        }

        const int rc = exynos_camera_backend_start_preview(backend_);
        if (rc == 0) {
            previewStarted_.store(true);
        } else {
            ALOGE("Native backend start_preview failed: %d", rc);
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
            // The backend stops and joins its preview thread from
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
            std::lock_guard<std::mutex> lock(backendOpsMutex_);
            if (generation == sessionGeneration_.load() && backend_ != nullptr) {
                // The still request waited behind older native preview frames.
                // Reapply its exact Camera3 parameter snapshot now so newer
                // repeating requests cannot replace JPEG orientation/quality
                // before the backend creates EXIF and starts capture.
                rc = frame->jpegParameters.empty()
                        ? 0
                        : exynos_camera_backend_set_parameters(
                                  backend_, frame->jpegParameters.c_str());
                if (rc == 0) {
                    rc = exynos_camera_backend_take_picture(backend_);
                }
            }
        }
        finishWorker();
        previewStarted_.store(false);

        if (generation != sessionGeneration_.load()) {
            return;
        }
        if (rc != 0) {
            ALOGE("Native backend take_picture failed: %d", rc);
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

    static int backendFrameCallback(const void* previewData, size_t previewSize,
                                    const void* recordingData, size_t recordingSize,
                                    uint32_t previewYAddr, uint32_t previewCbcrAddr,
                                    uint32_t recordingYAddr, uint32_t recordingCbcrAddr,
                                    int64_t timestampNs, void* user) {
        auto* self = static_cast<NativeCamera3Device*>(user);
        return self == nullptr
                ? -EINVAL
                : self->consumeNativeFrame(previewData, previewSize,
                                           recordingData, recordingSize,
                                           previewYAddr, previewCbcrAddr,
                                           recordingYAddr, recordingCbcrAddr,
                                           timestampNs);
    }

    int consumeNativeFrame(const void* previewData, size_t previewSize,
                           const void* recordingData, size_t recordingSize,
                           uint32_t previewYAddr, uint32_t previewCbcrAddr,
                           uint32_t recordingYAddr, uint32_t recordingCbcrAddr,
                           int64_t timestampNs) {
        if (previewData == nullptr || gralloc_ == nullptr) {
            return -EINVAL;
        }
        const int64_t processingStartNs = systemTime(SYSTEM_TIME_MONOTONIC);

        buffer_handle_t* handle = nullptr;
        int stride = 0;
        const int dequeueRc = dequeuePreview(&handle, &stride);
        if (dequeueRc != 0 || handle == nullptr) {
            return dequeueRc != 0 ? dequeueRc : -ENODEV;
        }
        const int64_t dequeueDoneNs = systemTime(SYSTEM_TIME_MONOTONIC);
        (void)stride;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (handle == &drainPreviewBuffer_.handle) {
                return 0;
            }
        }

        const size_t requiredSize = static_cast<size_t>(nativePreviewWidth_) *
                static_cast<size_t>(nativePreviewHeight_) * 3U / 2U;
        if (previewSize < requiredSize) {
            ALOGE("Native preview frame is too small: %zu/%zu",
                  previewSize, requiredSize);
            return completeNativeFrame(handle, true, recordingData, recordingSize,
                                       recordingYAddr, recordingCbcrAddr, false, 0);
        }

        // Resolve the Camera3 request before starting either memory-to-memory
        // operation. FIMC1 and FIMC3 are independent Exynos hardware engines,
        // so the HD NV21-to-NV12 conversion can run beside the 640x480 preview
        // transfer instead of extending the capture critical path.
        std::shared_ptr<PendingFrame> capturedFrame;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto it = inFlightPreview_.find(handle);
            if (it != inFlightPreview_.end()) {
                capturedFrame = it->second;
                if (capturedFrame != nullptr) {
                    capturedFrame->timestamp = timestampNs;
                }
            }
        }
        uint64_t videoBlitSequence = 0;
        if (capturedFrame != nullptr && capturedFrame->videoBuffer.has_value()) {
            videoBlitSequence = queueVideoBlit(
                    capturedFrame, recordingData, recordingSize,
                    recordingYAddr, recordingCbcrAddr,
                    &*capturedFrame->videoBuffer);
        }

        const int64_t previewCopyStartNs = systemTime(SYSTEM_TIME_MONOTONIC);
        const int previewTransferRc = copyNativePreviewToBuffer(
                previewData, requiredSize, previewYAddr, previewCbcrAddr, handle);
        const int64_t previewCopyNs =
                systemTime(SYSTEM_TIME_MONOTONIC) - previewCopyStartNs;
        const int videoRc = videoBlitSequence != 0
                ? waitForVideoBlit(videoBlitSequence)
                : 0;
        if (previewTransferRc != 0) {
            ALOGE("Could not transfer native Camera3 preview buffer: %d",
                  previewTransferRc);
            return completeNativeFrame(handle, true, recordingData, recordingSize,
                                       recordingYAddr, recordingCbcrAddr,
                                       videoBlitSequence != 0, videoRc);
        }

        // V4L2 callbacks are serialized in dequeue order, preserving Camera3's
        // monotonically increasing result order while using a real capture time.
        const int completeRc = completeNativeFrame(
                handle, false, recordingData, recordingSize,
                recordingYAddr, recordingCbcrAddr,
                videoBlitSequence != 0, videoRc);
        const int64_t processingNs =
                systemTime(SYSTEM_TIME_MONOTONIC) - processingStartNs;
        if (nativeFrameWindowStartNs_ == 0) {
            nativeFrameWindowStartNs_ = timestampNs;
        }
        ++nativeFrameCount_;
        nativeFrameProcessingNs_ += processingNs;
        nativeDequeueWaitNs_ += dequeueDoneNs - processingStartNs;
        nativePreviewCopyNs_ += previewCopyNs;
        const int64_t cadenceWindowNs = timestampNs - nativeFrameWindowStartNs_;
        if (cadenceWindowNs >= 5000000000LL && nativeFrameCount_ > 1) {
            const double fps = static_cast<double>(nativeFrameCount_ - 1U) * 1.0e9 /
                    static_cast<double>(cadenceWindowNs);
            const double averageProcessingMs =
                    static_cast<double>(nativeFrameProcessingNs_) / 1.0e6 /
                    static_cast<double>(nativeFrameCount_);
            const double averageDequeueMs =
                    static_cast<double>(nativeDequeueWaitNs_) / 1.0e6 /
                    static_cast<double>(nativeFrameCount_);
            const double averagePreviewCopyMs =
                    static_cast<double>(nativePreviewCopyNs_) / 1.0e6 /
                    static_cast<double>(nativeFrameCount_);
            const uint32_t resultDeliveryCount =
                    nativeResultDeliveryCount_.exchange(0, std::memory_order_relaxed);
            const int64_t resultDeliveryNs =
                    nativeResultDeliveryNs_.exchange(0, std::memory_order_relaxed);
            const uint32_t shutterDeliveryCount =
                    nativeShutterDeliveryCount_.exchange(0, std::memory_order_relaxed);
            const int64_t shutterDeliveryNs =
                    nativeShutterDeliveryNs_.exchange(0, std::memory_order_relaxed);
            const double averageResultMs = resultDeliveryCount == 0
                    ? 0.0
                    : static_cast<double>(resultDeliveryNs) / 1.0e6 /
                            static_cast<double>(resultDeliveryCount);
            const double averageShutterMs = shutterDeliveryCount == 0
                    ? 0.0
                    : static_cast<double>(shutterDeliveryNs) / 1.0e6 /
                            static_cast<double>(shutterDeliveryCount);
            if (videoBlitCount_ != 0) {
                const double averageFenceMs =
                        static_cast<double>(videoFenceWaitNs_) / 1.0e6 /
                        static_cast<double>(videoBlitCount_);
                const double averageBlitMs =
                        static_cast<double>(videoBlitProcessingNs_) / 1.0e6 /
                        static_cast<double>(videoBlitCount_);
                ALOGI("Native capture cadence %.1f fps; capture thread %.1f ms/frame "
                      "(request %.1f, preview transfer %.1f, async shutter %.1f, "
                      "result %.1f ms); preview FIMC3/CPU %u/%u; encoder fence "
                      "%.1f ms, persistent FIMC1 %.1f ms (%u frames)",
                      fps, averageProcessingMs, averageDequeueMs,
                      averagePreviewCopyMs, averageShutterMs, averageResultMs,
                      previewDmaCount_, previewCpuCopyCount_, averageFenceMs,
                      averageBlitMs, videoBlitCount_);
            } else {
                ALOGI("Native capture cadence %.1f fps; capture thread %.1f ms/frame "
                      "(request %.1f, preview transfer %.1f, async shutter %.1f, "
                      "result %.1f ms)",
                      fps, averageProcessingMs, averageDequeueMs,
                      averagePreviewCopyMs, averageShutterMs, averageResultMs);
            }
            nativeFrameWindowStartNs_ = 0;
            nativeFrameCount_ = 0;
            nativeFrameProcessingNs_ = 0;
            nativeDequeueWaitNs_ = 0;
            nativePreviewCopyNs_ = 0;
            previewDmaCount_ = 0;
            previewCpuCopyCount_ = 0;
            videoBlitCount_ = 0;
            videoFenceWaitNs_ = 0;
            videoBlitProcessingNs_ = 0;
        }
        return completeRc;
    }

    int copyNativePreviewToBuffer(const void* source, size_t sourceSize,
                                  uint32_t sourceYAddr, uint32_t sourceCbcrAddr,
                                  buffer_handle_t* target) {
        if (source == nullptr || target == nullptr || *target == nullptr ||
            gralloc_ == nullptr) {
            return -EINVAL;
        }
        const size_t requiredSize = static_cast<size_t>(nativePreviewWidth_) *
                static_cast<size_t>(nativePreviewHeight_) * 3U / 2U;
        if (sourceSize < requiredSize) {
            return -EINVAL;
        }

        private_handle_t* destination = private_handle_t::dynamicCast(*target);
        const bool letterbox16By9 = nativePreviewWidth_ == previewWidth_ &&
                nativePreviewHeight_ * 4 == previewHeight_ * 3;
        const bool contiguousNv21 = destination != nullptr &&
                (destination->flags & private_handle_t::PRIV_FLAGS_USES_ION) &&
                (destination->flags & private_handle_t::PRIV_FLAGS_CONTIGUOUS_ION) &&
                destination->paddr != 0 &&
                destination->format == HAL_PIXEL_FORMAT_YCrCb_420_SP &&
                sourceYAddr != 0 && sourceCbcrAddr != 0;

        if (contiguousNv21 && !previewBlitterUnavailable_) {
            if (letterbox16By9 &&
                std::find(previewLetterboxBuffers_.begin(),
                          previewLetterboxBuffers_.end(), *target) ==
                        previewLetterboxBuffers_.end()) {
                void* destinationData = nullptr;
                const int clearRc = gralloc_->lock(
                        gralloc_, *target, GRALLOC_USAGE_SW_WRITE_OFTEN,
                        0, 0, previewWidth_, previewHeight_, &destinationData);
                if (clearRc == 0 && destinationData != nullptr) {
                    const size_t lumaSize = static_cast<size_t>(previewWidth_) *
                            static_cast<size_t>(previewHeight_);
                    auto* destinationBytes = static_cast<uint8_t*>(destinationData);
                    memset(destinationBytes, 0, lumaSize);
                    memset(destinationBytes + destination->uoffset, 128, lumaSize / 2U);
                    gralloc_->unlock(gralloc_, *target);
                    previewLetterboxBuffers_.push_back(*target);
                } else {
                    ALOGW("Could not initialize preview letterbox buffer: %d", clearRc);
                }
            }
            if (previewBlitter_.flagCreate() &&
                (previewBlitterSourceWidth_ != nativePreviewWidth_ ||
                 previewBlitterSourceHeight_ != nativePreviewHeight_ ||
                 previewBlitterWidth_ != previewWidth_ ||
                 previewBlitterHeight_ != previewHeight_ ||
                 previewBlitterFormat_ != destination->format)) {
                resetPreviewBlitter();
            }
            if (!previewBlitter_.flagCreate()) {
                if (!previewBlitter_.create(SecFimc::FIMC_DEV3,
                                            FIMC_OVLY_NONE_SINGLE_BUF, 1)) {
                    ALOGW("FIMC3 preview DMA is unavailable; using CPU copy");
                    previewBlitterUnavailable_ = true;
                } else {
                    unsigned int sourceCropWidth =
                            static_cast<unsigned int>(nativePreviewWidth_);
                    unsigned int sourceCropHeight =
                            static_cast<unsigned int>(nativePreviewHeight_);
                    unsigned int destinationCropWidth = sourceCropWidth;
                    unsigned int destinationCropHeight = sourceCropHeight;
                    const unsigned int destinationY = letterbox16By9
                            ? static_cast<unsigned int>(
                                      (previewHeight_ - nativePreviewHeight_) / 2)
                            : 0U;
                    if (!previewBlitter_.setSrcParams(
                                nativePreviewWidth_, nativePreviewHeight_, 0, 0,
                                &sourceCropWidth, &sourceCropHeight,
                                HAL_PIXEL_FORMAT_YCrCb_420_SP) ||
                        !previewBlitter_.setDstParams(
                                previewWidth_, previewHeight_, 0, destinationY,
                                &destinationCropWidth, &destinationCropHeight,
                                HAL_PIXEL_FORMAT_YCrCb_420_SP)) {
                        ALOGW("Could not configure FIMC3 NV21 preview DMA; using CPU copy");
                        resetPreviewBlitter();
                        previewBlitterUnavailable_ = true;
                    } else {
                        previewBlitterSourceWidth_ = nativePreviewWidth_;
                        previewBlitterSourceHeight_ = nativePreviewHeight_;
                        previewBlitterWidth_ = previewWidth_;
                        previewBlitterHeight_ = previewHeight_;
                        previewBlitterFormat_ = destination->format;
                        ALOGI("Native persistent FIMC3 NV21 preview DMA path enabled");
                    }
                }
            }

            if (previewBlitter_.flagCreate()) {
                int dequeuedIndex = -1;
                const bool blitOk =
                        previewBlitter_.setSrcPhyAddr(
                                sourceYAddr, sourceCbcrAddr, 0,
                                HAL_PIXEL_FORMAT_YCrCb_420_SP) &&
                        previewBlitter_.setDstPhyAddr(
                                static_cast<unsigned int>(destination->paddr),
                                static_cast<unsigned int>(destination->paddr) +
                                        destination->uoffset, 0) &&
                        previewBlitter_.streamOn() &&
                        previewBlitter_.queueBuffer(0) &&
                        previewBlitter_.dequeueBuffer(&dequeuedIndex) &&
                        dequeuedIndex == 0;
                if (blitOk) {
                    ++previewDmaCount_;
                    return 0;
                }
                ALOGW("FIMC3 preview DMA failed; disabling it for this session");
                resetPreviewBlitter();
                previewBlitterUnavailable_ = true;
            }
        }

        // Internal drain/scratch buffers are intentionally UMP-only, and a
        // kernel without a usable FIMC3 output node must remain functional.
        // These cases keep the original CPU path; ordinary Camera3 preview
        // buffers are contiguous ION and take the DMA path above.
        void* destinationData = nullptr;
        const int lockRc = gralloc_->lock(gralloc_, *target,
                                          GRALLOC_USAGE_SW_WRITE_OFTEN,
                                          0, 0, previewWidth_, previewHeight_,
                                          &destinationData);
        if (lockRc != 0 || destinationData == nullptr) {
            return lockRc != 0 ? lockRc : -EIO;
        }
        if (letterbox16By9) {
            auto* destinationBytes = static_cast<uint8_t*>(destinationData);
            const auto* sourceBytes = static_cast<const uint8_t*>(source);
            const size_t destinationY =
                    static_cast<size_t>((previewHeight_ - nativePreviewHeight_) / 2);
            const size_t nativeLumaSize = static_cast<size_t>(nativePreviewWidth_) *
                    static_cast<size_t>(nativePreviewHeight_);
            const size_t previewLumaSize = static_cast<size_t>(previewWidth_) *
                    static_cast<size_t>(previewHeight_);
            memset(destinationBytes, 0, previewLumaSize);
            memset(destinationBytes + previewLumaSize, 128, previewLumaSize / 2U);
            for (int row = 0; row < nativePreviewHeight_; ++row) {
                memcpy(destinationBytes +
                               (destinationY + static_cast<size_t>(row)) * previewWidth_,
                       sourceBytes + static_cast<size_t>(row) * nativePreviewWidth_,
                       static_cast<size_t>(nativePreviewWidth_));
            }
            for (int row = 0; row < nativePreviewHeight_ / 2; ++row) {
                memcpy(destinationBytes + previewLumaSize +
                               (destinationY / 2U + static_cast<size_t>(row)) * previewWidth_,
                       sourceBytes + nativeLumaSize +
                               static_cast<size_t>(row) * nativePreviewWidth_,
                       static_cast<size_t>(nativePreviewWidth_));
            }
        } else {
            memcpy(destinationData, source, requiredSize);
        }
        const int unlockRc = gralloc_->unlock(gralloc_, *target);
        if (unlockRc != 0) {
            return unlockRc;
        }
        ++previewCpuCopyCount_;
        return 0;
    }

    int copyNativeNv21ToBuffer(const std::shared_ptr<PendingFrame>& frame,
                               const void* source, size_t sourceSize,
                               uint32_t sourceYAddr, uint32_t sourceCbcrAddr,
                               int sourceWidth, int sourceHeight,
                               camera3_stream_buffer_t* target) {
        (void)source;
        if (frame == nullptr || target == nullptr ||
            target->stream == nullptr || target->buffer == nullptr || gralloc_ == nullptr) {
            return -EINVAL;
        }
        const int targetWidth = static_cast<int>(target->stream->width);
        const int targetHeight = static_cast<int>(target->stream->height);
        const bool fullFov720 = sourceWidth == 1072 && sourceHeight == 800 &&
                targetWidth == 1280 && targetHeight == 720;
        if (!fullFov720 &&
            (targetWidth != sourceWidth || targetHeight != sourceHeight)) {
            ALOGE("Native stream size mismatch for frame %u: %dx%d -> %ux%u",
                  frame->frameNumber, sourceWidth, sourceHeight,
                  target->stream->width, target->stream->height);
            return -EINVAL;
        }
        const size_t requiredSize = static_cast<size_t>(sourceWidth) *
                static_cast<size_t>(sourceHeight) * 3U / 2U;
        if (sourceSize < requiredSize || sourceYAddr == 0 || sourceCbcrAddr == 0) {
            ALOGE("Native stream source is invalid for frame %u: size=%zu/%zu y=%#x cbcr=%#x",
                  frame->frameNumber, sourceSize, requiredSize,
                  sourceYAddr, sourceCbcrAddr);
            return -EINVAL;
        }
        const int64_t fenceStartNs = systemTime(SYSTEM_TIME_MONOTONIC);
        if (waitAndCloseAcquireFence(target) != 0) {
            ALOGE("Timed out waiting for native stream acquire fence for frame %u: %s",
                  frame->frameNumber, strerror(errno));
            return -errno;
        }
        const int64_t fenceWaitNs =
                systemTime(SYSTEM_TIME_MONOTONIC) - fenceStartNs;

        private_handle_t* destination = private_handle_t::dynamicCast(*target->buffer);
        if (destination == nullptr ||
            !(destination->flags & private_handle_t::PRIV_FLAGS_USES_ION) ||
            !(destination->flags & private_handle_t::PRIV_FLAGS_CONTIGUOUS_ION) ||
            destination->paddr == 0) {
            ALOGE("Encoder buffer for frame %u is not contiguous ION memory",
                  frame->frameNumber);
            return -EINVAL;
        }

        const int destinationFormat = target->stream->format;
        if (destinationFormat != HAL_PIXEL_FORMAT_YCbCr_420_SP &&
            destinationFormat != HAL_PIXEL_FORMAT_YCrCb_420_SP) {
            ALOGE("Unsupported native video format 0x%x for frame %u",
                  destinationFormat, frame->frameNumber);
            return -EINVAL;
        }

        const size_t packedChromaOffset = static_cast<size_t>(targetWidth) *
                static_cast<size_t>(targetHeight);
        if (destination->uoffset < packedChromaOffset ||
            (destination->uoffset & 0x7ffU) != 0) {
            ALOGE("Encoder chroma plane for frame %u is not MFC-aligned: "
                  "offset=%u packed=%zu",
                  frame->frameNumber, destination->uoffset, packedChromaOffset);
            return -EINVAL;
        }
        /*
         * FIMC's ordinary NV12 single-buffer mode always places C immediately
         * after width*height.  That address is not representable by MFC's
         * 2 KiB plane registers at 1920x1080.  Select the driver's native
         * NV12M layout whenever gralloc supplied an aligned plane gap; FIMC1
         * and MFC then agree on the exact physical chroma address with no CPU
         * staging copy.
         */
        const int fimcDestinationFormat =
                destinationFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP &&
                static_cast<size_t>(destination->uoffset) != packedChromaOffset
                ? V4L2_PIX_FMT_NV12M
                : destinationFormat;

        // FIMC writes only the centered 960x720 picture. Initialize each
        // encoder buffer once so the untouched Y/UV margins remain black.
        if (fullFov720 &&
            std::find(videoPillarboxBuffers_.begin(), videoPillarboxBuffers_.end(),
                      *target->buffer) == videoPillarboxBuffers_.end()) {
            void* destinationData = nullptr;
            const int clearRc = gralloc_->lock(
                    gralloc_, *target->buffer, GRALLOC_USAGE_SW_WRITE_OFTEN,
                    0, 0, targetWidth, targetHeight, &destinationData);
            if (clearRc != 0 || destinationData == nullptr) {
                ALOGE("Could not initialize 720p encoder pillarbox for frame %u: %d",
                      frame->frameNumber, clearRc);
                return clearRc != 0 ? clearRc : -EIO;
            }
            memset(destinationData, 0, packedChromaOffset);
            memset(static_cast<uint8_t*>(destinationData) + destination->uoffset,
                   128, packedChromaOffset / 2U);
            const int unlockRc = gralloc_->unlock(gralloc_, *target->buffer);
            if (unlockRc != 0) {
                ALOGE("Could not flush 720p encoder pillarbox for frame %u: %d",
                      frame->frameNumber, unlockRc);
                return unlockRc;
            }
            videoPillarboxBuffers_.push_back(*target->buffer);
        }

        if (videoBlitter_.flagCreate() &&
            (videoBlitterWidth_ != sourceWidth ||
             videoBlitterHeight_ != sourceHeight ||
             videoBlitterDestinationWidth_ != targetWidth ||
             videoBlitterDestinationHeight_ != targetHeight ||
             videoBlitterFormat_ != fimcDestinationFormat)) {
            resetVideoBlitter();
        }
        if (!videoBlitter_.flagCreate()) {
            if (!videoBlitter_.create(SecFimc::FIMC_DEV1,
                                      FIMC_OVLY_NONE_SINGLE_BUF, 1)) {
                ALOGE("Could not open FIMC1 video blitter");
                return -ENODEV;
            }
            unsigned int cropWidth = static_cast<unsigned int>(sourceWidth);
            unsigned int cropHeight = static_cast<unsigned int>(sourceHeight);
            unsigned int destinationCropWidth = fullFov720 ? 960U : cropWidth;
            unsigned int destinationCropHeight = fullFov720 ? 720U : cropHeight;
            const unsigned int destinationX = fullFov720 ? 160U : 0U;
            if (!videoBlitter_.setSrcParams(sourceWidth, sourceHeight, 0, 0,
                                            &cropWidth, &cropHeight,
                                            HAL_PIXEL_FORMAT_YCrCb_420_SP) ||
                !videoBlitter_.setDstParams(targetWidth, targetHeight,
                                            destinationX, 0,
                                            &destinationCropWidth,
                                            &destinationCropHeight,
                                            fimcDestinationFormat)) {
                ALOGE("Could not configure FIMC1 NV21-to-%s video blitter",
                      destinationFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP
                              ? "NV12" : "NV21");
                resetVideoBlitter();
                return -EINVAL;
            }
            videoBlitterWidth_ = sourceWidth;
            videoBlitterHeight_ = sourceHeight;
            videoBlitterDestinationWidth_ = targetWidth;
            videoBlitterDestinationHeight_ = targetHeight;
            videoBlitterFormat_ = fimcDestinationFormat;
            ALOGI("Native persistent FIMC1 NV21-to-%s DMA path enabled for camera video "
                  "(%dx%d -> %dx%d, chroma offset=%u%s)",
                  destinationFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP
                          ? (fimcDestinationFormat == V4L2_PIX_FMT_NV12M
                                  ? "NV12M" : "NV12")
                          : "NV21",
                  sourceWidth, sourceHeight, targetWidth, targetHeight,
                  destination->uoffset,
                  fullFov720 ? ", 960x720 centered pillarbox" : "");
        }

        const int64_t blitStartNs = systemTime(SYSTEM_TIME_MONOTONIC);
        int dequeuedIndex = -1;
        const bool blitOk =
                videoBlitter_.setSrcPhyAddr(sourceYAddr, sourceCbcrAddr, 0,
                                            HAL_PIXEL_FORMAT_YCrCb_420_SP) &&
                videoBlitter_.setDstPhyAddr(
                        static_cast<unsigned int>(destination->paddr),
                        static_cast<unsigned int>(destination->paddr) +
                                destination->uoffset, 0) &&
                videoBlitter_.streamOn() &&
                videoBlitter_.queueBuffer(0) &&
                videoBlitter_.dequeueBuffer(&dequeuedIndex) &&
                dequeuedIndex == 0;
        const int64_t blitProcessingNs =
                systemTime(SYSTEM_TIME_MONOTONIC) - blitStartNs;
        ++videoBlitCount_;
        videoFenceWaitNs_ += fenceWaitNs;
        videoBlitProcessingNs_ += blitProcessingNs;
        if (!blitOk) {
            ALOGE("FIMC1 video blit failed for frame %u", frame->frameNumber);
            resetVideoBlitter();
            return -EIO;
        }
        return 0;
    }

    int completeNativeFrame(buffer_handle_t* buffer, bool error,
                            const void* recordingData, size_t recordingSize,
                            uint32_t recordingYAddr, uint32_t recordingCbcrAddr,
                            bool videoBlitPrepared, int preparedVideoRc) {
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
                // flush() may have returned this frame while stop_preview()
                // waited for the V4L2 callback to leave its critical section.
                // Treat that stale callback as a cleanly drained frame.
                ALOGV("Ignoring stale native frame buffer %p during transition", buffer);
                return 0;
            }
            frame = it->second;
            inFlightPreview_.erase(it);
            scratch = scratchPreviewBufferForHandle(buffer);
        }
        if (frame == nullptr) {
            return 0;
        }

        bool resultQueued = false;
        if (error || frame->generation != sessionGeneration_.load()) {
            failFrame(frame);
        } else {
            camera3_stream_buffer_t* previewSource = scratch != nullptr
                    ? &scratch->buffer
                    : (frame->previewBuffer.has_value() ? &*frame->previewBuffer : nullptr);
            const bool metadataReturned = frame->metadataReturned;

            int videoRc = 0;
            int analysisRc = 0;
            if (frame->videoBuffer.has_value()) {
                videoRc = videoBlitPrepared
                        ? preparedVideoRc
                        : copyNativeNv21ToBuffer(
                                  frame, recordingData, recordingSize,
                                  recordingYAddr, recordingCbcrAddr,
                                  nativeVideoWidth_, nativeVideoHeight_,
                                  &*frame->videoBuffer);
            }
            if (frame->analysisBuffer.has_value()) {
                analysisRc = copyPreviewToAnalysis(frame, previewSource);
            }

            /*
             * A Camera3 capture result may contain all buffers for a frame.
             * Returning preview, video and metadata in separate callbacks
             * forced multiple synchronous provider/BufferQueue round trips
             * and held both FIMC capture nodes for another frame interval.
             */
            std::array<camera3_stream_buffer_t, 3> completedBuffers{};
            uint32_t completedBufferCount = 0;
            if (frame->previewBuffer.has_value()) {
                completedBuffers[completedBufferCount++] = *frame->previewBuffer;
            }
            if (frame->videoBuffer.has_value()) {
                camera3_stream_buffer_t video = *frame->videoBuffer;
                if (videoRc != 0) {
                    prepareErrorBuffer(&video);
                }
                completedBuffers[completedBufferCount++] = video;
            }
            if (frame->analysisBuffer.has_value()) {
                camera3_stream_buffer_t analysis = *frame->analysisBuffer;
                if (analysisRc != 0) {
                    prepareErrorBuffer(&analysis);
                }
                completedBuffers[completedBufferCount++] = analysis;
            }
            if (completedBufferCount != 0 || !metadataReturned) {
                NativeResultJob job;
                job.frame = frame;
                job.buffers = completedBuffers;
                job.bufferCount = completedBufferCount;
                job.sendShutter = true;
                job.includeMetadata = !metadataReturned;
                queueNativeResult(std::move(job));
                resultQueued = true;
            }
        }

        if (scratch != nullptr) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                scratch->inUse = false;
            }
            previewCv_.notify_all();
        }

        // Completed buffers remain owned by the FIFO result worker until it
        // has called process_capture_result(). A whole-frame failure remains
        // synchronous because no successful shutter/result is produced.
        if (resultQueued) {
            return 0;
        }
        if (error || frame->generation != sessionGeneration_.load()) {
            return 0;
        }
        if (!frame->previewBuffer.has_value() && !frame->videoBuffer.has_value() &&
            !frame->analysisBuffer.has_value() && frame->jpegBuffer.has_value()) {
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
                ALOGW("V4L2 backend returned an unknown preview buffer %p", buffer);
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
            bool sourceReturned = false;

            if (source != nullptr && frame->previewBuffer.has_value() &&
                frame->videoBuffer.has_value()) {
                if (source == &*frame->videoBuffer) {
                    derived = &*frame->previewBuffer;
                } else if (source == &*frame->previewBuffer) {
                    derived = &*frame->videoBuffer;
                }
                if (derived == nullptr) {
                    derivedRc = -EINVAL;
                } else {
                    // For the CameraX 720p recording layout, make a private
                    // CPU copy for the scaler and release the encoder buffer
                    // immediately. The old synchronous crop/downscale held
                    // one of only three V4L2 buffers for tens of milliseconds,
                    // starving the sensor queue and reducing video to ~11 fps.
                    const bool canUsePreviewCache =
                            source == &*frame->videoBuffer &&
                            derived == &*frame->previewBuffer &&
                            !frame->analysisBuffer.has_value() &&
                            schedulePreviewCacheUpdate(*source, frame->generation);
                    if (canUsePreviewCache) {
                        sendResult(frame, *source, !metadataReturned);
                        metadataReturned = true;
                        sourceReturned = true;
                        derivedRc = copyPreviewCacheToBuffer(frame, *derived);
                    } else {
                        // The first frame seeds the cache synchronously. This
                        // also remains the safe path for other stream layouts
                        // and ImageAnalysis requests that still read source.
                        derivedRc = copyNv21Buffer(frame, *source, *derived);
                    }
                }
            }
            if (frame->analysisBuffer.has_value()) {
                analysisRc = copyPreviewToAnalysis(frame, source);
            }

            // Do not return the direct V4L2 source buffer until every derived
            // output has finished reading it. Once process_capture_result() is
            // called, the framework may immediately recycle that buffer.
            if (source != nullptr && !sourceReturned) {
                sendResult(frame, *source, !metadataReturned);
                metadataReturned = true;
            } else {
                // A configured non-scratch frame must always have a direct
                // preview source. Return every affected output explicitly so
                // CameraService cannot wait forever for a missing buffer.
                if (source == nullptr && frame->previewBuffer.has_value()) {
                    sendBufferError(frame->frameNumber, *frame->previewBuffer);
                }
                if (source == nullptr && frame->videoBuffer.has_value()) {
                    sendBufferError(frame->frameNumber, *frame->videoBuffer);
                }
            }
            if (derived != nullptr) {
                if (derivedRc == 0) {
                    sendResult(frame, *derived, !metadataReturned);
                    metadataReturned = true;
                } else {
                    sendBufferError(frame->frameNumber, *derived);
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

    static void backendNotifyCallback(int32_t messageType, int32_t ext1, int32_t,
                                     void* user) {
        auto* self = static_cast<NativeCamera3Device*>(user);
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

    static void backendDataCallback(int32_t messageType, const camera_memory_t* data,
                                   unsigned int index, camera_frame_metadata_t*, void* user) {
        auto* self = static_cast<NativeCamera3Device*>(user);
        if (self != nullptr && messageType == CAMERA_MSG_COMPRESSED_IMAGE) {
            self->handleJpeg(data, index);
        }
    }

    static void backendTimestampCallback(int64_t, int32_t, const camera_memory_t*,
                                        unsigned int, void*) {}

    void handleJpeg(const camera_memory_t* memory, unsigned int index) {
        std::shared_ptr<PendingFrame> frame;
        bool callbackActive = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            // The picture thread is detached. flush() must wait until
            // every callback has stopped touching this HAL instance, including a stale
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
    exynos_camera* backend_ = nullptr;
    const gralloc_module_t* gralloc_ = nullptr;
    alloc_device_t* grallocAlloc_ = nullptr;
    SecFimc videoBlitter_;
    int videoBlitterWidth_ = 0;
    int videoBlitterHeight_ = 0;
    int videoBlitterDestinationWidth_ = 0;
    int videoBlitterDestinationHeight_ = 0;
    int videoBlitterFormat_ = 0;
    std::vector<buffer_handle_t> videoPillarboxBuffers_;
    SecFimc previewBlitter_;
    int previewBlitterWidth_ = 0;
    int previewBlitterHeight_ = 0;
    int previewBlitterSourceWidth_ = 0;
    int previewBlitterSourceHeight_ = 0;
    int previewBlitterFormat_ = 0;
    bool previewBlitterUnavailable_ = false;
    std::vector<buffer_handle_t> previewLetterboxBuffers_;
    const camera3_callback_ops_t* callbacks_ = nullptr;
    // Camera3 permits result callbacks from different HAL threads, but never
    // more than one process_capture_result() call at a time.
    std::mutex callbackMutex_;
    std::mutex stateMutex_;
    std::mutex backendOpsMutex_;
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

    std::mutex resultMutex_;
    std::condition_variable resultCv_;
    std::condition_variable resultIdleCv_;
    std::deque<NativeResultJob> resultQueue_;
    std::thread resultWorker_;
    bool resultWorkerExit_ = false;
    bool resultWorkerActive_ = false;

    std::mutex videoBlitMutex_;
    std::condition_variable videoBlitCv_;
    std::condition_variable videoBlitDoneCv_;
    std::thread videoBlitWorker_;
    NativeVideoBlitJob videoBlitJob_{};
    bool videoBlitExit_ = false;
    bool videoBlitPending_ = false;
    bool videoBlitActive_ = false;
    uint64_t videoBlitSubmittedSequence_ = 0;
    uint64_t videoBlitCompletedSequence_ = 0;
    int videoBlitResult_ = 0;

    std::mutex previewScalerMutex_;
    std::condition_variable previewScalerCv_;
    std::thread previewScalerWorker_;
    std::vector<uint8_t> previewScaleInput_;
    std::vector<uint8_t> previewScaleOutput_;
    std::vector<uint8_t> previewCache_;
    bool previewScalerExit_ = false;
    bool previewScalePending_ = false;
    bool previewScaleActive_ = false;
    bool previewCacheValid_ = false;
    uint32_t previewScalerGeneration_ = 0;
    uint32_t previewScaleJobGeneration_ = 0;
    uint32_t previewCacheGeneration_ = 0;
    int64_t previewScaleLastQueuedNs_ = 0;
    int64_t nativeFrameWindowStartNs_ = 0;
    uint32_t nativeFrameCount_ = 0;
    int64_t nativeFrameProcessingNs_ = 0;
    int64_t nativeDequeueWaitNs_ = 0;
    int64_t nativePreviewCopyNs_ = 0;
    std::atomic<int64_t> nativeResultDeliveryNs_{0};
    std::atomic<uint32_t> nativeResultDeliveryCount_{0};
    std::atomic<int64_t> nativeShutterDeliveryNs_{0};
    std::atomic<uint32_t> nativeShutterDeliveryCount_{0};
    uint32_t previewDmaCount_ = 0;
    uint32_t previewCpuCopyCount_ = 0;
    uint32_t videoBlitCount_ = 0;
    int64_t videoFenceWaitNs_ = 0;
    int64_t videoBlitProcessingNs_ = 0;

    camera3_stream_t* previewStream_ = nullptr;
    camera3_stream_t* videoStream_ = nullptr;
    camera3_stream_t* analysisStream_ = nullptr;
    camera3_stream_t* sourceStream_ = nullptr;
    camera3_stream_t* jpegStream_ = nullptr;
    int previewWidth_ = 0;
    int previewHeight_ = 0;
    int nativePreviewWidth_ = 0;
    int nativePreviewHeight_ = 0;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    int nativeVideoWidth_ = 0;
    int nativeVideoHeight_ = 0;
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
exynos_camera* gTorchBackend = nullptr;
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

int setBackendTorchParameter(exynos_camera* backend, bool enabled) {
    if (backend == nullptr) {
        return -ENODEV;
    }

    char* oldParameters = exynos_camera_backend_get_parameters(backend);
    ParameterMap parameters(oldParameters);
    if (oldParameters != nullptr) {
        exynos_camera_backend_free_parameters(oldParameters);
    }

    parameters.set("flash-mode", enabled ? "torch" : "off");
    const std::string flattened = parameters.flatten();
    return exynos_camera_backend_set_parameters(backend, flattened.c_str());
}

void releaseTorchForCameraOpen() {
    std::lock_guard<std::mutex> lock(gTorchMutex);
    if (gTorchBackend == nullptr) {
        return;
    }
    setBackendTorchParameter(gTorchBackend, false);
    exynos_camera_backend_close(gTorchBackend);
    gTorchBackend = nullptr;
}

int getNumberOfCameras() {
    return std::clamp(exynos_camera_backend_get_number_of_cameras(), 0, kMaxCameras);
}

int getCameraInfo(int cameraId, camera_info* info) {
    if (info == nullptr || cameraId < 0 || cameraId >= getNumberOfCameras()) {
        return -EINVAL;
    }
    int facing = 0;
    int orientation = 0;
    int rc = exynos_camera_backend_get_camera_info(cameraId, &facing, &orientation);
    if (rc != 0) {
        return rc;
    }
    memset(info, 0, sizeof(*info));
    info->facing = facing;
    info->orientation = orientation;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_2;
    info->static_camera_characteristics = getStaticMetadata(
            cameraId, facing, orientation);
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
        if (gTorchBackend != nullptr) {
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_ON);
            return 0;
        }

        exynos_camera* backend = nullptr;
        int rc = exynos_camera_backend_open(0, &backend);
        if (rc != 0 || backend == nullptr) {
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
            return rc != 0 ? rc : -ENODEV;
        }

        rc = setBackendTorchParameter(backend, true);
        if (rc != 0) {
            exynos_camera_backend_close(backend);
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
            return rc;
        }

        gTorchBackend = backend;
        notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_ON);
        return 0;
    }

    if (gTorchBackend != nullptr) {
        const int rc = setBackendTorchParameter(gTorchBackend, false);
        exynos_camera_backend_close(gTorchBackend);
        gTorchBackend = nullptr;
        notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
        return rc;
    }

    notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
    return 0;
}

int initModule() {
    return getNumberOfCameras() > 0 ? 0 : -ENODEV;
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

    auto camera = std::make_unique<NativeCamera3Device>(static_cast<int>(id));
    int rc = camera->openBackend();
    if (rc != 0) {
        if (id == 0) {
            notifyTorchStatus(TORCH_MODE_STATUS_AVAILABLE_OFF);
        }
        return rc;
    }
    camera->device()->common.module = const_cast<hw_module_t*>(module);
    NativeCamera3Device* rawCamera = camera.release();
    *device = &rawCamera->device()->common;
    return 0;
}

}  // namespace
}  // namespace camera3

extern "C" {

__attribute__((visibility("default"))) camera_module_t HAL_MODULE_INFO_SYM;

__attribute__((constructor)) static void initializeCameraModule() {
    using namespace camera3;
    memset(&HAL_MODULE_INFO_SYM, 0, sizeof(HAL_MODULE_INFO_SYM));
    memset(&gModuleMethods, 0, sizeof(gModuleMethods));
    gModuleMethods.open = openCamera;

    HAL_MODULE_INFO_SYM.common.tag = HARDWARE_MODULE_TAG;
    HAL_MODULE_INFO_SYM.common.module_api_version = CAMERA_MODULE_API_VERSION_2_4;
    HAL_MODULE_INFO_SYM.common.hal_api_version = HARDWARE_HAL_API_VERSION;
    HAL_MODULE_INFO_SYM.common.id = CAMERA_HARDWARE_MODULE_ID;
    HAL_MODULE_INFO_SYM.common.name = "Camera3 Exynos V4L2 HAL";
    HAL_MODULE_INFO_SYM.common.author = "ZhafKnight";
    HAL_MODULE_INFO_SYM.common.methods = &gModuleMethods;
    HAL_MODULE_INFO_SYM.get_number_of_cameras = getNumberOfCameras;
    HAL_MODULE_INFO_SYM.get_camera_info = getCameraInfo;
    HAL_MODULE_INFO_SYM.set_callbacks = setCallbacks;
    HAL_MODULE_INFO_SYM.set_torch_mode = setTorchMode;
    HAL_MODULE_INFO_SYM.init = initModule;
}

}  // extern "C"
