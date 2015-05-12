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
//         Ed Swartz (ed.swartz.258@gmail.com)
// date: Spring 2004, Spring 2015
//-----------------------------------------------------------------------------
#include "ulib_net.h"
#include "chuck_vm.h"
#include "chuck_lang.h"
#include "chuck_instr.h"
#include "util_network.h"
#include <string.h>
#include <errno.h>
#include <iostream>
#include <map>
#include <list>


#if defined(__PLATFORM_WIN32__)
#include <winsock.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif


using namespace std;

// for member data offset
static t_CKUINT netout_offset_out = 0;

struct BacklogEntry {
  const t_CKBYTE* buffer;
  t_CKUINT len;

  BacklogEntry(const t_CKBYTE* buffer, t_CKUINT len) : buffer(buffer), len(len) { }
};


//-----------------------------------------------------------------------------
// name: class GigaServerSocket
// desc: a single active server and count of clients
//-----------------------------------------------------------------------------
struct GigaServerSocket {

  GigaServerSocket(ck_socket sock) : sock(sock), clients(), mutex(), running(true), thread(), backlog() {
    ck_set_nonblocking(sock);
    ck_get_port(sock, &port);
    thread.start(server_thread, this);
  }

  ~GigaServerSocket() {
    lock();
    running = false;

    // FIXME: not deleting, but can't do it here without double-delete
    sock = 0;

    for (std::list<ck_socket>::iterator it = clients.begin(); it != clients.end(); ) {
      ck_socket sock = *it;
      ck_close(sock);
    }
    clients.clear();

    for (std::list<BacklogEntry*>::iterator it = backlog.begin(); it != backlog.end(); ) {
      delete *it;
    }
    backlog.clear();

    unlock();
  }

  void send(const t_CKBYTE* buffer, t_CKUINT len) {
    if (len == 0)
      return;

    lock();

    // until at least one client is connected, remember all the output in a backlog
    if (clients.empty()) {
      t_CKBYTE* blbuffer = new t_CKBYTE[len];
      memcpy(blbuffer, buffer, len);

      BacklogEntry* entry = new BacklogEntry(blbuffer, len);
      backlog.push_back(entry);
    }
    else {
      BacklogEntry entry(buffer, len);
      broadcast(&entry);
    }

    unlock();
  }

  ck_socket sock;
  int port;

private:
  GigaServerSocket(const GigaServerSocket& other);
  GigaServerSocket& operator=(const GigaServerSocket& other);

  //-----------------------------------------------------------------------------
  // name: server_thread(...)
  // desc: manage incoming connections
  //-----------------------------------------------------------------------------
  static THREAD_RETURN THREAD_TYPE server_thread(void* arg) {
    GigaServerSocket *server = (GigaServerSocket*) arg;

    while (true) {
      server->lock();
      if (!server->running) {
        server->unlock();
        break;
      }

      server->server_thread_iteration();

      server->unlock();

      usleep(50000);
    }

    return 0;
  }

private:
  void lock() {
    mutex.acquire();
  }
  void unlock() {
    mutex.release();
  }

  void server_thread_iteration() {
    int nclients = clients.size();
    ck_socket clients_array[nclients + 1];
    std::copy(clients.begin(), clients.end(), clients_array);
    clients_array[nclients] = sock;

    if (ck_select(clients_array, nclients + 1, READ, 0, 0)) {
      if (clients_array[nclients]) {
        try_accept();
      } else {
        for (int i = 0; i < nclients; i++) {
          ck_socket client = clients_array[i];
          if (client) {
            check_client(client);
          }
        }
      }
    }
  }

  void try_accept() {
    ck_socket client = ck_accept(sock);
    if (client) {
      cerr << "[chuck](via netout): accepted client for port " << port << endl;

      clients.push_back(client);

      // send the backlog to that first lucky client
      for (std::list<BacklogEntry*>::iterator it = backlog.begin(); it != backlog.end(); it++) {
        BacklogEntry* entry = *it;
        broadcast(entry);
        delete entry;
      }
      backlog.clear();
    }
  }

  void check_client(ck_socket client) {

    char empty[1];
    if (ck_recv(client, empty, 1) == 0) {
      // closed
      cerr << "[chuck](via netout): dropped client from port " << port << endl;

      // urgh
      for (std::list<ck_socket>::iterator it = clients.begin(); it != clients.end(); ++it) {
        if (*it == client) {
          ck_close(client);
          clients.erase(it);
          break;
        }
      }
    }

  }

  void broadcast(BacklogEntry* entry) {
    for (std::list<ck_socket>::iterator it = clients.begin(); it != clients.end(); ) {
      ck_socket sock = *it;

      ssize_t sent = ck_send(sock, (const char *) entry->buffer, entry->len);
      if (sent < 0) {
        cerr << "[chuck](via netout) error sending data for socket=" << port << ": " << strerror(errno) << endl;
        ck_close(sock);
        it = clients.erase(it);
      } else {
        it++;
      }
    }

  }

  std::list<ck_socket> clients;

  XMutex mutex;
  bool running;

  XThread thread;


  std::list<BacklogEntry*> backlog;
};

//-----------------------------------------------------------------------------
// name: class GigaServer
// desc: manage TCP clients
//-----------------------------------------------------------------------------
class GigaServer {
public:
  GigaServer() {
  }
  ~GigaServer() {
    m_servers.clear();
  }

  //-----------------------------------------------------------------------------
  // name: start(port)
  // desc: start a server socket for the given port
  //-----------------------------------------------------------------------------
  GigaServerSocket* start(int port) {
    if (port) {
      // already started?
      ServerSocketMap::iterator it = m_servers.find(port);
      if (it != m_servers.end()) {
        return it->second;
      }
    }

    // open tcp server
    ck_socket ssock = ck_tcp_create( 1 );

    if (!ssock || !ck_bind( ssock, port ) || !ck_listen( ssock, 10 )) {
      cerr << "[chuck](via netout): error: cannot bind to port '" << port << "'; "
            << strerror(errno)
              << endl;
      return NULL;
    }

    GigaServerSocket* server = new GigaServerSocket(ssock);

    // remember we're connected
    m_servers[server->port] = server;

    // let clients know what port to use
    cerr << "[netout] awaiting connections on port " << server->port << endl;

    return server;
  }

  t_CKBOOL stop(GigaServerSocket* server) {
    if (!server)
      return FALSE;

    ServerSocketMap::iterator it = m_servers.find(server->port);
    if (it == m_servers.end()) {
      return FALSE;
    }
    m_servers.erase(it);

    delete server;
    return TRUE;
  }

protected:
  typedef std::map<int, GigaServerSocket*> ServerSocketMap;
  ServerSocketMap m_servers;
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
  type_engine_register_deprecate(env, "netin", "NetIn");

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

  // add readLine
  func = make_new_mfun("void", "start", netout_start);
  if( !type_engine_import_mfun( env, func ) )
    goto error;

  // end the class import
  type_engine_import_class_end(env);

//
//    // add netin
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

  // data
  string m_hostname;
  int m_port;

protected:
  GigaServerSocket* m_socket;
  t_CKUINT m_red;
  t_CKUINT m_buffer_size;
  GigaMsg m_msg;
  t_CKUINT m_len;
  t_CKBYTE m_buffer[0x8000];
  int m_seq;
  int m_type;

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

  t_CKBOOL listen(int port);t_CKBOOL disconnect();t_CKBOOL recv(
      t_CKBYTE * buffer);t_CKBOOL expire();t_CKBOOL set_bufsize( t_CKUINT size);t_CKUINT get_bufsize();t_CKBOOL good();

  t_CKBOOL tick_in( SAMPLE * sample);t_CKBOOL tick_in( SAMPLE * l, SAMPLE * r);t_CKBOOL tick_in(
      SAMPLE * samples, t_CKUINT n);

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
  m_socket = NULL;
  m_red = 1;
  m_buffer_size = 0;
  m_len = sizeof(GigaMsg) + m_buffer_size;
  m_hostname = "127.0.0.1";
  m_port = 0;
  m_seq = 0;
  m_type = 0;
  m_ptr_w = m_writebuf;
  m_ptr_end = NULL;
}
t_CKBOOL GigaSend::good() {
  return m_socket != NULL;
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
  if (m_socket)
    return FALSE;

  // port may be zero; update after start
  m_hostname = hostname;
  m_port = port;

  m_socket = s_server->start(port);
  if (!m_socket)
    return FALSE;

  // now connected, update the port
  m_port = m_socket->port;

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
  if (!m_socket)
    return FALSE;

  s_server->stop(m_socket);

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
// name: send()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaSend::send(const t_CKBYTE * buffer) {
  if (!m_socket)
    return FALSE;

  m_msg.type = m_type;
  m_msg.len = m_buffer_size;
  m_msg.seq_num = m_seq++;

  memcpy(m_buffer, &m_msg, sizeof(GigaMsg));
  memcpy(m_buffer + sizeof(GigaMsg), buffer, m_buffer_size);

  for (int i = 0; i < m_red; i++) {
    m_socket->send(m_buffer, m_len);
  }

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

//-----------------------------------------------------------------------------
// name: GigaRecv()
// desc: ...
//-----------------------------------------------------------------------------
GigaRecv::GigaRecv() {
  m_sock = NULL;
  m_buffer_size = 0;
  m_msg.seq_num = 1;
  m_port = 8890;
  m_ptr_r = NULL;
  m_ptr_end = NULL;
}
t_CKBOOL GigaRecv::good() {
  return m_sock != NULL;
}

//-----------------------------------------------------------------------------
// name: ~GigaRecv()
// desc: ...
//-----------------------------------------------------------------------------
GigaRecv::~GigaRecv() {
  this->disconnect();
}

//-----------------------------------------------------------------------------
// name: listen()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaRecv::listen(int port) {
  if (m_sock)
    return FALSE;

  m_sock = ck_tcp_create( 1 );

  // bind
  if (!ck_bind(m_sock, port)) {
    cerr << "[chuck](via netin): error: cannot bind to port '" << port << "'"
        << endl;
    return FALSE;
  }

  m_port = port;
  m_msg.seq_num = 1;

  return TRUE;
}

//-----------------------------------------------------------------------------
// name: disconnect()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaRecv::disconnect() {
  if (!m_sock)
    return FALSE;

  ck_close(m_sock);
  m_sock = NULL;

  return TRUE;
}

//-----------------------------------------------------------------------------
// name: set_bufsize()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaRecv::set_bufsize( t_CKUINT bufsize) {
  m_buffer_size = bufsize;
  m_len = sizeof(GigaMsg) + bufsize;
  m_msg.type = 0;
  m_msg.len = m_len;

  return TRUE;
}
t_CKUINT GigaRecv::get_bufsize() {
  return m_buffer_size;
}

//-----------------------------------------------------------------------------
// name: recv()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaRecv::recv( t_CKBYTE * buffer) {
  GigaMsg * msg = (GigaMsg *) m_buffer;

  if (!m_sock)
    return FALSE;

  do {
    ck_recv(m_sock, (char *) m_buffer, 0x8000);
  } while (msg->seq_num < m_msg.seq_num);

  if (msg->seq_num > (m_msg.seq_num + 1))
    cerr << "[chuck](via netin): dropped packet, expect: " << m_msg.seq_num + 1
        << " got: " << msg->seq_num << endl;

  m_msg.seq_num = msg->seq_num;
  m_msg.len = msg->len;
  m_ptr_end = m_readbuf + msg->len;

  memcpy(buffer, m_buffer + sizeof(unsigned int) * 3,
      m_msg.len * sizeof(SAMPLE));

  return TRUE;
}

//-----------------------------------------------------------------------------
// name: expire()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaRecv::expire() {
  m_msg.seq_num++;
  return true;
}

//-----------------------------------------------------------------------------
// name: tick_in()
// desc: ...
//-----------------------------------------------------------------------------
t_CKBOOL GigaRecv::tick_in( SAMPLE * sample) {
  if (m_ptr_r >= m_ptr_end) {
    this->recv((t_CKBYTE *) m_readbuf);
    m_ptr_r = m_readbuf;
  }

  *sample = *m_ptr_r++;

  return TRUE;
}

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
  if (x->good() && x->m_hostname != str->str)
    x->disconnect();
  x->m_hostname = str->str;
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

  if (x->good() && x->m_port != port && port != 0)
    x->disconnect();

  x->m_port = port;
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

CK_DLL_MFUN( netout_start ) {
  GigaSend * x = (GigaSend *) OBJ_MEMBER_UINT(SELF, netout_offset_out);

  if (x->good()) {
    return;
  }

  // connect
  x->connect(x->m_hostname.c_str(), x->m_port);
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
