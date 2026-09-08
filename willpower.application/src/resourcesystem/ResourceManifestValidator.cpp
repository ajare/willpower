#include "ResourceManifestValidator.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace wp::application::resourcesystem {
namespace {
using Json = nlohmann::json;

ResourceManifestValidator::SchemaKey const manifestSchemaKey{"ResourceManifest", ""};
constexpr std::size_t maximumValidationFailures = 100;
constexpr std::string_view commonSchemaId =
    "https://schemas.willpower.dev/resource-manifest/common.schema.json";
constexpr std::string_view resourceSchemaId =
    "https://schemas.willpower.dev/resource-manifest/resource.schema.json";

ResourceSchemaCatalogSnapshot builtInCatalog() {
  static auto const catalog = ResourceSchemaCatalog::builtIn().snapshot();
  return catalog;
}

ResourceSchema const* findSchema(ResourceSchemaCatalogSnapshot const& catalog,
                                 ResourceManifestValidator::SchemaKey const& key) {
  if (key == manifestSchemaKey) {
    auto const found = std::find_if(catalog.entries().begin(), catalog.entries().end(),
                                    [](auto const& schema) {
                                      return schema.kind == ResourceSchemaKind::manifest;
                                    });
    return found == catalog.entries().end() ? nullptr : &*found;
  }
  return catalog.find(key);
}

Json reference(std::string const& schemaId) { return Json{{"$ref", schemaId}}; }

Json collection(Json const& item) {
  return Json{{"oneOf", Json::array({item, Json{{"type", "array"},
                                                {"minItems", 1},
                                                {"items", item}}})}};
}

Json commonManifestSchema() {
  auto const resourceReference =
      reference(std::string(resourceSchemaId) + "#/definitions/resource");
  auto const resourceCollection = collection(resourceReference);
  auto const namespaceSchema = Json{
      {"type", "object"},
      {"additionalProperties", false},
      {"required", Json::array({"name", "Resource"})},
      {"properties",
       {{"name", reference(std::string(commonSchemaId) + "#/definitions/nonEmptyString")},
        {"Resource", resourceCollection}}}};

  return Json{
      {"$schema", "http://json-schema.org/draft-07/schema#"},
      {"type", "object"},
      {"additionalProperties", false},
      {"required", Json::array({"Resources"})},
      {"properties",
       {{"Resources",
         {{"type", "object"},
          {"additionalProperties", false},
          {"properties",
           {{"Resource", resourceCollection},
            {"Namespace", collection(namespaceSchema)}}}}}}}};
}

Json definitionDispatch(std::map<std::string, ResourceSchema const*> const& schemas) {
  Json dispatches = Json::array();
  for (auto const& [factoryType, schema] : schemas) {
    if (factoryType.empty()) continue;
    dispatches.push_back(
        {{"if",
          {{"required", Json::array({"factory"})},
           {"properties", {{"factory", {{"const", factoryType}}}}}}},
         {"then", reference(schema->schemaId)}});
  }

  if (dispatches.empty()) return Json();
  return Json{{"properties",
               {{"Definitions",
                 {{"properties",
                   {{"Definition", collection(Json{{"allOf", dispatches}})}}}}}}}};
}

Json dispatchManifestSchema(ResourceSchemaCatalogSnapshot const& catalog,
                            std::vector<std::string> const& preferredTypes) {
  std::map<std::string, std::map<std::string, ResourceSchema const*>> schemasByType;
  for (auto const& schema : catalog.entries()) {
    if (schema.kind == ResourceSchemaKind::resourceType) {
      schemasByType[schema.resourceType][schema.factoryType] = &schema;
    }
  }

  std::vector<std::string> orderedTypes;
  for (auto const& type : preferredTypes) {
    if (schemasByType.contains(type) &&
        std::find(orderedTypes.begin(), orderedTypes.end(), type) == orderedTypes.end()) {
      orderedTypes.push_back(type);
    }
  }
  for (auto const& [type, schemas] : schemasByType) {
    (void)schemas;
    if (std::find(orderedTypes.begin(), orderedTypes.end(), type) == orderedTypes.end()) {
      orderedTypes.push_back(type);
    }
  }

  auto const genericResource =
      reference(std::string(resourceSchemaId) + "#/definitions/resource");
  Json registeredTypes = Json::array();
  for (auto const& type : orderedTypes) registeredTypes.push_back(type);

  Json resourceBranches = Json::array();
  resourceBranches.push_back(
      {{"allOf",
        Json::array(
            {genericResource,
             Json{{"required", Json::array({"type"})},
                  {"properties", {{"type", {{"not", {{"enum", registeredTypes}}}}}}}}})}});

  for (auto const& resourceType : orderedTypes) {
    auto const& schemas = schemasByType.at(resourceType);
    Json constraints = Json::array();
    auto const defaultSchema = schemas.find("");
    constraints.push_back(defaultSchema == schemas.end()
                              ? genericResource
                              : reference(defaultSchema->second->schemaId));
    constraints.push_back(
        {{"required", Json::array({"type"})},
         {"properties", {{"type", {{"enum", Json::array({resourceType})}}}}}});
    auto specializedDispatch = definitionDispatch(schemas);
    if (!specializedDispatch.is_null()) constraints.push_back(std::move(specializedDispatch));
    resourceBranches.push_back({{"allOf", std::move(constraints)}});
  }

  auto const resourceCollection = collection(Json{{"oneOf", resourceBranches}});
  // The object type keeps the singleton branch from also matching a Namespace
  // sequence (object-only keywords are otherwise ignored for arrays), which
  // would make collection(oneOf) reject valid sequences as ambiguous.
  auto const namespaceDispatch =
      Json{{"type", "object"}, {"properties", {{"Resource", resourceCollection}}}};
  return Json{
      {"$schema", "http://json-schema.org/draft-07/schema#"},
      {"type", "object"},
      {"properties",
       {{"Resources",
         {{"properties",
           {{"Resource", resourceCollection},
            {"Namespace", collection(namespaceDispatch)}}}}}}}};
}

void appendResourceTypes(utils::YamlReader const& reader, std::string const& collectionPointer,
                         std::vector<std::string>& result) {
  if (auto type = reader.scalarAtJsonPointer(collectionPointer + "/type")) {
    result.push_back(*type);
    return;
  }
  for (std::size_t index = 0;; ++index) {
    auto type = reader.scalarAtJsonPointer(collectionPointer + "/" +
                                           std::to_string(index) + "/type");
    if (!type) return;
    result.push_back(*type);
  }
}

std::vector<std::string> resourceTypesInDocument(utils::YamlReader const& reader) {
  std::vector<std::string> result;
  appendResourceTypes(reader, "/Resources/Resource", result);
  if (reader.scalarAtJsonPointer("/Resources/Namespace/name")) {
    appendResourceTypes(reader, "/Resources/Namespace/Resource", result);
  } else {
    for (std::size_t index = 0;; ++index) {
      auto const namespacePointer =
          "/Resources/Namespace/" + std::to_string(index);
      if (!reader.scalarAtJsonPointer(namespacePointer + "/name")) break;
      appendResourceTypes(reader, namespacePointer + "/Resource", result);
    }
  }
  return result;
}

std::vector<utils::JsonSchemaDocument> schemaDocuments(
    ResourceSchemaCatalogSnapshot const& catalog) {
  std::vector<utils::JsonSchemaDocument> result;
  std::set<std::string> addedIds;
  for (auto const& schema : catalog.entries()) {
    if (addedIds.insert(schema.schemaId).second) {
      result.push_back({schema.schemaId, schema.contents});
    }
  }
  return result;
}

std::vector<std::string> pointerTokens(std::string const& pointer) {
  std::vector<std::string> tokens;
  for (std::size_t begin = pointer.empty() ? std::string::npos : 1;
       begin != std::string::npos && begin <= pointer.size();) {
    auto const end = pointer.find('/', begin);
    tokens.push_back(pointer.substr(begin, end - begin));
    begin = end == std::string::npos ? std::string::npos : end + 1;
  }
  return tokens;
}

bool isArrayIndex(std::string const& token) {
  return !token.empty() && std::all_of(token.begin(), token.end(), [](unsigned char character) {
           return std::isdigit(character) != 0;
         });
}

std::string pointerThrough(std::vector<std::string> const& tokens, std::size_t last) {
  std::string pointer;
  for (std::size_t index = 0; index <= last; ++index) pointer += '/' + tokens[index];
  return pointer;
}

void addResourceIdentity(utils::YamlReader const& reader,
                         ResourceManifestValidator::Failure& failure) {
  auto const tokens = pointerTokens(failure.instancePath);
  if (tokens.size() < 2 || tokens[0] != "Resources") return;

  std::size_t index = 1;
  std::string namespacePointer;
  if (tokens[index] == "Namespace") {
    if (++index < tokens.size() && isArrayIndex(tokens[index])) ++index;
    if (index >= tokens.size() || tokens[index] != "Resource") return;
    namespacePointer = pointerThrough(tokens, index - 1);
  } else if (tokens[index] != "Resource") {
    return;
  }

  if (++index < tokens.size() && isArrayIndex(tokens[index])) ++index;
  auto const resourcePointer = pointerThrough(tokens, index - 1);
  if (!namespacePointer.empty()) {
    if (auto namesp = reader.scalarAtJsonPointer(namespacePointer + "/name")) {
      failure.resourceNamespace = *namesp;
    }
  }
  if (auto name = reader.scalarAtJsonPointer(resourcePointer + "/name")) {
    failure.resourceName = *name;
  } else if (auto location = reader.scalarAtJsonPointer(resourcePointer + "/location")) {
    failure.resourceName = *location;
  }
  if (auto type = reader.scalarAtJsonPointer(resourcePointer + "/type")) {
    failure.resourceType = *type;
  }
}

std::vector<ResourceManifestValidator::Failure> validateSchema(
    utils::YamlReader const& reader, std::string const& manifestPath,
    ResourceManifestValidator::SchemaKey const& key, std::string const& rootSchema,
    std::vector<utils::JsonSchemaDocument> const& catalog, std::size_t maximumFailures) {
  std::vector<ResourceManifestValidator::Failure> failures;
  for (auto const& failure :
       reader.validateJsonSchema(rootSchema, catalog, maximumFailures)) {
    ResourceManifestValidator::Failure result{
        manifestPath, key.resourceType, key.factoryType, "", "", failure.instancePath,
        failure.message, failure.line, failure.column};
    addResourceIdentity(reader, result);
    failures.push_back(std::move(result));
  }
  return failures;
}

void sortFailures(std::vector<ResourceManifestValidator::Failure>& failures) {
  std::stable_sort(failures.begin(), failures.end(), [](auto const& left, auto const& right) {
    if (left.line != right.line) return left.line < right.line;
    if (left.column != right.column) return left.column < right.column;
    if (left.instancePath != right.instancePath) return left.instancePath < right.instancePath;
    return left.message < right.message;
  });
}
}  // namespace

ResourceManifestValidator::ResourceManifestValidator() : mCatalog(builtInCatalog()) {}

ResourceManifestValidator::ResourceManifestValidator(ResourceSchemaCatalogSnapshot catalog)
    : mCatalog(std::move(catalog)) {}

bool ResourceManifestValidator::contains(SchemaKey const& key) const {
  return findSchema(mCatalog, key) != nullptr;
}

std::vector<ResourceManifestValidator::Failure> ResourceManifestValidator::validate(
    utils::YamlReader const& reader, std::string const& manifestPath) const {
  auto const catalog = schemaDocuments(mCatalog);
  auto failures = validateSchema(reader, manifestPath, manifestSchemaKey,
                                 commonManifestSchema().dump(), catalog,
                                 maximumValidationFailures);

  // Dispatch is a distinct second pass. Running it even when common validation
  // found errors preserves aggregate diagnostics for other declarations in the
  // same manifest; no conversion or publication occurs between the passes.
  // oneOf reports branch bookkeeping as well as the useful leaf failures. Give
  // the dispatch pass room for every catalog branch, then de-duplicate and
  // enforce the public diagnostic bound below.
  auto dispatched = validateSchema(reader, manifestPath, manifestSchemaKey,
                                   dispatchManifestSchema(mCatalog,
                                                          resourceTypesInDocument(reader))
                                       .dump(),
                                   catalog,
                                   maximumValidationFailures *
                                       (mCatalog.entries().size() + 1U));
  failures.insert(failures.end(), std::make_move_iterator(dispatched.begin()),
                  std::make_move_iterator(dispatched.end()));
  sortFailures(failures);
  failures.erase(std::unique(failures.begin(), failures.end(), [](auto const& left,
                                                                  auto const& right) {
                   return left.line == right.line && left.column == right.column &&
                          left.instancePath == right.instancePath &&
                          left.message == right.message;
                 }),
                 failures.end());
  if (failures.size() > maximumValidationFailures) {
    failures.resize(maximumValidationFailures);
  }
  return failures;
}

std::vector<ResourceManifestValidator::Failure> ResourceManifestValidator::validate(
    utils::YamlReader const& reader, std::string const& manifestPath,
    SchemaKey const& key) const {
  auto const* root = findSchema(mCatalog, key);
  if (!root) return {};

  auto failures = validateSchema(reader, manifestPath, key, root->contents,
                                 schemaDocuments(mCatalog), maximumValidationFailures);
  sortFailures(failures);
  return failures;
}

}  // namespace wp::application::resourcesystem
