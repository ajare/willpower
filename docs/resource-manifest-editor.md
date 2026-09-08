# Resource Manifest Editor desktop shell

`resource-manager` is the Resource Manifest Editor executable. Launching it without a
headless command opens one resizable SDL3/ImGui workspace with a menu bar, toolbar,
document tree, inspector, and persistent diagnostics region.

## Deployment and startup

The executable requires `resource-manager.ini` beside it by default. The normal build
stages the INI and runtime libraries beside the executable, and `cmake --install`
installs the executable, INI, and runtime dependencies. An alternate INI can be selected
with `--ini FILE`.

The deployment file must contain `formatVersion=1`. It may select one complete
application Resource Schema Bundle and repeat `bundle` for ordered extensions:

```ini
[ResourceManifestEditor]
formatVersion=1
baseBundle=../share/example/resource-schema-bundle
bundle=../share/vendor-a/resource-schema-extension
bundle=../share/vendor-b/resource-schema-extension
```

Paths are resolved relative to the INI location, so installed configurations remain
relocatable. If `baseBundle` is absent, and only then, the embedded Willpower catalog is
the base. Each `bundle` is merged in declaration order. A duplicate Resource
Type/factory lookup key is an error even when its schema bytes are identical. The editor
loads bundle directories only: it never loads Resource Type Plugins, application
libraries, factories, or network references.

The complete candidate must contain exactly one manifest schema and pass bundle
metadata, version, path, hash, schema-ID, collision, closed-reference, and
[editor-annotation](specifications/resource-schema-editor-annotations.md) checks. The
built-in `resource-manager.ini` omits `baseBundle` and therefore uses embedded built-ins.
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

## File-backed Resource authoring

The Resource menu creates `TextFile`, `XmlFile`, `Shader`, `AudioBank`, and `Image`
Resources from the loaded built-in catalog. A new Resource remains an inspector draft
until it has an explicit namespace-unique name and a selected source file. Resource Type
is displayed read-only after creation. Image options are generated from its schema.

The built-in schemas use the versioned
[Resource Schema editor annotations](specifications/resource-schema-editor-annotations.md)
to request native file controls and provide their filters. Source locations have no
free-text editor. Both the base and selection are canonicalised; missing files,
traversal outside the base, and symbolic-link or junction escapes are rejected. Accepted
locations are committed with `/` separators relative to the base directory.

Create, rename, source-file, Image-option, and delete changes are commands available
through Edit/Undo and Edit/Redo (also Ctrl+Z/Ctrl+Y and toolbar buttons). Continuous
changes coalesce, while incomplete text remains local to the inspector until commit.
Deleting the final Resource in a named namespace removes that otherwise-invalid
namespace in the same command.

## Flat namespace organization

The tree always shows the permanent default namespace followed by named namespaces in
document order. Named namespaces are flat, unique, non-empty, and cannot contain the
`/` qualifier separator. Creating one starts a tree draft; no YAML or history entry is
created until the user authors or moves its first Resource into it.

Namespace and Resource names can be edited in the inspector. Resources can be dragged
onto another Resource to reorder them or onto a namespace to move them. These operations
reject ambiguous duplicate identities and destination collisions. Moving an inferred-name
Resource materializes an explicit name when that name is valid.

Rename and move commands rewrite every standard
`DependentResources/DependentResource/ref` that is affected. A target in the reference
owner's namespace is unqualified; a target in another named namespace uses
`Namespace/Resource`, and a target in the default namespace from a named namespace uses
`/Resource`. The declaration change and all rewrites are one validated undoable command.

Resource deletion is blocked by known incoming standard references. Deleting the final
Resource confirms and removes its named namespace in the same command. Populated
namespace deletion has a separate confirmation, is undoable, and is blocked when a
standard reference arrives from outside that namespace. The default namespace cannot be
renamed or deleted.

## Dependencies and inline Resources

Standard `DependentResources/DependentResource/ref` properties are edited with Resource
selectors rather than text fields. Choices show namespace-qualified identities and are
filtered to the built-in dependency's allowed Resource Type. The owner is omitted and a
choice is disabled when selecting it would close a dependency cycle. Existing missing,
type-incompatible, self, or inline targets remain visible as disabled selections so they
can be understood and replaced. A dependency can be cleared only when removing its entry
leaves the Resource structurally valid.

Dependency diagnostics identify missing, ambiguous, incompatible, self, and cyclic
references. They also list known incoming references that block deletion. Selectors never
offer an inline Resource as a new target.

An inline dependent Resource is displayed below its syntactic owner with an inline marker.
Its name and supported file-backed properties can be edited, but it has no independent
move or delete action. Promotion copies the declaration to the selected namespace level
and replaces the owned declaration with a correctly qualified `ref` in one undoable
command. Existing references to the inline identity continue to be represented before
promotion and become ordinary selector targets afterwards.

## ImageSet and AnimationSet Definitions

`ImageSet` and `AnimationSet` are available from the same catalogued Add Resource menu.
Their drafts require a selector-backed compatible Image dependency (`Image` for an
ImageSet and `ImageSet` for an AnimationSet) and create a minimal valid default
Definition. Their inspectors expose required and optional nested properties, integer and
positive-number constraints, loop-style enums, and explicit-frame versus image-set-frame
alternatives.

Image, image-set, animation, frame, frame-override, and tag collections accept legacy
singleton input and are presented as indexed collections. Items can be added, duplicated,
removed, and reordered; output is normalized back to a singleton when one item remains.
Every action validates a preview before committing one undoable command. Removing a
required default Definition or final required collection item, supplying an invalid
number, or entering an incomplete alternative is rejected without changing the current
document. Rejected-edit diagnostics retain their complete instance paths and can be
selected to return to the owning Resource's nested inspector.

## Program, Material, and Definition factories

`Program` drafts require separate selector-backed Vertex and Fragment Shader
dependencies and create a minimal valid default Definition. Their inspector covers
attribute counts and flags, primitive/index/storage controls, and nested Buffer and
Channel collections with constrained component and data-type selectors.

`Material` drafts require a Program dependency. Image dependencies are added with a
unique dependency ID and an Image Resource selector. Texture collections provide atomic
`default` and `resource` alternatives; resource texture values select from the
Material's declared Image dependency IDs rather than accepting arbitrary text.

The Add Definition selector is populated from exact `(Resource Type, factory type)`
entries in the active Resource Schema Catalog. A factory already present on the Resource
is disabled, an unregistered or cross-type factory is rejected, and the selected
specialized schema alone determines its fields and constraints. Removing or duplicating
a Definition is previewed against uniqueness and whole-manifest validity, so a required
default Definition cannot be removed while a removable specialized Definition can.
Unsupported schema authoring shapes remain read-only rather than being guessed.

## Application schemas, unknown declarations, and reload

A catalogued application Resource Type using the supported generic form subset can be
created and edited entirely from bundle data. The subset is a common Resource
declaration with an explicit name, at most one annotated file property, annotated
standard dependencies, and an optional default Definition that is a closed flat object
of string, enum, boolean, integer, or number properties and their basic bounds/defaults.
Catalogued specialized Definition schemas continue to use the scalar Definition form
subset. Valid schemas outside this authoring subset remain visible and are preserved,
but their type-specific payload is read-only with an explicit unsupported-form warning.

A Resource Type with no default editing schema remains openable under common Resource
validation. Rename, move, reorder, reference-safe delete, and canonical round-trip
preserve its payload. Unknown Definition payloads are read-only, and the inspector warns
that unannotated custom data cannot be dependency-checked or rewritten.

**Schemas > Reload configuration and bundles** rereads the deployment INI and all bundle
bytes into an isolated candidate. Publication occurs only after annotation/form loading
and validation of the open Resource Manifest. Any failure retains the previous catalog,
forms, document, and history and reports the reason in Diagnostics and the log.

## Validation seams

The headless validation command remains documented in
[`resource-manifest-tools.md`](resource-manifest-tools.md). Additional shell checks are:

```sh
resource-manager --document-tests
resource-manager --authoring-tests
resource-manager --organization-tests
resource-manager --dependency-tests
resource-manager --composite-tests
resource-manager --advanced-tests
resource-manager --schema-tests
resource-manager --verify-schemas --ini /installed/bin/resource-manager.ini
resource-manager --smoke-test --ini /installed/bin/resource-manager.ini
```

`--document-tests` covers empty New, valid and rejected Open, canonical atomic Save, and
YAML-extension enforcement without initializing SDL. `--authoring-tests` covers all five
file-backed built-ins, drafts, native-selector metadata, path containment, canonical
relative output, and create/property/rename/delete undo and redo.
`--organization-tests` covers namespace drafts, duplicate identities, default-namespace
protection, ordering, moves, standard-reference rewrites, deletion rules, and compound
undo/redo. `--dependency-tests` covers allowed-type filtering, qualified identities,
cycle prevention, missing and incoming references, inline editing, selector visibility,
and atomic promotion. `--composite-tests` covers ImageSet and AnimationSet creation,
explicit and image-set frames, compatible scalar input, nested collection boundaries,
dependency selectors, rejected edits and transitions, diagnostics, canonical output,
and undo/redo. `--advanced-tests` covers valid and invalid Program and Material forms,
shader/Program/Image selectors, buffers, channels, texture alternatives, default and
specialized Definition factories, factory uniqueness, deletion validity, canonical
output, and history. `--schema-tests` covers complete and extension bundles, relative
ordered configuration, embedded fallback, duplicate keys, malformed annotations,
application-owned and unsupported forms, unknown payload preservation, and failed and
successful atomic reload. `--verify-schemas` checks deployment configuration and all
bundle/catalog/annotation contracts without initializing SDL video and returns `3` for
a configuration or bundle failure. `--smoke-test` executes schema reload, New, Save,
and Open in a temporary directory, initializes the real SDL3/ImGui/OpenGL stack,
renders the menu, toolbar, and editor for several frames, resizes the native window,
and verifies that the non-closable editor workspace continues to fill the viewport.
CTest registers all headless workflow suites, startup, and GUI smoke coverage. The GUI smoke test
is skipped with status `77` only when Linux has no display available.
