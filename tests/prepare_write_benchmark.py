#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Prepare identical, disposable Linux/core writer volumes and a static guest probe."""
from prepare_read_benchmark import main

if __name__ == '__main__':
    main(write=True)
