#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace {
using wp::application::resourcesystem::ResourceSchemaBundle;
using wp::application::resourcesystem::ResourceSchemaBundleDocument;
using wp::application::resourcesystem::ResourceSchemaCatalog;
using wp::application::resourcesystem::ResourceSchemaCatalogException;
using wp::application::resourcesystem::ResourceSchemaKind;

constexpr char widgetSchema[] =
    R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://example.test/widget.schema.json","type":"object"})";
constexpr char specialSchema[] =
    R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://example.test/widget-special.schema.json","type":"object","$ref":"https://example.test/widget.schema.json"})";
constexpr char gadgetSchema[] =
    R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://example.test/gadget.schema.json","type":"string"})";
constexpr char conflictingKeySchema[] =
    R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://example.test/widget-conflict.schema.json","type":"string"})";
constexpr char conflictingIdSchema[] =
    R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://example.test/widget.schema.json","type":"string"})";
constexpr char unresolvedSchema[] =
    R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://example.test/broken.schema.json","$ref":"https://example.test/missing.schema.json"})";

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string entry(std::string const& resourceType, std::string const& factoryType,
                  std::string const& schemaId, std::string const& document,
                  std::string const& hash) {
  return "    {\n"
         "      \"kind\": \"resourceType\",\n"
         "      \"resourceType\": \"" +
         resourceType + "\",\n"
                        "      \"factoryType\": " +
         (factoryType.empty() ? std::string("null") : "\"" + factoryType + "\"") +
         ",\n"
         "      \"schemaId\": \"" +
         schemaId + "\",\n"
                    "      \"document\": \"" +
         document + "\",\n"
                    "      \"documentHash\": \"sha256:" +
         hash + "\"\n"
                "    }";
}

ResourceSchemaBundle bundle(std::vector<std::string> entries,
                            std::vector<ResourceSchemaBundleDocument> documents,
                            std::string const& bundleVersion = "1.0",
                            std::string const& manifestVersion = "1.0") {
  ResourceSchemaBundle result;
  result.catalogJson = "{\n  \"bundleFormatVersion\": \"" + bundleVersion +
                       "\",\n  \"resourceManifestSchemaVersion\": \"" + manifestVersion +
                       "\",\n  \"schemas\": [\n";
  for (std::size_t index = 0; index < entries.size(); ++index) {
    result.catalogJson += entries[index] + (index + 1U == entries.size() ? "\n" : ",\n");
  }
  result.catalogJson += "  ]\n}\n";
  result.documents = std::move(documents);
  return result;
}

ResourceSchemaBundle widgetBundle() {
  return bundle({entry("Widget", "", "https://example.test/widget.schema.json",
                       "schemas/widget.schema.json",
                       "b9587adebb058d2a488e76f48b50949fa5926a5f5d821d109dff308b7bdc3dc6")},
                {{"schemas/widget.schema.json", widgetSchema}});
}

ResourceSchemaBundle specialBundle() {
  return bundle({entry("Widget", "SpecialFactory",
                       "https://example.test/widget-special.schema.json",
                       "schemas/widget-special.schema.json",
                       "bccf1670ed495fe44d98ea5d3f0580a47ebddaeaf6243a52a1197ebcd32e14cc")},
                {{"schemas/widget-special.schema.json", specialSchema}});
}

ResourceSchemaBundle gadgetBundle() {
  return bundle({entry("Gadget", "", "https://example.test/gadget.schema.json",
                       "schemas/widget.schema.json",
                       "6c76b457a6857b1d25badaf0b632bb62dfd3b6643affecd6c8fde4dbd0592f4f")},
                {{"schemas/widget.schema.json", gadgetSchema}});
}

template <typename Operation>
void requireFailure(Operation operation, std::string const& diagnostic) {
  try {
    operation();
  } catch (ResourceSchemaCatalogException const& error) {
    require(std::string(error.what()).find(diagnostic) != std::string::npos,
            "Catalog failure did not contain actionable text '" + diagnostic + "': " + error.what());
    return;
  }
  throw std::runtime_error("Expected Resource Schema Catalog failure containing '" + diagnostic + "'.");
}
}  // namespace

int main() {
  try {
    auto builtIn = ResourceSchemaCatalog::builtIn();
    auto builtInSnapshot = builtIn.snapshot();
    require(builtInSnapshot.entries().size() == 12U,
            "The public built-in catalog did not enumerate twelve schemas.");
    require(std::count_if(builtInSnapshot.entries().begin(), builtInSnapshot.entries().end(),
                          [](auto const& schema) {
                            return schema.kind == ResourceSchemaKind::resourceType;
                          }) == 9,
            "The public built-in catalog did not enumerate nine Resource Types.");
    require(builtInSnapshot.findExact({"Image", ""}) != nullptr,
            "Built-in exact lookup failed.");
    require(builtInSnapshot.find({"Image", "ApplicationFactory"}) ==
                builtInSnapshot.findExact({"Image", ""}),
            "Default-factory fallback failed.");
    require(builtInSnapshot.findExact({"Image", "ApplicationFactory"}) == nullptr,
            "Exact lookup incorrectly applied fallback.");

    auto firstExport = builtInSnapshot.exportBundle();
    auto secondExport = builtInSnapshot.exportBundle();
    require(firstExport.catalogJson == secondExport.catalogJson &&
                firstExport.documents.size() == secondExport.documents.size(),
            "Built-in bundle export was not deterministic.");
    auto firstComposed = builtInSnapshot.exportComposedBundle(
        "https://example.test/composed-resource-manifest.schema.json");
    auto secondComposed = builtInSnapshot.exportComposedBundle(
        "https://example.test/composed-resource-manifest.schema.json");
    require(firstComposed.catalogJson == secondComposed.catalogJson &&
                firstComposed.documents.size() == secondComposed.documents.size() &&
                std::equal(firstComposed.documents.begin(), firstComposed.documents.end(),
                           secondComposed.documents.begin(), [](auto const& left, auto const& right) {
                             return left.document == right.document &&
                                    left.contents == right.contents;
                           }),
            "Composed bundle export was not byte-for-byte deterministic.");
    ResourceSchemaCatalog composed(firstComposed);
    auto composedSnapshot = composed.snapshot();
    require(std::count_if(composedSnapshot.entries().begin(),
                          composedSnapshot.entries().end(), [](auto const& schema) {
                            return schema.kind == ResourceSchemaKind::manifest;
                          }) == 1,
            "Composed export did not contain exactly one root schema.");

    ResourceSchemaCatalog catalog;
    auto callerOwned = widgetBundle();
    catalog.addBundle(callerOwned);
    auto beforeMutation = catalog.snapshot();
    callerOwned.documents.front().contents.assign("destroyed");
    callerOwned.catalogJson.assign("destroyed");
    auto const* widget = beforeMutation.find({"Widget", "AnyFactory"});
    require(widget != nullptr && widget->contents == widgetSchema,
            "Snapshot retained caller-owned bundle buffers.");

    catalog.addBundle(widgetBundle());  // equivalent key and content is idempotent
    require(catalog.snapshot().entries().size() == 1U,
            "Equivalent duplicate registration created another entry.");
    catalog.addBundle(specialBundle());
    auto withSpecial = catalog.snapshot();
    require(withSpecial.findExact({"Widget", "SpecialFactory"}) != nullptr,
            "Specialized exact lookup failed.");
    auto const* fallback = withSpecial.find({"Widget", "UnknownFactory"});
    require(fallback != nullptr && fallback->schemaId == widget->schemaId,
            "Unknown factory did not fall back to the default schema.");
    require(beforeMutation.entries().size() == 1U,
            "A published snapshot changed after later catalog mutation.");

    ResourceSchemaCatalog forward;
    auto widgetInput = widgetBundle();
    auto gadgetInput = gadgetBundle();
    std::array<ResourceSchemaBundle, 2> forwardInputs{widgetInput, gadgetInput};
    forward.addBundles(forwardInputs);
    ResourceSchemaCatalog reverse;
    std::array<ResourceSchemaBundle, 2> reverseInputs{gadgetInput, widgetInput};
    reverse.addBundles(reverseInputs);
    require(forward.snapshot().exportBundle().catalogJson ==
                reverse.snapshot().exportBundle().catalogJson,
            "Merge order changed deterministic catalog export.");
    auto collisionExport = forward.snapshot().exportBundle();
    require(collisionExport.documents.size() == 2U &&
                collisionExport.documents[0].document != collisionExport.documents[1].document,
            "Conflicting input document paths were not deterministically disambiguated.");

    auto keyConflict = bundle(
        {entry("Widget", "", "https://example.test/widget-conflict.schema.json",
               "schemas/conflict.schema.json",
               "7e7653719fc5066a8d4ada093065d0c079d1e35f9073e7b7183a272142b144f7")},
        {{"schemas/conflict.schema.json", conflictingKeySchema}});
    requireFailure([&] { catalog.addBundle(keyConflict); }, "key collision ('Widget', '<default>')");

    auto idConflict = bundle(
        {entry("Other", "", "https://example.test/widget.schema.json",
               "schemas/id-conflict.schema.json",
               "732d6763a2e0ce36fcc3a031154f4f9d1379d292bb61a7ad0a4f157348ab4daa")},
        {{"schemas/id-conflict.schema.json", conflictingIdSchema}});
    requireFailure([&] { catalog.addBundle(idConflict); },
                   "schema ID collision for 'https://example.test/widget.schema.json'");

    auto badVersion = widgetBundle();
    badVersion.catalogJson.replace(badVersion.catalogJson.find("1.0"), 3U, "2.0");
    requireFailure([&] { ResourceSchemaCatalog invalid(badVersion); },
                   "unsupported bundleFormatVersion '2.0'");
    auto badManifestVersion = bundle(
        {entry("Widget", "", "https://example.test/widget.schema.json",
               "schemas/widget.schema.json",
               "b9587adebb058d2a488e76f48b50949fa5926a5f5d821d109dff308b7bdc3dc6")},
        {{"schemas/widget.schema.json", widgetSchema}}, "1.0", "2.0");
    requireFailure([&] { ResourceSchemaCatalog invalid(badManifestVersion); },
                   "unsupported resourceManifestSchemaVersion '2.0'");
    auto badHash = widgetBundle();
    auto const hashPosition = badHash.catalogJson.find("b9587");
    badHash.catalogJson[hashPosition] = '0';
    requireFailure([&] { ResourceSchemaCatalog invalid(badHash); }, "hash mismatch");
    auto missingDocument = widgetBundle();
    missingDocument.documents.clear();
    requireFailure([&] { ResourceSchemaCatalog invalid(missingDocument); },
                   "missing schema document");
    auto unresolved = bundle(
        {entry("Broken", "", "https://example.test/broken.schema.json",
               "schemas/broken.schema.json",
               "6d945ab13d9fdc25d1e4c4f5b4206edb5fc9c68561e99789361f61d508c5d9d7")},
        {{"schemas/broken.schema.json", unresolvedSchema}});
    requireFailure([&] { ResourceSchemaCatalog invalid(unresolved); }, "unresolved local $ref");

    auto exportDirectory = std::filesystem::temp_directory_path() /
                           "willpower-resource-schema-catalog-test";
    builtInSnapshot.exportBundle(exportDirectory);
    ResourceSchemaCatalog roundTrip(exportDirectory);
    require(roundTrip.snapshot().exportBundle().catalogJson == firstExport.catalogJson,
            "Filesystem bundle export did not round trip.");
    std::filesystem::remove_all(exportDirectory);

    std::cout << "Public Resource Schema Catalog passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
