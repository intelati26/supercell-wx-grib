#include <scwx/qt/ui/volume_view_widget.hpp>
#include <scwx/qt/gl/gl.hpp>
#include <scwx/qt/gl/shader_program.hpp>
#include <scwx/util/logger.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QWheelEvent>

namespace scwx::qt::ui
{

static const std::string logPrefix_ = "scwx::qt::ui::volume_view_widget";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

static constexpr float kDefaultYawDeg_        = 200.0f;
static constexpr float kDefaultPitchDeg_      = 28.0f;
static constexpr float kMinPitchDeg_          = 2.0f;
static constexpr float kMaxPitchDeg_          = 89.0f;
static constexpr float kOrbitDegPerPixel_     = 0.4f;
static constexpr float kZoomPerWheelStep_     = 0.88f;
static constexpr float kWheelDeltaPerStep_    = 120.0f;
static constexpr float kDefaultDistanceScale_ = 2.6f;
static constexpr float kMinDistanceKm_        = 2.0f;
static constexpr float kFieldOfViewDeg_       = 40.0f;
static constexpr float kTargetHeightKm_       = 4.0f;
static constexpr float kAxisTopKm_            = 20.0f;
static constexpr float kAxisTickKm_           = 5.0f;
static constexpr float kOpaqueThreshold_      = 0.999f;
static constexpr float kDefaultHalfSizeKm_    = 50.0f;
static constexpr float kDefaultVerticalScale_ = 3.0f;
static constexpr float kDefaultOpacity_       = 0.75f;
static constexpr float kMaxDistanceScale_     = 10.0f;
static constexpr float kLabelClipNdc_         = 1.2f;
static constexpr qreal kLabelMarginPx_        = 6.0;
static constexpr int   kMinWidthPx_           = 200;
static constexpr int   kMinHeightPx_          = 150;
static constexpr int   kAxisTickCount_ =
   static_cast<int>(kAxisTopKm_ / kAxisTickKm_);

static constexpr QRgb kAxisLabelColor_ = qRgb(210, 210, 215);
static constexpr QRgb kGridLabelColor_ = qRgb(150, 156, 168);
static constexpr QRgb kCaptionColor_   = qRgb(235, 235, 240);

static constexpr std::array<std::uint8_t, 4> kGroundColor_ {38, 42, 50, 255};
static constexpr std::array<std::uint8_t, 4> kGridColor_ {70, 76, 88, 255};
static constexpr std::array<std::uint8_t, 4> kEdgeColor_ {130, 138, 152, 255};
static constexpr std::array<std::uint8_t, 4> kAxisColor_ {200, 200, 205, 255};
static constexpr std::array<float, 3>        kClearColor_ {0.09f, 0.10f, 0.12f};

class VolumeViewWidget::Impl
{
public:
   explicit Impl(VolumeViewWidget* self) : self_ {self} {}
   ~Impl()                      = default;
   Impl(const Impl&)            = delete;
   Impl& operator=(const Impl&) = delete;
   Impl(Impl&&)                 = delete;
   Impl& operator=(Impl&&)      = delete;

   void CleanupGl();
   void UploadMesh();
   void UploadGuides();
   void DeleteMeshBuffers();

   [[nodiscard]] glm::mat4 ViewProjection() const;
   [[nodiscard]] float     GridSpacingKm() const;
   void                    DrawLabels(QPainter& painter, const glm::mat4& mvp);

   VolumeViewWidget* self_;

   std::unique_ptr<gl::ShaderProgram> shader_ {};
   GLint                              uMvpLocation_ {-1};
   GLint                              uVerticalScaleLocation_ {-1};
   GLint                              uOpacityLocation_ {-1};
   bool                               glReady_ {false};

   GLuint                meshVao_ {0};
   std::array<GLuint, 2> meshBuffers_ {0, 0};
   bool                  meshUploaded_ {false};

   GLuint  guideVao_ {0};
   GLuint  guideVbo_ {0};
   GLsizei groundVertexCount_ {0};
   GLsizei lineVertexCount_ {0};
   bool    guidesDirty_ {true};

   std::shared_ptr<const volume::VolumeMesh>  pendingMesh_ {};
   std::vector<volume::VolumeMesh::TiltRange> tilts_ {};

   double      halfSizeKm_ {kDefaultHalfSizeKm_};
   float       verticalScale_ {kDefaultVerticalScale_};
   float       opacity_ {kDefaultOpacity_};
   std::size_t visibleTiltCount_ {std::numeric_limits<std::size_t>::max()};
   QString     caption_ {};

   float     yawDeg_ {kDefaultYawDeg_};
   float     pitchDeg_ {kDefaultPitchDeg_};
   float     distanceKm_ {0.0f};
   glm::vec3 target_ {0.0f};

   QPointF         lastMousePos_ {};
   Qt::MouseButton dragButton_ {Qt::NoButton};
};

VolumeViewWidget::VolumeViewWidget(QWidget* parent) :
    QOpenGLWidget(parent), p {std::make_unique<Impl>(this)}
{
   setMinimumSize(kMinWidthPx_, kMinHeightPx_);
   ResetCamera();
}

VolumeViewWidget::~VolumeViewWidget()
{
   p->CleanupGl();
}

void VolumeViewWidget::SetMesh(std::shared_ptr<const volume::VolumeMesh> mesh,
                               double halfSizeKm)
{
   if (halfSizeKm != p->halfSizeKm_)
   {
      p->halfSizeKm_  = halfSizeKm;
      p->guidesDirty_ = true;
      ResetCamera();
   }
   p->pendingMesh_ = std::move(mesh);
   update();
}

void VolumeViewWidget::ClearMesh()
{
   p->pendingMesh_.reset();
   p->tilts_.clear();
   if (p->glReady_ && p->meshUploaded_)
   {
      makeCurrent();
      p->DeleteMeshBuffers();
      doneCurrent();
   }
   update();
}

void VolumeViewWidget::SetVerticalScale(float scale)
{
   p->verticalScale_ = scale;
   update();
}

void VolumeViewWidget::SetOpacity(float opacity)
{
   p->opacity_ = opacity;
   update();
}

void VolumeViewWidget::SetVisibleTiltCount(std::size_t count)
{
   p->visibleTiltCount_ = count;
   update();
}

void VolumeViewWidget::SetCaption(const QString& caption)
{
   p->caption_ = caption;
   update();
}

void VolumeViewWidget::ResetCamera()
{
   p->yawDeg_     = kDefaultYawDeg_;
   p->pitchDeg_   = kDefaultPitchDeg_;
   p->distanceKm_ = static_cast<float>(p->halfSizeKm_) * kDefaultDistanceScale_;
   p->target_     = {0.0f, 0.0f, kTargetHeightKm_};
   update();
}

void VolumeViewWidget::initializeGL()
{
   if (gladLoaderLoadGL() == 0)
   {
      logger_->error("gladLoaderLoadGL failed");
      return;
   }

   p->shader_ = std::make_unique<gl::ShaderProgram>();
   if (!p->shader_->Load(":/gl/volume.vert", ":/gl/volume.frag"))
   {
      logger_->error("Could not load the volume shaders");
      p->shader_.reset();
      return;
   }
   p->uMvpLocation_ = p->shader_->GetUniformLocation("uMVPMatrix");
   p->uVerticalScaleLocation_ =
      p->shader_->GetUniformLocation("uVerticalScale");
   p->uOpacityLocation_ = p->shader_->GetUniformLocation("uOpacity");

   glGenVertexArrays(1, &p->guideVao_);
   glGenBuffers(1, &p->guideVbo_);
   p->guidesDirty_ = true;
   p->glReady_     = true;

   // Floating or docking the pane again replaces the GL context: free this
   // one's objects and ask for the mesh again.
   connect(context(),
           &QOpenGLContext::aboutToBeDestroyed,
           this,
           [this]()
           {
              const bool hadMesh = p->meshUploaded_ || p->pendingMesh_;
              p->CleanupGl();
              if (hadMesh)
              {
                 Q_EMIT ContextLost();
              }
           });
}

void VolumeViewWidget::Impl::CleanupGl()
{
   if (!glReady_)
   {
      return;
   }
   self_->makeCurrent();
   DeleteMeshBuffers();
   glDeleteVertexArrays(1, &guideVao_);
   glDeleteBuffers(1, &guideVbo_);
   guideVao_ = 0;
   guideVbo_ = 0;
   shader_.reset();
   self_->doneCurrent();
   glReady_ = false;
}

void VolumeViewWidget::Impl::DeleteMeshBuffers()
{
   if (meshVao_ != 0)
   {
      glDeleteVertexArrays(1, &meshVao_);
      glDeleteBuffers(2, meshBuffers_.data());
      meshVao_        = 0;
      meshBuffers_[0] = 0;
      meshBuffers_[1] = 0;
   }
   meshUploaded_ = false;
}

// GL takes vertex attribute offsets into the bound buffer as pointers.
static const void* BufferOffset(std::size_t bytes)
{
   // NOLINTNEXTLINE(performance-no-int-to-ptr)
   return reinterpret_cast<const void*>(bytes);
}

static void SetVertexLayout()
{
   constexpr auto kStride = static_cast<GLsizei>(sizeof(volume::MeshVertex));
   glVertexAttribPointer(0,
                         3,
                         GL_FLOAT,
                         GL_FALSE,
                         kStride,
                         BufferOffset(offsetof(volume::MeshVertex, x)));
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(1,
                         4,
                         GL_UNSIGNED_BYTE,
                         GL_TRUE,
                         kStride,
                         BufferOffset(offsetof(volume::MeshVertex, rgba)));
   glEnableVertexAttribArray(1);
}

void VolumeViewWidget::Impl::UploadMesh()
{
   const std::shared_ptr<const volume::VolumeMesh> mesh =
      std::move(pendingMesh_);
   pendingMesh_.reset();

   DeleteMeshBuffers();
   tilts_ = mesh->tilts;
   if (mesh->indices.empty())
   {
      return;
   }

   glGenVertexArrays(1, &meshVao_);
   glGenBuffers(2, meshBuffers_.data());
   glBindVertexArray(meshVao_);

   glBindBuffer(GL_ARRAY_BUFFER, meshBuffers_[0]);
   glBufferData(GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(mesh->vertices.size() *
                                        sizeof(volume::MeshVertex)),
                mesh->vertices.data(),
                GL_STATIC_DRAW);
   SetVertexLayout();

   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshBuffers_[1]);
   glBufferData(
      GL_ELEMENT_ARRAY_BUFFER,
      static_cast<GLsizeiptr>(mesh->indices.size() * sizeof(std::uint32_t)),
      mesh->indices.data(),
      GL_STATIC_DRAW);

   glBindVertexArray(0);
   meshUploaded_ = true;
}

float VolumeViewWidget::Impl::GridSpacingKm() const
{
   constexpr std::array<float, 5> kSpacings {5.0f, 10.0f, 25.0f, 50.0f, 100.0f};
   constexpr float                kMaxLinesPerSide = 8.0f;
   for (const float spacing : kSpacings)
   {
      if (halfSizeKm_ / spacing <= kMaxLinesPerSide)
      {
         return spacing;
      }
   }
   return kSpacings.back();
}

void VolumeViewWidget::Impl::UploadGuides()
{
   const auto half = static_cast<float>(halfSizeKm_);

   std::vector<volume::MeshVertex> vertices {};
   auto add = [&vertices](float x, float y, float z, auto color)
   {
      vertices.push_back({.x = x, .y = y, .z = z, .rgba = color});
   };

   // Ground square, two triangles.
   add(-half, -half, 0.0f, kGroundColor_);
   add(half, -half, 0.0f, kGroundColor_);
   add(half, half, 0.0f, kGroundColor_);
   add(-half, -half, 0.0f, kGroundColor_);
   add(half, half, 0.0f, kGroundColor_);
   add(-half, half, 0.0f, kGroundColor_);
   groundVertexCount_ = static_cast<GLsizei>(vertices.size());

   // Grid lines through the centre, then outward.
   const float spacing = GridSpacingKm();
   for (int i = 0; static_cast<float>(i) * spacing < half; ++i)
   {
      const float d = static_cast<float>(i) * spacing;
      for (const float v : {d, -d})
      {
         add(v, -half, 0.0f, kGridColor_);
         add(v, half, 0.0f, kGridColor_);
         add(-half, v, 0.0f, kGridColor_);
         add(half, v, 0.0f, kGridColor_);
      }
   }

   // Region edge.
   const std::array<std::array<float, 2>, 4> corners {
      {{-half, -half}, {half, -half}, {half, half}, {-half, half}}};
   for (std::size_t i = 0; i < corners.size(); ++i)
   {
      const auto& a = corners.at(i);
      const auto& b = corners.at((i + 1) % corners.size());
      add(a[0], a[1], 0.0f, kEdgeColor_);
      add(b[0], b[1], 0.0f, kEdgeColor_);
   }

   // Height axis on the south-west corner, with ticks.
   add(-half, -half, 0.0f, kAxisColor_);
   add(-half, -half, kAxisTopKm_, kAxisColor_);
   const float tick = std::max(1.0f, half * 0.02f);
   for (int i = 1; i <= kAxisTickCount_; ++i)
   {
      const float z = static_cast<float>(i) * kAxisTickKm_;
      add(-half, -half, z, kAxisColor_);
      add(-half + tick, -half, z, kAxisColor_);
   }

   lineVertexCount_ =
      static_cast<GLsizei>(vertices.size()) - groundVertexCount_;

   glBindVertexArray(guideVao_);
   glBindBuffer(GL_ARRAY_BUFFER, guideVbo_);
   glBufferData(
      GL_ARRAY_BUFFER,
      static_cast<GLsizeiptr>(vertices.size() * sizeof(volume::MeshVertex)),
      vertices.data(),
      GL_STATIC_DRAW);
   SetVertexLayout();
   glBindVertexArray(0);

   guidesDirty_ = false;
}

glm::mat4 VolumeViewWidget::Impl::ViewProjection() const
{
   const float     yaw   = glm::radians(yawDeg_);
   const float     pitch = glm::radians(pitchDeg_);
   const glm::vec3 target {target_.x, target_.y, target_.z};
   const glm::vec3 eye =
      target + distanceKm_ * glm::vec3 {std::cos(pitch) * std::sin(yaw),
                                        std::cos(pitch) * std::cos(yaw),
                                        std::sin(pitch)};

   const float aspect = static_cast<float>(std::max(1, self_->width())) /
                        static_cast<float>(std::max(1, self_->height()));
   const glm::mat4 projection =
      glm::perspective(glm::radians(kFieldOfViewDeg_),
                       aspect,
                       std::max(0.05f, distanceKm_ * 0.005f),
                       distanceKm_ * 20.0f);
   const glm::mat4 view = glm::lookAt(eye, target, glm::vec3 {0, 0, 1});
   return projection * view;
}

void VolumeViewWidget::paintGL()
{
   glClearColor(kClearColor_[0], kClearColor_[1], kClearColor_[2], 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   const glm::mat4 mvp = p->ViewProjection();

   if (p->glReady_ && p->shader_ != nullptr)
   {
      if (p->pendingMesh_ != nullptr)
      {
         p->UploadMesh();
      }
      if (p->guidesDirty_)
      {
         p->UploadGuides();
      }

      p->shader_->Use();
      glUniformMatrix4fv(p->uMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));
      glUniform1f(p->uVerticalScaleLocation_, p->verticalScale_);

      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LEQUAL);
      glDisable(GL_CULL_FACE);
      glDepthMask(GL_TRUE);
      glDisable(GL_BLEND);

      // Ground, pushed back so the grid lines on it win the depth test.
      glUniform1f(p->uOpacityLocation_, 1.0f);
      glBindVertexArray(p->guideVao_);
      glEnable(GL_POLYGON_OFFSET_FILL);
      glPolygonOffset(1.0f, 1.0f);
      glDrawArrays(GL_TRIANGLES, 0, p->groundVertexCount_);
      glDisable(GL_POLYGON_OFFSET_FILL);
      glDrawArrays(GL_LINES, p->groundVertexCount_, p->lineVertexCount_);

      if (p->meshUploaded_ && !p->tilts_.empty())
      {
         const std::size_t tiltCount =
            std::min(p->visibleTiltCount_, p->tilts_.size());
         if (tiltCount > 0)
         {
            const auto&         last       = p->tilts_.at(tiltCount - 1);
            const std::uint32_t indexCount = last.firstIndex + last.indexCount;

            const bool opaque = p->opacity_ >= kOpaqueThreshold_;
            if (!opaque)
            {
               // Translucent tilts are not sorted, so they do not hide one
               // another; depth writes are off for them.
               glEnable(GL_BLEND);
               glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
               glDepthMask(GL_FALSE);
            }
            glUniform1f(p->uOpacityLocation_, p->opacity_);
            glBindVertexArray(p->meshVao_);
            glDrawElements(GL_TRIANGLES,
                           static_cast<GLsizei>(indexCount),
                           GL_UNSIGNED_INT,
                           nullptr);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
         }
      }

      glBindVertexArray(0);
      glDisable(GL_DEPTH_TEST);
   }

   QPainter painter {this};
   p->DrawLabels(painter, mvp);
}

void VolumeViewWidget::Impl::DrawLabels(QPainter& painter, const glm::mat4& mvp)
{
   painter.setRenderHint(QPainter::TextAntialiasing);

   const auto w       = static_cast<float>(self_->width());
   const auto h       = static_cast<float>(self_->height());
   auto       project = [&](float x, float y, float z) -> std::optional<QPointF>
   {
      const glm::vec4 clip = mvp * glm::vec4 {x, y, z * verticalScale_, 1.0f};
      if (clip.w <= 0.0f)
      {
         return std::nullopt;
      }
      const glm::vec3 ndc = glm::vec3 {clip} / clip.w;
      if (std::abs(ndc.x) > kLabelClipNdc_ || std::abs(ndc.y) > kLabelClipNdc_)
      {
         return std::nullopt;
      }
      return QPointF {(ndc.x + 1.0f) / 2 * w, (1.0f - ndc.y) / 2 * h};
   };

   const auto half = static_cast<float>(halfSizeKm_);

   painter.setPen(QColor::fromRgb(kAxisLabelColor_));
   for (int i = 1; i <= kAxisTickCount_; ++i)
   {
      const float z = static_cast<float>(i) * kAxisTickKm_;
      if (const auto pt = project(-half, -half, z))
      {
         painter.drawText(
            QPointF {pt->x() + kLabelMarginPx_, pt->y() + kLabelMarginPx_ / 2},
            QString("%1 km").arg(z, 0, 'f', 0));
      }
   }

   painter.setPen(QColor::fromRgb(kGridLabelColor_));
   if (const auto pt = project(0.0f, -half, 0.0f))
   {
      painter.drawText(QPointF {pt->x(), pt->y() + kLabelMarginPx_ * 3},
                       QString("%1 km grid").arg(GridSpacingKm(), 0, 'f', 0));
   }
   if (const auto pt = project(0.0f, half, 0.0f))
   {
      painter.drawText(QPointF {pt->x(), pt->y() - kLabelMarginPx_},
                       QStringLiteral("N"));
   }

   if (!caption_.isEmpty())
   {
      painter.setPen(QColor::fromRgb(kCaptionColor_));
      painter.drawText(QRectF {kLabelMarginPx_,
                               kLabelMarginPx_,
                               w - 2 * kLabelMarginPx_,
                               h - 2 * kLabelMarginPx_},
                       Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                       caption_);
   }
}

void VolumeViewWidget::mousePressEvent(QMouseEvent* event)
{
   p->lastMousePos_ = event->position();
   p->dragButton_   = event->button();
   event->accept();
}

void VolumeViewWidget::mouseMoveEvent(QMouseEvent* event)
{
   const QPointF delta = event->position() - p->lastMousePos_;
   p->lastMousePos_    = event->position();

   if (p->dragButton_ == Qt::LeftButton)
   {
      p->yawDeg_ -= static_cast<float>(delta.x()) * kOrbitDegPerPixel_;
      p->pitchDeg_ = std::clamp(p->pitchDeg_ + static_cast<float>(delta.y()) *
                                                  kOrbitDegPerPixel_,
                                kMinPitchDeg_,
                                kMaxPitchDeg_);
      update();
   }
   else if (p->dragButton_ == Qt::RightButton ||
            p->dragButton_ == Qt::MiddleButton)
   {
      // Pan in the ground plane, scaled so the ground under the cursor
      // roughly follows it.
      const float kmPerPixel = 2.0f * p->distanceKm_ *
                               std::tan(glm::radians(kFieldOfViewDeg_) / 2.0f) /
                               static_cast<float>(std::max(1, height()));
      const float yaw = glm::radians(p->yawDeg_);
      // Screen right and screen up, projected onto the ground.
      const glm::vec2 right {-std::cos(yaw), std::sin(yaw)};
      const glm::vec2 forward {-std::sin(yaw), -std::cos(yaw)};
      const glm::vec2 move = (-static_cast<float>(delta.x()) * right +
                              static_cast<float>(delta.y()) * forward) *
                             kmPerPixel;
      p->target_.x += move.x;
      p->target_.y += move.y;
      update();
   }
   event->accept();
}

void VolumeViewWidget::mouseReleaseEvent(QMouseEvent* event)
{
   p->dragButton_ = Qt::NoButton;
   event->accept();
}

void VolumeViewWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
   ResetCamera();
   event->accept();
}

void VolumeViewWidget::wheelEvent(QWheelEvent* event)
{
   const float steps =
      static_cast<float>(event->angleDelta().y()) / kWheelDeltaPerStep_;
   p->distanceKm_ =
      std::clamp(p->distanceKm_ * std::pow(kZoomPerWheelStep_, steps),
                 kMinDistanceKm_,
                 static_cast<float>(p->halfSizeKm_) * kMaxDistanceScale_);
   update();
   event->accept();
}

} // namespace scwx::qt::ui
