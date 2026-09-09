/*
 * Build the Exynos4210 FIMC memory-to-memory implementation into the native
 * camera HAL. Keeping this small translation unit in the camera package makes
 * clean Soong builds deterministic while retaining the Samsung implementation
 * as the single source of truth.
 */
#include "../../../../hardware/samsung/exynos4/hal/libfimc/SecFimc.cpp"
