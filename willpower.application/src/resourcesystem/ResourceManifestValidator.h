#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "utils/YamlReader.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace wp::application::resourcesystem {

// Internal infrastructure used by the Resource Manifest loader. Keeping this
// declaration under src prevents schema/validator implementation details from
// becoming part of Willpower.Application's exported ABI.
class ResourceManifestValidator {
 public:
  using SchemaKey = ResourceSchemaKey;

  struct Failure {
    std::string manifestPath;
    std::string resourceType;
    std::string factoryType;
    std::string resourceNamespace;
    std::string resourceName;
    std::string instancePath;
    std::string message;
    int line = 0;
    int column = 0;
  };

  ResourceManifestValidator();
  explicit ResourceManifestValidator(ResourceSchemaCatalogSnapshot catalog);

  [[nodiscard]] bool contains(SchemaKey const& key) const;
  [[nodiscard]] std::vector<Failure> validate(
      utils::YamlReader const& reader, std::string const& manifestPath,
      std::size_t maximumFailures = 100) const;
  [[nodiscard]] std::vector<Failure> validate(
      utils::YamlReader const& reader, std::string const& manifestPath,
      SchemaKey const& key, std::size_t maximumFailures = 100) const;

 private:
  ResourceSchemaCatalogSnapshot mCatalog;
};

}  // namespace wp::application::resourcesystem
