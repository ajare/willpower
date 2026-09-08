# Application Resource Schema integration

Applications should publish one **Resource Schema Bundle** containing Willpower's
built-in Resource Types and every application-owned Resource Type. The bundle, not a
C++ factory or the Willpower source directory, is the interchange format for editors,
CI, and the Resource Manifest Editor.

## Author a bundle

Write JSON Schema Draft 7 documents with stable absolute `$id` values. Register one
schema without `FACTORY_TYPE` for each Resource Type; it validates the complete
`Resource` declaration and its default Definitions. Register a separate schema for
each specialized Definition factory. A specialized schema validates the
`Definitions/Definition` object itself.

```cmake
list(APPEND CMAKE_MODULE_PATH "${WILLPOWER_PREFIX}/share/willpower/cmake")
include(ResourceSchemas)

willpower_declare_resource_schema(
    RESOURCE_TYPE Widget
    SCHEMA_ID "https://schemas.example.com/widget.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/widget.schema.json")
willpower_declare_resource_schema(
    RESOURCE_TYPE Widget FACTORY_TYPE GlowFactory
    SCHEMA_ID "https://schemas.example.com/widget-glow.schema.json"
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schemas/widget-glow.schema.json")
willpower_compose_resource_schema_bundle(
    TARGET example_resource_schemas
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resource-schema-bundle"
    ROOT_SCHEMA_ID "https://schemas.example.com/resource-manifest.schema.json"
    INSTALL_DESTINATION "share/example/resource-schema-bundle")
```

This works against `cmake --install` output; no Willpower source-tree path is needed.
The module discovers the installed built-in bundle next to itself. `BUILTIN_BUNDLE`
may select another package location explicitly. Composition fails on duplicate lookup
keys, conflicting schema IDs, malformed documents, or missing local `$ref` targets.
The output is byte-for-byte deterministic for identical inputs and resolves entirely
offline.

## Optional dynamic schema discovery

When a Resource Type is distributed independently and cannot be selected while composing
the application bundle, a trusted native Resource Type Plugin may provide the same
bundle content through the versioned C ABI:

```sh
willpower-resource-schemas export \
  --plugin /opt/example/lib/widget-resource-schemas.so \
  --output build/editor-schemas
```

C++ hosts can call `ResourceSchemaCatalog::addPlugin(path)` directly. The path is always
explicit; Willpower has no plugin directories, scanning, manifest-triggered loading, or
environment-based discovery. `addPlugin` copies the returned bytes, releases plugin-owned
memory, unloads the library, and only then validates and atomically merges through the
normal catalog path. Plugin registration remains entirely separate from runtime factory
registration and application subsystem initialization.

A plugin is architecture-specific executable code, unlike a bundle directory. It must
match the host OS, CPU, and C ABI and be deployed with its native dependencies. Module
initializers and the entry point run with full process privileges, so only trusted,
authenticated plugin paths should be accepted. A plugin can have effects before schema
validation and is not sandboxed by hash, JSON, or `$ref` checks. Prefer Resource Schema
Bundle directories when dynamic discovery is unnecessary. See the normative
[Resource Type schema plugin C ABI](specifications/resource-schema-plugin.md) for symbol,
negotiation, byte-container, lifetime, error, dependency, and unload rules.

## Register runtime behavior

Schema registration and factory registration are related but explicit operations. Do
all of them before the first `scanLocations()` or `rescanLocations()`:

```cpp
manager.addResourceSchemaBundle("share/example/resource-schema-bundle");
manager.addResourceFactory(new WidgetFactory);
manager.addResourceDefinitionFactory(new WidgetDefaultDefinitionFactory);
manager.addResourceDefinitionFactory(new WidgetGlowDefinitionFactory);
```

`ResourceFactory` constructs the application `Resource` subclass. The default
`ResourceDefinitionFactory` uses an empty factory type; specialized factories use the
same case-sensitive name as the schema's `FACTORY_TYPE`. The manager freezes one schema
snapshot when scanning begins, so late schema registration is rejected.

## Export and consume

For schema-selected-at-build-time applications, use the CMake bundle directly. For a
catalog assembled by application registration code, create a small executable around
`runResourceSchemaExporter`; see [Resource Schema Bundle tooling](resource-schema-tools.md).
Its `export` command writes a self-contained catalog plus a convenient root-schema copy:

```sh
my-schema-exporter export --output build/editor-schemas \
  --root-schema-id https://schemas.example.com/editor-resource-manifest.schema.json
```

An external process starts at `catalog.json`, verifies versions and document hashes,
loads every catalogued document into an ID-based resolver, and validates with the sole
`manifest` entry. An `https` `$id` is an identifier, not permission to fetch it. Never
follow network references or read files outside the bundle. Treat catalogs and schemas
as untrusted input: enforce path containment, size/resource limits, hashes, supported
versions, and bounded diagnostics. A bundle is data and must never cause application
libraries or Resource Type plugins to be loaded.

For VS Code with Red Hat YAML, associate the **exported application root**, rather than
the built-in source schema:

```json
{
  "yaml.schemas": {
    "./build/editor-schemas/resource-manifest.schema.json": [
      "**/Resources.yaml", "**/Resources.yml"
    ]
  }
}
```

The Resource Manifest Editor should likewise open the complete application bundle,
resolve only its catalogued documents, and use its manifest entry. This provides custom
Resource Type and factory completion without executing application code.

## Validation boundary and compatibility

Schema validation is **structural**. It checks manifest shape, required and unknown
fields, scalar syntax/ranges, Resource Type declarations, and registered specialized
Definition payloads. `ResourceLocation::scan()` performs this before publishing any
records; failed rescans retain the previous successful records.

Schema validation does **not** prove that source files exist or are safe, dependencies
resolve without cycles, names are unique across locations, referenced Resources exist,
or asset contents can be decoded. Those semantic/resource-loading checks run later in
`ResourceManager`, definition factories, and Resource implementations. Editor success
therefore does not guarantee that an application can load a manifest.

Bundle format and Resource Manifest schema versions are independent `MAJOR.MINOR`
values. Consumers reject unsupported majors. A minor may add only documented
backward-compatible capabilities; producers should pin and test the exact Willpower
minor they distribute. See the normative [bundle format and compatibility
policy](specifications/resource-schema-bundle.md).

The complete downstream example, including a custom `Resource`, Resource factory,
default and specialized Definition factories, schemas, an external consumer, and
runtime parity fixtures, is in
[`tests/downstream/resource-schema-bundle`](../tests/downstream/resource-schema-bundle).
