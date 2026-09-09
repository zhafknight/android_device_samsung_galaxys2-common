#pragma once

#include <hardware/camera_common.h>
#include <system/camera_metadata.h>

#include <cstdint>

namespace camera3 {

struct CameraDescriptor {
    int id;
    int facing;
    int orientation;
    int maxWidth;
    int maxHeight;
    int maxJpegSize;
    float focalLength;
    bool fixedFocus;
};

const CameraDescriptor& getCameraDescriptor(int id);
const camera_metadata_t* getStaticMetadata(int id, int facing, int orientation);
camera_metadata_t* buildDefaultRequest(int id, int templateType);
camera_metadata_t* buildResultMetadata(int id, int64_t timestamp, uint8_t afState,
                                       const int32_t* requestedCropRegion);

}  // namespace camera3
