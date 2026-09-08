#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

int main(int argc, char const* const* argv) {
  auto catalog = wp::application::resourcesystem::ResourceSchemaCatalog::builtIn();
  return wp::application::resourcesystem::runResourceSchemaExporter(catalog, argc, argv);
}
