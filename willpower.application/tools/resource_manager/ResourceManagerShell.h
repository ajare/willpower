#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "willpower/application/resourcesystem/ResourceManifestDocument.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace resource_manager {

bool hasYamlExtension(std::filesystem::path const& path);

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

 private:
  bool saveTo(std::filesystem::path const& manifestPath);
  void setFailure(std::string message);

  wp::application::resourcesystem::ResourceSchemaCatalogSnapshot mCatalog;
  std::unique_ptr<wp::application::resourcesystem::ResourceManifestDocument>
      mDocument;
  std::filesystem::path mPath;
  std::filesystem::path mBaseDirectory;
  std::vector<wp::application::resourcesystem::ResourceManifestDiagnostic>
      mStructuralDiagnostics;
  std::string mOperationDiagnostic;
  bool mDirty = false;
};

bool runDocumentTests(std::string* failure);

}  // namespace resource_manager
