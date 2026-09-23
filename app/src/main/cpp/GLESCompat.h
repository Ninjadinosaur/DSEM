#pragma once

// Lets melonDS's desktop OpenGL renderer build against OpenGL ES 3.2 (the Adreno 750's GL).
// melonDS includes this via MELONDS_GL_HEADER instead of its desktop glad loader.
// Only a handful of desktop-only entry points need translating; shaders are translated
// at compile time in OpenGLSupport.cpp (see third_party/README.md, "Local patches").

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GL_READ_WRITE
#define GL_READ_WRITE 0x88BA
#endif

#define MELONDS_GLES 1

static inline void glClearDepth(double d) { glClearDepthf((float)d); }
static inline void glDepthRange(double n, double f) { glDepthRangef((float)n, (float)f); }

// ES has no single-buffer glDrawBuffer; route it through glDrawBuffers.
static inline void glDrawBuffer(GLenum buf)
{
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    if (fbo == 0)
    {
        // The default framebuffer only accepts GL_BACK or GL_NONE.
        GLenum b = (buf == GL_NONE) ? GL_NONE : GL_BACK;
        glDrawBuffers(1, &b);
        return;
    }
    glDrawBuffers(1, &buf);
}

// ES only has glMapBufferRange.
static inline void* glMapBuffer(GLenum target, GLenum access)
{
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    GLbitfield bits = 0;
    if (access == GL_READ_ONLY || access == GL_READ_WRITE) bits |= GL_MAP_READ_BIT;
    if (access == GL_WRITE_ONLY || access == GL_READ_WRITE) bits |= GL_MAP_WRITE_BIT;
    return glMapBufferRange(target, 0, size, bits);
}

// Fragment output locations are declared in the shader source on ES; nothing to do here.
static inline void glBindFragDataLocation(GLuint, GLuint, const char*) {}

// ---------------------------------------------------------------------------------------------
// DS 15-bit colour. melonDS uploads DS colours as GL_UNSIGNED_SHORT_1_5_5_5_REV (red in the low
// bits, alpha in bit 15), which ES lacks. ES has the same depth as GL_UNSIGNED_SHORT_5_5_5_1
// with the bits in the opposite order, so those uploads and read-backs are converted here.
// ---------------------------------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <vector>

#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366

namespace ds13r_gles
{
inline std::vector<uint16_t>& Scratch16()
{
    static thread_local std::vector<uint16_t> buf;
    return buf;
}
inline std::vector<uint8_t>& Scratch8()
{
    static thread_local std::vector<uint8_t> buf;
    return buf;
}

// DS ABGR1555 (R = bits 0-4) -> ES RGBA5551 (R = bits 11-15, A = bit 0)
inline const void* ToES5551(const void* src, size_t count)
{
    if (!src) return nullptr;
    auto& out = Scratch16();
    out.resize(count);
    const uint16_t* in = static_cast<const uint16_t*>(src);
    for (size_t i = 0; i < count; i++)
    {
        uint16_t c = in[i];
        uint16_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F, a = c >> 15;
        out[i] = (uint16_t)((r << 11) | (g << 6) | (b << 1) | a);
    }
    return out.data();
}
}

static inline void ds13r_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                      GLint border, GLenum format, GLenum type, const void* pixels)
{
    if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV)
    {
        pixels = ds13r_gles::ToES5551(pixels, (size_t)width * height);
        type = GL_UNSIGNED_SHORT_5_5_5_1;
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

static inline void ds13r_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                      GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels)
{
    if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV)
    {
        pixels = ds13r_gles::ToES5551(pixels, (size_t)width * height * depth);
        type = GL_UNSIGNED_SHORT_5_5_5_1;
    }
    glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, pixels);
}

static inline void ds13r_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                                         GLenum format, GLenum type, const void* pixels)
{
    if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV)
    {
        pixels = ds13r_gles::ToES5551(pixels, (size_t)width * height);
        type = GL_UNSIGNED_SHORT_5_5_5_1;
    }
    glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
}

static inline void ds13r_glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width,
                                         GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV)
    {
        pixels = ds13r_gles::ToES5551(pixels, (size_t)width * height * depth);
        type = GL_UNSIGNED_SHORT_5_5_5_1;
    }
    glTexSubImage3D(target, level, x, y, z, width, height, depth, format, type, pixels);
}

// Read-backs of DS colour: read as RGBA8 (always allowed on ES) and pack into DS ABGR1555.
static inline void ds13r_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    if (type != GL_UNSIGNED_SHORT_1_5_5_5_REV)
    {
        glReadPixels(x, y, width, height, format, type, pixels);
        return;
    }
    auto& tmp = ds13r_gles::Scratch8();
    size_t n = (size_t)width * height;
    tmp.resize(n * 4);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    uint16_t* out = static_cast<uint16_t*>(pixels);
    for (size_t i = 0; i < n; i++)
    {
        const uint8_t* p = &tmp[i * 4];
        out[i] = (uint16_t)((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | ((p[3] >= 128 ? 1 : 0) << 15));
    }
}

#define glTexImage2D ds13r_glTexImage2D
#define glTexImage3D ds13r_glTexImage3D
#define glTexSubImage2D ds13r_glTexSubImage2D
#define glTexSubImage3D ds13r_glTexSubImage3D
#define glReadPixels ds13r_glReadPixels
