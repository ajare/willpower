#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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

// Bundle paths are absolute, lexically-normal paths resolved from the deployment
// INI directory. A missing base bundle means that embedded built-ins are used.
struct EditorSchemaConfiguration {
  std::filesystem::path iniPath;
  std::optional<std::filesystem::path> baseBundle;
  std::vector<std::filesystem::path> extensionBundles;
};

EditorSchemaConfiguration readEditorSchemaConfiguration(
    std::filesystem::path const& iniPath);
wp::application::resourcesystem::ResourceSchemaCatalogSnapshot
loadEditorSchemaCatalog(EditorSchemaConfiguration const& configuration);

struct ResourceOptionForm {
  std::string name;
  std::vector<std::string> values;
  std::optional<std::string> editorDefault;
  bool boolean = false;
};

// Library-neutral metadata extracted from one catalogued Resource Type schema.
// File controls are intentionally described by schema annotations rather than
// by Resource Type names in the UI.
struct ResourceDependencyForm {
  std::string id;
  std::vector<std::string> allowedResourceTypes;
};

struct ResourceForm {
  std::string resourceType;
  std::string title;
  std::string fileProperty;
  std::string fileKind;
  std::vector<std::string> fileExtensions;
  std::vector<ResourceOptionForm> options;
  std::vector<ResourceDependencyForm> requiredDependencies;
  bool composite = false;
  bool applicationOwned = false;
  bool requiresDefinition = false;
};

enum class NestedCollectionKind {
  definition,
  image,
  imageSet,
  animation,
  frame,
  overrideFrame,
  tag,
  buffer,
  channel,
  texture,
  object
};

struct NestedPropertyForm {
  std::string name;
  std::string value;
  bool required = false;
  bool integer = false;
  bool number = false;
  bool boolean = false;
  bool optional = false;
  double minimum = 0.0;
  bool hasMinimum = false;
  bool exclusiveMinimum = false;
  std::vector<std::string> enumValues;
  // A selector is used instead of free text when values are constrained by
  // the current Resource (for example a Material image dependency ID).
  std::vector<std::string> selectorValues;
};

// Paths are Resource-relative JSON instance paths. Collection values are
// normalized to indexed paths even when compatible YAML used a singleton.
struct NestedFormItem {
  NestedCollectionKind kind = NestedCollectionKind::image;
  std::string path;
  std::string label;
  std::vector<NestedPropertyForm> properties;
  std::string alternative;
  std::vector<std::string> alternatives;
};

struct DefinitionFactoryChoice {
  std::string factoryType;
  std::string title;
  bool selected = false;
  bool disabled = false;
};

enum class SemanticDiagnosticSeverity { warning, error };

enum class SemanticDiagnosticKind {
  duplicateName,
  invalidName,
  unresolvedReference,
  referenceTypeMismatch,
  dependencyCycle,
  missingFile,
  pathContainment,
  invalidFileTarget,
  unvalidatedData,
  defensiveLimit
};

struct SemanticDiagnostic {
  SemanticDiagnosticSeverity severity = SemanticDiagnosticSeverity::error;
  SemanticDiagnosticKind kind = SemanticDiagnosticKind::invalidName;
  std::string resourceNamespace;
  std::string resourceName;
  std::string resourcePath;
  std::string instancePath;
  std::string message;
  // Stable semantic subject used to distinguish a preserved error from an
  // unrelated error introduced by an edit.
  std::string subject;
  bool inlineResource = false;
  std::size_t dependencyIndex = 0;
};

struct DiagnosticNavigation {
  std::string resourceNamespace;
  std::string resourceName;
  std::string resourcePath;
  std::string instancePath;
  bool inlineResource = false;
  std::size_t dependencyIndex = 0;
};

struct NamespaceSummary {
  std::string name;
  std::string instancePath;
  std::size_t resourceCount = 0;
  bool isDefault = false;
  bool draft = false;
};

struct ResourceSummary {
  std::string resourceNamespace;
  std::string name;
  std::string instancePath;
  std::string resourceType;
  std::string location;
  std::vector<std::pair<std::string, std::string>> options;
  std::string limitationWarning;
  bool explicitName = false;
  bool editable = false;
  bool unknownType = false;
};

struct InlineResourceSummary : ResourceSummary {
  std::string ownerName;
  std::size_t dependencyIndex = 0;
  std::string dependencyId;
};

struct ResourceReferenceChoice {
  std::string resourceNamespace;
  std::string name;
  std::string resourceType;
  std::string qualifiedIdentity;
  bool selected = false;
  bool disabled = false;
  bool missing = false;
  bool inlineResource = false;
  std::string reason;
};

struct ResourceReferenceSelector {
  std::string ownerNamespace;
  std::string ownerName;
  std::size_t dependencyIndex = 0;
  std::string dependencyId;
  std::string reference;
  std::vector<std::string> allowedResourceTypes;
  std::vector<ResourceReferenceChoice> choices;
  bool clearable = false;
  bool missing = false;
};

struct DependencyDiagnostic {
  std::string resourceNamespace;
  std::string resourceName;
  std::size_t dependencyIndex = 0;
  std::string message;
  bool error = true;
};

struct NamespaceDraft {
  std::string name;
  std::string validationMessage;
};

struct ResourceDraftReference {
  std::string id;
  std::vector<std::string> allowedResourceTypes;
  std::string resourceNamespace;
  std::string name;
};

// The revision observed when a file-backed document was opened or saved.  The
// timestamp and size make inexpensive polling possible; contentHash is the
// authoritative comparison and is always computed before an overwrite.
struct SourceRevision {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified{};
  std::string contentHash;

  friend bool operator==(SourceRevision const&, SourceRevision const&) = default;
};

enum class ExternalChangeState { unchanged, cleanDocument, dirtyConflict, missing };

// User state is deliberately independent of the deployment INI and manifest.
// The file is written atomically below the platform preference directory.
class EditorPreferences {
 public:
  explicit EditorPreferences(std::filesystem::path directory = {});
  bool load(std::string* failure = nullptr);
  bool save(std::string* failure = nullptr) const;

  void noteManifest(std::filesystem::path const& manifest,
                    std::filesystem::path const& baseDirectory);
  [[nodiscard]] std::vector<std::filesystem::path> const& recentManifests() const noexcept;
  [[nodiscard]] std::optional<std::filesystem::path> baseDirectoryFor(
      std::filesystem::path const& manifest) const;
  void setSplitter(float treeFraction, float diagnosticsHeight) noexcept;
  [[nodiscard]] float treeFraction() const noexcept;
  [[nodiscard]] float diagnosticsHeight() const noexcept;
  void setViewPreference(std::string name, bool value);
  [[nodiscard]] bool viewPreference(std::string const& name,
                                    bool fallback = false) const;
  [[nodiscard]] std::filesystem::path const& directory() const noexcept;

 private:
  std::filesystem::path mDirectory;
  std::vector<std::filesystem::path> mRecentManifests;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>>
      mBaseDirectories;
  std::vector<std::pair<std::string, bool>> mViewPreferences;
  float mTreeFraction = 0.32f;
  float mDiagnosticsHeight = 145.0f;
};

struct ResourceDraft {
  std::string resourceNamespace;
  std::string resourceType;
  std::string name;
  std::string location;
  // Retained for source compatibility with the first required dependency.
  std::string dependencyNamespace;
  std::string dependencyName;
  std::vector<ResourceDraftReference> references;
  std::string validationMessage;
};

class ManifestWorkspace {
 public:
  ManifestWorkspace();
  explicit ManifestWorkspace(
      wp::application::resourcesystem::ResourceSchemaCatalogSnapshot catalog,
      std::filesystem::path preferenceDirectory = {});

  bool createNew(std::filesystem::path const& baseDirectory);
  bool open(std::filesystem::path const& manifestPath);
  bool open(std::filesystem::path const& manifestPath,
            std::filesystem::path const& baseDirectory);
  bool save();
  bool saveAs(std::filesystem::path const& manifestPath);
  // Save refuses to replace a source whose content changed since the last
  // observed revision.  overwriteExternal is the explicit destructive choice.
  bool overwriteExternal();
  [[nodiscard]] ExternalChangeState checkExternalChange();
  [[nodiscard]] ExternalChangeState externalChangeState() const noexcept;
  bool reloadExternal();
  bool discardChanges();

  // Called by the desktop loop. Dirty data is written only after the debounce
  // interval; tests may use writeRecoveryNow to avoid waiting.
  void updateRecovery();
  bool writeRecoveryNow();
  [[nodiscard]] bool hasNewerRecovery() const noexcept;
  bool recover();
  bool discardRecovery();

  bool changeBaseDirectory(std::filesystem::path const& baseDirectory);
  // Rereads the deployment INI and every configured bundle. Publication is
  // atomic: malformed candidates and candidates that reject the open document
  // leave the current catalog, forms, document, and history untouched.
  bool reloadSchemas(std::filesystem::path const& iniPath);
  void reportFailure(std::string message);

  [[nodiscard]] bool hasDocument() const noexcept;
  [[nodiscard]] bool hasPath() const noexcept;
  [[nodiscard]] bool dirty() const noexcept;
  [[nodiscard]] bool canSave() const;
  [[nodiscard]] bool canSaveAs() const;
  [[nodiscard]] std::filesystem::path const& path() const noexcept;
  [[nodiscard]] std::filesystem::path const& baseDirectory() const noexcept;
  [[nodiscard]] std::optional<SourceRevision> const& sourceRevision() const noexcept;
  [[nodiscard]] EditorPreferences& preferences() noexcept;
  [[nodiscard]] EditorPreferences const& preferences() const noexcept;
  [[nodiscard]] std::string const& operationDiagnostic() const noexcept;
  [[nodiscard]] std::vector<wp::application::resourcesystem::ResourceManifestDiagnostic>
      const& structuralDiagnostics() const noexcept;
  [[nodiscard]] std::vector<SemanticDiagnostic> const& semanticDiagnostics()
      const noexcept;
  [[nodiscard]] std::string canonicalYaml() const;

  [[nodiscard]] std::vector<ResourceForm> const& resourceForms() const noexcept;
  [[nodiscard]] wp::application::resourcesystem::ResourceSchemaCatalogSnapshot const&
  schemaCatalog() const noexcept { return mCatalog; }
  [[nodiscard]] ResourceForm const* resourceForm(
      std::string const& resourceType) const noexcept;
  [[nodiscard]] std::vector<NamespaceSummary> namespaces() const;
  [[nodiscard]] std::vector<ResourceSummary> resources() const;
  [[nodiscard]] std::vector<InlineResourceSummary> inlineResources(
      std::string const& ownerNamespace, std::string const& ownerName) const;
  [[nodiscard]] std::vector<ResourceReferenceSelector> resourceReferences(
      std::string const& ownerNamespace, std::string const& ownerName) const;
  [[nodiscard]] std::vector<DependencyDiagnostic> dependencyDiagnostics() const;
  [[nodiscard]] std::vector<std::string> incomingReferences(
      std::string const& resourceNamespace, std::string const& name) const;
  [[nodiscard]] std::vector<ResourceReferenceChoice> draftReferenceChoices(
      std::string const& dependencyId = {}) const;
  [[nodiscard]] std::vector<DefinitionFactoryChoice> definitionFactories(
      std::string const& resourceNamespace, std::string const& name) const;
  [[nodiscard]] std::vector<NestedFormItem> nestedFormItems(
      std::string const& resourceNamespace, std::string const& name) const;
  [[nodiscard]] std::optional<DiagnosticNavigation> diagnosticNavigation(
      std::size_t diagnosticIndex) const;
  [[nodiscard]] std::optional<DiagnosticNavigation> semanticDiagnosticNavigation(
      std::size_t diagnosticIndex) const;

  // A named namespace stays outside the document and command history until
  // its first Resource is committed or moved into it.
  bool beginNamespaceDraft();
  void setNamespaceDraftName(std::string name);
  [[nodiscard]] NamespaceDraft const* namespaceDraft() const noexcept;
  [[nodiscard]] bool namespaceDraftValid() const noexcept;
  void cancelNamespaceDraft() noexcept;

  // A draft is local UI state: it does not alter the committed document or
  // history until commitDraft succeeds.
  bool beginDraft(std::string resourceType,
                  std::string resourceNamespace = {});
  void setDraftName(std::string name);
  bool selectDraftFile(std::filesystem::path const& selectedFile);
  bool setDraftReference(std::string resourceNamespace, std::string name);
  bool setDraftReference(std::string const& dependencyId,
                         std::string resourceNamespace, std::string name);
  [[nodiscard]] ResourceDraft const* draft() const noexcept;
  [[nodiscard]] bool draftValid() const noexcept;
  bool commitDraft();
  void cancelDraft() noexcept;

  bool renameNamespace(std::string const& currentName, std::string newName,
                       bool continuous = false);
  bool renameNamespaceAtPath(std::string const& namespacePath,
                             std::string newName, bool continuous = false);
  bool deleteNamespace(std::string const& name);
  bool renameResource(std::string const& resourceNamespace,
                      std::string const& currentName, std::string newName,
                      bool continuous = false);
  bool renameResourceAtPath(std::string const& resourcePath,
                            std::string newName, bool continuous = false);
  bool reorderResource(std::string const& resourceNamespace,
                       std::string const& name, std::size_t newIndex,
                       bool continuous = false);
  bool moveResource(std::string const& sourceNamespace,
                    std::string const& name,
                    std::string const& targetNamespace,
                    std::size_t targetIndex = static_cast<std::size_t>(-1),
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
  bool setResourceReference(std::string const& ownerNamespace,
                            std::string const& ownerName,
                            std::size_t dependencyIndex,
                            std::optional<std::pair<std::string, std::string>> target);
  bool renameInlineResource(std::string const& ownerNamespace,
                            std::string const& ownerName,
                            std::size_t dependencyIndex, std::string newName,
                            bool continuous = false);
  bool setInlineResourceFile(std::string const& ownerNamespace,
                             std::string const& ownerName,
                             std::size_t dependencyIndex,
                             std::filesystem::path const& selectedFile,
                             bool continuous = false);
  bool setInlineResourceOption(std::string const& ownerNamespace,
                               std::string const& ownerName,
                               std::size_t dependencyIndex,
                               std::string const& optionName,
                               std::optional<std::string> value,
                               bool continuous = false);
  bool promoteInlineResource(std::string const& ownerNamespace,
                             std::string const& ownerName,
                             std::size_t dependencyIndex,
                             std::string const& targetNamespace);
  bool addResourceDependency(
      std::string const& ownerNamespace, std::string const& ownerName,
      std::string dependencyId,
      std::pair<std::string, std::string> const& target);
  bool addDefinition(std::string const& resourceNamespace,
                     std::string const& name, std::string factoryType);
  bool setNestedAlternative(std::string const& resourceNamespace,
                            std::string const& name,
                            std::string const& itemPath,
                            std::string alternative);
  bool setNestedProperty(std::string const& resourceNamespace,
                         std::string const& name,
                         std::string const& itemPath,
                         std::string const& property,
                         std::optional<std::string> value,
                         bool continuous = false);
  bool addNestedItem(std::string const& resourceNamespace,
                     std::string const& name,
                     std::string const& collectionPath,
                     NestedCollectionKind kind);
  bool duplicateNestedItem(std::string const& resourceNamespace,
                           std::string const& name,
                           std::string const& itemPath);
  bool removeNestedItem(std::string const& resourceNamespace,
                        std::string const& name,
                        std::string const& itemPath);
  bool reorderNestedItem(std::string const& resourceNamespace,
                         std::string const& name,
                         std::string const& itemPath, std::size_t newIndex);
  bool setFramesAlternative(std::string const& resourceNamespace,
                            std::string const& name,
                            std::string const& framesPath,
                            bool imageSetFrames,
                            std::string imageSet = {});
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
  bool openWithBase(std::filesystem::path const& manifestPath,
                    std::optional<std::filesystem::path> baseDirectory);
  bool saveTo(std::filesystem::path const& manifestPath, bool allowOverwrite = false);
  void documentChanged();
  std::filesystem::path recoveryPath() const;
  bool removeRecovery();
  void setFailure(std::string message);
  bool applyCommittedYaml(std::string const& yaml);
  bool executeYamlCommand(std::string name, std::string yaml,
                          std::string mergeKey = {}, bool continuous = false);
  std::optional<std::string> portableSelectedFile(
      std::filesystem::path const& selectedFile, ResourceForm const& form);
  void refreshSemanticDiagnostics();
  void validateNamespaceDraft();
  void validateDraft();

  wp::application::resourcesystem::ResourceSchemaCatalogSnapshot mCatalog;
  std::unique_ptr<wp::application::resourcesystem::ResourceManifestDocument>
      mDocument;
  std::filesystem::path mPath;
  std::filesystem::path mBaseDirectory;
  std::vector<wp::application::resourcesystem::ResourceManifestDiagnostic>
      mStructuralDiagnostics;
  std::vector<SemanticDiagnostic> mSemanticDiagnostics;
  std::string mOperationDiagnostic;
  std::vector<ResourceForm> mResourceForms;
  std::optional<NamespaceDraft> mNamespaceDraft;
  std::optional<ResourceDraft> mDraft;
  mpp::app::CommandStack mCommands{256};
  bool mUnsavedDocument = false;
  EditorPreferences mPreferences;
  std::optional<SourceRevision> mSourceRevision;
  ExternalChangeState mExternalChange = ExternalChangeState::unchanged;
  bool mHasNewerRecovery = false;
  std::chrono::steady_clock::time_point mRecoveryDue{};
  std::uint64_t mRecoveryGeneration = 0;
  std::uint64_t mWrittenRecoveryGeneration = 0;
};

bool runDocumentTests(std::string* failure);
bool runAuthoringTests(std::string* failure);
bool runOrganizationTests(std::string* failure);
bool runDependencyAuthoringTests(std::string* failure);
bool runCompositeAuthoringTests(std::string* failure);
bool runAdvancedAuthoringTests(std::string* failure);
bool runSchemaIntegrationTests(std::string* failure);
bool runSemanticRepairTests(std::string* failure);
bool runResilienceTests(std::string* failure);

}  // namespace resource_manager
