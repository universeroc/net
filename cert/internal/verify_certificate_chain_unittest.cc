// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/internal/verify_certificate_chain.h"

#include "net/cert/internal/signature_policy.h"
#include "net/cert/internal/trust_store.h"
#include "net/cert/internal/verify_certificate_chain_typed_unittest.h"

namespace net {

namespace {

class VerifyCertificateChainDelegate {
 public:
  static void Verify(const VerifyCertChainTest& test,
                     const std::string& test_file_path) {
    SimpleSignaturePolicy signature_policy(1024);

    CertPathErrors errors;
    // TODO(eroman): Check user_constrained_policy_set.
    VerifyCertificateChain(
        test.chain, test.last_cert_trust, &signature_policy, test.time,
        test.key_purpose, test.initial_explicit_policy,
        test.user_initial_policy_set, test.initial_policy_mapping_inhibit,
        test.initial_any_policy_inhibit,
        nullptr /*user_constrained_policy_set*/, &errors);
    EXPECT_EQ(test.expected_errors, errors.ToDebugString(test.chain))
        << "Test file: " << test_file_path;
  }
};

}  // namespace

INSTANTIATE_TYPED_TEST_CASE_P(VerifyCertificateChain,
                              VerifyCertificateChainSingleRootTest,
                              VerifyCertificateChainDelegate);

}  // namespace net
