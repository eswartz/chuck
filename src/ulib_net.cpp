/*----------------------------------------------------------------------------
 ChucK Concurrent, On-the-fly Audio Programming Language
 Compiler and Virtual Machine

 Copyright (c) 2004 Ge Wang and Perry R. Cook.  All rights reserved.
 http://chuck.cs.princeton.edu/
 http://soundlab.cs.princeton.edu/

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 U.S.A.
 -----------------------------------------------------------------------------*/

//-----------------------------------------------------------------------------
// file: ulib_net.cpp
// desc: ...
//
// author: Ge Wang (gewang@cs.princeton.edu)
//         Perry R. Cook (prc@cs.princeton.edu)
// date: Spring 2004
//-----------------------------------------------------------------------------
#include "ulib_net.h"
#include "chuck_vm.h"
#include "chuck_lang.h"
#include "chuck_instr.h"
#include "util_network.h"
#include "digiio_rtaudio.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <iostream>
#include <map>
using namespace std;

// for member data offset
static t_CKUINT netout_offset_out = 0;


//-----------------------------------------------------------------------------
// name: struct GigaMsg
// desc: ...
//-----------------------------------------------------------------------------
struct GigaMsg {
  unsigned int type;
  unsigned int len;
  unsigned int seq_num;

  // constructor
  GigaMsg() {
    type = len = seq_num = 0;
  }

  // init
  void init(unsigned int _type, unsigned int _len) {
    type = _type;
    len = _len;
  }

  // destructor
  ~GigaMsg() {

    type = 0;
    len = 0;
    seq_num = 0;
  }
};
//-----------------------------------------------------------------------------
// name: class GigaServer
// desc: manage TCP clients
//-----------------------------------------------------------------------------
struct GigaClient {
  ck_socket sock;
  int port;
  int clients;

  GigaClient() : sock(NULL), port(0), clients(0) { }
  GigaClient(ck_socket sock, int port) : sock(sock), port(port), clients(1) { }
  ~GigaClient() {
    if (port && sock) {
      // FIXME: not deleting, but can't do it here without double-delete
      sock = 0;
    }
    clients = 0;
  }
};

class GigaServer {
public:
  GigaServer() {
  }
  ~GigaServer() {
    cerr << "GigaServer::~GigaServer" << endl;
    m_sockets.clear();
  }

  ck_socket ensure(int port) {
    SocketMap::iterator it = m_sockets.find(port);
    if (it != m_sockets.end()) {
      cerr << "[chuck](via netout): added client for port "<< port << endl;
      it->second.second.clients++;
      return it->second.second.sock;
    }

    // open tcp sockets
    ck_socket ssock = ck_tcp_create( 2 );

    if (!ssock || !ck_bind( ssock, port ) || !ck_listen( ssock, 10 )) {
      cerr << "[chuck](via netout): error: cannot bind to port '" << port << "'"
            << strerror(errno)
              << endl;
      return NULL;
    }

    ck_socket sock = 0;
    cerr << "[chuck](via netout) Waiting for connection on socket " << port << "..." << endl;
    if (!(sock = ck_accept( ssock ))) {
      cerr << "[chuck](via netout): error: cannot accept to port '" << port << "'"
            << strerror(errno)
              << endl;
      return FALSE;
    }
    cerr << "[chuck](via netout) Connected!" << endl;

    // remember we're connected
    std::pair<ck_socket, GigaClient> info (ssock, GigaClient(sock, port));
    m_sockets[port] = info;

    return info.second.sock;

  }
  void unbind(int port) {
    SocketMap::iterator it = m_sockets.find(port);
     if (it != m_sockets.end()) {
       cerr << "[chuck](via netout): removed client for port "<< port << endl;
       if ( 0 == --it->second.second.clients) {
         // remove client, destroying it
         m_sockets.erase(it);
       }
     }
  }

protected:
  typedef std::map<int, std::pair<ck_socket,GigaClient> > SocketMap;
  SocketMap m_sockets;
};


static GigaServer* s_server = new GigaServer();

//-----------------------------------------------------------------------------
// name: net_query()
// desc: ...
//-----------------------------------------------------------------------------
DLL_QUERY net_query(Chuck_DL_Query * QUERY) {

  //! \sectionMain network
  /*! \nameinfo
   ChucK's network ugens
   */

  Chuck_Env * env = Chuck_Env::instance();
  Chuck_DL_Func * func = NULL;

  // add netout
  //! TCP-based network audio transmitter

  type_engine_register_deprecate(env, "netout", "NetOut");

  if (!type_engine_import_ugen_begin(env, "NetOut", "UGen", env->global(),
      netout_ctor, netout_dtor, netout_tick, NULL))
    return FALSE;

  // add member variable
  netout_offset_out = type_engine_import_mvar(env, "int", "@netout_data",
      FALSE);
  if (netout_offset_out == CK_INVALID_OFFSET)
    goto error;

  // ctrl funcs
  // add ctrl: addr
  func = make_new_mfun("string", "addr", netout_ctrl_addr);
  func->add_arg("string", "addr");
  func->doc = "Address to which to send audio.";
  if (!type_engine_import_mfun(env, func))
    goto error;
  func = make_new_mfun("string", "addr", netout_cget_addr);
  func->doc = "Address to which to send audio.";
  if (!type_engine_import_mfun(env, func))
    goto error;

  // add ctrl: port
  func = make_new_mfun("int", "port", netout_ctrl_port);
  func->add_arg("int", "port");
  func->doc = "TCP port to which to send audio.";
  if (!type_engine_import_mfun(env, func))
    goto error;
  func = make_new_mfun("int", "port", netout_cget_port);
  func->doc = "TCP port to which to send audio.";
  if (!type_engine_import_mfun(env, func))
    goto error;

  // add ctrl: packet size
  func = make_new_mfun("int", "size", netout_ctrl_size);
  func->add_arg("int", "size");
  func->doc = "Packet size in bytes.";
  if (!type_engine_import_mfun(env, func))
    goto error;
  func = make_new_mfun("int", "size", netout_cget_size);
  func->doc = "Packet size in bytes.";
  if (!type_engine_import_mfun(env, func))
    goto error;

  // add ctrl: type
  func = make_new_mfun("int", "type", netout_ctrl_type);
  func->add_arg("int", "type");
  func->doc = "Type identifier of stream.";
  if (!type_engine_import_mfun(env, func))
    goto error;
  func = make_new_mfun("int", "type", netout_cget_type);
  func->doc = "Type identifier of stream.";
  if (!type_engine_import_mfun(env, func))
    goto error;

  // add ctrl: sequence number
  func = make_new_mfun("int", "seq_num", netout_ctrl_seqnum);
  func->add_arg("int", "seq_num");
  func->doc = "Sequence number of packet.";
  if (!type_engine_import_mfun(env, func))
    goto error;
  func = make_new_mfun("int", "seq_num", netout_cget_seqnum);
  func->doc = "Sequence number of packet.";
  if (!type_engine_import_mfun(env, func))
    goto error;

  // add ctrl: type
  func = make_new_mfun("int", "realtime", netout_ctrl_realtime);
  func->add_arg("int", "realtime");
  func->doc = "Tell whether the receiver should block to simulate realtime behavior.";
  if (!type_engine_import_mfun(env, func))
    goto error;
  func = make_new_mfun("int", "realtime", netout_cget_realtime);
  func->doc = "Tell whether the receiver should block to simulate realtime behavior.";
  if (!type_engine_import_mfun(env, func))
		goto error;


  // end the class import
  type_engine_import_class_end(env);

//
//    // add netin
//  type_engine_register_deprecate(env, "netin", "NetIn");
//    //! TCP-based network audio receiver
//    QUERY->begin_class( QUERY, "NetIn", "Object" );
//    // set funcs
//    QUERY->ugen_func( QUERY, netin_ctor, netin_dtor, netin_tick, NULL );
//    // ctrl funcs
//    QUERY->ugen_ctrl( QUERY, netin_ctrl_port, netin_cget_port, "int", "port" ); //! set port to receive
//    QUERY->ugen_ctrl( QUERY, netin_ctrl_name, netin_cget_name, "string", "name" ); //! name?
//
//    QUERY->end_class( QUERY );

  return TRUE;

error:

  // end the class import
  type_engine_import_class_end(env);

  return FALSE;
}


//-----------------------------------------------------------------------------
// name: class GigaSend
// desc: ...
//-----------------------------------------------------------------------------
class GigaSend {
public:
  GigaSend();
  ~GigaSend();

  t_CKBOOL connect(const char * hostname, int port);
  t_CKBOOL disconnect();
  t_CKBOOL send(const t_CKBYTE * buffer);
  t_CKBOOL set_bufsize( t_CKUINT buffer_size);
  t_CKUINT get_bufsize();
  t_CKBOOL good();

  t_CKBOOL tick_out( SAMPLE sample );
  t_CKBOOL tick_out( SAMPLE l, SAMPLE r );
  t_CKBOOL tick_out( SAMPLE * samples, t_CKUINT n );

  void set_redundancy( t_CKUINT n);
  t_CKUINT get_redundancy();

  void set_seq( int seq );
  int get_seq();
  void set_type( int type );
  int get_type();
  void set_realtime( bool realtime );
  bool is_realtime();

  // data
  string m_hostname;
  int m_port;

protected:
  ck_socket m_sock;
  t_CKUINT m_red;
  t_CKUINT m_buffer_size;
  GigaMsg m_msg;
  t_CKUINT m_len;
  t_CKBYTE m_buffer[0x8000];
  int m_seq;
  int m_type;
  bool m_realtime;
  t_CKFLOAT m_last_send_time;
  t_CKFLOAT m_logical_elapsed_clock;
  t_CKFLOAT m_real_elapsed_clock;

  SAMPLE m_writebuf[0x8000];SAMPLE * m_ptr_w;SAMPLE * m_ptr_end;
};

//-----------------------------------------------------------------------------
// name: class GigaRecv
// desc: ...
//-----------------------------------------------------------------------------
class GigaRecv {
public:
  GigaRecv();
  ~GigaRecv();

  t_CKBOOL listen(int port);
  t_CKBOOL disconnect();
  t_CKBOOL recv(t_CKBYTE * buffer);
  t_CKBOOL expire();
  t_CKBOOL set_bufsize( t_CKUINT size);
  t_CKUINT get_bufsize();
  t_CKBOOL good();

  t_CKBOOL tick_in( SAMPLE * sample);
  t_CKBOOL tick_in( SAMPLE * l, SAMPLE * r);
  t_CKBOOL tick_in( SAMPLE * samples, t_CKUINT n);

  // data
  int m_port;

protected:
  ck_socket m_sock;t_CKUINT m_buffer_size;
  GigaMsg m_msg;t_CKUINT m_len;t_CKBYTE m_buffer[0x8000];

  SAMPLE m_readbuf[0x8000];SAMPLE * m_ptr_r;SAMPLE * m_ptr_end;

};

//-----------------------------------------------------------------------------
// name: GigaSend()
// desc: ...
//-----------------------------------------------------------------------------
GigaSend::GigaSend() {
  m_sock = NULL;
  m_red = 1;
  m_buffer_size = 0;
  m_len = sizeof(GigaMsg) + m_buffer_size;
  m_hostname = "127.0.0.1";
  m_port = 0;
  m_seq = 0;
  m_type = 0;
  m_ptr_w = m_writebuf;
  m_ptr_end = NULL;
  m_realtime = false;
  m_last_send_time = 0;
  m_logical_elapsed_clock = 0;
  m_real_elapsed_clock = 0;
}

t_CKBOOL GigaSend::good() {
  return m_sock != NULL;
}

//-----------------------------------------------------------------------------
// name: ~GigaSend()
// desc: ...
//-----------------------------------------------------------------------------
GigaSend::~GigaSend() {
  this->disconnect();
}

//-----------------------------------------------------------------------------
// name: connect()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaSend::connect(const char * hostname, int port) {
  if (m_sock || port == 0)
    return FALSE;

  m_sock = s_server->ensure(port);
  if (!m_sock)
    return FALSE;

  m_hostname = hostname;
  m_port = port;

  return TRUE;
}

//-----------------------------------------------------------------------------
// name: set_bufsize()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaSend::set_bufsize( t_CKUINT bufsize) {
  m_buffer_size = bufsize * sizeof(SAMPLE);
  m_len = sizeof(GigaMsg) + m_buffer_size;
  m_ptr_w = m_writebuf;
  m_ptr_end = m_writebuf + bufsize;

  return TRUE;
}
t_CKUINT GigaSend::get_bufsize() {
  return m_buffer_size;
}

//-----------------------------------------------------------------------------
// name: disconnect()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaSend::disconnect() {
  if (!m_sock)
    return FALSE;

  // send final packet
  m_msg.type = m_type;
  m_msg.len = 0;
  m_msg.seq_num = m_seq;

  memcpy(m_buffer, &m_msg, sizeof(GigaMsg));

  for (int i = 0; i < m_red; i++) {

    ssize_t sent = ck_send(m_sock, (const char *) m_buffer, sizeof(GigaMsg));
    if (sent < 0) {
      cerr << "error sending data for socket=" << m_port << ": " << strerror(errno) << endl;
    }
  }

  cerr << "GigaSend::disconnect done!" << endl;
  s_server->unbind(m_port);

  return TRUE;
}

//-----------------------------------------------------------------------------
// name: set_redundancy()
// desc: ...
//-----------------------------------------------------------------------------
void GigaSend::set_redundancy( t_CKUINT n) {
  m_red = n;
}

//-----------------------------------------------------------------------------
// name: get_redundancy()
// desc: ...
//-----------------------------------------------------------------------------
t_CKUINT GigaSend::get_redundancy() {
  return m_red;
}

//-----------------------------------------------------------------------------
// name: set_seq()
// desc: ...
//-----------------------------------------------------------------------------
void GigaSend::set_seq( int seq ) {
  m_seq = seq;
}

//-----------------------------------------------------------------------------
// name: get_seq()
// desc: ...
//-----------------------------------------------------------------------------
int GigaSend::get_seq() {
  return m_seq;
}

//-----------------------------------------------------------------------------
// name: set_type()
// desc: ...
//-----------------------------------------------------------------------------
void GigaSend::set_type( int type ) {
  m_type = type;
}

//-----------------------------------------------------------------------------
// name: get_type()
// desc: ...
//-----------------------------------------------------------------------------
int GigaSend::get_type() {
  return m_type ;
}

//-----------------------------------------------------------------------------
// name: set_realtime()
// desc: ...
//-----------------------------------------------------------------------------
void GigaSend::set_realtime( bool realtime ) {

  m_realtime = realtime;
}

//-----------------------------------------------------------------------------
// name: is_realtime()
// desc: ...
//-----------------------------------------------------------------------------
bool GigaSend::is_realtime() {
  return m_realtime ;
}


//-----------------------------------------------------------------------------
// name: get_current_time()
// desc: ...
//-----------------------------------------------------------------------------
static t_CKFLOAT get_clock(  )
{
#ifdef __PLATFORM_WIN32__
    return GetTickCount() / 1000.f;
#else
    return float(clock()) / CLOCKS_PER_SEC;
#endif
}

//-----------------------------------------------------------------------------
// name: send()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaSend::send(const t_CKBYTE * buffer) {
  if (!m_sock)
    return FALSE;

  m_msg.type = m_type;
  m_msg.len = m_buffer_size;
  m_msg.seq_num = m_seq++;

  memcpy(m_buffer, &m_msg, sizeof(GigaMsg));
  memcpy(m_buffer + sizeof(GigaMsg), buffer, m_buffer_size);

  t_CKFLOAT sendTime = get_clock();

  for (int i = 0; i < m_red; i++) {

    ssize_t sent = ck_send(m_sock, (const char *) m_buffer, m_len);
    if (sent < 0) {
      cerr << "error sending data for socket=" << m_port << ": " << strerror(errno) << endl;
    }
  }


  if (m_realtime) {
    // send up to one second of data before pausing

    t_CKFLOAT now = get_clock();

    float real_elapsed = now - m_last_send_time;
    m_real_elapsed_clock += real_elapsed;

    // how much time is represented here?
    float logical_elapsed = float(m_buffer_size) * 8 / Digitalio::m_sampling_rate / Digitalio::m_bps / Digitalio::m_num_channels_out;

    m_logical_elapsed_clock += logical_elapsed;

    //cerr << "realtime: real=" << m_real_elapsed_clock << "; log=" << m_logical_elapsed_clock << endl;

    float sleep = m_logical_elapsed_clock - m_real_elapsed_clock;

    // latency
    sleep -= logical_elapsed;

    if (sleep > 1) {
      // we can rest
      cerr << "sleeping for " << sleep - 1 << endl;
      usleep( (sleep - 1) * 1000000 );

      m_real_elapsed_clock += sleep - 1;

    } else if (sleep < -1)  {
      // oops, we're much too slow
      cerr << "too much sleep!" << endl;
      m_real_elapsed_clock = 0;
      m_logical_elapsed_clock = 0;
    }

  }

  m_last_send_time = sendTime;

  //cerr << "sent data for socket=" << m_port << ", type=" << m_type << ", seq=" << m_seq << ", size=" << m_len << endl;
  return TRUE;
}

//-----------------------------------------------------------------------------
// name: tick_out()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaSend::tick_out( SAMPLE sample) {
  // send
  if (m_ptr_w >= m_ptr_end) {
    this->send((t_CKBYTE *) m_writebuf);
    m_ptr_w = m_writebuf;
  }

  *m_ptr_w++ = sample;

  return TRUE;
}

////-----------------------------------------------------------------------------
//// name: GigaRecv()
//// desc: ...
////-----------------------------------------------------------------------------
//GigaRecv::GigaRecv() {
//  m_sock = NULL;
//  m_buffer_size = 0;
//  m_msg.seq_num = 1;
//  m_port = 8890;
//  m_ptr_r = NULL;
//  m_ptr_end = NULL;
//}
//t_CKBOOL GigaRecv::good() {
//  return m_sock != NULL;
//}
//
////-----------------------------------------------------------------------------
//// name: ~GigaRecv()
//// desc: ...
////-----------------------------------------------------------------------------
//GigaRecv::~GigaRecv() {
//  this->disconnect();
//}
//
////-----------------------------------------------------------------------------
//// name: listen()
//// desc: ...
////-----------------------------------------------------------------------------
//t_CKBOOL GigaRecv::listen(int port) {
//  if (m_sock)
//    return FALSE;
//
//  m_sock = ck_tcp_create( 2 );
//
//  // bind
//  if (!ck_bind(m_sock, port)) {
//    cerr << "[chuck](via netin): error: cannot bind to port '" << port << "'"
//        << endl;
//    return FALSE;
//  }
//
//  m_port = port;
//  m_msg.seq_num = 1;
//
//  return TRUE;
//}
//
////-----------------------------------------------------------------------------
//// name: disconnect()
//// desc: ...
////-----------------------------------------------------------------------------
//t_CKBOOL GigaRecv::disconnect() {
//  if (!m_sock)
//    return FALSE;
//
//  ck_close(m_sock);
//  m_sock = NULL;
//
//  return TRUE;
//}
//
////-----------------------------------------------------------------------------
//// name: set_bufsize()
//// desc: ...
////-----------------------------------------------------------------------------
//t_CKBOOL GigaRecv::set_bufsize( t_CKUINT bufsize) {
//  m_buffer_size = bufsize;
//  m_len = sizeof(GigaMsg) + bufsize;
//  m_msg.type = 0;
//  m_msg.len = m_len;
//
//  return TRUE;
//}
//t_CKUINT GigaRecv::get_bufsize() {
//  return m_buffer_size;
//}
//
////-----------------------------------------------------------------------------
//// name: recv()
//// desc: ...
////-----------------------------------------------------------------------------
//t_CKBOOL GigaRecv::recv( t_CKBYTE * buffer) {
//  GigaMsg * msg = (GigaMsg *) m_buffer;
//
//  if (!m_sock)
//    return FALSE;
//
//  do {
//    ck_recv(m_sock, (char *) m_buffer, 0x8000);
//  } while (msg->seq_num < m_msg.seq_num);
//
//  if (msg->seq_num > (m_msg.seq_num + 1))
//    cerr << "[chuck](via netin): dropped packet, expect: " << m_msg.seq_num + 1
//        << " got: " << msg->seq_num << endl;
//
//  m_msg.seq_num = msg->seq_num;
//  m_msg.len = msg->len;
//  m_ptr_end = m_readbuf + msg->len;
//
//  memcpy(buffer, m_buffer + sizeof(unsigned int) * 3,
//      m_msg.len * sizeof(SAMPLE));
//
//  return TRUE;
//}
//
////-----------------------------------------------------------------------------
//// name: expire()
//// desc: ...
////-----------------------------------------------------------------------------
//t_CKBOOL GigaRecv::expire() {
//  m_msg.seq_num++;
//  return true;
//}
//
////-----------------------------------------------------------------------------
//// name: tick_in()
//// desc: ...
////-----------------------------------------------------------------------------
//t_CKBOOL GigaRecv::tick_in( SAMPLE * sample) {
//  if (m_ptr_r >= m_ptr_end) {
//    this->recv((t_CKBYTE *) m_readbuf);
//    m_ptr_r = m_readbuf;
//  }
//
//  *sample = *m_ptr_r++;
//
//  return TRUE;
//}

//-----------------------------------------------------------------------------
// name: netout
// desc: ...
//-----------------------------------------------------------------------------
CK_DLL_CTOR ( netout_ctor ) {
  GigaSend * out = new GigaSend;
  out->set_bufsize(512);
  // return data to be used later
  OBJ_MEMBER_UINT(SELF, netout_offset_out) = (t_CKUINT) out;
}

CK_DLL_DTOR( netout_dtor ) {
  // get the data
  GigaSend * data = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  delete data;
}

CK_DLL_TICK( netout_tick ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  *out = 0.0f;
  return x->tick_out(in);
}

CK_DLL_CTRL( netout_ctrl_addr ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  Chuck_String * str = GET_CK_STRING(ARGS);

  // check if the same and already good
  if (x->good() && str->str == x->m_hostname )
    return;

  // connect
  x->disconnect();
  x->connect(str->str.c_str(), x->m_port);
}

CK_DLL_CGET( netout_cget_addr ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);

  Chuck_String * str = (Chuck_String *) instantiate_and_initialize_object(
      &t_string, NULL);
  str->str = x->m_hostname;
  RETURN->v_string = str;
}

CK_DLL_CTRL( netout_ctrl_port ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  int port = GET_CK_INT(ARGS);

  // check if the same and already connected
  if (x->good() && port == x->m_port)
    return;

  // connect
  x->disconnect();
  x->connect(x->m_hostname.c_str(), port);
}

CK_DLL_CGET( netout_cget_port ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  RETURN->v_int = x->m_port;
}

CK_DLL_CTRL( netout_ctrl_size ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  int size = GET_CK_INT(ARGS);

  // sanity check
  if (size < 1 || size > 0x8000) {
    cerr << "[chuck](via netout): invalid buffer size '" << size
        << "' (must be between 1 and 0x8000)" << endl;
    return;
  }

  x->set_bufsize((t_CKUINT) size);
}

CK_DLL_CGET( netout_cget_size ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  RETURN->v_int = x->get_bufsize();
}


CK_DLL_CTRL( netout_ctrl_seqnum ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  int seq = GET_CK_INT(ARGS);
  x->set_seq(seq);
}

CK_DLL_CGET( netout_cget_seqnum ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  RETURN->v_int = x->get_seq();
}


CK_DLL_CTRL( netout_ctrl_type ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  int type = GET_CK_INT(ARGS);
  x->set_type(type);
}

CK_DLL_CGET( netout_cget_type ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  RETURN->v_int = x->get_type();
}



CK_DLL_CTRL( netout_ctrl_realtime ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  int realtime = GET_CK_INT(ARGS);
  x->set_realtime(realtime != 0);
}

CK_DLL_CGET( netout_cget_realtime ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);
  RETURN->v_int = x->is_realtime();
}

//
////-----------------------------------------------------------------------------
//// name: netin
//// desc: ...
////-----------------------------------------------------------------------------
//UGEN_CTOR netin_ctor( t_CKTIME now )
//{
//    GigaRecv * x = new GigaRecv;
//    x->listen( x->m_port );
//
//    return x;
//}
//
//UGEN_DTOR netin_dtor( t_CKTIME now, void * data )
//{
//    GigaRecv * x = (GigaRecv *)data;
//    delete x;
//}
//
//UGEN_TICK netin_tick( t_CKTIME now, void * data, SAMPLE in, SAMPLE * out )
//{
//    GigaRecv * x = (GigaRecv *)data;
//    return x->tick_in( out );
//}
//
//UGEN_CTRL netin_ctrl_port( t_CKTIME now, void * data, void * value )
//{
//    GigaRecv * x = (GigaRecv *)data;
//    int port = GET_NEXT_INT(value);
//
//    // check if same and already connected
//    if( x->good() && x->m_port == port )
//        return;
//
//    // connect
//    x->disconnect( );
//    x->listen( port );
//}
//
//UGEN_CGET netin_cget_port( t_CKTIME now, void * data, void * out )
//{
//    GigaRecv * x = (GigaRecv *)data;
//    SET_NEXT_INT( out, x->m_port );
//}
//
