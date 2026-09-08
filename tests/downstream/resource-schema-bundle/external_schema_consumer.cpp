// Deliberately does not include or link Willpower.Application. This models an
// editor or CI process consuming only the language-neutral bundle contract.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <utils/YamlReader.h>

namespace {
std::string readFile(std::filesystem::path const& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Could not read " + path.string());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string quotedValue(std::string const& line) {
  auto const colon = line.find(':');
  auto const first = line.find('"', colon);
  auto const last = line.rfind('"');
  if (colon == std::string::npos || first == std::string::npos || last <= first) {
    throw std::runtime_error("Unexpected catalog member: " + line);
  }
  return line.substr(first + 1U, last - first - 1U);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: external-schema-consumer <bundle> <manifest>\n";
      return 2;
    }

    auto const bundle = std::filesystem::path(argv[1]);
    std::istringstream catalog(readFile(bundle / "catalog.json"));
    std::vector<utils::JsonSchemaDocument> schemas;
    std::string rootSchema;
    std::string line;
    std::string kind;
    std::string schemaId;
    std::string document;
    while (std::getline(catalog, line)) {
      if (line.find("\"kind\"") != std::string::npos) kind = quotedValue(line);
      if (line.find("\"schemaId\"") != std::string::npos) schemaId = quotedValue(line);
      if (line.find("\"document\"") != std::string::npos) document = quotedValue(line);
      if (line.find('}') != std::string::npos && !schemaId.empty() && !document.empty()) {
        auto contents = readFile(bundle / std::filesystem::path(document));
        schemas.push_back({schemaId, contents});
        if (kind == "manifest") rootSchema = contents;
        kind.clear();
        schemaId.clear();
        document.clear();
      }
    }
    if (rootSchema.empty()) throw std::runtime_error("Bundle contains no manifest schema");

    std::unique_ptr<utils::YamlReader> reader(utils::YamlReader::fromFile(argv[2]));
    auto failures = reader->validateJsonSchema(rootSchema, schemas);
    if (!failures.empty()) {
      for (auto const& failure : failures) {
        std::cerr << failure.instancePath << ": " << failure.message << '\n';
      }
      return 1;
    }
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
