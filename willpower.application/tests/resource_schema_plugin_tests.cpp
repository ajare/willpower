#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace {
using wp::application::resourcesystem::ResourceSchemaCatalog;
using wp::application::resourcesystem::ResourceSchemaCatalogException;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Operation>
void requireFailure(Operation operation, std::string const& expected) {
  try {
    operation();
  } catch (ResourceSchemaCatalogException const& error) {
    require(std::string(error.what()).find(expected) != std::string::npos,
            "Plugin failure did not contain '" + expected + "': " + error.what());
    return;
  }
  throw std::runtime_error("Expected plugin failure containing '" + expected + "'.");
}
}  // namespace

int main(int argc, char const* const* argv) {
  try {
    require(argc == 6, "Expected sample, incompatible, missing-symbol, malformed, and duplicate plugin paths.");
    auto const sample = std::filesystem::path(argv[1]);
    auto const incompatible = std::filesystem::path(argv[2]);
    auto const missingSymbol = std::filesystem::path(argv[3]);
    auto const malformed = std::filesystem::path(argv[4]);
    auto const duplicate = std::filesystem::path(argv[5]);

    auto catalog = ResourceSchemaCatalog::builtIn();
    require(catalog.snapshot().findExact({"PluginWidget", ""}) == nullptr,
            "A plugin was discovered without an explicitly supplied path.");

    auto copiedPlugin = std::filesystem::temp_directory_path() /
                        ("willpower-sample-schema-plugin" + sample.extension().string());
    std::error_code error;
    std::filesystem::remove(copiedPlugin, error);
    error.clear();
    std::filesystem::copy_file(sample, copiedPlugin,
                               std::filesystem::copy_options::overwrite_existing, error);
    require(!error, "Could not copy the sample plugin for its unload test: " + error.message());

    catalog.addPlugin(copiedPlugin);
    error.clear();
    require(std::filesystem::remove(copiedPlugin, error) && !error,
            "The sample plugin remained loaded after its bytes were copied: " + error.message());
    auto afterSample = catalog.snapshot();
    auto const* defaultSchema = afterSample.findExact({"PluginWidget", ""});
    auto const* specializedSchema = afterSample.findExact({"PluginWidget", "GlowFactory"});
    require(defaultSchema != nullptr && specializedSchema != nullptr,
            "The sample plugin did not register both custom schemas.");
    require(defaultSchema->contents.find("PluginWidget") != std::string::npos &&
                specializedSchema->contents.find("GlowFactory") != std::string::npos,
            "Catalog data did not survive plugin release and unload.");

    auto const stableCatalog = afterSample.exportBundle().catalogJson;
    requireFailure([&] { catalog.addPlugin(incompatible); }, "ABI version");
    require(catalog.snapshot().exportBundle().catalogJson == stableCatalog,
            "An incompatible plugin changed the existing catalog.");
    requireFailure([&] { catalog.addPlugin(missingSymbol); }, "missing required symbol");
    require(catalog.snapshot().exportBundle().catalogJson == stableCatalog,
            "A missing-symbol plugin changed the existing catalog.");
    requireFailure([&] { catalog.addPlugin(malformed); }, "malformed JSON");
    require(catalog.snapshot().exportBundle().catalogJson == stableCatalog,
            "A malformed plugin changed the existing catalog.");
    requireFailure([&] { catalog.addPlugin(duplicate); }, "key collision ('Image', '<default>')");
    require(catalog.snapshot().exportBundle().catalogJson == stableCatalog,
            "A duplicate-schema plugin changed the existing catalog.");

    requireFailure([&] { catalog.addPlugin({}); }, "path cannot be empty");
    std::cout << "Resource Type schema plugin ABI passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
