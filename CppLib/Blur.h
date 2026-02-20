#pragma once

#define NOMINMAX

#ifdef __cplusplus
extern "C" {
#endif

__declspec(dllexport)
    void blur_inplace(unsigned char* pixels, int width, int height, int centerX, int centerY, int radius, int threads);

#ifdef __cplusplus
}
#endif