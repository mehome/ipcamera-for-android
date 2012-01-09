/*
 * libjingle
 * Copyright 2011, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "talk/app/webrtc/webrtcsession.h"

#include "talk/app/webrtc/mediastream.h"
#include "talk/app/webrtc/peerconnection.h"
#include "talk/app/webrtc/peerconnectionsignaling.h"
#include "talk/base/helpers.h"
#include "talk/base/logging.h"
#include "talk/session/phone/channel.h"
#include "talk/session/phone/channelmanager.h"
#include "talk/session/phone/mediasession.h"
#include "talk/session/phone/videocapturer.h"

using cricket::MediaContentDescription;

namespace webrtc {

enum {
  MSG_CANDIDATE_TIMEOUT = 101,
};

// We allow 30 seconds to establish a connection, otherwise it's an error.
static const int kCallSetupTimeout = 30 * 1000;
// Session will accept one candidate per transport channel and dropping other
// candidates generated for that channel. During the session initialization
// one cricket::VoiceChannel and one cricket::VideoChannel will be created with
// rtcp enabled.
static const size_t kAllowedCandidates = 4;
// TODO - These are magic string used by cricket::VideoChannel.
// These should be moved to a common place.
static const std::string kRtpVideoChannelStr = "video_rtp";
static const std::string kRtcpVideoChannelStr = "video_rtcp";

WebRtcSession::WebRtcSession(cricket::ChannelManager* channel_manager,
                             talk_base::Thread* signaling_thread,
                             talk_base::Thread* worker_thread,
                             cricket::PortAllocator* port_allocator)
    : cricket::BaseSession(signaling_thread, worker_thread, port_allocator,
          talk_base::ToString(talk_base::CreateRandomId()),
          cricket::NS_JINGLE_RTP, true),
      channel_manager_(channel_manager),
      observer_(NULL),
      session_desc_factory_(channel_manager) {
}

WebRtcSession::~WebRtcSession() {
  Terminate();
}

bool WebRtcSession::Initialize() {
  // By default SRTP-SDES is enabled in WebRtc.
  set_secure_policy(cricket::SEC_REQUIRED);
  return CreateChannels();
}

void WebRtcSession::Terminate() {
  if (voice_channel_.get()) {
    channel_manager_->DestroyVoiceChannel(voice_channel_.release());
  }
  if (video_channel_.get()) {
    channel_manager_->DestroyVideoChannel(video_channel_.release());
  }
}

void WebRtcSession::set_secure_policy(
    cricket::SecureMediaPolicy secure_policy) {
  session_desc_factory_.set_secure(secure_policy);
}

bool WebRtcSession::CreateChannels() {
  voice_channel_.reset(channel_manager_->CreateVoiceChannel(
      this, cricket::CN_AUDIO, true));
  if (!voice_channel_.get()) {
    LOG(LS_ERROR) << "Failed to create voice channel";
    return false;
  }

  video_channel_.reset(channel_manager_->CreateVideoChannel(
      this, cricket::CN_VIDEO, true, voice_channel_.get()));
  if (!video_channel_.get()) {
    LOG(LS_ERROR) << "Failed to create video channel";
    return false;
  }

  // TransportProxies and TransportChannels will be created when
  // CreateVoiceChannel and CreateVideoChannel are called.
  // Try connecting all transport channels. This is necessary to generate
  // ICE candidates.
  SpeculativelyConnectAllTransportChannels();
  return true;
}

void WebRtcSession::SetRemoteCandidates(
    const cricket::Candidates& candidates) {
  // First partition the candidates for the proxies. During creation of channels
  // we created CN_AUDIO (audio) and CN_VIDEO (video) proxies.
  cricket::Candidates audio_candidates;
  cricket::Candidates video_candidates;
  for (cricket::Candidates::const_iterator citer = candidates.begin();
       citer != candidates.end(); ++citer) {
    if (((*citer).name().compare(kRtpVideoChannelStr) == 0) ||
        ((*citer).name().compare(kRtcpVideoChannelStr)) == 0) {
      // Candidate names for video rtp and rtcp channel
      video_candidates.push_back(*citer);
    } else {
      // Candidates for audio rtp and rtcp channel
      // Channel name will be "rtp" and "rtcp"
      audio_candidates.push_back(*citer);
    }
  }

  if (!audio_candidates.empty()) {
    cricket::TransportProxy* audio_proxy = GetTransportProxy(cricket::CN_AUDIO);
    if (audio_proxy) {
      // CompleteNegotiation will set actual impl's in Proxy.
      if (!audio_proxy->negotiated())
        audio_proxy->CompleteNegotiation();
      // TODO - Add a interface to TransportProxy to accept
      // remote candidate list.
      audio_proxy->impl()->OnRemoteCandidates(audio_candidates);
    } else {
      LOG(LS_INFO) << "No audio TransportProxy exists";
    }
  }

  if (!video_candidates.empty()) {
    cricket::TransportProxy* video_proxy = GetTransportProxy(cricket::CN_VIDEO);
    if (video_proxy) {
      // CompleteNegotiation will set actual impl's in Proxy.
      if (!video_proxy->negotiated())
        video_proxy->CompleteNegotiation();
      // TODO - Add a interface to TransportProxy to accept
      // remote candidate list.
      video_proxy->impl()->OnRemoteCandidates(video_candidates);
    } else {
      LOG(LS_INFO) << "No video TransportProxy exists";
    }
  }
}

void WebRtcSession::OnTransportRequestSignaling(
    cricket::Transport* transport) {
  ASSERT(signaling_thread()->IsCurrent());
  transport->OnSignalingReady();
}

void WebRtcSession::OnTransportConnecting(cricket::Transport* transport) {
  ASSERT(signaling_thread()->IsCurrent());
  // start monitoring for the write state of the transport.
  OnTransportWritable(transport);
}

void WebRtcSession::OnTransportWritable(cricket::Transport* transport) {
  ASSERT(signaling_thread()->IsCurrent());
  // If the transport is not in writable state, start a timer to monitor
  // the state. If the transport doesn't become writable state in 30 seconds
  // then we are assuming call can't be continued.
  signaling_thread()->Clear(this, MSG_CANDIDATE_TIMEOUT);
  if (transport->HasChannels() && !transport->writable()) {
    signaling_thread()->PostDelayed(
        kCallSetupTimeout, this, MSG_CANDIDATE_TIMEOUT);
  }
}

void WebRtcSession::OnTransportCandidatesReady(
    cricket::Transport* transport, const cricket::Candidates& candidates) {
  ASSERT(signaling_thread()->IsCurrent());
  // Drop additional candidates for the same channel;
  // local_candidates_ will have one candidate per channel.
  if (local_candidates_.size() == kAllowedCandidates)
    return;
  InsertTransportCandidates(candidates);
  if (local_candidates_.size() == kAllowedCandidates && observer_) {
    observer_->OnCandidatesReady(local_candidates_);
  }
}

void WebRtcSession::OnTransportChannelGone(cricket::Transport* transport,
                                           const std::string& name) {
  ASSERT(signaling_thread()->IsCurrent());
}

void WebRtcSession::OnMessage(talk_base::Message* msg) {
  switch (msg->message_id) {
    case MSG_CANDIDATE_TIMEOUT:
      LOG(LS_ERROR) << "Transport is not in writable state.";
      SignalError();
      break;
    default:
      break;
  }
}

void WebRtcSession::InsertTransportCandidates(
    const cricket::Candidates& candidates) {
  for (cricket::Candidates::const_iterator citer = candidates.begin();
       citer != candidates.end(); ++citer) {
    // Find candidates by name, if this channel name not exists in local
    // candidate list, store it.
    if (!CheckCandidate((*citer).name())) {
      local_candidates_.push_back(*citer);
    }
  }
}

// Check transport candidate already available for transport channel as only
// one cricket::Candidate allower per channel.
bool WebRtcSession::CheckCandidate(const std::string& name) {
  bool ret = false;
  for (cricket::Candidates::iterator iter = local_candidates_.begin();
       iter != local_candidates_.end(); ++iter) {
    if ((*iter).name().compare(name) == 0) {
      ret = true;
      break;
    }
  }
  return ret;
}

bool WebRtcSession::SetCaptureDevice(const std::string& name,
                                     cricket::VideoCapturer* camera) {
  // should be called from a signaling thread
  ASSERT(signaling_thread()->IsCurrent());

  // TODO: Refactor this when there is support for multiple cameras.
  const uint32 dummy_ssrc = 0;
  if (!channel_manager_->SetVideoCapturer(camera, dummy_ssrc)) {
    LOG(LS_ERROR) << "Failed to set capture device.";
    return false;
  }

  // Start the capture
  cricket::CaptureResult ret = channel_manager_->SetVideoCapture(true);
  if (ret != cricket::CR_SUCCESS && ret != cricket::CR_PENDING) {
    LOG(LS_ERROR) << "Failed to start the capture device.";
    return false;
  }

  return true;
}

void WebRtcSession::SetLocalRenderer(const std::string& name,
                                     cricket::VideoRenderer* renderer) {
  ASSERT(signaling_thread()->IsCurrent());
  // TODO: Fix SetLocalRenderer.
  //video_channel_->SetLocalRenderer(0, renderer);
}

void WebRtcSession::SetRemoteRenderer(const std::string& name,
                                      cricket::VideoRenderer* renderer) {
  ASSERT(signaling_thread()->IsCurrent());

  // TODO: Only the ssrc = 0 is supported at the moment.
  // Only one channel.
  video_channel_->SetRenderer(0, renderer);
}

const cricket::SessionDescription* WebRtcSession::ProvideOffer(
    const cricket::MediaSessionOptions& options) {
  if (!options.has_video) {
    LOG(LS_WARNING) << "To receive video, has_video flag must be set to true";
    return NULL;
  }

  cricket::SessionDescription* offer(
      session_desc_factory_.CreateOffer(options, local_description()));
  set_local_description(offer);
  return offer;
}

const cricket::SessionDescription* WebRtcSession::SetRemoteSessionDescription(
    const cricket::SessionDescription* remote_offer,
    const std::vector<cricket::Candidate>& remote_candidates) {
  set_remote_description(
      const_cast<cricket::SessionDescription*>(remote_offer));
  SetRemoteCandidates(remote_candidates);
  return remote_offer;
}

const cricket::SessionDescription* WebRtcSession::ProvideAnswer(
    const cricket::MediaSessionOptions& options) {
  cricket::SessionDescription* answer(
      session_desc_factory_.CreateAnswer(remote_description(), options,
                                         local_description()));
  set_local_description(answer);
  return answer;
}

void WebRtcSession::NegotiationDone() {
  // SetState of session is called after session receives both local and
  // remote descriptions. State transition will happen only when session
  // is in INIT state.
  if (state() == STATE_INIT) {
    SetState(STATE_SENTINITIATE);
    SetState(STATE_RECEIVEDACCEPT);

    // Enabling voice and video channel.
    voice_channel_->Enable(true);
    video_channel_->Enable(true);
  }

  const cricket::ContentInfo* audio_info =
      cricket::GetFirstAudioContent(local_description());
  if (audio_info) {
    const cricket::MediaContentDescription* audio_content =
        static_cast<const cricket::MediaContentDescription*>(
            audio_info->description);
    // Since channels are currently not supporting multiple send streams,
    // we can remove stream from a session by muting it.
    // TODO - Change needed when multiple send streams support
    // is available.
    voice_channel_->Mute(audio_content->streams().size() == 0);
  }

  const cricket::ContentInfo* video_info =
      cricket::GetFirstVideoContent(local_description());
  if (video_info) {
    const cricket::MediaContentDescription* video_content =
        static_cast<const cricket::MediaContentDescription*>(
            video_info->description);
    // Since channels are currently not supporting multiple send streams,
    // we can remove stream from a session by muting it.
    // TODO - Change needed when multiple send streams support
    // is available.
    video_channel_->Mute(video_content->streams().size() == 0);
  }
}

}  // namespace webrtc
