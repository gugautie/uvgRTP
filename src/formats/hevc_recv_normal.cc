#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>

#include "debug.hh"
#include "queue.hh"
#include "receiver.hh"
#include "send.hh"

#define RTP_FRAME_MAX_DELAY          100
#define INVALID_SEQ           0x13371338

#define TS(x)  ((x)->header.timestamp)
#define SEQ(x) ((x)->header.seq)

enum FRAG_TYPES {
    FT_INVALID   = -2, /* invalid combination of S and E bits */
    FT_NOT_FRAG  = -1, /* frame doesn't contain HEVC fragment */
    FT_START     =  1, /* frame contains a fragment with S bit set */
    FT_MIDDLE    =  2, /* frame is fragment but not S or E fragment */
    FT_END       =  3, /* frame contains a fragment with E bit set */
};

struct hevc_fu_info {
    kvz_rtp::clock::hrc::hrc_t sframe_time; /* clock reading when the first fragment is received */
    uint32_t sframe_seq;                    /* sequence number of the frame with s-bit */
    uint32_t eframe_seq;                    /* sequence number of the frame with e-bit */
    size_t pkts_received;                   /* how many fragments have been received */
    size_t total_size;                      /* total size of all fragments */
};

static int __check_frame(kvz_rtp::frame::rtp_frame *frame)
{
    bool first_frag = frame->payload[2] & 0x80;
    bool last_frag  = frame->payload[2] & 0x40;

    if ((frame->payload[0] >> 1) != 49)
        return FT_NOT_FRAG;

    if (first_frag && last_frag)
        return FT_INVALID;

    if (first_frag)
        return FT_START;

    if (last_frag)
        return FT_END;

    return FT_MIDDLE;
}

rtp_error_t __hevc_receiver(kvz_rtp::receiver *receiver)
{
    LOG_INFO("frameReceiver starting listening...");

    int nread = 0;
    sockaddr_in sender_addr;
    rtp_error_t ret = RTP_OK;
    kvz_rtp::socket socket = receiver->get_socket();
    kvz_rtp::frame::rtp_frame *frame, *frames[0xffff + 1] = { 0 };

    uint8_t nal_header[2] = { 0 };
    std::map<uint32_t, hevc_fu_info> s_timers;
    std::map<uint32_t, size_t> dropped_frames;

    fd_set read_fds;
    struct timeval t_val;

    FD_ZERO(&read_fds);

    t_val.tv_sec  = 0;
    t_val.tv_usec = 1000;

    while (!receiver->active())
        ;

    while (receiver->active()) {
        FD_SET(socket.get_raw_socket(), &read_fds);
        int sret = ::select(socket.get_raw_socket() + 1, &read_fds, nullptr, nullptr, &t_val);

        if (sret < 0) {
#ifdef __linux__
            LOG_ERROR("select failed: %s!", strerror(errno));
#else
            win_get_last_error();
#endif
            return RTP_GENERIC_ERROR;
        }

        do {
#ifdef __linux__
            ret = socket.recvfrom(receiver->get_recv_buffer(), receiver->get_recv_buffer_len(), MSG_DONTWAIT, &sender_addr, &nread);

            if (ret != RTP_OK) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;

                LOG_ERROR("recvfrom failed! FrameReceiver cannot continue %s!", strerror(errno));
                return RTP_GENERIC_ERROR;
            }
#else
            ret = socket.recvfrom(receiver->get_recv_buffer(), receiver->get_recv_buffer_len(), 0, &sender_addr, &nread);

            if (ret != RTP_OK) {
                if (WSAGetLastError() == WSAEWOULDBLOCK)
                    break;

                LOG_ERROR("recvfrom failed! FrameReceiver cannot continue %s!", strerror(errno));
                return RTP_GENERIC_ERROR;
            }
#endif

            /* Frame might be actually invalid or it might be a ZRTP frame, just continue to next frame */
            if ((frame = receiver->validate_rtp_frame(receiver->get_recv_buffer(), nread)) == nullptr)
                continue;

            /* TODO: ??? */
            memcpy(&frame->src_addr, &sender_addr, sizeof(sockaddr_in));

            /* Update session related statistics
             * If this is a new peer, RTCP will take care of initializing necessary stuff
             *
             * Skip processing the packet if it was invalid. This is mostly likely caused
             * by an SSRC collision */
            /* TODO:  */
            /* if (receiver->update_receiver_stats(frame) != RTP_OK) */
            /*     continue; */

            /* How to the frame is handled is based what its type is. Generic and Opus frames
             * don't require any extra processing so they can be returned to the user as soon as
             * they're received without any buffering.
             *
             * Frames that can be fragmented (only HEVC for now) require some preprocessing before they're returned.
             *
             * When a frame with an S-bit set is received, the frame is saved to "frames" array and an NTP timestamp
             * is saved. All subsequent frag frames are also saved in the "frames" array and they may arrive in
             * any order they want because they're saved in the array using the sequence number.
             *
             * Each time a new frame is received, the initial timestamp is compared with current time to see
             * how much time this frame still has left until it's discarded. Each frame is given N milliseconds
             * and if all its fragments are not received within that time window, the frame is considered invalid
             * and all fragments are discarded.
             *
             * When the last fragment is received (ie. the frame with E-bit set) AND if all previous fragments
             * very received too, frame_receiver() will call post-processing function to merge all the fragments
             * into one complete frame.
             *
             * If all previous fragments have not been received (ie. some frame is late), the code will wait
             * until the time windows closes. When that happens, the array is inspected once more to see if
             * all fragments were received and if so, the fragments are merged and returned to user.
             *
             * If some fragments were dropped, the whole HEVC frame is discarded
             *
             * Due to the nature of UDP, it's possible that during a fragment reception,
             * a stray RTP packet from earlier fragment might be received and that might corrupt
             * the reception process. To mitigate this, the sequence number and RTP timestamp of each
             * incoming packet is matched and checked against our own clock to get sense whether this packet valid
             *
             * Invalid packets (such as very late packets) are discarded automatically without further processing */
            const size_t HEVC_HDR_SIZE =
                kvz_rtp::frame::HEADER_SIZE_HEVC_NAL +
                kvz_rtp::frame::HEADER_SIZE_HEVC_FU;

            int type = __check_frame(frame);

            if (type == FT_NOT_FRAG) {
                receiver->return_frame(frame);
                continue;
            }

            if (type == FT_INVALID) {
                LOG_WARN("invalid frame received!");
                (void)kvz_rtp::frame::dealloc_frame(frame);
                continue;
            }

            /* TODO: this is ugly */
            bool duplicate = true;

            /* Save the received frame to "frames" array where frames are indexed using
             * their sequence number. This way when all fragments of a frame are received,
             * we can loop through the range sframe_seq - eframe_seq and merge all fragments */
            if (frames[SEQ(frame)] == nullptr) {
                frames[SEQ(frame)] = frame;
                duplicate          = false;

            } else if (TS(frame) != frames[SEQ(frame)]->header.timestamp) {
                (void)kvz_rtp::frame::dealloc_frame(frames[SEQ(frame)]);
                frames[SEQ(frame)] = frame;
                duplicate          = false;
            } else {
                LOG_INFO("valid duplicate packet, investigate!");
                (void)kvz_rtp::frame::dealloc_frame(frame);
                continue;
            }

            /* If frames[SEQ(frame)] is not nullptr, there's actually two possibilites:
             *    - The RTP frame is actually duplicate (quite rare)
             *    - Previous HEVC was never returned to user and the RTP frames are still in the array
             *
             * Because this normal receiver does not have a notion of active frames (because all frames are active)
             * it is very much possible that an RTP frame is dropped but the receiver never notices is because it's
             * constructing multiple frames at once.
             *
             * This actually results in quite unintended behaviour where no data is returned to user.
             * This problem can be averted by checking the timestamp of frames[SEQ(frame)].
             *
             * If the entry is more than RTP_FRAME_MAX_DELAY milliseconds old, it can be released and
             * the entry is replaced with this current RTP frame */
            if (duplicate) {
                uint64_t diff = kvz_rtp::clock::hrc::diff_now(
                    s_timers[frames[SEQ(frame)]->header.timestamp].sframe_time);

                if (diff >= RTP_FRAME_MAX_DELAY) {
                    LOG_ERROR("duplicate frame must be dropped");
                    kvz_rtp::frame::dealloc_frame(frames[SEQ(frame)]);
                    frames[SEQ(frame)] = frame;
                    duplicate = false;
                } else {
                    fprintf(stderr, "not old enough\n");
                }
            }

            /* If this is the first packet received with this timestamp, create new entry
             * to s_timers and save current time.
             *
             * This timestamp is used to keep track of how long we've been receiving chunks
             * and if the time exceeds RTP_FRAME_MAX_DELAY, we drop the frame */
            if (s_timers.find(TS(frame)) == s_timers.end()) {
                /* UDP being unreliable, we can't know for sure in what order the packets are arriving.
                 * Especially on linux where the fragment frames are batched and sent together it possible
                 * that the first fragment we receive is the fragment containing the E-bit which sounds weird
                 *
                 * When the first fragment is received (regardless of its type), the timer is started and if the
                 * fragment is special (S or E bit set), the sequence number is saved so we know the range of complete
                 * full HEVC frame if/when all fragments have been received */

                if (type == FT_START) {
                    s_timers[TS(frame)].sframe_seq = SEQ(frame);
                    s_timers[TS(frame)].eframe_seq = INVALID_SEQ;
                } else if (type == FT_END) {
                    s_timers[TS(frame)].eframe_seq = SEQ(frame);
                    s_timers[TS(frame)].sframe_seq = INVALID_SEQ;
                } else {
                    s_timers[TS(frame)].sframe_seq = INVALID_SEQ;
                    s_timers[TS(frame)].eframe_seq = INVALID_SEQ;
                }

                s_timers[TS(frame)].sframe_time   = kvz_rtp::clock::hrc::now();
                s_timers[TS(frame)].total_size    = frame->payload_len - HEVC_HDR_SIZE;
                s_timers[TS(frame)].pkts_received = 1;
                continue;
            }

            uint64_t diff = kvz_rtp::clock::hrc::diff_now(s_timers[TS(frame)].sframe_time);

            if (diff > RTP_FRAME_MAX_DELAY) {
                LOG_ERROR("frame must be dropped, max delay reached: %lu!", diff);

                if (dropped_frames.find(TS(frame)) == dropped_frames.end()) {
                    dropped_frames[TS(frame)] = 1;
                } else {
                    dropped_frames[TS(frame)]++;
                }

                frames[SEQ(frame)] = nullptr;
                (void)kvz_rtp::frame::dealloc_frame(frame);
                continue;
            }

            if (!duplicate) {
                s_timers[TS(frame)].pkts_received++;
                s_timers[TS(frame)].total_size += (frame->payload_len - HEVC_HDR_SIZE);
            }

            if (type == FT_START)
                s_timers[TS(frame)].sframe_seq = SEQ(frame);

            if (type == FT_END)
                s_timers[TS(frame)].eframe_seq = SEQ(frame);

            if (s_timers[TS(frame)].sframe_seq != INVALID_SEQ &&
                s_timers[TS(frame)].eframe_seq != INVALID_SEQ)
            {
                uint32_t ts     = TS(frame);
                uint16_t s_seq  = s_timers[ts].sframe_seq;
                uint16_t e_seq  = s_timers[ts].eframe_seq;
                size_t ptr      = 0;
                size_t received = 0;

                if (s_seq > e_seq)
                    received = 0xffff - s_seq + e_seq + 2;
                else
                    received = e_seq - s_seq + 1;

                /* we've received every fragment and the frame can be reconstructed */
                if (received == s_timers[TS(frame)].pkts_received) {
                    nal_header[0] = (frames[s_seq]->payload[0] & 0x81) | ((frame->payload[2] & 0x3f) << 1);
                    nal_header[1] =  frames[s_seq]->payload[1];

                    kvz_rtp::frame::rtp_frame *out = kvz_rtp::frame::alloc_rtp_frame();

                    out->payload_len = s_timers[TS(frame)].total_size + kvz_rtp::frame::HEADER_SIZE_HEVC_NAL;
                    out->payload     = new uint8_t[out->payload_len];

                    std::memcpy(&out->header,  &frames[s_seq]->header, kvz_rtp::frame::HEADER_SIZE_RTP);
                    std::memcpy(out->payload,  nal_header,             kvz_rtp::frame::HEADER_SIZE_HEVC_NAL);

                    ptr += kvz_rtp::frame::HEADER_SIZE_HEVC_NAL;

                    for (size_t i = s_seq; i <= e_seq; ++i) {
                        std::memcpy(
                            &out->payload[ptr],
                            &frames[i]->payload[HEVC_HDR_SIZE],
                            frames[i]->payload_len - HEVC_HDR_SIZE
                        );
                        ptr += frames[i]->payload_len - HEVC_HDR_SIZE;
                        (void)kvz_rtp::frame::dealloc_frame(frames[i]);
                        frames[i] = nullptr;
                    }

                    receiver->return_frame(out);
                    s_timers.erase(ts);
                }
            }
        } while (ret == RTP_OK);
    }

    for (int i = 0; i < 0xffff + 1; ++i)
        (void)kvz_rtp::frame::dealloc_frame(frames[i]);

    receiver->get_mutex().unlock();
    return ret;
}
