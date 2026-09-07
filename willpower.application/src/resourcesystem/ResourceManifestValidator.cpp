#include "ResourceManifestValidator.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "EmbeddedResourceManifestSchemas.h"

namespace wp::application::resourcesystem {
namespace {
using detail::EmbeddedSchemaRecord;

ResourceManifestValidator::SchemaKey const manifestSchemaKey{"ResourceManifest", ""};
constexpr std::size_t maximumValidationFailures = 100;

EmbeddedSchemaRecord const* findSchema(ResourceManifestValidator::SchemaKey const& key) {
  auto const schemas = detail::embeddedResourceManifestSchemas();
  auto found = std::find_if(schemas.begin(), schemas.end(), [&](auto const& schema) {
    return schema.resourceType == key.resourceType && schema.factoryType == key.factoryType;
  });
  // A specialized factory without a schema deliberately falls back to the
  // Resource Type schema. This keeps the key/API ready for later registration.
  if (found == schemas.end() && !key.factoryType.empty()) {
    found = std::find_if(schemas.begin(), schemas.end(), [&](auto const& schema) {
      return schema.resourceType == key.resourceType && schema.factoryType.empty();
    });
  }
  return found == schemas.end() ? nullptr : &*found;
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

bool ResourceManifestValidator::contains(SchemaKey const& key) const {
  return findSchema(key) != nullptr;
}

std::vector<ResourceManifestValidator::Failure> ResourceManifestValidator::validate(
    utils::YamlReader const& reader, std::string const& manifestPath) const {
  return validate(reader, manifestPath, manifestSchemaKey);
}

std::vector<ResourceManifestValidator::Failure> ResourceManifestValidator::validate(
    utils::YamlReader const& reader, std::string const& manifestPath,
    SchemaKey const& key) const {
  auto const* root = findSchema(key);
  if (!root) return {};

  std::vector<utils::JsonSchemaDocument> catalog;
  for (auto const& schema : detail::embeddedResourceManifestSchemas()) {
    catalog.push_back({std::string(schema.id), std::string(schema.source)});
  }

  std::vector<Failure> failures;
  // Bound diagnostics defensively so hostile manifests cannot produce an
  // unbounded exception while still aggregating ordinary authoring errors.
  for (auto const& failure : reader.validateJsonSchema(
           std::string(root->source), catalog, maximumValidationFailures)) {
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
