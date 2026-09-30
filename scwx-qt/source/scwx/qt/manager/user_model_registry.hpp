#pragma once

#include <scwx/util/grib_model_config.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QObject>

namespace scwx::qt::manager
{

// A model that loaded, and the folder it came from.
struct UserModelEntry
{
   std::string                          folderName;
   util::grib_model_config::ModelConfig config;
};

// A folder that didn't load cleanly, or loaded with skipped rows.
struct UserModelIssue
{
   std::string              folderName;
   std::vector<std::string> errors;
   std::vector<std::string> warnings;
};

struct UserModelImportResult
{
   bool                     ok {false};
   std::string              modelName;
   std::vector<std::string> errors;
   std::vector<std::string> warnings;
};

// User-imported GRIB models (scwx::util::grib_model_config), one folder each
// under <local app data>/grib-models/. The registry only knows which models
// exist and which is selected; turning one into products and a data source is
// GribManager's job (GribCategory::User).
//
// Every file here is untrusted text: nothing throws, nothing touches the
// network, and a bad folder becomes an issue to show, never a startup failure.
class UserModelRegistry : public QObject
{
   Q_OBJECT

public:
   explicit UserModelRegistry();
   ~UserModelRegistry();

   UserModelRegistry(const UserModelRegistry&)            = delete;
   UserModelRegistry& operator=(const UserModelRegistry&) = delete;
   UserModelRegistry(UserModelRegistry&&)                 = delete;
   UserModelRegistry& operator=(UserModelRegistry&&)      = delete;

   static std::shared_ptr<UserModelRegistry> Instance();

   // <local app data>/grib-models
   [[nodiscard]] static std::filesystem::path ModelsDirectory();

   // Test hook: use `directory` instead of the app-data location (or, with
   // an empty path, go back to it). Takes effect at the next Reload().
   static void SetModelsDirectoryForTesting(std::filesystem::path directory);

   // Scans a models directory. Static and pure so it can be tested against a
   // temporary folder; models come back sorted by name, and a second model
   // reusing a name is reported as an issue and skipped.
   static void Scan(const std::filesystem::path& modelsDirectory,
                    std::vector<UserModelEntry>& models,
                    std::vector<UserModelIssue>& issues);

   // Validates `sourceFolder` (model.json + products.csv) and, only if it is
   // clean, copies both files into `modelsDirectory/<slug of the model name>/`,
   // replacing any earlier import of the same model. Refuses symlinks and
   // files over kMaxImportFileBytes. Nothing is copied on any error.
   static UserModelImportResult
   ImportInto(const std::filesystem::path& sourceFolder,
              const std::filesystem::path& modelsDirectory);

   static constexpr std::uintmax_t kMaxImportFileBytes = 1024 * 1024;

   // Rescans ModelsDirectory(). Keeps the selection if that model still
   // exists, otherwise selects the first.
   void Reload();

   [[nodiscard]] std::vector<UserModelEntry> Models() const;
   [[nodiscard]] std::vector<UserModelIssue> Issues() const;

   // Empty when there are no models.
   [[nodiscard]] std::string                   SelectedModelName() const;
   [[nodiscard]] std::optional<UserModelEntry> SelectedModel() const;
   void SetSelectedModel(const std::string& name);

   // ImportInto(ModelsDirectory()), then Reload() and select the imported
   // model.
   UserModelImportResult Import(const std::filesystem::path& sourceFolder);

signals:
   void ModelsChanged();
   void SelectedModelChanged();

private:
   class Impl;
   std::unique_ptr<Impl> p;
};

} // namespace scwx::qt::manager
