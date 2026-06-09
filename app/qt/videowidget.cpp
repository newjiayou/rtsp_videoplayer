#include "videowidget.h"

#include <algorithm>

namespace {

constexpr const char* kVertexShader = R"(
attribute vec2 position;
attribute vec2 texcoord;
varying vec2 v_texcoord;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    v_texcoord = texcoord;
}
)";

constexpr const char* kYuvFragmentShader = R"(
varying vec2 v_texcoord;
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;
uniform int format;
void main() {
    float y = texture2D(tex_y, v_texcoord).r;
    float u = 0.0;
    float v = 0.0;
    if (format == 1) {
        u = texture2D(tex_u, v_texcoord).r - 0.5;
        v = texture2D(tex_v, v_texcoord).r - 0.5;
    } else {
        vec2 uv = texture2D(tex_u, v_texcoord).ra;
        u = uv.x - 0.5;
        v = uv.y - 0.5;
    }
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    gl_FragColor = vec4(r, g, b, 1.0);
}
)";

constexpr const char* kBgraFragmentShader = R"(
varying vec2 v_texcoord;
uniform sampler2D tex_bgra;
void main() {
    vec4 bgra = texture2D(tex_bgra, v_texcoord);
    gl_FragColor = vec4(bgra.b, bgra.g, bgra.r, bgra.a);
}
)";

constexpr GLfloat kVertices[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

int ChromaWidth(int width) {
    return (width + 1) / 2;
}

int ChromaHeight(int height) {
    return (height + 1) / 2;
}

} // namespace

VideoWidget::VideoWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setAutoFillBackground(false);
}

VideoWidget::~VideoWidget() {
    makeCurrent();
    DestroyTextures();
    doneCurrent();
}

void VideoWidget::setFrame(edgelive::VideoFrame frame) {
    frame_ = std::move(frame);
    update();
}

void VideoWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    EnsureTextures();

    yuv_program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    yuv_program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kYuvFragmentShader);
    yuv_program_.link();

    bgra_program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    bgra_program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kBgraFragmentShader);
    bgra_program_.link();

    initialized_ = true;
}

void VideoWidget::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
}

void VideoWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);
    if (!initialized_ || !frame_.buffer || frame_.width <= 0 || frame_.height <= 0) {
        return;
    }

    EnsureTextures();
    if (frame_.format == edgelive::VideoPixelFormat::Yuv420p ||
        frame_.format == edgelive::VideoPixelFormat::Nv12) {
        PaintYuvFrame();
    } else {
        PaintBgraFrame();
    }
}

void VideoWidget::EnsureTextures() {
    if (textures_[0] != 0) {
        return;
    }
    glGenTextures(3, textures_);
    for (GLuint texture : textures_) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VideoWidget::DestroyTextures() {
    if (textures_[0] == 0) {
        return;
    }
    glDeleteTextures(3, textures_);
    textures_[0] = 0;
    textures_[1] = 0;
    textures_[2] = 0;
    texture_widths_[0] = 0;
    texture_widths_[1] = 0;
    texture_widths_[2] = 0;
    texture_heights_[0] = 0;
    texture_heights_[1] = 0;
    texture_heights_[2] = 0;
    texture_formats_[0] = 0;
    texture_formats_[1] = 0;
    texture_formats_[2] = 0;
}

void VideoWidget::UploadTexture(
    int index,
    GLuint texture,
    int width,
    int height,
    int stride,
    const uint8_t* data,
    GLenum format) {
    if (index < 0 || index >= 3 || texture == 0 || width <= 0 || height <= 0 || stride <= 0 || data == nullptr) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    const bool size_changed =
        texture_widths_[index] != width || texture_heights_[index] != height || texture_formats_[index] != format;
    if (size_changed) {
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        texture_widths_[index] = width;
        texture_heights_[index] = height;
        texture_formats_[index] = format;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void VideoWidget::PaintYuvFrame() {
    if (!yuv_program_.bind()) {
        return;
    }

    UploadTexture(0, textures_[0], frame_.width, frame_.height, frame_.strides[0], frame_.planes[0], GL_LUMINANCE);
    if (frame_.format == edgelive::VideoPixelFormat::Nv12) {
        UploadTexture(
            1,
            textures_[1],
            ChromaWidth(frame_.width),
            ChromaHeight(frame_.height),
            frame_.strides[1] / 2,
            frame_.planes[1],
            GL_LUMINANCE_ALPHA);
    } else {
        UploadTexture(
            1,
            textures_[1],
            ChromaWidth(frame_.width),
            ChromaHeight(frame_.height),
            frame_.strides[1],
            frame_.planes[1],
            GL_LUMINANCE);
        UploadTexture(
            2,
            textures_[2],
            ChromaWidth(frame_.width),
            ChromaHeight(frame_.height),
            frame_.strides[2],
            frame_.planes[2],
            GL_LUMINANCE);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    yuv_program_.setUniformValue("tex_y", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    yuv_program_.setUniformValue("tex_u", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, textures_[2]);
    yuv_program_.setUniformValue("tex_v", 2);
    yuv_program_.setUniformValue(
        "format",
        frame_.format == edgelive::VideoPixelFormat::Yuv420p ? 1 : 2);

    const int position = yuv_program_.attributeLocation("position");
    const int texcoord = yuv_program_.attributeLocation("texcoord");
    yuv_program_.enableAttributeArray(position);
    yuv_program_.enableAttributeArray(texcoord);
    yuv_program_.setAttributeArray(position, GL_FLOAT, kVertices, 2, 4 * sizeof(GLfloat));
    yuv_program_.setAttributeArray(texcoord, GL_FLOAT, kVertices + 2, 2, 4 * sizeof(GLfloat));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    yuv_program_.disableAttributeArray(position);
    yuv_program_.disableAttributeArray(texcoord);
    yuv_program_.release();
}

void VideoWidget::PaintBgraFrame() {
    if (!bgra_program_.bind()) {
        return;
    }

    UploadTexture(0, textures_[0], frame_.width, frame_.height, frame_.strides[0], frame_.planes[0], GL_RGBA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    bgra_program_.setUniformValue("tex_bgra", 0);

    const int position = bgra_program_.attributeLocation("position");
    const int texcoord = bgra_program_.attributeLocation("texcoord");
    bgra_program_.enableAttributeArray(position);
    bgra_program_.enableAttributeArray(texcoord);
    bgra_program_.setAttributeArray(position, GL_FLOAT, kVertices, 2, 4 * sizeof(GLfloat));
    bgra_program_.setAttributeArray(texcoord, GL_FLOAT, kVertices + 2, 2, 4 * sizeof(GLfloat));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    bgra_program_.disableAttributeArray(position);
    bgra_program_.disableAttributeArray(texcoord);
    bgra_program_.release();
}
