# Resource Manifest document and headless validation

`Willpower.Application` exposes a library-neutral
`ResourceManifestDocument` contract in
`willpower/application/resourcesystem/ResourceManifestDocument.h`. It parses or loads
one YAML document, validates it against an immutable `ResourceSchemaCatalogSnapshot`,
and emits canonical YAML. The public types contain no yaml-cpp, JSON, or validator
objects.

Parsing distinguishes YAML syntax, defensive-limit, and filesystem failures. Structural
schema failures are returned by `validate()` with bounded diagnostics containing the
manifest path, Resource identity when available, instance path, source location, and
message. Validation resolves `$ref` only from the supplied catalog snapshot; it performs
no filesystem or network schema lookup.

Canonical output is deterministic UTF-8 YAML: LF endings, two-space indentation, block
mappings and sequences, native null/boolean/number scalars, safely quoted strings, and
one final newline. Mapping and sequence order is preserved. Comments, anchors, aliases,
quoting choices, and flow style are intentionally normalized.

Default defensive limits are 64 MiB of input, depth 128, 1,000,000 YAML nodes, and 100
reported structural diagnostics. `ResourceManifestLimits` lets API callers lower these
bounds.

## `resource-manager` headless command

Ticket #38 provides the non-interactive executable seam; it does not initialize SDL or
open dialogs:

```sh
resource-manager --validate Resources.yaml --base-directory assets
resource-manager --validate Resources.yaml --base-directory assets \
  --canonical-output canonical.yaml
resource-manager --validate Resources.yaml --base-directory assets \
  --canonical-output -
```

Deployment schemas can be checked separately without opening SDL or a document:

```sh
resource-manager --verify-schemas --ini /installed/bin/resource-manager.ini
```

This rereads the INI, resolves its optional base and ordered extension bundle paths,
and verifies metadata, versions, hashes, IDs, collisions, closed references, and editor
annotations. It reports catalog and Resource Type counts on success. Usage errors return
`2`; an INI, bundle, or annotation failure returns `3`. Bundle schema IDs never permit
network retrieval, and this command does not load Resource Type Plugins or application
code.

Both the Resource Manifest and base directory are mandatory. `.yaml` and `.yml` are
accepted case-insensitively. The base directory must exist and be a directory, but this
structural command does not load source assets or perform the semantic checks reserved
for later editor work. `--canonical-output` validates, serializes, reparses, and validates
the canonical bytes before writing them; `-` writes only those bytes to standard output.

Diagnostics use distinct `YAML syntax`, `structural validation`, `defensive limit`, and
`filesystem` labels. Stable process exit codes are:

| Code | Meaning |
| ---: | --- |
| 0 | valid Resource Manifest (and successful canonical round-trip when requested) |
| 2 | command-line usage or unsupported manifest extension |
| 4 | YAML syntax, structural validation, or manifest defensive-limit failure |
| 6 | Resource Manifest or base-directory filesystem failure |
| 7 | canonical serialization/output failure |
| 9 | internal round-trip failure |

YAML and structural failures intentionally share process code 4, as specified by the
Resource Manifest Editor command-line contract, while retaining distinct public API
statuses and diagnostic kinds.
