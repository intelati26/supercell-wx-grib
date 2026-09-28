#pragma once

#include <scwx/qt/settings/settings_category.hpp>
#include <scwx/qt/settings/settings_variable.hpp>

#include <memory>

namespace scwx::qt::settings
{

class HodographSettings : public SettingsCategory
{
public:
   explicit HodographSettings();
   ~HodographSettings() override;

   HodographSettings(const HodographSettings&)            = delete;
   HodographSettings& operator=(const HodographSettings&) = delete;

   HodographSettings(HodographSettings&&) noexcept;
   HodographSettings& operator=(HodographSettings&&) noexcept;

   // Multiplies HodographLayer's own per-zoom-tier geographic scale (see
   // its TierForZoom) -- unlike WindBarbLayer's icons, a hodograph's
   // on-screen size is a real geographic distance (GeoLines draws
   // between real lat/lon endpoints, not a screen-space pixel offset the
   // way GeoIcons does), so this scales how many real metres each 1 m/s
   // of wind is drawn as, not a pixel size directly. 1.0 (default) keeps
   // the existing tiers' first-guess sizing as-is.
   [[nodiscard]] SettingsVariable<double>& size_scale() const;

   // Concentric speed-reference rings (every 20kt) drawn behind each
   // hodograph -- see HodographLayer::RebuildLines' own comment. On by
   // default, matching the reference screenshot that originally motivated
   // this feature (see docs/gridded-hodograph-plan.md); toggleable since
   // they add real per-point draw cost and some users may prefer the
   // plainer look.
   [[nodiscard]] SettingsVariable<bool>& show_range_rings() const;

   static HodographSettings& Instance();

   friend bool operator==(const HodographSettings& lhs,
                          const HodographSettings& rhs);

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::settings
