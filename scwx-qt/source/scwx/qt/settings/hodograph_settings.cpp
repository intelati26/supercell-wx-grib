#include <scwx/qt/settings/hodograph_settings.hpp>

namespace scwx::qt::settings
{

class HodographSettings::Impl
{
public:
   explicit Impl()
   {
      sizeScale_.SetDefault(1.0);
      sizeScale_.SetMinimum(0.25);
      sizeScale_.SetMaximum(4.0);

      showRangeRings_.SetDefault(true);
   }
   ~Impl()                       = default;
   Impl(const Impl&)             = delete;
   Impl& operator=(const Impl&)  = delete;
   Impl(const Impl&&)            = delete;
   Impl& operator=(const Impl&&) = delete;

   SettingsVariable<double> sizeScale_ {"size_scale"};
   SettingsVariable<bool>   showRangeRings_ {"show_range_rings"};
};

HodographSettings::HodographSettings() :
    SettingsCategory("hodograph"), p(std::make_unique<Impl>())
{
   RegisterVariables({&p->sizeScale_, &p->showRangeRings_});
   SetDefaults();
}
HodographSettings::~HodographSettings() = default;

HodographSettings::HodographSettings(HodographSettings&&) noexcept = default;
HodographSettings&
HodographSettings::operator=(HodographSettings&&) noexcept = default;

SettingsVariable<double>& HodographSettings::size_scale() const
{ return p->sizeScale_; }

SettingsVariable<bool>& HodographSettings::show_range_rings() const
{ return p->showRangeRings_; }

HodographSettings& HodographSettings::Instance()
{
   static HodographSettings hodographSettings_;
   return hodographSettings_;
}

bool operator==(const HodographSettings& lhs, const HodographSettings& rhs)
{
   return (lhs.p->sizeScale_ == rhs.p->sizeScale_ &&
           lhs.p->showRangeRings_ == rhs.p->showRangeRings_);
}

} // namespace scwx::qt::settings
