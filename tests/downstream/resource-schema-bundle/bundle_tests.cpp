#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <utils/YamlReader.h>

#include "WidgetResource.h"
#include "willpower/common/Logger.h"
#include "willpower/application/resourcesystem/DirectoryResourceLocation.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"
#include "willpower/application/resourcesystem/ResourceManager.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace {
using wp::application::resourcesystem::DirectoryResourceLocation;
using wp::application::resourcesystem::Resource;
using wp::application::resourcesystem::ResourceFactory;
using wp::application::resourcesystem::ResourceLocation;
using wp::application::resourcesystem::ResourceManager;
using wp::application::resourcesystem::ResourceManifestValidationException;
using wp::application::resourcesystem::ResourceSchemaCatalog;
using wp::application::resourcesystem::ResourceSchemaKind;
using wp::application::resourcesystem::ResourceSystemException;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(std::filesystem::path const& path, std::string const& contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::trunc);
  stream << contents;
  if (!stream) throw std::runtime_error("Could not write test fixture: " + path.string());
}

class WidgetFactory final : public ResourceFactory {
 public:
  WidgetFactory() : ResourceFactory("Widget") {}

  Resource* createResource(std::string const& name, std::string const& namesp,
                           std::string const&, std::map<std::string, std::string> const& tags,
                           ResourceLocation* location) override {
    return new downstream_fixture::WidgetResource(name, namesp, tags, location);
  }
};

bool validates(ResourceSchemaCatalog const& catalog, std::string const& manifest) {
  auto snapshot = catalog.snapshot();
  auto const manifestSchema = std::find_if(
      snapshot.entries().begin(), snapshot.entries().end(),
      [](auto const& schema) { return schema.kind == ResourceSchemaKind::manifest; });
  require(manifestSchema != snapshot.entries().end(), "Composed catalog has no root schema.");

  std::vector<utils::JsonSchemaDocument> schemas;
  for (auto const& schema : snapshot.entries()) {
    schemas.push_back({schema.schemaId, schema.contents});
  }
  std::unique_ptr<utils::YamlReader> reader(utils::YamlReader::fromString(manifest));
  return reader->validateJsonSchema(manifestSchema->contents, schemas).empty();
}

void verifyRuntimeValidation(std::filesystem::path const& bundleDirectory) {
  auto const unique = std::chrono::steady_clock::now().time_since_epoch().count();
  auto const root = std::filesystem::temp_directory_path() /
                    ("willpower-runtime-schema-" + std::to_string(unique));
  auto const manifest = root / "Resources.yaml";
  std::filesystem::create_directories(root);

  try {
    wp::Logger logger;
    ResourceManager manager(nullptr, nullptr, nullptr, &logger);
    manager.addResourceSchemaBundle(ResourceSchemaCatalog::readBundle(bundleDirectory));
    manager.addResourceSchemaBundle(bundleDirectory);  // equivalent duplicate is idempotent
    manager.addResourceFactory(new WidgetFactory);
    manager.addResourceLocationFactory(
        "Directory", [&logger](std::string const& path,
                                std::string const& definition) -> ResourceLocation* {
          return new DirectoryResourceLocation(&logger, path, definition);
        });
    manager.addResourceLocation("Directory", root.string(), "Resources.yaml");

    writeFile(manifest, R"(Resources:
  Resource:
    type: Widget
    name: InvalidDefault
    Definitions:
      Definition:
        size: 0
  Namespace:
    name: Ui
    Resource:
      type: Widget
      name: InvalidGlow
      Definitions:
        Definition:
          factory: GlowFactory
)");
    std::string initialFailure;
    try {
      manager.scanLocations();
    } catch (ResourceManifestValidationException const& error) {
      initialFailure = error.what();
    }
    require(!initialFailure.empty() &&
                initialFailure.find("InvalidDefault") != std::string::npos &&
                initialFailure.find("InvalidGlow") != std::string::npos,
            "Runtime validation did not aggregate default and specialized custom failures: " +
                initialFailure);
    require(manager.getAllResources().empty(),
            "An invalid custom declaration published a ResourceRecord.");

    bool lateRegistrationRejected = false;
    try {
      manager.addResourceSchemaBundle(bundleDirectory);
    } catch (ResourceSystemException const&) {
      lateRegistrationRejected = true;
    }
    require(lateRegistrationRejected,
            "Schema registration was accepted after Resource Location scanning started.");

    writeFile(manifest, R"(Resources:
  Resource:
    - type: Widget
      name: DefaultWidget
      Definitions:
        Definition:
          size: 4
    - type: Widget
      name: FutureFactoryWidget
      Definitions:
        Definition:
          factory: FutureFactory
          applicationPayload: retained
  Namespace:
    name: Ui
    Resource:
      type: Widget
      name: GlowWidget
      Definitions:
        Definition:
          factory: GlowFactory
          intensity: 1.5
)");
    manager.scanLocations();
    auto defaultWidget = manager.getResource("DefaultWidget");
    auto glowWidget = manager.getResource("GlowWidget", "Ui");
    require(defaultWidget->getType() == "Widget" && glowWidget->getType() == "Widget" &&
                manager.getResource("FutureFactoryWidget")->getType() == "Widget",
            "Valid top-level, namespaced, or unknown-factory custom Resources were not published.");

    writeFile(manifest, R"(Resources:
  Resource:
    type: Widget
    name: BrokenRescan
    Definitions:
      Definition:
        size: 0
)");
    bool rescanRejected = false;
    try {
      manager.rescanLocations();
    } catch (ResourceManifestValidationException const&) {
      rescanRejected = true;
    }
    require(rescanRejected, "A structurally invalid custom-schema rescan was accepted.");
    require(manager.getResource("DefaultWidget") == defaultWidget &&
                manager.getResource("GlowWidget", "Ui") == glowWidget &&
                manager.getAllResources().size() == 3U,
            "A failed custom-schema rescan replaced the last successful Resources.");
  } catch (...) {
    std::filesystem::remove_all(root);
    throw;
  }
  std::filesystem::remove_all(root);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected the composed bundle directory.");
    ResourceSchemaCatalog catalog{std::filesystem::path(argv[1])};
    auto snapshot = catalog.snapshot();
    require(snapshot.entries().size() == 14U,
            "Composed bundle did not contain the built-in and custom schemas.");
    require(snapshot.findExact({"Widget", ""}) != nullptr,
            "Custom default schema is missing.");
    require(snapshot.findExact({"Widget", "GlowFactory"}) != nullptr,
            "Custom specialized schema is missing.");

    require(validates(catalog, R"(Resources:
  Resource:
    type: Widget
    name: panel
    Definitions:
      Definition:
        size: 4
)"), "A valid custom default Definition was rejected.");
    require(!validates(catalog, R"(Resources:
  Resource:
    type: Widget
    name: panel
    Definitions:
      Definition:
        size: 0
)"), "An invalid registered custom type passed the unknown-type fallback.");
    require(validates(catalog, R"(Resources:
  Resource:
    type: Widget
    name: panel
    Definitions:
      Definition:
        factory: GlowFactory
        intensity: 1.5
)"), "A valid specialized Definition was rejected.");
    require(!validates(catalog, R"(Resources:
  Resource:
    type: Widget
    name: panel
    Definitions:
      Definition:
        factory: GlowFactory
)"), "A matching specialized Definition bypassed its schema.");
    require(validates(catalog, R"(Resources:
  Resource:
    type: Widget
    name: panel
    Definitions:
      Definition:
        factory: FutureFactory
        applicationPayload: retained
)"), "An unregistered specialized factory did not retain generic fallback.");
    require(validates(catalog, R"(Resources:
  Resource:
    type: FutureResource
    name: future
    Definitions:
      Definition:
        value: retained
)"), "An unknown Resource Type did not retain common fallback.");
    require(validates(catalog, R"(Resources:
  Resource:
    type: TextFile
    location: readme.txt
)"), "A built-in Resource Type was lost from the composed root.");

    verifyRuntimeValidation(std::filesystem::path(argv[1]));

    std::cout << "Downstream Resource Schema Bundle passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
