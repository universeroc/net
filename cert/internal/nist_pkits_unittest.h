// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_CERT_INTERNAL_NIST_PKITS_UNITTEST_H_
#define NET_CERT_INTERNAL_NIST_PKITS_UNITTEST_H_

#include <set>

#include "net/cert/internal/test_helpers.h"
#include "net/der/parse_values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

// Describes the inputs and outputs (other than the certificates) for
// the PKITS tests.
struct PkitsTestInfo {
  // Default construction results in the "default settings".
  PkitsTestInfo();
  ~PkitsTestInfo();

  // Sets |initial_policy_set| to the specified policies. The
  // policies are described as comma-separated symbolic strings like
  // "anyPolicy" and "NIST-test-policy-1".
  //
  // If this isn't called, the default is "anyPolicy".
  void SetInitialPolicySet(const char* const policy_names);

  // Sets |user_constrained_policy_set| to the specified policies. The
  // policies are described as comma-separated symbolic strings like
  // "anyPolicy" and "NIST-test-policy-1".
  //
  // If this isn't called, the default is "NIST-test-policy-1".
  void SetUserConstrainedPolicySet(const char* const policy_names);

  void SetInitialExplicitPolicy(bool b);
  void SetInitialPolicyMappingInhibit(bool b);
  void SetInitialInhibitAnyPolicy(bool b);

  // ----------------
  // Inputs
  // ----------------

  // A set of policy OIDs to use for "initial-policy-set".
  std::set<der::Input> initial_policy_set;

  // The value of "initial-explicit-policy".
  bool initial_explicit_policy = false;

  // The value of "initial-policy-mapping-inhibit".
  bool initial_policy_mapping_inhibit = false;

  // The value of "initial-inhibit-any-policy".
  bool initial_inhibit_any_policy = false;

  // This is the time when PKITS was published.
  der::GeneralizedTime time = {2011, 4, 15, 0, 0, 0};

  // ----------------
  // Expected outputs
  // ----------------

  // Whether path validation should succeed.
  bool should_validate = false;

  std::set<der::Input> user_constrained_policy_set;
};

// Parameterized test class for PKITS tests.
// The instantiating code should define a PkitsTestDelegate with an appropriate
// static RunTest method, and then INSTANTIATE_TYPED_TEST_CASE_P for each
// testcase (each TYPED_TEST_CASE_P in pkits_testcases-inl.h).
template <typename PkitsTestDelegate>
class PkitsTest : public ::testing::Test {
 public:
  template <size_t num_certs, size_t num_crls>
  void RunTest(const char* const (&cert_names)[num_certs],
               const char* const (&crl_names)[num_crls],
               const PkitsTestInfo& info) {
    std::vector<std::string> cert_ders;
    for (const std::string& s : cert_names)
      cert_ders.push_back(net::ReadTestFileToString(
          "net/third_party/nist-pkits/certs/" + s + ".crt"));
    std::vector<std::string> crl_ders;
    for (const std::string& s : crl_names)
      crl_ders.push_back(net::ReadTestFileToString(
          "net/third_party/nist-pkits/crls/" + s + ".crl"));
    PkitsTestDelegate::RunTest(cert_ders, crl_ders, info);
  }
};

// Inline the generated test code:
#include "net/third_party/nist-pkits/pkits_testcases-inl.h"

}  // namespace net

#endif  // NET_CERT_INTERNAL_NIST_PKITS_UNITTEST_H_
