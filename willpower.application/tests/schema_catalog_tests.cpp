#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "ResourceManifestValidator.h"
#include "utils/YamlReader.h"

namespace {
void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() {
  try {
    using wp::application::resourcesystem::ResourceManifestValidator;
    ResourceManifestValidator validator;
    ResourceManifestValidator::SchemaKey const key{"Infrastructure", ""};
    require(validator.contains(key), "Embedded Resource Type schema was not registered.");
    // A future specialized factory key must retain the Resource Type fallback.
    require(validator.contains({"Infrastructure", "CustomFactory"}),
            "Factory-compatible Resource Type lookup did not fall back.");
    for (auto const* resourceType :
         {"TextFile", "XmlFile", "Shader", "AudioBank", "Image", "ImageSet", "AnimationSet"}) {
      require(validator.contains({resourceType, ""}),
              std::string("Built-in schema was not registered for ") + resourceType + '.');
    }

    auto reader = std::unique_ptr<utils::YamlReader>(utils::YamlReader::fromString(
        "Resources:\n"
        "  - count: 3\n"
        "    enabled: true\n"
        "    tags: [alpha, beta]\n"));
    auto failures = validator.validate(*reader, "memory/Resources.yaml", key);
    if (!failures.empty()) {
      std::string message = "Embedded schema validation failed:";
      for (auto const& failure : failures)
        message += "\n  " + failure.instancePath + ": " + failure.message;
      throw std::runtime_error(message);
    }

    // Validation and conversion use the same parser-owned document. In
    // particular, validation sees Resources as an array and native scalars
    // before readTree() performs its intentionally lossy conversion.
    auto converted = reader->readTree();
    require(converted.getName() == "Resources", "Validated YAML could not be converted afterwards.");

    auto invalid = std::unique_ptr<utils::YamlReader>(utils::YamlReader::fromString(
        "Resources:\n"
        "  count: 3\n"
        "  enabled: true\n"
        "  tags: [alpha, beta]\n"));
    auto first = validator.validate(*invalid, "memory/Resources.yaml", key);
    auto second = validator.validate(*invalid, "memory/Resources.yaml", key);
    require(!first.empty(), "A mapping incorrectly satisfied the embedded array schema.");
    if (!(first.size() == second.size() && first.front().instancePath == second.front().instancePath &&
          first.front().message == second.front().message)) {
      throw std::runtime_error("Validation failures were not deterministic: '" +
                               first.front().instancePath + ": " + first.front().message + "' vs '" +
                               second.front().instancePath + ": " + second.front().message + "'.");
    }
    require(first.front().manifestPath == "memory/Resources.yaml",
            "Manifest identity was not retained in failures.");

    // The only resolver installed by Utils is the supplied in-memory catalog.
    // An absent HTTP URI becomes a schema error and is never fetched.
    auto unresolved = reader->validateJsonSchema(
        R"({"$schema":"http://json-schema.org/draft-07/schema#","$ref":"http://127.0.0.1:1/not-present.schema.json"})");
    require(!unresolved.empty() && unresolved.front().message.find("Schema error:") == 0,
            "An unresolved schema did not fail through the local-only resolver.");

    std::cout << "Embedded Resource Manifest schema catalog passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
