# Resource Schema editor annotations 1.0

- **Status:** Accepted
- **Annotation vocabulary version:** 1.0
- **Related issue:** [#45](https://github.com/ajare/willpower/issues/45)

Resource Schema editor annotations are optional JSON Schema Draft 7 metadata for the
Resource Manifest Editor. They do not change Resource Manifest validation semantics;
Draft 7 consumers that do not recognize them ignore them. Annotation versions are
independent of Resource Schema Bundle and Resource Manifest schema versions.

Every annotated property has an `x-willpower-editor-version` sibling. Widget
annotations additionally have `x-willpower-widget`:

| Keyword | Value | Meaning |
| --- | --- | --- |
| `x-willpower-editor-version` | `"1.0"` | Annotation vocabulary version. |
| `x-willpower-widget` | `"file"` or `"resource-reference"` | Optional requested editor control. |

A property containing any `x-willpower-*` keyword must contain exactly the version,
the applicable metadata keyword, and any widget-specific keywords below. Unknown
keywords, unsupported versions or widgets, missing values, wrong JSON types, empty
values, duplicate allowed Resource Types, and invalid file extensions make the complete
candidate editor catalog invalid. Annotation validation is an editor catalog-loading
rule, not a JSON Schema validation keyword.

## Editor default

An option value schema may declare `x-willpower-editor-default`. The value must be a
scalar accepted by that option: a member of its string `enum`, or a JSON boolean for a
reference to the common boolean definition. When the editor creates a Resource, it
writes annotated defaults into that Resource's `Option` collection. This keyword is
editor-only metadata: it does not change JSON Schema validation or runtime Resource
loading behavior.

```json
"value": {
  "enum": ["none", "linear"],
  "x-willpower-editor-version": "1.0",
  "x-willpower-editor-default": "none"
}
```

## File widget

A file property adds:

| Keyword | Value |
| --- | --- |
| `x-willpower-file-kind` | Non-empty human-readable native-selector filter name. |
| `x-willpower-file-extensions` | Non-empty array of extension names without `.`, `*`, `;`, or path separators. |

```json
"location": {
  "$ref": "https://schemas.willpower.dev/resource-manifest/common.schema.json#/definitions/nonEmptyString",
  "x-willpower-editor-version": "1.0",
  "x-willpower-widget": "file",
  "x-willpower-file-kind": "XML files",
  "x-willpower-file-extensions": ["xml"]
}
```

A file widget accepts only an existing regular file after canonicalizing both it and the
active document base directory. The canonical target must remain inside that canonical
base subtree; `..` traversal and symbolic-link/junction escapes are rejected. The value
committed to the Resource Manifest is the base-relative path with `/` separators.

## Resource-reference widget

A Resource-reference property adds:

| Keyword | Value |
| --- | --- |
| `x-willpower-allowed-resource-types` | Non-empty array of unique, non-empty, case-sensitive Resource Type names. |
| `x-willpower-reference-scope` | `"manifest"` in version 1.0. Choices come from namespace-level Resources in the active Resource Manifest. |

```json
"ref": {
  "$ref": "https://schemas.willpower.dev/resource-manifest/common.schema.json#/definitions/nonEmptyString",
  "x-willpower-editor-version": "1.0",
  "x-willpower-widget": "resource-reference",
  "x-willpower-allowed-resource-types": ["Image"],
  "x-willpower-reference-scope": "manifest"
}
```

Version 1.0 applies Resource-reference annotations to standard
`DependentResources/DependentResource/ref` properties. The containing dependency shape
identifies its dependency `id` with a one-value enum. The editor displays qualified
manifest Resource identities, filters them to the allowed types, omits new inline
Resource targets, and preserves existing missing values for repair. Standard rename,
move, cycle, and deletion safeguards continue to apply. Unannotated strings are never
guessed to be paths or Resource references.

The built-in file-backed schemas annotate `location`. Built-in ImageSet, AnimationSet,
Program, and Material dependency schemas annotate their constrained `ref` properties.
All annotations remain data in the bundle; consuming them does not load application
code, Resource Type Plugins, or network resources.
