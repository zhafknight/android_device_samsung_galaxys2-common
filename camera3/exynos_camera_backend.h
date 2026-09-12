/*
 * Native in-process Exynos backend API for the smdk4210 Camera3 HAL.
 *
 * This deliberately exposes camera operations rather than a camera_device_t.
 * The standalone Camera1 ABI remains available only for old products which
 * build exynos_camera.c without EXYNOS_CAMERA_EMBEDDED_BACKEND.
 */
#ifndef EXYNOS_CAMERA_BACKEND_H
#define EXYNOS_CAMERA_BACKEND_H

#include <hardware/camera.h>

#ifdef __cplusplus
extern "C" {
#endif

struct exynos_camera;

/*
 * A positive callback result transfers the dequeued FIMC2 recording buffer
 * to the Camera3 FIMC1 worker. The backend must not QBUF that index until the
 * worker explicitly returns it through
 * exynos_camera_backend_requeue_recording_buffer().
 */
#define EXYNOS_CAMERA_FRAME_RECORDING_OWNED 1

typedef int (*exynos_camera_backend_frame_callback)(const void *preview_data,
	size_t preview_size, const void *recording_data, size_t recording_size,
	uint32_t preview_y_addr, uint32_t preview_cbcr_addr,
	uint32_t recording_y_addr, uint32_t recording_cbcr_addr,
	int recording_index, int64_t timestamp_ns, void *user);

int exynos_camera_backend_get_number_of_cameras(void);
int exynos_camera_backend_get_camera_info(int id, int *facing, int *orientation);

int exynos_camera_backend_open(int id, struct exynos_camera **camera);
void exynos_camera_backend_close(struct exynos_camera *camera);
int exynos_camera_backend_reset_capture_nodes(struct exynos_camera *camera);

#ifndef EXYNOS_CAMERA_EMBEDDED_BACKEND
int exynos_camera_backend_set_preview_window(struct exynos_camera *camera,
	struct preview_stream_ops *window);
#endif
void exynos_camera_backend_set_callbacks(struct exynos_camera *camera,
	camera_notify_callback notify_cb, camera_data_callback data_cb,
	camera_data_timestamp_callback timestamp_cb,
	camera_request_memory request_memory, void *user);
void exynos_camera_backend_enable_messages(struct exynos_camera *camera,
	int32_t message_types);
void exynos_camera_backend_set_frame_callback(struct exynos_camera *camera,
	exynos_camera_backend_frame_callback callback, void *user);
int exynos_camera_backend_requeue_recording_buffer(struct exynos_camera *camera,
	int index);
void exynos_camera_backend_set_recording_stream(struct exynos_camera *camera,
	int enabled);

int exynos_camera_backend_start_preview(struct exynos_camera *camera);
void exynos_camera_backend_stop_preview(struct exynos_camera *camera);
int exynos_camera_backend_auto_focus(struct exynos_camera *camera);
void exynos_camera_backend_cancel_auto_focus(struct exynos_camera *camera);
int exynos_camera_backend_take_picture(struct exynos_camera *camera);
void exynos_camera_backend_cancel_picture(struct exynos_camera *camera);

int exynos_camera_backend_set_parameters(struct exynos_camera *camera,
	const char *parameters);
char *exynos_camera_backend_get_parameters(struct exynos_camera *camera);
void exynos_camera_backend_free_parameters(char *parameters);

#ifdef __cplusplus
}
#endif

#endif /* EXYNOS_CAMERA_BACKEND_H */
