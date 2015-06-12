// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A QuicSession, which demuxes a single connection to individual streams.

#ifndef NET_QUIC_QUIC_SESSION_H_
#define NET_QUIC_QUIC_SESSION_H_

#include <map>
#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/hash_tables.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/string_piece.h"
#include "net/base/ip_endpoint.h"
#include "net/quic/quic_connection.h"
#include "net/quic/quic_crypto_stream.h"
#include "net/quic/quic_data_stream.h"
#include "net/quic/quic_headers_stream.h"
#include "net/quic/quic_packet_creator.h"
#include "net/quic/quic_protocol.h"
#include "net/quic/quic_write_blocked_list.h"
#include "net/quic/reliable_quic_stream.h"

namespace net {

class QuicCryptoStream;
class QuicFlowController;
class ReliableQuicStream;
class VisitorShim;

namespace test {
class QuicSessionPeer;
}  // namespace test

class NET_EXPORT_PRIVATE QuicSession : public QuicConnectionVisitorInterface {
 public:
  // CryptoHandshakeEvent enumerates the events generated by a QuicCryptoStream.
  enum CryptoHandshakeEvent {
    // ENCRYPTION_FIRST_ESTABLISHED indicates that a full client hello has been
    // sent by a client and that subsequent packets will be encrypted. (Client
    // only.)
    ENCRYPTION_FIRST_ESTABLISHED,
    // ENCRYPTION_REESTABLISHED indicates that a client hello was rejected by
    // the server and thus the encryption key has been updated. Therefore the
    // connection should resend any packets that were sent under
    // ENCRYPTION_INITIAL. (Client only.)
    ENCRYPTION_REESTABLISHED,
    // HANDSHAKE_CONFIRMED, in a client, indicates the the server has accepted
    // our handshake. In a server it indicates that a full, valid client hello
    // has been received. (Client and server.)
    HANDSHAKE_CONFIRMED,
  };

  QuicSession(QuicConnection* connection, const QuicConfig& config);
  virtual void Initialize();

  ~QuicSession() override;

  // QuicConnectionVisitorInterface methods:
  void OnStreamFrames(const std::vector<QuicStreamFrame>& frames) override;
  void OnRstStream(const QuicRstStreamFrame& frame) override;
  void OnGoAway(const QuicGoAwayFrame& frame) override;
  void OnWindowUpdateFrames(
      const std::vector<QuicWindowUpdateFrame>& frames) override;
  void OnBlockedFrames(const std::vector<QuicBlockedFrame>& frames) override;
  void OnConnectionClosed(QuicErrorCode error, bool from_peer) override;
  void OnWriteBlocked() override {}
  void OnSuccessfulVersionNegotiation(const QuicVersion& version) override;
  void OnCanWrite() override;
  void OnCongestionWindowChange(QuicTime now) override {}
  bool WillingAndAbleToWrite() const override;
  bool HasPendingHandshake() const override;
  bool HasOpenDataStreams() const override;

  // Called by the headers stream when headers have been received for a stream.
  virtual void OnStreamHeaders(QuicStreamId stream_id,
                               base::StringPiece headers_data);
  // Called by the headers stream when headers with a priority have been
  // received for this stream.  This method will only be called for server
  // streams.
  virtual void OnStreamHeadersPriority(QuicStreamId stream_id,
                                       QuicPriority priority);
  // Called by the headers stream when headers have been completely received
  // for a stream.  |fin| will be true if the fin flag was set in the headers
  // frame.
  virtual void OnStreamHeadersComplete(QuicStreamId stream_id,
                                       bool fin,
                                       size_t frame_len);

  // Called by streams when they want to write data to the peer.
  // Returns a pair with the number of bytes consumed from data, and a boolean
  // indicating if the fin bit was consumed.  This does not indicate the data
  // has been sent on the wire: it may have been turned into a packet and queued
  // if the socket was unexpectedly blocked.  |fec_protection| indicates if
  // data is to be FEC protected. Note that data that is sent immediately
  // following MUST_FEC_PROTECT data may get protected by falling within the
  // same FEC group.
  // If provided, |ack_notifier_delegate| will be registered to be notified when
  // we have seen ACKs for all packets resulting from this call.
  virtual QuicConsumedData WritevData(
      QuicStreamId id,
      const IOVector& data,
      QuicStreamOffset offset,
      bool fin,
      FecProtection fec_protection,
      QuicAckNotifier::DelegateInterface* ack_notifier_delegate);

  // Writes |headers| for the stream |id| to the dedicated headers stream.
  // If |fin| is true, then no more data will be sent for the stream |id|.
  // If provided, |ack_notifier_delegate| will be registered to be notified when
  // we have seen ACKs for all packets resulting from this call.
  size_t WriteHeaders(
      QuicStreamId id,
      const SpdyHeaderBlock& headers,
      bool fin,
      QuicPriority priority,
      QuicAckNotifier::DelegateInterface* ack_notifier_delegate);

  // Called by streams when they want to close the stream in both directions.
  virtual void SendRstStream(QuicStreamId id,
                             QuicRstStreamErrorCode error,
                             QuicStreamOffset bytes_written);

  // Called when the session wants to go away and not accept any new streams.
  void SendGoAway(QuicErrorCode error_code, const std::string& reason);

  // Removes the stream associated with 'stream_id' from the active stream map.
  virtual void CloseStream(QuicStreamId stream_id);

  // Returns true if outgoing packets will be encrypted, even if the server
  // hasn't confirmed the handshake yet.
  virtual bool IsEncryptionEstablished();

  // For a client, returns true if the server has confirmed our handshake. For
  // a server, returns true if a full, valid client hello has been received.
  virtual bool IsCryptoHandshakeConfirmed();

  // Called by the QuicCryptoStream when a new QuicConfig has been negotiated.
  virtual void OnConfigNegotiated();

  // Called by the QuicCryptoStream when the handshake enters a new state.
  //
  // Clients will call this function in the order:
  //   ENCRYPTION_FIRST_ESTABLISHED
  //   zero or more ENCRYPTION_REESTABLISHED
  //   HANDSHAKE_CONFIRMED
  //
  // Servers will simply call it once with HANDSHAKE_CONFIRMED.
  virtual void OnCryptoHandshakeEvent(CryptoHandshakeEvent event);

  // Called by the QuicCryptoStream when a handshake message is sent.
  virtual void OnCryptoHandshakeMessageSent(
      const CryptoHandshakeMessage& message);

  // Called by the QuicCryptoStream when a handshake message is received.
  virtual void OnCryptoHandshakeMessageReceived(
      const CryptoHandshakeMessage& message);

  // Returns mutable config for this session. Returned config is owned
  // by QuicSession.
  QuicConfig* config();

  // Returns true if the stream existed previously and has been closed.
  // Returns false if the stream is still active or if the stream has
  // not yet been created.
  bool IsClosedStream(QuicStreamId id);

  QuicConnection* connection() { return connection_.get(); }
  const QuicConnection* connection() const { return connection_.get(); }
  size_t num_active_requests() const { return stream_map_.size(); }
  const IPEndPoint& peer_address() const {
    return connection_->peer_address();
  }
  QuicConnectionId connection_id() const {
    return connection_->connection_id();
  }

  // Returns the number of currently open streams, including those which have
  // been implicitly created, but excluding the reserved headers and crypto
  // streams.
  virtual size_t GetNumOpenStreams() const;

  void MarkWriteBlocked(QuicStreamId id, QuicPriority priority);

  // Returns true if the session has data to be sent, either queued in the
  // connection, or in a write-blocked stream.
  bool HasDataToWrite() const;

  bool goaway_received() const { return goaway_received_; }

  bool goaway_sent() const { return goaway_sent_; }

  QuicErrorCode error() const { return error_; }

  Perspective perspective() const { return connection_->perspective(); }

  QuicFlowController* flow_controller() { return &flow_controller_; }

  // Returns true if connection is flow controller blocked.
  bool IsConnectionFlowControlBlocked() const;

  // Returns true if any stream is flow controller blocked.
  bool IsStreamFlowControlBlocked();

  // Returns true if this is a secure QUIC session.
  bool IsSecure() const { return connection()->is_secure(); }

  size_t get_max_open_streams() const { return max_open_streams_; }

  // Used in Chrome.
  const QuicHeadersStream* headers_stream() { return headers_stream_.get(); }

 protected:
  typedef base::hash_map<QuicStreamId, QuicDataStream*> DataStreamMap;

  // Creates a new stream, owned by the caller, to handle a peer-initiated
  // stream.  Returns nullptr and does error handling if the stream can not be
  // created.
  virtual QuicDataStream* CreateIncomingDataStream(QuicStreamId id) = 0;

  // Create a new stream, owned by the caller, to handle a locally-initiated
  // stream.  Returns nullptr if max streams have already been opened.
  virtual QuicDataStream* CreateOutgoingDataStream() = 0;

  // Return the reserved crypto stream.
  virtual QuicCryptoStream* GetCryptoStream() = 0;

  // Adds 'stream' to the active stream map.
  virtual void ActivateStream(QuicDataStream* stream);

  // Returns the stream id for a new stream.
  QuicStreamId GetNextStreamId();

  QuicDataStream* GetIncomingDataStream(QuicStreamId stream_id);

  QuicDataStream* GetDataStream(const QuicStreamId stream_id);

  ReliableQuicStream* GetStream(const QuicStreamId stream_id);

  // This is called after every call other than OnConnectionClose from the
  // QuicConnectionVisitor to allow post-processing once the work has been done.
  // In this case, it deletes streams given that it's safe to do so (no other
  // operations are being done on the streams at this time)
  virtual void PostProcessAfterData();

  base::hash_map<QuicStreamId, QuicDataStream*>* streams() {
    return &stream_map_;
  }

  const base::hash_map<QuicStreamId, QuicDataStream*>* streams() const {
    return &stream_map_;
  }

  std::vector<QuicDataStream*>* closed_streams() { return &closed_streams_; }

  void set_max_open_streams(size_t max_open_streams);

  scoped_ptr<QuicHeadersStream> headers_stream_;

 private:
  friend class test::QuicSessionPeer;
  friend class VisitorShim;

  // Performs the work required to close |stream_id|.  If |locally_reset|
  // then the stream has been reset by this endpoint, not by the peer.
  void CloseStreamInner(QuicStreamId stream_id, bool locally_reset);

  // When a stream is closed locally, it may not yet know how many bytes the
  // peer sent on that stream.
  // When this data arrives (via stream frame w. FIN, or RST) this method
  // is called, and correctly updates the connection level flow controller.
  void UpdateFlowControlOnFinalReceivedByteOffset(
      QuicStreamId id, QuicStreamOffset final_byte_offset);

  // Called in OnConfigNegotiated when we receive a new stream level flow
  // control window in a negotiated config. Closes the connection if invalid.
  void OnNewStreamFlowControlWindow(QuicStreamOffset new_window);

  // Called in OnConfigNegotiated when we receive a new session level flow
  // control window in a negotiated config. Closes the connection if invalid.
  void OnNewSessionFlowControlWindow(QuicStreamOffset new_window);

  // Called in OnConfigNegotiated when auto-tuning is enabled for flow
  // control receive windows.
  void EnableAutoTuneReceiveWindow();

  // Keep track of highest received byte offset of locally closed streams, while
  // waiting for a definitive final highest offset from the peer.
  std::map<QuicStreamId, QuicStreamOffset>
      locally_closed_streams_highest_offset_;

  scoped_ptr<QuicConnection> connection_;

  // A shim to stand between the connection and the session, to handle stream
  // deletions.
  scoped_ptr<VisitorShim> visitor_shim_;

  std::vector<QuicDataStream*> closed_streams_;

  QuicConfig config_;

  // Returns the maximum number of streams this connection can open.
  size_t max_open_streams_;

  // Map from StreamId to pointers to streams that are owned by the caller.
  DataStreamMap stream_map_;
  QuicStreamId next_stream_id_;

  // Set of stream ids that have been "implicitly created" by receipt
  // of a stream id larger than the next expected stream id.
  base::hash_set<QuicStreamId> implicitly_created_streams_;

  // A list of streams which need to write more data.
  QuicWriteBlockedList write_blocked_streams_;

  QuicStreamId largest_peer_created_stream_id_;

  // The latched error with which the connection was closed.
  QuicErrorCode error_;

  // Used for session level flow control.
  QuicFlowController flow_controller_;

  // Whether a GoAway has been received.
  bool goaway_received_;
  // Whether a GoAway has been sent.
  bool goaway_sent_;

  // Indicate if there is pending data for the crypto stream.
  bool has_pending_handshake_;

  DISALLOW_COPY_AND_ASSIGN(QuicSession);
};

}  // namespace net

#endif  // NET_QUIC_QUIC_SESSION_H_
