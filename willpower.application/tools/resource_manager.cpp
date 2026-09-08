#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "willpower/application/resourcesystem/ResourceManifestDocument.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace {
namespace fs = std::filesystem;
using namespace wp::application::resourcesystem;

constexpr int success = 0;
constexpr int usageFailure = 2;
constexpr int validationFailure = 4;
constexpr int filesystemFailure = 6;
constexpr int serializationFailure = 7;
constexpr int internalFailure = 9;

void printUsage(std::ostream& output) {
  output << "Resource Manifest Editor (headless)\n"
            "Usage:\n"
            "  resource-manager --validate FILE --base-directory DIR "
            "[--canonical-output FILE|-]\n"
            "  resource-manager --help\n";
}

std::string kindName(ResourceManifestDiagnosticKind kind) {
  switch (kind) {
    case ResourceManifestDiagnosticKind::yamlSyntax:
      return "YAML syntax";
    case ResourceManifestDiagnosticKind::structuralValidation:
      return "structural validation";
    case ResourceManifestDiagnosticKind::defensiveLimit:
      return "defensive limit";
    case ResourceManifestDiagnosticKind::filesystem:
      return "filesystem";
  }
  return "validation";
}

void printDiagnostic(ResourceManifestDiagnostic const& diagnostic) {
  std::cerr << (diagnostic.manifestPath.empty() ? "Resource Manifest"
                                                : diagnostic.manifestPath)
            << ": " << kindName(diagnostic.kind);
  if (!diagnostic.resourceName.empty()) {
    std::cerr << " for ";
    if (!diagnostic.resourceNamespace.empty()) {
      std::cerr << diagnostic.resourceNamespace << '/';
    }
    std::cerr << diagnostic.resourceName;
    if (!diagnostic.resourceType.empty() &&
        diagnostic.resourceType != "ResourceManifest") {
      std::cerr << " [" << diagnostic.resourceType << ']';
    }
  }
  std::cerr << " at "
            << (diagnostic.instancePath.empty() ? "/" : diagnostic.instancePath);
  if (diagnostic.line > 0 && diagnostic.column > 0) {
    std::cerr << " (line " << diagnostic.line << ", column "
              << diagnostic.column << ')';
  }
  std::cerr << ": " << diagnostic.message << '\n';
}

bool hasYamlExtension(fs::path const& path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".yaml" || extension == ".yml";
}

struct Arguments {
  fs::path manifest;
  fs::path baseDirectory;
  fs::path canonicalOutput;
  bool outputCanonical = false;
};

bool parseArguments(int argc, char const* const* argv, Arguments& result) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") return false;
  bool validate = false;
  for (int index = 1; index < argc; ++index) {
    std::string_view const argument(argv[index]);
    auto takeValue = [&](fs::path& destination) {
      if (++index == argc) return false;
      destination = fs::path(argv[index]);
      return true;
    };
    if (argument == "--validate" && result.manifest.empty()) {
      validate = takeValue(result.manifest);
      if (!validate) return false;
    } else if (argument == "--base-directory" && result.baseDirectory.empty()) {
      if (!takeValue(result.baseDirectory)) return false;
    } else if (argument == "--canonical-output" && !result.outputCanonical) {
      result.outputCanonical = true;
      if (!takeValue(result.canonicalOutput)) return false;
    } else {
      return false;
    }
  }
  return validate && !result.baseDirectory.empty();
}

int validateManifest(Arguments const& arguments) {
  std::error_code error;
  if (!fs::is_directory(arguments.baseDirectory, error)) {
    std::cerr << arguments.baseDirectory.string()
              << ": filesystem: base directory is not an accessible directory";
    if (error) std::cerr << ": " << error.message();
    std::cerr << '\n';
    return filesystemFailure;
  }
  if (!hasYamlExtension(arguments.manifest)) {
    std::cerr << "Resource Manifest must use a .yaml or .yml extension.\n";
    return usageFailure;
  }

  auto document = ResourceManifestDocument::load(arguments.manifest);
  auto const catalog = ResourceSchemaCatalog::builtIn().snapshot();
  auto validation = document.validate(catalog);
  if (!validation.valid()) {
    for (auto const& diagnostic : validation.diagnostics) {
      printDiagnostic(diagnostic);
    }
    return validation.status == ResourceManifestValidationStatus::filesystemError
               ? filesystemFailure
               : validationFailure;
  }

  if (arguments.outputCanonical) {
    std::string canonical;
    try {
      canonical = document.serializeCanonical();
      auto roundTrip = ResourceManifestDocument::parse(
          canonical, document.sourceName() + " (canonical round-trip)");
      auto roundTripValidation = roundTrip.validate(catalog);
      if (!roundTripValidation.valid() ||
          roundTrip.serializeCanonical() != canonical) {
        std::cerr << "Canonical Resource Manifest did not round-trip "
                     "deterministically.\n";
        return internalFailure;
      }
    } catch (std::exception const& exception) {
      std::cerr << "Could not serialize Resource Manifest: " << exception.what()
                << '\n';
      return serializationFailure;
    }

    if (arguments.canonicalOutput == fs::path("-")) {
      std::cout.write(canonical.data(), static_cast<std::streamsize>(canonical.size()));
      if (!std::cout) return serializationFailure;
    } else {
      std::ofstream output(arguments.canonicalOutput,
                           std::ios::binary | std::ios::trunc);
      output.write(canonical.data(),
                   static_cast<std::streamsize>(canonical.size()));
      if (!output) {
        std::cerr << arguments.canonicalOutput.string()
                  << ": could not write canonical Resource Manifest.\n";
        return serializationFailure;
      }
      std::cout << "Valid Resource Manifest; canonical output written to "
                << arguments.canonicalOutput.string() << '\n';
    }
  } else {
    std::cout << "Valid Resource Manifest: " << arguments.manifest.string()
              << '\n';
  }
  return success;
}

}  // namespace

int main(int argc, char const* const* argv) {
  try {
    Arguments arguments;
    if (!parseArguments(argc, argv, arguments)) {
      bool const help = argc == 2 && std::string_view(argv[1]) == "--help";
      printUsage(help ? std::cout : std::cerr);
      return help ? success : usageFailure;
    }
    return validateManifest(arguments);
  } catch (std::exception const& exception) {
    std::cerr << "Internal resource-manager failure: " << exception.what() << '\n';
    return internalFailure;
  }
}
