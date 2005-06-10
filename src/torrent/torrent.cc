// libTorrent - BitTorrent library
// Copyright (C) 2005, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <iostream>
#include <sstream>
#include <memory>
#include <unistd.h>
#include <sigc++/bind.h>
#include <rak/functional.h>

#include "exceptions.h"
#include "torrent.h"
#include "bencode.h"

#include "utils/sha1.h"
#include "utils/string_manip.h"
#include "utils/task_schedule.h"
#include "utils/throttle.h"
#include "net/listen.h"
#include "net/handshake_manager.h"
#include "net/poll_manager.h"
#include "net/poll_select.h"
#include "parse/parse.h"
#include "data/file_manager.h"
#include "data/hash_queue.h"
#include "data/hash_torrent.h"
#include "download/download_manager.h"
#include "download/download_wrapper.h"

namespace torrent {

int64_t Timer::m_cache;

ThrottlePeer throttleRead;
ThrottlePeer throttleWrite;

// New API.
class Torrent {
public:
  Torrent() : m_fileManager(100) {}

  std::string         m_ip;
  std::string         m_bind;

  Listen              m_listen;
  HashQueue           m_hashQueue;
  HandshakeManager    m_handshakeManager;
  DownloadManager     m_downloadManager;

  FileManager         m_fileManager;
};

Torrent* torrent = NULL;

// Find some better way of doing this, or rather... move it outside.
std::string
download_id(const std::string& hash) {
  DownloadWrapper* d = torrent->m_downloadManager.find(hash);

  return d &&
    d->get_main().is_active() &&
    d->get_main().is_checked() ?
    d->get_main().get_me().get_id() : "";
}

void
receive_connection(SocketFd fd, const std::string& hash, const PeerInfo& peer) {
  DownloadWrapper* d = torrent->m_downloadManager.find(hash);
  
  if (!d ||
      !d->get_main().is_active() ||
      !d->get_main().is_checked() ||
      !d->get_main().get_net().add_connection(fd, peer))
    fd.close();
}

std::string
bencode_hash(Bencode& b) {
  std::stringstream str;
  str << b;

  if (str.fail())
    throw bencode_error("Could not write bencode to stream");

  std::string s = str.str();
  Sha1 sha1;

  sha1.init();
  sha1.update(s.c_str(), s.size());

  return sha1.final();
}  

void
initialize() {
  if (torrent != NULL)
    throw client_error("torrent::initialize() called but the library has already been initialized");

  Timer::update();

  torrent = new Torrent;
  torrent->m_listen.slot_incoming(sigc::mem_fun(torrent->m_handshakeManager, &HandshakeManager::add_incoming));

  throttleRead.start();
  throttleWrite.start();

  torrent->m_handshakeManager.slot_connected(sigc::ptr_fun3(&receive_connection));
  torrent->m_handshakeManager.slot_download_id(sigc::ptr_fun1(download_id));

  PollManager::set_open_max(sysconf(_SC_OPEN_MAX));
}

// Clean up and close stuff. Stopping all torrents and waiting for
// them to finish is not required, but recommended.
void
cleanup() {
  if (torrent == NULL)
    throw client_error("torrent::cleanup() called but the library is not initialized");

  throttleRead.stop();
  throttleWrite.stop();

  torrent->m_handshakeManager.clear();
  torrent->m_downloadManager.clear();

  delete torrent;
  torrent = NULL;
}

bool
listen_open(uint16_t begin, uint16_t end) {
  if (torrent == NULL)
    throw client_error("listen_open called but the library has not been initialized");

  SocketAddress sa;

  if (!torrent->m_bind.empty() && !sa.set_address(torrent->m_bind))
    throw local_error("Could not parse the ip address to bind");

  if (!torrent->m_listen.open(begin, end, sa))
    return false;

  torrent->m_handshakeManager.set_bind_address(sa);

  for (DownloadManager::const_iterator itr = torrent->m_downloadManager.begin(), last = torrent->m_downloadManager.end();
       itr != last; ++itr)
    (*itr)->get_main().set_port(torrent->m_listen.get_port());

  return true;
}

void
listen_close() {
  torrent->m_listen.close();
}

// Set the file descriptors we want to pool for R/W/E events. All
// fd_set's must be valid pointers. Returns the highest fd.
void
mark(fd_set* readSet, fd_set* writeSet, fd_set* exceptSet, int* maxFd) {
  if (readSet == NULL || writeSet == NULL || exceptSet == NULL || maxFd == NULL)
    throw client_error("torrent::mark(...) received a NULL pointer");

  *maxFd = 0;

  PollManager::read_set().prepare();
  std::for_each(PollManager::read_set().begin(), PollManager::read_set().end(), poll_mark(readSet, maxFd));

  PollManager::write_set().prepare();
  std::for_each(PollManager::write_set().begin(), PollManager::write_set().end(), poll_mark(writeSet, maxFd));
  
  PollManager::except_set().prepare();
  std::for_each(PollManager::except_set().begin(), PollManager::except_set().end(), poll_mark(exceptSet, maxFd));
}

// Do work on the polled file descriptors.
void
work(fd_set* readSet, fd_set* writeSet, fd_set* exceptSet, int maxFd) {
  // Update the cached time.
  Timer::update();

  if (readSet == NULL || writeSet == NULL || exceptSet == NULL)
    throw client_error("Torrent::work(...) received a NULL pointer");

  // Make sure we don't do read/write on fd's that are in except. This should
  // not be a problem as any except call should remove it from the m_*Set's.

  PollManager::except_set().prepare();
  std::for_each(PollManager::except_set().begin(), PollManager::except_set().end(),
		poll_check(exceptSet, std::mem_fun(&SocketBase::except)));

  PollManager::read_set().prepare();
  std::for_each(PollManager::read_set().begin(), PollManager::read_set().end(),
		poll_check(readSet, std::mem_fun(&SocketBase::read)));

  PollManager::write_set().prepare();
  std::for_each(PollManager::write_set().begin(), PollManager::write_set().end(),
		poll_check(writeSet, std::mem_fun(&SocketBase::write)));

  // TODO: Consider moving before the r/w/e. libsic++ should remove the use of
  // zero timeout stuff to send signal. Better yet, use on both sides, it's cheap.
  TaskSchedule::perform(Timer::current());
}

bool
is_inactive() {
  return torrent == NULL ||
    std::find_if(torrent->m_downloadManager.begin(), torrent->m_downloadManager.end(),
		      std::not1(std::mem_fun(&DownloadWrapper::is_stopped)))
    == torrent->m_downloadManager.end();
}

const std::string&
get_ip() {
  return torrent->m_ip;
}

void
set_ip(const std::string& addr) {
  if (addr == torrent->m_ip)
    return;

  torrent->m_ip = addr;

  for (DownloadManager::const_iterator itr = torrent->m_downloadManager.begin(), last = torrent->m_downloadManager.begin();
       itr != last; ++itr)
    (*itr)->get_main().get_me().set_dns(torrent->m_ip);
}

const std::string&
get_bind() {
  return torrent->m_bind;
}

void
set_bind(const std::string& addr) {
  if (addr == torrent->m_bind)
    return;

  if (torrent->m_listen.is_open())
    throw client_error("torrent::set_bind(...) called, but listening socket is open");

  torrent->m_bind = addr;
}

uint16_t
get_listen_port() {
  return torrent->m_listen.get_port();
}

unsigned int
get_total_handshakes() {
  return torrent->m_handshakeManager.get_size();
}

int64_t
get_current_time() {
  return Timer::current().usec();
}

int64_t
get_next_timeout() {
  return TaskSchedule::get_timeout().usec();
}

unsigned int
get_read_throttle() {
  return std::max(throttleRead.get_quota(), 0);
}

void
set_read_throttle(unsigned int bytes) {
  throttleRead.set_quota(bytes > 0 ? bytes : ThrottlePeer::UNLIMITED);
}

unsigned int
get_write_throttle() {
  return std::max(throttleWrite.get_quota(), 0);
}

void
set_write_throttle(unsigned int bytes) {
  throttleWrite.set_quota(bytes > 0 ? bytes : ThrottlePeer::UNLIMITED);
}

std::string
get_version() {
  return VERSION;
}

unsigned int
get_hash_read_ahead() {
  return Settings::hashWillneed;
}

void
set_hash_read_ahead(unsigned int bytes) {
  if (bytes < 64 << 20)
    Settings::hashWillneed = bytes;
}

unsigned int
get_max_open_files() {
  return torrent->m_fileManager.max_size();
}

void
set_max_open_files(unsigned int size) {
  torrent->m_fileManager.set_max_size(size);
}

Download
download_create(std::istream* s) {
  if (s == NULL)
    throw client_error("torrent::download_create(...) received a NULL pointer");

  if (!s->good())
    throw input_error("Could not create download, the input stream is not valid");

  std::auto_ptr<DownloadWrapper> d(new DownloadWrapper);

  *s >> d->get_bencode();

  if (s->fail())
    // Make it configurable whetever we throw or return .end()?
    throw input_error("Could not create download, failed to parse the bencoded data");
  
  d->get_main().get_me().set_dns(torrent->m_ip);
  d->get_main().get_me().set_port(torrent->m_listen.get_port());

  parse_main(d->get_bencode(), d->get_main());
  parse_info(d->get_bencode()["info"], d->get_main().get_state().get_content());

  d->initialize(bencode_hash(d->get_bencode()["info"]),
		Settings::peerName + random_string(20 - Settings::peerName.size()));

  d->set_handshake_manager(&torrent->m_handshakeManager);
  d->set_hash_queue(&torrent->m_hashQueue);
  d->set_file_manager(&torrent->m_fileManager);

  parse_tracker(d->get_bencode(), &d->get_main().get_tracker());

  torrent->m_downloadManager.add(d.get());

  return Download(d.release());
}

// Add all downloads to dlist. Make sure it's cleared.
void
download_list(DList& dlist) {
  for (DownloadManager::const_iterator itr = torrent->m_downloadManager.begin();
       itr != torrent->m_downloadManager.end(); ++itr)
    dlist.push_back(Download(*itr));
}

// Make sure you check that it's valid.
Download
download_find(const std::string& id) {
  return torrent->m_downloadManager.find(id);
}

void
download_remove(const std::string& id) {
  torrent->m_downloadManager.remove(id);
}

Bencode&
download_bencode(const std::string& id) {
  DownloadWrapper* d = torrent->m_downloadManager.find(id);

  if (d == NULL)
    throw client_error("Tried to call download_bencode(id) with non-existing download");

  return d->get_bencode();
}

}
