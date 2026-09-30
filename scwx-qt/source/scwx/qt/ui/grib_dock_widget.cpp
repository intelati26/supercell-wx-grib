#include <scwx/qt/ui/grib_dock_widget.hpp>
#include <scwx/qt/manager/grib_manager.hpp>
#include <scwx/qt/manager/hodograph_manager.hpp>
#include <scwx/qt/map/grib_frame_info.hpp>
#include <scwx/qt/ui/checkable_combo_box.hpp>
#include <scwx/qt/ui/widgets/focused_spin_box.hpp>
#include <scwx/provider/nbm_data_provider.hpp>
#include <scwx/provider/rrfs_data_provider.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace scwx::qt::ui
{

namespace
{

std::string CategoryDisplayName(map::GribCategory category)
{
   switch (category)
   {
   case map::GribCategory::Mrms:
      return "MRMS";
   case map::GribCategory::Rtma:
      return "RTMA";
   case map::GribCategory::Rrfs:
      return "RRFS";
   case map::GribCategory::Nbm:
   default:
      return "NBM";
   }
}

// One entry per hourly cycle covers "today so far" at RRFS's real cadence
// (see RrfsDataProvider's own class comment: it really does cycle 24x/day)
// -- a first guess for how far back a run picker should reasonably let
// someone browse, not tied to any documented retention window. Generated
// purely from wall-clock arithmetic (no S3 listing needed just to populate
// the dropdown) -- picking a cycle whose data isn't actually published yet
// just leaves the previously-shown frame in place, same graceful miss
// FetchArchiveFrameForProduct already has for any other out-of-range time.
constexpr int kRrfsCycleHistoryHours_ = 24;

// One forecast-hour step per tick -- a full 84-step loop (a 6-hourly
// cycle's own max) takes ~42s at this pace, fast enough to actually watch
// evolve without being so fast the frame-by-frame detail blurs together.
constexpr int kRrfsAnimationIntervalMs_ = 500;

// Rough size of one RRFS hour's prslev object (see GribManager's
// PrefetchRrfsForecastHourRange doc) -- only used to tell the user what a
// pick list is about to cost, not for any accounting.
constexpr int kRrfsApproxHourMegabytes_ = 320;

// Hour buttons per grid row: 8 fits the dock width (a full 84h cycle is 11 rows).
constexpr int kRrfsHourButtonColumns_ = 8;

// Where the RRFS Play loop ends until the user picks otherwise -- every
// forecast hour is its own ~320MB download, so looping (and prefetching,
// see GribManager::PrefetchRrfsForecastHourRange()) a 6-hourly cycle's full
// 84 hours costs ~27GB; 18h is ~6GB, and is already the whole range of
// every non-6-hourly cycle.
constexpr int kDefaultRrfsLoopEndHour_ = 18;

// Same reasoning as kRrfsCycleHistoryHours_ -- NBM also cycles hourly.
constexpr int kNbmCycleHistoryHours_ = 24;

struct MapCropPreset
{
   std::string name;
   double      southLatitude;
   double      westLongitude;
   double      northLatitude;
   double      eastLongitude;
};

// SPC's own published mesoscale-analysis sector names (confirmed live,
// 2026-09-26, via https://www.spc.noaa.gov/exper/mesoanalysis/'s own
// sector-map image alt text -- "National", "Northwest", "Southwest",
// "Northern Plains", "Central Plains", "Southern Plains", "Northeast",
// "East Central", "Southeast", "Midwest") -- but the bounds below are
// only approximate. SPC's page doesn't expose the sectors' own numeric
// lat/lon bounds directly (only pixel coordinates in an HTML image map),
// so these were derived by linearly transforming that image map's own
// sector polygons against a standard CONUS bounding box, not scraped
// precisely from an authoritative source. Good enough for a "jump the
// view here" convenience preset, not meant to reproduce SPC's own
// internal analysis grid to the pixel.
const std::vector<MapCropPreset> kMapCropPresets_ {
   {"CONUS", 24.5, -125.0, 49.5, -66.0},
   {"Northwest", 39.2, -123.2, 49.0, -99.3},
   {"Southwest", 32.0, -124.9, 42.3, -99.0},
   {"Northern Plains", 39.1, -106.8, 48.1, -84.4},
   {"Central Plains", 33.8, -106.7, 42.5, -85.0},
   {"Southern Plains", 28.0, -108.7, 38.8, -83.0},
   {"Midwest", 34.6, -100.8, 44.1, -79.2},
   {"Northeast", 38.1, -91.2, 49.0, -67.1},
   {"East Central", 33.4, -90.1, 42.5, -69.6},
   {"Southeast", 28.2, -94.6, 36.8, -73.7},
};

} // namespace

// One category's worth of UI -- a header, a status label, and the
// checkable "which products are active" combo box.
struct CategorySection
{
   map::GribCategory                     category;
   std::shared_ptr<manager::GribManager> gribManager;
   QLabel*                               statusLabel {};
   CheckableComboBox*                    comboBox {};

   // RRFS-only (left null for Mrms/Rtma, which have no forecast-hour axis
   // -- see GribManager::SetRrfsCycle()'s own doc) -- run/cycle picker,
   // forecast-hour slider, and its play/pause animation loop.
   QComboBox*   cycleComboBox {};
   QSlider*     hourSlider {};
   QLabel*      hourLabel {};
   QPushButton* playButton {};
   QTimer*      animationTimer {};

   // Bounds the Play loop (and, via GribManager::SetRrfsLoopRange(),
   // PrefetchRrfsForecastHourRange()'s own footprint) to a sub-range of
   // [0, MaxRrfsForecastHour()] instead of always the whole cycle -- e.g.
   // "just loop 0-6h" costs proportionally less to prefetch/cache than
   // the full 84h a 6-hourly cycle can reach. Both default to the full
   // range (see BuildSection's own setup), so leaving them alone
   // preserves the original unbounded behavior.
   QSpinBox* loopStartSpinBox {};
   QSpinBox* loopEndSpinBox {};

   // Hour pick list (RRFS only). Clicking an hour button views that hour
   // and toggles it in the list; Play then downloads only the picked,
   // not-yet-cached hours and loops through exactly those. Kept per cycle
   // (keyed by the cycle's epoch ticks) so it survives switching products
   // -- the hour files are per cycle, not per product -- but doesn't leak
   // a run's picks onto a different run. With nothing picked, Play falls
   // back to the loop-range spin boxes above.
   QWidget*                         hourButtonHost {};
   QGridLayout*                     hourGrid {};
   std::vector<QPushButton*>        hourButtons;
   QPushButton*                     addRangeButton {};
   QPushButton*                     clearPicksButton {};
   QLabel*                          pickSummaryLabel {};
   std::map<qint64, std::set<int>>  picksByCycle;

   // Whether the user has picked a loop end themselves -- until then,
   // RerangeRrfsLoop() keeps it at the default (see
   // kDefaultRrfsLoopEndHour_) as the cycle's max hour changes.
   bool loopEndUserSet {false};

   // Captures the map view plus this section's own current product as a
   // PNG (see GribDockWidget::ExportSnapshotRequested).
   QPushButton* exportButton {};
};

// Applies a new max forecast hour (a product was first checked, or the
// cycle changed) to the RRFS loop-range spinboxes and GribManager. The
// spinboxes' own valueChanged handlers are blocked throughout, so they
// only ever see real user edits (see loopEndUserSet).
void RerangeRrfsLoop(CategorySection& section, int maxHour)
{
   {
      const QSignalBlocker blockStart(section.loopStartSpinBox);
      const QSignalBlocker blockEnd(section.loopEndSpinBox);

      section.loopStartSpinBox->setRange(0, maxHour);
      section.loopEndSpinBox->setRange(0, maxHour);
      if (!section.loopEndUserSet)
      {
         section.loopEndSpinBox->setValue(
            std::min(maxHour, kDefaultRrfsLoopEndHour_));
      }

      // Re-apply the start <= end cross-clamp setRange() just widened.
      section.loopStartSpinBox->setMaximum(section.loopEndSpinBox->value());
      section.loopEndSpinBox->setMinimum(section.loopStartSpinBox->value());
   }

   section.gribManager->SetRrfsLoopRange(section.loopStartSpinBox->value(),
                                         section.loopEndSpinBox->value());
}

class GribDockWidget::Impl
{
public:
   explicit Impl(GribDockWidget* self) : self_ {self} {}
   ~Impl() = default;

   void BuildSection(map::GribCategory category,
                     QVBoxLayout*      parentLayout,
                     QWidget*          dockContents);
   void RefreshSection(CategorySection& section);

   // RRFS hour pick list -- see CategorySection::picksByCycle.
   [[nodiscard]] std::set<int>& Picks(CategorySection& section);
   [[nodiscard]] std::vector<int> PlaybackHours(CategorySection& section);
   void RebuildHourButtons(CategorySection& section);
   void RefreshHourButtons(CategorySection& section);

   // Static (no live data, no manager, no signal wiring) -- just a color
   // key for HodographLayer's height-band coloring plus a range-ring
   // note, read from manager::HodographManager::HeightBands() (the same
   // shared table HodographLayer itself colors segments from) so this
   // can never show a color that doesn't match what's actually drawn on
   // the map.
   void BuildHodographLegend(QVBoxLayout* parentLayout, QWidget* dockContents);

   // A row of named preset buttons, each emitting MapBoundsRequested()
   // with that preset's own bounds (see kMapCropPresets_) -- map-wide, not
   // per-category, so built once rather than per BuildSection() call.
   void BuildMapCropPanel(QVBoxLayout* parentLayout, QWidget* dockContents);

   GribDockWidget*              self_;
   std::vector<CategorySection> sections_;
};

void GribDockWidget::Impl::BuildSection(map::GribCategory category,
                                        QVBoxLayout*      parentLayout,
                                        QWidget*          dockContents)
{
   CategorySection section;
   section.category    = category;
   section.gribManager = manager::GribManager::Instance(category);

   auto* groupBox = new QGroupBox(
      QString::fromStdString(CategoryDisplayName(category)), dockContents);
   auto* groupLayout = new QVBoxLayout(groupBox);

   section.statusLabel = new QLabel(tr("(no frame loaded)"), groupBox);
   section.statusLabel->setWordWrap(true);
   section.statusLabel->setTextInteractionFlags(
      Qt::TextInteractionFlag::TextSelectableByMouse);
   groupLayout->addWidget(section.statusLabel);

   section.comboBox = new CheckableComboBox(groupBox);
   section.comboBox->SetItems(section.gribManager->ProductNames());
   for (const auto& name : section.gribManager->ActiveProductNames())
   {
      section.comboBox->SetChecked(name, true);
   }
   groupLayout->addWidget(section.comboBox);

   section.exportButton = new QPushButton(tr("Export PNG..."), groupBox);
   groupLayout->addWidget(section.exportButton);

   if (category == map::GribCategory::Rrfs)
   {
      section.cycleComboBox = new QComboBox(groupBox);
      section.cycleComboBox->addItem(tr("Latest"), QVariant());

      const auto now = std::chrono::floor<std::chrono::hours>(
         std::chrono::system_clock::now());
      for (int i = 0; i < kRrfsCycleHistoryHours_; ++i)
      {
         const auto cycleTime = now - std::chrono::hours {i};
         const int  maxHour =
            provider::RrfsDataProvider::MaxForecastHourForCycle(cycleTime);
         const std::string label = fmt::format(
            "{:%Y-%m-%d %H}z ({}h)", fmt::gmtime(cycleTime), maxHour);
         section.cycleComboBox->addItem(
            QString::fromStdString(label),
            QVariant::fromValue<qint64>(cycleTime.time_since_epoch().count()));
      }
      groupLayout->addWidget(section.cycleComboBox);

      auto* hourRow            = new QHBoxLayout();
      section.hourLabel        = new QLabel(tr("F000"), groupBox);
      section.hourSlider       = new QSlider(Qt::Horizontal, groupBox);
      const int initialMaxHour = section.gribManager->MaxRrfsForecastHour();
      section.hourSlider->setRange(0, initialMaxHour);
      section.playButton = new QPushButton(tr("Play"), groupBox);
      hourRow->addWidget(section.hourLabel);
      hourRow->addWidget(section.hourSlider);
      hourRow->addWidget(section.playButton);
      groupLayout->addLayout(hourRow);

      // Bounds the Play loop to a sub-range instead of always [0,
      // MaxRrfsForecastHour()] -- defaults to 0 to kDefaultRrfsLoopEndHour_
      // (see RerangeRrfsLoop()) until the user picks otherwise.
      auto* loopRow            = new QHBoxLayout();
      auto* loopLabel          = new QLabel(tr("Loop:"), groupBox);
      section.loopStartSpinBox = new QFocusedSpinBox(groupBox);
      section.loopStartSpinBox->setRange(0, initialMaxHour);
      section.loopStartSpinBox->setValue(0);
      auto* loopToLabel      = new QLabel(tr("to"), groupBox);
      section.loopEndSpinBox = new QFocusedSpinBox(groupBox);
      section.loopEndSpinBox->setRange(0, initialMaxHour);
      section.loopEndSpinBox->setValue(initialMaxHour);
      auto* loopUnitsLabel = new QLabel(tr("h"), groupBox);
      loopRow->addWidget(loopLabel);
      loopRow->addWidget(section.loopStartSpinBox);
      loopRow->addWidget(loopToLabel);
      loopRow->addWidget(section.loopEndSpinBox);
      loopRow->addWidget(loopUnitsLabel);
      section.addRangeButton = new QPushButton(tr("Pick range"), groupBox);
      section.addRangeButton->setToolTip(
         tr("Add every hour from the loop start to the loop end to the pick "
            "list"));
      loopRow->addWidget(section.addRangeButton);
      groupLayout->addLayout(loopRow);

      section.hourButtonHost = new QWidget(groupBox);
      section.hourGrid       = new QGridLayout(section.hourButtonHost);
      section.hourGrid->setContentsMargins(0, 0, 0, 0);
      section.hourGrid->setSpacing(2);
      groupLayout->addWidget(section.hourButtonHost);

      auto* pickRow            = new QHBoxLayout();
      section.pickSummaryLabel = new QLabel(groupBox);
      section.pickSummaryLabel->setWordWrap(true);
      section.clearPicksButton = new QPushButton(tr("Clear"), groupBox);
      pickRow->addWidget(section.pickSummaryLabel, 1);
      pickRow->addWidget(section.clearPicksButton);
      groupLayout->addLayout(pickRow);

      RerangeRrfsLoop(section, initialMaxHour);

      section.animationTimer = new QTimer(self_);
      section.animationTimer->setInterval(kRrfsAnimationIntervalMs_);
   }
   else if (category == map::GribCategory::Nbm)
   {
      // Same shape as the Rrfs block above, minus the loop-range
      // spinboxes and HodographManager coupling -- neither applies here
      // (see GribManager::SetNbmCycle()'s own doc: no loop-range
      // equivalent, and NBM doesn't feed the hodograph).
      section.cycleComboBox = new QComboBox(groupBox);
      section.cycleComboBox->addItem(tr("Latest"), QVariant());

      const auto now = std::chrono::floor<std::chrono::hours>(
         std::chrono::system_clock::now());
      for (int i = 0; i < kNbmCycleHistoryHours_; ++i)
      {
         const auto cycleTime = now - std::chrono::hours {i};
         const int  maxHour =
            provider::NbmDataProvider::MaxForecastHourForCycle(cycleTime);
         const std::string label = fmt::format(
            "{:%Y-%m-%d %H}z ({}h)", fmt::gmtime(cycleTime), maxHour);
         section.cycleComboBox->addItem(
            QString::fromStdString(label),
            QVariant::fromValue<qint64>(cycleTime.time_since_epoch().count()));
      }
      groupLayout->addWidget(section.cycleComboBox);

      auto* hourRow      = new QHBoxLayout();
      section.hourLabel  = new QLabel(tr("F001"), groupBox);
      section.hourSlider = new QSlider(Qt::Horizontal, groupBox);
      // Starts at 1, not 0 -- NBM has no F000 file at all (see
      // NbmDataProvider's own kMinForecastHour_ comment).
      section.hourSlider->setRange(1, section.gribManager->MaxNbmForecastHour());
      section.playButton = new QPushButton(tr("Play"), groupBox);
      hourRow->addWidget(section.hourLabel);
      hourRow->addWidget(section.hourSlider);
      hourRow->addWidget(section.playButton);
      groupLayout->addLayout(hourRow);

      section.animationTimer = new QTimer(self_);
      section.animationTimer->setInterval(kRrfsAnimationIntervalMs_);
   }

   parentLayout->addWidget(groupBox);

   // sections_ must already have room for this push_back not to
   // reallocate -- see the constructor's reserve() -- since the lambdas
   // just below capture `stored` (a reference into this vector) for the
   // life of the connection, and a reallocation would dangle every
   // earlier section's capture, not just this one's.
   sections_.push_back(section);
   CategorySection& stored = sections_.back();

   if (category == map::GribCategory::Rrfs)
   {
      RebuildHourButtons(stored);
   }

   // Reconciles the combo box's checked set onto GribManager rather than
   // trying to diff "what changed" from CheckedItemsChanged alone (which
   // doesn't say which item toggled) -- SetProductActive is a no-op for
   // any product already in the requested state, so this is cheap even
   // though it re-touches every product on every toggle.
   connect(section.comboBox,
           &CheckableComboBox::CheckedItemsChanged,
           self_,
           [this, &stored, category]()
           {
              for (const auto& name : stored.gribManager->ProductNames())
              {
                 stored.gribManager->SetProductActive(
                    name, stored.comboBox->IsChecked(name));
              }

              // Rrfs/Nbm start with no product active (see GribManager's
              // own per-category default), so the hour slider/loop range
              // spinboxes are all still at their degenerate construction-
              // time [0,0] (or [1,0], for Nbm) range until a first
              // product gets checked here -- MaxRrfsForecastHour()/
              // MaxNbmForecastHour() can only resolve a real cycle once
              // something is actually active. Re-range now, the same way
              // the cycle combo box's own handler below already does
              // whenever the cycle changes.
              if (category == map::GribCategory::Rrfs)
              {
                 const int maxHour = stored.gribManager->MaxRrfsForecastHour();
                 stored.hourSlider->setRange(0, maxHour);
                 RerangeRrfsLoop(stored, maxHour);
                 RebuildHourButtons(stored);
              }
              else if (category == map::GribCategory::Nbm)
              {
                 stored.hourSlider->setRange(
                    1, stored.gribManager->MaxNbmForecastHour());
              }
           });

   connect(stored.exportButton,
           &QPushButton::clicked,
           self_,
           [this, category]()
           {
              Q_EMIT self_->ExportSnapshotRequested(
                 category, QString::fromStdString(CategoryDisplayName(category)));
           });

   connect(stored.gribManager.get(),
           &manager::GribManager::FrameReady,
           self_,
           [this, &stored](std::size_t) { RefreshSection(stored); });

   // Toggling a product changes the primary product (or leaves none)
   // before any new frame arrives -- refresh now rather than leaving the
   // previous primary's name/valid time up until the next FrameReady.
   connect(stored.gribManager.get(),
           &manager::GribManager::ActiveProductsChanged,
           self_,
           [this, &stored]() { RefreshSection(stored); });

   if (category == map::GribCategory::Rrfs)
   {
      // Reselecting a cycle re-ranges the hour slider too -- a 3-hourly
      // (non-6-hourly) cycle only reaches F018, not F084, and QSlider
      // clamps the current value into a shrunk range on its own (emitting
      // valueChanged if it had to), so no separate reset is needed here.
      connect(
         stored.cycleComboBox,
         qOverload<int>(&QComboBox::currentIndexChanged),
         self_,
         [this, &stored](int cycleIndex)
         {
            // Never creates one -- with no hodograph layer alive there is
            // nothing to forward this to (see InstanceIfExists()).
            const auto hodographManager =
               manager::HodographManager::InstanceIfExists();

            if (cycleIndex <= 0)
            {
               stored.gribManager->UseLatestRrfsCycle();
               if (hodographManager)
               {
                  hodographManager->UseLatestCycle();
               }
            }
            else
            {
               const qint64 ticks =
                  stored.cycleComboBox->itemData(cycleIndex).value<qint64>();
               const auto cycleTime = std::chrono::system_clock::time_point {
                  std::chrono::system_clock::duration {ticks}};
               stored.gribManager->SetRrfsCycle(cycleTime);
               if (hodographManager)
               {
                  hodographManager->SetCycle(cycleTime);
               }
            }
            stored.hourSlider->setRange(
               0, stored.gribManager->MaxRrfsForecastHour());

            // Same reasoning as hourSlider's own re-range above -- a
            // shrunk max (e.g. a 3-hourly cycle capping at F018) clamps
            // both spinboxes' current values automatically via Qt's own
            // setRange(); GribManager needs telling explicitly, since it
            // doesn't watch these spinboxes itself.
            RerangeRrfsLoop(stored, stored.gribManager->MaxRrfsForecastHour());
            RebuildHourButtons(stored);
         });

      connect(stored.hourSlider,
              &QSlider::valueChanged,
              self_,
              [this, &stored](int hour)
              {
                 stored.gribManager->SetRrfsForecastHour(hour);
                 if (const auto hodographManager =
                        manager::HodographManager::InstanceIfExists())
                 {
                    hodographManager->SetForecastHour(hour);
                 }
                 stored.hourLabel->setText(
                    QString::fromStdString(fmt::format("F{:03d}", hour)));
                 RefreshHourButtons(stored);
              });

      connect(stored.gribManager.get(),
              &manager::GribManager::RrfsCacheChanged,
              self_,
              [this, &stored]() { RefreshHourButtons(stored); });

      connect(stored.addRangeButton,
              &QPushButton::clicked,
              self_,
              [this, &stored]()
              {
                 auto& picks = Picks(stored);
                 for (int hour = stored.loopStartSpinBox->value();
                      hour <= stored.loopEndSpinBox->value();
                      ++hour)
                 {
                    picks.insert(hour);
                 }
                 RefreshHourButtons(stored);
              });

      connect(stored.clearPicksButton,
              &QPushButton::clicked,
              self_,
              [this, &stored]()
              {
                 Picks(stored).clear();
                 RefreshHourButtons(stored);
              });

      // Cross-clamped so start can never exceed end or vice versa --
      // standard two-spinbox range idiom (each bounds the *other's* own
      // range, not just its value) -- rather than validating and
      // rejecting an inverted range after the fact.
      connect(stored.loopStartSpinBox,
              qOverload<int>(&QSpinBox::valueChanged),
              self_,
              [&stored](int start)
              {
                 stored.loopEndSpinBox->setMinimum(start);
                 stored.gribManager->SetRrfsLoopRange(
                    start, stored.loopEndSpinBox->value());
              });

      connect(stored.loopEndSpinBox,
              qOverload<int>(&QSpinBox::valueChanged),
              self_,
              [&stored](int end)
              {
                 // Only user edits reach here (see RerangeRrfsLoop()).
                 stored.loopEndUserSet = true;
                 stored.loopStartSpinBox->setMaximum(end);
                 stored.gribManager->SetRrfsLoopRange(
                    stored.loopStartSpinBox->value(), end);
              });

      connect(stored.playButton,
              &QPushButton::clicked,
              self_,
              [this, &stored]()
              {
                 if (stored.animationTimer->isActive())
                 {
                    stored.animationTimer->stop();
                    stored.playButton->setText(tr("Play"));
                 }
                 else
                 {
                    const std::vector<int> hours = PlaybackHours(stored);
                    if (hours.empty())
                    {
                       return;
                    }

                    // Starting on an hour that isn't part of the loop (e.g.
                    // the slider was left at F040 while the picks are 2-6)
                    // would otherwise show it until the timer happened to
                    // wrap -- snap in first.
                    if (!std::binary_search(
                           hours.begin(), hours.end(), stored.hourSlider->value()))
                    {
                       stored.hourSlider->setValue(hours.front());
                    }

                    // This is the deferred download: only now, and only
                    // for the picked hours not already cached. No separate
                    // HodographManager prefetch needed: it decodes from the
                    // exact same downloaded GRIB2 file, so it benefits once
                    // this fills the shared cache.
                    stored.gribManager->PrefetchRrfsForecastHours(
                       std::set<int>(hours.begin(), hours.end()));
                    RefreshHourButtons(stored);
                    stored.animationTimer->start();
                    stored.playButton->setText(tr("Pause"));
                 }
              });

      connect(stored.animationTimer,
              &QTimer::timeout,
              self_,
              [this, &stored]()
              {
                 // Wraps to the first hour rather than stopping at the
                 // last -- "keep watching the run evolve" is the point of
                 // Play.
                 const std::vector<int> hours = PlaybackHours(stored);
                 if (hours.empty())
                 {
                    return;
                 }
                 const auto it = std::upper_bound(
                    hours.begin(), hours.end(), stored.hourSlider->value());
                 stored.hourSlider->setValue(it == hours.end() ? hours.front() :
                                                                  *it);
              });
   }
   else if (category == map::GribCategory::Nbm)
   {
      connect(
         stored.cycleComboBox,
         qOverload<int>(&QComboBox::currentIndexChanged),
         self_,
         [&stored](int cycleIndex)
         {
            if (cycleIndex <= 0)
            {
               stored.gribManager->UseLatestNbmCycle();
            }
            else
            {
               const qint64 ticks =
                  stored.cycleComboBox->itemData(cycleIndex).value<qint64>();
               const auto cycleTime = std::chrono::system_clock::time_point {
                  std::chrono::system_clock::duration {ticks}};
               stored.gribManager->SetNbmCycle(cycleTime);
            }
            stored.hourSlider->setRange(
               1, stored.gribManager->MaxNbmForecastHour());

            // setRange() only fires valueChanged if it had to clamp the
            // value into the new range -- re-snap explicitly too, since a
            // value that stayed numerically in-range can still land in a
            // step gap under the *new* cycle's own hourly/3-hourly/
            // 6-hourly rule (see SetNbmForecastHour()'s own doc).
            stored.gribManager->SetNbmForecastHour(stored.hourSlider->value());
            const int actualHour = stored.gribManager->NbmForecastHour();
            stored.hourSlider->setValue(actualHour);
            stored.hourLabel->setText(
               QString::fromStdString(fmt::format("F{:03d}", actualHour)));
         });

      connect(stored.hourSlider,
              &QSlider::valueChanged,
              self_,
              [&stored](int hour)
              {
                 stored.gribManager->SetNbmForecastHour(hour);

                 // NBM's own forecast-hour step is non-uniform (see
                 // SetNbmForecastHour()'s own doc) -- what actually got
                 // stored may differ from the raw slider position, so
                 // read it back and snap the slider (and label) to match
                 // rather than showing a value that wasn't really
                 // fetched. setValue() only re-emits valueChanged if this
                 // changes the value, and re-entering with an
                 // already-valid hour is a harmless no-op the second
                 // time.
                 const int actualHour = stored.gribManager->NbmForecastHour();
                 if (actualHour != hour)
                 {
                    stored.hourSlider->setValue(actualHour);
                 }
                 stored.hourLabel->setText(
                    QString::fromStdString(fmt::format("F{:03d}", actualHour)));
              });

      connect(stored.playButton,
              &QPushButton::clicked,
              self_,
              [&stored]()
              {
                 if (stored.animationTimer->isActive())
                 {
                    stored.animationTimer->stop();
                    stored.playButton->setText(tr("Play"));
                 }
                 else
                 {
                    stored.animationTimer->start();
                    stored.playButton->setText(tr("Pause"));
                 }
              });

      connect(stored.animationTimer,
              &QTimer::timeout,
              self_,
              [&stored]()
              {
                 int next = stored.hourSlider->value() + 1;
                 if (next > stored.hourSlider->maximum())
                 {
                    next = stored.hourSlider->minimum(); // 1, not 0 -- no F000
                 }
                 stored.hourSlider->setValue(next);
              });
   }

   RefreshSection(stored);
}

std::set<int>& GribDockWidget::Impl::Picks(CategorySection& section)
{
   const qint64 cycleKey =
      section.gribManager->CurrentRrfsCycle().time_since_epoch().count();
   return section.picksByCycle[cycleKey];
}

std::vector<int> GribDockWidget::Impl::PlaybackHours(CategorySection& section)
{
   const int        maxHour = section.gribManager->MaxRrfsForecastHour();
   std::vector<int> hours;

   for (const int hour : Picks(section))
   {
      if (hour <= maxHour)
      {
         hours.push_back(hour);
      }
   }

   if (hours.empty())
   {
      for (int hour = section.loopStartSpinBox->value();
           hour <= section.loopEndSpinBox->value();
           ++hour)
      {
         hours.push_back(hour);
      }
   }

   return hours;
}

void GribDockWidget::Impl::RebuildHourButtons(CategorySection& section)
{
   const int hourCount = section.gribManager->MaxRrfsForecastHour() + 1;

   if (static_cast<int>(section.hourButtons.size()) != hourCount)
   {
      for (auto* button : section.hourButtons)
      {
         section.hourGrid->removeWidget(button);
         delete button;
      }
      section.hourButtons.clear();

      // A cycle that's gone (nothing active yet) has one degenerate hour;
      // don't show a lone F000 button for it.
      const bool haveCycle = section.gribManager->CurrentProductIndex().has_value();
      for (int hour = 0; haveCycle && hour < hourCount; ++hour)
      {
         auto* button = new QPushButton(
            QString::fromStdString(fmt::format("{:03d}", hour)),
            section.hourButtonHost);
         button->setFixedWidth(34);
         button->setFlat(false);
         button->setFocusPolicy(Qt::NoFocus);
         button->setContentsMargins(0, 0, 0, 0);
         section.hourGrid->addWidget(
            button, hour / kRrfsHourButtonColumns_, hour % kRrfsHourButtonColumns_);
         section.hourButtons.push_back(button);

         connect(button,
                 &QPushButton::clicked,
                 self_,
                 [this, &section, hour]()
                 {
                    auto& picks = Picks(section);
                    if (!picks.erase(hour))
                    {
                       picks.insert(hour);
                    }
                    // Viewing is the on-demand fetch of just this hour; the
                    // deferred bulk download only starts at Play.
                    section.hourSlider->setValue(hour);
                    RefreshHourButtons(section);
                 });
      }
   }

   RefreshHourButtons(section);
}

void GribDockWidget::Impl::RefreshHourButtons(CategorySection& section)
{
   const std::set<int>& picks   = Picks(section);
   const std::set<int>  cached  = section.gribManager->CachedRrfsForecastHours();
   const int            current = section.hourSlider->value();

   // Three independent cues so any combination stays readable:
   // downloaded = filled, picked = accent border, viewing = bold text.
   for (std::size_t hour = 0; hour < section.hourButtons.size(); ++hour)
   {
      const int  h          = static_cast<int>(hour);
      const bool isCached   = cached.contains(h);
      const bool isPicked   = picks.contains(h);
      const bool isViewing  = (h == current);

      QString style = QStringLiteral("QPushButton { padding: 1px 0px; ");
      style += isCached ? QStringLiteral("background: palette(mid); ") :
                          QStringLiteral("background: palette(button); ");
      style += isPicked ? QStringLiteral("border: 2px solid palette(highlight); ") :
                          QStringLiteral("border: 1px solid palette(mid); ");
      style += isViewing ? QStringLiteral("font-weight: bold; ") : QString();
      style += QStringLiteral("}");
      section.hourButtons[hour]->setStyleSheet(style);
   }

   int toDownload = 0;
   for (const int hour : picks)
   {
      if (hour < static_cast<int>(section.hourButtons.size()) &&
          !cached.contains(hour))
      {
         ++toDownload;
      }
   }

   if (picks.empty())
   {
      section.pickSummaryLabel->setText(
         tr("Click hours to pick them; Play downloads and loops only the "
            "picked ones (or the loop range if none)."));
   }
   else
   {
      section.pickSummaryLabel->setText(
         tr("%1 picked, %2 to download (~%3 MB)")
            .arg(picks.size())
            .arg(toDownload)
            .arg(toDownload * kRrfsApproxHourMegabytes_));
   }
   section.clearPicksButton->setEnabled(!picks.empty());
}

void GribDockWidget::Impl::RefreshSection(CategorySection& section)
{
   const auto activeIndices = section.gribManager->ActiveProductIndices();

   if (activeIndices.empty())
   {
      // Reachable for Rrfs/Nbm, which start (and can be brought back down
      // to) zero active products -- see GribManager's own per-category
      // default. Unreachable for Mrms/Rtma, which keep the original
      // "always at least one" behavior.
      section.statusLabel->setText(tr("(no products active)"));
      return;
   }

   // One line per checked product -- GribProductLayer draws all of them --
   // with its frame's valid time, or that it's still on its way.
   std::string statusText;
   for (const std::size_t index : activeIndices)
   {
      const std::string validTime = map::ReadGribFrameValidTime(
         map::GetGribFramePath(section.category, index));

      if (!statusText.empty())
      {
         statusText += "\n";
      }
      statusText += section.gribManager->ProductName(index) + ": " +
                    (validTime.empty() ? "loading..." : validTime);
   }

   section.statusLabel->setText(QString::fromStdString(statusText));
}

void GribDockWidget::Impl::BuildHodographLegend(QVBoxLayout* parentLayout,
                                                QWidget*     dockContents)
{
   auto* groupBox    = new QGroupBox(tr("Hodograph"), dockContents);
   auto* groupLayout = new QVBoxLayout(groupBox);

   for (const auto& band : manager::HodographManager::HeightBands())
   {
      auto* row = new QHBoxLayout();

      // A small fixed-size colored square -- simplest way to show a
      // solid color swatch with QWidget alone, no custom paint event
      // needed for something this small/static.
      auto* swatch = new QLabel(groupBox);
      swatch->setFixedSize(14, 14);
      swatch->setStyleSheet(
         QString("background-color: rgb(%1, %2, %3); border: 1px solid "
                 "black;")
            .arg(band.rgb[0])
            .arg(band.rgb[1])
            .arg(band.rgb[2]));

      auto* label = new QLabel(QString::fromStdString(band.label), groupBox);

      row->addWidget(swatch);
      row->addWidget(label);
      row->addStretch();

      groupLayout->addLayout(row);
   }

   auto* ringsNote = new QLabel(tr("Range rings: every 20 kt"), groupBox);
   ringsNote->setWordWrap(true);
   groupLayout->addWidget(ringsNote);

   parentLayout->addWidget(groupBox);
}

void GribDockWidget::Impl::BuildMapCropPanel(QVBoxLayout* parentLayout,
                                             QWidget*     dockContents)
{
   auto* groupBox    = new QGroupBox(tr("Map View"), dockContents);
   auto* groupLayout = new QGridLayout(groupBox);

   constexpr int kColumns = 2;
   int           row = 0, col = 0;

   for (const auto& preset : kMapCropPresets_)
   {
      auto* button = new QPushButton(QString::fromStdString(preset.name),
                                     groupBox);

      connect(button,
              &QPushButton::clicked,
              self_,
              [this, preset]()
              {
                 Q_EMIT self_->MapBoundsRequested(preset.southLatitude,
                                                  preset.westLongitude,
                                                  preset.northLatitude,
                                                  preset.eastLongitude);
              });

      groupLayout->addWidget(button, row, col);
      if (++col >= kColumns)
      {
         col = 0;
         ++row;
      }
   }

   parentLayout->addWidget(groupBox);
}

GribDockWidget::GribDockWidget(QWidget* parent) :
    QDockWidget(parent), p {std::make_unique<Impl>(this)}
{
   setObjectName("GribDockWidget");
   setWindowTitle(tr("GRIB"));

   auto* contents = new QWidget(this);
   auto* layout   = new QVBoxLayout(contents);

   p->BuildMapCropPanel(layout, contents);

   // Fixed at 4 (Mrms/Rtma/Rrfs/Nbm) -- reserved upfront so BuildSection's
   // own push_back never reallocates mid-construction (see its comment).
   p->sections_.reserve(4);

   p->BuildSection(map::GribCategory::Mrms, layout, contents);
   p->BuildSection(map::GribCategory::Rtma, layout, contents);
   p->BuildSection(map::GribCategory::Rrfs, layout, contents);
   p->BuildSection(map::GribCategory::Nbm, layout, contents);
   p->BuildHodographLegend(layout, contents);

   layout->addStretch();

   // The RRFS hour button grid (up to 85 buttons) makes this taller than a
   // screen, and without a scroll area the layout just squashed every row.
   auto* scrollArea = new QScrollArea(this);
   scrollArea->setWidgetResizable(true);
   scrollArea->setFrameShape(QFrame::NoFrame);
   scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
   scrollArea->setWidget(contents);
   setWidget(scrollArea);
}

GribDockWidget::~GribDockWidget() = default;

} // namespace scwx::qt::ui
