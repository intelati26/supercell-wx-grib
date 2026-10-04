#pragma once

// glad before QOpenGLWidget, which would otherwise pull in the system GL
// header first.
#include <scwx/qt/gl/gl.hpp>
#include <scwx/qt/volume/radar_volume.hpp>

#include <memory>

#include <QOpenGLWidget>

namespace scwx::qt::ui
{

// The OpenGL view of the 3D volume pane: draws a volume::VolumeMesh over a
// ground grid the size of the region, with an orbit camera.
//
// Mouse: left drag orbits, right or middle drag pans, the wheel zooms and a
// double click resets the camera.
class VolumeViewWidget : public QOpenGLWidget
{
   Q_OBJECT
   Q_DISABLE_COPY_MOVE(VolumeViewWidget)

public:
   explicit VolumeViewWidget(QWidget* parent = nullptr);
   ~VolumeViewWidget() override;

   // Takes the mesh to upload on the next paint. The CPU copy is released
   // once it is on the GPU; ContextLost() asks for it again if the GL context
   // goes away (the pane being floated or docked again).
   void SetMesh(std::shared_ptr<const volume::VolumeMesh> mesh,
                double                                    halfSizeKm);
   void ClearMesh();

   void SetVerticalScale(float scale);
   void SetOpacity(float opacity);
   // Draw only the lowest count tilts.
   void SetVisibleTiltCount(std::size_t count);
   // Shown top left over the view (site, time, product).
   void SetCaption(const QString& caption);

   void ResetCamera();

signals:
   void ContextLost();

protected:
   void initializeGL() override;
   void paintGL() override;

   void mousePressEvent(QMouseEvent* event) override;
   void mouseMoveEvent(QMouseEvent* event) override;
   void mouseReleaseEvent(QMouseEvent* event) override;
   void mouseDoubleClickEvent(QMouseEvent* event) override;
   void wheelEvent(QWheelEvent* event) override;

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::ui
