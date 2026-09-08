#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include "EmbeddedResourceManifestSchemas.h"

namespace wp::application::resourcesystem {
namespace {
using Json = nlohmann::json;

constexpr std::string_view supportedBundleVersion = "1.0";
constexpr std::string_view supportedManifestVersion = "1.0";

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

ResourceSchemaKind parseKind(std::string const& value, std::string const& context) {
  if (value == "manifest") return ResourceSchemaKind::manifest;
  if (value == "resourceType") return ResourceSchemaKind::resourceType;
  if (value == "dependency") return ResourceSchemaKind::dependency;
  throw ResourceSchemaCatalogException(context + ": invalid schema kind '" + value + "'.");
}

std::string keyText(std::string const& resourceType, std::string const& factoryType) {
  return "('" + resourceType + "', '" +
         (factoryType.empty() ? std::string("<default>") : factoryType) + "')";
}

bool safeDocumentPath(std::string const& value) {
  if (value.empty() || value.front() == '/' || value.find('\\') != std::string::npos ||
      value.find(':') != std::string::npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin <= value.size()) {
    auto const end = value.find('/', begin);
    auto const segment = value.substr(begin, end - begin);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}

std::string readFile(std::filesystem::path const& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw ResourceSchemaCatalogException("Cannot read Resource Schema Bundle file '" +
                                         path.string() + "'.");
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw ResourceSchemaCatalogException("Failed while reading Resource Schema Bundle file '" +
                                         path.string() + "'.");
  }
  return contents.str();
}

void writeFile(std::filesystem::path const& path, std::string const& contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(contents.data(), static_cast<std::streamsize>(contents.size()))) {
    throw ResourceSchemaCatalogException("Cannot write Resource Schema Bundle file '" +
                                         path.string() + "'.");
  }
}

// Small private SHA-256 implementation. Keeping hashing here avoids adding a
// cryptography type or dependency to the public catalog ABI.
constexpr std::array<std::uint32_t, 64> shaConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotateRight(std::uint32_t value, unsigned amount) {
  return (value >> amount) | (value << (32U - amount));
}

std::string sha256(std::string_view source) {
  std::vector<std::uint8_t> bytes(source.begin(), source.end());
  auto const bitLength = static_cast<std::uint64_t>(bytes.size()) * 8U;
  bytes.push_back(0x80U);
  while (bytes.size() % 64U != 56U) bytes.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>(bitLength >> shift));
  }

  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                    0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                    0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t offset = 0; offset < bytes.size(); offset += 64U) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      auto const byte = offset + index * 4U;
      words[index] = (static_cast<std::uint32_t>(bytes[byte]) << 24U) |
                     (static_cast<std::uint32_t>(bytes[byte + 1U]) << 16U) |
                     (static_cast<std::uint32_t>(bytes[byte + 2U]) << 8U) |
                     static_cast<std::uint32_t>(bytes[byte + 3U]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      auto const s0 = rotateRight(words[index - 15U], 7U) ^
                      rotateRight(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
      auto const s1 = rotateRight(words[index - 2U], 17U) ^
                      rotateRight(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      auto const sum1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
      auto const choice = (e & f) ^ (~e & g);
      auto const temporary1 = h + sum1 + choice + shaConstants[index] + words[index];
      auto const sum0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
      auto const majority = (a & b) ^ (a & c) ^ (b & c);
      auto const temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (auto value : hash) result << std::setw(8) << value;
  return result.str();
}

Json parseJson(std::string const& text, std::string const& context) {
  try {
    return Json::parse(text);
  } catch (Json::exception const& error) {
    throw ResourceSchemaCatalogException(context + ": malformed JSON: " + error.what());
  }
}

void requireExactFields(Json const& object, std::set<std::string> const& expected,
                        std::string const& context) {
  if (!object.is_object()) throw ResourceSchemaCatalogException(context + " must be an object.");
  std::set<std::string> actual;
  for (auto const& [name, value] : object.items()) {
    static_cast<void>(value);
    actual.insert(name);
  }
  if (actual != expected) {
    for (auto const& name : expected) {
      if (!actual.contains(name))
        throw ResourceSchemaCatalogException(context + " is missing required field '" + name + "'.");
    }
    for (auto const& name : actual) {
      if (!expected.contains(name))
        throw ResourceSchemaCatalogException(context + " has unknown field '" + name + "'.");
    }
  }
}

std::string requiredString(Json const& object, char const* field, std::string const& context) {
  auto const& value = object.at(field);
  if (!value.is_string() || value.get_ref<std::string const&>().empty()) {
    throw ResourceSchemaCatalogException(context + ": '" + field +
                                         "' must be a non-empty string.");
  }
  return value.get<std::string>();
}

std::string optionalKey(Json const& object, char const* field, std::string const& context) {
  auto const& value = object.at(field);
  if (value.is_null()) return {};
  if (!value.is_string() || value.get_ref<std::string const&>().empty()) {
    throw ResourceSchemaCatalogException(context + ": '" + field +
                                         "' must be null or a non-empty string.");
  }
  return value.get<std::string>();
}

bool absoluteSchemaId(std::string const& value) {
  auto const colon = value.find(':');
  if (colon == std::string::npos || colon == 0 ||
      std::isalpha(static_cast<unsigned char>(value.front())) == 0)
    return false;
  return std::all_of(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(colon),
                     [](unsigned char character) {
                       return std::isalnum(character) != 0 || character == '+' || character == '-' ||
                              character == '.';
                     });
}

bool entryLess(ResourceSchema const& left, ResourceSchema const& right) {
  return std::tuple(kindName(left.kind), left.resourceType, left.factoryType, left.schemaId,
                    left.document) <
         std::tuple(kindName(right.kind), right.resourceType, right.factoryType, right.schemaId,
                    right.document);
}

std::string decodeUriFragment(std::string const& fragment, std::string const& context) {
  std::string result;
  for (std::size_t index = 0; index < fragment.size(); ++index) {
    if (fragment[index] != '%') {
      result.push_back(fragment[index]);
      continue;
    }
    if (index + 2U >= fragment.size()) {
      throw ResourceSchemaCatalogException(context + ": malformed percent escape in $ref fragment.");
    }
    auto hex = [](char character) -> int {
      if (character >= '0' && character <= '9') return character - '0';
      if (character >= 'a' && character <= 'f') return character - 'a' + 10;
      if (character >= 'A' && character <= 'F') return character - 'A' + 10;
      return -1;
    };
    auto const high = hex(fragment[index + 1U]);
    auto const low = hex(fragment[index + 2U]);
    if (high < 0 || low < 0) {
      throw ResourceSchemaCatalogException(context + ": malformed percent escape in $ref fragment.");
    }
    result.push_back(static_cast<char>((high << 4) | low));
    index += 2U;
  }
  return result;
}

std::string resolveRelativeId(std::string const& base, std::string const& reference) {
  if (reference.empty() || absoluteSchemaId(reference)) return reference;
  if (reference.front() == '/') {
    auto const scheme = base.find("://");
    if (scheme == std::string::npos) return reference;
    auto const authorityEnd = base.find('/', scheme + 3U);
    return (authorityEnd == std::string::npos ? base : base.substr(0, authorityEnd)) + reference;
  }
  auto const slash = base.rfind('/');
  return (slash == std::string::npos ? std::string{} : base.substr(0, slash + 1U)) + reference;
}

void visitReferences(Json const& value, std::vector<std::string>& references) {
  if (value.is_object()) {
    for (auto const& [name, child] : value.items()) {
      if (name == "$ref") {
        if (!child.is_string() || child.get_ref<std::string const&>().empty()) {
          throw ResourceSchemaCatalogException("JSON Schema $ref must be a non-empty string.");
        }
        references.push_back(child.get<std::string>());
      }
      visitReferences(child, references);
    }
  } else if (value.is_array()) {
    for (auto const& child : value) visitReferences(child, references);
  }
}

void validateReferenceClosure(std::vector<ResourceSchema> const& entries) {
  std::map<std::string, Json> documents;
  for (auto const& entry : entries) {
    if (documents.contains(entry.schemaId)) continue;
    documents.emplace(entry.schemaId,
                      parseJson(entry.contents, "Schema document '" + entry.document + "'"));
  }

  for (auto const& [schemaId, document] : documents) {
    std::vector<std::string> references;
    visitReferences(document, references);
    for (auto const& reference : references) {
      auto const hash = reference.find('#');
      auto targetId = reference.substr(0, hash);
      if (targetId.empty()) targetId = schemaId;
      targetId = resolveRelativeId(schemaId, targetId);
      auto const target = documents.find(targetId);
      if (target == documents.end()) {
        throw ResourceSchemaCatalogException("Schema '" + schemaId + "' has unresolved local $ref '" +
                                             reference + "'.");
      }
      if (hash == std::string::npos || hash + 1U == reference.size()) continue;
      auto const fragment = decodeUriFragment(reference.substr(hash + 1U),
                                              "Schema '" + schemaId + "'");
      if (fragment.empty()) continue;
      if (fragment.front() != '/') {
        throw ResourceSchemaCatalogException("Schema '" + schemaId + "' has unsupported local $ref "
                                             "fragment '#" + fragment + "'.");
      }
      try {
        static_cast<void>(target->second.at(Json::json_pointer(fragment)));
      } catch (Json::exception const&) {
        throw ResourceSchemaCatalogException("Schema '" + schemaId + "' has unresolved local $ref '" +
                                             reference + "'.");
      }
    }
  }
}

std::vector<ResourceSchema> parseBundle(ResourceSchemaBundle const& bundle,
                                        std::string const& label) {
  auto const catalog = parseJson(bundle.catalogJson, label + " catalog");
  requireExactFields(catalog, {"bundleFormatVersion", "resourceManifestSchemaVersion", "schemas"},
                     label + " catalog");
  auto const bundleVersion = requiredString(catalog, "bundleFormatVersion", label + " catalog");
  auto const manifestVersion =
      requiredString(catalog, "resourceManifestSchemaVersion", label + " catalog");
  if (bundleVersion != supportedBundleVersion) {
    throw ResourceSchemaCatalogException(label + ": unsupported bundleFormatVersion '" +
                                         bundleVersion + "' (supported: 1.0).");
  }
  if (manifestVersion != supportedManifestVersion) {
    throw ResourceSchemaCatalogException(label + ": unsupported resourceManifestSchemaVersion '" +
                                         manifestVersion + "' (supported: 1.0).");
  }
  auto const& schemas = catalog.at("schemas");
  if (!schemas.is_array()) {
    throw ResourceSchemaCatalogException(label + " catalog: 'schemas' must be an array.");
  }

  std::map<std::string, std::string> documents;
  for (auto const& document : bundle.documents) {
    if (!safeDocumentPath(document.document)) {
      throw ResourceSchemaCatalogException(label + ": unsafe schema document path '" +
                                           document.document + "'.");
    }
    if (!documents.emplace(document.document, document.contents).second) {
      throw ResourceSchemaCatalogException(label + ": duplicate schema document path '" +
                                           document.document + "'.");
    }
  }

  std::vector<ResourceSchema> result;
  std::set<std::pair<std::string, std::string>> keys;
  std::map<std::string, std::string> ids;
  std::map<std::string, std::string> paths;
  std::size_t index = 0;
  for (auto const& schema : schemas) {
    auto const context = label + " catalog schema entry " + std::to_string(index++);
    requireExactFields(schema,
                       {"kind", "resourceType", "factoryType", "schemaId", "document",
                        "documentHash"},
                       context);
    ResourceSchema entry;
    entry.kind = parseKind(requiredString(schema, "kind", context), context);
    entry.resourceType = optionalKey(schema, "resourceType", context);
    entry.factoryType = optionalKey(schema, "factoryType", context);
    entry.schemaId = requiredString(schema, "schemaId", context);
    entry.document = requiredString(schema, "document", context);
    entry.documentHash = requiredString(schema, "documentHash", context);

    if (entry.kind == ResourceSchemaKind::resourceType) {
      if (entry.resourceType.empty()) {
        throw ResourceSchemaCatalogException(context +
                                             ": resourceType entry requires a Resource Type.");
      }
      if (!keys.emplace(entry.resourceType, entry.factoryType).second) {
        throw ResourceSchemaCatalogException(label + ": duplicate Resource Schema key " +
                                             keyText(entry.resourceType, entry.factoryType) + ".");
      }
    } else if (!entry.resourceType.empty() || !entry.factoryType.empty()) {
      throw ResourceSchemaCatalogException(context +
                                           ": non-resource entry cannot have Resource or factory types.");
    }
    if (!absoluteSchemaId(entry.schemaId)) {
      throw ResourceSchemaCatalogException(context + ": schemaId must be an absolute URI.");
    }
    if (!safeDocumentPath(entry.document)) {
      throw ResourceSchemaCatalogException(context + ": unsafe schema document path '" +
                                           entry.document + "'.");
    }
    if (entry.documentHash.size() != 71U || !entry.documentHash.starts_with("sha256:") ||
        !std::all_of(entry.documentHash.begin() + 7, entry.documentHash.end(), [](char character) {
          return (character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f');
        })) {
      throw ResourceSchemaCatalogException(context + ": malformed or unsupported documentHash '" +
                                           entry.documentHash + "'.");
    }
    auto const document = documents.find(entry.document);
    if (document == documents.end()) {
      throw ResourceSchemaCatalogException(context + ": missing schema document '" + entry.document +
                                           "'.");
    }
    entry.contents = document->second;
    auto const actualHash = "sha256:" + sha256(entry.contents);
    if (actualHash != entry.documentHash) {
      throw ResourceSchemaCatalogException(context + ": hash mismatch for schema document '" +
                                           entry.document + "' (expected " + entry.documentHash +
                                           ", found " + actualHash + ").");
    }
    auto const parsedDocument = parseJson(entry.contents, "Schema document '" + entry.document + "'");
    if (!parsedDocument.is_object() || !parsedDocument.contains("$id") ||
        !parsedDocument.at("$id").is_string()) {
      throw ResourceSchemaCatalogException(context + ": schema document has no string $id.");
    }
    auto const actualId = parsedDocument.at("$id").get<std::string>();
    if (actualId != entry.schemaId) {
      throw ResourceSchemaCatalogException(context + ": schema ID mismatch for '" + entry.document +
                                           "' (expected '" + entry.schemaId + "', found '" + actualId +
                                           "').");
    }

    auto const pathHash = paths.emplace(entry.document, actualHash);
    if (!pathHash.second && pathHash.first->second != actualHash) {
      throw ResourceSchemaCatalogException(label + ": schema document path '" + entry.document +
                                           "' has conflicting content.");
    }
    auto const idHash = ids.emplace(entry.schemaId, actualHash);
    if (!idHash.second && idHash.first->second != actualHash) {
      throw ResourceSchemaCatalogException(label + ": duplicate schema ID '" + entry.schemaId +
                                           "' has conflicting content.");
    }
    result.push_back(std::move(entry));
  }
  return result;
}

void mergeEntries(std::vector<ResourceSchema>& destination,
                  std::vector<ResourceSchema> const& incoming, std::string const& label) {
  std::map<std::pair<std::string, std::string>, ResourceSchema> keys;
  std::map<std::string, std::string> ids;
  for (auto const& entry : destination) {
    if (entry.kind == ResourceSchemaKind::resourceType)
      keys[{entry.resourceType, entry.factoryType}] = entry;
    ids.emplace(entry.schemaId, entry.contents);
  }

  for (auto const& entry : incoming) {
    auto id = ids.find(entry.schemaId);
    if (id != ids.end() && id->second != entry.contents) {
      throw ResourceSchemaCatalogException(label + ": schema ID collision for '" + entry.schemaId +
                                           "': existing and incoming content differ.");
    }
    if (entry.kind == ResourceSchemaKind::resourceType) {
      auto key = keys.find({entry.resourceType, entry.factoryType});
      if (key != keys.end()) {
        if (key->second.schemaId != entry.schemaId || key->second.contents != entry.contents) {
          throw ResourceSchemaCatalogException(label + ": Resource Schema key collision " +
                                               keyText(entry.resourceType, entry.factoryType) +
                                               " (existing schema ID '" + key->second.schemaId +
                                               "', incoming schema ID '" + entry.schemaId + "').");
        }
        continue;  // Equivalent registration is idempotent.
      }
    } else {
      auto duplicate = std::find_if(destination.begin(), destination.end(), [&](auto const& current) {
        return current.kind == entry.kind && current.schemaId == entry.schemaId;
      });
      if (duplicate != destination.end()) continue;
    }
    destination.push_back(entry);
    ids.emplace(entry.schemaId, entry.contents);
    if (entry.kind == ResourceSchemaKind::resourceType)
      keys.emplace(std::pair(entry.resourceType, entry.factoryType), entry);
  }
  std::sort(destination.begin(), destination.end(), entryLess);
}

std::string jsonString(std::string const& value) { return Json(value).dump(); }

std::string suffixedDocument(std::string const& path, std::string const& hash) {
  auto const slash = path.rfind('/');
  auto const dot = path.rfind('.');
  auto const suffix = "-" + hash.substr(7U, 12U);
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + suffix;
  return path.substr(0, dot) + suffix + path.substr(dot);
}

ResourceSchemaBundle exportEntries(std::vector<ResourceSchema> entries) {
  // Reuse an original location unless two different documents claim it. In
  // that case stable hash suffixes preserve both without depending on merge order.
  std::map<std::string, std::set<std::string>> pathHashes;
  std::map<std::string, std::pair<std::string, std::string>> documentsById;
  for (auto const& entry : entries) {
    pathHashes[entry.document].insert(entry.documentHash);
    documentsById.emplace(entry.schemaId,
                          std::pair(entry.document, entry.documentHash));
  }
  std::map<std::string, std::string> occupiedPaths;
  std::map<std::string, std::string> exportedPaths;
  for (auto const& [schemaId, document] : documentsById) {
    auto path = document.first;
    if (pathHashes[path].size() > 1U) path = suffixedDocument(path, document.second);
    while (occupiedPaths.contains(path) && occupiedPaths[path] != document.second)
      path = suffixedDocument(path, document.second);
    occupiedPaths[path] = document.second;
    exportedPaths[schemaId] = std::move(path);
  }
  for (auto& entry : entries) entry.document = exportedPaths.at(entry.schemaId);
  std::sort(entries.begin(), entries.end(), entryLess);

  ResourceSchemaBundle result;
  std::map<std::string, std::string> documents;
  for (auto const& entry : entries) documents.emplace(entry.document, entry.contents);
  for (auto const& [path, contents] : documents) result.documents.push_back({path, contents});

  std::ostringstream catalog;
  catalog << "{\n"
          << "  \"bundleFormatVersion\": \"1.0\",\n"
          << "  \"resourceManifestSchemaVersion\": \"1.0\",\n"
          << "  \"schemas\": [\n";
  for (std::size_t index = 0; index < entries.size(); ++index) {
    auto const& entry = entries[index];
    catalog << "    {\n"
            << "      \"kind\": " << jsonString(kindName(entry.kind)) << ",\n"
            << "      \"resourceType\": "
            << (entry.resourceType.empty() ? "null" : jsonString(entry.resourceType)) << ",\n"
            << "      \"factoryType\": "
            << (entry.factoryType.empty() ? "null" : jsonString(entry.factoryType)) << ",\n"
            << "      \"schemaId\": " << jsonString(entry.schemaId) << ",\n"
            << "      \"document\": " << jsonString(entry.document) << ",\n"
            << "      \"documentHash\": " << jsonString(entry.documentHash) << "\n"
            << "    }" << (index + 1U == entries.size() ? "\n" : ",\n");
  }
  catalog << "  ]\n}\n";
  result.catalogJson = catalog.str();
  return result;
}

Json collectionOf(Json item) {
  return Json{{"oneOf", Json::array({item, Json{{"type", "array"},
                                                 {"minItems", 1},
                                                 {"items", item}}})}};
}

std::string composeRootSchema(std::vector<ResourceSchema> const& entries,
                              std::string const& rootSchemaId) {
  if (!absoluteSchemaId(rootSchemaId)) {
    throw ResourceSchemaCatalogException("Root schema ID must be an absolute URI: '" +
                                         rootSchemaId + "'.");
  }

  struct TypeSchemas {
    std::string defaultId;
    std::map<std::string, std::string> factories;
  };
  std::map<std::string, TypeSchemas> types;
  for (auto const& entry : entries) {
    if (entry.kind != ResourceSchemaKind::resourceType) continue;
    auto& type = types[entry.resourceType];
    if (entry.factoryType.empty())
      type.defaultId = entry.schemaId;
    else
      type.factories.emplace(entry.factoryType, entry.schemaId);
  }

  constexpr char commonId[] =
      "https://schemas.willpower.dev/resource-manifest/common.schema.json";
  constexpr char resourceId[] =
      "https://schemas.willpower.dev/resource-manifest/resource.schema.json";
  Json definitions = Json::object();
  Json resourceBranches = Json::array();
  Json knownTypes = Json::array();
  std::size_t number = 0;
  for (auto const& [resourceType, schemas] : types) {
    knownTypes.push_back(resourceType);
    Json base;
    if (!schemas.defaultId.empty()) {
      base = Json{{"$ref", schemas.defaultId}};
    } else {
      base = Json{{"allOf", Json::array(
                                {Json{{"$ref", std::string(resourceId) + "#/definitions/resource"}},
                                 Json{{"properties", Json{{"type", Json{{"enum", Json::array({resourceType})}}}}}}})}};
    }

    Json constraints = Json::array({base});
    if (!schemas.factories.empty()) {
      Json definitionBranches = Json::array();
      Json registeredFactories = Json::array();
      for (auto const& [factory, schemaId] : schemas.factories) {
        registeredFactories.push_back(factory);
        definitionBranches.push_back(
            Json{{"allOf", Json::array(
                               {Json{{"$ref", std::string(resourceId) + "#/definitions/definition"}},
                                Json{{"required", Json::array({"factory"})},
                                     {"properties", Json{{"factory", Json{{"enum", Json::array({factory})}}}}}},
                                Json{{"$ref", schemaId}}})}});
      }
      definitionBranches.push_back(
          Json{{"allOf", Json::array(
                             {Json{{"$ref", std::string(resourceId) + "#/definitions/definition"}},
                              Json{{"not", Json{{"required", Json::array({"factory"})},
                                                  {"properties", Json{{"factory", Json{{"enum", registeredFactories}}}}}}}}})}});

      auto const suffix = std::to_string(number);
      auto const definitionName = "applicationDefinition" + suffix;
      auto const collectionName = "applicationDefinitionCollection" + suffix;
      auto const definitionsName = "applicationDefinitions" + suffix;
      definitions[definitionName] = Json{{"oneOf", std::move(definitionBranches)}};
      definitions[collectionName] = collectionOf(Json{{"$ref", "#/definitions/" + definitionName}});
      definitions[definitionsName] =
          Json{{"type", "object"},
               {"additionalProperties", false},
               {"required", Json::array({"Definition"})},
               {"properties", Json{{"Definition", Json{{"$ref", "#/definitions/" + collectionName}}}}}};
      constraints.push_back(Json{{"properties", Json{{"Definitions", Json{{"$ref", "#/definitions/" + definitionsName}}}}}});
    }
    resourceBranches.push_back(Json{{"allOf", std::move(constraints)}});
    ++number;
  }
  resourceBranches.push_back(
      Json{{"allOf", Json::array(
                         {Json{{"$ref", std::string(resourceId) + "#/definitions/resource"}},
                          Json{{"properties", Json{{"type", Json{{"not", Json{{"enum", knownTypes}}}}}}}}})}});

  definitions["resource"] = Json{{"oneOf", std::move(resourceBranches)}};
  definitions["resourceCollection"] =
      collectionOf(Json{{"$ref", "#/definitions/resource"}});
  definitions["namespace"] =
      Json{{"type", "object"},
           {"additionalProperties", false},
           {"required", Json::array({"name", "Resource"})},
           {"properties",
            Json{{"name", Json{{"$ref", std::string(commonId) + "#/definitions/nonEmptyString"}}},
                 {"Resource", Json{{"$ref", "#/definitions/resourceCollection"}}}}}};

  Json root{{"$schema", "http://json-schema.org/draft-07/schema#"},
            {"$id", rootSchemaId},
            {"title", "Application Resource Manifest"},
            {"type", "object"},
            {"additionalProperties", false},
            {"required", Json::array({"Resources"})},
            {"properties",
             Json{{"Resources",
                   Json{{"type", "object"},
                        {"additionalProperties", false},
                        {"properties",
                         Json{{"Resource", Json{{"$ref", "#/definitions/resourceCollection"}}},
                              {"Namespace", collectionOf(Json{{"$ref", "#/definitions/namespace"}})}}}}}}},
            {"definitions", std::move(definitions)}};
  return root.dump(2) + "\n";
}

void writeExportedBundle(std::filesystem::path const& directory,
                         ResourceSchemaBundle const& bundle) {
  if (directory.empty()) throw ResourceSchemaCatalogException("Export directory cannot be empty.");
  std::error_code error;
  auto const target = std::filesystem::absolute(directory, error).lexically_normal();
  if (error || target == target.root_path() || target == std::filesystem::current_path(error)) {
    throw ResourceSchemaCatalogException("Refusing unsafe Resource Schema Bundle export directory '" +
                                         directory.string() + "'.");
  }
  std::filesystem::remove_all(target, error);
  if (error) {
    throw ResourceSchemaCatalogException("Cannot replace Resource Schema Bundle directory '" +
                                         target.string() + "': " + error.message());
  }
  std::filesystem::create_directories(target, error);
  if (error) {
    throw ResourceSchemaCatalogException("Cannot create Resource Schema Bundle directory '" +
                                         target.string() + "': " + error.message());
  }
  writeFile(target / "catalog.json", bundle.catalogJson);
  for (auto const& document : bundle.documents)
    writeFile(target / std::filesystem::path(document.document), document.contents);
}

std::shared_ptr<std::vector<ResourceSchema> const> emptySnapshot();
}  // namespace

namespace {
std::shared_ptr<std::vector<ResourceSchema> const> emptySnapshot() {
  static auto const empty = std::make_shared<std::vector<ResourceSchema> const>();
  return empty;
}

std::shared_ptr<std::vector<ResourceSchema> const> embeddedSnapshot() {
  static auto const snapshot = [] {
    auto result = std::make_shared<std::vector<ResourceSchema>>();
    for (auto const& schema : detail::embeddedResourceManifestSchemas()) {
      ResourceSchema entry;
      entry.kind = parseKind(std::string(schema.kind), "Embedded Resource Schema catalog");
      entry.resourceType = schema.resourceType;
      entry.factoryType = schema.factoryType;
      entry.schemaId = schema.id;
      entry.document = schema.document;
      entry.documentHash = schema.documentHash;
      entry.contents = schema.source;
      auto const actualHash = "sha256:" + sha256(entry.contents);
      if (actualHash != entry.documentHash) {
        throw ResourceSchemaCatalogException("Embedded schema '" + entry.schemaId +
                                             "' has an invalid generated hash.");
      }
      result->push_back(std::move(entry));
    }
    std::sort(result->begin(), result->end(), entryLess);
    validateReferenceClosure(*result);
    return std::shared_ptr<std::vector<ResourceSchema> const>(std::move(result));
  }();
  return snapshot;
}
}  // namespace

struct ResourceSchemaCatalog::Impl {
  mutable std::mutex mutex;
  std::shared_ptr<std::vector<ResourceSchema> const> current = emptySnapshot();
};

ResourceSchemaCatalogSnapshot::ResourceSchemaCatalogSnapshot() : mEntries(emptySnapshot()) {}

ResourceSchemaCatalogSnapshot::ResourceSchemaCatalogSnapshot(
    std::shared_ptr<std::vector<ResourceSchema> const> entries)
    : mEntries(std::move(entries)) {}

std::vector<ResourceSchema> const& ResourceSchemaCatalogSnapshot::entries() const noexcept {
  return *mEntries;
}

ResourceSchema const* ResourceSchemaCatalogSnapshot::findExact(
    ResourceSchemaKey const& key) const noexcept {
  auto const found = std::find_if(mEntries->begin(), mEntries->end(), [&](auto const& entry) {
    return entry.kind == ResourceSchemaKind::resourceType &&
           entry.resourceType == key.resourceType && entry.factoryType == key.factoryType;
  });
  return found == mEntries->end() ? nullptr : &*found;
}

ResourceSchema const* ResourceSchemaCatalogSnapshot::find(ResourceSchemaKey const& key) const noexcept {
  if (auto const* exact = findExact(key)) return exact;
  return key.factoryType.empty() ? nullptr : findExact({key.resourceType, {}});
}

ResourceSchema const* ResourceSchemaCatalogSnapshot::findBySchemaId(
    std::string const& schemaId) const noexcept {
  auto const found = std::find_if(mEntries->begin(), mEntries->end(),
                                  [&](auto const& entry) { return entry.schemaId == schemaId; });
  return found == mEntries->end() ? nullptr : &*found;
}

ResourceSchemaBundle ResourceSchemaCatalogSnapshot::exportBundle() const {
  return exportEntries(*mEntries);
}

void ResourceSchemaCatalogSnapshot::exportBundle(std::filesystem::path const& directory) const {
  writeExportedBundle(directory, exportBundle());
}

ResourceSchemaBundle ResourceSchemaCatalogSnapshot::exportComposedBundle(
    std::string const& rootSchemaId) const {
  std::vector<ResourceSchema> entries;
  for (auto const& entry : *mEntries) {
    if (entry.kind != ResourceSchemaKind::manifest) entries.push_back(entry);
  }
  ResourceSchema root;
  root.kind = ResourceSchemaKind::manifest;
  root.schemaId = rootSchemaId;
  root.document = "schemas/resource-manifest.schema.json";
  root.contents = composeRootSchema(entries, rootSchemaId);
  root.documentHash = "sha256:" + sha256(root.contents);
  entries.push_back(std::move(root));
  std::sort(entries.begin(), entries.end(), entryLess);
  validateReferenceClosure(entries);
  return exportEntries(std::move(entries));
}

void ResourceSchemaCatalogSnapshot::exportComposedBundle(
    std::filesystem::path const& directory, std::string const& rootSchemaId) const {
  writeExportedBundle(directory, exportComposedBundle(rootSchemaId));
}

ResourceSchemaCatalog::ResourceSchemaCatalog() : mImplementation(std::make_unique<Impl>()) {}

ResourceSchemaCatalog::ResourceSchemaCatalog(ResourceSchemaBundle const& bundle)
    : ResourceSchemaCatalog() {
  addBundle(bundle);
}

ResourceSchemaCatalog::ResourceSchemaCatalog(std::filesystem::path const& bundleDirectory)
    : ResourceSchemaCatalog() {
  addBundle(bundleDirectory);
}

ResourceSchemaCatalog::ResourceSchemaCatalog(std::unique_ptr<Impl> implementation)
    : mImplementation(std::move(implementation)) {}

ResourceSchemaCatalog::~ResourceSchemaCatalog() = default;
ResourceSchemaCatalog::ResourceSchemaCatalog(ResourceSchemaCatalog&&) noexcept = default;
ResourceSchemaCatalog& ResourceSchemaCatalog::operator=(ResourceSchemaCatalog&&) noexcept = default;

ResourceSchemaCatalog ResourceSchemaCatalog::builtIn() {
  auto implementation = std::make_unique<Impl>();
  implementation->current = embeddedSnapshot();
  return ResourceSchemaCatalog(std::move(implementation));
}

ResourceSchemaBundle ResourceSchemaCatalog::readBundle(
    std::filesystem::path const& bundleDirectory) {
  auto const catalogPath = bundleDirectory / "catalog.json";
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(catalogPath, error)) || error) {
    throw ResourceSchemaCatalogException("Missing or non-regular Resource Schema Bundle catalog '" +
                                         catalogPath.string() + "'.");
  }
  ResourceSchemaBundle bundle;
  bundle.catalogJson = readFile(catalogPath);
  auto const catalog = parseJson(bundle.catalogJson, "Resource Schema Bundle catalog '" +
                                                         catalogPath.string() + "'");
  if (!catalog.is_object() || !catalog.contains("schemas") || !catalog.at("schemas").is_array()) {
    throw ResourceSchemaCatalogException("Resource Schema Bundle catalog '" + catalogPath.string() +
                                         "' has no schemas array.");
  }
  std::set<std::string> paths;
  for (auto const& schema : catalog.at("schemas")) {
    if (!schema.is_object() || !schema.contains("document") ||
        !schema.at("document").is_string()) {
      throw ResourceSchemaCatalogException("Resource Schema Bundle catalog '" + catalogPath.string() +
                                           "' has an entry without a string document path.");
    }
    auto const path = schema.at("document").get<std::string>();
    if (!safeDocumentPath(path)) {
      throw ResourceSchemaCatalogException("Unsafe Resource Schema Bundle document path '" + path +
                                           "'.");
    }
    if (!paths.insert(path).second) continue;
    auto const fullPath = bundleDirectory / std::filesystem::path(path);
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(fullPath, error)) ||
        error) {
      throw ResourceSchemaCatalogException("Missing or non-regular schema document '" +
                                           fullPath.string() + "'.");
    }
    bundle.documents.push_back({path, readFile(fullPath)});
  }
  return bundle;
}

void ResourceSchemaCatalog::addBundle(ResourceSchemaBundle const& bundle) {
  addBundles(std::span<ResourceSchemaBundle const>(&bundle, 1U));
}

void ResourceSchemaCatalog::addBundle(std::filesystem::path const& bundleDirectory) {
  addBundle(readBundle(bundleDirectory));
}

void ResourceSchemaCatalog::addBundles(std::span<ResourceSchemaBundle const> bundles) {
  if (bundles.empty()) return;
  // Parse caller-owned input before taking the publication lock. All strings
  // are copied into parsedEntries, so no snapshot can alias caller storage.
  std::vector<std::vector<ResourceSchema>> parsed;
  parsed.reserve(bundles.size());
  for (std::size_t index = 0; index < bundles.size(); ++index) {
    parsed.push_back(parseBundle(bundles[index], "Resource Schema Bundle " + std::to_string(index)));
  }

  std::lock_guard lock(mImplementation->mutex);
  auto candidate = std::make_shared<std::vector<ResourceSchema>>(*mImplementation->current);
  for (std::size_t index = 0; index < parsed.size(); ++index) {
    mergeEntries(*candidate, parsed[index], "Resource Schema Bundle " + std::to_string(index));
  }
  validateReferenceClosure(*candidate);
  mImplementation->current = std::move(candidate);
}

ResourceSchemaCatalogSnapshot ResourceSchemaCatalog::snapshot() const {
  std::lock_guard lock(mImplementation->mutex);
  return ResourceSchemaCatalogSnapshot(mImplementation->current);
}

}  // namespace wp::application::resourcesystem
