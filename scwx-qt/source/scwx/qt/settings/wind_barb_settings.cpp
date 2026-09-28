#include <scwx/qt/settings/wind_barb_settings.hpp>

namespace scwx::qt::settings
{

class WindBarbSettings::Impl
{
public:
   explicit Impl()
   {
      showGustBarbs_.SetDefault(true);

      // User feedback: the fixed per-tier sizes/strides (see
      // WindBarbLayer::TierForZoom) read as too small and too sparse --
      // rather than just retuning those constants (a one-time guess that
      // may not suit every user/monitor), exposed as real, user-adjustable
      // multipliers instead. 0.25-4.0 gives a wide enough range to go from
      // noticeably smaller/sparser to noticeably bigger/denser than the
      // unscaled tiers, without either extreme collapsing to nothing or
      // overwhelming the map.
      iconScale_.SetDefault(1.0);
      iconScale_.SetMinimum(0.25);
      iconScale_.SetMaximum(4.0);

      densityScale_.SetDefault(1.0);
      densityScale_.SetMinimum(0.25);
      densityScale_.SetMaximum(4.0);
   }
   ~Impl()                       = default;
   Impl(const Impl&)             = delete;
   Impl& operator=(const Impl&)  = delete;
   Impl(const Impl&&)            = delete;
   Impl& operator=(const Impl&&) = delete;

   SettingsVariable<bool>   showGustBarbs_ {"show_gust_barbs"};
   SettingsVariable<double> iconScale_ {"icon_scale"};
   SettingsVariable<double> densityScale_ {"density_scale"};
};

WindBarbSettings::WindBarbSettings() :
    SettingsCategory("wind_barbs"), p(std::make_unique<Impl>())
{
   RegisterVariables({&p->showGustBarbs_, &p->iconScale_, &p->densityScale_});
   SetDefaults();
}
WindBarbSettings::~WindBarbSettings() = default;

WindBarbSettings::WindBarbSettings(WindBarbSettings&&) noexcept = default;
WindBarbSettings&
WindBarbSettings::operator=(WindBarbSettings&&) noexcept = default;

SettingsVariable<bool>& WindBarbSettings::show_gust_barbs() const
{ return p->showGustBarbs_; }

SettingsVariable<double>& WindBarbSettings::icon_scale() const
{ return p->iconScale_; }

SettingsVariable<double>& WindBarbSettings::density_scale() const
{ return p->densityScale_; }

WindBarbSettings& WindBarbSettings::Instance()
{
   static WindBarbSettings windBarbSettings_;
   return windBarbSettings_;
}

bool operator==(const WindBarbSettings& lhs, const WindBarbSettings& rhs)
{
   return (lhs.p->showGustBarbs_ == rhs.p->showGustBarbs_ &&
           lhs.p->iconScale_ == rhs.p->iconScale_ &&
           lhs.p->densityScale_ == rhs.p->densityScale_);
}

} // namespace scwx::qt::settings
