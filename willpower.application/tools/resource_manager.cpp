#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
            "  resource-manager --composite-tests\n"
            "  resource-manager --advanced-tests\n"
            "  resource-manager --schema-tests\n"
            "  resource-manager --semantic-tests\n"
            "  resource-manager --resilience-tests\n"
            "  resource-manager --verify-schemas [--ini FILE]\n"
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
  ResourceSchemaCatalogSnapshot catalog;
};

EditorConfiguration loadConfiguration(fs::path const& path) {
  EditorConfiguration result;
  auto schemaSources = resource_manager::readEditorSchemaConfiguration(path);
  result.catalog = resource_manager::loadEditorSchemaCatalog(schemaSources);
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
  enum class Purpose {
    none,
    createNew,
    changeBaseDirectory,
    open,
    saveAs,
    resourceFile
  };
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
    if (purpose == Purpose::createNew ||
        purpose == Purpose::changeBaseDirectory) {
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
                                   mpp::Logger& logger,
                                   fs::path const& deploymentIni,
                                   bool& running, bool closeRequested = false) {
  WorkspaceFrameResult result;
  static std::string selectedNamespace;
  static std::string selectedName;
  static std::string selectedResourcePath;
  static std::string selectedNamespacePath;
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
  static std::map<std::string, std::array<char, 256>> nestedBuffers;
  static std::array<char, 256> materialDependencyIdBuffer{};
  static std::string selectedNestedPath;
  enum class PendingDestructive { none, createNew, open, exit };
  static PendingDestructive pendingDestructive = PendingDestructive::none;
  static bool confirmOverwrite = false;
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
  bool requestChangeBase = false;
  bool requestExit = closeRequested;
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
                                     workspace.canSave());
      requestSaveAs |= ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false,
                                       workspace.canSaveAs() && !dialog.busy());
      requestChangeBase |= ImGui::MenuItem(
          "Change Base Directory...", nullptr, false,
          workspace.hasDocument() && !dialog.busy());
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) requestExit = true;
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
    if (ImGui::BeginMenu("Schemas")) {
      if (ImGui::MenuItem("Reload configuration and bundles")) {
        if (workspace.reloadSchemas(deploymentIni)) {
          logger.info("Reloaded Resource Schema configuration: " +
                      displayPath(deploymentIni));
        } else {
          report(logger, workspace.operationDiagnostic());
        }
      }
      ImGui::EndMenu();
    }
    for (auto const* menu : {"View", "Help"}) {
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
    ImGui::BeginDisabled(!workspace.canSave());
    if (ImGui::Button("Save")) requestSave = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!workspace.canSaveAs());
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
    selectedResourcePath.clear();
    selectedNamespacePath.clear();
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
  auto requestDestructive = [&](PendingDestructive action) {
    if (workspace.dirty()) {
      pendingDestructive = action;
      ImGui::OpenPopup("Unsaved Resource Manifest");
      return false;
    }
    return true;
  };
  if (requestNew && !dialog.busy() &&
      requestDestructive(PendingDestructive::createNew)) {
    dialog.begin(NativeDialog::Purpose::createNew, window,
                 workspace.baseDirectory().string());
  }
  if (requestOpen && !dialog.busy() &&
      requestDestructive(PendingDestructive::open)) {
    dialog.begin(NativeDialog::Purpose::open, window);
  }
  if (requestExit && requestDestructive(PendingDestructive::exit)) running = false;

  if (requestChangeBase && !dialog.busy()) {
    dialog.begin(NativeDialog::Purpose::changeBaseDirectory, window,
                 workspace.baseDirectory().string());
  }
  if (requestSave && workspace.canSave()) {
    if (workspace.hasPath()) {
      auto external = workspace.checkExternalChange();
      if (external != resource_manager::ExternalChangeState::unchanged) {
        confirmOverwrite = true;
        ImGui::OpenPopup("Confirm external overwrite");
      } else if (workspace.save()) {
        logger.info("Saved Resource Manifest: " + displayPath(workspace.path()));
      } else {
        report(logger, workspace.operationDiagnostic());
      }
    } else if (!dialog.busy()) {
      auto suggested = workspace.baseDirectory() / "Resources.yaml";
      dialog.begin(NativeDialog::Purpose::saveAs, window, suggested.string());
    }
  }
  if (requestSaveAs && workspace.canSaveAs() && !dialog.busy()) {
    auto suggested = workspace.hasPath()
                         ? workspace.path()
                         : workspace.baseDirectory() / "Resources.yaml";
    dialog.begin(NativeDialog::Purpose::saveAs, window, suggested.string());
  }

  auto completeDestructive = [&] {
    auto action = pendingDestructive;
    pendingDestructive = PendingDestructive::none;
    if (action == PendingDestructive::createNew) {
      dialog.begin(NativeDialog::Purpose::createNew, window,
                   workspace.baseDirectory().string());
    } else if (action == PendingDestructive::open) {
      dialog.begin(NativeDialog::Purpose::open, window);
    } else if (action == PendingDestructive::exit) {
      running = false;
    }
  };
  if (ImGui::BeginPopupModal("Unsaved Resource Manifest", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("This Resource Manifest has unsaved changes.");
    if (workspace.hasPath() && workspace.canSave()) {
      if (ImGui::Button("Save")) {
        if (workspace.save()) {
          completeDestructive();
          ImGui::CloseCurrentPopup();
        } else {
          report(logger, workspace.operationDiagnostic());
        }
      }
      ImGui::SameLine();
    }
    if (ImGui::Button("Discard")) {
      if (pendingDestructive == PendingDestructive::exit)
        workspace.discardRecovery();
      completeDestructive();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      pendingDestructive = PendingDestructive::none;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (confirmOverwrite &&
      ImGui::BeginPopupModal("Confirm external overwrite", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("Overwrite the externally changed source with local content?");
    if (ImGui::Button("Overwrite")) {
      if (!workspace.overwriteExternal()) report(logger, workspace.operationDiagnostic());
      confirmOverwrite = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      confirmOverwrite = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
    if (workspace.hasNewerRecovery()) {
      ImGui::TextWrapped("Newer recovery data is available for this Resource Manifest.");
      if (ImGui::Button("Recover")) workspace.recover();
      ImGui::SameLine();
      if (ImGui::Button("Discard recovery")) workspace.discardRecovery();
      ImGui::Separator();
    }
    auto externalState = workspace.externalChangeState();
    if (externalState != resource_manager::ExternalChangeState::unchanged) {
      bool const dirtyConflict =
          externalState == resource_manager::ExternalChangeState::dirtyConflict ||
          (externalState == resource_manager::ExternalChangeState::missing &&
           workspace.dirty());
      ImGui::TextWrapped(dirtyConflict
                             ? "The source changed externally and conflicts with unsaved edits."
                             : "The source changed externally.");
      if (externalState != resource_manager::ExternalChangeState::missing &&
          ImGui::Button(dirtyConflict ? "Discard edits and reload" : "Reload")) {
        workspace.reloadExternal();
      }
      if (dirtyConflict) {
        ImGui::SameLine();
        if (ImGui::Button("Overwrite source...")) {
          confirmOverwrite = true;
          ImGui::OpenPopup("Confirm external overwrite");
        }
      }
      ImGui::Separator();
    }
    auto const position = ImGui::GetWindowPos();
    auto const size = ImGui::GetWindowSize();
    result.fillsWorkArea =
        std::abs(position.x - expectedPosition.x) < 1.0f &&
        std::abs(position.y - expectedPosition.y) < 1.0f &&
        std::abs(size.x - expectedSize.x) < 1.0f &&
        std::abs(size.y - expectedSize.y) < 1.0f;

    float const diagnosticsHeight = workspace.preferences().diagnosticsHeight();
    float const upperHeight =
        (std::max)(80.0f, ImGui::GetContentRegionAvail().y - diagnosticsHeight);
    float const treeWidth =
        (std::max)(180.0f, ImGui::GetContentRegionAvail().x *
                               workspace.preferences().treeFraction());
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
              selectedResourcePath.clear();
              selectedNamespacePath.clear();
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
                                     : (item.draft ? item.name.c_str()
                                                   : item.instancePath.c_str()));
        auto const resourcePathPrefix =
            item.isDefault ? std::string("/Resources/Resource/")
                           : item.instancePath + "/Resource/";
        bool const exactNamespaceSelection =
            selectedNamespacePath.empty() ||
            selectedNamespacePath == item.instancePath;
        bool const resourceSelectionInNamespace =
            !selectedResourcePath.empty() &&
            selectedResourcePath.starts_with(resourcePathPrefix);
        auto const treeFlags = ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ((selectedNamespaceNode &&
                                 selectedNamespaceIsDraft == item.draft &&
                                 selectedNamespace == item.name &&
                                 exactNamespaceSelection)
                                    ? ImGuiTreeNodeFlags_Selected
                                    : 0);
        if (!selectedNamespaceIsDraft && selectedNamespace == item.name &&
            ((selectedNamespaceNode && exactNamespaceSelection) ||
             resourceSelectionInNamespace ||
             (selectedResourcePath.empty() && !selectedName.empty()))) {
          ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        bool const open = ImGui::TreeNodeEx(label.c_str(), treeFlags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
          selectedNamespace = item.name;
          selectedName.clear();
          selectedNamespacePath = item.instancePath;
          selectedResourcePath.clear();
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
            if (resource.resourceNamespace != item.name ||
                (!item.draft &&
                 !resource.instancePath.starts_with(resourcePathPrefix))) {
              continue;
            }
            bool const selected =
                !selectedNamespaceNode && !selectedInline &&
                selectedNamespace == item.name &&
                selectedName == resource.name &&
                (selectedResourcePath.empty() ||
                 selectedResourcePath == resource.instancePath);
            auto itemLabel =
                resource.name + " [" + resource.resourceType + "]";
            auto selectableLabel = itemLabel + "##" + resource.instancePath;
            if (ImGui::Selectable(selectableLabel.c_str(), selected)) {
              selectedNamespace = item.name;
              selectedName = resource.name;
              selectedResourcePath = resource.instancePath;
              selectedNamespacePath.clear();
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
                selectedResourcePath = resource.instancePath;
                selectedNamespacePath.clear();
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
      auto const* draftForm = workspace.resourceForm(draft->resourceType);
      for (auto const& reference : draft->references) {
        auto choices = workspace.draftReferenceChoices(reference.id);
        std::string preview = reference.name.empty()
                                  ? "Select compatible Resource"
                                  : (reference.resourceNamespace.empty()
                                         ? reference.name
                                         : reference.resourceNamespace + "/" +
                                               reference.name);
        auto label = reference.id + " dependency";
        if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
          for (auto const& choice : choices) {
            ImGui::BeginDisabled(choice.disabled);
            if (ImGui::Selectable(choice.qualifiedIdentity.c_str(),
                                  choice.selected)) {
              workspace.setDraftReference(reference.id,
                                          choice.resourceNamespace,
                                          choice.name);
            }
            ImGui::EndDisabled();
          }
          ImGui::EndCombo();
        }
      }
      if (draftForm && !draftForm->fileProperty.empty()) {
        ImGui::Text("Source file: %s", draft->location.empty()
                                            ? "Not selected"
                                            : draft->location.c_str());
        if (ImGui::Button("Select source file...") && !dialog.busy()) {
          dialog.beginResourceFile(*draftForm, window, true);
        }
      }
      if (draftForm && draftForm->requiresDefinition) {
        ImGui::TextDisabled(
            "A valid starter Definition is created and can be extended after creation.");
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
          selectedResourcePath.clear();
          selectedNamespacePath.clear();
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
            return !item.draft && item.name == selectedNamespace &&
                   (selectedNamespacePath.empty() ||
                    item.instancePath == selectedNamespacePath);
          });
      auto selected = std::find_if(
          resources.begin(), resources.end(), [&](auto const& resource) {
            return !selectedInline &&
                   resource.resourceNamespace == selectedNamespace &&
                   resource.name == selectedName &&
                   (selectedResourcePath.empty() ||
                    resource.instancePath == selectedResourcePath);
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
          bool const focusName =
              !selectedNestedPath.empty() &&
              selectedNestedPath.ends_with(item.instancePath + "/name");
          if (focusName) ImGui::SetKeyboardFocusHere();
          bool commitName = ImGui::InputText(
              "Name", namespaceBuffer.data(), namespaceBuffer.size(),
              ImGuiInputTextFlags_EnterReturnsTrue);
          commitName |= ImGui::IsItemDeactivatedAfterEdit();
          if (focusName) {
            ImGui::SetScrollHereY(0.5f);
            selectedNestedPath.clear();
          }
          if (commitName && std::string(namespaceBuffer.data()) != item.name) {
            auto newName = std::string(namespaceBuffer.data());
            bool const renamed =
                selectedNamespacePath.empty()
                    ? workspace.renameNamespace(item.name, newName)
                    : workspace.renameNamespaceAtPath(selectedNamespacePath,
                                                       newName);
            if (renamed) {
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
                selectedResourcePath.clear();
                selectedNamespacePath.clear();
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
        bool const focusInlineName =
            !selectedNestedPath.empty() &&
            selectedNestedPath.ends_with(inlineResource.instancePath + "/name");
        if (focusInlineName) ImGui::SetKeyboardFocusHere();
        bool commitName = ImGui::InputText(
            "Name", nameBuffer.data(), nameBuffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        commitName |= ImGui::IsItemDeactivatedAfterEdit();
        if (focusInlineName) {
          ImGui::SetScrollHereY(0.5f);
          selectedNestedPath.clear();
        }
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
          ImGui::TextWrapped("%s", inlineResource.limitationWarning.c_str());
        } else {
          ImGui::Text("Source file: %s", inlineResource.location.c_str());
          bool const focusInlineFile =
              !selectedNestedPath.empty() &&
              selectedNestedPath.ends_with(inlineResource.instancePath +
                                             "/location");
          if (focusInlineFile) ImGui::SetKeyboardFocusHere();
          if (ImGui::Button("Select source file...") && !dialog.busy()) {
            if (auto const* form =
                    workspace.resourceForm(inlineResource.resourceType)) {
              dialog.beginResourceFile(
                  *form, window, false, inlineResource.resourceNamespace,
                  inlineResource.ownerName, true,
                  inlineResource.dependencyIndex);
            }
          }
          if (focusInlineFile) {
            ImGui::SetScrollHereY(0.5f);
            selectedNestedPath.clear();
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
            selectedResourcePath.clear();
            selectedNamespacePath.clear();
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
        bool const focusName =
            !selectedNestedPath.empty() &&
            selectedNestedPath.ends_with(selected->instancePath + "/name");
        if (focusName) ImGui::SetKeyboardFocusHere();
        bool commitName = ImGui::InputText(
            "Name", nameBuffer.data(), nameBuffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        commitName |= ImGui::IsItemDeactivatedAfterEdit();
        if (focusName) {
          ImGui::SetScrollHereY(0.5f);
          selectedNestedPath.clear();
        }
        if (commitName && std::string(nameBuffer.data()) != selected->name) {
          auto newName = std::string(nameBuffer.data());
          bool const renamed = selected->instancePath.empty()
                                   ? workspace.renameResource(
                                         selected->resourceNamespace,
                                         selected->name, newName)
                                   : workspace.renameResourceAtPath(
                                         selected->instancePath, newName);
          if (renamed) {
            selectedName = std::move(newName);
            editingIdentity.clear();
          } else {
            setBuffer(nameBuffer, selected->name);
          }
        }
        auto const* selectedForm = workspace.resourceForm(selected->resourceType);
        if (!selected->editable) {
          ImGui::TextWrapped("%s", selected->limitationWarning.c_str());
          ImGui::TextDisabled(
              "Safe common rename, move, reorder, and delete operations remain available.");
        } else if (selectedForm && selectedForm->composite) {
          ImGui::Separator();
          ImGui::TextUnformatted("Definition");
          auto factoryChoices = workspace.definitionFactories(
              selected->resourceNamespace, selected->name);
          auto availableFactory = std::find_if(
              factoryChoices.begin(), factoryChoices.end(),
              [](auto const& choice) { return !choice.disabled; });
          if (availableFactory != factoryChoices.end() &&
              ImGui::BeginCombo("Add Definition", "Select factory")) {
            for (auto const& choice : factoryChoices) {
              ImGui::BeginDisabled(choice.disabled);
              auto label = choice.factoryType.empty()
                               ? std::string("Default")
                               : choice.factoryType;
              label += " [" + choice.title + "]";
              if (ImGui::Selectable(label.c_str(), false)) {
                workspace.addDefinition(selected->resourceNamespace,
                                        selected->name, choice.factoryType);
              }
              ImGui::EndDisabled();
            }
            ImGui::EndCombo();
          }
          auto nestedItems = workspace.nestedFormItems(
              selected->resourceNamespace, selected->name);
          for (auto const& item : nestedItems) {
            ImGui::PushID(item.path.c_str());
            bool const open = ImGui::TreeNodeEx(
                item.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                        ImGuiTreeNodeFlags_SpanAvailWidth);
            if (open) {
              if (item.kind == resource_manager::NestedCollectionKind::animation) {
                auto const imageSetMode = item.alternative == "image-set";
                if (ImGui::BeginCombo("Frames", item.alternative.c_str())) {
                  if (ImGui::Selectable("explicit", !imageSetMode)) {
                    workspace.setFramesAlternative(
                        selected->resourceNamespace, selected->name,
                        item.path + "/Frames", false);
                  }
                  if (ImGui::Selectable("image-set", imageSetMode)) {
                    workspace.setFramesAlternative(
                        selected->resourceNamespace, selected->name,
                        item.path + "/Frames", true, "ImageSet");
                  }
                  ImGui::EndCombo();
                }
              } else if (!item.alternatives.empty()) {
                if (ImGui::BeginCombo("Alternative", item.alternative.c_str())) {
                  for (auto const& alternative : item.alternatives) {
                    if (ImGui::Selectable(alternative.c_str(),
                                          alternative == item.alternative)) {
                      workspace.setNestedAlternative(
                          selected->resourceNamespace, selected->name,
                          item.path, alternative);
                    }
                  }
                  ImGui::EndCombo();
                }
              }
              for (auto const& property : item.properties) {
                auto propertyPath = item.path + "/" + property.name;
                auto singletonPath = propertyPath;
                for (std::size_t pathPosition = singletonPath.find("/0/");
                     pathPosition != std::string::npos;
                     pathPosition = singletonPath.find("/0/", pathPosition)) {
                  singletonPath.erase(pathPosition, 2U);
                }
                bool const focusProperty =
                    !selectedNestedPath.empty() &&
                    (selectedNestedPath.ends_with(propertyPath) ||
                     selectedNestedPath.ends_with(singletonPath));
                auto key = selected->resourceNamespace + "\n" + selected->name +
                           "\n" + item.path + "\n" + property.name;
                auto [buffer, inserted] = nestedBuffers.try_emplace(key);
                if (inserted || !ImGui::IsAnyItemActive())
                  setBuffer(buffer->second, property.value);
                if (!property.enumValues.empty() ||
                    !property.selectorValues.empty() || property.boolean) {
                  auto const* preview = property.value.empty()
                                            ? "Not set"
                                            : property.value.c_str();
                  if (ImGui::BeginCombo(property.name.c_str(), preview)) {
                    if (property.optional &&
                        ImGui::Selectable("Not set", property.value.empty())) {
                      workspace.setNestedProperty(
                          selected->resourceNamespace, selected->name, item.path,
                          property.name, {});
                    }
                    auto choices = property.selectorValues.empty()
                                       ? property.enumValues
                                       : property.selectorValues;
                    if (property.boolean) choices = {"true", "false"};
                    for (auto const& value : choices) {
                      if (ImGui::Selectable(value.c_str(),
                                            value == property.value)) {
                        workspace.setNestedProperty(
                            selected->resourceNamespace, selected->name,
                            item.path, property.name, value);
                      }
                    }
                    ImGui::EndCombo();
                  }
                } else {
                  bool commit = ImGui::InputText(
                      property.name.c_str(), buffer->second.data(),
                      buffer->second.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                  commit |= ImGui::IsItemDeactivatedAfterEdit();
                  if (commit && std::string(buffer->second.data()) != property.value) {
                    std::optional<std::string> value =
                        buffer->second[0] == '\0' && property.optional
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{buffer->second.data()};
                    if (!workspace.setNestedProperty(
                            selected->resourceNamespace, selected->name,
                            item.path, property.name, value)) {
                      setBuffer(buffer->second, property.value);
                    }
                  }
                }
                if (focusProperty) {
                  ImGui::SetScrollHereY(0.5f);
                  selectedNestedPath.clear();
                }
              }
              bool const indexedItem =
                  !item.path.empty() &&
                  std::isdigit(static_cast<unsigned char>(item.path.back()));
              if (indexedItem) {
                if (ImGui::SmallButton("Duplicate")) {
                  workspace.duplicateNestedItem(selected->resourceNamespace,
                                                selected->name, item.path);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                  workspace.removeNestedItem(selected->resourceNamespace,
                                             selected->name, item.path);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Up")) {
                  auto separator = item.path.rfind('/');
                  auto index = static_cast<std::size_t>(std::stoull(
                      item.path.substr(separator + 1U)));
                  if (index > 0U)
                    workspace.reorderNestedItem(selected->resourceNamespace,
                                                selected->name, item.path,
                                                index - 1U);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Down")) {
                  auto separator = item.path.rfind('/');
                  auto index = static_cast<std::size_t>(std::stoull(
                      item.path.substr(separator + 1U)));
                  workspace.reorderNestedItem(selected->resourceNamespace,
                                              selected->name, item.path,
                                              index + 1U);
                }
              }
              if (item.kind == resource_manager::NestedCollectionKind::definition &&
                  item.label == "Default Definition") {
                auto base = item.path;
                if (selected->resourceType == "ImageSet") {
                  if (ImGui::SmallButton("Add image"))
                    workspace.addNestedItem(selected->resourceNamespace,
                                            selected->name,
                                            base + "/Images/Image",
                                            resource_manager::NestedCollectionKind::image);
                  ImGui::SameLine();
                  if (ImGui::SmallButton("Add image set"))
                    workspace.addNestedItem(
                        selected->resourceNamespace, selected->name,
                        base + "/Images/ImageSet",
                        resource_manager::NestedCollectionKind::imageSet);
                } else if (selected->resourceType == "AnimationSet") {
                  if (ImGui::SmallButton("Add animation")) {
                    workspace.addNestedItem(
                        selected->resourceNamespace, selected->name,
                        base + "/Animations/Animation",
                        resource_manager::NestedCollectionKind::animation);
                  }
                } else if (selected->resourceType == "Program") {
                  if (ImGui::SmallButton("Add buffer")) {
                    workspace.addNestedItem(
                        selected->resourceNamespace, selected->name,
                        base + "/MeshSpecification/Buffers/Buffer",
                        resource_manager::NestedCollectionKind::buffer);
                  }
                } else if (selected->resourceType == "Material" &&
                           ImGui::SmallButton("Add texture")) {
                  workspace.addNestedItem(
                      selected->resourceNamespace, selected->name,
                      base + "/Textures/Texture",
                      resource_manager::NestedCollectionKind::texture);
                }
              } else if (item.kind == resource_manager::NestedCollectionKind::animation) {
                if (ImGui::SmallButton(item.alternative == "image-set"
                                           ? "Add override"
                                           : "Add frame")) {
                  workspace.addNestedItem(
                      selected->resourceNamespace, selected->name,
                      item.path + "/Frames/Frame",
                      item.alternative == "image-set"
                          ? resource_manager::NestedCollectionKind::overrideFrame
                          : resource_manager::NestedCollectionKind::frame);
                }
              } else if (item.kind == resource_manager::NestedCollectionKind::buffer) {
                if (ImGui::SmallButton("Add channel")) {
                  workspace.addNestedItem(
                      selected->resourceNamespace, selected->name,
                      item.path + "/Channels/Channel",
                      resource_manager::NestedCollectionKind::channel);
                }
              } else if ((item.kind == resource_manager::NestedCollectionKind::frame ||
                          item.kind == resource_manager::NestedCollectionKind::overrideFrame) &&
                         item.path.find("/Frame/") != std::string::npos) {
                if (ImGui::SmallButton("Add tag")) {
                  workspace.addNestedItem(
                      selected->resourceNamespace, selected->name,
                      item.path + "/Tags/Tag",
                      resource_manager::NestedCollectionKind::tag);
                }
              }
              ImGui::TreePop();
            }
            ImGui::PopID();
          }
        } else if (selectedForm && !selectedForm->fileProperty.empty()) {
          ImGui::Text("Source file: %s", selected->location.c_str());
          bool const focusFile =
              !selectedNestedPath.empty() &&
              selectedNestedPath.ends_with(selected->instancePath + "/" +
                                             selectedForm->fileProperty);
          if (focusFile) ImGui::SetKeyboardFocusHere();
          if (ImGui::Button("Select source file...") && !dialog.busy()) {
            dialog.beginResourceFile(*selectedForm, window, false,
                                     selected->resourceNamespace,
                                     selected->name);
          }
          if (focusFile) {
            ImGui::SetScrollHereY(0.5f);
            selectedNestedPath.clear();
          }
          if (selectedForm) {
            for (auto const& option : selectedForm->options) {
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
        if (selected->resourceType == "Material") {
          if (materialDependencyIdBuffer[0] == '\0')
            setBuffer(materialDependencyIdBuffer, "TextureImage");
          ImGui::Separator();
          ImGui::TextUnformatted("Add image dependency");
          ImGui::InputText("Dependency ID", materialDependencyIdBuffer.data(),
                           materialDependencyIdBuffer.size());
          if (ImGui::BeginCombo("Image Resource", "Select Image")) {
            for (auto const& candidate : resources) {
              if (candidate.resourceType != "Image" ||
                  (candidate.resourceNamespace == selected->resourceNamespace &&
                   candidate.name == selected->name)) {
                continue;
              }
              auto label = candidate.resourceNamespace.empty()
                               ? candidate.name
                               : candidate.resourceNamespace + "/" + candidate.name;
              if (ImGui::Selectable(label.c_str())) {
                workspace.addResourceDependency(
                    selected->resourceNamespace, selected->name,
                    materialDependencyIdBuffer.data(),
                    {candidate.resourceNamespace, candidate.name});
              }
            }
            ImGui::EndCombo();
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
            auto referencePath =
                selected->instancePath +
                "/DependentResources/DependentResource/" +
                std::to_string(selector.dependencyIndex) + "/ref";
            bool const focusReference =
                !selectedNestedPath.empty() &&
                selectedNestedPath.ends_with(referencePath);
            if (focusReference) ImGui::SetKeyboardFocusHere();
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
            if (focusReference) {
              ImGui::SetScrollHereY(0.5f);
              selectedNestedPath.clear();
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
              selectedResourcePath.clear();
              selectedNamespacePath.clear();
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
    std::size_t diagnosticIndex = 0;
    for (auto const& diagnostic : workspace.structuralDiagnostics()) {
      auto label = diagnostic.instancePath.empty()
                       ? diagnostic.message
                       : diagnostic.instancePath + ": " + diagnostic.message;
      if (ImGui::Selectable(label.c_str())) {
        if (auto navigation = workspace.diagnosticNavigation(diagnosticIndex)) {
          selectedNamespace = navigation->resourceNamespace;
          selectedName = navigation->resourceName;
          selectedResourcePath = navigation->resourcePath;
          selectedNamespacePath.clear();
          selectedNamespaceNode = navigation->resourceName.empty();
          selectedInline = false;
          editingIdentity.clear();
          selectedNestedPath = navigation->instancePath;
        }
      }
      ++diagnosticIndex;
      hasDiagnostics = true;
    }
    std::size_t semanticIndex = 0;
    for (auto const& diagnostic : workspace.semanticDiagnostics()) {
      auto label = std::string(
                       diagnostic.severity ==
                               resource_manager::SemanticDiagnosticSeverity::error
                           ? "Error: "
                           : "Warning: ") +
                   (diagnostic.instancePath.empty()
                        ? diagnostic.message
                        : diagnostic.instancePath + ": " + diagnostic.message);
      if (ImGui::Selectable(label.c_str())) {
        if (auto navigation =
                workspace.semanticDiagnosticNavigation(semanticIndex)) {
          selectedNamespace = navigation->resourceNamespace;
          selectedName = navigation->resourceName;
          selectedResourcePath = navigation->resourcePath;
          selectedNamespacePath = navigation->resourceName.empty()
                                      ? navigation->resourcePath
                                      : std::string{};
          selectedNamespaceNode = navigation->resourceName.empty();
          selectedInline = navigation->inlineResource;
          if (navigation->inlineResource) {
            selectedInlineOwner = navigation->resourceName;
            selectedInlineIndex = navigation->dependencyIndex;
          }
          editingIdentity.clear();
          selectedNestedPath = navigation->instancePath;
        }
      }
      ++semanticIndex;
      hasDiagnostics = true;
    }
    if (!hasDiagnostics) ImGui::TextDisabled("No errors or warnings.");
    ImGui::EndChild();
  }
  ImGui::End();
  return result;
}

int verifySchemas(int argc, char const* const* argv) {
  fs::path iniPath;
  for (int index = 2; index < argc; ++index) {
    if (std::string_view(argv[index]) != "--ini" || !iniPath.empty() ||
        ++index == argc) {
      printUsage(std::cerr);
      return usageFailure;
    }
    iniPath = argv[index];
  }
  try {
    if (iniPath.empty())
      iniPath = executableDirectory() / "resource-manager.ini";
    auto sources = resource_manager::readEditorSchemaConfiguration(iniPath);
    auto catalog = resource_manager::loadEditorSchemaCatalog(sources);
    auto resourceTypes = static_cast<std::size_t>(
        std::count_if(catalog.entries().begin(), catalog.entries().end(),
                      [](auto const& entry) {
                        return entry.kind == ResourceSchemaKind::resourceType &&
                               entry.factoryType.empty();
                      }));
    std::cout << "Verified Resource Schema configuration: "
              << displayPath(sources.iniPath) << "; "
              << catalog.entries().size() << " catalog entries, "
              << resourceTypes << " Resource Types.\n";
    return success;
  } catch (std::exception const& error) {
    std::cerr << "Resource Manifest Editor schema configuration failure: "
              << error.what() << '\n';
    return configurationFailure;
  }
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

  ManifestWorkspace workspace(configuration.catalog, logPath.parent_path());
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
    bool smokeSchemasReloaded = false;
    auto nextRevisionCheck = std::chrono::steady_clock::now();
    while (running) {
      bool const closeRequested = !window.processEvents(&input);
      imGuiHandleInput(&input, &backend);
      input.update();
      workspace.updateRecovery();
      if (std::chrono::steady_clock::now() >= nextRevisionCheck) {
        (void)workspace.checkExternalChange();
        nextRevisionCheck = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(750);
      }
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
            case NativeDialog::Purpose::changeBaseDirectory:
              completed = workspace.changeBaseDirectory(*result->path);
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
              case NativeDialog::Purpose::changeBaseDirectory:
                logger.info("Changed Resource Manifest base directory: " +
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
      auto frame = drawWorkspace(workspace, dialog, window.getWindow(), logger,
                                 arguments.iniPath, running, closeRequested);
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
        if (smokeFrames == 0) {
          if (!workspace.reloadSchemas(arguments.iniPath)) {
            throw std::runtime_error(
                "GUI schema reload failed: " + workspace.operationDiagnostic());
          }
          smokeSchemasReloaded = true;
        }
        if (smokeFrames == 1) window.setSize(960, 700);
        if (window.getWidth() == 960 && window.getHeight() == 700) {
          smokeResized = true;
        }
        if (++smokeFrames >= 8) {
          if (!smokeResized || !smokeSchemasReloaded) {
            throw std::runtime_error(
                "Native resize or schema reload did not reach the Resource Manifest Editor workspace.");
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
    workspace.preferences().save();
    logger.info("Resource Manifest Editor shutdown.");
    if (arguments.smokeTest) {
      std::cout << "Resource Manifest Editor smoke test passed: startup, schema "
                   "reload, new, save, open, resize, menu, toolbar, and persistent workspace.\n";
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
    if (argc >= 2 && std::string_view(argv[1]) == "--verify-schemas") {
      return verifySchemas(argc, argv);
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
    if (argc == 2 && std::string_view(argv[1]) == "--composite-tests") {
      std::string failure;
      if (!resource_manager::runCompositeAuthoringTests(&failure)) {
        std::cerr << "Resource Manifest Editor composite tests failed: "
                  << failure << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor composite tests passed.\n";
      return success;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--advanced-tests") {
      std::string failure;
      if (!resource_manager::runAdvancedAuthoringTests(&failure)) {
        std::cerr << "Resource Manifest Editor advanced authoring tests failed: "
                  << failure << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor advanced authoring tests passed.\n";
      return success;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--schema-tests") {
      std::string failure;
      if (!resource_manager::runSchemaIntegrationTests(&failure)) {
        std::cerr << "Resource Manifest Editor schema integration tests failed: "
                  << failure << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor schema integration tests passed.\n";
      return success;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--semantic-tests") {
      std::string failure;
      if (!resource_manager::runSemanticRepairTests(&failure)) {
        std::cerr << "Resource Manifest Editor semantic repair tests failed: "
                  << failure << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor semantic repair tests passed.\n";
      return success;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--resilience-tests") {
      std::string failure;
      if (!resource_manager::runResilienceTests(&failure)) {
        std::cerr << "Resource Manifest Editor resilience tests failed: "
                  << failure << '\n';
        return internalFailure;
      }
      std::cout << "Resource Manifest Editor resilience tests passed.\n";
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
