#!/usr/bin/python
# Copyright (c) 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Certificate chain where the target certificate sets the extended key usage
to clientAuth. Neither the root nor the intermediate have an EKU."""

import sys
sys.path += ['..']

import common

# Self-signed root certificate.
root = common.create_self_signed_root_certificate('Root')

# Intermediate certificate.
intermediate = common.create_intermediate_certificate('Intermediate', root)

# Target certificate.
target = common.create_end_entity_certificate('Target', intermediate)
target.get_extensions().set_property('extendedKeyUsage', 'clientAuth')

chain = [target, intermediate, root]
common.write_chain(__doc__, chain, 'chain.pem')
