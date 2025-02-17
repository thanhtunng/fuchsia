# Fuchsia System Interface

## Overview

The *Fuchsia System Interface* is the binary interface that the Fuchsia
operating system presents to software it runs. The foundation of the interface
is the vDSO, which provides access to the system calls. Programs are not allowed
to issue system calls directly (e.g., by trapping into the kernel). Instead,
programs use the vDSO to interface with the kernel.

The bulk of the system interface is provided through inter-process
communication protocols, typically defined using FIDL. These protocols are
spoken over various kernel primitives, including channels and sockets.

The `fuchsia.io` FIDL library provides protocols for file and directory
operations. Fuchsia uses the `fuchsia.io` protocols to provide a namespace to
components through which components can access system services and resources.
The names in these namespaces follow certain conventions, which are part of the
system ABI. See [namespaces](/docs/concepts/process/namespaces.md) for more details.

Packages themselves also provide an interface to the system in terms of
directory structure and file formats. The system uses this information to
initialize processes in which components stored in these packages execute.

## Terminology

The *Application Programming Interface* (API) for a system is the source-level
interface to the system. You typically write software that uses this
interface directly. Changes to the system API might require you to update
your source code to account for the changes to the API.

The *Application Binary Interface* (ABI) for a system is the binary-level interface
to the system. Typically you don't write software that uses the system ABI
directly. Instead, you write software against the system API. When the software
is compiled, the binary artifact created by the compiler interfaces with the system
through the ABI. Changes to the system ABI may require you to recompile
your source code to account for the changes in the ABI.

## ABI Surfaces

This section describes the various ABI surfaces for Fuchsia components.

### vDSO

The vDSO is a virtual shared library that provides access to the kernel.
Concretely, the vDSO is an ELF shared library, called `libzircon.so`, that
exports a number of symbols with a C calling convention. The source of truth for
these symbols is [//zircon/vdso](/zircon/vdso/). Their semantics are
described in [the documentation](/docs/reference/syscalls/).

### FIDL protocols

The bulk of the system interfaces are defined in the Fuchsia Interface
Definition Language (FIDL). The FIDL compiler produce language specific APIs and
runtime libraries, referred to as FIDL bindings, for a variety of target
languages. These bindings provide an idiomatic interface for sending and
receiving interprocess communication messages over Zircon channels (and other
primitives).

#### Wire format

The FIDL protocol definitions themselves and the language-specific bindings
generated by the compiler are part of the system *API* but not part of the
system *ABI*. Instead, the format of the serialized messages, called the *wire
format*, comprises the ABI. The FIDL wire format is defined by the
[specification](/docs/reference/fidl/language/wire-format/README.md).

#### User signals

In addition to the messages sent, some FIDL protocols make use of user signals
on the underlying kernel objects. Currently, these signals are not declared in
FIDL. Typically, the semantics of any associated user signals are documented
in prose in comments in the FIDL definitions.

### Namespace conventions

When run, components are given a [namespace][glossary.namespace] and serve an
[outgoing directory][glossary.outgoing-directory].
The names in the namespace and outgoing directory follow certain conventions,
which are are part of the system ABI.

#### Namespace

A component's [namespace][glossary.namespace] is provided during startup and
lets the component interact with capabilities published by the rest of the
system.

For example, components can discover implementations of FIDL
[protocols][glossary.protocol] and [services][glossary.service] through the
`/svc` entry in this namespace, which lists them under well-known names.
Similarly, by convention, the `pkg` namespace entry is mapped to the package
from which the component was resolved.

#### Outgoing directory {#outgoing_directory}

A component serves an [outgoing directory][glossary.outgoing-directory] to
publish capabilities to the rest of the system.

For example, components expose FIDL [protocols][glossary.protocol] and
[services][glossary.service] to other components using the `/svc` entry in this
directory, which lists them under well-known names.

#### Data formats

Some namespaces include files with data. The data format used by these files is
also part of the system ABI.
For example, components access the root certificates through a namespace entry
that contains a `certs.pem` file. The `pem` data format is therefore part of the
system ABI.

### Package conventions

Fuchsia packages have a directory structure that follows certain naming
conventions. These conventions are also part of the system ABI. This section
gives two examples of important packaging conventions.

#### meta

By convention, the `meta` directory in a package contains metadata files that
describe the package. The structure of this metadata, including the data formats
used by these files such as [component manifests][glossary.component-manifest],
is part of the system ABI.

#### lib

By convention, the `lib` directory in a package contains the shared libraries
used by components in the package. When the system runs an executable from the
package, requests for shared libraries are resolved relative to this `lib`
directory.

One important difference between Fuchsia and other operating systems is that the
shared libraries themselves are provided by the package creator rather than the
system itself. For that reason, the shared libraries themselves (including
`libc`) are not part of the system ABI.

The system does provide two shared libraries: the [vDSO](#vdso) and the
[Vulkan ICD](#vulkan-icd). See those sections for details.

### Process structure

Processes on Fuchsia are fairly flexible and largely under the control of the
executable running in the process, but some of the initial structure of the
process is controlled by the system and part of the system ABI.

For additional details, see
[Program Loading](/docs/concepts/booting/program_loading.md).

#### ELF loader

Fuchsia uses the ELF data format for executables. When loading an executable
into a process, the loader parses contents of the executable as ELF. The loader
reads the `INTERP` directive from the executable and resolves that name as a
file in the `lib` directory of the package that contained the executable. The
loader then parses the contents of the `INTERP` file as an ELF shared library,
relocates the library, and maps the library into the newly created process.

#### Startup message

As part of starting a process, the creator of the process supplies the process
with a message that contains, for example, the command line arguments, the
`environ`, the initial handles, and the namespace for the process.
(The outgoing directory is included in the set of initial handles for the
process.)

The format of this message, including length limitations on fields such as the
command line arguments and the `environ`, are part of the system ABI, as are
the conventions around the contents of the message. For example, by convention,
the `PWD` environment variable is the name the creator suggests the process use
as its current working directory.

The initial handles are associated with numerical identifiers. The conventions
around these identifiers are part of the system ABI. For example, by convention,
the `PA_PROC_SELF` handle is a handle to the process object for the newly
created process. In addition to the types of these handles, the rights
associated with these handles are also part of the system ABI.

#### VMAR structure

Before starting a process, the creator modifies the root VMAR for the process.
For example, the creator maps the vDSO and allocates a stack for the initial
thread. The structure of the VMAR when the process is started is part of the
system ABI.

#### Job policy

Processes are run in jobs, which can apply policy to the processes and jobs they
contain. The job policy applied to processes is part of the system ABI. For
example, components run in processes with `ZX_POL_NEW_PROCESS` set to
`ZX_POL_ACTION_DENY`. This enforces that the component use the
`fuchsia.process.Launcher` protocol to create processes rather than issuing the
`zx_process_create` system call directly.

### Vulkan ICD {#vulkan-icd}

Components that use the Vulkan API for hardware accelerated graphics link
against `libvulkan.so` and specify the `vulkan` feature in their manifests. This
library is provided by the package that contains the component and therefore is
not part of the system ABI. However, `libvulkan.so` loads another shared
library, called the *Vulkan Installable Client Driver* (Vulkan ICD). The Vulkan
ICD is loaded using `fuchsia.vulkan.loader.Loader`, which means the library is
provided by the system itself rather than the package that contains the
component. For this reason, the Vulkan ICD is part of the system ABI.

The Vulkan ICD is an ELF shared library that exports exactly three symbols.
These symbols are reserved for use by the Vulkan ICD and should not be used
directly.

 * `vk_icdGetInstanceProcAddr`
 * `vk_icdNegotiateLoaderICDInterfaceVersion`
 * `vk_icdInitializeOpenInNamespaceCallback`

In addition, the Vulkan ICD shared library has a `NEEDED` section that lists
several shared libraries upon which the Vulkan ICD depends. The package
containing the component is required to provide these shared libraries.

The Vulkan ICD also imports a number of symbols. The conventions around these
imported symbols, for example their parameters and semantics, are part of the
system ABI.

Currently, the `NEEDED` section and the list of imported symbols for the Vulkan
ICD are both larger than we desire. Hopefully we will be able to minimize these
aspects of the system ABI.

### Sockets

#### Datagram framing

Datagram sockets used for networking include a frame that specifies the network
address associated with the datagram. This frame is also part of the system ABI.

### Terminal protocol

Programs that run in the terminal communicate with the terminal using the
Fuchsia Terminal Protocol, which is a text-based protocol similar to `vt100`.
This protocol is also exposed over the network through `ssh`, both by clients
that expect incoming `ssh` connections to support this protocol and by servers
that expect outgoing `ssh` connections to support this protocol.

[glossary.component-manifest]: /docs/glossary/README.md#component-manifest
[glossary.namespace]: /docs/glossary/README.md#namespace
[glossary.outgoing-directory]: /docs/glossary/README.md#outgoing-directory
[glossary.protocol]: /docs/glossary/README.md#protocol
[glossary.service]: /docs/glossary/README.md#service
