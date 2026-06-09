#pragma once

#include "sdk/player.h"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    void setFrame(edgelive::VideoFrame frame);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void EnsureTextures();
    void DestroyTextures();
    void PaintBgraFrame();
    void PaintYuvFrame();
    void UploadTexture(
        int index,
        GLuint texture,
        int width,
        int height,
        int stride,
        const uint8_t* data,
        GLenum format);

private:
    edgelive::VideoFrame frame_;
    QOpenGLShaderProgram yuv_program_;
    QOpenGLShaderProgram bgra_program_;
    GLuint textures_[3]{0, 0, 0};
    int texture_widths_[3]{0, 0, 0};
    int texture_heights_[3]{0, 0, 0};
    GLenum texture_formats_[3]{0, 0, 0};
    bool initialized_{false};
};
