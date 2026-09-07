# Resource Schema Bundle format

- **Status:** Accepted
- **Format version:** 1.0
- **Resource Manifest schema version:** 1.0
- **Related issue:** [#31](https://github.com/ajare/willpower/issues/31)

## 1. Purpose and scope

A Resource Schema Bundle is a directory that lets a language-neutral consumer discover and validate Resource Manifest declarations without loading Willpower or reflecting C++ `Resource` classes. It contains one catalog and all JSON Schema documents needed by that catalog. A bundle is self-contained: filesystem lookup outside its directory and network retrieval are forbidden during schema resolution.

This version uses JSON Schema Draft 7. The built-in Willpower bundle is generated from the canonical files in `willpower.application/schemas`; generated files are not a second editable source of truth.

## 2. Directory layout

A bundle has this layout:

```text
resource-schema-bundle/
├── catalog.json
└── schemas/
    ├── resource-manifest.schema.json
    ├── common.schema.json
    ├── resource.schema.json
    └── ... Resource Type schemas
```

`catalog.json` is UTF-8 JSON. Paths in the catalog are `/`-separated, relative to the bundle root, must not be absolute, and must not contain a `..` segment. Schema documents are copied byte-for-byte from their canonical inputs.

## 3. Catalog contract

The root is an object with exactly these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `bundleFormatVersion` | string | Resource Schema Bundle contract version, currently `1.0`. |
| `resourceManifestSchemaVersion` | string | Resource Manifest and Resource Type schema semantics supported by the bundle, currently `1.0`. |
| `schemas` | array | Catalog entries in the deterministic order specified below. |

Every schema entry is an object with exactly these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `kind` | string | `manifest`, `resourceType`, or `dependency`. |
| `resourceType` | string or null | Resource Type lookup key; non-empty only for `resourceType`. |
| `factoryType` | string or null | Definition-factory lookup key. `null` denotes the default factory and is also used by non-resource entries. |
| `schemaId` | string | The document's absolute JSON Schema `$id`. |
| `document` | string | Safe bundle-relative location of the JSON Schema document. |
| `documentHash` | string | `sha256:` followed by exactly 64 lowercase hexadecimal digits, computed over the document bytes. |

A `manifest` entry identifies the root Resource Manifest schema. A `resourceType` entry participates in Resource Type/factory lookup. A `dependency` entry is catalog-only and participates in `$ref` resolution but not Resource Type enumeration. In the Willpower bundle, `common.schema.json` and `resource.schema.json` are dependency entries.

The built-in catalog therefore has twelve entries: one manifest, two dependencies, and these nine Resource Types: `AnimationSet`, `AudioBank`, `Image`, `ImageSet`, `Material`, `Program`, `Shader`, `TextFile`, and `XmlFile`.

## 4. Resolution and lookup

A consumer shall first load and validate the complete catalog and every document. It shall build a resolver keyed by `schemaId`; URI fragments are resolved inside the selected document according to JSON Pointer rules. Only documents in the catalog may satisfy a `$ref`, even when a schema ID uses `https`. The URI is an identifier, not permission to access the network.

For a Resource declaration and optional Definition `factory`:

1. use the exact `(resourceType, factoryType)` entry when present;
2. otherwise use `(resourceType, null)`, the default-factory entry; and
3. if no entry exists for the Resource Type, validate only against the common Resource declaration supplied by `resource.schema.json`.

The final rule deliberately preserves custom Resource Type compatibility. It does not make an unknown type valid if its common declaration is malformed. A specialized Definition that falls back to a default Resource Type schema receives the schema's generic-specialized-Definition behavior; consumers must not reinterpret it as a default Definition.

## 5. Determinism and required errors

Producers shall emit identical bytes for identical metadata and input documents. Entries are sorted by the bytewise lexical ordering of `kind`, `resourceType`, `factoryType`, `schemaId`, then document location. JSON uses two-space indentation, LF line endings, fixed field order as listed above, and one final newline. Documents retain their source bytes. Hashes therefore also remain stable.

A producer or consumer shall reject the entire bundle, rather than skipping an entry, when it encounters:

- a duplicate `(resourceType, factoryType)` key;
- the same `schemaId` associated with different document content;
- a missing, unsafe, or non-regular document;
- a malformed hash, unsupported hash algorithm, or digest mismatch;
- a document whose `$id` differs from `schemaId`;
- malformed JSON or a schema entry with the wrong field/type constraints; or
- a `$ref` whose target document or fragment cannot be resolved solely from bundle content.

The same schema ID may be shared by multiple lookup keys only when its document bytes are identical. Resource Type and factory comparisons are case-sensitive. Producers should avoid duplicate entries even in that permitted case.

## 6. Compatibility policy

Both versions use `MAJOR.MINOR` decimal strings. A major-version change may remove or reinterpret fields or validation behavior and requires explicit consumer support. A minor-version change is backward compatible: it may add optional capabilities, catalog entries, or stricter corrections that reject inputs never intended to be valid. Consumers must reject unsupported major versions and may accept a newer minor version after ignoring only additions explicitly documented as optional.

`bundleFormatVersion` versions the packaging and catalog rules. `resourceManifestSchemaVersion` independently versions declaration semantics. Changing one does not imply changing the other. Schema `$id` values remain stable within Resource Manifest schema major version 1; changed bytes are detected by `documentHash`.

## 7. Willpower locations

The `willpower_resource_schema_bundle` CMake target generates the built-in bundle at:

```text
<build-tree>/willpower.application/resource-schema-bundle
```

Installation places it at:

```text
<CMAKE_INSTALL_PREFIX>/<CMAKE_INSTALL_DATADIR>/willpower/resource-schema-bundle
```

On the usual platforms, `CMAKE_INSTALL_DATADIR` is `share`. Consumers start at `catalog.json`; no Willpower library or executable is needed.
