#include <scwx/qt/manager/hodograph_selection.hpp>

#include <vector>

#include <gtest/gtest.h>

namespace scwx::qt::manager
{

// One process-wide object, so each test puts it back how it found it (off).
class HodographSelectionTest : public testing::Test
{
protected:
   void TearDown() override
   { HodographSelection::Instance().SetEnabled(false); }
};

TEST_F(HodographSelectionTest, StartsOffAndTogglesOnRequest)
{
   auto& selection = HodographSelection::Instance();
   EXPECT_FALSE(selection.IsEnabled());

   selection.SetEnabled(true);
   EXPECT_TRUE(selection.IsEnabled());

   selection.SetEnabled(false);
   EXPECT_FALSE(selection.IsEnabled());
}

TEST_F(HodographSelectionTest, AnnouncesOnlyRealChanges)
{
   auto&                         selection = HodographSelection::Instance();
   std::vector<bool>             announced;
   const QMetaObject::Connection connection = QObject::connect(
      &selection,
      &HodographSelection::EnabledChanged,
      [&announced](bool enabled) { announced.push_back(enabled); });

   selection.SetEnabled(false); // already off
   EXPECT_TRUE(announced.empty());

   selection.SetEnabled(true);
   selection.SetEnabled(true); // already on
   ASSERT_EQ(announced.size(), 1u);
   EXPECT_TRUE(announced[0]);

   selection.SetEnabled(false);
   ASSERT_EQ(announced.size(), 2u);
   EXPECT_FALSE(announced[1]);

   QObject::disconnect(connection);
}

} // namespace scwx::qt::manager
