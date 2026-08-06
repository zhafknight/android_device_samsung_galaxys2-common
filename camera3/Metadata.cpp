#define LOG_TAG "N7000Camera3Metadata"

#include "Metadata.h"

#include <hardware/camera3.h>
#include <log/log.h>
#include <system/graphics.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

namespace n7000::camera3 {
namespace {

constexpr CameraDescriptor kDescriptors[] = {
        {0, CAMERA_FACING_BACK, 90, 3264, 2448, 12 * 1024 * 1024, 4.03f, false},
        {1, CAMERA_FACING_FRONT, 270, 1600, 1200, 4 * 1024 * 1024, 2.73f, true},
};

std::mutex gMetadataMutex;
std::array<camera_metadata_t*, 2> gStaticMetadata = {nullptr, nullptr};

template <typename T>
bool add(camera_metadata_t* metadata, uint32_t tag, const T* data, size_t count) {
    const int rc = add_camera_metadata_entry(metadata, tag, data, count);
    if (rc != 0) {
        ALOGE("add_camera_metadata_entry tag=0x%x rc=%d", tag, rc);
        return false;
    }
    return true;
}

template <typename T, size_t N>
bool add(camera_metadata_t* metadata, uint32_t tag, const std::array<T, N>& data) {
    return add(metadata, tag, data.data(), data.size());
}

void appendStreamConfig(std::vector<int32_t>* configs, int32_t format, int width, int height) {
    configs->push_back(format);
    configs->push_back(width);
    configs->push_back(height);
    configs->push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
}

void appendDuration(std::vector<int64_t>* durations, int64_t format, int width, int height,
                    int64_t duration) {
    durations->push_back(format);
    durations->push_back(width);
    durations->push_back(height);
    durations->push_back(duration);
}

std::vector<std::pair<int, int>> previewSizes(int id) {
    if (id == 0) {
        return {{1280, 720}, {800, 480}, {720, 480}, {640, 480},
                {352, 288}, {320, 240}, {176, 144}};
    }
    return {{640, 480}, {352, 288}, {320, 240}, {176, 144}};
}

std::vector<std::pair<int, int>> jpegSizes(int id) {
    if (id == 0) {
        return {{3264, 2448}, {3264, 1968}, {2048, 1536}, {2048, 1232},
                {1280, 960}, {800, 480}, {640, 480}};
    }
    return {{1600, 1200}, {640, 480}};
}

camera_metadata_t* buildStaticMetadataInternal(int id, int facing, int orientation) {
    const CameraDescriptor& descriptor = getCameraDescriptor(id);
    camera_metadata_t* metadata = allocate_camera_metadata(128, 8192);
    if (metadata == nullptr) {
        return nullptr;
    }

    const uint8_t lensFacing = facing == CAMERA_FACING_FRONT
            ? ANDROID_LENS_FACING_FRONT
            : ANDROID_LENS_FACING_BACK;
    const int32_t sensorOrientation = orientation;
    const uint8_t hardwareLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY;
    const uint8_t capability = ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE;
    const uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN;
    const int32_t syncLatency = ANDROID_SYNC_MAX_LATENCY_UNKNOWN;
    const uint8_t pipelineDepth = 2;
    const int32_t partialCount = 1;
    const uint8_t flashAvailable = id == 0 ? 1 : 0;

    add(metadata, ANDROID_LENS_FACING, &lensFacing, 1);
    add(metadata, ANDROID_SENSOR_ORIENTATION, &sensorOrientation, 1);
    add(metadata, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hardwareLevel, 1);
    add(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &capability, 1);
    add(metadata, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource, 1);
    add(metadata, ANDROID_SYNC_MAX_LATENCY, &syncLatency, 1);
    add(metadata, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipelineDepth, 1);
    add(metadata, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialCount, 1);
    add(metadata, ANDROID_FLASH_INFO_AVAILABLE, &flashAvailable, 1);

    const std::array<int32_t, 4> activeArray = {0, 0, descriptor.maxWidth, descriptor.maxHeight};
    const std::array<int32_t, 2> pixelArray = {descriptor.maxWidth, descriptor.maxHeight};
    const std::array<float, 2> physicalSize = id == 0
            ? std::array<float, 2>{5.76f, 4.29f}
            : std::array<float, 2>{3.20f, 2.40f};
    const std::array<int32_t, 2> sensitivityRange = {50, 800};
    const std::array<int64_t, 2> exposureRange = {1000000LL, 100000000LL};
    const int64_t maxFrameDuration = 142857142LL;

    add(metadata, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray);
    add(metadata, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, activeArray);
    add(metadata, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArray);
    add(metadata, ANDROID_SENSOR_INFO_PHYSICAL_SIZE, physicalSize);
    add(metadata, ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, sensitivityRange);
    add(metadata, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, exposureRange);
    add(metadata, ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &maxFrameDuration, 1);

    const float aperture = id == 0 ? 2.65f : 2.8f;
    const float minFocusDistance = descriptor.fixedFocus ? 0.0f : 6.67f;
    const float hyperfocalDistance = 0.0f;
    const uint8_t opticalStabilization = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    add(metadata, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &descriptor.focalLength, 1);
    add(metadata, ANDROID_LENS_INFO_AVAILABLE_APERTURES, &aperture, 1);
    add(metadata, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &minFocusDistance, 1);
    add(metadata, ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, &hyperfocalDistance, 1);
    add(metadata, ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
        &opticalStabilization, 1);

    std::vector<int32_t> streamConfigs;
    std::vector<int64_t> minDurations;
    std::vector<int64_t> stallDurations;
    for (const auto& [width, height] : previewSizes(id)) {
        appendStreamConfig(&streamConfigs, HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, width, height);
        appendStreamConfig(&streamConfigs, HAL_PIXEL_FORMAT_YCrCb_420_SP, width, height);
        if (width <= 640 && height <= 480) {
            appendStreamConfig(&streamConfigs, HAL_PIXEL_FORMAT_YCbCr_420_888, width, height);
        }
        constexpr int64_t minFrameDuration = 33333333LL;
        appendDuration(&minDurations, HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, width, height,
                       minFrameDuration);
        appendDuration(&minDurations, HAL_PIXEL_FORMAT_YCrCb_420_SP, width, height,
                       minFrameDuration);
        if (width <= 640 && height <= 480) {
            appendDuration(&minDurations, HAL_PIXEL_FORMAT_YCbCr_420_888,
                           width, height, minFrameDuration);
        }
        appendDuration(&stallDurations, HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, width, height, 0);
        appendDuration(&stallDurations, HAL_PIXEL_FORMAT_YCrCb_420_SP, width, height, 0);
        if (width <= 640 && height <= 480) {
            appendDuration(&stallDurations, HAL_PIXEL_FORMAT_YCbCr_420_888,
                           width, height, 0);
        }
    }
    for (const auto& [width, height] : jpegSizes(id)) {
        appendStreamConfig(&streamConfigs, HAL_PIXEL_FORMAT_BLOB, width, height);
        appendDuration(&minDurations, HAL_PIXEL_FORMAT_BLOB, width, height,
                       id == 0 ? 33333333LL : 66666666LL);
        appendDuration(&stallDurations, HAL_PIXEL_FORMAT_BLOB, width, height,
                       id == 0 ? 1500000000LL : 1000000000LL);
    }
    add(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        streamConfigs.data(), streamConfigs.size());
    add(metadata, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
        minDurations.data(), minDurations.size());
    add(metadata, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
        stallDurations.data(), stallDurations.size());

    // The back camera exposes the exact 1.0x-4.0x HAL1 zoom table.
    // Camera2 crop regions are translated to the nearest legacy zoom index.
    const float maxDigitalZoom = id == 0 ? 4.0f : 1.0f;
    const uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
    add(metadata, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxDigitalZoom, 1);
    add(metadata, ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

    // The wrapper can service preview, video and ImageAnalysis together,
    // plus one stalling JPEG stream.
    const std::array<int32_t, 3> maxOutputStreams = {0, 3, 1};
    const int32_t maxInputStreams = 0;
    add(metadata, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, maxOutputStreams);
    add(metadata, ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, &maxInputStreams, 1);

    const std::array<int32_t, 2> aeCompensationRange = {-4, 4};
    const camera_metadata_rational_t aeCompensationStep = {1, 2};
    // Keep Camera2 characteristics consistent with the encoder profiles.
    // The legacy front sensor is driven at its stable 15 fps internally, but
    // the encoder profile and the original HAL3 compatibility contract expose
    // a 30 fps video target. CameraX/Aperture validates this metadata before
    // opening the front camera in video mode.
    const std::array<int32_t, 6> fpsRanges = {7, 30, 15, 30, 30, 30};
    const std::array<int32_t, 3> maxRegions = {0, 0, descriptor.fixedFocus ? 0 : 1};
    const uint8_t aeLockAvailable = 0;
    const uint8_t awbLockAvailable = 0;
    add(metadata, ANDROID_CONTROL_AE_COMPENSATION_RANGE, aeCompensationRange);
    add(metadata, ANDROID_CONTROL_AE_COMPENSATION_STEP, &aeCompensationStep, 1);
    add(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fpsRanges);
    if (id == 1) {
        ALOGI("Front camera video metadata exposes 30 fps for CameraX profile compatibility");
    }
    add(metadata, ANDROID_CONTROL_MAX_REGIONS, maxRegions);
    add(metadata, ANDROID_CONTROL_AE_LOCK_AVAILABLE, &aeLockAvailable, 1);
    add(metadata, ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &awbLockAvailable, 1);

    const std::array<uint8_t, 2> controlModesBack = {
            ANDROID_CONTROL_MODE_AUTO,
            ANDROID_CONTROL_MODE_USE_SCENE_MODE,
    };
    const std::array<uint8_t, 1> controlModesFront = {ANDROID_CONTROL_MODE_AUTO};
    const std::array<uint8_t, 3> aeModesBack = {
            ANDROID_CONTROL_AE_MODE_ON,
            ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH,
            ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH,
    };
    const std::array<uint8_t, 1> aeModesFront = {ANDROID_CONTROL_AE_MODE_ON};
    const std::array<uint8_t, 5> awbModesBack = {
            ANDROID_CONTROL_AWB_MODE_AUTO,
            ANDROID_CONTROL_AWB_MODE_INCANDESCENT,
            ANDROID_CONTROL_AWB_MODE_FLUORESCENT,
            ANDROID_CONTROL_AWB_MODE_DAYLIGHT,
            ANDROID_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT,
    };
    const std::array<uint8_t, 1> awbModesFront = {ANDROID_CONTROL_AWB_MODE_AUTO};
    const std::array<uint8_t, 4> afModesBack = {
            ANDROID_CONTROL_AF_MODE_OFF,
            ANDROID_CONTROL_AF_MODE_AUTO,
            ANDROID_CONTROL_AF_MODE_MACRO,
            ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO,
    };
    const std::array<uint8_t, 1> afModesFront = {ANDROID_CONTROL_AF_MODE_OFF};
    const std::array<uint8_t, 5> effectsBack = {
            ANDROID_CONTROL_EFFECT_MODE_OFF,
            ANDROID_CONTROL_EFFECT_MODE_MONO,
            ANDROID_CONTROL_EFFECT_MODE_NEGATIVE,
            ANDROID_CONTROL_EFFECT_MODE_SEPIA,
            ANDROID_CONTROL_EFFECT_MODE_AQUA,
    };
    const std::array<uint8_t, 1> effectsFront = {ANDROID_CONTROL_EFFECT_MODE_OFF};
    const std::array<uint8_t, 10> scenesBack = {
            ANDROID_CONTROL_SCENE_MODE_ACTION,
            ANDROID_CONTROL_SCENE_MODE_PORTRAIT,
            ANDROID_CONTROL_SCENE_MODE_LANDSCAPE,
            ANDROID_CONTROL_SCENE_MODE_NIGHT,
            ANDROID_CONTROL_SCENE_MODE_BEACH,
            ANDROID_CONTROL_SCENE_MODE_SNOW,
            ANDROID_CONTROL_SCENE_MODE_SUNSET,
            ANDROID_CONTROL_SCENE_MODE_FIREWORKS,
            ANDROID_CONTROL_SCENE_MODE_PARTY,
            ANDROID_CONTROL_SCENE_MODE_CANDLELIGHT,
    };
    const std::array<uint8_t, 1> scenesFront = {ANDROID_CONTROL_SCENE_MODE_DISABLED};

    // Camera2 requires one AE/AWB/AF override triplet for every advertised
    // scene mode, in exactly the same order as AVAILABLE_SCENE_MODES.
    std::array<uint8_t, scenesBack.size() * 3> sceneOverridesBack{};
    for (size_t i = 0; i < scenesBack.size(); ++i) {
        sceneOverridesBack[i * 3] = ANDROID_CONTROL_AE_MODE_ON;
        sceneOverridesBack[i * 3 + 1] = ANDROID_CONTROL_AWB_MODE_AUTO;
        sceneOverridesBack[i * 3 + 2] = ANDROID_CONTROL_AF_MODE_AUTO;
    }
    const std::array<uint8_t, 3> sceneOverridesFront = {
            ANDROID_CONTROL_AE_MODE_ON,
            ANDROID_CONTROL_AWB_MODE_AUTO,
            ANDROID_CONTROL_AF_MODE_OFF,
    };
    const std::array<uint8_t, 1> videoStabilization = {
            ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF};
    const std::array<uint8_t, 1> antibanding = {
            ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO,
    };
    if (id == 0) {
        add(metadata, ANDROID_CONTROL_AVAILABLE_MODES, controlModesBack);
    } else {
        add(metadata, ANDROID_CONTROL_AVAILABLE_MODES, controlModesFront);
    }
    if (id == 0) {
        add(metadata, ANDROID_CONTROL_AE_AVAILABLE_MODES, aeModesBack);
    } else {
        add(metadata, ANDROID_CONTROL_AE_AVAILABLE_MODES, aeModesFront);
    }
    if (id == 0) {
        add(metadata, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModesBack);
    } else {
        add(metadata, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModesFront);
    }
    if (descriptor.fixedFocus) {
        add(metadata, ANDROID_CONTROL_AF_AVAILABLE_MODES, afModesFront);
    } else {
        add(metadata, ANDROID_CONTROL_AF_AVAILABLE_MODES, afModesBack);
    }
    if (id == 0) {
        add(metadata, ANDROID_CONTROL_AVAILABLE_EFFECTS, effectsBack);
    } else {
        add(metadata, ANDROID_CONTROL_AVAILABLE_EFFECTS, effectsFront);
    }
    if (id == 0) {
        add(metadata, ANDROID_CONTROL_AVAILABLE_SCENE_MODES, scenesBack);
        add(metadata, ANDROID_CONTROL_SCENE_MODE_OVERRIDES, sceneOverridesBack);
    } else {
        add(metadata, ANDROID_CONTROL_AVAILABLE_SCENE_MODES, scenesFront);
        add(metadata, ANDROID_CONTROL_SCENE_MODE_OVERRIDES, sceneOverridesFront);
    }
    add(metadata, ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
        videoStabilization);
    add(metadata, ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, antibanding);

    const std::array<uint8_t, 2> edgeModes = {ANDROID_EDGE_MODE_OFF, ANDROID_EDGE_MODE_FAST};
    const std::array<uint8_t, 2> noiseModes = {
            ANDROID_NOISE_REDUCTION_MODE_OFF, ANDROID_NOISE_REDUCTION_MODE_FAST};
    const std::array<uint8_t, 1> hotPixelModes = {ANDROID_HOT_PIXEL_MODE_OFF};
    const std::array<uint8_t, 1> aberrationModes = {
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF};
    const std::array<uint8_t, 1> shadingModes = {ANDROID_SHADING_MODE_OFF};
    const std::array<uint8_t, 1> tonemapModes = {ANDROID_TONEMAP_MODE_FAST};
    const std::array<uint8_t, 1> faceModes = {ANDROID_STATISTICS_FACE_DETECT_MODE_OFF};
    const int32_t maxFaceCount = 0;
    const std::array<int32_t, 1> testPatternModes = {ANDROID_SENSOR_TEST_PATTERN_MODE_OFF};
    add(metadata, ANDROID_EDGE_AVAILABLE_EDGE_MODES, edgeModes);
    add(metadata, ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, noiseModes);
    add(metadata, ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, hotPixelModes);
    add(metadata, ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES, aberrationModes);
    add(metadata, ANDROID_SHADING_AVAILABLE_MODES, shadingModes);
    add(metadata, ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, tonemapModes);
    add(metadata, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, faceModes);
    add(metadata, ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &maxFaceCount, 1);
    add(metadata, ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, testPatternModes);

    const std::array<int32_t, 6> thumbnailsBack = {0, 0, 320, 240, 400, 240};
    const std::array<int32_t, 4> thumbnailsFront = {0, 0, 160, 120};
    if (id == 0) {
        add(metadata, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, thumbnailsBack);
    } else {
        add(metadata, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, thumbnailsFront);
    }
    add(metadata, ANDROID_JPEG_MAX_SIZE, &descriptor.maxJpegSize, 1);

    const std::vector<int32_t> requestKeys = {
            ANDROID_CONTROL_MODE,
            ANDROID_CONTROL_CAPTURE_INTENT,
            ANDROID_CONTROL_AE_MODE,
            ANDROID_CONTROL_AE_LOCK,
            ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
            ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
            ANDROID_CONTROL_AE_ANTIBANDING_MODE,
            ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
            ANDROID_CONTROL_AF_MODE,
            ANDROID_CONTROL_AF_TRIGGER,
            ANDROID_CONTROL_AWB_MODE,
            ANDROID_CONTROL_AWB_LOCK,
            ANDROID_CONTROL_EFFECT_MODE,
            ANDROID_CONTROL_SCENE_MODE,
            ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
            ANDROID_FLASH_MODE,
            ANDROID_JPEG_ORIENTATION,
            ANDROID_JPEG_QUALITY,
            ANDROID_JPEG_THUMBNAIL_QUALITY,
            ANDROID_JPEG_THUMBNAIL_SIZE,
            ANDROID_JPEG_GPS_COORDINATES,
            ANDROID_JPEG_GPS_PROCESSING_METHOD,
            ANDROID_JPEG_GPS_TIMESTAMP,
            ANDROID_SCALER_CROP_REGION,
            ANDROID_EDGE_MODE,
            ANDROID_NOISE_REDUCTION_MODE,
            ANDROID_HOT_PIXEL_MODE,
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
            ANDROID_SHADING_MODE,
            ANDROID_TONEMAP_MODE,
            ANDROID_STATISTICS_FACE_DETECT_MODE,
            ANDROID_SENSOR_TEST_PATTERN_MODE,
            ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
    };
    const std::vector<int32_t> resultKeys = {
            ANDROID_SENSOR_TIMESTAMP,
            ANDROID_REQUEST_PIPELINE_DEPTH,
            ANDROID_CONTROL_AE_STATE,
            ANDROID_CONTROL_AF_STATE,
            ANDROID_CONTROL_AWB_STATE,
            ANDROID_FLASH_STATE,
            ANDROID_LENS_STATE,
            ANDROID_SCALER_CROP_REGION,
    };
    const std::vector<int32_t> characteristicKeys = {
            ANDROID_LENS_FACING,
            ANDROID_SENSOR_ORIENTATION,
            ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
            ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
            ANDROID_SYNC_MAX_LATENCY,
            ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
            ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
            ANDROID_FLASH_INFO_AVAILABLE,
            ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
            ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
            ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
            ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
            ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
            ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
            ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
            ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
            ANDROID_LENS_INFO_AVAILABLE_APERTURES,
            ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
            ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
            ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
            ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
            ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
            ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
            ANDROID_SCALER_CROPPING_TYPE,
            ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
            ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
            ANDROID_CONTROL_AE_COMPENSATION_RANGE,
            ANDROID_CONTROL_AE_COMPENSATION_STEP,
            ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
            ANDROID_CONTROL_MAX_REGIONS,
            ANDROID_CONTROL_AE_LOCK_AVAILABLE,
            ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
            ANDROID_CONTROL_AVAILABLE_MODES,
            ANDROID_CONTROL_AE_AVAILABLE_MODES,
            ANDROID_CONTROL_AWB_AVAILABLE_MODES,
            ANDROID_CONTROL_AF_AVAILABLE_MODES,
            ANDROID_CONTROL_AVAILABLE_EFFECTS,
            ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
            ANDROID_CONTROL_SCENE_MODE_OVERRIDES,
            ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
            ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
            ANDROID_EDGE_AVAILABLE_EDGE_MODES,
            ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
            ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
            ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
            ANDROID_SHADING_AVAILABLE_MODES,
            ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES,
            ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
            ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
            ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
            ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
            ANDROID_JPEG_MAX_SIZE,
            ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
            ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
            ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
    };
    add(metadata, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
        requestKeys.data(), requestKeys.size());
    add(metadata, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
        resultKeys.data(), resultKeys.size());
    add(metadata, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
        characteristicKeys.data(), characteristicKeys.size());

    sort_camera_metadata(metadata);
    return metadata;
}

uint8_t captureIntentForTemplate(int templateType) {
    switch (templateType) {
        case CAMERA3_TEMPLATE_STILL_CAPTURE:
            return ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
        case CAMERA3_TEMPLATE_VIDEO_RECORD:
            return ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
        case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:
            return ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
        case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
            return ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;
        case CAMERA3_TEMPLATE_MANUAL:
            return ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
        case CAMERA3_TEMPLATE_PREVIEW:
        default:
            return ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
    }
}

}  // namespace

const CameraDescriptor& getCameraDescriptor(int id) {
    return kDescriptors[(id >= 0 && id < 2) ? id : 0];
}

const camera_metadata_t* getStaticMetadata(int id, int facing, int orientation) {
    if (id < 0 || id >= static_cast<int>(gStaticMetadata.size())) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gMetadataMutex);
    if (gStaticMetadata[id] == nullptr) {
        gStaticMetadata[id] = buildStaticMetadataInternal(id, facing, orientation);
    }
    return gStaticMetadata[id];
}

camera_metadata_t* buildDefaultRequest(int id, int templateType) {
    if (templateType <= 0 || templateType >= CAMERA3_TEMPLATE_COUNT) {
        return nullptr;
    }
    const CameraDescriptor& descriptor = getCameraDescriptor(id);
    camera_metadata_t* metadata = allocate_camera_metadata(40, 2048);
    if (metadata == nullptr) {
        return nullptr;
    }

    const uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    const uint8_t captureIntent = captureIntentForTemplate(templateType);
    const uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    const uint8_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
    const int32_t aeCompensation = 0;
    const std::array<int32_t, 2> fpsRange =
            templateType == CAMERA3_TEMPLATE_VIDEO_RECORD
                    ? std::array<int32_t, 2>{30, 30}
                    : std::array<int32_t, 2>{15, 30};
    const uint8_t antibanding = ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
    const uint8_t aeTrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    const uint8_t afMode = descriptor.fixedFocus
            ? ANDROID_CONTROL_AF_MODE_OFF
            : (templateType == CAMERA3_TEMPLATE_VIDEO_RECORD
                    ? ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO
                    : ANDROID_CONTROL_AF_MODE_AUTO);
    const uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    const uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    const uint8_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
    const uint8_t effect = ANDROID_CONTROL_EFFECT_MODE_OFF;
    const uint8_t scene = ANDROID_CONTROL_SCENE_MODE_DISABLED;
    const uint8_t videoStabilization = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    const uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
    const int32_t jpegOrientation = 0;
    const uint8_t jpegQuality = 90;
    const uint8_t thumbnailQuality = 90;
    const std::array<int32_t, 2> thumbnailSize = id == 0
            ? std::array<int32_t, 2>{320, 240}
            : std::array<int32_t, 2>{160, 120};
    const std::array<int32_t, 4> crop = {0, 0, descriptor.maxWidth, descriptor.maxHeight};
    const uint8_t edge = ANDROID_EDGE_MODE_FAST;
    const uint8_t noise = ANDROID_NOISE_REDUCTION_MODE_FAST;
    const uint8_t hotPixel = ANDROID_HOT_PIXEL_MODE_OFF;
    const uint8_t aberration = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF;
    const uint8_t shading = ANDROID_SHADING_MODE_OFF;
    const uint8_t tonemap = ANDROID_TONEMAP_MODE_FAST;
    const uint8_t faceDetect = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
    const int32_t testPattern = ANDROID_SENSOR_TEST_PATTERN_MODE_OFF;
    const uint8_t ois = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;

    add(metadata, ANDROID_CONTROL_MODE, &controlMode, 1);
    add(metadata, ANDROID_CONTROL_CAPTURE_INTENT, &captureIntent, 1);
    add(metadata, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
    add(metadata, ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
    add(metadata, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &aeCompensation, 1);
    add(metadata, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fpsRange);
    add(metadata, ANDROID_CONTROL_AE_ANTIBANDING_MODE, &antibanding, 1);
    add(metadata, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &aeTrigger, 1);
    add(metadata, ANDROID_CONTROL_AF_MODE, &afMode, 1);
    add(metadata, ANDROID_CONTROL_AF_TRIGGER, &afTrigger, 1);
    add(metadata, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
    add(metadata, ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);
    add(metadata, ANDROID_CONTROL_EFFECT_MODE, &effect, 1);
    add(metadata, ANDROID_CONTROL_SCENE_MODE, &scene, 1);
    add(metadata, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &videoStabilization, 1);
    add(metadata, ANDROID_FLASH_MODE, &flashMode, 1);
    add(metadata, ANDROID_JPEG_ORIENTATION, &jpegOrientation, 1);
    add(metadata, ANDROID_JPEG_QUALITY, &jpegQuality, 1);
    add(metadata, ANDROID_JPEG_THUMBNAIL_QUALITY, &thumbnailQuality, 1);
    add(metadata, ANDROID_JPEG_THUMBNAIL_SIZE, thumbnailSize);
    add(metadata, ANDROID_SCALER_CROP_REGION, crop);
    add(metadata, ANDROID_EDGE_MODE, &edge, 1);
    add(metadata, ANDROID_NOISE_REDUCTION_MODE, &noise, 1);
    add(metadata, ANDROID_HOT_PIXEL_MODE, &hotPixel, 1);
    add(metadata, ANDROID_COLOR_CORRECTION_ABERRATION_MODE, &aberration, 1);
    add(metadata, ANDROID_SHADING_MODE, &shading, 1);
    add(metadata, ANDROID_TONEMAP_MODE, &tonemap, 1);
    add(metadata, ANDROID_STATISTICS_FACE_DETECT_MODE, &faceDetect, 1);
    add(metadata, ANDROID_SENSOR_TEST_PATTERN_MODE, &testPattern, 1);
    add(metadata, ANDROID_LENS_OPTICAL_STABILIZATION_MODE, &ois, 1);
    sort_camera_metadata(metadata);
    return metadata;
}

camera_metadata_t* buildResultMetadata(int id, int64_t timestamp, uint8_t afState,
                                       const int32_t* requestedCropRegion) {
    const CameraDescriptor& descriptor = getCameraDescriptor(id);
    camera_metadata_t* metadata = allocate_camera_metadata(12, 512);
    if (metadata == nullptr) {
        return nullptr;
    }
    const uint8_t pipelineDepth = 2;
    const uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
    const uint8_t awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
    const uint8_t flashState = id == 0
            ? ANDROID_FLASH_STATE_READY
            : ANDROID_FLASH_STATE_UNAVAILABLE;
    const uint8_t lensState = ANDROID_LENS_STATE_STATIONARY;
    const std::array<int32_t, 4> fullCrop = {
            0, 0, descriptor.maxWidth, descriptor.maxHeight};
    const int32_t* crop = requestedCropRegion != nullptr
            ? requestedCropRegion
            : fullCrop.data();
    add(metadata, ANDROID_SENSOR_TIMESTAMP, &timestamp, 1);
    add(metadata, ANDROID_REQUEST_PIPELINE_DEPTH, &pipelineDepth, 1);
    add(metadata, ANDROID_CONTROL_AE_STATE, &aeState, 1);
    add(metadata, ANDROID_CONTROL_AF_STATE, &afState, 1);
    add(metadata, ANDROID_CONTROL_AWB_STATE, &awbState, 1);
    add(metadata, ANDROID_FLASH_STATE, &flashState, 1);
    add(metadata, ANDROID_LENS_STATE, &lensState, 1);
    add(metadata, ANDROID_SCALER_CROP_REGION, crop, 4);
    sort_camera_metadata(metadata);
    return metadata;
}

}  // namespace n7000::camera3
