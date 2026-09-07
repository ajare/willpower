#include "ResourceManifestValidator.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "EmbeddedResourceManifestSchemas.h"

namespace wp::application::resourcesystem {
namespace {
using detail::EmbeddedSchemaRecord;

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
}  // namespace

bool ResourceManifestValidator::contains(SchemaKey const& key) const {
  return findSchema(key) != nullptr;
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
  for (auto const& failure : reader.validateJsonSchema(std::string(root->source), catalog)) {
    failures.push_back({manifestPath, key.resourceType, key.factoryType,
                        failure.instancePath, failure.message,
                        failure.line, failure.column});
  }
  std::stable_sort(failures.begin(), failures.end(), [](auto const& left, auto const& right) {
    if (left.instancePath != right.instancePath) return left.instancePath < right.instancePath;
    return left.message < right.message;
  });
  return failures;
}

}  // namespace wp::application::resourcesystem
