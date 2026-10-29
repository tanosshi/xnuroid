// OpenGL ES 3.2 backend plumbing for the Metal emulation: EGL contexts and frame capture.
//
// Every thread that issues Metal work gets its own EGL context in one share group, so textures, buffers, samplers
// and programs are visible everywhere; framebuffers and vertex arrays are per context and are only made by the
// thread that draws. There is no window: rendering goes to textures, and presented frames can be saved as PNG.
#pragma once

#include <cstdint>

namespace gles {

// Makes a context current on the calling thread (created on first use). False if EGL/GLES is unavailable.
bool ensure_context();
// Makes GL writes from this thread visible to the other contexts (glFinish when called off the drawing thread).
void publish();
// Logs GL_RENDERER/VERSION, limits and the extensions the Metal emulation cares about.
void probe();
bool has_extension(const char* name);
// Writes an RGBA8 image (rows top to bottom) as a PNG file.
bool write_png(const char* path, int w, int h, const uint8_t* rgba);
// LOADER_MSL_TEST: translates every <dir>/*.metal to GLSL, compiles it with the driver, prints a report.
// Returns the number of failures.
int msl_selftest(const char* dir);

}  // namespace gles
