#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "willpower/common/Logger.h"
#include "willpower/application/resourcesystem/DirectoryResourceLocation.h"
#include "willpower/application/resourcesystem/ResourceDefinitionFactory.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"
#include "willpower/application/resourcesystem/ResourceManager.h"

namespace {
namespace fs = std::filesystem;
using wp::application::resourcesystem::DirectoryResourceLocation;
using wp::application::resourcesystem::Resource;
using wp::application::resourcesystem::ResourceDefinitionFactory;
using wp::application::resourcesystem::ResourceFactory;
using wp::application::resourcesystem::ResourceLocation;
using wp::application::resourcesystem::ResourceManager;
using wp::application::resourcesystem::ResourceSystemException;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(fs::path const& path, std::string const& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path);
  stream << contents;
  if (!stream) throw std::runtime_error("Could not write test fixture: " + path.string());
}

class TestResourceFactory final : public ResourceFactory {
  size_t mCreatedResourceCount{0};

public:
  TestResourceFactory() : ResourceFactory("TestResource") {
  }

  Resource* createResource(std::string const& name, std::string const& namesp, std::string const& source,
                           std::map<std::string, std::string> const& tags, ResourceLocation* location) override {
    ++mCreatedResourceCount;
    return new Resource(name, namesp, "TestResource", source, tags, location);
  }

  size_t getCreatedResourceCount() const {
    return mCreatedResourceCount;
  }
};

class TrackingDefinitionFactory final : public ResourceDefinitionFactory {
  std::vector<std::string> mResourceNames;

public:
  explicit TrackingDefinitionFactory(std::string const& factoryType)
      : ResourceDefinitionFactory("TestResource", factoryType) {
  }

  void create(Resource* resource, ResourceManager*, wp::DataNode*) override {
    mResourceNames.push_back(resource->getName());
  }

  bool wasUsedFor(std::string const& resourceName) const {
    for (auto const& name : mResourceNames) {
      if (name == resourceName) return true;
    }
    return false;
  }
};

void configureDirectoryFactory(ResourceManager& manager, wp::Logger& logger) {
  manager.addResourceLocationFactory(
      "Directory", [&logger](std::string const& path, std::string const& definition) -> ResourceLocation* {
        return new DirectoryResourceLocation(&logger, path, definition);
      });
}

void testSpecializedDefinitionPrecedesDefault(fs::path const& root, wp::Logger& logger) {
  writeFile(root / "Resources.yaml", R"(Resources:
  Resource:
    - type: "TestResource"
      name: "Asset"
      Definitions:
        Definition:
          - value: "default"
          - factory: "Specialized"
            value: "specialized"
    - type: "TestResource"
      name: "Fallback"
      Definitions:
        Definition:
          value: "default"
)");

  DirectoryResourceLocation location(&logger, root.string(), "Resources.yaml");
  location.scan();
  location.validateResourceDefinitions();
  auto const& definitions = location.getNamespaceRecords().at("").resourceRecords.at("Asset").definitions;
  require(definitions.size() == 2, "The resource did not retain both definitions.");
  require(definitions[0].first == "Specialized" && definitions[1].first.empty(),
          "Validation did not persist specialized-before-default definition order.");

  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  configureDirectoryFactory(manager, logger);
  manager.addResourceFactory(new TestResourceFactory);
  auto* defaultFactory = new TrackingDefinitionFactory("");
  auto* specializedFactory = new TrackingDefinitionFactory("Specialized");
  manager.addResourceDefinitionFactory(defaultFactory);
  manager.addResourceDefinitionFactory(specializedFactory);
  manager.addResourceLocation("Directory", root.string(), "Resources.yaml");
  manager.scanLocations();
  manager.createResource(manager.getResource("Asset"));
  manager.createResource(manager.getResource("Fallback"));

  require(specializedFactory->wasUsedFor("Asset"),
          "The default definition masked the specialized definition while loading.");
  require(!defaultFactory->wasUsedFor("Asset"),
          "The default definition was used before the specialized definition.");
  require(defaultFactory->wasUsedFor("Fallback"),
          "The default definition was not used as the fallback while loading.");
}

void testIdempotentLocationScan(fs::path const& root, wp::Logger& logger) {
  writeFile(root / "Resources.yaml", R"(Resources:
  Resource:
    type: "TestResource"
    name: "Asset"
)");

  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  configureDirectoryFactory(manager, logger);
  auto* factory = new TestResourceFactory;
  manager.addResourceFactory(factory);

  std::vector<wp::application::resourcesystem::ResourceLocationState> callbacks;
  auto scan = [&manager, &callbacks] {
    manager.scanLocations([&callbacks](std::string const&,
                                       wp::application::resourcesystem::ResourceLocationState state) {
      callbacks.push_back(state);
    });
  };

  manager.addResourceLocation("Directory", root.string(), "Resources.yaml");
  scan();
  auto resource = manager.getResource("Asset");
  scan();

  require(manager.getAllResources().size() == 1,
          "Scanning a location twice produced more than one resource.");
  require(manager.getResource("Asset") == resource,
          "Scanning a location twice replaced the existing resource.");
  require(factory->getCreatedResourceCount() == 1,
          "Scanning a location twice instantiated the resource more than once.");
  require(callbacks.size() == 2 &&
              callbacks[0] == wp::application::resourcesystem::ResourceLocationState::Unscanned &&
              callbacks[1] == wp::application::resourcesystem::ResourceLocationState::Scanned,
          "Scanning a location twice invoked location callbacks more than once.");
}

void testDuplicateDefaultsAreRejected(fs::path const& root, wp::Logger& logger) {
  writeFile(root / "Resources.yaml", R"(Resources:
  Resource:
    type: "TestResource"
    name: "Asset"
    Definitions:
      Definition:
        - value: "first default"
        - value: "second default"
)");

  DirectoryResourceLocation location(&logger, root.string(), "Resources.yaml");
  location.scan();
  try {
    location.validateResourceDefinitions();
  } catch (ResourceSystemException const&) {
    return;
  }
  throw std::runtime_error("Multiple default definitions were accepted.");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: resource_definition_tests <specialized-before-default|idempotent-scan|duplicate-default>\n";
    return 2;
  }

  auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path root = fs::temp_directory_path() /
                  ("willpower-resource-definitions-" + std::to_string(unique));
  fs::create_directories(root);

  try {
    wp::Logger logger;
    std::string scenario = argv[1];
    if (scenario == "specialized-before-default") {
      testSpecializedDefinitionPrecedesDefault(root, logger);
    } else if (scenario == "idempotent-scan") {
      testIdempotentLocationScan(root, logger);
    } else if (scenario == "duplicate-default") {
      testDuplicateDefaultsAreRejected(root, logger);
    } else {
      throw std::runtime_error("Unknown scenario: " + scenario);
    }

    fs::remove_all(root);
    std::cout << "Resource definition scenario passed: " << scenario << '\n';
    return 0;
  } catch (std::exception const& error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
