# FIDL attributes

The following FIDL attributes are supported:

* [`@deprecated`](#deprecated)
* [`@discoverable`](#discoverable)
* [`@doc`](#doc)
* [`@for_deprecated_c_bindings`](#layout)
* [`@max_bytes`](#maxbytes)
* [`@max_handles`](#maxhandles)
* [`@no_doc`](#nodoc)
* [`@selector`](#selector)
* [`@transitional`](#transitional)
* [`@transport`](#transport)
* [`@unknown`](#unknown)

## Scope

An attribute precedes a FIDL element, for example:

```fidl
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/fuchsia.examples.docs/attributes.test.fidl" region_tag="attribute-one" %}
```

It's used to either modify the characteristics of the element, or provide
documentation.

Note: The attribute applies *only* to the *next* element, not all
subsequent ones.
Elements after the current one revert to having no attributes.

## Syntax

Attributes may include values, and multiple attributes may be stacked, for example:

```fidl
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/fuchsia.examples.docs/attributes.test.fidl" region_tag="attribute-many" %}
```

Illustrates both aspects:

* there are two attributes, `@discoverable` and `@transport`, and
* the `@transport` attribute takes a value from the list enumerated below.

## `@deprecated` {#deprecated}

**USAGE**: `@deprecated`

**MEANING**:
See [RFC-0058].

Note: Not implemented.

## `@discoverable` {#discoverable}

**USAGE**: `@discoverable`

**MEANING**:
Automatically generates a path name for the annotated protocol. That is to say,
a `@discoverable` protocol can be served under the generated name, and clients
that connect to that protocol can search for it under the same generated name.
This makes it possible to have a client search for the correct name without
manually ensuring that the lookup name matches the one passed on the server side.

## `@doc` {#doc}

**USAGE**: `@doc("`_string_`")`

**MEANING**:
In FIDL, comments can start with two ("`//`") or three slashes ("`///`"),
or they can be embodied within a `@doc` attribute.
The two-slash variant does not propagate the comments to the generated
target, whereas both the three-slash and `@doc` variants do.

That is:

```fidl
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/fuchsia.examples.docs/attributes.test.fidl" region_tag="doc-with-slashes" %}
```

and

```fidl
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/fuchsia.examples.docs/attributes.test.fidl" region_tag="doc-with-attributes" %}
```

have the same effect &mdash; one ("`///`") is syntactic sugar for the other.
The text of the comment is
emitted into the generated code, in a manner compatible with the syntax of
the target language.

Note: To be identical, the `@doc` version should be `@doc(" Foo\n")`. Note
the space before the "Foo" and the line-feed "`\n`".

## `@for_deprecated_c_bindings` {#layout}

**USAGE**: `@for_deprecated_c_bindings`

**MEANING**:
This attribute is used to ensure that a protocol is compatible with the
deprecated C bindings. There should be no new uses of this attribute.

```fidl
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/fuchsia.examples.docs/attributes.test.fidl" region_tag="layout-simple" %}
```

## `@max_bytes` {#maxbytes}

<!-- TODO(fxbug.dev/74955): Remove double quotes around numeric arguments. -->

**USAGE**: `@max_bytes("`_number_`")`

**MEANING**:
This attribute is used to limit the number of bytes that can be transferred
in a message.
The compiler will issue an error if the number of bytes exceeds this limit.

## `@max_handles` {#maxhandles}

**USAGE**: `@max_handles("`_number_`")`

**MEANING**:
This attribute is used to limit the number of handles that can be
transferred in a message.
The compiler will issue an error if the number of handles exceeds this limit.

## `@no_doc` {#nodoc}

**USAGE**: `@no_doc`

**MEANING**:
This attribute is used to mark libraries that should be skipped by documentation
generation tools. As an example, this attribute is used by generated FIDL
libraries, such as by the [driver bind compiler](/docs/concepts/drivers/device_driver_model/driver-binding.md).

## `@selector` {#selector}

**USAGE**: `@selector("`_selector_`")`

**MEANING**:
Allows you to change the hashing basis for the method ordinal, see [RFC-0020].
The *_selector_* can be either the original method's name (e.g. `SomeMethod`),
or the fully qualified name (e.g. `some.library/SomeProtocol.SomeMethod`).

It can be used to rename a method without breaking ABI compatibility.
For example, if we wish to rename the `Investigate` method to `Experiment`
in the `Science` interface, we can write:

```fidl
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/fuchsia.examples.docs/attributes.test.fidl" region_tag="selector-simple" %}
```

The attribute can also be used to handle a method moving from one protocol to
another, or to one library to another, or both. For example, consider the method
`Productionize` on the `Org` protocol in the `fuchsia.examples.docs` library
which was originally named `Discover` on the `Area120` protocol, in the
`purple.examples.docs` library:

```fidl
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/fuchsia.examples.docs/attributes.test.fidl" region_tag="selector-fq-name" %}
```

## `@transitional` {#transitional}

**USAGE**: `@transitional("`_description_`")`

**MEANING**:
Instructs bindings to generate code that will successfully build, regardless of
whether the method is implemented or not.
[RFC-0021] contains more details.

## `@transport` {#transport}

**USAGE**: `@transport("`_tranportList_`")`

**MEANING**:
Allows you to select a transport.
Provide a comma-separated list of values, selected from:

* `Channel` &mdash; use a [Zircon channel][channel].
* `Syscall` &mdash; transport used to specify that the protocol is used to
  define Zircon syscalls, rather than typical IPC.

The default is `Channel` if none specified.
If you do specify a value or values, then only those values are used (e.g.,
specifying `@transport("Foo")` disables `Channel` and uses only
`Foo`).

## `@unknown` {#unknown}

**USAGE**: `@unknown`

**MEANING**:
`@unknown` can be placed on an enum member to indicate that this member represents
a specific unknown placeholder. Its purpose is to make it possible to transition a strict enum
that was manually implementing flexible behavior with an extra "unknown" member
to transition into a flexible enum: annotating the member representing unknown
with the `@unknown` attribute before transitioning to flexible ensures that
the `@unknown` member is treated as unknown data, instead of as a
known member. Once usages of the `@unknown` member are removed, the member is
no longer necessary.

<!-- xrefs -->
[channel]: /docs/reference/kernel_objects/channel.md
[RFC-0058]: /docs/contribute/governance/rfcs/0058_deprecated_attribute.md
[RFC-0020]: /docs/contribute/governance/rfcs/0020_interface_ordinal_hashing.md
[RFC-0021]: /docs/contribute/governance/rfcs/0021_soft_transitions_methods_add_remove.md
