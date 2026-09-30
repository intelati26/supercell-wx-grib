#include <scwx/qt/manager/user_model_registry.hpp>
#include <scwx/qt/main/application_paths.hpp>
#include <scwx/util/logger.hpp>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <system_error>

namespace scwx::qt::manager
{

static const std::string logPrefix_ = "scwx::qt::manager::user_model_registry";
static const auto        logger_    = scwx::util::Logger::Create(logPrefix_);

namespace fs = std::filesystem;
using util::grib_model_config::LoadModelFolder;

static constexpr const char* kModelFile    = "model.json";
static constexpr const char* kProductsFile = "products.csv";

namespace
{

// Folder name for a model: lowercase letters and digits, runs of anything
// else collapsed to one '-'. The name itself is free text from a file, so it
// never becomes part of a path unfiltered.
std::string Slug(const std::string& name)
{
   std::string slug;
   for (const unsigned char c : name)
   {
      if (std::isalnum(c) && c < 0x80)
      {
         slug += static_cast<char>(std::tolower(c));
      }
      else if (!slug.empty() && slug.back() != '-')
      {
         slug += '-';
      }
   }
   while (!slug.empty() && slug.back() == '-')
   {
      slug.pop_back();
   }
   return slug.empty() ? "model" : slug.substr(0, 64);
}

// A plain file: not a symlink, not a directory, not a device.
bool IsPlainFile(const fs::path& path)
{
   std::error_code ec;
   const auto      status = fs::symlink_status(path, ec);
   return !ec && fs::is_regular_file(status);
}

} // namespace

void UserModelRegistry::Scan(const fs::path&              modelsDirectory,
                             std::vector<UserModelEntry>& models,
                             std::vector<UserModelIssue>& issues)
{
   models.clear();
   issues.clear();

   std::error_code ec;
   if (!fs::is_directory(modelsDirectory, ec))
   {
      return;
   }

   std::vector<fs::path> folders;
   for (const auto& entry : fs::directory_iterator(modelsDirectory, ec))
   {
      std::error_code entryEc;
      const auto      status = entry.symlink_status(entryEc);
      if (!entryEc && fs::is_directory(status)) // a symlinked folder is skipped
      {
         folders.push_back(entry.path());
      }
   }
   std::sort(folders.begin(), folders.end());

   for (const auto& folder : folders)
   {
      const std::string folderName = folder.filename().string();
      auto              result     = LoadModelFolder(folder.string());

      if (!result.errors.empty() || !result.warnings.empty())
      {
         issues.push_back({folderName, result.errors, result.warnings});
      }
      if (!result.config)
      {
         continue;
      }

      const bool duplicate =
         std::any_of(models.begin(),
                     models.end(),
                     [&](const UserModelEntry& m)
                     { return m.config.name == result.config->name; });
      if (duplicate)
      {
         issues.push_back({folderName,
                           {"a model named \"" + result.config->name +
                            "\" is already loaded from another folder"},
                           {}});
         continue;
      }

      models.push_back({folderName, std::move(*result.config)});
   }

   std::sort(models.begin(),
             models.end(),
             [](const UserModelEntry& a, const UserModelEntry& b)
             { return a.config.name < b.config.name; });
}

UserModelImportResult
UserModelRegistry::ImportInto(const fs::path& sourceFolder,
                              const fs::path& modelsDirectory)
{
   UserModelImportResult out;

   for (const char* file : {kModelFile, kProductsFile})
   {
      const fs::path path = sourceFolder / file;
      if (!IsPlainFile(path))
      {
         out.errors.push_back(std::string(file) +
                              ": missing (or not a regular file) in " +
                              sourceFolder.string());
         continue;
      }

      std::error_code ec;
      const auto      size = fs::file_size(path, ec);
      if (!ec && size > kMaxImportFileBytes)
      {
         out.errors.push_back(std::string(file) + ": larger than " +
                              std::to_string(kMaxImportFileBytes / 1024) +
                              " KB");
      }
   }
   if (!out.errors.empty())
   {
      return out;
   }

   auto loaded  = LoadModelFolder(sourceFolder.string());
   out.errors   = loaded.errors;
   out.warnings = loaded.warnings;
   if (!loaded.config)
   {
      return out;
   }
   out.modelName = loaded.config->name;

   const fs::path target = modelsDirectory / Slug(loaded.config->name);

   std::error_code ec;
   fs::create_directories(target, ec);
   if (ec)
   {
      out.errors.push_back("could not create " + target.string() + ": " +
                           ec.message());
      return out;
   }

   for (const char* file : {kModelFile, kProductsFile})
   {
      fs::copy_file(sourceFolder / file,
                    target / file,
                    fs::copy_options::overwrite_existing,
                    ec);
      if (ec)
      {
         out.errors.push_back("could not copy " + std::string(file) + ": " +
                              ec.message());
         return out;
      }
   }

   out.ok = true;
   return out;
}

class UserModelRegistry::Impl
{
public:
   mutable std::mutex          mutex_ {};
   std::vector<UserModelEntry> models_ {};
   std::vector<UserModelIssue> issues_ {};
   std::string                 selected_ {};
};

UserModelRegistry::UserModelRegistry() : p(std::make_unique<Impl>())
{ Reload(); }

UserModelRegistry::~UserModelRegistry() = default;

std::shared_ptr<UserModelRegistry> UserModelRegistry::Instance()
{
   static std::weak_ptr<UserModelRegistry> instanceRef_ {};
   static std::mutex                       instanceMutex_ {};

   std::unique_lock lock(instanceMutex_);

   std::shared_ptr<UserModelRegistry> instance = instanceRef_.lock();
   if (instance == nullptr)
   {
      instance     = std::make_shared<UserModelRegistry>();
      instanceRef_ = instance;
   }

   return instance;
}

namespace
{
std::mutex gDirectoryOverrideMutex;
fs::path   gDirectoryOverride;
} // namespace

void UserModelRegistry::SetModelsDirectoryForTesting(fs::path directory)
{
   std::lock_guard lock(gDirectoryOverrideMutex);
   gDirectoryOverride = std::move(directory);
}

fs::path UserModelRegistry::ModelsDirectory()
{
   {
      std::lock_guard lock(gDirectoryOverrideMutex);
      if (!gDirectoryOverride.empty())
      {
         return gDirectoryOverride;
      }
   }

   return main::ApplicationPaths::GetLocation(
             main::ApplicationPaths::StandardLocation::Local) /
          "grib-models";
}

void UserModelRegistry::Reload()
{
   std::vector<UserModelEntry> models;
   std::vector<UserModelIssue> issues;
   Scan(ModelsDirectory(), models, issues);

   for (const auto& issue : issues)
   {
      for (const auto& e : issue.errors)
      {
         logger_->warn("{}: {}", issue.folderName, e);
      }
   }

   bool selectionChanged = false;
   {
      std::lock_guard lock(p->mutex_);
      p->models_ = std::move(models);
      p->issues_ = std::move(issues);

      const auto found = std::any_of(p->models_.begin(),
                                     p->models_.end(),
                                     [&](const UserModelEntry& m)
                                     { return m.config.name == p->selected_; });
      if (!found)
      {
         const std::string next = p->models_.empty() ?
                                     std::string {} :
                                     p->models_.front().config.name;
         selectionChanged       = (next != p->selected_);
         p->selected_           = next;
      }
   }

   Q_EMIT ModelsChanged();
   if (selectionChanged)
   {
      Q_EMIT SelectedModelChanged();
   }
}

std::vector<UserModelEntry> UserModelRegistry::Models() const
{
   std::lock_guard lock(p->mutex_);
   return p->models_;
}

std::vector<UserModelIssue> UserModelRegistry::Issues() const
{
   std::lock_guard lock(p->mutex_);
   return p->issues_;
}

std::string UserModelRegistry::SelectedModelName() const
{
   std::lock_guard lock(p->mutex_);
   return p->selected_;
}

std::optional<UserModelEntry> UserModelRegistry::SelectedModel() const
{
   std::lock_guard lock(p->mutex_);
   for (const auto& model : p->models_)
   {
      if (model.config.name == p->selected_)
      {
         return model;
      }
   }
   return std::nullopt;
}

void UserModelRegistry::SetSelectedModel(const std::string& name)
{
   {
      std::lock_guard lock(p->mutex_);
      if (name == p->selected_ ||
          std::none_of(p->models_.begin(),
                       p->models_.end(),
                       [&](const UserModelEntry& m)
                       { return m.config.name == name; }))
      {
         return;
      }
      p->selected_ = name;
   }
   Q_EMIT SelectedModelChanged();
}

UserModelImportResult UserModelRegistry::Import(const fs::path& sourceFolder)
{
   auto result = ImportInto(sourceFolder, ModelsDirectory());
   if (result.ok)
   {
      Reload();
      SetSelectedModel(result.modelName);
   }
   return result;
}

} // namespace scwx::qt::manager
