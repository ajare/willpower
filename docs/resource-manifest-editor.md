# Resource Manifest Editor desktop shell

`resource-manager` is the Resource Manifest Editor executable. Launching it without a
headless command opens one resizable SDL3/ImGui workspace with a menu bar, toolbar,
document tree, inspector, and persistent diagnostics region.

## Deployment and startup

The executable requires `resource-manager.ini` beside it by default. The normal build
stages the INI and runtime libraries beside the executable, and `cmake --install`
installs the executable, INI, and runtime dependencies. An alternate INI can be selected
with `--ini FILE`.

The deployment file must contain:

```ini
[ResourceManifestEditor]
formatVersion=1
```

An optional `[mpp]` section uses the rendering settings accepted by MassivePolyPusher.
The installed INI supplies conservative defaults.

Before SDL video or the GUI is initialized, the editor creates and rotates
`resource-manager.log` in the platform preference directory. The previous log is kept
as `resource-manager.log.1`. If logging cannot be established, startup reports the
failure to standard error and returns `7`. A missing or invalid deployment INI is
reported to the log and standard error and returns `3`. GUI/platform initialization
failure returns `8`. `WILLPOWER_RESOURCE_MANAGER_PREFERENCE_DIR` overrides the
preference directory for unattended deployment tests.

`--startup-check` validates logging and the INI without initializing SDL video:

```sh
resource-manager --startup-check --ini /installed/bin/resource-manager.ini
```

## Document workflow

- **New** opens the native folder selector. Selecting a base directory creates a valid,
  empty, unsaved Resource Manifest.
- **Open** opens the native YAML selector. Only `.yaml` and `.yml` files (case
  insensitive) containing exactly one structurally valid Resource Manifest document
  replace the current document. A rejected file leaves the current document open and
  records the failure in Diagnostics and the log.
- **Save** writes the current path, or invokes Save As for an unsaved document.
- **Save As** uses the native YAML selector and rejects destinations without a `.yaml`
  or `.yml` extension.

Saves use `ResourceManifestDocument` canonical UTF-8 YAML and replace the destination
with a completed sibling temporary file. Failed operations are retained in the
persistent Diagnostics region and written to standard error and the rotating log; the
editor does not display error dialogs.

A manifest path can be supplied at launch. Its parent is the initial base directory.

## Validation seams

The headless validation command remains documented in
[`resource-manifest-tools.md`](resource-manifest-tools.md). Additional shell checks are:

```sh
resource-manager --document-tests
resource-manager --smoke-test --ini /installed/bin/resource-manager.ini
```

`--document-tests` covers empty New, valid and rejected Open, canonical atomic Save, and
YAML-extension enforcement without initializing SDL. `--smoke-test` executes New,
Save, and Open in a temporary directory, initializes the real SDL3/ImGui/OpenGL stack,
renders the menu, toolbar, and editor for several frames, resizes the native window,
and verifies that the non-closable editor workspace continues to fill the viewport.
CTest registers startup, document workflow, and GUI smoke coverage. The GUI smoke test
is skipped with status `77` only when Linux has no display available.
