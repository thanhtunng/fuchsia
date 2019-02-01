// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Library = `
{{- define "GenerateLibraryFile" -}}
// WARNING: This file is machine generated by fidlgen.

package {{ .Name }}

{{if .Libraries}}
import (
{{- range .Libraries }}
	{{ .Alias }} "{{ .Path }}"
{{- end }}
)
{{end}}

{{if .Consts}}
const (
{{- range $const := .Consts }}
	{{- range .DocComments}}
	//{{ . }}
	{{- end}}
	{{ .Name }} {{ .Type }} = {{ .Value }}
{{- end }}
)
{{end}}

{{ range $enum := .Enums -}}
{{ template "EnumDefinition" $enum }}
{{ end -}}
{{ range $struct := .Structs -}}
{{ template "StructDefinition" $struct }}
{{ end -}}
{{ range $union := .Unions -}}
{{ template "UnionDefinition" $union }}
{{ end -}}
{{ range $table := .Tables -}}
{{ template "TableDefinition" $table }}
{{ end -}}
{{ range $interface := .Interfaces -}}
{{ template "InterfaceDefinition" $interface }}
{{ end -}}

{{- end -}}
`
