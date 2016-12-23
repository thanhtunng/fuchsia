// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/modular/examples/todo_cpp/generator.h"

#include <iostream>

#include "lib/ftl/strings/concatenate.h"

namespace todo {
Generator::Generator(std::default_random_engine* rng) : rng_(rng) {
  actions_ = {
      "acquire",      "cancel",      "consider",
      "draw",         "evaluate",    "celebrate",
      "find",         "identify",    "meet with",
      "plan",         "solve",       "study",
      "talk to",      "think about", "write an article about",
      "check out",    "order",       "write a spec for",
      "order",        "track down",  "memorize",
      "git checkout",
  };
  action_distribution_ =
      std::uniform_int_distribution<>(0, actions_.size() - 1);

  objects_ = {
      "Christopher Columbus",
      "PHP",
      "a better way forward",
      "a glass of wine",
      "a good book on C++",
      "a huge simulation we are all part of",
      "a nice dinner out",
      "a sheep",
      "an AZERTY keyboard",
      "hipster bars south of Pigalle",
      "kittens",
      "manganese",
      "more cheese",
      "some bugs",
      "staticly-typed programming languages",
      "the cryptographic primitives",
      "the espresso machine",
      "the law of gravity",
      "the neighbor",
      "the pyramids",
      "the society",
      "velocity",
  };
  object_distribution_ =
      std::uniform_int_distribution<>(0, objects_.size() - 1);

  auto tag_distribution = std::uniform_int_distribution<>(10'000, 99'999);
  tag_ =
      ftl::Concatenate({"[ ", std::to_string(tag_distribution(*rng_)), " ] "});
  std::cout << "Items generated by this instace are tagged with: " << tag_
            << std::endl;
}
Generator::~Generator() {}

std::string Generator::Generate() {
  return ftl::Concatenate({tag_, actions_[action_distribution_(*rng_)], " ",
                           objects_[object_distribution_(*rng_)]});
}

}  // namespace todo
