
/********************************************************************************
 * TEMAT PROJEKTU: Nakładanie efektu rozmycia (blur) na zdjęcie.
 * OPIS ALGORYTMU: Implementacja efektu rozmycia (Blura) – porównanie algorytmu C++ i Asemblera MASM x64
 * DATA WYKONANIA: grudzień 2025 r.
 * SEMESTR / ROK AKADEMICKI: Semestr Zimowy 2025/2026
 * AUTOR: Mateusz Grzanka
 * * AKTUALNA WERSJA: 1.1
 ********************************************************************************/

#define NOMINMAX
#undef max
#undef min

#include <algorithm>
#include <thread>
#include <vector>
#include <cmath>
#include "Blur.h"

static void blur_region(const unsigned char* srcPixels, unsigned char* dstPixels, int width, int height, int centerX, int centerY, int radius, int y0, int y1) {
    const int kernel = 20;

    int xMinPossible = centerX - radius;
    int xMaxPossible = centerX + radius;

    int xStart = std::max(0, xMinPossible);
    int xEnd = std::min(width - 1, xMaxPossible);

    for (int y = y0; y < y1; ++y) {
        for (int x = xStart; x <= xEnd; ++x) {
            double dist = std::sqrt(
                double(x - centerX) * double(x - centerX) +
                double(y - centerY) * double(y - centerY)
            );

            if (dist > radius)
                continue;

            int rSum = 0, gSum = 0, bSum = 0, count = 0;

            for (int ky = -kernel; ky <= kernel; ++ky) {
                int ny = y + ky;

                if (ny < 0) ny = 0;
                if (ny >= height) ny = height - 1;

                const unsigned char* rowPtr = srcPixels + (ny * width) * 3;

                for (int kx = -kernel; kx <= kernel; ++kx) {
                    int nx = x + kx;

                    if (nx < 0) nx = 0;
                    if (nx >= width) nx = width - 1;

                    const unsigned char* p = rowPtr + nx * 3;

                    rSum += p[0];
                    gSum += p[1];
                    bSum += p[2];
                    ++count;
                }
            }

            if (count > 0) {
                unsigned char* dst = dstPixels + (y * width + x) * 3;
                dst[0] = (unsigned char)(rSum / count);
                dst[1] = (unsigned char)(gSum / count);
                dst[2] = (unsigned char)(bSum / count);
            }
        }
    }
}


void blur_inplace(unsigned char* pixels, int width, int height, int centerX, int centerY, int radius, int threads) {
    if (!pixels || width <= 0 || height <= 0 || radius <= 0)
        return;

    if (threads < 1) threads = 1;
    int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    int maxThreads = std::min(threads, hw);

    std::vector<unsigned char> sourceCopy(pixels, pixels + (width * height * 3));
    const unsigned char* srcPtr = sourceCopy.data();

    if (maxThreads <= 1) {
        blur_region(srcPtr, pixels, width, height, centerX, centerY, radius, 0, height);
        return;
    }

    std::vector<std::thread> pool;
    pool.reserve(maxThreads);

    int rowsPerThread = height / maxThreads;
    int extra = height % maxThreads;
    int yStart = 0;

    for (int i = 0; i < maxThreads; ++i) {
        int y0 = yStart;
        int block = rowsPerThread + (i < extra ? 1 : 0);
        int y1 = y0 + block;
        yStart = y1;

        pool.emplace_back([=]() {
            blur_region(srcPtr, pixels, width, height, centerX, centerY, radius, y0, y1);
        });
    }

    for (auto& t : pool)
        t.join();
}