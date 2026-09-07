# Resource Manifest schema validation

- **Status:** Accepted
- **Target module:** `Willpower.Application`
- **Schema vocabulary:** JSON Schema Draft 7
- **Runtime validator:** Valijson using its `yaml-cpp` adapter
- **Related issue:** [#24 — Resource editor/management UI](https://github.com/ajare/willpower/issues/24)

## 1. Summary

Willpower shall validate every YAML Resource Manifest while it is loaded. Validation occurs inside `ResourceLocation::scan()` and `ResourceLocation::rescan()`, after YAML syntax parsing but before conversion to `StructuredData`, creation of `ResourceRecord` objects, filesystem checks, or mutation of the location's cached records.

Validation has two layers:

1. A **Resource Manifest schema** validates the document envelope and common declarations.
2. A **Resource Type schema** validates each complete `Resource` declaration, including its options, dependencies, and default `Definition` payload where applicable.

The canonical schemas are JSON files. They are embedded in `Willpower.Application` at build time, so runtime validation does not depend on locating separately deployed schema files. The same source files remain available to editors, CI, and the future Resource Manifest Editor.

Existing semantic validation remains responsible for facts that JSON Schema cannot establish, such as file existence, dependency resolution, cycles, image bounds, and references into another resource's loaded data.

## 2. Goals

- Reject structurally invalid Resource Manifests as soon as they are loaded.
- Validate all built-in `wp::application::resourcesystem::Resource` subclasses.
- Report the manifest path, resource identity, instance path, and useful validation message.
- Preserve support for both singleton mappings and sequences in collection positions accepted by the current YAML reader.
- Preserve current scalar compatibility: numeric and boolean fields may be native YAML scalars or quoted strings.
- Keep `scan()` and `rescan()` atomic: a failed load must not publish partial records or discard the last valid scan.
- Make canonical schemas reusable by IDEs and Resource Manifest tooling.
- Permit custom Resource Types and specialized definition factories that do not yet publish a schema; unknown types remain subject to the common manifest schema.

## 3. Non-goals

- Replacing dependency, cycle, duplicate-name, or filesystem validation.
- Validating the contents of source assets such as images, XML files, shaders, or FMOD banks.
- Changing resource creation or factory-selection order.
- Requiring unquoted numeric or boolean YAML scalars.
- Standardizing schemas for Resource Types supplied by other repositories or plugins.
- Implementing the Resource Manifest Editor tracked by issue #24.

## 4. Terminology

The terms **Resource Manifest**, **Resource**, **Resource Type**, and **Resource Type Plugin** have the meanings in [`CONTEXT.md`](../../CONTEXT.md). In code, `Resource` means `wp::application::resourcesystem::Resource`.

A **default Definition** is a `Definitions/Definition` entry with no `factory` property. A **specialized Definition** has a non-empty `factory` property.

## 5. Current behavior and constraints

`ResourceLocation::scan()` currently:

1. reads the manifest;
2. parses it with `utils::YamlReader`/`yaml-cpp`;
3. converts it to `StructuredData`;
4. scans namespaces and resources into `ResourceRecord` objects.

The conversion must not precede schema validation because it:

- converts every scalar to `std::string`;
- converts null to an empty string;
- flattens YAML sequences into repeated named entries;
- loses native scalar and array distinctions; and
- does not retain YAML source marks.

The validator therefore operates on the original `YAML::Node` tree. Parsing and validation must share that tree rather than parsing the document twice.

The existing YAML shape allows a collection to be represented by either one mapping or a sequence. For example, both forms remain valid:

```yaml
Resource:
  type: Image
  location: image.png
```

```yaml
Resource:
  - type: Image
    location: image.png
  - type: Shader
    location: shader.vert
```

Canonical schemas shall model this with reusable singleton-or-array definitions. A future format version may require arrays consistently, but this work does not introduce that breaking change.

## 6. Architecture

### 6.1 Components

Introduce an internal `ResourceManifestValidator` in `Willpower.Application`. It owns the embedded schema catalog and asks `utils::YamlReader` to validate its parser-owned document before conversion:

```cpp
void validate(utils::YamlReader const& reader,
              std::string const& manifestPath) const;
```

Valijson and `yaml-cpp` remain private implementation dependencies of Utils. `YamlReader` gains a generic JSON Schema validation operation that accepts an in-memory root schema and local `$ref` catalog and returns library-neutral failures. This avoids exporting `YAML::Node` across the Utils DLL boundary or linking a second copy of its private static `yaml-cpp` into `Willpower.Application`.

The composed root schema is responsible for:

1. validating the complete document against `resource-manifest.schema.json`;
2. matching every top-level and namespaced Resource declaration;
3. dispatching known built-in types through `oneOf`/`if` conditions on `type`;
4. excluding built-in type names from the permissive custom-Resource fallback;
5. validating default Definition payloads as part of each Resource Type schema; and
6. returning all discovered failures in deterministic document order.

Schema matching must understand singleton and sequence forms but must not normalize or mutate the YAML tree.

### 6.2 Schema catalog

Canonical schemas live under:

```text
willpower.application/schemas/
  resource-manifest.schema.json
  common.schema.json
  resource.schema.json
  text-file.schema.json
  xml-file.schema.json
  image.schema.json
  shader.schema.json
  program.schema.json
  image-set.schema.json
  animation-set.schema.json
  material.schema.json
  audio-bank.schema.json
```

`common.schema.json` contains shared scalar and collection definitions. `resource.schema.json` contains the common Resource declaration shape. Every schema has a stable `$id`, uses Draft 7, and resolves `$ref` locally without network access.

CMake generates byte-array or string-view sources from these files and links them into `Willpower.Application`. The JSON files are the only hand-edited source of truth; embedded output is generated in the build tree and is not committed.

### 6.3 Extensibility

The initial catalog contains all built-in types. Unknown Resource Types pass common Resource validation, preserving existing plugin behavior.

The schema lookup API shall be designed so a later change can register a schema for `(resource type, factory type)`. This specification does not require a public plugin registration ABI.

A built-in Resource with a specialized `Definition` is validated as follows:

- its common declaration and built-in options/dependencies are validated;
- each default Definition is validated against the built-in default Definition schema;
- a specialized Definition with a non-empty `factory` receives only the generic Definition-object validation unless a matching schema is registered in the future.

This preserves the existing specialized-before-default fallback behavior.

### 6.4 Loader integration and atomicity

`utils::YamlReader` shall provide a supported way to validate its parser-owned `YAML::Node` before `readTree()` converts it. Ownership must remain RAII-safe, and neither `YAML::Node` nor Valijson types may become part of the exported Utils or Willpower public API.

`ResourceLocation::scan()` and `rescan()` shall use this order:

1. verify the definition-file extension;
2. read the complete document;
3. parse YAML once;
4. validate the raw YAML tree;
5. convert the validated tree to `StructuredData`;
6. construct namespaces and records in temporary storage;
7. run existing record-level validation where currently applicable;
8. atomically replace cached records only after success.

On failure:

- throw `ResourceSystemException` (or a dedicated subclass);
- do not expose records from the invalid document;
- do not instantiate or mutate any `Resource`;
- on `rescan()`, preserve records from the previous successful scan; and
- leave the location dirty so a corrected file can be retried.

`validateResourceDefinitions()` remains available during migration, but schema-covered structural checks should not be duplicated indefinitely. Semantic checks stay there or in `ResourceManager`.

## 7. Common schema rules

### 7.1 Document envelope

- The document is a mapping with exactly one top-level `Resources` key.
- `Resources` may contain only `Resource` and `Namespace`.
- `Namespace` is a mapping or non-empty sequence of mappings.
- A namespace requires a non-empty `name` and one or more Resources.
- Unknown keys are rejected at every schema-covered level.

### 7.2 Resource declaration

- `type` is a non-empty string and is required for every Resource, including composite resources.
- A Resource has either a non-empty `name`, a non-empty `location` from which the current loader can infer the name, or both.
- Explicit names must not contain `/`.
- `location`, when present, is a non-empty string.
- `Option`, `DependentResources`, and `Definitions` use their existing wrapper names and singleton-or-sequence behavior.
- Option names are non-empty; option values are scalar.
- Dependency `id`, when present, is non-empty.
- A dependent resource contains exactly one of `ref` or an inline declaration.
- A Definition is a mapping. `factory`, when present, is a non-empty string.
- Duplicate names, options, dependency IDs, defaults, and resources remain semantic checks because Draft 7 cannot express uniqueness by one object property reliably across the compatible YAML shapes.

### 7.3 Compatible scalar definitions

Shared schemas accept the forms currently consumed through string conversion:

- unsigned integer: YAML integer `>= 0` or a decimal digit string;
- integer: YAML integer or a signed decimal string;
- number: YAML number or a finite decimal/scientific-notation string;
- boolean: YAML boolean or case-insensitive string `true`, `false`, `yes`, `no`, `1`, or `0`.

Schemas must reject null, empty strings, partially numeric strings, and values outside explicitly stated ranges. This is intentionally stricter than `StringUtils::parseInt`, `parseUInt`, `parseFloat`, and `parseBool`, which can silently coerce malformed input.

## 8. Built-in Resource Type schemas

The schemas describe behavior visible in the current factories and Resource subclasses. Every built-in schema fixes `type` to its Resource Type name and rejects unknown fields within schema-owned structures.

### 8.1 `TextFile`

- Requires `location`; `name` remains optional because it can be inferred.
- Allows no type-specific options or dependencies.
- The default Definition is optional and, if present, must have no payload fields.

### 8.2 `XmlFile`

- Same declaration constraints as `TextFile`.
- The source file's XML syntax is validated later when the Resource is created, not while the manifest is loaded.

### 8.3 `Shader`

- Same declaration constraints as `TextFile`.
- Shader language/source validation remains out of scope.

### 8.4 `AudioBank`

- Requires `location`.
- Allows no type-specific default Definition payload.
- FMOD bank validation remains creation-time behavior and remains conditional on FMOD support.

### 8.5 `Image`

- Requires `location`.
- Allows these Option names and values:
  - `filtering`: `none` or `linear`;
  - `uv-style`: `atlas`;
  - `colour-space`: `linear`;
  - `wrapping`: `repeat`;
  - `mipmaps`: compatible boolean.
- Rejects duplicate or unknown Image options through schema plus semantic duplicate checking.
- Allows no default Definition payload.
- Image decoding, channel count, and dimensions remain creation-time validation.

### 8.6 `ImageSet`

- Requires an explicit `name` and no source `location`.
- Requires a dependency whose `id` is `Image`; dependency existence and actual type remain semantic checks.
- Requires one default Definition with an `Images` mapping.
- `Images` may contain `Image` and/or `ImageSet` entries.
- Every Image entry requires non-empty `name`, `x`, `y`, `width`, and `height`; coordinates and dimensions are unsigned integers, with width and height greater than zero.
- Every ImageSet entry additionally requires `count`, `dx`, and `dy`; count is greater than zero and offsets are integers.
- Duplicate image names and bounds against the loaded Image remain semantic checks.

### 8.7 `AnimationSet`

- Requires an explicit `name` and no source `location`.
- Requires a dependency whose `id` is `Image`.
- Requires one default Definition with `Animations/Animation`.
- Each Animation requires a non-empty `name`, optional `loopStyle` in `forwards`, `once`, or `pingpong`, and a `Frames` mapping.
- Frames supports the two existing modes:
  - an `imageset` reference with optional positive `count` and Frame overrides that require `frame` indexes; or
  - one or more explicit Frames requiring an `image` reference.
- Frame `time` values are numbers greater than zero when provided; `xoff` and `yoff` are integers.
- Tags require non-empty `key` and scalar `value`.
- Referenced image/image-set existence, index bounds, duplicate animation names, and the final requirement that every materialized frame has positive time remain semantic checks.

### 8.8 `Program`

- Requires an explicit `name` and no source `location`.
- Requires dependencies with IDs `Vertex` and `Fragment`.
- Requires one default Definition containing `Attribs` and `MeshSpecification`.
- `Attribs` permits:
  - `textures`: unsigned integer;
  - `diffuse`, `colours`, `atlas`, and `rotation`: compatible booleans.
- `MeshSpecification` requires:
  - `primitive`: case-insensitive `POINTS`, `LINES`, or `TRIANGLES`;
  - `indexed`: compatible boolean;
  - `storage`: case-insensitive `STATIC` or `DYNAMIC`;
  - one or more `Buffers/Buffer` entries.
- Each Buffer requires one or more `Channels/Channel` entries.
- Each Channel requires:
  - `data`: one of `POSITION2`, `POSITION3`, `POSITION4`, `NORMAL3`, `NORMAL4`, `TEXCOORD2`, `TEXCOORD3`, `TEXCOORD4`, `COLOUR1`, `COLOUR3`, `COLOUR4`, `USER1`, `USER2`, `USER3`, or `USER4`;
  - `type`: one of `FLOAT16`, `FLOAT32`, `INT8`, `INT16`, `INT32`, `UINT8`, `UINT16`, or `UINT32`;
  - optional `normalised`: compatible boolean.
- Enum matching remains case-insensitive to match current parsing.

### 8.9 `Material`

- Requires an explicit `name` and no source `location`.
- Requires a dependency whose `id` is `Program`.
- Requires one default Definition containing `Textures`.
- `Textures/Texture` entries require a non-empty `sampler` and `type`:
  - `type: resource` requires a non-empty scalar `value` naming a dependency ID;
  - `type: default` forbids a non-empty resource value.
- Whether a resource texture's value matches a dependency ID, and whether that dependency is an Image, remain semantic checks.

## 9. Error reporting

A validation exception shall include all failures found in one pass, capped at a documented defensive maximum. Each failure contains:

- Resource Manifest path;
- namespace and Resource name when dispatch reached a Resource declaration;
- YAML/JSON instance path, such as `/Resources/Resource/1/Definitions/Definition/Images/ImageSet/0/count`;
- schema title or Resource Type;
- validation message; and
- YAML line and column when a corresponding `YAML::Mark` is available.

Messages must not expose Valijson implementation types in the public API. Ordering is deterministic, and tests assert stable path/message fragments rather than complete library-generated prose.

Example:

```text
Invalid Resource Manifest 'res/Resources.yaml':
  World/PlayerAnimation at /Resources/Namespace/0/Resource/1/Definitions/Definition/Animations/Animation/0/loopStyle (line 42, column 20): expected one of [forwards, once, pingpong], got 'reverse'
```

## 10. Testing

Tests shall cover:

- malformed YAML versus schema-invalid YAML;
- invalid top-level and namespace structures;
- singleton and sequence collection forms;
- each built-in Resource Type with at least one valid fixture;
- missing required fields, unknown fields, invalid enums, invalid scalar strings, and nulls for every complex schema;
- default versus specialized Definitions;
- unknown custom Resource Types remaining loadable under the common schema;
- validation occurring in `scan()` without calling `validateResourceDefinitions()`;
- no records published after an initial invalid scan;
- a failed rescan retaining records from the prior successful scan;
- corrected content succeeding on a later rescan;
- schema `$ref` resolution without filesystem or network access; and
- Windows/MSVC and Linux/GCC/Clang builds.

The existing `resource_yaml_tests`, resource definition tests, and fixtures must remain green or be updated only where malformed input was previously accepted accidentally.

## 11. Documentation and tooling

README documentation shall describe load-time validation and how to interpret failures. The repository shall include a YAML language-server association for `Resources.yaml` where practical, pointing to the canonical manifest schema. Type-specific completion may initially be limited by the two-stage runtime dispatch; schemas must nevertheless be directly consumable by the future Resource Manifest Editor.

## 12. Acceptance criteria

The feature is complete when:

1. every YAML Resource Manifest is schema-validated during `scan()`/`rescan()` before `StructuredData` conversion;
2. all nine built-in Resource subclasses have canonical schemas and passing positive/negative tests;
3. invalid input cannot publish partial `ResourceRecord` state or replace a previous valid scan;
4. validation diagnostics identify the file, Resource where applicable, and instance path;
5. runtime schema resolution performs no file or network I/O;
6. custom unknown Resource Types retain current compatibility;
7. semantic resource validation still runs after structural validation; and
8. the full CMake build and CTest suite pass on supported Windows and Linux toolchains.
