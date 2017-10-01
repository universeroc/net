#!/usr/bin/python
# Copyright 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Pretty-prints certificates as an openssl-annotated PEM file."""

import argparse
import base64
import errno
import os
import re
import subprocess
import sys


def read_file_to_string(path):
  with open(path, 'r') as f:
    return f.read()


def read_certificates_data_from_server(hostname):
  """Uses openssl to fetch the PEM-encoded certificates for an SSL server."""
  p = subprocess.Popen(["openssl", "s_client", "-showcerts",
                        "-servername", hostname,
                        "-connect", hostname + ":443"],
                        stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE)
  result = p.communicate()

  if p.returncode == 0:
    return result[0]

  sys.stderr.write("Failed getting certificates for %s:\n%s\n" % (
      hostname, result[1]))
  return ""


def read_sources_from_commandline(sources):
  """Processes the command lines and returns an array of all the sources
  bytes."""
  sources_bytes = []

  if not sources:
    # If no command-line arguments were given to the program, read input from
    # stdin.
    sources_bytes.append(sys.stdin.read())
  else:
    for arg in sources:
      # If the argument identifies a file path, read it
      if os.path.exists(arg):
        sources_bytes.append(read_file_to_string(arg))
      else:
        # Otherwise treat it as a web server address.
        sources_bytes.append(read_certificates_data_from_server(arg))

  return sources_bytes


def strip_indentation_whitespace(text):
  """Strips leading whitespace from each line."""
  stripped_lines = [line.lstrip() for line in text.split("\n")]
  return "\n".join(stripped_lines)


def strip_all_whitespace(text):
  pattern = re.compile(r'\s+')
  return re.sub(pattern, '', text)


def extract_certificates_from_pem(pem_bytes):
  certificates_der = []

  regex = re.compile(
      r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', re.DOTALL)

  for match in regex.finditer(pem_bytes):
    cert_der = base64.b64decode(strip_all_whitespace(match.group(1)))
    certificates_der.append(cert_der)

  return certificates_der


def extract_certificates(source_bytes):
  if "BEGIN CERTIFICATE" in source_bytes:
    return extract_certificates_from_pem(source_bytes)

  # Otherwise assume it is the DER for a single certificate
  return [source_bytes]


def pretty_print_certificate(command, certificate_der):
  try:
    p = subprocess.Popen(command,
                         stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
  except OSError, e:
    if e.errno == errno.ENOENT:
      sys.stderr.write("Failed to execute %s\n" % command[0])
      return ""
    raise

  result = p.communicate(certificate_der)

  if p.returncode == 0:
    return result[0]

  # Otherwise failed.
  sys.stderr.write("Failed: %s: %s\n" % (" ".join(command), result[1]))
  return ""


def openssl_text_pretty_printer(certificate_der, unused_certificate_number):
  return pretty_print_certificate(["openssl", "x509", "-text", "-inform",
                                   "DER", "-noout"], certificate_der)


def pem_pretty_printer(certificate_der, unused_certificate_number):
  return pretty_print_certificate(["openssl", "x509", "-inform", "DER",
                                   "-outform", "PEM"], certificate_der)


def der2ascii_pretty_printer(certificate_der, unused_certificate_number):
  return pretty_print_certificate(["der2ascii"], certificate_der)


def header_pretty_printer(unused_certificate_der, certificate_number):
  return """===========================================
Certificate%d
===========================================""" % certificate_number


# This is actually just used as a magic value, since pretty_print_certificates
# special-cases der output.
def der_printer():
  raise RuntimeError


def pretty_print_certificates(certificates_der, pretty_printers):
  # Need to special-case DER output to avoid adding any newlines, and to
  # only allow a single certificate to be output.
  if pretty_printers == [der_printer]:
    if len(certificates_der) > 1:
      sys.stderr.write("DER output only supports a single certificate, "
                       "ignoring %d remaining certs\n" % (
                           len(certificates_der) - 1))
    return certificates_der[0]

  result = ""
  for i in range(len(certificates_der)):
    certificate_der = certificates_der[i]
    pretty = []
    for pretty_printer in pretty_printers:
      pretty_printed = pretty_printer(certificate_der, i)
      if pretty_printed:
        pretty.append(pretty_printed)
    result += "\n".join(pretty) + "\n"
  return result


def parse_outputs(outputs):
  pretty_printers = []
  output_map = {"der2ascii": der2ascii_pretty_printer,
                "openssl_text": openssl_text_pretty_printer,
                "pem": pem_pretty_printer,
                "header": header_pretty_printer,
                "der": der_printer}
  for output_name in outputs.split(','):
    if output_name not in output_map:
      sys.stderr.write("Invalid output type: %s\n" % output_name)
      return []
    pretty_printers.append(output_map[output_name])
  if der_printer in pretty_printers and len(pretty_printers) > 1:
      sys.stderr.write("Output type der must be used alone.\n")
      return []
  return pretty_printers


def main():
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawTextHelpFormatter)

  # TODO(mattm): support der2ascii as an input format too.
  parser.add_argument('sources', metavar='SOURCE', nargs='*',
                      help='''Each SOURCE can be one of:
  (1) A server name such as www.google.com.
  (2) A PEM [*] file containing one or more CERTIFICATE blocks
  (3) A binary file containing DER-encoded certificate

When multiple SOURCEs are listed, all certificates in them
are concatenated. If no SOURCE is given then data will be
read from stdin.

[*] Parsing of PEM files is relaxed - leading indentation
whitespace will be stripped (needed for copy-pasting data
from NetLogs).''')

  parser.add_argument(
      '--output', dest='outputs', action='store',
      default="header,der2ascii,openssl_text,pem",
      help='output formats to use. Default: %(default)s')

  args = parser.parse_args()

  sources_bytes = read_sources_from_commandline(args.sources)

  pretty_printers = parse_outputs(args.outputs)
  if not pretty_printers:
    sys.stderr.write('No pretty printers selected.\n')
    sys.exit(1)

  certificates_der = []
  for source_bytes in sources_bytes:
    certificates_der.extend(extract_certificates(source_bytes))

  sys.stdout.write(pretty_print_certificates(certificates_der, pretty_printers))


if __name__ == "__main__":
  main()
