/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

(function() {
  var numIter = 15000;
  var len = 10000;
  var a = Array(len);
  for (var i = 0; i < len; i++) {
    a[i] = i;
  }

  for (var i = 0; i < numIter; i++) {
    a.fill(i);
  }

  print('done');
})();