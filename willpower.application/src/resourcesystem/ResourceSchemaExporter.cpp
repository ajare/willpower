#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace wp::application::resourcesystem {
namespace {
using Json = nlohmann::json;

std::string kindName(ResourceSchemaKind kind) {
  switch (kind) {
    case ResourceSchemaKind::manifest:
      return "manifest";
    case ResourceSchemaKind::resourceType:
      return "resourceType";
    case ResourceSchemaKind::dependency:
      return "dependency";
  }
  return "dependency";
}

struct Arguments {
  std::string command;
  std::vector<std::filesystem::path> bundles;
  std::filesystem::path output;
  std::filesystem::path rootSchema;
  std::string rootSchemaId =
      "https://schemas.willpower.dev/resource-manifest/composed-resource-manifest.schema.json";
};

Arguments parseArguments(int argc, char const* const* argv) {
  if (argc < 2) {
    throw std::invalid_argument(
        "usage: willpower-resource-schemas <list|verify|merge|export> "
        "[--bundle DIR ...] [--output DIR] [--root-schema FILE] "
        "[--root-schema-id URI]");
  }
  Arguments result;
  result.command = argv[1];
  if (result.command != "list" && result.command != "verify" &&
      result.command != "merge" && result.command != "export") {
    throw std::invalid_argument("unknown command '" + result.command + "'");
  }
  for (int index = 2; index < argc; ++index) {
    std::string_view option = argv[index];
    auto value = [&](std::string_view name) -> std::string {
      if (++index >= argc) throw std::invalid_argument(std::string(name) + " requires a value");
      return argv[index];
    };
    if (option == "--bundle")
      result.bundles.emplace_back(value(option));
    else if (option == "--output")
      result.output = value(option);
    else if (option == "--root-schema")
      result.rootSchema = value(option);
    else if (option == "--root-schema-id")
      result.rootSchemaId = value(option);
    else if (option == "--help" || option == "-h")
      throw std::invalid_argument(
          "usage: willpower-resource-schemas <list|verify|merge|export> "
          "[--bundle DIR ...] [--output DIR] [--root-schema FILE] "
          "[--root-schema-id URI]");
    else
      throw std::invalid_argument("unknown option '" + std::string(option) + "'");
  }
  if ((result.command == "merge" || result.command == "export") && result.output.empty())
    throw std::invalid_argument("--output is required for " + result.command);
  if (result.command != "export" && !result.rootSchema.empty())
    throw std::invalid_argument("--root-schema is valid only for export");
  return result;
}

int failureCode(std::string const& message) {
  if (message.find("unsupported") != std::string::npos &&
      message.find("Version") != std::string::npos)
    return 3;
  if (message.find("collision") != std::string::npos ||
      message.find("duplicate Resource Schema key") != std::string::npos)
    return 4;
  if (message.find("hash") != std::string::npos ||
      message.find("documentHash") != std::string::npos)
    return 5;
  if (message.find("unresolved") != std::string::npos) return 6;
  return 7;
}

std::optional<std::string> quotedAfter(std::string const& message, std::string const& marker) {
  auto begin = message.find(marker);
  if (begin == std::string::npos) return std::nullopt;
  begin += marker.size();
  auto const end = message.find('\'', begin);
  if (end == std::string::npos) return std::nullopt;
  return message.substr(begin, end - begin);
}

void printFailure(std::string const& message, std::optional<std::filesystem::path> const& bundle,
                  int code) {
  Json diagnostic{{"ok", false}, {"code", code}, {"message", message}};
  diagnostic["bundle"] = bundle ? Json(bundle->generic_string()) : Json(nullptr);
  if (auto collisionId = quotedAfter(message, "schema ID collision for '"); collisionId)
    diagnostic["schemaId"] = *collisionId;
  else if (auto incomingId = quotedAfter(message, "incoming schema ID '"); incomingId)
    diagnostic["schemaId"] = *incomingId;
  else if (auto referenceId = quotedAfter(message, "Schema '"); referenceId)
    diagnostic["schemaId"] = *referenceId;
  else
    diagnostic["schemaId"] = nullptr;
  auto key = message.find("key collision (");
  std::size_t keyOffset = 14U;
  if (key == std::string::npos) {
    key = message.find("duplicate Resource Schema key (");
    keyOffset = 30U;
  }
  if (key != std::string::npos) {
    auto const end = message.find(')', key);
    diagnostic["key"] = message.substr(key + keyOffset, end - key - keyOffset + 1U);
  } else {
    diagnostic["key"] = nullptr;
  }

  // Validation errors identify their catalog entry numerically. Recover its
  // lookup metadata even when validation stopped before a ResourceSchema was
  // constructed (for example, on a hash mismatch).
  auto const entryMarker = message.find("catalog schema entry ");
  if (bundle && entryMarker != std::string::npos) {
    try {
      auto const begin = entryMarker + std::string_view("catalog schema entry ").size();
      auto const index = static_cast<std::size_t>(std::stoull(message.substr(begin)));
      std::ifstream input(*bundle / "catalog.json", std::ios::binary);
      Json catalog = Json::parse(input);
      auto const& entry = catalog.at("schemas").at(index);
      if (diagnostic["schemaId"].is_null() && entry.contains("schemaId"))
        diagnostic["schemaId"] = entry.at("schemaId");
      if (diagnostic["key"].is_null() && entry.value("kind", "") == "resourceType") {
        auto const resource = entry.value("resourceType", "");
        auto const factory = entry.at("factoryType").is_null()
                                 ? std::string("<default>")
                                 : entry.at("factoryType").get<std::string>();
        diagnostic["key"] = "('" + resource + "', '" + factory + "')";
      }
    } catch (std::exception const&) {
      // The primary diagnostic remains valid when malformed JSON prevents
      // best-effort metadata recovery.
    }
  }
  std::cerr << diagnostic.dump() << '\n';
}

void writeRootCopy(ResourceSchemaBundle const& bundle, std::filesystem::path const& path) {
  ResourceSchemaCatalog verified(bundle);
  auto const& entries = verified.snapshot().entries();
  auto const manifest = std::find_if(entries.begin(), entries.end(), [](auto const& entry) {
    return entry.kind == ResourceSchemaKind::manifest;
  });
  if (manifest == entries.end())
    throw ResourceSchemaCatalogException("Composed bundle has no manifest schema.");
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(manifest->contents.data(),
                               static_cast<std::streamsize>(manifest->contents.size())))
    throw ResourceSchemaCatalogException("Cannot write composed root schema '" + path.string() + "'.");
}
}  // namespace

int runResourceSchemaExporter(ResourceSchemaCatalog& catalog, int argc,
                              char const* const* argv) {
  Arguments arguments;
  try {
    arguments = parseArguments(argc, argv);
  } catch (std::invalid_argument const& error) {
    printFailure(error.what(), std::nullopt, 2);
    return 2;
  }

  for (auto const& bundle : arguments.bundles) {
    try {
      catalog.addBundle(bundle);
    } catch (std::exception const& error) {
      auto const code = failureCode(error.what());
      printFailure(error.what(), bundle, code);
      return code;
    }
  }

  try {
    auto const snapshot = catalog.snapshot();
    if (arguments.command == "list") {
      Json schemas = Json::array();
      for (auto const& entry : snapshot.entries()) {
        schemas.push_back(Json{{"kind", kindName(entry.kind)},
                               {"resourceType", entry.resourceType.empty() ? Json(nullptr)
                                                                           : Json(entry.resourceType)},
                               {"factoryType", entry.factoryType.empty() ? Json(nullptr)
                                                                         : Json(entry.factoryType)},
                               {"schemaId", entry.schemaId},
                               {"document", entry.document},
                               {"documentHash", entry.documentHash}});
      }
      std::cout << Json{{"ok", true}, {"schemas", std::move(schemas)}}.dump(2) << '\n';
    } else if (arguments.command == "verify") {
      std::cout << Json{{"ok", true},
                        {"bundles", arguments.bundles.size()},
                        {"schemas", snapshot.entries().size()}}
                       .dump()
                << '\n';
    } else if (arguments.command == "merge") {
      auto const bundle = snapshot.exportComposedBundle(arguments.rootSchemaId);
      snapshot.exportComposedBundle(arguments.output, arguments.rootSchemaId);
      std::cout << Json{{"ok", true},
                        {"output", arguments.output.generic_string()},
                        {"schemas", bundle.documents.size()}}
                       .dump()
                << '\n';
    } else {
      auto const bundle = snapshot.exportComposedBundle(arguments.rootSchemaId);
      snapshot.exportComposedBundle(arguments.output, arguments.rootSchemaId);
      auto rootPath = arguments.rootSchema;
      if (rootPath.empty()) rootPath = arguments.output / "resource-manifest.schema.json";
      writeRootCopy(bundle, rootPath);
      std::cout << Json{{"ok", true},
                        {"output", arguments.output.generic_string()},
                        {"rootSchema", rootPath.generic_string()},
                        {"schemas", bundle.documents.size()}}
                       .dump()
                << '\n';
    }
    return 0;
  } catch (std::exception const& error) {
    auto const code = failureCode(error.what());
    printFailure(error.what(), std::nullopt, code);
    return code;
  }
}

}  // namespace wp::application::resourcesystem
