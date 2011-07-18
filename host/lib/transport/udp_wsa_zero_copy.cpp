//
// Copyright 2010-2011 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "udp_common.hpp"
#include <uhd/transport/udp_zero_copy.hpp>
#include <uhd/transport/udp_simple.hpp> //mtu
#include <uhd/transport/bounded_buffer.hpp>
#include <uhd/transport/buffer_pool.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/utils/log.hpp>
#include <boost/format.hpp>
#include <vector>

using namespace uhd;
using namespace uhd::transport;
namespace asio = boost::asio;

//A reasonable number of frames for send/recv and async/sync
static const size_t DEFAULT_NUM_FRAMES = 32;

/***********************************************************************
 * Static initialization to take care of WSA init and cleanup
 **********************************************************************/
struct uhd_wsa_control{
    uhd_wsa_control(void){
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData); /*windows socket startup */
    }

    ~uhd_wsa_control(void){
        WSACleanup();
    }
};

/***********************************************************************
 * Reusable managed receiver buffer:
 *  - Initialize with memory and a release callback.
 *  - Call get new with a length in bytes to re-use.
 **********************************************************************/
class udp_zero_copy_asio_mrb : public managed_recv_buffer{
public:
    udp_zero_copy_asio_mrb(void *mem, bounded_buffer<udp_zero_copy_asio_mrb *> &pending):
        _mem(mem), _len(0), _pending(pending){/* NOP */}

    void release(void){
        if (_len == 0) return;
        _pending.push_with_haste(this);
        _len = 0;
    }

    sptr get_new(size_t len){
        _len = len;
        return make_managed_buffer(this);
    }

    template <class T> T cast(void) const{return static_cast<T>(_mem);}

private:
    const void *get_buff(void) const{return _mem;}
    size_t get_size(void) const{return _len;}

    void *_mem;
    size_t _len;
    bounded_buffer<udp_zero_copy_asio_mrb *> &_pending;
};

/***********************************************************************
 * Reusable managed send buffer:
 *  - committing the buffer calls the asynchronous socket send
 *  - getting a new buffer performs the blocking wait for completion
 **********************************************************************/
class udp_zero_copy_asio_msb : public managed_send_buffer{
public:
    udp_zero_copy_asio_msb(void *mem, int sock_fd, const size_t frame_size):
        _sock_fd(sock_fd), _frame_size(frame_size), _committed(false)
    {
        _wsa_buff.buf = reinterpret_cast<char *>(mem);
        ZeroMemory(&_overlapped, sizeof(_overlapped));
        _overlapped.hEvent = WSACreateEvent();
        UHD_ASSERT_THROW(_overlapped.hEvent != WSA_INVALID_EVENT);
        this->commit(0); //makes buffer available via get_new
    }

    ~udp_zero_copy_asio_msb(void){
        WSACloseEvent(_overlapped.hEvent);
    }

    UHD_INLINE void commit(size_t len){
        if (_committed) return;
        _committed = true;
        _wsa_buff.len = len;
        if (len == 0) WSASetEvent(_overlapped.hEvent);
        else WSASend(_sock_fd, &_wsa_buff, 1, NULL, 0, &_overlapped, NULL);
    }

    UHD_INLINE sptr get_new(const double timeout, size_t &index){
        const DWORD result = WSAWaitForMultipleEvents(
            1, &_overlapped.hEvent, true, DWORD(timeout*1000), true
        );
        if (result == WSA_WAIT_TIMEOUT) return managed_send_buffer::sptr();
        index++; //advances the caller's buffer

        WSAResetEvent(_overlapped.hEvent);
        _committed = false;
        _wsa_buff.len = _frame_size;
        return make_managed_buffer(this);
    }

private:
    void *get_buff(void) const{return _wsa_buff.buf;}
    size_t get_size(void) const{return _wsa_buff.len;}

    int _sock_fd;
    const size_t _frame_size;
    bool _committed;
    WSAOVERLAPPED _overlapped;
    WSABUF _wsa_buff;
};

/***********************************************************************
 * Zero Copy UDP implementation with WSA:
 *
 *   This is not a true zero copy implementation as each
 *   send and recv requires a copy operation to/from userspace.
 *
 *   For receive, use a blocking recv() call on the socket.
 *   This has better performance than the overlapped IO.
 *   For send, use overlapped IO to submit async sends.
 **********************************************************************/
class udp_zero_copy_wsa_impl : public udp_zero_copy{
public:
    typedef boost::shared_ptr<udp_zero_copy_wsa_impl> sptr;

    udp_zero_copy_wsa_impl(
        const std::string &addr,
        const std::string &port,
        const device_addr_t &hints
    ):
        _recv_frame_size(size_t(hints.cast<double>("recv_frame_size", udp_simple::mtu))),
        _num_recv_frames(size_t(hints.cast<double>("num_recv_frames", DEFAULT_NUM_FRAMES))),
        _send_frame_size(size_t(hints.cast<double>("send_frame_size", udp_simple::mtu))),
        _num_send_frames(size_t(hints.cast<double>("num_send_frames", DEFAULT_NUM_FRAMES))),
        _recv_buffer_pool(buffer_pool::make(_num_recv_frames, _recv_frame_size)),
        _send_buffer_pool(buffer_pool::make(_num_send_frames, _send_frame_size)),
        _pending_recv_buffs(_num_recv_frames),
        _next_send_buff_index(0)
    {
        UHD_MSG(status) << boost::format("Creating WSA UDP transport for %s:%s") % addr % port << std::endl;
        static uhd_wsa_control uhd_wsa; //makes wsa start happen via lazy initialization

        UHD_ASSERT_THROW(_num_send_frames <= WSA_MAXIMUM_WAIT_EVENTS);

        //resolve the address
        asio::io_service io_service;
        asio::ip::udp::resolver resolver(io_service);
        asio::ip::udp::resolver::query query(asio::ip::udp::v4(), addr, port);
        asio::ip::udp::endpoint receiver_endpoint = *resolver.resolve(query);

        //create the socket
        _sock_fd = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (_sock_fd == INVALID_SOCKET){
            const DWORD error = WSAGetLastError();
            throw uhd::os_error(str(boost::format("WSASocket() failed with error %d") % error));
        }

        //set the socket non-blocking for recv
        u_long mode = 1;
        ioctlsocket(_sock_fd, FIONBIO, &mode);

        //resize the socket buffers
        const int recv_buff_size = int(hints.cast<double>("recv_buff_size", 0.0));
        const int send_buff_size = int(hints.cast<double>("send_buff_size", 0.0));
        if (recv_buff_size > 0) setsockopt(_sock_fd, SOL_SOCKET, SO_RCVBUF, (const char *)&recv_buff_size, sizeof(recv_buff_size));
        if (send_buff_size > 0) setsockopt(_sock_fd, SOL_SOCKET, SO_SNDBUF, (const char *)&send_buff_size, sizeof(send_buff_size));

        //connect the socket so we can send/recv
        const asio::ip::udp::endpoint::data_type &servaddr = *receiver_endpoint.data();
        if (WSAConnect(_sock_fd, (const struct sockaddr *)&servaddr, sizeof(servaddr), NULL, NULL, NULL, NULL) != 0){
            const DWORD error = WSAGetLastError();
            closesocket(_sock_fd);
            throw uhd::os_error(str(boost::format("WSAConnect() failed with error %d") % error));
        }

        //allocate re-usable managed receive buffers
        for (size_t i = 0; i < get_num_recv_frames(); i++){
            _mrb_pool.push_back(boost::shared_ptr<udp_zero_copy_asio_mrb>(
                new udp_zero_copy_asio_mrb(_recv_buffer_pool->at(i), _pending_recv_buffs)
            ));
            _pending_recv_buffs.push_with_haste(_mrb_pool.back().get());
        }

        //allocate re-usable managed send buffers
        for (size_t i = 0; i < get_num_send_frames(); i++){
            _msb_pool.push_back(boost::shared_ptr<udp_zero_copy_asio_msb>(
                new udp_zero_copy_asio_msb(_send_buffer_pool->at(i), _sock_fd, get_send_frame_size())
            ));
        }
    }

    ~udp_zero_copy_wsa_impl(void){
        closesocket(_sock_fd);
    }

    /*******************************************************************
     * Receive implementation:
     *
     * Perform a non-blocking receive for performance,
     * and then fall back to a blocking receive with timeout.
     * Return the managed receive buffer with the new length.
     * When the caller is finished with the managed buffer,
     * the managed receive buffer is released back into the queue.
     ******************************************************************/
    managed_recv_buffer::sptr get_recv_buff(double timeout){
        udp_zero_copy_asio_mrb *mrb = NULL;
        if (_pending_recv_buffs.pop_with_timed_wait(mrb, timeout)){

            ssize_t ret = ::recv(_sock_fd, mrb->cast<char *>(), _recv_frame_size, 0);
            if (ret > 0) return mrb->get_new(ret);

            if (wait_for_recv_ready(_sock_fd, timeout)) return mrb->get_new(
                ::recv(_sock_fd, mrb->cast<char *>(), _recv_frame_size, 0)
            );

            _pending_recv_buffs.push_with_haste(mrb); //timeout: return the managed buffer to the queue
        }
        return managed_recv_buffer::sptr();
    }

    size_t get_num_recv_frames(void) const {return _num_recv_frames;}
    size_t get_recv_frame_size(void) const {return _recv_frame_size;}

    /*******************************************************************
     * Send implementation:
     * Block on the managed buffer's get call and advance the index.
     ******************************************************************/
    managed_send_buffer::sptr get_send_buff(double timeout){
        if (_next_send_buff_index == _num_send_frames) _next_send_buff_index = 0;
        return _msb_pool[_next_send_buff_index]->get_new(timeout, _next_send_buff_index);
    }

    size_t get_num_send_frames(void) const {return _num_send_frames;}
    size_t get_send_frame_size(void) const {return _send_frame_size;}

private:
    //memory management -> buffers and fifos
    const size_t _recv_frame_size, _num_recv_frames;
    const size_t _send_frame_size, _num_send_frames;
    buffer_pool::sptr _recv_buffer_pool, _send_buffer_pool;
    std::vector<boost::shared_ptr<udp_zero_copy_asio_msb> > _msb_pool;
    std::vector<boost::shared_ptr<udp_zero_copy_asio_mrb> > _mrb_pool;
    bounded_buffer<udp_zero_copy_asio_mrb *> _pending_recv_buffs;
    size_t _next_send_buff_index;

    //socket guts
    SOCKET                  _sock_fd;
};

/***********************************************************************
 * UDP zero copy make function
 **********************************************************************/
udp_zero_copy::sptr udp_zero_copy::make(
    const std::string &addr,
    const std::string &port,
    const device_addr_t &hints
){
    return sptr(new udp_zero_copy_wsa_impl(addr, port, hints));
}
