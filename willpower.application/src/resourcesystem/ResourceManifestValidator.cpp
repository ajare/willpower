#include "ResourceManifestValidator.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <utility>

namespace wp::application::resourcesystem {
namespace {
ResourceManifestValidator::SchemaKey const manifestSchemaKey{"ResourceManifest", ""};
constexpr std::size_t maximumValidationFailures = 100;

ResourceSchemaCatalogSnapshot builtInCatalog() {
  static auto const catalog = ResourceSchemaCatalog::builtIn().snapshot();
  return catalog;
}

ResourceSchema const* findSchema(ResourceSchemaCatalogSnapshot const& catalog,
                                 ResourceManifestValidator::SchemaKey const& key) {
  if (key == manifestSchemaKey) {
    auto const found = std::find_if(catalog.entries().begin(), catalog.entries().end(),
                                    [](auto const& schema) {
                                      return schema.kind == ResourceSchemaKind::manifest;
                                    });
    return found == catalog.entries().end() ? nullptr : &*found;
  }
  return catalog.find(key);
}

std::vector<std::string> pointerTokens(std::string const& pointer) {
  std::vector<std::string> tokens;
  for (std::size_t begin = pointer.empty() ? std::string::npos : 1;
       begin != std::string::npos && begin <= pointer.size();) {
    auto const end = pointer.find('/', begin);
    tokens.push_back(pointer.substr(begin, end - begin));
    begin = end == std::string::npos ? std::string::npos : end + 1;
  }
  return tokens;
}

bool isArrayIndex(std::string const& token) {
  return !token.empty() && std::all_of(token.begin(), token.end(), [](unsigned char character) {
           return std::isdigit(character) != 0;
         });
}

std::string pointerThrough(std::vector<std::string> const& tokens, std::size_t last) {
  std::string pointer;
  for (std::size_t index = 0; index <= last; ++index) pointer += '/' + tokens[index];
  return pointer;
}

void addResourceIdentity(utils::YamlReader const& reader,
                         ResourceManifestValidator::Failure& failure) {
  auto const tokens = pointerTokens(failure.instancePath);
  if (tokens.size() < 2 || tokens[0] != "Resources") return;

  std::size_t index = 1;
  std::string namespacePointer;
  if (tokens[index] == "Namespace") {
    if (++index < tokens.size() && isArrayIndex(tokens[index])) ++index;
    if (index >= tokens.size() || tokens[index] != "Resource") return;
    namespacePointer = pointerThrough(tokens, index - 1);
  } else if (tokens[index] != "Resource") {
    return;
  }

  if (++index < tokens.size() && isArrayIndex(tokens[index])) ++index;
  auto const resourcePointer = pointerThrough(tokens, index - 1);
  if (!namespacePointer.empty()) {
    if (auto namesp = reader.scalarAtJsonPointer(namespacePointer + "/name")) {
      failure.resourceNamespace = *namesp;
    }
  }
  if (auto name = reader.scalarAtJsonPointer(resourcePointer + "/name")) {
    failure.resourceName = *name;
  } else if (auto location = reader.scalarAtJsonPointer(resourcePointer + "/location")) {
    failure.resourceName = *location;
  }
  if (auto type = reader.scalarAtJsonPointer(resourcePointer + "/type")) {
    failure.resourceType = *type;
  }
}
}  // namespace

ResourceManifestValidator::ResourceManifestValidator() : mCatalog(builtInCatalog()) {}

ResourceManifestValidator::ResourceManifestValidator(ResourceSchemaCatalogSnapshot catalog)
    : mCatalog(std::move(catalog)) {}

bool ResourceManifestValidator::contains(SchemaKey const& key) const {
  return findSchema(mCatalog, key) != nullptr;
}

std::vector<ResourceManifestValidator::Failure> ResourceManifestValidator::validate(
    utils::YamlReader const& reader, std::string const& manifestPath) const {
  return validate(reader, manifestPath, manifestSchemaKey);
}

std::vector<ResourceManifestValidator::Failure> ResourceManifestValidator::validate(
    utils::YamlReader const& reader, std::string const& manifestPath,
    SchemaKey const& key) const {
  auto const* root = findSchema(mCatalog, key);
  if (!root) return {};

  std::vector<utils::JsonSchemaDocument> catalog;
  std::set<std::string> addedIds;
  for (auto const& schema : mCatalog.entries()) {
    if (addedIds.insert(schema.schemaId).second) {
      catalog.push_back({schema.schemaId, schema.contents});
    }
  }

  std::vector<Failure> failures;
  // Bound diagnostics defensively so hostile manifests cannot produce an
  // unbounded exception while still aggregating ordinary authoring errors.
  for (auto const& failure : reader.validateJsonSchema(
           root->contents, catalog, maximumValidationFailures)) {
    Failure result{manifestPath, key.resourceType, key.factoryType, "", "",
                   failure.instancePath, failure.message, failure.line, failure.column};
    addResourceIdentity(reader, result);
    failures.push_back(std::move(result));
  }
  std::stable_sort(failures.begin(), failures.end(), [](auto const& left, auto const& right) {
    if (left.line != right.line) return left.line < right.line;
    if (left.column != right.column) return left.column < right.column;
    if (left.instancePath != right.instancePath) return left.instancePath < right.instancePath;
    return left.message < right.message;
  });
  return failures;
}

}  // namespace wp::application::resourcesystem
