#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <mpp/BufferRenderer.h>
#include <mpp/Logger.h>
#include <mpp/RenderSystem.h>
#include <mpp/ResourceManager.h>
#include <mpp/app/ImGuiBackendData.h>
#include <mpp/app/ImGuiDataProvider.h>
#include <mpp/app/ImGuiPlatform.h>
#include <mpp/app/InputManagerSDL.h>
#include <mpp/app/RenderSystemConfig.h>
#include <mpp/app/WindowSDL.h>

#include "resource_manager/ResourceManagerShell.h"
#include "willpower/application/resourcesystem/ResourceManifestDocument.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace {
namespace fs = std::filesystem;
using namespace wp::application::resourcesystem;
using resource_manager::ManifestWorkspace;

constexpr int success = 0;
constexpr int usageFailure = 2;
constexpr int configurationFailure = 3;
constexpr int validationFailure = 4;
constexpr int filesystemFailure = 6;
constexpr int serializationFailure = 7;
constexpr int guiFailure = 8;
constexpr int internalFailure = 9;

struct SdlLifetime {
  ~SdlLifetime() { SDL_Quit(); }
};

std::string trim(std::string value) {
  auto const first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string displayPath(fs::path const& path) {
  auto const utf8 = path.generic_u8string();
  return std::string(reinterpret_cast<char const*>(utf8.data()), utf8.size());
}

std::optional<std::string> environmentValue(char const* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
  std::unique_ptr<char, decltype(&std::free)> holder(value, &std::free);
  if (!*value) return {};
  return std::string(value);
#else
  auto const* value = std::getenv(name);
  if (!value || !*value) return {};
  return std::string(value);
#endif
}

void printUsage(std::ostream& output) {
  output << "Resource Manifest Editor\n"
            "Usage:\n"
            "  resource-manager [--ini FILE] [MANIFEST.yaml]\n"
            "  resource-manager --smoke-test [--ini FILE]\n"
            "  resource-manager --startup-check [--ini FILE]\n"
            "  resource-manager --document-tests\n"
            "  resource-manager --authoring-tests\n"
            "  resource-manager --organization-tests\n"
            "  resource-manager --dependency-tests\n"
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

struct ValidationArguments {
  fs::path manifest;
  fs::path baseDirectory;
  fs::path canonicalOutput;
  bool outputCanonical = false;
};

bool parseValidationArguments(int argc, char const* const* argv,
                              ValidationArguments& result) {
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
    } else if (argument == "--base-directory" &&
               result.baseDirectory.empty()) {
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

int validateManifest(ValidationArguments const& arguments) {
  std::error_code error;
  if (!fs::is_directory(arguments.baseDirectory, error)) {
    std::cerr << displayPath(arguments.baseDirectory)
              << ": filesystem: base directory is not an accessible directory";
    if (error) std::cerr << ": " << error.message();
    std::cerr << '\n';
    return filesystemFailure;
  }
  if (!resource_manager::hasYamlExtension(arguments.manifest)) {
    std::cerr << "Resource Manifest must use a .yaml or .yml extension.\n";
    return usageFailure;
  }

  auto document = ResourceManifestDocument::load(arguments.manifest);
  auto const catalog = ResourceSchemaCatalog::builtIn().snapshot();
  auto validation = document.validate(catalog);
  if (!validation.valid()) {
    for (auto const& diagnostic : validation.diagnostics) printDiagnostic(diagnostic);
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
      std::cout.write(canonical.data(),
                      static_cast<std::streamsize>(canonical.size()));
      if (!std::cout) return serializationFailure;
    } else {
      std::ofstream output(arguments.canonicalOutput,
                           std::ios::binary | std::ios::trunc);
      output.write(canonical.data(),
                   static_cast<std::streamsize>(canonical.size()));
      if (!output) {
        std::cerr << displayPath(arguments.canonicalOutput)
                  << ": could not write canonical Resource Manifest.\n";
        return serializationFailure;
      }
      std::cout << "Valid Resource Manifest; canonical output written to "
                << displayPath(arguments.canonicalOutput) << '\n';
    }
  } else {
    std::cout << "Valid Resource Manifest: " << displayPath(arguments.manifest)
              << '\n';
  }
  return success;
}

struct DesktopArguments {
  fs::path iniPath;
  fs::path startupManifest;
  bool startupCheck = false;
  bool smokeTest = false;
};

bool parseDesktopArguments(int argc, char const* const* argv,
                           DesktopArguments& result) {
  for (int index = 1; index < argc; ++index) {
    std::string_view argument(argv[index]);
    if (argument == "--ini" && result.iniPath.empty()) {
      if (++index == argc) return false;
      result.iniPath = argv[index];
    } else if (argument == "--startup-check" && !result.startupCheck) {
      result.startupCheck = true;
    } else if (argument == "--smoke-test" && !result.smokeTest) {
      result.smokeTest = true;
    } else if (!argument.starts_with("--") && result.startupManifest.empty()) {
      result.startupManifest = argv[index];
    } else {
      return false;
    }
  }
  return !(result.startupCheck && result.smokeTest) &&
         (!result.smokeTest || result.startupManifest.empty());
}

fs::path executableDirectory() {
  char const* base = SDL_GetBasePath();
  if (!base || !*base) {
    throw std::runtime_error("Could not determine the executable directory.");
  }
  return fs::path(base);
}

struct EditorConfiguration {
  mpp::RenderSystemOptions renderOptions;
};

EditorConfiguration loadConfiguration(fs::path const& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Could not open required deployment INI '" +
                             displayPath(path) + "'.");
  }
  bool foundEditorSection = false;
  bool foundVersion = false;
  std::set<std::string> editorKeys;
  std::string section;
  std::string line;
  for (std::size_t lineNumber = 1; std::getline(input, line); ++lineNumber) {
    auto content = trim(line);
    if (content.empty() || content.front() == ';' || content.front() == '#') {
      continue;
    }
    if (content.front() == '[' && content.back() == ']') {
      section = trim(content.substr(1, content.size() - 2));
      foundEditorSection |= section == "ResourceManifestEditor";
      continue;
    }
    if (section != "ResourceManifestEditor") continue;
    auto const separator = content.find('=');
    auto const location = displayPath(path) + ":" + std::to_string(lineNumber);
    if (separator == std::string::npos) {
      throw std::runtime_error(location + ": expected key=value.");
    }
    auto const key = trim(content.substr(0, separator));
    auto const value = trim(content.substr(separator + 1));
    if (!editorKeys.insert(key).second) {
      throw std::runtime_error(location + ": duplicate setting '" + key + "'.");
    }
    if (key != "formatVersion") {
      throw std::runtime_error(location + ": unknown Resource Manifest Editor setting '" +
                               key + "'.");
    }
    if (value != "1") {
      throw std::runtime_error(location +
                               ": formatVersion must be exactly 1.");
    }
    foundVersion = true;
  }
  if (!input.eof() && input.fail()) {
    throw std::runtime_error("Could not read deployment INI '" +
                             displayPath(path) + "'.");
  }
  if (!foundEditorSection || !foundVersion) {
    throw std::runtime_error(
        "Deployment INI must define [ResourceManifestEditor] formatVersion=1.");
  }

  EditorConfiguration result;
  result.renderOptions = mpp::app::loadRenderSystemOptions(path);
  return result;
}

fs::path preferenceDirectory() {
  if (auto overridePath =
          environmentValue("WILLPOWER_RESOURCE_MANAGER_PREFERENCE_DIR")) {
    return fs::path(*overridePath);
  }
  char* path = SDL_GetPrefPath("ajare", "ResourceManifestEditor");
  if (!path || !*path) {
    if (path) SDL_free(path);
    throw std::runtime_error(
        "Could not determine the Resource Manifest Editor preference directory.");
  }
  fs::path result(path);
  SDL_free(path);
  return result;
}

bool initialiseLog(mpp::Logger& logger, fs::path& logPath,
                   std::string& failure) {
  try {
    auto directory = preferenceDirectory();
    std::error_code error;
    fs::create_directories(directory, error);
    if (error || !fs::is_directory(directory)) {
      failure = "Could not create preference directory '" +
                displayPath(directory) + "'";
      if (error) failure += ": " + error.message();
      return false;
    }
    logPath = directory / "resource-manager.log";
    auto previous = directory / "resource-manager.log.1";
    fs::remove(previous, error);
    error.clear();
    if (fs::exists(logPath)) {
      fs::rename(logPath, previous, error);
      if (error) {
        failure = "Could not rotate Resource Manifest Editor log '" +
                  displayPath(logPath) + "': " + error.message();
        return false;
      }
    }
    if (!logger.initialise(displayPath(logPath), mpp::Logger::Level::Debug)) {
      failure = "Could not create Resource Manifest Editor log '" +
                displayPath(logPath) + "'.";
      return false;
    }
    logger.info("Resource Manifest Editor startup.");
    return true;
  } catch (std::exception const& exception) {
    failure = exception.what();
    return false;
  }
}

void report(mpp::Logger& logger, std::string const& message) {
  logger.error(message);
  std::fprintf(stderr, "%s\n", message.c_str());
}

class NativeDialog {
 public:
  enum class Purpose { none, createNew, open, saveAs, resourceFile };
  struct Result {
    Purpose purpose = Purpose::none;
    std::optional<fs::path> path;
    std::string error;
    bool draftFile = false;
    bool inlineResource = false;
    std::size_t dependencyIndex = 0;
    std::string resourceNamespace;
    std::string resourceName;
  };

  bool busy() const {
    std::lock_guard lock(mState->mutex);
    return mState->pending;
  }

  void begin(Purpose purpose, SDL_Window* owner,
             std::string const& defaultLocation = {}) {
    {
      std::lock_guard lock(mState->mutex);
      if (mState->pending) return;
      mState->pending = true;
      mState->purpose = purpose;
      mState->result.reset();
    }
    auto holder = new std::shared_ptr<State>(mState);
    if (purpose == Purpose::createNew) {
      SDL_ShowOpenFolderDialog(callback, holder, owner,
                               defaultLocation.empty() ? nullptr
                                                       : defaultLocation.c_str(),
                               false);
    } else if (purpose == Purpose::saveAs) {
      SDL_ShowSaveFileDialog(callback, holder, owner, yamlFilters.data(),
                             static_cast<int>(yamlFilters.size()),
                             defaultLocation.empty() ? nullptr
                                                     : defaultLocation.c_str());
    } else {
      SDL_ShowOpenFileDialog(callback, holder, owner, yamlFilters.data(),
                             static_cast<int>(yamlFilters.size()),
                             defaultLocation.empty() ? nullptr
                                                     : defaultLocation.c_str(),
                             false);
    }
  }

  void beginResourceFile(resource_manager::ResourceForm const& form,
                         SDL_Window* owner, bool draft,
                         std::string resourceNamespace = {},
                         std::string resourceName = {},
                         bool inlineResource = false,
                         std::size_t dependencyIndex = 0) {
    {
      std::lock_guard lock(mState->mutex);
      if (mState->pending) return;
      mState->pending = true;
      mState->purpose = Purpose::resourceFile;
      mState->draftFile = draft;
      mState->inlineResource = inlineResource;
      mState->dependencyIndex = dependencyIndex;
      mState->resourceNamespace = std::move(resourceNamespace);
      mState->resourceName = std::move(resourceName);
      mState->result.reset();
    }
    mResourceFilterName = form.fileKind;
    mResourceFilterPattern.clear();
    for (auto const& extension : form.fileExtensions) {
      if (!mResourceFilterPattern.empty()) mResourceFilterPattern += ';';
      mResourceFilterPattern += extension;
    }
    mResourceFilters = {{{mResourceFilterName.c_str(),
                          mResourceFilterPattern.c_str()},
                         {"All files", "*"}}};
    auto holder = new std::shared_ptr<State>(mState);
    SDL_ShowOpenFileDialog(callback, holder, owner, mResourceFilters.data(),
                           static_cast<int>(mResourceFilters.size()), nullptr,
                           false);
  }

  std::optional<Result> poll() {
    std::lock_guard lock(mState->mutex);
    if (!mState->result) return {};
    auto result = std::move(mState->result);
    mState->result.reset();
    return result;
  }

 private:
  struct State {
    mutable std::mutex mutex;
    bool pending = false;
    Purpose purpose = Purpose::none;
    bool draftFile = false;
    bool inlineResource = false;
    std::size_t dependencyIndex = 0;
    std::string resourceNamespace;
    std::string resourceName;
    std::optional<Result> result;
  };

  static void SDLCALL callback(void* userdata, char const* const* files, int) {
    std::unique_ptr<std::shared_ptr<State>> holder(
        static_cast<std::shared_ptr<State>*>(userdata));
    Result result;
    {
      std::lock_guard lock((*holder)->mutex);
      result.purpose = (*holder)->purpose;
      result.draftFile = (*holder)->draftFile;
      result.inlineResource = (*holder)->inlineResource;
      result.dependencyIndex = (*holder)->dependencyIndex;
      result.resourceNamespace = (*holder)->resourceNamespace;
      result.resourceName = (*holder)->resourceName;
    }
    if (!files) {
      result.error = SDL_GetError();
    } else if (files[0]) {
      result.path = fs::path(files[0]);
    }
    std::lock_guard lock((*holder)->mutex);
    (*holder)->result = std::move(result);
    (*holder)->pending = false;
  }

  inline static std::array<SDL_DialogFileFilter, 2> const yamlFilters{{
      {"Resource Manifests", "yaml;yml"}, {"All files", "*"}}};
  std::shared_ptr<State> mState = std::make_shared<State>();
  std::string mResourceFilterName;
  std::string mResourceFilterPattern;
  std::array<SDL_DialogFileFilter, 2> mResourceFilters{};
};

struct WorkspaceFrameResult {
  bool menuDrawn = false;
  bool toolbarDrawn = false;
  bool editorDrawn = false;
  bool fillsWorkArea = false;
};

WorkspaceFrameResult drawWorkspace(ManifestWorkspace& workspace,
                                   NativeDialog& dialog, SDL_Window* window,
                                   mpp::Logger& logger, bool& running) {
  WorkspaceFrameResult result;
  static std::string selectedNamespace;
  static std::string selectedName;
  static bool selectedNamespaceNode = false;
  static bool selectedNamespaceIsDraft = false;
  static std::string editingIdentity;
  static std::string editingNamespace;
  static bool selectedInline = false;
  static std::string selectedInlineOwner;
  static std::size_t selectedInlineIndex = 0;
  static std::array<char, 256> nameBuffer{};
  static std::array<char, 256> namespaceBuffer{};
  static std::array<char, 256> draftNameBuffer{};
  static std::array<char, 256> namespaceDraftNameBuffer{};
  auto setBuffer = [](auto& buffer, std::string const& value) {
    buffer.fill('\0');
    auto const length = (std::min)(value.size(), buffer.size() - 1U);
    std::copy_n(value.data(), length, buffer.data());
  };
  std::optional<std::string> beginDraftType;
  bool requestNamespaceDraft = false;
  bool requestUndo = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z,
                                     ImGuiInputFlags_RouteGlobal);
  bool requestRedo = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y,
                                     ImGuiInputFlags_RouteGlobal);
  bool requestNew = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N,
                                    ImGuiInputFlags_RouteGlobal);
  bool requestOpen = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O,
                                     ImGuiInputFlags_RouteGlobal);
  bool requestSave = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S,
                                     ImGuiInputFlags_RouteGlobal);
  bool requestSaveAs = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift |
                                           ImGuiKey_S,
                                       ImGuiInputFlags_RouteGlobal);
  if (requestSaveAs) requestSave = false;

  if (ImGui::BeginMainMenuBar()) {
    result.menuDrawn = true;
    if (ImGui::BeginMenu("File")) {
      requestNew |= ImGui::MenuItem("New...", "Ctrl+N", false, !dialog.busy());
      requestOpen |=
          ImGui::MenuItem("Open...", "Ctrl+O", false, !dialog.busy());
      requestSave |= ImGui::MenuItem("Save", "Ctrl+S", false,
                                     workspace.hasDocument());
      requestSaveAs |= ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false,
                                       workspace.hasDocument() && !dialog.busy());
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) running = false;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      requestUndo |= ImGui::MenuItem("Undo", "Ctrl+Z", false,
                                     workspace.canUndo());
      requestRedo |= ImGui::MenuItem("Redo", "Ctrl+Y", false,
                                     workspace.canRedo());
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Resource")) {
      ImGui::BeginDisabled(!workspace.hasDocument() || workspace.draft() ||
                           workspace.namespaceDraft());
      requestNamespaceDraft |= ImGui::MenuItem("Add namespace...");
      ImGui::EndDisabled();
      ImGui::BeginDisabled(!workspace.hasDocument() || workspace.draft() ||
                           (workspace.namespaceDraft() &&
                            !workspace.namespaceDraftValid()));
      if (ImGui::BeginMenu("Add Resource")) {
        for (auto const& form : workspace.resourceForms()) {
          if (ImGui::MenuItem(form.resourceType.c_str()))
            beginDraftType = form.resourceType;
        }
        ImGui::EndMenu();
      }
      ImGui::EndDisabled();
      ImGui::EndMenu();
    }
    for (auto const* menu : {"Schemas", "View", "Help"}) {
      if (ImGui::BeginMenu(menu)) {
        ImGui::TextDisabled("No commands available.");
        ImGui::EndMenu();
      }
    }
    ImGui::EndMainMenuBar();
  }

  if (ImGui::BeginViewportSideBar(
          "##ResourceManifestToolbar", ImGui::GetMainViewport(), ImGuiDir_Up,
          ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2.0f,
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
    result.toolbarDrawn = true;
    if (ImGui::Button("New") && !dialog.busy()) requestNew = true;
    ImGui::SameLine();
    if (ImGui::Button("Open") && !dialog.busy()) requestOpen = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!workspace.hasDocument());
    if (ImGui::Button("Save")) requestSave = true;
    ImGui::SameLine();
    if (ImGui::Button("Save As")) requestSaveAs = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!workspace.canUndo());
    if (ImGui::Button("Undo")) requestUndo = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!workspace.canRedo());
    if (ImGui::Button("Redo")) requestRedo = true;
    ImGui::EndDisabled();
  }
  ImGui::End();

  if (requestUndo) workspace.undo();
  if (requestRedo) workspace.redo();
  if (requestNamespaceDraft && workspace.beginNamespaceDraft()) {
    setBuffer(namespaceDraftNameBuffer, {});
    selectedNamespace.clear();
    selectedName.clear();
    selectedNamespaceNode = true;
    selectedNamespaceIsDraft = true;
    editingNamespace.clear();
  }
  if (beginDraftType) {
    std::string targetNamespace = selectedNamespace;
    if (!selectedNamespaceNode && selectedName.empty()) targetNamespace.clear();
    if (workspace.beginDraft(*beginDraftType, targetNamespace)) {
      setBuffer(draftNameBuffer, {});
    }
  }
  if (requestNew && !dialog.busy()) {
    dialog.begin(NativeDialog::Purpose::createNew, window,
                 workspace.baseDirectory().string());
  }
  if (requestOpen && !dialog.busy()) {
    dialog.begin(NativeDialog::Purpose::open, window);
  }
  if (requestSave && workspace.hasDocument()) {
    if (workspace.hasPath()) {
      if (workspace.save()) {
        logger.info("Saved Resource Manifest: " + displayPath(workspace.path()));
      } else {
        report(logger, workspace.operationDiagnostic());
      }
    } else if (!dialog.busy()) {
      auto suggested = workspace.baseDirectory() / "Resources.yaml";
      dialog.begin(NativeDialog::Purpose::saveAs, window, suggested.string());
    }
  }
  if (requestSaveAs && workspace.hasDocument() && !dialog.busy()) {
    auto suggested = workspace.hasPath()
                         ? workspace.path()
                         : workspace.baseDirectory() / "Resources.yaml";
    dialog.begin(NativeDialog::Purpose::saveAs, window, suggested.string());
  }

  auto* viewport = ImGui::GetMainViewport();
  auto const expectedPosition = viewport->WorkPos;
  auto const expectedSize = viewport->WorkSize;
  ImGui::SetNextWindowPos(expectedPosition, ImGuiCond_Always);
  ImGui::SetNextWindowSize(expectedSize, ImGuiCond_Always);
  constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("Resource Manifest Editor", nullptr, flags)) {
    result.editorDrawn = true;
    auto const position = ImGui::GetWindowPos();
    auto const size = ImGui::GetWindowSize();
    result.fillsWorkArea =
        std::abs(position.x - expectedPosition.x) < 1.0f &&
        std::abs(position.y - expectedPosition.y) < 1.0f &&
        std::abs(size.x - expectedSize.x) < 1.0f &&
        std::abs(size.y - expectedSize.y) < 1.0f;

    float const diagnosticsHeight = 145.0f;
    float const upperHeight =
        (std::max)(80.0f, ImGui::GetContentRegionAvail().y - diagnosticsHeight);
    float const treeWidth =
        (std::max)(180.0f, ImGui::GetContentRegionAvail().x * 0.32f);
    auto resources = workspace.resources();
    auto namespaces = workspace.namespaces();
    ImGui::BeginChild("Manifest tree", ImVec2(treeWidth, upperHeight), true);
    ImGui::TextUnformatted("Resource Manifest");
    if (workspace.hasDocument()) {
      auto acceptResourceDrop = [&](std::string const& targetNamespace,
                                    std::size_t targetIndex) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (auto const* payload =
                ImGui::AcceptDragDropPayload("RESOURCE_MANIFEST_RESOURCE")) {
          std::string packed(static_cast<char const*>(payload->Data),
                             static_cast<std::size_t>(payload->DataSize));
          auto const separator = packed.find('\n');
          if (separator != std::string::npos) {
            auto sourceNamespace = packed.substr(0, separator);
            auto sourceName = packed.substr(separator + 1U);
            if (sourceNamespace == targetNamespace &&
                targetIndex != static_cast<std::size_t>(-1)) {
              std::size_t sourceIndex = 0;
              for (auto const& resource : resources) {
                if (resource.resourceNamespace != sourceNamespace) continue;
                if (resource.name == sourceName) break;
                ++sourceIndex;
              }
              if (sourceIndex < targetIndex) --targetIndex;
            }
            if (workspace.moveResource(sourceNamespace, sourceName,
                                       targetNamespace, targetIndex)) {
              selectedNamespace = targetNamespace;
              selectedName = sourceName;
              selectedNamespaceNode = false;
              selectedNamespaceIsDraft = false;
              selectedInline = false;
              editingIdentity.clear();
            }
          }
        }
        ImGui::EndDragDropTarget();
      };
      for (auto const& item : namespaces) {
        auto const label = item.isDefault
                               ? std::string("Default namespace")
                               : item.name + (item.draft ? " (draft)" : "");
        ImGui::PushID(item.isDefault ? "##default-namespace"
                                     : item.name.c_str());
        auto const treeFlags = ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ((selectedNamespaceNode &&
                                 selectedNamespaceIsDraft == item.draft &&
                                 selectedNamespace == item.name)
                                    ? ImGuiTreeNodeFlags_Selected
                                    : 0);
        bool const open = ImGui::TreeNodeEx(label.c_str(), treeFlags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
          selectedNamespace = item.name;
          selectedName.clear();
          selectedNamespaceNode = true;
          selectedNamespaceIsDraft = item.draft;
          selectedInline = false;
          editingIdentity.clear();
          editingNamespace.clear();
        }
        if (!item.draft || workspace.namespaceDraftValid())
          acceptResourceDrop(item.name, static_cast<std::size_t>(-1));
        if (open) {
          std::size_t resourceIndex = 0;
          for (auto const& resource : resources) {
            if (resource.resourceNamespace != item.name) continue;
            bool const selected = !selectedNamespaceNode && !selectedInline &&
                                  selectedNamespace == item.name &&
                                  selectedName == resource.name;
            auto itemLabel =
                resource.name + " [" + resource.resourceType + "]";
            if (ImGui::Selectable(itemLabel.c_str(), selected)) {
              selectedNamespace = item.name;
              selectedName = resource.name;
              selectedNamespaceNode = false;
              selectedNamespaceIsDraft = false;
              selectedInline = false;
              editingIdentity.clear();
              editingNamespace.clear();
            }
            if (ImGui::BeginDragDropSource()) {
              auto packed = item.name + "\n" + resource.name;
              ImGui::SetDragDropPayload("RESOURCE_MANIFEST_RESOURCE",
                                        packed.data(), packed.size());
              ImGui::TextUnformatted(itemLabel.c_str());
              ImGui::EndDragDropSource();
            }
            acceptResourceDrop(item.name, resourceIndex);
            ImGui::Indent();
            for (auto const& inlineResource :
                 workspace.inlineResources(item.name, resource.name)) {
              ImGui::PushID(static_cast<int>(inlineResource.dependencyIndex));
              auto inlineLabel = inlineResource.name + " [" +
                                 inlineResource.resourceType + "] (inline)";
              bool const inlineSelected =
                  selectedInline && selectedNamespace == item.name &&
                  selectedInlineOwner == resource.name &&
                  selectedInlineIndex == inlineResource.dependencyIndex;
              if (ImGui::Selectable(inlineLabel.c_str(), inlineSelected)) {
                selectedNamespace = item.name;
                selectedName = inlineResource.name;
                selectedInlineOwner = resource.name;
                selectedInlineIndex = inlineResource.dependencyIndex;
                selectedNamespaceNode = false;
                selectedNamespaceIsDraft = false;
                selectedInline = true;
                editingIdentity.clear();
                editingNamespace.clear();
              }
              if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Owned by %s; promote before moving independently.",
                                  resource.name.c_str());
              }
              ImGui::PopID();
            }
            ImGui::Unindent();
            ++resourceIndex;
          }
          ImGui::TreePop();
        }
        ImGui::PopID();
      }
    } else {
      ImGui::TextDisabled("Choose New or Open to begin.");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Inspector", ImVec2(0.0f, upperHeight), true);
    if (auto const* draft = workspace.draft()) {
      ImGui::TextUnformatted("New Resource draft");
      ImGui::Text("Namespace: %s", draft->resourceNamespace.empty()
                                       ? "Default namespace"
                                       : draft->resourceNamespace.c_str());
      ImGui::Text("Resource Type: %s", draft->resourceType.c_str());
      ImGui::TextDisabled("Resource Type is immutable after creation.");
      if (ImGui::InputText("Name", draftNameBuffer.data(),
                           draftNameBuffer.size()))
        workspace.setDraftName(draftNameBuffer.data());
      ImGui::Text("Source file: %s", draft->location.empty()
                                          ? "Not selected"
                                          : draft->location.c_str());
      if (ImGui::Button("Select source file...") && !dialog.busy()) {
        if (auto const* form = workspace.resourceForm(draft->resourceType))
          dialog.beginResourceFile(*form, window, true);
      }
      if (!draft->validationMessage.empty())
        ImGui::TextWrapped("%s", draft->validationMessage.c_str());
      ImGui::BeginDisabled(!workspace.draftValid());
      if (ImGui::Button("Create")) {
        auto resourceNamespace = draft->resourceNamespace;
        auto name = draft->name;
        if (workspace.commitDraft()) {
          selectedNamespace = std::move(resourceNamespace);
          selectedName = std::move(name);
          selectedNamespaceNode = false;
          selectedNamespaceIsDraft = false;
          editingIdentity.clear();
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) workspace.cancelDraft();
    } else if (auto const* namespaceDraft = workspace.namespaceDraft()) {
      ImGui::TextUnformatted("New namespace draft");
      ImGui::TextDisabled(
          "The namespace is committed only with its first Resource.");
      if (ImGui::InputText("Name", namespaceDraftNameBuffer.data(),
                           namespaceDraftNameBuffer.size())) {
        workspace.setNamespaceDraftName(namespaceDraftNameBuffer.data());
        selectedNamespace = namespaceDraftNameBuffer.data();
      }
      if (!namespaceDraft->validationMessage.empty())
        ImGui::TextWrapped("%s", namespaceDraft->validationMessage.c_str());
      ImGui::BeginDisabled(!workspace.namespaceDraftValid());
      ImGui::TextUnformatted("Add first Resource:");
      for (auto const& form : workspace.resourceForms()) {
        if (ImGui::Button(form.resourceType.c_str()) &&
            workspace.beginDraft(form.resourceType, selectedNamespace)) {
          setBuffer(draftNameBuffer, {});
        }
      }
      ImGui::EndDisabled();
      if (ImGui::Button("Cancel namespace")) {
        workspace.cancelNamespaceDraft();
        selectedNamespace.clear();
        selectedNamespaceNode = false;
        selectedNamespaceIsDraft = false;
      }
    } else {
      auto selectedNamespaceSummary = std::find_if(
          namespaces.begin(), namespaces.end(), [&](auto const& item) {
            return !item.draft && item.name == selectedNamespace;
          });
      auto selected = std::find_if(
          resources.begin(), resources.end(), [&](auto const& resource) {
            return !selectedInline &&
                   resource.resourceNamespace == selectedNamespace &&
                   resource.name == selectedName;
          });
      auto selectedOwnerInlineResources = workspace.inlineResources(
          selectedNamespace, selectedInlineOwner);
      auto selectedInlineResource = std::find_if(
          selectedOwnerInlineResources.begin(), selectedOwnerInlineResources.end(),
          [&](auto const& resource) {
            return selectedInline &&
                   resource.dependencyIndex == selectedInlineIndex;
          });
      if (selectedNamespaceNode && selectedNamespaceSummary != namespaces.end()) {
        auto const& item = *selectedNamespaceSummary;
        ImGui::Text("Namespace: %s",
                    item.isDefault ? "Default namespace" : item.name.c_str());
        ImGui::Text("Resources: %zu", item.resourceCount);
        if (item.isDefault) {
          ImGui::TextDisabled(
              "The default namespace is permanent and cannot be renamed or deleted.");
        } else {
          if (editingNamespace != item.name) {
            editingNamespace = item.name;
            setBuffer(namespaceBuffer, item.name);
          }
          bool commitName = ImGui::InputText(
              "Name", namespaceBuffer.data(), namespaceBuffer.size(),
              ImGuiInputTextFlags_EnterReturnsTrue);
          commitName |= ImGui::IsItemDeactivatedAfterEdit();
          if (commitName && std::string(namespaceBuffer.data()) != item.name) {
            auto newName = std::string(namespaceBuffer.data());
            if (workspace.renameNamespace(item.name, newName)) {
              selectedNamespace = std::move(newName);
              editingNamespace.clear();
            } else {
              setBuffer(namespaceBuffer, item.name);
            }
          }
          if (ImGui::Button("Delete namespace..."))
            ImGui::OpenPopup("Confirm namespace deletion");
          if (ImGui::BeginPopupModal("Confirm namespace deletion", nullptr,
                                     ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete namespace '%s' and all %zu Resources?",
                        item.name.c_str(), item.resourceCount);
            if (ImGui::Button("Delete")) {
              if (workspace.deleteNamespace(item.name)) {
                selectedNamespace.clear();
                selectedName.clear();
                selectedNamespaceNode = true;
                selectedNamespaceIsDraft = false;
              }
              ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
          }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Add Resource:");
        for (auto const& form : workspace.resourceForms()) {
          if (ImGui::Button(form.resourceType.c_str()) &&
              workspace.beginDraft(form.resourceType, item.name)) {
            setBuffer(draftNameBuffer, {});
          }
        }
      } else if (selectedInlineResource != selectedOwnerInlineResources.end()) {
        auto const& inlineResource = *selectedInlineResource;
        ImGui::Text("Inline Resource Type: %s", inlineResource.resourceType.c_str());
        ImGui::Text("Owner: %s", inlineResource.ownerName.c_str());
        ImGui::TextDisabled(
            "Inline Resources are owned here and cannot be moved independently.");
        auto identity = inlineResource.resourceNamespace + "\n" +
                        inlineResource.ownerName + "\n" +
                        std::to_string(inlineResource.dependencyIndex);
        if (editingIdentity != identity) {
          editingIdentity = identity;
          setBuffer(nameBuffer, inlineResource.name);
        }
        bool commitName = ImGui::InputText(
            "Name", nameBuffer.data(), nameBuffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        commitName |= ImGui::IsItemDeactivatedAfterEdit();
        if (commitName &&
            std::string(nameBuffer.data()) != inlineResource.name) {
          auto newName = std::string(nameBuffer.data());
          if (workspace.renameInlineResource(
                  inlineResource.resourceNamespace, inlineResource.ownerName,
                  inlineResource.dependencyIndex, newName)) {
            selectedName = std::move(newName);
            editingIdentity.clear();
          } else {
            setBuffer(nameBuffer, inlineResource.name);
          }
        }
        if (!inlineResource.editable) {
          ImGui::TextDisabled("This inline Resource payload is read-only.");
        } else {
          ImGui::Text("Source file: %s", inlineResource.location.c_str());
          if (ImGui::Button("Select source file...") && !dialog.busy()) {
            if (auto const* form =
                    workspace.resourceForm(inlineResource.resourceType)) {
              dialog.beginResourceFile(
                  *form, window, false, inlineResource.resourceNamespace,
                  inlineResource.ownerName, true,
                  inlineResource.dependencyIndex);
            }
          }
          if (auto const* form = workspace.resourceForm(inlineResource.resourceType)) {
            for (auto const& option : form->options) {
              auto value = std::find_if(
                  inlineResource.options.begin(), inlineResource.options.end(),
                  [&](auto const& optionValue) {
                    return optionValue.first == option.name;
                  });
              std::string current = value == inlineResource.options.end()
                                        ? std::string{}
                                        : value->second;
              auto label = option.name + "##inline";
              auto const* preview = current.empty() ? "Not set" : current.c_str();
              if (ImGui::BeginCombo(label.c_str(), preview)) {
                if (ImGui::Selectable("Not set", current.empty())) {
                  workspace.setInlineResourceOption(
                      inlineResource.resourceNamespace, inlineResource.ownerName,
                      inlineResource.dependencyIndex, option.name, {});
                }
                auto drawChoice = [&](std::string const& choice) {
                  if (ImGui::Selectable(choice.c_str(), current == choice)) {
                    workspace.setInlineResourceOption(
                        inlineResource.resourceNamespace, inlineResource.ownerName,
                        inlineResource.dependencyIndex, option.name, choice);
                  }
                };
                for (auto const& choice : option.values) drawChoice(choice);
                if (option.boolean) {
                  drawChoice("true");
                  drawChoice("false");
                }
                ImGui::EndCombo();
              }
            }
          }
        }
        if (ImGui::Button("Promote to namespace")) {
          auto promotedName = inlineResource.name;
          if (workspace.promoteInlineResource(
                  inlineResource.resourceNamespace, inlineResource.ownerName,
                  inlineResource.dependencyIndex,
                  inlineResource.resourceNamespace)) {
            selectedName = std::move(promotedName);
            selectedInline = false;
            selectedInlineOwner.clear();
            editingIdentity.clear();
          }
        }
      } else if (selected != resources.end()) {
        ImGui::Text("Resource Type: %s", selected->resourceType.c_str());
        ImGui::TextDisabled("Resource Type is immutable; recreate it to change type.");
        auto identity = selected->resourceNamespace + "\n" + selected->name;
        if (editingIdentity != identity) {
          editingIdentity = identity;
          setBuffer(nameBuffer, selected->name);
        }
        bool commitName = ImGui::InputText(
            "Name", nameBuffer.data(), nameBuffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        commitName |= ImGui::IsItemDeactivatedAfterEdit();
        if (commitName && std::string(nameBuffer.data()) != selected->name) {
          auto newName = std::string(nameBuffer.data());
          if (workspace.renameResource(selected->resourceNamespace,
                                       selected->name, newName)) {
            selectedName = std::move(newName);
            editingIdentity.clear();
          } else {
            setBuffer(nameBuffer, selected->name);
          }
        }
        if (!selected->editable) {
          ImGui::TextDisabled(
              "Type-specific properties are read-only; common organization is available.");
        } else {
          ImGui::Text("Source file: %s", selected->location.c_str());
          if (ImGui::Button("Select source file...") && !dialog.busy()) {
            if (auto const* form = workspace.resourceForm(selected->resourceType))
              dialog.beginResourceFile(*form, window, false,
                                       selected->resourceNamespace,
                                       selected->name);
          }
          if (auto const* form = workspace.resourceForm(selected->resourceType)) {
            for (auto const& option : form->options) {
              auto value = std::find_if(
                  selected->options.begin(), selected->options.end(),
                  [&](auto const& optionValue) {
                    return optionValue.first == option.name;
                  });
              std::string current = value == selected->options.end()
                                        ? std::string{}
                                        : value->second;
              auto const* preview = current.empty() ? "Not set" : current.c_str();
              if (ImGui::BeginCombo(option.name.c_str(), preview)) {
                if (ImGui::Selectable("Not set", current.empty()))
                  workspace.setResourceOption(selected->resourceNamespace,
                                              selected->name, option.name, {});
                auto drawChoice = [&](std::string const& choice) {
                  if (ImGui::Selectable(choice.c_str(), current == choice))
                    workspace.setResourceOption(selected->resourceNamespace,
                                                selected->name, option.name,
                                                choice);
                };
                for (auto const& choice : option.values) drawChoice(choice);
                if (option.boolean) {
                  drawChoice("true");
                  drawChoice("false");
                }
                ImGui::EndCombo();
              }
            }
          }
        }
        auto referenceSelectors = workspace.resourceReferences(
            selected->resourceNamespace, selected->name);
        if (!referenceSelectors.empty()) {
          ImGui::Separator();
          ImGui::TextUnformatted("Resource dependencies");
          for (auto const& selector : referenceSelectors) {
            ImGui::PushID(static_cast<int>(selector.dependencyIndex));
            auto current = std::find_if(
                selector.choices.begin(), selector.choices.end(),
                [](auto const& choice) { return choice.selected; });
            std::string preview = current == selector.choices.end()
                                      ? selector.reference
                                      : current->qualifiedIdentity;
            if (current != selector.choices.end() && current->missing) {
              preview += " (missing)";
            } else if (current != selector.choices.end() &&
                       current->inlineResource) {
              preview += " (inline)";
            }
            auto label = selector.dependencyId.empty()
                             ? std::string("Dependency")
                             : selector.dependencyId;
            if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
              for (auto const& choice : selector.choices) {
                auto choiceLabel = choice.qualifiedIdentity;
                if (!choice.resourceType.empty()) {
                  choiceLabel += " [" + choice.resourceType + "]";
                }
                if (choice.missing) choiceLabel += " (missing)";
                if (choice.inlineResource) choiceLabel += " (inline)";
                ImGui::BeginDisabled(choice.disabled);
                if (ImGui::Selectable(choiceLabel.c_str(), choice.selected)) {
                  workspace.setResourceReference(
                      selector.ownerNamespace, selector.ownerName,
                      selector.dependencyIndex,
                      std::pair{choice.resourceNamespace, choice.name});
                }
                ImGui::EndDisabled();
                if (choice.disabled && ImGui::IsItemHovered() &&
                    !choice.reason.empty()) {
                  ImGui::SetTooltip("%s", choice.reason.c_str());
                }
              }
              ImGui::EndCombo();
            }
            if (selector.clearable) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Clear")) {
                workspace.setResourceReference(
                    selector.ownerNamespace, selector.ownerName,
                    selector.dependencyIndex, {});
              }
            }
            ImGui::PopID();
          }
        }
        if (ImGui::Button("Delete Resource..."))
          ImGui::OpenPopup("Confirm Resource deletion");
        if (ImGui::BeginPopupModal("Confirm Resource deletion", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
          ImGui::Text("Delete Resource '%s'?", selected->name.c_str());
          auto namespaceEntry = std::find_if(
              namespaces.begin(), namespaces.end(), [&](auto const& item) {
                return item.name == selected->resourceNamespace;
              });
          if (!selected->resourceNamespace.empty() &&
              namespaceEntry != namespaces.end() &&
              namespaceEntry->resourceCount == 1U) {
            ImGui::TextWrapped(
                "This is the final Resource; namespace '%s' will also be removed.",
                selected->resourceNamespace.c_str());
          }
          if (ImGui::Button("Delete")) {
            if (workspace.deleteResource(selected->resourceNamespace,
                                         selected->name)) {
              selectedNamespace.clear();
              selectedName.clear();
              selectedNamespaceNode = true;
              selectedNamespaceIsDraft = false;
            }
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
      } else {
        ImGui::TextUnformatted("Document");
        if (workspace.hasDocument()) {
          ImGui::TextWrapped("Manifest: %s",
                             workspace.hasPath()
                                 ? displayPath(workspace.path()).c_str()
                                 : "Unsaved Resource Manifest");
          ImGui::TextWrapped("Base directory: %s",
                             displayPath(workspace.baseDirectory()).c_str());
          ImGui::Text("State: %s",
                      workspace.dirty() ? "Unsaved changes" : "Saved");
        } else {
          ImGui::TextDisabled("No Resource Manifest is open.");
        }
      }
    }
    ImGui::EndChild();
    ImGui::BeginChild("Diagnostics", ImVec2(0.0f, 0.0f), true);
    ImGui::TextUnformatted("Diagnostics");
    bool hasDiagnostics = false;
    if (!workspace.operationDiagnostic().empty()) {
      ImGui::TextWrapped("%s", workspace.operationDiagnostic().c_str());
      hasDiagnostics = true;
    }
    for (auto const& diagnostic : workspace.structuralDiagnostics()) {
      ImGui::BulletText("%s", diagnostic.message.c_str());
      hasDiagnostics = true;
    }
    for (auto const& diagnostic : workspace.dependencyDiagnostics()) {
      ImGui::BulletText("%s%s", diagnostic.error ? "Error: " : "Info: ",
                        diagnostic.message.c_str());
      hasDiagnostics = true;
    }
    if (!hasDiagnostics) ImGui::TextDisabled("No errors.");
    ImGui::EndChild();
  }
  ImGui::End();
  return result;
}

int runDesktop(DesktopArguments arguments) {
  mpp::Logger logger;
  fs::path logPath;
  std::string logFailure;
  if (!initialiseLog(logger, logPath, logFailure)) {
    std::fprintf(stderr, "Resource Manifest Editor logging failure: %s\n",
                 logFailure.c_str());
    return serializationFailure;
  }

  if (arguments.iniPath.empty()) {
    try {
      arguments.iniPath = executableDirectory() / "resource-manager.ini";
    } catch (std::exception const& exception) {
      report(logger, exception.what());
      return configurationFailure;
    }
  }

  EditorConfiguration configuration;
  try {
    configuration = loadConfiguration(arguments.iniPath);
    logger.info("Loaded deployment INI: " + displayPath(arguments.iniPath));
  } catch (std::exception const& exception) {
    report(logger, "Resource Manifest Editor configuration failure: " +
                       std::string(exception.what()));
    return configurationFailure;
  }
  if (arguments.startupCheck) {
    std::cout << "Resource Manifest Editor startup check passed; log: "
              << displayPath(logPath) << '\n';
    return success;
  }

  ManifestWorkspace workspace;
  fs::path smokeRoot;
  struct SmokeCleanup {
    fs::path* path;
    ~SmokeCleanup() {
      if (!path->empty()) {
        std::error_code ignored;
        fs::remove_all(*path, ignored);
      }
    }
  } smokeCleanup{&smokeRoot};
  if (arguments.smokeTest) {
    smokeRoot = fs::temp_directory_path() /
                ("willpower-resource-manager-smoke-" +
                 std::to_string(SDL_GetTicksNS()));
    fs::create_directories(smokeRoot / "base");
    auto target = smokeRoot / "Smoke.yaml";
    if (!workspace.createNew(smokeRoot / "base") || !workspace.saveAs(target) ||
        !workspace.open(target)) {
      report(logger, "Resource Manifest Editor smoke document workflow failed: " +
                         workspace.operationDiagnostic());
      return internalFailure;
    }
    logger.info("Smoke document New, Save As, and Open workflow completed.");
  } else if (!arguments.startupManifest.empty()) {
    if (!workspace.open(arguments.startupManifest)) {
      report(logger, workspace.operationDiagnostic());
    } else {
      logger.info("Opened startup Resource Manifest: " +
                  displayPath(workspace.path()));
    }
  }

#if defined(__linux__)
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    auto const message = std::string("Resource Manifest Editor GUI initialization failed: ") +
                         SDL_GetError();
    report(logger, message);
#if defined(__linux__)
    if (arguments.smokeTest && !environmentValue("DISPLAY")) return 77;
#endif
    return guiFailure;
  }
  SdlLifetime sdl;

  try {
    WindowSDL window("Resource Manifest Editor");
    window.create(1280, 800, false, true);
    mpp::RenderSystem renderSystem(window.getWidth(), window.getHeight(), &logger,
                                   configuration.renderOptions);
    auto resourceManager = std::make_unique<mpp::ResourceManager>(&renderSystem, &logger);
    renderSystem.createCoreResources(resourceManager.get());

    ImGuiBackendData backend{};
    imGuiSetup(&renderSystem, resourceManager.get(), &backend, false);
    auto font = resourceManager->getResource("__ImGui_Font__", true);
    auto provider = std::make_shared<ImGuiDataProvider>(
        std::vector<mpp::ResourcePtr>{font});
    mpp::BufferRenderer renderer(provider);
    renderSystem.getOrCreateRenderPipeline("ResourceManifestEditor.UI");

    NativeDialog dialog;
    InputManagerSDL input;
    bool running = true;
    unsigned int smokeFrames = 0;
    bool smokeResized = false;
    while (running) {
      bool const closeRequested = !window.processEvents(&input);
      imGuiHandleInput(&input, &backend);
      input.update();
      if (closeRequested) running = false;
      if (window.getWidth() > 0 && window.getHeight() > 0 &&
          (static_cast<std::size_t>(window.getWidth()) !=
               renderSystem.getWindowWidth() ||
           static_cast<std::size_t>(window.getHeight()) !=
               renderSystem.getWindowHeight())) {
        renderSystem.setDisplay(window.getWidth(), window.getHeight());
      }

      if (auto result = dialog.poll()) {
        if (!result->error.empty()) {
          workspace.reportFailure("Native selector failed: " + result->error);
          report(logger, workspace.operationDiagnostic());
        } else if (result->path) {
          bool completed = false;
          switch (result->purpose) {
            case NativeDialog::Purpose::createNew:
              completed = workspace.createNew(*result->path);
              break;
            case NativeDialog::Purpose::open:
              completed = workspace.open(*result->path);
              break;
            case NativeDialog::Purpose::saveAs:
              completed = workspace.saveAs(*result->path);
              break;
            case NativeDialog::Purpose::resourceFile:
              if (result->draftFile) {
                completed = workspace.selectDraftFile(*result->path);
              } else if (result->inlineResource) {
                completed = workspace.setInlineResourceFile(
                    result->resourceNamespace, result->resourceName,
                    result->dependencyIndex, *result->path);
              } else {
                completed = workspace.setResourceFile(
                    result->resourceNamespace, result->resourceName,
                    *result->path);
              }
              break;
            case NativeDialog::Purpose::none:
              break;
          }
          if (!completed && !workspace.operationDiagnostic().empty()) {
            report(logger, workspace.operationDiagnostic());
          } else if (completed) {
            switch (result->purpose) {
              case NativeDialog::Purpose::createNew:
                logger.info("Created empty Resource Manifest with base directory: " +
                            displayPath(workspace.baseDirectory()));
                break;
              case NativeDialog::Purpose::open:
                logger.info("Opened Resource Manifest: " +
                            displayPath(workspace.path()));
                break;
              case NativeDialog::Purpose::saveAs:
                logger.info("Saved Resource Manifest: " +
                            displayPath(workspace.path()));
                break;
              case NativeDialog::Purpose::resourceFile:
                logger.info("Selected a contained Resource source file.");
                break;
              case NativeDialog::Purpose::none:
                break;
            }
          }
        }
      }

      imGuiNewFrame(window.getWindow(), &backend);
      ImGui::NewFrame();
      auto frame =
          drawWorkspace(workspace, dialog, window.getWindow(), logger, running);
      SDL_SetWindowTitle(
          window.getWindow(),
          workspace.hasDocument()
              ? ((workspace.hasPath() ? workspace.path().filename().string()
                                      : std::string("Untitled")) +
                 (workspace.dirty() ? " * - Resource Manifest Editor"
                                    : " - Resource Manifest Editor"))
                    .c_str()
              : "Resource Manifest Editor");
      ImGui::Render();
      provider->setDrawData(ImGui::GetDrawData());
      glViewport(0, 0, static_cast<GLsizei>(renderSystem.getWindowWidth()),
                 static_cast<GLsizei>(renderSystem.getWindowHeight()));
      glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      renderer.render(&renderSystem);
      window.show();

      if (arguments.smokeTest) {
        if (!frame.menuDrawn || !frame.toolbarDrawn || !frame.editorDrawn ||
            !frame.fillsWorkArea) {
          throw std::runtime_error(
              "Persistent workspace did not fill the viewport beneath the toolbar.");
        }
        if (smokeFrames == 1) window.setSize(960, 700);
        if (window.getWidth() == 960 && window.getHeight() == 700) {
          smokeResized = true;
        }
        if (++smokeFrames >= 8) {
          if (!smokeResized) {
            throw std::runtime_error(
                "Native resize did not reach the Resource Manifest Editor workspace.");
          }
          running = false;
        }
      }
    }

    imGuiShutdown(&backend);
    provider->clearRegisteredTextures();
    font.reset();
    if (resourceManager->getResource("__ImGui_Font__", true)) {
      resourceManager->deleteResource("__ImGui_Font__");
    }
    renderSystem.removeRenderPipeline("ResourceManifestEditor.UI");
    renderSystem.destroyCoreResources();
    logger.info("Resource Manifest Editor shutdown.");
    if (arguments.smokeTest) {
      std::cout << "Resource Manifest Editor smoke test passed: startup, new, "
                   "save, open, resize, menu, toolbar, and persistent workspace.\n";
    }
    return success;
  } catch (std::exception const& exception) {
    report(logger, std::string("Resource Manifest Editor GUI failure: ") +
                       exception.what());
    return arguments.smokeTest ? internalFailure : guiFailure;
  }
}

}  // namespace

int main(int argc, char const* const* argv) {
  try {
    if (argc == 2 &&
        (std::string_view(argv[1]) == "--help" ||
         std::string_view(argv[1]) == "-h")) {
      printUsage(std::cout);
      return success;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--validate") {
      ValidationArguments arguments;
      if (!parseValidationArguments(argc, argv, arguments)) {
        printUsage(std::cerr);
        return usageFailure;
      }
      return validateManifest(arguments);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--document-tests") {
      std::string failure;
      if (!resource_manager::runDocumentTests(&failure)) {
        std::cerr << "Resource Manifest Editor document tests failed: " << failure
                  << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor document tests passed.\n";
      return success;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--authoring-tests") {
      std::string failure;
      if (!resource_manager::runAuthoringTests(&failure)) {
        std::cerr << "Resource Manifest Editor authoring tests failed: " << failure
                  << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor authoring tests passed.\n";
      return success;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--organization-tests") {
      std::string failure;
      if (!resource_manager::runOrganizationTests(&failure)) {
        std::cerr << "Resource Manifest Editor organization tests failed: "
                  << failure << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor organization tests passed.\n";
      return success;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--dependency-tests") {
      std::string failure;
      if (!resource_manager::runDependencyAuthoringTests(&failure)) {
        std::cerr << "Resource Manifest Editor dependency tests failed: "
                  << failure << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor dependency tests passed.\n";
      return success;
    }

    DesktopArguments arguments;
    if (!parseDesktopArguments(argc, argv, arguments)) {
      printUsage(std::cerr);
      return usageFailure;
    }
    return runDesktop(std::move(arguments));
  } catch (std::exception const& exception) {
    std::cerr << "Internal resource-manager failure: " << exception.what() << '\n';
    return internalFailure;
  }
}
