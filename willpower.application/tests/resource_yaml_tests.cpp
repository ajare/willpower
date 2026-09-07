// Exercises Resource Manifest YAML conversion, load-time schema validation, and
// scan/rescan atomicity through the real resource location.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "willpower/common/Logger.h"
#include "willpower/application/resourcesystem/DirectoryResourceLocation.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"

namespace {
namespace fs = std::filesystem;
using wp::application::resourcesystem::DirectoryResourceLocation;
using wp::application::resourcesystem::ResourceManifestValidationException;
using wp::application::resourcesystem::ResourceSystemException;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(fs::path const& path, std::string const& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::trunc);
  stream << contents;
  if (!stream) throw std::runtime_error("Could not write test fixture: " + path.string());
}

std::string expectInvalidScan(DirectoryResourceLocation& location, bool rescan = false) {
  try {
    if (rescan) {
      location.rescan();
    } else {
      location.scan();
    }
  } catch (ResourceManifestValidationException const& error) {
    return error.what();
  }
  throw std::runtime_error("A schema-invalid Resource Manifest was accepted.");
}

void verifyExistingFixture(fs::path const& definition, wp::Logger& logger) {
  DirectoryResourceLocation location(
      &logger, definition.parent_path().string(), definition.filename().string());
  location.scan();

  auto const& namespaces = location.getNamespaceRecords();
  auto const root = namespaces.find("");
  auto const world = namespaces.find("World");
  require(root != namespaces.end(), "The root namespace was not reconstructed.");
  require(world != namespaces.end(), "The 'World' namespace was not reconstructed.");
  require(root->second.resourceRecords.contains("EntityImage"),
          "Image Resource 'EntityImage' was not reconstructed.");

  auto const& image = root->second.resourceRecords.at("EntityImage");
  require(image.baseData.tags.at("filtering") == "none",
          "EntityImage's 'filtering' option did not survive conversion.");
  require(image.baseData.tags.at("uv-style") == "atlas",
          "EntityImage's 'uv-style' option did not survive conversion.");

  require(world->second.resourceRecords.contains("World"),
          "World/World Resource was not reconstructed.");
  auto const& map = world->second.resourceRecords.at("World");
  require(!map.definitions.empty(), "World/World has no definitions.");
  require(map.dependentResources.size() == 1,
          "World/World should have exactly one dependent Resource.");
  require(root->second.resourceRecords.contains("EntityImageSet"),
          "EntityImageSet was not reconstructed from a Resource sequence.");
}

void verifyCommonSchema(fs::path const& root, wp::Logger& logger) {
  struct InvalidCase {
    std::string yaml;
    std::string expectedPath;
  };
  std::vector<InvalidCase> const invalidCases{
      {"Resources:\n  unexpected: true\n", "/Resources"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: ''\n", "/Resources/Resource"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: bad/name\n", "/Resources/Resource"},
      {"Resources:\n  Resource:\n    type: null\n    name: Asset\n", "/Resources/Resource/type"},
      {"Resources:\n  Resource:\n    type: 12\n    name: Asset\n", "/Resources/Resource/type"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: Asset\n    Option:\n      name: ''\n      value: okay\n", "/Resources/Resource/Option"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: Asset\n    Definitions:\n      Definition: null\n", "/Resources/Resource/Definitions/Definition"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: Asset\n    DependentResources:\n      DependentResource:\n        id: Child\n        ref: Other\n        type: Custom\n        name: Inline\n", "/Resources/Resource/DependentResources/DependentResource"},
      {"Resources:\n  Namespace:\n    name: ''\n    Resource:\n      type: Custom\n      name: Asset\n", "/Resources/Namespace"}};

  for (std::size_t index = 0; index < invalidCases.size(); ++index) {
    auto const caseRoot = root / ("invalid-" + std::to_string(index));
    writeFile(caseRoot / "Resources.yaml", invalidCases[index].yaml);
    DirectoryResourceLocation location(&logger, caseRoot.string(), "Resources.yaml");
    auto const message = expectInvalidScan(location);
    require(location.getNamespaceRecords().empty(),
            "A failed initial scan published Resource records.");
    require(message.find("invalid-" + std::to_string(index)) != std::string::npos &&
                message.find("Resources.yaml") != std::string::npos,
            "A validation diagnostic omitted the Resource Manifest path: " + message);
    require(message.find(invalidCases[index].expectedPath) != std::string::npos,
            "A validation diagnostic omitted the expected instance path: " + message);
  }

  auto const customRoot = root / "custom";
  writeFile(customRoot / "Resources.yaml", R"(Resources:
  Resource:
    type: PluginResource
    name: PluginAsset
    Option:
      name: enabled
      value: true
    DependentResources:
      DependentResource:
        - id: Existing
          ref: Somewhere
        - id: Inline
          type: PluginChild
          location: child.asset
          Definitions:
            Definition:
              - factory: PluginFactory
                arbitraryPayload: null
              - arbitraryDefault: [one, two]
    Definitions:
      Definition:
        factory: PluginFactory
        pluginOwned:
          anything: null
)");
  DirectoryResourceLocation custom(&logger, customRoot.string(), "Resources.yaml");
  custom.scan();
  require(custom.getNamespaceRecords().at("").resourceRecords.contains("PluginAsset"),
          "An unknown custom Resource Type did not pass common validation.");

  // Structural loading must not absorb existing semantic filesystem checks.
  auto const missingRoot = root / "missing-file";
  writeFile(missingRoot / "Resources.yaml", R"(Resources:
  Resource:
    type: TextFile
    location: absent.txt
)");
  DirectoryResourceLocation missing(&logger, missingRoot.string(), "Resources.yaml");
  missing.scan();
  try {
    missing.validateResourceDefinitions();
  } catch (ResourceSystemException const&) {
    return;
  }
  throw std::runtime_error("Semantic validation no longer rejects missing source files.");
}

void verifyAtomicRescan(fs::path const& root, wp::Logger& logger) {
  auto const atomicRoot = root / "atomic";
  auto const manifest = atomicRoot / "Resources.yaml";
  writeFile(manifest, R"(Resources:
  Resource:
    type: Custom
    name: Stable
)");
  DirectoryResourceLocation location(&logger, atomicRoot.string(), "Resources.yaml");
  location.scan();
  require(location.getNamespaceRecords().at("").resourceRecords.contains("Stable"),
          "The initial valid scan did not publish its Resource.");

  // The first declaration must not become observable when a later declaration
  // in the same document is structurally invalid.
  writeFile(manifest, R"(Resources:
  Resource:
    - type: Custom
      name: Partial
    - type: Custom
      name: Broken
      unknown: rejected
)");
  auto const message = expectInvalidScan(location, true);
  auto const& retained = location.getNamespaceRecords().at("").resourceRecords;
  require(retained.size() == 1 && retained.contains("Stable"),
          "A failed rescan replaced or partially mutated the last valid records.");
  require(message.find("Broken") != std::string::npos,
          "A Resource-specific diagnostic omitted the Resource identity: " + message);
  require(message.find("/Resources/Resource/1") != std::string::npos,
          "A Resource-specific diagnostic omitted its sequence instance path: " + message);
  require(message.find("line ") != std::string::npos && message.find("column ") != std::string::npos,
          "A validation diagnostic omitted available YAML source coordinates: " + message);

  writeFile(manifest, R"(Resources:
  Resource:
    type: Custom
    name: Corrected
)");
  location.rescan();
  auto const& corrected = location.getNamespaceRecords().at("").resourceRecords;
  require(corrected.size() == 1 && corrected.contains("Corrected"),
          "A corrected Resource Manifest could not be retried after a failed rescan.");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: resource_yaml_tests <Resources.yaml>\n";
    return 2;
  }

  auto const unique = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path const temporaryRoot = fs::temp_directory_path() /
                                 ("willpower-resource-yaml-" + std::to_string(unique));
  fs::create_directories(temporaryRoot);

  try {
    wp::Logger logger;
    verifyExistingFixture(fs::absolute(argv[1]), logger);
    verifyCommonSchema(temporaryRoot, logger);
    verifyAtomicRescan(temporaryRoot, logger);

    fs::remove_all(temporaryRoot);
    std::cout << "Willpower YAML Resource Manifest tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    fs::remove_all(temporaryRoot);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
