// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_TOOLS_QUIC_QUIC_IN_MEMORY_CACHE_H_
#define NET_TOOLS_QUIC_QUIC_IN_MEMORY_CACHE_H_

#include <string>

#include "base/containers/hash_tables.h"
#include "base/memory/singleton.h"
#include "base/strings/string_piece.h"
#include "net/spdy/spdy_framer.h"

namespace base {

template <typename Type> struct DefaultSingletonTraits;

}  // namespace base

namespace net {
namespace tools {

namespace test {
class QuicInMemoryCachePeer;
}  // namespace test

class QuicServer;

// In-memory cache for HTTP responses.
// Reads from disk cache generated by:
// `wget -p --save_headers <url>`
class QuicInMemoryCache {
 public:
  enum SpecialResponseType {
    REGULAR_RESPONSE,  // Send the headers and body like a server should.
    CLOSE_CONNECTION,  // Close the connection (sending the close packet).
    IGNORE_REQUEST,  // Do nothing, expect the client to time out.
  };

  // Container for response header/body pairs.
  class Response {
   public:
    Response();
    ~Response();

    SpecialResponseType response_type() const { return response_type_; }
    const SpdyHeaderBlock& headers() const { return headers_; }
    const base::StringPiece body() const { return base::StringPiece(body_); }

    void set_response_type(SpecialResponseType response_type) {
      response_type_ = response_type;
    }
    void set_headers(const SpdyHeaderBlock& headers) {
      headers_ = headers;
    }
    void set_body(base::StringPiece body) {
      body.CopyToString(&body_);
    }

   private:
    SpecialResponseType response_type_;
    SpdyHeaderBlock headers_;
    std::string body_;

    DISALLOW_COPY_AND_ASSIGN(Response);
  };

  // Returns the singleton instance of the cache.
  static QuicInMemoryCache* GetInstance();

  // Retrieve a response from this cache for a given host and path..
  // If no appropriate response exists, nullptr is returned.
  const Response* GetResponse(base::StringPiece host,
                              base::StringPiece path) const;

  // Adds a simple response to the cache.  The response headers will
  // only contain the "content-length" header with the length of |body|.
  void AddSimpleResponse(base::StringPiece host,
                         base::StringPiece path,
                         int response_code,
                         base::StringPiece body);

  // Add a response to the cache.
  void AddResponse(base::StringPiece host,
                   base::StringPiece path,
                   const SpdyHeaderBlock& response_headers,
                   base::StringPiece response_body);

  // Simulate a special behavior at a particular path.
  void AddSpecialResponse(base::StringPiece host,
                          base::StringPiece path,
                          SpecialResponseType response_type);

  // Sets a default response in case of cache misses.  Takes ownership of
  // 'response'.
  void AddDefaultResponse(Response* response);

  // |cache_cirectory| can be generated using `wget -p --save-headers <url>`.
  void InitializeFromDirectory(const std::string& cache_directory);

 private:
  typedef base::hash_map<std::string, Response*> ResponseMap;

  friend struct base::DefaultSingletonTraits<QuicInMemoryCache>;
  friend class test::QuicInMemoryCachePeer;

  QuicInMemoryCache();
  ~QuicInMemoryCache();

  void ResetForTests();

  void AddResponseImpl(base::StringPiece host,
                       base::StringPiece path,
                       SpecialResponseType response_type,
                       const SpdyHeaderBlock& response_headers,
                       base::StringPiece response_body);

  std::string GetKey(base::StringPiece host, base::StringPiece path) const;

  // Cached responses.
  ResponseMap responses_;

  // The default response for cache misses, if set.
  scoped_ptr<Response> default_response_;

  DISALLOW_COPY_AND_ASSIGN(QuicInMemoryCache);
};

}  // namespace tools
}  // namespace net

#endif  // NET_TOOLS_QUIC_QUIC_IN_MEMORY_CACHE_H_
