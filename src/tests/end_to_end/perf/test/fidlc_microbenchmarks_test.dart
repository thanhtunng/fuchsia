// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:test/test.dart';

import 'helpers.dart';

void main() {
  enableLoggingOutput();

  test('/bin/fidlc_microbenchmarks', () async {
    final helper = await PerfTestHelper.make();
    await helper.runTestComponent(
        packageName: 'fidlc_microbenchmarks',
        componentName: 'fidlc_microbenchmarks.cmx',
        commandArgs: '-p --quiet --out ${PerfTestHelper.componentOutputPath}');
  }, timeout: Timeout.none);
}
