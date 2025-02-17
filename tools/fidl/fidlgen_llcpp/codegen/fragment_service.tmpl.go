// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fragmentServiceTmpl = `
{{- define "ServiceForwardDeclaration" }}
{{ EnsureNamespace . }}
class {{ .Name }};
{{- end }}

{{- define "ServiceDeclaration" }}
{{ EnsureNamespace . }}
{{ "" }}
{{- .Docs }}
class {{ .Name }} final {
  {{ .Name }}() = default;
 public:
  static constexpr char Name[] = "{{ .ServiceName }}";

  // Client protocol for connecting to member protocols of a service instance.
  class ServiceClient final {
    ServiceClient() = delete;
   public:
    ServiceClient(::zx::channel dir, ::fidl::internal::ConnectMemberFunc connect_func)
    {{- with .Members }}
        : dir_(std::move(dir)), connect_func_(connect_func) {}
    {{- else }}
        { (void)dir; (void)connect_func; }
    {{- end }}
    {{- range .Members }}
  {{ "" }}
    // Connects to the member protocol "{{ .Name }}".
    // Returns a |fidl::ClientEnd<{{ .ProtocolType }}>| on success,
    // which can be used with |fidl::BindSyncClient| to create a synchronous
    // client, or |fidl::WireClient| or |fidl::WireSharedClient| to create a
    // client that supports both asynchronous and synchronous operations.
    //
    // # Errors
    //
    // On failure, returns a |zx::error| with status != ZX_OK.
    // Failures can occur if channel creation failed, or if there was an issue making
    // a |fuchsia.io.Directory/Open| call.
    //
    // Since the call to |Open| is asynchronous, an error sent by the remote end will not
    // result in a failure of this method. Any errors sent by the remote will appear on
    // the |ClientEnd| returned from this method.
    ::zx::status<::fidl::ClientEnd<{{ .ProtocolType }}>> connect_{{ .Name }}() {
      auto endpoints = ::fidl::CreateEndpoints<{{ .ProtocolType }}>();
      if (endpoints.is_error()) {
        return endpoints.take_error();
      }
      auto connection = connect_func_(
          ::zx::unowned_channel(dir_),
          ::fidl::StringView("{{ .Name }}"),
          endpoints->server.TakeChannel());
      if (connection.is_error()) {
        return connection.take_error();
      }
      return ::zx::ok(std::move(endpoints->client));
    }
    {{- end }}

   private:
   {{- with .Members }}
    ::zx::channel dir_;
    ::fidl::internal::ConnectMemberFunc connect_func_;
   {{- end }}
  };

  // Facilitates member protocol registration for servers.
  class Handler final {
   public:
    // Constructs a FIDL Service-typed handler. Does not take ownership of |service_handler|.
    explicit Handler(::fidl::ServiceHandlerInterface* service_handler)
    {{- with .Members }}
        : service_handler_(service_handler) {}
    {{- else }}
        { (void)service_handler; }
    {{- end }}

    {{- range .Members }}
    {{ "" }}
    // Adds member "{{ .Name }}" to the service instance. |handler| will be invoked on connection
    // attempts.
    //
    // # Errors
    //
    // Returns ZX_ERR_ALREADY_EXISTS if the member was already added.
    ::zx::status<> add_{{ .Name }}(
        ::fidl::ServiceHandlerInterface::MemberHandler<{{ .ProtocolType }}> handler) {
      return service_handler_->AddMember("{{ .Name }}", std::move(handler));
    }
    {{- end }}

   private:
   {{- with .Members }}
    ::fidl::ServiceHandlerInterface* service_handler_;  // Not owned.
   {{- end }}
  };
};
{{- end }}
`
