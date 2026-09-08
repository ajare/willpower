#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <mpp/app/CommandStack.h>

#include "willpower/application/resourcesystem/ResourceManifestDocument.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace resource_manager {

bool hasYamlExtension(std::filesystem::path const& path);

struct ResourceOptionForm {
  std::string name;
  std::vector<std::string> values;
  bool boolean = false;
};

// Library-neutral metadata extracted from one catalogued Resource Type schema.
// File controls are intentionally described by schema annotations rather than
// by Resource Type names in the UI.
struct ResourceForm {
  std::string resourceType;
  std::string title;
  std::string fileProperty;
  std::string fileKind;
  std::vector<std::string> fileExtensions;
  std::vector<ResourceOptionForm> options;
};

struct ResourceSummary {
  std::string resourceNamespace;
  std::string name;
  std::string resourceType;
  std::string location;
  std::vector<std::pair<std::string, std::string>> options;
  bool explicitName = false;
  bool editable = false;
};

struct ResourceDraft {
  std::string resourceNamespace;
  std::string resourceType;
  std::string name;
  std::string location;
  std::string validationMessage;
};

class ManifestWorkspace {
 public:
  ManifestWorkspace();

  bool createNew(std::filesystem::path const& baseDirectory);
  bool open(std::filesystem::path const& manifestPath);
  bool save();
  bool saveAs(std::filesystem::path const& manifestPath);
  void reportFailure(std::string message);

  [[nodiscard]] bool hasDocument() const noexcept;
  [[nodiscard]] bool hasPath() const noexcept;
  [[nodiscard]] bool dirty() const noexcept;
  [[nodiscard]] std::filesystem::path const& path() const noexcept;
  [[nodiscard]] std::filesystem::path const& baseDirectory() const noexcept;
  [[nodiscard]] std::string const& operationDiagnostic() const noexcept;
  [[nodiscard]] std::vector<wp::application::resourcesystem::ResourceManifestDiagnostic>
      const& structuralDiagnostics() const noexcept;
  [[nodiscard]] std::string canonicalYaml() const;

  [[nodiscard]] std::vector<ResourceForm> const& resourceForms() const noexcept;
  [[nodiscard]] ResourceForm const* resourceForm(
      std::string const& resourceType) const noexcept;
  [[nodiscard]] std::vector<ResourceSummary> resources() const;

  // A draft is local UI state: it does not alter the committed document or
  // history until commitDraft succeeds.
  bool beginDraft(std::string resourceType,
                  std::string resourceNamespace = {});
  void setDraftName(std::string name);
  bool selectDraftFile(std::filesystem::path const& selectedFile);
  [[nodiscard]] ResourceDraft const* draft() const noexcept;
  [[nodiscard]] bool draftValid() const noexcept;
  bool commitDraft();
  void cancelDraft() noexcept;

  bool renameResource(std::string const& resourceNamespace,
                      std::string const& currentName, std::string newName,
                      bool continuous = false);
  bool setResourceFile(std::string const& resourceNamespace,
                       std::string const& name,
                       std::filesystem::path const& selectedFile,
                       bool continuous = false);
  bool setResourceOption(std::string const& resourceNamespace,
                         std::string const& name,
                         std::string const& optionName,
                         std::optional<std::string> value,
                         bool continuous = false);
  bool deleteResource(std::string const& resourceNamespace,
                      std::string const& name);

  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] bool canRedo() const noexcept;
  [[nodiscard]] std::string const* undoName() const noexcept;
  [[nodiscard]] std::string const* redoName() const noexcept;
  bool undo();
  bool redo();
  void endContinuousEdit();

 private:
  bool saveTo(std::filesystem::path const& manifestPath);
  void setFailure(std::string message);
  bool applyCommittedYaml(std::string const& yaml);
  bool executeYamlCommand(std::string name, std::string yaml,
                          std::string mergeKey = {}, bool continuous = false);
  std::optional<std::string> portableSelectedFile(
      std::filesystem::path const& selectedFile);
  void validateDraft();

  wp::application::resourcesystem::ResourceSchemaCatalogSnapshot mCatalog;
  std::unique_ptr<wp::application::resourcesystem::ResourceManifestDocument>
      mDocument;
  std::filesystem::path mPath;
  std::filesystem::path mBaseDirectory;
  std::vector<wp::application::resourcesystem::ResourceManifestDiagnostic>
      mStructuralDiagnostics;
  std::string mOperationDiagnostic;
  std::vector<ResourceForm> mResourceForms;
  std::optional<ResourceDraft> mDraft;
  mpp::app::CommandStack mCommands{256};
  bool mUnsavedDocument = false;
};

bool runDocumentTests(std::string* failure);
bool runAuthoringTests(std::string* failure);

}  // namespace resource_manager
