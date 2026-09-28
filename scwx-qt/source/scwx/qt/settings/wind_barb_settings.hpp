#pragma once

#include <scwx/qt/settings/settings_category.hpp>
#include <scwx/qt/settings/settings_variable.hpp>

#include <memory>

namespace scwx::qt::settings
{

class WindBarbSettings : public SettingsCategory
{
public:
   explicit WindBarbSettings();
   ~WindBarbSettings() override;

   WindBarbSettings(const WindBarbSettings&)            = delete;
   WindBarbSettings& operator=(const WindBarbSettings&) = delete;

   WindBarbSettings(WindBarbSettings&&) noexcept;
   WindBarbSettings& operator=(WindBarbSettings&&) noexcept;

   // Whether WindBarbLayer draws the red gust-excess barb underneath the
   // normal sustained-speed one (see GetWindBarbGustTexture's own
   // comment for the technique) -- on by default.
   [[nodiscard]] SettingsVariable<bool>& show_gust_barbs() const;

   // Multiplies WindBarbLayer's own per-zoom-tier icon scale (see
   // TierForZoom) -- 1.0 (default) keeps the existing tier sizes as-is;
   // >1.0 makes every barb bigger at every zoom level, without changing
   // the tiers' own relative sizing to each other.
   [[nodiscard]] SettingsVariable<double>& icon_scale() const;

   // Divides WindBarbLayer's own per-zoom-tier stride (see TierForZoom) --
   // 1.0 (default) keeps the existing tier spacing as-is; >1.0 packs
   // barbs closer together (denser) at every zoom level, <1.0 spreads
   // them out (sparser).
   [[nodiscard]] SettingsVariable<double>& density_scale() const;

   static WindBarbSettings& Instance();

   friend bool operator==(const WindBarbSettings& lhs,
                          const WindBarbSettings& rhs);

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::settings
