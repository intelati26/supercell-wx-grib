#include <scwx/qt/manager/status_manager.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include <QTimer>

namespace scwx::qt::manager
{

namespace
{

// Fixed-unit-step formatter -- every real download in this project (a
// few MB up to RRFS's own ~320MB) stays comfortably within one order of
// magnitude of "megabytes," so there's no need for a general-purpose
// byte-formatting utility that also handles KB/GB.
std::string FormatBytes(std::int64_t bytes)
{
   return fmt::format("{:.1f} MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// How long an entry survives with no fresh ReportProgress() call before
// it's treated as abandoned and pruned. A real backstop, not just
// tidiness: GribManager/WindBarbManager/HodographManager all call
// ReportComplete() explicitly, so this rarely matters for them, but
// network::cpr-based downloads (see its own DownloadProgressCallback
// doc) have no "done" hook to call ReportComplete() from at all -- a
// completed, errored, or timed-out request simply stops invoking the
// progress callback, with nothing else to say so. For those, this is
// the *only* way an entry ever clears.
constexpr std::chrono::seconds kStaleThreshold_ {3};

// A message (see StatusManager::ReportMessage()) is reported once and never
// refreshed, so it stays up long enough to be read rather than for the few
// seconds an abandoned download does.
constexpr std::chrono::seconds kMessageLifetime_ {15};

struct ProgressEntry
{
   std::string  description;
   std::int64_t bytesReceived {};
   std::int64_t totalBytes {-1};

   // A notice rather than a download: shown as its sentence alone, kept for
   // kMessageLifetime_, and not counted as activity.
   bool isMessage {false};

   // Monotonic sequence numbers (std::map's own key order, by id, says nothing
   // about either): when this entry was last reported, and when it first
   // appeared. A download keeps reporting many times a second, so which entry
   // CurrentStatusText() shows must not follow `sequence` -- two concurrent
   // downloads would take turns being the most recent, and the status bar
   // would flip between them on every chunk.
   std::uint64_t sequence {};
   std::uint64_t startSequence {};

   std::chrono::steady_clock::time_point lastUpdate {};
};

// Caller must already hold Impl::mutex_.
void PruneStaleEntries(std::map<std::string, ProgressEntry>& entries)
{
   const auto now = std::chrono::steady_clock::now();
   for (auto it = entries.begin(); it != entries.end();)
   {
      const auto lifetime =
         it->second.isMessage ? kMessageLifetime_ : kStaleThreshold_;
      if (now - it->second.lastUpdate > lifetime)
      {
         it = entries.erase(it);
      }
      else
      {
         ++it;
      }
   }
}

// Notices ahead of downloads, newest notice first; downloads longest-running
// first.
bool DisplaysBefore(const ProgressEntry& a, const ProgressEntry& b)
{
   if (a.isMessage != b.isMessage)
   {
      return a.isMessage;
   }
   return a.isMessage ? a.sequence > b.sequence :
                        a.startSequence < b.startSequence;
}

std::string FormatEntry(const ProgressEntry& entry)
{
   std::string text = entry.description;
   if (!entry.isMessage)
   {
      text += ": " + FormatBytes(entry.bytesReceived);
      if (entry.totalBytes > 0)
      {
         text += " of " + FormatBytes(entry.totalBytes);
      }
   }
   return text;
}

} // namespace

class StatusManager::Impl
{
public:
   std::mutex                           mutex_;
   std::map<std::string, ProgressEntry> entries_;
   std::uint64_t                        nextSequence_ {0};
   QTimer*                              staleCheckTimer_ {nullptr};
};

StatusManager::StatusManager() : p(std::make_unique<Impl>())
{
   // Polling for staleness, not event-driven, because there is nothing
   // to drive an event off of -- a cpr-based download that simply stops
   // (finished, errored, or timed out with no final callback -- see
   // ReportProgress()'s own doc) produces no signal of its own to react
   // to; this is what actually notices "it's been quiet" and lets the
   // status bar clear itself. Every kStaleThreshold_ seconds is frequent
   // enough that clearing feels prompt without polling meaningfully more
   // often than the staleness window itself warrants.
   p->staleCheckTimer_ = new QTimer(this);
   connect(p->staleCheckTimer_,
           &QTimer::timeout,
           this,
           &StatusManager::CheckForStaleEntries);
   p->staleCheckTimer_->start(static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(kStaleThreshold_)
         .count()));
}
StatusManager::~StatusManager() = default;

std::shared_ptr<StatusManager> StatusManager::Instance()
{
   static std::weak_ptr<StatusManager> instanceRef_ {};
   static std::mutex                   instanceMutex_ {};

   std::unique_lock lock(instanceMutex_);

   std::shared_ptr<StatusManager> instance = instanceRef_.lock();
   if (instance == nullptr)
   {
      instance     = std::shared_ptr<StatusManager>(new StatusManager());
      instanceRef_ = instance;
   }

   return instance;
}

void StatusManager::ReportProgress(const std::string& id,
                                   const std::string& description,
                                   std::int64_t       bytesReceived,
                                   std::int64_t       totalBytes)
{
   {
      std::lock_guard lock(p->mutex_);
      const auto [it, isNew] = p->entries_.try_emplace(id);
      auto& entry            = it->second;
      if (isNew || entry.isMessage)
      {
         entry.startSequence = p->nextSequence_++;
      }
      entry.description   = description;
      entry.bytesReceived = bytesReceived;
      entry.totalBytes    = totalBytes;
      entry.isMessage     = false;
      entry.sequence      = p->nextSequence_++;
      entry.lastUpdate    = std::chrono::steady_clock::now();

      // Opportunistic, not on a timer -- keeps the map bounded over a
      // long session without needing a dedicated cleanup thread/timer;
      // any still-active download keeps refreshing its own entry well
      // within kStaleThreshold_, so this never prunes something genuinely
      // in progress.
      PruneStaleEntries(p->entries_);
   }
   Q_EMIT StatusChanged();
}

void StatusManager::ReportMessage(const std::string& id,
                                  const std::string& description)
{
   {
      std::scoped_lock lock(p->mutex_);
      auto&            entry = p->entries_[id];
      entry.description      = description;
      entry.bytesReceived    = 0;
      entry.totalBytes       = -1;
      entry.isMessage        = true;
      entry.sequence         = p->nextSequence_++;
      entry.startSequence    = entry.sequence;
      entry.lastUpdate       = std::chrono::steady_clock::now();

      PruneStaleEntries(p->entries_);
   }
   Q_EMIT StatusChanged();
}

void StatusManager::ReportComplete(const std::string& id)
{
   {
      std::lock_guard lock(p->mutex_);
      p->entries_.erase(id);
   }
   Q_EMIT StatusChanged();
}

std::string StatusManager::CurrentStatusText() const
{
   std::lock_guard lock(p->mutex_);
   PruneStaleEntries(p->entries_);

   if (p->entries_.empty())
   {
      return {};
   }

   // A notice (something went wrong) is shown ahead of any download, newest
   // first. Otherwise the download that has been running longest: it stays on
   // screen until it finishes, then the next takes over, rather than the line
   // changing on every progress report of whichever download spoke last.
   const ProgressEntry* first = nullptr;
   for (const auto& [id, entry] : p->entries_)
   {
      if (first == nullptr || DisplaysBefore(entry, *first))
      {
         first = &entry;
      }
   }

   std::string text = FormatEntry(*first);

   if (p->entries_.size() > 1)
   {
      text += fmt::format(" (+{} more)", p->entries_.size() - 1);
   }

   return text;
}

std::vector<std::string> StatusManager::PendingLines() const
{
   std::lock_guard lock(p->mutex_);
   PruneStaleEntries(p->entries_);

   std::vector<const ProgressEntry*> sorted;
   for (const auto& [id, entry] : p->entries_)
   {
      sorted.push_back(&entry);
   }
   std::sort(sorted.begin(),
             sorted.end(),
             [](const ProgressEntry* a, const ProgressEntry* b)
             { return DisplaysBefore(*a, *b); });

   std::vector<std::string> lines;
   lines.reserve(sorted.size());
   for (const ProgressEntry* entry : sorted)
   {
      lines.push_back(FormatEntry(*entry));
   }
   return lines;
}

bool StatusManager::IsBusy() const
{
   std::lock_guard lock(p->mutex_);
   PruneStaleEntries(p->entries_);
   return std::any_of(p->entries_.cbegin(),
                      p->entries_.cend(),
                      [](const auto& entry)
                      { return !entry.second.isMessage; });
}

void StatusManager::CheckForStaleEntries()
{
   bool changed = false;
   {
      std::lock_guard   lock(p->mutex_);
      const std::size_t sizeBefore = p->entries_.size();
      PruneStaleEntries(p->entries_);
      changed = (p->entries_.size() != sizeBefore);
   }

   if (changed)
   {
      Q_EMIT StatusChanged();
   }
}

} // namespace scwx::qt::manager
