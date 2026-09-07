#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <utils/YamlReader.h>

#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace {
using wp::application::resourcesystem::ResourceSchemaCatalog;
using wp::application::resourcesystem::ResourceSchemaKind;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

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

    std::cout << "Downstream Resource Schema Bundle passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
