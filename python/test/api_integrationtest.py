#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import io
import os
import unittest

from perfetto.trace_processor.api import TraceProcessor
from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.trace_processor.api import TraceReference


def create_tp(trace: TraceReference):
  return TraceProcessor(
      trace=trace,
      config=TraceProcessorConfig(bin_path=os.environ["SHELL_PATH"]))


def example_android_trace_path():
  return os.path.join(os.environ["ROOT_DIR"], 'test', 'data',
                      'example_android_trace_30s.pb')


class TestApi(unittest.TestCase):

  def test_trace_path(self):
    # Get path to trace_processor_shell and construct TraceProcessor
    tp = create_tp(trace=example_android_trace_path())
    qr_iterator = tp.query('select * from slice limit 10')
    dur_result = [
        178646, 119740, 58073, 155000, 173177, 20209377, 3589167, 90104, 275312,
        65313
    ]

    for num, row in enumerate(qr_iterator):
      self.assertEqual(row.type, 'internal_slice')
      self.assertEqual(row.dur, dur_result[num])

    # Test the batching logic by issuing a large query and ensuring we receive
    # all rows, not just a truncated subset.
    qr_iterator = tp.query('select count(*) as cnt from slice')
    expected_count = next(qr_iterator).cnt
    self.assertGreater(expected_count, 0)

    qr_iterator = tp.query('select * from slice')
    count = sum(1 for _ in qr_iterator)
    self.assertEqual(count, expected_count)

    tp.close()

  def test_trace_byteio(self):
    f = io.BytesIO(
        b'\n(\n&\x08\x00\x12\x12\x08\x01\x10\xc8\x01\x1a\x0b\x12\t'
        b'B|200|foo\x12\x0e\x08\x02\x10\xc8\x01\x1a\x07\x12\x05E|200')
    with create_tp(trace=f) as tp:
      qr_iterator = tp.query('select * from slice limit 10')
      res = list(qr_iterator)

      self.assertEqual(len(res), 1)

      row = res[0]
      self.assertEqual(row.ts, 1)
      self.assertEqual(row.dur, 1)
      self.assertEqual(row.name, 'foo')

  def test_trace_file(self):
    with open(example_android_trace_path(), 'rb') as file:
      with create_tp(trace=file) as tp:
        qr_iterator = tp.query('select * from slice limit 10')
        dur_result = [
            178646, 119740, 58073, 155000, 173177, 20209377, 3589167, 90104,
            275312, 65313
        ]

        for num, row in enumerate(qr_iterator):
          self.assertEqual(row.dur, dur_result[num])

  def test_trace_generator(self):

    def reader_generator():
      with open(example_android_trace_path(), 'rb') as file:
        yield file.read(1024)

    with create_tp(trace=reader_generator()) as tp:
      qr_iterator = tp.query('select * from slice limit 10')
      dur_result = [
          178646, 119740, 58073, 155000, 173177, 20209377, 3589167, 90104,
          275312, 65313
      ]

      for num, row in enumerate(qr_iterator):
        self.assertEqual(row.dur, dur_result[num])
