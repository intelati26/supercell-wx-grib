// *****************************************************************************
// * This file is part of supercell-wx-grib.  Licensed under the GNU General
// * Public License v3.  See the COPYING file for the full license text.
// *****************************************************************************

#pragma once

#include <QImage>
#include <QString>

#include <vector>

namespace scwx::qt::util::image_export
{

// Saving images as WebP through libwebp's own command-line tools, which are
// optional: `cwebp` for one picture, `img2webp` for an animation. Nothing is
// bundled, and a missing tool is never papered over with a different format --
// callers get `false` and a sentence in `error` they can show the user.
//
// Lossless throughout, so flat colours and legend text stay exact the way PNG
// keeps them.

// The tool's path, searching PATH and a "tools" folder beside the program
// (where a user can simply drop the executables). Empty when it is not
// installed.
[[nodiscard]] QString FindTool(const QString& name);

[[nodiscard]] bool CanSaveWebp();      // cwebp
[[nodiscard]] bool CanSaveAnimation(); // img2webp

// A sentence telling the user what to install, worded for the current system.
[[nodiscard]] QString InstallHint(const QString& toolName);

// Saves `image` to `path`: WebP when the extension is ".webp", otherwise
// whatever Qt's writer makes of the extension (PNG in practice). On failure
// returns false with a human-readable reason in `error`.
[[nodiscard]] bool
SaveImage(const QImage& image, const QString& path, QString& error);

// Saves an endlessly looping animated WebP, `frameDelayMs` per frame. Needs at
// least two frames.
[[nodiscard]] bool SaveAnimation(const std::vector<QImage>& frames,
                                 int                        frameDelayMs,
                                 const QString&             path,
                                 QString&                   error);

} // namespace scwx::qt::util::image_export
