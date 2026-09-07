#pragma once

#include <string>
#include <vector>

#include "utils/YamlReader.h"

namespace wp::application::resourcesystem {

// Internal infrastructure used by the Resource Manifest loader. Keeping this
// declaration under src prevents schema/validator implementation details from
// becoming part of Willpower.Application's exported ABI.
class ResourceManifestValidator {
 public:
  struct SchemaKey {
    std::string resourceType;
    std::string factoryType;
  };

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

  [[nodiscard]] bool contains(SchemaKey const& key) const;
  [[nodiscard]] std::vector<Failure> validate(
      utils::YamlReader const& reader, std::string const& manifestPath) const;
  [[nodiscard]] std::vector<Failure> validate(
      utils::YamlReader const& reader, std::string const& manifestPath,
      SchemaKey const& key) const;
};

}  // namespace wp::application::resourcesystem
