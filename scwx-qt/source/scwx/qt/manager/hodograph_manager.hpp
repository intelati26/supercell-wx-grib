#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QObject>

namespace scwx::qt::manager
{

// Companion to WindBarbManager, same simpler-than-GribManager shape (own
// provider instance, no product multi-select, no TimelineManager archive
// subscription -- live-only, always whatever RRFS's latest resolved cycle
// is) but for a genuinely different purpose: feeds HodographLayer's
// gridded hodograph, which needs wind at *several heights per point*
// (RTMA -- what WindBarbManager reads -- is 10m-only), so this reads RRFS
// instead (see docs/gridded-hodograph-plan.md).
//
// Decodes u/v at every real discrete height level RRFS's 2dfld file
// carries (see Levels()) plus surface terrain (orog) into one frame file
// each -- a dedicated manager rather than 17 more GribManager(Rrfs)
// ProductConfig rows, since these are inputs to one composite plot, not
// individually user-selectable products (see the plan doc's own "what
// feeds it" note).
//
// Cycle/forecast-hour selection mirrors RrfsDataProvider's own API
// directly (SetCycle()/UseLatestCycle()/SetForecastHour() below just
// forward to the owned provider instance, then re-poll) -- GribDockWidget
// wires its existing RRFS run/hour picker to call these too, so the
// hodograph follows the same selection the user already made for the
// other RRFS layers rather than staying pinned to "latest, F000" forever.
class HodographManager : public QObject
{
   Q_OBJECT

public:
   explicit HodographManager();
   ~HodographManager();

   HodographManager(const HodographManager&)            = delete;
   HodographManager& operator=(const HodographManager&) = delete;
   HodographManager(HodographManager&&)                 = delete;
   HodographManager& operator=(HodographManager&&)      = delete;

   static std::shared_ptr<HodographManager> Instance();

   // The live manager if one already exists, otherwise null -- never
   // creates one. For callers that only want to *forward* state to
   // hodographs if something is using them (the GRIB dock's RRFS cycle/hour
   // pickers): Instance() there would construct a fresh manager per call
   // when no hodograph layer exists, and SetForecastHour()/SetCycle() poll
   // the network immediately.
   static std::shared_ptr<HodographManager> InstanceIfExists();

   // A HodographLayer reports whether it is currently *drawing*
   // hodographs (its zoom tier is visible), keyed by an opaque owner
   // pointer. The manager only polls/downloads/decodes while at least one
   // owner is drawing -- a layer that is enabled but zoomed out (hodographs
   // are not drawn below zoom 6) previously still pulled a ~320MB RRFS file
   // and decoded 35 grids in the background for nothing. The first owner to
   // start drawing triggers an immediate poll.
   void SetDrawing(const void* owner, bool drawing);

   // One real, verified-live RRFS 2dfld wind level. heightMeters_ is
   // always AGL -- for aboveSea_ levels (heightAboveSea in the raw GRIB,
   // i.e. NOT already AGL), HodographLayer must subtract the terrain
   // frame's own value at each point before comparing, the same
   // terrain-relative handling STP's own LCL-AGL term already does (see
   // decode_grib.cpp's ComputeStp) -- a level below terrain at a given
   // point is real (e.g. a Rockies point's "914m ASL" level), not
   // missing data, and must be masked out there, not decoded wrong.
   struct Level
   {
      std::string typeOfLevel;
      long        level;
      float       heightMeters;
      bool        aboveSea;

      // 10m ships as its own distinct shortNames ("10u"/"10v", already
      // unique on their own -- no typeOfLevel/level disambiguation
      // needed); every other level shares the plain "u"/"v" shortName
      // across all of heightAboveGround *and* heightAboveSea, needing
      // typeOfLevel+level to pick the right one (see decode_grib's
      // FindMessage). Baked in per-level here rather than inferred from
      // level==10 elsewhere.
      std::string uShortName;
      std::string vShortName;
   };

   // 17 levels, 10m through ~4.5km -- verified live (2026-09-25) against
   // a real downloaded 2dfld file, see docs/gridded-hodograph-plan.md.
   // Ordered by increasing height -- HodographLayer connects consecutive
   // valid (non-terrain-masked) levels directly, relying on this order.
   [[nodiscard]] static const std::vector<Level>& Levels();

   [[nodiscard]] static std::string GetUFramePath(std::size_t levelIndex);
   [[nodiscard]] static std::string GetVFramePath(std::size_t levelIndex);
   [[nodiscard]] static std::string GetTerrainFramePath();

   // Forward directly to the owned RrfsDataProvider (see its own
   // SetCycle()/UseLatestCycle()/SetForecastHour() doc) and immediately
   // re-poll -- same "just ask Refresh()+FindLatestKey() again" approach
   // GribManager::FetchRrfsSelection() uses, which works here for the
   // same underlying reason: RrfsDataProvider::GetPrefix() always
   // resolves through CurrentCycle()/ForecastHour() regardless of mode,
   // so re-polling after changing either naturally targets the new
   // selection with no separate fetch path needed.
   void SetCycle(std::chrono::system_clock::time_point cycleTime);
   void UseLatestCycle();
   void SetForecastHour(int hour);

   // One height-color band, the shared source of truth for both
   // HodographLayer's own per-segment coloring and GribDockWidget's
   // static legend -- kept here, not duplicated in either, specifically
   // so the two can never drift apart and show a legend that lies about
   // what a color on the map actually means. `maxHeightMeters` is the
   // inclusive upper bound for every entry except the last, which always
   // matches (see BandForHeight()) regardless of its own value.
   struct HeightBand
   {
      float                       maxHeightMeters;
      std::string                 label;
      std::array<std::uint8_t, 3> rgb;
   };

   // Rough SPC mesoanalysis hodograph convention (distinct colors per
   // height band), simplified to 3 bands rather than SPC's usual 4 (0-1/
   // 1-3/3-6/6-9km) -- RRFS's own real height inventory (see Levels()
   // above) only reaches ~4.5km AGL, so a 6-9km band would never have
   // anything to color.
   [[nodiscard]] static const std::vector<HeightBand>& HeightBands();

   // The band a given AGL height falls into, per HeightBands() above.
   [[nodiscard]] static const HeightBand& BandForHeight(float heightMeters);

   // Bumped once per successful decode of a new RRFS file, just before
   // HodographDataReady() fires. HodographLayer keys its shared parsed-frame
   // cache on this, so every map pane can reuse one parsed copy of the
   // frame files instead of each parsing its own.
   [[nodiscard]] std::uint64_t DataGeneration() const;

signals:
   // Emitted once every level's u/v frame plus the terrain frame have all
   // been refreshed for a new RRFS file -- may fire from a background
   // fetch thread, same cross-thread-safe Qt queued delivery as
   // GribManager::FrameReady/WindBarbManager::WindDataReady.
   void HodographDataReady();

private:
   void Poll();

   // Decodes every level's u/v plus orog out of the already-downloaded
   // GRIB2 payload for `key` into their frame files, atomically replacing
   // each. Emits HodographDataReady() and returns true only if every
   // decode succeeds. Safe to call from any thread.
   bool ApplyCachedDownload(const std::string& key);

   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::manager
