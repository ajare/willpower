#include "ResourceManagerShell.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#endif

namespace resource_manager {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace wp::application::resourcesystem;

std::string displayPath(fs::path const& path) {
  auto const utf8 = path.generic_u8string();
  return std::string(reinterpret_cast<char const*>(utf8.data()), utf8.size());
}

bool accessibleDirectory(fs::path const& path, fs::path& canonical,
                         std::string& failure) {
  std::error_code error;
  if (!fs::is_directory(path, error)) {
    failure = "Base directory is not an accessible directory: " +
              displayPath(path);
    if (error) failure += ": " + error.message();
    return false;
  }
  canonical = fs::canonical(path, error);
  if (error) {
    failure = "Could not canonicalize base directory '" + displayPath(path) +
              "': " + error.message();
    return false;
  }
  return true;
}

std::string validationMessage(ResourceManifestValidationResult const& result) {
  if (result.diagnostics.empty()) return "Resource Manifest is not structurally valid.";
  auto const& diagnostic = result.diagnostics.front();
  std::ostringstream message;
  message << diagnostic.message;
  if (!diagnostic.instancePath.empty()) message << " at " << diagnostic.instancePath;
  if (diagnostic.line > 0) {
    message << " (line " << diagnostic.line;
    if (diagnostic.column > 0) message << ", column " << diagnostic.column;
    message << ')';
  }
  if (result.diagnostics.size() > 1) {
    message << " (and " << (result.diagnostics.size() - 1)
            << " more diagnostic(s))";
  }
  return message.str();
}

fs::path temporarySibling(fs::path const& destination) {
  auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  auto const name = destination.filename().string();
  for (unsigned int attempt = 0; attempt != 100; ++attempt) {
    auto candidate = parent /
                     ("." + name + ".tmp-" + std::to_string(stamp) + "-" +
                      std::to_string(attempt));
    std::error_code error;
    if (!fs::exists(candidate, error)) return candidate;
  }
  throw std::runtime_error("Could not reserve a sibling temporary file.");
}

void replaceAtomically(fs::path const& temporary, fs::path const& destination) {
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    auto const code = GetLastError();
    throw std::system_error(static_cast<int>(code), std::system_category(),
                            "Could not atomically replace Resource Manifest");
  }
#else
  if (::rename(temporary.c_str(), destination.c_str()) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "Could not atomically replace Resource Manifest");
  }
#endif
}

void writeAtomically(fs::path const& destination, std::string const& bytes) {
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  std::error_code error;
  if (!fs::is_directory(parent, error)) {
    throw std::runtime_error("Save directory is not accessible: " +
                             displayPath(parent));
  }

  auto temporary = temporarySibling(destination);
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Could not create sibling temporary file '" +
                               displayPath(temporary) + "'.");
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      throw std::runtime_error("Could not write sibling temporary file '" +
                               displayPath(temporary) + "'.");
    }
    output.close();
    replaceAtomically(temporary, destination);
  } catch (...) {
    fs::remove(temporary, error);
    throw;
  }
}

std::string readBytes(fs::path const& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::vector<YAML::Node> collectionItems(YAML::Node const& collection) {
  std::vector<YAML::Node> result;
  if (!collection || collection.IsNull()) return result;
  if (collection.IsSequence()) {
    result.reserve(collection.size());
    for (auto const& item : collection) result.push_back(YAML::Clone(item));
  } else {
    result.push_back(YAML::Clone(collection));
  }
  return result;
}

void setCollection(YAML::Node parent, char const* key,
                   std::vector<YAML::Node> const& items) {
  if (items.empty()) {
    parent.remove(key);
  } else if (items.size() == 1U) {
    parent[key] = items.front();
  } else {
    YAML::Node sequence(YAML::NodeType::Sequence);
    for (auto const& item : items) sequence.push_back(item);
    parent[key] = sequence;
  }
}

// YAML::Node assignment mutates the aliased node rather than behaving like a
// value assignment. Rebuild vectors when changing their shape so std::vector's
// shifting assignments cannot duplicate or overwrite neighboring nodes.
void eraseCollectionItem(std::vector<YAML::Node>& items, std::size_t index) {
  std::vector<YAML::Node> replacement;
  replacement.reserve(items.size() - 1U);
  for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
    if (itemIndex != index) replacement.push_back(YAML::Clone(items[itemIndex]));
  }
  items.swap(replacement);
}

void insertCollectionItem(std::vector<YAML::Node>& items, std::size_t index,
                          YAML::Node const& item) {
  std::vector<YAML::Node> replacement;
  replacement.reserve(items.size() + 1U);
  for (std::size_t itemIndex = 0; itemIndex <= items.size(); ++itemIndex) {
    if (itemIndex == index) replacement.push_back(YAML::Clone(item));
    if (itemIndex < items.size())
      replacement.push_back(YAML::Clone(items[itemIndex]));
  }
  items.swap(replacement);
}

std::string scalar(YAML::Node const& node, char const* key) {
  auto value = node[key];
  return value && value.IsScalar() ? value.as<std::string>() : std::string{};
}

std::string resourceIdentity(YAML::Node const& node) {
  auto name = scalar(node, "name");
  return name.empty() ? scalar(node, "location") : name;
}

struct ResourceCollection {
  YAML::Node owner;
  std::vector<YAML::Node> resources;
  std::vector<YAML::Node> namespaces;
  std::size_t namespaceIndex = 0;
  bool named = false;
};

std::optional<ResourceCollection> findResourceCollection(
    YAML::Node root, std::string const& resourceNamespace) {
  auto resourcesRoot = root["Resources"];
  if (!resourcesRoot || !resourcesRoot.IsMap()) return {};
  if (resourceNamespace.empty()) {
    return ResourceCollection{resourcesRoot,
                              collectionItems(resourcesRoot["Resource"]), {}, 0,
                              false};
  }
  auto namespaces = collectionItems(resourcesRoot["Namespace"]);
  for (std::size_t index = 0; index < namespaces.size(); ++index) {
    if (scalar(namespaces[index], "name") == resourceNamespace) {
      return ResourceCollection{namespaces[index],
                                collectionItems(namespaces[index]["Resource"]),
                                std::move(namespaces), index, true};
    }
  }
  return {};
}

void publishCollection(YAML::Node root, ResourceCollection& collection) {
  setCollection(collection.owner, "Resource", collection.resources);
  if (collection.named) {
    collection.namespaces[collection.namespaceIndex] = collection.owner;
    setCollection(root["Resources"], "Namespace", collection.namespaces);
  }
}

std::optional<std::size_t> findResourceIndex(ResourceCollection const& collection,
                                             std::string const& name) {
  for (std::size_t index = 0; index < collection.resources.size(); ++index) {
    if (resourceIdentity(collection.resources[index]) == name) return index;
  }
  return {};
}

std::string emitYaml(YAML::Node const& root) {
  YAML::Emitter output;
  output.SetIndent(2);
  output << YAML::Block << root;
  if (!output.good()) throw std::runtime_error(output.GetLastError());
  return std::string(output.c_str(), output.size()) + "\n";
}

bool validResourceName(std::string const& name) {
  return !name.empty() && name.find('/') == std::string::npos;
}

bool validNamespaceName(std::string const& name) {
  return !name.empty() && name.find('/') == std::string::npos;
}

struct ResourceIdentity {
  std::string resourceNamespace;
  std::string name;

  bool operator==(ResourceIdentity const&) const = default;
};

ResourceIdentity parseReference(std::string const& reference,
                                std::string const& ownerNamespace) {
  auto const separator = reference.find('/');
  if (separator == std::string::npos) return {ownerNamespace, reference};
  return {reference.substr(0, separator), reference.substr(separator + 1U)};
}

std::string formatReference(ResourceIdentity const& target,
                            std::string const& ownerNamespace) {
  if (target.resourceNamespace == ownerNamespace) return target.name;
  return target.resourceNamespace + "/" + target.name;
}

template <typename Visitor>
void forEachStandardReference(YAML::Node const& root, Visitor visitor) {
  auto visitCollection = [&](std::string const& resourceNamespace,
                             YAML::Node const& collection) {
    for (auto const& resource : collectionItems(collection)) {
      ResourceIdentity const owner{resourceNamespace, resourceIdentity(resource)};
      auto dependentResources = resource["DependentResources"];
      if (!dependentResources || !dependentResources.IsMap()) continue;
      for (auto const& dependency :
           collectionItems(dependentResources["DependentResource"])) {
        auto reference = scalar(dependency, "ref");
        if (!reference.empty()) {
          visitor(owner, parseReference(reference, resourceNamespace));
        }
      }
    }
  };

  auto resourcesRoot = root["Resources"];
  visitCollection({}, resourcesRoot["Resource"]);
  for (auto const& item : collectionItems(resourcesRoot["Namespace"])) {
    visitCollection(scalar(item, "name"), item["Resource"]);
  }
}

template <typename Transform>
void rewriteStandardReferences(YAML::Node root, Transform transform) {
  auto rewriteCollection = [&](YAML::Node owner,
                               std::string const& resourceNamespace) {
    auto resources = collectionItems(owner["Resource"]);
    for (auto& resource : resources) {
      ResourceIdentity const oldOwner{resourceNamespace,
                                      resourceIdentity(resource)};
      auto const newOwner = transform(oldOwner);
      auto dependentResources = resource["DependentResources"];
      if (!dependentResources || !dependentResources.IsMap()) continue;
      auto dependencies =
          collectionItems(dependentResources["DependentResource"]);
      for (auto& dependency : dependencies) {
        auto reference = scalar(dependency, "ref");
        if (reference.empty()) continue;
        auto const oldTarget = parseReference(reference, resourceNamespace);
        auto const newTarget = transform(oldTarget);
        if (newOwner == oldOwner && newTarget == oldTarget) continue;
        dependency["ref"] =
            formatReference(newTarget, newOwner.resourceNamespace);
      }
      setCollection(dependentResources, "DependentResource", dependencies);
      resource["DependentResources"] = dependentResources;
    }
    setCollection(owner, "Resource", resources);
  };

  auto resourcesRoot = root["Resources"];
  rewriteCollection(resourcesRoot, {});
  auto namespaces = collectionItems(resourcesRoot["Namespace"]);
  for (auto& item : namespaces) {
    rewriteCollection(item, scalar(item, "name"));
  }
  setCollection(resourcesRoot, "Namespace", namespaces);
  root["Resources"] = resourcesRoot;
}

std::size_t namespaceCount(YAML::Node const& root, std::string const& name) {
  auto const namespaces = collectionItems(root["Resources"]["Namespace"]);
  return static_cast<std::size_t>(std::count_if(
      namespaces.begin(), namespaces.end(), [&](YAML::Node const& item) {
        return scalar(item, "name") == name;
      }));
}

std::size_t resourceCount(ResourceCollection const& collection,
                          std::string const& name) {
  return static_cast<std::size_t>(std::count_if(
      collection.resources.begin(), collection.resources.end(),
      [&](YAML::Node const& resource) {
        return resourceIdentity(resource) == name;
      }));
}

bool hasIncomingReference(YAML::Node const& root,
                          ResourceIdentity const& target,
                          std::function<bool(ResourceIdentity const&)> sourceIsRemoved) {
  bool incoming = false;
  forEachStandardReference(
      root, [&](ResourceIdentity const& source,
                ResourceIdentity const& referenced) {
        if (referenced == target && !sourceIsRemoved(source)) incoming = true;
      });
  return incoming;
}

bool equalPathComponent(fs::path const& left, fs::path const& right) {
#if defined(_WIN32)
  auto l = left.native();
  auto r = right.native();
  std::transform(l.begin(), l.end(), l.begin(), ::towlower);
  std::transform(r.begin(), r.end(), r.begin(), ::towlower);
  return l == r;
#else
  return left == right;
#endif
}

bool containedBy(fs::path const& base, fs::path const& target) {
  auto basePart = base.begin();
  auto targetPart = target.begin();
  for (; basePart != base.end(); ++basePart, ++targetPart) {
    if (targetPart == target.end() ||
        !equalPathComponent(*basePart, *targetPart)) {
      return false;
    }
  }
  return true;
}

std::vector<ResourceForm> loadResourceForms(
    ResourceSchemaCatalogSnapshot const& catalog) {
  static std::set<std::string> const authoredTypes{
      "TextFile", "XmlFile", "Shader", "AudioBank", "Image"};
  std::vector<ResourceForm> result;
  for (auto const& entry : catalog.entries()) {
    if (entry.kind != ResourceSchemaKind::resourceType ||
        !entry.factoryType.empty() || !authoredTypes.contains(entry.resourceType)) {
      continue;
    }
    Json schema;
    try {
      schema = Json::parse(entry.contents);
      auto const& resource = schema.at("definitions").at("resource");
      auto const& ownProperties = resource.at("allOf").at(1).at("properties");
      auto const& location = ownProperties.at("location");
      if (location.at("x-willpower-editor-version") != "1.0" ||
          location.at("x-willpower-widget") != "file" ||
          !location.at("x-willpower-file-kind").is_string() ||
          !location.at("x-willpower-file-extensions").is_array()) {
        throw std::runtime_error("malformed file editor annotation");
      }
      ResourceForm form;
      form.resourceType = entry.resourceType;
      form.title = schema.value("title", entry.resourceType);
      form.fileProperty = "location";
      form.fileKind = location.at("x-willpower-file-kind").get<std::string>();
      form.fileExtensions =
          location.at("x-willpower-file-extensions").get<std::vector<std::string>>();
      if (form.fileKind.empty() || form.fileExtensions.empty() ||
          std::any_of(form.fileExtensions.begin(), form.fileExtensions.end(),
                      [](std::string const& extension) {
                        return extension.empty() || extension.front() == '.' ||
                               extension.find_first_of(";*\\/") !=
                                   std::string::npos;
                      })) {
        throw std::runtime_error("invalid file selector metadata");
      }

      if (schema.at("definitions").contains("option")) {
        for (auto const& branch :
             schema.at("definitions").at("option").at("oneOf")) {
          ResourceOptionForm option;
          auto const& properties = branch.at("properties");
          option.name = properties.at("name").at("enum").at(0).get<std::string>();
          auto const& value = properties.at("value");
          if (value.contains("enum")) {
            option.values = value.at("enum").get<std::vector<std::string>>();
          } else if (value.value("$ref", std::string{}).ends_with(
                         "/definitions/boolean")) {
            option.boolean = true;
          }
          form.options.push_back(std::move(option));
        }
      }
      result.push_back(std::move(form));
    } catch (std::exception const& error) {
      throw std::runtime_error("Resource Type schema '" + entry.resourceType +
                               "' cannot generate a file-backed form: " +
                               error.what());
    }
  }
  if (result.size() != authoredTypes.size()) {
    throw std::runtime_error(
        "The built-in catalog does not contain all five file-backed Resource forms.");
  }
  return result;
}

class ReplaceYamlCommand final : public mpp::app::EditorCommand {
 public:
  using Apply = std::function<bool(std::string const&)>;

  ReplaceYamlCommand(std::string name, std::string mergeKey, Apply apply,
                     std::string before, std::string after)
      : mName(std::move(name)),
        mMergeKey(std::move(mergeKey)),
        mApply(std::move(apply)),
        mBefore(std::move(before)),
        mAfter(std::move(after)) {}

  std::string const& name() const override { return mName; }
  void execute() override {
    if (!mApply(mAfter)) throw std::runtime_error("Could not apply validated Resource edit.");
  }
  void undo() override {
    if (!mApply(mBefore)) throw std::runtime_error("Could not undo validated Resource edit.");
  }
  bool merge(mpp::app::EditorCommand const& other) override {
    auto const* replacement = dynamic_cast<ReplaceYamlCommand const*>(&other);
    if (!replacement || mMergeKey.empty() || replacement->mMergeKey != mMergeKey) {
      return false;
    }
    mAfter = replacement->mAfter;
    return true;
  }

 private:
  std::string mName;
  std::string mMergeKey;
  Apply mApply;
  std::string mBefore;
  std::string mAfter;
};

}  // namespace

bool hasYamlExtension(fs::path const& path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".yaml" || extension == ".yml";
}

ManifestWorkspace::ManifestWorkspace()
    : mCatalog(ResourceSchemaCatalog::builtIn().snapshot()),
      mResourceForms(loadResourceForms(mCatalog)) {}

bool ManifestWorkspace::createNew(fs::path const& baseDirectory) {
  fs::path canonicalBase;
  std::string failure;
  if (!accessibleDirectory(baseDirectory, canonicalBase, failure)) {
    setFailure(std::move(failure));
    return false;
  }

  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::parse("Resources: {}\n", "Untitled Resource Manifest"));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Could not create an empty Resource Manifest: " +
               validationMessage(validation));
    return false;
  }

  mDocument = std::move(candidate);
  mPath.clear();
  mBaseDirectory = std::move(canonicalBase);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  mNamespaceDraft.reset();
  mDraft.reset();
  mCommands.clear();
  mUnsavedDocument = true;
  return true;
}

bool ManifestWorkspace::open(fs::path const& manifestPath) {
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }

  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::load(manifestPath));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = validation.diagnostics;
    setFailure("Could not open Resource Manifest '" + displayPath(manifestPath) +
               "': " + validationMessage(validation));
    return false;
  }

  std::error_code error;
  auto canonicalPath = fs::canonical(manifestPath, error);
  if (error) {
    setFailure("Could not canonicalize Resource Manifest path '" +
               displayPath(manifestPath) + "': " + error.message());
    return false;
  }
  fs::path canonicalBase;
  std::string failure;
  if (!accessibleDirectory(canonicalPath.parent_path(), canonicalBase, failure)) {
    setFailure(std::move(failure));
    return false;
  }

  mDocument = std::move(candidate);
  mPath = std::move(canonicalPath);
  mBaseDirectory = std::move(canonicalBase);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  mNamespaceDraft.reset();
  mDraft.reset();
  mCommands.clear();
  mCommands.markSavePoint();
  mUnsavedDocument = false;
  return true;
}

bool ManifestWorkspace::save() {
  if (mPath.empty()) {
    setFailure("Save As is required for an unsaved Resource Manifest.");
    return false;
  }
  return saveTo(mPath);
}

bool ManifestWorkspace::saveAs(fs::path const& manifestPath) {
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }
  return saveTo(manifestPath);
}

bool ManifestWorkspace::saveTo(fs::path const& manifestPath) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }

  auto validation = mDocument->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Resource Manifest is not structurally valid and cannot be saved.");
    return false;
  }

  try {
    auto canonical = mDocument->serializeCanonical();
    writeAtomically(manifestPath, canonical);

    auto saved = std::make_unique<ResourceManifestDocument>(
        ResourceManifestDocument::load(manifestPath));
    auto savedValidation = saved->validate(mCatalog);
    if (!savedValidation.valid() || saved->serializeCanonical() != canonical) {
      throw std::runtime_error(
          "Saved Resource Manifest failed canonical round-trip validation.");
    }
    std::error_code error;
    auto canonicalPath = fs::canonical(manifestPath, error);
    if (error) canonicalPath = fs::absolute(manifestPath).lexically_normal();
    mDocument = std::move(saved);
    mPath = std::move(canonicalPath);
    mStructuralDiagnostics.clear();
    mOperationDiagnostic.clear();
    mCommands.markSavePoint();
    mUnsavedDocument = false;
    return true;
  } catch (std::exception const& exception) {
    setFailure("Could not save Resource Manifest '" + displayPath(manifestPath) +
               "': " + exception.what());
    return false;
  }
}

void ManifestWorkspace::reportFailure(std::string message) {
  setFailure(std::move(message));
}

bool ManifestWorkspace::hasDocument() const noexcept { return mDocument != nullptr; }
bool ManifestWorkspace::hasPath() const noexcept { return !mPath.empty(); }
bool ManifestWorkspace::dirty() const noexcept {
  return mDocument && (mUnsavedDocument || mCommands.dirty());
}
fs::path const& ManifestWorkspace::path() const noexcept { return mPath; }
fs::path const& ManifestWorkspace::baseDirectory() const noexcept {
  return mBaseDirectory;
}
std::string const& ManifestWorkspace::operationDiagnostic() const noexcept {
  return mOperationDiagnostic;
}
std::vector<ResourceManifestDiagnostic> const&
ManifestWorkspace::structuralDiagnostics() const noexcept {
  return mStructuralDiagnostics;
}
std::string ManifestWorkspace::canonicalYaml() const {
  return mDocument ? mDocument->serializeCanonical() : std::string{};
}

std::vector<ResourceForm> const& ManifestWorkspace::resourceForms() const noexcept {
  return mResourceForms;
}

ResourceForm const* ManifestWorkspace::resourceForm(
    std::string const& resourceType) const noexcept {
  auto found = std::find_if(mResourceForms.begin(), mResourceForms.end(),
                            [&](ResourceForm const& form) {
                              return form.resourceType == resourceType;
                            });
  return found == mResourceForms.end() ? nullptr : &*found;
}

std::vector<NamespaceSummary> ManifestWorkspace::namespaces() const {
  std::vector<NamespaceSummary> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto resourcesRoot = root["Resources"];
  result.push_back(
      NamespaceSummary{{}, collectionItems(resourcesRoot["Resource"]).size(), true});
  for (auto const& item : collectionItems(resourcesRoot["Namespace"])) {
    result.push_back(NamespaceSummary{scalar(item, "name"),
                                      collectionItems(item["Resource"]).size()});
  }
  if (mNamespaceDraft) {
    result.push_back(NamespaceSummary{mNamespaceDraft->name, 0, false, true});
  }
  return result;
}

std::vector<ResourceSummary> ManifestWorkspace::resources() const {
  std::vector<ResourceSummary> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto append = [&](std::string const& resourceNamespace,
                    YAML::Node const& collection) {
    for (auto const& resource : collectionItems(collection)) {
      ResourceSummary summary;
      summary.resourceNamespace = resourceNamespace;
      summary.resourceType = scalar(resource, "type");
      summary.name = scalar(resource, "name");
      summary.explicitName = !summary.name.empty();
      summary.location = scalar(resource, "location");
      for (auto const& option : collectionItems(resource["Option"])) {
        summary.options.emplace_back(scalar(option, "name"),
                                     scalar(option, "value"));
      }
      if (summary.name.empty()) summary.name = summary.location;
      summary.editable = resourceForm(summary.resourceType) != nullptr;
      result.push_back(std::move(summary));
    }
  };
  auto resourcesRoot = root["Resources"];
  append({}, resourcesRoot["Resource"]);
  for (auto const& item : collectionItems(resourcesRoot["Namespace"])) {
    append(scalar(item, "name"), item["Resource"]);
  }
  return result;
}

bool ManifestWorkspace::beginNamespaceDraft() {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  if (mNamespaceDraft || mDraft) {
    setFailure("Finish or cancel the current draft first.");
    return false;
  }
  mNamespaceDraft.emplace();
  validateNamespaceDraft();
  mOperationDiagnostic.clear();
  return true;
}

void ManifestWorkspace::setNamespaceDraftName(std::string name) {
  if (!mNamespaceDraft || mDraft) return;
  mNamespaceDraft->name = std::move(name);
  validateNamespaceDraft();
}

NamespaceDraft const* ManifestWorkspace::namespaceDraft() const noexcept {
  return mNamespaceDraft ? &*mNamespaceDraft : nullptr;
}

bool ManifestWorkspace::namespaceDraftValid() const noexcept {
  return mNamespaceDraft && mNamespaceDraft->validationMessage.empty();
}

void ManifestWorkspace::cancelNamespaceDraft() noexcept {
  mDraft.reset();
  mNamespaceDraft.reset();
}

void ManifestWorkspace::validateNamespaceDraft() {
  if (!mNamespaceDraft) return;
  if (!validNamespaceName(mNamespaceDraft->name)) {
    mNamespaceDraft->validationMessage =
        "A namespace name is required and cannot contain '/'.";
    return;
  }
  auto root = YAML::Load(canonicalYaml());
  if (namespaceCount(root, mNamespaceDraft->name) != 0U) {
    mNamespaceDraft->validationMessage = "Namespace names must be unique.";
    return;
  }
  mNamespaceDraft->validationMessage.clear();
}

bool ManifestWorkspace::beginDraft(std::string resourceType,
                                   std::string resourceNamespace) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  if (mDraft) {
    setFailure("Finish or cancel the current Resource draft first.");
    return false;
  }
  if (!resourceForm(resourceType)) {
    setFailure("Resource Type '" + resourceType +
               "' has no supported catalogued authoring form.");
    return false;
  }
  if (mNamespaceDraft &&
      (!namespaceDraftValid() || mNamespaceDraft->name != resourceNamespace)) {
    setFailure("The namespace draft must receive the next Resource.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  bool const pendingNamespace =
      mNamespaceDraft && namespaceDraftValid() &&
      mNamespaceDraft->name == resourceNamespace;
  if (!resourceNamespace.empty() && !pendingNamespace &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Namespace '" + resourceNamespace +
               "' was not found uniquely; duplicate namespaces are ambiguous.");
    return false;
  }
  if (!findResourceCollection(root, resourceNamespace) && !pendingNamespace) {
    setFailure("Namespace '" + resourceNamespace + "' does not exist.");
    return false;
  }
  mDraft = ResourceDraft{std::move(resourceNamespace), std::move(resourceType)};
  validateDraft();
  mOperationDiagnostic.clear();
  return true;
}

void ManifestWorkspace::setDraftName(std::string name) {
  if (!mDraft) return;
  mDraft->name = std::move(name);
  validateDraft();
}

bool ManifestWorkspace::selectDraftFile(fs::path const& selectedFile) {
  if (!mDraft) {
    setFailure("No Resource draft is active.");
    return false;
  }
  auto relative = portableSelectedFile(selectedFile);
  if (!relative) {
    validateDraft();
    return false;
  }
  mDraft->location = std::move(*relative);
  validateDraft();
  mOperationDiagnostic.clear();
  return true;
}

ResourceDraft const* ManifestWorkspace::draft() const noexcept {
  return mDraft ? &*mDraft : nullptr;
}

bool ManifestWorkspace::draftValid() const noexcept {
  return mDraft && mDraft->validationMessage.empty();
}

void ManifestWorkspace::validateDraft() {
  if (!mDraft) return;
  if (!validResourceName(mDraft->name)) {
    mDraft->validationMessage =
        "A Resource name is required and cannot contain '/'.";
    return;
  }
  auto allResources = resources();
  if (std::any_of(allResources.begin(), allResources.end(),
                  [&](ResourceSummary const& resource) {
                    return resource.resourceNamespace == mDraft->resourceNamespace &&
                           resource.name == mDraft->name;
                  })) {
    mDraft->validationMessage =
        "Resource names must be unique within their namespace.";
    return;
  }
  if (mDraft->location.empty()) {
    mDraft->validationMessage = "Select the required source file.";
    return;
  }
  mDraft->validationMessage.clear();
}

bool ManifestWorkspace::commitDraft() {
  if (!mDraft) {
    setFailure("No Resource draft is active.");
    return false;
  }
  validateDraft();
  if (!draftValid()) {
    setFailure("Resource draft is invalid: " + mDraft->validationMessage);
    return false;
  }

  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, mDraft->resourceNamespace);
  bool const createsNamespace =
      !collection && mNamespaceDraft && namespaceDraftValid() &&
      mNamespaceDraft->name == mDraft->resourceNamespace;
  YAML::Node resource(YAML::NodeType::Map);
  resource["type"] = mDraft->resourceType;
  resource["name"] = mDraft->name;
  resource["location"] = mDraft->location;
  if (createsNamespace) {
    auto resourcesRoot = root["Resources"];
    auto namespaces = collectionItems(resourcesRoot["Namespace"]);
    YAML::Node resourceNamespace(YAML::NodeType::Map);
    resourceNamespace["name"] = mDraft->resourceNamespace;
    setCollection(resourceNamespace, "Resource", {resource});
    namespaces.push_back(std::move(resourceNamespace));
    setCollection(resourcesRoot, "Namespace", namespaces);
  } else if (collection) {
    collection->resources.push_back(std::move(resource));
    publishCollection(root, *collection);
  } else {
    setFailure("The draft namespace no longer exists.");
    return false;
  }
  auto const commandName = createsNamespace
                               ? "Create namespace and " + mDraft->resourceType +
                                     " Resource"
                               : "Create " + mDraft->resourceType + " Resource";
  if (!executeYamlCommand(commandName, emitYaml(root))) return false;
  mDraft.reset();
  if (createsNamespace) mNamespaceDraft.reset();
  return true;
}

void ManifestWorkspace::cancelDraft() noexcept { mDraft.reset(); }

bool ManifestWorkspace::renameNamespace(std::string const& currentName,
                                        std::string newName, bool continuous) {
  if (currentName.empty()) {
    setFailure("The default namespace is permanent and cannot be renamed.");
    return false;
  }
  if (!validNamespaceName(newName)) {
    setFailure("A namespace name is required and cannot contain '/'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  if (namespaceCount(root, currentName) != 1U) {
    setFailure("Namespace '" + currentName +
               "' was not found uniquely; duplicate namespaces are ambiguous.");
    return false;
  }
  if (newName != currentName && namespaceCount(root, newName) != 0U) {
    setFailure("Namespace names must be unique.");
    return false;
  }
  if (newName == currentName) return true;

  auto beforeCollection = findResourceCollection(root, currentName);
  auto const namespaceIndex = beforeCollection->namespaceIndex;
  rewriteStandardReferences(root, [&](ResourceIdentity identity) {
    if (identity.resourceNamespace == currentName)
      identity.resourceNamespace = newName;
    return identity;
  });
  auto collection = findResourceCollection(root, currentName);
  if (!collection) {
    setFailure("Namespace '" + currentName + "' was not found.");
    return false;
  }
  collection->owner["name"] = std::move(newName);
  publishCollection(root, *collection);
  return executeYamlCommand("Rename namespace", emitYaml(root),
                            "namespace:" + std::to_string(namespaceIndex),
                            continuous);
}

bool ManifestWorkspace::deleteNamespace(std::string const& name) {
  if (name.empty()) {
    setFailure("The default namespace is permanent and cannot be deleted.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  if (namespaceCount(root, name) != 1U) {
    setFailure("Namespace '" + name +
               "' was not found uniquely; duplicate namespaces are ambiguous.");
    return false;
  }
  bool blocked = false;
  forEachStandardReference(
      root, [&](ResourceIdentity const& source,
                ResourceIdentity const& target) {
        if (source.resourceNamespace != name &&
            target.resourceNamespace == name) {
          blocked = true;
        }
      });
  if (blocked) {
    setFailure("Namespace '" + name +
               "' cannot be deleted while Resources outside it reference its "
               "contents.");
    return false;
  }

  auto collection = findResourceCollection(root, name);
  eraseCollectionItem(collection->namespaces, collection->namespaceIndex);
  setCollection(root["Resources"], "Namespace", collection->namespaces);
  return executeYamlCommand("Delete namespace", emitYaml(root));
}

bool ManifestWorkspace::renameResource(std::string const& resourceNamespace,
                                       std::string const& currentName,
                                       std::string newName, bool continuous) {
  if (!validResourceName(newName)) {
    setFailure("A Resource name is required and cannot contain '/'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  if (!resourceNamespace.empty() &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Resource namespace was not found uniquely; duplicate namespaces "
               "are ambiguous.");
    return false;
  }
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*collection, currentName) != 1U) {
    setFailure("Resource '" + currentName +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  auto index = findResourceIndex(*collection, currentName);
  if (newName != currentName && resourceCount(*collection, newName) != 0U) {
    setFailure("Resource names must be unique within their namespace.");
    return false;
  }

  ResourceIdentity const oldIdentity{resourceNamespace, currentName};
  ResourceIdentity const newIdentity{resourceNamespace, newName};
  rewriteStandardReferences(root, [&](ResourceIdentity identity) {
    return identity == oldIdentity ? newIdentity : identity;
  });
  collection = findResourceCollection(root, resourceNamespace);
  collection->resources[*index]["name"] = std::move(newName);
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Rename Resource", emitYaml(root),
      "name:" + resourceNamespace + ":" + std::to_string(*index), continuous);
}

bool ManifestWorkspace::reorderResource(
    std::string const& resourceNamespace, std::string const& name,
    std::size_t newIndex, bool continuous) {
  auto root = YAML::Load(canonicalYaml());
  if (!resourceNamespace.empty() &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Resource namespace was not found uniquely; duplicate namespaces "
               "are ambiguous.");
    return false;
  }
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*collection, name) != 1U) {
    setFailure("Resource '" + name +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  auto currentIndex = *findResourceIndex(*collection, name);
  if (newIndex >= collection->resources.size()) {
    setFailure("Resource reorder position is outside its namespace.");
    return false;
  }
  if (newIndex == currentIndex) return true;
  auto resource = YAML::Clone(collection->resources[currentIndex]);
  eraseCollectionItem(collection->resources, currentIndex);
  insertCollectionItem(collection->resources, newIndex, resource);
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Reorder Resource", emitYaml(root),
      "order:" + resourceNamespace + ":" + name, continuous);
}

bool ManifestWorkspace::moveResource(
    std::string const& sourceNamespace, std::string const& name,
    std::string const& targetNamespace, std::size_t targetIndex,
    bool continuous) {
  if (sourceNamespace == targetNamespace) {
    auto root = YAML::Load(canonicalYaml());
    auto collection = findResourceCollection(root, sourceNamespace);
    if (!collection || collection->resources.empty()) {
      setFailure("Resource namespace was not found.");
      return false;
    }
    if (targetIndex == static_cast<std::size_t>(-1))
      targetIndex = collection->resources.size() - 1U;
    return reorderResource(sourceNamespace, name, targetIndex, continuous);
  }

  auto root = YAML::Load(canonicalYaml());
  if ((!sourceNamespace.empty() &&
       namespaceCount(root, sourceNamespace) != 1U) ||
      (!targetNamespace.empty() &&
       !(mNamespaceDraft && namespaceDraftValid() &&
         mNamespaceDraft->name == targetNamespace) &&
       namespaceCount(root, targetNamespace) != 1U)) {
    setFailure("Source or target namespace was not found uniquely; duplicate "
               "namespaces are ambiguous.");
    return false;
  }
  auto source = findResourceCollection(root, sourceNamespace);
  if (!source) {
    setFailure("Source Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*source, name) != 1U) {
    setFailure("Resource '" + name +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  bool const createsNamespace =
      mNamespaceDraft && namespaceDraftValid() &&
      mNamespaceDraft->name == targetNamespace;
  auto target = findResourceCollection(root, targetNamespace);
  if (!target && !createsNamespace) {
    setFailure("Target Resource namespace was not found.");
    return false;
  }
  if (target && resourceCount(*target, name) != 0U) {
    setFailure("Moving the Resource would create a duplicate identity.");
    return false;
  }

  ResourceIdentity const oldIdentity{sourceNamespace, name};
  ResourceIdentity const newIdentity{targetNamespace, name};
  rewriteStandardReferences(root, [&](ResourceIdentity identity) {
    return identity == oldIdentity ? newIdentity : identity;
  });

  source = findResourceCollection(root, sourceNamespace);
  auto sourceIndex = *findResourceIndex(*source, name);
  auto movedResource = YAML::Clone(source->resources[sourceIndex]);
  if (scalar(movedResource, "name").empty()) {
    if (!validResourceName(name)) {
      setFailure("An inferred Resource name containing '/' must be renamed before "
                 "it can be moved.");
      return false;
    }
    movedResource["name"] = name;
  }
  eraseCollectionItem(source->resources, sourceIndex);
  if (source->named && source->resources.empty()) {
    eraseCollectionItem(source->namespaces, source->namespaceIndex);
    setCollection(root["Resources"], "Namespace", source->namespaces);
  } else {
    publishCollection(root, *source);
  }

  target = findResourceCollection(root, targetNamespace);
  if (!target) {
    auto resourcesRoot = root["Resources"];
    auto namespaces = collectionItems(resourcesRoot["Namespace"]);
    YAML::Node item(YAML::NodeType::Map);
    item["name"] = targetNamespace;
    setCollection(item, "Resource", {movedResource});
    namespaces.push_back(std::move(item));
    setCollection(resourcesRoot, "Namespace", namespaces);
  } else {
    if (targetIndex == static_cast<std::size_t>(-1) ||
        targetIndex > target->resources.size()) {
      targetIndex = target->resources.size();
    }
    insertCollectionItem(target->resources, targetIndex, movedResource);
    publishCollection(root, *target);
  }

  auto const commandName = source->named && source->resources.empty()
                               ? "Move Resource and remove namespace"
                               : "Move Resource";
  if (!executeYamlCommand(commandName, emitYaml(root),
                          "move:" + name, continuous)) {
    return false;
  }
  if (createsNamespace) mNamespaceDraft.reset();
  return true;
}

bool ManifestWorkspace::setResourceFile(std::string const& resourceNamespace,
                                        std::string const& name,
                                        fs::path const& selectedFile,
                                        bool continuous) {
  auto relative = portableSelectedFile(selectedFile);
  if (!relative) return false;
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  auto index = findResourceIndex(*collection, name);
  if (!index) {
    setFailure("Resource '" + name + "' was not found.");
    return false;
  }
  auto const type = scalar(collection->resources[*index], "type");
  auto const* form = resourceForm(type);
  if (!form || form->fileProperty != "location") {
    setFailure("Resource Type '" + type +
               "' has no editable file property.");
    return false;
  }
  collection->resources[*index][form->fileProperty] = *relative;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Select Resource file", emitYaml(root),
      "file:" + resourceNamespace + ":" + std::to_string(*index), continuous);
}

bool ManifestWorkspace::setResourceOption(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& optionName, std::optional<std::string> value,
    bool continuous) {
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  auto index = findResourceIndex(*collection, name);
  if (!index) {
    setFailure("Resource '" + name + "' was not found.");
    return false;
  }
  auto resource = collection->resources[*index];
  auto const* form = resourceForm(scalar(resource, "type"));
  ResourceOptionForm const* optionForm = nullptr;
  if (form) {
    auto foundForm = std::find_if(form->options.begin(), form->options.end(),
                                  [&](ResourceOptionForm const& option) {
                                    return option.name == optionName;
                                  });
    if (foundForm != form->options.end()) optionForm = &*foundForm;
  }
  if (!optionForm) {
    setFailure("Option '" + optionName + "' is not defined by this Resource schema.");
    return false;
  }
  if (value && !optionForm->boolean &&
      std::find(optionForm->values.begin(), optionForm->values.end(), *value) ==
          optionForm->values.end()) {
    setFailure("Option value is not permitted by the Resource schema.");
    return false;
  }
  if (value && optionForm->boolean && *value != "true" && *value != "false") {
    setFailure("Boolean Resource option must be true or false.");
    return false;
  }

  auto options = collectionItems(resource["Option"]);
  auto found = std::find_if(options.begin(), options.end(), [&](YAML::Node const& option) {
    return scalar(option, "name") == optionName;
  });
  if (!value) {
    if (found == options.end()) return true;
    eraseCollectionItem(options,
                        static_cast<std::size_t>(found - options.begin()));
  } else if (found == options.end()) {
    YAML::Node option(YAML::NodeType::Map);
    option["name"] = optionName;
    option["value"] = *value;
    options.push_back(std::move(option));
  } else {
    (*found)["value"] = *value;
  }
  setCollection(resource, "Option", options);
  collection->resources[*index] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Edit " + optionName + " option", emitYaml(root),
      "option:" + resourceNamespace + ":" + std::to_string(*index) + ":" +
          optionName,
      continuous);
}

bool ManifestWorkspace::deleteResource(std::string const& resourceNamespace,
                                       std::string const& name) {
  auto root = YAML::Load(canonicalYaml());
  if (!resourceNamespace.empty() &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Resource namespace was not found uniquely; duplicate namespaces "
               "are ambiguous.");
    return false;
  }
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*collection, name) != 1U) {
    setFailure("Resource '" + name +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  auto index = findResourceIndex(*collection, name);
  ResourceIdentity const target{resourceNamespace, name};
  if (hasIncomingReference(
          root, target,
          [&](ResourceIdentity const& source) { return source == target; })) {
    setFailure("Resource '" + name +
               "' cannot be deleted while another Resource references it.");
    return false;
  }

  eraseCollectionItem(collection->resources, *index);
  bool const removesNamespace = collection->named && collection->resources.empty();
  if (removesNamespace) {
    eraseCollectionItem(collection->namespaces, collection->namespaceIndex);
    setCollection(root["Resources"], "Namespace", collection->namespaces);
  } else {
    publishCollection(root, *collection);
  }
  return executeYamlCommand(removesNamespace
                                ? "Delete Resource and namespace"
                                : "Delete Resource",
                            emitYaml(root));
}

bool ManifestWorkspace::canUndo() const noexcept { return mCommands.canUndo(); }
bool ManifestWorkspace::canRedo() const noexcept { return mCommands.canRedo(); }
std::string const* ManifestWorkspace::undoName() const noexcept {
  return mCommands.undoName();
}
std::string const* ManifestWorkspace::redoName() const noexcept {
  return mCommands.redoName();
}
bool ManifestWorkspace::undo() {
  if (!mCommands.undo()) return false;
  mOperationDiagnostic.clear();
  mDraft.reset();
  return true;
}
bool ManifestWorkspace::redo() {
  if (!mCommands.redo()) return false;
  mOperationDiagnostic.clear();
  mDraft.reset();
  return true;
}
void ManifestWorkspace::endContinuousEdit() { mCommands.endCoalescing(); }

bool ManifestWorkspace::applyCommittedYaml(std::string const& yaml) {
  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::parse(yaml, mPath.empty() ? "Untitled Resource Manifest"
                                                          : displayPath(mPath)));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Resource edit was rejected: " + validationMessage(validation));
    return false;
  }
  mDocument = std::move(candidate);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  if (mNamespaceDraft) validateNamespaceDraft();
  return true;
}

bool ManifestWorkspace::executeYamlCommand(std::string name, std::string yaml,
                                           std::string mergeKey,
                                           bool continuous) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  auto candidate = ResourceManifestDocument::parse(yaml, "Resource edit preview");
  auto validation = candidate.validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Resource edit was rejected: " + validationMessage(validation));
    return false;
  }
  auto after = candidate.serializeCanonical();
  auto before = canonicalYaml();
  if (after == before) {
    if (!continuous) mCommands.endCoalescing();
    return true;
  }
  try {
    mCommands.execute(std::make_unique<ReplaceYamlCommand>(
                          std::move(name), std::move(mergeKey),
                          [this](std::string const& value) {
                            return applyCommittedYaml(value);
                          },
                          std::move(before), std::move(after)),
                      continuous);
    if (!continuous) mCommands.endCoalescing();
    return true;
  } catch (std::exception const& error) {
    setFailure(error.what());
    return false;
  }
}

std::optional<std::string> ManifestWorkspace::portableSelectedFile(
    fs::path const& selectedFile) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return {};
  }
  std::error_code error;
  if (!fs::is_regular_file(selectedFile, error)) {
    setFailure("Selected Resource file does not exist as a regular file: " +
               displayPath(selectedFile) +
               (error ? ": " + error.message() : std::string{}));
    return {};
  }
  auto canonicalTarget = fs::canonical(selectedFile, error);
  if (error) {
    setFailure("Could not canonicalize selected Resource file '" +
               displayPath(selectedFile) + "': " + error.message());
    return {};
  }
  if (!containedBy(mBaseDirectory, canonicalTarget)) {
    setFailure("Selected Resource file must be beneath the canonical base directory; "
               "traversal and link escapes are not allowed.");
    return {};
  }
  auto relative = canonicalTarget.lexically_relative(mBaseDirectory);
  if (relative.empty() || relative.is_absolute() ||
      std::any_of(relative.begin(), relative.end(), [](fs::path const& part) {
        return part == "..";
      })) {
    setFailure("Selected Resource file cannot be represented as a safe relative path.");
    return {};
  }
  return displayPath(relative);
}

void ManifestWorkspace::setFailure(std::string message) {
  mOperationDiagnostic = std::move(message);
}

bool runDocumentTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-document-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    fs::create_directories(root / "assets");
    ManifestWorkspace workspace;
    if (!workspace.createNew(root / "assets") || !workspace.hasDocument() ||
        workspace.hasPath() || !workspace.dirty()) {
      return fail("New did not create a valid empty unsaved Resource Manifest.");
    }
    if (workspace.canonicalYaml() != "Resources:\n  {}\n") {
      return fail("New Resource Manifest did not have canonical empty content.");
    }
    auto yamlPath = root / "manifest.YML";
    if (!workspace.saveAs(yamlPath) || !workspace.hasPath() || workspace.dirty()) {
      return fail("Save As did not save a YAML Resource Manifest.");
    }
    if (readBytes(yamlPath) != "Resources:\n  {}\n") {
      return fail("Save As did not emit canonical Resource Manifest YAML.");
    }

    fs::path originalPath = workspace.path();
    auto invalidPath = root / "invalid.yaml";
    std::ofstream(invalidPath) << "Resources:\n  unexpected: true\n";
    if (workspace.open(invalidPath) || workspace.path() != originalPath ||
        workspace.operationDiagnostic().empty()) {
      return fail("Structurally invalid Open replaced the current document.");
    }
    auto multiDocumentPath = root / "multi-document.yaml";
    std::ofstream(multiDocumentPath) << "Resources: {}\n---\nResources: {}\n";
    if (workspace.open(multiDocumentPath) || workspace.path() != originalPath) {
      return fail("Open accepted multiple YAML documents or replaced the current document.");
    }
    if (workspace.saveAs(root / "manifest.txt") ||
        fs::exists(root / "manifest.txt")) {
      return fail("Save As accepted a non-YAML extension.");
    }

    std::ofstream(yamlPath, std::ios::binary | std::ios::trunc)
        << "Resources:\n  Resource:\n    type: TextFile\n    name: notes\n"
           "    location: notes.txt\n";
    if (!workspace.open(yamlPath) || workspace.dirty()) {
      return fail("Open did not accept a structurally valid single-document manifest.");
    }
    auto expected = workspace.canonicalYaml();
    if (!workspace.save() || readBytes(yamlPath) != expected) {
      return fail("Save did not atomically replace the destination with canonical YAML.");
    }
    for (auto const& entry : fs::directory_iterator(root)) {
      if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
        return fail("Atomic Save left a sibling temporary file behind.");
      }
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runAuthoringTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-authoring-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    auto base = root / "base";
    fs::create_directories(base / "nested");
    fs::create_directories(root / "outside");
    std::vector<std::pair<std::string, fs::path>> fixtures{
        {"TextFile", base / "notes.txt"},
        {"XmlFile", base / "layout.xml"},
        {"Shader", base / "effect.vert"},
        {"AudioBank", base / "audio.bank"},
        {"Image", base / "nested" / "image.png"}};
    for (auto const& [type, path] : fixtures) {
      static_cast<void>(type);
      std::ofstream(path, std::ios::binary) << "fixture";
    }
    auto outside = root / "outside" / "escape.txt";
    std::ofstream(outside) << "outside";

    ManifestWorkspace workspace;
    if (!workspace.createNew(base) || workspace.resourceForms().size() != 5U) {
      return fail("The five built-in schema-generated forms were not available.");
    }
    for (auto const& form : workspace.resourceForms()) {
      if (form.fileProperty != "location" || form.fileKind.empty() ||
          form.fileExtensions.empty()) {
        return fail("A built-in file property did not expose selector annotations.");
      }
    }

    auto empty = workspace.canonicalYaml();
    if (!workspace.beginDraft("TextFile") || workspace.commitDraft() ||
        workspace.canonicalYaml() != empty || workspace.canUndo()) {
      return fail("An invalid draft entered the document or command history.");
    }
    workspace.setDraftName("Bad/Name");
    if (workspace.draftValid()) return fail("A slash-containing draft name was accepted.");
    workspace.setDraftName("Notes");
    if (workspace.selectDraftFile(root / "missing.txt") ||
        workspace.selectDraftFile(base / ".." / "outside" / "escape.txt")) {
      return fail("A nonexistent file or traversal escape was accepted.");
    }

    std::error_code linkError;
    fs::create_symlink(outside, base / "linked.txt", linkError);
    if (!linkError && workspace.selectDraftFile(base / "linked.txt")) {
      return fail("A symbolic-link escape was accepted.");
    }
    if (!workspace.selectDraftFile(fixtures.front().second) ||
        !workspace.commitDraft()) {
      return fail("A valid TextFile draft was not committed.");
    }

    for (std::size_t index = 1; index < fixtures.size(); ++index) {
      auto const& [type, path] = fixtures[index];
      if (!workspace.beginDraft(type)) return fail("Could not begin a built-in draft.");
      workspace.setDraftName(type);
      if (!workspace.selectDraftFile(path) || !workspace.commitDraft()) {
        return fail("Could not author built-in Resource Type " + type + ": " +
                    workspace.operationDiagnostic());
      }
    }
    auto summaries = workspace.resources();
    if (summaries.size() != fixtures.size() ||
        std::any_of(summaries.begin(), summaries.end(),
                    [](ResourceSummary const& item) {
                      return !item.explicitName || !item.editable;
                    })) {
      return fail("Authored Resources were not explicit editable tree entries.");
    }
    auto authored = workspace.canonicalYaml();
    for (auto const& [type, path] : fixtures) {
      static_cast<void>(path);
      if (authored.find("\"" + type + "\"") == std::string::npos) {
        return fail("Canonical output omitted built-in Resource Type " + type + ".");
      }
    }
    if (authored.find("nested/image.png") == std::string::npos ||
        authored.find(displayPath(base)) != std::string::npos ||
        authored.find('\\') != std::string::npos) {
      return fail("Selected files were not serialized as portable relative paths.");
    }

    if (!workspace.beginDraft("TextFile")) return fail("Could not begin duplicate draft.");
    workspace.setDraftName("Notes");
    workspace.selectDraftFile(fixtures.front().second);
    if (workspace.commitDraft()) return fail("A duplicate Resource name was accepted.");
    workspace.cancelDraft();

    if (!workspace.renameResource({}, "Notes", "Note", true) ||
        !workspace.renameResource({}, "Note", "Readme", true) ||
        !workspace.undo() || workspace.canonicalYaml() != authored ||
        !workspace.redo() ||
        workspace.canonicalYaml().find("\"Readme\"") == std::string::npos) {
      return fail("Continuous rename did not coalesce into observable undo/redo.");
    }
    workspace.endContinuousEdit();

    auto replacement = base / "replacement.txt";
    std::ofstream(replacement) << "replacement";
    auto beforeFile = workspace.canonicalYaml();
    if (!workspace.setResourceFile({}, "Readme", replacement) ||
        workspace.canonicalYaml() == beforeFile || !workspace.undo() ||
        workspace.canonicalYaml() != beforeFile || !workspace.redo()) {
      return fail("File-property edit did not support undo and redo.");
    }

    if (!workspace.setResourceOption({}, "Image", "filtering", "linear") ||
        workspace.canonicalYaml().find("\"filtering\"") == std::string::npos ||
        !workspace.undo() ||
        workspace.canonicalYaml().find("\"filtering\"") != std::string::npos ||
        !workspace.redo()) {
      return fail("Schema-generated Image option did not support undo and redo.");
    }

    auto beforeDelete = workspace.canonicalYaml();
    if (!workspace.deleteResource({}, "XmlFile") ||
        workspace.resources().size() != 4U || !workspace.undo() ||
        workspace.canonicalYaml() != beforeDelete || !workspace.redo() ||
        workspace.resources().size() != 4U) {
      return fail("Delete did not support command-level undo and redo.");
    }
    if (!workspace.undo()) return fail("Could not restore deleted Resource.");

    auto output = root / "authored.yaml";
    if (!workspace.saveAs(output)) return fail("Could not save authored manifest.");
    auto saved = ResourceManifestDocument::load(output);
    if (!saved.validate(ResourceSchemaCatalog::builtIn().snapshot()).valid() ||
        readBytes(output) != workspace.canonicalYaml()) {
      return fail("Authored canonical output was not externally valid and stable.");
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runOrganizationTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-organization-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    fs::create_directories(root / "base");
    auto sourceFile = root / "base" / "source.txt";
    std::ofstream(sourceFile) << "source";

    ManifestWorkspace drafts;
    if (!drafts.createNew(root / "base") || drafts.namespaces().size() != 1U ||
        !drafts.namespaces().front().isDefault ||
        drafts.renameNamespace({}, "Renamed") || drafts.deleteNamespace({})) {
      return fail("The permanent default namespace was not presented or protected.");
    }
    auto empty = drafts.canonicalYaml();
    if (!drafts.beginNamespaceDraft() || drafts.namespaceDraftValid()) {
      return fail("An empty named namespace draft was accepted.");
    }
    drafts.setNamespaceDraftName("Nested/Invalid");
    if (drafts.namespaceDraftValid()) {
      return fail("A namespace containing the qualifier separator was accepted.");
    }
    drafts.setNamespaceDraftName("Group");
    if (!drafts.namespaceDraftValid() || drafts.canonicalYaml() != empty ||
        drafts.canUndo() || drafts.namespaces().back().name != "Group" ||
        !drafts.namespaces().back().draft) {
      return fail("A named namespace did not remain an uncommitted tree draft.");
    }
    if (!drafts.beginDraft("TextFile", "Group")) {
      return fail("Could not begin the first Resource in a namespace draft.");
    }
    drafts.setDraftName("Alpha");
    if (!drafts.selectDraftFile(sourceFile) || !drafts.commitDraft() ||
        drafts.namespaceDraft() || drafts.namespaces().size() != 2U ||
        !drafts.canUndo()) {
      return fail("The first Resource did not commit its namespace atomically.");
    }
    auto groupYaml = drafts.canonicalYaml();
    if (!drafts.undo() || drafts.canonicalYaml() != empty ||
        drafts.namespaces().size() != 1U || !drafts.redo() ||
        drafts.canonicalYaml() != groupYaml) {
      return fail("Namespace-and-first-Resource undo/redo was not atomic.");
    }

    if (!drafts.beginNamespaceDraft())
      return fail("Could not begin a duplicate namespace draft.");
    drafts.setNamespaceDraftName("Group");
    if (drafts.namespaceDraftValid()) {
      return fail("A duplicate namespace name was accepted.");
    }
    drafts.cancelNamespaceDraft();

    auto addFileResource = [&](std::string const& name) {
      if (!drafts.beginDraft("TextFile")) return false;
      drafts.setDraftName(name);
      return drafts.selectDraftFile(sourceFile) && drafts.commitDraft();
    };
    if (!addFileResource("Alpha") || !addFileResource("Beta")) {
      return fail("Namespace-unique Resource identities were not accepted.");
    }
    if (!drafts.beginDraft("TextFile"))
      return fail("Could not begin a duplicate Resource draft.");
    drafts.setDraftName("Alpha");
    drafts.selectDraftFile(sourceFile);
    if (drafts.commitDraft()) {
      return fail("A duplicate Resource identity was accepted.");
    }
    drafts.cancelDraft();

    if (!drafts.reorderResource({}, "Beta", 0U)) {
      return fail("A valid Resource reorder was rejected.");
    }
    auto ordered = drafts.resources();
    std::vector<std::string> defaultOrder;
    for (auto const& resource : ordered) {
      if (resource.resourceNamespace.empty()) defaultOrder.push_back(resource.name);
    }
    if (defaultOrder != std::vector<std::string>{"Beta", "Alpha"}) {
      return fail("Resource order was not preserved after an explicit reorder: " +
                  drafts.canonicalYaml());
    }
    auto reordered = drafts.canonicalYaml();
    if (!drafts.undo() || drafts.canonicalYaml() == reordered || !drafts.redo() ||
        drafts.canonicalYaml() != reordered) {
      return fail("Resource reorder did not support undo and redo.");
    }
    if (drafts.moveResource("Group", "Alpha", {}) ||
        drafts.renameNamespace("Group", "Nested/Invalid") ||
        drafts.renameResource({}, "Alpha", "Nested/Invalid")) {
      return fail("A duplicate move or qualifier-containing rename was accepted.");
    }

    if (!drafts.beginNamespaceDraft())
      return fail("Could not begin a move-target namespace draft.");
    drafts.setNamespaceDraftName("Moved");
    auto beforeDraftMove = drafts.canonicalYaml();
    if (!drafts.moveResource({}, "Beta", "Moved") || drafts.namespaceDraft() ||
        drafts.canonicalYaml() == beforeDraftMove || !drafts.undo() ||
        drafts.canonicalYaml() != beforeDraftMove || !drafts.redo()) {
      return fail("Moving into a namespace draft was not one undoable command.");
    }

    auto referencesPath = root / "references.yaml";
    std::ofstream(referencesPath) << R"(Resources:
  Resource:
    - type: TextFile
      name: Root
      location: source.txt
    - type: Custom
      name: RootConsumer
      DependentResources:
        DependentResource:
          ref: Shared/Target
  Namespace:
    - name: Shared
      Resource:
        - type: TextFile
          name: Target
          location: source.txt
        - type: Custom
          name: LocalConsumer
          DependentResources:
            DependentResource:
              ref: Target
    - name: Other
      Resource:
        type: Custom
        name: Observer
        DependentResources:
          DependentResource:
            - ref: Shared/Target
            - ref: Shared/LocalConsumer
            - ref: /Root
)";
    ManifestWorkspace references;
    if (!references.open(referencesPath)) {
      return fail("Could not open the reference-rewrite fixture: " +
                  references.operationDiagnostic());
    }
    auto const referenceNamespaces = references.namespaces();
    if (referenceNamespaces.size() != 3U ||
        referenceNamespaces[1].name != "Shared" ||
        referenceNamespaces[2].name != "Other") {
      return fail("Named namespace source order was not preserved in the tree.");
    }
    if (!references.renameResource("Shared", "Target", "Renamed")) {
      return fail("Referenced Resource rename was rejected.");
    }
    auto renamed = references.canonicalYaml();
    if (renamed.find("Shared/Renamed") == std::string::npos ||
        renamed.find("ref: \"Renamed\"") == std::string::npos ||
        references.deleteResource("Shared", "Renamed")) {
      return fail("Resource rename did not rewrite incoming standard references or "
                  "deletion was not blocked.");
    }
    if (!references.moveResource("Shared", "Renamed", "Other")) {
      return fail("Referenced Resource move was rejected: " +
                  references.operationDiagnostic());
    }
    auto moved = references.canonicalYaml();
    if (moved.find("Other/Renamed") == std::string::npos ||
        moved.find("ref: \"Renamed\"") == std::string::npos ||
        moved.find("Shared/Renamed") != std::string::npos) {
      return fail("Move did not use qualified cross-namespace and unqualified "
                  "same-namespace references.");
    }
    if (!references.moveResource({}, "Root", "Other")) {
      return fail("Moving a default-namespace reference target was rejected.");
    }
    auto movedRoot = references.canonicalYaml();
    if (movedRoot.find("ref: \"/Root\"") != std::string::npos ||
        movedRoot.find("ref: \"Root\"") == std::string::npos) {
      return fail("Moving into the reference owner's namespace did not unqualify "
                  "the reference.");
    }
    if (!references.moveResource("Other", "Root", {})) {
      return fail("Moving a reference target back to the default namespace failed.");
    }
    auto defaultRoot = references.canonicalYaml();
    if (defaultRoot.find("ref: \"/Root\"") == std::string::npos ||
        !references.undo() || references.canonicalYaml() != movedRoot ||
        !references.redo() || references.canonicalYaml() != defaultRoot) {
      return fail("A cross-namespace reference to the default namespace was not "
                  "qualified and undoable.");
    }
    auto beforeNamespaceRename = references.canonicalYaml();
    if (!references.renameNamespace("Shared", "Common")) {
      return fail("Referenced namespace rename was rejected.");
    }
    auto renamedNamespace = references.canonicalYaml();
    if (renamedNamespace.find("Common/LocalConsumer") == std::string::npos ||
        renamedNamespace.find("Shared/LocalConsumer") != std::string::npos ||
        !references.undo() ||
        references.canonicalYaml() != beforeNamespaceRename ||
        !references.redo() ||
        references.canonicalYaml() != renamedNamespace) {
      return fail("Namespace rename and reference rewrites were not one undoable "
                  "transaction.");
    }
    if (references.deleteNamespace("Other")) {
      return fail("A populated namespace with external incoming references was deleted.");
    }

    auto deletionPath = root / "deletion.yaml";
    std::ofstream(deletionPath) << R"(Resources:
  Namespace:
    - name: Trash
      Resource:
        - type: TextFile
          name: One
          location: source.txt
        - type: Custom
          name: InternalConsumer
          DependentResources:
            DependentResource:
              ref: One
    - name: Solo
      Resource:
        type: TextFile
        name: Only
        location: source.txt
)";
    ManifestWorkspace deletions;
    if (!deletions.open(deletionPath)) {
      return fail("Could not open the namespace-deletion fixture.");
    }
    auto beforePopulatedDelete = deletions.canonicalYaml();
    if (!deletions.deleteNamespace("Trash") || !deletions.undo() ||
        deletions.canonicalYaml() != beforePopulatedDelete ||
        !deletions.redo()) {
      return fail("Explicit populated namespace deletion was not undoable.");
    }
    if (!deletions.undo()) return fail("Could not restore populated namespace.");
    auto beforeFinalMove = deletions.canonicalYaml();
    if (!deletions.moveResource("Solo", "Only", {}) ||
        !deletions.undo() || deletions.canonicalYaml() != beforeFinalMove ||
        !deletions.redo() || !deletions.undo()) {
      return fail("Moving a final Resource did not remove and restore its namespace "
                  "as one command.");
    }
    auto beforeFinalDelete = deletions.canonicalYaml();
    if (!deletions.deleteResource("Solo", "Only")) {
      return fail("Final-Resource deletion was rejected.");
    }
    auto const afterFinalDeleteNamespaces = deletions.namespaces();
    if (std::any_of(afterFinalDeleteNamespaces.begin(),
                    afterFinalDeleteNamespaces.end(),
                    [](NamespaceSummary const& item) {
                      return item.name == "Solo";
                    }) ||
        !deletions.undo() || deletions.canonicalYaml() != beforeFinalDelete ||
        !deletions.redo()) {
      return fail("Final-Resource deletion did not remove and restore its namespace "
                  "as one command.");
    }

    auto duplicatePath = root / "duplicates.yaml";
    std::ofstream(duplicatePath) << R"(Resources:
  Resource:
    - {type: TextFile, name: Duplicate, location: source.txt}
    - {type: TextFile, name: Duplicate, location: source.txt}
  Namespace:
    - {name: Repeated, Resource: {type: TextFile, name: A, location: source.txt}}
    - {name: Repeated, Resource: {type: TextFile, name: B, location: source.txt}}
)";
    ManifestWorkspace duplicates;
    if (!duplicates.open(duplicatePath) ||
        duplicates.renameResource({}, "Duplicate", "Unique") ||
        duplicates.deleteResource({}, "Duplicate") ||
        duplicates.renameNamespace("Repeated", "Unique") ||
        duplicates.moveResource("Repeated", "A", {})) {
      return fail("Commands did not reject ambiguous duplicate identities.");
    }

    auto nestedPath = root / "nested.yaml";
    std::ofstream(nestedPath) << R"(Resources:
  Namespace:
    name: Outer
    Namespace:
      name: Inner
      Resource: {type: TextFile, name: A, location: source.txt}
    Resource: {type: TextFile, name: B, location: source.txt}
)";
    ManifestWorkspace nested;
    if (nested.open(nestedPath)) {
      return fail("A nested namespace was accepted.");
    }

    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

}  // namespace resource_manager
