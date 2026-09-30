#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <QObject>

namespace scwx::qt::manager
{

// Companion to GribManager, but simpler and single-purpose: fetches
// RTMA's hourly file (own provider instance, not sharing GribManager's --
// simpler to reason about at the cost of a redundant ~84MB/hour download,
// acceptable given RTMA's cadence) and decodes three fixed fields out of
// it -- 10m wind direction ("10wdir"), speed ("10si"), and gust
// ("i10fg") -- into three frame files, regardless of whatever field the
// user currently has GribManager(Models) showing. WindBarbLayer reads
// all three to plot barbs (gust drawn as a second, red barb underneath
// the normal one -- see GetWindBarbGustTexture's own comment).
//
// Live-only for now (no TimelineManager archive-time subscription like
// GribManager has) -- always fetches whatever the latest RTMA file is.
class WindBarbManager : public QObject
{
   Q_OBJECT

public:
   explicit WindBarbManager();
   ~WindBarbManager();

   WindBarbManager(const WindBarbManager&)            = delete;
   WindBarbManager& operator=(const WindBarbManager&) = delete;
   WindBarbManager(WindBarbManager&&)                 = delete;
   WindBarbManager& operator=(WindBarbManager&&)      = delete;

   static std::shared_ptr<WindBarbManager> Instance();

   [[nodiscard]] static std::string GetWindDirectionFramePath();
   [[nodiscard]] static std::string GetWindSpeedFramePath();
   [[nodiscard]] static std::string GetWindGustFramePath();

   // Bumped once per successful decode of a new RTMA file, just before
   // WindDataReady() fires. WindBarbLayer keys its shared parsed-frame cache
   // on this so every map pane reuses one parsed copy of the three frame
   // files instead of each parsing its own.
   [[nodiscard]] std::uint64_t DataGeneration() const;

signals:
   // Emitted once all three frame files have been refreshed for a new
   // RTMA file -- may fire from a background fetch thread, same as
   // GribManager::FrameReady (Qt's queued cross-thread delivery makes
   // this safe to connect to from another thread).
   void WindDataReady();

private:
   void Poll();

   // Decodes "10wdir", "10si", and "i10fg" out of the already-downloaded
   // GRIB2 payload for `key` into the three frame files, atomically
   // replacing each. Emits WindDataReady() and returns true only if all
   // three succeed. Safe to call from any thread.
   bool ApplyCachedDownload(const std::string& key);

   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::manager
