// Exercises Resource Manifest YAML conversion, load-time schema validation, and
// scan/rescan atomicity through the real resource location.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "willpower/common/Logger.h"
#include "willpower/application/resourcesystem/DirectoryResourceLocation.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"

namespace {
namespace fs = std::filesystem;
using wp::application::resourcesystem::DirectoryResourceLocation;
using wp::application::resourcesystem::ResourceManifestValidationException;
using wp::application::resourcesystem::ResourceSystemException;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(fs::path const& path, std::string const& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::trunc);
  stream << contents;
  if (!stream) throw std::runtime_error("Could not write test fixture: " + path.string());
}

std::string replaceOnce(std::string source, std::string const& from, std::string const& to) {
  auto const position = source.find(from);
  if (position == std::string::npos || source.find(from, position + from.size()) != std::string::npos) {
    throw std::runtime_error("Test fixture replacement was not unique: " + from);
  }
  source.replace(position, from.size(), to);
  return source;
}

std::string expectInvalidScan(DirectoryResourceLocation& location, bool rescan = false) {
  try {
    if (rescan) {
      location.rescan();
    } else {
      location.scan();
    }
  } catch (ResourceManifestValidationException const& error) {
    return error.what();
  }
  throw std::runtime_error("A schema-invalid Resource Manifest was accepted.");
}

void verifyExistingFixture(fs::path const& definition, wp::Logger& logger) {
  DirectoryResourceLocation location(
      &logger, definition.parent_path().string(), definition.filename().string());
  location.scan();

  auto const& namespaces = location.getNamespaceRecords();
  auto const root = namespaces.find("");
  auto const world = namespaces.find("World");
  require(root != namespaces.end(), "The root namespace was not reconstructed.");
  require(world != namespaces.end(), "The 'World' namespace was not reconstructed.");
  require(root->second.resourceRecords.contains("EntityImage"),
          "Image Resource 'EntityImage' was not reconstructed.");

  auto const& image = root->second.resourceRecords.at("EntityImage");
  require(image.baseData.tags.at("filtering") == "none",
          "EntityImage's 'filtering' option did not survive conversion.");
  require(image.baseData.tags.at("uv-style") == "atlas",
          "EntityImage's 'uv-style' option did not survive conversion.");

  require(world->second.resourceRecords.contains("World"),
          "World/World Resource was not reconstructed.");
  auto const& map = world->second.resourceRecords.at("World");
  require(!map.definitions.empty(), "World/World has no definitions.");
  require(map.dependentResources.size() == 1,
          "World/World should have exactly one dependent Resource.");
  require(root->second.resourceRecords.contains("EntityImageSet"),
          "EntityImageSet was not reconstructed from a Resource sequence.");
}

void verifyCommonSchema(fs::path const& root, wp::Logger& logger) {
  struct InvalidCase {
    std::string yaml;
    std::string expectedPath;
  };
  std::vector<InvalidCase> const invalidCases{
      {"Resources:\n  unexpected: true\n", "/Resources"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: ''\n", "/Resources/Resource"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: bad/name\n", "/Resources/Resource"},
      {"Resources:\n  Resource:\n    type: null\n    name: Asset\n", "/Resources/Resource/type"},
      {"Resources:\n  Resource:\n    type: 12\n    name: Asset\n", "/Resources/Resource/type"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: Asset\n    Option:\n      name: ''\n      value: okay\n", "/Resources/Resource/Option"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: Asset\n    Definitions:\n      Definition: null\n", "/Resources/Resource/Definitions/Definition"},
      {"Resources:\n  Resource:\n    type: Custom\n    name: Asset\n    DependentResources:\n      DependentResource:\n        id: Child\n        ref: Other\n        type: Custom\n        name: Inline\n", "/Resources/Resource/DependentResources/DependentResource"},
      {"Resources:\n  Namespace:\n    name: ''\n    Resource:\n      type: Custom\n      name: Asset\n", "/Resources/Namespace"}};

  for (std::size_t index = 0; index < invalidCases.size(); ++index) {
    auto const caseRoot = root / ("invalid-" + std::to_string(index));
    writeFile(caseRoot / "Resources.yaml", invalidCases[index].yaml);
    DirectoryResourceLocation location(&logger, caseRoot.string(), "Resources.yaml");
    auto const message = expectInvalidScan(location);
    require(location.getNamespaceRecords().empty(),
            "A failed initial scan published Resource records.");
    require(message.find("invalid-" + std::to_string(index)) != std::string::npos &&
                message.find("Resources.yaml") != std::string::npos,
            "A validation diagnostic omitted the Resource Manifest path: " + message);
    require(message.find(invalidCases[index].expectedPath) != std::string::npos,
            "A validation diagnostic omitted the expected instance path: " + message);
  }

  auto const customRoot = root / "custom";
  writeFile(customRoot / "Resources.yaml", R"(Resources:
  Resource:
    type: PluginResource
    name: PluginAsset
    Option:
      name: enabled
      value: true
    DependentResources:
      DependentResource:
        - id: Existing
          ref: Somewhere
        - id: Inline
          type: PluginChild
          location: child.asset
          Definitions:
            Definition:
              - factory: PluginFactory
                arbitraryPayload: null
              - arbitraryDefault: [one, two]
    Definitions:
      Definition:
        factory: PluginFactory
        pluginOwned:
          anything: null
)");
  DirectoryResourceLocation custom(&logger, customRoot.string(), "Resources.yaml");
  custom.scan();
  require(custom.getNamespaceRecords().at("").resourceRecords.contains("PluginAsset"),
          "An unknown custom Resource Type did not pass common validation.");

  // Structural loading must not absorb existing semantic filesystem checks.
  auto const missingRoot = root / "missing-file";
  writeFile(missingRoot / "Resources.yaml", R"(Resources:
  Resource:
    type: TextFile
    location: absent.txt
)");
  DirectoryResourceLocation missing(&logger, missingRoot.string(), "Resources.yaml");
  missing.scan();
  try {
    missing.validateResourceDefinitions();
  } catch (ResourceSystemException const&) {
    return;
  }
  throw std::runtime_error("Semantic validation no longer rejects missing source files.");
}

void verifySourceBackedSchemas(fs::path const& root, wp::Logger& logger) {
  std::vector<std::string> const types{"TextFile", "XmlFile", "Shader", "AudioBank", "Image"};

  // Each Resource Type is valid as a singleton, infers its name from location,
  // and may carry an empty default Definition. None of these source paths
  // exists: scan-time schema validation must not open or decode source assets.
  for (auto const& type : types) {
    auto const caseRoot = root / ("valid-" + type);
    std::string option;
    if (type == "Image") {
      option = "    Option:\n      name: mipmaps\n      value: true\n";
    }
    writeFile(caseRoot / "Resources.yaml",
              "Resources:\n  Resource:\n    type: " + type +
                  "\n    location: missing/" + type + ".asset\n" + option +
                  "    Definitions:\n      Definition: {}\n");
    DirectoryResourceLocation location(&logger, caseRoot.string(), "Resources.yaml");
    location.scan();
    require(location.getNamespaceRecords().at("").resourceRecords.contains(
                "missing/" + type + ".asset"),
            type + " did not infer its Resource name from location.");
  }

  // Exercise the sequence form together with every Image option. A quoted
  // compatible boolean is intentionally distinct from the native boolean
  // exercised above.
  auto const sequenceRoot = root / "valid-sequence";
  writeFile(sequenceRoot / "Resources.yaml", R"(Resources:
  Resource:
    - type: TextFile
      location: absent.txt
    - type: XmlFile
      location: malformed.xml
    - type: Shader
      location: invalid.shader
    - type: AudioBank
      location: invalid.bank
      Definitions:
        Definition:
          factory: PluginFactory
          pluginPayload: accepted
    - type: Image
      location: undecodable.image
      Option:
        - name: filtering
          value: linear
        - name: uv-style
          value: atlas
        - name: colour-space
          value: linear
        - name: wrapping
          value: repeat
        - name: mipmaps
          value: "NO"
)");
  DirectoryResourceLocation sequence(&logger, sequenceRoot.string(), "Resources.yaml");
  sequence.scan();
  require(sequence.getNamespaceRecords().at("").resourceRecords.size() == types.size(),
          "The source-backed Resource sequence did not load every declaration.");

  struct InvalidCase {
    std::string label;
    std::string body;
    std::string expectedPath;
  };
  for (auto const& type : types) {
    std::vector<InvalidCase> cases{
        {"missing-location", "    name: MissingLocation\n", "/Resources/Resource"},
        {"unknown-field", "    name: UnknownField\n    location: source.asset\n    unknown: rejected\n",
         "/Resources/Resource"},
        {"invalid-option", "    name: InvalidOption\n    location: source.asset\n    Option:\n"
                           "      name: unsupported\n      value: rejected\n",
         "/Resources/Resource/Option"},
        {"null", "    name: NullLocation\n    location: null\n", "/Resources/Resource/location"},
        {"malformed-definition",
         "    name: MalformedDefinition\n    location: source.asset\n    Definitions:\n"
         "      Definition:\n        payload: rejected\n",
         "/Resources/Resource/Definitions"}};
    if (type == "Image") {
      cases.push_back({"unsupported-option-value",
                       "    name: InvalidFiltering\n    location: source.asset\n    Option:\n"
                       "      name: filtering\n      value: nearest\n",
                       "/Resources/Resource/Option"});
      cases.push_back({"null-option-value",
                       "    name: NullOption\n    location: source.asset\n    Option:\n"
                       "      name: mipmaps\n      value: null\n",
                       "/Resources/Resource/Option"});
    }

    for (auto const& invalid : cases) {
      auto const caseRoot = root / ("invalid-" + type + '-' + invalid.label);
      writeFile(caseRoot / "Resources.yaml",
                "Resources:\n  Resource:\n    type: " + type + "\n" + invalid.body);
      DirectoryResourceLocation location(&logger, caseRoot.string(), "Resources.yaml");
      auto const message = expectInvalidScan(location);
      require(message.find(invalid.expectedPath) != std::string::npos,
              type + " diagnostic omitted instance path " + invalid.expectedPath + ": " + message);
      auto const identity = invalid.body.find("name: ");
      if (identity != std::string::npos) {
        auto const begin = identity + 6;
        auto const end = invalid.body.find('\n', begin);
        require(message.find(invalid.body.substr(begin, end - begin)) != std::string::npos,
                type + " diagnostic omitted the Resource identity: " + message);
      }
      require(location.getNamespaceRecords().empty(),
              type + " published records after schema validation failed.");
    }
  }
}

void verifyImageAndAnimationSetSchemas(fs::path const& root, wp::Logger& logger) {
  std::string const validImageSet = R"(Resources:
  Resource:
    type: ImageSet
    name: Atlas
    DependentResources:
      DependentResource:
        - id: Image
          ref: MissingImage
    Definitions:
      Definition:
        - factory: PluginFactory
          pluginPayload: accepted
        - Images:
            Image:
              name: Icon
              x: 0
              y: "1"
              width: 16
              height: "16"
            ImageSet:
              - name: Walk
                x: "32"
                y: 0
                width: "8"
                height: 8
                count: "3"
                dx: -8
                dy: "+2"
              - name: Idle
                x: 0
                y: 0
                width: 8
                height: 8
                count: 1
                dx: 0
                dy: 0
)";
  auto const validImageRoot = root / "valid-ImageSet";
  writeFile(validImageRoot / "Resources.yaml", validImageSet);
  DirectoryResourceLocation validImages(&logger, validImageRoot.string(), "Resources.yaml");
  validImages.scan();
  require(validImages.getNamespaceRecords().at("").resourceRecords.contains("Atlas"),
          "A representative ImageSet did not pass scan-time validation.");

  std::string const validAnimationSet = R"(Resources:
  Resource:
    type: AnimationSet
    name: Characters
    DependentResources:
      DependentResource:
        id: Image
        ref: MissingAtlas
    Definitions:
      Definition:
        - factory: PluginFactory
          pluginPayload: accepted
        - Animations:
            Animation:
              - name: Walk
                loopStyle: forwards
                Frames:
                  time: "1.5e-1"
                  xoff: -2
                  yoff: "+3"
                  Frame:
                    - image: MissingWalk0
                      frame: "1"
                      time: 0.2
                      xoff: "-1"
                      Tags:
                        Tag:
                          key: event
                          value: true
                    - image: MissingWalk1
                      Tags:
                        Tag:
                          - key: sound
                            value: step
                          - key: strength
                            value: 2
              - name: Idle
                loopStyle: once
                Frames:
                  Frame:
                    image: MissingIdle
                    time: "1"
              - name: Attack
                loopStyle: pingpong
                Frames:
                  imageset: MissingAttackSet
                  count: "2"
                  time: 0.25
                  xoff: "-3"
                  yoff: 4
                  Frame:
                    - frame: 0
                      time: ".5"
                      Tags:
                        Tag:
                          key: hit
                          value: yes
                    - frame: "1"
                      xoff: 1
                      yoff: "-1"
)";
  auto const validAnimationRoot = root / "valid-AnimationSet";
  writeFile(validAnimationRoot / "Resources.yaml", validAnimationSet);
  DirectoryResourceLocation validAnimations(
      &logger, validAnimationRoot.string(), "Resources.yaml");
  validAnimations.scan();
  require(validAnimations.getNamespaceRecords().at("").resourceRecords.contains("Characters"),
          "A representative AnimationSet did not pass scan-time validation.");

  std::string const minimalImageSet = R"(Resources:
  Resource:
    type: ImageSet
    name: Atlas
    DependentResources:
      DependentResource:
        id: Image
        ref: MissingImage
    Definitions:
      Definition:
        Images:
          Image:
            name: Icon
            x: 0
            y: 0
            width: 1
            height: 1
)";
  std::string const imageSetEntry = R"(          ImageSet:
            name: Strip
            x: 0
            y: 0
            width: 1
            height: 1
            count: 2
            dx: 1
            dy: 0
)";

  struct InvalidCase {
    std::string label;
    std::string yaml;
    std::string expectedPath;
  };
  std::vector<InvalidCase> invalidCases;
  invalidCases.push_back({"image-set-missing-dependency",
                          replaceOnce(minimalImageSet,
                                      "    DependentResources:\n      DependentResource:\n"
                                      "        id: Image\n        ref: MissingImage\n",
                                      ""),
                          "/Resources/Resource"});
  invalidCases.push_back({"image-set-wrong-dependency-id",
                          replaceOnce(minimalImageSet, "        id: Image\n", "        id: Texture\n"),
                          "/Resources/Resource/DependentResources"});
  invalidCases.push_back({"image-set-location",
                          replaceOnce(minimalImageSet, "    name: Atlas\n", "    name: Atlas\n    location: atlas.png\n"),
                          "/Resources/Resource"});
  invalidCases.push_back({"image-set-missing-definition",
                          replaceOnce(minimalImageSet,
                                      "    Definitions:\n      Definition:\n        Images:\n"
                                      "          Image:\n            name: Icon\n            x: 0\n"
                                      "            y: 0\n            width: 1\n            height: 1\n",
                                      ""),
                          "/Resources/Resource"});
  invalidCases.push_back({"image-set-specialized-only",
                          replaceOnce(minimalImageSet,
                                      "        Images:\n          Image:\n            name: Icon\n"
                                      "            x: 0\n            y: 0\n            width: 1\n"
                                      "            height: 1\n",
                                      "        factory: PluginFactory\n        pluginPayload: accepted\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-missing-images",
                          replaceOnce(minimalImageSet,
                                      "        Images:\n          Image:\n            name: Icon\n"
                                      "            x: 0\n            y: 0\n            width: 1\n"
                                      "            height: 1\n",
                                      "        {}\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-empty-images",
                          replaceOnce(minimalImageSet,
                                      "        Images:\n          Image:\n            name: Icon\n"
                                      "            x: 0\n            y: 0\n            width: 1\n"
                                      "            height: 1\n",
                                      "        Images: {}\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-missing-field",
                          replaceOnce(minimalImageSet, "            height: 1\n", ""),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-empty-name",
                          replaceOnce(minimalImageSet, "            name: Icon\n", "            name: ''\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-invalid-coordinate",
                          replaceOnce(minimalImageSet, "            x: 0\n", "            x: '0oops'\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-negative-coordinate",
                          replaceOnce(minimalImageSet, "            y: 0\n", "            y: -1\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-zero-width",
                          replaceOnce(minimalImageSet, "            width: 1\n", "            width: '0'\n"),
                          "/Resources/Resource/Definitions"});

  auto const minimalImageSetCollection = replaceOnce(
      minimalImageSet,
      "          Image:\n            name: Icon\n            x: 0\n            y: 0\n"
      "            width: 1\n            height: 1\n",
      imageSetEntry);
  invalidCases.push_back({"image-set-entry-missing-name",
                          replaceOnce(minimalImageSetCollection, "            name: Strip\n", ""),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-entry-missing-count",
                          replaceOnce(minimalImageSetCollection, "            count: 2\n", ""),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-entry-zero-count",
                          replaceOnce(minimalImageSetCollection, "            count: 2\n", "            count: 0\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-entry-invalid-offset",
                          replaceOnce(minimalImageSetCollection, "            dx: 1\n", "            dx: '1px'\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-entry-missing-offset",
                          replaceOnce(minimalImageSetCollection, "            dy: 0\n", ""),
                          "/Resources/Resource/Definitions"});

  std::string const minimalAnimationSet = R"(Resources:
  Resource:
    type: AnimationSet
    name: Animations
    DependentResources:
      DependentResource:
        id: Image
        ref: MissingAtlas
    Definitions:
      Definition:
        Animations:
          Animation:
            name: Idle
            loopStyle: forwards
            Frames:
              Frame:
                image: MissingImage
                time: 1
)";
  std::string const imageSetFrames = R"(            Frames:
              imageset: MissingSet
              count: 2
              time: 1
              Frame:
                frame: 0
                xoff: -1
)";
  invalidCases.push_back({"animation-set-missing-dependency",
                          replaceOnce(minimalAnimationSet,
                                      "    DependentResources:\n      DependentResource:\n"
                                      "        id: Image\n        ref: MissingAtlas\n",
                                      ""),
                          "/Resources/Resource"});
  invalidCases.push_back({"animation-set-wrong-dependency-id",
                          replaceOnce(minimalAnimationSet, "        id: Image\n", "        id: Atlas\n"),
                          "/Resources/Resource/DependentResources"});
  invalidCases.push_back({"animation-set-missing-definition",
                          replaceOnce(minimalAnimationSet,
                                      "    Definitions:\n      Definition:\n        Animations:\n"
                                      "          Animation:\n            name: Idle\n"
                                      "            loopStyle: forwards\n            Frames:\n"
                                      "              Frame:\n                image: MissingImage\n"
                                      "                time: 1\n",
                                      ""),
                          "/Resources/Resource"});
  invalidCases.push_back({"animation-set-specialized-only",
                          replaceOnce(minimalAnimationSet,
                                      "        Animations:\n          Animation:\n            name: Idle\n"
                                      "            loopStyle: forwards\n            Frames:\n"
                                      "              Frame:\n                image: MissingImage\n"
                                      "                time: 1\n",
                                      "        factory: PluginFactory\n        pluginPayload: accepted\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"animation-missing-wrapper",
                          replaceOnce(minimalAnimationSet,
                                      "        Animations:\n          Animation:\n            name: Idle\n"
                                      "            loopStyle: forwards\n            Frames:\n"
                                      "              Frame:\n                image: MissingImage\n"
                                      "                time: 1\n",
                                      "        Animations: {}\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"animation-empty-name",
                          replaceOnce(minimalAnimationSet, "            name: Idle\n", "            name: ''\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"animation-invalid-loop-style",
                          replaceOnce(minimalAnimationSet, "            loopStyle: forwards\n",
                                      "            loopStyle: backwards\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"animation-missing-frames",
                          replaceOnce(minimalAnimationSet,
                                      "            Frames:\n              Frame:\n"
                                      "                image: MissingImage\n                time: 1\n",
                                      ""),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"explicit-mode-missing-frame",
                          replaceOnce(minimalAnimationSet,
                                      "            Frames:\n              Frame:\n"
                                      "                image: MissingImage\n                time: 1\n",
                                      "            Frames:\n              time: 1\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"explicit-frame-missing-image",
                          replaceOnce(minimalAnimationSet, "                image: MissingImage\n", ""),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"explicit-frame-empty-image",
                          replaceOnce(minimalAnimationSet, "                image: MissingImage\n",
                                      "                image: ''\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"explicit-frame-invalid-index",
                          replaceOnce(minimalAnimationSet, "                image: MissingImage\n",
                                      "                image: MissingImage\n                frame: 'first'\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"explicit-mode-illegal-count",
                          replaceOnce(minimalAnimationSet, "            Frames:\n",
                                      "            Frames:\n              count: 1\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"frame-zero-time",
                          replaceOnce(minimalAnimationSet, "                time: 1\n", "                time: '0'\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"frame-invalid-offset",
                          replaceOnce(minimalAnimationSet, "                time: 1\n",
                                      "                time: 1\n                yoff: 'down'\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"tag-empty-key",
                          replaceOnce(minimalAnimationSet, "                time: 1\n",
                                      "                time: 1\n                Tags:\n"
                                      "                  Tag: {key: '', value: event}\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"tag-null-value",
                          replaceOnce(minimalAnimationSet, "                time: 1\n",
                                      "                time: 1\n                Tags:\n"
                                      "                  Tag: {key: event, value: null}\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"tags-missing-tag",
                          replaceOnce(minimalAnimationSet, "                time: 1\n",
                                      "                time: 1\n                Tags: {}\n"),
                          "/Resources/Resource/Definitions"});

  auto const minimalImageSetAnimation = replaceOnce(
      minimalAnimationSet,
      "            Frames:\n              Frame:\n                image: MissingImage\n"
      "                time: 1\n",
      imageSetFrames);
  invalidCases.push_back({"image-set-mode-missing-reference",
                          replaceOnce(minimalImageSetAnimation, "              imageset: MissingSet\n", ""),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-mode-zero-count",
                          replaceOnce(minimalImageSetAnimation, "              count: 2\n", "              count: '0'\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-mode-invalid-time",
                          replaceOnce(minimalImageSetAnimation, "              time: 1\n", "              time: '1sec'\n"),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-override-missing-index",
                          replaceOnce(minimalImageSetAnimation, "                frame: 0\n", ""),
                          "/Resources/Resource/Definitions"});
  invalidCases.push_back({"image-set-override-illegal-image",
                          replaceOnce(minimalImageSetAnimation, "                frame: 0\n",
                                      "                frame: 0\n                image: MissingImage\n"),
                          "/Resources/Resource/Definitions"});

  for (auto const& invalid : invalidCases) {
    auto const caseRoot = root / invalid.label;
    writeFile(caseRoot / "Resources.yaml", invalid.yaml);
    DirectoryResourceLocation location(&logger, caseRoot.string(), "Resources.yaml");
    auto const message = expectInvalidScan(location);
    require(message.find(invalid.expectedPath) != std::string::npos,
            invalid.label + " diagnostic omitted " + invalid.expectedPath + ": " + message);
    require(location.getNamespaceRecords().empty(),
            invalid.label + " published records after schema validation failed.");
  }

  // Bounds, names in the dependent ImageSet, duplicate names, and complete
  // materialized-frame timing depend on loaded Resource data. Keep accepting
  // those declarations here so their existing creation-time checks own them.
  auto semanticOnlyImageSet = replaceOnce(
      minimalImageSet,
      "          Image:\n            name: Icon\n            x: 0\n            y: 0\n"
      "            width: 1\n            height: 1\n",
      "          Image:\n            - name: Icon\n              x: 999999\n"
      "              y: 0\n              width: 1\n              height: 1\n"
      "            - name: Icon\n              x: 0\n              y: 0\n"
      "              width: 1\n              height: 1\n");
  auto const semanticImageRoot = root / "semantic-only-image-set";
  writeFile(semanticImageRoot / "Resources.yaml", semanticOnlyImageSet);
  DirectoryResourceLocation semanticImages(
      &logger, semanticImageRoot.string(), "Resources.yaml");
  semanticImages.scan();

  std::string const semanticOnlyAnimation = R"(Resources:
  Resource:
    type: AnimationSet
    name: Animations
    DependentResources:
      DependentResource: {id: Image, ref: MissingAtlas}
    Definitions:
      Definition:
        Animations:
          Animation:
            - name: Duplicate
              Frames:
                Frame:
                  image: MissingImage
            - name: Duplicate
              Frames:
                imageset: MissingImageSet
)";
  auto const semanticAnimationRoot = root / "semantic-only-animation-set";
  writeFile(semanticAnimationRoot / "Resources.yaml", semanticOnlyAnimation);
  DirectoryResourceLocation semanticAnimations(
      &logger, semanticAnimationRoot.string(), "Resources.yaml");
  semanticAnimations.scan();
}

void verifyAtomicRescan(fs::path const& root, wp::Logger& logger) {
  auto const atomicRoot = root / "atomic";
  auto const manifest = atomicRoot / "Resources.yaml";
  writeFile(manifest, R"(Resources:
  Resource:
    type: Custom
    name: Stable
)");
  DirectoryResourceLocation location(&logger, atomicRoot.string(), "Resources.yaml");
  location.scan();
  require(location.getNamespaceRecords().at("").resourceRecords.contains("Stable"),
          "The initial valid scan did not publish its Resource.");

  // The first declaration must not become observable when a later declaration
  // in the same document is structurally invalid.
  writeFile(manifest, R"(Resources:
  Resource:
    - type: Custom
      name: Partial
    - type: Custom
      name: Broken
      unknown: rejected
)");
  auto const message = expectInvalidScan(location, true);
  auto const& retained = location.getNamespaceRecords().at("").resourceRecords;
  require(retained.size() == 1 && retained.contains("Stable"),
          "A failed rescan replaced or partially mutated the last valid records.");
  require(message.find("Broken") != std::string::npos,
          "A Resource-specific diagnostic omitted the Resource identity: " + message);
  require(message.find("/Resources/Resource/1") != std::string::npos,
          "A Resource-specific diagnostic omitted its sequence instance path: " + message);
  require(message.find("line ") != std::string::npos && message.find("column ") != std::string::npos,
          "A validation diagnostic omitted available YAML source coordinates: " + message);

  writeFile(manifest, R"(Resources:
  Resource:
    type: Custom
    name: Corrected
)");
  location.rescan();
  auto const& corrected = location.getNamespaceRecords().at("").resourceRecords;
  require(corrected.size() == 1 && corrected.contains("Corrected"),
          "A corrected Resource Manifest could not be retried after a failed rescan.");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: resource_yaml_tests <Resources.yaml>\n";
    return 2;
  }

  auto const unique = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path const temporaryRoot = fs::temp_directory_path() /
                                 ("willpower-resource-yaml-" + std::to_string(unique));
  fs::create_directories(temporaryRoot);

  try {
    wp::Logger logger;
    verifyExistingFixture(fs::absolute(argv[1]), logger);
    verifyCommonSchema(temporaryRoot, logger);
    verifySourceBackedSchemas(temporaryRoot, logger);
    verifyImageAndAnimationSetSchemas(temporaryRoot, logger);
    verifyAtomicRescan(temporaryRoot, logger);

    fs::remove_all(temporaryRoot);
    std::cout << "Willpower YAML Resource Manifest tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    fs::remove_all(temporaryRoot);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
