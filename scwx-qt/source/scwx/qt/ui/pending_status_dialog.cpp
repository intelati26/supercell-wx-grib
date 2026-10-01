// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#include <scwx/qt/ui/pending_status_dialog.hpp>
#include <scwx/qt/manager/status_manager.hpp>

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace scwx::qt::ui
{

namespace
{

// StatusManager announces a change on every progress report of every download,
// hundreds of times a second; the list is refreshed at most this often.
constexpr int kRefreshIntervalMs_ = 250;

} // namespace

class PendingStatusDialog::Impl
{
public:
   explicit Impl(PendingStatusDialog* self) : self_ {self} {}

   void BuildUi();
   void ScheduleRefresh();

   PendingStatusDialog* self_;

   std::shared_ptr<manager::StatusManager> statusManager_ {
      manager::StatusManager::Instance()};

   QLabel*      summaryLabel_ {};
   QListWidget* list_ {};
   QTimer*      refreshTimer_ {};
};

void PendingStatusDialog::Impl::BuildUi()
{
   self_->setWindowTitle(QObject::tr("Pending Downloads"));
   self_->setModal(false);
   self_->resize(520, 300);

   summaryLabel_ = new QLabel(self_);
   list_         = new QListWidget(self_);
   list_->setSelectionMode(QAbstractItemView::NoSelection);
   list_->setFocusPolicy(Qt::NoFocus);

   auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, self_);
   QObject::connect(
      buttons, &QDialogButtonBox::rejected, self_, &QDialog::close);

   auto* layout = new QVBoxLayout(self_);
   layout->addWidget(summaryLabel_);
   layout->addWidget(list_, 1);
   layout->addWidget(buttons);

   refreshTimer_ = new QTimer(self_);
   refreshTimer_->setSingleShot(true);
   refreshTimer_->setInterval(kRefreshIntervalMs_);
   QObject::connect(
      refreshTimer_, &QTimer::timeout, self_, [this]() { self_->Refresh(); });

   // May be emitted from any thread that reports; connecting to `self_` makes
   // delivery queued onto this one.
   QObject::connect(statusManager_.get(),
                    &manager::StatusManager::StatusChanged,
                    self_,
                    [this]() { ScheduleRefresh(); });
}

void PendingStatusDialog::Impl::ScheduleRefresh()
{
   if (self_->isVisible() && !refreshTimer_->isActive())
   {
      refreshTimer_->start();
   }
}

PendingStatusDialog::PendingStatusDialog(QWidget* parent) :
    QDialog(parent), p {std::make_unique<Impl>(this)}
{
   p->BuildUi();
   Refresh();
}

PendingStatusDialog::~PendingStatusDialog() = default;

void PendingStatusDialog::Refresh()
{
   const auto lines = p->statusManager_->PendingLines();

   p->list_->clear();
   for (const auto& line : lines)
   {
      p->list_->addItem(QString::fromStdString(line));
   }

   p->summaryLabel_->setText(
      lines.empty() ?
         tr("Nothing is pending.") :
         tr("%n item(s) pending:", "", static_cast<int>(lines.size())));
}

void PendingStatusDialog::showEvent(QShowEvent* event)
{
   QDialog::showEvent(event);
   Refresh();
}

} // namespace scwx::qt::ui
