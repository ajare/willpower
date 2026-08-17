#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "willpower/common/Logger.h"
#include "willpower/application/resourcesystem/DirectoryResourceLocation.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/application/resourcesystem/ResourceManager.h"

namespace {
namespace fs = std::filesystem;
using wp::application::resourcesystem::DirectoryResourceLocation;
using wp::application::resourcesystem::ResourceDependencyException;
using wp::application::resourcesystem::ResourceLocation;
using wp::application::resourcesystem::ResourceManager;

void writeFile(fs::path const& path, std::string const& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path);
  stream << contents;
  if (!stream) throw std::runtime_error("Could not write test fixture: " + path.string());
}

void expectDependencyError(ResourceManager& manager, std::string const& expectedText) {
  try {
    manager.scanLocations();
  } catch (ResourceDependencyException const& error) {
    if (std::string(error.what()).find(expectedText) == std::string::npos) {
      throw std::runtime_error("Unexpected dependency error: " + std::string(error.what()));
    }
    return;
  }
  throw std::runtime_error("Expected a resource dependency error.");
}

void configureDirectoryFactory(ResourceManager& manager, wp::Logger& logger) {
  manager.addResourceLocationFactory(
      "Directory", [&logger](std::string const& path, std::string const& definition) -> ResourceLocation* {
        return new DirectoryResourceLocation(&logger, path, definition);
      });
}

void testCycle(fs::path const& root, wp::Logger& logger) {
  writeFile(root / "Resources.yaml", R"(Resources:
  Resource:
    - type: "TextFile"
      name: "A"
      DependentResources:
        DependentResource:
          ref: "B"
    - type: "TextFile"
      name: "B"
      DependentResources:
        DependentResource:
          ref: "A"
)");

  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  configureDirectoryFactory(manager, logger);
  manager.addResourceLocation("Directory", root.string(), "Resources.yaml");
  expectDependencyError(manager, "cyclic dependencies");
}

void testMissingNamespace(fs::path const& root, wp::Logger& logger) {
  writeFile(root / "Resources.yaml", R"(Resources:
  Resource:
    type: "TextFile"
    name: "Consumer"
    DependentResources:
      DependentResource:
        ref: "Missing/Provider"
)");

  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  configureDirectoryFactory(manager, logger);
  manager.addResourceLocation("Directory", root.string(), "Resources.yaml");
  expectDependencyError(manager, "namespace 'Missing' could not be found");
}

void testMissingResource(fs::path const& root, wp::Logger& logger) {
  writeFile(root / "existing.txt", "existing");
  writeFile(root / "Resources.yaml", R"(Resources:
  Resource:
    type: "TextFile"
    name: "Consumer"
    DependentResources:
      DependentResource:
        ref: "Shared/Missing"
  Namespace:
    name: "Shared"
    Resource:
      type: "TextFile"
      name: "Existing"
      location: "existing.txt"
)");

  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  configureDirectoryFactory(manager, logger);
  manager.addResourceLocation("Directory", root.string(), "Resources.yaml");
  expectDependencyError(manager, "resource 'Shared/Missing'");
}

void testCrossLocation(fs::path const& root, wp::Logger& logger) {
  auto consumerLocation = root / "consumer";
  auto providerLocation = root / "provider";
  writeFile(consumerLocation / "Resources.yaml", R"(Resources:
  Resource:
    type: "TextFile"
    name: "Consumer"
    DependentResources:
      DependentResource:
        id: "Provider"
        ref: "Shared/Provider"
)");
  writeFile(providerLocation / "provider.txt", "provided");
  writeFile(providerLocation / "Resources.yaml", R"(Resources:
  Namespace:
    name: "Shared"
    Resource:
      type: "TextFile"
      name: "Provider"
      location: "provider.txt"
)");

  ResourceManager manager(nullptr, nullptr, nullptr, &logger);
  configureDirectoryFactory(manager, logger);
  manager.addResourceLocation("Directory", consumerLocation.string(), "Resources.yaml");
  manager.addResourceLocation("Directory", providerLocation.string(), "Resources.yaml");
  manager.scanLocations();

  auto consumer = manager.getQualifiedResource("Consumer");
  auto provider = manager.getQualifiedResource("Shared/Provider");
  if (consumer->getDependentResource("Provider") != provider) {
    throw std::runtime_error("Cross-location dependency did not resolve to the provider.");
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: resource_dependency_tests <scenario>\n";
    return 2;
  }

  auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path root = fs::temp_directory_path() /
                  ("willpower-resource-dependencies-" + std::to_string(unique));
  fs::create_directories(root);

  try {
    wp::Logger logger;
    std::string scenario = argv[1];
    if (scenario == "cycle") {
      testCycle(root, logger);
    } else if (scenario == "missing-namespace") {
      testMissingNamespace(root, logger);
    } else if (scenario == "missing-resource") {
      testMissingResource(root, logger);
    } else if (scenario == "cross-location") {
      testCrossLocation(root, logger);
    } else {
      throw std::runtime_error("Unknown scenario: " + scenario);
    }

    fs::remove_all(root);
    std::cout << "Resource dependency scenario passed: " << scenario << '\n';
    return 0;
  } catch (std::exception const& error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
