#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "willpower/application/resourcesystem/ResourceManifestDocument.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace {
using namespace wp::application::resourcesystem;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void verifyRoundTrip() {
  std::string const source =
      "Resources:\r\n"
      "  Resource:\r\n"
      "    type: CustomType\r\n"
      "    name: \"Caf\xC3\xA9\"\r\n"
      "    Definitions:\r\n"
      "      Definition:\r\n"
      "        payload: [1, \"1\", false, null, 1.0, 1e2]\r\n"
      "    Option:\r\n"
      "      - {name: code, value: \"007\"}\r\n"
      "      - {name: enabled, value: true}\r\n";

  auto document = ResourceManifestDocument::parse(source, "memory.yaml");
  require(document.status() == ResourceManifestDocumentStatus::ready,
          "A valid UTF-8 Resource Manifest did not parse.");
  auto const catalog = ResourceSchemaCatalog::builtIn().snapshot();
  require(document.validate(catalog).valid(),
          "A valid custom Resource Manifest did not validate.");

  auto const canonical = document.serializeCanonical();
  require(!canonical.empty() && canonical.back() == '\n',
          "Canonical YAML has no final newline.");
  require(canonical.size() == 1 || canonical[canonical.size() - 2] != '\n',
          "Canonical YAML has more than one final newline.");
  require(canonical.find('\r') == std::string::npos,
          "Canonical YAML contains a platform-specific CR character.");
  require(canonical.find("value: \"007\"") != std::string::npos,
          "A quoted numeric string lost string semantics.");
  require(canonical.find("value: true") != std::string::npos,
          "A native boolean lost boolean semantics.");
  require(canonical.find("- 1\n") != std::string::npos &&
              canonical.find("- \"1\"") != std::string::npos &&
              canonical.find("- 1.0") != std::string::npos &&
              canonical.find("- 100.0") != std::string::npos,
          "Sequence or numeric scalar semantics were not preserved.");

  auto reparsed = ResourceManifestDocument::parse(canonical, "canonical.yaml");
  require(reparsed.validate(catalog).valid(),
          "Canonical YAML did not validate after parsing again.");
  require(reparsed.serializeCanonical() == canonical,
          "Canonical serialization is not deterministic after a round-trip.");
}

void verifyStatusesAndDiagnostics() {
  auto const catalog = ResourceSchemaCatalog::builtIn().snapshot();

  auto malformed = ResourceManifestDocument::parse(
      "Resources:\n  Resource: [\n", "malformed.yaml");
  auto malformedResult = malformed.validate(catalog);
  require(malformedResult.status ==
              ResourceManifestValidationStatus::yamlSyntaxError &&
              !malformedResult.diagnostics.empty() &&
              malformedResult.diagnostics.front().kind ==
                  ResourceManifestDiagnosticKind::yamlSyntax,
          "Malformed YAML did not receive its distinct YAML syntax status.");

  auto structurallyInvalid = ResourceManifestDocument::parse(
      "Resources:\n  unexpected: true\n", "structural.yaml");
  auto structuralResult = structurallyInvalid.validate(catalog);
  bool foundResourcesPath = false;
  for (auto const& item : structuralResult.diagnostics) {
    foundResourcesPath = foundResourcesPath || item.instancePath == "/Resources";
  }
  require(structuralResult.status ==
              ResourceManifestValidationStatus::structurallyInvalid &&
              !structuralResult.diagnostics.empty() &&
              structuralResult.diagnostics.front().kind ==
                  ResourceManifestDiagnosticKind::structuralValidation &&
              foundResourcesPath,
          "Schema-invalid YAML did not receive structural diagnostics.");

  auto duplicate = ResourceManifestDocument::parse(
      "Resources: {}\nResources: {}\n", "duplicate.yaml");
  auto duplicateResult = duplicate.validate(catalog);
  require(duplicateResult.status ==
              ResourceManifestValidationStatus::yamlSyntaxError &&
              !duplicateResult.diagnostics.empty() &&
              duplicateResult.diagnostics.front().message.find("unique") !=
                  std::string::npos,
          "A duplicate YAML mapping key was accepted.");

  auto multiple = ResourceManifestDocument::parse(
      "Resources: {}\n---\nResources: {}\n", "multiple.yaml");
  require(multiple.status() == ResourceManifestDocumentStatus::yamlSyntaxError,
          "A multi-document YAML stream was accepted.");

  auto invalidUtf8 = ResourceManifestDocument::parse(
      std::string("Resources: \"") + static_cast<char>(0xff) + "\"\n",
      "invalid-utf8.yaml");
  require(invalidUtf8.status() == ResourceManifestDocumentStatus::yamlSyntaxError,
          "Invalid UTF-8 was accepted.");
}

void verifyDefensiveLimits() {
  ResourceManifestLimits limits;
  limits.maximumBytes = 8;
  auto oversized = ResourceManifestDocument::parse("Resources: {}\n", "large.yaml", limits);
  require(oversized.status() ==
              ResourceManifestDocumentStatus::defensiveLimitExceeded,
          "The Resource Manifest byte limit was not enforced.");

  limits = {};
  limits.maximumDepth = 3;
  auto deep = ResourceManifestDocument::parse(
      "Resources:\n  Resource:\n    type: Custom\n    name: Deep\n",
      "deep.yaml", limits);
  require(deep.status() ==
              ResourceManifestDocumentStatus::defensiveLimitExceeded,
          "The Resource Manifest nesting limit was not enforced.");

  limits = {};
  limits.maximumNodes = 2;
  auto numerous =
      ResourceManifestDocument::parse("Resources: {}\n", "nodes.yaml", limits);
  require(numerous.status() ==
              ResourceManifestDocumentStatus::defensiveLimitExceeded,
          "The Resource Manifest node-count limit was not enforced.");

  limits = {};
  limits.maximumDiagnostics = 2;
  auto invalid = ResourceManifestDocument::parse(
      "Resources:\n"
      "  Resource:\n"
      "    - {type: TextFile, name: One}\n"
      "    - {type: TextFile, name: Two}\n"
      "    - {type: TextFile, name: Three}\n",
      "bounded-diagnostics.yaml", limits);
  auto result = invalid.validate(ResourceSchemaCatalog::builtIn().snapshot());
  require(result.status == ResourceManifestValidationStatus::structurallyInvalid &&
              result.diagnostics.size() == limits.maximumDiagnostics,
          "Structural diagnostics were not capped at the configured limit.");
}

void verifyFileLoad(std::filesystem::path const& fixture) {
  auto document = ResourceManifestDocument::load(fixture);
  require(document.status() == ResourceManifestDocumentStatus::ready,
          "The built-in Resource Manifest fixture could not be loaded.");
  require(document.validate(ResourceSchemaCatalog::builtIn().snapshot()).valid(),
          "The built-in Resource Manifest fixture did not validate through the public API.");

  auto missing = ResourceManifestDocument::load(fixture.string() + ".missing");
  require(missing.status() == ResourceManifestDocumentStatus::filesystemError &&
              missing.validate(ResourceSchemaCatalog::builtIn().snapshot()).status ==
                  ResourceManifestValidationStatus::filesystemError,
          "A missing Resource Manifest did not report a filesystem status.");
}

}  // namespace

int main(int argc, char const* const* argv) {
  try {
    require(argc == 2, "Expected the built-in Resource Manifest fixture path.");
    verifyRoundTrip();
    verifyStatusesAndDiagnostics();
    verifyDefensiveLimits();
    verifyFileLoad(argv[1]);
    std::cout << "Resource Manifest document tests passed.\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
