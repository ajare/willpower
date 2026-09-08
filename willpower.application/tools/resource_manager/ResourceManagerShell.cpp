#include "ResourceManagerShell.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#endif

namespace resource_manager {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace wp::application::resourcesystem;

std::string displayPath(fs::path const& path) {
  auto const utf8 = path.generic_u8string();
  return std::string(reinterpret_cast<char const*>(utf8.data()), utf8.size());
}

bool accessibleDirectory(fs::path const& path, fs::path& canonical,
                         std::string& failure) {
  std::error_code error;
  if (!fs::is_directory(path, error)) {
    failure = "Base directory is not an accessible directory: " +
              displayPath(path);
    if (error) failure += ": " + error.message();
    return false;
  }
  canonical = fs::canonical(path, error);
  if (error) {
    failure = "Could not canonicalize base directory '" + displayPath(path) +
              "': " + error.message();
    return false;
  }
  return true;
}

std::string validationMessage(ResourceManifestValidationResult const& result) {
  if (result.diagnostics.empty()) return "Resource Manifest is not structurally valid.";
  auto const& diagnostic = result.diagnostics.front();
  std::ostringstream message;
  message << diagnostic.message;
  if (!diagnostic.instancePath.empty()) message << " at " << diagnostic.instancePath;
  if (diagnostic.line > 0) {
    message << " (line " << diagnostic.line;
    if (diagnostic.column > 0) message << ", column " << diagnostic.column;
    message << ')';
  }
  if (result.diagnostics.size() > 1) {
    message << " (and " << (result.diagnostics.size() - 1)
            << " more diagnostic(s))";
  }
  return message.str();
}

fs::path temporarySibling(fs::path const& destination) {
  auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  auto const name = destination.filename().string();
  for (unsigned int attempt = 0; attempt != 100; ++attempt) {
    auto candidate = parent /
                     ("." + name + ".tmp-" + std::to_string(stamp) + "-" +
                      std::to_string(attempt));
    std::error_code error;
    if (!fs::exists(candidate, error)) return candidate;
  }
  throw std::runtime_error("Could not reserve a sibling temporary file.");
}

void replaceAtomically(fs::path const& temporary, fs::path const& destination) {
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    auto const code = GetLastError();
    throw std::system_error(static_cast<int>(code), std::system_category(),
                            "Could not atomically replace Resource Manifest");
  }
#else
  if (::rename(temporary.c_str(), destination.c_str()) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "Could not atomically replace Resource Manifest");
  }
#endif
}

void writeAtomically(fs::path const& destination, std::string const& bytes) {
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  std::error_code error;
  if (!fs::is_directory(parent, error)) {
    throw std::runtime_error("Save directory is not accessible: " +
                             displayPath(parent));
  }

  auto temporary = temporarySibling(destination);
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Could not create sibling temporary file '" +
                               displayPath(temporary) + "'.");
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      throw std::runtime_error("Could not write sibling temporary file '" +
                               displayPath(temporary) + "'.");
    }
    output.close();
    replaceAtomically(temporary, destination);
  } catch (...) {
    fs::remove(temporary, error);
    throw;
  }
}

std::string readBytes(fs::path const& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::vector<YAML::Node> collectionItems(YAML::Node const& collection) {
  std::vector<YAML::Node> result;
  if (!collection || collection.IsNull()) return result;
  if (collection.IsSequence()) {
    result.reserve(collection.size());
    for (auto const& item : collection) result.push_back(YAML::Clone(item));
  } else {
    result.push_back(YAML::Clone(collection));
  }
  return result;
}

void setCollection(YAML::Node parent, char const* key,
                   std::vector<YAML::Node> const& items) {
  if (items.empty()) {
    parent.remove(key);
  } else if (items.size() == 1U) {
    parent[key] = items.front();
  } else {
    YAML::Node sequence(YAML::NodeType::Sequence);
    for (auto const& item : items) sequence.push_back(item);
    parent[key] = sequence;
  }
}

std::vector<YAML::Node> standardDependencyItems(YAML::Node const& resource) {
  auto dependentResources = resource["DependentResources"];
  if (!dependentResources || !dependentResources.IsMap()) return {};
  return collectionItems(dependentResources["DependentResource"]);
}

// YAML::Node assignment mutates the aliased node rather than behaving like a
// value assignment. Rebuild vectors when changing their shape so std::vector's
// shifting assignments cannot duplicate or overwrite neighboring nodes.
void eraseCollectionItem(std::vector<YAML::Node>& items, std::size_t index) {
  std::vector<YAML::Node> replacement;
  replacement.reserve(items.size() - 1U);
  for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
    if (itemIndex != index) replacement.push_back(YAML::Clone(items[itemIndex]));
  }
  items.swap(replacement);
}

void insertCollectionItem(std::vector<YAML::Node>& items, std::size_t index,
                          YAML::Node const& item) {
  std::vector<YAML::Node> replacement;
  replacement.reserve(items.size() + 1U);
  for (std::size_t itemIndex = 0; itemIndex <= items.size(); ++itemIndex) {
    if (itemIndex == index) replacement.push_back(YAML::Clone(item));
    if (itemIndex < items.size())
      replacement.push_back(YAML::Clone(items[itemIndex]));
  }
  items.swap(replacement);
}

std::string scalar(YAML::Node const& node, char const* key) {
  auto value = node[key];
  return value && value.IsScalar() ? value.as<std::string>() : std::string{};
}

std::string resourceIdentity(YAML::Node const& node) {
  auto name = scalar(node, "name");
  return name.empty() ? scalar(node, "location") : name;
}

struct ResourceCollection {
  YAML::Node owner;
  std::vector<YAML::Node> resources;
  std::vector<YAML::Node> namespaces;
  std::size_t namespaceIndex = 0;
  bool named = false;
};

std::optional<ResourceCollection> findResourceCollection(
    YAML::Node root, std::string const& resourceNamespace) {
  auto resourcesRoot = root["Resources"];
  if (!resourcesRoot || !resourcesRoot.IsMap()) return {};
  if (resourceNamespace.empty()) {
    return ResourceCollection{resourcesRoot,
                              collectionItems(resourcesRoot["Resource"]), {}, 0,
                              false};
  }
  auto namespaces = collectionItems(resourcesRoot["Namespace"]);
  for (std::size_t index = 0; index < namespaces.size(); ++index) {
    if (scalar(namespaces[index], "name") == resourceNamespace) {
      return ResourceCollection{namespaces[index],
                                collectionItems(namespaces[index]["Resource"]),
                                std::move(namespaces), index, true};
    }
  }
  return {};
}

void publishCollection(YAML::Node root, ResourceCollection& collection) {
  setCollection(collection.owner, "Resource", collection.resources);
  if (collection.named) {
    collection.namespaces[collection.namespaceIndex] = collection.owner;
    setCollection(root["Resources"], "Namespace", collection.namespaces);
  }
}

std::optional<std::size_t> findResourceIndex(ResourceCollection const& collection,
                                             std::string const& name) {
  for (std::size_t index = 0; index < collection.resources.size(); ++index) {
    if (resourceIdentity(collection.resources[index]) == name) return index;
  }
  return {};
}

std::string emitYaml(YAML::Node const& root) {
  YAML::Emitter output;
  output.SetIndent(2);
  output << YAML::Block << root;
  if (!output.good()) throw std::runtime_error(output.GetLastError());
  return std::string(output.c_str(), output.size()) + "\n";
}

bool validResourceName(std::string const& name) {
  return !name.empty() && name.find('/') == std::string::npos;
}

bool validNamespaceName(std::string const& name) {
  return !name.empty() && name.find('/') == std::string::npos;
}

struct ResourceIdentity {
  std::string resourceNamespace;
  std::string name;

  bool operator==(ResourceIdentity const&) const = default;
  bool operator<(ResourceIdentity const& other) const {
    return std::tie(resourceNamespace, name) <
           std::tie(other.resourceNamespace, other.name);
  }
};

std::string qualifiedIdentity(ResourceIdentity const& identity) {
  return identity.resourceNamespace.empty()
             ? identity.name
             : identity.resourceNamespace + "/" + identity.name;
}

struct ResourceDeclaration {
  ResourceIdentity identity;
  std::string resourceType;
  bool inlineResource = false;
  ResourceIdentity owner;
  std::size_t dependencyIndex = 0;
};

std::vector<ResourceDeclaration> resourceDeclarations(YAML::Node const& root) {
  std::vector<ResourceDeclaration> result;
  auto append = [&](std::string const& resourceNamespace,
                    YAML::Node const& collection) {
    for (auto const& resource : collectionItems(collection)) {
      ResourceIdentity const owner{resourceNamespace, resourceIdentity(resource)};
      result.push_back({owner, scalar(resource, "type")});
      auto dependencies = standardDependencyItems(resource);
      for (std::size_t index = 0; index < dependencies.size(); ++index) {
        auto const& dependency = dependencies[index];
        if (scalar(dependency, "ref").empty() &&
            !scalar(dependency, "type").empty()) {
          result.push_back({{resourceNamespace, resourceIdentity(dependency)},
                            scalar(dependency, "type"), true, owner, index});
        }
      }
    }
  };
  auto resourcesRoot = root["Resources"];
  append({}, resourcesRoot["Resource"]);
  for (auto const& item : collectionItems(resourcesRoot["Namespace"])) {
    append(scalar(item, "name"), item["Resource"]);
  }
  return result;
}

std::vector<std::string> standardAllowedTypes(std::string const& ownerType,
                                               std::string const& dependencyId) {
  if (ownerType == "ImageSet" && dependencyId == "Image") return {"Image"};
  if (ownerType == "AnimationSet" && dependencyId == "Image")
    return {"ImageSet"};
  if (ownerType == "Program" &&
      (dependencyId == "Vertex" || dependencyId == "Fragment"))
    return {"Shader"};
  if (ownerType == "Material")
    return dependencyId == "Program" ? std::vector<std::string>{"Program"}
                                     : std::vector<std::string>{"Image"};
  return {};
}

bool allowedResourceType(std::vector<std::string> const& allowed,
                         std::string const& type) {
  return allowed.empty() ||
         std::find(allowed.begin(), allowed.end(), type) != allowed.end();
}

ResourceIdentity parseReference(std::string const& reference,
                                std::string const& ownerNamespace) {
  auto const separator = reference.find('/');
  if (separator == std::string::npos) return {ownerNamespace, reference};
  return {reference.substr(0, separator), reference.substr(separator + 1U)};
}

std::string formatReference(ResourceIdentity const& target,
                            std::string const& ownerNamespace) {
  if (target.resourceNamespace == ownerNamespace) return target.name;
  return target.resourceNamespace + "/" + target.name;
}

template <typename Visitor>
void forEachStandardReference(YAML::Node const& root, Visitor visitor) {
  auto visitCollection = [&](std::string const& resourceNamespace,
                             YAML::Node const& collection) {
    for (auto const& resource : collectionItems(collection)) {
      ResourceIdentity const owner{resourceNamespace, resourceIdentity(resource)};
      auto dependentResources = resource["DependentResources"];
      if (!dependentResources || !dependentResources.IsMap()) continue;
      for (auto const& dependency :
           collectionItems(dependentResources["DependentResource"])) {
        auto reference = scalar(dependency, "ref");
        if (!reference.empty()) {
          visitor(owner, parseReference(reference, resourceNamespace));
        }
      }
    }
  };

  auto resourcesRoot = root["Resources"];
  visitCollection({}, resourcesRoot["Resource"]);
  for (auto const& item : collectionItems(resourcesRoot["Namespace"])) {
    visitCollection(scalar(item, "name"), item["Resource"]);
  }
}

bool dependencyPathExists(YAML::Node const& root,
                          ResourceIdentity const& start,
                          ResourceIdentity const& target) {
  std::map<ResourceIdentity, std::vector<ResourceIdentity>> edges;
  forEachStandardReference(
      root, [&](ResourceIdentity const& source,
                ResourceIdentity const& referenced) {
        edges[source].push_back(referenced);
      });
  for (auto const& declaration : resourceDeclarations(root)) {
    if (declaration.inlineResource) {
      edges[declaration.owner].push_back(declaration.identity);
    }
  }

  std::set<ResourceIdentity> visited;
  std::vector<ResourceIdentity> work{start};
  while (!work.empty()) {
    auto current = std::move(work.back());
    work.pop_back();
    if (current == target) return true;
    if (!visited.insert(current).second) continue;
    auto found = edges.find(current);
    if (found != edges.end()) {
      work.insert(work.end(), found->second.begin(), found->second.end());
    }
  }
  return false;
}

std::vector<ResourceDeclaration> matchingDeclarations(
    std::vector<ResourceDeclaration> const& declarations,
    ResourceIdentity const& identity) {
  std::vector<ResourceDeclaration> result;
  std::copy_if(declarations.begin(), declarations.end(),
               std::back_inserter(result), [&](auto const& declaration) {
                 return declaration.identity == identity;
               });
  return result;
}

template <typename Transform>
void rewriteStandardReferences(YAML::Node root, Transform transform) {
  auto rewriteCollection = [&](YAML::Node owner,
                               std::string const& resourceNamespace) {
    auto resources = collectionItems(owner["Resource"]);
    for (auto& resource : resources) {
      ResourceIdentity const oldOwner{resourceNamespace,
                                      resourceIdentity(resource)};
      auto const newOwner = transform(oldOwner);
      auto dependentResources = resource["DependentResources"];
      if (!dependentResources || !dependentResources.IsMap()) continue;
      auto dependencies =
          collectionItems(dependentResources["DependentResource"]);
      for (auto& dependency : dependencies) {
        auto reference = scalar(dependency, "ref");
        if (reference.empty()) continue;
        auto const oldTarget = parseReference(reference, resourceNamespace);
        auto const newTarget = transform(oldTarget);
        if (newOwner == oldOwner && newTarget == oldTarget) continue;
        dependency["ref"] =
            formatReference(newTarget, newOwner.resourceNamespace);
      }
      setCollection(dependentResources, "DependentResource", dependencies);
      resource["DependentResources"] = dependentResources;
    }
    setCollection(owner, "Resource", resources);
  };

  auto resourcesRoot = root["Resources"];
  rewriteCollection(resourcesRoot, {});
  auto namespaces = collectionItems(resourcesRoot["Namespace"]);
  for (auto& item : namespaces) {
    rewriteCollection(item, scalar(item, "name"));
  }
  setCollection(resourcesRoot, "Namespace", namespaces);
  root["Resources"] = resourcesRoot;
}

std::size_t namespaceCount(YAML::Node const& root, std::string const& name) {
  auto const namespaces = collectionItems(root["Resources"]["Namespace"]);
  return static_cast<std::size_t>(std::count_if(
      namespaces.begin(), namespaces.end(), [&](YAML::Node const& item) {
        return scalar(item, "name") == name;
      }));
}

std::size_t resourceCount(ResourceCollection const& collection,
                          std::string const& name) {
  return static_cast<std::size_t>(std::count_if(
      collection.resources.begin(), collection.resources.end(),
      [&](YAML::Node const& resource) {
        return resourceIdentity(resource) == name;
      }));
}

std::vector<std::string> pathParts(std::string const& path) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin < path.size()) {
    while (begin < path.size() && path[begin] == '/') ++begin;
    if (begin == path.size()) break;
    auto const end = path.find('/', begin);
    result.push_back(path.substr(begin, end - begin));
    begin = end == std::string::npos ? path.size() : end + 1U;
  }
  return result;
}

bool indexPart(std::string const& value, std::size_t& index) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return !std::isdigit(character);
      })) {
    return false;
  }
  try {
    index = static_cast<std::size_t>(std::stoull(value));
    return true;
  } catch (...) {
    return false;
  }
}

YAML::Node nodeAtPath(YAML::Node root, std::vector<std::string> const& parts,
                      std::size_t count) {
  YAML::Node current;
  current.reset(root);
  for (std::size_t partIndex = 0; partIndex < count; ++partIndex) {
    std::size_t index = 0;
    if (indexPart(parts[partIndex], index)) {
      if (current.IsSequence()) {
        if (index >= current.size()) return {};
        YAML::Node child = current[index];
        current.reset(child);
      } else if (index != 0U || !current) {
        return {};
      }
    } else {
      if (!current.IsMap() || !current[parts[partIndex]]) return {};
      YAML::Node child = current[parts[partIndex]];
      current.reset(child);
    }
  }
  return current;
}

std::optional<std::pair<YAML::Node, std::string>> collectionAtPath(
    YAML::Node resource, std::string const& path) {
  auto parts = pathParts(path);
  if (parts.empty()) return {};
  auto parent = nodeAtPath(resource, parts, parts.size() - 1U);
  if (!parent || !parent.IsMap()) return {};
  return std::pair{parent, parts.back()};
}

std::string collectionName(NestedCollectionKind kind) {
  switch (kind) {
    case NestedCollectionKind::definition:
      return "Definition";
    case NestedCollectionKind::image:
      return "Image";
    case NestedCollectionKind::imageSet:
      return "ImageSet";
    case NestedCollectionKind::animation:
      return "Animation";
    case NestedCollectionKind::frame:
    case NestedCollectionKind::overrideFrame:
      return "Frame";
    case NestedCollectionKind::tag:
      return "Tag";
    case NestedCollectionKind::buffer:
      return "Buffer";
    case NestedCollectionKind::channel:
      return "Channel";
    case NestedCollectionKind::texture:
      return "Texture";
    case NestedCollectionKind::object:
      return {};
  }
  return {};
}

YAML::Node defaultNestedItem(NestedCollectionKind kind) {
  YAML::Node item(YAML::NodeType::Map);
  switch (kind) {
    case NestedCollectionKind::definition:
      item["Images"]["Image"] = defaultNestedItem(NestedCollectionKind::image);
      break;
    case NestedCollectionKind::image:
      item["name"] = "Image";
      item["x"] = 0;
      item["y"] = 0;
      item["width"] = 1;
      item["height"] = 1;
      break;
    case NestedCollectionKind::imageSet:
      item["name"] = "ImageSet";
      item["x"] = 0;
      item["y"] = 0;
      item["width"] = 1;
      item["height"] = 1;
      item["count"] = 1;
      item["dx"] = 0;
      item["dy"] = 0;
      break;
    case NestedCollectionKind::animation:
      item["name"] = "Animation";
      item["Frames"]["Frame"] = defaultNestedItem(NestedCollectionKind::frame);
      break;
    case NestedCollectionKind::frame:
      item["image"] = "Image";
      item["time"] = 1.0;
      break;
    case NestedCollectionKind::overrideFrame:
      item["frame"] = 0;
      break;
    case NestedCollectionKind::tag:
      item["key"] = "Tag";
      item["value"] = "";
      break;
    case NestedCollectionKind::buffer:
      item["Channels"]["Channel"] =
          defaultNestedItem(NestedCollectionKind::channel);
      break;
    case NestedCollectionKind::channel:
      item["data"] = "POSITION3";
      item["type"] = "FLOAT32";
      item["normalised"] = false;
      break;
    case NestedCollectionKind::texture:
      item["sampler"] = "Texture";
      item["type"] = "default";
      break;
    case NestedCollectionKind::object:
      break;
  }
  return item;
}

NestedPropertyForm propertyForm(YAML::Node const& item, std::string name,
                                bool required, bool integer = false,
                                bool number = false) {
  NestedPropertyForm result;
  result.name = std::move(name);
  result.value = scalar(item, result.name.c_str());
  result.required = required;
  result.optional = !required;
  result.integer = integer;
  result.number = number;
  return result;
}

bool schemaScalarMatches(YAML::Node const& node, Json const& expected) {
  if (!node || !node.IsScalar()) return false;
  try {
    if (expected.is_string()) return node.as<std::string>() == expected.get<std::string>();
    if (expected.is_boolean()) return node.as<bool>() == expected.get<bool>();
    if (expected.is_number_integer())
      return node.as<long long>() == expected.get<long long>();
    if (expected.is_number()) return node.as<double>() == expected.get<double>();
  } catch (...) {
  }
  return false;
}

Json mergeSchemaShapes(Json left, Json const& right) {
  if (!left.is_object()) left = Json::object();
  if (!right.is_object()) return left;
  if (right.contains("properties") && right.at("properties").is_object()) {
    if (!left.contains("properties") || !left.at("properties").is_object())
      left["properties"] = Json::object();
    for (auto const& [name, property] : right.at("properties").items())
      left["properties"][name] = property;
  }
  if (right.contains("required") && right.at("required").is_array()) {
    if (!left.contains("required") || !left.at("required").is_array())
      left["required"] = Json::array();
    for (auto const& required : right.at("required")) {
      if (std::find(left["required"].begin(), left["required"].end(), required) ==
          left["required"].end()) {
        left["required"].push_back(required);
      }
    }
  }
  for (auto const& [name, value] : right.items()) {
    if (name != "properties" && name != "required" && name != "allOf" &&
        name != "oneOf" && name != "anyOf" && name != "if" && name != "then" &&
        name != "else" && name != "$ref") {
      left[name] = value;
    }
  }
  return left;
}

Json schemaReference(ResourceSchemaCatalogSnapshot const& catalog,
                     std::string const& currentSchemaId,
                     std::string const& reference) {
  auto const hash = reference.find('#');
  auto schemaId = reference.substr(0, hash);
  if (schemaId.empty()) schemaId = currentSchemaId;
  auto const* entry = catalog.findBySchemaId(schemaId);
  if (!entry) throw std::runtime_error("uncatalogued schema reference");
  auto document = Json::parse(entry->contents);
  if (hash != std::string::npos && hash + 1U < reference.size()) {
    auto pointer = reference.substr(hash + 1U);
    if (pointer.empty() || pointer.front() != '/')
      throw std::runtime_error("unsupported schema reference fragment");
    return document.at(Json::json_pointer(pointer));
  }
  return document;
}

bool schemaBranchMatches(YAML::Node const& node, Json const& schema) {
  if (!schema.is_object()) return false;
  if (schema.contains("type") && schema.at("type").is_string()) {
    auto const type = schema.at("type").get<std::string>();
    if ((type == "object" && (!node || !node.IsMap())) ||
        (type == "array" && (!node || !node.IsSequence()))) {
      return false;
    }
  }
  if (schema.contains("required") && schema.at("required").is_array()) {
    if (!node || !node.IsMap()) return false;
    for (auto const& required : schema.at("required")) {
      if (!required.is_string() || !node[required.get<std::string>()]) return false;
    }
  }
  if (node && node.IsMap() && schema.contains("properties") &&
      schema.at("properties").is_object()) {
    for (auto const& [name, property] : schema.at("properties").items()) {
      if (!node[name] || !property.is_object()) continue;
      if (property.contains("const") &&
          !schemaScalarMatches(node[name], property.at("const"))) {
        return false;
      }
      if (property.contains("enum") && property.at("enum").is_array() &&
          std::none_of(property.at("enum").begin(), property.at("enum").end(),
                       [&](auto const& value) {
                         return schemaScalarMatches(node[name], value);
                       })) {
        return false;
      }
    }
  }
  return true;
}

Json expandSchema(ResourceSchemaCatalogSnapshot const& catalog, Json schema,
                  std::string const& currentSchemaId, YAML::Node const& node,
                  unsigned int depth = 0U) {
  if (depth > 32U || !schema.is_object())
    throw std::runtime_error("unsupported schema composition depth");
  if (schema.contains("$ref")) {
    auto resolved = schemaReference(catalog, currentSchemaId,
                                    schema.at("$ref").get<std::string>());
    schema.erase("$ref");
    schema = mergeSchemaShapes(
        expandSchema(catalog, std::move(resolved), currentSchemaId, node,
                     depth + 1U),
        schema);
  }
  Json result = schema;
  if (schema.contains("allOf") && schema.at("allOf").is_array()) {
    for (auto const& branch : schema.at("allOf")) {
      result = mergeSchemaShapes(
          std::move(result),
          expandSchema(catalog, branch, currentSchemaId, node, depth + 1U));
    }
  }
  for (auto const* keyword : {"oneOf", "anyOf"}) {
    if (!schema.contains(keyword) || !schema.at(keyword).is_array() ||
        schema.at(keyword).empty()) {
      continue;
    }
    auto branch = std::find_if(schema.at(keyword).begin(), schema.at(keyword).end(),
                               [&](auto const& candidate) {
                                 return schemaBranchMatches(node, candidate);
                               });
    if (branch == schema.at(keyword).end()) branch = schema.at(keyword).begin();
    result = mergeSchemaShapes(
        std::move(result),
        expandSchema(catalog, *branch, currentSchemaId, node, depth + 1U));
  }
  if (schema.contains("if") && schema.at("if").is_object()) {
    auto const matches = schemaBranchMatches(node, schema.at("if"));
    auto const* keyword = matches ? "then" : "else";
    if (schema.contains(keyword)) {
      result = mergeSchemaShapes(
          std::move(result), expandSchema(catalog, schema.at(keyword),
                                          currentSchemaId, node, depth + 1U));
    }
  }
  return result;
}

void appendTags(std::vector<NestedFormItem>& result, YAML::Node const& owner,
                std::string const& ownerPath) {
  auto tags = owner["Tags"];
  if (!tags || !tags.IsMap()) return;
  auto items = collectionItems(tags["Tag"]);
  for (std::size_t index = 0; index < items.size(); ++index) {
    NestedFormItem tag;
    tag.kind = NestedCollectionKind::tag;
    tag.path = ownerPath + "/Tags/Tag/" + std::to_string(index);
    tag.label = "Tag " + std::to_string(index + 1U);
    tag.properties = {propertyForm(items[index], "key", true),
                      propertyForm(items[index], "value", true)};
    result.push_back(std::move(tag));
  }
}

bool equalPathComponent(fs::path const& left, fs::path const& right) {
#if defined(_WIN32)
  auto l = left.native();
  auto r = right.native();
  std::transform(l.begin(), l.end(), l.begin(), ::towlower);
  std::transform(r.begin(), r.end(), r.begin(), ::towlower);
  return l == r;
#else
  return left == right;
#endif
}

bool containedBy(fs::path const& base, fs::path const& target) {
  auto basePart = base.begin();
  auto targetPart = target.begin();
  for (; basePart != base.end(); ++basePart, ++targetPart) {
    if (targetPart == target.end() ||
        !equalPathComponent(*basePart, *targetPart)) {
      return false;
    }
  }
  return true;
}

constexpr char editorVersionKeyword[] = "x-willpower-editor-version";
constexpr char widgetKeyword[] = "x-willpower-widget";
constexpr char allowedTypesKeyword[] = "x-willpower-allowed-resource-types";
constexpr char referenceScopeKeyword[] = "x-willpower-reference-scope";

bool validFileExtension(std::string const& extension) {
  return !extension.empty() && extension.front() != '.' &&
         extension.find_first_of(";*\\/") == std::string::npos;
}

void validateAnnotations(Json const& value, std::string const& context) {
  if (value.is_object()) {
    std::set<std::string> annotationKeys;
    for (auto const& [name, child] : value.items()) {
      if (name.starts_with("x-willpower-")) annotationKeys.insert(name);
      validateAnnotations(child, context + "/" + name);
    }
    if (annotationKeys.empty()) return;
    if (!value.contains(editorVersionKeyword) ||
        !value.at(editorVersionKeyword).is_string() ||
        value.at(editorVersionKeyword) != "1.0" ||
        !value.contains(widgetKeyword) ||
        !value.at(widgetKeyword).is_string()) {
      throw std::runtime_error(
          context + ": editor annotations require version '1.0' and a widget.");
    }
    auto const widget = value.at(widgetKeyword).get<std::string>();
    std::set<std::string> expected{std::string(editorVersionKeyword),
                                   std::string(widgetKeyword)};
    if (widget == "file") {
      expected.insert("x-willpower-file-kind");
      expected.insert("x-willpower-file-extensions");
      if (!value.contains("x-willpower-file-kind") ||
          !value.at("x-willpower-file-kind").is_string() ||
          value.at("x-willpower-file-kind")
              .get_ref<std::string const&>()
              .empty() ||
          !value.contains("x-willpower-file-extensions") ||
          !value.at("x-willpower-file-extensions").is_array()) {
        throw std::runtime_error(context +
                                 ": malformed file widget annotation.");
      }
      auto const extensions = value.at("x-willpower-file-extensions")
                                  .get<std::vector<std::string>>();
      if (extensions.empty() ||
          std::any_of(extensions.begin(), extensions.end(),
                      [](auto const& extension) {
                        return !validFileExtension(extension);
                      })) {
        throw std::runtime_error(context + ": invalid file filter extensions.");
      }
    } else if (widget == "resource-reference") {
      expected.insert(std::string(allowedTypesKeyword));
      expected.insert(std::string(referenceScopeKeyword));
      if (!value.contains(allowedTypesKeyword) ||
          !value.at(allowedTypesKeyword).is_array() ||
          value.at(allowedTypesKeyword).empty() ||
          !value.contains(referenceScopeKeyword) ||
          value.at(referenceScopeKeyword) != "manifest") {
        throw std::runtime_error(
            context + ": malformed Resource-reference widget annotation.");
      }
      std::set<std::string> types;
      for (auto const& type : value.at(allowedTypesKeyword)) {
        if (!type.is_string() || type.get_ref<std::string const&>().empty() ||
            !types.insert(type.get<std::string>()).second) {
          throw std::runtime_error(
              context +
              ": allowed Resource Types must be unique non-empty strings.");
        }
      }
    } else {
      throw std::runtime_error(context + ": unsupported editor widget '" +
                               widget + "'.");
    }
    if (annotationKeys != expected) {
      throw std::runtime_error(
          context + ": incomplete or unknown editor annotation keyword.");
    }
  } else if (value.is_array()) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      validateAnnotations(value[index], context + "/" + std::to_string(index));
    }
  }
}

void collectAnnotatedDependencies(
    Json const& value, std::vector<ResourceDependencyForm>& result);

void validateEditorCatalog(ResourceSchemaCatalogSnapshot const& catalog) {
  std::size_t manifests = 0;
  for (auto const& entry : catalog.entries()) {
    if (entry.kind == ResourceSchemaKind::manifest) ++manifests;
    auto document = Json::parse(entry.contents);
    validateAnnotations(document, "Schema '" + entry.schemaId + "'");
    std::vector<ResourceDependencyForm> annotatedDependencies;
    collectAnnotatedDependencies(document, annotatedDependencies);
  }
  if (manifests != 1U) {
    throw std::runtime_error(
        "An editor Resource Schema Catalog must contain "
        "exactly one manifest schema.");
  }
}

void collectAnnotatedDependencies(Json const& value,
                                  std::vector<ResourceDependencyForm>& result) {
  if (value.is_object()) {
    if (value.contains("properties") && value.at("properties").is_object()) {
      auto const& properties = value.at("properties");
      bool const annotatedReference =
          properties.contains("ref") && properties.at("ref").is_object() &&
          properties.at("ref").value(std::string(widgetKeyword),
                                     std::string{}) == "resource-reference";
      bool const identifiesDependency =
          properties.contains("id") && properties.at("id").is_object() &&
          properties.at("id").contains("enum") &&
          properties.at("id").at("enum").is_array() &&
          properties.at("id").at("enum").size() == 1U &&
          properties.at("id").at("enum").front().is_string();
      if (annotatedReference && !identifiesDependency) {
        throw std::runtime_error(
            "A Resource-reference annotation must be paired with a single "
            "dependency id enum.");
      }
      if (annotatedReference) {
        ResourceDependencyForm dependency;
        dependency.id =
            properties.at("id").at("enum").front().get<std::string>();
        dependency.allowedResourceTypes = properties.at("ref")
                                              .at(allowedTypesKeyword)
                                              .get<std::vector<std::string>>();
        auto found = std::find_if(
            result.begin(), result.end(),
            [&](auto const& item) { return item.id == dependency.id; });
        if (found == result.end()) {
          result.push_back(std::move(dependency));
        } else if (found->allowedResourceTypes !=
                   dependency.allowedResourceTypes) {
          throw std::runtime_error(
              "conflicting Resource-reference annotations for dependency '" +
              dependency.id + "'.");
        }
      }
    }
    for (auto const& [name, child] : value.items()) {
      static_cast<void>(name);
      collectAnnotatedDependencies(child, result);
    }
  } else if (value.is_array()) {
    for (auto const& child : value) collectAnnotatedDependencies(child, result);
  }
}

Json defaultDefinitionSchema(ResourceSchemaCatalogSnapshot const& catalog,
                             ResourceSchema const& entry) {
  auto schema = Json::parse(entry.contents);
  auto definition = schema.at("definitions").at("defaultDefinition");
  return expandSchema(catalog, std::move(definition), entry.schemaId,
                      YAML::Node(YAML::NodeType::Map));
}

bool supportedScalarSchema(Json const& schema) {
  if (!schema.is_object()) return false;
  if (schema.contains("enum") && schema.at("enum").is_array() &&
      !schema.at("enum").empty()) {
    return std::all_of(schema.at("enum").begin(), schema.at("enum").end(),
                       [](auto const& value) {
                         return value.is_string() || value.is_boolean() ||
                                value.is_number();
                       });
  }
  auto const type = schema.value("type", std::string{});
  return type == "string" || type == "integer" || type == "number" ||
         type == "boolean";
}

bool supportedDefaultDefinition(ResourceSchemaCatalogSnapshot const& catalog,
                                ResourceSchema const& entry) {
  auto shape = defaultDefinitionSchema(catalog, entry);
  if (shape.value("type", std::string{}) != "object" ||
      !shape.contains("properties") || !shape.at("properties").is_object()) {
    return false;
  }
  for (auto const& [name, rawProperty] : shape.at("properties").items()) {
    static_cast<void>(name);
    auto property =
        expandSchema(catalog, rawProperty, entry.schemaId, YAML::Node());
    if (!supportedScalarSchema(property)) return false;
  }
  return true;
}

void assignJsonScalar(YAML::Node target, std::string const& name,
                      Json const& value) {
  if (value.is_string())
    target[name] = value.get<std::string>();
  else if (value.is_boolean())
    target[name] = value.get<bool>();
  else if (value.is_number_integer())
    target[name] = value.get<long long>();
  else if (value.is_number())
    target[name] = value.get<double>();
  else
    throw std::runtime_error("starter value is not a scalar");
}

YAML::Node applicationDefaultDefinition(
    ResourceSchemaCatalogSnapshot const& catalog, ResourceSchema const& entry) {
  auto shape = defaultDefinitionSchema(catalog, entry);
  YAML::Node result(YAML::NodeType::Map);
  auto required = shape.value("required", std::vector<std::string>{});
  for (auto const& name : required) {
    auto property = expandSchema(catalog, shape.at("properties").at(name),
                                 entry.schemaId, YAML::Node());
    if (property.contains("const")) {
      assignJsonScalar(result, name, property.at("const"));
    } else if (property.contains("default")) {
      assignJsonScalar(result, name, property.at("default"));
    } else if (property.contains("enum") && !property.at("enum").empty()) {
      assignJsonScalar(result, name, property.at("enum").front());
    } else {
      auto const type = property.value("type", std::string{});
      if (type == "string") {
        auto length = property.value("minLength", 1U);
        result[name] = std::string((std::max)(1U, length), 'x');
      } else if (type == "boolean") {
        result[name] = false;
      } else if (type == "integer") {
        auto value = property.value("minimum", 0.0);
        if (property.contains("exclusiveMinimum") &&
            property.at("exclusiveMinimum").is_number()) {
          value = property.at("exclusiveMinimum").get<double>() + 1.0;
        }
        result[name] = static_cast<long long>(std::ceil(value));
      } else if (type == "number") {
        auto value = property.value("minimum", 0.0);
        if (property.contains("exclusiveMinimum") &&
            property.at("exclusiveMinimum").is_number()) {
          value = property.at("exclusiveMinimum").get<double>() + 1.0;
        }
        result[name] = value;
      } else {
        throw std::runtime_error("required default Definition property '" +
                                 name +
                                 "' is outside the supported form subset");
      }
    }
  }
  return result;
}

std::vector<ResourceForm> loadResourceForms(
    ResourceSchemaCatalogSnapshot const& catalog) {
  static std::set<std::string> const builtInTypes{
      "TextFile", "XmlFile", "Shader", "AudioBank", "Image",
      "ImageSet", "AnimationSet", "Program", "Material"};
  std::vector<ResourceForm> result;
  for (auto const& entry : catalog.entries()) {
    if (entry.kind != ResourceSchemaKind::resourceType ||
        !entry.factoryType.empty()) {
      continue;
    }
    try {
      auto schema = Json::parse(entry.contents);
      auto const& resource = schema.at("definitions").at("resource");
      auto const& own = resource.at("allOf").at(1);
      auto const& ownProperties = own.at("properties");
      if (ownProperties.at("type").at("enum").size() != 1U ||
          ownProperties.at("type").at("enum").front() != entry.resourceType) {
        continue;
      }

      ResourceForm form;
      form.resourceType = entry.resourceType;
      form.title = schema.value("title", entry.resourceType);
      form.applicationOwned = !builtInTypes.contains(entry.resourceType);
      auto required = own.value("required", std::vector<std::string>{});
      form.requiresDefinition = std::find(required.begin(), required.end(),
                                          "Definitions") != required.end();
      form.composite = form.requiresDefinition;

      for (auto const& [propertyName, property] : ownProperties.items()) {
        if (!property.is_object() || property.value(std::string(widgetKeyword),
                                                    std::string{}) != "file") {
          continue;
        }
        if (!form.fileProperty.empty()) {
          throw std::runtime_error(
              "the supported form subset permits one file property");
        }
        form.fileProperty = propertyName;
        form.fileKind = property.at("x-willpower-file-kind").get<std::string>();
        form.fileExtensions = property.at("x-willpower-file-extensions")
                                  .get<std::vector<std::string>>();
      }
      collectAnnotatedDependencies(schema, form.requiredDependencies);

      if (builtInTypes.contains(entry.resourceType)) {
        auto const advanced = entry.resourceType == "ImageSet" ||
                              entry.resourceType == "AnimationSet" ||
                              entry.resourceType == "Program" ||
                              entry.resourceType == "Material";
        form.composite = advanced;
        form.requiresDefinition = advanced;
        if (advanced && entry.resourceType == "ImageSet") {
          static_cast<void>(schema.at("definitions").at("image"));
          static_cast<void>(schema.at("definitions").at("imageSet"));
        } else if (advanced && entry.resourceType == "AnimationSet") {
          static_cast<void>(schema.at("definitions").at("animation"));
          static_cast<void>(schema.at("definitions").at("explicitFrame"));
          static_cast<void>(
              schema.at("definitions").at("imageSetFrameOverride"));
        } else if (advanced && entry.resourceType == "Program") {
          static_cast<void>(schema.at("definitions").at("meshSpecification"));
          static_cast<void>(schema.at("definitions").at("buffer"));
          static_cast<void>(schema.at("definitions").at("channel"));
        } else if (advanced) {
          static_cast<void>(schema.at("definitions").at("texture"));
        }
      } else if (form.requiresDefinition &&
                 !supportedDefaultDefinition(catalog, entry)) {
        continue;  // Valid for validation, but deliberately read-only for
                   // authoring.
      }

      if (schema.at("definitions").contains("option")) {
        try {
          for (auto const& branch :
               schema.at("definitions").at("option").at("oneOf")) {
            ResourceOptionForm option;
            auto const& properties = branch.at("properties");
            option.name =
                properties.at("name").at("enum").at(0).get<std::string>();
            auto const& value = properties.at("value");
            if (value.contains("enum")) {
              option.values = value.at("enum").get<std::vector<std::string>>();
            } else if (value.value("$ref", std::string{})
                           .ends_with("/definitions/boolean")) {
              option.boolean = true;
            }
            form.options.push_back(std::move(option));
          }
        } catch (...) {
          form.options.clear();
        }
      }
      result.push_back(std::move(form));
    } catch (...) {
      // Catalog validity and form support are separate. Valid Draft 7 shapes
      // outside the documented subset remain visible but read-only.
    }
  }
  return result;
}

struct FileAnnotation {
  std::string property;
  std::vector<std::string> extensions;
};

void collectFileAnnotations(Json const& value,
                            std::vector<FileAnnotation>& result) {
  if (value.is_object()) {
    if (value.contains("properties") && value.at("properties").is_object()) {
      for (auto const& [name, property] : value.at("properties").items()) {
        if (!property.is_object() ||
            property.value(std::string(widgetKeyword), std::string{}) != "file") {
          continue;
        }
        FileAnnotation annotation;
        annotation.property = name;
        annotation.extensions =
            property.at("x-willpower-file-extensions")
                .get<std::vector<std::string>>();
        auto found = std::find_if(
            result.begin(), result.end(), [&](auto const& existing) {
              return existing.property == annotation.property;
            });
        if (found == result.end()) result.push_back(std::move(annotation));
      }
    }
    for (auto const& [name, child] : value.items()) {
      static_cast<void>(name);
      collectFileAnnotations(child, result);
    }
  } else if (value.is_array()) {
    for (auto const& child : value) collectFileAnnotations(child, result);
  }
}

std::vector<FileAnnotation> fileAnnotations(
    ResourceSchemaCatalogSnapshot const& catalog,
    std::string const& resourceType) {
  std::vector<FileAnnotation> result;
  auto const* schema = catalog.findExact({resourceType, {}});
  if (schema) collectFileAnnotations(Json::parse(schema->contents), result);
  return result;
}

std::vector<ResourceDependencyForm> dependencyAnnotations(
    ResourceSchemaCatalogSnapshot const& catalog,
    std::string const& resourceType) {
  std::vector<ResourceDependencyForm> result;
  auto const* schema = catalog.findExact({resourceType, {}});
  if (schema)
    collectAnnotatedDependencies(Json::parse(schema->contents), result);
  return result;
}

struct SemanticResource {
  YAML::Node node;
  ResourceIdentity identity;
  ResourceIdentity owner;
  std::string resourceType;
  std::string path;
  std::string navigationPath;
  std::string navigationName;
  bool inlineResource = false;
  std::size_t dependencyIndex = 0;
};

struct SemanticNamespace {
  YAML::Node node;
  std::string name;
  std::string path;
};

void collectSemanticDocument(
    YAML::Node const& root, std::vector<SemanticNamespace>& namespaces,
    std::vector<SemanticResource>& declarations) {
  auto appendResources = [&](std::string const& resourceNamespace,
                             YAML::Node const& collection,
                             std::string const& collectionPath) {
    auto resources = collectionItems(collection);
    for (std::size_t index = 0; index < resources.size(); ++index) {
      auto path = collectionPath + "/" + std::to_string(index);
      ResourceIdentity owner{resourceNamespace, resourceIdentity(resources[index])};
      declarations.push_back({resources[index], owner, {},
                              scalar(resources[index], "type"), path, path,
                              owner.name, false});
      auto dependencies = standardDependencyItems(resources[index]);
      for (std::size_t dependencyIndex = 0;
           dependencyIndex < dependencies.size(); ++dependencyIndex) {
        auto const& dependency = dependencies[dependencyIndex];
        if (!scalar(dependency, "ref").empty() ||
            scalar(dependency, "type").empty()) {
          continue;
        }
        auto dependencyPath =
            path + "/DependentResources/DependentResource/" +
            std::to_string(dependencyIndex);
        declarations.push_back(
            {dependency,
             {resourceNamespace, resourceIdentity(dependency)}, owner,
             scalar(dependency, "type"), std::move(dependencyPath), path,
             owner.name, true, dependencyIndex});
      }
    }
  };

  auto resourcesRoot = root["Resources"];
  appendResources({}, resourcesRoot["Resource"], "/Resources/Resource");
  auto namespaceItems = collectionItems(resourcesRoot["Namespace"]);
  for (std::size_t index = 0; index < namespaceItems.size(); ++index) {
    auto path = "/Resources/Namespace/" + std::to_string(index);
    auto name = scalar(namespaceItems[index], "name");
    namespaces.push_back({namespaceItems[index], name, path});
    appendResources(name, namespaceItems[index]["Resource"],
                    path + "/Resource");
  }
}

bool sameExtension(std::string left, std::string right) {
  auto lower = [](std::string& value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
  };
  lower(left);
  lower(right);
  return left == right;
}

std::vector<SemanticDiagnostic> semanticDiagnosticsFor(
    YAML::Node const& root, ResourceSchemaCatalogSnapshot const& catalog,
    fs::path const& baseDirectory) {
  std::vector<SemanticDiagnostic> result;
  std::vector<SemanticNamespace> namespaces;
  std::vector<SemanticResource> declarations;
  collectSemanticDocument(root, namespaces, declarations);

  auto add = [&](SemanticDiagnosticKind kind, SemanticResource const& resource,
                 std::string instancePath, std::string message,
                 std::string subject,
                 SemanticDiagnosticSeverity severity =
                     SemanticDiagnosticSeverity::error) {
    result.push_back({severity,
                      kind,
                      resource.identity.resourceNamespace,
                      resource.navigationName,
                      resource.navigationPath,
                      std::move(instancePath),
                      std::move(message),
                      std::move(subject)});
    result.back().inlineResource = resource.inlineResource;
    result.back().dependencyIndex = resource.dependencyIndex;
  };

  std::map<std::string, std::size_t> namespaceCounts;
  for (auto const& item : namespaces) ++namespaceCounts[item.name];
  for (auto const& item : namespaces) {
    SemanticResource navigation;
    navigation.identity.resourceNamespace = item.name;
    navigation.path = item.path;
    navigation.navigationPath = item.path;
    if (!validNamespaceName(item.name)) {
      add(SemanticDiagnosticKind::invalidName, navigation, item.path + "/name",
          "Namespace name is invalid; names cannot be empty or contain '/'.",
          "namespace:" + item.name);
    }
    if (namespaceCounts[item.name] > 1U) {
      add(SemanticDiagnosticKind::duplicateName, navigation,
          item.path + "/name", "Namespace name '" + item.name +
                                   "' is duplicated.",
          "namespace:" + item.name);
    }
  }

  std::map<ResourceIdentity, std::size_t> identityCounts;
  for (auto const& resource : declarations) ++identityCounts[resource.identity];
  for (auto const& resource : declarations) {
    auto const namePath = resource.path + "/name";
    if (!validResourceName(resource.identity.name)) {
      add(SemanticDiagnosticKind::invalidName, resource, namePath,
          "Resource name '" + resource.identity.name +
              "' is invalid; names cannot be empty or contain '/'.",
          "resource:" + qualifiedIdentity(resource.identity));
    }
    if (identityCounts[resource.identity] > 1U) {
      add(SemanticDiagnosticKind::duplicateName, resource, namePath,
          "Resource identity '" + qualifiedIdentity(resource.identity) +
              "' is duplicated.",
          "resource:" + qualifiedIdentity(resource.identity));
    }

    auto duplicateProperties = [&](YAML::Node const& collection,
                                   char const* property,
                                   std::string const& collectionPath,
                                   std::string const& label,
                                   bool includeEmpty) {
      auto items = collectionItems(collection);
      std::map<std::string, std::size_t> counts;
      for (auto const& item : items) {
        auto value = scalar(item, property);
        if (includeEmpty || !value.empty()) ++counts[value];
      }
      for (std::size_t index = 0; index < items.size(); ++index) {
        auto value = scalar(items[index], property);
        if ((!includeEmpty && value.empty()) || counts[value] < 2U) continue;
        add(SemanticDiagnosticKind::duplicateName, resource,
            resource.path + collectionPath + "/" + std::to_string(index) +
                "/" + property,
            label + " '" + (value.empty() ? std::string("<default>") : value) +
                "' is duplicated.",
            label + ":" + value);
      }
    };
    duplicateProperties(resource.node["Option"], "name", "/Option",
                        "Option name", false);
    auto dependentResources = resource.node["DependentResources"];
    if (dependentResources && dependentResources.IsMap()) {
      duplicateProperties(dependentResources["DependentResource"], "id",
                          "/DependentResources/DependentResource",
                          "Dependency ID", false);
    }
    auto definitions = resource.node["Definitions"];
    if (definitions && definitions.IsMap()) {
      duplicateProperties(definitions["Definition"], "factory",
                          "/Definitions/Definition", "Definition factory", true);
    }

    auto duplicateNestedNames = [&](std::vector<std::pair<YAML::Node, std::string>>
                                        const& namedItems,
                                    std::string const& label) {
      std::map<std::string, std::size_t> counts;
      for (auto const& [item, path] : namedItems) {
        static_cast<void>(path);
        ++counts[scalar(item, "name")];
      }
      for (auto const& [item, path] : namedItems) {
        auto name = scalar(item, "name");
        if (counts[name] < 2U) continue;
        add(SemanticDiagnosticKind::duplicateName, resource, path + "/name",
            label + " name '" + name + "' is duplicated.",
            label + ":" + name);
      }
    };
    if (resource.resourceType == "ImageSet") {
      for (std::size_t definitionIndex = 0;
           definitionIndex < collectionItems(
                                 resource.node["Definitions"]["Definition"])
                                 .size();
           ++definitionIndex) {
        auto definitionItems =
            collectionItems(resource.node["Definitions"]["Definition"]);
        auto images = definitionItems[definitionIndex]["Images"];
        std::vector<std::pair<YAML::Node, std::string>> namedItems;
        for (auto const* key : {"Image", "ImageSet"}) {
          auto items = collectionItems(images[key]);
          for (std::size_t index = 0; index < items.size(); ++index) {
            namedItems.emplace_back(
                items[index], resource.path + "/Definitions/Definition/" +
                                  std::to_string(definitionIndex) + "/Images/" +
                                  key + "/" + std::to_string(index));
          }
        }
        duplicateNestedNames(namedItems, "Image");
      }
    } else if (resource.resourceType == "AnimationSet") {
      auto definitionItems =
          collectionItems(resource.node["Definitions"]["Definition"]);
      for (std::size_t definitionIndex = 0;
           definitionIndex < definitionItems.size(); ++definitionIndex) {
        auto animations = collectionItems(
            definitionItems[definitionIndex]["Animations"]["Animation"]);
        std::vector<std::pair<YAML::Node, std::string>> namedItems;
        for (std::size_t index = 0; index < animations.size(); ++index) {
          namedItems.emplace_back(
              animations[index],
              resource.path + "/Definitions/Definition/" +
                  std::to_string(definitionIndex) +
                  "/Animations/Animation/" + std::to_string(index));
        }
        duplicateNestedNames(namedItems, "Animation");
      }
    }

    if (catalog.findExact({resource.resourceType, {}}) == nullptr) {
      add(SemanticDiagnosticKind::unvalidatedData, resource,
          resource.path + "/type",
          "No Resource Type schema is loaded; unannotated strings are preserved "
          "without guessed file or reference semantics.",
          "unknown:" + resource.resourceType,
          SemanticDiagnosticSeverity::warning);
    }

    for (auto const& annotation : fileAnnotations(catalog, resource.resourceType)) {
      auto valueNode = resource.node[annotation.property];
      if (!valueNode || !valueNode.IsScalar()) continue;
      auto value = valueNode.as<std::string>();
      auto stored = fs::path(value);
      auto target = stored.is_absolute() ? stored : baseDirectory / stored;
      std::error_code error;
      auto resolved = fs::weakly_canonical(target, error);
      if (error) resolved = fs::absolute(target, error).lexically_normal();
      auto instancePath = resource.path + "/" + annotation.property;
      if (stored.is_absolute() || error || !containedBy(baseDirectory, resolved)) {
        add(SemanticDiagnosticKind::pathContainment, resource, instancePath,
            "Annotated file target '" + value +
                "' is outside the canonical base directory or cannot be "
                "represented safely.",
            "file:" + value);
        continue;
      }
      error.clear();
      if (!fs::is_regular_file(resolved, error)) {
        add(SemanticDiagnosticKind::missingFile, resource, instancePath,
            "Annotated file target '" + value +
                "' does not exist as a regular file.",
            "file:" + value);
        continue;
      }
      auto extension = resolved.extension().string();
      if (!extension.empty() && extension.front() == '.') extension.erase(0, 1U);
      if (std::none_of(annotation.extensions.begin(), annotation.extensions.end(),
                       [&](auto const& allowed) {
                         return sameExtension(extension, allowed);
                       })) {
        add(SemanticDiagnosticKind::invalidFileTarget, resource, instancePath,
            "Annotated file target '" + value +
                "' does not match its schema file extensions.",
            "file-extension:" + value);
      }
    }
  }

  struct ReferenceEdge {
    ResourceIdentity owner;
    ResourceIdentity target;
    SemanticResource const* resource = nullptr;
    std::size_t dependencyIndex = 0;
    std::string reference;
  };
  std::vector<ReferenceEdge> edges;
  std::map<ResourceIdentity, std::vector<ResourceIdentity>> graph;
  for (auto const& resource : declarations) {
    if (resource.inlineResource) {
      if (identityCounts[resource.owner] == 1U &&
          identityCounts[resource.identity] == 1U)
        graph[resource.owner].push_back(resource.identity);
      continue;
    }
    auto dependencies = standardDependencyItems(resource.node);
    auto annotations = dependencyAnnotations(catalog, resource.resourceType);
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
      auto reference = scalar(dependencies[index], "ref");
      if (reference.empty()) continue;
      auto target = parseReference(reference, resource.identity.resourceNamespace);
      auto matches = identityCounts[target];
      auto path = resource.path + "/DependentResources/DependentResource/" +
                  std::to_string(index) + "/ref";
      if (matches != 1U) {
        add(SemanticDiagnosticKind::unresolvedReference, resource, path,
            "Known Resource reference '" + reference + "' is " +
                (matches == 0U ? "unresolved." : "ambiguous."),
            "reference:" + scalar(dependencies[index], "id") + ":" +
                qualifiedIdentity(target));
        continue;
      }
      auto declaration = std::find_if(
          declarations.begin(), declarations.end(), [&](auto const& candidate) {
            return candidate.identity == target;
          });
      auto annotation = std::find_if(
          annotations.begin(), annotations.end(), [&](auto const& candidate) {
            return candidate.id == scalar(dependencies[index], "id");
          });
      auto allowedTypes =
          standardAllowedTypes(resource.resourceType,
                               scalar(dependencies[index], "id"));
      if (annotation != annotations.end())
        allowedTypes = annotation->allowedResourceTypes;
      if (!allowedTypes.empty() &&
          !allowedResourceType(allowedTypes, declaration->resourceType)) {
        add(SemanticDiagnosticKind::referenceTypeMismatch, resource, path,
            "Typed Resource reference '" + reference + "' targets type '" +
                declaration->resourceType + "'; expected " +
                allowedTypes.front() + ".",
            "reference-type:" + scalar(dependencies[index], "id") + ":" +
                qualifiedIdentity(target));
      }
      graph[resource.identity].push_back(target);
      edges.push_back({resource.identity, target, &resource, index,
                       std::move(reference)});
    }
  }

  auto reaches = [&](ResourceIdentity start, ResourceIdentity target) {
    std::set<ResourceIdentity> visited;
    std::vector<ResourceIdentity> pending{std::move(start)};
    while (!pending.empty()) {
      auto current = std::move(pending.back());
      pending.pop_back();
      if (current == target) return true;
      if (!visited.insert(current).second) continue;
      auto found = graph.find(current);
      if (found != graph.end())
        pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    return false;
  };
  for (auto const& edge : edges) {
    if (!reaches(edge.target, edge.owner)) continue;
    auto path = edge.resource->path +
                "/DependentResources/DependentResource/" +
                std::to_string(edge.dependencyIndex) + "/ref";
    add(SemanticDiagnosticKind::dependencyCycle, *edge.resource, path,
        "Resource reference '" + edge.reference +
            "' participates in a dependency cycle.",
        "cycle");
  }
  return result;
}

std::string semanticKey(SemanticDiagnostic const& diagnostic) {
  return std::to_string(static_cast<int>(diagnostic.kind)) + "\n" +
         diagnostic.subject;
}

bool hasSemanticErrors(std::vector<SemanticDiagnostic> const& diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [](auto const& diagnostic) {
                       return diagnostic.severity ==
                              SemanticDiagnosticSeverity::error;
                     });
}

class ReplaceYamlCommand final : public mpp::app::EditorCommand {
 public:
  using Apply = std::function<bool(std::string const&)>;

  ReplaceYamlCommand(std::string name, std::string mergeKey, Apply apply,
                     std::string before, std::string after)
      : mName(std::move(name)),
        mMergeKey(std::move(mergeKey)),
        mApply(std::move(apply)),
        mBefore(std::move(before)),
        mAfter(std::move(after)) {}

  std::string const& name() const override { return mName; }
  void execute() override {
    if (!mApply(mAfter)) throw std::runtime_error("Could not apply validated Resource edit.");
  }
  void undo() override {
    if (!mApply(mBefore)) throw std::runtime_error("Could not undo validated Resource edit.");
  }
  bool merge(mpp::app::EditorCommand const& other) override {
    auto const* replacement = dynamic_cast<ReplaceYamlCommand const*>(&other);
    if (!replacement || mMergeKey.empty() || replacement->mMergeKey != mMergeKey) {
      return false;
    }
    mAfter = replacement->mAfter;
    return true;
  }

 private:
  std::string mName;
  std::string mMergeKey;
  Apply mApply;
  std::string mBefore;
  std::string mAfter;
};

}  // namespace

bool hasYamlExtension(fs::path const& path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".yaml" || extension == ".yml";
}

EditorSchemaConfiguration readEditorSchemaConfiguration(
    fs::path const& iniPath) {
  std::ifstream input(iniPath);
  if (!input) {
    throw std::runtime_error("Could not open required deployment INI '" +
                             displayPath(iniPath) + "'.");
  }
  auto trimValue = [](std::string value) {
    auto const first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string{};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1U);
  };
  std::error_code error;
  auto absoluteIni = fs::absolute(iniPath, error).lexically_normal();
  if (error) {
    throw std::runtime_error("Could not resolve deployment INI path '" +
                             displayPath(iniPath) + "': " + error.message());
  }
  auto resolveBundle = [&](std::string const& value,
                           std::string const& location) {
    if (value.empty())
      throw std::runtime_error(location + ": bundle path cannot be empty.");
    fs::path path(value);
    if (path.is_relative()) path = absoluteIni.parent_path() / path;
    return path.lexically_normal();
  };

  EditorSchemaConfiguration result;
  result.iniPath = absoluteIni;
  bool foundSection = false;
  bool foundVersion = false;
  bool foundBase = false;
  std::string section;
  std::string line;
  for (std::size_t lineNumber = 1; std::getline(input, line); ++lineNumber) {
    auto content = trimValue(line);
    if (content.empty() || content.front() == ';' || content.front() == '#')
      continue;
    if (content.front() == '[' && content.back() == ']') {
      section = trimValue(content.substr(1U, content.size() - 2U));
      foundSection |= section == "ResourceManifestEditor";
      continue;
    }
    if (section != "ResourceManifestEditor") continue;
    auto const separator = content.find('=');
    auto const location =
        displayPath(absoluteIni) + ":" + std::to_string(lineNumber);
    if (separator == std::string::npos) {
      throw std::runtime_error(location + ": expected key=value.");
    }
    auto const key = trimValue(content.substr(0U, separator));
    auto const value = trimValue(content.substr(separator + 1U));
    if (key == "formatVersion") {
      if (foundVersion) {
        throw std::runtime_error(location +
                                 ": duplicate setting 'formatVersion'.");
      }
      if (value != "1") {
        throw std::runtime_error(location +
                                 ": formatVersion must be exactly 1.");
      }
      foundVersion = true;
    } else if (key == "baseBundle") {
      if (foundBase) {
        throw std::runtime_error(location +
                                 ": duplicate setting 'baseBundle'.");
      }
      result.baseBundle = resolveBundle(value, location);
      foundBase = true;
    } else if (key == "bundle") {
      result.extensionBundles.push_back(resolveBundle(value, location));
    } else {
      throw std::runtime_error(location +
                               ": unknown Resource Manifest Editor setting '" +
                               key + "'.");
    }
  }
  if (!input.eof() && input.fail()) {
    throw std::runtime_error("Could not read deployment INI '" +
                             displayPath(absoluteIni) + "'.");
  }
  if (!foundSection || !foundVersion) {
    throw std::runtime_error(
        "Deployment INI must define [ResourceManifestEditor] formatVersion=1.");
  }
  return result;
}

ResourceSchemaCatalogSnapshot loadEditorSchemaCatalog(
    EditorSchemaConfiguration const& configuration) {
  auto catalog = configuration.baseBundle ? ResourceSchemaCatalog{}
                                          : ResourceSchemaCatalog::builtIn();
  std::vector<ResourceSchemaBundle> bundles;
  if (configuration.baseBundle) {
    bundles.push_back(
        ResourceSchemaCatalog::readBundle(*configuration.baseBundle));
  }
  for (auto const& path : configuration.extensionBundles) {
    bundles.push_back(ResourceSchemaCatalog::readBundle(path));
  }

  std::set<std::pair<std::string, std::string>> lookupKeys;
  if (!configuration.baseBundle) {
    for (auto const& entry : catalog.snapshot().entries()) {
      if (entry.kind == ResourceSchemaKind::resourceType) {
        lookupKeys.emplace(entry.resourceType, entry.factoryType);
      }
    }
  }
  for (std::size_t bundleIndex = 0; bundleIndex < bundles.size();
       ++bundleIndex) {
    // addBundles performs authoritative metadata validation. This preflight is
    // intentionally stricter for editor deployment: even byte-identical lookup
    // registrations are configuration errors rather than silently idempotent.
    auto metadata = Json::parse(bundles[bundleIndex].catalogJson);
    if (metadata.is_object() && metadata.contains("schemas") &&
        metadata.at("schemas").is_array()) {
      for (auto const& schema : metadata.at("schemas")) {
        if (!schema.is_object() ||
            schema.value("kind", std::string{}) != "resourceType" ||
            !schema.contains("resourceType") ||
            !schema.at("resourceType").is_string()) {
          continue;
        }
        auto resourceType = schema.at("resourceType").get<std::string>();
        std::string factoryType;
        if (schema.contains("factoryType") &&
            schema.at("factoryType").is_string()) {
          factoryType = schema.at("factoryType").get<std::string>();
        }
        if (!lookupKeys.emplace(resourceType, factoryType).second) {
          throw std::runtime_error(
              "Duplicate editor Resource Schema lookup key ('" + resourceType +
              "', '" +
              (factoryType.empty() ? std::string("<default>") : factoryType) +
              "') in configured bundle " + std::to_string(bundleIndex + 1U) +
              ".");
        }
      }
    }
  }
  if (!bundles.empty()) catalog.addBundles(bundles);
  auto snapshot = catalog.snapshot();
  validateEditorCatalog(snapshot);
  return snapshot;
}

ManifestWorkspace::ManifestWorkspace()
    : ManifestWorkspace(ResourceSchemaCatalog::builtIn().snapshot()) {}

ManifestWorkspace::ManifestWorkspace(ResourceSchemaCatalogSnapshot catalog)
    : mCatalog(std::move(catalog)) {
  validateEditorCatalog(mCatalog);
  mResourceForms = loadResourceForms(mCatalog);
}

bool ManifestWorkspace::createNew(fs::path const& baseDirectory) {
  fs::path canonicalBase;
  std::string failure;
  if (!accessibleDirectory(baseDirectory, canonicalBase, failure)) {
    setFailure(std::move(failure));
    return false;
  }

  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::parse("Resources: {}\n", "Untitled Resource Manifest"));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Could not create an empty Resource Manifest: " +
               validationMessage(validation));
    return false;
  }

  mDocument = std::move(candidate);
  mPath.clear();
  mBaseDirectory = std::move(canonicalBase);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  mNamespaceDraft.reset();
  mDraft.reset();
  mCommands.clear();
  mUnsavedDocument = true;
  refreshSemanticDiagnostics();
  return true;
}

bool ManifestWorkspace::open(fs::path const& manifestPath) {
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }

  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::load(manifestPath));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = validation.diagnostics;
    setFailure("Could not open Resource Manifest '" + displayPath(manifestPath) +
               "': " + validationMessage(validation));
    return false;
  }

  std::error_code error;
  auto canonicalPath = fs::canonical(manifestPath, error);
  if (error) {
    setFailure("Could not canonicalize Resource Manifest path '" +
               displayPath(manifestPath) + "': " + error.message());
    return false;
  }
  fs::path canonicalBase;
  std::string failure;
  if (!accessibleDirectory(canonicalPath.parent_path(), canonicalBase, failure)) {
    setFailure(std::move(failure));
    return false;
  }

  mDocument = std::move(candidate);
  mPath = std::move(canonicalPath);
  mBaseDirectory = std::move(canonicalBase);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  mNamespaceDraft.reset();
  mDraft.reset();
  mCommands.clear();
  mCommands.markSavePoint();
  mUnsavedDocument = false;
  refreshSemanticDiagnostics();
  return true;
}

bool ManifestWorkspace::save() {
  if (mPath.empty()) {
    setFailure("Save As is required for an unsaved Resource Manifest.");
    return false;
  }
  return saveTo(mPath);
}

bool ManifestWorkspace::saveAs(fs::path const& manifestPath) {
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }
  return saveTo(manifestPath);
}

bool ManifestWorkspace::changeBaseDirectory(fs::path const& baseDirectory) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  fs::path canonicalBase;
  std::string failure;
  if (!accessibleDirectory(baseDirectory, canonicalBase, failure)) {
    setFailure(std::move(failure));
    return false;
  }

  auto root = YAML::Load(canonicalYaml());
  bool changedFile = false;
  std::string migrationFailure;
  auto migrateCollection = [&](YAML::Node owner) {
    auto resources = collectionItems(owner["Resource"]);
    for (auto& resource : resources) {
      auto migrate = [&](YAML::Node node) {
        for (auto const& annotation :
             fileAnnotations(mCatalog, scalar(node, "type"))) {
          auto valueNode = node[annotation.property];
          if (!valueNode || !valueNode.IsScalar()) continue;
          auto value = valueNode.as<std::string>();
          auto stored = fs::path(value);
          auto target = stored.is_absolute() ? stored : mBaseDirectory / stored;
          std::error_code error;
          auto absoluteTarget = fs::canonical(target, error);
          if (error || !fs::is_regular_file(absoluteTarget, error)) {
            migrationFailure = "Annotated file target '" + value +
                               "' must exist before changing the base directory.";
            return false;
          }
          if (!containedBy(canonicalBase, absoluteTarget)) {
            migrationFailure = "Changing the base directory would place annotated "
                               "file target '" + value + "' outside the new base.";
            return false;
          }
          auto relative = absoluteTarget.lexically_relative(canonicalBase);
          if (relative.empty() || relative.is_absolute() ||
              std::any_of(relative.begin(), relative.end(),
                          [](fs::path const& part) { return part == ".."; })) {
            migrationFailure = "Annotated file target '" + value +
                               "' is not representable relative to the new base.";
            return false;
          }
          auto portable = displayPath(relative);
          changedFile |= portable != value;
          node[annotation.property] = std::move(portable);
        }
        return true;
      };
      if (!migrate(resource)) return false;
      auto dependencies = standardDependencyItems(resource);
      for (auto& dependency : dependencies) {
        if (scalar(dependency, "ref").empty() &&
            !scalar(dependency, "type").empty() && !migrate(dependency)) {
          return false;
        }
      }
      if (!dependencies.empty()) {
        setCollection(resource["DependentResources"], "DependentResource",
                      dependencies);
      }
    }
    setCollection(owner, "Resource", resources);
    return true;
  };

  auto resourcesRoot = root["Resources"];
  if (!migrateCollection(resourcesRoot)) {
    setFailure(std::move(migrationFailure));
    return false;
  }
  auto namespaces = collectionItems(resourcesRoot["Namespace"]);
  for (auto& item : namespaces) {
    if (!migrateCollection(item)) {
      setFailure(std::move(migrationFailure));
      return false;
    }
  }
  setCollection(resourcesRoot, "Namespace", namespaces);
  root["Resources"] = resourcesRoot;

  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::parse(emitYaml(root), "Base directory migration"));
  auto structural = candidate->validate(mCatalog);
  if (!structural.valid()) {
    setFailure("Changing the base directory produced an invalid Resource Manifest: " +
               validationMessage(structural));
    return false;
  }
  auto semantic = semanticDiagnosticsFor(
      YAML::Load(candidate->serializeCanonical()), mCatalog, canonicalBase);
  auto fileError = std::find_if(
      semantic.begin(), semantic.end(), [](auto const& item) {
        return item.severity == SemanticDiagnosticSeverity::error &&
               (item.kind == SemanticDiagnosticKind::missingFile ||
                item.kind == SemanticDiagnosticKind::pathContainment ||
                item.kind == SemanticDiagnosticKind::invalidFileTarget);
      });
  if (fileError != semantic.end()) {
    setFailure("Changing the base directory was rejected: " +
               fileError->message);
    return false;
  }

  bool const changedBase = canonicalBase != mBaseDirectory;
  mDocument = std::move(candidate);
  mBaseDirectory = std::move(canonicalBase);
  mSemanticDiagnostics = std::move(semantic);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  mNamespaceDraft.reset();
  mDraft.reset();
  if (changedBase || changedFile) {
    mCommands.clear();
    mUnsavedDocument = true;
  }
  return true;
}

bool ManifestWorkspace::reloadSchemas(fs::path const& iniPath) {
  try {
    auto candidateCatalog =
        loadEditorSchemaCatalog(readEditorSchemaConfiguration(iniPath));
    auto candidateForms = loadResourceForms(candidateCatalog);
    if (mDocument) {
      auto validation = mDocument->validate(candidateCatalog);
      if (!validation.valid()) {
        mStructuralDiagnostics = std::move(validation.diagnostics);
        setFailure("Schema reload was rejected because the open Resource Manifest "
                   "is invalid under the candidate catalog: " +
                   validationMessage(validation));
        return false;
      }
    }

    auto const wasDirty = dirty();
    mCatalog = std::move(candidateCatalog);
    mResourceForms = std::move(candidateForms);
    mNamespaceDraft.reset();
    mDraft.reset();
    mCommands.clear();
    if (wasDirty) mUnsavedDocument = true;
    mStructuralDiagnostics.clear();
    mOperationDiagnostic.clear();
    refreshSemanticDiagnostics();
    return true;
  } catch (std::exception const& error) {
    setFailure("Schema reload failed; the previous catalog remains active: " +
               std::string(error.what()));
    return false;
  }
}

bool ManifestWorkspace::saveTo(fs::path const& manifestPath) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }

  auto validation = mDocument->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Resource Manifest is not structurally valid and cannot be saved.");
    return false;
  }
  refreshSemanticDiagnostics();
  if (hasSemanticErrors(mSemanticDiagnostics)) {
    setFailure("Resource Manifest has semantic errors and cannot be saved.");
    return false;
  }

  try {
    auto canonical = mDocument->serializeCanonical();
    writeAtomically(manifestPath, canonical);

    auto saved = std::make_unique<ResourceManifestDocument>(
        ResourceManifestDocument::load(manifestPath));
    auto savedValidation = saved->validate(mCatalog);
    if (!savedValidation.valid() || saved->serializeCanonical() != canonical) {
      throw std::runtime_error(
          "Saved Resource Manifest failed canonical round-trip validation.");
    }
    std::error_code error;
    auto canonicalPath = fs::canonical(manifestPath, error);
    if (error) canonicalPath = fs::absolute(manifestPath).lexically_normal();
    mDocument = std::move(saved);
    mPath = std::move(canonicalPath);
    mStructuralDiagnostics.clear();
    mOperationDiagnostic.clear();
    refreshSemanticDiagnostics();
    mCommands.markSavePoint();
    mUnsavedDocument = false;
    return true;
  } catch (std::exception const& exception) {
    setFailure("Could not save Resource Manifest '" + displayPath(manifestPath) +
               "': " + exception.what());
    return false;
  }
}

void ManifestWorkspace::reportFailure(std::string message) {
  setFailure(std::move(message));
}

bool ManifestWorkspace::hasDocument() const noexcept { return mDocument != nullptr; }
bool ManifestWorkspace::hasPath() const noexcept { return !mPath.empty(); }
bool ManifestWorkspace::dirty() const noexcept {
  return mDocument && (mUnsavedDocument || mCommands.dirty());
}
bool ManifestWorkspace::canSave() const {
  // Every published workspace document has already passed structural
  // validation; rejected previews never replace it. Save revalidates before I/O.
  return mDocument && !hasSemanticErrors(mSemanticDiagnostics);
}
bool ManifestWorkspace::canSaveAs() const { return canSave(); }
fs::path const& ManifestWorkspace::path() const noexcept { return mPath; }
fs::path const& ManifestWorkspace::baseDirectory() const noexcept {
  return mBaseDirectory;
}
std::string const& ManifestWorkspace::operationDiagnostic() const noexcept {
  return mOperationDiagnostic;
}
std::vector<ResourceManifestDiagnostic> const&
ManifestWorkspace::structuralDiagnostics() const noexcept {
  return mStructuralDiagnostics;
}
std::vector<SemanticDiagnostic> const&
ManifestWorkspace::semanticDiagnostics() const noexcept {
  return mSemanticDiagnostics;
}
std::string ManifestWorkspace::canonicalYaml() const {
  return mDocument ? mDocument->serializeCanonical() : std::string{};
}

std::vector<ResourceForm> const& ManifestWorkspace::resourceForms() const noexcept {
  return mResourceForms;
}

ResourceForm const* ManifestWorkspace::resourceForm(
    std::string const& resourceType) const noexcept {
  auto found = std::find_if(mResourceForms.begin(), mResourceForms.end(),
                            [&](ResourceForm const& form) {
                              return form.resourceType == resourceType;
                            });
  return found == mResourceForms.end() ? nullptr : &*found;
}

std::vector<NamespaceSummary> ManifestWorkspace::namespaces() const {
  std::vector<NamespaceSummary> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto resourcesRoot = root["Resources"];
  result.push_back(NamespaceSummary{{}, "/Resources",
                                    collectionItems(resourcesRoot["Resource"]).size(),
                                    true});
  auto namespaceItems = collectionItems(resourcesRoot["Namespace"]);
  for (std::size_t index = 0; index < namespaceItems.size(); ++index) {
    auto const& item = namespaceItems[index];
    result.push_back(NamespaceSummary{
        scalar(item, "name"),
        "/Resources/Namespace/" + std::to_string(index),
        collectionItems(item["Resource"]).size()});
  }
  if (mNamespaceDraft) {
    result.push_back(
        NamespaceSummary{mNamespaceDraft->name, {}, 0, false, true});
  }
  return result;
}

std::vector<ResourceSummary> ManifestWorkspace::resources() const {
  std::vector<ResourceSummary> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto append = [&](std::string const& resourceNamespace,
                    YAML::Node const& collection,
                    std::string const& collectionPath) {
    auto items = collectionItems(collection);
    for (std::size_t index = 0; index < items.size(); ++index) {
      auto const& resource = items[index];
      ResourceSummary summary;
      summary.resourceNamespace = resourceNamespace;
      summary.instancePath = collectionPath + "/" + std::to_string(index);
      summary.resourceType = scalar(resource, "type");
      summary.name = scalar(resource, "name");
      summary.explicitName = !summary.name.empty();
      summary.location = scalar(resource, "location");
      for (auto const& option : collectionItems(resource["Option"])) {
        summary.options.emplace_back(scalar(option, "name"),
                                     scalar(option, "value"));
      }
      if (summary.name.empty()) summary.name = summary.location;
      summary.editable = resourceForm(summary.resourceType) != nullptr;
      summary.unknownType =
          mCatalog.findExact({summary.resourceType, {}}) == nullptr;
      if (summary.unknownType) {
        summary.limitationWarning =
            "No editing schema is loaded for this Resource Type. Its payload is "
            "preserved, unknown Definitions are read-only, and unannotated "
            "references cannot be checked or rewritten.";
      } else if (!summary.editable) {
        summary.limitationWarning =
            "This catalogued schema uses an unsupported authoring shape. Its "
            "payload and Definitions are preserved read-only.";
      }
      result.push_back(std::move(summary));
    }
  };
  auto resourcesRoot = root["Resources"];
  append({}, resourcesRoot["Resource"], "/Resources/Resource");
  auto namespaceItems = collectionItems(resourcesRoot["Namespace"]);
  for (std::size_t index = 0; index < namespaceItems.size(); ++index) {
    auto const& item = namespaceItems[index];
    append(scalar(item, "name"), item["Resource"],
           "/Resources/Namespace/" + std::to_string(index) + "/Resource");
  }
  return result;
}

std::vector<InlineResourceSummary> ManifestWorkspace::inlineResources(
    std::string const& ownerNamespace, std::string const& ownerName) const {
  std::vector<InlineResourceSummary> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, ownerNamespace);
  if (!collection || resourceCount(*collection, ownerName) != 1U) return result;
  auto resource = collection->resources[*findResourceIndex(*collection, ownerName)];
  auto dependencies = standardDependencyItems(resource);
  for (std::size_t index = 0; index < dependencies.size(); ++index) {
    auto const& dependency = dependencies[index];
    if (!scalar(dependency, "ref").empty() || scalar(dependency, "type").empty()) {
      continue;
    }
    InlineResourceSummary summary;
    summary.resourceNamespace = ownerNamespace;
    summary.instancePath = "/DependentResources/DependentResource/" +
                           std::to_string(index);
    summary.ownerName = ownerName;
    summary.dependencyIndex = index;
    summary.dependencyId = scalar(dependency, "id");
    summary.resourceType = scalar(dependency, "type");
    summary.name = scalar(dependency, "name");
    summary.explicitName = !summary.name.empty();
    summary.location = scalar(dependency, "location");
    for (auto const& option : collectionItems(dependency["Option"])) {
      summary.options.emplace_back(scalar(option, "name"),
                                   scalar(option, "value"));
    }
    if (summary.name.empty()) summary.name = summary.location;
    summary.editable = resourceForm(summary.resourceType) != nullptr;
    summary.unknownType =
        mCatalog.findExact({summary.resourceType, {}}) == nullptr;
    if (summary.unknownType) {
      summary.limitationWarning =
          "No editing schema is loaded for this inline Resource Type. Its "
          "payload and unknown Definitions are preserved read-only.";
    } else if (!summary.editable) {
      summary.limitationWarning =
          "This inline Resource schema uses an unsupported authoring shape; its "
          "payload is preserved read-only.";
    }
    result.push_back(std::move(summary));
  }
  return result;
}

std::vector<ResourceReferenceSelector> ManifestWorkspace::resourceReferences(
    std::string const& ownerNamespace, std::string const& ownerName) const {
  std::vector<ResourceReferenceSelector> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, ownerNamespace);
  if (!collection || resourceCount(*collection, ownerName) != 1U) return result;
  auto resource = collection->resources[*findResourceIndex(*collection, ownerName)];
  auto const ownerType = scalar(resource, "type");
  ResourceIdentity const owner{ownerNamespace, ownerName};
  auto dependencies = standardDependencyItems(resource);
  auto declarations = resourceDeclarations(root);

  for (std::size_t index = 0; index < dependencies.size(); ++index) {
    auto const& dependency = dependencies[index];
    auto const reference = scalar(dependency, "ref");
    if (reference.empty()) continue;

    ResourceReferenceSelector selector;
    selector.ownerNamespace = ownerNamespace;
    selector.ownerName = ownerName;
    selector.dependencyIndex = index;
    selector.dependencyId = scalar(dependency, "id");
    selector.reference = reference;
    selector.allowedResourceTypes =
        standardAllowedTypes(ownerType, selector.dependencyId);
    auto annotations = dependencyAnnotations(mCatalog, ownerType);
    auto annotated = std::find_if(
        annotations.begin(), annotations.end(), [&](auto const& dependency) {
          return dependency.id == selector.dependencyId;
        });
    if (annotated != annotations.end()) {
      selector.allowedResourceTypes = annotated->allowedResourceTypes;
    }
    auto const selectedIdentity = parseReference(reference, ownerNamespace);

    for (auto const& declaration : declarations) {
      if (declaration.inlineResource || declaration.identity == owner ||
          !allowedResourceType(selector.allowedResourceTypes,
                               declaration.resourceType) ||
          !validResourceName(declaration.identity.name)) {
        continue;
      }
      if (std::any_of(selector.choices.begin(), selector.choices.end(),
                      [&](auto const& choice) {
                        return choice.resourceNamespace ==
                                   declaration.identity.resourceNamespace &&
                               choice.name == declaration.identity.name;
                      })) {
        continue;
      }
      ResourceReferenceChoice choice;
      choice.resourceNamespace = declaration.identity.resourceNamespace;
      choice.name = declaration.identity.name;
      choice.resourceType = declaration.resourceType;
      choice.qualifiedIdentity = qualifiedIdentity(declaration.identity);
      choice.selected = declaration.identity == selectedIdentity;
      auto const candidateMatches =
          matchingDeclarations(declarations, declaration.identity);
      choice.disabled = candidateMatches.size() != 1U ||
                        dependencyPathExists(root, declaration.identity, owner);
      if (candidateMatches.size() != 1U) {
        choice.reason = "ambiguous Resource identity";
      } else if (choice.disabled) {
        choice.reason = "would create a dependency cycle";
      }
      selector.choices.push_back(std::move(choice));
    }

    auto selected = std::find_if(
        selector.choices.begin(), selector.choices.end(),
        [](ResourceReferenceChoice const& choice) { return choice.selected; });
    auto matches = matchingDeclarations(declarations, selectedIdentity);
    bool const selectedIsUsable =
        matches.size() == 1U && matches.front().identity != owner &&
        allowedResourceType(selector.allowedResourceTypes,
                            matches.front().resourceType);
    if (selected == selector.choices.end()) {
      ResourceReferenceChoice legacy;
      legacy.resourceNamespace = selectedIdentity.resourceNamespace;
      legacy.name = selectedIdentity.name;
      legacy.qualifiedIdentity = qualifiedIdentity(selectedIdentity);
      legacy.selected = true;
      legacy.disabled = true;
      if (matches.size() == 1U) {
        legacy.resourceType = matches.front().resourceType;
        legacy.inlineResource = matches.front().inlineResource;
        if (matches.front().inlineResource) {
          legacy.reason = "existing inline Resource reference";
        } else if (matches.front().identity == owner) {
          legacy.reason = "self-reference";
        } else {
          legacy.reason = "incompatible Resource Type";
        }
      } else {
        legacy.missing = true;
        legacy.reason = matches.empty() ? "missing Resource"
                                        : "ambiguous Resource identity";
        selector.missing = true;
      }
      selector.choices.insert(selector.choices.begin(), std::move(legacy));
    } else if (!selectedIsUsable) {
      selected->disabled = true;
      if (matches.size() != 1U) {
        selected->reason = "ambiguous Resource identity";
        selector.missing = true;
      } else if (matches.front().identity == owner) {
        selected->reason = "self-reference";
      } else {
        selected->reason = "incompatible Resource Type";
      }
    }

    auto previewRoot = YAML::Clone(root);
    auto previewCollection = findResourceCollection(previewRoot, ownerNamespace);
    auto previewResource =
        previewCollection->resources[*findResourceIndex(*previewCollection, ownerName)];
    auto previewDependencies = standardDependencyItems(previewResource);
    eraseCollectionItem(previewDependencies, index);
    if (previewDependencies.empty()) {
      previewResource.remove("DependentResources");
    } else {
      setCollection(previewResource["DependentResources"], "DependentResource",
                    previewDependencies);
    }
    previewCollection->resources[*findResourceIndex(*previewCollection, ownerName)] =
        previewResource;
    publishCollection(previewRoot, *previewCollection);
    auto previewDocument = ResourceManifestDocument::parse(
        emitYaml(previewRoot), "Resource reference clear preview");
    selector.clearable = previewDocument.validate(mCatalog).valid();
    result.push_back(std::move(selector));
  }
  return result;
}

std::vector<DependencyDiagnostic> ManifestWorkspace::dependencyDiagnostics() const {
  std::vector<DependencyDiagnostic> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto declarations = resourceDeclarations(root);
  for (auto const& resource : resources()) {
    ResourceIdentity const owner{resource.resourceNamespace, resource.name};
    for (auto const& selector :
         resourceReferences(resource.resourceNamespace, resource.name)) {
      auto target = parseReference(selector.reference, resource.resourceNamespace);
      auto matches = matchingDeclarations(declarations, target);
      if (matches.size() != 1U) {
        result.push_back({resource.resourceNamespace, resource.name,
                          selector.dependencyIndex,
                          "Reference '" + selector.reference + "' from '" +
                              qualifiedIdentity(owner) + "' is " +
                              (matches.empty() ? "missing." : "ambiguous.")});
      } else if (matches.front().identity == owner) {
        result.push_back({resource.resourceNamespace, resource.name,
                          selector.dependencyIndex,
                          "Resource '" + qualifiedIdentity(owner) +
                              "' references itself."});
      } else if (!allowedResourceType(selector.allowedResourceTypes,
                                      matches.front().resourceType)) {
        result.push_back({resource.resourceNamespace, resource.name,
                          selector.dependencyIndex,
                          "Reference '" + selector.reference + "' from '" +
                              qualifiedIdentity(owner) +
                              "' has an incompatible Resource Type."});
      } else if (dependencyPathExists(root, target, owner)) {
        result.push_back({resource.resourceNamespace, resource.name,
                          selector.dependencyIndex,
                          "Reference from '" + qualifiedIdentity(owner) +
                              "' to '" + qualifiedIdentity(target) +
                              "' participates in a dependency cycle."});
      }
    }
  }
  forEachStandardReference(
      root, [&](ResourceIdentity const& source, ResourceIdentity const& target) {
        auto matches = matchingDeclarations(declarations, target);
        if (matches.size() != 1U) return;
        bool const sourceIsRemovedWithTarget =
            source == target ||
            (matches.front().inlineResource && source == matches.front().owner);
        if (!sourceIsRemovedWithTarget) {
          result.push_back({target.resourceNamespace, target.name, 0,
                            "Resource '" + qualifiedIdentity(target) +
                                "' is referenced by '" +
                                qualifiedIdentity(source) +
                                "'; deletion is blocked.",
                            false});
        }
      });
  return result;
}

std::vector<std::string> ManifestWorkspace::incomingReferences(
    std::string const& resourceNamespace, std::string const& name) const {
  std::vector<std::string> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  ResourceIdentity const target{resourceNamespace, name};
  auto matches = matchingDeclarations(resourceDeclarations(root), target);
  std::optional<ResourceIdentity> inlineOwner;
  if (matches.size() == 1U && matches.front().inlineResource) {
    inlineOwner = matches.front().owner;
  }
  forEachStandardReference(
      root, [&](ResourceIdentity const& source, ResourceIdentity const& referenced) {
        if (referenced == target && source != target &&
            (!inlineOwner || source != *inlineOwner)) {
          result.push_back(qualifiedIdentity(source));
        }
      });
  return result;
}

std::vector<ResourceReferenceChoice> ManifestWorkspace::draftReferenceChoices(
    std::string const& dependencyId) const {
  std::vector<ResourceReferenceChoice> result;
  if (!mDraft || !mDocument || mDraft->references.empty()) return result;
  auto reference = std::find_if(
      mDraft->references.begin(), mDraft->references.end(),
      [&](auto const& item) {
        return dependencyId.empty() || item.id == dependencyId;
      });
  if (reference == mDraft->references.end()) return result;
  auto root = YAML::Load(canonicalYaml());
  auto declarations = resourceDeclarations(root);
  for (auto const& declaration : declarations) {
    if (declaration.inlineResource ||
        !allowedResourceType(reference->allowedResourceTypes,
                             declaration.resourceType) ||
        !validResourceName(declaration.identity.name)) {
      continue;
    }
    ResourceReferenceChoice choice;
    choice.resourceNamespace = declaration.identity.resourceNamespace;
    choice.name = declaration.identity.name;
    choice.resourceType = declaration.resourceType;
    choice.qualifiedIdentity = qualifiedIdentity(declaration.identity);
    choice.selected = choice.resourceNamespace == reference->resourceNamespace &&
                      choice.name == reference->name;
    auto matches = matchingDeclarations(declarations, declaration.identity);
    choice.disabled = matches.size() != 1U;
    if (choice.disabled) choice.reason = "ambiguous Resource identity";
    if (std::none_of(result.begin(), result.end(), [&](auto const& existing) {
          return existing.resourceNamespace == choice.resourceNamespace &&
                 existing.name == choice.name;
        })) {
      result.push_back(std::move(choice));
    }
  }
  return result;
}

std::vector<DefinitionFactoryChoice> ManifestWorkspace::definitionFactories(
    std::string const& resourceNamespace, std::string const& name) const {
  std::vector<DefinitionFactoryChoice> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) return result;
  auto const resource =
      collection->resources[*findResourceIndex(*collection, name)];
  auto const resourceType = scalar(resource, "type");
  std::set<std::string> existing;
  for (auto const& definition :
       collectionItems(resource["Definitions"]["Definition"])) {
    existing.insert(scalar(definition, "factory"));
  }
  for (auto const& entry : mCatalog.entries()) {
    if (entry.kind != ResourceSchemaKind::resourceType ||
        entry.resourceType != resourceType) {
      continue;
    }
    DefinitionFactoryChoice choice;
    choice.factoryType = entry.factoryType;
    choice.selected = existing.contains(choice.factoryType);
    choice.disabled = choice.selected;
    try {
      auto schema = Json::parse(entry.contents);
      choice.title = schema.value(
          "title", choice.factoryType.empty() ? "Default Definition"
                                                : choice.factoryType);
    } catch (...) {
      choice.title = choice.factoryType.empty() ? "Default Definition"
                                                : choice.factoryType;
    }
    result.push_back(std::move(choice));
  }
  return result;
}

std::vector<NestedFormItem> ManifestWorkspace::nestedFormItems(
    std::string const& resourceNamespace, std::string const& name) const {
  std::vector<NestedFormItem> result;
  if (!mDocument) return result;
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) return result;
  auto resource = collection->resources[*findResourceIndex(*collection, name)];
  auto const type = scalar(resource, "type");
  bool const builtInAdvanced =
      type == "ImageSet" || type == "AnimationSet" || type == "Program" ||
      type == "Material";
  if (!builtInAdvanced && !resourceForm(type)) return result;

  auto definitions = collectionItems(resource["Definitions"]["Definition"]);
  for (std::size_t definitionIndex = 0; definitionIndex < definitions.size();
       ++definitionIndex) {
    auto const& definition = definitions[definitionIndex];
    auto definitionPath = "/Definitions/Definition/" +
                          std::to_string(definitionIndex);
    NestedFormItem definitionItem;
    definitionItem.kind = NestedCollectionKind::definition;
    definitionItem.path = definitionPath;
    definitionItem.label = scalar(definition, "factory").empty()
                               ? "Default Definition"
                               : "Definition: " + scalar(definition, "factory");
    auto const factoryType = scalar(definition, "factory");
    if (!factoryType.empty()) {
      auto const* entry = mCatalog.findExact({type, factoryType});
      if (entry) {
        try {
          auto schema = expandSchema(mCatalog, Json::parse(entry->contents),
                                     entry->schemaId, definition);
          auto required = schema.value("required", std::vector<std::string>{});
          if (schema.contains("properties") && schema.at("properties").is_object()) {
            for (auto const& [propertyName, rawPropertySchema] :
                 schema.at("properties").items()) {
              auto propertySchema = expandSchema(
                  mCatalog, rawPropertySchema, entry->schemaId,
                  definition[propertyName]);
              auto property = propertyForm(
                  definition, propertyName,
                  std::find(required.begin(), required.end(), propertyName) !=
                      required.end());
              auto schemaType = propertySchema.value("type", std::string{});
              property.integer = schemaType == "integer";
              property.number = schemaType == "number";
              property.boolean = schemaType == "boolean";
              if (propertySchema.contains("minimum")) {
                property.hasMinimum = true;
                property.minimum = propertySchema.at("minimum").get<double>();
              } else if (propertySchema.contains("exclusiveMinimum") &&
                         propertySchema.at("exclusiveMinimum").is_number()) {
                property.hasMinimum = true;
                property.exclusiveMinimum = true;
                property.minimum =
                    propertySchema.at("exclusiveMinimum").get<double>();
              }
              if (propertySchema.contains("enum") &&
                  propertySchema.at("enum").is_array()) {
                for (auto const& enumValue : propertySchema.at("enum")) {
                  if (enumValue.is_string())
                    property.enumValues.push_back(enumValue.get<std::string>());
                }
              }
              definitionItem.properties.push_back(std::move(property));
            }
          }
        } catch (...) {
          // A catalog accepted by ResourceSchemaCatalog remains valid for
          // validation. Unsupported authoring shapes stay read-only rather
          // than being guessed by the editor.
        }
      }
      result.push_back(std::move(definitionItem));
      continue;
    }
    if (!builtInAdvanced) {
      auto const* entry = mCatalog.findExact({type, {}});
      if (entry) {
        try {
          auto schema = defaultDefinitionSchema(mCatalog, *entry);
          auto required = schema.value("required", std::vector<std::string>{});
          for (auto const& [propertyName, rawPropertySchema] :
               schema.at("properties").items()) {
            auto propertySchema = expandSchema(
                mCatalog, rawPropertySchema, entry->schemaId,
                definition[propertyName]);
            auto property = propertyForm(
                definition, propertyName,
                std::find(required.begin(), required.end(), propertyName) !=
                    required.end());
            auto schemaType = propertySchema.value("type", std::string{});
            property.integer = schemaType == "integer";
            property.number = schemaType == "number";
            property.boolean = schemaType == "boolean";
            if (propertySchema.contains("minimum")) {
              property.hasMinimum = true;
              property.minimum = propertySchema.at("minimum").get<double>();
            } else if (propertySchema.contains("exclusiveMinimum") &&
                       propertySchema.at("exclusiveMinimum").is_number()) {
              property.hasMinimum = true;
              property.exclusiveMinimum = true;
              property.minimum =
                  propertySchema.at("exclusiveMinimum").get<double>();
            }
            if (propertySchema.contains("enum") &&
                propertySchema.at("enum").is_array()) {
              for (auto const& value : propertySchema.at("enum")) {
                if (value.is_string())
                  property.enumValues.push_back(value.get<std::string>());
              }
            }
            definitionItem.properties.push_back(std::move(property));
          }
        } catch (...) {
          definitionItem.properties.clear();
        }
      }
      result.push_back(std::move(definitionItem));
      continue;
    }
    result.push_back(std::move(definitionItem));

    if (type == "ImageSet") {
      auto images = definition["Images"];
      for (auto const kind : {NestedCollectionKind::image,
                              NestedCollectionKind::imageSet}) {
        auto const key = collectionName(kind);
        auto items = collectionItems(images[key]);
        for (std::size_t index = 0; index < items.size(); ++index) {
          NestedFormItem item;
          item.kind = kind;
          item.path = definitionPath + "/Images/" + key + "/" +
                      std::to_string(index);
          item.label = key + " " + std::to_string(index + 1U);
          item.properties = {
              propertyForm(items[index], "name", true),
              propertyForm(items[index], "x", true, true),
              propertyForm(items[index], "y", true, true),
              propertyForm(items[index], "width", true, true),
              propertyForm(items[index], "height", true, true)};
          item.properties[1].hasMinimum = true;
          item.properties[2].hasMinimum = true;
          item.properties[3].hasMinimum = true;
          item.properties[3].minimum = 1.0;
          item.properties[4].hasMinimum = true;
          item.properties[4].minimum = 1.0;
          if (kind == NestedCollectionKind::imageSet) {
            item.properties.push_back(
                propertyForm(items[index], "count", true, true));
            item.properties.back().hasMinimum = true;
            item.properties.back().minimum = 1.0;
            item.properties.push_back(
                propertyForm(items[index], "dx", true, true));
            item.properties.push_back(
                propertyForm(items[index], "dy", true, true));
          }
          result.push_back(std::move(item));
        }
      }
      continue;
    }

    if (type == "Program") {
      auto attribs = definition["Attribs"];
      NestedFormItem attribItem;
      attribItem.kind = NestedCollectionKind::object;
      attribItem.path = definitionPath + "/Attribs";
      attribItem.label = "Attributes";
      attribItem.properties = {
          propertyForm(attribs, "textures", false, true),
          propertyForm(attribs, "diffuse", false),
          propertyForm(attribs, "colours", false),
          propertyForm(attribs, "atlas", false),
          propertyForm(attribs, "rotation", false)};
      attribItem.properties.front().hasMinimum = true;
      for (std::size_t index = 1; index < attribItem.properties.size(); ++index)
        attribItem.properties[index].boolean = true;
      result.push_back(std::move(attribItem));

      auto mesh = definition["MeshSpecification"];
      NestedFormItem meshItem;
      meshItem.kind = NestedCollectionKind::object;
      meshItem.path = definitionPath + "/MeshSpecification";
      meshItem.label = "Mesh specification";
      meshItem.properties = {propertyForm(mesh, "primitive", true),
                             propertyForm(mesh, "indexed", true),
                             propertyForm(mesh, "storage", true)};
      meshItem.properties[0].enumValues = {"POINTS", "LINES", "TRIANGLES"};
      meshItem.properties[1].boolean = true;
      meshItem.properties[2].enumValues = {"STATIC", "DYNAMIC"};
      result.push_back(std::move(meshItem));

      auto buffers = collectionItems(mesh["Buffers"]["Buffer"]);
      for (std::size_t bufferIndex = 0; bufferIndex < buffers.size();
           ++bufferIndex) {
        auto bufferPath = definitionPath + "/MeshSpecification/Buffers/Buffer/" +
                          std::to_string(bufferIndex);
        NestedFormItem bufferItem;
        bufferItem.kind = NestedCollectionKind::buffer;
        bufferItem.path = bufferPath;
        bufferItem.label = "Buffer " + std::to_string(bufferIndex + 1U);
        result.push_back(std::move(bufferItem));
        auto channels = collectionItems(buffers[bufferIndex]["Channels"]["Channel"]);
        for (std::size_t channelIndex = 0; channelIndex < channels.size();
             ++channelIndex) {
          NestedFormItem channelItem;
          channelItem.kind = NestedCollectionKind::channel;
          channelItem.path = bufferPath + "/Channels/Channel/" +
                             std::to_string(channelIndex);
          channelItem.label = "Channel " + std::to_string(channelIndex + 1U);
          channelItem.properties = {
              propertyForm(channels[channelIndex], "data", true),
              propertyForm(channels[channelIndex], "type", true),
              propertyForm(channels[channelIndex], "normalised", false)};
          channelItem.properties[0].enumValues = {
              "POSITION2", "POSITION3", "POSITION4", "NORMAL3", "NORMAL4",
              "TEXCOORD2", "TEXCOORD3", "TEXCOORD4", "COLOUR1", "COLOUR3",
              "COLOUR4", "USER1", "USER2", "USER3", "USER4"};
          channelItem.properties[1].enumValues = {
              "FLOAT16", "FLOAT32", "INT8", "INT16", "INT32", "UINT8",
              "UINT16", "UINT32"};
          channelItem.properties[2].boolean = true;
          result.push_back(std::move(channelItem));
        }
      }
      continue;
    }

    if (type == "Material") {
      std::vector<std::string> imageDependencyIds;
      auto declarations = resourceDeclarations(root);
      for (auto const& dependency : standardDependencyItems(resource)) {
        auto id = scalar(dependency, "id");
        if (id.empty() || id == "Program") continue;
        auto reference = scalar(dependency, "ref");
        if (reference.empty()) {
          if (scalar(dependency, "type") == "Image")
            imageDependencyIds.push_back(std::move(id));
          continue;
        }
        auto matches = matchingDeclarations(
            declarations, parseReference(reference, resourceNamespace));
        if (matches.size() == 1U && matches.front().resourceType == "Image")
          imageDependencyIds.push_back(std::move(id));
      }
      auto textures = collectionItems(definition["Textures"]["Texture"]);
      for (std::size_t textureIndex = 0; textureIndex < textures.size();
           ++textureIndex) {
        auto const& texture = textures[textureIndex];
        NestedFormItem textureItem;
        textureItem.kind = NestedCollectionKind::texture;
        textureItem.path = definitionPath + "/Textures/Texture/" +
                           std::to_string(textureIndex);
        textureItem.label = "Texture " + std::to_string(textureIndex + 1U);
        textureItem.alternative = scalar(texture, "type");
        textureItem.alternatives = {"resource", "default"};
        textureItem.properties = {propertyForm(texture, "sampler", true)};
        if (textureItem.alternative == "resource") {
          auto value = propertyForm(texture, "value", true);
          value.selectorValues = imageDependencyIds;
          textureItem.properties.push_back(std::move(value));
        }
        result.push_back(std::move(textureItem));
      }
      continue;
    }

    auto animations = collectionItems(definition["Animations"]["Animation"]);
    for (std::size_t animationIndex = 0; animationIndex < animations.size();
         ++animationIndex) {
      auto const& animation = animations[animationIndex];
      auto animationPath = definitionPath + "/Animations/Animation/" +
                           std::to_string(animationIndex);
      NestedFormItem animationItem;
      animationItem.kind = NestedCollectionKind::animation;
      animationItem.path = animationPath;
      animationItem.label = "Animation " + std::to_string(animationIndex + 1U);
      animationItem.properties = {propertyForm(animation, "name", true),
                                  propertyForm(animation, "loopStyle", false)};
      animationItem.properties.back().enumValues = {"forwards", "once",
                                                     "pingpong"};
      auto frames = animation["Frames"];
      bool const imageSetFrames = !scalar(frames, "imageset").empty();
      animationItem.alternative = imageSetFrames ? "image-set" : "explicit";
      result.push_back(std::move(animationItem));

      NestedFormItem frameSet;
      frameSet.kind = imageSetFrames ? NestedCollectionKind::overrideFrame
                                     : NestedCollectionKind::frame;
      frameSet.path = animationPath + "/Frames";
      frameSet.label = imageSetFrames ? "Image-set frames"
                                      : "Explicit frame defaults";
      frameSet.alternative = imageSetFrames ? "image-set" : "explicit";
      if (imageSetFrames) {
        frameSet.properties.push_back(propertyForm(frames, "imageset", true));
        frameSet.properties.push_back(propertyForm(frames, "count", false, true));
        frameSet.properties.back().hasMinimum = true;
        frameSet.properties.back().minimum = 1.0;
      }
      frameSet.properties.push_back(
          propertyForm(frames, "time", false, false, true));
      frameSet.properties.back().hasMinimum = true;
      frameSet.properties.back().exclusiveMinimum = true;
      frameSet.properties.push_back(propertyForm(frames, "xoff", false, true));
      frameSet.properties.push_back(propertyForm(frames, "yoff", false, true));
      result.push_back(std::move(frameSet));
      auto framesItems = collectionItems(frames["Frame"]);
      for (std::size_t frameIndex = 0; frameIndex < framesItems.size();
           ++frameIndex) {
        auto const& frame = framesItems[frameIndex];
        NestedFormItem frameItem;
        frameItem.kind = imageSetFrames ? NestedCollectionKind::overrideFrame
                                        : NestedCollectionKind::frame;
        frameItem.path = animationPath + "/Frames/Frame/" +
                         std::to_string(frameIndex);
        frameItem.label = imageSetFrames
                              ? "Frame override " + std::to_string(frameIndex + 1U)
                              : "Frame " + std::to_string(frameIndex + 1U);
        if (imageSetFrames) {
          frameItem.properties.push_back(propertyForm(frame, "frame", true, true));
          frameItem.properties.back().hasMinimum = true;
        } else {
          frameItem.properties.push_back(propertyForm(frame, "image", true));
          frameItem.properties.push_back(propertyForm(frame, "frame", false, true));
          frameItem.properties.back().hasMinimum = true;
        }
        frameItem.properties.push_back(
            propertyForm(frame, "time", false, false, true));
        frameItem.properties.back().hasMinimum = true;
        frameItem.properties.back().exclusiveMinimum = true;
        frameItem.properties.push_back(propertyForm(frame, "xoff", false, true));
        frameItem.properties.push_back(propertyForm(frame, "yoff", false, true));
        result.push_back(std::move(frameItem));
        appendTags(result, frame, animationPath + "/Frames/Frame/" +
                                      std::to_string(frameIndex));
      }
    }
  }
  return result;
}

std::optional<DiagnosticNavigation> ManifestWorkspace::diagnosticNavigation(
    std::size_t diagnosticIndex) const {
  if (diagnosticIndex >= mStructuralDiagnostics.size()) return {};
  auto const& diagnostic = mStructuralDiagnostics[diagnosticIndex];
  return DiagnosticNavigation{diagnostic.resourceNamespace,
                              diagnostic.resourceName, {},
                              diagnostic.instancePath};
}

std::optional<DiagnosticNavigation>
ManifestWorkspace::semanticDiagnosticNavigation(
    std::size_t diagnosticIndex) const {
  if (diagnosticIndex >= mSemanticDiagnostics.size()) return {};
  auto const& diagnostic = mSemanticDiagnostics[diagnosticIndex];
  return DiagnosticNavigation{diagnostic.resourceNamespace,
                              diagnostic.resourceName,
                              diagnostic.resourcePath,
                              diagnostic.instancePath,
                              diagnostic.inlineResource,
                              diagnostic.dependencyIndex};
}

bool ManifestWorkspace::beginNamespaceDraft() {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  if (mNamespaceDraft || mDraft) {
    setFailure("Finish or cancel the current draft first.");
    return false;
  }
  mNamespaceDraft.emplace();
  validateNamespaceDraft();
  mOperationDiagnostic.clear();
  return true;
}

void ManifestWorkspace::setNamespaceDraftName(std::string name) {
  if (!mNamespaceDraft || mDraft) return;
  mNamespaceDraft->name = std::move(name);
  validateNamespaceDraft();
}

NamespaceDraft const* ManifestWorkspace::namespaceDraft() const noexcept {
  return mNamespaceDraft ? &*mNamespaceDraft : nullptr;
}

bool ManifestWorkspace::namespaceDraftValid() const noexcept {
  return mNamespaceDraft && mNamespaceDraft->validationMessage.empty();
}

void ManifestWorkspace::cancelNamespaceDraft() noexcept {
  mDraft.reset();
  mNamespaceDraft.reset();
}

void ManifestWorkspace::validateNamespaceDraft() {
  if (!mNamespaceDraft) return;
  if (!validNamespaceName(mNamespaceDraft->name)) {
    mNamespaceDraft->validationMessage =
        "A namespace name is required and cannot contain '/'.";
    return;
  }
  auto root = YAML::Load(canonicalYaml());
  if (namespaceCount(root, mNamespaceDraft->name) != 0U) {
    mNamespaceDraft->validationMessage = "Namespace names must be unique.";
    return;
  }
  mNamespaceDraft->validationMessage.clear();
}

bool ManifestWorkspace::beginDraft(std::string resourceType,
                                   std::string resourceNamespace) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  if (mDraft) {
    setFailure("Finish or cancel the current Resource draft first.");
    return false;
  }
  if (!resourceForm(resourceType)) {
    setFailure("Resource Type '" + resourceType +
               "' has no supported catalogued authoring form.");
    return false;
  }
  if (mNamespaceDraft &&
      (!namespaceDraftValid() || mNamespaceDraft->name != resourceNamespace)) {
    setFailure("The namespace draft must receive the next Resource.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  bool const pendingNamespace =
      mNamespaceDraft && namespaceDraftValid() &&
      mNamespaceDraft->name == resourceNamespace;
  if (!resourceNamespace.empty() && !pendingNamespace &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Namespace '" + resourceNamespace +
               "' was not found uniquely; duplicate namespaces are ambiguous.");
    return false;
  }
  if (!findResourceCollection(root, resourceNamespace) && !pendingNamespace) {
    setFailure("Namespace '" + resourceNamespace + "' does not exist.");
    return false;
  }
  mDraft = ResourceDraft{std::move(resourceNamespace), std::move(resourceType)};
  auto const* form = resourceForm(mDraft->resourceType);
  if (form) {
    for (auto const& dependency : form->requiredDependencies) {
      mDraft->references.push_back(
          {dependency.id, dependency.allowedResourceTypes});
    }
  }
  validateDraft();
  mOperationDiagnostic.clear();
  return true;
}

void ManifestWorkspace::setDraftName(std::string name) {
  if (!mDraft) return;
  mDraft->name = std::move(name);
  validateDraft();
}

bool ManifestWorkspace::selectDraftFile(fs::path const& selectedFile) {
  if (!mDraft) {
    setFailure("No Resource draft is active.");
    return false;
  }
  auto const* form = resourceForm(mDraft->resourceType);
  if (!form || form->fileProperty.empty()) {
    setFailure("This Resource draft has no source-file property.");
    return false;
  }
  auto relative = portableSelectedFile(selectedFile, *form);
  if (!relative) {
    validateDraft();
    return false;
  }
  mDraft->location = std::move(*relative);
  validateDraft();
  mOperationDiagnostic.clear();
  return true;
}

bool ManifestWorkspace::setDraftReference(std::string resourceNamespace,
                                          std::string name) {
  if (!mDraft || mDraft->references.empty()) {
    setFailure("No Resource draft dependency is active.");
    return false;
  }
  return setDraftReference(mDraft->references.front().id,
                           std::move(resourceNamespace), std::move(name));
}

bool ManifestWorkspace::setDraftReference(std::string const& dependencyId,
                                          std::string resourceNamespace,
                                          std::string name) {
  if (!mDraft) {
    setFailure("No Resource draft is active.");
    return false;
  }
  auto reference = std::find_if(
      mDraft->references.begin(), mDraft->references.end(),
      [&](auto const& item) { return item.id == dependencyId; });
  if (reference == mDraft->references.end()) {
    setFailure("The requested dependency is not required by this Resource Type.");
    return false;
  }
  auto choices = draftReferenceChoices(dependencyId);
  auto found = std::find_if(choices.begin(), choices.end(),
                            [&](auto const& choice) {
                              return choice.resourceNamespace == resourceNamespace &&
                                     choice.name == name && !choice.disabled;
                            });
  if (found == choices.end()) {
    setFailure("The selected Resource is not a compatible dependency.");
    return false;
  }
  reference->resourceNamespace = std::move(resourceNamespace);
  reference->name = std::move(name);
  mDraft->dependencyNamespace = mDraft->references.front().resourceNamespace;
  mDraft->dependencyName = mDraft->references.front().name;
  validateDraft();
  mOperationDiagnostic.clear();
  return true;
}

ResourceDraft const* ManifestWorkspace::draft() const noexcept {
  return mDraft ? &*mDraft : nullptr;
}

bool ManifestWorkspace::draftValid() const noexcept {
  return mDraft && mDraft->validationMessage.empty();
}

void ManifestWorkspace::validateDraft() {
  if (!mDraft) return;
  if (!validResourceName(mDraft->name)) {
    mDraft->validationMessage =
        "A Resource name is required and cannot contain '/'.";
    return;
  }
  auto allResources = resources();
  if (std::any_of(allResources.begin(), allResources.end(),
                  [&](ResourceSummary const& resource) {
                    return resource.resourceNamespace == mDraft->resourceNamespace &&
                           resource.name == mDraft->name;
                  })) {
    mDraft->validationMessage =
        "Resource names must be unique within their namespace.";
    return;
  }
  auto const* form = resourceForm(mDraft->resourceType);
  if (!form) {
    mDraft->validationMessage = "The Resource Type form is no longer available.";
    return;
  }
  for (auto const& reference : mDraft->references) {
    if (reference.name.empty()) {
      mDraft->validationMessage =
          "Select the required compatible " + reference.id + " dependency.";
      return;
    }
    auto choices = draftReferenceChoices(reference.id);
    if (std::none_of(choices.begin(), choices.end(),
                     [](auto const& choice) {
                       return choice.selected && !choice.disabled;
                     })) {
      mDraft->validationMessage = "The selected " + reference.id +
                                  " dependency is no longer available.";
      return;
    }
  }
  if (!form->fileProperty.empty() && mDraft->location.empty()) {
    mDraft->validationMessage = "Select the required source file.";
    return;
  }
  mDraft->validationMessage.clear();
}

bool ManifestWorkspace::commitDraft() {
  if (!mDraft) {
    setFailure("No Resource draft is active.");
    return false;
  }
  validateDraft();
  if (!draftValid()) {
    setFailure("Resource draft is invalid: " + mDraft->validationMessage);
    return false;
  }

  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, mDraft->resourceNamespace);
  bool const createsNamespace =
      !collection && mNamespaceDraft && namespaceDraftValid() &&
      mNamespaceDraft->name == mDraft->resourceNamespace;
  YAML::Node resource(YAML::NodeType::Map);
  resource["type"] = mDraft->resourceType;
  resource["name"] = mDraft->name;
  auto const* form = resourceForm(mDraft->resourceType);
  if (form && !mDraft->references.empty()) {
    std::vector<YAML::Node> dependencies;
    for (auto const& selected : mDraft->references) {
      YAML::Node dependency(YAML::NodeType::Map);
      dependency["id"] = selected.id;
      dependency["ref"] = formatReference(
          {selected.resourceNamespace, selected.name}, mDraft->resourceNamespace);
      dependencies.push_back(std::move(dependency));
    }
    setCollection(resource["DependentResources"], "DependentResource",
                  dependencies);
  }
  if (form && !form->fileProperty.empty()) {
    resource[form->fileProperty] = mDraft->location;
  }
  if (form && form->requiresDefinition) {
    YAML::Node definition(YAML::NodeType::Map);
    if (mDraft->resourceType == "ImageSet") {
      definition["Images"]["Image"] =
          defaultNestedItem(NestedCollectionKind::image);
    } else if (mDraft->resourceType == "AnimationSet") {
      definition["Animations"]["Animation"] =
          defaultNestedItem(NestedCollectionKind::animation);
    } else if (mDraft->resourceType == "Program") {
      definition["Attribs"] = YAML::Node(YAML::NodeType::Map);
      definition["MeshSpecification"]["primitive"] = "TRIANGLES";
      definition["MeshSpecification"]["indexed"] = false;
      definition["MeshSpecification"]["storage"] = "STATIC";
      definition["MeshSpecification"]["Buffers"]["Buffer"] =
          defaultNestedItem(NestedCollectionKind::buffer);
    } else if (mDraft->resourceType == "Material") {
      definition["Textures"] = YAML::Node(YAML::NodeType::Map);
    } else {
      auto const* schemaEntry =
          mCatalog.findExact({mDraft->resourceType, {}});
      if (!schemaEntry) {
        setFailure("The application Resource Type schema is no longer available.");
        return false;
      }
      try {
        definition = applicationDefaultDefinition(mCatalog, *schemaEntry);
      } catch (std::exception const& error) {
        setFailure("Could not create the application default Definition: " +
                   std::string(error.what()));
        return false;
      }
    }
    resource["Definitions"]["Definition"] = definition;
  }
  if (createsNamespace) {
    auto resourcesRoot = root["Resources"];
    auto namespaces = collectionItems(resourcesRoot["Namespace"]);
    YAML::Node resourceNamespace(YAML::NodeType::Map);
    resourceNamespace["name"] = mDraft->resourceNamespace;
    setCollection(resourceNamespace, "Resource", {resource});
    namespaces.push_back(std::move(resourceNamespace));
    setCollection(resourcesRoot, "Namespace", namespaces);
  } else if (collection) {
    collection->resources.push_back(std::move(resource));
    publishCollection(root, *collection);
  } else {
    setFailure("The draft namespace no longer exists.");
    return false;
  }
  auto const commandName = createsNamespace
                               ? "Create namespace and " + mDraft->resourceType +
                                     " Resource"
                               : "Create " + mDraft->resourceType + " Resource";
  if (!executeYamlCommand(commandName, emitYaml(root))) return false;
  mDraft.reset();
  if (createsNamespace) mNamespaceDraft.reset();
  return true;
}

void ManifestWorkspace::cancelDraft() noexcept { mDraft.reset(); }

bool ManifestWorkspace::renameNamespace(std::string const& currentName,
                                        std::string newName, bool continuous) {
  if (currentName.empty()) {
    setFailure("The default namespace is permanent and cannot be renamed.");
    return false;
  }
  if (!validNamespaceName(newName)) {
    setFailure("A namespace name is required and cannot contain '/'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  if (namespaceCount(root, currentName) != 1U) {
    setFailure("Namespace '" + currentName +
               "' was not found uniquely; duplicate namespaces are ambiguous.");
    return false;
  }
  if (newName != currentName && namespaceCount(root, newName) != 0U) {
    setFailure("Namespace names must be unique.");
    return false;
  }
  if (newName == currentName) return true;

  auto beforeCollection = findResourceCollection(root, currentName);
  auto const namespaceIndex = beforeCollection->namespaceIndex;
  rewriteStandardReferences(root, [&](ResourceIdentity identity) {
    if (identity.resourceNamespace == currentName)
      identity.resourceNamespace = newName;
    return identity;
  });
  auto collection = findResourceCollection(root, currentName);
  if (!collection) {
    setFailure("Namespace '" + currentName + "' was not found.");
    return false;
  }
  collection->owner["name"] = std::move(newName);
  publishCollection(root, *collection);
  return executeYamlCommand("Rename namespace", emitYaml(root),
                            "namespace:" + std::to_string(namespaceIndex),
                            continuous);
}

bool ManifestWorkspace::renameNamespaceAtPath(
    std::string const& namespacePath, std::string newName, bool continuous) {
  if (!validNamespaceName(newName)) {
    setFailure("A namespace name is required and cannot contain '/'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto item = nodeAtPath(root, pathParts(namespacePath),
                         pathParts(namespacePath).size());
  if (!item || !item.IsMap()) {
    setFailure("The diagnostic namespace path is no longer available.");
    return false;
  }
  auto oldName = scalar(item, "name");
  if (newName != oldName && namespaceCount(root, newName) != 0U) {
    setFailure("Namespace names must be unique.");
    return false;
  }
  if (newName == oldName) return true;
  if (namespaceCount(root, oldName) == 1U) {
    rewriteStandardReferences(root, [&](ResourceIdentity identity) {
      if (identity.resourceNamespace == oldName)
        identity.resourceNamespace = newName;
      return identity;
    });
    item = nodeAtPath(root, pathParts(namespacePath),
                      pathParts(namespacePath).size());
  }
  item["name"] = std::move(newName);
  return executeYamlCommand("Repair namespace name", emitYaml(root),
                            "namespace-path:" + namespacePath, continuous);
}

bool ManifestWorkspace::deleteNamespace(std::string const& name) {
  if (name.empty()) {
    setFailure("The default namespace is permanent and cannot be deleted.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  if (namespaceCount(root, name) != 1U) {
    setFailure("Namespace '" + name +
               "' was not found uniquely; duplicate namespaces are ambiguous.");
    return false;
  }
  bool blocked = false;
  forEachStandardReference(
      root, [&](ResourceIdentity const& source,
                ResourceIdentity const& target) {
        if (source.resourceNamespace != name &&
            target.resourceNamespace == name) {
          blocked = true;
        }
      });
  if (blocked) {
    setFailure("Namespace '" + name +
               "' cannot be deleted while Resources outside it reference its "
               "contents.");
    return false;
  }

  auto collection = findResourceCollection(root, name);
  eraseCollectionItem(collection->namespaces, collection->namespaceIndex);
  setCollection(root["Resources"], "Namespace", collection->namespaces);
  return executeYamlCommand("Delete namespace", emitYaml(root));
}

bool ManifestWorkspace::renameResource(std::string const& resourceNamespace,
                                       std::string const& currentName,
                                       std::string newName, bool continuous) {
  if (!validResourceName(newName)) {
    setFailure("A Resource name is required and cannot contain '/'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  if (!resourceNamespace.empty() &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Resource namespace was not found uniquely; duplicate namespaces "
               "are ambiguous.");
    return false;
  }
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*collection, currentName) != 1U) {
    setFailure("Resource '" + currentName +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  auto index = findResourceIndex(*collection, currentName);
  if (newName != currentName && resourceCount(*collection, newName) != 0U) {
    setFailure("Resource names must be unique within their namespace.");
    return false;
  }

  ResourceIdentity const oldIdentity{resourceNamespace, currentName};
  ResourceIdentity const newIdentity{resourceNamespace, newName};
  rewriteStandardReferences(root, [&](ResourceIdentity identity) {
    return identity == oldIdentity ? newIdentity : identity;
  });
  collection = findResourceCollection(root, resourceNamespace);
  collection->resources[*index]["name"] = std::move(newName);
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Rename Resource", emitYaml(root),
      "name:" + resourceNamespace + ":" + std::to_string(*index), continuous);
}

bool ManifestWorkspace::renameResourceAtPath(
    std::string const& resourcePath, std::string newName, bool continuous) {
  if (!validResourceName(newName)) {
    setFailure("A Resource name is required and cannot contain '/'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  std::vector<SemanticNamespace> namespaceItems;
  std::vector<SemanticResource> declarations;
  collectSemanticDocument(root, namespaceItems, declarations);
  auto declaration = std::find_if(
      declarations.begin(), declarations.end(), [&](auto const& item) {
        return !item.inlineResource && item.path == resourcePath;
      });
  if (declaration == declarations.end()) {
    setFailure("The diagnostic Resource path is no longer available.");
    return false;
  }
  auto const oldIdentity = declaration->identity;
  auto const newIdentity =
      ResourceIdentity{oldIdentity.resourceNamespace, newName};
  if (newIdentity != oldIdentity &&
      std::any_of(declarations.begin(), declarations.end(),
                  [&](auto const& item) { return item.identity == newIdentity; })) {
    setFailure("Resource names must be unique within their namespace.");
    return false;
  }
  auto const oldCount = static_cast<std::size_t>(std::count_if(
      declarations.begin(), declarations.end(), [&](auto const& item) {
        return item.identity == oldIdentity;
      }));
  if (oldCount == 1U) {
    rewriteStandardReferences(root, [&](ResourceIdentity identity) {
      return identity == oldIdentity ? newIdentity : identity;
    });
  }
  auto parts = pathParts(resourcePath);
  auto resource = nodeAtPath(root, parts, parts.size());
  if (!resource || !resource.IsMap()) {
    setFailure("The diagnostic Resource path is no longer available.");
    return false;
  }
  resource["name"] = std::move(newName);
  return executeYamlCommand("Repair Resource name", emitYaml(root),
                            "resource-path:" + resourcePath, continuous);
}

bool ManifestWorkspace::reorderResource(
    std::string const& resourceNamespace, std::string const& name,
    std::size_t newIndex, bool continuous) {
  auto root = YAML::Load(canonicalYaml());
  if (!resourceNamespace.empty() &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Resource namespace was not found uniquely; duplicate namespaces "
               "are ambiguous.");
    return false;
  }
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*collection, name) != 1U) {
    setFailure("Resource '" + name +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  auto currentIndex = *findResourceIndex(*collection, name);
  if (newIndex >= collection->resources.size()) {
    setFailure("Resource reorder position is outside its namespace.");
    return false;
  }
  if (newIndex == currentIndex) return true;
  auto resource = YAML::Clone(collection->resources[currentIndex]);
  eraseCollectionItem(collection->resources, currentIndex);
  insertCollectionItem(collection->resources, newIndex, resource);
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Reorder Resource", emitYaml(root),
      "order:" + resourceNamespace + ":" + name, continuous);
}

bool ManifestWorkspace::moveResource(
    std::string const& sourceNamespace, std::string const& name,
    std::string const& targetNamespace, std::size_t targetIndex,
    bool continuous) {
  if (sourceNamespace == targetNamespace) {
    auto root = YAML::Load(canonicalYaml());
    auto collection = findResourceCollection(root, sourceNamespace);
    if (!collection || collection->resources.empty()) {
      setFailure("Resource namespace was not found.");
      return false;
    }
    if (targetIndex == static_cast<std::size_t>(-1))
      targetIndex = collection->resources.size() - 1U;
    return reorderResource(sourceNamespace, name, targetIndex, continuous);
  }

  auto root = YAML::Load(canonicalYaml());
  if ((!sourceNamespace.empty() &&
       namespaceCount(root, sourceNamespace) != 1U) ||
      (!targetNamespace.empty() &&
       !(mNamespaceDraft && namespaceDraftValid() &&
         mNamespaceDraft->name == targetNamespace) &&
       namespaceCount(root, targetNamespace) != 1U)) {
    setFailure("Source or target namespace was not found uniquely; duplicate "
               "namespaces are ambiguous.");
    return false;
  }
  auto source = findResourceCollection(root, sourceNamespace);
  if (!source) {
    setFailure("Source Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*source, name) != 1U) {
    setFailure("Resource '" + name +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  bool const createsNamespace =
      mNamespaceDraft && namespaceDraftValid() &&
      mNamespaceDraft->name == targetNamespace;
  auto target = findResourceCollection(root, targetNamespace);
  if (!target && !createsNamespace) {
    setFailure("Target Resource namespace was not found.");
    return false;
  }
  if (target && resourceCount(*target, name) != 0U) {
    setFailure("Moving the Resource would create a duplicate identity.");
    return false;
  }

  ResourceIdentity const oldIdentity{sourceNamespace, name};
  ResourceIdentity const newIdentity{targetNamespace, name};
  std::vector<ResourceIdentity> inlineIdentities;
  auto sourceResource = source->resources[*findResourceIndex(*source, name)];
  for (auto const& dependency : standardDependencyItems(sourceResource)) {
    if (scalar(dependency, "ref").empty() &&
        !scalar(dependency, "type").empty()) {
      inlineIdentities.push_back({sourceNamespace, resourceIdentity(dependency)});
    }
  }
  auto declarations = resourceDeclarations(root);
  for (auto const& identity : inlineIdentities) {
    ResourceIdentity const movedInline{targetNamespace, identity.name};
    if (std::any_of(declarations.begin(), declarations.end(),
                    [&](auto const& declaration) {
                      return declaration.identity == movedInline &&
                             !(declaration.inlineResource &&
                               declaration.owner == oldIdentity);
                    })) {
      setFailure("Moving the Resource would collide with an owned inline Resource.");
      return false;
    }
  }
  rewriteStandardReferences(root, [&](ResourceIdentity identity) {
    if (identity == oldIdentity) return newIdentity;
    auto inlineIdentity = std::find(inlineIdentities.begin(),
                                    inlineIdentities.end(), identity);
    if (inlineIdentity != inlineIdentities.end()) {
      return ResourceIdentity{targetNamespace, identity.name};
    }
    return identity;
  });

  source = findResourceCollection(root, sourceNamespace);
  auto sourceIndex = *findResourceIndex(*source, name);
  auto movedResource = YAML::Clone(source->resources[sourceIndex]);
  if (scalar(movedResource, "name").empty()) {
    if (!validResourceName(name)) {
      setFailure("An inferred Resource name containing '/' must be renamed before "
                 "it can be moved.");
      return false;
    }
    movedResource["name"] = name;
  }
  eraseCollectionItem(source->resources, sourceIndex);
  if (source->named && source->resources.empty()) {
    eraseCollectionItem(source->namespaces, source->namespaceIndex);
    setCollection(root["Resources"], "Namespace", source->namespaces);
  } else {
    publishCollection(root, *source);
  }

  target = findResourceCollection(root, targetNamespace);
  if (!target) {
    auto resourcesRoot = root["Resources"];
    auto namespaces = collectionItems(resourcesRoot["Namespace"]);
    YAML::Node item(YAML::NodeType::Map);
    item["name"] = targetNamespace;
    setCollection(item, "Resource", {movedResource});
    namespaces.push_back(std::move(item));
    setCollection(resourcesRoot, "Namespace", namespaces);
  } else {
    if (targetIndex == static_cast<std::size_t>(-1) ||
        targetIndex > target->resources.size()) {
      targetIndex = target->resources.size();
    }
    insertCollectionItem(target->resources, targetIndex, movedResource);
    publishCollection(root, *target);
  }

  auto const commandName = source->named && source->resources.empty()
                               ? "Move Resource and remove namespace"
                               : "Move Resource";
  if (!executeYamlCommand(commandName, emitYaml(root),
                          "move:" + name, continuous)) {
    return false;
  }
  if (createsNamespace) mNamespaceDraft.reset();
  return true;
}

bool ManifestWorkspace::setResourceFile(std::string const& resourceNamespace,
                                        std::string const& name,
                                        fs::path const& selectedFile,
                                        bool continuous) {
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  auto index = findResourceIndex(*collection, name);
  if (!index) {
    setFailure("Resource '" + name + "' was not found.");
    return false;
  }
  auto const type = scalar(collection->resources[*index], "type");
  auto const* form = resourceForm(type);
  if (!form || form->fileProperty.empty()) {
    setFailure("Resource Type '" + type +
               "' has no editable file property.");
    return false;
  }
  auto relative = portableSelectedFile(selectedFile, *form);
  if (!relative) return false;
  collection->resources[*index][form->fileProperty] = *relative;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Select Resource file", emitYaml(root),
      "file:" + resourceNamespace + ":" + std::to_string(*index), continuous);
}

bool ManifestWorkspace::setResourceOption(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& optionName, std::optional<std::string> value,
    bool continuous) {
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  auto index = findResourceIndex(*collection, name);
  if (!index) {
    setFailure("Resource '" + name + "' was not found.");
    return false;
  }
  auto resource = collection->resources[*index];
  auto const* form = resourceForm(scalar(resource, "type"));
  ResourceOptionForm const* optionForm = nullptr;
  if (form) {
    auto foundForm = std::find_if(form->options.begin(), form->options.end(),
                                  [&](ResourceOptionForm const& option) {
                                    return option.name == optionName;
                                  });
    if (foundForm != form->options.end()) optionForm = &*foundForm;
  }
  if (!optionForm) {
    setFailure("Option '" + optionName + "' is not defined by this Resource schema.");
    return false;
  }
  if (value && !optionForm->boolean &&
      std::find(optionForm->values.begin(), optionForm->values.end(), *value) ==
          optionForm->values.end()) {
    setFailure("Option value is not permitted by the Resource schema.");
    return false;
  }
  if (value && optionForm->boolean && *value != "true" && *value != "false") {
    setFailure("Boolean Resource option must be true or false.");
    return false;
  }

  auto options = collectionItems(resource["Option"]);
  auto found = std::find_if(options.begin(), options.end(), [&](YAML::Node const& option) {
    return scalar(option, "name") == optionName;
  });
  if (!value) {
    if (found == options.end()) return true;
    eraseCollectionItem(options,
                        static_cast<std::size_t>(found - options.begin()));
  } else if (found == options.end()) {
    YAML::Node option(YAML::NodeType::Map);
    option["name"] = optionName;
    option["value"] = *value;
    options.push_back(std::move(option));
  } else {
    (*found)["value"] = *value;
  }
  setCollection(resource, "Option", options);
  collection->resources[*index] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Edit " + optionName + " option", emitYaml(root),
      "option:" + resourceNamespace + ":" + std::to_string(*index) + ":" +
          optionName,
      continuous);
}

bool ManifestWorkspace::setResourceReference(
    std::string const& ownerNamespace, std::string const& ownerName,
    std::size_t dependencyIndex,
    std::optional<std::pair<std::string, std::string>> target) {
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, ownerNamespace);
  if (!collection || resourceCount(*collection, ownerName) != 1U) {
    setFailure("Reference owner was not found uniquely.");
    return false;
  }
  auto ownerIndex = *findResourceIndex(*collection, ownerName);
  auto resource = collection->resources[ownerIndex];
  auto dependencies =
      standardDependencyItems(resource);
  if (dependencyIndex >= dependencies.size() ||
      scalar(dependencies[dependencyIndex], "ref").empty()) {
    setFailure("The selected standard Resource reference was not found.");
    return false;
  }

  auto selectors = resourceReferences(ownerNamespace, ownerName);
  auto selector = std::find_if(selectors.begin(), selectors.end(),
                               [&](auto const& item) {
                                 return item.dependencyIndex == dependencyIndex;
                               });
  if (selector == selectors.end()) {
    setFailure("The selected standard Resource reference was not available.");
    return false;
  }
  if (!target) {
    if (!selector->clearable) {
      setFailure("This Resource reference is required and cannot be cleared.");
      return false;
    }
    eraseCollectionItem(dependencies, dependencyIndex);
    if (dependencies.empty()) {
      resource.remove("DependentResources");
    } else {
      setCollection(resource["DependentResources"], "DependentResource",
                    dependencies);
    }
  } else {
    auto choice = std::find_if(
        selector->choices.begin(), selector->choices.end(),
        [&](ResourceReferenceChoice const& item) {
          return item.resourceNamespace == target->first &&
                 item.name == target->second;
        });
    if (choice == selector->choices.end() || choice->disabled ||
        choice->inlineResource || choice->missing) {
      setFailure("The selected Resource is not a compatible, cycle-safe target.");
      return false;
    }

    auto targetCollection = findResourceCollection(root, target->first);
    if (!targetCollection || resourceCount(*targetCollection, target->second) != 1U) {
      setFailure("The selected Resource target was not found uniquely.");
      return false;
    }
    auto targetIndex = *findResourceIndex(*targetCollection, target->second);
    if (scalar(targetCollection->resources[targetIndex], "name").empty()) {
      if (!validResourceName(target->second)) {
        setFailure("An inferred Resource name must be made explicit before it can be referenced.");
        return false;
      }
      targetCollection->resources[targetIndex]["name"] = target->second;
      publishCollection(root, *targetCollection);
      collection = findResourceCollection(root, ownerNamespace);
      ownerIndex = *findResourceIndex(*collection, ownerName);
      resource = collection->resources[ownerIndex];
      dependencies = standardDependencyItems(resource);
    }
    dependencies[dependencyIndex]["ref"] = formatReference(
        {target->first, target->second}, ownerNamespace);
    setCollection(resource["DependentResources"], "DependentResource",
                  dependencies);
  }
  collection->resources[ownerIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Select Resource dependency", emitYaml(root));
}

bool ManifestWorkspace::renameInlineResource(
    std::string const& ownerNamespace, std::string const& ownerName,
    std::size_t dependencyIndex, std::string newName, bool continuous) {
  if (!validResourceName(newName)) {
    setFailure("An inline Resource name is required and cannot contain '/'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, ownerNamespace);
  if (!collection || resourceCount(*collection, ownerName) != 1U) {
    setFailure("Inline Resource owner was not found uniquely.");
    return false;
  }
  auto ownerIndex = *findResourceIndex(*collection, ownerName);
  auto resource = collection->resources[ownerIndex];
  auto dependencies =
      standardDependencyItems(resource);
  if (dependencyIndex >= dependencies.size() ||
      !scalar(dependencies[dependencyIndex], "ref").empty() ||
      scalar(dependencies[dependencyIndex], "type").empty()) {
    setFailure("Inline Resource was not found.");
    return false;
  }
  auto oldName = resourceIdentity(dependencies[dependencyIndex]);
  ResourceIdentity const oldIdentity{ownerNamespace, oldName};
  ResourceIdentity const newIdentity{ownerNamespace, newName};
  auto declarations = resourceDeclarations(root);
  if (newIdentity != oldIdentity &&
      std::any_of(declarations.begin(), declarations.end(),
                  [&](auto const& declaration) {
                    return declaration.identity == newIdentity;
                  })) {
    setFailure("Inline Resource names must be unique within their namespace.");
    return false;
  }
  if (newIdentity == oldIdentity &&
      scalar(dependencies[dependencyIndex], "name") == newName) {
    return true;
  }

  rewriteStandardReferences(root, [&](ResourceIdentity identity) {
    return identity == oldIdentity ? newIdentity : identity;
  });
  collection = findResourceCollection(root, ownerNamespace);
  ownerIndex = *findResourceIndex(*collection, ownerName);
  resource = collection->resources[ownerIndex];
  dependencies =
      standardDependencyItems(resource);
  dependencies[dependencyIndex]["name"] = newName;
  setCollection(resource["DependentResources"], "DependentResource",
                dependencies);
  collection->resources[ownerIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Rename inline Resource", emitYaml(root),
      "inline-name:" + ownerNamespace + ":" + ownerName + ":" +
          std::to_string(dependencyIndex),
      continuous);
}

bool ManifestWorkspace::setInlineResourceFile(
    std::string const& ownerNamespace, std::string const& ownerName,
    std::size_t dependencyIndex, fs::path const& selectedFile,
    bool continuous) {
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, ownerNamespace);
  if (!collection || resourceCount(*collection, ownerName) != 1U) {
    setFailure("Inline Resource owner was not found uniquely.");
    return false;
  }
  auto ownerIndex = *findResourceIndex(*collection, ownerName);
  auto resource = collection->resources[ownerIndex];
  auto dependencies =
      standardDependencyItems(resource);
  if (dependencyIndex >= dependencies.size() ||
      !scalar(dependencies[dependencyIndex], "ref").empty()) {
    setFailure("Inline Resource was not found.");
    return false;
  }
  auto const* form = resourceForm(scalar(dependencies[dependencyIndex], "type"));
  if (!form || form->fileProperty.empty()) {
    setFailure("Inline Resource Type has no editable file property.");
    return false;
  }
  auto relative = portableSelectedFile(selectedFile, *form);
  if (!relative) return false;
  dependencies[dependencyIndex][form->fileProperty] = *relative;
  setCollection(resource["DependentResources"], "DependentResource",
                dependencies);
  collection->resources[ownerIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Select inline Resource file", emitYaml(root),
      "inline-file:" + ownerNamespace + ":" + ownerName + ":" +
          std::to_string(dependencyIndex),
      continuous);
}

bool ManifestWorkspace::setInlineResourceOption(
    std::string const& ownerNamespace, std::string const& ownerName,
    std::size_t dependencyIndex, std::string const& optionName,
    std::optional<std::string> value, bool continuous) {
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, ownerNamespace);
  if (!collection || resourceCount(*collection, ownerName) != 1U) {
    setFailure("Inline Resource owner was not found uniquely.");
    return false;
  }
  auto ownerIndex = *findResourceIndex(*collection, ownerName);
  auto resource = collection->resources[ownerIndex];
  auto dependencies =
      standardDependencyItems(resource);
  if (dependencyIndex >= dependencies.size() ||
      !scalar(dependencies[dependencyIndex], "ref").empty()) {
    setFailure("Inline Resource was not found.");
    return false;
  }
  auto dependency = dependencies[dependencyIndex];
  auto const* form = resourceForm(scalar(dependency, "type"));
  ResourceOptionForm const* optionForm = nullptr;
  if (form) {
    auto found = std::find_if(form->options.begin(), form->options.end(),
                              [&](auto const& option) {
                                return option.name == optionName;
                              });
    if (found != form->options.end()) optionForm = &*found;
  }
  if (!optionForm ||
      (value && !optionForm->boolean &&
       std::find(optionForm->values.begin(), optionForm->values.end(), *value) ==
           optionForm->values.end()) ||
      (value && optionForm->boolean && *value != "true" && *value != "false")) {
    setFailure("Inline Resource option value is not permitted by its schema.");
    return false;
  }
  auto options = collectionItems(dependency["Option"]);
  auto found = std::find_if(options.begin(), options.end(), [&](auto const& option) {
    return scalar(option, "name") == optionName;
  });
  if (!value) {
    if (found == options.end()) return true;
    eraseCollectionItem(options, static_cast<std::size_t>(found - options.begin()));
  } else if (found == options.end()) {
    YAML::Node option(YAML::NodeType::Map);
    option["name"] = optionName;
    option["value"] = *value;
    options.push_back(std::move(option));
  } else {
    (*found)["value"] = *value;
  }
  setCollection(dependency, "Option", options);
  dependencies[dependencyIndex] = dependency;
  setCollection(resource["DependentResources"], "DependentResource",
                dependencies);
  collection->resources[ownerIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Edit inline Resource option", emitYaml(root),
      "inline-option:" + ownerNamespace + ":" + ownerName + ":" +
          std::to_string(dependencyIndex) + ":" + optionName,
      continuous);
}

bool ManifestWorkspace::promoteInlineResource(
    std::string const& ownerNamespace, std::string const& ownerName,
    std::size_t dependencyIndex, std::string const& targetNamespace) {
  auto root = YAML::Load(canonicalYaml());
  if (!targetNamespace.empty() && namespaceCount(root, targetNamespace) != 1U) {
    setFailure("Promotion target namespace was not found uniquely.");
    return false;
  }
  auto collection = findResourceCollection(root, ownerNamespace);
  auto targetCollection = findResourceCollection(root, targetNamespace);
  if (!collection || !targetCollection ||
      resourceCount(*collection, ownerName) != 1U) {
    setFailure("Inline Resource owner or promotion namespace was not found uniquely.");
    return false;
  }
  auto ownerIndex = *findResourceIndex(*collection, ownerName);
  auto resource = collection->resources[ownerIndex];
  auto dependencies =
      standardDependencyItems(resource);
  if (dependencyIndex >= dependencies.size() ||
      !scalar(dependencies[dependencyIndex], "ref").empty() ||
      scalar(dependencies[dependencyIndex], "type").empty()) {
    setFailure("Inline Resource was not found.");
    return false;
  }
  auto promoted = YAML::Clone(dependencies[dependencyIndex]);
  auto const dependencyId = scalar(promoted, "id");
  promoted.remove("id");
  auto const name = resourceIdentity(promoted);
  if (!validResourceName(name)) {
    setFailure("Inline Resource must have an explicit valid name before promotion.");
    return false;
  }
  ResourceIdentity const oldIdentity{ownerNamespace, name};
  ResourceIdentity const newIdentity{targetNamespace, name};
  auto declarations = resourceDeclarations(root);
  auto collisions = matchingDeclarations(declarations, newIdentity);
  bool const onlyPromotedInline =
      collisions.size() == 1U && collisions.front().inlineResource &&
      collisions.front().owner == ResourceIdentity{ownerNamespace, ownerName} &&
      collisions.front().dependencyIndex == dependencyIndex;
  if (!collisions.empty() && !onlyPromotedInline) {
    setFailure("Promotion would create a duplicate Resource identity.");
    return false;
  }

  if (oldIdentity != newIdentity) {
    rewriteStandardReferences(root, [&](ResourceIdentity identity) {
      return identity == oldIdentity ? newIdentity : identity;
    });
  }
  collection = findResourceCollection(root, ownerNamespace);
  ownerIndex = *findResourceIndex(*collection, ownerName);
  resource = collection->resources[ownerIndex];
  dependencies =
      standardDependencyItems(resource);
  YAML::Node reference(YAML::NodeType::Map);
  if (!dependencyId.empty()) reference["id"] = dependencyId;
  reference["ref"] = formatReference(newIdentity, ownerNamespace);
  dependencies[dependencyIndex] = reference;
  setCollection(resource["DependentResources"], "DependentResource",
                dependencies);
  collection->resources[ownerIndex] = resource;
  publishCollection(root, *collection);

  targetCollection = findResourceCollection(root, targetNamespace);
  targetCollection->resources.push_back(promoted);
  publishCollection(root, *targetCollection);
  return executeYamlCommand("Promote inline Resource", emitYaml(root));
}

bool ManifestWorkspace::addResourceDependency(
    std::string const& ownerNamespace, std::string const& ownerName,
    std::string dependencyId,
    std::pair<std::string, std::string> const& target) {
  if (!validResourceName(dependencyId) || dependencyId == "Program") {
    setFailure("A Material image dependency ID is required and cannot be 'Program'.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, ownerNamespace);
  auto targetCollection = findResourceCollection(root, target.first);
  if (!collection || !targetCollection ||
      resourceCount(*collection, ownerName) != 1U ||
      resourceCount(*targetCollection, target.second) != 1U) {
    setFailure("Dependency owner or target was not found uniquely.");
    return false;
  }
  auto ownerIndex = *findResourceIndex(*collection, ownerName);
  auto resource = collection->resources[ownerIndex];
  if (scalar(resource, "type") != "Material") {
    setFailure("Only Material image dependencies can be added by this form.");
    return false;
  }
  auto targetIndex = *findResourceIndex(*targetCollection, target.second);
  if (scalar(targetCollection->resources[targetIndex], "type") != "Image") {
    setFailure("A Material texture dependency must target an Image Resource.");
    return false;
  }
  auto dependencies = standardDependencyItems(resource);
  if (std::any_of(dependencies.begin(), dependencies.end(),
                  [&](auto const& dependency) {
                    return scalar(dependency, "id") == dependencyId;
                  })) {
    setFailure("Material dependency IDs must be unique.");
    return false;
  }
  YAML::Node dependency(YAML::NodeType::Map);
  dependency["id"] = std::move(dependencyId);
  dependency["ref"] = formatReference({target.first, target.second}, ownerNamespace);
  dependencies.push_back(std::move(dependency));
  setCollection(resource["DependentResources"], "DependentResource", dependencies);
  collection->resources[ownerIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Add Material image dependency", emitYaml(root));
}

bool ManifestWorkspace::addDefinition(std::string const& resourceNamespace,
                                      std::string const& name,
                                      std::string factoryType) {
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) {
    setFailure("Definition Resource was not found uniquely.");
    return false;
  }
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto const resourceType = scalar(resource, "type");
  auto const* schemaEntry = mCatalog.findExact({resourceType, factoryType});
  if (!schemaEntry) {
    setFailure("Definition factory '" + factoryType +
               "' is not registered for Resource Type '" + resourceType + "'.");
    return false;
  }
  auto definitions = collectionItems(resource["Definitions"]["Definition"]);
  if (std::any_of(definitions.begin(), definitions.end(), [&](auto const& item) {
        return scalar(item, "factory") == factoryType;
      })) {
    setFailure("A Resource cannot contain duplicate Definition factories.");
    return false;
  }

  YAML::Node definition(YAML::NodeType::Map);
  if (factoryType.empty()) {
    if (resourceType == "ImageSet") {
      definition["Images"]["Image"] = defaultNestedItem(NestedCollectionKind::image);
    } else if (resourceType == "AnimationSet") {
      definition["Animations"]["Animation"] =
          defaultNestedItem(NestedCollectionKind::animation);
    } else if (resourceType == "Program") {
      definition["Attribs"] = YAML::Node(YAML::NodeType::Map);
      definition["MeshSpecification"]["primitive"] = "TRIANGLES";
      definition["MeshSpecification"]["indexed"] = false;
      definition["MeshSpecification"]["storage"] = "STATIC";
      definition["MeshSpecification"]["Buffers"]["Buffer"] =
          defaultNestedItem(NestedCollectionKind::buffer);
    } else if (resourceType == "Material") {
      definition["Textures"] = YAML::Node(YAML::NodeType::Map);
    } else {
      try {
        if (!supportedDefaultDefinition(mCatalog, *schemaEntry)) {
          throw std::runtime_error("schema is outside the supported form subset");
        }
        definition = applicationDefaultDefinition(mCatalog, *schemaEntry);
      } catch (std::exception const& error) {
        setFailure("The selected default Definition has no supported starter form: " +
                   std::string(error.what()));
        return false;
      }
    }
  } else {
    try {
      auto schema = expandSchema(mCatalog, Json::parse(schemaEntry->contents),
                                 schemaEntry->schemaId, definition);
      auto const schemaType = schema.value("type", std::string{});
      if (!schemaType.empty() && schemaType != "object") {
        throw std::runtime_error("Definition schema root is not an object");
      }
      auto required = schema.value("required", std::vector<std::string>{});
      auto properties = schema.at("properties");
      for (auto const& propertyName : required) {
        auto property = expandSchema(mCatalog, properties.at(propertyName),
                                     schemaEntry->schemaId,
                                     definition[propertyName]);
        if (propertyName == "factory") {
          definition[propertyName] = factoryType;
        } else if (property.contains("const")) {
          auto const& value = property.at("const");
          if (value.is_string())
            definition[propertyName] = value.get<std::string>();
          else if (value.is_boolean())
            definition[propertyName] = value.get<bool>();
          else if (value.is_number_integer())
            definition[propertyName] = value.get<long long>();
          else if (value.is_number())
            definition[propertyName] = value.get<double>();
        } else if (property.contains("enum") && !property.at("enum").empty()) {
          auto const& value = property.at("enum").front();
          if (value.is_string())
            definition[propertyName] = value.get<std::string>();
          else if (value.is_boolean())
            definition[propertyName] = value.get<bool>();
          else if (value.is_number_integer())
            definition[propertyName] = value.get<long long>();
          else if (value.is_number())
            definition[propertyName] = value.get<double>();
        } else {
          auto const propertyType = property.value("type", std::string{});
          if (propertyType == "boolean") definition[propertyName] = false;
          else if (propertyType == "integer") {
            auto minimum = property.value("minimum", 0.0);
            definition[propertyName] = static_cast<long long>(std::ceil(minimum));
          } else if (propertyType == "number") {
            auto minimum = property.value("minimum", 0.0);
            if (property.contains("exclusiveMinimum") &&
                property.at("exclusiveMinimum").is_number()) {
              minimum = property.at("exclusiveMinimum").get<double>() + 1.0;
            }
            definition[propertyName] = minimum;
          } else if (propertyType == "string") {
            definition[propertyName] = "Value";
          } else if (propertyType == "object") {
            definition[propertyName] = YAML::Node(YAML::NodeType::Map);
          } else if (propertyType == "array") {
            definition[propertyName] = YAML::Node(YAML::NodeType::Sequence);
          } else {
            throw std::runtime_error(
                "required property uses an unsupported authoring construct");
          }
        }
      }
      definition["factory"] = factoryType;
    } catch (std::exception const& error) {
      setFailure("Could not create the selected Definition schema exactly: " +
                 std::string(error.what()));
      return false;
    }
  }
  definitions.push_back(std::move(definition));
  setCollection(resource["Definitions"], "Definition", definitions);
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Add Definition", emitYaml(root));
}

bool ManifestWorkspace::setNestedAlternative(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& itemPath, std::string alternative) {
  auto forms = nestedFormItems(resourceNamespace, name);
  auto form = std::find_if(forms.begin(), forms.end(), [&](auto const& item) {
    return item.path == itemPath;
  });
  if (form == forms.end() ||
      std::find(form->alternatives.begin(), form->alternatives.end(), alternative) ==
          form->alternatives.end()) {
    setFailure("The requested schema alternative is not available.");
    return false;
  }
  if (form->kind == NestedCollectionKind::animation) {
    return setFramesAlternative(resourceNamespace, name, itemPath + "/Frames",
                                alternative == "image-set", "ImageSet");
  }
  if (form->kind != NestedCollectionKind::texture) {
    setFailure("The selected schema alternative is read-only.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) return false;
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto parts = pathParts(itemPath);
  auto texture = nodeAtPath(resource, parts, parts.size());
  if (!texture || !texture.IsMap()) return false;
  auto sampler = scalar(texture, "sampler");
  YAML::Node replacement(YAML::NodeType::Map);
  replacement["sampler"] = sampler.empty() ? "Texture" : sampler;
  replacement["type"] = alternative;
  if (alternative == "resource") {
    std::string dependencyId;
    auto declarations = resourceDeclarations(root);
    for (auto const& dependency : standardDependencyItems(resource)) {
      auto id = scalar(dependency, "id");
      if (id.empty() || id == "Program") continue;
      auto reference = scalar(dependency, "ref");
      bool compatible = reference.empty()
                            ? scalar(dependency, "type") == "Image"
                            : [&] {
                                auto matches = matchingDeclarations(
                                    declarations,
                                    parseReference(reference, resourceNamespace));
                                return matches.size() == 1U &&
                                       matches.front().resourceType == "Image";
                              }();
      if (compatible) {
        dependencyId = std::move(id);
        break;
      }
    }
    if (dependencyId.empty()) {
      setFailure("A resource texture requires a selected Material image dependency.");
      return false;
    }
    replacement["value"] = dependencyId;
  }
  std::size_t itemIndex = 0;
  if (parts.empty() || !indexPart(parts.back(), itemIndex)) return false;
  auto collectionPath = itemPath.substr(0, itemPath.rfind('/'));
  auto target = collectionAtPath(resource, collectionPath);
  if (!target) return false;
  auto items = collectionItems(target->first[target->second]);
  if (itemIndex >= items.size()) return false;
  items[itemIndex] = replacement;
  setCollection(target->first, target->second.c_str(), items);
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Change Texture alternative", emitYaml(root));
}

bool ManifestWorkspace::setNestedProperty(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& itemPath, std::string const& property,
    std::optional<std::string> value, bool continuous) {
  auto forms = nestedFormItems(resourceNamespace, name);
  auto itemForm = std::find_if(forms.begin(), forms.end(), [&](auto const& item) {
    return item.path == itemPath;
  });
  if (itemForm == forms.end()) {
    setFailure("The nested form item was not found.");
    return false;
  }
  auto propertyForm = std::find_if(
      itemForm->properties.begin(), itemForm->properties.end(),
      [&](auto const& candidate) { return candidate.name == property; });
  if (propertyForm == itemForm->properties.end()) {
    setFailure("Property '" + property + "' is not defined by this nested form.");
    return false;
  }
  if (!value && propertyForm->required) {
    setFailure("Required property '" + property + "' cannot be removed.");
    return false;
  }
  auto const& permittedValues = propertyForm->selectorValues.empty()
                                    ? propertyForm->enumValues
                                    : propertyForm->selectorValues;
  if (value && !permittedValues.empty() &&
      std::find(permittedValues.begin(), permittedValues.end(), *value) ==
          permittedValues.end()) {
    setFailure("Property '" + property + "' is not one of the permitted values.");
    return false;
  }
  if (value && propertyForm->boolean && *value != "true" && *value != "false") {
    setFailure("Property '" + property + "' requires a boolean value.");
    return false;
  }

  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) {
    setFailure("Nested form Resource was not found uniquely.");
    return false;
  }
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto parts = pathParts(itemPath);
  auto item = nodeAtPath(resource, parts, parts.size());
  if (!item || !item.IsMap()) {
    setFailure("The nested property owner was not found.");
    return false;
  }
  if (!value) {
    item.remove(property);
  } else if (propertyForm->integer) {
    try {
      std::size_t consumed = 0;
      auto parsed = std::stoll(*value, &consumed);
      if (consumed != value->size() ||
          (propertyForm->hasMinimum &&
           static_cast<double>(parsed) < propertyForm->minimum)) {
        throw std::invalid_argument("constraint");
      }
      item[property] = parsed;
    } catch (...) {
      setFailure("Property '" + property +
                 "' requires an integer within its schema constraints.");
      return false;
    }
  } else if (propertyForm->boolean) {
    item[property] = *value == "true";
  } else if (propertyForm->number) {
    try {
      std::size_t consumed = 0;
      auto parsed = std::stod(*value, &consumed);
      bool const belowMinimum =
          propertyForm->hasMinimum &&
          (propertyForm->exclusiveMinimum ? parsed <= propertyForm->minimum
                                          : parsed < propertyForm->minimum);
      if (consumed != value->size() || !std::isfinite(parsed) || belowMinimum) {
        throw std::invalid_argument("constraint");
      }
      item[property] = parsed;
    } catch (...) {
      setFailure("Property '" + property +
                 "' requires a number within its schema constraints.");
      return false;
    }
  } else {
    item[property] = *value;
  }
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand(
      "Edit nested " + property, emitYaml(root),
      "nested:" + resourceNamespace + ":" + name + ":" + itemPath + ":" + property,
      continuous);
}

bool ManifestWorkspace::addNestedItem(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& collectionPath, NestedCollectionKind kind) {
  if (collectionName(kind).empty() ||
      !collectionPath.ends_with("/" + collectionName(kind))) {
    setFailure("The nested collection kind does not match its instance path.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) {
    setFailure("Nested form Resource was not found uniquely.");
    return false;
  }
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto target = collectionAtPath(resource, collectionPath);
  if (!target && kind == NestedCollectionKind::tag &&
      collectionPath.ends_with("/Tags/Tag")) {
    auto ownerPath = collectionPath.substr(0, collectionPath.size() - 9U);
    auto ownerParts = pathParts(ownerPath);
    auto owner = nodeAtPath(resource, ownerParts, ownerParts.size());
    if (owner && owner.IsMap()) {
      owner["Tags"] = YAML::Node(YAML::NodeType::Map);
      target = collectionAtPath(resource, collectionPath);
    }
  }
  if (!target) {
    setFailure("The nested collection parent was not found.");
    return false;
  }
  auto items = collectionItems(target->first[target->second]);
  auto added = defaultNestedItem(kind);
  auto addedName = scalar(added, "name");
  if (!addedName.empty()) {
    auto baseName = addedName;
    for (std::size_t suffix = 2U;
         std::any_of(items.begin(), items.end(), [&](auto const& item) {
           return scalar(item, "name") == addedName;
         });
         ++suffix) {
      addedName = baseName + " " + std::to_string(suffix);
    }
    added["name"] = addedName;
  }
  items.push_back(std::move(added));
  setCollection(target->first, target->second.c_str(), items);
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Add nested " + collectionName(kind), emitYaml(root));
}

bool ManifestWorkspace::duplicateNestedItem(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& itemPath) {
  auto parts = pathParts(itemPath);
  std::size_t index = 0;
  if (parts.size() < 2U || !indexPart(parts.back(), index)) {
    setFailure("A nested collection item path must end with an index.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) {
    setFailure("Nested form Resource was not found uniquely.");
    return false;
  }
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto collectionPath = itemPath.substr(0, itemPath.rfind('/'));
  auto target = collectionAtPath(resource, collectionPath);
  if (!target) {
    setFailure("The nested collection was not found.");
    return false;
  }
  auto items = collectionItems(target->first[target->second]);
  if (index >= items.size()) {
    setFailure("The nested collection item was not found.");
    return false;
  }
  if (collectionPath.ends_with("/Definitions/Definition") ||
      collectionPath == "/Definitions/Definition") {
    setFailure("A Resource cannot contain duplicate Definition factories.");
    return false;
  }
  auto duplicated = YAML::Clone(items[index]);
  auto duplicatedName = scalar(duplicated, "name");
  if (!duplicatedName.empty()) {
    auto baseName = duplicatedName + " copy";
    duplicatedName = baseName;
    for (std::size_t suffix = 2U;
         std::any_of(items.begin(), items.end(), [&](auto const& item) {
           return scalar(item, "name") == duplicatedName;
         });
         ++suffix) {
      duplicatedName = baseName + " " + std::to_string(suffix);
    }
    duplicated["name"] = duplicatedName;
  }
  insertCollectionItem(items, index + 1U, duplicated);
  setCollection(target->first, target->second.c_str(), items);
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Duplicate nested collection item", emitYaml(root));
}

bool ManifestWorkspace::removeNestedItem(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& itemPath) {
  auto parts = pathParts(itemPath);
  std::size_t index = 0;
  if (parts.size() < 2U || !indexPart(parts.back(), index)) {
    setFailure("A nested collection item path must end with an index.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) {
    setFailure("Nested form Resource was not found uniquely.");
    return false;
  }
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto collectionPath = itemPath.substr(0, itemPath.rfind('/'));
  auto target = collectionAtPath(resource, collectionPath);
  if (!target) {
    setFailure("The nested collection was not found.");
    return false;
  }
  auto items = collectionItems(target->first[target->second]);
  if (index >= items.size()) {
    setFailure("The nested collection item was not found.");
    return false;
  }
  eraseCollectionItem(items, index);
  setCollection(target->first, target->second.c_str(), items);
  if (items.empty() && target->second == "Tag" &&
      collectionPath.ends_with("/Tags/Tag")) {
    auto ownerPath = collectionPath.substr(0, collectionPath.size() - 9U);
    auto ownerParts = pathParts(ownerPath);
    auto owner = nodeAtPath(resource, ownerParts, ownerParts.size());
    if (owner && owner.IsMap()) owner.remove("Tags");
  }
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Remove nested collection item", emitYaml(root));
}

bool ManifestWorkspace::reorderNestedItem(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& itemPath, std::size_t newIndex) {
  auto parts = pathParts(itemPath);
  std::size_t index = 0;
  if (parts.size() < 2U || !indexPart(parts.back(), index)) {
    setFailure("A nested collection item path must end with an index.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) {
    setFailure("Nested form Resource was not found uniquely.");
    return false;
  }
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto collectionPath = itemPath.substr(0, itemPath.rfind('/'));
  auto target = collectionAtPath(resource, collectionPath);
  if (!target) {
    setFailure("The nested collection was not found.");
    return false;
  }
  auto items = collectionItems(target->first[target->second]);
  if (index >= items.size() || newIndex >= items.size()) {
    setFailure("Nested reorder position is outside its collection.");
    return false;
  }
  if (index == newIndex) return true;
  auto moved = YAML::Clone(items[index]);
  eraseCollectionItem(items, index);
  insertCollectionItem(items, newIndex, moved);
  setCollection(target->first, target->second.c_str(), items);
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Reorder nested collection item", emitYaml(root));
}

bool ManifestWorkspace::setFramesAlternative(
    std::string const& resourceNamespace, std::string const& name,
    std::string const& framesPath, bool imageSetFrames,
    std::string imageSet) {
  if (imageSetFrames && imageSet.empty()) {
    setFailure("Image-set frames require a non-empty image-set name.");
    return false;
  }
  auto root = YAML::Load(canonicalYaml());
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection || resourceCount(*collection, name) != 1U) {
    setFailure("Nested form Resource was not found uniquely.");
    return false;
  }
  auto resourceIndex = *findResourceIndex(*collection, name);
  auto resource = collection->resources[resourceIndex];
  auto parts = pathParts(framesPath);
  auto frames = nodeAtPath(resource, parts, parts.size());
  if (!frames || !frames.IsMap()) {
    setFailure("The Frames alternative was not found.");
    return false;
  }
  YAML::Node replacement(YAML::NodeType::Map);
  if (imageSetFrames) {
    replacement["imageset"] = std::move(imageSet);
  } else {
    replacement["Frame"] = defaultNestedItem(NestedCollectionKind::frame);
  }
  auto parent = nodeAtPath(resource, parts, parts.size() - 1U);
  if (!parent || !parent.IsMap()) {
    setFailure("The Frames alternative parent was not found.");
    return false;
  }
  parent[parts.back()] = replacement;
  collection->resources[resourceIndex] = resource;
  publishCollection(root, *collection);
  return executeYamlCommand("Change Frames alternative", emitYaml(root));
}

bool ManifestWorkspace::deleteResource(std::string const& resourceNamespace,
                                       std::string const& name) {
  auto root = YAML::Load(canonicalYaml());
  if (!resourceNamespace.empty() &&
      namespaceCount(root, resourceNamespace) != 1U) {
    setFailure("Resource namespace was not found uniquely; duplicate namespaces "
               "are ambiguous.");
    return false;
  }
  auto collection = findResourceCollection(root, resourceNamespace);
  if (!collection) {
    setFailure("Resource namespace was not found.");
    return false;
  }
  if (resourceCount(*collection, name) != 1U) {
    setFailure("Resource '" + name +
               "' was not found uniquely; duplicate identities are ambiguous.");
    return false;
  }
  auto index = findResourceIndex(*collection, name);
  ResourceIdentity const target{resourceNamespace, name};
  std::vector<ResourceIdentity> removed{target};
  for (auto const& dependency :
       standardDependencyItems(collection->resources[*index])) {
    if (scalar(dependency, "ref").empty() &&
        !scalar(dependency, "type").empty()) {
      removed.push_back({resourceNamespace, resourceIdentity(dependency)});
    }
  }
  std::vector<std::string> incoming;
  forEachStandardReference(
      root, [&](ResourceIdentity const& source, ResourceIdentity const& referenced) {
        bool const targetRemoved =
            std::find(removed.begin(), removed.end(), referenced) != removed.end();
        bool const sourceRemoved =
            std::find(removed.begin(), removed.end(), source) != removed.end();
        if (targetRemoved && !sourceRemoved) {
          incoming.push_back(qualifiedIdentity(source) + " -> " +
                             qualifiedIdentity(referenced));
        }
      });
  if (!incoming.empty()) {
    std::ostringstream message;
    message << "Resource '" << qualifiedIdentity(target)
            << "' cannot be deleted; known incoming reference(s): ";
    for (std::size_t incomingIndex = 0; incomingIndex < incoming.size();
         ++incomingIndex) {
      if (incomingIndex != 0U) message << ", ";
      message << incoming[incomingIndex];
    }
    setFailure(message.str());
    return false;
  }

  eraseCollectionItem(collection->resources, *index);
  bool const removesNamespace = collection->named && collection->resources.empty();
  if (removesNamespace) {
    eraseCollectionItem(collection->namespaces, collection->namespaceIndex);
    setCollection(root["Resources"], "Namespace", collection->namespaces);
  } else {
    publishCollection(root, *collection);
  }
  return executeYamlCommand(removesNamespace
                                ? "Delete Resource and namespace"
                                : "Delete Resource",
                            emitYaml(root));
}

bool ManifestWorkspace::canUndo() const noexcept { return mCommands.canUndo(); }
bool ManifestWorkspace::canRedo() const noexcept { return mCommands.canRedo(); }
std::string const* ManifestWorkspace::undoName() const noexcept {
  return mCommands.undoName();
}
std::string const* ManifestWorkspace::redoName() const noexcept {
  return mCommands.redoName();
}
bool ManifestWorkspace::undo() {
  if (!mCommands.undo()) return false;
  mOperationDiagnostic.clear();
  mDraft.reset();
  return true;
}
bool ManifestWorkspace::redo() {
  if (!mCommands.redo()) return false;
  mOperationDiagnostic.clear();
  mDraft.reset();
  return true;
}
void ManifestWorkspace::endContinuousEdit() { mCommands.endCoalescing(); }

bool ManifestWorkspace::applyCommittedYaml(std::string const& yaml) {
  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::parse(yaml, mPath.empty() ? "Untitled Resource Manifest"
                                                          : displayPath(mPath)));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Resource edit was rejected: " + validationMessage(validation));
    return false;
  }
  mDocument = std::move(candidate);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  refreshSemanticDiagnostics();
  if (mNamespaceDraft) validateNamespaceDraft();
  return true;
}

bool ManifestWorkspace::executeYamlCommand(std::string name, std::string yaml,
                                           std::string mergeKey,
                                           bool continuous) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  auto candidate = ResourceManifestDocument::parse(yaml, "Resource edit preview");
  auto validation = candidate.validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Resource edit was rejected: " + validationMessage(validation));
    return false;
  }
  auto after = candidate.serializeCanonical();
  auto candidateSemantic = semanticDiagnosticsFor(
      YAML::Load(after), mCatalog, mBaseDirectory);
  std::multiset<std::string> preservedErrors;
  for (auto const& diagnostic : mSemanticDiagnostics) {
    if (diagnostic.severity == SemanticDiagnosticSeverity::error)
      preservedErrors.insert(semanticKey(diagnostic));
  }
  for (auto const& diagnostic : candidateSemantic) {
    if (diagnostic.severity != SemanticDiagnosticSeverity::error) continue;
    auto found = preservedErrors.find(semanticKey(diagnostic));
    if (found != preservedErrors.end()) {
      preservedErrors.erase(found);
      continue;
    }
    setFailure("Resource edit was rejected because it would introduce an "
               "unrelated semantic error: " + diagnostic.message);
    return false;
  }

  auto before = canonicalYaml();
  if (after == before) {
    if (!continuous) mCommands.endCoalescing();
    return true;
  }
  try {
    mCommands.execute(std::make_unique<ReplaceYamlCommand>(
                          std::move(name), std::move(mergeKey),
                          [this](std::string const& value) {
                            return applyCommittedYaml(value);
                          },
                          std::move(before), std::move(after)),
                      continuous);
    if (!continuous) mCommands.endCoalescing();
    return true;
  } catch (std::exception const& error) {
    setFailure(error.what());
    return false;
  }
}

std::optional<std::string> ManifestWorkspace::portableSelectedFile(
    fs::path const& selectedFile, ResourceForm const& form) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return {};
  }
  std::error_code error;
  if (!fs::is_regular_file(selectedFile, error)) {
    setFailure("Selected Resource file does not exist as a regular file: " +
               displayPath(selectedFile) +
               (error ? ": " + error.message() : std::string{}));
    return {};
  }
  auto canonicalTarget = fs::canonical(selectedFile, error);
  if (error) {
    setFailure("Could not canonicalize selected Resource file '" +
               displayPath(selectedFile) + "': " + error.message());
    return {};
  }
  if (!containedBy(mBaseDirectory, canonicalTarget)) {
    setFailure("Selected Resource file must be beneath the canonical base directory; "
               "traversal and link escapes are not allowed.");
    return {};
  }
  auto extension = canonicalTarget.extension().string();
  if (!extension.empty() && extension.front() == '.') extension.erase(0, 1U);
  if (std::none_of(form.fileExtensions.begin(), form.fileExtensions.end(),
                   [&](auto const& allowed) {
                     return sameExtension(extension, allowed);
                   })) {
    setFailure("Selected Resource file does not match the extensions declared "
               "by its Resource Schema annotation.");
    return {};
  }
  auto relative = canonicalTarget.lexically_relative(mBaseDirectory);
  if (relative.empty() || relative.is_absolute() ||
      std::any_of(relative.begin(), relative.end(), [](fs::path const& part) {
        return part == "..";
      })) {
    setFailure("Selected Resource file cannot be represented as a safe relative path.");
    return {};
  }
  return displayPath(relative);
}

void ManifestWorkspace::refreshSemanticDiagnostics() {
  if (!mDocument) {
    mSemanticDiagnostics.clear();
    return;
  }
  mSemanticDiagnostics = semanticDiagnosticsFor(
      YAML::Load(canonicalYaml()), mCatalog, mBaseDirectory);
}

void ManifestWorkspace::setFailure(std::string message) {
  mOperationDiagnostic = std::move(message);
}

bool runDocumentTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-document-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    fs::create_directories(root / "assets");
    ManifestWorkspace workspace;
    if (!workspace.createNew(root / "assets") || !workspace.hasDocument() ||
        workspace.hasPath() || !workspace.dirty()) {
      return fail("New did not create a valid empty unsaved Resource Manifest.");
    }
    if (workspace.canonicalYaml() != "Resources:\n  {}\n") {
      return fail("New Resource Manifest did not have canonical empty content.");
    }
    auto yamlPath = root / "manifest.YML";
    if (!workspace.saveAs(yamlPath) || !workspace.hasPath() || workspace.dirty()) {
      return fail("Save As did not save a YAML Resource Manifest.");
    }
    if (readBytes(yamlPath) != "Resources:\n  {}\n") {
      return fail("Save As did not emit canonical Resource Manifest YAML.");
    }

    fs::path originalPath = workspace.path();
    auto invalidPath = root / "invalid.yaml";
    std::ofstream(invalidPath) << "Resources:\n  unexpected: true\n";
    if (workspace.open(invalidPath) || workspace.path() != originalPath ||
        workspace.operationDiagnostic().empty()) {
      return fail("Structurally invalid Open replaced the current document.");
    }
    auto multiDocumentPath = root / "multi-document.yaml";
    std::ofstream(multiDocumentPath) << "Resources: {}\n---\nResources: {}\n";
    if (workspace.open(multiDocumentPath) || workspace.path() != originalPath) {
      return fail("Open accepted multiple YAML documents or replaced the current document.");
    }
    if (workspace.saveAs(root / "manifest.txt") ||
        fs::exists(root / "manifest.txt")) {
      return fail("Save As accepted a non-YAML extension.");
    }

    std::ofstream(root / "notes.txt") << "notes";
    std::ofstream(yamlPath, std::ios::binary | std::ios::trunc)
        << "Resources:\n  Resource:\n    type: TextFile\n    name: notes\n"
           "    location: notes.txt\n";
    if (!workspace.open(yamlPath) || workspace.dirty()) {
      return fail("Open did not accept a structurally valid single-document manifest.");
    }
    auto expected = workspace.canonicalYaml();
    if (!workspace.save() || readBytes(yamlPath) != expected) {
      return fail("Save did not atomically replace the destination with canonical YAML.");
    }
    for (auto const& entry : fs::directory_iterator(root)) {
      if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
        return fail("Atomic Save left a sibling temporary file behind.");
      }
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runAuthoringTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-authoring-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    auto base = root / "base";
    fs::create_directories(base / "nested");
    fs::create_directories(root / "outside");
    std::vector<std::pair<std::string, fs::path>> fixtures{
        {"TextFile", base / "notes.txt"},
        {"XmlFile", base / "layout.xml"},
        {"Shader", base / "effect.vert"},
        {"AudioBank", base / "audio.bank"},
        {"Image", base / "nested" / "image.png"}};
    for (auto const& [type, path] : fixtures) {
      static_cast<void>(type);
      std::ofstream(path, std::ios::binary) << "fixture";
    }
    auto outside = root / "outside" / "escape.txt";
    std::ofstream(outside) << "outside";

    ManifestWorkspace workspace;
    if (!workspace.createNew(base) || workspace.resourceForms().size() != 9U) {
      return fail("The nine built-in authored forms were not available.");
    }
    for (auto const& form : workspace.resourceForms()) {
      if (!form.composite &&
          (form.fileProperty != "location" || form.fileKind.empty() ||
           form.fileExtensions.empty())) {
        return fail("A built-in file property did not expose selector annotations.");
      }
    }

    auto empty = workspace.canonicalYaml();
    if (!workspace.beginDraft("TextFile") || workspace.commitDraft() ||
        workspace.canonicalYaml() != empty || workspace.canUndo()) {
      return fail("An invalid draft entered the document or command history.");
    }
    workspace.setDraftName("Bad/Name");
    if (workspace.draftValid()) return fail("A slash-containing draft name was accepted.");
    workspace.setDraftName("Notes");
    if (workspace.selectDraftFile(root / "missing.txt") ||
        workspace.selectDraftFile(base / ".." / "outside" / "escape.txt")) {
      return fail("A nonexistent file or traversal escape was accepted.");
    }

    std::error_code linkError;
    fs::create_symlink(outside, base / "linked.txt", linkError);
    if (!linkError && workspace.selectDraftFile(base / "linked.txt")) {
      return fail("A symbolic-link escape was accepted.");
    }
    if (!workspace.selectDraftFile(fixtures.front().second) ||
        !workspace.commitDraft()) {
      return fail("A valid TextFile draft was not committed.");
    }

    for (std::size_t index = 1; index < fixtures.size(); ++index) {
      auto const& [type, path] = fixtures[index];
      if (!workspace.beginDraft(type)) return fail("Could not begin a built-in draft.");
      workspace.setDraftName(type);
      if (!workspace.selectDraftFile(path) || !workspace.commitDraft()) {
        return fail("Could not author built-in Resource Type " + type + ": " +
                    workspace.operationDiagnostic());
      }
    }
    auto summaries = workspace.resources();
    if (summaries.size() != fixtures.size() ||
        std::any_of(summaries.begin(), summaries.end(),
                    [](ResourceSummary const& item) {
                      return !item.explicitName || !item.editable;
                    })) {
      return fail("Authored Resources were not explicit editable tree entries.");
    }
    auto authored = workspace.canonicalYaml();
    for (auto const& [type, path] : fixtures) {
      static_cast<void>(path);
      if (authored.find("\"" + type + "\"") == std::string::npos) {
        return fail("Canonical output omitted built-in Resource Type " + type + ".");
      }
    }
    if (authored.find("nested/image.png") == std::string::npos ||
        authored.find(displayPath(base)) != std::string::npos ||
        authored.find('\\') != std::string::npos) {
      return fail("Selected files were not serialized as portable relative paths.");
    }

    if (!workspace.beginDraft("TextFile")) return fail("Could not begin duplicate draft.");
    workspace.setDraftName("Notes");
    workspace.selectDraftFile(fixtures.front().second);
    if (workspace.commitDraft()) return fail("A duplicate Resource name was accepted.");
    workspace.cancelDraft();

    if (!workspace.renameResource({}, "Notes", "Note", true) ||
        !workspace.renameResource({}, "Note", "Readme", true) ||
        !workspace.undo() || workspace.canonicalYaml() != authored ||
        !workspace.redo() ||
        workspace.canonicalYaml().find("\"Readme\"") == std::string::npos) {
      return fail("Continuous rename did not coalesce into observable undo/redo.");
    }
    workspace.endContinuousEdit();

    auto replacement = base / "replacement.txt";
    std::ofstream(replacement) << "replacement";
    auto beforeFile = workspace.canonicalYaml();
    if (!workspace.setResourceFile({}, "Readme", replacement) ||
        workspace.canonicalYaml() == beforeFile || !workspace.undo() ||
        workspace.canonicalYaml() != beforeFile || !workspace.redo()) {
      return fail("File-property edit did not support undo and redo.");
    }

    if (!workspace.setResourceOption({}, "Image", "filtering", "linear") ||
        workspace.canonicalYaml().find("\"filtering\"") == std::string::npos ||
        !workspace.undo() ||
        workspace.canonicalYaml().find("\"filtering\"") != std::string::npos ||
        !workspace.redo()) {
      return fail("Schema-generated Image option did not support undo and redo.");
    }

    auto beforeDelete = workspace.canonicalYaml();
    if (!workspace.deleteResource({}, "XmlFile") ||
        workspace.resources().size() != 4U || !workspace.undo() ||
        workspace.canonicalYaml() != beforeDelete || !workspace.redo() ||
        workspace.resources().size() != 4U) {
      return fail("Delete did not support command-level undo and redo.");
    }
    if (!workspace.undo()) return fail("Could not restore deleted Resource.");

    auto output = root / "authored.yaml";
    if (!workspace.saveAs(output)) return fail("Could not save authored manifest.");
    auto saved = ResourceManifestDocument::load(output);
    if (!saved.validate(ResourceSchemaCatalog::builtIn().snapshot()).valid() ||
        readBytes(output) != workspace.canonicalYaml()) {
      return fail("Authored canonical output was not externally valid and stable.");
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runOrganizationTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-organization-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    fs::create_directories(root / "base");
    auto sourceFile = root / "base" / "source.txt";
    std::ofstream(sourceFile) << "source";

    ManifestWorkspace drafts;
    if (!drafts.createNew(root / "base") || drafts.namespaces().size() != 1U ||
        !drafts.namespaces().front().isDefault ||
        drafts.renameNamespace({}, "Renamed") || drafts.deleteNamespace({})) {
      return fail("The permanent default namespace was not presented or protected.");
    }
    auto empty = drafts.canonicalYaml();
    if (!drafts.beginNamespaceDraft() || drafts.namespaceDraftValid()) {
      return fail("An empty named namespace draft was accepted.");
    }
    drafts.setNamespaceDraftName("Nested/Invalid");
    if (drafts.namespaceDraftValid()) {
      return fail("A namespace containing the qualifier separator was accepted.");
    }
    drafts.setNamespaceDraftName("Group");
    if (!drafts.namespaceDraftValid() || drafts.canonicalYaml() != empty ||
        drafts.canUndo() || drafts.namespaces().back().name != "Group" ||
        !drafts.namespaces().back().draft) {
      return fail("A named namespace did not remain an uncommitted tree draft.");
    }
    if (!drafts.beginDraft("TextFile", "Group")) {
      return fail("Could not begin the first Resource in a namespace draft.");
    }
    drafts.setDraftName("Alpha");
    if (!drafts.selectDraftFile(sourceFile) || !drafts.commitDraft() ||
        drafts.namespaceDraft() || drafts.namespaces().size() != 2U ||
        !drafts.canUndo()) {
      return fail("The first Resource did not commit its namespace atomically.");
    }
    auto groupYaml = drafts.canonicalYaml();
    if (!drafts.undo() || drafts.canonicalYaml() != empty ||
        drafts.namespaces().size() != 1U || !drafts.redo() ||
        drafts.canonicalYaml() != groupYaml) {
      return fail("Namespace-and-first-Resource undo/redo was not atomic.");
    }

    if (!drafts.beginNamespaceDraft())
      return fail("Could not begin a duplicate namespace draft.");
    drafts.setNamespaceDraftName("Group");
    if (drafts.namespaceDraftValid()) {
      return fail("A duplicate namespace name was accepted.");
    }
    drafts.cancelNamespaceDraft();

    auto addFileResource = [&](std::string const& name) {
      if (!drafts.beginDraft("TextFile")) return false;
      drafts.setDraftName(name);
      return drafts.selectDraftFile(sourceFile) && drafts.commitDraft();
    };
    if (!addFileResource("Alpha") || !addFileResource("Beta")) {
      return fail("Namespace-unique Resource identities were not accepted.");
    }
    if (!drafts.beginDraft("TextFile"))
      return fail("Could not begin a duplicate Resource draft.");
    drafts.setDraftName("Alpha");
    drafts.selectDraftFile(sourceFile);
    if (drafts.commitDraft()) {
      return fail("A duplicate Resource identity was accepted.");
    }
    drafts.cancelDraft();

    if (!drafts.reorderResource({}, "Beta", 0U)) {
      return fail("A valid Resource reorder was rejected.");
    }
    auto ordered = drafts.resources();
    std::vector<std::string> defaultOrder;
    for (auto const& resource : ordered) {
      if (resource.resourceNamespace.empty()) defaultOrder.push_back(resource.name);
    }
    if (defaultOrder != std::vector<std::string>{"Beta", "Alpha"}) {
      return fail("Resource order was not preserved after an explicit reorder: " +
                  drafts.canonicalYaml());
    }
    auto reordered = drafts.canonicalYaml();
    if (!drafts.undo() || drafts.canonicalYaml() == reordered || !drafts.redo() ||
        drafts.canonicalYaml() != reordered) {
      return fail("Resource reorder did not support undo and redo.");
    }
    if (drafts.moveResource("Group", "Alpha", {}) ||
        drafts.renameNamespace("Group", "Nested/Invalid") ||
        drafts.renameResource({}, "Alpha", "Nested/Invalid")) {
      return fail("A duplicate move or qualifier-containing rename was accepted.");
    }

    if (!drafts.beginNamespaceDraft())
      return fail("Could not begin a move-target namespace draft.");
    drafts.setNamespaceDraftName("Moved");
    auto beforeDraftMove = drafts.canonicalYaml();
    if (!drafts.moveResource({}, "Beta", "Moved") || drafts.namespaceDraft() ||
        drafts.canonicalYaml() == beforeDraftMove || !drafts.undo() ||
        drafts.canonicalYaml() != beforeDraftMove || !drafts.redo()) {
      return fail("Moving into a namespace draft was not one undoable command.");
    }

    auto referencesPath = root / "references.yaml";
    std::ofstream(referencesPath) << R"(Resources:
  Resource:
    - type: TextFile
      name: Root
      location: source.txt
    - type: Custom
      name: RootConsumer
      DependentResources:
        DependentResource:
          ref: Shared/Target
  Namespace:
    - name: Shared
      Resource:
        - type: TextFile
          name: Target
          location: source.txt
        - type: Custom
          name: LocalConsumer
          DependentResources:
            DependentResource:
              ref: Target
    - name: Other
      Resource:
        type: Custom
        name: Observer
        DependentResources:
          DependentResource:
            - ref: Shared/Target
            - ref: Shared/LocalConsumer
            - ref: /Root
)";
    ManifestWorkspace references;
    if (!references.open(referencesPath)) {
      return fail("Could not open the reference-rewrite fixture: " +
                  references.operationDiagnostic());
    }
    auto const referenceNamespaces = references.namespaces();
    if (referenceNamespaces.size() != 3U ||
        referenceNamespaces[1].name != "Shared" ||
        referenceNamespaces[2].name != "Other") {
      return fail("Named namespace source order was not preserved in the tree.");
    }
    if (!references.renameResource("Shared", "Target", "Renamed")) {
      return fail("Referenced Resource rename was rejected.");
    }
    auto renamed = references.canonicalYaml();
    if (renamed.find("Shared/Renamed") == std::string::npos ||
        renamed.find("ref: \"Renamed\"") == std::string::npos ||
        references.deleteResource("Shared", "Renamed")) {
      return fail("Resource rename did not rewrite incoming standard references or "
                  "deletion was not blocked.");
    }
    if (!references.moveResource("Shared", "Renamed", "Other")) {
      return fail("Referenced Resource move was rejected: " +
                  references.operationDiagnostic());
    }
    auto moved = references.canonicalYaml();
    if (moved.find("Other/Renamed") == std::string::npos ||
        moved.find("ref: \"Renamed\"") == std::string::npos ||
        moved.find("Shared/Renamed") != std::string::npos) {
      return fail("Move did not use qualified cross-namespace and unqualified "
                  "same-namespace references.");
    }
    if (!references.moveResource({}, "Root", "Other")) {
      return fail("Moving a default-namespace reference target was rejected.");
    }
    auto movedRoot = references.canonicalYaml();
    if (movedRoot.find("ref: \"/Root\"") != std::string::npos ||
        movedRoot.find("ref: \"Root\"") == std::string::npos) {
      return fail("Moving into the reference owner's namespace did not unqualify "
                  "the reference.");
    }
    if (!references.moveResource("Other", "Root", {})) {
      return fail("Moving a reference target back to the default namespace failed.");
    }
    auto defaultRoot = references.canonicalYaml();
    if (defaultRoot.find("ref: \"/Root\"") == std::string::npos ||
        !references.undo() || references.canonicalYaml() != movedRoot ||
        !references.redo() || references.canonicalYaml() != defaultRoot) {
      return fail("A cross-namespace reference to the default namespace was not "
                  "qualified and undoable.");
    }
    auto beforeNamespaceRename = references.canonicalYaml();
    if (!references.renameNamespace("Shared", "Common")) {
      return fail("Referenced namespace rename was rejected.");
    }
    auto renamedNamespace = references.canonicalYaml();
    if (renamedNamespace.find("Common/LocalConsumer") == std::string::npos ||
        renamedNamespace.find("Shared/LocalConsumer") != std::string::npos ||
        !references.undo() ||
        references.canonicalYaml() != beforeNamespaceRename ||
        !references.redo() ||
        references.canonicalYaml() != renamedNamespace) {
      return fail("Namespace rename and reference rewrites were not one undoable "
                  "transaction.");
    }
    if (references.deleteNamespace("Other")) {
      return fail("A populated namespace with external incoming references was deleted.");
    }

    auto deletionPath = root / "deletion.yaml";
    std::ofstream(deletionPath) << R"(Resources:
  Namespace:
    - name: Trash
      Resource:
        - type: TextFile
          name: One
          location: source.txt
        - type: Custom
          name: InternalConsumer
          DependentResources:
            DependentResource:
              ref: One
    - name: Solo
      Resource:
        type: TextFile
        name: Only
        location: source.txt
)";
    ManifestWorkspace deletions;
    if (!deletions.open(deletionPath)) {
      return fail("Could not open the namespace-deletion fixture.");
    }
    auto beforePopulatedDelete = deletions.canonicalYaml();
    if (!deletions.deleteNamespace("Trash") || !deletions.undo() ||
        deletions.canonicalYaml() != beforePopulatedDelete ||
        !deletions.redo()) {
      return fail("Explicit populated namespace deletion was not undoable.");
    }
    if (!deletions.undo()) return fail("Could not restore populated namespace.");
    auto beforeFinalMove = deletions.canonicalYaml();
    if (!deletions.moveResource("Solo", "Only", {}) ||
        !deletions.undo() || deletions.canonicalYaml() != beforeFinalMove ||
        !deletions.redo() || !deletions.undo()) {
      return fail("Moving a final Resource did not remove and restore its namespace "
                  "as one command.");
    }
    auto beforeFinalDelete = deletions.canonicalYaml();
    if (!deletions.deleteResource("Solo", "Only")) {
      return fail("Final-Resource deletion was rejected.");
    }
    auto const afterFinalDeleteNamespaces = deletions.namespaces();
    if (std::any_of(afterFinalDeleteNamespaces.begin(),
                    afterFinalDeleteNamespaces.end(),
                    [](NamespaceSummary const& item) {
                      return item.name == "Solo";
                    }) ||
        !deletions.undo() || deletions.canonicalYaml() != beforeFinalDelete ||
        !deletions.redo()) {
      return fail("Final-Resource deletion did not remove and restore its namespace "
                  "as one command.");
    }

    auto duplicatePath = root / "duplicates.yaml";
    std::ofstream(duplicatePath) << R"(Resources:
  Resource:
    - {type: TextFile, name: Duplicate, location: source.txt}
    - {type: TextFile, name: Duplicate, location: source.txt}
  Namespace:
    - {name: Repeated, Resource: {type: TextFile, name: A, location: source.txt}}
    - {name: Repeated, Resource: {type: TextFile, name: B, location: source.txt}}
)";
    ManifestWorkspace duplicates;
    if (!duplicates.open(duplicatePath) ||
        duplicates.renameResource({}, "Duplicate", "Unique") ||
        duplicates.deleteResource({}, "Duplicate") ||
        duplicates.renameNamespace("Repeated", "Unique") ||
        duplicates.moveResource("Repeated", "A", {})) {
      return fail("Commands did not reject ambiguous duplicate identities.");
    }

    auto nestedPath = root / "nested.yaml";
    std::ofstream(nestedPath) << R"(Resources:
  Namespace:
    name: Outer
    Namespace:
      name: Inner
      Resource: {type: TextFile, name: A, location: source.txt}
    Resource: {type: TextFile, name: B, location: source.txt}
)";
    ManifestWorkspace nested;
    if (nested.open(nestedPath)) {
      return fail("A nested namespace was accepted.");
    }

    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runDependencyAuthoringTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-dependency-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    fs::create_directories(root / "base");
    std::ofstream(root / "base" / "image.png", std::ios::binary) << "image";
    std::ofstream(root / "base" / "replacement.png", std::ios::binary)
        << "replacement";
    std::ofstream(root / "base" / "shader.vert") << "shader";
    auto manifest = root / "dependencies.yaml";
    std::ofstream(manifest) << R"(Resources:
  Resource:
    - {type: Image, name: RootImage, location: image.png}
    - {type: Shader, name: RootShader, location: shader.vert}
    - type: ImageSet
      name: Atlas
      DependentResources:
        DependentResource: {id: Image, ref: MissingImage}
      Definitions:
        Definition:
          Images:
            Image: {name: Pixel, x: 0, y: 0, width: 1, height: 1}
    - type: Custom
      name: CycleA
      DependentResources:
        DependentResource: {ref: CycleB}
    - type: Custom
      name: CycleB
      DependentResources:
        DependentResource: {ref: MissingCycleTarget}
    - type: Custom
      name: Optional
      DependentResources:
        DependentResource: {ref: MissingOptional}
    - type: Custom
      name: InlineOwner
      DependentResources:
        DependentResource:
          id: Texture
          type: Image
          name: InlineImage
          location: image.png
    - type: Custom
      name: InlineObserver
      DependentResources:
        DependentResource: {ref: InlineImage}
  Namespace:
    name: Assets
    Resource: {type: Image, name: SharedImage, location: image.png}
)";

    ManifestWorkspace workspace;
    if (!workspace.open(manifest)) {
      return fail("Could not open dependency fixture: " +
                  workspace.operationDiagnostic());
    }

    auto atlasSelectors = workspace.resourceReferences({}, "Atlas");
    if (atlasSelectors.size() != 1U || !atlasSelectors.front().missing ||
        atlasSelectors.front().clearable ||
        atlasSelectors.front().allowedResourceTypes !=
            std::vector<std::string>{"Image"}) {
      return fail("Required missing Image dependency metadata was not preserved.");
    }
    auto const& atlasChoices = atlasSelectors.front().choices;
    auto shared = std::find_if(atlasChoices.begin(), atlasChoices.end(),
                               [](auto const& choice) {
                                 return choice.qualifiedIdentity ==
                                        "Assets/SharedImage";
                               });
    if (shared == atlasChoices.end() || shared->disabled ||
        std::any_of(atlasChoices.begin(), atlasChoices.end(),
                    [](auto const& choice) {
                      return choice.resourceType == "Shader";
                    })) {
      return fail("Reference choices were not qualified and filtered by Resource Type.");
    }
    if (!workspace.setResourceReference(
            {}, "Atlas", atlasSelectors.front().dependencyIndex,
            std::pair{std::string("Assets"), std::string("SharedImage")}) ||
        workspace.canonicalYaml().find("Assets/SharedImage") ==
            std::string::npos) {
      return fail("A compatible qualified Resource selection was not committed.");
    }

    auto cycleSelectors = workspace.resourceReferences({}, "CycleB");
    if (cycleSelectors.empty()) {
      return fail("Cycle fixture did not expose its Resource selector.");
    }
    auto cycleChoice = std::find_if(
        cycleSelectors.front().choices.begin(),
        cycleSelectors.front().choices.end(), [](auto const& choice) {
          return choice.name == "CycleA";
        });
    if (cycleChoice == cycleSelectors.front().choices.end() ||
        !cycleChoice->disabled ||
        cycleChoice->reason.find("cycle") == std::string::npos) {
      return fail("A cycle-producing choice was not present and disabled.");
    }
    if (std::any_of(cycleSelectors.front().choices.begin(),
                    cycleSelectors.front().choices.end(),
                    [](auto const& choice) {
                      return choice.name == "CycleB";
                    })) {
      return fail("A Resource selector offered its owner as a new target.");
    }
    if (workspace.setResourceReference(
            {}, "CycleB", cycleSelectors.front().dependencyIndex,
            std::pair{std::string{}, std::string("CycleA")})) {
      return fail("A cycle-producing Resource selection was accepted.");
    }

    auto optionalSelectors = workspace.resourceReferences({}, "Optional");
    if (optionalSelectors.size() != 1U || !optionalSelectors.front().missing ||
        !optionalSelectors.front().clearable ||
        !workspace.setResourceReference({}, "Optional",
                                        optionalSelectors.front().dependencyIndex,
                                        {})) {
      return fail("An unresolved optional reference could not be displayed and cleared.");
    }
    if (!workspace.resourceReferences({}, "Optional").empty()) {
      return fail("Clearing an optional reference did not remove its dependency entry.");
    }

    auto inlineResources = workspace.inlineResources({}, "InlineOwner");
    if (inlineResources.size() != 1U ||
        inlineResources.front().name != "InlineImage" ||
        inlineResources.front().resourceType != "Image" ||
        !inlineResources.front().editable) {
      return fail("Inline Resource was not exposed beneath its owner for editing.");
    }
    auto observerSelectors = workspace.resourceReferences({}, "InlineObserver");
    if (observerSelectors.size() != 1U ||
        observerSelectors.front().choices.empty() ||
        !observerSelectors.front().choices.front().inlineResource ||
        !observerSelectors.front().choices.front().selected) {
      return fail("An existing reference to an inline Resource was not represented.");
    }
    auto ownerSelectors = workspace.resourceReferences({}, "CycleB");
    if (std::any_of(ownerSelectors.front().choices.begin(),
                    ownerSelectors.front().choices.end(),
                    [](auto const& choice) {
                      return choice.name == "InlineImage";
                    })) {
      return fail("A new Resource selector offered an inline Resource target.");
    }
    if (workspace.moveResource({}, "InlineImage", "Assets")) {
      return fail("An inline Resource could be moved independently.");
    }

    auto beforeInlineEdit = workspace.canonicalYaml();
    if (!workspace.renameInlineResource({}, "InlineOwner",
                                        inlineResources.front().dependencyIndex,
                                        "RenamedInline") ||
        workspace.canonicalYaml().find("ref: \"RenamedInline\"") ==
            std::string::npos ||
        !workspace.setInlineResourceFile(
            {}, "InlineOwner", inlineResources.front().dependencyIndex,
            root / "base" / "replacement.png") ||
        !workspace.setInlineResourceOption(
            {}, "InlineOwner", inlineResources.front().dependencyIndex,
            "filtering", "linear")) {
      return fail("Inline Resource rename or property editing failed: " +
                  workspace.operationDiagnostic());
    }
    auto editedInline = workspace.canonicalYaml();
    if (editedInline == beforeInlineEdit ||
        editedInline.find("replacement.png") == std::string::npos ||
        editedInline.find("filtering") == std::string::npos) {
      return fail("Inline Resource edits were not serialized.");
    }
    auto incoming = workspace.incomingReferences({}, "RenamedInline");
    auto diagnostics = workspace.dependencyDiagnostics();
    if (incoming != std::vector<std::string>{"InlineObserver"} ||
        std::none_of(diagnostics.begin(), diagnostics.end(),
                     [](auto const& diagnostic) {
                       return !diagnostic.error &&
                              diagnostic.message.find("InlineObserver") !=
                                  std::string::npos;
                     }) ||
        workspace.deleteResource({}, "InlineOwner") ||
        workspace.operationDiagnostic().find("InlineObserver") ==
            std::string::npos) {
      return fail("Known incoming references were not listed or did not block deletion.");
    }

    auto beforePromotion = workspace.canonicalYaml();
    if (!workspace.promoteInlineResource(
            {}, "InlineOwner", inlineResources.front().dependencyIndex,
            "Assets") ||
        workspace.inlineResources({}, "InlineOwner").size() != 0U) {
      return fail("Inline Resource promotion failed: " +
                  workspace.operationDiagnostic());
    }
    auto promoted = workspace.canonicalYaml();
    auto promotedResources = workspace.resources();
    if (promoted.find("id: \"Texture\"") == std::string::npos ||
        promoted.find("ref: \"Assets/RenamedInline\"") == std::string::npos ||
        std::none_of(promotedResources.begin(), promotedResources.end(),
                     [](auto const& resource) {
                       return resource.resourceNamespace == "Assets" &&
                              resource.name == "RenamedInline" &&
                              resource.resourceType == "Image";
                     }) ||
        !workspace.undo() || workspace.canonicalYaml() != beforePromotion ||
        !workspace.redo() || workspace.canonicalYaml() != promoted) {
      return fail("Promotion was not one undoable Resource-and-reference command.");
    }
    auto afterPromotionSelectors =
        workspace.resourceReferences({}, "InlineObserver");
    if (afterPromotionSelectors.empty() ||
        std::none_of(afterPromotionSelectors.front().choices.begin(),
                     afterPromotionSelectors.front().choices.end(),
                     [](auto const& choice) {
                       return choice.resourceNamespace == "Assets" &&
                              choice.name == "RenamedInline" &&
                              !choice.inlineResource && choice.selected;
                     })) {
      return fail("A promoted Resource did not become a normal selector target.");
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runCompositeAuthoringTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-composite-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    fs::create_directories(root / "base");
    std::ofstream(root / "base" / "image.png", std::ios::binary) << "image";
    auto manifest = root / "composites.yaml";
    std::ofstream(manifest) << R"(Resources:
  Resource:
    - {type: Image, name: Texture, location: base/image.png}
    - type: ImageSet
      name: Atlas
      DependentResources:
        DependentResource: {id: Image, ref: Texture}
      Definitions:
        Definition:
          Images:
            Image: {name: Pixel, x: "0", y: 0, width: "1", height: 1}
    - type: AnimationSet
      name: Motion
      DependentResources:
        DependentResource: {id: Image, ref: Atlas}
      Definitions:
        Definition:
          Animations:
            Animation:
              name: Idle
              loopStyle: forwards
              Frames:
                time: "1.0"
                Frame:
                  image: Pixel
                  time: 1
)";
    ManifestWorkspace workspace;
    if (!workspace.open(manifest)) {
      return fail("Could not open scalar-compatible composite fixture: " +
                  workspace.operationDiagnostic());
    }
    auto forms = workspace.resourceForms();
    auto imageSetForm = std::find_if(forms.begin(), forms.end(), [](auto const& form) {
      return form.resourceType == "ImageSet" && form.composite;
    });
    auto animationSetForm =
        std::find_if(forms.begin(), forms.end(), [](auto const& form) {
          return form.resourceType == "AnimationSet" && form.composite;
        });
    if (imageSetForm == forms.end() || animationSetForm == forms.end()) {
      return fail("Catalogued ImageSet and AnimationSet forms were not generated.");
    }
    auto scalarCanonical = workspace.canonicalYaml();
    if (scalarCanonical.find("x: \"0\"") == std::string::npos ||
        scalarCanonical.find("time: \"1.0\"") == std::string::npos) {
      return fail("Compatible scalar input types were not preserved canonically.");
    }
    auto atlasSelectors = workspace.resourceReferences({}, "Atlas");
    auto motionSelectors = workspace.resourceReferences({}, "Motion");
    if (atlasSelectors.size() != 1U || motionSelectors.size() != 1U ||
        atlasSelectors.front().allowedResourceTypes !=
            std::vector<std::string>{"Image"} ||
        motionSelectors.front().allowedResourceTypes !=
            std::vector<std::string>{"ImageSet"} ||
        std::any_of(motionSelectors.front().choices.begin(),
                    motionSelectors.front().choices.end(), [](auto const& choice) {
                      return choice.resourceType == "Image";
                    })) {
      return fail("Composite Image dependencies did not use compatible Resource selectors.");
    }

    auto imageItems = workspace.nestedFormItems({}, "Atlas");
    auto image = std::find_if(imageItems.begin(), imageItems.end(), [](auto const& item) {
      return item.kind == NestedCollectionKind::image;
    });
    if (image == imageItems.end() ||
        image->path != "/Definitions/Definition/0/Images/Image/0") {
      return fail("Singleton Image input was not normalized to an indexed form path.");
    }
    auto beforeInvalid = workspace.canonicalYaml();
    if (workspace.setNestedProperty({}, "Atlas", image->path, "width", "0") ||
        workspace.canonicalYaml() != beforeInvalid) {
      return fail("A numeric constraint violation mutated the committed document.");
    }
    if (workspace.setNestedProperty({}, "Atlas", image->path, "name", "") ||
        workspace.canonicalYaml() != beforeInvalid ||
        workspace.structuralDiagnostics().empty()) {
      return fail("A required invalid property edit did not report and reject atomically.");
    }
    bool navigableNestedDiagnostic = false;
    for (std::size_t index = 0; index < workspace.structuralDiagnostics().size();
         ++index) {
      auto navigation = workspace.diagnosticNavigation(index);
      if (navigation && navigation->resourceName == "Atlas" &&
          navigation->instancePath.find("/Images/Image") != std::string::npos) {
        navigableNestedDiagnostic = true;
        break;
      }
    }
    if (!navigableNestedDiagnostic) {
      return fail("A nested structural diagnostic did not retain a navigable instance path.");
    }

    if (!workspace.addNestedItem({}, "Atlas",
                                 "/Definitions/Definition/0/Images/ImageSet",
                                 NestedCollectionKind::imageSet)) {
      return fail("Could not add an image-set definition: " +
                  workspace.operationDiagnostic());
    }
    auto withImageSet = workspace.canonicalYaml();
    auto updatedImages = workspace.nestedFormItems({}, "Atlas");
    auto imageSet = std::find_if(updatedImages.begin(), updatedImages.end(),
                                [](auto const& item) {
                                  return item.kind == NestedCollectionKind::imageSet;
                                });
    if (imageSet == updatedImages.end() ||
        !workspace.duplicateNestedItem({}, "Atlas", imageSet->path)) {
      return fail("Image-set collection duplication failed.");
    }
    auto duplicatedImageSets = workspace.nestedFormItems({}, "Atlas");
    auto imageSetCount = static_cast<std::size_t>(std::count_if(
        duplicatedImageSets.begin(), duplicatedImageSets.end(), [](auto const& item) {
          return item.kind == NestedCollectionKind::imageSet;
        }));
    if (imageSetCount != 2U ||
        !workspace.reorderNestedItem({}, "Atlas",
                                     "/Definitions/Definition/0/Images/ImageSet/1",
                                     0U) ||
        !workspace.removeNestedItem({}, "Atlas",
                                    "/Definitions/Definition/0/Images/ImageSet/1")) {
      return fail("Image-set collection reorder or removal failed.");
    }
    if (!workspace.removeNestedItem({}, "Atlas", image->path)) {
      return fail("An Image could not be removed while an ImageSet remained.");
    }
    auto onlyImageSet = workspace.canonicalYaml();
    if (workspace.removeNestedItem(
            {}, "Atlas", "/Definitions/Definition/0/Images/ImageSet/0") ||
        workspace.canonicalYaml() != onlyImageSet) {
      return fail("The final Images alternative was removed or mutated the document.");
    }
    auto beforeDefinitionRemoval = workspace.canonicalYaml();
    if (workspace.removeNestedItem({}, "Atlas", "/Definitions/Definition/0") ||
        workspace.canonicalYaml() != beforeDefinitionRemoval) {
      return fail("Required default Definition removal was not rejected atomically.");
    }

    auto animationItems = workspace.nestedFormItems({}, "Motion");
    auto animation = std::find_if(animationItems.begin(), animationItems.end(),
                                  [](auto const& item) {
                                    return item.kind == NestedCollectionKind::animation;
                                  });
    auto frame = std::find_if(animationItems.begin(), animationItems.end(),
                              [](auto const& item) {
                                return item.kind == NestedCollectionKind::frame &&
                                       item.path.find("/Frame/") != std::string::npos;
                              });
    if (animation == animationItems.end() || frame == animationItems.end()) {
      return fail("Explicit animation frames were not rendered.");
    }
    if (!workspace.addNestedItem({}, "Motion", animation->path + "/Frames/Frame",
                                 NestedCollectionKind::frame) ||
        !workspace.duplicateNestedItem(
            {}, "Motion", animation->path + "/Frames/Frame/1") ||
        !workspace.reorderNestedItem(
            {}, "Motion", animation->path + "/Frames/Frame/2", 0U) ||
        !workspace.removeNestedItem(
            {}, "Motion", animation->path + "/Frames/Frame/2")) {
      return fail("Explicit frame add, duplicate, reorder, or remove failed.");
    }
    if (!workspace.addNestedItem({}, "Motion", frame->path + "/Tags/Tag",
                                 NestedCollectionKind::tag)) {
      return fail("Could not add a frame tag.");
    }
    auto tagged = workspace.nestedFormItems({}, "Motion");
    auto tag = std::find_if(tagged.begin(), tagged.end(), [](auto const& item) {
      return item.kind == NestedCollectionKind::tag;
    });
    if (tag == tagged.end() ||
        !workspace.duplicateNestedItem({}, "Motion", tag->path) ||
        !workspace.reorderNestedItem(
            {}, "Motion", tag->path.substr(0, tag->path.rfind('/') + 1U) + "1",
            0U) ||
        !workspace.removeNestedItem(
            {}, "Motion", tag->path.substr(0, tag->path.rfind('/') + 1U) + "1")) {
      return fail("Tag collection operations failed.");
    }

    auto beforeInvalidTransition = workspace.canonicalYaml();
    if (workspace.setFramesAlternative({}, "Motion", animation->path + "/Frames",
                                       true, {}) ||
        workspace.canonicalYaml() != beforeInvalidTransition) {
      return fail("An invalid Frames alternative transition mutated the document.");
    }
    if (!workspace.setFramesAlternative({}, "Motion", animation->path + "/Frames",
                                        true, "Strip")) {
      return fail("Could not transition to image-set frames.");
    }
    if (!workspace.addNestedItem({}, "Motion", animation->path + "/Frames/Frame",
                                 NestedCollectionKind::overrideFrame)) {
      return fail("Could not add an image-set frame override.");
    }
    auto overrides = workspace.nestedFormItems({}, "Motion");
    auto overrideItem = std::find_if(overrides.begin(), overrides.end(),
                                    [](auto const& item) {
                                      return item.kind == NestedCollectionKind::overrideFrame &&
                                             item.path.find("/Frame/") != std::string::npos;
                                    });
    if (overrideItem == overrides.end() ||
        !workspace.setNestedProperty({}, "Motion", overrideItem->path, "time",
                                     "0.5") ||
        !workspace.addNestedItem({}, "Motion", overrideItem->path + "/Tags/Tag",
                                 NestedCollectionKind::tag)) {
      return fail("Image-set override properties or tags could not be authored.");
    }
    auto imageSetFramesYaml = workspace.canonicalYaml();
    if (!workspace.setFramesAlternative({}, "Motion", animation->path + "/Frames",
                                        false) ||
        !workspace.undo() || workspace.canonicalYaml() != imageSetFramesYaml ||
        !workspace.redo()) {
      return fail("Frames alternative transition did not support undo and redo.");
    }

    auto beforeAnimationAdd = workspace.canonicalYaml();
    if (!workspace.addNestedItem({}, "Motion",
                                 "/Definitions/Definition/0/Animations/Animation",
                                 NestedCollectionKind::animation) ||
        !workspace.undo() || workspace.canonicalYaml() != beforeAnimationAdd ||
        !workspace.redo()) {
      return fail("Animation collection editing was not undoable.");
    }

    auto beforeDrafts = workspace.canonicalYaml();
    if (!workspace.beginDraft("ImageSet"))
      return fail("Could not begin an ImageSet draft.");
    workspace.setDraftName("NewAtlas");
    auto imageChoices = workspace.draftReferenceChoices();
    auto texture = std::find_if(imageChoices.begin(), imageChoices.end(),
                                [](auto const& choice) {
                                  return choice.name == "Texture" && !choice.disabled;
                                });
    if (texture == imageChoices.end() ||
        !workspace.setDraftReference(texture->resourceNamespace, texture->name) ||
        !workspace.commitDraft()) {
      return fail("A complete ImageSet draft could not select and commit its Image dependency.");
    }
    if (!workspace.beginDraft("AnimationSet"))
      return fail("Could not begin an AnimationSet draft.");
    workspace.setDraftName("NewMotion");
    auto atlasChoices = workspace.draftReferenceChoices();
    auto atlas = std::find_if(atlasChoices.begin(), atlasChoices.end(),
                              [](auto const& choice) {
                                return choice.name == "NewAtlas" && !choice.disabled;
                              });
    if (atlas == atlasChoices.end() ||
        !workspace.setDraftReference(atlas->resourceNamespace, atlas->name) ||
        !workspace.commitDraft()) {
      return fail("A complete AnimationSet draft could not select and commit its ImageSet dependency.");
    }
    auto authored = workspace.canonicalYaml();
    if (!workspace.undo() || !workspace.undo() ||
        workspace.canonicalYaml() != beforeDrafts || !workspace.redo() ||
        !workspace.redo() || workspace.canonicalYaml() != authored) {
      return fail("Composite Resource creation did not support command-level undo and redo.");
    }

    auto output = root / "canonical.yaml";
    if (!workspace.saveAs(output)) {
      return fail("Could not save composite canonical output: " +
                  workspace.operationDiagnostic());
    }
    auto saved = ResourceManifestDocument::load(output);
    if (!saved.validate(ResourceSchemaCatalog::builtIn().snapshot()).valid() ||
        saved.serializeCanonical() != workspace.canonicalYaml() ||
        readBytes(output) != workspace.canonicalYaml()) {
      return fail("Composite canonical output was not valid and deterministic.");
    }
    static_cast<void>(withImageSet);
    static_cast<void>(scalarCanonical);
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runAdvancedAuthoringTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-advanced-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    auto base = root / "base";
    fs::create_directories(base);
    std::ofstream(base / "vertex.vert") << "vertex";
    std::ofstream(base / "fragment.frag") << "fragment";
    std::ofstream(base / "texture.png", std::ios::binary) << "texture";

    ManifestWorkspace workspace;
    if (!workspace.createNew(base) || workspace.resourceForms().size() != 9U) {
      return fail("Program and Material catalogued forms were not available.");
    }
    auto createFile = [&](std::string const& type, std::string const& name,
                          fs::path const& path) {
      if (!workspace.beginDraft(type)) return false;
      workspace.setDraftName(name);
      return workspace.selectDraftFile(path) && workspace.commitDraft();
    };
    if (!createFile("Shader", "VertexShader", base / "vertex.vert") ||
        !createFile("Shader", "FragmentShader", base / "fragment.frag") ||
        !createFile("Image", "TextureImage", base / "texture.png")) {
      return fail("Could not create Program/Material dependencies.");
    }

    if (!workspace.beginDraft("Program")) return fail("Could not begin Program draft.");
    workspace.setDraftName("RenderProgram");
    if (workspace.draftValid() ||
        workspace.draftReferenceChoices("Vertex").size() != 2U ||
        !workspace.setDraftReference("Vertex", {}, "VertexShader") ||
        workspace.draftValid() ||
        !workspace.setDraftReference("Fragment", {}, "FragmentShader") ||
        !workspace.draftValid() || !workspace.commitDraft()) {
      return fail("A Program draft did not require both typed shader dependencies.");
    }
    auto programYaml = workspace.canonicalYaml();
    if (programYaml.find("id: \"Vertex\"") == std::string::npos ||
        programYaml.find("id: \"Fragment\"") == std::string::npos ||
        programYaml.find("MeshSpecification") == std::string::npos) {
      return fail("Program starter output omitted dependencies or its default Definition.");
    }
    auto programItems = workspace.nestedFormItems({}, "RenderProgram");
    auto attribs = std::find_if(programItems.begin(), programItems.end(), [](auto const& item) {
      return item.path.ends_with("/Attribs");
    });
    auto mesh = std::find_if(programItems.begin(), programItems.end(), [](auto const& item) {
      return item.path.ends_with("/MeshSpecification");
    });
    auto channel = std::find_if(programItems.begin(), programItems.end(), [](auto const& item) {
      return item.kind == NestedCollectionKind::channel;
    });
    auto buffer = std::find_if(programItems.begin(), programItems.end(), [](auto const& item) {
      return item.kind == NestedCollectionKind::buffer;
    });
    if (attribs == programItems.end() || mesh == programItems.end() ||
        channel == programItems.end() || buffer == programItems.end()) {
      return fail("Program attributes, mesh, buffers, or channels were not rendered.");
    }
    auto beforeInvalidProgram = workspace.canonicalYaml();
    if (workspace.setNestedProperty({}, "RenderProgram", attribs->path,
                                    "textures", "-1") ||
        workspace.setNestedProperty({}, "RenderProgram", mesh->path,
                                    "primitive", "QUADS") ||
        workspace.setNestedProperty({}, "RenderProgram", mesh->path,
                                    "indexed", "yes") ||
        workspace.canonicalYaml() != beforeInvalidProgram) {
      return fail("Invalid Program numbers, enums, or booleans were accepted.");
    }
    if (!workspace.setNestedProperty({}, "RenderProgram", attribs->path,
                                     "textures", "4") ||
        !workspace.setNestedProperty({}, "RenderProgram", attribs->path,
                                     "diffuse", "true") ||
        !workspace.setNestedProperty({}, "RenderProgram", mesh->path,
                                     "primitive", "LINES") ||
        !workspace.setNestedProperty({}, "RenderProgram", channel->path,
                                     "data", "TEXCOORD2") ||
        !workspace.setNestedProperty({}, "RenderProgram", channel->path,
                                     "type", "UINT8") ||
        !workspace.setNestedProperty({}, "RenderProgram", channel->path,
                                     "normalised", "true") ||
        !workspace.addNestedItem({}, "RenderProgram",
                                 buffer->path + "/Channels/Channel",
                                 NestedCollectionKind::channel) ||
        !workspace.addNestedItem({}, "RenderProgram",
                                 "/Definitions/Definition/0/MeshSpecification/Buffers/Buffer",
                                 NestedCollectionKind::buffer)) {
      return fail("Valid Program constrained fields or collections were rejected: " +
                  workspace.operationDiagnostic());
    }
    auto editedProgram = workspace.canonicalYaml();
    if (!workspace.undo() || workspace.canonicalYaml() == editedProgram ||
        !workspace.redo() || workspace.canonicalYaml() != editedProgram) {
      return fail("Program collection history was not reversible.");
    }

    if (!workspace.beginDraft("Material")) return fail("Could not begin Material draft.");
    workspace.setDraftName("Surface");
    auto programChoices = workspace.draftReferenceChoices("Program");
    if (programChoices.size() != 1U ||
        !workspace.setDraftReference("Program", {}, "RenderProgram") ||
        !workspace.commitDraft()) {
      return fail("Material Program dependency was not selector-backed.");
    }
    if (!workspace.addResourceDependency({}, "Surface", "DiffuseImage",
                                         {{}, "TextureImage"}) ||
        workspace.addResourceDependency({}, "Surface", "DiffuseImage",
                                        {{}, "TextureImage"})) {
      return fail("Material image dependency creation or uniqueness failed.");
    }
    auto materialDefinitions = workspace.nestedFormItems({}, "Surface");
    auto materialDefinition = std::find_if(
        materialDefinitions.begin(), materialDefinitions.end(), [](auto const& item) {
          return item.kind == NestedCollectionKind::definition;
        });
    if (materialDefinition == materialDefinitions.end() ||
        !workspace.addNestedItem({}, "Surface",
                                 materialDefinition->path + "/Textures/Texture",
                                 NestedCollectionKind::texture)) {
      return fail("A Material texture could not be added.");
    }
    auto textures = workspace.nestedFormItems({}, "Surface");
    auto texture = std::find_if(textures.begin(), textures.end(), [](auto const& item) {
      return item.kind == NestedCollectionKind::texture;
    });
    if (texture == textures.end() || texture->alternative != "default" ||
        !workspace.setNestedAlternative({}, "Surface", texture->path, "resource")) {
      return fail("Material default/resource texture alternatives were not rendered atomically.");
    }
    textures = workspace.nestedFormItems({}, "Surface");
    texture = std::find_if(textures.begin(), textures.end(), [](auto const& item) {
      return item.kind == NestedCollectionKind::texture;
    });
    auto value = std::find_if(texture->properties.begin(), texture->properties.end(),
                              [](auto const& property) { return property.name == "value"; });
    auto beforeBadTexture = workspace.canonicalYaml();
    if (value == texture->properties.end() ||
        value->selectorValues != std::vector<std::string>{"DiffuseImage"} ||
        workspace.setNestedProperty({}, "Surface", texture->path, "value",
                                    "MissingId") ||
        workspace.canonicalYaml() != beforeBadTexture ||
        !workspace.setNestedProperty({}, "Surface", texture->path, "sampler",
                                     "Diffuse") ||
        !workspace.setNestedAlternative({}, "Surface", texture->path, "default") ||
        workspace.canonicalYaml().find("value: \"DiffuseImage\"") !=
            std::string::npos) {
      return fail("Material texture selectors, constraints, or alternative cleanup failed.");
    }

    auto output = base / "advanced.yaml";
    if (!workspace.saveAs(output)) return fail("Could not save advanced fixture.");

    auto bundle = ResourceSchemaCatalog::builtIn().snapshot().exportBundle();
    std::string const specializedSchema = R"({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://schemas.willpower.test/program-post.schema.json",
  "title": "Program Post Definition",
  "allOf": [
    {"$ref": "https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/definition"},
    {
      "type": "object",
      "additionalProperties": false,
      "required": ["factory", "label", "passes", "enabled", "weight", "mode"],
      "properties": {
        "factory": {"enum": ["Post"]},
        "label": {"type": "string", "minLength": 1},
        "passes": {"type": "integer", "minimum": 1},
        "enabled": {"type": "boolean"},
        "weight": {"type": "number", "exclusiveMinimum": 0},
        "mode": {"enum": ["fast", "quality"]}
      },
      "if": {"properties": {"mode": {"const": "quality"}}},
      "then": {"properties": {"passes": {"type": "integer", "minimum": 2}}}
    }
  ]
})";
    auto catalogJson = Json::parse(bundle.catalogJson);
    catalogJson["schemas"].push_back(
        Json{{"kind", "resourceType"},
             {"resourceType", "Program"},
             {"factoryType", "Post"},
             {"schemaId", "https://schemas.willpower.test/program-post.schema.json"},
             {"document", "schemas/program-post.schema.json"},
             {"documentHash", "sha256:f8ceda1e0e77505b64fa2b2e2dca92a449dc010354b7b3d804c84d1ffed4720a"}});
    bundle.catalogJson = catalogJson.dump(2) + "\n";
    bundle.documents.push_back(
        {"schemas/program-post.schema.json", specializedSchema});
    ResourceSchemaCatalog specializedCatalog(bundle);
    ManifestWorkspace specialized(specializedCatalog.snapshot());
    if (!specialized.open(output)) {
      return fail("A catalog with a specialized Program factory could not open the fixture: " +
                  specialized.operationDiagnostic());
    }
    auto factories = specialized.definitionFactories({}, "RenderProgram");
    if (factories.size() != 2U ||
        std::none_of(factories.begin(), factories.end(), [](auto const& factory) {
          return factory.factoryType == "Post" && !factory.disabled;
        }) ||
        specialized.definitionFactories({}, "Surface").size() != 1U ||
        specialized.addDefinition({}, "Surface", "Post") ||
        !specialized.addDefinition({}, "RenderProgram", "Post") ||
        specialized.addDefinition({}, "RenderProgram", "Post")) {
      return fail("Definition factories were not filtered by type or kept unique.");
    }
    auto specializedItems = specialized.nestedFormItems({}, "RenderProgram");
    auto post = std::find_if(specializedItems.begin(), specializedItems.end(),
                             [](auto const& item) {
                               return item.label == "Definition: Post";
                             });
    if (post == specializedItems.end() || post->properties.size() != 6U) {
      return fail("The exact specialized Definition schema was not rendered.");
    }
    if (!specialized.setNestedProperty({}, "RenderProgram", post->path,
                                       "passes", "1")) {
      return fail("A valid specialized conditional baseline was rejected.");
    }
    auto beforeInvalidSpecialized = specialized.canonicalYaml();
    if (specialized.setNestedProperty({}, "RenderProgram", post->path,
                                      "passes", "0") ||
        specialized.setNestedProperty({}, "RenderProgram", post->path,
                                      "weight", "0") ||
        specialized.setNestedProperty({}, "RenderProgram", post->path,
                                      "mode", "invalid") ||
        specialized.setNestedProperty({}, "RenderProgram", post->path,
                                      "mode", "quality") ||
        specialized.canonicalYaml() != beforeInvalidSpecialized ||
        !specialized.setNestedProperty({}, "RenderProgram", post->path,
                                       "label", "Bloom") ||
        !specialized.setNestedProperty({}, "RenderProgram", post->path,
                                       "passes", "2") ||
        !specialized.setNestedProperty({}, "RenderProgram", post->path,
                                       "enabled", "true") ||
        !specialized.setNestedProperty({}, "RenderProgram", post->path,
                                       "weight", "0.5") ||
        !specialized.setNestedProperty({}, "RenderProgram", post->path,
                                       "mode", "quality")) {
      return fail("Specialized Definition constraints were not enforced.");
    }
    auto authoredSpecialized = specialized.canonicalYaml();
    if (specialized.removeNestedItem({}, "RenderProgram",
                                     "/Definitions/Definition/0") ||
        !specialized.removeNestedItem({}, "RenderProgram", post->path) ||
        !specialized.undo() || specialized.canonicalYaml() != authoredSpecialized ||
        !specialized.redo()) {
      return fail("Definition deletion validity or history was incorrect.");
    }
    auto finalOutput = base / "specialized.yaml";
    if (!specialized.saveAs(finalOutput)) {
      return fail("Could not save canonical specialized output.");
    }
    auto saved = ResourceManifestDocument::load(finalOutput);
    if (!saved.validate(specializedCatalog.snapshot()).valid() ||
        saved.serializeCanonical() != specialized.canonicalYaml() ||
        readBytes(finalOutput) != specialized.canonicalYaml()) {
      return fail("Advanced canonical output was invalid or unstable.");
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runSchemaIntegrationTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-schema-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  struct AddedSchema {
    std::string resourceType;
    std::string id;
    std::string document;
    std::string hash;
    std::string contents;
  };
  auto makeBundle = [](std::vector<AddedSchema> const& added,
                       bool dependencyBaseOnly) {
    auto bundle = ResourceSchemaCatalog::builtIn().snapshot().exportBundle();
    auto catalog = Json::parse(bundle.catalogJson);
    if (dependencyBaseOnly) {
      for (auto& entry : catalog.at("schemas")) {
        if (entry.at("kind") == "resourceType") {
          entry["kind"] = "dependency";
          entry["resourceType"] = nullptr;
          entry["factoryType"] = nullptr;
        }
      }
    }
    for (auto const& schema : added) {
      catalog["schemas"].push_back(
          Json{{"kind", "resourceType"},
               {"resourceType", schema.resourceType},
               {"factoryType", nullptr},
               {"schemaId", schema.id},
               {"document", schema.document},
               {"documentHash", "sha256:" + schema.hash}});
      bundle.documents.push_back({schema.document, schema.contents});
    }
    bundle.catalogJson = catalog.dump(2) + "\n";
    return bundle;
  };
  auto writeBundle = [](fs::path const& directory,
                        ResourceSchemaBundle const& bundle) {
    fs::create_directories(directory);
    std::ofstream(directory / "catalog.json", std::ios::binary)
        << bundle.catalogJson;
    for (auto const& document : bundle.documents) {
      auto path = directory / fs::path(document.document);
      fs::create_directories(path.parent_path());
      std::ofstream(path, std::ios::binary) << document.contents;
    }
  };

  std::string const widgetSchema =
      R"JSON({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://schemas.example.test/editor-widget.schema.json","title":"Editor Widget Resource","$ref":"#/definitions/resource","definitions":{"defaultDefinition":{"type":"object","additionalProperties":false,"required":["size","mode"],"properties":{"size":{"type":"integer","minimum":1},"mode":{"enum":["fast","quality"]}}},"definition":{"oneOf":[{"$ref":"#/definitions/defaultDefinition"},{"allOf":[{"$ref":"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/definition"},{"required":["factory"]}]}]},"definitionCollection":{"oneOf":[{"$ref":"#/definitions/definition"},{"type":"array","minItems":1,"items":{"$ref":"#/definitions/definition"}}]},"definitions":{"type":"object","additionalProperties":false,"required":["Definition"],"properties":{"Definition":{"$ref":"#/definitions/definitionCollection"}}},"targetDependency":{"allOf":[{"$ref":"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/dependentResource"},{"required":["id"],"properties":{"id":{"enum":["Target"]},"ref":{"$ref":"https://schemas.willpower.dev/resource-manifest/common.schema.json#/definitions/nonEmptyString","x-willpower-editor-version":"1.0","x-willpower-widget":"resource-reference","x-willpower-allowed-resource-types":["TextFile"],"x-willpower-reference-scope":"manifest"}}}]},"dependentResources":{"type":"object","additionalProperties":false,"required":["DependentResource"],"properties":{"DependentResource":{"$ref":"#/definitions/targetDependency"}}},"resource":{"allOf":[{"$ref":"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/resource"},{"required":["name","DependentResources","Definitions"],"properties":{"type":{"enum":["Widget"]},"DependentResources":{"$ref":"#/definitions/dependentResources"},"Definitions":{"$ref":"#/definitions/definitions"}},"not":{"required":["location"]}}]}}}
)JSON";
  std::string restrictiveWidget = widgetSchema;
  auto minimum = restrictiveWidget.find("\"minimum\":1");
  restrictiveWidget.replace(minimum, std::string("\"minimum\":1").size(),
                            "\"minimum\":2");
  std::string const unsupportedSchema =
      R"JSON({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://schemas.example.test/unsupported.schema.json","title":"Unsupported Resource","$ref":"#/definitions/resource","definitions":{"defaultDefinition":{"type":"object","additionalProperties":false,"required":["matrix"],"properties":{"matrix":{"type":"array","minItems":1,"items":{"type":"number"}}}},"definitions":{"type":"object","additionalProperties":false,"required":["Definition"],"properties":{"Definition":{"$ref":"#/definitions/defaultDefinition"}}},"resource":{"allOf":[{"$ref":"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/resource"},{"required":["name","Definitions"],"properties":{"type":{"enum":["Unsupported"]},"Definitions":{"$ref":"#/definitions/definitions"}},"not":{"required":["location"]}}]}}}
)JSON";
  std::string const extensionSchema =
      R"JSON({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://schemas.example.test/extension.schema.json","title":"Extension Resource","$ref":"#/definitions/resource","definitions":{"resource":{"allOf":[{"$ref":"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/resource"},{"required":["name"],"properties":{"type":{"enum":["ExtensionType"]}},"not":{"required":["location"]}}]}}}
)JSON";
  std::string const malformedAnnotationSchema =
      R"JSON({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://schemas.example.test/bad-annotation.schema.json","title":"Bad Annotation Resource","$ref":"#/definitions/resource","definitions":{"resource":{"allOf":[{"$ref":"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/resource"},{"required":["name","location"],"properties":{"type":{"enum":["BadAnnotation"]},"location":{"type":"string","x-willpower-editor-version":"1.0","x-willpower-widget":"file","x-willpower-file-kind":"Bad","x-willpower-file-extensions":[".bad"]}}}]}}}
)JSON";

  AddedSchema const widget{
      "Widget", "https://schemas.example.test/editor-widget.schema.json",
      "schemas/editor-widget.schema.json",
      "138545901211f8dbd92ca20017e6bb5f195217ca69efa805a8e8715b16e6c98e",
      widgetSchema};
  AddedSchema const restrictive{
      "Widget", "https://schemas.example.test/editor-widget.schema.json",
      "schemas/editor-widget.schema.json",
      "28c06a6062dc724b2e2299f474a75bbdd679ee7417212f653f15a884ceee3253",
      restrictiveWidget};
  AddedSchema const unsupported{
      "Unsupported", "https://schemas.example.test/unsupported.schema.json",
      "schemas/unsupported.schema.json",
      "e55ee74e3c774b60b4b2b66d750bdbd2eb23a8e2cce063d374fc7a5a06dbe7ab",
      unsupportedSchema};
  AddedSchema const extension{
      "ExtensionType", "https://schemas.example.test/extension.schema.json",
      "schemas/extension.schema.json",
      "8dbbb2ed7ed306fbbafaa5f62562a6984cfb2b5b65f135bc37f5880dc9379809",
      extensionSchema};
  AddedSchema const malformed{
      "BadAnnotation",
      "https://schemas.example.test/bad-annotation.schema.json",
      "schemas/bad-annotation.schema.json",
      "6592c6b7919981f416cd9a14b21e141c82a6165b2ebdd3af22829d597313463e",
      malformedAnnotationSchema};

  try {
    fs::create_directories(root);
    writeBundle(root / "complete", makeBundle({widget, unsupported}, false));
    writeBundle(root / "custom-only", makeBundle({widget}, true));
    writeBundle(root / "extension", makeBundle({extension}, true));
    writeBundle(root / "restrictive",
                makeBundle({restrictive, unsupported}, false));
    writeBundle(root / "malformed", makeBundle({malformed}, false));

    auto ini = root / "resource-manager.ini";
    std::ofstream(ini) << "[ResourceManifestEditor]\nformatVersion=1\n"
                          "baseBundle=custom-only\n";
    auto customSources = readEditorSchemaConfiguration(ini);
    auto customCatalog = loadEditorSchemaCatalog(customSources);
    ManifestWorkspace customOnly(customCatalog);
    if (!customOnly.resourceForm("Widget") ||
        customOnly.resourceForm("TextFile")) {
      return fail(
          "A configured complete base bundle did not replace embedded "
          "built-ins.");
    }

    std::ofstream(ini, std::ios::trunc)
        << "[ResourceManifestEditor]\nformatVersion=1\n"
           "baseBundle=complete\n"
           "bundle=extension\n";
    auto configured = readEditorSchemaConfiguration(ini);
    if (!configured.baseBundle ||
        configured.baseBundle->filename() != "complete" ||
        configured.extensionBundles.size() != 1U ||
        configured.extensionBundles.front().filename() != "extension") {
      return fail(
          "Relative base and ordered extension bundle paths were not "
          "resolved from the INI.");
    }
    auto catalog = loadEditorSchemaCatalog(configured);
    ManifestWorkspace workspace(catalog);
    if (!workspace.resourceForm("Widget") ||
        !workspace.resourceForm("ExtensionType") ||
        workspace.resourceForm("Unsupported")) {
      return fail(
          "Complete, extension, or unsupported authoring forms were "
          "classified incorrectly.");
    }

    std::ofstream(root / "source.txt") << "source";
    auto manifestPath = root / "application.yaml";
    std::ofstream(manifestPath) << R"YAML(Resources:
  Resource:
    - {type: TextFile, name: TargetFile, location: source.txt}
    - type: Widget
      name: ExistingWidget
      DependentResources:
        DependentResource: {id: Target, ref: TargetFile}
      Definitions:
        Definition: {size: 1, mode: fast}
    - type: Unsupported
      name: UnsupportedOne
      Definitions:
        Definition: {matrix: [1]}
    - type: FutureType
      name: Future
      Definitions:
        Definition: {future-token: preserve-me}
)YAML";
    if (!workspace.open(manifestPath)) {
      return fail("Application and unknown Resource fixture did not open: " +
                  workspace.operationDiagnostic());
    }
    auto resources = workspace.resources();
    auto unknown = std::find_if(
        resources.begin(), resources.end(),
        [](auto const& item) { return item.resourceType == "FutureType"; });
    auto unsupportedItem = std::find_if(
        resources.begin(), resources.end(),
        [](auto const& item) { return item.resourceType == "Unsupported"; });
    if (unknown == resources.end() || !unknown->unknownType ||
        unknown->editable ||
        unknown->limitationWarning.find("preserved") == std::string::npos ||
        unsupportedItem == resources.end() || unsupportedItem->editable ||
        unsupportedItem->unknownType) {
      return fail(
          "Unknown and unsupported Resource limitations were not explicit.");
    }
    if (!workspace.renameResource({}, "Future", "FutureRenamed") ||
        workspace.canonicalYaml().find("future-token") == std::string::npos ||
        workspace.canonicalYaml().find("preserve-me") == std::string::npos) {
      return fail(
          "A safe common operation did not preserve an unknown payload.");
    }

    auto references = workspace.resourceReferences({}, "ExistingWidget");
    if (references.size() != 1U || references.front().allowedResourceTypes !=
                                       std::vector<std::string>{"TextFile"}) {
      return fail(
          "Application Resource-reference annotations did not generate "
          "a typed selector.");
    }
    auto widgetItems = workspace.nestedFormItems({}, "ExistingWidget");
    auto widgetDefinition = std::find_if(
        widgetItems.begin(), widgetItems.end(), [](auto const& item) {
          return item.kind == NestedCollectionKind::definition;
        });
    if (widgetDefinition == widgetItems.end() ||
        widgetDefinition->properties.size() != 2U) {
      return fail(
          "Application default Definition did not generate a scalar form.");
    }
    if (!workspace.beginDraft("Widget"))
      return fail("Could not begin application Resource draft.");
    workspace.setDraftName("NewWidget");
    if (!workspace.setDraftReference("Target", {}, "TargetFile") ||
        !workspace.commitDraft() ||
        workspace.canonicalYaml().find("name: \"NewWidget\"") ==
            std::string::npos) {
      return fail(
          "Application-owned Resource could not be authored from bundle data.");
    }
    if (!workspace.deleteResource({}, "NewWidget")) {
      return fail("Could not remove the application draft reload fixture.");
    }

    auto previousFormCount = workspace.resourceForms().size();
    std::ofstream(ini, std::ios::trunc)
        << "[ResourceManifestEditor]\nformatVersion=1\nbaseBundle=malformed\n";
    if (workspace.reloadSchemas(ini) ||
        workspace.resourceForms().size() != previousFormCount ||
        !workspace.resourceForm("Widget")) {
      return fail(
          "Malformed annotations did not preserve the previous editor "
          "catalog.");
    }

    std::ofstream(ini, std::ios::trunc)
        << "[ResourceManifestEditor]\nformatVersion=1\nbaseBundle="
           "restrictive\n";
    if (workspace.reloadSchemas(ini) || !workspace.resourceForm("Widget") ||
        workspace.operationDiagnostic().find("open Resource Manifest") ==
            std::string::npos) {
      return fail(
          "Open-document validation failure did not reject schema "
          "reload atomically.");
    }

    std::ofstream(ini, std::ios::trunc)
        << "[ResourceManifestEditor]\nformatVersion=1\nbaseBundle=complete\n"
           "bundle=extension\nbundle=extension\n";
    bool duplicateRejected = false;
    try {
      static_cast<void>(
          loadEditorSchemaCatalog(readEditorSchemaConfiguration(ini)));
    } catch (std::exception const& error) {
      duplicateRejected =
          std::string(error.what()).find("Duplicate") != std::string::npos;
    }
    if (!duplicateRejected) {
      return fail("A duplicate configured lookup key was silently ignored.");
    }

    std::ofstream(ini, std::ios::trunc)
        << "[ResourceManifestEditor]\nformatVersion=1\nbaseBundle=complete\n"
           "bundle=extension\n";
    if (!workspace.reloadSchemas(ini) ||
        !workspace.resourceForm("ExtensionType")) {
      return fail(
          "A corrected INI and bundle set could not be reloaded successfully.");
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

bool runSemanticRepairTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-semantic-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    auto base = root / "base";
    fs::create_directories(base / "nested");
    fs::create_directories(root / "blocked-base");
    for (auto const& file : {"valid.txt", "fixed.txt", "fixed2.txt"})
      std::ofstream(base / file) << file;
    std::ofstream(base / "nested" / "implicit.txt") << "implicit";
    std::ofstream(base / "image.png", std::ios::binary) << "image";
    std::ofstream(base / "shader.vert") << "shader";
    std::ofstream(root / "outside.txt") << "outside";

    auto manifest = base / "Resources.yaml";
    std::ofstream(manifest) << R"YAML(Resources:
  Resource:
    - {type: TextFile, name: Duplicate, location: valid.txt}
    - {type: TextFile, name: Duplicate, location: valid.txt}
    - {type: TextFile, name: Missing, location: missing.txt}
    - {type: TextFile, name: Escaping, location: ../outside.txt}
    - {type: TextFile, location: nested/implicit.txt}
    - {type: Image, name: ImageTarget, location: image.png}
    - {type: Shader, name: ShaderTarget, location: shader.vert}
    - type: ImageSet
      name: AtlasMissing
      DependentResources:
        DependentResource: {id: Image, ref: DoesNotExist}
      Definitions:
        Definition:
          Images:
            Image: {name: Pixel, x: 0, y: 0, width: 1, height: 1}
    - type: ImageSet
      name: AtlasWrong
      DependentResources:
        DependentResource: {id: Image, ref: ShaderTarget}
      Definitions:
        Definition:
          Images:
            Image: {name: Pixel, x: 0, y: 0, width: 1, height: 1}
    - type: FutureType
      name: CycleA
      DependentResources:
        DependentResource: {id: Link, ref: CycleB}
    - type: FutureType
      name: CycleB
      DependentResources:
        DependentResource: {id: Link, ref: CycleA}
  Namespace:
    - name: Bad/Namespace
      Resource: {type: FutureType, name: BadNamespaceResource}
    - name: Repeated
      Resource: {type: FutureType, name: FirstRepeated}
    - name: Repeated
      Resource: {type: FutureType, name: SecondRepeated}
)YAML";
    auto original = readBytes(manifest);

    ManifestWorkspace workspace;
    if (!workspace.open(manifest) || !workspace.operationDiagnostic().empty()) {
      return fail("A structurally valid manifest with semantic errors did not open.");
    }
    std::set<SemanticDiagnosticKind> errorKinds;
    for (auto const& diagnostic : workspace.semanticDiagnostics()) {
      if (diagnostic.severity == SemanticDiagnosticSeverity::error)
        errorKinds.insert(diagnostic.kind);
    }
    for (auto kind : {SemanticDiagnosticKind::duplicateName,
                      SemanticDiagnosticKind::invalidName,
                      SemanticDiagnosticKind::unresolvedReference,
                      SemanticDiagnosticKind::referenceTypeMismatch,
                      SemanticDiagnosticKind::dependencyCycle,
                      SemanticDiagnosticKind::missingFile,
                      SemanticDiagnosticKind::pathContainment}) {
      if (!errorKinds.contains(kind))
        return fail("Semantic diagnostics omitted a required error category.");
    }
    auto unresolved = std::find_if(
        workspace.semanticDiagnostics().begin(),
        workspace.semanticDiagnostics().end(), [](auto const& diagnostic) {
          return diagnostic.kind == SemanticDiagnosticKind::unresolvedReference;
        });
    if (unresolved == workspace.semanticDiagnostics().end())
      return fail("Missing reference diagnostic was not available.");
    auto navigation = workspace.semanticDiagnosticNavigation(
        static_cast<std::size_t>(unresolved -
                                 workspace.semanticDiagnostics().begin()));
    if (!navigation || navigation->resourceName != "AtlasMissing" ||
        navigation->resourcePath.empty() ||
        !navigation->instancePath.ends_with("/ref")) {
      return fail("Semantic diagnostic navigation did not retain its Resource and property path.");
    }
    if (workspace.canSave() || workspace.canSaveAs() || workspace.save() ||
        readBytes(manifest) != original) {
      return fail("Semantic errors did not gate Save and Save As without modifying the file.");
    }

    // Name repairs are path-addressed because the invalid identities are
    // intentionally ambiguous. Other independent errors must remain open.
    if (!workspace.renameResourceAtPath("/Resources/Resource/1", "Duplicate2") ||
        !workspace.renameResourceAtPath("/Resources/Resource/4", "Implicit") ||
        !workspace.renameNamespaceAtPath("/Resources/Namespace/0", "BadNamespace") ||
        !workspace.renameNamespaceAtPath("/Resources/Namespace/2", "Repeated2")) {
      return fail("Duplicate and invalid names could not be repaired incrementally: " +
                  workspace.operationDiagnostic());
    }
    if (workspace.renameResourceAtPath("/Resources/Resource/1", "Duplicate")) {
      return fail("A name repair introduced a new duplicate identity.");
    }

    if (workspace.setResourceFile({}, "Missing", base / "image.png") ||
        workspace.operationDiagnostic().find("extensions") == std::string::npos) {
      return fail("An annotated file widget accepted a schema-incompatible target.");
    }
    if (!workspace.setResourceFile({}, "Missing", base / "fixed.txt") ||
        !workspace.setResourceFile({}, "Escaping", base / "fixed2.txt")) {
      return fail("Independent missing and escaping files could not be repaired.");
    }

    auto yamlBeforeBlockedBase = workspace.canonicalYaml();
    if (workspace.changeBaseDirectory(root / "blocked-base") ||
        workspace.baseDirectory() != fs::canonical(base) ||
        workspace.canonicalYaml() != yamlBeforeBlockedBase) {
      return fail("A base-directory change lost or escaped an annotated absolute target.");
    }
    std::map<std::string, fs::path> absoluteTargets;
    for (auto const& resource : workspace.resources()) {
      if (!resource.location.empty())
        absoluteTargets[resource.name] =
            fs::canonical(workspace.baseDirectory() / resource.location);
    }
    if (!workspace.changeBaseDirectory(root)) {
      return fail("A containing base directory could not preserve annotated targets: " +
                  workspace.operationDiagnostic());
    }
    for (auto const& resource : workspace.resources()) {
      auto found = absoluteTargets.find(resource.name);
      if (found != absoluteTargets.end() &&
          fs::canonical(workspace.baseDirectory() / resource.location) !=
              found->second) {
        return fail("Base-directory migration changed an absolute file target.");
      }
    }
    if (workspace.canSave())
      return fail("Non-file semantic errors stopped gating Save after base migration.");

    auto repairReference = [&](std::string const& owner) {
      auto selectors = workspace.resourceReferences({}, owner);
      if (selectors.size() != 1U) return false;
      auto image = std::find_if(
          selectors.front().choices.begin(), selectors.front().choices.end(),
          [](auto const& choice) {
            return choice.name == "ImageTarget" && !choice.disabled;
          });
      return image != selectors.front().choices.end() &&
             workspace.setResourceReference(
                 {}, owner, selectors.front().dependencyIndex,
                 std::pair{image->resourceNamespace, image->name});
    };
    if (!repairReference("AtlasMissing") || !repairReference("AtlasWrong")) {
      return fail("Known and annotated references could not be repaired through valid selectors.");
    }
    auto cycleSelectors = workspace.resourceReferences({}, "CycleB");
    if (cycleSelectors.size() != 1U ||
        std::none_of(cycleSelectors.front().choices.begin(),
                     cycleSelectors.front().choices.end(), [](auto const& choice) {
                       return choice.name == "CycleA" && choice.disabled;
                     }) ||
        !workspace.setResourceReference({}, "CycleB",
                                        cycleSelectors.front().dependencyIndex,
                                        {})) {
      return fail("Cycle-producing reference targets were not rejected or repairable.");
    }

    if (hasSemanticErrors(workspace.semanticDiagnostics()) ||
        !workspace.canSave() || !workspace.canSaveAs()) {
      return fail("Final semantic repairs did not enable saving.");
    }
    if (std::none_of(workspace.semanticDiagnostics().begin(),
                     workspace.semanticDiagnostics().end(), [](auto const& item) {
                       return item.severity == SemanticDiagnosticSeverity::warning;
                     })) {
      return fail("The warning-only final document did not exercise non-blocking warnings.");
    }
    auto output = root / "final.yaml";
    if (!workspace.saveAs(output) || workspace.dirty() ||
        readBytes(output) != workspace.canonicalYaml()) {
      return fail("Final canonical semantic repair output was not saved deterministically.");
    }
    auto saved = ResourceManifestDocument::load(output);
    if (!saved.validate(ResourceSchemaCatalog::builtIn().snapshot()).valid() ||
        saved.serializeCanonical() != readBytes(output) ||
        readBytes(output).find("base/fixed.txt") == std::string::npos ||
        readBytes(output).find("Duplicate2") == std::string::npos ||
        readBytes(output).find("ImageTarget") == std::string::npos) {
      return fail("Final canonical output did not contain the staged repairs.");
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

}  // namespace resource_manager
