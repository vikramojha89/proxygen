/*
 *  Copyright (c) 2017, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/session/HTTPDownstreamSession.h>

#include <proxygen/lib/http/session/HTTPSessionController.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <proxygen/lib/http/codec/HTTPCodecFactory.h>

namespace proxygen {

HTTPDownstreamSession::~HTTPDownstreamSession() {
}

void
HTTPDownstreamSession::startNow() {
  // Create virtual nodes should happen before startNow since ingress may come
  // before we can finish startNow. Since maxLevel = 0, this is a no-op unless
  // SPDY is used. And no frame will be sent to peer, so ignore returned value.
  codec_->addPriorityNodes(txnEgressQueue_, writeBuf_, 0);
  HTTPSession::startNow();
}

void
HTTPDownstreamSession::setupOnHeadersComplete(HTTPTransaction* txn,
                                              HTTPMessage* msg) {
  CHECK(!txn->getHandler());

  // We need to find a Handler to process the transaction.
  // Note: The handler is responsible for freeing itself
  // when it has finished processing the transaction.  The
  // transaction is responsible for freeing itself when both the
  // ingress and egress messages have completed (or failed).
  HTTPTransaction::Handler* handler = nullptr;

  // In the general case, delegate to the handler factory to generate
  // a handler for the transaction.
  handler = controller_->getRequestHandler(*txn, msg);
  CHECK(handler);

  DestructorGuard dg(this);
  auto txnID = txn->getID();
  txn->setHandler(handler);
  setNewTransactionPauseState(txnID);
}

HTTPTransaction::Handler*
HTTPDownstreamSession::getParseErrorHandler(HTTPTransaction* txn,
                                            const HTTPException& error) {
  // we encounter an error before we finish reading the ingress headers.
  return controller_->getParseErrorHandler(txn, error, localAddr_);
}

HTTPTransaction::Handler*
HTTPDownstreamSession::getTransactionTimeoutHandler(
  HTTPTransaction* txn) {
  return controller_->getTransactionTimeoutHandler(txn, localAddr_);
}

void
HTTPDownstreamSession::onHeadersSent(const HTTPMessage& headers,
                                     bool codecWasReusable) {
  if (!codec_->isReusable()) {
    // If the codec turned unreusable, some thing wrong must have happened.
    // Basically, the proxy decides the connection is not reusable.
    // e.g, an error message is being sent with Connection: close
    if (codecWasReusable) {
      uint32_t statusCode = headers.getStatusCode();
      if (statusCode >= 500) {
        setCloseReason(ConnectionCloseReason::REMOTE_ERROR);
      } else {
        if (statusCode >= 400) {
          setCloseReason(ConnectionCloseReason::ERR_RESP);
        } else {
          // should not be here
          setCloseReason(ConnectionCloseReason::UNKNOWN);
        }
      }
    } else {
      // shouldn't happen... this case is detected by REQ_NOTREUSABLE
      setCloseReason(ConnectionCloseReason::UNKNOWN);
    }
  }
}

bool HTTPDownstreamSession::allTransactionsStarted() const {
  for (const auto& txn: transactions_) {
    if (txn.second.isPushed() && !txn.second.isEgressStarted()) {
      return false;
    }
  }
  return true;
}

bool
HTTPDownstreamSession::onNativeProtocolUpgrade(
  HTTPCodec::StreamID streamID, CodecProtocol protocol,
  const std::string& protocolString,
  HTTPMessage& msg) {
  VLOG(4) << *this << " onNativeProtocolUpgrade streamID=" << streamID <<
    " protocol=" << protocolString;
  auto txn = findTransaction(streamID);
  CHECK(txn);
  if (txn->canSendHeaders()) {
    // Create the new Codec
    auto codec = HTTPCodecFactory::getCodec(protocol,
                                            TransportDirection::DOWNSTREAM);
    CHECK(codec);
    if (!codec->onIngressUpgradeMessage(msg)) {
      VLOG(4) << *this << " codec rejected upgrade";
      return false;
    }

    // Send a 101 Switching Protocols message while we still have HTTP codec
    // Note: it's possible that the client timed out waiting for a
    // 100-continue and ended up here first.  In this case the 100 may go
    // out in the new protocol
    HTTPMessage switchingProtos;
    switchingProtos.setHTTPVersion(1, 1);
    switchingProtos.setStatusCode(101);
    switchingProtos.setStatusMessage("Switching Protocols");
    switchingProtos.getHeaders().set(HTTP_HEADER_UPGRADE, protocolString);
    switchingProtos.getHeaders().set(HTTP_HEADER_CONNECTION, "Upgrade");
    txn->sendHeaders(switchingProtos);
    // no sendEOM for 1xx

    // This will actually switch the protocol
    bool ret = HTTPSession::onNativeProtocolUpgradeImpl(
      streamID, std::move(codec), protocolString);
    if (ret) {
      codec_->addPriorityNodes(txnEgressQueue_, writeBuf_, 0);
    }
    return ret;
  } else {
    VLOG(4) << *this << " plaintext upgrade failed due to early response";
    return false;
  }
}

} // proxygen
