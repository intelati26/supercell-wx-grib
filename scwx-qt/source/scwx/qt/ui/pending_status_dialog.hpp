// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <QDialog>

#include <memory>

namespace scwx::qt::ui
{

// Everything the status bar's one line stands for: each download in progress
// (with its byte counts) and each notice from a failed one, as the lines
// manager::StatusManager::PendingLines() reports, kept up to date while it is
// open. Not modal -- it is something to glance at while the app works.
class PendingStatusDialog : public QDialog
{
   Q_OBJECT

public:
   explicit PendingStatusDialog(QWidget* parent = nullptr);
   ~PendingStatusDialog() override;

   // Re-reads the list now (it also does so itself, a few times a second, while
   // visible and something changes).
   void Refresh();

protected:
   void showEvent(QShowEvent* event) override;

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::ui
