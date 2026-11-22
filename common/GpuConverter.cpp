// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "GpuConverter"

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <v4l2_codec2/common/GpuConverter.h>

#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <log/log.h>

namespace android {
namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

// Byte order R, G, B, A, which is what HAL_PIXEL_FORMAT_RGBA_8888 is.
constexpr uint32_t kDrmFormatAbgr8888 = fourcc('A', 'B', '2', '4');
constexpr uint32_t kDrmFormatR8 = fourcc('R', '8', ' ', ' ');
// Byte order R, G: U, V, as NV12 interleaves them.
constexpr uint32_t kDrmFormatGr88 = fourcc('G', 'R', '8', '8');

const char kVertexShader[] =
        "attribute vec2 position;\n"
        "varying vec2 texCoord;\n"
        "void main() {\n"
        "    texCoord = position * 0.5 + 0.5;\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "}\n";

// BT.601, limited range, the same as libyuv's ABGRToI420().
const char kFragmentShaderY[] =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision highp float;\n"
        "uniform samplerExternalOES tex;\n"
        "varying vec2 texCoord;\n"
        "void main() {\n"
        "    vec3 c = texture2D(tex, texCoord).rgb;\n"
        "    float y = dot(c, vec3(0.2578125, 0.50390625, 0.09765625)) + 0.0625;\n"
        "    gl_FragColor = vec4(y, 0.0, 0.0, 1.0);\n"
        "}\n";

// Drawn at half size: each fragment lands on the corner of four source pixels, which the linear
// filter averages.
const char kFragmentShaderUV[] =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision highp float;\n"
        "uniform samplerExternalOES tex;\n"
        "varying vec2 texCoord;\n"
        "void main() {\n"
        "    vec3 c = texture2D(tex, texCoord).rgb;\n"
        "    float u = dot(c, vec3(-0.1484375, -0.2890625, 0.4375)) + 0.5;\n"
        "    float v = dot(c, vec3(0.4375, -0.3671875, -0.0703125)) + 0.5;\n"
        "    gl_FragColor = vec4(u, v, 0.0, 1.0);\n"
        "}\n";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        ALOGE("Failed to compile shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(const char* fragmentSource) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        ALOGE("Failed to link program");
        glDeleteProgram(program);
        return 0;
    }
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);
    return program;
}

}  // namespace

// static
std::unique_ptr<GpuConverter> GpuConverter::create() {
    std::unique_ptr<GpuConverter> converter(new GpuConverter());
    if (!converter->initialize()) return nullptr;
    return converter;
}

bool GpuConverter::initialize() {
    // The shaders are tiny, and the service may not create the cache directory.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);

    mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mDisplay == EGL_NO_DISPLAY || !eglInitialize(mDisplay, nullptr, nullptr)) {
        ALOGE("Failed to initialize EGL");
        mDisplay = EGL_NO_DISPLAY;
        return false;
    }
    // Android's EGL loader leaves EGL_EXT_image_dma_buf_import out of the extensions it lists,
    // but passes the images on to the driver: the first import tells whether it works.

    const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                    EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(mDisplay, configAttribs, &config, 1, &numConfigs) || !numConfigs) {
        ALOGE("No EGL config");
        return false;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    mContext = eglCreateContext(mDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    mSurface = eglCreatePbufferSurface(mDisplay, config, surfaceAttribs);
    if (mContext == EGL_NO_CONTEXT || mSurface == EGL_NO_SURFACE ||
        !eglMakeCurrent(mDisplay, mSurface, mSurface, mContext)) {
        ALOGE("Failed to make an EGL context current");
        return false;
    }

    mProgramY = linkProgram(kFragmentShaderY);
    mProgramUV = linkProgram(kFragmentShaderUV);
    if (!mProgramY || !mProgramUV) return false;

    glGenTextures(1, &mSrcTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mSrcTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &mDstTexture);
    glGenFramebuffers(1, &mFramebuffer);

    static const GLfloat kQuad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    ALOGI("Converting RGBA to NV12 on the GPU (%s)", glGetString(GL_RENDERER));
    return true;
}

GpuConverter::~GpuConverter() {
    if (mDisplay == EGL_NO_DISPLAY) return;
    if (mContext != EGL_NO_CONTEXT) {
        glDeleteFramebuffers(1, &mFramebuffer);
        glDeleteTextures(1, &mDstTexture);
        glDeleteTextures(1, &mSrcTexture);
        if (mProgramY) glDeleteProgram(mProgramY);
        if (mProgramUV) glDeleteProgram(mProgramUV);
        eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(mDisplay, mContext);
    }
    if (mSurface != EGL_NO_SURFACE) eglDestroySurface(mDisplay, mSurface);
    eglTerminate(mDisplay);
}

void* GpuConverter::importImage(const Plane& plane, uint32_t format, uint32_t width,
                                      uint32_t height) {
    const EGLint attribs[] = {EGL_WIDTH,
                              static_cast<EGLint>(width),
                              EGL_HEIGHT,
                              static_cast<EGLint>(height),
                              EGL_LINUX_DRM_FOURCC_EXT,
                              static_cast<EGLint>(format),
                              EGL_DMA_BUF_PLANE0_FD_EXT,
                              plane.fd,
                              EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                              static_cast<EGLint>(plane.offset),
                              EGL_DMA_BUF_PLANE0_PITCH_EXT,
                              static_cast<EGLint>(plane.stride),
                              EGL_NONE};
    EGLImageKHR image = eglCreateImageKHR(mDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                          nullptr, attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        ALOGE("Failed to import a %ux%u dma-buf (format %#x): %#x", width, height, format,
              eglGetError());
    }
    return image;
}

bool GpuConverter::drawInto(void* image, uint32_t program, uint32_t width, uint32_t height) {
    glBindTexture(GL_TEXTURE_2D, mDstTexture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(image));
    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mDstTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        ALOGE("Cannot render into the %ux%u output plane", width, height);
        return false;
    }
    glViewport(0, 0, width, height);
    // The draw covers all of the plane: clearing first spares loading the old contents in.
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mSrcTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return true;
}

bool GpuConverter::convertRgbaToNv12(const Plane& src, uint32_t srcWidth, uint32_t srcHeight,
                                     const Plane& dstY, const Plane& dstUV, uint32_t width,
                                     uint32_t height) {
    void* srcImage = importImage(src, kDrmFormatAbgr8888, srcWidth, srcHeight);
    void* yImage = importImage(dstY, kDrmFormatR8, width, height);
    void* uvImage = importImage(dstUV, kDrmFormatGr88, width / 2, height / 2);
    bool ok = srcImage != EGL_NO_IMAGE_KHR && yImage != EGL_NO_IMAGE_KHR &&
              uvImage != EGL_NO_IMAGE_KHR;
    if (ok) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, mSrcTexture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(srcImage));
        ok = drawInto(yImage, mProgramY, width, height) &&
             drawInto(uvImage, mProgramUV, width / 2, height / 2);
        // The encoder reads the planes as soon as we return.
        glFinish();
        // Detach the images before they go.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (glGetError() != GL_NO_ERROR) ok = false;
    }
    if (srcImage != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(mDisplay, srcImage);
    if (yImage != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(mDisplay, yImage);
    if (uvImage != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(mDisplay, uvImage);
    return ok;
}

}  // namespace android
