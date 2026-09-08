#include <filesystem>

#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

// A downstream application's schema registration can be linked into this tiny
// executable independently of Resource, rendering, audio, and platform setup.
void registerApplicationSchemas(
    wp::application::resourcesystem::ResourceSchemaCatalog& catalog) {
  catalog.addBundle(std::filesystem::path(WILLPOWER_EXAMPLE_SCHEMA_BUNDLE));
}

int main(int argc, char const* const* argv) {
  auto catalog = wp::application::resourcesystem::ResourceSchemaCatalog::builtIn();
  registerApplicationSchemas(catalog);
  return wp::application::resourcesystem::runResourceSchemaExporter(catalog, argc, argv);
}
